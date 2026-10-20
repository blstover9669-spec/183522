/*
 * graphics_fractal.c - "Fractal Explorer": Mandelbrot, Julia, Burning Ship and
 * Tricorn sets with smooth (continuous escape time) colouring.
 *
 * Rendering never blocks the UI: an image is computed progressively in four
 * passes (8x8 blocks, then 4x4, 2x2 and single pixels, each pass computing
 * only the pixels the previous passes have not), and every window timer tick
 * does at most RENDER_BUDGET_MS of work before returning to the main loop.
 * The smooth iteration value of every pixel is kept (`mu`), so switching or
 * cycling the palette only recolours - no recomputation.
 *
 * Mouse: click = zoom in 2x centred on the point, right-click = zoom out,
 * drag = zoom to a box, wheel = zoom around the pointer, middle-drag = pan,
 * Shift+click on a Mandelbrot = the Julia set for that point.
 * Keys: + / - zoom, arrows pan, Backspace back, Home reset, P palette,
 * C colour cycling, J / M Julia / Mandelbrot, [ ] iterations, Ctrl+S save
 * as BMP, F11 full screen, F1 help.
 *
 * All state lives in a per-window fractal_t, freed on EV_CLOSE.
 */
#include <nova.h>
#include "graphics_bmp.h"

#define FX_W            820
#define FX_H            600
#define TB_H            44
#define ST_H            24
#define LUT_N           1024             /* palette entries (power of two)      */
#define DENS            24.0             /* palette entries per iteration        */
#define NHIST           64
#define RENDER_BUDGET_MS 14
#define RENDER_TICK_MS  4
#define CYCLE_TICK_MS   50
#define BAILOUT         65536.0          /* |z|^2 > 256^2: smooth colouring      */
#define MIN_SCALE       2e-15            /* about the limit of double precision  */
#define MSG_MS          5000

enum { F_MANDEL, F_JULIA, F_SHIP, F_TRICORN, F_COUNT };
static const char *const g_type_names[F_COUNT] = { "Mandelbrot", "Julia", "Burning Ship", "Tricorn" };

typedef struct { const char *name; double re, im; } jpreset_t;
static const jpreset_t g_julia[] = {
    { "Douady rabbit", -0.123, 0.745 },  { "Dendrite", 0.0, 1.0 },
    { "San Marco", -0.75, 0.0 },         { "Siegel disk", -0.391, -0.587 },
    { "Dragon", -0.8, 0.156 },           { "Spiral", 0.285, 0.01 },
    { "Galaxy", -0.7269, 0.1889 },       { "Lightning", -0.4, 0.6 },
    { "Starfish", -0.54, 0.54 },
};
#define NJULIA ARRAY_SIZE(g_julia)
#define JULIA_CUSTOM NJULIA              /* dropdown index of "Custom point" */

typedef struct { float t; uint32_t c; } stop_t;
typedef struct {
    const char *name;
    uint32_t inside;
    int n;
    stop_t st[7];
} palette_t;

static const palette_t g_palettes[] = {
    { "Classic", RGB(0, 0, 0), 5,
      { { 0.0f, RGB(0, 7, 100) }, { 0.16f, RGB(32, 107, 203) }, { 0.42f, RGB(237, 255, 255) },
        { 0.6425f, RGB(255, 170, 0) }, { 0.8575f, RGB(0, 2, 0) } } },
    { "Fire", RGB(0, 0, 0), 5,
      { { 0.0f, RGB(24, 0, 4) }, { 0.25f, RGB(170, 18, 0) }, { 0.5f, RGB(255, 150, 0) },
        { 0.68f, RGB(255, 250, 190) }, { 0.85f, RGB(210, 70, 0) } } },
    { "Ocean", RGB(0, 4, 14), 5,
      { { 0.0f, RGB(0, 12, 34) }, { 0.3f, RGB(0, 82, 160) }, { 0.55f, RGB(50, 196, 226) },
        { 0.7f, RGB(232, 255, 255) }, { 0.85f, RGB(16, 120, 176) } } },
    { "Electric", RGB(0, 0, 0), 5,
      { { 0.0f, RGB(12, 0, 44) }, { 0.3f, RGB(120, 0, 255) }, { 0.5f, RGB(255, 40, 200) },
        { 0.7f, RGB(255, 240, 255) }, { 0.85f, RGB(60, 200, 255) } } },
    { "Forest", RGB(4, 10, 4), 5,
      { { 0.0f, RGB(10, 30, 12) }, { 0.3f, RGB(40, 120, 34) }, { 0.55f, RGB(200, 222, 90) },
        { 0.7f, RGB(255, 250, 222) }, { 0.85f, RGB(112, 72, 30) } } },
    { "Sunset", RGB(10, 0, 20), 5,
      { { 0.0f, RGB(32, 10, 64) }, { 0.3f, RGB(182, 40, 100) }, { 0.55f, RGB(255, 140, 60) },
        { 0.75f, RGB(255, 232, 152) }, { 0.9f, RGB(92, 30, 112) } } },
    { "Grayscale", RGB(0, 0, 0), 2,
      { { 0.0f, RGB(10, 10, 10) }, { 0.5f, RGB(250, 250, 250) } } },
    { "Rainbow", RGB(0, 0, 0), 0, { { 0 } } },   /* n == 0: HSV hue wheel */
};
#define NPAL ARRAY_SIZE(g_palettes)

static const int g_iters[] = { 0, 100, 250, 500, 1000, 2500, 5000 };   /* 0 = auto */
#define NITERS ARRAY_SIZE(g_iters)
static const int g_steps[] = { 8, 4, 2, 1 };
#define NPASS ARRAY_SIZE(g_steps)

typedef struct view {
    int type;
    double cx, cy, scale;    /* centre and units per pixel */
    double jr, ji;           /* Julia constant              */
    int jpreset;             /* preset index or JULIA_CUSTOM */
} view_t;

