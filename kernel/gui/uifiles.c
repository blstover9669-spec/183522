/*
 * uifiles.c - the UI files on the drive (NovaOS 2.5): NovaOS loads its fonts,
 * themes, wallpapers and UI settings from /system/ui at startup.
 *
 *   /system/ui/ui.cfg                     UI settings: dark, accent, wallpaper,
 *                                         wallcolor, shadows (key=value; the
 *                                         command line and the user's Settings
 *                                         win over them)
 *   /system/ui/fonts/<name>.font          the UI and monospace fonts: an "NVFN"
 *                                         table (metrics, glyph boxes, 8-bit
 *                                         coverage) packed with NVLZ (fs/lz.h)
 *   /system/ui/themes/light.theme         the colours of light and dark mode
 *   /system/ui/themes/dark.theme          (key=RRGGBB or AARRGGBB)
 *   /system/ui/wallpapers/<n>.wallpaper   the built-in wallpapers as recipes:
 *                                         gradient, glowing blobs, stars and
 *                                         ridges (mountains, waves)
 *
 * On an ISO start (and the first start of an installed drive) the files are
 * written from the built-in copies; an installed drive keeps them with the
 * other files. The fonts loaded here replace the built-in ones for all text
 * (kernel/gfx/text.c asks font_active()); themes and wallpapers are used by
 * kernel/gui/theme.c. A file that is there but damaged is skipped (logged) and
 * the built-in copy is used; a missing file means the drive is incomplete:
 * "No drive found" (nodrive.c), checked before this runs. The screens that
 * must work without the drive (splash, No drive found, panic) use built-ins.
 *
 * Kept small for RAM: fonts are packed (the 8 fonts take about 110 KB on the
 * drive), wallpapers are recipes of a few hundred bytes instead of pictures.
 */
#include "wm_internal.h"
#include "bootscreen.h"
#include "gfx/font_internal.h"
#include "fs/lz.h"
#include <stddef.h>

#define UI_DIR "/system/ui"

/* ---- fonts ------------------------------------------------------------------------------ */
static const struct { const char *file; const font_t *builtin; } font_files[] = {
    { "ui.font", &font_ui },             { "ui-bold.font", &font_ui_bold },
    { "ui-large.font", &font_ui_large }, { "ui-title.font", &font_ui_title },
    { "ui-huge.font", &font_ui_huge },   { "mono.font", &font_mono },
    { "mono-bold.font", &font_mono_bold }, { "mono-2x.font", &font_mono_2x },
};
#define NFONTS ARRAY_SIZE(font_files)

const struct font *font_map_from[FONT_MAP_MAX], *font_map_to[FONT_MAP_MAX];
int font_map_n;

/* "NVFN" table: header, glyph records, coverage bytes (little endian) */
#define NVFN_HDR   36
#define NVFN_GLYPH 12

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }

static uint8_t *font_serialize(const font_t *f, uint32_t *len) {
    uint32_t data_len = 0;
    for (int i = 0; i < f->count; i++) {
        const glyph_t *g = &f->glyphs[i];
        data_len = MAX(data_len, g->off + (uint32_t)g->w * g->h);
    }
    uint32_t n = NVFN_HDR + (uint32_t)f->count * NVFN_GLYPH + data_len;
    uint8_t *b = kcalloc(1, n);
    if (!b) return NULL;
    memcpy(b, "NVFN", 4);
    put16(b + 4, 1);
    put16(b + 6, (uint32_t)f->height);
    put16(b + 8, (uint32_t)f->ascent);
    put16(b + 10, (uint32_t)f->mono);
    put16(b + 12, (uint32_t)f->first);
    put16(b + 14, (uint32_t)f->count);
    put32(b + 16, data_len);
    strlcpy((char *)b + 20, f->name, 16);
    uint8_t *r = b + NVFN_HDR;
    for (int i = 0; i < f->count; i++, r += NVFN_GLYPH) {
        const glyph_t *g = &f->glyphs[i];
        r[0] = g->adv;
        r[1] = (uint8_t)g->xoff;
        r[2] = (uint8_t)g->yoff;
        r[3] = g->w;
        r[4] = g->h;
        put32(r + 8, g->off);
    }
    memcpy(r, f->data, data_len);
    *len = n;
    return b;
}

