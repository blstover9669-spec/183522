/*
 * fun_memory.c - Memory Match (Games).
 *
 * Classic "pairs" game: the cards lie face down, turn over two per move and
 * keep them when the symbols match. Three board sizes (Easy 4x3, Normal 5x4,
 * Hard 6x6), a move counter, a clock that starts with the first card, and the
 * best result per board size (fewest moves, fastest time) kept in
 * ~/.fun_scores.
 *
 *   - 18 symbols, all drawn programmatically: every symbol is a set of
 *     implicit shapes (circles, polygons, capsules, set differences) rendered
 *     with 4x4 supersampling into a cached card face (one per symbol, built
 *     the first time it is needed and dropped when the board size or theme
 *     changes);
 *   - cards flip with a 3D-ish horizontal squash animation, matched pairs pop
 *     and get a check badge, a wrong pair flashes red and turns back after a
 *     moment (clicking another card turns it back at once);
 *   - mouse, or keyboard: arrows + Space/Enter, 1/2/3 board size, N/F2 new
 *     game.
 *
 * Everything runs from window timers (16 ms while something animates, a slow
 * tick for the clock otherwise, none when idle).
 */
#include "fun_common.h"

#define MM_W        480
#define MM_PAD      22
#define MM_HDR_Y    14
#define MM_HDR_H    30
#define MM_STATS_Y  56
#define MM_STATS_H  56
#define MM_BOARD_Y  124
#define MM_BOARD    (MM_W - 2 * MM_PAD)
#define MM_GAP      8
#define MM_HINT_Y   (MM_BOARD_Y + MM_BOARD + 10)
#define MM_H        (MM_HINT_Y + 26)

#define MM_MAXCARDS 36
#define MM_NSYM     18

#define FLIP_MS     260     /* one card turning over            */
#define SHOW_MS     800     /* a wrong pair stays visible       */
#define POP_MS      360     /* matched pair "pop"               */
#define WRONG_MS    520     /* red flash on a wrong pair        */
#define WIN_DELAY   500     /* result card after the last pair  */

enum { CARD_DOWN, CARD_UP, CARD_MATCHED };

typedef struct { const char *name; int cols, rows; } mm_level_t;
static const mm_level_t levels[3] = { { "Easy", 4, 3 }, { "Normal", 5, 4 }, { "Hard", 6, 6 } };
static const char *const level_names[3] = { "Easy 4x3", "Normal 5x4", "Hard 6x6" };

typedef struct {
    window_t *win;
    fn_rng_t rng;
    int level, cols, rows, ncards, npairs;
    int8_t sym[MM_MAXCARDS];
    uint8_t state[MM_MAXCARDS];
    uint32_t flip_t0[MM_MAXCARDS];   /* start of the last flip animation */
    uint32_t pop_t0[MM_MAXCARDS];
    uint32_t wrong_t0[MM_MAXCARDS];
    int open[2], nopen;              /* face-up cards not yet matched    */
    bool hide_pending;
    uint32_t hide_due;
    int moves, found;
    bool started, finished;
    uint32_t start_ms, end_ms, finish_t0;
    int best_moves, best_ms;         /* records for this level (0 = none) */
    bool rec_moves, rec_time;        /* this game set a record           */
    int stars;
    int timer, timer_iv;
    int hover, kb;
    bool kb_on;
    int seg_hover;
    fn_btn_t btn[2];                 /* 0 = New Game, 1 = Play Again (result card) */
    rect_t seg_r, board_r;
    surface_t *face[MM_NSYM];        /* cached card faces at the current card size */
    surface_t *back;
    int cache_w, cache_h;
    int8_t used[MM_NSYM];            /* symbols dealt this game          */
    int prerender;                   /* faces of used[] built so far      */
} mm_t;

/* ======================================================================================
 * Symbols: each one is a function of a point (u, v) in [-1, 1]^2 (v grows downwards)
 * returning which colour layer covers it (0 = none).
 * ==================================================================================== */
static const uint32_t sym_cols[MM_NSYM][4] = {
    { RGB(229, 57, 53) },                                                       /* heart     */
    { RGB(255, 179, 0), RGB(255, 213, 79) },                                    /* star      */
    { RGB(253, 216, 53), RGB(214, 160, 0) },                                    /* moon      */
    { RGB(251, 140, 0), RGB(255, 183, 77) },                                    /* sun       */
    { RGB(30, 110, 229), RGB(144, 202, 249) },                                  /* drop      */
    { RGB(124, 179, 66), RGB(197, 225, 165), RGB(85, 139, 47) },                /* leaf      */
    { RGB(171, 71, 188), RGB(255, 202, 40) },                                   /* flower    */
    { RGB(216, 27, 96) },                                                       /* notes     */
    { RGB(0, 151, 167), RGB(38, 198, 218), RGB(178, 235, 242) },                /* gem       */
    { RGB(3, 155, 229) },                                                       /* snowflake */
    { RGB(46, 125, 50), RGB(121, 85, 72) },                                     /* tree      */
    { RGB(0, 137, 123), RGB(0, 105, 92), COLOR_WHITE, RGB(33, 33, 33) },        /* fish      */
    { RGB(205, 145, 30), RGB(229, 57, 53), RGB(30, 136, 229) },                 /* crown     */
    { RGB(96, 125, 139) },                                                      /* key       */
    { RGB(216, 67, 21), RGB(215, 204, 200), RGB(109, 76, 65), RGB(144, 202, 249) }, /* house */
    { RGB(48, 63, 159) },                                                       /* anchor    */
    { RGB(103, 58, 183) },                                                      /* bolt      */
    { RGB(38, 38, 46) },                                                        /* spade     */
};

static bool in_circ(double u, double v, double cx, double cy, double r) {
    double dx = u - cx, dy = v - cy;
    return dx * dx + dy * dy <= r * r;
}

static bool in_ell(double u, double v, double cx, double cy, double rx, double ry) {
    double dx = (u - cx) / rx, dy = (v - cy) / ry;
    return dx * dx + dy * dy <= 1;
}

static bool in_box(double u, double v, double x0, double y0, double x1, double y1) {
    return u >= x0 && u <= x1 && v >= y0 && v <= y1;
}

