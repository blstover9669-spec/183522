/*
 * system_settings.c - "Settings": the NovaOS control panel.
 *
 * A sidebar on the left switches between pages (click, or Up/Down while the
 * sidebar has the focus, or Ctrl+Tab / Ctrl+PgUp/PgDn from anywhere):
 *
 *   Appearance  - light/dark mode, accent colour, wallpaper (the built-in
 *                 styles as live thumbnails, solid colours or a picture from
 *                 the disk), window shadows. Everything goes through the core
 *                 theme API and is applied live to the whole desktop.
 *   Mouse       - pointer speed, double-click speed, a double-click / wheel
 *                 test area, restore defaults.
 *   Date & Time - live analog + digital clock, ISO week / day of year, month
 *                 calendar, clock details, shortcuts to Clock and Calendar.
 *   Sound       - system sounds on/off (the same switch as the taskbar speaker
 *                 icon), test sounds, PC speaker notes.
 *   SafetyShield - the threat scanner (kernel/services/safetyshield.c): the
 *                 risk levels and what they mean, a Scan button, the
 *                 "Recent scanned files" list and a bar with the files
 *                 scanned / all files in the OS (empty until a scan runs).
 *   About       - version, processor (CPUID vendor, family/model/stepping,
 *                 feature flags, hypervisor), memory, display, storage,
 *                 uptime; "Copy details" puts it all on the clipboard.
 *
 * Settings are remembered in /system/config/settings. An INIT_FS hook applies them again
 * at boot (an installed system keeps them across restarts) and a small
 * background timer notices changes made elsewhere (desktop menu, taskbar mute
 * button) and saves those too. Options given on the kernel command line
 * (dark=, wallpaper=, mousespeed=) win over saved settings.
 *
 * autorun=Settings:mouse opens a page directly (appearance, mouse, time,
 * sound, safetyshield, about).
 */
#include "system_common.h"
#include <nova/disk.h>
#include <nova/safetyshield.h>

#define SW       780
#define SH       520
#define SIDE_W   200
#define PAD      32
#define CX       (SIDE_W + PAD)
#define CW       (SW - CX - PAD)
#define NAV_Y    72
#define NAV_H    40
#define MAX_PW   16

/* appearance layout */
#define MODE_Y   122
#define MODE_H   88
#define ACC_Y    250
#define ACC_D    30
#define ACC_GAP  14
#define WP_Y     320
#define WP_TW    77
#define WP_TH    56
#define WP_GAP   10
#define SOL_Y    414
#define SOL_X    (CX + 100)
#define SOL_D    22
#define SOL_GAP  10
#define NSOLID   8

/* mouse layout */
#define SPD_Y    144
#define DCL_Y    248
#define TEST_Y   330
#define TEST_H   106
#define SLIDER_W 380

/* date & time layout */
#define CLK_CX   (CX + 74)
#define CLK_CY   178
#define CLK_R    72
#define CARD_Y   270
#define CARD_H   220

/* sound / about layout */
#define SPK_X    CX
#define SPK_Y    100
#define SPK_S    104
#define INFO_Y   174
#define INFO_ROW 23

/* safetyshield layout */
#define RISK_Y   124
#define RISK_H   28
#define RISK_W   168
#define SCAN_Y   338
#define LINK_Y   380
#define COUNT_Y  452
#define BAR_Y    476
#define BAR_H    12
#define REC_TOP  64
#define REC_ROW  46

enum { PG_APPEARANCE, PG_MOUSE, PG_DATETIME, PG_SOUND, PG_SAFETY, PG_ABOUT, PG_COUNT };

static const char *const page_names[PG_COUNT] = { "Appearance", "Mouse", "Date & Time", "Sound", "SafetyShield",
                                                  "About" };
static const char *const page_subtitles[PG_COUNT] = {
    "Choose light or dark mode, an accent colour and a wallpaper.",
    "Adjust how the pointer and the mouse buttons respond.",
    "The system clock, read from the computer's real-time clock.",
    "System sounds, played on the PC speaker.",
    "Scans every file in NovaOS for threats automatically every 1 minute.",
    "Information about NovaOS and this computer.",
};
static const char *const page_keys[PG_COUNT] = { "appearance", "mouse", "time", "sound", "safetyshield", "about" };

static const uint32_t solid_colors[NSOLID] = {
    RGB(40, 90, 150), RGB(24, 118, 128), RGB(44, 112, 72), RGB(106, 64, 140),
    RGB(150, 48, 72), RGB(58, 62, 72), RGB(176, 140, 92), RGB(200, 206, 216),
};

typedef struct { const char *label; note_t notes[8]; int n; } sound_def_t;
static const sound_def_t sounds[] = {
    { "Beep",   { { 880, 140 } }, 1 },
    { "Chime",  { { 1047, 90 }, { 1319, 90 }, { 1568, 180 } }, 3 },
    { "Alert",  { { 988, 110 }, { 0, 60 }, { 988, 110 }, { 0, 60 }, { 988, 110 } }, 5 },
    { "Jingle", { { 523, 110 }, { 659, 110 }, { 784, 110 }, { 1047, 220 }, { 784, 110 }, { 1047, 320 } }, 6 },
};
#define NSOUNDS ((int)(sizeof sounds / sizeof sounds[0]))

typedef struct {
    window_t *win;
    int page;
    widget_t *side;
    widget_t *pw[PG_COUNT][MAX_PW];
    int pn[PG_COUNT];
    int timer;
    int hov_side;
    uint32_t last_sec;
    /* appearance */
    widget_t *w_mode, *w_accent, *w_wall, *w_solid, *w_shadows, *w_picture, *w_saver;
    int saver_custom;               /* timeout of the extra "N seconds" dropdown item, 0 = none */
    int hov_mode, hov_accent, hov_wall, hov_solid;
    surface_t *thumb[8];
    int nthumb;
    uint32_t thumb_accent, thumb_color;
    int last_wallpaper;
    /* mouse */
    widget_t *w_speed, *w_dclick, *w_test;
    bool folder_open;
    int wheel_pos;
    /* sound */
    widget_t *w_sound, *w_snd[NSOUNDS];
    uint32_t snd_until;
    /* safetyshield */
    widget_t *w_scan, *w_link, *w_risks, *w_recent;
    uint32_t ss_base_id;            /* scan id when the window opened: the bar stays empty until a scan runs */
    uint32_t ss_gen;
    int risk_sel, hov_risk;
    bool recent_open;
    int rec_scroll, rec_hov, rec_drag_y, rec_drag_scroll;
    bool rec_hov_x, rec_dragging;
    int fast;                       /* fast timer while a scan runs on screen */
    /* about */
    sys_cpu_info_t cpu;
} st_t;

/* ======================================================================================
 * Persistence: /system/config/settings, applied at boot, watched for changes made elsewhere
 * ==================================================================================== */
typedef struct {
    int dark, accent, wallpaper, wallcolor, shadows, mouse, dclick, sound;
} snap_t;

static snap_t g_saved;
static bool g_saved_valid;
static char g_wall_image[VFS_PATH_MAX];   /* picture chosen in Settings (wallpaper -1) */

static void snap_take(snap_t *s) {
    const theme_t *t = theme_get();
    memset(s, 0, sizeof *s);
    s->dark = t->dark;
    s->accent = (int)(t->accent & 0xFFFFFF);
    s->wallpaper = t->wallpaper;
    s->wallcolor = (int)(t->wallpaper_color & 0xFFFFFF);
    s->shadows = t->shadows;
    s->mouse = sys_get_mouse_speed();
    s->dclick = sys_get_double_click_ms();
    s->sound = sys_sound_enabled();
}

static void settings_save(void) {
    snap_t s;
    snap_take(&s);
    sys_cfg_t *c = malloc(sizeof *c);
    if (!c) return;
    sys_cfg_load(c);
    sys_cfg_set_int(c, "dark", s.dark);
    sys_cfg_set_hex(c, "accent", (uint32_t)s.accent);
    sys_cfg_set_int(c, "wallpaper", s.wallpaper);
    sys_cfg_set_hex(c, "wallcolor", (uint32_t)s.wallcolor);
    sys_cfg_set(c, "wallimage", g_wall_image);
    sys_cfg_set_int(c, "shadows", s.shadows);
    sys_cfg_set_int(c, "mousespeed", s.mouse);
    sys_cfg_set_int(c, "dclick", s.dclick);
    sys_cfg_set_int(c, "sound", s.sound);
    int r = sys_cfg_save(c);
    free(c);
    if (r < 0) klog("settings: cannot save %s: %s", SYS_CFG_PATH, vfs_strerror(r));
    g_saved = s;
    g_saved_valid = true;
}

/* Save if anything changed since the last save (called by a 2 s timer and after every
   change made in the Settings window). */
static void settings_watch(void *ctx) {
    (void)ctx;
    snap_t s;
    snap_take(&s);
    if (!g_saved_valid) { g_saved = s; g_saved_valid = true; return; }
    if (g_saved.wallpaper == -1 && s.wallpaper != -1) g_wall_image[0] = 0;
    if (memcmp(&s, &g_saved, sizeof s)) settings_save();
}

static bool load_wall_image(const char *path) {
    if (!path || !*path) return false;
    uint32_t size = 0;
    void *d = vfs_read_file(path, &size);
    if (!d) return false;
    surface_t *img = image_decode(d, size);
    free(d);
    if (!img) return false;
    theme_set_wallpaper_image(img);
    surface_destroy(img);
    return true;
}

static void apply_cfg(const sys_cfg_t *c) {
    const theme_t *t = theme_get();
    int v;
    if (!sys_cmdline_get("dark") && (v = sys_cfg_get_int(c, "dark", -1)) >= 0 && (v != 0) != t->dark)
        theme_set_dark(v != 0);
    if (sys_cfg_get(c, "accent")) {
        uint32_t a = ((uint32_t)sys_cfg_get_int(c, "accent", 0) & 0xFFFFFF) | 0xFF000000u;
        if (a != (t->accent | 0xFF000000u)) theme_set_accent(a);
    }
    if (sys_cfg_get(c, "wallcolor")) {
        uint32_t wc = ((uint32_t)sys_cfg_get_int(c, "wallcolor", 0) & 0xFFFFFF) | 0xFF000000u;
        if (wc != (t->wallpaper_color | 0xFF000000u)) theme_set_wallpaper_color(wc);
    }
    const char *img = sys_cfg_get(c, "wallimage");
    if (img) strlcpy(g_wall_image, img, sizeof g_wall_image);
    if (!sys_cmdline_get("wallpaper") && sys_cfg_get(c, "wallpaper")) {
        int wp = sys_cfg_get_int(c, "wallpaper", 0);
        if (wp == -1) {
            if (!load_wall_image(g_wall_image)) g_wall_image[0] = 0;
        } else if (wp >= 0 && wp < theme_wallpaper_count() && wp != t->wallpaper) {
            theme_set_wallpaper(wp);
        }
    }
    if ((v = sys_cfg_get_int(c, "shadows", -1)) >= 0 && (v != 0) != t->shadows) theme_set_shadows(v != 0);
    if (!sys_cmdline_get("mousespeed") && (v = sys_cfg_get_int(c, "mousespeed", 0)) >= 1 && v <= 10)
        sys_set_mouse_speed(v);
    if ((v = sys_cfg_get_int(c, "dclick", 0)) >= 150 && v <= 1500) sys_set_double_click_ms(v);
    if ((v = sys_cfg_get_int(c, "sound", -1)) >= 0) sys_set_sound_enabled(v != 0);
}

static void settings_boot(void) {
    sys_cfg_t *c = malloc(sizeof *c);
    if (c) {
        sys_cfg_load(c);
        if (c->n) {
            apply_cfg(c);
            klog("settings: applied %d saved settings from %s", c->n, SYS_CFG_PATH);
        }
        free(c);
    }
    snap_take(&g_saved);
    g_saved_valid = true;
    timer_add(2000, settings_watch, NULL, true);
}
REGISTER_INIT(INIT_FS, settings_boot);

/* ======================================================================================
 * Drawing helpers
 * ==================================================================================== */
static uint32_t side_bg(const theme_t *t) {
    return t->dark ? color_mix(t->window_bg, COLOR_BLACK, 50) : color_mix(t->window_bg, RGB(214, 221, 234), 130);
}

static uint32_t card_color(const theme_t *t) {
    return t->dark ? color_mix(t->window_bg, COLOR_WHITE, 10) : COLOR_WHITE;
}

static void section(surface_t *s, int x, int y, const char *title) {
    gfx_text(s, x, y, title, theme_get()->window_text, FONT_UI_BOLD);
}

