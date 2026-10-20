/*
 * puzzle_breakout.c - Breakout (Games).
 *
 *   - paddle follows the mouse, or Left/Right (A/D) on the keyboard;
 *   - ball physics in small sub-steps (no tunnelling), the bounce angle depends
 *     on where the ball meets the paddle, the ball speeds up a little with
 *     every paddle hit and with every level;
 *   - colourful bricks, some needing two or three hits (they crack), six
 *     hand-made level layouts that repeat faster after level 6;
 *   - falling power-ups: W wide paddle, S slow ball, M multi-ball, + extra life;
 *   - lives, score, level, best score (stored in ~/.puzzle_scores);
 *   - Space / click launches, P or Esc pauses (also when the window loses focus).
 *
 * Rendering: the backdrop and the bricks live in an offscreen layer that is only
 * re-rendered where a brick changes. Every frame the timer restores the layer
 * under the sprites' old rectangles, draws the sprites at their new positions
 * and pushes just those rectangles to the screen with win_update(). EV_PAINT
 * (exposure, theme change, overlays) draws everything.
 */
#include "puzzle_common.h"

#define BO_W        640
#define BO_HUD      50
#define BO_FH       470
#define BO_H        (BO_HUD + BO_FH)
#define COLS        12
#define MAXROWS     9
#define BRICK_W     46
#define BRICK_H     20
#define BRICK_GAP   4
#define BRICK_X0    ((BO_W - (COLS * BRICK_W + (COLS - 1) * BRICK_GAP)) / 2)
#define BRICK_Y0    56
#define PADDLE_Y    (BO_FH - 34)
#define PADDLE_H    14
#define PADDLE_W    92
#define PADDLE_WIDE 144
#define BALL_R      7
#define MAX_BALLS   3
#define MAX_PARTS   160
#define MAX_CAPS    6
#define MAX_RECTS   32
#define LIVES       3
#define MAX_LIVES   5

enum { ST_SERVE, ST_PLAY, ST_PAUSED, ST_CLEAR, ST_OVER };
enum { CAP_WIDE, CAP_SLOW, CAP_MULTI, CAP_LIFE, CAP_COUNT };

typedef struct { double x, y, vx, vy; bool alive; } ball_t;          /* (vx, vy) is a unit vector */
typedef struct { float x, y, vx, vy, life, max; uint32_t col; int size; } part_t;
typedef struct { double x, y; int kind; bool alive; } cap_t;

typedef struct {
    window_t *win;
    pz_rng_t rng;
    int state;
    int level, lives, score, best;
    bool new_best;
    int bonus;
    uint8_t hits[MAXROWS][COLS];       /* remaining hits, 0 = no brick */
    uint8_t maxhits[MAXROWS][COLS];
    int rows, bricks_left;
    double px, pw, pw_target;          /* paddle centre and width */
    double speed;                      /* ball speed, px/s */
    ball_t balls[MAX_BALLS];
    part_t parts[MAX_PARTS];
    int nparts;
    cap_t caps[MAX_CAPS];
    uint32_t wide_until, slow_until;
    uint32_t last_ms;
    int timer;
    surface_t *layer;                  /* backdrop + bricks, field coordinates */
    rect_t prev[MAX_RECTS];            /* sprite rects drawn last frame (window coords) */
    int nprev;
    rect_t brick_dirty;                /* bricks changed this frame (field coords) */
    bool key_left, key_right;
    int mouse_x;
    bool mouse_moved;
    char hud_sig[96];
    uint32_t last_wall_beep;
    bool fresh;                        /* level not launched yet (serve shows "Level N") */
} bo_t;

/* Level layouts: '.' empty, '1' one hit, '2' two hits, '3' three hits. */
static const char *const level_maps[][MAXROWS + 1] = {
    { "111111111111", "111111111111", "111111111111", "111111111111", "111111111111", "111111111111", NULL },
    { "....1111....", "...111111...", "..11111111..", ".1111111111.", "111111111111", "222222222222", NULL },
    { "222222222222", "1111.11.1111", "111..11..111", "1111.11.1111", "222222222222", "1.1.1..1.1.1", NULL },
    { "..1.....1...", "...1...1....", "..2222222...", ".22.222.22..", "11111111111.", "1.1111111.1.",
      "1.1.....1.1.", "...11.11....", NULL },
    { "333333333333", "2..........2", "2.11111111.2", "2.1.3333.1.2", "2.11111111.2", "2..........2",
      "222222222222", NULL },
    { "111111111111", "............", "222222222222", "............", "333333333333", "111111111111",
      "111111111111", NULL },
};
#define NLEVELS ARRAY_SIZE(level_maps)

static const uint32_t row_colors[MAXROWS] = {
    RGB(242, 76, 92), RGB(250, 136, 60), RGB(250, 196, 58), RGB(124, 210, 82), RGB(56, 196, 170),
    RGB(64, 160, 246), RGB(118, 110, 240), RGB(196, 96, 230), RGB(240, 96, 170),
};

static const char *const cap_labels[CAP_COUNT] = { "W", "S", "M", "+" };
static const uint32_t cap_colors[CAP_COUNT] = { RGB(56, 150, 250), RGB(40, 190, 140), RGB(236, 84, 160), RGB(240, 70, 70) };

