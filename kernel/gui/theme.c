/* theme.c - global theme (colours, dark mode, accent) and procedural wallpapers.
   The colours and wallpapers below are the built-in copies: once NovaOS has loaded
   /system/ui (kernel/gui/uifiles.c) the theme files and wallpaper recipes there are used. */
#include "wm_internal.h"
#include "bootscreen.h"

static theme_t theme;
surface_t *g_wallpaper;
static surface_t *custom_wallpaper;

static const struct { const char *name; uint32_t color; } accents[] = {
    { "Blue", RGB(59, 130, 246) },
    { "Indigo", RGB(99, 102, 241) },
    { "Purple", RGB(147, 84, 230) },
    { "Pink", RGB(226, 72, 145) },
    { "Red", RGB(225, 60, 60) },
    { "Orange", RGB(240, 128, 40) },
    { "Amber", RGB(215, 160, 20) },
    { "Green", RGB(34, 160, 90) },
    { "Teal", RGB(20, 160, 160) },
    { "Graphite", RGB(110, 116, 128) },
};

int theme_accent_count(void) { return ARRAY_SIZE(accents); }
uint32_t theme_accent_preset(int i) { return (i >= 0 && i < ARRAY_SIZE(accents)) ? accents[i].color : accents[0].color; }
const char *theme_accent_name(int i) { return (i >= 0 && i < ARRAY_SIZE(accents)) ? accents[i].name : "?"; }

static void derive_colors(theme_t *t) {
    uint32_t a = t->accent | 0xFF000000u;
    t->accent_hover = color_shade(a, t->dark ? 30 : -25);
    t->accent_text = color_luma(a) > 170 ? RGB(20, 20, 24) : COLOR_WHITE;
    t->selection_bg = a;
    t->selection_text = t->accent_text;
    t->link = t->dark ? color_shade(a, 60) : color_shade(a, -30);
    if (t->dark) {
        t->window_bg = RGB(32, 33, 37);
        t->window_text = RGB(232, 234, 238);
        t->window_border = RGB(78, 81, 90);
        t->panel_bg = RGB(42, 44, 49);
        t->title_active = RGB(44, 46, 52);
        t->title_inactive = RGB(34, 35, 39);
        t->title_text = RGB(236, 238, 242);
        t->title_text_inactive = RGB(140, 144, 152);
        t->button_bg = RGB(56, 58, 65);
        t->button_hover = RGB(68, 71, 79);
        t->button_pressed = RGB(48, 50, 56);
        t->button_border = RGB(80, 83, 92);
        t->button_text = RGB(236, 238, 242);
        t->field_bg = RGB(26, 27, 30);
        t->field_border = RGB(82, 85, 94);
        t->field_text = RGB(232, 234, 238);
        t->disabled_text = RGB(115, 119, 128);
        t->menu_bg = RGB(44, 46, 52);
        t->menu_text = RGB(232, 234, 238);
        t->menu_hover = color_mix(RGB(44, 46, 52), a, 90);
        t->menu_separator = RGB(66, 69, 77);
        t->taskbar_bg = RGBA(24, 26, 32, 205);
        t->taskbar_text = RGB(236, 238, 242);
        t->taskbar_hover = RGBA(255, 255, 255, 26);
        t->taskbar_active = RGBA(255, 255, 255, 40);
        t->scrollbar_bg = RGB(38, 40, 45);
        t->scrollbar_thumb = RGB(92, 96, 106);
    } else {
        t->window_bg = RGB(246, 247, 250);
        t->window_text = RGB(28, 30, 36);
        t->window_border = RGB(168, 174, 186);
        t->panel_bg = RGB(255, 255, 255);
        t->title_active = RGB(232, 236, 244);
        t->title_inactive = RGB(242, 243, 246);
        t->title_text = RGB(24, 26, 32);
        t->title_text_inactive = RGB(130, 134, 144);
        t->button_bg = RGB(255, 255, 255);
        t->button_hover = RGB(240, 243, 249);
        t->button_pressed = RGB(224, 229, 238);
        t->button_border = RGB(196, 201, 211);
        t->button_text = RGB(28, 30, 36);
        t->field_bg = RGB(255, 255, 255);
        t->field_border = RGB(186, 192, 204);
        t->field_text = RGB(28, 30, 36);
        t->disabled_text = RGB(160, 164, 172);
        t->menu_bg = RGB(252, 252, 254);
        t->menu_text = RGB(28, 30, 36);
        t->menu_hover = color_mix(RGB(252, 252, 254), a, 50);
        t->menu_separator = RGB(225, 228, 234);
        t->taskbar_bg = RGBA(238, 241, 247, 200);
        t->taskbar_text = RGB(24, 26, 32);
        t->taskbar_hover = RGBA(0, 0, 0, 18);
        t->taskbar_active = RGBA(0, 0, 0, 30);
        t->scrollbar_bg = RGB(238, 240, 244);
        t->scrollbar_thumb = RGB(184, 190, 202);
    }
    t->desktop_text = COLOR_WHITE;
}

