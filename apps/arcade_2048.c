/*
 * arcade_2048.c - 2048 (Games).
 *
 * Slide the tiles on a 4x4 board; two equal tiles that collide merge into
 * one with their sum. Standard rules: every tile merges at most once per
 * move, merging starts at the edge the tiles move towards ([2 2 2 2] -> [4 4]
 * and [2 2 4] -> [4 4], not [8]), a move that changes nothing does not count
 * and spawns no tile, otherwise a new 2 (90%) or 4 (10%) appears on a random
 * empty cell. Reaching 2048 wins (then you may keep going); no empty cell
 * and no possible merge loses.
 *
 * Extras: slide and merge animations (tiles glide, merged tiles pop, new
 * tiles grow in), one-step undo, mouse drags on the board work as swipes,
 * score with "+N" pop-ups, persistent high score, best tile and move count.
 * Tile numbers are drawn with an anti-aliased, bilinearly scaled rendering
 * of the bold UI font so they are large and readable on every tile size.
 *
 * Keys: arrows / WASD slide, U / Backspace / Ctrl+Z undo, F2 / N new game,
 * Enter keeps going after a win or restarts after a loss.
 * Launch arguments: "demo" lets a small expectimax AI play (attract mode,
 * tests); "tiles" shows one tile of every value from 2 to 65536; "test-win"
 * and "test-lose" load boards one move (Left / Right) from winning / losing.
 */
#include "arcade_common.h"

#define N 4
#define TILE 96
#define GAP 12
#define BS (N * TILE + (N + 1) * GAP)
#define BX 16
#define BY 16
#define PX (BX + BS + 20)
#define PW 164
#define CW (PX + PW + 16)
#define CH (BY + BS + 16)

#define SLIDE_MS 110
#define POP_MS 150
#define ANIM_MS (SLIDE_MS + POP_MS)
#define POPUP_MS 750
#define WIN_EXP 11              /* 2^11 = 2048 */
#define MAX_EXP 17
#define OVERLAY_DELAY 450

/* overlay card (centred on the board) */
#define OV_W 300
#define OV_H 178
#define OV_X (BX + (BS - OV_W) / 2)
#define OV_Y (BY + (BS - OV_H) / 2)

enum { D_UP, D_RIGHT, D_DOWN, D_LEFT };
enum { ST_PLAY, ST_WON, ST_OVER };
enum { BTN_UNDO, BTN_NEW, BTN_OV1, BTN_OV2, NBTN };

typedef struct { int8_t fr, fc, tr, tc; uint8_t v; } slide_t;

typedef struct {
    window_t *win;
    arcade_rng_t rng;
    uint8_t grid[N][N];            /* exponent: 0 = empty, 1 = 2, 2 = 4, ... */
    /* undo (one step) */
    uint8_t prev[N][N];
    int prev_score, prev_moves;
    bool can_undo;
    int score, best, saved_best, moves;
    bool new_best;
    int state;
    bool won_once;                 /* 2048 reached: don't show "You win" again */
    uint32_t over_ms;              /* time since ST_WON / ST_OVER began */
    /* animation of the last move */
    slide_t sl[N * N];
    int nsl;
    uint8_t merged[N][N];
    int new_r[2], new_c[2], nnew;
    uint32_t anim_ms;
    uint32_t last_ms;
    /* "+N" pop-up next to the score */
    int pop_value;
    uint32_t pop_ms;
    /* mouse swipe */
    bool dragging;
    int drag_x, drag_y;
    /* demo AI */
    bool ai;
    uint32_t ai_acc;
    arcade_btn_t btn[NBTN];
    int hover, press;
} game_t;

/* ---- tile colours ---------------------------------------------------------------------- */
static void tile_style(int e, uint32_t *bg, uint32_t *fg) {
    static const uint32_t BG[] = {
        0,
        RGB(238, 241, 248),   /* 2 */
        RGB(210, 222, 250),   /* 4 */
        RGB(118, 182, 255),   /* 8 */
        RGB(62, 136, 244),    /* 16 */
        RGB(34, 178, 192),    /* 32 */
        RGB(48, 172, 102),    /* 64 */
        RGB(244, 180, 50),    /* 128 */
        RGB(244, 134, 50),    /* 256 */
        RGB(236, 84, 70),     /* 512 */
        RGB(222, 62, 142),    /* 1024 */
        RGB(140, 82, 240),    /* 2048 */
    };
    if (e <= 0) { *bg = 0; *fg = 0; return; }
    if (e < ARRAY_SIZE(BG)) *bg = BG[e];
    else *bg = color_mix(RGB(46, 50, 70), RGB(26, 28, 40), MIN(255, (e - 12) * 60));   /* 4096+ */
    *fg = e <= 2 ? RGB(62, 70, 94) : COLOR_WHITE;
}

