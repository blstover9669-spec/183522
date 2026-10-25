/*
 * crypto_test.c - self tests of the TLS cryptography against published test vectors
 * (FIPS 180-4 SHA-2, RFC 4231 HMAC, RFC 5869 HKDF, the TLS 1.2 PRF vector, FIPS 197
 * AES, the GCM specification's test cases 4 and 16, RFC 8439 ChaCha20 / Poly1305 /
 * AEAD, RFC 7748 X25519) plus P-256 / P-384 ECDH + ECDSA and RSA PKCS #1 v1.5 / PSS
 * vectors made with OpenSSL. Run with selftest=1 (kernel/core/selftest.c).
 */
#include "kernel.h"
#include "crypto.h"

/* ECDH / ECDSA / RSA vectors (message "NovaOS crypto self test"), generated with OpenSSL */
#define P256_D "000000000000000000000000000000007f6e5d4bf4b2a18977ef66de55cd451b"
#define P256_PUB "045b6ada2bf0b1c59ace53a946aecea966475b93b016949af674f4d24317141ab519d6089e3b9db8d1a39ddf6268f8869d32b635d7e297738bee5afec52e804669"
#define P256_PEER "046aa8f482f49356b61926ef90542cf3fe09abaf7c19756e0f4a4dced13dac532c5a83973fcc3f6e2e9b2a66e983f0fea732161bb17630e1fd9761b92e2341fe58"
#define P256_SHARED "496764586147df34c7496d64ea87f68dd7ecb2332eaf59e4c5a467f296ecd0dc"
#define P256_SIG "3046022100e64e315bc2bc69969a35c9c608f9739169f01841baab7e8e138b11f826995804022100902f06829fc9f7134af0f8328f7be6addb46f6a29b0b96dc7b2dd78791978a49"
#define P384_D "00000000000000000000000000000000000000000000000000000000000000007f6e5d4bf4b2a18977ef66de55cd451b"
#define P384_PUB "0409d739baf5c2c4e79b73ea6d1e8ed88106d95c616a02e2982f8402799fddebbe9f1614395fb2da1a70e549242376b5ef65badbbe39af1d4d6bc7738ba2b1cf292b8fe7b7181266d4a40cb14682005c1b2339be75dc4f6ebd0ef1f9dadd8947ab"
#define P384_PEER "043d0c7767b8d1240932b4a1ef6274bd6c4f212daf6193e074a4ebc72a0e8ce47b52ab9992744e9f0e7885516b571ce8c9391fd8f4a3de413ec936d0763fa4ee1b78dea07d46336606fca120eb555795efcf721424ddbf098792fa7cb12722aad2"
#define P384_SHARED "7f525c7e6ebb7c483f53b29f725718f661d965777bec303caddbf90b2bf8c6d958738d6b34846a1b9c86249d7191b2fe"
#define P384_SIG "3065023100ac06ca8d13dda05e58803601e5d4200dd11f4321569b058591f8e774103205792effc90bf4438919fa992dbd860c66c602302e6d4f7322d2ba93a603f40e72fa8e078a4a1311f98355742abb695557d23f4aca54579a9d9266f985222c24cdee44f5"
#define RSA_N "b399197469c778d71044b457fa520fa95f6bfd1f31d1b16e1d0745f8ae54dfe287ce6d3388f916a178e5c11e2b0226278d5f8bbef1c009230c37d249791df6fcf32307b73882f1203d495d28b88b7b7ee44f56547221b64467fffab1201d0ab9d85f15f5fbc536d39c6855c2ef9d626d1ddd8848ff67205a1ed3513c871237421807d2093c95b33c8c4468607bfc4ac76d6049a1535f81234d000140c371641467ff91ce37c2e3859e6b3d97dec943819729c093ddc4808cb7c8d43fe711edb3a9e3ee8c4f8fd809288fb7ef6c03ff6438a1c580a6d3cbf3235fa6c92489fa8fdc040abab79cc7d2c116de28400708a25a6c8c2b707227e5103c5dfeffab3f8d"
#define RSA_PKCS1 "abdd72771b299a5a329ac8d9d36ecc67b54171bb182f6a101584ae4cc208749a11f37a7060b869e5a98539340dc759cc43c5da0c6b29e451f42d104fef43885f5c5030cc0a720961842cdbde72a431bd5ce05fe61976d45db1366f4b07676e57152dc93c1d4d5512cc24d734ee655d4307bf3baffc26c2e6e8c67ae5a07876433bde97250b67b543a0281eb1ad88644ee1a16ec9559c711437e912474a6937352d42407597f6accfdc151d6ffbebe11f6201a2be39f89ba86a125403e5b59cef68aff28cdfa49ad32aa08fd5ba717298ed6e64218901a8552087c4623b088d5e68f71e7500a5cefdcfd036bc350e69826bf6a9141937fec10b8f9948a45e0e4f"
#define RSA_PSS "57557a884c2b124f17d2a38b03f97147ed9f943f9390a3adc807a650dbf06bb055be92d5cda5ad53510cae459af96d66073fb2ddbb8cf191208b2fe25a6fac0a3d6b3c0c6ce39d4a162c145eebda1dabbbc6c9d3141a1203b088d1566e7e00c9fd91581488181374210d2fbe96ab4492c85d2a30022c132932d70cdc4a46163608f5c0f97957f3f241230e61ddaf98dd189322cf6ba216aa5161e74b4ab05e7284e4b32813c8acda9e12d9e5338177cf21d01f1f4a86c660fdec2e5d38b50bf8612131baeb17eaf0a19e2310e7ac62214d3e2ada9cb2325c04e85418a45ec683425c261aed1f538e7e4cf9ebe2baee4165d0e9d8e198191fea854c0868fc6e66"

