/*
 * js_lib_arr.c - NovaJS built-ins: Array, the iteration protocol (helpers,
 * %IteratorPrototype%, array / generator iterators) and typed arrays
 * (ArrayBuffer, the %TypedArray% family, DataView).
 */
#include "jsi.h"

#define FN(name) static JSValue name(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic)
#define A(i) JS_ARG(i)

/* ==== helpers ============================================================================ */
static JSValue key_of(JSContext *ctx, int64_t i) {
    if (i >= 0 && i < 0xFFFFFFFFll) return js_atom_from_index(ctx, (uint32_t)i);
    return js_atom(ctx, js_num_to_str(ctx, (double)i));
}

static JSValue get_i(JSContext *ctx, JSValue o, int64_t i) {
    if (i >= 0 && i < 0xFFFFFFFFll) return js_geti(ctx, o, (uint32_t)i);
    JSValue k = key_of(ctx, i);
    if (JS_IsException(k)) return k;
    return js_getp(ctx, o, k);
}

static int set_i(JSContext *ctx, JSValue o, int64_t i, JSValue v) {
    if (i >= 0 && i < 0xFFFFFFFFll && js_fast_array(o)) {
        JSObject *a = OBJ(o);
        if (i < a->u.a.len && i < a->u.a.cap && a->u.a.v[i] != JS_HOLE && !(a->h.flags & OF_FROZEN)) { a->u.a.v[i] = v; return 1; }
    }
    JSValue k = key_of(ctx, i);
    if (JS_IsException(k)) return -1;
    return js_setp(ctx, o, k, v, o, true);
}

static int has_i(JSContext *ctx, JSValue o, int64_t i) {
    if (i >= 0 && i < 0xFFFFFFFFll && js_fast_array(o)) {
        JSObject *a = OBJ(o);
        if (i < a->u.a.len && i < a->u.a.cap && a->u.a.v[i] != JS_HOLE) return 1;
        if (!a->proto || (a->proto == ctx->proto_array && ctx->proto_array->nprops < 64 && a->proto->proto == ctx->proto_object)) {
            /* holes: only the (index-free) prototypes could still have it */
            bool idxprops = false;
            for (JSObject *p = a->proto; p && !idxprops; p = p->proto)
                for (uint32_t k = 0; k < p->nprops; k++) { uint32_t x; if (js_atom_index(p->props[k].key, &x)) { idxprops = true; break; } }
            if (!idxprops) return 0;
        }
    }
    JSValue k = key_of(ctx, i);
    if (JS_IsException(k)) return -1;
    return js_has_prop(ctx, o, k);
}

static int del_i(JSContext *ctx, JSValue o, int64_t i) {
    JSValue k = key_of(ctx, i);
    if (JS_IsException(k)) return -1;
    return js_delete_prop(ctx, o, k, true);
}

static int create_i(JSContext *ctx, JSValue o, int64_t i, JSValue v) {
    JSValue k = key_of(ctx, i);
    if (JS_IsException(k)) return -1;
    return js_create_data_prop(ctx, o, k, v);
}

static int set_len(JSContext *ctx, JSValue o, int64_t n) {
    return js_setp(ctx, o, ctx->a[A_length], js_mknum((double)n), o, true) < 0 ? -1 : 0;
}

/* relative index argument: clamp into [0, len] */
static int rel_index(JSContext *ctx, JSValue v, int64_t len, int64_t def, int64_t *out) {
    if (JS_IsUndefined(v)) { *out = def; return 0; }
    bool e = false;
    double d = js_tointeger(ctx, v, &e);
    if (e) return -1;
    if (d < 0) { d += (double)len; if (d < 0) d = 0; }
    else if (d > (double)len) d = (double)len;
    *out = (int64_t)d;
    return 0;
}

static JSValue this_obj(JSContext *ctx, JSValue v) { return js_toobj(ctx, v); }

static JSValue array_species_create(JSContext *ctx, JSValue o, int64_t n) {
    int ia = js_is_array_v(ctx, o);
    if (ia < 0) return JS_EXCEPTION;
    if (!ia) {
        if (n > 0xFFFFFFFFll) return js_throw_range(ctx, "Invalid array length");
        return js_array_new(ctx, (uint32_t)n);
    }
    JSValue c = js_getp(ctx, o, ctx->a[A_constructor]);
    if (JS_IsException(c)) return c;
    if (JS_IsObject(c)) {
        c = js_getp(ctx, c, ctx->wk[WK_SPECIES]);
        if (JS_IsException(c)) return c;
        if (JS_IsNull(c)) c = JS_UNDEFINED;
    }
    if (JS_IsUndefined(c) || c == JS_MKOBJ(ctx->ctor_array)) {
        if (n > 0xFFFFFFFFll) return js_throw_range(ctx, "Invalid array length");
        return js_array_new(ctx, (uint32_t)n);
    }
    if (!js_is_ctor(c)) return js_throw_type(ctx, "object.constructor[Symbol.species] is not a constructor");
    JSValue nv = js_mknum((double)n);
    return js_construct_internal(ctx, c, 1, &nv, c);
}

/* ==== Array ================================================================================ */
FN(arr_ctor) {
    JSValue nt = js_new_target(ctx);
    JSObject *proto = ctx->proto_array;
    if (!JS_IsUndefined(nt) && nt != JS_MKOBJ(ctx->ctor_array)) {
        JSValue p = js_getp(ctx, nt, ctx->a[A_prototype]);
        if (JS_IsException(p)) return p;
        if (JS_IsObject(p)) proto = OBJ(p);
    }
    JSValue a;
    if (argc == 1 && JS_IsNumber(argv[0])) {
        double d = js_num(argv[0]);
        uint32_t n = js_d2u32(d);
        if ((double)n != d) return js_throw_range(ctx, "Invalid array length");
        a = js_array_new(ctx, n);
    } else a = js_array_from_list(ctx, argv, (uint32_t)argc);
    if (!JS_IsException(a)) OBJ(a)->proto = proto;
    return a;
}

FN(arr_is_array) {
    int r = js_is_array_v(ctx, A(0));
    return r < 0 ? JS_EXCEPTION : JS_BOOL(r);
}

FN(arr_of) {
    JSValue c = this_val;
    JSValue a;
    if (js_is_ctor(c) && c != JS_MKOBJ(ctx->ctor_array)) {
        JSValue nv = js_mknum(argc);
        a = js_construct_internal(ctx, c, 1, &nv, c);
    } else a = js_array_new(ctx, 0);
    if (JS_IsException(a)) return a;
    for (int i = 0; i < argc; i++) if (create_i(ctx, a, i, argv[i]) < 0) return JS_EXCEPTION;
    if (set_len(ctx, a, argc) < 0) return JS_EXCEPTION;
    return a;
}

FN(arr_from) {
    JSValue items = A(0), mapfn = A(1), thisarg = A(2);
    bool mapping = !JS_IsUndefined(mapfn);
    if (mapping && !js_is_callable(mapfn)) return js_throw_type(ctx, "Array.from: when provided, the second argument must be a function");
    JSValue c = this_val;
    bool use_ctor = js_is_ctor(c) && c != JS_MKOBJ(ctx->ctor_array);
    JSValue m = js_get_method(ctx, items, ctx->wk[WK_ITERATOR]);
    if (JS_IsException(m)) return m;
    JSValue a;
    if (!JS_IsUndefined(m)) {
        a = use_ctor ? js_construct_internal(ctx, c, 0, NULL, c) : js_array_new(ctx, 0);
        if (JS_IsException(a)) return a;
        JSValue nx;
        JSValue it = js_get_iterator(ctx, items, &nx, false);
        if (JS_IsException(it)) return it;
        int64_t k = 0;
        for (;; k++) {
            bool done;
            JSValue v = js_iter_step(ctx, it, nx, &done);
            if (JS_IsException(v)) return v;
            if (done) break;
            if (mapping) {
                JSValue args[2] = { v, js_mknum((double)k) };
                v = js_callv(ctx, mapfn, thisarg, 2, args);
                if (JS_IsException(v)) { js_iter_close(ctx, it, true); return v; }
            }
            if (!use_ctor && js_fast_array(a)) { if (js_array_push_v(ctx, a, v) < 0) return JS_EXCEPTION; }
            else if (create_i(ctx, a, k, v) < 0) { js_iter_close(ctx, it, true); return JS_EXCEPTION; }
        }
        if (set_len(ctx, a, k) < 0) return JS_EXCEPTION;
        return a;
    }
    JSValue o = js_toobj(ctx, items);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o);
    if (len < 0) return JS_EXCEPTION;
    if (use_ctor) { JSValue nv = js_mknum((double)len); a = js_construct_internal(ctx, c, 1, &nv, c); }
    else a = js_array_new(ctx, 0);
    if (JS_IsException(a)) return a;
    for (int64_t k = 0; k < len; k++) {
        JSValue v = get_i(ctx, o, k);
        if (JS_IsException(v)) return v;
        if (mapping) {
            JSValue args[2] = { v, js_mknum((double)k) };
            v = js_callv(ctx, mapfn, thisarg, 2, args);
            if (JS_IsException(v)) return v;
        }
        if (create_i(ctx, a, k, v) < 0) return JS_EXCEPTION;
    }
    if (set_len(ctx, a, len) < 0) return JS_EXCEPTION;
    return a;
}

FN(arr_push) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    if (js_fast_array(o) && (OBJ(o)->h.flags & OF_EXT) && !(OBJ(o)->h.flags & (OF_FROZEN | OF_LENRO)) && OBJ(o)->u.a.len <= OBJ(o)->u.a.cap) {
        JSObject *a = OBJ(o);
        if (js_array_ensure(ctx, a, a->u.a.len + (uint32_t)argc) < 0) return JS_EXCEPTION;
        a = OBJ(o);
        for (int i = 0; i < argc; i++) a->u.a.v[a->u.a.len++] = argv[i];
        return js_mknum(a->u.a.len);
    }
    int64_t len = js_length_of(ctx, o);
    if (len < 0) return JS_EXCEPTION;
    if (len + argc > 9007199254740991ll) return js_throw_type(ctx, "Pushing would exceed the maximum array length");
    for (int i = 0; i < argc; i++) if (set_i(ctx, o, len + i, argv[i]) < 0) return JS_EXCEPTION;
    if (set_len(ctx, o, len + argc) < 0) return JS_EXCEPTION;
    return js_mknum((double)(len + argc));
}

