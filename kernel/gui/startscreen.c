/*
 * startscreen.c - the start screen (NovaOS 2.5): every time NovaOS starts, before
 * the desktop.
 *
 * Over the desktop wallpaper, dimmed: the exact system time and the date start
 * above the screen and slide down (eased) to the top left, where the time keeps
 * running; the account name sits at the middle top with the account picture
 * under it (on ISO starts "Guest" with the classic person icon). Under the
 * picture is the Start button with an arrow - or, when the account has a
 * password, a password box with the arrow button in it. Start / Enter (with the
 * right password) opens the desktop. A wrong password shows "The password is
 * incorrect. Try again.", plays the alert sound and shakes the box.
 *
 * Order: ISO starts show it after the NovaOS Drive installation's Continue ISO
 * and the save file screen; installed drives after the splash with the loading
 * bar (and on the first start after the installation menu). It is a full-screen
 * window above all others (the bottom islands hide while it has the focus); the
 * keyboard always reaches it and the global shortcuts (Start menu, Alt+Tab,
 * Alt+F4, Win+R, Ctrl+Alt+Del) are held back, so the password can't be skipped.
 * Apps that would open at startup (autorun=, the Welcome tour) wait for it:
 * startscreen_when_done().
 *
 * Test hooks: automated starts (autorun=, testmouse=) skip it; startscreen=1 or
 * setup=1 bring it back, startscreen=0 skips it. Log: "startscreen: shown ...",
 * "startscreen: ready" (the time has arrived, it waits for input), "startscreen:
 * wrong password", "startscreen: opening the desktop".
 */
#include "wm_internal.h"
#include "bootscreen.h"
#include <nova/account.h>

enum { SS_NONE, SS_PENDING, SS_ACTIVE, SS_DONE };
enum { H_NONE, H_START, H_BOX };

#define SLIDE_MS   900       /* time and date: above the screen -> top left */
#define SHAKE_MS   420
#define ANIM_MS    15
#define IDLE_MS    100
#define BLINK_MS   530
#define MAX_WAIT   8

#define C_TEXT     RGB(246, 248, 252)
#define C_DIM      RGB(206, 212, 226)
#define C_ERR      RGB(255, 150, 150)

typedef struct { void (*fn)(void *); void *ctx; } waiter_t;

static struct {
    int state;
    window_t *win;
    surface_t *bg;                  /* the wallpaper, dimmed                    */
    int w, h;
    double k;
    uint32_t t0;                    /* slide start                               */
    bool arrived;
    int time_y;                     /* where the time block is drawn             */
    int timer, timer_ms;
    int last_sec;
    bool need_pw;
    char pw[ACCOUNT_PASS_MAX + 1];
    int pwlen;
    bool error;
    uint32_t shake_t0;
    bool shaking;
    int hover, pressed;
    waiter_t wait[MAX_WAIT];
    int nwait;
    bool (*prev_filter)(const input_event_t *);
} T;

extern bool (*wm_input_filter)(const input_event_t *ev);

static int K(double v) { return (int)lround(v * T.k); }

/* ---- when NovaOS has opened ------------------------------------------------------------ */
bool startscreen_pending(void) { return T.state == SS_PENDING || T.state == SS_ACTIVE; }

void startscreen_when_done(void (*fn)(void *ctx), void *ctx) {
    if (!fn) return;
    if (startscreen_pending() && T.nwait < MAX_WAIT) {
        T.wait[T.nwait].fn = fn;
        T.wait[T.nwait].ctx = ctx;
        T.nwait++;
        return;
    }
    timer_add(1, fn, ctx, false);
}

static void run_waiters(void) {
    for (int i = 0; i < T.nwait; i++) timer_add(1, T.wait[i].fn, T.wait[i].ctx, false);
    T.nwait = 0;
}

/* ---- who gets it ------------------------------------------------------------------------- */
int boot_flag(const char *key) {
    const char *v = sys_cmdline_get(key);
    if (!v) return -1;
    return strcmp(v, "0") && strcmp(v, "off") && strcmp(v, "no") ? 1 : 0;
}

bool boot_automated(void) { return sys_cmdline_get("autorun") || g_test_mouse; }

