/*
 * js_lib_misc.c - NovaJS built-ins that are not in the language core but
 * that pages expect everywhere: queueMicrotask, structuredClone, atob / btoa,
 * TextEncoder / TextDecoder, URL / URLSearchParams and a small (en-US) Intl.
 */
#include "jsi.h"

#define FN(name) static JSValue name(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic)
#define A(i) JS_ARG(i)

FN(g_queue_microtask) {
    if (!js_is_callable(A(0))) return js_throw_type(ctx, "queueMicrotask: callback is not a function");
    js_enqueue(ctx, A(0), 0, NULL);
    return JS_UNDEFINED;
}

/* ==== base64 ================================================================================= */
static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

FN(g_btoa) {
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    JSString *str = js_flat(ctx, s);
    if (!str) return JS_EXCEPTION;
    JSSB b;
    sb_init(ctx, &b);
    for (uint32_t i = 0; i < str->len; i += 3) {
        uint32_t v = 0;
        int n = 0;
        for (int k = 0; k < 3; k++) {
            v <<= 8;
            if (i + (uint32_t)k < str->len) {
                uint32_t c = str->s[i + (uint32_t)k];
                if (c > 255) { sb_free(&b); return js_throw_error(ctx, JS_ERROR, "InvalidCharacterError: The string to be encoded contains characters outside of the Latin1 range."); }
                v |= c;
                n++;
            }
        }
        for (int k = 0; k < 4; k++) sb_putc(&b, k <= n ? (uint8_t)b64[(v >> (18 - 6 * k)) & 63] : '=');
    }
    return sb_finish(&b);
}

static int b64v(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a' + 26);
    if (c >= '0' && c <= '9') return (int)(c - '0' + 52);
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

FN(g_atob) {
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    JSString *str = js_flat(ctx, s);
    if (!str) return JS_EXCEPTION;
    JSSB b;
    sb_init(ctx, &b);
    uint32_t acc = 0;
    int bits = 0;
    for (uint32_t i = 0; i < str->len; i++) {
        uint32_t c = str->s[i];
        if (c == '=' || js_is_space(c)) continue;
        int v = b64v(c);
        if (v < 0) { sb_free(&b); return js_throw_error(ctx, JS_ERROR, "InvalidCharacterError: The string to be decoded is not correctly encoded."); }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; sb_putc(&b, (acc >> bits) & 0xFF); }
    }
    return sb_finish(&b);
}

/* ==== TextEncoder / TextDecoder ================================================================= */
FN(te_ctor) {
    if (JS_IsUndefined(js_new_target(ctx))) return js_throw_type(ctx, "Constructor TextEncoder requires 'new'");
    JSValue o = js_obj_new(ctx, CL_OBJECT, ctx->proto_textenc);
    return o;
}

FN(te_encoding) { return js_strc(ctx, "utf-8"); }

FN(te_encode) {
    JSValue s = JS_IsUndefined(A(0)) ? ctx->empty_str : js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    size_t n;
    char *u = js_str_to_utf8(ctx, s, &n);
    if (!u) return JS_EXCEPTION;
    JSValue r = js_new_uint8array(ctx, (const uint8_t *)u, n);
    free(u);
    return r;
}

FN(te_encode_into) {
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    size_t cap;
    uint8_t *dst = js_get_bytes(ctx, A(1), &cap);
    if (!dst && !js_is_cls(A(1), CL_TYPEDARRAY)) return js_throw_type(ctx, "encodeInto: destination must be a Uint8Array");
    JSString *str = js_flat(ctx, s);
    if (!str) return JS_EXCEPTION;
    uint32_t read = 0;
    size_t written = 0;
    for (uint32_t i = 0; i < str->len; i++) {
        uint32_t c = str->s[i];
        uint32_t units = 1;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < str->len && str->s[i + 1] >= 0xDC00 && str->s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (str->s[i + 1] - 0xDC00);
            units = 2;
        } else if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
        char u[4];
        int n = js_utf8_encode(c, u);
        if (written + (size_t)n > cap) break;
        memcpy(dst + written, u, (size_t)n);
        written += (size_t)n;
        read += units;
        i += units - 1;
    }
    JSValue o = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
    if (JS_IsException(o)) return o;
    js_add_prop(ctx, OBJ(o), js_atomc(ctx, "read"), js_mknum(read), P_W | P_E | P_C);
    js_add_prop(ctx, OBJ(o), js_atomc(ctx, "written"), js_mknum((double)written), P_W | P_E | P_C);
    return o;
}

/* decoder state: hidden properties on the object */
FN(td_ctor) {
    if (JS_IsUndefined(js_new_target(ctx))) return js_throw_type(ctx, "Constructor TextDecoder requires 'new'");
    JSValue enc = js_strc(ctx, "utf-8");
    if (!JS_IsUndefined(A(0))) {
        JSValue l = js_tostr(ctx, A(0));
        if (JS_IsException(l)) return l;
        char t[32];
        js_str_to_ascii(ctx, l, t, sizeof t);
        for (char *p = t; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        if (!strcmp(t, "utf-8") || !strcmp(t, "utf8") || !strcmp(t, "unicode-1-1-utf-8")) enc = js_strc(ctx, "utf-8");
        else if (!strcmp(t, "utf-16le") || !strcmp(t, "utf-16")) enc = js_strc(ctx, "utf-16le");
        else if (!strcmp(t, "latin1") || !strcmp(t, "iso-8859-1") || !strcmp(t, "windows-1252") || !strcmp(t, "ascii") || !strcmp(t, "us-ascii"))
            enc = js_strc(ctx, "windows-1252");
        else return js_throw_range(ctx, "The encoding label provided ('%s') is invalid.", t);
    }
    JSValue o = js_obj_new(ctx, CL_OBJECT, ctx->proto_textdec);
    if (JS_IsException(o)) return o;
    js_add_prop(ctx, OBJ(o), ctx->td_enc_key, enc, 0);
    bool fatal = false, ignore_bom = false;
    if (JS_IsObject(A(1))) {
        JSValue f = js_get(ctx, A(1), "fatal");
        if (JS_IsException(f)) return f;
        fatal = js_truthy(f);
        f = js_get(ctx, A(1), "ignoreBOM");
        if (JS_IsException(f)) return f;
        ignore_bom = js_truthy(f);
    }
    js_add_prop(ctx, OBJ(o), js_atomc(ctx, "fatal"), JS_BOOL(fatal), P_E);
    js_add_prop(ctx, OBJ(o), js_atomc(ctx, "ignoreBOM"), JS_BOOL(ignore_bom), P_E);
    return o;
}

FN(td_encoding) {
    if (!JS_IsObject(this_val)) return js_throw_type(ctx, "not a TextDecoder");
    JSProp *p = js_find_prop(ctx, OBJ(this_val), ctx->td_enc_key);
    return p ? p->val : js_strc(ctx, "utf-8");
}

FN(td_decode) {
    if (!JS_IsObject(this_val)) return js_throw_type(ctx, "not a TextDecoder");
    JSProp *ep = js_find_prop(ctx, OBJ(this_val), ctx->td_enc_key);
    if (!ep) return js_throw_type(ctx, "not a TextDecoder");
    size_t n = 0;
    uint8_t *d = JS_IsUndefined(A(0)) ? NULL : js_get_bytes(ctx, A(0), &n);
    if (!d && !JS_IsUndefined(A(0))) return js_throw_type(ctx, "The provided value is not of type '(ArrayBuffer or ArrayBufferView)'");
    if (js_str_eq_ascii(ctx, ep->val, "utf-16le")) {
        JSSB b;
        sb_init(ctx, &b);
        for (size_t i = 0; i + 1 < n; i += 2) sb_putc(&b, (uint32_t)(d[i] | (d[i + 1] << 8)));
        return sb_finish(&b);
    }
    if (js_str_eq_ascii(ctx, ep->val, "windows-1252")) return js_str8(ctx, (const char *)d, (uint32_t)n);
    size_t st = 0;
    JSValue ib = js_get(ctx, this_val, "ignoreBOM");
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF && !js_truthy(ib)) st = 3;
    JSValue fat = js_get(ctx, this_val, "fatal");
    if (js_truthy(fat)) {
        for (size_t i = st; i < n;) {
            uint32_t cp;
            uint32_t l = js_utf8_decode(d + i, n - i, &cp);
            if (cp == 0xFFFD && !(n - i >= 3 && d[i] == 0xEF && d[i + 1] == 0xBF && d[i + 2] == 0xBD)) return js_throw_type(ctx, "The encoded data was not valid.");
            i += l;
        }
    }
    return js_str_utf8(ctx, (const char *)d + st, n - st);
}

