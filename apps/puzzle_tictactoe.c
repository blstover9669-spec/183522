/*
 * puzzle_tictactoe.c - Tic-Tac-Toe (Games).
 *
 *   - "vs Computer": you are X, the computer plays O with a full minimax search
 *     (alpha-beta pruned, depth-aware so it wins fast and loses never). Among
 *     equally good moves it picks at random, so games do not repeat;
 *   - "2 Players": hot-seat X and O;
 *   - the starting player alternates every game; a running tally of X wins,
 *     O wins and draws per window, reset with the "Reset Score" button;
 *   - marks are drawn with a short pen animation, the winning line strikes
 *     through, hover previews the next mark;
 *   - keyboard: arrows + Space/Enter, or 1-9 (reading order: 1 = top left),
 *     N/F2 new game, M switches mode.
 *
 * The computer "thinks" for a moment on a window timer (never blocking);
 * the search itself is tiny (< 10k nodes once one mark is on the board, and
 * the empty board uses an opening book).
 */
#include "puzzle_common.h"

#define TT_W       404
#define TT_SEG_Y   14
#define TT_SEG_W   260
#define TT_SEG_H   30
#define TT_SCORE_Y 58
#define TT_SCORE_H 66
#define TT_BOARD_Y 138
#define TT_CELL    106
#define TT_GAP     8
#define TT_BOARD   (3 * TT_CELL + 2 * TT_GAP)
#define TT_STATUS_Y (TT_BOARD_Y + TT_BOARD + 24)
#define TT_BTN_Y   (TT_STATUS_Y + 54)
#define TT_H       (TT_BTN_Y + 32 + 18)

#define MARK_MS    220      /* pen animation of one mark */
#define LINE_MS    320      /* strike-through animation  */
#define AI_DELAY   420      /* computer "thinking" time  */

enum { MODE_AI, MODE_2P };
enum { P_NONE = 0, P_X = 1, P_O = 2, P_DRAW = 3 };

static const char *const mode_names[2] = { "vs Computer", "2 Players" };
static const int8_t lines[8][3] = {
    { 0, 1, 2 }, { 3, 4, 5 }, { 6, 7, 8 },
    { 0, 3, 6 }, { 1, 4, 7 }, { 2, 5, 8 },
    { 0, 4, 8 }, { 2, 4, 6 },
};

typedef struct {
    window_t *win;
    pz_rng_t rng;
    int mode;
    int8_t b[9];
    int turn;                /* P_X / P_O */
    int starter;             /* who opened the current game */
    int result;              /* P_NONE while playing, else P_X / P_O / P_DRAW */
    int win_line;            /* index into lines[] or -1 */
    int score[4];            /* [P_X], [P_O], [P_DRAW] */
    uint32_t placed_at[9];   /* animation start per mark */
    uint32_t line_at;
    bool ai_pending;
    uint32_t ai_due;
    int timer;
    int hover, kb;
    bool kb_on;
    int seg_hover;
    pz_btn_t btn[2];
    rect_t seg_r, score_r, board_r, status_r;
} ttt_t;

/* ---- rules --------------------------------------------------------------------------- */
static int tt_eval(const int8_t *b, int *line) {
    for (int i = 0; i < 8; i++) {
        int a = b[lines[i][0]];
        if (a && a == b[lines[i][1]] && a == b[lines[i][2]]) {
            if (line) *line = i;
            return a;
        }
    }
    for (int i = 0; i < 9; i++)
        if (!b[i]) return P_NONE;
    return P_DRAW;
}

/* Minimax with alpha-beta. Scores are from `me`'s point of view; quicker wins
   and slower losses score better. */
static int tt_minimax(int8_t *b, int player, int me, int depth, int alpha, int beta) {
    int r = tt_eval(b, NULL);
    if (r == me) return 10 - depth;
    if (r == P_DRAW) return 0;
    if (r != P_NONE) return depth - 10;
    int other = 3 - player;
    if (player == me) {
        int best = -100;
        for (int i = 0; i < 9; i++) {
            if (b[i]) continue;
            b[i] = (int8_t)player;
            int v = tt_minimax(b, other, me, depth + 1, alpha, beta);
            b[i] = 0;
            if (v > best) best = v;
            if (best > alpha) alpha = best;
            if (beta <= alpha) break;
        }
        return best;
    }
    int best = 100;
    for (int i = 0; i < 9; i++) {
        if (b[i]) continue;
        b[i] = (int8_t)player;
        int v = tt_minimax(b, other, me, depth + 1, alpha, beta);
        b[i] = 0;
        if (v < best) best = v;
        if (best < beta) beta = best;
        if (beta <= alpha) break;
    }
    return best;
}

