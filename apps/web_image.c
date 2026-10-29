/*
 * web_image.c - image decoders for the Web Browser.
 *
 *   PNG   all colour types and bit depths, palettes, tRNS transparency,
 *         Adam7 interlacing (zlib data through nova/net.h inflate_buffer)
 *   JPEG  baseline and progressive Huffman JPEG, 1/3/4 components (grey,
 *         YCbCr, Adobe RGB / CMYK / YCCK), any sampling factors, restart
 *         markers; very large pictures are decoded at 1/2, 1/4 or 1/8 size
 *   GIF   the first frame, transparency, interlacing
 *   BMP   through the system decoder (image_decode)
 *
 * Every decoder checks its input and returns NULL for broken data instead of
 * reading past the end; truncated PNG / JPEG data gives the part received.
 */
#include "web.h"

#define MAX_PIXELS (24u * 1024 * 1024)

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ==== PNG ===================================================================== */
static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static void unfilter(uint8_t *row, const uint8_t *prev, int n, int bpp, int type) {
    switch (type) {
    case 1: for (int i = bpp; i < n; i++) row[i] = (uint8_t)(row[i] + row[i - bpp]); break;
    case 2: for (int i = 0; i < n; i++) row[i] = (uint8_t)(row[i] + prev[i]); break;
    case 3:
        for (int i = 0; i < n; i++) row[i] = (uint8_t)(row[i] + (((i >= bpp ? row[i - bpp] : 0) + prev[i]) >> 1));
        break;
    case 4:
        for (int i = 0; i < n; i++)
            row[i] = (uint8_t)(row[i] + paeth(i >= bpp ? row[i - bpp] : 0, prev[i], i >= bpp ? prev[i - bpp] : 0));
        break;
    default: break;
    }
}

