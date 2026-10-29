/*
 * web_font.c - the Web Browser's page fonts.
 *
 * tools/genwebfont.py renders Liberation Sans (regular, bold, italic, bold
 * italic) and Liberation Mono (regular, bold) at 9-24 px plus a 48 px master
 * for the proportional styles, and stores each one as base64(zlib(...)) in
 * apps/web_fontdata.c. A font is unpacked into a normal font_t the first time
 * a page uses it, so the usual gfx_text / font_text_width calls work with it.
 * Sizes above 24 px are made from the 48 px master (4 x 4 supersampling).
 */
#include "web.h"

#define MAXPX 96
#define NSTYLES 6

static const font_t *cache[NSTYLES][MAXPX + 1];
static bool failed;

static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint8_t *b64_decode(const char *s, uint32_t *outlen) {
    size_t n = strlen(s);
    uint8_t *out = malloc(n * 3 / 4 + 4);
    if (!out) return NULL;
    uint32_t acc = 0, o = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64v((unsigned char)s[i]);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (uint8_t)(acc >> bits); }
    }
    *outlen = o;
    return out;
}

/* Unpack a pack into (table, coverage) bytes. */
static uint8_t *unpack(const web_fontpack_t *p) {
    uint32_t zl;
    uint8_t *z = b64_decode(p->b64, &zl);
    if (!z) return NULL;
    uint8_t *raw = NULL;
    uint32_t rl = 0;
    int r = inflate_buffer(z, zl, INFLATE_ZLIB, &raw, &rl, p->rawlen + 16);
    free(z);
    if (r != 0 || rl != p->rawlen) { free(raw); return NULL; }
    return raw;
}

static font_t *font_from_raw(const web_fontpack_t *p, uint8_t *raw) {
    int count = 224;
    font_t *f = calloc(1, sizeof *f);
    glyph_t *g = calloc(count, sizeof *g);
    if (!f || !g) { free(f); free(g); return NULL; }
    uint32_t off = 0;
    int mono = 0;
    for (int i = 0; i < count; i++) {
        const uint8_t *t = raw + i * 5;
        g[i].adv = t[0];
        g[i].xoff = (int8_t)(t[1] - 128);
        g[i].yoff = (int8_t)(t[2] - 128);
        g[i].w = t[3];
        g[i].h = t[4];
        g[i].off = off;
        off += (uint32_t)t[3] * t[4];
        if (i == 'M' - 32) mono = t[0];
    }
    f->name = "web";
    f->height = p->height;
    f->ascent = p->ascent;
    f->mono = (p->style >= 4) ? mono : 0;
    f->first = 32;
    f->count = count;
    f->glyphs = g;
    f->data = raw + count * 5;
    return f;
}

static int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static int ceil_div(int a, int b) { return -floor_div(-a, b); }

/* Scale the 48 px master to px: every target pixel averages 4 x 4 samples of
   the master (integer arithmetic only - the x87 is slow under emulation). */
