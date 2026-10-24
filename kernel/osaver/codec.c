/*
 * osaver/codec.c - the OSaver number language: chunk table, base 62, CRC-32,
 * the streaming writer and the streaming parser. See codec.h for the format.
 *
 * Host-testable: tools/osaver_codec_test.c compiles this file unchanged.
 */
#include "osaver/codec.h"
#include <string.h>

/* ======================================================================================
 * Chunk table. Prefix-free: no number is the beginning of another one.
 *
 *   given by the user   132 vers, 24 ion, 4351991 files, 91235422 wallpaper,
 *                       333 System, 2198 Data
 *   everything else is three digits starting with 5 or 6, a first digit no given
 *   number uses (they start with 1, 2, 3, 4 and 9):
 *     510-535 a-z    540-565 A-Z    570-579 0-9    580 _  581 space  582 .  583 -  584 /
 *     600-     common chunks (set, ting, brow, ser, ...)
 *   Codes starting with 0, 7 and 8 are free for later.
 * ==================================================================================== */
static const osc_chunk_t chunks[] = {
    /* the user's numbers */
    { "132", "vers" }, { "24", "ion" },
    { "4351991", "files" }, { "91235422", "wallpaper" },
    { "333", "System" }, { "2198", "Data" },
    /* letters */
    { "510", "a" }, { "511", "b" }, { "512", "c" }, { "513", "d" }, { "514", "e" },
    { "515", "f" }, { "516", "g" }, { "517", "h" }, { "518", "i" }, { "519", "j" },
    { "520", "k" }, { "521", "l" }, { "522", "m" }, { "523", "n" }, { "524", "o" },
    { "525", "p" }, { "526", "q" }, { "527", "r" }, { "528", "s" }, { "529", "t" },
    { "530", "u" }, { "531", "v" }, { "532", "w" }, { "533", "x" }, { "534", "y" },
    { "535", "z" },
    { "540", "A" }, { "541", "B" }, { "542", "C" }, { "543", "D" }, { "544", "E" },
    { "545", "F" }, { "546", "G" }, { "547", "H" }, { "548", "I" }, { "549", "J" },
    { "550", "K" }, { "551", "L" }, { "552", "M" }, { "553", "N" }, { "554", "O" },
    { "555", "P" }, { "556", "Q" }, { "557", "R" }, { "558", "S" }, { "559", "T" },
    { "560", "U" }, { "561", "V" }, { "562", "W" }, { "563", "X" }, { "564", "Y" },
    { "565", "Z" },
    { "570", "0" }, { "571", "1" }, { "572", "2" }, { "573", "3" }, { "574", "4" },
    { "575", "5" }, { "576", "6" }, { "577", "7" }, { "578", "8" }, { "579", "9" },
    { "580", "_" }, { "581", " " }, { "582", "." }, { "583", "-" }, { "584", "/" },
    /* common chunks */
    { "600", "set" }, { "601", "ting" }, { "602", "brow" }, { "603", "ser" },
    { "604", "his" }, { "605", "tory" }, { "606", "boot" }, { "607", "id" },
    { "608", "app" }, { "609", "save" }, { "610", "wall" }, { "611", "paper" },
    { "612", "file" }, { "613", "name" }, { "614", "type" }, { "615", "size" },
    { "616", "text" }, { "617", "time" }, { "618", "search" }, { "619", "ing" },
    { "620", "the" }, { "621", "data" }, { "622", "con" }, { "623", "fig" },
    { "624", "er" }, { "625", "loc" }, { "626", "ation" }, { "627", "sys" },
    { "628", "tem" }, { "629", "user" },
};
#define NCHUNKS ((int)(sizeof chunks / sizeof chunks[0]))

int osc_chunk_count(void) { return NCHUNKS; }
const osc_chunk_t *osc_chunk_get(int i) { return (i >= 0 && i < NCHUNKS) ? &chunks[i] : NULL; }

const char *osc_chunk_table_check(void) {
    for (int i = 0; i < NCHUNKS; i++) {
        const char *a = chunks[i].code;
        if (!*a || !*chunks[i].text) return "empty code or text";
        for (const char *q = a; *q; q++)
            if (*q < '0' || *q > '9') return "code with a non-digit";
        size_t la = strlen(a);
        for (int j = 0; j < NCHUNKS; j++) {
            if (i == j) continue;
            const char *b = chunks[j].code;
            size_t lb = strlen(b);
            if (la <= lb && !memcmp(a, b, la)) return "a code is the beginning of another code";
            if (!strcmp(chunks[i].text, chunks[j].text)) return "two codes for the same text";
        }
    }
    return NULL;
}