static void card(surface_t *s, rect_t r) {
    const theme_t *t = theme_get();
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 10, t->window_border);
    gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 9, card_color(t));
}

/* selectable card: accent border when selected, hint of accent on hover */
static void choice_card(surface_t *s, rect_t c, bool sel, bool hover, bool focus) {
    const theme_t *t = theme_get();
    uint32_t border = sel ? t->accent : hover ? color_mix(t->window_border, t->accent, 140) : t->window_border;
    int bw = sel ? 2 : 1;
    gfx_fill_rounded_rect(s, c.x, c.y, c.w, c.h, 10, border);
    gfx_fill_rounded_rect(s, c.x + bw, c.y + bw, c.w - 2 * bw, c.h - 2 * bw, 10 - bw, card_color(t));
    if (focus) gfx_rounded_rect(s, c.x - 3, c.y - 3, c.w + 6, c.h + 6, 12, color_alpha(t->accent, 150));
}

static void check_badge(surface_t *s, int cx, int cy, int r) {
    const theme_t *t = theme_get();
    gfx_fill_circle_aa(s, cx, cy, r, t->accent);
    sys_check(s, cx - r * 2 / 3, cy - r * 2 / 3, r * 4 / 3, t->accent_text);
}

/* Gear outline: n teeth between radius rr (root) and ro (tip). */
static void draw_gear(surface_t *s, double cx, double cy, double ro, double rr, int n, uint32_t c) {
    point_t pts[64];
    int k = 0;
    double step = 2 * M_PI / n;
    for (int i = 0; i < n && k + 4 <= 64; i++) {
        double a = i * step - M_PI / 2;
        double ang[4] = { a - step * 0.30, a - step * 0.16, a + step * 0.16, a + step * 0.30 };
        double rad[4] = { rr, ro, ro, rr };
        for (int j = 0; j < 4; j++) {
            pts[k].x = (int)lround(cx + rad[j] * cos(ang[j]));
            pts[k].y = (int)lround(cy + rad[j] * sin(ang[j]));
            k++;
        }
    }
    gfx_fill_polygon(s, pts, k, c);
    gfx_fill_circle_aa(s, cx, cy, rr, c);
}

/* App icon: white gear on a slate tile. */
static void settings_icon(surface_t *s, int x, int y, int size) {
    double u = size / 48.0;
    int m = (int)(2 * u);
    sys_tile(s, x + m, y + m, size - 2 * m, RGB(138, 148, 168), RGB(74, 82, 102));
    double cx = x + size / 2.0, cy = y + size / 2.0;
    draw_gear(s, cx, cy + 1.2 * u, 16.5 * u, 12.5 * u, 8, RGBA(20, 24, 36, 90));
    draw_gear(s, cx, cy, 16.5 * u, 12.5 * u, 8, RGB(244, 246, 250));
    gfx_fill_circle_aa(s, cx, cy, 8.5 * u, RGB(196, 202, 214));
    gfx_fill_circle_aa(s, cx, cy, 5.5 * u, RGB(92, 100, 120));
}

/* Small coloured tile + white glyph for each page (sidebar, drawn directly at sz px). */
static void page_icon(surface_t *s, int page, int x, int y, int sz) {
    static const uint32_t top[PG_COUNT] = { RGB(176, 116, 250), RGB(70, 156, 252), RGB(252, 168, 64),
                                            RGB(250, 98, 126), RGB(70, 170, 252), RGB(146, 154, 170) };
    static const uint32_t bot[PG_COUNT] = { RGB(120, 72, 222), RGB(36, 104, 228), RGB(236, 112, 30),
                                            RGB(222, 52, 88), RGB(36, 128, 232), RGB(96, 104, 122) };
    if (page == PG_SAFETY) { icon_draw(ICON_SHIELD, s, x, y, sz); return; }
    sys_tile(s, x, y, sz, top[page], bot[page]);
    double cx = x + sz / 2.0, cy = y + sz / 2.0, u = sz / 22.0;
    uint32_t W = COLOR_WHITE;
    switch (page) {
    case PG_APPEARANCE: {
        double r = 6.5 * u;
        gfx_circle_aa(s, cx, cy, r, 1.6 * u, W);
        rect_t old = s->clip;
        s->clip = rect_intersect(old, rect_make(x, y, (int)cx - x, sz));
        gfx_fill_circle_aa(s, cx, cy, r, W);
        s->clip = old;
        break;
    }
    case PG_MOUSE: {
        int mw = (int)lround(9 * u), mh = (int)lround(14 * u);
        int mx = (int)lround(cx - mw / 2.0), my = (int)lround(cy - mh / 2.0);
        gfx_fill_rounded_rect(s, mx, my, mw, mh, mw / 2, W);
        gfx_fill_rect(s, mx + mw / 2, my + 1, MAX(1, (int)u), (int)(5 * u), bot[page]);
        gfx_fill_rect(s, mx, my + (int)(5 * u), mw, MAX(1, (int)u), bot[page]);
        break;
    }
    case PG_DATETIME:
        gfx_circle_aa(s, cx, cy, 7 * u, 1.7 * u, W);
        sys_stroke(s, cx, cy, cx, cy - 4.4 * u, 1.6 * u, W);
        sys_stroke(s, cx, cy, cx + 3.4 * u, cy + 1.6 * u, 1.6 * u, W);
        break;
    case PG_SOUND: {
        gfx_fill_rect(s, (int)(x + 4 * u), (int)(cy - 2.5 * u), (int)(3.5 * u), (int)(5 * u) + 1, W);
        point_t p[4] = { { (int)(x + 7 * u), (int)(cy - 2.5 * u) }, { (int)(x + 11 * u), (int)(cy - 6.5 * u) },
                         { (int)(x + 11 * u), (int)(cy + 6.5 * u) + 1 }, { (int)(x + 7 * u), (int)(cy + 2.5 * u) + 1 } };
        gfx_fill_polygon(s, p, 4, W);
        sys_arc(s, x + 11 * u, cy, 3.6 * u, -0.85, 0.85, 1.4 * u, W);
        sys_arc(s, x + 11 * u, cy, 6.6 * u, -0.85, 0.85, 1.4 * u, W);
        break;
    }
    default:
        gfx_fill_circle_aa(s, cx, y + 6.2 * u, 1.8 * u, W);
        gfx_fill_rounded_rect(s, (int)lround(cx - 1.6 * u), (int)lround(y + 9.4 * u), (int)lround(3.2 * u),
                              (int)lround(8 * u), 1, W);
        break;
    }
}

static void fmt_uptime(char *buf, int n) {
    uint32_t up = sys_uptime_ms() / 1000;
    if (up >= 86400)
        snprintf(buf, (size_t)n, "%ud %u:%02u:%02u", up / 86400, (up / 3600) % 24, (up / 60) % 60, up % 60);
    else
        snprintf(buf, (size_t)n, "%u:%02u:%02u", up / 3600, (up / 60) % 60, up % 60);
}

static const char *vendor_name(const char *v) {
    if (!strcmp(v, "GenuineIntel")) return "Intel";
    if (!strcmp(v, "AuthenticAMD")) return "AMD";
    if (!strcmp(v, "CentaurHauls") || !strcmp(v, "VIA VIA VIA ")) return "VIA / Centaur";
    if (!strcmp(v, "HygonGenuine")) return "Hygon";
    if (!strcmp(v, "CyrixInstead")) return "Cyrix";
    if (!strcmp(v, "GenuineTMx86")) return "Transmeta";
    return NULL;
}

/* ======================================================================================
 * Page switching
 * ==================================================================================== */
static void add_pw(st_t *st, int page, widget_t *w) {
    if (w && st->pn[page] < MAX_PW) st->pw[page][st->pn[page]++] = w;
}

static void show_recent(st_t *st, bool on);
static void update_fast_timer(st_t *st);

static void set_page(st_t *st, int p) {
    if (p < 0 || p >= PG_COUNT) return;
    st->page = p;
    for (int i = 0; i < PG_COUNT; i++)
        for (int k = 0; k < st->pn[i]; k++) ui_set_visible(st->pw[i][k], i == p && !(i == PG_SAFETY && st->recent_open));
    if (st->recent_open && p != PG_SAFETY) show_recent(st, false);
    st->last_sec = 0;
    update_fast_timer(st);
    win_invalidate(st->win);
}

/* ======================================================================================
 * Sidebar (custom widget)
 * ==================================================================================== */
static rect_t nav_rect(int i) { return rect_make(10, NAV_Y + i * (NAV_H + 4), SIDE_W - 20, NAV_H); }

static int nav_at(int x, int y) {
    for (int i = 0; i < PG_COUNT; i++)
        if (rect_contains(nav_rect(i), x, y)) return i;
    return -1;
}

static void side_draw(widget_t *w, surface_t *s, rect_t r) {
    st_t *st = ui_get_ctx(w);
    const theme_t *t = theme_get();
    uint32_t bg = side_bg(t);
    gfx_fill_rect(s, r.x, r.y, r.w, r.h, bg);
    gfx_vline(s, r.x + r.w - 1, r.y, r.h, color_mix(bg, t->window_text, t->dark ? 40 : 34));
    const app_desc_t *me = app_find("Settings");
    if (me) app_draw_icon(s, me, 18, 20, 32);
    gfx_text(s, 60, 26, "Settings", t->window_text, FONT_UI_LARGE);
    int mx, my, hov = -1;
    if (sys_mouse_in(st->win, r, &mx, &my)) hov = nav_at(mx, my);
    bool focus = ui_has_focus(w) && win_is_focused(st->win);
    for (int i = 0; i < PG_COUNT; i++) {
        rect_t ir = nav_rect(i);
        bool sel = i == st->page;
        if (sel) {
            gfx_fill_rounded_rect(s, ir.x, ir.y, ir.w, ir.h, 8, color_alpha(t->accent, t->dark ? 70 : 42));
            gfx_fill_rounded_rect(s, ir.x + 2, ir.y + 11, 4, ir.h - 22, 2, t->accent);
            if (focus) gfx_rounded_rect(s, ir.x - 1, ir.y - 1, ir.w + 2, ir.h + 2, 9, t->accent);
        } else if (i == hov) {
            gfx_fill_rounded_rect(s, ir.x, ir.y, ir.w, ir.h, 8, color_alpha(t->window_text, t->dark ? 22 : 16));
        }
        page_icon(s, i, ir.x + 14, ir.y + 9, 22);
        gfx_text(s, ir.x + 48, ir.y + 11, page_names[i], t->window_text, sel ? FONT_UI_BOLD : FONT_UI);
    }
    char buf[64];
    snprintf(buf, sizeof buf, "%s", sys_version());
    gfx_text(s, 20, r.y + r.h - 34, buf, sys_text_dim(t), FONT_UI);
}

static bool side_event(widget_t *w, const event_t *ev) {
    st_t *st = ui_get_ctx(w);
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int h = nav_at(ev->x, ev->y);
        if (h != st->hov_side) { st->hov_side = h; ui_invalidate(w); }
        return true;
    }
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_LEFT) {
            int h = nav_at(ev->x, ev->y);
            if (h >= 0 && h != st->page) set_page(st, h);
        }
        return true;
    case EV_MOUSE_UP:
        return true;
    case EV_KEY_DOWN:
        if (ev->mods & (MOD_CTRL | MOD_ALT)) return false;
        if (ev->key == KEY_UP) { set_page(st, (st->page + PG_COUNT - 1) % PG_COUNT); return true; }
        if (ev->key == KEY_DOWN) { set_page(st, (st->page + 1) % PG_COUNT); return true; }
        if (ev->key == KEY_HOME) { set_page(st, 0); return true; }
        if (ev->key == KEY_END) { set_page(st, PG_COUNT - 1); return true; }
        return false;
    default:
        return false;
    }
}

/* ======================================================================================
 * Appearance: mode cards
 * ==================================================================================== */
static rect_t mode_rect(int i) {
    int w = (CW - 16) / 2;
    return rect_make(CX + i * (w + 16), MODE_Y, w, MODE_H);
}

static int mode_at(int x, int y) {
    for (int i = 0; i < 2; i++)
        if (rect_contains(mode_rect(i), x, y)) return i;
    return -1;
}

