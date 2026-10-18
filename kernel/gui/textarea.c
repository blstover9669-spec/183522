/*
 * textarea.c - multi-line text editing widget (ui_textarea).
 *
 * The text lives in one contiguous NUL-terminated buffer (widget->text) with a
 * line-start index rebuilt after each edit; this keeps the code simple and is
 * fast enough for files of a few hundred KB. Undo/redo keep whole-text
 * snapshots of small documents, grouping consecutive typing.
 */
#include "widget_internal.h"

#define TA_PAD 6
#define TA_SB 12
#define UNDO_MAX 32
#define UNDO_LIMIT (256 * 1024)   /* no undo snapshots for larger texts */

typedef struct { char *text; int cursor; } snap_t;

typedef struct {
    int len, cap;
    int *lines;            /* start offset of each line */
    int nlines, lines_cap;
    int top;               /* first visible line */
    int scroll_x;          /* horizontal scroll in pixels */
    int cursor, anchor;    /* byte offsets */
    int pref_x;            /* preferred caret x for up/down, -1 = none */
    int max_w;             /* widest line in pixels */
    int tab_width;
    bool readonly, line_numbers, modified, dragging;
    int sb_drag;           /* 0 none, 1 vertical, 2 horizontal */
    int sb_off;
    snap_t undo[UNDO_MAX];
    int nundo;
    snap_t redo[UNDO_MAX];
    int nredo;
    uint32_t last_edit_tick;
    int last_edit_kind;    /* 1 typing, 2 delete, 3 other */
    widget_cb on_cursor;
    void *on_cursor_ctx;
} ta_t;

static ta_t *TA(widget_t *w) { return (ta_t *)w->ext; }
static const font_t *ta_font(widget_t *w) { return w->font ? w->font : FONT_MONO; }

/* ---- index -------------------------------------------------------------------------- */
static int char_w(widget_t *w, char c, int x) {
    const font_t *f = ta_font(w);
    if (c == '\t') {
        int tw = TA(w)->tab_width * font_char_width(f, ' ');
        return tw - (x % tw);
    }
    return font_char_width(f, (unsigned char)c);
}

static int line_start(ta_t *t, int i) { return t->lines[i]; }
static int line_end(widget_t *w, int i) {   /* offset of the '\n' (or len) */
    ta_t *t = TA(w);
    return i + 1 < t->nlines ? t->lines[i + 1] - 1 : t->len;
}

static int text_width(widget_t *w, int from, int to) {
    int x = 0;
    for (int i = from; i < to; i++) x += char_w(w, w->text[i], x);
    return x;
}

static void reindex(widget_t *w) {
    ta_t *t = TA(w);
    t->nlines = 0;
    int start = 0;
    t->max_w = 0;
    for (int i = 0; i <= t->len; i++) {
        if (i == t->len || w->text[i] == '\n') {
            if (t->nlines == t->lines_cap) {
                int nc = t->lines_cap ? t->lines_cap * 2 : 256;
                int *nl = krealloc(t->lines, sizeof(int) * (size_t)nc);
                if (!nl) return;
                t->lines = nl;
                t->lines_cap = nc;
            }
            t->lines[t->nlines++] = start;
            int lw = text_width(w, start, i);
            if (lw > t->max_w) t->max_w = lw;
            start = i + 1;
        }
    }
}