/* ==== structuredClone ================================================================================ */
static JSValue clone_rec(JSContext *ctx, JSValue v, JSValue memo, int depth) {
    if (!JS_IsObject(v)) {
        if (JS_IsSymbol(v)) return js_throw_error(ctx, JS_ERROR, "DataCloneError: Symbol cannot be cloned");
        return v;
    }
    if (depth > 500) return js_throw_range(ctx, "structuredClone: too deep");
    JSValue seen = js_invoke(ctx, memo, ctx->a[A_get], 1, &v);
    if (JS_IsException(seen) || !JS_IsUndefined(seen)) return seen;
    JSObject *o = OBJ(v);
    JSValue r;
    switch (o->h.cls) {
    case CL_ARRAY: r = js_array_new(ctx, 0); break;
    case CL_DATE: { JSValue t = js_mknum(js_num(o->u.prim)); r = js_construct_internal(ctx, js_get(ctx, ctx->global, "Date"), 1, &t, js_get(ctx, ctx->global, "Date")); break; }
    case CL_REGEXP: { JSRegExpObj *re = o->u.p; JSValue fl = js_getp(ctx, v, ctx->a[A_flags]); if (JS_IsException(fl)) return fl; r = js_regexp_create(ctx, re->source, fl); break; }
    case CL_BOOLEAN: case CL_NUMBER: case CL_STRING: r = js_toobj(ctx, o->u.prim); break;
    case CL_MAP: case CL_SET: { JSValue c = js_get(ctx, ctx->global, o->h.cls == CL_MAP ? "Map" : "Set"); r = js_construct_internal(ctx, c, 0, NULL, c); break; }
    case CL_ARRAYBUFFER: { size_t n; uint8_t *d = js_get_bytes(ctx, v, &n); r = js_new_arraybuffer(ctx, d, n); break; }
    case CL_TYPEDARRAY: {
        JSValue args[1] = { v };
        JSObject *c = ctx->ctor_typed[((JSTypedArray *)o->u.p)->type];
        r = c ? js_construct_internal(ctx, JS_MKOBJ(c), 1, args, JS_MKOBJ(c)) : JS_EXCEPTION;
        break;
    }
    case CL_ERROR: {
        JSValue m = js_getp(ctx, v, ctx->a[A_message]);
        if (JS_IsException(m)) return m;
        r = js_make_error(ctx, JS_ERROR, JS_IsString(m) ? m : ctx->empty_str);
        if (!JS_IsException(r)) {
            JSValue nm = js_getp(ctx, v, ctx->a[A_name]);
            if (JS_IsString(nm)) js_define_value(ctx, r, ctx->a[A_name], nm, P_W | P_C);
        }
        break;
    }
    case CL_OBJECT: case CL_ARGUMENTS: r = js_obj_new(ctx, CL_OBJECT, ctx->proto_object); break;
    default:
        return js_throw_error(ctx, JS_ERROR, "DataCloneError: %s could not be cloned.", js_is_callable(v) ? "function" : "object");
    }
    if (JS_IsException(r)) return r;
    JSValue pair[2] = { v, r };
    if (JS_IsException(js_invoke(ctx, memo, ctx->a[A_set], 2, pair))) return JS_EXCEPTION;
    if (o->h.cls == CL_MAP || o->h.cls == CL_SET) {
        JSMap *m = o->u.p;
        for (uint32_t i = 0; i < m->n; i++) {
            m = OBJ(v)->u.p;
            if (i >= m->n || m->e[i].key == JS_HOLE) continue;
            JSValue k = clone_rec(ctx, m->e[i].key, memo, depth + 1);
            if (JS_IsException(k)) return k;
            m = OBJ(v)->u.p;
            JSValue x = o->h.cls == CL_MAP ? clone_rec(ctx, m->e[i].val, memo, depth + 1) : k;
            if (JS_IsException(x)) return x;
            JSValue kv[2] = { k, x };
            if (JS_IsException(js_invoke(ctx, r, ctx->a[o->h.cls == CL_MAP ? A_set : A_add], o->h.cls == CL_MAP ? 2 : 1, kv))) return JS_EXCEPTION;
        }
        return r;
    }
    if (o->h.cls != CL_OBJECT && o->h.cls != CL_ARRAY && o->h.cls != CL_ARGUMENTS) return r;
    JSValue keys = js_own_keys(ctx, v, OK_STRINGS | OK_ENUM);
    if (JS_IsException(keys)) return keys;
    for (uint32_t i = 0; i < OBJ(keys)->u.a.len; i++) {
        JSValue k = OBJ(keys)->u.a.v[i];
        JSValue x = js_getp(ctx, v, k);
        if (JS_IsException(x)) return x;
        x = clone_rec(ctx, x, memo, depth + 1);
        if (JS_IsException(x) || js_create_data_prop(ctx, r, k, x) < 0) return JS_EXCEPTION;
    }
    return r;
}

FN(g_structured_clone) {
    JSValue mc = js_get(ctx, ctx->global, "Map");
    if (JS_IsException(mc)) return mc;
    JSValue memo = js_construct_internal(ctx, mc, 0, NULL, mc);
    if (JS_IsException(memo)) return memo;
    return clone_rec(ctx, A(0), memo, 0);
}

/* ==== URL ============================================================================================== */
typedef struct UrlParts {
    char scheme[24];
    char user[128], pass[128];
    char host[256];
    char port[8];
    char *path;        /* malloc'd */
    char *query;       /* malloc'd, without '?', NULL = none */
    char *frag;        /* malloc'd, without '#', NULL = none */
    bool special, opaque;
} UrlParts;

static void up_free(UrlParts *u) { free(u->path); free(u->query); free(u->frag); u->path = u->query = u->frag = NULL; }

static char *dupn(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (p) { memcpy(p, s, n); p[n] = 0; }
    return p;
}

static int default_port(const char *scheme) {
    if (!strcmp(scheme, "http") || !strcmp(scheme, "ws")) return 80;
    if (!strcmp(scheme, "https") || !strcmp(scheme, "wss")) return 443;
    if (!strcmp(scheme, "ftp")) return 21;
    return -1;
}

static bool is_special(const char *s) { return default_port(s) >= 0 || !strcmp(s, "file"); }

/* percent-encode bytes outside the allowed set */
static char *pct_encode(const char *s, size_t n, const char *keep) {
    static const char hex[] = "0123456789ABCDEF";
    char *o = malloc(n * 3 + 1);
    if (!o) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c > 0x20 && c < 0x7F && c != '"' && c != '<' && c != '>' && c != '`' && (!keep || c != '#') &&
            !(keep && strchr(keep, c)))
            o[k++] = (char)c;
        else if (c == '%' ) o[k++] = '%';
        else { o[k++] = '%'; o[k++] = hex[c >> 4]; o[k++] = hex[c & 15]; }
    }
    o[k] = 0;
    return o;
}

