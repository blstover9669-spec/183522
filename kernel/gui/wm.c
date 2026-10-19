/*
 * wm.c - window manager: window objects, z-order, compositor, input routing,
 * and the kernel main loop.
 *
 * Every window owns one surface holding its decorations and client area. The
 * compositor redraws only dirty screen rectangles: for each one it finds the
 * topmost window that fully covers it, paints from there upwards (drop
 * shadows, then window pixels with anti-aliased rounded-corner masks), draws
 * the cursor and copies the rectangle to the linear framebuffer.
 */
#include "wm_internal.h"

window_t *wm_bottom, *wm_top, *wm_focused;
surface_t *wm_backbuf;
const app_desc_t *wm_current_app;
int wm_mouse_x, wm_mouse_y, wm_mouse_buttons;
int wm_taskbar_h = 48;
int wm_cursor_shape = CURSOR_ARROW;
uint32_t wm_click_serial;

static int scr_w, scr_h;
static int next_win_id = 1;
static int cascade_idx;
static uint32_t frame_serial;
static bool taskbar_dirty;
static int double_click_ms = 450;

/* ---- dirty rectangle list ------------------------------------------------------- */
#define MAX_DIRTY 48
static rect_t dirty[MAX_DIRTY];
static int ndirty;

static int area(rect_t r) { return r.w * r.h; }

void wm_invalidate_screen(rect_t r) {
    r = rect_intersect(r, rect_make(0, 0, scr_w, scr_h));
    if (rect_empty(r)) return;
    for (int i = 0; i < ndirty; i++) {
        rect_t d = dirty[i];
        if (r.x >= d.x && r.y >= d.y && r.x + r.w <= d.x + d.w && r.y + r.h <= d.y + d.h) return;  /* covered */
        rect_t u = rect_union(d, r);
        /* merge when overlapping/adjacent and the union wastes little area */
        if (rect_overlaps(rect_inset(d, -4), r) && area(u) <= area(d) + area(r) + 8192) {
            dirty[i] = u;
            return;
        }
    }
    if (ndirty < MAX_DIRTY) { dirty[ndirty++] = r; return; }
    /* full: merge into the rect whose union grows least */
    int best = 0, bestg = 0x7fffffff;
    for (int i = 0; i < ndirty; i++) {
        int g = area(rect_union(dirty[i], r)) - area(dirty[i]);
        if (g < bestg) { bestg = g; best = i; }
    }
    dirty[best] = rect_union(dirty[best], r);
}

void wm_invalidate_all(void) {
    ndirty = 0;
    wm_invalidate_screen(rect_make(0, 0, scr_w, scr_h));
}

/* ---- geometry helpers -------------------------------------------------------------- */
static bool framed(const window_t *w) { return !(w->flags & (WIN_FRAMELESS | WIN_POPUP)) && !w->fullscreen; }
static bool drawn(const window_t *w) { return w->visible && !w->minimized && !w->closing; }

static bool has_shadow(const window_t *w) {
    if (!theme_get()->shadows || (w->flags & WIN_NO_SHADOW) || w->layer == LAYER_DESKTOP) return false;
    if (w->maximized || w->fullscreen) return false;
    return framed(w) || (w->flags & WIN_POPUP) || w->layer == LAYER_POPUP;
}

#define SHADOW_SPREAD 22
#define SHADOW_OFFSET 5

rect_t wm_visual_rect(window_t *w) {
    if (!has_shadow(w)) return w->frame;
    rect_t r = w->frame;
    return rect_make(r.x - SHADOW_SPREAD, r.y - SHADOW_SPREAD, r.w + 2 * SHADOW_SPREAD,
                     r.h + 2 * SHADOW_SPREAD + SHADOW_OFFSET);
}

void wm_invalidate_window(window_t *w) { wm_invalidate_screen(wm_visual_rect(w)); }

void wm_invalidate_client_rect(window_t *w, rect_t r) {
    r = rect_intersect(r, rect_make(0, 0, w->cw, w->ch));
    if (rect_empty(r) || !drawn(w)) return;
    r.x += w->frame.x + w->cx;
    r.y += w->frame.y + w->cy;
    wm_invalidate_screen(r);
}

int screen_width(void) { return scr_w; }
int screen_height(void) { return scr_h; }
int screen_work_height(void) { return scr_h - wm_taskbar_h; }
uint32_t wm_frame_serial(void) { return frame_serial; }

/* ---- rounded corner masks --------------------------------------------------------- */
#define MAX_RADIUS 16
static uint8_t cmask[MAX_RADIUS + 1][MAX_RADIUS][MAX_RADIUS];

static void build_corner_masks(void) {
    for (int r = 1; r <= MAX_RADIUS; r++)
        for (int y = 0; y < r; y++)
            for (int x = 0; x < r; x++) {
                double dx = x + 0.5 - r, dy = y + 0.5 - r;
                double cov = r + 0.5 - sqrt(dx * dx + dy * dy);
                cmask[r][y][x] = cov <= 0 ? 0 : cov >= 1 ? 255 : (uint8_t)(cov * 255);
            }
}

static inline uint32_t blend_px(uint32_t d, uint32_t s, uint32_t a) {
    uint32_t a1 = a + (a >> 7), ia = 256 - a1;
    uint32_t rb = (((s & 0xFF00FFu) * a1 + (d & 0xFF00FFu) * ia) >> 8) & 0xFF00FFu;
    uint32_t g = (((s & 0x00FF00u) * a1 + (d & 0x00FF00u) * ia) >> 8) & 0x00FF00u;
    return 0xFF000000u | rb | g;
}

static void blit_window(surface_t *dst, window_t *w, rect_t clip) {
    rect_t f = w->frame;
    rect_t r = rect_intersect(f, clip);
    if (rect_empty(r)) return;
    int rad = MIN(w->radius, MAX_RADIUS);
    surface_t *s = w->surf;
    for (int y = r.y; y < r.y + r.h; y++) {
        int wy = y - f.y;
        const uint32_t *src = s->pixels + (size_t)wy * s->pitch + (r.x - f.x);
        uint32_t *d = dst->pixels + (size_t)y * dst->pitch + r.x;
        int cy = -1;
        if (rad) {
            if (wy < rad) cy = wy;
            else if (wy >= f.h - rad) cy = f.h - 1 - wy;
        }
        if (w->opacity < 255) {
            /* fading in: blend everything, corners scaled by their mask */
            const uint8_t *m = cy >= 0 ? cmask[rad][cy] : NULL;
            int wx0 = r.x - f.x;
            for (int x = 0; x < r.w; x++) {
                int wx = wx0 + x;
                uint32_t a = (uint32_t)w->opacity;
                if (m) {
                    int cx = wx < rad ? wx : (wx >= f.w - rad ? f.w - 1 - wx : -1);
                    if (cx >= 0) a = a * m[cx] / 255;
                }
                if (a) d[x] = blend_px(d[x], src[x], a);
            }
            continue;
        }
        if (cy < 0) { memcpy(d, src, (size_t)r.w * 4); continue; }
        const uint8_t *m = cmask[rad][cy];
        int wx0 = r.x - f.x;
        for (int x = 0; x < r.w; x++) {
            int wx = wx0 + x;
            int cx = wx < rad ? wx : (wx >= f.w - rad ? f.w - 1 - wx : -1);
            if (cx < 0) { d[x] = src[x]; continue; }
            uint32_t a = m[cx];
            if (a == 255) d[x] = src[x];
            else if (a) d[x] = blend_px(d[x], src[x], a);
        }
    }
}