surface_t *web_png_decode(const uint8_t *d, uint32_t n) {
    if (n < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return NULL;
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = -1, interlace = 0;
    uint32_t pal[256];
    int npal = 0;
    for (int i = 0; i < 256; i++) pal[i] = 0xFF000000u;
    bool has_trns = false;
    int trns_g = -1, trns_r = -1, trns_gr = -1, trns_b = -1;
    uint8_t *idat = NULL;
    uint32_t ilen = 0, icap = 0;
    uint32_t p = 8;
    while (p + 12 <= n) {
        uint32_t len = be32(d + p);
        const uint8_t *type = d + p + 4, *cd = d + p + 8;
        if (len > n - p - 12) len = n - p - 12;   /* truncated: use what is there */
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = be32(cd); h = be32(cd + 4);
            depth = cd[8]; ctype = cd[9]; interlace = cd[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            npal = (int)(len / 3);
            if (npal > 256) npal = 256;
            for (int i = 0; i < npal; i++) pal[i] = RGB(cd[i * 3], cd[i * 3 + 1], cd[i * 3 + 2]);
        } else if (!memcmp(type, "tRNS", 4)) {
            has_trns = true;
            if (ctype == 3) for (uint32_t i = 0; i < len && i < 256; i++) pal[i] = color_alpha(pal[i], cd[i]);
            else if (ctype == 0 && len >= 2) trns_g = cd[0] << 8 | cd[1];
            else if (ctype == 2 && len >= 6) { trns_r = cd[0] << 8 | cd[1]; trns_gr = cd[2] << 8 | cd[3]; trns_b = cd[4] << 8 | cd[5]; }
        } else if (!memcmp(type, "IDAT", 4)) {
            if (ilen + len > icap) {
                uint32_t nc = icap ? icap * 2 : 65536;
                while (nc < ilen + len) nc *= 2;
                uint8_t *ni = realloc(idat, nc);
                if (!ni) { free(idat); return NULL; }
                idat = ni;
                icap = nc;
            }
            memcpy(idat + ilen, cd, len);
            ilen += len;
        } else if (!memcmp(type, "IEND", 4)) break;
        p += 12 + len;
    }
    int ch;
    switch (ctype) {
    case 0: ch = 1; break;
    case 2: ch = 3; break;
    case 3: ch = 1; break;
    case 4: ch = 2; break;
    case 6: ch = 4; break;
    default: free(idat); return NULL;
    }
    bool depth_ok = depth == 8 || (depth == 16 && ctype != 3) ||
                    ((ctype == 0 || ctype == 3) && (depth == 1 || depth == 2 || depth == 4));
    if (!depth_ok) { free(idat); return NULL; }
    if (!w || !h || w > 16384 || h > 16384 || (uint64_t)w * h > MAX_PIXELS || !ilen) { free(idat); return NULL; }
    int bits = ch * depth, bpp = bits >= 8 ? bits / 8 : 1;
    /* passes */
    static const int ax0[7] = { 0, 4, 0, 2, 0, 1, 0 }, ay0[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const int adx[7] = { 8, 8, 4, 4, 2, 2, 1 }, ady[7] = { 8, 8, 8, 4, 4, 2, 2 };
    int npass = interlace ? 7 : 1;
    uint64_t expect = 0;
    for (int ps = 0; ps < npass; ps++) {
        int x0 = interlace ? ax0[ps] : 0, y0 = interlace ? ay0[ps] : 0, dx = interlace ? adx[ps] : 1, dy = interlace ? ady[ps] : 1;
        uint32_t pw = w > (uint32_t)x0 ? (w - x0 + dx - 1) / dx : 0, ph = h > (uint32_t)y0 ? (h - y0 + dy - 1) / dy : 0;
        if (pw && ph) expect += (uint64_t)ph * (1 + ((uint64_t)pw * bits + 7) / 8);
    }
    uint8_t *raw = NULL;
    uint32_t rlen = 0;
    int r = inflate_buffer(idat, ilen, INFLATE_ZLIB, &raw, &rlen, (uint32_t)expect + 1024);
    free(idat);
    if (!raw || (r != 0 && rlen == 0)) { free(raw); return NULL; }
    bool alpha = ctype == 4 || ctype == 6 || has_trns;
    surface_t *s = alpha ? surface_create_alpha((int)w, (int)h) : surface_create((int)w, (int)h);
    if (!s) { free(raw); return NULL; }
    uint8_t *prev = calloc(1, ((size_t)w * bits + 7) / 8 + 8);
    if (!prev) { free(raw); surface_destroy(s); return NULL; }
    uint32_t pos = 0;
    int maxv = (1 << depth) - 1;
    for (int ps = 0; ps < npass; ps++) {
        int x0 = interlace ? ax0[ps] : 0, y0 = interlace ? ay0[ps] : 0, dx = interlace ? adx[ps] : 1, dy = interlace ? ady[ps] : 1;
        uint32_t pw = w > (uint32_t)x0 ? (w - x0 + dx - 1) / dx : 0, ph = h > (uint32_t)y0 ? (h - y0 + dy - 1) / dy : 0;
        if (!pw || !ph) continue;
        uint32_t rb = (uint32_t)(((uint64_t)pw * bits + 7) / 8);
        memset(prev, 0, rb);
        for (uint32_t y = 0; y < ph; y++) {
            if (pos + 1 + rb > rlen) goto done;    /* truncated data: keep what we have */
            int ft = raw[pos];
            uint8_t *row = raw + pos + 1;
            unfilter(row, prev, (int)rb, bpp, ft);
            uint32_t *out = surface_row(s, y0 + (int)y * dy);
            for (uint32_t x = 0; x < pw; x++) {
                uint32_t c;
                int ox = x0 + (int)x * dx;
                if (depth < 8) {
                    int bitpos = (int)x * depth;
                    int v = (row[bitpos >> 3] >> (8 - depth - (bitpos & 7))) & maxv;
                    if (ctype == 3) c = pal[v];
                    else {
                        int g = v * 255 / maxv;
                        c = RGB(g, g, g);
                        if (v == trns_g) c = 0;
                    }
                } else {
                    const uint8_t *px = row + (size_t)x * bpp;
                    int st = depth == 16 ? 2 : 1;
                    switch (ctype) {
                    case 0: {
                        c = RGB(px[0], px[0], px[0]);
                        int v = depth == 16 ? (px[0] << 8 | px[1]) : px[0];
                        if (v == trns_g) c = 0;
                        break;
                    }
                    case 2: {
                        c = RGB(px[0], px[st], px[2 * st]);
                        if (trns_r >= 0) {
                            int rv = depth == 16 ? (px[0] << 8 | px[1]) : px[0];
                            int gv = depth == 16 ? (px[2] << 8 | px[3]) : px[1];
                            int bv = depth == 16 ? (px[4] << 8 | px[5]) : px[2];
                            if (rv == trns_r && gv == trns_gr && bv == trns_b) c = 0;
                        }
                        break;
                    }
                    case 3: c = pal[px[0]]; break;
                    case 4: c = RGBA(px[0], px[0], px[0], px[st]); break;
                    default: c = RGBA(px[0], px[st], px[2 * st], px[3 * st]); break;
                    }
                }
                out[ox] = c;
            }
            memcpy(prev, row, rb);
            pos += 1 + rb;
        }
    }
done:
    free(prev);
    free(raw);
    return s;
}

/* ==== JPEG ===================================================================== */
static const uint8_t dezig[64 + 15] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    /* padding so that runs past the end are harmless */
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63 };

typedef struct huff {
    uint8_t look_nb[256], look_val[256];
    uint8_t vals[256];
    int maxcode[18], valptr[17], mincode[17];
    bool ok;
} huff_t;

typedef struct jcomp {
    int id, h, v, tq, td, ta;
    int bw, bh;             /* blocks per row / column, padded to whole MCUs */
    int cw, ch;             /* blocks really covered by the picture          */
    int16_t *coef;          /* progressive: all coefficients                 */
    uint8_t *plane;         /* decoded samples (scaled)                      */
    int pw, ph;             /* plane size                                    */
    int dcpred;
} jcomp_t;

typedef struct jpg {
    const uint8_t *p, *end;
    uint32_t buf;
    int cnt;
    bool marker;
    uint16_t q[4][64];      /* natural order */
    huff_t dc[4], ac[4];
    int w, h, nc, prog;
    jcomp_t c[4];
    int hmax, vmax, mcux, mcuy;
    int restart;
    int eobrun;
    int ns, sc[4], ss, se, ah, al;
    int adobe, transform;
    int scale;
    bool frame;
    int scans;
} jpg_t;

static void jfill(jpg_t *j) {
    while (j->cnt <= 24) {
        uint32_t b = 0;
        if (!j->marker && j->p < j->end) {
            b = *j->p;
            if (b == 0xFF) {
                uint32_t b2 = j->p + 1 < j->end ? j->p[1] : 0xD9;
                if (b2 == 0) j->p += 2;
                else { j->marker = true; b = 0; }
            } else j->p++;
        }
        j->buf = (j->buf << 8) | b;
        j->cnt += 8;
    }
}

static int jbits(jpg_t *j, int n) {
    if (!n) return 0;
    jfill(j);
    int v = (int)((j->buf >> (j->cnt - n)) & ((1u << n) - 1));
    j->cnt -= n;
    return v;
}

static int jbit(jpg_t *j) { return jbits(j, 1); }

static int extend(jpg_t *j, int n) {
    if (n <= 0 || n > 16) return 0;
    int v = jbits(j, n);
    if (v < (1 << (n - 1))) v -= (1 << n) - 1;
    return v;
}

static bool huff_build(huff_t *h, const uint8_t *counts, const uint8_t *vals, int nvals) {
    memset(h, 0, sizeof *h);
    memcpy(h->vals, vals, (size_t)nvals);
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        h->mincode[l] = code;
        h->valptr[l] = k;
        for (int i = 0; i < counts[l - 1]; i++) {
            if (k >= nvals) return false;
            if (l <= 8) {
                int base = code << (8 - l);
                for (int e = 0; e < (1 << (8 - l)); e++) { h->look_nb[base + e] = (uint8_t)l; h->look_val[base + e] = vals[k]; }
            }
            code++;
            k++;
        }
        h->maxcode[l] = counts[l - 1] ? code - 1 : -1;
        if (code > (1 << l)) return false;
        code <<= 1;
    }
    h->ok = true;
    return true;
}