static int tt_best_move(ttt_t *g, int me) {
    int8_t b[9];
    memcpy(b, g->b, sizeof b);
    int empty = 0;
    for (int i = 0; i < 9; i++) empty += !b[i];
    if (empty == 9) {
        /* opening book: every corner and the centre draw with perfect play */
        static const int open[5] = { 0, 2, 4, 6, 8 };
        return open[pz_rand_n(&g->rng, 5)];
    }
    int best = -1000, cand[9], nc = 0;
    for (int i = 0; i < 9; i++) {
        if (b[i]) continue;
        b[i] = (int8_t)me;
        int v = tt_minimax(b, 3 - me, me, 1, -1000, 1000);
        b[i] = 0;
        if (v > best) { best = v; nc = 0; }
        if (v == best) cand[nc++] = i;
    }
    return nc ? cand[pz_rand_n(&g->rng, nc)] : -1;
}

/* ---- layout / helpers ---------------------------------------------------------------- */
static rect_t tt_cell_rect(ttt_t *g, int i) {
    return rect_make(g->board_r.x + (i % 3) * (TT_CELL + TT_GAP), g->board_r.y + (i / 3) * (TT_CELL + TT_GAP),
                     TT_CELL, TT_CELL);
}

static int tt_cell_at(ttt_t *g, int x, int y) {
    for (int i = 0; i < 9; i++)
        if (rect_contains(tt_cell_rect(g, i), x, y)) return i;
    return -1;
}

static void tt_inval_board(ttt_t *g) {
    win_invalidate_rect(g->win, g->board_r.x - 16, g->board_r.y - 16, g->board_r.w + 32, g->board_r.h + 32);
}

static void tt_inval_info(ttt_t *g) {
    win_invalidate_rect(g->win, 0, g->score_r.y - 4, TT_W, g->score_r.h + 8);
    win_invalidate_rect(g->win, 0, g->status_r.y, TT_W, g->status_r.h);
}

static bool tt_human_turn(ttt_t *g) {
    return g->result == P_NONE && (g->mode == MODE_2P || g->turn == P_X);
}

static uint32_t tt_color(int p) {
    bool dark = theme_get()->dark;
    if (p == P_X) return dark ? RGB(96, 168, 255) : RGB(44, 120, 236);
    return dark ? RGB(255, 132, 104) : RGB(238, 92, 66);
}

