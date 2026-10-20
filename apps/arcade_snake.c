/*
 * arcade_snake.c - Snake (Games).
 *
 * Classic grid snake: eat apples to grow, every apple speeds the snake up,
 * walls and your own tail are deadly. A golden bonus apple shows up now and
 * then for a few seconds. Score + persistent high score, pause, game over and
 * restart.
 *
 * Movement is a fixed-step simulation (one cell per `interval` ms) driven by
 * a 16 ms window timer that accumulates sys_uptime_ms() deltas; rendering
 * interpolates between steps so the snake glides smoothly: the head segment
 * grows from the previous head cell into the new one while the tail retracts
 * out of the cell it just left. Turns are buffered (up to 3) so quick
 * key sequences such as Up-Left are never lost.
 *
 * Keys: arrows / WASD steer (and start), P / Space / Esc pause,
 * Enter restarts after game over, F2 / N new game.
 */
#include "arcade_common.h"

#define COLS 22
#define ROWS 18
#define CELL 22
#define MAXLEN (COLS * ROWS)
#define BX 16
#define BY 16
#define BW (COLS * CELL)
#define BH (ROWS * CELL)
#define PX (BX + BW + 20)
#define PW 164
#define CW (PX + PW + 16)
#define CH (BY + BH + 16)

#define START_INTERVAL 150
#define MIN_INTERVAL 62
#define BONUS_LIFE 6000
#define NPOP 4

enum { ST_READY, ST_RUNNING, ST_PAUSED, ST_OVER };
enum { D_UP, D_RIGHT, D_DOWN, D_LEFT };
static const int DX[4] = { 0, 1, 0, -1 };
static const int DY[4] = { -1, 0, 1, 0 };

enum { BTN_PAUSE, BTN_NEW, NBTN };

typedef struct { int8_t x, y; } cell_t;

typedef struct {
    double x, y;
    int value;
    uint32_t age;      /* ms; dead when >= 800 */
    bool gold;
} popup_t;

typedef struct {
    window_t *win;
    arcade_rng_t rng;
    int state;
    cell_t ring[MAXLEN];   /* body ring buffer, ring[head] = head cell */
    int head, len;
    uint8_t occ[ROWS][COLS];
    cell_t prev_tail;      /* cell vacated by the last step (tail interpolation) */
    bool grew;             /* last step ate an apple: tail did not move           */
    int dir;               /* direction of the last step                          */
    int queue[3], nq;      /* buffered turns                                      */
    cell_t food;
    cell_t bonus;
    bool bonus_on;
    uint32_t bonus_left;
    int eaten, score, best, level;
    bool new_best, won;
    uint32_t interval, acc, last_ms, clock;
    popup_t pops[NPOP];
    arcade_btn_t btn[NBTN];
    int hover, press;
    bool autopilot;        /* "demo" launch argument: the snake plays itself (attract mode / tests) */
} snake_t;

static cell_t seg(const snake_t *g, int i) { return g->ring[(g->head - i + MAXLEN) % MAXLEN]; }

static void invalidate_board(snake_t *g) { win_invalidate_rect(g->win, BX - 6, BY - 6, BW + 12, BH + 12); }
static void invalidate_panel(snake_t *g) { win_invalidate_rect(g->win, PX - 4, 0, CW - PX + 4, CH); }

/* Pick a random free cell (not snake, not the other apple). Returns false if the board is full. */
static bool random_free_cell(snake_t *g, cell_t *out, const cell_t *avoid) {
    int free_cells = 0;
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
            if (!g->occ[y][x] && !(avoid && avoid->x == x && avoid->y == y)) free_cells++;
    if (!free_cells) return false;
    int k = arcade_rand_n(&g->rng, free_cells);
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
            if (!g->occ[y][x] && !(avoid && avoid->x == x && avoid->y == y) && k-- == 0) {
                out->x = (int8_t)x;
                out->y = (int8_t)y;
                return true;
            }
    return false;
}

