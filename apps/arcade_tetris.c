/*
 * arcade_tetris.c - Tetris (Games).
 *
 * A guideline-style falling blocks game:
 *   - 10x20 well (plus 2 hidden rows above), 7-bag randomiser;
 *   - SRS rotation in both directions with the standard wall kick tables;
 *   - soft drop, hard drop, ghost piece, hold, 3-piece next preview;
 *   - lock delay (500 ms, reset by moving/rotating up to 15 times);
 *   - line clears with a flash animation, levels every 10 lines with the
 *     guideline gravity curve, score (100/300/500/800 x level, +1 per soft
 *     drop cell, +2 per hard drop cell) and a persistent high score;
 *   - pause (also on focus loss), game over with a "top out" animation, restart.
 *
 * Everything runs from a 16 ms window timer using sys_uptime_ms() deltas.
 * Left/Right auto-shift (DAS 160 ms, ARR 40 ms) is tracked from key down/up
 * events so it does not depend on the keyboard's typematic rate.
 *
 * Launch argument "demo" lets a simple placement AI play (attract mode, tests).
 */
#include "arcade_common.h"

#define COLS 10
#define VROWS 20
#define HIDDEN 2
#define TROWS (VROWS + HIDDEN)
#define CELL 26

#define LX 20
#define LW 118
#define BX (LX + LW + 18)
#define BY 20
#define BW (COLS * CELL)
#define BH (VROWS * CELL)
#define RX (BX + BW + 18)
#define RW 124
#define CW (RX + RW + 20)
#define CH (BY + BH + 20)

#define NEXT_N 3
#define DAS_MS 160
#define ARR_MS 40
#define LOCK_MS 500
#define MAX_RESETS 15
#define CLEAR_MS 320
#define GRAY 8

enum { ST_READY, ST_PLAY, ST_CLEAR, ST_TOPOUT, ST_OVER };
enum { BTN_PAUSE, BTN_NEW, NBTN };
enum { P_I, P_J, P_L, P_O, P_S, P_T, P_Z };

/* spawn orientation (SRS) cells inside an N x N box */
static const int8_t SPAWN[7][4][2] = {
    { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } },   /* I */
    { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },   /* J */
    { { 2, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },   /* L */
    { { 1, 0 }, { 2, 0 }, { 1, 1 }, { 2, 1 } },   /* O */
    { { 1, 0 }, { 2, 0 }, { 0, 1 }, { 1, 1 } },   /* S */
    { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },   /* T */
    { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 2, 1 } },   /* Z */
};
static const int BOXN[7] = { 4, 3, 3, 4, 3, 3, 3 };

/* SRS wall kicks (x right, y UP as in the guideline tables); index = rotation state we leave. */
static const int8_t KICK_CW[4][5][2] = {
    { { 0, 0 }, { -1, 0 }, { -1, 1 }, { 0, -2 }, { -1, -2 } },   /* 0->R */
    { { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, 2 }, { 1, 2 } },       /* R->2 */
    { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, -2 }, { 1, -2 } },      /* 2->L */
    { { 0, 0 }, { -1, 0 }, { -1, -1 }, { 0, 2 }, { -1, 2 } },    /* L->0 */
};
static const int8_t KICK_CCW[4][5][2] = {
    { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, -2 }, { 1, -2 } },      /* 0->L */
    { { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, 2 }, { 1, 2 } },       /* R->0 */
    { { 0, 0 }, { -1, 0 }, { -1, 1 }, { 0, -2 }, { -1, -2 } },   /* 2->R */
    { { 0, 0 }, { -1, 0 }, { -1, -1 }, { 0, 2 }, { -1, 2 } },    /* L->2 */
};
static const int8_t KICK_I_CW[4][5][2] = {
    { { 0, 0 }, { -2, 0 }, { 1, 0 }, { -2, -1 }, { 1, 2 } },     /* 0->R */
    { { 0, 0 }, { -1, 0 }, { 2, 0 }, { -1, 2 }, { 2, -1 } },     /* R->2 */
    { { 0, 0 }, { 2, 0 }, { -1, 0 }, { 2, 1 }, { -1, -2 } },     /* 2->L */
    { { 0, 0 }, { 1, 0 }, { -2, 0 }, { 1, -2 }, { -2, 1 } },     /* L->0 */
};
static const int8_t KICK_I_CCW[4][5][2] = {
    { { 0, 0 }, { -1, 0 }, { 2, 0 }, { -1, 2 }, { 2, -1 } },     /* 0->L */
    { { 0, 0 }, { 2, 0 }, { -1, 0 }, { 2, 1 }, { -1, -2 } },     /* R->0 */
    { { 0, 0 }, { 1, 0 }, { -2, 0 }, { 1, -2 }, { -2, 1 } },     /* 2->R */
    { { 0, 0 }, { -2, 0 }, { 1, 0 }, { -2, -1 }, { 1, 2 } },     /* L->2 */
};

