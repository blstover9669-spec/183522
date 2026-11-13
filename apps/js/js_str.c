/*
 * js_str.c - NovaJS strings: UTF-16 code units stored inline in a cell,
 * ropes for long concatenations (flattened on first use), the atom table
 * (interned property names), a string builder, UTF-8 and case mapping.
 */
#include "jsi.h"

#define STR_MAX (1u << 28)
#define ROPE_MIN 192
#define ROPE_MAXDEPTH 4000

JSString *js_str_alloc(JSContext *ctx, uint32_t len) {
    if (len >= STR_MAX) { js_throw_range(ctx, "Invalid string length"); return NULL; }
    JSString *s = js_cell(ctx, T_STRING, sizeof(JSString) + (size_t)len * 2);
    if (s) s->len = len;
    return s;
}

JSValue js_str16(JSContext *ctx, const uint16_t *s, uint32_t len) {
    if (!len && JS_IsString(ctx->empty_str)) return ctx->empty_str;
    JSString *p = js_str_alloc(ctx, len);
    if (!p) return JS_EXCEPTION;
    if (len) memcpy(p->s, s, (size_t)len * 2);
    return JS_MKSTR(p);
}

JSValue js_str8(JSContext *ctx, const char *s, uint32_t len) {
    if (!len && JS_IsString(ctx->empty_str)) return ctx->empty_str;
    JSString *p = js_str_alloc(ctx, len);
    if (!p) return JS_EXCEPTION;
    for (uint32_t i = 0; i < len; i++) p->s[i] = (uint8_t)s[i];
    return JS_MKSTR(p);
}

JSValue js_strc(JSContext *ctx, const char *a) { return js_str8(ctx, a, (uint32_t)strlen(a)); }

uint32_t js_utf8_decode(const uint8_t *s, size_t n, uint32_t *cp) {
    uint8_t c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int len = c >= 0xF0 && c < 0xF8 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
    if (!len || (size_t)len > n) { *cp = 0xFFFD; return 1; }
    uint32_t v = c & (0x3F >> (len - 1));
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (s[i] & 0x3F);
    }
    if ((len == 3 && v < 0x800) || (len == 4 && (v < 0x10000 || v > 0x10FFFF))) { *cp = 0xFFFD; return (uint32_t)len; }
    *cp = v;
    return (uint32_t)len;
}

int js_utf8_encode(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

JSValue js_str_utf8(JSContext *ctx, const char *src, size_t n) {
    const uint8_t *s = (const uint8_t *)src;
    size_t units = 0;
    bool ascii = true;
    for (size_t i = 0; i < n;) {
        if (s[i] < 0x80) { units++; i++; continue; }
        ascii = false;
        uint32_t cp;
        i += js_utf8_decode(s + i, n - i, &cp);
        units += cp >= 0x10000 ? 2 : 1;
    }
    if (units >= STR_MAX) return js_throw_range(ctx, "Invalid string length");
    if (ascii) return js_str8(ctx, src, (uint32_t)n);
    JSString *p = js_str_alloc(ctx, (uint32_t)units);
    if (!p) return JS_EXCEPTION;
    uint32_t o = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        i += js_utf8_decode(s + i, n - i, &cp);
        if (cp >= 0x10000) { cp -= 0x10000; p->s[o++] = (uint16_t)(0xD800 + (cp >> 10)); p->s[o++] = (uint16_t)(0xDC00 + (cp & 0x3FF)); }
        else p->s[o++] = (uint16_t)cp;
    }
    return JS_MKSTR(p);
}

/* ---- ropes ------------------------------------------------------------------------------- */
uint32_t js_strlen(JSValue v) {
    GCHdr *h = JS_PTR(v);
    return h->type == T_ROPE ? ((JSRope *)h)->len : ((JSString *)h)->len;
}

static uint32_t depth_of(JSValue v) {
    GCHdr *h = JS_PTR(v);
    return h->type == T_ROPE && !((JSRope *)h)->flat ? ((JSRope *)h)->depth : 0;
}

