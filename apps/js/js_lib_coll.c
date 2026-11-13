/*
 * js_lib_coll.c - NovaJS built-ins: Map, Set, WeakMap, WeakSet, WeakRef,
 * FinalizationRegistry, Promise and async generators.
 *
 * Map / Set keep entries in insertion order (a deleted entry becomes a hole
 * until the table is compacted; live iterators are moved along). Weak
 * collections are ephemerons handled by the GC (js_gc.c).
 */
#include "jsi.h"

#define FN(name) static JSValue name(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic)
#define A(i) JS_ARG(i)

/* ==== the hash table ========================================================================= */
static uint32_t map_hash(JSContext *ctx, JSValue k) {
    if (JS_IsString(k)) { JSString *s = js_flat(ctx, k); return s ? js_str_hash(s) : 0; }
    uint64_t x = k;
    if (JS_IsNumber(k)) { double d = js_num(k); if (d == 0) x = 0; }
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return (uint32_t)x;
}

static bool map_eq(JSContext *ctx, JSValue a, JSValue b) {
    if (a == b) return true;
    if (JS_IsString(a) && JS_IsString(b)) return js_streq(ctx, a, b);
    return js_same_value(a, b, true);
}

static JSValue norm_key(JSValue k) {
    if (JS_IsNumber(k) && js_num(k) == 0) return js_mknum(0);
    return k;
}

static void map_rehash(JSContext *ctx, JSMap *m, uint32_t hs) {
    uint32_t *h = js_mallocz(ctx, sizeof(uint32_t) * hs);
    if (!h) return;
    js_mfree(ctx, m->h, sizeof(uint32_t) * m->hsize);
    m->h = h;
    m->hsize = hs;
    for (uint32_t i = 0; i < m->n; i++) {
        if (m->e[i].key == JS_HOLE) continue;
        uint32_t b = map_hash(ctx, m->e[i].key) & (hs - 1);
        m->e[i].hnext = h[b];
        h[b] = i + 1;
    }
}

static int64_t map_find(JSContext *ctx, JSMap *m, JSValue k) {
    if (!m->hsize) return -1;
    uint32_t b = map_hash(ctx, k) & (m->hsize - 1);
    for (uint32_t i = m->h[b]; i; i = m->e[i - 1].hnext)
        if (m->e[i - 1].key != JS_HOLE && map_eq(ctx, m->e[i - 1].key, k)) return (int64_t)i - 1;
    return -1;
}

static void map_compact(JSContext *ctx, JSMap *m) {
    uint32_t o = 0;
    uint32_t *newpos = malloc(sizeof(uint32_t) * (m->n + 1));
    if (!newpos) return;
    for (uint32_t i = 0; i < m->n; i++) {
        newpos[i] = o;
        if (m->e[i].key == JS_HOLE) continue;
        m->e[o++] = m->e[i];
    }
    newpos[m->n] = o;
    for (uint32_t k = 0; k < m->niters; k++) {
        JSIter *it = m->iters[k]->u.p;
        if (it) it->idx = it->idx <= m->n ? newpos[it->idx] : o;
    }
    free(newpos);
    m->n = o;
    map_rehash(ctx, m, m->hsize ? m->hsize : 8);
}

static int map_set(JSContext *ctx, JSMap *m, JSValue k, JSValue v) {
    k = norm_key(k);
    int64_t i = map_find(ctx, m, k);
    if (i >= 0) { m->e[i].val = v; return 0; }
    if (m->n >= m->cap) {
        if (m->n > m->live * 2 && m->n > 16) map_compact(ctx, m);
        if (m->n >= m->cap) {
            uint32_t nc = m->cap ? m->cap * 2 : 8;
            JSMapEnt *n = js_realloc(ctx, m->e, sizeof(JSMapEnt) * m->cap, sizeof(JSMapEnt) * nc);
            if (!n) return -1;
            m->e = n;
            m->cap = nc;
        }
    }
    if (m->hsize < (m->n + 1) * 2) map_rehash(ctx, m, m->hsize ? m->hsize * 2 : 16);
    if (!m->hsize) return -1;
    JSMapEnt *e = &m->e[m->n];
    e->key = k;
    e->val = v;
    uint32_t b = map_hash(ctx, k) & (m->hsize - 1);
    e->hnext = m->h[b];
    m->h[b] = ++m->n;
    m->live++;
    return 0;
}

static bool map_delete(JSContext *ctx, JSMap *m, JSValue k) {
    int64_t i = map_find(ctx, m, norm_key(k));
    if (i < 0) return false;
    /* unlink from its chain */
    uint32_t b = map_hash(ctx, m->e[i].key) & (m->hsize - 1);
    uint32_t *pp = &m->h[b];
    while (*pp && *pp != (uint32_t)i + 1) pp = &m->e[*pp - 1].hnext;
    if (*pp) *pp = m->e[i].hnext;
    m->e[i].key = JS_HOLE;
    m->e[i].val = JS_UNDEFINED;
    m->live--;
    return true;
}

static void map_clear(JSContext *ctx, JSMap *m) {
    for (uint32_t i = 0; i < m->n; i++) { m->e[i].key = JS_HOLE; m->e[i].val = JS_UNDEFINED; }
    m->live = 0;
    if (m->h) memset(m->h, 0, sizeof(uint32_t) * m->hsize);
    (void)ctx;
}

void js_map_mark(JSContext *ctx, JSMap *m) {
    for (uint32_t i = 0; i < m->n; i++) {
        if (m->e[i].key == JS_HOLE) continue;
        js_gc_mark_value(ctx, m->e[i].key);
        js_gc_mark_value(ctx, m->e[i].val);
    }
}

void js_map_sweep_weak(JSContext *ctx, JSMap *m) {
    bool changed = false;
    for (uint32_t i = 0; i < m->n; i++) {
        JSValue k = m->e[i].key;
        if (k == JS_HOLE || !JS_IsPtr(k)) continue;
        if (!((GCHdr *)JS_PTR(k))->mark) { m->e[i].key = JS_HOLE; m->e[i].val = JS_UNDEFINED; m->live--; changed = true; }
    }
    if (changed && m->hsize) {
        uint32_t o = 0;
        for (uint32_t i = 0; i < m->n; i++) if (m->e[i].key != JS_HOLE) m->e[o++] = m->e[i];
        m->n = o;
        /* (rehash without allocating: the GC is running) */
        memset(m->h, 0, sizeof(uint32_t) * m->hsize);
        for (uint32_t i = 0; i < m->n; i++) {
            uint32_t b = map_hash(ctx, m->e[i].key) & (m->hsize - 1);
            m->e[i].hnext = m->h[b];
            m->h[b] = i + 1;
        }
    }
}

void js_map_free(JSContext *ctx, JSMap *m) {
    js_mfree(ctx, m->e, sizeof(JSMapEnt) * m->cap);
    js_mfree(ctx, m->h, sizeof(uint32_t) * m->hsize);
    free(m->iters);
    js_mfree(ctx, m, sizeof(JSMap));
}

