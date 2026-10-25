/*
 * crypto.h - cryptography for the network stack (TLS): hashes, HMAC/HKDF, AES-GCM,
 * ChaCha20-Poly1305, X25519, P-256/P-384 ECDH + ECDSA, RSA signature checks, a
 * secure random number generator. Internal kernel interface.
 *
 * All code is constant-time where it handles secrets (keys, plaintext): no
 * secret-dependent branches or table indexes, except the AES tables (cache timing
 * is not a concern inside a single-user VM).
 */
#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- secure random numbers (random.c) ---------------------------------------------- */
void     random_bytes(void *out, size_t n);
uint32_t random_u32(void);
/* mix in event timings / data (cheap; may be called from hot paths, not from IRQs) */
void     random_add_entropy(const void *data, size_t n);
void     random_add_timing(void);          /* the current TSC */
const char *random_source(void);           /* "RDSEED", "RDRAND" or "TSC jitter" */

/* ---- hashes (sha2.c) -------------------------------------------------------------------- */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t n; } sha256_ctx_t;
typedef struct { uint64_t h[8]; uint64_t len; uint8_t buf[128]; uint32_t n; uint32_t outlen; } sha512_ctx_t;
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; uint32_t n; } sha1_ctx_t;

void sha256_init(sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, size_t n);
void sha256_final(sha256_ctx_t *c, uint8_t out[32]);
void sha256(const void *data, size_t n, uint8_t out[32]);
void sha384_init(sha512_ctx_t *c);
void sha512_init(sha512_ctx_t *c);
void sha512_update(sha512_ctx_t *c, const void *data, size_t n);
void sha512_final(sha512_ctx_t *c, uint8_t *out);     /* 48 bytes for SHA-384, 64 for SHA-512 */
void sha384(const void *data, size_t n, uint8_t out[48]);
void sha512(const void *data, size_t n, uint8_t out[64]);
void sha1_init(sha1_ctx_t *c);
void sha1_update(sha1_ctx_t *c, const void *data, size_t n);
void sha1_final(sha1_ctx_t *c, uint8_t out[20]);

/* Generic hash interface (TLS transcript, HMAC, HKDF, signatures). */
#define HASH_MAX_SIZE  64
#define HASH_MAX_BLOCK 128
typedef union { sha256_ctx_t s256; sha512_ctx_t s512; sha1_ctx_t s1; } hash_ctx_t;
typedef struct hash_alg {
    const char *name;
    uint32_t size, block;
    void (*init)(hash_ctx_t *c);
    void (*update)(hash_ctx_t *c, const void *data, size_t n);
    void (*final)(hash_ctx_t *c, uint8_t *out);
} hash_alg_t;
extern const hash_alg_t HASH_SHA1, HASH_SHA256, HASH_SHA384, HASH_SHA512;
void hash_buf(const hash_alg_t *h, const void *data, size_t n, uint8_t *out);

/* ---- HMAC / HKDF / TLS 1.2 PRF (kdf.c) ---------------------------------------------- */
typedef struct { const hash_alg_t *h; hash_ctx_t inner, outer; } hmac_ctx_t;
void hmac_init(hmac_ctx_t *c, const hash_alg_t *h, const void *key, size_t klen);
void hmac_update(hmac_ctx_t *c, const void *data, size_t n);
void hmac_final(hmac_ctx_t *c, uint8_t *out);
void hmac(const hash_alg_t *h, const void *key, size_t klen, const void *msg, size_t mlen, uint8_t *out);
void hkdf_extract(const hash_alg_t *h, const void *salt, size_t slen, const void *ikm, size_t ilen, uint8_t *prk);
void hkdf_expand(const hash_alg_t *h, const uint8_t *prk, const void *info, size_t ilen, uint8_t *out, size_t olen);
/* TLS 1.3 HKDF-Expand-Label(secret, "tls13 " + label, context, length) */
void hkdf_expand_label(const hash_alg_t *h, const uint8_t *secret, const char *label,
                       const void *ctx, size_t clen, uint8_t *out, size_t olen);
/* TLS 1.2 PRF (P_hash with the suite's hash) */
void tls12_prf(const hash_alg_t *h, const void *secret, size_t slen, const char *label,
               const void *seed, size_t seedlen, uint8_t *out, size_t olen);

