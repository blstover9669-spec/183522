/*
 * browser_js.c - JavaScript on web pages: NovaJS (apps/js) in the page pipeline of
 * browser_net.c, so browser tabs and other apps that show pages through it (the HTML
 * Viewer) run the same scripts.
 *
 *   - one script context per page that has scripts (<script>, on...="" attributes,
 *     javascript: links); pages without scripts get none and work as before;
 *   - the DOM: nodes of the page's web_doc_t are wrapped as JS objects on demand
 *     (web_node_t.jsv); nodes scripts make are malloc'd and freed once neither the
 *     document nor a script can reach them; the JS half of the platform (interfaces,
 *     events, fetch, XMLHttpRequest ...) is browser_jsrt.js, which calls the natives
 *     below (N.*);
 *   - scripts run in time slices from the browser's timer tick, events and timer
 *     callbacks are tasks queued behind them: nothing ever waits, a script that runs
 *     too long is stopped ("page not responding") and the window stays usable;
 *     alert / confirm / prompt are NovaOS dialogs the script waits for (js_wait);
 *   - changes to the document are collected and the page is styled and laid out
 *     again once per batch (or at once when a script asks for sizes);
 *   - timers, requestAnimationFrame, fetch / XMLHttpRequest (http_fetch, with the
 *     cookie jar), localStorage (per site, saved in /home/user/.browser/storage),
 *     sessionStorage, document.cookie, location / history (same-page entries),
 *     console (serial log).
 *
 * Page input for the app (tab_page_input) lives here too: form controls (focus,
 * typing, check boxes, lists, buttons) with or without scripts.
 */
#include "browser.h"
#include "js/js.h"

extern char kernel_stack_top[];          /* boot/boot.asm: the GUI (and so scripts) run on it */
extern const char browser_jsrt[];        /* browser_jsrt.js (made by the Makefile)            */
extern const unsigned browser_jsrt_len;
/* the kernel's cookie jar (kernel/net/cookies.c), for document.cookie */
char *cookies_header_for(const url_t *u);
void  cookies_store(const url_t *u, const char *set_cookie);
void  random_bytes(void *out, size_t n);

#define TICK_MS     30         /* scripts per timer tick, at most            */
#define SLOW_MS     10000      /* a task running this long is stopped        */
#define STORE_MAX   (5u << 20) /* localStorage per site                      */
#define STORE_DIR   "/home/user/.browser/storage"

/* ==== state ================================================================================ */
typedef struct jtimer { int id; uint32_t due, every; bool raf, repeat; int fn, args; } jtimer_t;

enum { JT_CALL = 1, JT_SCRIPT };
typedef struct jscript jscript_t;
typedef struct jtask {
    struct jtask *next;
    int kind;
    int fn, self, args;          /* roots (JT_CALL)            */
    jscript_t *sc;               /* JT_SCRIPT                  */
    bool user;                   /* the user's input: popups are allowed */
} jtask_t;

enum { SC_WAIT = 0, SC_LOADING, SC_READY, SC_RUNNING, SC_DONE };
struct jscript {
    int el;                      /* root: the <script> element  */
    char *url;                   /* external: absolute URL      */
    char *src;                   /* UTF-8 source                */
    size_t len;
    int state;
    bool failed, async, defer, parser;
};

typedef struct jreq {
    struct bjs *J;
    int id;
    http_request_t *req;
    int cb;                      /* root: fetch's callback      */
    jscript_t *sc;               /* or a script being loaded    */
    struct jreq *next;
} jreq_t;

typedef struct jitem { char *k, *v; } jitem_t;
typedef struct jstore {
    char origin[300];
    tab_t *tab;                  /* sessionStorage: the tab's own (NULL: localStorage) */
    jitem_t *it;
    int n, cap;
    size_t bytes;
    bool dirty;
    struct jstore *next;
} jstore_t;

typedef struct selc { char *text; web_sel_t *sel; uint32_t used; } selc_t;

typedef struct bjs {
    tab_t *t;
    int id;
    struct bjs *next;
    JSContext *ctx;
    web_doc_t *doc;
    int cls_node, cls_store;
    JSValue rt, docobj, protos;  /* pinned */
    JSValue p_text, p_comment, p_frag, p_html, p_unknown, p_svg, p_doc;
    JSValue tagproto[T_COUNT];
    bool dying;
    /* nodes scripts made (and arena nodes with malloc'd parts) */
    web_node_t **hs;
    int hcap, hn;
    web_node_t **cand;           /* trees that may have become unreachable */
    int ncand, candcap;
    /* tasks */
    jtask_t *qh, *qt, *cur;
    uint32_t cur_ms;
    bool cur_user;
    /* timers */
    jtimer_t *tm;
    int ntm, tmcap, next_id;
    /* scripts */
    jscript_t **sc;
    int nsc, sccap;
    jscript_t *running;
    int ready;                   /* 0 loading, 1 interactive, 2 complete */
    uint32_t dcl_at;
    bool prefer_nomodule;
    /* requests */
    jreq_t *reqs;
    int next_req;
    /* the document */
    uint32_t version;
    bool dirty, repaint, dirty_css, observe;
    uint32_t flush_at, flush_cost;
    web_node_t *focus;           /* focused element (its wrapper is rooted) */
    int focus_root;
    uint32_t listen;             /* 1 hover events, 2 pointer events */
    selc_t selc[24];
    uint32_t selc_tick;
    /* dialog */
    int dlg;                     /* 0 none, 1 open, 2 answered */
    int dlg_kind;
    bool dlg_ok;
    char *dlg_text;
    /* to do once no script runs (navigation, popups, submitting) */
    char *nav_url;
    int nav_mode;
    int hist_go;
    char *open_url;
    int follow_root, submit_root, submit_ctl;
    /* last pointer position (document coordinates): links' click target */
    int px, py, pmods;
    /* console */
    int nlog;
    uint32_t log_sec;
    double wall0;
    uint32_t start_ms;           /* performance.now() counts from here */
} bjs_t;

static bjs_t *g_list;
static int g_next_id = 1;
static jstore_t *g_stores;
static uint32_t g_store_saved;

static bjs_t *by_id(int id) {
    for (bjs_t *J = g_list; J; J = J->next) if (J->id == id) return J;
    return NULL;
}

/* ==== small helpers ======================================================================== */
static char *dupstr(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

static char *dupn(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (p) { memcpy(p, s, n); p[n] = 0; }
    return p;
}

static void repaint(tab_t *t) { browser_page_changed(t); }

static JSValue str_1252(JSContext *ctx, const char *s, int len) {
    if (!s || len <= 0) return js_new_string_len(ctx, "", 0);
    bool ascii = true;
    for (int i = 0; i < len; i++) if ((unsigned char)s[i] >= 0x80) { ascii = false; break; }
    if (ascii) return js_new_string_len(ctx, s, (size_t)len);
    uint16_t *b = malloc(sizeof(uint16_t) * (size_t)len);
    if (!b) return js_new_string_len(ctx, "", 0);
    for (int i = 0; i < len; i++) b[i] = (uint16_t)web_1252_to_cp((unsigned char)s[i]);
    JSValue v = js_new_string16(ctx, b, (size_t)len);
    free(b);
    return v;
}

static JSValue cstr_1252(JSContext *ctx, const char *s) { return str_1252(ctx, s, s ? (int)strlen(s) : 0); }

/* JS value -> malloc'd Windows-1252 text (what page text is kept in) */
static char *to_1252(JSContext *ctx, JSValue v, int *outlen) {
    JSValue s = js_to_string(ctx, v);
    if (JS_IsException(s)) { js_take_exception(ctx); s = js_new_string(ctx, ""); }
    uint32_t n = 0;
    const uint16_t *u = js_string_chars(ctx, s, &n);
    char *out = malloc((size_t)n + 1);
    if (!out) { if (outlen) *outlen = 0; return NULL; }
    int o = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        }
        int b = web_cp_to_1252(cp);
        if (b >= 0) out[o++] = (char)b;
    }
    out[o] = 0;
    if (outlen) *outlen = o;
    return out;
}

/* 1252 -> UTF-8 (malloc'd) */
static char *utf8_of_1252(const char *s, size_t n, size_t *outlen) {
    char *o = malloc(n * 3 + 1);
    if (!o) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = web_1252_to_cp((unsigned char)s[i]);
        if (cp < 0x80) o[k++] = (char)cp;
        else if (cp < 0x800) { o[k++] = (char)(0xC0 | (cp >> 6)); o[k++] = (char)(0x80 | (cp & 0x3F)); }
        else { o[k++] = (char)(0xE0 | (cp >> 12)); o[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[k++] = (char)(0x80 | (cp & 0x3F)); }
    }
    o[k] = 0;
    if (outlen) *outlen = k;
    return o;
}

static bool valid_utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i];
        if (c < 0x80) { i++; continue; }
        int l = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
        if (!l || i + (size_t)l > n) return false;
        for (int k = 1; k < l; k++) if ((s[i + k] & 0xC0) != 0x80) return false;
        i += (size_t)l;
    }
    return true;
}

static char *js_utf8(JSContext *ctx, JSValue v, size_t *len) {
    char *s = js_to_cstring(ctx, v, len);
    if (!s) { js_take_exception(ctx); s = dupstr(""); if (len) *len = 0; }
    return s;
}

static JSValue throw_dom(bjs_t *J, const char *name, const char *msg) {
    JSValue c = js_get(J->ctx, js_global(J->ctx), "DOMException");
    JSValue a[2] = { js_new_string(J->ctx, msg), js_new_string(J->ctx, name) };
    JSValue e = js_construct(J->ctx, c, 2, a);
    if (JS_IsException(e)) return e;
    return js_throw(J->ctx, e);
}

#define JTHIS bjs_t *J = js_get_host_ud(ctx); (void)J; (void)this_val; (void)magic
#define NODE(v) ((web_node_t *)js_host_opaque((v), J->cls_node))

/* ==== heap nodes =========================================================================== */
static uint32_t hptr(const void *p) { uintptr_t x = (uintptr_t)p; return (uint32_t)((x >> 4) ^ (x >> 13)) * 2654435761u; }
#define HS_TOMB ((web_node_t *)1)

static void hs_add(bjs_t *J, web_node_t *n) {
    if ((J->hn + 1) * 2 > J->hcap) {
        int nc = J->hcap ? J->hcap * 2 : 256;
        web_node_t **nh = calloc((size_t)nc, sizeof *nh);
        if (!nh) return;
        for (int i = 0; i < J->hcap; i++) {
            web_node_t *x = J->hs[i];
            if (!x || x == HS_TOMB) continue;
            uint32_t h = hptr(x) & (uint32_t)(nc - 1);
            while (nh[h]) h = (h + 1) & (uint32_t)(nc - 1);
            nh[h] = x;
        }
        free(J->hs);
        J->hs = nh;
        J->hcap = nc;
    }
    uint32_t h = hptr(n) & (uint32_t)(J->hcap - 1);
    int tomb = -1;
    while (J->hs[h]) {
        if (J->hs[h] == n) return;
        if (J->hs[h] == HS_TOMB && tomb < 0) tomb = (int)h;
        h = (h + 1) & (uint32_t)(J->hcap - 1);
    }
    J->hs[tomb >= 0 ? (uint32_t)tomb : h] = n;
    J->hn++;
}

static void hs_del(bjs_t *J, web_node_t *n) {
    if (!J->hcap) return;
    uint32_t h = hptr(n) & (uint32_t)(J->hcap - 1);
    while (J->hs[h]) {
        if (J->hs[h] == n) { J->hs[h] = HS_TOMB; return; }
        h = (h + 1) & (uint32_t)(J->hcap - 1);
    }
}

static void free_attrs(web_node_t *n) {
    if (!(n->jsf & WNJ_ATTRS)) return;
    for (int i = 0; i < n->nattrs; i++) { free((char *)n->attrs[i].name); free((char *)n->attrs[i].value); }
    free(n->attrs);
    if (n->cls) { free((char *)n->cls[n->ncls]); free(n->cls); }
    n->attrs = NULL; n->nattrs = 0; n->cls = NULL; n->ncls = 0; n->id = NULL;
    n->jsf &= (uint16_t)~WNJ_ATTRS;
}

static void free_parts(web_node_t *n) {
    free_attrs(n);
    if (n->jsf & WNJ_TEXT) { free(n->text); n->text = NULL; n->len = 0; n->jsf &= (uint16_t)~WNJ_TEXT; }
}

static void release(bjs_t *J, web_node_t *n) {
    free_parts(n);
    n->jsf |= WNJ_DEAD;
    if (n->jsf & WNJ_HEAP) { hs_del(J, n); free(n); return; }
    n->parent = n->next = n->prev = n->first = n->last = NULL;
}

/* free a tree nothing reaches any more, children first (arena nodes are only marked dead) */
static void free_tree(bjs_t *J, web_node_t *top) {
    web_node_t *n = top;
    for (;;) {
        while (n->first) n = n->first;
        web_node_t *p = n->parent, *next = n->next;
        if (n == top) { release(J, n); return; }
        p->first = next;
        if (next) next->prev = NULL; else p->last = NULL;
        release(J, n);
        n = next ? next : p;
    }
}

static void cand_add(bjs_t *J, web_node_t *n) {
    if (J->ncand >= J->candcap) {
        int nc = J->candcap ? J->candcap * 2 : 64;
        web_node_t **x = realloc(J->cand, sizeof *x * (size_t)nc);
        if (!x) return;
        J->cand = x;
        J->candcap = nc;
    }
    J->cand[J->ncand++] = n;
}

static web_node_t *top_of(web_node_t *n) { while (n->parent) n = n->parent; return n; }

/* next node of top's subtree in document order */
static web_node_t *walk_next(web_node_t *n, const web_node_t *top) {
    if (n->first) return n->first;
    while (n && n != top) { if (n->next) return n->next; n = n->parent; }
    return NULL;
}

static bool tree_has_wrapper(web_node_t *top) {
    for (web_node_t *n = top; n; n = walk_next(n, top)) if (n->jsv) return true;
    return false;
}

/* after a layout: trees that scripts dropped and nothing reaches are freed */
static void free_dropped(bjs_t *J) {
    if (!J->ncand) return;
    web_node_t *root = J->doc->root;
    uint32_t mark = 0xFFFFFFF0u;
    int n = 0;
    for (int i = 0; i < J->ncand; i++) {
        web_node_t *x = J->cand[i];
        if (x->jsf & WNJ_DEAD) continue;
        web_node_t *top = top_of(x);
        if (top == root || top->jsmark == mark) continue;
        top->jsmark = mark;
        J->cand[n++] = top;
    }
    for (int i = 0; i < n; i++) {
        web_node_t *top = J->cand[i];
        top->jsmark = 0;
        if (!tree_has_wrapper(top)) free_tree(J, top);
    }
    J->ncand = 0;
}

/* ==== wrappers ============================================================================= */
static void node_finalize(JSContext *ctx, void *op) {
    web_node_t *n = op;
    bjs_t *J = js_get_host_ud(ctx);
    if (!n) return;
    n->jsv = 0;
    if (J && !J->dying && !(n->jsf & WNJ_DEAD)) cand_add(J, n);
}

/* a wrapper keeps its whole tree's wrappers alive (and so the tree): its listeners,
   its properties; the document's tree is always alive (the document is pinned) */
static void node_mark(JSContext *ctx, void *op) {
    web_node_t *n = op;
    if (!n) return;
    web_node_t *top = top_of(n);
    uint32_t ep = js_gc_count(ctx) + 1;
    if (top->jsmark == ep) return;
    top->jsmark = ep;
    for (web_node_t *x = top; x; x = walk_next(x, top)) if (x->jsv && x != n) js_mark(ctx, x->jsv);
}

static bool in_svg(const web_node_t *n) {
    if (n->jsf & WNJ_SVG) return true;
    for (const web_node_t *p = n; p && p->type == WN_ELEM; p = p->parent) if (p->tag == T_SVG) return true;
    return false;
}

static JSValue wrap(bjs_t *J, web_node_t *n) {
    if (!n) return JS_NULL;
    while (n->pseudo && n->parent) n = n->parent;
    if (n->jsv) return n->jsv;
    JSValue proto;
    switch (n->type) {
    case WN_TEXT: proto = J->p_text; break;
    case WN_COMMENT: proto = J->p_comment; break;
    case WN_FRAG: proto = J->p_frag; break;
    case WN_DOC: proto = J->p_doc; break;
    default:
        if (in_svg(n)) proto = J->p_svg;
        else if (n->tag && n->tag < T_COUNT && JS_IsObject(J->tagproto[n->tag])) proto = J->tagproto[n->tag];
        else if (!n->tag && n->name && strchr(n->name, '-')) proto = J->p_html;
        else if (!n->tag) proto = J->p_unknown;
        else proto = J->p_html;
        break;
    }
    JSValue o = js_new_host_object(J->ctx, J->cls_node, proto, n);
    if (JS_IsException(o)) return JS_NULL;
    n->jsv = o;
    return o;
}

/* ==== nodes ================================================================================ */
static const char *intern(const char *name) {
    static char *tab[512];
    static int n;
    int tag = web_tag_lookup(name);
    if (tag) return web_tag_name(tag);
    for (int i = 0; i < n; i++) if (!strcmp(tab[i], name)) return tab[i];
    char *s = dupstr(name);
    if (!s) return "span";
    if (n < (int)ARRAY_SIZE(tab)) tab[n++] = s;
    return s;
}

static web_node_t *node_new(bjs_t *J, int type) {
    web_node_t *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->type = (uint8_t)type;
    n->ctl = n->img = n->link = -1;
    n->ninl = -1;
    n->jsf = WNJ_HEAP;
    hs_add(J, n);
    return n;
}

static web_node_t *elem_new(bjs_t *J, const char *name) {
    web_node_t *n = node_new(J, WN_ELEM);
    if (!n) return NULL;
    n->tag = (uint16_t)web_tag_lookup(name);
    n->name = intern(name);
    return n;
}

static void set_text(bjs_t *J, web_node_t *n, char *t, int len) {
    if (n->jsf & WNJ_TEXT) free(n->text);
    n->text = t;
    n->len = len;
    n->jsf = (uint16_t)((n->jsf | WNJ_TEXT) & ~WNJ_UTF8);
    hs_add(J, n);
}

static web_node_t *text_new(bjs_t *J, int type, char *t, int len) {
    web_node_t *n = node_new(J, type);
    if (!n) { free(t); return NULL; }
    set_text(J, n, t ? t : dupstr(""), t ? len : 0);
    return n;
}

static bool visible(const web_node_t *c) { return !c->pseudo && !(c->jsf & WNJ_TPL); }
static web_node_t *vfirst(web_node_t *p) { for (web_node_t *c = p->first; c; c = c->next) if (visible(c)) return c; return NULL; }
static web_node_t *vlast(web_node_t *p) { for (web_node_t *c = p->last; c; c = c->prev) if (visible(c)) return c; return NULL; }
static web_node_t *vnext(web_node_t *n) { for (web_node_t *c = n->next; c; c = c->next) if (visible(c)) return c; return NULL; }
static web_node_t *vprev(web_node_t *n) { for (web_node_t *c = n->prev; c; c = c->prev) if (visible(c)) return c; return NULL; }
static bool connected(bjs_t *J, web_node_t *n) { return top_of(n) == J->doc->root; }