static int line_of(ta_t *t, int off) {
    int lo = 0, hi = t->nlines - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (t->lines[mid] <= off) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* ---- geometry --------------------------------------------------------------------- */
static int gutter_w(widget_t *w) {
    ta_t *t = TA(w);
    if (!t->line_numbers) return 0;
    int digits = 1, n = t->nlines;
    while (n >= 10) { n /= 10; digits++; }
    if (digits < 3) digits = 3;
    return digits * font_char_width(FONT_MONO, '0') + 14;
}

static int line_h(widget_t *w) { return font_height(ta_font(w)); }

typedef struct { rect_t text; bool vsb, hsb; rect_t vsb_r, hsb_r; int vis_lines; } ta_geom_t;

static ta_geom_t geom(widget_t *w) {
    ta_t *t = TA(w);
    ta_geom_t g;
    rect_t r = w->r;
    int gw = gutter_w(w);
    int lh = line_h(w);
    int avail_w = r.w - 2 - gw - 2 * TA_PAD;
    int avail_h = r.h - 2 - 2 * 2;
    g.vsb = t->nlines * lh > avail_h;
    if (g.vsb) avail_w -= TA_SB;
    g.hsb = t->max_w > avail_w;
    if (g.hsb) {
        avail_h -= TA_SB;
        if (!g.vsb && t->nlines * lh > avail_h) { g.vsb = true; avail_w -= TA_SB; }
    }
    g.text = rect_make(r.x + 1 + gw + TA_PAD, r.y + 3, MAX(1, avail_w), MAX(1, avail_h));
    g.vis_lines = MAX(1, avail_h / lh);
    g.vsb_r = rect_make(r.x + r.w - 1 - TA_SB, r.y + 1, TA_SB, r.h - 2 - (g.hsb ? TA_SB : 0));
    g.hsb_r = rect_make(r.x + 1, r.y + r.h - 1 - TA_SB, r.w - 2 - (g.vsb ? TA_SB : 0), TA_SB);
    return g;
}

static void clamp_scroll(widget_t *w) {
    ta_t *t = TA(w);
    ta_geom_t g = geom(w);
    t->top = CLAMP(t->top, 0, MAX(0, t->nlines - g.vis_lines));
    t->scroll_x = CLAMP(t->scroll_x, 0, MAX(0, t->max_w - g.text.w + 2));
}

static void ensure_visible(widget_t *w) {
    ta_t *t = TA(w);
    ta_geom_t g = geom(w);
    int ln = line_of(t, t->cursor);
    if (ln < t->top) t->top = ln;
    if (ln >= t->top + g.vis_lines) t->top = ln - g.vis_lines + 1;
    int cx = text_width(w, line_start(t, ln), t->cursor);
    if (cx - t->scroll_x > g.text.w - 4) t->scroll_x = cx - g.text.w + 24;
    if (cx - t->scroll_x < 0) t->scroll_x = MAX(0, cx - 24);
    clamp_scroll(w);
}

static void notify_cursor(widget_t *w) {
    ta_t *t = TA(w);
    if (t->on_cursor) ui_fire(w, t->on_cursor, t->on_cursor_ctx);
}

/* ---- undo ------------------------------------------------------------------------- */
static void snap_free(snap_t *s) { kfree(s->text); s->text = NULL; }

static void push_snap(snap_t *stack, int *n, char *text, int cursor) {
    if (*n == UNDO_MAX) {
        snap_free(&stack[0]);
        memmove(stack, stack + 1, sizeof(snap_t) * (UNDO_MAX - 1));
        (*n)--;
    }
    stack[*n].text = text;
    stack[*n].cursor = cursor;
    (*n)++;
}

/* Record the state before an edit of the given kind (typing is grouped). */
static void begin_edit(widget_t *w, int kind) {
    ta_t *t = TA(w);
    bool group = kind != 3 && kind == t->last_edit_kind && g_ticks - t->last_edit_tick < 1500;
    t->last_edit_tick = g_ticks;
    t->last_edit_kind = kind;
    for (int i = 0; i < t->nredo; i++) snap_free(&t->redo[i]);
    t->nredo = 0;
    if (group || t->len > UNDO_LIMIT) return;
    char *copy = kmalloc((size_t)t->len + 1);
    if (!copy) return;
    memcpy(copy, w->text, (size_t)t->len + 1);
    push_snap(t->undo, &t->nundo, copy, t->cursor);
}

static void restore_snap(widget_t *w, snap_t *s) {
    ta_t *t = TA(w);
    kfree(w->text);
    w->text = s->text;
    s->text = NULL;
    t->len = (int)strlen(w->text);
    t->cap = t->len + 1;
    t->cursor = t->anchor = CLAMP(s->cursor, 0, t->len);
    t->modified = true;
    t->last_edit_kind = 0;
    reindex(w);
    ensure_visible(w);
    ui_invalidate(w);
    ui_fire(w, w->cb, w->ctx);
    notify_cursor(w);
}

void ui_textarea_undo(widget_t *w) {
    if (!w || w->type != UI_TEXTAREA) return;
    ta_t *t = TA(w);
    if (!t->nundo) return;
    char *cur = strdup(w->text);
    if (cur) push_snap(t->redo, &t->nredo, cur, t->cursor);
    restore_snap(w, &t->undo[--t->nundo]);
}

void ui_textarea_redo(widget_t *w) {
    if (!w || w->type != UI_TEXTAREA) return;
    ta_t *t = TA(w);
    if (!t->nredo) return;
    char *cur = strdup(w->text);
    if (cur) push_snap(t->undo, &t->nundo, cur, t->cursor);
    restore_snap(w, &t->redo[--t->nredo]);
}

/* ---- editing -------------------------------------------------------------------- */
static bool reserve(widget_t *w, int need) {
    ta_t *t = TA(w);
    if (need + 1 <= t->cap) return true;
    int nc = t->cap ? t->cap : 256;
    while (nc < need + 1) nc *= 2;
    char *nt = krealloc(w->text, (size_t)nc);
    if (!nt) return false;
    w->text = nt;
    t->cap = nc;
    return true;
}

static void delete_range(widget_t *w, int a, int b) {
    ta_t *t = TA(w);
    if (a > b) { int x = a; a = b; b = x; }
    a = CLAMP(a, 0, t->len);
    b = CLAMP(b, 0, t->len);
    if (a == b) return;
    memmove(w->text + a, w->text + b, (size_t)(t->len - b) + 1);
    t->len -= b - a;
    t->cursor = t->anchor = a;
}

static void insert_raw(widget_t *w, const char *s, int n) {
    ta_t *t = TA(w);
    if (n <= 0 || !reserve(w, t->len + n)) return;
    memmove(w->text + t->cursor + n, w->text + t->cursor, (size_t)(t->len - t->cursor) + 1);
    memcpy(w->text + t->cursor, s, (size_t)n);
    t->len += n;
    t->cursor += n;
    t->anchor = t->cursor;
}

static void after_edit(widget_t *w) {
    ta_t *t = TA(w);
    t->modified = true;
    t->pref_x = -1;
    reindex(w);
    ensure_visible(w);
    ui_blink_on = true;
    ui_invalidate(w);
    ui_fire(w, w->cb, w->ctx);
    notify_cursor(w);
}

static bool has_sel(ta_t *t) { return t->cursor != t->anchor; }

static void insert_text(widget_t *w, const char *s, int kind) {
    ta_t *t = TA(w);
    begin_edit(w, kind);
    if (has_sel(t)) delete_range(w, t->cursor, t->anchor);
    /* normalise CRLF / CR to LF */
    int n = (int)strlen(s);
    char *buf = kmalloc((size_t)n + 1);
    if (!buf) return;
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\r') { buf[m++] = '\n'; if (s[i + 1] == '\n') i++; }
        else buf[m++] = s[i];
    }
    insert_raw(w, buf, m);
    kfree(buf);
    after_edit(w);
}

