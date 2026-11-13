/*
 * js_parse.c - NovaJS tokenizer and parser: UTF-16 source -> syntax tree.
 *
 * Recursive descent with precedence climbing for binary operators. Arrow
 * parameters and destructuring assignments are parsed as expressions first
 * (the "cover grammar") and converted to patterns when the => or = shows up.
 * Class fields and static blocks are collected into synthetic initialiser
 * functions while parsing, so nested functions get the right parent.
 * Errors: the first one is kept, the token stream turns into EOF so every
 * loop ends, and js_parse_program throws a SyntaxError.
 */
#include "js_ast.h"

#define KEYWORDS(X) X(break) X(case) X(catch) X(class) X(const) X(continue) X(debugger) X(default) X(delete) \
    X(do) X(else) X(enum) X(export) X(extends) X(false) X(finally) X(for) X(function) X(if) X(import) X(in) \
    X(instanceof) X(new) X(null) X(return) X(super) X(switch) X(this) X(throw) X(true) X(try) X(typeof) X(var) \
    X(void) X(while) X(with) X(yield) X(let) X(static) X(implements) X(interface) X(package) X(private) \
    X(protected) X(public) X(await) X(async) X(of) X(get) X(set) X(target) X(meta) X(arguments) X(eval) \
    X(constructor) X(as) X(from) X(prototype)
#define KW_ENUM(n) KW_##n,
enum { KEYWORDS(KW_ENUM) KW_COUNT };
#undef KW_ENUM
#define KW_STR(n) #n,
static const char *kw_names[KW_COUNT] = { KEYWORDS(KW_STR) };
#undef KW_STR

typedef struct Tok {
    int t;
    uint32_t start, end;
    int line;
    bool nl, esc, octal, tail, bad_escape;
    double num;
    JSValue v, v2;
} Tok;

typedef struct Chunk { struct Chunk *next; size_t used, cap; char data[]; } Chunk;

struct Parser {
    JSContext *ctx;
    const uint16_t *s;
    uint32_t n, pos;
    int line;
    Tok tok, ahead;
    bool has_ahead;
    uint32_t prev_end;
    int prev_line;
    Chunk *arena;
    Func *fn;
    bool no_in, in_class_body;
    bool failed;
    char err[200];
    int err_line;
    JSValue kw[KW_COUNT];
    JSSB sb;
    int field_keys;
    int depth;
    /* ES modules: the modules it imports, the prologue made for its imports / exports */
    JSValue *req;
    int nreq, reqcap;
    Node *pro_ns, **pro_ns_tail, *pro_exp, **pro_exp_tail;
    JSValue a_imp, a_exp, a_meta;
    int nre;
};

/* ==== arena ================================================================================= */
void *js_parser_alloc(Parser *p, size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (!p->arena || p->arena->used + n > p->arena->cap) {
        size_t cap = n > 60000 ? n : 65536 - sizeof(Chunk);
        Chunk *c = malloc(sizeof(Chunk) + cap);
        if (!c) {
            static char dummy[512];
            p->failed = true;
            if (!p->err[0]) snprintf(p->err, sizeof p->err, "out of memory while parsing");
            memset(dummy, 0, sizeof dummy);
            return n <= sizeof dummy ? dummy : NULL;
        }
        c->used = 0;
        c->cap = cap;
        c->next = p->arena;
        p->arena = c;
    }
    void *r = p->arena->data + p->arena->used;
    p->arena->used += n;
    memset(r, 0, n);
    return r;
}

void js_parser_free(Parser *p) {
    if (!p) return;
    Chunk *c = p->arena;
    while (c) { Chunk *nx = c->next; free(c); c = nx; }
    sb_free(&p->sb);
    free(p);
}

static void error_at(Parser *p, int line, const char *fmt, ...) {
    if (p->failed) return;
    p->failed = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof p->err, fmt, ap);
    va_end(ap);
    p->err_line = line;
    p->tok.t = TK_EOF;
    p->has_ahead = false;
    p->pos = p->n;
}
#define ERR(...) error_at(p, p->tok.line, __VA_ARGS__)

static Node *nnew(Parser *p, int k) {
    Node *x = js_parser_alloc(p, sizeof(Node));
    if (!x) { static Node d; memset(&d, 0, sizeof d); return &d; }
    x->k = (uint16_t)k;
    x->line = p->tok.line;
    x->pos = p->tok.start;
    x->v = x->v2 = JS_UNDEFINED;
    return x;
}

/* ==== lexer ==================================================================================== */
static inline uint32_t cat(Parser *p, uint32_t i) { return i < p->n ? p->s[i] : 0xFFFFFFFFu; }
static bool is_lt(uint32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }
static int hexval(uint32_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? (int)(c - 'a' + 10) : -1;
}

/* code point at i (surrogate pairs joined); *len = units */
static uint32_t cp_at(Parser *p, uint32_t i, int *len) {
    uint32_t c = cat(p, i);
    *len = 1;
    if (c >= 0xD800 && c < 0xDC00) {
        uint32_t d = cat(p, i + 1);
        if (d >= 0xDC00 && d < 0xE000) { *len = 2; return 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); }
    }
    return c;
}

static bool skip_space(Parser *p) {
    bool nl = false;
    bool line_start = p->pos == 0;
    for (;;) {
        uint32_t c = cat(p, p->pos);
        if (c == 0xFFFFFFFFu) return nl;
        if (c == '\n' || c == 0x2028 || c == 0x2029) { p->pos++; p->line++; nl = true; line_start = true; continue; }
        if (c == '\r') { p->pos++; if (cat(p, p->pos) == '\n') p->pos++; p->line++; nl = true; line_start = true; continue; }
        if (c == ' ' || c == '\t' || c == 0x0B || c == 0x0C || c == 0xA0 || c == 0xFEFF || (c > 0x7F && js_is_space(c))) { p->pos++; continue; }
        if (c == '/' && cat(p, p->pos + 1) == '/') {
        line_comment:
            while (p->pos < p->n && !is_lt(p->s[p->pos])) p->pos++;
            continue;
        }
        if (c == '/' && cat(p, p->pos + 1) == '*') {
            p->pos += 2;
            while (p->pos < p->n && !(p->s[p->pos] == '*' && cat(p, p->pos + 1) == '/')) {
                uint32_t d = p->s[p->pos];
                if (d == '\n' || d == 0x2028 || d == 0x2029 || (d == '\r' && cat(p, p->pos + 1) != '\n')) { p->line++; nl = true; line_start = true; }
                p->pos++;
            }
            if (p->pos >= p->n) { error_at(p, p->line, "unterminated comment"); return nl; }
            p->pos += 2;
            continue;
        }
        /* Annex B HTML-like comments */
        if (c == '<' && cat(p, p->pos + 1) == '!' && cat(p, p->pos + 2) == '-' && cat(p, p->pos + 3) == '-') goto line_comment;
        if (c == '-' && line_start && cat(p, p->pos + 1) == '-' && cat(p, p->pos + 2) == '>') goto line_comment;
        if (c == '#' && p->pos == 0 && cat(p, 1) == '!') goto line_comment;
        return nl;
    }
}

static void sbr(Parser *p) { p->sb.len = 0; p->sb.err = false; }

/* \uXXXX or \u{X...}: returns the code point or -1 */
static int32_t read_uescape(Parser *p, uint32_t *i) {
    uint32_t k = *i;   /* at 'u' */
    k++;
    int32_t v = 0;
    if (cat(p, k) == '{') {
        k++;
        int n = 0;
        while (hexval(cat(p, k)) >= 0) { v = v * 16 + hexval(cat(p, k)); if (v > 0x10FFFF) return -1; k++; n++; }
        if (!n || cat(p, k) != '}') return -1;
        k++;
    } else {
        for (int j = 0; j < 4; j++) { int h = hexval(cat(p, k)); if (h < 0) return -1; v = v * 16 + h; k++; }
    }
    *i = k;
    return v;
}

static void lex_ident(Parser *p, Tok *t) {
    sbr(p);
    uint32_t i = p->pos;
    bool esc = false;
    for (;;) {
        int l;
        uint32_t c = cp_at(p, i, &l);
        if (c == '\\' && cat(p, i + 1) == 'u') {
            uint32_t k = i + 1;
            int32_t v = read_uescape(p, &k);
            if (v < 0 || !(i == p->pos ? js_is_id_start((uint32_t)v) : js_is_id_part((uint32_t)v))) { error_at(p, p->line, "invalid escape in identifier"); return; }
            sb_putc(&p->sb, (uint32_t)v);
            esc = true;
            i = k;
            continue;
        }
        if (i == p->pos ? !js_is_id_start(c) : !js_is_id_part(c)) break;
        sb_putc(&p->sb, c);
        i += (uint32_t)l;
    }
    p->pos = i;
    t->t = TK_IDENT;
    t->esc = esc;
    t->v = js_atom16(p->ctx, p->sb.s, p->sb.len);
}

static void lex_number(Parser *p, Tok *t) {
    uint32_t i = p->pos;
    uint32_t c = cat(p, i);
    t->t = TK_NUM;
    if (c == '0' && ((cat(p, i + 1) | 0x20) == 'x' || (cat(p, i + 1) | 0x20) == 'o' || (cat(p, i + 1) | 0x20) == 'b')) {
        int base = (cat(p, i + 1) | 0x20) == 'x' ? 16 : (cat(p, i + 1) | 0x20) == 'o' ? 8 : 2;
        i += 2;
        double v = 0;
        int nd = 0;
        sbr(p);
        for (;;) {
            uint32_t d = cat(p, i);
            if (d == '_') { i++; continue; }
            int h = hexval(d);
            if (h < 0 || h >= base) break;
            v = v * base + h;
            sb_putc(&p->sb, d);
            nd++;
            i++;
        }
        if (!nd) { error_at(p, p->line, "invalid number"); return; }
        if (cat(p, i) == 'n') { i++; t->t = TK_BIGINT; t->v = js_atom16(p->ctx, p->sb.s, p->sb.len); t->v2 = js_mknum(base); }
        t->num = v;
        p->pos = i;
        if (js_is_id_start(cat(p, i)) || (cat(p, i) >= '0' && cat(p, i) <= '9')) error_at(p, p->line, "invalid number");
        return;
    }
    /* legacy octal 017 (sloppy) */
    if (c == '0' && cat(p, i + 1) >= '0' && cat(p, i + 1) <= '9') {
        uint32_t k = i + 1;
        bool octal = true;
        while (cat(p, k) >= '0' && cat(p, k) <= '9') { if (cat(p, k) >= '8') octal = false; k++; }
        if (octal && cat(p, k) != '.' && (cat(p, k) | 0x20) != 'e') {
            double v = 0;
            for (uint32_t j = i + 1; j < k; j++) v = v * 8 + (p->s[j] - '0');
            t->num = v;
            t->octal = true;
            p->pos = k;
            return;
        }
    }
    sbr(p);
    bool digits = false;
    while (cat(p, i) < 0x80 && ((cat(p, i) >= '0' && cat(p, i) <= '9') || cat(p, i) == '_')) { if (cat(p, i) != '_') sb_putc(&p->sb, cat(p, i)); i++; digits = true; }
    bool isint = true;
    if (cat(p, i) == 'n' && digits) {
        i++;
        t->t = TK_BIGINT;
        t->v = js_atom16(p->ctx, p->sb.s, p->sb.len);
        t->v2 = js_mknum(10);
        double v = 0;
        for (uint32_t k = 0; k < p->sb.len; k++) v = v * 10 + (p->sb.s[k] - '0');
        t->num = v;
        p->pos = i;
        return;
    }
    if (cat(p, i) == '.') {
        isint = false;
        sb_putc(&p->sb, '.');
        i++;
        while ((cat(p, i) >= '0' && cat(p, i) <= '9') || cat(p, i) == '_') { if (cat(p, i) != '_') sb_putc(&p->sb, cat(p, i)); i++; }
    }
    if ((cat(p, i) | 0x20) == 'e') {
        uint32_t k = i + 1;
        if (cat(p, k) == '+' || cat(p, k) == '-') k++;
        if (cat(p, k) >= '0' && cat(p, k) <= '9') {
            isint = false;
            sb_putc(&p->sb, 'e');
            if (cat(p, i + 1) == '-') sb_putc(&p->sb, '-');
            i = k;
            while ((cat(p, i) >= '0' && cat(p, i) <= '9') || cat(p, i) == '_') { if (cat(p, i) != '_') sb_putc(&p->sb, cat(p, i)); i++; }
        }
    }
    (void)isint;
    uint32_t used;
    t->num = js_strtod16(p->sb.s, p->sb.len, &used, false);
    p->pos = i;
    if (js_is_id_start(cat(p, i))) error_at(p, p->line, "invalid or unexpected token");
}