/* walk an element's (or document's / fragment's) visible subtree, n excluded */
static web_node_t *dom_next(web_node_t *n, const web_node_t *top) {
    web_node_t *c = vfirst(n);
    if (c && n->type != WN_TEXT) return c;
    while (n && n != top) {
        web_node_t *s = vnext(n);
        if (s) return s;
        n = n->parent;
    }
    return NULL;
}

static void strip_pseudo(web_node_t *p) {
    while (p->first && p->first->pseudo) { p->first = p->first->next; if (p->first) p->first->prev = NULL; else p->last = NULL; }
    while (p->last && p->last->pseudo) { p->last = p->last->prev; if (p->last) p->last->next = NULL; else p->first = NULL; }
}

static void changed(bjs_t *J) { J->version++; J->dirty = true; }

static bool styles_inside(web_node_t *n) {
    for (web_node_t *x = n; x; x = walk_next(x, n))
        if (x->type == WN_ELEM && (x->tag == T_STYLE || x->tag == T_LINK)) return true;
    return false;
}

static void mutation(bjs_t *J, int type, web_node_t *target, JSValue a, JSValue b, web_node_t *prev, web_node_t *next) {
    if (!J->observe) return;
    JSValue fn = js_get(J->ctx, J->rt, "mutation");
    JSValue args[7] = { js_mknum(type), wrap(J, target), a, b, prev ? wrap(J, prev) : JS_NULL, next ? wrap(J, next) : JS_NULL, JS_UNDEFINED };
    JSValue r = js_call(J->ctx, fn, JS_UNDEFINED, 6, args);
    if (JS_IsException(r)) js_report_exception(J->ctx);
}

static JSValue one_array(bjs_t *J, web_node_t *n) {
    JSValue a = js_new_array(J->ctx, 0);
    if (n) js_array_push(J->ctx, a, wrap(J, n));
    return a;
}

static void detach(bjs_t *J, web_node_t *n) {
    web_node_t *p = n->parent;
    if (!p) return;
    web_node_t *prev = J->observe ? vprev(n) : NULL, *next = J->observe ? vnext(n) : NULL;
    bool was_conn = connected(J, n);
    strip_pseudo(p);
    if (n->prev) n->prev->next = n->next; else p->first = n->next;
    if (n->next) n->next->prev = n->prev; else p->last = n->prev;
    n->parent = n->prev = n->next = NULL;
    /* its styles and generated content live in the styles' arena, which the next
       styling frees (and styles only the document) */
    for (web_node_t *x = n; x; x = walk_next(x, n))
        if (x->type == WN_ELEM && !x->pseudo) { x->st = NULL; strip_pseudo(x); }
    changed(J);
    if (was_conn) {
        if (styles_inside(n)) J->dirty_css = true;
        if (J->focus && top_of(J->focus) == n) {
            if (J->t->focus_ctrl >= 0 && J->focus->ctl == J->t->focus_ctrl) J->t->focus_ctrl = -1;
            J->focus = NULL;
            js_root_set(J->ctx, J->focus_root, JS_NULL);
        }
    }
    cand_add(J, n);
    if (J->observe) mutation(J, 0, p, js_new_array(J->ctx, 0), one_array(J, n), prev, next);
}

static void run_inserted_scripts(bjs_t *J, web_node_t *n);

static void link_before(web_node_t *p, web_node_t *n, web_node_t *ref) {
    n->parent = p;
    if (ref) {
        n->next = ref;
        n->prev = ref->prev;
        if (ref->prev) ref->prev->next = n; else p->first = n;
        ref->prev = n;
    } else {
        n->prev = p->last;
        n->next = NULL;
        if (p->last) p->last->next = n; else p->first = n;
        p->last = n;
    }
    n->depth = p->depth < 255 ? (uint8_t)(p->depth + 1) : 255;
}

/* insert n (a fragment: its children) into p before ref; checks done by the caller */
static void insert(bjs_t *J, web_node_t *p, web_node_t *n, web_node_t *ref) {
    strip_pseudo(p);
    bool conn = connected(J, p);
    if (n->type == WN_FRAG) {
        strip_pseudo(n);
        JSValue added = J->observe ? js_new_array(J->ctx, 0) : JS_UNDEFINED;
        web_node_t *c;
        while ((c = n->first)) {
            n->first = c->next;
            if (n->first) n->first->prev = NULL; else n->last = NULL;
            c->parent = c->prev = c->next = NULL;
            link_before(p, c, ref);
            if (J->observe) js_array_push(J->ctx, added, wrap(J, c));
            if (conn) {
                if (styles_inside(c)) J->dirty_css = true;
                run_inserted_scripts(J, c);
            }
        }
        changed(J);
        if (J->observe) mutation(J, 0, p, added, js_new_array(J->ctx, 0), NULL, ref);
        return;
    }
    if (n->parent) detach(J, n);
    link_before(p, n, ref);
    changed(J);
    if (J->observe) mutation(J, 0, p, one_array(J, n), js_new_array(J->ctx, 0), vprev(n), vnext(n));
    if (conn) {
        if (styles_inside(n)) J->dirty_css = true;
        run_inserted_scripts(J, n);
    }
}

static bool may_insert(bjs_t *J, web_node_t *p, web_node_t *n, web_node_t *ref, JSValue *err) {
    if (p->type != WN_ELEM && p->type != WN_DOC && p->type != WN_FRAG) {
        *err = throw_dom(J, "HierarchyRequestError", "Failed to execute 'insertBefore' on 'Node': This node type does not support this method.");
        return false;
    }
    if (n->type == WN_DOC) {
        *err = throw_dom(J, "HierarchyRequestError", "Failed to execute 'insertBefore' on 'Node': Nodes of type '#document' may not be inserted inside nodes of type '#document'.");
        return false;
    }
    for (web_node_t *a = p; a; a = a->parent)
        if (a == n) {
            *err = throw_dom(J, "HierarchyRequestError", "Failed to execute 'appendChild' on 'Node': The new child element contains the parent.");
            return false;
        }
    if (ref && ref->parent != p) {
        *err = throw_dom(J, "NotFoundError", "Failed to execute 'insertBefore' on 'Node': The node before which the new node is to be inserted is not a child of this node.");
        return false;
    }
    return true;
}

static bool own_attrs(bjs_t *J, web_node_t *n) {
    if (n->jsf & WNJ_ATTRS) return true;
    web_attr_t *a = NULL;
    if (n->nattrs) {
        a = calloc((size_t)n->nattrs, sizeof *a);
        if (!a) return false;
        for (int i = 0; i < n->nattrs; i++) { a[i].name = dupstr(n->attrs[i].name); a[i].value = dupstr(n->attrs[i].value ? n->attrs[i].value : ""); }
    }
    n->attrs = a;
    n->cls = NULL;
    n->ncls = 0;
    n->jsf |= WNJ_ATTRS;
    hs_add(J, n);
    return true;
}

/* id and class names from the (owned) attributes */
static void attrs_derived(web_node_t *n) {
    if (n->cls) { free((char *)n->cls[n->ncls]); free(n->cls); n->cls = NULL; n->ncls = 0; }
    n->id = web_attr(n, "id");
    if (n->id && !*n->id) n->id = NULL;
    const char *c = web_attr(n, "class");
    if (!c || !*c) return;
    char *buf = dupstr(c);
    if (!buf) return;
    int cnt = 0;
    for (char *q = buf; *q;) {
        while (*q && web_isspace((unsigned char)*q)) q++;
        if (!*q) break;
        cnt++;
        while (*q && !web_isspace((unsigned char)*q)) q++;
    }
    const char **cls = malloc(sizeof(char *) * (size_t)(cnt + 1));
    if (!cls) { free(buf); return; }
    int k = 0;
    for (char *q = buf; *q;) {
        while (*q && web_isspace((unsigned char)*q)) *q++ = 0;
        if (!*q) break;
        cls[k++] = q;
        while (*q && !web_isspace((unsigned char)*q)) q++;
    }
    cls[k] = buf;
    n->cls = cls;
    n->ncls = k;
}

static void attr_set(bjs_t *J, web_node_t *n, const char *name, const char *val) {
    if (!own_attrs(J, n)) return;
    char *old = NULL;
    for (int i = 0; i < n->nattrs; i++)
        if (!strcmp(n->attrs[i].name, name)) {
            if (!strcmp(n->attrs[i].value, val)) return;
            old = (char *)n->attrs[i].value;
            n->attrs[i].value = dupstr(val);
            goto done;
        }
    {
        web_attr_t *na = realloc(n->attrs, sizeof *na * (size_t)(n->nattrs + 1));
        if (!na) return;
        n->attrs = na;
        na[n->nattrs].name = dupstr(name);
        na[n->nattrs].value = dupstr(val);
        n->nattrs++;
    }
done:
    if (!strcmp(name, "id") || !strcmp(name, "class")) attrs_derived(n);
    changed(J);
    if ((n->tag == T_STYLE || n->tag == T_LINK) && connected(J, n)) J->dirty_css = true;
    if (J->observe) mutation(J, 1, n, js_new_string(J->ctx, name), old ? cstr_1252(J->ctx, old) : JS_NULL, NULL, NULL);
    free(old);
}

static void attr_del(bjs_t *J, web_node_t *n, const char *name) {
    int i = 0;
    for (; i < n->nattrs; i++) if (!strcmp(n->attrs[i].name, name)) break;
    if (i >= n->nattrs || !own_attrs(J, n)) return;
    char *oldv = (char *)n->attrs[i].value;
    free((char *)n->attrs[i].name);
    memmove(&n->attrs[i], &n->attrs[i + 1], sizeof(web_attr_t) * (size_t)(n->nattrs - i - 1));
    n->nattrs--;
    if (!strcmp(name, "id") || !strcmp(name, "class")) attrs_derived(n);
    changed(J);
    if ((n->tag == T_STYLE || n->tag == T_LINK) && connected(J, n)) J->dirty_css = true;
    if (J->observe) mutation(J, 1, n, js_new_string(J->ctx, name), cstr_1252(J->ctx, oldv), NULL, NULL);
    free(oldv);
}

/* a copy of n (and its subtree) made of malloc'd nodes */
static web_node_t *clone(bjs_t *J, web_node_t *src, bool deep) {
    web_node_t *n = node_new(J, src->type == WN_DOC ? WN_FRAG : src->type);
    if (!n) return NULL;
    n->tag = src->tag;
    n->name = src->name ? intern(src->name) : NULL;
    n->jsf |= (uint16_t)(src->jsf & (WNJ_UTF8 | WNJ_STARTED | WNJ_TPL | WNJ_SVG));
    if (src->type == WN_TEXT || src->type == WN_COMMENT) {
        n->text = dupn(src->text ? src->text : "", (size_t)src->len);
        n->len = n->text ? src->len : 0;
        n->jsf |= WNJ_TEXT;
    }
    if (src->type == WN_ELEM && src->nattrs) {
        n->attrs = calloc((size_t)src->nattrs, sizeof(web_attr_t));
        if (n->attrs) {
            for (int i = 0; i < src->nattrs; i++) { n->attrs[i].name = dupstr(src->attrs[i].name); n->attrs[i].value = dupstr(src->attrs[i].value ? src->attrs[i].value : ""); }
            n->nattrs = src->nattrs;
            n->jsf |= WNJ_ATTRS;
            attrs_derived(n);
        }
    }
    if (deep)
        for (web_node_t *c = src->first; c; c = c->next) {
            if (c->pseudo) continue;
            web_node_t *cc = clone(J, c, true);
            if (cc) link_before(n, cc, NULL);
        }
    return n;
}

/* ==== text, serialisation ================================================================== */
typedef struct sb16 { uint16_t *b; size_t n, cap; bool bad; } sb16_t;

static void sb_put(sb16_t *s, uint32_t cp) {
    if (s->n + 2 > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 256;
        uint16_t *nb = realloc(s->b, nc * sizeof(uint16_t));
        if (!nb) { s->bad = true; return; }
        s->b = nb;
        s->cap = nc;
    }
    if (cp >= 0x10000) { cp -= 0x10000; s->b[s->n++] = (uint16_t)(0xD800 + (cp >> 10)); s->b[s->n++] = (uint16_t)(0xDC00 + (cp & 0x3FF)); }
    else s->b[s->n++] = (uint16_t)cp;
}

static void sb_ascii(sb16_t *s, const char *t) { for (; *t; t++) sb_put(s, (unsigned char)*t); }

/* esc: 0 raw, 1 text (& < >), 2 attribute value (& ") */
static void sb_text(sb16_t *s, const web_node_t *n, const char *t, int len, int esc) {
    bool utf8 = n && (n->jsf & WNJ_UTF8);
    for (int i = 0; i < len;) {
        uint32_t cp;
        unsigned c = (unsigned char)t[i];
        if (utf8 && c >= 0x80) {
            int l = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            if (i + l > len) l = 1;
            cp = l == 1 ? 0xFFFD : c & (0x3F >> (l - 1));
            for (int k = 1; k < l; k++) cp = (cp << 6) | ((unsigned char)t[i + k] & 0x3F);
            i += l;
        } else {
            cp = utf8 ? c : web_1252_to_cp((int)c);
            i++;
        }
        if (esc && cp == '&') { sb_ascii(s, "&amp;"); continue; }
        if (esc == 1 && cp == '<') { sb_ascii(s, "&lt;"); continue; }
        if (esc == 1 && cp == '>') { sb_ascii(s, "&gt;"); continue; }
        if (esc == 2 && cp == '"') { sb_ascii(s, "&quot;"); continue; }
        if (esc && cp == 0xA0) { sb_ascii(s, "&nbsp;"); continue; }
        sb_put(s, cp);
    }
}

static bool void_tag(int t) {
    switch (t) {
    case T_AREA: case T_BASE: case T_BR: case T_COL: case T_EMBED: case T_HR: case T_IMG: case T_INPUT:
    case T_LINK: case T_META: case T_PARAM: case T_SOURCE: case T_TRACK: case T_WBR: case T_BASEFONT: case T_FRAME:
        return true;
    default:
        return false;
    }
}

static void ser_children(sb16_t *s, web_node_t *n, int depth);

static void ser_node(sb16_t *s, web_node_t *n, int depth) {
    if (n->pseudo || (n->jsf & WNJ_TPL) || depth > 400) return;
    switch (n->type) {
    case WN_TEXT: {
        const web_node_t *p = n->parent;
        bool raw = p && p->type == WN_ELEM && (p->tag == T_SCRIPT || p->tag == T_STYLE || p->tag == T_XMP ||
                   p->tag == T_IFRAME || p->tag == T_NOEMBED || p->tag == T_NOFRAMES || p->tag == T_PLAINTEXT);
        sb_text(s, n, n->text, n->len, raw ? 0 : 1);
        return;
    }
    case WN_COMMENT:
        sb_ascii(s, "<!--");
        sb_text(s, n, n->text, n->len, 0);
        sb_ascii(s, "-->");
        return;
    case WN_ELEM:
        sb_put(s, '<');
        sb_ascii(s, n->name ? n->name : "");
        for (int i = 0; i < n->nattrs; i++) {
            sb_put(s, ' ');
            sb_text(s, NULL, n->attrs[i].name, (int)strlen(n->attrs[i].name), 0);
            sb_ascii(s, "=\"");
            const char *v = n->attrs[i].value ? n->attrs[i].value : "";
            sb_text(s, NULL, v, (int)strlen(v), 2);
            sb_put(s, '"');
        }
        sb_put(s, '>');
        if (void_tag(n->tag)) return;
        if (n->tag == T_TEMPLATE)
            for (web_node_t *c = n->first; c; c = c->next) if (c->jsf & WNJ_TPL) sb_text(s, c, c->text, c->len, 0);
        ser_children(s, n, depth + 1);
        sb_ascii(s, "</");
        sb_ascii(s, n->name ? n->name : "");
        sb_put(s, '>');
        return;
    default:
        ser_children(s, n, depth + 1);
        return;
    }
}

static void ser_children(sb16_t *s, web_node_t *n, int depth) {
    for (web_node_t *c = n->first; c; c = c->next) ser_node(s, c, depth);
}

static JSValue sb_value(JSContext *ctx, sb16_t *s) {
    JSValue v = js_new_string16(ctx, s->b ? s->b : (const uint16_t *)"", s->n);
    free(s->b);
    return v;
}

static void text_of(sb16_t *s, web_node_t *n) {
    for (web_node_t *x = n; x; x = walk_next(x, n)) {
        if (x->pseudo || (x->jsf & WNJ_TPL)) continue;
        if (x->type == WN_TEXT) sb_text(s, x, x->text, x->len, 0);
    }
}

static JSValue data_value(bjs_t *J, web_node_t *n) {
    if (n->jsf & WNJ_UTF8) return js_new_string_len(J->ctx, n->text ? n->text : "", (size_t)n->len);
    return str_1252(J->ctx, n->text, n->len);
}

/* ==== fragments (innerHTML) ================================================================ */
static web_node_t *parse_frag(bjs_t *J, const char *utf8, size_t len, const web_node_t *context) {
    static const struct { int tag; const char *pre; const int path[3]; } wraps[] = {
        { T_TABLE, "<table>", { T_TABLE, 0, 0 } },
        { T_TBODY, "<table><tbody>", { T_TABLE, T_TBODY, 0 } },
        { T_THEAD, "<table><thead>", { T_TABLE, T_THEAD, 0 } },
        { T_TFOOT, "<table><tfoot>", { T_TABLE, T_TFOOT, 0 } },
        { T_TR, "<table><tbody><tr>", { T_TABLE, T_TBODY, T_TR } },
        { T_SELECT, "<select>", { T_SELECT, 0, 0 } },
        { T_COLGROUP, "<table><colgroup>", { T_TABLE, T_COLGROUP, 0 } },
    };
    int w = -1;
    for (int i = 0; i < (int)ARRAY_SIZE(wraps); i++) if (context && context->tag == wraps[i].tag) w = i;
    size_t pl = w >= 0 ? strlen(wraps[w].pre) : 0;
    char *src = malloc(pl + len + 1);
    if (!src) return NULL;
    if (pl) memcpy(src, wraps[w].pre, pl);
    memcpy(src + pl, utf8, len);
    src[pl + len] = 0;
    web_doc_t *d = web_parse_html_ex(src, pl + len, WEB_CS_UTF8, WEB_PARSE_BODY | WEB_PARSE_SCRIPTS);
    free(src);
    web_node_t *frag = node_new(J, WN_FRAG);
    if (!d || !frag) { web_doc_free(d); return frag; }
    web_node_t *box = d->body;
    if (box && w >= 0)
        for (int k = 0; k < 3 && wraps[w].path[k]; k++) {
            web_node_t *c = box->first;
            while (c && !(c->type == WN_ELEM && c->tag == wraps[w].path[k])) c = c->next;
            if (!c) break;
            box = c;
        }
    if (box)
        for (web_node_t *c = box->first; c; c = c->next) {
            web_node_t *cc = clone(J, c, true);
            if (!cc) continue;
            for (web_node_t *x = cc; x; x = walk_next(x, cc))
                if (x->type == WN_ELEM && x->tag == T_SCRIPT) x->jsf |= WNJ_STARTED;   /* never run */
            link_before(frag, cc, NULL);
        }
    web_doc_free(d);
    return frag;
}

