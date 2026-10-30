/*
 * web_layout.c - the Web Browser's layout engine.
 *
 * Turns a styled document (web_style_doc) into a display list of rectangles,
 * text runs, images, form controls and list markers in document coordinates.
 *
 *   - block flow with vertical margin collapsing (siblings, parent / first and
 *     last child, empty blocks), widths, auto margins (centring), max/min-width
 *   - inline formatting: words broken into lines at spaces (white-space normal,
 *     nowrap, pre, pre-wrap, pre-line), text-align, text-indent, line-height,
 *     vertical-align, inline backgrounds, inline margins / padding; adjacent
 *     words with the same style are merged into one text run
 *   - floats (left / right, clear), inline-blocks and shrink-to-fit widths,
 *     display:flex rows (items placed like inline-blocks)
 *   - lists with bullets, numbers, letters and roman numerals
 *   - tables: rows / cells / row groups, colspan, rowspan, column widths from
 *     the cells' minimum / maximum content widths, border-spacing, vertical
 *     alignment, cell and row backgrounds
 *   - images and form controls as replaced inline boxes
 *
 * Boxes that are laid out before their final position is known (inline-blocks,
 * floats, table cells) are laid out at a provisional position and their
 * display-list items are moved afterwards.
 */
#include "web.h"

#define MAX_DEPTH 110

typedef struct flt { int x, y, w, h; uint8_t right; } flt_t;

typedef struct marker {
    bool on;
    uint8_t type;
    int num;
    const font_t *font;
    uint32_t color;
    int x, fs;
} marker_t;

typedef struct lctx {
    web_page_t *pg;
    web_layout_t *L;
    int vw, vh, zoom;
    uint32_t gen;
    flt_t *fl;
    int nfl, flcap, flbase;
    marker_t mk;
    int link;
    uint32_t ibg;
    int depth;
    int last_base;               /* baseline of the last line laid out          */
    const web_node_t *skip_bg;   /* body whose background became the canvas     */
} lctx_t;

typedef struct bres { int h, w, mb; } bres_t;

static uint32_t g_gen;

/* The kernel stack is shared by everything (256 KiB): deeply nested pages stop
   being laid out long before it runs out. */
static uintptr_t stack_base;
#define LAYOUT_STACK_MAX (120 * 1024)
static bool stack_low(void) {
    char here;
    return stack_base - (uintptr_t)&here > LAYOUT_STACK_MAX;
}

static int layout_box(lctx_t *lc, web_node_t *n, int x, int y, int cbw, int fw, bres_t *r, int listnum);
static int block_children(lctx_t *lc, web_node_t *n, int cx, int cy, int cw, bool ctop, bool cbot, int *trailing);
static void intrinsic(lctx_t *lc, web_node_t *n, int *mn, int *mx);
static int run_ifc(lctx_t *lc, web_node_t *container, web_node_t *from, web_node_t *to, int cx, int y, int cw, bool *any);
static void flex_intrinsic(lctx_t *lc, web_node_t *box, int *mn, int *mx);
static bool is_flex_row(const web_style_t *st);

/* ---- helpers --------------------------------------------------------------------- */
static int rlen(web_len_t l, int base) {
    if (l.u == U_PX) return l.v;
    if (l.u == U_PCT) return (int)((int64_t)l.v * base / 1000);
    return 0;
}

static bool is_auto(web_len_t l) { return l.u == U_AUTO; }

static int collapse(int a, int b) {
    if (a >= 0 && b >= 0) return a > b ? a : b;
    if (a < 0 && b < 0) return a < b ? a : b;
    return a + b;
}

static const font_t *sfont(lctx_t *lc, const web_style_t *st) {
    int f = (st->bold ? WF_BOLD : 0) | (st->italic ? WF_ITALIC : 0) | (st->mono ? WF_MONO : 0);
    int px = st->fsize;
    /* browsers draw "monospace" at 13px when the size is the default 16px */
    if (st->mono && px == 16 * lc->zoom / 100) px = 13 * lc->zoom / 100;
    return web_font(px, f);
}

static void text_metrics(const web_style_t *st, const font_t *f, int *asc, int *desc) {
    int fh = font_height(f), fa = font_ascent(f);
    int lh = st->lh > 0 ? st->lh : st->lh < 0 ? (-st->lh * st->fsize + 50) / 100 : fh;
    int lead = lh - fh;
    *asc = fa + lead / 2;
    *desc = fh - fa + (lead - lead / 2);
}

static web_item_t *item_add(lctx_t *lc, int kind) {
    web_layout_t *L = lc->L;
    if (L->nitems >= L->cap) {
        int nc = L->cap ? L->cap * 2 : 1024;
        web_item_t *ni = realloc(L->items, sizeof *ni * nc);
        if (!ni) return NULL;
        L->items = ni;
        L->cap = nc;
    }
    web_item_t *it = &L->items[L->nitems++];
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t)kind;
    it->link = -1;
    return it;
}

static void rect_item(lctx_t *lc, int x, int y, int w, int h, uint32_t c) {
    if (w <= 0 || h <= 0 || !COLOR_A(c)) return;
    web_item_t *it = item_add(lc, WI_RECT);
    if (!it) return;
    it->x = x; it->y = y; it->w = w; it->h = h; it->color = c;
}

/* reserve a background rect now (painted under the content), fill it in later */
static int reserve_bg(lctx_t *lc, uint32_t c) {
    if (!COLOR_A(c)) return -1;
    web_item_t *it = item_add(lc, WI_RECT);
    if (!it) return -1;
    it->color = c;
    return lc->L->nitems - 1;
}

static void fill_bg(lctx_t *lc, int idx, int x, int y, int w, int h) {
    if (idx < 0) return;
    web_item_t *it = &lc->L->items[idx];
    it->x = x; it->y = y; it->w = w > 0 ? w : 0; it->h = h > 0 ? h : 0;
}

static void anchor_add(lctx_t *lc, const char *name, int y) {
    web_layout_t *L = lc->L;
    if (!name || !*name) return;
    if (L->nanchors >= L->acap) {
        int nc = L->acap ? L->acap * 2 : 64;
        web_anchor_t *na = realloc(L->anchors, sizeof *na * nc);
        if (!na) return;
        L->anchors = na;
        L->acap = nc;
    }
    L->anchors[L->nanchors].name = name;
    L->anchors[L->nanchors].y = y;
    L->anchors[L->nanchors].mark = L->nitems;
    L->nanchors++;
}

static void shift_items(lctx_t *lc, int from, int to, int dx, int dy) {
    web_layout_t *L = lc->L;
    if (dx == 0 && dy == 0) return;
    for (int i = from; i < to; i++) { L->items[i].x += dx; L->items[i].y += dy; }
    /* anchors were added in increasing mark order */
    int lo = 0, hi = L->nanchors;
    while (lo < hi) { int m = (lo + hi) / 2; if (L->anchors[m].mark < from) lo = m + 1; else hi = m; }
    for (int i = lo; i < L->nanchors && L->anchors[i].mark < to; i++) L->anchors[i].y += dy;
}

static void borders(lctx_t *lc, const web_style_t *st, int x, int y, int w, int h) {
    if (st->visibility_hidden) return;
    if (st->bw[0]) rect_item(lc, x, y, w, st->bw[0], st->bc[0]);
    if (st->bw[2]) rect_item(lc, x, y + h - st->bw[2], w, st->bw[2], st->bc[2]);
    if (st->bw[3]) rect_item(lc, x, y + st->bw[0], st->bw[3], h - st->bw[0] - st->bw[2], st->bc[3]);
    if (st->bw[1]) rect_item(lc, x + w - st->bw[1], y + st->bw[0], st->bw[1], h - st->bw[0] - st->bw[2], st->bc[1]);
}

static bool displayed(const web_node_t *n) {
    return n->type == WN_ELEM && n->st && n->st->display != D_NONE;
}

static bool ws_only(const web_node_t *t) {
    for (int i = 0; i < t->len; i++) if (!web_isspace((unsigned char)t->text[i])) return false;
    return true;
}

static bool has_block_child(const web_node_t *n);

static bool block_level(const web_node_t *n) {
    const web_style_t *st = n->st;
    switch (st->display) {
    case D_BLOCK: case D_LIST_ITEM: case D_TABLE: case D_FLEX: case D_GRID:
    case D_ROW_GROUP: case D_HEADER_GROUP: case D_FOOTER_GROUP: case D_ROW: case D_CELL:
    case D_CAPTION: case D_COLUMN: case D_COLUMN_GROUP:
        return true;
    case D_CONTENTS:
        return has_block_child(n);
    default:
        return false;
    }
}

static bool has_block_child(const web_node_t *n) {
    for (const web_node_t *c = n->first; c; c = c->next)
        if (displayed(c) && c->st->display != D_CONTENTS && block_level(c)) return true;
    return false;
}

static bool is_float(const web_node_t *n) {
    return n->st->floating && n->tag != T_TD && n->tag != T_TH && n->tag != T_TR;
}

/* ---- floats ---------------------------------------------------------------------- */
static void fl_bounds(lctx_t *lc, int y, int h, int cx, int cw, int *lx, int *aw) {
    int l = cx, r = cx + cw;
    if (h < 1) h = 1;
    for (int i = lc->flbase; i < lc->nfl; i++) {
        const flt_t *f = &lc->fl[i];
        if (f->y >= y + h || f->y + f->h <= y) continue;
        if (!f->right) { if (f->x + f->w > l) l = f->x + f->w; }
        else if (f->x < r) r = f->x;
    }
    if (l > cx + cw) l = cx + cw;
    *lx = l;
    *aw = r - l > 0 ? r - l : 0;
}

/* lowest bottom edge > y of the floats overlapping [y, y+h), or -1 */
static int fl_next_bottom(lctx_t *lc, int y, int h) {
    int best = -1;
    if (h < 1) h = 1;
    for (int i = lc->flbase; i < lc->nfl; i++) {
        const flt_t *f = &lc->fl[i];
        if (f->y >= y + h || f->y + f->h <= y) continue;
        int b = f->y + f->h;
        if (b > y && (best < 0 || b < best)) best = b;
    }
    return best;
}

static int fl_clear_y(lctx_t *lc, int y, int side) {
    for (int i = lc->flbase; i < lc->nfl; i++) {
        const flt_t *f = &lc->fl[i];
        if ((side & 1) && !f->right && f->y + f->h > y) y = f->y + f->h;
        if ((side & 2) && f->right && f->y + f->h > y) y = f->y + f->h;
    }
    return y;
}

static void fl_add(lctx_t *lc, int x, int y, int w, int h, bool right) {
    if (lc->nfl >= lc->flcap) {
        int nc = lc->flcap ? lc->flcap * 2 : 16;
        flt_t *nf = realloc(lc->fl, sizeof *nf * nc);
        if (!nf) return;
        lc->fl = nf;
        lc->flcap = nc;
    }
    lc->fl[lc->nfl++] = (flt_t){ x, y, w, h, right };
}

/* ---- box metrics ------------------------------------------------------------------ */
typedef struct bm { int ml, mr, mt, mb, bl, br, bt, bb, pl, pr, pt, pb; bool aml, amr; } bm_t;

static void box_metrics(const web_style_t *st, int cbw, bm_t *m) {
    m->aml = is_auto(st->margin[3]);
    m->amr = is_auto(st->margin[1]);
    m->mt = rlen(st->margin[0], cbw);
    m->mr = rlen(st->margin[1], cbw);
    m->mb = rlen(st->margin[2], cbw);
    m->ml = rlen(st->margin[3], cbw);
    m->pt = rlen(st->padding[0], cbw);
    m->pr = rlen(st->padding[1], cbw);
    m->pb = rlen(st->padding[2], cbw);
    m->pl = rlen(st->padding[3], cbw);
    m->bt = st->bw[0]; m->br = st->bw[1]; m->bb = st->bw[2]; m->bl = st->bw[3];
}

