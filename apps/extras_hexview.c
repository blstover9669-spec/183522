/*
 * extras_hexview.c - "Hex Viewer" (System): look at the raw bytes of any file.
 *
 *   - opens a VFS file given as the launch argument, typed into the path box
 *     (Enter or the Open button) or picked with Browse... (system file dialog);
 *   - classic three-column dump: offset / 16 hex bytes (two groups of 8) /
 *     ASCII, with a sticky column header, zebra rows and colour classes
 *     (zero bytes dimmed, printable text normal, control/high bytes tinted);
 *   - a byte cursor shown in both the hex and ASCII columns, Shift-extended
 *     selections (mouse drag or Shift+arrows), hover highlight;
 *   - scrolling with the scroll bar, wheel, arrows, PgUp/PgDn, Home/End,
 *     Ctrl+Home/End; Ctrl+G jumps to an offset; Ctrl+A selects all;
 *     Ctrl+C copies the selection as hex text; F5 reloads; Ctrl+O browses;
 *   - a status bar with the cursor offset and a little data inspector
 *     (byte, u16 / u32 little-endian) or the selection size;
 *   - follows the file: when the VFS changes and the file's size or mtime
 *     differs, it is reloaded in the background (polled from a 1 s timer).
 *
 * Painting only ever touches the visible rows, so even multi-megabyte files
 * scroll instantly. All state is per window; several viewers can be open.
 */
#include <nova.h>

#define HV_W        700
#define HV_H        500
#define HV_MIN_W    660
#define HV_MIN_H    260
#define TB_H        48          /* toolbar with the path box                */
#define ST_H        26          /* status bar                               */
#define SB_W        14          /* scroll bar width                         */
#define HDR_H       24          /* column header inside the view            */
#define ROW_H       18
#define CW          8           /* FONT_MONO cell width                     */
#define BPR         16          /* bytes per row                            */
#define MAX_FILE    (32u << 20) /* refuse files above 32 MiB                */

/* column geometry (relative to the view's left edge) */
#define X_OFF       14
#define X_HEX       (X_OFF + 8 * CW + 22)
#define HEX_STEP    24          /* "XX" + gap                               */
#define HEX_GAP     10          /* extra gap between the two groups of 8    */
#define X_ASC       (X_HEX + BPR * HEX_STEP + HEX_GAP + 18)
#define VIEW_W      (X_ASC + BPR * CW + 14)

typedef struct {
    window_t *win;
    widget_t *path_box, *open_btn, *browse_btn, *view, *sb;
    char path[VFS_PATH_MAX];      /* file being shown ("" = none)            */
    char err[160];                /* load error, shown in the empty view    */
    uint8_t *data;
    uint32_t size;
    uint32_t mtime;
    uint32_t gen;                 /* vfs_generation() when last checked     */
    int top;                      /* first visible row                      */
    uint32_t cur, anchor;         /* cursor byte and selection anchor       */
    int hover;                    /* hovered byte or -1                     */
    bool dragging;
    char msg[120];                /* transient status message               */
    uint32_t msg_until;
    int timer;
} hexview_t;

/* ---- helpers ------------------------------------------------------------------ */
static int hv_rows_total(hexview_t *h) { return (int)((h->size + BPR - 1) / BPR); }

static int hv_rows_visible(hexview_t *h) {
    rect_t r = ui_rect(h->view);
    return MAX(1, (r.h - HDR_H) / ROW_H);
}

static int hv_max_top(hexview_t *h) { return MAX(0, hv_rows_total(h) - hv_rows_visible(h)); }