/* ==== tasks ================================================================================ */
static void finish(bjs_t *J, jtask_t *k, int st);
static void flush(bjs_t *J, bool force);

static void task_free(bjs_t *J, jtask_t *k) {
    if (k->fn >= 0) js_root_free(J->ctx, k->fn);
    if (k->self >= 0) js_root_free(J->ctx, k->self);
    if (k->args >= 0) js_root_free(J->ctx, k->args);
    free(k);
}

static jtask_t *task_new(bjs_t *J, int kind) {
    jtask_t *k = calloc(1, sizeof *k);
    if (!k) return NULL;
    k->kind = kind;
    k->fn = k->self = k->args = -1;
    (void)J;
    return k;
}

static void q_push(bjs_t *J, jtask_t *k) {
    k->next = NULL;
    if (J->qt) J->qt->next = k; else J->qh = k;
    J->qt = k;
}

/* call fn(this, args...) as a task of its own (after the ones queued) */
static jtask_t *q_call(bjs_t *J, JSValue fn, JSValue self, int argc, const JSValue *argv, bool user) {
    jtask_t *k = task_new(J, JT_CALL);
    if (!k) return NULL;
    JSValue a = js_new_array(J->ctx, 0);
    for (int i = 0; i < argc; i++) js_array_push(J->ctx, a, argv[i]);
    k->fn = js_root_add(J->ctx, fn);
    k->self = js_root_add(J->ctx, self);
    k->args = js_root_add(J->ctx, a);
    k->user = user;
    q_push(J, k);
    return k;
}

static int task_start(bjs_t *J, jtask_t *k) {
    J->cur_ms = 0;
    J->cur_user = k->user;
    uint32_t t0 = sys_uptime_ms();
    int st;
    if (k->kind == JT_SCRIPT) {
        jscript_t *sc = k->sc;
        J->running = sc;
        sc->state = SC_RUNNING;
        char name[256];
        if (sc->url) strlcpy(name, sc->url, sizeof name);
        else snprintf(name, sizeof name, "%s (inline script)", J->t->url);
        st = js_eval_task(J->ctx, sc->src ? sc->src : "", sc->len, name, 1);
    } else {
        JSValue a = js_root_get(J->ctx, k->args);
        uint32_t n = js_array_length(J->ctx, a);
        JSValue argv[16];
        if (n > 16) n = 16;
        for (uint32_t i = 0; i < n; i++) argv[i] = js_get_index(J->ctx, a, i);
        st = js_call_task(J->ctx, js_root_get(J->ctx, k->fn), js_root_get(J->ctx, k->self), (int)n, argv);
    }
    J->cur_ms += sys_uptime_ms() - t0;
    return st;
}

/* run queued tasks while scripts are free and the tick's time lasts */
static void q_run(bjs_t *J, uint32_t budget_ms) {
    uint32_t t0 = sys_uptime_ms();
    while (J->qh && !J->cur && !js_busy(J->ctx) && !J->dying) {
        jtask_t *k = J->qh;
        J->qh = k->next;
        if (!J->qh) J->qt = NULL;
        int st = task_start(J, k);
        if (st == JS_SUSPENDED) { J->cur = k; break; }
        finish(J, k, st);
        if (sys_uptime_ms() - t0 >= budget_ms) break;
    }
}

static void script_done(bjs_t *J, jscript_t *sc);

static void finish(bjs_t *J, jtask_t *k, int st) {
    (void)st;
    if (k->kind == JT_SCRIPT && k->sc) script_done(J, k->sc);
    J->cur_user = false;
    task_free(J, k);
}

/* call a hook of browser_jsrt.js at once when scripts are free (else queued); *ret: its result */
static bool hook_now(bjs_t *J, const char *name, int argc, const JSValue *argv, JSValue *ret, bool user) {
    JSValue fn = js_get(J->ctx, J->rt, name);
    if (J->cur || J->qh || js_busy(J->ctx)) {
        q_call(J, fn, JS_UNDEFINED, argc, argv, user);
        return false;
    }
    J->cur_ms = 0;
    J->cur_user = user;
    uint32_t t0 = sys_uptime_ms();
    int st = js_call_task(J->ctx, fn, JS_UNDEFINED, argc, argv);
    J->cur_ms += sys_uptime_ms() - t0;
    if (st == JS_SUSPENDED) {
        /* (waits for a dialog or ran out of its slice: the tick carries it on) */
        jtask_t *k = task_new(J, JT_CALL);
        if (k) { k->user = user; J->cur = k; }
        return false;
    }
    J->cur_user = false;
    if (ret) *ret = st == JS_DONE ? js_task_value(J->ctx) : JS_UNDEFINED;
    return st == JS_DONE;
}

/* ==== the document's natives (N.*) ========================================================= */
static JSValue n_child_nodes(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *n = NODE(JS_ARG(0));
    JSValue a = js_new_array(ctx, 0);
    if (!n) return a;
    for (web_node_t *c = vfirst(n); c; c = vnext(c))
        if (magic == 0 || c->type == WN_ELEM) js_array_push(ctx, a, wrap(J, c));
    return a;
}

static JSValue n_insert(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *p = NODE(JS_ARG(0)), *n = NODE(JS_ARG(1)), *ref = JS_IsNullish(JS_ARG(2)) ? NULL : NODE(JS_ARG(2));
    if (!p || !n || (!JS_IsNullish(JS_ARG(2)) && !ref)) return js_throw_error(ctx, JS_TYPE_ERROR, "Failed to execute 'insertBefore' on 'Node': parameter is not of type 'Node'.");
    if (ref == n) ref = vnext(n);
    JSValue err;
    if (!may_insert(J, p, n, ref, &err)) return err;
    insert(J, p, n, ref);
    return JS_ARG(1);
}

static JSValue n_remove(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *n = NODE(JS_ARG(0));
    if (n && n->parent) detach(J, n);
    return JS_ARG(0);
}

static JSValue n_clone(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *n = NODE(JS_ARG(0));
    if (!n) return JS_NULL;
    if (n->type == WN_DOC) return js_throw_error(ctx, JS_TYPE_ERROR, "Cloning the document is not supported");
    web_node_t *c = clone(J, n, js_truthy(JS_ARG(1)));
    return wrap(J, c);
}

static JSValue n_contains(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *a = NODE(JS_ARG(0)), *b = NODE(JS_ARG(1));
    for (; b; b = b->parent) if (b == a) return JS_TRUE;
    return JS_FALSE;
}

static JSValue n_order(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *a = NODE(JS_ARG(0)), *b = NODE(JS_ARG(1));
    if (!a || !b) return js_mknum(0);
    web_node_t *top = top_of(a);
    for (web_node_t *x = top; x; x = walk_next(x, top)) {
        if (x == a) return js_mknum(-1);
        if (x == b) return js_mknum(1);
    }
    return js_mknum(0);
}

static web_sel_t *selector(bjs_t *J, JSValue v) {
    char *s = js_utf8(J->ctx, v, NULL);
    if (!s) return NULL;
    J->selc_tick++;
    selc_t *slot = &J->selc[0];
    for (int i = 0; i < (int)ARRAY_SIZE(J->selc); i++) {
        selc_t *c = &J->selc[i];
        if (c->text && !strcmp(c->text, s)) { c->used = J->selc_tick; free(s); return c->sel; }
        if (c->used < slot->used) slot = c;
    }
    /* (selectors are 1252 like the page; non-ASCII names are rare) */
    int l;
    char *s1252 = to_1252(J->ctx, v, &l);
    web_sel_t *sel = s1252 ? web_sel_parse(s1252) : NULL;
    free(s1252);
    if (!sel) {
        js_throw_error(J->ctx, JS_SYNTAX_ERROR, "Failed to execute 'querySelector': '%.200s' is not a valid selector.", s);
        free(s);
        return NULL;
    }
    free(slot->text);
    web_sel_free(slot->sel);
    slot->text = s;
    slot->sel = sel;
    slot->used = J->selc_tick;
    return sel;
}

static bool walk_from(web_node_t *root, web_node_t **n) {
    *n = dom_next(*n, root);
    return *n != NULL;
}

static JSValue n_query(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *root = NODE(JS_ARG(0));
    bool all = js_truthy(JS_ARG(2));
    if (!root) return all ? js_new_array(ctx, 0) : JS_NULL;
    web_sel_t *sel = selector(J, JS_ARG(1));
    if (!sel) return JS_EXCEPTION;
    web_sel_focus = J->focus;
    const web_node_t *scope = root->type == WN_ELEM ? root : NULL;
    JSValue a = all ? js_new_array(ctx, 0) : JS_NULL;
    for (web_node_t *n = root; walk_from(root, &n);) {
        if (n->type != WN_ELEM || !web_sel_match(sel, n, scope)) continue;
        if (!all) { a = wrap(J, n); break; }
        js_array_push(ctx, a, wrap(J, n));
    }
    web_sel_focus = NULL;
    return a;
}

static JSValue n_matches(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *n = NODE(JS_ARG(0));
    if (!n || n->type != WN_ELEM) return magic ? JS_NULL : JS_FALSE;
    web_sel_t *sel = selector(J, JS_ARG(1));
    if (!sel) return JS_EXCEPTION;
    web_sel_focus = J->focus;
    JSValue r = magic ? JS_NULL : JS_FALSE;
    if (!magic) r = JS_BOOL(web_sel_match(sel, n, n));
    else
        for (web_node_t *x = n; x && x->type == WN_ELEM; x = x->parent)
            if (web_sel_match(sel, x, n)) { r = wrap(J, x); break; }
    web_sel_focus = NULL;
    return r;
}

/* magic 0 getElementsByTagName, 1 getElementsByClassName, 2 getElementById */
static JSValue n_by(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *root = NODE(JS_ARG(0));
    if (!root) return magic == 2 ? JS_NULL : js_new_array(ctx, 0);
    int l;
    char *key = to_1252(ctx, JS_ARG(1), &l);
    if (!key) return JS_NULL;
    if (magic == 0) for (char *p = key; *p; p++) *p = (char)tolower((unsigned char)*p);
    /* class names: split */
    char *names[32];
    int nn = 0;
    if (magic == 1)
        for (char *p = key; *p && nn < 32;) {
            while (*p && web_isspace((unsigned char)*p)) *p++ = 0;
            if (!*p) break;
            names[nn++] = p;
            while (*p && !web_isspace((unsigned char)*p)) p++;
        }
    JSValue a = magic == 2 ? JS_NULL : js_new_array(ctx, 0);
    if (magic == 1 && !nn) { free(key); return a; }
    for (web_node_t *n = root; walk_from(root, &n);) {
        if (n->type != WN_ELEM) continue;
        bool ok;
        if (magic == 0) ok = !strcmp(key, "*") || (n->name && !strcmp(n->name, key));
        else if (magic == 1) {
            ok = true;
            for (int i = 0; i < nn && ok; i++) {
                bool has = false;
                for (int k = 0; k < n->ncls && !has; k++) has = !strcmp(n->cls[k], names[i]);
                ok = has;
            }
        } else ok = n->id && !strcmp(n->id, key);
        if (!ok) continue;
        if (magic == 2) { a = wrap(J, n); break; }
        js_array_push(ctx, a, wrap(J, n));
    }
    free(key);
    return a;
}

/* kind: 1 element, 2 SVG element, 3 text, 8 comment, 11 fragment */
static JSValue n_create(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    int kind = js_to_int32(ctx, JS_ARG(0));
    web_node_t *n = NULL;
    if (kind == 1 || kind == 2) {
        int l;
        char *name = to_1252(ctx, JS_ARG(1), &l);
        if (!name) return JS_NULL;
        n = elem_new(J, name);
        free(name);
        if (n && kind == 2) n->jsf |= WNJ_SVG;
    } else if (kind == 3 || kind == 8) {
        int l;
        char *t = to_1252(ctx, JS_ARG(1), &l);
        n = text_new(J, kind == 3 ? WN_TEXT : WN_COMMENT, t, l);
    } else {
        n = node_new(J, WN_FRAG);
    }
    if (!n) return js_throw_error(ctx, JS_RANGE_ERROR, "out of memory");
    return wrap(J, n);
}

