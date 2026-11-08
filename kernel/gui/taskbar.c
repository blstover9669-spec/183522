/*
 * taskbar.c - the bottom of the screen. There is no taskbar: separate rounded
 * "islands" float in the bottom wm_taskbar_h (48) px, which maximized windows
 * and the desktop leave free:
 *
 *   [Start] [running apps]        [Settings Welcome Files]        [label  clock]
 *
 *  - Start: the Start button (Start menu).
 *  - running apps: one icon button per running app (minimized ones too; the
 *    button goes when the app closes). Click focuses / restores / minimizes,
 *    right-click opens a menu, the window title shows on hover. Hidden while
 *    no app runs.
 *  - the app rectangle, centred: Settings, Welcome, Files - icons only, and
 *    fixed (it cannot be dragged, its icons cannot be dragged, reordered or
 *    removed). A click darkens the icon and shows a spinner in it until the
 *    app's window is open; while the app is open its outline glows (the glow
 *    grows in, a light keeps running round it, and it fades out when the app
 *    closes). A window timer drives this and repaints only this island.
 *  - the clock (click: calendar) with the "Developer version 2.5" label.
 *
 * Each island is its own frameless topmost window with rounded corners, so
 * whatever lies between them stays visible and clickable. The pin data
 * (shell_pin_*) is kept but no longer shown.
 */
#include "wm_internal.h"

#define ISLAND_H    40
#define ISLAND_R    12
#define ISLAND_GAP  6            /* between islands, and to the screen edges */
#define START_W     44
#define PAD         4            /* running apps: padding round the buttons */
#define BTN_W       36
#define BTN_MIN_W   28
#define BTN_GAP     4
#define BTN_ICON    24
#define MAX_GROUPS  32
#define NDOCK       3            /* the app rectangle */
#define SLOT_W      40
#define SLOT_H      32
#define SLOT_GAP    8
#define SLOT_R      9
#define DOCK_PAD    8
#define DOCK_ICON   28
#define HALO        5            /* the glow reaches this far outside a slot */
#define CLOCK_TXT_W 80
#define FRAME_MS    33           /* animation frame interval */
#define DARKEN_MS   140          /* the icon darkens, then the app starts */
#define GLOW_IN_MS  520
#define GLOW_OUT_MS 420
#define LIGHT_MS    2200         /* one trip of the light round the outline */
#define SPIN_MS     850          /* one turn of the spinner */
#define LAUNCH_TIMEOUT_MS 8000

enum { BAR_START, BAR_APPS, BAR_DOCK, BAR_CLOCK, NBARS };
static window_t *bars[NBARS];
static bool bars_hidden;          /* a fullscreen window is active */
static bool apps_shown;           /* the running-apps island has buttons */
static surface_t *tb_bg;          /* blurred + tinted wallpaper strip behind the islands */
static bool hover_start, hover_clock;
static int hover_btn = -1, hover_slot = -1;
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

/* The islands show the wallpaper blurred and tinted in the old taskbar colours
   (white in light mode, dark in dark mode): one strip, each island takes its part. */
void taskbar_wallpaper_changed(void) {
    if (!bars[BAR_START]) return;
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
    gfx_fill_rect(tb_bg, 0, 0, w, h, theme_get()->taskbar_bg);
    for (int i = 0; i < NBARS; i++) win_invalidate(bars[i]);
}

/* ---- geometry ------------------------------------------------------------------------ */
static int island_y(void) { return screen_height() - wm_taskbar_h + (wm_taskbar_h - ISLAND_H) / 2; }
static int apps_x(void) { return ISLAND_GAP + START_W + ISLAND_GAP; }
static int dock_w(void) { return 2 * DOCK_PAD + NDOCK * SLOT_W + (NDOCK - 1) * SLOT_GAP; }
static int dock_x(void) { return (screen_width() - dock_w()) / 2; }

/* "Developer version 2.5" on developer editions (NOVA_EDITION "Developer Preview"), else NULL */
static const char *developer_label(void) {
    return strstr(NOVA_EDITION, "Developer") ? "Developer version " NOVA_VERSION : NULL;
}

static int clock_w(void) {
    const char *l = developer_label();
    return 12 + (l ? font_text_width(FONT_UI, l) + 14 : 0) + CLOCK_TXT_W + 10;
}