static const uint32_t PIECE_COL[9] = {
    0, RGB(56, 200, 236), RGB(66, 110, 230), RGB(246, 146, 44), RGB(250, 206, 48),
    RGB(92, 202, 82), RGB(172, 86, 222), RGB(238, 66, 78), RGB(120, 126, 142),
};

static int8_t cells_tbl[7][4][4][2];   /* [piece][rotation][cell][x/y], built once */
static bool cells_ready;

static void build_cells(void) {
    if (cells_ready) return;
    for (int p = 0; p < 7; p++) {
        int n = BOXN[p];
        for (int c = 0; c < 4; c++) {
            int x = SPAWN[p][c][0], y = SPAWN[p][c][1];
            for (int r = 0; r < 4; r++) {
                cells_tbl[p][r][c][0] = (int8_t)x;
                cells_tbl[p][r][c][1] = (int8_t)y;
                if (p != P_O) { int nx = n - 1 - y, ny = x; x = nx; y = ny; }   /* rotate clockwise */
            }
        }
    }
    cells_ready = true;
}

typedef struct {
    window_t *win;
    arcade_rng_t rng;
    uint8_t grid[TROWS][COLS];     /* 0 = empty, 1..7 = piece + 1, 8 = grey */
    int cur, rot, px, py;
    int hold;                      /* -1 = empty */
    bool hold_used;
    int bag[7], bag_n;
    int next[NEXT_N];
    int score, lines, level, best;
    bool new_best;
    int state;
    bool paused;
    uint32_t last_ms, grav_acc, lock_ms;
    int lock_resets, lowest_y;
    /* input */
    bool held_l, held_r, soft;
    int das_dir;
    uint32_t das_ms, arr_acc;
    bool das_charged;
    /* line clear animation */
    int clear_rows[4], nclear;
    uint32_t clear_ms;
    /* top-out animation */
    int top_row;
    uint32_t top_acc;
    /* message ("TETRIS!", "Level 4") */
    char msg[32];
    uint32_t msg_ms;
    /* demo AI */
    bool ai;
    int ai_rot, ai_x;
    uint32_t ai_acc;
    arcade_btn_t btn[NBTN];
    int hover, press;
} tetris_t;

static void inv_board(tetris_t *g) { win_invalidate_rect(g->win, BX - 6, BY - 6, BW + 12, BH + 12); }
static void inv_sides(tetris_t *g) {
    win_invalidate_rect(g->win, 0, 0, BX - 6, CH);
    win_invalidate_rect(g->win, RX - 6, 0, CW - RX + 6, CH);
}

static bool fits(const tetris_t *g, int p, int rot, int x, int y) {
    for (int c = 0; c < 4; c++) {
        int cx = x + cells_tbl[p][rot][c][0], cy = y + cells_tbl[p][rot][c][1];
        if (cx < 0 || cx >= COLS || cy >= TROWS) return false;
        if (cy >= 0 && g->grid[cy][cx]) return false;
    }
    return true;
}

static int bag_next(tetris_t *g) {
    if (g->bag_n == 0) {
        for (int i = 0; i < 7; i++) g->bag[i] = i;
        for (int i = 6; i > 0; i--) {
            int j = arcade_rand_n(&g->rng, i + 1);
            int t = g->bag[i]; g->bag[i] = g->bag[j]; g->bag[j] = t;
        }
        g->bag_n = 7;
    }
    return g->bag[--g->bag_n];
}

static uint32_t gravity_ms(int level) {
    int l = MIN(level, 20);
    double sec = pow(0.8 - (l - 1) * 0.007, l - 1);
    double ms = sec * 1000.0;
    return ms < 1 ? 1 : (uint32_t)ms;
}

static void show_msg(tetris_t *g, const char *m) {
    strlcpy(g->msg, m, sizeof g->msg);
    g->msg_ms = 0;
}

static void ai_plan(tetris_t *g);

static void game_over(tetris_t *g) {
    g->state = ST_TOPOUT;
    g->top_row = TROWS - 1;
    g->top_acc = 0;
    g->soft = false;
    g->das_dir = 0;
    if (g->score > g->best) {
        g->best = g->score;
        g->new_best = true;
        arcade_hiscore_set("tetris", g->best);
    }
    arcade_sound_gameover();
    klog("tetris: game over, score %d, lines %d, level %d", g->score, g->lines, g->level);
    win_invalidate(g->win);
}

