/*
 * js_lib_json.c - NovaJS JSON.parse / JSON.stringify.
 */
#include "jsi.h"

#define FN(name) static JSValue name(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic)
#define A(i) JS_ARG(i)

/* ==== parse ================================================================================= */
typedef struct JP { JSContext *ctx; const uint16_t *s; uint32_t n, i; int depth; } JP;

static void jws(JP *p) {
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r')) p->i++;
}

static JSValue jerr(JP *p, const char *what) {
    if (p->i >= p->n) return js_throw_syntax(p->ctx, "Unexpected end of JSON input");
    uint32_t c = p->s[p->i];
    if (c >= 32 && c < 127) return js_throw_syntax(p->ctx, "Unexpected token '%c' in JSON at position %u (%s)", (char)c, (unsigned)p->i, what);
    return js_throw_syntax(p->ctx, "Unexpected token in JSON at position %u (%s)", (unsigned)p->i, what);
}

static JSValue jstr(JP *p, bool atom) {
    p->i++;   /* " */
    uint32_t st = p->i;
    bool simple = true;
    while (p->i < p->n && p->s[p->i] != '"') {
        if (p->s[p->i] == '\\') { simple = false; break; }
        if (p->s[p->i] < 0x20) return jerr(p, "control character in string");
        p->i++;
    }
    if (simple) {
        if (p->i >= p->n) return jerr(p, "unterminated string");
        uint32_t e = p->i++;
        return atom ? js_atom16(p->ctx, p->s + st, e - st) : js_str16(p->ctx, p->s + st, e - st);
    }
    JSSB b;
    sb_init(p->ctx, &b);
    sb_put16(&b, p->s + st, p->i - st);
    while (p->i < p->n && p->s[p->i] != '"') {
        uint32_t c = p->s[p->i++];
        if (c < 0x20) { sb_free(&b); p->i--; return jerr(p, "control character in string"); }
        if (c != '\\') { sb_putc(&b, c); continue; }
        if (p->i >= p->n) break;
        c = p->s[p->i++];
        switch (c) {
        case '"': case '\\': case '/': sb_putc(&b, c); break;
        case 'b': sb_putc(&b, 8); break;
        case 'f': sb_putc(&b, 12); break;
        case 'n': sb_putc(&b, 10); break;
        case 'r': sb_putc(&b, 13); break;
        case 't': sb_putc(&b, 9); break;
        case 'u': {
            uint32_t v = 0;
            for (int k = 0; k < 4; k++) {
                if (p->i >= p->n) { sb_free(&b); return jerr(p, "bad unicode escape"); }
                uint32_t h = p->s[p->i++];
                int d = h >= '0' && h <= '9' ? (int)(h - '0') : (h | 0x20) >= 'a' && (h | 0x20) <= 'f' ? (int)((h | 0x20) - 'a' + 10) : -1;
                if (d < 0) { p->i--; sb_free(&b); return jerr(p, "bad unicode escape"); }
                v = v * 16 + (uint32_t)d;
            }
            if (b.len >= b.cap) sb_putc(&b, v); else b.s[b.len++] = (uint16_t)v;
            break;
        }
        default: p->i--; sb_free(&b); return jerr(p, "bad escape");
        }
    }
    if (p->i >= p->n) { sb_free(&b); return jerr(p, "unterminated string"); }
    p->i++;
    JSValue r = sb_finish(&b);
    if (atom && !JS_IsException(r)) r = js_atom(p->ctx, r);
    return r;
}