/* ---- big numbers: bold UI font rendered once to an alpha mask, drawn scaled (bilinear) ---- */
typedef struct { int w, h; uint8_t *a; } mask_t;
static mask_t g_masks[MAX_EXP + 1];   /* immutable glyph cache shared by all windows */

static const mask_t *get_mask(int e) {
    mask_t *m = &g_masks[e];
    if (m->a || e < 1 || e > MAX_EXP) return m;
    char str[16];
    snprintf(str, sizeof str, "%u", 1u << e);
    const font_t *f = FONT_UI_TITLE;
    int sw = font_text_width(f, str) + 4, sh = font_height(f);
    surface_t *s = surface_create(sw, sh);          /* opaque black */
    if (!s) return m;
    gfx_text(s, 2, 0, str, COLOR_WHITE, f);
    /* crop to the ink so numbers centre optically */
    int x0 = sw, y0 = sh, x1 = -1, y1 = -1;
    for (int y = 0; y < sh; y++) {
        uint32_t *row = surface_row(s, y);
        for (int x = 0; x < sw; x++)
            if (COLOR_R(row[x]) > 8) {
                x0 = MIN(x0, x); x1 = MAX(x1, x); y0 = MIN(y0, y); y1 = MAX(y1, y);
            }
    }
    if (x1 >= x0) {
        m->w = x1 - x0 + 1;
        m->h = y1 - y0 + 1;
        m->a = malloc((size_t)(m->w * m->h));
        if (m->a)
            for (int y = 0; y < m->h; y++) {
                uint32_t *row = surface_row(s, y0 + y);
                for (int x = 0; x < m->w; x++) m->a[y * m->w + x] = (uint8_t)COLOR_R(row[x0 + x]);
            }
    }
    surface_destroy(s);
    return m;
}

static inline int mask_px(const mask_t *m, int x, int y) {
    if (x < 0 || y < 0 || x >= m->w || y >= m->h) return 0;
    return m->a[y * m->w + x];
}

/* Draw mask m centred at (cx, cy), scaled by `scale`, in colour col (alpha honoured).
   Upscaled edges get a contrast boost so they stay crisp instead of blurry. */
static void draw_mask(surface_t *s, const mask_t *m, double cx, double cy, double scale, uint32_t col) {
    if (!m->a || scale < 0.05) return;
    double w = m->w * scale, h = m->h * scale;
    double x0 = cx - w / 2, y0 = cy - h / 2;
    int ix0 = (int)floor(x0) - 1, iy0 = (int)floor(y0) - 1;
    int ix1 = (int)ceil(x0 + w) + 1, iy1 = (int)ceil(y0 + h) + 1;
    rect_t r = rect_intersect(rect_make(ix0, iy0, ix1 - ix0, iy1 - iy0), s->clip);
    if (rect_empty(r)) return;
    double k = scale > 1.0 ? MIN(1.0 + (scale - 1.0) * 1.1, 2.2) : 1.0;
    int cr = COLOR_R(col), cg = COLOR_G(col), cb = COLOR_B(col), ca = COLOR_A(col);
    double inv = 1.0 / scale;
    for (int py = r.y; py < r.y + r.h; py++) {
        double v = (py + 0.5 - y0) * inv - 0.5;
        int vy = (int)floor(v);
        double fy = v - vy;
        uint32_t *row = surface_row(s, py);
        for (int px = r.x; px < r.x + r.w; px++) {
            double u = (px + 0.5 - x0) * inv - 0.5;
            int ux = (int)floor(u);
            double fx = u - ux;
            double top = mask_px(m, ux, vy) * (1 - fx) + mask_px(m, ux + 1, vy) * fx;
            double bot = mask_px(m, ux, vy + 1) * (1 - fx) + mask_px(m, ux + 1, vy + 1) * fx;
            double a = top * (1 - fy) + bot * fy;
            if (k > 1.0) a = (a - 127.5) * k + 127.5;
            if (a <= 0.5) continue;
            if (a > 255) a = 255;
            int ai = (int)(a * ca / 255.0 + 0.5);
            if (ai <= 0) continue;
            uint32_t d = row[px];
            int dr = COLOR_R(d), dg = COLOR_G(d), db = COLOR_B(d);
            dr += (cr - dr) * ai / 255;
            dg += (cg - dg) * ai / 255;
            db += (cb - db) * ai / 255;
            row[px] = RGB(dr, dg, db);
        }
    }
}