/* Draw everything inside r into dst (dst must be screen-sized). */
static void compose_rect(surface_t *dst, rect_t r, bool with_cursor) {
    surface_set_clip(dst, r);
    window_t *start = NULL;
    for (window_t *w = wm_top; w; w = w->below) {
        if (!drawn(w) || w->opacity < 255) continue;
        rect_t op = w->frame;
        if (w->radius) { op.y += w->radius; op.h -= 2 * w->radius; }
        if (r.x >= op.x && r.y >= op.y && r.x + r.w <= op.x + op.w && r.y + r.h <= op.y + op.h) {
            start = w;      /* fully covers r: nothing below it is visible */
            break;
        }
    }
    if (!start) {
        gfx_fill_rect(dst, r.x, r.y, r.w, r.h, RGB(20, 24, 40));
        start = wm_bottom;
    }
    for (window_t *w = start; w; w = w->above) {
        if (!drawn(w)) continue;
        if (w != start && has_shadow(w)) {
            rect_t vr = wm_visual_rect(w);
            if (rect_overlaps(vr, r)) {
                bool active = (w == wm_focused) || (w->flags & WIN_POPUP);
                int sa = (active ? 80 : 46) * w->opacity / 255;
                gfx_shadow_occluded(dst, w->frame.x, w->frame.y + SHADOW_OFFSET, w->frame.w, w->frame.h,
                                    w->radius, SHADOW_SPREAD, sa, w->opacity < 255 ? rect_make(0, 0, 0, 0) : w->frame, w->radius);
            }
        }
        if (rect_overlaps(w->frame, r)) blit_window(dst, w, r);
    }
    if (with_cursor && wm_cursor_shape != CURSOR_NONE) {
        rect_t cr = cursor_rect(wm_mouse_x, wm_mouse_y, wm_cursor_shape);
        if (rect_overlaps(cr, r)) cursor_draw(dst, wm_mouse_x, wm_mouse_y, wm_cursor_shape);
    }
    surface_reset_clip(dst);
}

static uint32_t fps_count, fps_last_tick;
static int fps_value;
int sys_fps(void) { return fps_value; }

static void composite(void) {
    if (!ndirty) return;
    int n = ndirty;
    rect_t local[MAX_DIRTY];
    memcpy(local, dirty, sizeof(rect_t) * (size_t)n);
    ndirty = 0;
    for (int i = 0; i < n; i++) compose_rect(wm_backbuf, local[i], true);
    for (int i = 0; i < n; i++) fb_present(wm_backbuf, local[i]);
    frame_serial++;
    fps_count++;
    if (g_ticks - fps_last_tick >= 1000) {
        fps_value = (int)fps_count;
        fps_count = 0;
        fps_last_tick = g_ticks;
    }
}

surface_t *screen_capture(void) {
    surface_t *s = surface_create(scr_w, scr_h);
    if (!s) return NULL;
    compose_rect(s, rect_make(0, 0, scr_w, scr_h), false);
    return s;
}

/* ---- z-order ------------------------------------------------------------------------ */
static void zlist_unlink(window_t *w) {
    if (w->below) w->below->above = w->above; else if (wm_bottom == w) wm_bottom = w->above;
    if (w->above) w->above->below = w->below; else if (wm_top == w) wm_top = w->below;
    w->above = w->below = NULL;
}

/* insert w at the top of its layer */
static void zlist_insert(window_t *w) {
    window_t *pos = wm_bottom;   /* first window with a higher layer */
    while (pos && pos->layer <= w->layer) pos = pos->above;
    if (!pos) {
        w->below = wm_top; w->above = NULL;
        if (wm_top) wm_top->above = w; else wm_bottom = w;
        wm_top = w;
    } else {
        w->above = pos; w->below = pos->below;
        if (pos->below) pos->below->above = w; else wm_bottom = w;
        pos->below = w;
    }
}

void wm_raise(window_t *w) {
    if (!w) return;
    /* already at the top of its layer? */
    if (!w->above || w->above->layer > w->layer) {
        return;
    }
    zlist_unlink(w);
    zlist_insert(w);
    wm_invalidate_window(w);
    /* owned dialogs stay above their owner */
    for (window_t *o = wm_bottom; o; o = o->above) {
        if (o->owner == w && o->layer == w->layer && !o->closing) {
            zlist_unlink(o);
            zlist_insert(o);
            wm_invalidate_window(o);
            break;
        }
    }
}

/* ---- decorations ------------------------------------------------------------------- */
static bool has_max_btn(const window_t *w) {
    return (w->flags & WIN_RESIZABLE) && !(w->flags & (WIN_NO_MAXIMIZE | WIN_DIALOG));
}
static bool has_min_btn(const window_t *w) { return !(w->flags & (WIN_NO_MINIMIZE | WIN_DIALOG)); }

#define BTN_W 46

static rect_t title_btn_rect(const window_t *w, int ht) {
    int W = w->frame.w;
    int slot = 0;
    if (ht == HT_CLOSE) slot = 1;
    else if (ht == HT_MAXIMIZE) slot = 2;
    else if (ht == HT_MINIMIZE) slot = has_max_btn(w) ? 3 : 2;
    return rect_make(W - slot * BTN_W, 0, BTN_W, WIN_TITLE_H - 1);
}

static bool window_active(const window_t *w) {
    if (w == wm_focused) return true;
    /* an owner with an active modal dialog looks inactive */
    return false;
}

static void draw_border(window_t *w, bool only_bottom) {
    const theme_t *t = theme_get();
    bool active = window_active(w);
    uint32_t bc = active ? t->window_border : color_mix(t->window_border, t->window_bg, 90);
    surface_t *s = w->surf;
    surface_reset_clip(s);
    int W = w->frame.w, H = w->frame.h, r = w->radius;
    if (only_bottom) {
        if (!r) { gfx_hline(s, 0, H - 1, W, bc); return; }
        surface_set_clip(s, rect_make(0, H - r - 1, W, r + 1));
        gfx_rounded_rect(s, 0, 0, W, H, r, bc);
        surface_reset_clip(s);
        return;
    }
    gfx_rounded_rect(s, 0, 0, W, H, r, bc);
}

void wm_redraw_decor(window_t *w) {
    w->deco_dirty = false;
    if (!framed(w)) return;
    const theme_t *t = theme_get();
    bool active = window_active(w);
    surface_t *s = w->surf;
    surface_reset_clip(s);
    int W = w->frame.w;
    uint32_t tb = active ? t->title_active : t->title_inactive;
    uint32_t tc = active ? t->title_text : t->title_text_inactive;
    gfx_fill_rect(s, 0, 0, W, WIN_TITLE_H, tb);
    gfx_hline(s, 1, WIN_TITLE_H - 1, W - 2, color_mix(tb, t->window_border, 110));

    int tx = 12;
    if (w->app) {
        app_draw_icon(s, w->app, 10, (WIN_TITLE_H - 18) / 2, 18);
        tx = 36;
    }
    int nbtn = 1 + has_max_btn(w) + has_min_btn(w);
    int maxw = W - nbtn * BTN_W - tx - 8;
    gfx_text_ellipsis(s, tx, (WIN_TITLE_H - font_height(FONT_UI)) / 2, maxw, w->title, tc, FONT_UI);

    int hts[3] = { HT_CLOSE, HT_MAXIMIZE, HT_MINIMIZE };
    for (int i = 0; i < 3; i++) {
        int ht = hts[i];
        if (ht == HT_MAXIMIZE && !has_max_btn(w)) continue;
        if (ht == HT_MINIMIZE && !has_min_btn(w)) continue;
        rect_t br = title_btn_rect(w, ht);
        bool hov = w->hover_ht == ht, prs = w->pressed_ht == ht && hov;
        uint32_t gc = tc;
        if (ht == HT_CLOSE && (hov || prs)) {
            gfx_fill_rect(s, br.x, br.y, br.w, br.h, prs ? RGB(241, 112, 122) : RGB(232, 17, 35));
            gc = COLOR_WHITE;
        } else if (hov) {
            gfx_fill_rect(s, br.x, br.y, br.w, br.h, color_mix(tb, t->title_text, prs ? 60 : 30));
        }
        int glyph = ht == HT_CLOSE ? UI_GLYPH_CLOSE : ht == HT_MINIMIZE ? UI_GLYPH_MINIMIZE
                    : (w->maximized ? UI_GLYPH_RESTORE : UI_GLYPH_MAXIMIZE);
        ui_draw_glyph(s, glyph, br.x + (br.w - 10) / 2, br.y + (br.h - 10) / 2, 10, gc);
    }
    draw_border(w, false);
    wm_invalidate_screen(rect_make(w->frame.x, w->frame.y, w->frame.w, WIN_TITLE_H));
    wm_invalidate_screen(rect_make(w->frame.x, w->frame.y, 1, w->frame.h));
    wm_invalidate_screen(rect_make(w->frame.x + w->frame.w - 1, w->frame.y, 1, w->frame.h));
    wm_invalidate_screen(rect_make(w->frame.x, w->frame.y + w->frame.h - MAX(1, w->radius), w->frame.w, MAX(1, w->radius)));
}