/* string escapes shared by strings and templates; returns false on a bad escape */
static bool lex_escape(Parser *p, uint32_t *ip, bool tmpl, bool *octal) {
    uint32_t i = *ip;   /* after the backslash */
    uint32_t c = cat(p, i);
    switch (c) {
    case 'n': sb_putc(&p->sb, '\n'); i++; break;
    case 't': sb_putc(&p->sb, '\t'); i++; break;
    case 'r': sb_putc(&p->sb, '\r'); i++; break;
    case 'b': sb_putc(&p->sb, '\b'); i++; break;
    case 'f': sb_putc(&p->sb, '\f'); i++; break;
    case 'v': sb_putc(&p->sb, 0x0B); i++; break;
    case '\r': i++; if (cat(p, i) == '\n') i++; p->line++; break;
    case '\n': case 0x2028: case 0x2029: i++; p->line++; break;
    case 'x': {
        int a = hexval(cat(p, i + 1)), b = hexval(cat(p, i + 2));
        if (a < 0 || b < 0) { *ip = i; return false; }
        sb_putc(&p->sb, (uint32_t)(a * 16 + b));
        i += 3;
        break;
    }
    case 'u': {
        int32_t v = read_uescape(p, &i);
        if (v < 0) { *ip = i; return false; }
        sb_putc(&p->sb, (uint32_t)v);
        break;
    }
    default:
        if (c >= '0' && c <= '7') {
            if (c == '0' && !(cat(p, i + 1) >= '0' && cat(p, i + 1) <= '9')) { sb_putc(&p->sb, 0); i++; break; }
            if (tmpl) { *ip = i; return false; }
            *octal = true;
            uint32_t v = c - '0';
            i++;
            if (cat(p, i) >= '0' && cat(p, i) <= '7') { v = v * 8 + (cat(p, i) - '0'); i++;
                if (c <= '3' && cat(p, i) >= '0' && cat(p, i) <= '7') { v = v * 8 + (cat(p, i) - '0'); i++; } }
            sb_putc(&p->sb, v);
            break;
        }
        if ((c == '8' || c == '9') && tmpl) { *ip = i; return false; }
        if (c == 0xFFFFFFFFu) { *ip = i; return false; }
        {
            int l;
            uint32_t cp = cp_at(p, i, &l);
            sb_putc(&p->sb, cp);
            i += (uint32_t)l;
        }
        break;
    }
    *ip = i;
    return true;
}

static void lex_string(Parser *p, Tok *t) {
    uint32_t q = cat(p, p->pos);
    uint32_t i = p->pos + 1;
    sbr(p);
    bool octal = false;
    for (;;) {
        uint32_t c = cat(p, i);
        if (c == 0xFFFFFFFFu || c == '\n' || c == '\r') { error_at(p, p->line, "unterminated string literal"); return; }
        if (c == q) { i++; break; }
        if (c == '\\') {
            i++;
            if (!lex_escape(p, &i, false, &octal)) { error_at(p, p->line, "invalid escape sequence"); return; }
            continue;
        }
        sb_putc(&p->sb, c);
        i++;
    }
    p->pos = i;
    t->t = TK_STR;
    t->octal = octal;
    t->v = js_atom16(p->ctx, p->sb.s, p->sb.len);
}

/* a template chunk starting after ` or } */
static void lex_template_chunk(Parser *p, Tok *t) {
    uint32_t i = p->pos;
    sbr(p);
    bool bad = false, octal = false;
    JSSB raw;
    sb_init(p->ctx, &raw);
    for (;;) {
        uint32_t c = cat(p, i);
        if (c == 0xFFFFFFFFu) { sb_free(&raw); error_at(p, p->line, "unterminated template literal"); return; }
        if (c == '`') { i++; t->tail = true; break; }
        if (c == '$' && cat(p, i + 1) == '{') { i += 2; t->tail = false; break; }
        if (c == '\\') {
            uint32_t st = i;
            i++;
            uint32_t save_len = p->sb.len;
            if (!lex_escape(p, &i, true, &octal)) {
                bad = true;
                p->sb.len = save_len;
                /* skip the bad escape's character */
                if (i < p->n) i++;
            }
            for (uint32_t k = st; k < i; k++) {
                uint32_t r = p->s[k];
                if (r == '\r') { sb_putc(&raw, '\n'); if (cat(p, k + 1) == '\n') k++; continue; }
                sb_putc(&raw, r);
            }
            continue;
        }
        if (c == '\r') { sb_putc(&p->sb, '\n'); sb_putc(&raw, '\n'); i++; if (cat(p, i) == '\n') i++; p->line++; continue; }
        if (c == '\n' || c == 0x2028 || c == 0x2029) p->line++;
        sb_putc(&p->sb, c);
        sb_putc(&raw, c);
        i++;
    }
    p->pos = i;
    t->t = TK_TEMPLATE;
    t->bad_escape = bad;
    t->v = bad ? JS_UNDEFINED : js_atom16(p->ctx, p->sb.s, p->sb.len);
    t->v2 = js_atom16(p->ctx, raw.s ? raw.s : (const uint16_t *)"", raw.len);
    sb_free(&raw);
}

static void lex(Parser *p, Tok *t) {
    t->nl = skip_space(p);
    t->esc = t->octal = t->bad_escape = false;
    t->v = t->v2 = JS_UNDEFINED;
    t->start = p->pos;
    t->line = p->line;
    if (p->failed) { t->t = TK_EOF; t->end = p->pos; return; }
    uint32_t c = cat(p, p->pos);
    if (c == 0xFFFFFFFFu) { t->t = TK_EOF; t->end = p->pos; return; }
    int l;
    uint32_t cp = cp_at(p, p->pos, &l);
    if (js_is_id_start(cp) || c == '\\') { lex_ident(p, t); t->end = p->pos; return; }
    if ((c >= '0' && c <= '9') || (c == '.' && cat(p, p->pos + 1) >= '0' && cat(p, p->pos + 1) <= '9')) { lex_number(p, t); t->end = p->pos; return; }
    if (c == '"' || c == '\'') { lex_string(p, t); t->end = p->pos; return; }
    if (c == '`') { p->pos++; lex_template_chunk(p, t); t->end = p->pos; return; }
    if (c == '#') {
        p->pos++;
        if (!js_is_id_start(cat(p, p->pos)) && cat(p, p->pos) != '\\') { error_at(p, p->line, "invalid or unexpected token '#'"); t->t = TK_EOF; return; }
        Tok tmp;
        lex_ident(p, &tmp);
        JSSB b;
        sb_init(p->ctx, &b);
        sb_putc(&b, '#');
        sb_putv(&b, tmp.v);
        JSValue s = sb_finish(&b);
        t->t = TK_PRIVATE;
        t->v = JS_IsException(s) ? JS_UNDEFINED : js_atom(p->ctx, s);
        t->end = p->pos;
        return;
    }
    uint32_t c1 = cat(p, p->pos + 1), c2 = cat(p, p->pos + 2), c3 = cat(p, p->pos + 3);
    int tt = (int)c, len = 1;
    switch (c) {
    case '=':
        if (c1 == '=') { if (c2 == '=') { tt = TK_SEQ; len = 3; } else { tt = TK_EQ; len = 2; } }
        else if (c1 == '>') { tt = TK_ARROW; len = 2; }
        break;
    case '!': if (c1 == '=') { if (c2 == '=') { tt = TK_SNE; len = 3; } else { tt = TK_NE; len = 2; } } break;
    case '<':
        if (c1 == '<') { if (c2 == '=') { tt = TK_SHL_A; len = 3; } else { tt = TK_SHL; len = 2; } }
        else if (c1 == '=') { tt = TK_LE; len = 2; }
        break;
    case '>':
        if (c1 == '>') {
            if (c2 == '>') { if (c3 == '=') { tt = TK_SHR_A; len = 4; } else { tt = TK_SHR; len = 3; } }
            else if (c2 == '=') { tt = TK_SAR_A; len = 3; }
            else { tt = TK_SAR; len = 2; }
        } else if (c1 == '=') { tt = TK_GE; len = 2; }
        break;
    case '+': if (c1 == '+') { tt = TK_INC; len = 2; } else if (c1 == '=') { tt = TK_ADD_A; len = 2; } break;
    case '-': if (c1 == '-') { tt = TK_DEC; len = 2; } else if (c1 == '=') { tt = TK_SUB_A; len = 2; } break;
    case '*':
        if (c1 == '*') { if (c2 == '=') { tt = TK_POW_A; len = 3; } else { tt = TK_POW; len = 2; } }
        else if (c1 == '=') { tt = TK_MUL_A; len = 2; }
        break;
    case '/': if (c1 == '=') { tt = TK_DIV_A; len = 2; } break;
    case '%': if (c1 == '=') { tt = TK_MOD_A; len = 2; } break;
    case '&':
        if (c1 == '&') { if (c2 == '=') { tt = TK_LAND_A; len = 3; } else { tt = TK_AND; len = 2; } }
        else if (c1 == '=') { tt = TK_AND_A; len = 2; }
        break;
    case '|':
        if (c1 == '|') { if (c2 == '=') { tt = TK_LOR_A; len = 3; } else { tt = TK_OR; len = 2; } }
        else if (c1 == '=') { tt = TK_OR_A; len = 2; }
        break;
    case '^': if (c1 == '=') { tt = TK_XOR_A; len = 2; } break;
    case '?':
        if (c1 == '?') { if (c2 == '=') { tt = TK_NULLISH_A; len = 3; } else { tt = TK_NULLISH; len = 2; } }
        else if (c1 == '.' && !(c2 >= '0' && c2 <= '9')) { tt = TK_OPTCHAIN; len = 2; }
        break;
    case '.': if (c1 == '.' && c2 == '.') { tt = TK_ELLIPSIS; len = 3; } break;
    case '{': case '}': case '(': case ')': case '[': case ']': case ';': case ',': case '~': case ':':
        break;
    default:
        error_at(p, p->line, "invalid or unexpected token");
        t->t = TK_EOF;
        return;
    }
    p->pos += (uint32_t)len;
    t->t = tt;
    t->end = p->pos;
}

static void next(Parser *p) {
    p->prev_end = p->tok.end;
    p->prev_line = p->tok.line;
    if (p->has_ahead) { p->tok = p->ahead; p->has_ahead = false; return; }
    lex(p, &p->tok);
}

static Tok *peek(Parser *p) {
    if (!p->has_ahead) {
        int line = p->line;
        uint32_t pos = p->pos;
        lex(p, &p->ahead);
        (void)line; (void)pos;
        p->has_ahead = true;
    }
    return &p->ahead;
}

/* rescan the current token ('/' or '/=') as a regular expression literal */
static void lex_regexp(Parser *p) {
    p->has_ahead = false;
    uint32_t i = p->tok.start + 1;
    bool cls = false;
    sbr(p);
    for (;;) {
        uint32_t c = cat(p, i);
        if (c == 0xFFFFFFFFu || is_lt(c)) { error_at(p, p->tok.line, "unterminated regular expression"); return; }
        if (c == '\\') {
            sb_putc(&p->sb, c);
            i++;
            c = cat(p, i);
            if (c == 0xFFFFFFFFu || is_lt(c)) { error_at(p, p->tok.line, "unterminated regular expression"); return; }
            sb_putc(&p->sb, c);
            i++;
            continue;
        }
        if (c == '[') cls = true;
        else if (c == ']') cls = false;
        else if (c == '/' && !cls) { i++; break; }
        sb_putc(&p->sb, c);
        i++;
    }
    JSValue pat = js_atom16(p->ctx, p->sb.s, p->sb.len);
    sbr(p);
    while (js_is_id_part(cat(p, i))) { sb_putc(&p->sb, cat(p, i)); i++; }
    p->tok.t = TK_REGEXP;
    p->tok.v = pat;
    p->tok.v2 = js_atom16(p->ctx, p->sb.s, p->sb.len);
    p->pos = i;
    p->tok.end = i;
}

