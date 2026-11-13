/*
 * js_obj.c - NovaJS objects: property tables (linear for small objects, a
 * hash index for big ones; insertion order kept), fast arrays (dense
 * elements, holes, lengths past the allocation), the exotic objects (arrays,
 * strings, arguments, typed arrays, proxies, host hooks), the spec's internal
 * methods ([[Get]], [[Set]], [[DefineOwnProperty]], ...), conversions,
 * operators and errors.
 */
#include "jsi.h"

/* ==== property tables ===================================================================== */
static inline uint32_t key_hash(JSValue k) {
    uint64_t x = k ^ (k >> 29);
    return (uint32_t)((x * 0x9E3779B97F4A7C15ull) >> 32);
}

static void hidx_build(JSContext *ctx, JSObject *o) {
    uint32_t hs = 16;
    while (hs < o->nprops * 2 + 2) hs *= 2;
    if (hs != o->hsize) {
        uint32_t *n = js_mallocz(ctx, sizeof(uint32_t) * hs);
        if (!n) return;   /* (stays linear) */
        js_mfree(ctx, o->hidx, sizeof(uint32_t) * o->hsize);
        o->hidx = n;
        o->hsize = hs;
    } else memset(o->hidx, 0, sizeof(uint32_t) * hs);
    for (uint32_t i = 0; i < o->nprops; i++) {
        uint32_t k = key_hash(o->props[i].key) & (hs - 1);
        while (o->hidx[k]) k = (k + 1) & (hs - 1);
        o->hidx[k] = i + 1;
    }
}

static inline bool lazy_key(JSContext *ctx, JSValue key) {
    return key == ctx->a[A_length] || key == ctx->a[A_name] || key == ctx->a[A_prototype];
}

JSProp *js_find_prop(JSContext *ctx, JSObject *o, JSValue key) {
    if (JS_UNLIKELY(o->h.flags & OF_LAZY) && lazy_key(ctx, key)) js_materialize(ctx, o);
    if (o->hidx) {
        uint32_t m = o->hsize - 1, k = key_hash(key) & m;
        uint32_t e;
        while ((e = o->hidx[k])) {
            if (o->props[e - 1].key == key) return &o->props[e - 1];
            k = (k + 1) & m;
        }
        return NULL;
    }
    JSProp *p = o->props;
    for (uint32_t i = 0, n = o->nprops; i < n; i++) if (p[i].key == key) return &p[i];
    return NULL;
}

JSProp *js_add_prop(JSContext *ctx, JSObject *o, JSValue key, JSValue v, uint32_t flags) {
    if (JS_UNLIKELY(o->h.flags & OF_LAZY)) js_materialize(ctx, o);
    if (o->nprops >= o->pcap) {
        uint32_t nc = o->pcap ? o->pcap * 2 : 4;
        JSProp *n = js_realloc(ctx, o->props, sizeof(JSProp) * o->pcap, sizeof(JSProp) * nc);
        if (!n) return NULL;
        o->props = n;
        o->pcap = nc;
    }
    JSProp *p = &o->props[o->nprops++];
    p->key = key;
    p->val = v;
    p->flags = flags;
    if (o->hidx) {
        if (o->nprops * 2 + 2 > o->hsize) hidx_build(ctx, o);
        else {
            uint32_t m = o->hsize - 1, k = key_hash(key) & m;
            while (o->hidx[k]) k = (k + 1) & m;
            o->hidx[k] = o->nprops;
        }
    } else if (o->nprops > 8) hidx_build(ctx, o);
    return &o->props[o->nprops - 1];
}

int js_del_prop(JSContext *ctx, JSObject *o, JSValue key) {
    JSProp *p = js_find_prop(ctx, o, key);
    if (!p) return 0;
    uint32_t i = (uint32_t)(p - o->props);
    memmove(&o->props[i], &o->props[i + 1], sizeof(JSProp) * (o->nprops - i - 1));
    o->nprops--;
    if (o->hidx) {
        if (o->nprops <= 8) { js_mfree(ctx, o->hidx, sizeof(uint32_t) * o->hsize); o->hidx = NULL; o->hsize = 0; }
        else hidx_build(ctx, o);
    }
    return 1;
}

static JSAccessor *new_accessor(JSContext *ctx, JSValue get, JSValue set) {
    JSAccessor *a = js_cell(ctx, T_ACCESSOR, sizeof(JSAccessor));
    if (!a) return NULL;
    a->get = get;
    a->set = set;
    return a;
}

/* ==== lazy function properties ================================================================= */
void js_materialize(JSContext *ctx, JSObject *o) {
    if (!(o->h.flags & OF_LAZY)) return;
    o->h.flags &= (uint8_t)~OF_LAZY;
    if (o->h.cls == CL_NATIVE) {
        js_add_prop(ctx, o, ctx->a[A_length], js_mknum(o->u.n.length), P_C);
        js_add_prop(ctx, o, ctx->a[A_name], JS_IsString(o->u.n.name) ? o->u.n.name : ctx->empty_str, P_C);
        return;
    }
    if (o->h.cls != CL_FUNCTION) return;
    JSBytecode *b = o->u.f.b;
    js_add_prop(ctx, o, ctx->a[A_length], js_mknum(b->length), P_C);
    js_add_prop(ctx, o, ctx->a[A_name], JS_IsString(b->name) ? b->name : ctx->empty_str, P_C);
    uint32_t f = b->fflags;
    if (f & (F_ARROW | F_METHOD | F_SCRIPT | F_CLASSCTOR | F_FIELDINIT)) return;
    if ((f & F_ASYNC) && !(f & F_GEN)) return;
    JSObject *proto;
    if (f & F_GEN) proto = js_obj_alloc(ctx, CL_OBJECT, (f & F_ASYNC) ? ctx->proto_async_gen : ctx->proto_generator);
    else proto = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
    if (!proto) return;
    if (!(f & F_GEN)) js_add_prop(ctx, proto, ctx->a[A_constructor], JS_MKOBJ(o), P_W | P_C);
    js_add_prop(ctx, o, ctx->a[A_prototype], JS_MKOBJ(proto), P_W);
}

/* ==== objects ======================================================================================= */
JSObject *js_obj_alloc(JSContext *ctx, int cls, JSObject *proto) {
    JSObject *o = js_cell(ctx, T_OBJECT, sizeof(JSObject));
    if (!o) return NULL;
    o->h.cls = (uint8_t)cls;
    o->h.flags = OF_EXT;
    o->proto = proto;
    return o;
}

JSValue js_obj_new(JSContext *ctx, int cls, JSObject *proto) {
    JSObject *o = js_obj_alloc(ctx, cls, proto);
    return o ? JS_MKOBJ(o) : JS_EXCEPTION;
}

/* ==== arrays ========================================================================================== */
bool js_fast_array(JSValue v) {
    return JS_IsObject(v) && OBJ(v)->h.cls == CL_ARRAY && !(OBJ(v)->h.flags & OF_SLOW);
}

int js_array_ensure(JSContext *ctx, JSObject *a, uint32_t cap) {
    if (cap <= a->u.a.cap) return 0;
    uint32_t nc = a->u.a.cap ? a->u.a.cap : 4;
    while (nc < cap) nc = nc < 0x40000000u ? nc * 2 : cap;
    JSValue *n = js_realloc(ctx, a->u.a.v, sizeof(JSValue) * a->u.a.cap, sizeof(JSValue) * nc);
    if (!n) return -1;
    for (uint32_t i = a->u.a.cap; i < nc; i++) n[i] = JS_HOLE;
    a->u.a.v = n;
    a->u.a.cap = nc;
    return 0;
}

JSValue js_array_new(JSContext *ctx, uint32_t len) {
    JSObject *a = js_obj_alloc(ctx, CL_ARRAY, ctx->proto_array);
    if (!a) return JS_EXCEPTION;
    a->u.a.len = len;
    if (len && len <= 1024 && js_array_ensure(ctx, a, len) < 0) return JS_EXCEPTION;
    return JS_MKOBJ(a);
}

JSValue js_array_from_list(JSContext *ctx, const JSValue *v, uint32_t n) {
    JSValue arr = js_array_new(ctx, 0);
    if (JS_IsException(arr)) return arr;
    JSObject *a = OBJ(arr);
    if (n && js_array_ensure(ctx, a, n) < 0) return JS_EXCEPTION;
    if (n) memcpy(a->u.a.v, v, sizeof(JSValue) * n);
    a->u.a.len = n;
    return arr;
}

/* move the elements into the property table (attributes on elements, very sparse arrays) */
void js_array_to_slow(JSContext *ctx, JSObject *a) {
    if (a->h.flags & OF_SLOW) return;
    uint32_t n = MIN(a->u.a.len, a->u.a.cap);
    uint32_t ef = (a->h.flags & OF_FROZEN) ? (P_E) : (a->h.flags & OF_SEALED) ? (P_W | P_E) : (P_W | P_E | P_C);
    /* index keys first so they keep their order */
    JSProp *old = a->props;
    uint32_t oldn = a->nprops, oldcap = a->pcap;
    a->props = NULL;
    a->nprops = a->pcap = 0;
    if (a->hidx) { js_mfree(ctx, a->hidx, sizeof(uint32_t) * a->hsize); a->hidx = NULL; a->hsize = 0; }
    for (uint32_t i = 0; i < n; i++) {
        if (a->u.a.v[i] == JS_HOLE) continue;
        JSValue k = js_atom_from_index(ctx, i);
        if (JS_IsException(k)) break;
        js_add_prop(ctx, a, k, a->u.a.v[i], ef);
    }
    for (uint32_t i = 0; i < oldn; i++) js_add_prop(ctx, a, old[i].key, old[i].val, old[i].flags);
    js_mfree(ctx, old, sizeof(JSProp) * oldcap);
    js_mfree(ctx, a->u.a.v, sizeof(JSValue) * a->u.a.cap);
    a->u.a.v = NULL;
    a->u.a.cap = 0;
    a->h.flags |= OF_SLOW;
}

int js_array_set_length(JSContext *ctx, JSObject *a, uint32_t len) {
    if (a->h.flags & OF_SLOW) {
        if (len < a->u.a.len) {
            /* delete index properties >= len (from the highest; stop at a non-configurable one) */
            for (uint32_t i = a->nprops; i-- > 0;) {
                uint32_t idx;
                if (!js_atom_index(a->props[i].key, &idx) || idx < len) continue;
                if (!(a->props[i].flags & P_C)) { if (idx + 1 > len) len = idx + 1; continue; }
                js_del_prop(ctx, a, a->props[i].key);
                if (i > a->nprops) i = a->nprops;
            }
        }
        a->u.a.len = len;
        return 0;
    }
    if (len < a->u.a.len) {
        if (a->h.flags & OF_SEALED) return 0;
        for (uint32_t i = len; i < MIN(a->u.a.len, a->u.a.cap); i++) a->u.a.v[i] = JS_HOLE;
        if (a->u.a.cap > 64 && len < a->u.a.cap / 4) {
            uint32_t nc = MAX(len, 16u);
            JSValue *n = js_realloc(ctx, a->u.a.v, sizeof(JSValue) * a->u.a.cap, sizeof(JSValue) * nc);
            if (n) { a->u.a.v = n; a->u.a.cap = nc; }
        }
    }
    a->u.a.len = len;
    return 0;
}

int js_array_push_v(JSContext *ctx, JSValue arr, JSValue v) {
    JSObject *a = OBJ(arr);
    if (js_fast_array(arr) && (a->h.flags & OF_EXT) && !(a->h.flags & (OF_FROZEN | OF_LENRO))) {
        uint32_t n = a->u.a.len;
        if (n == 0xFFFFFFFFu) { js_throw_range(ctx, "Invalid array length"); return -1; }
        if (n < a->u.a.cap || (n == a->u.a.cap && js_array_ensure(ctx, a, n + 1) == 0)) {
            if (n < a->u.a.cap) {
                a->u.a.v[n] = v;
                a->u.a.len = n + 1;
                return 0;
            }
        }
        if (n > a->u.a.cap && n - a->u.a.cap < 1024 && js_array_ensure(ctx, a, n + 1) == 0) {
            a->u.a.v[n] = v;
            a->u.a.len = n + 1;
            return 0;
        }
    }
    int64_t len = js_length_of(ctx, arr);
    if (len < 0) return -1;
    if (js_setv(ctx, arr, js_mknum((double)len), v, true) < 0) return -1;
    return js_setp(ctx, arr, ctx->a[A_length], js_mknum((double)len + 1), arr, true) < 0 ? -1 : 0;
}

static inline JSValue arr_elem(JSObject *a, uint32_t i) {
    return i < a->u.a.len && i < a->u.a.cap ? a->u.a.v[i] : JS_HOLE;
}