static bool spawn(tetris_t *g, int p) {
    g->cur = p;
    g->rot = 0;
    g->px = 3;
    /* spawn with the piece's top row on the first visible row; if that is blocked,
       try one and two rows higher (inside the hidden rows) before topping out */
    int miny = 4;
    for (int c = 0; c < 4; c++) miny = MIN(miny, cells_tbl[p][0][c][1]);
    int top = HIDDEN - miny;
    g->py = top;
    while (!fits(g, p, 0, g->px, g->py)) {
        if (--g->py < top - 2) { g->py = top - 2; game_over(g); return false; }
    }
    g->lock_ms = 0;
    g->lock_resets = 0;
    g->lowest_y = g->py;
    g->grav_acc = 0;
    if (g->ai) ai_plan(g);
    return true;
}

static void spawn_next(tetris_t *g) {
    int p = g->next[0];
    memmove(g->next, g->next + 1, sizeof(int) * (NEXT_N - 1));
    g->next[NEXT_N - 1] = bag_next(g);
    g->hold_used = false;
    spawn(g, p);
    inv_sides(g);
}

static void new_game(tetris_t *g, bool start, bool ai) {
    g->ai = ai;
    memset(g->grid, 0, sizeof g->grid);
    g->bag_n = 0;
    for (int i = 0; i < NEXT_N; i++) g->next[i] = bag_next(g);
    g->hold = -1;
    g->hold_used = false;
    g->score = g->lines = 0;
    g->level = 1;
    g->new_best = false;
    g->paused = false;
    g->nclear = 0;
    g->msg[0] = 0;
    g->held_l = g->held_r = g->soft = false;
    g->das_dir = 0;
    g->last_ms = sys_uptime_ms();
    if (start) {
        g->state = ST_PLAY;
        spawn_next(g);
    } else {
        g->state = ST_READY;
    }
    win_invalidate(g->win);
}

/* successful move/rotation: lock delay reset while on the ground */
static void moved(tetris_t *g) {
    if (!fits(g, g->cur, g->rot, g->px, g->py + 1) && g->lock_resets < MAX_RESETS) {
        g->lock_ms = 0;
        g->lock_resets++;
    }
    inv_board(g);
}

static bool try_move(tetris_t *g, int dx) {
    if (g->state != ST_PLAY || g->paused) return false;
    if (!fits(g, g->cur, g->rot, g->px + dx, g->py)) return false;
    g->px += dx;
    moved(g);
    return true;
}

static bool try_rotate(tetris_t *g, int dir) {
    if (g->state != ST_PLAY || g->paused) return false;
    if (g->cur == P_O) return true;
    int nr = (g->rot + dir + 4) % 4;
    const int8_t (*k)[2] = g->cur == P_I ? (dir > 0 ? KICK_I_CW[g->rot] : KICK_I_CCW[g->rot])
                                         : (dir > 0 ? KICK_CW[g->rot] : KICK_CCW[g->rot]);
    for (int i = 0; i < 5; i++) {
        int dx = k[i][0], dy = -k[i][1];
        if (fits(g, g->cur, nr, g->px + dx, g->py + dy)) {
            g->px += dx;
            g->py += dy;
            g->rot = nr;
            moved(g);
            return true;
        }
    }
    return false;
}

static void lock_piece(tetris_t *g) {
    bool above = true;
    for (int c = 0; c < 4; c++) {
        int cx = g->px + cells_tbl[g->cur][g->rot][c][0], cy = g->py + cells_tbl[g->cur][g->rot][c][1];
        if (cy >= 0 && cy < TROWS && cx >= 0 && cx < COLS) g->grid[cy][cx] = (uint8_t)(g->cur + 1);
        if (cy >= HIDDEN) above = false;
    }
    if (above) { game_over(g); return; }     /* lock out: piece entirely above the visible well */
    g->nclear = 0;
    for (int r = 0; r < TROWS; r++) {
        bool full = true;
        for (int c = 0; c < COLS && full; c++)
            if (!g->grid[r][c]) full = false;
        if (full && g->nclear < 4) g->clear_rows[g->nclear++] = r;
    }
    if (g->nclear) {
        static const int base[5] = { 0, 100, 300, 500, 800 };
        static const char *names[5] = { "", "Single", "Double", "Triple", "TETRIS!" };
        g->score += base[g->nclear] * g->level;
        int old_level = g->level;
        g->lines += g->nclear;
        g->level = 1 + g->lines / 10;
        if (g->level != old_level) {
            char m[32];
            snprintf(m, sizeof m, "Level %d", g->level);
            show_msg(g, m);
            arcade_sound_fanfare();
        } else {
            show_msg(g, names[g->nclear]);
            if (g->nclear == 4) arcade_sound_fanfare();
            else sys_beep(660 + 110 * g->nclear, 60);
        }
        g->state = ST_CLEAR;
        g->clear_ms = 0;
        inv_sides(g);
    } else {
        spawn_next(g);
    }
    inv_board(g);
}