static void map_add_iter(JSMap *m, JSObject *it) {
    for (uint32_t i = 0; i < m->niters; i++) {
        JSIter *x = m->iters[i]->u.p;
        if (!x || x->done) { m->iters[i] = it; return; }
    }
    if (m->niters >= m->itcap) {
        uint32_t nc = m->itcap ? m->itcap * 2 : 4;
        JSObject **n = realloc(m->iters, sizeof(JSObject *) * nc);
        if (!n) return;
        m->iters = n;
        m->itcap = nc;
    }
    m->iters[m->niters++] = it;
}

/* drop iterators that are finished (the GC may have freed some: only called while marked) */
static void map_prune_iters(JSMap *m) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < m->niters; i++) {
        JSIter *x = m->iters[i]->u.p;
        if (x && !x->done) m->iters[o++] = m->iters[i];
    }
    m->niters = o;
}

/* ==== Map / Set ================================================================================= */
static JSMap *this_map(JSContext *ctx, JSValue v, int cls) {
    if (!js_is_cls(v, cls) || !OBJ(v)->u.p) {
        js_throw_type(ctx, "Method called on incompatible receiver");
        return NULL;
    }
    return OBJ(v)->u.p;
}

static JSValue coll_ctor(JSContext *ctx, int argc, JSValue *argv, int cls, JSObject *defproto, const char *name) {
    JSValue nt = js_new_target(ctx);
    if (JS_IsUndefined(nt)) return js_throw_type(ctx, "Constructor %s requires 'new'", name);
    JSValue p = js_getp(ctx, nt, ctx->a[A_prototype]);
    if (JS_IsException(p)) return p;
    JSObject *o = js_obj_alloc(ctx, cls, JS_IsObject(p) ? OBJ(p) : defproto);
    if (!o) return JS_EXCEPTION;
    JSMap *m = js_mallocz(ctx, sizeof(JSMap));
    if (!m) return JS_EXCEPTION;
    m->weak = cls == CL_WEAKMAP || cls == CL_WEAKSET;
    o->u.p = m;
    JSValue ov = JS_MKOBJ(o);
    JSValue it = JS_ARG(0);
    if (JS_IsNullish(it)) return ov;
    bool is_map = cls == CL_MAP || cls == CL_WEAKMAP;
    JSValue adder = js_getp(ctx, ov, ctx->a[is_map ? A_set : A_add]);
    if (JS_IsException(adder)) return adder;
    if (!js_is_callable(adder)) return js_throw_type(ctx, "'%s' is not a function", is_map ? "set" : "add");
    JSValue nx;
    JSValue iter = js_get_iterator(ctx, it, &nx, false);
    if (JS_IsException(iter)) return iter;
    for (;;) {
        bool done;
        JSValue v = js_iter_step(ctx, iter, nx, &done);
        if (JS_IsException(v)) return v;
        if (done) break;
        JSValue r;
        if (is_map) {
            if (!JS_IsObject(v)) { js_iter_close(ctx, iter, true); return js_throw_type(ctx, "Iterator value %s is not an entry object", "x"); }
            JSValue kv[2];
            kv[0] = js_geti(ctx, v, 0);
            if (JS_IsException(kv[0])) { js_iter_close(ctx, iter, true); return kv[0]; }
            kv[1] = js_geti(ctx, v, 1);
            if (JS_IsException(kv[1])) { js_iter_close(ctx, iter, true); return kv[1]; }
            r = js_callv(ctx, adder, ov, 2, kv);
        } else r = js_callv(ctx, adder, ov, 1, &v);
        if (JS_IsException(r)) { js_iter_close(ctx, iter, true); return r; }
    }
    return ov;
}

FN(map_ctor) { return coll_ctor(ctx, argc, argv, magic, magic == CL_MAP ? ctx->proto_map : magic == CL_SET ? ctx->proto_set : magic == CL_WEAKMAP ? ctx->proto_weakmap : ctx->proto_weakset,
                                magic == CL_MAP ? "Map" : magic == CL_SET ? "Set" : magic == CL_WEAKMAP ? "WeakMap" : "WeakSet"); }

static bool can_be_weak(JSValue k) {
    return JS_IsObject(k) || (JS_IsSymbol(k) && !(SYM(k)->h.flags & SYM_REGISTERED));
}

/* get 0, has 1, delete 2, set 3, add 4, clear 5 ; cls in the high bits */
FN(map_method) {
    int cls = magic >> 8, op = magic & 0xFF;
    JSMap *m = this_map(ctx, this_val, cls);
    if (!m) return JS_EXCEPTION;
    JSValue k = A(0);
    if (m->weak && !can_be_weak(k)) {
        if (op == 3 || op == 4) return js_throw_type(ctx, "Invalid value used %s", op == 3 ? "as weak map key" : "in weak set");
        return op == 0 ? JS_UNDEFINED : JS_FALSE;
    }
    switch (op) {
    case 0: { int64_t i = map_find(ctx, m, norm_key(k)); return i < 0 ? JS_UNDEFINED : m->e[i].val; }
    case 1: return JS_BOOL(map_find(ctx, m, norm_key(k)) >= 0);
    case 2: return JS_BOOL(map_delete(ctx, m, k));
    case 3: if (map_set(ctx, m, k, A(1)) < 0) return JS_EXCEPTION; return this_val;
    case 4: if (map_set(ctx, m, k, k) < 0) return JS_EXCEPTION; return this_val;
    default: map_clear(ctx, m); return JS_UNDEFINED;
    }
}

FN(map_size) {
    JSMap *m = this_map(ctx, this_val, magic);
    if (!m) return JS_EXCEPTION;
    return js_mknum(m->live);
}

FN(map_for_each) {
    JSMap *m = this_map(ctx, this_val, magic);
    if (!m) return JS_EXCEPTION;
    if (!js_is_callable(A(0))) return js_throw_type(ctx, "forEach: callback is not a function");
    JSValue it = js_new_iter(ctx, NULL, this_val, magic == CL_MAP ? ITT_MAP : ITT_SET, IT_ENTRIES);
    if (JS_IsException(it)) return it;
    JSIter *st = OBJ(it)->u.p;
    map_add_iter(m, OBJ(it));
    for (;;) {
        m = OBJ(this_val)->u.p;
        while (st->idx < m->n && m->e[st->idx].key == JS_HOLE) st->idx++;
        if (st->idx >= m->n) break;
        JSMapEnt *e = &m->e[st->idx++];
        JSValue args[3] = { magic == CL_MAP ? e->val : e->key, e->key, this_val };
        JSValue r = js_callv(ctx, A(0), A(1), 3, args);
        if (JS_IsException(r)) { st->done = true; map_prune_iters(OBJ(this_val)->u.p); return r; }
    }
    st->done = true;
    map_prune_iters(OBJ(this_val)->u.p);
    return JS_UNDEFINED;
}