/* the parser sits on '}' that closes a template substitution: continue the template */
static void lex_template_cont(Parser *p) {
    p->has_ahead = false;
    p->pos = p->tok.start + 1;
    lex_template_chunk(p, &p->tok);
    p->tok.end = p->pos;
}

/* ==== token helpers ============================================================================ */
static inline bool is(Parser *p, int t) { return p->tok.t == t; }
static inline bool is_kw(Parser *p, int kw) { return p->tok.t == TK_IDENT && !p->tok.esc && p->tok.v == p->kw[kw]; }
static inline bool tok_kw(Parser *p, const Tok *t, int kw) { return t->t == TK_IDENT && !t->esc && t->v == p->kw[kw]; }
static bool eat(Parser *p, int t) { if (p->tok.t == t) { next(p); return true; } return false; }
static bool eat_kw(Parser *p, int kw) { if (is_kw(p, kw)) { next(p); return true; } return false; }

static const char *tok_desc(Parser *p, char *buf, size_t n) {
    switch (p->tok.t) {
    case TK_EOF: return "end of input";
    case TK_IDENT: { char t[64]; js_str_to_ascii(p->ctx, p->tok.v, t, sizeof t); snprintf(buf, n, "'%s'", t); return buf; }
    case TK_NUM: return "number";
    case TK_STR: return "string";
    case TK_TEMPLATE: return "template string";
    default:
        if (p->tok.t < 256) { snprintf(buf, n, "'%c'", p->tok.t); return buf; }
        {
            uint32_t len = p->tok.end - p->tok.start;
            char t[16];
            uint32_t k;
            for (k = 0; k < len && k < 15; k++) t[k] = (char)p->s[p->tok.start + k];
            t[k] = 0;
            snprintf(buf, n, "'%s'", t);
            return buf;
        }
    }
}

static void unexpected(Parser *p) {
    char b[80];
    ERR("Unexpected token %s", tok_desc(p, b, sizeof b));
}

static void expect(Parser *p, int t) {
    if (p->tok.t == t) { next(p); return; }
    if (p->failed) return;
    char b[80], want[8];
    if (t < 256) snprintf(want, sizeof want, "'%c'", t); else snprintf(want, sizeof want, "token");
    ERR("Unexpected token %s (expected %s)", tok_desc(p, b, sizeof b), want);
}

static void semicolon(Parser *p) {
    if (eat(p, ';')) return;
    if (is(p, '}') || is(p, TK_EOF) || p->tok.nl) return;
    unexpected(p);
}

static bool strict(Parser *p) { return p->fn && (p->fn->flags & F_STRICT); }

/* reserved words that can never be identifiers */
static bool reserved(Parser *p, const Tok *t) {
    if (t->t != TK_IDENT || t->esc) return false;
    for (int k = KW_break; k <= KW_with; k++) if (t->v == p->kw[k]) return true;
    if (strict(p)) for (int k = KW_yield; k <= KW_public; k++) if (t->v == p->kw[k]) return true;
    if (t->v == p->kw[KW_yield] && p->fn && (p->fn->flags & F_GEN)) return true;
    if (t->v == p->kw[KW_await] && p->fn && (p->fn->flags & (F_ASYNC | F_MODULE))) return true;
    return false;
}

static bool is_ident_tok(Parser *p) { return p->tok.t == TK_IDENT && !reserved(p, &p->tok); }

static JSValue ident_name(Parser *p) {
    if (!is_ident_tok(p)) { unexpected(p); return JS_UNDEFINED; }
    JSValue v = p->tok.v;
    next(p);
    return v;
}

/* ==== forward declarations ==================================================================== */
static Node *parse_assign(Parser *p);
static Node *parse_expr(Parser *p);
static Node *parse_stmt(Parser *p);
static Node *parse_stmt_inner(Parser *p);
static Node *parse_unary(Parser *p);
static Node *parse_lhs(Parser *p, bool allow_call);
static Node *parse_binding_target(Parser *p);
static Func *parse_function_rest(Parser *p, int kind, uint32_t fflags, JSValue name, uint32_t start, int line);
static Node *parse_class(Parser *p, bool decl);
static Node *parse_function_body_list(Parser *p, Func *f);

static Func *new_func(Parser *p, int kind, uint32_t fflags) {
    Func *f = js_parser_alloc(p, sizeof(Func));
    if (!f) { static Func d; memset(&d, 0, sizeof d); return &d; }
    f->parent = p->fn;
    f->kind = kind;
    f->flags = fflags;
    f->name = JS_UNDEFINED;
    f->simple_params = true;
    f->rest_slot = -1;
    if (p->fn && (p->fn->flags & F_STRICT)) f->flags |= F_STRICT;
    return f;
}

/* ==== patterns ========================================================================================= */
static Node *to_pattern(Parser *p, Node *e, bool binding);

static Node *to_pattern_elem(Parser *p, Node *e, bool binding) {
    if (e->k == N_ASSIGN && e->op == '=') {
        Node *x = nnew(p, N_ASSIGN_PAT);
        x->line = e->line;
        x->a = to_pattern(p, e->a, binding);
        x->b = e->b;
        return x;
    }
    return to_pattern(p, e, binding);
}

static Node *to_pattern(Parser *p, Node *e, bool binding) {
    if (p->failed) return e;
    switch (e->k) {
    case N_IDENT:
        if (strict(p) && (e->v == p->kw[KW_eval] || e->v == p->kw[KW_arguments])) error_at(p, e->line, "Unexpected eval or arguments in strict mode");
        return e;
    case N_MEMBER: case N_INDEX: case N_PRIVMEMBER:
        if (binding || (e->flags & NF_OPTIONAL)) break;
        return e;
    case N_ASSIGN_PAT: case N_ARRAY_PAT: case N_OBJECT_PAT:
        return e;
    case N_ARRAY: {
        if (e->flags & NF_PAREN) break;
        Node *x = nnew(p, N_ARRAY_PAT);
        x->line = e->line;
        Node **tail = &x->a;
        for (Node *el = e->a; el; el = el->next) {
            Node *pe;
            if (el->k == N_HOLE) pe = el;
            else if (el->k == N_SPREAD) {
                if (el->next) { error_at(p, el->line, "Rest element must be last element"); return e; }
                pe = nnew(p, N_REST);
                pe->a = to_pattern(p, el->a, binding);
            } else pe = to_pattern_elem(p, el, binding);
            Node *c = pe;
            if (c == el) { /* reuse node: clear the link first */ }
            *tail = c;
            tail = &c->next;
        }
        *tail = NULL;
        return x;
    }
    case N_OBJECT: {
        if (e->flags & NF_PAREN) break;
        Node *x = nnew(p, N_OBJECT_PAT);
        x->line = e->line;
        Node **tail = &x->a;
        for (Node *pr = e->a; pr; pr = pr->next) {
            Node *np = nnew(p, N_PROP);
            np->line = pr->line;
            np->flags = pr->flags & (NF_COMPUTED | NF_SHORTHAND);
            np->a = pr->a;
            np->v = pr->v;
            if (pr->op == PK_SPREAD) {
                if (pr->next) { error_at(p, pr->line, "Rest element must be last element"); return e; }
                np->op = PK_REST;
                np->b = to_pattern(p, pr->b, binding);
            } else if (pr->op == PK_INIT || pr->op == PK_PROTO) {
                np->op = PK_INIT;
                np->b = to_pattern_elem(p, pr->b, binding);
            } else { error_at(p, pr->line, "Invalid destructuring assignment target"); return e; }
            *tail = np;
            tail = &np->next;
        }
        return x;
    }
    default: break;
    }
    error_at(p, e->line, "Invalid destructuring assignment target");
    return e;
}

/* ==== primary expressions ================================================================================ */
static Node *parse_template(Parser *p, Node *tag) {
    Node *t = nnew(p, N_TEMPLATE);
    Node **qt = &t->a, **et = &t->b;
    for (;;) {
        if (!is(p, TK_TEMPLATE)) { unexpected(p); return t; }
        if (p->tok.bad_escape && !tag) { ERR("Invalid escape sequence in template"); return t; }
        Node *q = nnew(p, N_TQUASI);
        q->v = p->tok.v;
        q->v2 = p->tok.v2;
        *qt = q;
        qt = &q->next;
        if (p->tok.tail) { next(p); break; }
        next(p);
        bool save = p->no_in;
        p->no_in = false;
        Node *e = parse_expr(p);
        p->no_in = save;
        *et = e;
        et = &e->next;
        if (!is(p, '}')) { unexpected(p); return t; }
        lex_template_cont(p);
    }
    if (tag) {
        Node *x = nnew(p, N_TAGGED);
        x->a = tag;
        x->b = t;
        x->line = tag->line;
        return x;
    }
    return t;
}

static Node *parse_array_lit(Parser *p) {
    Node *x = nnew(p, N_ARRAY);
    next(p);
    Node **tail = &x->a;
    bool save = p->no_in;
    p->no_in = false;
    while (!is(p, ']') && !p->failed) {
        Node *e;
        if (is(p, ',')) { e = nnew(p, N_HOLE); next(p); *tail = e; tail = &e->next; continue; }
        if (is(p, TK_ELLIPSIS)) { e = nnew(p, N_SPREAD); next(p); e->a = parse_assign(p); }
        else e = parse_assign(p);
        *tail = e;
        tail = &e->next;
        if (!is(p, ']')) expect(p, ',');
    }
    p->no_in = save;
    expect(p, ']');
    return x;
}

/* property name: identifier name, string, number, [computed], #private (classes) */
static Node *parse_prop_key(Parser *p, uint32_t *flags, bool allow_private) {
    Node *k;
    if (is(p, '[')) {
        next(p);
        bool save = p->no_in;
        p->no_in = false;
        k = parse_assign(p);
        p->no_in = save;
        expect(p, ']');
        *flags |= NF_COMPUTED;
        return k;
    }
    if (is(p, TK_IDENT)) { k = nnew(p, N_STR); k->v = p->tok.v; next(p); return k; }
    if (is(p, TK_STR)) { k = nnew(p, N_STR); k->v = p->tok.v; next(p); return k; }
    if (is(p, TK_NUM) || is(p, TK_BIGINT)) {
        k = nnew(p, N_STR);
        k->v = js_atom(p->ctx, js_num_to_str(p->ctx, p->tok.num));
        next(p);
        return k;
    }
    if (is(p, TK_PRIVATE) && allow_private) { k = nnew(p, N_STR); k->v = p->tok.v; *flags |= NF_PRIVATE; next(p); return k; }
    unexpected(p);
    return nnew(p, N_STR);
}

static Node *method_value(Parser *p, int kind, uint32_t fflags, Node *key, uint32_t start, int line) {
    JSValue name = key->k == N_STR && !(key->flags & NF_COMPUTED) ? key->v : JS_UNDEFINED;
    Func *f = parse_function_rest(p, kind, fflags | F_METHOD, name, start, line);
    Node *fn = nnew(p, N_FUNC);
    fn->fn = f;
    fn->line = line;
    return fn;
}