/* ---- public API ---------------------------------------------------------------------- */
widget_t *ui_textarea(window_t *win, int x, int y, int w, int h, const char *text) {
    widget_t *wd = ui_widget_new(win, UI_TEXTAREA, x, y, w, h);
    if (!wd) return NULL;
    ta_t *t = kcalloc(1, sizeof *t);
    if (!t) { ui_destroy(wd); return NULL; }
    wd->ext = t;
    t->tab_width = 4;
    t->pref_x = -1;
    textarea_set_text(wd, text);
    ui_start_blink();
    return wd;
}

void textarea_set_text(widget_t *w, const char *text) {
    ta_t *t = TA(w);
    if (!text) text = "";
    kfree(w->text);
    int n = (int)strlen(text);
    t->cap = n + 1;
    w->text = kmalloc((size_t)t->cap);
    t->len = 0;
    if (w->text) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            if (text[i] == '\r') { w->text[m++] = '\n'; if (text[i + 1] == '\n') i++; }
            else w->text[m++] = text[i];
        }
        w->text[m] = 0;
        t->len = m;
    }
    t->cursor = t->anchor = 0;
    t->top = 0;
    t->scroll_x = 0;
    t->modified = false;
    for (int i = 0; i < t->nundo; i++) snap_free(&t->undo[i]);
    for (int i = 0; i < t->nredo; i++) snap_free(&t->redo[i]);
    t->nundo = t->nredo = 0;
    reindex(w);
    ui_invalidate(w);
    notify_cursor(w);
}

