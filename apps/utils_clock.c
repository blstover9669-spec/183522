/*
 * utils_clock.c - Clock (Accessories): analog + digital clock, stopwatch and
 * countdown timer in three tabs.
 *
 * The clock face is rendered once into a cached surface (re-rendered on theme
 * changes); each frame only blits it and draws the hands. The second hand
 * sweeps smoothly: sys_time() only has whole seconds, so the sub-second phase
 * is taken from sys_uptime_ms() at the moment the second changes.
 * Stopwatch and timer are computed from sys_uptime_ms() deltas, so they keep
 * running while another tab is shown. The timer alarm uses the PC speaker
 * (sys_play_notes, non-blocking) and a toast notification.
 */
#include <nova.h>

#define CW 380
#define CH 520
#define FACE_R 136
#define FACE_CY 200
#define MAX_LAPS 99
#define LAP_ROW_H 22
#define LAP_TOP 372

enum { TAB_CLOCK, TAB_STOPWATCH, TAB_TIMER };
enum { T_IDLE, T_RUNNING, T_PAUSED, T_RINGING };

typedef struct {
    window_t *win;
    int tab;
    int timer;
    widget_t *tabs, *chk24;
    widget_t *sw_start, *sw_lap, *sw_reset;
    widget_t *t_start, *t_cancel, *t_preset[6];
    /* clock */
    surface_t *face;
    uint32_t face_bg, face_accent;
    uint32_t last_time, phase_ms;
    bool h24;
    int last_sec_drawn;
    /* stopwatch */
    bool sw_running;
    uint32_t sw_start_ms, sw_accum;
    uint32_t laps[MAX_LAPS];   /* total time at each lap */
    int nlaps, lap_scroll;
    /* countdown timer */
    int t_state;
    int t_val[3];              /* h, m, s being edited */
    int t_field;               /* field with keyboard focus */
    uint32_t t_total, t_remain, t_end;
    uint32_t t_ring_start, t_next_beep;
    uint32_t t_last_draw;
} clk_t;

static const int presets_min[6] = { 1, 3, 5, 10, 15, 30 };

/* ---- helpers ------------------------------------------------------------------------ */
static uint32_t now_ms(void) { return sys_uptime_ms(); }

static void fmt_hms_cs(uint32_t ms, char *out, int n) {
    uint32_t cs = (ms / 10) % 100, s = (ms / 1000) % 60, m = (ms / 60000) % 60, h = ms / 3600000;
    if (h) snprintf(out, n, "%u:%02u:%02u.%02u", h, m, s, cs);
    else snprintf(out, n, "%02u:%02u.%02u", m, s, cs);
}

static void fmt_remain(uint32_t ms, char *out, int n) {
    uint32_t t = (ms + 999) / 1000;   /* round up: shows 00:01 until the very end */
    uint32_t s = t % 60, m = (t / 60) % 60, h = t / 3600;
    if (h) snprintf(out, n, "%u:%02u:%02u", h, m, s);
    else snprintf(out, n, "%02u:%02u", m, s);
}

/* Thick anti-aliased segment (filled quad + Wu lines along the long edges). */
static void aa_stroke(surface_t *s, double x0, double y0, double x1, double y1, double wdt, uint32_t col) {
    double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
    if (len < 0.01) return;
    double nx = -dy / len * wdt / 2, ny = dx / len * wdt / 2;
    point_t p[4] = {
        { (int)lround(x0 + nx), (int)lround(y0 + ny) }, { (int)lround(x1 + nx), (int)lround(y1 + ny) },
        { (int)lround(x1 - nx), (int)lround(y1 - ny) }, { (int)lround(x0 - nx), (int)lround(y0 - ny) },
    };
    gfx_fill_polygon(s, p, 4, col);
    gfx_line_aa(s, x0 + nx, y0 + ny, x1 + nx, y1 + ny, col);
    gfx_line_aa(s, x0 - nx, y0 - ny, x1 - nx, y1 - ny, col);
}

/* Tapered clock hand from a short tail behind the centre to the tip; angle 0 = 12 o'clock. */
static void draw_hand(surface_t *s, double cx, double cy, double ang, double len, double tail,
                      double wbase, double wtip, uint32_t col) {
    double sx = sin(ang), sy = -cos(ang);
    double px = -sy, py = sx;    /* perpendicular */
    double bx = cx - sx * tail, by = cy - sy * tail;
    double tx = cx + sx * len, ty = cy + sy * len;
    double p[4][2] = {
        { bx + px * wbase / 2, by + py * wbase / 2 }, { tx + px * wtip / 2, ty + py * wtip / 2 },
        { tx - px * wtip / 2, ty - py * wtip / 2 }, { bx - px * wbase / 2, by - py * wbase / 2 },
    };
    point_t ip[4];
    for (int i = 0; i < 4; i++) { ip[i].x = (int)lround(p[i][0]); ip[i].y = (int)lround(p[i][1]); }
    gfx_fill_polygon(s, ip, 4, col);
    for (int i = 0; i < 4; i++) gfx_line_aa(s, p[i][0], p[i][1], p[(i + 1) % 4][0], p[(i + 1) % 4][1], col);
    gfx_fill_circle_aa(s, tx, ty, wtip / 2, col);
    gfx_fill_circle_aa(s, bx, by, wbase / 2, col);
}