static void lower_name(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

/* magic 0 get, 1 set, 2 remove, 3 names */
static JSValue n_attr(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *n = NODE(JS_ARG(0));
    if (magic == 3) {
        JSValue a = js_new_array(ctx, 0);
        if (n && n->type == WN_ELEM) for (int i = 0; i < n->nattrs; i++) js_array_push(ctx, a, cstr_1252(ctx, n->attrs[i].name));
        return a;
    }
    if (!n || n->type != WN_ELEM) return magic == 0 ? JS_NULL : JS_UNDEFINED;
    int l;
    char *name = to_1252(ctx, JS_ARG(1), &l);
    if (!name) return JS_NULL;
    if (!in_svg(n)) lower_name(name);
    else lower_name(name);
    JSValue r = JS_UNDEFINED;
    if (magic == 0) {
        const char *v = web_attr(n, name);
        r = v ? cstr_1252(ctx, v) : JS_NULL;
    } else if (magic == 1) {
        char *v = to_1252(ctx, JS_ARG(2), &l);
        if (v) attr_set(J, n, name, v);
        free(v);
    } else {
        attr_del(J, n, name);
    }
    free(name);
    return r;
}

/* magic 0 textContent, 1 set it, 2 data, 3 set data */
static JSValue n_text(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *n = NODE(JS_ARG(0));
    if (!n) return JS_NULL;
    bool chars = n->type == WN_TEXT || n->type == WN_COMMENT;
    if (magic == 0 || magic == 2) {
        if (chars) return data_value(J, n);
        if (magic == 2) return JS_NULL;
        sb16_t s = { 0 };
        text_of(&s, n);
        return sb_value(ctx, &s);
    }
    int l;
    char *t = to_1252(ctx, JS_ARG(1), &l);
    if (!t) return JS_UNDEFINED;
    if (chars) {
        JSValue old = J->observe ? data_value(J, n) : JS_UNDEFINED;
        set_text(J, n, t, l);
        changed(J);
        if (n->parent && n->parent->type == WN_ELEM && n->parent->tag == T_STYLE && connected(J, n)) J->dirty_css = true;
        if (J->observe) mutation(J, 2, n, old, JS_UNDEFINED, NULL, NULL);
        return JS_UNDEFINED;
    }
    if (magic == 3) { free(t); return JS_UNDEFINED; }
    web_node_t *c;
    while ((c = vfirst(n))) detach(J, c);
    if (l > 0) {
        web_node_t *tn = text_new(J, WN_TEXT, t, l);
        if (tn) insert(J, n, tn, NULL);
    } else free(t);
    if (n->type == WN_ELEM && n->tag == T_STYLE && connected(J, n)) J->dirty_css = true;
    return JS_UNDEFINED;
}

static JSValue n_html(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *n = NODE(JS_ARG(0));
    if (!n) return js_new_string(ctx, "");
    sb16_t s = { 0 };
    if (js_truthy(JS_ARG(1))) ser_node(&s, n, 0);
    else ser_children(&s, n, 0);
    return sb_value(ctx, &s);
}

/* where: 0 inner, 1 outer, 2 beforebegin, 3 afterbegin, 4 beforeend, 5 afterend */
static JSValue n_set_html(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *n = NODE(JS_ARG(0));
    int where = js_to_int32(ctx, JS_ARG(2));
    if (!n) return JS_UNDEFINED;
    web_node_t *p = n->parent;
    if ((where == 1 || where == 2 || where == 5) && (!p || p->type == WN_DOC)) {
        if (where == 1 && !p) return JS_UNDEFINED;
        return throw_dom(J, "NoModificationAllowedError", "The element has no parent.");
    }
    size_t len;
    char *html = js_utf8(ctx, JS_ARG(1), &len);
    const web_node_t *context = (where == 0 || where == 3 || where == 4) ? n : p;
    web_node_t *frag = parse_frag(J, html ? html : "", html ? len : 0, context && context->type == WN_ELEM ? context : NULL);
    free(html);
    if (!frag) return js_throw_error(ctx, JS_RANGE_ERROR, "out of memory");
    switch (where) {
    case 0: {
        web_node_t *c;
        while ((c = vfirst(n))) detach(J, c);
        insert(J, n, frag, NULL);
        break;
    }
    case 1: insert(J, p, frag, n); detach(J, n); break;
    case 2: insert(J, p, frag, n); break;
    case 3: insert(J, n, frag, vfirst(n)); break;
    case 4: insert(J, n, frag, NULL); break;
    default: insert(J, p, frag, vnext(n)); break;
    }
    cand_add(J, frag);
    return JS_UNDEFINED;
}

/* a <template>'s content: a fragment parsed from the source the parser kept */
static JSValue n_template(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *n = NODE(JS_ARG(0));
    web_node_t *frag = NULL;
    if (n)
        for (web_node_t *c = n->first; c; c = c->next)
            if (c->jsf & WNJ_TPL) {
                size_t ul;
                char *u = utf8_of_1252(c->text, (size_t)c->len, &ul);
                if (u) { frag = parse_frag(J, u, ul, NULL); free(u); }
                break;
            }
    if (!frag) frag = node_new(J, WN_FRAG);
    return wrap(J, frag);
}

/* ---- node properties on the prototypes ---------------------------------------------------- */
static JSValue node_get(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    bjs_t *J = js_get_host_ud(ctx);
    web_node_t *n = NODE(this_val);
    if (!n) return magic == 9 ? JS_FALSE : JS_UNDEFINED;
    switch (magic) {
    case 0: return n->parent ? wrap(J, n->parent) : JS_NULL;
    case 1: return n->parent && n->parent->type == WN_ELEM ? wrap(J, n->parent) : JS_NULL;
    case 2: return n->type == WN_TEXT ? JS_NULL : wrap(J, vfirst(n));
    case 3: return n->type == WN_TEXT ? JS_NULL : wrap(J, vlast(n));
    case 4: return n->parent ? wrap(J, vprev(n)) : JS_NULL;
    case 5: return n->parent ? wrap(J, vnext(n)) : JS_NULL;
    case 6: return js_mknum(n->type == WN_ELEM ? 1 : n->type == WN_TEXT ? 3 : n->type == WN_COMMENT ? 8 : n->type == WN_DOC ? 9 : 11);
    case 7: case 10: {
        if (n->type == WN_TEXT) return js_new_string(ctx, "#text");
        if (n->type == WN_COMMENT) return js_new_string(ctx, "#comment");
        if (n->type == WN_DOC) return js_new_string(ctx, "#document");
        if (n->type == WN_FRAG) return js_new_string(ctx, "#document-fragment");
        char buf[128];
        strlcpy(buf, n->name ? n->name : "", sizeof buf);
        if (!in_svg(n)) for (char *p = buf; *p; p++) *p = (char)toupper((unsigned char)*p);
        return cstr_1252(ctx, buf);
    }
    case 8: return n->type == WN_DOC ? JS_NULL : J->docobj;
    case 9: return JS_BOOL(connected(J, n));
    case 11: return n->type == WN_ELEM ? cstr_1252(ctx, n->name ? n->name : "") : JS_NULL;
    case 12: { web_node_t *x = n->parent ? vprev(n) : NULL; while (x && x->type != WN_ELEM) x = vprev(x); return wrap(J, x); }
    case 13: { web_node_t *x = n->parent ? vnext(n) : NULL; while (x && x->type != WN_ELEM) x = vnext(x); return wrap(J, x); }
    }
    return JS_UNDEFINED;
}

/* ==== form controls ========================================================================= */
static bool is_text_ctl(const web_ctrl_t *c) { return c->type == C_TEXT || c->type == C_PASSWORD || c->type == C_TEXTAREA; }

static web_ctrl_t *ctl_of(tab_t *t, const web_node_t *n) {
    if (!n || n->ctl < 0 || n->ctl >= t->pg.nctrls || t->pg.ctrls[n->ctl].node != n) return NULL;
    return &t->pg.ctrls[n->ctl];
}

static void caret_on(tab_t *t) { t->b->caret_on = true; t->b->caret_t = sys_uptime_ms(); }

/* caret from a point (document coordinates) */
static void ctl_caret_at(web_ctrl_t *c, int dx, int dy) {
    if (c->type == C_TEXTAREA) {
        const font_t *f = c->font ? c->font : FONT_MONO;
        int line = (dy - c->y - 4) / MAX(1, font_height(f));
        const char *p = c->value;
        for (int l = 0; l < line && strchr(p, '\n'); l++) p = strchr(p, '\n') + 1;
        int len = (int)strcspn(p, "\n");
        char tmp[1024];
        int k = MIN(len, (int)sizeof tmp - 1);
        memcpy(tmp, p, (size_t)k);
        tmp[k] = 0;
        c->caret = (int)(p - c->value) + font_hit_test(f, tmp, dx - c->x - 5);
    } else {
        const font_t *f = c->font ? c->font : FONT_UI;
        if (c->type == C_PASSWORD) {
            int cw = font_char_width(f, 0x95);
            c->caret = cw ? (dx - c->x - 5 + c->scroll + cw / 2) / cw : c->vlen;
        } else c->caret = font_hit_test(f, c->value, dx - c->x - 5 + c->scroll);
    }
    c->caret = CLAMP(c->caret, 0, c->vlen);
}

static bool ctl_insert(web_ctrl_t *c, const char *s, int n) {
    if (c->readonly) return false;
    if (c->maxlen > 0 && c->vlen + n > c->maxlen) n = c->maxlen - c->vlen;
    if (n <= 0) return false;
    char *nv = malloc((size_t)c->vlen + (size_t)n + 1);
    if (!nv) return false;
    memcpy(nv, c->value, (size_t)c->caret);
    memcpy(nv + c->caret, s, (size_t)n);
    memcpy(nv + c->caret + n, c->value + c->caret, (size_t)(c->vlen - c->caret) + 1);
    int caret = c->caret + n;
    web_ctrl_set_value(c, nv, c->vlen + n);
    c->caret = caret;
    free(nv);
    return true;
}

/* editing keys in a text field: 0 not one, 1 used, 2 the value changed */
static int ctl_edit_key(web_ctrl_t *c, int key, int mods) {
    bool ctrl = mods & MOD_CTRL;
    switch (key) {
    case KEY_LEFT: if (c->caret > 0) c->caret--; return 1;
    case KEY_RIGHT: if (c->caret < c->vlen) c->caret++; return 1;
    case KEY_HOME: c->caret = 0; return 1;
    case KEY_END: c->caret = c->vlen; return 1;
    case KEY_BACKSPACE:
        if (c->caret > 0 && !c->readonly) {
            memmove(c->value + c->caret - 1, c->value + c->caret, (size_t)(c->vlen - c->caret) + 1);
            c->vlen--;
            c->caret--;
            return 2;
        }
        return 1;
    case KEY_DELETE:
        if (c->caret < c->vlen && !c->readonly) {
            memmove(c->value + c->caret, c->value + c->caret + 1, (size_t)(c->vlen - c->caret));
            c->vlen--;
            return 2;
        }
        return 1;
    case KEY_ENTER:
        if (c->type == C_TEXTAREA) return ctl_insert(c, "\n", 1) ? 2 : 1;
        return 0;
    default:
        if (ctrl && key == 'v') {
            const char *clip = clipboard_get();
            char buf[1024];
            size_t k = 0;
            for (; *clip && k < sizeof buf - 1; clip++)
                if (*clip != '\r' && (*clip != '\n' || c->type == C_TEXTAREA)) buf[k++] = *clip;
            return ctl_insert(c, buf, (int)k) ? 2 : 1;
        }
        if (ctrl && key == 'a') { c->caret = c->vlen; return 1; }
        return 0;
    }
}

static void ctl_check(tab_t *t, web_ctrl_t *c, bool on) {
    if (c->type == C_RADIO && on)
        for (int i = 0; i < t->pg.nctrls; i++) {
            web_ctrl_t *o = &t->pg.ctrls[i];
            if (o != c && o->type == C_RADIO && o->form == c->form && o->name && c->name && !strcmp(o->name, c->name)) o->checked = false;
        }
    c->checked = on;
}

static void ctl_reset(tab_t *t, web_node_t *form) {
    for (int i = 0; i < t->pg.nctrls; i++) {
        web_ctrl_t *c = &t->pg.ctrls[i];
        if (c->form != form) continue;
        if (c->type == C_CHECKBOX || c->type == C_RADIO) c->checked = c->init_checked;
        else if (c->type == C_SELECT) c->sel = c->init_sel;
        else if (is_text_ctl(c)) {
            web_ctrl_set_value(c, c->init_value ? c->init_value : "", -1);
            c->caret = c->vlen;
        }
    }
}

/* the list of a <select> */
static struct { tab_t *t; int jid; int ctl; uint32_t gen; } g_pop;

static void pop_pick(int item, void *ctx) {
    (void)ctx;
    tab_t *t = g_pop.t;
    bjs_t *J = g_pop.jid ? by_id(g_pop.jid) : NULL;
    if (!t || (g_pop.jid && (!J || J->t != t)) || t->gen != g_pop.gen) return;
    if (g_pop.ctl < 0 || g_pop.ctl >= t->pg.nctrls) return;
    web_ctrl_t *c = &t->pg.ctrls[g_pop.ctl];
    if (c->type != C_SELECT || item < 0 || item >= c->nopts) return;
    bool ch = c->sel != item;
    c->sel = item;
    repaint(t);
    if (J && ch && c->node) {
        JSValue el = wrap(J, c->node);
        hook_now(J, "selected", 1, &el, NULL, true);
        flush(J, true);
    }
}

static void ctl_popup(tab_t *t, int idx) {
    web_ctrl_t *c = &t->pg.ctrls[idx];
    if (!c->nopts || c->w <= 0) return;
    size_t cap = 64;
    for (int i = 0; i < c->nopts; i++) cap += strlen(c->opt_text[i]) + 4;
    char *items = malloc(cap);
    if (!items) return;
    items[0] = 0;
    char ui[200];
    for (int i = 0; i < c->nopts; i++) {
        web_1252_to_ui(c->opt_text[i], ui, sizeof ui);
        for (char *p = ui; *p; p++) if (*p == '|') *p = '/';
        if (i) strcat(items, "|");
        if (!ui[0] || ui[0] == '-' || ui[0] == '!') strcat(items, " ");
        strcat(items, ui);
    }
    g_pop.t = t;
    g_pop.jid = t->js ? t->js->id : 0;
    g_pop.ctl = idx;
    g_pop.gen = t->gen;
    rect_t v = browser_view(t->b);
    ui_popup_menu(t->b->win, v.x + c->x - t->sx, v.y + c->y - t->sy + c->h, items, pop_pick, NULL);
    free(items);
}

/* what a control does when clicked (pages without scripts) */
static void ctl_activate(tab_t *t, int idx) {
    web_ctrl_t *c = &t->pg.ctrls[idx];
    if (c->disabled) return;
    switch (c->type) {
    case C_CHECKBOX: ctl_check(t, c, !c->checked); break;
    case C_RADIO: ctl_check(t, c, true); break;
    case C_SELECT: ctl_popup(t, idx); break;
    case C_SUBMIT: case C_IMAGE: tab_submit(t, idx); return;
    case C_RESET: ctl_reset(t, c->form); break;
    default: break;
    }
    repaint(t);
}

static web_node_t *form_of(bjs_t *J, web_node_t *n) {
    if (!n) return NULL;
    if (n->form && !(n->form->jsf & WNJ_DEAD) && connected(J, n->form)) return n->form;
    for (web_node_t *a = n->parent; a; a = a->parent) if (a->type == WN_ELEM && a->tag == T_FORM) return a;
    return NULL;
}

static bool listed_ctl(const web_node_t *n) {
    return n->type == WN_ELEM && (n->tag == T_INPUT || n->tag == T_BUTTON || n->tag == T_SELECT || n->tag == T_TEXTAREA ||
                                  n->tag == T_FIELDSET || n->tag == T_OUTPUT || n->tag == T_OBJECT);
}

static void set_focus(bjs_t *J, web_node_t *n) {
    tab_t *t = J->t;
    J->focus = n;
    js_root_set(J->ctx, J->focus_root, n ? wrap(J, n) : JS_NULL);
    web_ctrl_t *c = ctl_of(t, n);
    t->focus_ctrl = c ? (int)(c - t->pg.ctrls) : -1;
    if (c) caret_on(t);
    J->repaint = true;
}

static JSValue n_ctl(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    tab_t *t = J->t;
    web_node_t *n = NODE(JS_ARG(0));
    int op = js_to_int32(ctx, JS_ARG(1));
    if (!n || n->type != WN_ELEM) return JS_UNDEFINED;
    web_ctrl_t *c = ctl_of(t, n);
    switch (op) {
    case 0: {                                       /* value */
        if (c) {
            if (c->type == C_SELECT) return c->sel >= 0 && c->sel < c->nopts ? cstr_1252(ctx, c->opt_val[c->sel]) : js_new_string(ctx, "");
            return cstr_1252(ctx, c->value ? c->value : "");
        }
        if (n->tag == T_TEXTAREA) { sb16_t s = { 0 }; text_of(&s, n); return sb_value(ctx, &s); }
        if (n->tag == T_SELECT) {
            JSValue first = JS_UNDEFINED;
            for (web_node_t *o = n; walk_from(n, &o);)
                if (o->type == WN_ELEM && o->tag == T_OPTION) {
                    const char *v = web_attr(o, "value");
                    JSValue val;
                    if (v) val = cstr_1252(ctx, v);
                    else { sb16_t s = { 0 }; text_of(&s, o); val = sb_value(ctx, &s); }
                    if (web_has_attr(o, "selected")) return val;
                    if (JS_IsUndefined(first)) first = val;
                }
            return JS_IsUndefined(first) ? js_new_string(ctx, "") : first;
        }
        const char *v = web_attr(n, "value");
        if (!v && n->tag == T_INPUT) {
            const char *ty = web_attr(n, "type");
            if (ty && (!strcasecmp(ty, "checkbox") || !strcasecmp(ty, "radio"))) v = "on";
        }
        return cstr_1252(ctx, v ? v : "");
    }
    case 1: {
        int l;
        char *v = to_1252(ctx, JS_ARG(2), &l);
        if (!v) return JS_UNDEFINED;
        if (c && c->type == C_SELECT) {
            c->sel = -1;
            for (int i = 0; i < c->nopts; i++) if (!strcmp(c->opt_val[i], v)) { c->sel = i; break; }
            if (c->sel < 0) c->sel = 0;
            J->repaint = true;
        } else if (c && c->type != C_SELECT) {
            web_ctrl_set_value(c, v, l);
            c->caret = c->vlen;
            J->repaint = true;
        } else if (n->tag == T_TEXTAREA) {
            web_node_t *x;
            while ((x = vfirst(n))) detach(J, x);
            web_node_t *tn = text_new(J, WN_TEXT, v, l);
            if (tn) insert(J, n, tn, NULL);
            return JS_UNDEFINED;
        } else if (n->tag == T_SELECT) {
            for (web_node_t *o = n; walk_from(n, &o);)
                if (o->type == WN_ELEM && o->tag == T_OPTION) {
                    const char *ov = web_attr(o, "value");
                    if (ov && !strcmp(ov, v)) attr_set(J, o, "selected", ""); else if (web_has_attr(o, "selected")) attr_del(J, o, "selected");
                }
        } else {
            attr_set(J, n, "value", v);
        }
        free(v);
        return JS_UNDEFINED;
    }
    case 2: return JS_BOOL(c ? c->checked : web_has_attr(n, "checked"));
    case 3: {
        bool on = js_truthy(JS_ARG(2));
        if (c) { ctl_check(t, c, on); J->repaint = true; }
        else if (on) attr_set(J, n, "checked", ""); else attr_del(J, n, "checked");
        return JS_UNDEFINED;
    }
    case 4: {
        if (c) return js_mknum(c->nopts ? c->sel : -1);
        int i = 0, sel = -1, any = 0;
        for (web_node_t *o = n; walk_from(n, &o);)
            if (o->type == WN_ELEM && o->tag == T_OPTION) { if (web_has_attr(o, "selected") && sel < 0) sel = i; i++; any = 1; }
        return js_mknum(sel >= 0 ? sel : any ? 0 : -1);
    }
    case 5: {
        int i = js_to_int32(ctx, JS_ARG(2));
        if (c) { c->sel = i >= 0 && i < c->nopts ? i : -1; if (c->sel < 0 && c->nopts) c->sel = 0; J->repaint = true; return JS_UNDEFINED; }
        int k = 0;
        for (web_node_t *o = n; walk_from(n, &o);)
            if (o->type == WN_ELEM && o->tag == T_OPTION) { if (k == i) attr_set(J, o, "selected", ""); else if (web_has_attr(o, "selected")) attr_del(J, o, "selected"); k++; }
        return JS_UNDEFINED;
    }
    case 6: set_focus(J, n); return JS_UNDEFINED;
    case 7: if (J->focus == n) set_focus(J, NULL); return JS_UNDEFINED;
    case 9: {                                       /* submit the form (no events) */
        if (J->submit_root >= 0) return JS_UNDEFINED;
        J->submit_root = js_root_add(ctx, JS_ARG(0));
        web_node_t *s = NODE(JS_ARG(2));
        J->submit_ctl = s ? js_root_add(ctx, JS_ARG(2)) : -1;
        return JS_UNDEFINED;
    }
    case 10: ctl_reset(t, n); J->repaint = true; return JS_UNDEFINED;
    case 11: return js_mknum(c ? c->caret : 0);
    case 12: if (c) { int i = js_to_int32(ctx, JS_ARG(2)); c->caret = i < 0 ? c->vlen : MIN(i, c->vlen); J->repaint = true; } return JS_UNDEFINED;
    case 20: { web_node_t *f = listed_ctl(n) ? form_of(J, n) : NULL; return f ? wrap(J, f) : JS_NULL; }
    case 21: {                                      /* a form's controls */
        JSValue a = js_new_array(ctx, 0);
        web_node_t *top = top_of(n);
        for (web_node_t *x = top; x; x = dom_next(x, top))
            if (listed_ctl(x) && x->tag != T_FIELDSET && x->tag != T_OBJECT && x->tag != T_OUTPUT && form_of(J, x) == n) js_array_push(ctx, a, wrap(J, x));
        return a;
    }
    case 22: return JS_BOOL((n->jsf & WNJ_HEAP) && !(n->jsf & WNJ_SYNC));
    case 23: if (js_truthy(JS_ARG(2))) n->jsf &= (uint16_t)~WNJ_SYNC; else n->jsf |= WNJ_SYNC; return JS_UNDEFINED;
    case 24: n->jsf &= (uint16_t)~WNJ_STARTED; return JS_UNDEFINED;
    case 25:                                        /* caret from a point (viewport) */
        if (c && is_text_ctl(c)) { ctl_caret_at(c, js_to_int32(ctx, JS_ARG(2)) + t->sx, js_to_int32(ctx, JS_ARG(3)) + t->sy); J->repaint = true; caret_on(t); }
        return JS_UNDEFINED;
    case 26: {                                      /* type text at the caret */
        if (!c || !is_text_ctl(c) || c->disabled) return JS_FALSE;
        int l;
        char *s = to_1252(ctx, JS_ARG(2), &l);
        bool ok = s && ctl_insert(c, s, l);
        free(s);
        if (ok) { J->repaint = true; caret_on(t); }
        return JS_BOOL(ok);
    }
    case 30: if (c && c->type == C_SELECT && !c->disabled) ctl_popup(t, (int)(c - t->pg.ctrls)); return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

static JSValue n_key(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *n = NODE(JS_ARG(0));
    web_ctrl_t *c = ctl_of(J->t, n);
    if (!c || !is_text_ctl(c)) return js_mknum(0);
    int r = ctl_edit_key(c, js_to_int32(ctx, JS_ARG(1)), js_to_int32(ctx, JS_ARG(2)));
    if (r) { J->repaint = true; caret_on(J->t); }
    return js_mknum(r);
}

static JSValue n_active(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    if (J->focus && !connected(J, J->focus)) set_focus(J, NULL);
    return J->focus ? wrap(J, J->focus) : JS_NULL;
}

static JSValue n_in_link(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    for (web_node_t *n = NODE(JS_ARG(0)); n && n->type == WN_ELEM; n = n->parent)
        if ((n->tag == T_A || n->tag == T_AREA) && web_has_attr(n, "href")) return JS_TRUE;
    return JS_FALSE;
}

/* ==== layout queries ======================================================================== */
static JSValue n_rect(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *n = NODE(JS_ARG(0));
    if (!n || n->type != WN_ELEM || !connected(J, n)) return JS_NULL;
    flush(J, true);
    int x, y, w, h;
    if (!J->t->styled || !web_node_rect(&J->t->pg, n, &x, &y, &w, &h)) return JS_NULL;
    JSValue a = js_new_array(ctx, 0);
    js_array_push(ctx, a, js_mknum(x));
    js_array_push(ctx, a, js_mknum(y));
    js_array_push(ctx, a, js_mknum(w));
    js_array_push(ctx, a, js_mknum(h));
    return a;
}

static JSValue n_view(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    tab_t *t = J->t;
    rect_t v = browser_view(t->b);
    JSValue a = js_new_array(ctx, 0);
    int vals[6] = { t->sx, t->sy, v.w, v.h, MAX(t->pg.lay.width, v.w), MAX(t->pg.lay.height, v.h) };
    for (int i = 0; i < 6; i++) js_array_push(ctx, a, js_mknum(vals[i]));
    return a;
}

static JSValue n_scroll(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    tab_t *t = J->t;
    double x = js_to_number(ctx, JS_ARG(0), NULL), y = js_to_number(ctx, JS_ARG(1), NULL);
    if (x != x) x = 0;
    if (y != y) y = 0;
    flush(J, true);
    tab_scroll_to(t, (int)x, (int)y);
    J->repaint = true;
    return JS_UNDEFINED;
}

static JSValue n_hit(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    tab_t *t = J->t;
    flush(J, true);
    int x = js_to_int32(ctx, JS_ARG(0)), y = js_to_int32(ctx, JS_ARG(1));
    web_node_t *n = t->styled ? web_hit_node(&t->pg, x + t->sx, y + t->sy) : NULL;
    if (!n) n = J->doc->body;
    return wrap(J, n);
}

static const char *const disp_names[] = { "none", "inline", "block", "list-item", "inline-block", "table", "inline-table",
    "table-row-group", "table-header-group", "table-footer-group", "table-row", "table-cell", "table-caption",
    "table-column", "table-column-group", "flex", "inline-flex", "grid", "contents", "inline-grid" };

static void css_color(char *out, size_t size, uint32_t c) {
    unsigned a = c >> 24, r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    if (a == 255) snprintf(out, size, "rgb(%u, %u, %u)", r, g, b);
    else snprintf(out, size, "rgba(%u, %u, %u, %u.%02u)", r, g, b, a / 255, (a % 255) * 100 / 255);
}

static void css_len(char *out, size_t size, web_len_t l) {
    if (l.u == U_PX) snprintf(out, size, "%dpx", l.v);
    else if (l.u == U_PCT) snprintf(out, size, "%d.%d%%", l.v / 10, abs(l.v % 10));
    else if (l.u == U_AUTO) strlcpy(out, "auto", size);
    else strlcpy(out, "0px", size);
}

/* getComputedStyle: what the styling found for a property (a useful subset) */
static JSValue n_style(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *n = NODE(JS_ARG(0));
    char prop[64], out[96];
    char *p = js_utf8(ctx, JS_ARG(1), NULL);
    strlcpy(prop, p ? p : "", sizeof prop);
    free(p);
    out[0] = 0;
    if (!n || n->type != WN_ELEM || !connected(J, n)) return js_new_string(ctx, "");
    flush(J, true);
    const web_style_t *st = n->st;
    if (!st) return js_new_string(ctx, !strcmp(prop, "display") ? "none" : "");
    static const char *const sides[4] = { "top", "right", "bottom", "left" };
    if (!strcmp(prop, "display")) strlcpy(out, st->display < ARRAY_SIZE(disp_names) ? disp_names[st->display] : "block", sizeof out);
    else if (!strcmp(prop, "visibility")) strlcpy(out, st->visibility_hidden == 1 ? "hidden" : "visible", sizeof out);
    else if (!strcmp(prop, "color")) css_color(out, sizeof out, st->color);
    else if (!strcmp(prop, "background-color")) css_color(out, sizeof out, st->bg);
    else if (!strcmp(prop, "font-size")) snprintf(out, sizeof out, "%dpx", st->fsize);
    else if (!strcmp(prop, "font-weight")) strlcpy(out, st->bold ? "700" : "400", sizeof out);
    else if (!strcmp(prop, "font-style")) strlcpy(out, st->italic ? "italic" : "normal", sizeof out);
    else if (!strcmp(prop, "font-family")) strlcpy(out, st->mono ? "monospace" : "sans-serif", sizeof out);
    else if (!strcmp(prop, "line-height")) { if (st->lh) snprintf(out, sizeof out, "%dpx", st->lh); else strlcpy(out, "normal", sizeof out); }
    else if (!strcmp(prop, "position")) { static const char *const pos[] = { "static", "relative", "absolute", "fixed", "sticky" }; strlcpy(out, pos[st->position % 5], sizeof out); }
    else if (!strcmp(prop, "opacity")) { int o = 255 - st->fade; snprintf(out, sizeof out, o == 255 ? "1" : "0.%02d", o * 100 / 255); }
    else if (!strcmp(prop, "text-align")) { static const char *const al[] = { "left", "right", "center", "justify" }; strlcpy(out, al[st->align % 4], sizeof out); }
    else if (!strcmp(prop, "z-index")) { if (st->z_set) snprintf(out, sizeof out, "%d", st->z); else strlcpy(out, "auto", sizeof out); }
    else if (!strcmp(prop, "overflow") || !strcmp(prop, "overflow-x") || !strcmp(prop, "overflow-y")) strlcpy(out, st->ovf_scroll ? "auto" : st->overflow_hidden ? "hidden" : "visible", sizeof out);
    else if (!strcmp(prop, "white-space")) { static const char *const ws[] = { "normal", "pre", "nowrap", "pre-wrap", "pre-line" }; strlcpy(out, ws[st->ws % 5], sizeof out); }
    else if (!strcmp(prop, "pointer-events")) strlcpy(out, st->no_pointer ? "none" : "auto", sizeof out);
    else if (!strcmp(prop, "float")) strlcpy(out, st->floating == 1 ? "left" : st->floating == 2 ? "right" : "none", sizeof out);
    else if (!strcmp(prop, "box-sizing")) strlcpy(out, st->border_box ? "border-box" : "content-box", sizeof out);
    else if (!strcmp(prop, "text-decoration") || !strcmp(prop, "text-decoration-line")) strlcpy(out, (st->deco & DECO_UNDERLINE) ? "underline" : (st->deco & DECO_STRIKE) ? "line-through" : "none", sizeof out);
    else if (!strcmp(prop, "transform") || !strcmp(prop, "content") || !strcmp(prop, "animation-name") || !strcmp(prop, "transition-property")) strlcpy(out, "none", sizeof out);
    else if (!strcmp(prop, "transition-duration") || !strcmp(prop, "animation-duration")) strlcpy(out, "0s", sizeof out);
    else if (!strcmp(prop, "cursor")) strlcpy(out, "auto", sizeof out);
    else if (!strcmp(prop, "width") || !strcmp(prop, "height")) {
        int x, y, w, h;
        if (J->t->styled && web_node_rect(&J->t->pg, n, &x, &y, &w, &h)) snprintf(out, sizeof out, "%dpx", prop[0] == 'w' ? w : h);
        else strlcpy(out, "auto", sizeof out);
    } else {
        for (int i = 0; i < 4; i++) {
            char nm[40];
            snprintf(nm, sizeof nm, "margin-%s", sides[i]);
            if (!strcmp(prop, nm)) { css_len(out, sizeof out, st->margin[i]); break; }
            snprintf(nm, sizeof nm, "padding-%s", sides[i]);
            if (!strcmp(prop, nm)) { css_len(out, sizeof out, st->padding[i]); break; }
            snprintf(nm, sizeof nm, "border-%s-width", sides[i]);
            if (!strcmp(prop, nm)) { snprintf(out, sizeof out, "%dpx", st->bw[i]); break; }
            snprintf(nm, sizeof nm, "border-%s-color", sides[i]);
            if (!strcmp(prop, nm)) { css_color(out, sizeof out, st->bc[i]); break; }
        }
        if (!out[0] && (!strcmp(prop, "top") || !strcmp(prop, "left") || !strcmp(prop, "right") || !strcmp(prop, "bottom"))) {
            web_len_t l = prop[0] == 't' ? st->top : prop[0] == 'l' ? st->left : prop[0] == 'r' ? st->right : st->bottom;
            css_len(out, sizeof out, l);
        }
        if (!out[0] && !strncmp(prop, "--", 2))
            for (const web_var_t *v = st->vars; v; v = v->next) if (v->name && !strcmp(v->name, prop)) { strlcpy(out, v->value ? v->value : "", sizeof out); break; }
    }
    return cstr_1252(ctx, out);
}

static JSValue n_img(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    web_node_t *n = NODE(JS_ARG(0));
    tab_t *t = J->t;
    int w = 0, h = 0, done = 0;
    if (n && n->img >= 0 && n->img < t->pg.nimgs) {
        web_image_t *im = &t->pg.imgs[n->img];
        if (im->surf) { w = im->surf->w; h = im->surf->h; }
        done = im->state >= IMG_OK;
    } else if (n && !web_attr(n, "src")) done = 1;
    JSValue a = js_new_array(ctx, 0);
    js_array_push(ctx, a, js_mknum(w));
    js_array_push(ctx, a, js_mknum(h));
    js_array_push(ctx, a, js_mknum(done));
    return a;
}

static JSValue n_media(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    char *q = js_utf8(ctx, JS_ARG(0), NULL);
    rect_t v = browser_view(J->t->b);
    bool m = magic ? true : web_css_media(q ? q : "", v.w, v.h);
    free(q);
    return JS_BOOL(m);
}

/* ==== window natives ======================================================================= */
static void host_log(void *ud, int level, const char *msg) {
    bjs_t *J = ud;
    uint32_t sec = sys_uptime_ms() / 1000;
    if (J) {
        if (sec != J->log_sec) { J->log_sec = sec; J->nlog = 0; }
        if (++J->nlog > 60) { if (J->nlog == 61) klog("js: (more console lines this second are not logged)"); return; }
    }
    const char *pre = level == JS_LOG_ERROR ? "error: " : level == JS_LOG_WARN ? "warning: " : "";
    int lines = 0;
    while (msg && *msg && lines < 8) {
        size_t n = strcspn(msg, "\n");
        klog("js: %s%.*s", lines ? "  " : pre, (int)MIN(n, (size_t)300), msg);
        msg += n;
        if (*msg == '\n') msg++;
        lines++;
    }
}

static double host_wall(void *ud) {
    bjs_t *J = ud;
    return J ? J->wall0 + (double)sys_uptime_ms() : (double)sys_time() * 1000.0;
}

static double host_mono(void *ud) { (void)ud; return (double)sys_uptime_ms(); }

static JSValue n_log(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    char *s = js_utf8(ctx, JS_ARG(1), NULL);
    int lv = js_to_int32(ctx, JS_ARG(0));
    host_log(J, lv == 3 ? JS_LOG_ERROR : lv == 2 ? JS_LOG_WARN : JS_LOG_LOG, s ? s : "");
    free(s);
    return JS_UNDEFINED;
}

static JSValue n_now(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    return js_mknum((double)(sys_uptime_ms() - J->start_ms));
}

/* ---- timers ---- */
static int jtimer_add(bjs_t *J, JSValue fn, uint32_t ms, bool repeat, bool raf, JSValue args) {
    if (J->ntm >= J->tmcap) {
        int nc = J->tmcap ? J->tmcap * 2 : 32;
        if (nc > 100000) return 0;
        jtimer_t *x = realloc(J->tm, sizeof *x * (size_t)nc);
        if (!x) return 0;
        J->tm = x;
        J->tmcap = nc;
    }
    jtimer_t *tm = &J->tm[J->ntm++];
    memset(tm, 0, sizeof *tm);
    tm->id = ++J->next_id;
    tm->due = sys_uptime_ms() + ms;
    tm->every = repeat ? MAX(ms, 4u) : 0;
    tm->repeat = repeat;
    tm->raf = raf;
    tm->fn = js_root_add(J->ctx, fn);
    tm->args = js_root_add(J->ctx, args);
    return tm->id;
}

static JSValue n_timer(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    if (!js_is_function(ctx, JS_ARG(0))) return js_mknum(0);
    if (magic) return js_mknum(jtimer_add(J, JS_ARG(0), 16, false, true, JS_UNDEFINED));
    double ms = js_to_number(ctx, JS_ARG(1), NULL);
    if (!(ms >= 0)) ms = 0;
    if (ms > 2147483647.0) ms = 2147483647.0;
    return js_mknum(jtimer_add(J, JS_ARG(0), (uint32_t)ms, js_truthy(JS_ARG(2)), false, JS_ARG(3)));
}

static void timer_drop(bjs_t *J, int i) {
    js_root_free(J->ctx, J->tm[i].fn);
    js_root_free(J->ctx, J->tm[i].args);
    J->tm[i] = J->tm[--J->ntm];
}

static JSValue n_clear(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    int id = js_to_int32(ctx, JS_ARG(0));
    for (int i = 0; i < J->ntm; i++) if (J->tm[i].id == id) { timer_drop(J, i); break; }
    return JS_UNDEFINED;
}

/* queue the timers that are due, earliest first */
static void timers_due(bjs_t *J) {
    uint32_t now = sys_uptime_ms();
    for (int guard = 0; guard < 64; guard++) {
        int best = -1;
        for (int i = 0; i < J->ntm; i++) {
            jtimer_t *tm = &J->tm[i];
            if ((int32_t)(now - tm->due) < 0) continue;
            if (best < 0 || (int32_t)(tm->due - J->tm[best].due) < 0 || (tm->due == J->tm[best].due && tm->id < J->tm[best].id)) best = i;
        }
        if (best < 0) return;
        jtimer_t *tm = &J->tm[best];
        JSValue fn = js_root_get(J->ctx, tm->fn);
        JSValue argv[8];
        int argc = 0;
        if (tm->raf) argv[argc++] = js_mknum((double)now);
        else {
            JSValue a = js_root_get(J->ctx, tm->args);
            if (js_is_array(J->ctx, a)) {
                uint32_t n = js_array_length(J->ctx, a);
                for (uint32_t i = 0; i < n && argc < 8; i++) argv[argc++] = js_get_index(J->ctx, a, i);
            }
        }
        q_call(J, fn, js_global(J->ctx), argc, argv, false);
        if (tm->repeat) {
            tm->due += tm->every;
            if ((int32_t)(now - tm->due) > 0) tm->due = now + tm->every;
        } else timer_drop(J, best);
    }
}

/* ---- navigation, history ---- */
static bool same_doc(const char *a, const char *b) {
    size_t la = strcspn(a, "#"), lb = strcspn(b, "#");
    return la == lb && !strncmp(a, b, la);
}

static JSValue n_href(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    return js_new_string(ctx, J->t->url);
}

static void fire_hashchange(bjs_t *J, const char *old_url, const char *new_url) {
    JSValue args[7] = { js_global(J->ctx), js_mknum(8), js_new_string(J->ctx, "hashchange"), JS_FALSE, JS_FALSE,
                        js_new_string(J->ctx, old_url), js_new_string(J->ctx, new_url) };
    q_call(J, js_get(J->ctx, J->rt, "event"), JS_UNDEFINED, 7, args, false);
}

/* mode 0 assign, 1 replace, 2 reload, 3 the #fragment only */
static JSValue n_nav(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    tab_t *t = J->t;
    char *url = js_utf8(ctx, JS_ARG(0), NULL);
    int mode = js_to_int32(ctx, JS_ARG(1));
    if (!url) return JS_UNDEFINED;
    if (!strncasecmp(url, "javascript:", 11)) { free(url); return JS_UNDEFINED; }
    if (mode != 2 && same_doc(url, t->url) && strchr(url, '#')) {
        /* the same page, another #fragment: no reload */
        char old[B_URL_MAX];
        strlcpy(old, t->url, sizeof old);
        if (strcmp(url, t->url)) {
            tab_push_url(t, url, mode == 1);
            flush(J, true);
            int y = web_anchor_y(&t->pg, strchr(url, '#') + 1);
            if (y >= 0) { tab_scroll_to(t, t->sx, y); J->repaint = true; }
            fire_hashchange(J, old, url);
        }
        free(url);
        return JS_UNDEFINED;
    }
    free(J->nav_url);
    J->nav_url = url;
    J->nav_mode = mode;
    return JS_UNDEFINED;
}

/* op 0 length, 1 go(delta), 2 pushState(url), 3 replaceState(url), 4 position */
static JSValue n_hist(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    tab_t *t = J->t;
    int op = js_to_int32(ctx, JS_ARG(0));
    switch (op) {
    case 0: return js_mknum(MAX(t->nhist, 1));
    case 1: J->hist_go = js_to_int32(ctx, JS_ARG(1)); break;
    case 2: case 3: {
        char *url = js_utf8(ctx, JS_ARG(1), NULL);
        if (url && strlen(url) < B_URL_MAX) tab_push_url(t, url, op == 3);
        free(url);
        break;
    }
    case 4: return js_mknum(t->hpos);
    }
    return JS_UNDEFINED;
}

static JSValue n_open(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    if (!J->cur_user) { host_log(J, JS_LOG_WARN, "window.open: a pop-up without a click was blocked"); return JS_UNDEFINED; }
    char *url = js_utf8(ctx, JS_ARG(0), NULL);
    if (url && (!strncasecmp(url, "http", 4) || !strncasecmp(url, "file:", 5))) { free(J->open_url); J->open_url = url; }
    else free(url);
    return JS_UNDEFINED;
}

static JSValue n_follow(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    if (J->follow_root < 0 && NODE(JS_ARG(0))) J->follow_root = js_root_add(ctx, JS_ARG(0));
    return JS_UNDEFINED;
}

/* ---- document bits ---- */
static JSValue n_title(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    tab_t *t = J->t;
    web_doc_t *d = J->doc;
    if (argc < 1) {
        web_node_t *ti = NULL;
        for (web_node_t *n = d->root; n && !ti; n = walk_next(n, d->root)) if (n->type == WN_ELEM && n->tag == T_TITLE) ti = n;
        if (ti) { char buf[256]; web_text_content(ti, buf, sizeof buf); return cstr_1252(ctx, buf); }
        return cstr_1252(ctx, d->title);
    }
    int l;
    char *s = to_1252(ctx, JS_ARG(0), &l);
    if (!s) return JS_UNDEFINED;
    strlcpy(d->title, s, sizeof d->title);
    web_node_t *ti = NULL;
    for (web_node_t *n = d->root; n && !ti; n = walk_next(n, d->root)) if (n->type == WN_ELEM && n->tag == T_TITLE) ti = n;
    if (!ti && d->head) { ti = elem_new(J, "title"); if (ti) insert(J, d->head, ti, NULL); }
    if (ti) {
        web_node_t *c;
        while ((c = vfirst(ti))) detach(J, c);
        web_node_t *tn = text_new(J, WN_TEXT, s, l);
        if (tn) insert(J, ti, tn, NULL);
        s = NULL;
    }
    free(s);
    web_1252_to_ui(d->title, t->title, sizeof t->title);
    browser_tab_changed(t);
    return JS_UNDEFINED;
}

static JSValue n_cookie(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    url_t u;
    if (!url_parse(J->t->url, &u)) return argc ? JS_UNDEFINED : js_new_string(ctx, "");
    if (argc < 1) {
        char *h = cookies_header_for(&u);
        JSValue v = js_new_string(ctx, h ? h : "");
        free(h);
        return v;
    }
    char *s = js_utf8(ctx, JS_ARG(0), NULL);
    if (s && !strcasestr(s, "httponly") && strlen(s) < 8192) cookies_store(&u, s);
    free(s);
    return JS_UNDEFINED;
}

static JSValue n_info(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    switch (js_to_int32(ctx, JS_ARG(0))) {
    case 0: return js_new_string(ctx, J->ready == 0 ? "loading" : J->ready == 1 ? "interactive" : "complete");
    case 1: return js_new_string(ctx, J->doc->quirks ? "BackCompat" : "CSS1Compat");
    case 2: return J->running ? js_root_get(ctx, J->running->el) : JS_NULL;
    default: return js_new_string(ctx, "");
    }
}

static JSValue n_listen(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    if (magic) return JS_BOOL(J->listen & (1u << js_to_int32(ctx, JS_ARG(0))));
    char *s = js_utf8(ctx, JS_ARG(0), NULL);
    if (s) {
        if (!strncmp(s, "mouse", 5) && strcmp(s, "mousedown") && strcmp(s, "mouseup")) J->listen |= 1;
        if (!strncmp(s, "pointer", 7)) J->listen |= 2 | (strcmp(s, "pointerdown") && strcmp(s, "pointerup") ? 1 : 0);
        if (!strcmp(s, "wheel") || !strcmp(s, "mousewheel")) J->listen |= 4;
        free(s);
    }
    return JS_UNDEFINED;
}

static JSValue n_observe(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    J->observe = js_truthy(JS_ARG(0));
    return JS_UNDEFINED;
}

static JSValue n_version(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    return js_mknum(J->version);
}

static JSValue n_random(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    int n = js_to_int32(ctx, JS_ARG(0));
    uint8_t buf[256];
    if (n < 0) n = 0;
    if (n > 65536) n = 65536;
    uint8_t *b = n <= (int)sizeof buf ? buf : malloc((size_t)n);
    if (!b) return js_new_uint8array(ctx, buf, 0);
    random_bytes(b, (size_t)n);
    JSValue v = js_new_uint8array(ctx, b, (size_t)n);
    if (b != buf) free(b);
    return v;
}

static JSValue n_clip(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    if (argc < 1) return cstr_1252(ctx, clipboard_get());
    int l;
    char *s = to_1252(ctx, JS_ARG(0), &l);
    if (s && J->cur_user) clipboard_set(s);
    free(s);
    return JS_UNDEFINED;
}

static JSValue n_status(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    int l;
    char *s = to_1252(ctx, JS_ARG(0), &l);
    if (s) {
        web_1252_to_ui(s, J->t->b->status, sizeof J->t->b->status);
        browser_tab_changed(J->t);
        free(s);
    }
    return JS_UNDEFINED;
}

static void script_write(bjs_t *J, web_node_t *frag);

static JSValue n_write(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    if (!J->running || !J->running->parser || J->ready) {
        host_log(J, JS_LOG_WARN, "document.write after the page loaded is ignored");
        return JS_UNDEFINED;
    }
    size_t len;
    char *html = js_utf8(ctx, JS_ARG(0), &len);
    web_node_t *frag = parse_frag(J, html ? html : "", html ? len : 0, NULL);
    free(html);
    if (frag) script_write(J, frag);
    return JS_UNDEFINED;
}

/* ---- storage ----------------------------------------------------------------------------- */
static void origin_of(const char *url, char *out, size_t size) {
    url_t u;
    if (url_parse(url, &u)) snprintf(out, size, "%s://%s:%u", u.scheme, u.host, (unsigned)u.port);
    else if (!strncasecmp(url, "file:", 5)) strlcpy(out, "file://", size);
    else strlcpy(out, "null", size);
}

static void store_file(const jstore_t *s, char *out, size_t size) {
    char name[160];
    size_t k = 0;
    for (const char *p = s->origin; *p && k < sizeof name - 1; p++) name[k++] = isalnum((unsigned char)*p) || *p == '.' || *p == '-' ? *p : '_';
    name[k] = 0;
    snprintf(out, size, "%s/%s", STORE_DIR, name);
}

static void store_put(jstore_t *s, const char *k, const char *v) {
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->it[i].k, k)) {
            s->bytes -= strlen(s->it[i].v);
            free(s->it[i].v);
            s->it[i].v = dupstr(v);
            s->bytes += strlen(v);
            return;
        }
    if (s->n >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 16;
        jitem_t *x = realloc(s->it, sizeof *x * (size_t)nc);
        if (!x) return;
        s->it = x;
        s->cap = nc;
    }
    s->it[s->n].k = dupstr(k);
    s->it[s->n].v = dupstr(v);
    if (!s->it[s->n].k || !s->it[s->n].v) { free(s->it[s->n].k); free(s->it[s->n].v); return; }
    s->bytes += strlen(k) + strlen(v);
    s->n++;
}

/* "key<TAB>value" lines; \\, \t and \n escaped */
static void unescape(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) { s++; *o++ = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s == 'r' ? '\r' : *s; continue; }
        *o++ = *s;
    }
    *o = 0;
}