void startscreen_prepare(void) {
    int f = boot_flag("startscreen");
    bool want;
    const char *why;
    if (f >= 0) { want = f; why = "startscreen= on the command line"; }
    else if (boot_flag("setup") == 1) { want = true; why = "setup=1"; }
    else if (boot_automated()) { want = false; why = "automated start (autorun/testmouse)"; }
    else { want = true; why = "every start"; }
    T.state = want ? SS_PENDING : SS_DONE;
    klog("startscreen: %s (%s)", want ? "will be shown" : "skipped", why);
}

/* ---- layout -------------------------------------------------------------------------------- */
static int time_block_h(void) { return font_height(FONT_UI_HUGE) + K(4) + font_height(FONT_UI_LARGE); }
static int time_final_y(void) { return K(34); }
static int time_x(void) { return K(46); }
static rect_t time_rect(int y) {
    int w = MAX(font_text_width(FONT_UI_HUGE, "00:00:00"), font_text_width(FONT_UI_LARGE, "Wednesday, 30 September 0000"));
    return rect_make(time_x() - 4, y - 4, w + 24, time_block_h() + 12);
}
static int name_y(void) { return K(40); }
static rect_t name_rect(void) { return rect_make(0, name_y(), T.w, font_height(FONT_UI_TITLE)); }
static int pic_size(void) { return K(168); }
static rect_t pic_rect(void) {
    int s = pic_size();
    return rect_make((T.w - s) / 2, name_y() + font_height(FONT_UI_TITLE) + K(22), s, s);
}
static int ctl_h(void) { return MAX(36, K(46)); }
static int ctl_y(void) { rect_t p = pic_rect(); return p.y + p.h + K(34); }
static rect_t start_rect(void) { int w = K(184); return rect_make((T.w - w) / 2, ctl_y(), w, ctl_h()); }
static int shake_dx(void) {
    if (!T.shaking) return 0;
    uint32_t el = g_ticks - T.shake_t0;
    if (el >= SHAKE_MS) return 0;
    double t = el / (double)SHAKE_MS;
    return (int)lround(sin(t * 6 * M_PI) * K(12) * (1 - t));
}
static rect_t box_rect(void) { int w = K(330); return rect_make((T.w - w) / 2 + shake_dx(), ctl_y(), w, ctl_h()); }
static rect_t arrow_rect(void) {
    rect_t b = box_rect();
    int d = b.h - 8;
    return rect_make(b.x + b.w - d - 4, b.y + 4, d, d);
}
static rect_t err_rect(void) { rect_t b = box_rect(); return rect_make(0, b.y + b.h + K(14), T.w, font_height(FONT_UI_LARGE)); }
/* everything the controls can cover (the box shakes) */
static rect_t ctl_area(void) {
    rect_t e = err_rect();
    return rect_make(0, ctl_y() - 6, T.w, e.y + e.h + 6 - (ctl_y() - 6));
}

/* ---- drawing ------------------------------------------------------------------------------ */
static void make_background(void) {
    T.bg = surface_create(T.w, T.h);
    if (!T.bg) return;
    if (g_wallpaper && g_wallpaper->w == T.w && g_wallpaper->h == T.h) gfx_blit(T.bg, 0, 0, g_wallpaper);
    else gfx_gradient_v(T.bg, 0, 0, T.w, T.h, SPLASH_BG_TOP, SPLASH_BG_BOTTOM);
    gfx_fill_rect(T.bg, 0, 0, T.w, T.h, RGBA(4, 6, 16, 120));                 /* dimmed            */
    gfx_gradient_v(T.bg, 0, 0, T.w, T.h / 3, RGBA(0, 0, 0, 70), RGBA(0, 0, 0, 0));   /* top: the time  */
}

static void text_shadowed(surface_t *s, rect_t r, const char *t, uint32_t c, const font_t *f, int align) {
    gfx_text_aligned(s, rect_make(r.x + 1, r.y + 2, r.w, r.h), t, RGBA(0, 0, 0, 90), f, align);
    gfx_text_aligned(s, r, t, c, f, align);
}

static void draw_time(surface_t *s) {
    datetime_t dt;
    sys_datetime(&dt);
    T.last_sec = dt.second;
    char tb[16], db[48];
    snprintf(tb, sizeof tb, "%02d:%02d:%02d", dt.hour, dt.minute, dt.second);
    snprintf(db, sizeof db, "%s, %d %s %d", sys_weekday_name(dt.weekday), dt.day, sys_month_name(dt.month), dt.year);
    int x = time_x(), y = T.time_y, th = font_height(FONT_UI_HUGE);
    text_shadowed(s, rect_make(x, y, T.w, th), tb, C_TEXT, FONT_UI_HUGE, ALIGN_LEFT | ALIGN_MIDDLE);
    text_shadowed(s, rect_make(x + 3, y + th + K(4), T.w, font_height(FONT_UI_LARGE)), db, C_DIM, FONT_UI_LARGE,
                  ALIGN_LEFT | ALIGN_MIDDLE);
}