/* Arc stroke between angles a0..a1 (radians, clockwise from 12 o'clock) with round caps. */
static void draw_arc(surface_t *s, double cx, double cy, double r, double thick, double a0, double a1, uint32_t col) {
    if (a1 - a0 < 0.001) return;
    int n = (int)((a1 - a0) / (2 * M_PI) * 120) + 2;
    if (n > 122) n = 122;
    point_t pts[2 * 123];
    double ro = r + thick / 2, ri = r - thick / 2;
    double prev_ox = 0, prev_oy = 0, prev_ix = 0, prev_iy = 0;
    for (int i = 0; i <= n; i++) {
        double a = a0 + (a1 - a0) * i / n;
        double ox = cx + sin(a) * ro, oy = cy - cos(a) * ro;
        double ix = cx + sin(a) * ri, iy = cy - cos(a) * ri;
        pts[i].x = (int)lround(ox); pts[i].y = (int)lround(oy);
        pts[2 * n + 1 - i].x = (int)lround(ix); pts[2 * n + 1 - i].y = (int)lround(iy);
        if (i) {
            gfx_line_aa(s, prev_ox, prev_oy, ox, oy, col);
            gfx_line_aa(s, prev_ix, prev_iy, ix, iy, col);
        }
        prev_ox = ox; prev_oy = oy; prev_ix = ix; prev_iy = iy;
    }
    gfx_fill_polygon(s, pts, 2 * n + 2, col);
    gfx_fill_circle_aa(s, cx + sin(a0) * r, cy - cos(a0) * r, thick / 2, col);
    gfx_fill_circle_aa(s, cx + sin(a1) * r, cy - cos(a1) * r, thick / 2, col);
}

static uint32_t muted(const theme_t *t) { return color_mix(t->window_text, t->window_bg, 110); }

/* ---- clock face ------------------------------------------------------------------- */
static void render_face(clk_t *c) {
    const theme_t *t = theme_get();
    int R = FACE_R, size = 2 * R + 16;
    if (!c->face) c->face = surface_create(size, size);
    if (!c->face) return;
    surface_t *s = c->face;
    gfx_clear(s, t->window_bg);
    double cx = size / 2.0, cy = size / 2.0 - 2;
    /* soft shadow */
    for (int i = 6; i >= 1; i--)
        gfx_fill_circle_aa(s, cx, cy + 3, R + i, RGBA(0, 0, 0, t->dark ? 14 : 7));
    uint32_t bezel = t->dark ? RGB(62, 65, 74) : RGB(222, 226, 234);
    uint32_t face = t->dark ? RGB(40, 42, 48) : RGB(255, 255, 255);
    uint32_t ink = t->dark ? RGB(226, 229, 235) : RGB(38, 42, 52);
    gfx_fill_circle_aa(s, cx, cy, R, bezel);
    gfx_fill_circle_aa(s, cx, cy, R - 7, face);
    gfx_circle_aa(s, cx, cy, R - 7.5, 1.5, color_alpha(t->accent, 150));
    /* ticks */
    for (int i = 0; i < 60; i++) {
        double a = i * M_PI / 30, sx = sin(a), sy = -cos(a);
        bool hour = i % 5 == 0;
        double r0 = R - 14, r1 = hour ? R - 28 : R - 20;
        aa_stroke(s, cx + sx * r0, cy + sy * r0, cx + sx * r1, cy + sy * r1, hour ? 3.2 : 1.3,
                  hour ? ink : color_mix(ink, face, 120));
    }
    /* numerals */
    for (int h = 1; h <= 12; h++) {
        double a = h * M_PI / 6, rr = R - 46;
        char num[4];
        snprintf(num, sizeof num, "%d", h);
        int tw = font_text_width(FONT_UI_LARGE, num), th = font_height(FONT_UI_LARGE);
        gfx_text(s, (int)lround(cx + sin(a) * rr - tw / 2.0), (int)lround(cy - cos(a) * rr - th / 2.0),
                 num, ink, FONT_UI_LARGE);
    }
    int bw = font_text_width(FONT_UI_BOLD, "NOVA");
    gfx_text(s, (int)(cx - bw / 2.0), (int)(cy - R * 0.42), "NOVA", color_alpha(t->accent, 220), FONT_UI_BOLD);
    c->face_bg = t->window_bg;
    c->face_accent = t->accent;
}

