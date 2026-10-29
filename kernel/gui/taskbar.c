/* taskbar.c - bottom taskbar: Start button, window buttons, sound toggle, clock + calendar. */
#include "wm_internal.h"

window_t *g_taskbar_win;
static surface_t *tb_bg;         /* blurred + tinted wallpaper strip */

#define START_W 48
#define BTN_MAXW 200
#define CLOCK_W 96
#define TRAY_W 34
#define SHOWDESK_W 8

#define PIN_W 40
#define MAX_PINNED 16
#define HIT_PIN(i) (-10 - (i))     /* hit codes of pinned app buttons */

typedef struct { window_t *win; rect_t r; } tbtn_t;
static tbtn_t btns[64];
static int nbtns;
typedef struct { const app_desc_t *app; rect_t r; } tpin_t;
static tpin_t pinbtns[MAX_PINNED];
static int npinbtns;
static int hover_btn = -1;       /* -1 none, -2 start, -3 clock, -4 sound, -5 show desktop, >=0 task, <=-10 pin */
static int pressed_btn = -1000;
static int last_minute = -1;

/* box blur of a strip (horizontal + vertical passes) */
static void blur(surface_t *s, int radius) {
    int w = s->w, h = s->h;
    uint32_t *tmp = kmalloc((size_t)MAX(w, h) * 4);
    if (!tmp) return;
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < h; y++) {
            uint32_t *row = surface_row(s, y);
            int r = 0, g = 0, b = 0, n = 0;
            for (int x = -radius; x <= radius; x++) {
                uint32_t c = row[CLAMP(x, 0, w - 1)];
                r += COLOR_R(c); g += COLOR_G(c); b += COLOR_B(c); n++;
            }
            for (int x = 0; x < w; x++) {
                tmp[x] = RGB(r / n, g / n, b / n);
                uint32_t c0 = row[CLAMP(x - radius, 0, w - 1)], c1 = row[CLAMP(x + radius + 1, 0, w - 1)];
                r += COLOR_R(c1) - COLOR_R(c0); g += COLOR_G(c1) - COLOR_G(c0); b += COLOR_B(c1) - COLOR_B(c0);
            }
            memcpy(row, tmp, (size_t)w * 4);
        }
        for (int x = 0; x < w; x++) {
            int r = 0, g = 0, b = 0, n = 0;
            for (int y = -radius; y <= radius; y++) {
                uint32_t c = surface_row(s, CLAMP(y, 0, h - 1))[x];
                r += COLOR_R(c); g += COLOR_G(c); b += COLOR_B(c); n++;
            }
            for (int y = 0; y < h; y++) {
                tmp[y] = RGB(r / n, g / n, b / n);
                uint32_t c0 = surface_row(s, CLAMP(y - radius, 0, h - 1))[x], c1 = surface_row(s, CLAMP(y + radius + 1, 0, h - 1))[x];
                r += COLOR_R(c1) - COLOR_R(c0); g += COLOR_G(c1) - COLOR_G(c0); b += COLOR_B(c1) - COLOR_B(c0);
            }
            for (int y = 0; y < h; y++) surface_row(s, y)[x] = tmp[y];
        }
    }
    kfree(tmp);
}

void taskbar_wallpaper_changed(void) {
    if (!g_taskbar_win) return;
    int w = screen_width(), h = wm_taskbar_h;
    if (!tb_bg || tb_bg->w != w || tb_bg->h != h) {
        surface_destroy(tb_bg);
        tb_bg = surface_create(w, h);
    }
    if (!tb_bg) return;
    if (g_wallpaper) {
        /* blur a slightly taller strip so the top edge is smooth */
        int extra = 24;
        surface_t *strip = surface_create(w, h + extra);
        if (strip) {
            gfx_blit_rect(strip, 0, 0, g_wallpaper, 0, screen_height() - h - extra, w, h + extra);
            blur(strip, 14);
            gfx_blit_rect(tb_bg, 0, 0, strip, 0, extra, w, h);
            surface_destroy(strip);
        }
    } else {
        gfx_clear(tb_bg, RGB(30, 30, 40));
    }
    const theme_t *t = theme_get();
    gfx_fill_rect(tb_bg, 0, 0, w, h, t->taskbar_bg);
    gfx_hline(tb_bg, 0, 0, w, t->dark ? RGBA(255, 255, 255, 30) : RGBA(255, 255, 255, 160));
    gfx_hline(tb_bg, 0, 1, w, t->dark ? RGBA(255, 255, 255, 10) : RGBA(0, 0, 0, 12));
    win_invalidate(g_taskbar_win);
}

