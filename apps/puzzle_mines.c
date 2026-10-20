/*
 * puzzle_mines.c - Minesweeper (Games).
 *
 *   - Beginner (9x9, 10 mines), Intermediate (16x16, 40), Expert (30x16, 99),
 *     chosen with the segmented control at the top or the 1/2/3 keys;
 *   - the first click is always safe: mines are placed after it, never in the
 *     clicked cell or its eight neighbours;
 *   - empty areas open with an iterative flood fill (explicit stack, no
 *     recursion on the shared kernel stack);
 *   - right click toggles a flag; chording (click a satisfied number, middle
 *     click, or press both buttons) opens the remaining neighbours;
 *   - LED mine counter and timer, smiley reset button with moods, best time
 *     per level (stored in ~/.puzzle_scores);
 *   - keyboard play: arrows move a cursor, Space/Enter reveals, F flags,
 *     F2/N starts a new game.
 *
 * All state lives in a per-window struct, so several games can run at once.
 */
#include "puzzle_common.h"

#define MS_MARGIN   14
#define MS_SEG_Y    12
#define MS_SEG_W    330
#define MS_SEG_H    30
#define MS_PANEL_Y  52
#define MS_PANEL_H  58
#define MS_GRID_Y   124
#define MS_STATUS_H 30
#define MS_MIN_W    380

/* cell bits: low nibble = number of adjacent mines */
#define C_ADJ  0x0F
#define C_MINE 0x10
#define C_OPEN 0x20
#define C_FLAG 0x40

enum { ST_READY, ST_PLAYING, ST_WON, ST_LOST };
enum { MOOD_SMILE, MOOD_OH, MOOD_DEAD, MOOD_COOL };

typedef struct {
    const char *key;
    int cols, rows, mines, cell;
} ms_level_t;

static const ms_level_t levels[3] = {
    { "mines_beginner", 9, 9, 10, 32 },
    { "mines_intermediate", 16, 16, 40, 28 },
    { "mines_expert", 30, 16, 99, 24 },
};
static const char *const level_names[3] = { "Beginner", "Intermediate", "Expert" };

typedef struct {
    window_t *win;
    pz_rng_t rng;
    int level, cols, rows, mines, cell;
    uint8_t *c;            /* cols * rows cells */
    int *stack;            /* flood-fill work stack, cols * rows entries */
    int state;
    bool placed;           /* mines placed (after the first click) */
    int opened, flags;
    uint32_t start_ms;
    int elapsed;           /* seconds */
    int timer;
    int boom;              /* exploded cell or -1 */
    /* mouse */
    int hover, press;      /* cell under the pointer / pressed cell, -1 = none */
    bool left_down, right_down, chording, ignore_left_up;
    bool grid_press;       /* left button went down on the grid */
    bool face_hover, face_down;
    int seg_hover;
    /* keyboard */
    int kb;
    bool kb_on;
    /* layout */
    int W, H;
    rect_t seg_r, panel_r, face_r, mines_r, time_r, grid_r, status_r;
    int best;              /* best time for the level in seconds, 0 = none */
    bool new_best;
    int final_time;
} ms_t;

/* ---- helpers ----------------------------------------------------------------------- */
static int ms_total(ms_t *m) { return m->cols * m->rows; }

static int ms_neighbours(ms_t *m, int idx, int *out) {
    int x = idx % m->cols, y = idx / m->cols, n = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= m->cols || ny >= m->rows) continue;
            out[n++] = ny * m->cols + nx;
        }
    return n;
}

static rect_t ms_cell_rect(ms_t *m, int idx) {
    return rect_make(m->grid_r.x + (idx % m->cols) * m->cell, m->grid_r.y + (idx / m->cols) * m->cell,
                     m->cell, m->cell);
}

static void ms_inval_cell(ms_t *m, int idx) {
    if (idx < 0) return;
    rect_t r = ms_cell_rect(m, idx);
    win_invalidate_rect(m->win, r.x - 2, r.y - 2, r.w + 4, r.h + 4);
}

static void ms_inval_grid(ms_t *m) {
    win_invalidate_rect(m->win, m->grid_r.x - 6, m->grid_r.y - 6, m->grid_r.w + 12, m->grid_r.h + 12);
}

static void ms_inval_panel(ms_t *m) {
    win_invalidate_rect(m->win, m->panel_r.x, m->panel_r.y, m->panel_r.w, m->panel_r.h);
}

static void ms_inval_status(ms_t *m) {
    win_invalidate_rect(m->win, m->status_r.x, m->status_r.y, m->status_r.w, m->status_r.h);
}

static int ms_cell_at(ms_t *m, int x, int y) {
    if (!rect_contains(m->grid_r, x, y)) return -1;
    int cx = (x - m->grid_r.x) / m->cell, cy = (y - m->grid_r.y) / m->cell;
    if (cx < 0 || cy < 0 || cx >= m->cols || cy >= m->rows) return -1;
    return cy * m->cols + cx;
}

