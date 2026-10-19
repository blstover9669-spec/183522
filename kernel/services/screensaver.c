/*
 * screensaver.c - the NovaOS starfield + aurora screensaver.
 *
 * After `timeout` seconds without keyboard/mouse input a full-screen window
 * shows stars flying out of an aurora-lit night sky with a slowly drifting
 * clock. Any key, click or real mouse movement dismisses it; that input is
 * swallowed (through wm_input_filter) so it never reaches the windows below -
 * including the release of the key or button that woke it up.
 */
#include "services/svc_internal.h"

#define NSTARS     260
#define FRAME_MS   50
#define WAKE_MOVE  12          /* pixels of mouse travel that dismiss it */

typedef struct { double x, y, z; } star_t;

static int timeout_s = 300;
static bool timeout_from_cmdline;
static bool running, active;
static window_t *ss_win;
static int anim_timer;
static star_t stars[NSTARS];
static surface_t *sky;         /* pre-rendered background gradient */
static uint32_t t0, last_frame;
static int move_acc;
static bool eat_mouse;
static uint16_t eat_keys[8];
static int clock_x, clock_y;
static uint32_t clock_moved;

/* ---- settings ---------------------------------------------------------------------- */
int screensaver_get_timeout(void) { return timeout_s; }

void screensaver_set_timeout(int seconds) {
    timeout_s = MAX(0, seconds);
    char buf[48];
    snprintf(buf, sizeof buf, "timeout=%d\n", timeout_s);
    config_write("screensaver", buf);
    log_info("screensaver", "timeout set to %d s%s", timeout_s, timeout_s ? "" : " (disabled)");
}

static void load_settings(void) {
    const char *cl = sys_cmdline_get("screensaver");
    if (cl && *cl) { timeout_s = atoi(cl); timeout_from_cmdline = true; return; }
    char *text = config_read("screensaver");
    if (!text) return;
    char *cur = text, *line;
    while ((line = config_next_line(&cur)))
        if (!strncmp(line, "timeout=", 8)) timeout_s = MAX(0, atoi(line + 8));
    free(text);
}

bool screensaver_active(void) { return active; }

/* ---- drawing ------------------------------------------------------------------------- */
static void reset_star(star_t *s, bool anywhere) {
    s->x = (rand() % 2000 - 1000) / 1000.0;
    s->y = (rand() % 2000 - 1000) / 1000.0;
    s->z = anywhere ? 0.05 + (rand() % 950) / 1000.0 : 1.0;
}

static void build_sky(int w, int h) {
    surface_destroy(sky);
    sky = surface_create(w, h);
    if (!sky) return;
    gfx_gradient_v(sky, 0, 0, w, h * 2 / 3, RGB(4, 6, 20), RGB(12, 16, 44));
    gfx_gradient_v(sky, 0, h * 2 / 3, w, h - h * 2 / 3, RGB(12, 16, 44), RGB(26, 14, 48));
    /* faint fixed background stars */
    for (int i = 0; i < 400; i++) {
        int x = rand() % w, y = rand() % h, b = 40 + rand() % 90;
        gfx_pixel(sky, x, y, RGB(b, b, b + 20));
    }
}

static inline uint32_t blend(uint32_t d, uint32_t c, int a) {
    int ia = 255 - a;
    return 0xFF000000u | (uint32_t)((COLOR_R(c) * a + COLOR_R(d) * ia) / 255) << 16 |
           (uint32_t)((COLOR_G(c) * a + COLOR_G(d) * ia) / 255) << 8 | (uint32_t)((COLOR_B(c) * a + COLOR_B(d) * ia) / 255);
}

/* Aurora: translucent vertical curtains whose top edge and height wave over time. */
static void draw_aurora(surface_t *s, double t) {
    int w = s->w, h = s->h;
    for (int x = 0; x < w; x += 2) {
        double fx = x;
        double top = h * 0.22 + 40 * sin(fx * 0.0042 + t * 0.21) + 18 * sin(fx * 0.011 - t * 0.37);
        double len = 150 + 60 * sin(fx * 0.006 + t * 0.29) + 30 * sin(fx * 0.017 + t * 0.5);
        double inten = 0.55 + 0.45 * sin(fx * 0.009 - t * 0.33) * sin(fx * 0.0023 + t * 0.12);
        if (inten < 0.05) continue;
        int y0 = (int)top, y1 = (int)(top + len);
        y0 = CLAMP(y0, 0, h - 1);
        y1 = CLAMP(y1, 0, h - 1);
        for (int y = y0; y < y1; y++) {
            double f = (y - top) / len;                    /* 0 at the top edge */
            double a = (f < 0.12 ? f / 0.12 : 1.0 - (f - 0.12) / 0.88);
            a = a * a * inten * 150;
            if (a < 3) continue;
            uint32_t c = color_mix(RGB(90, 255, 170), RGB(150, 90, 255), (int)(CLAMP(f * 1.3, 0.0, 1.0) * 255));
            uint32_t *row = surface_row(s, y);
            row[x] = blend(row[x], c, (int)a);
            if (x + 1 < w) row[x + 1] = blend(row[x + 1], c, (int)a);
        }
    }
}