static void draw_controls(surface_t *s) {
    bool hov = T.hover == H_START, prs = hov && T.pressed == H_START;
    if (!T.need_pw) {
        rect_t r = start_rect();
        uint32_t c = prs ? RGB(37, 99, 220) : hov ? RGB(92, 156, 255) : RGB(59, 130, 246);
        gfx_shadow(s, r.x, r.y, r.w, r.h, r.h / 2, K(10), 60);
        gfx_fill_rounded_gradient_v(s, r.x, r.y, r.w, r.h, r.h / 2, color_shade(c, 18), color_shade(c, -10));
        const char *label = "Start";
        int g = MAX(14, K(18)), tw = font_text_width(FONT_UI_LARGE, label), gap = K(10);
        int x0 = r.x + (r.w - tw - gap - g) / 2;
        gfx_text_aligned(s, rect_make(x0, r.y, tw, r.h), label, COLOR_WHITE, FONT_UI_LARGE, ALIGN_LEFT | ALIGN_MIDDLE);
        ui_draw_glyph(s, UI_GLYPH_ARROW_RIGHT, x0 + tw + gap, r.y + (r.h - g) / 2, g, COLOR_WHITE);
        return;
    }
    rect_t b = box_rect(), a = arrow_rect();
    gfx_shadow(s, b.x, b.y, b.w, b.h, b.h / 2, K(10), 60);
    gfx_fill_rounded_rect(s, b.x, b.y, b.w, b.h, b.h / 2, RGBA(255, 255, 255, 40));
    gfx_rounded_rect(s, b.x, b.y, b.w, b.h, b.h / 2, T.error ? RGBA(255, 140, 140, 230) : RGBA(255, 255, 255, 150));
    int tx = b.x + K(20), cy = b.y + b.h / 2;
    if (!T.pwlen) {
        gfx_text_aligned(s, rect_make(tx, b.y, a.x - tx, b.h), "Password", RGBA(255, 255, 255, 150), FONT_UI_LARGE,
                         ALIGN_LEFT | ALIGN_MIDDLE);
    }
    double dr = MAX(3.0, K(4.2)), step = dr * 2 + K(5);
    int maxdots = (int)((a.x - tx - K(12)) / step);
    int n = MIN(T.pwlen, maxdots);
    for (int i = 0; i < n; i++) gfx_fill_circle_aa(s, tx + dr + i * step, cy, dr, C_TEXT);
    if ((g_ticks / BLINK_MS) % 2 == 0 && win_is_focused(T.win)) {
        int cx = T.pwlen ? (int)(tx + n * step) : tx;
        gfx_fill_rect(s, cx, cy - K(11), 2, K(22), C_TEXT);
    }
    uint32_t c = prs ? RGB(37, 99, 220) : hov ? RGB(92, 156, 255) : RGB(59, 130, 246);
    gfx_fill_circle_aa(s, a.x + a.w / 2.0, a.y + a.h / 2.0, a.w / 2.0, c);
    int g = a.w / 2;
    ui_draw_glyph(s, UI_GLYPH_ARROW_RIGHT, a.x + (a.w - g) / 2, a.y + (a.h - g) / 2, g, COLOR_WHITE);
    if (T.error)
        text_shadowed(s, err_rect(), "The password is incorrect. Try again.", C_ERR, FONT_UI_LARGE,
                      ALIGN_CENTER | ALIGN_MIDDLE);
}

static void paint(surface_t *s) {
    if (T.bg) gfx_blit(s, 0, 0, T.bg);
    else gfx_gradient_v(s, 0, 0, T.w, T.h, SPLASH_BG_TOP, SPLASH_BG_BOTTOM);
    draw_time(s);
    text_shadowed(s, name_rect(), account_name(), C_TEXT, FONT_UI_TITLE, ALIGN_CENTER | ALIGN_MIDDLE);
    rect_t p = pic_rect();
    gfx_fill_circle_aa(s, p.x + p.w / 2.0, p.y + p.h / 2.0 + K(4), p.w / 2.0 + K(6), RGBA(0, 0, 0, 50));
    gfx_fill_circle_aa(s, p.x + p.w / 2.0, p.y + p.h / 2.0, p.w / 2.0 + K(4), RGBA(255, 255, 255, 70));
    account_draw_picture(s, p.x, p.y, p.w);
    draw_controls(s);
}