/* Read a file of the drive, unpacking it if it is NVLZ. kmalloc'd, NULL if missing / bad. */
static uint8_t *read_sysfile(const char *path, uint32_t *len) {
    uint32_t n = 0;
    uint8_t *d = vfs_read_file(path, &n);
    if (!d) return NULL;
    if (!nvlz_is(d, n)) { *len = n; return d; }
    uint8_t *raw = nvlz_unpack(d, n, len);
    kfree(d);
    return raw;
}

static const font_t *font_load(const char *path, const font_t *builtin) {
    uint32_t n = 0;
    uint8_t *b = read_sysfile(path, &n);
    if (!b) return NULL;
    font_t *f = NULL;
    glyph_t *g = NULL;
    if (n < NVFN_HDR || memcmp(b, "NVFN", 4) || get16(b + 4) != 1) goto bad;
    int height = (int)get16(b + 6), ascent = (int)get16(b + 8), mono = (int)get16(b + 10);
    int first = (int)get16(b + 12), count = (int)get16(b + 14);
    uint32_t data_len = get32(b + 16);
    if (height < 4 || height > 255 || ascent > height || mono > 255 || count < 1 || first + count > 256 ||
        first > '?' || first + count <= '?' || n != NVFN_HDR + (uint32_t)count * NVFN_GLYPH + data_len)
        goto bad;
    f = kcalloc(1, sizeof *f);
    g = kmalloc(sizeof *g * (size_t)count);
    if (!f || !g) goto bad;
    const uint8_t *r = b + NVFN_HDR;
    for (int i = 0; i < count; i++, r += NVFN_GLYPH) {
        g[i].adv = r[0];
        g[i].xoff = (int8_t)r[1];
        g[i].yoff = (int8_t)r[2];
        g[i].w = r[3];
        g[i].h = r[4];
        g[i].off = get32(r + 8);
        if (g[i].off + (uint32_t)g[i].w * g[i].h > data_len) goto bad;
    }
    f->name = builtin->name;
    f->height = height;
    f->ascent = ascent;
    f->mono = mono;
    f->first = first;
    f->count = count;
    f->glyphs = g;
    f->data = r;             /* points into b, which stays allocated with the font */
    return f;
bad:
    klog("ui: %s is damaged - using the built-in font", path);
    kfree(f);
    kfree(g);
    kfree(b);
    return NULL;
}

/* ---- themes ----------------------------------------------------------------------------- */
#define TK(name) { #name, offsetof(theme_t, name) }
static const struct { const char *key; size_t off; } theme_keys[] = {
    TK(window_bg), TK(window_text), TK(window_border), TK(panel_bg),
    TK(title_active), TK(title_inactive), TK(title_text), TK(title_text_inactive),
    TK(button_bg), TK(button_hover), TK(button_pressed), TK(button_border), TK(button_text),
    TK(field_bg), TK(field_border), TK(field_text), TK(disabled_text),
    TK(menu_bg), TK(menu_text), TK(menu_separator),
    TK(taskbar_bg), TK(taskbar_text), TK(taskbar_hover), TK(taskbar_active),
    TK(desktop_text), TK(scrollbar_bg), TK(scrollbar_thumb),
};
#define NTHEMEKEYS ARRAY_SIZE(theme_keys)
static const char *const theme_names[2] = { "light", "dark" };

static struct { bool loaded; uint32_t set; uint32_t v[NTHEMEKEYS]; } themes[2];

