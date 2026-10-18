/* image.c - BMP (24/32-bit, 8-bit palettized, uncompressed) and PPM (P6/P3) decoding, BMP encoding. */
#include "kernel.h"

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static surface_t *decode_bmp(const uint8_t *d, size_t size) {
    if (size < 54 || d[0] != 'B' || d[1] != 'M') return NULL;
    uint32_t off = rd32(d + 10);
    uint32_t hdr = rd32(d + 14);
    int32_t w = (int32_t)rd32(d + 18), h = (int32_t)rd32(d + 22);
    uint16_t bpp = rd16(d + 28);
    uint32_t comp = rd32(d + 30);
    if (hdr < 40 || w <= 0 || w > 8192 || h == 0 || h > 8192 || h < -8192) return NULL;
    if (comp != 0 && comp != 3) return NULL;
    bool top_down = h < 0;
    if (h < 0) h = -h;
    if (bpp != 24 && bpp != 32 && bpp != 8) return NULL;
    uint32_t stride = (((uint32_t)w * bpp + 31) / 32) * 4;
    if (off + stride * (uint32_t)h > size) return NULL;
    const uint8_t *pal = d + 14 + hdr;
    surface_t *s = (bpp == 32) ? surface_create_alpha(w, h) : surface_create(w, h);
    if (!s) return NULL;
    bool any_alpha = false;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = d + off + stride * (uint32_t)(top_down ? y : h - 1 - y);
        uint32_t *out = surface_row(s, y);
        for (int x = 0; x < w; x++) {
            if (bpp == 24) out[x] = RGB(row[x * 3 + 2], row[x * 3 + 1], row[x * 3]);
            else if (bpp == 32) {
                out[x] = RGBA(row[x * 4 + 2], row[x * 4 + 1], row[x * 4], row[x * 4 + 3]);
                if (row[x * 4 + 3]) any_alpha = true;
            } else {
                const uint8_t *e = pal + row[x] * 4;
                out[x] = RGB(e[2], e[1], e[0]);
            }
        }
    }
    if (bpp == 32 && !any_alpha) {
        /* alpha channel unused: make it opaque */
        for (int y = 0; y < h; y++) {
            uint32_t *out = surface_row(s, y);
            for (int x = 0; x < w; x++) out[x] |= 0xFF000000u;
        }
        s->flags &= ~SURF_ALPHA;
    }
    return s;
}

static const uint8_t *ppm_token(const uint8_t *p, const uint8_t *end, int *val) {
    while (p < end) {
        if (*p == '#') { while (p < end && *p != '\n') p++; }
        else if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        else break;
    }
    int v = 0, any = 0;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
    *val = any ? v : -1;
    return p;
}

static surface_t *decode_ppm(const uint8_t *d, size_t size) {
    if (size < 8 || d[0] != 'P' || (d[1] != '6' && d[1] != '3')) return NULL;
    const uint8_t *end = d + size, *p = d + 2;
    int w, h, maxv;
    p = ppm_token(p, end, &w);
    p = ppm_token(p, end, &h);
    p = ppm_token(p, end, &maxv);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || maxv <= 0 || maxv > 255) return NULL;
    surface_t *s = surface_create(w, h);
    if (!s) return NULL;
    if (d[1] == '6') {
        p++;
        if ((size_t)(end - p) < (size_t)w * (size_t)h * 3) { surface_destroy(s); return NULL; }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++, p += 3) surface_row(s, y)[x] = RGB(p[0] * 255 / maxv, p[1] * 255 / maxv, p[2] * 255 / maxv);
    } else {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int r, g, b;
                p = ppm_token(p, end, &r); p = ppm_token(p, end, &g); p = ppm_token(p, end, &b);
                if (b < 0) { surface_destroy(s); return NULL; }
                surface_row(s, y)[x] = RGB(r * 255 / maxv, g * 255 / maxv, b * 255 / maxv);
            }
    }
    return s;
}

surface_t *image_decode(const void *data, size_t size) {
    const uint8_t *d = data;
    if (!d || size < 4) return NULL;
    if (d[0] == 'B' && d[1] == 'M') return decode_bmp(d, size);
    if (d[0] == 'P') return decode_ppm(d, size);
    return NULL;
}

void *image_encode_bmp(const surface_t *s, size_t *out_size) {
    uint32_t stride = ((uint32_t)s->w * 3 + 3) & ~3u;
    uint32_t img = stride * (uint32_t)s->h;
    uint32_t total = 54 + img;
    uint8_t *b = kcalloc(1, total);
    if (!b) return NULL;
    b[0] = 'B'; b[1] = 'M';
    memcpy(b + 2, &total, 4);
    uint32_t v = 54; memcpy(b + 10, &v, 4);
    v = 40; memcpy(b + 14, &v, 4);
    v = (uint32_t)s->w; memcpy(b + 18, &v, 4);
    v = (uint32_t)s->h; memcpy(b + 22, &v, 4);
    b[26] = 1; b[28] = 24;
    memcpy(b + 34, &img, 4);
    v = 2835; memcpy(b + 38, &v, 4); memcpy(b + 42, &v, 4);
    for (int y = 0; y < s->h; y++) {
        uint8_t *row = b + 54 + stride * (uint32_t)(s->h - 1 - y);
        const uint32_t *src = surface_row(s, y);
        for (int x = 0; x < s->w; x++) {
            row[x * 3] = (uint8_t)COLOR_B(src[x]);
            row[x * 3 + 1] = (uint8_t)COLOR_G(src[x]);
            row[x * 3 + 2] = (uint8_t)COLOR_R(src[x]);
        }
    }
    if (out_size) *out_size = total;
    return b;
}
