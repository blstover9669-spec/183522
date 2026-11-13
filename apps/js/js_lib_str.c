/*
 * js_lib_str.c - NovaJS built-ins: String, the string iterator, RegExp
 * objects and their Symbol.match / matchAll / replace / search / split
 * protocol (the matcher itself is js_re.c).
 */
#include "jsi.h"

#define FN(name) static JSValue name(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic)
#define A(i) JS_ARG(i)

static JSValue this_str(JSContext *ctx, JSValue v) {
    if (JS_IsString(v)) return v;
    if (JS_IsNullish(v)) return js_throw_type(ctx, "String.prototype method called on null or undefined");
    return js_tostr(ctx, v);
}

static int arg_int(JSContext *ctx, JSValue v, double def, double *out) {
    if (JS_IsUndefined(v)) { *out = def; return 0; }
    bool e = false;
    *out = js_tointeger(ctx, v, &e);
    return e ? -1 : 0;
}

/* ==== String ============================================================================== */
FN(str_ctor) {
    JSValue s;
    if (argc == 0) s = ctx->empty_str;
    else if (JS_IsSymbol(argv[0]) && JS_IsUndefined(js_new_target(ctx))) {
        JSSB b;
        sb_init(ctx, &b);
        sb_puts(&b, "Symbol(");
        if (JS_IsString(SYM(argv[0])->desc)) sb_putv(&b, SYM(argv[0])->desc);
        sb_putc(&b, ')');
        return sb_finish(&b);
    } else {
        s = js_tostr(ctx, argv[0]);
        if (JS_IsException(s)) return s;
    }
    JSValue nt = js_new_target(ctx);
    if (JS_IsUndefined(nt)) return s;
    JSValue p = js_getp(ctx, nt, ctx->a[A_prototype]);
    if (JS_IsException(p)) return p;
    JSObject *o = js_obj_alloc(ctx, CL_STRING, JS_IsObject(p) ? OBJ(p) : ctx->proto_string);
    if (!o) return JS_EXCEPTION;
    o->u.prim = s;
    return JS_MKOBJ(o);
}

FN(str_from_char_code) {
    JSSB b;
    sb_init(ctx, &b);
    for (int i = 0; i < argc; i++) {
        bool e = false;
        double d = js_tonum(ctx, argv[i], &e);
        if (e) { sb_free(&b); return JS_EXCEPTION; }
        uint32_t u = js_d2u32(d) & 0xFFFF;
        if (b.len >= b.cap && !b.err) sb_putc(&b, u); else if (!b.err) b.s[b.len++] = (uint16_t)u;
    }
    return sb_finish(&b);
}

FN(str_from_code_point) {
    JSSB b;
    sb_init(ctx, &b);
    for (int i = 0; i < argc; i++) {
        bool e = false;
        double d = js_tonum(ctx, argv[i], &e);
        if (e) { sb_free(&b); return JS_EXCEPTION; }
        if (d != trunc(d) || d < 0 || d > 0x10FFFF) { sb_free(&b); return js_throw_range(ctx, "Invalid code point %g", d); }
        sb_putc(&b, (uint32_t)d);
    }
    return sb_finish(&b);
}

FN(str_raw) {
    JSValue cooked = js_toobj(ctx, A(0));
    if (JS_IsException(cooked)) return cooked;
    JSValue raw = js_getp(ctx, cooked, ctx->a[A_raw]);
    if (JS_IsException(raw)) return raw;
    raw = js_toobj(ctx, raw);
    if (JS_IsException(raw)) return raw;
    int64_t n = js_length_of(ctx, raw);
    if (n < 0) return JS_EXCEPTION;
    JSSB b;
    sb_init(ctx, &b);
    for (int64_t i = 0; i < n; i++) {
        JSValue s = js_geti(ctx, raw, (uint32_t)i);
        if (!JS_IsException(s)) s = js_tostr(ctx, s);
        if (JS_IsException(s)) { sb_free(&b); return s; }
        sb_putv(&b, s);
        if (i + 1 < n && i + 1 < argc) {
            JSValue x = js_tostr(ctx, argv[i + 1]);
            if (JS_IsException(x)) { sb_free(&b); return x; }
            sb_putv(&b, x);
        }
    }
    return sb_finish(&b);
}

FN(strp_to_string) {
    if (JS_IsString(this_val)) return this_val;
    if (js_is_cls(this_val, CL_STRING)) return OBJ(this_val)->u.prim;
    return js_throw_type(ctx, "String.prototype.toString requires that 'this' be a String");
}

/* charAt 0, charCodeAt 1, codePointAt 2, at 3 */
FN(strp_char_at) {
    JSValue s = this_str(ctx, this_val);
    if (JS_IsException(s)) return s;
    JSString *str = js_flat(ctx, s);
    if (!str) return JS_EXCEPTION;
    double d;
    if (arg_int(ctx, A(0), 0, &d) < 0) return JS_EXCEPTION;
    if (magic == 3 && d < 0) d += str->len;
    if (d < 0 || d >= str->len) return magic == 0 ? ctx->empty_str : magic == 3 ? JS_UNDEFINED : (magic == 1 ? js_mknum(NAN) : JS_UNDEFINED);
    uint32_t i = (uint32_t)d;
    uint32_t c = str->s[i];
    if (magic == 1) return js_mknum(c);
    if (magic == 2) {
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < str->len && str->s[i + 1] >= 0xDC00 && str->s[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (str->s[i + 1] - 0xDC00);
        return js_mknum(c);
    }
    return js_str16(ctx, str->s + i, 1);
}

FN(strp_concat) {
    JSValue s = this_str(ctx, this_val);
    if (JS_IsException(s)) return s;
    for (int i = 0; i < argc; i++) {
        JSValue x = js_tostr(ctx, argv[i]);
        if (JS_IsException(x)) return x;
        s = js_concat(ctx, s, x);
        if (JS_IsException(s)) return s;
    }
    return s;
}

static int is_regexp(JSContext *ctx, JSValue v) {
    if (!JS_IsObject(v)) return 0;
    JSValue m = js_getp(ctx, v, ctx->wk[WK_MATCH]);
    if (JS_IsException(m)) return -1;
    if (!JS_IsUndefined(m)) return js_truthy(m);
    return js_is_cls(v, CL_REGEXP);
}

/* includes 0, startsWith 1, endsWith 2, indexOf 3, lastIndexOf 4 */
FN(strp_search_str) {
    JSValue s = this_str(ctx, this_val);
    if (JS_IsException(s)) return s;
    if (magic <= 2) {
        int r = is_regexp(ctx, A(0));
        if (r < 0) return JS_EXCEPTION;
        if (r) return js_throw_type(ctx, "First argument must not be a regular expression");
    }
    JSValue n = js_tostr(ctx, A(0));
    if (JS_IsException(n)) return n;
    JSString *h = js_flat(ctx, s), *nd = js_flat(ctx, n);
    if (!h || !nd) return JS_EXCEPTION;
    uint32_t len = h->len;
    double d;
    if (magic == 4) {
        bool e = false;
        double pos = JS_IsUndefined(A(1)) ? NAN : js_tonum(ctx, A(1), &e);
        if (e) return JS_EXCEPTION;
        double start = pos != pos ? len : MAX(0, MIN(trunc(pos), (double)len));
        if (nd->len > len) return js_mknum(-1);
        return js_mknum(js_str_last_index_of(ctx, h, nd, (uint32_t)start));
    }
    if (arg_int(ctx, A(1), magic == 2 ? len : 0, &d) < 0) return JS_EXCEPTION;
    uint32_t p = d < 0 ? 0 : d > len ? len : (uint32_t)d;
    switch (magic) {
    case 0: return JS_BOOL(js_str_index_of(ctx, h, nd, p) >= 0);
    case 1: return JS_BOOL(p + nd->len <= len && !memcmp(h->s + p, nd->s, (size_t)nd->len * 2));
    case 2: return JS_BOOL(nd->len <= p && !memcmp(h->s + p - nd->len, nd->s, (size_t)nd->len * 2));
    default: return js_mknum(js_str_index_of(ctx, h, nd, p));
    }
}

/* slice 0, substring 1, substr 2 */
FN(strp_slice) {
    JSValue s = this_str(ctx, this_val);
    if (JS_IsException(s)) return s;
    double len = js_strlen(s), a, b;
    if (magic == 2) {
        if (arg_int(ctx, A(0), 0, &a) < 0 || arg_int(ctx, A(1), len, &b) < 0) return JS_EXCEPTION;
        if (a < 0) a = MAX(0, a + len);
        a = MIN(a, len);
        b = MIN(MAX(b, 0), len - a);
        if (b <= 0) return ctx->empty_str;
        return js_substr(ctx, s, (uint32_t)a, (uint32_t)(a + b));
    }
    if (arg_int(ctx, A(0), 0, &a) < 0 || arg_int(ctx, A(1), len, &b) < 0) return JS_EXCEPTION;
    if (magic == 0) {
        if (a < 0) a = MAX(0, a + len); else a = MIN(a, len);
        if (b < 0) b = MAX(0, b + len); else b = MIN(b, len);
        if (a >= b) return ctx->empty_str;
    } else {
        a = MIN(MAX(a, 0), len);
        b = MIN(MAX(b, 0), len);
        if (a > b) { double t = a; a = b; b = t; }
    }
    return js_substr(ctx, s, (uint32_t)a, (uint32_t)b);
}

/* toLowerCase 0, toUpperCase 1 */
FN(strp_case) {
    JSValue s = this_str(ctx, this_val);
    if (JS_IsException(s)) return s;
    JSString *str = js_flat(ctx, s);
    if (!str) return JS_EXCEPTION;
    bool change = false;
    for (uint32_t i = 0; i < str->len; i++) {
        uint32_t c = str->s[i];
        if ((magic ? js_case_upper(c) : js_case_lower(c)) != c || (magic && c == 0xDF) || (c >= 0xD800 && c < 0xE000)) { change = true; break; }
    }
    if (!change) return s;
    JSSB b;
    sb_init(ctx, &b);
    for (uint32_t i = 0; i < str->len; i++) {
        uint32_t c = str->s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < str->len && str->s[i + 1] >= 0xDC00 && str->s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (str->s[i + 1] - 0xDC00);
            i++;
        }
        if (magic && c == 0xDF) { sb_puts(&b, "SS"); continue; }
        if (!magic && c == 0x3A3) {
            /* final sigma */
            bool before = i > 0 && js_is_id_start(str->s[i - 1]);
            bool after = i + 1 < str->len && js_is_id_start(str->s[i + 1]);
            sb_putc(&b, before && !after ? 0x3C2 : 0x3C3);
            continue;
        }
        sb_putc(&b, magic ? js_case_upper(c) : js_case_lower(c));
    }
    return sb_finish(&b);
}