static void new_game(snake_t *g) {
    memset(g->occ, 0, sizeof g->occ);
    int r = ROWS / 2;
    g->len = 3;
    g->head = 2;
    for (int i = 0; i < 3; i++) {
        g->ring[i].x = (int8_t)(4 + i);
        g->ring[i].y = (int8_t)r;
        g->occ[r][4 + i] = 1;
    }
    g->prev_tail.x = 3;
    g->prev_tail.y = (int8_t)r;
    g->grew = false;
    g->dir = D_RIGHT;
    g->nq = 0;
    g->eaten = g->score = 0;
    g->level = 1;
    g->interval = START_INTERVAL;
    g->acc = 0;
    g->new_best = g->won = false;
    g->bonus_on = false;
    g->autopilot = false;
    memset(g->pops, 0, sizeof g->pops);
    random_free_cell(g, &g->food, NULL);
    g->state = ST_READY;
    win_invalidate(g->win);
}

static void add_popup(snake_t *g, cell_t c, int value, bool gold) {
    int slot = 0;
    for (int i = 0; i < NPOP; i++)
        if (g->pops[i].age >= 800 || g->pops[i].value == 0) { slot = i; break; }
        else if (g->pops[i].age > g->pops[slot].age) slot = i;
    popup_t *p = &g->pops[slot];
    p->x = BX + c.x * CELL + CELL / 2.0;
    p->y = BY + c.y * CELL + CELL / 2.0;
    p->value = value;
    p->age = 0;
    p->gold = gold;
}

static void game_over(snake_t *g, bool won) {
    g->state = ST_OVER;
    g->won = won;
    g->acc = g->interval;          /* render the final position */
    int stored = arcade_hiscore_get("snake");   /* another Snake window may have set a higher one */
    if (stored > g->best) g->best = stored;
    if (g->score > g->best) {
        g->best = g->score;
        g->new_best = true;
        arcade_hiscore_set("snake", g->best);
    }
    if (won) arcade_sound_fanfare(); else arcade_sound_gameover();
    klog("snake: game over, score %d, length %d, best %d", g->score, g->len, g->best);
    win_invalidate(g->win);
}

static bool cell_free(const snake_t *g, int x, int y) {
    if (x < 0 || y < 0 || x >= COLS || y >= ROWS) return false;
    if (!g->occ[y][x]) return true;
    cell_t t = seg(g, g->len - 1);
    return t.x == x && t.y == y;     /* the tail moves away this step */
}

/* Greedy autopilot: head for the nearest apple, avoid moves into dead ends. */
static void autopilot(snake_t *g) {
    cell_t h = seg(g, 0);
    cell_t target = g->bonus_on ? g->bonus : g->food;
    int best = -1, best_cost = INT_MAX;
    for (int d = 0; d < 4; d++) {
        if (d == (g->dir + 2) % 4) continue;
        int nx = h.x + DX[d], ny = h.y + DY[d];
        if (!cell_free(g, nx, ny)) continue;
        int exits = 0;
        for (int e = 0; e < 4; e++)
            if (cell_free(g, nx + DX[e], ny + DY[e]) && !(nx + DX[e] == h.x && ny + DY[e] == h.y)) exits++;
        int cost = abs(nx - target.x) + abs(ny - target.y) + (exits == 0 ? 1000 : 0) + (d == g->dir ? 0 : 1);
        if (cost < best_cost) { best_cost = cost; best = d; }
    }
    g->nq = 0;
    if (best >= 0) g->dir = best;
}