/* store into a fast array: 1 done, 0 refused (frozen / not extensible), -1 must go slow, -2 exc */
static int arr_store(JSContext *ctx, JSObject *a, uint32_t i, JSValue v) {
    if (i < a->u.a.len && i < a->u.a.cap && a->u.a.v[i] != JS_HOLE) {
        if (a->h.flags & OF_FROZEN) return 0;
        a->u.a.v[i] = v;
        return 1;
    }
    if (!(a->h.flags & OF_EXT)) return 0;
    if (i >= a->u.a.len && (a->h.flags & OF_LENRO)) return 0;
    if (i == 0xFFFFFFFFu) return -1;
    if (i >= a->u.a.cap) {
        uint64_t lim = (uint64_t)MAX(a->u.a.cap, a->u.a.len > a->u.a.cap ? 0u : a->u.a.len) * 2 + 1024;
        if (i > lim && i > 4096) return -1;
        if (js_array_ensure(ctx, a, i + 1) < 0) return -2;
    }
    a->u.a.v[i] = v;
    if (i >= a->u.a.len) a->u.a.len = i + 1;
    return 1;
}

int64_t js_length_of(JSContext *ctx, JSValue obj) {
    if (js_fast_array(obj) || js_is_cls(obj, CL_ARRAY)) return OBJ(obj)->u.a.len;
    JSValue v = js_getv(ctx, obj, ctx->a[A_length]);
    if (JS_IsException(v)) return -1;
    if (JS_IsNumber(v)) {
        double d = js_num(v);
        if (!(d > 0)) return 0;
        if (d >= 9007199254740991.0) return 9007199254740991ll;
        return (int64_t)d;
    }
    bool exc = false;
    double d = js_tointeger(ctx, v, &exc);
    if (exc) return -1;
    if (!(d > 0)) return 0;
    if (d >= 9007199254740991.0) return 9007199254740991ll;
    return (int64_t)d;
}

/* ==== typed arrays ======================================================================================= */
int js_ta_elem_size(int type) {
    static const uint8_t sz[TA_COUNT] = { 1, 1, 1, 2, 2, 4, 4, 4, 8, 8, 8 };
    return sz[type];
}

uint32_t js_ta_length(JSObject *o) {
    JSTypedArray *ta = o->u.p;
    if (!ta || !ta->buf) return 0;
    JSArrayBuffer *ab = ta->buf->u.p;
    if (!ab || ab->detached) return 0;
    if (ta->track) {
        uint32_t es = (uint32_t)js_ta_elem_size(ta->type);
        return ab->len > ta->off ? (ab->len - ta->off) / es : 0;
    }
    if ((uint64_t)ta->off + (uint64_t)ta->len * (uint32_t)js_ta_elem_size(ta->type) > ab->len) return 0;
    return ta->len;
}

static uint8_t *ta_ptr(JSObject *o, uint32_t i) {
    JSTypedArray *ta = o->u.p;
    JSArrayBuffer *ab = ta->buf->u.p;
    return ab->data + ta->off + (size_t)i * (size_t)js_ta_elem_size(ta->type);
}

JSValue js_ta_get(JSContext *ctx, JSObject *o, uint32_t i) {
    (void)ctx;
    if (i >= js_ta_length(o)) return JS_UNDEFINED;
    JSTypedArray *ta = o->u.p;
    uint8_t *p = ta_ptr(o, i);
    switch (ta->type) {
    case TA_I8: return js_mknum((int8_t)*p);
    case TA_U8: case TA_U8C: return js_mknum(*p);
    case TA_I16: { int16_t x; memcpy(&x, p, 2); return js_mknum(x); }
    case TA_U16: { uint16_t x; memcpy(&x, p, 2); return js_mknum(x); }
    case TA_I32: { int32_t x; memcpy(&x, p, 4); return js_mknum(x); }
    case TA_U32: { uint32_t x; memcpy(&x, p, 4); return js_mknum(x); }
    case TA_F32: { float x; memcpy(&x, p, 4); return js_mknum(x); }
    case TA_F64: { double x; memcpy(&x, p, 8); return js_mknum(x); }
    case TA_BI64: { int64_t x; memcpy(&x, p, 8); return js_mknum((double)x); }
    case TA_BU64: { uint64_t x; memcpy(&x, p, 8); return js_mknum((double)x); }
    }
    return JS_UNDEFINED;
}

int js_ta_set(JSContext *ctx, JSObject *o, uint32_t i, JSValue v) {
    bool exc = false;
    double d = js_tonum(ctx, v, &exc);
    if (exc) return -1;
    if (i >= js_ta_length(o)) return 1;
    JSTypedArray *ta = o->u.p;
    uint8_t *p = ta_ptr(o, i);
    switch (ta->type) {
    case TA_I8: case TA_U8: *p = (uint8_t)js_d2u32(d); break;
    case TA_U8C: { double r = d != d ? 0 : d < 0 ? 0 : d > 255 ? 255 : d; double f = floor(r);
                   double diff = r - f; uint32_t x = (uint32_t)f; if (diff > 0.5 || (diff == 0.5 && (x & 1))) x++; *p = (uint8_t)x; break; }
    case TA_I16: case TA_U16: { uint16_t x = (uint16_t)js_d2u32(d); memcpy(p, &x, 2); break; }
    case TA_I32: case TA_U32: { uint32_t x = js_d2u32(d); memcpy(p, &x, 4); break; }
    case TA_F32: { float x = (float)d; memcpy(p, &x, 4); break; }
    case TA_F64: memcpy(p, &d, 8); break;
    case TA_BI64: case TA_BU64: { int64_t x = d != d ? 0 : (int64_t)d; memcpy(p, &x, 8); break; }
    }
    return 1;
}

/* ==== keys ======================================================================================== */
JSValue js_to_key(JSContext *ctx, JSValue v) {
    for (;;) {
        if (JS_IsString(v)) return js_atom(ctx, v);
        if (JS_IsSymbol(v)) return v;
        if (JS_IsNumber(v)) {
            double d = js_num(v);
            if (d >= 0 && d < 4294967295.0 && d == (double)(uint32_t)d) return js_atom_from_index(ctx, (uint32_t)d);
            return js_atom(ctx, js_num_to_str(ctx, d));
        }
        if (JS_IsObject(v)) {
            v = js_to_primitive(ctx, v, 2);
            if (JS_IsException(v)) return v;
            continue;
        }
        JSValue s = js_tostr(ctx, v);
        if (JS_IsException(s)) return s;
        return js_atom(ctx, s);
    }
}

/* number value -> index if it is one */
static inline bool num_index(JSValue v, uint32_t *idx) {
    if (!JS_IsNumber(v)) return false;
    double d = js_num(v);
    if (!(d >= 0 && d < 4294967295.0)) return false;
    uint32_t i = (uint32_t)d;
    if ((double)i != d) return false;
    *idx = i;
    return true;
}

/* ==== proxies ========================================================================================== */
static JSValue proxy_trap(JSContext *ctx, JSObject *p, int atom, JSValue *handler) {
    if (!JS_IsObject(p->u.px.handler)) { js_throw_type(ctx, "Cannot perform '%s' on a proxy that has been revoked", "operation"); return JS_EXCEPTION; }
    *handler = p->u.px.handler;
    return js_get_method(ctx, p->u.px.handler, ctx->a[atom]);
}

static JSValue proxy_get(JSContext *ctx, JSObject *p, JSValue key, JSValue recv) {
    JSValue h, t = js_check_stack(ctx) ? proxy_trap(ctx, p, A_get, &h) : js_throw_range(ctx, "Maximum call stack size exceeded");
    if (JS_IsException(t)) return t;
    if (JS_IsUndefined(t)) return js_getp_recv(ctx, p->u.px.target, key, recv);
    JSValue args[3] = { p->u.px.target, key, recv };
    return js_callv(ctx, t, h, 3, args);
}

static int proxy_set(JSContext *ctx, JSObject *p, JSValue key, JSValue v, JSValue recv, bool strict) {
    JSValue h, t = proxy_trap(ctx, p, A_set, &h);
    if (JS_IsException(t)) return -1;
    if (JS_IsUndefined(t)) return js_setp(ctx, p->u.px.target, key, v, recv, strict);
    JSValue args[4] = { p->u.px.target, key, v, recv };
    JSValue r = js_callv(ctx, t, h, 4, args);
    if (JS_IsException(r)) return -1;
    if (!js_truthy(r)) {
        if (strict) { char k[64]; js_value_desc(ctx, key, k, sizeof k); js_throw_type(ctx, "'set' on proxy: trap returned falsish for property %s", k); return -1; }
        return 0;
    }
    return 1;
}

static int proxy_has(JSContext *ctx, JSObject *p, JSValue key) {
    JSValue h, t = proxy_trap(ctx, p, A_has, &h);
    if (JS_IsException(t)) return -1;
    if (JS_IsUndefined(t)) return js_has_prop(ctx, p->u.px.target, key);
    JSValue args[2] = { p->u.px.target, key };
    JSValue r = js_callv(ctx, t, h, 2, args);
    if (JS_IsException(r)) return -1;
    return js_truthy(r);
}

static int proxy_delete(JSContext *ctx, JSObject *p, JSValue key, bool strict) {
    JSValue h, t = proxy_trap(ctx, p, A_deleteProperty, &h);
    if (JS_IsException(t)) return -1;
    if (JS_IsUndefined(t)) return js_delete_prop(ctx, p->u.px.target, key, strict);
    JSValue args[2] = { p->u.px.target, key };
    JSValue r = js_callv(ctx, t, h, 2, args);
    if (JS_IsException(r)) return -1;
    if (!js_truthy(r) && strict) { js_throw_type(ctx, "'deleteProperty' on proxy: trap returned falsish"); return -1; }
    return js_truthy(r);
}

static int proxy_define(JSContext *ctx, JSObject *p, JSValue key, const JSDesc *d) {
    JSValue h, t = proxy_trap(ctx, p, A_defineProperty, &h);
    if (JS_IsException(t)) return -1;
    if (JS_IsUndefined(t)) return js_define_own(ctx, p->u.px.target, key, d);
    JSValue dobj = js_desc_to_obj(ctx, d);
    if (JS_IsException(dobj)) return -1;
    JSValue args[3] = { p->u.px.target, key, dobj };
    JSValue r = js_callv(ctx, t, h, 3, args);
    if (JS_IsException(r)) return -1;
    if (!js_truthy(r)) {
        if (d->flags & DP_THROW) { js_throw_type(ctx, "'defineProperty' on proxy: trap returned falsish"); return -1; }
        return 0;
    }
    return 1;
}

static int proxy_get_own(JSContext *ctx, JSObject *p, JSValue key, JSDesc *d) {
    JSValue h, t = proxy_trap(ctx, p, A_getOwnPropertyDescriptor, &h);
    if (JS_IsException(t)) return -1;
    if (JS_IsUndefined(t)) return js_get_own_desc(ctx, p->u.px.target, key, d);
    JSValue args[2] = { p->u.px.target, key };
    JSValue r = js_callv(ctx, t, h, 2, args);
    if (JS_IsException(r)) return -1;
    if (JS_IsUndefined(r)) return 0;
    if (!JS_IsObject(r)) { js_throw_type(ctx, "'getOwnPropertyDescriptor' on proxy: trap returned neither object nor undefined"); return -1; }
    if (!d) return 1;
    if (js_obj_to_desc(ctx, r, d) < 0) return -1;
    if (!(d->flags & (DP_HAS_GET | DP_HAS_SET))) {
        if (!(d->flags & DP_HAS_VALUE)) d->value = JS_UNDEFINED;
    } else {
        if (!(d->flags & DP_HAS_GET)) d->get = JS_UNDEFINED;
        if (!(d->flags & DP_HAS_SET)) d->set = JS_UNDEFINED;
        d->flags |= P_ACC;
    }
    return 1;
}

static JSValue proxy_own_keys(JSContext *ctx, JSObject *p) {
    JSValue h, t = proxy_trap(ctx, p, A_ownKeys, &h);
    if (JS_IsException(t)) return t;
    if (JS_IsUndefined(t)) return js_own_keys(ctx, p->u.px.target, OK_STRINGS | OK_SYMBOLS);
    JSValue args[1] = { p->u.px.target };
    JSValue r = js_callv(ctx, t, h, 1, args);
    if (JS_IsException(r)) return r;
    if (!JS_IsObject(r)) return js_throw_type(ctx, "CreateListFromArrayLike called on non-object");
    int64_t n = js_length_of(ctx, r);
    if (n < 0) return JS_EXCEPTION;
    JSValue out = js_array_new(ctx, 0);
    if (JS_IsException(out)) return out;
    for (int64_t i = 0; i < n; i++) {
        JSValue k = js_geti(ctx, r, (uint32_t)i);
        if (JS_IsException(k)) return k;
        if (!JS_IsString(k) && !JS_IsSymbol(k)) return js_throw_type(ctx, "proxy [[OwnPropertyKeys]] must return an array with only string and symbol elements");
        if (JS_IsString(k)) k = js_atom(ctx, k);
        if (js_array_push_v(ctx, out, k) < 0) return JS_EXCEPTION;
    }
    return out;
}

/* ==== [[GetOwnProperty]] ================================================================================ */
static void desc_data(JSDesc *d, JSValue v, uint32_t pf) {
    d->value = v;
    d->get = d->set = JS_UNDEFINED;
    d->flags = (pf & (P_W | P_E | P_C)) | DP_DATA_ALL;
}

static void desc_from_prop(JSDesc *d, const JSProp *p) {
    if (p->flags & P_ACC) {
        JSAccessor *a = JS_PTR(p->val);
        d->value = JS_UNDEFINED;
        d->get = a->get;
        d->set = a->set;
        d->flags = (p->flags & (P_E | P_C | P_ACC)) | DP_HAS_GET | DP_HAS_SET | DP_HAS_E | DP_HAS_C;
    } else desc_data(d, p->val, p->flags);
}