static void finish_clear(tetris_t *g) {
    for (int i = 0; i < g->nclear; i++) {
        int r = g->clear_rows[i];        /* ascending order: rows above shift down, lower rows unaffected */
        memmove(&g->grid[1][0], &g->grid[0][0], (size_t)r * COLS);
        memset(&g->grid[0][0], 0, COLS);
    }
    g->nclear = 0;
    g->state = ST_PLAY;
    spawn_next(g);
    inv_board(g);
}

static void hard_drop(tetris_t *g) {
    if (g->state != ST_PLAY || g->paused) return;
    int d = 0;
    while (fits(g, g->cur, g->rot, g->px, g->py + 1)) { g->py++; d++; }
    g->score += 2 * d;
    lock_piece(g);
    inv_sides(g);
}

static void hold_piece(tetris_t *g) {
    if (g->state != ST_PLAY || g->paused || g->hold_used) return;
    int cur = g->cur;
    if (g->hold < 0) {
        g->hold = cur;
        spawn_next(g);
    } else {
        int h = g->hold;
        g->hold = cur;
        spawn(g, h);
    }
    g->hold_used = true;
    inv_sides(g);
    inv_board(g);
}

static void set_paused(tetris_t *g, bool p) {
    if (g->state != ST_PLAY && g->state != ST_CLEAR) return;
    if (g->paused == p) return;
    g->paused = p;
    g->held_l = g->held_r = g->soft = false;
    g->das_dir = 0;
    g->last_ms = sys_uptime_ms();
    win_invalidate(g->win);
}

/* ---- demo AI: evaluate every rotation/column, then steer the piece there ---------------- */
static int ai_eval(const tetris_t *g, int p, int rot, int x, int y) {
    uint8_t b[TROWS][COLS];
    memcpy(b, g->grid, sizeof b);
    for (int c = 0; c < 4; c++) {
        int cy = y + cells_tbl[p][rot][c][1];
        if (cy >= 0) b[cy][x + cells_tbl[p][rot][c][0]] = 1;
    }
    int lines = 0;
    for (int r = 0; r < TROWS; r++) {
        bool full = true;
        for (int c = 0; c < COLS && full; c++) if (!b[r][c]) full = false;
        if (full) {
            lines++;
            memmove(&b[1][0], &b[0][0], (size_t)r * COLS);
            memset(&b[0][0], 0, COLS);
        }
    }
    int h[COLS], agg = 0, holes = 0, bump = 0;
    for (int c = 0; c < COLS; c++) {
        h[c] = 0;
        for (int r = 0; r < TROWS; r++)
            if (b[r][c]) { h[c] = TROWS - r; break; }
        agg += h[c];
        for (int r = TROWS - h[c] + 1; r < TROWS; r++) if (!b[r][c]) holes++;
        if (c) bump += abs(h[c] - h[c - 1]);
    }
    return -51 * agg + 76 * lines * 10 - 36 * holes * 10 - 18 * bump;
}

static void ai_plan(tetris_t *g) {
    int best = INT_MIN;
    g->ai_rot = g->rot;
    g->ai_x = g->px;
    for (int r = 0; r < (g->cur == P_O ? 1 : 4); r++)
        for (int x = -2; x < COLS; x++) {
            if (!fits(g, g->cur, r, x, g->py)) continue;
            int y = g->py;
            while (fits(g, g->cur, r, x, y + 1)) y++;
            int v = ai_eval(g, g->cur, r, x, y);
            if (v > best) { best = v; g->ai_rot = r; g->ai_x = x; }
        }
    g->ai_acc = 0;
}

static void ai_step(tetris_t *g, uint32_t dt) {
    g->ai_acc += dt;
    if (g->ai_acc < 70) return;
    g->ai_acc = 0;
    if (g->rot != g->ai_rot) { if (!try_rotate(g, 1)) hard_drop(g); }
    else if (g->px < g->ai_x) { if (!try_move(g, 1)) hard_drop(g); }
    else if (g->px > g->ai_x) { if (!try_move(g, -1)) hard_drop(g); }
    else hard_drop(g);
}