/* ---- geometry -------------------------------------------------------------------- */
static rect_t brick_rect(int r, int c) {
    return rect_make(BRICK_X0 + c * (BRICK_W + BRICK_GAP), BRICK_Y0 + r * (BRICK_H + BRICK_GAP), BRICK_W, BRICK_H);
}

static rect_t field_rect(void) { return rect_make(0, BO_HUD, BO_W, BO_FH); }

static rect_t clip_field_local(rect_t r) { return rect_intersect(r, rect_make(0, 0, BO_W, BO_FH)); }

/* ---- layer: backdrop + bricks --------------------------------------------------------- */
static uint32_t brick_color(bo_t *g, int r, int c) {
    int mh = g->maxhits[r][c];
    if (mh >= 3) return RGB(232, 182, 62);           /* gold */
    if (mh == 2) return color_mix(row_colors[r % MAXROWS], RGB(170, 178, 196), 120);
    return row_colors[r % MAXROWS];
}

static void draw_brick(bo_t *g, surface_t *s, int r, int c) {
    rect_t b = brick_rect(r, c);
    uint32_t col = brick_color(g, r, c);
    int mh = g->maxhits[r][c], h = g->hits[r][c];
    gfx_fill_rounded_rect(s, b.x, b.y + 2, b.w, b.h, 5, RGBA(0, 0, 0, 90));
    gfx_fill_rounded_gradient_v(s, b.x, b.y, b.w, b.h, 5, color_shade(col, 40), color_shade(col, -36));
    gfx_hline(s, b.x + 4, b.y + 1, b.w - 8, RGBA(255, 255, 255, 120));
    if (mh >= 2) {
        /* rivets mark the strong bricks */
        gfx_fill_circle_aa(s, b.x + 5.5, b.y + b.h / 2.0, 1.8, RGBA(255, 255, 255, 150));
        gfx_fill_circle_aa(s, b.x + b.w - 5.5, b.y + b.h / 2.0, 1.8, RGBA(255, 255, 255, 150));
    }
    if (h < mh) {
        /* cracks */
        uint32_t k = RGBA(30, 20, 30, 170);
        double cx = b.x + b.w * 0.45, cy = b.y + b.h * 0.5;
        gfx_line_aa(s, cx, b.y + 2, cx - 4, cy, k);
        gfx_line_aa(s, cx - 4, cy, cx + 3, b.y + b.h - 3, k);
        gfx_line_aa(s, cx - 4, cy, cx - 12, cy + 3, k);
        if (mh - h >= 2) {
            gfx_line_aa(s, cx + 10, b.y + 3, cx + 14, cy + 1, k);
            gfx_line_aa(s, cx + 14, cy + 1, cx + 21, b.y + b.h - 4, k);
        }
    }
}

static void render_layer_rect(bo_t *g, rect_t r) {
    surface_t *s = g->layer;
    if (!s) return;
    r = clip_field_local(r);
    if (rect_empty(r)) return;
    surface_set_clip(s, r);
    gfx_gradient_v(s, 0, 0, BO_W, BO_FH, RGB(18, 22, 46), RGB(42, 24, 66));
    for (int x = 16; x < BO_W; x += 32) gfx_vline(s, x, 0, BO_FH, RGBA(255, 255, 255, 9));
    for (int y = 16; y < BO_FH; y += 32) gfx_hline(s, 0, y, BO_W, RGBA(255, 255, 255, 9));
    for (int row = 0; row < g->rows; row++)
        for (int c = 0; c < COLS; c++) {
            if (!g->hits[row][c]) continue;
            rect_t b = brick_rect(row, c);
            if (rect_overlaps(rect_make(b.x - 2, b.y - 2, b.w + 4, b.h + 6), r)) draw_brick(g, s, row, c);
        }
    surface_reset_clip(s);
}

static void render_layer(bo_t *g) { render_layer_rect(g, rect_make(0, 0, BO_W, BO_FH)); }

/* ---- sprites --------------------------------------------------------------------------- */
static rect_t ball_box(const ball_t *b) {
    int r = BALL_R + 6;
    return rect_make((int)b->x - r, BO_HUD + (int)b->y - r, 2 * r + 1, 2 * r + 1);
}

static rect_t paddle_box(bo_t *g) {
    int x0 = (int)floor(g->px - g->pw / 2) - 4;
    return rect_make(x0, BO_HUD + PADDLE_Y - 4, (int)ceil(g->pw) + 9, PADDLE_H + 10);
}

static rect_t cap_box(const cap_t *c) {
    return rect_make((int)c->x - 19, BO_HUD + (int)c->y - 10, 39, 22);
}

static void draw_ball(surface_t *s, const ball_t *b) {
    double cx = b->x, cy = BO_HUD + b->y;
    gfx_fill_circle_aa(s, cx, cy, BALL_R + 5, RGBA(150, 200, 255, 22));
    gfx_fill_circle_aa(s, cx, cy, BALL_R + 2.5, RGBA(170, 215, 255, 50));
    gfx_fill_circle_aa(s, cx, cy, BALL_R, RGB(246, 248, 255));
    gfx_fill_circle_aa(s, cx - 2.2, cy - 2.4, 2.4, RGB(255, 255, 255));
    gfx_circle_aa(s, cx, cy, BALL_R - 0.5, 1, RGBA(120, 150, 210, 120));
}