static void draw_frame(void) {
    if (!ss_win || !win_is_valid(ss_win)) return;
    surface_t *s = win_surface(ss_win);
    if (!sky || sky->w != s->w || sky->h != s->h) build_sky(s->w, s->h);
    uint32_t now = sys_uptime_ms();
    double dt = MIN((now - last_frame) / 1000.0, 0.2);
    last_frame = now;
    double t = (now - t0) / 1000.0;
    if (sky) for (int y = 0; y < s->h; y++) memcpy(surface_row(s, y), surface_row(sky, y), (size_t)s->w * 4);
    draw_aurora(s, t);
    /* starfield */
    double cx = s->w / 2.0, cy = s->h / 2.0;
    for (int i = 0; i < NSTARS; i++) {
        star_t *st = &stars[i];
        st->z -= dt * 0.22;
        if (st->z <= 0.04) reset_star(st, false);
        double sx = cx + st->x / st->z * cx * 0.5, sy = cy + st->y / st->z * cy * 0.5;
        if (sx < -4 || sy < -4 || sx > s->w + 4 || sy > s->h + 4) { reset_star(st, false); continue; }
        double k = 1.0 - st->z;
        int b = (int)(90 + 165 * k);
        double r = 0.6 + 1.9 * k * k;
        gfx_fill_circle_aa(s, sx, sy, r, RGB(b, b, MIN(255, b + 25)));
        if (k > 0.7) gfx_fill_circle_aa(s, sx, sy, r * 2.6, RGBA(160, 190, 255, (int)(40 * k)));
    }
    /* drifting clock, moved every 12 s to avoid burn-in */
    if (!clock_moved || now - clock_moved > 12000) {
        clock_x = 80 + rand() % MAX(1, s->w - 460);
        clock_y = 80 + rand() % MAX(1, s->h - 300);
        clock_moved = now;
    }
    int fade = (int)MIN(255u, (now - clock_moved) * 255 / 900);
    uint32_t left = 12000 - MIN(12000u, now - clock_moved);
    if (left < 900) fade = (int)(left * 255 / 900);
    datetime_t d;
    sys_datetime(&d);
    char tbuf[16], dbuf[48];
    snprintf(tbuf, sizeof tbuf, "%02d:%02d", d.hour, d.minute);
    snprintf(dbuf, sizeof dbuf, "%s, %d %s", sys_weekday_name(d.weekday), d.day, sys_month_name(d.month));
    gfx_text(s, clock_x, clock_y, tbuf, RGBA(240, 244, 255, fade), FONT_UI_HUGE);
    gfx_text(s, clock_x + 4, clock_y + 70, dbuf, RGBA(190, 200, 235, fade * 3 / 4), FONT_UI_LARGE);
    win_update(ss_win, 0, 0, 0, 0);
    /* toasts or popups created meanwhile must not cover the screensaver */
    if (ss_win->above) wm_raise(ss_win);
}

static void anim_cb(void *ctx) { (void)ctx; draw_frame(); }

static void ss_event(window_t *win, const event_t *ev) {
    switch (ev->type) {
    case EV_PAINT: draw_frame(); break;
    case EV_CLOSE:
        if (win == ss_win) ss_win = NULL;
        break;
    default: break;
    }
}

/* ---- activation ------------------------------------------------------------------------ */
void screensaver_activate(void) {
    if (!running || active || !screen_width()) return;
    wm_close_popups(NULL);
    const app_desc_t *saved = wm_current_app;
    wm_current_app = NULL;
    ss_win = wm_create_ex("Screensaver", screen_width(), screen_height(),
                          WIN_FRAMELESS | WIN_NO_TASKBAR | WIN_NO_SHADOW, LAYER_POPUP);
    wm_current_app = saved;
    if (!ss_win) return;
    win_move(ss_win, 0, 0);
    win_set_cursor(ss_win, CURSOR_NONE);
    win_set_event_handler(ss_win, ss_event);
    active = true;
    move_acc = 0;
    t0 = last_frame = sys_uptime_ms();
    clock_moved = 0;
    for (int i = 0; i < NSTARS; i++) reset_star(&stars[i], true);
    anim_timer = timer_add(FRAME_MS, anim_cb, NULL, true);
    /* hide the pointer even though the WM thinks it is over another window */
    wm_cursor_shape = CURSOR_NONE;
    wm_invalidate_all();
    log_info("screensaver", "activated after %u s idle", (g_ticks - wm_last_input_tick) / 1000);
}