typedef struct fractal {
    window_t *win;
    widget_t *dd_type, *dd_pal, *dd_iter, *dd_julia, *chk_cycle;
    widget_t *b_back, *b_reset, *b_save, *sink;
    view_t v;
    view_t hist[NHIST];
    int nhist;
    view_t mandel;           /* Mandelbrot view to return to from a Julia set */
    bool have_mandel;

    int pal, iter_idx, maxit;
    double peps;             /* periodicity-check tolerance */
    uint32_t lut[LUT_N], inside, bg;
    int cycle_off;
    bool cycling;
    int cycle_timer;

    int vw, vh;              /* view size in pixels */
    surface_t *img;
    float *mu;               /* smooth iteration count, -1 inside, -2 not computed */

    bool rendering;
    int pass, ry, rx, render_timer;
    uint32_t done, t_start, t_ms;

    /* mouse */
    int press_btn;           /* button pressed in the view, 0 = none */
    int px0, py0, px1, py1;  /* press / current position (view coords) */
    bool box, shift_click;
    int pan_dx, pan_dy;
    int mx, my;
    bool mouse_in;

    char msg[160];
    uint32_t msg_until;
    int msg_timer;
    char save_path[VFS_PATH_MAX];
} fractal_t;

static void fx_restart(fractal_t *f);

/* ======================================================================================
 * helpers
 * ====================================================================================== */
static bool flipped(int type) { return type == F_SHIP; }   /* draw the ship upright */

static double default_scale(const fractal_t *f, int type) {
    double sx, sy;
    switch (type) {
    case F_JULIA:   sx = 3.4; sy = 2.5; break;
    case F_SHIP:    sx = 3.6; sy = 2.7; break;
    case F_TRICORN: sx = 4.2; sy = 3.4; break;
    default:        sx = 3.3; sy = 2.5; break;
    }
    return fmax(sx / MAX(1, f->vw), sy / MAX(1, f->vh));
}

static void default_view(fractal_t *f, view_t *v, int type) {
    double jr = v->jr, ji = v->ji;
    int jp = v->jpreset;
    memset(v, 0, sizeof *v);
    v->type = type;
    v->jr = jr; v->ji = ji; v->jpreset = jp;
    switch (type) {
    case F_MANDEL:  v->cx = -0.65; v->cy = 0; break;
    case F_SHIP:    v->cx = -0.45; v->cy = -0.5; break;
    case F_TRICORN: v->cx = -0.25; v->cy = 0; break;
    default:        v->cx = 0; v->cy = 0; break;
    }
    v->scale = default_scale(f, type);
}

static double zoom_factor(const fractal_t *f) { return default_scale(f, f->v.type) / f->v.scale; }

static void px_to_c(const fractal_t *f, double px, double py, double *re, double *im) {
    *re = f->v.cx + (px - f->vw * 0.5) * f->v.scale;
    double dy = (py - f->vh * 0.5) * f->v.scale;
    *im = flipped(f->v.type) ? f->v.cy + dy : f->v.cy - dy;
}

static rect_t view_rect(const fractal_t *f) { return rect_make(0, TB_H, f->vw, f->vh); }
static void view_dirty(fractal_t *f) { win_invalidate_rect(f->win, 0, TB_H, f->vw, f->vh); }
static void status_dirty(fractal_t *f) {
    win_invalidate_rect(f->win, 0, win_height(f->win) - ST_H, win_width(f->win), ST_H);
}

static void fx_message(fractal_t *f, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void fx_message(fractal_t *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->msg, sizeof f->msg, fmt, ap);
    va_end(ap);
    f->msg_until = sys_uptime_ms() + MSG_MS;
    if (!f->msg_timer) f->msg_timer = win_set_timer(f->win, 500);
    status_dirty(f);
}

/* ======================================================================================
 * palettes
 * ====================================================================================== */
static void build_lut(fractal_t *f) {
    const palette_t *p = &g_palettes[f->pal];
    f->inside = p->inside;
    f->bg = RGB(16, 18, 24);
    for (int i = 0; i < LUT_N; i++) {
        float t = (float)i / LUT_N;
        if (p->n == 0) { f->lut[i] = color_hsv(i * 360 / LUT_N, 210, 255); continue; }
        /* find the segment [st[k], st[k+1]] containing t (cyclic) */
        int k = p->n - 1;
        for (int j = 0; j < p->n; j++) if (p->st[j].t <= t) k = j;
        int k2 = (k + 1) % p->n;
        float t0 = p->st[k].t, t1 = k2 ? p->st[k2].t : 1.0f + p->st[0].t;
        float u = (t1 > t0) ? (t - t0) / (t1 - t0) : 0;
        if (u < 0) u = 0;
        if (u > 1) u = 1;
        u = u * u * (3 - 2 * u);                       /* smoothstep: softer bands */
        f->lut[i] = color_mix(p->st[k].c, p->st[k2].c, (int)(u * 255 + 0.5f));
    }
}

static inline uint32_t fx_color(const fractal_t *f, float m) {
    if (m < 0) return m < -1.5f ? f->bg : f->inside;
    return f->lut[((int)(m * DENS) + f->cycle_off) & (LUT_N - 1)];
}

static void recolor(fractal_t *f) {
    if (!f->img) return;
    for (int y = 0; y < f->vh; y++) {
        uint32_t *row = surface_row(f->img, y);
        const float *m = f->mu + (size_t)y * f->vw;
        for (int x = 0; x < f->vw; x++) row[x] = fx_color(f, m[x]);
    }
    view_dirty(f);
}

/* ======================================================================================
 * the fractal kernels
 * ====================================================================================== */
static float smooth_mu(int n, double m2) {
    double lz = 0.5 * log(m2);                 /* log|z| */
    double mu = n + 1 - log(lz / M_LN2) / M_LN2;
    return mu < 0 ? 0.0f : (float)mu;
}