static int host_get_hook(JSContext *ctx, JSObject *o, JSValue key, JSValue *v) {
    int c = o->u.host.cls;
    if (c <= 0 || c > ctx->nclasses || !ctx->classes[c - 1].c.get) return 0;
    return ctx->classes[c - 1].c.get(ctx, JS_MKOBJ(o), key, v);
}

int js_get_own_desc(JSContext *ctx, JSValue objv, JSValue key, JSDesc *d) {
    JSDesc tmp;
    if (!d) d = &tmp;
    JSObject *o = OBJ(objv);
    uint32_t idx;
    switch (o->h.cls) {
    case CL_PROXY: return proxy_get_own(ctx, o, key, d);
    case CL_ARRAY:
        if (key == ctx->a[A_length]) { desc_data(d, js_mknum(o->u.a.len), (o->h.flags & (OF_LENRO | OF_FROZEN)) ? 0 : P_W); return 1; }
        if (!(o->h.flags & OF_SLOW) && js_atom_index(key, &idx)) {
            JSValue v = arr_elem(o, idx);
            if (v == JS_HOLE) return 0;
            desc_data(d, v, (o->h.flags & OF_FROZEN) ? P_E : (o->h.flags & OF_SEALED) ? (P_W | P_E) : (P_W | P_E | P_C));
            return 1;
        }
        break;
    case CL_ARGUMENTS:
        if (!(o->h.flags & OF_SLOW) && js_atom_index(key, &idx)) {
            JSValue v = arr_elem(o, idx);
            if (v == JS_HOLE) return 0;
            desc_data(d, v, (o->h.flags & OF_FROZEN) ? P_E : (o->h.flags & OF_SEALED) ? (P_W | P_E) : (P_W | P_E | P_C));
            return 1;
        }
        break;
    case CL_STRING: {
        JSValue s = o->u.prim;
        if (key == ctx->a[A_length]) { desc_data(d, js_mknum(js_strlen(s)), 0); return 1; }
        if (js_atom_index(key, &idx) && idx < js_strlen(s)) {
            JSValue ch = js_substr(ctx, s, idx, idx + 1);
            if (JS_IsException(ch)) return -1;
            desc_data(d, ch, P_E);
            return 1;
        }
        break;
    }
    case CL_TYPEDARRAY:
        if (js_atom_index(key, &idx)) {
            if (idx >= js_ta_length(o)) return 0;
            desc_data(d, js_ta_get(ctx, o, idx), P_W | P_E | P_C);
            return 1;
        }
        break;
    case CL_HOST: {
        JSValue v;
        int r = host_get_hook(ctx, o, key, &v);
        if (r < 0) return -1;
        if (r > 0) { desc_data(d, v, P_W | P_E | P_C); return 1; }
        break;
    }
    default: break;
    }
    JSProp *p = js_find_prop(ctx, o, key);
    if (!p) return 0;
    desc_from_prop(d, p);
    return 1;
}

/* ==== [[Get]] ============================================================================================= */
static JSValue call_getter(JSContext *ctx, JSProp *p, JSValue recv) {
    JSAccessor *a = JS_PTR(p->val);
    if (JS_IsUndefined(a->get)) return JS_UNDEFINED;
    return js_callv(ctx, a->get, recv, 0, NULL);
}

JSValue js_getp_recv(JSContext *ctx, JSValue objv, JSValue key, JSValue recv) {
    JSObject *o = OBJ(objv);
    uint32_t idx;
    for (;;) {
        switch (o->h.cls) {
        case CL_ARRAY:
            if (!(o->h.flags & OF_SLOW)) {
                if (key == ctx->a[A_length]) return js_mknum(o->u.a.len);
                if (js_atom_index(key, &idx)) {
                    JSValue v = arr_elem(o, idx);
                    if (v != JS_HOLE) return v;
                    goto next;
                }
            } else if (key == ctx->a[A_length]) return js_mknum(o->u.a.len);
            break;
        case CL_ARGUMENTS:
            if (!(o->h.flags & OF_SLOW) && js_atom_index(key, &idx)) {
                JSValue v = arr_elem(o, idx);
                if (v != JS_HOLE) return v;
                goto next;
            }
            break;
        case CL_STRING:
            if (key == ctx->a[A_length]) return js_mknum(js_strlen(o->u.prim));
            if (js_atom_index(key, &idx) && idx < js_strlen(o->u.prim)) return js_substr(ctx, o->u.prim, idx, idx + 1);
            break;
        case CL_TYPEDARRAY:
            if (js_atom_index(key, &idx)) return js_ta_get(ctx, o, idx);
            break;
        case CL_PROXY:
            return proxy_get(ctx, o, key, recv);
        case CL_HOST: {
            JSValue v;
            int r = host_get_hook(ctx, o, key, &v);
            if (r < 0) return JS_EXCEPTION;
            if (r > 0) return v;
            break;
        }
        default: break;
        }
        {
            JSProp *p = js_find_prop(ctx, o, key);
            if (p) return (p->flags & P_ACC) ? call_getter(ctx, p, recv) : p->val;
        }
    next:
        o = o->proto;
        if (!o) return JS_UNDEFINED;
    }
}

static JSObject *proto_of_prim(JSContext *ctx, JSValue v) {
    if (JS_IsString(v)) return ctx->proto_string;
    if (JS_IsNumber(v)) return ctx->proto_number;
    if (JS_IsBool(v)) return ctx->proto_boolean;
    if (JS_IsSymbol(v)) return ctx->proto_symbol;
    if (JS_TAGOF(v) == JS_TAG_BIGINT) return ctx->proto_bigint;
    return NULL;
}

static JSValue read_of_nullish(JSContext *ctx, JSValue obj, JSValue key) {
    char k[80];
    js_value_desc(ctx, key, k, sizeof k);
    return js_throw_type(ctx, "Cannot read properties of %s (reading %s)", JS_IsNull(obj) ? "null" : "undefined", k);
}

JSValue js_getp(JSContext *ctx, JSValue obj, JSValue key) {
    if (JS_LIKELY(JS_IsObject(obj))) return js_getp_recv(ctx, obj, key, obj);
    if (JS_IsString(obj)) {
        uint32_t idx;
        if (key == ctx->a[A_length]) return js_mknum(js_strlen(obj));
        if (js_atom_index(key, &idx)) {
            if (idx < js_strlen(obj)) return js_substr(ctx, obj, idx, idx + 1);
        }
    }
    JSObject *p = proto_of_prim(ctx, obj);
    if (!p) return read_of_nullish(ctx, obj, key);
    return js_getp_recv(ctx, JS_MKOBJ(p), key, obj);
}

JSValue js_geti(JSContext *ctx, JSValue obj, uint32_t i) {
    if (js_fast_array(obj)) {
        JSValue v = arr_elem(OBJ(obj), i);
        if (v != JS_HOLE) return v;
    }
    JSValue k = js_atom_from_index(ctx, i);
    if (JS_IsException(k)) return k;
    return js_getp(ctx, obj, k);
}

JSValue js_getv(JSContext *ctx, JSValue obj, JSValue key) {
    uint32_t idx;
    if (num_index(key, &idx)) {
        if (JS_IsObject(obj)) {
            JSObject *o = OBJ(obj);
            if ((o->h.cls == CL_ARRAY || o->h.cls == CL_ARGUMENTS) && !(o->h.flags & OF_SLOW)) {
                JSValue v = arr_elem(o, idx);
                if (v != JS_HOLE) return v;
            } else if (o->h.cls == CL_TYPEDARRAY) return js_ta_get(ctx, o, idx);
        } else if (JS_IsString(obj)) {
            if (idx < js_strlen(obj)) return js_substr(ctx, obj, idx, idx + 1);
        }
    }
    if (JS_IsNullish(obj)) return read_of_nullish(ctx, obj, key);
    JSValue k = js_to_key(ctx, key);
    if (JS_IsException(k)) return k;
    return js_getp(ctx, obj, k);
}

/* ==== [[Set]] ============================================================================================= */
static int set_fail(JSContext *ctx, JSValue key, bool strict, const char *why) {
    if (!strict) return 0;
    char k[80];
    js_value_desc(ctx, key, k, sizeof k);
    js_throw_type(ctx, "Cannot assign to %s property %s of object", why, k);
    return -1;
}

static int define_on_receiver(JSContext *ctx, JSValue recv, JSValue key, JSValue v, bool strict) {
    if (!JS_IsObject(recv)) return set_fail(ctx, key, strict, "read only");
    JSDesc ex;
    int r = js_get_own_desc(ctx, recv, key, &ex);
    if (r < 0) return -1;
    JSDesc d;
    if (r) {
        if (ex.flags & P_ACC) return set_fail(ctx, key, strict, "accessor");
        if (!(ex.flags & P_W)) return set_fail(ctx, key, strict, "read only");
        d.value = v;
        d.flags = DP_HAS_VALUE;
    } else {
        d.value = v;
        d.flags = DP_DATA_ALL | P_W | P_E | P_C;
    }
    d.get = d.set = JS_UNDEFINED;
    r = js_define_own(ctx, recv, key, &d);
    if (r < 0) return -1;
    if (!r) return set_fail(ctx, key, strict, "read only");
    return 1;
}

int js_setp(JSContext *ctx, JSValue objv, JSValue key, JSValue v, JSValue recv, bool strict) {
    if (!JS_IsObject(objv)) {
        if (JS_IsNullish(objv)) {
            char k[80];
            js_value_desc(ctx, key, k, sizeof k);
            js_throw_type(ctx, "Cannot set properties of %s (setting %s)", JS_IsNull(objv) ? "null" : "undefined", k);
            return -1;
        }
        JSObject *p = proto_of_prim(ctx, objv);
        if (!p) return set_fail(ctx, key, strict, "read only");
        objv = JS_MKOBJ(p);
    }
    JSObject *o = OBJ(objv);
    bool same = recv == objv;
    uint32_t idx;
    for (;;) {
        switch (o->h.cls) {
        case CL_ARRAY:
            if (key == ctx->a[A_length]) {
                if (!same) goto ordinary_found_data_len;
                {
                    JSDesc d = { v, JS_UNDEFINED, JS_UNDEFINED, DP_HAS_VALUE };
                    int r = js_define_own(ctx, objv, key, &d);
                    if (r < 0) return -1;
                    return r ? 1 : set_fail(ctx, key, strict, "read only");
                }
            ordinary_found_data_len:
                if (o->h.flags & (OF_LENRO | OF_FROZEN)) return set_fail(ctx, key, strict, "read only");
                return define_on_receiver(ctx, recv, key, v, strict);
            }
            if (!(o->h.flags & OF_SLOW) && js_atom_index(key, &idx)) {
                JSValue cur = arr_elem(o, idx);
                if (cur != JS_HOLE) {
                    if (o->h.flags & OF_FROZEN) return set_fail(ctx, key, strict, "read only");
                    if (same) { o->u.a.v[idx] = v; return 1; }
                    return define_on_receiver(ctx, recv, key, v, strict);
                }
                if (same && (!o->proto || o->proto == ctx->proto_array)) {
                    int r = arr_store(ctx, o, idx, v);
                    if (r == 1) return 1;
                    if (r == 0) return set_fail(ctx, key, strict, "read only");
                    if (r == -2) return -1;
                    js_array_to_slow(ctx, o);
                    break;
                }
                goto next;
            }
            break;
        case CL_ARGUMENTS:
            if (!(o->h.flags & OF_SLOW) && js_atom_index(key, &idx)) {
                JSValue cur = arr_elem(o, idx);
                if (cur != JS_HOLE && !(o->h.flags & OF_FROZEN) && same) { o->u.a.v[idx] = v; return 1; }
                js_array_to_slow(ctx, o);
            }
            break;
        case CL_STRING:
            if (key == ctx->a[A_length] || (js_atom_index(key, &idx) && idx < js_strlen(o->u.prim)))
                return set_fail(ctx, key, strict, "read only");
            break;
        case CL_TYPEDARRAY:
            if (js_atom_index(key, &idx)) {
                if (same) return js_ta_set(ctx, o, idx, v) < 0 ? -1 : 1;
                if (idx >= js_ta_length(o)) return 1;
                return define_on_receiver(ctx, recv, key, v, strict);
            }
            break;
        case CL_PROXY:
            return proxy_set(ctx, o, key, v, recv, strict);
        case CL_HOST: {
            int c = o->u.host.cls;
            if (same && c > 0 && c <= ctx->nclasses && ctx->classes[c - 1].c.set) {
                int r = ctx->classes[c - 1].c.set(ctx, objv, key, v);
                if (r != 0) return r;
            }
            break;
        }
        default: break;
        }
        {
            JSProp *p = js_find_prop(ctx, o, key);
            if (p) {
                if (p->flags & P_ACC) {
                    JSAccessor *a = JS_PTR(p->val);
                    if (JS_IsUndefined(a->set)) return set_fail(ctx, key, strict, "getter-only");
                    JSValue args[1] = { v };
                    JSValue r = js_callv(ctx, a->set, recv, 1, args);
                    return JS_IsException(r) ? -1 : 1;
                }
                if (!(p->flags & P_W)) return set_fail(ctx, key, strict, "read only");
                if (same) { p->val = v; return 1; }
                return define_on_receiver(ctx, recv, key, v, strict);
            }
        }
    next:
        if (!o->proto) break;
        o = o->proto;
    }
    /* not found anywhere: a new property on the receiver */
    if (same) {
        JSObject *r = OBJ(recv);
        if (r->h.cls == CL_OBJECT || r->h.cls == CL_FUNCTION || r->h.cls == CL_HOST || r->h.cls == CL_ERROR) {
            if (!(r->h.flags & OF_EXT)) return set_fail(ctx, key, strict, "non-extensible");
            return js_add_prop(ctx, r, key, v, P_W | P_E | P_C) ? 1 : -1;
        }
    }
    return define_on_receiver(ctx, recv, key, v, strict);
}

