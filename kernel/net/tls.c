/*
 * tls.c - TLS 1.3 (RFC 8446) and TLS 1.2 (RFC 5246) client.
 *
 *   Cipher suites  TLS 1.3: AES_128_GCM_SHA256, AES_256_GCM_SHA384, CHACHA20_POLY1305_SHA256
 *                  TLS 1.2: ECDHE_{ECDSA,RSA}_WITH_{AES_128_GCM_SHA256, AES_256_GCM_SHA384,
 *                           CHACHA20_POLY1305_SHA256}
 *   Key exchange   X25519 (offered first), P-256, P-384 (HelloRetryRequest / TLS 1.2)
 *   Signatures     RSA PKCS #1 v1.5 and PSS, ECDSA P-256 / P-384 (SHA-256/384/512)
 *   Certificates   chain to the built-in roots, dates, host name (x509.c)
 *   Extras         SNI, ALPN "http/1.1", extended master secret (TLS 1.2), downgrade
 *                  protection, middlebox compatibility mode, KeyUpdate. No resumption,
 *                  no renegotiation, no client certificates (an empty one is sent if asked).
 *
 * A connection is a state machine stepped from the network tick (tls_step, through
 * http.c). Expensive public-key operations (key generation, ECDH, each signature
 * check) are done one per step, so the main loop keeps running during a handshake.
 * Received records are decrypted on demand by tls_read, with a per-step budget.
 */
#include "net_internal.h"
#include "crypto/crypto.h"

#define MAX_PLAIN      16384
#define MAX_CIPHER     (MAX_PLAIN + 2048)
#define MAX_HS_MSG     (128 * 1024)
#define READ_BUDGET    (64 * 1024)          /* plaintext decrypted per step */
#define OUT_CAP        (2 * (MAX_PLAIN + 64))

enum { AEAD_AES128 = 1, AEAD_AES256, AEAD_CHACHA };

typedef struct {
    uint16_t id;
    const char *name;
    int aead;
    const hash_alg_t *h;
    bool tls13;
} suite_t;

static const suite_t SUITES[] = {
    { 0x1301, "AES_128_GCM_SHA256",                   AEAD_AES128, &HASH_SHA256, true },
    { 0x1302, "AES_256_GCM_SHA384",                   AEAD_AES256, &HASH_SHA384, true },
    { 0x1303, "CHACHA20_POLY1305_SHA256",             AEAD_CHACHA, &HASH_SHA256, true },
    { 0xC02B, "ECDHE_ECDSA_WITH_AES_128_GCM_SHA256",  AEAD_AES128, &HASH_SHA256, false },
    { 0xC02F, "ECDHE_RSA_WITH_AES_128_GCM_SHA256",    AEAD_AES128, &HASH_SHA256, false },
    { 0xC02C, "ECDHE_ECDSA_WITH_AES_256_GCM_SHA384",  AEAD_AES256, &HASH_SHA384, false },
    { 0xC030, "ECDHE_RSA_WITH_AES_256_GCM_SHA384",    AEAD_AES256, &HASH_SHA384, false },
    { 0xCCA9, "ECDHE_ECDSA_WITH_CHACHA20_POLY1305",   AEAD_CHACHA, &HASH_SHA256, false },
    { 0xCCA8, "ECDHE_RSA_WITH_CHACHA20_POLY1305",     AEAD_CHACHA, &HASH_SHA256, false },
};

#define GROUP_X25519 0x001D
#define GROUP_P256   0x0017
#define GROUP_P384   0x0018

/* record content types / handshake message types */
enum { CT_CCS = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APP = 23 };
enum { HS_HELLO_REQUEST = 0, HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_TICKET = 4, HS_EOED = 5,
       HS_ENC_EXT = 8, HS_CERTIFICATE = 11, HS_SKE = 12, HS_CERT_REQ = 13, HS_SHD = 14, HS_CERT_VERIFY = 15,
       HS_CKE = 16, HS_FINISHED = 20, HS_KEY_UPDATE = 24, HS_MESSAGE_HASH = 254 };

/* handshake progress */
enum { H_CONNECT, H_KEYGEN, H_WAIT_SH, H_WAIT_EE, H_WAIT_CERT_CR, H_WAIT_CERT, H_WAIT_CV, H_WAIT_FIN,
       H12_WAIT_CERT, H12_WAIT_SKE, H12_WAIT_SHD, H12_SEND, H12_WAIT_CCS, H12_WAIT_FIN, H_DONE };

typedef struct {
    bool on;
    int aead;
    aes_gcm_ctx_t *gcm;
    uint8_t key[32];
    uint8_t iv[12];
    uint64_t seq;
} prot_t;

struct tls_conn {
    tcp_conn_t *tcp;
    char     host[256];
    uint32_t flags;
    int      state, err, alert;
    char     errtext[160];
    int      hs, sub;
    uint16_t version;
    const suite_t *cs;
    const hash_alg_t *h;
    uint8_t  client_random[32], server_random[32], session_id[32];
    /* key exchange */
    uint16_t group;
    uint8_t  x_priv[32], x_pub[32];
    uint8_t  ec_priv[48], ec_pub[97];
    uint8_t  peer_pub[133];
    uint32_t peer_pub_len;
    uint8_t  shared[48];
    uint32_t shared_len;
    bool     hrr;
    uint8_t *cookie;
    uint32_t cookie_len;
    /* transcript: both hashes until the suite is known */
    hash_ctx_t th256, th384;
    /* secrets */
    uint8_t  hs_secret[48], c_hs[48], s_hs[48], c_ap[48], s_ap[48], master[48];
    bool     ems;
    uint16_t ske_sigalg;
    prot_t   rd, wr;
    /* input */
    uint8_t *rec;                 /* one record being received: header + body */
    uint32_t rec_have;
    uint8_t *hsbuf;               /* handshake bytes received, not yet processed */
    uint32_t hs_len, hs_cap;
    uint8_t *app;                 /* decrypted application data */
    uint32_t app_off, app_len;
    uint32_t budget;
    bool     peer_closed;
    bool     heavy;               /* an expensive operation was done: end this step */
    /* output (records waiting for room in the TCP send buffer) */
    uint8_t *out;
    uint32_t out_len;
    /* certificates */
    uint8_t *certmsg;
    x509_chain_t *chain;
    bool     cert_requested;
    uint8_t  cert_req_ctx[256];
    uint32_t cert_req_ctx_len;
    bool     cert_ok;
    char     cert_error[160], cert_subject[128], cert_issuer[128];
    char     info[96];
};

/* ---- small helpers ----------------------------------------------------------------------------- */
static inline uint32_t rd24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static inline void wr24(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }

static const char *alert_name(int a) {
    switch (a) {
    case 0: return "close notify";
    case 10: return "unexpected message";
    case 20: return "bad record MAC";
    case 22: return "record overflow";
    case 40: return "handshake failure";
    case 42: return "bad certificate";
    case 43: return "unsupported certificate";
    case 44: return "certificate revoked";
    case 45: return "certificate expired";
    case 46: return "certificate unknown";
    case 47: return "illegal parameter";
    case 48: return "unknown CA";
    case 49: return "access denied";
    case 50: return "decode error";
    case 51: return "decrypt error";
    case 70: return "protocol version";
    case 71: return "insufficient security";
    case 80: return "internal error";
    case 86: return "inappropriate fallback";
    case 90: return "user canceled";
    case 109: return "missing extension";
    case 110: return "unsupported extension";
    case 112: return "unrecognized name";
    case 116: return "certificate required";
    case 120: return "no application protocol";
    default: return "alert";
    }
}