static void draw_paddle(bo_t *g, surface_t *s) {
    const theme_t *t = theme_get();
    int x = (int)lround(g->px - g->pw / 2), y = BO_HUD + PADDLE_Y, w = (int)lround(g->pw);
    uint32_t col = t->accent;
    gfx_fill_rounded_rect(s, x, y + 3, w, PADDLE_H, PADDLE_H / 2, RGBA(0, 0, 0, 110));
    gfx_fill_rounded_gradient_v(s, x, y, w, PADDLE_H, PADDLE_H / 2, color_shade(col, 70), color_shade(col, -40));
    gfx_fill_rounded_rect(s, x + 6, y + 2, w - 12, 3, 1, RGBA(255, 255, 255, 110));
    /* end caps */
    gfx_fill_circle_aa(s, x + PADDLE_H / 2.0, y + PADDLE_H / 2.0, 3, RGBA(255, 255, 255, 150));
    gfx_fill_circle_aa(s, x + w - PADDLE_H / 2.0, y + PADDLE_H / 2.0, 3, RGBA(255, 255, 255, 150));
}

static void draw_cap(surface_t *s, const cap_t *c) {
    int x = (int)c->x - 17, y = BO_HUD + (int)c->y - 8;
    uint32_t col = cap_colors[c->kind];
    gfx_fill_rounded_rect(s, x, y + 2, 34, 16, 8, RGBA(0, 0, 0, 100));
    gfx_fill_rounded_gradient_v(s, x, y, 34, 16, 8, color_shade(col, 60), color_shade(col, -30));
    gfx_rounded_rect(s, x, y, 34, 16, 8, RGBA(255, 255, 255, 140));
    gfx_text_aligned(s, rect_make(x, y, 34, 16), cap_labels[c->kind], COLOR_WHITE, FONT_UI_BOLD,
                     ALIGN_CENTER | ALIGN_MIDDLE);
}

static void draw_parts(bo_t *g, surface_t *s) {
    for (int i = 0; i < g->nparts; i++) {
        part_t *p = &g->parts[i];
        int a = (int)(255 * p->life / p->max);
        gfx_fill_rect(s, (int)p->x, BO_HUD + (int)p->y, p->size, p->size, color_alpha(p->col, CLAMP(a, 0, 255)));
    }
}

static rect_t parts_box(bo_t *g) {
    rect_t u = rect_make(0, 0, 0, 0);
    for (int i = 0; i < g->nparts; i++) {
        part_t *p = &g->parts[i];
        u = rect_union(u, rect_make((int)p->x - 1, BO_HUD + (int)p->y - 1, p->size + 2, p->size + 2));
    }
    return u;
}

/* Collect the current sprite rectangles (window coordinates). */
static int sprite_rects(bo_t *g, rect_t *out) {
    int n = 0;
    out[n++] = paddle_box(g);
    for (int i = 0; i < MAX_BALLS; i++)
        if (g->balls[i].alive) out[n++] = ball_box(&g->balls[i]);
    for (int i = 0; i < MAX_CAPS; i++)
        if (g->caps[i].alive) out[n++] = cap_box(&g->caps[i]);
    if (g->nparts) out[n++] = parts_box(g);
    for (int i = 0; i < n; i++) out[i] = rect_intersect(out[i], field_rect());
    return n;
}

static void draw_sprites(bo_t *g, surface_t *s) {
    draw_parts(g, s);
    for (int i = 0; i < MAX_CAPS; i++)
        if (g->caps[i].alive) draw_cap(s, &g->caps[i]);
    draw_paddle(g, s);
    for (int i = 0; i < MAX_BALLS; i++)
        if (g->balls[i].alive) draw_ball(s, &g->balls[i]);
}

/* ---- HUD -------------------------------------------------------------------------------- */
static void hud_signature(bo_t *g, char *buf, int n) {
    uint32_t now = sys_uptime_ms();
    int w = g->wide_until > now ? (int)((g->wide_until - now) / 250) + 1 : 0;
    int sl = g->slow_until > now ? (int)((g->slow_until - now) / 250) + 1 : 0;
    snprintf(buf, (size_t)n, "%d/%d/%d/%d/%d/%d/%d", g->score, g->best, g->level, g->lives, w, sl, theme_get()->dark);
}

static void draw_pill_timer(surface_t *s, int x, int y, const char *label, uint32_t col, double frac) {
    gfx_fill_rounded_rect(s, x, y, 58, 20, 10, color_alpha(col, 50));
    gfx_fill_rounded_rect(s, x, y, (int)(58 * CLAMP(frac, 0.0, 1.0)), 20, 10, color_alpha(col, 140));
    gfx_text_aligned(s, rect_make(x, y, 58, 20), label, COLOR_WHITE, FONT_UI_BOLD, ALIGN_CENTER | ALIGN_MIDDLE);
}