JSString *js_flat(JSContext *ctx, JSValue v) {
    GCHdr *h = JS_PTR(v);
    if (h->type == T_STRING) return (JSString *)h;
    JSRope *r = (JSRope *)h;
    if (r->flat) return r->flat;
    JSString *out = js_str_alloc(ctx, r->len);
    if (!out) return NULL;
    /* iterative walk: left to right */
    int cap = 64, sp = 0;
    JSValue small[64], *st = small;
    st[sp++] = v;
    uint32_t o = 0;
    while (sp) {
        JSValue x = st[--sp];
        GCHdr *xh = JS_PTR(x);
        if (xh->type == T_STRING || ((JSRope *)xh)->flat) {
            JSString *s = xh->type == T_STRING ? (JSString *)xh : ((JSRope *)xh)->flat;
            memcpy(out->s + o, s->s, (size_t)s->len * 2);
            o += s->len;
            continue;
        }
        JSRope *xr = (JSRope *)xh;
        if (sp + 2 > cap) {
            int nc = cap * 2;
            JSValue *n = malloc(sizeof(JSValue) * (size_t)nc);
            if (!n) { if (st != small) free(st); return js_throw_oom(ctx), NULL; }
            memcpy(n, st, sizeof(JSValue) * (size_t)sp);
            if (st != small) free(st);
            st = n;
            cap = nc;
        }
        st[sp++] = xr->right;
        st[sp++] = xr->left;
    }
    if (st != small) free(st);
    r->flat = out;
    r->left = r->right = JS_UNDEFINED;
    return out;
}

JSValue js_concat(JSContext *ctx, JSValue a, JSValue b) {
    uint32_t la = js_strlen(a), lb = js_strlen(b);
    if (!la) return b;
    if (!lb) return a;
    if ((uint64_t)la + lb >= STR_MAX) return js_throw_range(ctx, "Invalid string length");
    if (la + lb < ROPE_MIN) {
        JSString *sa = js_flat(ctx, a);
        if (!sa) return JS_EXCEPTION;
        JSString *sb = js_flat(ctx, b);
        if (!sb) return JS_EXCEPTION;
        JSString *p = js_str_alloc(ctx, la + lb);
        if (!p) return JS_EXCEPTION;
        memcpy(p->s, sa->s, (size_t)la * 2);
        memcpy(p->s + la, sb->s, (size_t)lb * 2);
        return JS_MKSTR(p);
    }
    JSRope *r = js_cell(ctx, T_ROPE, sizeof(JSRope));
    if (!r) return JS_EXCEPTION;
    r->len = la + lb;
    r->left = a;
    r->right = b;
    r->depth = MAX(depth_of(a), depth_of(b)) + 1;
    JSValue v = JS_MKTAG(JS_TAG_STRING, (uintptr_t)r);
    if (r->depth > ROPE_MAXDEPTH && !js_flat(ctx, v)) return JS_EXCEPTION;
    return v;
}

JSValue js_substr(JSContext *ctx, JSValue v, uint32_t start, uint32_t end) {
    JSString *s = js_flat(ctx, v);
    if (!s) return JS_EXCEPTION;
    if (end > s->len) end = s->len;
    if (start >= end) return ctx->empty_str;
    if (start == 0 && end == s->len) return v;
    return js_str16(ctx, s->s + start, end - start);
}

int js_strcmp(JSContext *ctx, JSValue a, JSValue b) {
    JSString *x = js_flat(ctx, a), *y = js_flat(ctx, b);
    if (!x || !y) return 0;
    uint32_t n = MIN(x->len, y->len);
    for (uint32_t i = 0; i < n; i++) if (x->s[i] != y->s[i]) return x->s[i] < y->s[i] ? -1 : 1;
    return x->len < y->len ? -1 : x->len > y->len;
}

