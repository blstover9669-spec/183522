/*
 * js_re.c - NovaJS regular expressions: a recursive-descent compiler to a
 * small instruction set and a backtracking matcher with an explicit stack.
 *
 * Syntax: alternation, groups (capturing, (?:), named (?<n>)), lookahead and
 * lookbehind (?= ?! ?<= ?<!), quantifiers * + ? {n} {n,} {n,m} (greedy and
 * lazy), classes with ranges and escapes, \d \w \s \b \B \cX \xHH \uHHHH
 * \u{...} \p{...}, backreferences \1 and \k<n>. Flags i m s u v y (g and d are
 * handled by the RegExp methods). Lookbehind bodies are compiled to run
 * backwards. Counted repetition is expanded (atom copies), captures inside a
 * quantified group are reset each iteration, empty iterations stop a loop.
 */
#include "jsi.h"

enum {
    R_CHAR = 1, R_CHARI, R_ANY, R_ANYNL, R_CLASS, R_NCLASS, R_BOL, R_EOL, R_WORDB, R_NWORDB,
    R_SPLIT, R_JMP, R_SAVE, R_BACKREF, R_LOOK, R_LOOK_END, R_MATCH, R_RESET, R_LOOP_SET, R_LOOP_CHECK
};
/* R_SPLIT a b: offsets relative to the next instruction; try a first.
   R_LOOK kind len: kind bit0 = negative, bit1 = behind; body follows, then R_LOOK_END. */

typedef struct RClass { uint32_t *r; int n, cap; } RClass;

struct JSRegExp {
    uint32_t flags;
    int ncaps;                   /* groups + 1 */
    int nregs;
    int32_t *prog;
    int plen;
    RClass *cls;
    int ncls;
    uint16_t **gname;            /* per group: name (UTF-16) or NULL */
    uint32_t *gnlen;
};

typedef struct RC {   /* compiler state */
    JSContext *ctx;
    const uint16_t *p;
    uint32_t n, i;
    uint32_t flags;
    bool u, ic, back;
    int32_t *code;
    int len, cap;
    RClass *cls;
    int ncls, clscap;
    int ncaps;
    int nregs;
    uint16_t *gname[256];
    uint32_t gnlen[256];
    struct { int at; uint16_t name[64]; uint32_t len; } fix[64];
    int nfix;
    char *err;
    int errsize;
    bool failed, ngroups_named;
    int depth;
} RC;

static void rerr(RC *c, const char *msg) {
    if (c->failed) return;
    c->failed = true;
    snprintf(c->err, (size_t)c->errsize, "%s", msg);
}

static int emit_w(RC *c, int32_t w) {
    if (c->len >= c->cap) {
        int nc = c->cap ? c->cap * 2 : 64;
        if (nc > 4000000) { rerr(c, "regular expression too large"); return 0; }
        int32_t *n = realloc(c->code, sizeof(int32_t) * (size_t)nc);
        if (!n) { rerr(c, "out of memory"); return 0; }
        c->code = n;
        c->cap = nc;
    }
    c->code[c->len] = w;
    return c->len++;
}

/* ---- character classes ---------------------------------------------------------------- */
static int new_class(RC *c) {
    if (c->ncls >= c->clscap) {
        int nc = c->clscap ? c->clscap * 2 : 8;
        RClass *n = realloc(c->cls, sizeof(RClass) * (size_t)nc);
        if (!n) { rerr(c, "out of memory"); return 0; }
        c->cls = n;
        c->clscap = nc;
    }
    memset(&c->cls[c->ncls], 0, sizeof(RClass));
    return c->ncls++;
}

static void cls_add(RC *c, int k, uint32_t lo, uint32_t hi) {
    RClass *r = &c->cls[k];
    if (r->n + 2 > r->cap) {
        int nc = r->cap ? r->cap * 2 : 16;
        uint32_t *n = realloc(r->r, sizeof(uint32_t) * (size_t)nc);
        if (!n) { rerr(c, "out of memory"); return; }
        r->r = n;
        r->cap = nc;
    }
    r->r[r->n++] = lo;
    r->r[r->n++] = hi;
}

static void cls_add_set(RC *c, int k, const uint32_t *set, int n, bool neg) {
    if (!neg) { for (int i = 0; i < n; i += 2) cls_add(c, k, set[i], set[i + 1]); return; }
    uint32_t lo = 0;
    for (int i = 0; i < n; i += 2) {
        if (set[i] > lo) cls_add(c, k, lo, set[i] - 1);
        lo = set[i + 1] + 1;
    }
    if (lo <= 0x10FFFF) cls_add(c, k, lo, 0x10FFFF);
}

static const uint32_t set_d[] = { '0', '9' };
static const uint32_t set_w[] = { '0', '9', 'A', 'Z', '_', '_', 'a', 'z' };
static const uint32_t set_s[] = { 9, 13, 32, 32, 0xA0, 0xA0, 0x1680, 0x1680, 0x2000, 0x200A, 0x2028, 0x2029,
                                  0x202F, 0x202F, 0x205F, 0x205F, 0x3000, 0x3000, 0xFEFF, 0xFEFF };
/* approximate Unicode property sets */
static const uint32_t set_L[] = { 'A', 'Z', 'a', 'z', 0xAA, 0xAA, 0xB5, 0xB5, 0xBA, 0xBA, 0xC0, 0xD6, 0xD8, 0xF6, 0xF8, 0x2C1,
    0x370, 0x373, 0x376, 0x377, 0x37B, 0x37D, 0x386, 0x386, 0x388, 0x3FF, 0x400, 0x481, 0x48A, 0x52F, 0x531, 0x556,
    0x561, 0x587, 0x5D0, 0x5EA, 0x620, 0x64A, 0x671, 0x6D3, 0x904, 0x939, 0x10A0, 0x10FF, 0x1100, 0x11FF, 0x1E00, 0x1FFF,
    0x2C00, 0x2CFF, 0x3040, 0x309F, 0x30A0, 0x30FF, 0x3400, 0x4DBF, 0x4E00, 0x9FFF, 0xAC00, 0xD7A3, 0xF900, 0xFAFF,
    0xFF21, 0xFF3A, 0xFF41, 0xFF5A, 0x20000, 0x2FFFF };