static Node *parse_object_lit(Parser *p) {
    Node *x = nnew(p, N_OBJECT);
    next(p);
    Node **tail = &x->a;
    bool save = p->no_in;
    p->no_in = false;
    while (!is(p, '}') && !p->failed) {
        Node *pr = nnew(p, N_PROP);
        uint32_t start = p->tok.start;
        int line = p->tok.line;
        if (is(p, TK_ELLIPSIS)) {
            next(p);
            pr->op = PK_SPREAD;
            pr->b = parse_assign(p);
        } else {
            uint32_t fflags = 0;
            int kind = PK_INIT;
            Tok *pk = NULL;
            if (is_kw(p, KW_async) && (pk = peek(p), pk->t != ',' && pk->t != ':' && pk->t != '(' && pk->t != '}' && pk->t != '=' && !pk->nl)) {
                next(p);
                fflags |= F_ASYNC;
                kind = PK_METHOD;
            }
            if (is(p, '*')) { next(p); fflags |= F_GEN; kind = PK_METHOD; }
            if ((is_kw(p, KW_get) || is_kw(p, KW_set)) && kind == PK_INIT &&
                (pk = peek(p), pk->t != ',' && pk->t != ':' && pk->t != '(' && pk->t != '}' && pk->t != '=')) {
                kind = is_kw(p, KW_get) ? PK_GET : PK_SET;
                next(p);
            }
            uint32_t kf = 0;
            bool ident_key = is(p, TK_IDENT);
            Tok keytok = p->tok;
            Node *key = parse_prop_key(p, &kf, false);
            pr->a = key;
            pr->flags |= kf;
            if (kind == PK_GET || kind == PK_SET) {
                pr->op = (uint16_t)kind;
                pr->b = method_value(p, kind == PK_GET ? FK_GETTER : FK_SETTER, 0, key, start, line);
            } else if (kind == PK_METHOD || is(p, '(')) {
                pr->op = PK_METHOD;
                pr->b = method_value(p, FK_METHOD, fflags, key, start, line);
            } else if (eat(p, ':')) {
                pr->b = parse_assign(p);
                if (!(kf & NF_COMPUTED) && key->v == p->ctx->a[A___proto__]) pr->op = PK_PROTO;
            } else if (ident_key && !(kf & NF_COMPUTED)) {
                /* shorthand: {a} or {a = 1} (the latter only as a pattern) */
                if (reserved(p, &keytok) && keytok.v != p->kw[KW_yield] && keytok.v != p->kw[KW_await]) { error_at(p, line, "Unexpected reserved word"); break; }
                Node *id = nnew(p, N_IDENT);
                id->v = key->v;
                id->line = line;
                pr->flags |= NF_SHORTHAND;
                if (is(p, '=')) {
                    next(p);
                    Node *as = nnew(p, N_ASSIGN);
                    as->op = '=';
                    as->a = id;
                    as->b = parse_assign(p);
                    pr->b = as;
                    pr->flags |= NF_COVERINIT;
                    x->flags |= NF_COVERINIT;
                } else pr->b = id;
            } else { unexpected(p); break; }
        }
        *tail = pr;
        tail = &pr->next;
        if (!is(p, '}')) expect(p, ',');
    }
    p->no_in = save;
    expect(p, '}');
    return x;
}

/* function / async function / generator expression */
static Node *parse_func_expr(Parser *p, uint32_t fflags, uint32_t start, int line) {
    if (is(p, '*')) { next(p); fflags |= F_GEN; }
    JSValue name = JS_UNDEFINED;
    if (is(p, TK_IDENT) && !is(p, '(')) {
        /* the name is bound inside: yield / await rules of the function itself */
        Func tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.flags = fflags | (strict(p) ? F_STRICT : 0);
        Func *save = p->fn;
        p->fn = &tmp;
        name = ident_name(p);
        p->fn = save;
    }
    Func *f = parse_function_rest(p, FK_NORMAL, fflags, name, start, line);
    f->is_expr_named = !JS_IsUndefined(name);
    Node *n = nnew(p, N_FUNC);
    n->fn = f;
    n->line = line;
    return n;
}

static Node *parse_arrow_from(Parser *p, Node *params, uint32_t fflags, uint32_t start, int line);

/* ( ... ) : a parenthesized expression or arrow parameters */
static Node *parse_paren(Parser *p) {
    uint32_t start = p->tok.start;
    int line = p->tok.line;
    next(p);
    bool save = p->no_in;
    p->no_in = false;
    if (is(p, ')')) {
        next(p);
        p->no_in = save;
        if (!is(p, TK_ARROW) || p->tok.nl) { unexpected(p); return nnew(p, N_EMPTY); }
        return parse_arrow_from(p, NULL, 0, start, line);
    }
    Node *list = NULL, **tail = &list;
    int n = 0;
    bool rest = false, trailing = false;
    while (!p->failed) {
        Node *e;
        if (is(p, TK_ELLIPSIS)) {
            next(p);
            e = nnew(p, N_REST);
            e->a = parse_binding_target(p);
            rest = true;
        } else e = parse_assign(p);
        *tail = e;
        tail = &e->next;
        n++;
        if (rest) break;
        if (!eat(p, ',')) break;
        if (is(p, ')')) { trailing = true; break; }
    }
    expect(p, ')');
    p->no_in = save;
    if (is(p, TK_ARROW) && !p->tok.nl) return parse_arrow_from(p, list, 0, start, line);
    if (rest || trailing) { unexpected(p); return list ? list : nnew(p, N_EMPTY); }
    Node *r;
    if (n == 1) r = list;
    else { r = nnew(p, N_SEQ); r->a = list; r->line = line; }
    r->flags |= NF_PAREN;
    return r;
}

static Node *parse_primary(Parser *p) {
    Node *x;
    uint32_t start = p->tok.start;
    int line = p->tok.line;
    switch (p->tok.t) {
    case TK_NUM:
        if (p->tok.octal && strict(p)) { ERR("Octal literals are not allowed in strict mode"); }
        x = nnew(p, N_NUM);
        x->num = p->tok.num;
        next(p);
        return x;
    case TK_BIGINT:
        x = nnew(p, N_BIGINT);
        x->num = p->tok.num;
        x->v = p->tok.v;
        x->v2 = p->tok.v2;
        next(p);
        return x;
    case TK_STR:
        if (p->tok.octal && strict(p)) { ERR("Octal escape sequences are not allowed in strict mode"); }
        x = nnew(p, N_STR);
        x->v = p->tok.v;
        next(p);
        return x;
    case TK_TEMPLATE:
        return parse_template(p, NULL);
    case '/': case TK_DIV_A:
        lex_regexp(p);
        x = nnew(p, N_REGEXP);
        x->v = p->tok.v;
        x->v2 = p->tok.v2;
        next(p);
        return x;
    case '[': return parse_array_lit(p);
    case '{': return parse_object_lit(p);
    case '(': return parse_paren(p);
    case TK_PRIVATE: {
        /* #x in obj */
        x = nnew(p, N_PRIVIN);
        x->v = p->tok.v;
        next(p);
        if (!is_kw(p, KW_in)) { unexpected(p); return x; }
        return x;
    }
    case TK_IDENT:
        if (!p->tok.esc) {
            JSValue v = p->tok.v;
            if (v == p->kw[KW_function]) { next(p); return parse_func_expr(p, 0, start, line); }
            if (v == p->kw[KW_class]) return parse_class(p, false);
            if (v == p->kw[KW_this]) { next(p); return nnew(p, N_THIS); }
            if (v == p->kw[KW_null]) { next(p); x = nnew(p, N_IDENT); x->k = N_NUM; x->op = 1; return x; }
            if (v == p->kw[KW_true] || v == p->kw[KW_false]) { x = nnew(p, N_NUM); x->op = v == p->kw[KW_true] ? 3 : 2; next(p); return x; }
            if (v == p->kw[KW_async]) {
                Tok *pk = peek(p);
                if (tok_kw(p, pk, KW_function) && !pk->nl) { next(p); next(p); return parse_func_expr(p, F_ASYNC, start, line); }
                if (pk->t == TK_IDENT && !pk->nl && !reserved(p, pk)) {
                    /* async x => ... */
                    next(p);
                    Func tmp;
                    memset(&tmp, 0, sizeof tmp);
                    tmp.flags = F_ASYNC | (strict(p) ? F_STRICT : 0);
                    Func *sv = p->fn;
                    p->fn = &tmp;
                    Node *id = nnew(p, N_IDENT);
                    id->v = ident_name(p);
                    p->fn = sv;
                    if (!is(p, TK_ARROW) || p->tok.nl) { unexpected(p); return id; }
                    return parse_arrow_from(p, id, F_ASYNC, start, line);
                }
            }
            if (v == p->kw[KW_new]) {
                next(p);
                if (eat(p, '.')) {
                    if (!is_kw(p, KW_target)) { unexpected(p); return nnew(p, N_EMPTY); }
                    next(p);
                    return nnew(p, N_NEW_TARGET);
                }
                x = nnew(p, N_NEW);
                x->line = line;
                x->a = parse_lhs(p, false);
                if (is(p, '(')) {
                    next(p);
                    Node **tail = &x->b;
                    bool save = p->no_in;
                    p->no_in = false;
                    while (!is(p, ')') && !p->failed) {
                        Node *e;
                        if (is(p, TK_ELLIPSIS)) { e = nnew(p, N_SPREAD); next(p); e->a = parse_assign(p); }
                        else e = parse_assign(p);
                        *tail = e;
                        tail = &e->next;
                        if (!is(p, ')')) expect(p, ',');
                    }
                    p->no_in = save;
                    expect(p, ')');
                }
                return x;
            }
            if (v == p->kw[KW_super]) {
                next(p);
                if (is(p, '(')) { x = nnew(p, N_SUPER_CALL); return x; }   /* args parsed by the caller */
                x = nnew(p, N_SUPER_MEMBER);
                if (eat(p, '.')) {
                    if (!is(p, TK_IDENT)) { unexpected(p); return x; }
                    x->v = p->tok.v;
                    next(p);
                } else if (eat(p, '[')) {
                    x->a = parse_expr(p);
                    x->flags |= NF_COMPUTED;
                    expect(p, ']');
                } else unexpected(p);
                return x;
            }
            if (v == p->kw[KW_import]) {
                next(p);
                if (eat(p, '.')) {
                    if (!is_kw(p, KW_meta)) { unexpected(p); return nnew(p, N_EMPTY); }
                    next(p);
                    if (!JS_IsUndefined(p->a_meta)) { x = nnew(p, N_IDENT); x->v = p->a_meta; return x; }
                    return nnew(p, N_IMPORT_META);
                }
                expect(p, '(');
                x = nnew(p, N_IMPORT_CALL);
                x->a = parse_assign(p);
                if (eat(p, ',') && !is(p, ')')) { parse_assign(p); eat(p, ','); }
                expect(p, ')');
                return x;
            }
        }
        if (!is_ident_tok(p)) { unexpected(p); return nnew(p, N_EMPTY); }
        x = nnew(p, N_IDENT);
        x->v = p->tok.v;
        next(p);
        if (is(p, TK_ARROW) && !p->tok.nl) return parse_arrow_from(p, x, 0, start, line);
        return x;
    default:
        unexpected(p);
        return nnew(p, N_EMPTY);
    }
}

/* ==== left-hand side: members, calls, optional chains ======================================================== */
static Node *parse_args(Parser *p, Node *call) {
    next(p);   /* ( */
    Node **tail = &call->b;
    bool save = p->no_in;
    p->no_in = false;
    while (!is(p, ')') && !p->failed) {
        Node *e;
        if (is(p, TK_ELLIPSIS)) { e = nnew(p, N_SPREAD); next(p); e->a = parse_assign(p); }
        else e = parse_assign(p);
        *tail = e;
        tail = &e->next;
        if (!is(p, ')')) expect(p, ',');
    }
    p->no_in = save;
    expect(p, ')');
    return call;
}

