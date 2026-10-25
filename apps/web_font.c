/*
 * web_font.c - the Web Browser's page fonts.
 *
 * tools/genwebfont.py renders Liberation Sans (regular, bold, italic, bold
 * italic) and Liberation Mono (regular, bold) at 9-24 px plus a 48 px master
 * for the proportional styles, and stores each one as base64(zlib(...)) in
 * apps/web_fontdata.c. A font is unpacked into a normal font_t the first time
 * a page uses it, so the usual gfx_text / font_text_width calls work with it.
 * Sizes above 24 px are made from the 48 px master by area-averaging.
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
    if (!WEB_HAVE(inflate_buffer)) return NULL;
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

/* 1-D area resampling of n source samples into m (scale k = m/n). */
static void resample(const float *src, int n, float *dst, int m, float k, float o) {
    for (int i = 0; i < m; i++) {
        float a = o + i / k, b = o + (i + 1) / k;   /* source interval */
        int ia = (int)floorf(a), ib = (int)ceilf(b);
        float sum = 0;
        for (int j = ia; j < ib; j++) {
            if (j < 0 || j >= n) continue;
            float lo = j < a ? a : (float)j;
            float hi = j + 1 > b ? b : (float)(j + 1);
            if (hi > lo) sum += src[j] * (hi - lo);
        }
        dst[i] = sum * k;
    }
}

/* Scale the 48 px master to px. */
static font_t *font_scaled(const font_t *m, int masterpx, int px) {
    float k = (float)px / masterpx;
    int count = m->count;
    glyph_t *g = calloc(count, sizeof *g);
    font_t *f = calloc(1, sizeof *f);
    if (!g || !f) { free(g); free(f); return NULL; }
    /* first pass: sizes */
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        const glyph_t *s = &m->glyphs[i];
        g[i].adv = (uint8_t)CLAMP((int)lroundf(s->adv * k), 0, 255);
        if (!s->w || !s->h) { g[i].xoff = (int8_t)lroundf(s->xoff * k); g[i].yoff = (int8_t)lroundf(s->yoff * k); continue; }
        float x0 = s->xoff * k, x1 = (s->xoff + s->w) * k;
        float y0 = s->yoff * k, y1 = (s->yoff + s->h) * k;
        int tx0 = (int)floorf(x0), tx1 = (int)ceilf(x1);
        int ty0 = (int)floorf(y0), ty1 = (int)ceilf(y1);
        g[i].xoff = (int8_t)CLAMP(tx0, -128, 127);
        g[i].yoff = (int8_t)CLAMP(ty0, -128, 127);
        g[i].w = (uint8_t)CLAMP(tx1 - tx0, 1, 255);
        g[i].h = (uint8_t)CLAMP(ty1 - ty0, 1, 255);
        total += (size_t)g[i].w * g[i].h;
    }
    uint8_t *data = calloc(1, total + 1);
    float *row = malloc(sizeof(float) * 512), *tmp = malloc(sizeof(float) * 512);
    float *mid = malloc(sizeof(float) * 256 * 256);
    if (!data || !row || !tmp || !mid) { free(g); free(f); free(data); free(row); free(tmp); free(mid); return NULL; }
    uint32_t off = 0;
    for (int i = 0; i < count; i++) {
        const glyph_t *s = &m->glyphs[i];
        g[i].off = off;
        if (!s->w || !s->h) continue;
        int W = g[i].w, H = g[i].h;
        /* source placed in a padded frame whose origin maps to (xoff, yoff) of the target */
        float sx0 = g[i].xoff / k, sy0 = g[i].yoff / k;   /* master coords of target origin */
        int padx = (int)floorf(s->xoff - sx0), pady = (int)floorf(s->yoff - sy0);
        if (padx < 0) padx = 0;
        if (pady < 0) pady = 0;
        float fx = (s->xoff - sx0) - padx, fy = (s->yoff - sy0) - pady;
        int SW = s->w + padx + 2, SH = s->h + pady + 2;
        if (SW > 512 || SH > 512 || W > 256 || H > 256) { off += W * H; continue; }
        const uint8_t *bits = m->data + s->off;
        /* horizontal pass: each source row -> W samples */
        for (int y = 0; y < SH; y++) {
            for (int x = 0; x < SW; x++) {
                int bx = x - padx, by = y - pady;
                row[x] = (bx >= 0 && by >= 0 && bx < s->w && by < s->h) ? bits[by * s->w + bx] : 0.0f;
            }
            resample(row, SW, tmp, W, k, -fx);
            for (int x = 0; x < W; x++) mid[y * W + x] = tmp[x];
        }
        /* vertical pass */
        for (int x = 0; x < W; x++) {
            for (int y = 0; y < SH; y++) row[y] = mid[y * W + x];
            resample(row, SH, tmp, H, k, -fy);
            for (int y = 0; y < H; y++) {
                int v = (int)(tmp[y] + 0.5f);
                data[off + y * W + x] = (uint8_t)CLAMP(v, 0, 255);
            }
        }
        off += W * H;
    }
    free(row); free(tmp); free(mid);
    f->name = "web";
    f->height = (int)lroundf(m->height * k);
    f->ascent = (int)lroundf(m->ascent * k);
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
        if (!WEB_HAVE(inflate_buffer) || px <= 24) {
            if (!failed) klog("browser: page fonts unavailable, using system fonts");
            failed = true;
        }
        return fallback(px, flags);
    }
    cache[style][px] = f;
    return f;
}

bool web_fonts_real(void) {
    if (failed) return false;
    return WEB_HAVE(inflate_buffer);
}