/* Smooth escape count of one point, or -1 if it does not escape. */
static float fx_point(const fractal_t *f, double re, double im) {
    const int maxit = f->maxit;
    const double eps = f->peps;
    double zx, zy, cr, ci;
    int type = f->v.type;
    if (type == F_JULIA) {
        zx = re; zy = im; cr = f->v.jr; ci = f->v.ji;
    } else {
        cr = re; ci = im; zx = zy = 0;
        if (type == F_MANDEL) {
            /* main cardioid and period-2 bulb are inside: skip them */
            double xq = re - 0.25, q = xq * xq + im * im;
            if (q * (q + xq) <= 0.25 * im * im) return -1;
            if ((re + 1) * (re + 1) + im * im <= 0.0625) return -1;
        }
    }
    double x2 = zx * zx, y2 = zy * zy;
    double ox = zx, oy = zy;                   /* Brent periodicity check */
    int n = 0, cnt = 0, period = 8;
    switch (type) {
    case F_MANDEL:
    case F_JULIA:
        while (n < maxit) {
            zy = 2 * zx * zy + ci;
            zx = x2 - y2 + cr;
            x2 = zx * zx; y2 = zy * zy;
            n++;
            if (x2 + y2 > BAILOUT) return smooth_mu(n, x2 + y2);
            if (fabs(zx - ox) < eps && fabs(zy - oy) < eps) return -1;
            if (++cnt == period) { cnt = 0; period <<= 1; ox = zx; oy = zy; }
        }
        break;
    case F_SHIP:
        while (n < maxit) {
            zy = 2 * fabs(zx * zy) + ci;
            zx = x2 - y2 + cr;
            x2 = zx * zx; y2 = zy * zy;
            n++;
            if (x2 + y2 > BAILOUT) return smooth_mu(n, x2 + y2);
            if (fabs(zx - ox) < eps && fabs(zy - oy) < eps) return -1;
            if (++cnt == period) { cnt = 0; period <<= 1; ox = zx; oy = zy; }
        }
        break;
    case F_TRICORN:
        while (n < maxit) {
            zy = -2 * zx * zy + ci;
            zx = x2 - y2 + cr;
            x2 = zx * zx; y2 = zy * zy;
            n++;
            if (x2 + y2 > BAILOUT) return smooth_mu(n, x2 + y2);
            if (fabs(zx - ox) < eps && fabs(zy - oy) < eps) return -1;
            if (++cnt == period) { cnt = 0; period <<= 1; ox = zx; oy = zy; }
        }
        break;
    }
    return -1;
}

/* ======================================================================================
 * progressive, time-sliced rendering
 * ====================================================================================== */
static void fill_block(fractal_t *f, int x, int y, int s, float m) {
    uint32_t c = fx_color(f, m);
    int w = MIN(s, f->vw - x), h = MIN(s, f->vh - y);
    for (int j = 0; j < h; j++) {
        float *mr = f->mu + (size_t)(y + j) * f->vw + x;
        uint32_t *row = surface_row(f->img, y + j) + x;
        for (int i = 0; i < w; i++) { mr[i] = m; row[i] = c; }
    }
}

/* Final (1-pixel) pass only: a pixel whose already computed neighbours one
   pixel away are all inside the set is inside too. This skips about 3/4 of
   the most expensive pixels (they run to the iteration limit); the only
   possible error is an exterior gap narrower than one pixel. */
static bool guess_inside(const fractal_t *f, int x, int y) {
    const float *m = f->mu;
    int w = f->vw, h = f->vh, n = 0;
    bool ox = x & 1, oy = y & 1;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if ((dx != 0) != ox || (dy != 0) != oy) continue;   /* only the computed (even) grid */
            if (!dx && !dy) continue;
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            if (m[(size_t)ny * w + nx] != -1.0f) return false;
            n++;
        }
    return n > 0;
}

static void render_finish(fractal_t *f) {
    f->rendering = false;
    f->t_ms = sys_uptime_ms() - f->t_start;
    if (f->render_timer) { win_kill_timer(f->win, f->render_timer); f->render_timer = 0; }
    klog("fractal: %s %dx%d rendered in %u ms (zoom %.3g, %d iterations)",
         g_type_names[f->v.type], f->vw, f->vh, f->t_ms, zoom_factor(f), f->maxit);
    view_dirty(f);
    status_dirty(f);
}

static void render_slice(fractal_t *f) {
    if (!f->rendering || !f->img) return;
    uint32_t t0 = sys_uptime_ms();
    int miny = INT_MAX, maxy = -1, cnt = 0;
    bool out_of_time = false;
    while (f->pass < NPASS && !out_of_time) {
        int s = g_steps[f->pass];
        if (f->ry >= f->vh) { f->pass++; f->ry = 0; f->rx = -1; continue; }
        int y = f->ry;
        bool odd_only = f->pass > 0 && (y % (2 * s)) == 0;   /* even columns done by the coarser pass */
        int x0 = odd_only ? s : 0, dx = odd_only ? 2 * s : s;
        if (f->rx < 0) f->rx = x0;
        double re0, im;
        px_to_c(f, 0, y, &re0, &im);
        for (; f->rx < f->vw; f->rx += dx) {
            double re = re0 + f->rx * f->v.scale;
            bool guess = s == 1 && guess_inside(f, f->rx, y);
            fill_block(f, f->rx, y, s, guess ? -1.0f : fx_point(f, re, im));
            f->done++;
            if ((++cnt & 15) == 0 && sys_uptime_ms() - t0 >= RENDER_BUDGET_MS) {
                f->rx += dx;
                out_of_time = true;
                break;
            }
        }
        miny = MIN(miny, y);
        maxy = MAX(maxy, y + s);
        if (f->rx >= f->vw) { f->ry += s; f->rx = -1; }
        if (sys_uptime_ms() - t0 >= RENDER_BUDGET_MS) out_of_time = true;
    }
    if (maxy > miny) win_invalidate_rect(f->win, 0, TB_H + miny, f->vw, MIN(maxy, f->vh) - miny);
    win_invalidate_rect(f->win, 0, TB_H, f->vw, 3);          /* progress bar */
    status_dirty(f);
    if (f->pass >= NPASS) render_finish(f);
}

static int auto_iters(const fractal_t *f) {
    double z = zoom_factor(f);
    double l = z > 1 ? log2(z) : 0;
    int it = (int)(160 + 55 * l);
    if (f->v.type == F_JULIA) it += 60;
    return CLAMP(it, 120, 6000);
}