/* ---- rules ------------------------------------------------------------------------------- */
static void line_cell(int dir, int i, int k, int *r, int *c) {
    switch (dir) {
    case D_LEFT:  *r = i;         *c = k;         break;
    case D_RIGHT: *r = i;         *c = N - 1 - k; break;
    case D_UP:    *r = k;         *c = i;         break;
    default:      *r = N - 1 - k; *c = i;         break;
    }
}

/* Slide `in` towards dir into `out`. Returns true if any tile moved. *gain = points scored.
   sl/nsl (optional) receive every tile's source and destination, merged (optional) the
   cells holding a freshly merged tile. */
static bool slide_board(const uint8_t in[N][N], uint8_t out[N][N], int dir, int *gain,
                        slide_t *sl, int *nsl, uint8_t merged[N][N]) {
    uint8_t mg[N][N];
    memset(mg, 0, sizeof mg);
    memset(out, 0, sizeof(uint8_t) * N * N);
    bool moved = false;
    int pts = 0, n = 0;
    for (int i = 0; i < N; i++) {
        int pos = -1;                  /* index along the line of the last placed tile */
        for (int k = 0; k < N; k++) {
            int r, c, tr = 0, tc = 0;
            line_cell(dir, i, k, &r, &c);
            int v = in[r][c];
            if (!v) continue;
            if (pos >= 0) line_cell(dir, i, pos, &tr, &tc);
            if (pos >= 0 && out[tr][tc] == v && !mg[tr][tc] && v < MAX_EXP) {
                out[tr][tc] = (uint8_t)(v + 1);
                mg[tr][tc] = 1;
                pts += 1 << (v + 1);
            } else {
                pos++;
                line_cell(dir, i, pos, &tr, &tc);
                out[tr][tc] = (uint8_t)v;
            }
            if (tr != r || tc != c) moved = true;
            if (sl) sl[n++] = (slide_t){ (int8_t)r, (int8_t)c, (int8_t)tr, (int8_t)tc, (uint8_t)v };
        }
    }
    if (gain) *gain = pts;
    if (nsl) *nsl = n;
    if (merged) memcpy(merged, mg, sizeof mg);
    return moved;
}

static int empty_count(const uint8_t b[N][N]) {
    int n = 0;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            if (!b[r][c]) n++;
    return n;
}

static bool can_move(const uint8_t b[N][N]) {
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            if (!b[r][c]) return true;
            if (c + 1 < N && b[r][c] == b[r][c + 1]) return true;
            if (r + 1 < N && b[r][c] == b[r + 1][c]) return true;
        }
    return false;
}

static int max_tile(const uint8_t b[N][N]) {
    int m = 0;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) m = MAX(m, b[r][c]);
    return m;
}

/* put a 2 (90%) or 4 (10%) on a random empty cell; records it for the grow-in animation */
static void spawn_tile(game_t *g) {
    int n = empty_count(g->grid);
    if (!n || g->nnew >= 2) return;
    int k = arcade_rand_n(&g->rng, n);
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            if (!g->grid[r][c] && k-- == 0) {
                g->grid[r][c] = arcade_rand_n(&g->rng, 10) == 0 ? 2 : 1;
                g->new_r[g->nnew] = r;
                g->new_c[g->nnew] = c;
                g->nnew++;
                return;
            }
}

static void inv_all(game_t *g) { win_invalidate(g->win); }
static void inv_board(game_t *g) { win_invalidate_rect(g->win, BX - 8, BY - 8, BS + 16, BS + 16); }
static void inv_panel(game_t *g) { win_invalidate_rect(g->win, PX - 4, 0, CW - PX + 4, CH); }

/* Another 2048 window may have stored a higher score meanwhile: never overwrite it. */
static void save_best(game_t *g) {
    if (g->best <= g->saved_best) return;
    int stored = arcade_hiscore_get("2048");
    if (g->best > stored) arcade_hiscore_set("2048", g->best);
    g->saved_best = MAX(g->best, stored);
}

static void add_score(game_t *g, int pts) {
    if (pts <= 0) return;
    g->score += pts;
    g->pop_value = pts;
    g->pop_ms = 0;
    if (g->score > g->best) {
        g->best = g->score;
        g->new_best = true;
    }
}

static void finish_anim(game_t *g) {
    if (g->anim_ms < ANIM_MS) {
        g->anim_ms = ANIM_MS;
        inv_board(g);
    }
}

static void new_game(game_t *g) {
    save_best(g);
    int stored = arcade_hiscore_get("2048");
    g->best = MAX(g->best, stored);
    g->saved_best = MAX(g->saved_best, stored);
    memset(g->grid, 0, sizeof g->grid);
    memset(g->merged, 0, sizeof g->merged);
    g->score = g->moves = 0;
    g->new_best = false;
    g->state = ST_PLAY;
    g->won_once = false;
    g->can_undo = false;
    g->nsl = 0;
    g->nnew = 0;
    g->pop_ms = POPUP_MS;
    spawn_tile(g);
    spawn_tile(g);
    g->anim_ms = SLIDE_MS;       /* only the grow-in part */
    inv_all(g);
}