static char *theme_text(bool dark) {
    theme_t t;
    theme_builtin_colors(dark, &t);
    size_t cap = 160 + NTHEMEKEYS * 32, len;
    char *s = kmalloc(cap);
    if (!s) return NULL;
    len = (size_t)snprintf(s, cap, "# NovaOS theme: %s mode\n# key=RRGGBB, or AARRGGBB with transparency\n",
                           dark ? "dark" : "light");
    for (int i = 0; i < NTHEMEKEYS; i++) {
        uint32_t c = *(const uint32_t *)((const char *)&t + theme_keys[i].off);
        if (COLOR_A(c) == 255) len += (size_t)snprintf(s + len, cap - len, "%s=%06X\n", theme_keys[i].key, c & 0xFFFFFF);
        else len += (size_t)snprintf(s + len, cap - len, "%s=%08X\n", theme_keys[i].key, c);
    }
    return s;
}

/* Split "key=value" lines; calls fn for each (comments and blank lines skipped). */
static void each_line(char *text, void (*fn)(const char *k, const char *v, void *ctx), void *ctx) {
    char *save = NULL;
    for (char *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        char *ke = eq;
        while (ke > line && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        *ke = 0;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        size_t vl = strlen(v);
        while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) v[--vl] = 0;
        fn(line, v, ctx);
    }
}

static bool parse_color(const char *v, uint32_t *out) {
    char *e;
    size_t n = strlen(v);
    if (n != 6 && n != 8) return false;
    uint32_t c = (uint32_t)strtoul(v, &e, 16);
    if (*e) return false;
    *out = n == 6 ? c | 0xFF000000u : c;
    return true;
}

static void theme_kv(const char *k, const char *v, void *ctx) {
    int m = (int)(intptr_t)ctx;
    for (int i = 0; i < NTHEMEKEYS; i++) {
        if (strcmp(k, theme_keys[i].key)) continue;
        uint32_t c;
        if (parse_color(v, &c)) { themes[m].v[i] = c; themes[m].set |= 1u << i; }
        return;
    }
}

void uifiles_theme_override(theme_t *t) {
    int m = t->dark ? 1 : 0;
    if (!themes[m].loaded) return;
    for (int i = 0; i < NTHEMEKEYS; i++)
        if (themes[m].set & (1u << i)) *(uint32_t *)((char *)t + theme_keys[i].off) = themes[m].v[i];
    /* the menu hover colour follows the accent over the menu background */
    t->menu_hover = color_mix(t->menu_bg, t->accent | 0xFF000000u, t->dark ? 90 : 50);
}

