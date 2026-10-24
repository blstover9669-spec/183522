/*
 * splash.c - NovaOS logo, the animated boot splash, the loading bar, the fade to
 * the desktop, autorun.
 *
 * The splash runs before the window manager's main loop and draws straight into
 * the back buffer. Every frame touches only what changes: the rising icon (with
 * its glow), the icon itself while it shimmers, the loading bar. Motion is timed
 * with the 1 kHz PIT, so it stays smooth even when a frame is late.
 *
 *   splash_show()      background (as soon as there is a framebuffer)
 *   splash_animate()   icon rises from the bottom to the middle (eases into place),
 *                      0.5 s later a flowing rainbow shimmer, 1 s later back to
 *                      its colours. /system/config/splash.cfg "noanimation=1":
 *                      the icon just appears in the middle.
 *   splash_progress()  loading bar under the icon (installed-drive boots)
 *   splash_finish()    bar to 100 %, cross-fade into the desktop
 */
#include "wm_internal.h"
#include "bootscreen.h"

/* ---- the logo ---------------------------------------------------------------------- */
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

/* A new transparent surface with the logo, supersampled (3x, area-downsampled). */
static surface_t *logo_render(int size) {
    int big = size * 3;
    surface_t *hi = big <= 768 ? surface_create_alpha(big, big) : NULL;
    if (!hi) {
        surface_t *s = surface_create_alpha(size, size);
        if (s) draw_logo_raw(s, 0, 0, size);
        return s;
    }
    draw_logo_raw(hi, 0, 0, big);
    surface_t *lo = surface_scaled(hi, size, size);
    surface_destroy(hi);
    return lo;
}

/* Public. The last few sizes are cached since the taskbar and apps redraw it often. */
#define LOGO_CACHE 6
static struct { int size; surface_t *img; } logo_cache[LOGO_CACHE];
static int logo_next;

void ui_draw_logo(surface_t *s, int x, int y, int size) {
    if (size <= 0) return;
    for (int i = 0; i < LOGO_CACHE; i++)
        if (logo_cache[i].img && logo_cache[i].size == size) { gfx_blit_alpha(s, x, y, logo_cache[i].img, 255); return; }
    surface_t *lo = logo_render(size);
    if (!lo) { draw_logo_raw(s, x, y, size); return; }
    gfx_blit_alpha(s, x, y, lo, 255);
    surface_destroy(logo_cache[logo_next].img);
    logo_cache[logo_next].img = lo;
    logo_cache[logo_next].size = size;
    logo_next = (logo_next + 1) % LOGO_CACHE;
}

/* ---- splash state -------------------------------------------------------------------- */
#define RISE_MS     1000      /* bottom -> middle                         */
#define HOLD_MS     500       /* arrived -> rainbow                       */
#define RAINBOW_MS  1000      /* rainbow -> original colours              */
#define FRAME_MS    14        /* ~70 frames per second                    */
#define GLOW_RGB    RGB(100, 110, 255)
#define GLOW_ALPHA  64

static struct {
    bool on;              /* the splash owns the screen                       */
    bool placed;          /* the icon is on screen                            */
    bool bar_on;
    surface_t *bg;        /* clean background, screen size                    */
    surface_t *logo;      /* icon image (alpha), size x size                  */
    surface_t *glow;      /* soft glow sprite (alpha), 2*glow_r square        */
    surface_t *scaled;    /* icon at another size (splash_place_logo)         */
    int size, glow_r;
    int x, y, cur_size, glow_alpha;   /* icon on screen (top-left, size)      */
    surface_t *bar_under; /* what is under the loading bar                    */
    rect_t bar;
    int bar_val;          /* shown, 0..1000                                   */
} S;

bool splash_active(void) { return S.on; }
surface_t *splash_background(void) { return S.bg; }
rect_t splash_logo_rect(void) { return rect_make(S.x, S.y, S.cur_size, S.cur_size); }

static rect_t screen_rect(void) { return rect_make(0, 0, wm_backbuf->w, wm_backbuf->h); }

static void present(rect_t r) {
    r = rect_intersect(r, screen_rect());
    if (!rect_empty(r)) fb_present(wm_backbuf, r);
}

