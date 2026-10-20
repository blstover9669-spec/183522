/*
 * puzzle_pong.c - Pong (Games).
 *
 *   - you play the left paddle (mouse, or Up/Down / W/S), the computer the
 *     right one; first to 10 points wins the match;
 *   - the computer predicts where the ball will arrive (including wall
 *     bounces) and moves there with limited speed, a reaction delay and an
 *     aiming error that depend on the difficulty (Easy / Normal / Hard, the
 *     segmented control in the top bar or keys 1-3; applies immediately);
 *   - the ball speeds up with every return, the bounce angle depends on where
 *     it meets the paddle; sparks on hits, a short trail behind the ball;
 *   - per-window tally of matches won, Space / click starts and serves,
 *     P, Esc or a right click pauses (also when the window loses focus);
 *     resuming leaves the ball where it was for a moment before it moves on.
 *
 * Rendering follows Breakout: the table (backdrop, centre line, big scores)
 * is cached in an offscreen layer; each frame only the sprites' rectangles are
 * restored from it, redrawn and pushed with win_update().
 */
#include "puzzle_common.h"

#define PG_W        640
#define PG_BAR      44
#define PG_FH       440
#define PG_H        (PG_BAR + PG_FH)
#define PAD_W       12
#define PAD_H       78
#define PAD_X_L     26
#define PAD_X_R     (PG_W - 26 - PAD_W)
#define BALL_R      7
#define WIN_SCORE   10
#define TRAIL       8
#define MAX_PARTS   96
#define MAX_RECTS   16
#define SERVE_MS    1000
#define HOLD_MS     700
#define BALL_V0     330.0
#define BALL_VMAX   780.0

enum { ST_TITLE, ST_SERVE, ST_PLAY, ST_PAUSED, ST_OVER };

typedef struct { float x, y, vx, vy, life, max; uint32_t col; int size; } part_t;

typedef struct {
    int speed;          /* px/s */
    double react;       /* starts tracking when the ball passes this fraction of the width */
    int error;          /* +- px of aiming error */
} ai_level_t;

static const ai_level_t ai_levels[3] = {
    { 250, 0.55, 46 },
    { 370, 0.40, 26 },
    { 540, 0.18, 12 },
};
static const char *const diff_names[3] = { "Easy", "Normal", "Hard" };

typedef struct {
    window_t *win;
    pz_rng_t rng;
    int state;
    int diff;
    int score[2];                  /* [0] you, [1] computer */
    int matches[2];
    double py[2];                  /* paddle centres (field coords) */
    double bx, by, vx, vy, speed;  /* ball; (vx, vy) unit vector */
    double tx[TRAIL], ty[TRAIL];
    int ntrail;
    double ai_target, ai_err;
    bool ai_tracking;
    int serve_dir;                 /* -1 towards you, +1 towards the computer */
    uint32_t serve_at;
    uint32_t hold_until;           /* after a resume the ball stays put until then */
    int paused_from;               /* ST_PLAY or ST_SERVE */
    uint32_t last_ms;
    int rally;
    part_t parts[MAX_PARTS];
    int nparts;
    surface_t *layer;
    rect_t prev[MAX_RECTS];
    int nprev;
    bool key_up, key_down;
    int mouse_y;
    bool mouse_moved;
    int seg_hover;
    rect_t seg_r;
    char msg[48];                  /* drawn on the table (layer) */
    uint32_t msg_col;
} pong_t;

/* ---- table layer ----------------------------------------------------------------------- */
static rect_t field_rect(void) { return rect_make(0, PG_BAR, PG_W, PG_FH); }

