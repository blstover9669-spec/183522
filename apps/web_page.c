/*
 * web_page.c - the resources of a parsed page: links, images and form
 * controls (collected from the document once after parsing), and form
 * value encoding.
 */
#include "web.h"

static char *dupstr(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

static int add_link(web_page_t *pg, web_node_t *n, const char *href) {
    if (pg->nlinks >= pg->lcap) {
        int nc = pg->lcap ? pg->lcap * 2 : 64;
        web_link_t *nl = realloc(pg->links, sizeof *nl * nc);
        if (!nl) return -1;
        pg->links = nl;
        pg->lcap = nc;
    }
    web_link_t *l = &pg->links[pg->nlinks];
    l->node = n;
    l->href = href;
    l->target = web_attr(n, "target");
    return pg->nlinks++;
}

/* index of the image for an absolute URL (shared between <img> elements) */
static int add_image(web_page_t *pg, const char *src) {
    char *abs = malloc(4096);
    if (!abs) return -1;
    if (!url_resolve(pg->base, src, abs, 4096)) strlcpy(abs, src, 4096);
    for (int i = 0; i < pg->nimgs; i++)
        if (!strcmp(pg->imgs[i].url, abs)) { free(abs); return i; }
    if (pg->nimgs >= pg->icap) {
        int nc = pg->icap ? pg->icap * 2 : 16;
        web_image_t *ni = realloc(pg->imgs, sizeof *ni * nc);
        if (!ni) { free(abs); return -1; }
        pg->imgs = ni;
        pg->icap = nc;
    }
    web_image_t *im = &pg->imgs[pg->nimgs];
    memset(im, 0, sizeof *im);
    im->url = realloc(abs, strlen(abs) + 1);
    if (!im->url) im->url = abs;
    im->state = IMG_PENDING;
    return pg->nimgs++;
}

static const char *img_src(const web_node_t *n) {
    const char *src = web_attr(n, "src");
    const char *lazy = web_attr(n, "data-src");
    if (!lazy) lazy = web_attr(n, "data-original");
    if (lazy && *lazy && (!src || !*src || !strncasecmp(src, "data:", 5))) return lazy;
    if (src && *src) return src;
    /* srcset: the first candidate */
    const char *ss = web_attr(n, "srcset");
    if (ss && *ss) {
        static char buf[1024];
        while (*ss == ' ') ss++;
        size_t k = 0;
        while (ss[k] && ss[k] != ' ' && ss[k] != ',' && k < sizeof buf - 1) { buf[k] = ss[k]; k++; }
        buf[k] = 0;
        if (k) return buf;
    }
    return NULL;
}

void web_ctrl_set_value(web_ctrl_t *c, const char *s, int len) {
    if (len < 0) len = s ? (int)strlen(s) : 0;
    if (len + 1 > c->vcap) {
        int nc = len + 32;
        char *nv = realloc(c->value, (size_t)nc);
        if (!nv) return;
        c->value = nv;
        c->vcap = nc;
    }
    if (len) memcpy(c->value, s, (size_t)len);
    c->value[len] = 0;
    c->vlen = len;
    if (c->caret > len) c->caret = len;
}

static web_ctrl_t *new_ctrl(web_page_t *pg, web_node_t *n) {
    if (pg->nctrls >= pg->ccap) {
        int nc = pg->ccap ? pg->ccap * 2 : 16;
        web_ctrl_t *ncs = realloc(pg->ctrls, sizeof *ncs * nc);
        if (!ncs) return NULL;
        pg->ctrls = ncs;
        pg->ccap = nc;
    }
    web_ctrl_t *c = &pg->ctrls[pg->nctrls];
    memset(c, 0, sizeof *c);
    c->node = n;
    c->form = n->form;
    c->name = dupstr(web_attr(n, "name"));
    c->disabled = web_has_attr(n, "disabled");
    c->readonly = web_has_attr(n, "readonly");
    c->img = -1;
    n->ctl = pg->nctrls++;
    return c;
}

/* raw text inside an element (for <textarea>: newlines kept) */
static void raw_text(const web_node_t *n, char *out, size_t size) {
    size_t o = 0;
    for (const web_node_t *c = n->first; c && o + 1 < size; c = c->next)
        if (c->type == WN_TEXT)
            for (int i = 0; i < c->len && o + 1 < size; i++) if (c->text[i] != '\r') out[o++] = c->text[i];
    out[o] = 0;
}

static void collect_options(web_ctrl_t *c, web_node_t *n, char *buf, int depth) {
    for (web_node_t *o = n->first; o; o = o->next) {
        if (o->type != WN_ELEM) continue;
        if (o->tag == T_OPTGROUP && depth < 2) { collect_options(c, o, buf, depth + 1); continue; }
        if (o->tag != T_OPTION || c->nopts >= 500) continue;
        char **nt = realloc(c->opt_text, sizeof(char *) * (c->nopts + 1));
        if (!nt) return;
        c->opt_text = nt;
        char **nv = realloc(c->opt_val, sizeof(char *) * (c->nopts + 1));
        if (!nv) return;
        c->opt_val = nv;
        web_text_content(o, buf, 512);
        const char *lab = web_attr(o, "label");
        c->opt_text[c->nopts] = dupstr(lab && *lab ? lab : buf);
        const char *v = web_attr(o, "value");
        c->opt_val[c->nopts] = dupstr(v ? v : buf);
        if (web_has_attr(o, "selected")) c->sel = c->nopts;
        c->nopts++;
    }
}

static void collect_ctrl(web_page_t *pg, web_node_t *n) {
    char *buf = malloc(8192);
    if (!buf) return;
    web_ctrl_t *c = NULL;
    if (n->tag == T_INPUT) {
        const char *ty = web_attr(n, "type");
        if (!ty) ty = "text";
        int type = C_TEXT;
        if (!strcasecmp(ty, "password")) type = C_PASSWORD;
        else if (!strcasecmp(ty, "checkbox")) type = C_CHECKBOX;
        else if (!strcasecmp(ty, "radio")) type = C_RADIO;
        else if (!strcasecmp(ty, "submit")) type = C_SUBMIT;
        else if (!strcasecmp(ty, "reset")) type = C_RESET;
        else if (!strcasecmp(ty, "button")) type = C_BUTTON;
        else if (!strcasecmp(ty, "hidden")) type = C_HIDDEN;
        else if (!strcasecmp(ty, "image")) type = C_IMAGE;
        else if (!strcasecmp(ty, "file")) type = C_FILE;
        c = new_ctrl(pg, n);
        if (!c) { free(buf); return; }
        c->type = type;
        const char *v = web_attr(n, "value");
        switch (type) {
        case C_CHECKBOX: case C_RADIO:
            web_ctrl_set_value(c, v ? v : "on", -1);
            c->checked = c->init_checked = web_has_attr(n, "checked");
            break;
        case C_SUBMIT: case C_RESET: case C_BUTTON: case C_IMAGE: case C_FILE: {
            const char *def = type == C_SUBMIT ? "Submit" : type == C_RESET ? "Reset" :
                              type == C_FILE ? "Choose file..." : "";
            if (type == C_IMAGE) { const char *alt = web_attr(n, "alt"); def = alt && *alt ? alt : "Submit"; }
            strlcpy(c->label, (v && type != C_FILE && type != C_IMAGE) ? v : def, sizeof c->label);
            web_ctrl_set_value(c, v ? v : "", -1);
            if (type == C_FILE) c->disabled = true;
            break;
        }
        default:
            web_ctrl_set_value(c, v ? v : "", -1);
            c->size = atoi(web_attr(n, "size") ? web_attr(n, "size") : "0");
            c->maxlen = atoi(web_attr(n, "maxlength") ? web_attr(n, "maxlength") : "0");
            break;
        }
    } else if (n->tag == T_BUTTON) {
        c = new_ctrl(pg, n);
        if (!c) { free(buf); return; }
        const char *ty = web_attr(n, "type");
        c->type = !ty || !strcasecmp(ty, "submit") ? C_SUBMIT : !strcasecmp(ty, "reset") ? C_RESET : C_BUTTON;
        web_text_content(n, buf, 8192);
        if (!buf[0]) { const char *t = web_attr(n, "title"); if (!t) t = web_attr(n, "aria-label"); strlcpy(buf, t ? t : "", 8192); }
        strlcpy(c->label, buf, sizeof c->label);
        const char *v = web_attr(n, "value");
        web_ctrl_set_value(c, v ? v : "", -1);
    } else if (n->tag == T_SELECT) {
        c = new_ctrl(pg, n);
        if (!c) { free(buf); return; }
        c->type = C_SELECT;
        collect_options(c, n, buf, 0);
        c->init_sel = c->sel;
    } else if (n->tag == T_TEXTAREA) {
        c = new_ctrl(pg, n);
        if (!c) { free(buf); return; }
        c->type = C_TEXTAREA;
        raw_text(n, buf, 8192);
        web_ctrl_set_value(c, buf, -1);
        c->rows = atoi(web_attr(n, "rows") ? web_attr(n, "rows") : "2");
        c->cols = atoi(web_attr(n, "cols") ? web_attr(n, "cols") : "20");
        c->maxlen = atoi(web_attr(n, "maxlength") ? web_attr(n, "maxlength") : "0");
    }
    if (c) {
        c->init_value = dupstr(c->value ? c->value : "");
        c->caret = c->vlen;
        if (c->type == C_IMAGE) {
            const char *src = web_attr(n, "src");
            if (src && *src) c->img = add_image(pg, src);
        }
    }
    free(buf);
}

static void collect_rec(web_page_t *pg, web_node_t *n, int depth) {
    if (depth > 200) return;
    for (web_node_t *c = n->first; c; c = c->next) {
        if (c->type != WN_ELEM) continue;
        switch (c->tag) {
        case T_A: case T_AREA: {
            const char *h = web_attr(c, "href");
            if (h) c->link = add_link(pg, c, h);
            break;
        }
        case T_IMG: {
            const char *src = img_src(c);
            if (src) c->img = add_image(pg, src);
            break;
        }
        case T_INPUT: case T_BUTTON: case T_SELECT: case T_TEXTAREA:
            collect_ctrl(pg, c);
            if (c->tag == T_SELECT || c->tag == T_TEXTAREA) continue;
            break;
        default:
            break;
        }
        collect_rec(pg, c, depth + 1);
    }
}

void web_page_collect(web_page_t *pg) {
    if (!pg->doc || !pg->doc->root) return;
    collect_rec(pg, pg->doc->root, 0);
}

void web_page_free_resources(web_page_t *pg) {
    for (int i = 0; i < pg->nimgs; i++) {
        web_image_t *im = &pg->imgs[i];
        free(im->url);
        if (im->surf) surface_destroy(im->surf);
        if (im->scaled) surface_destroy(im->scaled);
        free(im->data);
    }
    free(pg->imgs);
    pg->imgs = NULL;
    pg->nimgs = pg->icap = 0;
    for (int i = 0; i < pg->nctrls; i++) {
        web_ctrl_t *c = &pg->ctrls[i];
        free(c->name);
        free(c->value);
        free(c->init_value);
        for (int k = 0; k < c->nopts; k++) { free(c->opt_text[k]); free(c->opt_val[k]); }
        free(c->opt_text);
        free(c->opt_val);
    }
    free(pg->ctrls);
    pg->ctrls = NULL;
    pg->nctrls = pg->ccap = 0;
    free(pg->links);
    pg->links = NULL;
    pg->nlinks = pg->lcap = 0;
    web_layout_free(&pg->lay);
    web_doc_free(pg->doc);
    pg->doc = NULL;
}

/* application/x-www-form-urlencoded, the page's 1252 text sent as UTF-8 */
void web_urlencode(const char *s, char *out, size_t size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = strlen(out);
    for (; *s && o + 13 < size; s++) {
        unsigned c = (unsigned char)*s;
        if (isalnum((int)c) || c == '*' || c == '-' || c == '.' || c == '_') { out[o++] = (char)c; continue; }
        if (c == ' ') { out[o++] = '+'; continue; }
        if (c == '\n') { memcpy(out + o, "%0D%0A", 6); o += 6; continue; }
        uint32_t cp = web_1252_to_cp((int)c);
        uint8_t u[4];
        int n = 0;
        if (cp < 0x80) u[n++] = (uint8_t)cp;
        else if (cp < 0x800) { u[n++] = (uint8_t)(0xC0 | (cp >> 6)); u[n++] = (uint8_t)(0x80 | (cp & 0x3F)); }
        else { u[n++] = (uint8_t)(0xE0 | (cp >> 12)); u[n++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); u[n++] = (uint8_t)(0x80 | (cp & 0x3F)); }
        for (int k = 0; k < n; k++) { out[o++] = '%'; out[o++] = hex[u[k] >> 4]; out[o++] = hex[u[k] & 15]; }
    }
    out[o] = 0;
}