static rect_t start_rect(void) { return rect_make(6, 4, START_W - 4, wm_taskbar_h - 8); }
static rect_t clock_rect(void) { return rect_make(screen_width() - SHOWDESK_W - CLOCK_W - 4, 4, CLOCK_W, wm_taskbar_h - 8); }
static rect_t sound_rect(void) { rect_t c = clock_rect(); return rect_make(c.x - TRAY_W - 2, 4, TRAY_W, wm_taskbar_h - 8); }
static rect_t showdesk_rect(void) { return rect_make(screen_width() - SHOWDESK_W, 0, SHOWDESK_W, wm_taskbar_h); }

static void layout_pins(void) {
    npinbtns = 0;
    int n = MIN(shell_pin_count(), MAX_PINNED);
    for (int i = 0; i < n; i++) {
        const app_desc_t *a = app_find(shell_pin_get(i));
        if (!a) continue;
        pinbtns[npinbtns].app = a;
        pinbtns[npinbtns].r = rect_make(START_W + 8 + npinbtns * (PIN_W + 2), 4, PIN_W, wm_taskbar_h - 8);
        npinbtns++;
    }
}

static int pins_end(void) { return npinbtns ? START_W + 8 + npinbtns * (PIN_W + 2) + 6 : START_W + 4; }

static void layout_buttons(void) {
    window_t *list[64];
    int n = win_list(list, 64);
    if (n > 64) n = 64;
    layout_pins();
    int x0 = pins_end() + 6, x1 = sound_rect().x - 8;
    int avail = x1 - x0;
    int bw = n ? MIN(BTN_MAXW, avail / n - 4) : BTN_MAXW;
    if (bw < 44) bw = 44;
    nbtns = 0;
    for (int i = 0; i < n; i++) {
        int x = x0 + i * (bw + 4);
        if (x + bw > x1) break;
        btns[nbtns].win = list[i];
        btns[nbtns].r = rect_make(x, 4, bw, wm_taskbar_h - 8);
        nbtns++;
    }
}

static void draw_speaker(surface_t *s, int x, int y, uint32_t c, bool on) {
    point_t p[6] = { { x, y + 5 }, { x + 4, y + 5 }, { x + 9, y + 1 }, { x + 9, y + 15 }, { x + 4, y + 11 }, { x, y + 11 } };
    gfx_fill_polygon(s, p, 6, c);
    if (on) {
        rect_t old = s->clip;
        surface_set_clip(s, rect_intersect(old, rect_make(x + 11, y - 4, 16, 26)));
        gfx_circle_aa(s, x + 9, y + 8, 4.5, 1.3, color_alpha(c, 220));
        gfx_circle_aa(s, x + 9, y + 8, 8, 1.3, color_alpha(c, 160));
        s->clip = old;
    } else {
        gfx_line_aa(s, x + 12, y + 5, x + 18, y + 11, c);
        gfx_line_aa(s, x + 18, y + 5, x + 12, y + 11, c);
    }
}

/* "Developer version 2.5" on developer editions (NOVA_EDITION "Developer Preview"), else NULL */
static const char *developer_label(void) {
    return strstr(NOVA_EDITION, "Developer") ? "Developer version " NOVA_VERSION : NULL;
}