static void render_layer(pong_t *g) {
    surface_t *s = g->layer;
    if (!s) return;
    surface_reset_clip(s);
    gfx_gradient_v(s, 0, 0, PG_W, PG_FH, RGB(14, 30, 44), RGB(20, 46, 58));
    /* soft vignette at the goals */
    gfx_gradient_h(s, 0, 0, 60, PG_FH, RGBA(0, 0, 0, 70), RGBA(0, 0, 0, 0));
    gfx_gradient_h(s, PG_W - 60, 0, 60, PG_FH, RGBA(0, 0, 0, 0), RGBA(0, 0, 0, 70));
    /* rails and centre line */
    gfx_fill_rect(s, 0, 0, PG_W, 3, RGBA(255, 255, 255, 40));
    gfx_fill_rect(s, 0, PG_FH - 3, PG_W, 3, RGBA(255, 255, 255, 40));
    for (int y = 10; y < PG_FH - 10; y += 22)
        gfx_fill_rounded_rect(s, PG_W / 2 - 2, y, 4, 12, 2, RGBA(255, 255, 255, 46));
    gfx_circle_aa(s, PG_W / 2.0, PG_FH / 2.0, 46, 2, RGBA(255, 255, 255, 22));
    /* scores */
    char buf[8];
    snprintf(buf, sizeof buf, "%d", g->score[0]);
    gfx_text_aligned(s, rect_make(PG_W / 2 - 150, 18, 120, 64), buf, RGBA(255, 255, 255, 200), FONT_UI_HUGE,
                     ALIGN_CENTER | ALIGN_MIDDLE);
    snprintf(buf, sizeof buf, "%d", g->score[1]);
    gfx_text_aligned(s, rect_make(PG_W / 2 + 30, 18, 120, 64), buf, RGBA(255, 255, 255, 200), FONT_UI_HUGE,
                     ALIGN_CENTER | ALIGN_MIDDLE);
    gfx_text_aligned(s, rect_make(PG_W / 2 - 150, 82, 120, 16), "YOU", RGBA(150, 200, 255, 170), FONT_UI_BOLD, ALIGN_CENTER);
    gfx_text_aligned(s, rect_make(PG_W / 2 + 30, 82, 120, 16), "COMPUTER", RGBA(255, 170, 140, 170), FONT_UI_BOLD,
                     ALIGN_CENTER);
    if (g->msg[0]) {
        /* a dark pill behind the text keeps it readable over the centre line */
        int tw = font_text_width(FONT_UI_LARGE, g->msg) + 36;
        gfx_fill_rounded_rect(s, (PG_W - tw) / 2, PG_FH - 94, tw, 34, 17, RGBA(6, 16, 26, 200));
        gfx_text_aligned(s, rect_make(0, PG_FH - 92, PG_W, 30), g->msg, g->msg_col, FONT_UI_LARGE,
                         ALIGN_CENTER | ALIGN_MIDDLE);
    }
}

/* ---- sprites ------------------------------------------------------------------------------ */
static uint32_t pad_color(int side) {
    return side == 0 ? theme_get()->accent : RGB(255, 124, 92);
}

static rect_t pad_box(pong_t *g, int side) {
    int x = side == 0 ? PAD_X_L : PAD_X_R;
    return rect_make(x - 8, PG_BAR + (int)(g->py[side] - PAD_H / 2) - 8, PAD_W + 16, PAD_H + 18);
}

static rect_t ball_box(pong_t *g) {
    int r = BALL_R + 7;
    rect_t u = rect_make((int)g->bx - r, PG_BAR + (int)g->by - r, 2 * r + 1, 2 * r + 1);
    for (int i = 0; i < g->ntrail; i++)
        u = rect_union(u, rect_make((int)g->tx[i] - BALL_R - 1, PG_BAR + (int)g->ty[i] - BALL_R - 1, 2 * BALL_R + 3,
                                    2 * BALL_R + 3));
    return u;
}

static rect_t parts_box(pong_t *g) {
    rect_t u = rect_make(0, 0, 0, 0);
    for (int i = 0; i < g->nparts; i++)
        u = rect_union(u, rect_make((int)g->parts[i].x - 1, PG_BAR + (int)g->parts[i].y - 1, g->parts[i].size + 2,
                                    g->parts[i].size + 2));
    return u;
}

static bool ball_visible(pong_t *g) {
    if (g->state == ST_TITLE || g->state == ST_OVER) return false;
    if (g->state == ST_SERVE) {
        /* blink while waiting to serve */
        uint32_t left = g->serve_at - sys_uptime_ms();
        return (int32_t)left <= 0 || (left / 160) % 2 == 0;
    }
    return true;
}