/* ---- AEAD ciphers (aes.c, chacha.c) ------------------------------------------------------ */
typedef struct { uint32_t rk[60]; int rounds; } aes_ctx_t;
void aes_set_key(aes_ctx_t *c, const uint8_t *key, int keylen);   /* 16 or 32 */
void aes_encrypt_block(const aes_ctx_t *c, const uint8_t in[16], uint8_t out[16]);

typedef struct { aes_ctx_t aes; uint64_t hl[16], hh[16]; } aes_gcm_ctx_t;   /* GHASH table: i * H */
void aes_gcm_init(aes_gcm_ctx_t *c, const uint8_t *key, int keylen);
/* in-place; tag = 16 bytes; nonce = 12 bytes */
void aes_gcm_encrypt(const aes_gcm_ctx_t *c, const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                     uint8_t *data, size_t len, uint8_t tag[16]);
bool aes_gcm_decrypt(const aes_gcm_ctx_t *c, const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                     uint8_t *data, size_t len, const uint8_t tag[16]);

void chacha20_block(const uint32_t key[8], uint32_t counter, const uint32_t nonce[3], uint8_t out[64]);
void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t *data, size_t len);
void poly1305_mac(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t tag[16]);
void chacha20poly1305_encrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                              uint8_t *data, size_t len, uint8_t tag[16]);
bool chacha20poly1305_decrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                              uint8_t *data, size_t len, const uint8_t tag[16]);

/* ---- key exchange / signatures (x25519.c, ec.c, rsa.c) ------------------------------------ */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_public(uint8_t pub[32], const uint8_t priv[32]);

/* NIST curves: 1 = P-256, 2 = P-384 */
#define EC_P256 1
#define EC_P384 2
int  ec_size(int curve);                                  /* 32 / 48 */
/* priv: random scalar (size bytes); pub: uncompressed point 04||X||Y (1 + 2*size) */
bool ec_keygen(int curve, uint8_t *priv, uint8_t *pub);
/* shared X coordinate (size bytes); false if the peer point is invalid */
bool ec_ecdh(int curve, const uint8_t *priv, const uint8_t *peer_pub, size_t peer_len, uint8_t *shared);
/* ECDSA check: hash already computed; sig = DER SEQUENCE { r, s } */
bool ecdsa_verify(int curve, const uint8_t *pub, size_t pub_len, const uint8_t *hash, size_t hlen,
                  const uint8_t *sig_der, size_t sig_len);

/* RSA public key operations (modulus n, exponent e, big endian) */
bool rsa_pkcs1_verify(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                      const hash_alg_t *h, const uint8_t *hash, const uint8_t *sig, size_t slen);
bool rsa_pss_verify(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                    const hash_alg_t *h, const uint8_t *hash, const uint8_t *sig, size_t slen);

/* ---- big integers (bignum.c): 32-bit limbs, least significant first ---------------------- */
#define BN_MAX_LIMBS 256                      /* 8192 bits */
int      bn_cmp(const uint32_t *a, const uint32_t *b, int n);
bool     bn_is_zero(const uint32_t *a, int n);
uint32_t bn_add(uint32_t *r, const uint32_t *a, const uint32_t *b, int n);   /* returns the carry  */
uint32_t bn_sub(uint32_t *r, const uint32_t *a, const uint32_t *b, int n);   /* returns the borrow */
bool     bn_from_bytes(uint32_t *r, int n, const uint8_t *b, size_t len);    /* big endian */
void     bn_to_bytes(uint8_t *b, size_t len, const uint32_t *a, int n);
uint32_t bn_m0inv(uint32_t m0);                                              /* -m^-1 mod 2^32 */
void     bn_mont_mul(uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *m, uint32_t m0inv, int n);
void     bn_r2(uint32_t *r, const uint32_t *m, int n);                       /* R^2 mod m */
void     bn_modexp(uint32_t *r, const uint32_t *base, const uint8_t *exp, size_t elen, const uint32_t *m, int n);

/* constant-time compare: true when equal */
bool ct_equal(const void *a, const void *b, size_t n);
void secure_zero(void *p, size_t n);

/* self tests with published test vectors; logs "SELFTEST: crypto ..." lines */
int  crypto_selftest(void (*report)(const char *name, bool ok));

#endif