static void fx_restart(fractal_t *f) {
    if (!f->img) return;
    f->maxit = g_iters[f->iter_idx] ? g_iters[f->iter_idx] : auto_iters(f);
    f->peps = fmax(1e-17, fmin(1e-11, f->v.scale * 1e-6));
    f->pass = 0;
    f->ry = 0;
    f->rx = -1;
    f->done = 0;
    f->t_start = sys_uptime_ms();
    f->rendering = true;
    f->pan_dx = f->pan_dy = 0;
    if (!f->render_timer) f->render_timer = win_set_timer(f->win, RENDER_TICK_MS);
    render_slice(f);       /* first slice right away: instant coarse preview */
    status_dirty(f);
}

static bool fx_alloc(fractal_t *f) {
    int W = win_width(f->win), H = win_height(f->win);
    int vw = MAX(16, W), vh = MAX(16, H - TB_H - ST_H);
    if (f->img && vw == f->vw && vh == f->vh) return true;
    surface_t *img = surface_create(vw, vh);
    float *mu = malloc((size_t)vw * vh * sizeof *mu);
    if (!img || !mu) {
        surface_destroy(img);
        free(mu);
        return false;
    }
    for (size_t i = 0; i < (size_t)vw * vh; i++) mu[i] = -2.0f;
    gfx_clear(img, f->bg);
    /* keep the old picture centred while the new one renders */
    if (f->img) {
        gfx_blit(img, (vw - f->vw) / 2, (vh - f->vh) / 2, f->img);
        surface_destroy(f->img);
    }
    free(f->mu);
    f->img = img;
    f->mu = mu;
    f->vw = vw;
    f->vh = vh;
    return true;
}

/* ======================================================================================
 * navigation
 * ====================================================================================== */
static void sync_widgets(fractal_t *f) {
    ui_set_value(f->dd_type, f->v.type);
    ui_set_value(f->dd_julia, f->v.jpreset);
    ui_set_visible(f->dd_julia, f->v.type == F_JULIA);
    ui_set_enabled(f->b_back, f->nhist > 0);
}

static void push_hist(fractal_t *f) {
    if (f->nhist == NHIST) {
        memmove(f->hist, f->hist + 1, (NHIST - 1) * sizeof f->hist[0]);
        f->nhist--;
    }
    f->hist[f->nhist++] = f->v;
    ui_set_enabled(f->b_back, true);
}

/* Change to view nv (recording the old one) and re-render. */
static void go(fractal_t *f, const view_t *nv) {
    view_t v = *nv;
    if (v.scale < MIN_SCALE) {
        v.scale = MIN_SCALE;
        fx_message(f, "Maximum zoom reached (the limit of double precision)");
    }
    if (v.scale > 1.0) v.scale = 1.0;
    push_hist(f);
    if (f->v.type == F_MANDEL && v.type != F_MANDEL) { f->mandel = f->v; f->have_mandel = true; }
    f->v = v;
    sync_widgets(f);
    fx_restart(f);
}

/* Zoom by `factor` (> 1 = in) keeping view point (px,py) fixed, or centring it. */
static void zoom_at(fractal_t *f, double px, double py, double factor, bool centre) {
    view_t v = f->v;
    double re, im;
    px_to_c(f, px, py, &re, &im);
    v.scale = f->v.scale / factor;
    if (centre) {
        v.cx = re;
        v.cy = im;
    } else {
        double dx = (px - f->vw * 0.5) * v.scale, dy = (py - f->vh * 0.5) * v.scale;
        v.cx = re - dx;
        v.cy = flipped(v.type) ? im - dy : im + dy;
    }
    go(f, &v);
}

static void pan_by(fractal_t *f, double dx_px, double dy_px) {
    view_t v = f->v;
    v.cx += dx_px * v.scale;
    v.cy += (flipped(v.type) ? dy_px : -dy_px) * v.scale;
    go(f, &v);
}

static void go_back(fractal_t *f) {
    if (!f->nhist) { fx_message(f, "No earlier view"); return; }
    f->v = f->hist[--f->nhist];
    sync_widgets(f);
    fx_restart(f);
}

static void go_reset(fractal_t *f) {
    view_t v = f->v;
    default_view(f, &v, f->v.type);
    go(f, &v);
}

static void set_type(fractal_t *f, int type) {
    if (type == f->v.type) return;
    view_t v = f->v;
    if (type == F_MANDEL && f->have_mandel) v = f->mandel;
    else default_view(f, &v, type);
    go(f, &v);
}

static void julia_at(fractal_t *f, double px, double py) {
    double re, im;
    px_to_c(f, px, py, &re, &im);
    view_t v = f->v;
    default_view(f, &v, F_JULIA);
    v.jr = re;
    v.ji = im;
    v.jpreset = JULIA_CUSTOM;
    go(f, &v);
    fx_message(f, "Julia set for c = %.6f %c %.6fi  (M returns to the Mandelbrot set)", re, im < 0 ? '-' : '+', fabs(im));
}

static void set_palette(fractal_t *f, int pal) {
    f->pal = (pal + (int)NPAL) % (int)NPAL;
    ui_set_value(f->dd_pal, f->pal);
    build_lut(f);
    recolor(f);
    status_dirty(f);
}

static void set_iters(fractal_t *f, int idx) {
    idx = CLAMP(idx, 0, (int)NITERS - 1);
    if (idx == f->iter_idx) return;
    f->iter_idx = idx;
    ui_set_value(f->dd_iter, idx);
    fx_restart(f);
}

static void set_cycling(fractal_t *f, bool on) {
    f->cycling = on;
    ui_set_value(f->chk_cycle, on);
    if (on && !f->cycle_timer) f->cycle_timer = win_set_timer(f->win, CYCLE_TICK_MS);
    if (!on && f->cycle_timer) { win_kill_timer(f->win, f->cycle_timer); f->cycle_timer = 0; }
}

/* ======================================================================================
 * saving
 * ====================================================================================== */