/* ---- list markers ----------------------------------------------------------------- */
static void roman(int n, bool upper, char *out, size_t size) {
    static const int val[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
    static const char *sym[] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
    size_t o = 0;
    if (n <= 0 || n >= 4000) { snprintf(out, size, "%d", n); return; }
    for (int i = 0; i < 13 && o + 4 < size; i++)
        while (n >= val[i] && o + 4 < size) {
            for (const char *s = sym[i]; *s; s++) out[o++] = upper ? (char)toupper((unsigned char)*s) : *s;
            n -= val[i];
        }
    out[o] = 0;
}

static void emit_marker(lctx_t *lc, int base) {
    marker_t *m = &lc->mk;
    if (!m->on) return;
    m->on = false;
    if (m->type == LS_NONE) return;
    if (m->type <= LS_SQUARE) {
        int sz = m->fs * 36 / 100;
        if (sz < 4) sz = 4;
        web_item_t *it = item_add(lc, WI_MARKER);
        if (!it) return;
        it->marker = m->type;
        it->w = it->h = sz;
        it->x = m->x - sz - m->fs / 2;
        it->y = base - m->fs * 33 / 100 - sz / 2;
        it->color = m->color;
        return;
    }
    char buf[24];
    int n = m->num;
    switch (m->type) {
    case LS_LOWER_ALPHA: case LS_UPPER_ALPHA: {
        char tmp[8];
        int k = 0;
        if (n < 1) n = 1;
        while (n > 0 && k < 7) { n--; tmp[k++] = (char)((m->type == LS_UPPER_ALPHA ? 'A' : 'a') + n % 26); n /= 26; }
        for (int i = 0; i < k; i++) buf[i] = tmp[k - 1 - i];
        buf[k] = 0;
        break;
    }
    case LS_LOWER_ROMAN: case LS_UPPER_ROMAN: roman(n, m->type == LS_UPPER_ROMAN, buf, sizeof buf - 2); break;
    default: snprintf(buf, sizeof buf - 2, "%d", n); break;
    }
    strcat(buf, ".");
    int len = (int)strlen(buf);
    char *t = wa_strndup(&lc->L->ar, buf, (size_t)len);
    if (!t) return;
    int w = font_text_width_n(m->font, t, len);
    web_item_t *it = item_add(lc, WI_TEXT);
    if (!it) return;
    it->text = t;
    it->len = len;
    it->font = m->font;
    it->color = m->color;
    it->x = m->x - w - m->fs / 3;
    it->y = base - font_ascent(m->font);
    it->w = w;
    it->h = font_height(m->font);
    it->ref = font_ascent(m->font);
}

/* ---- replaced elements -------------------------------------------------------------- */
static void image_box(lctx_t *lc, const web_node_t *n, int cbw, int *ow, int *oh) {
    const web_style_t *st = n->st;
    web_image_t *im = n->img >= 0 && n->img < lc->pg->nimgs ? &lc->pg->imgs[n->img] : NULL;
    int nw = 0, nh = 0;
    if (im && im->surf) {
        nw = im->surf->w * lc->zoom / 100;
        nh = im->surf->h * lc->zoom / 100;
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
    }
    int w = -1, h = -1;
    if (st->width.u == U_PX) w = st->width.v;
    else if (st->width.u == U_PCT) w = rlen(st->width, cbw);
    if (st->height.u == U_PX) h = st->height.v;
    if (w >= 0 && h < 0) h = nw ? (int)((int64_t)w * nh / nw) : 0;
    else if (h >= 0 && w < 0) w = nh ? (int)((int64_t)h * nw / nh) : 0;
    else if (w < 0 && h < 0) { w = nw; h = nh; }
    if (st->maxw.u != U_NONE) {
        int mx = rlen(st->maxw, cbw);
        if (mx > 0 && w > mx) { if (w > 0) h = (int)((int64_t)h * mx / w); w = mx; }
    }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    *ow = w;
    *oh = h;
}

static int avg_char_w(const font_t *f) {
    return (font_text_width(f, "abcdefghijklmnopqrstuvwxyz") + 13) / 26;
}

static void ctrl_box(lctx_t *lc, web_ctrl_t *c, const web_style_t *st, int cbw, int *ow, int *oh) {
    int z = lc->zoom;
    const font_t *f = web_font(st->fsize, st->bold ? WF_BOLD : 0);
    if (c->type == C_TEXTAREA) f = web_font(st->fsize, WF_MONO);
    c->font = f;
    c->color = st->color;
    int fh = font_height(f);
    int w = 0, h = fh + 8 * z / 100;
    switch (c->type) {
    case C_TEXT: case C_PASSWORD:
        w = (c->size > 0 ? c->size : 20) * avg_char_w(f) + 10 * z / 100;
        break;
    case C_CHECKBOX: case C_RADIO:
        w = h = 13 * z / 100 + 6;
        break;
    case C_SUBMIT: case C_RESET: case C_BUTTON: case C_FILE:
    case C_IMAGE:
        w = font_text_width(f, c->label) + 16 * z / 100;
        break;
    case C_SELECT: {
        int mw = 0;
        for (int i = 0; i < c->nopts; i++) { int tw = font_text_width(f, c->opt_text[i]); if (tw > mw) mw = tw; }
        w = mw + 28 * z / 100;
        break;
    }
    case C_TEXTAREA:
        w = (c->cols > 0 ? c->cols : 20) * font_char_width(f, 'm') + 24 * z / 100;
        h = (c->rows > 0 ? c->rows : 2) * fh + 8 * z / 100;
        break;
    default:
        break;
    }
    if (c->type != C_CHECKBOX && c->type != C_RADIO) {
        if (st->width.u == U_PX) w = st->width.v;
        else if (st->width.u == U_PCT) w = rlen(st->width, cbw);
        if (st->height.u == U_PX && st->height.v > 4) h = st->height.v;
        if (st->maxw.u != U_NONE) { int mx = rlen(st->maxw, cbw); if (mx > 0 && w > mx) w = mx; }
    }
    if (w < 4) w = 4;
    *ow = w;
    *oh = h;
}

/* ---- measuring (min / max content widths) ---------------------------------------------- */
typedef struct meas {
    int min, max, line;
    int run;                 /* width since the last break opportunity */
    bool pend;               /* a space is pending                     */
    bool brk;                /* ... and it is a break opportunity       */
} meas_t;

static void meas_flush(meas_t *m) {
    if (m->line > m->max) m->max = m->line;
    m->line = 0;
    m->run = 0;
    m->pend = m->brk = false;
}

static void meas_add(meas_t *m, int w, int minw) {
    m->line += w;
    if (minw > m->min) m->min = minw;
    if (m->line > m->max) m->max = m->line;
}

/* unbreakable content of width w; sp = the pending space's width */
static void meas_glue(meas_t *m, int w, int sp) {
    if (m->pend && m->brk) m->run = 0;
    else m->run += sp;
    m->run += w;
    if (m->run > m->min) m->min = m->run;
    m->line += sp + w;
    if (m->line > m->max) m->max = m->line;
    m->pend = m->brk = false;
}

/* an atomic box (image, inline-block): break opportunities around it */
static void meas_atom(meas_t *m, int w, int minw) {
    m->run = 0;
    m->pend = m->brk = false;
    meas_add(m, w, minw);
}

static void meas_text(lctx_t *lc, meas_t *m, const web_node_t *t, const web_style_t *st) {
    const font_t *f = sfont(lc, st);
    int spw = font_char_width(f, ' ');
    const char *s = t->text;
    int n = t->len, ws = st->ws;
    if (ws == WS_PRE || ws == WS_PRE_WRAP) {
        int i = 0;
        while (i < n) {
            if (s[i] == '\n') { meas_flush(m); i++; continue; }
            int j = i;
            while (j < n && s[j] != '\n') j++;
            int w = 0;
            for (int k = i; k < j; k++) w += s[k] == '\t' ? spw * 8 : font_char_width(f, (unsigned char)s[k]);
            int mw = w;
            if (ws == WS_PRE_WRAP) {   /* longest word */
                mw = 0;
                int cur = 0;
                for (int k = i; k <= j; k++) {
                    if (k == j || s[k] == ' ' || s[k] == '\t') { if (cur > mw) mw = cur; cur = 0; }
                    else cur += font_char_width(f, (unsigned char)s[k]);
                }
            }
            meas_add(m, w, ws == WS_PRE ? m->line + w : mw);
            i = j;
        }
        return;
    }
    int i = 0;
    while (i < n) {
        if (web_isspace((unsigned char)s[i])) {
            if (ws == WS_PRE_LINE && s[i] == '\n') meas_flush(m);
            else { m->pend = true; if (ws != WS_NOWRAP) m->brk = true; }
            i++;
            continue;
        }
        int j = i;
        while (j < n && !web_isspace((unsigned char)s[j])) j++;
        int w = font_text_width_n(f, s + i, j - i);
        if (st->transform == TT_UPPER) w = w * 115 / 100;
        int sp = (m->pend && m->line) ? spw : 0;
        meas_glue(m, w, sp);
        i = j;
    }
}

static void meas_children(lctx_t *lc, meas_t *m, web_node_t *n, int depth);

static void meas_node(lctx_t *lc, meas_t *m, web_node_t *c, const web_style_t *pst, int depth) {
    if (c->type == WN_TEXT) { meas_text(lc, m, c, pst); return; }
    if (!displayed(c) || depth > MAX_DEPTH || stack_low()) return;
    const web_style_t *st = c->st;
    if (c->tag == T_BR) { meas_flush(m); return; }
    if (c->tag == T_IMG) {
        int w, h;
        image_box(lc, c, 0, &w, &h);
        if (st->width.u == U_PCT) w = 0;
        bm_t b;
        box_metrics(st, 0, &b);
        w += b.ml + b.mr;
        meas_atom(m, w, w);
        return;
    }
    if (c->ctl >= 0 && c->tag != T_BUTTON) {
        web_ctrl_t *ct = &lc->pg->ctrls[c->ctl];
        if (ct->type == C_HIDDEN) return;
        int w, h;
        ctrl_box(lc, ct, st, 0, &w, &h);
        if (st->width.u == U_PCT) w = 40;
        meas_atom(m, w + 4, w + 4);
        return;
    }
    if (c->ctl >= 0 && c->tag == T_BUTTON) {
        web_ctrl_t *ct = &lc->pg->ctrls[c->ctl];
        int w, h;
        ctrl_box(lc, ct, st, 0, &w, &h);
        meas_atom(m, w + 4, w + 4);
        return;
    }
    if (block_level(c) || is_float(c)) {
        int mn, mx;
        intrinsic(lc, c, &mn, &mx);
        if (is_float(c)) { meas_atom(m, mx, mn); return; }
        meas_flush(m);
        if (mn > m->min) m->min = mn;
        if (mx > m->max) m->max = mx;
        return;
    }
    int d = st->display;
    if (d == D_INLINE_BLOCK || d == D_INLINE_TABLE || d == D_INLINE_FLEX) {
        int mn, mx;
        intrinsic(lc, c, &mn, &mx);
        meas_atom(m, mx, mn);
        return;
    }
    /* inline */
    bm_t b;
    box_metrics(st, 0, &b);
    int lpad = b.ml + b.bl + b.pl, rpad = b.mr + b.br + b.pr;
    if (lpad > 0) meas_glue(m, lpad, (m->pend && m->line) ? font_char_width(sfont(lc, st), ' ') : 0);
    meas_children(lc, m, c, depth + 1);
    if (rpad > 0) meas_glue(m, rpad, 0);
}

static void meas_children(lctx_t *lc, meas_t *m, web_node_t *n, int depth) {
    for (web_node_t *c = n->first; c; c = c->next) meas_node(lc, m, c, n->st, depth);
}

/* ---- tables -------------------------------------------------------------------------- */
typedef struct tcell {
    web_node_t *n;
    int r, c, rs, cs;
    int mn, mx, fix, pct;
    int h, ch, first, last, bgi;
    int x, w;
} tcell_t;

typedef struct tbl {
    tcell_t *cell;
    int ncell, ccap;
    web_node_t **row;
    int nrow, rcap;
    int ncol;
    int *cmin, *cmax, *cfix, *cpct, *cw;
    int sp;
    web_node_t *caption;
} tbl_t;

#define MAXCOLS 200

static void tbl_add_row(tbl_t *t, web_node_t *r) {
    if (t->nrow >= t->rcap) {
        int nc = t->rcap ? t->rcap * 2 : 32;
        web_node_t **nr = realloc(t->row, sizeof *nr * nc);
        if (!nr) return;
        t->row = nr;
        t->rcap = nc;
    }
    t->row[t->nrow++] = r;
}

static void tbl_gather(tbl_t *t, web_node_t *n, int depth) {
    for (web_node_t *c = n->first; c; c = c->next) {
        if (!displayed(c)) continue;
        int d = c->st->display;
        if (d == D_ROW) tbl_add_row(t, c);
        else if ((d == D_ROW_GROUP || d == D_HEADER_GROUP || d == D_FOOTER_GROUP) && depth < 2) tbl_gather(t, c, depth + 1);
        else if (d == D_CAPTION && !t->caption) t->caption = c;
    }
}

static int attr_int(const web_node_t *n, const char *name, int def, int lo, int hi) {
    const char *a = web_attr(n, name);
    if (!a || !*a) return def;
    int v = atoi(a);
    if (v < lo) v = def < lo ? lo : def;
    return v > hi ? hi : v;
}

static void tbl_free(tbl_t *t) {
    free(t->cell); free(t->row);
    free(t->cmin); free(t->cmax); free(t->cfix); free(t->cpct); free(t->cw);
    memset(t, 0, sizeof *t);
}

/* Build the cell grid and the columns' min / max widths. */
static bool tbl_build(lctx_t *lc, tbl_t *t, web_node_t *n, int cbw) {
    memset(t, 0, sizeof *t);
    tbl_gather(t, n, 0);
    t->sp = n->st->spacing;
    if (!t->nrow) return true;
    int *occ = calloc(MAXCOLS, sizeof(int));
    if (!occ) return false;
    for (int r = 0; r < t->nrow; r++) {
        int col = 0;
        for (web_node_t *c = t->row[r]->first; c; c = c->next) {
            if (!displayed(c)) continue;
            while (col < MAXCOLS && occ[col] > 0) col++;
            if (col >= MAXCOLS) break;
            int cs = attr_int(c, "colspan", 1, 1, 100), rs = attr_int(c, "rowspan", 1, 1, 200);
            if (rs > t->nrow - r) rs = t->nrow - r;
            if (col + cs > MAXCOLS) cs = MAXCOLS - col;
            if (t->ncell >= t->ccap) {
                int nc = t->ccap ? t->ccap * 2 : 64;
                tcell_t *ncl = realloc(t->cell, sizeof *ncl * nc);
                if (!ncl) { free(occ); return false; }
                t->cell = ncl;
                t->ccap = nc;
            }
            tcell_t *tc = &t->cell[t->ncell++];
            memset(tc, 0, sizeof *tc);
            tc->n = c; tc->r = r; tc->c = col; tc->rs = rs; tc->cs = cs;
            for (int k = col; k < col + cs; k++) occ[k] = rs;
            col += cs;
            if (col > t->ncol) t->ncol = col;
        }
        for (int k = 0; k < MAXCOLS; k++) if (occ[k] > 0) occ[k]--;
    }
    free(occ);
    if (!t->ncol) return true;
    t->cmin = calloc(t->ncol, sizeof(int));
    t->cmax = calloc(t->ncol, sizeof(int));
    t->cfix = calloc(t->ncol, sizeof(int));
    t->cpct = calloc(t->ncol, sizeof(int));
    t->cw = calloc(t->ncol, sizeof(int));
    if (!t->cmin || !t->cmax || !t->cfix || !t->cpct || !t->cw) return false;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < t->ncell; i++) {
            tcell_t *tc = &t->cell[i];
            if ((tc->cs == 1) != (pass == 0)) continue;
            const web_style_t *st = tc->n->st;
            if (pass == 0 || !tc->mx) intrinsic(lc, tc->n, &tc->mn, &tc->mx);
            if (st->width.u == U_PX) {
                bm_t b;
                box_metrics(st, cbw, &b);
                tc->fix = st->width.v + (st->border_box ? 0 : b.pl + b.pr + b.bl + b.br);
                if (tc->fix < tc->mn) tc->fix = tc->mn;
                if (tc->mx < tc->fix) tc->mx = tc->fix;
            } else if (st->width.u == U_PCT) tc->pct = st->width.v;
            if (tc->cs == 1) {
                int k = tc->c;
                if (tc->mn > t->cmin[k]) t->cmin[k] = tc->mn;
                if (tc->mx > t->cmax[k]) t->cmax[k] = tc->mx;
                if (tc->fix > t->cfix[k]) t->cfix[k] = tc->fix;
                if (tc->pct > t->cpct[k]) t->cpct[k] = tc->pct;
            } else {
                int smin = t->sp * (tc->cs - 1), smax = smin;
                for (int k = tc->c; k < tc->c + tc->cs; k++) { smin += t->cmin[k]; smax += t->cmax[k]; }
                if (tc->mn > smin) {
                    int d = tc->mn - smin;
                    for (int k = tc->c; k < tc->c + tc->cs; k++) t->cmin[k] += d / tc->cs + (k - tc->c < d % tc->cs);
                }
                if (tc->mx > smax) {
                    int d = tc->mx - smax;
                    for (int k = tc->c; k < tc->c + tc->cs; k++) t->cmax[k] += d / tc->cs + (k - tc->c < d % tc->cs);
                }
            }
        }
    }
    for (int k = 0; k < t->ncol; k++) {
        if (t->cfix[k] && t->cfix[k] > t->cmax[k]) t->cmax[k] = t->cfix[k];
        if (t->cmax[k] < t->cmin[k]) t->cmax[k] = t->cmin[k];
    }
    return true;
}