void screensaver_dismiss(void) {
    if (!active) return;
    active = false;
    if (anim_timer) { timer_cancel(anim_timer); anim_timer = 0; }
    if (ss_win && win_is_valid(ss_win)) win_close(ss_win);
    ss_win = NULL;
    wm_cursor_shape = CURSOR_ARROW;
    wm_invalidate_all();
    log_info("screensaver", "dismissed");
}

static void remember_key(uint16_t key) {
    for (int i = 0; i < 8; i++) if (eat_keys[i] == key) return;
    for (int i = 0; i < 8; i++) if (!eat_keys[i]) { eat_keys[i] = key; return; }
    eat_keys[0] = key;
}

static bool forget_key(uint16_t key, bool release) {
    for (int i = 0; i < 8; i++)
        if (eat_keys[i] == key) { if (release) eat_keys[i] = 0; return true; }
    return false;
}

/* Raw input filter (runs before the window manager sees the event). */
static bool ss_filter(const input_event_t *ev) {
    if (ev->type == IN_MOUSE) {
        if (active) {
            if (ev->buttons) { eat_mouse = true; screensaver_dismiss(); return true; }
            if (ev->dz) { screensaver_dismiss(); return true; }
            move_acc += abs(ev->dx) + abs(ev->dy);
            if (move_acc > WAKE_MOVE) screensaver_dismiss();
            return true;
        }
        if (eat_mouse) {
            if (!ev->buttons) eat_mouse = false;   /* the release itself is swallowed too */
            return true;
        }
        return false;
    }
    if (ev->type == IN_KEY) {
        if (active) {
            if (ev->pressed) { remember_key(ev->key); screensaver_dismiss(); }
            return true;
        }
        if (forget_key(ev->key, !ev->pressed)) return true;   /* release / repeats of the waking key */
    }
    return false;
}

/* ---- service ------------------------------------------------------------------------------ */
static int ss_start(void) {
    load_settings();
    running = true;
    wm_input_filter = ss_filter;
    log_info("screensaver", "timeout %d s%s", timeout_s, timeout_from_cmdline ? " (kernel command line)" : "");
    return 0;
}

static void ss_stop(void) {
    screensaver_dismiss();
    running = false;
    if (wm_input_filter == ss_filter) wm_input_filter = NULL;
    eat_mouse = false;
    memset(eat_keys, 0, sizeof eat_keys);
    surface_destroy(sky);
    sky = NULL;
}

static void ss_tick(void) {
    if (active || timeout_s <= 0) return;
    if (g_ticks - wm_last_input_tick >= (uint32_t)timeout_s * 1000u) screensaver_activate();
}

REGISTER_SERVICE(.name = "screensaver", .display_name = "Screensaver",
                 .description = "Starfield and aurora screensaver after a few idle minutes",
                 .autostart = true, .restart_on_failure = true,
                 .start = ss_start, .stop = ss_stop, .tick = ss_tick, .tick_ms = 1000,
                 .icon = ICON_COMPUTER);

static int cmd_screensaver(int argc, char **argv, cmd_writer_t *out) {
    if (argc < 2) {
        cmd_printf(out, "screensaver: %s, timeout %d s%s\n", service_is_running("screensaver") ? "enabled" : "service stopped",
                   timeout_s, timeout_s ? "" : " (never)");
        cmd_puts(out, "usage: screensaver now | off | SECONDS\n");
        return 0;
    }
    if (!strcmp(argv[1], "now")) {
        if (!running) { cmd_puts(out, "screensaver: the service is not running\n"); return 1; }
        screensaver_activate();
        return 0;
    }
    if (!strcmp(argv[1], "off")) { screensaver_set_timeout(0); return 0; }
    int s = atoi(argv[1]);
    if (s <= 0) { cmd_puts(out, "screensaver: expected a number of seconds\n"); return 2; }
    screensaver_set_timeout(s);
    cmd_printf(out, "screensaver: starts after %d s without input\n", s);
    return 0;
}
REGISTER_COMMAND("screensaver", "screensaver [now | off | SECONDS] - start it or change the idle timeout", cmd_screensaver);