/* built-in colours, then the colours of the loaded theme file */
static void derive(void) {
    derive_colors(&theme);
    uifiles_theme_override(&theme);
}

void theme_builtin_colors(bool dark, theme_t *out) {
    *out = theme;
    out->dark = dark;
    derive_colors(out);
}

const theme_t *theme_get(void) { return &theme; }

/* ---- wallpapers ------------------------------------------------------------------- */
static const char *wp_names[] = { "Nova", "Aurora", "Sunset", "Ocean", "Midnight", "Solid colour" };
#define WP_COUNT ARRAY_SIZE(wp_names)

int theme_wallpaper_count(void) { return WP_COUNT; }
const char *theme_wallpaper_name(int i) {
    if (i == -1) return "Custom image";
    return (i >= 0 && i < (int)WP_COUNT) ? wp_names[i] : "?";
}

typedef struct { double x, y, r; uint32_t c; double strength; } blob_t;

/* Smooth gradient + glowing blobs rendered at low resolution, then upscaled. */
static void render_blobs(surface_t *s, uint32_t top, uint32_t bottom, const blob_t *blobs, int n) {
    int lw = MAX(2, s->w / 8), lh = MAX(2, s->h / 8);
    surface_t *lo = surface_create(lw, lh);
    if (!lo) { gfx_gradient_v(s, 0, 0, s->w, s->h, top, bottom); return; }
    for (int y = 0; y < lh; y++) {
        uint32_t base = color_mix(top, bottom, y * 255 / (lh - 1));
        double fy = (double)y / (lh - 1);
        for (int x = 0; x < lw; x++) {
            double fx = (double)x / (lw - 1);
            double r = COLOR_R(base), g = COLOR_G(base), b = COLOR_B(base);
            for (int i = 0; i < n; i++) {
                double dx = (fx - blobs[i].x) * ((double)s->w / s->h), dy = fy - blobs[i].y;
                double d2 = (dx * dx + dy * dy) / (blobs[i].r * blobs[i].r);
                if (d2 >= 1.0) continue;
                double k = (1.0 - d2);
                k = k * k * blobs[i].strength;
                r += (COLOR_R(blobs[i].c) - r) * k;
                g += (COLOR_G(blobs[i].c) - g) * k;
                b += (COLOR_B(blobs[i].c) - b) * k;
            }
            surface_row(lo, y)[x] = RGB((int)r, (int)g, (int)b);
        }
    }
    gfx_blit_scaled(s, 0, 0, s->w, s->h, lo, 0, 0, lw, lh, true);
    surface_destroy(lo);
}

static void stars(surface_t *s, int count, uint32_t seed, int max_y) {
    uint32_t st = seed;
    for (int i = 0; i < count; i++) {
        st = st * 1103515245u + 12345u;
        int x = (int)((st >> 8) % (uint32_t)s->w);
        st = st * 1103515245u + 12345u;
        int y = (int)((st >> 8) % (uint32_t)MAX(1, max_y));
        st = st * 1103515245u + 12345u;
        int br = 90 + (int)((st >> 10) % 150);
        gfx_pixel(s, x, y, RGBA(255, 255, 255, br));
        if (br > 220) {
            gfx_pixel(s, x + 1, y, RGBA(255, 255, 255, br / 3));
            gfx_pixel(s, x - 1, y, RGBA(255, 255, 255, br / 3));
            gfx_pixel(s, x, y + 1, RGBA(255, 255, 255, br / 3));
            gfx_pixel(s, x, y - 1, RGBA(255, 255, 255, br / 3));
        }
    }
}