static void tbl_widths(tbl_t *t, int W) {
    int n = t->ncol, sum = 0;
    for (int k = 0; k < n; k++) { t->cw[k] = t->cmin[k]; sum += t->cw[k]; }
    int inner = W - t->sp * (n + 1);
    int extra = inner - sum;
    if (extra <= 0) return;
    /* percentage and fixed columns towards their target */
    for (int k = 0; k < n && extra > 0; k++) {
        int target = 0;
        if (t->cpct[k]) target = (int)((int64_t)inner * t->cpct[k] / 1000);
        else if (t->cfix[k]) target = t->cfix[k];
        if (target > t->cw[k]) { int d = MIN(target - t->cw[k], extra); t->cw[k] += d; extra -= d; }
    }
    /* auto columns towards their max width */
    int64_t want = 0;
    for (int k = 0; k < n; k++) if (!t->cpct[k] && !t->cfix[k] && t->cmax[k] > t->cw[k]) want += t->cmax[k] - t->cw[k];
    if (want > 0 && extra > 0) {
        int give = (int)MIN((int64_t)extra, want), given = 0;
        for (int k = 0; k < n; k++) {
            if (t->cpct[k] || t->cfix[k] || t->cmax[k] <= t->cw[k]) continue;
            int d = (int)((int64_t)give * (t->cmax[k] - t->cw[k]) / want);
            t->cw[k] += d;
            given += d;
        }
        for (int k = 0; k < n && given < give; k++)
            if (!t->cpct[k] && !t->cfix[k] && t->cmax[k] > t->cw[k]) { t->cw[k]++; given++; }
        extra -= given;
    }
    if (extra <= 0) return;
    /* the table is wider than its content: share out the rest */
    int nauto = 0;
    int64_t wsum = 0;
    for (int k = 0; k < n; k++) if (!t->cpct[k] && !t->cfix[k]) { nauto++; wsum += t->cmax[k]; }
    bool all = nauto == 0;
    if (all) for (int k = 0; k < n; k++) wsum += t->cmax[k];
    int given = 0, cnt = all ? n : nauto;
    for (int k = 0; k < n; k++) {
        if (!all && (t->cpct[k] || t->cfix[k])) continue;
        int d = wsum > 0 ? (int)((int64_t)extra * t->cmax[k] / wsum) : extra / cnt;
        t->cw[k] += d;
        given += d;
    }
    for (int k = n - 1; k >= 0 && given < extra; k--) {
        if (!all && (t->cpct[k] || t->cfix[k])) continue;
        t->cw[k] += extra - given;
        given = extra;
    }
}

static void table_intrinsic(lctx_t *lc, web_node_t *n, int *mn, int *mx) {
    tbl_t t;
    bm_t b;
    box_metrics(n->st, 0, &b);
    int tb = b.bl + b.br + b.pl + b.pr;
    *mn = *mx = tb;
    if (tbl_build(lc, &t, n, lc->vw) && t.ncol) {
        int smin = t.sp * (t.ncol + 1), smax = smin;
        for (int k = 0; k < t.ncol; k++) { smin += t.cmin[k]; smax += t.cmax[k]; }
        *mn += smin;
        *mx += smax;
    }
    tbl_free(&t);
    if (t.caption) {
        int cmn, cmx;
        intrinsic(lc, t.caption, &cmn, &cmx);
        if (cmn > *mn) *mn = cmn;
    }
    const web_style_t *st = n->st;
    if (st->width.u == U_PX) {
        int w = st->width.v + (st->border_box ? 0 : tb);
        if (w < *mn) w = *mn;
        *mn = *mx = w;
    }
    *mn += b.ml + b.mr;
    *mx += b.ml + b.mr;
}