/* ---- window creation & geometry ---------------------------------------------------------- */
static void layout_client(window_t *w) {
    if (framed(w)) { w->cx = WIN_BORDER; w->cy = WIN_TITLE_H; }
    else { w->cx = 0; w->cy = 0; }
    w->client = surface_sub(w->surf, rect_make(w->cx, w->cy, w->cw, w->ch));
}

static void frame_size_for(const window_t *w, int cw, int ch, int *fw, int *fh) {
    if (framed(w)) { *fw = cw + 2 * WIN_BORDER; *fh = ch + WIN_TITLE_H + WIN_BORDER; }
    else { *fw = cw; *fh = ch; }
}

window_t *wm_create_ex(const char *title, int cw, int ch, uint32_t flags, int layer) {
    window_t *w = kcalloc(1, sizeof *w);
    if (!w) return NULL;
    w->magic = WIN_MAGIC;
    w->id = next_win_id++;
    w->flags = flags;
    if (flags & WIN_POPUP) layer = LAYER_POPUP;
    else if ((flags & WIN_TOPMOST) && layer < LAYER_TOPMOST) layer = LAYER_TOPMOST;
    w->layer = layer;
    strlcpy(w->title, title ? title : "", sizeof w->title);
    int maxcw = scr_w - (framed(w) ? 2 * WIN_BORDER : 0);
    int maxch = scr_h - wm_taskbar_h - (framed(w) ? WIN_TITLE_H + WIN_BORDER : 0);
    if (layer == LAYER_DESKTOP) maxch = scr_h;
    w->cw = CLAMP(cw, 1, maxcw);
    w->ch = CLAMP(ch, 1, layer == LAYER_DESKTOP || layer == LAYER_TOPMOST || layer == LAYER_POPUP ? scr_h : maxch);
    w->min_cw = MIN(w->cw, 120);
    w->min_ch = MIN(w->ch, 60);
    int fw, fh;
    frame_size_for(w, w->cw, w->ch, &fw, &fh);
    w->surf = surface_create(fw, fh);
    if (!w->surf) { kfree(w); return NULL; }
    gfx_clear(w->surf, theme_get()->window_bg);
    w->radius = framed(w) ? 8 : ((flags & WIN_POPUP) ? 8 : 0);
    layout_client(w);
    w->app = wm_current_app;
    w->cursor = CURSOR_ARROW;
    w->opacity = 255;
    if (layer == LAYER_NORMAL || (flags & WIN_POPUP)) { w->opacity = 0; w->fade_start = g_ticks; }

    /* placement */
    int x, y;
    int work_h = scr_h - wm_taskbar_h;
    if (flags & WIN_CENTER) {
        x = (scr_w - fw) / 2;
        y = (work_h - fh) / 2;
        /* don't stack a second centred window exactly on top of the first */
        for (int tries = 0; tries < 8; tries++) {
            bool clash = false;
            for (window_t *o = wm_bottom; o; o = o->above)
                if (!o->closing && o->layer == LAYER_NORMAL && o->frame.x == x && o->frame.y == y) clash = true;
            if (!clash) break;
            x += 28;
            y += 28;
        }
        if (x + fw > scr_w) x = MAX(0, scr_w - fw);
        if (y + fh > work_h) y = MAX(0, work_h - fh);
    } else if (layer == LAYER_NORMAL && !(flags & (WIN_DIALOG | WIN_POPUP))) {
        x = 72 + cascade_idx * 30;
        y = 40 + cascade_idx * 30;
        cascade_idx = (cascade_idx + 1) % 9;
        if (x + fw > scr_w) x = MAX(0, scr_w - fw - 20);
        if (y + fh > work_h) y = MAX(0, work_h - fh - 10);
    } else {
        x = (scr_w - fw) / 2;
        y = (work_h - fh) / 2;
    }
    w->frame = rect_make(x, y, fw, fh);
    w->normal_frame = w->frame;
    w->visible = !(flags & WIN_HIDDEN);
    w->need_paint = true;
    w->paint_rect = rect_make(0, 0, w->cw, w->ch);
    w->deco_dirty = framed(w);
    zlist_insert(w);
    if (w->visible) {
        wm_invalidate_window(w);
        if (layer == LAYER_NORMAL) wm_focus(w);
    }
    wm_taskbar_changed();
    return w;
}

window_t *win_create(const char *title, int cw, int ch, uint32_t flags) {
    window_t *w = wm_create_ex(title, cw, ch, flags, LAYER_NORMAL);
    if (w) klog("wm: window %d \"%s\" %dx%d", w->id, w->title, w->cw, w->ch);
    return w;
}

window_t *win_create_dialog(window_t *owner, const char *title, int cw, int ch, uint32_t flags) {
    if (owner && !win_is_valid(owner)) owner = NULL;
    const app_desc_t *saved = wm_current_app;
    if (owner) wm_current_app = owner->app;
    window_t *w = wm_create_ex(title, cw, ch, flags | WIN_DIALOG | WIN_HIDDEN, LAYER_NORMAL);
    wm_current_app = saved;
    if (!w) return NULL;
    w->owner = owner;
    int work_h = scr_h - wm_taskbar_h;
    rect_t f = w->frame;
    if (owner && !owner->minimized) {
        f.x = owner->frame.x + (owner->frame.w - f.w) / 2;
        f.y = owner->frame.y + (owner->frame.h - f.h) / 3;
        owner->modal = w;
    } else {
        f.x = (scr_w - f.w) / 2;
        f.y = (work_h - f.h) / 2;
    }
    f.x = CLAMP(f.x, 0, MAX(0, scr_w - f.w));
    f.y = CLAMP(f.y, 0, MAX(0, work_h - f.h));
    w->frame = f;
    w->normal_frame = f;
    if (!(flags & WIN_HIDDEN)) win_show(w);
    return w;
}

void wm_set_frame(window_t *w, rect_t f) {
    if (f.x == w->frame.x && f.y == w->frame.y && f.w == w->frame.w && f.h == w->frame.h) return;
    if (drawn(w)) wm_invalidate_window(w);
    w->frame = f;
    if (drawn(w)) wm_invalidate_window(w);
}

/* Resize the client area to cw x ch (frame position kept). */
static void resize_client(window_t *w, int cw, int ch) {
    cw = MAX(cw, 1);
    ch = MAX(ch, 1);
    if (cw == w->cw && ch == w->ch) return;
    int fw, fh;
    frame_size_for(w, cw, ch, &fw, &fh);
    surface_t *ns = surface_create(fw, fh);
    if (!ns) return;
    gfx_clear(ns, theme_get()->window_bg);
    /* keep old client pixels to avoid flashing until the app repaints */
    gfx_blit_rect(ns, w->cx, w->cy, w->surf, w->cx, w->cy, MIN(cw, w->cw), MIN(ch, w->ch));
    if (drawn(w)) wm_invalidate_window(w);
    surface_destroy(w->surf);
    w->surf = ns;
    w->cw = cw;
    w->ch = ch;
    w->frame.w = fw;
    w->frame.h = fh;
    layout_client(w);
    w->deco_dirty = framed(w);
    w->need_paint = true;
    w->paint_rect = rect_make(0, 0, cw, ch);
    if (drawn(w)) wm_invalidate_window(w);
    event_t ev = { .type = EV_RESIZE, .w = cw, .h = ch };
    wm_send(w, &ev);
}

/* ---- focus ------------------------------------------------------------------------- */
static bool focusable(const window_t *w) {
    return w && drawn(w) && (w->layer == LAYER_NORMAL || w->layer == LAYER_DESKTOP ||
                             (w->layer == LAYER_TOPMOST && w != g_taskbar_win));
}