static double ease_out(double t) {
    t = CLAMP(t, 0.0, 1.0);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

static bool tt_animating(ttt_t *g) {
    uint32_t now = sys_uptime_ms();
    for (int i = 0; i < 9; i++)
        if (g->b[i] && now - g->placed_at[i] < MARK_MS + 20) return true;
    return g->win_line >= 0 && now - g->line_at < LINE_MS + 20;
}

static void tt_ensure_timer(ttt_t *g) {
    if (g->timer <= 0) g->timer = win_set_timer(g->win, 16);
}

/* ---- game flow ------------------------------------------------------------------------ */
static void tt_schedule_ai(ttt_t *g) {
    if (g->mode != MODE_AI || g->result != P_NONE || g->turn != P_O) return;
    g->ai_pending = true;
    g->ai_due = sys_uptime_ms() + AI_DELAY;
    tt_ensure_timer(g);
}

static void tt_new_game(ttt_t *g, bool alternate) {
    if (alternate) g->starter = 3 - g->starter;
    memset(g->b, 0, sizeof g->b);
    g->turn = g->starter;
    g->result = P_NONE;
    g->win_line = -1;
    g->ai_pending = false;
    tt_schedule_ai(g);
    win_invalidate(g->win);
}

static void tt_place(ttt_t *g, int i) {
    if (i < 0 || i > 8 || g->b[i] || g->result != P_NONE) return;
    g->b[i] = (int8_t)g->turn;
    g->placed_at[i] = sys_uptime_ms();
    sys_beep(g->turn == P_X ? 740 : 520, 18);
    int line = -1;
    int r = tt_eval(g->b, &line);
    if (r != P_NONE) {
        g->result = r;
        g->score[r]++;
        if (r != P_DRAW) {
            g->win_line = line;
            g->line_at = sys_uptime_ms() + MARK_MS;   /* strike after the last mark is drawn */
        }
        bool human_lost = g->mode == MODE_AI && r == P_O;
        if (r == P_DRAW) {
            static const note_t tune[] = { { 440, 90 }, { 440, 90 } };
            sys_play_notes(tune, ARRAY_SIZE(tune));
        } else if (human_lost) {
            static const note_t tune[] = { { 392, 120 }, { 330, 120 }, { 262, 220 } };
            sys_play_notes(tune, ARRAY_SIZE(tune));
        } else {
            static const note_t tune[] = { { 523, 90 }, { 659, 90 }, { 784, 90 }, { 1047, 200 } };
            sys_play_notes(tune, ARRAY_SIZE(tune));
        }
    } else {
        g->turn = 3 - g->turn;
        tt_schedule_ai(g);
    }
    tt_ensure_timer(g);
    tt_inval_board(g);
    tt_inval_info(g);
}

static void tt_set_mode(ttt_t *g, int mode) {
    g->mode = CLAMP(mode, 0, 1);
    g->score[P_X] = g->score[P_O] = g->score[P_DRAW] = 0;
    g->starter = P_X;
    pz_score_set("ttt_mode", g->mode);
    tt_new_game(g, false);
}

/* ---- drawing -------------------------------------------------------------------------- */
static void draw_x(surface_t *s, double cx, double cy, double r, double wdt, double p, uint32_t col) {
    double p1 = CLAMP(p * 2, 0.0, 1.0), p2 = CLAMP(p * 2 - 1, 0.0, 1.0);
    if (p1 > 0) pz_stroke(s, cx - r, cy - r, cx - r + 2 * r * p1, cy - r + 2 * r * p1, wdt, col);
    if (p2 > 0) pz_stroke(s, cx + r, cy - r, cx + r - 2 * r * p2, cy - r + 2 * r * p2, wdt, col);
}

static void draw_o(surface_t *s, double cx, double cy, double r, double wdt, double p, uint32_t col) {
    if (p <= 0) return;
    if (p >= 1) { gfx_circle_aa(s, cx, cy, r, wdt, col); return; }
    pz_arc(s, cx, cy, r, -M_PI / 2, -M_PI / 2 + 2 * M_PI * p, wdt, col);
}

static void tt_draw_mark(surface_t *s, rect_t r, int p, double prog, uint32_t col) {
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0, wdt = r.w * 0.115;
    if (p == P_X) draw_x(s, cx, cy, r.w * 0.24, wdt, prog, col);
    else draw_o(s, cx, cy, r.w * 0.27, wdt, prog, col);
}

static void tt_draw_score_card(surface_t *s, rect_t r, const char *label, int value, uint32_t col, bool active,
                               int mark) {
    const theme_t *t = theme_get();
    uint32_t bg = t->dark ? color_mix(t->panel_bg, COLOR_WHITE, 8) : t->panel_bg;
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 10, active ? color_mix(t->window_border, col, 200) : t->window_border);
    gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 9, bg);
    if (active) {
        gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 9, color_alpha(col, t->dark ? 34 : 22));
        gfx_fill_rounded_rect(s, r.x + 14, r.y + r.h - 5, r.w - 28, 3, 1, col);
    }
    /* small mark glyph next to the label */
    int lw = font_text_width(FONT_UI, label) + (mark ? 16 : 0);
    int lx = r.x + (r.w - lw) / 2;
    if (mark) {
        rect_t mr = rect_make(lx - 2, r.y + 8, 16, 16);
        double cx = mr.x + 7, cy = mr.y + 8.5;
        if (mark == P_X) draw_x(s, cx, cy, 3.6, 2.2, 1, col);
        else gfx_circle_aa(s, cx, cy, 5.2, 2.2, col);
        lx += 16;
    }
    gfx_text(s, lx, r.y + 9, label, pz_muted(), FONT_UI);
    char buf[16];
    snprintf(buf, sizeof buf, "%d", value);
    gfx_text_aligned(s, rect_make(r.x, r.y + 26, r.w, 36), buf, mark ? col : t->window_text, FONT_UI_TITLE,
                     ALIGN_CENTER | ALIGN_MIDDLE);
}

