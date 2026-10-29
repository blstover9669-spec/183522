/*
 * net_test.c - self tests of the certificate verifier (run with selftest=1 through
 * crypto_selftest): a test chain (P-384 root -> RSA-2048 intermediate, ECDSA-SHA384
 * -> P-256 site certificate, RSA-PSS) checked against a test trust anchor at fixed
 * times, and the built-in roots (they all parse; every self-signature made with a
 * supported algorithm verifies).
 */
#include "net_internal.h"
#include "crypto/crypto.h"

/* made with tools from the Python cryptography package; valid 2026-01-01 .. 2027-01-01,
   names www.novaos.test and *.novaos.test */
#define X509_ROOT \
    "308201c43082014aa00302010202143dd4e6237fb51bc6c18c4dc0da250ad9e39ec505300a06082a8648ce3d04030330" \
    "3131143012060355040a0c0b4e6f76614f5320546573743119301706035504030c104e6f76614f53205465737420526f" \
    "6f74301e170d3230303130313030303030305a170d3430303130313030303030305a303131143012060355040a0c0b4e" \
    "6f76614f5320546573743119301706035504030c104e6f76614f53205465737420526f6f743076301006072a8648ce3d" \
    "020106052b81040022036200047b75041c583db0e52a8617cce250e74997d8e016826ba186150861afca1d869e32dc68" \
    "ffba3f6c33adf4a405a4134d61ef625f137acd412a0385bf683924478b5aacc62dbe9dd09ae54564277642fc148973c0" \
    "586bcb6f9d8ce7ac749a74a192a3233021300f0603551d130101ff040530030101ff300e0603551d0f0101ff04040302" \
    "0106300a06082a8648ce3d040303036800306502301949008509127592e805450a1c5608a382eec55583343f236738aa" \
    "132a83fd62d1bb145b63445f3e370e90a7520b92ee023100c83ee31ef55affe744a5257cbf208c1c4225144179abe82e" \
    "bb486b669dd436bfffab70c8712321af72e643f5e5f20fbe"
#define X509_INTER \
    "3082027e30820203a003020102021465112f1b8750223a1e67dd8b40752de594cda12f300a06082a8648ce3d04030330" \
    "3131143012060355040a0c0b4e6f76614f5320546573743119301706035504030c104e6f76614f53205465737420526f" \
    "6f74301e170d3234303130313030303030305a170d3334303130313030303030305a303931143012060355040a0c0b4e" \
    "6f76614f5320546573743121301f06035504030c184e6f76614f53205465737420496e7465726d656469617465308201" \
    "22300d06092a864886f70d01010105000382010f003082010a0282010100af3fdb7fb5d211fd19cae190c84db918c84b" \
    "4ddf0f5577b06900fcac41ad28f8c0ca0f7938dc2e879fbf02000c503e5f1e5b1d54f112c188ec3a98fd662ffe782376" \
    "c4ff743693c3b981abf37e70628a083b002523e5abcc3961d7c886cbdf99868c2c843ad8293a0db52ea068885eff8ecd" \
    "1d2684f0d8d270b7bbc32b026b54ca1ba306bac81c33390297172bcf1a07170458863af9d7ef145b60b87e20cf616e65" \
    "e36c2eef67c600df0d10490879dd9c999763702f7cd59d15fe75e7f1d677961fe2e4733c92d55ce65aca200e675bce9a" \
    "b653aa834411adb376ebbc33fa7c144e7fee83359069f3097f358d30a55b3270aff93ea36c312542d22421f30ae70203" \
    "010001a326302430120603551d130101ff040830060101ff020100300e0603551d0f0101ff040403020106300a06082a" \
    "8648ce3d0403030369003066023100f3142ce0422bbfc79ffa80f7753c45f43dfc5b7589f9b8c1f5e6524eb64e566f00" \
    "1a3b3437b1e91229d7ea9e9eb10427023100bfa2da63f36ac4dce82506c914b30109b35a304370c971b77d6fa055370d" \
    "6ef94338a3a1a454f8fced7aab871260341e"
#define X509_LEAF \
    "308202e430820198a0030201020214670c5c103e438d1d86dcb78fd56f03dded4fe44f304106092a864886f70d01010a" \
    "3034a00f300d06096086480165030402010500a11c301a06092a864886f70d010108300d060960864801650304020105" \
    "00a203020120303931143012060355040a0c0b4e6f76614f5320546573743121301f06035504030c184e6f76614f5320" \
    "5465737420496e7465726d656469617465301e170d3236303130313030303030305a170d323730313031303030303030" \
    "5a303031143012060355040a0c0b4e6f76614f5320546573743118301606035504030c0f7777772e6e6f76616f732e74" \
    "6573743059301306072a8648ce3d020106082a8648ce3d030107034200040c4c3f8689b356ea8147a5b71ee5a39458d5" \
    "2b7e765341088fe528cf02295be7461c5be781654e87ad11b18f7a6bc313294d7beed0e9f33c5642128abf1e7f86a350" \
    "304e300c0603551d130101ff0402300030130603551d25040c300a06082b0601050507030130290603551d1104223020" \
    "820f7777772e6e6f76616f732e74657374820d2a2e6e6f76616f732e74657374304106092a864886f70d01010a3034a0" \
    "0f300d06096086480165030402010500a11c301a06092a864886f70d010108300d06096086480165030402010500a203" \
    "02012003820101008384c22c41b8a0d129bac404d7436b8b92eaacf75b6bffbbd6b860eb5962f657671398e3001c0950" \
    "50be0b779ebd90601a7bbdb4cc763d7accdb9981d543faf1265d2f69284b9ea1dd058c3ff77c46039d690c5eb16a1b6e" \
    "4b619097e79ab21c870c7dbd59929540a335928b3ae9bc9cfac79ec4b8f1e5cfa321e4afae50d401cfdbfb73b0e7e48f" \
    "49fea541761b8765f01166f330ba00abcd275c274c03b38468ce1feb99d9d894cea98fa89a4bc0f30bb461216b8b042e" \
    "56460417b058d8a930c23c72e2d7ba6cf6aefb1d39c0aff05f5216bc2c4bc1891d9497a0d9b66eee7108b6e549dfd2b7" \
    "dab09466209d9f165d739ee81b29c6b314977d8b1b165f7f"