bool js_streq(JSContext *ctx, JSValue a, JSValue b) {
    if (a == b) return true;
    JSString *x = js_flat(ctx, a), *y = js_flat(ctx, b);
    if (!x || !y || x->len != y->len) return false;
    if ((x->h.flags & y->h.flags & STR_ATOM)) return false;    /* distinct atoms differ */
    if ((x->h.flags & y->h.flags & STR_HASHED) && x->hash != y->hash) return false;
    return !memcmp(x->s, y->s, (size_t)x->len * 2);
}

bool js_str_eq_ascii(JSContext *ctx, JSValue v, const char *a) {
    JSString *s = js_flat(ctx, v);
    if (!s) return false;
    size_t n = strlen(a);
    if (n != s->len) return false;
    for (size_t i = 0; i < n; i++) if (s->s[i] != (uint8_t)a[i]) return false;
    return true;
}

uint32_t js_str_hash(JSString *s) {
    if (s->h.flags & STR_HASHED) return s->hash;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < s->len; i++) { h ^= s->s[i]; h *= 16777619u; }
    if (!h) h = 1;
    s->hash = h;
    s->h.flags |= STR_HASHED;
    return h;
}

/* ---- atoms -------------------------------------------------------------------------------- */
static bool atoms_grow(JSContext *ctx) {
    uint32_t nc = ctx->atomcap ? ctx->atomcap * 2 : 1024;
    JSString **n = calloc(nc, sizeof *n);
    if (!n) return false;
    for (uint32_t i = 0; i < ctx->atomcap; i++) {
        JSString *s = ctx->atoms[i];
        if (!s) continue;
        uint32_t k = s->hash & (nc - 1);
        while (n[k]) k = (k + 1) & (nc - 1);
        n[k] = s;
    }
    free(ctx->atoms);
    ctx->atoms = n;
    ctx->atomcap = nc;
    return true;
}

static JSString *atom_lookup(JSContext *ctx, const uint16_t *s, uint32_t len, uint32_t h, uint32_t *slot) {
    uint32_t m = ctx->atomcap - 1, k = h & m;
    while (ctx->atoms[k]) {
        JSString *a = ctx->atoms[k];
        if (a->hash == h && a->len == len && !memcmp(a->s, s, (size_t)len * 2)) return a;
        k = (k + 1) & m;
    }
    *slot = k;
    return NULL;
}

JSValue js_atom(JSContext *ctx, JSValue v) {
    JSString *s = js_flat(ctx, v);
    if (!s) return JS_EXCEPTION;
    if (s->h.flags & STR_ATOM) return JS_MKSTR(s);
    if ((ctx->natoms + 1) * 2 > ctx->atomcap && !atoms_grow(ctx)) return js_throw_oom(ctx);
    uint32_t h = js_str_hash(s), slot = 0;
    JSString *a = atom_lookup(ctx, s->s, s->len, h, &slot);
    if (a) return JS_MKSTR(a);
    s->h.flags |= STR_ATOM;
    ctx->atoms[slot] = s;
    ctx->natoms++;
    return JS_MKSTR(s);
}

JSValue js_atom16(JSContext *ctx, const uint16_t *p, uint32_t len) {
    if ((ctx->natoms + 1) * 2 > ctx->atomcap && !atoms_grow(ctx)) return js_throw_oom(ctx);
    uint32_t h = 2166136261u, slot = 0;
    for (uint32_t i = 0; i < len; i++) { h ^= p[i]; h *= 16777619u; }
    if (!h) h = 1;
    JSString *a = atom_lookup(ctx, p, len, h, &slot);
    if (a) return JS_MKSTR(a);
    JSString *s = js_str_alloc(ctx, len);
    if (!s) return JS_EXCEPTION;
    if (len) memcpy(s->s, p, (size_t)len * 2);
    s->hash = h;
    s->h.flags |= STR_HASHED | STR_ATOM;
    /* (the allocation may have collected: find the slot again) */
    if ((ctx->natoms + 1) * 2 > ctx->atomcap && !atoms_grow(ctx)) return js_throw_oom(ctx);
    if (atom_lookup(ctx, p, len, h, &slot)) { s->h.flags &= (uint8_t)~STR_ATOM; return js_atom16(ctx, p, len); }
    ctx->atoms[slot] = s;
    ctx->natoms++;
    return JS_MKSTR(s);
}