static void store_load(jstore_t *s) {
    char path[256];
    store_file(s, path, sizeof path);
    size_t sz;
    char *text = vfs_read_file(path, &sz);
    if (!text) return;
    char *save = NULL;
    for (char *ln = strtok_r(text, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(ln, '\t');
        if (!tab) continue;
        *tab = 0;
        unescape(ln);
        unescape(tab + 1);
        store_put(s, ln, tab + 1);
    }
    free(text);
}

static void esc_add(char **b, size_t *n, size_t *cap, const char *s) {
    size_t need = strlen(s) * 2 + 4;
    if (*n + need > *cap) {
        size_t nc = (*cap ? *cap * 2 : 4096) + need;
        char *x = realloc(*b, nc);
        if (!x) return;
        *b = x;
        *cap = nc;
    }
    for (; *s; s++) {
        char c = *s;
        if (c == '\\' || c == '\t' || c == '\n' || c == '\r') { (*b)[(*n)++] = '\\'; c = c == '\t' ? 't' : c == '\n' ? 'n' : c == '\r' ? 'r' : '\\'; }
        (*b)[(*n)++] = c;
    }
}

static void store_save(jstore_t *s) {
    s->dirty = false;
    char path[256];
    store_file(s, path, sizeof path);
    if (!s->n) { if (vfs_exists(path)) vfs_remove(path); return; }
    char *b = NULL;
    size_t n = 0, cap = 0;
    for (int i = 0; i < s->n; i++) {
        esc_add(&b, &n, &cap, s->it[i].k);
        esc_add(&b, &n, &cap, "");          /* (makes room) */
        if (b) b[n++] = '\t';
        esc_add(&b, &n, &cap, s->it[i].v);
        esc_add(&b, &n, &cap, "");
        if (b) b[n++] = '\n';
    }
    if (!vfs_is_dir(STORE_DIR)) vfs_mkdirs(STORE_DIR);
    if (b) vfs_write_file(path, b, n);
    free(b);
}

static void stores_save(bool now) {
    uint32_t t = sys_uptime_ms();
    if (!now && t - g_store_saved < 1000) return;
    g_store_saved = t;
    for (jstore_t *s = g_stores; s; s = s->next) if (s->dirty && !s->tab) store_save(s);
}

static jstore_t *store_for(bjs_t *J, bool session) {
    char origin[300];
    origin_of(J->t->url, origin, sizeof origin);
    tab_t *tab = session ? J->t : NULL;
    for (jstore_t *s = g_stores; s; s = s->next) if (s->tab == tab && !strcmp(s->origin, origin)) return s;
    jstore_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    strlcpy(s->origin, origin, sizeof s->origin);
    s->tab = tab;
    if (!session) store_load(s);
    s->next = g_stores;
    g_stores = s;
    return s;
}

static bool store_method(const char *k) {
    static const char *const m[] = { "getItem", "setItem", "removeItem", "clear", "key", "length" };
    for (int i = 0; i < 6; i++) if (!strcmp(k, m[i])) return true;
    return false;
}

static int store_hook_get(JSContext *ctx, JSValue obj, JSValue key, JSValue *v) {
    bjs_t *J = js_get_host_ud(ctx);
    jstore_t *s = js_host_opaque(obj, J->cls_store);
    if (!s || !JS_IsString(key)) return 0;
    char *k = js_utf8(ctx, key, NULL);
    int r = 0;
    if (k && !store_method(k))
        for (int i = 0; i < s->n; i++) if (!strcmp(s->it[i].k, k)) { *v = js_new_string(ctx, s->it[i].v); r = 1; break; }
    free(k);
    return r;
}

static bool store_set(jstore_t *s, const char *k, const char *v) {
    size_t old = 0;
    bool had = false;
    for (int i = 0; i < s->n; i++) if (!strcmp(s->it[i].k, k)) { old = strlen(s->it[i].v); had = true; break; }
    if (s->bytes - old + strlen(v) + (had ? 0 : strlen(k)) > STORE_MAX) return false;
    store_put(s, k, v);
    s->dirty = true;
    return true;
}

static void store_remove(jstore_t *s, const char *k) {
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->it[i].k, k)) {
            s->bytes -= strlen(s->it[i].k) + strlen(s->it[i].v);
            free(s->it[i].k);
            free(s->it[i].v);
            s->it[i] = s->it[--s->n];
            s->dirty = true;
            return;
        }
}