/* trim 0, trimStart 1, trimEnd 2 */
FN(strp_trim) {
    JSValue s = this_str(ctx, this_val);
    if (JS_IsException(s)) return s;
    JSString *str = js_flat(ctx, s);
    if (!str) return JS_EXCEPTION;
    uint32_t a = 0, b = str->len;
    if (magic != 2) while (a < b && js_is_space(str->s[a])) a++;
    if (magic != 1) while (b > a && js_is_space(str->s[b - 1])) b--;
    return js_substr(ctx, s, a, b);
}

/* padStart 0, padEnd 1 */
FN(strp_pad) {
    JSValue s = this_str(ctx, this_val);
    if (JS_IsException(s)) return s;
    bool e = false;
    double max = js_tointeger(ctx, A(0), &e);
    if (e) return JS_EXCEPTION;
    uint32_t len = js_strlen(s);
    if (max <= len) return s;
    if (max > (1 << 28)) return js_throw_range(ctx, "Invalid string length");
    JSValue fill = JS_IsUndefined(A(1)) ? js_strc(ctx, " ") : js_tostr(ctx, A(1));
    if (JS_IsException(fill)) return fill;
    JSString *f = js_flat(ctx, fill);
    if (!f) return JS_EXCEPTION;
    if (!f->len) return s;
    uint32_t need = (uint32_t)max - len;
    JSSB b;
    sb_init(ctx, &b);
    if (magic) sb_putv(&b, s);
    for (uint32_t i = 0; i < need; i++) sb_putc(&b, f->s[i % f->len]);
    if (!magic) sb_putv(&b, s);
    return sb_finish(&b);
}

FN(strp_repeat) {
    JSValue s = this_str(ctx, this_val);
    if (JS_IsException(s)) return s;
    bool e = false;
    double n = js_tointeger(ctx, A(0), &e);
    if (e) return JS_EXCEPTION;
    if (n < 0 || isinf(n)) return js_throw_range(ctx, "Invalid count value: %g", n);
    uint32_t len = js_strlen(s);
    if (n == 0 || len == 0) return ctx->empty_str;
    if (n * len >= (1 << 28)) return js_throw_range(ctx, "Invalid string length");
    JSSB b;
    sb_init(ctx, &b);
    for (uint32_t i = 0; i < (uint32_t)n && !b.err; i++) sb_putv(&b, s);
    return sb_finish(&b);
}

FN(strp_locale_compare) {
    JSValue s = this_str(ctx, this_val);
    if (JS_IsException(s)) return s;
    JSValue t = js_tostr(ctx, A(0));
    if (JS_IsException(t)) return t;
    JSString *a = js_flat(ctx, s), *b = js_flat(ctx, t);
    if (!a || !b) return JS_EXCEPTION;
    /* primary: letters case-insensitively (accents folded for Latin-1), then case, then code units */
    uint32_t n = MIN(a->len, b->len);
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t x = a->s[i], y = b->s[i];
            if (pass == 0) {
                x = js_case_lower(x); y = js_case_lower(y);
                static const char fold[] = "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
                if (x >= 0xE0 && x <= 0xFF) x = (uint8_t)fold[x - 0xE0];
                if (y >= 0xE0 && y <= 0xFF) y = (uint8_t)fold[y - 0xE0];
            } else {
                /* lower case sorts first */
                bool xl = js_case_lower(x) == x, yl = js_case_lower(y) == y;
                if (xl != yl) return js_mknum(xl ? -1 : 1);
            }
            if (x != y) return js_mknum(x < y ? -1 : 1);
        }
        if (pass == 0 && a->len != b->len) return js_mknum(a->len < b->len ? -1 : 1);
    }
    int c = js_strcmp(ctx, s, t);
    return js_mknum(c);
}

FN(strp_normalize) {
    JSValue s = this_str(ctx, this_val);
    if (JS_IsException(s)) return s;
    if (!JS_IsUndefined(A(0))) {
        JSValue f = js_tostr(ctx, A(0));
        if (JS_IsException(f)) return f;
        if (!js_str_eq_ascii(ctx, f, "NFC") && !js_str_eq_ascii(ctx, f, "NFD") && !js_str_eq_ascii(ctx, f, "NFKC") && !js_str_eq_ascii(ctx, f, "NFKD"))
            return js_throw_range(ctx, "The normalization form should be one of NFC, NFD, NFKC, NFKD.");
    }
    return s;
}