static void taskbar_paint(window_t *win, const event_t *ev) {
    (void)ev;
    const char *dev_label = developer_label();
    surface_t *s = win_surface(win);
    const theme_t *t = theme_get();
    if (tb_bg) gfx_blit(s, 0, 0, tb_bg);
    layout_buttons();

    /* start button */
    rect_t sr = start_rect();
    if (hover_btn == -2 || startmenu_is_open())
        gfx_fill_rounded_rect(s, sr.x, sr.y, sr.w, sr.h, 6, startmenu_is_open() ? t->taskbar_active : t->taskbar_hover);
    ui_draw_logo(s, sr.x + (sr.w - 28) / 2, sr.y + (sr.h - 28) / 2, 28);

    /* pinned apps */
    for (int i = 0; i < npinbtns; i++) {
        rect_t r = pinbtns[i].r;
        if (hover_btn == HIT_PIN(i)) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, t->taskbar_hover);
        app_draw_icon(s, pinbtns[i].app, r.x + (r.w - 24) / 2, r.y + (r.h - 24) / 2 - 1, 24);
        if (app_window_count(pinbtns[i].app) > 0)
            gfx_fill_circle_aa(s, r.x + r.w / 2.0, r.y + r.h - 2.5, 2, color_alpha(t->taskbar_text, 150));
    }
    if (npinbtns) gfx_vline(s, pins_end() - 2, 12, wm_taskbar_h - 24, color_alpha(t->taskbar_text, 50));

    /* window buttons */
    for (int i = 0; i < nbtns; i++) {
        window_t *w = btns[i].win;
        rect_t r = btns[i].r;
        bool active = w == wm_focused && !w->minimized;
        if (active) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, t->taskbar_active);
        else if (hover_btn == i) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, t->taskbar_hover);
        int ix = r.x + 8;
        if (w->app) app_draw_icon(s, w->app, ix, r.y + (r.h - 22) / 2, 22);
        else default_icon(s, ix, r.y + (r.h - 22) / 2, 22, w->title);
        if (r.w > 60) {
            uint32_t tc = w->minimized ? color_mix(t->taskbar_text, t->dark ? COLOR_BLACK : COLOR_WHITE, 100) : t->taskbar_text;
            gfx_text_ellipsis(s, ix + 30, r.y + (r.h - font_height(FONT_UI)) / 2, r.w - 46, w->title, tc, FONT_UI);
        }
        /* indicator bar */
        int iw = active ? 18 : 8;
        gfx_fill_rounded_rect(s, r.x + (r.w - iw) / 2, r.y + r.h - 3, iw, 3, 1,
                              active ? t->accent : color_alpha(t->taskbar_text, 110));
    }

    /* sound toggle */
    rect_t so = sound_rect();
    if (hover_btn == -4) gfx_fill_rounded_rect(s, so.x, so.y, so.w, so.h, 6, t->taskbar_hover);
    draw_speaker(s, so.x + 8, so.y + (so.h - 17) / 2, t->taskbar_text, sys_sound_enabled());

    /* clock */
    rect_t cr = clock_rect();
    if (hover_btn == -3) gfx_fill_rounded_rect(s, cr.x, cr.y, cr.w, cr.h, 6, t->taskbar_hover);
    datetime_t dt;
    sys_datetime(&dt);
    last_minute = dt.minute;
    char tbuf[16], dbuf[32];
    snprintf(tbuf, sizeof tbuf, "%02d:%02d", dt.hour, dt.minute);
    snprintf(dbuf, sizeof dbuf, "%d %.3s %d", dt.day, sys_month_name(dt.month), dt.year);
    int fh = font_height(FONT_UI);
    /* with the developer label below them, time and date move up a little */
    int ty = dev_label ? 1 : cr.y + cr.h / 2 - fh + 1, dy = dev_label ? 15 : cr.y + cr.h / 2 + 1;
    gfx_text_aligned(s, rect_make(cr.x, ty, cr.w - 8, fh), tbuf, t->taskbar_text, FONT_UI_BOLD, ALIGN_RIGHT);
    gfx_text_aligned(s, rect_make(cr.x, dy, cr.w - 8, fh), dbuf, color_mix(t->taskbar_text, t->dark ? COLOR_BLACK : COLOR_WHITE, 60), FONT_UI, ALIGN_RIGHT);

    /* show desktop sliver */
    rect_t sd = showdesk_rect();
    gfx_vline(s, sd.x, 10, wm_taskbar_h - 20, color_alpha(t->taskbar_text, 60));
    if (hover_btn == -5) gfx_fill_rect(s, sd.x + 1, 0, sd.w - 1, sd.h, t->taskbar_hover);

    /* developer editions: "Developer version 2.5" at the bottom right, drawn last so
       it is always in front (also over the clock); black, white in dark mode */
    if (dev_label) {
        int lw = font_text_width(FONT_UI, dev_label), lh = font_height(FONT_UI);
        gfx_text(s, sd.x - 12 - lw, wm_taskbar_h - lh, dev_label, t->dark ? COLOR_WHITE : COLOR_BLACK, FONT_UI);
    }
}