static void ms_layout(ms_t *m) {
    int gw = m->cols * m->cell, gh = m->rows * m->cell;
    m->W = MAX(gw + 2 * MS_MARGIN + 8, MS_MIN_W);
    m->H = MS_GRID_Y + gh + 4 + 14 + MS_STATUS_H;
    m->seg_r = rect_make((m->W - MS_SEG_W) / 2, MS_SEG_Y, MS_SEG_W, MS_SEG_H);
    m->panel_r = rect_make(MS_MARGIN, MS_PANEL_Y, m->W - 2 * MS_MARGIN, MS_PANEL_H);
    m->face_r = rect_make((m->W - 46) / 2, MS_PANEL_Y + 6, 46, 46);
    m->mines_r = rect_make(m->panel_r.x + 10, MS_PANEL_Y + 10, 68, 38);
    m->time_r = rect_make(m->panel_r.x + m->panel_r.w - 10 - 68, MS_PANEL_Y + 10, 68, 38);
    m->grid_r = rect_make((m->W - gw) / 2, MS_GRID_Y + 4, gw, gh);
    m->status_r = rect_make(0, m->H - MS_STATUS_H, m->W, MS_STATUS_H);
}

static void ms_stop_timer(ms_t *m) {
    if (m->timer > 0) win_kill_timer(m->win, m->timer);
    m->timer = 0;
}

static void ms_new_game(ms_t *m) {
    ms_stop_timer(m);
    memset(m->c, 0, (size_t)ms_total(m));
    m->state = ST_READY;
    m->placed = false;
    m->opened = m->flags = 0;
    m->elapsed = 0;
    m->boom = -1;
    m->press = -1;
    m->chording = m->grid_press = false;
    m->new_best = false;
    m->final_time = 0;
    m->best = pz_score_get(levels[m->level].key, 0);
    win_invalidate(m->win);
}

static bool ms_set_level(ms_t *m, int level) {
    level = CLAMP(level, 0, 2);
    const ms_level_t *L = &levels[level];
    int n = L->cols * L->rows;
    uint8_t *c = malloc((size_t)n);
    int *st = malloc(sizeof(int) * (size_t)n);
    if (!c || !st) { free(c); free(st); return false; }
    free(m->c);
    free(m->stack);
    m->c = c;
    m->stack = st;
    m->level = level;
    m->cols = L->cols;
    m->rows = L->rows;
    m->mines = L->mines;
    m->cell = L->cell;
    m->hover = -1;
    m->kb = (m->rows / 2) * m->cols + m->cols / 2;
    ms_layout(m);
    if (win_width(m->win) != m->W || win_height(m->win) != m->H) {
        win_resize(m->win, m->W, m->H);
        /* a bigger board must not hang off the screen */
        rect_t f = win_frame_rect(m->win);
        int x = f.x, y = f.y;
        if (x + f.w > screen_width()) x = screen_width() - f.w;
        if (y + f.h > screen_work_height()) y = screen_work_height() - f.h;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x != f.x || y != f.y) win_move(m->win, x, y);
    }
    ms_new_game(m);
    pz_score_set("mines_level", level);
    return true;
}

/* Place mines anywhere except the first-clicked cell and its neighbours. */
static void ms_place_mines(ms_t *m, int safe) {
    int total = ms_total(m);
    int nb[8], nn = ms_neighbours(m, safe, nb);
    bool keep_area = total - (nn + 1) >= m->mines;
    int placed = 0;
    while (placed < m->mines) {
        int i = pz_rand_n(&m->rng, total);
        if (i == safe || (m->c[i] & C_MINE)) continue;
        if (keep_area) {
            bool near = false;
            for (int k = 0; k < nn; k++) if (nb[k] == i) { near = true; break; }
            if (near) continue;
        }
        m->c[i] |= C_MINE;
        placed++;
    }
    for (int i = 0; i < total; i++) {
        int cnt = 0;
        nn = ms_neighbours(m, i, nb);
        for (int k = 0; k < nn; k++) if (m->c[nb[k]] & C_MINE) cnt++;
        m->c[i] = (uint8_t)((m->c[i] & ~C_ADJ) | cnt);
    }
    m->placed = true;
    m->state = ST_PLAYING;
    m->start_ms = sys_uptime_ms();
    m->elapsed = 0;
    if (m->timer <= 0) m->timer = win_set_timer(m->win, 250);
}

static void ms_lose(ms_t *m, int idx) {
    m->state = ST_LOST;
    m->boom = idx;
    m->final_time = (int)((sys_uptime_ms() - m->start_ms) / 1000);
    ms_stop_timer(m);
    static const note_t boom[] = { { 180, 70 }, { 120, 110 }, { 80, 220 } };
    sys_play_notes(boom, ARRAY_SIZE(boom));
    win_invalidate(m->win);
}