/* keys 0, values 1, entries 2 */
FN(map_iter_make) {
    int cls = magic >> 8;
    JSMap *m = this_map(ctx, this_val, cls);
    if (!m) return JS_EXCEPTION;
    JSValue it = js_new_iter(ctx, cls == CL_MAP ? ctx->proto_map_iter : ctx->proto_set_iter, this_val, cls == CL_MAP ? ITT_MAP : ITT_SET, magic & 0xFF);
    if (JS_IsException(it)) return it;
    map_prune_iters(m);
    map_add_iter(m, OBJ(it));
    return it;
}

FN(map_iter_next) {
    if (!js_is_cls(this_val, CL_ITER)) return js_throw_type(ctx, "next method called on incompatible receiver");
    JSIter *it = OBJ(this_val)->u.p;
    if (it->type != ITT_MAP && it->type != ITT_SET) return js_throw_type(ctx, "next method called on incompatible receiver");
    if (it->done) return js_iter_result(ctx, JS_UNDEFINED, true);
    JSMap *m = OBJ(it->target)->u.p;
    while (it->idx < m->n && m->e[it->idx].key == JS_HOLE) it->idx++;
    if (it->idx >= m->n) { it->done = true; map_prune_iters(m); it->target = JS_UNDEFINED; return js_iter_result(ctx, JS_UNDEFINED, true); }
    JSMapEnt *e = &m->e[it->idx++];
    JSValue v;
    if (it->kind == IT_KEYS) v = e->key;
    else if (it->kind == IT_VALUES) v = e->val;
    else {
        JSValue pair[2] = { e->key, e->val };
        v = js_array_from_list(ctx, pair, 2);
        if (JS_IsException(v)) return v;
    }
    return js_iter_result(ctx, v, false);
}

FN(map_group_by) {
    JSValue out = js_construct_internal(ctx, js_getp(ctx, ctx->global, js_atomc(ctx, "Map")), 0, NULL, js_getp(ctx, ctx->global, js_atomc(ctx, "Map")));
    if (JS_IsException(out)) return out;
    if (!js_is_callable(A(1))) return js_throw_type(ctx, "callback is not a function");
    JSMap *m = OBJ(out)->u.p;
    JSValue nx;
    JSValue it = js_get_iterator(ctx, A(0), &nx, false);
    if (JS_IsException(it)) return it;
    for (uint32_t k = 0;; k++) {
        bool done;
        JSValue v = js_iter_step(ctx, it, nx, &done);
        if (JS_IsException(v)) return v;
        if (done) break;
        JSValue args[2] = { v, js_mknum(k) };
        JSValue key = js_callv(ctx, A(1), JS_UNDEFINED, 2, args);
        if (JS_IsException(key)) return key;
        int64_t i = map_find(ctx, m, norm_key(key));
        JSValue g;
        if (i < 0) { g = js_array_new(ctx, 0); if (JS_IsException(g) || map_set(ctx, m, key, g) < 0) return JS_EXCEPTION; }
        else g = m->e[i].val;
        if (js_array_push_v(ctx, g, v) < 0) return JS_EXCEPTION;
    }
    return out;
}

/* Set methods (ES2025): union 0, intersection 1, difference 2, symmetricDifference 3, isSubsetOf 4, isSupersetOf 5, isDisjointFrom 6 */
FN(set_algebra) {
    JSMap *m = this_map(ctx, this_val, CL_SET);
    if (!m) return JS_EXCEPTION;
    JSValue other = A(0);
    if (!JS_IsObject(other)) return js_throw_type(ctx, "Set method argument must be an object");
    JSValue has = js_getp(ctx, other, ctx->a[A_has]);
    if (JS_IsException(has)) return has;
    JSValue keysf = js_getp(ctx, other, js_atomc(ctx, "keys"));
    if (JS_IsException(keysf)) return keysf;
    if (!js_is_callable(has) || !js_is_callable(keysf)) return js_throw_type(ctx, "Set method argument must be set-like");
    /* the other set's keys */
    JSValue kit = js_callv(ctx, keysf, other, 0, NULL);
    if (JS_IsException(kit)) return kit;
    JSValue knext = js_getp(ctx, kit, ctx->a[A_next]);
    if (JS_IsException(knext)) return knext;
    JSValue okeys = js_array_new(ctx, 0);
    if (JS_IsException(okeys)) return okeys;
    for (;;) {
        bool done;
        JSValue v = js_iter_step(ctx, kit, knext, &done);
        if (JS_IsException(v)) return v;
        if (done) break;
        if (js_array_push_v(ctx, okeys, v) < 0) return JS_EXCEPTION;
    }
    JSValue setc = js_getp(ctx, ctx->global, js_atomc(ctx, "Set"));
    JSValue r = magic < 4 ? js_construct_internal(ctx, setc, 0, NULL, setc) : JS_UNDEFINED;
    if (JS_IsException(r)) return r;
    JSMap *rm = magic < 4 ? OBJ(r)->u.p : NULL;
    uint32_t on = OBJ(okeys)->u.a.len;
    m = OBJ(this_val)->u.p;
    for (uint32_t i = 0; i < m->n; i++) {
        JSValue k = m->e[i].key;
        if (k == JS_HOLE) continue;
        bool in_other = false;
        for (uint32_t j = 0; j < on; j++) if (map_eq(ctx, OBJ(okeys)->u.a.v[j], k)) { in_other = true; break; }
        switch (magic) {
        case 0: case 2: case 3: if (magic == 0 || !in_other) map_set(ctx, rm, k, k); break;
        case 1: if (in_other) map_set(ctx, rm, k, k); break;
        case 4: if (!in_other) return JS_FALSE; break;
        case 6: if (in_other) return JS_FALSE; break;
        default: break;
        }
    }
    for (uint32_t j = 0; j < on; j++) {
        JSValue k = OBJ(okeys)->u.a.v[j];
        bool in_this = map_find(ctx, OBJ(this_val)->u.p, norm_key(k)) >= 0;
        if (magic == 0 || (magic == 3 && !in_this)) map_set(ctx, rm, k, k);
        if (magic == 5 && !in_this) return JS_FALSE;
    }
    if (magic >= 4) return JS_TRUE;
    return r;
}

/* ==== WeakRef / FinalizationRegistry ===================================================================== */
FN(weakref_ctor) {
    JSValue nt = js_new_target(ctx);
    if (JS_IsUndefined(nt)) return js_throw_type(ctx, "Constructor WeakRef requires 'new'");
    if (!can_be_weak(A(0))) return js_throw_type(ctx, "WeakRef: target must be an object");
    JSObject *o = js_obj_alloc(ctx, CL_WEAKREF, ctx->proto_weakref);
    if (!o) return JS_EXCEPTION;
    o->u.prim = A(0);
    return JS_MKOBJ(o);
}