static void island_bg(window_t *w) {
    surface_t *s = win_surface(w);
    const theme_t *t = theme_get();
    if (tb_bg) gfx_blit_rect(s, 0, 0, tb_bg, w->frame.x, w->frame.y - (screen_height() - wm_taskbar_h), w->cw, w->ch);
    else gfx_clear(s, color_alpha(t->taskbar_bg, 255));
    gfx_rounded_rect(s, 0, 0, w->cw, w->ch, ISLAND_R, t->dark ? RGBA(255, 255, 255, 30) : RGBA(0, 0, 0, 26));
}

static void set_visible(window_t *w, bool on) {
    if (!w || w->visible == on) return;
    wm_invalidate_window(w);
    w->visible = on;
    w->need_paint = true;
    wm_invalidate_window(w);
}

/* ---- hover title ----------------------------------------------------------------------- */
static window_t *tip_win;
static char tip_text[160];
static rect_t tip_anchor;

static void tip_handler(window_t *w, const event_t *ev) {
    if (ev->type != EV_PAINT) return;
    surface_t *s = win_surface(w);
    const theme_t *t = theme_get();
    gfx_clear(s, t->menu_bg);
    gfx_rounded_rect(s, 0, 0, s->w, s->h, 6, color_mix(t->menu_bg, t->window_border, 140));
    gfx_text_ellipsis(s, 10, (s->h - font_height(FONT_UI)) / 2, s->w - 20, tip_text, t->menu_text, FONT_UI);
}

static void tip_hide(void) {
    if (tip_win && win_is_valid(tip_win)) win_close(tip_win);
    tip_win = NULL;
    tip_text[0] = 0;
}

/* show text centred above r (screen coordinates) */
static void tip_show(const char *text, rect_t r) {
    if (!text || !*text || bars_hidden) { tip_hide(); return; }
    if (tip_win && win_is_valid(tip_win) && !strcmp(text, tip_text) && r.x == tip_anchor.x && r.y == tip_anchor.y) return;
    tip_hide();
    strlcpy(tip_text, text, sizeof tip_text);
    tip_anchor = r;
    int w = MIN(font_text_width(FONT_UI, tip_text) + 20, 420), h = font_height(FONT_UI) + 10;
    const app_desc_t *saved = wm_current_app;
    wm_current_app = NULL;
    tip_win = wm_create_ex("title", w, h, WIN_FRAMELESS | WIN_NO_TASKBAR, LAYER_POPUP);
    wm_current_app = saved;
    if (!tip_win) return;
    tip_win->radius = 6;
    win_set_event_handler(tip_win, tip_handler);
    win_move(tip_win, CLAMP(r.x + r.w / 2 - w / 2, 4, screen_width() - w - 4), r.y - h - 8);
}

/* ---- running apps ---------------------------------------------------------------------- */
typedef struct {
    const app_desc_t *app;      /* NULL: a window without an app - it has its own button */
    int win_id;                 /* that window (app == NULL) */
    int key;                    /* the oldest window's id: keeps the order stable */
    int nwins;
    window_t *top;              /* topmost window */
    bool active, all_min;
} group_t;

static group_t groups[MAX_GROUPS];
static int ngroups, nshown, btn_w = BTN_W;

static void build_groups(void) {
    window_t *list[64];
    int n = MIN(win_list(list, 64), 64);
    ngroups = 0;
    for (int i = 0; i < n; i++) {
        window_t *w = list[i];
        group_t *g = NULL;
        for (int k = 0; k < ngroups && !g && w->app; k++)
            if (groups[k].app == w->app) g = &groups[k];
        if (!g) {
            if (ngroups == MAX_GROUPS) continue;
            g = &groups[ngroups++];
            *g = (group_t){ .app = w->app, .win_id = w->id, .key = w->id, .all_min = true };
        }
        g->nwins++;
        g->key = MIN(g->key, w->id);
        g->top = w;                                   /* win_list is bottom to top */
        if (!w->minimized) {
            g->all_min = false;
            if (wm_focused == w || (wm_focused && wm_focused->owner == w)) g->active = true;
        }
    }
    for (int a = 1; a < ngroups; a++)                 /* oldest app first */
        for (int b = a; b > 0 && groups[b - 1].key > groups[b].key; b--) {
            group_t t = groups[b];
            groups[b] = groups[b - 1];
            groups[b - 1] = t;
        }
}