static void remove_dots(char *path) {
    /* in place: segments separated by '/', the path starts with '/' */
    char *out = path;
    char *in = path;
    size_t olen = 0;
    while (*in) {
        if (*in == '/') {
            char *seg = in + 1;
            char *e = strchr(seg, '/');
            size_t sl = e ? (size_t)(e - seg) : strlen(seg);
            bool last = !e;
            if ((sl == 1 && seg[0] == '.') || (sl == 3 && !strncasecmp(seg, "%2e", 3))) {
                in = seg + sl;
                if (last) { out[olen++] = '/'; }
                continue;
            }
            if ((sl == 2 && seg[0] == '.' && seg[1] == '.') || (sl == 4 && (!strncasecmp(seg, ".%2e", 4) || !strncasecmp(seg, "%2e.", 4))) ||
                (sl == 6 && !strncasecmp(seg, "%2e%2e", 6))) {
                while (olen > 0 && out[olen - 1] != '/') olen--;
                if (olen > 0) olen--;
                in = seg + sl;
                if (last) out[olen++] = '/';
                continue;
            }
            out[olen++] = '/';
            memmove(out + olen, seg, sl);
            olen += sl;
            in = seg + sl;
        } else in++;
    }
    if (!olen) out[olen++] = '/';
    out[olen] = 0;
}

/* parse (relative to base when given); false = invalid */
static bool url_parse_parts(const char *in, const UrlParts *base, UrlParts *u) {
    memset(u, 0, sizeof *u);
    while (*in && (unsigned char)*in <= 0x20) in++;
    size_t n = strlen(in);
    while (n && (unsigned char)in[n - 1] <= 0x20) n--;
    char *buf = dupn(in, n);
    if (!buf) return false;
    /* tabs / newlines are removed */
    size_t w = 0;
    for (size_t i = 0; buf[i]; i++) if (buf[i] != '\t' && buf[i] != '\n' && buf[i] != '\r') buf[w++] = buf[i];
    buf[w] = 0;
    const char *p = buf;
    /* scheme */
    size_t sl = 0;
    if (((*p | 0x20) >= 'a' && (*p | 0x20) <= 'z')) {
        while (p[sl] && (isalnum((unsigned char)p[sl]) || p[sl] == '+' || p[sl] == '-' || p[sl] == '.')) sl++;
        if (p[sl] != ':') sl = 0;
    }
    bool ok = true;
    const char *rest;
    if (sl && sl < sizeof u->scheme) {
        for (size_t i = 0; i < sl; i++) u->scheme[i] = (char)tolower((unsigned char)p[i]);
        u->scheme[sl] = 0;
        rest = p + sl + 1;
        u->special = is_special(u->scheme);
        if (base && u->special && !strcmp(base->scheme, u->scheme) && rest[0] != '/' && rest[0] != '\\') {
            /* "http:foo" relative to an http base */
            goto relative;
        }
        if (!u->special) {
            if (rest[0] == '/' && rest[1] == '/') goto authority;
            /* opaque path */
            u->opaque = rest[0] != '/';
            const char *q = strpbrk(rest, "?#");
            size_t pl = q ? (size_t)(q - rest) : strlen(rest);
            u->path = dupn(rest, pl);
            rest += pl;
            goto query;
        }
        while (*rest == '/' || *rest == '\\') rest++;
        goto host;
    }
    if (!base) { ok = false; goto done; }
relative:
    strcpy(u->scheme, base->scheme);
    u->special = base->special;
    rest = sl ? p + sl + 1 : p;
    if ((rest[0] == '/' || (u->special && rest[0] == '\\')) && (rest[1] == '/' || (u->special && rest[1] == '\\'))) {
        rest += 2;
        goto host;
    }
    strcpy(u->user, base->user);
    strcpy(u->pass, base->pass);
    strcpy(u->host, base->host);
    strcpy(u->port, base->port);
    if (base->opaque) {
        if (rest[0] != '#') { ok = false; goto done; }
        u->opaque = true;
        u->path = strdup(base->path ? base->path : "");
        u->query = base->query ? strdup(base->query) : NULL;
        u->frag = dupn(rest + 1, strlen(rest + 1));
        goto done;
    }
    if (!rest[0]) {
        u->path = strdup(base->path ? base->path : "/");
        u->query = base->query ? strdup(base->query) : NULL;
        goto done;
    }
    if (rest[0] == '?') { u->path = strdup(base->path ? base->path : "/"); goto query; }
    if (rest[0] == '#') {
        u->path = strdup(base->path ? base->path : "/");
        u->query = base->query ? strdup(base->query) : NULL;
        goto query;
    }
    {
        const char *q = strpbrk(rest, "?#");
        size_t pl = q ? (size_t)(q - rest) : strlen(rest);
        char *seg = dupn(rest, pl);
        if (!seg) { ok = false; goto done; }
        for (char *c = seg; u->special && *c; c++) if (*c == '\\') *c = '/';
        if (seg[0] == '/') u->path = seg;
        else {
            const char *bp = base->path ? base->path : "/";
            const char *last = strrchr(bp, '/');
            size_t dl = last ? (size_t)(last - bp) + 1 : 0;
            u->path = malloc(dl + pl + 2);
            if (!u->path) { free(seg); ok = false; goto done; }
            if (!dl) { u->path[0] = '/'; dl = 1; memcpy(u->path + 1, seg, pl); }
            else { memcpy(u->path, bp, dl); memcpy(u->path + dl, seg, pl); }
            u->path[dl + pl] = 0;
            free(seg);
        }
        rest += pl;
        goto path_done;
    }
authority:
    rest += 2;
host:
    {
        const char *e = rest;
        while (*e && *e != '/' && *e != '?' && *e != '#' && !(u->special && *e == '\\')) e++;
        const char *at = NULL;
        for (const char *c = rest; c < e; c++) if (*c == '@') at = c;
        const char *hs = rest;
        if (at) {
            const char *colon = memchr(rest, ':', (size_t)(at - rest));
            size_t ul = colon ? (size_t)(colon - rest) : (size_t)(at - rest);
            if (ul >= sizeof u->user) ul = sizeof u->user - 1;
            memcpy(u->user, rest, ul);
            u->user[ul] = 0;
            if (colon) {
                size_t pl = (size_t)(at - colon - 1);
                if (pl >= sizeof u->pass) pl = sizeof u->pass - 1;
                memcpy(u->pass, colon + 1, pl);
                u->pass[pl] = 0;
            }
            hs = at + 1;
        }
        const char *colon = NULL;
        bool bracket = false;
        for (const char *c = hs; c < e; c++) { if (*c == '[') bracket = true; else if (*c == ']') bracket = false; else if (*c == ':' && !bracket) colon = c; }
        const char *he = colon ? colon : e;
        size_t hl = (size_t)(he - hs);
        if (hl >= sizeof u->host) { ok = false; goto done; }
        for (size_t i = 0; i < hl; i++) u->host[i] = (char)tolower((unsigned char)hs[i]);
        u->host[hl] = 0;
        if (u->special && strcmp(u->scheme, "file") && !hl) { ok = false; goto done; }
        for (size_t i = 0; i < hl; i++) if (strchr(" #%/:<>?@[\\]^|", u->host[i]) && !(u->host[0] == '[')) { ok = false; goto done; }
        if (colon) {
            size_t pl = (size_t)(e - colon - 1);
            if (pl > 5) { ok = false; goto done; }
            long pv = 0;
            for (size_t i = 0; i < pl; i++) { if (!isdigit((unsigned char)colon[1 + i])) { ok = false; goto done; } pv = pv * 10 + (colon[1 + i] - '0'); }
            if (pv > 65535) { ok = false; goto done; }
            if (pl && pv != default_port(u->scheme)) snprintf(u->port, sizeof u->port, "%u", (unsigned)(pv & 0xFFFF));
        }
        rest = e;
        const char *q = strpbrk(rest, "?#");
        size_t pl = q ? (size_t)(q - rest) : strlen(rest);
        u->path = pl ? dupn(rest, pl) : strdup(u->special ? "/" : "");
        if (u->path && u->special) for (char *c = u->path; *c; c++) if (*c == '\\') *c = '/';
        rest += pl;
    }
path_done:
    if (u->path && u->path[0] == '/') remove_dots(u->path);
query:
    if (!u->path) u->path = strdup("");
    if (*rest == '?') {
        const char *h = strchr(rest, '#');
        size_t ql = h ? (size_t)(h - rest - 1) : strlen(rest + 1);
        free(u->query);
        u->query = pct_encode(rest + 1, ql, NULL);
        rest += 1 + ql;
    }
    if (*rest == '#') { free(u->frag); u->frag = pct_encode(rest + 1, strlen(rest + 1), "#"); }
    if (u->path && !u->opaque) {
        char *enc = pct_encode(u->path, strlen(u->path), "?");
        if (enc) { free(u->path); u->path = enc; }
    }
done:
    free(buf);
    if (!ok) up_free(u);
    return ok;
}