/* isWellFormed 0, toWellFormed 1 */
FN(strp_well_formed) {
    JSValue s = this_str(ctx, this_val);
    if (JS_IsException(s)) return s;
    JSString *str = js_flat(ctx, s);
    if (!str) return JS_EXCEPTION;
    JSSB b;
    if (magic) sb_init(ctx, &b);
    for (uint32_t i = 0; i < str->len; i++) {
        uint32_t c = str->s[i];
        bool bad = false;
        if (c >= 0xD800 && c < 0xDC00) {
            if (i + 1 < str->len && str->s[i + 1] >= 0xDC00 && str->s[i + 1] < 0xE000) { if (magic) { sb_putc(&b, c); sb_putc(&b, str->s[i + 1]); } i++; continue; }
            bad = true;
        } else if (c >= 0xDC00 && c < 0xE000) bad = true;
        if (bad && !magic) return JS_FALSE;
        if (magic) sb_putc(&b, bad ? 0xFFFD : c);
    }
    return magic ? sb_finish(&b) : JS_TRUE;
}

FN(strp_split) {
    if (JS_IsNullish(this_val)) return js_throw_type(ctx, "String.prototype.split called on null or undefined");
    JSValue sep = A(0), lim = A(1);
    if (!JS_IsNullish(sep)) {
        JSValue sp = js_get_method(ctx, sep, ctx->wk[WK_SPLIT]);
        if (JS_IsException(sp)) return sp;
        if (!JS_IsUndefined(sp)) { JSValue args[2] = { this_val, lim }; return js_callv(ctx, sp, sep, 2, args); }
    }
    JSValue s = js_tostr(ctx, this_val);
    if (JS_IsException(s)) return s;
    uint32_t limit = 0xFFFFFFFFu;
    if (!JS_IsUndefined(lim)) { bool e = false; double d = js_tonum(ctx, lim, &e); if (e) return JS_EXCEPTION; limit = js_d2u32(d); }
    JSValue r = js_array_new(ctx, 0);
    if (JS_IsException(r)) return r;
    JSValue sepstr = js_tostr(ctx, sep);
    if (JS_IsException(sepstr)) return sepstr;
    if (limit == 0) return r;
    if (JS_IsUndefined(sep)) { js_array_push_v(ctx, r, s); return r; }
    JSString *h = js_flat(ctx, s), *n = js_flat(ctx, sepstr);
    if (!h || !n) return JS_EXCEPTION;
    if (n->len == 0) {
        for (uint32_t i = 0; i < h->len && OBJ(r)->u.a.len < limit; i++) {
            JSValue c = js_substr(ctx, s, i, i + 1);
            if (JS_IsException(c) || js_array_push_v(ctx, r, c) < 0) return JS_EXCEPTION;
            h = js_flat(ctx, s);
        }
        return r;
    }
    uint32_t p = 0;
    for (;;) {
        int32_t q = js_str_index_of(ctx, h, n, p);
        if (q < 0) break;
        JSValue part = js_substr(ctx, s, p, (uint32_t)q);
        if (JS_IsException(part) || js_array_push_v(ctx, r, part) < 0) return JS_EXCEPTION;
        if (OBJ(r)->u.a.len >= limit) return r;
        p = (uint32_t)q + n->len;
        h = js_flat(ctx, s);
        n = js_flat(ctx, sepstr);
    }
    JSValue rest = js_substr(ctx, s, p, js_strlen(s));
    if (JS_IsException(rest) || js_array_push_v(ctx, r, rest) < 0) return JS_EXCEPTION;
    return r;
}

/* GetSubstitution */
static JSValue substitution(JSContext *ctx, JSValue matched, JSValue str, uint32_t pos, JSValue caps, JSValue named, JSValue repl) {
    JSString *r = js_flat(ctx, repl);
    if (!r) return JS_EXCEPTION;
    bool has_dollar = false;
    for (uint32_t i = 0; i < r->len; i++) if (r->s[i] == '$') { has_dollar = true; break; }
    if (!has_dollar) return repl;
    uint32_t mlen = js_strlen(matched), slen = js_strlen(str);
    uint32_t m = js_is_cls(caps, CL_ARRAY) ? OBJ(caps)->u.a.len : 0;
    JSSB b;
    sb_init(ctx, &b);
    for (uint32_t i = 0; i < r->len; i++) {
        uint32_t c = r->s[i];
        if (c != '$' || i + 1 >= r->len) { sb_putc(&b, c); continue; }
        uint32_t d = r->s[i + 1];
        if (d == '$') { sb_putc(&b, '$'); i++; }
        else if (d == '&') { sb_putv(&b, matched); i++; }
        else if (d == '`') { JSValue x = js_substr(ctx, str, 0, pos); if (!JS_IsException(x)) sb_putv(&b, x); i++; }
        else if (d == '\'') { uint32_t t = MIN(pos + mlen, slen); JSValue x = js_substr(ctx, str, t, slen); if (!JS_IsException(x)) sb_putv(&b, x); i++; }
        else if (d >= '0' && d <= '9') {
            uint32_t k = d - '0', used = 1;
            if (i + 2 < r->len && r->s[i + 2] >= '0' && r->s[i + 2] <= '9') {
                uint32_t k2 = k * 10 + (r->s[i + 2] - '0');
                if (k2 >= 1 && k2 <= m) { k = k2; used = 2; }
            }
            if (k >= 1 && k <= m) {
                JSValue cv = OBJ(caps)->u.a.v[k - 1];
                if (!JS_IsUndefined(cv)) sb_putv(&b, cv);
                i += used;
            } else sb_putc(&b, '$');
        } else if (d == '<' && !JS_IsUndefined(named)) {
            uint32_t e = i + 2;
            while (e < r->len && r->s[e] != '>') e++;
            if (e >= r->len) { sb_putc(&b, '$'); continue; }
            JSValue key = js_atom16(ctx, r->s + i + 2, e - i - 2);
            JSValue v = JS_IsException(key) ? key : js_getp(ctx, named, key);
            if (JS_IsException(v)) { sb_free(&b); return v; }
            if (!JS_IsUndefined(v)) { JSValue sv = js_tostr(ctx, v); if (JS_IsException(sv)) { sb_free(&b); return sv; } sb_putv(&b, sv); }
            i = e;
            r = js_flat(ctx, repl);
        } else sb_putc(&b, '$');
    }
    return sb_finish(&b);
}

/* replace 0, replaceAll 1 */
FN(strp_replace) {
    if (JS_IsNullish(this_val)) return js_throw_type(ctx, "String.prototype.replace called on null or undefined");
    JSValue sv = A(0), rv = A(1);
    if (!JS_IsNullish(sv)) {
        if (magic) {
            int r = is_regexp(ctx, sv);
            if (r < 0) return JS_EXCEPTION;
            if (r) {
                JSValue fl = js_getp(ctx, sv, ctx->a[A_flags]);
                if (JS_IsException(fl)) return fl;
                fl = js_tostr(ctx, fl);
                if (JS_IsException(fl)) return fl;
                JSString *f = js_flat(ctx, fl);
                bool g = false;
                for (uint32_t i = 0; f && i < f->len; i++) if (f->s[i] == 'g') g = true;
                if (!g) return js_throw_type(ctx, "replaceAll must be called with a global RegExp");
            }
        }
        JSValue m = js_get_method(ctx, sv, ctx->wk[WK_REPLACE]);
        if (JS_IsException(m)) return m;
        if (!JS_IsUndefined(m)) { JSValue args[2] = { this_val, rv }; return js_callv(ctx, m, sv, 2, args); }
    }
    JSValue s = js_tostr(ctx, this_val);
    if (JS_IsException(s)) return s;
    JSValue search = js_tostr(ctx, sv);
    if (JS_IsException(search)) return search;
    bool functional = js_is_callable(rv);
    if (!functional) { rv = js_tostr(ctx, rv); if (JS_IsException(rv)) return rv; }
    JSString *h = js_flat(ctx, s), *n = js_flat(ctx, search);
    if (!h || !n) return JS_EXCEPTION;
    uint32_t adv = MAX(1u, n->len);
    JSSB b;
    sb_init(ctx, &b);
    uint32_t end = 0;
    int32_t pos = js_str_index_of(ctx, h, n, 0);
    if (pos < 0) { sb_free(&b); return s; }
    while (pos >= 0) {
        JSValue pre = js_substr(ctx, s, end, (uint32_t)pos);
        if (JS_IsException(pre)) { sb_free(&b); return pre; }
        sb_putv(&b, pre);
        JSValue rep;
        if (functional) {
            JSValue args[3] = { search, js_mknum(pos), s };
            rep = js_callv(ctx, rv, JS_UNDEFINED, 3, args);
            if (!JS_IsException(rep)) rep = js_tostr(ctx, rep);
        } else rep = substitution(ctx, search, s, (uint32_t)pos, JS_UNDEFINED, JS_UNDEFINED, rv);
        if (JS_IsException(rep)) { sb_free(&b); return rep; }
        sb_putv(&b, rep);
        end = (uint32_t)pos + n->len;
        if (!magic) break;
        h = js_flat(ctx, s);
        n = js_flat(ctx, search);
        pos = (uint32_t)pos + adv <= h->len ? js_str_index_of(ctx, h, n, (uint32_t)pos + adv) : -1;
    }
    JSValue rest = js_substr(ctx, s, end, js_strlen(s));
    if (JS_IsException(rest)) { sb_free(&b); return rest; }
    sb_putv(&b, rest);
    return sb_finish(&b);
}

