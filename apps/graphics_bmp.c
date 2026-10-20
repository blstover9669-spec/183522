/*
 * graphics_bmp.c - BMP codec shared by Paint, Image Viewer and Fractal Explorer.
 * See graphics_bmp.h. (This file registers no app.)
 */
#include "graphics_bmp.h"

#define GBMP_MAX_DIM    16384
#define GBMP_MAX_PIXELS (24u << 20)       /* 24 Mpx: 96 MiB as ARGB */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* A channel described by a bit mask: shift to the lowest set bit, width in bits. */
typedef struct { uint32_t mask; int shift, bits; } chan_t;

static chan_t chan_make(uint32_t mask) {
    chan_t c = { mask, 0, 0 };
    if (!mask) return c;
    while (!(mask & 1)) { mask >>= 1; c.shift++; }
    while (mask & 1) { mask >>= 1; c.bits++; }
    return c;
}

/* Extract a channel and scale it to 0..255. */
static inline uint32_t chan_get(const chan_t *c, uint32_t v) {
    if (!c->bits) return 0;
    uint32_t x = (v & c->mask) >> c->shift;
    if (c->bits >= 8) return x >> (c->bits - 8);
    uint32_t max = (1u << c->bits) - 1;
    return (x * 255 + max / 2) / max;
}

static surface_t *fail(const char **err, const char *msg) {
    if (err) *err = msg;
    return NULL;
}