#define T_2025_06 1748736000
#define T_2026_06 1780272000
#define T_2027_06 1811808000

static uint8_t *unhex_alloc(const char *s, uint32_t *len) {
    uint32_t n = (uint32_t)strlen(s) / 2;
    uint8_t *b = kmalloc(n);
    if (!b) return NULL;
    for (uint32_t i = 0; i < n; i++) {
        int hi = isdigit((unsigned char)s[2 * i]) ? s[2 * i] - '0' : s[2 * i] - 'a' + 10;
        int lo = isdigit((unsigned char)s[2 * i + 1]) ? s[2 * i + 1] - '0' : s[2 * i + 1] - 'a' + 10;
        b[i] = (uint8_t)(hi << 4 | lo);
    }
    *len = n;
    return b;
}

static int run(x509_chain_t *v, const x509_t *const *chain, int n, const char *host, int64_t now, const x509_t *anchor) {
    for (int i = 0; i < n; i++) v->certs[i] = *chain[i];
    v->n = n;
    x509_chain_start(v, host, now);
    v->extra_root = anchor;
    int r, guard = 0;
    while ((r = x509_chain_step(v)) == X509_PENDING && ++guard < 1000) {}
    return r;
}

bool x509_selftest(void) {
    uint32_t rl, il, ll;
    uint8_t *rd = unhex_alloc(X509_ROOT, &rl), *id = unhex_alloc(X509_INTER, &il), *ld = unhex_alloc(X509_LEAF, &ll);
    x509_t *c = kcalloc(4, sizeof *c);
    x509_chain_t *v = kmalloc(sizeof *v);
    bool ok = rd && id && ld && c && v;
    if (ok) ok = x509_parse(&c[0], rd, rl) && x509_parse(&c[1], id, il) && x509_parse(&c[2], ld, ll);
    if (ok) {
        const x509_t *root = &c[0], *inter = &c[1], *leaf = &c[2];
        const x509_t *normal[] = { leaf, inter }, *mixed[] = { leaf, root, inter }, *alone[] = { leaf };
        struct { const x509_t *const *ch; int n; const char *host; int64_t now; bool anchor; int want; } T[] = {
            { normal, 2, "www.novaos.test", T_2026_06, true, X509_OK },
            { normal, 2, "x.novaos.test", T_2026_06, true, X509_OK },
            { normal, 2, "a.b.novaos.test", T_2026_06, true, X509_EHOST },
            { normal, 2, "novaos.test", T_2026_06, true, X509_EHOST },
            { normal, 2, "www.novaos.test.evil.com", T_2026_06, true, X509_EHOST },
            { normal, 2, "www.novaos.test", T_2027_06, true, X509_EDATE },
            { normal, 2, "www.novaos.test", T_2025_06, true, X509_EDATE },
            { normal, 2, "www.novaos.test", T_2026_06, false, X509_EUNTRUSTED },
            { mixed, 3, "www.novaos.test", T_2026_06, true, X509_OK },
            { alone, 1, "www.novaos.test", T_2026_06, true, X509_EUNTRUSTED },
        };
        for (int i = 0; i < ARRAY_SIZE(T); i++) {
            int r = run(v, T[i].ch, T[i].n, T[i].host, T[i].now, T[i].anchor ? root : NULL);
            if (r != T[i].want) { klog("  x509 case %d (%s): got %d, expected %d: %s", i, T[i].host, r, T[i].want, v->error); ok = false; }
        }
        /* a damaged signature */
        ld[ll - 5] ^= 0x10;
        if (x509_parse(&c[3], ld, ll)) {
            const x509_t *bad[] = { &c[3], inter };
            int r = run(v, bad, 2, "www.novaos.test", T_2026_06, root);
            if (r != X509_EUNTRUSTED) { klog("  x509 damaged signature accepted (%d)", r); ok = false; }
        }
    }
    kfree(rd);
    kfree(id);
    kfree(ld);
    kfree(c);
    kfree(v);
    return ok;
}

/* every built-in root parses, and its self-signature verifies when the algorithm is supported */
bool tls_roots_selftest(void) {
    int n = x509_root_count(), checked = 0, bad = 0, skipped = 0;
    x509_t *r = kmalloc(sizeof *r);
    if (!r) return false;
    for (int i = 0; i < tls_root_count; i++) {
        if (!x509_parse(r, tls_roots[i].der, tls_roots[i].len)) continue;
        if (r->sig_alg == X509_SIG_SHA1 || r->sig_alg == X509_SIG_UNSUPPORTED || r->key_type == X509_KEY_NONE ||
            r->issuer_len != r->subject_len || memcmp(r->issuer, r->subject, r->subject_len)) { skipped++; continue; }
        checked++;
        if (!x509_check_signature(r, r)) { bad++; klog("  root %d (%s): self-signature does not verify", i, r->cn); }
    }
    kfree(r);
    klog("  %d/%d roots parsed, %d self-signatures verified, %d failed, %d skipped (SHA-1 / other)",
         n, tls_root_count, checked - bad, bad, skipped);
    return n == tls_root_count && bad == 0 && checked > 0;
}