FN(weakref_deref) {
    if (!js_is_cls(this_val, CL_WEAKREF)) return js_throw_type(ctx, "WeakRef.prototype.deref called on incompatible receiver");
    return OBJ(this_val)->u.prim;
}

FN(finreg_ctor) {
    JSValue nt = js_new_target(ctx);
    if (JS_IsUndefined(nt)) return js_throw_type(ctx, "Constructor FinalizationRegistry requires 'new'");
    if (!js_is_callable(A(0))) return js_throw_type(ctx, "FinalizationRegistry: cleanup must be callable");
    return js_obj_new(ctx, CL_OBJECT, ctx->proto_finreg);
}

FN(finreg_noop) { return magic ? JS_FALSE : JS_UNDEFINED; }

/* ==== Promise ================================================================================================ */
typedef struct Reaction { JSValue ok, fail, resolve, reject; } Reaction;
typedef struct JSPromiseData {
    int state;                 /* 0 pending, 1 fulfilled, 2 rejected */
    JSValue result;
    Reaction *r;
    int nr, rcap;
    bool handled;
} JSPromiseData;

void js_promise_mark(JSContext *ctx, void *p) {
    JSPromiseData *d = p;
    js_gc_mark_value(ctx, d->result);
    for (int i = 0; i < d->nr; i++) {
        js_gc_mark_value(ctx, d->r[i].ok);
        js_gc_mark_value(ctx, d->r[i].fail);
        js_gc_mark_value(ctx, d->r[i].resolve);
        js_gc_mark_value(ctx, d->r[i].reject);
    }
}

void js_promise_free(JSContext *ctx, void *p) {
    JSPromiseData *d = p;
    js_mfree(ctx, d->r, sizeof(Reaction) * (size_t)d->rcap);
    js_mfree(ctx, d, sizeof *d);
}

static JSPromiseData *pdata(JSValue p) { return js_is_cls(p, CL_PROMISE) ? OBJ(p)->u.p : NULL; }

static void track_rejection(JSContext *ctx, JSValue p, bool add) {
    JSValue list = ctx->unhandled;
    if (!js_is_cls(list, CL_ARRAY)) {
        if (!add) return;
        list = js_array_new(ctx, 0);
        if (JS_IsException(list)) { js_take_exception(ctx); return; }
        ctx->unhandled = list;
    }
    JSObject *a = OBJ(list);
    if (add) { js_array_push_v(ctx, list, p); return; }
    for (uint32_t i = 0; i < a->u.a.len; i++) if (a->u.a.v[i] == p) a->u.a.v[i] = JS_UNDEFINED;
}

void js_promise_report_unhandled(JSContext *ctx) {
    JSValue list = ctx->unhandled;
    if (!js_is_cls(list, CL_ARRAY)) return;
    ctx->unhandled = JS_UNDEFINED;
    JSObject *a = OBJ(list);
    for (uint32_t i = 0; i < a->u.a.len; i++) {
        JSPromiseData *d = pdata(a->u.a.v[i]);
        if (!d || d->handled || d->state != 2) continue;
        d->handled = true;
        char *t = js_error_text(ctx, d->result);
        if (ctx->host.log) {
            char buf[600];
            snprintf(buf, sizeof buf, "Uncaught (in promise) %s", t ? t : "error");
            ctx->host.log(ctx->host.ud, JS_LOG_ERROR, buf);
        }
        free(t);
    }
}

static void settle_promise(JSContext *ctx, JSValue p, int state, JSValue v) {
    JSPromiseData *d = pdata(p);
    if (!d || d->state) return;
    d->state = state;
    d->result = v;
    for (int i = 0; i < d->nr; i++) {
        Reaction *r = &d->r[i];
        js_enqueue_reaction(ctx, state == 1 ? r->ok : r->fail, v, r->resolve, r->reject, state == 2);
    }
    js_mfree(ctx, d->r, sizeof(Reaction) * (size_t)d->rcap);
    d->r = NULL;
    d->nr = d->rcap = 0;
    if (state == 2 && !d->handled) track_rejection(ctx, p, true);
}

static JSValue make_resolvers(JSContext *ctx, JSValue p, JSValue *res, JSValue *rej);

/* resolve p with x */
static void resolve_promise(JSContext *ctx, JSValue p, JSValue x) {
    if (x == p) {
        JSValue e = js_new_error(ctx, JS_TYPE_ERROR, "Chaining cycle detected for promise");
        settle_promise(ctx, p, 2, JS_IsException(e) ? js_take_exception(ctx) : e);
        return;
    }
    if (!JS_IsObject(x)) { settle_promise(ctx, p, 1, x); return; }
    JSValue then = js_getp(ctx, x, ctx->a[A_then]);
    if (JS_IsException(then)) { settle_promise(ctx, p, 2, js_take_exception(ctx)); return; }
    if (!js_is_callable(then)) { settle_promise(ctx, p, 1, x); return; }
    JSValue res, rej;
    if (JS_IsException(make_resolvers(ctx, p, &res, &rej))) { settle_promise(ctx, p, 2, js_take_exception(ctx)); return; }
    js_enqueue_thenable(ctx, then, x, res, rej);
}

/* data: [promise, alreadyResolved] shared by the pair */
FN(resolver_fn) {
    JSValue d = js_func_data(ctx);
    JSObject *a = OBJ(d);
    if (a->u.a.v[1] == JS_TRUE) return JS_UNDEFINED;
    a->u.a.v[1] = JS_TRUE;
    JSValue p = a->u.a.v[0];
    if (magic) settle_promise(ctx, p, 2, A(0));
    else resolve_promise(ctx, p, A(0));
    return JS_UNDEFINED;
}

static JSValue make_resolvers(JSContext *ctx, JSValue p, JSValue *res, JSValue *rej) {
    JSValue pair[2] = { p, JS_FALSE };
    JSValue d = js_array_from_list(ctx, pair, 2);
    if (JS_IsException(d)) return d;
    *res = js_mk_native(ctx, resolver_fn, ctx->empty_str, 1, 0);
    if (JS_IsException(*res)) return *res;
    OBJ(*res)->u.n.data = d;
    *rej = js_mk_native(ctx, resolver_fn, ctx->empty_str, 1, 1);
    if (JS_IsException(*rej)) return *rej;
    OBJ(*rej)->u.n.data = d;
    return JS_UNDEFINED;
}

static JSValue new_promise_obj(JSContext *ctx, JSObject *proto) {
    JSObject *o = js_obj_alloc(ctx, CL_PROMISE, proto ? proto : ctx->proto_promise);
    if (!o) return JS_EXCEPTION;
    JSPromiseData *d = js_mallocz(ctx, sizeof *d);
    if (!d) return JS_EXCEPTION;
    d->result = JS_UNDEFINED;
    o->u.p = d;
    return JS_MKOBJ(o);
}