JSValue js_atomc(JSContext *ctx, const char *a) {
    uint16_t buf[128];
    size_t n = strlen(a);
    if (n > 128) return js_atom(ctx, js_strc(ctx, a));
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)a[i];
    return js_atom16(ctx, buf, (uint32_t)n);
}

bool js_atom_index(JSValue v, uint32_t *idx) {
    if (!JS_IsString(v)) return false;
    JSString *s = JS_PTR(v);
    if (s->h.type != T_STRING) return false;
    if (s->h.flags & STR_IDXCHK) { *idx = s->idx; return (s->h.flags & STR_ISIDX) != 0; }
    bool ok = false;
    uint32_t n = s->len;
    if (n > 0 && n <= 10 && s->s[0] >= '0' && s->s[0] <= '9' && (n == 1 || s->s[0] != '0')) {
        uint64_t x = 0;
        ok = true;
        for (uint32_t i = 0; i < n; i++) {
            if (s->s[i] < '0' || s->s[i] > '9') { ok = false; break; }
            x = x * 10 + (s->s[i] - '0');
        }
        if (ok && x >= 0xFFFFFFFFull) ok = false;
        if (ok) s->idx = (uint32_t)x;
    }
    s->h.flags |= STR_IDXCHK | (ok ? STR_ISIDX : 0);
    *idx = s->idx;
    return ok;
}

JSValue js_atom_from_index(JSContext *ctx, uint32_t i) {
    uint16_t buf[12];
    int n = 0;
    char t[12];
    do { t[n++] = (char)('0' + i % 10); i /= 10; } while (i);
    for (int k = 0; k < n; k++) buf[k] = (uint16_t)t[n - 1 - k];
    return js_atom16(ctx, buf, (uint32_t)n);
}

void js_atoms_sweep(JSContext *ctx) {
    uint32_t cap = ctx->atomcap, live = 0;
    if (!cap) return;
    JSString **old = ctx->atoms;
    JSString **n = calloc(cap, sizeof *n);
    if (!n) {   /* cannot rebuild: keep every atom alive instead */
        for (uint32_t i = 0; i < cap; i++) if (old[i]) old[i]->h.mark = 1;
        return;
    }
    for (uint32_t i = 0; i < cap; i++) {
        JSString *s = old[i];
        if (!s || !s->h.mark) continue;
        uint32_t k = s->hash & (cap - 1);
        while (n[k]) k = (k + 1) & (cap - 1);
        n[k] = s;
        live++;
    }
    free(old);
    ctx->atoms = n;
    ctx->natoms = live;
}

/* ---- conversions for C ------------------------------------------------------------------------ */
char *js_str_to_utf8(JSContext *ctx, JSValue v, size_t *len) {
    JSString *s = js_flat(ctx, v);
    if (!s) return NULL;
    size_t n = 0;
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t c = s->s[i];
        if (c < 0x80) n++;
        else if (c < 0x800) n += 2;
        else if (c >= 0xD800 && c < 0xDC00 && i + 1 < s->len && s->s[i + 1] >= 0xDC00 && s->s[i + 1] < 0xE000) { n += 4; i++; }
        else n += 3;
    }
    char *o = malloc(n + 1);
    if (!o) { js_throw_oom(ctx); return NULL; }
    size_t k = 0;
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t c = s->s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s->len && s->s[i + 1] >= 0xDC00 && s->s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s->s[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
        k += (size_t)js_utf8_encode(c, o + k);
    }
    o[k] = 0;
    if (len) *len = k;
    return o;
}

