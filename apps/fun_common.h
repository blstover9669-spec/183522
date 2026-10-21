/*
 * fun_common.h - small helpers shared by the apps in apps/fun_*.c
 * (Chess, Memory Match, Typing Tutor):
 *
 *   - a per-window xorshift random number generator;
 *   - persistent records ("key=value" lines in ~/.fun_scores);
 *   - custom-drawn push buttons and a segmented control. The apps draw these
 *     with the themed ui_draw_* helpers instead of using button widgets, so no
 *     widget ever takes the keyboard focus and every key reaches the app;
 *   - stat "cards", anti-aliased thick strokes and an overlay card for
 *     end-of-game messages.
 *
 * Everything is static inline so each app can include it without
 * unused-function warnings.
 */
#ifndef FUN_COMMON_H
#define FUN_COMMON_H

#include <nova.h>

#define FN_SCORES_PATH "/home/user/.fun_scores"

/* ---- random numbers ------------------------------------------------------------- */
typedef struct { uint32_t s; } fn_rng_t;

static inline void fn_rng_seed(fn_rng_t *r, uint32_t extra) {
    uint32_t s = (sys_uptime_ms() + 11u) * 2654435761u;
    s ^= sys_time() * 2246822519u;
    s ^= (extra + 0x27d4eb2fu) * 3266489917u;
    r->s = s ? s : 0x9E3779B9u;
}