static void ms_check_win(ms_t *m) {
    if (m->state != ST_PLAYING || m->opened != ms_total(m) - m->mines) return;
    m->state = ST_WON;
    uint32_t ms = sys_uptime_ms() - m->start_ms;
    m->final_time = (int)((ms + 999) / 1000);
    if (m->final_time < 1) m->final_time = 1;
    m->elapsed = MIN(m->final_time, 999);
    ms_stop_timer(m);
    for (int i = 0; i < ms_total(m); i++)
        if (m->c[i] & C_MINE) m->c[i] |= C_FLAG;
    m->flags = m->mines;
    if (m->best <= 0 || m->final_time < m->best) {
        m->new_best = true;
        m->best = m->final_time;
        pz_score_set(levels[m->level].key, m->best);
    }
    static const note_t tune[] = { { 523, 90 }, { 659, 90 }, { 784, 90 }, { 1047, 220 } };
    sys_play_notes(tune, ARRAY_SIZE(tune));
    win_invalidate(m->win);
}

/* Open one cell; empty cells flood outwards iteratively. */
static void ms_open(ms_t *m, int idx) {
    uint8_t *c = m->c;
    if (c[idx] & (C_OPEN | C_FLAG)) return;
    if (c[idx] & C_MINE) { ms_lose(m, idx); return; }
    int sp = 0, nb[8];
    c[idx] |= C_OPEN;
    m->stack[sp++] = idx;
    while (sp > 0) {
        int i = m->stack[--sp];
        m->opened++;
        if ((c[i] & C_ADJ) != 0) continue;
        int nn = ms_neighbours(m, i, nb);
        for (int k = 0; k < nn; k++) {
            int j = nb[k];
            if (c[j] & (C_OPEN | C_FLAG | C_MINE)) continue;
            c[j] |= C_OPEN;
            m->stack[sp++] = j;
        }
    }
}

static void ms_chord(ms_t *m, int idx) {
    if (m->state != ST_PLAYING) return;
    uint8_t v = m->c[idx];
    if (!(v & C_OPEN) || !(v & C_ADJ)) return;
    int nb[8], nn = ms_neighbours(m, idx, nb), fl = 0, closed = 0;
    for (int k = 0; k < nn; k++) {
        if (m->c[nb[k]] & C_FLAG) fl++;
        else if (!(m->c[nb[k]] & C_OPEN)) closed++;
    }
    if (fl != (v & C_ADJ) || !closed) return;
    for (int k = 0; k < nn && m->state == ST_PLAYING; k++) ms_open(m, nb[k]);
    if (m->state == ST_PLAYING) sys_beep(1100, 10);
    ms_check_win(m);
    ms_inval_grid(m);
    ms_inval_panel(m);
    ms_inval_status(m);
}

static void ms_click(ms_t *m, int idx) {
    if (m->state == ST_WON || m->state == ST_LOST) return;
    uint8_t v = m->c[idx];
    if (v & C_FLAG) return;
    if (v & C_OPEN) { ms_chord(m, idx); return; }
    if (!m->placed) ms_place_mines(m, idx);
    int before = m->opened;
    ms_open(m, idx);
    if (m->state == ST_PLAYING && m->opened - before > 1) sys_beep(1200, 8);
    ms_check_win(m);
    ms_inval_grid(m);
    ms_inval_panel(m);
    ms_inval_status(m);
}

static void ms_toggle_flag(ms_t *m, int idx) {
    if (m->state == ST_WON || m->state == ST_LOST) return;
    uint8_t *v = &m->c[idx];
    if (*v & C_OPEN) return;
    if (*v & C_FLAG) { *v &= ~C_FLAG; m->flags--; }
    else { *v |= C_FLAG; m->flags++; }
    sys_beep(*v & C_FLAG ? 880 : 660, 12);
    ms_inval_cell(m, idx);
    ms_inval_panel(m);
}

/* ---- drawing ---------------------------------------------------------------------- */
static void seg_h(surface_t *s, int x0, int x1, int y, int t, uint32_t c) {
    int h = t / 2;
    point_t p[6] = { { x0, y }, { x0 + h, y - h }, { x1 - h, y - h }, { x1, y }, { x1 - h, y + h }, { x0 + h, y + h } };
    gfx_fill_polygon(s, p, 6, c);
}

static void seg_v(surface_t *s, int x, int y0, int y1, int t, uint32_t c) {
    int h = t / 2;
    point_t p[6] = { { x, y0 }, { x + h, y0 + h }, { x + h, y1 - h }, { x, y1 }, { x - h, y1 - h }, { x - h, y0 + h } };
    gfx_fill_polygon(s, p, 6, c);
}

/* Seven-segment digit; mask bits a..g = 0..6. */
static void seg_digit(surface_t *s, int x, int y, int w, int h, int mask, uint32_t on, uint32_t off) {
    int t = 4, hm = y + h / 2;
    int xl = x + t / 2, xr = x + w - t / 2;
    uint32_t col[7];
    for (int i = 0; i < 7; i++) col[i] = (mask & (1 << i)) ? on : off;
    seg_h(s, x + 1, x + w - 1, y + t / 2, t, col[0]);          /* a */
    seg_v(s, xr, y + t / 2 + 1, hm - 1, t, col[1]);            /* b */
    seg_v(s, xr, hm + 1, y + h - t / 2 - 1, t, col[2]);        /* c */
    seg_h(s, x + 1, x + w - 1, y + h - t / 2, t, col[3]);      /* d */
    seg_v(s, xl, hm + 1, y + h - t / 2 - 1, t, col[4]);        /* e */
    seg_v(s, xl, y + t / 2 + 1, hm - 1, t, col[5]);            /* f */
    seg_h(s, x + 1, x + w - 1, hm, t, col[6]);                 /* g */
}