/* even-odd point in polygon; pts = x0, y0, x1, y1, ... */
static bool in_poly(double u, double v, const double *pts, int n) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        double xi = pts[2 * i], yi = pts[2 * i + 1], xj = pts[2 * j], yj = pts[2 * j + 1];
        if ((yi > v) != (yj > v) && u < (xj - xi) * (v - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

/* capsule: within distance r of the segment (x0,y0)-(x1,y1) */
static bool in_seg(double u, double v, double x0, double y0, double x1, double y1, double r) {
    double dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((u - x0) * dx + (v - y0) * dy) / len2 : 0;
    t = CLAMP(t, 0.0, 1.0);
    double px = x0 + t * dx - u, py = y0 + t * dy - v;
    return px * px + py * py <= r * r;
}

/* rotate by an angle given as its cosine / sine */
static void rot(double *u, double *v, const double *cs) {
    double x = *u * cs[0] - *v * cs[1], y = *u * cs[1] + *v * cs[0];
    *u = x;
    *v = y;
}

/* Trigonometry the shapes need, computed once (the predicates run ~10^5 times per
   card face, and x87 sin/cos are slow under emulation). */
static struct {
    bool ready;
    double star[20], star_in[20];     /* outer and inner star polygons */
    double sun[8][2];                 /* ray directions                */
    double petal[5][2];               /* flower petal centres          */
    double arm[6][2], branch[6][2][2];/* snowflake                     */
    double r45[2], r22[2], r35[2];    /* rotations                     */
} T;

static void sym_init(void) {
    if (T.ready) return;
    for (int i = 0; i < 10; i++) {
        double a = -M_PI / 2 + i * M_PI / 5, r = (i & 1) ? 0.44 : 1.0;
        T.star[2 * i] = cos(a) * r;
        T.star[2 * i + 1] = sin(a) * r + 0.1;
        T.star_in[2 * i] = cos(a) * r * 0.5;
        T.star_in[2 * i + 1] = sin(a) * r * 0.5 + 0.1;
    }
    for (int k = 0; k < 8; k++) { T.sun[k][0] = cos(k * M_PI / 4); T.sun[k][1] = sin(k * M_PI / 4); }
    for (int k = 0; k < 5; k++) {
        double a = -M_PI / 2 + k * 2 * M_PI / 5;
        T.petal[k][0] = cos(a) * 0.55;
        T.petal[k][1] = sin(a) * 0.55;
    }
    for (int k = 0; k < 6; k++) {
        double a = -M_PI / 2 + k * M_PI / 3;
        T.arm[k][0] = cos(a);
        T.arm[k][1] = sin(a);
        for (int s = 0; s < 2; s++) {
            double b = a + (s ? 1 : -1) * M_PI / 4;
            T.branch[k][s][0] = cos(b);
            T.branch[k][s][1] = sin(b);
        }
    }
    T.r45[0] = cos(M_PI / 4);  T.r45[1] = sin(M_PI / 4);
    T.r22[0] = cos(22 * M_PI / 180); T.r22[1] = sin(22 * M_PI / 180);
    T.r35[0] = cos(35 * M_PI / 180); T.r35[1] = sin(35 * M_PI / 180);
    T.ready = true;
}

static bool heart_at(double u, double v) {
    double x = u / 0.86, y = -v / 0.86 + 0.13;
    double q = x * x + y * y - 1;
    return q * q * q - x * x * y * y * y <= 0;
}

static int sym_layer(int sym, double u, double v) {
    if (u < -1.05 || u > 1.05 || v < -1.05 || v > 1.05) return 0;
    switch (sym) {
    case 0:   /* heart */
        return heart_at(u, v) ? 1 : 0;
    case 1:   /* star with a lighter inner star */
        if (in_poly(u, v, T.star_in, 10)) return 2;
        return in_poly(u, v, T.star, 10) ? 1 : 0;
    case 2: { /* crescent moon with a darker rim */
        if (!in_circ(u, v, 0, 0, 0.92) || in_circ(u, v, 0.42, -0.26, 0.72)) return 0;
        double d1 = 0.92 - sqrt(u * u + v * v);
        double dx = u - 0.42, dy = v + 0.26, d2 = sqrt(dx * dx + dy * dy) - 0.72;
        return MIN(d1, d2) < 0.085 ? 2 : 1;
    }
    case 3: { /* sun */
        if (in_circ(u, v, 0, 0, 0.36)) return 2;
        if (in_circ(u, v, 0, 0, 0.5)) return 1;
        if (u * u + v * v > 0.99 * 0.99) return 0;
        for (int k = 0; k < 8; k++) {
            double c = T.sun[k][0], d = T.sun[k][1];
            if (in_seg(u, v, c * 0.68, d * 0.68, c * 0.9, d * 0.9, 0.085)) return 1;
        }
        return 0;
    }
    case 4: { /* water drop with a highlight */
        static const double tri[6] = { 0, -0.98, 0.512, 0.03, -0.512, 0.03 };
        if (in_ell(u, v, -0.2, 0.3, 0.09, 0.19)) return 2;
        return in_circ(u, v, 0, 0.3, 0.58) || in_poly(u, v, tri, 3) ? 1 : 0;
    }
    case 5: { /* leaf: a lens with a midrib and a stem */
        double x = u, y = v;
        rot(&x, &y, T.r45);
        x /= 0.92;
        y /= 0.92;
        if (in_seg(x, y, -1.02, 0, -0.78, 0, 0.06)) return 3;
        bool lens = in_circ(x, y, 0, 0.864, 1.284) && in_circ(x, y, 0, -0.864, 1.284);
        if (!lens) return 0;
        if (in_seg(x, y, -0.8, 0, 0.7, 0, 0.035)) return 2;
        for (int k = -1; k <= 1; k += 2)
            for (int j = 0; j < 3; j++) {
                double x0 = -0.45 + j * 0.38;
                if (in_seg(x, y, x0, 0, x0 + 0.24, k * 0.24, 0.025)) return 2;
            }
        return 1;
    }
    case 6: { /* flower */
        if (in_circ(u, v, 0, 0, 0.3)) return 2;
        for (int k = 0; k < 5; k++)
            if (in_circ(u, v, T.petal[k][0], T.petal[k][1], 0.38)) return 1;
        return 0;
    }
    case 7: { /* two beamed eighth notes */
        double x = u, y = v;
        for (int k = 0; k < 2; k++) {
            double hx = k ? 0.42 : -0.5, hy = k ? 0.5 : 0.66;
            double ex = x - hx, ey = y - hy;
            rot(&ex, &ey, T.r22);
            if (in_ell(ex, ey, 0, 0, 0.29, 0.2)) return 1;
            double sx = hx + 0.2;
            if (in_box(x, y, sx, (k ? -0.85 : -0.7) + 0.05, sx + 0.12, hy)) return 1;
        }
        static const double beam[8] = { -0.3, -0.72, 0.74, -0.97, 0.74, -0.7, -0.3, -0.45 };
        return in_poly(x, y, beam, 4) ? 1 : 0;
    }
    case 8: { /* cut gem */
        static const double crown[8] = { -0.45, -0.62, 0.45, -0.62, 0.92, -0.18, -0.92, -0.18 };
        static const double table[8] = { -0.2, -0.62, 0.2, -0.62, 0.36, -0.18, -0.36, -0.18 };
        static const double pav[6] = { -0.92, -0.18, 0.92, -0.18, 0, 0.9 };
        static const double mid[6] = { -0.36, -0.18, 0.36, -0.18, 0, 0.9 };
        if (in_poly(u, v, table, 4)) return 3;
        if (in_poly(u, v, crown, 4)) return 2;
        if (in_poly(u, v, mid, 3)) return 2;
        if (in_poly(u, v, pav, 3)) return 1;
        return 0;
    }
    case 9: { /* snowflake */
        if (in_circ(u, v, 0, 0, 0.16)) return 1;
        if (u * u + v * v > 0.99 * 0.99) return 0;
        /* every feature of an arm lies within 30 degrees of it: test the nearest arm only */
        int k = 0;
        double best = -2;
        for (int a = 0; a < 6; a++) {
            double d = u * T.arm[a][0] + v * T.arm[a][1];
            if (d > best) { best = d; k = a; }
        }
        double ca = T.arm[k][0], sa = T.arm[k][1];
        if (in_seg(u, v, 0, 0, ca * 0.92, sa * 0.92, 0.075)) return 1;
        double bx = ca * 0.5, by = sa * 0.5;
        for (int s = 0; s < 2; s++)
            if (in_seg(u, v, bx, by, bx + T.branch[k][s][0] * 0.3, by + T.branch[k][s][1] * 0.3, 0.06)) return 1;
        return 0;
    }
    case 10: { /* pine tree */
        static const double t1[6] = { 0, -0.98, 0.46, -0.36, -0.46, -0.36 };
        static const double t2[6] = { 0, -0.66, 0.64, 0.1, -0.64, 0.1 };
        static const double t3[6] = { 0, -0.3, 0.82, 0.56, -0.82, 0.56 };
        if (in_poly(u, v, t1, 3) || in_poly(u, v, t2, 3) || in_poly(u, v, t3, 3)) return 1;
        return in_box(u, v, -0.14, 0.5, 0.14, 0.94) ? 2 : 0;
    }
    case 11: { /* fish facing left */
        static const double tail[6] = { 0.36, 0, 0.96, -0.46, 0.96, 0.46 };
        static const double fin[6] = { -0.22, -0.36, 0.1, -0.66, 0.22, -0.3 };
        if (in_circ(u, v, -0.44, -0.08, 0.055)) return 4;
        if (in_circ(u, v, -0.44, -0.08, 0.12)) return 3;
        if (in_ell(u, v, -0.12, 0, 0.62, 0.42)) {
            double dx = u + 0.02, dy = v;   /* gill line */
            double d = sqrt(dx * dx + dy * dy);
            if (u < -0.02 && d > 0.27 && d < 0.33 && fabs(dy) < 0.26) return 2;
            return 1;
        }
        if (in_poly(u, v, tail, 3)) return 2;
        return in_poly(u, v, fin, 3) ? 2 : 0;
    }
    case 12: { /* crown */
        static const double body[14] = { -0.8, 0.34, -0.92, -0.42, -0.45, -0.02, 0, -0.62, 0.45, -0.02, 0.92, -0.42, 0.8, 0.34 };
        if (in_circ(u, v, -0.42, 0.53, 0.1)) return 2;
        if (in_circ(u, v, 0.42, 0.53, 0.1)) return 2;
        if (in_circ(u, v, 0, 0.53, 0.11)) return 3;
        if (in_box(u, v, -0.82, 0.34, 0.82, 0.72)) return 1;
        if (in_circ(u, v, -0.92, -0.5, 0.12) || in_circ(u, v, 0, -0.72, 0.12) || in_circ(u, v, 0.92, -0.5, 0.12))
            return 1;
        return in_poly(u, v, body, 7) ? 1 : 0;
    }
    case 13: { /* key */
        double x = u, y = v;
        rot(&x, &y, T.r35);
        x /= 0.95;
        y /= 0.95;
        if (in_circ(x, y, -0.55, 0, 0.4) && !in_circ(x, y, -0.55, 0, 0.18)) return 1;
        if (in_box(x, y, -0.2, -0.09, 0.95, 0.09)) return 1;
        if (in_box(x, y, 0.5, 0.0, 0.64, 0.36) || in_box(x, y, 0.76, 0.0, 0.95, 0.3)) return 1;
        return 0;
    }
    case 14: { /* house */
        static const double roof[6] = { 0, -0.92, 0.98, -0.04, -0.98, -0.04 };
        if (in_box(u, v, 0.3, 0.12, 0.56, 0.38)) return 4;
        if (in_box(u, v, -0.2, 0.3, 0.14, 0.88)) return 3;
        if (in_poly(u, v, roof, 3) || in_box(u, v, 0.44, -0.78, 0.66, -0.3)) return 1;
        return in_box(u, v, -0.72, -0.12, 0.72, 0.88) ? 2 : 0;
    }
    case 15: { /* anchor */
        if (in_circ(u, v, 0, -0.74, 0.19) && !in_circ(u, v, 0, -0.74, 0.085)) return 1;
        if (in_box(u, v, -0.075, -0.58, 0.075, 0.86)) return 1;
        if (in_box(u, v, -0.42, -0.44, 0.42, -0.31)) return 1;
        if (v > 0.12 && in_circ(u, v, 0, 0.12, 0.78) && !in_circ(u, v, 0, 0.12, 0.64)) return 1;
        static const double l[6] = { -0.98, 0.2, -0.46, 0.2, -0.82, -0.18 };
        static const double r[6] = { 0.98, 0.2, 0.46, 0.2, 0.82, -0.18 };
        return in_poly(u, v, l, 3) || in_poly(u, v, r, 3) ? 1 : 0;
    }
    case 16: { /* lightning bolt */
        static const double bolt[14] = { 0.1, -0.98, -0.56, 0.1, -0.05, 0.1, -0.3, 0.98, 0.6, -0.22, 0.08, -0.22, 0.42, -0.98 };
        return in_poly(u, v, bolt, 7) ? 1 : 0;
    }
    case 17: { /* spade */
        static const double stem[6] = { 0, 0.3, 0.34, 0.95, -0.34, 0.95 };
        if (heart_at(u / 0.8, -(v + 0.17) / 0.8)) return 1;   /* upside-down heart */
        return in_poly(u, v, stem, 3) ? 1 : 0;
    }
    }
    return 0;
}

/* Render symbol `sym` into the box centred at (cx, cy) with half-size r. Each pixel is
   probed with 2x2 samples; only pixels where they disagree (edges) get the full 4x4. */
static void mm_draw_symbol(surface_t *s, int sym, double cx, double cy, double r) {
    sym_init();
    int x0 = (int)floor(cx - r) - 1, x1 = (int)ceil(cx + r) + 1;
    int y0 = (int)floor(cy - r) - 1, y1 = (int)ceil(cy + r) + 1;
    x0 = MAX(x0, s->clip.x);
    y0 = MAX(y0, s->clip.y);
    x1 = MIN(x1, s->clip.x + s->clip.w - 1);
    y1 = MIN(y1, s->clip.y + s->clip.h - 1);
    const uint32_t *cols = sym_cols[sym];
    double inv = 1.0 / r;
    for (int py = y0; py <= y1; py++) {
        for (int px = x0; px <= x1; px++) {
            int cnt[5] = { 0 };
            double bu = (px - cx) * inv, bv = (py - cy) * inv;
            int l0 = sym_layer(sym, bu + 0.25 * inv, bv + 0.25 * inv);
            int l1 = sym_layer(sym, bu + 0.75 * inv, bv + 0.25 * inv);
            int l2 = sym_layer(sym, bu + 0.25 * inv, bv + 0.75 * inv);
            int l3 = sym_layer(sym, bu + 0.75 * inv, bv + 0.75 * inv);
            if (l0 == l1 && l0 == l2 && l0 == l3) {
                if (!l0) continue;
                cnt[l0] = 16;
            } else {
                for (int sy = 0; sy < 4; sy++) {
                    double v = bv + (sy + 0.5) * 0.25 * inv;
                    for (int sx = 0; sx < 4; sx++) cnt[sym_layer(sym, bu + (sx + 0.5) * 0.25 * inv, v)]++;
                }
            }
            int tot = 16 - cnt[0];
            if (!tot) continue;
            uint32_t rr = 0, gg = 0, bb = 0;
            for (int l = 1; l <= 4; l++) {
                if (!cnt[l]) continue;
                uint32_t c = cols[l - 1];
                rr += COLOR_R(c) * (uint32_t)cnt[l];
                gg += COLOR_G(c) * (uint32_t)cnt[l];
                bb += COLOR_B(c) * (uint32_t)cnt[l];
            }
            uint32_t col = RGBA(rr / (uint32_t)tot, gg / (uint32_t)tot, bb / (uint32_t)tot, tot * 255 / 16);
            gfx_pixel(s, px, py, col);
        }
    }
}

/* ======================================================================================
 * Card images
 * ==================================================================================== */
static int mm_radius(int w, int h) { return MAX(6, MIN(w, h) / 9); }

static surface_t *mm_render_face(int sym, int w, int h) {
    surface_t *s = surface_create_alpha(w, h);
    if (!s) return NULL;
    int rad = mm_radius(w, h);
    bool dark = theme_get()->dark;
    gfx_fill_rounded_rect(s, 0, 0, w, h, rad, dark ? RGB(150, 156, 170) : RGB(206, 211, 222));
    gfx_fill_rounded_gradient_v(s, 1, 1, w - 2, h - 2, rad - 1, dark ? RGB(240, 242, 246) : COLOR_WHITE,
                                dark ? RGB(222, 226, 234) : RGB(244, 246, 250));
    int m = MIN(w, h);
    double cx = w / 2.0, cy = h / 2.0;
    gfx_fill_circle_aa(s, cx, cy, m * 0.37, color_alpha(sym_cols[sym][0], 26));
    mm_draw_symbol(s, sym, cx, cy, m * 0.29);
    return s;
}

static surface_t *mm_render_back(int w, int h) {
    surface_t *s = surface_create_alpha(w, h);
    if (!s) return NULL;
    const theme_t *t = theme_get();
    int rad = mm_radius(w, h);
    uint32_t base = t->accent;
    gfx_fill_rounded_gradient_v(s, 0, 0, w, h, rad, color_shade(base, 34), color_shade(base, -46));
    /* lattice inside an inset frame */
    int in = MAX(5, MIN(w, h) / 14);
    rect_t ir = rect_make(in, in, w - 2 * in, h - 2 * in);
    surface_set_clip(s, ir);
    int step = MAX(8, MIN(w, h) / 7);
    for (int k = -h; k < w + h; k += step) {
        gfx_line_aa(s, k, 0, k + h, h, RGBA(255, 255, 255, 34));
        gfx_line_aa(s, k + h, 0, k, h, RGBA(255, 255, 255, 34));
    }
    surface_reset_clip(s);
    gfx_rounded_rect(s, ir.x, ir.y, ir.w, ir.h, MAX(3, rad - in / 2), RGBA(255, 255, 255, 110));
    /* centre medallion */
    double cx = w / 2.0, cy = h / 2.0, m = MIN(w, h);
    gfx_fill_circle_aa(s, cx, cy, m * 0.2, color_shade(base, -30));
    gfx_circle_aa(s, cx, cy, m * 0.2, MAX(1.5, m / 40.0), RGBA(255, 255, 255, 170));
    fn_star(s, cx, cy + m * 0.01, m * 0.13, 0.45, RGBA(255, 255, 255, 235));
    return s;
}

static void mm_free_cache(mm_t *g) {
    for (int i = 0; i < MM_NSYM; i++) {
        if (g->face[i]) surface_destroy(g->face[i]);
        g->face[i] = NULL;
    }
    if (g->back) surface_destroy(g->back);
    g->back = NULL;
    g->cache_w = g->cache_h = 0;
}

/* ======================================================================================
 * Layout
 * ==================================================================================== */
static void mm_card_size(mm_t *g, int *cw, int *ch) {
    int w = (MM_BOARD - (g->cols - 1) * MM_GAP) / g->cols;
    int h = (MM_BOARD - (g->rows - 1) * MM_GAP) / g->rows;
    if (h > w * 14 / 10) h = w * 14 / 10;
    if (w > h) w = h;
    *cw = w;
    *ch = h;
}

static rect_t mm_card_rect(mm_t *g, int i) {
    int cw, ch;
    mm_card_size(g, &cw, &ch);
    int tw = g->cols * cw + (g->cols - 1) * MM_GAP, th = g->rows * ch + (g->rows - 1) * MM_GAP;
    int ox = g->board_r.x + (MM_BOARD - tw) / 2, oy = g->board_r.y + (MM_BOARD - th) / 2;
    return rect_make(ox + (i % g->cols) * (cw + MM_GAP), oy + (i / g->cols) * (ch + MM_GAP), cw, ch);
}

static int mm_card_at(mm_t *g, int x, int y) {
    for (int i = 0; i < g->ncards; i++)
        if (rect_contains(mm_card_rect(g, i), x, y)) return i;
    return -1;
}

static void mm_ensure_cache(mm_t *g) {
    int cw, ch;
    mm_card_size(g, &cw, &ch);
    if (cw != g->cache_w || ch != g->cache_h) {
        mm_free_cache(g);
        g->cache_w = cw;
        g->cache_h = ch;
    }
    if (!g->back) g->back = mm_render_back(cw, ch);
}

static surface_t *mm_face(mm_t *g, int sym) {
    mm_ensure_cache(g);
    if (!g->face[sym]) g->face[sym] = mm_render_face(sym, g->cache_w, g->cache_h);
    return g->face[sym];
}

static void mm_inval_card(mm_t *g, int i) {
    if (i < 0 || i >= g->ncards) return;
    rect_t r = mm_card_rect(g, i);
    int e = r.w / 10 + 6;
    win_invalidate_rect(g->win, r.x - e, r.y - e, r.w + 2 * e, r.h + 2 * e);
}

static void mm_inval_stats(mm_t *g) {
    win_invalidate_rect(g->win, 0, MM_STATS_Y - 2, MM_W, MM_STATS_H + 4);
}

static void mm_inval_board(mm_t *g) {
    win_invalidate_rect(g->win, 0, MM_BOARD_Y - 12, MM_W, MM_H - MM_BOARD_Y + 12);
}

/* ======================================================================================
 * Game flow
 * ==================================================================================== */
static uint32_t mm_elapsed(mm_t *g) {
    if (!g->started) return 0;
    return (g->finished ? g->end_ms : sys_uptime_ms()) - g->start_ms;
}

static bool mm_animating(mm_t *g) {
    uint32_t now = sys_uptime_ms();
    for (int i = 0; i < g->ncards; i++) {
        if (now - g->flip_t0[i] < FLIP_MS + 30) return true;
        if (g->state[i] == CARD_MATCHED && (int32_t)(now - g->pop_t0[i]) < POP_MS + 30) return true;
        if ((int32_t)(now - g->wrong_t0[i]) < WRONG_MS + 30 && g->wrong_t0[i]) return true;
    }
    if (g->finished && (int32_t)(now - g->finish_t0) < 900) return true;
    return g->hide_pending || g->prerender < g->npairs;
}

static void mm_update_timer(mm_t *g) {
    int want = mm_animating(g) ? 16 : (g->started && !g->finished ? 250 : 0);
    if (want == g->timer_iv) return;
    if (g->timer > 0) win_kill_timer(g->win, g->timer);
    g->timer = want ? win_set_timer(g->win, (uint32_t)want) : 0;
    g->timer_iv = g->timer > 0 ? want : 0;
}

static void mm_load_records(mm_t *g) {
    char key[32];
    snprintf(key, sizeof key, "memory_moves_%d", g->level);
    g->best_moves = fn_score_get(key, 0);
    snprintf(key, sizeof key, "memory_time_%d", g->level);
    g->best_ms = fn_score_get(key, 0);
}

static void mm_new_game(mm_t *g) {
    const mm_level_t *lv = &levels[g->level];
    g->cols = lv->cols;
    g->rows = lv->rows;
    g->ncards = g->cols * g->rows;
    g->npairs = g->ncards / 2;
    /* pick npairs distinct symbols, then deal two of each and shuffle */
    int pool[MM_NSYM];
    for (int i = 0; i < MM_NSYM; i++) pool[i] = i;
    for (int i = MM_NSYM - 1; i > 0; i--) {
        int j = fn_rand_n(&g->rng, i + 1), t = pool[i];
        pool[i] = pool[j];
        pool[j] = t;
    }
    for (int i = 0; i < g->npairs; i++) {
        g->sym[2 * i] = g->sym[2 * i + 1] = (int8_t)pool[i];
        g->used[i] = (int8_t)pool[i];
    }
    g->prerender = 0;
    for (int i = g->ncards - 1; i > 0; i--) {
        int j = fn_rand_n(&g->rng, i + 1);
        int8_t t = g->sym[i];
        g->sym[i] = g->sym[j];
        g->sym[j] = t;
    }
    uint32_t old = sys_uptime_ms() - 100000;
    for (int i = 0; i < MM_MAXCARDS; i++) {
        g->state[i] = CARD_DOWN;
        g->flip_t0[i] = old;
        g->pop_t0[i] = old;
        g->wrong_t0[i] = 0;
    }
    g->nopen = 0;
    g->hide_pending = false;
    g->moves = g->found = 0;
    g->started = g->finished = false;
    g->rec_moves = g->rec_time = false;
    g->kb = CLAMP(g->kb, 0, g->ncards - 1);
    g->hover = -1;
    g->btn[1].hidden = true;
    mm_load_records(g);
    mm_update_timer(g);
    win_invalidate(g->win);
}

static void mm_set_level(mm_t *g, int level) {
    level = CLAMP(level, 0, 2);
    g->level = level;
    fn_score_set("memory_level", level);
    mm_new_game(g);
}

static void mm_flip(mm_t *g, int i, bool up) {
    uint32_t now = sys_uptime_ms();
    /* a card turned while still mid-flip continues from its current angle */
    uint32_t el = now - g->flip_t0[i];
    g->flip_t0[i] = el < FLIP_MS ? now - (FLIP_MS - el) : now;
    g->state[i] = up ? CARD_UP : CARD_DOWN;
    mm_inval_card(g, i);
}

static void mm_hide_open(mm_t *g) {
    for (int k = 0; k < g->nopen; k++) mm_flip(g, g->open[k], false);
    g->nopen = 0;
    g->hide_pending = false;
}

static void mm_finish(mm_t *g) {
    uint32_t now = sys_uptime_ms();
    g->finished = true;
    g->end_ms = now;
    g->finish_t0 = now + WIN_DELAY;
    int ms = (int)(g->end_ms - g->start_ms);
    char key[32];
    if (!g->best_moves || g->moves < g->best_moves) {
        g->rec_moves = g->best_moves != 0;
        g->best_moves = g->moves;
        snprintf(key, sizeof key, "memory_moves_%d", g->level);
        fn_score_set(key, g->moves);
    }
    if (!g->best_ms || ms < g->best_ms) {
        g->rec_time = g->best_ms != 0;
        g->best_ms = ms;
        snprintf(key, sizeof key, "memory_time_%d", g->level);
        fn_score_set(key, ms);
    }
    g->stars = g->moves * 10 <= g->npairs * 15 ? 3 : (g->moves * 10 <= g->npairs * 22 ? 2 : 1);
    g->btn[1].hidden = false;
    static const note_t tune[] = { { 523, 90 }, { 659, 90 }, { 784, 90 }, { 1047, 140 }, { 0, 40 }, { 784, 90 }, { 1047, 260 } };
    sys_play_notes(tune, ARRAY_SIZE(tune));
}

static void mm_pick(mm_t *g, int i) {
    if (g->finished || i < 0 || i >= g->ncards) return;
    if (g->state[i] != CARD_DOWN) return;
    if (g->nopen == 2) mm_hide_open(g);   /* impatient: turn the wrong pair back now */
    uint32_t now = sys_uptime_ms();
    if (!g->started) {
        g->started = true;
        g->start_ms = now;
    }
    mm_flip(g, i, true);
    g->open[g->nopen++] = i;
    sys_beep(880, 12);
    if (g->nopen == 2) {
        int a = g->open[0], b = g->open[1];
        g->moves++;
        if (g->sym[a] == g->sym[b]) {
            g->state[a] = g->state[b] = CARD_MATCHED;
            g->pop_t0[a] = g->pop_t0[b] = now + FLIP_MS;
            g->nopen = 0;
            g->found++;
            if (g->found == g->npairs) {
                mm_finish(g);
            } else {
                static const note_t ding[] = { { 0, FLIP_MS - 40 }, { 784, 70 }, { 1175, 120 } };
                sys_play_notes(ding, ARRAY_SIZE(ding));
            }
        } else {
            g->wrong_t0[a] = g->wrong_t0[b] = now + FLIP_MS;
            g->hide_pending = true;
            g->hide_due = now + FLIP_MS + SHOW_MS;
        }
    }
    mm_inval_stats(g);
    mm_update_timer(g);
}

/* ======================================================================================
 * Painting
 * ==================================================================================== */
static void mm_draw_check_badge(surface_t *s, int cx, int cy, int r) {
    gfx_fill_circle_aa(s, cx, cy, r + 1.5, COLOR_WHITE);
    gfx_fill_circle_aa(s, cx, cy, r, RGB(46, 160, 67));
    double k = r / 10.0;
    fn_stroke(s, cx - 4.6 * k, cy + 0.2 * k, cx - 1.4 * k, cy + 3.4 * k, MAX(1.6, 2.4 * k), COLOR_WHITE);
    fn_stroke(s, cx - 1.4 * k, cy + 3.4 * k, cx + 4.8 * k, cy - 3.4 * k, MAX(1.6, 2.4 * k), COLOR_WHITE);
}

static void mm_draw_card(mm_t *g, surface_t *s, int i, uint32_t now) {
    const theme_t *t = theme_get();
    rect_t r = mm_card_rect(g, i);
    if (!rect_overlaps(rect_inset(r, -(r.w / 10 + 6)), s->clip)) return;
    bool up = g->state[i] != CARD_DOWN;
    uint32_t el = now - g->flip_t0[i];
    double p = el < FLIP_MS ? (double)el / FLIP_MS : 1.0;
    /* p: 0 = flip just started, 1 = finished. During the first half the old side shows. */
    bool show_face = p < 0.5 ? !up : up;
    double sx = fabs(cos(p * M_PI));
    double lift = sin(p * M_PI) * r.h * 0.06;
    double scale = 1.0;
    if (g->state[i] == CARD_MATCHED) {
        int32_t pe = (int32_t)(now - g->pop_t0[i]);
        if (pe >= 0 && pe < POP_MS) scale = 1.0 + 0.09 * sin((double)pe / POP_MS * M_PI);
    }
    bool hovered = !g->finished && g->state[i] == CARD_DOWN && p >= 1 && (i == g->hover);
    if (hovered) lift = 2;
    int w = (int)lround(r.w * sx * scale), h = (int)lround(r.h * scale);
    if (w < 2) w = 2;
    int x = r.x + (r.w - w) / 2, y = r.y + (r.h - h) / 2 - (int)lround(lift);
    int rad = mm_radius(r.w, r.h);
    /* shadow */
    gfx_fill_rounded_rect(s, x, y + 3 + (int)lift, w, h, rad, RGBA(0, 0, 0, t->dark ? 80 : 30));
    surface_t *img = show_face ? mm_face(g, g->sym[i]) : (mm_ensure_cache(g), g->back);
    if (!img) return;
    if (w == img->w && h == img->h) gfx_blit_alpha(s, x, y, img, 255);
    else gfx_blit_scaled(s, x, y, w, h, img, 0, 0, img->w, img->h, true);
    if (hovered) gfx_fill_rounded_rect(s, x, y, w, h, rad, RGBA(255, 255, 255, 40));
    /* wrong pair: red flash */
    if (g->wrong_t0[i] && show_face) {
        int32_t we = (int32_t)(now - g->wrong_t0[i]);
        if (we >= 0 && we < WRONG_MS) {
            int a = (int)(150 * (1.0 - (double)we / WRONG_MS));
            gfx_rounded_rect(s, x, y, w, h, rad, RGBA(229, 57, 53, a + 80));
            gfx_rounded_rect(s, x + 1, y + 1, w - 2, h - 2, rad - 1, RGBA(229, 57, 53, a + 60));
            gfx_fill_rounded_rect(s, x, y, w, h, rad, RGBA(229, 57, 53, a / 4));
        }
    }
    /* matched: soft green glow, then a check badge */
    if (g->state[i] == CARD_MATCHED && p >= 1 && (int32_t)(now - g->pop_t0[i]) >= 0) {
        gfx_rounded_rect(s, x, y, w, h, rad, RGBA(46, 160, 67, 200));
        int br = MAX(6, MIN(r.w, r.h) / 9);
        mm_draw_check_badge(s, x + w - br - 3, y + br + 3, br);
    }
    /* keyboard cursor */
    if (g->kb_on && i == g->kb && !g->finished) {
        gfx_rounded_rect(s, r.x - 4, r.y - 4, r.w + 8, r.h + 8, rad + 3, t->accent);
        gfx_rounded_rect(s, r.x - 3, r.y - 3, r.w + 6, r.h + 6, rad + 2, t->accent);
    }
}

static void mm_paint_result(mm_t *g, surface_t *s, uint32_t now) {
    const theme_t *t = theme_get();
    int32_t el = (int32_t)(now - g->finish_t0);
    if (el < 0) return;
    double p = fn_ease_out(el / 300.0);
    rect_t area = rect_make(g->board_r.x - 8, g->board_r.y - 8, g->board_r.w + 16, g->board_r.h + 16);
    int cw = 300, ch = 236;
    rect_t c = fn_overlay_card(s, area, cw, ch, (int)(70 * p));
    c.y += (int)((1 - p) * 20);
    fn_text_c(s, c.x, c.y + 18, c.w, "All pairs found!", t->window_text, FONT_UI_TITLE);
    /* stars pop in one after the other */
    for (int k = 0; k < 3; k++) {
        double sp = fn_ease_out((el - 250 - k * 150) / 220.0);
        double cx = c.x + c.w / 2.0 + (k - 1) * 46, cy = c.y + 86;
        bool lit = k < g->stars;
        double rr = 18 * (lit ? (0.4 + 0.6 * sp) : 1);
        uint32_t col = lit ? RGB(255, 184, 28) : color_mix(t->panel_bg, t->window_text, 40);
        if (lit && sp <= 0) col = color_mix(t->panel_bg, t->window_text, 40), rr = 18;
        fn_star(s, cx, cy, rr, 0.47, col);
    }
    char buf[96], tb[24];
    fn_fmt_time(tb, sizeof tb, g->end_ms - g->start_ms);
    snprintf(buf, sizeof buf, "%d moves in %s", g->moves, tb);
    fn_text_c(s, c.x, c.y + 116, c.w, buf, t->window_text, FONT_UI_LARGE);
    if (g->rec_moves || g->rec_time) {
        snprintf(buf, sizeof buf, "New record: %s%s%s", g->rec_moves ? "fewest moves" : "",
                 g->rec_moves && g->rec_time ? " and " : "", g->rec_time ? "best time" : "");
        fn_text_c(s, c.x, c.y + 144, c.w, buf, RGB(46, 160, 67), FONT_UI_BOLD);
    } else {
        char bt[24];
        fn_fmt_time(bt, sizeof bt, (uint32_t)g->best_ms);
        snprintf(buf, sizeof buf, "Best: %d moves, %s", g->best_moves, bt);
        fn_text_c(s, c.x, c.y + 144, c.w, buf, fn_muted(), FONT_UI);
    }
    g->btn[1].r = rect_make(c.x + (c.w - 140) / 2, c.y + c.h - 52, 140, 34);
    fn_btn_draw(s, &g->btn[1]);
}

static void mm_paint(mm_t *g, surface_t *s) {
    const theme_t *t = theme_get();
    uint32_t now = sys_uptime_ms();
    fn_seg_draw(s, g->seg_r, level_names, 3, g->level, g->seg_hover);
    fn_btn_draw(s, &g->btn[0]);

    /* stats */
    int n = 4, gap = 10, cw = (MM_W - 2 * MM_PAD - (n - 1) * gap) / n;
    char buf[32];
    rect_t r = rect_make(MM_PAD, MM_STATS_Y, cw, MM_STATS_H);
    snprintf(buf, sizeof buf, "%d", g->moves);
    fn_stat_card(s, r, "Moves", buf, t->window_text, false);
    r.x += cw + gap;
    fn_fmt_time(buf, sizeof buf, mm_elapsed(g));
    fn_stat_card(s, r, "Time", buf, g->started && !g->finished ? t->accent : t->window_text, g->started && !g->finished);
    r.x += cw + gap;
    snprintf(buf, sizeof buf, "%d / %d", g->found, g->npairs);
    fn_stat_card(s, r, "Pairs", buf, g->found == g->npairs ? RGB(46, 160, 67) : t->window_text, false);
    r.x += cw + gap;
    r.w = MM_W - MM_PAD - r.x;
    if (g->best_moves) snprintf(buf, sizeof buf, "%d moves", g->best_moves);
    else snprintf(buf, sizeof buf, "-");
    fn_stat_card(s, r, "Best", buf, fn_muted(), false);

    /* board */
    rect_t br = g->board_r;
    uint32_t felt = t->dark ? color_mix(t->window_bg, COLOR_BLACK, 70) : color_mix(t->window_bg, t->window_border, 90);
    gfx_fill_rounded_rect(s, br.x - 8, br.y - 8, br.w + 16, br.h + 16, 14, felt);
    for (int i = 0; i < g->ncards; i++) mm_draw_card(g, s, i, now);

    const char *hint;
    if (g->finished) hint = "Press Enter or click Play Again for a new game";
    else if (g->kb_on) hint = "Arrows move, Space turns a card, 1-3 board size, N new game";
    else if (!g->started) hint = "Turn over two cards per move and find all matching pairs";
    else hint = "Click a card to turn it over";
    fn_text_c(s, 0, MM_HINT_Y + 2, MM_W, hint, fn_muted(), FONT_UI);

    if (g->finished) mm_paint_result(g, s, now);
}

/* ======================================================================================
 * Events
 * ==================================================================================== */
static void mm_set_hover(mm_t *g, int i) {
    if (i == g->hover) return;
    int old = g->hover;
    g->hover = i;
    mm_inval_card(g, old);
    mm_inval_card(g, i);
    win_set_cursor(g->win, i >= 0 && g->state[i] == CARD_DOWN && !g->finished ? CURSOR_HAND : CURSOR_ARROW);
}

static void mm_set_kb(mm_t *g, bool on) {
    if (g->kb_on == on) return;
    g->kb_on = on;
    mm_inval_board(g);
}

static void mm_mouse(mm_t *g, const event_t *ev) {
    bool used = false;
    int fired = fn_btns_event(g->win, g->btn, 2, ev, &used);
    if (fired == 0 || fired == 1) { mm_new_game(g); return; }
    if (used) return;
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int sh = fn_seg_hit(g->seg_r, 3, ev->x, ev->y);
        if (sh != g->seg_hover) {
            g->seg_hover = sh;
            win_invalidate_rect(g->win, g->seg_r.x, g->seg_r.y, g->seg_r.w, g->seg_r.h);
        }
        int c = g->finished ? -1 : mm_card_at(g, ev->x, ev->y);
        if (c >= 0) mm_set_kb(g, false);
        mm_set_hover(g, c);
        break;
    }
    case EV_MOUSE_LEAVE:
        mm_set_hover(g, -1);
        if (g->seg_hover >= 0) {
            g->seg_hover = -1;
            win_invalidate_rect(g->win, g->seg_r.x, g->seg_r.y, g->seg_r.w, g->seg_r.h);
        }
        break;
    case EV_MOUSE_DOWN: {
        if (ev->button != MOUSE_LEFT) break;
        int sg = fn_seg_hit(g->seg_r, 3, ev->x, ev->y);
        if (sg >= 0) {
            if (sg != g->level) mm_set_level(g, sg);
            break;
        }
        if (g->finished) break;
        int c = mm_card_at(g, ev->x, ev->y);
        if (c >= 0) {
            mm_pick(g, c);
            win_set_cursor(g->win, CURSOR_ARROW);
        }
        break;
    }
    default:
        break;
    }
}