/* Miniature desktop + window in light or dark style. */
static void draw_mode_preview(surface_t *s, int x, int y, int w, int h, bool dark, uint32_t accent) {
    gfx_fill_rounded_gradient_v(s, x, y, w, h, 7, dark ? RGB(44, 50, 86) : RGB(128, 160, 236),
                                dark ? RGB(22, 22, 40) : RGB(196, 150, 226));
    int wx = x + w * 16 / 100, wy = y + h * 17 / 100, ww = w * 70 / 100, wh = h * 66 / 100;
    uint32_t body = dark ? RGB(32, 33, 37) : RGB(248, 249, 251);
    uint32_t title = dark ? RGB(48, 50, 57) : RGB(226, 230, 238);
    uint32_t line = dark ? RGB(110, 114, 124) : RGB(170, 176, 188);
    gfx_shadow(s, wx, wy, ww, wh, 4, 4, 70);
    gfx_fill_rounded_rect(s, wx, wy, ww, wh, 4, title);
    gfx_fill_rounded_rect(s, wx, wy + 9, ww, wh - 9, 4, body);
    gfx_fill_rect(s, wx, wy + 9, ww, 4, body);
    gfx_fill_circle_aa(s, wx + ww - 7, wy + 4.5, 2, dark ? RGB(150, 154, 164) : RGB(140, 146, 158));
    gfx_fill_rounded_rect(s, wx + 7, wy + 16, ww * 55 / 100, 4, 2, line);
    gfx_fill_rounded_rect(s, wx + 7, wy + 25, ww * 38 / 100, 4, 2, line);
    gfx_fill_rounded_rect(s, wx + ww - 27, wy + wh - 13, 20, 8, 3, accent);
    /* taskbar */
    gfx_fill_rect(s, x + 4, y + h - 9, w - 8, 6, dark ? RGBA(16, 18, 24, 190) : RGBA(246, 248, 252, 200));
    gfx_fill_circle_aa(s, x + 9, y + h - 6, 2, accent);
}

static void mode_draw(widget_t *w, surface_t *s, rect_t r) {
    st_t *st = ui_get_ctx(w);
    const theme_t *t = theme_get();
    int mx, my, hov = -1;
    if (sys_mouse_in(st->win, r, &mx, &my)) hov = mode_at(mx, my);
    bool focus = ui_has_focus(w) && win_is_focused(st->win);
    for (int i = 0; i < 2; i++) {
        rect_t c = mode_rect(i);
        bool sel = (i == 1) == t->dark;
        choice_card(s, c, sel, i == hov, sel && focus);
        draw_mode_preview(s, c.x + 10, c.y + 10, 108, c.h - 20, i == 1, t->accent);
        gfx_text(s, c.x + 132, c.y + 26, i ? "Dark" : "Light", t->window_text, FONT_UI_BOLD);
        gfx_text(s, c.x + 132, c.y + 46, i ? "Easy on the eyes" : "Bright and crisp", sys_text_dim(t), FONT_UI);
        if (sel) check_badge(s, c.x + c.w - 20, c.y + 20, 9);
    }
}

static bool mode_event(widget_t *w, const event_t *ev) {
    st_t *st = ui_get_ctx(w);
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int h = mode_at(ev->x, ev->y);
        if (h != st->hov_mode) { st->hov_mode = h; ui_invalidate(w); }
        return true;
    }
    case EV_MOUSE_DOWN: {
        int h = mode_at(ev->x, ev->y);
        if (ev->button == MOUSE_LEFT && h >= 0 && (h == 1) != theme_get()->dark) {
            theme_set_dark(h == 1);
            settings_watch(NULL);
        }
        return true;
    }
    case EV_MOUSE_UP:
        return true;
    case EV_KEY_DOWN:
        if (ev->mods & (MOD_CTRL | MOD_ALT)) return false;
        if (ev->key == KEY_LEFT || ev->key == KEY_RIGHT || ev->key == KEY_SPACE) {
            bool dark = ev->key == KEY_RIGHT ? true : ev->key == KEY_LEFT ? false : !theme_get()->dark;
            if (dark != theme_get()->dark) { theme_set_dark(dark); settings_watch(NULL); }
            return true;
        }
        return false;
    default:
        return false;
    }
}

/* ======================================================================================
 * Appearance: accent colours
 * ==================================================================================== */
static rect_t accent_rect(int i) { return rect_make(CX + i * (ACC_D + ACC_GAP), ACC_Y, ACC_D, ACC_D); }

static int accent_at(int x, int y) {
    for (int i = 0; i < theme_accent_count(); i++)
        if (rect_contains(rect_inset(accent_rect(i), -5), x, y)) return i;
    return -1;
}

static int accent_current(void) {
    uint32_t a = theme_get()->accent | 0xFF000000u;
    for (int i = 0; i < theme_accent_count(); i++)
        if ((theme_accent_preset(i) | 0xFF000000u) == a) return i;
    return -1;
}

static void set_accent_index(int i) {
    int n = theme_accent_count();
    if (n <= 0) return;
    i = (i % n + n) % n;
    if (i == accent_current()) return;
    theme_set_accent(theme_accent_preset(i));
    settings_watch(NULL);
}

static void accent_draw(widget_t *w, surface_t *s, rect_t r) {
    st_t *st = ui_get_ctx(w);
    const theme_t *t = theme_get();
    int mx, my, hov = -1;
    if (sys_mouse_in(st->win, r, &mx, &my)) hov = accent_at(mx, my);
    int cur = accent_current(), n = theme_accent_count();
    bool focus = ui_has_focus(w) && win_is_focused(st->win);
    for (int i = 0; i < n; i++) {
        rect_t a = accent_rect(i);
        uint32_t c = theme_accent_preset(i) | 0xFF000000u;
        double cx = a.x + ACC_D / 2.0, cy = a.y + ACC_D / 2.0, rad = ACC_D / 2.0;
        if (i == cur) gfx_circle_aa(s, cx, cy, rad + 4.5, 2.2, c);
        else if (i == hov) gfx_circle_aa(s, cx, cy, rad + 4.5, 2.2, color_alpha(c, 110));
        if (focus && i == (cur >= 0 ? cur : 0)) gfx_circle_aa(s, cx, cy, rad + 8, 1.5, color_alpha(t->window_text, 120));
        gfx_fill_circle_aa(s, cx, cy, rad, c);
        gfx_circle_aa(s, cx, cy, rad - 0.5, 1, RGBA(0, 0, 0, 28));
        if (i == cur) sys_check(s, (int)cx - 8, (int)cy - 8, 16, color_luma(c) > 170 ? RGB(20, 20, 24) : COLOR_WHITE);
    }
    const char *name = cur >= 0 ? theme_accent_name(cur) : "Custom";
    if (hov >= 0) name = theme_accent_name(hov);
    gfx_text(s, CX + n * (ACC_D + ACC_GAP) + 2, ACC_Y + 7, name, hov >= 0 && hov != cur ? sys_text_dim(t) : t->window_text,
             FONT_UI);
}

static bool accent_event(widget_t *w, const event_t *ev) {
    st_t *st = ui_get_ctx(w);
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int h = accent_at(ev->x, ev->y);
        if (h != st->hov_accent) { st->hov_accent = h; ui_invalidate(w); }
        return true;
    }
    case EV_MOUSE_DOWN: {
        int h = accent_at(ev->x, ev->y);
        if (ev->button == MOUSE_LEFT && h >= 0) set_accent_index(h);
        return true;
    }
    case EV_MOUSE_UP:
        return true;
    case EV_KEY_DOWN:
        if (ev->mods & (MOD_CTRL | MOD_ALT)) return false;
        if (ev->key == KEY_LEFT) { set_accent_index(accent_current() - 1); return true; }
        if (ev->key == KEY_RIGHT) { set_accent_index(accent_current() + 1); return true; }
        return false;
    default:
        return false;
    }
}

/* ======================================================================================
 * Appearance: wallpapers
 * ==================================================================================== */
static surface_t *make_thumb(int idx) {
    surface_t *big = surface_create(WP_TW * 2, WP_TH * 2);
    if (!big) return NULL;
    theme_render_wallpaper(big, idx);
    surface_t *small = surface_scaled(big, WP_TW, WP_TH);
    surface_destroy(big);
    if (!small) return NULL;
    surface_t *out = surface_create_alpha(WP_TW, WP_TH);
    if (!out) { surface_destroy(small); return NULL; }
    gfx_fill_rounded_rect(out, 0, 0, WP_TW, WP_TH, 7, COLOR_WHITE);     /* rounded mask */
    for (int y = 0; y < WP_TH; y++) {
        uint32_t *o = surface_row(out, y);
        const uint32_t *p = surface_row(small, y);
        for (int x = 0; x < WP_TW; x++) o[x] = (p[x] & 0xFFFFFFu) | (o[x] & 0xFF000000u);
    }
    surface_destroy(small);
    return out;
}

/* (Re)render thumbnails; the Midnight style follows the accent, Solid colour the colour. */
static void refresh_thumbs(st_t *st, bool all) {
    const theme_t *t = theme_get();
    int n = MIN(theme_wallpaper_count(), 8);
    for (int i = 0; i < n; i++) {
        const char *name = theme_wallpaper_name(i);
        bool dyn = !strcmp(name, "Midnight") || i == n - 1;
        if (!all && !dyn) continue;
        if (st->thumb[i] && !all) {
            if (!strcmp(name, "Midnight") && st->thumb_accent == t->accent) continue;
            if (i == n - 1 && st->thumb_color == t->wallpaper_color) continue;
        }
        surface_destroy(st->thumb[i]);
        st->thumb[i] = make_thumb(i);
    }
    st->nthumb = n;
    st->thumb_accent = t->accent;
    st->thumb_color = t->wallpaper_color;
}

static rect_t wall_rect(int i) { return rect_make(CX + i * (WP_TW + WP_GAP), WP_Y, WP_TW, WP_TH); }

static int wall_at(st_t *st, int x, int y) {
    for (int i = 0; i < st->nthumb; i++)
        if (rect_contains(rect_inset(wall_rect(i), -3), x, y)) return i;
    return -1;
}

static void set_wallpaper_index(st_t *st, int i) {
    if (i < 0 || i >= st->nthumb || i == theme_get()->wallpaper) return;
    theme_set_wallpaper(i);
    settings_watch(NULL);
    st->last_wallpaper = i;
    win_invalidate(st->win);
}

static void wall_draw(widget_t *w, surface_t *s, rect_t r) {
    st_t *st = ui_get_ctx(w);
    const theme_t *t = theme_get();
    int mx, my, hov = -1;
    if (sys_mouse_in(st->win, r, &mx, &my)) hov = wall_at(st, mx, my);
    bool focus = ui_has_focus(w) && win_is_focused(st->win);
    for (int i = 0; i < st->nthumb; i++) {
        rect_t a = wall_rect(i);
        bool sel = t->wallpaper == i;
        if (sel || i == hov) {
            gfx_fill_rounded_rect(s, a.x - 4, a.y - 4, a.w + 8, a.h + 8, 11, sel ? t->accent : color_alpha(t->accent, 110));
            gfx_fill_rounded_rect(s, a.x - 2, a.y - 2, a.w + 4, a.h + 4, 9, t->window_bg);
        }
        if (focus && (sel || (t->wallpaper < 0 && i == 0)))
            gfx_rounded_rect(s, a.x - 6, a.y - 6, a.w + 12, a.h + 12, 12, color_alpha(t->window_text, 110));
        if (st->thumb[i]) gfx_blit_alpha(s, a.x, a.y, st->thumb[i], 255);
        else gfx_fill_rounded_rect(s, a.x, a.y, a.w, a.h, 7, t->panel_bg);
        gfx_rounded_rect(s, a.x, a.y, a.w, a.h, 7, RGBA(0, 0, 0, 40));
        gfx_text_aligned(s, rect_make(a.x - 5, a.y + a.h + 6, a.w + 10, 17), theme_wallpaper_name(i),
                         sel ? t->window_text : sys_text_dim(t), sel ? FONT_UI_BOLD : FONT_UI, ALIGN_CENTER);
    }
}

static bool wall_event(widget_t *w, const event_t *ev) {
    st_t *st = ui_get_ctx(w);
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int h = wall_at(st, ev->x, ev->y);
        if (h != st->hov_wall) { st->hov_wall = h; ui_invalidate(w); }
        return true;
    }
    case EV_MOUSE_DOWN: {
        int h = wall_at(st, ev->x, ev->y);
        if (ev->button == MOUSE_LEFT && h >= 0) set_wallpaper_index(st, h);
        return true;
    }
    case EV_MOUSE_UP:
        return true;
    case EV_KEY_DOWN: {
        if (ev->mods & (MOD_CTRL | MOD_ALT)) return false;
        int cur = theme_get()->wallpaper;
        if (ev->key == KEY_LEFT) { set_wallpaper_index(st, cur <= 0 ? st->nthumb - 1 : cur - 1); return true; }
        if (ev->key == KEY_RIGHT) { set_wallpaper_index(st, cur < 0 || cur >= st->nthumb - 1 ? 0 : cur + 1); return true; }
        return false;
    }
    default:
        return false;
    }
}