static int store_hook_set(JSContext *ctx, JSValue obj, JSValue key, JSValue v) {
    bjs_t *J = js_get_host_ud(ctx);
    jstore_t *s = js_host_opaque(obj, J->cls_store);
    if (!s || !JS_IsString(key)) return 0;
    char *k = js_utf8(ctx, key, NULL), *val = js_utf8(ctx, v, NULL);
    bool ok = k && val ? store_set(s, k, val) : false;
    free(k);
    free(val);
    if (!ok) { throw_dom(J, "QuotaExceededError", "Setting the value exceeded the quota."); return -1; }
    return 1;
}

static int store_hook_del(JSContext *ctx, JSValue obj, JSValue key) {
    bjs_t *J = js_get_host_ud(ctx);
    jstore_t *s = js_host_opaque(obj, J->cls_store);
    if (!s || !JS_IsString(key)) return 0;
    char *k = js_utf8(ctx, key, NULL);
    if (k) store_remove(s, k);
    free(k);
    return 1;
}

static JSValue store_hook_keys(JSContext *ctx, JSValue obj) {
    bjs_t *J = js_get_host_ud(ctx);
    jstore_t *s = js_host_opaque(obj, J->cls_store);
    JSValue a = js_new_array(ctx, 0);
    if (s) for (int i = 0; i < s->n; i++) js_array_push(ctx, a, js_new_string(ctx, s->it[i].k));
    return a;
}

static JSValue n_storage(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    jstore_t *s = store_for(J, js_to_int32(ctx, JS_ARG(0)) == 1);
    if (!s) return JS_NULL;
    return js_new_host_object(ctx, J->cls_store, JS_ARG(1), s);
}

/* op 0 getItem, 1 setItem, 2 removeItem, 3 clear, 4 key(i), 5 length */
static JSValue n_store(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    jstore_t *s = js_host_opaque(JS_ARG(0), J->cls_store);
    if (!s) return js_throw_error(ctx, JS_TYPE_ERROR, "Illegal invocation");
    int op = js_to_int32(ctx, JS_ARG(1));
    if (op == 3) { while (s->n) store_remove(s, s->it[0].k); return JS_UNDEFINED; }
    if (op == 5) return js_mknum(s->n);
    if (op == 4) { int i = js_to_int32(ctx, JS_ARG(2)); return i >= 0 && i < s->n ? js_new_string(ctx, s->it[i].k) : JS_NULL; }
    char *k = js_utf8(ctx, JS_ARG(2), NULL);
    JSValue r = JS_UNDEFINED;
    if (k) {
        if (op == 0) {
            r = JS_NULL;
            for (int i = 0; i < s->n; i++) if (!strcmp(s->it[i].k, k)) { r = js_new_string(ctx, s->it[i].v); break; }
        } else if (op == 1) {
            char *v = js_utf8(ctx, JS_ARG(3), NULL);
            r = JS_BOOL(v && store_set(s, k, v));
            free(v);
        } else store_remove(s, k);
    }
    free(k);
    return r;
}

/* ---- requests (fetch, XMLHttpRequest, scripts) ------------------------------------------- */
static void req_unlink(bjs_t *J, jreq_t *q) {
    for (jreq_t **pp = &J->reqs; *pp; pp = &(*pp)->next) if (*pp == q) { *pp = q->next; break; }
}

static void script_loaded(bjs_t *J, jscript_t *sc, const uint8_t *body, uint32_t len, bool ok);

static void req_done(http_request_t *r, void *ctx) {
    jreq_t *q = ctx;
    bjs_t *J = q->J;
    req_unlink(J, q);
    bool ok = http_error(r) == HTTP_OK;
    if (q->sc) {
        int st = http_status(r);
        script_loaded(J, q->sc, http_body(r), http_body_len(r), ok && st >= 200 && st < 300);
        http_free(r);
        free(q);
        return;
    }
    JSContext *c = J->ctx;
    JSValue args[6];
    args[0] = js_mknum(ok ? http_status(r) : 0);
    args[1] = js_new_string(c, ok ? http_reason(r) : "");
    args[2] = js_new_string(c, http_url(r) ? http_url(r) : "");
    JSValue h = js_new_array(c, 0);
    if (ok) {
        int n = http_header_count(r);
        for (int i = 0; i < n; i++) {
            const char *k, *v;
            if (!http_header_at(r, i, &k, &v)) continue;
            js_array_push(c, h, js_new_string(c, k));
            js_array_push(c, h, js_new_string(c, v));
        }
    }
    args[3] = h;
    args[4] = js_new_uint8array(c, ok && http_body(r) ? http_body(r) : (const uint8_t *)"", ok ? http_body_len(r) : 0);
    args[5] = ok ? js_new_string(c, "") : js_new_string(c, http_error_text(r));
    q_call(J, js_root_get(c, q->cb), JS_UNDEFINED, 6, args, false);
    js_root_free(c, q->cb);
    http_free(r);
    free(q);
    q_run(J, TICK_MS);
    flush(J, false);
}

static jreq_t *req_start(bjs_t *J, const char *url, const char *method, const char *headers, const char *ctype,
                         const uint8_t *body, size_t blen) {
    jreq_t *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    q->J = J;
    q->id = ++J->next_req;
    q->cb = -1;
    http_options_t o;
    memset(&o, 0, sizeof o);
    o.method = method;
    o.body = body;
    o.body_len = (uint32_t)blen;
    o.content_type = ctype && *ctype ? ctype : NULL;
    o.headers = headers && *headers ? headers : NULL;
    o.flags = browser_host_allowed(url) ? HTTP_F_ALLOW_BAD_CERT : 0;
    o.max_body = 32u << 20;
    o.timeout_ms = 30000;
    q->req = http_fetch_ex(url, &o, req_done, q);
    if (!q->req) { free(q); return NULL; }
    q->next = J->reqs;
    J->reqs = q;
    return q;
}

/* a local file (pages opened from disk) */
static uint8_t *read_file_url(const char *url, uint32_t *len) {
    if (strncasecmp(url, "file://", 7)) return NULL;
    char path[VFS_PATH_MAX];
    size_t k = 0;
    for (const char *p = url + 7; *p && *p != '?' && *p != '#' && k < sizeof path - 1; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hx[3] = { p[1], p[2], 0 };
            path[k++] = (char)strtol(hx, NULL, 16);
            p += 2;
        } else path[k++] = *p;
    }
    path[k] = 0;
    size_t sz;
    char *d = vfs_read_file(path, &sz);
    if (len) *len = (uint32_t)sz;
    return (uint8_t *)d;
}

static JSValue n_fetch(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    char *url = js_utf8(ctx, JS_ARG(0), NULL), *method = js_utf8(ctx, JS_ARG(1), NULL);
    char *hdr = js_utf8(ctx, JS_ARG(2), NULL), *ctype = js_utf8(ctx, JS_ARG(3), NULL);
    size_t blen = 0;
    uint8_t *body = JS_IsNullish(JS_ARG(4)) ? NULL : js_get_bytes(ctx, JS_ARG(4), &blen);
    JSValue cb = JS_ARG(5);
    int id = 0;
    if (url && !strncasecmp(url, "file:", 5)) {
        uint32_t len = 0;
        uint8_t *d = read_file_url(url, &len);
        JSValue args[6] = { js_mknum(d ? 200 : 0), js_new_string(ctx, d ? "OK" : ""), js_new_string(ctx, url), js_new_array(ctx, 0),
                            js_new_uint8array(ctx, d ? d : (const uint8_t *)"", d ? len : 0), js_new_string(ctx, d ? "" : "file not found") };
        free(d);
        q_call(J, cb, JS_UNDEFINED, 6, args, false);
        id = ++J->next_req;
    } else if (url && (!strncasecmp(url, "http://", 7) || !strncasecmp(url, "https://", 8))) {
        jreq_t *q = req_start(J, url, method && *method ? method : "GET", hdr, ctype, body, blen);
        if (q) { q->cb = js_root_add(ctx, cb); id = q->id; }
    }
    if (!id) {
        JSValue args[6] = { js_mknum(0), js_new_string(ctx, ""), js_new_string(ctx, url ? url : ""), js_new_array(ctx, 0),
                            js_new_uint8array(ctx, (const uint8_t *)"", 0), js_new_string(ctx, "this address cannot be fetched") };
        q_call(J, cb, JS_UNDEFINED, 6, args, false);
        id = ++J->next_req;
    }
    free(url); free(method); free(hdr); free(ctype);
    return js_mknum(id);
}

static JSValue n_abort(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    int id = js_to_int32(ctx, JS_ARG(0));
    for (jreq_t *q = J->reqs; q; q = q->next)
        if (q->id == id && !q->sc) {
            req_unlink(J, q);
            http_free(q->req);
            js_root_free(ctx, q->cb);
            free(q);
            break;
        }
    return JS_UNDEFINED;
}

/* ---- dialogs ----------------------------------------------------------------------------- */
static void dlg_box_cb(int result, void *ctx) {
    bjs_t *J = by_id((int)(intptr_t)ctx);
    if (!J || J->dlg != 1) return;
    J->dlg = 2;
    J->dlg_ok = result == MB_RESULT_OK || result == MB_RESULT_YES;
}

static void dlg_input_cb(const char *text, void *ctx) {
    bjs_t *J = by_id((int)(intptr_t)ctx);
    if (!J || J->dlg != 1) return;
    J->dlg = 2;
    J->dlg_ok = text != NULL;
    free(J->dlg_text);
    J->dlg_text = text ? dupstr(text) : NULL;
}

/* kind 0 alert, 1 confirm, 2 prompt */
static JSValue n_dialog(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    tab_t *t = J->t;
    int kind = js_to_int32(ctx, JS_ARG(0));
    int l;
    char *m = to_1252(ctx, JS_ARG(1), &l), *d = to_1252(ctx, JS_ARG(2), &l);
    char text[700], def[300], title[300];
    if (m && strlen(m) > 600) strcpy(m + 597, "...");
    web_1252_to_ui(m ? m : "", text, sizeof text);
    web_1252_to_ui(d ? d : "", def, sizeof def);
    free(m);
    free(d);
    url_t u;
    snprintf(title, sizeof title, "%s says", url_parse(t->url, &u) ? u.host : "This page");
    bool wait = js_can_wait(ctx) && J->dlg == 0 && win_is_valid(t->b->win);
    void *id = (void *)(intptr_t)J->id;
    if (!wait) {
        /* (called from code that cannot pause: the message is shown, the answer is "no") */
        if (kind == 0) ui_message_box(t->b->win, title, text, MB_OK | MB_ICON_INFO, NULL, NULL);
        return kind == 0 ? JS_UNDEFINED : kind == 1 ? JS_FALSE : JS_NULL;
    }
    J->dlg = 1;
    J->dlg_kind = kind;
    if (kind == 2) ui_input_box(t->b->win, title, text, def, dlg_input_cb, id);
    else ui_message_box(t->b->win, title, text, kind == 0 ? MB_OK | MB_ICON_INFO : MB_OKCANCEL | MB_ICON_QUESTION, dlg_box_cb, id);
    return js_wait(ctx);
}

