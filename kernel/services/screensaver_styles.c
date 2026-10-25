/*
 * screensaver_styles.c - the screensaver animations (see nova/screensaver.h).
 *
 * Every style draws into a surface of any size: the full-screen screensaver
 * window, or the small live preview in Settings > Styles (sizes then scale
 * with k = height / 768).
 *
 * Only what changes is redrawn: Nova Drop, Bubbles and Bouncing App Logos are
 * sprite scenes over a pre-rendered background - each frame restores the
 * background under a sprite's old and new place and redraws the sprites
 * there; Lines draws each new pipe segment into a canvas and copies just that
 * piece. Only the Original redraws the whole screen (it always did).
 */
#include "services/svc_internal.h"
#include <nova/screensaver.h>

#define MAX_MARK 64

typedef struct style_ops {
    const char *name, *key;
    int frame_ms;
    bool (*init)(ss_anim_t *a);
    void (*fini)(ss_anim_t *a);
    void (*frame)(ss_anim_t *a, surface_t *s, bool full);
} style_ops_t;

struct ss_anim {
    const style_ops_t *ops;
    int style, w, h;
    double k;              /* scale: 1 = a 768-pixel-tall screen                 */
    bool preview;
    uint32_t t0, last;
    double t, dt;          /* seconds since the start / since the last frame    */
    rect_t mark[MAX_MARK];
    int nmark;
    bool all;
    void *p;               /* the style's state                                  */
};

static double frand(double lo, double hi) { return lo + (hi - lo) * (rand() % 10001) / 10000.0; }

static void mark(ss_anim_t *a, rect_t r) {
    r = rect_intersect(r, rect_make(0, 0, a->w, a->h));
    if (rect_empty(r) || a->all) return;
    if (a->nmark < MAX_MARK) a->mark[a->nmark++] = r;
    else a->mark[MAX_MARK - 1] = rect_union(a->mark[MAX_MARK - 1], r);
}

static void copy_rect(surface_t *dst, const surface_t *src, rect_t r) {
    r = rect_intersect(r, rect_make(0, 0, MIN(dst->w, src->w), MIN(dst->h, src->h)));
    if (rect_empty(r)) return;
    for (int y = r.y; y < r.y + r.h; y++)
        memcpy(surface_row(dst, y) + r.x, surface_row((surface_t *)src, y) + r.x, (size_t)r.w * 4);
}

/* ======================================================================================
 * 2. Original: 3D stars flying out of an aurora-lit sky, with a drifting clock
 *    (the NovaOS 1.5 screensaver; at full screen k = 1 and it draws exactly as before)
 * ==================================================================================== */
#define NSTARS 260

typedef struct { double x, y, z; } star_t;
typedef struct {
    star_t stars[NSTARS];
    int nstars;
    surface_t *sky;
    int clock_x, clock_y;
    uint32_t clock_moved;
} orig_t;

static void reset_star(star_t *s, bool anywhere) {
    s->x = (rand() % 2000 - 1000) / 1000.0;
    s->y = (rand() % 2000 - 1000) / 1000.0;
    s->z = anywhere ? 0.05 + (rand() % 950) / 1000.0 : 1.0;
}

static void build_sky(orig_t *o, int w, int h, double k) {
    surface_destroy(o->sky);
    o->sky = surface_create(w, h);
    if (!o->sky) return;
    gfx_gradient_v(o->sky, 0, 0, w, h * 2 / 3, RGB(4, 6, 20), RGB(12, 16, 44));
    gfx_gradient_v(o->sky, 0, h * 2 / 3, w, h - h * 2 / 3, RGB(12, 16, 44), RGB(26, 14, 48));
    /* faint fixed background stars */
    int n = k >= 1 ? 400 : MAX(30, (int)(400 * k * k * 1.6));
    for (int i = 0; i < n; i++) {
        int x = rand() % w, y = rand() % h, b = 40 + rand() % 90;
        gfx_pixel(o->sky, x, y, RGB(b, b, b + 20));
    }
}

static inline uint32_t blend(uint32_t d, uint32_t c, int a) {
    int ia = 255 - a;
    return 0xFF000000u | (uint32_t)((COLOR_R(c) * a + COLOR_R(d) * ia) / 255) << 16 |
           (uint32_t)((COLOR_G(c) * a + COLOR_G(d) * ia) / 255) << 8 | (uint32_t)((COLOR_B(c) * a + COLOR_B(d) * ia) / 255);
}