/* Lay out a table whose border box top is at y. */
static int layout_table(lctx_t *lc, web_node_t *n, int x, int y, int cbw, int fw, bres_t *res) {
    const web_style_t *st = n->st;
    if (stack_low()) { res->h = res->w = res->mb = 0; return 0; }
    bm_t b;
    box_metrics(st, cbw, &b);
    int tb = b.bl + b.br + b.pl + b.pr;
    tbl_t t;
    if (!tbl_build(lc, &t, n, cbw)) { tbl_free(&t); res->h = res->w = 0; res->mb = 0; return 0; }
    int smin = t.sp * (t.ncol + 1), smax = smin;
    for (int k = 0; k < t.ncol; k++) { smin += t.cmin[k]; smax += t.cmax[k]; }
    int avail = cbw - b.ml - b.mr;
    int W;   /* content width (inside border + padding) */
    if (fw >= 0) W = fw;
    else if (st->width.u == U_PX || st->width.u == U_PCT) {
        W = rlen(st->width, cbw);
        if (st->border_box || st->width.u == U_PCT) W -= tb;
    } else {
        W = MIN(smax, avail - tb);
    }
    if (t.caption) {
        int cmn, cmx;
        intrinsic(lc, t.caption, &cmn, &cmx);
        if (W < cmn && fw < 0 && st->width.u == U_AUTO) W = MIN(cmx, avail - tb);
    }
    if (W < smin) W = smin;
    if (W < 0) W = 0;
    if (t.ncol) tbl_widths(&t, W);
    int bw = W + tb;
    /* horizontal position: auto margins / legacy centring */
    int ml = b.ml;
    const web_style_t *ps = n->parent && n->parent->st ? n->parent->st : NULL;
    if (b.aml && b.amr) ml = (cbw - bw) / 2;
    else if (b.aml) ml = cbw - bw - b.mr;
    else if (ps && ps->align == TA_CENTER && ps->align_legacy && !b.aml) ml = (cbw - bw) / 2;
    else if (ps && ps->align == TA_RIGHT && ps->align_legacy) ml = cbw - bw - b.mr;
    if (ml < 0 && !(b.ml < 0)) ml = 0;
    int bx = x + ml;
    int bgi = st->visibility_hidden || n == lc->skip_bg ? -1 : reserve_bg(lc, st->bg);
    if (n->id) anchor_add(lc, n->id, y);
    int cx = bx + b.bl + b.pl, cy = y + b.bt + b.pt;
    int ty = cy;
    int saved_link = lc->link;
    if (t.caption) {
        bres_t cr;
        ty += layout_box(lc, t.caption, cx, ty, W, W, &cr, 0);
    }
    int *rh = t.nrow ? calloc(t.nrow, sizeof(int)) : NULL;
    int *ry = t.nrow ? calloc(t.nrow, sizeof(int)) : NULL;
    int *rbg = t.nrow ? calloc(t.nrow, sizeof(int)) : NULL;
    if (t.nrow && (!rh || !ry || !rbg)) { free(rh); free(ry); free(rbg); tbl_free(&t); res->h = res->w = res->mb = 0; return 0; }
    /* column x positions */
    int *colx = malloc(sizeof(int) * (size_t)(t.ncol + 1));
    if (!colx) { free(rh); free(ry); free(rbg); tbl_free(&t); res->h = res->w = res->mb = 0; return 0; }
    int px = cx + t.sp;
    for (int k = 0; k < t.ncol; k++) { colx[k] = px; px += t.cw[k] + t.sp; }
    colx[t.ncol] = px;
    ty += t.sp;
    int ci = 0;
    for (int r = 0; r < t.nrow; r++) {
        ry[r] = ty;
        const web_style_t *rst = t.row[r]->st;
        rbg[r] = rst->visibility_hidden ? -1 : reserve_bg(lc, rst->bg);
        int h = 0;
        if (rst->height.u == U_PX) h = rst->height.v;
        for (; ci < t.ncell && t.cell[ci].r == r; ci++) {
            tcell_t *tc = &t.cell[ci];
            const web_style_t *cs = tc->n->st;
            bm_t cb;
            int w = colx[tc->c + tc->cs] - t.sp - colx[tc->c];
            box_metrics(cs, w, &cb);
            tc->x = colx[tc->c];
            tc->w = w;
            tc->bgi = cs->visibility_hidden ? -1 : reserve_bg(lc, cs->bg);
            tc->first = lc->L->nitems;
            if (tc->n->id) anchor_add(lc, tc->n->id, ty);
            int ccw = w - cb.pl - cb.pr - cb.bl - cb.br;
            if (ccw < 0) ccw = 0;
            int base = lc->flbase;
            lc->flbase = lc->nfl;
            marker_t mk = lc->mk;
            lc->mk.on = false;
            int tr;
            int fmark = lc->nfl;
            int ch = block_children(lc, tc->n, tc->x + cb.bl + cb.pl, ty + cb.bt + cb.pt, ccw, false, false, &tr);
            for (int i = fmark; i < lc->nfl; i++) {
                int fb = lc->fl[i].y + lc->fl[i].h - (ty + cb.bt + cb.pt);
                if (fb > ch) ch = fb;
            }
            lc->nfl = fmark;
            lc->mk = mk;
            lc->flbase = base;
            tc->last = lc->L->nitems;
            tc->ch = ch;
            tc->h = cb.bt + cb.pt + ch + cb.pb + cb.bb;
            if (cs->height.u == U_PX) {
                int sh = cs->height.v + (cs->border_box ? 0 : cb.pt + cb.pb + cb.bt + cb.bb);
                if (sh > tc->h) tc->h = sh;
            }
            if (tc->rs == 1 && tc->h > h) h = tc->h;
        }
        /* rowspan cells ending in this row */
        for (int i = 0; i < ci; i++) {
            tcell_t *tc = &t.cell[i];
            if (tc->rs > 1 && tc->r + tc->rs - 1 == r) {
                int span = ty + h - ry[tc->r];
                if (tc->h > span) h += tc->h - span;
            }
        }
        rh[r] = h;
        ty += h + t.sp;
    }
    /* final cell boxes: vertical alignment, backgrounds, borders */
    for (int i = 0; i < t.ncell; i++) {
        tcell_t *tc = &t.cell[i];
        const web_style_t *cs = tc->n->st;
        int last = tc->r + tc->rs - 1;
        int boxh = ry[last] + rh[last] - ry[tc->r];
        int free_h = boxh - tc->h;
        if (free_h > 0) {
            int dy = cs->valign == VA_MIDDLE ? free_h / 2 : cs->valign == VA_BOTTOM ? free_h : 0;
            shift_items(lc, tc->first, tc->last, 0, dy);
        }
        fill_bg(lc, tc->bgi, tc->x, ry[tc->r], tc->w, boxh);
        borders(lc, cs, tc->x, ry[tc->r], tc->w, boxh);
    }
    for (int r = 0; r < t.nrow; r++) fill_bg(lc, rbg[r], cx + t.sp, ry[r], W - 2 * t.sp, rh[r]);
    lc->link = saved_link;
    int ch = ty - cy;
    if (st->height.u == U_PX) {
        int sh = st->height.v - (st->border_box ? b.pt + b.pb + b.bt + b.bb : 0);
        if (sh > ch) ch = sh;
    }
    int bh = b.bt + b.pt + ch + b.pb + b.bb;
    fill_bg(lc, bgi, bx, y, bw, bh);
    borders(lc, st, bx, y, bw, bh);
    free(rh); free(ry); free(rbg); free(colx);
    tbl_free(&t);
    res->h = bh;
    res->w = bw;
    res->mb = b.mb;
    return bh;
}

/* ---- intrinsic widths ---------------------------------------------------------------- */
static void intrinsic(lctx_t *lc, web_node_t *n, int *mn, int *mx) {
    if (n->mgen == lc->gen) { *mn = n->minw; *mx = n->maxw; return; }
    if (stack_low()) { *mn = *mx = 0; return; }
    const web_style_t *st = n->st;
    bm_t b;
    box_metrics(st, 0, &b);
    int mbp = b.bl + b.br + b.pl + b.pr;
    lc->depth++;
    if (lc->depth > MAX_DEPTH) { *mn = *mx = 0; }
    else if (st->display == D_TABLE || st->display == D_INLINE_TABLE) {
        table_intrinsic(lc, n, mn, mx);
        lc->depth--;
        n->mgen = lc->gen; n->minw = *mn; n->maxw = *mx;
        return;
    } else if (st->width.u == U_PX) {
        *mn = *mx = st->width.v + (st->border_box ? 0 : mbp);
    } else if (is_flex_row(st)) {
        flex_intrinsic(lc, n, mn, mx);
        *mn += mbp;
        *mx += mbp;
    } else {
        meas_t m = { 0, 0, 0, 0, false, false };
        meas_children(lc, &m, n, 0);
        meas_flush(&m);
        *mn = m.min + mbp;
        *mx = m.max + mbp;
        if (st->maxw.u == U_PX) {
            int cap = st->maxw.v + (st->border_box ? 0 : mbp);
            if (*mx > cap) *mx = cap;
            if (*mn > *mx) *mn = *mx;
        }
        if (st->minw.u == U_PX) {
            int lo = st->minw.v + (st->border_box ? 0 : mbp);
            if (*mn < lo) *mn = lo;
            if (*mx < lo) *mx = lo;
        }
    }
    lc->depth--;
    *mn += b.ml + b.mr;
    *mx += b.ml + b.mr;
    if (*mn < 0) *mn = 0;
    if (*mx < *mn) *mx = *mn;
    n->mgen = lc->gen;
    n->minw = *mn;
    n->maxw = *mx;
}

/* content width of a shrink-to-fit box (inline-block, float) in avail (margin box) */
static int shrink_to_fit(lctx_t *lc, web_node_t *n, int avail) {
    const web_style_t *st = n->st;
    bm_t b;
    box_metrics(st, avail, &b);
    int mbp = b.bl + b.br + b.pl + b.pr;
    if (st->width.u == U_PX || st->width.u == U_PCT) {
        int w = rlen(st->width, avail);
        if (st->border_box) w -= mbp;
        if (st->maxw.u != U_NONE) { int c = rlen(st->maxw, avail) - (st->border_box ? mbp : 0); if (w > c) w = c; }
        return w > 0 ? w : 0;
    }
    int mn, mx;
    intrinsic(lc, n, &mn, &mx);
    int ow = MIN(MAX(mn, avail), mx);
    int w = ow - b.ml - b.mr - mbp;
    if (st->maxw.u != U_NONE) { int c = rlen(st->maxw, avail) - (st->border_box ? mbp : 0); if (c >= 0 && w > c) w = c; }
    return w > 0 ? w : 0;
}

/* ---- inline formatting --------------------------------------------------------------- */
enum { LI_TEXT = 1, LI_BOX, LI_SIMPLE, LI_SPACE };

typedef struct litem {
    uint8_t kind, valign, deco, hidden, glued;
    uint16_t nsp;            /* spaces before (text runs)                 */
    int spw;                 /* their width                               */
    int x, w, asc, desc;
    int link;
    uint32_t color, bg;
    const font_t *font;
    const char *text;
    int len;
    int first, last;         /* LI_BOX: display-list range; LI_SIMPLE: kind, ref */
    int h, ml, mt;           /* LI_SIMPLE: box size, left / top margin + border */
    int fs;
    const web_style_t *bst;  /* LI_SIMPLE: style whose borders are drawn round the box */
} litem_t;

typedef struct ifc {
    lctx_t *lc;
    int cx, cw, y;
    int lx, aw, used;
    int align, indent;
    bool first_line, pend, wrapped;
    bool pend_brk;           /* the pending space is a break opportunity */
    int col;                 /* characters on the line (tab stops)        */
    litem_t *it;
    int n, cap;
    int sasc, sdesc;         /* strut */
    int lines;
    web_node_t *defer[8];
    int ndefer;
} ifc_t;

static void place_float(lctx_t *lc, web_node_t *c, int cx, int cw, int y);

static void ifc_bounds(ifc_t *f) {
    fl_bounds(f->lc, f->y, f->sasc + f->sdesc, f->cx, f->cw, &f->lx, &f->aw);
    if (f->first_line && f->indent) {
        f->lx += f->indent;
        f->aw -= f->indent;
        if (f->aw < 0) f->aw = 0;
    }
}

static litem_t *ifc_push(ifc_t *f) {
    if (f->n >= f->cap) {
        int nc = f->cap ? f->cap * 2 : 64;
        litem_t *ni = realloc(f->it, sizeof *ni * nc);
        if (!ni) return NULL;
        f->it = ni;
        f->cap = nc;
    }
    litem_t *it = &f->it[f->n++];
    memset(it, 0, sizeof *it);
    return it;
}

static void emit_text_run(lctx_t *lc, litem_t *a, int cnt, int x0, int base) {
    /* a[0..cnt) have the same style and follow each other */
    int total = 0;
    for (int i = 0; i < cnt; i++) total += a[i].len + (i ? a[i].nsp : 0);
    char *buf = wa_alloc(&lc->L->ar, (size_t)total + 1);
    if (!buf) return;
    int o = 0;
    for (int i = 0; i < cnt; i++) {
        if (i) for (int k = 0; k < a[i].nsp; k++) buf[o++] = ' ';
        memcpy(buf + o, a[i].text, (size_t)a[i].len);
        o += a[i].len;
    }
    buf[o] = 0;
    const font_t *f = a[0].font;
    int x = x0 + a[0].x, w = a[cnt - 1].x + a[cnt - 1].w - a[0].x;
    int ty = base - font_ascent(f);
    if (a[0].bg) rect_item(lc, x, ty, w, font_height(f), a[0].bg);
    web_item_t *it = item_add(lc, WI_TEXT);
    if (!it) return;
    it->x = x;
    it->y = ty;
    it->w = w;
    it->h = font_height(f);
    it->font = f;
    it->color = a[0].color;
    it->deco = a[0].deco;
    it->link = a[0].link;
    it->text = buf;
    it->len = o;
    it->ref = font_ascent(f);
}