static void do_move(game_t *g, int dir) {
    finish_anim(g);
    if (g->state != ST_PLAY) return;
    uint8_t out[N][N], mg[N][N];
    slide_t sl[N * N];
    int gain = 0, nsl = 0;
    if (!slide_board(g->grid, out, dir, &gain, sl, &nsl, mg)) return;   /* nothing moves: no turn */
    memcpy(g->prev, g->grid, sizeof g->grid);
    g->prev_score = g->score;
    g->prev_moves = g->moves;
    g->can_undo = true;
    int old_max = max_tile(g->grid);
    memcpy(g->grid, out, sizeof out);
    memcpy(g->merged, mg, sizeof mg);
    memcpy(g->sl, sl, sizeof sl);
    g->nsl = nsl;
    g->nnew = 0;
    g->moves++;
    add_score(g, gain);
    spawn_tile(g);
    g->anim_ms = 0;
    int m = max_tile(g->grid);
    if (m >= WIN_EXP && !g->won_once) {
        g->won_once = true;
        g->state = ST_WON;
        g->over_ms = 0;
        save_best(g);
        arcade_sound_fanfare();
        klog("2048: reached 2048 in %d moves, score %d", g->moves, g->score);
    } else if (!can_move(g->grid)) {
        g->state = ST_OVER;
        g->over_ms = 0;
        save_best(g);
        arcade_sound_gameover();
        klog("2048: game over, score %d, best tile %u, %d moves", g->score, 1u << m, g->moves);
    } else if (m > old_max && m >= 7) {
        sys_beep(330 + 60 * m, 40);          /* new best tile (128+) */
    }
    inv_all(g);
}

static void undo(game_t *g) {
    if (!g->can_undo) return;
    finish_anim(g);
    memcpy(g->grid, g->prev, sizeof g->grid);
    g->score = g->prev_score;
    g->moves = g->prev_moves;
    g->can_undo = false;
    g->state = ST_PLAY;
    g->won_once = max_tile(g->grid) >= WIN_EXP;
    g->nsl = 0;
    g->nnew = 0;
    memset(g->merged, 0, sizeof g->merged);
    g->anim_ms = ANIM_MS;
    g->pop_ms = POPUP_MS;
    inv_all(g);
}

static void keep_going(game_t *g) {
    if (g->state != ST_WON) return;
    g->state = ST_PLAY;
    inv_all(g);
}

/* ---- demo AI: depth-2 expectimax over "snake" weights ------------------------------------------ */
static double ai_score(const uint8_t b[N][N]) {
    static const double W[N][N] = {
        { 32768, 16384, 8192, 4096 },
        { 256, 512, 1024, 2048 },
        { 128, 64, 32, 16 },
        { 1, 2, 4, 8 },
    };
    double s = 0;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            if (b[r][c]) s += W[r][c] * (double)(1u << b[r][c]);
    return s + empty_count(b) * 2000.0;
}

static double ai_best_after_spawn(const uint8_t b[N][N]) {
    double total = 0;
    int cnt = 0;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            if (b[r][c]) continue;
            uint8_t t[N][N], o[N][N];
            memcpy(t, b, sizeof t);
            t[r][c] = 1;
            double best = -1;
            for (int d = 0; d < 4; d++)
                if (slide_board(t, o, d, NULL, NULL, NULL, NULL)) best = MAX(best, ai_score(o));
            total += best < 0 ? 0 : best;
            cnt++;
        }
    return cnt ? total / cnt : ai_score(b);
}

static int ai_choose(const game_t *g) {
    int best_d = -1;
    double best = -1;
    for (int d = 0; d < 4; d++) {
        uint8_t o[N][N];
        if (!slide_board(g->grid, o, d, NULL, NULL, NULL, NULL)) continue;
        double v = ai_best_after_spawn(o);
        if (v > best) { best = v; best_d = d; }
    }
    return best_d;
}