static Node *parse_lhs(Parser *p, bool allow_call) {
    uint32_t x_start = p->tok.start;
    int x_line = p->tok.line;
    bool async_ok = p->tok.t == TK_IDENT && !p->tok.esc && p->tok.v == p->kw[KW_async];
    Node *x = parse_primary(p);
    bool chain = false;
    for (;;) {
        if (p->failed) return x;
        int line = p->tok.line;
        if (is(p, '.')) {
            next(p);
            if (is(p, TK_PRIVATE)) {
                Node *m = nnew(p, N_PRIVMEMBER);
                m->a = x; m->v = p->tok.v; m->line = line;
                next(p);
                x = m;
                continue;
            }
            if (!is(p, TK_IDENT)) { unexpected(p); return x; }
            Node *m = nnew(p, N_MEMBER);
            m->a = x; m->v = p->tok.v; m->line = line;
            next(p);
            x = m;
            continue;
        }
        if (is(p, TK_OPTCHAIN)) {
            if (!allow_call) { unexpected(p); return x; }
            next(p);
            chain = true;
            if (is(p, '(')) {
                Node *c = nnew(p, N_CALL);
                c->a = x; c->line = line; c->flags |= NF_OPTIONAL;
                x = parse_args(p, c);
                continue;
            }
            if (is(p, '[')) {
                next(p);
                Node *m = nnew(p, N_INDEX);
                m->a = x; m->line = line; m->flags |= NF_OPTIONAL;
                bool save = p->no_in;
                p->no_in = false;
                m->b = parse_expr(p);
                p->no_in = save;
                expect(p, ']');
                x = m;
                continue;
            }
            if (is(p, TK_PRIVATE)) {
                Node *m = nnew(p, N_PRIVMEMBER);
                m->a = x; m->v = p->tok.v; m->line = line; m->flags |= NF_OPTIONAL;
                next(p);
                x = m;
                continue;
            }
            if (!is(p, TK_IDENT)) { unexpected(p); return x; }
            Node *m = nnew(p, N_MEMBER);
            m->a = x; m->v = p->tok.v; m->line = line; m->flags |= NF_OPTIONAL;
            next(p);
            x = m;
            continue;
        }
        if (is(p, '[')) {
            next(p);
            Node *m = nnew(p, N_INDEX);
            m->a = x; m->line = line;
            bool save = p->no_in;
            p->no_in = false;
            m->b = parse_expr(p);
            p->no_in = save;
            expect(p, ']');
            x = m;
            continue;
        }
        if (is(p, TK_TEMPLATE)) {
            if (chain) { ERR("Invalid tagged template on optional chain"); return x; }
            x = parse_template(p, x);
            continue;
        }
        if (is(p, '(') && (allow_call || x->k == N_SUPER_CALL)) {
            if (x->k == N_SUPER_CALL) { x = parse_args(p, x); continue; }
            Node *c = nnew(p, N_CALL);
            c->a = x; c->line = line;
            if (x->k == N_IDENT && x->v == p->kw[KW_eval]) c->flags |= NF_DIRECTEVAL;
            bool async_arrow = async_ok && x->k == N_IDENT && x->v == p->kw[KW_async] && line == x_line && !chain;
            x = parse_args(p, c);
            if (async_arrow && is(p, TK_ARROW) && !p->tok.nl) {
                /* async (a, b) => ...: the arguments were the parameters */
                for (Node *a = c->b; a; a = a->next)
                    if (a->k == N_SPREAD) {
                        if (a->next) { ERR("Rest parameter must be last formal parameter"); return x; }
                        a->k = N_REST;
                        a->a = to_pattern(p, a->a, true);
                    }
                return parse_arrow_from(p, c->b, F_ASYNC, x_start, x_line);
            }
            continue;
        }
        break;
    }
    if (chain) {
        Node *o = nnew(p, N_OPTCHAIN);
        o->a = x;
        o->line = x->line;
        return o;
    }
    return x;
}

/* ==== unary / binary ===================================================================================== */
static bool simple_target(Node *e) {
    return e->k == N_IDENT || e->k == N_MEMBER || e->k == N_INDEX || e->k == N_PRIVMEMBER || e->k == N_SUPER_MEMBER;
}

static Node *parse_postfix(Parser *p) {
    Node *x = parse_lhs(p, true);
    if ((is(p, TK_INC) || is(p, TK_DEC)) && !p->tok.nl) {
        if (!simple_target(x)) { ERR("Invalid left-hand side expression in postfix operation"); return x; }
        Node *u = nnew(p, N_UPDATE);
        u->op = (uint16_t)(is(p, TK_INC) ? '+' : '-');
        u->a = x;
        u->line = x->line;
        next(p);
        return u;
    }
    return x;
}

static Node *parse_unary(Parser *p) {
    if (++p->depth > 400) { ERR("Expression nesting too deep"); p->depth--; return nnew(p, N_EMPTY); }
    Node *r;
    int t = p->tok.t;
    int line = p->tok.line;
    if (t == '!' || t == '~' || t == '+' || t == '-') {
        next(p);
        r = nnew(p, N_UNARY);
        r->op = (uint16_t)t;
        r->line = line;
        r->a = parse_unary(p);
    } else if (t == TK_INC || t == TK_DEC) {
        next(p);
        r = nnew(p, N_UPDATE);
        r->op = (uint16_t)(t == TK_INC ? '+' : '-');
        r->flags |= NF_PREFIX;
        r->line = line;
        r->a = parse_unary(p);
        if (!simple_target(r->a)) ERR("Invalid left-hand side expression in prefix operation");
    } else if (is_kw(p, KW_typeof) || is_kw(p, KW_void) || is_kw(p, KW_delete)) {
        int kw = is_kw(p, KW_typeof) ? KW_typeof : is_kw(p, KW_void) ? KW_void : KW_delete;
        next(p);
        r = nnew(p, N_UNARY);
        r->op = (uint16_t)(kw == KW_typeof ? 't' : kw == KW_void ? 'v' : 'd');
        r->line = line;
        r->a = parse_unary(p);
        if (kw == KW_delete && strict(p) && r->a->k == N_IDENT) ERR("Delete of an unqualified identifier in strict mode.");
    } else if (is_kw(p, KW_await) && p->fn && (p->fn->flags & (F_ASYNC | F_MODULE)) && !(p->fn->flags & F_ARROW && !(p->fn->flags & F_ASYNC))) {
        next(p);
        r = nnew(p, N_AWAIT);
        r->line = line;
        r->a = parse_unary(p);
    } else r = parse_postfix(p);
    p->depth--;
    return r;
}

static int binprec(Parser *p, int t) {
    switch (t) {
    case TK_NULLISH: return 1;
    case TK_OR: return 2;
    case TK_AND: return 3;
    case '|': return 4;
    case '^': return 5;
    case '&': return 6;
    case TK_EQ: case TK_NE: case TK_SEQ: case TK_SNE: return 7;
    case '<': case '>': case TK_LE: case TK_GE: return 8;
    case TK_SHL: case TK_SAR: case TK_SHR: return 9;
    case '+': case '-': return 10;
    case '*': case '/': case '%': return 11;
    case TK_POW: return 12;
    case TK_IDENT:
        if (is_kw(p, KW_instanceof)) return 8;
        if (is_kw(p, KW_in) && !p->no_in) return 8;
        return 0;
    default: return 0;
    }
}

static Node *parse_binary(Parser *p, int minprec) {
    Node *x;
    if (is(p, TK_PRIVATE)) x = parse_primary(p);     /* #x in obj */
    else x = parse_unary(p);
    for (;;) {
        int t = p->tok.t;
        int prec = binprec(p, t);
        if (!prec || prec < minprec || p->failed) break;
        int op = t;
        if (t == TK_IDENT) op = is_kw(p, KW_in) ? 'i' : 'I';
        int line = p->tok.line;
        next(p);
        if (op == TK_POW && x->k == N_UNARY && !(x->flags & NF_PAREN)) { ERR("Unary operator used immediately before exponentiation expression"); break; }
        Node *rhs = parse_binary(p, op == TK_POW ? prec : prec + 1);
        Node *b;
        if (x->k == N_PRIVIN && op == 'i') {
            x->a = rhs;
            continue;
        }
        if (op == TK_AND || op == TK_OR || op == TK_NULLISH) { b = nnew(p, N_LOGICAL); }
        else b = nnew(p, N_BINARY);
        b->op = (uint16_t)op;
        b->a = x;
        b->b = rhs;
        b->line = line;
        x = b;
    }
    return x;
}

static Node *parse_cond(Parser *p) {
    Node *x = parse_binary(p, 1);
    if (!is(p, '?')) return x;
    next(p);
    Node *c = nnew(p, N_COND);
    c->a = x;
    c->line = x->line;
    bool save = p->no_in;
    p->no_in = false;
    c->b = parse_assign(p);
    p->no_in = save;
    expect(p, ':');
    c->c = parse_assign(p);
    return c;
}

static int assign_op(int t) {
    switch (t) {
    case '=': return '=';
    case TK_ADD_A: return '+'; case TK_SUB_A: return '-'; case TK_MUL_A: return '*'; case TK_DIV_A: return '/';
    case TK_MOD_A: return '%'; case TK_POW_A: return TK_POW; case TK_SHL_A: return TK_SHL; case TK_SAR_A: return TK_SAR;
    case TK_SHR_A: return TK_SHR; case TK_AND_A: return '&'; case TK_OR_A: return '|'; case TK_XOR_A: return '^';
    case TK_LAND_A: return TK_AND; case TK_LOR_A: return TK_OR; case TK_NULLISH_A: return TK_NULLISH;
    default: return 0;
    }
}

static Node *parse_assign(Parser *p) {
    int line = p->tok.line;
    if (is_kw(p, KW_yield) && p->fn && (p->fn->flags & F_GEN) && !(p->fn->flags & F_ARROW)) {
        next(p);
        Node *y = nnew(p, N_YIELD);
        y->line = line;
        if (is(p, '*')) { next(p); y->flags |= NF_DELEGATE; y->a = parse_assign(p); return y; }
        if (!p->tok.nl && !is(p, ')') && !is(p, ']') && !is(p, '}') && !is(p, ',') && !is(p, ';') && !is(p, ':') &&
            !is(p, TK_EOF) && !(is_kw(p, KW_in)) && !is(p, TK_TEMPLATE))
            y->a = parse_assign(p);
        return y;
    }
    Node *x = parse_cond(p);
    if (x->k == N_FUNC && (x->fn->flags & F_ARROW)) return x;
    int op = assign_op(p->tok.t);
    if (!op) return x;
    if (op == '=' && (x->k == N_ARRAY || x->k == N_OBJECT)) x = to_pattern(p, x, false);
    else if (!simple_target(x)) { ERR("Invalid left-hand side in assignment"); return x; }
    else if (x->k == N_IDENT && strict(p) && (x->v == p->kw[KW_eval] || x->v == p->kw[KW_arguments])) { ERR("Unexpected eval or arguments in strict mode"); return x; }
    next(p);
    Node *a = nnew(p, N_ASSIGN);
    a->op = (uint16_t)op;
    a->a = x;
    a->line = line;
    a->b = parse_assign(p);
    return a;
}

static Node *parse_expr(Parser *p) {
    Node *x = parse_assign(p);
    if (!is(p, ',')) return x;
    Node *s = nnew(p, N_SEQ);
    s->line = x->line;
    s->a = x;
    Node **tail = &x->next;
    while (eat(p, ',') && !p->failed) {
        Node *e = parse_assign(p);
        *tail = e;
        tail = &e->next;
    }
    return s;
}

/* ==== functions ============================================================================================ */
static Node *parse_binding_target(Parser *p) {
    if (is(p, '[')) {
        Node *x = parse_array_lit(p);
        return to_pattern(p, x, true);
    }
    if (is(p, '{')) {
        Node *x = parse_object_lit(p);
        return to_pattern(p, x, true);
    }
    Node *id = nnew(p, N_IDENT);
    id->v = ident_name(p);
    if (strict(p) && (id->v == p->kw[KW_eval] || id->v == p->kw[KW_arguments])) ERR("Unexpected eval or arguments in strict mode");
    return id;
}

static Node *parse_binding_elem(Parser *p) {
    Node *t = parse_binding_target(p);
    if (is(p, '=')) {
        next(p);
        Node *a = nnew(p, N_ASSIGN_PAT);
        a->a = t;
        a->b = parse_assign(p);
        return a;
    }
    return t;
}

static void count_length(Func *f) {
    int n = 0;
    for (Node *x = f->params; x; x = x->next) {
        if (x->k == N_ASSIGN_PAT || x->k == N_REST) break;
        n++;
    }
    f->length = n;
    for (Node *x = f->params; x; x = x->next) if (x->k != N_IDENT) f->simple_params = false;
}

static bool directive_strict(Parser *p, Node *st) {
    if (st->k != N_EXPR || st->a->k != N_STR || (st->a->flags & NF_PAREN)) return false;
    uint32_t s = st->a->pos, l = 12;
    if (s + l > p->n) return false;
    static const char us[] = "use strict";
    for (int i = 0; i < 10; i++) if (p->s[s + 1 + (uint32_t)i] != (uint8_t)us[i]) return false;
    return (p->s[s] == '"' || p->s[s] == '\'') && p->s[s + 11] == p->s[s];
}