static const uint32_t set_N[] = { '0', '9', 0xB2, 0xB3, 0xB9, 0xB9, 0xBC, 0xBE, 0x660, 0x669, 0x6F0, 0x6F9, 0x966, 0x96F,
    0x2070, 0x2079, 0x2080, 0x2089, 0x2150, 0x2189, 0x2460, 0x249B, 0xFF10, 0xFF19 };
static const uint32_t set_Nd[] = { '0', '9', 0x660, 0x669, 0x6F0, 0x6F9, 0x966, 0x96F, 0xFF10, 0xFF19 };
static const uint32_t set_P[] = { '!', '#', '%', '*', ',', '/', ':', ';', '?', '@', '[', ']', '_', '_', '{', '{', '}', '}',
    0xA1, 0xA1, 0xA7, 0xA7, 0xAB, 0xAB, 0xB6, 0xB7, 0xBB, 0xBB, 0xBF, 0xBF, 0x2010, 0x2027, 0x2030, 0x205E, 0x3001, 0x3003,
    0x3008, 0x3011, 0xFF01, 0xFF0F };
static const uint32_t set_S[] = { '$', '$', '+', '+', '<', '>', '^', '^', '`', '`', '|', '|', '~', '~', 0xA2, 0xA9, 0xAC, 0xAC,
    0xAE, 0xB1, 0xB4, 0xB4, 0xD7, 0xD7, 0xF7, 0xF7, 0x2100, 0x214F, 0x2190, 0x23FF, 0x2500, 0x27BF, 0x2900, 0x2BFF,
    0x1F000, 0x1FAFF };
static const uint32_t set_Lu[] = { 'A', 'Z', 0xC0, 0xD6, 0xD8, 0xDE, 0x391, 0x3A9, 0x410, 0x42F };
static const uint32_t set_Ll[] = { 'a', 'z', 0xDF, 0xF6, 0xF8, 0xFF, 0x3B1, 0x3C9, 0x430, 0x44F };
static const uint32_t set_Emoji[] = { 0x23, 0x23, 0x2A, 0x2A, 0x30, 0x39, 0xA9, 0xA9, 0xAE, 0xAE, 0x203C, 0x203C, 0x2049, 0x2049,
    0x2122, 0x2122, 0x2139, 0x2139, 0x2194, 0x21AA, 0x231A, 0x23FF, 0x24C2, 0x24C2, 0x25AA, 0x27BF, 0x2934, 0x2935,
    0x2B05, 0x2B55, 0x3030, 0x3030, 0x303D, 0x303D, 0x3297, 0x3299, 0x1F000, 0x1FAFF };
static const uint32_t set_Latin[] = { 'A', 'Z', 'a', 'z', 0xAA, 0xAA, 0xBA, 0xBA, 0xC0, 0xD6, 0xD8, 0xF6, 0xF8, 0x24F, 0x1E00, 0x1EFF };
static const uint32_t set_Greek[] = { 0x370, 0x3FF, 0x1F00, 0x1FFF };
static const uint32_t set_Cyrillic[] = { 0x400, 0x52F };
static const uint32_t set_Han[] = { 0x2E80, 0x2FDF, 0x3005, 0x3007, 0x3021, 0x3029, 0x3400, 0x4DBF, 0x4E00, 0x9FFF, 0xF900, 0xFAFF, 0x20000, 0x2FFFF };
static const uint32_t set_Hiragana[] = { 0x3041, 0x309F };
static const uint32_t set_Katakana[] = { 0x30A0, 0x30FF, 0x31F0, 0x31FF };
static const uint32_t set_Any[] = { 0, 0x10FFFF };
static const uint32_t set_Ascii[] = { 0, 0x7F };
static const uint32_t set_Z[] = { 32, 32, 0xA0, 0xA0, 0x1680, 0x1680, 0x2000, 0x200A, 0x2028, 0x2029, 0x202F, 0x202F, 0x205F, 0x205F, 0x3000, 0x3000 };

static bool prop_set(const uint16_t *nm, uint32_t n, const uint32_t **set, int *cnt) {
    char b[48];
    uint32_t k = 0;
    for (uint32_t i = 0; i < n && k < sizeof b - 1; i++) b[k++] = (char)nm[i];
    b[k] = 0;
    const char *v = strchr(b, '=');
    if (v) v++; else v = b;
#define PS(name, s) if (!strcmp(v, name)) { *set = s; *cnt = JS_COUNTOF(s); return true; }
    PS("L", set_L) PS("Letter", set_L) PS("Alphabetic", set_L) PS("Alpha", set_L) PS("ID_Start", set_L) PS("XID_Start", set_L)
    PS("N", set_N) PS("Number", set_N) PS("Nd", set_Nd) PS("Decimal_Number", set_Nd) PS("digit", set_Nd)
    PS("P", set_P) PS("Punctuation", set_P) PS("S", set_S) PS("Symbol", set_S) PS("Sm", set_S) PS("So", set_S) PS("Sc", set_S)
    PS("Lu", set_Lu) PS("Uppercase_Letter", set_Lu) PS("Uppercase", set_Lu) PS("Ll", set_Ll) PS("Lowercase_Letter", set_Ll)
    PS("Lowercase", set_Ll) PS("Emoji", set_Emoji) PS("Emoji_Presentation", set_Emoji) PS("Extended_Pictographic", set_Emoji)
    PS("Latin", set_Latin) PS("Latn", set_Latin) PS("Greek", set_Greek) PS("Grek", set_Greek) PS("Cyrillic", set_Cyrillic)
    PS("Cyrl", set_Cyrillic) PS("Han", set_Han) PS("Hani", set_Han) PS("Hiragana", set_Hiragana) PS("Katakana", set_Katakana)
    PS("Any", set_Any) PS("ASCII", set_Ascii) PS("Z", set_Z) PS("Zs", set_Z) PS("Separator", set_Z) PS("White_Space", set_s)
    PS("space", set_s) PS("ID_Continue", set_L) PS("XID_Continue", set_L) PS("Mn", set_Any) PS("M", set_Any)
#undef PS
    return false;
}

/* ---- the parser ------------------------------------------------------------------------------- */
static inline bool more(RC *c) { return c->i < c->n && !c->failed; }
static inline uint32_t peekc(RC *c) { return c->i < c->n ? c->p[c->i] : 0xFFFFFFFFu; }

static uint32_t next_cp(RC *c) {
    uint32_t ch = c->p[c->i++];
    if (c->u && ch >= 0xD800 && ch < 0xDC00 && c->i < c->n && c->p[c->i] >= 0xDC00 && c->p[c->i] < 0xE000)
        ch = 0x10000 + ((ch - 0xD800) << 10) + (c->p[c->i++] - 0xDC00);
    return ch;
}