/* ---- timer -------------------------------------------------------------------------- */
static void tick(tetris_t *g) {
    uint32_t now = sys_uptime_ms();
    uint32_t dt = now - g->last_ms;
    g->last_ms = now;
    if (dt > 100) dt = 100;
    if (g->paused || g->state == ST_READY) return;

    if (g->msg[0]) {
        g->msg_ms += dt;
        if (g->msg_ms > 1300) g->msg[0] = 0;
        inv_board(g);
    }
    if (g->state == ST_OVER) return;
    if (g->state == ST_TOPOUT) {
        g->top_acc += dt;
        while (g->top_acc >= 28 && g->state == ST_TOPOUT) {
            g->top_acc -= 28;
            for (int c = 0; c < COLS; c++)
                if (g->grid[g->top_row][c]) g->grid[g->top_row][c] = GRAY;
            if (--g->top_row < HIDDEN) g->state = ST_OVER;
        }
        win_invalidate(g->win);
        return;
    }
    if (g->state == ST_CLEAR) {
        g->clear_ms += dt;
        if (g->clear_ms >= CLEAR_MS) finish_clear(g);
        inv_board(g);
        return;
    }

    /* ST_PLAY */
    if (g->ai) { ai_step(g, dt); if (g->state != ST_PLAY) return; }
    if (g->das_dir) {
        g->das_ms += dt;
        if (!g->das_charged && g->das_ms >= DAS_MS) {
            g->das_charged = true;
            g->arr_acc = ARR_MS;
        }
        if (g->das_charged) {
            g->arr_acc += dt;
            while (g->arr_acc >= ARR_MS) {
                g->arr_acc -= ARR_MS;
                if (!try_move(g, g->das_dir)) { g->arr_acc = 0; break; }
            }
        }
    }
    uint32_t iv = gravity_ms(g->level);
    if (g->soft && iv > 30) iv = 30;
    if (!fits(g, g->cur, g->rot, g->px, g->py + 1)) {
        g->grav_acc = 0;
        g->lock_ms += dt;
        if (g->lock_ms >= LOCK_MS || g->lock_resets >= MAX_RESETS) {
            lock_piece(g);
            inv_sides(g);
        }
    } else {
        g->grav_acc += dt;
        while (g->grav_acc >= iv) {
            g->grav_acc -= iv;
            if (!fits(g, g->cur, g->rot, g->px, g->py + 1)) { g->grav_acc = 0; break; }
            g->py++;
            if (g->soft) { g->score++; inv_sides(g); }
            if (g->py > g->lowest_y) {     /* new lowest row: fresh lock delay */
                g->lowest_y = g->py;
                g->lock_resets = 0;
                g->lock_ms = 0;
            }
        }
    }
    inv_board(g);
}

/* ---- drawing ------------------------------------------------------------------------ */
static void draw_block(surface_t *s, int x, int y, int sz, uint32_t c) {
    gfx_fill_rounded_rect(s, x + 1, y + 1, sz - 2, sz - 2, 4, color_shade(c, -70));
    gfx_fill_rounded_gradient_v(s, x + 2, y + 2, sz - 4, sz - 4, 3, color_shade(c, 60), color_shade(c, -30));
    int in = sz >= 20 ? 5 : 4;
    gfx_fill_rounded_gradient_v(s, x + in, y + in, sz - 2 * in, sz - 2 * in, 2, color_shade(c, 18), color_shade(c, -6));
    gfx_fill_rect(s, x + in, y + 3, sz - 2 * in, 1, color_alpha(COLOR_WHITE, 90));
}

static void draw_ghost(surface_t *s, int x, int y, int sz, uint32_t c) {
    gfx_fill_rounded_rect(s, x + 2, y + 2, sz - 4, sz - 4, 4, color_alpha(c, 46));
    gfx_rounded_rect(s, x + 2, y + 2, sz - 4, sz - 4, 4, color_alpha(c, 170));
}

/* piece p (spawn rotation) centred in rect r with block size sz */
static void draw_preview(surface_t *s, rect_t r, int p, int sz, bool dim) {
    int minx = 9, maxx = -1, miny = 9, maxy = -1;
    for (int c = 0; c < 4; c++) {
        int x = cells_tbl[p][0][c][0], y = cells_tbl[p][0][c][1];
        minx = MIN(minx, x); maxx = MAX(maxx, x); miny = MIN(miny, y); maxy = MAX(maxy, y);
    }
    int w = (maxx - minx + 1) * sz, h = (maxy - miny + 1) * sz;
    int ox = r.x + (r.w - w) / 2, oy = r.y + (r.h - h) / 2;
    uint32_t col = dim ? PIECE_COL[GRAY] : PIECE_COL[p + 1];
    for (int c = 0; c < 4; c++)
        draw_block(s, ox + (cells_tbl[p][0][c][0] - minx) * sz, oy + (cells_tbl[p][0][c][1] - miny) * sz, sz, col);
}

