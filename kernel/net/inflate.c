/*
 * inflate.c - DEFLATE decoder (RFC 1951) with zlib (RFC 1950) and gzip (RFC 1952)
 * wrappers, for HTTP Content-Encoding and PNG images.
 *
 * Canonical Huffman decoding with a 10-bit lookup table (codes up to 10 bits
 * decode in one step, longer ones fall back to a short canonical walk). The
 * gzip CRC-32 and zlib Adler-32 trailers are checked.
 */
#include "net_internal.h"

#define MAXBITS  15
#define FASTBITS 10

typedef struct {
    uint16_t count[MAXBITS + 1];
    uint16_t symbol[320];
    uint16_t fast[1 << FASTBITS];      /* (symbol << 4) | length, 0 = not in table */
} huff_t;

typedef struct {
    const uint8_t *in;
    uint32_t in_len, pos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    uint32_t out_len, out_cap, max_out;
    int err;                           /* 0, -1 corrupt, -2 too big, -3 memory */
} inf_t;

static inline void need(inf_t *s, int n) {
    while (s->bitcnt < n) {
        uint32_t b = 0;
        if (s->pos < s->in_len) b = s->in[s->pos];
        else if (s->pos > s->in_len + 4) { s->err = -1; }   /* ran far past the end */
        s->pos++;
        s->bitbuf |= b << s->bitcnt;
        s->bitcnt += 8;
    }
}