/* match 0, matchAll 1, search 2 */
FN(strp_match) {
    if (JS_IsNullish(this_val)) return js_throw_type(ctx, "String.prototype.match called on null or undefined");
    JSValue rx = A(0);
    int wk = magic == 0 ? WK_MATCH : magic == 1 ? WK_MATCH_ALL : WK_SEARCH;
    if (!JS_IsNullish(rx)) {
        if (magic == 1) {
            int r = is_regexp(ctx, rx);
            if (r < 0) return JS_EXCEPTION;
            if (r) {
                JSValue fl = js_getp(ctx, rx, ctx->a[A_flags]);
                if (!JS_IsException(fl)) fl = js_tostr(ctx, fl);
                if (JS_IsException(fl)) return fl;
                JSString *f = js_flat(ctx, fl);
                bool g = false;
                for (uint32_t i = 0; f && i < f->len; i++) if (f->s[i] == 'g') g = true;
                if (!g) return js_throw_type(ctx, "matchAll must be called with a global RegExp");
            }
        }
        JSValue m = js_get_method(ctx, rx, ctx->wk[wk]);
        if (JS_IsException(m)) return m;
        if (!JS_IsUndefined(m)) return js_callv(ctx, m, rx, 1, &this_val);
    }
    JSValue s = js_tostr(ctx, this_val);
    if (JS_IsException(s)) return s;
    JSValue flags = magic == 1 ? js_strc(ctx, "g") : JS_UNDEFINED;
    JSValue re = js_regexp_create(ctx, JS_IsUndefined(rx) ? ctx->empty_str : rx, flags);
    if (JS_IsException(re)) return re;
    return js_invoke(ctx, re, ctx->wk[wk], 1, &s);
}

/* ==== string iterator ======================================================================== */
FN(strp_iter) {
    JSValue s = this_str(ctx, this_val);
    if (JS_IsException(s)) return s;
    return js_new_iter(ctx, ctx->proto_string_iter, s, ITT_STRING, IT_VALUES);
}

FN(str_iter_next) {
    if (!js_is_cls(this_val, CL_ITER)) return js_throw_type(ctx, "next method called on incompatible receiver");
    JSIter *it = OBJ(this_val)->u.p;
    if (it->type != ITT_STRING) return js_throw_type(ctx, "next method called on incompatible receiver");
    if (it->done) return js_iter_result(ctx, JS_UNDEFINED, true);
    JSString *s = js_flat(ctx, it->target);
    if (!s) return JS_EXCEPTION;
    if (it->idx >= s->len) { it->done = true; return js_iter_result(ctx, JS_UNDEFINED, true); }
    uint32_t i = it->idx, n = 1;
    if (s->s[i] >= 0xD800 && s->s[i] < 0xDC00 && i + 1 < s->len && s->s[i + 1] >= 0xDC00 && s->s[i + 1] < 0xE000) n = 2;
    it->idx += n;
    JSValue c = js_str16(ctx, s->s + i, n);
    if (JS_IsException(c)) return c;
    return js_iter_result(ctx, c, false);
}

/* ==== RegExp ==================================================================================== */
static int parse_flags(JSContext *ctx, JSValue f, uint32_t *out) {
    *out = 0;
    if (JS_IsUndefined(f)) return 0;
    JSString *s = js_flat(ctx, f);
    if (!s) return -1;
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t bit;
        switch (s->s[i]) {
        case 'g': bit = RE_G; break;
        case 'i': bit = RE_I; break;
        case 'm': bit = RE_M; break;
        case 's': bit = RE_S; break;
        case 'u': bit = RE_U; break;
        case 'y': bit = RE_Y; break;
        case 'd': bit = RE_D; break;
        case 'v': bit = RE_V; break;
        default: bit = 0; break;
        }
        if (!bit || (*out & bit)) { char t[64]; js_str_to_ascii(ctx, f, t, sizeof t); js_throw_syntax(ctx, "Invalid regular expression flags '%s'", t); return -1; }
        *out |= bit;
    }
    if ((*out & RE_U) && (*out & RE_V)) { js_throw_syntax(ctx, "Invalid regular expression flags"); return -1; }
    return 0;
}

static JSValue regexp_init(JSContext *ctx, JSObject *o, JSValue pattern, JSValue flags) {
    uint32_t fl;
    if (parse_flags(ctx, flags, &fl) < 0) return JS_EXCEPTION;
    JSString *p = js_flat(ctx, pattern);
    if (!p) return JS_EXCEPTION;
    char err[160];
    JSRegExp *re = js_re_compile(ctx, p->s, p->len, fl, err, sizeof err);
    if (!re) {
        char t[100];
        js_str_to_ascii(ctx, pattern, t, sizeof t);
        return js_throw_syntax(ctx, "Invalid regular expression: /%s/: %s", t, err);
    }
    JSRegExpObj *r = o->u.p;
    if (!r) {
        r = js_mallocz(ctx, sizeof(JSRegExpObj));
        if (!r) { js_re_free(ctx, re); return JS_EXCEPTION; }
        o->u.p = r;
    } else js_re_free(ctx, r->re);
    r->re = re;
    r->source = pattern;
    r->flags = fl;
    JSProp *lp = js_find_prop(ctx, o, ctx->a[A_lastIndex]);
    if (lp) lp->val = js_mknum(0);
    else js_add_prop(ctx, o, ctx->a[A_lastIndex], js_mknum(0), P_W);
    return JS_MKOBJ(o);
}

JSValue js_regexp_create(JSContext *ctx, JSValue pattern, JSValue flags) {
    JSValue p = JS_IsUndefined(pattern) ? ctx->empty_str : js_tostr(ctx, pattern);
    if (JS_IsException(p)) return p;
    JSValue f = JS_IsUndefined(flags) ? JS_UNDEFINED : js_tostr(ctx, flags);
    if (JS_IsException(f)) return f;
    JSObject *o = js_obj_alloc(ctx, CL_REGEXP, ctx->proto_regexp);
    if (!o) return JS_EXCEPTION;
    return regexp_init(ctx, o, p, f);
}

