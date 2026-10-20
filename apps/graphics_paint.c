/*
 * graphics_paint.c - "Paint": a bitmap painting program.
 *
 * Tools: pencil, round brush (7 sizes), eraser, flood fill (iterative
 * scanline, no recursion), colour picker, line, rectangle, filled rectangle,
 * ellipse and filled ellipse. The left mouse button paints with the primary
 * colour, the right button with the secondary colour. A 28-colour palette, six
 * custom colour slots and an HSV colour editor dialog. Multi-level undo/redo,
 * zoom 1x-16x with an optional pixel grid, image operations (flip, rotate,
 * invert, grayscale, resize canvas, scale) and New/Open/Save as 24-bit BMP
 * through the VFS (codec in graphics_bmp.c). launch(arg) opens a BMP file.
 *
 * Undo model: `backup` holds the canvas as it was before the current stroke.
 * Strokes draw straight into the canvas (a shape preview is erased by copying
 * the previous preview rectangle back from `backup`). When the stroke ends the
 * touched rectangle of `backup` becomes the undo record and the canvas
 * rectangle is copied into `backup`. Undo and redo swap the record's pixels
 * with the canvas, so one record serves both directions. Operations that
 * change the canvas size keep the whole previous canvas instead.
 *
 * All state is per window (paint_t) and freed on EV_CLOSE; the only globals
 * are immutable caches of the tool icons.
 */
#include <nova.h>
#include "graphics_bmp.h"

#define PT_W        860
#define PT_H        600
#define MENU_H      UI_MENUBAR_H
#define TOOLBOX_W   80
#define PALETTE_H   60
#define STATUS_H    24
#define SB_SIZE     12
#define PAD         16           /* margin around the canvas when it scrolls */
#define MAX_DIM     2048
#define DEF_CW      640
#define DEF_CH      400
#define MAX_UNDO    48
#define UNDO_BUDGET (40u << 20)   /* bytes of pixels kept for undo per window */
#define MSG_MS      4000

enum { T_PENCIL, T_BRUSH, T_ERASER, T_FILL, T_PICKER, T_LINE, T_RECT, T_FRECT, T_ELLIPSE, T_FELLIPSE, T_COUNT };

static const char *const g_tool_names[T_COUNT] = {
    "Pencil", "Brush", "Eraser", "Fill", "Colour picker", "Line",
    "Rectangle", "Filled rectangle", "Ellipse", "Filled ellipse"
};
static const char *const g_tool_keys[T_COUNT] = { "P", "B", "E", "F", "I", "L", "R", "Shift+R", "O", "Shift+O" };