static JSValue jval(JP *p) {
    jws(p);
    if (p->i >= p->n) return jerr(p, "value");
    if (++p->depth > 1000) return js_throw_range(p->ctx, "JSON nested too deeply");
    uint32_t c = p->s[p->i];
    JSValue r;
    if (c == '{') {
        p->i++;
        r = js_obj_new(p->ctx, CL_OBJECT, p->ctx->proto_object);
        if (JS_IsException(r)) goto out;
        jws(p);
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; goto out; }
        for (;;) {
            jws(p);
            if (p->i >= p->n || p->s[p->i] != '"') { r = jerr(p, "property name"); goto out; }
            JSValue k = jstr(p, true);
            if (JS_IsException(k)) { r = k; goto out; }
            jws(p);
            if (p->i >= p->n || p->s[p->i] != ':') { r = jerr(p, "':'"); goto out; }
            p->i++;
            JSValue v = jval(p);
            if (JS_IsException(v)) { r = v; goto out; }
            if (js_create_data_prop(p->ctx, r, k, v) < 0) { r = JS_EXCEPTION; goto out; }
            jws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == '}') { p->i++; break; }
            r = jerr(p, "',' or '}'");
            goto out;
        }
    } else if (c == '[') {
        p->i++;
        r = js_array_new(p->ctx, 0);
        if (JS_IsException(r)) goto out;
        jws(p);
        if (p->i < p->n && p->s[p->i] == ']') { p->i++; goto out; }
        for (;;) {
            JSValue v = jval(p);
            if (JS_IsException(v)) { r = v; goto out; }
            if (js_array_push_v(p->ctx, r, v) < 0) { r = JS_EXCEPTION; goto out; }
            jws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == ']') { p->i++; break; }
            r = jerr(p, "',' or ']'");
            goto out;
        }
    } else if (c == '"') r = jstr(p, false);
    else if (c == '-' || (c >= '0' && c <= '9')) {
        uint32_t st = p->i;
        if (p->s[p->i] == '-') p->i++;
        if (p->i < p->n && p->s[p->i] == '0') p->i++;
        else if (p->i < p->n && p->s[p->i] >= '1' && p->s[p->i] <= '9') while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
        else { r = jerr(p, "number"); goto out; }
        if (p->i < p->n && p->s[p->i] == '.') {
            p->i++;
            if (!(p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9')) { r = jerr(p, "number"); goto out; }
            while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
        }
        if (p->i < p->n && (p->s[p->i] | 0x20) == 'e') {
            p->i++;
            if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
            if (!(p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9')) { r = jerr(p, "number"); goto out; }
            while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
        }
        uint32_t used;
        r = js_mknum(js_strtod16(p->s + st, p->i - st, &used, false));
    } else {
        static const char *w[3] = { "true", "false", "null" };
        static const JSValue wv[3] = { JS_TRUE, JS_FALSE, JS_NULL };
        r = JS_EXCEPTION;
        for (int k = 0; k < 3; k++) {
            size_t l = strlen(w[k]);
            if (p->i + l <= p->n) {
                bool m = true;
                for (size_t j = 0; j < l; j++) if (p->s[p->i + j] != (uint8_t)w[k][j]) { m = false; break; }
                if (m) { p->i += (uint32_t)l; r = wv[k]; break; }
            }
        }
        if (JS_IsException(r)) r = jerr(p, "value");
    }
out:
    p->depth--;
    return r;
}

static JSValue revive(JSContext *ctx, JSValue holder, JSValue key, JSValue reviver) {
    JSValue v = js_getp(ctx, holder, key);
    if (JS_IsException(v)) return v;
    if (JS_IsObject(v)) {
        int ia = js_is_array_v(ctx, v);
        if (ia < 0) return JS_EXCEPTION;
        if (ia) {
            int64_t n = js_length_of(ctx, v);
            if (n < 0) return JS_EXCEPTION;
            for (int64_t i = 0; i < n; i++) {
                JSValue k = js_atom_from_index(ctx, (uint32_t)i);
                JSValue e = revive(ctx, v, k, reviver);
                if (JS_IsException(e)) return e;
                if (JS_IsUndefined(e)) { if (js_delete_prop(ctx, v, k, false) < 0) return JS_EXCEPTION; }
                else if (js_create_data_prop(ctx, v, k, e) < 0) return JS_EXCEPTION;
            }
        } else {
            JSValue keys = js_own_keys(ctx, v, OK_STRINGS | OK_ENUM);
            if (JS_IsException(keys)) return keys;
            for (uint32_t i = 0; i < OBJ(keys)->u.a.len; i++) {
                JSValue k = OBJ(keys)->u.a.v[i];
                JSValue e = revive(ctx, v, k, reviver);
                if (JS_IsException(e)) return e;
                if (JS_IsUndefined(e)) { if (js_delete_prop(ctx, v, k, false) < 0) return JS_EXCEPTION; }
                else if (js_create_data_prop(ctx, v, k, e) < 0) return JS_EXCEPTION;
            }
        }
    }
    JSValue args[2] = { key, v };
    return js_callv(ctx, reviver, holder, 2, args);
}

static JSValue parse_str(JSContext *ctx, JSValue s, JSValue reviver) {
    JSString *str = js_flat(ctx, s);
    if (!str) return JS_EXCEPTION;
    JP p = { ctx, str->s, str->len, 0, 0 };
    JSValue v = jval(&p);
    if (JS_IsException(v)) return v;
    jws(&p);
    if (p.i < p.n) return jerr(&p, "end");
    if (js_is_callable(reviver)) {
        JSValue root = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
        if (JS_IsException(root)) return root;
        js_create_data_prop(ctx, root, ctx->empty_str, v);
        return revive(ctx, root, ctx->empty_str, reviver);
    }
    return v;
}

FN(json_parse) {
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    return parse_str(ctx, s, A(1));
}

/* ==== stringify =============================================================================== */
typedef struct JS_ { JSContext *ctx; JSSB b; JSValue replacer, plist, gap; JSValue stack[512]; int depth; } JSt;

JSValue js_json_quote(JSContext *ctx, JSValue s) {
    JSSB b;
    sb_init(ctx, &b);
    JSString *str = js_flat(ctx, s);
    if (!str) return JS_EXCEPTION;
    static const char hex[] = "0123456789abcdef";
    sb_putc(&b, '"');
    for (uint32_t i = 0; i < str->len; i++) {
        uint32_t c = str->s[i];
        switch (c) {
        case '"': sb_puts(&b, "\\\""); continue;
        case '\\': sb_puts(&b, "\\\\"); continue;
        case '\b': sb_puts(&b, "\\b"); continue;
        case '\f': sb_puts(&b, "\\f"); continue;
        case '\n': sb_puts(&b, "\\n"); continue;
        case '\r': sb_puts(&b, "\\r"); continue;
        case '\t': sb_puts(&b, "\\t"); continue;
        default: break;
        }
        bool lone = false;
        if (c >= 0xD800 && c < 0xDC00) { if (i + 1 < str->len && str->s[i + 1] >= 0xDC00 && str->s[i + 1] < 0xE000) { sb_putc(&b, c); sb_putc(&b, str->s[++i]); continue; } lone = true; }
        else if (c >= 0xDC00 && c < 0xE000) lone = true;
        if (c < 0x20 || lone) {
            sb_puts(&b, "\\u");
            for (int k = 12; k >= 0; k -= 4) sb_putc(&b, (uint8_t)hex[(c >> k) & 15]);
            continue;
        }
        sb_putc(&b, c);
    }
    sb_putc(&b, '"');
    return sb_finish(&b);
}

static int ser(JSt *st, JSValue holder, JSValue key, JSValue v);

static void indent(JSt *st) {
    if (!JS_IsString(st->gap) || !js_strlen(st->gap)) return;
    sb_putc(&st->b, '\n');
    for (int i = 0; i < st->depth; i++) sb_putv(&st->b, st->gap);
}

/* 1 written, 0 undefined (nothing written), -1 exception */
static int ser(JSt *st, JSValue holder, JSValue key, JSValue v) {
    JSContext *ctx = st->ctx;
    if (JS_IsObject(v) || JS_TAGOF(v) == JS_TAG_BIGINT) {
        JSValue tj = js_getp(ctx, v, ctx->a[A_toJSON]);
        if (JS_IsException(tj)) return -1;
        if (js_is_callable(tj)) { v = js_callv(ctx, tj, v, 1, &key); if (JS_IsException(v)) return -1; }
    }
    if (js_is_callable(st->replacer)) {
        JSValue args[2] = { key, v };
        v = js_callv(ctx, st->replacer, holder, 2, args);
        if (JS_IsException(v)) return -1;
    }
    if (JS_IsObject(v)) {
        int c = OBJ(v)->h.cls;
        if (c == CL_NUMBER) { bool e = false; double d = js_tonum(ctx, v, &e); if (e) return -1; v = js_mknum(d); }
        else if (c == CL_STRING) { v = js_tostr(ctx, v); if (JS_IsException(v)) return -1; }
        else if (c == CL_BOOLEAN) v = OBJ(v)->u.prim;
    }
    if (JS_IsNull(v)) { sb_puts(&st->b, "null"); return 1; }
    if (v == JS_TRUE) { sb_puts(&st->b, "true"); return 1; }
    if (v == JS_FALSE) { sb_puts(&st->b, "false"); return 1; }
    if (JS_IsString(v)) { JSValue q = js_json_quote(ctx, v); if (JS_IsException(q)) return -1; sb_putv(&st->b, q); return 1; }
    if (JS_IsNumber(v)) {
        double d = js_num(v);
        if (!isfinite(d)) { sb_puts(&st->b, "null"); return 1; }
        char buf[40];
        js_dtoa(d, buf);
        sb_puts(&st->b, buf);
        return 1;
    }
    if (JS_TAGOF(v) == JS_TAG_BIGINT) { js_throw_type(ctx, "Do not know how to serialize a BigInt"); return -1; }
    if (!JS_IsObject(v) || js_is_callable(v)) return 0;
    for (int i = 0; i < st->depth; i++) if (st->stack[i] == v) { js_throw_type(ctx, "Converting circular structure to JSON"); return -1; }
    if (st->depth >= 512) { js_throw_range(ctx, "Maximum call stack size exceeded"); return -1; }
    st->stack[st->depth++] = v;
    int ia = js_is_array_v(ctx, v);
    if (ia < 0) return -1;
    if (ia) {
        int64_t n = js_length_of(ctx, v);
        if (n < 0) return -1;
        sb_putc(&st->b, '[');
        for (int64_t i = 0; i < n; i++) {
            if (i) sb_putc(&st->b, ',');
            indent(st);
            JSValue e = js_geti(ctx, v, (uint32_t)i);
            if (JS_IsException(e)) return -1;
            int r = ser(st, v, js_atom_from_index(ctx, (uint32_t)i), e);
            if (r < 0) return -1;
            if (!r) sb_puts(&st->b, "null");
        }
        st->depth--;
        if (n) indent(st);
        sb_putc(&st->b, ']');
        return 1;
    }
    JSValue keys = JS_IsObject(st->plist) ? st->plist : js_own_keys(ctx, v, OK_STRINGS | OK_ENUM);
    if (JS_IsException(keys)) return -1;
    sb_putc(&st->b, '{');
    bool any = false;
    uint32_t nk = OBJ(keys)->u.a.len;
    for (uint32_t i = 0; i < nk; i++) {
        JSValue k = OBJ(keys)->u.a.v[i];
        JSValue e = js_getp(ctx, v, k);
        if (JS_IsException(e)) return -1;
        uint32_t mark = st->b.len;
        if (any) sb_putc(&st->b, ',');
        indent(st);
        JSValue q = js_json_quote(ctx, k);
        if (JS_IsException(q)) return -1;
        sb_putv(&st->b, q);
        sb_putc(&st->b, ':');
        if (JS_IsString(st->gap) && js_strlen(st->gap)) sb_putc(&st->b, ' ');
        int r = ser(st, v, k, e);
        if (r < 0) return -1;
        if (!r) { st->b.len = mark; continue; }
        any = true;
        keys = JS_IsObject(st->plist) ? st->plist : keys;
    }
    st->depth--;
    if (any) indent(st);
    sb_putc(&st->b, '}');
    return 1;
}

static JSValue stringify(JSContext *ctx, JSValue v, JSValue replacer, JSValue space) {
    JSt *st = calloc(1, sizeof *st);
    if (!st) return js_throw_oom(ctx);
    st->ctx = ctx;
    st->replacer = JS_UNDEFINED;
    st->plist = JS_UNDEFINED;
    st->gap = JS_UNDEFINED;
    sb_init(ctx, &st->b);
    JSValue r = JS_EXCEPTION;
    volatile JSValue keep_list = JS_UNDEFINED, keep_gap = JS_UNDEFINED;   /* (st is not scanned by the GC) */
    if (js_is_callable(replacer)) st->replacer = replacer;
    else if (js_is_array_v(ctx, replacer) > 0) {
        JSValue list = js_array_new(ctx, 0);
        if (JS_IsException(list)) goto out;
        int64_t n = js_length_of(ctx, replacer);
        for (int64_t i = 0; i < n; i++) {
            JSValue e = js_geti(ctx, replacer, (uint32_t)i);
            if (JS_IsException(e)) goto out;
            JSValue item = JS_UNDEFINED;
            if (JS_IsString(e)) item = e;
            else if (JS_IsNumber(e) || js_is_cls(e, CL_NUMBER) || js_is_cls(e, CL_STRING)) item = js_tostr(ctx, e);
            if (JS_IsException(item)) goto out;
            if (JS_IsUndefined(item)) continue;
            item = js_atom(ctx, item);
            bool dup = false;
            for (uint32_t k = 0; k < OBJ(list)->u.a.len; k++) if (OBJ(list)->u.a.v[k] == item) dup = true;
            if (!dup) js_array_push_v(ctx, list, item);
        }
        st->plist = list;
        keep_list = list;
    }
    if (js_is_cls(space, CL_NUMBER)) { bool e = false; space = js_mknum(js_tonum(ctx, space, &e)); }
    else if (js_is_cls(space, CL_STRING)) space = js_tostr(ctx, space);
    if (JS_IsNumber(space)) {
        double d = js_num(space);
        int n = d < 1 ? 0 : d > 10 ? 10 : (int)d;
        st->gap = js_str8(ctx, "          ", (uint32_t)n);
    } else if (JS_IsString(space)) st->gap = js_substr(ctx, space, 0, 10);
    keep_gap = st->gap;
    JSValue holder = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
    if (JS_IsException(holder)) goto out;
    js_create_data_prop(ctx, holder, ctx->empty_str, v);
    int k = ser(st, holder, ctx->empty_str, v);
    if (k < 0) goto out;
    if (k == 0) { sb_free(&st->b); r = JS_UNDEFINED; goto done; }
    r = sb_finish(&st->b);
    goto done;
out:
    sb_free(&st->b);
done:
    (void)keep_list;
    (void)keep_gap;
    free(st);
    return r;
}

FN(json_stringify) { return stringify(ctx, A(0), A(1), A(2)); }

JSValue js_json_parse(JSContext *ctx, const char *utf8, size_t len) {
    JS_GC_ENTER(ctx);
    JSValue s = js_str_utf8(ctx, utf8, len);
    JSValue r = JS_IsException(s) ? s : parse_str(ctx, s, JS_UNDEFINED);
    JS_GC_LEAVE(ctx);
    return r;
}

JSValue js_json_stringify(JSContext *ctx, JSValue v) {
    JS_GC_ENTER(ctx);
    JSValue r = stringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
    JS_GC_LEAVE(ctx);
    return r;
}

void js_init_lib_json(JSContext *ctx) {
    JSValue j = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
    js_def_native(ctx, j, "parse", json_parse, 2, 0);
    js_def_native(ctx, j, "stringify", json_stringify, 3, 0);
    js_add_prop(ctx, OBJ(j), ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, "JSON"), P_C);
    js_add_prop(ctx, OBJ(ctx->global), js_atomc(ctx, "JSON"), j, P_W | P_C);
}