static int hdecode(jpg_t *j, const huff_t *h) {
    if (!h->ok) return -1;
    jfill(j);
    int look = (int)((j->buf >> (j->cnt - 8)) & 0xFF);
    int nb = h->look_nb[look];
    if (nb) { j->cnt -= nb; return h->look_val[look]; }
    int code = (int)((j->buf >> (j->cnt - 16)) & 0xFFFF);
    for (int l = 9; l <= 16; l++) {
        int c = code >> (16 - l);
        if (h->maxcode[l] >= 0 && c <= h->maxcode[l]) {
            j->cnt -= l;
            int idx = h->valptr[l] + c - h->mincode[l];
            return idx >= 0 && idx < 256 ? h->vals[idx] : -1;
        }
    }
    return -1;
}

#define F2F_0541 2217
#define F2F_N1847 (-7567)
#define F2F_0765 3135
#define F2F_1175 4816
#define F2F_0298 1223
#define F2F_2053 8410
#define F2F_3072 12586
#define F2F_1501 6149
#define F2F_N0899 (-3685)
#define F2F_N2562 (-10497)
#define F2F_N1961 (-8034)
#define F2F_N0390 (-1597)

#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)                                   \
    int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;                         \
    p2 = s2; p3 = s6;                                                              \
    p1 = (p2 + p3) * F2F_0541;                                                     \
    t2 = p1 + p3 * F2F_N1847;                                                      \
    t3 = p1 + p2 * F2F_0765;                                                       \
    p2 = s0; p3 = s4;                                                              \
    t0 = (p2 + p3) * 4096;                                                         \
    t1 = (p2 - p3) * 4096;                                                         \
    x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2;                        \
    t0 = s7; t1 = s5; t2 = s3; t3 = s1;                                            \
    p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2;                        \
    p5 = (p3 + p4) * F2F_1175;                                                     \
    t0 = t0 * F2F_0298; t1 = t1 * F2F_2053; t2 = t2 * F2F_3072; t3 = t3 * F2F_1501; \
    p1 = p5 + p1 * F2F_N0899; p2 = p5 + p2 * F2F_N2562;                            \
    p3 = p3 * F2F_N1961; p4 = p4 * F2F_N0390;                                      \
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* corrupt data can make these sums overflow: let them wrap (defined behaviour) */
__attribute__((optimize("wrapv")))
static void idct8(const int16_t *d, uint8_t *out /* 8x8 */) {
    int val[64];
    for (int i = 0; i < 8; i++) {
        const int16_t *c = d + i;
        int *v = val + i;
        if (!c[8] && !c[16] && !c[24] && !c[32] && !c[40] && !c[48] && !c[56]) {
            int dc = c[0] * 4;
            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc;
        } else {
            IDCT_1D(c[0], c[8], c[16], c[24], c[32], c[40], c[48], c[56])
            x0 += 512; x1 += 512; x2 += 512; x3 += 512;
            v[0] = (x0 + t3) >> 10; v[56] = (x0 - t3) >> 10;
            v[8] = (x1 + t2) >> 10; v[48] = (x1 - t2) >> 10;
            v[16] = (x2 + t1) >> 10; v[40] = (x2 - t1) >> 10;
            v[24] = (x3 + t0) >> 10; v[32] = (x3 - t0) >> 10;
        }
    }
    for (int i = 0; i < 8; i++) {
        const int *v = val + i * 8;
        uint8_t *o = out + i * 8;
        IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        x0 += 65536 + (128 << 17); x1 += 65536 + (128 << 17);
        x2 += 65536 + (128 << 17); x3 += 65536 + (128 << 17);
        o[0] = clamp8((x0 + t3) >> 17); o[7] = clamp8((x0 - t3) >> 17);
        o[1] = clamp8((x1 + t2) >> 17); o[6] = clamp8((x1 - t2) >> 17);
        o[2] = clamp8((x2 + t1) >> 17); o[5] = clamp8((x2 - t1) >> 17);
        o[3] = clamp8((x3 + t0) >> 17); o[4] = clamp8((x3 - t0) >> 17);
    }
}

