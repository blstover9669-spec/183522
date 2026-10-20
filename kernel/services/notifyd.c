/*
 * notifyd.c - toast notifications.
 *
 * notify(title, body, icon) shows a card that slides in from the right just
 * above the taskbar. New toasts appear at the bottom and push older ones up;
 * each disappears after a few seconds (the timer pauses while the pointer is
 * over it) or when clicked. Everything is recorded in a history ring.
 */
#include "services/svc_internal.h"

#define TOAST_W     340
#define TOAST_LIFE  5000
#define MAX_VISIBLE 4
#define HISTORY     50
#define ANIM_MS     15

typedef struct toast {
    notification_t n;
    window_t *win;
    double slide;            /* 0 = off screen right, 1 = in place  */
    int y;                   /* current top (screen coords)         */
    uint32_t alive_ms;       /* time shown, not counting hover      */
    bool leaving, hover, hover_close;
    struct toast *next;      /* newest first                        */
} toast_t;

static bool running;
static toast_t *toasts;
static int anim_timer;
static uint32_t last_anim_tick;
static int next_id = 1;
static notification_t history[HISTORY];
static int hist_n;           /* total ever recorded                 */
static notification_t pending[MAX_VISIBLE];
static int npending;

bool notifyd_running(void) { return running; }

/* ---- history ----------------------------------------------------------------------- */
int notify_history_count(void) { return MIN(hist_n, HISTORY); }

bool notify_history_get(int index, notification_t *out) {
    if (index < 0 || index >= notify_history_count() || !out) return false;
    *out = history[(hist_n - 1 - index) % HISTORY];
    return true;
}

void notify_history_clear(void) { hist_n = 0; }

/* ---- toasts -------------------------------------------------------------------------- */
static int toast_height(const notification_t *n) {
    int bh = n->body[0] ? font_wrap_height(FONT_UI, n->body, TOAST_W - 74) : 0;
    bh = MIN(bh, font_height(FONT_UI) * 3);
    return MAX(68, 36 + bh + 16);
}

static int final_x(void) { return screen_width() - TOAST_W - 12; }

static double ease(double t) { t = CLAMP(t, 0.0, 1.0); return 1 - (1 - t) * (1 - t) * (1 - t); }

static void toast_paint(window_t *win, toast_t *t) {
    surface_t *s = win_surface(win);
    const theme_t *th = theme_get();
    gfx_clear(s, th->menu_bg);
    gfx_rounded_rect(s, 0, 0, s->w, s->h, 10, color_mix(th->menu_bg, th->window_border, 150));
    icon_draw(t->n.icon ? t->n.icon : ICON_INFO, s, 16, 16, 32);
    int tw = s->w - 64 - (t->hover ? 34 : 16);
    gfx_text_ellipsis(s, 62, 13, tw, t->n.title, th->menu_text, FONT_UI_BOLD);
    if (t->n.body[0]) {
        int maxh = font_height(FONT_UI) * 3;
        rect_t old = s->clip;
        surface_set_clip(s, rect_intersect(old, rect_make(62, 33, s->w - 74, maxh)));
        gfx_text_wrapped(s, rect_make(62, 33, s->w - 74, maxh), t->n.body, color_mix(th->menu_text, th->menu_bg, 70), FONT_UI);
        s->clip = old;
    }
    if (t->hover) {
        rect_t cb = rect_make(s->w - 30, 8, 22, 22);
        if (t->hover_close) gfx_fill_rounded_rect(s, cb.x, cb.y, cb.w, cb.h, 5, th->menu_hover);
        ui_draw_glyph(s, UI_GLYPH_CLOSE, cb.x + 6, cb.y + 6, 10, color_mix(th->menu_text, th->menu_bg, 40));
    }
    /* remaining time */
    int left = (int)(TOAST_LIFE - MIN(t->alive_ms, (uint32_t)TOAST_LIFE));
    int bw = (s->w - 24) * left / TOAST_LIFE;
    if (bw > 0) gfx_fill_rounded_rect(s, 12, s->h - 4, bw, 2, 1, color_alpha(th->accent, t->hover ? 90 : 170));
}

static toast_t *toast_of(window_t *w) {
    for (toast_t *t = toasts; t; t = t->next) if (t->win == w) return t;
    return NULL;
}

static void start_anim(void);

static void toast_event(window_t *win, const event_t *ev) {
    toast_t *t = toast_of(win);
    if (!t) return;
    switch (ev->type) {
    case EV_PAINT:
        toast_paint(win, t);
        break;
    case EV_MOUSE_MOVE: {
        bool hc = ev->x >= win_width(win) - 30 && ev->x < win_width(win) - 8 && ev->y >= 8 && ev->y < 30;
        if (!t->hover || hc != t->hover_close) { t->hover = true; t->hover_close = hc; win_invalidate(win); }
        break;
    }
    case EV_MOUSE_LEAVE:
        t->hover = t->hover_close = false;
        win_invalidate(win);
        break;
    case EV_MOUSE_DOWN:
        t->leaving = true;
        start_anim();
        break;
    case EV_CLOSE: {
        toast_t **pp = &toasts;
        while (*pp && *pp != t) pp = &(*pp)->next;
        if (*pp) *pp = t->next;
        kfree(t);
        start_anim();     /* let the others slide down */
        break;
    }
    default: break;
    }
}