int js_seti(JSContext *ctx, JSValue obj, uint32_t i, JSValue v) {
    if (js_fast_array(obj)) {
        JSObject *a = OBJ(obj);
        if (i < a->u.a.len && i < a->u.a.cap && a->u.a.v[i] != JS_HOLE && !(a->h.flags & OF_FROZEN)) { a->u.a.v[i] = v; return 1; }
    }
    JSValue k = js_atom_from_index(ctx, i);
    if (JS_IsException(k)) return -1;
    return js_setp(ctx, obj, k, v, obj, true);
}

int js_setv(JSContext *ctx, JSValue obj, JSValue key, JSValue v, bool strict) {
    uint32_t idx;
    if (num_index(key, &idx) && JS_IsObject(obj)) {
        JSObject *o = OBJ(obj);
        if (o->h.cls == CL_ARRAY && !(o->h.flags & OF_SLOW)) {
            if (idx < o->u.a.len && idx < o->u.a.cap && o->u.a.v[idx] != JS_HOLE) {
                if (!(o->h.flags & OF_FROZEN)) { o->u.a.v[idx] = v; return 1; }
            } else if (o->proto == ctx->proto_array) {
                int r = arr_store(ctx, o, idx, v);
                if (r == 1) return 1;
                if (r == -2) return -1;
            }
        } else if (o->h.cls == CL_TYPEDARRAY) return js_ta_set(ctx, o, idx, v) < 0 ? -1 : 1;
    }
    JSValue k = js_to_key(ctx, key);
    if (JS_IsException(k)) return -1;
    return js_setp(ctx, obj, k, v, obj, strict);
}

/* ==== [[DefineOwnProperty]] ========================================================================== */
static int reject(JSContext *ctx, JSValue key, const JSDesc *d) {
    if (d->flags & DP_THROW) {
        char k[80];
        js_value_desc(ctx, key, k, sizeof k);
        js_throw_type(ctx, "Cannot redefine property: %s", k);
        return -1;
    }
    return 0;
}

static bool is_accessor_desc(const JSDesc *d) { return (d->flags & (DP_HAS_GET | DP_HAS_SET)) != 0; }
static bool is_data_desc(const JSDesc *d) { return (d->flags & (DP_HAS_VALUE | DP_HAS_W)) != 0; }

/* ordinary define on the property table */
static int define_table(JSContext *ctx, JSObject *o, JSValue key, const JSDesc *d) {
    JSProp *p = js_find_prop(ctx, o, key);
    if (!p) {
        if (!(o->h.flags & OF_EXT)) return reject(ctx, key, d);
        uint32_t f = 0;
        if ((d->flags & DP_HAS_E) && (d->flags & P_E)) f |= P_E;
        if ((d->flags & DP_HAS_C) && (d->flags & P_C)) f |= P_C;
        if (is_accessor_desc(d)) {
            JSAccessor *a = new_accessor(ctx, (d->flags & DP_HAS_GET) ? d->get : JS_UNDEFINED, (d->flags & DP_HAS_SET) ? d->set : JS_UNDEFINED);
            if (!a) return -1;
            return js_add_prop(ctx, o, key, JS_MKINT(a), f | P_ACC) ? 1 : -1;
        }
        if ((d->flags & DP_HAS_W) && (d->flags & P_W)) f |= P_W;
        return js_add_prop(ctx, o, key, (d->flags & DP_HAS_VALUE) ? d->value : JS_UNDEFINED, f) ? 1 : -1;
    }
    uint32_t cf = p->flags;
    bool cur_acc = (cf & P_ACC) != 0;
    if (!(cf & P_C)) {
        if ((d->flags & DP_HAS_C) && (d->flags & P_C)) return reject(ctx, key, d);
        if ((d->flags & DP_HAS_E) && ((d->flags & P_E) != (cf & P_E))) return reject(ctx, key, d);
        if (is_accessor_desc(d) || is_data_desc(d)) {
            if (cur_acc != is_accessor_desc(d)) return reject(ctx, key, d);
            if (!cur_acc) {
                if (!(cf & P_W)) {
                    if ((d->flags & DP_HAS_W) && (d->flags & P_W)) return reject(ctx, key, d);
                    if ((d->flags & DP_HAS_VALUE) && !js_same_value(d->value, p->val, false)) return reject(ctx, key, d);
                }
            } else {
                JSAccessor *a = JS_PTR(p->val);
                if ((d->flags & DP_HAS_GET) && d->get != a->get) return reject(ctx, key, d);
                if ((d->flags & DP_HAS_SET) && d->set != a->set) return reject(ctx, key, d);
            }
        }
    }
    /* apply */
    if (is_accessor_desc(d) && !cur_acc) {
        JSAccessor *a = new_accessor(ctx, (d->flags & DP_HAS_GET) ? d->get : JS_UNDEFINED, (d->flags & DP_HAS_SET) ? d->set : JS_UNDEFINED);
        if (!a) return -1;
        p = js_find_prop(ctx, o, key);
        p->val = JS_MKINT(a);
        p->flags = (p->flags & (P_E | P_C)) | P_ACC;
    } else if (is_data_desc(d) && cur_acc) {
        p->val = (d->flags & DP_HAS_VALUE) ? d->value : JS_UNDEFINED;
        p->flags = (p->flags & (P_E | P_C)) | (((d->flags & DP_HAS_W) && (d->flags & P_W)) ? P_W : 0);
    } else if (cur_acc) {
        JSAccessor *a = JS_PTR(p->val);
        if ((d->flags & DP_HAS_GET) || (d->flags & DP_HAS_SET)) {
            JSAccessor *na = new_accessor(ctx, (d->flags & DP_HAS_GET) ? d->get : a->get, (d->flags & DP_HAS_SET) ? d->set : a->set);
            if (!na) return -1;
            p = js_find_prop(ctx, o, key);
            p->val = JS_MKINT(na);
        }
    } else {
        if (d->flags & DP_HAS_VALUE) p->val = d->value;
        if (d->flags & DP_HAS_W) p->flags = (p->flags & ~(uint32_t)P_W) | (d->flags & P_W);
    }
    if (d->flags & DP_HAS_E) p->flags = (p->flags & ~(uint32_t)P_E) | (d->flags & P_E);
    if (d->flags & DP_HAS_C) p->flags = (p->flags & ~(uint32_t)P_C) | (d->flags & P_C);
    return 1;
}

static bool plain_elem_desc(const JSDesc *d, bool exists) {
    if (is_accessor_desc(d)) return false;
    if (exists) {
        if ((d->flags & DP_HAS_W) && !(d->flags & P_W)) return false;
        if ((d->flags & DP_HAS_E) && !(d->flags & P_E)) return false;
        if ((d->flags & DP_HAS_C) && !(d->flags & P_C)) return false;
        return true;
    }
    return (d->flags & DP_HAS_W) && (d->flags & P_W) && (d->flags & DP_HAS_E) && (d->flags & P_E) &&
           (d->flags & DP_HAS_C) && (d->flags & P_C);
}

static int array_define_length(JSContext *ctx, JSObject *a, JSValue key, const JSDesc *d) {
    if (is_accessor_desc(d)) return reject(ctx, key, d);
    if ((d->flags & DP_HAS_C) && (d->flags & P_C)) return reject(ctx, key, d);
    if ((d->flags & DP_HAS_E) && (d->flags & P_E)) return reject(ctx, key, d);
    bool ro = (a->h.flags & (OF_LENRO | OF_FROZEN)) != 0;
    if (d->flags & DP_HAS_VALUE) {
        double nl;
        if (JS_IsNumber(d->value)) nl = js_num(d->value);
        else {
            bool exc = false;
            nl = js_tonum(ctx, d->value, &exc);
            if (exc) return -1;
        }
        bool e2 = false;
        double n2 = js_tonum(ctx, d->value, &e2);
        if (e2) return -1;
        uint32_t len = js_d2u32(nl);
        if ((double)len != n2) { js_throw_range(ctx, "Invalid array length"); return -1; }
        if (len != a->u.a.len) {
            if (ro) return reject(ctx, key, d);
            uint32_t want = len;
            js_array_set_length(ctx, a, len);
            if (a->u.a.len != want) { if ((d->flags & DP_HAS_W) && !(d->flags & P_W)) a->h.flags |= OF_LENRO; return reject(ctx, key, d); }
        }
    }
    if (d->flags & DP_HAS_W) {
        if ((d->flags & P_W) && ro) return reject(ctx, key, d);
        if (!(d->flags & P_W)) a->h.flags |= OF_LENRO;
    }
    return 1;
}

int js_define_own(JSContext *ctx, JSValue objv, JSValue key, const JSDesc *d) {
    JSObject *o = OBJ(objv);
    uint32_t idx;
    switch (o->h.cls) {
    case CL_PROXY: return proxy_define(ctx, o, key, d);
    case CL_ARRAY:
        if (key == ctx->a[A_length]) return array_define_length(ctx, o, key, d);
        if (js_atom_index(key, &idx)) {
            if (idx >= o->u.a.len && (o->h.flags & (OF_LENRO | OF_FROZEN))) return reject(ctx, key, d);
            if (!(o->h.flags & OF_SLOW)) {
                JSValue cur = arr_elem(o, idx);
                bool exists = cur != JS_HOLE;
                if (plain_elem_desc(d, exists) && !(o->h.flags & (OF_FROZEN | OF_SEALED))) {
                    if (!exists && !(o->h.flags & OF_EXT)) return reject(ctx, key, d);
                    JSValue v = (d->flags & DP_HAS_VALUE) ? d->value : (exists ? cur : JS_UNDEFINED);
                    if (exists) { o->u.a.v[idx] = v; return 1; }
                    int r = arr_store(ctx, o, idx, v);
                    if (r == 1) return 1;
                    if (r == -2) return -1;
                    if (r == 0) return reject(ctx, key, d);
                }
                js_array_to_slow(ctx, o);
            }
            int r = define_table(ctx, o, key, d);
            if (r == 1 && idx >= o->u.a.len) o->u.a.len = idx + 1;
            return r;
        }
        break;
    case CL_ARGUMENTS:
        if (js_atom_index(key, &idx) && !(o->h.flags & OF_SLOW)) {
            JSValue cur = arr_elem(o, idx);
            if (cur != JS_HOLE && plain_elem_desc(d, true) && !(o->h.flags & (OF_FROZEN | OF_SEALED))) {
                if (d->flags & DP_HAS_VALUE) o->u.a.v[idx] = d->value;
                return 1;
            }
            js_array_to_slow(ctx, o);
        }
        break;
    case CL_STRING:
        if (key == ctx->a[A_length] || (js_atom_index(key, &idx) && idx < js_strlen(o->u.prim))) {
            JSDesc cur;
            js_get_own_desc(ctx, objv, key, &cur);
            if (is_accessor_desc(d) || ((d->flags & DP_HAS_C) && (d->flags & P_C)) ||
                ((d->flags & DP_HAS_E) && ((d->flags & P_E) != (cur.flags & P_E))) ||
                ((d->flags & DP_HAS_W) && (d->flags & P_W)) ||
                ((d->flags & DP_HAS_VALUE) && !js_same_value(d->value, cur.value, false)))
                return reject(ctx, key, d);
            return 1;
        }
        break;
    case CL_TYPEDARRAY:
        if (js_atom_index(key, &idx)) {
            if (idx >= js_ta_length(o)) return reject(ctx, key, d);
            if (is_accessor_desc(d) || ((d->flags & DP_HAS_C) && !(d->flags & P_C)) ||
                ((d->flags & DP_HAS_E) && !(d->flags & P_E)) || ((d->flags & DP_HAS_W) && !(d->flags & P_W)))
                return reject(ctx, key, d);
            if (d->flags & DP_HAS_VALUE) return js_ta_set(ctx, o, idx, d->value) < 0 ? -1 : 1;
            return 1;
        }
        break;
    default: break;
    }
    return define_table(ctx, o, key, d);
}

int js_define_value(JSContext *ctx, JSValue obj, JSValue key, JSValue v, uint32_t pf) {
    JSDesc d = { v, JS_UNDEFINED, JS_UNDEFINED, DP_DATA_ALL | DP_THROW | (pf & (P_W | P_E | P_C)) };
    return js_define_own(ctx, obj, key, &d);
}