static void draw_board(tetris_t *g, surface_t *s) {
    uint32_t bg = RGB(20, 23, 32), line = RGB(30, 34, 46);
    gfx_shadow(s, BX - 5, BY - 5, BW + 10, BH + 10, 10, 6, 50);
    gfx_fill_rounded_rect(s, BX - 5, BY - 5, BW + 10, BH + 10, 9, RGB(48, 53, 70));
    gfx_fill_rect(s, BX, BY, BW, BH, bg);
    for (int c = 1; c < COLS; c++) gfx_vline(s, BX + c * CELL, BY, BH, line);
    for (int r = 1; r < VROWS; r++) gfx_hline(s, BX, BY + r * CELL, BW, line);

    for (int r = HIDDEN; r < TROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (g->grid[r][c]) {
                bool clearing = false;
                if (g->state == ST_CLEAR)
                    for (int i = 0; i < g->nclear; i++)
                        if (g->clear_rows[i] == r) clearing = true;
                if (clearing) {
                    /* blocks vanish from the centre outwards */
                    double p = (double)g->clear_ms / CLEAR_MS;
                    if (fabs(c - 4.5) < p * 6.0 - 0.5) continue;
                }
                draw_block(s, BX + c * CELL, BY + (r - HIDDEN) * CELL, CELL, PIECE_COL[g->grid[r][c]]);
            }
    if (g->state == ST_CLEAR) {
        double p = (double)g->clear_ms / CLEAR_MS;
        int a = (int)(200 * sin(M_PI * MIN(1.0, p * 1.3)));
        for (int i = 0; i < g->nclear; i++)
            gfx_fill_rect(s, BX, BY + (g->clear_rows[i] - HIDDEN) * CELL, BW, CELL, RGBA(255, 255, 255, MAX(a, 0)));
    }

    if (g->state == ST_PLAY) {
        rect_t old_clip = s->clip;
        s->clip = rect_intersect(old_clip, rect_make(BX, BY, BW, BH));
        int gy = g->py;
        while (fits(g, g->cur, g->rot, g->px, gy + 1)) gy++;
        uint32_t col = PIECE_COL[g->cur + 1];
        if (gy != g->py)
            for (int c = 0; c < 4; c++)
                draw_ghost(s, BX + (g->px + cells_tbl[g->cur][g->rot][c][0]) * CELL,
                           BY + (gy + cells_tbl[g->cur][g->rot][c][1] - HIDDEN) * CELL, CELL, col);
        for (int c = 0; c < 4; c++) {
            int cy = g->py + cells_tbl[g->cur][g->rot][c][1];
            if (cy < HIDDEN) continue;
            draw_block(s, BX + (g->px + cells_tbl[g->cur][g->rot][c][0]) * CELL, BY + (cy - HIDDEN) * CELL, CELL, col);
        }
        /* lock delay hint: the piece dims slightly while it is about to lock */
        if (!fits(g, g->cur, g->rot, g->px, g->py + 1) && g->lock_ms > 0) {
            int a = (int)(g->lock_ms * 90 / LOCK_MS);
            for (int c = 0; c < 4; c++) {
                int cy = g->py + cells_tbl[g->cur][g->rot][c][1];
                if (cy < HIDDEN) continue;
                gfx_fill_rounded_rect(s, BX + (g->px + cells_tbl[g->cur][g->rot][c][0]) * CELL + 1,
                                      BY + (cy - HIDDEN) * CELL + 1, CELL - 2, CELL - 2, 4, RGBA(0, 0, 0, a));
            }
        }
        s->clip = old_clip;
    }

    if (g->msg[0] && g->state != ST_OVER) {
        int a = g->msg_ms < 900 ? 255 : 255 - (int)((g->msg_ms - 900) * 255 / 400);
        a = CLAMP(a, 0, 255);
        int yy = BY + BH / 3 - (int)(g->msg_ms / 40);
        const font_t *f = FONT_UI_TITLE;
        int tw = font_text_width(f, g->msg);
        gfx_text(s, BX + (BW - tw) / 2 + 2, yy + 2, g->msg, RGBA(0, 0, 0, a / 2), f);
        gfx_text(s, BX + (BW - tw) / 2, yy, g->msg, color_alpha(RGB(255, 236, 120), a), f);
    }

    rect_t area = rect_make(BX, BY, BW, BH);
    char l1[48];
    if (g->state == ST_READY) {
        arcade_overlay(s, area, 60, 228, 128, "Tetris", 0, "Clear lines, level up!", "Press Enter to start");
    } else if (g->paused) {
        arcade_overlay(s, area, 140, 220, 112, "Paused", 0, "Press P to resume", NULL);
    } else if (g->state == ST_OVER) {
        if (g->new_best) snprintf(l1, sizeof l1, "New high score: %d", g->score);
        else snprintf(l1, sizeof l1, "Score %d", g->score);
        arcade_overlay(s, area, 120, 228, 128, "Game over", 0, l1, "Press Enter to play again");
    }
}

static void draw_box(surface_t *s, int x, int y, int w, int h, const char *label) {
    ui_draw_panel(s, rect_make(x, y, w, h));
    gfx_text(s, x + 12, y + 8, label, arcade_muted(), FONT_UI_BOLD);
}

static void draw_sides(tetris_t *g, surface_t *s) {
    const theme_t *t = theme_get();
    char buf[32];
    /* left column */
    const app_desc_t *app = app_find("Tetris");
    if (app) app_draw_icon(s, app, LX, BY, 32);
    gfx_text(s, LX + 40, BY + 5, "Tetris", t->window_text, FONT_UI_LARGE);
    int y = BY + 44;
    draw_box(s, LX, y, LW, 88, "HOLD");
    if (g->hold >= 0) draw_preview(s, rect_make(LX, y + 26, LW, 56), g->hold, 18, g->hold_used);
    y += 98;
    snprintf(buf, sizeof buf, "%d", g->score);
    arcade_card(s, LX, y, LW, 58, "SCORE", buf, FONT_UI_LARGE, 0);
    y += 66;
    snprintf(buf, sizeof buf, "%d", g->level);
    arcade_card(s, LX, y, LW, 58, "LEVEL", buf, FONT_UI_LARGE, 0);
    y += 66;
    snprintf(buf, sizeof buf, "%d", g->lines);
    arcade_card(s, LX, y, LW, 58, "LINES", buf, FONT_UI_LARGE, 0);

    g->btn[BTN_PAUSE].label = g->state == ST_READY ? "Start" : (g->paused ? "Resume" : "Pause");
    g->btn[BTN_PAUSE].primary = g->state == ST_READY || g->paused;
    g->btn[BTN_PAUSE].disabled = g->state == ST_OVER || g->state == ST_TOPOUT;
    g->btn[BTN_NEW].primary = g->state == ST_OVER;
    for (int i = 0; i < NBTN; i++) arcade_btn_draw(s, &g->btn[i], i, g->hover, g->press);

    /* right column */
    y = BY;
    draw_box(s, RX, y, RW, 214, "NEXT");
    draw_preview(s, rect_make(RX, y + 28, RW, 64), g->next[0], 22, false);
    ui_draw_separator(s, RX + 12, y + 100, RW - 24);
    draw_preview(s, rect_make(RX, y + 108, RW, 48), g->next[1], 16, false);
    draw_preview(s, rect_make(RX, y + 158, RW, 48), g->next[2], 16, false);
    y += 222;
    snprintf(buf, sizeof buf, "%d", g->best);
    arcade_card(s, RX, y, RW, 58, "HIGH SCORE", buf, FONT_UI_LARGE, g->new_best ? t->accent : 0);
    y += 74;
    static const char *keys[][2] = {
        { "Left/Right", "move" }, { "Up / X", "rotate" }, { "Z", "rotate left" }, { "Down", "soft drop" },
        { "Space", "hard drop" }, { "C / Shift", "hold" }, { "P / Esc", "pause" }, { "F2", "new game" },
    };
    uint32_t m = color_mix(t->window_text, t->window_bg, 110);
    for (int i = 0; i < ARRAY_SIZE(keys); i++) {
        gfx_text(s, RX, y + i * 19, keys[i][0], t->window_text, FONT_UI_BOLD);
        int tw = font_text_width(FONT_UI, keys[i][1]);
        gfx_text(s, RX + RW - tw, y + i * 19, keys[i][1], m, FONT_UI);
    }
}

/* ---- events -------------------------------------------------------------------------- */
static void press_button(tetris_t *g, int b) {
    if (b == BTN_PAUSE) {
        if (g->state == ST_READY) new_game(g, true, false);
        else set_paused(g, !g->paused);
    } else if (b == BTN_NEW) {
        new_game(g, true, false);
    }
}

static void on_key_down(tetris_t *g, const event_t *ev) {
    if (ev->mods & (MOD_CTRL | MOD_ALT)) return;
    int k = ev->key;
    if (k == KEY_F2 || k == 'n') { if (!ev->repeat) new_game(g, true, false); return; }
    if (g->state == ST_READY) {
        if ((k == KEY_ENTER || k == KEY_SPACE) && !ev->repeat) new_game(g, true, false);
        return;
    }
    if (g->state == ST_OVER) {
        if (k == KEY_ENTER && !ev->repeat) new_game(g, true, false);
        return;
    }
    if (k == 'p' || k == KEY_ESCAPE || k == KEY_PAUSE) {
        if (!ev->repeat) set_paused(g, !g->paused);
        return;
    }
    if (g->paused) {
        if (k == KEY_ENTER && !ev->repeat) set_paused(g, false);
        return;
    }
    if (g->state != ST_PLAY && g->state != ST_CLEAR) return;
    if (ev->repeat) return;          /* own auto-repeat for movement; rotations/drops are one per press */
    g->ai = false;
    switch (k) {
    case KEY_LEFT:
        g->held_l = true;
        g->das_dir = -1; g->das_ms = 0; g->das_charged = false;
        try_move(g, -1);
        break;
    case KEY_RIGHT:
        g->held_r = true;
        g->das_dir = 1; g->das_ms = 0; g->das_charged = false;
        try_move(g, 1);
        break;
    case KEY_DOWN:
        g->soft = true;
        g->grav_acc = 30;        /* drop one row right away */
        break;
    case KEY_UP: case 'x':
        try_rotate(g, 1);
        break;
    case 'z':
        try_rotate(g, -1);
        break;
    case KEY_SPACE:
        hard_drop(g);
        break;
    case 'c': case KEY_LSHIFT: case KEY_RSHIFT:
        hold_piece(g);
        break;
    }
}

static void on_key_up(tetris_t *g, const event_t *ev) {
    switch (ev->key) {
    case KEY_LEFT:
        g->held_l = false;
        if (g->das_dir == -1) {
            g->das_dir = g->held_r ? 1 : 0;
            g->das_ms = 0; g->das_charged = false;
        }
        break;
    case KEY_RIGHT:
        g->held_r = false;
        if (g->das_dir == 1) {
            g->das_dir = g->held_l ? -1 : 0;
            g->das_ms = 0; g->das_charged = false;
        }
        break;
    case KEY_DOWN:
        g->soft = false;
        break;
    }
}

static void tetris_event(window_t *win, const event_t *ev) {
    tetris_t *g = win_get_userdata(win);
    if (!g) return;
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        draw_board(g, s);
        draw_sides(g, s);
        break;
    }
    case EV_TIMER:
        tick(g);
        break;
    case EV_KEY_DOWN:
        on_key_down(g, ev);
        break;
    case EV_KEY_UP:
        on_key_up(g, ev);
        break;
    case EV_BLUR:
        set_paused(g, true);
        g->held_l = g->held_r = g->soft = false;
        g->das_dir = 0;
        break;
    case EV_MOUSE_MOVE: {
        int h = arcade_btn_hit(g->btn, NBTN, ev->x, ev->y);
        if (h != g->hover) { g->hover = h; inv_sides(g); }
        break;
    }
    case EV_MOUSE_LEAVE:
        if (g->hover != -1) { g->hover = -1; inv_sides(g); }
        break;
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_LEFT) {
            g->press = arcade_btn_hit(g->btn, NBTN, ev->x, ev->y);
            if (g->press >= 0) inv_sides(g);
        }
        break;
    case EV_MOUSE_UP:
        if (ev->button == MOUSE_LEFT && g->press >= 0) {
            int b = g->press;
            g->press = -1;
            inv_sides(g);
            if (arcade_btn_hit(g->btn, NBTN, ev->x, ev->y) == b) press_button(g, b);
        }
        break;
    case EV_CLOSE:
        win_set_userdata(win, NULL);
        free(g);
        break;
    default:
        break;
    }
}