/* buttons shrink when space runs out before the app rectangle */
static void layout_apps(void) {
    build_groups();
    int avail = dock_x() - ISLAND_GAP - apps_x();
    btn_w = BTN_W;
    if (ngroups && 2 * PAD + ngroups * (BTN_W + BTN_GAP) - BTN_GAP > avail)
        btn_w = MAX(BTN_MIN_W, (avail - 2 * PAD + BTN_GAP) / ngroups - BTN_GAP);
    nshown = ngroups;
    while (nshown && 2 * PAD + nshown * (btn_w + BTN_GAP) - BTN_GAP > avail) nshown--;
}

static int apps_w(void) { return nshown ? 2 * PAD + nshown * (btn_w + BTN_GAP) - BTN_GAP : 2 * PAD + BTN_W; }
static rect_t btn_rect(int i) { return rect_make(PAD + i * (btn_w + BTN_GAP), PAD, btn_w, ISLAND_H - 2 * PAD); }

static int btn_at(int x, int y) {
    for (int i = 0; i < nshown; i++)
        if (rect_contains(btn_rect(i), x, y)) return i;
    return -1;
}

/* the windows of a button (an app's, or the one window), bottom to top */
static int group_windows(const app_desc_t *app, int win_id, window_t **out, int max) {
    window_t *list[64];
    int n = MIN(win_list(list, 64), 64), k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (app ? list[i]->app == app : list[i]->id == win_id) out[k++] = list[i];
    return k;
}

static void group_title(const group_t *g, char *buf, size_t n) {
    if (g->app && g->nwins > 1) snprintf(buf, n, "%s (%d windows)", g->app->name, g->nwins);
    else strlcpy(buf, g->top->title[0] ? g->top->title : g->app ? g->app->name : "Window", n);
}

/* like the old window buttons: the active app minimizes, any other comes to the front */
static void group_click(const group_t *g) {
    window_t *ws[64];
    int n = group_windows(g->app, g->win_id, ws, 64);
    for (int i = 0; i < n; i++) {
        if (g->active) win_minimize(ws[i]);
        else win_focus(ws[i]);                       /* restores; the topmost ends up focused */
    }
}

static const app_desc_t *menu_app;
static int menu_win_id;

static void apps_menu_cb(int item, void *ctx) {
    (void)ctx;
    window_t *ws[64];
    int n = group_windows(menu_app, menu_win_id, ws, 64);
    if (!n) return;
    window_t *top = ws[n - 1];
    for (int i = 0; i < n; i++) {
        window_t *w = ws[i];
        if (item == 0) win_focus(w);
        else if (item == 1) win_minimize(w);
        else if (item == 4) {
            if (w->flags & WIN_CONFIRM_CLOSE) { event_t ev = { .type = EV_CLOSE_REQUEST }; wm_send(w, &ev); }
            else win_close(w);
        }
    }
    if (item == 2) {
        if (top->maximized) win_restore(top);
        else { win_focus(top); win_maximize(top); }
    }
}

static void apps_menu(int i) {
    const group_t *g = &groups[i];
    window_t *ws[64];
    int n = group_windows(g->app, g->win_id, ws, 64);
    if (!n) return;
    menu_app = g->app;
    menu_win_id = g->win_id;
    char items[96];
    snprintf(items, sizeof items, "Restore|Minimize|%s|-|%s", ws[n - 1]->maximized ? "Restore size" : "Maximize",
             n > 1 ? "Close all windows" : "Close");
    ui_popup_menu(bars[BAR_APPS], btn_rect(i).x, -4, items, apps_menu_cb, NULL);
}

static void paint_apps(window_t *win) {
    layout_apps();                      /* never paint from a stale window list */
    island_bg(win);
    surface_t *s = win_surface(win);
    const theme_t *t = theme_get();
    for (int i = 0; i < nshown; i++) {
        const group_t *g = &groups[i];
        rect_t r = btn_rect(i);
        if (r.x + r.w > s->w) break;
        if (g->active) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 8, t->taskbar_active);
        else if (hover_btn == i) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 8, t->taskbar_hover);
        int ix = r.x + (r.w - BTN_ICON) / 2, iy = r.y + (r.h - BTN_ICON) / 2 - 1;
        const surface_t *img = g->app ? app_icon(g->app, BTN_ICON) : NULL;
        if (img) gfx_blit_alpha(s, ix, iy, img, g->all_min ? 130 : 255);   /* minimized: faded */
        else default_icon(s, ix, iy, BTN_ICON, g->top->title);
        int iw = g->active ? 14 : 6;
        gfx_fill_rounded_rect(s, r.x + (r.w - iw) / 2, r.y + r.h - 3, iw, 3, 1,
                              g->active ? t->accent : color_alpha(t->taskbar_text, 110));
    }
}

