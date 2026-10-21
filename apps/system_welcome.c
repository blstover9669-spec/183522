/*
 * system_welcome.c - "Welcome": a friendly first-run tour of NovaOS.
 *
 * Seven pages (Next / Back buttons, Left / Right arrow keys, or click the page
 * dots): hello, the desktop and Start menu, windows, the bundled apps (click
 * an icon to open it), accent colour + dark mode (applied live), keeping your
 * files (Installer), and a shortcut cheat sheet. Each page has an animated
 * illustration drawn with gfx primitives, and pages slide in.
 *
 * First run: an INIT_LATE init hook opens the tour shortly after the desktop
 * appears - unless the user switched it off ("welcome=0" in
 * /system/config/settings, written when the tour is closed with "Show this
 * tour when NovaOS starts" unticked), the kernel command line has autorun=
 * or testmouse= (automated tests), or welcome=0. welcome=1 forces it.
 */
#include "system_common.h"
#include <nova/disk.h>

#define W        640
#define H        440
#define HERO_H   212
#define TX       36
#define FOOT_Y   380
#define NPAGES   7
#define MAX_APPS 10
#define ANIM_MS  280
#define MINI_W   300
#define MINI_H   170

enum { P_HELLO, P_DESKTOP, P_WINDOWS, P_APPS, P_STYLE, P_FILES, P_DONE };

/* clickable items (hover / click ids) */
#define HIT_APP    100
#define HIT_ACCENT 200
#define HIT_LIGHT  300
#define HIT_DARK   301
#define HIT_DOT    400

typedef struct {
    window_t *win;
    int page;
    uint32_t anim_start, page_start;
    int anim_dir;
    int timer;
    int hover;
    widget_t *back, *next, *settings_btn, *installer_btn, *startup_cb;
    surface_t *mini;                 /* miniature of the current wallpaper */
    int mini_idx;
    uint32_t mini_accent, mini_color;
    const app_desc_t *apps[MAX_APPS];
    int napps;
} wl_t;

static const char *const page_titles[NPAGES] = {
    "Welcome to NovaOS", "Your desktop", "Working with windows", "Apps for every day",
    "Make it yours", "Keep your files", "You're all set!",
};

/* ======================================================================================
 * Small drawing helpers
 * ==================================================================================== */
static double ease_out(double p) { p = CLAMP(p, 0.0, 1.0); return 1 - (1 - p) * (1 - p) * (1 - p); }
static double ease_back(double p) {
    p = CLAMP(p, 0.0, 1.0);
    double c1 = 1.70158, c3 = c1 + 1;
    return 1 + c3 * pow(p - 1, 3) + c1 * pow(p - 1, 2);
}

/* four-pointed sparkle */
static void sparkle(surface_t *s, double cx, double cy, double r, uint32_t c) {
    if (r < 0.8) return;
    double k = r * 0.26;
    point_t p[8] = {
        { (int)lround(cx), (int)lround(cy - r) }, { (int)lround(cx + k), (int)lround(cy - k) },
        { (int)lround(cx + r), (int)lround(cy) }, { (int)lround(cx + k), (int)lround(cy + k) },
        { (int)lround(cx), (int)lround(cy + r) }, { (int)lround(cx - k), (int)lround(cy + k) },
        { (int)lround(cx - r), (int)lround(cy) }, { (int)lround(cx - k), (int)lround(cy - k) },
    };
    gfx_fill_polygon(s, p, 8, c);
    gfx_fill_circle_aa(s, cx, cy, k * 1.1, c);
}

static void mouse_pointer(surface_t *s, int x, int y) {
    point_t p[7] = { { x, y }, { x, y + 17 }, { x + 4, y + 13 }, { x + 7, y + 20 },
                     { x + 10, y + 19 }, { x + 7, y + 12 }, { x + 12, y + 12 } };
    point_t q[7];
    for (int i = 0; i < 7; i++) { q[i].x = p[i].x + 1; q[i].y = p[i].y + 1; }
    gfx_fill_polygon(s, q, 7, RGBA(0, 0, 0, 90));
    gfx_fill_polygon(s, p, 7, COLOR_WHITE);
    for (int i = 0; i < 7; i++) {
        point_t a = p[i], b = p[(i + 1) % 7];
        gfx_line_aa(s, a.x, a.y, b.x, b.y, RGB(20, 20, 24));
    }
}

/* miniature window used in the illustrations */
static void mini_window(surface_t *s, int x, int y, int w, int h, bool dark, uint32_t accent, bool active) {
    uint32_t body = dark ? RGB(32, 33, 37) : RGB(248, 249, 251);
    uint32_t title = dark ? (active ? RGB(52, 54, 61) : RGB(40, 41, 46)) : (active ? RGB(224, 229, 238) : RGB(236, 238, 242));
    uint32_t line = dark ? RGB(104, 108, 118) : RGB(178, 184, 196);
    gfx_shadow(s, x, y, w, h, 5, 6, active ? 90 : 50);
    gfx_fill_rounded_rect(s, x, y, w, h, 5, title);
    gfx_fill_rounded_rect(s, x, y + 14, w, h - 14, 5, body);
    gfx_fill_rect(s, x, y + 14, w, 5, body);
    gfx_fill_circle_aa(s, x + w - 9, y + 7, 2.5, dark ? RGB(160, 164, 172) : RGB(130, 136, 148));
    gfx_fill_circle_aa(s, x + 8, y + 7, 3, accent);
    gfx_fill_rounded_rect(s, x + 10, y + 26, w * 50 / 100, 5, 2, line);
    gfx_fill_rounded_rect(s, x + 10, y + 38, w * 36 / 100, 5, 2, line);
    if (h > 70) gfx_fill_rounded_rect(s, x + 10, y + 50, w * 44 / 100, 5, 2, line);
    gfx_fill_rounded_rect(s, x + w - 38, y + h - 18, 28, 10, 4, accent);
}