/* Aurora: translucent vertical curtains whose top edge and height wave over time. */
static void draw_aurora(surface_t *s, double t, double k) {
    int w = s->w, h = s->h;
    for (int x = 0; x < w; x += 2) {
        double fx = x / k;
        double top = h * 0.22 + 40 * k * sin(fx * 0.0042 + t * 0.21) + 18 * k * sin(fx * 0.011 - t * 0.37);
        double len = k * (150 + 60 * sin(fx * 0.006 + t * 0.29) + 30 * sin(fx * 0.017 + t * 0.5));
        double inten = 0.55 + 0.45 * sin(fx * 0.009 - t * 0.33) * sin(fx * 0.0023 + t * 0.12);
        if (inten < 0.05) continue;
        int y0 = (int)top, y1 = (int)(top + len);
        y0 = CLAMP(y0, 0, h - 1);
        y1 = CLAMP(y1, 0, h - 1);
        for (int y = y0; y < y1; y++) {
            double f = (y - top) / len;                    /* 0 at the top edge */
            double a = (f < 0.12 ? f / 0.12 : 1.0 - (f - 0.12) / 0.88);
            a = a * a * inten * 150;
            if (a < 3) continue;
            uint32_t c = color_mix(RGB(90, 255, 170), RGB(150, 90, 255), (int)(CLAMP(f * 1.3, 0.0, 1.0) * 255));
            uint32_t *row = surface_row(s, y);
            row[x] = blend(row[x], c, (int)a);
            if (x + 1 < w) row[x + 1] = blend(row[x + 1], c, (int)a);
        }
    }
}

static bool orig_init(ss_anim_t *a) {
    orig_t *o = kcalloc(1, sizeof *o);
    if (!o) return false;
    o->nstars = a->preview ? MAX(60, (int)(NSTARS * a->k * 1.2)) : NSTARS;
    o->nstars = MIN(o->nstars, NSTARS);
    for (int i = 0; i < o->nstars; i++) reset_star(&o->stars[i], true);
    a->p = o;
    return true;
}

static void orig_fini(ss_anim_t *a) {
    orig_t *o = a->p;
    surface_destroy(o->sky);
    kfree(o);
}

static void orig_frame(ss_anim_t *a, surface_t *s, bool full) {
    (void)full;
    orig_t *o = a->p;
    double k = a->k;
    if (!o->sky || o->sky->w != s->w || o->sky->h != s->h) build_sky(o, s->w, s->h, k);
    uint32_t now = a->last;
    double dt = a->dt, t = a->t;
    if (o->sky) for (int y = 0; y < s->h; y++) memcpy(surface_row(s, y), surface_row(o->sky, y), (size_t)s->w * 4);
    draw_aurora(s, t, k);
    /* starfield */
    double cx = s->w / 2.0, cy = s->h / 2.0;
    double rs = a->preview ? MAX(k * 1.8, 0.45) : 1.0;
    for (int i = 0; i < o->nstars; i++) {
        star_t *st = &o->stars[i];
        st->z -= dt * 0.22;
        if (st->z <= 0.04) reset_star(st, false);
        double sx = cx + st->x / st->z * cx * 0.5, sy = cy + st->y / st->z * cy * 0.5;
        if (sx < -4 || sy < -4 || sx > s->w + 4 || sy > s->h + 4) { reset_star(st, false); continue; }
        double kk = 1.0 - st->z;
        int b = (int)(90 + 165 * kk);
        double r = (0.6 + 1.9 * kk * kk) * rs;
        gfx_fill_circle_aa(s, sx, sy, r, RGB(b, b, MIN(255, b + 25)));
        if (kk > 0.7) gfx_fill_circle_aa(s, sx, sy, r * 2.6, RGBA(160, 190, 255, (int)(40 * kk)));
    }
    datetime_t d;
    sys_datetime(&d);
    char tbuf[16], dbuf[48];
    snprintf(tbuf, sizeof tbuf, "%02d:%02d", d.hour, d.minute);
    snprintf(dbuf, sizeof dbuf, "%s, %d %s", sys_weekday_name(d.weekday), d.day, sys_month_name(d.month));
    /* drifting clock, moved every 12 s to avoid burn-in */
    if (!o->clock_moved || now - o->clock_moved > 12000) {
        if (a->preview) {           /* keep the (smaller) date inside the small preview */
            int m = MAX(4, (int)(80 * k)), tw = font_text_width(FONT_UI, dbuf) + 2;
            o->clock_x = m + rand() % MAX(1, s->w - 2 * m - tw);
            o->clock_y = m + rand() % MAX(1, s->h - 2 * m - 42);
        } else {
            o->clock_x = (int)(80 * k) + rand() % MAX(1, s->w - (int)(460 * k));
            o->clock_y = (int)(80 * k) + rand() % MAX(1, s->h - (int)(300 * k));
        }
        o->clock_moved = now ? now : 1;
    }
    int fade = (int)MIN(255u, (now - o->clock_moved) * 255 / 900);
    uint32_t left = 12000 - MIN(12000u, now - o->clock_moved);
    if (left < 900) fade = (int)(left * 255 / 900);
    if (a->preview) {
        gfx_text(s, o->clock_x, o->clock_y, tbuf, RGBA(240, 244, 255, fade), FONT_UI_LARGE);
        gfx_text(s, o->clock_x + 1, o->clock_y + 22, dbuf, RGBA(190, 200, 235, fade * 3 / 4), FONT_UI);
    } else {
        gfx_text(s, o->clock_x, o->clock_y, tbuf, RGBA(240, 244, 255, fade), FONT_UI_HUGE);
        gfx_text(s, o->clock_x + 4, o->clock_y + 70, dbuf, RGBA(190, 200, 235, fade * 3 / 4), FONT_UI_LARGE);
    }
    a->all = true;
}