static inline uint32_t bits(inf_t *s, int n) {
    if (n == 0) return 0;
    need(s, n);
    uint32_t v = s->bitbuf & ((1u << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

static bool grow(inf_t *s, uint32_t extra) {
    uint64_t want = (uint64_t)s->out_len + extra;
    if (want > s->max_out) { s->err = -2; return false; }
    if (want + 1 <= s->out_cap) return true;
    uint64_t cap = s->out_cap ? s->out_cap : 65536;
    while (cap < want + 1) cap *= 2;
    if (cap > (uint64_t)s->max_out + 1) cap = (uint64_t)s->max_out + 1;
    uint8_t *n = krealloc(s->out, (size_t)cap);
    if (!n) { s->err = -3; return false; }
    s->out = n;
    s->out_cap = (uint32_t)cap;
    return true;
}

/* Build a decoding table from code lengths; false if over-subscribed. */
static bool build(huff_t *h, const uint8_t *len, int n) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    if (h->count[0] == n) { memset(h->fast, 0, sizeof h->fast); return true; }   /* empty (allowed for distances) */
    int left = 1;
    for (int i = 1; i <= MAXBITS; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return false;
    }
    uint16_t offs[MAXBITS + 1];
    offs[1] = 0;
    for (int i = 1; i < MAXBITS; i++) offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++) if (len[i]) h->symbol[offs[len[i]]++] = (uint16_t)i;
    /* fast table: walk codes in canonical order */
    memset(h->fast, 0, sizeof h->fast);
    uint32_t code = 0;
    int idx = 0;
    for (int l = 1; l <= FASTBITS; l++) {
        for (int k = 0; k < h->count[l]; k++, idx++) {
            /* the code is read LSB-first from the stream: reverse it */
            uint32_t rev = 0;
            for (int b = 0; b < l; b++) rev |= ((code >> b) & 1u) << (l - 1 - b);
            for (uint32_t f = rev; f < (1u << FASTBITS); f += 1u << l)
                h->fast[f] = (uint16_t)((h->symbol[idx] << 4) | l);
            code++;
        }
        code <<= 1;
    }
    return true;
}

static int decode(inf_t *s, const huff_t *h) {
    need(s, FASTBITS);
    uint16_t e = h->fast[s->bitbuf & ((1u << FASTBITS) - 1)];
    if (e) {
        int l = e & 15;
        s->bitbuf >>= l;
        s->bitcnt -= l;
        return e >> 4;
    }
    /* slow path: canonical decode bit by bit */
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= MAXBITS; l++) {
        code |= (int)bits(s, 1);
        int count = h->count[l];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = -1;
    return -1;
}

static const uint16_t LBASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t LEXT[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t DBASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t DEXT[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(inf_t *s, const huff_t *lc, const huff_t *dc) {
    for (;;) {
        int sym = decode(s, lc);
        if (s->err) return s->err;
        if (sym < 256) {
            if (s->out_len + 1 >= s->out_cap && !grow(s, 1)) return s->err;
            if (s->out_len >= s->max_out) { s->err = -2; return s->err; }
            s->out[s->out_len++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return s->err = -1;
            uint32_t len = LBASE[sym] + bits(s, LEXT[sym]);
            int ds = decode(s, dc);
            if (s->err) return s->err;
            if (ds < 0 || ds >= 30) return s->err = -1;
            uint32_t dist = DBASE[ds] + bits(s, DEXT[ds]);
            if (dist > s->out_len) return s->err = -1;
            if (!grow(s, len)) return s->err;
            uint8_t *o = s->out + s->out_len;
            const uint8_t *from = o - dist;
            for (uint32_t i = 0; i < len; i++) o[i] = from[i];
            s->out_len += len;
        }
    }
}

static int stored(inf_t *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;                         /* to a byte boundary (the bits buffered are whole bytes) */
    if (s->pos + 4 > s->in_len) return s->err = -1;
    uint32_t len = s->in[s->pos] | (s->in[s->pos + 1] << 8);
    uint32_t nlen = s->in[s->pos + 2] | (s->in[s->pos + 3] << 8);
    s->pos += 4;
    if (len != (~nlen & 0xFFFF) || s->pos + len > s->in_len) return s->err = -1;
    if (!grow(s, len)) return s->err;
    memcpy(s->out + s->out_len, s->in + s->pos, len);
    s->out_len += len;
    s->pos += len;
    return 0;
}

static int run(inf_t *s) {
    huff_t *lc = kmalloc(sizeof(huff_t)), *dc = kmalloc(sizeof(huff_t));
    if (!lc || !dc) { kfree(lc); kfree(dc); return s->err = -3; }
    int last;
    do {
        last = (int)bits(s, 1);
        int type = (int)bits(s, 2);
        if (s->err) break;
        if (type == 0) {
            /* drop the partial byte: un-read whole buffered bytes */
            int whole = s->bitcnt / 8;
            s->pos -= (uint32_t)whole;
            stored(s);
        } else if (type == 1) {
            uint8_t len[288 + 30];
            int i = 0;
            for (; i < 144; i++) len[i] = 8;
            for (; i < 256; i++) len[i] = 9;
            for (; i < 280; i++) len[i] = 7;
            for (; i < 288; i++) len[i] = 8;
            for (i = 0; i < 30; i++) len[288 + i] = 5;
            build(lc, len, 288);
            build(dc, len + 288, 30);
            codes(s, lc, dc);
        } else if (type == 2) {
            int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1, ncode = (int)bits(s, 4) + 4;
            static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            uint8_t len[320];
            if (nlen > 286 || ndist > 30) { s->err = -1; break; }
            memset(len, 0, 19);
            for (int i = 0; i < ncode; i++) len[order[i]] = (uint8_t)bits(s, 3);
            if (!build(lc, len, 19)) { s->err = -1; break; }
            int idx = 0;
            while (idx < nlen + ndist && !s->err) {
                int sym = decode(s, lc);
                if (sym < 0) break;
                if (sym < 16) { len[idx++] = (uint8_t)sym; continue; }
                uint8_t v = 0;
                int rep;
                if (sym == 16) { if (!idx) { s->err = -1; break; } v = len[idx - 1]; rep = 3 + (int)bits(s, 2); }
                else if (sym == 17) rep = 3 + (int)bits(s, 3);
                else rep = 11 + (int)bits(s, 7);
                if (idx + rep > nlen + ndist) { s->err = -1; break; }
                while (rep--) len[idx++] = v;
            }
            if (s->err) break;
            if (!len[256] || !build(lc, len, nlen) || !build(dc, len + nlen, ndist)) { s->err = -1; break; }
            codes(s, lc, dc);
        } else {
            s->err = -1;
        }
    } while (!last && !s->err);
    kfree(lc);
    kfree(dc);
    /* give back whole bytes that were buffered but not used */
    if (!s->err) s->pos -= (uint32_t)(s->bitcnt / 8);
    return s->err;
}

static uint32_t crc_table[256];
static uint32_t crc32(const uint8_t *p, uint32_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}

static uint32_t adler32(const uint8_t *p, uint32_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        uint32_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

int inflate_buffer(const void *in, uint32_t in_len, int format, uint8_t **out, uint32_t *out_len, uint32_t max_out) {
    *out = NULL;
    *out_len = 0;
    const uint8_t *p = in;
    inf_t s = { 0 };
    s.in = p;
    s.in_len = in_len;
    s.max_out = max_out ? max_out : 64u << 20;
    if (format == INFLATE_AUTO) {
        if (in_len >= 2 && p[0] == 0x1F && p[1] == 0x8B) format = INFLATE_GZIP;
        else if (in_len >= 2 && (p[0] & 0x0F) == 8 && (p[0] >> 4) <= 7 && ((p[0] << 8) | p[1]) % 31 == 0) format = INFLATE_ZLIB;
        else format = INFLATE_RAW;
    }
    if (format == INFLATE_GZIP) {
        if (in_len < 18 || p[0] != 0x1F || p[1] != 0x8B || p[2] != 8) return -1;
        uint8_t flg = p[3];
        uint32_t o = 10;
        if (flg & 4) { if (o + 2 > in_len) return -1; o += 2u + (p[o] | (p[o + 1] << 8)); }
        if (flg & 8) { while (o < in_len && p[o]) o++; o++; }
        if (flg & 16) { while (o < in_len && p[o]) o++; o++; }
        if (flg & 2) o += 2;
        if (o >= in_len) return -1;
        s.pos = o;
    } else if (format == INFLATE_ZLIB) {
        if (in_len < 6 || (p[0] & 0x0F) != 8 || ((p[0] << 8) | p[1]) % 31 || (p[1] & 0x20)) return -1;
        s.pos = 2;
    }
    if (!grow(&s, 0)) { kfree(s.out); return s.err; }
    int r = run(&s);
    if (r == 0) {
        if (format == INFLATE_GZIP) {
            if (s.pos + 8 <= in_len) {
                uint32_t crc = p[s.pos] | (p[s.pos + 1] << 8) | (p[s.pos + 2] << 16) | ((uint32_t)p[s.pos + 3] << 24);
                if (crc != crc32(s.out, s.out_len)) r = -1;
            }
        } else if (format == INFLATE_ZLIB) {
            if (s.pos + 4 <= in_len && rd32be(p + s.pos) != adler32(s.out, s.out_len)) r = -1;
        }
    }
    if (r) { kfree(s.out); return r; }
    s.out[s.out_len] = 0;
    *out = s.out;
    *out_len = s.out_len;
    return 0;
}