void js_str_to_ascii(JSContext *ctx, JSValue v, char *out, size_t size) {
    if (!size) return;
    out[0] = 0;
    if (!JS_IsString(v)) return;
    JSString *s = js_flat(ctx, v);
    if (!s) return;
    size_t k = 0;
    for (uint32_t i = 0; i < s->len && k + 4 < size; i++) {
        uint32_t c = s->s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s->len) { c = 0x10000 + ((c - 0xD800) << 10) + (s->s[i + 1] - 0xDC00); i++; }
        k += (size_t)js_utf8_encode(c, out + k);
    }
    out[k] = 0;
}

int32_t js_str_index_of(JSContext *ctx, JSString *h, JSString *n, uint32_t from) {
    (void)ctx;
    if (n->len == 0) return from <= h->len ? (int32_t)from : (int32_t)h->len;
    if (n->len > h->len) return -1;
    uint16_t c0 = n->s[0];
    for (uint32_t i = from; i + n->len <= h->len; i++) {
        if (h->s[i] != c0) continue;
        if (!memcmp(h->s + i + 1, n->s + 1, (size_t)(n->len - 1) * 2)) return (int32_t)i;
    }
    return -1;
}

int32_t js_str_last_index_of(JSContext *ctx, JSString *h, JSString *n, uint32_t from) {
    (void)ctx;
    if (n->len > h->len) return -1;
    uint32_t i = MIN(from, h->len - n->len);
    for (;; i--) {
        if (!memcmp(h->s + i, n->s, (size_t)n->len * 2)) return (int32_t)i;
        if (!i) break;
    }
    return -1;
}

/* ---- character classes ------------------------------------------------------------------------- */
bool js_is_space(uint32_t c) {
    if (c < 128) return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B || c == 0x0C;
    return c == 0xA0 || c == 0xFEFF || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x3000;
}

bool js_is_id_start(uint32_t c) {
    if (c < 128) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_';
    if (js_is_space(c)) return false;
    if (c < 0x100) return c == 0xAA || c == 0xB5 || c == 0xBA || (c >= 0xC0 && c != 0xD7 && c != 0xF7);
    if ((c >= 0x2000 && c <= 0x206F) || (c >= 0x2190 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x3003) ||
        (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF0F) || (c >= 0xD800 && c <= 0xDFFF) || c == 0xFFFD)
        return false;
    return true;
}

bool js_is_id_part(uint32_t c) {
    if (c < 128) return js_is_id_start(c) || (c >= '0' && c <= '9');
    if (c == 0x200C || c == 0x200D) return true;
    if (c >= 0x300 && c <= 0x36F) return true;   /* combining marks */
    return js_is_id_start(c);
}