static void hv_message(hexview_t *h, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void hv_message(hexview_t *h, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(h->msg, sizeof h->msg, fmt, ap);
    va_end(ap);
    h->msg_until = sys_uptime_ms() + 3000;
    win_invalidate_rect(h->win, 0, win_height(h->win) - ST_H, win_width(h->win), ST_H);
}

static void hv_status_dirty(hexview_t *h) {
    win_invalidate_rect(h->win, 0, win_height(h->win) - ST_H, win_width(h->win), ST_H);
}

static void hv_sync_scrollbar(hexview_t *h) {
    ui_scrollbar_set(h->sb, MAX(hv_rows_total(h), 1), hv_rows_visible(h), h->top);
}

static void hv_set_top(hexview_t *h, int top) {
    top = CLAMP(top, 0, hv_max_top(h));
    if (top == h->top) return;
    h->top = top;
    hv_sync_scrollbar(h);
    ui_invalidate(h->view);
}

static void hv_ensure_visible(hexview_t *h) {
    int row = (int)(h->cur / BPR), vis = hv_rows_visible(h);
    if (row < h->top) hv_set_top(h, row);
    else if (row >= h->top + vis) hv_set_top(h, row - vis + 1);
}

static void hv_move_cursor(hexview_t *h, int64_t pos, bool extend) {
    if (!h->size) return;
    if (pos < 0) pos = 0;
    if (pos >= (int64_t)h->size) pos = h->size - 1;
    h->cur = (uint32_t)pos;
    if (!extend) h->anchor = h->cur;
    hv_ensure_visible(h);
    ui_invalidate(h->view);
    hv_status_dirty(h);
}

static void hv_update_title(hexview_t *h) {
    char t[VFS_NAME_MAX + 24];
    if (h->path[0] && h->data) snprintf(t, sizeof t, "%s - Hex Viewer", vfs_basename(h->path));
    else snprintf(t, sizeof t, "Hex Viewer");
    win_set_title(h->win, t);
}

static void fmt_size(char *out, size_t n, uint32_t bytes) {
    /* 1,234,567 bytes */
    char digits[16];
    snprintf(digits, sizeof digits, "%u", bytes);
    int len = (int)strlen(digits), o = 0;
    for (int i = 0; i < len && o < (int)n - 1; i++) {
        if (i && (len - i) % 3 == 0 && o < (int)n - 2) out[o++] = ',';
        out[o++] = digits[i];
    }
    out[o] = 0;
}

/* (Re)load h->path. keep_pos keeps scroll and cursor (used for live reloads). */
static bool hv_load(hexview_t *h, bool keep_pos) {
    vfs_stat_t st;
    h->err[0] = 0;
    int rc = vfs_stat(h->path, &st);
    uint8_t *d = NULL;
    uint32_t size = 0;
    if (rc < 0) snprintf(h->err, sizeof h->err, "Cannot open %s: %s", h->path, vfs_strerror(rc));
    else if (st.is_dir) snprintf(h->err, sizeof h->err, "%s is a folder, not a file", h->path);
    else if (st.size > MAX_FILE) snprintf(h->err, sizeof h->err, "%s is too large (over 32 MB)", vfs_basename(h->path));
    else {
        d = vfs_read_file(h->path, &size);
        if (!d) snprintf(h->err, sizeof h->err, "Cannot read %s", h->path);
    }
    if (h->err[0]) {
        free(h->data);
        h->data = NULL;
        h->size = 0;
        h->top = 0;
        h->cur = h->anchor = 0;
        klog("hexview: %s", h->err);
    } else {
        free(h->data);
        h->data = d;
        h->size = size;
        h->mtime = st.mtime;
        if (!keep_pos) {
            h->top = 0;
            h->cur = h->anchor = 0;
        } else {
            if (h->cur >= size) h->cur = size ? size - 1 : 0;
            if (h->anchor >= size) h->anchor = size ? size - 1 : 0;
            h->top = CLAMP(h->top, 0, hv_max_top(h));
        }
        klog("hexview: opened %s (%u bytes)", h->path, size);
    }
    h->gen = vfs_generation();
    h->hover = -1;
    hv_sync_scrollbar(h);
    hv_update_title(h);
    win_invalidate(h->win);
    return !h->err[0];
}

static void hv_open_path(hexview_t *h, const char *path) {
    char norm[VFS_PATH_MAX];
    if (!path || !*path) return;
    if (vfs_normalize("/home/user", path, norm) < 0) {
        hv_message(h, "Not a valid path: %s", path);
        return;
    }
    strlcpy(h->path, norm, sizeof h->path);
    ui_set_text(h->path_box, h->path);
    if (hv_load(h, false)) ui_focus(h->view);
}

/* ---- toolbar callbacks ------------------------------------------------------------- */
static void on_open(widget_t *w, void *ctx) {
    hexview_t *h = ctx;
    hv_open_path(h, ui_get_text(h->path_box));
}

static void on_browse_result(const char *path, void *ctx) {
    hexview_t *h = ctx;
    if (path) hv_open_path(h, path);
}

static void hv_browse(hexview_t *h) {
    char dir[VFS_PATH_MAX];
    if (h->path[0]) vfs_dirname(h->path, dir);
    else strlcpy(dir, "/home/user", sizeof dir);
    ui_file_dialog(h->win, UI_FILE_OPEN, "Open file in Hex Viewer", dir, NULL, on_browse_result, h);
}

static void on_browse(widget_t *w, void *ctx) { hv_browse((hexview_t *)ctx); }

static void on_scroll(widget_t *w, void *ctx) {
    hexview_t *h = ctx;
    int v = ui_get_value(w);
    if (v != h->top) {
        h->top = CLAMP(v, 0, hv_max_top(h));
        ui_invalidate(h->view);
    }
}

static void on_goto_result(const char *text, void *ctx) {
    hexview_t *h = ctx;
    if (!text) return;
    while (*text == ' ') text++;
    if (!*text) return;
    bool hex = false;
    const char *p = text;
    char buf[32];
    strlcpy(buf, text, sizeof buf);
    int n = (int)strlen(buf);
    while (n > 0 && buf[n - 1] == ' ') buf[--n] = 0;
    if (n > 0 && (buf[n - 1] == 'h' || buf[n - 1] == 'H')) { buf[--n] = 0; hex = true; }
    p = buf;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; hex = true; }
    if (p[0] == '$') { p++; hex = true; }
    for (const char *q = p; *q; q++)
        if (isxdigit((unsigned char)*q) && !isdigit((unsigned char)*q)) hex = true;
    char *end = NULL;
    unsigned long v = strtoul(p, &end, hex ? 16 : 10);
    if (!*p || (end && *end)) {
        hv_message(h, "\"%s\" is not an offset", text);
        return;
    }
    if (!h->size) return;
    if (v >= h->size) {
        hv_message(h, "Offset 0x%lX is past the end of the file", v);
        v = h->size - 1;
    }
    hv_move_cursor(h, (int64_t)v, false);
    /* show the target row near the top third */
    hv_set_top(h, (int)(v / BPR) - hv_rows_visible(h) / 3);
    ui_focus(h->view);
}