void wm_focus(window_t *w) {
    if (w && w->modal && win_is_valid(w->modal)) w = w->modal;
    if (w && !focusable(w)) return;
    if (w && w->layer != LAYER_DESKTOP) wm_raise(w);
    if (w == wm_focused) return;
    window_t *old = wm_focused;
    wm_focused = w;
    if (old && win_is_valid(old)) {
        old->deco_dirty = framed(old);
        event_t ev = { .type = EV_BLUR };
        wm_send(old, &ev);
        if (has_shadow(old)) wm_invalidate_window(old);
    }
    if (w) {
        w->deco_dirty = framed(w);
        event_t ev = { .type = EV_FOCUS };
        wm_send(w, &ev);
        if (has_shadow(w)) wm_invalidate_window(w);
    }
    wm_update_taskbar_visibility();
    wm_taskbar_changed();
}

/* The taskbar hides while a fullscreen window is active. */
void wm_update_taskbar_visibility(void) {
    if (!g_taskbar_win) return;
    bool hide = wm_focused && wm_focused->fullscreen && drawn(wm_focused);
    if (g_taskbar_win->visible == !hide) return;
    g_taskbar_win->visible = !hide;
    g_taskbar_win->need_paint = true;
    wm_invalidate_window(g_taskbar_win);
}

static void focus_next(window_t *except) {
    for (window_t *w = wm_top; w; w = w->below) {
        if (w == except || w->layer != LAYER_NORMAL || !drawn(w)) continue;
        wm_focus(w);
        return;
    }
    wm_focused = NULL;
    if (g_desktop_win) wm_focus(g_desktop_win);
    wm_taskbar_changed();
}

/* ---- public window API -------------------------------------------------------------- */
bool win_is_valid(window_t *w) {
    if (!w) return false;
    for (window_t *x = wm_bottom; x; x = x->above)
        if (x == w) return w->magic == WIN_MAGIC && !w->closing;
    return false;
}

void win_set_event_handler(window_t *w, win_event_fn fn) { if (w) w->handler = fn; }
void win_set_userdata(window_t *w, void *d) { if (w) w->userdata = d; }
void *win_get_userdata(window_t *w) { return w ? w->userdata : NULL; }
surface_t *win_surface(window_t *w) { return w ? &w->client : NULL; }
int win_width(window_t *w) { return w ? w->cw : 0; }
int win_height(window_t *w) { return w ? w->ch : 0; }
uint32_t win_flags(window_t *w) { return w ? w->flags : 0; }
const app_desc_t *win_get_app(window_t *w) { return w ? w->app : NULL; }
window_t *win_get_owner(window_t *w) { return w ? w->owner : NULL; }
int win_id(window_t *w) { return w ? w->id : 0; }
window_t *win_get_focused(void) { return wm_focused; }

window_t *win_find_by_id(int id) {
    for (window_t *w = wm_bottom; w; w = w->above)
        if (w->id == id && !w->closing) return w;
    return NULL;
}

void win_invalidate(window_t *w) {
    if (!w || w->closing) return;
    w->need_paint = true;
    w->paint_rect = rect_make(0, 0, w->cw, w->ch);
}

void win_invalidate_rect(window_t *w, int x, int y, int ww, int hh) {
    if (!w || w->closing) return;
    rect_t r = rect_intersect(rect_make(x, y, ww, hh), rect_make(0, 0, w->cw, w->ch));
    if (rect_empty(r)) return;
    w->paint_rect = w->need_paint ? rect_union(w->paint_rect, r) : r;
    w->need_paint = true;
}

void win_update(window_t *w, int x, int y, int ww, int hh) {
    if (!w || w->closing) return;
    if (ww <= 0 || hh <= 0) { x = 0; y = 0; ww = w->cw; hh = w->ch; }
    wm_invalidate_client_rect(w, rect_make(x, y, ww, hh));
}

void win_set_title(window_t *w, const char *title) {
    if (!w) return;
    strlcpy(w->title, title ? title : "", sizeof w->title);
    w->deco_dirty = framed(w);
    wm_taskbar_changed();
}

const char *win_get_title(window_t *w) { return w ? w->title : ""; }

void win_show(window_t *w) {
    if (!w || w->visible) return;
    w->visible = true;
    if (w->layer == LAYER_NORMAL || (w->flags & WIN_POPUP)) { w->opacity = 0; w->fade_start = g_ticks; }
    w->need_paint = true;
    w->paint_rect = rect_make(0, 0, w->cw, w->ch);
    wm_invalidate_window(w);
    if (w->layer == LAYER_NORMAL) wm_focus(w);
    wm_taskbar_changed();
}

void win_hide(window_t *w) {
    if (!w || !w->visible) return;
    wm_invalidate_window(w);
    w->visible = false;
    if (w == wm_focused) focus_next(w);
    wm_taskbar_changed();
}

void win_resize(window_t *w, int cw, int ch) {
    if (!w || w->closing) return;
    int maxcw = scr_w - (framed(w) ? 2 * WIN_BORDER : 0);
    int maxch = scr_h - (framed(w) ? WIN_TITLE_H + WIN_BORDER : 0);
    if (w->layer == LAYER_NORMAL) maxch -= wm_taskbar_h;
    resize_client(w, CLAMP(cw, 1, maxcw), CLAMP(ch, 1, maxch));
    /* keep on screen */
    rect_t f = w->frame;
    if (f.x + f.w > scr_w) f.x = MAX(0, scr_w - f.w);
    if (w->layer == LAYER_NORMAL && f.y + f.h > scr_h - wm_taskbar_h) f.y = MAX(0, scr_h - wm_taskbar_h - f.h);
    wm_set_frame(w, f);
}

void win_set_min_size(window_t *w, int cw, int ch) {
    if (!w) return;
    w->min_cw = MAX(cw, 40);
    w->min_ch = MAX(ch, 20);
}

void win_move(window_t *w, int x, int y) {
    if (!w) return;
    wm_set_frame(w, rect_make(x, y, w->frame.w, w->frame.h));
}

rect_t win_frame_rect(window_t *w) { return w ? w->frame : rect_make(0, 0, 0, 0); }
rect_t win_client_rect(window_t *w) {
    return w ? rect_make(w->frame.x + w->cx, w->frame.y + w->cy, w->cw, w->ch) : rect_make(0, 0, 0, 0);
}

void win_focus(window_t *w) {
    if (!w || w->closing) return;
    if (w->minimized) win_restore(w);
    if (!w->visible) win_show(w);
    wm_focus(w);
}

bool win_is_focused(window_t *w) { return w && w == wm_focused; }
bool win_is_minimized(window_t *w) { return w && w->minimized; }
bool win_is_maximized(window_t *w) { return w && w->maximized; }
void win_set_cursor(window_t *w, int c) { if (w) w->cursor = c; }

void win_minimize(window_t *w) {
    if (!w || w->minimized || w->layer != LAYER_NORMAL) return;
    wm_invalidate_window(w);
    w->minimized = true;
    if (w->modal && win_is_valid(w->modal)) win_minimize(w->modal);
    if (w == wm_focused || (wm_focused && wm_focused->owner == w)) focus_next(w);
    wm_update_taskbar_visibility();
    wm_taskbar_changed();
}

void win_maximize(window_t *w) {
    if (!w || w->maximized || !framed(w)) return;
    if (w->minimized) { w->minimized = false; }
    wm_invalidate_window(w);
    w->normal_frame = w->frame;
    w->maximized = true;
    w->radius = 0;
    w->frame.x = 0;
    w->frame.y = 0;
    resize_client(w, scr_w - 2 * WIN_BORDER, scr_h - wm_taskbar_h - WIN_TITLE_H - WIN_BORDER);
    w->deco_dirty = true;
    wm_invalidate_window(w);
    wm_focus(w);
}

void win_restore(window_t *w) {
    if (!w) return;
    if (w->minimized) {
        w->minimized = false;
        w->need_paint = true;
        w->paint_rect = rect_make(0, 0, w->cw, w->ch);
        wm_invalidate_window(w);
        wm_focus(w);
        wm_taskbar_changed();
        return;
    }
    if (!w->maximized) return;
    wm_invalidate_window(w);
    w->maximized = false;
    w->radius = 8;
    rect_t nf = w->normal_frame;
    w->frame.x = nf.x;
    w->frame.y = nf.y;
    int cw = nf.w - 2 * WIN_BORDER, ch = nf.h - WIN_TITLE_H - WIN_BORDER;
    resize_client(w, cw, ch);
    w->deco_dirty = true;
    wm_invalidate_window(w);
}

