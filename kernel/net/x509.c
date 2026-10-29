/*
 * x509.c - X.509 certificates (RFC 5280) for TLS: a DER parser, signature checks
 * (RSA PKCS #1 v1.5 / PSS, ECDSA P-256 / P-384 with SHA-256/384/512), host name
 * matching (subjectAltName, wildcards) and the chain verifier.
 *
 * The verifier builds a path from the server's certificate to one of the built-in
 * roots (roots.c), trying the roots first so chains with extra cross-signed
 * certificates still work. It checks validity dates, CA basic constraints, key
 * usage and the host name. Each x509_chain_step() does at most one signature check,
 * so a long chain is spread over several network ticks.
 */
#include "net_internal.h"
#include "crypto/crypto.h"

/* ---- DER ------------------------------------------------------------------------------------ */
typedef struct { const uint8_t *p, *end; } der_t;

/* Read one element: tag, content (val/len) and the whole element (start, total). */
static bool der_get(der_t *d, uint8_t *tag, const uint8_t **val, uint32_t *len, const uint8_t **start, uint32_t *total) {
    const uint8_t *p = d->p;
    if (d->end - p < 2) return false;
    uint8_t t = *p++;
    if ((t & 0x1F) == 0x1F) return false;               /* high tag numbers: not used in certificates */
    uint32_t l = *p++;
    if (l & 0x80) {
        int nb = l & 0x7F;
        if (nb < 1 || nb > 3 || d->end - p < nb) return false;
        l = 0;
        while (nb--) l = (l << 8) | *p++;
    }
    if ((uint32_t)(d->end - p) < l) return false;
    if (tag) *tag = t;
    if (val) *val = p;
    if (len) *len = l;
    if (start) *start = d->p;
    if (total) *total = (uint32_t)(p + l - d->p);
    d->p = p + l;
    return true;
}

/* Enter an element with the expected tag: *in = its content. */
static bool der_enter(der_t *d, uint8_t want, der_t *in) {
    uint8_t tag;
    const uint8_t *v;
    uint32_t l;
    if (!der_get(d, &tag, &v, &l, NULL, NULL) || tag != want) return false;
    in->p = v;
    in->end = v + l;
    return true;
}

static bool der_peek_tag(const der_t *d, uint8_t tag) { return d->end > d->p && d->p[0] == tag; }

static bool oid_eq(const uint8_t *v, uint32_t l, const uint8_t *oid, uint32_t ol) { return l == ol && !memcmp(v, oid, ol); }
#define OID(v, l, o) oid_eq(v, l, o, sizeof o)

static const uint8_t O_RSA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
static const uint8_t O_RSA_SHA1[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x05 };
static const uint8_t O_RSA_PSS[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0a };
static const uint8_t O_RSA_SHA256[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b };
static const uint8_t O_RSA_SHA384[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c };
static const uint8_t O_RSA_SHA512[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d };
static const uint8_t O_EC_KEY[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };
static const uint8_t O_P256[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };
static const uint8_t O_P384[] = { 0x2b, 0x81, 0x04, 0x00, 0x22 };
static const uint8_t O_ECDSA_SHA256[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };
static const uint8_t O_ECDSA_SHA384[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03 };
static const uint8_t O_ECDSA_SHA512[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x04 };
static const uint8_t O_SHA256[] = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01 };
static const uint8_t O_SHA384[] = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02 };
static const uint8_t O_SHA512[] = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03 };
static const uint8_t O_CN[] = { 0x55, 0x04, 0x03 };
static const uint8_t O_ORG[] = { 0x55, 0x04, 0x0a };
static const uint8_t O_SAN[] = { 0x55, 0x1d, 0x11 };
static const uint8_t O_BC[] = { 0x55, 0x1d, 0x13 };
static const uint8_t O_KU[] = { 0x55, 0x1d, 0x0f };
static const uint8_t O_EKU[] = { 0x55, 0x1d, 0x25 };
static const uint8_t O_EKU_ANY[] = { 0x55, 0x1d, 0x25, 0x00 };
static const uint8_t O_SERVER_AUTH[] = { 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01 };