static void draw_counter(surface_t *s, rect_t r, int value) {
    static const uint8_t digits[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 7, RGB(20, 21, 26));
    gfx_rounded_rect(s, r.x, r.y, r.w, r.h, 7, RGB(58, 60, 70));
    uint32_t on = RGB(255, 76, 64), off = RGB(62, 28, 30);
    int v = CLAMP(value, -99, 999);
    int masks[3];
    if (v < 0) {
        int a = -v;
        masks[0] = 0x40;
        masks[1] = digits[(a / 10) % 10];
        masks[2] = digits[a % 10];
    } else {
        masks[0] = digits[(v / 100) % 10];
        masks[1] = digits[(v / 10) % 10];
        masks[2] = digits[v % 10];
    }
    int dw = 18, dh = 30, gap = 3;
    int x = r.x + (r.w - (3 * dw + 2 * gap)) / 2, y = r.y + (r.h - dh) / 2;
    for (int i = 0; i < 3; i++) seg_digit(s, x + i * (dw + gap), y, dw, dh, masks[i], on, off);
}

static void draw_mine(surface_t *s, double cx, double cy, double r) {
    uint32_t body = RGB(34, 36, 44);
    for (int i = 0; i < 4; i++) {
        double a = i * M_PI / 4, l = r * 1.5;
        pz_stroke(s, cx - cos(a) * l, cy - sin(a) * l, cx + cos(a) * l, cy + sin(a) * l, MAX(1.6, r * 0.3), body);
    }
    gfx_fill_circle_aa(s, cx, cy, r, body);
    gfx_fill_circle_aa(s, cx - r * 0.34, cy - r * 0.34, r * 0.3, RGBA(255, 255, 255, 210));
}

static void draw_flag(surface_t *s, double x, double y, double c, bool dark) {
    uint32_t ink = dark ? RGB(226, 229, 238) : RGB(44, 46, 56), red = RGB(236, 58, 52);
    double px = x + c * 0.6, top = y + c * 0.17, bot = y + c * 0.74;
    /* base: a low plinth with a narrower step on top */
    gfx_fill_rounded_rect(s, (int)lround(x + c * 0.26), (int)lround(bot), (int)lround(c * 0.5), (int)MAX(3, lround(c * 0.1)),
                          2, ink);
    gfx_fill_rect(s, (int)lround(x + c * 0.38), (int)lround(bot - c * 0.06), (int)lround(c * 0.3), (int)MAX(2, lround(c * 0.07)), ink);
    pz_stroke(s, px, top, px, bot, MAX(1.8, c * 0.085), ink);
    double fx = px - c * 0.03;
    point_t p[3] = { { (int)lround(fx), (int)lround(top - c * 0.02) }, { (int)lround(fx - c * 0.42), (int)lround(top + c * 0.17) },
                     { (int)lround(fx), (int)lround(top + c * 0.36) } };
    gfx_fill_polygon(s, p, 3, red);
    gfx_line_aa(s, fx, top - c * 0.02, fx - c * 0.42, top + c * 0.17, red);
    gfx_line_aa(s, fx - c * 0.42, top + c * 0.17, fx, top + c * 0.36, red);
}

static uint32_t number_color(int n, bool dark) {
    static const uint32_t light[9] = { 0, RGB(30, 110, 220), RGB(46, 140, 60), RGB(214, 48, 48), RGB(60, 60, 170),
                                       RGB(150, 40, 40), RGB(0, 130, 140), RGB(50, 50, 56), RGB(120, 120, 128) };
    static const uint32_t darkc[9] = { 0, RGB(110, 170, 255), RGB(110, 205, 120), RGB(255, 112, 104), RGB(170, 150, 255),
                                       RGB(255, 160, 100), RGB(80, 210, 215), RGB(230, 230, 236), RGB(170, 170, 178) };
    n = CLAMP(n, 0, 8);
    return dark ? darkc[n] : light[n];
}