JSValue js_promise_new(JSContext *ctx, JSValue *resolve, JSValue *reject) {
    JSValue p = new_promise_obj(ctx, NULL);
    if (JS_IsException(p)) return p;
    if (JS_IsException(make_resolvers(ctx, p, resolve, reject))) return JS_EXCEPTION;
    return p;
}

JSValue js_new_promise(JSContext *ctx, JSValue *resolve, JSValue *reject) {
    JS_GC_ENTER(ctx);
    JSValue r = js_promise_new(ctx, resolve, reject);
    JS_GC_LEAVE(ctx);
    return r;
}

/* NewPromiseCapability(C): {promise, resolve, reject} */
FN(cap_executor) {
    JSValue d = js_func_data(ctx);
    JSObject *a = OBJ(d);
    if (!JS_IsUndefined(a->u.a.v[1]) || !JS_IsUndefined(a->u.a.v[2])) return js_throw_type(ctx, "Promise executor has already been invoked");
    a->u.a.v[1] = A(0);
    a->u.a.v[2] = A(1);
    return JS_UNDEFINED;
}

static JSValue new_capability(JSContext *ctx, JSValue c, JSValue *res, JSValue *rej) {
    if (c == JS_MKOBJ(ctx->ctor_promise) || JS_IsUndefined(c)) return js_promise_new(ctx, res, rej);
    if (!js_is_ctor(c)) return js_throw_type(ctx, "Promise resolve or reject function is not callable");
    JSValue slots[3] = { JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED };
    JSValue d = js_array_from_list(ctx, slots, 3);
    if (JS_IsException(d)) return d;
    JSValue ex = js_mk_native(ctx, cap_executor, ctx->empty_str, 2, 0);
    if (JS_IsException(ex)) return ex;
    OBJ(ex)->u.n.data = d;
    JSValue p = js_construct_internal(ctx, c, 1, &ex, c);
    if (JS_IsException(p)) return p;
    *res = OBJ(d)->u.a.v[1];
    *rej = OBJ(d)->u.a.v[2];
    if (!js_is_callable(*res) || !js_is_callable(*rej)) return js_throw_type(ctx, "Promise resolve or reject function is not callable");
    return p;
}

static void perform_then(JSContext *ctx, JSValue p, JSValue ok, JSValue fail, JSValue res, JSValue rej) {
    JSPromiseData *d = pdata(p);
    if (!js_is_callable(ok)) ok = JS_UNDEFINED;
    if (!js_is_callable(fail)) fail = JS_UNDEFINED;
    if (d->state == 0) {
        if (d->nr >= d->rcap) {
            int nc = d->rcap ? d->rcap * 2 : 2;
            Reaction *n = js_realloc(ctx, d->r, sizeof(Reaction) * (size_t)d->rcap, sizeof(Reaction) * (size_t)nc);
            if (!n) return;
            d = pdata(p);
            d->r = n;
            d->rcap = nc;
        }
        d->r[d->nr].ok = ok;
        d->r[d->nr].fail = fail;
        d->r[d->nr].resolve = res;
        d->r[d->nr].reject = rej;
        d->nr++;
    } else {
        if (d->state == 2 && !d->handled) track_rejection(ctx, p, false);
        js_enqueue_reaction(ctx, d->state == 1 ? ok : fail, d->result, res, rej, d->state == 2);
    }
    d->handled = true;
}

void js_promise_then_native(JSContext *ctx, JSValue p, JSNativeFn ok, JSNativeFn fail, JSValue data) {
    if (!pdata(p)) return;
    if (!ok && !fail) {   /* await: resume data (an async function / generator) */
        perform_then(ctx, p, ctx->async_ok, ctx->async_fail, data, JS_UNDEFINED);
        return;
    }
    JSValue o = ok ? js_mk_native(ctx, ok, ctx->empty_str, 1, 0) : JS_UNDEFINED;
    JSValue f = fail ? js_mk_native(ctx, fail, ctx->empty_str, 1, 0) : JS_UNDEFINED;
    if (JS_IsException(o) || JS_IsException(f)) { js_take_exception(ctx); return; }
    if (ok) OBJ(o)->u.n.data = data;
    if (fail) OBJ(f)->u.n.data = data;
    perform_then(ctx, p, o, f, JS_UNDEFINED, JS_UNDEFINED);
}

JSValue js_promise_resolve_value(JSContext *ctx, JSValue v) {
    if (js_is_cls(v, CL_PROMISE)) {
        JSValue c = js_getp(ctx, v, ctx->a[A_constructor]);
        if (JS_IsException(c)) return c;
        if (c == JS_MKOBJ(ctx->ctor_promise)) return v;
    }
    JSValue res, rej;
    JSValue p = js_promise_new(ctx, &res, &rej);
    if (JS_IsException(p)) return p;
    resolve_promise(ctx, p, v);
    OBJ(res)->u.n.data = JS_UNDEFINED;
    return p;
}

FN(promise_ctor) {
    JSValue nt = js_new_target(ctx);
    if (JS_IsUndefined(nt)) return js_throw_type(ctx, "Promise constructor cannot be invoked without 'new'");
    JSValue ex = A(0);
    if (!js_is_callable(ex)) return js_throw_type(ctx, "Promise resolver is not a function");
    JSValue pr = js_getp(ctx, nt, ctx->a[A_prototype]);
    if (JS_IsException(pr)) return pr;
    JSValue p = new_promise_obj(ctx, JS_IsObject(pr) ? OBJ(pr) : NULL);
    if (JS_IsException(p)) return p;
    JSValue res, rej;
    if (JS_IsException(make_resolvers(ctx, p, &res, &rej))) return JS_EXCEPTION;
    JSValue args[2] = { res, rej };
    JSValue r = js_callv(ctx, ex, JS_UNDEFINED, 2, args);
    if (JS_IsException(r)) {
        JSValue e = js_take_exception(ctx);
        if (ctx->killed) return js_throw(ctx, e);
        js_callv(ctx, rej, JS_UNDEFINED, 1, &e);
    }
    return p;
}

FN(promisep_then) {
    if (!pdata(this_val)) return js_throw_type(ctx, "Method Promise.prototype.then called on incompatible receiver");
    JSValue c = js_species_ctor(ctx, this_val, ctx->ctor_promise);
    if (JS_IsException(c)) return c;
    JSValue res, rej;
    JSValue p = new_capability(ctx, c, &res, &rej);
    if (JS_IsException(p)) return p;
    perform_then(ctx, this_val, A(0), A(1), res, rej);
    return p;
}

FN(promisep_catch) {
    JSValue args[2] = { JS_UNDEFINED, A(0) };
    return js_invoke(ctx, this_val, ctx->a[A_then], 2, args);
}

/* finally: then(v => C.resolve(onFinally()).then(() => v), e => C.resolve(onFinally()).then(() => { throw e })) */
FN(finally_value) {
    /* data: [value, isThrow] */
    JSObject *d = OBJ(js_func_data(ctx));
    if (d->u.a.v[1] == JS_TRUE) return js_throw(ctx, d->u.a.v[0]);
    return d->u.a.v[0];
}

