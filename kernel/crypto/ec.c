/*
 * ec.c - NIST P-256 and P-384 (FIPS 186-4): ECDH key pairs and shared secrets, ECDSA
 * signature verification.
 *
 * Field elements are kept in Montgomery form (bignum.c), points in Jacobian
 * coordinates (a = -3 formulas), scalar multiplication with a 4-bit window.
 * TLS prefers X25519; these curves are used when a server insists on them and
 * for certificate signatures.
 */
#include "kernel.h"
#include "crypto.h"

#define EC_LIMBS 12

typedef struct {
    int      n, size;                       /* limbs, bytes */
    uint32_t p[EC_LIMBS], p0inv, pr2[EC_LIMBS], one[EC_LIMBS], b[EC_LIMBS], gx[EC_LIMBS], gy[EC_LIMBS];
    uint32_t pm2[EC_LIMBS];                 /* p - 2 (plain) */
    uint32_t q[EC_LIMBS], q0inv, qr2[EC_LIMBS], qm2[EC_LIMBS];   /* group order */
    bool     ready;
} curve_t;

typedef struct { uint32_t x[EC_LIMBS], y[EC_LIMBS], z[EC_LIMBS]; } pt_t;   /* z == 0: infinity */

static const uint8_t P256_P[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static const uint8_t P256_B[32] = {
    0x5A,0xC6,0x35,0xD8,0xAA,0x3A,0x93,0xE7,0xB3,0xEB,0xBD,0x55,0x76,0x98,0x86,0xBC,
    0x65,0x1D,0x06,0xB0,0xCC,0x53,0xB0,0xF6,0x3B,0xCE,0x3C,0x3E,0x27,0xD2,0x60,0x4B };
static const uint8_t P256_GX[32] = {
    0x6B,0x17,0xD1,0xF2,0xE1,0x2C,0x42,0x47,0xF8,0xBC,0xE6,0xE5,0x63,0xA4,0x40,0xF2,
    0x77,0x03,0x7D,0x81,0x2D,0xEB,0x33,0xA0,0xF4,0xA1,0x39,0x45,0xD8,0x98,0xC2,0x96 };
static const uint8_t P256_GY[32] = {
    0x4F,0xE3,0x42,0xE2,0xFE,0x1A,0x7F,0x9B,0x8E,0xE7,0xEB,0x4A,0x7C,0x0F,0x9E,0x16,
    0x2B,0xCE,0x33,0x57,0x6B,0x31,0x5E,0xCE,0xCB,0xB6,0x40,0x68,0x37,0xBF,0x51,0xF5 };
static const uint8_t P256_N[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xBC,0xE6,0xFA,0xAD,0xA7,0x17,0x9E,0x84,0xF3,0xB9,0xCA,0xC2,0xFC,0x63,0x25,0x51 };

static const uint8_t P384_P[48] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF };
static const uint8_t P384_B[48] = {
    0xB3,0x31,0x2F,0xA7,0xE2,0x3E,0xE7,0xE4,0x98,0x8E,0x05,0x6B,0xE3,0xF8,0x2D,0x19,
    0x18,0x1D,0x9C,0x6E,0xFE,0x81,0x41,0x12,0x03,0x14,0x08,0x8F,0x50,0x13,0x87,0x5A,
    0xC6,0x56,0x39,0x8D,0x8A,0x2E,0xD1,0x9D,0x2A,0x85,0xC8,0xED,0xD3,0xEC,0x2A,0xEF };
static const uint8_t P384_GX[48] = {
    0xAA,0x87,0xCA,0x22,0xBE,0x8B,0x05,0x37,0x8E,0xB1,0xC7,0x1E,0xF3,0x20,0xAD,0x74,
    0x6E,0x1D,0x3B,0x62,0x8B,0xA7,0x9B,0x98,0x59,0xF7,0x41,0xE0,0x82,0x54,0x2A,0x38,
    0x55,0x02,0xF2,0x5D,0xBF,0x55,0x29,0x6C,0x3A,0x54,0x5E,0x38,0x72,0x76,0x0A,0xB7 };
static const uint8_t P384_GY[48] = {
    0x36,0x17,0xDE,0x4A,0x96,0x26,0x2C,0x6F,0x5D,0x9E,0x98,0xBF,0x92,0x92,0xDC,0x29,
    0xF8,0xF4,0x1D,0xBD,0x28,0x9A,0x14,0x7C,0xE9,0xDA,0x31,0x13,0xB5,0xF0,0xB8,0xC0,
    0x0A,0x60,0xB1,0xCE,0x1D,0x7E,0x81,0x9D,0x7A,0x43,0x1D,0x7C,0x90,0xEA,0x0E,0x5F };