static char *up_href(const UrlParts *u) {
    size_t n = strlen(u->scheme) + strlen(u->user) + strlen(u->pass) + strlen(u->host) + strlen(u->port) +
               (u->path ? strlen(u->path) : 0) + (u->query ? strlen(u->query) : 0) + (u->frag ? strlen(u->frag) : 0) + 16;
    char *o = malloc(n);
    if (!o) return NULL;
    size_t k = (size_t)snprintf(o, n, "%s:", u->scheme);
    if (!u->opaque && (u->host[0] || u->special)) {
        k += (size_t)snprintf(o + k, n - k, "//");
        if (u->user[0] || u->pass[0]) k += (size_t)snprintf(o + k, n - k, "%s%s%s@", u->user, u->pass[0] ? ":" : "", u->pass);
        k += (size_t)snprintf(o + k, n - k, "%s%s%s", u->host, u->port[0] ? ":" : "", u->port);
    }
    k += (size_t)snprintf(o + k, n - k, "%s", u->path ? u->path : "");
    if (u->query) k += (size_t)snprintf(o + k, n - k, "?%s", u->query);
    if (u->frag) snprintf(o + k, n - k, "#%s", u->frag);
    return o;
}

typedef struct UrlObj { UrlParts u; JSValue params; } UrlObj;

static void url_finalize(JSContext *ctx, void *op) { UrlObj *o = op; up_free(&o->u); free(o); (void)ctx; }
static void url_mark(JSContext *ctx, void *op) { UrlObj *o = op; js_mark(ctx, o->params); }

static char *arg_utf8(JSContext *ctx, JSValue v) {
    JSValue s = js_tostr(ctx, v);
    if (JS_IsException(s)) return NULL;
    return js_str_to_utf8(ctx, s, NULL);
}

static JSValue params_from_query(JSContext *ctx, const char *q, JSValue url);

FN(url_ctor) {
    if (JS_IsUndefined(js_new_target(ctx))) return js_throw_type(ctx, "Constructor URL requires 'new'");
    char *in = arg_utf8(ctx, A(0));
    if (!in) return JS_EXCEPTION;
    UrlParts base, u;
    bool have_base = false;
    if (!JS_IsUndefined(A(1))) {
        char *b = arg_utf8(ctx, A(1));
        if (!b) { free(in); return JS_EXCEPTION; }
        bool ok = url_parse_parts(b, NULL, &base);
        free(b);
        if (!ok) { free(in); return js_throw_type(ctx, "Failed to construct 'URL': Invalid base URL"); }
        have_base = true;
    }
    bool ok = url_parse_parts(in, have_base ? &base : NULL, &u);
    if (have_base) up_free(&base);
    if (!ok) {
        JSValue e = js_throw_type(ctx, "Failed to construct 'URL': Invalid URL '%.100s'", in);
        free(in);
        return e;
    }
    free(in);
    UrlObj *o = calloc(1, sizeof *o);
    if (!o) { up_free(&u); return js_throw_oom(ctx); }
    o->u = u;
    o->params = JS_UNDEFINED;
    JSValue v = js_new_host_object(ctx, ctx->url_class, JS_MKOBJ(ctx->proto_url), o);
    if (JS_IsException(v)) { url_finalize(ctx, o); return v; }
    JSValue sp = params_from_query(ctx, u.query ? u.query : "", v);
    if (JS_IsException(sp)) return sp;
    o->params = sp;
    return v;
}

static UrlObj *this_url(JSContext *ctx, JSValue v) {
    UrlObj *o = js_host_opaque(v, ctx->url_class);
    if (!o) js_throw_type(ctx, "not a URL");
    return o;
}

static JSValue cstr(JSContext *ctx, const char *s) { return js_str_utf8(ctx, s ? s : "", strlen(s ? s : "")); }