static const int g_sizes[] = { 1, 2, 3, 5, 8, 12, 20 };
#define NSIZES ARRAY_SIZE(g_sizes)
static const int g_zooms[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
#define NZOOMS ARRAY_SIZE(g_zooms)

#define PAL_COLS 14
#define NPAL     28
#define NCUSTOM  6
static const uint32_t g_palette[NPAL] = {
    RGB(0, 0, 0),       RGB(64, 64, 64),    RGB(127, 127, 127), RGB(136, 0, 21),    RGB(237, 28, 36),
    RGB(255, 127, 39),  RGB(255, 242, 0),   RGB(34, 177, 76),   RGB(0, 162, 232),   RGB(63, 72, 204),
    RGB(163, 73, 164),  RGB(0, 110, 100),   RGB(24, 44, 110),   RGB(96, 56, 24),
    RGB(255, 255, 255), RGB(195, 195, 195), RGB(225, 225, 225), RGB(185, 122, 87),  RGB(255, 174, 201),
    RGB(255, 201, 14),  RGB(239, 228, 176), RGB(181, 230, 29),  RGB(153, 217, 234), RGB(112, 146, 190),
    RGB(200, 191, 231), RGB(120, 214, 190), RGB(150, 178, 255), RGB(222, 184, 135),
};

/* palette bar hit codes */
#define HIT_NONE   -1
#define HIT_CUSTOM 100   /* + slot */
#define HIT_FG     200
#define HIT_BG     201
#define HIT_EDIT   300

enum { ACT_NONE, ACT_NEW, ACT_OPEN, ACT_CLOSE };

typedef struct undo {
    bool full;            /* pix is the whole previous canvas            */
    int x, y;             /* region origin (when !full)                  */
    surface_t *pix;
    uint32_t id;
} undo_t;

typedef struct paint {
    window_t *win;
    widget_t *menubar, *hsb, *vsb;
    surface_t *canvas, *backup;
    char path[VFS_PATH_MAX];           /* "" = untitled                   */
    char pending_path[VFS_PATH_MAX];   /* save-as target awaiting "overwrite?" */
    int  pending;                      /* ACT_* to run after save/discard */

    int tool, prev_tool, size_idx;
    uint32_t fg, bg;
    uint32_t custom[NCUSTOM];          /* 0 = empty slot                  */
    int next_custom;

    int zoom_idx, scroll_x, scroll_y;
    rect_t view;                       /* visible canvas viewport (client coords) */
    int org_x, org_y;                  /* client position of canvas pixel (0,0)   */
    bool hs, vs, grid;

    /* current stroke */
    bool drawing;
    int draw_button;
    uint32_t draw_color;
    int sx, sy, lx, ly;                /* start / last point, canvas coords */
    rect_t dirty, preview;             /* canvas rects                       */
    int span_d, span_a[32], span_b[32];
    bool stamp_square;
    bool panning;
    int pan_mx, pan_my, pan_sx, pan_sy;

    /* hover feedback */
    int hover_tool, hover_size, hover_pal;
    bool in_view;
    int mx, my;                        /* last mouse position (client) */
    rect_t outline;                    /* brush outline drawn last (client) */
    int cursor;

    undo_t *undo[MAX_UNDO];
    int nundo;
    undo_t *redo[MAX_UNDO];
    int nredo;
    uint32_t next_id, saved_id;
    bool lost_change;                  /* an edit could not be recorded */
    bool menu_undo, menu_redo, shown_modified;

    char msg[160];
    uint32_t msg_until;
    int msg_timer;
    int picker_id;                     /* colour editor dialog window id */
} paint_t;

static void pt_build_menus(paint_t *p);
static void pt_do(paint_t *p, int act);
static void pt_open_picker(paint_t *p, int target, uint32_t initial);

/* ======================================================================================
 * small helpers
 * ====================================================================================== */
static int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static int zoom(paint_t *p) { return g_zooms[p->zoom_idx]; }
static int pal_top(paint_t *p) { return win_height(p->win) - STATUS_H - PALETTE_H; }
static rect_t canvas_bounds(paint_t *p) { return rect_make(0, 0, p->canvas->w, p->canvas->h); }
static const char *pt_name(paint_t *p) { return p->path[0] ? vfs_basename(p->path) : "Untitled"; }

static uint32_t top_id(paint_t *p) { return p->nundo ? p->undo[p->nundo - 1]->id : 0; }
static bool pt_modified(paint_t *p) { return p->lost_change || top_id(p) != p->saved_id; }

static bool tool_uses_size(int t) {
    return t == T_BRUSH || t == T_ERASER || t == T_LINE || t == T_RECT || t == T_ELLIPSE;
}
static int eraser_side(paint_t *p) { return MAX(4, g_sizes[p->size_idx] * 2); }

static void pt_update_title(paint_t *p) {
    char t[VFS_NAME_MAX + 32];
    bool mod = pt_modified(p);
    snprintf(t, sizeof t, "%s%s - Paint", mod ? "*" : "", pt_name(p));
    win_set_title(p->win, t);
    p->shown_modified = mod;
}

static void status_dirty(paint_t *p) {
    win_invalidate_rect(p->win, 0, win_height(p->win) - STATUS_H, win_width(p->win), STATUS_H);
}
static void toolbox_dirty(paint_t *p) { win_invalidate_rect(p->win, 0, MENU_H, TOOLBOX_W, pal_top(p) - MENU_H); }
static void palette_dirty(paint_t *p) { win_invalidate_rect(p->win, 0, pal_top(p), win_width(p->win), PALETTE_H); }

static void pt_message(paint_t *p, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void pt_message(paint_t *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->msg, sizeof p->msg, fmt, ap);
    va_end(ap);
    p->msg_until = sys_uptime_ms() + MSG_MS;
    if (!p->msg_timer) p->msg_timer = win_set_timer(p->win, 500);
    status_dirty(p);
}

static void pt_error(paint_t *p, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void pt_error(paint_t *p, const char *fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ui_message_box(p->win, "Paint", buf, MB_OK | MB_ICON_ERROR, NULL, NULL);
}

/* After anything that changes the document or the undo stacks. */
static void pt_changed(paint_t *p) {
    if (pt_modified(p) != p->shown_modified) pt_update_title(p);
    if ((p->nundo > 0) != p->menu_undo || (p->nredo > 0) != p->menu_redo) pt_build_menus(p);
    status_dirty(p);
}

/* Parse "640 x 400", "640x400", "640, 400". */
static bool parse_size(const char *s, int *w, int *h) {
    while (*s && !isdigit((unsigned char)*s)) s++;
    if (!*s) return false;
    int a = atoi(s);
    while (isdigit((unsigned char)*s)) s++;
    while (*s && !isdigit((unsigned char)*s)) s++;
    if (!*s) return false;
    int b = atoi(s);
    if (a < 1 || b < 1 || a > MAX_DIM || b > MAX_DIM) return false;
    *w = a;
    *h = b;
    return true;
}

/* ======================================================================================
 * pixel helpers on the canvas (all clip to the canvas)
 * ====================================================================================== */
static inline void cspan(surface_t *c, int x0, int x1, int y, uint32_t col) {
    if (y < 0 || y >= c->h) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= c->w) x1 = c->w - 1;
    if (x0 > x1) return;
    memset32(surface_row(c, y) + x0, col, (size_t)(x1 - x0 + 1));
}

static void cfill(surface_t *c, int x, int y, int w, int h, uint32_t col) {
    for (int j = 0; j < h; j++) cspan(c, x, x + w - 1, y + j, col);
}

/* Copy rectangle r (clipped) from src to dst; both surfaces have the same size. */
static void copy_region(surface_t *dst, const surface_t *src, rect_t r) {
    r = rect_intersect(r, rect_make(0, 0, MIN(dst->w, src->w), MIN(dst->h, src->h)));
    for (int y = r.y; y < r.y + r.h; y++)
        memcpy(surface_row(dst, y) + r.x, surface_row(src, y) + r.x, (size_t)r.w * 4);
}

static void make_disc(paint_t *p, int d) {
    if (d > 32) d = 32;
    p->span_d = d;
    double r = d / 2.0;
    for (int j = 0; j < d; j++) {
        double dy = j + 0.5 - r;
        double hw = sqrt(MAX(0.0, r * r - dy * dy));
        int a = (int)ceil(r - hw - 0.5), b = (int)floor(r + hw - 0.5);
        if (a > b) a = b = d / 2;
        p->span_a[j] = a;
        p->span_b[j] = b;
    }
}

static void dirty_add(paint_t *p, rect_t r) { p->dirty = rect_union(p->dirty, r); }

static void stamp(paint_t *p, int x, int y) {
    surface_t *c = p->canvas;
    int d = p->span_d;
    int x0 = x - d / 2, y0 = y - d / 2;
    if (d <= 1) {
        if (x >= 0 && y >= 0 && x < c->w && y < c->h) surface_row(c, y)[x] = p->draw_color;
    } else if (p->stamp_square) {
        cfill(c, x0, y0, d, d, p->draw_color);
    } else {
        for (int j = 0; j < d; j++) cspan(c, x0 + p->span_a[j], x0 + p->span_b[j], y0 + j, p->draw_color);
    }
    dirty_add(p, rect_make(x0, y0, d, d));
}

static void stamp_line(paint_t *p, int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        stamp(p, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static rect_t norm_rect(int x0, int y0, int x1, int y1) {
    return rect_make(MIN(x0, x1), MIN(y0, y1), abs(x1 - x0) + 1, abs(y1 - y0) + 1);
}

static void draw_rect_shape(surface_t *c, rect_t r, int t, bool filled, uint32_t col) {
    if (filled || r.w <= 2 * t || r.h <= 2 * t) { cfill(c, r.x, r.y, r.w, r.h, col); return; }
    cfill(c, r.x, r.y, r.w, t, col);
    cfill(c, r.x, r.y + r.h - t, r.w, t, col);
    cfill(c, r.x, r.y + t, t, r.h - 2 * t, col);
    cfill(c, r.x + r.w - t, r.y + t, t, r.h - 2 * t, col);
}

/* Ellipse inscribed in r: filled, or an outline t pixels thick, drawn as spans. */
static void draw_ellipse_shape(surface_t *c, rect_t r, int t, bool filled, uint32_t col) {
    if (r.w <= 2 && r.h <= 2) { cfill(c, r.x, r.y, r.w, r.h, col); return; }
    double rx = r.w / 2.0, ry = r.h / 2.0, cx = r.x + rx, cy = r.y + ry;
    double irx = rx - t, iry = ry - t;
    bool hollow = !filled && irx > 0.5 && iry > 0.5;
    for (int j = 0; j < r.h; j++) {
        int y = r.y + j;
        if (y < 0 || y >= c->h) continue;
        double dy = y + 0.5 - cy;
        double q = 1.0 - (dy * dy) / (ry * ry);
        if (q < 0) continue;
        double hw = rx * sqrt(q);
        int a = (int)ceil(cx - hw - 0.5), b = (int)floor(cx + hw - 0.5);
        if (a > b) { a = b = (int)floor(cx); }
        if (hollow && fabs(dy) < iry) {
            double ihw = irx * sqrt(1.0 - (dy * dy) / (iry * iry));
            int ia = (int)floor(cx - ihw - 0.5) + 1, ib = (int)ceil(cx + ihw - 0.5) - 1;
            if (ia <= ib) {
                cspan(c, a, MIN(b, ia - 1), y, col);
                cspan(c, MAX(a, ib + 1), b, y, col);
                continue;
            }
        }
        cspan(c, a, b, y, col);
    }
}

/* Iterative scanline flood fill. Returns the bounding box of the filled area. */
static rect_t flood_fill(surface_t *c, int x, int y, uint32_t col) {
    rect_t none = { 0, 0, 0, 0 };
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return none;
    uint32_t target = surface_row(c, y)[x];
    if (target == col) return none;
    int cap = 4096, n = 0;
    uint32_t *st = malloc((size_t)cap * sizeof *st);
    if (!st) return none;
    int minx = x, maxx = x, miny = y, maxy = y;
    st[n++] = ((uint32_t)y << 16) | (uint32_t)x;
    bool oom = false;
    while (n && !oom) {
        uint32_t v = st[--n];
        int px = (int)(v & 0xFFFF), py = (int)(v >> 16);
        uint32_t *row = surface_row(c, py);
        if (row[px] != target) continue;
        int l = px, r = px;
        while (l > 0 && row[l - 1] == target) l--;
        while (r < c->w - 1 && row[r + 1] == target) r++;
        memset32(row + l, col, (size_t)(r - l + 1));
        if (l < minx) minx = l;
        if (r > maxx) maxx = r;
        if (py < miny) miny = py;
        if (py > maxy) maxy = py;
        for (int k = 0; k < 2 && !oom; k++) {
            int ny = py + (k ? 1 : -1);
            if (ny < 0 || ny >= c->h) continue;
            const uint32_t *nrow = surface_row(c, ny);
            bool in = false;
            for (int i = l; i <= r; i++) {
                if (nrow[i] != target) { in = false; continue; }
                if (in) continue;
                in = true;
                if (n == cap) {
                    uint32_t *ns = realloc(st, (size_t)cap * 2 * sizeof *st);
                    if (!ns) { oom = true; break; }
                    st = ns;
                    cap *= 2;
                }
                st[n++] = ((uint32_t)ny << 16) | (uint32_t)i;
            }
        }
    }
    free(st);
    return rect_make(minx, miny, maxx - minx + 1, maxy - miny + 1);
}

/* ======================================================================================
 * undo / redo
 * ====================================================================================== */
static void undo_free(undo_t *u) {
    if (!u) return;
    surface_destroy(u->pix);
    free(u);
}

static size_t undo_size(const undo_t *u) { return (size_t)u->pix->w * (size_t)u->pix->h * 4; }

static void clear_redo(paint_t *p) {
    for (int i = 0; i < p->nredo; i++) undo_free(p->redo[i]);
    p->nredo = 0;
}

static void clear_history(paint_t *p) {
    clear_redo(p);
    for (int i = 0; i < p->nundo; i++) undo_free(p->undo[i]);
    p->nundo = 0;
}

static void push_undo(paint_t *p, undo_t *u) {
    clear_redo(p);
    size_t total = undo_size(u);
    for (int i = 0; i < p->nundo; i++) total += undo_size(p->undo[i]);
    /* drop the oldest records beyond the count / memory budget */
    while (p->nundo > 0 && (p->nundo >= MAX_UNDO || total > UNDO_BUDGET)) {
        total -= undo_size(p->undo[0]);
        undo_free(p->undo[0]);
        memmove(p->undo, p->undo + 1, (size_t)(p->nundo - 1) * sizeof p->undo[0]);
        p->nundo--;
    }
    u->id = ++p->next_id;
    p->undo[p->nundo++] = u;
}

/* The canvas changed inside r (relative to `backup`): record it and resync backup. */
static void commit_rect(paint_t *p, rect_t r) {
    r = rect_intersect(r, canvas_bounds(p));
    if (rect_empty(r)) return;
    undo_t *u = calloc(1, sizeof *u);
    surface_t *pix = u ? surface_create(r.w, r.h) : NULL;
    if (pix) {
        for (int y = 0; y < r.h; y++)
            memcpy(surface_row(pix, y), surface_row(p->backup, r.y + y) + r.x, (size_t)r.w * 4);
        u->x = r.x;
        u->y = r.y;
        u->pix = pix;
        push_undo(p, u);
    } else {
        free(u);
        p->lost_change = true;
    }
    copy_region(p->backup, p->canvas, r);
    pt_changed(p);
}

static void pt_layout(paint_t *p);

/* Replace the canvas (size may change); the old canvas becomes the undo record. */
static bool commit_full(paint_t *p, surface_t *ncanvas) {
    surface_t *nb = surface_clone(ncanvas);
    undo_t *u = calloc(1, sizeof *u);
    if (!nb || !u) {
        surface_destroy(nb);
        free(u);
        surface_destroy(ncanvas);
        pt_error(p, "Not enough memory for this operation.");
        return false;
    }
    u->full = true;
    u->pix = p->canvas;
    push_undo(p, u);
    p->canvas = ncanvas;
    surface_destroy(p->backup);
    p->backup = nb;
    pt_layout(p);
    win_invalidate(p->win);
    pt_changed(p);
    return true;
}

/* Swap a record's pixels with the canvas (used for both undo and redo). */
static bool apply_swap(paint_t *p, undo_t *u) {
    if (u->full) {
        surface_t *nb = surface_clone(u->pix);
        if (!nb) return false;
        surface_t *t = p->canvas;
        p->canvas = u->pix;
        u->pix = t;
        surface_destroy(p->backup);
        p->backup = nb;
        pt_layout(p);
        win_invalidate(p->win);
        return true;
    }
    rect_t r = rect_make(u->x, u->y, u->pix->w, u->pix->h);
    surface_t *tmp = surface_create(r.w, r.h);
    if (!tmp) return false;
    for (int y = 0; y < r.h; y++) {
        uint32_t *crow = surface_row(p->canvas, r.y + y) + r.x;
        memcpy(surface_row(tmp, y), crow, (size_t)r.w * 4);
        memcpy(crow, surface_row(u->pix, y), (size_t)r.w * 4);
    }
    surface_destroy(u->pix);
    u->pix = tmp;
    copy_region(p->backup, p->canvas, r);
    int z = zoom(p);
    win_invalidate_rect(p->win, p->org_x + r.x * z, p->org_y + r.y * z, r.w * z, r.h * z);
    return true;
}

static void pt_cancel_stroke(paint_t *p);

static void pt_undo(paint_t *p) {
    if (p->drawing) pt_cancel_stroke(p);
    if (!p->nundo) { pt_message(p, "Nothing to undo"); return; }
    undo_t *u = p->undo[p->nundo - 1];
    if (!apply_swap(p, u)) { pt_error(p, "Not enough memory to undo."); return; }
    p->nundo--;
    p->redo[p->nredo++] = u;
    pt_changed(p);
}

static void pt_redo(paint_t *p) {
    if (p->drawing) pt_cancel_stroke(p);
    if (!p->nredo) { pt_message(p, "Nothing to redo"); return; }
    undo_t *u = p->redo[p->nredo - 1];
    if (!apply_swap(p, u)) { pt_error(p, "Not enough memory to redo."); return; }
    p->nredo--;
    p->undo[p->nundo++] = u;
    pt_changed(p);
}

/* ======================================================================================
 * layout and coordinates
 * ====================================================================================== */
static void pt_layout(paint_t *p) {
    int W = win_width(p->win), H = win_height(p->win);
    rect_t area = rect_make(TOOLBOX_W, MENU_H, W - TOOLBOX_W, H - MENU_H - PALETTE_H - STATUS_H);
    int z = zoom(p);
    int cw = p->canvas->w * z, ch = p->canvas->h * z;
    bool hs = false, vs = false;
    for (int i = 0; i < 3; i++) {
        int vw = area.w - (vs ? SB_SIZE : 0), vh = area.h - (hs ? SB_SIZE : 0);
        hs = cw + 2 * PAD > vw;
        vs = ch + 2 * PAD > vh;
    }
    int vw = MAX(1, area.w - (vs ? SB_SIZE : 0)), vh = MAX(1, area.h - (hs ? SB_SIZE : 0));
    p->view = rect_make(area.x, area.y, vw, vh);
    p->hs = hs;
    p->vs = vs;
    int tw = cw + 2 * PAD, th = ch + 2 * PAD;
    if (hs) {
        p->scroll_x = CLAMP(p->scroll_x, 0, tw - vw);
        p->org_x = area.x + PAD - p->scroll_x;
    } else {
        p->scroll_x = 0;
        p->org_x = area.x + (vw - cw) / 2;
    }
    if (vs) {
        p->scroll_y = CLAMP(p->scroll_y, 0, th - vh);
        p->org_y = area.y + PAD - p->scroll_y;
    } else {
        p->scroll_y = 0;
        p->org_y = area.y + (vh - ch) / 2;
    }
    ui_set_visible(p->hsb, hs);
    ui_set_visible(p->vsb, vs);
    if (hs) {
        ui_move(p->hsb, area.x, area.y + vh, vw, SB_SIZE);
        ui_scrollbar_set(p->hsb, tw, vw, p->scroll_x);
    }
    if (vs) {
        ui_move(p->vsb, area.x + vw, area.y, SB_SIZE, vh);
        ui_scrollbar_set(p->vsb, th, vh, p->scroll_y);
    }
}

static void view_dirty(paint_t *p) {
    win_invalidate_rect(p->win, TOOLBOX_W, MENU_H, win_width(p->win) - TOOLBOX_W, pal_top(p) - MENU_H);
}

static void to_canvas(paint_t *p, int x, int y, int *cx, int *cy) {
    *cx = floordiv(x - p->org_x, zoom(p));
    *cy = floordiv(y - p->org_y, zoom(p));
}

static void canvas_dirty(paint_t *p, rect_t r) {
    if (rect_empty(r)) return;
    int z = zoom(p);
    rect_t s = rect_intersect(rect_make(p->org_x + r.x * z, p->org_y + r.y * z, r.w * z, r.h * z), p->view);
    if (!rect_empty(s)) win_invalidate_rect(p->win, s.x, s.y, s.w, s.h);
}

static void pt_set_zoom(paint_t *p, int idx, int ax, int ay) {
    idx = CLAMP(idx, 0, NZOOMS - 1);
    if (idx == p->zoom_idx) return;
    double ux = (ax - p->org_x) / (double)zoom(p), uy = (ay - p->org_y) / (double)zoom(p);
    p->zoom_idx = idx;
    int z = zoom(p);
    p->scroll_x = (int)floor(p->view.x + PAD - ax + ux * z);
    p->scroll_y = (int)floor(p->view.y + PAD - ay + uy * z);
    pt_layout(p);
    view_dirty(p);
    status_dirty(p);
}

static void pt_zoom_step(paint_t *p, int dir) {
    pt_set_zoom(p, p->zoom_idx + dir, p->view.x + p->view.w / 2, p->view.y + p->view.h / 2);
}

static void on_hscroll(widget_t *w, void *ctx) {
    paint_t *p = ctx;
    p->scroll_x = ui_get_value(w);
    pt_layout(p);
    view_dirty(p);
}

static void on_vscroll(widget_t *w, void *ctx) {
    paint_t *p = ctx;
    p->scroll_y = ui_get_value(w);
    pt_layout(p);
    view_dirty(p);
}

static void pt_scroll_by(paint_t *p, int dx, int dy) {
    p->scroll_x += dx;
    p->scroll_y += dy;
    pt_layout(p);
    view_dirty(p);
}

/* ======================================================================================
 * toolbox / palette geometry
 * ====================================================================================== */
static rect_t tool_rect(int t) { return rect_make(6 + (t % 2) * 36, MENU_H + 8 + (t / 2) * 36, 32, 32); }
static int sizes_top(void) { return MENU_H + 8 + 5 * 36 + 4; }
static rect_t size_rect(int i) { return rect_make(6, sizes_top() + 22 + i * 22, 68, 20); }

static rect_t fg_rect(paint_t *p) { return rect_make(14, pal_top(p) + 9, 28, 28); }
static rect_t bg_rect(paint_t *p) { return rect_make(32, pal_top(p) + 23, 28, 28); }
static rect_t pal_rect(paint_t *p, int i) {
    return rect_make(76 + (i % PAL_COLS) * 23, pal_top(p) + 9 + (i / PAL_COLS) * 23, 20, 20);
}
static rect_t custom_rect(paint_t *p, int i) {
    return rect_make(76 + PAL_COLS * 23 + 14 + (i % 3) * 23, pal_top(p) + 9 + (i / 3) * 23, 20, 20);
}
static rect_t edit_rect(paint_t *p) {
    rect_t c = custom_rect(p, 2);
    return rect_make(c.x + 34, pal_top(p) + 9, 124, 43);
}

static int tool_hit(int x, int y) {
    for (int t = 0; t < T_COUNT; t++) if (rect_contains(tool_rect(t), x, y)) return t;
    return -1;
}
static int size_hit(int x, int y) {
    for (int i = 0; i < NSIZES; i++) if (rect_contains(size_rect(i), x, y)) return i;
    return -1;
}
static int pal_hit(paint_t *p, int x, int y) {
    if (rect_contains(fg_rect(p), x, y)) return HIT_FG;
    if (rect_contains(bg_rect(p), x, y)) return HIT_BG;
    for (int i = 0; i < NPAL; i++) if (rect_contains(pal_rect(p, i), x, y)) return i;
    for (int i = 0; i < NCUSTOM; i++) if (rect_contains(custom_rect(p, i), x, y)) return HIT_CUSTOM + i;
    if (rect_contains(edit_rect(p), x, y)) return HIT_EDIT;
    return HIT_NONE;
}

/* ======================================================================================
 * vector icon helpers (tools and app icon); unit coordinates scaled by S
 * ====================================================================================== */
static point_t upt(int x, int y, int S, double fx, double fy) {
    point_t p = { x + (int)lround(fx * S), y + (int)lround(fy * S) };
    return p;
}

/* A rotated bar from A to B with half-width hw. */
static void ubar(surface_t *s, int x, int y, int S, double ax, double ay, double bx, double by, double hw, uint32_t c) {
    double dx = bx - ax, dy = by - ay, l = sqrt(dx * dx + dy * dy);
    if (l <= 0) return;
    double nx = -dy / l * hw, ny = dx / l * hw;
    point_t pt[4] = { upt(x, y, S, ax + nx, ay + ny), upt(x, y, S, bx + nx, by + ny),
                      upt(x, y, S, bx - nx, by - ny), upt(x, y, S, ax - nx, ay - ny) };
    gfx_fill_polygon(s, pt, 4, c);
}

static void utri(surface_t *s, int x, int y, int S, double ax, double ay, double bx, double by,
                 double cx, double cy, uint32_t c) {
    point_t a = upt(x, y, S, ax, ay), b = upt(x, y, S, bx, by), d = upt(x, y, S, cx, cy);
    gfx_fill_triangle(s, a.x, a.y, b.x, b.y, d.x, d.y, c);
}

static void ucircle(surface_t *s, int x, int y, int S, double cx, double cy, double r, uint32_t c) {
    gfx_fill_circle_aa(s, x + cx * S, y + cy * S, r * S, c);
}

/* Elliptical ring (even-odd polygon: outer contour then inner contour). */
static void uring(surface_t *s, int x, int y, int S, double cx, double cy, double rx, double ry, double t, uint32_t c) {
    enum { N = 48 };
    point_t pt[2 * (N + 1)];
    for (int i = 0; i <= N; i++) {
        double a = 2 * M_PI * i / N;
        pt[i] = upt(x, y, S, cx + rx * cos(a), cy + ry * sin(a));
        pt[2 * N + 1 - i] = upt(x, y, S, cx + (rx - t) * cos(a), cy + (ry - t) * sin(a));
    }
    gfx_fill_polygon(s, pt, 2 * (N + 1), c);
}

static void draw_tool_glyph(surface_t *s, int t, int x, int y, int S, uint32_t ink) {
    const uint32_t blue = RGB(52, 120, 230);
    switch (t) {
    case T_PENCIL:
        ubar(s, x, y, S, 0.93, 0.07, 0.85, 0.15, 0.12, RGB(236, 112, 138));     /* eraser cap */
        ubar(s, x, y, S, 0.85, 0.15, 0.79, 0.21, 0.12, RGB(170, 176, 188));     /* ferrule    */
        ubar(s, x, y, S, 0.79, 0.21, 0.34, 0.66, 0.12, RGB(250, 188, 40));      /* body       */
        ubar(s, x, y, S, 0.79, 0.21, 0.34, 0.66, 0.04, RGB(226, 150, 20));
        utri(s, x, y, S, 0.34 - 0.085, 0.66 - 0.085, 0.34 + 0.085, 0.66 + 0.085, 0.10, 0.90, RGB(240, 212, 164));
        utri(s, x, y, S, 0.18 - 0.03, 0.82 - 0.03, 0.18 + 0.03, 0.82 + 0.03, 0.10, 0.90, ink);
        break;
    case T_BRUSH:
        ubar(s, x, y, S, 0.92, 0.08, 0.55, 0.45, 0.07, RGB(176, 104, 52));
        ubar(s, x, y, S, 0.57, 0.43, 0.44, 0.56, 0.10, RGB(176, 182, 194));
        ucircle(s, x, y, S, 0.33, 0.67, 0.15, blue);
        utri(s, x, y, S, 0.22, 0.60, 0.40, 0.78, 0.08, 0.92, blue);
        ubar(s, x, y, S, 0.46, 0.54, 0.40, 0.60, 0.10, RGB(130, 136, 150));
        break;
    case T_ERASER:
        ubar(s, x, y, S, 0.80, 0.24, 0.52, 0.52, 0.20, RGB(84, 140, 232));
        ubar(s, x, y, S, 0.52, 0.52, 0.30, 0.74, 0.20, RGB(244, 140, 160));
        ubar(s, x, y, S, 0.55, 0.49, 0.49, 0.55, 0.20, color_alpha(COLOR_WHITE, 200));
        gfx_fill_rect(s, x + S * 12 / 100, y + S * 90 / 100, S * 76 / 100, MAX(1, S / 22), ink);
        break;
    case T_FILL: {
        point_t b[4] = { upt(x, y, S, 0.14, 0.46), upt(x, y, S, 0.48, 0.14), upt(x, y, S, 0.80, 0.46), upt(x, y, S, 0.46, 0.80) };
        gfx_fill_polygon(s, b, 4, ink);
        point_t i[4] = { upt(x, y, S, 0.22, 0.46), upt(x, y, S, 0.48, 0.22), upt(x, y, S, 0.72, 0.46), upt(x, y, S, 0.46, 0.72) };
        gfx_fill_polygon(s, i, 4, RGB(214, 220, 230));
        ubar(s, x, y, S, 0.18, 0.46, 0.76, 0.46, 0.05, blue);
        ucircle(s, x, y, S, 0.84, 0.76, 0.09, blue);
        utri(s, x, y, S, 0.76, 0.74, 0.92, 0.74, 0.84, 0.50, blue);
        ubar(s, x, y, S, 0.36, 0.30, 0.20, 0.12, 0.035, ink);
        break;
    }
    case T_PICKER:
        ucircle(s, x, y, S, 0.80, 0.20, 0.13, ink);
        ubar(s, x, y, S, 0.76, 0.24, 0.62, 0.38, 0.11, ink);
        ubar(s, x, y, S, 0.66, 0.34, 0.22, 0.78, 0.06, ink);
        ubar(s, x, y, S, 0.64, 0.36, 0.24, 0.76, 0.035, RGB(214, 228, 244));
        ubar(s, x, y, S, 0.40, 0.60, 0.24, 0.76, 0.035, blue);
        utri(s, x, y, S, 0.19, 0.75, 0.25, 0.81, 0.08, 0.92, ink);
        break;
    case T_LINE:
        ubar(s, x, y, S, 0.16, 0.84, 0.84, 0.16, 0.05, ink);
        ucircle(s, x, y, S, 0.16, 0.84, 0.05, ink);
        ucircle(s, x, y, S, 0.84, 0.16, 0.05, ink);
        break;
    case T_RECT:
    case T_FRECT: {
        int x0 = x + S * 14 / 100, y0 = y + S * 24 / 100, w = S * 72 / 100, h = S * 52 / 100, th = MAX(1, S * 8 / 100);
        if (t == T_FRECT) gfx_fill_rect(s, x0, y0, w, h, blue);
        gfx_fill_rect(s, x0, y0, w, th, ink);
        gfx_fill_rect(s, x0, y0 + h - th, w, th, ink);
        gfx_fill_rect(s, x0, y0, th, h, ink);
        gfx_fill_rect(s, x0 + w - th, y0, th, h, ink);
        break;
    }
    case T_ELLIPSE:
    case T_FELLIPSE:
        if (t == T_FELLIPSE) gfx_fill_ellipse(s, x + S / 2, y + S / 2, S * 36 / 100, S * 26 / 100, blue);
        uring(s, x, y, S, 0.5, 0.5, 0.38, 0.28, 0.08, ink);
        break;
    }
}

/* Tool icons are drawn at 3x and downsampled once per theme brightness. */
#define TOOL_ICON 20
static surface_t *g_tool_icons[2][T_COUNT];

static const surface_t *tool_icon(int t, bool dark) {
    surface_t **slot = &g_tool_icons[dark][t];
    if (!*slot) {
        int big = TOOL_ICON * 3;
        surface_t *b = surface_create_alpha(big, big);
        if (!b) return NULL;
        draw_tool_glyph(b, t, 0, 0, big, dark ? RGB(222, 226, 234) : RGB(52, 56, 66));
        *slot = surface_scaled(b, TOOL_ICON, TOOL_ICON);
        surface_destroy(b);
    }
    return *slot;
}

/* ======================================================================================
 * painting the window
 * ====================================================================================== */
/* Integer zoom blit of the canvas, clipped to clip. */
static void blit_zoom(surface_t *s, rect_t clip, const surface_t *src, int ox, int oy, int z) {
    rect_t r = rect_intersect(clip, rect_make(ox, oy, src->w * z, src->h * z));
    if (rect_empty(r)) return;
    for (int y = r.y; y < r.y + r.h; y++) {
        const uint32_t *srow = surface_row(src, (y - oy) / z);
        uint32_t *d = surface_row(s, y);
        if (z == 1) { memcpy(d + r.x, srow + (r.x - ox), (size_t)r.w * 4); continue; }
        int x = r.x, xe = r.x + r.w;
        int sx = (x - ox) / z;
        int run = z - (x - ox) % z;
        while (x < xe) {
            int n = MIN(run, xe - x);
            memset32(d + x, srow[sx] | 0xFF000000u, (size_t)n);
            x += n;
            sx++;
            run = z;
        }
    }
}

static void draw_swatch(surface_t *s, rect_t r, uint32_t c, uint32_t border, int radius) {
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, radius, border);
    gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, MAX(0, radius - 1), c);
}

static void draw_hue_ring(surface_t *s, int cx, int cy, int ro, int ri) {
    for (int y = -ro; y <= ro; y++)
        for (int x = -ro; x <= ro; x++) {
            int d2 = x * x + y * y;
            if (d2 > ro * ro || d2 < ri * ri) continue;
            int px = cx + x, py = cy + y;
            if (!rect_contains(s->clip, px, py)) continue;
            int h = (int)(atan2((double)y, (double)x) * 180.0 / M_PI + 360.0) % 360;
            surface_row(s, py)[px] = color_hsv(h, 220, 245);
        }
}

static void pt_paint(paint_t *p) {
    surface_t *s = win_surface(p->win);
    const theme_t *t = theme_get();
    int W = s->w, H = s->h;
    int ptop = pal_top(p);
    uint32_t line = color_mix(t->window_bg, t->window_border, 90);
    uint32_t side_bg = t->dark ? color_shade(t->window_bg, -10) : color_shade(t->window_bg, -5);
    uint32_t muted = color_mix(t->window_text, t->window_bg, 90);
    uint32_t sel_bg = color_mix(side_bg, t->accent, t->dark ? 110 : 70);
    uint32_t hov_bg = color_mix(side_bg, t->window_text, 20);

    /* ---- toolbox ---- */
    gfx_fill_rect(s, 0, MENU_H, TOOLBOX_W, ptop - MENU_H, side_bg);
    gfx_vline(s, TOOLBOX_W - 1, MENU_H, ptop - MENU_H, line);
    for (int i = 0; i < T_COUNT; i++) {
        rect_t r = tool_rect(i);
        if (i == p->tool) {
            gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, sel_bg);
            gfx_rounded_rect(s, r.x, r.y, r.w, r.h, 6, color_alpha(t->accent, 200));
        } else if (i == p->hover_tool) {
            gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, hov_bg);
        }
        const surface_t *ic = tool_icon(i, t->dark);
        if (ic) gfx_blit_alpha(s, r.x + (r.w - TOOL_ICON) / 2, r.y + (r.h - TOOL_ICON) / 2, ic, 255);
    }
    int st = sizes_top();
    gfx_hline(s, 10, st, TOOLBOX_W - 20, line);
    bool sized = tool_uses_size(p->tool);
    gfx_text(s, 10, st + 4, "Size", sized ? muted : t->disabled_text, FONT_UI_BOLD);
    for (int i = 0; i < NSIZES; i++) {
        rect_t r = size_rect(i);
        if (r.y + r.h > ptop) break;
        bool cur = i == p->size_idx;
        if (cur) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 5, sized ? sel_bg : hov_bg);
        else if (i == p->hover_size) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 5, hov_bg);
        uint32_t ink = sized ? t->window_text : t->disabled_text;
        int d = MIN(g_sizes[i], 16);
        int cx = r.x + 16, cy = r.y + r.h / 2;
        if (d <= 2) gfx_fill_rect(s, cx - d / 2, cy - d / 2, d, d, ink);
        else gfx_fill_circle_aa(s, cx + 0.5, cy + 0.5, d / 2.0, ink);
        char num[8];
        snprintf(num, sizeof num, "%d", g_sizes[i]);
        gfx_text_aligned(s, rect_make(r.x + 30, r.y, r.w - 36, r.h), num, ink, FONT_UI, ALIGN_RIGHT | ALIGN_MIDDLE);
    }

    /* ---- canvas well ---- */
    rect_t area = rect_make(TOOLBOX_W, MENU_H, W - TOOLBOX_W, ptop - MENU_H);
    gfx_fill_rect(s, area.x, area.y, area.w, area.h, t->dark ? RGB(22, 23, 27) : RGB(212, 217, 226));
    rect_t old_clip = s->clip;
    rect_t vclip = rect_intersect(old_clip, p->view);
    if (!rect_empty(vclip)) {
        s->clip = vclip;
        int z = zoom(p);
        int cw = p->canvas->w * z, ch = p->canvas->h * z;
        gfx_shadow(s, p->org_x, p->org_y + 2, cw, ch, 0, 10, t->dark ? 120 : 60);
        blit_zoom(s, vclip, p->canvas, p->org_x, p->org_y, z);
        if (p->grid && z >= 4) {
            rect_t r = rect_intersect(vclip, rect_make(p->org_x, p->org_y, cw, ch));
            uint32_t gc = RGBA(128, 128, 128, z >= 8 ? 110 : 70);
            int k0 = MAX(1, (r.x - p->org_x) / z), k1 = (r.x + r.w - p->org_x) / z;
            for (int k = k0; k <= k1 && k < p->canvas->w; k++) gfx_vline(s, p->org_x + k * z, r.y, r.h, gc);
            k0 = MAX(1, (r.y - p->org_y) / z);
            k1 = (r.y + r.h - p->org_y) / z;
            for (int k = k0; k <= k1 && k < p->canvas->h; k++) gfx_hline(s, r.x, p->org_y + k * z, r.w, gc);
        }
        /* brush outline under the pointer */
        if (!rect_empty(p->outline)) {
            rect_t o = p->outline;
            if (p->tool == T_ERASER) {
                gfx_rect(s, o.x, o.y, o.w, o.h, RGBA(0, 0, 0, 170));
                gfx_rect(s, o.x + 1, o.y + 1, o.w - 2, o.h - 2, RGBA(255, 255, 255, 200));
            } else {
                double r = o.w / 2.0 - 1;
                gfx_circle_aa(s, o.x + o.w / 2.0, o.y + o.h / 2.0, r, 1.0, RGBA(0, 0, 0, 170));
                gfx_circle_aa(s, o.x + o.w / 2.0, o.y + o.h / 2.0, r - 1, 1.0, RGBA(255, 255, 255, 190));
            }
        }
        s->clip = old_clip;
    }
    if (p->hs && p->vs)
        gfx_fill_rect(s, p->view.x + p->view.w, p->view.y + p->view.h, SB_SIZE, SB_SIZE, side_bg);

    /* ---- palette bar ---- */
    gfx_fill_rect(s, 0, ptop, W, PALETTE_H, side_bg);
    gfx_hline(s, 0, ptop, W, line);
    uint32_t sw_border = t->dark ? RGB(10, 10, 12) : RGB(120, 126, 138);
    rect_t br = bg_rect(p), fr = fg_rect(p);
    draw_swatch(s, br, p->bg, p->hover_pal == HIT_BG ? t->accent : sw_border, 5);
    gfx_fill_rounded_rect(s, fr.x - 2, fr.y - 2, fr.w + 4, fr.h + 4, 7, side_bg);
    draw_swatch(s, fr, p->fg, p->hover_pal == HIT_FG ? t->accent : sw_border, 5);
    for (int i = 0; i < NPAL; i++) {
        rect_t r = pal_rect(p, i);
        bool hov = p->hover_pal == i;
        draw_swatch(s, r, g_palette[i], hov ? t->accent : color_mix(sw_border, side_bg, 90), 4);
        if (hov) gfx_rounded_rect(s, r.x - 1, r.y - 1, r.w + 2, r.h + 2, 5, color_alpha(t->accent, 150));
    }
    rect_t c0 = custom_rect(p, 0);
    gfx_vline(s, c0.x - 8, ptop + 10, 40, line);
    for (int i = 0; i < NCUSTOM; i++) {
        rect_t r = custom_rect(p, i);
        bool hov = p->hover_pal == HIT_CUSTOM + i;
        if (p->custom[i]) {
            draw_swatch(s, r, p->custom[i], hov ? t->accent : color_mix(sw_border, side_bg, 90), 4);
        } else {
            uint32_t dc = hov ? t->accent : color_mix(muted, side_bg, 120);
            for (int k = 0; k < r.w; k += 3) {
                gfx_pixel(s, r.x + k, r.y, dc);
                gfx_pixel(s, r.x + k, r.y + r.h - 1, dc);
                gfx_pixel(s, r.x, r.y + k, dc);
                gfx_pixel(s, r.x + r.w - 1, r.y + k, dc);
            }
            gfx_fill_rect(s, r.x + 6, r.y + 9, 8, 2, dc);
            gfx_fill_rect(s, r.x + 9, r.y + 6, 2, 8, dc);
        }
    }
    rect_t er = edit_rect(p);
    ui_draw_button(s, er, NULL, p->hover_pal == HIT_EDIT ? UI_STATE_HOVER : 0);
    draw_hue_ring(s, er.x + 22, er.y + er.h / 2, 13, 7);
    gfx_fill_circle_aa(s, er.x + 22.5, er.y + er.h / 2 + 0.5, 5, p->fg);
    gfx_text(s, er.x + 42, er.y + 5, "Edit", t->button_text, FONT_UI);
    gfx_text(s, er.x + 42, er.y + 21, "colours...", t->button_text, FONT_UI);

    /* ---- status bar ---- */
    int sy = H - STATUS_H;
    gfx_fill_rect(s, 0, sy, W, STATUS_H, t->dark ? color_shade(t->window_bg, -10) : color_shade(t->window_bg, -6));
    gfx_hline(s, 0, sy, W, line);
    int ty = sy + 1 + (STATUS_H - 1 - font_height(FONT_UI)) / 2;
    char buf[160];
    if (tool_uses_size(p->tool)) {
        int sz = p->tool == T_ERASER ? eraser_side(p) : g_sizes[p->size_idx];
        snprintf(buf, sizeof buf, "%s, %d px", g_tool_names[p->tool], sz);
    } else {
        snprintf(buf, sizeof buf, "%s", g_tool_names[p->tool]);
    }
    gfx_text(s, 12, ty, buf, t->window_text, FONT_UI);
    int mx = 190;
    if (p->msg[0]) {
        gfx_text_ellipsis(s, mx, ty, W - mx - 190, p->msg, t->window_text, FONT_UI_BOLD);
    } else if (p->hover_tool >= 0) {
        snprintf(buf, sizeof buf, "%s  (key %s)", g_tool_names[p->hover_tool], g_tool_keys[p->hover_tool]);
        gfx_text(s, mx, ty, buf, muted, FONT_UI);
    } else if (p->drawing && p->tool >= T_LINE) {
        int cx, cy;
        to_canvas(p, p->mx, p->my, &cx, &cy);
        snprintf(buf, sizeof buf, "%d, %d  \x93  %d x %d", p->sx, p->sy, abs(cx - p->sx) + 1, abs(cy - p->sy) + 1);
        gfx_text(s, mx, ty, buf, t->window_text, FONT_UI);
    } else if (p->in_view) {
        int cx, cy;
        to_canvas(p, p->mx, p->my, &cx, &cy);
        if (cx >= 0 && cy >= 0 && cx < p->canvas->w && cy < p->canvas->h) {
            uint32_t c = surface_row(p->canvas, cy)[cx];
            int w = gfx_text(s, mx, ty, "", t->window_text, FONT_UI);
            snprintf(buf, sizeof buf, "%d, %d px", cx, cy);
            w = gfx_text(s, mx, ty, buf, t->window_text, FONT_UI);
            int qx = mx + MAX(w + 16, 90);
            draw_swatch(s, rect_make(qx, sy + 6, 12, 12), c, sw_border, 3);
            snprintf(buf, sizeof buf, "#%02X%02X%02X", COLOR_R(c), COLOR_G(c), COLOR_B(c));
            gfx_text(s, qx + 18, ty, buf, muted, FONT_UI);
        }
    }
    snprintf(buf, sizeof buf, "%d x %d px      %d%%", p->canvas->w, p->canvas->h, zoom(p) * 100);
    int rw = font_text_width(FONT_UI, buf);
    gfx_text(s, W - 12 - rw, ty, buf, muted, FONT_UI);
}