/* Lay out items [0, upto) as one line; items after upto move to the next line. */
static void ifc_flush(ifc_t *f, int upto) {
    lctx_t *lc = f->lc;
    int n = upto;
    if (n <= 0) return;
    bool has_text = false;
    int asc = 0, desc = 0, toph = 0;
    for (int i = 0; i < n; i++) {
        litem_t *it = &f->it[i];
        if (it->kind == LI_SPACE) continue;
        if (it->kind == LI_TEXT) has_text = true;
        int a = it->asc, d = it->desc;
        switch (it->valign) {
        case VA_SUPER: a += it->fs / 3; d -= it->fs / 3; break;
        case VA_SUB: a -= it->fs / 5; d += it->fs / 5; break;
        case VA_TOP: case VA_BOTTOM: if (a + d > toph) toph = a + d; continue;
        case VA_MIDDLE:
            if (it->kind != LI_TEXT) { int hh = a + d; a = hh / 2 + f->sasc * 30 / 100; d = hh - a; }
            break;
        default: break;
        }
        if (a > asc) asc = a;
        if (d > desc) desc = d;
    }
    if (has_text) {
        if (f->sasc > asc) asc = f->sasc;
        if (f->sdesc > desc) desc = f->sdesc;
    }
    int lh = asc + desc;
    if (toph > lh) { desc += toph - lh; lh = toph; }
    int lastx = f->it[n - 1].x + f->it[n - 1].w;
    int free_w = f->aw - lastx, dx = 0;
    if (free_w > 0) {
        if (f->align == TA_CENTER) dx = free_w / 2;
        else if (f->align == TA_RIGHT) dx = free_w;
    }
    int x0 = f->lx + dx, base = f->y + asc;
    for (int i = 0; i < n;) {
        litem_t *it = &f->it[i];
        if (it->kind == LI_TEXT) {
            int j = i + 1;
            while (j < n && f->it[j].kind == LI_TEXT && f->it[j].font == it->font && f->it[j].color == it->color &&
                   f->it[j].deco == it->deco && f->it[j].link == it->link && f->it[j].bg == it->bg &&
                   f->it[j].hidden == it->hidden && f->it[j].valign == it->valign &&
                   f->it[j].x == f->it[j - 1].x + f->it[j - 1].w + f->it[j].spw)
                j++;
            if (!it->hidden) {
                int b = base;
                if (it->valign == VA_SUPER) b -= it->fs / 3;
                else if (it->valign == VA_SUB) b += it->fs / 5;
                emit_text_run(lc, it, j - i, x0, b);
            }
            i = j;
            continue;
        }
        if (it->kind == LI_SPACE) {
            if (it->bg && it->font && !it->hidden)
                rect_item(lc, x0 + it->x, base - font_ascent(it->font), it->w, font_height(it->font), it->bg);
            i++;
            continue;
        }
        int a = it->asc, hh = it->asc + it->desc, top;
        switch (it->valign) {
        case VA_TOP: top = f->y; break;
        case VA_BOTTOM: top = f->y + lh - hh; break;
        case VA_MIDDLE: a = hh / 2 + f->sasc * 30 / 100; top = base - a; break;
        case VA_SUPER: top = base - a - it->fs / 3; break;
        case VA_SUB: top = base - a + it->fs / 5; break;
        default: top = base - a; break;
        }
        if (it->kind == LI_BOX) {
            shift_items(lc, it->first, it->last, x0 + it->x, top);
        } else if (it->kind == LI_SIMPLE && !it->hidden) {
            if (it->bst) {
                const web_style_t *bs = it->bst;
                borders(lc, bs, x0 + it->x + it->ml - bs->bw[3], top + it->mt - bs->bw[0],
                        it->len + bs->bw[3] + bs->bw[1], it->h + bs->bw[0] + bs->bw[2]);
            }
            web_item_t *wi = item_add(lc, it->first);
            if (wi) {
                wi->x = x0 + it->x + it->ml;
                wi->y = top + it->mt;
                wi->w = it->len;
                wi->h = it->h;
                wi->ref = it->last;
                wi->link = it->link;
                wi->text = it->text;
                wi->font = it->font;
                wi->color = it->color;
            }
        }
        i++;
    }
    emit_marker(lc, base);
    lc->last_base = base;
    f->y += lh;
    f->lines++;
    f->first_line = false;
    /* carry the rest over */
    int rest = f->n - n;
    if (rest > 0) {
        memmove(f->it, f->it + n, sizeof(litem_t) * rest);
        int sx = f->it[0].x;   /* the space before the first carried item is dropped */
        f->it[0].spw = 0;
        f->it[0].nsp = 0;
        for (int i = 0; i < rest; i++) f->it[i].x -= sx;
        int used = 0;
        for (int i = 0; i < rest; i++) used = f->it[i].x + f->it[i].w;
        f->used = used;
    } else {
        f->used = 0;
    }
    f->n = rest;
    f->col = 0;
    /* floats that waited for this line */
    int nd = f->ndefer;
    f->ndefer = 0;
    for (int i = 0; i < nd; i++) place_float(lc, f->defer[i], f->cx, f->cw, f->y);
    ifc_bounds(f);
}

/* a forced line break (<br>, newline in pre) */
static void ifc_break(ifc_t *f) {
    if (f->n) ifc_flush(f, f->n);
    else {
        if (f->lc->mk.on) emit_marker(f->lc, f->y + f->sasc);
        f->y += f->sasc + f->sdesc;
        f->lines++;
        f->first_line = false;
        ifc_bounds(f);
    }
    f->pend = f->pend_brk = false;
    f->wrapped = false;
    f->col = 0;
}

static int last_break(ifc_t *f) {
    for (int k = f->n - 1; k > 0; k--) if (!f->it[k].glued) return k;
    return 0;
}

/* make room for an item of width w (space spw before it) */
static void ifc_make_room(ifc_t *f, int w, int *spw, uint16_t *nsp, bool glued, bool wrap) {
    if (f->n > 0 && wrap && f->used + *spw + w > f->aw) {
        if (!glued) {
            ifc_flush(f, f->n);
            *spw = 0;
            *nsp = 0;
            f->wrapped = true;
        } else {
            int k = last_break(f);
            if (k > 0) { ifc_flush(f, k); f->wrapped = true; }
        }
    }
    if (f->n == 0) {
        *spw = 0;
        *nsp = 0;
        ifc_bounds(f);
        int guard = 0;
        while (w > f->aw && f->aw < f->cw && guard++ < 64) {
            int nb = fl_next_bottom(f->lc, f->y, f->sasc + f->sdesc);
            if (nb < 0) break;
            f->y = nb;
            ifc_bounds(f);
        }
    }
}

static int up1252(int c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0x9A || c == 0x9C || c == 0x9E) return c - 16;
    if (c == 0xFF) return 0x9F;
    return c;
}
static int low1252(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    return c;
}

static void add_word(ifc_t *f, const char *s, int len, const web_style_t *st, const font_t *font,
                     int asc, int desc, bool wrap) {
    lctx_t *lc = f->lc;
    if (len <= 0) return;
    if (st->transform != TT_NONE) {
        char *t = wa_alloc(&lc->L->ar, (size_t)len + 1);
        if (t) {
            bool start = !(f->n && f->it[f->n - 1].kind == LI_TEXT && !f->pend);
            for (int i = 0; i < len; i++) {
                int c = (unsigned char)s[i];
                if (st->transform == TT_UPPER) c = up1252(c);
                else if (st->transform == TT_LOWER) c = low1252(c);
                else if (start && isalnum(c & 0x7F)) c = up1252(c);
                start = false;
                t[i] = (char)c;
            }
            s = t;
        }
    }
    int spw = 0;
    uint16_t nsp = 0;
    if (f->pend && f->n > 0) { spw = font_char_width(font, ' '); nsp = 1; }
    /* a break may come before this word only after a space that allows one */
    bool glued = !(f->pend && f->pend_brk) && f->n > 0 &&
                 (f->it[f->n - 1].kind == LI_TEXT || f->it[f->n - 1].kind == LI_SPACE);
    f->pend = f->pend_brk = false;
    int w = font_text_width_n(font, s, len);
    ifc_make_room(f, w, &spw, &nsp, glued, true);
    /* a word longer than the whole line: break it */
    while (wrap && f->n == 0 && w > f->aw && len > 1 && f->aw > 0) {
        int k = 0, acc = 0;
        while (k < len) {
            int cw = font_char_width(font, (unsigned char)s[k]);
            if (acc + cw > f->aw && k > 0) break;
            acc += cw;
            k++;
        }
        if (k >= len) break;
        litem_t *it = ifc_push(f);
        if (!it) return;
        it->kind = LI_TEXT; it->font = font; it->text = s; it->len = k; it->x = 0; it->w = acc;
        it->asc = asc; it->desc = desc; it->color = st->color; it->deco = st->deco; it->link = lc->link;
        it->bg = lc->ibg; it->hidden = st->visibility_hidden; it->valign = st->valign; it->fs = st->fsize;
        ifc_flush(f, f->n);
        s += k;
        len -= k;
        w = font_text_width_n(font, s, len);
        ifc_make_room(f, w, &spw, &nsp, false, wrap);
    }
    litem_t *it = ifc_push(f);
    if (!it) return;
    it->kind = LI_TEXT;
    it->glued = glued;
    it->font = font;
    it->text = s;
    it->len = len;
    it->spw = spw;
    it->nsp = nsp;
    it->x = f->used + spw;
    it->w = w;
    it->asc = asc;
    it->desc = desc;
    it->color = st->color;
    it->deco = st->deco;
    it->link = lc->link;
    it->bg = lc->ibg;
    it->hidden = st->visibility_hidden;
    it->valign = (st->valign == VA_SUPER || st->valign == VA_SUB) ? st->valign : VA_BASELINE;
    it->fs = st->fsize;
    f->used = it->x + w;
}

static void add_text(ifc_t *f, const web_node_t *t, const web_style_t *st) {
    lctx_t *lc = f->lc;
    const font_t *font = sfont(lc, st);
    int asc, desc;
    text_metrics(st, font, &asc, &desc);
    const char *s = t->text;
    int n = t->len, ws = st->ws;
    if (ws == WS_PRE || ws == WS_PRE_WRAP) {
        int i = 0;
        while (i < n) {
            unsigned char ch = (unsigned char)s[i];
            if (ch == '\n') { ifc_break(f); i++; continue; }
            if (ch == '\r') { i++; continue; }
            int j = i;
            if (ws == WS_PRE_WRAP && (ch == ' ' || ch == '\t')) {
                while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            } else {
                while (j < n && s[j] != '\n' && s[j] != '\r' && !(ws == WS_PRE_WRAP && (s[j] == ' ' || s[j] == '\t'))) j++;
            }
            /* expand tabs */
            bool tab = false;
            for (int k = i; k < j; k++) if (s[k] == '\t') { tab = true; break; }
            const char *w = s + i;
            int wl = j - i;
            if (tab) {
                char *buf = wa_alloc(&lc->L->ar, (size_t)wl * 8 + 1);
                if (buf) {
                    int o = 0, col = f->col;
                    for (int k = i; k < j; k++) {
                        if (s[k] == '\t') { do { buf[o++] = ' '; col++; } while (col % 8); }
                        else { buf[o++] = s[k]; col++; }
                    }
                    w = buf;
                    wl = o;
                }
            }
            bool spaces = ws == WS_PRE_WRAP && (ch == ' ' || ch == '\t');
            if (spaces) {
                /* preserved spaces: glued to what comes before, a break may follow */
                bool glued_prev = f->n > 0;
                int sw = font_text_width_n(font, w, wl);
                if (f->n == 0 && f->wrapped) { i = j; continue; }   /* spaces at a wrap are dropped */
                if (f->n == 0) ifc_make_room(f, sw, &(int){0}, &(uint16_t){0}, false, true);
                litem_t *it = ifc_push(f);
                if (!it) return;
                it->kind = LI_TEXT; it->glued = glued_prev; it->font = font; it->text = w; it->len = wl;
                it->x = f->used; it->w = sw; it->asc = asc; it->desc = desc; it->color = st->color;
                it->deco = st->deco; it->link = lc->link; it->bg = lc->ibg; it->hidden = st->visibility_hidden;
                it->fs = st->fsize;
                f->used += sw;
                f->col += wl;
                f->pend = f->pend_brk = false;
                i = j;
                /* the next word may start a new line */
                continue;
            }
            f->pend = f->pend_brk = false;
            bool prev_space = f->n > 0 && f->it[f->n - 1].kind == LI_TEXT && f->it[f->n - 1].text[0] == ' ' && ws == WS_PRE_WRAP;
            int before = f->n;
            add_word(f, w, wl, st, font, asc, desc, ws == WS_PRE_WRAP);
            if (prev_space && f->n > before) f->it[f->n - 1].glued = false;
            f->col += wl;
            i = j;
        }
        return;
    }
    bool wrap = ws != WS_NOWRAP;
    int i = 0;
    while (i < n) {
        unsigned char ch = (unsigned char)s[i];
        if (web_isspace(ch)) {
            if (ws == WS_PRE_LINE && ch == '\n') ifc_break(f);
            else { f->pend = true; if (wrap) f->pend_brk = true; }
            i++;
            continue;
        }
        int j = i;
        while (j < n && !web_isspace((unsigned char)s[j])) j++;
        add_word(f, s + i, j - i, st, font, asc, desc, wrap);
        i = j;
    }
}