/* ======================================================================================
 * Appearance: solid colour chips
 * ==================================================================================== */
static rect_t solid_rect(int i) { return rect_make(SOL_X + i * (SOL_D + SOL_GAP), SOL_Y + 5, SOL_D, SOL_D); }

static int solid_at(int x, int y) {
    for (int i = 0; i < NSOLID; i++)
        if (rect_contains(rect_inset(solid_rect(i), -4), x, y)) return i;
    return -1;
}

static int solid_wallpaper(void) { return theme_wallpaper_count() - 1; }

static int solid_current(void) {
    const theme_t *t = theme_get();
    if (t->wallpaper != solid_wallpaper()) return -1;
    for (int i = 0; i < NSOLID; i++)
        if ((solid_colors[i] & 0xFFFFFF) == (t->wallpaper_color & 0xFFFFFF)) return i;
    return -1;
}

static void set_solid(int i) {
    i = (i % NSOLID + NSOLID) % NSOLID;
    theme_set_wallpaper_color(solid_colors[i]);
    if (theme_get()->wallpaper != solid_wallpaper()) theme_set_wallpaper(solid_wallpaper());
    settings_watch(NULL);
}

static void solid_changed(st_t *st) {
    refresh_thumbs(st, false);
    st->last_wallpaper = theme_get()->wallpaper;
    win_invalidate(st->win);
}

static void solid_draw(widget_t *w, surface_t *s, rect_t r) {
    st_t *st = ui_get_ctx(w);
    const theme_t *t = theme_get();
    int mx, my, hov = -1;
    if (sys_mouse_in(st->win, r, &mx, &my)) hov = solid_at(mx, my);
    int cur = solid_current();
    bool focus = ui_has_focus(w) && win_is_focused(st->win);
    for (int i = 0; i < NSOLID; i++) {
        rect_t a = solid_rect(i);
        uint32_t c = solid_colors[i];
        if (i == cur) gfx_rounded_rect(s, a.x - 3, a.y - 3, a.w + 6, a.h + 6, 8, t->accent);
        else if (i == hov) gfx_rounded_rect(s, a.x - 3, a.y - 3, a.w + 6, a.h + 6, 8, color_alpha(t->accent, 120));
        if (focus && i == (cur >= 0 ? cur : 0))
            gfx_rounded_rect(s, a.x - 5, a.y - 5, a.w + 10, a.h + 10, 9, color_alpha(t->window_text, 110));
        gfx_fill_rounded_gradient_v(s, a.x, a.y, a.w, a.h, 6, color_shade(c, 18), color_shade(c, -18));
        gfx_rounded_rect(s, a.x, a.y, a.w, a.h, 6, RGBA(0, 0, 0, 40));
        if (i == cur) sys_check(s, a.x + 4, a.y + 4, a.w - 8, color_luma(c) > 170 ? RGB(20, 20, 24) : COLOR_WHITE);
    }
}

static bool solid_event(widget_t *w, const event_t *ev) {
    st_t *st = ui_get_ctx(w);
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int h = solid_at(ev->x, ev->y);
        if (h != st->hov_solid) { st->hov_solid = h; ui_invalidate(w); }
        return true;
    }
    case EV_MOUSE_DOWN: {
        int h = solid_at(ev->x, ev->y);
        if (ev->button == MOUSE_LEFT && h >= 0) { set_solid(h); solid_changed(st); }
        return true;
    }
    case EV_MOUSE_UP:
        return true;
    case EV_KEY_DOWN:
        if (ev->mods & (MOD_CTRL | MOD_ALT)) return false;
        if (ev->key == KEY_LEFT) { set_solid(solid_current() < 0 ? 0 : solid_current() - 1); solid_changed(st); return true; }
        if (ev->key == KEY_RIGHT) { set_solid(solid_current() + 1); solid_changed(st); return true; }
        if (ev->key == KEY_SPACE) { set_solid(MAX(0, solid_current())); solid_changed(st); return true; }
        return false;
    default:
        return false;
    }
}

static void on_picture_chosen(const char *path, void *ctx) {
    st_t *st = ctx;
    if (!path) return;
    if (load_wall_image(path)) {
        strlcpy(g_wall_image, path, sizeof g_wall_image);
        settings_save();
        st->last_wallpaper = theme_get()->wallpaper;
        win_invalidate(st->win);
        klog("settings: wallpaper picture %s", path);
    } else {
        ui_message_box(st->win, "Wallpaper", "This file could not be opened as a picture.\n\n"
                       "NovaOS can show BMP (8, 24 or 32-bit, uncompressed) and PPM images.",
                       MB_OK | MB_ICON_WARNING, NULL, NULL);
    }
}

static void on_picture(widget_t *w, void *ctx) {
    st_t *st = ctx;
    (void)w;
    const char *start = vfs_is_dir("/home/user/Pictures") ? "/home/user/Pictures" : "/home/user";
    ui_file_dialog(st->win, UI_FILE_OPEN, "Choose a wallpaper picture", start, "bmp,ppm", on_picture_chosen, st);
}

/* ---- screensaver (core service, timeout persisted by the service itself) ---- */
static const int saver_times[] = { 0, 60, 120, 300, 600, 900, 1800 };
static const char *const saver_names[] = { "Never", "1 minute", "2 minutes", "5 minutes", "10 minutes",
                                           "15 minutes", "30 minutes" };
#define NSAVER ((int)(sizeof saver_times / sizeof saver_times[0]))

static int saver_index(st_t *st, int secs) {
    for (int i = 0; i < NSAVER; i++)
        if (saver_times[i] == secs) return i;
    if (st->saver_custom && st->saver_custom == secs) return NSAVER;
    return -1;
}

static void sync_saver(st_t *st) {
    if (!st->w_saver) return;
    int secs = screensaver_get_timeout();
    int idx = saver_index(st, secs);
    if (idx < 0) {          /* e.g. screensaver=5 on the command line: show it as an extra item */
        char b[32];
        snprintf(b, sizeof b, "%d seconds", secs);
        if (st->saver_custom) ui_list_set(st->w_saver, NSAVER, b);
        else ui_list_add(st->w_saver, b);
        st->saver_custom = secs;
        idx = NSAVER;
    }
    if (ui_get_value(st->w_saver) != idx) ui_set_value(st->w_saver, idx);
}

static void on_saver(widget_t *w, void *ctx) {
    st_t *st = ctx;
    int i = ui_get_value(w);
    if (i >= 0 && i < NSAVER) screensaver_set_timeout(saver_times[i]);
    else if (i == NSAVER && st->saver_custom) screensaver_set_timeout(st->saver_custom);
}

static void on_saver_preview(widget_t *w, void *ctx) {
    (void)w; (void)ctx;
    if (!service_is_running("screensaver")) {
        ui_notify("Screensaver", "The screensaver service is stopped. Start it in the Services app.");
        return;
    }
    screensaver_activate();
}

static void on_shadows(widget_t *w, void *ctx) {
    (void)ctx;
    theme_set_shadows(ui_get_value(w) != 0);
    settings_watch(NULL);
}

/* ======================================================================================
 * Mouse page
 * ==================================================================================== */
static int dclick_to_slider(int ms) { return CLAMP((900 - ms + 25) / 50, 0, 15); }
static int slider_to_dclick(int v) { return 900 - CLAMP(v, 0, 15) * 50; }

static const char *speed_name(int v) {
    if (v <= 2) return "Very slow";
    if (v <= 4) return "Slow";
    if (v == 5) return "Normal";
    if (v <= 7) return "Fast";
    return "Very fast";
}

static void on_speed(widget_t *w, void *ctx) {
    st_t *st = ctx;
    sys_set_mouse_speed(ui_get_value(w));
    win_invalidate_rect(st->win, CX + SLIDER_W + 8, SPD_Y - 8, CW - SLIDER_W - 4, 40);
    settings_watch(NULL);
}

static void on_dclick(widget_t *w, void *ctx) {
    st_t *st = ctx;
    sys_set_double_click_ms(slider_to_dclick(ui_get_value(w)));
    win_invalidate_rect(st->win, CX + SLIDER_W + 8, DCL_Y - 8, CW - SLIDER_W - 4, 40);
    settings_watch(NULL);
}

static void on_mouse_reset(widget_t *w, void *ctx) {
    st_t *st = ctx;
    (void)w;
    sys_set_mouse_speed(5);
    sys_set_double_click_ms(450);
    ui_set_value(st->w_speed, 5);
    ui_set_value(st->w_dclick, dclick_to_slider(450));
    st->folder_open = false;
    st->wheel_pos = 0;
    win_invalidate(st->win);
    settings_watch(NULL);
}

/* folder drawing for the double-click test */
static void draw_folder(surface_t *s, int x, int y, int w, int h, bool open) {
    uint32_t back = RGB(236, 170, 40), front = RGB(255, 204, 84), dark = RGB(206, 140, 24);
    gfx_fill_rounded_rect(s, x, y, w * 42 / 100, h / 3, 4, back);
    gfx_fill_rounded_rect(s, x, y + h / 7, w, h - h / 7, 5, back);
    if (!open) {
        gfx_fill_rounded_gradient_v(s, x, y + h * 30 / 100, w, h * 70 / 100, 5, front, color_shade(front, -14));
        gfx_hline(s, x + 3, y + h * 30 / 100, w - 6, RGBA(255, 255, 255, 120));
    } else {
        gfx_fill_rounded_rect(s, x + w / 8, y + h / 14, w * 3 / 4, h * 2 / 3, 3, COLOR_WHITE);
        for (int i = 0; i < 3; i++)
            gfx_fill_rect(s, x + w / 8 + 6, y + h / 14 + 7 + i * 6, w * 3 / 4 - 12, 2, RGB(170, 180, 196));
        point_t p[4] = { { x + w / 6, y + h * 40 / 100 }, { x + w + w / 10, y + h * 40 / 100 },
                         { x + w - w / 12, y + h }, { x, y + h } };
        gfx_fill_polygon(s, p, 4, front);
        gfx_line_aa(s, p[0].x, p[0].y, p[1].x, p[1].y, RGBA(255, 255, 255, 140));
        gfx_line_aa(s, p[3].x, p[3].y, p[2].x, p[2].y, dark);
    }
}

static void test_draw(widget_t *w, surface_t *s, rect_t r) {
    st_t *st = ui_get_ctx(w);
    const theme_t *t = theme_get();
    int pw = (r.w - 16) / 2;
    rect_t a = rect_make(r.x, r.y, pw, r.h), b = rect_make(r.x + pw + 16, r.y, pw, r.h);
    card(s, a);
    card(s, b);
    /* double-click */
    draw_folder(s, a.x + 22, a.y + (a.h - 46) / 2, 58, 46, st->folder_open);
    gfx_text(s, a.x + 100, a.y + 22, "Double-click test", t->window_text, FONT_UI_BOLD);
    gfx_text_wrapped(s, rect_make(a.x + 100, a.y + 44, a.w - 112, a.h - 50),
                     st->folder_open ? "It opened! Double-click again to close it." : "Double-click the folder to open it.",
                     sys_text_dim(t), FONT_UI);
    /* wheel */
    int wx = b.x + 34, wy = b.y + 18, ww = 28, wh = b.h - 36;
    gfx_fill_rounded_rect(s, wx - 6, wy - 6, ww + 12, wh + 12, 12, t->window_border);
    gfx_fill_rounded_rect(s, wx - 5, wy - 5, ww + 10, wh + 10, 11, t->field_bg);
    rect_t old = s->clip;
    s->clip = rect_intersect(old, rect_make(wx, wy, ww, wh));
    gfx_fill_rounded_gradient_v(s, wx, wy, ww, wh, 8, RGB(120, 126, 140), RGB(60, 64, 76));
    int off = ((st->wheel_pos * 5) % 10 + 10) % 10;
    for (int y = wy - 10 + off; y < wy + wh + 10; y += 10) {
        double f = 1.0 - fabs((y - (wy + wh / 2.0)) / (wh / 2.0 + 6));
        if (f < 0) f = 0;
        gfx_fill_rect(s, wx + 3, y, ww - 6, 2, RGBA(255, 255, 255, (int)(40 + 120 * f)));
    }
    s->clip = old;
    gfx_text(s, b.x + 92, b.y + 22, "Scroll wheel", t->window_text, FONT_UI_BOLD);
    char buf[80];
    if (!sys_mouse_has_wheel())
        snprintf(buf, sizeof buf, "No wheel was detected on this mouse.");
    else if (st->wheel_pos == 0)
        snprintf(buf, sizeof buf, "Scroll over this box to try the wheel.");
    else
        snprintf(buf, sizeof buf, "Scrolled %d notch%s %s.", abs(st->wheel_pos), abs(st->wheel_pos) == 1 ? "" : "es",
                 st->wheel_pos > 0 ? "down" : "up");
    gfx_text_wrapped(s, rect_make(b.x + 92, b.y + 44, b.w - 104, b.h - 50), buf, sys_text_dim(t), FONT_UI);
}