/* ======================================================================================
 * Sprite scenes (Nova Drop, Bubbles, Bouncing App Logos)
 * ==================================================================================== */
#define MAX_SPR 40

typedef struct { const surface_t *img; int x, y, op; } spr_t;
typedef struct {
    surface_t *bg;               /* background, same size as the target */
    spr_t spr[MAX_SPR];          /* this frame, drawn in order          */
    int n;
    rect_t prev[MAX_SPR];        /* where each sprite was last frame    */
    int nprev;
} scene_t;

static rect_t spr_rect(const spr_t *s) {
    return s->img && s->op > 0 ? rect_make(s->x, s->y, s->img->w, s->img->h) : rect_make(0, 0, 0, 0);
}

/* Background + every sprite, clipped to r. */
static void scene_draw_rect(ss_anim_t *a, scene_t *sc, surface_t *dst, rect_t r) {
    r = rect_intersect(r, rect_make(0, 0, a->w, a->h));
    if (rect_empty(r)) return;
    copy_rect(dst, sc->bg, r);
    rect_t old = dst->clip;
    dst->clip = rect_intersect(old, r);
    for (int i = 0; i < sc->n; i++) {
        rect_t sr = spr_rect(&sc->spr[i]);
        if (!rect_empty(sr) && rect_overlaps(sr, r)) gfx_blit_alpha(dst, sc->spr[i].x, sc->spr[i].y, sc->spr[i].img, sc->spr[i].op);
    }
    dst->clip = old;
    mark(a, r);
}

static void scene_render(ss_anim_t *a, scene_t *sc, surface_t *dst, bool full) {
    if (full) {
        scene_draw_rect(a, sc, dst, rect_make(0, 0, a->w, a->h));
        a->all = true;
    } else {
        int m = MAX(sc->n, sc->nprev);
        for (int i = 0; i < m; i++) {
            rect_t o = i < sc->nprev ? sc->prev[i] : rect_make(0, 0, 0, 0);
            rect_t nw = i < sc->n ? spr_rect(&sc->spr[i]) : rect_make(0, 0, 0, 0);
            bool eo = rect_empty(o), en = rect_empty(nw);
            if (eo && en) continue;
            if (!eo && !en && o.x == nw.x && o.y == nw.y && o.w == nw.w && o.h == nw.h && i < sc->nprev) {
                /* did not move: only redraw when its opacity may have changed (cheap enough to always do) */
                scene_draw_rect(a, sc, dst, nw);
            } else if (!eo && !en && rect_overlaps(rect_inset(o, -2), nw)) {
                scene_draw_rect(a, sc, dst, rect_union(o, nw));
            } else {
                if (!eo) scene_draw_rect(a, sc, dst, o);
                if (!en) scene_draw_rect(a, sc, dst, nw);
            }
        }
    }
    for (int i = 0; i < sc->n; i++) sc->prev[i] = spr_rect(&sc->spr[i]);
    sc->nprev = sc->n;
}

static surface_t *make_bg(int w, int h, uint32_t top, uint32_t bottom) {
    surface_t *bg = surface_create(w, h);
    if (bg) gfx_gradient_v(bg, 0, 0, w, h, top, bottom);
    return bg;
}

/* ======================================================================================
 * 1. Nova Drop: the Nova icon appears at the top centre and bounces down, each bounce
 *    smaller; after 4 s it disappears, then reappears and does it again
 * ==================================================================================== */
#define DROP_SHOW   4.0      /* seconds visible            */
#define DROP_FADE   0.35     /* fade out                   */
#define DROP_CYCLE  5.0      /* then reappears             */
#define DROP_FALL   0.85     /* first fall, seconds        */
#define DROP_BOUNCE 0.6      /* each bounce keeps 60 % speed */

typedef struct {
    scene_t sc;
    surface_t *logo, *shadow;
    int L;
    double top, floor_y, g, y, v, ct;
} drop_t;

static surface_t *make_shadow(int w, int h) {
    surface_t *s = surface_create_alpha(MAX(w, 2), MAX(h, 2));
    if (!s) return NULL;
    for (int y = 0; y < s->h; y++) {
        uint32_t *row = surface_row(s, y);
        for (int x = 0; x < s->w; x++) {
            double dx = (x + 0.5) / s->w * 2 - 1, dy = (y + 0.5) / s->h * 2 - 1;
            double d = dx * dx + dy * dy;
            if (d >= 1) continue;
            double f = (1 - d) * (1 - d);
            row[x] = (uint32_t)(f * 150) << 24;
        }
    }
    return s;
}

static void drop_reset(drop_t *d) {
    d->ct = 0;
    d->y = d->top;
    d->v = 0;
}