static void hv_goto(hexview_t *h) {
    if (!h->data) return;
    char init[16];
    snprintf(init, sizeof init, "0x%X", h->cur);
    ui_input_box(h->win, "Go to offset", "Offset (decimal, or hex like 0x1F0 or 1F0h):", init, on_goto_result, h);
}

static void hv_copy(hexview_t *h) {
    if (!h->data || !h->size) return;
    uint32_t a = MIN(h->cur, h->anchor), b = MAX(h->cur, h->anchor);
    uint32_t n = b - a + 1;
    bool cut = false;
    if (n > 65536) { n = 65536; cut = true; }
    char *txt = malloc((size_t)n * 3 + 1);
    if (!txt) return;
    static const char hx[] = "0123456789ABCDEF";
    int o = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t v = h->data[a + i];
        if (i) txt[o++] = (i % BPR) ? ' ' : '\n';
        txt[o++] = hx[v >> 4];
        txt[o++] = hx[v & 15];
    }
    txt[o] = 0;
    clipboard_set(txt);
    free(txt);
    if (cut) hv_message(h, "Copied the first 65,536 bytes as hex");
    else hv_message(h, "Copied %u byte%s as hex", n, n == 1 ? "" : "s");
}

/* ---- the dump view (custom widget) --------------------------------------------------- */
static int hex_x(int col) { return X_HEX + col * HEX_STEP + (col >= 8 ? HEX_GAP : 0); }

/* Byte offset under a view-relative point, or -1. */
static int hv_hit(hexview_t *h, int x, int y, bool clamp) {
    rect_t r = ui_rect(h->view);
    x -= r.x;
    y -= r.y;
    if (!h->size) return -1;
    if (y < HDR_H) { if (!clamp) return -1; y = HDR_H; }
    int row = h->top + (y - HDR_H) / ROW_H;
    int col = -1;
    if (x >= X_ASC - 6) {
        col = (x - X_ASC) / CW;
        if (x < X_ASC) col = 0;
        if (col >= BPR) { if (!clamp) return -1; col = BPR - 1; }
    } else if (x >= X_HEX - 6) {
        for (int c = BPR - 1; c >= 0; c--)
            if (x >= hex_x(c) - 6) { col = c; break; }
        if (col < 0) col = 0;
    } else {
        if (!clamp) return -1;
        col = 0;
    }
    int64_t off = (int64_t)row * BPR + col;
    if (off >= (int64_t)h->size) {
        if (!clamp) return -1;
        off = h->size - 1;
    }
    if (off < 0) off = 0;
    return (int)off;
}