/* ======================================================================================
 * strokes
 * ====================================================================================== */
static rect_t outline_rect(paint_t *p, int cx, int cy) {
    int z = zoom(p);
    int d = p->tool == T_ERASER ? eraser_side(p) : g_sizes[p->size_idx];
    int x0 = cx - d / 2, y0 = cy - d / 2;
    return rect_make(p->org_x + x0 * z, p->org_y + y0 * z, d * z, d * z);
}

static void update_outline(paint_t *p) {
    rect_t old = p->outline;
    rect_t nr = { 0, 0, 0, 0 };
    if (p->in_view && !p->panning && (p->tool == T_BRUSH || p->tool == T_ERASER)) {
        int cx, cy;
        to_canvas(p, p->mx, p->my, &cx, &cy);
        nr = outline_rect(p, cx, cy);
        if (nr.w < 5) nr = rect_make(0, 0, 0, 0);   /* too small to be useful */
    }
    if (old.x == nr.x && old.y == nr.y && old.w == nr.w && old.h == nr.h) return;
    p->outline = nr;
    if (!rect_empty(old)) win_invalidate_rect(p->win, old.x - 1, old.y - 1, old.w + 2, old.h + 2);
    if (!rect_empty(nr)) win_invalidate_rect(p->win, nr.x - 1, nr.y - 1, nr.w + 2, nr.h + 2);
}