static bool test_event(widget_t *w, const event_t *ev) {
    st_t *st = ui_get_ctx(w);
    rect_t r = ui_rect(w);
    int pw = (r.w - 16) / 2;
    bool left = ev->x < r.x + pw;
    switch (ev->type) {
    case EV_DOUBLE_CLICK:
        if (left && ev->button == MOUSE_LEFT) {
            st->folder_open = !st->folder_open;
            ui_invalidate(w);
        }
        return true;
    case EV_MOUSE_WHEEL:
        st->wheel_pos += ev->wheel;
        ui_invalidate(w);
        return true;
    case EV_MOUSE_DOWN: case EV_MOUSE_UP:
        return true;
    default:
        return false;
    }
}

/* ======================================================================================
 * Sound page
 * ==================================================================================== */
static void sync_sound_buttons(st_t *st) {
    bool on = sys_sound_enabled();
    for (int i = 0; i < NSOUNDS; i++) ui_set_enabled(st->w_snd[i], on);
}

static void on_sound_toggle(widget_t *w, void *ctx) {
    st_t *st = ctx;
    bool on = ui_get_value(w) != 0;
    sys_set_sound_enabled(on);
    sync_sound_buttons(st);
    theme_refresh();        /* no "sound changed" notification exists: repaint so the taskbar icon follows */
    if (on) {
        static const note_t ok[] = { { 784, 70 }, { 1175, 110 } };
        sys_play_notes(ok, 2);
        st->snd_until = sys_uptime_ms() + 180;
    }
    win_invalidate(st->win);
    settings_watch(NULL);
}

static void on_play_sound(widget_t *w, void *ctx) {
    st_t *st = ctx;
    int i = (int)ui_get_tag(w);
    if (i < 0 || i >= NSOUNDS || !sys_sound_enabled()) return;
    sys_play_notes(sounds[i].notes, sounds[i].n);
    uint32_t total = 0;
    for (int k = 0; k < sounds[i].n; k++) total += sounds[i].notes[k].ms;
    st->snd_until = sys_uptime_ms() + total;
}

static void on_open_app(widget_t *w, void *ctx) {
    (void)w;
    const char *name = ctx;
    if (!app_launch_by_name(name, NULL)) ui_notify("Settings", "That app is not installed.");
}

static void draw_big_speaker(st_t *st, surface_t *s, int x, int y, int size) {
    const theme_t *t = theme_get();
    bool on = sys_sound_enabled();
    uint32_t now = sys_uptime_ms();
    bool playing = on && now < st->snd_until;
    double u = size / 104.0;
    gfx_fill_circle_aa(s, x + size / 2.0, y + size / 2.0, size / 2.0, color_alpha(on ? t->accent : t->window_text, on ? 40 : 20));
    uint32_t c = on ? t->accent : sys_text_dim(t);
    double cy = y + size / 2.0;
    gfx_fill_rounded_rect(s, (int)(x + 24 * u), (int)(cy - 11 * u), (int)(15 * u), (int)(22 * u), (int)(3 * u), c);
    point_t p[4] = { { (int)(x + 36 * u), (int)(cy - 11 * u) }, { (int)(x + 56 * u), (int)(cy - 28 * u) },
                     { (int)(x + 56 * u), (int)(cy + 28 * u) }, { (int)(x + 36 * u), (int)(cy + 11 * u) } };
    gfx_fill_polygon(s, p, 4, c);
    if (on) {
        for (int i = 0; i < 3; i++) {
            int a = 255;
            if (playing) a = 110 + (int)(145 * (0.5 + 0.5 * sin(now / 80.0 - i * 1.3)));
            sys_arc(s, x + 56 * u, cy, (12 + i * 10) * u, -0.8, 0.8, 4.5 * u, color_alpha(c, a));
        }
    } else {
        sys_stroke(s, x + 66 * u, cy - 11 * u, x + 86 * u, cy + 11 * u, 5 * u, c);
        sys_stroke(s, x + 86 * u, cy - 11 * u, x + 66 * u, cy + 11 * u, 5 * u, c);
    }
}

/* ======================================================================================
 * Date & Time page
 * ==================================================================================== */
static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int days_in_month(int y, int m) {
    static const int md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && leap(y) ? 29 : md[(m - 1) % 12];
}
static int day_of_year(const datetime_t *dt) {
    int d = dt->day;
    for (int m = 1; m < dt->month; m++) d += days_in_month(dt->year, m);
    return d;
}
static int weeks_in_year(int y) {
    int p = (y + y / 4 - y / 100 + y / 400) % 7, q = ((y - 1) + (y - 1) / 4 - (y - 1) / 100 + (y - 1) / 400) % 7;
    return (p == 4 || q == 3) ? 53 : 52;
}
static int iso_week(const datetime_t *dt) {
    int wd = dt->weekday == 0 ? 7 : dt->weekday;
    int w = (day_of_year(dt) - wd + 10) / 7;
    if (w < 1) return weeks_in_year(dt->year - 1);
    if (w > weeks_in_year(dt->year)) return 1;
    return w;
}

static void draw_clock_face(surface_t *s, const datetime_t *dt) {
    const theme_t *t = theme_get();
    double cx = CLK_CX, cy = CLK_CY, r = CLK_R;
    gfx_fill_circle_aa(s, cx, cy + 2, r + 2, RGBA(0, 0, 0, t->dark ? 60 : 24));
    gfx_fill_circle_aa(s, cx, cy, r, t->window_border);
    gfx_fill_circle_aa(s, cx, cy, r - 1.5, card_color(t));
    for (int i = 0; i < 60; i++) {
        double a = i * M_PI / 30;
        bool hour = i % 5 == 0;
        double r0 = r - (hour ? 13 : 7), r1 = r - 5;
        if (hour) sys_stroke(s, cx + r0 * sin(a), cy - r0 * cos(a), cx + r1 * sin(a), cy - r1 * cos(a), 2.6, t->window_text);
        else gfx_line_aa(s, cx + r0 * sin(a), cy - r0 * cos(a), cx + r1 * sin(a), cy - r1 * cos(a), sys_text_dim(t));
    }
    double sec = dt->second, min = dt->minute + sec / 60.0, hr = (dt->hour % 12) + min / 60.0;
    double ha = hr * M_PI / 6, ma = min * M_PI / 30, sa = sec * M_PI / 30;
    sys_stroke(s, cx, cy, cx + r * 0.50 * sin(ha), cy - r * 0.50 * cos(ha), 5.5, t->window_text);
    sys_stroke(s, cx, cy, cx + r * 0.74 * sin(ma), cy - r * 0.74 * cos(ma), 3.5, t->window_text);
    sys_stroke(s, cx - r * 0.16 * sin(sa), cy + r * 0.16 * cos(sa), cx + r * 0.84 * sin(sa), cy - r * 0.84 * cos(sa), 1.6, t->accent);
    gfx_fill_circle_aa(s, cx, cy, 4.5, t->accent);
    gfx_fill_circle_aa(s, cx, cy, 1.8, card_color(t));
}