static void view_draw(widget_t *w, surface_t *s, rect_t r) {
    hexview_t *h = ui_get_ctx(w);
    const theme_t *t = theme_get();
    rect_t old = s->clip;
    surface_set_clip(s, rect_intersect(old, r));
    uint32_t bg = t->field_bg, fg = t->field_text;
    gfx_fill_rect(s, r.x, r.y, r.w, r.h, bg);

    uint32_t dim = color_mix(fg, bg, 120);
    uint32_t faint = color_mix(fg, bg, 190);
    uint32_t off_col = color_mix(t->accent, fg, t->dark ? 60 : 110);
    uint32_t high_col = t->dark ? RGB(240, 170, 90) : RGB(184, 92, 20);
    uint32_t line = color_mix(bg, t->window_border, 140);
    bool focused = ui_has_focus(w) && win_is_focused(h->win);

    if (!h->data) {
        /* empty state */
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2 - 36;
        uint32_t ink = color_mix(fg, bg, 110);
        ui_draw_glyph(s, UI_GLYPH_FILE, cx - 24, cy - 28, 48, ink);
        const char *l1 = h->err[0] ? h->err : "No file open";
        const char *l2 = h->err[0] ? "Check the path, or pick a file with Browse..."
                                   : "Type a path above and press Enter, or click Browse...";
        gfx_text_aligned(s, rect_make(r.x + 16, cy + 30, r.w - 32, 26), l1,
                         h->err[0] ? (t->dark ? RGB(255, 140, 130) : RGB(190, 40, 30)) : fg,
                         FONT_UI_LARGE, ALIGN_CENTER | ALIGN_MIDDLE);
        gfx_text_aligned(s, rect_make(r.x + 16, cy + 58, r.w - 32, 20), l2, dim, FONT_UI,
                         ALIGN_CENTER | ALIGN_MIDDLE);
        s->clip = old;
        return;
    }

    int x0 = r.x;
    uint32_t a = MIN(h->cur, h->anchor), b = MAX(h->cur, h->anchor);
    int cur_col = (int)(h->cur % BPR);
    int vis = hv_rows_visible(h) + 1;
    uint32_t sel_bg = focused ? color_mix(bg, t->selection_bg, 110) : color_mix(bg, fg, 34);
    uint32_t hover_bg = color_mix(bg, t->accent, 40);

    /* rows */
    for (int i = 0; i < vis; i++) {
        int row = h->top + i;
        if (row >= hv_rows_total(h)) break;
        int y = r.y + HDR_H + i * ROW_H;
        if (y >= r.y + r.h) break;
        if (row & 1) gfx_fill_rect(s, r.x, y, r.w, ROW_H, color_mix(bg, fg, t->dark ? 10 : 8));
        uint32_t base = (uint32_t)row * BPR;
        char offs[12];
        snprintf(offs, sizeof offs, "%08X", base);
        bool cur_row = h->cur / BPR == (uint32_t)row;
        gfx_text(s, x0 + X_OFF, y + 1, offs, cur_row ? t->accent : off_col, cur_row ? FONT_MONO_BOLD : FONT_MONO);
        for (int c = 0; c < BPR; c++) {
            uint32_t off = base + c;
            if (off >= h->size) break;
            uint8_t v = h->data[off];
            int hx = x0 + hex_x(c), ax = x0 + X_ASC + c * CW;
            bool sel = a != b && off >= a && off <= b;
            if (sel) {
                /* join neighbouring selected cells into one band */
                bool next_sel = off + 1 <= b && c + 1 < BPR && off + 1 < h->size;
                int ww = next_sel ? (hex_x(c + 1) - hex_x(c)) : 2 * CW + 6;
                gfx_fill_rect(s, hx - 3, y, ww, ROW_H, sel_bg);
                gfx_fill_rect(s, ax, y, CW, ROW_H, sel_bg);
            } else if ((int)off == h->hover) {
                gfx_fill_rounded_rect(s, hx - 3, y + 1, 2 * CW + 6, ROW_H - 2, 3, hover_bg);
                gfx_fill_rect(s, ax, y + 1, CW, ROW_H - 2, hover_bg);
            }
            if (off == h->cur) {
                uint32_t cc = focused ? t->accent : dim;
                gfx_fill_rounded_rect(s, hx - 3, y + 1, 2 * CW + 6, ROW_H - 2, 3, color_alpha(cc, focused ? 70 : 50));
                gfx_rounded_rect(s, hx - 3, y + 1, 2 * CW + 6, ROW_H - 2, 3, cc);
                gfx_fill_rect(s, ax, y + 1, CW, ROW_H - 2, color_alpha(cc, focused ? 70 : 50));
                gfx_rect(s, ax, y + 1, CW, ROW_H - 2, cc);
            }
            static const char hexd[] = "0123456789ABCDEF";
            uint32_t col = v == 0 ? (sel ? dim : faint) : (v >= 0x20 && v < 0x7F) ? fg : high_col;
            if ((int)off == (int)h->cur && focused) col = t->dark ? COLOR_WHITE : fg;
            gfx_char(s, hx, y + 1, (unsigned char)hexd[v >> 4], col, FONT_MONO);
            gfx_char(s, hx + CW, y + 1, (unsigned char)hexd[v & 15], col, FONT_MONO);
            if (v >= 0x20 && v < 0x7F) gfx_char(s, ax, y + 1, v, fg, FONT_MONO);
            else gfx_char(s, ax, y + 1, '.', faint, FONT_MONO);
        }
    }

    /* column separators */
    gfx_vline(s, x0 + X_HEX - 12, r.y, r.h, line);
    gfx_vline(s, x0 + X_ASC - 10, r.y, r.h, line);

    /* sticky header */
    uint32_t hbg = color_mix(bg, fg, t->dark ? 22 : 14);
    gfx_fill_rect(s, r.x, r.y, r.w, HDR_H, hbg);
    gfx_hline(s, r.x, r.y + HDR_H - 1, r.w, line);
    int hy = r.y + (HDR_H - 16) / 2;
    gfx_text(s, x0 + X_OFF, hy, "Offset", dim, FONT_MONO_BOLD);
    for (int c = 0; c < BPR; c++) {
        char lab[4];
        snprintf(lab, sizeof lab, "%02X", c);
        bool on = c == cur_col;
        gfx_text(s, x0 + hex_x(c), hy, lab, on ? t->accent : dim, on ? FONT_MONO_BOLD : FONT_MONO);
    }
    gfx_text(s, x0 + X_ASC, hy, "ASCII", dim, FONT_MONO_BOLD);
    gfx_vline(s, x0 + X_HEX - 12, r.y, HDR_H, line);
    gfx_vline(s, x0 + X_ASC - 10, r.y, HDR_H, line);
    s->clip = old;
}