static void draw_face(surface_t *s, rect_t r, int mood, bool pressed, bool hover) {
    ui_draw_button(s, r, NULL, pressed ? UI_STATE_PRESSED : (hover ? UI_STATE_HOVER : 0));
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0 + (pressed ? 1 : 0), R = 15;
    gfx_fill_circle_aa(s, cx, cy + 1, R + 1, RGBA(0, 0, 0, 40));
    gfx_fill_circle_aa(s, cx, cy, R + 1, RGB(206, 146, 22));
    gfx_fill_circle_aa(s, cx, cy, R, RGB(255, 206, 64));
    gfx_fill_circle_aa(s, cx - 4, cy - 6, 6, RGBA(255, 255, 255, 60));
    uint32_t ink = RGB(70, 44, 12);
    switch (mood) {
    case MOOD_OH:
        gfx_fill_circle_aa(s, cx - 5, cy - 4, 2, ink);
        gfx_fill_circle_aa(s, cx + 5, cy - 4, 2, ink);
        gfx_circle_aa(s, cx, cy + 5, 3, 1.8, ink);
        break;
    case MOOD_DEAD:
        for (int e = -1; e <= 1; e += 2) {
            double ex = cx + e * 5, ey = cy - 4;
            pz_stroke(s, ex - 2.2, ey - 2.2, ex + 2.2, ey + 2.2, 1.6, ink);
            pz_stroke(s, ex - 2.2, ey + 2.2, ex + 2.2, ey - 2.2, 1.6, ink);
        }
        pz_arc(s, cx, cy + 11, 6, M_PI * 1.2, M_PI * 1.8, 1.8, ink);
        break;
    case MOOD_COOL:
        gfx_fill_rounded_rect(s, (int)cx - 11, (int)cy - 7, 9, 6, 2, RGB(24, 24, 30));
        gfx_fill_rounded_rect(s, (int)cx + 2, (int)cy - 7, 9, 6, 2, RGB(24, 24, 30));
        pz_stroke(s, cx - 3, cy - 6, cx + 3, cy - 6, 1.4, RGB(24, 24, 30));
        pz_stroke(s, cx - 11, cy - 6, cx - 14, cy - 8, 1.2, RGB(24, 24, 30));
        pz_stroke(s, cx + 11, cy - 6, cx + 14, cy - 8, 1.2, RGB(24, 24, 30));
        pz_arc(s, cx, cy + 1, 7.5, M_PI * 0.2, M_PI * 0.8, 1.8, ink);
        break;
    default:
        gfx_fill_circle_aa(s, cx - 5, cy - 4, 2, ink);
        gfx_fill_circle_aa(s, cx + 5, cy - 4, 2, ink);
        pz_arc(s, cx, cy + 1, 7.5, M_PI * 0.2, M_PI * 0.8, 1.8, ink);
        break;
    }
}

static void ms_draw_cell(ms_t *m, surface_t *s, int idx, const theme_t *t, uint32_t open_bg,
                         uint32_t tile_top, uint32_t tile_bot) {
    rect_t r = ms_cell_rect(m, idx);
    int c = m->cell;
    uint8_t v = m->c[idx];
    bool over = m->state == ST_WON || m->state == ST_LOST;
    bool show_mine = m->state == ST_LOST && (v & C_MINE) && !(v & C_FLAG);
    bool wrong_flag = m->state == ST_LOST && (v & C_FLAG) && !(v & C_MINE);
    bool pressed = false;
    if (!over && !(v & (C_OPEN | C_FLAG)) && m->press >= 0) {
        if (m->chording) {
            int px = m->press % m->cols, py = m->press / m->cols, x = idx % m->cols, y = idx / m->cols;
            pressed = abs(px - x) <= 1 && abs(py - y) <= 1;
        } else {
            pressed = idx == m->press;
        }
    }
    if ((v & C_OPEN) || show_mine || pressed) {
        uint32_t bg = (idx == m->boom) ? RGB(236, 72, 64) : open_bg;
        gfx_fill_rect(s, r.x, r.y, c - 1, c - 1, bg);
        if (show_mine) {
            draw_mine(s, r.x + (c - 1) / 2.0, r.y + (c - 1) / 2.0, c * 0.22);
        } else if ((v & C_OPEN) && (v & C_ADJ)) {
            char d[2] = { (char)('0' + (v & C_ADJ)), 0 };
            gfx_text_aligned(s, rect_make(r.x, r.y, c - 1, c - 1), d, number_color(v & C_ADJ, t->dark),
                             FONT_UI_LARGE, ALIGN_CENTER | ALIGN_MIDDLE);
        }
    } else {
        uint32_t top = tile_top, bot = tile_bot;
        if (!over && idx == m->hover && m->press < 0) { top = color_shade(top, 16); bot = color_shade(bot, 16); }
        gfx_fill_rounded_gradient_v(s, r.x + 1, r.y + 1, c - 3, c - 3, 3, top, bot);
        gfx_hline(s, r.x + 4, r.y + 2, c - 9, RGBA(255, 255, 255, t->dark ? 40 : 150));
        if (v & C_FLAG) {
            draw_flag(s, r.x, r.y, c - 1, t->dark);
            if (wrong_flag) {
                uint32_t xc = RGB(220, 30, 30);
                pz_stroke(s, r.x + c * 0.2, r.y + c * 0.2, r.x + c * 0.76, r.y + c * 0.76, 2.2, xc);
                pz_stroke(s, r.x + c * 0.76, r.y + c * 0.2, r.x + c * 0.2, r.y + c * 0.76, 2.2, xc);
            }
        }
    }
    if (m->kb_on && idx == m->kb && !over) {
        gfx_rounded_rect(s, r.x - 1, r.y - 1, c + 1, c + 1, 4, t->accent);
        gfx_rounded_rect(s, r.x, r.y, c - 1, c - 1, 3, t->accent);
    }
}