/* ---- case mapping (the common alphabets) ------------------------------------------------------------- */
uint32_t js_case_upper(uint32_t c) {
    if (c < 128) return c >= 'a' && c <= 'z' ? c - 32 : c;
    if (c < 0x100) {
        if (c == 0xB5) return 0x39C;
        if (c == 0xFF) return 0x178;
        if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
        return c;
    }
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x131) return 'I';
        if (c == 0x17F) return 'S';
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
        if (c == 0x138 || c == 0x149) return c;
        return (c & 1) ? c - 1 : c;
    }
    if (c >= 0x180 && c <= 0x24F) {
        if (c >= 0x1CD && c <= 0x1DC) return (c & 1) ? c : c - 1;
        if (c >= 0x1DE && c <= 0x1EF) return (c & 1) ? c - 1 : c;
        if (c >= 0x1F8 && c <= 0x24F) return (c & 1) ? c - 1 : c;
        if (c == 0x1C6 || c == 0x1C9 || c == 0x1CC) return c - 2;
        if (c == 0x1F3) return 0x1F1;
        return c;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c == 0x3C2) return 0x3A3;
    if (c >= 0x3AC && c <= 0x3CE) {
        switch (c) { case 0x3AC: return 0x386; case 0x3AD: return 0x388; case 0x3AE: return 0x389; case 0x3AF: return 0x38A;
                     case 0x3CC: return 0x38C; case 0x3CD: return 0x38E; case 0x3CE: return 0x38F; default: return c; }
    }
    if (c >= 0x3D8 && c <= 0x3EF) return (c & 1) ? c - 1 : c;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) return (c & 1) ? c - 1 : c;
    if (c >= 0x4C1 && c <= 0x4CE) return (c & 1) ? c : c - 1;
    if (c == 0x4CF) return 0x4C0;
    if (c >= 0x561 && c <= 0x586) return c - 48;
    if (c >= 0x10D0 && c <= 0x10FA) return c + 3008;
    if (c >= 0x1E00 && c <= 0x1EFF) return (c & 1) ? c - 1 : c;
    if (c >= 0x1F00 && c <= 0x1FFF) {
        uint32_t lo = c & 0xF;
        uint32_t row = c & 0xFFF0;
        if ((row == 0x1F00 || row == 0x1F10 || row == 0x1F20 || row == 0x1F30 || row == 0x1F40 || row == 0x1F60) && lo < 8) return c + 8;
        if (row == 0x1F50 && lo < 8 && (lo & 1)) return c + 8;
        return c;
    }
    if (c >= 0x2170 && c <= 0x217F) return c - 16;
    if (c >= 0x24D0 && c <= 0x24E9) return c - 26;
    if (c >= 0x2C30 && c <= 0x2C5F) return c - 48;
    if (c >= 0x2D00 && c <= 0x2D25) return c - 7264;
    if (c >= 0xA640 && c <= 0xA66D) return (c & 1) ? c - 1 : c;
    if (c >= 0xA680 && c <= 0xA69B) return (c & 1) ? c - 1 : c;
    if (c >= 0xA722 && c <= 0xA787 && c != 0xA730 && c != 0xA731) return (c & 1) ? c - 1 : c;
    if (c >= 0xFF41 && c <= 0xFF5A) return c - 32;
    if (c >= 0x10428 && c <= 0x1044F) return c - 40;
    if (c >= 0x1E922 && c <= 0x1E943) return c - 34;
    return c;
}

uint32_t js_case_lower(uint32_t c) {
    if (c < 128) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if (c < 0x100) return (c >= 0xC0 && c <= 0xDE && c != 0xD7) ? c + 32 : c;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x130) return 'i';
        if (c == 0x178) return 0xFF;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
        if (c == 0x138 || c == 0x149 || c == 0x131 || c == 0x17F) return c;
        return (c & 1) ? c : c + 1;
    }
    if (c >= 0x180 && c <= 0x24F) {
        if (c >= 0x1CD && c <= 0x1DC) return (c & 1) ? c + 1 : c;
        if (c >= 0x1DE && c <= 0x1EF) return (c & 1) ? c : c + 1;
        if (c >= 0x1F8 && c <= 0x24F) return (c & 1) ? c : c + 1;
        if (c == 0x1C4 || c == 0x1C7 || c == 0x1CA) return c + 2;
        if (c == 0x1F1) return 0x1F3;
        return c;
    }
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c >= 0x386 && c <= 0x38F) {
        switch (c) { case 0x386: return 0x3AC; case 0x388: return 0x3AD; case 0x389: return 0x3AE; case 0x38A: return 0x3AF;
                     case 0x38C: return 0x3CC; case 0x38E: return 0x3CD; case 0x38F: return 0x3CE; default: return c; }
    }
    if (c >= 0x3D8 && c <= 0x3EF) return (c & 1) ? c : c + 1;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) return (c & 1) ? c : c + 1;
    if (c >= 0x4C1 && c <= 0x4CE) return (c & 1) ? c + 1 : c;
    if (c == 0x4C0) return 0x4CF;
    if (c >= 0x531 && c <= 0x556) return c + 48;
    if (c >= 0x10A0 && c <= 0x10C5) return c + 7264;
    if (c >= 0x1C90 && c <= 0x1CBA) return c - 3008;
    if (c >= 0x1E00 && c <= 0x1EFF) { if (c == 0x1E9E) return 0xDF; return (c & 1) ? c : c + 1; }
    if (c >= 0x1F00 && c <= 0x1FFF) {
        uint32_t lo = c & 0xF, row = c & 0xFFF0;
        if ((row == 0x1F00 || row == 0x1F10 || row == 0x1F20 || row == 0x1F30 || row == 0x1F40 || row == 0x1F60) && lo >= 8) return c - 8;
        if (row == 0x1F50 && lo >= 8 && (lo & 1)) return c - 8;
        return c;
    }
    if (c >= 0x2160 && c <= 0x216F) return c + 16;
    if (c >= 0x24B6 && c <= 0x24CF) return c + 26;
    if (c >= 0x2C00 && c <= 0x2C2F) return c + 48;
    if (c >= 0xA640 && c <= 0xA66D) return (c & 1) ? c : c + 1;
    if (c >= 0xA680 && c <= 0xA69B) return (c & 1) ? c : c + 1;
    if (c >= 0xA722 && c <= 0xA787 && c != 0xA730 && c != 0xA731) return (c & 1) ? c : c + 1;
    if (c >= 0xFF21 && c <= 0xFF3A) return c + 32;
    if (c >= 0x10400 && c <= 0x10427) return c + 40;
    if (c >= 0x1E900 && c <= 0x1E921) return c + 34;
    return c;
}