/* ---- timer ------------------------------------------------------------------------------- */
static void tick(game_t *g) {
    uint32_t now = sys_uptime_ms();
    uint32_t dt = now - g->last_ms;
    g->last_ms = now;
    if (dt > 100) dt = 100;
    if (g->anim_ms < ANIM_MS) {
        g->anim_ms += dt;
        inv_board(g);
    }
    if (g->pop_ms < POPUP_MS) {
        g->pop_ms += dt;
        inv_panel(g);
    }
    if (g->state != ST_PLAY && g->over_ms < OVERLAY_DELAY + 100) {
        bool was = g->over_ms >= OVERLAY_DELAY;
        g->over_ms += dt;
        if (!was && g->over_ms >= OVERLAY_DELAY) inv_all(g);
    }
    if (g->ai) {
        g->ai_acc += dt;
        if (g->state == ST_PLAY && g->ai_acc >= 140) {
            g->ai_acc = 0;
            int d = ai_choose(g);
            if (d >= 0) do_move(g, d);
        } else if (g->state == ST_WON && g->ai_acc >= 2500) {
            g->ai_acc = 0;
            keep_going(g);
        } else if (g->state == ST_OVER && g->ai_acc >= 4000) {
            g->ai_acc = 0;
            new_game(g);
        }
    }
}

/* ---- drawing ----------------------------------------------------------------------------- */
static double cell_cx(int c) { return BX + GAP + c * (TILE + GAP) + TILE / 2.0; }
static double cell_cy(int r) { return BY + GAP + r * (TILE + GAP) + TILE / 2.0; }

static double ease_out(double p) { p = 1 - p; return 1 - p * p * p; }

static void draw_tile(surface_t *s, double cx, double cy, double scale, int e) {
    if (e <= 0 || scale <= 0.02) return;
    uint32_t bg, fg;
    tile_style(e, &bg, &fg);
    int sz = (int)lround(TILE * scale);
    int x = (int)lround(cx - sz / 2.0), y = (int)lround(cy - sz / 2.0);
    int rad = MAX(2, (int)lround(9 * scale));
    if (e >= 10) {                                        /* glow for 1024 and up */
        int ga = e >= WIN_EXP ? 30 : 20;
        for (int i = 4; i >= 1; i--)
            gfx_fill_rounded_rect(s, x - i * 2, y - i * 2, sz + i * 4, sz + i * 4, rad + i * 2,
                                  color_alpha(bg, ga - i * 4));
    }
    gfx_fill_rounded_rect(s, x, y + 2, sz, sz, rad, color_shade(bg, -48));
    gfx_fill_rounded_gradient_v(s, x, y, sz, sz, rad, color_shade(bg, 16), color_shade(bg, -10));
    gfx_fill_rect(s, x + rad, y + 1, sz - 2 * rad, 1, color_alpha(COLOR_WHITE, 70));
    const mask_t *m = get_mask(e);
    if (!m->a) return;
    double ts = MIN(2.0, (TILE - 22.0) / m->w) * scale;
    if (fg == COLOR_WHITE) draw_mask(s, m, cx + 1, cy + 1.5, ts, RGBA(0, 0, 0, 60));
    draw_mask(s, m, cx, cy, ts, fg);
}

static void board_colors(uint32_t *board, uint32_t *slot) {
    const theme_t *t = theme_get();
    *board = t->dark ? RGB(44, 48, 64) : RGB(182, 191, 212);
    *slot = t->dark ? RGB(58, 63, 82) : RGB(205, 212, 228);
}

static bool overlay_shown(const game_t *g) {
    return g->state != ST_PLAY && g->over_ms >= OVERLAY_DELAY;
}

static void update_buttons(game_t *g) {
    bool ov = overlay_shown(g);
    g->btn[BTN_UNDO].disabled = !g->can_undo;
    g->btn[BTN_NEW].primary = g->state == ST_OVER;
    g->btn[BTN_OV1].visible = ov;
    g->btn[BTN_OV2].visible = ov && (g->state == ST_WON || g->can_undo);
    if (g->state == ST_WON) {
        g->btn[BTN_OV1].label = "Keep going";
        g->btn[BTN_OV2].label = "New game";
    } else {
        g->btn[BTN_OV1].label = "Try again";
        g->btn[BTN_OV2].label = "Undo";
    }
}