static void do_save(fractal_t *f, const char *path) {
    int err = gbmp_save(path, f->img, false);
    if (err < 0) {
        char buf[VFS_PATH_MAX + 64];
        snprintf(buf, sizeof buf, "Could not save \"%s\":\n%s.", path, vfs_strerror(err));
        ui_message_box(f->win, "Fractal Explorer", buf, MB_OK | MB_ICON_ERROR, NULL, NULL);
        return;
    }
    klog("fractal: saved %s", path);
    fx_message(f, "Saved %s%s", path, f->rendering ? " (still rendering - saved the preview)" : "");
}

static void overwrite_cb(int r, void *ctx) {
    fractal_t *f = ctx;
    if (r == MB_RESULT_YES) do_save(f, f->save_path);
}

static void save_cb(const char *path, void *ctx) {
    fractal_t *f = ctx;
    if (!path) return;
    strlcpy(f->save_path, path, sizeof f->save_path);
    if (strcasecmp(vfs_extension(f->save_path), "bmp")) strlcat(f->save_path, ".bmp", sizeof f->save_path);
    if (vfs_exists(f->save_path)) {
        char q[VFS_PATH_MAX + 64];
        snprintf(q, sizeof q, "\"%s\" already exists.\nDo you want to replace it?", vfs_basename(f->save_path));
        ui_message_box(f->win, "Save image", q, MB_YESNO | MB_ICON_WARNING, overwrite_cb, f);
        return;
    }
    do_save(f, f->save_path);
}

static void fx_save(fractal_t *f) {
    char start[VFS_PATH_MAX];
    vfs_mkdirs("/home/user/Pictures");
    snprintf(start, sizeof start, "/home/user/Pictures/%s.bmp", g_type_names[f->v.type]);
    ui_file_dialog(f->win, UI_FILE_SAVE, "Save fractal as BMP", start, "bmp", save_cb, f);
}

static void fx_help(fractal_t *f) {
    ui_message_box(f->win, "Fractal Explorer",
                   "Click: zoom in (2x) on that point.  Right-click: zoom out.\n"
                   "Drag a box: zoom to it.  Wheel: zoom around the pointer.\n"
                   "Middle-drag or arrow keys: move around.\n"
                   "Shift+click (Mandelbrot): open the Julia set of that point.\n\n"
                   "+ / -  zoom      Backspace  previous view      Home  reset\n"
                   "P  next palette      C  cycle colours      [ ]  iterations\n"
                   "J  Julia at pointer      M  Mandelbrot\n"
                   "Ctrl+S  save as BMP      F11  full screen",
                   MB_OK | MB_ICON_INFO, NULL, NULL);
}

/* ======================================================================================
 * painting
 * ====================================================================================== */
/* Zoom box with the view's aspect ratio, anchored at the press point. */
static rect_t zoom_box(const fractal_t *f) {
    int dx = f->px1 - f->px0, dy = f->py1 - f->py0;
    double aspect = (double)f->vw / f->vh;
    int w = abs(dx), h = abs(dy);
    if (w > h * aspect) h = (int)(w / aspect + 0.5);
    else w = (int)(h * aspect + 0.5);
    int x = dx < 0 ? f->px0 - w : f->px0, y = dy < 0 ? f->py0 - h : f->py0;
    return rect_make(x, y, w, h);
}

static void draw_status(fractal_t *f, surface_t *s) {
    const theme_t *t = theme_get();
    int W = s->w, sy = s->h - ST_H;
    uint32_t line = color_mix(t->window_bg, t->window_border, 90);
    uint32_t muted = color_mix(t->window_text, t->window_bg, 90);
    gfx_fill_rect(s, 0, sy, W, ST_H, t->dark ? color_shade(t->window_bg, -10) : color_shade(t->window_bg, -6));
    gfx_hline(s, 0, sy, W, line);
    int ty = sy + 1 + (ST_H - 1 - font_height(FONT_UI)) / 2;

    /* right: render state + palette strip */
    char right[64];
    if (f->rendering) {
        uint32_t total = (uint32_t)f->vw * f->vh;
        snprintf(right, sizeof right, "Rendering %u%%", total ? (uint32_t)((uint64_t)f->done * 100 / total) : 0);
    } else {
        snprintf(right, sizeof right, "Rendered in %u.%02u s", f->t_ms / 1000, (f->t_ms % 1000) / 10);
    }
    int strip_w = 72, strip_x = W - 12 - strip_w;
    for (int i = 0; i < strip_w; i++)
        gfx_vline(s, strip_x + i, sy + 7, ST_H - 13, f->lut[(i * LUT_N / strip_w + f->cycle_off) & (LUT_N - 1)]);
    gfx_rect(s, strip_x - 1, sy + 6, strip_w + 2, ST_H - 11, line);
    int rw = font_text_width(FONT_UI, right);
    int rx = strip_x - 14 - rw;
    gfx_text(s, rx, ty, right, f->rendering ? t->window_text : muted, FONT_UI);

    /* left: message, pointer position or view info */
    char left[200];
    if (f->msg[0]) {
        gfx_text_ellipsis(s, 12, ty, rx - 24, f->msg, t->window_text, FONT_UI_BOLD);
        return;
    }
    char zs[32];
    double z = zoom_factor(f);
    if (z < 10000) snprintf(zs, sizeof zs, "%.1fx", z);
    else snprintf(zs, sizeof zs, "%.2ex", z);
    int w = gfx_text(s, 12, ty, g_type_names[f->v.type], t->window_text, FONT_UI_BOLD);
    double re = f->v.cx, im = f->v.cy;
    const char *what = "centre";
    if (f->mouse_in && f->press_btn == 0) {
        px_to_c(f, f->mx, f->my - TB_H, &re, &im);
        what = "pointer";
    }
    int prec = z < 1e3 ? 6 : z < 1e7 ? 10 : 14;
    snprintf(left, sizeof left, "%s %.*f %c %.*fi    zoom %s    %d iterations", what, prec, re,
             im < 0 ? '-' : '+', prec, fabs(im), zs, f->maxit);
    gfx_text_ellipsis(s, 12 + w + 12, ty, rx - 36 - w, left, muted, FONT_UI);
}