/* ==== scripts ============================================================================== */
static bool js_type(const web_node_t *n) {
    const char *ty = web_attr(n, "type");
    if (!ty || !*ty) {
        const char *lang = web_attr(n, "language");
        return !lang || !*lang || !strncasecmp(lang, "javascript", 10) || !strcasecmp(lang, "jscript");
    }
    while (*ty == ' ') ty++;
    static const char *const ok[] = { "text/javascript", "application/javascript", "text/ecmascript", "application/ecmascript",
        "application/x-javascript", "application/x-ecmascript", "text/x-javascript", "text/x-ecmascript", "text/jscript",
        "text/livescript", "text/javascript1.0", "text/javascript1.1", "text/javascript1.2", "text/javascript1.3",
        "text/javascript1.4", "text/javascript1.5", "module" };
    size_t tl = strcspn(ty, "; ");
    for (int i = 0; i < (int)ARRAY_SIZE(ok); i++) if (strlen(ok[i]) == tl && !strncasecmp(ty, ok[i], tl)) return true;
    return false;
}

static bool is_module(const web_node_t *n) {
    const char *ty = web_attr(n, "type");
    return ty && !strncasecmp(ty, "module", 6);
}

static char *inline_source(const web_node_t *n, size_t *len) {
    size_t total = 0;
    for (web_node_t *c = n->first; c; c = c->next) if (c->type == WN_TEXT) total += (size_t)c->len * 3;
    char *s = malloc(total + 1);
    if (!s) return NULL;
    size_t o = 0;
    for (web_node_t *c = n->first; c; c = c->next) {
        if (c->type != WN_TEXT) continue;
        if (c->jsf & WNJ_UTF8) { memcpy(s + o, c->text, (size_t)c->len); o += (size_t)c->len; continue; }
        size_t ul;
        char *u = utf8_of_1252(c->text, (size_t)c->len, &ul);
        if (u) { memcpy(s + o, u, ul); o += ul; free(u); }
    }
    s[o] = 0;
    *len = o;
    return s;
}

static jscript_t *script_new(bjs_t *J, web_node_t *n, bool parser, int at) {
    if (J->nsc >= J->sccap) {
        int nc = J->sccap ? J->sccap * 2 : 16;
        jscript_t **x = realloc(J->sc, sizeof *x * (size_t)nc);
        if (!x) return NULL;
        J->sc = x;
        J->sccap = nc;
    }
    jscript_t *sc = calloc(1, sizeof *sc);
    if (!sc) return NULL;
    n->jsf |= WNJ_STARTED;
    sc->el = js_root_add(J->ctx, wrap(J, n));
    sc->parser = parser;
    const char *src = web_attr(n, "src");
    if (src) {
        char abs[B_URL_MAX];
        if (url_resolve(J->t->pg.base, src, abs, sizeof abs)) sc->url = dupstr(abs);
        else sc->failed = true;
        if (!sc->url) sc->failed = true;
        sc->async = !parser ? !(n->jsf & WNJ_SYNC) : web_has_attr(n, "async");
        sc->defer = parser && (web_has_attr(n, "defer") || is_module(n));
    } else {
        sc->src = inline_source(n, &sc->len);
        sc->state = SC_READY;
        sc->defer = parser && is_module(n);
    }
    if (at < 0 || at > J->nsc) at = J->nsc;
    memmove(&J->sc[at + 1], &J->sc[at], sizeof(jscript_t *) * (size_t)(J->nsc - at));
    J->sc[at] = sc;
    J->nsc++;
    return sc;
}

static void script_fetch(bjs_t *J, jscript_t *sc) {
    if (sc->state != SC_WAIT || sc->failed || !sc->url) return;
    if (!strncasecmp(sc->url, "file:", 5) || !strncasecmp(sc->url, "data:", 5)) {
        uint32_t len = 0;
        uint8_t *d = NULL;
        if (!strncasecmp(sc->url, "file:", 5)) d = read_file_url(sc->url, &len);
        else {
            /* data:[type][;base64],... (scripts: usually plain or base64 text) */
            const char *comma = strchr(sc->url, ',');
            if (comma) {
                bool b64 = (size_t)(comma - sc->url) >= 7 && !strncasecmp(comma - 7, ";base64", 7);
                const char *p = comma + 1;
                size_t n = strlen(p);
                d = malloc(n + 1);
                if (d) {
                    size_t k = 0;
                    if (b64) {
                        uint32_t acc = 0;
                        int bits = 0;
                        for (; *p; p++) {
                            const char *al = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                            const char *x = strchr(al, *p);
                            if (!x || !*p) continue;
                            acc = (acc << 6) | (uint32_t)(x - al);
                            bits += 6;
                            if (bits >= 8) { bits -= 8; d[k++] = (uint8_t)(acc >> bits); }
                        }
                    } else {
                        for (; *p; p++) {
                            if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) { char hx[3] = { p[1], p[2], 0 }; d[k++] = (uint8_t)strtol(hx, NULL, 16); p += 2; }
                            else d[k++] = (uint8_t)*p;
                        }
                    }
                    len = (uint32_t)k;
                }
            }
        }
        script_loaded(J, sc, d, len, d != NULL);
        free(d);
        return;
    }
    if (strncasecmp(sc->url, "http://", 7) && strncasecmp(sc->url, "https://", 8)) { script_loaded(J, sc, NULL, 0, false); return; }
    jreq_t *q = req_start(J, sc->url, "GET", NULL, NULL, NULL, 0);
    if (!q) { script_loaded(J, sc, NULL, 0, false); return; }
    q->sc = sc;
    sc->state = SC_LOADING;
}

static void script_loaded(bjs_t *J, jscript_t *sc, const uint8_t *body, uint32_t len, bool ok) {
    if (!ok || !body) {
        sc->failed = true;
        sc->state = SC_READY;
        host_log(J, JS_LOG_ERROR, "a script could not be loaded:");
        host_log(J, JS_LOG_ERROR, sc->url ? sc->url : "?");
        return;
    }
    if (len >= 3 && body[0] == 0xEF && body[1] == 0xBB && body[2] == 0xBF) { body += 3; len -= 3; }
    if (valid_utf8(body, len)) { sc->src = dupn((const char *)body, len); sc->len = len; }
    else sc->src = utf8_of_1252((const char *)body, len, &sc->len);
    sc->state = SC_READY;
}

static void fire_simple(bjs_t *J, JSValue target, const char *type, bool bubbles) {
    JSValue args[4] = { target, js_new_string(J->ctx, type), JS_BOOL(bubbles), JS_FALSE };
    q_call(J, js_get(J->ctx, J->rt, "fire"), JS_UNDEFINED, 4, args, false);
}

static void script_done(bjs_t *J, jscript_t *sc) {
    sc->state = SC_DONE;
    if (J->running == sc) J->running = NULL;
    free(sc->src);
    sc->src = NULL;
    if (sc->url) fire_simple(J, js_root_get(J->ctx, sc->el), sc->failed ? "error" : "load", false);
}

static void run_script(bjs_t *J, jscript_t *sc) {
    if (sc->failed) {
        script_done(J, sc);
        return;
    }
    jtask_t *k = task_new(J, JT_SCRIPT);
    if (!k) { script_done(J, sc); return; }
    k->sc = sc;
    sc->state = SC_RUNNING;
    q_push(J, k);
}

/* scripts a script put into the document */
static void run_inserted_scripts(bjs_t *J, web_node_t *n) {
    if (J->dying) return;
    for (web_node_t *x = n; x; x = walk_next(x, n)) {
        if (x->type != WN_ELEM || x->tag != T_SCRIPT || (x->jsf & WNJ_STARTED) || !js_type(x)) continue;
        bool ext = web_attr(x, "src") != NULL;
        bool has_code = false;
        for (web_node_t *c = x->first; c; c = c->next) if (c->type == WN_TEXT && c->len) has_code = true;
        if (!ext && !has_code) continue;                   /* (runs when it gets code) */
        jscript_t *sc = script_new(J, x, false, -1);
        if (!sc) continue;
        if (ext) { script_fetch(J, sc); continue; }
        /* inline: at once, like browsers */
        jscript_t *was = J->running;
        J->running = sc;
        char name[200];
        snprintf(name, sizeof name, "%s (inserted script)", J->t->url);
        JSValue r = js_eval(J->ctx, sc->src ? sc->src : "", sc->len, name);
        if (JS_IsException(r)) js_report_exception(J->ctx);
        J->running = was;
        sc->state = SC_DONE;
        free(sc->src);
        sc->src = NULL;
    }
}

/* document.write while a parser-inserted script runs: what it wrote goes after it; its
   scripts run next, in order */
static void script_write(bjs_t *J, web_node_t *frag) {
    jscript_t *cur = J->running;
    web_node_t *el = NODE(js_root_get(J->ctx, cur->el));
    if (!el || !el->parent) { cand_add(J, frag); return; }
    int at = 0;
    while (at < J->nsc && J->sc[at] != cur) at++;
    at++;
    for (web_node_t *x = frag; x; x = walk_next(x, frag))
        if (x->type == WN_ELEM && x->tag == T_SCRIPT) x->jsf &= (uint16_t)~WNJ_STARTED;
    web_node_t *ref = vnext(el);
    /* collect the scripts first (inserting runs inline ones at once otherwise) */
    web_node_t *list[64];
    int nl = 0;
    for (web_node_t *x = frag; x && nl < 64; x = walk_next(x, frag))
        if (x->type == WN_ELEM && x->tag == T_SCRIPT && js_type(x)) { x->jsf |= WNJ_STARTED; list[nl++] = x; }
    insert(J, el->parent, frag, ref);
    cand_add(J, frag);
    for (int i = 0; i < nl; i++) {
        jscript_t *sc = script_new(J, list[i], true, at++);
        if (sc) { sc->async = sc->defer = false; script_fetch(J, sc); }
    }
}

static bool page_has_scripts(web_doc_t *d) {
    for (web_node_t *n = d->root; n; n = walk_next(n, d->root)) {
        if (n->type != WN_ELEM) continue;
        if (n->tag == T_SCRIPT && js_type(n)) return true;
        for (int i = 0; i < n->nattrs; i++) {
            const char *a = n->attrs[i].name;
            if (a[0] == 'o' && a[1] == 'n' && a[2]) return true;
            if (!strcmp(a, "href") && !strncasecmp(n->attrs[i].value, "javascript:", 11)) return true;
        }
    }
    return false;
}

static void gather_scripts(bjs_t *J) {
    web_doc_t *d = J->doc;
    bool nomodule = false;
    for (web_node_t *n = d->root; n; n = walk_next(n, d->root))
        if (n->type == WN_ELEM && n->tag == T_SCRIPT && web_has_attr(n, "nomodule")) nomodule = true;
    J->prefer_nomodule = nomodule;
    for (web_node_t *n = d->root; n; n = walk_next(n, d->root)) {
        if (n->type != WN_ELEM || n->tag != T_SCRIPT || (n->jsf & WNJ_STARTED) || !js_type(n)) continue;
        /* (no ES modules: a page that has fallbacks for that gets them) */
        if (nomodule && is_module(n)) { n->jsf |= WNJ_STARTED; continue; }
        jscript_t *sc = script_new(J, n, true, -1);
        if (sc) script_fetch(J, sc);
    }
}

static bool subs_busy(tab_t *t) {
    for (int i = 0; i < B_MAX_SUBS; i++) if (t->subs[i]) return true;
    return false;
}

/* the next scripts to run, and the page's loading events */
static void scripts_step(bjs_t *J) {
    if (J->cur || J->qh || js_busy(J->ctx)) return;
    jscript_t *next = NULL;
    bool waiting = false;
    for (int i = 0; i < J->nsc; i++) {
        jscript_t *sc = J->sc[i];
        if (!sc->parser || sc->async || sc->defer || sc->state == SC_DONE) continue;
        if (sc->state == SC_READY) next = sc; else waiting = true;
        break;
    }
    if (!next && !waiting && J->ready == 0)
        for (int i = 0; i < J->nsc; i++) {
            jscript_t *sc = J->sc[i];
            if (!sc->parser || !sc->defer || sc->state == SC_DONE) continue;
            if (sc->state == SC_READY) next = sc; else waiting = true;
            break;
        }
    if (next) { run_script(J, next); return; }
    for (int i = 0; i < J->nsc; i++) {
        jscript_t *sc = J->sc[i];
        if ((sc->async || !sc->parser) && sc->state == SC_READY) { run_script(J, sc); return; }
    }
    if (waiting) return;
    if (J->ready == 0) {
        J->ready = 1;
        J->dcl_at = sys_uptime_ms();
        fire_simple(J, J->docobj, "readystatechange", false);
        fire_simple(J, J->docobj, "DOMContentLoaded", true);
        return;
    }
    if (J->ready == 1) {
        for (int i = 0; i < J->nsc; i++) if (J->sc[i]->state == SC_LOADING) return;
        if (subs_busy(J->t) && sys_uptime_ms() - J->dcl_at < 5000) return;
        J->ready = 2;
        fire_simple(J, J->docobj, "readystatechange", false);
        fire_simple(J, js_global(J->ctx), "load", false);
        fire_simple(J, js_global(J->ctx), "pageshow", false);
    }
}

/* ==== batching the page's updates ========================================================== */
/* before styling: the page's lists (links, controls, pictures) follow the document */
static void sync(bjs_t *J) {
    tab_t *t = J->t;
    if (!J->dirty) return;
    J->dirty = false;
    web_page_recollect(&t->pg);
    web_svg_free_cache(&t->pg);
    web_ctrl_t *c = ctl_of(t, J->focus);
    t->focus_ctrl = c ? (int)(c - t->pg.ctrls) : -1;
    t->press_ctl = -1;
    t->hover_link = -1;
}

static void flush(bjs_t *J, bool force) {
    tab_t *t = J->t;
    if (J->dying) return;
    if (!J->dirty && !J->dirty_css) {
        if (J->repaint) { J->repaint = false; repaint(t); }
        return;
    }
    uint32_t now = sys_uptime_ms();
    if (!force && now - J->flush_at < MAX(30u, J->flush_cost * 2)) return;
    if (!t->styled && !force) { sync(J); return; }      /* (the first styling comes when the stylesheets are in) */
    uint32_t t0 = now;
    if (J->dirty_css) { J->dirty_css = false; sync(J); tab_css_reload(t); }
    tab_restyle(t);
    tab_load_resources(t);
    J->repaint = false;
    J->flush_cost = sys_uptime_ms() - t0;
    J->flush_at = sys_uptime_ms();
}

void bjs_styled(tab_t *t) {
    bjs_t *J = t->js;
    if (!J) return;
    J->flush_at = sys_uptime_ms();
    free_dropped(J);
}

/* tab_restyle calls this first */
void bjs_sync(tab_t *t) { if (t->js) sync(t->js); }

/* back / forward to another entry of the same page (pushState, #fragment) */
void bjs_popstate(tab_t *t, const char *old_url) {
    bjs_t *J = t->js;
    if (!J) return;
    JSValue args[6] = { js_global(J->ctx), js_mknum(9), js_new_string(J->ctx, "popstate"), JS_FALSE, JS_FALSE, js_mknum(t->hpos) };
    q_call(J, js_get(J->ctx, J->rt, "event"), JS_UNDEFINED, 6, args, true);
    const char *a = strchr(old_url, '#'), *b = strchr(t->url, '#');
    if ((a || b) && (!a || !b || strcmp(a, b))) fire_hashchange(J, old_url, t->url);
    q_run(J, TICK_MS);
    flush(J, true);
}

/* ==== setting up a page ==================================================================== */
static const js_class_t node_class = { "Node", node_finalize, node_mark, NULL, NULL, NULL, NULL };
static const js_class_t store_class = { "Storage", NULL, NULL, store_hook_get, store_hook_set, store_hook_del, store_hook_keys };

static JSValue n_init(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JTHIS;
    JSValue p = JS_ARG(0);
    J->protos = p;
    js_pin(ctx, p);
    JSValue node = js_get(ctx, js_get(ctx, p, "Node"), "prototype");
    JSValue elem = js_get(ctx, js_get(ctx, p, "Element"), "prototype");
    J->p_text = js_get(ctx, js_get(ctx, p, "Text"), "prototype");
    J->p_comment = js_get(ctx, js_get(ctx, p, "Comment"), "prototype");
    J->p_frag = js_get(ctx, js_get(ctx, p, "DocumentFragment"), "prototype");
    J->p_html = js_get(ctx, js_get(ctx, p, "HTMLElement"), "prototype");
    J->p_unknown = js_get(ctx, js_get(ctx, p, "HTMLUnknownElement"), "prototype");
    J->p_svg = js_get(ctx, js_get(ctx, p, "SVGElement"), "prototype");
    J->p_doc = js_get(ctx, js_get(ctx, p, "Document"), "prototype");
    JSValue tags = js_get(ctx, p, "tags");
    for (int i = 1; i < T_COUNT; i++) {
        JSValue v = js_get(ctx, tags, web_tag_name(i));
        J->tagproto[i] = JS_IsObject(v) ? v : JS_UNDEFINED;
    }
    static const char *const ng[] = { "parentNode", "parentElement", "firstChild", "lastChild", "previousSibling",
                                      "nextSibling", "nodeType", "nodeName", "ownerDocument", "isConnected" };
    for (int i = 0; i < 10; i++) js_def_getset(ctx, node, ng[i], node_get, NULL, i);
    js_def_getset(ctx, elem, "tagName", node_get, NULL, 10);
    js_def_getset(ctx, elem, "localName", node_get, NULL, 11);
    js_def_getset(ctx, elem, "previousElementSibling", node_get, NULL, 12);
    js_def_getset(ctx, elem, "nextElementSibling", node_get, NULL, 13);
    J->docobj = wrap(J, J->doc->root);
    js_pin(ctx, J->docobj);
    return J->docobj;
}

static void def_natives(bjs_t *J, JSValue N) {
    JSContext *c = J->ctx;
    static const struct { const char *name; JSNativeFn fn; int len, magic; } tab[] = {
        { "init", n_init, 1, 0 }, { "childNodes", n_child_nodes, 1, 0 }, { "children", n_child_nodes, 1, 1 },
        { "insert", n_insert, 3, 0 }, { "remove", n_remove, 1, 0 }, { "clone", n_clone, 2, 0 },
        { "contains", n_contains, 2, 0 }, { "order", n_order, 2, 0 }, { "query", n_query, 3, 0 },
        { "matches", n_matches, 2, 0 }, { "closest", n_matches, 2, 1 }, { "byTag", n_by, 2, 0 },
        { "byClass", n_by, 2, 1 }, { "byId", n_by, 2, 2 }, { "create", n_create, 2, 0 },
        { "attr", n_attr, 2, 0 }, { "setAttr", n_attr, 3, 1 }, { "delAttr", n_attr, 2, 2 }, { "attrNames", n_attr, 1, 3 },
        { "text", n_text, 1, 0 }, { "setText", n_text, 2, 1 }, { "data", n_text, 1, 2 }, { "setData", n_text, 2, 3 },
        { "html", n_html, 2, 0 }, { "setHTML", n_set_html, 3, 0 }, { "template", n_template, 1, 0 },
        { "ctl", n_ctl, 4, 0 }, { "key", n_key, 3, 0 }, { "active", n_active, 0, 0 }, { "inLink", n_in_link, 1, 0 },
        { "rect", n_rect, 1, 0 }, { "view", n_view, 0, 0 }, { "scroll", n_scroll, 2, 0 }, { "hit", n_hit, 2, 0 },
        { "style", n_style, 2, 0 }, { "img", n_img, 1, 0 }, { "media", n_media, 1, 0 }, { "supports", n_media, 1, 1 },
        { "log", n_log, 2, 0 }, { "now", n_now, 0, 0 }, { "timer", n_timer, 4, 0 }, { "raf", n_timer, 1, 1 },
        { "clear", n_clear, 1, 0 }, { "href", n_href, 0, 0 }, { "nav", n_nav, 2, 0 }, { "hist", n_hist, 2, 0 },
        { "open", n_open, 1, 0 }, { "follow", n_follow, 1, 0 }, { "title", n_title, 0, 0 }, { "cookie", n_cookie, 0, 0 },
        { "info", n_info, 1, 0 }, { "listen", n_listen, 1, 0 }, { "listening", n_listen, 1, 1 },
        { "observe", n_observe, 1, 0 }, { "version", n_version, 0, 0 }, { "random", n_random, 1, 0 },
        { "clip", n_clip, 0, 0 }, { "status", n_status, 1, 0 }, { "write", n_write, 1, 0 },
        { "storage", n_storage, 2, 0 }, { "store", n_store, 4, 0 }, { "fetch", n_fetch, 6, 0 }, { "abort", n_abort, 1, 0 },
        { "dialog", n_dialog, 3, 0 },
    };
    for (int i = 0; i < (int)ARRAY_SIZE(tab); i++) js_def_func(c, N, tab[i].name, tab[i].fn, tab[i].len, tab[i].magic);
}