static void step(snake_t *g) {
    if (g->autopilot) autopilot(g);
    if (g->nq) {
        int d = g->queue[0];
        memmove(g->queue, g->queue + 1, sizeof(int) * (size_t)(g->nq - 1));
        g->nq--;
        if (d != (g->dir + 2) % 4) g->dir = d;
    }
    cell_t h = seg(g, 0);
    int nx = h.x + DX[g->dir], ny = h.y + DY[g->dir];
    if (nx < 0 || ny < 0 || nx >= COLS || ny >= ROWS) { game_over(g, false); return; }
    bool eat = nx == g->food.x && ny == g->food.y;
    bool eat_bonus = g->bonus_on && nx == g->bonus.x && ny == g->bonus.y;
    cell_t tail = seg(g, g->len - 1);
    if (!eat) g->occ[tail.y][tail.x] = 0;             /* the tail moves out of the way */
    if (g->occ[ny][nx]) {
        if (!eat) g->occ[tail.y][tail.x] = 1;
        game_over(g, false);
        return;
    }
    g->head = (g->head + 1) % MAXLEN;
    g->ring[g->head].x = (int8_t)nx;
    g->ring[g->head].y = (int8_t)ny;
    g->occ[ny][nx] = 1;
    cell_t nc = g->ring[g->head];
    if (eat) {
        g->len++;
        g->grew = true;
        g->eaten++;
        int pts = 10 * g->level;
        g->score += pts;
        add_popup(g, nc, pts, false);
        g->level = 1 + g->eaten / 5;
        int iv = START_INTERVAL - g->eaten * 3;
        g->interval = (uint32_t)MAX(MIN_INTERVAL, iv);
        sys_beep(880, 25);
        if (!random_free_cell(g, &g->food, g->bonus_on ? &g->bonus : NULL)) { game_over(g, true); return; }
        if (!g->bonus_on && g->eaten % 4 == 0) {
            if (random_free_cell(g, &g->bonus, &g->food)) {
                g->bonus_on = true;
                g->bonus_left = BONUS_LIFE;
            }
        }
        invalidate_panel(g);
    } else {
        g->prev_tail = tail;
        g->grew = false;
    }
    if (eat_bonus) {
        int pts = 25 * g->level + 25;
        g->score += pts;
        g->bonus_on = false;
        add_popup(g, nc, pts, true);
        sys_beep(1320, 40);
        invalidate_panel(g);
    }
}

static void start(snake_t *g) {
    g->state = ST_RUNNING;
    g->acc = 0;
    g->last_ms = sys_uptime_ms();
    step(g);                /* first step right away: the drawing continues from the shown snake */
    win_invalidate(g->win);
}

static void set_paused(snake_t *g, bool paused) {
    if (paused && g->state == ST_RUNNING) g->state = ST_PAUSED;
    else if (!paused && g->state == ST_PAUSED) {
        g->state = ST_RUNNING;
        g->last_ms = sys_uptime_ms();
    } else return;
    win_invalidate(g->win);
}

static void steer(snake_t *g, int d) {
    int last = g->nq ? g->queue[g->nq - 1] : g->dir;
    if (d == last || d == (last + 2) % 4) return;
    if (g->nq < 3) g->queue[g->nq++] = d;
}

/* ---- drawing ------------------------------------------------------------------------- */
static void cell_center(cell_t c, double *x, double *y) {
    *x = BX + c.x * CELL + CELL / 2.0;
    *y = BY + c.y * CELL + CELL / 2.0;
}

static void draw_apple(surface_t *s, double cx, double cy, double r, bool gold) {
    uint32_t body = gold ? RGB(250, 196, 40) : RGB(232, 52, 48);
    uint32_t dark = gold ? RGB(196, 132, 16) : RGB(170, 28, 30);
    gfx_fill_ellipse(s, (int)cx, (int)(cy + r * 0.85), (int)(r * 0.8), (int)(r * 0.3 + 1), RGBA(0, 0, 0, 50));
    gfx_fill_circle_aa(s, cx, cy + 0.5, r, dark);
    gfx_fill_circle_aa(s, cx, cy, r - 1, body);
    gfx_fill_circle_aa(s, cx - r * 0.35, cy - r * 0.35, r * 0.3, color_alpha(COLOR_WHITE, 150));
    if (gold) {
        /* a little sparkle */
        gfx_fill_circle_aa(s, cx + r * 0.45, cy + r * 0.2, r * 0.14, color_alpha(COLOR_WHITE, 200));
    } else {
        gfx_thick_line(s, (int)cx, (int)(cy - r + 1), (int)(cx + 1), (int)(cy - r - 3), 2, RGB(110, 70, 30));
    }
    gfx_fill_ellipse(s, (int)(cx + r * 0.45), (int)(cy - r - 1), (int)(r * 0.42 + 1), (int)(r * 0.22 + 1), RGB(70, 170, 60));
}