/* statements of a function body up to the closing brace (not consumed) */
static Node *parse_function_body_list(Parser *p, Func *f) {
    Node *list = NULL, **tail = &list;
    bool prologue = true;
    while (!is(p, '}') && !is(p, TK_EOF) && !p->failed) {
        bool str_first = prologue && is(p, TK_STR);
        Node *st = parse_stmt(p);
        if (prologue) {
            if (str_first && st->k == N_EXPR && st->a->k == N_STR) {
                if (directive_strict(p, st)) {
                    f->flags |= F_STRICT;
                    f->use_strict = true;
                    if (!f->simple_params) { error_at(p, st->line, "Illegal 'use strict' directive in function with non-simple parameter list"); }
                }
            } else prologue = false;
        }
        *tail = st;
        tail = &st->next;
    }
    return list;
}

static Func *parse_function_rest(Parser *p, int kind, uint32_t fflags, JSValue name, uint32_t start, int line) {
    Func *f = new_func(p, kind, fflags);
    f->name = name;
    f->start = start;
    f->line = line;
    Func *save = p->fn;
    bool save_in = p->no_in, save_cls = p->in_class_body;
    p->fn = f;
    p->no_in = false;
    p->in_class_body = false;
    expect(p, '(');
    Node **tail = &f->params;
    while (!is(p, ')') && !p->failed) {
        Node *e;
        if (is(p, TK_ELLIPSIS)) {
            next(p);
            e = nnew(p, N_REST);
            e->a = parse_binding_target(p);
            *tail = e;
            if (!is(p, ')')) { ERR("Rest parameter must be last formal parameter"); break; }
            break;
        }
        e = parse_binding_elem(p);
        *tail = e;
        tail = &e->next;
        if (!is(p, ')')) expect(p, ',');
    }
    expect(p, ')');
    count_length(f);
    if (kind == FK_GETTER && f->params) ERR("Getter must not have any formal parameters.");
    if (kind == FK_SETTER && (!f->params || f->params->next || f->params->k == N_REST)) ERR("Setter must have exactly one formal parameter.");
    expect(p, '{');
    f->body = parse_function_body_list(p, f);
    f->end = p->tok.end;
    expect(p, '}');
    p->fn = save;
    p->no_in = save_in;
    p->in_class_body = save_cls;
    return f;
}

static Node *parse_arrow_from(Parser *p, Node *params, uint32_t fflags, uint32_t start, int line) {
    /* the => */
    next(p);
    Func *f = new_func(p, FK_ARROW, fflags | F_ARROW);
    f->start = start;
    f->line = line;
    Node **tail = &f->params;
    for (Node *x = params, *nx; x; x = nx) {
        nx = x->next;
        Node *pe;
        if (x->k == N_REST) { pe = x; if (nx) { ERR("Rest parameter must be last formal parameter"); break; } }
        else pe = to_pattern_elem(p, x, true);
        pe->next = NULL;
        *tail = pe;
        tail = &pe->next;
    }
    count_length(f);
    Func *save = p->fn;
    bool save_cls = p->in_class_body;
    p->fn = f;
    p->in_class_body = false;
    if (is(p, '{')) {
        bool save_in = p->no_in;
        p->no_in = false;
        next(p);
        f->body = parse_function_body_list(p, f);
        f->end = p->tok.end;
        expect(p, '}');
        p->no_in = save_in;
    } else {
        Node *e = parse_assign(p);
        Node *r = nnew(p, N_RETURN);
        r->a = e;
        r->line = e->line;
        f->body = r;
        f->expr_body = true;
        f->end = p->prev_end;
    }
    p->fn = save;
    p->in_class_body = save_cls;
    Node *n = nnew(p, N_FUNC);
    n->fn = f;
    n->line = line;
    return n;
}

/* ==== classes ============================================================================================== */
static Func *class_init_func(Parser *p, Node *cls, bool is_static) {
    Func *f = new_func(p, is_static ? FK_STATIC_INIT : FK_FIELDS, F_METHOD | F_STRICT | F_FIELDINIT);
    f->cls = cls;
    f->start = f->end = cls->pos;
    f->line = cls->line;
    return f;
}

static Node *parse_class(Parser *p, bool decl) {
    uint32_t start = p->tok.start;
    int line = p->tok.line;
    next(p);   /* class */
    Node *c = nnew(p, N_CLASS);
    c->line = line;
    c->pos = start;
    Func *save = p->fn;
    /* class code is strict */
    Func strict_tmp;
    memset(&strict_tmp, 0, sizeof strict_tmp);
    if (p->fn) { strict_tmp = *p->fn; }
    strict_tmp.flags |= F_STRICT;
    bool was_strict = p->fn && (p->fn->flags & F_STRICT);
    if (p->fn) p->fn->flags |= F_STRICT;
    if (is(p, TK_IDENT) && !is_kw(p, KW_extends)) {
        Node *id = nnew(p, N_IDENT);
        id->v = ident_name(p);
        c->a = id;
    } else if (decl) { ERR("A class declaration needs a name"); }
    if (eat_kw(p, KW_extends)) c->b = parse_lhs(p, true);
    expect(p, '{');
    Func *inst = NULL, *stat = NULL;
    Node **etail = &c->c;
    Node **itail = NULL, **stail = NULL;
    bool save_cls = p->in_class_body;
    p->in_class_body = true;
    while (!is(p, '}') && !p->failed) {
        if (eat(p, ';')) continue;
        uint32_t estart = p->tok.start;
        int eline = p->tok.line;
        Node *el = nnew(p, N_CLASS_ELEM);
        el->line = eline;
        uint32_t fflags = 0;
        int kind = PK_METHOD;
        bool is_static = false;
        Tok *pk;
        if (is_kw(p, KW_static) && (pk = peek(p), pk->t != '(' && pk->t != '=' && pk->t != ';' && pk->t != '}' && !(pk->nl && pk->t != '{' && pk->t != '['  && pk->t != TK_IDENT && pk->t != TK_PRIVATE && pk->t != '*'))) {
            next(p);
            is_static = true;
            if (is(p, '{')) {
                /* static block */
                if (!stat) { stat = class_init_func(p, c, true); }
                p->fn = stat;
                next(p);
                Node *blk = nnew(p, N_BLOCK);
                blk->line = eline;
                Node **bt = &blk->a;
                while (!is(p, '}') && !p->failed) { Node *st = parse_stmt(p); *bt = st; bt = &st->next; }
                expect(p, '}');
                p->fn = save;
                if (!stail) stail = &stat->body;
                *stail = blk;
                stail = &blk->next;
                continue;
            }
        }
        if (is_kw(p, KW_async) && (pk = peek(p), pk->t != '(' && pk->t != '=' && pk->t != ';' && pk->t != '}' && !pk->nl)) { next(p); fflags |= F_ASYNC; }
        if (is(p, '*')) { next(p); fflags |= F_GEN; }
        if ((is_kw(p, KW_get) || is_kw(p, KW_set)) && !(fflags & (F_ASYNC | F_GEN)) &&
            (pk = peek(p), pk->t != '(' && pk->t != '=' && pk->t != ';' && pk->t != '}' && !pk->nl)) {
            kind = is_kw(p, KW_get) ? PK_GET : PK_SET;
            next(p);
        }
        uint32_t kf = 0;
        Node *key = parse_prop_key(p, &kf, true);
        el->a = key;
        el->flags = kf | (is_static ? NF_STATIC : 0);
        bool is_ctor = !is_static && !(kf & (NF_COMPUTED | NF_PRIVATE)) && key->v == p->kw[KW_constructor];
        if (is(p, '(')) {
            el->op = (uint16_t)kind;
            if (is_ctor) {
                if (kind != PK_METHOD || fflags) { ERR("Class constructor may not be an accessor, a generator or async"); break; }
                Func *f = parse_function_rest(p, c->b ? FK_DERIVED : FK_CTOR, F_STRICT | F_CLASSCTOR | (c->b ? F_DERIVED : F_CTOR), JS_UNDEFINED, start, line);
                f->cls = c;
                if (c->fn) { ERR("A class may only have one constructor"); break; }
                c->fn = f;
                continue;
            }
            el->b = method_value(p, kind == PK_GET ? FK_GETTER : kind == PK_SET ? FK_SETTER : FK_METHOD, fflags | F_STRICT, key, estart, eline);
        } else {
            /* a field */
            if (fflags || kind != PK_METHOD || is_ctor) { unexpected(p); break; }
            el->op = PK_FIELD;
            Func **pf = is_static ? &stat : &inst;
            if (!*pf) *pf = class_init_func(p, c, is_static);
            Node *fd = nnew(p, N_FIELD_DEF);
            fd->line = eline;
            fd->flags = kf;
            fd->a = key;
            if (kf & NF_COMPUTED) {
                char nm[32];
                snprintf(nm, sizeof nm, "<field %d>", p->field_keys++);
                el->v = js_atomc(p->ctx, nm);
                Node *ref = nnew(p, N_IDENT);
                ref->v = el->v;
                fd->a = ref;
            }
            if (eat(p, '=')) {
                p->fn = *pf;
                bool save_in = p->no_in;
                p->no_in = false;
                fd->b = parse_assign(p);
                p->no_in = save_in;
                p->fn = save;
            }
            semicolon(p);
            Node ***tl = is_static ? &stail : &itail;
            if (!*tl) *tl = &(*pf)->body;
            **tl = fd;
            *tl = &fd->next;
            el->b = fd;
        }
        *etail = el;
        etail = &el->next;
    }
    p->in_class_body = save_cls;
    c->v2 = js_mknum((double)(p->tok.end));
    uint32_t end = p->tok.end;
    expect(p, '}');
    if (p->fn && !was_strict) p->fn->flags &= ~(uint32_t)F_STRICT;
    p->fn = save;
    (void)strict_tmp;
    /* the class node keeps its initialisers: inst in ival slot via d, stat via res? use dedicated nodes */
    Node *holder = nnew(p, N_EMPTY);
    holder->fn = inst;
    Node *holder2 = nnew(p, N_EMPTY);
    holder2->fn = stat;
    c->d = holder;
    holder->next = holder2;
    if (c->fn) { c->fn->fields = inst; c->fn->end = end; c->fn->start = start; }
    c->num = (double)end;
    return c;
}

/* ==== ES modules ============================================================================================= */
/* A module compiles to async function (imports, exports, meta): its prologue binds each
   imported module's namespace (const *ns<i>* = imports(i)), registers its exports
   (exports(name, () => local) - live bindings; exports('*', ns) for export *) and then
   waits for meta.go, so a whole module graph is linked before any module runs. Imported
   names become BK_IMPORT bindings (reads of *ns<i>*.name). */
static bool module_top(Parser *p) { return p->fn && p->fn->kind == FK_MODULE && !JS_IsUndefined(p->a_exp) && p->depth == 1; }

static Node *mk_ident(Parser *p, JSValue name) { Node *x = nnew(p, N_IDENT); x->v = name; return x; }
static Node *mk_str(Parser *p, JSValue str) { Node *x = nnew(p, N_STR); x->v = str; return x; }
static Node *mk_stmt(Parser *p, Node *e) { Node *x = nnew(p, N_EXPR); x->a = e; x->line = e->line; return x; }
static Node *mk_call2(Parser *p, JSValue fn, Node *a, Node *b) {
    Node *c = nnew(p, N_CALL);
    c->a = mk_ident(p, fn);
    a->next = b;
    c->b = a;
    return c;
}