static void hard_drive(surface_t *s, int x, int y, int w, int h) {
    int r = MAX(3, h / 6);
    gfx_shadow(s, x, y, w, h, r, 6, 70);
    gfx_fill_rounded_gradient_v(s, x, y, w, h, r, RGB(200, 208, 222), RGB(130, 140, 158));
    gfx_rounded_rect(s, x, y, w, h, r, RGBA(40, 48, 64, 150));
    int pr = h * 32 / 100, pcx = x + w * 38 / 100, pcy = y + h / 2;
    gfx_fill_circle_aa(s, pcx, pcy, pr + 1, RGBA(60, 70, 90, 120));
    gfx_fill_circle_aa(s, pcx, pcy, pr, RGB(228, 233, 242));
    gfx_circle_aa(s, pcx, pcy, pr * 0.62, 1.5, RGBA(120, 130, 150, 140));
    gfx_fill_circle_aa(s, pcx, pcy, MAX(2.0, pr * 0.18), RGB(90, 100, 120));
    sys_stroke(s, x + w * 0.80, y + h * 0.24, pcx + pr / 3.0, pcy - pr / 4.0, MAX(2.0, h / 14.0), RGB(70, 78, 96));
    gfx_fill_circle_aa(s, x + w * 0.80, y + h * 0.24, MAX(2.0, h / 11.0), RGB(70, 78, 96));
    gfx_fill_circle_aa(s, x + w * 0.86, y + h * 0.78, MAX(1.5, h / 16.0), RGB(80, 220, 120));
}

static void compact_disc(surface_t *s, int cx, int cy, int r, double spin, int opacity) {
    int a = opacity;
    gfx_fill_circle_aa(s, cx + 2, cy + 3, r, RGBA(0, 0, 0, a * 50 / 255));
    gfx_fill_circle_aa(s, cx, cy, r, RGBA(214, 220, 232, a));
    for (int i = 0; i < 6; i++) {
        double ang = spin + i * M_PI / 3;
        uint32_t c = color_hsv((i * 60 + 200) % 360, 140, 255);
        sys_arc(s, cx, cy, r * 0.70, ang, ang + 0.55, r * 0.42, color_alpha(c, a * 70 / 255));
    }
    gfx_circle_aa(s, cx, cy, r - 0.5, 1.2, RGBA(120, 130, 150, a * 160 / 255));
    gfx_fill_circle_aa(s, cx, cy, r * 0.30, RGBA(240, 243, 248, a));
    gfx_fill_circle_aa(s, cx, cy, r * 0.13, RGBA(120, 130, 150, a));
}

/* ======================================================================================
 * State helpers
 * ==================================================================================== */
static void pick_apps(wl_t *wl) {
    static const char *const pref[] = { "Files", "Notepad", "Terminal", "Calculator", "Paint", "Clock",
                                        "Calendar", "Piano", "Tetris", "Minesweeper", "Task Manager", "Snake" };
    wl->napps = 0;
    for (int i = 0; i < ARRAY_SIZE(pref) && wl->napps < MAX_APPS; i++) {
        const app_desc_t *a = app_find(pref[i]);
        if (a && !(a->flags & APP_HIDDEN)) wl->apps[wl->napps++] = a;
    }
    for (int i = 0; i < app_count() && wl->napps < MAX_APPS; i++) {
        const app_desc_t *a = app_get(i);
        if (!a || (a->flags & APP_HIDDEN) || !strcmp(a->name, "Welcome")) continue;
        bool dup = false;
        for (int k = 0; k < wl->napps; k++) dup |= wl->apps[k] == a;
        if (!dup) wl->apps[wl->napps++] = a;
    }
}

static void refresh_mini(wl_t *wl) {
    const theme_t *t = theme_get();
    int idx = t->wallpaper >= 0 ? t->wallpaper : 0;
    if (wl->mini && idx == wl->mini_idx && wl->mini_accent == t->accent && wl->mini_color == t->wallpaper_color) return;
    if (!wl->mini) wl->mini = surface_create(MINI_W, MINI_H);
    if (!wl->mini) return;
    theme_render_wallpaper(wl->mini, idx);
    wl->mini_idx = idx;
    wl->mini_accent = t->accent;
    wl->mini_color = t->wallpaper_color;
}

static int visible_apps(void) {
    int n = 0;
    for (int i = 0; i < app_count(); i++) {
        const app_desc_t *a = app_get(i);
        if (a && !(a->flags & APP_HIDDEN)) n++;
    }
    return n;
}