static void invalidate(rect_t r) { win_invalidate_rect(T.win, r.x, r.y, r.w, r.h); }

/* ---- the timer: slide, shake, clock, caret --------------------------------------------- */
static void set_timer(int ms) {
    if (T.timer_ms == ms) return;
    if (T.timer) win_kill_timer(T.win, T.timer);
    T.timer = win_set_timer(T.win, (uint32_t)ms);
    T.timer_ms = ms;
}

static bool my_filter(const input_event_t *ev);

static void tick(void) {
    bool busy = false;
    if (!T.arrived) {
        uint32_t el = g_ticks - T.t0;
        double t = el >= SLIDE_MS ? 1.0 : (double)el / SLIDE_MS;
        double e = 1.0 - pow(1.0 - t, 3);
        int y0 = -time_block_h() - K(16), y = (int)lround(y0 + (time_final_y() - y0) * e);
        if (y != T.time_y) {
            invalidate(time_rect(T.time_y));
            T.time_y = y;
            invalidate(time_rect(y));
        }
        if (t >= 1.0) {
            T.arrived = true;
            klog("startscreen: ready (%s)", T.need_pw ? "password box" : "Start button");
        } else {
            busy = true;
        }
    } else {
        datetime_t dt;
        sys_datetime(&dt);
        if (dt.second != T.last_sec) invalidate(time_rect(T.time_y));
    }
    if (T.shaking) {
        invalidate(ctl_area());
        if (g_ticks - T.shake_t0 >= SHAKE_MS) T.shaking = false;
        else busy = true;
    }
    if (T.need_pw) {
        static uint32_t last_blink;
        uint32_t b = g_ticks / BLINK_MS;
        if (b != last_blink) { last_blink = b; invalidate(box_rect()); }
    }
    /* keep the keyboard filter (the screensaver service may have set its own) and the focus */
    if (wm_input_filter != my_filter) { T.prev_filter = wm_input_filter; wm_input_filter = my_filter; }
    if (wm_focused != T.win && !wm_any_popup()) wm_focus(T.win);
    set_timer(busy ? ANIM_MS : IDLE_MS);
}

/* ---- leaving ---------------------------------------------------------------------------------- */
static void open_desktop(void) {
    if (T.state != SS_ACTIVE) return;
    klog("startscreen: opening the desktop");
    T.state = SS_DONE;
    if (wm_input_filter == my_filter) wm_input_filter = T.prev_filter;
    memset(T.pw, 0, sizeof T.pw);
    T.pwlen = 0;
    window_t *w = T.win;
    T.win = NULL;
    if (win_is_valid(w)) win_close(w);
    surface_destroy(T.bg);
    T.bg = NULL;
    run_waiters();
}

static void submit(void) {
    if (!T.need_pw) { open_desktop(); return; }
    T.pw[T.pwlen] = 0;
    if (account_check_password(T.pw)) { open_desktop(); return; }
    klog("startscreen: wrong password");
    sys_alert_sound();
    memset(T.pw, 0, sizeof T.pw);
    T.pwlen = 0;
    T.error = true;
    T.shaking = true;
    T.shake_t0 = g_ticks;
    invalidate(ctl_area());
    set_timer(ANIM_MS);
}

/* Held back while the start screen is up: Start menu (Win, Ctrl+Esc), Alt+Tab, Alt+F4,
   Win+R, Ctrl+Alt+Del. Every other key goes to the start screen. */
static bool my_filter(const input_event_t *ev) {
    if (T.prev_filter && T.prev_filter(ev)) return true;
    if (T.state != SS_ACTIVE || ev->type != IN_KEY) return false;
    int m = ev->mods, k = ev->key;
    if (k == KEY_LWIN || k == KEY_RWIN || (m & MOD_WIN)) return true;
    if ((m & MOD_ALT) && (k == KEY_TAB || k == KEY_F4)) return true;
    if ((m & MOD_CTRL) && (k == KEY_ESCAPE || ((m & MOD_ALT) && k == KEY_DELETE))) return true;
    if (win_is_valid(T.win) && wm_focused != T.win && !wm_any_popup()) wm_focus(T.win);
    return false;
}