static void ms_paint(ms_t *m, surface_t *s) {
    const theme_t *t = theme_get();
    /* difficulty selector */
    pz_seg_draw(s, m->seg_r, level_names, 3, m->level, m->seg_hover);

    /* status panel: mine counter, smiley, timer */
    ui_draw_panel(s, m->panel_r);
    draw_counter(s, m->mines_r, m->mines - m->flags);
    draw_counter(s, m->time_r, MIN(m->elapsed, 999));
    int mood = MOOD_SMILE;
    if (m->state == ST_LOST) mood = MOOD_DEAD;
    else if (m->state == ST_WON) mood = MOOD_COOL;
    else if (m->press >= 0) mood = MOOD_OH;
    draw_face(s, m->face_r, mood, m->face_down && m->face_hover, m->face_hover);

    /* board */
    rect_t g = m->grid_r;
    uint32_t frame = t->dark ? RGB(24, 25, 29) : color_mix(t->window_border, t->window_text, 30);
    uint32_t line = t->dark ? RGB(56, 58, 66) : RGB(214, 218, 226);
    uint32_t open_bg = t->dark ? RGB(40, 42, 48) : RGB(252, 252, 254);
    uint32_t tile_top = t->dark ? color_mix(RGB(96, 100, 114), t->accent, 46) : color_mix(RGB(250, 251, 254), t->accent, 40);
    uint32_t tile_bot = t->dark ? color_mix(RGB(70, 74, 86), t->accent, 40) : color_mix(RGB(222, 228, 238), t->accent, 66);
    gfx_shadow(s, g.x - 4, g.y - 4, g.w + 8, g.h + 8, 6, 6, t->dark ? 90 : 40);
    gfx_fill_rounded_rect(s, g.x - 4, g.y - 4, g.w + 8, g.h + 8, 6, frame);
    gfx_fill_rect(s, g.x, g.y, g.w, g.h, line);
    rect_t clip = s->clip;
    for (int y = 0; y < m->rows; y++) {
        int py = g.y + y * m->cell;
        if (py + m->cell <= clip.y || py >= clip.y + clip.h) continue;
        for (int x = 0; x < m->cols; x++) {
            int px = g.x + x * m->cell;
            if (px + m->cell <= clip.x - 2 || px >= clip.x + clip.w + 2) continue;
            ms_draw_cell(m, s, y * m->cols + x, t, open_bg, tile_top, tile_bot);
        }
    }

    /* win banner */
    if (m->state == ST_WON) {
        int cw = MIN(260, g.w - 20), ch = 92;
        rect_t c = pz_card(s, g, cw, ch, 0);
        pz_text_c(s, c.x, c.y + 12, c.w, "You win!", t->window_text, FONT_UI_LARGE);
        char buf[64];
        snprintf(buf, sizeof buf, "Cleared in %d second%s", m->final_time, m->final_time == 1 ? "" : "s");
        pz_text_c(s, c.x, c.y + 40, c.w, buf, t->window_text, FONT_UI);
        pz_text_c(s, c.x, c.y + 62, c.w, m->new_best ? "New best time!" : "Click to play again",
                  m->new_best ? t->accent : pz_muted(), FONT_UI_BOLD);
    }

    /* status bar */
    rect_t sb = m->status_r;
    gfx_fill_rect(s, sb.x, sb.y, sb.w, sb.h, t->dark ? color_mix(t->window_bg, COLOR_BLACK, 50) : color_mix(t->window_bg, t->window_border, 50));
    gfx_hline(s, sb.x, sb.y, sb.w, t->window_border);
    const char *msg;
    uint32_t mc = pz_muted();
    if (m->state == ST_LOST) { msg = "Boom! Click the face or press F2 to try again."; mc = RGB(220, 70, 60); }
    else if (m->state == ST_WON) msg = "Board cleared. Click the board or press F2.";
    else if (m->kb_on) msg = "Arrows move, Space reveals, F flags";
    else if (m->state == ST_READY) msg = "Click any cell to start - the first click is safe";
    else msg = "Left click reveals, right click flags";
    char best[32];
    if (m->best > 0) snprintf(best, sizeof best, "Best: %d s", m->best);
    else snprintf(best, sizeof best, "Best: --");
    int bw = font_text_width(FONT_UI, best);
    gfx_text_ellipsis(s, sb.x + 12, sb.y + (sb.h - font_height(FONT_UI)) / 2 + 1, sb.w - bw - 36, msg, mc, FONT_UI);
    gfx_text(s, sb.x + sb.w - bw - 12, sb.y + (sb.h - font_height(FONT_UI)) / 2 + 1, best, pz_muted(), FONT_UI);
}

/* ---- events ----------------------------------------------------------------------- */
static void ms_set_hover(ms_t *m, int cell) {
    if (cell == m->hover) return;
    ms_inval_cell(m, m->hover);
    m->hover = cell;
    ms_inval_cell(m, cell);
}