/* IDCT one block into the component's (possibly scaled down) plane */
static void put_block(jpg_t *j, jcomp_t *c, int bx, int by, const int16_t *coef) {
    uint8_t px[64];
    int s = j->scale, n = 8 / s;
    if (s == 8) {
        int v = ((coef[0] + 4) >> 3) + 128;   /* DC only */
        px[0] = clamp8(v);
    } else {
        idct8(coef, px);
        if (s > 1) {
            for (int y = 0; y < n; y++)
                for (int x = 0; x < n; x++) {
                    int sum = 0;
                    for (int yy = 0; yy < s; yy++) for (int xx = 0; xx < s; xx++) sum += px[(y * s + yy) * 8 + x * s + xx];
                    px[y * 8 + x] = (uint8_t)((sum + s * s / 2) / (s * s));
                }
        }
    }
    int ox = bx * n, oy = by * n;
    for (int y = 0; y < n; y++) {
        if (oy + y >= c->ph) break;
        uint8_t *row = c->plane + (size_t)(oy + y) * c->pw + ox;
        int lim = MIN(n, c->pw - ox);
        for (int x = 0; x < lim; x++) row[x] = px[s == 8 ? 0 : y * 8 + x];
    }
}

static bool decode_block(jpg_t *j, jcomp_t *c, int16_t *data) {
    memset(data, 0, 64 * sizeof(int16_t));
    int t = hdecode(j, &j->dc[c->td]);
    if (t < 0 || t > 16) return false;
    int dc = c->dcpred + (t ? extend(j, t) : 0);
    c->dcpred = dc;
    const uint16_t *q = j->q[c->tq];
    data[0] = (int16_t)(dc * q[0]);
    int k = 1;
    while (k < 64) {
        int rs = hdecode(j, &j->ac[c->ta]);
        if (rs < 0) return false;
        int s = rs & 15, r = rs >> 4;
        if (!s) {
            if (rs != 0xF0) break;
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) return false;
        int z = dezig[k++];
        data[z] = (int16_t)(extend(j, s) * q[z]);
    }
    return true;
}

static bool prog_dc(jpg_t *j, jcomp_t *c, int16_t *data) {
    if (j->ah == 0) {
        int t = hdecode(j, &j->dc[c->td]);
        if (t < 0 || t > 16) return false;
        int dc = c->dcpred + (t ? extend(j, t) : 0);
        c->dcpred = dc;
        data[0] = (int16_t)(dc * (1 << j->al));
    } else if (jbit(j)) {
        data[0] = (int16_t)(data[0] + (1 << j->al));
    }
    return true;
}

static bool prog_ac(jpg_t *j, jcomp_t *c, int16_t *data) {
    const huff_t *h = &j->ac[c->ta];
    if (j->ah == 0) {
        if (j->eobrun) { j->eobrun--; return true; }
        int k = j->ss;
        while (k <= j->se) {
            int rs = hdecode(j, h);
            if (rs < 0) return false;
            int s = rs & 15, r = rs >> 4;
            if (!s) {
                if (r < 15) {
                    j->eobrun = (1 << r) - 1;
                    if (r) j->eobrun += jbits(j, r);
                    break;
                }
                k += 16;
            } else {
                k += r;
                if (k > 63) return false;
                data[dezig[k++]] = (int16_t)(extend(j, s) * (1 << j->al));
            }
        }
        return true;
    }
    int16_t bit = (int16_t)(1 << j->al);
    int k = j->ss;
    if (j->eobrun) {
        j->eobrun--;
        for (; k <= j->se; k++) {
            int16_t *p = &data[dezig[k]];
            if (*p && jbit(j) && !(*p & bit)) *p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
        }
        return true;
    }
    while (k <= j->se) {
        int rs = hdecode(j, h);
        if (rs < 0) return false;
        int s = rs & 15, r = rs >> 4, val = 0;
        if (!s) {
            if (r < 15) {
                j->eobrun = (1 << r) - 1;
                if (r) j->eobrun += jbits(j, r);
                r = 64;
            }
        } else {
            if (s != 1) return false;
            val = jbit(j) ? bit : -bit;
        }
        while (k <= j->se) {
            int16_t *p = &data[dezig[k++]];
            if (*p) {
                if (jbit(j) && !(*p & bit)) *p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
            } else {
                if (r == 0) { *p = (int16_t)val; break; }
                r--;
            }
        }
    }
    return true;
}