static int hexd(uint32_t ch) {
    if (ch >= '0' && ch <= '9') return (int)(ch - '0');
    ch |= 0x20;
    return ch >= 'a' && ch <= 'f' ? (int)(ch - 'a' + 10) : -1;
}

/* escape after '\' that stands for one character; -1 = not a char escape */
static int32_t char_escape(RC *c, bool in_class) {
    uint32_t ch = peekc(c);
    switch (ch) {
    case 'n': c->i++; return '\n';
    case 'r': c->i++; return '\r';
    case 't': c->i++; return '\t';
    case 'v': c->i++; return 0x0B;
    case 'f': c->i++; return '\f';
    case '0':
        if (c->i + 1 < c->n && c->p[c->i + 1] >= '0' && c->p[c->i + 1] <= '9' && !c->u) {
            /* legacy octal */
            int32_t v = 0, k = 0;
            while (k < 3 && c->i < c->n && c->p[c->i] >= '0' && c->p[c->i] <= '7') { v = v * 8 + (c->p[c->i] - '0'); c->i++; k++; }
            return v;
        }
        c->i++;
        return 0;
    case 'c':
        if (c->i + 1 < c->n && ((c->p[c->i + 1] | 0x20) >= 'a' && (c->p[c->i + 1] | 0x20) <= 'z')) {
            uint32_t l = c->p[c->i + 1];
            c->i += 2;
            return (int32_t)(l % 32);
        }
        if (c->u) { rerr(c, "Invalid unicode escape"); return -1; }
        return '\\';
    case 'x': {
        int a = c->i + 2 < c->n ? hexd(c->p[c->i + 1]) : -1, b = c->i + 2 < c->n ? hexd(c->p[c->i + 2]) : -1;
        if (a >= 0 && b >= 0) { c->i += 3; return a * 16 + b; }
        if (c->u) { rerr(c, "Invalid escape"); return -1; }
        c->i++;
        return 'x';
    }
    case 'u': {
        uint32_t k = c->i + 1;
        if (c->u && k < c->n && c->p[k] == '{') {
            k++;
            int32_t v = 0, nd = 0;
            while (k < c->n && hexd(c->p[k]) >= 0) { v = v * 16 + hexd(c->p[k]); k++; nd++; if (v > 0x10FFFF) break; }
            if (!nd || k >= c->n || c->p[k] != '}' || v > 0x10FFFF) { rerr(c, "Invalid Unicode escape"); return -1; }
            c->i = k + 1;
            return v;
        }
        if (k + 4 <= c->n) {
            int32_t v = 0;
            bool ok = true;
            for (int j = 0; j < 4; j++) { int h = hexd(c->p[k + (uint32_t)j]); if (h < 0) { ok = false; break; } v = v * 16 + h; }
            if (ok) {
                c->i = k + 4;
                /* 😀 in unicode mode: one code point */
                if (c->u && v >= 0xD800 && v < 0xDC00 && c->i + 6 <= c->n && c->p[c->i] == '\\' && c->p[c->i + 1] == 'u') {
                    int32_t w = 0;
                    bool ok2 = true;
                    for (int j = 0; j < 4; j++) { int h = hexd(c->p[c->i + 2 + (uint32_t)j]); if (h < 0) { ok2 = false; break; } w = w * 16 + h; }
                    if (ok2 && w >= 0xDC00 && w < 0xE000) { c->i += 6; v = 0x10000 + ((v - 0xD800) << 10) + (w - 0xDC00); }
                }
                return v;
            }
        }
        if (c->u) { rerr(c, "Invalid Unicode escape"); return -1; }
        c->i++;
        return 'u';
    }
    case 'b':
        if (in_class) { c->i++; return 8; }
        return -1;
    case '-':
        if (in_class) { c->i++; return '-'; }
        break;
    default: break;
    }
    if (ch == 'd' || ch == 'D' || ch == 'w' || ch == 'W' || ch == 's' || ch == 'S' || ch == 'B' ||
        ((ch == 'p' || ch == 'P') && c->u) || (ch == 'k' && (c->u || c->ngroups_named)))
        return -1;
    if (ch >= '1' && ch <= '9' && !in_class) return -1;
    if (ch >= '1' && ch <= '9' && in_class) {
        if (c->u) { rerr(c, "Invalid class escape"); return -1; }
        if (ch <= '7') {
            int32_t v = 0, k = 0;
            while (k < 3 && c->i < c->n && c->p[c->i] >= '0' && c->p[c->i] <= '7') { v = v * 8 + (c->p[c->i] - '0'); c->i++; k++; }
            return v;
        }
        c->i++;
        return (int32_t)ch;
    }
    if (c->u && js_is_id_part(ch) && ch != '$') { rerr(c, "Invalid escape"); return -1; }
    return (int32_t)next_cp(c);
}

/* class escapes \d \w \s \p{..} into class k; true if one was consumed */
static bool class_escape(RC *c, int k) {
    uint32_t ch = peekc(c);
    bool neg = ch == 'D' || ch == 'W' || ch == 'S' || ch == 'P';
    switch (ch) {
    case 'd': case 'D': c->i++; cls_add_set(c, k, set_d, JS_COUNTOF(set_d), neg); return true;
    case 'w': case 'W': c->i++; cls_add_set(c, k, set_w, JS_COUNTOF(set_w), neg); return true;
    case 's': case 'S': c->i++; cls_add_set(c, k, set_s, JS_COUNTOF(set_s), neg); return true;
    case 'p': case 'P': {
        if (!c->u) return false;
        c->i++;
        if (peekc(c) != '{') { rerr(c, "Invalid property name"); return true; }
        uint32_t st = ++c->i;
        while (c->i < c->n && c->p[c->i] != '}') c->i++;
        if (c->i >= c->n) { rerr(c, "Invalid property name"); return true; }
        const uint32_t *set;
        int cnt;
        if (!prop_set(c->p + st, c->i - st, &set, &cnt)) { rerr(c, "Invalid property name"); return true; }
        c->i++;
        cls_add_set(c, k, set, cnt, neg);
        return true;
    }
    default: return false;
    }
}