static void tt_paint(ttt_t *g, surface_t *s) {
    const theme_t *t = theme_get();
    uint32_t now = sys_uptime_ms();
    pz_seg_draw(s, g->seg_r, mode_names, 2, g->mode, g->seg_hover);

    /* score cards */
    int cw = (g->score_r.w - 2 * 10) / 3;
    const char *lx = g->mode == MODE_AI ? "You" : "Player X";
    const char *lo = g->mode == MODE_AI ? "Computer" : "Player O";
    bool playing = g->result == P_NONE;
    tt_draw_score_card(s, rect_make(g->score_r.x, g->score_r.y, cw, g->score_r.h), lx, g->score[P_X], tt_color(P_X),
                       (playing && g->turn == P_X) || g->result == P_X, P_X);
    tt_draw_score_card(s, rect_make(g->score_r.x + cw + 10, g->score_r.y, cw, g->score_r.h), "Draws",
                       g->score[P_DRAW], t->window_text, g->result == P_DRAW, 0);
    tt_draw_score_card(s, rect_make(g->score_r.x + 2 * (cw + 10), g->score_r.y, g->score_r.w - 2 * (cw + 10),
                                    g->score_r.h), lo, g->score[P_O], tt_color(P_O),
                       (playing && g->turn == P_O) || g->result == P_O, P_O);

    /* board */
    rect_t br = g->board_r;
    gfx_shadow(s, br.x - 10, br.y - 10, br.w + 20, br.h + 20, 14, 8, t->dark ? 110 : 40);
    uint32_t board_bg = t->dark ? color_mix(t->window_bg, COLOR_BLACK, 90) : color_mix(t->window_border, t->accent, 40);
    gfx_fill_rounded_rect(s, br.x - 10, br.y - 10, br.w + 20, br.h + 20, 16, board_bg);
    uint32_t tile = t->dark ? color_mix(t->panel_bg, COLOR_WHITE, 14) : COLOR_WHITE;
    bool over = g->result != P_NONE;
    bool line_done = g->win_line >= 0 && now >= g->line_at + LINE_MS;
    for (int i = 0; i < 9; i++) {
        rect_t r = tt_cell_rect(g, i);
        bool in_line = false;
        if (g->win_line >= 0)
            for (int k = 0; k < 3; k++) if (lines[g->win_line][k] == i) in_line = true;
        uint32_t bg = tile;
        if (in_line && now >= g->line_at) bg = color_mix(tile, tt_color(g->result), t->dark ? 60 : 40);
        else if (!over && !g->b[i] && i == g->hover && tt_human_turn(g)) bg = color_mix(tile, t->accent, 22);
        gfx_fill_rounded_rect(s, r.x, r.y + 2, r.w, r.h, 12, RGBA(0, 0, 0, t->dark ? 70 : 26));
        gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 12, bg);
        if (g->b[i]) {
            uint32_t col = tt_color(g->b[i]);
            if (over && !in_line && (g->win_line >= 0 ? line_done : true))
                col = color_mix(col, bg, g->result == P_DRAW ? 70 : 150);
            double prog = ease_out((double)(int32_t)(now - g->placed_at[i]) / MARK_MS);
            tt_draw_mark(s, r, g->b[i], prog, col);
        } else if (!over && i == g->hover && tt_human_turn(g)) {
            tt_draw_mark(s, r, g->turn, 1, color_mix(bg, tt_color(g->turn), 70));
        }
        if (g->kb_on && i == g->kb && !over) {
            gfx_rounded_rect(s, r.x - 3, r.y - 3, r.w + 6, r.h + 6, 14, t->accent);
            gfx_rounded_rect(s, r.x - 2, r.y - 2, r.w + 4, r.h + 4, 13, t->accent);
        }
    }
    /* strike-through */
    if (g->win_line >= 0 && now >= g->line_at) {
        double p = ease_out((double)(now - g->line_at) / LINE_MS);
        rect_t a = tt_cell_rect(g, lines[g->win_line][0]), c = tt_cell_rect(g, lines[g->win_line][2]);
        double x0 = a.x + a.w / 2.0, y0 = a.y + a.h / 2.0, x1 = c.x + c.w / 2.0, y1 = c.y + c.h / 2.0;
        double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
        double ext = TT_CELL * 0.36;
        x0 -= dx / len * ext; y0 -= dy / len * ext;
        x1 += dx / len * ext; y1 += dy / len * ext;
        uint32_t col = tt_color(g->result);
        double ex = x0 + (x1 - x0) * p, ey = y0 + (y1 - y0) * p;
        pz_stroke(s, x0, y0 + 2, ex, ey + 2, 12, color_mix(col, COLOR_BLACK, 90));
        pz_stroke(s, x0, y0, ex, ey, 12, col);
        pz_stroke(s, x0, y0 - 1.5, ex, ey - 1.5, 3, color_mix(col, COLOR_WHITE, 90));
    }

    /* status line */
    char msg[64];
    uint32_t mc = t->window_text;
    if (g->result == P_DRAW) {
        snprintf(msg, sizeof msg, "It's a draw!");
    } else if (g->result != P_NONE) {
        mc = tt_color(g->result);
        if (g->mode == MODE_AI) snprintf(msg, sizeof msg, g->result == P_X ? "You win!" : "The computer wins!");
        else snprintf(msg, sizeof msg, "%s wins!", g->result == P_X ? "X" : "O");
    } else if (g->mode == MODE_AI) {
        if (g->turn == P_X) snprintf(msg, sizeof msg, "Your move");
        else { snprintf(msg, sizeof msg, "Computer is thinking..."); mc = pz_muted(); }
    } else {
        snprintf(msg, sizeof msg, "%s to move", g->turn == P_X ? "X" : "O");
        mc = tt_color(g->turn);
    }
    gfx_text_aligned(s, rect_make(0, g->status_r.y, TT_W, 24), msg, mc, FONT_UI_LARGE, ALIGN_CENTER | ALIGN_MIDDLE);
    const char *hint = over ? "Click the board or press Enter for the next game"
                            : (g->kb_on ? "Arrows move, Space places, 1-9 pick a square" : "Click a square to place your mark");
    gfx_text_aligned(s, rect_make(0, g->status_r.y + 24, TT_W, 16), hint, pz_muted(), FONT_UI, ALIGN_CENTER);

    pz_btn_draw(s, &g->btn[0]);
    pz_btn_draw(s, &g->btn[1]);
}