/* skip to and over a restart marker */
static void jrestart(jpg_t *j) {
    j->cnt = 0;
    j->buf = 0;
    j->marker = false;
    while (j->p + 1 < j->end) {
        if (j->p[0] == 0xFF && j->p[1] >= 0xD0 && j->p[1] <= 0xD7) { j->p += 2; break; }
        if (j->p[0] == 0xFF && j->p[1] != 0 && j->p[1] != 0xFF) break;   /* another marker: stop */
        j->p++;
    }
    for (int i = 0; i < j->nc; i++) j->c[i].dcpred = 0;
    j->eobrun = 0;
}

static bool decode_scan(jpg_t *j) {
    j->cnt = 0;
    j->buf = 0;
    j->marker = false;
    j->eobrun = 0;
    for (int i = 0; i < j->nc; i++) j->c[i].dcpred = 0;
    int16_t blk[64];
    int todo = j->restart;
    if (j->ns == 1) {
        jcomp_t *c = &j->c[j->sc[0]];
        for (int by = 0; by < c->ch; by++)
            for (int bx = 0; bx < c->cw; bx++) {
                if (j->restart && todo-- == 0) { jrestart(j); todo = j->restart - 1; }
                if (j->prog) {
                    int16_t *d = c->coef + ((size_t)by * c->bw + bx) * 64;
                    if (!(j->ss == 0 ? prog_dc(j, c, d) : prog_ac(j, c, d))) return false;
                } else {
                    if (!decode_block(j, c, blk)) return false;
                    put_block(j, c, bx, by, blk);
                }
            }
        return true;
    }
    for (int my = 0; my < j->mcuy; my++)
        for (int mx = 0; mx < j->mcux; mx++) {
            if (j->restart && todo-- == 0) { jrestart(j); todo = j->restart - 1; }
            for (int s = 0; s < j->ns; s++) {
                jcomp_t *c = &j->c[j->sc[s]];
                for (int y = 0; y < c->v; y++)
                    for (int x = 0; x < c->h; x++) {
                        int bx = mx * c->h + x, by = my * c->v + y;
                        if (j->prog) {
                            int16_t *d = c->coef + ((size_t)by * c->bw + bx) * 64;
                            if (!prog_dc(j, c, d)) return false;
                        } else {
                            if (!decode_block(j, c, blk)) return false;
                            put_block(j, c, bx, by, blk);
                        }
                    }
            }
        }
    return true;
}

static bool jpeg_frame(jpg_t *j, const uint8_t *s, int len, bool prog) {
    if (len < 6 || j->frame) return false;
    if (s[0] != 8) return false;   /* 12-bit precision is not supported */
    j->h = s[1] << 8 | s[2];
    j->w = s[3] << 8 | s[4];
    j->nc = s[5];
    j->prog = prog;
    if (!j->w || !j->h || (j->nc != 1 && j->nc != 3 && j->nc != 4) || len < 6 + 3 * j->nc) return false;
    if ((uint64_t)j->w * j->h > 64u * 1024 * 1024) return false;
    j->hmax = j->vmax = 1;
    for (int i = 0; i < j->nc; i++) {
        jcomp_t *c = &j->c[i];
        c->id = s[6 + i * 3];
        c->h = s[7 + i * 3] >> 4;
        c->v = s[7 + i * 3] & 15;
        c->tq = s[8 + i * 3] & 3;
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4) return false;
        if (c->h > j->hmax) j->hmax = c->h;
        if (c->v > j->vmax) j->vmax = c->v;
    }
    j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
    /* big pictures are decoded smaller */
    j->scale = 1;
    while (j->scale < 8 && (j->w / j->scale > 2048 || j->h / j->scale > 2048 ||
                            (uint64_t)(j->w / j->scale) * (j->h / j->scale) > MAX_PIXELS / 2))
        j->scale *= 2;
    int n = 8 / j->scale;
    for (int i = 0; i < j->nc; i++) {
        jcomp_t *c = &j->c[i];
        c->bw = j->mcux * c->h;
        c->bh = j->mcuy * c->v;
        c->cw = ((j->w * c->h + j->hmax - 1) / j->hmax + 7) / 8;   /* blocks covering the component */
        c->ch = ((j->h * c->v + j->vmax - 1) / j->vmax + 7) / 8;
        c->pw = c->bw * n;
        c->ph = c->bh * n;
        c->plane = calloc(1, (size_t)c->pw * c->ph);
        if (!c->plane) return false;
        if (prog) {
            size_t sz = (size_t)c->bw * c->bh * 64 * sizeof(int16_t);
            if (sz > (64u << 20)) return false;
            c->coef = calloc(1, sz);
            if (!c->coef) return false;
        }
    }
    j->frame = true;
    return true;
}