void win_set_fullscreen(window_t *w, bool on) {
    if (!w || w->closing || w->layer != LAYER_NORMAL || w->fullscreen == on) return;
    wm_invalidate_window(w);
    if (on) {
        w->fs_saved_max = w->maximized;
        w->fs_saved_frame = w->maximized ? w->normal_frame : w->frame;
        w->maximized = false;
        w->fullscreen = true;
        w->radius = 0;
        w->frame.x = 0;
        w->frame.y = 0;
        int ocw = w->cw;
        w->cw = -1;   /* force reallocation even if the client size happens to match */
        (void)ocw;
        resize_client(w, scr_w, scr_h);
    } else {
        w->fullscreen = false;
        rect_t f = w->fs_saved_frame;
        w->radius = framed(w) ? 8 : 0;
        w->frame.x = f.x;
        w->frame.y = f.y;
        w->cw = -1;
        int cw = framed(w) ? f.w - 2 * WIN_BORDER : f.w, ch = framed(w) ? f.h - WIN_TITLE_H - WIN_BORDER : f.h;
        resize_client(w, cw, ch);
        if (w->fs_saved_max) { w->normal_frame = f; win_maximize(w); }
    }
    w->deco_dirty = framed(w);
    wm_invalidate_window(w);
    wm_focus(w);
    wm_update_taskbar_visibility();
    wm_taskbar_changed();
}

bool win_is_fullscreen(window_t *w) { return w && w->fullscreen; }

/* ---- closing ------------------------------------------------------------------------------ */
static void close_window(window_t *w, bool callbacks) {
    if (!w || w->closing || w->magic != WIN_MAGIC) return;
    if (!callbacks) w->no_callbacks = true;
    /* close owned windows (dialogs, popups) first - without firing their callbacks */
    for (window_t *o = wm_bottom; o; o = o->above)
        if (o->owner == w && !o->closing) close_window(o, false);
    w->closing = true;
    event_t ev = { .type = EV_CLOSE };
    const app_desc_t *saved = wm_current_app;
    wm_current_app = w->app;
    if (w->handler) w->handler(w, &ev);
    wm_current_app = saved;
    if (w->owner && w->owner->modal == w) w->owner->modal = NULL;
    wm_invalidate_window(w);
    if (w == wm_focused) {
        if (w->owner && win_is_valid(w->owner) && drawn(w->owner)) { wm_focused = NULL; wm_focus(w->owner); }
        else focus_next(w);
    }
    timers_remove_window(w);
    wm_update_taskbar_visibility();
    wm_taskbar_changed();
}

void win_close(window_t *w) { close_window(w, true); }

static window_t *capture_win, *hover_win, *drag_win;

static void destroy_closed(void) {
    window_t *w = wm_bottom;
    while (w) {
        window_t *next = w->above;
        if (w->closing) {
            zlist_unlink(w);
            if (w->on_destroy) w->on_destroy(w);
            ui_destroy_all(w);
            timers_remove_window(w);
            if (capture_win == w) capture_win = NULL;
            if (hover_win == w) hover_win = NULL;
            if (drag_win == w) drag_win = NULL;
            if (wm_focused == w) wm_focused = NULL;
            for (window_t *o = wm_bottom; o; o = o->above) {
                if (o->owner == w) o->owner = NULL;
                if (o->modal == w) o->modal = NULL;
            }
            surface_destroy(w->surf);
            w->magic = 0;
            kfree(w);
        }
        w = next;
    }
}

/* ---- event delivery ---------------------------------------------------------------------- */
void wm_send(window_t *w, const event_t *ev) {
    if (!w || w->closing || w->magic != WIN_MAGIC) return;
    const app_desc_t *saved = wm_current_app;
    wm_current_app = w->app;
    if (!ui_dispatch(w, ev) && w->handler && !w->closing) w->handler(w, ev);
    wm_current_app = saved;
}

/* posted events */
#define POST_Q 64
static struct { int win_id; intptr_t data; } postq[POST_Q];
static int post_head, post_tail;

void win_post_event(window_t *w, intptr_t data) {
    if (!w) return;
    int next = (post_head + 1) % POST_Q;
    if (next == post_tail) return;
    postq[post_head].win_id = w->id;
    postq[post_head].data = data;
    post_head = next;
}

static void deliver_posted(void) {
    int end = post_head;
    while (post_tail != end) {
        int id = postq[post_tail].win_id;
        intptr_t data = postq[post_tail].data;
        post_tail = (post_tail + 1) % POST_Q;
        window_t *w = win_find_by_id(id);
        if (w) {
            event_t ev = { .type = EV_USER, .data = data };
            wm_send(w, &ev);
        }
    }
}

/* ---- window enumeration ---------------------------------------------------------------- */
int win_list(window_t **out, int max) {
    int n = 0;
    for (window_t *w = wm_bottom; w; w = w->above) {
        if (w->closing || w->layer != LAYER_NORMAL || (w->flags & (WIN_NO_TASKBAR | WIN_DIALOG))) continue;
        if (!w->visible && !w->minimized) continue;
        if (out && n < max) out[n] = w;
        n++;
    }
    return n;
}

void wm_taskbar_changed(void) { taskbar_dirty = true; }

/* ---- popups ----------------------------------------------------------------------------- */
bool wm_any_popup(void) {
    for (window_t *w = wm_top; w && w->layer == LAYER_POPUP; w = w->below)
        if (!w->closing && (w->flags & WIN_POPUP)) return true;
    return false;
}

void wm_close_popups(window_t *except) {
    for (window_t *w = wm_top; w; w = w->below)
        if ((w->flags & WIN_POPUP) && w != except && !w->closing) close_window(w, true);
}

static window_t *top_popup(void) {
    for (window_t *w = wm_top; w; w = w->below)
        if ((w->flags & WIN_POPUP) && drawn(w)) return w;
    return NULL;
}

/* ---- hit testing ------------------------------------------------------------------------ */
window_t *wm_window_at(int x, int y) {
    for (window_t *w = wm_top; w; w = w->below) {
        if (!drawn(w) || !rect_contains(w->frame, x, y)) continue;
        /* transparent rounded corners do not catch clicks */
        int lx = x - w->frame.x, ly = y - w->frame.y, r = MIN(w->radius, MAX_RADIUS);
        if (r) {
            int cx = lx < r ? lx : (lx >= w->frame.w - r ? w->frame.w - 1 - lx : -1);
            int cy = ly < r ? ly : (ly >= w->frame.h - r ? w->frame.h - 1 - ly : -1);
            if (cx >= 0 && cy >= 0 && cmask[r][cy][cx] < 128) continue;
        }
        return w;
    }
    return NULL;
}

static int hit_test(window_t *w, int x, int y) {
    if (!w || !rect_contains(w->frame, x, y)) return HT_NONE;
    int lx = x - w->frame.x, ly = y - w->frame.y;
    if (!framed(w)) return HT_CLIENT;
    int W = w->frame.w, H = w->frame.h;
    if ((w->flags & WIN_RESIZABLE) && !w->maximized) {
        const int g = 5, c = 14;
        bool l = lx < g, r = lx >= W - g, t = ly < 4, b = ly >= H - g;
        bool lc = lx < c, rc = lx >= W - c, tc = ly < c, bc = ly >= H - c;
        if ((t && lc) || (l && tc)) return HT_TOPLEFT;
        if ((t && rc) || (r && tc)) return HT_TOPRIGHT;
        if ((b && lc) || (l && bc)) return HT_BOTTOMLEFT;
        if ((b && rc) || (r && bc)) return HT_BOTTOMRIGHT;
        if (l) return HT_LEFT;
        if (r) return HT_RIGHT;
        if (b) return HT_BOTTOM;
        if (t) return HT_TOP;
    }
    if (ly < WIN_TITLE_H) {
        if (rect_contains(title_btn_rect(w, HT_CLOSE), lx, ly)) return HT_CLOSE;
        if (has_max_btn(w) && rect_contains(title_btn_rect(w, HT_MAXIMIZE), lx, ly)) return HT_MAXIMIZE;
        if (has_min_btn(w) && rect_contains(title_btn_rect(w, HT_MINIMIZE), lx, ly)) return HT_MINIMIZE;
        return HT_TITLE;
    }
    if (lx >= w->cx && lx < w->cx + w->cw && ly >= w->cy && ly < w->cy + w->ch) return HT_CLIENT;
    return HT_BORDER;
}