/* ======================================================================================
 * Hit testing (apps, accent swatches, light/dark switch, page dots)
 * ==================================================================================== */
static rect_t app_cell(int i) {
    int cols = 5, cw = 104, ch = 92;
    int x0 = (W - cols * cw) / 2, y0 = 16;
    return rect_make(x0 + (i % cols) * cw, y0 + (i / cols) * ch, cw, ch);
}

static rect_t swatch_rect(int i) { return rect_make(354 + (i % 5) * 42, 40 + (i / 5) * 42, 30, 30); }
static rect_t seg_rect(void) { return rect_make(354, 138, 198, 34); }

static rect_t dot_rect(wl_t *wl, int i) {
    int x = TX;
    for (int k = 0; k < i; k++) x += (k == wl->page ? 24 : 9) + 7;
    return rect_make(x, FOOT_Y + 25, i == wl->page ? 24 : 9, 9);
}

static int hit_test(wl_t *wl, int x, int y) {
    for (int i = 0; i < NPAGES; i++)
        if (rect_contains(rect_inset(dot_rect(wl, i), -4), x, y)) return HIT_DOT + i;
    if (wl->page == P_APPS) {
        for (int i = 0; i < wl->napps; i++)
            if (rect_contains(rect_inset(app_cell(i), 6), x, y)) return HIT_APP + i;
    } else if (wl->page == P_STYLE) {
        int n = MIN(theme_accent_count(), 10);
        for (int i = 0; i < n; i++)
            if (rect_contains(rect_inset(swatch_rect(i), -4), x, y)) return HIT_ACCENT + i;
        rect_t r = seg_rect();
        if (rect_contains(r, x, y)) return x < r.x + r.w / 2 ? HIT_LIGHT : HIT_DARK;
    }
    return -1;
}

/* ======================================================================================
 * Illustrations (drawn with an x offset while a page slides in)
 * ==================================================================================== */
static void hero_hello(wl_t *wl, surface_t *s, int dx, uint32_t now) {
    const theme_t *t = theme_get();
    double tt = (now - wl->page_start) / 1000.0;
    int cx = W / 2 + dx, cy = 104;
    double glow = 0.5 + 0.5 * sin(tt * 2.2);
    for (int i = 5; i > 0; i--)
        gfx_fill_circle_aa(s, cx, cy, 56 + i * (10 + 6 * glow), color_alpha(t->accent, 10 + (int)(6 * glow)));
    ui_draw_logo(s, cx - 52, cy - 52, 104);
    static const struct { double x, y, r, ph; } sp[] = {
        { -150, -50, 12, 0.0 }, { -110, 52, 8, 1.7 }, { -205, 20, 6, 3.1 }, { 140, -60, 10, 2.3 },
        { 120, 46, 13, 0.9 }, { 200, -6, 7, 4.0 }, { -60, -86, 6, 5.1 }, { 70, -88, 5, 3.6 },
    };
    for (int i = 0; i < ARRAY_SIZE(sp); i++) {
        double k = 0.35 + 0.65 * (0.5 + 0.5 * sin(tt * 2.6 + sp[i].ph));
        sparkle(s, cx + sp[i].x, cy + sp[i].y, sp[i].r * k, RGBA(255, 255, 255, (int)(120 + 135 * k)));
    }
}

static void hero_desktop(wl_t *wl, surface_t *s, int dx, uint32_t now) {
    const theme_t *t = theme_get();
    double tt = (now - wl->page_start) / 1000.0;
    int x = (W - MINI_W) / 2 + dx, y = 22;
    gfx_shadow(s, x, y, MINI_W, MINI_H, 10, 10, 80);
    if (wl->mini) gfx_blit(s, x, y, wl->mini);
    else gfx_fill_rect(s, x, y, MINI_W, MINI_H, RGB(40, 60, 120));
    /* desktop icons (real app icons) */
    for (int i = 0; i < 4 && i < wl->napps; i++) {
        app_draw_icon(s, wl->apps[i], x + 12, y + 10 + i * 30, 22);
        gfx_fill_rounded_rect(s, x + 38, y + 18 + i * 30, 26, 5, 2, RGBA(255, 255, 255, 150));
    }
    /* taskbar */
    int tbh = 20, ty = y + MINI_H - tbh;
    gfx_fill_rect(s, x, ty, MINI_W, tbh, t->dark ? RGBA(24, 26, 32, 215) : RGBA(240, 242, 248, 215));
    ui_draw_logo(s, x + 5, ty + 3, 14);
    for (int i = 0; i < 3; i++) gfx_fill_rounded_rect(s, x + 28 + i * 18, ty + 5, 12, 10, 3, color_alpha(t->taskbar_text, 90));
    gfx_fill_rounded_rect(s, x + MINI_W - 40, ty + 7, 32, 6, 3, color_alpha(t->taskbar_text, 120));
    /* start menu opens and closes in a loop; the pointer points at the Start button */
    double cyc = fmod(tt, 3.6);
    double open = cyc < 0.6 ? 0 : cyc < 0.9 ? (cyc - 0.6) / 0.3 : cyc < 3.0 ? 1 : cyc < 3.3 ? 1 - (cyc - 3.0) / 0.3 : 0;
    double pulse = 0.5 + 0.5 * sin(tt * 5);
    gfx_circle_aa(s, x + 12, ty + 10, 10 + pulse * 3, 2, color_alpha(t->accent, (int)(200 - pulse * 120)));
    if (open > 0) {
        int mh = (int)(110 * ease_out(open)), mw = 118;
        int mx = x + 4, my = ty - mh - 3;
        rect_t old = s->clip;
        s->clip = rect_intersect(old, rect_make(mx - 2, my - 2, mw + 4, mh + 4));
        gfx_fill_rounded_rect(s, mx, ty - 113, mw, 110, 6, t->dark ? RGB(44, 46, 52) : RGB(250, 251, 253));
        gfx_fill_rounded_rect(s, mx + 8, ty - 105, mw - 16, 10, 5, color_alpha(t->window_text, 40));
        for (int i = 0; i < 4 && i < wl->napps; i++) {
            app_draw_icon(s, wl->apps[(i + 4) % wl->napps], mx + 8, ty - 88 + i * 21, 16);
            gfx_fill_rounded_rect(s, mx + 30, ty - 82 + i * 21, 50 + (i * 13) % 30, 5, 2, color_alpha(t->window_text, 90));
        }
        s->clip = old;
    }
    mouse_pointer(s, x + 15, ty + 8);
}