static int hit(int x, int y) {
    if (T.need_pw) {
        if (rect_contains(arrow_rect(), x, y)) return H_START;
        if (rect_contains(box_rect(), x, y)) return H_BOX;
        return H_NONE;
    }
    return rect_contains(start_rect(), x, y) ? H_START : H_NONE;
}

static void handler(window_t *win, const event_t *ev) {
    if (win != T.win) return;
    switch (ev->type) {
    case EV_PAINT:
        paint(win_surface(win));
        break;
    case EV_TIMER:
        tick();
        break;
    case EV_MOUSE_MOVE: {
        int h = hit(ev->x, ev->y);
        if (h != T.hover) { T.hover = h; invalidate(ctl_area()); }
        win_set_cursor(win, h == H_START ? CURSOR_HAND : h == H_BOX ? CURSOR_IBEAM : CURSOR_ARROW);
        break;
    }
    case EV_MOUSE_DOWN:
        if (ev->button != MOUSE_LEFT) break;
        T.pressed = hit(ev->x, ev->y);
        invalidate(ctl_area());
        break;
    case EV_MOUSE_UP: {
        if (ev->button != MOUSE_LEFT) break;
        int p = T.pressed;
        T.pressed = H_NONE;
        invalidate(ctl_area());
        if (p == H_START && hit(ev->x, ev->y) == H_START) submit();
        break;
    }
    case EV_KEY_DOWN:
        if (ev->key == KEY_ENTER) { submit(); break; }
        if (!T.need_pw) break;
        if (ev->key == KEY_BACKSPACE && T.pwlen) { T.pw[--T.pwlen] = 0; invalidate(ctl_area()); }
        else if (ev->key == KEY_ESCAPE && T.pwlen) { memset(T.pw, 0, sizeof T.pw); T.pwlen = 0; invalidate(ctl_area()); }
        break;
    case EV_CHAR:
        if (!T.need_pw || ev->ch < 32 || ev->ch > 126 || T.pwlen >= ACCOUNT_PASS_MAX) break;
        T.pw[T.pwlen++] = (char)ev->ch;
        T.pw[T.pwlen] = 0;
        if (T.error) T.error = false;
        invalidate(ctl_area());
        break;
    case EV_CLOSE_REQUEST:              /* Alt+F4 does not skip it */
        break;
    case EV_CLOSE:
        if (T.win == win) {             /* closed from outside: the desktop opens */
            T.win = NULL;
            if (T.state == SS_ACTIVE) {
                T.state = SS_DONE;
                if (wm_input_filter == my_filter) wm_input_filter = T.prev_filter;
                run_waiters();
            }
        }
        break;
    default:
        break;
    }
}

/* ---- showing it --------------------------------------------------------------------------------- */
void startscreen_show(void) {
    if (T.state != SS_PENDING || !wm_backbuf) {
        if (T.state == SS_PENDING) { T.state = SS_DONE; run_waiters(); }
        return;
    }
    T.w = screen_width();
    T.h = screen_height();
    T.k = CLAMP(T.h / 768.0, 0.72, 1.6);
    window_t *w = wm_create_ex("Start", T.w, T.h, WIN_FRAMELESS | WIN_NO_TASKBAR | WIN_NO_SHADOW | WIN_CONFIRM_CLOSE,
                               LAYER_TOPMOST);
    if (!w) {
        klog("startscreen: no window - opening the desktop");
        T.state = SS_DONE;
        run_waiters();
        return;
    }
    T.win = w;
    T.state = SS_ACTIVE;
    make_background();
    T.need_pw = account_has_password();
    T.t0 = g_ticks;
    T.time_y = -time_block_h() - K(16);
    T.arrived = false;
    T.hover = T.pressed = H_NONE;
    win_move(w, 0, 0);
    w->fullscreen = true;               /* the bottom islands hide while it has the focus */
    w->opacity = 255;                   /* no fade-in: it replaces the screen before it */
    win_set_event_handler(w, handler);
    T.timer_ms = 0;
    set_timer(ANIM_MS);
    T.prev_filter = wm_input_filter;
    wm_input_filter = my_filter;
    wm_focus(w);
    wm_update_taskbar_visibility();
    win_invalidate(w);
    klog("startscreen: shown (account \"%s\"%s, %s)", account_name(), account_is_guest() ? ", ISO" : "",
         T.need_pw ? "password" : "no password");
}