static void send_alert(tls_conn_t *t, int level, int desc);

static void fail(tls_conn_t *t, int err, int alert, const char *fmt, ...) PRINTF_LIKE(4, 5);
static void fail(tls_conn_t *t, int err, int alert, const char *fmt, ...) {
    if (t->state == TLSS_ERROR) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->errtext, sizeof t->errtext, fmt, ap);
    va_end(ap);
    if (alert >= 0) send_alert(t, 2, alert);
    t->err = err;
    t->state = TLSS_ERROR;
    NWARN("TLS %s: %s", t->host, t->errtext);
}

static void th_add(tls_conn_t *t, const uint8_t *msg, uint32_t len) {
    if (!t->h || t->h == &HASH_SHA256) HASH_SHA256.update(&t->th256, msg, len);
    if (!t->h || t->h == &HASH_SHA384) HASH_SHA384.update(&t->th384, msg, len);
}

static void th_hash(tls_conn_t *t, uint8_t *out) {
    hash_ctx_t c = t->h == &HASH_SHA384 ? t->th384 : t->th256;
    t->h->final(&c, out);
}

/* ---- record protection --------------------------------------------------------------------------- */
static void prot_set(prot_t *p, int aead, const uint8_t *key, int keylen, const uint8_t *iv, int ivlen) {
    p->aead = aead;
    memcpy(p->key, key, (size_t)keylen);
    memset(p->iv, 0, sizeof p->iv);
    memcpy(p->iv, iv, (size_t)ivlen);
    if (aead != AEAD_CHACHA) {
        if (!p->gcm) p->gcm = kmalloc(sizeof(aes_gcm_ctx_t));
        if (p->gcm) aes_gcm_init(p->gcm, key, keylen);
    }
    p->seq = 0;
    p->on = true;
}

static void prot_free(prot_t *p) {
    if (p->gcm) { secure_zero(p->gcm, sizeof *p->gcm); kfree(p->gcm); p->gcm = NULL; }
    secure_zero(p->key, sizeof p->key);
    p->on = false;
}

static int keylen_of(int aead) { return aead == AEAD_AES128 ? 16 : 32; }

/* TLS 1.3: set read or write keys from a traffic secret */
static void set_traffic_keys(tls_conn_t *t, prot_t *p, const uint8_t *secret) {
    uint8_t key[32], iv[12];
    int kl = keylen_of(t->cs->aead);
    hkdf_expand_label(t->h, secret, "key", NULL, 0, key, (size_t)kl);
    hkdf_expand_label(t->h, secret, "iv", NULL, 0, iv, 12);
    prot_set(p, t->cs->aead, key, kl, iv, 12);
    secure_zero(key, sizeof key);
}