static int hit(int x, int y) {
    if (rect_contains(start_rect(), x, y) || (x < START_W + 4)) return -2;
    if (rect_contains(clock_rect(), x, y)) return -3;
    if (rect_contains(sound_rect(), x, y)) return -4;
    if (rect_contains(showdesk_rect(), x, y)) return -5;
    for (int i = 0; i < npinbtns; i++) if (rect_contains(pinbtns[i].r, x, y)) return HIT_PIN(i);
    for (int i = 0; i < nbtns; i++) if (rect_contains(btns[i].r, x, y)) return i;
    return -1;
}

/* ---- calendar popup ------------------------------------------------------------------ */
typedef struct { int year, month; } cal_t;
static uint32_t cal_closed_serial;
static window_t *cal_win;

static void cal_handler(window_t *win, const event_t *ev) {
    cal_t *c = win_get_userdata(win);
    if (!c) return;
    const theme_t *t = theme_get();
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        gfx_clear(s, t->menu_bg);
        gfx_rounded_rect(s, 0, 0, s->w, s->h, 8, color_mix(t->menu_bg, t->window_border, 140));
        datetime_t now;
        sys_datetime(&now);
        char buf[64];
        snprintf(buf, sizeof buf, "%s, %d %s %d", sys_weekday_name(now.weekday), now.day, sys_month_name(now.month), now.year);
        gfx_text(s, 18, 14, buf, color_mix(t->menu_text, t->menu_bg, 60), FONT_UI);
        snprintf(buf, sizeof buf, "%02d:%02d:%02d", now.hour, now.minute, now.second);
        gfx_text(s, 16, 32, buf, t->menu_text, FONT_UI_TITLE);
        gfx_hline(s, 12, 76, s->w - 24, t->menu_separator);
        snprintf(buf, sizeof buf, "%s %d", sys_month_name(c->month), c->year);
        gfx_text(s, 18, 88, buf, t->menu_text, FONT_UI_BOLD);
        ui_draw_glyph(s, UI_GLYPH_ARROW_LEFT, s->w - 66, 88, 16, t->menu_text);
        ui_draw_glyph(s, UI_GLYPH_ARROW_RIGHT, s->w - 34, 88, 16, t->menu_text);
        static const char *wd[7] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
        int cw = (s->w - 24) / 7;
        for (int i = 0; i < 7; i++)
            gfx_text_aligned(s, rect_make(12 + i * cw, 116, cw, 20), wd[i], color_mix(t->menu_text, t->menu_bg, 90), FONT_UI, ALIGN_CENTER);
        datetime_t first = { c->year, c->month, 1, 0, 0, 0, 0 };
        sys_time_to_datetime(sys_datetime_to_time(&first), &first);
        int col = (first.weekday + 6) % 7;
        static const int md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int days = md[c->month - 1] + (c->month == 2 && ((c->year % 4 == 0 && c->year % 100) || c->year % 400 == 0));
        int row = 0;
        for (int d = 1; d <= days; d++) {
            int x = 12 + col * cw, y = 140 + row * 30;
            bool today = d == now.day && c->month == now.month && c->year == now.year;
            if (today) gfx_fill_circle_aa(s, x + cw / 2.0, y + 13, 13, t->accent);
            snprintf(buf, sizeof buf, "%d", d);
            gfx_text_aligned(s, rect_make(x, y + 4, cw, 18), buf, today ? t->accent_text : t->menu_text, today ? FONT_UI_BOLD : FONT_UI, ALIGN_CENTER);
            if (++col == 7) { col = 0; row++; }
        }
        break;
    }
    case EV_MOUSE_DOWN:
        if (ev->y >= 80 && ev->y < 112) {
            int w = win_width(win);
            if (ev->x >= w - 72 && ev->x < w - 44) { if (--c->month < 1) { c->month = 12; c->year--; } win_invalidate(win); }
            else if (ev->x >= w - 40) { if (++c->month > 12) { c->month = 1; c->year++; } win_invalidate(win); }
        }
        break;
    case EV_TIMER:
        win_invalidate_rect(win, 0, 0, win_width(win), 76);
        break;
    case EV_CLOSE:
        cal_closed_serial = wm_click_serial;
        kfree(c);
        win_set_userdata(win, NULL);
        break;
    default:
        break;
    }
}