/* ---- events ----------------------------------------------------------------------------- */
static void tt_set_hover(ttt_t *g, int cell) {
    if (cell == g->hover) return;
    if (g->hover >= 0) { rect_t r = tt_cell_rect(g, g->hover); win_invalidate_rect(g->win, r.x - 4, r.y - 4, r.w + 8, r.h + 8); }
    g->hover = cell;
    if (cell >= 0) { rect_t r = tt_cell_rect(g, cell); win_invalidate_rect(g->win, r.x - 4, r.y - 4, r.w + 8, r.h + 8); }
}

static void tt_human_move(ttt_t *g, int cell) {
    if (g->result != P_NONE) { tt_new_game(g, true); return; }
    if (!tt_human_turn(g) || cell < 0 || g->b[cell]) return;
    tt_place(g, cell);
}

static void tt_button(ttt_t *g, int which) {
    if (which == 0) {
        tt_new_game(g, g->result != P_NONE);
    } else if (which == 1) {
        g->score[P_X] = g->score[P_O] = g->score[P_DRAW] = 0;
        g->starter = P_X;
        tt_new_game(g, false);
    }
}

static void tt_mouse(ttt_t *g, const event_t *ev) {
    bool used = false;
    int fired = pz_btns_event(g->win, g->btn, 2, ev, &used);
    if (fired >= 0) { tt_button(g, fired); return; }
    if (used) return;
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int sh = pz_seg_hit(g->seg_r, 2, ev->x, ev->y);
        if (sh != g->seg_hover) {
            g->seg_hover = sh;
            win_invalidate_rect(g->win, g->seg_r.x, g->seg_r.y, g->seg_r.w, g->seg_r.h);
        }
        int cell = tt_cell_at(g, ev->x, ev->y);
        if (cell >= 0 && g->kb_on) { g->kb_on = false; tt_inval_board(g); tt_inval_info(g); }
        tt_set_hover(g, cell);
        break;
    }
    case EV_MOUSE_LEAVE:
        tt_set_hover(g, -1);
        if (g->seg_hover >= 0) { g->seg_hover = -1; win_invalidate(g->win); }
        break;
    case EV_MOUSE_DOWN: {
        if (ev->button != MOUSE_LEFT) break;
        int sg = pz_seg_hit(g->seg_r, 2, ev->x, ev->y);
        if (sg >= 0) {
            if (sg != g->mode) tt_set_mode(g, sg);
            break;
        }
        if (rect_contains(rect_inset(g->board_r, -10), ev->x, ev->y)) {
            int cell = tt_cell_at(g, ev->x, ev->y);
            if (g->result != P_NONE || cell >= 0) tt_human_move(g, cell);
        }
        break;
    }
    default:
        break;
    }
}