static void emit_class(RC *c, int k, bool neg) {
    /* sort + merge the ranges */
    RClass *r = &c->cls[k];
    for (int i = 2; i < r->n; i += 2) {
        uint32_t lo = r->r[i], hi = r->r[i + 1];
        int j = i;
        while (j > 0 && r->r[j - 2] > lo) { r->r[j] = r->r[j - 2]; r->r[j + 1] = r->r[j - 1]; j -= 2; }
        r->r[j] = lo;
        r->r[j + 1] = hi;
    }
    int o = 0;
    for (int i = 0; i < r->n; i += 2) {
        if (o && r->r[i] <= r->r[o - 1] + 1) { if (r->r[i + 1] > r->r[o - 1]) r->r[o - 1] = r->r[i + 1]; }
        else { r->r[o++] = r->r[i]; r->r[o++] = r->r[i + 1]; }
    }
    r->n = o;
    emit_w(c, neg ? R_NCLASS : R_CLASS);
    emit_w(c, k);
}

static void parse_class(RC *c) {
    c->i++;   /* [ */
    bool neg = false;
    if (peekc(c) == '^') { neg = true; c->i++; }
    int k = new_class(c);
    while (more(c) && peekc(c) != ']') {
        int32_t lo;
        if (peekc(c) == '\\') {
            c->i++;
            if (class_escape(c, k)) continue;
            lo = char_escape(c, true);
            if (lo < 0) { if (!c->failed) rerr(c, "Invalid class escape"); return; }
        } else lo = (int32_t)next_cp(c);
        if (peekc(c) == '-' && c->i + 1 < c->n && c->p[c->i + 1] != ']') {
            c->i++;
            int32_t hi;
            if (peekc(c) == '\\') {
                c->i++;
                uint32_t save = c->i;
                int t = new_class(c);
                if (class_escape(c, t)) {
                    /* [a-\d]: a literal '-' (annex B) */
                    if (c->u) { rerr(c, "Invalid character class"); return; }
                    for (int q = 0; q < c->cls[t].n; q++) cls_add(c, k, c->cls[t].r[q], c->cls[t].r[q + 1]), q++;
                    cls_add(c, k, (uint32_t)lo, (uint32_t)lo);
                    cls_add(c, k, '-', '-');
                    (void)save;
                    continue;
                }
                hi = char_escape(c, true);
                if (hi < 0) { if (!c->failed) rerr(c, "Invalid class escape"); return; }
            } else hi = (int32_t)next_cp(c);
            if (hi < lo) { rerr(c, "Range out of order in character class"); return; }
            cls_add(c, k, (uint32_t)lo, (uint32_t)hi);
        } else cls_add(c, k, (uint32_t)lo, (uint32_t)lo);
    }
    if (peekc(c) != ']') { rerr(c, "Unterminated character class"); return; }
    c->i++;
    emit_class(c, k, neg);
}

static void parse_alt(RC *c);

static bool parse_int(RC *c, uint32_t *v) {
    if (!(peekc(c) >= '0' && peekc(c) <= '9')) return false;
    uint64_t x = 0;
    while (c->i < c->n && c->p[c->i] >= '0' && c->p[c->i] <= '9') { x = x * 10 + (c->p[c->i] - '0'); if (x > 100000) x = 100000; c->i++; }
    *v = (uint32_t)x;
    return true;
}

/* {n}, {n,}, {n,m} at c->i; false (position restored) if it is not a quantifier */
static bool parse_brace(RC *c, uint32_t *mn, uint32_t *mx) {
    uint32_t save = c->i;
    c->i++;
    if (!parse_int(c, mn)) { c->i = save; return false; }
    *mx = *mn;
    if (peekc(c) == ',') {
        c->i++;
        if (!parse_int(c, mx)) *mx = 0xFFFFFFFFu;
    }
    if (peekc(c) != '}') { c->i = save; return false; }
    c->i++;
    return true;
}

static void apply_quant(RC *c, int start, int cap_lo, int cap_hi) {
    uint32_t mn, mx;
    uint32_t ch = peekc(c);
    if (ch == '*') { mn = 0; mx = 0xFFFFFFFFu; c->i++; }
    else if (ch == '+') { mn = 1; mx = 0xFFFFFFFFu; c->i++; }
    else if (ch == '?') { mn = 0; mx = 1; c->i++; }
    else if (ch == '{') {
        if (!parse_brace(c, &mn, &mx)) { if (c->u) rerr(c, "Incomplete quantifier"); return; }
        if (mx < mn) { rerr(c, "numbers out of order in {} quantifier"); return; }
    } else return;
    bool lazy = false;
    if (peekc(c) == '?') { lazy = true; c->i++; }
    int alen = c->len - start;
    if (alen <= 0) return;
    int32_t *atom = malloc(sizeof(int32_t) * (size_t)alen);
    if (!atom) { rerr(c, "out of memory"); return; }
    memcpy(atom, c->code + start, sizeof(int32_t) * (size_t)alen);
    c->len = start;
    uint32_t copies = mn + (mx == 0xFFFFFFFFu ? 1 : mx - mn);
    if ((uint64_t)copies * (uint32_t)(alen + 8) > 2000000u) { free(atom); rerr(c, "regular expression too large"); return; }
    bool reset = cap_hi > cap_lo;
    /* mandatory copies */
    for (uint32_t k = 0; k < mn; k++) {
        if (reset) { emit_w(c, R_RESET); emit_w(c, cap_lo); emit_w(c, cap_hi); }
        for (int q = 0; q < alen; q++) emit_w(c, atom[q]);
    }
    if (mx == 0xFFFFFFFFu) {
        /* loop: SPLIT body exit; body: LOOP_SET r, atom, LOOP_CHECK r, JMP split */
        int reg = c->nregs++;
        int split = emit_w(c, R_SPLIT);
        emit_w(c, 0);
        emit_w(c, 0);
        int body = c->len;
        emit_w(c, R_LOOP_SET);
        emit_w(c, reg);
        if (reset) { emit_w(c, R_RESET); emit_w(c, cap_lo); emit_w(c, cap_hi); }
        for (int q = 0; q < alen; q++) emit_w(c, atom[q]);
        emit_w(c, R_LOOP_CHECK);
        emit_w(c, reg);
        int j = emit_w(c, R_JMP);
        emit_w(c, 0);
        c->code[j + 1] = split - (j + 2);
        int exit = c->len;
        int a = body - (split + 3), b = exit - (split + 3);
        c->code[split + 1] = lazy ? b : a;
        c->code[split + 2] = lazy ? a : b;
    } else {
        /* optional copies, nested: (x(x(x)?)?)? */
        uint32_t opt = mx - mn;
        int *splits = opt ? malloc(sizeof(int) * opt) : NULL;
        if (opt && !splits) { free(atom); rerr(c, "out of memory"); return; }
        for (uint32_t k = 0; k < opt; k++) {
            splits[k] = emit_w(c, R_SPLIT);
            emit_w(c, 0);
            emit_w(c, 0);
            if (reset) { emit_w(c, R_RESET); emit_w(c, cap_lo); emit_w(c, cap_hi); }
            for (int q = 0; q < alen; q++) emit_w(c, atom[q]);
        }
        int exit = c->len;
        for (uint32_t k = 0; k < opt; k++) {
            int s = splits[k];
            int a = 0, b = exit - (s + 3);
            c->code[s + 1] = lazy ? b : a;
            c->code[s + 2] = lazy ? a : b;
        }
        free(splits);
    }
    free(atom);
}