FN(arr_pop) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    if (js_fast_array(o) && !(OBJ(o)->h.flags & (OF_SEALED | OF_LENRO)) && OBJ(o)->u.a.len <= OBJ(o)->u.a.cap && OBJ(o)->u.a.len) {
        JSObject *a = OBJ(o);
        JSValue v = a->u.a.v[a->u.a.len - 1];
        if (v != JS_HOLE) { a->u.a.v[--a->u.a.len] = JS_HOLE; return v; }
    }
    int64_t len = js_length_of(ctx, o);
    if (len < 0) return JS_EXCEPTION;
    if (len == 0) { if (set_len(ctx, o, 0) < 0) return JS_EXCEPTION; return JS_UNDEFINED; }
    JSValue v = get_i(ctx, o, len - 1);
    if (JS_IsException(v)) return v;
    if (del_i(ctx, o, len - 1) < 0 || set_len(ctx, o, len - 1) < 0) return JS_EXCEPTION;
    return v;
}

FN(arr_shift) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    if (js_fast_array(o) && !(OBJ(o)->h.flags & (OF_SEALED | OF_LENRO)) && OBJ(o)->u.a.len <= OBJ(o)->u.a.cap && OBJ(o)->u.a.len) {
        JSObject *a = OBJ(o);
        bool holes = false;
        for (uint32_t i = 0; i < a->u.a.len; i++) if (a->u.a.v[i] == JS_HOLE) { holes = true; break; }
        if (!holes) {
            JSValue v = a->u.a.v[0];
            memmove(a->u.a.v, a->u.a.v + 1, sizeof(JSValue) * (a->u.a.len - 1));
            a->u.a.v[--a->u.a.len] = JS_HOLE;
            return v;
        }
    }
    int64_t len = js_length_of(ctx, o);
    if (len < 0) return JS_EXCEPTION;
    if (len == 0) { if (set_len(ctx, o, 0) < 0) return JS_EXCEPTION; return JS_UNDEFINED; }
    JSValue first = get_i(ctx, o, 0);
    if (JS_IsException(first)) return first;
    for (int64_t k = 1; k < len; k++) {
        int h = has_i(ctx, o, k);
        if (h < 0) return JS_EXCEPTION;
        if (h) {
            JSValue v = get_i(ctx, o, k);
            if (JS_IsException(v) || set_i(ctx, o, k - 1, v) < 0) return JS_EXCEPTION;
        } else if (del_i(ctx, o, k - 1) < 0) return JS_EXCEPTION;
    }
    if (del_i(ctx, o, len - 1) < 0 || set_len(ctx, o, len - 1) < 0) return JS_EXCEPTION;
    return first;
}

FN(arr_unshift) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o);
    if (len < 0) return JS_EXCEPTION;
    if (argc > 0) {
        if (js_fast_array(o) && (OBJ(o)->h.flags & OF_EXT) && !(OBJ(o)->h.flags & (OF_FROZEN | OF_SEALED | OF_LENRO)) && OBJ(o)->u.a.len <= OBJ(o)->u.a.cap) {
            JSObject *a = OBJ(o);
            if (js_array_ensure(ctx, a, a->u.a.len + (uint32_t)argc) < 0) return JS_EXCEPTION;
            a = OBJ(o);
            memmove(a->u.a.v + argc, a->u.a.v, sizeof(JSValue) * a->u.a.len);
            for (int i = 0; i < argc; i++) a->u.a.v[i] = argv[i];
            a->u.a.len += (uint32_t)argc;
            return js_mknum(a->u.a.len);
        }
        for (int64_t k = len; k > 0; k--) {
            int h = has_i(ctx, o, k - 1);
            if (h < 0) return JS_EXCEPTION;
            if (h) {
                JSValue v = get_i(ctx, o, k - 1);
                if (JS_IsException(v) || set_i(ctx, o, k - 1 + argc, v) < 0) return JS_EXCEPTION;
            } else if (del_i(ctx, o, k - 1 + argc) < 0) return JS_EXCEPTION;
        }
        for (int i = 0; i < argc; i++) if (set_i(ctx, o, i, argv[i]) < 0) return JS_EXCEPTION;
    }
    if (set_len(ctx, o, len + argc) < 0) return JS_EXCEPTION;
    return js_mknum((double)(len + argc));
}

FN(arr_slice) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o), s, e;
    if (len < 0 || rel_index(ctx, A(0), len, 0, &s) < 0 || rel_index(ctx, A(1), len, len, &e) < 0) return JS_EXCEPTION;
    int64_t n = e > s ? e - s : 0;
    if (js_fast_array(o) && OBJ(o)->proto == ctx->proto_array && !js_find_prop(ctx, OBJ(o), ctx->a[A_constructor])) {
        JSObject *a = OBJ(o);
        JSValue r = js_array_new(ctx, 0);
        if (JS_IsException(r)) return r;
        if (n && js_array_ensure(ctx, OBJ(r), (uint32_t)n) < 0) return JS_EXCEPTION;
        a = OBJ(o);
        for (int64_t i = 0; i < n; i++) {
            JSValue v = (s + i) < a->u.a.cap ? a->u.a.v[s + i] : JS_HOLE;
            OBJ(r)->u.a.v[i] = v;
        }
        OBJ(r)->u.a.len = (uint32_t)n;
        return r;
    }
    JSValue r = array_species_create(ctx, o, n);
    if (JS_IsException(r)) return r;
    int64_t k = 0;
    for (int64_t i = s; i < e; i++, k++) {
        int h = has_i(ctx, o, i);
        if (h < 0) return JS_EXCEPTION;
        if (!h) continue;
        JSValue v = get_i(ctx, o, i);
        if (JS_IsException(v) || create_i(ctx, r, k, v) < 0) return JS_EXCEPTION;
    }
    if (set_len(ctx, r, k) < 0) return JS_EXCEPTION;
    return r;
}

FN(arr_splice) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o), start, dc;
    if (len < 0 || rel_index(ctx, A(0), len, 0, &start) < 0) return JS_EXCEPTION;
    int ins = argc > 2 ? argc - 2 : 0;
    if (argc == 0) dc = 0;
    else if (argc == 1) dc = len - start;
    else {
        bool e = false;
        double d = js_tointeger(ctx, argv[1], &e);
        if (e) return JS_EXCEPTION;
        dc = d < 0 ? 0 : d > (double)(len - start) ? len - start : (int64_t)d;
    }
    JSValue removed = array_species_create(ctx, o, dc);
    if (JS_IsException(removed)) return removed;
    for (int64_t k = 0; k < dc; k++) {
        int h = has_i(ctx, o, start + k);
        if (h < 0) return JS_EXCEPTION;
        if (!h) continue;
        JSValue v = get_i(ctx, o, start + k);
        if (JS_IsException(v) || create_i(ctx, removed, k, v) < 0) return JS_EXCEPTION;
    }
    if (set_len(ctx, removed, dc) < 0) return JS_EXCEPTION;
    if (js_fast_array(o) && (OBJ(o)->h.flags & OF_EXT) && !(OBJ(o)->h.flags & (OF_FROZEN | OF_SEALED | OF_LENRO)) && OBJ(o)->u.a.len <= OBJ(o)->u.a.cap) {
        JSObject *a = OBJ(o);
        uint32_t nl = (uint32_t)(len - dc + ins);
        if (ins > dc && js_array_ensure(ctx, a, nl) < 0) return JS_EXCEPTION;
        a = OBJ(o);
        memmove(a->u.a.v + start + ins, a->u.a.v + start + dc, sizeof(JSValue) * (size_t)(len - start - dc));
        for (int i = 0; i < ins; i++) a->u.a.v[start + i] = argv[2 + i];
        for (uint32_t i = nl; i < (uint32_t)len; i++) a->u.a.v[i] = JS_HOLE;
        a->u.a.len = nl;
        return removed;
    }
    if (ins < dc) {
        for (int64_t k = start; k < len - dc; k++) {
            int h = has_i(ctx, o, k + dc);
            if (h < 0) return JS_EXCEPTION;
            if (h) { JSValue v = get_i(ctx, o, k + dc); if (JS_IsException(v) || set_i(ctx, o, k + ins, v) < 0) return JS_EXCEPTION; }
            else if (del_i(ctx, o, k + ins) < 0) return JS_EXCEPTION;
        }
        for (int64_t k = len; k > len - dc + ins; k--) if (del_i(ctx, o, k - 1) < 0) return JS_EXCEPTION;
    } else if (ins > dc) {
        for (int64_t k = len - dc; k > start; k--) {
            int h = has_i(ctx, o, k + dc - 1);
            if (h < 0) return JS_EXCEPTION;
            if (h) { JSValue v = get_i(ctx, o, k + dc - 1); if (JS_IsException(v) || set_i(ctx, o, k + ins - 1, v) < 0) return JS_EXCEPTION; }
            else if (del_i(ctx, o, k + ins - 1) < 0) return JS_EXCEPTION;
        }
    }
    for (int i = 0; i < ins; i++) if (set_i(ctx, o, start + i, argv[2 + i]) < 0) return JS_EXCEPTION;
    if (set_len(ctx, o, len - dc + ins) < 0) return JS_EXCEPTION;
    return removed;
}

static int concat_spreadable(JSContext *ctx, JSValue v) {
    if (!JS_IsObject(v)) return 0;
    JSValue s = js_getp(ctx, v, ctx->wk[WK_IS_CONCAT_SPREADABLE]);
    if (JS_IsException(s)) return -1;
    if (!JS_IsUndefined(s)) return js_truthy(s);
    return js_is_array_v(ctx, v);
}

FN(arr_concat) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    JSValue r = array_species_create(ctx, o, 0);
    if (JS_IsException(r)) return r;
    int64_t n = 0;
    for (int i = -1; i < argc; i++) {
        JSValue e = i < 0 ? o : argv[i];
        int sp = concat_spreadable(ctx, e);
        if (sp < 0) return JS_EXCEPTION;
        if (sp) {
            int64_t len = js_length_of(ctx, e);
            if (len < 0) return JS_EXCEPTION;
            if (js_fast_array(e) && js_fast_array(r)) {
                JSObject *s = OBJ(e);
                if (js_array_ensure(ctx, OBJ(r), (uint32_t)(n + len)) < 0) return JS_EXCEPTION;
                s = OBJ(e);
                for (int64_t k = 0; k < len; k++) OBJ(r)->u.a.v[n + k] = k < s->u.a.cap ? s->u.a.v[k] : JS_HOLE;
                n += len;
                OBJ(r)->u.a.len = (uint32_t)n;
                continue;
            }
            for (int64_t k = 0; k < len; k++, n++) {
                int h = has_i(ctx, e, k);
                if (h < 0) return JS_EXCEPTION;
                if (!h) continue;
                JSValue v = get_i(ctx, e, k);
                if (JS_IsException(v) || create_i(ctx, r, n, v) < 0) return JS_EXCEPTION;
            }
        } else {
            if (create_i(ctx, r, n, e) < 0) return JS_EXCEPTION;
            n++;
        }
    }
    if (set_len(ctx, r, n) < 0) return JS_EXCEPTION;
    return r;
}