void textarea_free(widget_t *w) {
    ta_t *t = TA(w);
    if (!t) return;
    for (int i = 0; i < t->nundo; i++) snap_free(&t->undo[i]);
    for (int i = 0; i < t->nredo; i++) snap_free(&t->redo[i]);
    kfree(t->lines);
    kfree(t);
    w->ext = NULL;
}

bool textarea_wants_tab(widget_t *w) { return TA(w) && !TA(w)->readonly; }

void ui_textarea_set_readonly(widget_t *w, bool ro) { if (w && w->type == UI_TEXTAREA) { TA(w)->readonly = ro; ui_invalidate(w); } }
void ui_textarea_set_line_numbers(widget_t *w, bool on) { if (w && w->type == UI_TEXTAREA) { TA(w)->line_numbers = on; clamp_scroll(w); ui_invalidate(w); } }
void ui_textarea_set_tab_width(widget_t *w, int n) { if (w && w->type == UI_TEXTAREA) { TA(w)->tab_width = CLAMP(n, 1, 16); reindex(w); ui_invalidate(w); } }
void ui_textarea_on_cursor(widget_t *w, widget_cb cb, void *ctx) { if (w && w->type == UI_TEXTAREA) { TA(w)->on_cursor = cb; TA(w)->on_cursor_ctx = ctx; } }
bool ui_textarea_modified(widget_t *w) { return w && w->type == UI_TEXTAREA && TA(w)->modified; }
void ui_textarea_set_modified(widget_t *w, bool m) { if (w && w->type == UI_TEXTAREA) TA(w)->modified = m; }
int  ui_textarea_length(widget_t *w) { return (w && w->type == UI_TEXTAREA) ? TA(w)->len : 0; }
int  ui_textarea_line_count(widget_t *w) { return (w && w->type == UI_TEXTAREA) ? TA(w)->nlines : 0; }

void ui_textarea_insert(widget_t *w, const char *text) {
    if (!w || w->type != UI_TEXTAREA || !text) return;
    insert_text(w, text, 3);
}

void ui_textarea_append(widget_t *w, const char *text) {
    if (!w || w->type != UI_TEXTAREA || !text) return;
    ta_t *t = TA(w);
    t->cursor = t->anchor = t->len;
    int n = (int)strlen(text);
    insert_raw(w, text, n);
    reindex(w);
    ensure_visible(w);
    ui_invalidate(w);
}

void ui_textarea_get_cursor(widget_t *w, int *line, int *col) {
    if (!w || w->type != UI_TEXTAREA) return;
    ta_t *t = TA(w);
    int ln = line_of(t, t->cursor);
    if (line) *line = ln;
    if (col) *col = t->cursor - line_start(t, ln);
}

void ui_textarea_set_cursor(widget_t *w, int line, int col) {
    if (!w || w->type != UI_TEXTAREA) return;
    ta_t *t = TA(w);
    line = CLAMP(line, 0, t->nlines - 1);
    int s = line_start(t, line), e = line_end(w, line);
    t->cursor = t->anchor = CLAMP(s + col, s, e);
    t->pref_x = -1;
    ensure_visible(w);
    ui_invalidate(w);
    notify_cursor(w);
}

void ui_textarea_select(widget_t *w, int a, int b) {
    if (!w || w->type != UI_TEXTAREA) return;
    ta_t *t = TA(w);
    t->anchor = CLAMP(a, 0, t->len);
    t->cursor = CLAMP(b, 0, t->len);
    ensure_visible(w);
    ui_invalidate(w);
    notify_cursor(w);
}