static void constrain(paint_t *p, int *x, int *y) {
    int dx = *x - p->sx, dy = *y - p->sy, adx = abs(dx), ady = abs(dy);
    if (p->tool == T_LINE) {
        if (adx > 2 * ady) { *y = p->sy; return; }
        if (ady > 2 * adx) { *x = p->sx; return; }
    }
    int m = MAX(adx, ady);
    *x = p->sx + (dx < 0 ? -m : m);
    *y = p->sy + (dy < 0 ? -m : m);
}

/* Draw the shape from the start point to (x,y); returns its bounding box. */
static rect_t draw_shape(paint_t *p, int x, int y) {
    int th = g_sizes[p->size_idx];
    rect_t r = norm_rect(p->sx, p->sy, x, y);
    switch (p->tool) {
    case T_LINE:
        stamp_line(p, p->sx, p->sy, x, y);
        return rect_make(r.x - th, r.y - th, r.w + 2 * th, r.h + 2 * th);
    case T_RECT:     draw_rect_shape(p->canvas, r, th, false, p->draw_color); break;
    case T_FRECT:    draw_rect_shape(p->canvas, r, th, true, p->draw_color); break;
    case T_ELLIPSE:  draw_ellipse_shape(p->canvas, r, th, false, p->draw_color); break;
    case T_FELLIPSE: draw_ellipse_shape(p->canvas, r, th, true, p->draw_color); break;
    }
    return r;
}