FN(arr_join) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o);
    if (len < 0) return JS_EXCEPTION;
    JSValue sep;
    if (magic == 1 || JS_IsUndefined(A(0))) sep = js_strc(ctx, ",");
    else { sep = js_tostr(ctx, A(0)); if (JS_IsException(sep)) return sep; }
    /* cycle check: a nested join of the same array gives "" */
    for (int i = 0; i < ctx->njoin; i++) if (ctx->join_stack[i] == OBJ(o)) return ctx->empty_str;
    if (ctx->njoin >= 64) return js_throw_range(ctx, "Maximum call stack size exceeded");
    ctx->join_stack[ctx->njoin++] = OBJ(o);
    JSSB b;
    sb_init(ctx, &b);
    JSValue r = JS_UNDEFINED;
    for (int64_t k = 0; k < len; k++) {
        if (k) sb_putv(&b, sep);
        JSValue v = get_i(ctx, o, k);
        if (JS_IsException(v)) { r = v; break; }
        if (JS_IsNullish(v)) continue;
        if (magic == 1) {   /* toLocaleString */
            v = js_invoke(ctx, v, js_atomc(ctx, "toLocaleString"), 0, NULL);
            if (JS_IsException(v)) { r = v; break; }
        }
        JSValue s = js_tostr(ctx, v);
        if (JS_IsException(s)) { r = s; break; }
        sb_putv(&b, s);
        if (b.err) { r = JS_EXCEPTION; break; }
    }
    ctx->njoin--;
    if (JS_IsException(r)) { sb_free(&b); return r; }
    return sb_finish(&b);
}

FN(arr_to_string) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    JSValue j = js_getp(ctx, o, ctx->a[A_join]);
    if (JS_IsException(j)) return j;
    if (!js_is_callable(j)) return js_callv(ctx, ctx->obj_to_string, o, 0, NULL);
    return js_callv(ctx, j, o, 0, NULL);
}

FN(arr_reverse) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o);
    if (len < 0) return JS_EXCEPTION;
    if (js_fast_array(o) && !(OBJ(o)->h.flags & OF_FROZEN) && OBJ(o)->u.a.len <= OBJ(o)->u.a.cap) {
        JSObject *a = OBJ(o);
        bool holes = false;
        for (uint32_t i = 0; i < a->u.a.len; i++) if (a->u.a.v[i] == JS_HOLE) { holes = true; break; }
        if (!holes) {
            for (uint32_t i = 0, j = a->u.a.len; i + 1 < j; i++, j--) { JSValue t = a->u.a.v[i]; a->u.a.v[i] = a->u.a.v[j - 1]; a->u.a.v[j - 1] = t; }
            return o;
        }
    }
    for (int64_t lo = 0, hi = len - 1; lo < hi; lo++, hi--) {
        int hl = has_i(ctx, o, lo), hh = has_i(ctx, o, hi);
        if (hl < 0 || hh < 0) return JS_EXCEPTION;
        JSValue lv = JS_UNDEFINED, hv = JS_UNDEFINED;
        if (hl) { lv = get_i(ctx, o, lo); if (JS_IsException(lv)) return lv; }
        if (hh) { hv = get_i(ctx, o, hi); if (JS_IsException(hv)) return hv; }
        if (hh) { if (set_i(ctx, o, lo, hv) < 0) return JS_EXCEPTION; } else if (del_i(ctx, o, lo) < 0) return JS_EXCEPTION;
        if (hl) { if (set_i(ctx, o, hi, lv) < 0) return JS_EXCEPTION; } else if (del_i(ctx, o, hi) < 0) return JS_EXCEPTION;
    }
    return o;
}

/* indexOf (0) / lastIndexOf (1) / includes (2) */
FN(arr_index_of) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o);
    if (len < 0) return JS_EXCEPTION;
    if (len == 0) return magic == 2 ? JS_FALSE : js_mknum(-1);
    JSValue x = A(0);
    if (magic == 1) {
        int64_t k = len - 1;
        if (argc > 1) {
            bool e = false;
            double d = js_tointeger(ctx, argv[1], &e);
            if (e) return JS_EXCEPTION;
            if (d < 0) d += (double)len;
            if (d < 0) return js_mknum(-1);
            k = d >= (double)len ? len - 1 : (int64_t)d;
        }
        for (; k >= 0; k--) {
            int h = has_i(ctx, o, k);
            if (h < 0) return JS_EXCEPTION;
            if (!h) continue;
            JSValue v = get_i(ctx, o, k);
            if (JS_IsException(v)) return v;
            if (js_strict_eq(ctx, v, x)) return js_mknum((double)k);
        }
        return js_mknum(-1);
    }
    int64_t k = 0;
    if (argc > 1) {
        bool e = false;
        double d = js_tointeger(ctx, argv[1], &e);
        if (e) return JS_EXCEPTION;
        if (d < 0) { d += (double)len; if (d < 0) d = 0; }
        if (d >= (double)len) return magic == 2 ? JS_FALSE : js_mknum(-1);
        k = (int64_t)d;
    }
    if (js_fast_array(o)) {
        JSObject *a = OBJ(o);
        for (; k < len && k < a->u.a.cap; k++) {
            JSValue v = a->u.a.v[k];
            if (v == JS_HOLE) { if (magic == 2 && JS_IsUndefined(x)) return JS_TRUE; continue; }
            if (magic == 2 ? (js_same_value(v, x, true) || (JS_IsString(v) && JS_IsString(x) && js_streq(ctx, v, x))) : js_strict_eq(ctx, v, x))
                return magic == 2 ? JS_TRUE : js_mknum((double)k);
        }
        if (magic == 2 && JS_IsUndefined(x) && k < len) return JS_TRUE;
        return magic == 2 ? JS_FALSE : js_mknum(-1);
    }
    for (; k < len; k++) {
        if (magic != 2) {
            int h = has_i(ctx, o, k);
            if (h < 0) return JS_EXCEPTION;
            if (!h) continue;
        }
        JSValue v = get_i(ctx, o, k);
        if (JS_IsException(v)) return v;
        if (magic == 2 ? (js_same_value(v, x, true) || (JS_IsString(v) && JS_IsString(x) && js_streq(ctx, v, x))) : js_strict_eq(ctx, v, x))
            return magic == 2 ? JS_TRUE : js_mknum((double)k);
    }
    return magic == 2 ? JS_FALSE : js_mknum(-1);
}

/* forEach 0, map 1, filter 2, some 3, every 4, find 5, findIndex 6, findLast 7, findLastIndex 8, flatMap 9 */
static int flatten_into(JSContext *ctx, JSValue target, JSValue src, int64_t len, int64_t *n, double depth, JSValue fn, JSValue thisarg);

FN(arr_iterate) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o);
    if (len < 0) return JS_EXCEPTION;
    JSValue fn = A(0), thisarg = A(1);
    if (!js_is_callable(fn)) {
        char d[64];
        js_value_desc(ctx, fn, d, sizeof d);
        return js_throw_type(ctx, "%s is not a function", d);
    }
    JSValue r = JS_UNDEFINED;
    int64_t rn = 0;
    if (magic == 1) { r = array_species_create(ctx, o, len); if (JS_IsException(r)) return r; }
    else if (magic == 2 || magic == 9) { r = array_species_create(ctx, o, 0); if (JS_IsException(r)) return r; }
    bool back = magic == 7 || magic == 8;
    bool skip_holes = magic <= 4 || magic == 9;
    for (int64_t i = 0; i < len; i++) {
        int64_t k = back ? len - 1 - i : i;
        JSValue v;
        if (js_fast_array(o) && k < OBJ(o)->u.a.cap && OBJ(o)->u.a.v[k] != JS_HOLE && k < OBJ(o)->u.a.len) v = OBJ(o)->u.a.v[k];
        else {
            if (skip_holes) {
                int h = has_i(ctx, o, k);
                if (h < 0) return JS_EXCEPTION;
                if (!h) continue;
            }
            v = get_i(ctx, o, k);
            if (JS_IsException(v)) return v;
        }
        JSValue args[3] = { v, js_mknum((double)k), o };
        JSValue res = js_callv(ctx, fn, thisarg, 3, args);
        if (JS_IsException(res)) return res;
        switch (magic) {
        case 1: if (create_i(ctx, r, k, res) < 0) return JS_EXCEPTION; break;
        case 2: if (js_truthy(res) && create_i(ctx, r, rn++, v) < 0) return JS_EXCEPTION; break;
        case 3: if (js_truthy(res)) return JS_TRUE; break;
        case 4: if (!js_truthy(res)) return JS_FALSE; break;
        case 5: case 7: if (js_truthy(res)) return v; break;
        case 6: case 8: if (js_truthy(res)) return js_mknum((double)k); break;
        case 9: {
            int sp = js_is_array_v(ctx, res);
            if (sp < 0) return JS_EXCEPTION;
            if (sp) {
                int64_t l2 = js_length_of(ctx, res);
                if (l2 < 0 || flatten_into(ctx, r, res, l2, &rn, 0, JS_UNDEFINED, JS_UNDEFINED) < 0) return JS_EXCEPTION;
            } else if (create_i(ctx, r, rn++, res) < 0) return JS_EXCEPTION;
            break;
        }
        default: break;
        }
    }
    switch (magic) {
    case 1: return r;
    case 2: case 9: if (set_len(ctx, r, rn) < 0) return JS_EXCEPTION; return r;
    case 3: return JS_FALSE;
    case 4: return JS_TRUE;
    case 6: case 8: return js_mknum(-1);
    default: return JS_UNDEFINED;
    }
}

static int flatten_into(JSContext *ctx, JSValue target, JSValue src, int64_t len, int64_t *n, double depth, JSValue fn, JSValue thisarg) {
    for (int64_t k = 0; k < len; k++) {
        int h = has_i(ctx, src, k);
        if (h < 0) return -1;
        if (!h) continue;
        JSValue v = get_i(ctx, src, k);
        if (JS_IsException(v)) return -1;
        int sp = depth > 0 ? js_is_array_v(ctx, v) : 0;
        if (sp < 0) return -1;
        if (sp) {
            int64_t l2 = js_length_of(ctx, v);
            if (l2 < 0 || flatten_into(ctx, target, v, l2, n, depth - 1, fn, thisarg) < 0) return -1;
        } else if (create_i(ctx, target, (*n)++, v) < 0) return -1;
    }
    return 0;
}

FN(arr_flat) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o);
    if (len < 0) return JS_EXCEPTION;
    double depth = 1;
    if (!JS_IsUndefined(A(0))) { bool e = false; depth = js_tointeger(ctx, A(0), &e); if (e) return JS_EXCEPTION; if (depth < 0) depth = 0; }
    JSValue r = array_species_create(ctx, o, 0);
    if (JS_IsException(r)) return r;
    int64_t n = 0;
    if (flatten_into(ctx, r, o, len, &n, depth, JS_UNDEFINED, JS_UNDEFINED) < 0) return JS_EXCEPTION;
    if (set_len(ctx, r, n) < 0) return JS_EXCEPTION;
    return r;
}