static void draw_board(game_t *g, surface_t *s) {
    uint32_t board, slot;
    board_colors(&board, &slot);
    gfx_shadow(s, BX, BY, BS, BS, 12, 6, 40);
    gfx_fill_rounded_rect(s, BX, BY, BS, BS, 12, board);
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            gfx_fill_rounded_rect(s, (int)cell_cx(c) - TILE / 2, (int)cell_cy(r) - TILE / 2, TILE, TILE, 9, slot);

    rect_t old_clip = s->clip;
    s->clip = rect_intersect(old_clip, rect_make(BX - 8, BY - 8, BS + 16, BS + 16));
    if (g->anim_ms < SLIDE_MS && g->nsl) {
        double p = ease_out((double)g->anim_ms / SLIDE_MS);
        for (int pass = 0; pass < 2; pass++)          /* resting tiles first, moving ones on top */
            for (int i = 0; i < g->nsl; i++) {
                const slide_t *t = &g->sl[i];
                bool moving = t->fr != t->tr || t->fc != t->tc;
                if (moving != (pass == 1)) continue;
                double cx = cell_cx(t->fc) + (cell_cx(t->tc) - cell_cx(t->fc)) * p;
                double cy = cell_cy(t->fr) + (cell_cy(t->tr) - cell_cy(t->fr)) * p;
                draw_tile(s, cx, cy, 1.0, t->v);
            }
    } else {
        bool anim = g->anim_ms < ANIM_MS;
        double p = anim ? (double)(g->anim_ms - SLIDE_MS) / POP_MS : 1.0;
        p = CLAMP(p, 0.0, 1.0);
        for (int pass = 0; pass < 2; pass++)          /* merged (enlarged) tiles on top */
            for (int r = 0; r < N; r++)
                for (int c = 0; c < N; c++) {
                    int e = g->grid[r][c];
                    if (!e) continue;
                    bool mg = anim && g->merged[r][c];
                    if (mg != (pass == 1)) continue;
                    double sc = 1.0;
                    if (mg) sc = 1.0 + 0.17 * sin(M_PI * p);
                    if (anim)
                        for (int i = 0; i < g->nnew; i++)
                            if (g->new_r[i] == r && g->new_c[i] == c) sc = 0.25 + 0.75 * ease_out(p);
                    draw_tile(s, cell_cx(c), cell_cy(r), sc, e);
                }
    }
    s->clip = old_clip;

    if (overlay_shown(g)) {
        char l1[48], l2[48];
        rect_t area = rect_make(BX, BY, BS, BS);
        if (g->state == ST_WON) {
            uint32_t bg, fg;
            tile_style(WIN_EXP, &bg, &fg);
            snprintf(l1, sizeof l1, "You made the 2048 tile in %d move%s", g->moves, g->moves == 1 ? "" : "s");
            arcade_overlay(s, area, 90, OV_W, OV_H, "You win!", bg, l1, "Keep going for a higher score?");
        } else {
            snprintf(l1, sizeof l1, g->new_best ? "New high score: %d" : "Score %d", g->score);
            snprintf(l2, sizeof l2, "Best tile %u", 1u << max_tile(g->grid));
            arcade_overlay(s, area, 110, OV_W, OV_H, "Game over", 0, l1, l2);
        }
        update_buttons(g);
        arcade_btn_draw(s, &g->btn[BTN_OV1], BTN_OV1, g->hover, g->press);
        arcade_btn_draw(s, &g->btn[BTN_OV2], BTN_OV2, g->hover, g->press);
    }
}

static void draw_panel(game_t *g, surface_t *s) {
    const theme_t *t = theme_get();
    char buf[32];
    int x = PX, y = BY;
    const app_desc_t *app = app_find("2048");
    if (app) app_draw_icon(s, app, x, y, 32);
    gfx_text(s, x + 40, y + 5, "2048", t->window_text, FONT_UI_LARGE);
    y += 46;
    snprintf(buf, sizeof buf, "%d", g->score);
    arcade_card(s, x, y, PW, 66, "SCORE", buf, FONT_UI_TITLE, 0);
    if (g->pop_ms < POPUP_MS && g->pop_value > 0) {
        double p = (double)g->pop_ms / POPUP_MS;
        int a = p < 0.5 ? 255 : (int)(255 * (1 - (p - 0.5) * 2));
        snprintf(buf, sizeof buf, "+%d", g->pop_value);
        int tw = font_text_width(FONT_UI_BOLD, buf);
        int py = y + 34 - (int)(p * 22);
        gfx_text(s, x + PW - 12 - tw, py, buf, color_alpha(t->accent, CLAMP(a, 0, 255)), FONT_UI_BOLD);
    }
    y += 74;
    snprintf(buf, sizeof buf, "%d", g->best);
    arcade_card(s, x, y, PW, 66, "HIGH SCORE", buf, FONT_UI_TITLE, g->new_best ? t->accent : 0);
    y += 74;
    int hw = 68;
    snprintf(buf, sizeof buf, "%d", g->moves);
    arcade_card(s, x, y, hw, 56, "MOVES", buf, FONT_UI_LARGE, 0);
    int mt = max_tile(g->grid);
    snprintf(buf, sizeof buf, "%u", mt ? 1u << mt : 0u);
    uint32_t bg, fg;
    tile_style(mt, &bg, &fg);
    arcade_card(s, x + hw + 8, y, PW - hw - 8, 56, "BEST TILE", buf, FONT_UI_LARGE,
                mt >= 3 ? color_shade(bg, t->dark ? 10 : -25) : 0);

    update_buttons(g);
    arcade_btn_draw(s, &g->btn[BTN_UNDO], BTN_UNDO, g->hover, g->press);
    arcade_btn_draw(s, &g->btn[BTN_NEW], BTN_NEW, g->hover, g->press);

    int hy = g->btn[BTN_NEW].r.y + g->btn[BTN_NEW].r.h + 14;
    uint32_t m = color_mix(t->window_text, t->window_bg, 110);
    static const char *keys[][2] = {
        { "Arrows / WASD", "slide" }, { "Drag on board", "slide" }, { "U / Ctrl+Z", "undo" }, { "F2", "new game" },
    };
    for (int i = 0; i < ARRAY_SIZE(keys); i++) {
        gfx_text(s, x, hy + i * 18, keys[i][0], t->window_text, FONT_UI_BOLD);
        int tw = font_text_width(FONT_UI, keys[i][1]);
        gfx_text(s, x + PW - tw, hy + i * 18, keys[i][1], m, FONT_UI);
    }
}