static void draw_paddle(pong_t *g, surface_t *s, int side) {
    int x = side == 0 ? PAD_X_L : PAD_X_R, y = PG_BAR + (int)lround(g->py[side] - PAD_H / 2);
    uint32_t c = pad_color(side);
    gfx_fill_rounded_rect(s, x - 3, y - 3, PAD_W + 6, PAD_H + 6, 9, color_alpha(c, 40));
    gfx_fill_rounded_rect(s, x + 1, y + 4, PAD_W, PAD_H, 6, RGBA(0, 0, 0, 110));
    gfx_fill_rounded_gradient_v(s, x, y, PAD_W, PAD_H, 6, color_shade(c, 70), color_shade(c, -30));
    gfx_fill_rounded_rect(s, x + 3, y + 5, 3, PAD_H - 10, 1, RGBA(255, 255, 255, 110));
}

static void draw_ball(pong_t *g, surface_t *s) {
    for (int i = 0; i < g->ntrail; i++) {
        int a = 18 + 50 * (i + 1) / TRAIL;
        double r = BALL_R * (0.45 + 0.5 * (i + 1) / TRAIL);
        gfx_fill_circle_aa(s, g->tx[i], PG_BAR + g->ty[i], r, RGBA(190, 230, 255, a));
    }
    double cx = g->bx, cy = PG_BAR + g->by;
    gfx_fill_circle_aa(s, cx, cy, BALL_R + 6, RGBA(170, 225, 255, 24));
    gfx_fill_circle_aa(s, cx, cy, BALL_R + 2.5, RGBA(190, 235, 255, 56));
    gfx_fill_circle_aa(s, cx, cy, BALL_R, RGB(248, 252, 255));
    gfx_fill_circle_aa(s, cx - 2.2, cy - 2.4, 2.3, COLOR_WHITE);
}

static void draw_parts(pong_t *g, surface_t *s) {
    for (int i = 0; i < g->nparts; i++) {
        part_t *p = &g->parts[i];
        int a = (int)(255 * p->life / p->max);
        gfx_fill_rect(s, (int)p->x, PG_BAR + (int)p->y, p->size, p->size, color_alpha(p->col, CLAMP(a, 0, 255)));
    }
}

static int sprite_rects(pong_t *g, rect_t *out) {
    int n = 0;
    out[n++] = pad_box(g, 0);
    out[n++] = pad_box(g, 1);
    if (g->state != ST_TITLE && g->state != ST_OVER) out[n++] = ball_box(g);
    if (g->nparts) out[n++] = parts_box(g);
    for (int i = 0; i < n; i++) out[i] = rect_intersect(out[i], field_rect());
    return n;
}

static void draw_sprites(pong_t *g, surface_t *s) {
    draw_parts(g, s);
    draw_paddle(g, s, 0);
    draw_paddle(g, s, 1);
    if (ball_visible(g)) draw_ball(g, s);
}

/* ---- top bar ---------------------------------------------------------------------------------- */
static void draw_bar(pong_t *g, surface_t *s) {
    const theme_t *t = theme_get();
    uint32_t bg = t->dark ? color_mix(t->window_bg, COLOR_BLACK, 40) : t->panel_bg;
    gfx_fill_rect(s, 0, 0, PG_W, PG_BAR, bg);
    gfx_hline(s, 0, PG_BAR - 1, PG_W, t->window_border);
    char buf[48];
    snprintf(buf, sizeof buf, "First to %d", WIN_SCORE);
    gfx_text(s, 16, (PG_BAR - font_height(FONT_UI_BOLD)) / 2, buf, t->window_text, FONT_UI_BOLD);
    pz_seg_draw(s, g->seg_r, diff_names, 3, g->diff, g->seg_hover);
    snprintf(buf, sizeof buf, "Matches  You %d - %d CPU", g->matches[0], g->matches[1]);
    int w = font_text_width(FONT_UI, buf);
    gfx_text(s, PG_W - 16 - w, (PG_BAR - font_height(FONT_UI)) / 2, buf, pz_muted(), FONT_UI);
}