static void push_simple(ifc_t *f, int kind, int ref, int w, int h, int ml, int mr, int mt, int mb,
                        int asc, const web_style_t *st, const char *text, const font_t *font) {
    lctx_t *lc = f->lc;
    int ow = ml + w + mr, oh = mt + h + mb;
    int spw = 0;
    uint16_t nsp = 0;
    if (f->pend && f->n > 0) { spw = font_char_width(sfont(lc, st), ' '); nsp = 1; }
    f->pend = f->pend_brk = false;
    ifc_make_room(f, ow, &spw, &nsp, false, st->ws != WS_NOWRAP && st->ws != WS_PRE);
    litem_t *it = ifc_push(f);
    if (!it) return;
    it->kind = LI_SIMPLE;
    it->first = kind;
    it->last = ref;
    it->x = f->used + spw;
    it->spw = spw;
    it->w = ow;
    it->len = w;          /* the box itself, without margins */
    it->h = h;
    it->ml = ml;
    it->mt = mt;
    it->asc = asc >= 0 ? mt + asc : oh;
    it->desc = oh - it->asc;
    it->valign = st->valign;
    it->link = lc->link;
    it->hidden = st->visibility_hidden;
    it->fs = st->fsize;
    it->text = text;
    it->font = font;
    it->color = st->color;
    f->used = it->x + ow;
}

static void add_image(ifc_t *f, web_node_t *c) {
    lctx_t *lc = f->lc;
    const web_style_t *st = c->st;
    int w, h;
    image_box(lc, c, f->cw, &w, &h);
    web_image_t *im = c->img >= 0 ? &lc->pg->imgs[c->img] : NULL;
    const char *alt = web_attr(c, "alt");
    bool failed = !im || im->state == IMG_FAILED;
    if (w <= 0 || h <= 0) {
        if (failed && alt && *alt) {
            /* show the alternative text instead */
            web_node_t tmp;
            memset(&tmp, 0, sizeof tmp);
            tmp.type = WN_TEXT;
            tmp.text = (char *)alt;
            tmp.len = (int)strlen(alt);
            add_text(f, &tmp, st);
        }
        return;
    }
    bm_t b;
    box_metrics(st, f->cw, &b);
    int kind = (failed && alt && *alt) ? WI_IMGALT : WI_IMAGE;
    push_simple(f, kind, c->img, w, h, b.ml + b.bl, b.mr + b.br, b.mt + b.bt, b.mb + b.bb, -1, st,
                kind == WI_IMGALT ? alt : NULL, sfont(lc, st));
    if (f->n && (b.bl || b.br || b.bt || b.bb)) f->it[f->n - 1].bst = st;
}

static void add_ctrl(ifc_t *f, web_node_t *c) {
    lctx_t *lc = f->lc;
    web_ctrl_t *ct = &lc->pg->ctrls[c->ctl];
    if (ct->type == C_HIDDEN) return;
    const web_style_t *st = c->st;
    int w, h;
    ctrl_box(lc, ct, st, f->cw, &w, &h);
    int asc;
    if (ct->type == C_CHECKBOX || ct->type == C_RADIO) asc = h - 2;
    else asc = (h - font_height(ct->font)) / 2 + font_ascent(ct->font);
    int m = 2;
    push_simple(f, WI_CTRL, c->ctl, w, h, ct->type == C_CHECKBOX || ct->type == C_RADIO ? 3 : 0, m, 1, 1, asc, st, NULL, ct->font);
}

static void add_box(ifc_t *f, web_node_t *c, bool flex_item) {
    lctx_t *lc = f->lc;
    const web_style_t *st = c->st;
    bm_t b;
    box_metrics(st, f->cw, &b);
    int first = lc->L->nitems;
    int avail = f->cw;
    int w = shrink_to_fit(lc, c, avail);
    marker_t mk = lc->mk;
    lc->mk.on = false;
    int base = lc->flbase;
    lc->flbase = lc->nfl;
    int saved_lb = lc->last_base;
    lc->last_base = INT_MIN;
    bres_t r;
    lc->depth++;
    layout_box(lc, c, 0, b.mt, avail, w, &r, 0);
    lc->depth--;
    int lb = lc->last_base;
    lc->last_base = saved_lb;
    lc->flbase = base;
    lc->mk = mk;
    int ow = b.ml + r.w + b.mr, oh = b.mt + r.h + (r.mb > 0 ? r.mb : 0);
    int spw = 0;
    uint16_t nsp = 0;
    if (f->pend && f->n > 0) { spw = font_char_width(sfont(lc, st), ' '); nsp = 1; }
    f->pend = f->pend_brk = false;
    ifc_make_room(f, ow, &spw, &nsp, false, true);
    litem_t *it = ifc_push(f);
    if (!it) return;
    it->kind = LI_BOX;
    it->first = first;
    it->last = lc->L->nitems;
    it->x = f->used + spw;
    it->spw = spw;
    it->w = ow;
    it->asc = (!flex_item && lb != INT_MIN && !st->overflow_hidden && lb <= oh) ? lb : oh;
    it->desc = oh - it->asc;
    it->valign = st->valign;
    it->fs = st->fsize;
    f->used = it->x + ow;
}

static void block_in_inline(ifc_t *f, web_node_t *c);
static void inline_node(ifc_t *f, web_node_t *c, const web_style_t *pst);

static void inline_children(ifc_t *f, web_node_t *n) {
    for (web_node_t *c = n->first; c; c = c->next) inline_node(f, c, n->st);
}

static void inline_node(ifc_t *f, web_node_t *c, const web_style_t *pst) {
    lctx_t *lc = f->lc;
    if (c->type == WN_TEXT) { if (pst) add_text(f, c, pst); return; }
    if (!displayed(c) || lc->depth > MAX_DEPTH || stack_low()) return;
    const web_style_t *st = c->st;
    if (is_float(c)) {
        if (f->n == 0) { place_float(lc, c, f->cx, f->cw, f->y); ifc_bounds(f); }
        else if (f->ndefer < 8) f->defer[f->ndefer++] = c;
        return;
    }
    if (c->tag == T_BR) { ifc_break(f); return; }
    if (c->tag == T_IMG) { add_image(f, c); return; }
    if (c->ctl >= 0) { add_ctrl(f, c); return; }
    if (c->tag == T_A && web_attr(c, "name") && !c->id) anchor_add(lc, web_attr(c, "name"), f->y);
    if (block_level(c)) { block_in_inline(f, c); return; }
    int d = st->display;
    if (d == D_INLINE_BLOCK || d == D_INLINE_TABLE || d == D_INLINE_FLEX) {
        int saved = lc->link;
        if (c->link >= 0) lc->link = c->link;
        add_box(f, c, false);
        lc->link = saved;
        return;
    }
    /* inline element */
    if (c->id) anchor_add(lc, c->id, f->y);
    int saved_link = lc->link;
    uint32_t saved_bg = lc->ibg;
    if (c->link >= 0) lc->link = c->link;
    if (COLOR_A(st->bg) && !st->visibility_hidden) lc->ibg = st->bg;
    bm_t b;
    box_metrics(st, f->cw, &b);
    int lpad = b.ml + b.bl + b.pl, rpad = b.mr + b.br + b.pr;
    if (lpad > 0) {
        int spw = 0;
        uint16_t nsp = 0;
        if (f->pend && f->n > 0) { spw = font_char_width(sfont(lc, st), ' '); nsp = 1; }
        bool glued = !(f->pend && f->pend_brk) && f->n > 0 && f->it[f->n - 1].kind != LI_BOX && f->it[f->n - 1].kind != LI_SIMPLE;
        f->pend = f->pend_brk = false;
        ifc_make_room(f, lpad, &spw, &nsp, glued, st->ws != WS_NOWRAP);
        litem_t *it = ifc_push(f);
        if (it) {
            it->kind = LI_SPACE; it->glued = glued; it->x = f->used + spw; it->spw = spw; it->w = lpad; f->used = it->x + lpad;
            it->bg = lc->ibg; it->font = sfont(lc, st); it->hidden = st->visibility_hidden;
        }
    }
    lc->depth++;
    inline_children(f, c);
    lc->depth--;
    if (rpad > 0) {
        litem_t *it = ifc_push(f);
        if (it) {
            it->kind = LI_SPACE; it->glued = true; it->x = f->used; it->w = rpad; f->used += rpad;
            it->bg = lc->ibg; it->font = sfont(lc, st); it->hidden = st->visibility_hidden;
        }
    }
    lc->link = saved_link;
    lc->ibg = saved_bg;
}

static void ifc_init(ifc_t *f, lctx_t *lc, const web_style_t *cst, int cx, int y, int cw) {
    memset(f, 0, sizeof *f);
    f->lc = lc;
    f->cx = cx;
    f->cw = cw;
    f->y = y;
    f->align = cst->align == TA_JUSTIFY ? TA_LEFT : cst->align;
    f->indent = cst->indent;
    f->first_line = true;
    const font_t *font = sfont(lc, cst);
    text_metrics(cst, font, &f->sasc, &f->sdesc);
    ifc_bounds(f);
}

static void ifc_finish(ifc_t *f) {
    if (f->n) ifc_flush(f, f->n);
    lctx_t *lc = f->lc;
    for (int i = 0; i < f->ndefer; i++) place_float(lc, f->defer[i], f->cx, f->cw, f->y);
    f->ndefer = 0;
    free(f->it);
    f->it = NULL;
}

static void block_in_inline(ifc_t *f, web_node_t *c) {
    lctx_t *lc = f->lc;
    if (f->n) ifc_flush(f, f->n);
    f->pend = f->pend_brk = false;
    bm_t b;
    box_metrics(c->st, f->cw, &b);
    if (c->st->clear) f->y = fl_clear_y(lc, f->y, c->st->clear);
    bres_t r;
    int top = b.mt > 0 ? b.mt : 0;
    layout_box(lc, c, f->cx, f->y + top, f->cw, -1, &r, 0);
    f->y += top + r.h + (r.mb > 0 ? r.mb : 0);
    f->lines++;
    f->first_line = false;
    ifc_bounds(f);
}