static void emit_char(RC *c, uint32_t ch) {
    if (c->ic) {
        emit_w(c, R_CHARI);
        emit_w(c, (int32_t)js_case_lower(js_case_upper(ch)));
    } else {
        emit_w(c, R_CHAR);
        emit_w(c, (int32_t)ch);
    }
}

/* in a lookbehind the terms of a sequence are emitted in reverse order */
static void parse_term(RC *c) {
    if (++c->depth > 200) { rerr(c, "regular expression too deeply nested"); c->depth--; return; }
    int start = c->len;
    int cap_lo = c->ncaps, cap_hi = c->ncaps;
    uint32_t ch = peekc(c);
    bool quant_ok = true;
    switch (ch) {
    case '^': c->i++; emit_w(c, R_BOL); quant_ok = false; break;
    case '$': c->i++; emit_w(c, R_EOL); quant_ok = false; break;
    case '.': c->i++; emit_w(c, (c->flags & RE_S) ? R_ANYNL : R_ANY); break;
    case '[': parse_class(c); break;
    case '(': {
        c->i++;
        if (peekc(c) == '?') {
            c->i++;
            uint32_t k = peekc(c);
            if (k == ':') { c->i++; parse_alt(c); }
            else if (k == '=' || k == '!' || (k == '<' && c->i + 1 < c->n && (c->p[c->i + 1] == '=' || c->p[c->i + 1] == '!'))) {
                bool behind = k == '<';
                if (behind) c->i++;
                bool negl = peekc(c) == '!';
                c->i++;
                int at = emit_w(c, R_LOOK);
                emit_w(c, (negl ? 1 : 0) | (behind ? 2 : 0));
                emit_w(c, 0);
                bool sb = c->back;
                c->back = behind;
                parse_alt(c);
                c->back = sb;
                emit_w(c, R_LOOK_END);
                c->code[at + 2] = c->len - (at + 3);
                quant_ok = !c->u && !behind;
            } else if (k == '<') {
                c->i++;
                uint32_t st = c->i;
                while (c->i < c->n && c->p[c->i] != '>') c->i++;
                if (c->i >= c->n || c->i == st || c->ncaps >= 255) { rerr(c, "Invalid capture group name"); break; }
                int g = c->ncaps++;
                uint32_t nl = c->i - st;
                c->gname[g] = malloc(sizeof(uint16_t) * (nl + 1));
                if (c->gname[g]) memcpy(c->gname[g], c->p + st, sizeof(uint16_t) * nl);
                c->gnlen[g] = nl;
                c->i++;
                emit_w(c, R_SAVE);
                emit_w(c, c->back ? 2 * g + 1 : 2 * g);
                parse_alt(c);
                emit_w(c, R_SAVE);
                emit_w(c, c->back ? 2 * g : 2 * g + 1);
                cap_hi = c->ncaps;
            } else { rerr(c, "Invalid group"); break; }
        } else {
            if (c->ncaps >= 255) { rerr(c, "Too many capture groups"); break; }
            int g = c->ncaps++;
            emit_w(c, R_SAVE);
            emit_w(c, c->back ? 2 * g + 1 : 2 * g);
            parse_alt(c);
            emit_w(c, R_SAVE);
            emit_w(c, c->back ? 2 * g : 2 * g + 1);
            cap_hi = c->ncaps;
        }
        if (peekc(c) != ')') { rerr(c, "Unterminated group"); break; }
        c->i++;
        break;
    }
    case ')': rerr(c, "Unmatched ')'"); break;
    case '*': case '+': case '?': rerr(c, "Nothing to repeat"); break;
    case '{': {
        uint32_t a, b2;
        uint32_t save = c->i;
        if (parse_brace(c, &a, &b2)) { c->i = save; rerr(c, "Nothing to repeat"); break; }
        if (c->u) { rerr(c, "Lone quantifier brackets"); break; }
        c->i++;
        emit_char(c, '{');
        break;
    }
    case '\\': {
        c->i++;
        if (c->i >= c->n) { rerr(c, "\\ at end of pattern"); break; }
        uint32_t e = peekc(c);
        if (e == 'b' || e == 'B') { c->i++; emit_w(c, e == 'b' ? R_WORDB : R_NWORDB); quant_ok = false; break; }
        if (e >= '1' && e <= '9') {
            uint32_t save = c->i, v;
            parse_int(c, &v);
            if (v < 256) {
                emit_w(c, R_BACKREF);
                emit_w(c, (int32_t)v);
                break;
            }
            c->i = save;
        }
        if (e == 'k' && (c->u || c->ngroups_named)) {
            c->i++;
            if (peekc(c) != '<') { rerr(c, "Invalid named reference"); break; }
            uint32_t st = ++c->i;
            while (c->i < c->n && c->p[c->i] != '>') c->i++;
            if (c->i >= c->n || c->nfix >= 64) { rerr(c, "Invalid named reference"); break; }
            int at = emit_w(c, R_BACKREF);
            emit_w(c, -1);
            c->fix[c->nfix].at = at;
            uint32_t nl = c->i - st;
            if (nl > 63) nl = 63;
            memcpy(c->fix[c->nfix].name, c->p + st, sizeof(uint16_t) * nl);
            c->fix[c->nfix].len = nl;
            c->nfix++;
            c->i++;
            break;
        }
        int k = -1;
        if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S' || ((e == 'p' || e == 'P') && c->u)) {
            k = new_class(c);
            class_escape(c, k);
            emit_class(c, k, false);
            break;
        }
        int32_t v = char_escape(c, false);
        if (v < 0) { if (!c->failed) rerr(c, "Invalid escape"); break; }
        emit_char(c, (uint32_t)v);
        break;
    }
    default:
        if (ch == ']' && c->u) { rerr(c, "Lone quantifier brackets"); break; }
        if (ch == '}' && c->u) { rerr(c, "Lone quantifier brackets"); break; }
        emit_char(c, next_cp(c));
        break;
    }
    if (quant_ok && more(c)) apply_quant(c, start, cap_lo, cap_hi);
    else if (!quant_ok && more(c) && (peekc(c) == '*' || peekc(c) == '+' || peekc(c) == '?') && c->u) rerr(c, "Nothing to repeat");
    c->depth--;
}