void ui_textarea_get_selection(widget_t *w, int *a, int *b) {
    if (!w || w->type != UI_TEXTAREA) return;
    ta_t *t = TA(w);
    if (a) *a = MIN(t->cursor, t->anchor);
    if (b) *b = MAX(t->cursor, t->anchor);
}

bool ui_textarea_find(widget_t *w, const char *needle, bool match_case) {
    if (!w || w->type != UI_TEXTAREA || !needle || !*needle) return false;
    ta_t *t = TA(w);
    int n = (int)strlen(needle);
    int start = MAX(t->cursor, t->anchor);
    for (int pass = 0; pass < 2; pass++) {
        int from = pass == 0 ? start : 0, to = pass == 0 ? t->len : start;
        for (int i = from; i + n <= t->len && i < to + n; i++) {
            if (match_case ? !strncmp(w->text + i, needle, (size_t)n) : !strncasecmp(w->text + i, needle, (size_t)n)) {
                ui_textarea_select(w, i, i + n);
                return true;
            }
        }
    }
    return false;
}

/* ---- painting ---------------------------------------------------------------------------- */
void textarea_paint(surface_t *s, widget_t *w) {
    ta_t *t = TA(w);
    const theme_t *th = theme_get();
    bool focused = w->win->focus_widget == w && w->win == wm_focused;
    rect_t r = w->r;
    ui_draw_field(s, r, (focused ? UI_STATE_FOCUSED : 0) | (w->enabled ? 0 : UI_STATE_DISABLED));
    clamp_scroll(w);
    ta_geom_t g = geom(w);
    const font_t *f = ta_font(w);
    int lh = line_h(w);
    int a = MIN(t->cursor, t->anchor), b = MAX(t->cursor, t->anchor);
    rect_t old = s->clip;
    /* gutter */
    int gw = gutter_w(w);
    if (gw) {
        rect_t gr = rect_make(r.x + 1, r.y + 1, gw, r.h - 2 - (g.hsb ? TA_SB : 0));
        surface_set_clip(s, rect_intersect(old, gr));
        gfx_fill_rect(s, gr.x, gr.y, gr.w, gr.h, color_mix(th->field_bg, th->field_text, 14));
        int cur_line = line_of(t, t->cursor);
        for (int i = 0; i <= g.vis_lines && t->top + i < t->nlines; i++) {
            char num[12];
            snprintf(num, sizeof num, "%d", t->top + i + 1);
            int tw = font_text_width(FONT_MONO, num);
            uint32_t c = (t->top + i == cur_line) ? th->field_text : color_mix(th->field_text, th->field_bg, 120);
            gfx_text(s, gr.x + gw - 8 - tw, g.text.y + i * lh + (lh - 16) / 2, num, c, FONT_MONO);
        }
    }
    surface_set_clip(s, rect_intersect(old, rect_make(g.text.x - 2, g.text.y, g.text.w + 2, g.text.h)));
    uint32_t sel_bg = focused ? color_alpha(th->selection_bg, 120) : color_mix(th->field_bg, th->field_text, 40);
    int cur_line = line_of(t, t->cursor);
    for (int i = 0; i <= g.vis_lines && t->top + i < t->nlines; i++) {
        int ln = t->top + i;
        int ls = line_start(t, ln), le = line_end(w, ln);
        int y = g.text.y + i * lh;
        int x0 = g.text.x - t->scroll_x;
        /* selection */
        if (b > ls && a <= le && a != b) {
            int sa = MAX(a, ls), sb = MIN(b, le);
            int sx = x0 + text_width(w, ls, sa);
            int sw = text_width(w, sa, sb) + (b > le ? font_char_width(f, ' ') / 2 + 2 : 0);
            gfx_fill_rect(s, sx, y, sw, lh, sel_bg);
        }
        /* text */
        int x = 0;
        int limit = t->scroll_x + g.text.w + 64;
        for (int k = ls; k < le && x < limit; k++) {
            char c = w->text[k];
            int cw = char_w(w, c, x);
            if (c != '\t' && x + cw >= t->scroll_x)
                gfx_char(s, x0 + x, y, (unsigned char)c, w->enabled ? th->field_text : th->disabled_text, f);
            x += cw;
        }
        /* caret */
        if (ln == cur_line && focused && ui_blink_on && !t->readonly) {
            int cx = x0 + text_width(w, ls, t->cursor);
            gfx_fill_rect(s, cx, y, 2, lh, th->accent);
        }
    }
    s->clip = old;
    if (g.vsb) ui_draw_scrollbar(s, g.vsb_r, true, t->nlines, g.vis_lines, t->top, t->sb_drag == 1 ? UI_STATE_PRESSED : 0);
    if (g.hsb) ui_draw_scrollbar(s, g.hsb_r, false, t->max_w + 4, g.text.w, t->scroll_x, t->sb_drag == 2 ? UI_STATE_PRESSED : 0);
    if (g.vsb && g.hsb) gfx_fill_rect(s, g.vsb_r.x, g.hsb_r.y, TA_SB, TA_SB, th->scrollbar_bg);
}