FN(re_ctor) {
    JSValue pat = A(0), flags = A(1), nt = js_new_target(ctx);
    int isre = is_regexp(ctx, pat);
    if (isre < 0) return JS_EXCEPTION;
    if (JS_IsUndefined(nt)) {
        if (isre && JS_IsUndefined(flags)) {
            JSValue c = js_getp(ctx, pat, ctx->a[A_constructor]);
            if (JS_IsException(c)) return c;
            if (c == JS_MKOBJ(ctx->ctor_regexp)) return pat;
        }
        nt = JS_MKOBJ(ctx->ctor_regexp);
    }
    JSValue P, F;
    if (js_is_cls(pat, CL_REGEXP)) {
        JSRegExpObj *r = OBJ(pat)->u.p;
        P = r->source;
        if (JS_IsUndefined(flags)) {
            char fb[10];
            int k = 0;
            if (r->flags & RE_D) fb[k++] = 'd';
            if (r->flags & RE_G) fb[k++] = 'g';
            if (r->flags & RE_I) fb[k++] = 'i';
            if (r->flags & RE_M) fb[k++] = 'm';
            if (r->flags & RE_S) fb[k++] = 's';
            if (r->flags & RE_U) fb[k++] = 'u';
            if (r->flags & RE_V) fb[k++] = 'v';
            if (r->flags & RE_Y) fb[k++] = 'y';
            F = js_str8(ctx, fb, (uint32_t)k);
        } else F = flags;
    } else if (isre) {
        P = js_getp(ctx, pat, ctx->a[A_source]);
        if (JS_IsException(P)) return P;
        F = JS_IsUndefined(flags) ? js_getp(ctx, pat, ctx->a[A_flags]) : flags;
        if (JS_IsException(F)) return F;
    } else { P = pat; F = flags; }
    JSValue proto = js_getp(ctx, nt, ctx->a[A_prototype]);
    if (JS_IsException(proto)) return proto;
    JSObject *o = js_obj_alloc(ctx, CL_REGEXP, JS_IsObject(proto) ? OBJ(proto) : ctx->proto_regexp);
    if (!o) return JS_EXCEPTION;
    P = JS_IsUndefined(P) ? ctx->empty_str : js_tostr(ctx, P);
    if (JS_IsException(P)) return P;
    if (!JS_IsUndefined(F)) { F = js_tostr(ctx, F); if (JS_IsException(F)) return F; }
    return regexp_init(ctx, o, P, F);
}

/* the builtin exec: match object or null */
static JSValue builtin_exec(JSContext *ctx, JSValue rx, JSValue s) {
    JSRegExpObj *r = OBJ(rx)->u.p;
    if (!r || !r->re) return js_throw_type(ctx, "RegExp not initialized");
    JSValue li = js_getp(ctx, rx, ctx->a[A_lastIndex]);
    if (JS_IsException(li)) return li;
    bool e = false;
    double last = js_tointeger(ctx, li, &e);
    if (e) return JS_EXCEPTION;
    if (last < 0) last = 0;
    bool global = (r->flags & RE_G) != 0, sticky = (r->flags & RE_Y) != 0;
    if (!global && !sticky) last = 0;
    JSString *str = js_flat(ctx, s);
    if (!str) return JS_EXCEPTION;
    if (last > str->len) {
        if (global || sticky) { if (js_setp(ctx, rx, ctx->a[A_lastIndex], js_mknum(0), rx, true) < 0) return JS_EXCEPTION; }
        return JS_NULL;
    }
    int nc = js_re_ncaps(r->re);
    int32_t small[40], *caps = nc * 2 <= 40 ? small : malloc(sizeof(int32_t) * (size_t)nc * 2);
    if (!caps) return js_throw_oom(ctx);
    int m = js_re_exec(ctx, r->re, str->s, str->len, (uint32_t)last, caps);
    if (m <= 0) {
        if (caps != small) free(caps);
        if (m < 0) return JS_EXCEPTION;
        if (global || sticky) { if (js_setp(ctx, rx, ctx->a[A_lastIndex], js_mknum(0), rx, true) < 0) return JS_EXCEPTION; }
        return JS_NULL;
    }
    if (global || sticky) {
        if (js_setp(ctx, rx, ctx->a[A_lastIndex], js_mknum(caps[1]), rx, true) < 0) { if (caps != small) free(caps); return JS_EXCEPTION; }
    }
    JSValue a = js_array_new(ctx, 0);
    JSValue r_ = JS_EXCEPTION;
    if (JS_IsException(a)) goto out;
    for (int g = 0; g < nc; g++) {
        JSValue v = caps[2 * g] >= 0 && caps[2 * g + 1] >= 0 ? js_substr(ctx, s, (uint32_t)caps[2 * g], (uint32_t)caps[2 * g + 1]) : JS_UNDEFINED;
        if (JS_IsException(v) || js_array_push_v(ctx, a, v) < 0) goto out;
    }
    js_add_prop(ctx, OBJ(a), ctx->a[A_index], js_mknum(caps[0]), P_W | P_E | P_C);
    js_add_prop(ctx, OBJ(a), ctx->a[A_input], s, P_W | P_E | P_C);
    JSValue names = js_re_group_names(ctx, r->re);
    if (JS_IsException(names)) goto out;
    JSValue groups = JS_UNDEFINED;
    if (!JS_IsUndefined(names)) {
        groups = js_obj_new(ctx, CL_OBJECT, NULL);
        if (JS_IsException(groups)) goto out;
        for (int g = 1; g < nc; g++) {
            JSValue nm = OBJ(names)->u.a.v[g];
            if (JS_IsUndefined(nm)) continue;
            js_add_prop(ctx, OBJ(groups), nm, OBJ(a)->u.a.v[g], P_W | P_E | P_C);
        }
    }
    js_add_prop(ctx, OBJ(a), ctx->a[A_groups], groups, P_W | P_E | P_C);
    if (r->flags & RE_D) {
        JSValue ind = js_array_new(ctx, 0);
        if (JS_IsException(ind)) goto out;
        for (int g = 0; g < nc; g++) {
            JSValue v = JS_UNDEFINED;
            if (caps[2 * g] >= 0) {
                JSValue pr[2] = { js_mknum(caps[2 * g]), js_mknum(caps[2 * g + 1]) };
                v = js_array_from_list(ctx, pr, 2);
                if (JS_IsException(v)) goto out;
            }
            js_array_push_v(ctx, ind, v);
        }
        js_add_prop(ctx, OBJ(a), ctx->a[A_indices], ind, P_W | P_E | P_C);
    }
    r_ = a;
out:
    if (caps != small) free(caps);
    return r_;
}

JSValue js_regexp_exec_v(JSContext *ctx, JSValue rx, JSValue s) {
    JSValue ex = js_getp(ctx, rx, ctx->a[A_exec]);
    if (JS_IsException(ex)) return ex;
    if (js_is_callable(ex) && ex != ctx->regexp_exec) {
        JSValue r = js_callv(ctx, ex, rx, 1, &s);
        if (JS_IsException(r)) return r;
        if (!JS_IsObject(r) && !JS_IsNull(r)) return js_throw_type(ctx, "exec result must be an object or null");
        return r;
    }
    if (!js_is_cls(rx, CL_REGEXP)) return js_throw_type(ctx, "RegExp.prototype.exec called on incompatible receiver");
    return builtin_exec(ctx, rx, s);
}

FN(rep_exec) {
    if (!js_is_cls(this_val, CL_REGEXP)) return js_throw_type(ctx, "RegExp.prototype.exec called on incompatible receiver");
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    return builtin_exec(ctx, this_val, s);
}

FN(rep_test) {
    if (!JS_IsObject(this_val)) return js_throw_type(ctx, "RegExp.prototype.test called on incompatible receiver");
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    JSValue r = js_regexp_exec_v(ctx, this_val, s);
    if (JS_IsException(r)) return r;
    return JS_BOOL(!JS_IsNull(r));
}