static void ms_mouse(ms_t *m, const event_t *ev) {
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int sh = pz_seg_hit(m->seg_r, 3, ev->x, ev->y);
        if (sh != m->seg_hover) {
            m->seg_hover = sh;
            win_invalidate_rect(m->win, m->seg_r.x, m->seg_r.y, m->seg_r.w, m->seg_r.h);
        }
        bool fh = rect_contains(m->face_r, ev->x, ev->y);
        if (fh != m->face_hover) { m->face_hover = fh; ms_inval_panel(m); }
        int cell = ms_cell_at(m, ev->x, ev->y);
        if (cell >= 0 && m->kb_on) { m->kb_on = false; ms_inval_cell(m, m->kb); ms_inval_status(m); }
        ms_set_hover(m, cell);
        if (m->grid_press || m->chording) {
            if (cell != m->press) {
                m->press = cell;
                ms_inval_grid(m);
                ms_inval_panel(m);
            }
        }
        break;
    }
    case EV_MOUSE_LEAVE:
        ms_set_hover(m, -1);
        if (m->seg_hover >= 0) { m->seg_hover = -1; win_invalidate(m->win); }
        if (m->face_hover) { m->face_hover = false; ms_inval_panel(m); }
        break;
    case EV_MOUSE_DOWN: {
        if (ev->button == MOUSE_LEFT) m->left_down = true;
        if (ev->button == MOUSE_RIGHT) m->right_down = true;
        if (ev->button == MOUSE_LEFT && rect_contains(m->face_r, ev->x, ev->y)) {
            m->face_down = m->face_hover = true;
            ms_inval_panel(m);
            break;
        }
        int sg = pz_seg_hit(m->seg_r, 3, ev->x, ev->y);
        if (sg >= 0) {
            if (ev->button == MOUSE_LEFT) {
                m->left_down = false;
                if (sg != m->level) ms_set_level(m, sg);
                else ms_new_game(m);
            }
            break;
        }
        int cell = ms_cell_at(m, ev->x, ev->y);
        if (cell < 0) break;
        if (m->state == ST_WON) {
            if (ev->button == MOUSE_LEFT) { m->left_down = false; ms_new_game(m); }
            break;
        }
        if (m->state == ST_LOST) break;
        if (ev->button == MOUSE_MIDDLE || (m->left_down && m->right_down)) {
            m->chording = true;
            m->press = cell;
            ms_inval_grid(m);
            ms_inval_panel(m);
        } else if (ev->button == MOUSE_RIGHT) {
            ms_toggle_flag(m, cell);
        } else if (ev->button == MOUSE_LEFT) {
            m->press = cell;
            m->grid_press = true;
            ms_inval_cell(m, cell);
            ms_inval_panel(m);
        }
        break;
    }
    case EV_MOUSE_UP: {
        bool left = ev->button == MOUSE_LEFT;
        if (left) m->left_down = false;
        if (ev->button == MOUSE_RIGHT) m->right_down = false;
        if (left && m->face_down) {
            m->face_down = false;
            if (rect_contains(m->face_r, ev->x, ev->y)) ms_new_game(m);
            ms_inval_panel(m);
            break;
        }
        if (m->chording) {
            int cell = ms_cell_at(m, ev->x, ev->y);
            m->chording = false;
            m->grid_press = false;
            m->press = -1;
            m->ignore_left_up = m->left_down;
            if (cell >= 0) ms_chord(m, cell);
            ms_inval_grid(m);
            ms_inval_panel(m);
            break;
        }
        if (left && m->ignore_left_up) { m->ignore_left_up = false; break; }
        if (left && m->grid_press) {
            int cell = ms_cell_at(m, ev->x, ev->y);
            m->grid_press = false;
            m->press = -1;
            if (cell >= 0) ms_click(m, cell);
            ms_inval_grid(m);
            ms_inval_panel(m);
        }
        break;
    }
    default:
        break;
    }
}

/* Test hook (kernel command line "pztest" only): F12 opens every safe cell, so the
   automated tests can reach the win screen. */
static void ms_test_reveal(ms_t *m) {
    if (!sys_cmdline_get("pztest") || m->state == ST_WON || m->state == ST_LOST) return;
    if (!m->placed) ms_place_mines(m, m->kb);
    for (int i = 0; i < ms_total(m) && m->state == ST_PLAYING; i++)
        if (!(m->c[i] & C_MINE)) { m->c[i] &= ~C_FLAG; ms_open(m, i); }
    m->flags = 0;
    ms_check_win(m);
    win_invalidate(m->win);
}

static void ms_key(ms_t *m, const event_t *ev) {
    int k = ev->key;
    if (k == KEY_F12) { ms_test_reveal(m); return; }
    if (k == KEY_F2 || k == 'n') { ms_new_game(m); return; }
    if (k >= '1' && k <= '3') { ms_set_level(m, k - '1'); return; }
    int dx = 0, dy = 0;
    if (k == KEY_LEFT) dx = -1;
    else if (k == KEY_RIGHT) dx = 1;
    else if (k == KEY_UP) dy = -1;
    else if (k == KEY_DOWN) dy = 1;
    if (dx || dy) {
        ms_inval_cell(m, m->kb);
        if (m->kb_on) {
            int x = CLAMP(m->kb % m->cols + dx, 0, m->cols - 1), y = CLAMP(m->kb / m->cols + dy, 0, m->rows - 1);
            m->kb = y * m->cols + x;
        } else {
            m->kb_on = true;
            ms_inval_status(m);
        }
        ms_inval_cell(m, m->kb);
        return;
    }
    if (k == KEY_SPACE || k == KEY_ENTER) {
        if (m->state == ST_WON || m->state == ST_LOST) { if (k == KEY_ENTER) ms_new_game(m); return; }
        if (!m->kb_on) { m->kb_on = true; ms_inval_cell(m, m->kb); ms_inval_status(m); return; }
        ms_click(m, m->kb);
        return;
    }
    if (k == 'f') {
        if (!m->kb_on) { m->kb_on = true; ms_inval_cell(m, m->kb); ms_inval_status(m); return; }
        ms_toggle_flag(m, m->kb);
    }
}