static void paint_clock(clk_t *c, surface_t *s) {
    const theme_t *t = theme_get();
    if (!c->face || c->face_bg != t->window_bg || c->face_accent != t->accent) render_face(c);
    int W = s->w;
    double cx = W / 2.0, cy = FACE_CY;
    if (c->face) gfx_blit(s, (int)cx - c->face->w / 2, FACE_CY + 2 - c->face->h / 2, c->face);

    datetime_t dt;
    sys_datetime(&dt);
    uint32_t now = now_ms();
    double frac = (c->last_time == sys_time()) ? (now - c->phase_ms) / 1000.0 : 0;
    if (frac < 0) frac = 0;
    if (frac > 0.999) frac = 0.999;
    double sec = dt.second + frac;
    double min = dt.minute + sec / 60.0;
    double hr = (dt.hour % 12) + min / 60.0;
    uint32_t ink = t->dark ? RGB(236, 238, 243) : RGB(34, 38, 48);
    uint32_t shadow = RGBA(0, 0, 0, t->dark ? 70 : 38);
    double ha = hr * M_PI / 6, ma = min * M_PI / 30, sa = sec * M_PI / 30;
    /* shadows first, then hands */
    draw_hand(s, cx + 2, cy + 3, ha, FACE_R * 0.50, 7, 9, 5, shadow);
    draw_hand(s, cx + 2, cy + 3, ma, FACE_R * 0.74, 8, 7, 3.5, shadow);
    draw_hand(s, cx, cy, ha, FACE_R * 0.50, 7, 9, 5, ink);
    draw_hand(s, cx, cy, ma, FACE_R * 0.74, 8, 7, 3.5, ink);
    uint32_t red = RGB(240, 84, 60);
    double sx = sin(sa), sy = -cos(sa);
    aa_stroke(s, cx + 2 - sx * 26, cy + 3 - sy * 26, cx + 2 + sx * (FACE_R - 22), cy + 3 + sy * (FACE_R - 22), 1.8, shadow);
    aa_stroke(s, cx - sx * 26, cy - sy * 26, cx + sx * (FACE_R - 22), cy + sy * (FACE_R - 22), 1.8, red);
    gfx_fill_circle_aa(s, cx - sx * 22, cy - sy * 22, 4, red);
    gfx_fill_circle_aa(s, cx, cy, 7, ink);
    gfx_fill_circle_aa(s, cx, cy, 4, red);
    gfx_fill_circle_aa(s, cx, cy, 1.6, t->dark ? RGB(40, 42, 48) : COLOR_WHITE);

    /* digital time + date */
    char buf[64];
    if (c->h24) snprintf(buf, sizeof buf, "%02d:%02d:%02d", dt.hour, dt.minute, dt.second);
    else snprintf(buf, sizeof buf, "%d:%02d:%02d %s", dt.hour % 12 ? dt.hour % 12 : 12, dt.minute, dt.second,
                  dt.hour < 12 ? "AM" : "PM");
    int ty = FACE_CY + FACE_R + 12;
    gfx_text_aligned(s, rect_make(0, ty, W, 36), buf, t->window_text, FONT_UI_TITLE, ALIGN_CENTER | ALIGN_MIDDLE);
    snprintf(buf, sizeof buf, "%s, %d %s %d", sys_weekday_name(dt.weekday), dt.day, sys_month_name(dt.month), dt.year);
    gfx_text_aligned(s, rect_make(0, ty + 40, W, 20), buf, muted(t), FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
}

/* ---- stopwatch -------------------------------------------------------------------- */
static uint32_t sw_elapsed(clk_t *c) { return c->sw_running ? c->sw_accum + (now_ms() - c->sw_start_ms) : c->sw_accum; }

static int lap_rows_visible(clk_t *c) { return (win_height(c->win) - LAP_TOP - 26) / LAP_ROW_H; }

static void paint_stopwatch(clk_t *c, surface_t *s) {
    const theme_t *t = theme_get();
    int W = s->w;
    double cx = W / 2.0, cy = 190, r = 118;
    uint32_t el = sw_elapsed(c);
    double secf = (el % 60000) / 1000.0;
    /* 60 tick ring: passed seconds in the accent colour */
    uint32_t off = color_mix(t->window_bg, t->window_text, 50);
    for (int i = 0; i < 60; i++) {
        double a = i * M_PI / 30, sx = sin(a), sy = -cos(a);
        bool on = (el > 0) && (i < (int)secf || (i == 0 && el >= 60000 && (int)secf == 0));
        bool major = i % 5 == 0;
        double r0 = r, r1 = r - (major ? 14 : 9);
        aa_stroke(s, cx + sx * r0, cy + sy * r0, cx + sx * r1, cy + sy * r1, major ? 3 : 2, on ? t->accent : off);
    }
    /* moving marker */
    double ma = secf * M_PI / 30;
    gfx_fill_circle_aa(s, cx + sin(ma) * (r + 9), cy - cos(ma) * (r + 9), 4.5, el ? t->accent : off);

    char buf[32];
    fmt_hms_cs(el, buf, sizeof buf);
    const font_t *f = font_text_width(FONT_UI_HUGE, buf) <= 2 * r - 30 ? FONT_UI_HUGE : FONT_UI_TITLE;
    gfx_text_aligned(s, rect_make(0, (int)cy - 40, W, 64), buf, t->window_text, f, ALIGN_CENTER | ALIGN_MIDDLE);
    const char *sub = c->sw_running ? "Running" : (el ? "Stopped" : "Ready");
    char lap[48];
    if (c->nlaps) {
        char lt[24];
        fmt_hms_cs(el - c->laps[c->nlaps - 1], lt, sizeof lt);
        snprintf(lap, sizeof lap, "Lap %d  %s", c->nlaps + 1, lt);
        sub = lap;
    }
    gfx_text_aligned(s, rect_make(0, (int)cy + 26, W, 20), sub, muted(t), FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);

    /* lap table */
    int x0 = 20, x1 = W - 20, y = LAP_TOP;
    gfx_text(s, x0 + 8, y, "Lap", muted(t), FONT_UI_BOLD);
    gfx_text_aligned(s, rect_make(x0, y, (x1 - x0) * 62 / 100, 18), "Lap time", muted(t), FONT_UI_BOLD, ALIGN_RIGHT);
    gfx_text_aligned(s, rect_make(x0, y, x1 - x0 - 8, 18), "Total", muted(t), FONT_UI_BOLD, ALIGN_RIGHT);
    gfx_hline(s, x0, y + 21, x1 - x0, color_mix(t->window_bg, t->window_border, 110));
    if (!c->nlaps) {
        gfx_text_aligned(s, rect_make(0, y + 34, W, 20), "Press Lap while running to record split times",
                         muted(t), FONT_UI, ALIGN_CENTER);
        return;
    }
    /* fastest / slowest when there are at least 3 laps */
    int fast = -1, slow = -1;
    if (c->nlaps >= 3) {
        uint32_t best = 0xFFFFFFFF, worst = 0;
        for (int i = 0; i < c->nlaps; i++) {
            uint32_t d = c->laps[i] - (i ? c->laps[i - 1] : 0);
            if (d < best) { best = d; fast = i; }
            if (d > worst) { worst = d; slow = i; }
        }
    }
    int vis = lap_rows_visible(c);
    int ry = y + 24;
    for (int k = c->lap_scroll; k < c->nlaps && k < c->lap_scroll + vis; k++) {
        int i = c->nlaps - 1 - k;   /* newest first */
        uint32_t d = c->laps[i] - (i ? c->laps[i - 1] : 0);
        uint32_t col = t->window_text;
        if (i == fast) col = t->dark ? RGB(110, 210, 130) : RGB(30, 150, 70);
        if (i == slow) col = t->dark ? RGB(255, 120, 110) : RGB(210, 60, 50);
        if (k % 2 == 0) gfx_fill_rounded_rect(s, x0, ry, x1 - x0, LAP_ROW_H, 4, color_mix(t->window_bg, t->window_text, 8));
        char a[24], b[24], n[16];
        snprintf(n, sizeof n, "Lap %d", i + 1);
        fmt_hms_cs(d, a, sizeof a);
        fmt_hms_cs(c->laps[i], b, sizeof b);
        gfx_text(s, x0 + 8, ry + 2, n, col, FONT_UI);
        gfx_text_aligned(s, rect_make(x0, ry + 2, (x1 - x0) * 62 / 100, 18), a, col, FONT_UI, ALIGN_RIGHT);
        gfx_text_aligned(s, rect_make(x0, ry + 2, x1 - x0 - 8, 18), b, muted(t), FONT_UI, ALIGN_RIGHT);
        ry += LAP_ROW_H;
    }
}

static void sw_update_buttons(clk_t *c) {
    ui_set_text(c->sw_start, c->sw_running ? "Stop" : (sw_elapsed(c) ? "Resume" : "Start"));
    ui_set_enabled(c->sw_lap, c->sw_running && c->nlaps < MAX_LAPS);
    ui_set_enabled(c->sw_reset, !c->sw_running && sw_elapsed(c) > 0);
}

static void sw_toggle(clk_t *c) {
    if (c->sw_running) { c->sw_accum = sw_elapsed(c); c->sw_running = false; }
    else { c->sw_start_ms = now_ms(); c->sw_running = true; }
    sw_update_buttons(c);
    win_invalidate(c->win);
}

static void sw_lap(clk_t *c) {
    if (!c->sw_running || c->nlaps >= MAX_LAPS) return;
    c->laps[c->nlaps++] = sw_elapsed(c);
    c->lap_scroll = 0;
    sw_update_buttons(c);
    win_invalidate(c->win);
}

static void sw_reset(clk_t *c) {
    if (c->sw_running) return;
    c->sw_accum = 0;
    c->nlaps = 0;
    c->lap_scroll = 0;
    sw_update_buttons(c);
    win_invalidate(c->win);
}

static void on_sw_start(widget_t *w, void *ctx) { (void)w; sw_toggle(ctx); }
static void on_sw_lap(widget_t *w, void *ctx) { (void)w; sw_lap(ctx); }
static void on_sw_reset(widget_t *w, void *ctx) { (void)w; sw_reset(ctx); }

/* ---- countdown timer -------------------------------------------------------------- */
static const note_t alarm_notes[] = {
    { 1760, 110 }, { 0, 70 }, { 1760, 110 }, { 0, 70 }, { 1760, 110 }, { 0, 70 }, { 1760, 110 }, { 0, 10 },
};

/* field geometry in the idle (setting) view */
#define FIELD_W 96
#define COLON_W 24
#define FIELD_Y 140
#define FIELD_H 72

static rect_t field_rect(clk_t *c, int i) {
    int total = 3 * FIELD_W + 2 * COLON_W;
    int x0 = (win_width(c->win) - total) / 2;
    return rect_make(x0 + i * (FIELD_W + COLON_W), FIELD_Y, FIELD_W, FIELD_H);
}
static rect_t chevron_rect(clk_t *c, int i, bool up) {
    rect_t f = field_rect(c, i);
    return up ? rect_make(f.x + 18, f.y - 36, f.w - 36, 30) : rect_make(f.x + 18, f.y + f.h + 6, f.w - 36, 30);
}

static uint32_t t_remaining(clk_t *c) {
    if (c->t_state == T_RUNNING) {
        int32_t d = (int32_t)(c->t_end - now_ms());
        return d > 0 ? (uint32_t)d : 0;
    }
    if (c->t_state == T_PAUSED) return c->t_remain;
    return 0;
}

static void t_update_buttons(clk_t *c) {
    bool idle = c->t_state == T_IDLE;
    for (int i = 0; i < 6; i++) ui_set_visible(c->t_preset[i], c->tab == TAB_TIMER && idle);
    const char *lbl = "Start";
    if (c->t_state == T_RUNNING) lbl = "Pause";
    else if (c->t_state == T_PAUSED) lbl = "Resume";
    else if (c->t_state == T_RINGING) lbl = "Dismiss";
    ui_set_text(c->t_start, lbl);
    uint32_t set = (uint32_t)(c->t_val[0] * 3600 + c->t_val[1] * 60 + c->t_val[2]);
    ui_set_enabled(c->t_start, !idle || set > 0);
    ui_set_enabled(c->t_cancel, !idle || set > 0);
    ui_set_text(c->t_cancel, idle ? "Clear" : "Cancel");
}

static void t_stop_alarm(clk_t *c) {
    if (c->t_state == T_RINGING) speaker_off();
}

static void t_start_pause(clk_t *c) {
    uint32_t now = now_ms();
    switch (c->t_state) {
    case T_IDLE: {
        uint32_t secs = (uint32_t)(c->t_val[0] * 3600 + c->t_val[1] * 60 + c->t_val[2]);
        if (!secs) { sys_beep(330, 60); return; }
        c->t_total = secs * 1000;
        c->t_end = now + c->t_total;
        c->t_state = T_RUNNING;
        break;
    }
    case T_RUNNING:
        c->t_remain = t_remaining(c);
        c->t_state = T_PAUSED;
        break;
    case T_PAUSED:
        c->t_end = now + c->t_remain;
        c->t_state = T_RUNNING;
        break;
    case T_RINGING:
        t_stop_alarm(c);
        c->t_state = T_IDLE;
        break;
    }
    t_update_buttons(c);
    win_invalidate(c->win);
}

static void t_cancel(clk_t *c) {
    if (c->t_state == T_IDLE) { c->t_val[0] = c->t_val[1] = c->t_val[2] = 0; }
    t_stop_alarm(c);
    c->t_state = T_IDLE;
    t_update_buttons(c);
    win_invalidate(c->win);
}

static void t_adjust(clk_t *c, int field, int delta) {
    if (c->t_state != T_IDLE) return;
    int max = field == 0 ? 24 : 60;
    c->t_val[field] = ((c->t_val[field] + delta) % max + max) % max;
    t_update_buttons(c);
    win_invalidate(c->win);
}

static void on_t_start(widget_t *w, void *ctx) { (void)w; t_start_pause(ctx); }
static void on_t_cancel(widget_t *w, void *ctx) { (void)w; t_cancel(ctx); }
static void on_t_preset(widget_t *w, void *ctx) {
    clk_t *c = ctx;
    int m = (int)ui_get_tag(w);
    c->t_val[0] = m / 60;
    c->t_val[1] = m % 60;
    c->t_val[2] = 0;
    t_update_buttons(c);
    win_invalidate(c->win);
}

static void draw_chevron(surface_t *s, rect_t r, bool up, uint32_t col) {
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    double d = up ? -1 : 1;
    aa_stroke(s, cx - 8, cy - d * 4, cx, cy + d * 4, 2.2, col);
    aa_stroke(s, cx, cy + d * 4, cx + 8, cy - d * 4, 2.2, col);
}

static void paint_timer(clk_t *c, surface_t *s) {
    const theme_t *t = theme_get();
    int W = s->w;
    char buf[48];
    if (c->t_state == T_IDLE) {
        static const char *names[3] = { "hours", "minutes", "seconds" };
        gfx_text_aligned(s, rect_make(0, 62, W, 20), "Set a countdown", muted(t), FONT_UI, ALIGN_CENTER);
        for (int i = 0; i < 3; i++) {
            rect_t f = field_rect(c, i);
            bool sel = i == c->t_field;
            gfx_fill_rounded_rect(s, f.x, f.y, f.w, f.h, 10,
                                  sel ? color_mix(t->window_bg, t->accent, t->dark ? 60 : 36)
                                      : color_mix(t->window_bg, t->window_text, 10));
            if (sel) gfx_rounded_rect(s, f.x, f.y, f.w, f.h, 10, color_alpha(t->accent, 200));
            snprintf(buf, sizeof buf, "%02d", c->t_val[i]);
            gfx_text_aligned(s, f, buf, t->window_text, FONT_UI_HUGE, ALIGN_CENTER | ALIGN_MIDDLE);
            draw_chevron(s, chevron_rect(c, i, true), true, t->window_text);
            draw_chevron(s, chevron_rect(c, i, false), false, t->window_text);
            gfx_text_aligned(s, rect_make(f.x, f.y + f.h + 40, f.w, 18), names[i], muted(t), FONT_UI, ALIGN_CENTER);
            if (i < 2)
                gfx_text_aligned(s, rect_make(f.x + f.w, f.y, COLON_W, f.h - 6), ":", muted(t), FONT_UI_HUGE,
                                 ALIGN_CENTER | ALIGN_MIDDLE);
        }
        gfx_text_aligned(s, rect_make(0, 346, W, 18), "Arrow keys or the mouse wheel change the value",
                         muted(t), FONT_UI, ALIGN_CENTER);
        return;
    }
    double cx = W / 2.0, cy = 210, r = 128;
    uint32_t rem = t_remaining(c);
    double frac = c->t_total ? (double)rem / c->t_total : 0;
    uint32_t track = color_mix(t->window_bg, t->window_text, 26);
    gfx_circle_aa(s, cx, cy, r, 12, track);
    uint32_t col = t->accent;
    if (c->t_state == T_RINGING) {
        bool on = ((now_ms() - c->t_ring_start) / 500) % 2 == 0;
        col = on ? RGB(240, 84, 60) : color_mix(RGB(240, 84, 60), t->window_bg, 120);
        gfx_circle_aa(s, cx, cy, r, 12, col);
        gfx_text_aligned(s, rect_make(0, (int)cy - 44, W, 64), "00:00", t->window_text, FONT_UI_HUGE, ALIGN_CENTER | ALIGN_MIDDLE);
        gfx_text_aligned(s, rect_make(0, (int)cy + 22, W, 24), "Time's up!", col, FONT_UI_LARGE, ALIGN_CENTER | ALIGN_MIDDLE);
        return;
    }
    if (c->t_state == T_PAUSED) col = color_mix(t->accent, t->window_bg, 110);
    if (frac > 0) draw_arc(s, cx, cy, r, 12, 0, frac * 2 * M_PI, col);
    fmt_remain(rem, buf, sizeof buf);
    const font_t *f = font_text_width(FONT_UI_HUGE, buf) <= 2 * r - 40 ? FONT_UI_HUGE : FONT_UI_TITLE;
    gfx_text_aligned(s, rect_make(0, (int)cy - 44, W, 64), buf, t->window_text, f, ALIGN_CENTER | ALIGN_MIDDLE);
    if (c->t_state == T_PAUSED) {
        strcpy(buf, "Paused");
    } else {
        datetime_t dt;
        sys_time_to_datetime(sys_time() + (rem + 999) / 1000, &dt);
        snprintf(buf, sizeof buf, "Ends at %02d:%02d:%02d", dt.hour, dt.minute, dt.second);
    }
    gfx_text_aligned(s, rect_make(0, (int)cy + 22, W, 20), buf, muted(t), FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
    char tot[32];
    fmt_remain(c->t_total, tot, sizeof tot);
    snprintf(buf, sizeof buf, "of %s", tot);
    gfx_text_aligned(s, rect_make(0, (int)cy + 42, W, 20), buf, muted(t), FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
}

static void t_check(clk_t *c) {
    uint32_t now = now_ms();
    if (c->t_state == T_RUNNING && t_remaining(c) == 0) {
        c->t_state = T_RINGING;
        c->t_ring_start = now;
        c->t_next_beep = now;
        ui_notify("Timer", "Time's up!");
        if (c->tab != TAB_TIMER) {
            ui_set_value(c->tabs, TAB_TIMER);
            c->tab = TAB_TIMER;
        }
        t_update_buttons(c);
        win_invalidate(c->win);
    }
    if (c->t_state == T_RINGING) {
        if ((int32_t)(now - c->t_next_beep) >= 0 && now - c->t_ring_start < 30000) {
            sys_play_notes(alarm_notes, ARRAY_SIZE(alarm_notes));
            c->t_next_beep = now + 1500;
        }
    }
}

/* ---- tabs + events ---------------------------------------------------------------- */
static void show_tab(clk_t *c, int tab) {
    c->tab = tab;
    ui_set_visible(c->chk24, tab == TAB_CLOCK);
    ui_set_visible(c->sw_start, tab == TAB_STOPWATCH);
    ui_set_visible(c->sw_lap, tab == TAB_STOPWATCH);
    ui_set_visible(c->sw_reset, tab == TAB_STOPWATCH);
    ui_set_visible(c->t_start, tab == TAB_TIMER);
    ui_set_visible(c->t_cancel, tab == TAB_TIMER);
    t_update_buttons(c);
    sw_update_buttons(c);
    win_invalidate(c->win);
}

static void on_tab(widget_t *w, void *ctx) { show_tab(ctx, ui_get_value(w)); }
static void on_24h(widget_t *w, void *ctx) {
    clk_t *c = ctx;
    c->h24 = ui_get_value(w);
    win_invalidate(c->win);
}

static void clock_tick(clk_t *c) {
    uint32_t now = now_ms();
    uint32_t t = sys_time();
    if (t != c->last_time) { c->last_time = t; c->phase_ms = now; }
    t_check(c);
    if (win_is_minimized(c->win)) return;
    int W = win_width(c->win);
    switch (c->tab) {
    case TAB_CLOCK:
        win_invalidate_rect(c->win, W / 2 - FACE_R - 8, FACE_CY - FACE_R - 8, 2 * FACE_R + 16, 2 * FACE_R + 16);
        if ((int)(t % 60) != c->last_sec_drawn) {
            c->last_sec_drawn = (int)(t % 60);
            win_invalidate_rect(c->win, 0, FACE_CY + FACE_R + 10, W, 66);
        }
        break;
    case TAB_STOPWATCH:
        if (c->sw_running) win_invalidate_rect(c->win, 0, 60, W, 260);
        break;
    case TAB_TIMER:
        if (c->t_state == T_RUNNING || c->t_state == T_RINGING) {
            if (now - c->t_last_draw >= 100) {
                c->t_last_draw = now;
                win_invalidate_rect(c->win, 0, 60, W, 300);
            }
        }
        break;
    }
}

static int field_at(clk_t *c, int x, int y) {
    for (int i = 0; i < 3; i++) {
        rect_t f = field_rect(c, i);
        rect_t big = rect_make(f.x, f.y - 36, f.w, f.h + 72);
        if (rect_contains(big, x, y)) return i;
    }
    return -1;
}

static void clock_event(window_t *win, const event_t *ev) {
    clk_t *c = win_get_userdata(win);
    if (!c) return;
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        if (c->tab == TAB_CLOCK) paint_clock(c, s);
        else if (c->tab == TAB_STOPWATCH) paint_stopwatch(c, s);
        else paint_timer(c, s);
        break;
    }
    case EV_TIMER:
        clock_tick(c);
        break;
    case EV_THEME:
        render_face(c);
        break;
    case EV_MOUSE_DOWN:
        if (ev->button != MOUSE_LEFT) break;
        if (c->tab == TAB_TIMER && c->t_state == T_IDLE) {
            for (int i = 0; i < 3; i++) {
                if (rect_contains(chevron_rect(c, i, true), ev->x, ev->y)) { c->t_field = i; t_adjust(c, i, 1); return; }
                if (rect_contains(chevron_rect(c, i, false), ev->x, ev->y)) { c->t_field = i; t_adjust(c, i, -1); return; }
            }
            int f = field_at(c, ev->x, ev->y);
            if (f >= 0) { c->t_field = f; win_invalidate(win); }
        } else if (c->tab == TAB_CLOCK && ev->y > FACE_CY + FACE_R + 8 && ev->y < FACE_CY + FACE_R + 52) {
            c->h24 = !c->h24;   /* clicking the digital time toggles 12/24 h */
            ui_set_value(c->chk24, c->h24);
            win_invalidate(win);
        }
        break;
    case EV_MOUSE_WHEEL:
        if (c->tab == TAB_TIMER && c->t_state == T_IDLE) {
            int f = field_at(c, ev->x, ev->y);
            if (f >= 0) { c->t_field = f; t_adjust(c, f, -ev->wheel); }
        } else if (c->tab == TAB_STOPWATCH && ev->y >= LAP_TOP) {
            int maxs = MAX(0, c->nlaps - lap_rows_visible(c));
            c->lap_scroll = CLAMP(c->lap_scroll + ev->wheel, 0, maxs);
            win_invalidate(win);
        }
        break;
    case EV_KEY_DOWN: {
        int k = ev->key;
        if (ev->mods & (MOD_CTRL | MOD_ALT)) {
            if ((ev->mods & MOD_CTRL) && k >= '1' && k <= '3') { ui_set_value(c->tabs, k - '1'); show_tab(c, k - '1'); }
            break;
        }
        if (c->tab == TAB_STOPWATCH) {
            if (k == KEY_SPACE || k == 's') sw_toggle(c);
            else if (k == 'l') sw_lap(c);
            else if (k == 'r') sw_reset(c);
        } else if (c->tab == TAB_TIMER) {
            if (c->t_state == T_IDLE) {
                if (k == KEY_UP) t_adjust(c, c->t_field, 1);
                else if (k == KEY_DOWN) t_adjust(c, c->t_field, -1);
                else if (k == KEY_LEFT) { c->t_field = (c->t_field + 2) % 3; win_invalidate(win); }
                else if (k == KEY_RIGHT) { c->t_field = (c->t_field + 1) % 3; win_invalidate(win); }
                else if (k == KEY_SPACE) t_start_pause(c);
            } else if (k == KEY_SPACE) {
                t_start_pause(c);
            } else if (k == KEY_ESCAPE) {
                t_cancel(c);
            }
        }
        break;
    }
    case EV_CLOSE:
        t_stop_alarm(c);
        if (c->face) surface_destroy(c->face);
        win_set_userdata(win, NULL);
        free(c);
        break;
    default:
        break;
    }
}

static void clock_launch(const char *arg) {
    clk_t *c = calloc(1, sizeof *c);
    if (!c) return;
    window_t *win = win_create("Clock", CW, CH, WIN_NO_MAXIMIZE);
    if (!win) { free(c); return; }
    c->win = win;
    c->h24 = true;
    c->last_sec_drawn = -1;
    c->t_val[1] = 5;            /* default countdown: 5 minutes */
    c->t_field = 1;
    win_set_userdata(win, c);
    win_set_event_handler(win, clock_event);

    c->tabs = ui_tabs(win, 12, 8, CW - 24, 34, on_tab, c);
    ui_list_add(c->tabs, "Clock");
    ui_list_add(c->tabs, "Stopwatch");
    ui_list_add(c->tabs, "Timer");

    c->chk24 = ui_checkbox(win, (CW - 130) / 2, CH - 40, 130, 24, "24-hour clock", true, on_24h, c);

    int bw = 96, by = 324, bx = (CW - (3 * bw + 2 * 16)) / 2;
    c->sw_reset = ui_button(win, bx, by, bw, 32, "Reset", on_sw_reset, c);
    c->sw_lap = ui_button(win, bx + bw + 16, by, bw, 32, "Lap", on_sw_lap, c);
    c->sw_start = ui_button(win, bx + 2 * (bw + 16), by, bw, 32, "Start", on_sw_start, c);
    ui_button_set_default(c->sw_start, true);

    int pw = 52, pg = 6, px = (CW - (6 * pw + 5 * pg)) / 2;
    for (int i = 0; i < 6; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "%d min", presets_min[i]);
        c->t_preset[i] = ui_button(win, px + i * (pw + pg), 300, pw, 30, lbl, on_t_preset, c);
        ui_set_tag(c->t_preset[i], presets_min[i]);
    }
    int tb = 120, tx = (CW - (2 * tb + 16)) / 2;
    c->t_cancel = ui_button(win, tx, CH - 56, tb, 34, "Cancel", on_t_cancel, c);
    c->t_start = ui_button(win, tx + tb + 16, CH - 56, tb, 34, "Start", on_t_start, c);
    ui_button_set_default(c->t_start, true);

    int tab = TAB_CLOCK;
    if (arg && !strcasecmp(arg, "stopwatch")) tab = TAB_STOPWATCH;
    else if (arg && !strcasecmp(arg, "timer")) tab = TAB_TIMER;
    ui_set_value(c->tabs, tab);
    show_tab(c, tab);
    c->timer = win_set_timer(win, 40);
}