static const uint8_t P384_N[48] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xC7,0x63,0x4D,0x81,0xF4,0x37,0x2D,0xDF,
    0x58,0x1A,0x0D,0xB2,0x48,0xB0,0xA7,0x7A,0xEC,0xEC,0x19,0x6A,0xCC,0xC5,0x29,0x73 };

static curve_t curves[2];

int ec_size(int curve) { return curve == EC_P256 ? 32 : curve == EC_P384 ? 48 : 0; }

static void sub_small(uint32_t *r, const uint32_t *a, uint32_t k, int n) {
    int64_t c = -(int64_t)k;
    for (int i = 0; i < n; i++) {
        c += a[i];
        r[i] = (uint32_t)c;
        c >>= 32;
    }
}

/* ---- field arithmetic mod p (Montgomery form) ----------------------------------------------- */
static void fmul(const curve_t *c, uint32_t *r, const uint32_t *a, const uint32_t *b) {
    bn_mont_mul(r, a, b, c->p, c->p0inv, c->n);
}
static void fadd(const curve_t *c, uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint32_t carry = bn_add(r, a, b, c->n);
    if (carry || bn_cmp(r, c->p, c->n) >= 0) bn_sub(r, r, c->p, c->n);
}
static void fsub(const curve_t *c, uint32_t *r, const uint32_t *a, const uint32_t *b) {
    if (bn_sub(r, a, b, c->n)) bn_add(r, r, c->p, c->n);
}
static void to_mont(const curve_t *c, uint32_t *r, const uint32_t *a) { fmul(c, r, a, c->pr2); }
static void from_mont(const curve_t *c, uint32_t *r, const uint32_t *a) {
    uint32_t one[EC_LIMBS] = { 1 };
    fmul(c, r, a, one);
}

/* r = a^e in Montgomery form (e plain limbs, public) */
static void mpow(uint32_t *r, const uint32_t *a, const uint32_t *e, const uint32_t *one,
                 const uint32_t *m, uint32_t m0inv, int n) {
    uint32_t acc[EC_LIMBS];
    memcpy(acc, one, sizeof(uint32_t) * (size_t)n);
    for (int i = n * 32 - 1; i >= 0; i--) {
        bn_mont_mul(acc, acc, acc, m, m0inv, n);
        if ((e[i / 32] >> (i % 32)) & 1) bn_mont_mul(acc, acc, a, m, m0inv, n);
    }
    memcpy(r, acc, sizeof(uint32_t) * (size_t)n);
}

static void finv(const curve_t *c, uint32_t *r, const uint32_t *a) { mpow(r, a, c->pm2, c->one, c->p, c->p0inv, c->n); }

static const curve_t *get_curve(int id) {
    if (id != EC_P256 && id != EC_P384) return NULL;
    curve_t *c = &curves[id - 1];
    if (c->ready) return c;
    bool big = id == EC_P384;
    c->size = big ? 48 : 32;
    c->n = c->size / 4;
    uint32_t t[EC_LIMBS];
    bn_from_bytes(c->p, c->n, big ? P384_P : P256_P, (size_t)c->size);
    c->p0inv = bn_m0inv(c->p[0]);
    bn_r2(c->pr2, c->p, c->n);
    memset(t, 0, sizeof t);
    t[0] = 1;
    to_mont(c, c->one, t);
    bn_from_bytes(t, c->n, big ? P384_B : P256_B, (size_t)c->size);
    to_mont(c, c->b, t);
    bn_from_bytes(t, c->n, big ? P384_GX : P256_GX, (size_t)c->size);
    to_mont(c, c->gx, t);
    bn_from_bytes(t, c->n, big ? P384_GY : P256_GY, (size_t)c->size);
    to_mont(c, c->gy, t);
    sub_small(c->pm2, c->p, 2, c->n);
    bn_from_bytes(c->q, c->n, big ? P384_N : P256_N, (size_t)c->size);
    c->q0inv = bn_m0inv(c->q[0]);
    bn_r2(c->qr2, c->q, c->n);
    sub_small(c->qm2, c->q, 2, c->n);
    c->ready = true;
    return c;
}