static void shape_update(paint_t *p, int x, int y, int mods) {
    rect_t old = p->preview;
    if (!rect_empty(old)) copy_region(p->canvas, p->backup, old);
    if (mods & MOD_SHIFT) constrain(p, &x, &y);
    rect_t nr = draw_shape(p, x, y);
    p->preview = nr;
    dirty_add(p, nr);
    canvas_dirty(p, rect_union(old, nr));
    status_dirty(p);
}

static void pt_begin_stroke(paint_t *p, int cx, int cy, int button, int mods) {
    p->draw_button = button;
    p->draw_color = p->tool == T_ERASER ? p->bg : (button == MOUSE_RIGHT ? p->bg : p->fg);
    p->dirty = rect_make(0, 0, 0, 0);
    p->preview = rect_make(0, 0, 0, 0);
    p->sx = p->lx = cx;
    p->sy = p->ly = cy;
    p->stamp_square = false;
    switch (p->tool) {
    case T_FILL: {
        rect_t r = flood_fill(p->canvas, cx, cy, p->draw_color);
        if (!rect_empty(r)) { canvas_dirty(p, r); commit_rect(p, r); }
        return;
    }
    case T_PICKER:
        if (cx >= 0 && cy >= 0 && cx < p->canvas->w && cy < p->canvas->h) {
            uint32_t c = surface_row(p->canvas, cy)[cx] | 0xFF000000u;
            if (button == MOUSE_RIGHT) p->bg = c; else p->fg = c;
            p->tool = p->prev_tool == T_PICKER ? T_PENCIL : p->prev_tool;
            palette_dirty(p);
            toolbox_dirty(p);
            status_dirty(p);
        }
        return;
    case T_PENCIL:
        make_disc(p, 1);
        break;
    case T_BRUSH:
        make_disc(p, g_sizes[p->size_idx]);
        break;
    case T_ERASER:
        p->span_d = eraser_side(p);
        p->stamp_square = true;
        break;
    default:   /* shapes: line uses round stamps of the stroke width */
        make_disc(p, g_sizes[p->size_idx]);
        break;
    }
    p->drawing = true;
    if (p->tool <= T_ERASER) {
        stamp(p, cx, cy);
        canvas_dirty(p, p->dirty);
    } else {
        shape_update(p, cx, cy, mods);
    }
}

static void pt_continue_stroke(paint_t *p, int cx, int cy, int mods) {
    if (p->tool <= T_ERASER) {
        if (cx == p->lx && cy == p->ly) return;
        rect_t before = p->dirty;
        p->dirty = rect_make(0, 0, 0, 0);
        stamp_line(p, p->lx, p->ly, cx, cy);
        canvas_dirty(p, p->dirty);
        p->dirty = rect_union(before, p->dirty);
        p->lx = cx;
        p->ly = cy;
    } else {
        shape_update(p, cx, cy, mods);
    }
}

static void pt_end_stroke(paint_t *p) {
    if (!p->drawing) return;
    p->drawing = false;
    commit_rect(p, p->dirty);
    p->dirty = p->preview = rect_make(0, 0, 0, 0);
    status_dirty(p);
}

static void pt_cancel_stroke(paint_t *p) {
    if (!p->drawing) return;
    p->drawing = false;
    copy_region(p->canvas, p->backup, p->dirty);
    canvas_dirty(p, p->dirty);
    p->dirty = p->preview = rect_make(0, 0, 0, 0);
    status_dirty(p);
}

/* ======================================================================================
 * document: new / open / save
 * ====================================================================================== */
static void pt_set_canvas(paint_t *p, surface_t *c, const char *path) {
    surface_t *b = surface_clone(c);
    if (!b) { surface_destroy(c); pt_error(p, "Not enough memory for an image of this size."); return; }
    if (p->drawing) p->drawing = false;
    clear_history(p);
    surface_destroy(p->canvas);
    surface_destroy(p->backup);
    p->canvas = c;
    p->backup = b;
    p->saved_id = top_id(p);
    p->lost_change = false;
    strlcpy(p->path, path ? path : "", sizeof p->path);
    p->scroll_x = p->scroll_y = 0;
    p->zoom_idx = 0;
    pt_layout(p);
    win_invalidate(p->win);
    pt_update_title(p);
    pt_changed(p);
}

static surface_t *new_blank(int w, int h, uint32_t c) {
    surface_t *s = surface_create(w, h);
    if (s) gfx_clear(s, c);
    return s;
}

static bool pt_load(paint_t *p, const char *path) {
    gbmp_info_t info;
    const char *err = NULL;
    uint32_t fsize = 0;
    surface_t *img = gbmp_load(path, &info, &fsize, &err);
    if (!img) { pt_error(p, "Cannot open \"%s\":\n%s.", vfs_basename(path), err ? err : "unknown error"); return false; }
    if (img->w > MAX_DIM || img->h > MAX_DIM) {
        pt_error(p, "\"%s\" is %d x %d pixels; Paint edits images up to %d x %d.",
                 vfs_basename(path), img->w, img->h, MAX_DIM, MAX_DIM);
        surface_destroy(img);
        return false;
    }
    if (img->flags & SURF_ALPHA) {   /* flatten transparency onto white */
        surface_t *o = new_blank(img->w, img->h, COLOR_WHITE);
        if (!o) { surface_destroy(img); pt_error(p, "Not enough memory to open this image."); return false; }
        gfx_blit_alpha(o, 0, 0, img, 255);
        surface_destroy(img);
        img = o;
    }
    pt_set_canvas(p, img, path);
    if (strcasecmp(vfs_extension(path), "bmp") && strcasecmp(vfs_extension(path), "dib")) {
        p->path[0] = 0;     /* not a BMP: Save will ask for a new name */
        pt_update_title(p);
    }
    pt_message(p, "Opened %s (%d x %d, %s)", vfs_basename(path), info.width, info.height, info.format);
    klog("paint: opened %s (%dx%d)", path, info.width, info.height);
    return true;
}

static bool pt_write(paint_t *p, const char *path) {
    int err = gbmp_save(path, p->canvas, false);
    if (err < 0) {
        pt_error(p, "Could not save \"%s\":\n%s.", path, vfs_strerror(err));
        return false;
    }
    if (path != p->path) strlcpy(p->path, path, sizeof p->path);
    p->saved_id = top_id(p);
    p->lost_change = false;
    pt_update_title(p);
    pt_message(p, "Saved %s", p->path);
    klog("paint: saved %s (%dx%d)", p->path, p->canvas->w, p->canvas->h);
    return true;
}

static void pt_overwrite_cb(int r, void *ctx) {
    paint_t *p = ctx;
    int act = p->pending;
    p->pending = ACT_NONE;
    if (r == MB_RESULT_YES && pt_write(p, p->pending_path)) pt_do(p, act);
}

static void pt_saveas_cb(const char *path, void *ctx) {
    paint_t *p = ctx;
    if (!path) { p->pending = ACT_NONE; return; }
    char full[VFS_PATH_MAX];
    strlcpy(full, path, sizeof full);
    const char *ext = vfs_extension(full);
    if (strcasecmp(ext, "bmp") && strcasecmp(ext, "dib") && strlen(full) + 4 < sizeof full) strlcat(full, ".bmp", sizeof full);
    if (vfs_is_dir(full)) { pt_error(p, "\"%s\" is a folder.", full); p->pending = ACT_NONE; return; }
    if (vfs_exists(full) && strcmp(full, p->path) != 0) {
        strlcpy(p->pending_path, full, sizeof p->pending_path);
        char q[VFS_PATH_MAX + 64];
        snprintf(q, sizeof q, "\"%s\" already exists.\nDo you want to replace it?", vfs_basename(full));
        ui_message_box(p->win, "Save as", q, MB_YESNO | MB_ICON_WARNING, pt_overwrite_cb, p);
        return;
    }
    int act = p->pending;
    p->pending = ACT_NONE;
    if (pt_write(p, full)) pt_do(p, act);
}

static void pt_save_as(paint_t *p, int then) {
    p->pending = then;
    char start[VFS_PATH_MAX];
    if (p->path[0]) strlcpy(start, p->path, sizeof start);
    else snprintf(start, sizeof start, "/home/user/Pictures/untitled.bmp");
    ui_file_dialog(p->win, UI_FILE_SAVE, "Save image as", start, "bmp", pt_saveas_cb, p);
}

static void pt_save(paint_t *p, int then) {
    if (!p->path[0]) { pt_save_as(p, then); return; }
    if (pt_write(p, p->path)) pt_do(p, then);
}

static void pt_prompt_cb(int r, void *ctx) {
    paint_t *p = ctx;
    int act = p->pending;
    p->pending = ACT_NONE;
    if (r == MB_RESULT_YES) pt_save(p, act);
    else if (r == MB_RESULT_NO) pt_do(p, act);
}

/* Run `act`, first offering to save unsaved changes. */
static void pt_request(paint_t *p, int act) {
    if (p->drawing) pt_end_stroke(p);
    if (!pt_modified(p)) { pt_do(p, act); return; }
    p->pending = act;
    char q[VFS_NAME_MAX + 64];
    snprintf(q, sizeof q, "Do you want to save changes to \"%s\"?", pt_name(p));
    ui_message_box(p->win, "Paint", q, MB_YESNOCANCEL | MB_ICON_QUESTION, pt_prompt_cb, p);
}

static void pt_new_cb(const char *text, void *ctx) {
    paint_t *p = ctx;
    if (!text) return;
    int w, h;
    if (!parse_size(text, &w, &h)) { pt_error(p, "Please enter a size like \"640 x 400\" (1 to %d pixels).", MAX_DIM); return; }
    surface_t *c = new_blank(w, h, COLOR_WHITE);
    if (!c) { pt_error(p, "Not enough memory for a %d x %d image.", w, h); return; }
    pt_set_canvas(p, c, NULL);
    pt_message(p, "New %d x %d image", w, h);
}