static void draw_hud(bo_t *g, surface_t *s) {
    const theme_t *t = theme_get();
    uint32_t bg = t->dark ? color_mix(t->window_bg, COLOR_BLACK, 40) : t->panel_bg;
    gfx_fill_rect(s, 0, 0, BO_W, BO_HUD, bg);
    gfx_hline(s, 0, BO_HUD - 1, BO_W, t->window_border);
    uint32_t mut = pz_muted();
    char buf[32];
    /* score + best */
    gfx_text(s, 18, 7, "SCORE", mut, FONT_UI_BOLD);
    snprintf(buf, sizeof buf, "%d", g->score);
    gfx_text(s, 18, 22, buf, t->window_text, FONT_UI_LARGE);
    gfx_text(s, 128, 7, "BEST", mut, FONT_UI_BOLD);
    snprintf(buf, sizeof buf, "%d", MAX(g->best, g->score));
    gfx_text(s, 128, 22, buf, g->score > g->best && g->best > 0 ? t->accent : t->window_text, FONT_UI_LARGE);
    /* level */
    snprintf(buf, sizeof buf, "LEVEL %d", g->level);
    gfx_text_aligned(s, rect_make(0, 0, BO_W, BO_HUD), buf, t->window_text, FONT_UI_LARGE, ALIGN_CENTER | ALIGN_MIDDLE);
    /* power-up timers */
    uint32_t now = sys_uptime_ms();
    int px = BO_W / 2 + 70;
    if (g->wide_until > now) {
        draw_pill_timer(s, px, 15, "WIDE", cap_colors[CAP_WIDE], (g->wide_until - now) / 15000.0);
        px += 64;
    }
    if (g->slow_until > now) draw_pill_timer(s, px, 15, "SLOW", cap_colors[CAP_SLOW], (g->slow_until - now) / 12000.0);
    /* lives */
    gfx_text(s, BO_W - 18 - font_text_width(FONT_UI_BOLD, "LIVES"), 7, "LIVES", mut, FONT_UI_BOLD);
    for (int i = 0; i < MAX_LIVES; i++) {
        double cx = BO_W - 24 - i * 17, cy = 33;
        if (i < g->lives) {
            gfx_fill_circle_aa(s, cx, cy, 6, t->accent);
            gfx_fill_circle_aa(s, cx - 1.8, cy - 2, 2, RGBA(255, 255, 255, 170));
        } else if (i < LIVES) {
            gfx_circle_aa(s, cx, cy, 5.5, 1.2, color_alpha(t->window_text, 60));
        }
    }
}