static bool jpeg_sos(jpg_t *j, const uint8_t *s, int len) {
    if (!j->frame || len < 1) return false;
    j->ns = s[0];
    if (j->ns < 1 || j->ns > 4 || len < 1 + 2 * j->ns + 3) return false;
    for (int i = 0; i < j->ns; i++) {
        int id = s[1 + i * 2], tb = s[2 + i * 2];
        int k = -1;
        for (int c = 0; c < j->nc; c++) if (j->c[c].id == id) k = c;
        if (k < 0) return false;
        j->sc[i] = k;
        j->c[k].td = (tb >> 4) & 3;
        j->c[k].ta = tb & 3;
    }
    const uint8_t *p = s + 1 + 2 * j->ns;
    j->ss = p[0];
    j->se = p[1];
    j->ah = p[2] >> 4;
    j->al = p[2] & 15;
    if (j->prog) {
        if (j->ss > 63 || j->se > 63 || j->ss > j->se || (j->ss == 0 && j->se != 0) || (j->ss && j->ns != 1)) return false;
    } else {
        j->ss = 0; j->se = 63; j->ah = j->al = 0;
    }
    return true;
}

static void jpeg_finish_prog(jpg_t *j) {
    int16_t blk[64];
    for (int i = 0; i < j->nc; i++) {
        jcomp_t *c = &j->c[i];
        const uint16_t *q = j->q[c->tq];
        for (int by = 0; by < c->ch; by++)
            for (int bx = 0; bx < c->cw; bx++) {
                const int16_t *d = c->coef + ((size_t)by * c->bw + bx) * 64;
                for (int k = 0; k < 64; k++) blk[k] = (int16_t)(d[k] * q[k]);
                put_block(j, c, bx, by, blk);
            }
    }
}

/* sample a subsampled plane at output pixel (x, y) with bilinear interpolation
   (chroma sample centres sit between the luma samples they cover) */
static int sample(const jcomp_t *c, int fx, int fy) {
    int x0 = fx >> 8, y0 = fy >> 8, ax = fx & 255, ay = fy & 255;
    if (fx < 0) { x0 = 0; ax = 0; }
    if (fy < 0) { y0 = 0; ay = 0; }
    int x1 = x0 + 1 < c->pw ? x0 + 1 : c->pw - 1, y1 = y0 + 1 < c->ph ? y0 + 1 : c->ph - 1;
    if (x0 >= c->pw) x0 = x1 = c->pw - 1;
    if (y0 >= c->ph) y0 = y1 = c->ph - 1;
    const uint8_t *r0 = c->plane + (size_t)y0 * c->pw, *r1 = c->plane + (size_t)y1 * c->pw;
    int top = r0[x0] * (256 - ax) + r0[x1] * ax, bot = r1[x0] * (256 - ax) + r1[x1] * ax;
    return (top * (256 - ay) + bot * ay + 32768) >> 16;
}

static surface_t *jpeg_output(jpg_t *j) {
    int s = j->scale;
    int w = (j->w + s - 1) / s, h = (j->h + s - 1) / s;
    surface_t *out = surface_create(w, h);
    if (!out) return NULL;
    for (int y = 0; y < h; y++) {
        uint32_t *row = surface_row(out, y);
        const uint8_t *pl[4];
        int fy[4];
        for (int i = 0; i < j->nc; i++) {
            jcomp_t *c = &j->c[i];
            int py = y * c->v / j->vmax;
            if (py >= c->ph) py = c->ph - 1;
            pl[i] = c->plane + (size_t)py * c->pw;
            fy[i] = ((2 * y + 1) * c->v * 256) / (2 * j->vmax) - 128;
        }
        for (int x = 0; x < w; x++) {
            int v[4];
            for (int i = 0; i < j->nc; i++) {
                jcomp_t *c = &j->c[i];
                if (c->h == j->hmax && c->v == j->vmax) {
                    v[i] = pl[i][x < c->pw ? x : c->pw - 1];
                } else {
                    int fx = ((2 * x + 1) * c->h * 256) / (2 * j->hmax) - 128;
                    v[i] = sample(c, fx, fy[i]);
                }
            }
            int r, g, b;
            if (j->nc == 1) { r = g = b = v[0]; }
            else {
                bool ycc = j->nc == 3 ? !(j->adobe && j->transform == 0) : (j->adobe && j->transform == 2);
                if (ycc) {
                    int Y = v[0] << 16, cb = v[1] - 128, cr = v[2] - 128;
                    r = (Y + 91881 * cr + 32768) >> 16;
                    g = (Y - 22554 * cb - 46802 * cr + 32768) >> 16;
                    b = (Y + 116130 * cb + 32768) >> 16;
                    r = clamp8(r); g = clamp8(g); b = clamp8(b);
                } else { r = v[0]; g = v[1]; b = v[2]; }
                if (j->nc == 4) {
                    /* Adobe CMYK / YCCK store inverted values */
                    int k = v[3];
                    if (j->adobe) { r = r * k / 255; g = g * k / 255; b = b * k / 255; }
                    else { r = (255 - r) * (255 - k) / 255; g = (255 - g) * (255 - k) / 255; b = (255 - b) * (255 - k) / 255; }
                }
            }
            row[x] = RGB(r, g, b);
        }
    }
    return out;
}