static bool view_event(widget_t *w, const event_t *ev) {
    hexview_t *h = ui_get_ctx(w);
    switch (ev->type) {
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_LEFT && h->data) {
            int off = hv_hit(h, ev->x, ev->y, true);
            if (off >= 0) hv_move_cursor(h, off, (ev->mods & MOD_SHIFT) != 0);
            h->dragging = true;
        }
        return true;
    case EV_MOUSE_MOVE: {
        if (h->dragging && (ev->buttons & MOUSE_LEFT) && h->data) {
            rect_t r = ui_rect(w);
            int off = hv_hit(h, ev->x, ev->y, true);
            /* auto-scroll while dragging above / below the view */
            if (ev->y < r.y + HDR_H && h->top > 0) { hv_set_top(h, h->top - 1); off = hv_hit(h, ev->x, r.y + HDR_H, true); }
            else if (ev->y >= r.y + r.h) { hv_set_top(h, h->top + 1); off = hv_hit(h, ev->x, r.y + r.h - 1, true); }
            if (off >= 0 && (uint32_t)off != h->cur) hv_move_cursor(h, off, true);
            return true;
        }
        int off = hv_hit(h, ev->x, ev->y, false);
        if (off != h->hover) { h->hover = off; ui_invalidate(w); }
        return true;
    }
    case EV_MOUSE_UP:
        h->dragging = false;
        return true;
    case EV_MOUSE_LEAVE:
        if (h->hover >= 0) { h->hover = -1; ui_invalidate(w); }
        return false;
    case EV_MOUSE_WHEEL:
        hv_set_top(h, h->top + ev->wheel * 3);
        return true;
    case EV_DOUBLE_CLICK:
        return true;
    case EV_KEY_DOWN: {
        if (!h->data || !h->size) return false;
        bool sh = (ev->mods & MOD_SHIFT) != 0, ctrl = (ev->mods & MOD_CTRL) != 0;
        int64_t c = h->cur;
        int page = hv_rows_visible(h) * BPR;
        switch (ev->key) {
        case KEY_LEFT: hv_move_cursor(h, c - 1, sh); return true;
        case KEY_RIGHT: hv_move_cursor(h, c + 1, sh); return true;
        case KEY_UP:
            if (ctrl) { hv_set_top(h, h->top - 1); return true; }
            if (c >= BPR) hv_move_cursor(h, c - BPR, sh);
            return true;
        case KEY_DOWN:
            if (ctrl) { hv_set_top(h, h->top + 1); return true; }
            if (c + BPR < (int64_t)h->size) hv_move_cursor(h, c + BPR, sh);
            else if (c / BPR < (int64_t)(h->size - 1) / BPR) hv_move_cursor(h, h->size - 1, sh);
            return true;
        case KEY_PAGEUP:
            hv_set_top(h, h->top - hv_rows_visible(h));
            hv_move_cursor(h, c >= page ? c - page : c % BPR, sh);
            return true;
        case KEY_PAGEDOWN:
            hv_set_top(h, h->top + hv_rows_visible(h));
            hv_move_cursor(h, c + page < (int64_t)h->size ? c + page : (int64_t)h->size - 1, sh);
            return true;
        case KEY_HOME: hv_move_cursor(h, ctrl ? 0 : c - c % BPR, sh); return true;
        case KEY_END: hv_move_cursor(h, ctrl ? (int64_t)h->size - 1 : c - c % BPR + BPR - 1, sh); return true;
        case 'a':
            if (ctrl) {
                h->anchor = 0;
                h->cur = h->size - 1;
                hv_ensure_visible(h);
                ui_invalidate(w);
                hv_status_dirty(h);
                return true;
            }
            return false;
        case 'c':
            if (ctrl) { hv_copy(h); return true; }
            return false;
        case KEY_ESCAPE:
            if (h->anchor != h->cur) { h->anchor = h->cur; ui_invalidate(w); hv_status_dirty(h); return true; }
            return false;
        default:
            return false;
        }
    }
    default:
        return false;
    }
}