void boot_sleep_until(uint32_t t) {
    while ((int32_t)(g_ticks - t) < 0) __asm__ volatile("hlt");
}

static surface_t *make_glow(int r) {
    surface_t *g = surface_create_alpha(2 * r, 2 * r);
    if (!g) return NULL;
    for (int y = 0; y < 2 * r; y++) {
        uint32_t *row = surface_row(g, y);
        for (int x = 0; x < 2 * r; x++) {
            double dx = x + 0.5 - r, dy = y + 0.5 - r;
            double d = sqrt(dx * dx + dy * dy) / r;
            if (d >= 1.0) { row[x] = 0; continue; }
            double f = (1.0 - d) * (1.0 - d);
            row[x] = color_alpha(GLOW_RGB, (int)(GLOW_ALPHA * f));
        }
    }
    return g;
}

/* Area covered by the icon and its glow when the icon is at x,y with size sz. */
static rect_t sprite_rect(int x, int y, int sz, int glow_alpha) {
    rect_t icon = rect_make(x, y, sz, sz);
    if (glow_alpha <= 0 || !S.glow) return icon;
    int gr = S.glow_r * sz / MAX(1, S.size);
    int cx = x + sz / 2, cy = y + sz / 2;
    return rect_union(icon, rect_make(cx - gr, cy - gr, 2 * gr, 2 * gr));
}

static void restore_bg(rect_t r) {
    r = rect_intersect(r, screen_rect());
    if (!rect_empty(r)) gfx_blit_rect(wm_backbuf, r.x, r.y, S.bg, r.x, r.y, r.w, r.h);
}

/* Draw the icon (and glow) at x,y,sz over whatever is in the back buffer. */
static void draw_sprite(int x, int y, int sz, int glow_alpha, const surface_t *icon) {
    if (glow_alpha > 0 && S.glow) {
        int gr = S.glow_r * sz / MAX(1, S.size);
        int cx = x + sz / 2, cy = y + sz / 2;
        if (gr == S.glow_r) gfx_blit_alpha(wm_backbuf, cx - gr, cy - gr, S.glow, glow_alpha);
        else if (gr > 0) {
            /* scaled glow: blend through a temporary (only during the move to the top) */
            surface_t *t = surface_scaled(S.glow, 2 * gr, 2 * gr);
            if (t) { gfx_blit_alpha(wm_backbuf, cx - gr, cy - gr, t, glow_alpha); surface_destroy(t); }
        }
    }
    if (icon) gfx_blit_alpha(wm_backbuf, x, y, icon, 255);
}

/* Move the icon: restore the old area, draw at the new place, present the union. */
static void move_sprite(int x, int y, int sz, int glow_alpha, const surface_t *icon) {
    rect_t old = S.placed ? sprite_rect(S.x, S.y, S.cur_size, S.glow_alpha) : rect_make(0, 0, 0, 0);
    rect_t now = sprite_rect(x, y, sz, glow_alpha);
    if (!rect_empty(old)) restore_bg(old);
    draw_sprite(x, y, sz, glow_alpha, icon);
    S.x = x;
    S.y = y;
    S.cur_size = sz;
    S.glow_alpha = glow_alpha;
    S.placed = true;
    present(rect_empty(old) ? now : rect_union(old, now));
}

void splash_place_logo(int x, int y, int size, int glow_alpha) {
    if (!S.on || !S.logo) return;
    const surface_t *icon = S.logo;
    if (size != S.size) {
        if (!S.scaled || S.scaled->w != size) {
            surface_destroy(S.scaled);
            S.scaled = surface_create_alpha(size, size);
            if (S.scaled) gfx_blit_scaled(S.scaled, 0, 0, size, size, S.logo, 0, 0, S.size, S.size, true);
        }
        icon = S.scaled ? S.scaled : S.logo;
    }
    move_sprite(x, y, size, glow_alpha, icon);
}