static void hero_windows(wl_t *wl, surface_t *s, int dx, uint32_t now) {
    const theme_t *t = theme_get();
    double tt = (now - wl->page_start) / 1000.0;
    int x = (W - MINI_W) / 2 + dx, y = 22;
    gfx_shadow(s, x, y, MINI_W, MINI_H, 10, 10, 80);
    if (wl->mini) gfx_blit(s, x, y, wl->mini);
    else gfx_fill_rect(s, x, y, MINI_W, MINI_H, RGB(40, 60, 120));
    rect_t old = s->clip;
    s->clip = rect_intersect(old, rect_make(x, y, MINI_W, MINI_H));
    mini_window(s, x + 22, y + 22, 130, 92, t->dark, t->accent, false);
    /* the second window is dragged to the right edge, snaps to the right half, then resets */
    double cyc = fmod(tt, 4.2);
    double p = cyc < 0.4 ? 0 : cyc < 1.8 ? ease_out((cyc - 0.4) / 1.4) : 1;
    bool snapped = cyc >= 2.1 && cyc < 3.8;
    int wx0 = x + 110, wy0 = y + 60, wx1 = x + MINI_W - 70, wy1 = y + 40;
    if (cyc >= 1.8 && cyc < 2.1) {   /* preview of the snap area */
        double k = (cyc - 1.8) / 0.3;
        gfx_fill_rounded_rect(s, x + MINI_W / 2 + 4, y + 4, MINI_W / 2 - 8, MINI_H - 8, 6, color_alpha(t->accent, (int)(80 * k)));
        gfx_rounded_rect(s, x + MINI_W / 2 + 4, y + 4, MINI_W / 2 - 8, MINI_H - 8, 6, color_alpha(t->accent, (int)(200 * k)));
    }
    if (snapped) {
        mini_window(s, x + MINI_W / 2, y, MINI_W / 2, MINI_H, t->dark, t->accent, true);
        if (cyc < 2.5) mouse_pointer(s, x + MINI_W - 16, y + 6);
    } else {
        int wx = (int)(wx0 + (wx1 - wx0) * p), wy = (int)(wy0 + (wy1 - wy0) * p);
        mini_window(s, wx, wy, 140, 96, t->dark, t->accent, true);
        mouse_pointer(s, wx + 60, wy + 4);
    }
    s->clip = old;
}

static void hero_apps(wl_t *wl, surface_t *s, int dx) {
    const theme_t *t = theme_get();
    for (int i = 0; i < wl->napps; i++) {
        rect_t c = app_cell(i);
        c.x += dx;
        if (wl->hover == HIT_APP + i)
            gfx_fill_rounded_rect(s, c.x + 6, c.y + 2, c.w - 12, c.h - 4, 10, color_alpha(t->window_text, t->dark ? 34 : 22));
        app_draw_icon(s, wl->apps[i], c.x + (c.w - 48) / 2, c.y + 8, 48);
        gfx_text_aligned(s, rect_make(c.x + 2, c.y + 62, c.w - 4, 18), wl->apps[i]->name, t->window_text, FONT_UI,
                         ALIGN_CENTER);
    }
}