int js_create_data_prop(JSContext *ctx, JSValue obj, JSValue key, JSValue v) {
    JSObject *o = OBJ(obj);
    uint32_t idx;
    if (o->h.cls == CL_ARRAY && !(o->h.flags & OF_SLOW) && js_atom_index(key, &idx)) {
        int r = arr_store(ctx, o, idx, v);
        if (r == 1) return 1;
        if (r == -2) return -1;
    } else if (o->h.cls == CL_OBJECT && !(o->h.flags & OF_LAZY)) {
        JSProp *p = js_find_prop(ctx, o, key);
        if (!p && (o->h.flags & OF_EXT)) return js_add_prop(ctx, o, key, v, P_W | P_E | P_C) ? 1 : -1;
        if (p && !(p->flags & P_ACC) && (p->flags & P_C)) { p->val = v; p->flags = P_W | P_E | P_C; return 1; }
    }
    return js_define_value(ctx, obj, key, v, P_W | P_E | P_C);
}

/* ==== has / delete ===================================================================================== */
int js_has_prop(JSContext *ctx, JSValue objv, JSValue key) {
    if (!JS_IsObject(objv)) return 0;
    JSObject *o = OBJ(objv);
    for (;;) {
        if (o->h.cls == CL_PROXY) return proxy_has(ctx, o, key);
        if (o->h.cls == CL_TYPEDARRAY) {
            uint32_t idx;
            if (js_atom_index(key, &idx)) return idx < js_ta_length(o);
        }
        int r = js_get_own_desc(ctx, JS_MKOBJ(o), key, NULL);
        if (r) return r;
        o = o->proto;
        if (!o) return 0;
    }
}

int js_delete_prop(JSContext *ctx, JSValue objv, JSValue key, bool strict) {
    JSObject *o = OBJ(objv);
    uint32_t idx;
    switch (o->h.cls) {
    case CL_PROXY: return proxy_delete(ctx, o, key, strict);
    case CL_ARRAY:
        if (key == ctx->a[A_length]) goto refuse;
        if (!(o->h.flags & OF_SLOW) && js_atom_index(key, &idx)) {
            if (arr_elem(o, idx) == JS_HOLE) return 1;
            if (o->h.flags & OF_SEALED) goto refuse;
            o->u.a.v[idx] = JS_HOLE;
            return 1;
        }
        break;
    case CL_ARGUMENTS:
        if (!(o->h.flags & OF_SLOW) && js_atom_index(key, &idx)) {
            if (arr_elem(o, idx) == JS_HOLE) return 1;
            if (o->h.flags & OF_SEALED) goto refuse;
            o->u.a.v[idx] = JS_HOLE;
            return 1;
        }
        break;
    case CL_STRING:
        if (key == ctx->a[A_length] || (js_atom_index(key, &idx) && idx < js_strlen(o->u.prim))) goto refuse;
        break;
    case CL_TYPEDARRAY:
        if (js_atom_index(key, &idx)) { if (idx < js_ta_length(o)) goto refuse; return 1; }
        break;
    case CL_HOST: {
        int c = o->u.host.cls;
        if (c > 0 && c <= ctx->nclasses && ctx->classes[c - 1].c.del) {
            int r = ctx->classes[c - 1].c.del(ctx, objv, key);
            if (r) return r;
        }
        break;
    }
    default: break;
    }
    {
        JSProp *p = js_find_prop(ctx, o, key);
        if (!p) return 1;
        if (!(p->flags & P_C)) goto refuse;
        js_del_prop(ctx, o, key);
        return 1;
    }
refuse:
    if (strict) {
        char k[80];
        js_value_desc(ctx, key, k, sizeof k);
        js_throw_type(ctx, "Cannot delete property %s of object", k);
        return -1;
    }
    return 0;
}

/* ==== [[OwnPropertyKeys]] ============================================================================== */
static int u32cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int push_key(JSContext *ctx, JSValue arr, JSValue k) { return js_array_push_v(ctx, arr, k); }

JSValue js_own_keys(JSContext *ctx, JSValue objv, int flags) {
    JSObject *o = OBJ(objv);
    if (o->h.cls == CL_PROXY) {
        JSValue all = proxy_own_keys(ctx, o);
        if (JS_IsException(all) || flags == (OK_STRINGS | OK_SYMBOLS)) return all;
        JSValue out = js_array_new(ctx, 0);
        if (JS_IsException(out)) return out;
        uint32_t n = OBJ(all)->u.a.len;
        for (uint32_t i = 0; i < n; i++) {
            JSValue k = OBJ(all)->u.a.v[i];
            if (JS_IsSymbol(k) ? !(flags & OK_SYMBOLS) : !(flags & OK_STRINGS)) continue;
            if (flags & OK_ENUM) {
                JSDesc d;
                int r = js_get_own_desc(ctx, objv, k, &d);
                if (r < 0) return JS_EXCEPTION;
                if (!r || !(d.flags & P_E)) continue;
            }
            if (push_key(ctx, out, k) < 0) return JS_EXCEPTION;
        }
        return out;
    }
    js_materialize(ctx, o);
    JSValue out = js_array_new(ctx, 0);
    if (JS_IsException(out)) return out;
    if (flags & OK_STRINGS) {
        /* integer indices first, ascending */
        uint32_t *ix = NULL, nix = 0, cix = 0;
        if ((o->h.cls == CL_ARRAY || o->h.cls == CL_ARGUMENTS) && !(o->h.flags & OF_SLOW)) {
            uint32_t n = MIN(o->u.a.len, o->u.a.cap);
            for (uint32_t i = 0; i < n; i++) {
                if (OBJ(objv)->u.a.v[i] == JS_HOLE) continue;
                JSValue k = js_atom_from_index(ctx, i);
                if (JS_IsException(k) || push_key(ctx, out, k) < 0) return JS_EXCEPTION;
            }
        } else if (o->h.cls == CL_STRING) {
            uint32_t n = js_strlen(o->u.prim);
            for (uint32_t i = 0; i < n; i++) {
                JSValue k = js_atom_from_index(ctx, i);
                if (JS_IsException(k) || push_key(ctx, out, k) < 0) return JS_EXCEPTION;
            }
        } else if (o->h.cls == CL_TYPEDARRAY) {
            uint32_t n = js_ta_length(o);
            for (uint32_t i = 0; i < n; i++) {
                JSValue k = js_atom_from_index(ctx, i);
                if (JS_IsException(k) || push_key(ctx, out, k) < 0) return JS_EXCEPTION;
            }
        }
        for (uint32_t i = 0; i < o->nprops; i++) {
            uint32_t idx;
            if (!js_atom_index(o->props[i].key, &idx)) continue;
            if ((flags & OK_ENUM) && !(o->props[i].flags & P_E)) continue;
            if (nix >= cix) {
                cix = cix ? cix * 2 : 16;
                uint32_t *n = realloc(ix, sizeof(uint32_t) * cix);
                if (!n) { free(ix); return js_throw_oom(ctx); }
                ix = n;
            }
            ix[nix++] = idx;
        }
        if (nix) {
            qsort(ix, nix, sizeof(uint32_t), u32cmp);
            for (uint32_t i = 0; i < nix; i++) {
                JSValue k = js_atom_from_index(ctx, ix[i]);
                if (JS_IsException(k) || push_key(ctx, out, k) < 0) { free(ix); return JS_EXCEPTION; }
            }
        }
        free(ix);
        if (o->h.cls == CL_STRING && !(flags & OK_ENUM)) {
            if (push_key(ctx, out, ctx->a[A_length]) < 0) return JS_EXCEPTION;
        }
        if (o->h.cls == CL_ARRAY && !(flags & OK_ENUM)) {
            if (push_key(ctx, out, ctx->a[A_length]) < 0) return JS_EXCEPTION;
        }
        for (uint32_t i = 0; i < o->nprops; i++) {
            JSValue k = o->props[i].key;
            uint32_t idx;
            if (!JS_IsString(k) || js_atom_index(k, &idx)) continue;
            if ((flags & OK_ENUM) && !(o->props[i].flags & P_E)) continue;
            if (push_key(ctx, out, k) < 0) return JS_EXCEPTION;
        }
        if (o->h.cls == CL_HOST) {
            int c = o->u.host.cls;
            if (c > 0 && c <= ctx->nclasses && ctx->classes[c - 1].c.keys) {
                JSValue extra = ctx->classes[c - 1].c.keys(ctx, objv);
                if (JS_IsException(extra)) return extra;
                if (js_is_cls(extra, CL_ARRAY))
                    for (uint32_t i = 0; i < OBJ(extra)->u.a.len; i++) {
                        JSValue k = js_atom(ctx, OBJ(extra)->u.a.v[i]);
                        if (JS_IsException(k) || push_key(ctx, out, k) < 0) return JS_EXCEPTION;
                    }
            }
        }
    }
    if (flags & OK_SYMBOLS) {
        for (uint32_t i = 0; i < o->nprops; i++) {
            JSValue k = o->props[i].key;
            if (!JS_IsSymbol(k)) continue;
            if ((SYM(k)->h.flags & SYM_PRIVATE) && !(flags & OK_PRIVATE)) continue;
            if ((flags & OK_ENUM) && !(o->props[i].flags & P_E)) continue;
            if (push_key(ctx, out, k) < 0) return JS_EXCEPTION;
        }
    }
    return out;
}

/* ==== prototypes / extensibility ========================================================================== */
JSObject *js_get_proto_obj(JSContext *ctx, JSValue v, bool *exc) {
    *exc = false;
    if (!JS_IsObject(v)) return proto_of_prim(ctx, v);
    JSObject *o = OBJ(v);
    if (o->h.cls == CL_PROXY) {
        JSValue h, t = proxy_trap(ctx, o, A_getPrototypeOf, &h);
        if (JS_IsException(t)) { *exc = true; return NULL; }
        if (JS_IsUndefined(t)) return js_get_proto_obj(ctx, o->u.px.target, exc);
        JSValue args[1] = { o->u.px.target };
        JSValue r = js_callv(ctx, t, h, 1, args);
        if (JS_IsException(r)) { *exc = true; return NULL; }
        if (JS_IsObject(r)) return OBJ(r);
        if (!JS_IsNull(r)) { js_throw_type(ctx, "'getPrototypeOf' on proxy: trap returned neither object nor null"); *exc = true; }
        return NULL;
    }
    return o->proto;
}

int js_set_proto_obj(JSContext *ctx, JSValue v, JSObject *proto, bool throw) {
    JSObject *o = OBJ(v);
    if (o->h.cls == CL_PROXY) {
        JSValue h, t = proxy_trap(ctx, o, A_setPrototypeOf, &h);
        if (JS_IsException(t)) return -1;
        if (JS_IsUndefined(t)) return js_set_proto_obj(ctx, o->u.px.target, proto, throw);
        JSValue args[2] = { o->u.px.target, proto ? JS_MKOBJ(proto) : JS_NULL };
        JSValue r = js_callv(ctx, t, h, 2, args);
        if (JS_IsException(r)) return -1;
        if (!js_truthy(r)) { if (throw) { js_throw_type(ctx, "'setPrototypeOf' on proxy: trap returned falsish"); return -1; } return 0; }
        return 1;
    }
    if (o->proto == proto) return 1;
    if (!(o->h.flags & OF_EXT) || o == ctx->proto_object) {
        if (throw) { js_throw_type(ctx, "#<Object> is not extensible"); return -1; }
        return 0;
    }
    for (JSObject *p = proto; p; p = p->proto) {
        if (p == o) { if (throw) { js_throw_type(ctx, "Cyclic __proto__ value"); return -1; } return 0; }
        if (p->h.cls == CL_PROXY) break;
    }
    o->proto = proto;
    return 1;
}

int js_prevent_ext(JSContext *ctx, JSValue v) {
    JSObject *o = OBJ(v);
    if (o->h.cls == CL_PROXY) {
        JSValue h, t = proxy_trap(ctx, o, A_preventExtensions, &h);
        if (JS_IsException(t)) return -1;
        if (JS_IsUndefined(t)) return js_prevent_ext(ctx, o->u.px.target);
        JSValue args[1] = { o->u.px.target };
        JSValue r = js_callv(ctx, t, h, 1, args);
        if (JS_IsException(r)) return -1;
        return js_truthy(r);
    }
    js_materialize(ctx, o);
    o->h.flags &= (uint8_t)~OF_EXT;
    return 1;
}

int js_is_extensible(JSContext *ctx, JSValue v) {
    if (!JS_IsObject(v)) return 0;
    JSObject *o = OBJ(v);
    if (o->h.cls == CL_PROXY) {
        JSValue h, t = proxy_trap(ctx, o, A_isExtensible, &h);
        if (JS_IsException(t)) return -1;
        if (JS_IsUndefined(t)) return js_is_extensible(ctx, o->u.px.target);
        JSValue args[1] = { o->u.px.target };
        JSValue r = js_callv(ctx, t, h, 1, args);
        if (JS_IsException(r)) return -1;
        return js_truthy(r);
    }
    return (o->h.flags & OF_EXT) != 0;
}