static bool drop_init(ss_anim_t *a) {
    drop_t *d = kcalloc(1, sizeof *d);
    if (!d) return false;
    a->p = d;
    d->L = MAX(16, (int)lround(150 * a->k));
    d->logo = surface_create_alpha(d->L, d->L);
    d->shadow = make_shadow(d->L * 8 / 10, MAX(4, d->L * 15 / 100));
    d->sc.bg = make_bg(a->w, a->h, RGB(5, 7, 20), RGB(16, 18, 46));
    if (!d->logo || !d->shadow || !d->sc.bg) return false;
    ui_draw_logo(d->logo, 0, 0, d->L);
    d->top = a->h * 0.06;
    d->floor_y = a->h * 0.86 - d->L;
    double dist = MAX(1.0, d->floor_y - d->top);
    d->g = 2 * dist / (DROP_FALL * DROP_FALL);
    drop_reset(d);
    return true;
}

static void drop_fini(ss_anim_t *a) {
    drop_t *d = a->p;
    surface_destroy(d->logo);
    surface_destroy(d->shadow);
    surface_destroy(d->sc.bg);
    kfree(d);
}

static void drop_frame(ss_anim_t *a, surface_t *s, bool full) {
    drop_t *d = a->p;
    d->ct += a->dt;
    if (d->ct >= DROP_CYCLE) drop_reset(d);
    /* physics in small steps: gravity, bounce on the floor losing speed */
    double rem = d->ct < DROP_SHOW + DROP_FADE ? a->dt : 0;
    while (rem > 0) {
        double h = MIN(rem, 0.004);
        d->v += d->g * h;
        d->y += d->v * h;
        if (d->y >= d->floor_y) {
            d->y = d->floor_y;
            d->v = -d->v * DROP_BOUNCE;
            if (fabs(d->v) < d->g * 0.03) d->v = 0;
        }
        rem -= h;
    }
    int op = 255;
    if (d->ct < 0.2) op = (int)(255 * d->ct / 0.2);
    else if (d->ct >= DROP_SHOW) op = d->ct >= DROP_SHOW + DROP_FADE ? 0 : (int)(255 * (1 - (d->ct - DROP_SHOW) / DROP_FADE));
    double near = 1 - (d->floor_y - d->y) / MAX(1.0, d->floor_y - d->top);   /* 1 on the floor */
    scene_t *sc = &d->sc;
    sc->n = 2;
    sc->spr[0].img = d->shadow;
    sc->spr[0].x = (a->w - d->shadow->w) / 2;
    sc->spr[0].y = (int)lround(d->floor_y + d->L * 0.97 - d->shadow->h / 2.0);
    sc->spr[0].op = (int)((30 + 225 * near * near) * op / 255);
    sc->spr[1].img = d->logo;
    sc->spr[1].x = (a->w - d->L) / 2;
    sc->spr[1].y = (int)lround(d->y);
    sc->spr[1].op = op;
    scene_render(a, sc, s, full);
}

/* ======================================================================================
 * 3. Bubbles: shiny glass bubbles float up and wobble
 * ==================================================================================== */
#define NBUB   22
#define NBSIZE 6

static const int bub_radius[NBSIZE] = { 12, 18, 26, 36, 48, 62 };

typedef struct { double x0, y, vy, amp, freq, ph; int size; } bub_t;
typedef struct {
    scene_t sc;
    surface_t *spr[NBSIZE];
    bub_t b[NBUB];
} bubbles_t;