FN(rep_source) {
    if (!js_is_cls(this_val, CL_REGEXP)) {
        if (this_val == JS_MKOBJ(ctx->proto_regexp)) return js_strc(ctx, "(?:)");
        return js_throw_type(ctx, "RegExp.prototype.source getter called on non-RegExp");
    }
    JSRegExpObj *r = OBJ(this_val)->u.p;
    JSString *s = js_flat(ctx, r->source);
    if (!s) return JS_EXCEPTION;
    if (!s->len) return js_strc(ctx, "(?:)");
    /* escape / and line terminators */
    bool need = false;
    for (uint32_t i = 0; i < s->len; i++) if (s->s[i] == '/' || s->s[i] == '\n' || s->s[i] == '\r') { need = true; break; }
    if (!need) return r->source;
    JSSB b;
    sb_init(ctx, &b);
    bool cls = false;
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t c = s->s[i];
        if (c == '\\' && i + 1 < s->len) { sb_putc(&b, c); sb_putc(&b, s->s[++i]); continue; }
        if (c == '[') cls = true; else if (c == ']') cls = false;
        if (c == '/' && !cls) { sb_puts(&b, "\\/"); continue; }
        if (c == '\n') { sb_puts(&b, "\\n"); continue; }
        if (c == '\r') { sb_puts(&b, "\\r"); continue; }
        sb_putc(&b, c);
    }
    return sb_finish(&b);
}

static const struct { uint32_t bit; char ch; int atom; } flag_tab[] = {
    { RE_D, 'd', A_hasIndices }, { RE_G, 'g', A_global }, { RE_I, 'i', A_ignoreCase }, { RE_M, 'm', A_multiline },
    { RE_S, 's', A_dotAll }, { RE_U, 'u', A_unicode }, { RE_V, 'v', A_unicodeSets }, { RE_Y, 'y', A_sticky },
};

FN(rep_flag) {
    if (!js_is_cls(this_val, CL_REGEXP)) {
        if (this_val == JS_MKOBJ(ctx->proto_regexp)) return JS_UNDEFINED;
        return js_throw_type(ctx, "RegExp flag getter called on non-RegExp");
    }
    return JS_BOOL(((JSRegExpObj *)OBJ(this_val)->u.p)->flags & (uint32_t)magic);
}

FN(rep_flags) {
    if (!JS_IsObject(this_val)) return js_throw_type(ctx, "RegExp.prototype.flags getter called on non-object");
    char fb[10];
    int k = 0;
    for (int i = 0; i < JS_COUNTOF(flag_tab); i++) {
        JSValue v = js_getp(ctx, this_val, ctx->a[flag_tab[i].atom]);
        if (JS_IsException(v)) return v;
        if (js_truthy(v)) fb[k++] = flag_tab[i].ch;
    }
    return js_str8(ctx, fb, (uint32_t)k);
}

FN(rep_to_string) {
    if (!JS_IsObject(this_val)) return js_throw_type(ctx, "RegExp.prototype.toString called on non-object");
    JSValue src = js_getp(ctx, this_val, ctx->a[A_source]);
    if (!JS_IsException(src)) src = js_tostr(ctx, src);
    if (JS_IsException(src)) return src;
    JSValue fl = js_getp(ctx, this_val, ctx->a[A_flags]);
    if (!JS_IsException(fl)) fl = js_tostr(ctx, fl);
    if (JS_IsException(fl)) return fl;
    JSSB b;
    sb_init(ctx, &b);
    sb_putc(&b, '/');
    sb_putv(&b, src);
    sb_putc(&b, '/');
    sb_putv(&b, fl);
    return sb_finish(&b);
}

FN(rep_compile) {
    if (!js_is_cls(this_val, CL_REGEXP)) return js_throw_type(ctx, "not a RegExp");
    JSValue p = A(0), f = A(1);
    if (js_is_cls(p, CL_REGEXP)) {
        if (!JS_IsUndefined(f)) return js_throw_type(ctx, "Cannot supply flags when constructing one RegExp from another");
        JSRegExpObj *r = OBJ(p)->u.p;
        p = r->source;
        JSValue fv = js_getp(ctx, A(0), ctx->a[A_flags]);
        if (JS_IsException(fv)) return fv;
        f = fv;
    }
    p = JS_IsUndefined(p) ? ctx->empty_str : js_tostr(ctx, p);
    if (JS_IsException(p)) return p;
    if (!JS_IsUndefined(f)) { f = js_tostr(ctx, f); if (JS_IsException(f)) return f; }
    return regexp_init(ctx, OBJ(this_val), p, f);
}

static uint32_t advance_index(JSString *s, uint32_t i, bool unicode) {
    if (!unicode || i + 1 >= s->len) return i + 1;
    if (s->s[i] >= 0xD800 && s->s[i] < 0xDC00 && s->s[i + 1] >= 0xDC00 && s->s[i + 1] < 0xE000) return i + 2;
    return i + 1;
}

static int read_flags(JSContext *ctx, JSValue rx, bool *g, bool *u) {
    JSValue fl = js_getp(ctx, rx, ctx->a[A_flags]);
    if (!JS_IsException(fl)) fl = js_tostr(ctx, fl);
    if (JS_IsException(fl)) return -1;
    JSString *f = js_flat(ctx, fl);
    *g = *u = false;
    for (uint32_t i = 0; f && i < f->len; i++) { if (f->s[i] == 'g') *g = true; if (f->s[i] == 'u' || f->s[i] == 'v') *u = true; }
    return 0;
}

static int bump_last_index(JSContext *ctx, JSValue rx, JSValue s, bool unicode) {
    JSValue li = js_getp(ctx, rx, ctx->a[A_lastIndex]);
    if (JS_IsException(li)) return -1;
    bool e = false;
    double d = js_tointeger(ctx, li, &e);
    if (e) return -1;
    JSString *str = js_flat(ctx, s);
    if (!str) return -1;
    uint32_t ni = d < 0 ? 1 : advance_index(str, (uint32_t)d, unicode);
    return js_setp(ctx, rx, ctx->a[A_lastIndex], js_mknum(ni), rx, true) < 0 ? -1 : 0;
}

FN(rep_match) {
    if (!JS_IsObject(this_val)) return js_throw_type(ctx, "RegExp.prototype[Symbol.match] called on non-object");
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    bool g, u;
    if (read_flags(ctx, this_val, &g, &u) < 0) return JS_EXCEPTION;
    if (!g) return js_regexp_exec_v(ctx, this_val, s);
    if (js_setp(ctx, this_val, ctx->a[A_lastIndex], js_mknum(0), this_val, true) < 0) return JS_EXCEPTION;
    JSValue out = js_array_new(ctx, 0);
    if (JS_IsException(out)) return out;
    for (;;) {
        JSValue r = js_regexp_exec_v(ctx, this_val, s);
        if (JS_IsException(r)) return r;
        if (JS_IsNull(r)) return OBJ(out)->u.a.len ? out : JS_NULL;
        JSValue m = js_geti(ctx, r, 0);
        if (!JS_IsException(m)) m = js_tostr(ctx, m);
        if (JS_IsException(m) || js_array_push_v(ctx, out, m) < 0) return JS_EXCEPTION;
        if (!js_strlen(m) && bump_last_index(ctx, this_val, s, u) < 0) return JS_EXCEPTION;
    }
}