/* Fill below a sine-ish ridge line (mountains / waves). */
static void ridge(surface_t *s, double base, double amp, double f1, double f2, double phase, uint32_t c, uint32_t c2) {
    int w = s->w, h = s->h;
    for (int x = 0; x < w; x++) {
        double fx = (double)x / w;
        double y = base + amp * (0.6 * sin(fx * f1 * 2 * M_PI + phase) + 0.4 * sin(fx * f2 * 2 * M_PI + phase * 1.7));
        int yi = (int)(y * h);
        if (yi < 0) yi = 0;
        if (yi >= h) continue;
        /* AA top pixel */
        double frac = y * h - floor(y * h);
        gfx_pixel(s, x, yi, color_alpha(c, (int)(255 * (1.0 - frac))));
        for (int yy = yi + 1; yy < h; yy++) {
            uint32_t col = c2 == c ? c : color_mix(c, c2, (yy - yi) * 255 / MAX(1, h - yi));
            surface_row(s, yy)[x] = col | 0xFF000000u;
        }
    }
}

void theme_render_wallpaper(surface_t *s, int idx) {
    if (uifiles_render_wallpaper(s, idx)) return;      /* the recipe from /system/ui/wallpapers */
    double sc = (double)s->w / 1024.0;
    (void)sc;
    switch (idx) {
    case 0: {   /* Nova: deep space with nebula glow */
        blob_t b[] = {
            { 0.22, 0.30, 0.55, RGB(110, 70, 230), 0.75 },
            { 0.78, 0.25, 0.45, RGB(40, 140, 255), 0.65 },
            { 0.62, 0.80, 0.60, RGB(220, 70, 170), 0.55 },
            { 0.10, 0.90, 0.40, RGB(30, 190, 200), 0.40 },
        };
        render_blobs(s, RGB(10, 12, 38), RGB(18, 8, 40), b, ARRAY_SIZE(b));
        stars(s, s->w * s->h / 2600, 1234567, s->h);
        break;
    }
    case 1: {   /* Aurora */
        blob_t b[] = {
            { 0.30, 0.35, 0.50, RGB(40, 220, 160), 0.70 },
            { 0.65, 0.28, 0.45, RGB(60, 180, 255), 0.55 },
            { 0.85, 0.50, 0.35, RGB(150, 90, 240), 0.45 },
        };
        render_blobs(s, RGB(6, 18, 34), RGB(4, 30, 40), b, ARRAY_SIZE(b));
        stars(s, s->w * s->h / 5000, 7654321, s->h * 2 / 3);
        ridge(s, 0.80, 0.05, 1.3, 3.1, 0.7, RGB(10, 30, 40), RGB(4, 12, 18));
        ridge(s, 0.87, 0.04, 2.1, 4.3, 2.2, RGB(4, 14, 20), RGB(2, 6, 10));
        break;
    }
    case 2: {   /* Sunset */
        blob_t b[] = {
            { 0.50, 0.62, 0.35, RGB(255, 210, 120), 0.85 },
            { 0.20, 0.20, 0.60, RGB(120, 60, 160), 0.40 },
        };
        render_blobs(s, RGB(60, 40, 120), RGB(250, 120, 90), b, ARRAY_SIZE(b));
        ridge(s, 0.68, 0.06, 1.1, 2.7, 0.3, RGB(120, 60, 110), RGB(90, 40, 90));
        ridge(s, 0.76, 0.05, 1.7, 3.9, 1.9, RGB(70, 35, 80), RGB(50, 25, 60));
        ridge(s, 0.85, 0.04, 2.3, 5.1, 4.0, RGB(36, 20, 48), RGB(20, 12, 30));
        break;
    }
    case 3: {   /* Ocean */
        blob_t b[] = {
            { 0.70, 0.18, 0.40, RGB(170, 230, 255), 0.6 },
            { 0.25, 0.55, 0.50, RGB(20, 120, 200), 0.4 },
        };
        render_blobs(s, RGB(70, 160, 230), RGB(10, 60, 130), b, ARRAY_SIZE(b));
        ridge(s, 0.62, 0.025, 2.0, 5.0, 0.0, RGB(40, 120, 200), RGB(20, 80, 160));
        ridge(s, 0.72, 0.03, 1.6, 4.0, 1.0, RGB(25, 95, 175), RGB(12, 60, 130));
        ridge(s, 0.83, 0.03, 2.4, 3.3, 2.0, RGB(12, 70, 140), RGB(6, 40, 95));
        break;
    }
    case 4: {   /* Midnight: dark minimal gradient with subtle glow */
        blob_t b[] = {
            { 0.50, 1.05, 0.70, theme.accent | 0xFF000000u, 0.45 },
            { 0.15, 0.10, 0.40, RGB(60, 70, 110), 0.35 },
        };
        render_blobs(s, RGB(16, 18, 24), RGB(10, 11, 15), b, ARRAY_SIZE(b));
        break;
    }
    default: {  /* solid colour with a gentle vertical shade */
        uint32_t c = theme.wallpaper_color | 0xFF000000u;
        gfx_gradient_v(s, 0, 0, s->w, s->h, color_shade(c, 18), color_shade(c, -18));
        break;
    }
    }
}