static void paint_datetime(st_t *st, surface_t *s) {
    (void)st;
    const theme_t *t = theme_get();
    datetime_t dt;
    sys_datetime(&dt);
    draw_clock_face(s, &dt);
    char buf[96];
    int tx = CX + 176;
    snprintf(buf, sizeof buf, "%02d:%02d", dt.hour, dt.minute);
    int tw = gfx_text(s, tx - 2, 104, buf, t->window_text, FONT_UI_HUGE);
    snprintf(buf, sizeof buf, ":%02d", dt.second);
    gfx_text(s, tx + tw + 2, 104 + font_ascent(FONT_UI_HUGE) - font_ascent(FONT_UI_LARGE), buf, sys_text_dim(t), FONT_UI_LARGE);
    snprintf(buf, sizeof buf, "%s, %d %s %d", sys_weekday_name(dt.weekday), dt.day, sys_month_name(dt.month), dt.year);
    gfx_text(s, tx, 176, buf, t->window_text, FONT_UI_LARGE);
    snprintf(buf, sizeof buf, "Week %d  \xB7  day %d of %d", iso_week(&dt), day_of_year(&dt), leap(dt.year) ? 366 : 365);
    gfx_text(s, tx, 206, buf, sys_text_dim(t), FONT_UI);

    /* month calendar */
    rect_t cr = rect_make(CX, CARD_Y, 250, CARD_H);
    card(s, cr);
    snprintf(buf, sizeof buf, "%s %d", sys_month_name(dt.month), dt.year);
    gfx_text(s, cr.x + 16, cr.y + 12, buf, t->window_text, FONT_UI_BOLD);
    static const char *const wdn[7] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
    int cw = 31, x0 = cr.x + (cr.w - 7 * cw) / 2, y0 = cr.y + 38, rh = 26;
    for (int i = 0; i < 7; i++)
        gfx_text_aligned(s, rect_make(x0 + i * cw, y0, cw, 18), wdn[i], i >= 5 ? t->accent : sys_text_dim(t), FONT_UI_BOLD,
                         ALIGN_CENTER);
    int first = ((dt.weekday - (dt.day - 1) % 7) % 7 + 7) % 7;      /* weekday of the 1st, 0 = Sunday */
    int col0 = (first + 6) % 7;                                        /* Monday = 0 */
    int dim = days_in_month(dt.year, dt.month);
    for (int d = 1; d <= dim; d++) {
        int cell = col0 + d - 1, col = cell % 7, row = cell / 7;
        rect_t c = rect_make(x0 + col * cw, y0 + 22 + row * rh, cw, rh);
        char n[4];
        snprintf(n, sizeof n, "%d", d);
        uint32_t col_txt = col >= 5 ? color_mix(t->window_text, t->accent, 110) : t->window_text;
        if (d == dt.day) {
            gfx_fill_circle_aa(s, c.x + cw / 2.0, c.y + rh / 2.0, 11.5, t->accent);
            col_txt = t->accent_text;
        }
        gfx_text_aligned(s, c, n, col_txt, d == dt.day ? FONT_UI_BOLD : FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
    }

    /* details */
    rect_t dr = rect_make(CX + 266, CARD_Y, CW - 266, CARD_H);
    card(s, dr);
    gfx_text(s, dr.x + 16, dr.y + 12, "Details", t->window_text, FONT_UI_BOLD);
    char up[32];
    fmt_uptime(up, sizeof up);
    char unixt[24];
    snprintf(unixt, sizeof unixt, "%u", sys_time());
    const char *rows[5][2] = {
        { "Time format", "24-hour" },
        { "Time source", "Real-time clock" },
        { "Time zone", "Local time (BIOS)" },
        { "Unix time", unixt },
        { "Uptime", up },
    };
    for (int i = 0; i < 5; i++) {
        int y = dr.y + 40 + i * 24;
        gfx_text(s, dr.x + 16, y, rows[i][0], sys_text_dim(t), FONT_UI);
        gfx_text_ellipsis(s, dr.x + 104, y, dr.w - 116, rows[i][1], t->window_text, FONT_UI);
    }
}

/* ======================================================================================
 * About page
 * ==================================================================================== */
static int about_lines(st_t *st, char lines[][2][96], int max) {
    int n = 0;
    const sys_cpu_info_t *ci = &st->cpu;
#define ROW(k, ...) do { if (n < max) { strlcpy(lines[n][0], k, 96); snprintf(lines[n][1], 96, __VA_ARGS__); n++; } } while (0)
    ROW("Processor", "%s", sys_cpu_name());
    const char *vn = vendor_name(ci->vendor);
    if (vn) ROW("CPU vendor", "%s (%s)", ci->vendor, vn);
    else ROW("CPU vendor", "%s", ci->vendor[0] ? ci->vendor : "unknown");
    ROW("CPU family", "Family %d, model %d, stepping %d", ci->family, ci->model, ci->stepping);
    ROW("Features", "%s", ci->features[0] ? ci->features : "-");
    const char *hv = sys_hypervisor_name(ci->hypervisor);
    if (hv) ROW("Running in", "%s", hv);
    else ROW("Running in", "Real hardware (no hypervisor detected)");
    uint32_t used = sys_mem_used(), total = sys_mem_total();
    ROW("Memory", "%u MiB RAM, %u.%u MiB in use", total >> 20, used >> 20, ((used & 0xFFFFF) * 10) >> 20);
    ROW("Display", "%d x %d pixels", screen_width(), screen_height());
    if (persist_active()) {
        persist_info_t pi;
        persist_get_info(&pi);
        const disk_info_t *d = disk_get(pi.disk);
        ROW("Storage", "Installed on %s, files are saved", d ? d->model : "a hard disk");
    } else {
        ROW("Storage", "Live CD, files are kept in memory");
    }
    char up[32];
    fmt_uptime(up, sizeof up);
    ROW("Uptime", "%s", up);
    ROW("Software", "Kernel %s, %d apps", sys_kernel_version(), app_count());
#undef ROW
    return n;
}

static void paint_about(st_t *st, surface_t *s) {
    const theme_t *t = theme_get();
    char buf[128];
    const char *ver = sys_version();
    if (!strncmp(ver, "NovaOS ", 7)) ver += 7;      /* the title already says NovaOS */
    static const char tagline[] = "written from scratch in C and assembly";
    snprintf(buf, sizeof buf, "Version %s %s  \xB7  %s", ver, sys_edition(), tagline);
    int tx = CX + 82, tw = CX + CW - tx;            /* keep inside the info panel's right edge */
    if (font_text_width(FONT_UI, buf) <= tw) {
        ui_draw_logo(s, CX, 96, 64);
        gfx_text(s, CX + 80, 96, "NovaOS", t->window_text, FONT_UI_TITLE);
        gfx_text(s, tx, 134, buf, sys_text_dim(t), FONT_UI);
    } else {
        /* too long for one line (e.g. "Developer Preview"): version, then the tagline */
        ui_draw_logo(s, CX, 92, 64);
        gfx_text(s, CX + 80, 86, "NovaOS", t->window_text, FONT_UI_TITLE);
        snprintf(buf, sizeof buf, "Version %s %s", ver, sys_edition());
        gfx_text_ellipsis(s, tx, 122, tw, buf, sys_text_dim(t), FONT_UI);
        gfx_text_ellipsis(s, tx, 122 + font_height(FONT_UI) + 2, tw, "Written from scratch in C and assembly",
                          sys_text_dim(t), FONT_UI);
    }
    char (*lines)[2][96] = malloc(12 * sizeof *lines);
    if (!lines) return;
    int n = about_lines(st, lines, 12);
    rect_t r = rect_make(CX, INFO_Y, CW, 16 + n * INFO_ROW + 10);
    card(s, r);
    for (int i = 0; i < n; i++) {
        int y = r.y + 14 + i * INFO_ROW;
        gfx_text(s, r.x + 16, y, lines[i][0], sys_text_dim(t), FONT_UI);
        int vw = r.w - 128 - 16;
        if (!strcmp(lines[i][0], "Memory")) {
            /* heap usage bar after the text */
            int tw = gfx_text(s, r.x + 128, y, lines[i][1], t->window_text, FONT_UI);
            int bx = r.x + 128 + tw + 14, bw = r.x + r.w - 16 - bx;
            if (bw > 40) {
                uint32_t heap = MAX(sys_heap_size(), 1u), used = sys_mem_used();
                int fill = (int)((uint64_t)bw * MIN(used, heap) / heap);
                gfx_fill_rounded_rect(s, bx, y + 5, bw, 8, 4, color_alpha(t->window_text, 30));
                gfx_fill_rounded_rect(s, bx, y + 5, MAX(fill, 8), 8, 4, t->accent);
            }
        } else {
            gfx_text_ellipsis(s, r.x + 128, y, vw, lines[i][1], t->window_text, FONT_UI);
        }
    }
    free(lines);
}

static void on_copy_details(widget_t *w, void *ctx) {
    st_t *st = ctx;
    (void)w;
    char (*lines)[2][96] = malloc(12 * sizeof *lines);
    char *out = malloc(2048);
    if (lines && out) {
        int n = about_lines(st, lines, 12);
        int p = snprintf(out, 2048, "%s %s\n", sys_version(), sys_edition());
        for (int i = 0; i < n && p < 2000; i++) p += snprintf(out + p, (size_t)(2048 - p), "%s: %s\n", lines[i][0], lines[i][1]);
        clipboard_set(out);
        ui_notify("Settings", "System details copied to the clipboard.");
    }
    free(lines);
    free(out);
}

/* ======================================================================================
 * SafetyShield page
 * ==================================================================================== */
/* Progress shown under the Scan button: empty until a scan runs while the window is open. */
static bool ss_progress(st_t *st, safetyshield_status_t *ss, int *done, int *all) {
    safetyshield_status(ss);
    bool show = ss->scanning || ss->scan_id != st->ss_base_id;
    *done = show ? ss->scanned : 0;
    *all = show ? MAX(ss->total, ss->scanned) : safetyshield_total_files();
    return show;
}

static void paint_safety(st_t *st, surface_t *s) {
    const theme_t *t = theme_get();
    section(s, CX, RISK_Y - 24, "Risk levels");
    safetyshield_status_t ss;
    int done, all;
    bool show = ss_progress(st, &ss, &done, &all);
    char buf[64];
    snprintf(buf, sizeof buf, "%d/%d", done, all);
    gfx_text(s, CX, COUNT_Y, buf, t->window_text, FONT_UI_BOLD);
    const char *state = "";
    char tbuf[48];
    if (!ss.running) state = "The SafetyShield service is stopped";
    else if (ss.scanning) state = "Scanning...";
    else if (show && ss.threats) {
        snprintf(tbuf, sizeof tbuf, "%d threat%s found", ss.threats, ss.threats == 1 ? "" : "s");
        state = tbuf;
    } else if (show) state = "No threats found";
    gfx_text_aligned(s, rect_make(CX, COUNT_Y, CW, 17), state, ss.threats && show ? safetyshield_risk_color(SS_RISK_HIGH)
                     : sys_text_dim(t), FONT_UI, ALIGN_RIGHT);
    gfx_fill_rounded_rect(s, CX, BAR_Y, CW, BAR_H, BAR_H / 2, color_alpha(t->window_text, t->dark ? 40 : 28));
    if (show && all > 0 && done > 0) {
        int fw = (int)((int64_t)CW * MIN(done, all) / all);
        uint32_t c = ss.threats ? safetyshield_risk_color(SS_RISK_HIGH) : t->accent;
        gfx_fill_rounded_rect(s, CX, BAR_Y, MAX(fw, BAR_H), BAR_H, BAR_H / 2, c);
    }
}

static void invalidate_progress(st_t *st) {
    win_invalidate_rect(st->win, CX - 2, COUNT_Y - 2, CW + 4, BAR_Y + BAR_H + 4 - (COUNT_Y - 2));
}

/* ---- risk levels: list on the left, description card on the right ---- */
static rect_t risk_rect(int i) { return rect_make(CX, RISK_Y + i * RISK_H, RISK_W, RISK_H - 2); }

static int risk_at(int x, int y) {
    for (int i = 0; i < SS_RISK_COUNT; i++)
        if (rect_contains(risk_rect(i), x, y)) return i;
    return -1;
}

static void risks_draw(widget_t *w, surface_t *s, rect_t r) {
    st_t *st = ui_get_ctx(w);
    const theme_t *t = theme_get();
    int mx, my, hov = -1;
    if (sys_mouse_in(st->win, r, &mx, &my)) hov = risk_at(mx, my);
    bool focus = ui_has_focus(w) && win_is_focused(st->win);
    for (int i = 0; i < SS_RISK_COUNT; i++) {
        rect_t a = risk_rect(i);
        bool sel = i == st->risk_sel;
        if (sel) gfx_fill_rounded_rect(s, a.x, a.y, a.w, a.h, 7, color_alpha(t->accent, t->dark ? 70 : 42));
        else if (i == hov) gfx_fill_rounded_rect(s, a.x, a.y, a.w, a.h, 7, color_alpha(t->window_text, t->dark ? 22 : 14));
        if (sel && focus) gfx_rounded_rect(s, a.x - 1, a.y - 1, a.w + 2, a.h + 2, 8, t->accent);
        gfx_fill_circle_aa(s, a.x + 16, a.y + a.h / 2.0, 5.5, safetyshield_risk_color(i));
        gfx_text(s, a.x + 32, a.y + (a.h - font_height(FONT_UI)) / 2, safetyshield_risk_name(i), t->window_text,
                 sel ? FONT_UI_BOLD : FONT_UI);
    }
    rect_t c = rect_make(CX + RISK_W + 16, RISK_Y, CW - RISK_W - 16, SS_RISK_COUNT * RISK_H - 2);
    card(s, c);
    int risk = st->risk_sel;
    uint32_t rc = safetyshield_risk_color(risk);
    gfx_fill_circle_aa(s, c.x + 22, c.y + 24, 6.5, rc);
    gfx_text(s, c.x + 38, c.y + 13, safetyshield_risk_name(risk), t->window_text, FONT_UI_LARGE);
    gfx_text_wrapped(s, rect_make(c.x + 16, c.y + 46, c.w - 32, c.h - 54), safetyshield_risk_description(risk),
                     sys_text_dim(t), FONT_UI);
}

static void set_risk_sel(st_t *st, int i) {
    i = CLAMP(i, 0, SS_RISK_COUNT - 1);
    if (i == st->risk_sel) return;
    st->risk_sel = i;
    ui_invalidate(st->w_risks);
}

static bool risks_event(widget_t *w, const event_t *ev) {
    st_t *st = ui_get_ctx(w);
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int h = risk_at(ev->x, ev->y);
        if (h != st->hov_risk) { st->hov_risk = h; ui_invalidate(w); }
        return true;
    }
    case EV_MOUSE_DOWN: {
        int h = risk_at(ev->x, ev->y);
        if (ev->button == MOUSE_LEFT && h >= 0) set_risk_sel(st, h);
        return true;
    }
    case EV_MOUSE_UP:
        return true;
    case EV_KEY_DOWN:
        if (ev->mods & (MOD_CTRL | MOD_ALT)) return false;
        if (ev->key == KEY_UP) { set_risk_sel(st, st->risk_sel - 1); return true; }
        if (ev->key == KEY_DOWN) { set_risk_sel(st, st->risk_sel + 1); return true; }
        return false;
    default:
        return false;
    }
}

/* ---- Scan button and the "Recent scanned files" link ---- */
static void sync_scan_button(st_t *st) {
    if (!st->w_scan) return;
    safetyshield_status_t ss;
    safetyshield_status(&ss);
    bool en = ss.running && !ss.scanning;
    if (ui_is_enabled(st->w_scan) != en) ui_set_enabled(st->w_scan, en);
}

static void update_fast_timer(st_t *st);

static void on_scan(widget_t *w, void *ctx) {
    st_t *st = ctx;
    (void)w;
    int r = safetyshield_scan();
    if (r < 0) {
        ui_notify("SafetyShield", "The SafetyShield service is stopped. Start it in the Services app.");
        return;
    }
    sync_scan_button(st);
    invalidate_progress(st);
    update_fast_timer(st);
}

static void show_recent(st_t *st, bool on);

static void link_draw(widget_t *w, surface_t *s, rect_t r) {
    st_t *st = ui_get_ctx(w);
    const theme_t *t = theme_get();
    bool hover = sys_mouse_in(st->win, r, NULL, NULL);
    uint32_t c = hover ? color_shade(t->link, t->dark ? 30 : -30) : t->link;
    int tw = gfx_text(s, r.x + 1, r.y + 1, "Recent scanned files", c, FONT_UI);
    gfx_hline(s, r.x + 1, r.y + 1 + font_ascent(FONT_UI) + 2, tw, c);
    if (ui_has_focus(w) && win_is_focused(st->win))
        gfx_rounded_rect(s, r.x - 3, r.y - 2, tw + 8, r.h + 4, 4, color_alpha(t->accent, 160));
}

static bool link_event(widget_t *w, const event_t *ev) {
    st_t *st = ui_get_ctx(w);
    switch (ev->type) {
    case EV_MOUSE_MOVE:
        return true;
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_LEFT) show_recent(st, true);
        return true;
    case EV_MOUSE_UP:
        return true;
    case EV_KEY_DOWN:
        if (ev->key == KEY_ENTER || ev->key == KEY_SPACE) { show_recent(st, true); return true; }
        return false;
    default:
        return false;
    }
}

/* ---- the "Recent scanned files" list (covers the page, X at the top left) ---- */
static rect_t rec_close_rect(void) { return rect_make(SIDE_W + 16, 16, 32, 32); }
static rect_t rec_list_rect(void) { return rect_make(SIDE_W + 16, REC_TOP, SW - SIDE_W - 32, SH - REC_TOP - 12); }