/* ---- events --------------------------------------------------------------------------------- */
static void press_button(game_t *g, int b) {
    switch (b) {
    case BTN_UNDO: g->ai = false; undo(g); break;
    case BTN_NEW: g->ai = false; new_game(g); break;
    case BTN_OV1:
        if (g->state == ST_WON) keep_going(g);
        else { g->ai = false; new_game(g); }
        break;
    case BTN_OV2:
        g->ai = false;
        if (g->state == ST_WON) new_game(g);
        else undo(g);
        break;
    }
}

static int key_dir(int key) {
    switch (key) {
    case KEY_UP: case 'w': return D_UP;
    case KEY_RIGHT: case 'd': return D_RIGHT;
    case KEY_DOWN: case 's': return D_DOWN;
    case KEY_LEFT: case 'a': return D_LEFT;
    default: return -1;
    }
}

static void on_key(game_t *g, const event_t *ev) {
    if (ev->mods & MOD_ALT) return;
    if (ev->mods & MOD_CTRL) {
        if (ev->key == 'z') { g->ai = false; undo(g); }
        return;
    }
    int d = key_dir(ev->key);
    if (d >= 0) {
        g->ai = false;
        if (g->state == ST_PLAY) do_move(g, d);
        return;
    }
    switch (ev->key) {
    case 'u': case KEY_BACKSPACE:
        g->ai = false;
        undo(g);
        break;
    case KEY_F2: case 'n':
        if (ev->repeat) break;
        g->ai = false;
        new_game(g);
        break;
    case KEY_ENTER: case KEY_SPACE:
        if (ev->repeat) break;
        if (g->state == ST_WON) keep_going(g);
        else if (g->state == ST_OVER) { g->ai = false; new_game(g); }
        break;
    case KEY_ESCAPE:
        if (g->state == ST_WON) keep_going(g);
        break;
    }
}

static void game_event(window_t *win, const event_t *ev) {
    game_t *g = win_get_userdata(win);
    if (!g) return;
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        update_buttons(g);
        draw_board(g, s);
        draw_panel(g, s);
        break;
    }
    case EV_TIMER:
        tick(g);
        break;
    case EV_KEY_DOWN:
        on_key(g, ev);
        break;
    case EV_MOUSE_MOVE: {
        update_buttons(g);
        int h = arcade_btn_hit(g->btn, NBTN, ev->x, ev->y);
        if (h != g->hover) { g->hover = h; inv_all(g); }
        break;
    }
    case EV_MOUSE_LEAVE:
        if (g->hover != -1) { g->hover = -1; inv_all(g); }
        break;
    case EV_MOUSE_DOWN:
        if (ev->button != MOUSE_LEFT) break;
        update_buttons(g);
        g->press = arcade_btn_hit(g->btn, NBTN, ev->x, ev->y);
        if (g->press >= 0) { inv_all(g); break; }
        if (g->state == ST_PLAY && rect_contains(rect_make(BX, BY, BS, BS), ev->x, ev->y)) {
            g->dragging = true;
            g->drag_x = ev->x;
            g->drag_y = ev->y;
        }
        break;
    case EV_MOUSE_UP:
        if (ev->button != MOUSE_LEFT) break;
        if (g->press >= 0) {
            int b = g->press;
            g->press = -1;
            inv_all(g);
            update_buttons(g);
            if (arcade_btn_hit(g->btn, NBTN, ev->x, ev->y) == b) press_button(g, b);
        } else if (g->dragging) {
            g->dragging = false;
            int dx = ev->x - g->drag_x, dy = ev->y - g->drag_y;
            if (MAX(abs(dx), abs(dy)) >= 30) {
                g->ai = false;
                if (abs(dx) > abs(dy)) do_move(g, dx > 0 ? D_RIGHT : D_LEFT);
                else do_move(g, dy > 0 ? D_DOWN : D_UP);
            }
        }
        break;
    case EV_CLOSE:
        save_best(g);
        win_set_userdata(win, NULL);
        free(g);
        break;
    default:
        break;
    }
}