/* ---- the app rectangle ----------------------------------------------------------------- */
static const char *const dock_names[NDOCK] = { "Settings", "Welcome", "Files" };

typedef struct {
    const app_desc_t *app;
    bool launching;             /* dark icon + spinner */
    bool launched;              /* app_launch() done, waiting for its window */
    uint32_t t0;                /* when it was clicked */
    int min_id;                 /* the new window has a higher id */
    double glow;                /* 0..1: grows while the app is open, shrinks once it closed */
} dslot_t;

static dslot_t dock[NDOCK];
static int dock_timer;
static uint32_t dock_tick;      /* g_ticks of the last animation step */
/* per pixel of a slot's glow box: distance to the outline, position round it (0..1) */
#define RING_W (SLOT_W + 2 * HALO)
#define RING_H (SLOT_H + 2 * HALO)
static float ring_d[RING_W * RING_H], ring_a[RING_W * RING_H];

static void ring_init(void) {
    double cx = RING_W / 2.0, cy = RING_H / 2.0, hx = SLOT_W / 2.0 - SLOT_R, hy = SLOT_H / 2.0 - SLOT_R;
    for (int y = 0; y < RING_H; y++)
        for (int x = 0; x < RING_W; x++) {
            double px = fabs(x + 0.5 - cx) - hx, py = fabs(y + 0.5 - cy) - hy;
            double ox = MAX(px, 0.0), oy = MAX(py, 0.0);
            double a = atan2(y + 0.5 - cy, x + 0.5 - cx) / (2 * M_PI);
            ring_d[y * RING_W + x] = (float)(sqrt(ox * ox + oy * oy) + MIN(MAX(px, py), 0.0) - SLOT_R);
            ring_a[y * RING_W + x] = (float)(a < 0 ? a + 1 : a);
        }
}

static rect_t slot_rect(int i) { return rect_make(DOCK_PAD + i * (SLOT_W + SLOT_GAP), (ISLAND_H - SLOT_H) / 2, SLOT_W, SLOT_H); }

static int slot_at(int x, int y) {
    for (int i = 0; i < NDOCK; i++)
        if (rect_contains(slot_rect(i), x, y)) return i;
    return -1;
}

/* Has the app a window (with an id above min_id)? opened: also fully faded in. */
static bool app_window(const app_desc_t *a, int min_id, bool opened) {
    if (!a) return false;
    for (window_t *w = wm_bottom; w; w = w->above) {
        if (w->app != a || w->closing || w->layer != LAYER_NORMAL || w->id <= min_id) continue;
        if ((w->flags & (WIN_DIALOG | WIN_NO_TASKBAR)) || (!w->visible && !w->minimized)) continue;
        if (opened && (w->minimized || w->opacity < 255)) continue;
        return true;
    }
    return false;
}

static void dock_animate(void) {
    if (!bars[BAR_DOCK]) return;
    if (!dock_timer) {
        dock_tick = g_ticks;
        dock_timer = win_set_timer(bars[BAR_DOCK], FRAME_MS);
    }
    win_invalidate(bars[BAR_DOCK]);
}

static void dock_launch(dslot_t *d) {
    d->launched = true;
    int maxid = 0;
    for (window_t *w = wm_bottom; w; w = w->above) maxid = MAX(maxid, w->id);
    d->min_id = maxid;
    app_launch(d->app, NULL);
    if (!app_window(d->app, d->min_id, false)) d->launching = false;   /* no new window: the spinner ends at once */
}

static void dock_click(int i) {
    dslot_t *d = &dock[i];
    if (!d->app || d->launching) return;
    /* launching would only focus the open window: do that, no spinner */
    if ((d->app->flags & APP_SINGLE) && app_window(d->app, 0, false)) { app_launch(d->app, NULL); return; }
    d->launching = true;
    d->launched = false;
    d->t0 = g_ticks;
    dock_animate();
}