/* ---- overlay cards (EV_PAINT only) ------------------------------------------------------------- */
static void draw_overlay(pong_t *g, surface_t *s) {
    const theme_t *t = theme_get();
    if (g->state != ST_TITLE && g->state != ST_PAUSED && g->state != ST_OVER) return;
    rect_t f = field_rect();
    int h = g->state == ST_PAUSED ? 104 : 168;
    rect_t c = pz_card(s, f, 320, h, 90);
    int y = c.y + 18;
    char buf[64];
    if (g->state == ST_PAUSED) {
        pz_text_c(s, c.x, y, c.w, "Paused", t->window_text, FONT_UI_TITLE);
        pz_text_c(s, c.x, y + 46, c.w, "Press P or click to resume", pz_muted(), FONT_UI);
        return;
    }
    if (g->state == ST_TITLE) {
        pz_text_c(s, c.x, y, c.w, "Pong", t->window_text, FONT_UI_TITLE);
        snprintf(buf, sizeof buf, "First to %d points wins  -  %s", WIN_SCORE, diff_names[g->diff]);
        pz_text_c(s, c.x, y + 44, c.w, buf, t->window_text, FONT_UI);
        pz_text_c(s, c.x, y + 66, c.w, "Mouse or Up/Down moves your paddle", pz_muted(), FONT_UI);
        pz_text_c(s, c.x, y + 84, c.w, "1 / 2 / 3 pick the difficulty, P pauses", pz_muted(), FONT_UI);
        pz_text_c(s, c.x, y + 114, c.w, "Click or press Space to start", t->accent, FONT_UI_BOLD);
        return;
    }
    bool won = g->score[0] > g->score[1];
    pz_text_c(s, c.x, y, c.w, won ? "You win!" : "Computer wins", won ? t->accent : t->window_text, FONT_UI_TITLE);
    snprintf(buf, sizeof buf, "%d - %d", g->score[0], g->score[1]);
    pz_text_c(s, c.x, y + 42, c.w, buf, t->window_text, FONT_UI_LARGE);
    snprintf(buf, sizeof buf, "Matches: you %d, computer %d", g->matches[0], g->matches[1]);
    pz_text_c(s, c.x, y + 74, c.w, buf, pz_muted(), FONT_UI);
    pz_text_c(s, c.x, y + 114, c.w, "Click or press Space for a rematch", t->accent, FONT_UI_BOLD);
}

static void pg_paint(pong_t *g, surface_t *s) {
    draw_bar(g, s);
    if (g->layer) gfx_blit(s, 0, PG_BAR, g->layer);
    rect_t old = s->clip;
    surface_set_clip(s, rect_intersect(old, field_rect()));
    draw_sprites(g, s);
    draw_overlay(g, s);
    s->clip = old;
    g->nprev = sprite_rects(g, g->prev);
}

/* ---- game flow ---------------------------------------------------------------------------------- */
static void set_msg(pong_t *g, const char *m, uint32_t col) {
    strlcpy(g->msg, m ? m : "", sizeof g->msg);
    g->msg_col = col;
    render_layer(g);
    win_invalidate(g->win);
}

static void set_state(pong_t *g, int st) {
    g->state = st;
    win_set_cursor(g->win, (st == ST_PLAY || st == ST_SERVE) ? CURSOR_NONE : CURSOR_ARROW);
    win_invalidate(g->win);
}

static void prepare_serve(pong_t *g, int dir) {
    g->bx = PG_W / 2.0;
    g->by = PG_FH / 2.0;
    g->ntrail = 0;
    g->serve_dir = dir;
    g->serve_at = sys_uptime_ms() + SERVE_MS;
    g->speed = BALL_V0 + 15 * g->diff;
    g->rally = 0;
    g->ai_tracking = false;
    set_state(g, ST_SERVE);
}

static void new_match(pong_t *g) {
    g->score[0] = g->score[1] = 0;
    g->py[0] = g->py[1] = PG_FH / 2.0;
    g->nparts = 0;
    g->msg[0] = 0;
    render_layer(g);
    prepare_serve(g, pz_rand_n(&g->rng, 2) ? 1 : -1);
}

static void launch(pong_t *g) {
    double ang = pz_rand_f(&g->rng, -0.5, 0.5);
    g->vx = g->serve_dir * cos(ang);
    g->vy = sin(ang);
    if (g->msg[0]) set_msg(g, NULL, 0);
    sys_beep(600, 18);
    set_state(g, ST_PLAY);
}

