/* splash.c - NovaOS logo, boot splash with progress bar, fade to desktop, autorun. */
#include "wm_internal.h"

/* Draw the logo directly (polygon edges are not anti-aliased). */
static void draw_logo_raw(surface_t *s, int x, int y, int size) {
    double sz = size;
    int rad = MAX(2, size * 2 / 9);
    gfx_fill_rounded_gradient_v(s, x, y, size, size, rad, RGB(124, 92, 255), RGB(34, 190, 238));
    /* soft highlight on the upper half */
    gfx_fill_rounded_rect(s, x + size / 12, y + size / 14, size - size / 6, size / 2 - size / 14, MAX(1, rad - size / 12), RGBA(255, 255, 255, 34));
    /* four-pointed star */
    double cx = x + sz * 0.5, cy = y + sz * 0.52, R = sz * 0.36, r = sz * 0.085;
    point_t p[8];
    for (int i = 0; i < 8; i++) {
        double a = i * M_PI / 4 - M_PI / 2;
        double rr = (i % 2 == 0) ? R : r;
        p[i].x = (int)lround(cx + cos(a) * rr);
        p[i].y = (int)lround(cy + sin(a) * rr);
    }
    gfx_fill_polygon(s, p, 8, COLOR_WHITE);
    /* small sparkle */
    double sx = x + sz * 0.76, sy = y + sz * 0.24, sr = sz * 0.09;
    point_t q[8];
    for (int i = 0; i < 8; i++) {
        double a = i * M_PI / 4 - M_PI / 2;
        double rr = (i % 2 == 0) ? sr : sr * 0.28;
        q[i].x = (int)lround(sx + cos(a) * rr);
        q[i].y = (int)lround(sy + sin(a) * rr);
    }
    if (size >= 24) gfx_fill_polygon(s, q, 8, RGBA(255, 255, 255, 220));
}

/* Public: supersampled (rendered at 3x, area-downsampled) for smooth edges.
   The last few sizes are cached since the taskbar and apps redraw it often. */
#define LOGO_CACHE 6
static struct { int size; surface_t *img; } logo_cache[LOGO_CACHE];
static int logo_next;

void ui_draw_logo(surface_t *s, int x, int y, int size) {
    if (size <= 0) return;
    for (int i = 0; i < LOGO_CACHE; i++)
        if (logo_cache[i].img && logo_cache[i].size == size) { gfx_blit_alpha(s, x, y, logo_cache[i].img, 255); return; }
    int big = size * 3;
    surface_t *hi = big <= 768 ? surface_create_alpha(big, big) : NULL;
    if (!hi) { draw_logo_raw(s, x, y, size); return; }
    draw_logo_raw(hi, 0, 0, big);
    surface_t *lo = surface_scaled(hi, size, size);
    surface_destroy(hi);
    if (!lo) { draw_logo_raw(s, x, y, size); return; }
    gfx_blit_alpha(s, x, y, lo, 255);
    surface_destroy(logo_cache[logo_next].img);
    logo_cache[logo_next].img = lo;
    logo_cache[logo_next].size = size;
    logo_next = (logo_next + 1) % LOGO_CACHE;
}

/* Soft radial glow (per-pixel falloff, no banding). */
static void glow(surface_t *s, int cx, int cy, int radius, uint32_t color, int max_alpha) {
    for (int y = cy - radius; y <= cy + radius; y++)
        for (int x = cx - radius; x <= cx + radius; x++) {
            double dx = x - cx, dy = y - cy;
            double d = sqrt(dx * dx + dy * dy) / radius;
            if (d >= 1.0) continue;
            double f = (1.0 - d);
            f = f * f;
            gfx_pixel(s, x, y, color_alpha(color, (int)(max_alpha * f)));
        }
}

static int splash_pct;
static char splash_msg[64];
static bool splash_on;

