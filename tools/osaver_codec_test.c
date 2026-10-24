/*
 * osaver_codec_test.c - host-side round-trip tests for the OSaver number language
 * (kernel/osaver/codec.c, compiled unchanged).
 *
 *   gcc -O2 -Wall -Wextra -Ikernel -o /tmp/osaver_test tools/osaver_codec_test.c kernel/osaver/codec.c
 *   /tmp/osaver_test            # prints "ALL PASSED" and exits 0
 *   /tmp/osaver_test big        # also a 200 MB code (streamed in pieces)
 */
#include "osaver/codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* ---- growable string sink ---------------------------------------------------------- */
typedef struct { char *s; size_t n, cap; } buf_t;
static void buf_add(buf_t *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
static int sink(void *ctx, const char *s, uint32_t n) { buf_add(ctx, s, n); return 0; }

/* ---- a fake file system to save and restore ------------------------------------------ */
typedef struct { int dir; char name[64]; char type[16]; uint8_t *data; uint32_t size; } tfile_t;
typedef struct { int id, parent; char name[64]; } tdir_t;
typedef struct {
    tdir_t dirs[16]; int ndirs;
    tfile_t files[64]; int nfiles;
    int wall_kind; uint32_t wall_index; char wall_name[64];
    uint8_t *wall_data; uint32_t wall_size;
} tfs_t;

static void tfs_free(tfs_t *t) {
    for (int i = 0; i < t->nfiles; i++) free(t->files[i].data);
    free(t->wall_data);
    memset(t, 0, sizeof *t);
}

static void build_inner(const tfs_t *t, buf_t *out) {
    osc_writer_t w;
    osc_w_init(&w, sink, out);
    uint32_t total = 0;
    for (int i = 0; i < t->nfiles; i++) total += t->files[i].size;
    osc_w_record(&w, 'B', 3141592653u);
    osc_w_num(&w, 1793000000u);
    osc_w_num(&w, 0);
    osc_w_num(&w, (uint32_t)t->nfiles);
    osc_w_num(&w, (uint32_t)t->ndirs);
    osc_w_num(&w, total);
    if (t->wall_kind == 0) {
        osc_w_record(&w, 'W', 0);
        osc_w_num(&w, t->wall_index);
        osc_w_bytes(&w, t->wall_name, (uint32_t)strlen(t->wall_name));
    } else {
        osc_w_record(&w, 'W', 1);
        osc_w_bytes(&w, t->wall_name, (uint32_t)strlen(t->wall_name));
        osc_w_num(&w, t->wall_size);
        osc_w_bytes(&w, t->wall_data, t->wall_size);
    }
    for (int i = 0; i < t->ndirs; i++) {
        osc_w_record(&w, 'D', (uint32_t)t->dirs[i].id);
        osc_w_num(&w, (uint32_t)t->dirs[i].parent);
        osc_w_bytes(&w, t->dirs[i].name, (uint32_t)strlen(t->dirs[i].name));
    }
    for (int i = 0; i < t->nfiles; i++) {
        const tfile_t *f = &t->files[i];
        osc_w_record(&w, 'F', (uint32_t)f->dir);
        osc_w_bytes(&w, f->name, (uint32_t)strlen(f->name));
        osc_w_bytes(&w, f->type, (uint32_t)strlen(f->type));
        osc_w_num(&w, f->size);
        /* stream the data in odd-sized parts */
        osc_w_data_begin(&w);
        uint32_t o = 0;
        while (o < f->size) {
            uint32_t part = 1 + rnd() % 977;
            if (part > f->size - o) part = f->size - o;
            osc_w_data_part(&w, f->data + o, part);
            o += part;
        }
        osc_w_data_end(&w);
    }
    osc_w_end(&w);
}

static void build_code(const tfs_t *t, buf_t *out, bool hidden) {
    char head[256];
    buf_t inner = {0};
    build_inner(t, &inner);
    int hl = osc_head(head, sizeof head, "2.5", t->nfiles, hidden);
    out->n = 0;
    buf_add(out, head, (size_t)hl);
    buf_add(out, inner.s ? inner.s : "", inner.n);
    buf_add(out, OSC_TAIL, 2);
    free(inner.s);
}

/* ---- restore callbacks ------------------------------------------------------------------ */
typedef struct {
    tfs_t got;
    tfile_t *cur;
    uint8_t *wbuf; uint32_t wlen;
    char version[40], files[16];
    int entries;
} rest_t;

static int cb_entry(void *ctx, int key, const char *v, int n) {
    rest_t *r = ctx;
    r->entries++;
    if (key == OSC_K_VERSION) { memcpy(r->version, v, (size_t)n); r->version[n] = 0; }
    if (key == OSC_K_FILES) { memcpy(r->files, v, (size_t)n); r->files[n] = 0; }
    return 0;
}
static int cb_rec(void *ctx, const osc_rec_t *rec) {
    rest_t *r = ctx;
    r->cur = NULL;
    if (rec->tag == 'D') {
        tdir_t *d = &r->got.dirs[r->got.ndirs++];
        d->id = (int)rec->num[0]; d->parent = (int)rec->num[1];
        strcpy(d->name, rec->str[0]);
    } else if (rec->tag == 'F') {
        tfile_t *f = &r->got.files[r->got.nfiles++];
        f->dir = (int)rec->num[0];
        strcpy(f->name, rec->str[0]);
        strcpy(f->type, rec->str[1]);
        f->size = 0;
        f->data = malloc(rec->size + 1);
        r->cur = f;
    } else if (rec->tag == 'W') {
        r->got.wall_kind = (int)rec->num[0];
        if (rec->num[0] == 0) { r->got.wall_index = rec->num[1]; strcpy(r->got.wall_name, rec->str[0]); }
        else { strcpy(r->got.wall_name, rec->str[0]); r->got.wall_data = malloc(rec->size + 1); r->got.wall_size = 0; }
    }
    return 0;
}
static int cb_data(void *ctx, const uint8_t *p, uint32_t n) {
    rest_t *r = ctx;
    if (r->cur) { memcpy(r->cur->data + r->cur->size, p, n); r->cur->size += n; }
    else if (r->got.wall_data) { memcpy(r->got.wall_data + r->got.wall_size, p, n); r->got.wall_size += n; }
    return 0;
}
static int cb_end(void *ctx, const osc_rec_t *rec) { (void)ctx; (void)rec; return 0; }

static bool same_fs(const tfs_t *a, const tfs_t *b) {
    if (a->ndirs != b->ndirs || a->nfiles != b->nfiles) return false;
    for (int i = 0; i < a->ndirs; i++)
        if (a->dirs[i].id != b->dirs[i].id || a->dirs[i].parent != b->dirs[i].parent || strcmp(a->dirs[i].name, b->dirs[i].name)) return false;
    for (int i = 0; i < a->nfiles; i++) {
        const tfile_t *x = &a->files[i], *y = &b->files[i];
        if (x->dir != y->dir || strcmp(x->name, y->name) || strcmp(x->type, y->type) || x->size != y->size) return false;
        if (x->size && memcmp(x->data, y->data, x->size)) return false;
    }
    if (a->wall_kind != b->wall_kind || strcmp(a->wall_name, b->wall_name)) return false;
    if (a->wall_kind == 0 && a->wall_index != b->wall_index) return false;
    if (a->wall_kind == 1 && (a->wall_size != b->wall_size || memcmp(a->wall_data, b->wall_data, a->wall_size))) return false;
    return true;
}

/* parse `code` fed in pieces of `step` bytes (0 = random sizes) */
static int parse_code(const char *code, size_t n, size_t step, rest_t *r, char *err, size_t errcap) {
    static osc_parser_t p;
    osc_cb_t cb = { r, cb_entry, cb_rec, cb_data, cb_end };
    memset(r, 0, sizeof *r);
    osc_parser_init(&p, &cb);
    size_t o = 0;
    while (o < n) {
        size_t part = step ? step : 1 + rnd() % 70000;
        if (part > n - o) part = n - o;
        if (osc_parser_feed(&p, code + o, (uint32_t)part)) break;
        o += part;
    }
    int res = osc_parser_finish(&p);
    if (err) snprintf(err, errcap, "%s", res ? osc_parser_error(&p) : "");
    return res;
}

static void make_fs(tfs_t *t, int nfiles, uint32_t maxsize) {
    memset(t, 0, sizeof *t);
    t->ndirs = 3;
    t->dirs[0] = (tdir_t){ 1, 0, "home" };
    t->dirs[1] = (tdir_t){ 2, 1, "user" };
    t->dirs[2] = (tdir_t){ 3, 2, "My Documents (old)" };
    t->wall_kind = 0; t->wall_index = 2; strcpy(t->wall_name, "Sunset");
    static const char *names[] = { "notes", ".arcade_scores", "photo.jpg", "settings", "a b c", "x" };
    static const char *types[] = { "txt", "", "exe", "", "md", "bmp" };
    for (int i = 0; i < nfiles; i++) {
        tfile_t *f = &t->files[t->nfiles++];
        f->dir = (int)(rnd() % 4);
        snprintf(f->name, sizeof f->name, "%s%d", names[i % 6], i);
        strcpy(f->type, types[i % 6]);
        f->size = maxsize ? rnd() % maxsize : 0;
        if (i == 0) f->size = 0;              /* an empty file */
        f->data = malloc(f->size + 1);
        for (uint32_t k = 0; k < f->size; k++) f->data[k] = (i % 2) ? (uint8_t)rnd() : (uint8_t)("Hello\n\nworld\t"[k % 13]);
    }
}

/* strip every char in `drop` */
static char *strip(const char *s, size_t n, const char *drop, size_t *outn) {
    char *o = malloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) if (!strchr(drop, s[i]) || !s[i]) o[k++] = s[i];
    o[k] = 0;
    *outn = k;
    return o;
}