static void draw_board(snake_t *g, surface_t *s) {
    const theme_t *t = theme_get();
    bool dk = t->dark;
    uint32_t c1 = dk ? RGB(52, 86, 44) : RGB(170, 215, 81);
    uint32_t c2 = dk ? RGB(46, 78, 39) : RGB(162, 209, 73);
    uint32_t frame = dk ? RGB(30, 52, 28) : RGB(87, 138, 52);
    gfx_shadow(s, BX - 5, BY - 5, BW + 10, BH + 10, 10, 6, 40);
    gfx_fill_rounded_rect(s, BX - 5, BY - 5, BW + 10, BH + 10, 9, frame);
    gfx_fill_rect(s, BX, BY, BW, BH, c1);
    for (int y = 0; y < ROWS; y++)
        for (int x = (y & 1); x < COLS; x += 2)
            gfx_fill_rect(s, BX + x * CELL, BY + y * CELL, CELL, CELL, c2);

    /* apples */
    double pulse = (g->state == ST_RUNNING) ? sin(g->clock / 160.0) * 0.8 : 0;
    double fx, fy;
    cell_center(g->food, &fx, &fy);
    draw_apple(s, fx, fy, CELL * 0.36 + pulse, false);
    if (g->bonus_on) {
        bool blink = g->bonus_left < 1800 && (g->bonus_left / 150) % 2;
        if (!blink) {
            cell_center(g->bonus, &fx, &fy);
            draw_apple(s, fx, fy, CELL * 0.40 + pulse, true);
        }
    }

    /* snake: chain of points from head to tail, interpolated between steps */
    double tt = g->interval ? (double)g->acc / g->interval : 1;
    if (g->state == ST_READY || g->state == ST_OVER) tt = 1;
    if (tt > 1) tt = 1;
    int n = 0;
    static double px[MAXLEN + 2], py[MAXLEN + 2];   /* only used inside this call */
    double hx, hy, ax, ay;
    cell_center(seg(g, 0), &hx, &hy);
    cell_center(seg(g, 1), &ax, &ay);
    px[n] = ax + (hx - ax) * tt; py[n] = ay + (hy - ay) * tt; n++;
    for (int i = 1; i < g->len; i++) { cell_center(seg(g, i), &px[n], &py[n]); n++; }
    if (!g->grew && g->state != ST_READY) {
        double ex, ey;
        cell_center(g->prev_tail, &ex, &ey);
        double lx = px[n - 1], ly = py[n - 1];
        px[n] = ex + (lx - ex) * tt; py[n] = ey + (ly - ey) * tt; n++;
    }
    uint32_t head_c = color_shade(t->accent | 0xFF000000u, 10);
    uint32_t tail_c = color_mix(head_c, dk ? RGB(20, 30, 60) : RGB(30, 50, 110), 120);
    double w = CELL * 0.70;
    for (int i = n - 2; i >= 0; i--) {
        uint32_t c = color_mix(head_c, tail_c, n > 2 ? i * 255 / (n - 2) : 0);
        arcade_capsule(s, px[i], py[i], px[i + 1], py[i + 1], w, c);
    }
    /* head */
    double hr = CELL * 0.42;
    gfx_fill_circle_aa(s, px[0], py[0], hr, head_c);
    double fdx = DX[g->dir], fdy = DY[g->dir];
    double pdx = -fdy, pdy = fdx;
    for (int k = -1; k <= 1; k += 2) {
        double ex = px[0] + fdx * 3 + pdx * 4.6 * k, ey = py[0] + fdy * 3 + pdy * 4.6 * k;
        if (g->state == ST_OVER && !g->won) {
            gfx_line_aa(s, ex - 2.5, ey - 2.5, ex + 2.5, ey + 2.5, RGB(255, 255, 255));
            gfx_line_aa(s, ex - 2.5, ey + 2.5, ex + 2.5, ey - 2.5, RGB(255, 255, 255));
            gfx_line_aa(s, ex - 2.5 + 0.7, ey - 2.5, ex + 2.5 + 0.7, ey + 2.5, RGB(255, 255, 255));
            gfx_line_aa(s, ex - 2.5 + 0.7, ey + 2.5, ex + 2.5 + 0.7, ey - 2.5, RGB(255, 255, 255));
        } else {
            gfx_fill_circle_aa(s, ex, ey, 3.4, COLOR_WHITE);
            gfx_fill_circle_aa(s, ex + fdx * 1.2, ey + fdy * 1.2, 1.8, RGB(20, 24, 36));
        }
    }

    /* score popups */
    for (int i = 0; i < NPOP; i++) {
        popup_t *p = &g->pops[i];
        if (!p->value || p->age >= 800) continue;
        char buf[16];
        snprintf(buf, sizeof buf, "+%d", p->value);
        int a = 255 - (int)(p->age * 255 / 800);
        double yy = p->y - 14 - p->age / 30.0;
        int tw = font_text_width(FONT_UI_BOLD, buf);
        uint32_t col = dk ? (p->gold ? RGB(255, 226, 110) : COLOR_WHITE) : (p->gold ? RGB(150, 92, 0) : RGB(32, 62, 20));
        uint32_t sh = dk ? RGBA(0, 0, 0, a / 2) : RGBA(255, 255, 255, a / 3);
        gfx_text(s, (int)p->x - tw / 2 + 1, (int)yy + 1, buf, sh, FONT_UI_BOLD);
        gfx_text(s, (int)p->x - tw / 2, (int)yy, buf, color_alpha(col, a), FONT_UI_BOLD);
    }

    /* overlays */
    rect_t area = rect_make(BX, BY, BW, BH);
    char l1[64];
    if (g->state == ST_READY) {
        arcade_overlay(s, area, 40, 320, 128, "Snake", 0, "Eat apples to grow. Avoid walls and your tail.",
                       "Press an arrow key to start");
    } else if (g->state == ST_PAUSED) {
        arcade_overlay(s, area, 110, 280, 112, "Paused", 0, "Press P or Space to resume", NULL);
    } else if (g->state == ST_OVER) {
        if (g->new_best) snprintf(l1, sizeof l1, "Score %d  -  new high score!", g->score);
        else snprintf(l1, sizeof l1, "Score %d", g->score);
        arcade_overlay(s, area, 110, 300, 128, g->won ? "You win!" : "Game over",
                       g->won ? RGB(40, 170, 80) : 0, l1, "Press Enter to play again");
    }
}