/* advance the animations; true while any still runs */
static bool dock_step(void) {
    uint32_t now = g_ticks;
    double dt = MIN((double)(now - dock_tick), 50.0);   /* smooth after a long frame (an app starting) */
    dock_tick = now;
    bool anim = false;
    for (int i = 0; i < NDOCK; i++) {
        dslot_t *d = &dock[i];
        if (!d->app) continue;
        if (d->launching) {
            if (!d->launched) { if (now - d->t0 >= DARKEN_MS) dock_launch(d); }
            else if (app_window(d->app, d->min_id, true) || now - d->t0 > LAUNCH_TIMEOUT_MS) d->launching = false;
        }
        if (!d->launching) {
            if (app_window(d->app, 0, false)) d->glow = MIN(1.0, d->glow + dt / GLOW_IN_MS);
            else d->glow = MAX(0.0, d->glow - dt / GLOW_OUT_MS);
        }
        if (d->launching || d->glow > 0) anim = true;
    }
    return anim;
}

/* start the frame timer when an app of the rectangle opened or closed */
static void dock_check(void) {
    for (int i = 0; i < NDOCK; i++)
        if (dock[i].launching || dock[i].glow > 0 || app_window(dock[i].app, 0, false)) { dock_animate(); return; }
}

/* the glowing outline: a soft accent halo and a brighter light travelling round it */
static void draw_glow(surface_t *s, rect_t r, double level, uint32_t now, int idx) {
    uint32_t accent = theme_get()->accent;
    double e = level * level * (3 - 2 * level);                     /* smoothstep */
    double head = fmod(now / (double)LIGHT_MS + idx * 0.31, 1.0);
    double reach = 1.0 + (HALO - 1) * e;                            /* the halo grows in, shrinks out */
    for (int y = 0; y < RING_H; y++) {
        int sy = r.y - HALO + y;
        if (sy < 0 || sy >= s->h) continue;
        for (int x = 0; x < RING_W; x++) {
            double d = ring_d[y * RING_W + x];
            if (d < -2.5 || d > reach) continue;
            double behind = head - ring_a[y * RING_W + x];
            if (behind < 0) behind += 1;
            double light = MIN(1.0, exp(-behind * 4.5) + exp(-(1 - behind) * 30.0));
            double core = CLAMP(1.4 - fabs(d + 0.3), 0.0, 1.0);
            double halo = d > 0 ? (1 - d / reach) * (1 - d / reach) : 0;
            double a = e * (core * (0.5 + 0.5 * light) + halo * (0.3 + 0.7 * light));
            if (a < 0.01) continue;
            uint32_t c = color_mix(accent, COLOR_WHITE, (int)(light * 200));
            gfx_pixel(s, r.x - HALO + x, sy, color_alpha(c, (int)(MIN(a, 1.0) * 255)));
        }
    }
}

/* a white arc with a fading tail, turning */
static void draw_spinner(surface_t *s, double cx, double cy, double rad, double th, double head, int alpha) {
    int x0 = (int)(cx - rad - th) - 1, x1 = (int)(cx + rad + th) + 1, y0 = (int)(cy - rad - th) - 1, y1 = (int)(cy + rad + th) + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            double cov = th / 2 + 0.5 - fabs(sqrt(dx * dx + dy * dy) - rad);
            if (cov <= 0) continue;
            double a = atan2(dy, dx) / (2 * M_PI);
            double behind = head - a;
            behind -= floor(behind);
            if (behind > 0.78) continue;                           /* the gap */
            double f = 1 - behind / 0.78;
            gfx_pixel(s, x, y, RGBA(255, 255, 255, (int)(alpha * MIN(cov, 1.0) * (0.2 + 0.8 * f))));
        }
}

static void paint_dock(window_t *win) {
    island_bg(win);
    surface_t *s = win_surface(win);
    const theme_t *t = theme_get();
    uint32_t now = g_ticks;
    for (int i = 0; i < NDOCK; i++) {
        dslot_t *d = &dock[i];
        rect_t r = slot_rect(i);
        if (d->glow > 0) draw_glow(s, r, d->glow, now, i);
        if (hover_slot == i && !d->launching) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, SLOT_R, t->taskbar_hover);
        if (!d->app) continue;
        int ix = r.x + (r.w - DOCK_ICON) / 2, iy = r.y + (r.h - DOCK_ICON) / 2;
        const surface_t *img = app_icon(d->app, DOCK_ICON);
        if (img) gfx_blit_alpha(s, ix, iy, img, 255);
        if (d->launching) {
            /* the icon turns dark (its own shape), a spinner appears inside it */
            double p = MIN(1.0, (now - d->t0) / (double)DARKEN_MS);
            int dark = (int)(165 * p);
            for (int y = 0; img && y < img->h; y++)
                for (int x = 0; x < img->w; x++) {
                    int a = COLOR_A(surface_row(img, y)[x]) * dark / 255;
                    if (a) gfx_pixel(s, ix + x, iy + y, RGBA(0, 0, 0, a));
                }
            draw_spinner(s, r.x + r.w / 2.0, r.y + r.h / 2.0, 8.5, 2.6, fmod(now / (double)SPIN_MS, 1.0), (int)(255 * p));
        }
    }
}