/* ---- input ------------------------------------------------------------------------- */
static int offset_at(widget_t *w, int mx, int my) {
    ta_t *t = TA(w);
    ta_geom_t g = geom(w);
    int ln = t->top + (my - g.text.y) / line_h(w);
    if (my < g.text.y) ln = t->top - 1;
    ln = CLAMP(ln, 0, t->nlines - 1);
    int ls = line_start(t, ln), le = line_end(w, ln);
    int px = mx - g.text.x + t->scroll_x;
    int x = 0;
    for (int k = ls; k < le; k++) {
        int cw = char_w(w, w->text[k], x);
        if (px < x + cw / 2) return k;
        x += cw;
    }
    return le;
}

static bool is_word(char c) { return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }

static int word_left(widget_t *w, int i) {
    while (i > 0 && !is_word(w->text[i - 1])) i--;
    while (i > 0 && is_word(w->text[i - 1])) i--;
    return i;
}

static int word_right(widget_t *w, int i) {
    int n = TA(w)->len;
    while (i < n && !is_word(w->text[i])) i++;
    while (i < n && is_word(w->text[i])) i++;
    return i;
}

static int sb_pos(int m, int origin, int len, int total, int page, int off) {
    int tl = total > 0 ? MAX(24, (int)((int64_t)len * page / total)) : len;
    if (tl > len) tl = len;
    int maxpos = total - page;
    if (len - tl <= 0 || maxpos <= 0) return 0;
    return CLAMP((int)((int64_t)(m - origin - off) * maxpos / (len - tl)), 0, maxpos);
}

static void sb_thumb(int len, int total, int page, int pos, int *tp, int *tl) {
    *tl = total > 0 ? MAX(24, (int)((int64_t)len * page / total)) : len;
    if (*tl > len) *tl = len;
    int maxpos = total - page;
    *tp = maxpos > 0 ? (int)((int64_t)(len - *tl) * pos / maxpos) : 0;
}

static void move_caret(widget_t *w, int pos, bool extend) {
    ta_t *t = TA(w);
    t->cursor = CLAMP(pos, 0, t->len);
    if (!extend) t->anchor = t->cursor;
    ensure_visible(w);
    ui_blink_on = true;
    ui_invalidate(w);
    notify_cursor(w);
}

static void copy_sel(widget_t *w) {
    ta_t *t = TA(w);
    if (!has_sel(t)) return;
    int a = MIN(t->cursor, t->anchor), b = MAX(t->cursor, t->anchor);
    char *c = strndup(w->text + a, (size_t)(b - a));
    if (c) { clipboard_set(c); kfree(c); }
}