static int rec_max_scroll(void) {
    int content = safetyshield_recent_count() * REC_ROW;
    return MAX(0, content - rec_list_rect().h);
}

static rect_t rec_thumb(int scroll) {
    rect_t l = rec_list_rect();
    int content = safetyshield_recent_count() * REC_ROW;
    if (content <= l.h) return rect_make(0, 0, 0, 0);
    int th = MAX(28, l.h * l.h / content);
    int ty = l.y + (int)((int64_t)(l.h - th) * scroll / MAX(1, content - l.h));
    return rect_make(l.x + l.w - 6, ty, 6, th);
}

static void recent_draw(widget_t *w, surface_t *s, rect_t r) {
    st_t *st = ui_get_ctx(w);
    const theme_t *t = theme_get();
    gfx_fill_rect(s, r.x, r.y, r.w, r.h, t->window_bg);
    rect_t x = rec_close_rect();
    int mx, my;
    bool in = sys_mouse_in(st->win, r, &mx, &my);
    st->rec_hov_x = in && rect_contains(x, mx, my);
    st->rec_hov = -1;
    if (in && !st->rec_dragging && rect_contains(rect_make(rec_list_rect().x, rec_list_rect().y, rec_list_rect().w - 10,
                                                          rec_list_rect().h), mx, my))
        st->rec_hov = (my - rec_list_rect().y + st->rec_scroll) / REC_ROW;
    if (st->rec_hov_x) gfx_fill_rounded_rect(s, x.x, x.y, x.w, x.h, 7, color_alpha(t->window_text, t->dark ? 34 : 22));
    ui_draw_glyph(s, UI_GLYPH_CLOSE, x.x + 10, x.y + 10, 12, t->window_text);
    gfx_text(s, x.x + x.w + 12, x.y + (x.h - font_height(FONT_UI_LARGE)) / 2, "Recent scanned files", t->window_text,
             FONT_UI_LARGE);
    rect_t l = rec_list_rect();
    int n = safetyshield_recent_count();
    st->rec_scroll = CLAMP(st->rec_scroll, 0, rec_max_scroll());
    if (!n) {
        gfx_text_aligned(s, rect_make(l.x, l.y + 40, l.w, 20), "No files have been scanned yet.", sys_text_dim(t),
                         FONT_UI, ALIGN_CENTER);
        return;
    }
    gfx_hline(s, l.x, l.y - 1, l.w, color_mix(t->window_bg, t->window_text, 40));
    rect_t old = s->clip;
    s->clip = rect_intersect(old, l);
    safetyshield_entry_t *e = malloc(sizeof *e);
    if (!e) { s->clip = old; return; }
    int first = st->rec_scroll / REC_ROW;
    int tw_max = l.w - 24;
    for (int i = first; i < n; i++) {
        int y = l.y + i * REC_ROW - st->rec_scroll;
        if (y >= l.y + l.h) break;
        if (!safetyshield_recent_get(i, e)) break;
        if (i == st->rec_hov) gfx_fill_rect(s, l.x, y, l.w - 10, REC_ROW, color_alpha(t->window_text, t->dark ? 14 : 9));
        const char *name = vfs_basename(e->path);
        char dir[VFS_PATH_MAX];
        vfs_dirname(e->path, dir);
        int nw = gfx_text_ellipsis(s, l.x + 12, y + 6, tw_max * 2 / 3, name, t->window_text, FONT_UI_BOLD);
        gfx_text_ellipsis(s, l.x + 12 + nw + 10, y + 6, tw_max - nw - 10, dir, sys_text_dim(t), FONT_UI);
        uint32_t rc = safetyshield_risk_color(e->risk);
        gfx_fill_circle_aa(s, l.x + 16, y + 32, 4, rc);
        int rw = gfx_text(s, l.x + 26, y + 24, safetyshield_risk_name(e->risk), rc, FONT_UI_BOLD);
        gfx_text_ellipsis(s, l.x + 26 + rw + 8, y + 24, tw_max - rw - 22, safetyshield_risk_summary(e->risk),
                          sys_text_dim(t), FONT_UI);
        gfx_hline(s, l.x, y + REC_ROW - 1, l.w - 10, color_mix(t->window_bg, t->window_text, 26));
    }
    free(e);
    rect_t th = rec_thumb(st->rec_scroll);
    if (th.w) gfx_fill_rounded_rect(s, th.x, th.y, th.w, th.h, 3, color_alpha(t->window_text, st->rec_dragging ? 120 : 70));
    s->clip = old;
}

static void rec_scroll_to(st_t *st, int v) {
    v = CLAMP(v, 0, rec_max_scroll());
    if (v != st->rec_scroll) { st->rec_scroll = v; ui_invalidate(st->w_recent); }
}

static bool recent_event(widget_t *w, const event_t *ev) {
    st_t *st = ui_get_ctx(w);
    rect_t l = rec_list_rect();
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        if (st->rec_dragging) {
            int content = safetyshield_recent_count() * REC_ROW;
            rect_t th = rec_thumb(st->rec_drag_scroll);
            int track = l.h - th.h;
            if (track > 0)
                rec_scroll_to(st, st->rec_drag_scroll + (int)((int64_t)(ev->y - st->rec_drag_y) * (content - l.h) / track));
            return true;
        }
        bool hx = rect_contains(rec_close_rect(), ev->x, ev->y);
        int hr = rect_contains(rect_make(l.x, l.y, l.w - 10, l.h), ev->x, ev->y) ? (ev->y - l.y + st->rec_scroll) / REC_ROW : -1;
        if (hr >= safetyshield_recent_count()) hr = -1;
        if (hx != st->rec_hov_x || hr != st->rec_hov) { st->rec_hov_x = hx; st->rec_hov = hr; ui_invalidate(w); }
        return true;
    }
    case EV_MOUSE_DOWN:
        if (ev->button != MOUSE_LEFT) return true;
        if (rect_contains(rec_close_rect(), ev->x, ev->y)) { show_recent(st, false); return true; }
        {
            rect_t th = rec_thumb(st->rec_scroll);
            if (th.w && ev->x >= th.x - 6 && ev->x < l.x + l.w && ev->y >= l.y && ev->y < l.y + l.h) {
                if (ev->y >= th.y && ev->y < th.y + th.h) {
                    st->rec_dragging = true;
                    st->rec_drag_y = ev->y;
                    st->rec_drag_scroll = st->rec_scroll;
                    ui_invalidate(w);
                } else {
                    rec_scroll_to(st, st->rec_scroll + (ev->y < th.y ? -l.h : l.h));
                }
            }
        }
        return true;
    case EV_MOUSE_UP:
        if (st->rec_dragging) { st->rec_dragging = false; ui_invalidate(w); }
        return true;
    case EV_MOUSE_WHEEL:
        rec_scroll_to(st, st->rec_scroll + ev->wheel * REC_ROW * 2);
        return true;
    case EV_KEY_DOWN:
        if (ev->mods & (MOD_CTRL | MOD_ALT)) return false;
        if (ev->key == KEY_ESCAPE) { show_recent(st, false); return true; }
        if (ev->key == KEY_UP) { rec_scroll_to(st, st->rec_scroll - REC_ROW); return true; }
        if (ev->key == KEY_DOWN) { rec_scroll_to(st, st->rec_scroll + REC_ROW); return true; }
        if (ev->key == KEY_PAGEUP) { rec_scroll_to(st, st->rec_scroll - l.h); return true; }
        if (ev->key == KEY_PAGEDOWN) { rec_scroll_to(st, st->rec_scroll + l.h); return true; }
        if (ev->key == KEY_HOME) { rec_scroll_to(st, 0); return true; }
        if (ev->key == KEY_END) { rec_scroll_to(st, rec_max_scroll()); return true; }
        return false;
    default:
        return false;
    }
}

static void show_recent(st_t *st, bool on) {
    if (!st->w_recent || on == st->recent_open) return;
    st->recent_open = on;
    for (int k = 0; k < st->pn[PG_SAFETY]; k++) ui_set_visible(st->pw[PG_SAFETY][k], !on && st->page == PG_SAFETY);
    ui_set_visible(st->w_recent, on);
    st->rec_scroll = 0;
    st->rec_hov = -1;
    st->rec_hov_x = st->rec_dragging = false;
    if (on) ui_focus(st->w_recent);
    else if (st->page == PG_SAFETY && st->w_link) ui_focus(st->w_link);
    win_invalidate(st->win);
}

/* Fast refresh (bar, list) while a scan runs and the page is on screen. */
static void update_fast_timer(st_t *st) {
    safetyshield_status_t ss;
    safetyshield_status(&ss);
    bool want = st->page == PG_SAFETY && ss.scanning;
    if (want && !st->fast) st->fast = win_set_timer(st->win, 40);
    else if (!want && st->fast) { win_kill_timer(st->win, st->fast); st->fast = 0; }
}

static void safety_refresh(st_t *st) {
    uint32_t g = safetyshield_generation();
    if (g == st->ss_gen) return;
    st->ss_gen = g;
    sync_scan_button(st);
    if (st->page == PG_SAFETY) {
        if (st->recent_open) ui_invalidate(st->w_recent);
        else invalidate_progress(st);
    }
    update_fast_timer(st);
}

/* ======================================================================================
 * Window
 * ==================================================================================== */
static void paint_header(st_t *st, surface_t *s) {
    const theme_t *t = theme_get();
    if (st->page == PG_SAFETY) {             /* "SafetyShield" with its icon at the top */
        icon_draw(ICON_SHIELD, s, CX, 21, 36);
        gfx_text(s, CX + 46, 22, page_names[st->page], t->window_text, FONT_UI_TITLE);
    } else {
        gfx_text(s, CX, 22, page_names[st->page], t->window_text, FONT_UI_TITLE);
    }
    gfx_text(s, CX + 1, 60, page_subtitles[st->page], sys_text_dim(t), FONT_UI);
}

static void paint_page(st_t *st, surface_t *s) {
    const theme_t *t = theme_get();
    char buf[96];
    switch (st->page) {
    case PG_APPEARANCE: {
        section(s, CX, 100, "Mode");
        section(s, CX, ACC_Y - 24, "Accent colour");
        int tw = font_text_width(FONT_UI_BOLD, "Wallpaper");
        section(s, CX, WP_Y - 26, "Wallpaper");
        if (t->wallpaper < 0) gfx_text(s, CX + tw + 10, WP_Y - 26, "(a picture is in use)", sys_text_dim(t), FONT_UI);
        gfx_text(s, CX, SOL_Y + 7, "Solid colour", sys_text_dim(t), FONT_UI);
        gfx_text(s, CX + 226, SOL_Y + 57, "Screensaver", t->window_text, FONT_UI);
        break;
    }
    case PG_MOUSE: {
        bool test = sys_cmdline_get("testmouse") != NULL;
        section(s, CX, 100, "Pointer speed");
        gfx_text(s, CX, 118, test ? "How far the pointer moves (fixed while the testmouse option is on)."
                                  : "How far the pointer moves when you move the mouse.", sys_text_dim(t), FONT_UI);
        gfx_text(s, CX + 4, SPD_Y + 28, "Slow", sys_text_dim(t), FONT_UI);
        gfx_text_aligned(s, rect_make(CX, SPD_Y + 28, SLIDER_W - 4, 17), "Fast", sys_text_dim(t), FONT_UI, ALIGN_RIGHT);
        int v = sys_get_mouse_speed();
        snprintf(buf, sizeof buf, "%d", v);
        int tw = gfx_text(s, CX + SLIDER_W + 22, SPD_Y - 2, buf, t->window_text, FONT_UI_LARGE);
        gfx_text(s, CX + SLIDER_W + 30 + tw, SPD_Y + 3, speed_name(v), sys_text_dim(t), FONT_UI);

        section(s, CX, DCL_Y - 44, "Double-click speed");
        gfx_text(s, CX, DCL_Y - 26, "How quickly two clicks must follow each other.", sys_text_dim(t), FONT_UI);
        gfx_text(s, CX + 4, DCL_Y + 28, "Slow", sys_text_dim(t), FONT_UI);
        gfx_text_aligned(s, rect_make(CX, DCL_Y + 28, SLIDER_W - 4, 17), "Fast", sys_text_dim(t), FONT_UI, ALIGN_RIGHT);
        snprintf(buf, sizeof buf, "%d", sys_get_double_click_ms());
        tw = gfx_text(s, CX + SLIDER_W + 22, DCL_Y - 2, buf, t->window_text, FONT_UI_LARGE);
        gfx_text(s, CX + SLIDER_W + 28 + tw, DCL_Y + 3, "ms", sys_text_dim(t), FONT_UI);
        section(s, CX, TEST_Y - 24, "Try it");
        break;
    }
    case PG_DATETIME:
        paint_datetime(st, s);
        break;
    case PG_SOUND: {
        draw_big_speaker(st, s, SPK_X, SPK_Y, SPK_S);
        gfx_text_wrapped(s, rect_make(CX + 140, 136, CW - 140, 60),
                         "Beeps, alerts and melodies play on the PC speaker. The speaker icon in the taskbar "
                         "turns sounds on and off too.", sys_text_dim(t), FONT_UI);
        section(s, CX, 226, "Try a sound");
        rect_t r = rect_make(CX, 310, CW, 106);
        card(s, r);
        gfx_text(s, r.x + 16, r.y + 14, "About the PC speaker", t->window_text, FONT_UI_BOLD);
        gfx_text_wrapped(s, rect_make(r.x + 16, r.y + 36, r.w - 32, r.h - 40),
                         "The PC speaker is the computer's built-in one-voice tone generator, driven by channel 2 "
                         "of the timer chip. It plays one note at a time at a fixed volume, so there is no volume "
                         "control - only on or off.", sys_text_dim(t), FONT_UI);
        break;
    }
    case PG_SAFETY:
        paint_safety(st, s);
        break;
    case PG_ABOUT:
        paint_about(st, s);
        break;
    }
}