/* ---- window ------------------------------------------------------------------------ */
static void hv_layout(hexview_t *h) {
    int W = win_width(h->win), H = win_height(h->win);
    int bw_open = 76, bw_browse = 92, gap = 8;
    int px = 14, pw = W - px - 14 - bw_open - gap - bw_browse - gap;
    ui_move(h->path_box, px, 10, MAX(pw, 80), 28);
    ui_move(h->open_btn, W - 14 - bw_browse - gap - bw_open, 10, bw_open, 28);
    ui_move(h->browse_btn, W - 14 - bw_browse, 10, bw_browse, 28);
    int vh = H - TB_H - ST_H;
    ui_move(h->view, 0, TB_H, W - SB_W, vh);
    ui_move(h->sb, W - SB_W, TB_H, SB_W, vh);
    h->top = CLAMP(h->top, 0, hv_max_top(h));
    hv_sync_scrollbar(h);
}

static void hv_draw_status(hexview_t *h, surface_t *s) {
    const theme_t *t = theme_get();
    int W = s->w, sy = s->h - ST_H;
    uint32_t line = color_mix(t->window_bg, t->window_border, 90);
    uint32_t muted = color_mix(t->window_text, t->window_bg, 90);
    gfx_fill_rect(s, 0, sy, W, ST_H, t->dark ? color_shade(t->window_bg, -10) : color_shade(t->window_bg, -6));
    gfx_hline(s, 0, sy, W, line);
    int ty = sy + 1 + (ST_H - 1 - font_height(FONT_UI)) / 2;
    char right[48] = "", left[200] = "";
    if (h->data) {
        char sz[20];
        fmt_size(sz, sizeof sz, h->size);
        snprintf(right, sizeof right, "%s byte%s", sz, h->size == 1 ? "" : "s");
    }
    int rw = font_text_width(FONT_UI, right);
    gfx_text(s, W - 14 - rw, ty, right, muted, FONT_UI);
    int maxw = W - 40 - rw;
    if (h->msg[0] && sys_uptime_ms() < h->msg_until) {
        gfx_text_ellipsis(s, 14, ty, maxw, h->msg, t->window_text, FONT_UI_BOLD);
        return;
    }
    if (!h->data) {
        gfx_text_ellipsis(s, 14, ty, maxw, h->err[0] ? "Could not open the file" : "Ready", muted, FONT_UI);
        return;
    }
    if (!h->size) {
        gfx_text_ellipsis(s, 14, ty, maxw, "The file is empty", muted, FONT_UI);
        return;
    }
    uint32_t a = MIN(h->cur, h->anchor), b = MAX(h->cur, h->anchor);
    if (a != b) {
        char n[20];
        fmt_size(n, sizeof n, b - a + 1);
        snprintf(left, sizeof left, "Selection 0x%X - 0x%X    (%s bytes)    Ctrl+C copies as hex", a, b, n);
    } else {
        uint32_t c = h->cur;
        uint8_t v = h->data[c];
        char ch[8];
        if (v >= 0x20 && v < 0x7F) snprintf(ch, sizeof ch, "'%c'", v);
        else snprintf(ch, sizeof ch, "--");
        int o = snprintf(left, sizeof left, "Offset 0x%08X (%u)    Byte 0x%02X = %u %s", c, c, v, v, ch);
        if (c + 1 < h->size) {
            uint32_t u16 = h->data[c] | (h->data[c + 1] << 8);
            o += snprintf(left + o, sizeof left - (size_t)o, "    u16 %u", u16);
        }
        if (c + 3 < h->size) {
            uint32_t u32 = h->data[c] | (h->data[c + 1] << 8) | (h->data[c + 2] << 16) | ((uint32_t)h->data[c + 3] << 24);
            snprintf(left + o, sizeof left - (size_t)o, "    u32 %u", u32);
        }
    }
    gfx_text_ellipsis(s, 14, ty, maxw, left, t->window_text, FONT_UI);
}