static void spawn_sparks(pong_t *g, double x, double y, int dir, uint32_t col, int n) {
    for (int i = 0; i < n && g->nparts < MAX_PARTS; i++) {
        part_t *p = &g->parts[g->nparts++];
        p->x = (float)x;
        p->y = (float)y;
        double a = pz_rand_f(&g->rng, -1.1, 1.1);
        double sp = pz_rand_f(&g->rng, 60, 240);
        p->vx = (float)(dir * cos(a) * sp);
        p->vy = (float)(sin(a) * sp);
        p->max = p->life = (float)pz_rand_f(&g->rng, 0.25, 0.55);
        p->col = color_shade(col, pz_rand_n(&g->rng, 70));
        p->size = 2 + pz_rand_n(&g->rng, 2);
    }
}

static void point_scored(pong_t *g, int who) {
    g->score[who]++;
    spawn_sparks(g, who == 0 ? PG_W - 4 : 4, CLAMP(g->by, 10.0, PG_FH - 10.0), who == 0 ? -1 : 1, RGB(255, 230, 160), 22);
    if (g->score[who] >= WIN_SCORE) {
        g->matches[who]++;
        g->msg[0] = 0;
        render_layer(g);
        if (who == 0) {
            static const note_t tune[] = { { 523, 100 }, { 659, 100 }, { 784, 100 }, { 1047, 260 } };
            sys_play_notes(tune, ARRAY_SIZE(tune));
        } else {
            static const note_t tune[] = { { 392, 140 }, { 330, 140 }, { 262, 300 } };
            sys_play_notes(tune, ARRAY_SIZE(tune));
        }
        set_state(g, ST_OVER);
        return;
    }
    static const note_t pt[] = { { 880, 60 }, { 660, 90 } };
    static const note_t lost[] = { { 300, 80 }, { 220, 120 } };
    if (who == 0) sys_play_notes(pt, ARRAY_SIZE(pt));
    else sys_play_notes(lost, ARRAY_SIZE(lost));
    bool match_pt = g->score[who] == WIN_SCORE - 1;
    char m[48];
    if (match_pt) snprintf(m, sizeof m, who == 0 ? "Point - match point for you!" : "Point - match point for the computer");
    else snprintf(m, sizeof m, who == 0 ? "Your point" : "Computer's point");
    strlcpy(g->msg, m, sizeof g->msg);
    g->msg_col = who == 0 ? RGBA(160, 210, 255, 230) : RGBA(255, 180, 150, 230);
    render_layer(g);
    /* the side that lost the point receives the next serve */
    prepare_serve(g, who == 0 ? 1 : -1);
}

/* Where will the ball cross x = target_x? Follows wall bounces. */
static double predict_y(pong_t *g, double target_x) {
    if (fabs(g->vx) < 1e-6) return g->by;
    double t = (target_x - g->bx) / g->vx;     /* distance along the unit vector, in px */
    if (t < 0) return g->by;
    double y = g->by + g->vy * t;
    double lo = BALL_R, hi = PG_FH - BALL_R, span = hi - lo;
    double m = fmod(y - lo, 2 * span);
    if (m < 0) m += 2 * span;
    return m <= span ? lo + m : hi - (m - span);
}

static void update_ai(pong_t *g, double dt) {
    const ai_level_t *L = &ai_levels[g->diff];
    double target;
    if (g->state == ST_PLAY && g->vx > 0 && g->bx > PG_W * L->react) {
        if (!g->ai_tracking) {
            g->ai_tracking = true;
            /* new error per approach: makes the computer beatable and varies its angles */
            g->ai_err = pz_rand_f(&g->rng, -L->error, L->error);
        }
        target = predict_y(g, PAD_X_R - BALL_R) + g->ai_err;
    } else {
        g->ai_tracking = false;
        target = g->state == ST_PLAY ? PG_FH / 2.0 + (g->by - PG_FH / 2.0) * 0.3 : PG_FH / 2.0;
    }
    double d = target - g->py[1];
    double step = L->speed * dt * (g->ai_tracking ? 1.0 : 0.5);
    if (fabs(d) <= step) g->py[1] = target;
    else g->py[1] += d > 0 ? step : -step;
    g->py[1] = CLAMP(g->py[1], PAD_H / 2.0 + 4, PG_FH - PAD_H / 2.0 - 4);
}