static void sync_controls(st_t *st) {
    const theme_t *t = theme_get();
    if (st->w_shadows && ui_get_value(st->w_shadows) != (t->shadows ? 1 : 0)) ui_set_value(st->w_shadows, t->shadows);
    sync_saver(st);
    if (st->w_sound && ui_get_value(st->w_sound) != (sys_sound_enabled() ? 1 : 0)) {
        ui_set_value(st->w_sound, sys_sound_enabled());
        sync_sound_buttons(st);
        if (st->page == PG_SOUND) win_invalidate(st->win);
    }
    if (st->w_speed && ui_get_value(st->w_speed) != sys_get_mouse_speed()) {
        ui_set_value(st->w_speed, sys_get_mouse_speed());
        if (st->page == PG_MOUSE) win_invalidate(st->win);
    }
    if (st->w_dclick && slider_to_dclick(ui_get_value(st->w_dclick)) != sys_get_double_click_ms()) {
        ui_set_value(st->w_dclick, dclick_to_slider(sys_get_double_click_ms()));
        if (st->page == PG_MOUSE) win_invalidate(st->win);
    }
}

static void tick(st_t *st) {
    uint32_t now = sys_uptime_ms();
    uint32_t sec = sys_time();
    if (sec != st->last_sec) {
        st->last_sec = sec;
        if (st->page == PG_DATETIME) {
            win_invalidate_rect(st->win, CX - 6, 92, CW + 12, CARD_Y - 92);
            win_invalidate_rect(st->win, CX + 266, CARD_Y, CW - 266, CARD_H);
        } else if (st->page == PG_ABOUT) {
            win_invalidate_rect(st->win, CX, INFO_Y, CW, 280);
        }
    }
    if (st->page == PG_SOUND && now < st->snd_until + 250) win_invalidate_rect(st->win, SPK_X, SPK_Y, SPK_S, SPK_S);
    safety_refresh(st);
    if (st->page == PG_SAFETY && !st->recent_open) {
        static int last_total = -1;             /* files added / removed: "0/N" follows */
        int n = safetyshield_total_files();
        if (n != last_total) { last_total = n; invalidate_progress(st); }
    }
    sync_controls(st);
    /* the solid colour can change without a theme event (wallpaper setters only repaint) */
    const theme_t *t = theme_get();
    if (st->thumb_color != t->wallpaper_color || st->thumb_accent != t->accent) {
        refresh_thumbs(st, false);
        if (st->w_wall) ui_invalidate(st->w_wall);
    }
    if (st->last_wallpaper != t->wallpaper) {   /* changed here or from the desktop menu */
        st->last_wallpaper = t->wallpaper;
        if (st->page == PG_APPEARANCE) win_invalidate(st->win);
    }
}

static void settings_event(window_t *win, const event_t *ev) {
    st_t *st = win_get_userdata(win);
    if (!st) return;
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        paint_header(st, s);
        paint_page(st, s);
        break;
    }
    case EV_TIMER:
        if (st->fast && ev->timer_id == st->fast) safety_refresh(st);
        else tick(st);
        break;
    case EV_THEME:
        refresh_thumbs(st, false);
        sync_controls(st);
        break;
    case EV_FOCUS:
        sync_controls(st);
        break;
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESCAPE) {
            if (st->recent_open) show_recent(st, false);
            else win_close(win);
            return;
        }
        if ((ev->mods & MOD_CTRL) && (ev->key == KEY_TAB || ev->key == KEY_PAGEDOWN || ev->key == KEY_PAGEUP)) {
            bool back = ev->key == KEY_PAGEUP || (ev->key == KEY_TAB && (ev->mods & MOD_SHIFT));
            set_page(st, (st->page + (back ? PG_COUNT - 1 : 1)) % PG_COUNT);
            ui_focus(st->side);
        }
        break;
    case EV_CLOSE:
        settings_watch(NULL);
        for (int i = 0; i < 8; i++) surface_destroy(st->thumb[i]);
        win_set_userdata(win, NULL);
        free(st);
        break;
    default:
        break;
    }
}

static void build_appearance(st_t *st) {
    window_t *win = st->win;
    st->w_mode = ui_custom(win, CX - 4, MODE_Y - 4, CW + 8, MODE_H + 8, mode_draw, mode_event, st);
    add_pw(st, PG_APPEARANCE, st->w_mode);
    st->w_accent = ui_custom(win, CX - 9, ACC_Y - 9, CW + 18, ACC_D + 18, accent_draw, accent_event, st);
    add_pw(st, PG_APPEARANCE, st->w_accent);
    st->w_wall = ui_custom(win, CX - 7, WP_Y - 7, CW + 14, WP_TH + 32, wall_draw, wall_event, st);
    add_pw(st, PG_APPEARANCE, st->w_wall);
    st->w_solid = ui_custom(win, SOL_X - 6, SOL_Y, NSOLID * (SOL_D + SOL_GAP) + 4, SOL_D + 10, solid_draw, solid_event, st);
    add_pw(st, PG_APPEARANCE, st->w_solid);
    st->w_picture = ui_button(win, CX + CW - 132, SOL_Y, 132, 32, "Picture...", on_picture, st);
    add_pw(st, PG_APPEARANCE, st->w_picture);
    st->w_shadows = ui_checkbox(win, CX, SOL_Y + 54, 210, 24, "Soft window shadows", theme_get()->shadows,
                                on_shadows, st);
    add_pw(st, PG_APPEARANCE, st->w_shadows);
    st->w_saver = ui_dropdown(win, CX + 306, SOL_Y + 51, 122, 30, on_saver, st);
    for (int i = 0; i < NSAVER; i++) ui_list_add(st->w_saver, saver_names[i]);
    sync_saver(st);
    add_pw(st, PG_APPEARANCE, st->w_saver);
    add_pw(st, PG_APPEARANCE, ui_button(win, CX + CW - 80, SOL_Y + 50, 80, 32, "Preview", on_saver_preview, st));
    refresh_thumbs(st, true);
}

static void build_mouse(st_t *st) {
    window_t *win = st->win;
    st->w_speed = ui_slider(win, CX, SPD_Y, SLIDER_W, 24, 1, 10, sys_get_mouse_speed(), on_speed, st);
    add_pw(st, PG_MOUSE, st->w_speed);
    st->w_dclick = ui_slider(win, CX, DCL_Y, SLIDER_W, 24, 0, 15, dclick_to_slider(sys_get_double_click_ms()), on_dclick, st);
    add_pw(st, PG_MOUSE, st->w_dclick);
    st->w_test = ui_custom(win, CX, TEST_Y, CW, TEST_H, test_draw, test_event, st);
    add_pw(st, PG_MOUSE, st->w_test);
    add_pw(st, PG_MOUSE, ui_button(win, CX, TEST_Y + TEST_H + 16, 150, 32, "Restore defaults", on_mouse_reset, st));
}

static void build_datetime(st_t *st) {
    window_t *win = st->win;
    int bx = CX + 266 + 16, bw = (CW - 266 - 32 - 10) / 2, by = CARD_Y + CARD_H - 46;
    if (app_find("Clock")) add_pw(st, PG_DATETIME, ui_button(win, bx, by, bw, 30, "Clock", on_open_app, "Clock"));
    if (app_find("Calendar"))
        add_pw(st, PG_DATETIME, ui_button(win, bx + bw + 10, by, bw, 30, "Calendar", on_open_app, "Calendar"));
}

static void build_sound(st_t *st) {
    window_t *win = st->win;
    st->w_sound = ui_checkbox(win, CX + 138, 106, 300, 24, "Play system sounds", sys_sound_enabled(), on_sound_toggle, st);
    add_pw(st, PG_SOUND, st->w_sound);
    int bw = (CW - (NSOUNDS - 1) * 10) / NSOUNDS;
    for (int i = 0; i < NSOUNDS; i++) {
        st->w_snd[i] = ui_button(win, CX + i * (bw + 10), 250, bw, 32, sounds[i].label, on_play_sound, st);
        ui_set_tag(st->w_snd[i], i);
        add_pw(st, PG_SOUND, st->w_snd[i]);
    }
    sync_sound_buttons(st);
    if (app_find("Piano")) add_pw(st, PG_SOUND, ui_button(win, CX, 434, 150, 32, "Open Piano", on_open_app, "Piano"));
}

static void build_safety(st_t *st) {
    window_t *win = st->win;
    st->w_risks = ui_custom(win, CX - 4, RISK_Y - 4, CW + 8, SS_RISK_COUNT * RISK_H + 6, risks_draw, risks_event, st);
    add_pw(st, PG_SAFETY, st->w_risks);
    st->w_scan = ui_button(win, CX, SCAN_Y, 120, 32, "Scan", on_scan, st);
    add_pw(st, PG_SAFETY, st->w_scan);
    st->w_link = ui_custom(win, CX, LINK_Y, font_text_width(FONT_UI, "Recent scanned files") + 4, 20, link_draw,
                           link_event, st);
    add_pw(st, PG_SAFETY, st->w_link);
    st->w_recent = ui_custom(win, SIDE_W, 0, SW - SIDE_W, SH, recent_draw, recent_event, st);
    ui_set_visible(st->w_recent, false);
    safetyshield_status_t ss;
    safetyshield_status(&ss);
    st->ss_base_id = ss.scanning ? ss.scan_id - 1 : ss.scan_id;
    st->ss_gen = safetyshield_generation();
    sync_scan_button(st);
}

static void build_about(st_t *st) {
    window_t *win = st->win;
    int y = SH - 32 - 24;
    widget_t *copy = ui_button(win, CX + CW - 130, y, 130, 32, "Copy details", on_copy_details, st);
    add_pw(st, PG_ABOUT, copy);
    if (app_find("Welcome"))
        add_pw(st, PG_ABOUT, ui_button(win, CX + CW - 130 - 10 - 140, y, 140, 32, "Welcome tour", on_open_app, "Welcome"));
}

static void settings_launch(const char *arg) {
    st_t *st = calloc(1, sizeof *st);
    if (!st) return;
    window_t *win = win_create("Settings", SW, SH, WIN_CENTER);
    if (!win) { free(st); return; }
    st->win = win;
    st->hov_side = st->hov_mode = st->hov_accent = st->hov_wall = st->hov_solid = -1;
    st->hov_risk = st->rec_hov = -1;
    st->last_wallpaper = theme_get()->wallpaper;
    sys_cpu_info(&st->cpu);
    win_set_userdata(win, st);
    win_set_event_handler(win, settings_event);

    st->side = ui_custom(win, 0, 0, SIDE_W, SH, side_draw, side_event, st);
    build_appearance(st);
    build_mouse(st);
    build_datetime(st);
    build_sound(st);
    build_safety(st);
    build_about(st);

    int page = PG_APPEARANCE;
    if (arg && *arg) {
        for (int i = 0; i < PG_COUNT; i++)
            if (!strcasecmp(arg, page_keys[i]) || !strcasecmp(arg, page_names[i])) page = i;
        if (!strcasecmp(arg, "datetime") || !strcasecmp(arg, "date")) page = PG_DATETIME;
    }
    set_page(st, page);
    ui_focus(st->side);
    st->timer = win_set_timer(win, 200);
}

REGISTER_APP(.name = "Settings", .category = CAT_SYSTEM,
             .description = "Appearance, mouse, date & time, sound and system information",
             .launch = settings_launch, .draw_icon = settings_icon, .flags = APP_SINGLE);
