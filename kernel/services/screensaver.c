/*
 * screensaver.c - the NovaOS screensaver service.
 *
 * After `timeout` seconds without keyboard/mouse input a full-screen window
 * shows the chosen screensaver style (kernel/services/screensaver_styles.c:
 * Nova Drop, Original, Bubbles, Bouncing App Logos, Lines). Any key, click or
 * real mouse movement dismisses it; that input is swallowed (through
 * wm_input_filter) so it never reaches the windows below - including the
 * release of the key or button that woke it up.
 *
 * The timeout and the style are kept in /system/config/screensaver
 * ("timeout=N", "style=<key>").
 */
#include "services/svc_internal.h"
#include <nova/screensaver.h>

#define WAKE_MOVE  12          /* pixels of mouse travel that dismiss it */
#define MAX_RECTS  48

static int timeout_s = 300;
static int saved_timeout = 300;   /* what the config file says (timeout_s may come from the command line) */
static bool timeout_from_cmdline;
static int style = SS_STYLE_ORIGINAL;
static bool running, active;
static window_t *ss_win;
static int anim_timer;
static ss_anim_t *anim;
static int move_acc;
static bool eat_mouse;
static uint16_t eat_keys[8];

/* ---- settings ---------------------------------------------------------------------- */
int screensaver_get_timeout(void) { return timeout_s; }

static void save_settings(void) {
    char buf[80];
    snprintf(buf, sizeof buf, "timeout=%d\nstyle=%s\n", saved_timeout, screensaver_style_key(style));
    config_write("screensaver", buf);
}

void screensaver_set_timeout(int seconds) {
    timeout_s = saved_timeout = MAX(0, seconds);
    save_settings();
    log_info("screensaver", "timeout set to %d s%s", timeout_s, timeout_s ? "" : " (disabled)");
}

int screensaver_get_style(void) { return style; }

void screensaver_set_style(int s) {
    if (s < 0 || s >= SS_STYLE_COUNT || s == style) return;
    style = s;
    save_settings();
    log_info("screensaver", "style set to %s", screensaver_style_name(style));
}

static void load_settings(void) {
    char *text = config_read("screensaver");
    if (text) {
        char *cur = text, *line;
        while ((line = config_next_line(&cur))) {
            if (!strncmp(line, "timeout=", 8)) timeout_s = saved_timeout = MAX(0, atoi(line + 8));
            if (!strncmp(line, "style=", 6))
                for (int i = 0; i < SS_STYLE_COUNT; i++)
                    if (!strcmp(line + 6, screensaver_style_key(i))) style = i;
        }
        free(text);
    }
    const char *cl = sys_cmdline_get("screensaver");
    if (cl && *cl) { timeout_s = atoi(cl); timeout_from_cmdline = true; }
}

bool screensaver_active(void) { return active; }

/* ---- drawing ------------------------------------------------------------------------- */
static void draw_frame(bool full) {
    if (!ss_win || !win_is_valid(ss_win) || !anim) return;
    rect_t r[MAX_RECTS];
    int n = ss_anim_frame(anim, win_surface(ss_win), full, r, MAX_RECTS);
    for (int i = 0; i < n; i++) win_update(ss_win, r[i].x, r[i].y, r[i].w, r[i].h);
    /* toasts or popups created meanwhile must not cover the screensaver */
    if (ss_win->above) wm_raise(ss_win);
}

static void anim_cb(void *ctx) { (void)ctx; draw_frame(false); }

static void ss_event(window_t *win, const event_t *ev) {
    switch (ev->type) {
    case EV_PAINT:            /* the WM cleared (part of) the surface: draw everything */
        surface_reset_clip(win_surface(win));
        draw_frame(true);
        break;
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
    anim = ss_anim_create(style, screen_width(), screen_height(), false);
    if (!anim && style != SS_STYLE_ORIGINAL) anim = ss_anim_create(SS_STYLE_ORIGINAL, screen_width(), screen_height(), false);
    if (!anim) { win_close(ss_win); ss_win = NULL; return; }
    win_move(ss_win, 0, 0);
    win_set_cursor(ss_win, CURSOR_NONE);
    win_set_event_handler(ss_win, ss_event);
    active = true;
    move_acc = 0;
    anim_timer = timer_add((uint32_t)ss_anim_interval(anim), anim_cb, NULL, true);
    /* hide the pointer even though the WM thinks it is over another window */
    wm_cursor_shape = CURSOR_NONE;
    wm_invalidate_all();
    log_info("screensaver", "activated after %u s idle (%s)", (g_ticks - wm_last_input_tick) / 1000,
             screensaver_style_name(style));
}

void screensaver_dismiss(void) {
    if (!active) return;
    active = false;
    if (anim_timer) { timer_cancel(anim_timer); anim_timer = 0; }
    if (ss_win && win_is_valid(ss_win)) win_close(ss_win);
    ss_win = NULL;
    ss_anim_destroy(anim);
    anim = NULL;
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
    log_info("screensaver", "timeout %d s%s, style %s", timeout_s, timeout_from_cmdline ? " (kernel command line)" : "",
             screensaver_style_name(style));
    return 0;
}

static void ss_stop(void) {
    screensaver_dismiss();
    running = false;
    if (wm_input_filter == ss_filter) wm_input_filter = NULL;
    eat_mouse = false;
    memset(eat_keys, 0, sizeof eat_keys);
}

static void ss_tick(void) {
    if (active || timeout_s <= 0) return;
    if (g_ticks - wm_last_input_tick >= (uint32_t)timeout_s * 1000u) screensaver_activate();
}

REGISTER_SERVICE(.name = "screensaver", .display_name = "Screensaver",
                 .description = "Full-screen animation after a few idle minutes",
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