FN(rep_replace) {
    JSValue rx = this_val;
    if (!JS_IsObject(rx)) return js_throw_type(ctx, "RegExp.prototype[Symbol.replace] called on non-object");
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    JSValue rv = A(1);
    bool functional = js_is_callable(rv);
    if (!functional) { rv = js_tostr(ctx, rv); if (JS_IsException(rv)) return rv; }
    bool g, u;
    if (read_flags(ctx, rx, &g, &u) < 0) return JS_EXCEPTION;
    if (g && js_setp(ctx, rx, ctx->a[A_lastIndex], js_mknum(0), rx, true) < 0) return JS_EXCEPTION;
    JSValue results = js_array_new(ctx, 0);
    if (JS_IsException(results)) return results;
    for (;;) {
        JSValue r = js_regexp_exec_v(ctx, rx, s);
        if (JS_IsException(r)) return r;
        if (JS_IsNull(r)) break;
        if (js_array_push_v(ctx, results, r) < 0) return JS_EXCEPTION;
        if (!g) break;
        JSValue m = js_geti(ctx, r, 0);
        if (!JS_IsException(m)) m = js_tostr(ctx, m);
        if (JS_IsException(m)) return m;
        if (!js_strlen(m) && bump_last_index(ctx, rx, s, u) < 0) return JS_EXCEPTION;
    }
    JSSB b;
    sb_init(ctx, &b);
    uint32_t next = 0, slen = js_strlen(s);
    uint32_t nres = OBJ(results)->u.a.len;
    for (uint32_t i = 0; i < nres; i++) {
        JSValue r = OBJ(results)->u.a.v[i];
        int64_t nc = js_length_of(ctx, r);
        if (nc < 0) goto fail;
        nc = nc > 0 ? nc - 1 : 0;
        JSValue matched = js_geti(ctx, r, 0);
        if (!JS_IsException(matched)) matched = js_tostr(ctx, matched);
        if (JS_IsException(matched)) goto fail;
        JSValue pv = js_getp(ctx, r, ctx->a[A_index]);
        if (JS_IsException(pv)) goto fail;
        bool e = false;
        double pd = js_tointeger(ctx, pv, &e);
        if (e) goto fail;
        uint32_t pos = pd < 0 ? 0 : pd > slen ? slen : (uint32_t)pd;
        JSValue caps = js_array_new(ctx, 0);
        if (JS_IsException(caps)) goto fail;
        for (int64_t k = 1; k <= nc; k++) {
            JSValue c = js_geti(ctx, r, (uint32_t)k);
            if (JS_IsException(c)) goto fail;
            if (!JS_IsUndefined(c)) { c = js_tostr(ctx, c); if (JS_IsException(c)) goto fail; }
            if (js_array_push_v(ctx, caps, c) < 0) goto fail;
        }
        JSValue named = js_getp(ctx, r, ctx->a[A_groups]);
        if (JS_IsException(named)) goto fail;
        JSValue rep;
        if (functional) {
            uint32_t na = (uint32_t)nc + 3 + (JS_IsUndefined(named) ? 0 : 1);
            JSValue *args = malloc(sizeof(JSValue) * na);
            if (!args) { js_throw_oom(ctx); goto fail; }
            args[0] = matched;
            for (int64_t k = 0; k < nc; k++) args[1 + k] = OBJ(caps)->u.a.v[k];
            args[nc + 1] = js_mknum(pos);
            args[nc + 2] = s;
            if (!JS_IsUndefined(named)) args[nc + 3] = named;
            rep = js_callv(ctx, rv, JS_UNDEFINED, (int)na, args);
            free(args);
            if (!JS_IsException(rep)) rep = js_tostr(ctx, rep);
        } else {
            if (!JS_IsUndefined(named)) { named = js_toobj(ctx, named); if (JS_IsException(named)) goto fail; }
            rep = substitution(ctx, matched, s, pos, caps, named, rv);
        }
        if (JS_IsException(rep)) goto fail;
        if (pos >= next) {
            JSValue pre = js_substr(ctx, s, next, pos);
            if (JS_IsException(pre)) goto fail;
            sb_putv(&b, pre);
            sb_putv(&b, rep);
            next = pos + js_strlen(matched);
        }
    }
    if (next < slen) {
        JSValue rest = js_substr(ctx, s, next, slen);
        if (JS_IsException(rest)) goto fail;
        sb_putv(&b, rest);
    }
    return sb_finish(&b);
fail:
    sb_free(&b);
    return JS_EXCEPTION;
}

FN(rep_search) {
    JSValue rx = this_val;
    if (!JS_IsObject(rx)) return js_throw_type(ctx, "RegExp.prototype[Symbol.search] called on non-object");
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    JSValue prev = js_getp(ctx, rx, ctx->a[A_lastIndex]);
    if (JS_IsException(prev)) return prev;
    if (!js_same_value(prev, js_mknum(0), false) && js_setp(ctx, rx, ctx->a[A_lastIndex], js_mknum(0), rx, true) < 0) return JS_EXCEPTION;
    JSValue r = js_regexp_exec_v(ctx, rx, s);
    if (JS_IsException(r)) return r;
    JSValue cur = js_getp(ctx, rx, ctx->a[A_lastIndex]);
    if (JS_IsException(cur)) return cur;
    if (!js_same_value(cur, prev, false) && js_setp(ctx, rx, ctx->a[A_lastIndex], prev, rx, true) < 0) return JS_EXCEPTION;
    if (JS_IsNull(r)) return js_mknum(-1);
    return js_getp(ctx, r, ctx->a[A_index]);
}

FN(rep_split) {
    JSValue rx = this_val;
    if (!js_is_cls(rx, CL_REGEXP)) return js_throw_type(ctx, "RegExp.prototype[Symbol.split] called on incompatible receiver");
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    uint32_t lim = 0xFFFFFFFFu;
    if (!JS_IsUndefined(A(1))) { bool e = false; double d = js_tonum(ctx, A(1), &e); if (e) return JS_EXCEPTION; lim = js_d2u32(d); }
    JSValue out = js_array_new(ctx, 0);
    if (JS_IsException(out)) return out;
    if (!lim) return out;
    JSRegExpObj *r = OBJ(rx)->u.p;
    JSRegExp *re = r->re;
    int nc = js_re_ncaps(re);
    int32_t *caps = malloc(sizeof(int32_t) * (size_t)nc * 2);
    if (!caps) return js_throw_oom(ctx);
    uint32_t saved = js_re_get_flags(re);
    js_re_set_flags(re, saved | RE_Y);
    JSString *str = js_flat(ctx, s);
    uint32_t size = str ? str->len : 0;
    JSValue res = out;
    bool unicode = (saved & (RE_U | RE_V)) != 0;
    if (!str) { res = JS_EXCEPTION; goto done; }
    if (size == 0) {
        int m = js_re_exec(ctx, re, str->s, 0, 0, caps);
        if (m < 0) { res = JS_EXCEPTION; goto done; }
        if (!m && js_array_push_v(ctx, out, s) < 0) res = JS_EXCEPTION;
        goto done;
    }
    uint32_t p = 0, q = 0;
    while (q < size) {
        str = js_flat(ctx, s);
        int m = js_re_exec(ctx, re, str->s, size, q, caps);
        if (m < 0) { res = JS_EXCEPTION; goto done; }
        if (!m) { q = advance_index(str, q, unicode); continue; }
        uint32_t e = (uint32_t)caps[1] < size ? (uint32_t)caps[1] : size;
        if (e == p) { q = advance_index(str, q, unicode); continue; }
        JSValue part = js_substr(ctx, s, p, q);
        if (JS_IsException(part) || js_array_push_v(ctx, out, part) < 0) { res = JS_EXCEPTION; goto done; }
        if (OBJ(out)->u.a.len == lim) goto done;
        p = e;
        for (int g = 1; g < nc; g++) {
            JSValue c = caps[2 * g] >= 0 ? js_substr(ctx, s, (uint32_t)caps[2 * g], (uint32_t)caps[2 * g + 1]) : JS_UNDEFINED;
            if (JS_IsException(c) || js_array_push_v(ctx, out, c) < 0) { res = JS_EXCEPTION; goto done; }
            if (OBJ(out)->u.a.len == lim) goto done;
        }
        q = p;
    }
    {
        JSValue rest = js_substr(ctx, s, p, size);
        if (JS_IsException(rest) || js_array_push_v(ctx, out, rest) < 0) res = JS_EXCEPTION;
    }
done:
    js_re_set_flags(re, saved);
    free(caps);
    return res;
}