static void pt_open_cb(const char *path, void *ctx) {
    paint_t *p = ctx;
    if (path) pt_load(p, path);
}

static void pt_do(paint_t *p, int act) {
    switch (act) {
    case ACT_NEW: {
        char init[32];
        snprintf(init, sizeof init, "%d x %d", p->canvas->w, p->canvas->h);
        ui_input_box(p->win, "New image", "Image size in pixels (width x height):", init, pt_new_cb, p);
        break;
    }
    case ACT_OPEN: {
        char dir[VFS_PATH_MAX];
        if (p->path[0]) vfs_dirname(p->path, dir);
        else strlcpy(dir, "/home/user/Pictures", sizeof dir);
        ui_file_dialog(p->win, UI_FILE_OPEN, "Open image", dir, "bmp,dib,ppm", pt_open_cb, p);
        break;
    }
    case ACT_CLOSE:
        win_close(p->win);   /* p is freed: do not touch it afterwards */
        break;
    default:
        break;
    }
}

/* ======================================================================================
 * image operations
 * ====================================================================================== */
static void pt_whole(paint_t *p) {
    commit_rect(p, canvas_bounds(p));
    view_dirty(p);
}

static void op_flip(paint_t *p, bool horizontal) {
    surface_t *c = p->canvas;
    if (horizontal) {
        for (int y = 0; y < c->h; y++) {
            uint32_t *row = surface_row(c, y);
            for (int a = 0, b = c->w - 1; a < b; a++, b--) { uint32_t t = row[a]; row[a] = row[b]; row[b] = t; }
        }
    } else {
        for (int a = 0, b = c->h - 1; a < b; a++, b--) {
            uint32_t *ra = surface_row(c, a), *rb = surface_row(c, b);
            for (int x = 0; x < c->w; x++) { uint32_t t = ra[x]; ra[x] = rb[x]; rb[x] = t; }
        }
    }
    pt_whole(p);
}

static void op_colors(paint_t *p, bool invert) {
    surface_t *c = p->canvas;
    for (int y = 0; y < c->h; y++) {
        uint32_t *row = surface_row(c, y);
        for (int x = 0; x < c->w; x++) {
            uint32_t v = row[x];
            if (invert) row[x] = (v ^ 0x00FFFFFFu) | 0xFF000000u;
            else {
                int l = (COLOR_R(v) * 77 + COLOR_G(v) * 150 + COLOR_B(v) * 29) >> 8;
                row[x] = RGB(l, l, l);
            }
        }
    }
    pt_whole(p);
}

static void op_rotate(paint_t *p, bool right) {
    surface_t *c = p->canvas;
    surface_t *n = surface_create(c->h, c->w);
    if (!n) { pt_error(p, "Not enough memory to rotate the image."); return; }
    for (int y = 0; y < c->h; y++) {
        const uint32_t *row = surface_row(c, y);
        for (int x = 0; x < c->w; x++) {
            int nx = right ? c->h - 1 - y : y, ny = right ? x : c->w - 1 - x;
            surface_row(n, ny)[nx] = row[x];
        }
    }
    commit_full(p, n);
}

static void op_clear(paint_t *p) {
    gfx_clear(p->canvas, p->bg);
    pt_whole(p);
    pt_message(p, "Image cleared to the secondary colour");
}

static void pt_resize_cb(const char *text, void *ctx) {
    paint_t *p = ctx;
    if (!text) return;
    int w, h;
    if (!parse_size(text, &w, &h)) { pt_error(p, "Please enter a size like \"800 x 600\" (1 to %d pixels).", MAX_DIM); return; }
    surface_t *n = new_blank(w, h, p->bg);
    if (!n) { pt_error(p, "Not enough memory for a %d x %d image.", w, h); return; }
    gfx_blit(n, 0, 0, p->canvas);
    if (commit_full(p, n)) pt_message(p, "Canvas resized to %d x %d", w, h);
}

static void pt_scale_cb(const char *text, void *ctx) {
    paint_t *p = ctx;
    if (!text) return;
    int w, h;
    const char *pc = strchr(text, '%');
    if (pc) {
        int pct = atoi(text);
        w = p->canvas->w * pct / 100;
        h = p->canvas->h * pct / 100;
        if (pct <= 0 || w < 1 || h < 1 || w > MAX_DIM || h > MAX_DIM) { pt_error(p, "That scale gives an image outside 1..%d pixels.", MAX_DIM); return; }
    } else if (!parse_size(text, &w, &h)) {
        pt_error(p, "Please enter a size like \"320 x 200\" or a percentage like \"50%%\".");
        return;
    }
    surface_t *n = surface_scaled(p->canvas, w, h);
    if (!n) { pt_error(p, "Not enough memory to scale the image."); return; }
    if (commit_full(p, n)) pt_message(p, "Image scaled to %d x %d", w, h);
}

/* ======================================================================================
 * menus
 * ====================================================================================== */
static void pt_file_menu(int item, void *ctx) {
    paint_t *p = ctx;
    switch (item) {
    case 0: pt_request(p, ACT_NEW); break;
    case 1: pt_request(p, ACT_OPEN); break;
    case 2: pt_save(p, ACT_NONE); break;
    case 3: pt_save_as(p, ACT_NONE); break;
    case 5: pt_request(p, ACT_CLOSE); break;
    }
}

static void pt_swap_colors(paint_t *p) {
    uint32_t t = p->fg;
    p->fg = p->bg;
    p->bg = t;
    palette_dirty(p);
}

static void pt_edit_menu(int item, void *ctx) {
    paint_t *p = ctx;
    switch (item) {
    case 0: pt_undo(p); break;
    case 1: pt_redo(p); break;
    case 3: op_clear(p); break;
    case 5: pt_swap_colors(p); break;
    case 6: pt_open_picker(p, 0, p->fg); break;
    }
}

static void pt_image_menu(int item, void *ctx) {
    paint_t *p = ctx;
    char init[32];
    switch (item) {
    case 0: op_flip(p, true); break;
    case 1: op_flip(p, false); break;
    case 2: op_rotate(p, true); break;
    case 3: op_rotate(p, false); break;
    case 5: op_colors(p, true); break;
    case 6: op_colors(p, false); break;
    case 8:
        snprintf(init, sizeof init, "%d x %d", p->canvas->w, p->canvas->h);
        ui_input_box(p->win, "Resize canvas", "New canvas size (the image stays at the top-left):", init, pt_resize_cb, p);
        break;
    case 9:
        snprintf(init, sizeof init, "%d x %d", p->canvas->w, p->canvas->h);
        ui_input_box(p->win, "Scale image", "New image size, or a percentage (e.g. 50%):", init, pt_scale_cb, p);
        break;
    }
}

static void pt_view_menu(int item, void *ctx) {
    paint_t *p = ctx;
    switch (item) {
    case 0: pt_zoom_step(p, 1); break;
    case 1: pt_zoom_step(p, -1); break;
    case 2: pt_set_zoom(p, 0, p->view.x + p->view.w / 2, p->view.y + p->view.h / 2); break;
    case 4:
        p->grid = !p->grid;
        pt_build_menus(p);
        view_dirty(p);
        if (p->grid && zoom(p) < 4) pt_message(p, "The pixel grid shows at 400%% zoom and above");
        break;
    }
}

static void pt_help_menu(int item, void *ctx) {
    paint_t *p = ctx;
    if (item == 0) {
        ui_message_box(p->win, "Paint shortcuts",
                       "Tools: P pencil, B brush, E eraser, F fill, I colour picker,\n"
                       "L line, R rectangle (Shift+R filled), O ellipse (Shift+O filled).\n\n"
                       "Left button = primary colour, right button = secondary colour.\n"
                       "Shift while drawing: straight lines, squares and circles.\n"
                       "[ and ] change the size, X swaps the colours, Esc cancels a shape.\n\n"
                       "Ctrl+Z undo, Ctrl+Y redo, Ctrl+N/O/S new/open/save.\n"
                       "Ctrl+wheel or Ctrl++ / Ctrl+- zoom, middle button drags the view.",
                       MB_OK | MB_ICON_INFO, NULL, NULL);
    } else {
        ui_message_box(p->win, "About Paint",
                       "Paint for NovaOS\n\nDraw with the mouse, then save your picture as a 24-bit BMP file. "
                       "Open the Pictures folder in Image Viewer to see your work.",
                       MB_OK | MB_ICON_INFO, NULL, NULL);
    }
}

static void pt_build_menus(paint_t *p) {
    if (p->menubar) ui_destroy(p->menubar);
    widget_t *mb = p->menubar = ui_menubar(p->win);
    if (!mb) return;
    p->menu_undo = p->nundo > 0;
    p->menu_redo = p->nredo > 0;
    ui_menubar_add(mb, "File", "New...\tCtrl+N|Open...\tCtrl+O|Save\tCtrl+S|Save as...\tCtrl+Shift+S|-|Close\tCtrl+W",
                   pt_file_menu, p);
    char edit[200];
    snprintf(edit, sizeof edit, "%sUndo\tCtrl+Z|%sRedo\tCtrl+Y|-|Clear image\tDel|-|Swap colours\tX|Edit colours...",
             p->menu_undo ? "" : "!", p->menu_redo ? "" : "!");
    ui_menubar_add(mb, "Edit", edit, pt_edit_menu, p);
    ui_menubar_add(mb, "Image", "Flip horizontal|Flip vertical|Rotate right\tCtrl+R|Rotate left|-|"
                   "Invert colours\tCtrl+I|Grayscale|-|Resize canvas...|Scale image...", pt_image_menu, p);
    char view[160];
    snprintf(view, sizeof view, "Zoom in\tCtrl++|Zoom out\tCtrl+-|Actual size\tCtrl+0|-|Pixel grid\t%s",
             p->grid ? "\x96" : "G");
    ui_menubar_add(mb, "View", view, pt_view_menu, p);
    ui_menubar_add(mb, "Help", "Keyboard shortcuts|About Paint", pt_help_menu, p);
}

/* ======================================================================================
 * colour editor dialog (HSV square + hue bar + hex field)
 * ====================================================================================== */
#define PK_W   404
#define PK_H   282
#define SV_X   16
#define SV_Y   16
#define SV_S   200
#define HUE_X  228
#define HUE_W  22
#define PK_COL 266

typedef struct picker {
    int owner_id;
    int target;             /* 0 = primary, 1 = secondary, 2 + n = custom slot n */
    int h, s, v;
    uint32_t rgb, orig;
    int drag;               /* 0 none, 1 SV square, 2 hue bar */
    widget_t *hex;
    bool setting;
} picker_t;

static void rgb_to_hsv(uint32_t c, int *h, int *s, int *v) {
    int r = COLOR_R(c), g = COLOR_G(c), b = COLOR_B(c);
    int mx = MAX(r, MAX(g, b)), mn = MIN(r, MIN(g, b)), d = mx - mn;
    *v = mx;
    *s = mx ? d * 255 / mx : 0;
    if (!d) return;            /* grey: keep the current hue */
    int hh;
    if (mx == r) hh = 60 * (g - b) / d;
    else if (mx == g) hh = 120 + 60 * (b - r) / d;
    else hh = 240 + 60 * (r - g) / d;
    if (hh < 0) hh += 360;
    *h = hh % 360;
}

static void pk_sync_hex(picker_t *k) {
    char buf[16];
    snprintf(buf, sizeof buf, "#%02X%02X%02X", COLOR_R(k->rgb), COLOR_G(k->rgb), COLOR_B(k->rgb));
    k->setting = true;
    ui_set_text(k->hex, buf);
    k->setting = false;
}

static bool parse_hex(const char *s, uint32_t *out) {
    while (*s == ' ' || *s == '#') s++;
    uint32_t v = 0;
    int n = 0;
    for (; *s && n < 7; s++) {
        if (*s == ' ') continue;
        int d = isdigit((unsigned char)*s) ? *s - '0' : (isxdigit((unsigned char)*s) ? tolower((unsigned char)*s) - 'a' + 10 : -1);
        if (d < 0) return false;
        v = v * 16 + (uint32_t)d;
        n++;
    }
    if (n != 6) return false;
    *out = 0xFF000000u | v;
    return true;
}