/* ---- time ------------------------------------------------------------------------------------ */
static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int digits(const uint8_t *p, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

static bool parse_time(der_t *d, int64_t *out) {
    uint8_t tag;
    const uint8_t *v;
    uint32_t l;
    if (!der_get(d, &tag, &v, &l, NULL, NULL)) return false;
    int y;
    if (tag == 0x17 && l >= 13) {                       /* UTCTime YYMMDDHHMMSSZ */
        y = digits(v, 2);
        if (y < 0) return false;
        y += y < 50 ? 2000 : 1900;
        v += 2;
    } else if (tag == 0x18 && l >= 15) {                /* GeneralizedTime YYYYMMDDHHMMSSZ */
        y = digits(v, 4);
        if (y < 0) return false;
        v += 4;
    } else return false;
    int mo = digits(v, 2), dd = digits(v + 2, 2), hh = digits(v + 4, 2), mi = digits(v + 6, 2), ss = digits(v + 8, 2);
    if (mo < 1 || mo > 12 || dd < 1 || dd > 31 || hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 60) return false;
    *out = days_from_civil(y, mo, dd) * 86400 + hh * 3600 + mi * 60 + ss;
    return true;
}

/* ---- names --------------------------------------------------------------------------------- */
static void copy_string(char *out, size_t cap, uint8_t tag, const uint8_t *v, uint32_t l) {
    size_t o = 0;
    if (tag == 0x1E) {                                  /* BMPString (UCS-2): keep ASCII */
        for (uint32_t i = 0; i + 1 < l && o + 1 < cap; i += 2) out[o++] = v[i] ? '?' : (char)(v[i + 1] < 0x7F ? v[i + 1] : '?');
    } else {
        for (uint32_t i = 0; i < l && o + 1 < cap; i++) out[o++] = (v[i] >= 0x20 && v[i] != 0x7F) ? (char)v[i] : '?';
    }
    out[o] = 0;
}

/* first value of an attribute (CN / O) in a Name */
static void name_attr(const uint8_t *name, uint32_t len, const uint8_t *oid, uint32_t olen, char *out, size_t cap) {
    out[0] = 0;
    der_t d = { name, name + len }, seq, set, atv;
    if (!der_enter(&d, 0x30, &seq)) return;
    while (seq.p < seq.end) {
        if (!der_enter(&seq, 0x31, &set)) return;
        while (set.p < set.end) {
            if (!der_enter(&set, 0x30, &atv)) return;
            uint8_t tag;
            const uint8_t *v;
            uint32_t l;
            if (!der_get(&atv, &tag, &v, &l, NULL, NULL) || tag != 0x06) return;
            if (!oid_eq(v, l, oid, olen)) continue;
            if (!der_get(&atv, &tag, &v, &l, NULL, NULL)) return;
            copy_string(out, cap, tag, v, l);
            return;
        }
    }
}

/* ---- parsing ------------------------------------------------------------------------------- */
/* AlgorithmIdentifier of the signature -> scheme + hash */
static bool parse_sig_alg(der_t *d, x509_t *c) {
    der_t ai;
    uint8_t tag;
    const uint8_t *v;
    uint32_t l;
    if (!der_enter(d, 0x30, &ai) || !der_get(&ai, &tag, &v, &l, NULL, NULL) || tag != 0x06) return false;
    c->sig_alg = X509_SIG_UNSUPPORTED;
    c->sig_hash = NULL;
    if (OID(v, l, O_RSA_SHA256)) { c->sig_alg = X509_SIG_RSA_PKCS1; c->sig_hash = &HASH_SHA256; }
    else if (OID(v, l, O_RSA_SHA384)) { c->sig_alg = X509_SIG_RSA_PKCS1; c->sig_hash = &HASH_SHA384; }
    else if (OID(v, l, O_RSA_SHA512)) { c->sig_alg = X509_SIG_RSA_PKCS1; c->sig_hash = &HASH_SHA512; }
    else if (OID(v, l, O_RSA_SHA1)) { c->sig_alg = X509_SIG_SHA1; }
    else if (OID(v, l, O_ECDSA_SHA256)) { c->sig_alg = X509_SIG_ECDSA; c->sig_hash = &HASH_SHA256; }
    else if (OID(v, l, O_ECDSA_SHA384)) { c->sig_alg = X509_SIG_ECDSA; c->sig_hash = &HASH_SHA384; }
    else if (OID(v, l, O_ECDSA_SHA512)) { c->sig_alg = X509_SIG_ECDSA; c->sig_hash = &HASH_SHA512; }
    else if (OID(v, l, O_RSA_PSS)) {
        /* RSASSA-PSS-params { [0] hashAlgorithm DEFAULT sha1, ... } (MGF1 with the same hash assumed) */
        c->sig_alg = X509_SIG_RSA_PSS;
        c->sig_hash = &HASH_SHA1;
        der_t params, h0, hai;
        if (der_enter(&ai, 0x30, &params) && der_peek_tag(&params, 0xA0) && der_enter(&params, 0xA0, &h0) &&
            der_enter(&h0, 0x30, &hai) && der_get(&hai, &tag, &v, &l, NULL, NULL) && tag == 0x06) {
            if (OID(v, l, O_SHA256)) c->sig_hash = &HASH_SHA256;
            else if (OID(v, l, O_SHA384)) c->sig_hash = &HASH_SHA384;
            else if (OID(v, l, O_SHA512)) c->sig_hash = &HASH_SHA512;
            else c->sig_hash = NULL;
        }
        if (c->sig_hash == &HASH_SHA1) { c->sig_alg = X509_SIG_SHA1; c->sig_hash = NULL; }
    }
    return true;
}

static bool parse_spki(der_t *d, x509_t *c) {
    der_t spki, ai;
    uint8_t tag;
    const uint8_t *v, *start;
    uint32_t l, total;
    der_t save = *d;
    if (!der_get(&save, &tag, NULL, NULL, &start, &total) || tag != 0x30) return false;
    c->spki = start;
    c->spki_len = total;
    if (!der_enter(d, 0x30, &spki) || !der_enter(&spki, 0x30, &ai)) return false;
    if (!der_get(&ai, &tag, &v, &l, NULL, NULL) || tag != 0x06) return false;
    const uint8_t *bits;
    uint32_t bl;
    c->key_type = X509_KEY_NONE;
    if (OID(v, l, O_RSA)) {
        if (!der_get(&spki, &tag, &bits, &bl, NULL, NULL) || tag != 0x03 || bl < 2 || bits[0]) return false;
        der_t key = { bits + 1, bits + bl }, seq;
        if (!der_enter(&key, 0x30, &seq)) return false;
        if (!der_get(&seq, &tag, &c->rsa_n, &c->rsa_n_len, NULL, NULL) || tag != 0x02) return false;
        if (!der_get(&seq, &tag, &c->rsa_e, &c->rsa_e_len, NULL, NULL) || tag != 0x02) return false;
        c->key_type = X509_KEY_RSA;
    } else if (OID(v, l, O_EC_KEY)) {
        if (!der_get(&ai, &tag, &v, &l, NULL, NULL) || tag != 0x06) return false;
        if (OID(v, l, O_P256)) c->curve = EC_P256;
        else if (OID(v, l, O_P384)) c->curve = EC_P384;
        else c->curve = 0;                              /* P-521 etc.: not supported */
        if (!der_get(&spki, &tag, &bits, &bl, NULL, NULL) || tag != 0x03 || bl < 2 || bits[0]) return false;
        c->ec_point = bits + 1;
        c->ec_point_len = bl - 1;
        if (c->curve) c->key_type = X509_KEY_EC;
    }
    return true;
}

static void parse_extensions(der_t *d, x509_t *c) {
    der_t outer, exts, ext;
    if (!der_enter(d, 0xA3, &outer) || !der_enter(&outer, 0x30, &exts)) return;
    while (exts.p < exts.end) {
        if (!der_enter(&exts, 0x30, &ext)) return;
        uint8_t tag;
        const uint8_t *oid, *val;
        uint32_t ol, vl;
        if (!der_get(&ext, &tag, &oid, &ol, NULL, NULL) || tag != 0x06) return;
        if (der_peek_tag(&ext, 0x01)) der_get(&ext, NULL, NULL, NULL, NULL, NULL);    /* critical flag */
        if (!der_get(&ext, &tag, &val, &vl, NULL, NULL) || tag != 0x04) return;
        der_t ev = { val, val + vl }, s;
        if (OID(oid, ol, O_SAN)) {
            if (der_enter(&ev, 0x30, &s)) { c->san = s.p; c->san_len = (uint32_t)(s.end - s.p); }
        } else if (OID(oid, ol, O_BC)) {
            c->has_bc = true;
            if (der_enter(&ev, 0x30, &s)) {
                const uint8_t *v;
                uint32_t l;
                if (der_peek_tag(&s, 0x01) && der_get(&s, NULL, &v, &l, NULL, NULL) && l == 1) c->is_ca = v[0] != 0;
                if (der_peek_tag(&s, 0x02) && der_get(&s, NULL, &v, &l, NULL, NULL) && l >= 1 && l <= 2)
                    c->path_len = l == 1 ? v[0] : (v[0] << 8 | v[1]);
            }
        } else if (OID(oid, ol, O_KU)) {
            const uint8_t *v;
            uint32_t l;
            if (der_get(&ev, &tag, &v, &l, NULL, NULL) && tag == 0x03 && l >= 2) {
                c->has_ku = true;
                c->ku = (uint32_t)v[1] << 8 | (l >= 3 ? v[2] : 0);   /* bit 0 = digitalSignature (0x8000) */
            }
        } else if (OID(oid, ol, O_EKU)) {
            c->has_eku = true;
            if (der_enter(&ev, 0x30, &s))
                while (s.p < s.end) {
                    const uint8_t *v;
                    uint32_t l;
                    if (!der_get(&s, &tag, &v, &l, NULL, NULL)) break;
                    if (tag == 0x06 && (OID(v, l, O_SERVER_AUTH) || OID(v, l, O_EKU_ANY))) c->eku_server = true;
                }
        }
    }
}

bool x509_parse(x509_t *c, const uint8_t *der, uint32_t len) {
    memset(c, 0, sizeof *c);
    c->path_len = -1;
    der_t d = { der, der + len }, cert, tbs;
    uint8_t tag;
    const uint8_t *start, *v;
    uint32_t total, l;
    if (!der_enter(&d, 0x30, &cert)) return false;
    c->der = der;
    c->der_len = (uint32_t)(cert.end - der);
    /* tbsCertificate (keep its full encoding: that is what is signed) */
    der_t probe = cert;
    if (!der_get(&probe, &tag, NULL, NULL, &start, &total) || tag != 0x30) return false;
    c->tbs = start;
    c->tbs_len = total;
    if (!der_enter(&cert, 0x30, &tbs)) return false;
    if (der_peek_tag(&tbs, 0xA0)) der_get(&tbs, NULL, NULL, NULL, NULL, NULL);    /* version */
    if (!der_get(&tbs, &tag, NULL, NULL, NULL, NULL) || tag != 0x02) return false; /* serial */
    if (!der_get(&tbs, &tag, NULL, NULL, NULL, NULL) || tag != 0x30) return false; /* signature (repeated below) */
    if (!der_get(&tbs, &tag, NULL, NULL, &c->issuer, &c->issuer_len) || tag != 0x30) return false;
    der_t val;
    if (!der_enter(&tbs, 0x30, &val) || !parse_time(&val, &c->not_before) || !parse_time(&val, &c->not_after)) return false;
    if (!der_get(&tbs, &tag, NULL, NULL, &c->subject, &c->subject_len) || tag != 0x30) return false;
    if (!parse_spki(&tbs, c)) return false;
    while (tbs.p < tbs.end && !der_peek_tag(&tbs, 0xA3)) der_get(&tbs, NULL, NULL, NULL, NULL, NULL);  /* unique ids */
    if (tbs.p < tbs.end) parse_extensions(&tbs, c);
    /* signatureAlgorithm, signatureValue */
    if (!parse_sig_alg(&cert, c)) return false;
    if (!der_get(&cert, &tag, &v, &l, NULL, NULL) || tag != 0x03 || l < 1 || v[0]) return false;
    c->sig = v + 1;
    c->sig_len = l - 1;
    name_attr(c->subject, c->subject_len, O_CN, sizeof O_CN, c->cn, sizeof c->cn);
    name_attr(c->subject, c->subject_len, O_ORG, sizeof O_ORG, c->org, sizeof c->org);
    name_attr(c->issuer, c->issuer_len, O_CN, sizeof O_CN, c->issuer_cn, sizeof c->issuer_cn);
    name_attr(c->issuer, c->issuer_len, O_ORG, sizeof O_ORG, c->issuer_org, sizeof c->issuer_org);
    return true;
}

/* ---- signatures ------------------------------------------------------------------------------ */
bool x509_verify_with_key(const x509_t *key, int alg, const hash_alg_t *h, const uint8_t *data, uint32_t len,
                          const uint8_t *sig, uint32_t sig_len) {
    if (!h) return false;
    uint8_t hash[HASH_MAX_SIZE];
    hash_buf(h, data, len, hash);
    switch (alg) {
    case X509_SIG_RSA_PKCS1:
        return key->key_type == X509_KEY_RSA &&
               rsa_pkcs1_verify(key->rsa_n, key->rsa_n_len, key->rsa_e, key->rsa_e_len, h, hash, sig, sig_len);
    case X509_SIG_RSA_PSS:
        return key->key_type == X509_KEY_RSA &&
               rsa_pss_verify(key->rsa_n, key->rsa_n_len, key->rsa_e, key->rsa_e_len, h, hash, sig, sig_len);
    case X509_SIG_ECDSA:
        return key->key_type == X509_KEY_EC &&
               ecdsa_verify(key->curve, key->ec_point, key->ec_point_len, hash, h->size, sig, sig_len);
    }
    return false;
}

bool x509_check_signature(const x509_t *c, const x509_t *issuer) {
    return x509_verify_with_key(issuer, c->sig_alg, c->sig_hash, c->tbs, c->tbs_len, c->sig, c->sig_len);
}

/* ---- host names ------------------------------------------------------------------------------ */
static bool name_match(const char *pat, size_t pl, const char *host) {
    size_t hl = strlen(host);
    while (pl && pat[pl - 1] == '.') pl--;
    if (pl >= 2 && pat[0] == '*' && pat[1] == '.') {
        /* "*.example.com": exactly one label for the star, and not a bare TLD */
        const char *suffix = pat + 1;                   /* ".example.com" */
        size_t sl = pl - 1;
        if (!memchr(suffix + 1, '.', sl - 1)) return false;
        if (hl <= sl || strncasecmp(host + hl - sl, suffix, sl)) return false;
        return memchr(host, '.', hl - sl) == NULL;
    }
    return pl == hl && !strncasecmp(pat, host, hl);
}

bool x509_matches_host(const x509_t *c, const char *host) {
    uint32_t ip;
    bool is_ip = net_parse_ipv4(host, &ip);
    bool any_dns = false;
    if (c->san) {
        der_t d = { c->san, c->san + c->san_len };
        while (d.p < d.end) {
            uint8_t tag;
            const uint8_t *v;
            uint32_t l;
            if (!der_get(&d, &tag, &v, &l, NULL, NULL)) break;
            if (tag == 0x82) {                          /* dNSName */
                any_dns = true;
                if (!is_ip && name_match((const char *)v, l, host)) return true;
            } else if (tag == 0x87 && l == 4 && is_ip) {   /* iPAddress */
                if (rd32be(v) == ip) return true;
            }
        }
    }
    if (!any_dns && !is_ip && c->cn[0]) return name_match(c->cn, strlen(c->cn), host);   /* old certificates */
    return false;
}

/* first DNS name of the certificate, for display */
static void first_dns_name(const x509_t *c, char *out, size_t cap) {
    if (c->san) {
        der_t d = { c->san, c->san + c->san_len };
        while (d.p < d.end) {
            uint8_t tag;
            const uint8_t *v;
            uint32_t l;
            if (!der_get(&d, &tag, &v, &l, NULL, NULL)) break;
            if (tag == 0x82) { copy_string(out, cap, 0x16, v, l); return; }
        }
    }
    strlcpy(out, c->cn, cap);
}

/* ---- built-in roots ---------------------------------------------------------------------------- */
static x509_t *roots;
static int nroots;

static void load_roots(void) {
    if (roots) return;
    roots = kcalloc((size_t)tls_root_count, sizeof *roots);
    if (!roots) return;
    for (int i = 0; i < tls_root_count; i++)
        if (x509_parse(&roots[nroots], tls_roots[i].der, tls_roots[i].len)) nroots++;
    NLOG("TLS: %d built-in root certificates", nroots);
}

int x509_root_count(void) { load_roots(); return nroots; }

static bool same_name(const uint8_t *a, uint32_t al, const uint8_t *b, uint32_t bl) { return al == bl && !memcmp(a, b, al); }

/* ---- chain verification ---------------------------------------------------------------------- */
void x509_chain_start(x509_chain_t *v, const char *host, int64_t now) {
    load_roots();
    v->extra_root = NULL;
    strlcpy(v->host, host, sizeof v->host);
    v->now = now;
    v->cur = 0;
    v->cand = 0;
    v->depth = 0;
    v->result = X509_PENDING;
    v->error[0] = v->subject[0] = v->issuer[0] = 0;
    memset(v->used, 0, sizeof v->used);
    if (v->n > 0) {
        first_dns_name(&v->certs[0], v->subject, sizeof v->subject);
        strlcpy(v->issuer, v->certs[0].issuer_org[0] ? v->certs[0].issuer_org : v->certs[0].issuer_cn, sizeof v->issuer);
    }
}

static int finish(x509_chain_t *v, int result, const char *fmt, ...) PRINTF_LIKE(3, 4);
static int finish(x509_chain_t *v, int result, const char *fmt, ...) {
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(v->error, sizeof v->error, fmt, ap);
        va_end(ap);
    }
    v->result = result;
    return result;
}