static void anim_tick(void *ctx) {
    (void)ctx;
    uint32_t now = sys_uptime_ms();
    uint32_t dt = now - last_anim_tick;
    last_anim_tick = now;
    if (dt > 100) dt = 100;
    bool busy = false;
    /* stack from the bottom: newest lowest */
    int y = screen_height() - wm_taskbar_h - 12;
    int count = 0;
    for (toast_t *t = toasts, *nx; t; t = nx) {
        nx = t->next;    /* win_close() below frees t */
        if (!win_is_valid(t->win)) continue;
        int h = win_height(t->win);
        if (!t->leaving && ++count > MAX_VISIBLE) t->leaving = true;
        if (!t->leaving) {
            if (t->slide < 1) { t->slide = MIN(1.0, t->slide + dt / 220.0); busy = true; }
            if (!t->hover) {
                uint32_t before = t->alive_ms;
                t->alive_ms += dt;
                if (t->alive_ms / 100 != before / 100) win_invalidate_rect(t->win, 0, h - 6, win_width(t->win), 6);
                if (t->alive_ms >= TOAST_LIFE) t->leaving = true;
            }
            busy = true;   /* lifetime counting */
            y -= h;
        } else {
            t->slide -= dt / 180.0;
            busy = true;
            if (t->slide <= 0) { win_close(t->win); continue; }
        }
        int target = t->leaving ? t->y : y;
        if (!t->leaving) y -= 10;
        int ny = t->y == 0 ? target : t->y + (target - t->y) * 3 / 10 + (target > t->y ? 1 : target < t->y ? -1 : 0);
        if ((target - t->y) * (target - ny) < 0) ny = target;   /* don't overshoot */
        t->y = ny;
        if (t->y != target) busy = true;
        int x = final_x() + (int)((1 - ease(t->slide)) * (TOAST_W + 24));
        win_move(t->win, x, t->y);
    }
    if (!busy && anim_timer) { timer_cancel(anim_timer); anim_timer = 0; }
}

static void start_anim(void) {
    if (anim_timer) return;
    last_anim_tick = sys_uptime_ms();
    anim_timer = timer_add(ANIM_MS, anim_tick, NULL, true);
}

static void show_toast(const notification_t *n) {
    toast_t *t = kcalloc(1, sizeof *t);
    if (!t) return;
    t->n = *n;
    const app_desc_t *saved = wm_current_app;
    wm_current_app = NULL;
    window_t *w = wm_create_ex("toast", TOAST_W, toast_height(n), WIN_FRAMELESS | WIN_NO_TASKBAR, LAYER_POPUP);
    wm_current_app = saved;
    if (!w) { kfree(t); return; }
    w->radius = 10;
    t->win = w;
    t->y = screen_height() - wm_taskbar_h - 12 - w->ch;
    t->next = toasts;
    toasts = t;
    win_set_event_handler(w, toast_event);
    win_move(w, screen_width(), t->y);
    start_anim();
    anim_tick(NULL);
}

int notify(const char *title, const char *body, int icon_kind) {
    notification_t n;
    memset(&n, 0, sizeof n);
    n.id = next_id++;
    n.time = sys_time();
    n.icon = icon_kind > 0 && icon_kind < ICON_COUNT ? icon_kind : ICON_INFO;
    strlcpy(n.title, title ? title : "", sizeof n.title);
    strlcpy(n.body, body ? body : "", sizeof n.body);
    history[hist_n % HISTORY] = n;
    hist_n++;
    log_info("notifyd", "%s%s%s", n.title, n.body[0] ? ": " : "", n.body);
    if (!running || !screen_width()) return 0;
    if (screensaver_active()) {
        if (npending == MAX_VISIBLE) { memmove(pending, pending + 1, sizeof pending[0] * (MAX_VISIBLE - 1)); npending--; }
        pending[npending++] = n;
        return n.id;
    }
    show_toast(&n);
    sys_beep(1175, 30);
    return n.id;
}

void notify_dismiss(int id) {
    for (toast_t *t = toasts; t; t = t->next)
        if (t->n.id == id) { t->leaving = true; start_anim(); }
}

/* legacy API */
void ui_notify(const char *title, const char *text) { notify(title, text, ICON_INFO); }

/* ---- service ---------------------------------------------------------------------------- */
static int notifyd_start(void) { running = true; return 0; }

static void notifyd_stop(void) {
    running = false;
    npending = 0;
    while (toasts) {
        toast_t *t = toasts;
        if (win_is_valid(t->win)) win_close(t->win);   /* EV_CLOSE unlinks and frees */
        if (toasts == t) { toasts = t->next; kfree(t); }
    }
    if (anim_timer) { timer_cancel(anim_timer); anim_timer = 0; }
}

static void notifyd_tick(void) {
    if (npending && !screensaver_active()) {
        for (int i = 0; i < npending; i++) show_toast(&pending[i]);
        npending = 0;
        sys_beep(1175, 30);
    }
}

REGISTER_SERVICE(.name = "notifyd", .display_name = "Notifications",
                 .description = "Shows toast notifications above the taskbar and keeps a history",
                 .autostart = true, .restart_on_failure = true,
                 .start = notifyd_start, .stop = notifyd_stop, .tick = notifyd_tick, .tick_ms = 500,
                 .icon = ICON_BELL);