static int cursor_for_ht(int ht) {
    switch (ht) {
    case HT_LEFT: case HT_RIGHT: return CURSOR_RESIZE_H;
    case HT_TOP: case HT_BOTTOM: return CURSOR_RESIZE_V;
    case HT_TOPLEFT: case HT_BOTTOMRIGHT: return CURSOR_RESIZE_NWSE;
    case HT_TOPRIGHT: case HT_BOTTOMLEFT: return CURSOR_RESIZE_NESW;
    default: return CURSOR_ARROW;
    }
}

/* ---- mouse handling --------------------------------------------------------------------- */
enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE };
static int drag_mode, drag_ht;
static int drag_off_x, drag_off_y, drag_start_x, drag_start_y;
static rect_t drag_start_frame;
static bool drag_restore_pending;
static rect_t pending_resize;
static bool have_pending_resize;
static uint32_t last_resize_tick;
static uint32_t last_click_tick;
static int last_click_x, last_click_y, last_click_btn;
static window_t *last_click_win;
static window_t *btn_win;          /* window whose title button is pressed */
static int acc_x, acc_y;           /* sub-pixel motion accumulators (1/16 px) */

static void set_cursor_shape(int shape) {
    if (shape == wm_cursor_shape) return;
    wm_invalidate_screen(cursor_rect(wm_mouse_x, wm_mouse_y, wm_cursor_shape));
    wm_cursor_shape = shape;
    wm_invalidate_screen(cursor_rect(wm_mouse_x, wm_mouse_y, wm_cursor_shape));
}

static void make_mouse_event(event_t *ev, int type, window_t *w) {
    memset(ev, 0, sizeof *ev);
    ev->type = type;
    ev->x = wm_mouse_x - w->frame.x - w->cx;
    ev->y = wm_mouse_y - w->frame.y - w->cy;
    ev->buttons = wm_mouse_buttons;
    ev->mods = input_mods();
}

static void set_hover_ht(window_t *w, int ht) {
    if (!w || !framed(w)) return;
    if (ht != HT_CLOSE && ht != HT_MAXIMIZE && ht != HT_MINIMIZE) ht = 0;
    if (w->hover_ht != ht) {
        w->hover_ht = ht;
        w->deco_dirty = true;
    }
}

static window_t *last_title_hover;

static void update_hover(void) {
    window_t *w = capture_win ? capture_win : wm_window_at(wm_mouse_x, wm_mouse_y);
    int ht = capture_win ? HT_CLIENT : hit_test(w, wm_mouse_x, wm_mouse_y);
    /* title-bar button hover */
    if (last_title_hover && last_title_hover != w && win_is_valid(last_title_hover)) {
        if (last_title_hover->hover_ht) { last_title_hover->hover_ht = 0; last_title_hover->deco_dirty = true; }
    }
    if (w && framed(w)) { set_hover_ht(w, ht); last_title_hover = w; }
    /* client enter/leave */
    window_t *client_w = (ht == HT_CLIENT) ? w : NULL;
    if (hover_win && hover_win != client_w && win_is_valid(hover_win)) {
        event_t ev;
        make_mouse_event(&ev, EV_MOUSE_LEAVE, hover_win);
        hover_win->mouse_inside = false;
        wm_send(hover_win, &ev);
    }
    hover_win = client_w;
    if (client_w) client_w->mouse_inside = true;
    /* cursor shape */
    int shape = CURSOR_ARROW;
    if (drag_mode == DRAG_RESIZE) shape = cursor_for_ht(drag_ht);
    else if (ht == HT_CLIENT && w) shape = w->widget_cursor ? w->widget_cursor : w->cursor;
    else shape = cursor_for_ht(ht);
    set_cursor_shape(shape);
}

static void do_drag_motion(void) {
    if (!drag_win || !win_is_valid(drag_win)) { drag_mode = DRAG_NONE; return; }
    window_t *w = drag_win;
    int work_h = scr_h - wm_taskbar_h;
    if (drag_mode == DRAG_MOVE) {
        if (drag_restore_pending) {
            if (abs(wm_mouse_x - drag_start_x) + abs(wm_mouse_y - drag_start_y) < 6) return;
            /* un-maximize while dragging: keep the cursor at the same relative title position */
            int rel = (wm_mouse_x * 1000) / MAX(1, w->frame.w);
            win_restore(w);
            drag_off_x = w->frame.w * rel / 1000;
            drag_off_y = MIN(drag_off_y, WIN_TITLE_H - 4);
            drag_restore_pending = false;
        }
        int nx = wm_mouse_x - drag_off_x, ny = wm_mouse_y - drag_off_y;
        ny = CLAMP(ny, 0, work_h - 24);
        nx = CLAMP(nx, -w->frame.w + 80, scr_w - 80);
        wm_set_frame(w, rect_make(nx, ny, w->frame.w, w->frame.h));
    } else if (drag_mode == DRAG_RESIZE) {
        int dx = wm_mouse_x - drag_start_x, dy = wm_mouse_y - drag_start_y;
        rect_t f = drag_start_frame;
        int minfw = w->min_cw + 2 * WIN_BORDER, minfh = w->min_ch + WIN_TITLE_H + WIN_BORDER;
        int ht = drag_ht;
        if (ht == HT_LEFT || ht == HT_TOPLEFT || ht == HT_BOTTOMLEFT) {
            int nw = MAX(minfw, f.w - dx);
            f.x += f.w - nw; f.w = nw;
        }
        if (ht == HT_RIGHT || ht == HT_TOPRIGHT || ht == HT_BOTTOMRIGHT) f.w = MAX(minfw, f.w + dx);
        if (ht == HT_TOP || ht == HT_TOPLEFT || ht == HT_TOPRIGHT) {
            int nh = MAX(minfh, f.h - dy);
            f.y += f.h - nh; f.h = nh;
        }
        if (ht == HT_BOTTOM || ht == HT_BOTTOMLEFT || ht == HT_BOTTOMRIGHT) f.h = MAX(minfh, f.h + dy);
        if (f.y < 0) { f.h += f.y; f.y = 0; }
        if (f.y + f.h > work_h) f.h = work_h - f.y;
        pending_resize = f;
        have_pending_resize = true;
    }
}

static void apply_pending_resize(bool force) {
    if (!have_pending_resize) return;
    if (!force && g_ticks - last_resize_tick < 33) return;
    have_pending_resize = false;
    last_resize_tick = g_ticks;
    window_t *w = drag_win;
    if (!w || !win_is_valid(w)) return;
    rect_t f = pending_resize;
    wm_invalidate_window(w);
    w->frame.x = f.x;
    w->frame.y = f.y;
    resize_client(w, f.w - 2 * WIN_BORDER, f.h - WIN_TITLE_H - WIN_BORDER);
    wm_invalidate_window(w);
}