static bool key_down(widget_t *w, const event_t *ev) {
    ta_t *t = TA(w);
    bool shift = ev->mods & MOD_SHIFT, ctrl = ev->mods & MOD_CTRL;
    int ln = line_of(t, t->cursor);
    ta_geom_t g = geom(w);
    switch (ev->key) {
    case KEY_LEFT:
        if (has_sel(t) && !shift && !ctrl) move_caret(w, MIN(t->cursor, t->anchor), false);
        else move_caret(w, ctrl ? word_left(w, t->cursor) : t->cursor - 1, shift);
        t->pref_x = -1;
        return true;
    case KEY_RIGHT:
        if (has_sel(t) && !shift && !ctrl) move_caret(w, MAX(t->cursor, t->anchor), false);
        else move_caret(w, ctrl ? word_right(w, t->cursor) : t->cursor + 1, shift);
        t->pref_x = -1;
        return true;
    case KEY_UP: case KEY_DOWN: case KEY_PAGEUP: case KEY_PAGEDOWN: {
        if (ctrl && (ev->key == KEY_UP || ev->key == KEY_DOWN)) {   /* scroll without moving */
            t->top += ev->key == KEY_UP ? -1 : 1;
            clamp_scroll(w);
            ui_invalidate(w);
            return true;
        }
        if (t->pref_x < 0) t->pref_x = text_width(w, line_start(t, ln), t->cursor);
        int delta = ev->key == KEY_UP ? -1 : ev->key == KEY_DOWN ? 1 : ev->key == KEY_PAGEUP ? -g.vis_lines : g.vis_lines;
        int nl = CLAMP(ln + delta, 0, t->nlines - 1);
        int pos;
        if (nl == ln && delta < 0) pos = 0;
        else if (nl == ln && delta > 0) pos = t->len;
        else {
            int ls = line_start(t, nl), le = line_end(w, nl), x = 0;
            pos = le;
            for (int k = ls; k < le; k++) {
                int cw = char_w(w, w->text[k], x);
                if (t->pref_x < x + cw / 2) { pos = k; break; }
                x += cw;
            }
        }
        if (ev->key == KEY_PAGEUP || ev->key == KEY_PAGEDOWN) { t->top += delta; clamp_scroll(w); }
        int keep = t->pref_x;
        move_caret(w, pos, shift);
        t->pref_x = keep;
        return true;
    }
    case KEY_HOME: {
        int ls = line_start(t, ln);
        /* smart home: first non-blank, then column 0 */
        int fnb = ls;
        while (fnb < line_end(w, ln) && (w->text[fnb] == ' ' || w->text[fnb] == '\t')) fnb++;
        move_caret(w, ctrl ? 0 : (t->cursor == fnb ? ls : fnb), shift);
        t->pref_x = -1;
        return true;
    }
    case KEY_END:
        move_caret(w, ctrl ? t->len : line_end(w, ln), shift);
        t->pref_x = -1;
        return true;
    default:
        break;
    }
    if (ctrl) {
        switch (ev->key) {
        case 'a': t->anchor = 0; move_caret(w, t->len, true); return true;
        case 'c': copy_sel(w); return true;
        case 'x': if (!t->readonly && has_sel(t)) { copy_sel(w); begin_edit(w, 3); delete_range(w, t->cursor, t->anchor); after_edit(w); } return true;
        case 'v': if (!t->readonly) insert_text(w, clipboard_get(), 3); return true;
        case 'z': if (!t->readonly) ui_textarea_undo(w); return true;
        case 'y': if (!t->readonly) ui_textarea_redo(w); return true;
        default: return false;   /* let the app see Ctrl+S, Ctrl+F, ... */
        }
    }
    if (t->readonly) return false;
    switch (ev->key) {
    case KEY_ENTER: {
        /* keep the current line's indentation */
        int ls = line_start(t, ln), k = ls;
        char ind[64];
        int n = 0;
        while (k < t->cursor && (w->text[k] == ' ' || w->text[k] == '\t') && n < 62) ind[n++] = w->text[k++];
        ind[n] = 0;
        char buf[66];
        snprintf(buf, sizeof buf, "\n%s", ind);
        insert_text(w, buf, 3);
        return true;
    }
    case KEY_TAB:
        insert_text(w, "\t", 1);
        return true;
    case KEY_BACKSPACE:
        if (has_sel(t)) { begin_edit(w, 2); delete_range(w, t->cursor, t->anchor); }
        else if (t->cursor > 0) { begin_edit(w, 2); delete_range(w, ctrl ? word_left(w, t->cursor) : t->cursor - 1, t->cursor); }
        else return true;
        after_edit(w);
        return true;
    case KEY_DELETE:
        if (has_sel(t)) { begin_edit(w, 2); delete_range(w, t->cursor, t->anchor); }
        else if (t->cursor < t->len) { begin_edit(w, 2); delete_range(w, t->cursor, ctrl ? word_right(w, t->cursor) : t->cursor + 1); }
        else return true;
        after_edit(w);
        return true;
    default:
        return false;
    }
}