static void pk_paint(window_t *win, picker_t *k) {
    surface_t *s = win_surface(win);
    const theme_t *t = theme_get();
    rect_t clip = s->clip;
    uint32_t border = color_mix(t->window_bg, t->window_border, 160);
    /* saturation/value square */
    uint32_t hc = color_hsv(k->h, 255, 255);
    int hr = COLOR_R(hc), hg = COLOR_G(hc), hb = COLOR_B(hc);
    rect_t sq = rect_intersect(clip, rect_make(SV_X, SV_Y, SV_S, SV_S));
    for (int y = sq.y; y < sq.y + sq.h; y++) {
        int v = 255 - (y - SV_Y) * 255 / (SV_S - 1);
        uint32_t *row = surface_row(s, y);
        for (int x = sq.x; x < sq.x + sq.w; x++) {
            int sat = (x - SV_X) * 255 / (SV_S - 1);
            int r = v * (65025 - sat * (255 - hr)) / 65025;
            int g = v * (65025 - sat * (255 - hg)) / 65025;
            int b = v * (65025 - sat * (255 - hb)) / 65025;
            row[x] = RGB(r, g, b);
        }
    }
    gfx_rect(s, SV_X - 1, SV_Y - 1, SV_S + 2, SV_S + 2, border);
    /* hue bar */
    rect_t hb_r = rect_intersect(clip, rect_make(HUE_X, SV_Y, HUE_W, SV_S));
    for (int y = hb_r.y; y < hb_r.y + hb_r.h; y++)
        gfx_hline(s, hb_r.x, y, hb_r.w, color_hsv((y - SV_Y) * 359 / (SV_S - 1), 255, 255));
    gfx_rect(s, HUE_X - 1, SV_Y - 1, HUE_W + 2, SV_S + 2, border);
    /* markers */
    int mx = SV_X + k->s * (SV_S - 1) / 255, my = SV_Y + (255 - k->v) * (SV_S - 1) / 255;
    surface_t sub = surface_sub(s, rect_make(SV_X - 1, SV_Y - 1, SV_S + 2, SV_S + 2));
    sub.clip = rect_intersect(sub.clip, rect_make(clip.x - (SV_X - 1), clip.y - (SV_Y - 1), clip.w, clip.h));
    gfx_circle_aa(&sub, mx - SV_X + 1.5, my - SV_Y + 1.5, 6, 2, COLOR_WHITE);
    gfx_circle_aa(&sub, mx - SV_X + 1.5, my - SV_Y + 1.5, 8, 1, RGBA(0, 0, 0, 160));
    int hy = SV_Y + k->h * (SV_S - 1) / 359;
    gfx_fill_triangle(s, HUE_X - 7, hy - 5, HUE_X - 7, hy + 5, HUE_X - 2, hy, t->window_text);
    gfx_fill_triangle(s, HUE_X + HUE_W + 6, hy - 5, HUE_X + HUE_W + 6, hy + 5, HUE_X + HUE_W + 1, hy, t->window_text);
    /* preview swatches */
    int cw = PK_W - 16 - PK_COL;
    gfx_text(s, PK_COL, SV_Y - 2, "New", color_mix(t->window_text, t->window_bg, 90), FONT_UI);
    gfx_fill_rounded_rect(s, PK_COL, SV_Y + 16, cw, 30, 5, border);
    gfx_fill_rounded_rect(s, PK_COL + 1, SV_Y + 17, cw - 2, 28, 4, k->rgb);
    gfx_text(s, PK_COL, SV_Y + 52, "Current", color_mix(t->window_text, t->window_bg, 90), FONT_UI);
    gfx_fill_rounded_rect(s, PK_COL, SV_Y + 70, cw, 30, 5, border);
    gfx_fill_rounded_rect(s, PK_COL + 1, SV_Y + 71, cw - 2, 28, 4, k->orig);
    char buf[48];
    snprintf(buf, sizeof buf, "R %d  G %d  B %d", COLOR_R(k->rgb), COLOR_G(k->rgb), COLOR_B(k->rgb));
    gfx_text(s, PK_COL, SV_Y + 110, buf, t->window_text, FONT_UI);
    snprintf(buf, sizeof buf, "H %d\xB0  S %d%%  V %d%%", k->h, k->s * 100 / 255, k->v * 100 / 255);
    gfx_text(s, PK_COL, SV_Y + 128, buf, t->window_text, FONT_UI);
}

static void pk_set_from_hsv(window_t *win, picker_t *k) {
    k->rgb = color_hsv(k->h, k->s, k->v);
    pk_sync_hex(k);
    win_invalidate(win);
}

static void pk_mouse(window_t *win, picker_t *k, int x, int y) {
    if (k->drag == 1) {
        k->s = CLAMP((x - SV_X) * 255 / (SV_S - 1), 0, 255);
        k->v = CLAMP(255 - (y - SV_Y) * 255 / (SV_S - 1), 0, 255);
    } else if (k->drag == 2) {
        k->h = CLAMP((y - SV_Y) * 359 / (SV_S - 1), 0, 359);
    } else {
        return;
    }
    pk_set_from_hsv(win, k);
}

static void pk_apply(window_t *win, picker_t *k) {
    window_t *ow = win_find_by_id(k->owner_id);
    paint_t *p = ow ? win_get_userdata(ow) : NULL;
    if (p) {
        uint32_t c = k->rgb | 0xFF000000u;
        if (k->target == 1) p->bg = c;
        else {
            p->fg = c;
            int slot = k->target >= 2 ? k->target - 2 : -1;
            if (slot < 0) {
                /* remember it in a custom slot (unless it is already there) */
                for (int i = 0; i < NCUSTOM; i++) if (p->custom[i] == c) slot = -2;
                if (slot == -1) {
                    for (int i = 0; i < NCUSTOM && slot < 0; i++) if (!p->custom[i]) slot = i;
                    if (slot < 0) { slot = p->next_custom; p->next_custom = (p->next_custom + 1) % NCUSTOM; }
                }
            }
            if (slot >= 0) p->custom[slot] = c;
        }
        palette_dirty(p);
        status_dirty(p);
    }
    win_close(win);
}

static void pk_ok(widget_t *w, void *ctx) { pk_apply(ui_window(w), ctx); }
static void pk_cancel(widget_t *w, void *ctx) { (void)ctx; win_close(ui_window(w)); }

static void pk_hex_changed(widget_t *w, void *ctx) {
    picker_t *k = ctx;
    if (k->setting) return;
    uint32_t c;
    if (!parse_hex(ui_get_text(w), &c)) return;
    k->rgb = c;
    rgb_to_hsv(c, &k->h, &k->s, &k->v);
    win_invalidate(ui_window(w));
}

static void pk_hex_submit(widget_t *w, void *ctx) {
    picker_t *k = ctx;
    uint32_t c;
    if (parse_hex(ui_get_text(w), &c)) { k->rgb = c; pk_apply(ui_window(w), k); }
    else sys_beep(220, 60);
}

static void pk_event(window_t *win, const event_t *ev) {
    picker_t *k = win_get_userdata(win);
    if (!k) return;
    switch (ev->type) {
    case EV_PAINT: pk_paint(win, k); break;
    case EV_MOUSE_DOWN:
        if (ev->button != MOUSE_LEFT) break;
        if (rect_contains(rect_make(SV_X - 4, SV_Y - 4, SV_S + 8, SV_S + 8), ev->x, ev->y)) k->drag = 1;
        else if (rect_contains(rect_make(HUE_X - 8, SV_Y - 6, HUE_W + 16, SV_S + 12), ev->x, ev->y)) k->drag = 2;
        pk_mouse(win, k, ev->x, ev->y);
        break;
    case EV_MOUSE_MOVE:
        if (k->drag && (ev->buttons & MOUSE_LEFT)) pk_mouse(win, k, ev->x, ev->y);
        else k->drag = 0;
        break;
    case EV_MOUSE_UP: k->drag = 0; break;
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESCAPE) win_close(win);
        break;
    case EV_CLOSE: {
        window_t *ow = win_find_by_id(k->owner_id);
        paint_t *p = ow ? win_get_userdata(ow) : NULL;
        if (p && p->picker_id == win_id(win)) p->picker_id = 0;
        free(k);
        win_set_userdata(win, NULL);
        break;
    }
    }
}

static void pt_open_picker(paint_t *p, int target, uint32_t initial) {
    window_t *old = p->picker_id ? win_find_by_id(p->picker_id) : NULL;
    if (old) { win_close(old); p->picker_id = 0; }
    picker_t *k = calloc(1, sizeof *k);
    if (!k) return;
    window_t *win = win_create_dialog(p->win, target == 1 ? "Secondary colour" : "Edit colour", PK_W, PK_H, 0);
    if (!win) { free(k); return; }
    k->owner_id = win_id(p->win);
    k->target = target;
    k->orig = k->rgb = initial | 0xFF000000u;
    k->h = 0;
    rgb_to_hsv(k->rgb, &k->h, &k->s, &k->v);
    win_set_userdata(win, k);
    win_set_event_handler(win, pk_event);
    gfx_text(win_surface(win), 0, 0, "", 0, FONT_UI);
    ui_label(win, PK_COL, SV_Y + 150, PK_W - 16 - PK_COL, 18, "Hex");
    k->hex = ui_textbox(win, PK_COL, SV_Y + 170, PK_W - 16 - PK_COL, 30, "");
    ui_textbox_set_maxlen(k->hex, 7);
    ui_set_callback(k->hex, pk_hex_changed, k);
    ui_textbox_on_submit(k->hex, pk_hex_submit, k);
    pk_sync_hex(k);
    ui_button(win, PK_W - 16 - 90 - 8 - 90, PK_H - 46, 90, 32, "Cancel", pk_cancel, k);
    widget_t *ok = ui_button(win, PK_W - 16 - 90, PK_H - 46, 90, 32, "OK", pk_ok, k);
    ui_button_set_default(ok, true);
    p->picker_id = win_id(win);
}

/* ======================================================================================
 * input
 * ====================================================================================== */
static void pt_select_tool(paint_t *p, int t) {
    if (p->drawing) pt_cancel_stroke(p);
    if (t != T_PICKER) p->prev_tool = t;
    else if (p->tool != T_PICKER) p->prev_tool = p->tool;
    p->tool = t;
    toolbox_dirty(p);
    status_dirty(p);
    update_outline(p);
}

static void pt_set_size(paint_t *p, int i) {
    p->size_idx = CLAMP(i, 0, NSIZES - 1);
    toolbox_dirty(p);
    status_dirty(p);
    p->outline = rect_make(0, 0, 0, 0);
    view_dirty(p);
    update_outline(p);
}

static void pt_palette_click(paint_t *p, int hit, int button, bool dbl) {
    if (hit == HIT_FG) { pt_open_picker(p, 0, p->fg); return; }
    if (hit == HIT_BG) { pt_open_picker(p, 1, p->bg); return; }
    if (hit == HIT_EDIT) { pt_open_picker(p, 0, p->fg); return; }
    uint32_t c;
    if (hit >= HIT_CUSTOM && hit < HIT_CUSTOM + NCUSTOM) {
        int slot = hit - HIT_CUSTOM;
        if (!p->custom[slot] || dbl) { pt_open_picker(p, 2 + slot, p->custom[slot] ? p->custom[slot] : p->fg); return; }
        c = p->custom[slot];
    } else if (hit >= 0 && hit < NPAL) {
        c = g_palette[hit];
    } else {
        return;
    }
    if (button == MOUSE_RIGHT) p->bg = c; else p->fg = c;
    palette_dirty(p);
}

static void pt_set_cursor(paint_t *p, int c) {
    if (c == p->cursor) return;
    p->cursor = c;
    win_set_cursor(p->win, c);
}

static void pt_hover(paint_t *p, int x, int y) {
    int ht = tool_hit(x, y);
    int hs = ht < 0 && x < TOOLBOX_W ? size_hit(x, y) : -1;
    int hp = y >= pal_top(p) ? pal_hit(p, x, y) : HIT_NONE;
    if (ht != p->hover_tool || hs != p->hover_size) { p->hover_tool = ht; p->hover_size = hs; toolbox_dirty(p); status_dirty(p); }
    if (hp != p->hover_pal) { p->hover_pal = hp; palette_dirty(p); }
}