/* ==== descriptors <-> objects ============================================================================= */
JSValue js_desc_to_obj(JSContext *ctx, const JSDesc *d) {
    JSValue o = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
    if (JS_IsException(o)) return o;
    if (d->flags & (DP_HAS_GET | DP_HAS_SET | P_ACC)) {
        js_add_prop(ctx, OBJ(o), ctx->a[A_get], d->get, P_W | P_E | P_C);
        js_add_prop(ctx, OBJ(o), ctx->a[A_set], d->set, P_W | P_E | P_C);
    } else {
        if (d->flags & DP_HAS_VALUE) js_add_prop(ctx, OBJ(o), ctx->a[A_value], d->value, P_W | P_E | P_C);
        if (d->flags & DP_HAS_W) js_add_prop(ctx, OBJ(o), ctx->a[A_writable], JS_BOOL(d->flags & P_W), P_W | P_E | P_C);
    }
    if (d->flags & DP_HAS_E) js_add_prop(ctx, OBJ(o), ctx->a[A_enumerable], JS_BOOL(d->flags & P_E), P_W | P_E | P_C);
    if (d->flags & DP_HAS_C) js_add_prop(ctx, OBJ(o), ctx->a[A_configurable], JS_BOOL(d->flags & P_C), P_W | P_E | P_C);
    return o;
}