static void paddle_bounce(pong_t *g, int side) {
    double off = CLAMP((g->by - g->py[side]) / (PAD_H / 2.0 + BALL_R), -1.0, 1.0);
    double ang = off * 0.96;                         /* up to ~55 degrees */
    int dir = side == 0 ? 1 : -1;
    g->vx = dir * cos(ang);
    g->vy = sin(ang);
    g->speed = MIN(g->speed * 1.05, BALL_VMAX);
    g->rally++;
    g->bx = side == 0 ? PAD_X_L + PAD_W + BALL_R : PAD_X_R - BALL_R;
    spawn_sparks(g, side == 0 ? PAD_X_L + PAD_W : PAD_X_R, g->by, dir, pad_color(side), 9);
    sys_beep(side == 0 ? 640 : 520, 14);
}

static void step_ball(pong_t *g, double dist) {
    int steps = (int)ceil(dist / 3.0);
    if (steps < 1) steps = 1;
    double st = dist / steps;
    for (int i = 0; i < steps; i++) {
        g->bx += g->vx * st;
        g->by += g->vy * st;
        if (g->by < BALL_R + 3) { g->by = BALL_R + 3; g->vy = fabs(g->vy); sys_beep(420, 8); }
        if (g->by > PG_FH - BALL_R - 3) { g->by = PG_FH - BALL_R - 3; g->vy = -fabs(g->vy); sys_beep(420, 8); }
        /* paddles: only the front face counts, while the ball moves towards it */
        if (g->vx < 0 && g->bx - BALL_R <= PAD_X_L + PAD_W && g->bx - BALL_R >= PAD_X_L - 2 &&
            fabs(g->by - g->py[0]) <= PAD_H / 2.0 + BALL_R * 0.8)
            paddle_bounce(g, 0);
        else if (g->vx > 0 && g->bx + BALL_R >= PAD_X_R && g->bx + BALL_R <= PAD_X_R + PAD_W + 2 &&
                 fabs(g->by - g->py[1]) <= PAD_H / 2.0 + BALL_R * 0.8)
            paddle_bounce(g, 1);
        if (g->bx < -BALL_R) { point_scored(g, 1); return; }
        if (g->bx > PG_W + BALL_R) { point_scored(g, 0); return; }
    }
}

static void update_parts(pong_t *g, double dt) {
    int n = 0;
    for (int i = 0; i < g->nparts; i++) {
        part_t p = g->parts[i];
        p.life -= (float)dt;
        if (p.life <= 0) continue;
        p.x += p.vx * (float)dt;
        p.y += p.vy * (float)dt;
        p.vx *= 0.97f;
        p.vy *= 0.97f;
        g->parts[n++] = p;
    }
    g->nparts = n;
}

static void update(pong_t *g, double dt) {
    /* your paddle */
    if (g->key_up) g->py[0] -= 560 * dt;
    if (g->key_down) g->py[0] += 560 * dt;
    if (g->mouse_moved) { g->py[0] = g->mouse_y; g->mouse_moved = false; }
    g->py[0] = CLAMP(g->py[0], PAD_H / 2.0 + 4, PG_FH - PAD_H / 2.0 - 4);
    update_ai(g, dt);
    update_parts(g, dt);

    if (g->state == ST_SERVE) {
        if ((int32_t)(sys_uptime_ms() - g->serve_at) >= 0) launch(g);
        return;
    }
    if (g->state != ST_PLAY) return;
    if ((int32_t)(sys_uptime_ms() - g->hold_until) < 0) return;   /* just resumed */
    /* trail */
    if (g->ntrail == TRAIL) {
        memmove(g->tx, g->tx + 1, sizeof(double) * (TRAIL - 1));
        memmove(g->ty, g->ty + 1, sizeof(double) * (TRAIL - 1));
    } else {
        g->ntrail++;
    }
    g->tx[g->ntrail - 1] = g->bx;
    g->ty[g->ntrail - 1] = g->by;
    step_ball(g, g->speed * dt);
}