static void mm_key(mm_t *g, const event_t *ev) {
    int k = ev->key;
    if (k == KEY_F2 || k == 'n') { mm_new_game(g); return; }
    if (k >= '1' && k <= '3') { mm_set_level(g, k - '1'); return; }
    if (g->finished) {
        if (k == KEY_ENTER || k == KEY_SPACE) mm_new_game(g);
        return;
    }
    int dx = 0, dy = 0;
    if (k == KEY_LEFT) dx = -1;
    else if (k == KEY_RIGHT) dx = 1;
    else if (k == KEY_UP) dy = -1;
    else if (k == KEY_DOWN) dy = 1;
    if (dx || dy) {
        if (g->kb_on) {
            int x = CLAMP(g->kb % g->cols + dx, 0, g->cols - 1), y = CLAMP(g->kb / g->cols + dy, 0, g->rows - 1);
            mm_inval_card(g, g->kb);
            g->kb = y * g->cols + x;
            mm_inval_card(g, g->kb);
        } else {
            mm_set_kb(g, true);
        }
        return;
    }
    if (k == KEY_SPACE || k == KEY_ENTER) {
        if (!g->kb_on) { mm_set_kb(g, true); return; }
        mm_pick(g, g->kb);
    }
}

static void mm_timer(mm_t *g) {
    /* build the card faces of this game in the background, one per tick, so turning a
       card over never has to wait for one */
    if (g->prerender < g->npairs) mm_face(g, g->used[g->prerender++]);
    uint32_t now = sys_uptime_ms();
    if (g->hide_pending && (int32_t)(now - g->hide_due) >= 0) mm_hide_open(g);
    for (int i = 0; i < g->ncards; i++) {
        bool anim = now - g->flip_t0[i] < FLIP_MS + 30 ||
                    (g->state[i] == CARD_MATCHED && (int32_t)(now - g->pop_t0[i]) > -40 &&
                     (int32_t)(now - g->pop_t0[i]) < POP_MS + 30) ||
                    (g->wrong_t0[i] && (int32_t)(now - g->wrong_t0[i]) > -40 &&
                     (int32_t)(now - g->wrong_t0[i]) < WRONG_MS + 30);
        if (anim) mm_inval_card(g, i);
    }
    if (g->finished && (int32_t)(now - g->finish_t0) > -20 && (int32_t)(now - g->finish_t0) < 960) mm_inval_board(g);
    if (g->started && !g->finished) mm_inval_stats(g);
    mm_update_timer(g);
}