static int put(char *out, int cap, int len, const char *s, int n) {
    if (len < 0) return -1;
    if (len + n >= cap) return -1;
    memcpy(out + len, s, (size_t)n);
    out[len + n] = 0;
    return len + n;
}

int osc_key_encode(const char *word, char *out, int cap, bool hidden) {
    int len = 0, n = (int)strlen(word);
    if (cap <= 0) return -1;
    out[0] = 0;
    for (int i = 0; i < n; ) {
        int best = -1, bl = 0;
        for (int c = 0; c < NCHUNKS; c++) {
            int tl = (int)strlen(chunks[c].text);
            if (tl > bl && tl <= n - i && !memcmp(word + i, chunks[c].text, (size_t)tl)) { best = c; bl = tl; }
        }
        if (best < 0) return -1;
        if (i && hidden) len = put(out, cap, len, "\x1f", 1);
        len = put(out, cap, len, chunks[best].code, (int)strlen(chunks[best].code));
        if (len < 0) return -1;
        i += bl;
    }
    return len;
}

/* Find the chunk whose code equals digits[0..n). Because the table is prefix-free,
   reading digits one at a time and stopping at the first exact match is the only way
   to split. */
static int chunk_exact(const char *d, int n) {
    for (int c = 0; c < NCHUNKS; c++)
        if ((int)strlen(chunks[c].code) == n && !memcmp(chunks[c].code, d, (size_t)n)) return c;
    return -1;
}

static bool chunk_prefix(const char *d, int n) {
    for (int c = 0; c < NCHUNKS; c++)
        if ((int)strlen(chunks[c].code) > n && !memcmp(chunks[c].code, d, (size_t)n)) return true;
    return false;
}

int osc_key_split(const char *digits, int n, char *codes, int ccap, char *texts, int tcap) {
    char cur[16];
    int cl = 0, clen = 0, tlen = 0, nchunks = 0;
    if (codes && ccap > 0) codes[0] = 0;
    if (texts && tcap > 0) texts[0] = 0;
    for (int i = 0; i < n; i++) {
        char ch = digits[i];
        if (ch == OSC_HS || ch == ' ') continue;
        if (ch < '0' || ch > '9') return -1;
        if (cl >= (int)sizeof cur) return -1;
        cur[cl++] = ch;
        int c = chunk_exact(cur, cl);
        if (c >= 0) {
            if (codes) {
                if (nchunks) clen = put(codes, ccap, clen, " ", 1);
                clen = put(codes, ccap, clen, chunks[c].code, (int)strlen(chunks[c].code));
            }
            if (texts) tlen = put(texts, tcap, tlen, chunks[c].text, (int)strlen(chunks[c].text));
            if (clen < 0 || tlen < 0) return -1;
            nchunks++;
            cl = 0;
        } else if (!chunk_prefix(cur, cl)) {
            return -1;
        }
    }
    if (cl) return -1;          /* digits left over: not a whole chunk */
    return nchunks ? tlen : -1;
}

int osc_key_decode(const char *digits, int n, char *out, int cap) {
    return osc_key_split(digits, n, NULL, 0, out, cap);
}

static const char *const key_names[OSC_K_COUNT] = { "version", "files", "wallpaper", "SystemData" };

const char *osc_key_name(int k) { return (k >= 0 && k < OSC_K_COUNT) ? key_names[k] : "?"; }

int osc_key_lookup(const char *word) {
    for (int k = 0; k < OSC_K_COUNT; k++)
        if (!strcmp(word, key_names[k])) return k;
    return -1;
}

/* ======================================================================================
 * Base 62 and CRC-32
 * ==================================================================================== */
static const char b62[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
static const uint8_t tail_digits[4] = { 0, 2, 3, 5 };   /* tail bytes -> digits */

int osc_b62_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c >= 'a' && c <= 'z') return c - 'a' + 36;
    return -1;
}

uint32_t osc_b62_len(uint32_t n) { return (n / 4) * 6 + tail_digits[n % 4]; }

static void b62_group(uint32_t v, int digits, char *out) {
    for (int i = digits - 1; i >= 0; i--) { out[i] = b62[v % 62]; v /= 62; }
}