/* ---- floats ------------------------------------------------------------------------ */
static void place_float(lctx_t *lc, web_node_t *c, int cx, int cw, int y) {
    const web_style_t *st = c->st;
    bm_t b;
    box_metrics(st, cw, &b);
    int w = shrink_to_fit(lc, c, cw);
    int first = lc->L->nitems;
    marker_t mk = lc->mk;
    lc->mk.on = false;
    int base = lc->flbase;
    lc->flbase = lc->nfl;
    int saved_link = lc->link;
    if (c->link >= 0) lc->link = c->link;
    bres_t r;
    lc->depth++;
    layout_box(lc, c, 0, b.mt, cw, w, &r, 0);
    lc->depth--;
    lc->link = saved_link;
    lc->flbase = base;
    lc->mk = mk;
    /* the margin box: negative margins let a float overlap (the "margin-left:-100%" sidebar) */
    int ow = b.ml + r.w + b.mr, oh = b.mt + r.h + (r.mb > 0 ? r.mb : 0);
    if (ow > cw) ow = cw;
    int fy = y;
    if (st->clear) fy = fl_clear_y(lc, fy, st->clear);
    int lx, aw, guard = 0;
    for (;;) {
        fl_bounds(lc, fy, oh, cx, cw, &lx, &aw);
        if (ow <= aw || aw >= cw || guard++ > 64) break;
        int nb = fl_next_bottom(lc, fy, oh);
        if (nb < 0) break;
        fy = nb;
    }
    bool right = st->floating == 2;
    int fx = right ? lx + aw - ow : lx;
    shift_items(lc, first, lc->L->nitems, fx, fy);
    fl_add(lc, fx, fy, ow, oh, right);
}

/* ---- block layout -------------------------------------------------------------------- */
static web_node_t *first_inflow(web_node_t *n) {
    for (web_node_t *c = n->first; c; c = c->next) {
        if (c->type == WN_TEXT) { if (ws_only(c)) continue; return NULL; }
        if (!displayed(c)) continue;
        if (is_float(c)) continue;
        if (!block_level(c)) return NULL;
        return c;
    }
    return NULL;
}

static bool plain_block(const web_node_t *n) {
    int d = n->st->display;
    return (d == D_BLOCK || d == D_LIST_ITEM) && !n->st->overflow_hidden;
}

static int leading_margin(web_node_t *n, int cbw, int depth) {
    const web_style_t *st = n->st;
    int mt = rlen(st->margin[0], cbw);
    if (depth > 30 || !plain_block(n)) return mt;
    if (st->bw[0] || rlen(st->padding[0], cbw)) return mt;
    web_node_t *c = first_inflow(n);
    if (!c) return mt;
    return collapse(mt, leading_margin(c, cbw, depth + 1));
}

/* ---- flex rows ------------------------------------------------------------------------ */
typedef struct fitem {
    web_node_t *n;              /* the item, or NULL for a run of text            */
    web_node_t *from, *to;      /* text run                                       */
    int order, idx;
    int base, minw, w;          /* margin-box widths                              */
    int grow, shrink;
    int ml, mr, mt, mb, hp;
    int first, last, h;
} fitem_t;

static bool is_flex_row(const web_style_t *st) {
    return (st->display == D_FLEX || st->display == D_INLINE_FLEX) && !st->flex_column;
}

static int fitem_cmp(const fitem_t *a, const fitem_t *b) {
    return a->order != b->order ? (a->order < b->order ? -1 : 1) : (a->idx < b->idx ? -1 : a->idx > b->idx);
}

/* the items of a flex container, in display order; *n returns the count */
static fitem_t *flex_items(lctx_t *lc, web_node_t *box, int cw, int *count) {
    fitem_t *it = NULL;
    int n = 0, cap = 0;
    for (web_node_t *c = box->first; c;) {
        web_node_t *from = NULL, *to = NULL;
        if (c->type == WN_TEXT) {
            from = c;
            bool content = false;
            while (c && c->type == WN_TEXT) { if (!ws_only(c)) content = true; c = c->next; }
            to = c;
            if (!content) continue;
        } else if (!displayed(c)) { c = c->next; continue; }
        if (n >= cap) {
            int nc = cap ? cap * 2 : 16;
            fitem_t *ni = realloc(it, sizeof *ni * nc);
            if (!ni) break;
            it = ni;
            cap = nc;
        }
        fitem_t *f = &it[n];
        memset(f, 0, sizeof *f);
        f->idx = n;
        if (from) {
            f->from = from;
            f->to = to;
            meas_t m = { 0, 0, 0, 0, false, false };
            for (web_node_t *x = from; x != to; x = x->next) meas_node(lc, &m, x, box->st, 0);
            meas_flush(&m);
            f->base = m.max;
            f->minw = m.min;
            f->shrink = 100;
        } else {
            const web_style_t *st = c->st;
            bm_t b;
            box_metrics(st, cw, &b);
            f->n = c;
            f->order = st->order;
            f->grow = st->grow;
            f->shrink = st->shrink;
            f->ml = b.ml; f->mr = b.mr; f->mt = b.mt; f->mb = b.mb;
            f->hp = b.pl + b.pr + b.bl + b.br;
            int mn, mx;
            intrinsic(lc, c, &mn, &mx);
            web_len_t size = st->basis.u == U_PX || st->basis.u == U_PCT ? st->basis : st->width;
            if (size.u == U_PX || size.u == U_PCT) {
                int content = rlen(size, cw) - (st->border_box ? f->hp : 0);
                if (content < 0) content = 0;
                f->base = f->ml + f->hp + content + f->mr;
            } else {
                f->base = mx;
            }
            f->minw = st->overflow_hidden ? f->ml + f->hp + f->mr : MIN(mn, f->base);
            if (st->width.u == U_PX) f->minw = MIN(f->minw, f->base);
            if (st->minw.u == U_PX || st->minw.u == U_PCT) {
                int lo = f->ml + f->hp + f->mr + rlen(st->minw, cw) - (st->border_box ? f->hp : 0);
                if (lo > f->minw) f->minw = lo;
                if (lo > f->base) f->base = lo;
            }
            if (st->maxw.u == U_PX || st->maxw.u == U_PCT) {
                int hi = f->ml + f->hp + f->mr + rlen(st->maxw, cw) - (st->border_box ? f->hp : 0);
                if (f->base > hi) f->base = hi;
                if (f->minw > hi) f->minw = hi;
            }
        }
        n++;
        if (!from) c = c->next;
    }
    /* stable sort by `order` */
    for (int i = 1; i < n; i++) {
        fitem_t x = it[i];
        int j = i - 1;
        while (j >= 0 && fitem_cmp(&it[j], &x) > 0) { it[j + 1] = it[j]; j--; }
        it[j + 1] = x;
    }
    *count = n;
    return it;
}

static void flex_intrinsic(lctx_t *lc, web_node_t *box, int *mn, int *mx) {
    int n;
    fitem_t *it = flex_items(lc, box, lc->vw, &n);
    int gap = box->st->gap, smin = 0, smax = 0;
    for (int i = 0; i < n; i++) {
        smax += it[i].base + (i ? gap : 0);
        if (box->st->flex_wrap) smin = MAX(smin, it[i].minw);
        else smin += it[i].minw + (i ? gap : 0);
    }
    free(it);
    *mn = smin;
    *mx = smax;
}

/* grow a laid-out box (border box bx,by,bw,bh) by `room` pixels: its background
   gets taller and its bottom / side borders move with it */
static void stretch_box(lctx_t *lc, int first, int last, int bx, int by, int bw, int bh, int room) {
    for (int i = first; i < last; i++) {
        web_item_t *it = &lc->L->items[i];
        if (it->kind != WI_RECT) continue;
        if (it->x == bx && it->y == by && it->w == bw && it->h == bh) it->h += room;                 /* background */
        else if (it->y + it->h == by + bh && it->y > by && it->w == bw && it->x == bx) it->y += room;  /* bottom border */
        else if ((it->x == bx || it->x + it->w == bx + bw) && it->y >= by && it->y + it->h <= by + bh &&
                 it->w < bw && it->y + it->h >= by + bh - 8) it->h += room;                          /* side borders */
    }
}

static int flex_row(lctx_t *lc, web_node_t *box, int cx, int cy, int cw, int fixed_h) {
    const web_style_t *cs = box->st;
    int n;
    fitem_t *it = flex_items(lc, box, cw, &n);
    if (!it) return 0;
    int gap = cs->gap, y = cy;
    for (int a = 0; a < n;) {
        /* one line of items */
        int b = a, sum = 0;
        while (b < n) {
            int add = it[b].base + (b > a ? gap : 0);
            if (cs->flex_wrap && b > a && sum + add > cw) break;
            sum += add;
            b++;
        }
        int free_w = cw - sum;
        for (int i = a; i < b; i++) it[i].w = it[i].base;
        if (free_w > 0) {
            int64_t g = 0;
            for (int i = a; i < b; i++) g += it[i].grow;
            if (g > 0) {
                int given = 0;
                for (int i = a; i < b; i++) { int d = (int)((int64_t)free_w * it[i].grow / g); it[i].w += d; given += d; }
                for (int i = b - 1; i >= a && given < free_w; i--) if (it[i].grow) { it[i].w += free_w - given; given = free_w; }
                free_w = 0;
            }
        } else if (free_w < 0) {
            /* shrink in proportion to shrink factor x base size, never below the minimum */
            for (int pass = 0; pass < 3 && free_w < 0; pass++) {
                int64_t sw = 0;
                for (int i = a; i < b; i++) if (it[i].w > it[i].minw) sw += (int64_t)it[i].shrink * it[i].base;
                if (sw <= 0) break;
                int need = -free_w, taken = 0;
                for (int i = a; i < b; i++) {
                    if (it[i].w <= it[i].minw) continue;
                    int d = (int)((int64_t)need * it[i].shrink * it[i].base / sw);
                    if (it[i].w - d < it[i].minw) d = it[i].w - it[i].minw;
                    it[i].w -= d;
                    taken += d;
                }
                free_w += taken;
                if (!taken) break;
            }
        }
        /* justify-content */
        int cnt = b - a, x = cx, spacing = gap;
        if (free_w > 0) {
            switch (cs->justify) {
            case JC_CENTER: x += free_w / 2; break;
            case JC_END: x += free_w; break;
            case JC_BETWEEN: if (cnt > 1) spacing += free_w / (cnt - 1); break;
            case JC_AROUND: spacing += free_w / cnt; x += free_w / cnt / 2; break;
            case JC_EVENLY: spacing += free_w / (cnt + 1); x += free_w / (cnt + 1); break;
            default: break;
            }
        }
        /* lay the items out */
        int lineh = 0;
        for (int i = a; i < b; i++) {
            fitem_t *f = &it[i];
            f->first = lc->L->nitems;
            if (f->n) {
                int content = f->w - f->ml - f->mr - f->hp;
                if (content < 0) content = 0;
                int saved = lc->link;
                if (f->n->link >= 0) lc->link = f->n->link;
                bres_t r;
                layout_box(lc, f->n, x, y + f->mt, cw, content, &r, 0);
                lc->link = saved;
                f->h = f->mt + r.h + f->mb;
            } else {
                bool any;
                f->h = run_ifc(lc, box, f->from, f->to, x, y, MAX(f->w, 1), &any);
            }
            f->last = lc->L->nitems;
            if (f->h > lineh) lineh = f->h;
            x += f->w + spacing;
        }
        if (a == 0 && b == n && fixed_h > lineh) lineh = fixed_h;
        /* align-items */
        for (int i = a; i < b; i++) {
            fitem_t *f = &it[i];
            int room = lineh - f->h;
            if (room <= 0) continue;
            if (cs->align_items == AI_CENTER) shift_items(lc, f->first, f->last, 0, room / 2);
            else if (cs->align_items == AI_END) shift_items(lc, f->first, f->last, 0, room);
            else if (cs->align_items == AI_STRETCH && f->n && f->n->st->height.u == U_AUTO && f->last > f->first) {
                web_item_t *bg = &lc->L->items[f->first];
                /* the box's border box: the first item is its background when it has one */
                int bwid = f->w - f->ml - f->mr, bh = f->h - f->mt - f->mb;
                int bxx = bg->kind == WI_RECT && bg->w == bwid ? bg->x : INT_MIN;
                if (bxx != INT_MIN) stretch_box(lc, f->first, f->last, bxx, y + f->mt, bwid, bh, room);
                else {
                    /* no background: find the left border to locate the box */
                    for (int k = f->first; k < f->last; k++) {
                        web_item_t *r = &lc->L->items[k];
                        if (r->kind == WI_RECT && r->y == y + f->mt && r->w == bwid) {
                            stretch_box(lc, f->first, f->last, r->x, y + f->mt, bwid, bh, room);
                            break;
                        }
                    }
                }
            }
        }
        y += lineh;
        a = b;
        if (a < n) y += cs->rgap;
    }
    free(it);
    return y - cy;
}