static void tt_key(ttt_t *g, const event_t *ev) {
    int k = ev->key;
    if (k == KEY_F2 || k == 'n') { tt_button(g, 0); return; }
    if (k == 'm') { tt_set_mode(g, 1 - g->mode); return; }
    if (k >= '1' && k <= '9') {
        if (g->kb_on) { g->kb_on = false; tt_inval_board(g); tt_inval_info(g); }
        tt_human_move(g, k - '1');
        return;
    }
    int dx = 0, dy = 0;
    if (k == KEY_LEFT) dx = -1;
    else if (k == KEY_RIGHT) dx = 1;
    else if (k == KEY_UP) dy = -1;
    else if (k == KEY_DOWN) dy = 1;
    if (dx || dy) {
        if (g->kb_on) {
            int x = CLAMP(g->kb % 3 + dx, 0, 2), y = CLAMP(g->kb / 3 + dy, 0, 2);
            g->kb = y * 3 + x;
        } else {
            g->kb_on = true;
            tt_inval_info(g);
        }
        tt_inval_board(g);
        return;
    }
    if (k == KEY_SPACE || k == KEY_ENTER) {
        if (g->result != P_NONE) { tt_new_game(g, true); return; }
        if (!g->kb_on) { g->kb_on = true; tt_inval_board(g); tt_inval_info(g); return; }
        tt_human_move(g, g->kb);
    }
}

static void tt_timer(ttt_t *g) {
    if (g->ai_pending && (int32_t)(sys_uptime_ms() - g->ai_due) >= 0) {
        g->ai_pending = false;
        if (g->mode == MODE_AI && g->result == P_NONE && g->turn == P_O) tt_place(g, tt_best_move(g, P_O));
    }
    tt_inval_board(g);
    if (!g->ai_pending && !tt_animating(g)) {
        win_kill_timer(g->win, g->timer);
        g->timer = 0;
        tt_inval_board(g);
    }
}

static void tt_event(window_t *win, const event_t *ev) {
    ttt_t *g = win_get_userdata(win);
    if (!g) return;
    switch (ev->type) {
    case EV_PAINT:
        tt_paint(g, win_surface(win));
        break;
    case EV_TIMER:
        tt_timer(g);
        break;
    case EV_MOUSE_MOVE: case EV_MOUSE_DOWN: case EV_MOUSE_UP: case EV_MOUSE_LEAVE: case EV_DOUBLE_CLICK:
        tt_mouse(g, ev);
        break;
    case EV_KEY_DOWN:
        tt_key(g, ev);
        break;
    case EV_BLUR:
        for (int i = 0; i < 2; i++) g->btn[i].pressed = g->btn[i].hover = false;
        win_invalidate(win);
        break;
    case EV_CLOSE:
        free(g);
        win_set_userdata(win, NULL);
        break;
    default:
        break;
    }
}