uint32_t osc_b62_encode(const uint8_t *in, uint32_t n, char *out) {
    uint32_t o = 0, i = 0;
    for (; i + 4 <= n; i += 4, o += 6)
        b62_group((uint32_t)in[i] << 24 | (uint32_t)in[i + 1] << 16 | (uint32_t)in[i + 2] << 8 | in[i + 3], 6, out + o);
    uint32_t t = n - i;
    if (t) {
        uint32_t v = 0;
        for (uint32_t k = 0; k < t; k++) v = v << 8 | in[i + k];
        b62_group(v, tail_digits[t], out + o);
        o += tail_digits[t];
    }
    return o;
}

/* Decode one group of `nd` digits into `nb` bytes. Returns false on a bad digit or a
   value that does not fit (every code has exactly one spelling). */
static bool b62_ungroup(const char *g, int nd, int nb, uint8_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < nd; i++) {
        int d = osc_b62_digit((unsigned char)g[i]);
        if (d < 0) return false;
        v = v * 62 + (uint64_t)d;
    }
    if (v >> (8 * nb)) return false;
    for (int i = nb - 1; i >= 0; i--) { out[i] = (uint8_t)v; v >>= 8; }
    return true;
}

static int tail_bytes(int nd) {   /* digits in a tail -> bytes, -1 if impossible */
    switch (nd) { case 0: return 0; case 2: return 1; case 3: return 2; case 5: return 3; default: return -1; }
}

int osc_b62_decode(const char *in, uint32_t n, uint8_t *out, uint32_t cap) {
    uint32_t o = 0, i = 0;
    for (; i + 6 <= n; i += 6, o += 4) {
        if (o + 4 > cap || !b62_ungroup(in + i, 6, 4, out + o)) return -1;
    }
    int tb = tail_bytes((int)(n - i));
    if (tb < 0 || o + (uint32_t)tb > cap) return -1;
    if (tb && !b62_ungroup(in + i, (int)(n - i), tb, out + o)) return -1;
    return (int)(o + (uint32_t)tb);
}

static uint32_t crc_table[256];
static bool crc_ready;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    crc_ready = true;
}

