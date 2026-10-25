/* kdf.c - HMAC (RFC 2104), HKDF (RFC 5869), the TLS 1.3 HKDF-Expand-Label (RFC 8446
   7.1) and the TLS 1.2 PRF (RFC 5246 5). */
#include "kernel.h"
#include "crypto.h"

void hmac_init(hmac_ctx_t *c, const hash_alg_t *h, const void *key, size_t klen) {
    uint8_t k[HASH_MAX_BLOCK], pad[HASH_MAX_BLOCK];
    memset(k, 0, sizeof k);
    if (klen > h->block) hash_buf(h, key, klen, k);
    else memcpy(k, key, klen);
    c->h = h;
    for (uint32_t i = 0; i < h->block; i++) pad[i] = k[i] ^ 0x36;
    h->init(&c->inner);
    h->update(&c->inner, pad, h->block);
    for (uint32_t i = 0; i < h->block; i++) pad[i] = k[i] ^ 0x5c;
    h->init(&c->outer);
    h->update(&c->outer, pad, h->block);
    secure_zero(k, sizeof k);
    secure_zero(pad, sizeof pad);
}

void hmac_update(hmac_ctx_t *c, const void *data, size_t n) { c->h->update(&c->inner, data, n); }

void hmac_final(hmac_ctx_t *c, uint8_t *out) {
    uint8_t ih[HASH_MAX_SIZE];
    c->h->final(&c->inner, ih);
    c->h->update(&c->outer, ih, c->h->size);
    c->h->final(&c->outer, out);
    secure_zero(ih, sizeof ih);
}

void hmac(const hash_alg_t *h, const void *key, size_t klen, const void *msg, size_t mlen, uint8_t *out) {
    hmac_ctx_t c;
    hmac_init(&c, h, key, klen);
    hmac_update(&c, msg, mlen);
    hmac_final(&c, out);
    secure_zero(&c, sizeof c);
}

void hkdf_extract(const hash_alg_t *h, const void *salt, size_t slen, const void *ikm, size_t ilen, uint8_t *prk) {
    static const uint8_t zero[HASH_MAX_SIZE];
    if (!salt || !slen) { salt = zero; slen = h->size; }
    hmac(h, salt, slen, ikm, ilen, prk);
}

void hkdf_expand(const hash_alg_t *h, const uint8_t *prk, const void *info, size_t ilen, uint8_t *out, size_t olen) {
    uint8_t t[HASH_MAX_SIZE];
    size_t tlen = 0, done = 0;
    for (uint8_t ctr = 1; done < olen; ctr++) {
        hmac_ctx_t c;
        hmac_init(&c, h, prk, h->size);
        hmac_update(&c, t, tlen);
        hmac_update(&c, info, ilen);
        hmac_update(&c, &ctr, 1);
        hmac_final(&c, t);
        tlen = h->size;
        size_t k = MIN(olen - done, tlen);
        memcpy(out + done, t, k);
        done += k;
    }
    secure_zero(t, sizeof t);
}

void hkdf_expand_label(const hash_alg_t *h, const uint8_t *secret, const char *label,
                       const void *ctx, size_t clen, uint8_t *out, size_t olen) {
    uint8_t info[2 + 1 + 255 + 1 + 255];
    size_t ll = strlen(label);
    size_t o = 0;
    info[o++] = (uint8_t)(olen >> 8);
    info[o++] = (uint8_t)olen;
    info[o++] = (uint8_t)(6 + ll);
    memcpy(info + o, "tls13 ", 6);
    o += 6;
    memcpy(info + o, label, ll);
    o += ll;
    info[o++] = (uint8_t)clen;
    if (clen) memcpy(info + o, ctx, clen);
    o += clen;
    hkdf_expand(h, secret, info, o, out, olen);
}

void tls12_prf(const hash_alg_t *h, const void *secret, size_t slen, const char *label,
               const void *seed, size_t seedlen, uint8_t *out, size_t olen) {
    uint8_t a[HASH_MAX_SIZE], blk[HASH_MAX_SIZE];
    size_t ll = strlen(label);
    hmac_ctx_t c;
    /* A(1) = HMAC(secret, label + seed) */
    hmac_init(&c, h, secret, slen);
    hmac_update(&c, label, ll);
    hmac_update(&c, seed, seedlen);
    hmac_final(&c, a);
    size_t done = 0;
    while (done < olen) {
        hmac_init(&c, h, secret, slen);
        hmac_update(&c, a, h->size);
        hmac_update(&c, label, ll);
        hmac_update(&c, seed, seedlen);
        hmac_final(&c, blk);
        size_t k = MIN(olen - done, (size_t)h->size);
        memcpy(out + done, blk, k);
        done += k;
        hmac(h, secret, slen, a, h->size, a);     /* A(i+1) = HMAC(secret, A(i)) */
    }
    secure_zero(a, sizeof a);
    secure_zero(blk, sizeof blk);
    secure_zero(&c, sizeof c);
}