static size_t unhex(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    while (s[0] && s[1] && n < cap) {
        int hi = isdigit((unsigned char)s[0]) ? s[0] - '0' : (tolower((unsigned char)s[0]) - 'a' + 10);
        int lo = isdigit((unsigned char)s[1]) ? s[1] - '0' : (tolower((unsigned char)s[1]) - 'a' + 10);
        out[n++] = (uint8_t)(hi << 4 | lo);
        s += 2;
    }
    return n;
}

static bool eqhex(const uint8_t *got, size_t len, const char *hex) {
    uint8_t want[512];
    size_t n = unhex(hex, want, sizeof want);
    return n == len && memcmp(got, want, len) == 0;
}

static const char LADIES[] = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
                             "sunscreen would be it.";

static bool t_sha2(void) {
    uint8_t o[64];
    bool ok = true;
    sha256("abc", 3, o);
    ok &= eqhex(o, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, o);
    ok &= eqhex(o, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    sha384("abc", 3, o);
    ok &= eqhex(o, 48, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
    sha512("abc", 3, o);
    ok &= eqhex(o, 64, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                       "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    sha1_ctx_t c;
    sha1_init(&c);
    sha1_update(&c, "abc", 3);
    sha1_final(&c, o);
    ok &= eqhex(o, 20, "a9993e364706816aba3e25717850c26c9cd0d89d");
    return ok;
}

static bool t_hmac_hkdf_prf(void) {
    uint8_t o[100], prk[32], ikm[22], salt[13], info[10];
    bool ok = true;
    hmac(&HASH_SHA256, "Jefe", 4, "what do ya want for nothing?", 28, o);
    ok &= eqhex(o, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    memset(ikm, 0x0b, sizeof ikm);
    for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
    for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
    hkdf_extract(&HASH_SHA256, salt, sizeof salt, ikm, sizeof ikm, prk);
    ok &= eqhex(prk, 32, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
    hkdf_expand(&HASH_SHA256, prk, info, sizeof info, o, 42);
    ok &= eqhex(o, 42, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
    uint8_t secret[16], seed[16];
    unhex("9bbe436ba940f017b17652849a71db35", secret, 16);
    unhex("a0ba9f936cda311827a6f796ffd5198c", seed, 16);
    tls12_prf(&HASH_SHA256, secret, 16, "test label", seed, 16, o, 100);
    ok &= eqhex(o, 100, "e3f229ba727be17b8d122620557cd453c2aab21d07c3d495329b52d4e61edb5a6b301791e90d35c9c9a46b4e14"
                        "baf9af0fa022f7077def17abfd3797c0564bab4fbc91666e9def9b97fce34f796789baa48082d122ee42c5a7"
                        "2e5a5110fff70187347b66");
    return ok;
}

static bool t_aes_gcm(void) {
    uint8_t key[32], blk[16], ct[64], tag[16], aad[20], iv[12];
    bool ok = true;
    aes_ctx_t a;
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    unhex("00112233445566778899aabbccddeeff", blk, 16);
    aes_set_key(&a, key, 16);
    aes_encrypt_block(&a, blk, ct);
    ok &= eqhex(ct, 16, "69c4e0d86a7b0430d8cdb78070b4c55a");
    aes_set_key(&a, key, 32);
    aes_encrypt_block(&a, blk, ct);
    ok &= eqhex(ct, 16, "8ea2b7ca516745bfeafc49904b496089");
    static const char *PT = "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
                            "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39";
    unhex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", key, 32);
    unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, 20);
    unhex("cafebabefacedbaddecaf888", iv, 12);
    aes_gcm_ctx_t *g = kmalloc(sizeof *g);
    if (!g) return false;
    for (int ks = 16; ks <= 32; ks += 16) {
        size_t n = unhex(PT, ct, sizeof ct);
        aes_gcm_init(g, key, ks);
        aes_gcm_encrypt(g, iv, aad, 20, ct, n, tag);
        if (ks == 16) {
            ok &= eqhex(ct, n, "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
                               "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091");
            ok &= eqhex(tag, 16, "5bc94fbc3221a5db94fae95ae7121a47");
        } else {
            ok &= eqhex(ct, n, "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
                               "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662");
            ok &= eqhex(tag, 16, "76fc6ece0f4e1768cddf8853bb2d551b");
        }
        ok &= aes_gcm_decrypt(g, iv, aad, 20, ct, n, tag) && eqhex(ct, n, PT);
        tag[0] ^= 1;
        ok &= !aes_gcm_decrypt(g, iv, aad, 20, ct, n, tag);
    }
    kfree(g);
    return ok;
}

static bool t_chacha(void) {
    uint8_t key[32], nonce[12], buf[128], tag[16], aad[12];
    bool ok = true;
    size_t n = strlen(LADIES);
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    unhex("000000000000004a00000000", nonce, 12);
    memcpy(buf, LADIES, n);
    chacha20_xor(key, 1, nonce, buf, n);
    ok &= eqhex(buf, n, "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab8f593dabcd62b357"
                        "1639d624e65152ab8f530c359f0861d807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
                        "5af90bbf74a35be6b40b8eedf2785e42874d");
    unhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", key, 32);
    poly1305_mac(key, (const uint8_t *)"Cryptographic Forum Research Group", 34, tag);
    ok &= eqhex(tag, 16, "a8061dc1305136c6c22b8baf0c0127a9");
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x80 + i);
    unhex("070000004041424344454647", nonce, 12);
    unhex("50515253c0c1c2c3c4c5c6c7", aad, 12);
    memcpy(buf, LADIES, n);
    chacha20poly1305_encrypt(key, nonce, aad, 12, buf, n, tag);
    ok &= eqhex(buf, n, "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b"
                        "1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
                        "3ff4def08e4b7a9de576d26586cec64b6116");
    ok &= eqhex(tag, 16, "1ae10b594f09e26a7e902ecbd0600691");
    ok &= chacha20poly1305_decrypt(key, nonce, aad, 12, buf, n, tag) && memcmp(buf, LADIES, n) == 0;
    buf[5] ^= 0x40;
    ok &= !chacha20poly1305_decrypt(key, nonce, aad, 12, buf, n, tag);
    return ok;
}

static bool t_x25519(void) {
    uint8_t k[32], u[32], o[32], a[32], b[32], pa[32], pb[32], s1[32], s2[32];
    bool ok = true;
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k, 32);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u, 32);
    x25519(o, k, u);
    ok &= eqhex(o, 32, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, 32);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b, 32);
    x25519_public(pa, a);
    x25519_public(pb, b);
    ok &= eqhex(pa, 32, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    ok &= eqhex(pb, 32, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    x25519(s1, a, pb);
    x25519(s2, b, pa);
    ok &= eqhex(s1, 32, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742") && !memcmp(s1, s2, 32);
    return ok;
}

static bool t_ec(int curve, const char *dh, const char *pubh, const char *peerh, const char *sharedh, const char *sigh,
                 const hash_alg_t *halg) {
    uint8_t d[48], pub[97], peer[97], shared[48], sig[128], hash[64], g[97];
    int sz = ec_size(curve);
    unhex(dh, d, sizeof d);
    size_t pl = unhex(pubh, pub, sizeof pub);
    size_t ql = unhex(peerh, peer, sizeof peer);
    size_t sl = unhex(sigh, sig, sizeof sig);
    bool ok = ec_ecdh(curve, d, peer, ql, shared) && eqhex(shared, (size_t)sz, sharedh);
    hash_buf(halg, "NovaOS crypto self test", 23, hash);
    ok &= ecdsa_verify(curve, pub, pl, hash, halg->size, sig, sl);
    hash[3] ^= 1;
    ok &= !ecdsa_verify(curve, pub, pl, hash, halg->size, sig, sl);
    peer[20] ^= 1;                                    /* a point that is not on the curve */
    ok &= !ec_ecdh(curve, d, peer, ql, shared);
    /* a fresh key pair agrees with the fixed key */
    uint8_t priv[48], s1[48], s2[48];
    ok &= ec_keygen(curve, priv, g) && ec_ecdh(curve, priv, pub, pl, s1) && ec_ecdh(curve, d, g, pl, s2) &&
          !memcmp(s1, s2, (size_t)sz);
    return ok;
}

static bool t_rsa(void) {
    uint8_t *n = kmalloc(256 * 3), hash[32];
    if (!n) return false;
    uint8_t *s1 = n + 256, *s2 = n + 512;
    static const uint8_t e[3] = { 1, 0, 1 };
    unhex(RSA_N, n, 256);
    unhex(RSA_PKCS1, s1, 256);
    unhex(RSA_PSS, s2, 256);
    sha256("NovaOS crypto self test", 23, hash);
    bool ok = rsa_pkcs1_verify(n, 256, e, 3, &HASH_SHA256, hash, s1, 256);
    ok &= rsa_pss_verify(n, 256, e, 3, &HASH_SHA256, hash, s2, 256);
    ok &= !rsa_pss_verify(n, 256, e, 3, &HASH_SHA256, hash, s1, 256);
    hash[0] ^= 1;
    ok &= !rsa_pkcs1_verify(n, 256, e, 3, &HASH_SHA256, hash, s1, 256);
    ok &= !rsa_pss_verify(n, 256, e, 3, &HASH_SHA256, hash, s2, 256);
    kfree(n);
    return ok;
}

int crypto_selftest(void (*report)(const char *name, bool ok)) {
    int failed = 0;
    struct { const char *name; bool ok; } r[9];
    int k = 0;
    uint32_t t0 = g_ticks;
    r[k].name = "crypto sha2";            r[k++].ok = t_sha2();
    r[k].name = "crypto hmac/hkdf/prf";   r[k++].ok = t_hmac_hkdf_prf();
    r[k].name = "crypto aes-gcm";         r[k++].ok = t_aes_gcm();
    r[k].name = "crypto chacha20-poly1305"; r[k++].ok = t_chacha();
    r[k].name = "crypto x25519";          r[k++].ok = t_x25519();
    r[k].name = "crypto p256";            r[k++].ok = t_ec(EC_P256, P256_D, P256_PUB, P256_PEER, P256_SHARED, P256_SIG, &HASH_SHA256);
    r[k].name = "crypto p384";            r[k++].ok = t_ec(EC_P384, P384_D, P384_PUB, P384_PEER, P384_SHARED, P384_SIG, &HASH_SHA384);
    r[k].name = "crypto rsa";             r[k++].ok = t_rsa();
    for (int i = 0; i < k; i++) {
        report(r[i].name, r[i].ok);
        if (!r[i].ok) failed++;
    }
    klog("crypto self test: %d ms", g_ticks - t0);
    return failed;
}