FN(finally_step) {
    /* data: [onFinally, C, isReject] */
    JSObject *d = OBJ(js_func_data(ctx));
    JSValue r = js_callv(ctx, d->u.a.v[0], JS_UNDEFINED, 0, NULL);
    if (JS_IsException(r)) return r;
    JSValue c = d->u.a.v[1];
    JSValue p;
    if (c == JS_MKOBJ(ctx->ctor_promise)) p = js_promise_resolve_value(ctx, r);
    else p = js_invoke(ctx, c, js_atomc(ctx, "resolve"), 1, &r);
    if (JS_IsException(p)) return p;
    JSValue vd[2] = { A(0), d->u.a.v[2] };
    JSValue data = js_array_from_list(ctx, vd, 2);
    if (JS_IsException(data)) return data;
    JSValue f = js_mk_native(ctx, finally_value, ctx->empty_str, 0, 0);
    if (JS_IsException(f)) return f;
    OBJ(f)->u.n.data = data;
    return js_invoke(ctx, p, ctx->a[A_then], 1, &f);
}

FN(promisep_finally) {
    if (!JS_IsObject(this_val)) return js_throw_type(ctx, "Promise.prototype.finally called on non-object");
    JSValue c = js_species_ctor(ctx, this_val, ctx->ctor_promise);
    if (JS_IsException(c)) return c;
    JSValue onf = A(0);
    if (!js_is_callable(onf)) { JSValue args[2] = { onf, onf }; return js_invoke(ctx, this_val, ctx->a[A_then], 2, args); }
    JSValue fns[2];
    for (int k = 0; k < 2; k++) {
        JSValue dv[3] = { onf, c, JS_BOOL(k == 1) };
        JSValue data = js_array_from_list(ctx, dv, 3);
        if (JS_IsException(data)) return data;
        fns[k] = js_mk_native(ctx, finally_step, ctx->empty_str, 1, 0);
        if (JS_IsException(fns[k])) return fns[k];
        OBJ(fns[k])->u.n.data = data;
    }
    return js_invoke(ctx, this_val, ctx->a[A_then], 2, fns);
}

FN(promise_resolve) {
    if (!JS_IsObject(this_val)) return js_throw_type(ctx, "PromiseResolve called on non-object");
    JSValue x = A(0);
    if (js_is_cls(x, CL_PROMISE)) {
        JSValue c = js_getp(ctx, x, ctx->a[A_constructor]);
        if (JS_IsException(c)) return c;
        if (c == this_val) return x;
    }
    JSValue res, rej;
    JSValue p = new_capability(ctx, this_val, &res, &rej);
    if (JS_IsException(p)) return p;
    JSValue r = js_callv(ctx, magic ? rej : res, JS_UNDEFINED, 1, &x);
    if (JS_IsException(r)) return r;
    return p;
}

FN(promise_with_resolvers) {
    JSValue res, rej;
    JSValue p = new_capability(ctx, this_val, &res, &rej);
    if (JS_IsException(p)) return p;
    JSValue o = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
    if (JS_IsException(o)) return o;
    js_add_prop(ctx, OBJ(o), js_atomc(ctx, "promise"), p, P_W | P_E | P_C);
    js_add_prop(ctx, OBJ(o), js_atomc(ctx, "resolve"), res, P_W | P_E | P_C);
    js_add_prop(ctx, OBJ(o), js_atomc(ctx, "reject"), rej, P_W | P_E | P_C);
    return o;
}

/* combinator element functions; state: [values, remaining, resolve, reject, errors] ; data: [state, index, called] */
enum { CB_ALL = 0, CB_ALLSETTLED, CB_ANY, CB_RACE };

FN(combo_elem) {
    JSObject *d = OBJ(js_func_data(ctx));
    if (d->u.a.v[2] == JS_TRUE) return JS_UNDEFINED;
    d->u.a.v[2] = JS_TRUE;
    JSObject *st = OBJ(d->u.a.v[0]);
    uint32_t idx = (uint32_t)js_num(d->u.a.v[1]);
    JSValue v = A(0);
    /* magic: 0 all-resolve, 1 settled-fulfilled, 2 settled-rejected, 3 any-reject */
    if (magic == 1 || magic == 2) {
        JSValue o = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
        if (JS_IsException(o)) return o;
        js_add_prop(ctx, OBJ(o), ctx->a[A_status], ctx->a[magic == 1 ? A_fulfilled : A_rejected], P_W | P_E | P_C);
        js_add_prop(ctx, OBJ(o), ctx->a[magic == 1 ? A_value : A_reason], v, P_W | P_E | P_C);
        v = o;
    }
    JSValue arr = st->u.a.v[magic == 3 ? 4 : 0];
    if (js_seti(ctx, arr, idx, v) < 0) return JS_EXCEPTION;
    double rem = js_num(st->u.a.v[1]) - 1;
    st->u.a.v[1] = js_mknum(rem);
    if (rem == 0) {
        if (magic == 3) {
            JSValue err = js_new_error(ctx, JS_AGGREGATE_ERROR, "All promises were rejected");
            if (JS_IsException(err)) return err;
            js_add_prop(ctx, OBJ(err), ctx->a[A_errors], arr, P_W | P_C);
            return js_callv(ctx, st->u.a.v[3], JS_UNDEFINED, 1, &err);
        }
        return js_callv(ctx, st->u.a.v[2], JS_UNDEFINED, 1, &arr);
    }
    return JS_UNDEFINED;
}

static JSValue make_elem(JSContext *ctx, JSValue state, uint32_t idx, int kind) {
    JSValue dv[3] = { state, js_mknum(idx), JS_FALSE };
    JSValue data = js_array_from_list(ctx, dv, 3);
    if (JS_IsException(data)) return data;
    JSValue f = js_mk_native(ctx, combo_elem, ctx->empty_str, 1, kind);
    if (!JS_IsException(f)) OBJ(f)->u.n.data = data;
    return f;
}