/* reduce (0) / reduceRight (1) */
FN(arr_reduce) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o);
    if (len < 0) return JS_EXCEPTION;
    JSValue fn = A(0);
    if (!js_is_callable(fn)) return js_throw_type(ctx, "reduce: callback is not a function");
    int64_t i = 0;
    JSValue acc = JS_UNDEFINED;
    if (argc >= 2) acc = argv[1];
    else {
        bool found = false;
        for (; i < len; i++) {
            int64_t k = magic ? len - 1 - i : i;
            int h = has_i(ctx, o, k);
            if (h < 0) return JS_EXCEPTION;
            if (h) { acc = get_i(ctx, o, k); if (JS_IsException(acc)) return acc; found = true; i++; break; }
        }
        if (!found) return js_throw_type(ctx, "Reduce of empty array with no initial value");
    }
    for (; i < len; i++) {
        int64_t k = magic ? len - 1 - i : i;
        int h = has_i(ctx, o, k);
        if (h < 0) return JS_EXCEPTION;
        if (!h) continue;
        JSValue v = get_i(ctx, o, k);
        if (JS_IsException(v)) return v;
        JSValue args[4] = { acc, v, js_mknum((double)k), o };
        acc = js_callv(ctx, fn, JS_UNDEFINED, 4, args);
        if (JS_IsException(acc)) return acc;
    }
    return acc;
}

FN(arr_fill) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o), s, e;
    if (len < 0 || rel_index(ctx, A(1), len, 0, &s) < 0 || rel_index(ctx, A(2), len, len, &e) < 0) return JS_EXCEPTION;
    for (int64_t k = s; k < e; k++) if (set_i(ctx, o, k, A(0)) < 0) return JS_EXCEPTION;
    return o;
}

FN(arr_copy_within) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o), to, from, fin;
    if (len < 0 || rel_index(ctx, A(0), len, 0, &to) < 0 || rel_index(ctx, A(1), len, 0, &from) < 0 ||
        rel_index(ctx, A(2), len, len, &fin) < 0) return JS_EXCEPTION;
    int64_t count = MIN(fin - from, len - to);
    int dir = 1;
    if (from < to && to < from + count) { dir = -1; from += count - 1; to += count - 1; }
    for (; count > 0; count--, from += dir, to += dir) {
        int h = has_i(ctx, o, from);
        if (h < 0) return JS_EXCEPTION;
        if (h) { JSValue v = get_i(ctx, o, from); if (JS_IsException(v) || set_i(ctx, o, to, v) < 0) return JS_EXCEPTION; }
        else if (del_i(ctx, o, to) < 0) return JS_EXCEPTION;
    }
    return o;
}

FN(arr_at) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o);
    if (len < 0) return JS_EXCEPTION;
    bool e = false;
    double d = js_tointeger(ctx, A(0), &e);
    if (e) return JS_EXCEPTION;
    if (d < 0) d += (double)len;
    if (d < 0 || d >= (double)len) return JS_UNDEFINED;
    return get_i(ctx, o, (int64_t)d);
}

/* ---- sorting (stable merge sort) ---- */
typedef struct SortCtx { JSContext *ctx; JSValue cmp; bool err; bool numeric_ta; } SortCtx;

static int sort_cmp(SortCtx *s, JSValue a, JSValue b) {
    JSContext *ctx = s->ctx;
    if (s->err) return 0;
    if (JS_IsUndefined(a)) return JS_IsUndefined(b) ? 0 : 1;
    if (JS_IsUndefined(b)) return -1;
    if (!JS_IsUndefined(s->cmp)) {
        JSValue args[2] = { a, b };
        JSValue r = js_callv(ctx, s->cmp, JS_UNDEFINED, 2, args);
        if (JS_IsException(r)) { s->err = true; return 0; }
        double d = JS_IsNumber(r) ? js_num(r) : js_tonum(ctx, r, &s->err);
        return d < 0 ? -1 : d > 0 ? 1 : 0;
    }
    if (s->numeric_ta || (JS_IsNumber(a) && JS_IsNumber(b) && s->numeric_ta)) {
        double x = js_num(a), y = js_num(b);
        if (x != x) return y != y ? 0 : 1;
        if (y != y) return -1;
        if (x < y) return -1;
        if (x > y) return 1;
        if (x == 0 && y == 0) return signbit(x) && !signbit(y) ? -1 : (!signbit(x) && signbit(y) ? 1 : 0);
        return 0;
    }
    if (JS_IsString(a) && JS_IsString(b)) return js_strcmp(ctx, a, b);
    JSValue sa = js_tostr(ctx, a);
    if (JS_IsException(sa)) { s->err = true; return 0; }
    JSValue sb = js_tostr(ctx, b);
    if (JS_IsException(sb)) { s->err = true; return 0; }
    return js_strcmp(ctx, sa, sb);
}

static void merge_sort(SortCtx *s, JSValue *v, JSValue *tmp, uint32_t n) {
    if (n < 2 || s->err) return;
    if (n <= 8) {
        for (uint32_t i = 1; i < n; i++) {
            JSValue x = v[i];
            uint32_t j = i;
            while (j > 0 && sort_cmp(s, v[j - 1], x) > 0) { v[j] = v[j - 1]; j--; }
            v[j] = x;
            if (s->err) return;
        }
        return;
    }
    uint32_t m = n / 2;
    merge_sort(s, v, tmp, m);
    merge_sort(s, v + m, tmp, n - m);
    if (s->err || sort_cmp(s, v[m - 1], v[m]) <= 0) return;
    memcpy(tmp, v, sizeof(JSValue) * m);
    uint32_t i = 0, j = m, k = 0;
    while (i < m && j < n) {
        if (sort_cmp(s, v[j], tmp[i]) < 0) v[k++] = v[j++];
        else v[k++] = tmp[i++];
        if (s->err) return;
    }
    while (i < m) v[k++] = tmp[i++];
}

/* sort the values in place; values are rooted through the result array */
static int sort_values(JSContext *ctx, JSValue holder, JSValue cmp, bool numeric) {
    uint32_t n = OBJ(holder)->u.a.len;
    /* the merge buffer is a JS array, so the collector sees values that are only there
       while a comparator runs */
    volatile JSValue tv = js_array_new(ctx, 0);
    if (JS_IsException(tv)) return -1;
    JSObject *t = OBJ(tv);
    if (js_array_ensure(ctx, t, n / 2 + 1) < 0) return -1;
    t->u.a.len = n / 2 + 1;
    SortCtx s = { ctx, cmp, false, numeric };
    merge_sort(&s, OBJ(holder)->u.a.v, t->u.a.v, n);
    t->u.a.len = 0;
    return s.err ? -1 : 0;
}

/* collect the defined values (holes skipped) into a new array */
static JSValue sort_collect(JSContext *ctx, JSValue o, int64_t len, int64_t *nholes) {
    JSValue h = js_array_new(ctx, 0);
    if (JS_IsException(h)) return h;
    *nholes = 0;
    for (int64_t k = 0; k < len; k++) {
        int has = has_i(ctx, o, k);
        if (has < 0) return JS_EXCEPTION;
        if (!has) { (*nholes)++; continue; }
        JSValue v = get_i(ctx, o, k);
        if (JS_IsException(v) || js_array_push_v(ctx, h, v) < 0) return JS_EXCEPTION;
    }
    return h;
}

FN(arr_sort) {
    JSValue cmp = A(0);
    if (!JS_IsUndefined(cmp) && !js_is_callable(cmp)) return js_throw_type(ctx, "The comparison function must be either a function or undefined");
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o), nholes;
    if (len < 0) return JS_EXCEPTION;
    JSValue h = sort_collect(ctx, o, len, &nholes);
    if (JS_IsException(h) || sort_values(ctx, h, cmp, false) < 0) return JS_EXCEPTION;
    uint32_t n = OBJ(h)->u.a.len;
    for (uint32_t i = 0; i < n; i++) if (set_i(ctx, o, i, OBJ(h)->u.a.v[i]) < 0) return JS_EXCEPTION;
    for (int64_t k = n; k < len; k++) if (del_i(ctx, o, k) < 0) return JS_EXCEPTION;
    return o;
}

FN(arr_to_sorted) {
    JSValue cmp = A(0);
    if (!JS_IsUndefined(cmp) && !js_is_callable(cmp)) return js_throw_type(ctx, "The comparison function must be either a function or undefined");
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o);
    if (len < 0) return JS_EXCEPTION;
    if (len > 0xFFFFFFFFll) return js_throw_range(ctx, "Invalid array length");
    JSValue h = js_array_new(ctx, 0);
    if (JS_IsException(h)) return h;
    for (int64_t k = 0; k < len; k++) {
        JSValue v = get_i(ctx, o, k);
        if (JS_IsException(v) || js_array_push_v(ctx, h, v) < 0) return JS_EXCEPTION;
    }
    if (sort_values(ctx, h, cmp, false) < 0) return JS_EXCEPTION;
    return h;
}

FN(arr_to_reversed) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o);
    if (len < 0) return JS_EXCEPTION;
    JSValue r = js_array_new(ctx, 0);
    if (JS_IsException(r)) return r;
    for (int64_t k = len - 1; k >= 0; k--) {
        JSValue v = get_i(ctx, o, k);
        if (JS_IsException(v) || js_array_push_v(ctx, r, v) < 0) return JS_EXCEPTION;
    }
    return r;
}

FN(arr_with) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o);
    if (len < 0) return JS_EXCEPTION;
    bool e = false;
    double d = js_tointeger(ctx, A(0), &e);
    if (e) return JS_EXCEPTION;
    if (d < 0) d += (double)len;
    if (d < 0 || d >= (double)len) return js_throw_range(ctx, "Invalid index");
    JSValue r = js_array_new(ctx, 0);
    if (JS_IsException(r)) return r;
    for (int64_t k = 0; k < len; k++) {
        JSValue v = k == (int64_t)d ? A(1) : get_i(ctx, o, k);
        if (JS_IsException(v) || js_array_push_v(ctx, r, v) < 0) return JS_EXCEPTION;
    }
    return r;
}