/* ---- points ------------------------------------------------------------------------------------ */
static bool pt_is_inf(const curve_t *c, const pt_t *p) { return bn_is_zero(p->z, c->n); }

static void pt_double(const curve_t *c, pt_t *r, const pt_t *p) {
    if (pt_is_inf(c, p) || bn_is_zero(p->y, c->n)) { memset(r, 0, sizeof *r); return; }
    uint32_t delta[EC_LIMBS], gamma[EC_LIMBS], beta[EC_LIMBS], alpha[EC_LIMBS], t1[EC_LIMBS], t2[EC_LIMBS];
    fmul(c, delta, p->z, p->z);
    fmul(c, gamma, p->y, p->y);
    fmul(c, beta, p->x, gamma);
    fsub(c, t1, p->x, delta);
    fadd(c, t2, p->x, delta);
    fmul(c, alpha, t1, t2);
    fadd(c, t1, alpha, alpha);
    fadd(c, alpha, t1, alpha);                  /* alpha = 3 (X - delta)(X + delta) */
    fadd(c, t1, p->y, p->z);
    fmul(c, t1, t1, t1);
    fsub(c, t1, t1, gamma);
    fsub(c, r->z, t1, delta);                   /* Z3 = (Y + Z)^2 - gamma - delta */
    fadd(c, beta, beta, beta);
    fadd(c, beta, beta, beta);                  /* 4 beta */
    fmul(c, t1, alpha, alpha);
    fadd(c, t2, beta, beta);
    fsub(c, r->x, t1, t2);                      /* X3 = alpha^2 - 8 beta */
    fsub(c, t1, beta, r->x);
    fmul(c, t1, alpha, t1);
    fmul(c, t2, gamma, gamma);
    fadd(c, t2, t2, t2);
    fadd(c, t2, t2, t2);
    fadd(c, t2, t2, t2);                        /* 8 gamma^2 */
    fsub(c, r->y, t1, t2);                      /* Y3 = alpha (4 beta - X3) - 8 gamma^2 */
}

static void pt_add(const curve_t *c, pt_t *r, const pt_t *p, const pt_t *q) {
    if (pt_is_inf(c, p)) { *r = *q; return; }
    if (pt_is_inf(c, q)) { *r = *p; return; }
    uint32_t z1z1[EC_LIMBS], z2z2[EC_LIMBS], u1[EC_LIMBS], u2[EC_LIMBS], s1[EC_LIMBS], s2[EC_LIMBS];
    uint32_t h[EC_LIMBS], rr[EC_LIMBS], i[EC_LIMBS], j[EC_LIMBS], v[EC_LIMBS], t[EC_LIMBS];
    fmul(c, z1z1, p->z, p->z);
    fmul(c, z2z2, q->z, q->z);
    fmul(c, u1, p->x, z2z2);
    fmul(c, u2, q->x, z1z1);
    fmul(c, s1, p->y, q->z);
    fmul(c, s1, s1, z2z2);
    fmul(c, s2, q->y, p->z);
    fmul(c, s2, s2, z1z1);
    fsub(c, h, u2, u1);
    fsub(c, rr, s2, s1);
    if (bn_is_zero(h, c->n)) {
        if (bn_is_zero(rr, c->n)) pt_double(c, r, p);
        else memset(r, 0, sizeof *r);
        return;
    }
    fadd(c, rr, rr, rr);
    fadd(c, i, h, h);
    fmul(c, i, i, i);                           /* I = (2H)^2 */
    fmul(c, j, h, i);                           /* J = H I */
    fmul(c, v, u1, i);                          /* V = U1 I */
    pt_t out;
    fmul(c, t, rr, rr);
    fsub(c, t, t, j);
    fsub(c, t, t, v);
    fsub(c, out.x, t, v);                       /* X3 = r^2 - J - 2V */
    fsub(c, t, v, out.x);
    fmul(c, t, rr, t);
    fmul(c, s1, s1, j);
    fadd(c, s1, s1, s1);
    fsub(c, out.y, t, s1);                      /* Y3 = r (V - X3) - 2 S1 J */
    fadd(c, t, p->z, q->z);
    fmul(c, t, t, t);
    fsub(c, t, t, z1z1);
    fsub(c, t, t, z2z2);
    fmul(c, out.z, t, h);                       /* Z3 = ((Z1 + Z2)^2 - Z1Z1 - Z2Z2) H */
    *r = out;
}

