/*
 * hangtest.c - "Hang Test": a hidden test app for the app watchdog
 * (kernel/core/watchdog.c). Its buttons make it stop responding on purpose:
 *
 *   Freeze                 loops forever (Close the app is the only way out)
 *   Busy 12 s              works for 12 seconds, then returns (Wait brings it back)
 *   Freeze in malloc loop  allocates and frees memory forever: the watchdog must only
 *                          suspend it outside the heap's critical sections
 *
 * Hidden from the desktop, the Start menu and search: the Terminal's `test hang`
 * opens it; with the kernel option hangtest=1 it is shown like any app. A line under
 * the buttons counts the seconds of a window timer, so it is easy to see whether the
 * app runs normally.
 */
#include <nova.h>

typedef struct {
    widget_t *status;
    int timer;
    uint32_t ticks;
    uint32_t busy_ms;          /* last Busy 12 s run took this long (0 = none) */
} hang_t;

static void status_update(window_t *win, hang_t *h) {
    char buf[128];
    if (h->busy_ms)
        snprintf(buf, sizeof buf, "Running normally (%u s). Busy 12 s returned after %u.%u s.",
                 (unsigned)h->ticks, (unsigned)(h->busy_ms / 1000), (unsigned)(h->busy_ms % 1000 / 100));
    else
        snprintf(buf, sizeof buf, "Running normally (%u s).", (unsigned)h->ticks);
    ui_set_text(h->status, buf);
    (void)win;
}

static void on_freeze(widget_t *w, void *ctx) {
    (void)w; (void)ctx;
    klog("hangtest: Freeze - looping forever");
    volatile uint32_t n = 0;
    for (;;) n++;
}

static void on_busy(widget_t *w, void *ctx) {
    hang_t *h = ctx;
    klog("hangtest: Busy 12 s - working");
    uint32_t t0 = sys_uptime_ms();
    volatile uint32_t n = 0;
    while (sys_uptime_ms() - t0 < 12000) n++;
    h->busy_ms = sys_uptime_ms() - t0;
    klog("hangtest: Busy 12 s - returned after %u ms", (unsigned)h->busy_ms);
    status_update(ui_window(w), h);
}

static void on_malloc_loop(widget_t *w, void *ctx) {
    (void)w; (void)ctx;
    klog("hangtest: Freeze in malloc loop - allocating and freeing forever");
    void *slot[32] = { 0 };
    uint32_t i = 0;
    for (;;) {
        int k = (int)(i % 32);
        free(slot[k]);
        size_t n = 16 + (i * 7919u) % 6000;
        slot[k] = malloc(n);
        if (slot[k]) memset(slot[k], (int)(i & 0xFF), n);
        if ((i & 7) == 0) { void *p = realloc(slot[(k + 5) % 32], n / 2 + 8); if (p) slot[(k + 5) % 32] = p; }
        i++;
    }
}

static void hang_event(window_t *win, const event_t *ev) {
    hang_t *h = win_get_userdata(win);
    if (!h) return;
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        const theme_t *t = theme_get();
        gfx_text(s, 20, 16, "Make this app stop responding:", t->window_text, FONT_UI_BOLD);
        break;
    }
    case EV_TIMER:
        h->ticks++;
        status_update(win, h);
        break;
    case EV_CLOSE:
        free(h);
        win_set_userdata(win, NULL);
        break;
    default:
        break;
    }
}

static void hang_launch(const char *arg) {
    (void)arg;
    hang_t *h = calloc(1, sizeof *h);
    if (!h) return;
    window_t *win = win_create("Hang Test", 420, 230, 0);
    if (!win) { free(h); return; }
    win_set_userdata(win, h);
    win_set_event_handler(win, hang_event);
    ui_button(win, 20, 48, 180, 32, "Freeze", on_freeze, h);
    ui_button(win, 20, 90, 180, 32, "Busy 12 s", on_busy, h);
    ui_button(win, 20, 132, 180, 32, "Freeze in malloc loop", on_malloc_loop, h);
    ui_label(win, 214, 54, 196, 24, "loops forever");
    ui_label(win, 214, 96, 196, 24, "works 12 s, then returns");
    ui_label(win, 214, 138, 196, 24, "allocates and frees forever");
    h->status = ui_label(win, 20, 186, 390, 24, "");
    h->timer = win_set_timer(win, 1000);
    status_update(win, h);
}

static void hang_icon(surface_t *s, int x, int y, int size) {
    int r = size / 2 - size / 10;
    gfx_fill_circle_aa(s, x + size / 2, y + size / 2, r, RGB(232, 120, 40));
    gfx_fill_circle_aa(s, x + size / 2, y + size / 2, r - size / 10, RGB(255, 244, 230));
    int cx = x + size / 2, cy = y + size / 2;
    gfx_fill_rect(s, cx - size / 32 - 1, cy - r * 3 / 5, size / 16 + 2, r * 3 / 5, RGB(60, 60, 70));
    gfx_fill_rect(s, cx, cy - size / 32 - 1, r / 2, size / 16 + 2, RGB(60, 60, 70));
}

REGISTER_APP(.name = "Hang Test", .category = CAT_DEVELOPMENT,
             .description = "Test app for the app watchdog: stops responding on purpose",
             .launch = hang_launch, .draw_icon = hang_icon, .flags = APP_HIDDEN);