static void parse_seq(RC *c) {
    if (!c->back) {
        while (more(c) && peekc(c) != '|' && peekc(c) != ')') parse_term(c);
        return;
    }
    /* backwards: emit each term, then reverse the order of the term blocks */
    int starts[512];
    int nt = 0;
    int base = c->len;
    while (more(c) && peekc(c) != '|' && peekc(c) != ')') {
        if (nt < 512) starts[nt++] = c->len;
        parse_term(c);
    }
    if (nt < 2 || c->failed) return;
    int end = c->len;
    int total = end - base;
    int32_t *tmp = malloc(sizeof(int32_t) * (size_t)total);
    if (!tmp) { rerr(c, "out of memory"); return; }
    int o = 0;
    for (int t = nt - 1; t >= 0; t--) {
        int s = starts[t], e = t + 1 < nt ? starts[t + 1] : end;
        memcpy(tmp + o, c->code + s, sizeof(int32_t) * (size_t)(e - s));
        o += e - s;
    }
    memcpy(c->code + base, tmp, sizeof(int32_t) * (size_t)total);
    free(tmp);
}

static void parse_alt(RC *c) {
    int start = c->len;
    parse_seq(c);
    if (peekc(c) != '|' || c->failed) return;
    /* SPLIT a b, a: seq, JMP end, b: rest */
    int jumps[256];
    int nj = 0;
    while (peekc(c) == '|' && !c->failed) {
        c->i++;
        int alen = c->len - start;
        int32_t *seq = malloc(sizeof(int32_t) * (size_t)(alen ? alen : 1));
        if (!seq) { rerr(c, "out of memory"); return; }
        memcpy(seq, c->code + start, sizeof(int32_t) * (size_t)alen);
        c->len = start;
        int s = emit_w(c, R_SPLIT);
        emit_w(c, 0);
        emit_w(c, 0);
        for (int q = 0; q < alen; q++) emit_w(c, seq[q]);
        free(seq);
        int j = emit_w(c, R_JMP);
        emit_w(c, 0);
        if (nj < 256) jumps[nj++] = j;
        c->code[s + 1] = 0;
        c->code[s + 2] = c->len - (s + 3);
        start = c->len;
        parse_seq(c);
    }
    for (int k = 0; k < nj; k++) c->code[jumps[k] + 1] = c->len - (jumps[k] + 2);
}

/* jump offsets inside alternations are relative to positions that moved when an
   earlier alternative was re-emitted after its SPLIT; parse_alt keeps them relative to
   their own instruction, so moving whole blocks is safe. */

JSRegExp *js_re_compile(JSContext *ctx, const uint16_t *pat, uint32_t plen, uint32_t flags, char *err, int errsize) {
    RC *c = calloc(1, sizeof *c);
    if (!c) { snprintf(err, (size_t)errsize, "out of memory"); return NULL; }
    c->ctx = ctx;
    c->p = pat;
    c->n = plen;
    c->flags = flags;
    c->u = (flags & (RE_U | RE_V)) != 0;
    c->ic = (flags & RE_I) != 0;
    c->err = err;
    c->errsize = errsize;
    c->ncaps = 1;
    /* named groups present? (enables \k outside unicode mode) */
    for (uint32_t i = 0; i + 2 < plen; i++) if (pat[i] == '(' && pat[i + 1] == '?' && pat[i + 2] == '<' && i + 3 < plen && pat[i + 3] != '=' && pat[i + 3] != '!') { c->ngroups_named = true; break; }
    emit_w(c, R_SAVE);
    emit_w(c, 0);
    parse_alt(c);
    if (more(c)) rerr(c, peekc(c) == ')' ? "Unmatched ')'" : "Unexpected character");
    emit_w(c, R_SAVE);
    emit_w(c, 1);
    emit_w(c, R_MATCH);
    for (int k = 0; k < c->nfix && !c->failed; k++) {
        int g;
        for (g = 1; g < c->ncaps; g++)
            if (c->gname[g] && c->gnlen[g] == c->fix[k].len && !memcmp(c->gname[g], c->fix[k].name, sizeof(uint16_t) * c->fix[k].len)) break;
        if (g >= c->ncaps) { rerr(c, "Invalid named capture referenced"); break; }
        c->code[c->fix[k].at + 1] = g;
    }
    JSRegExp *re = NULL;
    if (!c->failed) {
        re = calloc(1, sizeof *re);
        if (re) {
            re->flags = flags;
            re->ncaps = c->ncaps;
            re->nregs = c->nregs;
            re->prog = c->code;
            re->plen = c->len;
            re->cls = c->cls;
            re->ncls = c->ncls;
            re->gname = calloc((size_t)c->ncaps, sizeof(uint16_t *));
            re->gnlen = calloc((size_t)c->ncaps, sizeof(uint32_t));
            if (re->gname && re->gnlen)
                for (int g = 0; g < c->ncaps; g++) { re->gname[g] = c->gname[g]; re->gnlen[g] = c->gnlen[g]; c->gname[g] = NULL; }
            c->code = NULL;
            c->cls = NULL;
        }
    }
    free(c->code);
    if (c->cls) { for (int k = 0; k < c->ncls; k++) free(c->cls[k].r); free(c->cls); }
    for (int g = 0; g < 256; g++) free(c->gname[g]);
    free(c);
    return re;
}

void js_re_free(JSContext *ctx, JSRegExp *re) {
    (void)ctx;
    if (!re) return;
    free(re->prog);
    for (int k = 0; k < re->ncls; k++) free(re->cls[k].r);
    free(re->cls);
    if (re->gname) for (int g = 0; g < re->ncaps; g++) free(re->gname[g]);
    free(re->gname);
    free(re->gnlen);
    free(re);
}