static void mm_event(window_t *win, const event_t *ev) {
    mm_t *g = win_get_userdata(win);
    if (!g) return;
    switch (ev->type) {
    case EV_PAINT:
        mm_paint(g, win_surface(win));
        break;
    case EV_TIMER:
        mm_timer(g);
        break;
    case EV_MOUSE_MOVE: case EV_MOUSE_DOWN: case EV_MOUSE_UP: case EV_MOUSE_LEAVE: case EV_DOUBLE_CLICK:
        mm_mouse(g, ev);
        break;
    case EV_KEY_DOWN:
        mm_key(g, ev);
        break;
    case EV_BLUR:
        fn_btns_reset(win, g->btn, 2);
        break;
    case EV_THEME:
        mm_free_cache(g);   /* the card back follows the accent colour */
        g->prerender = 0;
        mm_update_timer(g);
        win_invalidate(win);
        break;
    case EV_CLOSE:
        mm_free_cache(g);
        free(g);
        win_set_userdata(win, NULL);
        break;
    default:
        break;
    }
}

static void mm_launch(const char *arg) {
    (void)arg;
    mm_t *g = calloc(1, sizeof *g);
    if (!g) return;
    fn_rng_seed(&g->rng, (uint32_t)(uintptr_t)g);
    g->hover = g->seg_hover = -1;
    g->seg_r = rect_make(MM_PAD, MM_HDR_Y, 300, MM_HDR_H);
    g->board_r = rect_make(MM_PAD, MM_BOARD_Y, MM_BOARD, MM_BOARD);
    g->btn[0] = (fn_btn_t){ .r = rect_make(MM_W - MM_PAD - 116, MM_HDR_Y, 116, MM_HDR_H), .label = "New Game" };
    g->btn[1] = (fn_btn_t){ .label = "Play Again", .primary = true, .hidden = true };
    g->win = win_create("Memory Match", MM_W, MM_H, WIN_CENTER);
    if (!g->win) { free(g); return; }
    win_set_userdata(g->win, g);
    win_set_event_handler(g->win, mm_event);
    g->level = CLAMP(fn_score_get("memory_level", 1), 0, 2);
    if (arg && *arg) {
        if (!strcasecmp(arg, "easy")) g->level = 0;
        else if (!strcasecmp(arg, "normal")) g->level = 1;
        else if (!strcasecmp(arg, "hard")) g->level = 2;
    }
    mm_new_game(g);
}

