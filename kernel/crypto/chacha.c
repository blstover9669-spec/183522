/* chacha.c - ChaCha20, Poly1305 and the ChaCha20-Poly1305 AEAD (RFC 8439). */
#include "kernel.h"
#include "crypto.h"

static inline uint32_t le32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

#define QR(a, b, c, d)                       \
    a += b; d ^= a; d = rol(d, 16);          \
    c += d; b ^= c; b = rol(b, 12);          \
    a += b; d ^= a; d = rol(d, 8);           \
    c += d; b ^= c; b = rol(b, 7)

void chacha20_block(const uint32_t key[8], uint32_t counter, const uint32_t nonce[3], uint8_t out[64]) {
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = key[i];
    s[12] = counter;
    s[13] = nonce[0]; s[14] = nonce[1]; s[15] = nonce[2];
    memcpy(x, s, sizeof s);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) put_le32(out + 4 * i, x[i] + s[i]);
    secure_zero(x, sizeof x);
    secure_zero(s, sizeof s);
}

void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t *data, size_t len) {
    uint32_t k[8], n[3];
    uint8_t ks[64];
    for (int i = 0; i < 8; i++) k[i] = le32(key + 4 * i);
    for (int i = 0; i < 3; i++) n[i] = le32(nonce + 4 * i);
    while (len) {
        chacha20_block(k, counter++, n, ks);
        size_t m = len < 64 ? len : 64;
        for (size_t i = 0; i < m; i++) data[i] ^= ks[i];
        data += m;
        len -= m;
    }
    secure_zero(ks, sizeof ks);
    secure_zero(k, sizeof k);
}

/* ---- Poly1305 (26-bit limbs) ----------------------------------------------------------------- */
typedef struct {
    uint32_t r[5], s[4], h[5];
    uint8_t buf[16];
    size_t n;
} poly_t;

static void poly_init(poly_t *p, const uint8_t key[32]) {
    p->r[0] = le32(key + 0) & 0x3ffffff;
    p->r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (le32(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 4; i++) p->s[i] = le32(key + 16 + 4 * i);
    memset(p->h, 0, sizeof p->h);
    p->n = 0;
}

static void poly_blocks(poly_t *p, const uint8_t *m, size_t len, uint32_t hibit) {
    uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    while (len >= 16) {
        h0 += le32(m + 0) & 0x3ffffff;
        h1 += (le32(m + 3) >> 2) & 0x3ffffff;
        h2 += (le32(m + 6) >> 4) & 0x3ffffff;
        h3 += (le32(m + 9) >> 6) & 0x3ffffff;
        h4 += (le32(m + 12) >> 8) | hibit;
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
        m += 16;
        len -= 16;
    }
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

static void poly_update(poly_t *p, const uint8_t *m, size_t len) {
    if (p->n) {
        size_t k = MIN(len, 16 - p->n);
        memcpy(p->buf + p->n, m, k);
        p->n += k; m += k; len -= k;
        if (p->n < 16) return;
        poly_blocks(p, p->buf, 16, 1u << 24);
        p->n = 0;
    }
    size_t full = len & ~(size_t)15;
    if (full) poly_blocks(p, m, full, 1u << 24);
    m += full;
    len -= full;
    if (len) { memcpy(p->buf, m, len); p->n = len; }
}

static void poly_pad16(poly_t *p) {
    static const uint8_t zero[16];
    if (p->n) poly_update(p, zero, 16 - p->n);
}

static void poly_finish(poly_t *p, uint8_t tag[16]) {
    if (p->n) {
        p->buf[p->n++] = 1;
        while (p->n < 16) p->buf[p->n++] = 0;
        poly_blocks(p, p->buf, 16, 0);
    }
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    /* g = h + 5 - 2^130; use g if it did not go negative */
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;            /* all ones when g4 >= 0 */
    h0 = (h0 & ~mask) | (g0 & mask);
    h1 = (h1 & ~mask) | (g1 & mask);
    h2 = (h2 & ~mask) | (g2 & mask);
    h3 = (h3 & ~mask) | (g3 & mask);
    h4 = (h4 & ~mask) | (g4 & mask);
    uint32_t w0 = h0 | (h1 << 26);
    uint32_t w1 = (h1 >> 6) | (h2 << 20);
    uint32_t w2 = (h2 >> 12) | (h3 << 14);
    uint32_t w3 = (h3 >> 18) | (h4 << 8);
    uint64_t f = (uint64_t)w0 + p->s[0];          put_le32(tag, (uint32_t)f);
    f = (uint64_t)w1 + p->s[1] + (f >> 32);       put_le32(tag + 4, (uint32_t)f);
    f = (uint64_t)w2 + p->s[2] + (f >> 32);       put_le32(tag + 8, (uint32_t)f);
    f = (uint64_t)w3 + p->s[3] + (f >> 32);       put_le32(tag + 12, (uint32_t)f);
    secure_zero(p, sizeof *p);
}

void poly1305_mac(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t tag[16]) {
    poly_t p;
    poly_init(&p, key);
    poly_update(&p, msg, len);
    poly_finish(&p, tag);
}

/* ---- AEAD ------------------------------------------------------------------------------------ */
static void aead_tag(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                     const uint8_t *ct, size_t len, uint8_t tag[16]) {
    uint8_t otk[64] = { 0 };
    chacha20_xor(key, 0, nonce, otk, 64);       /* one-time Poly1305 key = first 32 bytes of block 0 */
    poly_t p;
    poly_init(&p, otk);
    poly_update(&p, aad, aadlen);
    poly_pad16(&p);
    poly_update(&p, ct, len);
    poly_pad16(&p);
    uint8_t lens[16];
    put_le32(lens, (uint32_t)aadlen);
    put_le32(lens + 4, (uint32_t)((uint64_t)aadlen >> 32));
    put_le32(lens + 8, (uint32_t)len);
    put_le32(lens + 12, (uint32_t)((uint64_t)len >> 32));
    poly_update(&p, lens, 16);
    poly_finish(&p, tag);
    secure_zero(otk, sizeof otk);
}

void chacha20poly1305_encrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                              uint8_t *data, size_t len, uint8_t tag[16]) {
    chacha20_xor(key, 1, nonce, data, len);
    aead_tag(key, nonce, aad, aadlen, data, len, tag);
}

bool chacha20poly1305_decrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                              uint8_t *data, size_t len, const uint8_t tag[16]) {
    uint8_t t[16];
    aead_tag(key, nonce, aad, aadlen, data, len, t);
    if (!ct_equal(t, tag, 16)) return false;
    chacha20_xor(key, 1, nonce, data, len);
    return true;
}