/* ---- wallpapers ----------------------------------------------------------------------------- */
static const struct { const char *file; const char *text; } wall_files[] = {
    { "nova.wallpaper",
      "# NovaOS wallpaper: Nova - deep space with nebula glow\n"
      "top=0A0C26\nbottom=120828\n"
      "# blob=X Y RADIUS COLOUR STRENGTH (X, Y, RADIUS: parts of the screen height)\n"
      "blob=0.22 0.30 0.55 6E46E6 0.75\nblob=0.78 0.25 0.45 288CFF 0.65\n"
      "blob=0.62 0.80 0.60 DC46AA 0.55\nblob=0.10 0.90 0.40 1EBEC8 0.40\n"
      "# stars=PIXELS-PER-STAR SEED HEIGHT-NUMERATOR HEIGHT-DENOMINATOR\n"
      "stars=2600 1234567 1 1\n" },
    { "aurora.wallpaper",
      "# NovaOS wallpaper: Aurora\n"
      "top=061222\nbottom=041E28\n"
      "blob=0.30 0.35 0.50 28DCA0 0.70\nblob=0.65 0.28 0.45 3CB4FF 0.55\nblob=0.85 0.50 0.35 965AF0 0.45\n"
      "stars=5000 7654321 2 3\n"
      "# ridge=BASE AMPLITUDE FREQ1 FREQ2 PHASE TOP-COLOUR BOTTOM-COLOUR\n"
      "ridge=0.80 0.05 1.3 3.1 0.7 0A1E28 040C12\nridge=0.87 0.04 2.1 4.3 2.2 040E14 02060A\n" },
    { "sunset.wallpaper",
      "# NovaOS wallpaper: Sunset\n"
      "top=3C2878\nbottom=FA785A\n"
      "blob=0.50 0.62 0.35 FFD278 0.85\nblob=0.20 0.20 0.60 783CA0 0.40\n"
      "ridge=0.68 0.06 1.1 2.7 0.3 783C6E 5A285A\nridge=0.76 0.05 1.7 3.9 1.9 462350 32193C\n"
      "ridge=0.85 0.04 2.3 5.1 4.0 241430 140C1E\n" },
    { "ocean.wallpaper",
      "# NovaOS wallpaper: Ocean\n"
      "top=46A0E6\nbottom=0A3C82\n"
      "blob=0.70 0.18 0.40 AAE6FF 0.6\nblob=0.25 0.55 0.50 1478C8 0.4\n"
      "ridge=0.62 0.025 2.0 5.0 0.0 2878C8 1450A0\nridge=0.72 0.03 1.6 4.0 1.0 195FAF 0C3C82\n"
      "ridge=0.83 0.03 2.4 3.3 2.0 0C468C 06285F\n" },
    { "midnight.wallpaper",
      "# NovaOS wallpaper: Midnight - dark minimal gradient, glow in the accent colour\n"
      "top=101218\nbottom=0A0B0F\n"
      "blob=0.50 1.05 0.70 accent 0.45\nblob=0.15 0.10 0.40 3C466E 0.35\n" },
    { "solid.wallpaper",
      "# NovaOS wallpaper: Solid colour - the chosen colour with a gentle vertical shade\n"
      "solid=18\n" },
};
#define NWALLS ARRAY_SIZE(wall_files)

#define MAX_BLOBS 8
#define MAX_RIDGES 6
typedef struct { double x, y, r, strength; uint32_t c; bool accent; } wblob_t;
typedef struct { double base, amp, f1, f2, phase; uint32_t c, c2; } wridge_t;
typedef struct {
    bool loaded, solid;
    int shade;
    uint32_t top, bottom;
    int nblobs, nridges;
    wblob_t blobs[MAX_BLOBS];
    wridge_t ridges[MAX_RIDGES];
    int star_div, star_num, star_den;
    uint32_t star_seed;
} wallpaper_t;

static wallpaper_t walls[NWALLS];

/* Next number in a line ("0.22 0.30 ..."). */
static bool next_num(const char **p, double *out) {
    char *e;
    while (**p == ' ' || **p == '\t') (*p)++;
    double v = strtod(*p, &e);
    if (e == *p) return false;
    *p = e;
    *out = v;
    return true;
}

static bool next_color(const char **p, uint32_t *out, bool *accent) {
    while (**p == ' ' || **p == '\t') (*p)++;
    char tok[16];
    int n = 0;
    while (**p && **p != ' ' && **p != '\t' && n < 15) tok[n++] = *(*p)++;
    tok[n] = 0;
    if (accent && !strcmp(tok, "accent")) { *accent = true; *out = 0; return true; }
    if (accent) *accent = false;
    return parse_color(tok, out);
}