surface_t *gbmp_decode(const void *data, size_t size, gbmp_info_t *info, const char **err) {
    const uint8_t *d = data;
    if (err) *err = NULL;
    if (!d || size < 26) return fail(err, "The file is too short to be an image");
    if (d[0] != 'B' || d[1] != 'M') return fail(err, "Not a BMP file");
    uint32_t off = rd32(d + 10);
    uint32_t hdr = rd32(d + 14);
    if (hdr < 12 || 14 + (size_t)hdr > size) return fail(err, "Damaged BMP header");

    int32_t w, h;
    uint32_t bpp, comp = 0, colors_used = 0;
    if (hdr == 12) {                     /* BITMAPCOREHEADER (OS/2 1.x) */
        w = rd16(d + 18);
        h = (int16_t)rd16(d + 20);
        bpp = rd16(d + 24);
    } else {
        if (hdr < 40) return fail(err, "Unsupported BMP header version");
        w = (int32_t)rd32(d + 18);
        h = (int32_t)rd32(d + 22);
        bpp = rd16(d + 28);
        comp = rd32(d + 30);
        colors_used = rd32(d + 46);
    }
    bool top_down = h < 0;
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0) return fail(err, "The image has no pixels");
    if (w > GBMP_MAX_DIM || h > GBMP_MAX_DIM || (uint64_t)w * (uint64_t)h > GBMP_MAX_PIXELS)
        return fail(err, "The image is too large");

    if (comp == 1 || comp == 2) return fail(err, "RLE-compressed bitmaps are not supported");
    if (comp == 4 || comp == 5) return fail(err, "BMP files with embedded JPEG/PNG are not supported");
    if (comp != 0 && comp != 3 && comp != 6) return fail(err, "Unknown BMP compression");
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
        return fail(err, "Unsupported bit depth");
    if ((comp == 3 || comp == 6) && bpp != 16 && bpp != 32) return fail(err, "Damaged BMP (bit fields)");

    /* channel masks */
    uint32_t mr = 0, mg = 0, mb = 0, ma = 0;
    size_t after_hdr = 14 + hdr;
    if (comp == 3 || comp == 6) {
        if (hdr >= 52) {
            mr = rd32(d + 54); mg = rd32(d + 58); mb = rd32(d + 62);
            if (hdr >= 56) ma = rd32(d + 66);
        } else {                         /* masks follow a 40-byte header */
            size_t n = comp == 6 ? 16 : 12;
            if (after_hdr + n > size) return fail(err, "Damaged BMP (bit fields)");
            mr = rd32(d + after_hdr); mg = rd32(d + after_hdr + 4); mb = rd32(d + after_hdr + 8);
            if (comp == 6) ma = rd32(d + after_hdr + 12);
            after_hdr += n;
        }
    } else if (bpp == 16) {
        mr = 0x7C00; mg = 0x03E0; mb = 0x001F;
    } else if (bpp == 24 || bpp == 32) {
        mr = 0x00FF0000; mg = 0x0000FF00; mb = 0x000000FF;
        /* BI_RGB 32-bit: the 4th byte is officially unused, but many writers store
           alpha there. It is honoured below only if it actually varies. */
        if (bpp == 32) ma = 0xFF000000;
        if (hdr >= 56 && bpp == 32 && rd32(d + 66)) ma = rd32(d + 66);
    }

    /* palette */
    uint32_t pal[256];
    if (bpp <= 8) {
        uint32_t maxc = 1u << bpp;
        uint32_t n = colors_used ? colors_used : maxc;
        if (n > maxc) n = maxc;
        size_t esz = hdr == 12 ? 3 : 4;
        for (uint32_t i = 0; i < 256; i++) pal[i] = COLOR_BLACK;
        for (uint32_t i = 0; i < n; i++) {
            size_t p = after_hdr + i * esz;
            if (p + 3 > size) break;
            pal[i] = RGB(d[p + 2], d[p + 1], d[p]);
        }
    }

    uint32_t stride = (((uint32_t)w * bpp + 31) / 32) * 4;
    if (off < 14 + hdr || off >= size) return fail(err, "Damaged BMP (pixel data offset)");
    if ((uint64_t)off + (uint64_t)stride * (uint32_t)(h - 1) + ((uint32_t)w * bpp + 7) / 8 > size)
        return fail(err, "The file is truncated");

    chan_t cr = chan_make(mr), cg = chan_make(mg), cb = chan_make(mb), ca = chan_make(ma);
    surface_t *s = surface_create_alpha(w, h);
    if (!s) return fail(err, "Not enough memory to open the image");

    bool alpha_varies = false, alpha_nonzero = false;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = d + off + stride * (uint32_t)(top_down ? y : h - 1 - y);
        uint32_t *out = surface_row(s, y);
        switch (bpp) {
        case 1:
            for (int x = 0; x < w; x++) out[x] = pal[(row[x >> 3] >> (7 - (x & 7))) & 1];
            break;
        case 4:
            for (int x = 0; x < w; x++) out[x] = pal[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15];
            break;
        case 8:
            for (int x = 0; x < w; x++) out[x] = pal[row[x]];
            break;
        case 24:
            for (int x = 0; x < w; x++) out[x] = RGB(row[x * 3 + 2], row[x * 3 + 1], row[x * 3]);
            break;
        case 16:
        case 32:
            for (int x = 0; x < w; x++) {
                uint32_t v = bpp == 16 ? rd16(row + x * 2) : rd32(row + x * 4);
                uint32_t a = ca.bits ? chan_get(&ca, v) : 255;
                if (a) alpha_nonzero = true;
                if (a != 255) alpha_varies = true;
                out[x] = RGBA(chan_get(&cr, v), chan_get(&cg, v), chan_get(&cb, v), a);
            }
            break;
        }
    }

    /* An alpha channel that is entirely 0 means "unused": make the image opaque. */
    bool has_alpha = ca.bits && alpha_varies && alpha_nonzero;
    if (!has_alpha) {
        for (int y = 0; y < h; y++) {
            uint32_t *out = surface_row(s, y);
            for (int x = 0; x < w; x++) out[x] |= 0xFF000000u;
        }
        s->flags &= ~SURF_ALPHA;
    }

    if (info) {
        info->width = w;
        info->height = h;
        info->bpp = (int)bpp;
        info->top_down = top_down;
        info->has_alpha = has_alpha;
        if (bpp <= 8) snprintf(info->format, sizeof info->format, "%u-bit indexed", bpp);
        else if (bpp == 16) snprintf(info->format, sizeof info->format, "16-bit %s", cg.bits == 6 ? "5-6-5" : "5-5-5");
        else if (has_alpha) snprintf(info->format, sizeof info->format, "%u-bit RGBA", bpp);
        else snprintf(info->format, sizeof info->format, "%u-bit RGB", bpp);
    }
    return s;
}