static void hero_style(wl_t *wl, surface_t *s, int dx) {
    const theme_t *t = theme_get();
    /* live preview */
    int px = 76 + dx, py = 30;
    gfx_shadow(s, px, py, 230, 150, 8, 8, 70);
    gfx_fill_rounded_gradient_v(s, px, py, 230, 150, 8, color_mix(t->accent, COLOR_WHITE, t->dark ? 40 : 110),
                                color_mix(t->accent, COLOR_BLACK, t->dark ? 150 : 60));
    mini_window(s, px + 26, py + 22, 178, 108, t->dark, t->accent, true);
    /* accent swatches */
    int n = MIN(theme_accent_count(), 10);
    uint32_t cur = t->accent | 0xFF000000u;
    for (int i = 0; i < n; i++) {
        rect_t r = swatch_rect(i);
        r.x += dx;
        uint32_t c = theme_accent_preset(i) | 0xFF000000u;
        double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
        if (c == cur) gfx_circle_aa(s, cx, cy, r.w / 2.0 + 4.5, 2.2, c);
        else if (wl->hover == HIT_ACCENT + i) gfx_circle_aa(s, cx, cy, r.w / 2.0 + 4.5, 2.2, color_alpha(c, 110));
        gfx_fill_circle_aa(s, cx, cy, r.w / 2.0, c);
        if (c == cur) sys_check(s, (int)cx - 8, (int)cy - 8, 16, color_luma(c) > 170 ? RGB(20, 20, 24) : COLOR_WHITE);
    }
    /* light / dark segmented switch */
    rect_t sr = seg_rect();
    sr.x += dx;
    gfx_fill_rounded_rect(s, sr.x, sr.y, sr.w, sr.h, 9, color_alpha(t->window_text, t->dark ? 40 : 26));
    int half = sr.w / 2;
    int selx = t->dark ? sr.x + half : sr.x;
    gfx_fill_rounded_rect(s, selx + 3, sr.y + 3, half - 6, sr.h - 6, 7, t->accent);
    for (int i = 0; i < 2; i++) {
        bool sel = (i == 1) == t->dark;
        uint32_t c = sel ? t->accent_text : (wl->hover == HIT_LIGHT + i ? t->accent : t->window_text);
        gfx_text_aligned(s, rect_make(sr.x + i * half, sr.y, half, sr.h), i ? "Dark" : "Light", c,
                         sel ? FONT_UI_BOLD : FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
    }
}

static void hero_files(wl_t *wl, surface_t *s, int dx, uint32_t now) {
    const theme_t *t = theme_get();
    double tt = (now - wl->page_start) / 1000.0;
    bool installed = persist_active();
    int cdx = 190 + dx, cdy = 106;
    compact_disc(s, cdx, cdy, 58, tt * 1.5, installed ? 110 : 255);
    int hx = 362 + dx, hy = 62, hw = 132, hh = 88;
    /* dashed arrow from the CD to the disk */
    int ax0 = cdx + 72, ax1 = hx - 14;
    double off = fmod(tt * 40, 16);
    rect_t old = s->clip;
    s->clip = rect_intersect(old, rect_make(ax0, cdy - 10, ax1 - ax0 - 10, 20));
    for (double xx = ax0 - 16 + off; xx < ax1; xx += 16)
        gfx_fill_rounded_rect(s, (int)xx, cdy - 3, 9, 6, 3, color_alpha(t->accent, installed ? 90 : 230));
    s->clip = old;
    gfx_fill_triangle(s, ax1 - 12, cdy - 9, ax1 - 12, cdy + 9, ax1, cdy, color_alpha(t->accent, installed ? 90 : 230));
    hard_drive(s, hx, hy, hw, hh);
    if (installed) {
        int bx = hx + hw - 6, by = hy + hh - 4;
        gfx_fill_circle_aa(s, bx, by, 19, COLOR_WHITE);
        gfx_fill_circle_aa(s, bx, by, 16, RGB(34, 170, 90));
        sys_check(s, bx - 10, by - 10, 20, COLOR_WHITE);
    }
}

static void hero_done(wl_t *wl, surface_t *s, int dx, uint32_t now) {
    const theme_t *t = theme_get();
    double tt = (now - wl->page_start) / 1000.0;
    /* confetti */
    for (int i = 0; i < 46; i++) {
        uint32_t h = (uint32_t)(i + 1) * 2654435761u;
        double speed = 26 + (h >> 7) % 38;
        double y = fmod(tt * speed + (h >> 3) % 260, HERO_H + 30) - 15;
        double x = (h >> 11) % W + 10 * sin(tt * 1.3 + i) + dx;
        double tumble = fabs(sin(tt * 3 + i));
        int cw = 3 + (int)(4 * tumble), chh = 7;
        uint32_t c = color_hsv((int)((h >> 5) % 360), 170, 250);
        gfx_fill_rounded_rect(s, (int)x, (int)y, cw, chh, 2, color_alpha(c, 220));
    }
    double k = ease_back((now - wl->page_start) / 520.0);
    int cx = W / 2 + dx, cy = 106;
    double r = 52 * k;
    if (r > 1) {
        gfx_fill_circle_aa(s, cx, cy, r + 12, color_alpha(t->accent, 40));
        gfx_fill_circle_aa(s, cx, cy, r, t->accent);
        int cs = (int)(r * 1.1);
        sys_check(s, cx - cs / 2, cy - cs / 2, cs, t->accent_text);
    }
}

/* ======================================================================================
 * Text of each page
 * ==================================================================================== */
static void page_body(wl_t *wl, char *buf, int n) {
    switch (wl->page) {
    case P_HELLO:
        snprintf(buf, (size_t)n, "NovaOS is a small operating system written from scratch in C and assembly. "
                 "This short tour shows you around - it takes about a minute. Press Next (or the right "
                 "arrow key) to continue.");
        break;
    case P_DESKTOP:
        snprintf(buf, (size_t)n, "Double-click an icon to open it. The Start button (or the Windows key) opens "
                 "the Start menu: just start typing to find apps and files. Right-click the desktop for new "
                 "files, wallpapers and dark mode.");
        break;
    case P_WINDOWS:
        snprintf(buf, (size_t)n, "Drag a window by its title bar. Drop it on the top edge to maximise it, or on "
                 "the left or right edge to fill half the screen. Alt+Tab switches windows, Alt+F4 closes one, "
                 "and taskbar buttons minimise and restore them.");
        break;
    case P_APPS:
        snprintf(buf, (size_t)n, "%d apps come with NovaOS - files, a text editor, a terminal, tools, music and "
                 "games. Click an icon above to open it; the Start menu lists everything by category.",
                 visible_apps());
        break;
    case P_STYLE:
        snprintf(buf, (size_t)n, "Pick an accent colour and light or dark mode right here - the whole desktop "
                 "follows along. Settings has wallpapers, mouse speed, sounds and more.");
        break;
    case P_FILES:
        if (persist_active())
            snprintf(buf, (size_t)n, "NovaOS is installed on this computer's hard disk. Your files are saved to "
                     "the disk automatically a few seconds after every change, and before you shut down.");
        else
            snprintf(buf, (size_t)n, "You are running NovaOS from the CD, so new files live in memory and are "
                     "gone after a restart. The Installer puts NovaOS on the hard disk: it then starts without "
                     "the CD and saves your files automatically.");
        break;
    default:
        buf[0] = 0;
        break;
    }
}

static void paint_text(wl_t *wl, surface_t *s, int dx, int fade) {
    const theme_t *t = theme_get();
    uint32_t title = color_mix(t->window_bg, t->window_text, fade);
    uint32_t body = color_mix(t->window_bg, color_mix(t->window_text, t->window_bg, 70), fade);
    gfx_text(s, TX + dx, HERO_H + 16, page_titles[wl->page], title, FONT_UI_TITLE);
    if (wl->page == P_DONE) {
        static const char *const keys[6][2] = {
            { "Windows key", "Start menu and search" }, { "Win+R", "Run a command or app" },
            { "Alt+Tab", "Switch between windows" },     { "Alt+F4", "Close the window" },
            { "Ctrl+Alt+Del", "Task Manager" },          { "PrintScreen", "Screenshot to Pictures" },
        };
        for (int i = 0; i < 6; i++) {
            int x = TX + dx + (i % 2) * 290, y = HERO_H + 62 + (i / 2) * 24;
            int kw = font_text_width(FONT_UI_BOLD, keys[i][0]) + 14;
            gfx_fill_rounded_rect(s, x, y - 2, kw, 21, 5, color_mix(t->window_bg, color_alpha(t->window_text, 255), t->dark ? 34 : 20));
            gfx_text(s, x + 7, y, keys[i][0], title, FONT_UI_BOLD);
            gfx_text(s, x + kw + 10, y, keys[i][1], body, FONT_UI);
        }
        return;
    }
    char buf[400];
    page_body(wl, buf, sizeof buf);
    gfx_text_wrapped(s, rect_make(TX + dx, HERO_H + 60, W - 2 * TX, 80), buf, body, FONT_UI);
}

/* ======================================================================================
 * Window
 * ==================================================================================== */
static void set_page(wl_t *wl, int p) {
    p = CLAMP(p, 0, NPAGES - 1);
    if (p == wl->page && wl->page_start) return;
    wl->anim_dir = p >= wl->page ? 1 : -1;
    wl->page = p;
    uint32_t now = sys_uptime_ms();
    wl->anim_start = now;
    wl->page_start = now;
    wl->hover = -1;
    ui_set_visible(wl->back, p > 0);
    ui_set_text(wl->next, p == NPAGES - 1 ? "Finish" : "Next");
    if (wl->settings_btn) ui_set_visible(wl->settings_btn, p == P_STYLE);
    if (wl->installer_btn) ui_set_visible(wl->installer_btn, p == P_FILES && !persist_active());
    ui_set_visible(wl->startup_cb, p == P_DONE);
    win_set_cursor(wl->win, CURSOR_ARROW);
    win_invalidate(wl->win);
    klog("welcome: page %d (%s)", p + 1, page_titles[p]);
}

static void on_next(widget_t *w, void *ctx) {
    wl_t *wl = ctx;
    (void)w;
    if (wl->page >= NPAGES - 1) win_close(wl->win);
    else set_page(wl, wl->page + 1);
}

static void on_back(widget_t *w, void *ctx) {
    wl_t *wl = ctx;
    (void)w;
    if (wl->page > 0) set_page(wl, wl->page - 1);
}

static void on_open(widget_t *w, void *ctx) {
    (void)w;
    app_launch_by_name((const char *)ctx, NULL);
}

static void on_startup_toggle(widget_t *w, void *ctx) {
    (void)ctx;
    sys_cfg_put("welcome", ui_get_value(w) ? "1" : "0");
}

static void click(wl_t *wl, int id) {
    if (id >= HIT_DOT && id < HIT_DOT + NPAGES) {
        set_page(wl, id - HIT_DOT);
    } else if (id >= HIT_APP && id < HIT_APP + wl->napps) {
        app_launch(wl->apps[id - HIT_APP], NULL);
    } else if (id >= HIT_ACCENT && id < HIT_ACCENT + 10) {
        theme_set_accent(theme_accent_preset(id - HIT_ACCENT));
    } else if (id == HIT_LIGHT || id == HIT_DARK) {
        if ((id == HIT_DARK) != theme_get()->dark) theme_set_dark(id == HIT_DARK);
    }
}

static void paint(wl_t *wl, surface_t *s) {
    const theme_t *t = theme_get();
    uint32_t now = sys_uptime_ms();
    double p = ease_out((now - wl->anim_start) / (double)ANIM_MS);
    int dx = (int)lround(wl->anim_dir * (1 - p) * 44);
    int fade = (int)(p * 255);

    /* hero band: accent-tinted gradient with slowly drifting glows */
    uint32_t top = color_mix(t->window_bg, t->accent, t->dark ? 70 : 52);
    gfx_gradient_v(s, 0, 0, W, HERO_H, top, t->window_bg);
    double tt = now / 1000.0;
    gfx_fill_circle_aa(s, 120 + 40 * sin(tt * 0.35), 30 + 20 * cos(tt * 0.3), 150, color_alpha(t->accent, t->dark ? 16 : 22));
    gfx_fill_circle_aa(s, 540 + 50 * cos(tt * 0.28), 60 + 25 * sin(tt * 0.33), 130,
                       color_alpha(color_hsv((int)(fmod(tt * 8, 360)), 120, 255), t->dark ? 9 : 18));
    rect_t old = s->clip;
    s->clip = rect_intersect(old, rect_make(0, 0, W, HERO_H));
    switch (wl->page) {
    case P_HELLO:   hero_hello(wl, s, dx, now); break;
    case P_DESKTOP: hero_desktop(wl, s, dx, now); break;
    case P_WINDOWS: hero_windows(wl, s, dx, now); break;
    case P_APPS:    hero_apps(wl, s, dx); break;
    case P_STYLE:   hero_style(wl, s, dx); break;
    case P_FILES:   hero_files(wl, s, dx, now); break;
    default:        hero_done(wl, s, dx, now); break;
    }
    s->clip = old;

    paint_text(wl, s, dx / 2, fade);

    /* footer */
    gfx_hline(s, 0, FOOT_Y, W, color_mix(t->window_bg, t->window_text, t->dark ? 40 : 30));
    for (int i = 0; i < NPAGES; i++) {
        rect_t r = dot_rect(wl, i);
        uint32_t c = i == wl->page ? t->accent
                   : wl->hover == HIT_DOT + i ? color_mix(t->window_bg, t->window_text, 150)
                                              : color_mix(t->window_bg, t->window_text, 70);
        gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, r.h / 2, c);
    }
}