/* Icon: a round wall clock at 10:10 with a red second hand. */
static void clock_icon(surface_t *s, int x, int y, int size) {
    double cx = x + size / 2.0, cy = y + size / 2.0, R = size * 0.44;
    gfx_fill_circle_aa(s, cx, cy + size * 0.02, R, RGBA(0, 0, 0, 60));
    gfx_fill_circle_aa(s, cx, cy, R, RGB(60, 120, 230));
    gfx_fill_circle_aa(s, cx, cy, R * 0.84, RGB(250, 251, 253));
    for (int i = 0; i < 12; i++) {
        double a = i * M_PI / 6, r0 = R * 0.74, r1 = R * (i % 3 == 0 ? 0.56 : 0.64);
        aa_stroke(s, cx + sin(a) * r0, cy - cos(a) * r0, cx + sin(a) * r1, cy - cos(a) * r1,
                  size * (i % 3 == 0 ? 0.05 : 0.03), RGB(70, 76, 90));
    }
    uint32_t ink = RGB(36, 40, 50);
    draw_hand(s, cx, cy, 10.17 * M_PI / 6, R * 0.45, R * 0.08, size * 0.075, size * 0.05, ink);
    draw_hand(s, cx, cy, 2 * M_PI * 10 / 60, R * 0.66, R * 0.08, size * 0.06, size * 0.035, ink);
    aa_stroke(s, cx, cy + R * 0.15, cx, cy - R * 0.70, size * 0.025, RGB(240, 84, 60));
    gfx_fill_circle_aa(s, cx, cy, size * 0.05, RGB(240, 84, 60));
}

REGISTER_APP(.name = "Clock", .category = CAT_ACCESSORIES,
             .description = "Analog clock, stopwatch and countdown timer",
             .launch = clock_launch, .draw_icon = clock_icon);