int js_re_ncaps(JSRegExp *re) { return re->ncaps; }
uint32_t js_re_get_flags(JSRegExp *re) { return re->flags; }
void js_re_set_flags(JSRegExp *re, uint32_t flags) { re->flags = flags; }

JSValue js_re_group_names(JSContext *ctx, JSRegExp *re) {
    bool any = false;
    for (int g = 1; g < re->ncaps; g++) if (re->gname && re->gname[g]) any = true;
    if (!any) return JS_UNDEFINED;
    JSValue a = js_array_new(ctx, 0);
    if (JS_IsException(a)) return a;
    for (int g = 0; g < re->ncaps; g++) {
        JSValue v = g && re->gname[g] ? js_atom16(ctx, re->gname[g], re->gnlen[g]) : JS_UNDEFINED;
        if (JS_IsException(v) || js_array_push_v(ctx, a, v) < 0) return JS_EXCEPTION;
    }
    return a;
}

/* ==== matching ===================================================================================== */
typedef struct RM {
    JSRegExp *re;
    const uint16_t *s;
    uint32_t len;
    int32_t *caps;
    int32_t *regs;
    bool u, ic, multi;
    uint32_t steps;
    int32_t *bt;          /* backtrack stack: entries of 3 words */
    int btn, btcap;
    bool overflow;
} RM;

enum { BT_CHOICE = 1, BT_CAP, BT_REG, BT_LOOK };

static bool bt_push(RM *m, int32_t a, int32_t b, int32_t c) {
    if (m->btn + 3 > m->btcap) {
        int nc = m->btcap ? m->btcap * 2 : 768;
        if (nc > 30000000) { m->overflow = true; return false; }
        int32_t *n = realloc(m->bt, sizeof(int32_t) * (size_t)nc);
        if (!n) { m->overflow = true; return false; }
        m->bt = n;
        m->btcap = nc;
    }
    m->bt[m->btn++] = a;
    m->bt[m->btn++] = b;
    m->bt[m->btn++] = c;
    return true;
}

static inline uint32_t canon(RM *m, uint32_t c) { return m->ic ? js_case_lower(js_case_upper(c)) : c; }

static inline bool is_word(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static inline bool is_lt(uint32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }

/* character at pos going forward (n = units), or backward (before pos) */
static inline uint32_t ch_fwd(RM *m, uint32_t pos, int *n) {
    uint32_t c = m->s[pos];
    *n = 1;
    if (m->u && c >= 0xD800 && c < 0xDC00 && pos + 1 < m->len && m->s[pos + 1] >= 0xDC00 && m->s[pos + 1] < 0xE000) {
        *n = 2;
        return 0x10000 + ((c - 0xD800) << 10) + (m->s[pos + 1] - 0xDC00);
    }
    return c;
}

static inline uint32_t ch_back(RM *m, uint32_t pos, int *n) {
    uint32_t c = m->s[pos - 1];
    *n = 1;
    if (m->u && c >= 0xDC00 && c < 0xE000 && pos >= 2 && m->s[pos - 2] >= 0xD800 && m->s[pos - 2] < 0xDC00) {
        *n = 2;
        return 0x10000 + ((m->s[pos - 2] - 0xD800) << 10) + (c - 0xDC00);
    }
    return c;
}

static bool in_class(RM *m, RClass *k, uint32_t c) {
    for (int pass = 0; pass < (m->ic ? 3 : 1); pass++) {
        uint32_t x = pass == 0 ? c : pass == 1 ? js_case_lower(c) : js_case_upper(c);
        int lo = 0, hi = k->n / 2 - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (x < k->r[2 * mid]) hi = mid - 1;
            else if (x > k->r[2 * mid + 1]) lo = mid + 1;
            else return true;
        }
    }
    return false;
}

/* run from pc at pos; backward = a lookbehind body; returns 1 match (*endpos), 0 fail, -1 overflow */
static int re_run(RM *m, int pc, uint32_t pos, bool backward, uint32_t *endpos, int depth);