FN(rep_match_all) {
    JSValue rx = this_val;
    if (!JS_IsObject(rx)) return js_throw_type(ctx, "RegExp.prototype[Symbol.matchAll] called on non-object");
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    JSValue fl = js_getp(ctx, rx, ctx->a[A_flags]);
    if (!JS_IsException(fl)) fl = js_tostr(ctx, fl);
    if (JS_IsException(fl)) return fl;
    JSValue src = js_getp(ctx, rx, ctx->a[A_source]);
    if (JS_IsException(src)) return src;
    JSValue copy = js_regexp_create(ctx, src, fl);
    if (JS_IsException(copy)) return copy;
    JSValue li = js_getp(ctx, rx, ctx->a[A_lastIndex]);
    if (JS_IsException(li)) return li;
    if (js_setp(ctx, copy, ctx->a[A_lastIndex], li, copy, true) < 0) return JS_EXCEPTION;
    JSValue it = js_new_iter(ctx, ctx->proto_regexp_iter, copy, ITT_REGEXP, 0);
    if (JS_IsException(it)) return it;
    ((JSIter *)OBJ(it)->u.p)->extra = s;
    return it;
}

FN(re_iter_next) {
    if (!js_is_cls(this_val, CL_ITER)) return js_throw_type(ctx, "next method called on incompatible receiver");
    JSIter *it = OBJ(this_val)->u.p;
    if (it->type != ITT_REGEXP) return js_throw_type(ctx, "next method called on incompatible receiver");
    if (it->done) return js_iter_result(ctx, JS_UNDEFINED, true);
    bool g, u;
    if (read_flags(ctx, it->target, &g, &u) < 0) return JS_EXCEPTION;
    JSValue r = js_regexp_exec_v(ctx, it->target, it->extra);
    if (JS_IsException(r)) return r;
    if (JS_IsNull(r)) { it->done = true; return js_iter_result(ctx, JS_UNDEFINED, true); }
    if (!g) { it->done = true; return js_iter_result(ctx, r, false); }
    JSValue m = js_geti(ctx, r, 0);
    if (!JS_IsException(m)) m = js_tostr(ctx, m);
    if (JS_IsException(m)) return m;
    if (!js_strlen(m) && bump_last_index(ctx, it->target, it->extra, u) < 0) return JS_EXCEPTION;
    return js_iter_result(ctx, r, false);
}

FN(re_species) { return this_val; }

/* ==== setup ============================================================================================= */
static const JSFuncDef strp_fns[] = {
    { "toString", strp_to_string, 0, 0 }, { "valueOf", strp_to_string, 0, 0 }, { "charAt", strp_char_at, 1, 0 },
    { "charCodeAt", strp_char_at, 1, 1 }, { "codePointAt", strp_char_at, 1, 2 }, { "at", strp_char_at, 1, 3 },
    { "concat", strp_concat, 1, 0 }, { "includes", strp_search_str, 1, 0 }, { "startsWith", strp_search_str, 1, 1 },
    { "endsWith", strp_search_str, 1, 2 }, { "indexOf", strp_search_str, 1, 3 }, { "lastIndexOf", strp_search_str, 1, 4 },
    { "slice", strp_slice, 2, 0 }, { "substring", strp_slice, 2, 1 }, { "substr", strp_slice, 2, 2 },
    { "toLowerCase", strp_case, 0, 0 }, { "toUpperCase", strp_case, 0, 1 }, { "toLocaleLowerCase", strp_case, 0, 0 },
    { "toLocaleUpperCase", strp_case, 0, 1 }, { "trim", strp_trim, 0, 0 }, { "trimStart", strp_trim, 0, 1 },
    { "trimEnd", strp_trim, 0, 2 }, { "trimLeft", strp_trim, 0, 1 }, { "trimRight", strp_trim, 0, 2 },
    { "padStart", strp_pad, 2, 0 }, { "padEnd", strp_pad, 2, 1 }, { "repeat", strp_repeat, 1, 0 },
    { "localeCompare", strp_locale_compare, 1, 0 }, { "normalize", strp_normalize, 0, 0 },
    { "isWellFormed", strp_well_formed, 0, 0 }, { "toWellFormed", strp_well_formed, 0, 1 },
    { "split", strp_split, 2, 0 }, { "replace", strp_replace, 2, 0 }, { "replaceAll", strp_replace, 2, 1 },
    { "match", strp_match, 1, 0 }, { "matchAll", strp_match, 1, 1 }, { "search", strp_match, 1, 2 },
    { "@iterator", strp_iter, 0, 0 },
};

void js_init_lib_str(JSContext *ctx) {
    ctx->proto_string = js_obj_alloc(ctx, CL_STRING, ctx->proto_object);
    ctx->proto_string->u.prim = ctx->empty_str;
    JSObject *sc = js_new_ctor(ctx, "String", str_ctor, 1, ctx->proto_string, 0);
    JSValue scv = JS_MKOBJ(sc);
    js_def_native(ctx, scv, "fromCharCode", str_from_char_code, 1, 0);
    js_def_native(ctx, scv, "fromCodePoint", str_from_code_point, 1, 0);
    js_def_native(ctx, scv, "raw", str_raw, 1, 0);
    js_def_funcs(ctx, JS_MKOBJ(ctx->proto_string), strp_fns, JS_COUNTOF(strp_fns));
    ctx->proto_string_iter = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_iterator);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_string_iter), "next", str_iter_next, 0, 0);
    js_add_prop(ctx, ctx->proto_string_iter, ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, "String Iterator"), P_C);
    /* RegExp */
    ctx->proto_regexp = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
    JSObject *rc = js_new_ctor(ctx, "RegExp", re_ctor, 2, ctx->proto_regexp, 0);
    ctx->ctor_regexp = rc;
    js_def_accessor(ctx, JS_MKOBJ(rc), ctx->wk[WK_SPECIES], re_species, NULL, 0, P_C);
    JSValue rp = JS_MKOBJ(ctx->proto_regexp);
    ctx->regexp_exec = js_def_native(ctx, rp, "exec", rep_exec, 1, 0);
    js_def_native(ctx, rp, "test", rep_test, 1, 0);
    js_def_native(ctx, rp, "toString", rep_to_string, 0, 0);
    js_def_native(ctx, rp, "compile", rep_compile, 2, 0);
    js_def_native(ctx, rp, "@match", rep_match, 1, 0);
    js_def_native(ctx, rp, "@matchAll", rep_match_all, 1, 0);
    js_def_native(ctx, rp, "@replace", rep_replace, 2, 0);
    js_def_native(ctx, rp, "@search", rep_search, 1, 0);
    js_def_native(ctx, rp, "@split", rep_split, 2, 0);
    js_def_accessor(ctx, rp, ctx->a[A_source], rep_source, NULL, 0, P_C);
    js_def_accessor(ctx, rp, ctx->a[A_flags], rep_flags, NULL, 0, P_C);
    for (int i = 0; i < JS_COUNTOF(flag_tab); i++) js_def_accessor(ctx, rp, ctx->a[flag_tab[i].atom], rep_flag, NULL, (int)flag_tab[i].bit, P_C);
    ctx->proto_regexp_iter = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_iterator);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_regexp_iter), "next", re_iter_next, 0, 0);
    js_add_prop(ctx, ctx->proto_regexp_iter, ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, "RegExp String Iterator"), P_C);
}