static void test_table(void) {
    const char *e = osc_chunk_table_check();
    CHECK(!e, "chunk table: %s", e ? e : "");
    char b[64];
    osc_key_encode("version", b, sizeof b, false);    CHECK(!strcmp(b, "13224"), "version = %s", b);
    osc_key_encode("version", b, sizeof b, true);     CHECK(!strcmp(b, "132\x1f" "24"), "version (device) wrong");
    osc_key_encode("files", b, sizeof b, false);      CHECK(!strcmp(b, "4351991"), "files = %s", b);
    osc_key_encode("files", b, sizeof b, true);       CHECK(!strcmp(b, "4351991"), "files (device) = %s", b);
    osc_key_encode("wallpaper", b, sizeof b, false);  CHECK(!strcmp(b, "91235422"), "wallpaper = %s", b);
    osc_key_encode("SystemData", b, sizeof b, false); CHECK(!strcmp(b, "3332198"), "SystemData = %s", b);
    osc_key_encode("SystemData", b, sizeof b, true);  CHECK(!strcmp(b, "333\x1f" "2198"), "SystemData (device) wrong");
    osc_key_encode("System", b, sizeof b, false);     CHECK(!strcmp(b, "333"), "System = %s", b);
    osc_key_encode("Data", b, sizeof b, false);       CHECK(!strcmp(b, "2198"), "Data = %s", b);
    osc_key_encode("vers", b, sizeof b, false);       CHECK(!strcmp(b, "132"), "vers = %s", b);
    osc_key_encode("ion", b, sizeof b, false);        CHECK(!strcmp(b, "24"), "ion = %s", b);
    char codes[64], texts[64];
    CHECK(osc_key_split("13224", 5, codes, sizeof codes, texts, sizeof texts) > 0 && !strcmp(codes, "132 24") && !strcmp(texts, "version"), "split 13224: %s / %s", codes, texts);
    CHECK(osc_key_split("3332198", 7, codes, sizeof codes, texts, sizeof texts) > 0 && !strcmp(codes, "333 2198") && !strcmp(texts, "SystemData"), "split 3332198");
    CHECK(osc_key_decode("1322", 4, b, sizeof b) < 0, "1322 must not decode");
    CHECK(osc_key_decode("1", 1, b, sizeof b) < 0, "1 must not decode");
    CHECK(osc_key_decode("", 0, b, sizeof b) < 0, "empty must not decode");
    CHECK(osc_key_decode("435199", 6, b, sizeof b) < 0, "435199 must not decode");
    /* no code is the start of another: brute force over every pair, and every
       concatenation of two codes decodes back to exactly those two texts */
    for (int i = 0; i < osc_chunk_count(); i++)
        for (int j = 0; j < osc_chunk_count(); j++) {
            char cat[64], want[64], got[64];
            snprintf(cat, sizeof cat, "%s%s", osc_chunk_get(i)->code, osc_chunk_get(j)->code);
            snprintf(want, sizeof want, "%s%s", osc_chunk_get(i)->text, osc_chunk_get(j)->text);
            int r = osc_key_split(cat, (int)strlen(cat), codes, sizeof codes, texts, sizeof texts);
            (void)got;
            if (r < 0 || strcmp(texts, want)) { CHECK(0, "pair %s + %s decodes to %s", osc_chunk_get(i)->code, osc_chunk_get(j)->code, texts); i = j = 9999; }
        }
    /* random words round-trip */
    const char alpha[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_ .-/";
    for (int t = 0; t < 20000; t++) {
        char w[24], enc[256], dec[64];
        int n = 1 + (int)(rnd() % 20);
        for (int i = 0; i < n; i++) w[i] = alpha[rnd() % (sizeof alpha - 1)];
        w[n] = 0;
        int el = osc_key_encode(w, enc, sizeof enc, (t & 1) != 0);
        int dl = el < 0 ? -1 : osc_key_decode(enc, el, dec, sizeof dec);
        if (el < 0 || dl < 0 || strcmp(dec, w)) { CHECK(0, "word %s round trip", w); break; }
    }
}

static void test_b62(void) {
    uint8_t in[1100], out[1100];
    char enc[2000];
    for (int t = 0; t < 3000; t++) {
        uint32_t n = rnd() % 1050;
        for (uint32_t i = 0; i < n; i++) in[i] = (t % 3 == 0) ? 0xFF : (t % 3 == 1) ? 0 : (uint8_t)rnd();
        uint32_t el = osc_b62_encode(in, n, enc);
        CHECK(el == osc_b62_len(n), "b62 len");
        int dl = osc_b62_decode(enc, el, out, sizeof out);
        if (dl != (int)n || memcmp(in, out, n)) { CHECK(0, "b62 round trip n=%u", n); break; }
    }
    CHECK(osc_b62_decode("zzzzzz", 6, out, 8) < 0, "overflowing group must be rejected");
    CHECK(osc_b62_decode("z", 1, out, 8) < 0, "1-digit tail must be rejected");
    CHECK(osc_b62_decode("zz", 2, out, 8) < 0, "2-digit tail over 255 must be rejected");
    CHECK(osc_crc32(0, "123456789", 9) == 0xCBF43926u, "crc32 check value");
}

static void test_placeholder(void) {
    char b[256];
    osc_placeholder(b, sizeof b, false);
    CHECK(!strcmp(b, "13224:0.0\n4351991:((-0))\n91235422:\n3332198:(())"), "placeholder text:\n%s", b);
    char why[96];
    CHECK(osc_validate(b, (uint32_t)strlen(b), why, sizeof why), "placeholder must be valid: %s", why);
    osc_placeholder(b, sizeof b, true);
    CHECK(!strcmp(b, "132\x1f" "24:0.0\n4351991:((-0))\n91235422:\n333\x1f" "2198:(())"), "placeholder (device)");
    CHECK(osc_validate(b, (uint32_t)strlen(b), why, sizeof why), "device placeholder must be valid: %s", why);
    /* pretty print of the placeholder */
    rest_t r;
    static osc_parser_t p;
    osc_parser_init(&p, NULL);
    osc_parser_feed(&p, b, (uint32_t)strlen(b));
    CHECK(!osc_parser_finish(&p), "placeholder parse");
    char pp[512];
    osc_pretty(pp, sizeof pp, &p, "");
    CHECK(!strcmp(pp, "{\n\"version\":\"0.0\"\n\"files\":((-0))\n\"wallpaper\":\"\"\nSystemData:((HiddenForPrettyPrint))\n}"), "placeholder pretty:\n%s", pp);
    (void)r;
    /* the user's illustrative example is not a real code */
    const char *ex = "13224:2.5\n4351991:((5))\n91235422:\n3332198:((A4-A1-12185Data4a148B26-4444a ...))";
    CHECK(!osc_validate(ex, (uint32_t)strlen(ex), why, sizeof why), "illustrative example must not validate");
    /* garbage */
    const char *bad[] = { "", "hello", "13224:2.5", "13224:2.5\n4351991:((5))\n91235422:\n", "1322:0.0",
                          "13224:0.0\n4351991:((-1))\n91235422:\n3332198:(())",
                          "13224:0.0\n4351991:((-0))\n91235422:\n3332198:(()",
                          "13224:0.0\n4351991:((1))\n91235422:\n3332198:(())",
                          "13224:0.0\n13224:0.0\n4351991:((-0))\n91235422:\n3332198:(())",
                          "13224:0..0\n4351991:((-0))\n91235422:\n3332198:(())",
                          "13224:0.0\n4351991:((-0))\n91235422:5\n3332198:(())",
                          "13224:0.0\n4351991:((-0))\n91235422:\n3332198:((x))" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(!osc_validate(bad[i], (uint32_t)strlen(bad[i]), why, sizeof why), "garbage %zu must not validate", i);
    /* joined lines and empty lines */
    const char *ok[] = { "13224:0.04351991:((-0))91235422:3332198:(())",
                         "13224:0.0\n\n4351991:((-0))\n\n91235422:\n\n3332198:(())\n",
                         "\n  13224 : 0.0\r\n4351991:((-0))\r\n91235422:\r\n3332198:(())\r\n",
                         "3332198:(())\n91235422:\n4351991:((-0))\n13224:0.0",
                         "132\n24:0.0\n4351991:((-0))\n91235422:\n3332198:(\n())" };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
        CHECK(osc_validate(ok[i], (uint32_t)strlen(ok[i]), why, sizeof why), "variant %zu must validate: %s", i, why);
    /* an empty line splits a key */
    const char *split = "132\n\n24:0.0\n4351991:((-0))\n91235422:\n3332198:(())";
    CHECK(!osc_validate(split, (uint32_t)strlen(split), why, sizeof why), "key split by empty line must fail");
}

static void test_roundtrip(void) {
    for (int round = 0; round < 40; round++) {
        tfs_t t;
        make_fs(&t, 1 + round % 30, round < 5 ? 40 : 20000);
        if (round % 4 == 3) {
            t.wall_kind = 1;
            strcpy(t.wall_name, "/home/user/Pictures/me.bmp");
            t.wall_size = rnd() % 70000;
            t.wall_data = malloc(t.wall_size + 1);
            for (uint32_t i = 0; i < t.wall_size; i++) t.wall_data[i] = (uint8_t)rnd();
        }
        buf_t code = {0};
        build_code(&t, &code, true);
        rest_t r;
        char err[128];
        int res = parse_code(code.s, code.n, 0, &r, err, sizeof err);
        CHECK(res == 0, "round %d: parse failed: %s", round, err);
        CHECK(res != 0 || same_fs(&t, &r.got), "round %d: restored data differs", round);
        CHECK(!strcmp(r.version, "2.5"), "version value");
        tfs_free(&r.got);
        /* the copy form (hidden separators removed) parses the same */
        size_t cn;
        char *copy = strip(code.s, code.n, "\x1f", &cn);
        CHECK(memchr(copy, 0x1f, cn) == NULL, "copy form has no hidden separators");
        res = parse_code(copy, cn, 0, &r, err, sizeof err);
        CHECK(res == 0 && same_fs(&t, &r.got), "round %d: copy form: %s", round, err);
        tfs_free(&r.got);
        /* every character class is allowed: digits, : . ( ) - and letters inside SystemData only */
        const char *sd = strstr(copy, "((B");
        bool clean = sd != NULL;
        for (const char *q = copy; q < sd && clean; q++)
            if (!(strchr("0123456789:.()-\n", *q))) clean = false;
        CHECK(clean, "round %d: head has only numbers and marks", round);
        /* joined into one line */
        size_t jn;
        char *joined = strip(copy, cn, "\n", &jn);
        res = parse_code(joined, jn, 0, &r, err, sizeof err);
        CHECK(res == 0 && same_fs(&t, &r.got), "round %d: joined: %s", round, err);
        tfs_free(&r.got);
        /* wrapped: SystemData broken into raw lines of 76 characters */
        buf_t wrapped = {0};
        for (size_t i = 0; i < cn; i += 76) {
            size_t k = cn - i < 76 ? cn - i : 76;
            buf_add(&wrapped, copy + i, k);
            buf_add(&wrapped, "\n", 1);
        }
        res = parse_code(wrapped.s, wrapped.n, 0, &r, err, sizeof err);
        CHECK(res == 0 && same_fs(&t, &r.got), "round %d: wrapped: %s", round, err);
        tfs_free(&r.got);
        /* byte by byte */
        if (round < 3) {
            res = parse_code(code.s, code.n, 1, &r, err, sizeof err);
            CHECK(res == 0 && same_fs(&t, &r.got), "round %d: byte by byte: %s", round, err);
            tfs_free(&r.got);
        }
        /* damage: change one SystemData character -> rejected */
        if (code.n > 60) {
            char *dmg = malloc(code.n + 1);
            memcpy(dmg, code.s, code.n + 1);
            char *inner = strstr(dmg, "((B") + 3;
            size_t span = (size_t)(dmg + code.n - 2 - inner);
            size_t pos = (size_t)(inner - dmg) + rnd() % span;
            char old = dmg[pos];
            dmg[pos] = old == '7' ? '8' : '7';
            if (dmg[pos] != old) {
                res = parse_code(dmg, code.n, 0, &r, err, sizeof err);
                CHECK(res != 0, "round %d: damaged code at %zu accepted", round, pos);
                tfs_free(&r.got);
            }
            /* a missing character */
            memmove(dmg + pos, dmg + pos + 1, code.n - pos);
            memcpy(dmg, code.s, pos);
            res = parse_code(dmg, code.n - 1, 0, &r, err, sizeof err);
            CHECK(res != 0, "round %d: shortened code accepted", round);
            tfs_free(&r.got);
            free(dmg);
        }
        /* pretty print */
        static osc_parser_t p;
        osc_parser_init(&p, NULL);
        osc_parser_feed(&p, code.s, (uint32_t)code.n);
        osc_parser_finish(&p);
        char pp[512], want[512];
        osc_pretty(pp, sizeof pp, &p, t.wall_kind ? "Custom" : "/system/wallpapers/Sunset.bmp");
        snprintf(want, sizeof want, "{\n\"version\":\"2.5\"\n\"files\":((%d))\n\"wallpaper\":\"%s\"\nSystemData:((HiddenForPrettyPrint))\n}",
                 t.nfiles, t.wall_kind ? "Custom" : "/system/wallpapers/Sunset.bmp");
        CHECK(!strcmp(pp, want), "pretty:\n%s", pp);
        if (round == 7) printf("example code (copy form, first 300 chars):\n%.300s...\n\npretty print:\n%s\n\n", copy, pp);
        free(joined); free(copy); free(code.s); free(wrapped.s);
        tfs_free(&t);
    }
}

/* ---- huge code: streamed writer -> streamed parser, nothing held but the code ------- */
typedef struct { uint32_t crc; uint64_t bytes; } hsum_t;
static int big_data(void *ctx, const uint8_t *p, uint32_t n) { hsum_t *h = ctx; h->crc = osc_crc32(h->crc, p, n); h->bytes += n; return 0; }
static int big_entry(void *c, int k, const char *v, int n) { (void)c; (void)k; (void)v; (void)n; return 0; }
static int big_rec(void *c, const osc_rec_t *r) { (void)c; (void)r; return 0; }

static void test_big(uint32_t mb) {
    uint32_t size = mb << 20;
    uint8_t *data = malloc(size);
    for (uint32_t i = 0; i < size; i++) data[i] = (uint8_t)rnd();
    buf_t code = {0};
    char head[256];
    int hl = osc_head(head, sizeof head, "2.5", 1, true);
    buf_add(&code, head, (size_t)hl);
    clock_t t0 = clock();
    osc_writer_t w;
    osc_w_init(&w, sink, &code);
    osc_w_record(&w, 'B', 1); osc_w_num(&w, 2); osc_w_num(&w, 0); osc_w_num(&w, 1); osc_w_num(&w, 0); osc_w_num(&w, size);
    osc_w_record(&w, 'W', 0); osc_w_num(&w, 0); osc_w_bytes(&w, "Nova", 4);
    osc_w_record(&w, 'F', 0); osc_w_bytes(&w, "big", 3); osc_w_bytes(&w, "bin", 3); osc_w_num(&w, size);
    osc_w_data_begin(&w);
    for (uint32_t o = 0; o < size; o += 1 << 20) osc_w_data_part(&w, data + o, 1 << 20);
    osc_w_data_end(&w);
    osc_w_end(&w);
    buf_add(&code, OSC_TAIL, 2);
    double te = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("big: %u MB of data -> %zu MB code (%.2f chars/byte), encode %.2f s\n", mb, code.n >> 20, (double)code.n / size, te);
    CHECK(code.n <= OSC_MAX_CODE || mb > 170, "code size");
    hsum_t h = {0, 0};
    static osc_parser_t p;
    osc_cb_t cb = { &h, big_entry, big_rec, big_data, NULL };
    osc_parser_init(&p, &cb);
    t0 = clock();
    for (size_t o = 0; o < code.n; o += 1 << 20) {
        size_t part = code.n - o < (1u << 20) ? code.n - o : (1u << 20);
        if (osc_parser_feed(&p, code.s + o, (uint32_t)part)) break;
    }
    int res = osc_parser_finish(&p);
    double td = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("big: decode %.2f s (%.0f MB/s of code)\n", td, (code.n / 1048576.0) / td);
    CHECK(res == 0, "big parse: %s", osc_parser_error(&p));
    CHECK(h.bytes == size && h.crc == osc_crc32(0, data, size), "big data round trip");
    free(data);
    free(code.s);
}

int main(int argc, char **argv) {
    test_table();
    test_b62();
    test_placeholder();
    test_roundtrip();
    test_big(argc > 1 && !strcmp(argv[1], "big") ? 170 : 12);
    printf("%d checks, %d failed\n", checks, failures);
    if (!failures) printf("ALL PASSED\n");
    return failures ? 1 : 0;
}