static void ms_event(window_t *win, const event_t *ev) {
    ms_t *m = win_get_userdata(win);
    if (!m) return;
    switch (ev->type) {
    case EV_PAINT:
        ms_paint(m, win_surface(win));
        break;
    case EV_TIMER:
        if (m->state == ST_PLAYING) {
            int e = (int)((sys_uptime_ms() - m->start_ms) / 1000);
            if (e != m->elapsed) {
                m->elapsed = e;
                win_invalidate_rect(win, m->time_r.x, m->time_r.y, m->time_r.w, m->time_r.h);
            }
        }
        break;
    case EV_MOUSE_MOVE: case EV_MOUSE_DOWN: case EV_MOUSE_UP: case EV_MOUSE_LEAVE:
        ms_mouse(m, ev);
        break;
    case EV_KEY_DOWN:
        ms_key(m, ev);
        break;
    case EV_BLUR:
        /* a release outside the window never arrives: reset the press state */
        m->left_down = m->right_down = m->chording = m->face_down = m->grid_press = false;
        if (m->press >= 0) { m->press = -1; win_invalidate(win); }
        break;
    case EV_CLOSE:
        free(m->c);
        free(m->stack);
        free(m);
        win_set_userdata(win, NULL);
        break;
    default:
        break;
    }
}

static void ms_launch(const char *arg) {
    (void)arg;
    ms_t *m = calloc(1, sizeof *m);
    if (!m) return;
    pz_rng_seed(&m->rng, (uint32_t)(uintptr_t)m);
    m->hover = m->press = m->boom = m->seg_hover = -1;
    int level = CLAMP(pz_score_get("mines_level", 0), 0, 2);
    const ms_level_t *L = &levels[level];
    m->cols = L->cols; m->rows = L->rows; m->cell = L->cell;
    ms_layout(m);
    m->win = win_create("Minesweeper", m->W, m->H, WIN_CENTER);
    if (!m->win) { free(m); return; }
    win_set_userdata(m->win, m);
    win_set_event_handler(m->win, ms_event);
    if (!ms_set_level(m, level)) { win_close(m->win); return; }
}

/* Icon: a tile with a mine and a little red flag. */
static void ms_icon(surface_t *s, int x, int y, int size) {
    double u = size / 48.0;
    gfx_fill_rounded_gradient_v(s, x + (int)(3 * u), y + (int)(3 * u), (int)(42 * u), (int)(42 * u), (int)(9 * u),
                                RGB(120, 170, 245), RGB(52, 104, 210));
    gfx_fill_rounded_rect(s, x + (int)(6 * u), y + (int)(6 * u), (int)(36 * u), (int)(36 * u), (int)(7 * u),
                          RGB(236, 241, 250));
    /* faint grid */
    for (int i = 1; i < 3; i++) {
        gfx_fill_rect(s, x + (int)((6 + 12 * i) * u), y + (int)(6 * u), (int)MAX(1, u), (int)(36 * u), RGB(206, 214, 230));
        gfx_fill_rect(s, x + (int)(6 * u), y + (int)((6 + 12 * i) * u), (int)(36 * u), (int)MAX(1, u), RGB(206, 214, 230));
    }
    double cx = x + 22 * u, cy = y + 26 * u, r = 8.5 * u;
    uint32_t body = RGB(30, 32, 40);
    for (int i = 0; i < 4; i++) {
        double a = i * M_PI / 4, l = r * 1.55;
        pz_stroke(s, cx - cos(a) * l, cy - sin(a) * l, cx + cos(a) * l, cy + sin(a) * l, 2.6 * u, body);
    }
    gfx_fill_circle_aa(s, cx, cy, r, body);
    gfx_fill_circle_aa(s, cx - r * 0.35, cy - r * 0.35, r * 0.32, RGBA(255, 255, 255, 220));
    /* flag in the top-right corner */
    double px = x + 37 * u, top = y + 8 * u;
    pz_stroke(s, px, top, px, top + 15 * u, 1.8 * u, RGB(50, 52, 60));
    point_t p[3] = { { (int)lround(px - u), (int)lround(top) }, { (int)lround(px - 10 * u), (int)lround(top + 4 * u) },
                     { (int)lround(px - u), (int)lround(top + 8 * u) } };
    gfx_fill_polygon(s, p, 3, RGB(236, 58, 52));
}

REGISTER_APP(.name = "Minesweeper", .category = CAT_GAMES,
             .description = "Clear the minefield without setting off a mine",
             .launch = ms_launch, .draw_icon = ms_icon);