static void splash_draw(void) {
    surface_t *s = wm_backbuf;
    if (!s) return;
    int w = s->w, h = s->h;
    int cx = w / 2, cy = h / 2 - 40;
    int lg = 112;
    rect_t area = rect_make(0, 0, w, h);
    static bool bg_done;
    if (!bg_done) {
        gfx_gradient_v(s, 0, 0, w, h, RGB(12, 14, 34), RGB(6, 7, 16));
        /* glow behind the logo */
        glow(s, cx, cy, lg * 3 / 2, RGB(100, 110, 255), 64);
        ui_draw_logo(s, cx - lg / 2, cy - lg / 2, lg);
        gfx_text_aligned(s, rect_make(0, cy + lg / 2 + 22, w, 40), "NovaOS", COLOR_WHITE, FONT_UI_TITLE, ALIGN_CENTER);
        bg_done = true;
    } else {
        area = rect_make(cx - 200, cy + lg / 2 + 76, 400, 60);
        surface_set_clip(s, area);
        gfx_gradient_v(s, 0, 0, w, h, RGB(12, 14, 34), RGB(6, 7, 16));
        surface_reset_clip(s);
    }
    int bw = 260, by = cy + lg / 2 + 84;
    gfx_fill_rounded_rect(s, cx - bw / 2, by, bw, 6, 3, RGBA(255, 255, 255, 30));
    int fw = bw * splash_pct / 100;
    if (fw > 6) gfx_fill_rounded_gradient_v(s, cx - bw / 2, by, fw, 6, 3, RGB(150, 120, 255), RGB(60, 190, 240));
    gfx_text_aligned(s, rect_make(0, by + 18, w, 20), splash_msg, RGB(150, 156, 190), FONT_UI, ALIGN_CENTER);
    fb_present(s, area);
}

void splash_show(void) {
    if (sys_cmdline_get("nosplash")) return;
    splash_on = true;
    splash_pct = 0;
    strlcpy(splash_msg, "Starting NovaOS...", sizeof splash_msg);
    /* full background */
    splash_draw();
    fb_present(wm_backbuf, rect_make(0, 0, wm_backbuf->w, wm_backbuf->h));
}

void splash_progress(int pct, const char *msg) {
    if (!splash_on) return;
    splash_pct = CLAMP(pct, 0, 100);
    if (msg) strlcpy(splash_msg, msg, sizeof splash_msg);
    splash_draw();
}

/* Animate the bar to 100% and cross-fade into the desktop. Runs before the main loop. */
void splash_finish(void) {
    if (!splash_on) { wm_invalidate_all(); return; }
    uint32_t start = g_ticks;
    int from = splash_pct;
    while (splash_pct < 100) {
        uint32_t el = g_ticks - start;
        splash_progress(from + (int)((100 - from) * MIN(el, 500u) / 500), "Starting desktop...");
        if (el >= 500) break;
        __asm__ volatile("hlt");
    }
    splash_progress(100, "Welcome");
    surface_t *from_img = surface_clone(wm_backbuf);
    surface_t *to_img = screen_capture();
    if (from_img && to_img) {
        int w = wm_backbuf->w, h = wm_backbuf->h;
        const int steps = 6;
        for (int st = 1; st <= steps; st++) {
            int a = st * 255 / steps;
            for (int y = 0; y < h; y++) {
                uint32_t *d = surface_row(wm_backbuf, y);
                const uint32_t *p = surface_row(from_img, y), *q = surface_row(to_img, y);
                for (int x = 0; x < w; x++) {
                    uint32_t c0 = p[x], c1 = q[x];
                    uint32_t rb = ((c0 & 0xFF00FF) * (uint32_t)(255 - a) + (c1 & 0xFF00FF) * (uint32_t)a) >> 8;
                    uint32_t g = ((c0 & 0x00FF00) * (uint32_t)(255 - a) + (c1 & 0x00FF00) * (uint32_t)a) >> 8;
                    d[x] = 0xFF000000u | (rb & 0xFF00FF) | (g & 0x00FF00);
                }
            }
            fb_present(wm_backbuf, rect_make(0, 0, w, h));
        }
    }
    surface_destroy(from_img);
    surface_destroy(to_img);
    splash_on = false;
    wm_invalidate_all();
}

/* Launch the apps listed in autorun=<a>,<b> once the desktop is up. */
static void autorun_cb(void *ctx) {
    (void)ctx;
    const char *list = sys_cmdline_get("autorun");
    if (!list || !*list) return;
    char buf[160];
    strlcpy(buf, list, sizeof buf);
    char *save, *tok = strtok_r(buf, ",", &save);
    while (tok) {
        /* allow '+' or '_' for spaces: autorun=Task_Manager */
        for (char *p = tok; *p; p++) if (*p == '+') *p = ' ';
        char *arg = strchr(tok, ':');
        if (arg) *arg++ = 0;
        if (!app_launch_by_name(tok, arg)) klog("autorun: app \"%s\" not found", tok);
        tok = strtok_r(NULL, ",", &save);
    }
    klog("NOVA: autorun done");
}

void shell_autorun(void) {
    if (sys_cmdline_get("autorun")) timer_add(50, autorun_cb, NULL, false);
    if (sys_cmdline_get("startmenu")) startmenu_open();
}