FN(arr_to_spliced) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int64_t len = js_length_of(ctx, o), start, skip;
    if (len < 0 || rel_index(ctx, A(0), len, 0, &start) < 0) return JS_EXCEPTION;
    if (argc == 0) skip = 0;
    else if (argc == 1) skip = len - start;
    else { bool e = false; double d = js_tointeger(ctx, argv[1], &e); if (e) return JS_EXCEPTION; skip = d < 0 ? 0 : d > (double)(len - start) ? len - start : (int64_t)d; }
    JSValue r = js_array_new(ctx, 0);
    if (JS_IsException(r)) return r;
    for (int64_t k = 0; k < start; k++) { JSValue v = get_i(ctx, o, k); if (JS_IsException(v) || js_array_push_v(ctx, r, v) < 0) return JS_EXCEPTION; }
    for (int i = 2; i < argc; i++) if (js_array_push_v(ctx, r, argv[i]) < 0) return JS_EXCEPTION;
    for (int64_t k = start + skip; k < len; k++) { JSValue v = get_i(ctx, o, k); if (JS_IsException(v) || js_array_push_v(ctx, r, v) < 0) return JS_EXCEPTION; }
    return r;
}

/* ==== iterators ================================================================================ */
JSValue js_new_iter(JSContext *ctx, JSObject *proto, JSValue target, int type, int kind) {
    JSObject *o = js_obj_alloc(ctx, type == ITT_FORIN ? CL_FORIN : CL_ITER, proto);
    if (!o) return JS_EXCEPTION;
    JSIter *it = js_mallocz(ctx, sizeof(JSIter));
    if (!it) return JS_EXCEPTION;
    it->target = target;
    it->type = (uint8_t)type;
    it->kind = (uint8_t)kind;
    it->extra = JS_UNDEFINED;
    o->u.p = it;
    return JS_MKOBJ(o);
}

JSValue js_iter_result(JSContext *ctx, JSValue v, bool done) {
    JSObject *o = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
    if (!o) return JS_EXCEPTION;
    if (!js_add_prop(ctx, o, ctx->a[A_value], v, P_W | P_E | P_C) || !js_add_prop(ctx, o, ctx->a[A_done], JS_BOOL(done), P_W | P_E | P_C))
        return JS_EXCEPTION;
    return JS_MKOBJ(o);
}

JSValue js_get_iterator(JSContext *ctx, JSValue obj, JSValue *next, bool async) {
    JSValue m;
    if (async) {
        m = js_get_method(ctx, obj, ctx->wk[WK_ASYNC_ITERATOR]);
        if (JS_IsException(m)) return m;
        if (JS_IsUndefined(m)) {
            /* CreateAsyncFromSyncIterator */
            JSValue snext;
            JSValue sit = js_get_iterator(ctx, obj, &snext, false);
            if (JS_IsException(sit)) return sit;
            JSValue w = js_new_iter(ctx, ctx->proto_async_from_sync, sit, ITT_ASYNC_FROM_SYNC, 0);
            if (JS_IsException(w)) return w;
            ((JSIter *)OBJ(w)->u.p)->extra = snext;
            JSValue nx = js_getp(ctx, w, ctx->a[A_next]);
            if (JS_IsException(nx)) return nx;
            *next = nx;
            return w;
        }
    } else {
        m = js_get_method(ctx, obj, ctx->wk[WK_ITERATOR]);
        if (JS_IsException(m)) return m;
    }
    if (JS_IsUndefined(m)) {
        char d[64];
        js_value_desc(ctx, obj, d, sizeof d);
        return js_throw_type(ctx, "%s is not iterable", d);
    }
    JSValue it = js_callv(ctx, m, obj, 0, NULL);
    if (JS_IsException(it)) return it;
    if (!JS_IsObject(it)) return js_throw_type(ctx, "Result of the Symbol.iterator method is not an object");
    JSValue nx = js_getp(ctx, it, ctx->a[A_next]);
    if (JS_IsException(nx)) return nx;
    *next = nx;
    return it;
}

JSValue js_iter_step(JSContext *ctx, JSValue it, JSValue next, bool *done) {
    JSValue r = js_callv(ctx, next, it, 0, NULL);
    if (JS_IsException(r)) return r;
    if (!JS_IsObject(r)) return js_throw_type(ctx, "Iterator result %s is not an object", JS_IsUndefined(r) ? "undefined" : "value");
    JSValue d = js_getp(ctx, r, ctx->a[A_done]);
    if (JS_IsException(d)) return d;
    if (js_truthy(d)) { *done = true; return JS_UNDEFINED; }
    *done = false;
    return js_getp(ctx, r, ctx->a[A_value]);
}

int js_iter_close(JSContext *ctx, JSValue it, bool throwing) {
    JSValue saved = ctx->exception;
    bool had = ctx->has_exc;
    JSValue m = js_get_method(ctx, it, ctx->a[A_return]);
    if (JS_IsException(m)) { if (throwing) { ctx->exception = saved; ctx->has_exc = had; return 0; } return -1; }
    if (JS_IsUndefined(m)) return 0;
    JSValue r = js_callv(ctx, m, it, 0, NULL);
    if (throwing) { ctx->exception = saved; ctx->has_exc = had; return 0; }
    if (JS_IsException(r)) return -1;
    if (!JS_IsObject(r)) { js_throw_type(ctx, "Iterator result is not an object"); return -1; }
    return 0;
}

JSValue js_iterable_to_list(JSContext *ctx, JSValue v) {
    JSValue out = js_array_new(ctx, 0);
    if (JS_IsException(out)) return out;
    JSValue nx;
    JSValue it = js_get_iterator(ctx, v, &nx, false);
    if (JS_IsException(it)) return it;
    for (;;) {
        bool done;
        JSValue x = js_iter_step(ctx, it, nx, &done);
        if (JS_IsException(x)) return x;
        if (done) break;
        if (js_array_push_v(ctx, out, x) < 0) return JS_EXCEPTION;
    }
    return out;
}

FN(iter_self) { return this_val; }

/* keys 0, values 1, entries 2 */
FN(arr_iter_make) {
    JSValue o = this_obj(ctx, this_val);
    if (JS_IsException(o)) return o;
    return js_new_iter(ctx, ctx->proto_array_iter, o, js_is_cls(o, CL_TYPEDARRAY) ? ITT_TYPED : ITT_ARRAY, magic);
}

FN(arr_iter_next) {
    if (!js_is_cls(this_val, CL_ITER)) return js_throw_type(ctx, "next method called on incompatible receiver");
    JSIter *it = OBJ(this_val)->u.p;
    if (it->type != ITT_ARRAY && it->type != ITT_TYPED) return js_throw_type(ctx, "next method called on incompatible receiver");
    if (it->done) return js_iter_result(ctx, JS_UNDEFINED, true);
    int64_t len;
    if (it->type == ITT_TYPED) len = js_ta_length(OBJ(it->target));
    else { len = js_length_of(ctx, it->target); if (len < 0) return JS_EXCEPTION; }
    if (it->idx >= len) { it->done = true; it->target = JS_UNDEFINED; return js_iter_result(ctx, JS_UNDEFINED, true); }
    uint32_t i = it->idx++;
    if (it->kind == IT_KEYS) return js_iter_result(ctx, js_mknum(i), false);
    JSValue v = js_geti(ctx, it->target, i);
    if (JS_IsException(v)) return v;
    if (it->kind == IT_VALUES) return js_iter_result(ctx, v, false);
    JSValue pair[2] = { js_mknum(i), v };
    JSValue p = js_array_from_list(ctx, pair, 2);
    if (JS_IsException(p)) return p;
    return js_iter_result(ctx, p, false);
}

/* ---- generators ---- */
FN(gen_method) {
    if (!js_is_cls(this_val, CL_GENERATOR)) return js_throw_type(ctx, "next method called on incompatible receiver");
    bool done;
    JSValue r = js_gen_resume(ctx, OBJ(this_val), A(0), magic, &done);
    if (JS_IsException(r)) return r;
    if (ctx->gen_raw) { ctx->gen_raw = false; return r; }
    return js_iter_result(ctx, r, done);
}

/* ---- async-from-sync iterators (for await over a sync iterable) ---- */
FN(afs_unwrap) { return js_iter_result(ctx, A(0), js_func_data(ctx) == JS_TRUE); }

static JSValue rejected(JSContext *ctx) {
    JSValue e = js_take_exception(ctx);
    if (ctx->killed) return js_throw(ctx, e);
    JSValue res, rej;
    JSValue p = js_promise_new(ctx, &res, &rej);
    if (JS_IsException(p)) return p;
    js_callv(ctx, rej, JS_UNDEFINED, 1, &e);
    return p;
}

/* next 0, return 1, throw 2 */
FN(afs_method) {
    if (!js_is_cls(this_val, CL_ITER) || ((JSIter *)OBJ(this_val)->u.p)->type != ITT_ASYNC_FROM_SYNC)
        return js_throw_type(ctx, "not an async-from-sync iterator");
    JSIter *it = OBJ(this_val)->u.p;
    JSValue r;
    if (magic == 0) r = js_callv(ctx, it->extra, it->target, argc ? 1 : 0, argv);
    else {
        JSValue m = js_get_method(ctx, it->target, ctx->a[magic == 1 ? A_return : A_throw]);
        if (JS_IsException(m)) return rejected(ctx);
        if (JS_IsUndefined(m)) {
            if (magic == 2) { js_throw_type(ctx, "The iterator does not provide a 'throw' method"); return rejected(ctx); }
            JSValue res = js_iter_result(ctx, A(0), true);
            if (JS_IsException(res)) return res;
            return js_promise_resolve_value(ctx, res);
        }
        r = js_callv(ctx, m, it->target, argc ? 1 : 0, argv);
    }
    if (JS_IsException(r)) return rejected(ctx);
    if (!JS_IsObject(r)) { js_throw_type(ctx, "Iterator result is not an object"); return rejected(ctx); }
    JSValue d = js_getp(ctx, r, ctx->a[A_done]);
    if (JS_IsException(d)) return rejected(ctx);
    JSValue v = js_getp(ctx, r, ctx->a[A_value]);
    if (JS_IsException(v)) return rejected(ctx);
    JSValue vp = js_promise_resolve_value(ctx, v);
    if (JS_IsException(vp)) return rejected(ctx);
    JSValue fn = js_mk_native(ctx, afs_unwrap, ctx->empty_str, 1, 0);
    if (JS_IsException(fn)) return fn;
    OBJ(fn)->u.n.data = JS_BOOL(js_truthy(d));
    return js_invoke(ctx, vp, ctx->a[A_then], 1, &fn);
}

/* ---- Iterator helpers (Iterator.prototype.toArray / forEach / map ...) ---- */
FN(iterp_to_array) {
    JSValue nx = js_getp(ctx, this_val, ctx->a[A_next]);
    if (JS_IsException(nx)) return nx;
    JSValue out = js_array_new(ctx, 0);
    if (JS_IsException(out)) return out;
    for (uint32_t k = 0;; k++) {
        bool done;
        JSValue v = js_iter_step(ctx, this_val, nx, &done);
        if (JS_IsException(v)) return v;
        if (done) break;
        if (magic == 1) {
            JSValue args[2] = { v, js_mknum(k) };
            JSValue r = js_callv(ctx, A(0), JS_UNDEFINED, 2, args);
            if (JS_IsException(r)) { js_iter_close(ctx, this_val, true); return r; }
            continue;
        }
        if (js_array_push_v(ctx, out, v) < 0) return JS_EXCEPTION;
    }
    return magic == 1 ? JS_UNDEFINED : out;
}