static void wall_kv(const char *k, const char *v, void *ctx) {
    wallpaper_t *w = ctx;
    const char *p = v;
    double a[5];
    if (!strcmp(k, "top")) parse_color(v, &w->top);
    else if (!strcmp(k, "bottom")) parse_color(v, &w->bottom);
    else if (!strcmp(k, "solid")) { w->solid = true; w->shade = atoi(v); }
    else if (!strcmp(k, "blob") && w->nblobs < MAX_BLOBS) {
        wblob_t *b = &w->blobs[w->nblobs];
        if (next_num(&p, &b->x) && next_num(&p, &b->y) && next_num(&p, &b->r) && b->r > 0 &&
            next_color(&p, &b->c, &b->accent) && next_num(&p, &b->strength))
            w->nblobs++;
    } else if (!strcmp(k, "ridge") && w->nridges < MAX_RIDGES) {
        wridge_t *r = &w->ridges[w->nridges];
        if (next_num(&p, &a[0]) && next_num(&p, &a[1]) && next_num(&p, &a[2]) && next_num(&p, &a[3]) &&
            next_num(&p, &a[4]) && next_color(&p, &r->c, NULL) && next_color(&p, &r->c2, NULL)) {
            r->base = a[0]; r->amp = a[1]; r->f1 = a[2]; r->f2 = a[3]; r->phase = a[4];
            w->nridges++;
        }
    } else if (!strcmp(k, "stars")) {
        if (next_num(&p, &a[0]) && next_num(&p, &a[1]) && next_num(&p, &a[2]) && next_num(&p, &a[3]) &&
            a[0] >= 1 && a[3] >= 1) {
            w->star_div = (int)a[0];
            w->star_seed = (uint32_t)a[1];
            w->star_num = (int)a[2];
            w->star_den = (int)a[3];
        }
    }
}

/* The drawing below is the same as theme.c's built-in wallpapers: a smooth gradient
   with glowing blobs rendered at 1/8 size and scaled up, stars, ridges. */
static void render_blobs(surface_t *s, const wallpaper_t *w) {
    int lw = MAX(2, s->w / 8), lh = MAX(2, s->h / 8);
    uint32_t top = w->top | 0xFF000000u, bottom = w->bottom | 0xFF000000u;
    surface_t *lo = surface_create(lw, lh);
    if (!lo) { gfx_gradient_v(s, 0, 0, s->w, s->h, top, bottom); return; }
    uint32_t accent = theme_get()->accent | 0xFF000000u;
    for (int y = 0; y < lh; y++) {
        uint32_t base = color_mix(top, bottom, y * 255 / (lh - 1));
        double fy = (double)y / (lh - 1);
        for (int x = 0; x < lw; x++) {
            double fx = (double)x / (lw - 1);
            double r = COLOR_R(base), g = COLOR_G(base), b = COLOR_B(base);
            for (int i = 0; i < w->nblobs; i++) {
                const wblob_t *bl = &w->blobs[i];
                uint32_t c = bl->accent ? accent : bl->c;
                double dx = (fx - bl->x) * ((double)s->w / s->h), dy = fy - bl->y;
                double d2 = (dx * dx + dy * dy) / (bl->r * bl->r);
                if (d2 >= 1.0) continue;
                double k = (1.0 - d2);
                k = k * k * bl->strength;
                r += (COLOR_R(c) - r) * k;
                g += (COLOR_G(c) - g) * k;
                b += (COLOR_B(c) - b) * k;
            }
            surface_row(lo, y)[x] = RGB((int)r, (int)g, (int)b);
        }
    }
    gfx_blit_scaled(s, 0, 0, s->w, s->h, lo, 0, 0, lw, lh, true);
    surface_destroy(lo);
}