static void draw_panel(snake_t *g, surface_t *s) {
    const theme_t *t = theme_get();
    char buf[32];
    int x = PX, y = BY;
    /* header: icon + name */
    const app_desc_t *app = app_find("Snake");
    if (app) app_draw_icon(s, app, x, y, 32);
    gfx_text(s, x + 40, y + 5, "Snake", t->window_text, FONT_UI_LARGE);
    y += 46;
    snprintf(buf, sizeof buf, "%d", g->score);
    arcade_card(s, x, y, PW, 66, "SCORE", buf, FONT_UI_TITLE, 0);
    y += 74;
    snprintf(buf, sizeof buf, "%d", g->best);
    arcade_card(s, x, y, PW, 66, "HIGH SCORE", buf, FONT_UI_TITLE, g->new_best ? t->accent : 0);
    y += 74;
    int hw = (PW - 8) / 2;
    snprintf(buf, sizeof buf, "%d", g->level);
    arcade_card(s, x, y, hw, 56, "SPEED", buf, FONT_UI_LARGE, 0);
    snprintf(buf, sizeof buf, "%d", g->len);
    arcade_card(s, x + hw + 8, y, PW - hw - 8, 56, "LENGTH", buf, FONT_UI_LARGE, 0);

    g->btn[BTN_PAUSE].label = g->state == ST_PAUSED ? "Resume" : (g->state == ST_READY ? "Start" : "Pause");
    g->btn[BTN_PAUSE].primary = g->state == ST_READY || g->state == ST_PAUSED;
    g->btn[BTN_PAUSE].disabled = g->state == ST_OVER;
    g->btn[BTN_NEW].primary = g->state == ST_OVER;
    for (int i = 0; i < NBTN; i++) arcade_btn_draw(s, &g->btn[i], i, g->hover, g->press);

    int hy = g->btn[BTN_NEW].r.y + g->btn[BTN_NEW].r.h + 14;
    uint32_t m = color_mix(t->window_text, t->window_bg, 110);
    gfx_text(s, x, hy, "Arrows / WASD", t->window_text, FONT_UI_BOLD);
    gfx_text(s, x + 103, hy, "steer", m, FONT_UI);
    gfx_text(s, x, hy + 18, "P / Space", t->window_text, FONT_UI_BOLD);
    gfx_text(s, x + 103, hy + 18, "pause", m, FONT_UI);
    gfx_text(s, x, hy + 36, "F2", t->window_text, FONT_UI_BOLD);
    gfx_text(s, x + 103, hy + 36, "new game", m, FONT_UI);
}