static void tetris_launch(const char *arg) {
    build_cells();
    tetris_t *g = calloc(1, sizeof *g);
    if (!g) return;
    const app_desc_t *app = app_find("Tetris");
    uint32_t flags = (app && app_window_count(app) > 0) ? 0 : WIN_CENTER;
    window_t *win = win_create("Tetris", CW, CH, flags);
    if (!win) { free(g); return; }
    g->win = win;
    arcade_rng_seed(&g->rng, (uint32_t)(uintptr_t)g);
    g->best = arcade_hiscore_get("tetris");
    g->hover = g->press = -1;
    g->btn[BTN_PAUSE] = (arcade_btn_t){ rect_make(LX, BY + BH - 72, LW, 32), "Start", true, true };
    g->btn[BTN_NEW] = (arcade_btn_t){ rect_make(LX, BY + BH - 32, LW, 32), "New game", false, true };
    win_set_userdata(win, g);
    win_set_event_handler(win, tetris_event);
    bool demo = arg && !strcmp(arg, "demo");
    new_game(g, demo, demo);
    win_set_timer(win, 16);
}

/* Icon: dark rounded tile with a little stack of coloured blocks. */
static void tetris_icon(surface_t *s, int x, int y, int size) {
    int m = size / 16;
    gfx_fill_rounded_gradient_v(s, x + m, y + m, size - 2 * m, size - 2 * m, size / 5, RGB(52, 58, 82), RGB(24, 27, 40));
    static const int8_t map[4][4] = {
        { 0, 6, 0, 1 },
        { 6, 6, 6, 1 },
        { 3, 4, 4, 1 },
        { 3, 4, 4, 1 },
    };
    int cs = size * 18 / 100;
    int ox = x + (size - 4 * cs) / 2, oy = y + (size - 4 * cs) / 2 + size / 40;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            int p = map[r][c];
            if (!p) continue;
            uint32_t col = PIECE_COL[p];
            int bx = ox + c * cs, by = oy + r * cs;
            int rad = MAX(1, cs / 6);
            gfx_fill_rounded_rect(s, bx + 1, by + 1, cs - 2, cs - 2, rad, color_shade(col, -60));
            gfx_fill_rounded_gradient_v(s, bx + 2, by + 2, cs - 4, cs - 4, rad, color_shade(col, 50), color_shade(col, -20));
        }
}

REGISTER_APP(.name = "Tetris", .category = CAT_GAMES,
             .description = "Falling blocks: rotate, drop and clear lines",
             .launch = tetris_launch, .draw_icon = tetris_icon);