void splash_show(void) {
    if (S.on || sys_cmdline_get("nosplash") || !wm_backbuf) return;
    int w = wm_backbuf->w, h = wm_backbuf->h;
    S.bg = surface_create(w, h);
    if (!S.bg) return;
    gfx_gradient_v(S.bg, 0, 0, w, h, SPLASH_BG_TOP, SPLASH_BG_BOTTOM);
    S.size = CLAMP(h / 6, 64, 200);
    S.logo = logo_render(S.size);
    S.glow_r = S.size * 7 / 5;
    S.glow = make_glow(S.glow_r);
    S.cur_size = S.size;
    S.on = S.logo != NULL;
    gfx_blit(wm_backbuf, 0, 0, S.bg);
    present(screen_rect());
}

/* /system/config/splash.cfg: "noanimation=1" (written by Settings). */
static bool no_animation(void) {
    char *text = vfs_read_file("/system/config/splash.cfg", NULL);
    if (!text) return false;
    bool on = false;
    char *save = NULL;
    for (char *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        while (*line == ' ' || *line == '\t') line++;
        if (strncmp(line, "noanimation", 11)) continue;
        char *v = line + 11;
        while (*v == ' ' || *v == '\t') v++;
        if (*v != '=') continue;
        v++;
        while (*v == ' ' || *v == '\t') v++;
        on = *v == '1';
    }
    kfree(text);
    return on;
}

static int final_x(void) { return (wm_backbuf->w - S.size) / 2; }
static int final_y(void) { return (wm_backbuf->h - S.size) / 2; }

/* Recolour the icon for one rainbow frame: hue flows diagonally across the tile,
   the white star stays white, a soft sheen sweeps over it; k = strength 0..255. */
static void rainbow_frame(surface_t *dst, const uint8_t *sat, const uint8_t *val, uint32_t t, int k) {
    int sz = S.size;
    int phase = (int)(t * 360 / 600);                  /* one hue turn every 0.6 s   */
    int band = -sz / 2 + (int)(t * (uint32_t)(2 * sz) / RAINBOW_MS);   /* sheen position */
    int bw = MAX(4, sz / 5);
    for (int y = 0; y < sz; y++) {
        const uint32_t *src = surface_row(S.logo, y);
        uint32_t *d = surface_row(dst, y);
        for (int x = 0; x < sz; x++) {
            uint32_t c = src[x];
            uint32_t a = COLOR_A(c);
            if (!a) { d[x] = 0; continue; }
            int i = y * sz + x;
            int hue = (x + y) * 540 / (2 * sz) - phase;
            int s = sat[i] < 40 ? sat[i] : MAX(sat[i], 215);
            uint32_t rb = color_hsv(hue, s, MAX(val[i], 200));
            int dd = abs((x + y) / 2 - band);
            if (dd < bw) {
                int f = (bw - dd) * 255 / bw;
                rb = color_mix(rb, COLOR_WHITE, f * f / 255 * 90 / 255);
            }
            uint32_t o = color_mix(c | 0xFF000000u, rb, k);
            d[x] = (o & 0x00FFFFFFu) | (a << 24);
        }
    }
}

static void play_rainbow(void) {
    int sz = S.size;
    uint8_t *sat = kmalloc((size_t)sz * sz), *val = kmalloc((size_t)sz * sz);
    surface_t *frame = surface_create_alpha(sz, sz);
    surface_t *under = surface_create(sz, sz);
    if (!sat || !val || !frame || !under) goto out;
    for (int y = 0; y < sz; y++) {
        const uint32_t *src = surface_row(S.logo, y);
        for (int x = 0; x < sz; x++) {
            uint32_t c = src[x];
            int r = COLOR_R(c), g = COLOR_G(c), b = COLOR_B(c);
            int mx = MAX(r, MAX(g, b)), mn = MIN(r, MIN(g, b));
            sat[y * sz + x] = (uint8_t)(mx ? (mx - mn) * 255 / mx : 0);
            val[y * sz + x] = (uint8_t)mx;
        }
    }
    /* what is under the icon: background + glow */
    restore_bg(rect_make(S.x, S.y, sz, sz));
    draw_sprite(S.x, S.y, sz, S.glow_alpha, NULL);
    gfx_blit_rect(under, 0, 0, wm_backbuf, S.x, S.y, sz, sz);
    uint32_t t0 = g_ticks, next = t0;
    for (;;) {
        uint32_t t = g_ticks - t0;
        if (t >= RAINBOW_MS) break;
        /* strength: fade in over 180 ms, out over the last 280 ms (smoothstep) */
        int k = 255;
        if (t < 180) k = (int)(t * 255 / 180);
        else if (t > RAINBOW_MS - 280) k = (int)((RAINBOW_MS - t) * 255 / 280);
        k = k * k * (765 - 2 * k) / (255 * 255);
        rainbow_frame(frame, sat, val, t, k);
        gfx_blit(wm_backbuf, S.x, S.y, under);
        gfx_blit_alpha(wm_backbuf, S.x, S.y, frame, 255);
        present(rect_make(S.x, S.y, sz, sz));
        next += FRAME_MS;
        if ((int32_t)(next - g_ticks) < 0) next = g_ticks;
        boot_sleep_until(next);
    }
    /* back to the original colours */
    gfx_blit(wm_backbuf, S.x, S.y, under);
    gfx_blit_alpha(wm_backbuf, S.x, S.y, S.logo, 255);
    present(rect_make(S.x, S.y, sz, sz));
out:
    kfree(sat);
    kfree(val);
    surface_destroy(frame);
    surface_destroy(under);
}

