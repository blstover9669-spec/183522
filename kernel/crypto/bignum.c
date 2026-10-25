/*
 * bignum.c - fixed-size unsigned big integers for RSA and the NIST curves: 32-bit
 * limbs, least significant first. Montgomery multiplication (CIOS) does the
 * modular arithmetic; numbers up to BN_MAX_LIMBS limbs (8192 bits).
 */
#include "kernel.h"
#include "crypto.h"

int bn_cmp(const uint32_t *a, const uint32_t *b, int n) {
    for (int i = n - 1; i >= 0; i--)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

bool bn_is_zero(const uint32_t *a, int n) {
    uint32_t x = 0;
    for (int i = 0; i < n; i++) x |= a[i];
    return x == 0;
}

uint32_t bn_add(uint32_t *r, const uint32_t *a, const uint32_t *b, int n) {
    uint64_t c = 0;
    for (int i = 0; i < n; i++) {
        c += (uint64_t)a[i] + b[i];
        r[i] = (uint32_t)c;
        c >>= 32;
    }
    return (uint32_t)c;
}

uint32_t bn_sub(uint32_t *r, const uint32_t *a, const uint32_t *b, int n) {
    int64_t c = 0;
    for (int i = 0; i < n; i++) {
        c += (int64_t)a[i] - b[i];
        r[i] = (uint32_t)c;
        c >>= 32;
    }
    return (uint32_t)(c & 1);         /* 1 = borrow */
}

/* Big-endian bytes -> n limbs. false if the value does not fit. */
bool bn_from_bytes(uint32_t *r, int n, const uint8_t *b, size_t len) {
    while (len && !*b) { b++; len--; }
    if (len > (size_t)n * 4) return false;
    memset(r, 0, (size_t)n * 4);
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        r[bit / 32] |= (uint32_t)b[i] << (bit % 32);
    }
    return true;
}

void bn_to_bytes(uint8_t *b, size_t len, const uint32_t *a, int n) {
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        b[i] = bit / 32 < (size_t)n ? (uint8_t)(a[bit / 32] >> (bit % 32)) : 0;
    }
}

uint32_t bn_m0inv(uint32_t m0) {
    uint32_t inv = 1;
    for (int i = 0; i < 5; i++) inv *= 2 - m0 * inv;     /* Newton: inv = m0^-1 mod 2^32 */
    return 0u - inv;
}

/* r = a * b * R^-1 mod m (R = 2^(32n)); a, b < m; r may alias a or b */
void bn_mont_mul(uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *m, uint32_t m0inv, int n) {
    uint32_t t[BN_MAX_LIMBS + 2];
    memset(t, 0, sizeof(uint32_t) * (size_t)(n + 2));
    for (int i = 0; i < n; i++) {
        uint64_t c = 0;
        uint32_t bi = b[i];
        for (int j = 0; j < n; j++) {
            c += (uint64_t)a[j] * bi + t[j];
            t[j] = (uint32_t)c;
            c >>= 32;
        }
        c += t[n];
        t[n] = (uint32_t)c;
        t[n + 1] = (uint32_t)(c >> 32);
        uint32_t u = t[0] * m0inv;
        c = (uint64_t)u * m[0] + t[0];
        c >>= 32;
        for (int j = 1; j < n; j++) {
            c += (uint64_t)u * m[j] + t[j];
            t[j - 1] = (uint32_t)c;
            c >>= 32;
        }
        c += t[n];
        t[n - 1] = (uint32_t)c;
        t[n] = t[n + 1] + (uint32_t)(c >> 32);
    }
    if (t[n] || bn_cmp(t, m, n) >= 0) bn_sub(t, t, m, n);
    memcpy(r, t, sizeof(uint32_t) * (size_t)n);
}

/* r = R^2 mod m (m odd, m > 1) */
void bn_r2(uint32_t *r, const uint32_t *m, int n) {
    memset(r, 0, sizeof(uint32_t) * (size_t)n);
    r[0] = 1;
    for (int i = 0; i < 64 * n; i++) {
        uint32_t carry = bn_add(r, r, r, n);
        if (carry || bn_cmp(r, m, n) >= 0) bn_sub(r, r, m, n);
    }
}

/* r = base^exp mod m (exp big endian bytes; public data: not constant time). base < m. */
void bn_modexp(uint32_t *r, const uint32_t *base, const uint8_t *exp, size_t elen, const uint32_t *m, int n) {
    uint32_t m0inv = bn_m0inv(m[0]);
    uint32_t *r2 = kmalloc(sizeof(uint32_t) * (size_t)n * 3);
    if (!r2) { memset(r, 0, sizeof(uint32_t) * (size_t)n); return; }
    uint32_t *bm = r2 + n, *acc = r2 + 2 * n;
    bn_r2(r2, m, n);
    bn_mont_mul(bm, base, r2, m, m0inv, n);          /* base in Montgomery form */
    memset(acc, 0, sizeof(uint32_t) * (size_t)n);
    acc[0] = 1;
    bn_mont_mul(acc, acc, r2, m, m0inv, n);           /* 1 in Montgomery form */
    bool started = false;
    for (size_t i = 0; i < elen; i++)
        for (int bit = 7; bit >= 0; bit--) {
            if (started) bn_mont_mul(acc, acc, acc, m, m0inv, n);
            if ((exp[i] >> bit) & 1) { bn_mont_mul(acc, acc, bm, m, m0inv, n); started = true; }
        }
    uint32_t one[BN_MAX_LIMBS];
    memset(one, 0, sizeof(uint32_t) * (size_t)n);
    one[0] = 1;
    bn_mont_mul(r, acc, one, m, m0inv, n);            /* out of Montgomery form */
    kfree(r2);
}
