/*
 * hello.c - "About NovaOS": the reference example app.
 *
 * Demonstrates the main pieces of the app API:
 *   - REGISTER_APP() with a name, category, description, launch callback and
 *     an icon drawn with gfx primitives;
 *   - win_create() + per-window state stored with win_set_userdata();
 *   - an event handler that paints on EV_PAINT, animates on EV_TIMER and
 *     frees its state on EV_CLOSE;
 *   - widgets (labels, buttons, checkbox) and an asynchronous message box;
 *   - reading system information and the theme.
 */
#include <nova.h>

typedef struct {
    int timer;            /* animation timer id                 */
    double phase;         /* animation phase for the logo glow  */
    widget_t *uptime;     /* label updated once a second        */
    widget_t *memory;
    int ticks;
} about_t;

#define W 460
#define H 400

static void update_labels(about_t *a) {
    char buf[96];
    uint32_t up = sys_uptime_ms() / 1000;
    snprintf(buf, sizeof buf, "Uptime: %u:%02u:%02u", up / 3600, (up / 60) % 60, up % 60);
    ui_set_text(a->uptime, buf);
    snprintf(buf, sizeof buf, "Memory: %u MiB used of %u MiB", (sys_mem_used() >> 20) + 1, sys_mem_total() >> 20);
    ui_set_text(a->memory, buf);
}

static void on_close_clicked(widget_t *w, void *ctx) {
    (void)ctx;
    win_close(ui_window(w));
}

static void on_hello_result(int result, void *ctx) {
    (void)ctx;
    if (result == MB_RESULT_YES) ui_notify("NovaOS", "Glad you like it!");
}

static void on_hello_clicked(widget_t *w, void *ctx) {
    (void)ctx;
    sys_beep(660, 80);
    ui_message_box(ui_window(w), "Hello", "Hello from NovaOS!\n\nDo you like it so far?",
                   MB_YESNO | MB_ICON_QUESTION, on_hello_result, NULL);
}

static void on_dark_toggled(widget_t *w, void *ctx) {
    (void)ctx;
    theme_set_dark(ui_get_value(w));
}

static void about_event(window_t *win, const event_t *ev) {
    about_t *a = win_get_userdata(win);
    const theme_t *t = theme_get();
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        /* header band with a gentle accent gradient */
        gfx_gradient_v(s, 0, 0, s->w, 150, color_mix(t->window_bg, t->accent, t->dark ? 70 : 45), t->window_bg);
        int glow = (int)(16 + 8 * sin(a->phase));   /* 8..24 px halo */
        for (int i = 4; i > 0; i--)
            gfx_fill_circle_aa(s, 80, 80, 44 + i * glow / 4, color_alpha(t->accent, 12));
        ui_draw_logo(s, 40, 40, 80);
        gfx_text(s, 140, 44, "NovaOS", t->window_text, FONT_UI_TITLE);
        gfx_text(s, 142, 84, sys_kernel_version(), color_mix(t->window_text, t->window_bg, 90), FONT_UI);
        gfx_text(s, 142, 104, "A hobby OS written from scratch in C", color_mix(t->window_text, t->window_bg, 90), FONT_UI);
        ui_draw_panel(s, rect_make(20, 160, s->w - 40, 150));
        break;
    }
    case EV_TIMER:
        a->phase += 0.25;
        win_invalidate_rect(win, 0, 0, 160, 152);   /* logo + halo area */
        if (++a->ticks % 10 == 0) update_labels(a);
        break;
    case EV_THEME:
        /* colours are read from theme_get() at paint time; nothing to do */
        break;
    case EV_CLOSE:
        free(a);   /* timers and widgets are cleaned up by the system */
        break;
    default:
        break;
    }
}

static void about_launch(const char *arg) {
    (void)arg;
    about_t *a = calloc(1, sizeof *a);
    if (!a) return;
    window_t *win = win_create("About NovaOS", W, H, WIN_CENTER);
    if (!win) { free(a); return; }
    win_set_userdata(win, a);
    win_set_event_handler(win, about_event);

    char buf[128];
    int y = 172;
    snprintf(buf, sizeof buf, "Processor: %s", sys_cpu_name());
    ui_label(win, 36, y, W - 72, 22, buf);
    snprintf(buf, sizeof buf, "Display: %d x %d, %d apps installed", screen_width(), screen_height(), app_count());
    ui_label(win, 36, y + 26, W - 72, 22, buf);
    a->memory = ui_label(win, 36, y + 52, W - 72, 22, "");
    a->uptime = ui_label(win, 36, y + 78, W - 72, 22, "");
    ui_checkbox(win, 36, y + 106, 200, 24, "Dark mode", theme_get()->dark, on_dark_toggled, NULL);
    update_labels(a);

    ui_button(win, W - 220, H - 52, 96, 32, "Say hello", on_hello_clicked, NULL);
    widget_t *ok = ui_button(win, W - 116, H - 52, 96, 32, "Close", on_close_clicked, NULL);
    ui_button_set_default(ok, true);

    a->timer = win_set_timer(win, 100);
}

/* Icon: the NovaOS logo with an "i" badge. Drawn at any size on a transparent surface. */
static void about_icon(surface_t *s, int x, int y, int size) {
    ui_draw_logo(s, x + size / 16, y + size / 16, size * 7 / 8);
    int r = size / 5;
    int cx = x + size - r - size / 32, cy = y + size - r - size / 32;
    gfx_fill_circle_aa(s, cx, cy, r + size / 32, COLOR_WHITE);
    gfx_fill_circle_aa(s, cx, cy, r, RGB(40, 120, 230));
    gfx_fill_rect(s, cx - size / 40 - 1, cy - r / 6, size / 20 + 2, r * 2 / 3, COLOR_WHITE);
    gfx_fill_circle_aa(s, cx, cy - r / 2, size / 32 + 1, COLOR_WHITE);
}

REGISTER_APP(.name = "About", .category = CAT_SYSTEM,
             .description = "About NovaOS and this computer",
             .launch = about_launch, .draw_icon = about_icon,
             .flags = APP_SINGLE);