static int mod_request(Parser *p, JSValue spec) {
    for (int i = 0; i < p->nreq; i++) if (js_strict_equals(p->ctx, p->req[i], spec)) return i;
    if (p->nreq >= p->reqcap) {
        int nc = p->reqcap ? p->reqcap * 2 : 8;
        JSValue *n = js_parser_alloc(p, sizeof(JSValue) * (size_t)nc);
        if (!n) return 0;
        if (p->nreq) memcpy(n, p->req, sizeof(JSValue) * (size_t)p->nreq);
        p->req = n;
        p->reqcap = nc;
    }
    p->req[p->nreq] = spec;
    /* const *ns<i>* = imports(i); */
    char nm[24];
    snprintf(nm, sizeof nm, "*ns%d*", p->nreq);
    Node *v = nnew(p, N_VAR);
    v->op = VK_CONST;
    Node *d = nnew(p, N_DECL);
    d->a = mk_ident(p, js_atomc(p->ctx, nm));
    Node *num = nnew(p, N_NUM);
    num->num = p->nreq;
    Node *c = nnew(p, N_CALL);
    c->a = mk_ident(p, p->a_imp);
    c->b = num;
    d->b = c;
    v->a = d;
    *p->pro_ns_tail = v;
    p->pro_ns_tail = &v->next;
    return p->nreq++;
}

static JSValue ns_name(Parser *p, int i) {
    char nm[24];
    snprintf(nm, sizeof nm, "*ns%d*", i);
    return js_atomc(p->ctx, nm);
}

/* exports(name, () => local) */
static void export_reg(Parser *p, JSValue exported, JSValue local) {
    Func *f = new_func(p, FK_ARROW, F_ARROW | F_STRICT);
    f->start = f->end = p->tok.start;
    f->line = p->tok.line;
    Node *r = nnew(p, N_RETURN);
    r->a = mk_ident(p, local);
    f->body = r;
    f->expr_body = true;
    Node *fn = nnew(p, N_FUNC);
    fn->fn = f;
    Node *st = mk_stmt(p, mk_call2(p, p->a_exp, mk_str(p, exported), fn));
    *p->pro_exp_tail = st;
    p->pro_exp_tail = &st->next;
}

static void export_pattern(Parser *p, Node *n) {
    if (!n) return;
    switch (n->k) {
    case N_IDENT: export_reg(p, n->v, n->v); break;
    case N_ASSIGN_PAT: case N_REST: export_pattern(p, n->a); break;
    case N_ARRAY_PAT: for (Node *e = n->a; e; e = e->next) export_pattern(p, e); break;
    case N_OBJECT_PAT: for (Node *pr = n->a; pr; pr = pr->next) export_pattern(p, pr->b); break;
    default: break;
    }
}

/* a module name or export name: an identifier (keywords too) or a string */
static JSValue module_name(Parser *p) {
    if (is(p, TK_IDENT) || is(p, TK_STR)) { JSValue v = p->tok.v; next(p); return v; }
    unexpected(p);
    return JS_UNDEFINED;
}

static JSValue from_clause(Parser *p) {
    if (!is_kw(p, KW_from)) { unexpected(p); return JS_UNDEFINED; }
    next(p);
    if (!is(p, TK_STR)) { unexpected(p); return JS_UNDEFINED; }
    JSValue spec = p->tok.v;
    next(p);
    /* import attributes: with { type: "json" } / assert {...} (accepted, not used) */
    if ((is_kw(p, KW_with) || (is(p, TK_IDENT) && !p->tok.nl && p->tok.v == js_atomc(p->ctx, "assert"))) && peek(p)->t == '{') {
        next(p);
        int depth = 0;
        do { if (is(p, '{')) depth++; else if (is(p, '}')) depth--; next(p); } while (depth > 0 && !is(p, TK_EOF));
    }
    return spec;
}

/* N_IMPORT: ival = request, a = list of N_PROP (v local, v2 imported name; UNDEFINED = the namespace) */
static void import_spec(Parser *p, Node ***tail, JSValue local, JSValue imported) {
    Node *s = nnew(p, N_PROP);
    s->v = local;
    s->v2 = imported;
    **tail = s;
    *tail = &s->next;
}

static Node *parse_import(Parser *p) {
    int line = p->tok.line;
    next(p);   /* import */
    Node *x = nnew(p, N_IMPORT);
    x->line = line;
    Node **tail = &x->a;
    if (is(p, TK_STR)) {
        JSValue spec = p->tok.v;
        next(p);
        x->ival = mod_request(p, spec);
        semicolon(p);
        return x;
    }
    if (is_ident_tok(p)) {
        JSValue local = ident_name(p);
        import_spec(p, &tail, local, p->kw[KW_default]);
        if (!eat(p, ',')) goto from;
    }
    if (eat(p, '*')) {
        if (!eat_kw(p, KW_as)) { unexpected(p); return x; }
        import_spec(p, &tail, ident_name(p), JS_UNDEFINED);
    } else if (eat(p, '{')) {
        while (!is(p, '}') && !p->failed) {
            JSValue imported = module_name(p), local = imported;
            if (eat_kw(p, KW_as)) local = ident_name(p);
            else if (reserved(p, &(Tok){ .t = TK_IDENT, .v = imported })) { ERR("Unexpected reserved word"); }
            import_spec(p, &tail, local, imported);
            if (!eat(p, ',')) break;
        }
        expect(p, '}');
    } else if (!x->a) { unexpected(p); return x; }
from:;
    JSValue spec = from_clause(p);
    if (JS_IsUndefined(spec)) return x;
    x->ival = mod_request(p, spec);
    semicolon(p);
    return x;
}

static Node *parse_export(Parser *p) {
    int line = p->tok.line;
    uint32_t start = p->tok.start;
    next(p);   /* export */
    if (eat(p, '*')) {
        JSValue as = JS_UNDEFINED;
        if (eat_kw(p, KW_as)) as = module_name(p);
        JSValue spec = from_clause(p);
        if (JS_IsUndefined(spec)) return nnew(p, N_EMPTY);
        int i = mod_request(p, spec);
        if (!JS_IsUndefined(as)) export_reg(p, as, ns_name(p, i));
        else {
            /* exports('*', *ns<i>*) */
            Node *st = mk_stmt(p, mk_call2(p, p->a_exp, mk_str(p, js_atomc(p->ctx, "*")), mk_ident(p, ns_name(p, i))));
            *p->pro_exp_tail = st;
            p->pro_exp_tail = &st->next;
        }
        semicolon(p);
        return nnew(p, N_EMPTY);
    }
    if (eat(p, '{')) {
        JSValue loc[256], exp[256];
        int n = 0;
        while (!is(p, '}') && !p->failed) {
            JSValue l = module_name(p), e = l;
            if (eat_kw(p, KW_as)) e = module_name(p);
            if (n < 256) { loc[n] = l; exp[n] = e; n++; }
            if (!eat(p, ',')) break;
        }
        expect(p, '}');
        if (is_kw(p, KW_from)) {
            /* export { a as b } from "m": hidden import bindings, exported */
            JSValue spec = from_clause(p);
            if (JS_IsUndefined(spec)) return nnew(p, N_EMPTY);
            Node *x = nnew(p, N_IMPORT);
            x->line = line;
            x->ival = mod_request(p, spec);
            Node **tail = &x->a;
            for (int i = 0; i < n; i++) {
                char nm[32];
                snprintf(nm, sizeof nm, "*re%d*", p->nre++);
                JSValue hid = js_atomc(p->ctx, nm);
                import_spec(p, &tail, hid, loc[i]);
                export_reg(p, exp[i], hid);
            }
            semicolon(p);
            return x;
        }
        for (int i = 0; i < n; i++) export_reg(p, exp[i], loc[i]);
        semicolon(p);
        return nnew(p, N_EMPTY);
    }
    if (is_kw(p, KW_default)) {
        next(p);
        JSValue dflt = p->kw[KW_default];
        bool fn = is_kw(p, KW_function), afn = false;
        if (is_kw(p, KW_async)) { Tok *pk = peek(p); afn = tok_kw(p, pk, KW_function) && !pk->nl; }
        if (fn || afn) {
            /* export default function [name] (...) {...}: a hoisted declaration */
            if (afn) next(p);
            next(p);
            uint32_t ff = afn ? F_ASYNC : 0;
            if (eat(p, '*')) ff |= F_GEN;
            JSValue name = is_ident_tok(p) ? ident_name(p) : dflt;
            Func *f = parse_function_rest(p, FK_NORMAL, ff, name, start, line);
            f->is_decl = true;
            Node *x = nnew(p, N_FUNC_DECL);
            x->fn = f;
            x->line = line;
            export_reg(p, dflt, name);
            return x;
        }
        if (is_kw(p, KW_class)) {
            Node *x = nnew(p, N_CLASS_DECL);
            Tok *pk = peek(p);
            bool named = pk->t == TK_IDENT && !tok_kw(p, pk, KW_extends);
            x->a = parse_class(p, false);
            x->line = line;
            if (!named) { x->a->a = mk_ident(p, dflt); }
            export_reg(p, dflt, x->a->a->v);
            return x;
        }
        /* export default <expression>: const default = <expression> */
        Node *v = nnew(p, N_VAR);
        v->op = VK_CONST;
        v->line = line;
        Node *d = nnew(p, N_DECL);
        d->a = mk_ident(p, dflt);
        d->b = parse_assign(p);
        v->a = d;
        semicolon(p);
        export_reg(p, dflt, dflt);
        return v;
    }
    /* export var / let / const / function / class */
    Node *st = parse_stmt_inner(p);
    switch (st->k) {
    case N_VAR: for (Node *d = st->a; d; d = d->next) export_pattern(p, d->a); break;
    case N_FUNC_DECL: export_reg(p, st->fn->name, st->fn->name); break;
    case N_CLASS_DECL: if (st->a && st->a->a) export_reg(p, st->a->a->v, st->a->a->v); break;
    default: ERR("Unexpected token 'export'"); break;
    }
    return st;
}

/* ==== statements ============================================================================================= */
static Node *parse_var(Parser *p, int kind, bool for_init) {
    Node *v = nnew(p, N_VAR);
    v->op = (uint16_t)kind;
    Node **tail = &v->a;
    do {
        Node *d = nnew(p, N_DECL);
        d->a = parse_binding_target(p);
        if (kind != VK_VAR && d->a->k == N_IDENT && d->a->v == p->kw[KW_let]) ERR("let is disallowed as a lexically bound name");
        if (eat(p, '=')) d->b = parse_assign(p);
        else if (!for_init || (!is_kw(p, KW_in) && !is_kw(p, KW_of))) {
            if (kind == VK_CONST) ERR("Missing initializer in const declaration");
            else if (d->a->k != N_IDENT) ERR("Missing initializer in destructuring declaration");
        }
        *tail = d;
        tail = &d->next;
    } while (eat(p, ',') && !p->failed);
    return v;
}

static bool let_decl_start(Parser *p) {
    if (!is_kw(p, KW_let)) return false;
    Tok *pk = peek(p);
    if (pk->t == '[' || pk->t == '{') return true;
    if (pk->t == TK_IDENT && !(pk->nl && (tok_kw(p, pk, KW_in) || tok_kw(p, pk, KW_instanceof)))) {
        if (tok_kw(p, pk, KW_in) || tok_kw(p, pk, KW_instanceof) || tok_kw(p, pk, KW_of)) return false;
        return true;
    }
    return false;
}

static Node *parse_block(Parser *p) {
    Node *b = nnew(p, N_BLOCK);
    expect(p, '{');
    Node **tail = &b->a;
    while (!is(p, '}') && !is(p, TK_EOF) && !p->failed) {
        Node *s = parse_stmt(p);
        *tail = s;
        tail = &s->next;
    }
    expect(p, '}');
    return b;
}