static void game_launch(const char *arg) {
    game_t *g = calloc(1, sizeof *g);
    if (!g) return;
    const app_desc_t *app = app_find("2048");
    uint32_t flags = (app && app_window_count(app) > 0) ? 0 : WIN_CENTER;
    window_t *win = win_create("2048", CW, CH, flags);
    if (!win) { free(g); return; }
    g->win = win;
    arcade_rng_seed(&g->rng, (uint32_t)(uintptr_t)g);
    g->best = g->saved_best = arcade_hiscore_get("2048");
    g->hover = g->press = -1;
    int by = BY + 46 + 74 + 74 + 56 + 14;
    g->btn[BTN_UNDO] = (arcade_btn_t){ rect_make(PX, by, PW, 32), "Undo", false, true };
    g->btn[BTN_NEW] = (arcade_btn_t){ rect_make(PX, by + 40, PW, 32), "New game", false, true };
    int bw = (OV_W - 36 - 10) / 2, oby = OV_Y + OV_H - 18 - 32;
    g->btn[BTN_OV1] = (arcade_btn_t){ rect_make(OV_X + 18, oby, bw, 32), "Keep going", true, false };
    g->btn[BTN_OV2] = (arcade_btn_t){ rect_make(OV_X + 18 + bw + 10, oby, bw, 32), "New game", false, false };
    win_set_userdata(win, g);
    win_set_event_handler(win, game_event);
    new_game(g);
    if (arg && !strcmp(arg, "demo")) {
        g->ai = true;
    } else if (arg && (!strcmp(arg, "tiles") || !strcmp(arg, "test-win") || !strcmp(arg, "test-lose"))) {
        /* preset boards: every value / one move from 2048 (Left) / one move from losing (Right) */
        static const uint8_t WINB[N * N] = { 10, 10, 0, 0, 1, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
        static const uint8_t LOSEB[N * N] = { 3, 4, 5, 6, 7, 8, 9, 10, 3, 4, 5, 6, 7, 8, 9, 0 };
        for (int i = 0; i < N * N; i++)
            g->grid[i / N][i % N] = !strcmp(arg, "tiles") ? (uint8_t)(i + 1)
                                    : !strcmp(arg, "test-win") ? WINB[i] : LOSEB[i];
        g->nnew = 0;
        g->anim_ms = ANIM_MS;
    }
    g->last_ms = sys_uptime_ms();
    win_set_timer(win, 16);
}

/* Icon: slate board with four tiles reading 2 0 / 4 8, coloured like 2, 16, 128 and 2048. */
static void icon_2048(surface_t *s, int x, int y, int size) {
    int m = size / 16;
    int in = size - 2 * m;
    gfx_fill_rounded_gradient_v(s, x + m, y + m, in, in, size / 5, RGB(76, 84, 112), RGB(42, 47, 66));
    int gap = MAX(2, in / 13);
    int t = (in - 3 * gap) / 2;
    int ox = x + m + (in - 2 * t - gap) / 2, oy = y + m + (in - 2 * t - gap) / 2;
    static const char *digits[4] = { "2", "0", "4", "8" };
    static const int exps[4] = { 1, 4, 7, 11 };
    const font_t *f = t >= 34 ? FONT_UI_TITLE : (t >= 22 ? FONT_UI_LARGE : FONT_UI_BOLD);
    for (int i = 0; i < 4; i++) {
        uint32_t bg, fg;
        tile_style(exps[i], &bg, &fg);
        int tx = ox + (i % 2) * (t + gap), ty = oy + (i / 2) * (t + gap);
        int rad = MAX(2, t / 5);
        gfx_fill_rounded_rect(s, tx, ty + MAX(1, t / 18), t, t, rad, color_shade(bg, -50));
        gfx_fill_rounded_gradient_v(s, tx, ty, t, t, rad, color_shade(bg, 18), color_shade(bg, -8));
        gfx_text_aligned(s, rect_make(tx, ty, t, t), digits[i], fg, f, ALIGN_CENTER | ALIGN_MIDDLE);
    }
}

REGISTER_APP(.name = "2048", .category = CAT_GAMES,
             .description = "Slide and merge the tiles to reach 2048",
             .launch = game_launch, .draw_icon = icon_2048);