/* the path is complete: dates, usage, host name */
static int final_checks(x509_chain_t *v) {
    const x509_t *leaf = &v->certs[0];
    for (int i = 0; i < v->n; i++) {
        if (i && !v->used[i]) continue;
        const x509_t *c = &v->certs[i];
        if (v->now < c->not_before)
            return finish(v, X509_EDATE, i ? "A certificate in the chain is not valid yet" : "The certificate is not valid yet");
        if (v->now > c->not_after)
            return finish(v, X509_EDATE, i ? "A certificate in the chain has expired" : "The certificate has expired");
    }
    if (leaf->has_eku && !leaf->eku_server) return finish(v, X509_EUSAGE, "The certificate is not meant for web sites");
    if (!x509_matches_host(leaf, v->host))
        return finish(v, X509_EHOST, "The certificate belongs to %s, not %s", v->subject[0] ? v->subject : "another site", v->host);
    return finish(v, X509_OK, NULL);
}

static bool can_sign_certs(const x509_t *ca, int below) {
    if (!ca->has_bc || !ca->is_ca) return false;
    if (ca->has_ku && !(ca->ku & 0x0400)) return false;            /* keyCertSign */
    if (ca->path_len >= 0 && below > ca->path_len) return false;
    return true;
}

int x509_chain_step(x509_chain_t *v) {
    if (v->result != X509_PENDING) return v->result;
    if (v->n == 0) return finish(v, X509_EBROKEN, "The site sent no certificate");
    for (;;) {
        if (v->depth > X509_MAX_CHAIN + 2) return finish(v, X509_EUNTRUSTED, "The certificate chain is too long");
        const x509_t *c = &v->certs[v->cur];
        /* the certificate is itself a trusted root (same name and key) */
        int ntrust = nroots + (v->extra_root ? 1 : 0);
        for (int i = 0; i < ntrust; i++) {
            const x509_t *r = i < nroots ? &roots[i] : v->extra_root;
            if (same_name(r->subject, r->subject_len, c->subject, c->subject_len) &&
                r->spki_len == c->spki_len && !memcmp(r->spki, c->spki, c->spki_len))
                return final_checks(v);
        }
        /* candidates: roots first, then the other certificates the server sent */
        int total = ntrust + v->n;
        while (v->cand < total) {
            int k = v->cand++;
            const x509_t *iss;
            if (k < ntrust) iss = k < nroots ? &roots[k] : v->extra_root;
            else {
                int idx = k - ntrust;
                if (idx == v->cur || v->used[idx] || idx == 0) continue;
                iss = &v->certs[idx];
                if (!can_sign_certs(iss, v->depth)) continue;
            }
            if (!same_name(iss->subject, iss->subject_len, c->issuer, c->issuer_len)) continue;
            if (c->sig_alg == X509_SIG_SHA1 || c->sig_alg == X509_SIG_UNSUPPORTED) {
                return finish(v, X509_EUNTRUSTED, c->sig_alg == X509_SIG_SHA1 ?
                              "The certificate uses an outdated signature (SHA-1)" :
                              "The certificate uses an unsupported signature type");
            }
            bool ok = x509_check_signature(c, iss);     /* the one expensive operation of this step */
            if (!ok) return X509_PENDING;                /* try the next candidate on the next step */
            if (k < ntrust) return final_checks(v);
            int idx = k - ntrust;
            v->used[idx] = 1;
            v->cur = idx;
            v->cand = 0;
            v->depth++;
            return X509_PENDING;
        }
        /* no issuer found */
        if (v->cur == 0 && same_name(c->subject, c->subject_len, c->issuer, c->issuer_len))
            return finish(v, X509_EUNTRUSTED, "The certificate is self-signed, so it is not trusted");
        if (same_name(c->subject, c->subject_len, c->issuer, c->issuer_len))
            return finish(v, X509_EUNTRUSTED, "The certificate was issued by an unknown authority (%s)",
                          c->org[0] ? c->org : c->cn[0] ? c->cn : "unnamed");
        return finish(v, X509_EUNTRUSTED, "The certificate was not issued by a trusted authority (%s)",
                      c->issuer_org[0] ? c->issuer_org : c->issuer_cn[0] ? c->issuer_cn : "unknown issuer");
    }
}