static Node *parse_for(Parser *p) {
    int line = p->tok.line;
    next(p);   /* for */
    bool await_ = false;
    if (is_kw(p, KW_await)) {
        if (!(p->fn && (p->fn->flags & (F_ASYNC | F_MODULE)))) { unexpected(p); return nnew(p, N_EMPTY); }
        next(p);
        await_ = true;
    }
    expect(p, '(');
    Node *init = NULL;
    bool save = p->no_in;
    p->no_in = true;
    if (is(p, ';')) { }
    else if (is_kw(p, KW_var)) { next(p); init = parse_var(p, VK_VAR, true); }
    else if (is_kw(p, KW_const)) { next(p); init = parse_var(p, VK_CONST, true); }
    else if (let_decl_start(p)) { next(p); init = parse_var(p, VK_LET, true); }
    else init = parse_expr(p);
    p->no_in = save;
    if (is_kw(p, KW_in) || is_kw(p, KW_of)) {
        bool of = is_kw(p, KW_of);
        if (await_ && !of) { unexpected(p); return nnew(p, N_EMPTY); }
        if (init->k == N_VAR) {
            if (init->a->next) { ERR("Invalid left-hand side in for-%s loop: Must have a single binding.", of ? "of" : "in"); return init; }
            if (init->a->b && (of || strict(p) || init->op != VK_VAR || init->a->a->k != N_IDENT)) { ERR("for-%s loop variable declaration may not have an initializer.", of ? "of" : "in"); return init; }
        } else if (init->k == N_ARRAY || init->k == N_OBJECT) init = to_pattern(p, init, false);
        else if (!simple_target(init)) { ERR("Invalid left-hand side in for-%s loop", of ? "of" : "in"); return init; }
        next(p);
        Node *f = nnew(p, of ? N_FOROF : N_FORIN);
        f->line = line;
        f->a = init;
        f->b = of ? parse_assign(p) : parse_expr(p);
        if (await_) f->flags |= NF_AWAIT;
        expect(p, ')');
        f->d = parse_stmt(p);
        return f;
    }
    if (await_) { unexpected(p); return nnew(p, N_EMPTY); }
    Node *f = nnew(p, N_FOR);
    f->line = line;
    f->a = init;
    expect(p, ';');
    if (!is(p, ';')) f->b = parse_expr(p);
    expect(p, ';');
    if (!is(p, ')')) f->c = parse_expr(p);
    expect(p, ')');
    f->d = parse_stmt(p);
    return f;
}

static Node *parse_stmt_inner(Parser *p) {
    int line = p->tok.line;
    uint32_t start = p->tok.start;
    Node *x;
    switch (p->tok.t) {
    case '{': return parse_block(p);
    case ';': next(p); return nnew(p, N_EMPTY);
    case TK_IDENT:
        if (p->tok.esc) break;
        if (is_kw(p, KW_var)) { next(p); x = parse_var(p, VK_VAR, false); semicolon(p); return x; }
        if (is_kw(p, KW_const)) { next(p); x = parse_var(p, VK_CONST, false); semicolon(p); return x; }
        if (let_decl_start(p)) { next(p); x = parse_var(p, VK_LET, false); semicolon(p); return x; }
        if (is_kw(p, KW_function)) {
            next(p);
            uint32_t ff = 0;
            if (eat(p, '*')) ff |= F_GEN;
            Func tmp;
            memset(&tmp, 0, sizeof tmp);
            JSValue name = ident_name(p);
            (void)tmp;
            Func *f = parse_function_rest(p, FK_NORMAL, ff, name, start, line);
            f->is_decl = true;
            x = nnew(p, N_FUNC_DECL);
            x->fn = f;
            x->line = line;
            return x;
        }
        if (is_kw(p, KW_async)) {
            Tok *pk = peek(p);
            if (tok_kw(p, pk, KW_function) && !pk->nl) {
                next(p); next(p);
                uint32_t ff = F_ASYNC;
                if (eat(p, '*')) ff |= F_GEN;
                JSValue name = ident_name(p);
                Func *f = parse_function_rest(p, FK_NORMAL, ff, name, start, line);
                f->is_decl = true;
                x = nnew(p, N_FUNC_DECL);
                x->fn = f;
                x->line = line;
                return x;
            }
            break;
        }
        if (is_kw(p, KW_class)) {
            x = nnew(p, N_CLASS_DECL);
            x->a = parse_class(p, true);
            x->line = line;
            return x;
        }
        if (is_kw(p, KW_if)) {
            next(p);
            x = nnew(p, N_IF);
            expect(p, '(');
            x->a = parse_expr(p);
            expect(p, ')');
            x->b = parse_stmt(p);
            if (eat_kw(p, KW_else)) x->c = parse_stmt(p);
            return x;
        }
        if (is_kw(p, KW_for)) return parse_for(p);
        if (is_kw(p, KW_while)) {
            next(p);
            x = nnew(p, N_WHILE);
            expect(p, '(');
            x->a = parse_expr(p);
            expect(p, ')');
            x->d = parse_stmt(p);
            return x;
        }
        if (is_kw(p, KW_do)) {
            next(p);
            x = nnew(p, N_DOWHILE);
            x->d = parse_stmt(p);
            if (!eat_kw(p, KW_while)) { unexpected(p); return x; }
            expect(p, '(');
            x->a = parse_expr(p);
            expect(p, ')');
            eat(p, ';');
            return x;
        }
        if (is_kw(p, KW_continue) || is_kw(p, KW_break)) {
            bool brk = is_kw(p, KW_break);
            next(p);
            x = nnew(p, brk ? N_BREAK : N_CONTINUE);
            x->line = line;
            if (is(p, TK_IDENT) && !p->tok.nl && !reserved(p, &p->tok)) { x->v = p->tok.v; next(p); }
            semicolon(p);
            return x;
        }
        if (is_kw(p, KW_return)) {
            if (!p->fn || p->fn->kind == FK_SCRIPT || p->fn->kind == FK_FIELDS || p->fn->kind == FK_STATIC_INIT || p->fn->kind == FK_MODULE) { ERR("Illegal return statement"); return nnew(p, N_EMPTY); }
            next(p);
            x = nnew(p, N_RETURN);
            x->line = line;
            if (!is(p, ';') && !is(p, '}') && !is(p, TK_EOF) && !p->tok.nl) x->a = parse_expr(p);
            semicolon(p);
            return x;
        }
        if (is_kw(p, KW_throw)) {
            next(p);
            if (p->tok.nl) { ERR("Illegal newline after throw"); return nnew(p, N_EMPTY); }
            x = nnew(p, N_THROW);
            x->line = line;
            x->a = parse_expr(p);
            semicolon(p);
            return x;
        }
        if (is_kw(p, KW_try)) {
            next(p);
            x = nnew(p, N_TRY);
            x->line = line;
            x->a = parse_block(p);
            if (eat_kw(p, KW_catch)) {
                if (eat(p, '(')) {
                    x->b = parse_binding_target(p);
                    expect(p, ')');
                }
                x->c = parse_block(p);
            }
            if (eat_kw(p, KW_finally)) x->d = parse_block(p);
            if (!x->c && !x->d) ERR("Missing catch or finally after try");
            return x;
        }
        if (is_kw(p, KW_switch)) {
            next(p);
            x = nnew(p, N_SWITCH);
            x->line = line;
            expect(p, '(');
            x->a = parse_expr(p);
            expect(p, ')');
            expect(p, '{');
            Node **tail = &x->b;
            bool has_default = false;
            while (!is(p, '}') && !p->failed) {
                Node *c = nnew(p, N_CASE);
                if (eat_kw(p, KW_case)) c->a = parse_expr(p);
                else if (eat_kw(p, KW_default)) {
                    if (has_default) { ERR("More than one default clause in switch statement"); break; }
                    has_default = true;
                } else { unexpected(p); break; }
                expect(p, ':');
                Node **st = &c->b;
                while (!is(p, '}') && !is_kw(p, KW_case) && !is_kw(p, KW_default) && !p->failed) {
                    Node *s = parse_stmt(p);
                    *st = s;
                    st = &s->next;
                }
                *tail = c;
                tail = &c->next;
            }
            expect(p, '}');
            return x;
        }
        if (is_kw(p, KW_with)) {
            if (strict(p)) { ERR("Strict mode code may not include a with statement"); return nnew(p, N_EMPTY); }
            next(p);
            x = nnew(p, N_WITH);
            x->line = line;
            expect(p, '(');
            x->a = parse_expr(p);
            expect(p, ')');
            x->d = parse_stmt(p);
            return x;
        }
        if (is_kw(p, KW_debugger)) { next(p); semicolon(p); return nnew(p, N_DEBUGGER); }
        if (is_kw(p, KW_import)) {
            Tok *pk = peek(p);
            if (pk->t != '(' && pk->t != '.') {
                if (module_top(p)) return parse_import(p);
                ERR(p->fn && p->fn->kind == FK_MODULE ? "An import declaration can only be used at the top level of a module"
                                                      : "Cannot use import statement outside a module");
                return nnew(p, N_EMPTY);
            }
            break;
        }
        if (is_kw(p, KW_export)) {
            if (module_top(p)) return parse_export(p);
            ERR("Unexpected token 'export'");
            return nnew(p, N_EMPTY);
        }
        /* label */
        if (!reserved(p, &p->tok)) {
            Tok *pk = peek(p);
            if (pk->t == ':') {
                JSValue lab = p->tok.v;
                next(p); next(p);
                x = nnew(p, N_LABELED);
                x->v = lab;
                x->line = line;
                if (is_kw(p, KW_function)) { ERR("In strict mode code, functions can only be declared at top level or inside a block."); return x; }
                x->a = parse_stmt(p);
                return x;
            }
        }
        break;
    default: break;
    }
    x = nnew(p, N_EXPR);
    x->line = line;
    x->a = parse_expr(p);
    semicolon(p);
    return x;
}

static Node *parse_stmt(Parser *p) {
    if (++p->depth > 300) { ERR("Statements nested too deeply"); p->depth--; return nnew(p, N_EMPTY); }
    Node *s = parse_stmt_inner(p);
    p->depth--;
    return s;
}

/* ==== entry ==================================================================================================== */
Func *js_parse_program(JSContext *ctx, Parser **pp, const uint16_t *src, uint32_t len, int line, uint32_t fflags) {
    Parser *p = calloc(1, sizeof *p);
    *pp = p;
    if (!p) { js_throw_oom(ctx); return NULL; }
    p->ctx = ctx;
    p->s = src;
    p->n = len;
    p->line = line > 0 ? line : 1;
    sb_init(ctx, &p->sb);
    for (int i = 0; i < KW_COUNT; i++) p->kw[i] = js_atomc(ctx, kw_names[i]);
    p->a_imp = p->a_exp = p->a_meta = JS_UNDEFINED;
    p->pro_ns_tail = &p->pro_ns;
    p->pro_exp_tail = &p->pro_exp;
    Func *f = new_func(p, (fflags & F_MODULE) ? FK_MODULE : FK_SCRIPT, F_SCRIPT | (fflags & (F_STRICT | F_EVAL | F_MODULE)));
    if (fflags & F_MODULE) {
        /* async function (*imp*, *exp*, *meta*) */
        f->flags |= F_STRICT | F_ASYNC;
        p->a_imp = js_atomc(ctx, "*imp*");
        p->a_exp = js_atomc(ctx, "*exp*");
        p->a_meta = js_atomc(ctx, "*meta*");
        f->params = mk_ident(p, p->a_imp);
        f->params->next = mk_ident(p, p->a_exp);
        f->params->next->next = mk_ident(p, p->a_meta);
        f->length = 3;
    }
    f->start = 0;
    f->end = len;
    f->line = p->line;
    p->fn = f;
    next(p);
    Node *list = NULL, **tail = &list;
    bool prologue = true;
    while (!is(p, TK_EOF) && !p->failed) {
        bool str_first = prologue && is(p, TK_STR);
        Node *st = parse_stmt(p);
        if (prologue) {
            if (str_first && st->k == N_EXPR && st->a->k == N_STR) { if (directive_strict(p, st)) { f->flags |= F_STRICT; f->use_strict = true; } }
            else prologue = false;
        }
        *tail = st;
        tail = &st->next;
    }
    if (f->kind == FK_MODULE && !p->failed) {
        /* prologue: namespaces, exports, then wait until the graph is linked (await meta.go) */
        Node *aw = nnew(p, N_AWAIT);
        Node *m = nnew(p, N_MEMBER);
        m->a = mk_ident(p, p->a_meta);
        m->v = js_atomc(ctx, "go");
        aw->a = m;
        Node *go = mk_stmt(p, aw);
        go->next = list;
        *p->pro_exp_tail = go;
        *p->pro_ns_tail = p->pro_exp;
        list = p->pro_ns;
        f->mreq = p->req;
        f->nmreq = p->nreq;
    }
    f->body = list;
    if (p->failed) {
        js_throw_syntax(ctx, "%s (line %d)", p->err, p->err_line ? p->err_line : p->tok.line);
        return NULL;
    }
    return f;
}
