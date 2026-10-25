/*
 * aes.c - AES-128 / AES-256 encryption (FIPS 197) and AES-GCM (NIST SP 800-38D).
 *
 * Only the forward cipher is needed (GCM runs AES in counter mode). Table based
 * ("T-tables", built at first use); GHASH uses Shoup's 4-bit table method.
 */
#include "kernel.h"
#include "crypto.h"

static uint8_t SBOX[256];
static uint32_t TE0[256], TE1[256], TE2[256], TE3[256];
static bool tables_ready;

static inline uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

static void build_tables(void) {
    /* S-box: multiplicative inverse in GF(2^8) followed by the affine transform */
    uint8_t exp[256], log[256];
    uint8_t x = 1;
    for (int i = 0; i < 255; i++) {
        exp[i] = x;
        log[x] = (uint8_t)i;
        x = (uint8_t)(x ^ xtime(x));            /* multiply by the generator 3 */
    }
    for (int i = 0; i < 256; i++) {
        uint8_t inv = i ? exp[(255 - log[i]) % 255] : 0;
        uint8_t s = inv, r = inv;
        for (int k = 0; k < 4; k++) { r = (uint8_t)((r << 1) | (r >> 7)); s ^= r; }
        SBOX[i] = s ^ 0x63;
    }
    for (int i = 0; i < 256; i++) {
        uint8_t s = SBOX[i], s2 = xtime(s), s3 = s2 ^ s;
        uint32_t t = ((uint32_t)s2 << 24) | ((uint32_t)s << 16) | ((uint32_t)s << 8) | s3;
        TE0[i] = t;
        TE1[i] = ror32(t, 8);
        TE2[i] = ror32(t, 16);
        TE3[i] = ror32(t, 24);
    }
    tables_ready = true;
}

static uint32_t sub_word(uint32_t w) {
    return ((uint32_t)SBOX[w >> 24] << 24) | ((uint32_t)SBOX[(w >> 16) & 255] << 16) |
           ((uint32_t)SBOX[(w >> 8) & 255] << 8) | SBOX[w & 255];
}

void aes_set_key(aes_ctx_t *c, const uint8_t *key, int keylen) {
    if (!tables_ready) build_tables();
    int nk = keylen / 4;
    c->rounds = nk + 6;
    int total = 4 * (c->rounds + 1);
    for (int i = 0; i < nk; i++) c->rk[i] = get32(key + 4 * i);
    uint8_t rcon = 1;
    for (int i = nk; i < total; i++) {
        uint32_t t = c->rk[i - 1];
        if (i % nk == 0) {
            t = sub_word((t << 8) | (t >> 24)) ^ ((uint32_t)rcon << 24);
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            t = sub_word(t);
        }
        c->rk[i] = c->rk[i - nk] ^ t;
    }
}