void splash_animate(void) {
    if (!S.on || S.placed) return;
    int fx = final_x(), fy = final_y();
    if (no_animation()) {
        klog("splash: no animation (splash.cfg)");
        move_sprite(fx, fy, S.size, 255, S.logo);
        return;
    }
    uint32_t start = g_ticks;
    /* rise: from just below the bottom edge to the middle, easing into place */
    int y0 = wm_backbuf->h + 4;
    uint32_t t0 = g_ticks, next = t0;
    for (;;) {
        uint32_t el = g_ticks - t0;
        double t = el >= RISE_MS ? 1.0 : (double)el / RISE_MS;
        double e = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
        int y = (int)lround(y0 + (fy - y0) * e);
        if (!S.placed || y != S.y) move_sprite(fx, y, S.size, 255, S.logo);
        if (t >= 1.0) break;
        next += FRAME_MS;
        if ((int32_t)(next - g_ticks) < 0) next = g_ticks;
        boot_sleep_until(next);
    }
    boot_sleep_until(g_ticks + HOLD_MS);
    play_rainbow();
    klog("splash: animation done in %u ms", g_ticks - start);
}

/* ---- loading bar --------------------------------------------------------------------- */
static void bar_draw(int val, int alpha) {
    rect_t b = S.bar;
    gfx_blit(wm_backbuf, b.x, b.y, S.bar_under);
    gfx_fill_rounded_rect(wm_backbuf, b.x, b.y + 1, b.w, b.h - 2, (b.h - 2) / 2, RGBA(255, 255, 255, 30 * alpha / 255));
    int fw = (b.w * val) / 1000;
    if (fw >= b.h - 2)
        gfx_fill_rounded_gradient_v(wm_backbuf, b.x, b.y + 1, fw, b.h - 2, (b.h - 2) / 2, RGB(150, 120, 255), RGB(60, 190, 240));
    present(b);
}

static void bar_show(void) {
    int bw = MAX(200, S.size * 2), bh = 8;
    S.bar = rect_make((wm_backbuf->w - bw) / 2, final_y() + S.size + S.size / 2, bw, bh);
    S.bar_under = surface_create(bw, bh);
    if (!S.bar_under) return;
    gfx_blit_rect(S.bar_under, 0, 0, wm_backbuf, S.bar.x, S.bar.y, bw, bh);
    S.bar_on = true;
    S.bar_val = 0;
    /* fade the empty bar in */
    uint32_t t0 = g_ticks;
    for (;;) {
        uint32_t el = g_ticks - t0;
        bar_draw(0, el >= 150 ? 255 : (int)(el * 255 / 150));
        if (el >= 150) break;
        boot_sleep_until(g_ticks + FRAME_MS);
    }
}