/* r = k P, k big endian (size bytes) */
static void pt_mul(const curve_t *c, pt_t *r, const uint8_t *k, const pt_t *p) {
    pt_t *tab = kmalloc(sizeof(pt_t) * 16);
    if (!tab) { memset(r, 0, sizeof *r); return; }
    memset(&tab[0], 0, sizeof(pt_t));
    tab[1] = *p;
    for (int i = 2; i < 16; i++) pt_add(c, &tab[i], &tab[i - 1], p);
    pt_t acc;
    memset(&acc, 0, sizeof acc);
    for (int i = 0; i < c->size * 2; i++) {
        int nib = (k[i / 2] >> ((i & 1) ? 0 : 4)) & 15;
        for (int d = 0; d < 4; d++) pt_double(c, &acc, &acc);
        if (nib) pt_add(c, &acc, &acc, &tab[nib]);
    }
    *r = acc;
    secure_zero(tab, sizeof(pt_t) * 16);
    kfree(tab);
}

static bool pt_affine(const curve_t *c, const pt_t *p, uint32_t *x, uint32_t *y) {
    if (pt_is_inf(c, p)) return false;
    uint32_t zi[EC_LIMBS], zi2[EC_LIMBS], t[EC_LIMBS];
    finv(c, zi, p->z);
    fmul(c, zi2, zi, zi);
    fmul(c, t, p->x, zi2);
    from_mont(c, x, t);
    if (y) {
        fmul(c, zi2, zi2, zi);
        fmul(c, t, p->y, zi2);
        from_mont(c, y, t);
    }
    return true;
}

/* uncompressed point 04 || X || Y -> Montgomery-form point; checks it is on the curve */
static bool pt_load(const curve_t *c, pt_t *r, const uint8_t *pub, size_t len) {
    if (len != 1 + 2 * (size_t)c->size || pub[0] != 4) return false;
    uint32_t x[EC_LIMBS], y[EC_LIMBS], lhs[EC_LIMBS], rhs[EC_LIMBS], t[EC_LIMBS];
    bn_from_bytes(x, c->n, pub + 1, (size_t)c->size);
    bn_from_bytes(y, c->n, pub + 1 + c->size, (size_t)c->size);
    if (bn_cmp(x, c->p, c->n) >= 0 || bn_cmp(y, c->p, c->n) >= 0) return false;
    to_mont(c, r->x, x);
    to_mont(c, r->y, y);
    memcpy(r->z, c->one, sizeof(uint32_t) * (size_t)c->n);
    fmul(c, lhs, r->y, r->y);
    fmul(c, rhs, r->x, r->x);
    fmul(c, rhs, rhs, r->x);                    /* x^3 */
    fadd(c, t, r->x, r->x);
    fadd(c, t, t, r->x);
    fsub(c, rhs, rhs, t);                       /* - 3x */
    fadd(c, rhs, rhs, c->b);
    return bn_cmp(lhs, rhs, c->n) == 0;
}

/* ---- public API ------------------------------------------------------------------------------- */
bool ec_keygen(int curve, uint8_t *priv, uint8_t *pub) {
    const curve_t *c = get_curve(curve);
    if (!c) return false;
    uint32_t k[EC_LIMBS];
    for (int tries = 0; tries < 64; tries++) {
        random_bytes(priv, (size_t)c->size);
        bn_from_bytes(k, c->n, priv, (size_t)c->size);
        if (!bn_is_zero(k, c->n) && bn_cmp(k, c->q, c->n) < 0) break;
    }
    pt_t g, r;
    memcpy(g.x, c->gx, sizeof g.x);
    memcpy(g.y, c->gy, sizeof g.y);
    memcpy(g.z, c->one, sizeof g.z);
    pt_mul(c, &r, priv, &g);
    uint32_t x[EC_LIMBS], y[EC_LIMBS];
    if (!pt_affine(c, &r, x, y)) return false;
    pub[0] = 4;
    bn_to_bytes(pub + 1, (size_t)c->size, x, c->n);
    bn_to_bytes(pub + 1 + c->size, (size_t)c->size, y, c->n);
    secure_zero(k, sizeof k);
    return true;
}