bool textarea_event(widget_t *w, const event_t *ev) {
    ta_t *t = TA(w);
    if (!t) return false;
    ta_geom_t g = geom(w);
    switch (ev->type) {
    case EV_KEY_DOWN:
        return key_down(w, ev);
    case EV_CHAR:
        if (t->readonly) return false;
        if (ev->ch >= 32) {
            char s[2] = { (char)ev->ch, 0 };
            insert_text(w, s, 1);
        }
        return true;
    case EV_MOUSE_DOWN:
        if (ev->button != MOUSE_LEFT) return true;
        if (g.vsb && rect_contains(g.vsb_r, ev->x, ev->y)) {
            int tp, tl;
            sb_thumb(g.vsb_r.h, t->nlines, g.vis_lines, t->top, &tp, &tl);
            int rel = ev->y - g.vsb_r.y;
            if (rel >= tp && rel < tp + tl) { t->sb_drag = 1; t->sb_off = rel - tp; }
            else t->top += rel < tp ? -g.vis_lines : g.vis_lines;
            clamp_scroll(w);
            ui_invalidate(w);
            return true;
        }
        if (g.hsb && rect_contains(g.hsb_r, ev->x, ev->y)) {
            int tp, tl;
            sb_thumb(g.hsb_r.w, t->max_w + 4, g.text.w, t->scroll_x, &tp, &tl);
            int rel = ev->x - g.hsb_r.x;
            if (rel >= tp && rel < tp + tl) { t->sb_drag = 2; t->sb_off = rel - tp; }
            else t->scroll_x += rel < tp ? -g.text.w : g.text.w;
            clamp_scroll(w);
            ui_invalidate(w);
            return true;
        }
        t->dragging = true;
        move_caret(w, offset_at(w, ev->x, ev->y), ev->mods & MOD_SHIFT);
        t->pref_x = -1;
        return true;
    case EV_MOUSE_MOVE:
        if (t->sb_drag == 1) {
            t->top = sb_pos(ev->y, g.vsb_r.y, g.vsb_r.h, t->nlines, g.vis_lines, t->sb_off);
            clamp_scroll(w);
            ui_invalidate(w);
        } else if (t->sb_drag == 2) {
            t->scroll_x = sb_pos(ev->x, g.hsb_r.x, g.hsb_r.w, t->max_w + 4, g.text.w, t->sb_off);
            clamp_scroll(w);
            ui_invalidate(w);
        } else if (t->dragging) {
            int pos = offset_at(w, ev->x, ev->y);
            if (pos != t->cursor) move_caret(w, pos, true);
        }
        return true;
    case EV_MOUSE_UP:
        t->dragging = false;
        t->sb_drag = 0;
        ui_invalidate(w);
        return true;
    case EV_DOUBLE_CLICK: {
        int pos = offset_at(w, ev->x, ev->y);
        int a = pos, b = pos;
        while (a > 0 && is_word(w->text[a - 1])) a--;
        while (b < t->len && is_word(w->text[b])) b++;
        ui_textarea_select(w, a, b);
        t->dragging = false;
        return true;
    }
    case EV_MOUSE_WHEEL:
        if (ev->mods & MOD_SHIFT) t->scroll_x += ev->wheel * 48;
        else t->top += ev->wheel * 3;
        clamp_scroll(w);
        ui_invalidate(w);
        return true;
    default:
        return false;
    }
}