void aes_encrypt_block(const aes_ctx_t *c, const uint8_t in[16], uint8_t out[16]) {
    const uint32_t *rk = c->rk;
    uint32_t s0 = get32(in) ^ rk[0], s1 = get32(in + 4) ^ rk[1];
    uint32_t s2 = get32(in + 8) ^ rk[2], s3 = get32(in + 12) ^ rk[3];
    uint32_t t0, t1, t2, t3;
    for (int r = 1; r < c->rounds; r++) {
        rk += 4;
        t0 = TE0[s0 >> 24] ^ TE1[(s1 >> 16) & 255] ^ TE2[(s2 >> 8) & 255] ^ TE3[s3 & 255] ^ rk[0];
        t1 = TE0[s1 >> 24] ^ TE1[(s2 >> 16) & 255] ^ TE2[(s3 >> 8) & 255] ^ TE3[s0 & 255] ^ rk[1];
        t2 = TE0[s2 >> 24] ^ TE1[(s3 >> 16) & 255] ^ TE2[(s0 >> 8) & 255] ^ TE3[s1 & 255] ^ rk[2];
        t3 = TE0[s3 >> 24] ^ TE1[(s0 >> 16) & 255] ^ TE2[(s1 >> 8) & 255] ^ TE3[s2 & 255] ^ rk[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    rk += 4;
    t0 = ((uint32_t)SBOX[s0 >> 24] << 24) ^ ((uint32_t)SBOX[(s1 >> 16) & 255] << 16) ^
         ((uint32_t)SBOX[(s2 >> 8) & 255] << 8) ^ SBOX[s3 & 255] ^ rk[0];
    t1 = ((uint32_t)SBOX[s1 >> 24] << 24) ^ ((uint32_t)SBOX[(s2 >> 16) & 255] << 16) ^
         ((uint32_t)SBOX[(s3 >> 8) & 255] << 8) ^ SBOX[s0 & 255] ^ rk[1];
    t2 = ((uint32_t)SBOX[s2 >> 24] << 24) ^ ((uint32_t)SBOX[(s3 >> 16) & 255] << 16) ^
         ((uint32_t)SBOX[(s0 >> 8) & 255] << 8) ^ SBOX[s1 & 255] ^ rk[2];
    t3 = ((uint32_t)SBOX[s3 >> 24] << 24) ^ ((uint32_t)SBOX[(s0 >> 16) & 255] << 16) ^
         ((uint32_t)SBOX[(s1 >> 8) & 255] << 8) ^ SBOX[s2 & 255] ^ rk[3];
    put32(out, t0);
    put32(out + 4, t1);
    put32(out + 8, t2);
    put32(out + 12, t3);
}

/* ---- GCM ---------------------------------------------------------------------------------- */
void aes_gcm_init(aes_gcm_ctx_t *c, const uint8_t *key, int keylen) {
    aes_set_key(&c->aes, key, keylen);
    uint8_t h[16] = { 0 };
    aes_encrypt_block(&c->aes, h, h);
    uint64_t vh = ((uint64_t)get32(h) << 32) | get32(h + 4);
    uint64_t vl = ((uint64_t)get32(h + 8) << 32) | get32(h + 12);
    /* table of i * H for the 16 4-bit values (bit-reflected order) */
    c->hl[8] = vl;
    c->hh[8] = vh;
    c->hl[0] = c->hh[0] = 0;
    for (int i = 4; i > 0; i >>= 1) {
        uint32_t t = (uint32_t)(vl & 1) * 0xe1000000u;
        vl = (vh << 63) | (vl >> 1);
        vh = (vh >> 1) ^ ((uint64_t)t << 32);
        c->hl[i] = vl;
        c->hh[i] = vh;
    }
    for (int i = 2; i <= 8; i *= 2)
        for (int j = 1; j < i; j++) {
            c->hh[i + j] = c->hh[i] ^ c->hh[j];
            c->hl[i + j] = c->hl[i] ^ c->hl[j];
        }
}

static const uint16_t LAST4[16] = { 0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
                                    0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0 };

/* y = y * H */
static void gmult(const aes_gcm_ctx_t *c, uint8_t y[16]) {
    uint8_t lo = y[15] & 15, hi, rem;
    uint64_t zh = c->hh[lo], zl = c->hl[lo];
    for (int i = 15; i >= 0; i--) {
        lo = y[i] & 15;
        hi = y[i] >> 4;
        if (i != 15) {
            rem = (uint8_t)(zl & 15);
            zl = (zh << 60) | (zl >> 4);
            zh = (zh >> 4) ^ ((uint64_t)LAST4[rem] << 48);
            zh ^= c->hh[lo];
            zl ^= c->hl[lo];
        }
        rem = (uint8_t)(zl & 15);
        zl = (zh << 60) | (zl >> 4);
        zh = (zh >> 4) ^ ((uint64_t)LAST4[rem] << 48);
        zh ^= c->hh[hi];
        zl ^= c->hl[hi];
    }
    put32(y, (uint32_t)(zh >> 32));
    put32(y + 4, (uint32_t)zh);
    put32(y + 8, (uint32_t)(zl >> 32));
    put32(y + 12, (uint32_t)zl);
}

static void ghash(const aes_gcm_ctx_t *c, uint8_t y[16], const uint8_t *data, size_t len) {
    while (len) {
        size_t k = len < 16 ? len : 16;
        for (size_t i = 0; i < k; i++) y[i] ^= data[i];
        gmult(c, y);
        data += k;
        len -= k;
    }
}

static void ctr_xor(const aes_gcm_ctx_t *c, const uint8_t j0[16], uint8_t *data, size_t len) {
    uint8_t ctr[16], ks[16];
    memcpy(ctr, j0, 16);
    uint32_t n = get32(ctr + 12);
    while (len) {
        put32(ctr + 12, ++n);
        aes_encrypt_block(&c->aes, ctr, ks);
        size_t k = len < 16 ? len : 16;
        for (size_t i = 0; i < k; i++) data[i] ^= ks[i];
        data += k;
        len -= k;
    }
    secure_zero(ks, sizeof ks);
}

static void gcm_tag(const aes_gcm_ctx_t *c, const uint8_t j0[16], const uint8_t *aad, size_t aadlen,
                    const uint8_t *ct, size_t len, uint8_t tag[16]) {
    uint8_t y[16] = { 0 }, lens[16], ek[16];
    ghash(c, y, aad, aadlen);
    ghash(c, y, ct, len);
    uint64_t abits = (uint64_t)aadlen * 8, cbits = (uint64_t)len * 8;
    put32(lens, (uint32_t)(abits >> 32));
    put32(lens + 4, (uint32_t)abits);
    put32(lens + 8, (uint32_t)(cbits >> 32));
    put32(lens + 12, (uint32_t)cbits);
    ghash(c, y, lens, 16);
    aes_encrypt_block(&c->aes, j0, ek);
    for (int i = 0; i < 16; i++) tag[i] = y[i] ^ ek[i];
}

void aes_gcm_encrypt(const aes_gcm_ctx_t *c, const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                     uint8_t *data, size_t len, uint8_t tag[16]) {
    uint8_t j0[16];
    memcpy(j0, nonce, 12);
    put32(j0 + 12, 1);
    ctr_xor(c, j0, data, len);
    gcm_tag(c, j0, aad, aadlen, data, len, tag);
}

bool aes_gcm_decrypt(const aes_gcm_ctx_t *c, const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                     uint8_t *data, size_t len, const uint8_t tag[16]) {
    uint8_t j0[16], t[16];
    memcpy(j0, nonce, 12);
    put32(j0 + 12, 1);
    gcm_tag(c, j0, aad, aadlen, data, len, t);
    if (!ct_equal(t, tag, 16)) return false;
    ctr_xor(c, j0, data, len);
    return true;
}