static void mouse_motion(int dx, int dy) {
    if (!dx && !dy) return;
    int nx, ny;
    if (g_test_mouse) {
        nx = wm_mouse_x + dx;
        ny = wm_mouse_y + dy;
    } else {
        int mult = g_mouse_speed * 16 / 5;        /* 16 = 1.0x */
        int mag = abs(dx) + abs(dy);
        if (mag > 10) mult = mult * 3 / 2;        /* gentle acceleration */
        acc_x += dx * mult;
        acc_y += dy * mult;
        int mx = acc_x / 16, my = acc_y / 16;
        acc_x -= mx * 16;
        acc_y -= my * 16;
        nx = wm_mouse_x + mx;
        ny = wm_mouse_y + my;
    }
    nx = CLAMP(nx, 0, scr_w - 1);
    ny = CLAMP(ny, 0, scr_h - 1);
    if (nx == wm_mouse_x && ny == wm_mouse_y) return;
    wm_invalidate_screen(cursor_rect(wm_mouse_x, wm_mouse_y, wm_cursor_shape));
    wm_mouse_x = nx;
    wm_mouse_y = ny;
    wm_invalidate_screen(cursor_rect(wm_mouse_x, wm_mouse_y, wm_cursor_shape));

    if (drag_mode != DRAG_NONE) { do_drag_motion(); return; }
    update_hover();
    window_t *target = capture_win;
    if (!target) {
        window_t *w = wm_window_at(nx, ny);
        if (w && hit_test(w, nx, ny) == HT_CLIENT && !w->modal) target = w;
    }
    if (target) {
        event_t ev;
        make_mouse_event(&ev, EV_MOUSE_MOVE, target);
        wm_send(target, &ev);
        /* widgets may have changed the wanted cursor while handling the move */
        if (win_is_valid(target) && drag_mode == DRAG_NONE && hit_test(target, wm_mouse_x, wm_mouse_y) == HT_CLIENT)
            set_cursor_shape(target->widget_cursor ? target->widget_cursor : target->cursor);
    }
}

static void mouse_down(int button) {
    wm_click_serial++;
    window_t *w = wm_window_at(wm_mouse_x, wm_mouse_y);
    if (wm_any_popup() && (!w || !(w->flags & WIN_POPUP))) {
        wm_close_popups(NULL);
        if (w && !win_is_valid(w)) w = NULL;
    }
    if (!w) return;
    if (w->modal && win_is_valid(w->modal)) {
        wm_focus(w->modal);
        sys_beep(660, 40);
        return;
    }
    int ht = hit_test(w, wm_mouse_x, wm_mouse_y);
    if (w->layer == LAYER_NORMAL || w->layer == LAYER_DESKTOP || (w->layer == LAYER_TOPMOST && w != g_taskbar_win))
        wm_focus(w);
    else if (w->layer == LAYER_POPUP)
        wm_raise(w);

    bool dbl = false;
    if (button == last_click_btn && w == last_click_win && g_ticks - last_click_tick < (uint32_t)double_click_ms &&
        abs(wm_mouse_x - last_click_x) <= 4 && abs(wm_mouse_y - last_click_y) <= 4) {
        dbl = true;
        last_click_tick = 0;
    } else {
        last_click_tick = g_ticks;
        last_click_x = wm_mouse_x;
        last_click_y = wm_mouse_y;
        last_click_btn = button;
        last_click_win = w;
    }

    switch (ht) {
    case HT_CLIENT: {
        capture_win = w;
        event_t ev;
        make_mouse_event(&ev, EV_MOUSE_DOWN, w);
        ev.button = button;
        wm_send(w, &ev);
        if (dbl && win_is_valid(w)) {
            make_mouse_event(&ev, EV_DOUBLE_CLICK, w);
            ev.button = button;
            wm_send(w, &ev);
        }
        break;
    }
    case HT_TITLE:
        if (button != MOUSE_LEFT) break;
        if (dbl && has_max_btn(w)) {
            if (w->maximized) win_restore(w); else win_maximize(w);
            break;
        }
        drag_mode = DRAG_MOVE;
        drag_win = w;
        drag_off_x = wm_mouse_x - w->frame.x;
        drag_off_y = wm_mouse_y - w->frame.y;
        drag_start_x = wm_mouse_x;
        drag_start_y = wm_mouse_y;
        drag_start_frame = w->maximized ? w->normal_frame : w->frame;
        drag_restore_pending = w->maximized;
        break;
    case HT_CLOSE: case HT_MAXIMIZE: case HT_MINIMIZE:
        if (button != MOUSE_LEFT) break;
        w->pressed_ht = ht;
        w->hover_ht = ht;
        w->deco_dirty = true;
        btn_win = w;
        break;
    case HT_LEFT: case HT_RIGHT: case HT_TOP: case HT_BOTTOM:
    case HT_TOPLEFT: case HT_TOPRIGHT: case HT_BOTTOMLEFT: case HT_BOTTOMRIGHT:
        if (button != MOUSE_LEFT) break;
        drag_mode = DRAG_RESIZE;
        drag_ht = ht;
        drag_win = w;
        drag_start_x = wm_mouse_x;
        drag_start_y = wm_mouse_y;
        drag_start_frame = w->frame;
        break;
    default:
        break;
    }
}

static void mouse_up(int button) {
    if (drag_mode != DRAG_NONE && button == MOUSE_LEFT) {
        if (drag_mode == DRAG_RESIZE) apply_pending_resize(true);
        if (drag_mode == DRAG_MOVE && drag_win && win_is_valid(drag_win) && !drag_restore_pending &&
            (drag_win->flags & WIN_RESIZABLE)) {
            /* edge snapping */
            int work_h = scr_h - wm_taskbar_h;
            if (wm_mouse_y <= 0 && has_max_btn(drag_win)) {
                drag_win->normal_frame = drag_start_frame;
                win_maximize(drag_win);
                drag_win->normal_frame = drag_start_frame;
            } else if (wm_mouse_x <= 0 || wm_mouse_x >= scr_w - 1) {
                window_t *w = drag_win;
                rect_t f = rect_make(wm_mouse_x <= 0 ? 0 : scr_w / 2, 0, scr_w / 2, work_h);
                w->normal_frame = drag_start_frame;
                wm_invalidate_window(w);
                w->frame.x = f.x;
                w->frame.y = f.y;
                resize_client(w, f.w - 2 * WIN_BORDER, f.h - WIN_TITLE_H - WIN_BORDER);
                wm_invalidate_window(w);
            }
        }
        drag_mode = DRAG_NONE;
        drag_win = NULL;
        update_hover();
        return;
    }
    if (btn_win && button == MOUSE_LEFT) {
        window_t *w = btn_win;
        btn_win = NULL;
        if (win_is_valid(w)) {
            int ht = hit_test(w, wm_mouse_x, wm_mouse_y);
            int pressed = w->pressed_ht;
            w->pressed_ht = 0;
            w->deco_dirty = true;
            if (ht == pressed) {
                if (ht == HT_CLOSE) {
                    if (w->flags & WIN_CONFIRM_CLOSE) {
                        event_t ev = { .type = EV_CLOSE_REQUEST };
                        wm_send(w, &ev);
                    } else {
                        win_close(w);
                    }
                } else if (ht == HT_MAXIMIZE) {
                    if (w->maximized) win_restore(w); else win_maximize(w);
                } else if (ht == HT_MINIMIZE) {
                    win_minimize(w);
                }
            }
        }
        update_hover();
        return;
    }
    if (capture_win) {
        window_t *w = capture_win;
        if (!wm_mouse_buttons) capture_win = NULL;
        if (win_is_valid(w)) {
            event_t ev;
            make_mouse_event(&ev, EV_MOUSE_UP, w);
            ev.button = button;
            wm_send(w, &ev);
        }
        if (!capture_win) update_hover();
    }
}

static void mouse_wheel(int dz) {
    window_t *w = capture_win ? capture_win : wm_window_at(wm_mouse_x, wm_mouse_y);
    if (!w || w->modal) return;
    if (hit_test(w, wm_mouse_x, wm_mouse_y) != HT_CLIENT && w != capture_win) return;
    event_t ev;
    make_mouse_event(&ev, EV_MOUSE_WHEEL, w);
    ev.wheel = dz > 0 ? 1 : -1;
    int n = abs(dz);
    for (int i = 0; i < n && i < 4 && win_is_valid(w); i++) wm_send(w, &ev);
}

static void mouse_buttons_changed(int nb) {
    int old = wm_mouse_buttons;
    static const int order[3] = { MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE };
    for (int i = 0; i < 3; i++) {
        int b = order[i];
        if ((nb & b) && !(old & b)) { wm_mouse_buttons |= b; mouse_down(b); }
        else if (!(nb & b) && (old & b)) { wm_mouse_buttons &= ~b; mouse_up(b); }
    }
    wm_mouse_buttons = nb;
}