int js_obj_to_desc(JSContext *ctx, JSValue o, JSDesc *d) {
    if (!JS_IsObject(o)) { js_throw_type(ctx, "Property description must be an object"); return -1; }
    d->value = d->get = d->set = JS_UNDEFINED;
    d->flags = 0;
    static const int atoms[6] = { A_enumerable, A_configurable, A_value, A_writable, A_get, A_set };
    for (int i = 0; i < 6; i++) {
        JSValue k = ctx->a[atoms[i]];
        int h = js_has_prop(ctx, o, k);
        if (h < 0) return -1;
        if (!h) continue;
        JSValue v = js_getp(ctx, o, k);
        if (JS_IsException(v)) return -1;
        switch (i) {
        case 0: d->flags |= DP_HAS_E | (js_truthy(v) ? P_E : 0); break;
        case 1: d->flags |= DP_HAS_C | (js_truthy(v) ? P_C : 0); break;
        case 2: d->flags |= DP_HAS_VALUE; d->value = v; break;
        case 3: d->flags |= DP_HAS_W | (js_truthy(v) ? P_W : 0); break;
        case 4:
            if (!JS_IsUndefined(v) && !js_is_callable(v)) { js_throw_type(ctx, "Getter must be a function"); return -1; }
            d->flags |= DP_HAS_GET; d->get = v; break;
        case 5:
            if (!JS_IsUndefined(v) && !js_is_callable(v)) { js_throw_type(ctx, "Setter must be a function"); return -1; }
            d->flags |= DP_HAS_SET; d->set = v; break;
        }
    }
    if ((d->flags & (DP_HAS_GET | DP_HAS_SET)) && (d->flags & (DP_HAS_VALUE | DP_HAS_W))) {
        js_throw_type(ctx, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
        return -1;
    }
    return 0;
}

/* ==== conversions ============================================================================================ */
bool js_truthy(JSValue v) {
    if (JS_IsNumber(v)) { double d = js_num(v); return d == d && d != 0; }
    if (v == JS_TRUE) return true;
    if (JS_TAGOF(v) == JS_TAG_SPECIAL) return false;
    if (JS_IsString(v)) return js_strlen(v) != 0;
    return true;
}

bool js_is_callable(JSValue v) {
    if (!JS_IsObject(v)) return false;
    int c = OBJ(v)->h.cls;
    if (c == CL_FUNCTION || c == CL_NATIVE || c == CL_BOUND) return true;
    if (c == CL_PROXY) return js_is_callable(OBJ(v)->u.px.target);
    return false;
}

bool js_is_ctor(JSValue v) {
    if (!JS_IsObject(v)) return false;
    JSObject *o = OBJ(v);
    switch (o->h.cls) {
    case CL_FUNCTION: {
        uint32_t f = o->u.f.b->fflags;
        if (f & (F_CTOR | F_DERIVED | F_CLASSCTOR)) return true;
        return !(f & (F_ARROW | F_METHOD | F_GEN | F_ASYNC | F_SCRIPT | F_FIELDINIT));
    }
    case CL_NATIVE: return (o->h.flags & OF_CTOR) != 0;
    case CL_BOUND: return js_is_ctor(o->u.bf.target);
    case CL_PROXY: return js_is_ctor(o->u.px.target);
    default: return false;
    }
}

int js_is_array_v(JSContext *ctx, JSValue v) {
    if (!JS_IsObject(v)) return 0;
    JSObject *o = OBJ(v);
    if (o->h.cls == CL_ARRAY) return 1;
    if (o->h.cls == CL_PROXY) {
        if (!JS_IsObject(o->u.px.handler)) { js_throw_type(ctx, "Cannot perform 'IsArray' on a proxy that has been revoked"); return -1; }
        return js_is_array_v(ctx, o->u.px.target);
    }
    return 0;
}

JSValue js_get_method(JSContext *ctx, JSValue v, JSValue key) {
    JSValue f = js_getp(ctx, v, key);
    if (JS_IsException(f) || JS_IsNullish(f)) return JS_IsException(f) ? f : JS_UNDEFINED;
    if (!js_is_callable(f)) {
        char k[80];
        js_value_desc(ctx, key, k, sizeof k);
        return js_throw_type(ctx, "%s is not a function", k);
    }
    return f;
}

JSValue js_invoke(JSContext *ctx, JSValue obj, JSValue key, int argc, JSValue *argv) {
    JSValue f = js_getp(ctx, obj, key);
    if (JS_IsException(f)) return f;
    if (!js_is_callable(f)) {
        char k[80];
        js_value_desc(ctx, key, k, sizeof k);
        return js_throw_type(ctx, "%s is not a function", k);
    }
    return js_callv(ctx, f, obj, argc, argv);
}

static JSValue ordinary_to_primitive(JSContext *ctx, JSValue v, int hint) {
    int order[2] = { A_valueOf, A_toString };
    if (hint == 2) { order[0] = A_toString; order[1] = A_valueOf; }
    for (int i = 0; i < 2; i++) {
        JSValue f = js_getp(ctx, v, ctx->a[order[i]]);
        if (JS_IsException(f)) return f;
        if (js_is_callable(f)) {
            JSValue r = js_callv(ctx, f, v, 0, NULL);
            if (JS_IsException(r) || !JS_IsObject(r)) return r;
        }
    }
    return js_throw_type(ctx, "Cannot convert object to primitive value");
}

JSValue js_to_primitive(JSContext *ctx, JSValue v, int hint) {
    if (!JS_IsObject(v)) return v;
    JSValue ex = js_getp(ctx, v, ctx->wk[WK_TO_PRIMITIVE]);
    if (JS_IsException(ex)) return ex;
    if (!JS_IsNullish(ex)) {
        if (!js_is_callable(ex)) return js_throw_type(ctx, "Symbol.toPrimitive is not a function");
        JSValue h = ctx->a[hint == 1 ? A_number : hint == 2 ? A_string : A_default];
        JSValue r = js_callv(ctx, ex, v, 1, &h);
        if (JS_IsException(r)) return r;
        if (JS_IsObject(r)) return js_throw_type(ctx, "Cannot convert object to primitive value");
        return r;
    }
    return ordinary_to_primitive(ctx, v, hint == 0 ? 1 : hint);
}

double js_tonum(JSContext *ctx, JSValue v, bool *exc) {
    for (;;) {
        if (JS_IsNumber(v)) return js_num(v);
        switch (JS_TAGOF(v)) {
        case JS_TAG_SPECIAL:
            if (v == JS_TRUE) return 1;
            if (v == JS_FALSE || v == JS_NULL) return 0;
            return NAN;
        case JS_TAG_STRING: {
            JSString *s = js_flat(ctx, v);
            if (!s) { if (exc) *exc = true; return 0; }
            return js_str_to_number(ctx, s);
        }
        case JS_TAG_SYMBOL:
            js_throw_type(ctx, "Cannot convert a Symbol value to a number");
            if (exc) *exc = true;
            return 0;
        case JS_TAG_OBJECT:
            v = js_to_primitive(ctx, v, 1);
            if (JS_IsException(v)) { if (exc) *exc = true; return 0; }
            continue;
        default:
            js_throw_type(ctx, "Cannot convert value to a number");
            if (exc) *exc = true;
            return 0;
        }
    }
}

JSValue js_tonumeric(JSContext *ctx, JSValue v) {
    if (JS_IsNumber(v)) return v;
    bool exc = false;
    double d = js_tonum(ctx, v, &exc);
    return exc ? JS_EXCEPTION : js_mknum(d);
}

JSValue js_tostr(JSContext *ctx, JSValue v) {
    for (;;) {
        if (JS_IsString(v)) return v;
        if (JS_IsNumber(v)) return js_num_to_str(ctx, js_num(v));
        switch (JS_TAGOF(v)) {
        case JS_TAG_SPECIAL:
            if (v == JS_UNDEFINED) return ctx->a[A_undefined];
            if (v == JS_NULL) return ctx->a[A_null];
            if (v == JS_TRUE) return ctx->a[A_true];
            if (v == JS_FALSE) return ctx->a[A_false];
            return ctx->empty_str;
        case JS_TAG_SYMBOL:
            return js_throw_type(ctx, "Cannot convert a Symbol value to a string");
        case JS_TAG_OBJECT:
            v = js_to_primitive(ctx, v, 2);
            if (JS_IsException(v)) return v;
            continue;
        default:
            return js_throw_type(ctx, "Cannot convert value to a string");
        }
    }
}

JSValue js_toobj(JSContext *ctx, JSValue v) {
    if (JS_IsObject(v)) return v;
    if (JS_IsNullish(v)) return js_throw_type(ctx, "Cannot convert undefined or null to object");
    int cls = JS_IsString(v) ? CL_STRING : JS_IsNumber(v) ? CL_NUMBER : JS_IsBool(v) ? CL_BOOLEAN :
              JS_IsSymbol(v) ? CL_SYMBOL : CL_BIGINT;
    JSObject *o = js_obj_alloc(ctx, cls, proto_of_prim(ctx, v));
    if (!o) return JS_EXCEPTION;
    o->u.prim = v;
    return JS_MKOBJ(o);
}

int32_t js_d2i32(double d) { return (int32_t)js_d2u32(d); }

uint32_t js_d2u32(double d) {
    if (d >= -2147483648.0 && d <= 2147483647.0) {
        int32_t i = (int32_t)d;    /* truncates */
        return (uint32_t)i;
    }
    if (d != d || isinf(d)) return 0;
    double t = trunc(d);
    t = fmod(t, 4294967296.0);
    if (t < 0) t += 4294967296.0;
    return (uint32_t)(uint64_t)t;
}

int js_toint32_e(JSContext *ctx, JSValue v, int32_t *out) {
    if (JS_IsNumber(v)) { *out = js_d2i32(js_num(v)); return 0; }
    bool exc = false;
    double d = js_tonum(ctx, v, &exc);
    if (exc) return -1;
    *out = js_d2i32(d);
    return 0;
}

double js_tointeger(JSContext *ctx, JSValue v, bool *exc) {
    double d = JS_IsNumber(v) ? js_num(v) : js_tonum(ctx, v, exc);
    if (d != d) return 0;
    if (isinf(d)) return d;
    return trunc(d) + 0.0;
}

int js_toindex(JSContext *ctx, JSValue v, double *out) {
    if (JS_IsUndefined(v)) { *out = 0; return 0; }
    bool exc = false;
    double d = js_tointeger(ctx, v, &exc);
    if (exc) return -1;
    if (d < 0 || d > 9007199254740991.0) { js_throw_range(ctx, "Invalid index"); return -1; }
    *out = d;
    return 0;
}

/* ==== equality and comparison ========================================================================== */
bool js_same_value(JSValue a, JSValue b, bool zero) {
    if (JS_IsNumber(a) && JS_IsNumber(b)) {
        double x = js_num(a), y = js_num(b);
        if (x != x) return y != y;
        if (x == 0 && y == 0) return zero || (signbit(x) == signbit(y));
        return x == y;
    }
    if (JS_IsString(a) && JS_IsString(b)) {
        if (a == b) return true;
        JSString *x = JS_PTR(a);
        JSString *y = JS_PTR(b);
        uint32_t la = js_strlen(a), lb = js_strlen(b);
        if (la != lb) return false;
        if (x->h.type == T_STRING && y->h.type == T_STRING) return !memcmp(x->s, y->s, (size_t)la * 2);
        return false;   /* (ropes: the callers flatten first via js_strict_eq) */
    }
    return a == b;
}

bool js_strict_eq(JSContext *ctx, JSValue a, JSValue b) {
    if (JS_IsNumber(a)) return JS_IsNumber(b) && js_num(a) == js_num(b);
    if (JS_IsString(a)) return JS_IsString(b) && js_streq(ctx, a, b);
    return a == b;
}

int js_loose_eq(JSContext *ctx, JSValue a, JSValue b) {
    for (;;) {
        if (JS_IsNumber(a) && JS_IsNumber(b)) return js_num(a) == js_num(b);
        if (JS_IsString(a) && JS_IsString(b)) return js_streq(ctx, a, b);
        uint32_t ta = JS_TAGOF(a), tb = JS_TAGOF(b);
        if (!JS_IsNumber(a) && !JS_IsNumber(b) && ta == tb && ta != JS_TAG_SPECIAL) return a == b;
        if (JS_IsNullish(a) || JS_IsNullish(b)) return JS_IsNullish(a) && JS_IsNullish(b);
        if (JS_IsBool(a) && JS_IsBool(b)) return a == b;
        if (a == JS_HOLE || b == JS_HOLE) return a == b;
        if (JS_IsNumber(a) && JS_IsString(b)) { bool e = false; double d = js_tonum(ctx, b, &e); if (e) return -1; return js_num(a) == d; }
        if (JS_IsString(a) && JS_IsNumber(b)) { bool e = false; double d = js_tonum(ctx, a, &e); if (e) return -1; return d == js_num(b); }
        if (JS_IsBool(a)) { a = js_mknum(a == JS_TRUE); continue; }
        if (JS_IsBool(b)) { b = js_mknum(b == JS_TRUE); continue; }
        if (JS_IsObject(b) && !JS_IsObject(a)) {
            b = js_to_primitive(ctx, b, 0);
            if (JS_IsException(b)) return -1;
            continue;
        }
        if (JS_IsObject(a) && !JS_IsObject(b)) {
            a = js_to_primitive(ctx, a, 0);
            if (JS_IsException(a)) return -1;
            continue;
        }
        return 0;
    }
}

int js_less(JSContext *ctx, JSValue a, JSValue b, bool left_first) {
    if (JS_IsNumber(a) && JS_IsNumber(b)) {
        double x = js_num(a), y = js_num(b);
        if (x != x || y != y) return 2;
        return x < y;
    }
    JSValue pa, pb;
    if (left_first) {
        pa = js_to_primitive(ctx, a, 1);
        if (JS_IsException(pa)) return -1;
        pb = js_to_primitive(ctx, b, 1);
        if (JS_IsException(pb)) return -1;
    } else {
        pb = js_to_primitive(ctx, b, 1);
        if (JS_IsException(pb)) return -1;
        pa = js_to_primitive(ctx, a, 1);
        if (JS_IsException(pa)) return -1;
    }
    if (JS_IsString(pa) && JS_IsString(pb)) return js_strcmp(ctx, pa, pb) < 0;
    bool e = false;
    double x = js_tonum(ctx, pa, &e);
    if (e) return -1;
    double y = js_tonum(ctx, pb, &e);
    if (e) return -1;
    if (x != x || y != y) return 2;
    return x < y;
}

JSValue js_add(JSContext *ctx, JSValue a, JSValue b) {
    if (JS_IsNumber(a) && JS_IsNumber(b)) return js_mknum(js_num(a) + js_num(b));
    if (JS_IsString(a) && JS_IsString(b)) return js_concat(ctx, a, b);
    JSValue pa = js_to_primitive(ctx, a, 0);
    if (JS_IsException(pa)) return pa;
    JSValue pb = js_to_primitive(ctx, b, 0);
    if (JS_IsException(pb)) return pb;
    if (JS_IsString(pa) || JS_IsString(pb)) {
        JSValue sa = js_tostr(ctx, pa);
        if (JS_IsException(sa)) return sa;
        JSValue sb = js_tostr(ctx, pb);
        if (JS_IsException(sb)) return sb;
        return js_concat(ctx, sa, sb);
    }
    bool e = false;
    double x = js_tonum(ctx, pa, &e);
    if (e) return JS_EXCEPTION;
    double y = js_tonum(ctx, pb, &e);
    if (e) return JS_EXCEPTION;
    return js_mknum(x + y);
}

JSValue js_typeof(JSContext *ctx, JSValue v) {
    if (JS_IsNumber(v)) return ctx->a[A_number];
    switch (JS_TAGOF(v)) {
    case JS_TAG_STRING: return ctx->a[A_string];
    case JS_TAG_SYMBOL: return ctx->a[A_symbol];
    case JS_TAG_BIGINT: return ctx->a[A_bigint];
    case JS_TAG_OBJECT: return js_is_callable(v) ? ctx->a[A_function] : ctx->a[A_object];
    default:
        if (v == JS_NULL) return ctx->a[A_object];
        if (JS_IsBool(v)) return ctx->a[A_boolean];
        return ctx->a[A_undefined];
    }
}

int js_ordinary_has_instance(JSContext *ctx, JSValue ctor, JSValue v) {
    if (!js_is_callable(ctor)) return 0;
    if (js_is_cls(ctor, CL_BOUND)) return js_instanceof(ctx, v, OBJ(ctor)->u.bf.target);
    if (!JS_IsObject(v)) return 0;
    JSValue p = js_getp(ctx, ctor, ctx->a[A_prototype]);
    if (JS_IsException(p)) return -1;
    if (!JS_IsObject(p)) { js_throw_type(ctx, "Function has non-object prototype in instanceof check"); return -1; }
    bool exc;
    JSObject *o = js_get_proto_obj(ctx, v, &exc);
    if (exc) return -1;
    while (o) {
        if (o == OBJ(p)) return 1;
        if (o->h.cls == CL_PROXY) { o = js_get_proto_obj(ctx, JS_MKOBJ(o), &exc); if (exc) return -1; continue; }
        o = o->proto;
    }
    return 0;
}

int js_instanceof(JSContext *ctx, JSValue v, JSValue ctor) {
    if (!JS_IsObject(ctor)) { js_throw_type(ctx, "Right-hand side of 'instanceof' is not an object"); return -1; }
    JSValue h = js_getp(ctx, ctor, ctx->wk[WK_HAS_INSTANCE]);
    if (JS_IsException(h)) return -1;
    if (h == ctx->fn_has_instance) return js_ordinary_has_instance(ctx, ctor, v);
    if (!JS_IsNullish(h)) {
        if (!js_is_callable(h)) { js_throw_type(ctx, "Symbol.hasInstance is not callable"); return -1; }
        JSValue r = js_callv(ctx, h, ctor, 1, &v);
        if (JS_IsException(r)) return -1;
        return js_truthy(r);
    }
    if (!js_is_callable(ctor)) { js_throw_type(ctx, "Right-hand side of 'instanceof' is not callable"); return -1; }
    return js_ordinary_has_instance(ctx, ctor, v);
}

JSValue js_species_ctor(JSContext *ctx, JSValue obj, JSObject *def) {
    JSValue c = js_getp(ctx, obj, ctx->a[A_constructor]);
    if (JS_IsException(c)) return c;
    if (JS_IsUndefined(c)) return JS_MKOBJ(def);
    if (!JS_IsObject(c)) return js_throw_type(ctx, "object.constructor is not an object");
    JSValue s = js_getp(ctx, c, ctx->wk[WK_SPECIES]);
    if (JS_IsException(s)) return s;
    if (JS_IsNullish(s)) return JS_MKOBJ(def);
    if (js_is_ctor(s)) return s;
    return js_throw_type(ctx, "object.constructor[Symbol.species] is not a constructor");
}

/* ==== native functions ======================================================================================= */
JSValue js_mk_native(JSContext *ctx, JSNativeFn fn, JSValue name, int len, int magic) {
    JSObject *o = js_obj_alloc(ctx, CL_NATIVE, ctx->proto_function);
    if (!o) return JS_EXCEPTION;
    o->h.flags |= OF_LAZY;
    o->u.n.fn = fn;
    o->u.n.data = JS_UNDEFINED;
    o->u.n.name = name;
    o->u.n.length = (int16_t)len;
    o->u.n.magic = (int16_t)magic;
    return JS_MKOBJ(o);
}

JSValue js_def_native(JSContext *ctx, JSValue obj, const char *name, JSNativeFn fn, int len, int magic) {
    JSValue key;
    if (name[0] == '@') {   /* "@iterator" etc: a well-known symbol */
        static const char *wk[] = { "@iterator", "@asyncIterator", "@hasInstance", "@toPrimitive", "@toStringTag",
                                    "@species", "@isConcatSpreadable", "@unscopables", "@match", "@matchAll",
                                    "@replace", "@search", "@split" };
        int i;
        for (i = 0; i < WK_COUNT; i++) if (!strcmp(name, wk[i])) break;
        if (i == WK_COUNT) return JS_EXCEPTION;
        key = ctx->wk[i];
        char nm[48];
        snprintf(nm, sizeof nm, "[Symbol.%s]", name + 1);
        JSValue f = js_mk_native(ctx, fn, js_atomc(ctx, nm), len, magic);
        if (JS_IsException(f)) return f;
        js_add_prop(ctx, OBJ(obj), key, f, (i == WK_HAS_INSTANCE || i == WK_TO_PRIMITIVE) ? (i == WK_TO_PRIMITIVE ? P_C : 0) : P_W | P_C);
        return f;
    }
    key = js_atomc(ctx, name);
    JSValue f = js_mk_native(ctx, fn, key, len, magic);
    if (JS_IsException(f)) return f;
    JSProp *p = js_find_prop(ctx, OBJ(obj), key);
    if (p) { p->val = f; p->flags = P_W | P_C; }
    else js_add_prop(ctx, OBJ(obj), key, f, P_W | P_C);
    return f;
}

void js_def_funcs(JSContext *ctx, JSValue obj, const JSFuncDef *d, int n) {
    for (int i = 0; i < n; i++) js_def_native(ctx, obj, d[i].name, d[i].fn, d[i].len, d[i].magic);
}

void js_def_accessor(JSContext *ctx, JSValue obj, JSValue key, JSNativeFn get, JSNativeFn set, int magic, uint32_t flags) {
    char nm[96], ks[80];
    if (JS_IsSymbol(key)) { js_value_desc(ctx, key, ks, sizeof ks); }
    else js_str_to_ascii(ctx, key, ks, sizeof ks);
    JSValue g = JS_UNDEFINED, s = JS_UNDEFINED;
    if (get) { snprintf(nm, sizeof nm, "get %s", ks); g = js_mk_native(ctx, get, js_atomc(ctx, nm), 0, magic); }
    if (set) { snprintf(nm, sizeof nm, "set %s", ks); s = js_mk_native(ctx, set, js_atomc(ctx, nm), 1, magic); }
    JSAccessor *a = new_accessor(ctx, g, s);
    if (!a) return;
    JSProp *p = js_find_prop(ctx, OBJ(obj), key);
    if (p) { p->val = JS_MKINT(a); p->flags = (flags & (P_E | P_C)) | P_ACC; }
    else js_add_prop(ctx, OBJ(obj), key, JS_MKINT(a), (flags & (P_E | P_C)) | P_ACC);
}

void js_set_func_name(JSContext *ctx, JSValue fn, JSValue key, const char *prefix) {
    if (!JS_IsObject(fn)) return;
    JSObject *o = OBJ(fn);
    JSValue name;
    if (JS_IsSymbol(key)) {
        JSValue d = SYM(key)->desc;
        if (JS_IsUndefined(d)) name = ctx->empty_str;
        else {
            JSSB b;
            sb_init(ctx, &b);
            sb_putc(&b, '[');
            sb_putv(&b, d);
            sb_putc(&b, ']');
            name = sb_finish(&b);
        }
    } else name = key;
    if (prefix) {
        JSSB b;
        sb_init(ctx, &b);
        sb_puts(&b, prefix);
        sb_putc(&b, ' ');
        sb_putv(&b, name);
        name = sb_finish(&b);
    }
    if (JS_IsException(name)) return;
    if (o->h.cls == CL_FUNCTION && (o->h.flags & OF_LAZY)) {
        /* still lazy: materialize with the new name */
        js_materialize(ctx, o);
    }
    JSProp *p = js_find_prop(ctx, o, ctx->a[A_name]);
    if (p && !(p->flags & P_ACC)) { if (p->flags & P_C) p->val = name; }
    else if (!p) js_add_prop(ctx, o, ctx->a[A_name], name, P_C);
}

JSObject *js_new_ctor(JSContext *ctx, const char *name, JSNativeFn fn, int len, JSObject *proto, int magic) {
    JSValue f = js_mk_native(ctx, fn, js_atomc(ctx, name), len, magic);
    if (JS_IsException(f)) return NULL;
    JSObject *o = OBJ(f);
    o->h.flags |= OF_CTOR;
    js_materialize(ctx, o);
    if (proto) {
        js_add_prop(ctx, o, ctx->a[A_prototype], JS_MKOBJ(proto), 0);
        js_add_prop(ctx, proto, ctx->a[A_constructor], f, P_W | P_C);
    }
    js_add_prop(ctx, OBJ(ctx->global), js_atomc(ctx, name), f, P_W | P_C);
    return o;
}

/* ==== errors =================================================================================================== */
static const char *err_names[JS_NERRORS] = { "Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError",
                                             "EvalError", "URIError", "AggregateError" };

void js_error_add_stack(JSContext *ctx, JSValue err) {
    if (!JS_IsObject(err)) return;
    JSSB b;
    sb_init(ctx, &b);
    JSValue s = js_error_to_string_v(ctx, err);
    if (JS_IsException(s)) { js_take_exception(ctx); s = ctx->a[A_empty]; }
    sb_putv(&b, s);
    js_stack_trace(ctx, &b);
    JSValue st = sb_finish(&b);
    if (JS_IsException(st)) { js_take_exception(ctx); return; }
    JSProp *p = js_find_prop(ctx, OBJ(err), ctx->a[A_stack]);
    if (p) p->val = st;
    else js_add_prop(ctx, OBJ(err), ctx->a[A_stack], st, P_W | P_C);
}

JSValue js_make_error(JSContext *ctx, int type, JSValue msg) {
    JSObject *proto = ctx->proto_errors[type] ? ctx->proto_errors[type] : ctx->proto_object;
    JSObject *o = js_obj_alloc(ctx, CL_ERROR, proto);
    if (!o) return JS_EXCEPTION;
    JSValue e = JS_MKOBJ(o);
    if (JS_IsString(msg)) js_add_prop(ctx, o, ctx->a[A_message], msg, P_W | P_C);
    js_error_add_stack(ctx, e);
    return e;
}

JSValue js_new_error(JSContext *ctx, int type, const char *msg) {
    JSValue m = js_str_utf8(ctx, msg, strlen(msg));
    if (JS_IsException(m)) return m;
    return js_make_error(ctx, type, m);
}

JSValue js_throw(JSContext *ctx, JSValue v) {
    ctx->exception = v;
    ctx->has_exc = true;
    return JS_EXCEPTION;
}

static JSValue throw_v(JSContext *ctx, int type, const char *fmt, va_list ap) {
    char buf[400];
    vsnprintf(buf, sizeof buf, fmt, ap);
    if (ctx->in_gc) { ctx->has_exc = true; ctx->exception = JS_NULL; return JS_EXCEPTION; }
    JSValue e = js_new_error(ctx, type, buf);
    if (JS_IsException(e)) return e;
    return js_throw(ctx, e);
}

JSValue js_throw_error(JSContext *ctx, int type, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); JSValue r = throw_v(ctx, type, fmt, ap); va_end(ap); return r;
}
JSValue js_throw_type(JSContext *ctx, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); JSValue r = throw_v(ctx, JS_TYPE_ERROR, fmt, ap); va_end(ap); return r;
}
JSValue js_throw_range(JSContext *ctx, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); JSValue r = throw_v(ctx, JS_RANGE_ERROR, fmt, ap); va_end(ap); return r;
}
JSValue js_throw_ref(JSContext *ctx, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); JSValue r = throw_v(ctx, JS_REFERENCE_ERROR, fmt, ap); va_end(ap); return r;
}
JSValue js_throw_syntax(JSContext *ctx, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); JSValue r = throw_v(ctx, JS_SYNTAX_ERROR, fmt, ap); va_end(ap); return r;
}