/* Icon: two cards - a blue face-down card behind, a white card with a red heart in front. */
static void mm_icon(surface_t *s, int x, int y, int size) {
    double u = size / 48.0;
    /* back card, tilted */
    {
        point_t p[4];
        double cx = x + 18 * u, cy = y + 21 * u, hw = 12 * u, hh = 16 * u, a = -0.24;
        double ca = cos(a), sa = sin(a);
        double px[4] = { -hw, hw, hw, -hw }, py[4] = { -hh, -hh, hh, hh };
        for (int i = 0; i < 4; i++) {
            p[i].x = (int)lround(cx + px[i] * ca - py[i] * sa);
            p[i].y = (int)lround(cy + px[i] * sa + py[i] * ca);
        }
        gfx_fill_polygon(s, p, 4, RGB(40, 92, 200));
        for (int i = 0; i < 4; i++) {
            int j = (i + 1) % 4;
            gfx_line_aa(s, p[i].x, p[i].y, p[j].x, p[j].y, RGB(40, 92, 200));
        }
        double ix = 8 * u, iy = 12 * u;
        double qx[4] = { -ix, ix, ix, -ix }, qy[4] = { -iy, -iy, iy, iy };
        for (int i = 0; i < 4; i++) {
            int j = (i + 1) % 4;
            gfx_line_aa(s, cx + qx[i] * ca - qy[i] * sa, cy + qx[i] * sa + qy[i] * ca,
                        cx + qx[j] * ca - qy[j] * sa, cy + qx[j] * sa + qy[j] * ca, RGBA(255, 255, 255, 150));
        }
        fn_star(s, cx, cy, 5 * u, 0.45, RGBA(255, 255, 255, 230));
    }
    /* front card */
    int fx = x + (int)(20 * u), fy = y + (int)(10 * u), fw = (int)(24 * u), fh = (int)(33 * u);
    gfx_fill_rounded_rect(s, fx + (int)u, fy + (int)(2 * u), fw, fh, (int)(4 * u), RGBA(0, 0, 0, 60));
    gfx_fill_rounded_rect(s, fx, fy, fw, fh, (int)(4 * u), RGB(196, 202, 214));
    gfx_fill_rounded_rect(s, fx + (int)u, fy + (int)u, fw - (int)(2 * u), fh - (int)(2 * u), (int)(3 * u), COLOR_WHITE);
    mm_draw_symbol(s, 0, fx + fw / 2.0, fy + fh / 2.0, 9 * u);
}

REGISTER_APP(.name = "Memory Match", .category = CAT_GAMES,
             .description = "Turn over cards and find the matching pairs",
             .launch = mm_launch, .draw_icon = mm_icon);