void input_mouse_pos(int *x, int *y) { if (x) *x = wm_mouse_x; if (y) *y = wm_mouse_y; }
int input_mouse_buttons(void) { return wm_mouse_buttons; }
void sys_set_double_click_ms(int ms) { double_click_ms = CLAMP(ms, 150, 1500); }
int sys_get_double_click_ms(void) { return double_click_ms; }

/* ---- keyboard ------------------------------------------------------------------------- */
static bool win_key_alone;

static void alt_tab(void) {
    /* bring the bottom-most normal window to the front (cycles through all) */
    for (window_t *w = wm_bottom; w; w = w->above) {
        if (w->layer != LAYER_NORMAL || w->closing || !w->visible || (w->flags & WIN_NO_TASKBAR)) continue;
        if (w == wm_focused) continue;
        win_focus(w);
        return;
    }
}

static void key_event(const input_event_t *ie) {
    int mods = ie->mods;
    if (ie->pressed) {
        if (ie->key == KEY_LWIN || ie->key == KEY_RWIN) {
            if (!ie->repeat) win_key_alone = true;
        } else {
            win_key_alone = false;
        }
        if ((mods & MOD_ALT) && ie->key == KEY_F4) {
            window_t *w = wm_focused;
            if (w && w->layer == LAYER_NORMAL) {
                if (w->flags & WIN_CONFIRM_CLOSE) { event_t ev = { .type = EV_CLOSE_REQUEST }; wm_send(w, &ev); }
                else win_close(w);
            }
            return;
        }
        if ((mods & MOD_ALT) && ie->key == KEY_TAB) { wm_close_popups(NULL); alt_tab(); return; }
        if ((mods & MOD_CTRL) && ie->key == KEY_ESCAPE) { startmenu_toggle(); return; }
        if ((mods & MOD_CTRL) && (mods & MOD_ALT) && ie->key == KEY_DELETE) {
            if (!app_launch_by_name("Task Manager", NULL)) app_launch_by_name("System Monitor", NULL);
            return;
        }
        if (ie->key == KEY_PRINTSCREEN) { wm_screenshot(); return; }
    } else {
        if ((ie->key == KEY_LWIN || ie->key == KEY_RWIN) && win_key_alone) {
            win_key_alone = false;
            startmenu_toggle();
            return;
        }
    }
    window_t *target = top_popup();
    if (!target) target = wm_focused;
    if (!target) return;
    event_t ev = { .type = ie->pressed ? EV_KEY_DOWN : EV_KEY_UP, .key = ie->key, .mods = mods, .repeat = ie->repeat };
    wm_send(target, &ev);
    if (ie->pressed && ie->ch && !(mods & (MOD_CTRL | MOD_ALT)) && win_is_valid(target)) {
        event_t cev = { .type = EV_CHAR, .ch = ie->ch, .mods = mods, .key = ie->key };
        wm_send(target, &cev);
    }
}

static void process_input(void) {
    input_event_t ev;
    int adx = 0, ady = 0;
    while (input_pop(&ev)) {
        if (ev.type == IN_MOUSE) {
            if (ev.buttons == wm_mouse_buttons && ev.dz == 0) {
                adx += ev.dx;
                ady += ev.dy;
                continue;
            }
            mouse_motion(adx + ev.dx, ady + ev.dy);
            adx = ady = 0;
            if (ev.dz) mouse_wheel(ev.dz);
            if (ev.buttons != wm_mouse_buttons) mouse_buttons_changed(ev.buttons);
        } else if (ev.type == IN_KEY) {
            if (adx || ady) { mouse_motion(adx, ady); adx = ady = 0; }
            key_event(&ev);
        }
    }
    if (adx || ady) mouse_motion(adx, ady);
}

/* ---- painting -------------------------------------------------------------------------- */
static void paint_window(window_t *w) {
    if (w->deco_dirty) wm_redraw_decor(w);
    if (!w->need_paint) return;
    w->need_paint = false;
    rect_t pr = rect_intersect(w->paint_rect, rect_make(0, 0, w->cw, w->ch));
    if (rect_empty(pr)) return;
    surface_t *s = &w->client;
    surface_set_clip(s, pr);
    gfx_fill_rect(s, pr.x, pr.y, pr.w, pr.h, theme_get()->window_bg);
    event_t ev = { .type = EV_PAINT, .x = pr.x, .y = pr.y, .w = pr.w, .h = pr.h };
    const app_desc_t *saved = wm_current_app;
    wm_current_app = w->app;
    if (w->handler) w->handler(w, &ev);
    if (!w->closing) {
        surface_set_clip(s, pr);
        ui_paint_all(w);
    }
    wm_current_app = saved;
    surface_reset_clip(s);
    if (framed(w) && w->radius && pr.y + pr.h >= w->ch - w->radius) draw_border(w, true);
    wm_invalidate_client_rect(w, pr);
}

#define FADE_MS 140

/* Advance fade-in animations; returns true while any window is still fading. */
static bool animate(void) {
    bool any = false;
    for (window_t *w = wm_bottom; w; w = w->above) {
        if (w->opacity >= 255 || !drawn(w)) continue;
        uint32_t el = g_ticks - w->fade_start;
        int op = el >= FADE_MS ? 255 : (int)(el * 255 / FADE_MS);
        op = op < 40 ? 40 : op;   /* start visible enough that first frames are not empty */
        if (op != w->opacity) {
            w->opacity = op;
            wm_invalidate_window(w);
        }
        if (w->opacity < 255) any = true;
    }
    return any;
}

static void paint_all(void) {
    for (window_t *w = wm_bottom; w; w = w->above) {
        if (!drawn(w)) continue;
        if (w->need_paint || w->deco_dirty) paint_window(w);
    }
    if (btn_win && !win_is_valid(btn_win)) btn_win = NULL;
}

/* ---- screenshot (PrintScreen) ---------------------------------------------------------- */
bool wm_screenshot(void) {
    surface_t *s = screen_capture();
    if (!s) return false;
    size_t size;
    void *bmp = image_encode_bmp(s, &size);
    surface_destroy(s);
    if (!bmp) return false;
    char path[VFS_PATH_MAX];
    datetime_t dt;
    sys_datetime(&dt);
    vfs_mkdirs("/home/user/Pictures");
    snprintf(path, sizeof path, "/home/user/Pictures/Screenshot %04d-%02d-%02d %02d%02d%02d.bmp",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    int r = vfs_write_file(path, bmp, (uint32_t)size);
    kfree(bmp);
    if (r == 0) {
        ui_notify("Screenshot saved", vfs_basename(path));
        klog("wm: screenshot %s", path);
    }
    return r == 0;
}

/* ---- init + main loop ------------------------------------------------------------------- */
void wm_init(void) {
    scr_w = g_fb.width;
    scr_h = g_fb.height;
    wm_backbuf = surface_create(scr_w, scr_h);
    if (!wm_backbuf) kpanic("wm: cannot allocate back buffer");
    build_corner_masks();
    wm_mouse_x = scr_w / 2;
    wm_mouse_y = scr_h / 2;
    if (g_test_mouse) { wm_mouse_x = 0; wm_mouse_y = 0; }
    klog("wm: %dx%d", scr_w, scr_h);
}

void wm_run(void) {
    klog("wm: entering main loop");
    for (;;) {
        stack_guard_check();
        process_input();
        timers_run();
        apply_pending_resize(false);
        deliver_posted();
        destroy_closed();
        vfs_tick();
        if (taskbar_dirty) { taskbar_dirty = false; taskbar_refresh(); }
        paint_all();
        destroy_closed();
        bool fading = animate();
        composite();
        /* idle: sleep until the next interrupt unless work is pending */
        __asm__ volatile("cli");
        bool busy = input_pending() || ndirty || (post_head != post_tail) || have_pending_resize || fading;
        if (!busy) {
            for (window_t *w = wm_bottom; w; w = w->above)
                if (drawn(w) && (w->need_paint || w->deco_dirty)) { busy = true; break; }
        }
        if (!busy) {
            uint32_t due = timers_next_due();
            if (due && (int32_t)(due - g_ticks) <= 0) busy = true;
        }
        if (busy) __asm__ volatile("sti");
        else __asm__ volatile("sti; hlt");
    }
}