static void fx_paint(fractal_t *f) {
    surface_t *s = win_surface(f->win);
    const theme_t *t = theme_get();
    int W = s->w;
    uint32_t line = color_mix(t->window_bg, t->window_border, 90);
    gfx_hline(s, 0, TB_H - 1, W, line);

    rect_t vr = view_rect(f);
    rect_t old = s->clip;
    rect_t vc = rect_intersect(old, vr);
    if (!rect_empty(vc) && f->img) {
        s->clip = vc;
        if (f->pan_dx || f->pan_dy) gfx_fill_rect(s, vr.x, vr.y, vr.w, vr.h, f->bg);
        gfx_blit(s, vr.x + f->pan_dx, vr.y + f->pan_dy, f->img);
        if (f->box) {
            rect_t b = zoom_box(f);
            b.y += TB_H;
            gfx_fill_rect(s, b.x, b.y, b.w, b.h, RGBA(255, 255, 255, 36));
            gfx_rect(s, b.x - 1, b.y - 1, b.w + 2, b.h + 2, RGBA(0, 0, 0, 150));
            gfx_rect(s, b.x, b.y, b.w, b.h, COLOR_WHITE);
            int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
            gfx_hline(s, cx - 5, cy, 11, COLOR_WHITE);
            gfx_vline(s, cx, cy - 5, 11, COLOR_WHITE);
        }
        if (f->rendering) {
            uint32_t total = (uint32_t)f->vw * f->vh;
            int pw = total ? (int)((uint64_t)f->done * f->vw / total) : 0;
            gfx_fill_rect(s, vr.x, vr.y, pw, 3, t->accent);
            gfx_fill_rect(s, vr.x + pw, vr.y, vr.w - pw, 3, RGBA(0, 0, 0, 90));
        }
        s->clip = old;
    }
    draw_status(f, s);
}

/* ======================================================================================
 * layout, widgets
 * ====================================================================================== */
static void fx_layout(fractal_t *f) {
    int W = win_width(f->win);
    ui_move(f->b_save, W - 10 - 58, 8, 58, 28);
    ui_move(f->b_reset, W - 10 - 58 - 6 - 58, 8, 58, 28);
    ui_move(f->b_back, W - 10 - 3 * 58 - 2 * 6, 8, 58, 28);
    ui_move(f->sink, 0, TB_H, f->vw, f->vh);
}

static void refocus(fractal_t *f) { ui_focus(f->sink); }

static void on_type(widget_t *w, void *ctx) {
    fractal_t *f = ctx;
    set_type(f, ui_get_value(w));
    refocus(f);
}

static void on_pal(widget_t *w, void *ctx) {
    fractal_t *f = ctx;
    set_palette(f, ui_get_value(w));
    refocus(f);
}

static void on_iter(widget_t *w, void *ctx) {
    fractal_t *f = ctx;
    set_iters(f, ui_get_value(w));
    refocus(f);
}

static void on_julia(widget_t *w, void *ctx) {
    fractal_t *f = ctx;
    int i = ui_get_value(w);
    if (i >= 0 && i < (int)NJULIA) {
        view_t v = f->v;
        default_view(f, &v, F_JULIA);
        v.jr = g_julia[i].re;
        v.ji = g_julia[i].im;
        v.jpreset = i;
        go(f, &v);
    } else {
        ui_set_value(w, f->v.jpreset);
        fx_message(f, "Shift+click a point of the Mandelbrot set to explore its Julia set");
    }
    refocus(f);
}

static void on_cycle(widget_t *w, void *ctx) {
    fractal_t *f = ctx;
    set_cycling(f, ui_get_value(w));
    refocus(f);
}

static void on_back(widget_t *w, void *ctx) { fractal_t *f = ctx; go_back(f); refocus(f); }
static void on_reset(widget_t *w, void *ctx) { fractal_t *f = ctx; go_reset(f); refocus(f); }
static void on_save(widget_t *w, void *ctx) { fractal_t *f = ctx; refocus(f); fx_save(f); }

/* ======================================================================================
 * input
 * ====================================================================================== */
static void fx_mouse_down(fractal_t *f, const event_t *ev) {
    int x = ev->x, y = ev->y - TB_H;
    if (!rect_contains(view_rect(f), ev->x, ev->y) || f->press_btn) return;
    f->press_btn = ev->button;
    f->px0 = f->px1 = x;
    f->py0 = f->py1 = y;
    f->box = false;
    f->shift_click = ev->mods & MOD_SHIFT;
    if (ev->button == MOUSE_MIDDLE) win_set_cursor(f->win, CURSOR_MOVE);
}

static void fx_mouse_move(fractal_t *f, const event_t *ev) {
    int x = ev->x, y = ev->y - TB_H;
    bool in = rect_contains(view_rect(f), ev->x, ev->y);
    f->mx = ev->x;
    f->my = ev->y;
    if (in != f->mouse_in) { f->mouse_in = in; win_set_cursor(f->win, in ? CURSOR_CROSSHAIR : CURSOR_ARROW); }
    if (f->press_btn && !(ev->buttons & f->press_btn)) {   /* release happened outside */
        f->press_btn = 0;
        f->box = false;
        view_dirty(f);
    }
    if (f->press_btn == MOUSE_LEFT && !f->shift_click) {
        f->px1 = x;
        f->py1 = y;
        if (!f->box && (abs(x - f->px0) > 4 || abs(y - f->py0) > 4)) f->box = true;
        if (f->box) view_dirty(f);
    } else if (f->press_btn == MOUSE_MIDDLE) {
        f->pan_dx = x - f->px0;
        f->pan_dy = y - f->py0;
        view_dirty(f);
    }
    if (in || f->press_btn) status_dirty(f);
}