static void frame(pong_t *g) {
    surface_t *s = win_surface(g->win);
    if (!s || !g->layer) return;
    rect_t f = field_rect();
    surface_reset_clip(s);
    surface_set_clip(s, f);
    rect_t upd[MAX_RECTS * 2];
    int nu = 0;
    for (int i = 0; i < g->nprev; i++) {
        rect_t r = rect_intersect(g->prev[i], f);
        if (rect_empty(r)) continue;
        gfx_blit_rect(s, r.x, r.y, g->layer, r.x, r.y - PG_BAR, r.w, r.h);
        upd[nu++] = r;
    }
    draw_sprites(g, s);
    g->nprev = sprite_rects(g, g->prev);
    for (int i = 0; i < g->nprev; i++) upd[nu++] = g->prev[i];
    surface_reset_clip(s);
    for (int i = 0; i < nu; i++)
        if (!rect_empty(upd[i])) win_update(g->win, upd[i].x, upd[i].y, upd[i].w, upd[i].h);
}

static void pg_timer(pong_t *g) {
    uint32_t now = sys_uptime_ms();
    double dt = (now - g->last_ms) / 1000.0;
    g->last_ms = now;
    if (dt > 0.04) dt = 0.04;
    if (g->state != ST_PLAY && g->state != ST_SERVE) {
        /* idle (title, paused, match over): only the last sparks fade. They sit
           under the dimmed overlay card, so repaint them through EV_PAINT (which
           draws the overlay) instead of the sprite fast path. */
        if (!g->nparts) return;
        rect_t r = parts_box(g);
        update_parts(g, dt);
        r = rect_intersect(rect_union(r, parts_box(g)), field_rect());
        if (!rect_empty(r)) win_invalidate_rect(g->win, r.x, r.y, r.w, r.h);
        return;
    }
    int before = g->state;
    update(g, dt);
    if (g->state == before) frame(g);
}

static void toggle_pause(pong_t *g) {
    if (g->state == ST_PLAY || g->state == ST_SERVE) {
        g->paused_from = g->state;
        g->key_up = g->key_down = false;
        set_state(g, ST_PAUSED);
    } else if (g->state == ST_PAUSED) {
        uint32_t now = sys_uptime_ms();
        g->last_ms = now;
        if (g->paused_from == ST_SERVE) g->serve_at = now + SERVE_MS;
        else g->hold_until = now + HOLD_MS;      /* the ball waits a moment where it was */
        set_state(g, g->paused_from);
    }
}

static void pg_action(pong_t *g) {
    switch (g->state) {
    case ST_TITLE: case ST_OVER: new_match(g); break;
    case ST_PAUSED: toggle_pause(g); break;
    case ST_SERVE: launch(g); break;
    default: break;
    }
}

static void set_diff(pong_t *g, int d) {
    g->diff = CLAMP(d, 0, 2);
    pz_score_set("pong_level", g->diff);
    win_invalidate(g->win);
}

static void pg_key(pong_t *g, const event_t *ev, bool down) {
    int k = ev->key;
    if (k == KEY_UP || k == 'w') { g->key_up = down; return; }
    if (k == KEY_DOWN || k == 's') { g->key_down = down; return; }
    if (!down || ev->repeat) return;
    if (k == 'p' || k == KEY_ESCAPE) toggle_pause(g);
    else if (k == KEY_SPACE || k == KEY_ENTER) pg_action(g);
    else if (k >= '1' && k <= '3') set_diff(g, k - '1');
    else if (k == KEY_F2 || k == 'n') new_match(g);
}