FN(promise_combinator) {
    JSValue c = this_val;
    JSValue res, rej;
    JSValue p = new_capability(ctx, c, &res, &rej);
    if (JS_IsException(p)) return p;
    JSValue presolve = js_getp(ctx, c, js_atomc(ctx, "resolve"));
    if (JS_IsException(presolve) || !js_is_callable(presolve)) {
        if (!JS_IsException(presolve)) js_throw_type(ctx, "Promise resolve is not a function");
        goto reject;
    }
    JSValue values = js_array_new(ctx, 0);
    if (JS_IsException(values)) goto reject;
    JSValue errors = js_array_new(ctx, 0);
    if (JS_IsException(errors)) goto reject;
    JSValue sv[5] = { values, js_mknum(1), res, rej, errors };
    JSValue state = js_array_from_list(ctx, sv, 5);
    if (JS_IsException(state)) goto reject;
    JSValue nx;
    JSValue it = js_get_iterator(ctx, A(0), &nx, false);
    if (JS_IsException(it)) goto reject;
    for (uint32_t idx = 0;; idx++) {
        bool done;
        JSValue v = js_iter_step(ctx, it, nx, &done);
        if (JS_IsException(v)) goto reject;
        if (done) break;
        if (magic != CB_RACE) {
            js_array_push_v(ctx, magic == CB_ANY ? errors : values, JS_UNDEFINED);
            OBJ(state)->u.a.v[1] = js_mknum(js_num(OBJ(state)->u.a.v[1]) + 1);
        }
        JSValue np = js_callv(ctx, presolve, c, 1, &v);
        if (JS_IsException(np)) { js_iter_close(ctx, it, true); goto reject; }
        JSValue ok, fail;
        switch (magic) {
        case CB_ALL: ok = make_elem(ctx, state, idx, 0); fail = rej; break;
        case CB_ALLSETTLED: ok = make_elem(ctx, state, idx, 1); fail = make_elem(ctx, state, idx, 2); break;
        case CB_ANY: ok = res; fail = make_elem(ctx, state, idx, 3); break;
        default: ok = res; fail = rej; break;
        }
        if (JS_IsException(ok) || JS_IsException(fail)) goto reject;
        JSValue args[2] = { ok, fail };
        JSValue r = js_invoke(ctx, np, ctx->a[A_then], 2, args);
        if (JS_IsException(r)) { js_iter_close(ctx, it, true); goto reject; }
    }
    if (magic != CB_RACE) {
        double rem = js_num(OBJ(state)->u.a.v[1]) - 1;
        OBJ(state)->u.a.v[1] = js_mknum(rem);
        if (rem == 0) {
            if (magic == CB_ANY) {
                JSValue err = js_new_error(ctx, JS_AGGREGATE_ERROR, "All promises were rejected");
                if (JS_IsException(err)) goto reject;
                js_add_prop(ctx, OBJ(err), ctx->a[A_errors], errors, P_W | P_C);
                js_callv(ctx, rej, JS_UNDEFINED, 1, &err);
            } else js_callv(ctx, res, JS_UNDEFINED, 1, &values);
        }
    }
    return p;
reject: {
        if (ctx->killed) return JS_EXCEPTION;
        JSValue e = js_take_exception(ctx);
        JSValue r = js_callv(ctx, rej, JS_UNDEFINED, 1, &e);
        if (JS_IsException(r)) return r;
        return p;
    }
}

FN(promise_try) {
    JSValue res, rej;
    JSValue p = new_capability(ctx, this_val, &res, &rej);
    if (JS_IsException(p)) return p;
    if (!js_is_callable(A(0))) return js_throw_type(ctx, "Promise.try: callback is not a function");
    JSValue r = js_callv(ctx, A(0), JS_UNDEFINED, argc > 1 ? argc - 1 : 0, argv + 1);
    if (JS_IsException(r)) { JSValue e = js_take_exception(ctx); js_callv(ctx, rej, JS_UNDEFINED, 1, &e); }
    else js_callv(ctx, res, JS_UNDEFINED, 1, &r);
    return p;
}

FN(promise_species) { return this_val; }

FN(async_reaction) { return js_throw_type(ctx, "internal async reaction called directly"); }

/* ==== async generators =================================================================================== */
/* request: [mode, value, resolve, reject] in g->queue */
static void ag_drain(JSContext *ctx, JSObject *go);

static void ag_settle_front(JSContext *ctx, JSObject *go, JSValue v, bool done, bool threw) {
    JSGen *g = go->u.p;
    JSObject *q = OBJ(g->queue);
    if (!q->u.a.len) return;
    JSValue req = q->u.a.v[0];
    memmove(q->u.a.v, q->u.a.v + 1, sizeof(JSValue) * (q->u.a.len - 1));
    q->u.a.v[--q->u.a.len] = JS_HOLE;
    JSObject *r = OBJ(req);
    if (threw) { JSValue x = js_callv(ctx, r->u.a.v[3], JS_UNDEFINED, 1, &v); if (JS_IsException(x)) js_take_exception(ctx); return; }
    JSValue res = js_iter_result(ctx, v, done);
    if (JS_IsException(res)) { js_take_exception(ctx); return; }
    JSValue x = js_callv(ctx, r->u.a.v[2], JS_UNDEFINED, 1, &res);
    if (JS_IsException(x)) js_take_exception(ctx);
}

void js_async_gen_step_done(JSContext *ctx, JSObject *go, JSValue r, bool threw, bool done, bool awaiting) {
    JSGen *g = go->u.p;
    if (awaiting) return;
    if (threw) {
        g->state = GS_DONE;
        JSValue e = js_take_exception(ctx);
        ag_settle_front(ctx, go, e, true, true);
    } else if (done) {
        g->state = GS_DONE;
        ag_settle_front(ctx, go, r, true, false);
    } else {
        g->state = GS_SUSPENDED_YIELD;
        ag_settle_front(ctx, go, r, false, false);
    }
    ag_drain(ctx, go);
}

static void ag_drain(JSContext *ctx, JSObject *go) {
    JSGen *g = go->u.p;
    for (;;) {
        if (g->state == GS_RUNNING || g->state == GS_AWAIT) return;
        JSObject *q = OBJ(g->queue);
        if (!q->u.a.len) return;
        JSObject *req = OBJ(q->u.a.v[0]);
        int mode = (int)js_num(req->u.a.v[0]);
        JSValue v = req->u.a.v[1];
        if (g->state == GS_START && mode != GR_NEXT) g->state = GS_DONE;
        if (g->state == GS_DONE) {
            if (mode == GR_THROW) ag_settle_front(ctx, go, v, true, true);
            else ag_settle_front(ctx, go, mode == GR_RETURN ? v : JS_UNDEFINED, true, false);
            continue;
        }
        bool done, awaiting;
        JSValue r = js_async_gen_run(ctx, go, v, mode, &done, &awaiting);
        if (awaiting) return;
        js_async_gen_step_done(ctx, go, r, JS_IsException(r), done, false);
        return;
    }
}

FN(agen_method) {
    JSValue res, rej;
    JSValue p = js_promise_new(ctx, &res, &rej);
    if (JS_IsException(p)) return p;
    if (!js_is_cls(this_val, CL_ASYNC_GEN)) {
        JSValue e = js_new_error(ctx, JS_TYPE_ERROR, "AsyncGenerator method called on incompatible receiver");
        js_callv(ctx, rej, JS_UNDEFINED, 1, &e);
        return p;
    }
    JSObject *go = OBJ(this_val);
    JSGen *g = go->u.p;
    JSValue rv[4] = { js_mknum(magic), A(0), res, rej };
    JSValue req = js_array_from_list(ctx, rv, 4);
    if (JS_IsException(req) || js_array_push_v(ctx, g->queue, req) < 0) return JS_EXCEPTION;
    ag_drain(ctx, go);
    return p;
}