static void destroy(bjs_t *J);

void bjs_page(tab_t *t, int charset) {
    (void)charset;
    web_doc_t *d = t->pg.doc;
    if (t->js || !d || !bdata_js_on || t->internal || !page_has_scripts(d)) return;
    bjs_t *J = calloc(1, sizeof *J);
    if (!J) return;
    J->t = t;
    J->doc = d;
    J->id = g_next_id++;
    J->focus_root = -1;
    J->follow_root = J->submit_root = J->submit_ctl = -1;
    J->start_ms = sys_uptime_ms();
    J->wall0 = (double)sys_time() * 1000.0 - (double)J->start_ms;
    size_t lim = sys_mem_free() / 3;
    if (lim > (96u << 20)) lim = 96u << 20;
    if (lim < (16u << 20)) lim = 16u << 20;
    js_host_t h;
    memset(&h, 0, sizeof h);
    h.log = host_log;
    h.now_ms = host_wall;
    h.mono_ms = host_mono;
    h.ud = J;
    h.mem_limit = lim;
    h.slice_ms = 20;
    h.nested_limit_ms = 4000;
    h.stack_top = (uintptr_t)kernel_stack_top;
    J->ctx = js_new_context(&h);
    if (!J->ctx) { free(J); return; }
    J->cls_node = js_new_class(J->ctx, &node_class);
    J->cls_store = js_new_class(J->ctx, &store_class);
    J->next = g_list;
    g_list = J;
    t->js = J;
    JSValue N = js_new_object(J->ctx);
    def_natives(J, N);
    uint32_t t0 = sys_uptime_ms();
    JSValue fn = js_eval(J->ctx, browser_jsrt, browser_jsrt_len, "browser_jsrt.js");
    JSValue rt = JS_EXCEPTION;
    if (!JS_IsException(fn)) {
        JSValue args[2] = { N, js_global(J->ctx) };
        rt = js_call(J->ctx, fn, JS_UNDEFINED, 2, args);
    }
    if (JS_IsException(rt) || !JS_IsObject(rt) || !JS_IsObject(J->docobj)) {
        js_report_exception(J->ctx);
        klog("browser: scripts could not be set up for %s", t->url);
        destroy(J);
        return;
    }
    J->rt = rt;
    js_pin(J->ctx, rt);
    J->focus_root = js_root_add(J->ctx, JS_NULL);
    d->scripting = true;
    t->pg.want_boxes = true;
    gather_scripts(J);
    klog("browser: %s: scripts on (%d scripts, set up in %u ms)", t->url, J->nsc, (unsigned)(sys_uptime_ms() - t0));
}

static void destroy(bjs_t *J) {
    tab_t *t = J->t;
    J->dying = true;
    for (bjs_t **pp = &g_list; *pp; pp = &(*pp)->next) if (*pp == J) { *pp = J->next; break; }
    while (J->reqs) { jreq_t *q = J->reqs; J->reqs = q->next; http_free(q->req); free(q); }
    while (J->qh) { jtask_t *k = J->qh; J->qh = k->next; free(k); }
    free(J->cur);
    for (int i = 0; i < J->nsc; i++) { free(J->sc[i]->url); free(J->sc[i]->src); free(J->sc[i]); }
    free(J->sc);
    free(J->tm);
    stores_save(true);
    if (J->ctx) js_free_context(J->ctx);
    for (int i = 0; i < J->hcap; i++) {
        web_node_t *x = J->hs[i];
        if (!x || x == HS_TOMB) continue;
        free_parts(x);
        if (x->jsf & WNJ_HEAP) free(x);
    }
    free(J->hs);
    free(J->cand);
    for (int i = 0; i < (int)ARRAY_SIZE(J->selc); i++) { free(J->selc[i].text); web_sel_free(J->selc[i].sel); }
    free(J->nav_url);
    free(J->open_url);
    free(J->dlg_text);
    if (t->js == J) t->js = NULL;
    t->pg.want_boxes = false;
    if (t->pg.doc == J->doc) J->doc->scripting = false;
    free(J);
}

void bjs_free(tab_t *t) {
    if (t->js) destroy(t->js);
}

/* ==== the tick ============================================================================== */
static void cur_finished(bjs_t *J, int st) {
    jtask_t *k = J->cur;
    J->cur = NULL;
    if (k) finish(J, k, st);
}

/* a task suspended (its slice ran out) carries on; stopped when it runs too long */
static void carry_on(bjs_t *J) {
    tab_t *t = J->t;
    if (js_waiting(J->ctx)) {
        if (J->dlg != 2) return;
        JSValue v = JS_UNDEFINED;
        if (J->dlg_kind == 1) v = JS_BOOL(J->dlg_ok);
        else if (J->dlg_kind == 2) v = J->dlg_ok && J->dlg_text ? cstr_1252(J->ctx, J->dlg_text) : JS_NULL;
        J->dlg = 0;
        free(J->dlg_text);
        J->dlg_text = NULL;
        uint32_t t0 = sys_uptime_ms();
        int st = js_resume_with(J->ctx, v);
        J->cur_ms += sys_uptime_ms() - t0;
        if (st != JS_SUSPENDED) cur_finished(J, st);
        return;
    }
    if (!js_busy(J->ctx)) { cur_finished(J, JS_DONE); return; }
    uint32_t t0 = sys_uptime_ms();
    int st = js_resume(J->ctx);
    J->cur_ms += sys_uptime_ms() - t0;
    if (st != JS_SUSPENDED) { cur_finished(J, st); return; }
    if (J->cur_ms >= SLOW_MS) {
        js_kill(J->ctx);
        klog("browser: %s: a script ran for %u s and was stopped (page not responding)", t->url, (unsigned)(J->cur_ms / 1000));
        strlcpy(t->b->status, "Page not responding: a script ran too long and was stopped", sizeof t->b->status);
        browser_tab_changed(t);
        cur_finished(J, JS_KILLED);
    }
}

/* things scripts asked for that replace or leave the page: done last, from the tick */
static void pending(bjs_t *J) {
    tab_t *t = J->t;
    if (J->cur || js_busy(J->ctx)) return;
    if (J->open_url) {
        char *u = J->open_url;
        J->open_url = NULL;
        browser_new_tab(t->b, u, true);
        free(u);
    }
    if (J->submit_root >= 0) {
        web_node_t *f = NODE(js_root_get(J->ctx, J->submit_root));
        web_node_t *s = J->submit_ctl >= 0 ? NODE(js_root_get(J->ctx, J->submit_ctl)) : NULL;
        js_root_free(J->ctx, J->submit_root);
        if (J->submit_ctl >= 0) js_root_free(J->ctx, J->submit_ctl);
        J->submit_root = J->submit_ctl = -1;
        sync(J);
        if (f && connected(J, f)) { tab_submit_form(t, f, s ? s->ctl : -1); return; }
    }
    if (J->follow_root >= 0) {
        web_node_t *a = NODE(js_root_get(J->ctx, J->follow_root));
        js_root_free(J->ctx, J->follow_root);
        J->follow_root = -1;
        sync(J);
        if (a && a->link >= 0) { tab_follow_link_now(t, a->link); return; }
    }
    if (J->hist_go) {
        int d = J->hist_go;
        J->hist_go = 0;
        tab_history_go(t, d);
        return;
    }
    if (J->nav_url) {
        char url[B_URL_MAX];
        strlcpy(url, J->nav_url, sizeof url);
        int mode = J->nav_mode;
        free(J->nav_url);
        J->nav_url = NULL;
        tab_go(t, url, mode == 1 ? NAV_REPLACE : mode == 2 ? NAV_RELOAD : NAV_NEW);
        return;
    }
}

void bjs_tick(tab_t *t) {
    bjs_t *J = t->js;
    if (!J) return;
    uint32_t t0 = sys_uptime_ms();
    if (J->cur) carry_on(J);
    if (!J->cur) {
        scripts_step(J);
        timers_due(J);
        q_run(J, TICK_MS);
        if (!J->cur && sys_uptime_ms() - t0 < TICK_MS) { scripts_step(J); q_run(J, TICK_MS); }
    }
    flush(J, false);
    stores_save(false);
    pending(J);           /* (last: the page may be gone after it) */
}

/* ==== input ================================================================================= */
static int js_mods(int m) { return ((m & MOD_CTRL) ? 1 : 0) | ((m & MOD_SHIFT) ? 2 : 0) | ((m & MOD_ALT) ? 4 : 0); }
static int js_button(int b) { return b == MOUSE_RIGHT ? 2 : b == MOUSE_MIDDLE ? 1 : 0; }

static void key_name(const event_t *ev, char *out, size_t size) {
    static const struct { int k; const char *n; } names[] = {
        { KEY_BACKSPACE, "Backspace" }, { KEY_TAB, "Tab" }, { KEY_ENTER, "Enter" }, { KEY_ESCAPE, "Escape" }, { KEY_SPACE, " " },
        { KEY_UP, "ArrowUp" }, { KEY_DOWN, "ArrowDown" }, { KEY_LEFT, "ArrowLeft" }, { KEY_RIGHT, "ArrowRight" },
        { KEY_HOME, "Home" }, { KEY_END, "End" }, { KEY_PAGEUP, "PageUp" }, { KEY_PAGEDOWN, "PageDown" },
        { KEY_INSERT, "Insert" }, { KEY_DELETE, "Delete" }, { KEY_F1, "F1" }, { KEY_F2, "F2" }, { KEY_F3, "F3" },
        { KEY_F4, "F4" }, { KEY_F5, "F5" }, { KEY_F6, "F6" }, { KEY_F7, "F7" }, { KEY_F8, "F8" }, { KEY_F9, "F9" },
        { KEY_F10, "F10" }, { KEY_F11, "F11" }, { KEY_F12, "F12" }, { KEY_LSHIFT, "Shift" }, { KEY_RSHIFT, "Shift" },
        { KEY_LCTRL, "Control" }, { KEY_RCTRL, "Control" }, { KEY_LALT, "Alt" }, { KEY_RALT, "Alt" },
        { KEY_LWIN, "Meta" }, { KEY_RWIN, "Meta" }, { KEY_MENU, "ContextMenu" }, { KEY_CAPSLOCK, "CapsLock" },
    };
    for (int i = 0; i < (int)ARRAY_SIZE(names); i++) if (names[i].k == ev->key) { strlcpy(out, names[i].n, size); return; }
    int k = ev->key;
    if (k > 32 && k < 127) {
        static const char shifted[] = ")!@#$%^&*(";
        if ((ev->mods & MOD_SHIFT) && k >= 'a' && k <= 'z') k -= 32;
        else if ((ev->mods & MOD_SHIFT) && k >= '0' && k <= '9') k = shifted[k - '0'];
        else if ((ev->mods & MOD_CAPS) && k >= 'a' && k <= 'z') k -= 32;
        out[0] = (char)k;
        out[1] = 0;
        return;
    }
    strlcpy(out, "Unidentified", size);
}

/* pages without scripts: what form controls do */
static bool plain_key(tab_t *t, const event_t *ev) {
    int idx = t->focus_ctrl;
    web_ctrl_t *c = &t->pg.ctrls[idx];
    caret_on(t);
    if (ev->type == EV_CHAR) {
        if (!is_text_ctl(c)) return false;
        char ch = (char)ev->ch;
        ctl_insert(c, &ch, 1);
        repaint(t);
        return true;
    }
    switch (ev->key) {
    case KEY_ESCAPE: t->focus_ctrl = -1; repaint(t); return true;
    case KEY_SPACE:
        if (c->type == C_CHECKBOX || c->type == C_RADIO || c->type == C_SUBMIT || c->type == C_RESET || c->type == C_SELECT) {
            ctl_activate(t, idx);
            return true;
        }
        break;
    case KEY_ENTER:
        if (c->type == C_TEXTAREA) { ctl_insert(c, "\n", 1); repaint(t); return true; }
        if (c->type == C_TEXT || c->type == C_PASSWORD || c->type == C_SUBMIT || c->type == C_IMAGE ||
            c->type == C_CHECKBOX || c->type == C_RADIO) { tab_submit(t, idx); return true; }
        return false;
    default: break;
    }
    if (!is_text_ctl(c)) return false;
    if (ctl_edit_key(c, ev->key, ev->mods)) { repaint(t); return true; }
    return !(ev->mods & MOD_CTRL) && (ev->key == KEY_UP || ev->key == KEY_DOWN || ev->key == KEY_SPACE);
}

static bool plain_input(tab_t *t, const event_t *ev, int x, int y) {
    switch (ev->type) {
    case EV_MOUSE_DOWN: {
        if (ev->button != MOUSE_LEFT || !t->styled) return false;
        int c = web_hit_ctrl(&t->pg, x, y);
        t->press_ctl = c;
        if (c >= 0) {
            web_ctrl_t *ct = &t->pg.ctrls[c];
            if (ct->disabled) return true;
            t->focus_ctrl = c;
            caret_on(t);
            if (is_text_ctl(ct)) ctl_caret_at(ct, x, y);
            repaint(t);
            return true;
        }
        if (t->focus_ctrl >= 0) { t->focus_ctrl = -1; repaint(t); }
        return false;
    }
    case EV_MOUSE_UP: {
        if (ev->button != MOUSE_LEFT || !t->styled) return false;
        int c = web_hit_ctrl(&t->pg, x, y), pc = t->press_ctl;
        t->press_ctl = -1;
        if (c >= 0 && c == pc) { ctl_activate(t, c); return true; }
        return c >= 0;
    }
    case EV_KEY_DOWN: case EV_CHAR:
        if (t->focus_ctrl >= 0 && t->focus_ctrl < t->pg.nctrls && !(ev->mods & MOD_ALT)) return plain_key(t, ev);
        return false;
    default:
        return false;
    }
}

bool tab_page_input(tab_t *t, const event_t *ev, int x, int y) {
    bjs_t *J = t->js;
    if (!J) return plain_input(t, ev, x, y);
    JSValue argv[8];
    JSValue r = JS_UNDEFINED;
    bool used = false;
    switch (ev->type) {
    case EV_MOUSE_DOWN: case EV_MOUSE_UP: case EV_MOUSE_MOVE: case EV_DOUBLE_CLICK: case EV_MOUSE_LEAVE: case EV_MOUSE_WHEEL: {
        if (!t->styled) return false;
        int kind = ev->type == EV_MOUSE_DOWN ? 0 : ev->type == EV_MOUSE_UP ? 1 : ev->type == EV_MOUSE_MOVE ? 2 :
                   ev->type == EV_DOUBLE_CLICK ? 3 : ev->type == EV_MOUSE_LEAVE ? 4 : 5;
        if (kind == 2 && !(J->listen & 3)) return false;
        if (kind == 2 && (J->qh || J->cur)) return false;        /* (moves are dropped while scripts are busy) */
        if (kind == 5 && !(J->listen & 4)) return false;
        web_node_t *n = kind == 4 ? NULL : web_hit_node(&t->pg, x, y);
        if (!n && kind != 4) n = J->doc->body ? J->doc->body : J->doc->html;
        if (kind == 0 || kind == 1) { J->px = x; J->py = y; J->pmods = ev->mods; }
        argv[0] = js_mknum(kind);
        argv[1] = n ? wrap(J, n) : JS_NULL;
        argv[2] = js_mknum(x - t->sx);
        argv[3] = js_mknum(y - t->sy);
        argv[4] = js_mknum(js_button(ev->button));
        argv[5] = js_mknum(kind == 5 ? ev->wheel : ev->buttons);
        argv[6] = js_mknum(js_mods(ev->mods));
        bool done = hook_now(J, "pointer", 7, argv, &r, kind != 2 && kind != 4);
        if (kind == 0) used = web_hit_ctrl(&t->pg, x, y) >= 0;
        if (kind == 5) used = done && r == JS_FALSE;
        break;
    }
    case EV_KEY_DOWN: case EV_KEY_UP: case EV_CHAR: {
        char k[24];
        if (ev->type == EV_CHAR) {
            uint32_t cp = web_1252_to_cp(ev->ch & 255);
            if (cp < 0x80) { k[0] = (char)cp; k[1] = 0; }
            else if (cp < 0x800) { k[0] = (char)(0xC0 | (cp >> 6)); k[1] = (char)(0x80 | (cp & 0x3F)); k[2] = 0; }
            else { k[0] = (char)(0xE0 | (cp >> 12)); k[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); k[2] = (char)(0x80 | (cp & 0x3F)); k[3] = 0; }
        } else key_name(ev, k, sizeof k);
        argv[0] = js_mknum(ev->type == EV_KEY_DOWN ? 0 : ev->type == EV_CHAR ? 1 : 2);
        argv[1] = js_new_string(J->ctx, k);
        argv[2] = JS_BOOL(ev->repeat);
        argv[3] = js_mknum(js_mods(ev->mods));
        argv[4] = js_mknum(ev->key);
        argv[5] = js_mknum(ev->mods);
        bool done = hook_now(J, "key", 6, argv, &r, true);
        if (done) used = js_truthy(r);
        else used = ev->type != EV_KEY_UP && t->focus_ctrl >= 0;
        if (ev->type == EV_KEY_UP) used = false;
        break;
    }
    default:
        return false;
    }
    flush(J, ev->type != EV_MOUSE_MOVE);
    return used;
}

/* a link the user clicked: the page's click handlers first, then (if they let it) the link */
bool bjs_link(tab_t *t, int link) {
    bjs_t *J = t->js;
    if (!J || link < 0 || link >= t->pg.nlinks) return false;
    web_node_t *a = t->pg.links[link].node;
    if (!a) return false;
    web_node_t *n = web_hit_node(&t->pg, J->px, J->py);
    bool inside = false;
    for (web_node_t *x = n; x; x = x->parent) if (x == a) { inside = true; break; }
    JSValue argv[5] = { wrap(J, inside ? n : a), wrap(J, a), js_mknum(J->px - t->sx), js_mknum(J->py - t->sy), js_mknum(js_mods(J->pmods)) };
    hook_now(J, "link", 5, argv, NULL, true);
    flush(J, true);
    return true;
}