static void cal_destroyed(window_t *w) { (void)w; cal_win = NULL; }

static void show_calendar(void) {
    if (cal_closed_serial == wm_click_serial) return;   /* this click just closed it */
    cal_t *c = kcalloc(1, sizeof *c);
    if (!c) return;
    datetime_t dt;
    sys_datetime(&dt);
    c->year = dt.year;
    c->month = dt.month;
    const app_desc_t *saved = wm_current_app;
    wm_current_app = NULL;
    window_t *w = wm_create_ex("calendar", 300, 330, WIN_POPUP | WIN_NO_TASKBAR, LAYER_POPUP);
    wm_current_app = saved;
    if (!w) { kfree(c); return; }
    win_set_userdata(w, c);
    win_set_event_handler(w, cal_handler);
    w->on_destroy = cal_destroyed;
    cal_win = w;
    win_move(w, screen_width() - 300 - 10, screen_height() - wm_taskbar_h - 330 - 10);
    win_set_timer(w, 1000);
}

/* ---- events -------------------------------------------------------------------------- */
static void task_menu_cb(int item, void *ctx) {
    window_t *w = win_find_by_id((int)(intptr_t)ctx);
    if (!w) return;
    if (item == 6 && w->app) { shell_set_pinned(w->app->name, !shell_is_pinned(w->app->name)); return; }
    switch (item) {
    case 0: win_focus(w); break;
    case 1: win_minimize(w); break;
    case 2: if (w->maximized) win_restore(w); else { win_focus(w); win_maximize(w); } break;
    case 4: if (w->flags & WIN_CONFIRM_CLOSE) { event_t ev = { .type = EV_CLOSE_REQUEST }; wm_send(w, &ev); } else win_close(w); break;
    default: break;
    }
}

/* topmost normal window of an app (not its dialogs), or NULL */
static window_t *app_top_window(const app_desc_t *a) {
    for (window_t *w = wm_top; w; w = w->below)
        if (w->app == a && !w->closing && w->layer == LAYER_NORMAL && !w->owner && w->visible) return w;
    return NULL;
}

static void pin_click(const app_desc_t *a) {
    window_t *w = app_top_window(a);
    if (!w) { app_launch(a, NULL); return; }
    if (w == wm_focused && !w->minimized) win_minimize(w);
    else win_focus(w);
}

static const app_desc_t *pin_menu_app;
static void pin_menu_cb(int item, void *ctx) {
    (void)ctx;
    const app_desc_t *a = pin_menu_app;
    if (!a) return;
    if (item == 0) app_launch(a, NULL);
    else if (item == 2) shell_set_pinned(a->name, false);
}

