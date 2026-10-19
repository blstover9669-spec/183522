/*
 * arcade_common.h - small helpers shared by the arcade games (Snake, Tetris,
 * 2048): a per-window random number generator, persistent high scores,
 * themed score cards, custom-drawn push buttons and overlay cards.
 *
 * Everything is static inline so each game can include it without unused
 * function warnings. Games draw their own buttons (instead of using button
 * widgets) so that no widget ever takes keyboard focus away from the game -
 * arrows, Space and Enter always reach the game's event handler.
 */
#ifndef ARCADE_COMMON_H
#define ARCADE_COMMON_H

#include <nova.h>

#define ARCADE_SCORES_PATH "/home/user/.arcade_scores"

/* ---- random numbers (xorshift32, one generator per window) ------------------------ */
typedef struct { uint32_t s; } arcade_rng_t;

static inline void arcade_rng_seed(arcade_rng_t *r, uint32_t extra) {
    uint32_t s = (sys_uptime_ms() + 1) * 2654435761u;
    s ^= sys_time() * 40503u;
    s ^= extra * 2246822519u;
    r->s = s ? s : 0x9E3779B9u;
}

static inline uint32_t arcade_rand(arcade_rng_t *r) {
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

/* 0 .. n-1 */
static inline int arcade_rand_n(arcade_rng_t *r, int n) {
    return n > 0 ? (int)(arcade_rand(r) % (uint32_t)n) : 0;
}

/* ---- high scores: "key=value" lines in ~/.arcade_scores ---------------------------- */
static inline int arcade_hiscore_get(const char *key) {
    uint32_t size = 0;
    char *d = vfs_read_file(ARCADE_SCORES_PATH, &size);
    if (!d) return 0;
    int klen = (int)strlen(key), val = 0;
    char *p = d;
    while (p && *p) {
        if (!strncmp(p, key, (size_t)klen) && p[klen] == '=') { val = atoi(p + klen + 1); break; }
        p = strchr(p, '\n');
        if (p) p++;
    }
    free(d);
    return val < 0 ? 0 : val;
}

static inline void arcade_hiscore_set(const char *key, int value) {
    uint32_t size = 0;
    char *d = vfs_read_file(ARCADE_SCORES_PATH, &size);
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
    vfs_write_file(ARCADE_SCORES_PATH, out, (uint32_t)n);
    free(out);
}

/* ---- colours ---------------------------------------------------------------------------- */
static inline uint32_t arcade_muted(void) {
    const theme_t *t = theme_get();
    return color_mix(t->window_text, t->panel_bg, 115);
}

/* ---- score card: small caps label + big value on a themed panel ---------------------------- */
static inline void arcade_card(surface_t *s, int x, int y, int w, int h, const char *label,
                               const char *value, const font_t *vfont, uint32_t vcolor) {
    const theme_t *t = theme_get();
    ui_draw_panel(s, rect_make(x, y, w, h));
    gfx_text(s, x + 12, y + 8, label, arcade_muted(), FONT_UI_BOLD);
    int vy = y + 8 + font_height(FONT_UI_BOLD) + (h - 16 - font_height(FONT_UI_BOLD) - font_height(vfont)) / 2;
    gfx_text_ellipsis(s, x + 12, vy, w - 20, value, vcolor ? vcolor : t->window_text, vfont);
}

/* ---- custom-drawn buttons ------------------------------------------------------------------------ */
typedef struct {
    rect_t r;
    const char *label;
    bool primary;    /* accent coloured */
    bool visible;
    bool disabled;   /* greyed out, ignores clicks */
} arcade_btn_t;

/* hover / pressed: index of the hovered / pressed button or -1 */
static inline void arcade_btn_draw(surface_t *s, const arcade_btn_t *b, int idx, int hover, int pressed) {
    if (!b->visible) return;
    int st = 0;
    if (b->primary) st |= UI_STATE_DEFAULT;
    if (b->disabled) st |= UI_STATE_DISABLED;
    else if (hover == idx) st |= UI_STATE_HOVER;
    if (!b->disabled && pressed == idx && hover == idx) st |= UI_STATE_PRESSED;
    ui_draw_button(s, b->r, b->label, st);
}

static inline int arcade_btn_hit(const arcade_btn_t *b, int n, int x, int y) {
    for (int i = 0; i < n; i++)
        if (b[i].visible && !b[i].disabled && rect_contains(b[i].r, x, y)) return i;
    return -1;
}

/* ---- overlay card centred in `area`: dims the area, then a panel with a title and two lines --- */
static inline rect_t arcade_overlay(surface_t *s, rect_t area, int dim_alpha, int card_w, int card_h,
                                    const char *title, uint32_t title_color,
                                    const char *line1, const char *line2) {
    const theme_t *t = theme_get();
    if (dim_alpha > 0) gfx_fill_rect(s, area.x, area.y, area.w, area.h, RGBA(8, 10, 16, dim_alpha));
    rect_t c = rect_make(area.x + (area.w - card_w) / 2, area.y + (area.h - card_h) / 2, card_w, card_h);
    gfx_shadow(s, c.x, c.y, c.w, c.h, 12, 10, 70);
    ui_draw_panel(s, c);
    int y = c.y + 18;
    gfx_text_aligned(s, rect_make(c.x, y, c.w, font_height(FONT_UI_TITLE)), title,
                     title_color ? title_color : t->window_text, FONT_UI_TITLE, ALIGN_CENTER | ALIGN_MIDDLE);
    y += font_height(FONT_UI_TITLE) + 8;
    if (line1 && *line1) {
        gfx_text_aligned(s, rect_make(c.x + 8, y, c.w - 16, font_height(FONT_UI)), line1, t->window_text,
                         FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
        y += font_height(FONT_UI) + 4;
    }
    if (line2 && *line2)
        gfx_text_aligned(s, rect_make(c.x + 8, y, c.w - 16, font_height(FONT_UI)), line2, arcade_muted(),
                         FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
    return c;
}

/* Thick axis-aligned (or nearly) segment with round caps. Opaque colours only. */
static inline void arcade_capsule(surface_t *s, double x0, double y0, double x1, double y1, double w, uint32_t c) {
    double r = w / 2;
    if (fabs(y1 - y0) <= fabs(x1 - x0)) {
        double a = MIN(x0, x1), b = MAX(x0, x1), ym = (y0 + y1) / 2;
        int ix = (int)lround(a), iy = (int)lround(ym - r);
        gfx_fill_rect(s, ix, iy, (int)lround(b) - ix, (int)lround(ym + r) - iy, c);
    } else {
        double a = MIN(y0, y1), b = MAX(y0, y1), xm = (x0 + x1) / 2;
        int ix = (int)lround(xm - r), iy = (int)lround(a);
        gfx_fill_rect(s, ix, iy, (int)lround(xm + r) - ix, (int)lround(b) - iy, c);
    }
    gfx_fill_circle_aa(s, x0, y0, r, c);
    gfx_fill_circle_aa(s, x1, y1, r, c);
}

/* Short melodies (non-blocking, respect the taskbar mute toggle inside the kernel). */
static inline void arcade_sound_gameover(void) {
    static const note_t notes[] = { { 523, 90 }, { 0, 20 }, { 392, 90 }, { 0, 20 }, { 330, 90 }, { 0, 20 }, { 262, 220 } };
    sys_play_notes(notes, ARRAY_SIZE(notes));
}

static inline void arcade_sound_fanfare(void) {
    static const note_t notes[] = { { 523, 80 }, { 659, 80 }, { 784, 80 }, { 1047, 200 } };
    sys_play_notes(notes, ARRAY_SIZE(notes));
}

#endif