/* ---- overlays (drawn in EV_PAINT only) ---------------------------------------------------- */
static void draw_overlay(bo_t *g, surface_t *s) {
    const theme_t *t = theme_get();
    rect_t f = field_rect();
    char buf[64];
    if (g->state == ST_SERVE) {
        int y = BO_HUD + PADDLE_Y - 150;
        if (g->fresh) snprintf(buf, sizeof buf, "Level %d", g->level);
        else snprintf(buf, sizeof buf, "Get ready - %d %s left", g->lives, g->lives == 1 ? "life" : "lives");
        gfx_text_aligned(s, rect_make(0, y, BO_W, 40), buf, RGB(240, 242, 255), FONT_UI_TITLE, ALIGN_CENTER | ALIGN_MIDDLE);
        gfx_text_aligned(s, rect_make(0, y + 44, BO_W, 20), "Click or press Space to launch",
                         RGBA(220, 226, 255, 200), FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
        gfx_text_aligned(s, rect_make(0, y + 64, BO_W, 20), "Mouse or Left/Right moves the paddle  -  P pauses",
                         RGBA(200, 206, 240, 140), FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
        return;
    }
    if (g->state == ST_PLAY) return;
    rect_t c = pz_card(s, f, 300, g->state == ST_PAUSED ? 104 : 150, 110);
    int y = c.y + 18;
    if (g->state == ST_PAUSED) {
        pz_text_c(s, c.x, y, c.w, "Paused", t->window_text, FONT_UI_TITLE);
        pz_text_c(s, c.x, y + 46, c.w, "Press P or click to resume", pz_muted(), FONT_UI);
    } else if (g->state == ST_CLEAR) {
        snprintf(buf, sizeof buf, "Level %d cleared!", g->level);
        pz_text_c(s, c.x, y, c.w, buf, t->window_text, FONT_UI_TITLE);
        snprintf(buf, sizeof buf, "Bonus +%d", g->bonus);
        pz_text_c(s, c.x, y + 46, c.w, buf, t->accent, FONT_UI_LARGE);
        pz_text_c(s, c.x, y + 88, c.w, "Click or press Space to continue", pz_muted(), FONT_UI);
    } else if (g->state == ST_OVER) {
        pz_text_c(s, c.x, y, c.w, "Game Over", t->window_text, FONT_UI_TITLE);
        snprintf(buf, sizeof buf, "Score %d  -  level %d", g->score, g->level);
        pz_text_c(s, c.x, y + 44, c.w, buf, t->window_text, FONT_UI);
        if (g->new_best) pz_text_c(s, c.x, y + 64, c.w, "New best score!", t->accent, FONT_UI_BOLD);
        else {
            snprintf(buf, sizeof buf, "Best %d", g->best);
            pz_text_c(s, c.x, y + 64, c.w, buf, pz_muted(), FONT_UI);
        }
        pz_text_c(s, c.x, y + 92, c.w, "Click or press Space to play again", pz_muted(), FONT_UI);
    }
}

static void bo_paint(bo_t *g, surface_t *s) {
    draw_hud(g, s);
    hud_signature(g, g->hud_sig, sizeof g->hud_sig);
    if (g->layer) gfx_blit(s, 0, BO_HUD, g->layer);
    rect_t old = s->clip;
    surface_set_clip(s, rect_intersect(old, field_rect()));
    draw_sprites(g, s);
    draw_overlay(g, s);
    s->clip = old;
    g->nprev = sprite_rects(g, g->prev);
}

/* ---- game flow ------------------------------------------------------------------------- */
static void set_cursor(bo_t *g) {
    win_set_cursor(g->win, (g->state == ST_PLAY || g->state == ST_SERVE) ? CURSOR_NONE : CURSOR_ARROW);
}

static void set_state(bo_t *g, int st) {
    g->state = st;
    set_cursor(g);
    win_invalidate(g->win);
}

static void park_ball(bo_t *g) {
    for (int i = 0; i < MAX_BALLS; i++) g->balls[i].alive = false;
    ball_t *b = &g->balls[0];
    b->alive = true;
    b->x = g->px;
    b->y = PADDLE_Y - BALL_R - 1;
    b->vx = 0;
    b->vy = -1;
}

static void serve(bo_t *g) {
    g->nparts = 0;
    for (int i = 0; i < MAX_CAPS; i++) g->caps[i].alive = false;
    g->wide_until = g->slow_until = 0;
    g->pw_target = PADDLE_W;
    park_ball(g);
    set_state(g, ST_SERVE);
}

static void load_level(bo_t *g) {
    const char *const *map = level_maps[(g->level - 1) % NLEVELS];
    memset(g->hits, 0, sizeof g->hits);
    memset(g->maxhits, 0, sizeof g->maxhits);
    g->rows = 0;
    g->bricks_left = 0;
    for (int r = 0; r < MAXROWS && map[r]; r++) {
        g->rows = r + 1;
        for (int c = 0; c < COLS && map[r][c]; c++) {
            int h = map[r][c] >= '1' && map[r][c] <= '3' ? map[r][c] - '0' : 0;
            g->hits[r][c] = g->maxhits[r][c] = (uint8_t)h;
            if (h) g->bricks_left++;
        }
    }
    g->speed = MIN(300.0 + 22.0 * (g->level - 1), 520.0);
    g->fresh = true;
    render_layer(g);
    serve(g);
}

static void new_game(bo_t *g) {
    g->level = 1;
    g->lives = LIVES;
    g->score = 0;
    g->new_best = false;
    g->best = pz_score_get("breakout_best", 0);
    g->px = BO_W / 2.0;
    g->pw = g->pw_target = PADDLE_W;
    load_level(g);
}

static void launch_ball(bo_t *g) {
    ball_t *b = &g->balls[0];
    double ang = pz_rand_f(&g->rng, -0.45, 0.45);
    if (g->key_left) ang = -0.5;
    if (g->key_right) ang = 0.5;
    b->vx = sin(ang);
    b->vy = -cos(ang);
    g->last_ms = sys_uptime_ms();
    g->fresh = false;
    sys_beep(660, 20);
    set_state(g, ST_PLAY);
}

static void game_over(bo_t *g) {
    if (g->score > g->best) {
        g->new_best = g->best > 0 || g->score > 0;
        g->best = g->score;
        pz_score_set("breakout_best", g->best);
    }
    static const note_t tune[] = { { 392, 140 }, { 311, 140 }, { 262, 140 }, { 196, 320 } };
    sys_play_notes(tune, ARRAY_SIZE(tune));
    set_state(g, ST_OVER);
}

static void lose_life(bo_t *g) {
    g->lives--;
    if (g->lives <= 0) { g->lives = 0; game_over(g); return; }
    static const note_t tune[] = { { 330, 90 }, { 262, 160 } };
    sys_play_notes(tune, ARRAY_SIZE(tune));
    serve(g);
}

static void level_cleared(bo_t *g) {
    g->bonus = 100 * g->level + 50 * g->lives;
    g->score += g->bonus;
    static const note_t tune[] = { { 523, 90 }, { 659, 90 }, { 784, 90 }, { 1047, 160 }, { 784, 90 }, { 1047, 260 } };
    sys_play_notes(tune, ARRAY_SIZE(tune));
    set_state(g, ST_CLEAR);
}

static void spawn_particles(bo_t *g, rect_t b, uint32_t col) {
    for (int i = 0; i < 14 && g->nparts < MAX_PARTS; i++) {
        part_t *p = &g->parts[g->nparts++];
        p->x = (float)(b.x + pz_rand_n(&g->rng, b.w));
        p->y = (float)(b.y + pz_rand_n(&g->rng, b.h));
        p->vx = (float)pz_rand_f(&g->rng, -150, 150);
        p->vy = (float)pz_rand_f(&g->rng, -170, 40);
        p->max = p->life = (float)pz_rand_f(&g->rng, 0.35, 0.7);
        p->col = color_shade(col, pz_rand_n(&g->rng, 60) - 10);
        p->size = 2 + pz_rand_n(&g->rng, 3);
    }
}

static void spawn_cap(bo_t *g, rect_t b) {
    if (pz_rand_n(&g->rng, 100) >= 14) return;
    for (int i = 0; i < MAX_CAPS; i++) {
        cap_t *c = &g->caps[i];
        if (c->alive) continue;
        int k = pz_rand_n(&g->rng, 100);
        c->kind = k < 32 ? CAP_WIDE : k < 60 ? CAP_SLOW : k < 88 ? CAP_MULTI : CAP_LIFE;
        c->x = b.x + b.w / 2.0;
        c->y = b.y + b.h / 2.0;
        c->alive = true;
        return;
    }
}

static void apply_cap(bo_t *g, int kind) {
    uint32_t now = sys_uptime_ms();
    switch (kind) {
    case CAP_WIDE: g->wide_until = now + 15000; g->pw_target = PADDLE_WIDE; break;
    case CAP_SLOW: g->slow_until = now + 12000; break;
    case CAP_LIFE: if (g->lives < MAX_LIVES) g->lives++; break;
    case CAP_MULTI: {
        ball_t *src = NULL;
        for (int i = 0; i < MAX_BALLS; i++) if (g->balls[i].alive) { src = &g->balls[i]; break; }
        if (!src) break;
        double base = atan2(src->vx, -src->vy);
        int made = 0;
        for (int i = 0; i < MAX_BALLS && made < 2; i++) {
            if (g->balls[i].alive) continue;
            ball_t *b = &g->balls[i];
            double a = base + (made ? -0.5 : 0.5);
            a = CLAMP(a, -1.1, 1.1);
            *b = *src;
            b->vx = sin(a);
            b->vy = -cos(a);
            b->alive = true;
            made++;
        }
        break;
    }
    }
    static const note_t tune[] = { { 880, 50 }, { 1175, 50 }, { 1568, 80 } };
    sys_play_notes(tune, ARRAY_SIZE(tune));
}

static void hit_brick(bo_t *g, int r, int c) {
    rect_t b = brick_rect(r, c);
    g->brick_dirty = rect_union(g->brick_dirty, rect_make(b.x - 2, b.y - 2, b.w + 4, b.h + 6));
    if (--g->hits[r][c] > 0) {
        sys_beep(300, 14);
        return;
    }
    int mh = g->maxhits[r][c];
    g->score += 10 + (g->rows - 1 - r) * 5 + 15 * (mh - 1);
    g->bricks_left--;
    sys_beep((uint32_t)(520 + (g->rows - r) * 60), 16);
    spawn_particles(g, b, brick_color(g, r, c));
    spawn_cap(g, b);
}

/* Is the ball (circle) overlapping a brick? Damages the first one found. */
static bool collide_bricks(bo_t *g, ball_t *b) {
    int c0 = (int)((b->x - BALL_R - BRICK_X0) / (BRICK_W + BRICK_GAP)) - 1;
    int c1 = (int)((b->x + BALL_R - BRICK_X0) / (BRICK_W + BRICK_GAP)) + 1;
    int r0 = (int)((b->y - BALL_R - BRICK_Y0) / (BRICK_H + BRICK_GAP)) - 1;
    int r1 = (int)((b->y + BALL_R - BRICK_Y0) / (BRICK_H + BRICK_GAP)) + 1;
    c0 = MAX(c0, 0); c1 = MIN(c1, COLS - 1);
    r0 = MAX(r0, 0); r1 = MIN(r1, g->rows - 1);
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++) {
            if (!g->hits[r][c]) continue;
            rect_t br = brick_rect(r, c);
            double nx = CLAMP(b->x, (double)br.x, (double)(br.x + br.w));
            double ny = CLAMP(b->y, (double)br.y, (double)(br.y + br.h));
            double dx = b->x - nx, dy = b->y - ny;
            if (dx * dx + dy * dy < BALL_R * BALL_R) {
                hit_brick(g, r, c);
                return true;
            }
        }
    return false;
}

static void wall_beep(bo_t *g) {
    uint32_t now = sys_uptime_ms();
    if (now - g->last_wall_beep > 60) { sys_beep(420, 8); g->last_wall_beep = now; }
}

static void step_ball(bo_t *g, ball_t *b, double dist) {
    int steps = (int)ceil(dist / 3.0);
    if (steps < 1) steps = 1;
    double st = dist / steps;
    for (int i = 0; i < steps && b->alive; i++) {
        b->x += b->vx * st;
        if (b->x < BALL_R) { b->x = BALL_R; b->vx = fabs(b->vx); wall_beep(g); }
        else if (b->x > BO_W - BALL_R) { b->x = BO_W - BALL_R; b->vx = -fabs(b->vx); wall_beep(g); }
        else if (collide_bricks(g, b)) { b->x -= b->vx * st; b->vx = -b->vx; }

        b->y += b->vy * st;
        if (b->y < BALL_R) { b->y = BALL_R; b->vy = fabs(b->vy); wall_beep(g); }
        else if (collide_bricks(g, b)) { b->y -= b->vy * st; b->vy = -b->vy; }

        /* paddle */
        double half = g->pw / 2;
        if (b->vy > 0 && b->y + BALL_R >= PADDLE_Y && b->y + BALL_R <= PADDLE_Y + PADDLE_H &&
            b->x >= g->px - half - BALL_R * 0.7 && b->x <= g->px + half + BALL_R * 0.7) {
            double off = CLAMP((b->x - g->px) / (half + BALL_R * 0.7), -1.0, 1.0);
            double ang = off * 1.08;                      /* up to ~62 degrees */
            b->vx = sin(ang);
            b->vy = -cos(ang);
            b->y = PADDLE_Y - BALL_R;
            g->speed = MIN(g->speed * 1.012, 640.0);
            sys_beep(560, 12);
        }
        if (b->y - BALL_R > BO_FH) b->alive = false;
    }
    /* keep the motion from getting too flat */
    if (fabs(b->vy) < 0.28) {
        b->vy = b->vy < 0 ? -0.28 : 0.28;
        b->vx = (b->vx < 0 ? -1 : 1) * sqrt(1 - 0.28 * 0.28);
    }
}

static void update(bo_t *g, double dt) {
    uint32_t now = sys_uptime_ms();
    /* paddle */
    if (g->key_left) g->px -= 600 * dt;
    if (g->key_right) g->px += 600 * dt;
    if (g->mouse_moved) { g->px = g->mouse_x; g->mouse_moved = false; }
    if (g->wide_until && now >= g->wide_until) { g->wide_until = 0; g->pw_target = PADDLE_W; }
    if (g->slow_until && now >= g->slow_until) g->slow_until = 0;
    g->pw += (g->pw_target - g->pw) * MIN(1.0, dt * 10);
    if (fabs(g->pw - g->pw_target) < 0.5) g->pw = g->pw_target;
    g->px = CLAMP(g->px, g->pw / 2, BO_W - g->pw / 2);

    if (g->state == ST_SERVE) {
        g->balls[0].x = g->px;
        return;
    }
    /* balls */
    double spd = g->speed * (g->slow_until ? 0.68 : 1.0);
    int alive = 0;
    for (int i = 0; i < MAX_BALLS; i++) {
        if (!g->balls[i].alive) continue;
        step_ball(g, &g->balls[i], spd * dt);
        alive += g->balls[i].alive;
    }
    /* capsules */
    for (int i = 0; i < MAX_CAPS; i++) {
        cap_t *c = &g->caps[i];
        if (!c->alive) continue;
        c->y += 150 * dt;
        if (c->y + 8 >= PADDLE_Y && c->y - 8 <= PADDLE_Y + PADDLE_H && fabs(c->x - g->px) <= g->pw / 2 + 17) {
            c->alive = false;
            apply_cap(g, c->kind);
        } else if (c->y - 10 > BO_FH) {
            c->alive = false;
        }
    }
    /* particles */
    int n = 0;
    for (int i = 0; i < g->nparts; i++) {
        part_t p = g->parts[i];
        p.life -= (float)dt;
        if (p.life <= 0) continue;
        p.x += p.vx * (float)dt;
        p.y += p.vy * (float)dt;
        p.vy += 520 * (float)dt;
        g->parts[n++] = p;
    }
    g->nparts = n;

    if (g->bricks_left <= 0) { level_cleared(g); return; }
    if (!alive) lose_life(g);
}

/* One animation frame: restore old sprite areas from the layer, draw the new ones. */
static void frame(bo_t *g) {
    surface_t *s = win_surface(g->win);
    if (!s || !g->layer) return;
    rect_t f = field_rect();
    surface_reset_clip(s);
    surface_set_clip(s, f);
    rect_t upd[MAX_RECTS * 2 + 2];
    int nu = 0;
    for (int i = 0; i < g->nprev; i++) {
        rect_t r = rect_intersect(g->prev[i], f);
        if (rect_empty(r)) continue;
        gfx_blit_rect(s, r.x, r.y, g->layer, r.x, r.y - BO_HUD, r.w, r.h);
        upd[nu++] = r;
    }
    if (!rect_empty(g->brick_dirty)) {
        rect_t r = clip_field_local(g->brick_dirty);
        render_layer_rect(g, r);
        gfx_blit_rect(s, r.x, r.y + BO_HUD, g->layer, r.x, r.y, r.w, r.h);
        upd[nu++] = rect_make(r.x, r.y + BO_HUD, r.w, r.h);
        g->brick_dirty = rect_make(0, 0, 0, 0);
    }
    draw_sprites(g, s);
    g->nprev = sprite_rects(g, g->prev);
    for (int i = 0; i < g->nprev; i++) upd[nu++] = g->prev[i];
    surface_reset_clip(s);
    for (int i = 0; i < nu; i++)
        if (!rect_empty(upd[i])) win_update(g->win, upd[i].x, upd[i].y, upd[i].w, upd[i].h);
    /* HUD only when something visible changed */
    char sig[96];
    hud_signature(g, sig, sizeof sig);
    if (strcmp(sig, g->hud_sig)) {
        strcpy(g->hud_sig, sig);
        draw_hud(g, s);
        win_update(g->win, 0, 0, BO_W, BO_HUD);
    }
}

static void bo_timer(bo_t *g) {
    uint32_t now = sys_uptime_ms();
    double dt = (now - g->last_ms) / 1000.0;
    g->last_ms = now;
    if (dt > 0.04) dt = 0.04;
    if (g->state != ST_PLAY && g->state != ST_SERVE) return;
    int before = g->state;
    update(g, dt);
    if (g->state == before) frame(g);
}

static void toggle_pause(bo_t *g) {
    if (g->state == ST_PLAY) set_state(g, ST_PAUSED);
    else if (g->state == ST_PAUSED) { g->last_ms = sys_uptime_ms(); set_state(g, ST_PLAY); }
}

static void bo_action(bo_t *g) {
    switch (g->state) {
    case ST_SERVE: launch_ball(g); break;
    case ST_PAUSED: toggle_pause(g); break;
    case ST_CLEAR: g->level++; load_level(g); break;
    case ST_OVER: new_game(g); break;
    default: break;
    }
}

static void bo_key(bo_t *g, const event_t *ev, bool down) {
    int k = ev->key;
    if (k == KEY_LEFT || k == 'a') { g->key_left = down; return; }
    if (k == KEY_RIGHT || k == 'd') { g->key_right = down; return; }
    if (!down || ev->repeat) return;
    if (k == 'p' || k == KEY_ESCAPE) toggle_pause(g);
    else if (k == KEY_SPACE || k == KEY_ENTER) bo_action(g);
    else if (k == KEY_F2 || k == 'n') new_game(g);
    else if (k == KEY_F11 && sys_cmdline_get("pztest") && g->state == ST_PLAY) {
        /* test hook: drop one capsule of each kind */
        for (int i = 0; i < CAP_COUNT && i < MAX_CAPS; i++)
            g->caps[i] = (cap_t){ .x = g->px - 48 + i * 32, .y = PADDLE_Y - 40 - i * 12, .kind = i, .alive = true };
    }
    else if (k == KEY_F12 && sys_cmdline_get("pztest") && g->state == ST_PLAY) {
        /* test hook (kernel command line "pztest" only): clear the board */
        memset(g->hits, 0, sizeof g->hits);
        g->bricks_left = 0;
        render_layer(g);
        win_invalidate(g->win);
    }
}

static void bo_event(window_t *win, const event_t *ev) {
    bo_t *g = win_get_userdata(win);
    if (!g) return;
    switch (ev->type) {
    case EV_PAINT:
        bo_paint(g, win_surface(win));
        break;
    case EV_TIMER:
        bo_timer(g);
        break;
    case EV_MOUSE_MOVE:
        if (ev->y >= BO_HUD - 20) { g->mouse_x = ev->x; g->mouse_moved = true; }
        break;
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_LEFT) {
            if (g->state == ST_PLAY) break;
            g->mouse_x = ev->x;
            g->mouse_moved = g->state == ST_SERVE;
            bo_action(g);
        } else if (ev->button == MOUSE_RIGHT) {
            toggle_pause(g);
        }
        break;
    case EV_KEY_DOWN:
        bo_key(g, ev, true);
        break;
    case EV_KEY_UP:
        bo_key(g, ev, false);
        break;
    case EV_BLUR:
        g->key_left = g->key_right = false;
        if (g->state == ST_PLAY) set_state(g, ST_PAUSED);
        break;
    case EV_CLOSE:
        if (g->layer) surface_destroy(g->layer);
        free(g);
        win_set_userdata(win, NULL);
        break;
    default:
        break;
    }
}