static font_t *font_scaled(const font_t *m, int masterpx, int px) {
    int count = m->count;
    glyph_t *g = calloc(count, sizeof *g);
    font_t *f = calloc(1, sizeof *f);
    if (!g || !f) { free(g); free(f); return NULL; }
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        const glyph_t *s = &m->glyphs[i];
        g[i].adv = (uint8_t)CLAMP((s->adv * px + masterpx / 2) / masterpx, 0, 255);
        int tx0 = floor_div(s->xoff * px, masterpx), ty0 = floor_div(s->yoff * px, masterpx);
        g[i].xoff = (int8_t)CLAMP(tx0, -128, 127);
        g[i].yoff = (int8_t)CLAMP(ty0, -128, 127);
        if (!s->w || !s->h) continue;
        int tx1 = ceil_div((s->xoff + s->w) * px, masterpx), ty1 = ceil_div((s->yoff + s->h) * px, masterpx);
        g[i].w = (uint8_t)CLAMP(tx1 - tx0, 1, 255);
        g[i].h = (uint8_t)CLAMP(ty1 - ty0, 1, 255);
        total += (size_t)g[i].w * g[i].h;
    }
    uint8_t *data = calloc(1, total + 1);
    int *cols = malloc(sizeof(int) * 256 * 4), *rows = malloc(sizeof(int) * 256 * 4);
    if (!data || !cols || !rows) { free(g); free(f); free(data); free(cols); free(rows); return NULL; }
    uint32_t off = 0;
    for (int i = 0; i < count; i++) {
        const glyph_t *s = &m->glyphs[i];
        g[i].off = off;
        if (!s->w || !s->h) continue;
        int W = g[i].w, H = g[i].h;
        /* source column / row of each sub-sample (-1 = outside the bitmap) */
        for (int x = 0; x < W * 4; x++) {
            int t8 = (g[i].xoff + x / 4) * 8 + 2 * (x % 4) + 1;
            int sx = floor_div(t8 * masterpx, 8 * px) - s->xoff;
            cols[x] = sx >= 0 && sx < s->w ? sx : -1;
        }
        for (int y = 0; y < H * 4; y++) {
            int t8 = (g[i].yoff + y / 4) * 8 + 2 * (y % 4) + 1;
            int sy = floor_div(t8 * masterpx, 8 * px) - s->yoff;
            rows[y] = sy >= 0 && sy < s->h ? sy : -1;
        }
        const uint8_t *bits = m->data + s->off;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int sum = 0;
                for (int j = 0; j < 4; j++) {
                    int sy = rows[y * 4 + j];
                    if (sy < 0) continue;
                    const uint8_t *row = bits + sy * s->w;
                    for (int k = 0; k < 4; k++) { int sx = cols[x * 4 + k]; if (sx >= 0) sum += row[sx]; }
                }
                data[off + y * W + x] = (uint8_t)(sum >> 4);
            }
        }
        off += (uint32_t)(W * H);
    }
    free(cols);
    free(rows);
    f->name = "web";
    f->height = (m->height * px + masterpx / 2) / masterpx;
    f->ascent = (m->ascent * px + masterpx / 2) / masterpx;
    f->mono = 0;
    f->first = m->first;
    f->count = count;
    f->glyphs = g;
    f->data = data;
    return f;
}

static const web_fontpack_t *find_pack(int style, int px) {
    for (int i = 0; i < web_fontpack_count; i++)
        if (web_fontpacks[i].style == style && web_fontpacks[i].px == px) return &web_fontpacks[i];
    return NULL;
}

static const font_t *fallback(int px, int flags) {
    if (flags & WF_MONO) return (flags & WF_BOLD) ? FONT_MONO_BOLD : FONT_MONO;
    if (px >= 24) return FONT_UI_TITLE;
    if (px >= 17) return FONT_UI_LARGE;
    return (flags & WF_BOLD) ? FONT_UI_BOLD : FONT_UI;
}

static const font_t *load(int style, int px) {
    const web_fontpack_t *p = find_pack(style, px);
    if (!p) return NULL;
    uint8_t *raw = unpack(p);
    if (!raw) return NULL;
    font_t *f = font_from_raw(p, raw);
    if (!f) free(raw);
    return f;
}

static const int small_sizes[] = { 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24 };

const font_t *web_font(int px, int flags) {
    int style = (flags & WF_MONO) ? 4 + ((flags & WF_BOLD) ? 1 : 0)
                                  : ((flags & WF_BOLD) ? 1 : 0) + ((flags & WF_ITALIC) ? 2 : 0);
    if (px < 9) px = 9;
    if (px > MAXPX) px = MAXPX;
    if (style >= 4 && px > 24) px = 24;
    if (px <= 24) {
        /* nearest pre-rendered size */
        int best = small_sizes[0];
        for (int i = 0; i < ARRAY_SIZE(small_sizes); i++)
            if (abs(small_sizes[i] - px) < abs(best - px) ||
                (abs(small_sizes[i] - px) == abs(best - px) && small_sizes[i] < px)) best = small_sizes[i];
        px = best;
    }
    if (cache[style][px]) return cache[style][px];
    if (failed) return fallback(px, flags);
    const font_t *f = NULL;
    if (px <= 24 || px == 48) {
        f = load(style, px);
    } else {
        const font_t *m = web_font(48, flags);
        if (m && web_fonts_real()) f = font_scaled(m, 48, px);
    }
    if (!f) {
        if (px <= 24) {
            if (!failed) klog("browser: page fonts unavailable, using system fonts");
            failed = true;
        }
        return fallback(px, flags);
    }
    cache[style][px] = f;
    return f;
}

bool web_fonts_real(void) {
    return !failed;
}
