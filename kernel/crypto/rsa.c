/* rsa.c - RSA signature verification: PKCS #1 v1.5 and PSS (RFC 8017 8.2, 9.1). */
#include "kernel.h"
#include "crypto.h"

/* s^e mod n as k bytes (k = byte length of n). false on bad sizes / s >= n. */
static bool rsa_public(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                       const uint8_t *sig, size_t slen, uint8_t *em, size_t *k_out) {
    while (nlen && !*n) { n++; nlen--; }
    while (elen && !*e) { e++; elen--; }
    if (nlen < 64 || nlen > BN_MAX_LIMBS * 4 || !elen || elen > 8 || !(n[nlen - 1] & 1)) return false;
    if (slen != nlen) {                     /* allow a signature with leading zero bytes trimmed / added */
        while (slen > nlen && !*sig) { sig++; slen--; }
        if (slen > nlen) return false;
    }
    int limbs = (int)((nlen + 3) / 4);
    uint32_t *buf = kmalloc(sizeof(uint32_t) * (size_t)limbs * 3);
    if (!buf) return false;
    uint32_t *m = buf, *s = buf + limbs, *r = buf + 2 * limbs;
    bool ok = bn_from_bytes(m, limbs, n, nlen) && bn_from_bytes(s, limbs, sig, slen) && bn_cmp(s, m, limbs) < 0;
    if (ok) {
        bn_modexp(r, s, e, elen, m, limbs);
        bn_to_bytes(em, nlen, r, limbs);
        *k_out = nlen;
    }
    kfree(buf);
    return ok;
}

static const uint8_t DI_SHA1[] = { 0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14 };
static const uint8_t DI_SHA256[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01,
                                     0x05, 0x00, 0x04, 0x20 };
static const uint8_t DI_SHA384[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02,
                                     0x05, 0x00, 0x04, 0x30 };
static const uint8_t DI_SHA512[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03,
                                     0x05, 0x00, 0x04, 0x40 };

bool rsa_pkcs1_verify(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                      const hash_alg_t *h, const uint8_t *hash, const uint8_t *sig, size_t slen) {
    const uint8_t *di;
    size_t dil;
    if (h == &HASH_SHA256) { di = DI_SHA256; dil = sizeof DI_SHA256; }
    else if (h == &HASH_SHA384) { di = DI_SHA384; dil = sizeof DI_SHA384; }
    else if (h == &HASH_SHA512) { di = DI_SHA512; dil = sizeof DI_SHA512; }
    else if (h == &HASH_SHA1) { di = DI_SHA1; dil = sizeof DI_SHA1; }
    else return false;
    uint8_t *em = kmalloc(BN_MAX_LIMBS * 4 * 2);
    if (!em) return false;
    uint8_t *want = em + BN_MAX_LIMBS * 4;
    size_t k = 0;
    bool ok = rsa_public(n, nlen, e, elen, sig, slen, em, &k) && k >= dil + h->size + 11;
    if (ok) {
        /* EM = 00 01 FF..FF 00 DigestInfo hash */
        size_t tl = dil + h->size;
        want[0] = 0;
        want[1] = 1;
        memset(want + 2, 0xFF, k - tl - 3);
        want[k - tl - 1] = 0;
        memcpy(want + k - tl, di, dil);
        memcpy(want + k - h->size, hash, h->size);
        ok = ct_equal(em, want, k);
        if (!ok && di != DI_SHA1) {
            /* some encoders leave out the NULL parameters of the hash algorithm */
            size_t tl2 = tl - 2;
            memset(want + 2, 0xFF, k - tl2 - 3);
            want[k - tl2 - 1] = 0;
            memcpy(want + k - tl2, di, dil - 2);
            want[k - tl2] = (uint8_t)(di[1] - 2);
            want[k - tl2 + 3] = (uint8_t)(di[3] - 2);
            memcpy(want + k - tl2 + dil - 4, di + dil - 2, 2);
            memcpy(want + k - h->size, hash, h->size);
            ok = ct_equal(em, want, k);
        }
    }
    kfree(em);
    return ok;
}

/* MGF1 mask generation (RFC 8017 B.2.1), XORed into out */
static void mgf1_xor(const hash_alg_t *h, const uint8_t *seed, size_t slen, uint8_t *out, size_t len) {
    uint8_t blk[HASH_MAX_SIZE];
    for (uint32_t ctr = 0, done = 0; done < len; ctr++) {
        hash_ctx_t c;
        uint8_t cb[4] = { (uint8_t)(ctr >> 24), (uint8_t)(ctr >> 16), (uint8_t)(ctr >> 8), (uint8_t)ctr };
        h->init(&c);
        h->update(&c, seed, slen);
        h->update(&c, cb, 4);
        h->final(&c, blk);
        for (uint32_t i = 0; i < h->size && done < len; i++) out[done++] ^= blk[i];
    }
}

bool rsa_pss_verify(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                    const hash_alg_t *h, const uint8_t *hash, const uint8_t *sig, size_t slen) {
    uint8_t *em = kmalloc(BN_MAX_LIMBS * 4);
    if (!em) return false;
    size_t k = 0;
    bool ok = rsa_public(n, nlen, e, elen, sig, slen, em, &k);
    while (ok) {
        ok = false;
        while (nlen && !*n) { n++; nlen--; }
        int top = 7;
        while (top >= 0 && !((n[0] >> top) & 1)) top--;
        size_t mod_bits = (nlen - 1) * 8 + (size_t)top + 1;
        size_t em_bits = mod_bits - 1;
        size_t em_len = (em_bits + 7) / 8;
        const uint8_t *p = em;
        if (em_len < k) { if (em[0]) break; p++; }       /* modBits - 1 a multiple of 8 */
        size_t hl = h->size;
        if (em_len < hl + 2 || p[em_len - 1] != 0xBC) break;
        size_t dbl = em_len - hl - 1;
        uint8_t *db = (uint8_t *)p;                      /* unmask in place */
        const uint8_t *H = p + dbl;
        uint8_t topmask = (uint8_t)(0xFF >> (8 * em_len - em_bits));
        if (db[0] & ~topmask) break;
        mgf1_xor(h, H, hl, db, dbl);
        db[0] &= topmask;
        size_t i = 0;
        while (i < dbl && db[i] == 0) i++;
        if (i >= dbl || db[i] != 0x01) break;
        const uint8_t *salt = db + i + 1;
        size_t salt_len = dbl - i - 1;
        static const uint8_t zero8[8];
        hash_ctx_t c;
        uint8_t hh[HASH_MAX_SIZE];
        h->init(&c);
        h->update(&c, zero8, 8);
        h->update(&c, hash, hl);
        h->update(&c, salt, salt_len);
        h->final(&c, hh);
        ok = ct_equal(hh, H, hl);
        break;
    }
    kfree(em);
    return ok;
}