static void tt_launch(const char *arg) {
    (void)arg;
    ttt_t *g = calloc(1, sizeof *g);
    if (!g) return;
    pz_rng_seed(&g->rng, (uint32_t)(uintptr_t)g);
    g->hover = g->seg_hover = g->win_line = -1;
    g->kb = 4;
    g->seg_r = rect_make((TT_W - TT_SEG_W) / 2, TT_SEG_Y, TT_SEG_W, TT_SEG_H);
    g->score_r = rect_make(24, TT_SCORE_Y, TT_W - 48, TT_SCORE_H);
    g->board_r = rect_make((TT_W - TT_BOARD) / 2, TT_BOARD_Y, TT_BOARD, TT_BOARD);
    g->status_r = rect_make(0, TT_STATUS_Y, TT_W, 40);
    int bw = 132;
    g->btn[0] = (pz_btn_t){ .r = rect_make(TT_W / 2 - bw - 6, TT_BTN_Y, bw, 32), .label = "New Game", .primary = true };
    g->btn[1] = (pz_btn_t){ .r = rect_make(TT_W / 2 + 6, TT_BTN_Y, bw, 32), .label = "Reset Score" };
    g->win = win_create("Tic-Tac-Toe", TT_W, TT_H, WIN_CENTER);
    if (!g->win) { free(g); return; }
    win_set_userdata(g->win, g);
    win_set_event_handler(g->win, tt_event);
    g->mode = CLAMP(pz_score_get("ttt_mode", MODE_AI), 0, 1);
    g->starter = P_X;
    tt_new_game(g, false);
}

/* Icon: a rounded tile with a white # grid, a blue X and a coral O. */
static void tt_icon(surface_t *s, int x, int y, int size) {
    double u = size / 48.0;
    gfx_fill_rounded_gradient_v(s, x + (int)(3 * u), y + (int)(3 * u), (int)(42 * u), (int)(42 * u), (int)(10 * u),
                                RGB(132, 96, 236), RGB(78, 52, 182));
    uint32_t grid = RGB(244, 240, 255);
    double g0 = 3 + 42 / 3.0, g1 = 3 + 2 * 42 / 3.0;
    pz_stroke(s, x + g0 * u, y + 9 * u, x + g0 * u, y + 39 * u, 2.6 * u, grid);
    pz_stroke(s, x + g1 * u, y + 9 * u, x + g1 * u, y + 39 * u, 2.6 * u, grid);
    pz_stroke(s, x + 9 * u, y + g0 * u, x + 39 * u, y + g0 * u, 2.6 * u, grid);
    pz_stroke(s, x + 9 * u, y + g1 * u, x + 39 * u, y + g1 * u, 2.6 * u, grid);
    /* X top-left, O centre, X bottom-right */
    uint32_t xc = RGB(120, 200, 255), oc = RGB(255, 150, 120);
    double c0 = 3 + 42 / 6.0, c1 = 24, c2 = 45 - 42 / 6.0, r = 4.2;
    pz_stroke(s, x + (c0 - r) * u, y + (c0 - r) * u, x + (c0 + r) * u, y + (c0 + r) * u, 3 * u, xc);
    pz_stroke(s, x + (c0 + r) * u, y + (c0 - r) * u, x + (c0 - r) * u, y + (c0 + r) * u, 3 * u, xc);
    gfx_circle_aa(s, x + c1 * u, y + c1 * u, 6 * u, 3 * u, oc);
    pz_stroke(s, x + (c2 - r) * u, y + (c2 - r) * u, x + (c2 + r) * u, y + (c2 + r) * u, 3 * u, xc);
    pz_stroke(s, x + (c2 + r) * u, y + (c2 - r) * u, x + (c2 - r) * u, y + (c2 + r) * u, 3 * u, xc);
}

REGISTER_APP(.name = "Tic-Tac-Toe", .category = CAT_GAMES,
             .description = "Noughts and crosses against an unbeatable computer or a friend",
             .launch = tt_launch, .draw_icon = tt_icon);