static void fx_mouse_up(fractal_t *f, const event_t *ev) {
    if (!f->press_btn || ev->button != f->press_btn) return;
    int btn = f->press_btn;
    f->press_btn = 0;
    int x = ev->x, y = ev->y - TB_H;
    if (btn == MOUSE_MIDDLE) {
        win_set_cursor(f->win, f->mouse_in ? CURSOR_CROSSHAIR : CURSOR_ARROW);
        int dx = f->pan_dx, dy = f->pan_dy;
        if (dx || dy) {
            /* shift the current picture so the preview matches the new view */
            surface_t *n = surface_create(f->vw, f->vh);
            float *m = malloc((size_t)f->vw * f->vh * sizeof *m);
            if (n && m) {
                gfx_clear(n, f->bg);
                gfx_blit(n, dx, dy, f->img);
                for (int j = 0; j < f->vh; j++)
                    for (int i = 0; i < f->vw; i++) {
                        int sx = i - dx, sy = j - dy;
                        m[(size_t)j * f->vw + i] = (sx >= 0 && sy >= 0 && sx < f->vw && sy < f->vh)
                                                   ? f->mu[(size_t)sy * f->vw + sx] : -2.0f;
                    }
                surface_destroy(f->img);
                free(f->mu);
                f->img = n;
                f->mu = m;
            } else {
                surface_destroy(n);
                free(m);
            }
            f->pan_dx = f->pan_dy = 0;
            pan_by(f, -dx, -dy);
        }
        return;
    }
    if (btn == MOUSE_RIGHT) {
        zoom_at(f, x, y, 0.5, true);
        return;
    }
    if (btn != MOUSE_LEFT) return;
    if (f->box) {
        f->box = false;
        f->px1 = x;
        f->py1 = y;
        rect_t b = zoom_box(f);
        view_dirty(f);
        if (b.w >= 6 && b.h >= 4) {
            double factor = (double)f->vw / b.w;
            zoom_at(f, b.x + b.w * 0.5, b.y + b.h * 0.5, factor, true);
        }
        return;
    }
    if (f->shift_click && f->v.type == F_MANDEL) julia_at(f, x, y);
    else zoom_at(f, x, y, 2.0, true);
}

static void fx_key(fractal_t *f, const event_t *ev) {
    int k = ev->key;
    bool shift = ev->mods & MOD_SHIFT;
    if (ev->mods & MOD_CTRL) {
        if (k == 's') fx_save(f);
        else if (k == 'w') win_close(f->win);
        return;
    }
    if (ev->mods & MOD_ALT) return;
    int step = shift ? MIN(f->vw, f->vh) / 2 : MIN(f->vw, f->vh) / 8;
    switch (k) {
    case '=': case '+': zoom_at(f, f->vw / 2.0, f->vh / 2.0, 2.0, true); break;
    case '-': zoom_at(f, f->vw / 2.0, f->vh / 2.0, 0.5, true); break;
    case KEY_LEFT: pan_by(f, -step, 0); break;
    case KEY_RIGHT: pan_by(f, step, 0); break;
    case KEY_UP: pan_by(f, 0, -step); break;
    case KEY_DOWN: pan_by(f, 0, step); break;
    case KEY_BACKSPACE: go_back(f); break;
    case KEY_HOME: case 'r': go_reset(f); break;
    case 'p': set_palette(f, f->pal + (shift ? -1 : 1)); break;
    case 'c': set_cycling(f, !f->cycling); break;
    case '[': set_iters(f, f->iter_idx - 1); break;
    case ']': set_iters(f, f->iter_idx + 1); break;
    case 'm': set_type(f, F_MANDEL); break;
    case 'b': set_type(f, F_SHIP); break;
    case 't': set_type(f, F_TRICORN); break;
    case 'j':
        if (f->v.type == F_MANDEL && f->mouse_in) julia_at(f, f->mx, f->my - TB_H);
        else set_type(f, F_JULIA);
        break;
    case KEY_F1: fx_help(f); break;
    case KEY_F11: win_set_fullscreen(f->win, !win_is_fullscreen(f->win)); break;
    case KEY_ESCAPE:
        if (f->box || f->press_btn) { f->box = false; f->press_btn = 0; f->pan_dx = f->pan_dy = 0; view_dirty(f); }
        else if (win_is_fullscreen(f->win)) win_set_fullscreen(f->win, false);
        break;
    }
}

static void fx_free(fractal_t *f) {
    surface_destroy(f->img);
    free(f->mu);
    free(f);
}

static void fx_event(window_t *win, const event_t *ev) {
    fractal_t *f = win_get_userdata(win);
    if (!f) return;
    switch (ev->type) {
    case EV_PAINT: fx_paint(f); break;
    case EV_RESIZE:
        if (!fx_alloc(f)) { fx_message(f, "Not enough memory for a view this large"); break; }
        fx_layout(f);
        fx_restart(f);
        break;
    case EV_TIMER:
        if (ev->timer_id == f->render_timer) render_slice(f);
        else if (ev->timer_id == f->cycle_timer) {
            f->cycle_off = (f->cycle_off + 6) & (LUT_N - 1);
            recolor(f);
            status_dirty(f);
        } else if (ev->timer_id == f->msg_timer) {
            if (f->msg[0] && sys_uptime_ms() >= f->msg_until) { f->msg[0] = 0; status_dirty(f); }
            if (!f->msg[0]) { win_kill_timer(win, f->msg_timer); f->msg_timer = 0; }
        }
        break;
    case EV_MOUSE_DOWN: fx_mouse_down(f, ev); break;
    case EV_MOUSE_MOVE: fx_mouse_move(f, ev); break;
    case EV_MOUSE_UP: fx_mouse_up(f, ev); break;
    case EV_MOUSE_WHEEL:
        if (rect_contains(view_rect(f), ev->x, ev->y))
            zoom_at(f, ev->x, ev->y - TB_H, ev->wheel < 0 ? 1.5 : 1 / 1.5, false);
        break;
    case EV_MOUSE_LEAVE:
        f->mouse_in = false;
        status_dirty(f);
        break;
    case EV_BLUR:
        if (f->press_btn) { f->press_btn = 0; f->box = false; f->pan_dx = f->pan_dy = 0; view_dirty(f); }
        break;
    case EV_KEY_DOWN: fx_key(f, ev); break;
    case EV_CLOSE:
        win_set_userdata(win, NULL);
        fx_free(f);
        break;
    }
}