/* ==== ArrayBuffer ================================================================================ */
static JSValue new_buffer(JSContext *ctx, JSObject *proto, uint32_t len) {
    JSObject *o = js_obj_alloc(ctx, CL_ARRAYBUFFER, proto ? proto : ctx->proto_arraybuffer);
    if (!o) return JS_EXCEPTION;
    JSArrayBuffer *ab = js_mallocz(ctx, sizeof(JSArrayBuffer));
    if (!ab) return JS_EXCEPTION;
    o->u.p = ab;
    ab->data = js_mallocz(ctx, len ? len : 1);
    if (!ab->data) return JS_EXCEPTION;
    ab->len = len;
    return JS_MKOBJ(o);
}

FN(ab_ctor) {
    JSValue nt = js_new_target(ctx);
    if (JS_IsUndefined(nt)) return js_throw_type(ctx, "Constructor ArrayBuffer requires 'new'");
    double n;
    if (js_toindex(ctx, A(0), &n) < 0) return JS_EXCEPTION;
    if (n > 1e9) return js_throw_range(ctx, "Array buffer allocation failed");
    JSValue p = js_getp(ctx, nt, ctx->a[A_prototype]);
    if (JS_IsException(p)) return p;
    return new_buffer(ctx, JS_IsObject(p) ? OBJ(p) : NULL, (uint32_t)n);
}

FN(ab_is_view) { return JS_BOOL(js_is_cls(A(0), CL_TYPEDARRAY) || js_is_cls(A(0), CL_DATAVIEW)); }

FN(ab_byte_length) {
    if (!js_is_cls(this_val, CL_ARRAYBUFFER)) return js_throw_type(ctx, "not an ArrayBuffer");
    JSArrayBuffer *ab = OBJ(this_val)->u.p;
    return js_mknum(ab->detached ? 0 : ab->len);
}

FN(ab_slice) {
    if (!js_is_cls(this_val, CL_ARRAYBUFFER)) return js_throw_type(ctx, "not an ArrayBuffer");
    JSArrayBuffer *ab = OBJ(this_val)->u.p;
    int64_t len = ab->len, s, e;
    if (rel_index(ctx, A(0), len, 0, &s) < 0 || rel_index(ctx, A(1), len, len, &e) < 0) return JS_EXCEPTION;
    uint32_t n = e > s ? (uint32_t)(e - s) : 0;
    JSValue r = new_buffer(ctx, NULL, n);
    if (JS_IsException(r)) return r;
    ab = OBJ(this_val)->u.p;
    if (n) memcpy(((JSArrayBuffer *)OBJ(r)->u.p)->data, ab->data + s, n);
    return r;
}

/* ==== typed arrays ================================================================================== */
static const char *ta_names[TA_COUNT] = { "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array",
                                          "Int32Array", "Uint32Array", "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array" };

static JSValue ta_create(JSContext *ctx, int type, JSObject *proto, JSValue buf, uint32_t off, uint32_t len) {
    JSObject *o = js_obj_alloc(ctx, CL_TYPEDARRAY, proto ? proto : ctx->proto_ta[type]);
    if (!o) return JS_EXCEPTION;
    JSTypedArray *ta = js_mallocz(ctx, sizeof(JSTypedArray));
    if (!ta) return JS_EXCEPTION;
    ta->buf = OBJ(buf);
    ta->off = off;
    ta->len = len;
    ta->type = (uint8_t)type;
    o->u.p = ta;
    return JS_MKOBJ(o);
}

static JSValue ta_new_len(JSContext *ctx, int type, JSObject *proto, uint32_t len) {
    uint64_t bytes = (uint64_t)len * (uint32_t)js_ta_elem_size(type);
    if (bytes > 1000000000ull) return js_throw_range(ctx, "Invalid typed array length");
    JSValue buf = new_buffer(ctx, NULL, (uint32_t)bytes);
    if (JS_IsException(buf)) return buf;
    return ta_create(ctx, type, proto, buf, 0, len);
}

FN(ta_ctor) {
    int type = magic;
    JSValue nt = js_new_target(ctx);
    if (JS_IsUndefined(nt)) return js_throw_type(ctx, "Constructor %s requires 'new'", ta_names[type]);
    JSObject *proto = ctx->proto_ta[type];
    JSValue p = js_getp(ctx, nt, ctx->a[A_prototype]);
    if (JS_IsException(p)) return p;
    if (JS_IsObject(p)) proto = OBJ(p);
    JSValue a0 = A(0);
    if (!JS_IsObject(a0)) {
        double n;
        if (js_toindex(ctx, a0, &n) < 0) return JS_EXCEPTION;
        if (n > 1e9) return js_throw_range(ctx, "Invalid typed array length");
        return ta_new_len(ctx, type, proto, (uint32_t)n);
    }
    uint32_t es = (uint32_t)js_ta_elem_size(type);
    if (js_is_cls(a0, CL_ARRAYBUFFER)) {
        JSArrayBuffer *ab = OBJ(a0)->u.p;
        double off;
        if (js_toindex(ctx, A(1), &off) < 0) return JS_EXCEPTION;
        if ((uint64_t)off % es) return js_throw_range(ctx, "start offset of %s should be a multiple of %u", ta_names[type], (unsigned)es);
        uint32_t len;
        if (JS_IsUndefined(A(2))) {
            if (ab->len % es) return js_throw_range(ctx, "byte length of %s should be a multiple of %u", ta_names[type], (unsigned)es);
            if (off > ab->len) return js_throw_range(ctx, "Start offset is outside the bounds of the buffer");
            len = (uint32_t)((ab->len - (uint32_t)off) / es);
        } else {
            double l;
            if (js_toindex(ctx, A(2), &l) < 0) return JS_EXCEPTION;
            if (off + l * es > ab->len) return js_throw_range(ctx, "Invalid typed array length");
            len = (uint32_t)l;
        }
        return ta_create(ctx, type, proto, a0, (uint32_t)off, len);
    }
    /* from a typed array, an iterable or an array-like */
    JSValue src = a0;
    if (!js_is_cls(src, CL_TYPEDARRAY)) {
        JSValue m = js_get_method(ctx, src, ctx->wk[WK_ITERATOR]);
        if (JS_IsException(m)) return m;
        if (!JS_IsUndefined(m) && !js_fast_array(src)) { src = js_iterable_to_list(ctx, src); if (JS_IsException(src)) return src; }
    }
    int64_t n = js_is_cls(src, CL_TYPEDARRAY) ? js_ta_length(OBJ(src)) : js_length_of(ctx, src);
    if (n < 0) return JS_EXCEPTION;
    JSValue r = ta_new_len(ctx, type, proto, (uint32_t)n);
    if (JS_IsException(r)) return r;
    for (int64_t k = 0; k < n; k++) {
        JSValue v = js_geti(ctx, src, (uint32_t)k);
        if (JS_IsException(v) || js_ta_set(ctx, OBJ(r), (uint32_t)k, v) < 0) return JS_EXCEPTION;
    }
    return r;
}

FN(ta_abstract) { return js_throw_type(ctx, "Abstract class TypedArray not directly constructable"); }

static JSObject *this_ta(JSContext *ctx, JSValue v) {
    if (!js_is_cls(v, CL_TYPEDARRAY)) { js_throw_type(ctx, "this is not a typed array."); return NULL; }
    return OBJ(v);
}