static void show_desktop(void) {
    bool any = false;
    for (window_t *w = wm_bottom; w; w = w->above)
        if (w->layer == LAYER_NORMAL && !w->closing && w->visible && !w->minimized) { any = true; break; }
    for (window_t *w = wm_bottom; w; w = w->above) {
        if (w->layer != LAYER_NORMAL || w->closing || !w->visible) continue;
        if (any) win_minimize(w); else if (w->minimized) win_restore(w);
    }
}

static void taskbar_handler(window_t *win, const event_t *ev) {
    switch (ev->type) {
    case EV_PAINT:
        taskbar_paint(win, ev);
        break;
    case EV_MOUSE_MOVE: {
        int h = hit(ev->x, ev->y);
        if (h != hover_btn) { hover_btn = h; win_invalidate(win); }
        break;
    }
    case EV_MOUSE_LEAVE:
        if (hover_btn != -1) { hover_btn = -1; win_invalidate(win); }
        break;
    case EV_MOUSE_DOWN: {
        int h = hit(ev->x, ev->y);
        pressed_btn = h;
        if (ev->button == MOUSE_RIGHT && h >= 0 && h < nbtns) {
            window_t *w = btns[h].win;
            rect_t r = btns[h].r;
            char items[160];
            snprintf(items, sizeof items, "%s%s", w->maximized ? "Restore|Minimize|Restore size|-|Close" : "Restore|Minimize|Maximize|-|Close",
                     !w->app || (w->app->flags & APP_HIDDEN) ? "" : shell_is_pinned(w->app->name) ? "|-|Unpin from taskbar" : "|-|Pin to taskbar");
            ui_popup_menu(win, r.x, -4, items, task_menu_cb, (void *)(intptr_t)w->id);
            break;
        }
        if (ev->button == MOUSE_RIGHT && h <= HIT_PIN(0) && h > HIT_PIN(npinbtns)) {
            int i = HIT_PIN(0) - h;
            pin_menu_app = pinbtns[i].app;
            ui_popup_menu(win, pinbtns[i].r.x, -4, "Open new window|-|Unpin from taskbar", pin_menu_cb, NULL);
            break;
        }
        if (ev->button != MOUSE_LEFT) break;
        if (h == -2) { if (!startmenu_just_closed()) startmenu_toggle(); }
        else if (h == -3) show_calendar();
        else if (h == -4) { sys_set_sound_enabled(!sys_sound_enabled()); if (sys_sound_enabled()) sys_beep(880, 40); win_invalidate(win); }
        else if (h == -5) show_desktop();
        else if (h <= HIT_PIN(0) && h > HIT_PIN(npinbtns)) pin_click(pinbtns[HIT_PIN(0) - h].app);
        else if (h >= 0 && h < nbtns) {
            window_t *w = btns[h].win;
            if (w == wm_focused && !w->minimized) win_minimize(w);
            else win_focus(w);
        }
        break;
    }
    case EV_TIMER: {
        datetime_t dt;
        sys_datetime(&dt);
        if (dt.minute != last_minute) win_invalidate(win);
        break;
    }
    default:
        break;
    }
}

void taskbar_refresh(void) {
    if (g_taskbar_win) win_invalidate(g_taskbar_win);
}

void taskbar_init(void) {
    const app_desc_t *saved = wm_current_app;
    wm_current_app = NULL;
    g_taskbar_win = wm_create_ex("Taskbar", screen_width(), wm_taskbar_h, WIN_FRAMELESS | WIN_NO_TASKBAR | WIN_NO_SHADOW, LAYER_TOPMOST);
    wm_current_app = saved;
    if (!g_taskbar_win) kpanic("cannot create taskbar");
    win_move(g_taskbar_win, 0, screen_height() - wm_taskbar_h);
    win_set_event_handler(g_taskbar_win, taskbar_handler);
    win_set_timer(g_taskbar_win, 1000);
    taskbar_wallpaper_changed();
}