uint32_t osc_crc32(uint32_t crc, const void *p, uint32_t n) {
    if (!crc_ready) crc_init();
    const uint8_t *b = p;
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *b++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ======================================================================================
 * Writer
 * ==================================================================================== */
static void w_emit(osc_writer_t *w, const char *s, uint32_t n) {
    if (w->err || !n) return;
    w->crc = osc_crc32(w->crc, s, n);
    w->total += n;
    if (w->write && w->write(w->ctx, s, n)) w->err = -1;
}

static int utoa10(uint32_t v, char *out) {
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0;
    return n;
}

void osc_w_init(osc_writer_t *w, int (*write)(void *, const char *, uint32_t), void *ctx) {
    memset(w, 0, sizeof *w);
    w->write = write;
    w->ctx = ctx;
    w->first = true;
}

void osc_w_record(osc_writer_t *w, char tag, uint32_t first) {
    char b[16];
    int n = 0;
    if (!w->first) b[n++] = '-';
    w->first = false;
    b[n++] = tag;
    n += utoa10(first, b + n);
    w_emit(w, b, (uint32_t)n);
}

void osc_w_num(osc_writer_t *w, uint32_t v) {
    char b[16];
    b[0] = '-';
    int n = 1 + utoa10(v, b + 1);
    w_emit(w, b, (uint32_t)n);
}

void osc_w_data_begin(osc_writer_t *w) {
    w->ncarry = 0;
    w_emit(w, "-", 1);
}

void osc_w_data_part(osc_writer_t *w, const void *p, uint32_t n) {
    const uint8_t *b = p;
    char out[6 * 256];
    while (n && w->ncarry) {                 /* finish a pending group first */
        w->carry[w->ncarry++] = *b++;
        n--;
        if (w->ncarry == 4) { osc_b62_encode(w->carry, 4, out); w_emit(w, out, 6); w->ncarry = 0; }
    }
    while (n >= 4) {
        uint32_t take = n & ~3u;
        if (take > 4 * 256) take = 4 * 256;
        uint32_t o = osc_b62_encode(b, take, out);
        w_emit(w, out, o);
        b += take;
        n -= take;
    }
    while (n) { w->carry[w->ncarry++] = *b++; n--; }
}

void osc_w_data_end(osc_writer_t *w) {
    char out[8];
    if (w->ncarry) {
        uint32_t o = osc_b62_encode(w->carry, (uint32_t)w->ncarry, out);
        w_emit(w, out, o);
    }
    w->ncarry = 0;
}

void osc_w_bytes(osc_writer_t *w, const void *p, uint32_t n) {
    osc_w_data_begin(w);
    osc_w_data_part(w, p, n);
    osc_w_data_end(w);
}

void osc_w_end(osc_writer_t *w) {
    char b[16];
    w_emit(w, "-", 1);
    uint32_t crc = w->crc;                   /* everything before the 'E' */
    b[0] = 'E';
    int n = 1 + utoa10(crc, b + 1);
    w_emit(w, b, (uint32_t)n);
}

/* ======================================================================================
 * Head / placeholder / pretty print
 * ==================================================================================== */
static int put_s(char *out, int cap, int len, const char *s) { return put(out, cap, len, s, (int)strlen(s)); }

static int put_key(char *out, int cap, int len, int k, bool hidden) {
    char kb[64];
    if (osc_key_encode(key_names[k], kb, sizeof kb, hidden) < 0) return -1;
    len = put_s(out, cap, len, kb);
    return put_s(out, cap, len, ":");
}

int osc_head(char *out, int cap, const char *version, long files, bool hidden) {
    char num[16];
    int len = 0;
    if (cap <= 0) return -1;
    out[0] = 0;
    len = put_key(out, cap, len, OSC_K_VERSION, hidden);
    len = put_s(out, cap, len, version);
    len = put_s(out, cap, len, "\n");
    len = put_key(out, cap, len, OSC_K_FILES, hidden);
    if (files < 0) len = put_s(out, cap, len, "((-0))");
    else {
        utoa10((uint32_t)files, num);
        len = put_s(out, cap, len, "((");
        len = put_s(out, cap, len, num);
        len = put_s(out, cap, len, "))");
    }
    len = put_s(out, cap, len, "\n");
    len = put_key(out, cap, len, OSC_K_WALLPAPER, hidden);
    len = put_s(out, cap, len, "\n");
    len = put_key(out, cap, len, OSC_K_SYSDATA, hidden);
    len = put_s(out, cap, len, "((");
    return len;
}

int osc_placeholder(char *out, int cap, bool hidden) {
    int len = osc_head(out, cap, "0.0", -1, hidden);
    return put_s(out, cap, len, OSC_TAIL);
}

int osc_pretty(char *out, int cap, const osc_parser_t *p, const char *wall_text) {
    char name[OSC_K_COUNT][64];
    for (int k = 0; k < OSC_K_COUNT; k++)
        if (osc_key_decode(p->keydig[k], (int)strlen(p->keydig[k]), name[k], sizeof name[k]) < 0) return -1;
    int len = 0;
    if (cap <= 0) return -1;
    out[0] = 0;
    len = put_s(out, cap, len, "{\n\"");
    len = put_s(out, cap, len, name[OSC_K_VERSION]);
    len = put_s(out, cap, len, "\":\"");
    len = put_s(out, cap, len, p->version);
    len = put_s(out, cap, len, "\"\n\"");
    len = put_s(out, cap, len, name[OSC_K_FILES]);
    len = put_s(out, cap, len, "\":((");
    len = put_s(out, cap, len, p->filesval);
    len = put_s(out, cap, len, "))\n\"");
    len = put_s(out, cap, len, name[OSC_K_WALLPAPER]);
    len = put_s(out, cap, len, "\":\"");
    len = put_s(out, cap, len, wall_text ? wall_text : "");
    len = put_s(out, cap, len, "\"\n");
    len = put_s(out, cap, len, name[OSC_K_SYSDATA]);
    len = put_s(out, cap, len, ":((HiddenForPrettyPrint))\n}");
    return len;
}

/* ======================================================================================
 * Parser
 * ==================================================================================== */
enum { H_KEY, H_VERSION, H_FILES, H_SYSOPEN, H_INNER, H_SYSCLOSE, H_ERR };
enum { I_TAG, I_FIELD };

static int fail(osc_parser_t *p, const char *msg) {
    if (!p->err) {
        p->err = -1;
        size_t n = strlen(msg);
        if (n >= sizeof p->errmsg) n = sizeof p->errmsg - 1;
        memcpy(p->errmsg, msg, n);
        p->errmsg[n] = 0;
    }
    p->st = H_ERR;
    return -1;
}

void osc_parser_init(osc_parser_t *p, const osc_cb_t *cb) {
    memset(p, 0, sizeof *p);
    if (cb) p->cb = *cb;
    p->st = H_KEY;
    if (!crc_ready) crc_init();
}

const char *osc_parser_error(const osc_parser_t *p) { return p->err ? p->errmsg : NULL; }

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* A version value: digits ('.' digits)* */
static bool version_ok(const char *v, int n) {
    if (n <= 0 || n >= 40 || !is_digit(v[0]) || !is_digit(v[n - 1])) return false;
    for (int i = 0; i < n; i++) {
        if (v[i] == '.') { if (v[i + 1] == '.') return false; }
        else if (!is_digit(v[i])) return false;
    }
    return true;
}

/* Which known, not yet seen key do these digits spell? -1 if none. */
static int key_of(osc_parser_t *p, const char *d, int n) {
    char word[64];
    if (osc_key_decode(d, n, word, sizeof word) < 0) return -1;
    int k = osc_key_lookup(word);
    if (k < 0 || p->seen[k]) return -1;
    return k;
}

static int key_done(osc_parser_t *p, const char *d, int n) {
    char word[64];
    if (n <= 0) return fail(p, "a ':' without a key in front of it");
    if (osc_key_decode(d, n, word, sizeof word) < 0) return fail(p, "a key that is not made of whole chunks");
    int k = osc_key_lookup(word);
    if (k < 0) return fail(p, "an unknown key");
    if (p->seen[k]) return fail(p, "a key that appears twice");
    if (n >= (int)sizeof p->keydig[k]) return fail(p, "a key that is too long");
    p->seen[k] = true;
    memcpy(p->keydig[k], d, (size_t)n);
    p->keydig[k][n] = 0;
    p->klen = 0;
    switch (k) {
    case OSC_K_VERSION: p->st = H_VERSION; p->vlen = 0; break;
    case OSC_K_FILES:   p->st = H_FILES; p->sub = 0; p->flen = 0; break;
    case OSC_K_WALLPAPER:
        p->st = H_KEY;
        if (p->cb.entry && p->cb.entry(p->cb.ctx, k, "", 0)) return fail(p, "stopped");
        break;
    case OSC_K_SYSDATA: p->st = H_SYSOPEN; p->sub = 0; break;
    }
    return 0;
}

static int version_done(osc_parser_t *p, const char *v, int n) {
    if (!version_ok(v, n)) return fail(p, "a bad version number");
    memcpy(p->version, v, (size_t)n);
    p->version[n] = 0;
    p->st = H_KEY;
    if (p->cb.entry && p->cb.entry(p->cb.ctx, OSC_K_VERSION, v, n)) return fail(p, "stopped");
    return 0;
}

/* ':' after a version value: the buffer holds the value followed by the next key
   (no empty line in between). Take the shortest valid value whose remainder spells a
   known key. */
static int version_split(osc_parser_t *p) {
    for (int i = 1; i < p->vlen; i++) {
        if (!version_ok(p->vbuf, i)) continue;
        bool digits = true;
        for (int j = i; j < p->vlen; j++) if (!is_digit(p->vbuf[j])) { digits = false; break; }
        if (!digits) continue;
        if (key_of(p, p->vbuf + i, p->vlen - i) < 0) continue;
        char key[80];
        int kn = p->vlen - i;
        memcpy(key, p->vbuf + i, (size_t)kn);
        if (version_done(p, p->vbuf, i)) return -1;
        return key_done(p, key, kn);
    }
    return fail(p, "a bad version number");
}

/* ---- inner (SystemData) ------------------------------------------------------------- */
/* Field types: 'n' number, 's' string, 'd' data, 0 = record complete. */
static char field_type(const osc_parser_t *p, int fi) {
    const osc_rec_t *r = &p->rec;
    switch (r->tag) {
    case 'B': return fi < 6 ? 'n' : 0;
    case 'E': return fi < 1 ? 'n' : 0;
    case 'D': return fi < 2 ? 'n' : fi == 2 ? 's' : 0;
    case 'F': { static const char f[] = "nssnd"; return fi < 5 ? f[fi] : 0; }
    case 'W':
        if (fi == 0) return 'n';
        if (r->num[0] == 0) { static const char w0[] = "nns"; return fi < 3 ? w0[fi] : 0; }
        { static const char w1[] = "nsnd"; return fi < 4 ? w1[fi] : 0; }
    }
    return 0;
}

static bool has_data(const osc_parser_t *p) {
    return p->rec.tag == 'F' || (p->rec.tag == 'W' && p->rec.num[0] == 1);
}

static int flush_data(osc_parser_t *p) {
    if (p->olen && p->cb.data && p->cb.data(p->cb.ctx, p->obuf, p->olen)) return fail(p, "stopped");
    p->olen = 0;
    return 0;
}

static int start_field(osc_parser_t *p) {
    char t = field_type(p, p->fi);
    p->numv = 0;
    p->ndig = 0;
    p->sblen = 0;
    p->ng = 0;
    p->olen = 0;
    p->dgot = 0;
    if (t == 'd' && p->cb.rec && p->cb.rec(p->cb.ctx, &p->rec)) return fail(p, "stopped");
    return 0;
}

static int record_end(osc_parser_t *p) {
    osc_rec_t *r = &p->rec;
    if (r->tag == 'B' && r->num[2] > 1) return fail(p, "a bad boot record");
    if (r->tag == 'E' && r->num[0] != p->crc_at_e) return fail(p, "a damaged code (the check number does not match)");
    if (!has_data(p) && p->cb.rec && p->cb.rec(p->cb.ctx, r)) return fail(p, "stopped");
    if (p->cb.rec_end && p->cb.rec_end(p->cb.ctx, r)) return fail(p, "stopped");
    if (r->tag == 'F') p->nfile++;
    if (r->tag == 'D') p->ndir++;
    if (r->tag == 'E') p->got_e = true;
    p->nrec++;
    p->ist = I_TAG;
    return 0;
}

static int end_field(osc_parser_t *p) {
    osc_rec_t *r = &p->rec;
    switch (field_type(p, p->fi)) {
    case 'n':
        if (!p->ndig) return fail(p, "a missing number in SystemData");
        r->num[r->nnum++] = (uint32_t)p->numv;
        if ((r->tag == 'F' && p->fi == 3) || (r->tag == 'W' && p->fi == 2)) r->size = (uint32_t)p->numv;
        if (r->tag == 'W' && p->fi == 0 && p->numv > 1) return fail(p, "an unknown wallpaper record");
        break;
    case 's': {
        int n = osc_b62_decode(p->sbuf, (uint32_t)p->sblen, (uint8_t *)r->str[r->nstr], OSC_SMAX);
        if (n < 0) return fail(p, "a damaged name in SystemData");
        r->str[r->nstr][n] = 0;
        r->slen[r->nstr] = (uint16_t)n;
        r->nstr++;
        break;
    }
    case 'd': {
        int tb = tail_bytes(p->ng);
        if (tb < 0 || p->olen + (uint32_t)tb > sizeof p->obuf + 0) return fail(p, "damaged data in SystemData");
        if (tb && !b62_ungroup(p->grp, p->ng, tb, p->obuf + p->olen)) return fail(p, "damaged data in SystemData");
        p->olen += (uint32_t)tb;
        p->dgot += (uint32_t)tb;
        if (p->dgot != r->size) return fail(p, "data of the wrong size in SystemData");
        if (flush_data(p)) return -1;
        break;
    }
    default:
        return fail(p, "too many fields in a SystemData record");
    }
    p->fi++;
    if (!field_type(p, p->fi)) return record_end(p);
    return start_field(p);
}

static int start_record(osc_parser_t *p, char tag) {
    if (tag != 'B' && tag != 'W' && tag != 'D' && tag != 'F' && tag != 'E')
        return fail(p, "an unknown record in SystemData");
    if (p->got_e) return fail(p, "something after the end of SystemData");
    if (p->nrec == 0 && tag != 'B') return fail(p, "SystemData that does not start with its boot record");
    if (p->nrec > 0 && tag == 'B') return fail(p, "two boot records");
    if (tag == 'W') { if (p->got_w) return fail(p, "two wallpaper records"); p->got_w = true; }
    memset(&p->rec, 0, sizeof p->rec);
    p->rec.tag = tag;
    p->fi = 0;
    p->ist = I_FIELD;
    return start_field(p);
}

static int inner_feed(osc_parser_t *p, char c) {
    if (p->ist == I_TAG && c == 'E') p->crc_at_e = p->icrc;
    {
        uint32_t crc = ~p->icrc;
        crc = crc_table[(crc ^ (uint8_t)c) & 0xFF] ^ (crc >> 8);
        p->icrc = ~crc;
    }
    p->inner_len++;
    if (p->ist == I_TAG) {
        if (c == '-') return fail(p, "an empty record in SystemData");
        return start_record(p, c);
    }
    if (c == '-') return end_field(p);
    switch (field_type(p, p->fi)) {
    case 'n':
        if (!is_digit(c) || p->ndig >= 10) return fail(p, "a bad number in SystemData");
        p->numv = p->numv * 10 + (uint64_t)(c - '0');
        p->ndig++;
        if (p->numv > 0xFFFFFFFFu) return fail(p, "a bad number in SystemData");
        return 0;
    case 's':
        if (osc_b62_digit((unsigned char)c) < 0 || p->sblen >= (int)sizeof p->sbuf - 1)
            return fail(p, "a bad name in SystemData");
        p->sbuf[p->sblen++] = c;
        return 0;
    case 'd':
        p->grp[p->ng++] = c;
        if (p->ng == 6) {
            if (!b62_ungroup(p->grp, 6, 4, p->obuf + p->olen)) return fail(p, "damaged data in SystemData");
            p->olen += 4;
            p->dgot += 4;
            p->ng = 0;
            if (p->dgot > p->rec.size) return fail(p, "data of the wrong size in SystemData");
            if (p->olen + 4 > sizeof p->obuf && flush_data(p)) return -1;
        }
        return 0;
    }
    return fail(p, "a bad SystemData record");
}

static int inner_finish(osc_parser_t *p) {
    if (p->inner_len == 0) return 0;               /* "(())": nothing saved */
    if (p->ist == I_FIELD) {
        if (end_field(p)) return -1;
        if (p->ist != I_TAG) return fail(p, "SystemData that ends in the middle of a record");
    } else if (!p->got_e) {
        return fail(p, "SystemData that ends with '-'");
    }
    if (!p->got_e) return fail(p, "SystemData without its end record");
    return 0;
}

static int files_done(osc_parser_t *p) {
    p->fbuf[p->flen] = 0;
    memcpy(p->filesval, p->fbuf, (size_t)p->flen + 1);
    if (p->fbuf[0] == '-') {
        if (strcmp(p->fbuf, "-0")) return fail(p, "a bad files number");
        p->files_neg0 = true;
        p->files = 0;
    } else {
        uint64_t v = 0;
        for (int i = 0; i < p->flen; i++) v = v * 10 + (uint64_t)(p->fbuf[i] - '0');
        if (v > 0x7FFFFFFF) return fail(p, "a bad files number");
        p->files = (long)v;
    }
    p->st = H_KEY;
    if (p->cb.entry && p->cb.entry(p->cb.ctx, OSC_K_FILES, p->fbuf, p->flen)) return fail(p, "stopped");
    return 0;
}

static int on_break(osc_parser_t *p) {
    switch (p->st) {
    case H_KEY:     return p->klen ? fail(p, "a key broken by an empty line") : 0;
    case H_VERSION: return version_done(p, p->vbuf, p->vlen);
    case H_INNER:   return 0;
    default:        return fail(p, "a value broken by an empty line");
    }
}

static int on_char(osc_parser_t *p, char c) {
    switch (p->st) {
    case H_KEY:
        if (is_digit(c)) {
            if (p->klen >= (int)sizeof p->kbuf - 1) return fail(p, "a key that is too long");
            p->kbuf[p->klen++] = c;
            return 0;
        }
        if (c == ':') return key_done(p, p->kbuf, p->klen);
        if (c == '(' || c == ')') return fail(p, "brackets in the wrong place");
        return fail(p, "letters or marks outside SystemData");
    case H_VERSION:
        if (is_digit(c) || c == '.') {
            if (p->vlen >= (int)sizeof p->vbuf - 1) return fail(p, "a bad version number");
            p->vbuf[p->vlen++] = c;
            return 0;
        }
        if (c == ':') return version_split(p);
        return fail(p, "a bad version number");
    case H_FILES:
        if (p->sub < 2) {
            if (c != '(') return fail(p, "a files value without ((");
            p->sub++;
            return 0;
        }
        if (p->sub == 2) {
            if (c == '-' && p->flen == 0) { p->fbuf[p->flen++] = c; return 0; }
            if (is_digit(c) && p->flen < 11) { p->fbuf[p->flen++] = c; return 0; }
            if (c == ')' && p->flen > 0 && is_digit(p->fbuf[p->flen - 1])) { p->sub = 3; return 0; }
            return fail(p, "a bad files value");
        }
        if (c != ')') return fail(p, "a files value without ))");
        return files_done(p);
    case H_SYSOPEN:
        if (c != '(') return fail(p, "a SystemData value without ((");
        if (++p->sub == 2) { p->st = H_INNER; p->ist = I_TAG; }
        return 0;
    case H_INNER:
        if (c == ')') {
            if (inner_finish(p)) return -1;
            p->st = H_SYSCLOSE;
            return 0;
        }
        if (c == '-' || is_digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return inner_feed(p, c);
        return fail(p, "a character that is not allowed in SystemData");
    case H_SYSCLOSE:
        if (c != ')') return fail(p, "a SystemData value without ))");
        p->sys_done = true;
        p->st = H_KEY;
        if (p->cb.entry && p->cb.entry(p->cb.ctx, OSC_K_SYSDATA, "", 0)) return fail(p, "stopped");
        return 0;
    }
    return -1;
}

int osc_parser_feed(osc_parser_t *p, const char *s, uint32_t n) {
    if (p->err) return p->err;
    for (uint32_t i = 0; i < n; i++) {
        char c = s[i];
        /* fast path: data digits inside SystemData */
        if (p->st == H_INNER && p->ist == I_FIELD && osc_b62_digit((unsigned char)c) >= 0 &&
            field_type(p, p->fi) == 'd') {
            uint32_t j = i;
            uint32_t crc = ~p->icrc;
            while (j < n) {
                char d = s[j];
                if (d == '\n' || d == ' ' || d == '\t' || d == '\r' || d == OSC_HS) { j++; continue; }
                if (osc_b62_digit((unsigned char)d) < 0) break;
                crc = crc_table[(crc ^ (uint8_t)d) & 0xFF] ^ (crc >> 8);
                p->inner_len++;
                p->grp[p->ng++] = d;
                if (p->ng == 6) {
                    if (!b62_ungroup(p->grp, 6, 4, p->obuf + p->olen)) { p->icrc = ~crc; return fail(p, "damaged data in SystemData"); }
                    p->olen += 4;
                    p->dgot += 4;
                    p->ng = 0;
                    if (p->dgot > p->rec.size) { p->icrc = ~crc; return fail(p, "data of the wrong size in SystemData"); }
                    if (p->olen + 4 > sizeof p->obuf) {
                        p->icrc = ~crc;
                        if (flush_data(p)) return -1;
                    }
                }
                j++;
            }
            p->icrc = ~crc;
            p->nl = 0;
            p->consumed += j - i;
            i = j - 1;
            continue;
        }
        p->consumed++;
        if (c == ' ' || c == '\t' || c == '\r' || c == OSC_HS) continue;
        if (c == '\n') { p->nl++; continue; }
        if (p->nl >= 2 && on_break(p)) return p->err;
        p->nl = 0;
        if (on_char(p, c)) return p->err ? p->err : -1;
    }
    return p->err;
}

bool osc_parser_complete(const osc_parser_t *p) {
    if (p->err || p->st != H_KEY || p->klen) return false;
    for (int k = 0; k < OSC_K_COUNT; k++) if (!p->seen[k]) return false;
    return true;
}

int osc_parser_finish(osc_parser_t *p) {
    if (p->err) return p->err;
    if (p->st == H_VERSION && version_done(p, p->vbuf, p->vlen)) return p->err;
    if (p->st != H_KEY || p->klen) return fail(p, "a code that ends in the middle of an entry");
    for (int k = 0; k < OSC_K_COUNT; k++) {
        if (!p->seen[k]) {
            static const char *const missing[OSC_K_COUNT] = {
                "a code without its version", "a code without its files number",
                "a code without its wallpaper entry", "a code without its SystemData" };
            return fail(p, missing[k]);
        }
    }
    if (p->files_neg0 && p->inner_len) return fail(p, "\"nothing saved\" with saved data");
    if (!p->files_neg0 && (uint32_t)p->files != p->nfile) return fail(p, "a files number that does not match the files");
    return 0;
}

bool osc_validate(const char *s, uint32_t n, char *why, int whycap) {
    osc_parser_t *p;
    static osc_parser_t sp;          /* big struct: not on the (shared) stack */
    p = &sp;
    osc_parser_init(p, NULL);
    osc_parser_feed(p, s, n);
    int r = osc_parser_finish(p);
    if (why && whycap > 0) {
        const char *e = r ? osc_parser_error(p) : "";
        size_t l = strlen(e);
        if ((int)l >= whycap) l = (size_t)whycap - 1;
        memcpy(why, e, l);
        why[l] = 0;
    }
    return r == 0;
}