bool ec_ecdh(int curve, const uint8_t *priv, const uint8_t *peer_pub, size_t peer_len, uint8_t *shared) {
    const curve_t *c = get_curve(curve);
    pt_t p, r;
    if (!c || !pt_load(c, &p, peer_pub, peer_len)) return false;
    pt_mul(c, &r, priv, &p);
    uint32_t x[EC_LIMBS];
    if (!pt_affine(c, &r, x, NULL)) return false;
    bn_to_bytes(shared, (size_t)c->size, x, c->n);
    secure_zero(x, sizeof x);
    return true;
}

/* DER INTEGER -> limbs (must be 0 < v and fit) */
static bool der_int(const uint8_t **pp, const uint8_t *end, uint32_t *v, int n) {
    const uint8_t *p = *pp;
    if (end - p < 2 || p[0] != 0x02) return false;
    size_t len = p[1];
    p += 2;
    if (len & 0x80) return false;
    if ((size_t)(end - p) < len || !len || (p[0] & 0x80)) return false;
    if (!bn_from_bytes(v, n, p, len)) return false;
    *pp = p + len;
    return true;
}

bool ecdsa_verify(int curve, const uint8_t *pub, size_t pub_len, const uint8_t *hash, size_t hlen,
                  const uint8_t *sig, size_t sig_len) {
    const curve_t *c = get_curve(curve);
    if (!c || sig_len < 8) return false;
    /* SEQUENCE { INTEGER r, INTEGER s } (short or one-byte long form length) */
    const uint8_t *p = sig, *end = sig + sig_len;
    if (p[0] != 0x30) return false;
    size_t sl = p[1];
    p += 2;
    if (sl == 0x81) { sl = *p++; }
    else if (sl & 0x80) return false;
    if ((size_t)(end - p) != sl) return false;
    uint32_t r[EC_LIMBS], s[EC_LIMBS], e[EC_LIMBS], w[EC_LIMBS], u1[EC_LIMBS], u2[EC_LIMBS], t[EC_LIMBS];
    if (!der_int(&p, end, r, c->n) || !der_int(&p, end, s, c->n) || p != end) return false;
    if (bn_is_zero(r, c->n) || bn_is_zero(s, c->n) || bn_cmp(r, c->q, c->n) >= 0 || bn_cmp(s, c->q, c->n) >= 0)
        return false;
    pt_t q;
    if (!pt_load(c, &q, pub, pub_len)) return false;
    /* e = leftmost bits of the hash */
    bn_from_bytes(e, c->n, hash, hlen > (size_t)c->size ? (size_t)c->size : hlen);
    if (bn_cmp(e, c->q, c->n) >= 0) bn_sub(e, e, c->q, c->n);
    /* w = s^-1 mod q; u1 = e w, u2 = r w (Montgomery arithmetic mod q) */
    uint32_t one_q[EC_LIMBS], plain1[EC_LIMBS] = { 1 };
    bn_mont_mul(one_q, plain1, c->qr2, c->q, c->q0inv, c->n);
    bn_mont_mul(t, s, c->qr2, c->q, c->q0inv, c->n);                 /* s in Montgomery form */
    mpow(w, t, c->qm2, one_q, c->q, c->q0inv, c->n);                 /* w (Montgomery form) */
    bn_mont_mul(u1, e, w, c->q, c->q0inv, c->n);                     /* e w (plain) */
    bn_mont_mul(u2, r, w, c->q, c->q0inv, c->n);                     /* r w (plain) */
    /* X = u1 G + u2 Q (Shamir's trick) */
    pt_t g, gq, acc;
    memcpy(g.x, c->gx, sizeof g.x);
    memcpy(g.y, c->gy, sizeof g.y);
    memcpy(g.z, c->one, sizeof g.z);
    pt_add(c, &gq, &g, &q);
    memset(&acc, 0, sizeof acc);
    for (int i = c->n * 32 - 1; i >= 0; i--) {
        pt_double(c, &acc, &acc);
        int b1 = (u1[i / 32] >> (i % 32)) & 1, b2 = (u2[i / 32] >> (i % 32)) & 1;
        if (b1 && b2) pt_add(c, &acc, &acc, &gq);
        else if (b1) pt_add(c, &acc, &acc, &g);
        else if (b2) pt_add(c, &acc, &acc, &q);
    }
    uint32_t x[EC_LIMBS];
    if (!pt_affine(c, &acc, x, NULL)) return false;
    if (bn_cmp(x, c->q, c->n) >= 0) bn_sub(x, x, c->q, c->n);
    return bn_cmp(x, r, c->n) == 0;
}