static void fractal_launch(const char *arg) {
    fractal_t *f = calloc(1, sizeof *f);
    if (!f) return;
    window_t *win = win_create("Fractal Explorer", FX_W, FX_H, WIN_RESIZABLE);
    if (!win) { free(f); return; }
    f->win = win;
    f->pal = 0;
    f->iter_idx = 0;
    build_lut(f);
    if (!fx_alloc(f)) {
        ui_message_box(NULL, "Fractal Explorer", "Not enough memory to start.", MB_OK | MB_ICON_ERROR, NULL, NULL);
        win_close(win);
        free(f);
        return;
    }
    win_set_userdata(win, f);
    win_set_event_handler(win, fx_event);
    win_set_min_size(win, FX_W, 420);

    f->v.jr = g_julia[0].re;
    f->v.ji = g_julia[0].im;
    f->v.jpreset = 0;
    int type = F_MANDEL;
    if (arg && *arg) {
        for (int i = 0; i < F_COUNT; i++) if (!strcasecmp(arg, g_type_names[i])) type = i;
        if (!strcasecmp(arg, "ship")) type = F_SHIP;
    }
    default_view(f, &f->v, type);

    f->sink = ui_custom(win, 0, TB_H, f->vw, f->vh, NULL, NULL, f);
    f->dd_type = ui_dropdown(win, 10, 8, 124, 28, on_type, f);
    for (int i = 0; i < F_COUNT; i++) ui_list_add(f->dd_type, g_type_names[i]);
    f->dd_pal = ui_dropdown(win, 142, 8, 112, 28, on_pal, f);
    for (int i = 0; i < (int)NPAL; i++) ui_list_add(f->dd_pal, g_palettes[i].name);
    f->dd_iter = ui_dropdown(win, 262, 8, 96, 28, on_iter, f);
    for (int i = 0; i < (int)NITERS; i++) {
        char b[24];
        if (g_iters[i]) snprintf(b, sizeof b, "Iter: %d", g_iters[i]);
        else snprintf(b, sizeof b, "Iter: auto");
        ui_list_add(f->dd_iter, b);
    }
    f->chk_cycle = ui_checkbox(win, 366, 8, 72, 28, "Cycle", false, on_cycle, f);
    f->dd_julia = ui_dropdown(win, 444, 8, 140, 28, on_julia, f);
    for (int i = 0; i < (int)NJULIA; i++) ui_list_add(f->dd_julia, g_julia[i].name);
    ui_list_add(f->dd_julia, "Custom point");
    f->b_back = ui_button(win, 0, 8, 58, 28, "Back", on_back, f);
    f->b_reset = ui_button(win, 0, 8, 58, 28, "Reset", on_reset, f);
    f->b_save = ui_button(win, 0, 8, 58, 28, "Save", on_save, f);
    ui_set_value(f->dd_pal, f->pal);
    ui_set_value(f->dd_iter, f->iter_idx);
    sync_widgets(f);
    fx_layout(f);
    refocus(f);
    fx_restart(f);
    fx_message(f, "Click to zoom in, right-click to zoom out, drag a box, Shift+click for a Julia set.  F1: help");
}

/* ======================================================================================
 * icon: a tiny Mandelbrot set rendered into a rounded tile
 * ====================================================================================== */
static void fractal_icon(surface_t *s, int x, int y, int size) {
    int r = size / 5, m = size / 14;
    int tx = x + m, ty = y + m, ts = size - 2 * m;
    gfx_fill_rounded_gradient_v(s, tx, ty, ts, ts, r, RGB(14, 30, 92), RGB(4, 8, 36));
    const palette_t *p = &g_palettes[0];
    for (int j = 1; j < ts - 1; j++) {
        int py = ty + j;
        if (py < s->clip.y || py >= s->clip.y + s->clip.h) continue;
        for (int i = 1; i < ts - 1; i++) {
            int px = tx + i;
            if (px < s->clip.x || px >= s->clip.x + s->clip.w) continue;
            /* stay clear of the anti-aliased rounded corners */
            int cxr = i < r ? r - i : (i >= ts - r ? i - (ts - 1 - r) : 0);
            int cyr = j < r ? r - j : (j >= ts - r ? j - (ts - 1 - r) : 0);
            if (cxr * cxr + cyr * cyr > (r - 1) * (r - 1)) continue;
            double re = -0.62 + (i - ts * 0.5) * 2.7 / ts, im = (j - ts * 0.5) * 2.7 / ts;
            double zx = 0, zy = 0, x2 = 0, y2 = 0;
            int n = 0;
            while (n < 60 && x2 + y2 <= 256) { zy = 2 * zx * zy + im; zx = x2 - y2 + re; x2 = zx * zx; y2 = zy * zy; n++; }
            uint32_t *px_ptr = surface_row(s, py) + px;
            if (n >= 60) { *px_ptr = RGB(6, 6, 16); continue; }
            double mu = n + 1 - log(0.5 * log(x2 + y2) / M_LN2) / M_LN2;
            if (mu < 2.5) continue;                       /* far outside: keep the tile */
            double t = fmod(mu * 0.06 + 0.05, 1.0);
            int k = 0;
            for (int q = 0; q < p->n; q++) if (p->st[q].t <= t) k = q;
            int k2 = (k + 1) % p->n;
            double t0 = p->st[k].t, t1 = k2 ? p->st[k2].t : 1.0;
            uint32_t c = color_mix(p->st[k].c, p->st[k2].c, (int)((t - t0) / (t1 - t0) * 255));
            int a = (int)CLAMP((mu - 2.5) * 70, 0, 255);
            *px_ptr = color_mix(*px_ptr | 0xFF000000u, c, a);
        }
    }
    /* soft highlight along the top edge */
    gfx_fill_rounded_rect(s, tx + r / 2, ty + 1, ts - r, MAX(1, size / 48), 1, RGBA(255, 255, 255, 60));
}

REGISTER_APP(.name = "Fractal Explorer", .category = CAT_GRAPHICS,
             .description = "Explore the Mandelbrot and Julia sets",
             .launch = fractal_launch, .draw_icon = fractal_icon);