static void hv_event(window_t *win, const event_t *ev) {
    hexview_t *h = win_get_userdata(win);
    if (!h) return;
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        const theme_t *t = theme_get();
        uint32_t line = color_mix(t->window_bg, t->window_border, 90);
        gfx_hline(s, 0, TB_H - 1, s->w, line);
        hv_draw_status(h, s);
        break;
    }
    case EV_RESIZE:
        hv_layout(h);
        break;
    case EV_KEY_DOWN:
        if ((ev->mods & MOD_CTRL) && ev->key == 'o') { hv_browse(h); break; }
        if ((ev->mods & MOD_CTRL) && ev->key == 'g') { hv_goto(h); break; }
        if ((ev->mods & MOD_CTRL) && ev->key == 'l') { ui_focus(h->path_box); ui_textbox_select_all(h->path_box); break; }
        if (ev->key == KEY_F5) {
            if (h->path[0]) { hv_load(h, true); hv_message(h, "Reloaded"); }
            break;
        }
        if (ev->key == KEY_ESCAPE && ui_has_focus(h->path_box) && h->data) { ui_focus(h->view); break; }
        break;
    case EV_TIMER: {
        if (h->msg[0] && sys_uptime_ms() >= h->msg_until) { h->msg[0] = 0; hv_status_dirty(h); }
        uint32_t g = vfs_generation();
        if (g != h->gen && h->path[0]) {
            h->gen = g;
            vfs_stat_t st;
            int rc = vfs_stat(h->path, &st);
            if (h->data && (rc < 0 || st.is_dir)) {
                hv_load(h, true);           /* shows "cannot open" */
            } else if (rc == 0 && !st.is_dir && (!h->data || st.size != h->size || st.mtime != h->mtime)) {
                if (hv_load(h, true)) hv_message(h, "The file changed on disk and was reloaded");
            }
        }
        break;
    }
    case EV_FOCUS: case EV_BLUR:
        ui_invalidate(h->view);
        break;
    case EV_CLOSE:
        free(h->data);
        free(h);
        win_set_userdata(win, NULL);
        break;
    default:
        break;
    }
}