surface_t *web_jpeg_decode(const uint8_t *d, uint32_t n) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return NULL;
    jpg_t *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->end = d + n;
    const uint8_t *p = d + 2;
    bool ok = true;
    while (ok && p + 4 <= d + n) {
        if (*p != 0xFF) { p++; continue; }
        while (p < d + n && *p == 0xFF) p++;
        if (p >= d + n) break;
        int m = *p++;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) continue;
        if (m == 0xD9) break;
        if (p + 2 > d + n) break;
        int len = (p[0] << 8 | p[1]) - 2;
        const uint8_t *s = p + 2;
        if (len < 0 || s + len > d + n) { if (m != 0xDA) break; len = (int)(d + n - s); }
        switch (m) {
        case 0xC0: case 0xC1: case 0xC2:
            ok = jpeg_frame(j, s, len, m == 0xC2);
            break;
        case 0xC4: {
            const uint8_t *q = s, *e = s + len;
            while (q + 17 <= e) {
                int tc = q[0] >> 4, th = q[0] & 3, total = 0;
                for (int i = 1; i <= 16; i++) total += q[i];
                if (q + 17 + total > e || total > 256) { ok = false; break; }
                huff_t *h = tc ? &j->ac[th] : &j->dc[th];
                if (!huff_build(h, q + 1, q + 17, total)) { ok = false; break; }
                q += 17 + total;
            }
            break;
        }
        case 0xDB: {
            const uint8_t *q = s, *e = s + len;
            while (q < e) {
                int pq = q[0] >> 4, tq = q[0] & 3;
                if (q + 1 + 64 * (pq ? 2 : 1) > e) break;
                for (int i = 0; i < 64; i++) j->q[tq][dezig[i]] = pq ? (uint16_t)(q[1 + 2 * i] << 8 | q[2 + 2 * i]) : q[1 + i];
                q += 1 + 64 * (pq ? 2 : 1);
            }
            break;
        }
        case 0xDD:
            if (len >= 2) j->restart = s[0] << 8 | s[1];
            break;
        case 0xEE:
            if (len >= 12 && !memcmp(s, "Adobe", 5)) { j->adobe = 1; j->transform = s[11]; }
            break;
        case 0xDA:
            if (!jpeg_sos(j, s, len)) { ok = false; break; }
            j->p = s + len;
            if (!decode_scan(j) && !j->scans) { ok = false; break; }
            j->scans++;
            p = j->p;
            /* the bit reader stopped at the next marker (or ran past broken data) */
            while (p + 1 < d + n && !(p[0] == 0xFF && p[1] != 0 && !(p[1] >= 0xD0 && p[1] <= 0xD7))) p++;
            continue;
        case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            ok = false;   /* lossless / hierarchical / arithmetic coding */
            break;
        default:
            break;
        }
        p = s + len;
    }
    surface_t *out = NULL;
    if (j->frame && j->scans) {
        if (j->prog) jpeg_finish_prog(j);
        out = jpeg_output(j);
    }
    for (int i = 0; i < 4; i++) { free(j->c[i].plane); free(j->c[i].coef); }
    free(j);
    return out;
}