/* ---- string builder --------------------------------------------------------------------------------- */
void sb_init(JSContext *ctx, JSSB *b) { b->ctx = ctx; b->s = NULL; b->len = b->cap = 0; b->err = false; }

static bool sb_grow(JSSB *b, uint32_t extra) {
    if (b->err) return false;
    if ((uint64_t)b->len + extra >= STR_MAX) { b->err = true; js_throw_range(b->ctx, "Invalid string length"); return false; }
    if (b->len + extra <= b->cap) return true;
    uint32_t nc = b->cap ? b->cap : 32;
    while (nc < b->len + extra) nc *= 2;
    uint16_t *n = js_realloc(b->ctx, b->s, (size_t)b->cap * 2, (size_t)nc * 2);
    if (!n) { b->err = true; return false; }
    b->s = n;
    b->cap = nc;
    return true;
}

void sb_putc(JSSB *b, uint32_t c) {
    if (c >= 0x10000) {
        if (!sb_grow(b, 2)) return;
        c -= 0x10000;
        b->s[b->len++] = (uint16_t)(0xD800 + (c >> 10));
        b->s[b->len++] = (uint16_t)(0xDC00 + (c & 0x3FF));
        return;
    }
    if (b->len >= b->cap && !sb_grow(b, 1)) return;
    b->s[b->len++] = (uint16_t)c;
}

void sb_put16(JSSB *b, const uint16_t *s, uint32_t n) {
    if (!n || !sb_grow(b, n)) return;
    memcpy(b->s + b->len, s, (size_t)n * 2);
    b->len += n;
}

void sb_puts(JSSB *b, const char *a) {
    size_t n = strlen(a);
    if (!n || !sb_grow(b, (uint32_t)n)) return;
    for (size_t i = 0; i < n; i++) b->s[b->len++] = (uint8_t)a[i];
}

void sb_putv(JSSB *b, JSValue v) {
    if (b->err) return;
    JSString *s = js_flat(b->ctx, v);
    if (!s) { b->err = true; return; }
    sb_put16(b, s->s, s->len);
}

JSValue sb_finish(JSSB *b) {
    if (b->err) { sb_free(b); return JS_EXCEPTION; }
    JSValue v = js_str16(b->ctx, b->s, b->len);
    sb_free(b);
    return v;
}

void sb_free(JSSB *b) {
    js_mfree(b->ctx, b->s, (size_t)b->cap * 2);
    b->s = NULL;
    b->len = b->cap = 0;
}