static void pt_mouse_down(paint_t *p, const event_t *ev) {
    int x = ev->x, y = ev->y;
    if (rect_contains(p->view, x, y)) {
        if (ev->button == MOUSE_MIDDLE) {
            if (p->drawing) pt_cancel_stroke(p);
            p->panning = true;
            p->pan_mx = x; p->pan_my = y;
            p->pan_sx = p->scroll_x; p->pan_sy = p->scroll_y;
            pt_set_cursor(p, CURSOR_MOVE);
            update_outline(p);
            return;
        }
        if (p->drawing) {          /* the other button cancels the shape in progress */
            if (ev->button != p->draw_button) pt_cancel_stroke(p);
            return;
        }
        if (ev->button != MOUSE_LEFT && ev->button != MOUSE_RIGHT) return;
        int cx, cy;
        to_canvas(p, x, y, &cx, &cy);
        pt_begin_stroke(p, cx, cy, ev->button, ev->mods);
        return;
    }
    if (ev->button != MOUSE_LEFT && ev->button != MOUSE_RIGHT) return;
    int t = tool_hit(x, y);
    if (t >= 0) { pt_select_tool(p, t); return; }
    if (x < TOOLBOX_W) {
        int si = size_hit(x, y);
        if (si >= 0) pt_set_size(p, si);
        return;
    }
    if (y >= pal_top(p)) pt_palette_click(p, pal_hit(p, x, y), ev->button, false);
}

static void pt_mouse_move(paint_t *p, const event_t *ev) {
    p->mx = ev->x;
    p->my = ev->y;
    if (p->panning) {
        if (!(ev->buttons & MOUSE_MIDDLE)) { p->panning = false; return; }
        p->scroll_x = p->pan_sx - (ev->x - p->pan_mx);
        p->scroll_y = p->pan_sy - (ev->y - p->pan_my);
        pt_layout(p);
        view_dirty(p);
        return;
    }
    bool in = rect_contains(p->view, ev->x, ev->y);
    if (p->drawing) {
        if (!(ev->buttons & p->draw_button)) { pt_end_stroke(p); return; }
        int cx, cy;
        to_canvas(p, ev->x, ev->y, &cx, &cy);
        pt_continue_stroke(p, cx, cy, ev->mods);
        p->in_view = in;
        update_outline(p);
        return;
    }
    if (in != p->in_view || in) status_dirty(p);
    p->in_view = in;
    pt_set_cursor(p, in ? CURSOR_CROSSHAIR : CURSOR_ARROW);
    update_outline(p);
    pt_hover(p, ev->x, ev->y);
}

static void pt_mouse_up(paint_t *p, const event_t *ev) {
    if (p->panning && ev->button == MOUSE_MIDDLE) {
        p->panning = false;
        pt_set_cursor(p, rect_contains(p->view, ev->x, ev->y) ? CURSOR_CROSSHAIR : CURSOR_ARROW);
        return;
    }
    if (p->drawing && ev->button == p->draw_button) pt_end_stroke(p);
}

static void pt_wheel(paint_t *p, const event_t *ev) {
    if (ev->mods & MOD_CTRL) { pt_set_zoom(p, p->zoom_idx - ev->wheel, ev->x, ev->y); update_outline(p); return; }
    if ((ev->mods & MOD_SHIFT) || (!p->vs && p->hs)) pt_scroll_by(p, ev->wheel * 48, 0);
    else pt_scroll_by(p, 0, ev->wheel * 48);
    update_outline(p);
}

static void pt_key(paint_t *p, const event_t *ev) {
    int k = ev->key;
    bool ctrl = ev->mods & MOD_CTRL, shift = ev->mods & MOD_SHIFT;
    if (ctrl) {
        switch (k) {
        case 'n': pt_request(p, ACT_NEW); return;
        case 'o': pt_request(p, ACT_OPEN); return;
        case 's': if (shift) pt_save_as(p, ACT_NONE); else pt_save(p, ACT_NONE); return;
        case 'w': pt_request(p, ACT_CLOSE); return;
        case 'z': if (shift) pt_redo(p); else pt_undo(p); return;
        case 'y': pt_redo(p); return;
        case '=': case '+': pt_zoom_step(p, 1); return;
        case '-': pt_zoom_step(p, -1); return;
        case '0': pt_set_zoom(p, 0, p->view.x + p->view.w / 2, p->view.y + p->view.h / 2); return;
        case 'r': op_rotate(p, true); return;
        case 'i': op_colors(p, true); return;
        }
        return;
    }
    if (ev->mods & MOD_ALT) return;
    switch (k) {
    case 'p': pt_select_tool(p, T_PENCIL); break;
    case 'b': pt_select_tool(p, T_BRUSH); break;
    case 'e': pt_select_tool(p, T_ERASER); break;
    case 'f': pt_select_tool(p, T_FILL); break;
    case 'i': case 'k': pt_select_tool(p, T_PICKER); break;
    case 'l': pt_select_tool(p, T_LINE); break;
    case 'r': pt_select_tool(p, shift ? T_FRECT : T_RECT); break;
    case 'o': pt_select_tool(p, shift ? T_FELLIPSE : T_ELLIPSE); break;
    case 'x': pt_swap_colors(p); break;
    case 'g': pt_view_menu(4, p); break;
    case '[': pt_set_size(p, p->size_idx - 1); break;
    case ']': pt_set_size(p, p->size_idx + 1); break;
    case '=': case '+': pt_zoom_step(p, 1); break;
    case '-': pt_zoom_step(p, -1); break;
    case KEY_DELETE: op_clear(p); break;
    case KEY_ESCAPE: if (p->drawing) pt_cancel_stroke(p); break;
    case KEY_LEFT: pt_scroll_by(p, -40, 0); break;
    case KEY_RIGHT: pt_scroll_by(p, 40, 0); break;
    case KEY_UP: pt_scroll_by(p, 0, -40); break;
    case KEY_DOWN: pt_scroll_by(p, 0, 40); break;
    case KEY_PAGEUP: pt_scroll_by(p, 0, -p->view.h + 40); break;
    case KEY_PAGEDOWN: pt_scroll_by(p, 0, p->view.h - 40); break;
    }
}

static void pt_free(paint_t *p) {
    clear_history(p);
    surface_destroy(p->canvas);
    surface_destroy(p->backup);
    free(p);
}

static void pt_event(window_t *win, const event_t *ev) {
    paint_t *p = win_get_userdata(win);
    if (!p) return;
    switch (ev->type) {
    case EV_PAINT: pt_paint(p); break;
    case EV_RESIZE: pt_layout(p); break;
    case EV_MOUSE_DOWN: pt_mouse_down(p, ev); break;
    case EV_MOUSE_MOVE: pt_mouse_move(p, ev); break;
    case EV_MOUSE_UP: pt_mouse_up(p, ev); break;
    case EV_DOUBLE_CLICK:
        if (ev->y >= pal_top(p)) {
            int hit = pal_hit(p, ev->x, ev->y);
            if (hit >= HIT_CUSTOM && hit < HIT_CUSTOM + NCUSTOM) pt_palette_click(p, hit, ev->button, true);
        }
        break;
    case EV_MOUSE_WHEEL: pt_wheel(p, ev); break;
    case EV_MOUSE_LEAVE:
        p->in_view = false;
        p->hover_tool = p->hover_size = -1;
        p->hover_pal = HIT_NONE;
        update_outline(p);
        toolbox_dirty(p);
        palette_dirty(p);
        status_dirty(p);
        break;
    case EV_KEY_DOWN: pt_key(p, ev); break;
    case EV_BLUR:
        if (p->drawing) pt_end_stroke(p);
        p->panning = false;
        break;
    case EV_TIMER:
        if (p->msg[0] && sys_uptime_ms() >= p->msg_until) {
            p->msg[0] = 0;
            status_dirty(p);
        }
        if (!p->msg[0] && p->msg_timer) { win_kill_timer(win, p->msg_timer); p->msg_timer = 0; }
        break;
    case EV_CLOSE_REQUEST: pt_request(p, ACT_CLOSE); break;
    case EV_CLOSE:
        win_set_userdata(win, NULL);
        pt_free(p);
        break;
    }
}

static void paint_launch(const char *arg) {
    paint_t *p = calloc(1, sizeof *p);
    if (!p) return;
    p->canvas = new_blank(DEF_CW, DEF_CH, COLOR_WHITE);
    p->backup = p->canvas ? surface_clone(p->canvas) : NULL;
    if (!p->backup) { surface_destroy(p->canvas); free(p); return; }
    window_t *win = win_create("Untitled - Paint", PT_W, PT_H, WIN_RESIZABLE | WIN_CONFIRM_CLOSE);
    if (!win) { pt_free(p); return; }
    p->win = win;
    p->tool = p->prev_tool = T_BRUSH;
    p->size_idx = 2;
    p->fg = COLOR_BLACK;
    p->bg = COLOR_WHITE;
    p->hover_tool = p->hover_size = -1;
    p->hover_pal = HIT_NONE;
    p->cursor = -1;
    win_set_userdata(win, p);
    win_set_event_handler(win, pt_event);
    win_set_min_size(win, 640, 480);
    p->hsb = ui_scrollbar(win, 0, 0, 10, SB_SIZE, false, on_hscroll, p);
    p->vsb = ui_scrollbar(win, 0, 0, SB_SIZE, 10, true, on_vscroll, p);
    pt_build_menus(p);
    pt_layout(p);
    pt_update_title(p);
    if (arg && *arg && !vfs_is_dir(arg)) pt_load(p, arg);
}

/* App icon: an artist's palette with paint blobs and a brush. */
static void paint_icon(surface_t *s, int x, int y, int size) {
    int S = size;
    gfx_fill_ellipse(s, x + S * 46 / 100, y + S * 58 / 100, S * 43 / 100, S * 36 / 100, RGB(164, 110, 60));
    gfx_fill_ellipse(s, x + S * 46 / 100, y + S * 57 / 100, S * 41 / 100, S * 34 / 100, RGB(246, 222, 182));
    ucircle(s, x, y, S, 0.30, 0.74, 0.085, RGB(164, 110, 60));
    ucircle(s, x, y, S, 0.30, 0.74, 0.060, RGB(214, 182, 140));
    static const struct { double x, y; uint32_t c; } blobs[] = {
        { 0.20, 0.52, RGB(232, 48, 58) }, { 0.33, 0.36, RGB(255, 196, 30) }, { 0.52, 0.32, RGB(52, 180, 90) },
        { 0.70, 0.42, RGB(40, 120, 230) }, { 0.70, 0.66, RGB(150, 70, 200) },
    };
    for (int i = 0; i < ARRAY_SIZE(blobs); i++) {
        ucircle(s, x, y, S, blobs[i].x, blobs[i].y + 0.012, 0.085, color_shade(blobs[i].c, -60));
        ucircle(s, x, y, S, blobs[i].x, blobs[i].y, 0.08, blobs[i].c);
        ucircle(s, x, y, S, blobs[i].x - 0.025, blobs[i].y - 0.025, 0.022, color_alpha(COLOR_WHITE, 170));
    }
    /* brush across the palette */
    ubar(s, x, y, S, 0.97, 0.04, 0.62, 0.42, 0.055, RGB(40, 44, 54));
    ubar(s, x, y, S, 0.96, 0.05, 0.63, 0.41, 0.035, RGB(196, 60, 50));
    ubar(s, x, y, S, 0.64, 0.40, 0.53, 0.52, 0.075, RGB(186, 192, 204));
    ubar(s, x, y, S, 0.60, 0.44, 0.57, 0.47, 0.075, RGB(140, 146, 160));
    ucircle(s, x, y, S, 0.46, 0.58, 0.085, RGB(36, 38, 46));
    utri(s, x, y, S, 0.39, 0.53, 0.52, 0.66, 0.34, 0.72, RGB(36, 38, 46));
    ucircle(s, x, y, S, 0.42, 0.63, 0.055, RGB(40, 120, 230));
    utri(s, x, y, S, 0.37, 0.61, 0.45, 0.69, 0.34, 0.72, RGB(40, 120, 230));
}

REGISTER_APP(.name = "Paint", .category = CAT_GRAPHICS,
             .description = "Draw and edit pictures (BMP)",
             .launch = paint_launch, .draw_icon = paint_icon,
             .file_types = "bmp,dib");