JSValue js_throw_oom(JSContext *ctx) {
    if (ctx->in_gc || ctx->oom_throwing) { ctx->has_exc = true; ctx->exception = JS_NULL; return JS_EXCEPTION; }
    ctx->oom_throwing = true;
    /* a plain message: making an error object may itself need memory */
    ctx->has_exc = true;
    ctx->exception = JS_IsString(ctx->a[A_empty]) ? js_str8(ctx, "Out of memory", 13) : JS_NULL;
    if (JS_IsException(ctx->exception)) ctx->exception = JS_NULL;
    ctx->has_exc = true;
    ctx->oom_throwing = false;
    return JS_EXCEPTION;
}

JSValue js_take_exception(JSContext *ctx) {
    JSValue e = ctx->exception;
    ctx->exception = JS_UNDEFINED;
    ctx->has_exc = false;
    return e;
}

bool js_has_exception(JSContext *ctx) { return ctx->has_exc; }

void js_value_desc(JSContext *ctx, JSValue v, char *out, size_t size) {
    if (JS_IsString(v)) {
        char tmp[120];
        js_str_to_ascii(ctx, v, tmp, sizeof tmp);
        snprintf(out, size, "'%s'", tmp);
    } else if (JS_IsSymbol(v)) {
        char tmp[100] = "";
        if (JS_IsString(SYM(v)->desc)) js_str_to_ascii(ctx, SYM(v)->desc, tmp, sizeof tmp);
        snprintf(out, size, "Symbol(%s)", tmp);
    } else if (JS_IsNumber(v)) {
        js_dtoa(js_num(v), out);
    } else if (JS_IsObject(v)) {
        snprintf(out, size, js_is_callable(v) ? "function" : js_is_cls(v, CL_ARRAY) ? "array" : "object");
    } else snprintf(out, size, "%s", v == JS_NULL ? "null" : v == JS_TRUE ? "true" : v == JS_FALSE ? "false" : "undefined");
}

const char *js_class_name(int cls) {
    switch (cls) {
    case CL_ARRAY: return "Array";
    case CL_ARGUMENTS: return "Arguments";
    case CL_FUNCTION: case CL_NATIVE: case CL_BOUND: return "Function";
    case CL_ERROR: return "Error";
    case CL_BOOLEAN: return "Boolean";
    case CL_NUMBER: return "Number";
    case CL_STRING: return "String";
    case CL_DATE: return "Date";
    case CL_REGEXP: return "RegExp";
    default: return "Object";
    }
}

const char *js_error_type_name(int t) { return t >= 0 && t < JS_NERRORS ? err_names[t] : "Error"; }

/* ==== the public object API ========================================================================================= */
JSValue js_new_object(JSContext *ctx) { return js_obj_new(ctx, CL_OBJECT, ctx->proto_object); }

JSValue js_new_object_proto(JSContext *ctx, JSValue proto) {
    return js_obj_new(ctx, CL_OBJECT, JS_IsObject(proto) ? OBJ(proto) : NULL);
}

JSValue js_new_array(JSContext *ctx, uint32_t len) { return js_array_new(ctx, len); }

JSValue js_get(JSContext *ctx, JSValue obj, const char *name) {
    JSValue k = js_atomc(ctx, name);
    if (JS_IsException(k)) return k;
    return js_getp(ctx, obj, k);
}

JSValue js_get_value(JSContext *ctx, JSValue obj, JSValue key) { return js_getv(ctx, obj, key); }
JSValue js_get_index(JSContext *ctx, JSValue obj, uint32_t i) { return js_geti(ctx, obj, i); }

int js_set(JSContext *ctx, JSValue obj, const char *name, JSValue v) {
    JSValue k = js_atomc(ctx, name);
    if (JS_IsException(k)) return -1;
    return js_setp(ctx, obj, k, v, obj, true) < 0 ? -1 : 0;
}

int js_set_value(JSContext *ctx, JSValue obj, JSValue key, JSValue v) { return js_setv(ctx, obj, key, v, true) < 0 ? -1 : 0; }
int js_set_index(JSContext *ctx, JSValue obj, uint32_t i, JSValue v) { return js_seti(ctx, obj, i, v) < 0 ? -1 : 0; }

int js_define(JSContext *ctx, JSValue obj, const char *name, JSValue v, int flags) {
    JSValue k = js_atomc(ctx, name);
    if (JS_IsException(k)) return -1;
    return js_define_value(ctx, obj, k, v, (uint32_t)flags) < 0 ? -1 : 0;
}

bool js_has(JSContext *ctx, JSValue obj, const char *name) {
    JSValue k = js_atomc(ctx, name);
    return !JS_IsException(k) && js_has_prop(ctx, obj, k) > 0;
}

int js_delete(JSContext *ctx, JSValue obj, const char *name) {
    JSValue k = js_atomc(ctx, name);
    if (JS_IsException(k) || !JS_IsObject(obj)) return -1;
    return js_delete_prop(ctx, obj, k, false);
}

uint32_t js_array_length(JSContext *ctx, JSValue arr) {
    int64_t n = js_length_of(ctx, arr);
    return n < 0 ? 0 : (uint32_t)n;
}

int js_array_push(JSContext *ctx, JSValue arr, JSValue v) { return js_array_push_v(ctx, arr, v); }

JSValue js_get_prototype(JSContext *ctx, JSValue obj) {
    bool exc;
    JSObject *p = js_get_proto_obj(ctx, obj, &exc);
    if (exc) return JS_EXCEPTION;
    return p ? JS_MKOBJ(p) : JS_NULL;
}

int js_set_prototype(JSContext *ctx, JSValue obj, JSValue proto) {
    if (!JS_IsObject(obj)) return -1;
    return js_set_proto_obj(ctx, obj, JS_IsObject(proto) ? OBJ(proto) : NULL, true) < 0 ? -1 : 0;
}

JSValue js_object_keys(JSContext *ctx, JSValue obj) {
    if (!JS_IsObject(obj)) return js_array_new(ctx, 0);
    return js_own_keys(ctx, obj, OK_STRINGS | OK_ENUM);
}

JSValue js_new_function(JSContext *ctx, JSNativeFn fn, const char *name, int length, int magic) {
    return js_mk_native(ctx, fn, js_atomc(ctx, name ? name : ""), length, magic);
}

JSValue js_new_function_data(JSContext *ctx, JSNativeFn fn, const char *name, int length, int magic, JSValue data) {
    JSValue f = js_new_function(ctx, fn, name, length, magic);
    if (!JS_IsException(f)) OBJ(f)->u.n.data = data;
    return f;
}

JSValue js_func_data(JSContext *ctx) { return ctx->native_data; }
JSValue js_new_target(JSContext *ctx) { return ctx->native_nt; }

void js_def_func(JSContext *ctx, JSValue obj, const char *name, JSNativeFn fn, int length, int magic) {
    js_def_native(ctx, obj, name, fn, length, magic);
}

void js_def_getset(JSContext *ctx, JSValue obj, const char *name, JSNativeFn get, JSNativeFn set, int magic) {
    js_def_accessor(ctx, obj, js_atomc(ctx, name), get, set, magic, P_C);
}

void js_def_value(JSContext *ctx, JSValue obj, const char *name, JSValue v) {
    JSValue k = js_atomc(ctx, name);
    if (JS_IsException(k)) return;
    JSProp *p = js_find_prop(ctx, OBJ(obj), k);
    if (p) { p->val = v; p->flags = P_W | P_C; }
    else js_add_prop(ctx, OBJ(obj), k, v, P_W | P_C);
}

bool js_is_function(JSContext *ctx, JSValue v) { (void)ctx; return js_is_callable(v); }
bool js_is_array(JSContext *ctx, JSValue v) { return js_is_array_v(ctx, v) > 0; }
bool js_strict_equals(JSContext *ctx, JSValue a, JSValue b) { return js_strict_eq(ctx, a, b); }

const char *js_typeof_name(JSContext *ctx, JSValue v) {
    JSValue t = js_typeof(ctx, v);
    for (int i = A_undefined; i <= A_bigint; i++) if (ctx->a[i] == t) {
        static const char *n[] = { "undefined", "null", "true", "false", "object", "function", "number", "string", "boolean", "symbol", "bigint" };
        return n[i - A_undefined];
    }
    return "object";
}

int js_new_class(JSContext *ctx, const js_class_t *cls) {
    js_class_rec *n = realloc(ctx->classes, sizeof *n * (size_t)(ctx->nclasses + 1));
    if (!n) return -1;
    ctx->classes = n;
    ctx->classes[ctx->nclasses].c = *cls;
    return ++ctx->nclasses;
}

JSValue js_new_host_object(JSContext *ctx, int class_id, JSValue proto, void *opaque) {
    JSObject *o = js_obj_alloc(ctx, CL_HOST, JS_IsObject(proto) ? OBJ(proto) : ctx->proto_object);
    if (!o) return JS_EXCEPTION;
    o->u.host.cls = class_id;
    o->u.host.opaque = opaque;
    return JS_MKOBJ(o);
}

void *js_host_opaque(JSValue v, int class_id) {
    if (!js_is_cls(v, CL_HOST) || OBJ(v)->u.host.cls != class_id) return NULL;
    return OBJ(v)->u.host.opaque;
}

int js_host_class(JSValue v) { return js_is_cls(v, CL_HOST) ? OBJ(v)->u.host.cls : 0; }

void js_set_host_opaque(JSValue v, void *opaque) { if (js_is_cls(v, CL_HOST)) OBJ(v)->u.host.opaque = opaque; }

JSValue js_to_string(JSContext *ctx, JSValue v) { return js_tostr(ctx, v); }

double js_to_number(JSContext *ctx, JSValue v, bool *exc) {
    bool e = false;
    double d = js_tonum(ctx, v, &e);
    if (exc) *exc = e;
    return d;
}

int32_t js_to_int32(JSContext *ctx, JSValue v) {
    int32_t r = 0;
    if (js_toint32_e(ctx, v, &r) < 0) return 0;
    return r;
}

JSValue js_new_string(JSContext *ctx, const char *s) { return js_str_utf8(ctx, s, strlen(s)); }
JSValue js_new_string_len(JSContext *ctx, const char *s, size_t len) { return js_str_utf8(ctx, s, len); }
JSValue js_new_string16(JSContext *ctx, const uint16_t *s, size_t len) { return js_str16(ctx, s, (uint32_t)len); }
JSValue js_new_string_latin1(JSContext *ctx, const char *s, size_t len) { return js_str8(ctx, s, (uint32_t)len); }

char *js_to_cstring(JSContext *ctx, JSValue v, size_t *len) {
    JSValue s = js_tostr(ctx, v);
    if (JS_IsException(s)) return NULL;
    return js_str_to_utf8(ctx, s, len);
}

const uint16_t *js_string_chars(JSContext *ctx, JSValue s, uint32_t *len) {
    if (!JS_IsString(s)) { *len = 0; return NULL; }
    JSString *f = js_flat(ctx, s);
    if (!f) { *len = 0; return NULL; }
    *len = f->len;
    return f->s;
}