surface_t *gbmp_load(const char *path, gbmp_info_t *info, uint32_t *file_size, const char **err) {
    if (err) *err = NULL;
    vfs_stat_t st;
    if (vfs_stat(path, &st) < 0) return fail(err, "The file does not exist");
    if (st.is_dir) return fail(err, "This is a folder");
    if (st.size > (100u << 20)) return fail(err, "The file is too large");
    uint32_t size = 0;
    uint8_t *data = vfs_read_file(path, &size);
    if (!data) return fail(err, "The file could not be read");
    if (file_size) *file_size = size;
    surface_t *s = NULL;
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') {
        s = gbmp_decode(data, size, info, err);
    } else if (size >= 2 && data[0] == 'P' && (data[1] == '6' || data[1] == '3')) {
        s = image_decode(data, size);
        if (!s) fail(err, "Damaged or unsupported PPM image");
        else if (info) {
            info->width = s->w; info->height = s->h; info->bpp = 24;
            info->top_down = true; info->has_alpha = false;
            snprintf(info->format, sizeof info->format, "PPM (%s)", data[1] == '6' ? "binary" : "text");
        }
    } else {
        fail(err, "Not a supported image (BMP or PPM)");
    }
    free(data);
    return s;
}

void *gbmp_encode(const surface_t *s, bool keep_alpha, size_t *out_size) {
    if (!s || s->w <= 0 || s->h <= 0) return NULL;
    uint32_t bpp = keep_alpha ? 32 : 24;
    uint32_t hdr = keep_alpha ? 108 : 40;   /* BITMAPV4HEADER carries the alpha mask */
    uint32_t stride = (((uint32_t)s->w * bpp + 31) / 32) * 4;
    uint32_t img = stride * (uint32_t)s->h;
    uint32_t off = 14 + hdr;
    uint32_t total = off + img;
    uint8_t *b = calloc(1, total);
    if (!b) return NULL;
    b[0] = 'B'; b[1] = 'M';
    wr32(b + 2, total);
    wr32(b + 10, off);
    wr32(b + 14, hdr);
    wr32(b + 18, (uint32_t)s->w);
    wr32(b + 22, (uint32_t)s->h);          /* positive: bottom-up */
    wr16(b + 26, 1);
    wr16(b + 28, (uint16_t)bpp);
    wr32(b + 30, keep_alpha ? 3 : 0);      /* BI_BITFIELDS / BI_RGB */
    wr32(b + 34, img);
    wr32(b + 38, 2835);                    /* 72 dpi */
    wr32(b + 42, 2835);
    if (keep_alpha) {
        wr32(b + 54, 0x00FF0000);
        wr32(b + 58, 0x0000FF00);
        wr32(b + 62, 0x000000FF);
        wr32(b + 66, 0xFF000000);
        wr32(b + 70, 0x73524742);          /* LCS_sRGB 'sRGB' */
    }
    bool src_alpha = s->flags & SURF_ALPHA;
    for (int y = 0; y < s->h; y++) {
        uint8_t *row = b + off + stride * (uint32_t)(s->h - 1 - y);
        const uint32_t *src = surface_row(s, y);
        if (keep_alpha) {
            for (int x = 0; x < s->w; x++) {
                uint32_t c = src[x];
                row[x * 4] = (uint8_t)COLOR_B(c);
                row[x * 4 + 1] = (uint8_t)COLOR_G(c);
                row[x * 4 + 2] = (uint8_t)COLOR_R(c);
                row[x * 4 + 3] = src_alpha ? (uint8_t)COLOR_A(c) : 255;
            }
        } else {
            for (int x = 0; x < s->w; x++) {
                uint32_t c = src[x];
                row[x * 3] = (uint8_t)COLOR_B(c);
                row[x * 3 + 1] = (uint8_t)COLOR_G(c);
                row[x * 3 + 2] = (uint8_t)COLOR_R(c);
            }
        }
    }
    if (out_size) *out_size = total;
    return b;
}

int gbmp_save(const char *path, const surface_t *s, bool keep_alpha) {
    size_t size = 0;
    void *data = gbmp_encode(s, keep_alpha, &size);
    if (!data) return VFS_ENOMEM;
    int r = vfs_write_file(path, data, (uint32_t)size);
    free(data);
    return r < 0 ? r : 0;
}

bool gbmp_is_image_name(const char *name) {
    const char *e = vfs_extension(name);
    return !strcasecmp(e, "bmp") || !strcasecmp(e, "dib") || !strcasecmp(e, "ppm");
}