static void welcome_event(window_t *win, const event_t *ev) {
    wl_t *wl = win_get_userdata(win);
    if (!wl) return;
    switch (ev->type) {
    case EV_PAINT:
        paint(wl, win_surface(win));
        break;
    case EV_TIMER: {
        uint32_t now = sys_uptime_ms();
        bool sliding = now - wl->anim_start < ANIM_MS + 40;
        bool animated = wl->page != P_APPS && wl->page != P_STYLE;
        if (sliding) win_invalidate_rect(win, 0, 0, W, FOOT_Y);
        else if (animated && win_is_focused(win)) win_invalidate_rect(win, 0, 0, W, HERO_H);
        else if (animated && (now / 40) % 3 == 0) win_invalidate_rect(win, 0, 0, W, HERO_H);   /* slower when idle */
        break;
    }
    case EV_MOUSE_MOVE: {
        int h = hit_test(wl, ev->x, ev->y);
        if (h != wl->hover) {
            wl->hover = h;
            win_set_cursor(win, h >= 0 ? CURSOR_HAND : CURSOR_ARROW);
            win_invalidate_rect(win, 0, 0, W, HERO_H);
            win_invalidate_rect(win, 0, FOOT_Y, W, H - FOOT_Y);
        }
        break;
    }
    case EV_MOUSE_LEAVE:
        if (wl->hover >= 0) { wl->hover = -1; win_invalidate(win); }
        break;
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_LEFT) {
            int h = hit_test(wl, ev->x, ev->y);
            if (h >= 0) click(wl, h);
        }
        break;
    case EV_KEY_DOWN:
        if (ev->mods & (MOD_CTRL | MOD_ALT)) break;
        if (ev->key == KEY_ESCAPE) { win_close(win); return; }
        if (ev->key == KEY_RIGHT || ev->key == KEY_PAGEDOWN) { if (wl->page < NPAGES - 1) set_page(wl, wl->page + 1); }
        else if (ev->key == KEY_LEFT || ev->key == KEY_PAGEUP || ev->key == KEY_BACKSPACE) on_back(NULL, wl);
        else if (ev->key == KEY_HOME) set_page(wl, 0);
        else if (ev->key == KEY_END) set_page(wl, NPAGES - 1);
        break;
    case EV_THEME:
        refresh_mini(wl);
        break;
    case EV_CLOSE:
        /* first-run tour: after it has been seen once it only comes back if the box is ticked */
        sys_cfg_put("welcome", ui_get_value(wl->startup_cb) ? "1" : "0");
        surface_destroy(wl->mini);
        win_set_userdata(win, NULL);
        free(wl);
        break;
    default:
        break;
    }
}