static int re_run(RM *m, int pc, uint32_t pos, bool backward, uint32_t *endpos, int depth) {
    const int32_t *prog = m->re->prog;
    int base = m->btn;
    if (depth > 100) { m->overflow = true; return -1; }
    for (;;) {
        if (++m->steps > 20000000u) { m->overflow = true; return -1; }
        int32_t op = prog[pc];
        bool ok = true;
        switch (op) {
        case R_CHAR: case R_CHARI: {
            int n;
            if (backward) {
                if (pos == 0) { ok = false; break; }
                uint32_t c = ch_back(m, pos, &n);
                if ((op == R_CHARI ? canon(m, c) : c) != (uint32_t)prog[pc + 1]) { ok = false; break; }
                pos -= (uint32_t)n;
            } else {
                if (pos >= m->len) { ok = false; break; }
                uint32_t c = ch_fwd(m, pos, &n);
                if ((op == R_CHARI ? canon(m, c) : c) != (uint32_t)prog[pc + 1]) { ok = false; break; }
                pos += (uint32_t)n;
            }
            pc += 2;
            break;
        }
        case R_ANY: case R_ANYNL: case R_CLASS: case R_NCLASS: {
            int n;
            uint32_t c;
            if (backward) { if (pos == 0) { ok = false; break; } c = ch_back(m, pos, &n); }
            else { if (pos >= m->len) { ok = false; break; } c = ch_fwd(m, pos, &n); }
            if (op == R_ANY) ok = !is_lt(c);
            else if (op == R_CLASS || op == R_NCLASS) { ok = in_class(m, &m->re->cls[prog[pc + 1]], c); if (op == R_NCLASS) ok = !ok; }
            if (!ok) break;
            pos = backward ? pos - (uint32_t)n : pos + (uint32_t)n;
            pc += op == R_ANY || op == R_ANYNL ? 1 : 2;
            break;
        }
        case R_BOL:
            ok = pos == 0 || (m->multi && is_lt(m->s[pos - 1]));
            pc++;
            break;
        case R_EOL:
            ok = pos == m->len || (m->multi && is_lt(m->s[pos]));
            pc++;
            break;
        case R_WORDB: case R_NWORDB: {
            bool a = pos > 0 && is_word(m->ic ? canon(m, m->s[pos - 1]) : m->s[pos - 1]);
            bool b = pos < m->len && is_word(m->ic ? canon(m, m->s[pos]) : m->s[pos]);
            ok = (a != b) == (op == R_WORDB);
            pc++;
            break;
        }
        case R_SPLIT:
            if (!bt_push(m, BT_CHOICE, pc + 3 + prog[pc + 2], (int32_t)pos)) return -1;
            pc = pc + 3 + prog[pc + 1];
            break;
        case R_JMP: pc = pc + 2 + prog[pc + 1]; break;
        case R_SAVE: {
            int k = prog[pc + 1];
            if (!bt_push(m, BT_CAP, k, m->caps[k])) return -1;
            m->caps[k] = (int32_t)pos;
            pc += 2;
            break;
        }
        case R_RESET: {
            for (int g = prog[pc + 1]; g < prog[pc + 2]; g++) {
                if (m->caps[2 * g] < 0 && m->caps[2 * g + 1] < 0) continue;
                if (!bt_push(m, BT_CAP, 2 * g, m->caps[2 * g]) || !bt_push(m, BT_CAP, 2 * g + 1, m->caps[2 * g + 1])) return -1;
                m->caps[2 * g] = m->caps[2 * g + 1] = -1;
            }
            pc += 3;
            break;
        }
        case R_LOOP_SET: {
            int r = prog[pc + 1];
            if (!bt_push(m, BT_REG, r, m->regs[r])) return -1;
            m->regs[r] = (int32_t)pos;
            pc += 2;
            break;
        }
        case R_LOOP_CHECK:
            ok = m->regs[prog[pc + 1]] != (int32_t)pos;   /* an empty iteration ends the loop */
            pc += 2;
            break;
        case R_BACKREF: {
            int g = prog[pc + 1];
            int32_t s0 = g < m->re->ncaps ? m->caps[2 * g] : -1, s1 = g < m->re->ncaps ? m->caps[2 * g + 1] : -1;
            pc += 2;
            if (s0 < 0 || s1 < 0) break;   /* unset group: matches empty */
            uint32_t n = (uint32_t)(s1 - s0);
            if (backward) {
                if (pos < n) { ok = false; break; }
                for (uint32_t k = 0; k < n; k++) if (canon(m, m->s[pos - n + k]) != canon(m, m->s[(uint32_t)s0 + k])) { ok = false; break; }
                if (ok) pos -= n;
            } else {
                if (pos + n > m->len) { ok = false; break; }
                for (uint32_t k = 0; k < n; k++) if (canon(m, m->s[pos + k]) != canon(m, m->s[(uint32_t)s0 + k])) { ok = false; break; }
                if (ok) pos += n;
            }
            break;
        }
        case R_LOOK: {
            int kind = prog[pc + 1], blen = prog[pc + 2];
            int body = pc + 3, after = pc + 3 + blen;
            uint32_t e;
            int save_bt = m->btn;
            int r = re_run(m, body, pos, (kind & 2) != 0, &e, depth + 1);
            if (r < 0) return -1;
            bool neg = kind & 1;
            if (neg) {
                /* undo whatever the body did */
                while (m->btn > save_bt) {
                    m->btn -= 3;
                    int32_t t = m->bt[m->btn];
                    if (t == BT_CAP) m->caps[m->bt[m->btn + 1]] = m->bt[m->btn + 2];
                    else if (t == BT_REG) m->regs[m->bt[m->btn + 1]] = m->bt[m->btn + 2];
                }
                ok = !r;
            } else {
                /* keep the captures (their undo records stay), drop the body's choice points */
                if (r) {
                    int w = save_bt;
                    for (int k = save_bt; k < m->btn; k += 3)
                        if (m->bt[k] != BT_CHOICE) { m->bt[w] = m->bt[k]; m->bt[w + 1] = m->bt[k + 1]; m->bt[w + 2] = m->bt[k + 2]; w += 3; }
                    m->btn = w;
                }
                ok = r != 0;
            }
            pc = after;
            break;
        }
        case R_LOOK_END:
        case R_MATCH:
            *endpos = pos;
            return 1;
        default:
            return 0;
        }
        if (ok) continue;
        /* backtrack */
        for (;;) {
            if (m->btn <= base) return 0;
            m->btn -= 3;
            int32_t t = m->bt[m->btn];
            if (t == BT_CAP) m->caps[m->bt[m->btn + 1]] = m->bt[m->btn + 2];
            else if (t == BT_REG) m->regs[m->bt[m->btn + 1]] = m->bt[m->btn + 2];
            else if (t == BT_CHOICE) { pc = m->bt[m->btn + 1]; pos = (uint32_t)m->bt[m->btn + 2]; break; }
        }
    }
}

int js_re_exec(JSContext *ctx, JSRegExp *re, const uint16_t *s, uint32_t len, uint32_t start, int32_t *caps) {
    RM m;
    memset(&m, 0, sizeof m);
    m.re = re;
    m.s = s;
    m.len = len;
    m.caps = caps;
    m.u = (re->flags & (RE_U | RE_V)) != 0;
    m.ic = (re->flags & RE_I) != 0;
    m.multi = (re->flags & RE_M) != 0;
    int32_t regs_small[16];
    m.regs = re->nregs <= 16 ? regs_small : malloc(sizeof(int32_t) * (size_t)re->nregs);
    if (!m.regs) { js_throw_oom(ctx); return -1; }
    bool sticky = (re->flags & RE_Y) != 0;
    int r = 0;
    /* a first literal char lets us skip positions quickly */
    int32_t first = -1;
    if (re->prog[2] == R_CHAR && !m.u) first = re->prog[3];
    for (uint32_t p = start; p <= len; p++) {
        if (first >= 0 && !sticky) {
            while (p < len && s[p] != (uint32_t)first) p++;
            if (p >= len) break;
        }
        for (int i = 0; i < 2 * re->ncaps; i++) caps[i] = -1;
        for (int i = 0; i < re->nregs; i++) m.regs[i] = -1;
        m.btn = 0;
        uint32_t e;
        r = re_run(&m, 0, p, false, &e, 0);
        if (r != 0 || sticky) break;
        if (m.u && p < len && s[p] >= 0xD800 && s[p] < 0xDC00 && p + 1 < len && s[p + 1] >= 0xDC00 && s[p + 1] < 0xE000) p++;
    }
    if (m.regs != regs_small) free(m.regs);
    free(m.bt);
    if (r < 0) { js_throw_range(ctx, "Regular expression is too complex (too much backtracking)"); return -1; }
    return r;
}