/* ---- Start button and clock -------------------------------------------------------------- */
static void paint_start(window_t *win) {
    island_bg(win);
    surface_t *s = win_surface(win);
    const theme_t *t = theme_get();
    bool open = startmenu_is_open();
    if (hover_start || open) gfx_fill_rounded_rect(s, 3, 3, s->w - 6, s->h - 6, 9, open ? t->taskbar_active : t->taskbar_hover);
    ui_draw_logo(s, (s->w - 28) / 2, (s->h - 28) / 2, 28);
}

static void paint_clock(window_t *win) {
    island_bg(win);
    surface_t *s = win_surface(win);
    const theme_t *t = theme_get();
    if (hover_clock) gfx_fill_rounded_rect(s, 3, 3, s->w - 6, s->h - 6, 9, t->taskbar_hover);
    datetime_t dt;
    sys_datetime(&dt);
    last_minute = dt.minute;
    char tbuf[16], dbuf[32];
    snprintf(tbuf, sizeof tbuf, "%02d:%02d", dt.hour, dt.minute);
    snprintf(dbuf, sizeof dbuf, "%d %.3s %d", dt.day, sys_month_name(dt.month), dt.year);
    int fh = font_height(FONT_UI), tx = s->w - 10 - CLOCK_TXT_W;
    gfx_text_aligned(s, rect_make(tx, s->h / 2 - fh + 1, CLOCK_TXT_W, fh), tbuf, t->taskbar_text, FONT_UI_BOLD, ALIGN_RIGHT);
    gfx_text_aligned(s, rect_make(tx, s->h / 2 + 1, CLOCK_TXT_W, fh), dbuf,
                     color_mix(t->taskbar_text, t->dark ? COLOR_BLACK : COLOR_WHITE, 60), FONT_UI, ALIGN_RIGHT);
    /* developer editions: "Developer version 2.5", drawn last so it is always in
       front of the time; black, white in dark mode */
    const char *label = developer_label();
    if (label) gfx_text(s, 12, (s->h - fh) / 2, label, t->dark ? COLOR_WHITE : COLOR_BLACK, FONT_UI);
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
static void update_tip(void) {
    char buf[160];
    if (hover_btn >= 0 && hover_btn < nshown) {
        rect_t r = btn_rect(hover_btn);
        group_title(&groups[hover_btn], buf, sizeof buf);
        tip_show(buf, rect_make(bars[BAR_APPS]->frame.x + r.x, bars[BAR_APPS]->frame.y, r.w, r.h));
    } else if (hover_slot >= 0 && dock[hover_slot].app) {
        rect_t r = slot_rect(hover_slot);
        tip_show(dock[hover_slot].app->name, rect_make(bars[BAR_DOCK]->frame.x + r.x, bars[BAR_DOCK]->frame.y, r.w, r.h));
    } else {
        tip_hide();
    }
}

static void set_hover(int b, int x, int y) {
    bool hs = b == BAR_START, hc = b == BAR_CLOCK;
    int hb = -1, hsl = -1;
    if (b == BAR_APPS) { layout_apps(); hb = btn_at(x, y); }
    if (b == BAR_DOCK) hsl = slot_at(x, y);
    if (hs != hover_start) { hover_start = hs; win_invalidate(bars[BAR_START]); }
    if (hc != hover_clock) { hover_clock = hc; win_invalidate(bars[BAR_CLOCK]); }
    if (hb != hover_btn) { hover_btn = hb; win_invalidate(bars[BAR_APPS]); }
    if (hsl != hover_slot) { hover_slot = hsl; win_invalidate(bars[BAR_DOCK]); }
    update_tip();
}

static void bar_click(int b, const event_t *ev) {
    if (b == BAR_START) {
        if (ev->button == MOUSE_LEFT && !startmenu_just_closed()) startmenu_toggle();
    } else if (b == BAR_CLOCK) {
        if (ev->button == MOUSE_LEFT) show_calendar();
    } else if (b == BAR_DOCK) {
        int i = slot_at(ev->x, ev->y);
        if (i >= 0 && ev->button == MOUSE_LEFT) dock_click(i);   /* fixed: no drag, no menu */
    } else {
        layout_apps();
        int i = btn_at(ev->x, ev->y);
        if (i < 0) return;
        if (ev->button == MOUSE_LEFT) group_click(&groups[i]);
        else if (ev->button == MOUSE_RIGHT) apps_menu(i);
    }
}

static void bar_handler(window_t *win, const event_t *ev) {
    int b = -1;
    for (int i = 0; i < NBARS; i++)
        if (bars[i] == win) b = i;
    if (b < 0) return;
    switch (ev->type) {
    case EV_PAINT:
        if (b == BAR_START) paint_start(win);
        else if (b == BAR_APPS) paint_apps(win);
        else if (b == BAR_DOCK) paint_dock(win);
        else paint_clock(win);
        break;
    case EV_MOUSE_MOVE:
        set_hover(b, ev->x, ev->y);
        break;
    case EV_MOUSE_LEAVE:
        set_hover(-1, 0, 0);
        break;
    case EV_MOUSE_DOWN:
        tip_hide();
        bar_click(b, ev);
        break;
    case EV_TIMER:
        if (b == BAR_DOCK && ev->timer_id == dock_timer) {
            bool anim = dock_step();
            win_invalidate(win);
            if (!anim) { win_kill_timer(win, dock_timer); dock_timer = 0; }
        } else if (b == BAR_CLOCK) {
            datetime_t dt;
            sys_datetime(&dt);
            if (dt.minute != last_minute) win_invalidate(win);
        }
        break;
    default:
        break;
    }
}

bool taskbar_owns(const window_t *w) {
    if (!w) return false;
    if (w == tip_win) return true;
    for (int i = 0; i < NBARS; i++)
        if (bars[i] == w) return true;
    return false;
}

void taskbar_set_hidden(bool hide) {
    if (!bars[BAR_START] || bars_hidden == hide) return;
    bars_hidden = hide;
    for (int i = 0; i < NBARS; i++) set_visible(bars[i], !hide && (i != BAR_APPS || apps_shown));
    if (hide) tip_hide();
}

/* windows changed (opened, closed, focused, renamed, ...) */
void taskbar_refresh(void) {
    if (!bars[BAR_START]) return;
    layout_apps();
    window_t *aw = bars[BAR_APPS];
    if (nshown && aw->cw != apps_w()) win_resize(aw, apps_w(), ISLAND_H);
    win_move(aw, apps_x(), island_y());
    apps_shown = nshown > 0;
    set_visible(aw, apps_shown && !bars_hidden);
    if (hover_btn >= nshown) hover_btn = -1;
    dock_check();
    for (int i = 0; i < NBARS; i++) win_invalidate(bars[i]);
    if (tip_win) update_tip();      /* the hovered app's title may have changed */
}

void taskbar_init(void) {
    static const char *const titles[NBARS] = { "Start", "Running apps", "Apps", "Clock" };
    int xs[NBARS] = { ISLAND_GAP, apps_x(), dock_x(), screen_width() - ISLAND_GAP - clock_w() };
    int ws[NBARS] = { START_W, 2 * PAD + BTN_W, dock_w(), clock_w() };
    const app_desc_t *saved = wm_current_app;
    wm_current_app = NULL;
    for (int i = 0; i < NBARS; i++) {
        bars[i] = wm_create_ex(titles[i], ws[i], ISLAND_H,
                               WIN_FRAMELESS | WIN_NO_TASKBAR | WIN_NO_SHADOW | (i == BAR_APPS ? WIN_HIDDEN : 0), LAYER_TOPMOST);
        if (!bars[i]) kpanic("cannot create the bottom bar");
        bars[i]->radius = ISLAND_R;
        win_move(bars[i], xs[i], island_y());
        win_set_event_handler(bars[i], bar_handler);
    }
    wm_current_app = saved;
    for (int i = 0; i < NDOCK; i++) dock[i].app = app_find(dock_names[i]);
    ring_init();
    win_set_timer(bars[BAR_CLOCK], 1000);
    taskbar_wallpaper_changed();
    taskbar_refresh();
}