static inline uint32_t fn_rand(fn_rng_t *r) {
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

/* 0 .. n-1 */
static inline int fn_rand_n(fn_rng_t *r, int n) {
    return n > 0 ? (int)(fn_rand(r) % (uint32_t)n) : 0;
}

/* ---- persistent records ------------------------------------------------------------ */
static inline int fn_score_get(const char *key, int def) {
    uint32_t size = 0;
    char *d = vfs_read_file(FN_SCORES_PATH, &size);
    if (!d) return def;
    int klen = (int)strlen(key), val = def;
    char *p = d;
    while (p && *p) {
        if (!strncmp(p, key, (size_t)klen) && p[klen] == '=') { val = atoi(p + klen + 1); break; }
        p = strchr(p, '\n');
        if (p) p++;
    }
    free(d);
    return val;
}

static inline void fn_score_set(const char *key, int value) {
    uint32_t size = 0;
    char *d = vfs_read_file(FN_SCORES_PATH, &size);
    int klen = (int)strlen(key);
    int cap = (int)size + klen + 32;
    char *out = malloc((size_t)cap);
    if (!out) { free(d); return; }
    int n = 0;
    if (d) {
        char *p = d;
        while (*p) {
            char *e = strchr(p, '\n');
            int len = e ? (int)(e - p) : (int)strlen(p);
            bool mine = len > klen && !strncmp(p, key, (size_t)klen) && p[klen] == '=';
            if (!mine && len > 0 && n + len + 1 < cap) {
                memcpy(out + n, p, (size_t)len);
                n += len;
                out[n++] = '\n';
            }
            p += len;
            if (*p) p++;
        }
        free(d);
    }
    n += snprintf(out + n, (size_t)(cap - n), "%s=%d\n", key, value);
    vfs_write_file(FN_SCORES_PATH, out, (uint32_t)n);
    free(out);
}

/* ---- custom-drawn buttons ---------------------------------------------------------- */
typedef struct {
    rect_t r;
    const char *label;
    bool hover, pressed;
    bool primary;    /* accent coloured */
    bool disabled;
    bool hidden;
} fn_btn_t;

static inline void fn_btn_draw(surface_t *s, const fn_btn_t *b) {
    if (b->hidden) return;
    int st = 0;
    if (b->primary) st |= UI_STATE_DEFAULT;
    if (b->disabled) st |= UI_STATE_DISABLED;
    else if (b->pressed && b->hover) st |= UI_STATE_PRESSED;
    else if (b->hover) st |= UI_STATE_HOVER;
    ui_draw_button(s, b->r, b->label, st);
}

static inline void fn_btn_inval(window_t *win, const fn_btn_t *b) {
    win_invalidate_rect(win, b->r.x - 3, b->r.y - 3, b->r.w + 6, b->r.h + 6);
}

/* Feed a mouse event to a set of buttons. Returns the index of the button that
   was clicked (pressed and released inside it), else -1. *consumed is set when
   the event belonged to a button (the app should then ignore it). */
static inline int fn_btns_event(window_t *win, fn_btn_t *b, int n, const event_t *ev, bool *consumed) {
    int fired = -1;
    bool used = false;
    for (int i = 0; i < n; i++) {
        fn_btn_t *k = &b[i];
        if (k->hidden || k->disabled) {
            if (k->hover || k->pressed) { k->hover = k->pressed = false; fn_btn_inval(win, k); }
            continue;
        }
        bool inside = rect_contains(k->r, ev->x, ev->y);
        switch (ev->type) {
        case EV_MOUSE_MOVE:
            if (inside != k->hover) { k->hover = inside; fn_btn_inval(win, k); }
            if (k->pressed) used = true;
            break;
        case EV_MOUSE_LEAVE:
            if (k->hover) { k->hover = false; fn_btn_inval(win, k); }
            break;
        case EV_MOUSE_DOWN:
            if (inside) {
                used = true;
                if (ev->button == MOUSE_LEFT) { k->pressed = true; k->hover = true; fn_btn_inval(win, k); }
            }
            break;
        case EV_DOUBLE_CLICK:
            if (inside) used = true;
            break;
        case EV_MOUSE_UP:
            if (k->pressed && ev->button == MOUSE_LEFT) {
                k->pressed = false;
                fn_btn_inval(win, k);
                used = true;
                if (inside) fired = i;
            }
            break;
        default:
            break;
        }
    }
    if (consumed) *consumed = used;
    return fired;
}

static inline void fn_btns_reset(window_t *win, fn_btn_t *b, int n) {
    for (int i = 0; i < n; i++)
        if (b[i].hover || b[i].pressed) { b[i].hover = b[i].pressed = false; fn_btn_inval(win, &b[i]); }
}

/* ---- segmented control ("Easy | Normal | Hard") ------------------------------------ */
static inline int fn_seg_hit(rect_t r, int n, int x, int y) {
    if (n <= 0 || !rect_contains(r, x, y)) return -1;
    int i = (x - r.x) * n / r.w;
    return CLAMP(i, 0, n - 1);
}

static inline void fn_seg_draw(surface_t *s, rect_t r, const char *const *labels, int n, int sel, int hover) {
    const theme_t *t = theme_get();
    uint32_t track = t->dark ? color_mix(t->window_bg, COLOR_BLACK, 70)
                             : color_mix(t->window_bg, t->window_border, 110);
    int rad = MIN(8, r.h / 2);
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, rad, track);
    for (int i = 0; i < n; i++) {
        int x0 = r.x + r.w * i / n, x1 = r.x + r.w * (i + 1) / n;
        rect_t c = rect_make(x0 + 3, r.y + 3, x1 - x0 - 6, r.h - 6);
        uint32_t fg;
        const font_t *f = FONT_UI;
        if (i == sel) {
            gfx_fill_rounded_rect(s, c.x, c.y + 1, c.w, c.h, rad - 2, RGBA(0, 0, 0, t->dark ? 90 : 34));
            gfx_fill_rounded_rect(s, c.x, c.y, c.w, c.h, rad - 2, t->dark ? color_shade(t->button_bg, 10) : t->button_bg);
            fg = t->window_text;
            f = FONT_UI_BOLD;
        } else {
            if (i == hover) gfx_fill_rounded_rect(s, c.x, c.y, c.w, c.h, rad - 2, color_alpha(t->window_text, 20));
            fg = color_mix(t->window_text, t->window_bg, 70);
        }
        gfx_text_aligned(s, c, labels[i], fg, f, ALIGN_CENTER | ALIGN_MIDDLE);
    }
}

/* ---- colours ---------------------------------------------------------------------- */
/* Muted text colour (secondary labels). */
static inline uint32_t fn_muted(void) {
    const theme_t *t = theme_get();
    return color_mix(t->window_text, t->window_bg, 100);
}

/* Card / tile background that stands out a little from the window background. */
static inline uint32_t fn_card_bg(void) {
    const theme_t *t = theme_get();
    return t->dark ? color_mix(t->panel_bg, COLOR_WHITE, 8) : t->panel_bg;
}

/* ---- stat card: a small label above a big value ------------------------------------ */
static inline void fn_stat_card(surface_t *s, rect_t r, const char *label, const char *value, uint32_t vcol,
                                bool highlight) {
    const theme_t *t = theme_get();
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 10, highlight ? color_mix(t->window_border, t->accent, 200)
                                                                : t->window_border);
    gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 9, fn_card_bg());
    if (highlight) gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 9, color_alpha(t->accent, t->dark ? 34 : 20));
    gfx_text_aligned(s, rect_make(r.x, r.y + 6, r.w, 17), label, fn_muted(), FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
    gfx_text_aligned(s, rect_make(r.x, r.y + 22, r.w, r.h - 26), value, vcol, FONT_UI_LARGE,
                     ALIGN_CENTER | ALIGN_MIDDLE);
}