static void make_nonce(const prot_t *p, uint8_t nonce[12]) {
    memcpy(nonce, p->iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(p->seq >> (8 * i));
}

static bool aead_seal(prot_t *p, uint8_t *nonce, const uint8_t *aad, uint32_t aadlen, uint8_t *data, uint32_t len, uint8_t *tag) {
    if (p->aead == AEAD_CHACHA) chacha20poly1305_encrypt(p->key, nonce, aad, aadlen, data, len, tag);
    else if (p->gcm) aes_gcm_encrypt(p->gcm, nonce, aad, aadlen, data, len, tag);
    else return false;
    return true;
}

static bool aead_open(prot_t *p, uint8_t *nonce, const uint8_t *aad, uint32_t aadlen, uint8_t *data, uint32_t len, const uint8_t *tag) {
    if (p->aead == AEAD_CHACHA) return chacha20poly1305_decrypt(p->key, nonce, aad, aadlen, data, len, tag);
    return p->gcm && aes_gcm_decrypt(p->gcm, nonce, aad, aadlen, data, len, tag);
}

/* ---- output ------------------------------------------------------------------------------------------ */
static void flush_out(tls_conn_t *t) {
    while (t->out_len) {
        uint32_t n = tcp_write(t->tcp, t->out, t->out_len);
        if (!n) break;
        memmove(t->out, t->out + n, t->out_len - n);
        t->out_len -= n;
    }
}

/* Append one record (encrypting it when write protection is on). */
static bool put_record(tls_conn_t *t, int type, const uint8_t *data, uint32_t len) {
    if (len > MAX_PLAIN || t->out_len + len + 64 > OUT_CAP) return false;
    uint8_t *r = t->out + t->out_len;
    uint16_t ver = (type == CT_HANDSHAKE && t->hs <= H_KEYGEN) ? 0x0301 : 0x0303;
    r[0] = (uint8_t)type;
    wr16be(r + 1, ver);
    if (!t->wr.on || type == CT_CCS) {
        wr16be(r + 3, (uint16_t)len);
        memcpy(r + 5, data, len);
        t->out_len += 5 + len;
    } else if (t->version == 0x0304) {
        /* inner plaintext: data || type, outer type application_data */
        uint32_t clen = len + 1 + 16;
        r[0] = CT_APP;
        wr16be(r + 3, (uint16_t)clen);
        memcpy(r + 5, data, len);
        r[5 + len] = (uint8_t)type;
        uint8_t nonce[12];
        make_nonce(&t->wr, nonce);
        aead_seal(&t->wr, nonce, r, 5, r + 5, len + 1, r + 5 + len + 1);
        t->wr.seq++;
        t->out_len += 5 + clen;
    } else {
        /* TLS 1.2: AAD = seq || type || version || length */
        uint8_t aad[13], nonce[12];
        for (int i = 0; i < 8; i++) aad[i] = (uint8_t)(t->wr.seq >> (56 - 8 * i));
        aad[8] = (uint8_t)type;
        wr16be(aad + 9, 0x0303);
        wr16be(aad + 11, (uint16_t)len);
        uint8_t *body = r + 5;
        uint32_t explicit_len = 0;
        if (t->wr.aead == AEAD_CHACHA) {
            make_nonce(&t->wr, nonce);
        } else {
            memcpy(nonce, t->wr.iv, 4);
            memcpy(nonce + 4, aad, 8);                   /* explicit nonce = sequence number */
            memcpy(body, aad, 8);
            explicit_len = 8;
        }
        memcpy(body + explicit_len, data, len);
        aead_seal(&t->wr, nonce, aad, 13, body + explicit_len, len, body + explicit_len + len);
        wr16be(r + 3, (uint16_t)(explicit_len + len + 16));
        t->wr.seq++;
        t->out_len += 5 + explicit_len + len + 16;
    }
    return true;
}

static void send_alert(tls_conn_t *t, int level, int desc) {
    if (!t->tcp || !t->out) return;
    uint8_t a[2] = { (uint8_t)level, (uint8_t)desc };
    put_record(t, CT_ALERT, a, 2);
    flush_out(t);
}

/* Send a handshake message (header added here) and add it to the transcript. */
static bool send_hs(tls_conn_t *t, int type, const uint8_t *body, uint32_t len) {
    uint8_t *m = kmalloc(4 + len);
    if (!m) return false;
    m[0] = (uint8_t)type;
    wr24(m + 1, len);
    memcpy(m + 4, body, len);
    th_add(t, m, 4 + len);
    bool ok = true;
    for (uint32_t off = 0; off < 4 + len && ok; off += MAX_PLAIN)
        ok = put_record(t, CT_HANDSHAKE, m + off, MIN((uint32_t)MAX_PLAIN, 4 + len - off));
    kfree(m);
    flush_out(t);
    return ok;
}

/* ---- ClientHello --------------------------------------------------------------------------------------- */
static bool keygen(tls_conn_t *t) {
    if (t->group == GROUP_X25519) {
        random_bytes(t->x_priv, 32);
        x25519_public(t->x_pub, t->x_priv);
        return true;
    }
    return ec_keygen(t->group == GROUP_P256 ? EC_P256 : EC_P384, t->ec_priv, t->ec_pub);
}

static uint32_t key_share_len(tls_conn_t *t) {
    return t->group == GROUP_X25519 ? 32 : t->group == GROUP_P256 ? 65 : 97;
}

static bool send_client_hello(tls_conn_t *t) {
    uint8_t *b = kmalloc(2048 + t->cookie_len);
    if (!b) return false;
    uint32_t o = 0;
    wr16be(b + o, 0x0303); o += 2;
    memcpy(b + o, t->client_random, 32); o += 32;
    b[o++] = 32;
    memcpy(b + o, t->session_id, 32); o += 32;
    wr16be(b + o, (uint16_t)(2 * (ARRAY_SIZE(SUITES) + 1))); o += 2;
    for (int i = 0; i < ARRAY_SIZE(SUITES); i++) { wr16be(b + o, SUITES[i].id); o += 2; }
    wr16be(b + o, 0x00FF); o += 2;                                 /* renegotiation SCSV */
    b[o++] = 1;
    b[o++] = 0;                                                    /* null compression */
    uint32_t ext_len_at = o;
    o += 2;
    uint32_t ip;
    if (!net_parse_ipv4(t->host, &ip)) {                           /* server_name */
        uint32_t hl = (uint32_t)strlen(t->host);
        wr16be(b + o, 0); wr16be(b + o + 2, (uint16_t)(hl + 5)); wr16be(b + o + 4, (uint16_t)(hl + 3));
        b[o + 6] = 0;
        wr16be(b + o + 7, (uint16_t)hl);
        memcpy(b + o + 9, t->host, hl);
        o += 9 + hl;
    }
    static const uint8_t fixed[] = {
        0x00, 0x0a, 0x00, 0x08, 0x00, 0x06, 0x00, 0x1d, 0x00, 0x17, 0x00, 0x18,      /* groups */
        0x00, 0x0b, 0x00, 0x02, 0x01, 0x00,                                          /* EC point formats */
        0x00, 0x0d, 0x00, 0x12, 0x00, 0x10, 0x04, 0x03, 0x05, 0x03, 0x08, 0x04, 0x08, 0x05, 0x08, 0x06,
                                            0x04, 0x01, 0x05, 0x01, 0x06, 0x01,      /* signature algorithms */
        0x00, 0x10, 0x00, 0x0b, 0x00, 0x09, 0x08, 'h', 't', 't', 'p', '/', '1', '.', '1',   /* ALPN */
        0x00, 0x17, 0x00, 0x00,                                                      /* extended master secret */
        0x00, 0x2b, 0x00, 0x05, 0x04, 0x03, 0x04, 0x03, 0x03,                        /* versions 1.3, 1.2 */
    };
    memcpy(b + o, fixed, sizeof fixed);
    o += sizeof fixed;
    uint32_t kl = key_share_len(t);                                /* key_share */
    wr16be(b + o, 0x0033); wr16be(b + o + 2, (uint16_t)(kl + 6)); wr16be(b + o + 4, (uint16_t)(kl + 4));
    wr16be(b + o + 6, t->group); wr16be(b + o + 8, (uint16_t)kl);
    memcpy(b + o + 10, t->group == GROUP_X25519 ? t->x_pub : t->ec_pub, kl);
    o += 10 + kl;
    if (t->cookie) {
        wr16be(b + o, 0x002c); wr16be(b + o + 2, (uint16_t)(t->cookie_len + 2)); wr16be(b + o + 4, (uint16_t)t->cookie_len);
        memcpy(b + o + 6, t->cookie, t->cookie_len);
        o += 6 + t->cookie_len;
    }
    wr16be(b + ext_len_at, (uint16_t)(o - ext_len_at - 2));
    bool ok = send_hs(t, HS_CLIENT_HELLO, b, o);
    kfree(b);
    return ok;
}

/* ---- TLS 1.3 key schedule ----------------------------------------------------------------------------- */
static void derive_secret(tls_conn_t *t, const uint8_t *secret, const char *label, const uint8_t *thash, uint8_t *out) {
    hkdf_expand_label(t->h, secret, label, thash, t->h->size, out, t->h->size);
}

static void derive_handshake_keys(tls_conn_t *t) {
    const hash_alg_t *h = t->h;
    uint8_t zero[48] = { 0 }, early[48], derived[48], empty_hash[48], th[48];
    hkdf_extract(h, NULL, 0, zero, h->size, early);
    hash_buf(h, NULL, 0, empty_hash);
    derive_secret(t, early, "derived", empty_hash, derived);
    hkdf_extract(h, derived, h->size, t->shared, t->shared_len, t->hs_secret);
    th_hash(t, th);
    derive_secret(t, t->hs_secret, "c hs traffic", th, t->c_hs);
    derive_secret(t, t->hs_secret, "s hs traffic", th, t->s_hs);
    set_traffic_keys(t, &t->rd, t->s_hs);
    secure_zero(early, sizeof early);
    secure_zero(t->shared, sizeof t->shared);
}

static void derive_app_secrets(tls_conn_t *t) {
    const hash_alg_t *h = t->h;
    uint8_t zero[48] = { 0 }, derived[48], empty_hash[48], th[48];
    hash_buf(h, NULL, 0, empty_hash);
    derive_secret(t, t->hs_secret, "derived", empty_hash, derived);
    hkdf_extract(h, derived, h->size, zero, h->size, t->master);
    th_hash(t, th);
    derive_secret(t, t->master, "c ap traffic", th, t->c_ap);
    derive_secret(t, t->master, "s ap traffic", th, t->s_ap);
}

static void finished_mac(tls_conn_t *t, const uint8_t *base, uint8_t *out) {
    uint8_t fk[48], th[48];
    hkdf_expand_label(t->h, base, "finished", NULL, 0, fk, t->h->size);
    th_hash(t, th);
    hmac(t->h, fk, t->h->size, th, t->h->size, out);
    secure_zero(fk, sizeof fk);
}

/* ---- ECDH ---------------------------------------------------------------------------------------------- */
static bool compute_shared(tls_conn_t *t) {
    if (t->group == GROUP_X25519) {
        if (t->peer_pub_len != 32) return false;
        x25519(t->shared, t->x_priv, t->peer_pub);
        t->shared_len = 32;
        uint8_t z = 0;
        for (int i = 0; i < 32; i++) z |= t->shared[i];
        return z != 0;                                  /* all-zero result: a bad public key */
    }
    int curve = t->group == GROUP_P256 ? EC_P256 : EC_P384;
    t->shared_len = (uint32_t)ec_size(curve);
    return ec_ecdh(curve, t->ec_priv, t->peer_pub, t->peer_pub_len, t->shared);
}

/* ---- certificates ---------------------------------------------------------------------------------------- */
/* Parse a Certificate message body into t->chain (TLS 1.3 has a context and per-entry extensions). */
static int parse_certificates(tls_conn_t *t, const uint8_t *body, uint32_t len) {
    kfree(t->certmsg);
    t->certmsg = kmalloc(len ? len : 1);
    if (!t->chain) t->chain = kmalloc(sizeof(x509_chain_t));
    if (!t->certmsg || !t->chain) return -1;
    memcpy(t->certmsg, body, len);
    const uint8_t *p = t->certmsg, *end = p + len;
    if (t->version == 0x0304) {
        if (end - p < 1 || (uint32_t)(end - p) < 1u + p[0]) return -1;
        p += 1 + p[0];
    }
    if (end - p < 3 || rd24(p) != (uint32_t)(end - p - 3)) return -1;
    p += 3;
    x509_chain_t *v = t->chain;
    v->n = 0;
    while (p < end) {
        if (end - p < 3) return -1;
        uint32_t cl = rd24(p);
        p += 3;
        if ((uint32_t)(end - p) < cl) return -1;
        if (v->n < X509_MAX_CHAIN) {
            if (!x509_parse(&v->certs[v->n], p, cl)) {
                if (v->n == 0) return -2;
            } else v->n++;
        }
        p += cl;
        if (t->version == 0x0304) {
            if (end - p < 2 || (uint32_t)(end - p) < 2u + rd16be(p)) return -1;
            p += 2 + rd16be(p);
        }
    }
    if (!v->n) return -3;
    x509_chain_start(v, t->host, (int64_t)sys_time());
    return 0;
}

/* One verification step. Returns 1 while busy, 0 when decided (continue), -1 failed (connection ends). */
static int verify_step(tls_conn_t *t) {
    int r = x509_chain_step(t->chain);
    if (r == X509_PENDING) return 1;
    strlcpy(t->cert_subject, t->chain->subject, sizeof t->cert_subject);
    strlcpy(t->cert_issuer, t->chain->issuer, sizeof t->cert_issuer);
    if (r == X509_OK) {
        t->cert_ok = true;
        t->cert_error[0] = 0;
        return 0;
    }
    t->cert_ok = false;
    strlcpy(t->cert_error, t->chain->error, sizeof t->cert_error);
    if (t->flags & TLS_F_ALLOW_BAD_CERT) {
        NWARN("TLS %s: certificate not trusted (%s), continuing as asked", t->host, t->cert_error);
        return 0;
    }
    int alert = r == X509_EDATE ? 45 : r == X509_EUNTRUSTED ? 48 : 42;
    fail(t, TLS_ECERT, alert, "%s", t->cert_error);
    return -1;
}

/* TLS SignatureScheme -> X509_SIG_* and hash */
static bool scheme_info(uint16_t s, int *alg, const hash_alg_t **h, bool tls13) {
    switch (s) {
    case 0x0403: *alg = X509_SIG_ECDSA; *h = &HASH_SHA256; return true;
    case 0x0503: *alg = X509_SIG_ECDSA; *h = &HASH_SHA384; return true;
    case 0x0603: *alg = X509_SIG_ECDSA; *h = &HASH_SHA512; return true;
    case 0x0804: *alg = X509_SIG_RSA_PSS; *h = &HASH_SHA256; return true;
    case 0x0805: *alg = X509_SIG_RSA_PSS; *h = &HASH_SHA384; return true;
    case 0x0806: *alg = X509_SIG_RSA_PSS; *h = &HASH_SHA512; return true;
    case 0x0401: *alg = X509_SIG_RSA_PKCS1; *h = &HASH_SHA256; return !tls13;
    case 0x0501: *alg = X509_SIG_RSA_PKCS1; *h = &HASH_SHA384; return !tls13;
    case 0x0601: *alg = X509_SIG_RSA_PKCS1; *h = &HASH_SHA512; return !tls13;
    }
    return false;
}

/* ---- handshake message handlers ---------------------------------------------------------------------
 * Called with one complete message (type, body). Return 0 when the message is fully
 * processed, 1 to be called again with the same message on the next step (an
 * expensive operation was done; t->sub counts the sub steps), -1 on failure. */

static const uint8_t HRR_RANDOM[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C };

static int on_server_hello(tls_conn_t *t, const uint8_t *msg, const uint8_t *b, uint32_t len) {
    if (t->sub == 0) {
        if (len < 38) { fail(t, TLS_EFAIL, 50, "malformed ServerHello"); return -1; }
        uint16_t legacy = rd16be(b);
        const uint8_t *random = b + 2;
        uint32_t o = 34;
        uint32_t sidl = b[o++];
        if (o + sidl + 3 > len) { fail(t, TLS_EFAIL, 50, "malformed ServerHello"); return -1; }
        o += sidl;
        uint16_t suite = rd16be(b + o);
        o += 2;
        if (b[o++] != 0) { fail(t, TLS_EFAIL, 47, "the server chose compression"); return -1; }
        uint16_t sv = 0, ks_group = 0;
        const uint8_t *ks = NULL;
        uint32_t ksl = 0;
        const uint8_t *cookie = NULL;
        uint32_t cookiel = 0;
        bool ems = false;
        if (o + 2 <= len) {
            uint32_t el = rd16be(b + o);
            o += 2;
            if (o + el > len) { fail(t, TLS_EFAIL, 50, "malformed ServerHello extensions"); return -1; }
            const uint8_t *e = b + o, *eend = e + el;
            while (eend - e >= 4) {
                uint16_t type = rd16be(e), l = rd16be(e + 2);
                const uint8_t *v = e + 4;
                if (eend - v < l) { fail(t, TLS_EFAIL, 50, "malformed ServerHello extension"); return -1; }
                if (type == 0x002b && l == 2) sv = rd16be(v);
                else if (type == 0x0033 && l >= 2) {
                    ks_group = rd16be(v);
                    if (l >= 4 && rd16be(v + 2) == l - 4u) { ks = v + 4; ksl = l - 4u; }
                } else if (type == 0x002c && l >= 2 && rd16be(v) == l - 2u) { cookie = v + 2; cookiel = l - 2u; }
                else if (type == 0x0017) ems = true;
                e = v + l;
            }
        }
        const suite_t *cs = NULL;
        for (int i = 0; i < ARRAY_SIZE(SUITES); i++) if (SUITES[i].id == suite) cs = &SUITES[i];
        bool tls13 = sv == 0x0304;
        if (!tls13 && (sv || legacy != 0x0303)) { fail(t, TLS_EFAIL, 70, "the server only supports old TLS versions"); return -1; }
        if (!cs || cs->tls13 != tls13) { fail(t, TLS_EFAIL, 47, "the server chose an unsupported cipher suite"); return -1; }
        if (t->hrr && t->cs && cs != t->cs) { fail(t, TLS_EFAIL, 47, "the cipher suite changed after HelloRetryRequest"); return -1; }
        t->version = tls13 ? 0x0304 : 0x0303;
        t->cs = cs;
        t->h = cs->h;
        if (tls13 && !memcmp(random, HRR_RANDOM, 32)) {
            /* HelloRetryRequest: the server wants another key share group (and maybe a cookie) */
            if (t->hrr) { fail(t, TLS_EFAIL, 10, "a second HelloRetryRequest"); return -1; }
            if (ks_group != GROUP_X25519 && ks_group != GROUP_P256 && ks_group != GROUP_P384) {
                fail(t, TLS_EFAIL, 47, "the server wants an unsupported key exchange group");
                return -1;
            }
            if (ks_group == t->group && !cookie) { fail(t, TLS_EFAIL, 47, "a useless HelloRetryRequest"); return -1; }
            t->hrr = true;
            t->group = ks_group;
            if (cookie) {
                t->cookie = kmalloc(cookiel);
                if (!t->cookie) { fail(t, TLS_EFAIL, 80, "out of memory"); return -1; }
                memcpy(t->cookie, cookie, cookiel);
                t->cookie_len = cookiel;
            }
            /* transcript = message_hash(ClientHello1) || HelloRetryRequest */
            uint8_t ch1[48], mh[4 + 48];
            th_hash(t, ch1);
            mh[0] = HS_MESSAGE_HASH; mh[1] = 0; mh[2] = 0; mh[3] = (uint8_t)t->h->size;
            memcpy(mh + 4, ch1, t->h->size);
            t->h->init(t->h == &HASH_SHA384 ? &t->th384 : &t->th256);
            th_add(t, mh, 4 + t->h->size);
            th_add(t, msg, 4 + len);
            t->sub = 10;                                 /* next: new key share (expensive), ClientHello 2 */
            return 1;
        }
        memcpy(t->server_random, random, 32);
        if (!tls13) {
            static const uint8_t DOWNGRADE[8] = { 0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44, 0x01 };
            if (!memcmp(random + 24, DOWNGRADE, 8)) { fail(t, TLS_EFAIL, 47, "TLS downgrade attack detected"); return -1; }
            t->ems = ems;
            th_add(t, msg, 4 + len);
            t->hs = H12_WAIT_CERT;
            return 0;
        }
        if (ks_group != t->group || !ks || ksl > sizeof t->peer_pub) { fail(t, TLS_EFAIL, 47, "bad key share from the server"); return -1; }
        memcpy(t->peer_pub, ks, ksl);
        t->peer_pub_len = ksl;
        th_add(t, msg, 4 + len);
        t->sub = 1;
        return 1;                                        /* ECDH on the next step */
    }
    if (t->sub == 1) {                                   /* TLS 1.3: shared secret, handshake keys */
        if (!compute_shared(t)) { fail(t, TLS_EFAIL, 47, "bad key share from the server"); return -1; }
        t->heavy = true;
        derive_handshake_keys(t);
        t->hs = H_WAIT_EE;
        return 0;
    }
    if (t->sub == 10) {                                  /* after HRR: new key share */
        if (!keygen(t)) { fail(t, TLS_EFAIL, 80, "key generation failed"); return -1; }
        if (!send_client_hello(t)) { fail(t, TLS_EFAIL, 80, "out of memory"); return -1; }
        t->heavy = true;
        t->hs = H_WAIT_SH;
        return 0;
    }
    return -1;
}

static int on_encrypted_extensions(tls_conn_t *t, const uint8_t *msg, const uint8_t *b, uint32_t len) {
    if (len < 2 || rd16be(b) != len - 2) { fail(t, TLS_EFAIL, 50, "malformed EncryptedExtensions"); return -1; }
    th_add(t, msg, 4 + len);
    t->hs = H_WAIT_CERT_CR;
    return 0;
}

static int on_certificate_request(tls_conn_t *t, const uint8_t *msg, const uint8_t *b, uint32_t len) {
    t->cert_requested = true;
    if (t->version == 0x0304) {
        if (len < 1 || b[0] >= len) { fail(t, TLS_EFAIL, 50, "malformed CertificateRequest"); return -1; }
        t->cert_req_ctx_len = b[0];
        memcpy(t->cert_req_ctx, b + 1, b[0]);
        t->hs = H_WAIT_CERT;
    }
    th_add(t, msg, 4 + len);
    return 0;
}

static int on_certificate(tls_conn_t *t, const uint8_t *msg, const uint8_t *b, uint32_t len) {
    if (t->sub == 0) {
        int r = parse_certificates(t, b, len);
        if (r == -3) { fail(t, TLS_ECERT, 50, "The site sent no certificate"); strlcpy(t->cert_error, t->errtext, sizeof t->cert_error); return -1; }
        if (r == -2) { fail(t, TLS_ECERT, 42, "The site's certificate is damaged"); strlcpy(t->cert_error, t->errtext, sizeof t->cert_error); return -1; }
        if (r) { fail(t, TLS_EFAIL, 50, "malformed Certificate message"); return -1; }
        const x509_t *leaf = &t->chain->certs[0];
        if (leaf->key_type == X509_KEY_NONE) {
            fail(t, TLS_ECERT, 43, "The site's certificate uses an unsupported key type");
            strlcpy(t->cert_error, t->errtext, sizeof t->cert_error);
            return -1;
        }
        t->sub = 1;
    }
    int v = verify_step(t);
    if (v != 0) return v;
    t->heavy = true;
    th_add(t, msg, 4 + len);
    t->hs = t->version == 0x0304 ? H_WAIT_CV : H12_WAIT_SKE;
    return 0;
}

static int on_certificate_verify(tls_conn_t *t, const uint8_t *msg, const uint8_t *b, uint32_t len) {
    if (len < 4 || rd16be(b + 2) != len - 4) { fail(t, TLS_EFAIL, 50, "malformed CertificateVerify"); return -1; }
    int alg;
    const hash_alg_t *h;
    if (!scheme_info(rd16be(b), &alg, &h, true)) { fail(t, TLS_EFAIL, 47, "unsupported signature scheme %04x", rd16be(b)); return -1; }
    /* signed content: 64 spaces, context string, 0, transcript hash */
    uint8_t content[64 + 34 + 48];
    memset(content, 0x20, 64);
    memcpy(content + 64, "TLS 1.3, server CertificateVerify", 34);   /* includes the 0 byte */
    th_hash(t, content + 98);
    if (!x509_verify_with_key(&t->chain->certs[0], alg, h, content, 98 + t->h->size, b + 4, len - 4)) {
        fail(t, TLS_EFAIL, 51, "the server's signature is invalid");
        return -1;
    }
    t->heavy = true;
    th_add(t, msg, 4 + len);
    t->hs = H_WAIT_FIN;
    return 0;
}

static void build_info(tls_conn_t *t) {
    const char *g = t->group == GROUP_X25519 ? "X25519" : t->group == GROUP_P256 ? "P-256" : "P-384";
    snprintf(t->info, sizeof t->info, "TLS %s, %s, %s", t->version == 0x0304 ? "1.3" : "1.2", t->cs->name, g);
}

static bool send_empty_certificate(tls_conn_t *t) {
    uint8_t b[1 + 255 + 3];
    uint32_t o = 0;
    if (t->version == 0x0304) {
        b[o++] = (uint8_t)t->cert_req_ctx_len;
        memcpy(b + o, t->cert_req_ctx, t->cert_req_ctx_len);
        o += t->cert_req_ctx_len;
    }
    wr24(b + o, 0);
    o += 3;
    return send_hs(t, HS_CERTIFICATE, b, o);
}

static int on_finished13(tls_conn_t *t, const uint8_t *msg, const uint8_t *b, uint32_t len) {
    uint8_t want[48];
    finished_mac(t, t->s_hs, want);
    if (len != t->h->size || !ct_equal(want, b, len)) { fail(t, TLS_EFAIL, 51, "the server's Finished message is wrong"); return -1; }
    th_add(t, msg, 4 + len);
    derive_app_secrets(t);
    /* our flight: (CCS for middleboxes) [empty Certificate] Finished, with the handshake keys */
    put_record(t, CT_CCS, (const uint8_t *)"\x01", 1);
    set_traffic_keys(t, &t->wr, t->c_hs);
    if (t->cert_requested && !send_empty_certificate(t)) { fail(t, TLS_EFAIL, 80, "out of memory"); return -1; }
    uint8_t fin[48];
    finished_mac(t, t->c_hs, fin);
    if (!send_hs(t, HS_FINISHED, fin, t->h->size)) { fail(t, TLS_EFAIL, 80, "out of memory"); return -1; }
    set_traffic_keys(t, &t->wr, t->c_ap);
    set_traffic_keys(t, &t->rd, t->s_ap);
    secure_zero(t->hs_secret, sizeof t->hs_secret);
    t->hs = H_DONE;
    t->state = TLSS_OPEN;
    build_info(t);
    NLOG("TLS %s: %s, certificate %s", t->host, t->info, t->cert_ok ? "verified" : "NOT trusted");
    return 0;
}

/* TLS 1.2 ServerKeyExchange: ECDHE parameters signed with the certificate key */
static int on_server_key_exchange(tls_conn_t *t, const uint8_t *msg, const uint8_t *b, uint32_t len) {
    if (t->sub == 0) {
        if (len < 4 || b[0] != 3) { fail(t, TLS_EFAIL, 50, "unsupported ServerKeyExchange"); return -1; }
        uint16_t group = rd16be(b + 1);
        uint32_t pl = b[3];
        if (4 + pl + 4 > len || pl > sizeof t->peer_pub) { fail(t, TLS_EFAIL, 50, "malformed ServerKeyExchange"); return -1; }
        if (group != GROUP_X25519 && group != GROUP_P256 && group != GROUP_P384) {
            fail(t, TLS_EFAIL, 47, "the server chose an unsupported curve");
            return -1;
        }
        uint32_t params_len = 4 + pl;
        uint16_t scheme = rd16be(b + params_len);
        uint32_t sl = rd16be(b + params_len + 2);
        if (params_len + 4 + sl != len) { fail(t, TLS_EFAIL, 50, "malformed ServerKeyExchange"); return -1; }
        int alg;
        const hash_alg_t *h;
        if (!scheme_info(scheme, &alg, &h, false)) { fail(t, TLS_EFAIL, 47, "unsupported signature scheme %04x", scheme); return -1; }
        uint8_t *signed_data = kmalloc(64 + params_len);
        if (!signed_data) { fail(t, TLS_EFAIL, 80, "out of memory"); return -1; }
        memcpy(signed_data, t->client_random, 32);
        memcpy(signed_data + 32, t->server_random, 32);
        memcpy(signed_data + 64, b, params_len);
        bool ok = x509_verify_with_key(&t->chain->certs[0], alg, h, signed_data, 64 + params_len, b + params_len + 4, sl);
        kfree(signed_data);
        if (!ok) { fail(t, TLS_EFAIL, 51, "the server's signature is invalid"); return -1; }
        memcpy(t->peer_pub, b + 4, pl);
        t->peer_pub_len = pl;
        t->ske_sigalg = scheme;
        if (group != t->group) {                         /* our ClientHello key was for another group */
            t->group = group;
            t->sub = 1;
            return 1;
        }
        t->sub = 2;
        return 1;
    }
    if (t->sub == 1) {
        if (!keygen(t)) { fail(t, TLS_EFAIL, 80, "key generation failed"); return -1; }
        t->sub = 2;
        return 1;
    }
    if (!compute_shared(t)) { fail(t, TLS_EFAIL, 47, "bad key from the server"); return -1; }
    t->heavy = true;
    th_add(t, msg, 4 + len);
    t->hs = H12_WAIT_SHD;
    return 0;
}

static int on_server_hello_done(tls_conn_t *t, const uint8_t *msg, const uint8_t *b, uint32_t len) {
    (void)b;
    if (len) { fail(t, TLS_EFAIL, 50, "malformed ServerHelloDone"); return -1; }
    th_add(t, msg, 4);
    if (t->cert_requested && !send_empty_certificate(t)) { fail(t, TLS_EFAIL, 80, "out of memory"); return -1; }
    /* ClientKeyExchange */
    uint8_t cke[1 + 97];
    uint32_t kl = key_share_len(t);
    cke[0] = (uint8_t)kl;
    memcpy(cke + 1, t->group == GROUP_X25519 ? t->x_pub : t->ec_pub, kl);
    if (!send_hs(t, HS_CKE, cke, 1 + kl)) { fail(t, TLS_EFAIL, 80, "out of memory"); return -1; }
    /* master secret */
    const hash_alg_t *h = t->h;
    if (t->ems) {
        uint8_t sh[48];
        th_hash(t, sh);
        tls12_prf(h, t->shared, t->shared_len, "extended master secret", sh, h->size, t->master, 48);
    } else {
        uint8_t seed[64];
        memcpy(seed, t->client_random, 32);
        memcpy(seed + 32, t->server_random, 32);
        tls12_prf(h, t->shared, t->shared_len, "master secret", seed, 64, t->master, 48);
    }
    secure_zero(t->shared, sizeof t->shared);
    /* key block: client key, server key, client IV, server IV */
    int kln = keylen_of(t->cs->aead), ivl = t->cs->aead == AEAD_CHACHA ? 12 : 4;
    uint8_t seed[64], kb[2 * 32 + 2 * 12];
    memcpy(seed, t->server_random, 32);
    memcpy(seed + 32, t->client_random, 32);
    tls12_prf(h, t->master, 48, "key expansion", seed, 64, kb, (size_t)(2 * kln + 2 * ivl));
    put_record(t, CT_CCS, (const uint8_t *)"\x01", 1);
    prot_set(&t->wr, t->cs->aead, kb, kln, kb + 2 * kln, ivl);
    prot_set(&t->rd, t->cs->aead, kb + kln, kln, kb + 2 * kln + ivl, ivl);
    t->rd.on = false;                                    /* until the server's ChangeCipherSpec */
    secure_zero(kb, sizeof kb);
    uint8_t th[48], fin[12];
    th_hash(t, th);
    tls12_prf(h, t->master, 48, "client finished", th, h->size, fin, 12);
    if (!send_hs(t, HS_FINISHED, fin, 12)) { fail(t, TLS_EFAIL, 80, "out of memory"); return -1; }
    t->hs = H12_WAIT_CCS;
    return 0;
}

static int on_finished12(tls_conn_t *t, const uint8_t *msg, const uint8_t *b, uint32_t len) {
    uint8_t th[48], want[12];
    th_hash(t, th);
    tls12_prf(t->h, t->master, 48, "server finished", th, t->h->size, want, 12);
    if (len != 12 || !ct_equal(want, b, 12)) { fail(t, TLS_EFAIL, 51, "the server's Finished message is wrong"); return -1; }
    th_add(t, msg, 4 + len);
    t->hs = H_DONE;
    t->state = TLSS_OPEN;
    build_info(t);
    NLOG("TLS %s: %s, certificate %s", t->host, t->info, t->cert_ok ? "verified" : "NOT trusted");
    return 0;
}

static int on_key_update(tls_conn_t *t, const uint8_t *b, uint32_t len) {
    if (len != 1 || b[0] > 1) { fail(t, TLS_EFAIL, 50, "malformed KeyUpdate"); return -1; }
    hkdf_expand_label(t->h, t->s_ap, "traffic upd", NULL, 0, t->s_ap, t->h->size);
    set_traffic_keys(t, &t->rd, t->s_ap);
    if (b[0] == 1) {                                     /* update_requested: answer with our own */
        uint8_t m[5] = { HS_KEY_UPDATE, 0, 0, 1, 0 };
        put_record(t, CT_HANDSHAKE, m, 5);
        flush_out(t);
        hkdf_expand_label(t->h, t->c_ap, "traffic upd", NULL, 0, t->c_ap, t->h->size);
        set_traffic_keys(t, &t->wr, t->c_ap);
    }
    return 0;
}

/* Dispatch the first complete handshake message in hsbuf. Returns 1 = yield, 0 = done, -1 = fail. */
static int process_message(tls_conn_t *t) {
    const uint8_t *msg = t->hsbuf;
    int type = msg[0];
    uint32_t len = rd24(msg + 1);
    const uint8_t *b = msg + 4;
    int r;
    if (t->hs == H_DONE) {
        if (t->version == 0x0304 && type == HS_NEW_TICKET) r = 0;            /* no resumption: ignore */
        else if (t->version == 0x0304 && type == HS_KEY_UPDATE) r = on_key_update(t, b, len);
        else if (t->version == 0x0303 && type == HS_HELLO_REQUEST) r = 0;    /* no renegotiation */
        else { fail(t, TLS_EFAIL, 10, "unexpected handshake message %d", type); r = -1; }
    } else {
        switch (t->hs) {
        case H_WAIT_SH:
            r = type == HS_SERVER_HELLO ? on_server_hello(t, msg, b, len) : -2;
            break;
        case H_WAIT_EE:
            r = type == HS_ENC_EXT ? on_encrypted_extensions(t, msg, b, len) : -2;
            break;
        case H_WAIT_CERT_CR:
            if (type == HS_CERT_REQ) r = on_certificate_request(t, msg, b, len);
            else r = type == HS_CERTIFICATE ? on_certificate(t, msg, b, len) : -2;
            break;
        case H_WAIT_CERT:
        case H12_WAIT_CERT:
            r = type == HS_CERTIFICATE ? on_certificate(t, msg, b, len) : -2;
            break;
        case H_WAIT_CV:
            r = type == HS_CERT_VERIFY ? on_certificate_verify(t, msg, b, len) : -2;
            break;
        case H_WAIT_FIN:
            r = type == HS_FINISHED ? on_finished13(t, msg, b, len) : -2;
            break;
        case H12_WAIT_SKE:
            r = type == HS_SKE ? on_server_key_exchange(t, msg, b, len) : -2;
            break;
        case H12_WAIT_SHD:
            if (type == HS_CERT_REQ) r = on_certificate_request(t, msg, b, len);
            else r = type == HS_SHD ? on_server_hello_done(t, msg, b, len) : -2;
            break;
        case H12_WAIT_FIN:
            r = type == HS_FINISHED ? on_finished12(t, msg, b, len) : -2;
            break;
        default:
            r = -2;
        }
        if (r == -2) { fail(t, TLS_EFAIL, 10, "unexpected handshake message %d", type); r = -1; }
    }
    if (r == 0) {
        t->sub = 0;
        uint32_t used = 4 + len;
        memmove(t->hsbuf, t->hsbuf + used, t->hs_len - used);
        t->hs_len -= used;
    }
    return r;
}

static bool have_message(tls_conn_t *t) { return t->hs_len >= 4 && t->hs_len >= 4 + rd24(t->hsbuf + 1); }

/* ---- input ---------------------------------------------------------------------------------------------- */
static bool hs_append(tls_conn_t *t, const uint8_t *d, uint32_t n) {
    if (t->hs_len + n > MAX_HS_MSG) { fail(t, TLS_EFAIL, 80, "handshake message too large"); return false; }
    if (t->hs_len + n > t->hs_cap) {
        uint32_t cap = t->hs_cap ? t->hs_cap : 8192;
        while (cap < t->hs_len + n) cap *= 2;
        uint8_t *nb = krealloc(t->hsbuf, cap);
        if (!nb) { fail(t, TLS_EFAIL, 80, "out of memory"); return false; }
        t->hsbuf = nb;
        t->hs_cap = cap;
    }
    memcpy(t->hsbuf + t->hs_len, d, n);
    t->hs_len += n;
    return true;
}

/* Read and handle one record. Returns false when no complete record is available. */
static bool read_record(tls_conn_t *t) {
    if (t->rec_have < 5) {
        t->rec_have += tcp_read(t->tcp, t->rec + t->rec_have, 5 - t->rec_have);
        if (t->rec_have < 5) return false;
    }
    uint32_t len = rd16be(t->rec + 3);
    if (len > MAX_CIPHER) { fail(t, TLS_EFAIL, 22, "record too large"); return false; }
    if (t->rec_have < 5 + len) {
        t->rec_have += tcp_read(t->tcp, t->rec + t->rec_have, 5 + len - t->rec_have);
        if (t->rec_have < 5 + len) return false;
    }
    t->rec_have = 0;
    int type = t->rec[0];
    uint8_t *body = t->rec + 5;
    if (type == CT_CCS) {
        if (len != 1 || body[0] != 1 || t->hs == H_DONE) { fail(t, TLS_EFAIL, 10, "unexpected ChangeCipherSpec"); return false; }
        if (t->version == 0x0303 && t->hs == H12_WAIT_CCS) {
            if (t->hs_len) { fail(t, TLS_EFAIL, 10, "unexpected ChangeCipherSpec"); return false; }
            t->rd.on = true;
            t->hs = H12_WAIT_FIN;
        }
        return true;                                     /* TLS 1.3 middlebox compatibility: ignore */
    }
    if (t->rd.on) {
        if (type != CT_APP && t->version == 0x0304) { fail(t, TLS_EFAIL, 10, "unencrypted record"); return false; }
        uint8_t nonce[12];
        uint8_t *ct = body;
        uint32_t ctlen = len;
        if (t->version == 0x0304) {
            if (len < 17) { fail(t, TLS_EFAIL, 50, "short record"); return false; }
            make_nonce(&t->rd, nonce);
            if (!aead_open(&t->rd, nonce, t->rec, 5, ct, len - 16, ct + len - 16)) {
                fail(t, TLS_EFAIL, 20, "decryption failed (bad record MAC)");
                return false;
            }
            t->rd.seq++;
            ctlen = len - 16;
            while (ctlen && !ct[ctlen - 1]) ctlen--;     /* strip padding */
            if (!ctlen) { fail(t, TLS_EFAIL, 10, "empty record"); return false; }
            type = ct[--ctlen];
        } else {
            uint32_t expl = t->rd.aead == AEAD_CHACHA ? 0 : 8;
            if (len < expl + 16) { fail(t, TLS_EFAIL, 50, "short record"); return false; }
            ctlen = len - expl - 16;
            uint8_t aad[13];
            for (int i = 0; i < 8; i++) aad[i] = (uint8_t)(t->rd.seq >> (56 - 8 * i));
            aad[8] = (uint8_t)type;
            wr16be(aad + 9, 0x0303);
            wr16be(aad + 11, (uint16_t)ctlen);
            if (expl) { memcpy(nonce, t->rd.iv, 4); memcpy(nonce + 4, body, 8); }
            else make_nonce(&t->rd, nonce);
            ct = body + expl;
            if (!aead_open(&t->rd, nonce, aad, 13, ct, ctlen, ct + ctlen)) {
                fail(t, TLS_EFAIL, 20, "decryption failed (bad record MAC)");
                return false;
            }
            t->rd.seq++;
        }
        if (ctlen > MAX_PLAIN) { fail(t, TLS_EFAIL, 22, "record too large"); return false; }
        body = ct;
        len = ctlen;
    } else if (type == CT_APP) {
        fail(t, TLS_EFAIL, 10, "unexpected application data");
        return false;
    }
    switch (type) {
    case CT_ALERT:
        if (len < 2) { fail(t, TLS_EFAIL, 50, "malformed alert"); return false; }
        if (body[1] == 0) { t->peer_closed = true; return true; }         /* close_notify */
        if (body[0] == 1 && body[1] != 0) return true;                    /* warning: ignore */
        t->alert = body[1];
        fail(t, TLS_EFAIL, -1, "the server refused the connection (%s, alert %d)", alert_name(body[1]), body[1]);
        return false;
    case CT_HANDSHAKE:
        if (!len) { fail(t, TLS_EFAIL, 10, "empty handshake record"); return false; }
        return hs_append(t, body, len);
    case CT_APP:
        if (t->state != TLSS_OPEN) { fail(t, TLS_EFAIL, 10, "application data before the handshake finished"); return false; }
        memcpy(t->app, body, len);                       /* the app buffer is empty here */
        t->app_off = 0;
        t->app_len = len;
        return true;
    default:
        fail(t, TLS_EFAIL, 10, "unknown record type %d", type);
        return false;
    }
}

/* Process handshake messages and records until a yield, lack of data or (open) app data. */
static void pump(tls_conn_t *t) {
    for (int guard = 0; guard < 64 && t->state != TLSS_ERROR; guard++) {
        if (have_message(t)) {
            int r = process_message(t);
            if (r != 0) return;                         /* yield (one expensive step) or failure */
            if (t->heavy) { t->heavy = false; return; }
            continue;
        }
        if (t->state == TLSS_OPEN && t->app_len) return;
        if (t->state == TLSS_OPEN && t->budget == 0) return;
        if (t->peer_closed) return;
        if (!read_record(t)) return;
        if (t->app_len) {
            t->budget = t->budget > t->app_len ? t->budget - t->app_len : 0;
            return;
        }
    }
}

void tls_step(tls_conn_t *t) {
    if (!t || t->state == TLSS_ERROR) return;
    t->budget = READ_BUDGET;
    int ts = tcp_state(t->tcp);
    /* a reset after the handshake: still decrypt what was received (tls_state reports
       the end once it is read) */
    if (ts == TCPS_ERROR && t->state != TLSS_OPEN) { fail(t, TLS_ETCP, -1, "the connection failed"); return; }
    if (t->hs == H_CONNECT) {
        if (ts == TCPS_CONNECTING) return;
        if (ts != TCPS_OPEN) { fail(t, TLS_ETCP, -1, "the server closed the connection"); return; }
        t->hs = H_KEYGEN;
        return;                                          /* key generation on the next step */
    }
    if (t->hs == H_KEYGEN) {
        if (!keygen(t)) { fail(t, TLS_EFAIL, -1, "key generation failed"); return; }
        if (!send_client_hello(t)) { fail(t, TLS_EFAIL, -1, "out of memory"); return; }
        t->hs = H_WAIT_SH;
        return;
    }
    flush_out(t);
    pump(t);
    flush_out(t);
    if (t->state == TLSS_HANDSHAKE && ts == TCPS_PEER_CLOSED && !tcp_readable(t->tcp) && !have_message(t))
        fail(t, TLS_EFAIL, -1, "the server closed the connection during the handshake");
}

/* ---- API ---------------------------------------------------------------------------------------------- */
tls_conn_t *tls_open(tcp_conn_t *tcp, const char *host, uint32_t flags) {
    tls_conn_t *t = kcalloc(1, sizeof *t);
    if (t) {
        t->rec = kmalloc(5 + MAX_CIPHER);
        t->app = kmalloc(MAX_PLAIN + 1);
        t->out = kmalloc(OUT_CAP);
    }
    if (!t || !t->rec || !t->app || !t->out) {
        if (t) { kfree(t->rec); kfree(t->app); kfree(t->out); kfree(t); }
        tcp_close(tcp, true);
        return NULL;
    }
    t->tcp = tcp;
    strlcpy(t->host, host, sizeof t->host);
    t->flags = flags;
    t->state = TLSS_HANDSHAKE;
    t->hs = H_CONNECT;
    t->group = GROUP_X25519;
    random_bytes(t->client_random, 32);
    random_bytes(t->session_id, 32);
    HASH_SHA256.init(&t->th256);
    HASH_SHA384.init(&t->th384);
    return t;
}

int tls_state(tls_conn_t *t) {
    if (t->state != TLSS_OPEN) return t->state;
    if (t->peer_closed) return TLSS_PEER_CLOSED;
    int ts = tcp_state(t->tcp);
    if ((ts == TCPS_PEER_CLOSED || ts == TCPS_ERROR) && !tcp_readable(t->tcp)) return TLSS_PEER_CLOSED;
    return TLSS_OPEN;
}

int tls_error(tls_conn_t *t) { return t->err; }
int tls_tcp_error(tls_conn_t *t) { return tcp_error(t->tcp); }
const char *tls_error_text(tls_conn_t *t) { return t->errtext; }

uint32_t tls_readable(tls_conn_t *t) { return t->app_len; }

uint32_t tls_read(tls_conn_t *t, void *buf, uint32_t max) {
    if (!t->app_len && t->state == TLSS_OPEN && t->budget) pump(t);    /* decrypt the next record */
    uint32_t n = MIN(max, t->app_len);
    memcpy(buf, t->app + t->app_off, n);
    t->app_off += n;
    t->app_len -= n;
    return n;
}

uint32_t tls_writable(tls_conn_t *t) {
    if (t->state != TLSS_OPEN) return 0;
    flush_out(t);
    return t->out_len ? 0 : MAX_PLAIN;
}

uint32_t tls_write(tls_conn_t *t, const void *data, uint32_t len) {
    if (t->state != TLSS_OPEN) return 0;
    flush_out(t);
    if (t->out_len) return 0;                            /* wait until TCP took the last record */
    uint32_t n = MIN(len, (uint32_t)MAX_PLAIN);
    if (!put_record(t, CT_APP, data, n)) return 0;
    flush_out(t);
    return n;
}

bool tls_cert_ok(tls_conn_t *t) { return t->cert_ok; }
const char *tls_cert_error(tls_conn_t *t) { return t->cert_error; }
const char *tls_cert_subject(tls_conn_t *t) { return t->cert_subject; }
const char *tls_cert_issuer(tls_conn_t *t) { return t->cert_issuer; }
const char *tls_info(tls_conn_t *t) { return t->info; }

void tls_close(tls_conn_t *t, bool abort) {
    if (!t) return;
    if (!abort && t->state == TLSS_OPEN) send_alert(t, 1, 0);         /* close_notify */
    tcp_close(t->tcp, abort);
    prot_free(&t->rd);
    prot_free(&t->wr);
    secure_zero(t->x_priv, sizeof t->x_priv);
    secure_zero(t->ec_priv, sizeof t->ec_priv);
    secure_zero(t->master, sizeof t->master);
    secure_zero(t->c_ap, sizeof t->c_ap);
    secure_zero(t->s_ap, sizeof t->s_ap);
    kfree(t->rec);
    kfree(t->app);
    kfree(t->out);
    kfree(t->hsbuf);
    kfree(t->certmsg);
    kfree(t->chain);
    kfree(t->cookie);
    kfree(t);
}