static int run_ifc(lctx_t *lc, web_node_t *container, web_node_t *from, web_node_t *to, int cx, int y, int cw, bool *any) {
    ifc_t f;
    ifc_init(&f, lc, container->st, cx, y, cw);
    for (web_node_t *c = from; c != to; c = c->next) inline_node(&f, c, container->st);
    ifc_finish(&f);
    *any = f.lines > 0;
    return f.y - y;
}

static int block_children(lctx_t *lc, web_node_t *n, int cx, int cy, int cw, bool ctop, bool cbot, int *trailing) {
    int y = cy, pending = 0;
    bool first = true;
    int counter = 1;
    if (n->tag == T_OL) counter = attr_int(n, "start", 1, -100000, 1000000);
    web_node_t *c = n->first;
    while (c) {
        if (c->type == WN_TEXT || (displayed(c) && !block_level(c) && !is_float(c))) {
            /* a run of inline content: an anonymous block */
            web_node_t *e = c;
            bool content = false;
            while (e && (e->type == WN_TEXT || !displayed(e) || (!block_level(e) && !is_float(e)))) {
                if (e->type == WN_TEXT ? !ws_only(e) : displayed(e)) content = true;
                e = e->next;
            }
            if (content) {
                int gap = (first && ctop) ? 0 : collapse(pending, 0);
                bool any;
                int h = run_ifc(lc, n, c, e, cx, y + gap, cw, &any);
                if (any) { y += gap + h; pending = 0; first = false; }
            }
            c = e;
            continue;
        }
        if (c->type != WN_ELEM || !displayed(c)) { c = c->next; continue; }
        if (is_float(c)) {
            place_float(lc, c, cx, cw, y + (pending > 0 ? pending : 0));
            c = c->next;
            continue;
        }
        const web_style_t *st = c->st;
        int lm = leading_margin(c, cw, 0);
        int gap = (first && ctop) ? 0 : collapse(pending, lm);
        if (st->clear) {
            int cy2 = fl_clear_y(lc, y + gap, st->clear);
            if (cy2 > y + gap) gap = cy2 - y;
        }
        int bx = cx, bw = cw;
        if ((st->display == D_TABLE || st->overflow_hidden || st->display == D_FLEX) && lc->nfl > lc->flbase) {
            int lx, aw;
            fl_bounds(lc, y + gap, 1, cx, cw, &lx, &aw);
            bx = lx;
            bw = aw;
        }
        int saved_link = lc->link;
        if (c->link >= 0) lc->link = c->link;
        bres_t r;
        int num = 0;
        if (st->display == D_LIST_ITEM) {
            const char *v = web_attr(c, "value");
            if (v && *v) counter = atoi(v);
            num = counter++;
        }
        int top = y + gap;
        /* the leading margin passed above the box: its own top margin is not added again */
        layout_box(lc, c, bx, top, bw, -1, &r, num);
        lc->link = saved_link;
        if (r.h == 0 && first_inflow(c) == NULL && !c->first) {
            pending = collapse(collapse(pending, lm), r.mb);
        } else {
            y = top + r.h;
            pending = r.mb;
            first = false;
        }
        c = c->next;
    }
    if (cbot) *trailing = pending;
    else { y += pending > 0 ? pending : 0; *trailing = 0; }
    return y - cy;
}

static int layout_box(lctx_t *lc, web_node_t *n, int x, int y, int cbw, int fw, bres_t *r, int listnum) {
    const web_style_t *st = n->st;
    memset(r, 0, sizeof *r);
    if (lc->depth > MAX_DEPTH || stack_low()) return 0;
    if (st->display == D_TABLE || st->display == D_INLINE_TABLE) return layout_table(lc, n, x, y, cbw, fw, r);
    bm_t b;
    box_metrics(st, cbw, &b);
    int hp = b.pl + b.pr + b.bl + b.br;
    int w;
    bool spec = false;
    if (fw >= 0) { w = fw; spec = true; }
    else if (st->width.u == U_PX || st->width.u == U_PCT) {
        w = rlen(st->width, cbw);
        if (st->border_box) w -= hp;
        spec = true;
    } else {
        w = cbw - b.ml - b.mr - hp;
    }
    if (fw < 0) {
        if (st->maxw.u != U_NONE) {
            int mx = rlen(st->maxw, cbw) - (st->border_box ? hp : 0);
            if (mx >= 0 && w > mx) { w = mx; spec = true; }
        }
        if (st->minw.u == U_PX || st->minw.u == U_PCT) {
            int mn = rlen(st->minw, cbw) - (st->border_box ? hp : 0);
            if (w < mn) w = mn;
        }
    }
    if (w < 0) w = 0;
    int bw = w + hp;
    int ml = b.ml;
    if (spec && fw < 0) {
        const web_style_t *ps = n->parent && n->parent->st ? n->parent->st : NULL;
        if (b.aml && b.amr) ml = (cbw - bw) / 2;
        else if (b.aml) ml = cbw - bw - b.mr;
        else if (ps && ps->align_legacy && ps->align == TA_CENTER) ml = (cbw - bw) / 2;
        else if (ps && ps->align_legacy && ps->align == TA_RIGHT) ml = cbw - bw - b.mr;
        if (ml < 0 && b.ml >= 0 && (b.aml || (ps && ps->align_legacy))) ml = 0;
    }
    int bx = x + ml;
    int cx = bx + b.bl + b.pl, cy = y + b.bt + b.pt;
    int first_item = lc->L->nitems;
    int bgi = (st->visibility_hidden || n == lc->skip_bg) ? -1 : reserve_bg(lc, st->bg);
    if (n->id) anchor_add(lc, n->id, y);
    if (st->display == D_LIST_ITEM) {
        if (lc->mk.on) emit_marker(lc, cy + font_ascent(lc->mk.font));
        lc->mk.on = !st->visibility_hidden;
        lc->mk.type = st->list;
        lc->mk.num = listnum;
        lc->mk.font = sfont(lc, st);
        lc->mk.color = st->color;
        lc->mk.x = cx;
        lc->mk.fs = st->fsize;
    }
    int fmark = lc->nfl;
    bool root = n->parent && n->parent->type == WN_DOC;
    bool bfc = st->overflow_hidden || fw >= 0 || st->display == D_FLEX || st->display == D_INLINE_FLEX;
    bool ctop = !root && !bfc && b.pt == 0 && b.bt == 0;
    bool cbot = !root && !bfc && b.pb == 0 && b.bb == 0 && st->height.u == U_AUTO;
    int trailing = 0, ch;
    lc->depth++;
    if (is_flex_row(st)) ch = flex_row(lc, n, cx, cy, w, st->height.u == U_PX ? st->height.v - (st->border_box ? b.pt + b.pb + b.bt + b.bb : 0) : -1);
    else ch = block_children(lc, n, cx, cy, w, ctop, cbot, &trailing);
    lc->depth--;
    if (st->display == D_LIST_ITEM && lc->mk.on && lc->mk.x == cx) emit_marker(lc, cy + font_ascent(lc->mk.font));
    /* contain the floats placed inside */
    for (int i = fmark; i < lc->nfl; i++) {
        int fb = lc->fl[i].y + lc->fl[i].h - cy;
        if (fb > ch) { ch = fb; trailing = 0; }
    }
    lc->nfl = fmark;
    if (st->height.u == U_PX) {
        int sh = st->height.v - (st->border_box ? b.pt + b.pb + b.bt + b.bb : 0);
        if (sh > ch || st->overflow_hidden) ch = sh > 0 ? sh : 0;
    }
    int bh = b.bt + b.pt + ch + b.pb + b.bb;
    fill_bg(lc, bgi, bx, y, bw, bh);
    borders(lc, st, bx, y, bw, bh);
    if (st->position == POS_RELATIVE) {
        int dx = st->left.u == U_PX ? st->left.v : 0, dy = st->top.u == U_PX ? st->top.v : 0;
        shift_items(lc, first_item, lc->L->nitems, dx, dy);
    }
    r->h = bh;
    r->w = bw;
    r->mb = cbot ? collapse(b.mb, trailing) : b.mb;
    return bh;
}

/* ---- entry points ------------------------------------------------------------------- */
void web_layout_free(web_layout_t *L) {
    free(L->items);
    free(L->anchors);
    wa_free(&L->ar);
    memset(L, 0, sizeof *L);
}

void web_layout(web_page_t *pg, int width, int height) {
    web_layout_t *L = &pg->lay;
    uint32_t t0 = sys_uptime_ms();
    free(L->items);
    free(L->anchors);
    wa_free(&L->ar);
    memset(L, 0, sizeof *L);
    L->bg = 0xFFFFFFFFu;
    L->vw = width;
    for (int i = 0; i < pg->nctrls; i++) pg->ctrls[i].w = 0;
    web_doc_t *d = pg->doc;
    if (!d || !d->html || !d->html->st || d->html->st->display == D_NONE) { L->width = width; L->height = height; return; }
    lctx_t lc;
    memset(&lc, 0, sizeof lc);
    stack_base = (uintptr_t)&lc;
    lc.pg = pg;
    lc.L = L;
    lc.vw = width;
    lc.vh = height;
    lc.zoom = pg->zoom > 0 ? pg->zoom : 100;
    lc.gen = ++g_gen;
    lc.link = -1;
    lc.last_base = INT_MIN;
    const web_style_t *hs = d->html->st;
    const web_style_t *bs = d->body && d->body->st ? d->body->st : NULL;
    if (COLOR_A(hs->bg)) L->bg = color_alpha(hs->bg, 255);
    else if (bs && COLOR_A(bs->bg)) { L->bg = color_alpha(bs->bg, 255); lc.skip_bg = d->body; }
    bres_t r;
    layout_box(&lc, d->html, 0, 0, width, -1, &r, 0);
    int h = r.h + (r.mb > 0 ? r.mb : 0);
    int w = width;
    for (int i = 0; i < L->nitems; i++) {
        web_item_t *it = &L->items[i];
        if (it->x + it->w > w && it->kind != WI_RECT) w = it->x + it->w;
        if (it->y + it->h > h) h = it->y + it->h;
        if (it->kind == WI_CTRL && it->ref >= 0 && it->ref < pg->nctrls) {
            web_ctrl_t *c = &pg->ctrls[it->ref];
            c->x = it->x; c->y = it->y; c->w = it->w; c->h = it->h;
        }
    }
    L->width = w;
    L->height = h;
    free(lc.fl);
    L->ms = (int)(sys_uptime_ms() - t0);
}

int web_anchor_y(web_page_t *pg, const char *name) {
    web_layout_t *L = &pg->lay;
    if (!name || !*name) return -1;
    for (int i = 0; i < L->nanchors; i++) if (!strcmp(L->anchors[i].name, name)) return L->anchors[i].y;
    return -1;
}