FN(async_iter_self) { return this_val; }

/* ==== setup ================================================================================================== */
void js_init_lib_coll(JSContext *ctx) {
    static const char *names[4] = { "Map", "Set", "WeakMap", "WeakSet" };
    int clss[4] = { CL_MAP, CL_SET, CL_WEAKMAP, CL_WEAKSET };
    JSObject **protos[4] = { &ctx->proto_map, &ctx->proto_set, &ctx->proto_weakmap, &ctx->proto_weakset };
    for (int i = 0; i < 4; i++) {
        *protos[i] = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
        JSObject *c = js_new_ctor(ctx, names[i], map_ctor, 0, *protos[i], clss[i]);
        JSValue p = JS_MKOBJ(*protos[i]);
        int cls = clss[i] << 8;
        bool is_map = clss[i] == CL_MAP || clss[i] == CL_WEAKMAP;
        bool weak = clss[i] == CL_WEAKMAP || clss[i] == CL_WEAKSET;
        if (is_map) { js_def_native(ctx, p, "get", map_method, 1, cls | 0); js_def_native(ctx, p, "set", map_method, 2, cls | 3); }
        else js_def_native(ctx, p, "add", map_method, 1, cls | 4);
        js_def_native(ctx, p, "has", map_method, 1, cls | 1);
        js_def_native(ctx, p, "delete", map_method, 1, cls | 2);
        js_add_prop(ctx, *protos[i], ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, names[i]), P_C);
        if (!weak) {
            js_def_native(ctx, p, "clear", map_method, 0, cls | 5);
            js_def_accessor(ctx, p, ctx->a[A_size], map_size, NULL, clss[i], P_C);
            js_def_native(ctx, p, "forEach", map_for_each, 1, clss[i]);
            JSValue ent = js_def_native(ctx, p, "entries", map_iter_make, 0, cls | IT_ENTRIES);
            JSValue vals = js_def_native(ctx, p, "values", map_iter_make, 0, cls | IT_VALUES);
            if (is_map) {
                js_def_native(ctx, p, "keys", map_iter_make, 0, cls | IT_KEYS);
                js_add_prop(ctx, *protos[i], ctx->wk[WK_ITERATOR], ent, P_W | P_C);
            } else {
                js_add_prop(ctx, *protos[i], js_atomc(ctx, "keys"), vals, P_W | P_C);
                js_add_prop(ctx, *protos[i], ctx->wk[WK_ITERATOR], vals, P_W | P_C);
                static const char *sa[7] = { "union", "intersection", "difference", "symmetricDifference", "isSubsetOf", "isSupersetOf", "isDisjointFrom" };
                for (int k = 0; k < 7; k++) js_def_native(ctx, p, sa[k], set_algebra, 1, k);
            }
            js_def_accessor(ctx, JS_MKOBJ(c), ctx->wk[WK_SPECIES], promise_species, NULL, 0, P_C);
        }
        if (clss[i] == CL_MAP) js_def_native(ctx, JS_MKOBJ(c), "groupBy", map_group_by, 2, 0);
    }
    ctx->proto_map_iter = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_iterator);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_map_iter), "next", map_iter_next, 0, 0);
    js_add_prop(ctx, ctx->proto_map_iter, ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, "Map Iterator"), P_C);
    ctx->proto_set_iter = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_iterator);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_set_iter), "next", map_iter_next, 0, 0);
    js_add_prop(ctx, ctx->proto_set_iter, ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, "Set Iterator"), P_C);
    /* WeakRef, FinalizationRegistry */
    ctx->proto_weakref = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
    js_new_ctor(ctx, "WeakRef", weakref_ctor, 1, ctx->proto_weakref, 0);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_weakref), "deref", weakref_deref, 0, 0);
    js_add_prop(ctx, ctx->proto_weakref, ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, "WeakRef"), P_C);
    ctx->proto_finreg = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
    js_new_ctor(ctx, "FinalizationRegistry", finreg_ctor, 1, ctx->proto_finreg, 0);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_finreg), "register", finreg_noop, 2, 0);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_finreg), "unregister", finreg_noop, 1, 1);
    js_add_prop(ctx, ctx->proto_finreg, ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, "FinalizationRegistry"), P_C);
    /* Promise */
    ctx->proto_promise = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
    JSObject *pc = js_new_ctor(ctx, "Promise", promise_ctor, 1, ctx->proto_promise, 0);
    ctx->ctor_promise = pc;
    JSValue pcv = JS_MKOBJ(pc);
    js_def_native(ctx, pcv, "resolve", promise_resolve, 1, 0);
    js_def_native(ctx, pcv, "reject", promise_resolve, 1, 1);
    js_def_native(ctx, pcv, "all", promise_combinator, 1, CB_ALL);
    js_def_native(ctx, pcv, "allSettled", promise_combinator, 1, CB_ALLSETTLED);
    js_def_native(ctx, pcv, "any", promise_combinator, 1, CB_ANY);
    js_def_native(ctx, pcv, "race", promise_combinator, 1, CB_RACE);
    js_def_native(ctx, pcv, "withResolvers", promise_with_resolvers, 0, 0);
    js_def_native(ctx, pcv, "try", promise_try, 1, 0);
    js_def_accessor(ctx, pcv, ctx->wk[WK_SPECIES], promise_species, NULL, 0, P_C);
    JSValue pp = JS_MKOBJ(ctx->proto_promise);
    js_def_native(ctx, pp, "then", promisep_then, 2, 0);
    js_def_native(ctx, pp, "catch", promisep_catch, 1, 0);
    js_def_native(ctx, pp, "finally", promisep_finally, 1, 0);
    js_add_prop(ctx, ctx->proto_promise, ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, "Promise"), P_C);
    ctx->async_ok = js_mk_native(ctx, async_reaction, ctx->empty_str, 1, 0);
    ctx->async_fail = js_mk_native(ctx, async_reaction, ctx->empty_str, 1, 1);
    /* async iteration */
    ctx->proto_async_iter = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_async_iter), "@asyncIterator", async_iter_self, 0, 0);
    ctx->proto_async_gen = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_async_iter);
    JSValue agp = JS_MKOBJ(ctx->proto_async_gen);
    js_def_native(ctx, agp, "next", agen_method, 1, GR_NEXT);
    js_def_native(ctx, agp, "return", agen_method, 1, GR_RETURN);
    js_def_native(ctx, agp, "throw", agen_method, 1, GR_THROW);
    js_add_prop(ctx, ctx->proto_async_gen, ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, "AsyncGenerator"), P_C);
    js_add_prop(ctx, ctx->proto_async_gen_func, ctx->a[A_prototype], agp, P_C);
    js_add_prop(ctx, ctx->proto_async_gen, ctx->a[A_constructor], JS_MKOBJ(ctx->proto_async_gen_func), P_C);
}