/* href 0, origin 1, protocol 2, username 3, password 4, host 5, hostname 6, port 7, pathname 8, search 9, hash 10, searchParams 11 */
FN(url_get) {
    UrlObj *o = this_url(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    UrlParts *u = &o->u;
    char buf[600];
    switch (magic) {
    case 0: { char *h = up_href(u); JSValue r = cstr(ctx, h); free(h); return r; }
    case 1:
        if (!u->special || !strcmp(u->scheme, "file")) return js_strc(ctx, "null");
        snprintf(buf, sizeof buf, "%s://%s%s%s", u->scheme, u->host, u->port[0] ? ":" : "", u->port);
        return cstr(ctx, buf);
    case 2: snprintf(buf, sizeof buf, "%s:", u->scheme); return cstr(ctx, buf);
    case 3: return cstr(ctx, u->user);
    case 4: return cstr(ctx, u->pass);
    case 5: snprintf(buf, sizeof buf, "%s%s%s", u->host, u->port[0] ? ":" : "", u->port); return cstr(ctx, buf);
    case 6: return cstr(ctx, u->host);
    case 7: return cstr(ctx, u->port);
    case 8: return cstr(ctx, u->path);
    case 9: if (!u->query || !u->query[0]) return ctx->empty_str; snprintf(buf, sizeof buf, "?%.590s", u->query); return cstr(ctx, buf);
    case 10: if (!u->frag || !u->frag[0]) return ctx->empty_str; snprintf(buf, sizeof buf, "#%.590s", u->frag); return cstr(ctx, buf);
    default: return o->params;
    }
}

static void params_reload(JSContext *ctx, JSValue params, const char *q);

FN(url_set) {
    UrlObj *o = this_url(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    char *v = arg_utf8(ctx, A(0));
    if (!v) return JS_EXCEPTION;
    UrlParts *u = &o->u;
    switch (magic) {
    case 0: {
        UrlParts n;
        if (!url_parse_parts(v, NULL, &n)) { free(v); return js_throw_type(ctx, "Invalid URL"); }
        up_free(u);
        *u = n;
        params_reload(ctx, o->params, u->query ? u->query : "");
        break;
    }
    case 2: {
        char *c = strchr(v, ':');
        if (c) *c = 0;
        if (v[0] && strlen(v) < sizeof u->scheme && is_special(v) == u->special) { for (char *p = v; *p; p++) *p = (char)tolower((unsigned char)*p); strcpy(u->scheme, v); }
        break;
    }
    case 3: strlcpy(u->user, v, sizeof u->user); break;
    case 4: strlcpy(u->pass, v, sizeof u->pass); break;
    case 5: case 6: {
        char *c = magic == 5 ? strchr(v, ':') : NULL;
        if (c) { *c = 0; strlcpy(u->port, c + 1, sizeof u->port); if (atoi(u->port) == default_port(u->scheme)) u->port[0] = 0; }
        if (v[0]) { for (char *p = v; *p; p++) *p = (char)tolower((unsigned char)*p); strlcpy(u->host, v, sizeof u->host); }
        break;
    }
    case 7: if (!v[0] || atoi(v) == default_port(u->scheme)) u->port[0] = 0; else snprintf(u->port, sizeof u->port, "%d", atoi(v) & 0xFFFF); break;
    case 8: {
        size_t l = strlen(v);
        char *p = malloc(l + 2);
        if (!p) break;
        if (v[0] != '/' && u->special) { p[0] = '/'; memcpy(p + 1, v, l + 1); } else memcpy(p, v, l + 1);
        if (p[0] == '/') remove_dots(p);
        char *enc = pct_encode(p, strlen(p), "?");
        free(p);
        free(u->path);
        u->path = enc;
        break;
    }
    case 9: {
        const char *s = v[0] == '?' ? v + 1 : v;
        free(u->query);
        u->query = v[0] ? pct_encode(s, strlen(s), NULL) : NULL;
        params_reload(ctx, o->params, u->query ? u->query : "");
        break;
    }
    case 10: {
        const char *s = v[0] == '#' ? v + 1 : v;
        free(u->frag);
        u->frag = v[0] ? pct_encode(s, strlen(s), "#") : NULL;
        break;
    }
    default: break;
    }
    free(v);
    return JS_UNDEFINED;
}

FN(url_to_string) { return url_get(ctx, this_val, 0, NULL, 0); }

FN(url_can_parse) {
    char *in = arg_utf8(ctx, A(0));
    if (!in) return JS_EXCEPTION;
    UrlParts base, u;
    bool hb = false;
    if (!JS_IsUndefined(A(1))) {
        char *b = arg_utf8(ctx, A(1));
        if (!b) { free(in); return JS_EXCEPTION; }
        hb = url_parse_parts(b, NULL, &base);
        free(b);
        if (!hb) { free(in); return magic ? JS_NULL : JS_FALSE; }
    }
    bool ok = url_parse_parts(in, hb ? &base : NULL, &u);
    if (hb) up_free(&base);
    free(in);
    if (ok) up_free(&u);
    if (!magic) return JS_BOOL(ok);
    if (!ok) return JS_NULL;
    JSValue args[2] = { A(0), A(1) };
    JSValue c = JS_MKOBJ(ctx->ctor_url);
    return js_construct_internal(ctx, c, argc > 1 ? 2 : 1, args, c);
}

/* ---- URLSearchParams: a JS array of [name, value] pairs in a hidden property ---- */
static JSValue sp_list(JSContext *ctx, JSValue sp) {
    if (!JS_IsObject(sp)) return JS_EXCEPTION;
    JSProp *p = js_find_prop(ctx, OBJ(sp), ctx->sp_list_key);
    if (!p) { js_throw_type(ctx, "not a URLSearchParams"); return JS_EXCEPTION; }
    return p->val;
}

static JSValue form_decode(JSContext *ctx, const char *s, size_t n) {
    char *o = malloc(n + 1);
    if (!o) return js_throw_oom(ctx);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '+') o[k++] = ' ';
        else if (s[i] == '%' && i + 2 < n && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            char h[3] = { s[i + 1], s[i + 2], 0 };
            o[k++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else o[k++] = s[i];
    }
    JSValue r = js_str_utf8(ctx, o, k);
    free(o);
    return r;
}

static int sp_parse_into(JSContext *ctx, JSValue list, const char *q) {
    if (*q == '?') q++;
    while (*q) {
        const char *e = strchr(q, '&');
        size_t l = e ? (size_t)(e - q) : strlen(q);
        if (l) {
            const char *eq = memchr(q, '=', l);
            size_t nl = eq ? (size_t)(eq - q) : l;
            JSValue nm = form_decode(ctx, q, nl);
            JSValue v = eq ? form_decode(ctx, eq + 1, l - nl - 1) : ctx->empty_str;
            if (JS_IsException(nm) || JS_IsException(v)) return -1;
            JSValue pair[2] = { nm, v };
            JSValue pr = js_array_from_list(ctx, pair, 2);
            if (JS_IsException(pr) || js_array_push_v(ctx, list, pr) < 0) return -1;
        }
        q += l;
        if (*q == '&') q++;
    }
    return 0;
}

static JSValue params_from_query(JSContext *ctx, const char *q, JSValue url) {
    JSValue o = js_obj_new(ctx, CL_OBJECT, ctx->proto_usp);
    if (JS_IsException(o)) return o;
    JSValue list = js_array_new(ctx, 0);
    if (JS_IsException(list)) return list;
    js_add_prop(ctx, OBJ(o), ctx->sp_list_key, list, P_W);
    js_add_prop(ctx, OBJ(o), ctx->sp_url_key, url, P_W);
    if (sp_parse_into(ctx, list, q) < 0) return JS_EXCEPTION;
    return o;
}

static void params_reload(JSContext *ctx, JSValue params, const char *q) {
    if (!JS_IsObject(params)) return;
    JSValue list = js_array_new(ctx, 0);
    if (JS_IsException(list)) { js_take_exception(ctx); return; }
    JSProp *p = js_find_prop(ctx, OBJ(params), ctx->sp_list_key);
    if (p) p->val = list;
    if (sp_parse_into(ctx, list, q) < 0) js_take_exception(ctx);
}

static void form_encode(JSSB *b, JSContext *ctx, JSValue s) {
    char *u = js_str_to_utf8(ctx, s, NULL);
    if (!u) return;
    static const char hex[] = "0123456789ABCDEF";
    for (char *p = u; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '*' || c == '-' || c == '.' || c == '_') sb_putc(b, c);
        else if (c == ' ') sb_putc(b, '+');
        else { sb_putc(b, '%'); sb_putc(b, (uint8_t)hex[c >> 4]); sb_putc(b, (uint8_t)hex[c & 15]); }
    }
    free(u);
}

static JSValue sp_serialize(JSContext *ctx, JSValue list) {
    JSSB b;
    sb_init(ctx, &b);
    JSObject *l = OBJ(list);
    for (uint32_t i = 0; i < l->u.a.len; i++) {
        JSObject *pr = OBJ(l->u.a.v[i]);
        if (i) sb_putc(&b, '&');
        form_encode(&b, ctx, pr->u.a.v[0]);
        sb_putc(&b, '=');
        form_encode(&b, ctx, pr->u.a.v[1]);
    }
    return sb_finish(&b);
}

/* after a change: write the query back into the URL */
static void sp_update(JSContext *ctx, JSValue sp, JSValue list) {
    JSProp *p = js_find_prop(ctx, OBJ(sp), ctx->sp_url_key);
    if (!p || !JS_IsObject(p->val)) return;
    UrlObj *o = js_host_opaque(p->val, ctx->url_class);
    if (!o) return;
    JSValue s = sp_serialize(ctx, list);
    if (JS_IsException(s)) { js_take_exception(ctx); return; }
    char *q = js_str_to_utf8(ctx, s, NULL);
    free(o->u.query);
    o->u.query = q && q[0] ? q : NULL;
    if (q && !q[0]) free(q);
}

FN(usp_ctor) {
    if (JS_IsUndefined(js_new_target(ctx))) return js_throw_type(ctx, "Constructor URLSearchParams requires 'new'");
    JSValue o = params_from_query(ctx, "", JS_UNDEFINED);
    if (JS_IsException(o)) return o;
    JSValue list = sp_list(ctx, o);
    JSValue init = A(0);
    if (JS_IsUndefined(init) || JS_IsNull(init)) return o;
    if (JS_IsObject(init)) {
        JSValue m = js_get_method(ctx, init, ctx->wk[WK_ITERATOR]);
        if (JS_IsException(m)) return m;
        if (!JS_IsUndefined(m)) {
            JSValue items = js_iterable_to_list(ctx, init);
            if (JS_IsException(items)) return items;
            for (uint32_t i = 0; i < OBJ(items)->u.a.len; i++) {
                JSValue pr = OBJ(items)->u.a.v[i];
                JSValue nm = js_geti(ctx, pr, 0), v = js_geti(ctx, pr, 1);
                if (JS_IsException(nm) || JS_IsException(v)) return JS_EXCEPTION;
                nm = js_tostr(ctx, nm);
                v = js_tostr(ctx, v);
                if (JS_IsException(nm) || JS_IsException(v)) return JS_EXCEPTION;
                JSValue pair[2] = { nm, v };
                JSValue p = js_array_from_list(ctx, pair, 2);
                if (JS_IsException(p) || js_array_push_v(ctx, list, p) < 0) return JS_EXCEPTION;
            }
            return o;
        }
        JSValue keys = js_own_keys(ctx, init, OK_STRINGS | OK_ENUM);
        if (JS_IsException(keys)) return keys;
        for (uint32_t i = 0; i < OBJ(keys)->u.a.len; i++) {
            JSValue k = OBJ(keys)->u.a.v[i];
            JSValue v = js_getp(ctx, init, k);
            if (!JS_IsException(v)) v = js_tostr(ctx, v);
            if (JS_IsException(v)) return v;
            JSValue pair[2] = { k, v };
            JSValue p = js_array_from_list(ctx, pair, 2);
            if (JS_IsException(p) || js_array_push_v(ctx, list, p) < 0) return JS_EXCEPTION;
        }
        return o;
    }
    char *q = arg_utf8(ctx, init);
    if (!q) return JS_EXCEPTION;
    int r = sp_parse_into(ctx, list, q);
    free(q);
    return r < 0 ? JS_EXCEPTION : o;
}

/* get 0, getAll 1, has 2, append 3, set 4, delete 5, toString 6, sort 7, forEach 8, size 9 */
FN(usp_method) {
    JSValue list = sp_list(ctx, this_val);
    if (JS_IsException(list)) return list;
    JSValue name = JS_UNDEFINED, val = JS_UNDEFINED;
    if (magic <= 5) { name = js_tostr(ctx, A(0)); if (JS_IsException(name)) return name; }
    if (magic == 3 || magic == 4) { val = js_tostr(ctx, A(1)); if (JS_IsException(val)) return val; }
    JSObject *l = OBJ(list);
    switch (magic) {
    case 0: for (uint32_t i = 0; i < l->u.a.len; i++) { JSObject *p = OBJ(l->u.a.v[i]); if (js_streq(ctx, p->u.a.v[0], name)) return p->u.a.v[1]; } return JS_NULL;
    case 1: {
        JSValue r = js_array_new(ctx, 0);
        if (JS_IsException(r)) return r;
        for (uint32_t i = 0; i < l->u.a.len; i++) { JSObject *p = OBJ(OBJ(list)->u.a.v[i]); if (js_streq(ctx, p->u.a.v[0], name)) js_array_push_v(ctx, r, p->u.a.v[1]); }
        return r;
    }
    case 2:
        for (uint32_t i = 0; i < l->u.a.len; i++) {
            JSObject *p = OBJ(l->u.a.v[i]);
            if (js_streq(ctx, p->u.a.v[0], name) && (argc < 2 || JS_IsUndefined(argv[1]) || js_streq(ctx, p->u.a.v[1], js_tostr(ctx, argv[1])))) return JS_TRUE;
        }
        return JS_FALSE;
    case 3: {
        JSValue pair[2] = { name, val };
        JSValue p = js_array_from_list(ctx, pair, 2);
        if (JS_IsException(p) || js_array_push_v(ctx, list, p) < 0) return JS_EXCEPTION;
        sp_update(ctx, this_val, list);
        return JS_UNDEFINED;
    }
    case 4: {
        bool found = false;
        uint32_t o = 0;
        for (uint32_t i = 0; i < l->u.a.len; i++) {
            JSObject *p = OBJ(l->u.a.v[i]);
            if (js_streq(ctx, p->u.a.v[0], name)) { if (found) continue; found = true; p->u.a.v[1] = val; }
            l->u.a.v[o++] = l->u.a.v[i];
        }
        js_array_set_length(ctx, l, o);
        if (!found) {
            JSValue pair[2] = { name, val };
            JSValue p = js_array_from_list(ctx, pair, 2);
            if (JS_IsException(p) || js_array_push_v(ctx, list, p) < 0) return JS_EXCEPTION;
        }
        sp_update(ctx, this_val, list);
        return JS_UNDEFINED;
    }
    case 5: {
        uint32_t o = 0;
        JSValue vv = argc > 1 && !JS_IsUndefined(argv[1]) ? js_tostr(ctx, argv[1]) : JS_UNDEFINED;
        for (uint32_t i = 0; i < l->u.a.len; i++) {
            JSObject *p = OBJ(l->u.a.v[i]);
            if (js_streq(ctx, p->u.a.v[0], name) && (JS_IsUndefined(vv) || js_streq(ctx, p->u.a.v[1], vv))) continue;
            l->u.a.v[o++] = l->u.a.v[i];
        }
        js_array_set_length(ctx, l, o);
        sp_update(ctx, this_val, list);
        return JS_UNDEFINED;
    }
    case 6: return sp_serialize(ctx, list);
    case 7: {
        /* stable insertion sort by name (code units) */
        for (uint32_t i = 1; i < l->u.a.len; i++) {
            JSValue x = l->u.a.v[i];
            uint32_t j = i;
            while (j > 0 && js_strcmp(ctx, OBJ(l->u.a.v[j - 1])->u.a.v[0], OBJ(x)->u.a.v[0]) > 0) { l->u.a.v[j] = l->u.a.v[j - 1]; j--; }
            l->u.a.v[j] = x;
        }
        sp_update(ctx, this_val, list);
        return JS_UNDEFINED;
    }
    case 8: {
        if (!js_is_callable(A(0))) return js_throw_type(ctx, "forEach: callback is not a function");
        for (uint32_t i = 0; i < OBJ(list)->u.a.len; i++) {
            JSObject *p = OBJ(OBJ(list)->u.a.v[i]);
            JSValue args[3] = { p->u.a.v[1], p->u.a.v[0], this_val };
            JSValue r = js_callv(ctx, A(0), A(1), 3, args);
            if (JS_IsException(r)) return r;
        }
        return JS_UNDEFINED;
    }
    default: return js_mknum(l->u.a.len);
    }
}

/* keys 0, values 1, entries 2: array iterators over a snapshot */
FN(usp_iter) {
    JSValue list = sp_list(ctx, this_val);
    if (JS_IsException(list)) return list;
    JSValue out = js_array_new(ctx, 0);
    if (JS_IsException(out)) return out;
    for (uint32_t i = 0; i < OBJ(list)->u.a.len; i++) {
        JSObject *p = OBJ(OBJ(list)->u.a.v[i]);
        JSValue v = magic == 0 ? p->u.a.v[0] : magic == 1 ? p->u.a.v[1] : js_array_from_list(ctx, p->u.a.v, 2);
        if (JS_IsException(v) || js_array_push_v(ctx, out, v) < 0) return JS_EXCEPTION;
    }
    return js_new_iter(ctx, ctx->proto_array_iter, out, ITT_ARRAY, IT_VALUES);
}

/* ==== Intl (en-US only) ======================================================================================= */
static JSValue intl_obj(JSContext *ctx, JSObject *proto, JSValue opts) {
    JSValue o = js_obj_new(ctx, CL_OBJECT, proto);
    if (JS_IsException(o)) return o;
    js_add_prop(ctx, OBJ(o), ctx->intl_opts_key, JS_IsObject(opts) ? opts : JS_UNDEFINED, P_W);
    return o;
}

static JSValue intl_opt(JSContext *ctx, JSValue self, const char *name) {
    JSProp *p = JS_IsObject(self) ? js_find_prop(ctx, OBJ(self), ctx->intl_opts_key) : NULL;
    if (!p || !JS_IsObject(p->val)) return JS_UNDEFINED;
    return js_get(ctx, p->val, name);
}

FN(intl_nf_ctor) { return intl_obj(ctx, ctx->proto_intl_nf, A(1)); }

FN(intl_nf_format) {
    bool e = false;
    double d = js_tonum(ctx, A(0), &e);
    if (e) return JS_EXCEPTION;
    JSValue style = intl_opt(ctx, this_val, "style");
    bool pct = js_str_eq_ascii(ctx, style, "percent"), cur = js_str_eq_ascii(ctx, style, "currency");
    if (pct) d *= 100;
    JSValue o = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
    if (JS_IsException(o)) return o;
    JSValue mx = intl_opt(ctx, this_val, "maximumFractionDigits"), mn = intl_opt(ctx, this_val, "minimumFractionDigits");
    if (cur) { if (JS_IsUndefined(mx)) mx = js_mknum(2); if (JS_IsUndefined(mn)) mn = js_mknum(2); }
    if (pct && JS_IsUndefined(mx)) mx = js_mknum(0);
    if (!JS_IsUndefined(mx)) js_set(ctx, o, "maximumFractionDigits", mx);
    if (!JS_IsUndefined(mn)) js_set(ctx, o, "minimumFractionDigits", mn);
    JSValue n = js_mknum(d);
    JSValue args[2] = { JS_UNDEFINED, o };
    JSValue s = js_invoke(ctx, n, js_atomc(ctx, "toLocaleString"), 2, args);
    if (JS_IsException(s)) return s;
    JSSB b;
    sb_init(ctx, &b);
    if (cur) {
        JSValue c = intl_opt(ctx, this_val, "currency");
        if (js_str_eq_ascii(ctx, c, "EUR")) sb_putc(&b, 0x20AC);
        else if (js_str_eq_ascii(ctx, c, "GBP")) sb_putc(&b, 0xA3);
        else if (js_str_eq_ascii(ctx, c, "JPY")) sb_putc(&b, 0xA5);
        else if (JS_IsString(c) && !js_str_eq_ascii(ctx, c, "USD")) { sb_putv(&b, c); sb_putc(&b, ' '); }
        else sb_putc(&b, '$');
    }
    sb_putv(&b, s);
    if (pct) sb_putc(&b, '%');
    return sb_finish(&b);
}

FN(intl_dtf_ctor) { return intl_obj(ctx, ctx->proto_intl_dtf, A(1)); }

FN(intl_dtf_format) {
    JSValue d = A(0);
    double t;
    if (JS_IsUndefined(d)) t = js_now(ctx);
    else if (js_is_cls(d, CL_DATE)) t = js_num(OBJ(d)->u.prim);
    else { bool e = false; t = js_tonum(ctx, d, &e); if (e) return JS_EXCEPTION; }
    JSValue hour = intl_opt(ctx, this_val, "hour"), year = intl_opt(ctx, this_val, "year"), month = intl_opt(ctx, this_val, "month");
    JSValue ts = intl_opt(ctx, this_val, "timeStyle"), ds = intl_opt(ctx, this_val, "dateStyle");
    int kind = 6;
    bool has_time = !JS_IsUndefined(hour) || !JS_IsUndefined(ts);
    bool has_date = !JS_IsUndefined(year) || !JS_IsUndefined(month) || !JS_IsUndefined(ds) || !has_time;
    if (has_time && has_date && (!JS_IsUndefined(year) || !JS_IsUndefined(ds))) kind = 5;
    else if (has_time && !has_date) kind = 7;
    else if (has_time) kind = 5;
    if (js_str_eq_ascii(ctx, month, "long") || js_str_eq_ascii(ctx, month, "short") || js_str_eq_ascii(ctx, ds, "long") || js_str_eq_ascii(ctx, ds, "medium")) {
        static const char *mn[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
        if (t != t) return js_strc(ctx, "Invalid Date");
        JSValue dt = js_mknum(t);
        JSValue dc = js_get(ctx, ctx->global, "Date");
        JSValue dobj = js_construct_internal(ctx, dc, 1, &dt, dc);
        if (JS_IsException(dobj)) return dobj;
        JSValue y = js_invoke(ctx, dobj, js_atomc(ctx, "getUTCFullYear"), 0, NULL);
        JSValue m = js_invoke(ctx, dobj, js_atomc(ctx, "getUTCMonth"), 0, NULL);
        JSValue dd = js_invoke(ctx, dobj, js_atomc(ctx, "getUTCDate"), 0, NULL);
        char buf[64];
        bool shrt = js_str_eq_ascii(ctx, month, "short") || js_str_eq_ascii(ctx, ds, "medium");
        int mi = (int)js_num(m);
        if (mi < 0 || mi > 11) mi = 0;
        char mb[12];
        snprintf(mb, sizeof mb, "%.*s", shrt ? 3 : 12, mn[mi]);
        if (JS_IsUndefined(year) && JS_IsUndefined(ds)) snprintf(buf, sizeof buf, "%s %d", mb, (int)js_num(dd));
        else snprintf(buf, sizeof buf, "%s %d, %d", mb, (int)js_num(dd), (int)js_num(y));
        return js_strc(ctx, buf);
    }
    return js_date_to_string(ctx, t, kind);
}

FN(intl_resolved) {
    JSValue o = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
    if (JS_IsException(o)) return o;
    js_set(ctx, o, "locale", js_strc(ctx, "en-US"));
    js_set(ctx, o, "timeZone", js_strc(ctx, "UTC"));
    js_set(ctx, o, "calendar", js_strc(ctx, "gregory"));
    js_set(ctx, o, "numberingSystem", js_strc(ctx, "latn"));
    return o;
}

FN(intl_coll_ctor) { return intl_obj(ctx, ctx->proto_intl_coll, A(1)); }

FN(intl_coll_compare) {
    JSValue a = js_tostr(ctx, A(0));
    if (JS_IsException(a)) return a;
    JSValue b = A(1);
    return js_invoke(ctx, a, js_atomc(ctx, "localeCompare"), 1, &b);
}

FN(intl_pr_ctor) { return intl_obj(ctx, ctx->proto_intl_pr, A(1)); }

FN(intl_pr_select) {
    bool e = false;
    double d = js_tonum(ctx, A(0), &e);
    if (e) return JS_EXCEPTION;
    JSValue type = intl_opt(ctx, this_val, "type");
    if (js_str_eq_ascii(ctx, type, "ordinal")) {
        long n = (long)fabs(d);
        long m10 = n % 10, m100 = n % 100;
        return js_strc(ctx, m10 == 1 && m100 != 11 ? "one" : m10 == 2 && m100 != 12 ? "two" : m10 == 3 && m100 != 13 ? "few" : "other");
    }
    return js_strc(ctx, d == 1 ? "one" : "other");
}

FN(intl_rtf_ctor) { return intl_obj(ctx, ctx->proto_intl_rtf, A(1)); }

FN(intl_rtf_format) {
    bool e = false;
    double v = js_tonum(ctx, A(0), &e);
    if (e) return JS_EXCEPTION;
    JSValue u = js_tostr(ctx, A(1));
    if (JS_IsException(u)) return u;
    char unit[24];
    js_str_to_ascii(ctx, u, unit, sizeof unit);
    size_t ul = strlen(unit);
    if (ul && unit[ul - 1] == 's') unit[--ul] = 0;
    double a = fabs(v);
    char buf[80], num[40];
    js_dtoa(a, num);
    JSValue numeric = intl_opt(ctx, this_val, "numeric");
    if (js_str_eq_ascii(ctx, numeric, "auto") && a <= 1) {
        if (!strcmp(unit, "day")) return js_strc(ctx, v == 0 ? "today" : v > 0 ? "tomorrow" : "yesterday");
        if (v == 0) { snprintf(buf, sizeof buf, "this %s", unit); return js_strc(ctx, buf); }
        snprintf(buf, sizeof buf, "%s %s", v > 0 ? "next" : "last", unit);
        return js_strc(ctx, buf);
    }
    if (v < 0 || (v == 0 && signbit(v))) snprintf(buf, sizeof buf, "%s %s%s ago", num, unit, a == 1 ? "" : "s");
    else snprintf(buf, sizeof buf, "in %s %s%s", num, unit, a == 1 ? "" : "s");
    return js_strc(ctx, buf);
}

FN(intl_locales) {
    JSValue l = js_strc(ctx, "en-US");
    return js_array_from_list(ctx, &l, 1);
}

/* ==== setup ================================================================================================= */
static JSObject *def_class(JSContext *ctx, JSValue on, const char *name, JSNativeFn ctor, int len, JSObject **proto) {
    *proto = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
    JSValue c = js_mk_native(ctx, ctor, js_atomc(ctx, name), len, 0);
    JSObject *co = OBJ(c);
    co->h.flags |= OF_CTOR;
    js_materialize(ctx, co);
    js_add_prop(ctx, co, ctx->a[A_prototype], JS_MKOBJ(*proto), 0);
    js_add_prop(ctx, *proto, ctx->a[A_constructor], c, P_W | P_C);
    js_add_prop(ctx, OBJ(on), js_atomc(ctx, name), c, P_W | P_C);
    return co;
}

void js_init_lib_misc(JSContext *ctx) {
    JSValue g = ctx->global;
    js_def_native(ctx, g, "queueMicrotask", g_queue_microtask, 1, 0);
    js_def_native(ctx, g, "structuredClone", g_structured_clone, 1, 0);
    js_def_native(ctx, g, "btoa", g_btoa, 1, 0);
    js_def_native(ctx, g, "atob", g_atob, 1, 0);
    ctx->td_enc_key = js_atomc(ctx, "<encoding>");
    ctx->sp_list_key = js_atomc(ctx, "<list>");
    ctx->sp_url_key = js_atomc(ctx, "<url>");
    ctx->intl_opts_key = js_atomc(ctx, "<options>");
    /* TextEncoder / TextDecoder */
    def_class(ctx, g, "TextEncoder", te_ctor, 0, &ctx->proto_textenc);
    JSValue tep = JS_MKOBJ(ctx->proto_textenc);
    js_def_native(ctx, tep, "encode", te_encode, 1, 0);
    js_def_native(ctx, tep, "encodeInto", te_encode_into, 2, 0);
    js_def_accessor(ctx, tep, js_atomc(ctx, "encoding"), te_encoding, NULL, 0, P_C);
    def_class(ctx, g, "TextDecoder", td_ctor, 0, &ctx->proto_textdec);
    JSValue tdp = JS_MKOBJ(ctx->proto_textdec);
    js_def_native(ctx, tdp, "decode", td_decode, 1, 0);
    js_def_accessor(ctx, tdp, js_atomc(ctx, "encoding"), td_encoding, NULL, 0, P_C);
    /* URL */
    js_class_t uc = { "URL", url_finalize, url_mark, NULL, NULL, NULL, NULL };
    ctx->url_class = js_new_class(ctx, &uc);
    ctx->ctor_url = def_class(ctx, g, "URL", url_ctor, 1, &ctx->proto_url);
    JSValue up = JS_MKOBJ(ctx->proto_url);
    static const char *un[12] = { "href", "origin", "protocol", "username", "password", "host", "hostname", "port", "pathname", "search", "hash", "searchParams" };
    for (int i = 0; i < 12; i++)
        js_def_accessor(ctx, up, js_atomc(ctx, un[i]), url_get, (i == 1 || i == 11) ? NULL : url_set, i, P_E | P_C);
    js_def_native(ctx, up, "toString", url_to_string, 0, 0);
    js_def_native(ctx, up, "toJSON", url_to_string, 0, 0);
    js_def_native(ctx, JS_MKOBJ(ctx->ctor_url), "canParse", url_can_parse, 1, 0);
    js_def_native(ctx, JS_MKOBJ(ctx->ctor_url), "parse", url_can_parse, 1, 1);
    js_add_prop(ctx, ctx->proto_url, ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, "URL"), P_C);
    def_class(ctx, g, "URLSearchParams", usp_ctor, 0, &ctx->proto_usp);
    JSValue spp = JS_MKOBJ(ctx->proto_usp);
    static const char *sn[9] = { "get", "getAll", "has", "append", "set", "delete", "toString", "sort", "forEach" };
    static const int sl[9] = { 1, 1, 1, 2, 2, 1, 0, 0, 1 };
    for (int i = 0; i < 9; i++) js_def_native(ctx, spp, sn[i], usp_method, sl[i], i);
    js_def_accessor(ctx, spp, ctx->a[A_size], usp_method, NULL, 9, P_C);
    js_def_native(ctx, spp, "keys", usp_iter, 0, 0);
    js_def_native(ctx, spp, "values", usp_iter, 0, 1);
    JSValue ent = js_def_native(ctx, spp, "entries", usp_iter, 0, 2);
    js_add_prop(ctx, ctx->proto_usp, ctx->wk[WK_ITERATOR], ent, P_W | P_C);
    js_add_prop(ctx, ctx->proto_usp, ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, "URLSearchParams"), P_C);
    /* Intl */
    JSValue intl = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
    js_add_prop(ctx, OBJ(g), js_atomc(ctx, "Intl"), intl, P_W | P_C);
    def_class(ctx, intl, "NumberFormat", intl_nf_ctor, 0, &ctx->proto_intl_nf);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_intl_nf), "format", intl_nf_format, 1, 0);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_intl_nf), "resolvedOptions", intl_resolved, 0, 0);
    def_class(ctx, intl, "DateTimeFormat", intl_dtf_ctor, 0, &ctx->proto_intl_dtf);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_intl_dtf), "format", intl_dtf_format, 1, 0);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_intl_dtf), "resolvedOptions", intl_resolved, 0, 0);
    def_class(ctx, intl, "Collator", intl_coll_ctor, 0, &ctx->proto_intl_coll);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_intl_coll), "compare", intl_coll_compare, 2, 0);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_intl_coll), "resolvedOptions", intl_resolved, 0, 0);
    def_class(ctx, intl, "PluralRules", intl_pr_ctor, 0, &ctx->proto_intl_pr);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_intl_pr), "select", intl_pr_select, 1, 0);
    def_class(ctx, intl, "RelativeTimeFormat", intl_rtf_ctor, 0, &ctx->proto_intl_rtf);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_intl_rtf), "format", intl_rtf_format, 2, 0);
    js_def_native(ctx, intl, "getCanonicalLocales", intl_locales, 1, 0);
}