/* buffer 0, byteLength 1, byteOffset 2, length 3 */
FN(ta_getter) {
    JSObject *o = this_ta(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    JSTypedArray *ta = o->u.p;
    uint32_t len = js_ta_length(o);
    switch (magic) {
    case 0: return JS_MKOBJ(ta->buf);
    case 1: return js_mknum((double)len * js_ta_elem_size(ta->type));
    case 2: return js_mknum(len ? ta->off : 0);
    default: return js_mknum(len);
    }
}

FN(ta_tag) {
    if (!js_is_cls(this_val, CL_TYPEDARRAY)) return JS_UNDEFINED;
    return js_atomc(ctx, ta_names[((JSTypedArray *)OBJ(this_val)->u.p)->type]);
}

FN(ta_set_m) {
    JSObject *o = this_ta(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    double off = 0;
    if (!JS_IsUndefined(A(1))) { bool e = false; off = js_tointeger(ctx, A(1), &e); if (e) return JS_EXCEPTION; }
    if (off < 0) return js_throw_range(ctx, "offset is out of bounds");
    JSValue src = A(0);
    int64_t n = js_is_cls(src, CL_TYPEDARRAY) ? js_ta_length(OBJ(src)) : js_length_of(ctx, src);
    if (n < 0) return JS_EXCEPTION;
    uint32_t len = js_ta_length(o);
    if (off + (double)n > len) return js_throw_range(ctx, "offset is out of bounds");
    if (js_is_cls(src, CL_TYPEDARRAY) && ((JSTypedArray *)OBJ(src)->u.p)->type == ((JSTypedArray *)o->u.p)->type) {
        JSTypedArray *d = o->u.p, *s = OBJ(src)->u.p;
        uint32_t es = (uint32_t)js_ta_elem_size(d->type);
        memmove(((JSArrayBuffer *)d->buf->u.p)->data + d->off + (uint32_t)off * es, ((JSArrayBuffer *)s->buf->u.p)->data + s->off, (size_t)n * es);
        return JS_UNDEFINED;
    }
    for (int64_t k = 0; k < n; k++) {
        JSValue v = js_geti(ctx, src, (uint32_t)k);
        if (JS_IsException(v) || js_ta_set(ctx, o, (uint32_t)(off + k), v) < 0) return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

FN(ta_subarray) {
    JSObject *o = this_ta(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    JSTypedArray *ta = o->u.p;
    int64_t len = js_ta_length(o), s, e;
    if (rel_index(ctx, A(0), len, 0, &s) < 0 || rel_index(ctx, A(1), len, len, &e) < 0) return JS_EXCEPTION;
    uint32_t n = e > s ? (uint32_t)(e - s) : 0;
    return ta_create(ctx, ta->type, o->proto, JS_MKOBJ(ta->buf), ta->off + (uint32_t)s * (uint32_t)js_ta_elem_size(ta->type), n);
}

FN(ta_slice) {
    JSObject *o = this_ta(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    JSTypedArray *ta = o->u.p;
    int64_t len = js_ta_length(o), s, e;
    if (rel_index(ctx, A(0), len, 0, &s) < 0 || rel_index(ctx, A(1), len, len, &e) < 0) return JS_EXCEPTION;
    uint32_t n = e > s ? (uint32_t)(e - s) : 0;
    JSValue r = ta_new_len(ctx, ta->type, NULL, n);
    if (JS_IsException(r)) return r;
    uint32_t es = (uint32_t)js_ta_elem_size(ta->type);
    JSTypedArray *d = OBJ(r)->u.p;
    if (n) memcpy(((JSArrayBuffer *)d->buf->u.p)->data, ((JSArrayBuffer *)ta->buf->u.p)->data + ta->off + (uint32_t)s * es, (size_t)n * es);
    return r;
}

/* map 0, filter 1 */
FN(ta_map) {
    JSObject *o = this_ta(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    JSTypedArray *ta = o->u.p;
    if (!js_is_callable(A(0))) return js_throw_type(ctx, "callback is not a function");
    uint32_t len = js_ta_length(o);
    JSValue tmp = js_array_new(ctx, 0);
    if (JS_IsException(tmp)) return tmp;
    for (uint32_t k = 0; k < len; k++) {
        JSValue v = js_ta_get(ctx, o, k);
        JSValue args[3] = { v, js_mknum(k), this_val };
        JSValue r = js_callv(ctx, A(0), A(1), 3, args);
        if (JS_IsException(r)) return r;
        if (magic == 0) { if (js_array_push_v(ctx, tmp, r) < 0) return JS_EXCEPTION; }
        else if (js_truthy(r) && js_array_push_v(ctx, tmp, v) < 0) return JS_EXCEPTION;
    }
    uint32_t n = OBJ(tmp)->u.a.len;
    JSValue out = ta_new_len(ctx, ta->type, NULL, n);
    if (JS_IsException(out)) return out;
    for (uint32_t k = 0; k < n; k++) if (js_ta_set(ctx, OBJ(out), k, OBJ(tmp)->u.a.v[k]) < 0) return JS_EXCEPTION;
    return out;
}

FN(ta_sort) {
    JSObject *o = this_ta(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    JSValue cmp = A(0);
    if (!JS_IsUndefined(cmp) && !js_is_callable(cmp)) return js_throw_type(ctx, "The comparison function must be either a function or undefined");
    uint32_t len = js_ta_length(o);
    JSValue h = js_array_new(ctx, 0);
    if (JS_IsException(h)) return h;
    for (uint32_t k = 0; k < len; k++) if (js_array_push_v(ctx, h, js_ta_get(ctx, o, k)) < 0) return JS_EXCEPTION;
    if (sort_values(ctx, h, cmp, JS_IsUndefined(cmp)) < 0) return JS_EXCEPTION;
    JSValue target = this_val;
    if (magic == 1) {   /* toSorted */
        target = ta_new_len(ctx, ((JSTypedArray *)o->u.p)->type, NULL, len);
        if (JS_IsException(target)) return target;
    }
    for (uint32_t k = 0; k < len && k < OBJ(h)->u.a.len; k++) js_ta_set(ctx, OBJ(target), k, OBJ(h)->u.a.v[k]);
    return target;
}

FN(ta_from_of) {
    if (!js_is_ctor(this_val)) return js_throw_type(ctx, "this is not a constructor");
    JSValue list;
    if (magic) list = js_array_from_list(ctx, argv, (uint32_t)argc);
    else {
        JSValue args[3] = { A(0), A(1), A(2) };
        list = arr_from(ctx, JS_MKOBJ(ctx->ctor_array), 3, args, 0);
    }
    if (JS_IsException(list)) return list;
    return js_construct_internal(ctx, this_val, 1, &list, this_val);
}

/* ==== DataView ======================================================================================= */
FN(dv_ctor) {
    JSValue nt = js_new_target(ctx);
    if (JS_IsUndefined(nt)) return js_throw_type(ctx, "Constructor DataView requires 'new'");
    if (!js_is_cls(A(0), CL_ARRAYBUFFER)) return js_throw_type(ctx, "First argument to DataView constructor must be an ArrayBuffer");
    JSArrayBuffer *ab = OBJ(A(0))->u.p;
    double off, len;
    if (js_toindex(ctx, A(1), &off) < 0) return JS_EXCEPTION;
    if (off > ab->len) return js_throw_range(ctx, "Start offset is outside the bounds of the buffer");
    if (JS_IsUndefined(A(2))) len = ab->len - off;
    else { if (js_toindex(ctx, A(2), &len) < 0) return JS_EXCEPTION; if (off + len > ab->len) return js_throw_range(ctx, "Invalid DataView length"); }
    JSValue p = js_getp(ctx, nt, ctx->a[A_prototype]);
    if (JS_IsException(p)) return p;
    JSObject *o = js_obj_alloc(ctx, CL_DATAVIEW, JS_IsObject(p) ? OBJ(p) : ctx->proto_dataview);
    if (!o) return JS_EXCEPTION;
    JSTypedArray *ta = js_mallocz(ctx, sizeof(JSTypedArray));
    if (!ta) return JS_EXCEPTION;
    ta->buf = OBJ(A(0));
    ta->off = (uint32_t)off;
    ta->len = (uint32_t)len;
    o->u.p = ta;
    return JS_MKOBJ(o);
}

FN(dv_getter) {
    if (!js_is_cls(this_val, CL_DATAVIEW)) return js_throw_type(ctx, "not a DataView");
    JSTypedArray *ta = OBJ(this_val)->u.p;
    return magic == 0 ? JS_MKOBJ(ta->buf) : js_mknum(magic == 1 ? ta->len : ta->off);
}

/* magic: type | 0x100 for set */
FN(dv_access) {
    if (!js_is_cls(this_val, CL_DATAVIEW)) return js_throw_type(ctx, "not a DataView");
    JSTypedArray *ta = OBJ(this_val)->u.p;
    int type = magic & 0xFF;
    bool set = (magic & 0x100) != 0;
    double idx;
    if (js_toindex(ctx, A(0), &idx) < 0) return JS_EXCEPTION;
    uint32_t es = (uint32_t)js_ta_elem_size(type);
    JSValue le = set ? A(2) : A(1);
    double d = 0;
    if (set) { bool e = false; d = js_tonum(ctx, A(1), &e); if (e) return JS_EXCEPTION; }
    if (idx + es > ta->len) return js_throw_range(ctx, "Offset is outside the bounds of the DataView");
    uint8_t *p = ((JSArrayBuffer *)ta->buf->u.p)->data + ta->off + (uint32_t)idx;
    bool little = js_truthy(le);
    uint8_t b[8];
    if (set) {
        uint64_t u = 0;
        switch (type) {
        case TA_I8: case TA_U8: u = js_d2u32(d) & 0xFF; break;
        case TA_I16: case TA_U16: u = js_d2u32(d) & 0xFFFF; break;
        case TA_I32: case TA_U32: u = js_d2u32(d); break;
        case TA_F32: { float f = (float)d; uint32_t x; memcpy(&x, &f, 4); u = x; break; }
        case TA_F64: memcpy(&u, &d, 8); break;
        default: u = (uint64_t)(int64_t)d; break;
        }
        for (uint32_t i = 0; i < es; i++) b[i] = (uint8_t)(u >> (8 * i));
        for (uint32_t i = 0; i < es; i++) p[i] = little ? b[i] : b[es - 1 - i];
        return JS_UNDEFINED;
    }
    uint64_t u = 0;
    for (uint32_t i = 0; i < es; i++) u |= (uint64_t)(little ? p[i] : p[es - 1 - i]) << (8 * i);
    switch (type) {
    case TA_I8: return js_mknum((int8_t)u);
    case TA_U8: return js_mknum((uint8_t)u);
    case TA_I16: return js_mknum((int16_t)u);
    case TA_U16: return js_mknum((uint16_t)u);
    case TA_I32: return js_mknum((int32_t)u);
    case TA_U32: return js_mknum((uint32_t)u);
    case TA_F32: { uint32_t x = (uint32_t)u; float f; memcpy(&f, &x, 4); return js_mknum(f); }
    case TA_F64: { double x; memcpy(&x, &u, 8); return js_mknum(x); }
    default: return js_mknum((double)(int64_t)u);
    }
}

/* ==== public helpers ================================================================================== */
JSValue js_new_arraybuffer(JSContext *ctx, const uint8_t *data, size_t len) {
    JS_GC_ENTER(ctx);
    JSValue b = new_buffer(ctx, NULL, (uint32_t)len);
    if (!JS_IsException(b) && len) memcpy(((JSArrayBuffer *)OBJ(b)->u.p)->data, data, len);
    JS_GC_LEAVE(ctx);
    return b;
}

JSValue js_new_uint8array(JSContext *ctx, const uint8_t *data, size_t len) {
    JS_GC_ENTER(ctx);
    JSValue b = new_buffer(ctx, NULL, (uint32_t)len);
    JSValue r = b;
    if (!JS_IsException(b)) {
        if (len) memcpy(((JSArrayBuffer *)OBJ(b)->u.p)->data, data, len);
        r = ta_create(ctx, TA_U8, NULL, b, 0, (uint32_t)len);
    }
    JS_GC_LEAVE(ctx);
    return r;
}

uint8_t *js_get_bytes(JSContext *ctx, JSValue v, size_t *len) {
    (void)ctx;
    if (js_is_cls(v, CL_ARRAYBUFFER)) {
        JSArrayBuffer *ab = OBJ(v)->u.p;
        *len = ab->detached ? 0 : ab->len;
        return ab->data;
    }
    if (js_is_cls(v, CL_TYPEDARRAY) || js_is_cls(v, CL_DATAVIEW)) {
        JSTypedArray *ta = OBJ(v)->u.p;
        JSArrayBuffer *ab = ta->buf->u.p;
        *len = js_is_cls(v, CL_DATAVIEW) ? ta->len : (size_t)js_ta_length(OBJ(v)) * (size_t)js_ta_elem_size(ta->type);
        return ab->data + ta->off;
    }
    *len = 0;
    return NULL;
}

/* ==== setup ============================================================================================ */
static const JSFuncDef arrp_fns[] = {
    { "at", arr_at, 1, 0 }, { "concat", arr_concat, 1, 0 }, { "copyWithin", arr_copy_within, 2, 0 },
    { "every", arr_iterate, 1, 4 }, { "fill", arr_fill, 1, 0 }, { "filter", arr_iterate, 1, 2 },
    { "find", arr_iterate, 1, 5 }, { "findIndex", arr_iterate, 1, 6 }, { "findLast", arr_iterate, 1, 7 },
    { "findLastIndex", arr_iterate, 1, 8 }, { "flat", arr_flat, 0, 0 }, { "flatMap", arr_iterate, 1, 9 },
    { "forEach", arr_iterate, 1, 0 }, { "includes", arr_index_of, 1, 2 }, { "indexOf", arr_index_of, 1, 0 },
    { "join", arr_join, 1, 0 }, { "lastIndexOf", arr_index_of, 1, 1 }, { "map", arr_iterate, 1, 1 },
    { "pop", arr_pop, 0, 0 }, { "push", arr_push, 1, 0 }, { "reduce", arr_reduce, 1, 0 },
    { "reduceRight", arr_reduce, 1, 1 }, { "reverse", arr_reverse, 0, 0 }, { "shift", arr_shift, 0, 0 },
    { "slice", arr_slice, 2, 0 }, { "some", arr_iterate, 1, 3 }, { "sort", arr_sort, 1, 0 },
    { "splice", arr_splice, 2, 0 }, { "toLocaleString", arr_join, 0, 1 }, { "toString", arr_to_string, 0, 0 },
    { "unshift", arr_unshift, 1, 0 }, { "keys", arr_iter_make, 0, IT_KEYS }, { "entries", arr_iter_make, 0, IT_ENTRIES },
    { "toReversed", arr_to_reversed, 0, 0 }, { "toSorted", arr_to_sorted, 1, 0 }, { "toSpliced", arr_to_spliced, 2, 0 },
    { "with", arr_with, 2, 0 },
};
static const JSFuncDef tap_fns[] = {
    { "at", arr_at, 1, 0 }, { "copyWithin", arr_copy_within, 2, 0 }, { "every", arr_iterate, 1, 4 },
    { "fill", arr_fill, 1, 0 }, { "filter", ta_map, 1, 1 }, { "find", arr_iterate, 1, 5 }, { "findIndex", arr_iterate, 1, 6 },
    { "findLast", arr_iterate, 1, 7 }, { "findLastIndex", arr_iterate, 1, 8 }, { "forEach", arr_iterate, 1, 0 },
    { "includes", arr_index_of, 1, 2 }, { "indexOf", arr_index_of, 1, 0 }, { "join", arr_join, 1, 0 },
    { "lastIndexOf", arr_index_of, 1, 1 }, { "map", ta_map, 1, 0 }, { "reduce", arr_reduce, 1, 0 },
    { "reduceRight", arr_reduce, 1, 1 }, { "reverse", arr_reverse, 0, 0 }, { "set", ta_set_m, 1, 0 },
    { "slice", ta_slice, 2, 0 }, { "some", arr_iterate, 1, 3 }, { "sort", ta_sort, 1, 0 }, { "toSorted", ta_sort, 1, 1 },
    { "subarray", ta_subarray, 2, 0 }, { "toLocaleString", arr_join, 0, 1 }, { "keys", arr_iter_make, 0, IT_KEYS },
    { "entries", arr_iter_make, 0, IT_ENTRIES },
};

void js_init_lib_arr(JSContext *ctx) {
    /* %IteratorPrototype% */
    ctx->proto_iterator = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
    JSValue ip = JS_MKOBJ(ctx->proto_iterator);
    js_def_native(ctx, ip, "@iterator", iter_self, 0, 0);
    js_def_native(ctx, ip, "toArray", iterp_to_array, 0, 0);
    js_def_native(ctx, ip, "forEach", iterp_to_array, 1, 1);
    JSObject *itc = js_new_ctor(ctx, "Iterator", ta_abstract, 0, ctx->proto_iterator, 0);
    (void)itc;
    /* Array */
    ctx->proto_array = js_obj_alloc(ctx, CL_ARRAY, ctx->proto_object);
    JSObject *ac = js_new_ctor(ctx, "Array", arr_ctor, 1, ctx->proto_array, 0);
    ctx->ctor_array = ac;
    JSValue acv = JS_MKOBJ(ac);
    js_def_native(ctx, acv, "isArray", arr_is_array, 1, 0);
    js_def_native(ctx, acv, "of", arr_of, 0, 0);
    js_def_native(ctx, acv, "from", arr_from, 1, 0);
    js_def_accessor(ctx, acv, ctx->wk[WK_SPECIES], iter_self, NULL, 0, P_C);
    JSValue ap = JS_MKOBJ(ctx->proto_array);
    js_def_funcs(ctx, ap, arrp_fns, JS_COUNTOF(arrp_fns));
    JSValue values = js_def_native(ctx, ap, "values", arr_iter_make, 0, IT_VALUES);
    ctx->array_values = values;
    js_add_prop(ctx, ctx->proto_array, ctx->wk[WK_ITERATOR], values, P_W | P_C);
    JSValue unsc = js_obj_new(ctx, CL_OBJECT, NULL);
    static const char *un[] = { "at", "copyWithin", "entries", "fill", "find", "findIndex", "findLast", "findLastIndex",
                                "flat", "flatMap", "includes", "keys", "toReversed", "toSorted", "toSpliced", "values" };
    for (int i = 0; i < JS_COUNTOF(un); i++) js_add_prop(ctx, OBJ(unsc), js_atomc(ctx, un[i]), JS_TRUE, P_W | P_E | P_C);
    js_add_prop(ctx, ctx->proto_array, ctx->wk[WK_UNSCOPABLES], unsc, P_C);
    /* array iterator */
    ctx->proto_array_iter = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_iterator);
    ctx->array_iter_next = js_def_native(ctx, JS_MKOBJ(ctx->proto_array_iter), "next", arr_iter_next, 0, 0);
    js_add_prop(ctx, ctx->proto_array_iter, ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, "Array Iterator"), P_C);
    /* async-from-sync iterator */
    ctx->proto_async_from_sync = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_async_from_sync), "next", afs_method, 1, 0);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_async_from_sync), "return", afs_method, 1, 1);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_async_from_sync), "throw", afs_method, 1, 2);
    /* generators */
    ctx->proto_generator = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_iterator);
    JSValue gp = JS_MKOBJ(ctx->proto_generator);
    js_def_native(ctx, gp, "next", gen_method, 1, GR_NEXT);
    js_def_native(ctx, gp, "return", gen_method, 1, GR_RETURN);
    js_def_native(ctx, gp, "throw", gen_method, 1, GR_THROW);
    js_add_prop(ctx, ctx->proto_generator, ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, "Generator"), P_C);
    js_add_prop(ctx, ctx->proto_gen_func, ctx->a[A_prototype], gp, P_C);
    js_add_prop(ctx, ctx->proto_generator, ctx->a[A_constructor], JS_MKOBJ(ctx->proto_gen_func), P_C);
    /* ArrayBuffer */
    ctx->proto_arraybuffer = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
    JSObject *abc = js_new_ctor(ctx, "ArrayBuffer", ab_ctor, 1, ctx->proto_arraybuffer, 0);
    js_def_native(ctx, JS_MKOBJ(abc), "isView", ab_is_view, 1, 0);
    js_def_accessor(ctx, JS_MKOBJ(abc), ctx->wk[WK_SPECIES], iter_self, NULL, 0, P_C);
    JSValue abp = JS_MKOBJ(ctx->proto_arraybuffer);
    js_def_accessor(ctx, abp, js_atomc(ctx, "byteLength"), ab_byte_length, NULL, 0, P_C);
    js_def_native(ctx, abp, "slice", ab_slice, 2, 0);
    js_add_prop(ctx, ctx->proto_arraybuffer, ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, "ArrayBuffer"), P_C);
    /* %TypedArray% */
    ctx->proto_typed = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
    JSValue tp = JS_MKOBJ(ctx->proto_typed);
    JSValue tac = js_mk_native(ctx, ta_abstract, js_atomc(ctx, "TypedArray"), 0, 0);
    OBJ(tac)->h.flags |= OF_CTOR;
    js_materialize(ctx, OBJ(tac));
    js_add_prop(ctx, OBJ(tac), ctx->a[A_prototype], tp, 0);
    js_add_prop(ctx, ctx->proto_typed, ctx->a[A_constructor], tac, P_W | P_C);
    js_def_native(ctx, tac, "from", ta_from_of, 1, 0);
    js_def_native(ctx, tac, "of", ta_from_of, 0, 1);
    js_def_accessor(ctx, tac, ctx->wk[WK_SPECIES], iter_self, NULL, 0, P_C);
    js_def_funcs(ctx, tp, tap_fns, JS_COUNTOF(tap_fns));
    js_add_prop(ctx, ctx->proto_typed, js_atomc(ctx, "values"), values, P_W | P_C);
    js_add_prop(ctx, ctx->proto_typed, ctx->wk[WK_ITERATOR], values, P_W | P_C);
    JSProp *tsp = js_find_prop(ctx, ctx->proto_array, ctx->a[A_toString]);
    if (tsp) js_add_prop(ctx, ctx->proto_typed, ctx->a[A_toString], tsp->val, P_W | P_C);
    static const char *tg[4] = { "buffer", "byteLength", "byteOffset", "length" };
    for (int i = 0; i < 4; i++) js_def_accessor(ctx, tp, js_atomc(ctx, tg[i]), ta_getter, NULL, i, P_C);
    js_def_accessor(ctx, tp, ctx->wk[WK_TO_STRING_TAG], ta_tag, NULL, 0, P_C);
    for (int t = 0; t < TA_BI64; t++) {
        JSObject *p = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_typed);
        ctx->proto_ta[t] = p;
        JSObject *c = js_new_ctor(ctx, ta_names[t], ta_ctor, 3, p, t);
        c->proto = OBJ(tac);
        ctx->ctor_typed[t] = c;
        js_add_prop(ctx, c, js_atomc(ctx, "BYTES_PER_ELEMENT"), js_mknum(js_ta_elem_size(t)), 0);
        js_add_prop(ctx, p, js_atomc(ctx, "BYTES_PER_ELEMENT"), js_mknum(js_ta_elem_size(t)), 0);
    }
    /* DataView */
    ctx->proto_dataview = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
    js_new_ctor(ctx, "DataView", dv_ctor, 1, ctx->proto_dataview, 0);
    JSValue dp = JS_MKOBJ(ctx->proto_dataview);
    js_def_accessor(ctx, dp, js_atomc(ctx, "buffer"), dv_getter, NULL, 0, P_C);
    js_def_accessor(ctx, dp, js_atomc(ctx, "byteLength"), dv_getter, NULL, 1, P_C);
    js_def_accessor(ctx, dp, js_atomc(ctx, "byteOffset"), dv_getter, NULL, 2, P_C);
    static const char *dn[] = { "Int8", "Uint8", NULL, "Int16", "Uint16", "Int32", "Uint32", "Float32", "Float64" };
    for (int t = 0; t <= TA_F64; t++) {
        if (!dn[t]) continue;
        char nm[24];
        snprintf(nm, sizeof nm, "get%s", dn[t]);
        js_def_native(ctx, dp, nm, dv_access, 1, t);
        snprintf(nm, sizeof nm, "set%s", dn[t]);
        js_def_native(ctx, dp, nm, dv_access, 2, t | 0x100);
    }
    js_add_prop(ctx, ctx->proto_dataview, ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, "DataView"), P_C);
}