static void hv_launch(const char *arg) {
    hexview_t *h = calloc(1, sizeof *h);
    if (!h) return;
    window_t *win = win_create("Hex Viewer", HV_W, HV_H, WIN_RESIZABLE);
    if (!win) { free(h); return; }
    h->win = win;
    h->hover = -1;
    win_set_userdata(win, h);
    win_set_event_handler(win, hv_event);
    win_set_min_size(win, HV_MIN_W, HV_MIN_H);

    h->path_box = ui_textbox(win, 14, 10, 300, 28, "");
    ui_textbox_set_placeholder(h->path_box, "File path, e.g. /system/version.txt");
    ui_textbox_on_submit(h->path_box, on_open, h);
    h->open_btn = ui_button(win, 0, 10, 76, 28, "Open", on_open, h);
    h->browse_btn = ui_button(win, 0, 10, 92, 28, "Browse...", on_browse, h);
    h->view = ui_custom(win, 0, TB_H, 100, 100, view_draw, view_event, h);
    h->sb = ui_scrollbar(win, 0, TB_H, SB_W, 100, true, on_scroll, h);
    hv_layout(h);
    h->timer = win_set_timer(win, 1000);
    h->gen = vfs_generation();

    if (arg && *arg) hv_open_path(h, arg);
    else ui_focus(h->path_box);
}

/* Icon: a dark "dump" tile with offset / hex / ASCII columns and a magnifier. */
static void hv_icon(surface_t *s, int x, int y, int size) {
    int m = size / 16, w = size - 2 * m;
    int r = size / 6;
    gfx_fill_rounded_gradient_v(s, x + m, y + m, w, w, r, RGB(52, 62, 86), RGB(26, 31, 46));
    /* header strip */
    gfx_fill_rect(s, x + m + r / 2, y + m + size / 7, w - r, MAX(1, size / 48), RGBA(255, 255, 255, 60));
    int rows = 4;
    int top = y + m + size / 5, rh = (w - size / 5 - size / 8) / rows;
    int chip_h = MAX(2, rh * 5 / 9);
    int ox = x + m + size / 10;
    int hx = ox + size / 7;
    int cw = size / 9, gap = size / 40 + 1;
    int ax = hx + 3 * (cw + gap) + size / 24;
    static const uint32_t pal[4] = { 0xFF5FD3F3u, 0xFF7EE08Au, 0xFFF6B25Eu, 0xFFB79CFFu };
    for (int i = 0; i < rows; i++) {
        int ry = top + i * rh;
        /* offset chip */
        gfx_fill_rounded_rect(s, ox, ry, size / 10, chip_h, chip_h / 3, RGBA(120, 170, 255, 200));
        for (int c = 0; c < 3; c++) {
            uint32_t col = pal[(i + c * 3) & 3];
            if ((i == 1 && c == 2) || (i == 3 && c == 0)) col = RGBA(255, 255, 255, 70);
            gfx_fill_rounded_rect(s, hx + c * (cw + gap), ry, cw, chip_h, chip_h / 3, col);
        }
        /* ascii column */
        gfx_fill_rect(s, ax, ry + chip_h / 4, size / 7, MAX(1, chip_h / 2), RGBA(255, 255, 255, 150));
    }
    /* magnifier */
    double lx = x + size * 0.70, ly = y + size * 0.70, lr = size * 0.17;
    gfx_thick_line(s, (int)(lx + lr * 0.6), (int)(ly + lr * 0.6), x + size - m - size / 24, y + size - m - size / 24,
                   MAX(2, size / 11), RGB(235, 238, 245));
    gfx_fill_circle_aa(s, lx, ly, lr + size / 28.0, RGB(235, 238, 245));
    gfx_fill_circle_aa(s, lx, ly, lr - size / 40.0, RGB(40, 120, 200));
    gfx_fill_circle_aa(s, lx - lr * 0.3, ly - lr * 0.3, lr * 0.3, RGBA(255, 255, 255, 110));
}

REGISTER_APP(.name = "Hex Viewer", .category = CAT_SYSTEM,
             .description = "Inspect the raw bytes of any file",
             .launch = hv_launch, .draw_icon = hv_icon,
             .file_types = "bin,dat,raw,hex,img,nvfs");