/* Move the bar to pct, smoothly (~120 ms per step). */
void splash_progress(int pct, const char *msg) {
    (void)msg;
    if (!S.on) return;
    if (!S.placed) move_sprite(final_x(), final_y(), S.size, 255, S.logo);
    if (!S.bar_on) bar_show();
    if (!S.bar_on) return;
    int from = S.bar_val, to = CLAMP(pct, 0, 100) * 10;
    if (to <= from) return;
    uint32_t t0 = g_ticks, dur = 90 + (uint32_t)(to - from) / 4;
    for (;;) {
        uint32_t el = g_ticks - t0;
        double t = el >= dur ? 1.0 : (double)el / dur;
        double e = t * t * (3 - 2 * t);
        S.bar_val = from + (int)lround((to - from) * e);
        bar_draw(S.bar_val, 255);
        if (t >= 1.0) break;
        boot_sleep_until(g_ticks + FRAME_MS);
    }
}

/* ---- leaving the splash ------------------------------------------------------------------ */
void splash_crossfade(const surface_t *from, const surface_t *to, int ms) {
    int w = wm_backbuf->w, h = wm_backbuf->h;
    uint32_t t0 = g_ticks;
    for (;;) {
        uint32_t el = g_ticks - t0;
        int a = el >= (uint32_t)ms ? 255 : (int)(el * 255 / (uint32_t)ms);
        for (int y = 0; y < h; y++) {
            uint32_t *d = surface_row(wm_backbuf, y);
            const uint32_t *p = surface_row(from, y), *q = surface_row(to, y);
            for (int x = 0; x < w; x++) {
                uint32_t c0 = p[x], c1 = q[x];
                uint32_t rb = ((c0 & 0xFF00FF) * (uint32_t)(255 - a) + (c1 & 0xFF00FF) * (uint32_t)a) >> 8;
                uint32_t g = ((c0 & 0x00FF00) * (uint32_t)(255 - a) + (c1 & 0x00FF00) * (uint32_t)a) >> 8;
                d[x] = 0xFF000000u | (rb & 0xFF00FF) | (g & 0x00FF00);
            }
        }
        fb_present(wm_backbuf, rect_make(0, 0, w, h));
        if (a >= 255) break;
    }
}

void splash_release(void) {
    surface_destroy(S.bg);
    surface_destroy(S.logo);
    surface_destroy(S.glow);
    surface_destroy(S.scaled);
    surface_destroy(S.bar_under);
    memset(&S, 0, sizeof S);
}

/* Let the windows that exist before the main loop (desktop, taskbar, ...) paint, so
   a capture of the screen shows them and not empty surfaces. */
static void paint_windows_now(void) {
    for (window_t *w = wm_bottom; w; w = w->above) {
        if (!w->visible || w->minimized || w->closing) continue;
        if (w->deco_dirty) wm_redraw_decor(w);
        if (!w->need_paint) continue;
        w->need_paint = false;
        rect_t pr = rect_intersect(w->paint_rect, rect_make(0, 0, w->cw, w->ch));
        if (rect_empty(pr)) continue;
        surface_t *s = &w->client;
        surface_set_clip(s, pr);
        gfx_fill_rect(s, pr.x, pr.y, pr.w, pr.h, theme_get()->window_bg);
        event_t ev = { .type = EV_PAINT, .x = pr.x, .y = pr.y, .w = pr.w, .h = pr.h };
        const app_desc_t *saved = wm_current_app;
        wm_current_app = w->app;
        if (w->handler) w->handler(w, &ev);
        if (!w->closing) {
            surface_set_clip(s, pr);
            ui_paint_all(w);
        }
        wm_current_app = saved;
        surface_reset_clip(s);
    }
}

surface_t *splash_capture_desktop(void) {
    paint_windows_now();
    return screen_capture();
}

/* Bar to 100 % (if it is shown) and cross-fade into the desktop. Runs before the main loop. */
void splash_finish(void) {
    if (!S.on) { wm_invalidate_all(); return; }
    paint_windows_now();                 /* the desktop's first paint is part of the loading */
    if (S.bar_on) {
        splash_progress(100, NULL);
        boot_sleep_until(g_ticks + 100);
    } else if (!S.placed) {
        move_sprite(final_x(), final_y(), S.size, 255, S.logo);
    }
    surface_t *from_img = surface_clone(wm_backbuf);
    surface_t *to_img = splash_capture_desktop();
    if (from_img && to_img) splash_crossfade(from_img, to_img, 220);
    surface_destroy(from_img);
    surface_destroy(to_img);
    splash_release();
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