static double smooth01(double e0, double e1, double x) {
    double t = CLAMP((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

/* Add a soft round glow of colour c (strongest alpha `peak` in the middle, fading to 0). */
static void soft_glow(surface_t *s, int cx, int cy, int r, uint32_t c, int peak) {
    if (r <= 0) return;
    int x0 = MAX(0, cx - r), x1 = MIN(s->w - 1, cx + r), y0 = MAX(0, cy - r), y1 = MIN(s->h - 1, cy + r);
    int rr = r * r;
    for (int y = y0; y <= y1; y++) {
        uint32_t *row = surface_row(s, y);
        int dy2 = (y - cy) * (y - cy);
        for (int x = x0; x <= x1; x++) {
            int d2 = (x - cx) * (x - cx) + dy2;
            if (d2 >= rr) continue;
            int f = 256 - (int)((int64_t)d2 * 256 / rr);       /* 1 - d^2, 0..256 */
            int al = peak * f * f >> 16;
            if (al > 0) row[x] = blend(row[x], c, al);
        }
    }
}

/* A glass bubble: nearly clear middle, bright iridescent rim, a soft highlight. */
static surface_t *make_bubble(int R) {
    int D = 2 * R + 2;
    surface_t *s = surface_create_alpha(D, D);
    if (!s) return NULL;
    double c = D / 2.0;
    for (int y = 0; y < D; y++) {
        uint32_t *row = surface_row(s, y);
        for (int x = 0; x < D; x++) {
            double dx = (x + 0.5 - c) / R, dy = (y + 0.5 - c) / R;
            double d = sqrt(dx * dx + dy * dy);
            double cover = CLAMP((1 - d) * R + 0.5, 0.0, 1.0);
            if (cover <= 0) continue;
            double alpha = 0.07 + 0.38 * pow(d, 4) + 0.45 * smooth01(0.80, 0.97, d);
            /* iridescent tint around the rim */
            int hue = (int)(atan2(dy, dx) * 180 / M_PI + 360 + 200) % 360;
            uint32_t tint = color_hsv(hue, 110, 255);
            uint32_t col = color_mix(RGB(210, 236, 255), tint, (int)(200 * smooth01(0.55, 1.0, d)));
            /* main highlight (upper left) and a small one (lower right) */
            double hx = dx + 0.36, hy = dy + 0.40;
            double hs = 1 - sqrt(hx * hx * 1.3 + hy * hy) / 0.30;
            double hs2x = dx - 0.36, hs2y = dy - 0.42;
            double hs2 = 1 - sqrt(hs2x * hs2x + hs2y * hs2y * 2.2) / 0.16;
            double spec = 0;
            if (hs > 0) spec += pow(hs, 1.6) * 0.95;
            if (hs2 > 0) spec += pow(hs2, 1.4) * 0.35;
            if (spec > 0) {
                col = color_mix(col, COLOR_WHITE, (int)(CLAMP(spec, 0.0, 1.0) * 255));
                alpha += spec;
            }
            alpha = CLAMP(alpha, 0.0, 1.0) * cover;
            row[x] = ((uint32_t)(alpha * 255) << 24) | (col & 0xFFFFFF);
        }
    }
    return s;
}

static void bub_spawn(ss_anim_t *a, bub_t *b, bool anywhere) {
    double k = a->k;
    int R = MAX(3, (int)lround(bub_radius[b->size] * k));
    b->x0 = frand(-R, a->w - R);
    b->y = anywhere ? frand(-R, a->h) : a->h + frand(0, a->h * 0.3);
    b->vy = (26 + bub_radius[b->size] * 1.1 + frand(-6, 6)) * k;
    b->amp = frand(6, 22) * k;
    b->freq = frand(0.7, 1.6);
    b->ph = frand(0, 2 * M_PI);
}

static bool bub_init(ss_anim_t *a) {
    bubbles_t *bb = kcalloc(1, sizeof *bb);
    if (!bb) return false;
    a->p = bb;
    bb->sc.bg = make_bg(a->w, a->h, RGB(6, 20, 46), RGB(8, 62, 94));
    if (!bb->sc.bg) return false;
    /* a few soft light patches for depth */
    for (int i = 0; i < 5; i++)
        soft_glow(bb->sc.bg, (int)frand(0, a->w), (int)frand(0, a->h * 0.6), (int)(frand(0.15, 0.35) * a->h),
                  RGB(120, 200, 255), 16);
    for (int i = 0; i < NBSIZE; i++) {
        bb->spr[i] = make_bubble(MAX(3, (int)lround(bub_radius[i] * a->k)));
        if (!bb->spr[i]) return false;
    }
    for (int i = 0; i < NBUB; i++) {
        bb->b[i].size = i * NBSIZE / NBUB;     /* small (far) bubbles first, big ones drawn on top */
        bub_spawn(a, &bb->b[i], true);
    }
    return true;
}

static void bub_fini(ss_anim_t *a) {
    bubbles_t *bb = a->p;
    for (int i = 0; i < NBSIZE; i++) surface_destroy(bb->spr[i]);
    surface_destroy(bb->sc.bg);
    kfree(bb);
}

static void bub_frame(ss_anim_t *a, surface_t *s, bool full) {
    bubbles_t *bb = a->p;
    scene_t *sc = &bb->sc;
    sc->n = NBUB;
    for (int i = 0; i < NBUB; i++) {
        bub_t *b = &bb->b[i];
        const surface_t *img = bb->spr[b->size];
        b->y -= b->vy * a->dt;
        if (b->y + img->h < 0) bub_spawn(a, b, false);
        double x = b->x0 + b->amp * sin(b->ph + a->t * b->freq);
        sc->spr[i].img = img;
        sc->spr[i].x = (int)lround(x);
        sc->spr[i].y = (int)lround(b->y);
        sc->spr[i].op = 255;
    }
    scene_render(a, sc, s, full);
}

/* ======================================================================================
 * 4. Bouncing App Logos: the apps' own icons (their draw_icon) bounce around the screen
 * ==================================================================================== */
#define NLOGO 8

typedef struct { const surface_t *img; double x, y, vx, vy; } logo_t;
typedef struct {
    scene_t sc;
    logo_t l[NLOGO];
    int n, S;
} logos_t;

static bool logos_init(ss_anim_t *a) {
    logos_t *lg = kcalloc(1, sizeof *lg);
    if (!lg) return false;
    a->p = lg;
    lg->sc.bg = make_bg(a->w, a->h, RGB(8, 10, 26), RGB(20, 22, 50));
    if (!lg->sc.bg) return false;
    lg->S = MAX(16, (int)lround(96 * a->k));
    const app_desc_t *cand[64];
    int nc = 0;
    for (int i = 0; i < app_count() && nc < 64; i++) {
        const app_desc_t *ap = app_get(i);
        if (ap && !(ap->flags & APP_HIDDEN) && ap->draw_icon) cand[nc++] = ap;   /* the app's own icon */
    }
    for (int i = nc - 1; i > 0; i--) {            /* shuffle, take the first NLOGO */
        int j = rand() % (i + 1);
        const app_desc_t *t = cand[i]; cand[i] = cand[j]; cand[j] = t;
    }
    int S = lg->S;
    for (int i = 0; i < nc && lg->n < NLOGO; i++) {
        const surface_t *img = app_icon(cand[i], S);
        if (!img) continue;
        logo_t *l = &lg->l[lg->n++];
        l->img = img;
        l->x = frand(0, MAX(1, a->w - S));
        l->y = frand(0, MAX(1, a->h - S));
        double ang = frand(0.35, 1.2) + (rand() % 4) * M_PI / 2;
        double sp = frand(110, 190) * a->k;
        l->vx = sp * cos(ang);
        l->vy = sp * sin(ang);
    }
    return true;
}

static void logos_fini(ss_anim_t *a) {
    logos_t *lg = a->p;
    surface_destroy(lg->sc.bg);
    kfree(lg);
}

static void logos_frame(ss_anim_t *a, surface_t *s, bool full) {
    logos_t *lg = a->p;
    int S = lg->S;
    double mx = MAX(0, a->w - S), my = MAX(0, a->h - S);
    scene_t *sc = &lg->sc;
    sc->n = lg->n;
    for (int i = 0; i < lg->n; i++) {
        logo_t *l = &lg->l[i];
        l->x += l->vx * a->dt;
        l->y += l->vy * a->dt;
        if (l->x < 0) { l->x = -l->x; l->vx = fabs(l->vx); }
        if (l->x > mx) { l->x = 2 * mx - l->x; l->vx = -fabs(l->vx); }
        if (l->y < 0) { l->y = -l->y; l->vy = fabs(l->vy); }
        if (l->y > my) { l->y = 2 * my - l->y; l->vy = -fabs(l->vy); }
        l->x = CLAMP(l->x, 0.0, mx);
        l->y = CLAMP(l->y, 0.0, my);
        sc->spr[i].img = l->img;
        sc->spr[i].x = (int)lround(l->x);
        sc->spr[i].y = (int)lround(l->y);
        sc->spr[i].op = 255;
    }
    scene_render(a, sc, s, full);
}

/* ======================================================================================
 * 5. Lines: like the classic Pipes - colourful pipes grow cell by cell, turn at right
 *    angles, never cross; when the screen is full it clears and starts again
 * ==================================================================================== */
#define PIPE_RATE   38.0     /* cells per second       */
#define PIPE_FULL   0.55     /* clear at 55 % occupied */
#define PIPE_PAUSE  1.2      /* seconds shown full     */

typedef struct {
    surface_t *canvas, *ball;
    uint8_t *occ;
    int gw, gh, cell, ox, oy, thick;
    int hx, hy, dir, len, maxlen, run;
    bool active;
    int filled;
    double acc, pause;
    uint32_t shade[96];
} lines_t;

static const int ddx[4] = { 1, 0, -1, 0 }, ddy[4] = { 0, 1, 0, -1 };

static int cell_cx(const lines_t *L, int x) { return L->ox + x * L->cell + L->cell / 2; }
static int cell_cy(const lines_t *L, int y) { return L->oy + y * L->cell + L->cell / 2; }

static bool cell_free(const lines_t *L, int x, int y) {
    return x >= 0 && y >= 0 && x < L->gw && y < L->gh && !L->occ[y * L->gw + x];
}

/* Shades across the pipe (a lit cylinder) and a matching ball joint. */
static void pipe_colour(lines_t *L, uint32_t base) {
    int T = L->thick;
    for (int j = 0; j < T && j < 96; j++) {
        double u = (j + 0.5) / T * 2 - 1;
        double lit = sqrt(MAX(0.0, 1 - u * u));
        double spec = pow(MAX(0.0, 1 - fabs(u + 0.4) * 3.2), 3);
        uint32_t c = color_mix(RGB(0, 0, 0), base, (int)(255 * (0.22 + 0.78 * lit)));
        L->shade[j] = color_mix(c, COLOR_WHITE, (int)(200 * spec));
    }
    int D = L->ball ? L->ball->w : 0;
    for (int y = 0; y < D; y++) {
        uint32_t *row = surface_row(L->ball, y);
        for (int x = 0; x < D; x++) {
            double dx = (x + 0.5) / D * 2 - 1, dy = (y + 0.5) / D * 2 - 1;
            double d2 = dx * dx + dy * dy, d = sqrt(d2);
            double cover = CLAMP((1 - d) * D / 2 + 0.5, 0.0, 1.0);
            if (cover <= 0) { row[x] = 0; continue; }
            double nz = sqrt(MAX(0.0, 1 - d2));
            double lam = CLAMP(-0.45 * dx - 0.5 * dy + 0.74 * nz, 0.0, 1.0);
            double sx = dx + 0.35, sy = dy + 0.4;
            double spec = pow(MAX(0.0, 1 - sqrt(sx * sx + sy * sy) / 0.45), 2.2);
            uint32_t c = color_mix(RGB(0, 0, 0), base, (int)(255 * (0.2 + 0.8 * lam)));
            c = color_mix(c, COLOR_WHITE, (int)(230 * spec));
            row[x] = ((uint32_t)(cover * 255) << 24) | (c & 0xFFFFFF);
        }
    }
}

static void lines_mark_ball(ss_anim_t *a, lines_t *L, int x, int y) {
    int D = L->ball->w, cx = cell_cx(L, x), cy = cell_cy(L, y);
    gfx_blit_alpha(L->canvas, cx - D / 2, cy - D / 2, L->ball, 255);
    mark(a, rect_make(cx - D / 2, cy - D / 2, D, D));
}

static void lines_segment(ss_anim_t *a, lines_t *L, int x0, int y0, int x1, int y1) {
    int T = L->thick, ax = cell_cx(L, x0), ay = cell_cy(L, y0), bx = cell_cx(L, x1), by = cell_cy(L, y1);
    rect_t r;
    if (ay == by) {
        int xs = MIN(ax, bx), len = abs(bx - ax) + 1;
        for (int j = 0; j < T; j++) gfx_hline(L->canvas, xs, ay - T / 2 + j, len, L->shade[MIN(j, 95)]);
        r = rect_make(xs, ay - T / 2, len, T);
    } else {
        int ys = MIN(ay, by), len = abs(by - ay) + 1;
        for (int j = 0; j < T; j++) gfx_vline(L->canvas, ax - T / 2 + j, ys, len, L->shade[MIN(j, 95)]);
        r = rect_make(ax - T / 2, ys, T, len);
    }
    mark(a, r);
}

static void lines_clear(ss_anim_t *a, lines_t *L) {
    gfx_clear(L->canvas, RGB(0, 0, 0));
    memset(L->occ, 0, (size_t)(L->gw * L->gh));
    L->filled = 0;
    L->active = false;
    a->all = true;
}

static bool lines_new_pipe(ss_anim_t *a, lines_t *L) {
    for (int tries = 0; tries < 80; tries++) {
        int x = rand() % L->gw, y = rand() % L->gh;
        if (!cell_free(L, x, y)) continue;
        L->hx = x;
        L->hy = y;
        L->dir = rand() % 4;
        L->len = L->run = 0;
        L->maxlen = 40 + rand() % 120;
        L->active = true;
        L->occ[y * L->gw + x] = 1;
        L->filled++;
        pipe_colour(L, color_hsv(rand() % 360, 190 + rand() % 50, 225 + rand() % 30));
        lines_mark_ball(a, L, x, y);
        return true;
    }
    return false;
}

static void lines_end_pipe(ss_anim_t *a, lines_t *L) {
    lines_mark_ball(a, L, L->hx, L->hy);
    L->active = false;
    if (L->filled >= L->gw * L->gh * PIPE_FULL) L->pause = PIPE_PAUSE;
}

static void lines_step(ss_anim_t *a, lines_t *L) {
    if (!L->active) {
        if (!lines_new_pipe(a, L)) L->pause = PIPE_PAUSE;    /* no room left: full */
        return;
    }
    int d = L->dir;
    bool turn = rand() % 100 < 20 || L->run > 6 + rand() % 10 || !cell_free(L, L->hx + ddx[d], L->hy + ddy[d]);
    if (turn) {
        int opts[2] = { (d + 1) % 4, (d + 3) % 4 };
        if (rand() & 1) { int t = opts[0]; opts[0] = opts[1]; opts[1] = t; }
        int nd = -1;
        for (int i = 0; i < 2; i++)
            if (cell_free(L, L->hx + ddx[opts[i]], L->hy + ddy[opts[i]])) { nd = opts[i]; break; }
        if (nd < 0 && cell_free(L, L->hx + ddx[d], L->hy + ddy[d])) nd = d;
        if (nd < 0) { lines_end_pipe(a, L); return; }
        d = nd;
    }
    int nx = L->hx + ddx[d], ny = L->hy + ddy[d];
    bool turned = d != L->dir && L->len > 0;
    lines_segment(a, L, L->hx, L->hy, nx, ny);
    if (turned) lines_mark_ball(a, L, L->hx, L->hy);     /* ball joint at the corner */
    L->run = d == L->dir ? L->run + 1 : 0;
    L->dir = d;
    L->hx = nx;
    L->hy = ny;
    L->occ[ny * L->gw + nx] = 1;
    L->filled++;
    if (++L->len >= L->maxlen) lines_end_pipe(a, L);
}

static bool lines_init(ss_anim_t *a) {
    lines_t *L = kcalloc(1, sizeof *L);
    if (!L) return false;
    a->p = L;
    L->cell = MAX(5, (int)lround(30 * a->k));
    L->thick = CLAMP((int)lround(L->cell * 0.44), 2, 95);
    L->gw = MAX(2, a->w / L->cell);
    L->gh = MAX(2, a->h / L->cell);
    L->ox = (a->w - L->gw * L->cell) / 2;
    L->oy = (a->h - L->gh * L->cell) / 2;
    L->canvas = surface_create(a->w, a->h);
    L->ball = surface_create_alpha(L->thick + MAX(2, L->thick / 2), L->thick + MAX(2, L->thick / 2));
    L->occ = kcalloc((size_t)(L->gw * L->gh), 1);
    if (!L->canvas || !L->ball || !L->occ) return false;
    lines_clear(a, L);
    return true;
}

static void lines_fini(ss_anim_t *a) {
    lines_t *L = a->p;
    surface_destroy(L->canvas);
    surface_destroy(L->ball);
    kfree(L->occ);
    kfree(L);
}

static void lines_frame(ss_anim_t *a, surface_t *s, bool full) {
    lines_t *L = a->p;
    if (L->pause > 0) {
        L->pause -= a->dt;
        if (L->pause <= 0) { L->pause = 0; lines_clear(a, L); }
    } else {
        L->acc += a->dt * PIPE_RATE;
        int steps = 0;
        while (L->acc >= 1 && steps < 8 && L->pause <= 0) { lines_step(a, L); L->acc -= 1; steps++; }
        if (L->acc > 1) L->acc = 1;
    }
    if (full || a->all) {
        copy_rect(s, L->canvas, rect_make(0, 0, a->w, a->h));
        a->all = true;
    } else {
        for (int i = 0; i < a->nmark; i++) copy_rect(s, L->canvas, a->mark[i]);
    }
}

/* ======================================================================================
 * Registry and the public API
 * ==================================================================================== */
static const style_ops_t styles[SS_STYLE_COUNT] = {
    [SS_STYLE_NOVA_DROP] = { "Nova Drop", "nova-drop", 20, drop_init, drop_fini, drop_frame },
    [SS_STYLE_ORIGINAL]  = { "Original", "original", 50, orig_init, orig_fini, orig_frame },
    [SS_STYLE_BUBBLES]   = { "Bubbles", "bubbles", 33, bub_init, bub_fini, bub_frame },
    [SS_STYLE_LOGOS]     = { "Bouncing App Logos", "logos", 20, logos_init, logos_fini, logos_frame },
    [SS_STYLE_LINES]     = { "Lines", "lines", 33, lines_init, lines_fini, lines_frame },
};

int screensaver_style_count(void) { return SS_STYLE_COUNT; }

const char *screensaver_style_name(int style) {
    return style >= 0 && style < SS_STYLE_COUNT ? styles[style].name : "";
}

const char *screensaver_style_key(int style) {
    return style >= 0 && style < SS_STYLE_COUNT ? styles[style].key : "";
}

ss_anim_t *ss_anim_create(int style, int w, int h, bool preview) {
    if (style < 0 || style >= SS_STYLE_COUNT || w <= 0 || h <= 0) return NULL;
    ss_anim_t *a = kcalloc(1, sizeof *a);
    if (!a) return NULL;
    a->ops = &styles[style];
    a->style = style;
    a->w = w;
    a->h = h;
    a->preview = preview;
    /* the full-screen Original keeps its fixed pixel sizes; everything else scales */
    a->k = (style == SS_STYLE_ORIGINAL && !preview) ? 1.0 : h / 768.0;
    a->t0 = a->last = sys_uptime_ms();
    if (!a->ops->init(a)) {
        if (a->p) a->ops->fini(a);
        kfree(a);
        return NULL;
    }
    return a;
}

int ss_anim_interval(const ss_anim_t *a) { return a ? a->ops->frame_ms : 50; }

int ss_anim_frame(ss_anim_t *a, surface_t *dst, bool full, rect_t *dirty, int max) {
    if (!a || !dst || !dirty || max <= 0 || dst->w < a->w || dst->h < a->h) return 0;
    uint32_t now = sys_uptime_ms();
    a->dt = MIN((now - a->last) / 1000.0, 0.2);
    a->last = now;
    a->t = (now - a->t0) / 1000.0;
    a->nmark = 0;
    a->all = full;
    rect_t clip = dst->clip;
    surface_reset_clip(dst);
    a->ops->frame(a, dst, full);
    dst->clip = clip;
    if (a->all) { dirty[0] = rect_make(0, 0, a->w, a->h); return 1; }
    int n = 0;
    for (int i = 0; i < a->nmark; i++) {
        if (n < max) dirty[n++] = a->mark[i];
        else dirty[max - 1] = rect_union(dirty[max - 1], a->mark[i]);
    }
    return n;
}

void ss_anim_destroy(ss_anim_t *a) {
    if (!a) return;
    if (a->p) a->ops->fini(a);
    kfree(a);
}