void theme_render_current_wallpaper(void) {
    int w = screen_width(), h = screen_height();
    if (!g_wallpaper || g_wallpaper->w != w || g_wallpaper->h != h) {
        surface_destroy(g_wallpaper);
        g_wallpaper = surface_create(w, h);
        if (!g_wallpaper) return;
    }
    if (theme.wallpaper == -1 && custom_wallpaper) {
        /* cover: scale preserving aspect, crop centre */
        int iw = custom_wallpaper->w, ih = custom_wallpaper->h;
        double s1 = (double)w / iw, s2 = (double)h / ih, sc = s1 > s2 ? s1 : s2;
        int sw = (int)(w / sc), sh = (int)(h / sc);
        gfx_blit_scaled(g_wallpaper, 0, 0, w, h, custom_wallpaper, (iw - sw) / 2, (ih - sh) / 2, sw, sh, true);
    } else {
        theme_render_wallpaper(g_wallpaper, theme.wallpaper < 0 ? 0 : theme.wallpaper);
    }
}

static void wallpaper_changed(void) {
    theme_render_current_wallpaper();
    desktop_wallpaper_changed();
    taskbar_wallpaper_changed();
    wm_invalidate_all();
}

void theme_refresh(void) {
    derive();
    for (window_t *w = wm_bottom; w; w = w->above) {
        if (w->closing) continue;
        w->deco_dirty = !(w->flags & (WIN_FRAMELESS | WIN_POPUP));
        w->need_paint = true;
        w->paint_rect = rect_make(0, 0, w->cw, w->ch);
        event_t ev = { .type = EV_THEME };
        wm_send(w, &ev);
    }
    desktop_wallpaper_changed();
    taskbar_wallpaper_changed();
    wm_invalidate_all();
}

void theme_set_dark(bool dark) {
    theme.dark = dark;
    theme_refresh();
}

void theme_set_accent(uint32_t accent) {
    theme.accent = accent | 0xFF000000u;
    derive();
    if (theme.wallpaper == 4) theme_render_current_wallpaper();
    theme_refresh();
}

void theme_set_wallpaper(int idx) {
    if (idx < -1 || idx >= (int)WP_COUNT) return;
    if (idx == -1 && !custom_wallpaper) return;
    theme.wallpaper = idx;
    wallpaper_changed();
}

void theme_set_wallpaper_color(uint32_t c) {
    theme.wallpaper_color = c | 0xFF000000u;
    if (theme.wallpaper == (int)WP_COUNT - 1) wallpaper_changed();
}

void theme_set_wallpaper_image(const surface_t *img) {
    if (!img) return;
    surface_destroy(custom_wallpaper);
    custom_wallpaper = surface_clone(img);
    if (!custom_wallpaper) return;
    theme.wallpaper = -1;
    wallpaper_changed();
}

void theme_set_shadows(bool on) {
    theme.shadows = on;
    wm_invalidate_all();
}

void theme_init(void) {
    memset(&theme, 0, sizeof theme);
    theme.dark = false;
    theme.accent = accents[0].color;
    theme.wallpaper = 0;
    theme.wallpaper_color = RGB(40, 90, 150);
    theme.font_scale = 1;
    theme.shadows = true;
    const char *d = sys_cmdline_get("dark");
    if (d && (!*d || *d == '1')) theme.dark = true;
    const char *wp = sys_cmdline_get("wallpaper");
    if (wp) theme.wallpaper = CLAMP(atoi(wp), 0, (int)WP_COUNT - 1);
    derive();
}