static void render_stars(surface_t *s, int count, uint32_t seed, int max_y) {
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

static void render_ridge(surface_t *s, const wridge_t *rg) {
    int w = s->w, h = s->h;
    uint32_t c = rg->c, c2 = rg->c2;
    for (int x = 0; x < w; x++) {
        double fx = (double)x / w;
        double y = rg->base + rg->amp * (0.6 * sin(fx * rg->f1 * 2 * M_PI + rg->phase) +
                                         0.4 * sin(fx * rg->f2 * 2 * M_PI + rg->phase * 1.7));
        int yi = (int)(y * h);
        if (yi < 0) yi = 0;
        if (yi >= h) continue;
        double frac = y * h - floor(y * h);
        gfx_pixel(s, x, yi, color_alpha(c, (int)(255 * (1.0 - frac))));
        for (int yy = yi + 1; yy < h; yy++) {
            uint32_t col = c2 == c ? c : color_mix(c, c2, (yy - yi) * 255 / MAX(1, h - yi));
            surface_row(s, yy)[x] = col | 0xFF000000u;
        }
    }
}

bool uifiles_render_wallpaper(surface_t *s, int idx) {
    if (idx < 0 || idx >= NWALLS || !walls[idx].loaded) return false;
    const wallpaper_t *w = &walls[idx];
    if (w->solid) {
        uint32_t c = theme_get()->wallpaper_color | 0xFF000000u;
        gfx_gradient_v(s, 0, 0, s->w, s->h, color_shade(c, w->shade), color_shade(c, -w->shade));
        return true;
    }
    render_blobs(s, w);
    if (w->star_div > 0) render_stars(s, s->w * s->h / w->star_div, w->star_seed, s->h * w->star_num / w->star_den);
    for (int i = 0; i < w->nridges; i++) render_ridge(s, &w->ridges[i]);
    return true;
}

/* ---- UI settings ------------------------------------------------------------------------------ */
static const char *ui_cfg_text(void) {
    return "# NovaOS UI settings, read at startup. The command line (dark=, wallpaper=)\n"
           "# and the choices saved by Settings win over them.\n"
           "dark=0\naccent=3B82F6\nwallpaper=0\nwallcolor=285A96\nshadows=1\n";
}

typedef struct { int dark, wallpaper, shadows; uint32_t accent, wallcolor; bool has_accent, has_wallcolor; } uicfg_t;

static void ui_kv(const char *k, const char *v, void *ctx) {
    uicfg_t *c = ctx;
    if (!strcmp(k, "dark")) c->dark = atoi(v) != 0;
    else if (!strcmp(k, "wallpaper")) c->wallpaper = atoi(v);
    else if (!strcmp(k, "shadows")) c->shadows = atoi(v) != 0;
    else if (!strcmp(k, "accent")) c->has_accent = parse_color(v, &c->accent);
    else if (!strcmp(k, "wallcolor")) c->has_wallcolor = parse_color(v, &c->wallcolor);
}

/* ---- the files ---------------------------------------------------------------------------------- */
static void path_of(char *out, const char *sub, const char *name) {
    snprintf(out, VFS_PATH_MAX, UI_DIR "%s%s", sub, name);
}

int uifiles_count(void) { return 1 + NFONTS + 2 + NWALLS; }

/* The i-th UI file (0 .. uifiles_count()-1) into out. */
void uifiles_path(int i, char *out) {
    if (i == 0) { path_of(out, "/", "ui.cfg"); return; }
    i--;
    if (i < NFONTS) { path_of(out, "/fonts/", font_files[i].file); return; }
    i -= NFONTS;
    if (i < 2) { char n[16]; snprintf(n, sizeof n, "%s.theme", theme_names[i]); path_of(out, "/themes/", n); return; }
    i -= 2;
    path_of(out, "/wallpapers/", wall_files[i].file);
}

static void write_text(const char *path, const char *text) {
    if (text) vfs_write_file(path, text, (uint32_t)strlen(text));
}

/* Write /system/ui from the built-in copies. */
void uifiles_seed(void) {
    char path[VFS_PATH_MAX];
    vfs_mkdirs(UI_DIR "/fonts");
    vfs_mkdirs(UI_DIR "/themes");
    vfs_mkdirs(UI_DIR "/wallpapers");
    path_of(path, "/", "ui.cfg");
    write_text(path, ui_cfg_text());
    uint32_t total = 0;
    for (int i = 0; i < NFONTS; i++) {
        uint32_t raw_len = 0, packed_len = 0;
        uint8_t *raw = font_serialize(font_files[i].builtin, &raw_len);
        uint8_t *packed = raw ? nvlz_pack(raw, raw_len, &packed_len) : NULL;
        path_of(path, "/fonts/", font_files[i].file);
        if (packed) { vfs_write_file(path, packed, packed_len); total += packed_len; }
        else if (raw) vfs_write_file(path, raw, raw_len);
        kfree(packed);
        kfree(raw);
    }
    for (int m = 0; m < 2; m++) {
        char name[16];
        snprintf(name, sizeof name, "%s.theme", theme_names[m]);
        path_of(path, "/themes/", name);
        char *t = theme_text(m == 1);
        write_text(path, t);
        kfree(t);
    }
    for (int i = 0; i < NWALLS; i++) {
        path_of(path, "/wallpapers/", wall_files[i].file);
        write_text(path, wall_files[i].text);
    }
    klog("ui: wrote the UI files (%d fonts, %u bytes packed)", NFONTS, total);
}

/* Load fonts, themes, wallpapers and UI settings from /system/ui. */
void uifiles_load(void) {
    char path[VFS_PATH_MAX];
    uint32_t t0 = g_ticks;
    int nf = 0, nt = 0, nw = 0;
    font_map_n = 0;
    for (int i = 0; i < NFONTS; i++) {
        path_of(path, "/fonts/", font_files[i].file);
        const font_t *f = font_load(path, font_files[i].builtin);
        if (!f || font_map_n >= FONT_MAP_MAX) continue;
        font_map_from[font_map_n] = font_files[i].builtin;
        font_map_to[font_map_n] = f;
        font_map_n++;
        nf++;
    }
    for (int m = 0; m < 2; m++) {
        char name[16];
        snprintf(name, sizeof name, "%s.theme", theme_names[m]);
        path_of(path, "/themes/", name);
        char *text = vfs_read_file(path, NULL);
        memset(&themes[m], 0, sizeof themes[m]);
        if (!text) continue;
        each_line(text, theme_kv, (void *)(intptr_t)m);
        kfree(text);
        themes[m].loaded = themes[m].set != 0;
        if (themes[m].loaded) nt++;
        else klog("ui: %s has no colours - using the built-in theme", path);
    }
    for (int i = 0; i < NWALLS; i++) {
        path_of(path, "/wallpapers/", wall_files[i].file);
        char *text = vfs_read_file(path, NULL);
        memset(&walls[i], 0, sizeof walls[i]);
        if (!text) continue;
        each_line(text, wall_kv, &walls[i]);
        kfree(text);
        walls[i].loaded = walls[i].solid || walls[i].nblobs > 0 || walls[i].nridges > 0;
        if (walls[i].loaded) nw++;
        else klog("ui: %s describes no wallpaper - using the built-in one", path);
    }
    /* colours of the loaded theme */
    theme_refresh();
    /* UI settings (the command line and the user's Settings, applied later, win) */
    path_of(path, "/", "ui.cfg");
    char *text = vfs_read_file(path, NULL);
    if (text) {
        uicfg_t c = { .dark = -1, .wallpaper = -2, .shadows = -1 };
        each_line(text, ui_kv, &c);
        kfree(text);
        const theme_t *t = theme_get();
        if (c.dark >= 0 && !sys_cmdline_get("dark") && (c.dark != 0) != t->dark) theme_set_dark(c.dark != 0);
        if (c.has_accent && (c.accent | 0xFF000000u) != (t->accent | 0xFF000000u)) theme_set_accent(c.accent);
        if (c.has_wallcolor && (c.wallcolor | 0xFF000000u) != (t->wallpaper_color | 0xFF000000u))
            theme_set_wallpaper_color(c.wallcolor);
        if (c.wallpaper >= 0 && c.wallpaper < theme_wallpaper_count() && !sys_cmdline_get("wallpaper") &&
            c.wallpaper != t->wallpaper)
            theme_set_wallpaper(c.wallpaper);
        if (c.shadows >= 0 && (c.shadows != 0) != t->shadows) theme_set_shadows(c.shadows != 0);
    }
    klog("ui: loaded %d fonts, %d themes, %d wallpapers and the UI settings from " UI_DIR " in %u ms", nf, nt, nw,
         g_ticks - t0);
}