static void pg_event(window_t *win, const event_t *ev) {
    pong_t *g = win_get_userdata(win);
    if (!g) return;
    switch (ev->type) {
    case EV_PAINT:
        pg_paint(g, win_surface(win));
        break;
    case EV_TIMER:
        pg_timer(g);
        break;
    case EV_MOUSE_MOVE: {
        int sh = pz_seg_hit(g->seg_r, 3, ev->x, ev->y);
        if (sh != g->seg_hover) {
            g->seg_hover = sh;
            win_invalidate_rect(win, g->seg_r.x, g->seg_r.y, g->seg_r.w, g->seg_r.h);
        }
        if (ev->y >= PG_BAR) { g->mouse_y = ev->y - PG_BAR; g->mouse_moved = true; }
        break;
    }
    case EV_MOUSE_LEAVE:
        if (g->seg_hover >= 0) { g->seg_hover = -1; win_invalidate_rect(win, g->seg_r.x, g->seg_r.y, g->seg_r.w, g->seg_r.h); }
        break;
    case EV_MOUSE_DOWN: {
        int sg = pz_seg_hit(g->seg_r, 3, ev->x, ev->y);
        if (sg >= 0 && ev->button == MOUSE_LEFT) { set_diff(g, sg); break; }
        if (ev->y < PG_BAR) break;
        if (ev->button == MOUSE_LEFT) pg_action(g);
        else if (ev->button == MOUSE_RIGHT) toggle_pause(g);
        break;
    }
    case EV_KEY_DOWN:
        pg_key(g, ev, true);
        break;
    case EV_KEY_UP:
        pg_key(g, ev, false);
        break;
    case EV_BLUR:
        g->key_up = g->key_down = false;
        if (g->state == ST_PLAY || g->state == ST_SERVE) toggle_pause(g);
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

static void pg_launch(const char *arg) {
    (void)arg;
    pong_t *g = calloc(1, sizeof *g);
    if (!g) return;
    g->layer = surface_create(PG_W, PG_FH);
    if (!g->layer) { free(g); return; }
    pz_rng_seed(&g->rng, (uint32_t)(uintptr_t)g);
    g->seg_hover = -1;
    g->seg_r = rect_make((PG_W - 240) / 2, (PG_BAR - 30) / 2, 240, 30);
    g->diff = CLAMP(pz_score_get("pong_level", 1), 0, 2);
    g->py[0] = g->py[1] = PG_FH / 2.0;
    g->win = win_create("Pong", PG_W, PG_H, WIN_CENTER);
    if (!g->win) { surface_destroy(g->layer); free(g); return; }
    win_set_userdata(g->win, g);
    win_set_event_handler(g->win, pg_event);
    render_layer(g);
    g->state = ST_TITLE;
    g->last_ms = sys_uptime_ms();
    win_set_timer(g->win, 16);
}

/* Icon: a dark table with a dashed centre line, two paddles and the ball. */
static void pg_icon(surface_t *s, int x, int y, int size) {
    double u = size / 48.0;
    gfx_fill_rounded_gradient_v(s, x + (int)(3 * u), y + (int)(5 * u), (int)(42 * u), (int)(38 * u), (int)(8 * u),
                                RGB(30, 66, 90), RGB(16, 36, 54));
    for (int i = 0; i < 4; i++)
        gfx_fill_rect(s, x + (int)(23 * u), y + (int)((10 + i * 8) * u), (int)MAX(1, 2 * u), (int)(4 * u),
                      RGB(120, 160, 180));
    gfx_fill_rounded_gradient_v(s, x + (int)(8 * u), y + (int)(13 * u), (int)(4 * u), (int)(14 * u), (int)(2 * u),
                                RGB(120, 190, 255), RGB(50, 120, 240));
    gfx_fill_rounded_gradient_v(s, x + (int)(36 * u), y + (int)(22 * u), (int)(4 * u), (int)(14 * u), (int)(2 * u),
                                RGB(255, 170, 140), RGB(240, 100, 70));
    gfx_fill_circle_aa(s, x + 17 * u, y + 20 * u, 2 * u, RGBA(220, 240, 255, 90));
    gfx_fill_circle_aa(s, x + 21 * u, y + 22 * u, 2.4 * u, RGBA(220, 240, 255, 140));
    gfx_fill_circle_aa(s, x + 27 * u, y + 25 * u, 3.2 * u, RGB(250, 252, 255));
}

REGISTER_APP(.name = "Pong", .category = CAT_GAMES,
             .description = "The classic: beat the computer to 10 points",
             .launch = pg_launch, .draw_icon = pg_icon);