/* ---- strokes ------------------------------------------------------------------------- */
/* Thick anti-aliased line with round caps. Use an opaque colour (overlapping parts
   would otherwise blend twice). */
static inline void fn_stroke(surface_t *s, double x0, double y0, double x1, double y1, double wdt, uint32_t col) {
    double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
    double r = wdt / 2;
    if (len > 0.01) {
        double nx = -dy / len * r, ny = dx / len * r;
        point_t p[4] = {
            { (int)lround(x0 + nx), (int)lround(y0 + ny) }, { (int)lround(x1 + nx), (int)lround(y1 + ny) },
            { (int)lround(x1 - nx), (int)lround(y1 - ny) }, { (int)lround(x0 - nx), (int)lround(y0 - ny) },
        };
        gfx_fill_polygon(s, p, 4, col);
        gfx_line_aa(s, x0 + nx, y0 + ny, x1 + nx, y1 + ny, col);
        gfx_line_aa(s, x0 - nx, y0 - ny, x1 - nx, y1 - ny, col);
    }
    gfx_fill_circle_aa(s, x0, y0, r, col);
    gfx_fill_circle_aa(s, x1, y1, r, col);
}

/* Arc (angles in radians, 0 = 3 o'clock, increasing clockwise on screen) drawn as a
   chain of round-capped strokes. */
static inline void fn_arc(surface_t *s, double cx, double cy, double rad, double a0, double a1,
                          double wdt, uint32_t col) {
    double span = a1 - a0;
    int n = (int)(fabs(span) * rad / 4) + 2;
    if (n > 200) n = 200;
    double px = cx + cos(a0) * rad, py = cy + sin(a0) * rad;
    for (int i = 1; i <= n; i++) {
        double a = a0 + span * i / n;
        double x = cx + cos(a) * rad, y = cy + sin(a) * rad;
        fn_stroke(s, px, py, x, y, wdt, col);
        px = x; py = y;
    }
}

/* ---- overlay card ------------------------------------------------------------------ */
/* Dims `area` and draws a centred rounded card of size w x h; returns the card rect. */
static inline rect_t fn_overlay_card(surface_t *s, rect_t area, int w, int h, int dim_alpha) {
    const theme_t *t = theme_get();
    if (dim_alpha > 0) gfx_fill_rect(s, area.x, area.y, area.w, area.h, RGBA(0, 0, 0, dim_alpha));
    rect_t c = rect_make(area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h);
    gfx_shadow(s, c.x, c.y, c.w, c.h, 12, 14, t->dark ? 150 : 90);
    gfx_fill_rounded_rect(s, c.x, c.y, c.w, c.h, 12, color_mix(t->panel_bg, t->window_border, 60));
    gfx_fill_rounded_rect(s, c.x + 1, c.y + 1, c.w - 2, c.h - 2, 11, t->panel_bg);
    return c;
}

/* Centred single text line. */
static inline void fn_text_c(surface_t *s, int x, int y, int w, const char *str, uint32_t col, const font_t *f) {
    gfx_text_aligned(s, rect_make(x, y, w, font_height(f)), str, col, f, ALIGN_CENTER);
}

/* Five-pointed star centred at (cx, cy), outer radius r. */
static inline void fn_star(surface_t *s, double cx, double cy, double r, double inner, uint32_t col) {
    point_t p[10];
    for (int i = 0; i < 10; i++) {
        double a = -M_PI / 2 + i * M_PI / 5;
        double rr = (i & 1) ? r * inner : r;
        p[i].x = (int)lround(cx + cos(a) * rr);
        p[i].y = (int)lround(cy + sin(a) * rr);
    }
    gfx_fill_polygon(s, p, 10, col);
    for (int i = 0; i < 10; i++) {
        int j = (i + 1) % 10;
        double a0 = -M_PI / 2 + i * M_PI / 5, a1 = -M_PI / 2 + j * M_PI / 5;
        double r0 = (i & 1) ? r * inner : r, r1 = (j & 1) ? r * inner : r;
        gfx_line_aa(s, cx + cos(a0) * r0, cy + sin(a0) * r0, cx + cos(a1) * r1, cy + sin(a1) * r1, col);
    }
}

/* "m:ss" (or "h:mm:ss") from milliseconds. */
static inline void fn_fmt_time(char *buf, int size, uint32_t ms) {
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(buf, (size_t)size, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(buf, (size_t)size, "%u:%02u", s / 60, s % 60);
}

static inline double fn_ease_out(double t) {
    t = CLAMP(t, 0.0, 1.0);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

#endif