static void bo_launch(const char *arg) {
    (void)arg;
    bo_t *g = calloc(1, sizeof *g);
    if (!g) return;
    g->layer = surface_create(BO_W, BO_FH);
    if (!g->layer) { free(g); return; }
    pz_rng_seed(&g->rng, (uint32_t)(uintptr_t)g);
    g->win = win_create("Breakout", BO_W, BO_H, WIN_CENTER);
    if (!g->win) { surface_destroy(g->layer); free(g); return; }
    win_set_userdata(g->win, g);
    win_set_event_handler(g->win, bo_event);
    new_game(g);
    g->last_ms = sys_uptime_ms();
    g->timer = win_set_timer(g->win, 16);
}

/* Icon: a night-blue tile with three rows of bricks, a ball and a paddle. */
static void bo_icon(surface_t *s, int x, int y, int size) {
    double u = size / 48.0;
    gfx_fill_rounded_gradient_v(s, x + (int)(3 * u), y + (int)(3 * u), (int)(42 * u), (int)(42 * u), (int)(9 * u),
                                RGB(36, 44, 92), RGB(70, 36, 104));
    static const uint32_t cols[3] = { RGB(246, 84, 96), RGB(250, 190, 60), RGB(80, 200, 120) };
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) {
            if (r == 2 && c == 1) continue;     /* a knocked-out brick */
            int bx = x + (int)((8 + c * 8.3) * u), by = y + (int)((9 + r * 5.6) * u);
            gfx_fill_rounded_rect(s, bx, by, (int)(7.2 * u), (int)(4.4 * u), (int)MAX(1, 1.2 * u), cols[r]);
        }
    gfx_fill_circle_aa(s, x + 21 * u, y + 30 * u, 3.2 * u, RGB(248, 250, 255));
    gfx_fill_rounded_gradient_v(s, x + (int)(14 * u), y + (int)(36 * u), (int)(20 * u), (int)(4.6 * u), (int)(2.3 * u),
                                RGB(120, 190, 255), RGB(50, 120, 240));
}

REGISTER_APP(.name = "Breakout", .category = CAT_GAMES,
             .description = "Smash every brick with the ball and paddle",
             .launch = bo_launch, .draw_icon = bo_icon);