static void welcome_launch(const char *arg) {
    wl_t *wl = calloc(1, sizeof *wl);
    if (!wl) return;
    window_t *win = win_create("Welcome to NovaOS", W, H, WIN_CENTER);
    if (!win) { free(wl); return; }
    wl->win = win;
    wl->hover = -1;
    wl->mini_idx = -2;
    win_set_userdata(win, wl);
    win_set_event_handler(win, welcome_event);
    pick_apps(wl);
    refresh_mini(wl);

    int by = FOOT_Y + 14;
    wl->next = ui_button(win, W - TX - 112, by, 112, 32, "Next", on_next, wl);
    ui_button_set_default(wl->next, true);
    wl->back = ui_button(win, W - TX - 112 - 10 - 100, by, 100, 32, "Back", on_back, wl);
    if (app_find("Settings"))
        wl->settings_btn = ui_button(win, TX, HERO_H + 116, 140, 32, "Open Settings", on_open, "Settings");
    if (app_find("Installer"))
        wl->installer_btn = ui_button(win, TX, HERO_H + 126, 140, 32, "Open Installer", on_open, "Installer");
    char v[8];
    bool on = sys_cfg_read("welcome", v, sizeof v) && !strcmp(v, "1");
    wl->startup_cb = ui_checkbox(win, TX, HERO_H + 136, 340, 24, "Show this tour when NovaOS starts", on, on_startup_toggle, wl);
    int page = 0;
    if (arg && isdigit((unsigned char)arg[0])) page = CLAMP(atoi(arg) - 1, 0, NPAGES - 1);
    set_page(wl, page);
    ui_focus(wl->next);
    wl->timer = win_set_timer(win, 40);
    if (arg && !strcmp(arg, "startup")) klog("welcome: first-run tour opened at startup");
}

