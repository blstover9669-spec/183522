/*
 * x25519.c - X25519 Diffie-Hellman (RFC 7748).
 *
 * Field elements mod p = 2^255 - 19 are eight 32-bit limbs (little endian) holding a
 * value below 2^256 that is only fully reduced at the end. 2^256 = 38 (mod p), so a
 * 512-bit product folds into 256 bits by adding 38 times the high half. The
 * Montgomery ladder runs the same instructions for every scalar bit (conditional
 * swaps with masks), so it is constant time.
 */
#include "kernel.h"
#include "crypto.h"

typedef uint32_t fe[8];

static void fe_copy(fe r, const fe a) { memcpy(r, a, sizeof(fe)); }

/* r = a + 38 * carry folding of a 257-bit value held as 8 limbs + carry */
static void fe_fold(fe r, uint32_t carry) {
    for (int pass = 0; pass < 2; pass++) {
        uint64_t t = (uint64_t)carry * 38;
        for (int i = 0; i < 8; i++) {
            t += r[i];
            r[i] = (uint32_t)t;
            t >>= 32;
        }
        carry = (uint32_t)t;
    }
}

static void fe_add(fe r, const fe a, const fe b) {
    uint64_t t = 0;
    for (int i = 0; i < 8; i++) {
        t += (uint64_t)a[i] + b[i];
        r[i] = (uint32_t)t;
        t >>= 32;
    }
    fe_fold(r, (uint32_t)t);
}

static void fe_sub(fe r, const fe a, const fe b) {
    int64_t t = 0;
    for (int i = 0; i < 8; i++) {
        t += (int64_t)a[i] - b[i];
        r[i] = (uint32_t)t;
        t >>= 32;                        /* arithmetic: 0 or -1 */
    }
    /* a borrow means we hold value + 2^256 = value + 38 (mod p): subtract 38, twice at most */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t borrow = (uint32_t)(-t) & 1;
        t = -(int64_t)(borrow * 38);
        for (int i = 0; i < 8; i++) {
            t += r[i];
            r[i] = (uint32_t)t;
            t >>= 32;
        }
    }
}

static void fe_mul(fe r, const fe a, const fe b) {
    uint32_t t[16] = { 0 };
    for (int i = 0; i < 8; i++) {
        uint64_t c = 0;
        for (int j = 0; j < 8; j++) {
            c += (uint64_t)a[i] * b[j] + t[i + j];
            t[i + j] = (uint32_t)c;
            c >>= 32;
        }
        t[i + 8] = (uint32_t)c;
    }
    uint64_t c = 0;
    for (int i = 0; i < 8; i++) {
        c += (uint64_t)t[i + 8] * 38 + t[i];
        r[i] = (uint32_t)c;
        c >>= 32;
    }
    fe_fold(r, (uint32_t)c);
}

static void fe_sq(fe r, const fe a) { fe_mul(r, a, a); }

static void fe_mul_small(fe r, const fe a, uint32_t k) {
    uint64_t c = 0;
    for (int i = 0; i < 8; i++) {
        c += (uint64_t)a[i] * k;
        r[i] = (uint32_t)c;
        c >>= 32;
    }
    fe_fold(r, (uint32_t)c);
}

/* r = a^(p-2) = 1/a */
static void fe_inv(fe r, const fe a) {
    fe x;
    fe_copy(x, a);
    /* p - 2 = 2^255 - 21: bits 254..5 are 1, bits 4..0 = 01011 */
    fe acc;
    memset(acc, 0, sizeof acc);
    acc[0] = 1;
    for (int i = 254; i >= 0; i--) {
        fe_sq(acc, acc);
        bool bit = i >= 5 || ((0x0B >> i) & 1);
        if (bit) fe_mul(acc, acc, x);
    }
    fe_copy(r, acc);
}

/* fully reduce to [0, p) */
static void fe_freeze(fe r) {
    static const uint32_t P[8] = { 0xFFFFFFED, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                                   0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x7FFFFFFF };
    for (int pass = 0; pass < 2; pass++) {
        uint32_t s[8];
        int64_t t = 0;
        for (int i = 0; i < 8; i++) {
            t += (int64_t)r[i] - P[i];
            s[i] = (uint32_t)t;
            t >>= 32;
        }
        uint32_t keep = (uint32_t)t;     /* all ones if r < p (borrow): keep r */
        for (int i = 0; i < 8; i++) r[i] = (r[i] & keep) | (s[i] & ~keep);
    }
}

static void fe_cswap(fe a, fe b, uint32_t swap) {
    uint32_t m = 0u - swap;
    for (int i = 0; i < 8; i++) {
        uint32_t x = m & (a[i] ^ b[i]);
        a[i] ^= x;
        b[i] ^= x;
    }
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t k[32];
    memcpy(k, scalar, 32);
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;
    fe x1, x2, z2, x3, z3, a, aa, b, bb, e, c, d, da, cb;
    for (int i = 0; i < 8; i++)
        x1[i] = point[4 * i] | ((uint32_t)point[4 * i + 1] << 8) | ((uint32_t)point[4 * i + 2] << 16) |
                ((uint32_t)point[4 * i + 3] << 24);
    x1[7] &= 0x7FFFFFFF;
    memset(x2, 0, sizeof x2); x2[0] = 1;
    memset(z2, 0, sizeof z2);
    fe_copy(x3, x1);
    memset(z3, 0, sizeof z3); z3[0] = 1;
    uint32_t swap = 0;
    for (int t = 254; t >= 0; t--) {
        uint32_t kt = (k[t >> 3] >> (t & 7)) & 1;
        swap ^= kt;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = kt;
        fe_add(a, x2, z2);
        fe_sq(aa, a);
        fe_sub(b, x2, z2);
        fe_sq(bb, b);
        fe_sub(e, aa, bb);
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        fe_mul(da, d, a);
        fe_mul(cb, c, b);
        fe_add(x3, da, cb);
        fe_sq(x3, x3);
        fe_sub(z3, da, cb);
        fe_sq(z3, z3);
        fe_mul(z3, z3, x1);
        fe_mul(x2, aa, bb);
        fe_mul_small(z2, e, 121665);
        fe_add(z2, z2, aa);
        fe_mul(z2, z2, e);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);
    fe_inv(z2, z2);
    fe_mul(x2, x2, z2);
    fe_freeze(x2);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)x2[i];
        out[4 * i + 1] = (uint8_t)(x2[i] >> 8);
        out[4 * i + 2] = (uint8_t)(x2[i] >> 16);
        out[4 * i + 3] = (uint8_t)(x2[i] >> 24);
    }
    secure_zero(k, sizeof k);
    secure_zero(x2, sizeof x2);
    secure_zero(z2, sizeof z2);
    secure_zero(x3, sizeof x3);
    secure_zero(z3, sizeof z3);
}

void x25519_public(uint8_t pub[32], const uint8_t priv[32]) {
    static const uint8_t base[32] = { 9 };
    x25519(pub, priv, base);
}