/* ==== GIF ======================================================================== */
surface_t *web_gif_decode(const uint8_t *d, uint32_t n) {
    if (n < 13 || memcmp(d, "GIF8", 4)) return NULL;
    int sw = le16(d + 6), sh = le16(d + 8), flags = d[10];
    uint32_t p = 13;
    uint32_t gct[256], lct[256];
    int ngct = 0;
    if (flags & 0x80) {
        ngct = 2 << (flags & 7);
        if (p + 3u * ngct > n) return NULL;
        for (int i = 0; i < ngct; i++) gct[i] = RGB(d[p + i * 3], d[p + i * 3 + 1], d[p + i * 3 + 2]);
        p += 3u * ngct;
    }
    int trans = -1;
    while (p < n) {
        int b = d[p++];
        if (b == 0x3B) break;
        if (b == 0x21) {
            if (p >= n) break;
            int label = d[p++];
            if (label == 0xF9 && p + 5 < n && d[p] >= 4) trans = (d[p + 1] & 1) ? d[p + 4] : -1;
            while (p < n && d[p]) p += 1u + d[p];
            p++;
            continue;
        }
        if (b != 0x2C || p + 9 > n) return NULL;
        int fx = le16(d + p), fy = le16(d + p + 2), fw = le16(d + p + 4), fh = le16(d + p + 6), ff = d[p + 8];
        p += 9;
        uint32_t *pal = gct;
        int npal = ngct;
        if (ff & 0x80) {
            npal = 2 << (ff & 7);
            if (p + 3u * npal > n) return NULL;
            for (int i = 0; i < npal; i++) lct[i] = RGB(d[p + i * 3], d[p + i * 3 + 1], d[p + i * 3 + 2]);
            p += 3u * npal;
            pal = lct;
        }
        if (!sw || !sh) { sw = fw; sh = fh; }
        if (!fw || !fh || sw > 8192 || sh > 8192 || (uint32_t)sw * sh > MAX_PIXELS) return NULL;
        if (p >= n) return NULL;
        int mincode = d[p++];
        if (mincode < 2 || mincode > 11) return NULL;
        /* gather the data sub-blocks */
        uint32_t total = 0, q = p;
        while (q < n && d[q]) { total += d[q]; q += 1u + d[q]; }
        uint8_t *data = malloc(total + 4);
        uint8_t *idx = calloc(1, (size_t)fw * fh);
        uint16_t *prefix = malloc(4096 * sizeof(uint16_t));
        uint8_t *suffix = malloc(4096), *stack = malloc(4097);
        if (!data || !idx || !prefix || !suffix || !stack) { free(data); free(idx); free(prefix); free(suffix); free(stack); return NULL; }
        uint32_t o = 0;
        while (p < n && d[p]) {
            uint32_t l = d[p];
            if (p + 1 + l > n) l = n - p - 1;
            memcpy(data + o, d + p + 1, l);
            o += l;
            p += 1 + l;
        }
        /* LZW */
        int clear = 1 << mincode, eoi = clear + 1, next = clear + 2, size = mincode + 1;
        int old = -1, first = 0;
        uint32_t acc = 0, out = 0, npx = (uint32_t)fw * fh;
        int nb = 0;
        uint32_t ip = 0;
        for (int i = 0; i < clear; i++) { prefix[i] = 0xFFFF; suffix[i] = (uint8_t)i; }
        while (out < npx) {
            while (nb < size && ip < o) { acc |= (uint32_t)data[ip++] << nb; nb += 8; }
            if (nb < size) break;
            int code = (int)(acc & ((1u << size) - 1));
            acc >>= size;
            nb -= size;
            if (code == clear) { size = mincode + 1; next = clear + 2; old = -1; continue; }
            if (code == eoi) break;
            int sp = 0, c = code;
            if (old < 0) {
                if (code >= clear) break;
                idx[out++] = (uint8_t)code;
                old = first = code;
                continue;
            }
            if (code > next) break;
            if (code == next) { stack[sp++] = (uint8_t)first; c = old; }
            while (c >= clear && sp < 4096) { stack[sp++] = suffix[c]; c = prefix[c]; }
            first = c;
            stack[sp++] = (uint8_t)c;
            if (next < 4096) {
                prefix[next] = (uint16_t)old;
                suffix[next] = (uint8_t)first;
                next++;
                if (next == (1 << size) && size < 12) size++;
            }
            while (sp && out < npx) idx[out++] = stack[--sp];
            old = code;
        }
        free(data); free(prefix); free(suffix); free(stack);
        bool alpha = trans >= 0 || fx || fy || fw < sw || fh < sh;
        surface_t *s = alpha ? surface_create_alpha(sw, sh) : surface_create(sw, sh);
        if (!s) { free(idx); return NULL; }
        /* rows in display order (interlaced: 0,8,16.. 4,12.. 2,6,10.. 1,3,5..) */
        static const int istart[4] = { 0, 4, 2, 1 }, istep[4] = { 8, 8, 4, 2 };
        int r = 0;
        for (int pass = 0; pass < ((ff & 0x40) ? 4 : 1); pass++) {
            int y0 = (ff & 0x40) ? istart[pass] : 0, dy = (ff & 0x40) ? istep[pass] : 1;
            for (int y = y0; y < fh; y += dy, r++) {
                int yy = fy + y;
                if (yy >= sh) continue;
                uint32_t *dst = surface_row(s, yy);
                const uint8_t *src = idx + (size_t)r * fw;
                for (int x = 0; x < fw && fx + x < sw; x++) {
                    int ci = src[x];
                    dst[fx + x] = (ci == trans || ci >= npal) ? 0 : pal[ci];
                }
            }
        }
        free(idx);
        return s;
    }
    return NULL;
}

/* ==== dispatch ======================================================================= */
surface_t *web_image_decode(const uint8_t *data, uint32_t len) {
    if (!data || len < 4) return NULL;
    if (len >= 8 && !memcmp(data, "\x89PNG", 4)) return web_png_decode(data, len);
    if (data[0] == 0xFF && data[1] == 0xD8) return web_jpeg_decode(data, len);
    if (!memcmp(data, "GIF8", 4)) return web_gif_decode(data, len);
    if (data[0] == 'B' && data[1] == 'M') return image_decode(data, len);
    return NULL;
}