/* ---- first run: open the tour after the desktop appears ------------------------------ */
static void welcome_autostart(void *ctx) {
    (void)ctx;
    app_launch_by_name("Welcome", "startup");
}

static void welcome_boot(void) {
    const char *w = sys_cmdline_get("welcome");
    bool show;
    const char *why;
    if (w) {
        show = !(*w == '0' || !strcmp(w, "off") || !strcmp(w, "no"));
        why = "welcome= on the command line";
    } else if (sys_cmdline_get("autorun") || sys_cmdline_get("testmouse")) {
        show = false;
        why = "autorun/testmouse (automated start)";
    } else {
        char v[8];
        show = !sys_cfg_read("welcome", v, sizeof v) || strcmp(v, "0") != 0;
        why = show ? "first run or switched on" : "switched off by the user";
    }
    klog("welcome: tour at startup: %s (%s)", show ? "yes" : "no", why);
    if (show) timer_add(700, welcome_autostart, NULL, false);
}
REGISTER_INIT(INIT_LATE, welcome_boot);

/* ---- icon: white speech bubble saying "Hi" on a warm gradient tile, with a sparkle ---- */
static void welcome_icon(surface_t *s, int x, int y, int size) {
    double u = size / 48.0;
    int m = (int)lround(2 * u);
    sys_tile(s, x + m, y + m, size - 2 * m, RGB(255, 178, 72), RGB(236, 78, 122));
    uint32_t ink = RGB(236, 92, 112);
    gfx_fill_rounded_rect(s, (int)lround(x + 7 * u), (int)lround(y + 14 * u), (int)lround(30 * u), (int)lround(21 * u),
                          (int)lround(7 * u), COLOR_WHITE);
    gfx_fill_triangle(s, (int)lround(x + 12 * u), (int)lround(y + 33 * u), (int)lround(x + 21 * u), (int)lround(y + 33 * u),
                      (int)lround(x + 10 * u), (int)lround(y + 41 * u), COLOR_WHITE);
    double sw = 2.8 * u;
    sys_stroke(s, x + 15 * u, y + 19 * u, x + 15 * u, y + 30 * u, sw, ink);
    sys_stroke(s, x + 22.5 * u, y + 19 * u, x + 22.5 * u, y + 30 * u, sw, ink);
    sys_stroke(s, x + 15 * u, y + 24.5 * u, x + 22.5 * u, y + 24.5 * u, sw, ink);
    sys_stroke(s, x + 29 * u, y + 23.5 * u, x + 29 * u, y + 30 * u, sw, ink);
    gfx_fill_circle_aa(s, x + 29 * u, y + 19.2 * u, 1.7 * u, ink);
    sparkle(s, x + 38.5 * u, y + 11 * u, 6.5 * u, COLOR_WHITE);
}

REGISTER_APP(.name = "Welcome", .category = CAT_SYSTEM,
             .description = "A quick tour of NovaOS",
             .launch = welcome_launch, .draw_icon = welcome_icon, .flags = APP_SINGLE);