/* ---- events --------------------------------------------------------------------------- */
static void press_button(snake_t *g, int b) {
    if (b == BTN_PAUSE) {
        if (g->state == ST_READY) start(g);
        else if (g->state == ST_RUNNING) set_paused(g, true);
        else if (g->state == ST_PAUSED) set_paused(g, false);
        else { new_game(g); start(g); }
    } else if (b == BTN_NEW) {
        new_game(g);
    }
}

static void tick(snake_t *g) {
    uint32_t now = sys_uptime_ms();
    uint32_t dt = now - g->last_ms;
    g->last_ms = now;
    if (dt > 100) dt = 100;
    if (g->state != ST_RUNNING) return;
    g->clock += dt;
    for (int i = 0; i < NPOP; i++)
        if (g->pops[i].value && g->pops[i].age < 800) g->pops[i].age += dt;
    if (g->bonus_on) {
        if (g->bonus_left <= dt) g->bonus_on = false;
        else g->bonus_left -= dt;
    }
    g->acc += dt;
    while (g->acc >= g->interval && g->state == ST_RUNNING) {
        g->acc -= g->interval;
        step(g);
    }
    invalidate_board(g);
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

static void on_key(snake_t *g, const event_t *ev) {
    if (ev->mods & (MOD_CTRL | MOD_ALT)) return;
    int d = key_dir(ev->key);
    if (d >= 0) {
        g->autopilot = false;
        if (g->state == ST_READY) {
            if (d != (g->dir + 2) % 4) g->dir = d;
            start(g);
        } else if (g->state == ST_PAUSED) {
            set_paused(g, false);
            steer(g, d);
        } else if (g->state == ST_RUNNING) {
            steer(g, d);
        }
        return;
    }
    switch (ev->key) {
    case 'p': case KEY_SPACE: case KEY_ESCAPE: case KEY_PAUSE:
        if (ev->repeat) break;
        if (g->state == ST_RUNNING) set_paused(g, true);
        else if (g->state == ST_PAUSED) set_paused(g, false);
        else if (g->state == ST_READY && ev->key == KEY_SPACE) start(g);
        else if (g->state == ST_OVER && ev->key == KEY_SPACE) { new_game(g); start(g); }
        break;
    case KEY_ENTER:
        if (ev->repeat) break;
        if (g->state == ST_OVER) { new_game(g); start(g); }
        else if (g->state == ST_READY) start(g);
        else if (g->state == ST_PAUSED) set_paused(g, false);
        break;
    case KEY_F2: case 'n':
        new_game(g);
        break;
    }
}

static void snake_event(window_t *win, const event_t *ev) {
    snake_t *g = win_get_userdata(win);
    if (!g) return;
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
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
    case EV_BLUR:
        set_paused(g, true);
        break;
    case EV_MOUSE_MOVE: {
        int h = arcade_btn_hit(g->btn, NBTN, ev->x, ev->y);
        if (h != g->hover) { g->hover = h; invalidate_panel(g); }
        break;
    }
    case EV_MOUSE_LEAVE:
        if (g->hover != -1) { g->hover = -1; invalidate_panel(g); }
        break;
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_LEFT) {
            g->press = arcade_btn_hit(g->btn, NBTN, ev->x, ev->y);
            if (g->press >= 0) invalidate_panel(g);
        }
        break;
    case EV_MOUSE_UP:
        if (ev->button == MOUSE_LEFT && g->press >= 0) {
            int b = g->press;
            g->press = -1;
            invalidate_panel(g);
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

static void snake_launch(const char *arg) {
    snake_t *g = calloc(1, sizeof *g);
    if (!g) return;
    const app_desc_t *app = app_find("Snake");
    uint32_t flags = (app && app_window_count(app) > 0) ? 0 : WIN_CENTER;
    window_t *win = win_create("Snake", CW, CH, flags);
    if (!win) { free(g); return; }
    g->win = win;
    arcade_rng_seed(&g->rng, (uint32_t)(uintptr_t)g);
    g->best = arcade_hiscore_get("snake");
    g->hover = g->press = -1;
    int by = BY + 46 + 74 + 74 + 56 + 14;
    g->btn[BTN_PAUSE] = (arcade_btn_t){ rect_make(PX, by, PW, 32), "Start", true, true };
    g->btn[BTN_NEW] = (arcade_btn_t){ rect_make(PX, by + 40, PW, 32), "New game", false, true };
    win_set_userdata(win, g);
    win_set_event_handler(win, snake_event);
    new_game(g);
    g->last_ms = sys_uptime_ms();
    win_set_timer(win, 16);
    if (arg && !strcmp(arg, "demo")) {
        g->autopilot = true;
        start(g);
    }
}

/* Icon: green rounded tile, an S-shaped blue snake heading for a red apple. */
static void snake_icon(surface_t *s, int x, int y, int size) {
    double S = size;
    int m = size / 16;
    gfx_fill_rounded_gradient_v(s, x + m, y + m, size - 2 * m, size - 2 * m, size / 5,
                                RGB(140, 214, 86), RGB(74, 158, 56));
    /* faint checker hint */
    int q = (size - 2 * m) / 4;
    for (int j = 0; j < 4; j++)
        for (int i = (j & 1); i < 4; i += 2)
            if (!((i == 0 || i == 3) && (j == 0 || j == 3)))
                gfx_fill_rect(s, x + m + i * q, y + m + j * q, q, q, RGBA(255, 255, 255, 22));
    double w = S * 0.14;
    uint32_t body = RGB(52, 104, 228), dark = RGB(36, 74, 170);
    double pts[6][2] = { { 0.26, 0.74 }, { 0.70, 0.74 }, { 0.70, 0.52 }, { 0.30, 0.52 }, { 0.30, 0.30 }, { 0.54, 0.30 } };
    for (int i = 0; i < 5; i++) {
        uint32_t c = color_mix(dark, body, i * 255 / 4);
        arcade_capsule(s, x + pts[i][0] * S, y + pts[i][1] * S, x + pts[i + 1][0] * S, y + pts[i + 1][1] * S, w, c);
    }
    double hx = x + 0.56 * S, hy = y + 0.30 * S;
    gfx_fill_circle_aa(s, hx, hy, S * 0.095, body);
    gfx_fill_circle_aa(s, hx + S * 0.02, hy - S * 0.035, S * 0.032, COLOR_WHITE);
    gfx_fill_circle_aa(s, hx + S * 0.028, hy - S * 0.035, S * 0.016, RGB(20, 24, 36));
    /* apple */
    double ax = x + 0.78 * S, ay = y + 0.30 * S, ar = S * 0.085;
    gfx_fill_circle_aa(s, ax, ay, ar, RGB(226, 44, 44));
    gfx_fill_circle_aa(s, ax - ar * 0.35, ay - ar * 0.35, ar * 0.32, RGBA(255, 255, 255, 150));
    gfx_fill_ellipse(s, (int)(ax + ar * 0.4), (int)(ay - ar * 1.1), (int)(ar * 0.5 + 1), (int)(ar * 0.25 + 1), RGB(40, 120, 40));
}

REGISTER_APP(.name = "Snake", .category = CAT_GAMES,
             .description = "Guide the snake to the apples - don't bite your tail",
             .launch = snake_launch, .draw_icon = snake_icon);
