/*
 * js_vm.c - NovaJS interpreter: frames on a VM stack, the bytecode loop,
 * calls and constructors, generators and async functions (their frames are
 * copied out on yield / await and back in on resume), the task / job driver
 * with time-sliced suspension, globals and context setup.
 *
 * JS -> JS calls never recurse on the C stack: run() switches frames. Native
 * code calling JS (js_call_internal) starts a nested run(); only the
 * outermost run (the driver's) may suspend when the time slice is used up,
 * so a runaway loop in a task, a timer or an event handler yields to the
 * desktop. Code that cannot yield is stopped after host.nested_limit_ms.
 */
#include "js_ast.h"

#define STACK_VALUES (128 * 1024)
#define MAX_FRAMES   4000
#define BUDGET       20000
#define JS_SUSPEND   JS_MKTAG(JS_TAG_SPECIAL, 7)
#define ITER_FAST    0x40000000u     /* array iterator marker: 0x40000000 | index */

static double mono(JSContext *ctx) { return ctx->host.mono_ms ? ctx->host.mono_ms(ctx->host.ud) : 0; }

static JSValue run(JSContext *ctx, int stop);

/* ==== frames ============================================================================== */
static JSGen *new_gen(JSContext *ctx, JSObject *owner, int kind) {
    JSGen *g = js_mallocz(ctx, sizeof(JSGen));
    if (!g) return NULL;
    g->kind = (uint8_t)kind;
    g->this_val = g->new_target = g->promise = g->resolve = g->reject = g->queue = g->delegate = JS_UNDEFINED;
    owner->u.p = g;
    return g;
}

void js_gen_mark(JSContext *ctx, JSGen *g) {
    if (g->func) js_gc_mark_cell(ctx, g->func);
    js_gc_mark_value(ctx, g->this_val);
    js_gc_mark_value(ctx, g->new_target);
    js_gc_mark_value(ctx, g->promise);
    js_gc_mark_value(ctx, g->resolve);
    js_gc_mark_value(ctx, g->reject);
    js_gc_mark_value(ctx, g->queue);
    js_gc_mark_value(ctx, g->delegate);
    for (uint32_t i = 0; i < g->nsaved; i++) js_gc_mark_value(ctx, g->saved[i]);
}

void js_gen_free(JSContext *ctx, JSGen *g) {
    js_mfree(ctx, g->saved, sizeof(JSValue) * g->savecap);
    js_mfree(ctx, g, sizeof(JSGen));
}

static JSValue make_this_for(JSContext *ctx, JSValue nt) {
    JSValue proto = js_getp(ctx, nt, ctx->a[A_prototype]);
    if (JS_IsException(proto)) return proto;
    return js_obj_new(ctx, CL_OBJECT, JS_IsObject(proto) ? OBJ(proto) : ctx->proto_object);
}

/* push a frame for closure fo; base[0] = function, base[1] = this, args at base + 2 */
static JSFrame *frame_push(JSContext *ctx, JSValue *base, int argc, JSObject *fo, JSValue this_val, JSValue nt, int flags) {
    JSBytecode *b = fo->u.f.b;
    if (ctx->nframes >= MAX_FRAMES) { js_throw_range(ctx, "Maximum call stack size exceeded"); return NULL; }
    JSValue *argv = base + 2;
    JSValue *locals = argv + argc;
    if (locals + b->nlocals + b->stack_max + 32 > ctx->stack_end) { js_throw_range(ctx, "Maximum call stack size exceeded"); return NULL; }
    uint32_t np = b->nparams;
    for (uint32_t i = 0; i < np; i++) locals[i] = i < (uint32_t)argc ? argv[i] : JS_UNDEFINED;
    for (uint32_t i = np; i < b->nlocals; i++) locals[i] = JS_UNDEFINED;
    JSFrame *f = &ctx->frames[ctx->nframes++];
    f->base = base;
    f->argv = argv;
    f->argc = argc;
    f->locals = locals;
    f->sp = locals + b->nlocals;
    f->pc = b->code;
    f->func = fo;
    f->b = b;
    f->flags = (uint16_t)flags;
    f->gen = NULL;
    f->resume = 0;
    f->new_target = nt;
    f->this_val = JS_UNDEFINED;
    f->cont = NULL;
    f->cont_val = JS_UNDEFINED;
    JSValue *hw = f->sp + b->stack_max + 8;
    if (hw > ctx->stack_hw) ctx->stack_hw = hw;
    ctx->sp = f->sp;
    if (flags & FR_CONSTRUCT) {
        if (b->fflags & F_DERIVED) this_val = JS_UNINIT;
        else {
            this_val = make_this_for(ctx, nt);
            if (JS_IsException(this_val)) { ctx->nframes--; return NULL; }
        }
    } else if (!(b->fflags & (F_STRICT | F_ARROW))) {
        if (JS_IsNullish(this_val)) this_val = ctx->global;
        else if (!JS_IsObject(this_val)) {
            this_val = js_toobj(ctx, this_val);
            if (JS_IsException(this_val)) { ctx->nframes--; return NULL; }
        }
    }
    f->this_val = this_val;
    if ((b->fflags & F_ASYNC) && !(b->fflags & F_GEN)) {
        JSObject *ao = js_obj_alloc(ctx, CL_ASYNC, NULL);
        JSGen *g = ao ? new_gen(ctx, ao, 1) : NULL;
        if (!g) { ctx->nframes--; return NULL; }
        f->gen = ao;
        f->flags |= FR_GEN;
        g->func = fo;
        g->b = b;
        g->state = GS_RUNNING;
        g->promise = js_promise_new(ctx, &g->resolve, &g->reject);
        if (JS_IsException(g->promise)) { ctx->nframes--; return NULL; }
    }
    return f;
}

static bool gen_save(JSContext *ctx, JSFrame *f, JSValue *sp, const uint8_t *pc) {
    JSGen *g = f->gen->u.p;
    uint32_t nl = f->b->nlocals, ns = (uint32_t)(sp - (f->locals + nl));
    uint32_t n = nl + ns;
    if (n > g->savecap) {
        uint32_t nc = n + 8;
        JSValue *s = js_realloc(ctx, g->saved, sizeof(JSValue) * g->savecap, sizeof(JSValue) * nc);
        if (!s) return false;
        g->saved = s;
        g->savecap = nc;
    }
    memcpy(g->saved, f->locals, sizeof(JSValue) * n);
    g->nsaved = n;
    g->sp_off = ns;
    g->pc = (uint32_t)(pc - f->b->code);
    g->this_val = f->this_val;
    g->new_target = f->new_target;
    g->func = f->func;
    g->b = f->b;
    return true;
}

static JSFrame *gen_restore(JSContext *ctx, JSObject *go, int flags) {
    JSGen *g = go->u.p;
    JSValue *base = ctx->sp;
    if (ctx->nframes >= MAX_FRAMES || base + 2 + g->nsaved + g->b->stack_max + 32 > ctx->stack_end) {
        js_throw_range(ctx, "Maximum call stack size exceeded");
        return NULL;
    }
    base[0] = JS_MKOBJ(g->func);
    base[1] = g->this_val;
    JSValue *locals = base + 2;
    memcpy(locals, g->saved, sizeof(JSValue) * g->nsaved);
    JSFrame *f = &ctx->frames[ctx->nframes++];
    f->base = base;
    f->argv = locals;
    f->argc = 0;
    f->locals = locals;
    f->sp = locals + g->nsaved;
    f->pc = g->b->code + g->pc;
    f->func = g->func;
    f->b = g->b;
    f->flags = (uint16_t)(flags | FR_GEN);
    f->gen = go;
    f->resume = 0;
    f->this_val = g->this_val;
    f->new_target = g->new_target;
    f->cont = NULL;
    f->cont_val = JS_UNDEFINED;
    JSValue *hw = f->sp + g->b->stack_max + 8;
    if (hw > ctx->stack_hw) ctx->stack_hw = hw;
    ctx->sp = f->sp;
    g->state = GS_RUNNING;
    return f;
}

void js_vm_mark(JSContext *ctx) {
    for (int i = 0; i < ctx->nframes; i++) {
        JSFrame *f = &ctx->frames[i];
        if (f->func) js_gc_mark_cell(ctx, f->func);
        js_gc_mark_value(ctx, f->this_val);
        js_gc_mark_value(ctx, f->new_target);
        js_gc_mark_value(ctx, f->cont_val);
        if (f->gen) js_gc_mark_cell(ctx, f->gen);
    }
    js_gc_mark_value(ctx, ctx->resume_val);
    js_gc_mark_value(ctx, ctx->host_val);
    js_gc_mark_value(ctx, ctx->import_hook);
}

/* ==== closures ================================================================================ */
static JSObject *closure_proto(JSContext *ctx, JSBytecode *b) {
    uint32_t f = b->fflags;
    if ((f & F_GEN) && (f & F_ASYNC)) return ctx->proto_async_gen_func;
    if (f & F_GEN) return ctx->proto_gen_func;
    if (f & F_ASYNC) return ctx->proto_async_func;
    return ctx->proto_function;
}

static JSValue make_closure(JSContext *ctx, JSBytecode *b, JSFrame *fr) {
    JSObject *o = js_obj_alloc(ctx, CL_FUNCTION, closure_proto(ctx, b));
    if (!o) return JS_EXCEPTION;
    o->h.flags |= OF_LAZY;
    o->u.f.b = b;
    if (b->ncv) {
        JSVarRef **vr = js_malloc(ctx, sizeof(JSVarRef *) * b->ncv);
        if (!vr) return JS_EXCEPTION;
        for (int i = 0; i < b->ncv; i++) {
            uint16_t src = b->cv[i];
            if (src & 0x8000) {
                JSValue box = fr->locals[src & 0x7FFF];
                vr[i] = JS_IsInternal(box) ? JS_PTR(box) : NULL;
            } else vr[i] = fr->func->u.f.vr ? fr->func->u.f.vr[src] : NULL;
            if (!vr[i]) {
                /* (should not happen: a box missing) - give it an empty one */
                JSVarRef *r = js_cell(ctx, T_VARREF, sizeof(JSVarRef));
                if (!r) { js_mfree(ctx, vr, sizeof(JSVarRef *) * b->ncv); return JS_EXCEPTION; }
                r->v = JS_UNDEFINED;
                vr[i] = r;
            }
        }
        o->u.f.vr = vr;
    }
    if (b->fflags & F_ARROW) o->u.f.home = fr && fr->func ? fr->func->u.f.home : NULL;
    return JS_MKOBJ(o);
}

JSValue js_new_closure(JSContext *ctx, JSBytecode *b, JSVarRef **vr, JSObject *home) {
    JSObject *o = js_obj_alloc(ctx, CL_FUNCTION, closure_proto(ctx, b));
    if (!o) return JS_EXCEPTION;
    o->h.flags |= OF_LAZY;
    o->u.f.b = b;
    o->u.f.vr = vr;
    o->u.f.home = home;
    return JS_MKOBJ(o);
}

/* ==== calls from C ==================================================================================== */
static inline void after_nested(JSContext *ctx);

bool js_check_stack(JSContext *ctx) { return ctx->native_depth < 180; }

static JSValue call_native(JSContext *ctx, JSObject *o, JSValue this_val, int argc, JSValue *argv, JSValue nt) {
    if (ctx->native_depth >= 180) return js_throw_range(ctx, "Maximum call stack size exceeded");
    JSValue sd = ctx->native_data, snt = ctx->native_nt;
    ctx->native_data = o->u.n.data;
    ctx->native_nt = nt;
    ctx->native_depth++;
    /* natives may read up to their declared length: pad short argument lists */
    JSValue pad[8];
    if (argc < o->u.n.length && o->u.n.length <= 8) {
        for (int i = 0; i < argc; i++) pad[i] = argv[i];
        for (int i = argc; i < o->u.n.length; i++) pad[i] = JS_UNDEFINED;
        argv = pad;
    }
    JSValue r = o->u.n.fn(ctx, this_val, argc, argv, o->u.n.magic);
    ctx->native_depth--;
    ctx->native_data = sd;
    ctx->native_nt = snt;
    if (JS_IsException(r) && !ctx->has_exc) js_throw_type(ctx, "native function failed");
    return r;
}

JSValue js_call_internal(JSContext *ctx, JSValue fn, JSValue this_val, int argc, JSValue *argv, JSValue nt, int flags) {
    if (!JS_IsObject(fn)) goto not_fn;
    JSObject *o = OBJ(fn);
    bool construct = (flags & FR_CONSTRUCT) != 0;
    switch (o->h.cls) {
    case CL_FUNCTION: {
        uint32_t ff = o->u.f.b->fflags;
        if ((ff & F_CLASSCTOR) && !construct) {
            return js_throw_type(ctx, "Class constructor cannot be invoked without 'new'");
        }
        if (ctx->native_depth >= 180) return js_throw_range(ctx, "Maximum call stack size exceeded");
        JSValue *base = ctx->sp;
        if (base + argc + 2 + 64 > ctx->stack_end) return js_throw_range(ctx, "Maximum call stack size exceeded");
        base[0] = fn;
        base[1] = this_val;
        for (int i = 0; i < argc; i++) base[2 + i] = argv[i];
        ctx->sp = base + 2 + argc;
        int stop = ctx->nframes;
        JSFrame *f = frame_push(ctx, base, argc, o, this_val, nt, FR_BOUNDARY | (construct ? FR_CONSTRUCT : 0));
        if (!f) { ctx->sp = base; return JS_EXCEPTION; }
        ctx->native_depth++;
        JSValue r = run(ctx, stop);
        ctx->native_depth--;
        after_nested(ctx);
        ctx->sp = base;
        return r;
    }
    case CL_NATIVE:
        if (construct && !(o->h.flags & OF_CTOR)) goto not_ctor;
        return call_native(ctx, o, this_val, argc, argv, construct ? nt : JS_UNDEFINED);
    case CL_BOUND: {
        int n = o->u.bf.argc + argc;
        JSValue small[16], *args = n <= 16 ? small : malloc(sizeof(JSValue) * (size_t)n);
        if (!args) return js_throw_oom(ctx);
        for (int i = 0; i < o->u.bf.argc; i++) args[i] = o->u.bf.args[i];
        for (int i = 0; i < argc; i++) args[o->u.bf.argc + i] = argv[i];
        JSValue r;
        if (construct) r = js_call_internal(ctx, o->u.bf.target, JS_UNDEFINED, n, args, nt == fn ? o->u.bf.target : nt, FR_CONSTRUCT);
        else r = js_call_internal(ctx, o->u.bf.target, o->u.bf.this_val, n, args, JS_UNDEFINED, 0);
        if (args != small) free(args);
        return r;
    }
    case CL_PROXY:
        if (construct) return js_proxy_construct(ctx, o, argc, argv, nt);
        return js_proxy_call(ctx, o, this_val, argc, argv);
    default: break;
    }
not_fn:
    {
        char d[64];
        js_value_desc(ctx, fn, d, sizeof d);
        return js_throw_type(ctx, "%s is not a function", d);
    }
not_ctor:
    return js_throw_type(ctx, "not a constructor");
}

JSValue js_callv(JSContext *ctx, JSValue fn, JSValue this_val, int argc, JSValue *argv) {
    return js_call_internal(ctx, fn, this_val, argc, argv, JS_UNDEFINED, 0);
}

JSValue js_construct_internal(JSContext *ctx, JSValue fn, int argc, JSValue *argv, JSValue nt) {
    if (!js_is_ctor(fn)) {
        char d[64];
        js_value_desc(ctx, fn, d, sizeof d);
        return js_throw_type(ctx, "%s is not a constructor", d);
    }
    return js_call_internal(ctx, fn, JS_UNDEFINED, argc, argv, nt, FR_CONSTRUCT);
}

static void reset_vm(JSContext *ctx);

/* A call from the host (not from a running script) cannot suspend: it runs to the end
   or is stopped after nested_limit_ms like code inside a native call. */
static bool host_call_enter(JSContext *ctx) {
    if (ctx->run_depth) return false;
    ctx->no_suspend_saved = ctx->no_suspend;
    ctx->no_suspend = true;
    ctx->slice_start_d = mono(ctx);
    ctx->slice_over = false;
    ctx->budget = BUDGET;
    return true;
}

static JSValue host_call_leave(JSContext *ctx, bool top, JSValue r) {
    if (!top) return r;
    ctx->no_suspend = ctx->no_suspend_saved;
    if (ctx->killed) {
        reset_vm(ctx);
        ctx->killed = false;
        return JS_EXCEPTION;
    }
    return r;
}

JSValue js_call(JSContext *ctx, JSValue fn, JSValue this_val, int argc, const JSValue *argv) {
    JS_GC_ENTER(ctx);
    bool top = host_call_enter(ctx);
    JSValue r = js_call_internal(ctx, fn, this_val, argc, (JSValue *)argv, JS_UNDEFINED, 0);
    r = host_call_leave(ctx, top, r);
    JS_GC_LEAVE(ctx);
    return r;
}

JSValue js_construct(JSContext *ctx, JSValue fn, int argc, const JSValue *argv) {
    JS_GC_ENTER(ctx);
    bool top = host_call_enter(ctx);
    JSValue r = js_construct_internal(ctx, fn, argc, (JSValue *)argv, fn);
    r = host_call_leave(ctx, top, r);
    JS_GC_LEAVE(ctx);
    return r;
}

/* ==== proxies: call / construct traps ================================================================= */
JSValue js_proxy_call(JSContext *ctx, JSObject *p, JSValue this_val, int argc, JSValue *argv) {
    if (!JS_IsObject(p->u.px.handler)) return js_throw_type(ctx, "Cannot perform 'apply' on a proxy that has been revoked");
    JSValue t = js_get_method(ctx, p->u.px.handler, ctx->a[A_apply]);
    if (JS_IsException(t)) return t;
    if (JS_IsUndefined(t)) return js_callv(ctx, p->u.px.target, this_val, argc, argv);
    JSValue arr = js_array_from_list(ctx, argv, (uint32_t)argc);
    if (JS_IsException(arr)) return arr;
    JSValue args[3] = { p->u.px.target, this_val, arr };
    return js_callv(ctx, t, p->u.px.handler, 3, args);
}

JSValue js_proxy_construct(JSContext *ctx, JSObject *p, int argc, JSValue *argv, JSValue nt) {
    if (!JS_IsObject(p->u.px.handler)) return js_throw_type(ctx, "Cannot perform 'construct' on a proxy that has been revoked");
    JSValue t = js_get_method(ctx, p->u.px.handler, ctx->a[A_construct]);
    if (JS_IsException(t)) return t;
    if (JS_IsUndefined(t)) return js_construct_internal(ctx, p->u.px.target, argc, argv, nt == JS_MKOBJ(p) ? p->u.px.target : nt);
    JSValue arr = js_array_from_list(ctx, argv, (uint32_t)argc);
    if (JS_IsException(arr)) return arr;
    JSValue args[3] = { p->u.px.target, arr, nt };
    JSValue r = js_callv(ctx, t, p->u.px.handler, 3, args);
    if (!JS_IsException(r) && !JS_IsObject(r)) return js_throw_type(ctx, "proxy [[Construct]] must return an object");
    return r;
}

/* ==== globals ============================================================================================ */
static JSValue global_get(JSContext *ctx, JSValue name, bool typeof_mode) {
    JSProp *p = js_find_prop(ctx, OBJ(ctx->global_lex), name);
    if (p) {
        if (p->val == JS_UNINIT) {
            char nm[80];
            js_str_to_ascii(ctx, name, nm, sizeof nm);
            return js_throw_ref(ctx, "Cannot access '%s' before initialization", nm);
        }
        return p->val;
    }
    JSObject *g = OBJ(ctx->global);
    p = js_find_prop(ctx, g, name);
    if (p && !(p->flags & P_ACC)) return p->val;
    int h = js_has_prop(ctx, ctx->global, name);
    if (h < 0) return JS_EXCEPTION;
    if (!h) {
        if (typeof_mode) return JS_UNDEFINED;
        char nm[80];
        js_str_to_ascii(ctx, name, nm, sizeof nm);
        return js_throw_ref(ctx, "%s is not defined", nm);
    }
    return js_getp(ctx, ctx->global, name);
}

static int global_put(JSContext *ctx, JSValue name, JSValue v, bool strict) {
    JSProp *p = js_find_prop(ctx, OBJ(ctx->global_lex), name);
    char nm[80];
    if (p) {
        if (p->val == JS_UNINIT) { js_str_to_ascii(ctx, name, nm, sizeof nm); js_throw_ref(ctx, "Cannot access '%s' before initialization", nm); return -1; }
        if (!(p->flags & P_W)) { js_throw_type(ctx, "Assignment to constant variable."); return -1; }
        p->val = v;
        return 0;
    }
    JSObject *g = OBJ(ctx->global);
    p = js_find_prop(ctx, g, name);
    if (p && !(p->flags & P_ACC) && (p->flags & P_W)) { p->val = v; return 0; }
    if (strict && !p) {
        int h = js_has_prop(ctx, ctx->global, name);
        if (h < 0) return -1;
        if (!h) { js_str_to_ascii(ctx, name, nm, sizeof nm); js_throw_ref(ctx, "%s is not defined", nm); return -1; }
    }
    return js_setp(ctx, ctx->global, name, v, ctx->global, strict) < 0 ? -1 : 0;
}

/* ==== iteration helpers ================================================================================= */
static JSValue forin_start(JSContext *ctx, JSValue obj) {
    JSValue keys = js_array_new(ctx, 0);
    if (JS_IsException(keys)) return keys;
    JSValue it = js_new_iter(ctx, NULL, obj, ITT_FORIN, 0);
    if (JS_IsException(it)) return it;
    JSIter *st = OBJ(it)->u.p;
    st->extra = keys;
    if (JS_IsNullish(obj)) { st->done = true; return it; }
    JSValue o = js_toobj(ctx, obj);
    if (JS_IsException(o)) return o;
    st->target = o;
    /* own + inherited enumerable string keys, shadowed ones once */
    JSValue seen = js_obj_new(ctx, CL_OBJECT, NULL);
    if (JS_IsException(seen)) return seen;
    bool exc;
    for (JSValue cur = o; JS_IsObject(cur);) {
        JSValue ks = js_own_keys(ctx, cur, OK_STRINGS);
        if (JS_IsException(ks)) return ks;
        uint32_t n = OBJ(ks)->u.a.len;
        for (uint32_t i = 0; i < n; i++) {
            JSValue k = OBJ(ks)->u.a.v[i];
            if (js_find_prop(ctx, OBJ(seen), k)) continue;
            js_add_prop(ctx, OBJ(seen), k, JS_TRUE, P_W);
            JSDesc d;
            int r = js_get_own_desc(ctx, cur, k, &d);
            if (r < 0) return JS_EXCEPTION;
            if (r && (d.flags & P_E)) js_array_push_v(ctx, keys, k);
        }
        JSObject *p = js_get_proto_obj(ctx, cur, &exc);
        if (exc) return JS_EXCEPTION;
        cur = p ? JS_MKOBJ(p) : JS_NULL;
    }
    return it;
}

/* 1 = key in *out, 0 = done, -1 exception */
static int forin_next(JSContext *ctx, JSValue it, JSValue *out) {
    JSIter *st = OBJ(it)->u.p;
    if (st->done) return 0;
    JSObject *keys = OBJ(st->extra);
    while (st->idx < keys->u.a.len) {
        JSValue k = keys->u.a.v[st->idx++];
        int h = js_has_prop(ctx, st->target, k);
        if (h < 0) return -1;
        if (h) { *out = k; return 1; }
    }
    st->done = true;
    return 0;
}

static bool array_iter_pristine(JSContext *ctx, JSValue obj) {
    if (!js_fast_array(obj) || OBJ(obj)->proto != ctx->proto_array) return false;
    JSProp *p = js_find_prop(ctx, ctx->proto_array, ctx->wk[WK_ITERATOR]);
    if (!p || p->val != ctx->array_values) return false;
    if (js_find_prop(ctx, OBJ(obj), ctx->wk[WK_ITERATOR])) return false;
    JSProp *n = js_find_prop(ctx, ctx->proto_array_iter, ctx->a[A_next]);
    return n && n->val == ctx->array_iter_next;
}

/* ==== the interpreter ==================================================================================== */
#define RD8()  (*pc++)
#define RD16() (pc += 2, (uint32_t)(pc[-2] | (pc[-1] << 8)))
#define RD32() (pc += 4, (uint32_t)pc[-4] | ((uint32_t)pc[-3] << 8) | ((uint32_t)pc[-2] << 16) | ((uint32_t)pc[-1] << 24))
#define SAVE() do { f->pc = pc; f->sp = sp; ctx->sp = sp; } while (0)
#define LOAD() do { f = &ctx->frames[ctx->nframes - 1]; pc = f->pc; sp = f->sp; b = f->b; locals = f->locals; cp = b->cp; } while (0)
#define THROW() goto exception
#define CHECK(v) do { if (JS_UNLIKELY(JS_IsException(v))) THROW(); } while (0)
#define STRICT() ((b->fflags & F_STRICT) != 0)

static JSValue tdz_error(JSContext *ctx, JSBytecode *b, uint32_t slot) {
    char nm[80] = "variable";
    if (b->lnames && slot < b->nlocals && JS_IsString(b->lnames[slot])) js_str_to_ascii(ctx, b->lnames[slot], nm, sizeof nm);
    return js_throw_ref(ctx, "Cannot access '%s' before initialization", nm);
}

static JSValue not_callable(JSContext *ctx, JSValue fn, JSBytecode *b, uint32_t nameidx) {
    char nm[96], d[64];
    js_value_desc(ctx, fn, d, sizeof d);
    if (nameidx != 0xFFFFFFFFu && nameidx < b->ncp && JS_IsString(b->cp[nameidx])) {
        js_str_to_ascii(ctx, b->cp[nameidx], nm, sizeof nm);
        return js_throw_type(ctx, "%s is not a function", nm);
    }
    return js_throw_type(ctx, "%s is not a function", d);
}

/* log that a runaway script was stopped, and where (allocates nothing on the JS heap) */
static void log_stopped_at(JSContext *ctx) {
    if (!ctx->host.log) return;
    char buf[700];
    size_t n = (size_t)snprintf(buf, sizeof buf, "Script stopped: it ran too long (page not responding)");
    for (int i = ctx->nframes - 1, shown = 0; i >= 0 && shown < 6 && n < sizeof buf - 150; i--) {
        JSBytecode *b = ctx->frames[i].b;
        if (!b) continue;
        char nm[48] = "", src[80] = "";
        if (JS_IsString(b->name)) js_str_to_ascii(ctx, b->name, nm, sizeof nm);
        if (JS_IsString(b->srcname)) js_str_to_ascii(ctx, b->srcname, src, sizeof src);
        n += (size_t)snprintf(buf + n, sizeof buf - n, "\n    at %s (%s:%d)", nm[0] ? nm : (b->fflags & F_SCRIPT) ? "<script>" : "<anonymous>",
                              src, js_line_of(b, (uint32_t)(ctx->frames[i].pc - b->code)));
        shown++;
    }
    ctx->host.log(ctx->host.ud, JS_LOG_ERROR, buf);
}

static int budget_expired(JSContext *ctx) {
    ctx->budget = BUDGET;
    if (ctx->killed) return -1;
    double now = mono(ctx);
    double el = now - ctx->slice_start_d;
    if (ctx->run_depth == 1 && !ctx->no_suspend) { ctx->slice_over = false; return el >= ctx->host.slice_ms ? 1 : 0; }
    if (el >= ctx->host.nested_limit_ms) {
        log_stopped_at(ctx);
        ctx->killed = true;
        return -1;
    }
    /* inside a nested call (a getter, a proxy trap, a callback of a native): when the slice
       is used up, look again soon, so the task suspends as soon as its own frame runs again */
    if (el >= ctx->host.slice_ms && !ctx->no_suspend) { ctx->budget = 200; ctx->slice_over = true; }
    return 0;
}

/* back from a nested run: the task's own frame suspends at once when the slice ran out */
static inline void after_nested(JSContext *ctx) {
    if (ctx->slice_over && ctx->run_depth == 1) ctx->budget = 1;
}

static JSValue killed_error(JSContext *ctx) {
    ctx->uncatchable = true;
    ctx->has_exc = true;
    ctx->exception = js_strc(ctx, "Script stopped: it ran too long (page not responding)");
    return JS_EXCEPTION;
}

/* the result of a generator / async frame that finished (returned or threw) */
static JSValue gen_finish(JSContext *ctx, JSFrame *f, JSValue v, bool threw, bool *absorbed) {
    JSObject *go = f->gen;
    JSGen *g = go->u.p;
    *absorbed = false;
    g->state = GS_DONE;
    js_mfree(ctx, g->saved, sizeof(JSValue) * g->savecap);
    g->saved = NULL;
    g->nsaved = g->savecap = 0;
    if (g->kind == 1) {
        JSValue r;
        if (threw) {
            JSValue e = js_take_exception(ctx);
            r = js_callv(ctx, g->reject, JS_UNDEFINED, 1, &e);
            *absorbed = true;
        } else r = js_callv(ctx, g->resolve, JS_UNDEFINED, 1, &v);
        if (JS_IsException(r)) js_take_exception(ctx);
        return (f->flags & FR_RESUMED) ? JS_UNDEFINED : g->promise;
    }
    if (!threw) ctx->gen_done = true;
    return v;
}

static JSValue arguments_obj(JSContext *ctx, JSFrame *f) {
    JSObject *a = js_obj_alloc(ctx, CL_ARGUMENTS, ctx->proto_object);
    if (!a) return JS_EXCEPTION;
    JSValue av = JS_MKOBJ(a);
    if (f->argc && js_array_ensure(ctx, a, (uint32_t)f->argc) < 0) return JS_EXCEPTION;
    for (int i = 0; i < f->argc; i++) a->u.a.v[i] = f->argv[i];
    a->u.a.len = (uint32_t)f->argc;
    js_add_prop(ctx, a, ctx->a[A_length], js_mknum(f->argc), P_W | P_C);
    js_add_prop(ctx, a, ctx->wk[WK_ITERATOR], ctx->array_values, P_W | P_C);
    if (f->b->fflags & F_STRICT) {
        JSAccessor *acc = js_cell(ctx, T_ACCESSOR, sizeof(JSAccessor));
        if (!acc) return JS_EXCEPTION;
        acc->get = acc->set = ctx->throw_type_error;
        js_add_prop(ctx, a, ctx->a[A_callee], JS_MKINT(acc), P_ACC);
    } else js_add_prop(ctx, a, ctx->a[A_callee], JS_MKOBJ(f->func), P_W | P_C);
    return av;
}

static JSValue iter_step_value(JSContext *ctx, JSValue *it, bool *done) {
    /* it[0] iter, it[1] next (or fast marker); *done set when finished */
    if (JS_IsMarker(it[1])) {
        uint32_t idx = (uint32_t)it[1] & 0x3FFFFFFFu;
        JSObject *a = OBJ(it[0]);
        if (a->h.cls == CL_ARRAY && !(a->h.flags & OF_SLOW) && idx < a->u.a.len) {
            JSValue v = idx < a->u.a.cap ? a->u.a.v[idx] : JS_HOLE;
            it[1] = JS_MKMARK(ITER_FAST | (idx + 1));
            *done = false;
            return v == JS_HOLE ? (a->proto ? js_geti(ctx, it[0], idx) : JS_UNDEFINED) : v;
        }
        if (a->h.cls == CL_ARRAY && (a->h.flags & OF_SLOW) && idx < a->u.a.len) {
            it[1] = JS_MKMARK(ITER_FAST | (idx + 1));
            *done = false;
            return js_geti(ctx, it[0], idx);
        }
        *done = true;
        return JS_UNDEFINED;
    }
    return js_iter_step(ctx, it[0], it[1], done);
}

static int iter_close_v(JSContext *ctx, JSValue iter, JSValue next, bool throwing) {
    if (JS_IsMarker(next)) return 0;
    return js_iter_close(ctx, iter, throwing);
}

static JSValue define_method(JSContext *ctx, JSValue obj, JSValue key, JSValue fn, int kind) {
    if (js_is_cls(fn, CL_FUNCTION)) OBJ(fn)->u.f.home = OBJ(obj);
    js_set_func_name(ctx, fn, key, (kind & 3) == 1 ? "get" : (kind & 3) == 2 ? "set" : NULL);
    uint32_t en = (kind & 4) ? P_E : 0;
    JSDesc d;
    if ((kind & 3) == 0) {
        d.value = fn; d.get = d.set = JS_UNDEFINED;
        d.flags = DP_DATA_ALL | P_W | P_C | en | DP_THROW;
    } else {
        d.value = JS_UNDEFINED;
        d.get = (kind & 3) == 1 ? fn : JS_UNDEFINED;
        d.set = (kind & 3) == 2 ? fn : JS_UNDEFINED;
        d.flags = ((kind & 3) == 1 ? DP_HAS_GET : DP_HAS_SET) | DP_HAS_E | DP_HAS_C | P_C | en | DP_THROW;
    }
    if (JS_IsSymbol(key) && (SYM(key)->h.flags & SYM_PRIVATE)) {
        /* private methods / accessors: own property keyed by the private symbol */
        JSObject *o = OBJ(obj);
        JSProp *p = js_find_prop(ctx, o, key);
        if ((kind & 3) && p && (p->flags & P_ACC)) {
            JSAccessor *a = JS_PTR(p->val);
            if ((kind & 3) == 1) a->get = fn; else a->set = fn;
            return obj;
        }
    }
    if (js_define_own(ctx, obj, key, &d) < 0) return JS_EXCEPTION;
    return obj;
}

static JSValue copy_data(JSContext *ctx, JSValue target, JSValue src, JSValue excl) {
    if (JS_IsNullish(src)) return target;
    JSValue s = js_toobj(ctx, src);
    if (JS_IsException(s)) return s;
    JSValue keys = js_own_keys(ctx, s, OK_STRINGS | OK_SYMBOLS);
    if (JS_IsException(keys)) return keys;
    uint32_t n = OBJ(keys)->u.a.len;
    for (uint32_t i = 0; i < n; i++) {
        JSValue k = OBJ(keys)->u.a.v[i];
        if (JS_IsObject(excl)) {
            bool skip = false;
            JSObject *e = OBJ(excl);
            for (uint32_t j = 0; j < e->u.a.len; j++) {
                JSValue x = e->u.a.v[j];
                if (JS_IsString(x)) x = js_atom(ctx, x);
                if (x == k) { skip = true; break; }
            }
            if (skip) continue;
        }
        JSDesc d;
        int r = js_get_own_desc(ctx, s, k, &d);
        if (r < 0) return JS_EXCEPTION;
        if (!r || !(d.flags & P_E)) continue;
        JSValue v = js_getp(ctx, s, k);
        if (JS_IsException(v)) return v;
        if (js_create_data_prop(ctx, target, k, v) < 0) return JS_EXCEPTION;
    }
    return target;
}

static JSValue private_get(JSContext *ctx, JSValue obj, JSValue sym) {
    if (!JS_IsObject(obj)) return js_throw_type(ctx, "Cannot read private member from a non-object");
    for (JSObject *o = OBJ(obj); o; o = o->proto) {
        JSProp *p = js_find_prop(ctx, o, sym);
        if (p) {
            if (p->flags & P_ACC) {
                JSAccessor *a = JS_PTR(p->val);
                if (JS_IsUndefined(a->get)) return js_throw_type(ctx, "'%s' was defined without a getter", "private accessor");
                return js_callv(ctx, a->get, obj, 0, NULL);
            }
            return p->val;
        }
    }
    char nm[80];
    js_str_to_ascii(ctx, SYM(sym)->desc, nm, sizeof nm);
    return js_throw_type(ctx, "Cannot read private member %s from an object whose class did not declare it", nm);
}

static int private_put(JSContext *ctx, JSValue obj, JSValue sym, JSValue v) {
    char nm[80];
    if (!JS_IsObject(obj)) { js_throw_type(ctx, "Cannot write private member to a non-object"); return -1; }
    for (JSObject *o = OBJ(obj); o; o = o->proto) {
        JSProp *p = js_find_prop(ctx, o, sym);
        if (!p) continue;
        if (p->flags & P_ACC) {
            JSAccessor *a = JS_PTR(p->val);
            if (JS_IsUndefined(a->set)) { js_str_to_ascii(ctx, SYM(sym)->desc, nm, sizeof nm); js_throw_type(ctx, "'%s' was defined without a setter", nm); return -1; }
            JSValue r = js_callv(ctx, a->set, obj, 1, &v);
            return JS_IsException(r) ? -1 : 0;
        }
        if (o != OBJ(obj)) break;
        p->val = v;
        return 0;
    }
    js_str_to_ascii(ctx, SYM(sym)->desc, nm, sizeof nm);
    js_throw_type(ctx, "Cannot write private member %s to an object whose class did not declare it", nm);
    return -1;
}

static JSValue class_create(JSContext *ctx, JSValue heritage, JSValue ctor, bool has_extends, JSValue *proto_out) {
    JSObject *pp = ctx->proto_object, *cp_ = ctx->proto_function;
    if (has_extends) {
        if (JS_IsNull(heritage)) pp = NULL;
        else {
            if (!js_is_ctor(heritage)) {
                char d[64];
                js_value_desc(ctx, heritage, d, sizeof d);
                return js_throw_type(ctx, "Class extends value %s is not a constructor or null", d);
            }
            JSValue hp = js_getp(ctx, heritage, ctx->a[A_prototype]);
            if (JS_IsException(hp)) return hp;
            if (JS_IsNull(hp)) pp = NULL;
            else if (JS_IsObject(hp)) pp = OBJ(hp);
            else return js_throw_type(ctx, "Class extends value does not have valid prototype property");
            cp_ = OBJ(heritage);
        }
    }
    JSValue proto = js_obj_new(ctx, CL_OBJECT, pp);
    if (JS_IsException(proto)) return proto;
    JSObject *co = OBJ(ctor);
    co->proto = cp_;
    js_materialize(ctx, co);
    js_add_prop(ctx, co, ctx->a[A_prototype], proto, 0);
    js_add_prop(ctx, OBJ(proto), ctx->a[A_constructor], ctor, P_W | P_C);
    co->u.f.home = OBJ(proto);
    *proto_out = proto;
    return ctor;
}

static JSValue run(JSContext *ctx, int stop) {
    JSFrame *f;
    const uint8_t *pc;
    JSValue *sp, *locals, *cp;
    JSBytecode *b;
    ctx->run_depth++;
    LOAD();
    if (ctx->host_push) { ctx->host_push = false; *sp++ = ctx->host_val; ctx->host_val = JS_UNDEFINED; }
    if (ctx->resume_mode == GR_THROW) { ctx->resume_mode = 0; goto exception; }
    if (ctx->resume_mode == GR_RETURN) { ctx->resume_mode = 0; goto do_return; }
    for (;;) {
        if (JS_UNLIKELY(--ctx->budget <= 0)) {
            SAVE();
            int r = budget_expired(ctx);
            if (r > 0) { ctx->run_depth--; return JS_SUSPEND; }
            if (r < 0) { killed_error(ctx); THROW(); }
        }
        uint8_t op = *pc++;
        switch (op) {
        case OP_NOP: case OP_LOOP_HINT: case OP_DEBUGGER: break;
        case OP_PUSH_UNDEF: *sp++ = JS_UNDEFINED; break;
        case OP_PUSH_NULL: *sp++ = JS_NULL; break;
        case OP_PUSH_TRUE: *sp++ = JS_TRUE; break;
        case OP_PUSH_FALSE: *sp++ = JS_FALSE; break;
        case OP_PUSH_I8: *sp++ = js_mknum((int8_t)RD8()); break;
        case OP_PUSH_I32: *sp++ = js_mknum((int32_t)RD32()); break;
        case OP_PUSH_CONST: *sp++ = cp[RD32()]; break;
        case OP_PUSH_THIS: *sp++ = f->this_val; break;
        case OP_PUSH_NEW_TARGET: *sp++ = f->new_target; break;
        case OP_PUSH_FUNC: *sp++ = JS_MKOBJ(f->func); break;
        case OP_PUSH_UNINIT: *sp++ = JS_UNINIT; break;
        case OP_PUSH_EMPTY: *sp++ = ctx->empty_str; break;
        case OP_PUSH_HOME: *sp++ = f->func->u.f.home ? JS_MKOBJ(f->func->u.f.home) : JS_UNDEFINED; break;
        case OP_OBJECT_NEW: {
            SAVE();
            JSValue o = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
            CHECK(o);
            *sp++ = o;
            break;
        }
        case OP_ARRAY_NEW: {
            uint32_t n = RD16();
            SAVE();
            JSValue a = js_array_from_list(ctx, sp - n, n);
            CHECK(a);
            sp -= n;
            *sp++ = a;
            break;
        }
        case OP_ARRAY_PUSH: {
            SAVE();
            if (js_array_push_v(ctx, sp[-2], sp[-1]) < 0) THROW();
            sp--;
            break;
        }
        case OP_ARRAY_HOLE: {
            JSObject *a = OBJ(sp[-1]);
            a->u.a.len++;
            break;
        }
        case OP_ARRAY_SPREAD: {
            SAVE();
            JSValue it = sp[-1], arr = sp[-2];
            if (array_iter_pristine(ctx, it)) {
                JSObject *s = OBJ(it);
                for (uint32_t i = 0; i < s->u.a.len; i++) {
                    JSValue v = i < s->u.a.cap ? s->u.a.v[i] : JS_HOLE;
                    if (v == JS_HOLE) v = JS_UNDEFINED;
                    if (js_array_push_v(ctx, arr, v) < 0) THROW();
                    s = OBJ(it);
                }
            } else {
                JSValue nx;
                JSValue iter = js_get_iterator(ctx, it, &nx, false);
                CHECK(iter);
                sp[-1] = iter;
                *sp++ = nx;
                SAVE();
                for (;;) {
                    bool done;
                    JSValue v = js_iter_step(ctx, iter, nx, &done);
                    CHECK(v);
                    if (done) break;
                    if (js_array_push_v(ctx, arr, v) < 0) THROW();
                }
                sp--;
            }
            sp--;
            break;
        }
        case OP_CLOSURE: {
            JSBytecode *nb = JS_PTR(cp[RD32()]);
            SAVE();
            JSValue c = make_closure(ctx, nb, f);
            CHECK(c);
            *sp++ = c;
            break;
        }
        case OP_REGEXP: {
            uint32_t i = RD32();
            SAVE();
            JSValue r = js_regexp_create(ctx, cp[i], cp[i + 1]);
            CHECK(r);
            *sp++ = r;
            break;
        }
        case OP_TEMPLATE: *sp++ = cp[RD32()]; break;
        case OP_POP: sp--; break;
        case OP_DUP: sp[0] = sp[-1]; sp++; break;
        case OP_DUP2: sp[0] = sp[-2]; sp[1] = sp[-1]; sp += 2; break;
        case OP_SWAP: { JSValue t = sp[-1]; sp[-1] = sp[-2]; sp[-2] = t; break; }
        case OP_ROT3L: { JSValue t = sp[-3]; sp[-3] = sp[-2]; sp[-2] = sp[-1]; sp[-1] = t; break; }
        case OP_ROT3R: { JSValue t = sp[-1]; sp[-1] = sp[-2]; sp[-2] = sp[-3]; sp[-3] = t; break; }
        case OP_OVER: sp[0] = sp[-2]; sp++; break;
        case OP_ROT4L: { JSValue t = sp[-4]; sp[-4] = sp[-3]; sp[-3] = sp[-2]; sp[-2] = sp[-1]; sp[-1] = t; break; }
        case OP_SWAP2: { JSValue a = sp[-4], c = sp[-3]; sp[-4] = sp[-2]; sp[-3] = sp[-1]; sp[-2] = a; sp[-1] = c; break; }
        case OP_NIP: sp[-2] = sp[-1]; sp--; break;

        case OP_GET_LOC: *sp++ = locals[RD16()]; break;
        case OP_PUT_LOC: locals[RD16()] = *--sp; break;
        case OP_SET_LOC: locals[RD16()] = sp[-1]; break;
        case OP_GET_LOC_CHK: {
            uint32_t i = RD16();
            if (JS_UNLIKELY(locals[i] == JS_UNINIT)) { SAVE(); tdz_error(ctx, b, i); THROW(); }
            *sp++ = locals[i];
            break;
        }
        case OP_PUT_LOC_CHK: {
            uint32_t i = RD16();
            if (JS_UNLIKELY(locals[i] == JS_UNINIT)) { SAVE(); tdz_error(ctx, b, i); THROW(); }
            locals[i] = *--sp;
            break;
        }
        case OP_GET_BOX: *sp++ = ((JSVarRef *)JS_PTR(locals[RD16()]))->v; break;
        case OP_PUT_BOX: ((JSVarRef *)JS_PTR(locals[RD16()]))->v = *--sp; break;
        case OP_INIT_BOX: ((JSVarRef *)JS_PTR(locals[RD16()]))->v = *--sp; break;
        case OP_SET_BOX: ((JSVarRef *)JS_PTR(locals[RD16()]))->v = sp[-1]; break;
        case OP_GET_BOX_CHK: {
            uint32_t i = RD16();
            JSValue v = ((JSVarRef *)JS_PTR(locals[i]))->v;
            if (JS_UNLIKELY(v == JS_UNINIT)) { SAVE(); tdz_error(ctx, b, i); THROW(); }
            *sp++ = v;
            break;
        }
        case OP_PUT_BOX_CHK: {
            uint32_t i = RD16();
            JSVarRef *r = JS_PTR(locals[i]);
            if (JS_UNLIKELY(r->v == JS_UNINIT)) { SAVE(); tdz_error(ctx, b, i); THROW(); }
            r->v = *--sp;
            break;
        }
        case OP_NEW_BOX: case OP_NEW_BOX_UNDEF: case OP_COPY_BOX: {
            uint32_t i = RD16();
            SAVE();
            JSVarRef *r = js_cell(ctx, T_VARREF, sizeof(JSVarRef));
            if (!r) THROW();
            r->v = op == OP_NEW_BOX ? JS_UNINIT : op == OP_NEW_BOX_UNDEF ? JS_UNDEFINED : ((JSVarRef *)JS_PTR(locals[i]))->v;
            locals[i] = JS_MKINT(r);
            break;
        }
        case OP_GET_VREF: *sp++ = f->func->u.f.vr[RD16()]->v; break;
        case OP_PUT_VREF: f->func->u.f.vr[RD16()]->v = *--sp; break;
        case OP_SET_VREF: f->func->u.f.vr[RD16()]->v = sp[-1]; break;
        case OP_GET_VREF_CHK: {
            JSVarRef *r = f->func->u.f.vr[RD16()];
            if (JS_UNLIKELY(r->v == JS_UNINIT)) { SAVE(); js_throw_ref(ctx, "Cannot access a variable before initialization"); THROW(); }
            *sp++ = r->v;
            break;
        }
        case OP_PUT_VREF_CHK: {
            JSVarRef *r = f->func->u.f.vr[RD16()];
            if (JS_UNLIKELY(r->v == JS_UNINIT)) { SAVE(); js_throw_ref(ctx, "Cannot access a variable before initialization"); THROW(); }
            r->v = *--sp;
            break;
        }

        case OP_GET_GLOBAL: case OP_GET_GLOBAL_TYPEOF: {
            JSValue name = cp[RD32()];
            SAVE();
            JSValue v = global_get(ctx, name, op == OP_GET_GLOBAL_TYPEOF);
            CHECK(v);
            *sp++ = v;
            break;
        }
        case OP_PUT_GLOBAL: case OP_SET_GLOBAL: {
            JSValue name = cp[RD32()];
            SAVE();
            if (global_put(ctx, name, sp[-1], STRICT()) < 0) THROW();
            if (op == OP_PUT_GLOBAL) sp--;
            break;
        }
        case OP_DEFINE_GLOBAL_VAR: {
            JSValue name = cp[RD32()];
            SAVE();
            if (!js_find_prop(ctx, OBJ(ctx->global), name)) {
                int h = js_has_prop(ctx, ctx->global, name);
                if (h < 0) THROW();
                if (!h) js_add_prop(ctx, OBJ(ctx->global), name, JS_UNDEFINED, P_W | P_E);
            }
            break;
        }
        case OP_DEFINE_GLOBAL_FUNC: {
            JSValue name = cp[RD32()];
            SAVE();
            JSProp *p = js_find_prop(ctx, OBJ(ctx->global), name);
            if (p && !(p->flags & P_ACC) && ((p->flags & P_W) || (p->flags & P_C))) { p->val = sp[-1]; if (p->flags & P_C) p->flags = P_W | P_E; }
            else if (!p) js_add_prop(ctx, OBJ(ctx->global), name, sp[-1], P_W | P_E);
            else if (js_setp(ctx, ctx->global, name, sp[-1], ctx->global, false) < 0) THROW();
            sp--;
            break;
        }
        case OP_DEFINE_GLOBAL_LEX: {
            JSValue name = cp[RD32()];
            uint32_t isconst = RD8();
            SAVE();
            if (js_find_prop(ctx, OBJ(ctx->global_lex), name)) {
                char nm[80];
                js_str_to_ascii(ctx, name, nm, sizeof nm);
                js_throw_syntax(ctx, "Identifier '%s' has already been declared", nm);
                THROW();
            }
            js_add_prop(ctx, OBJ(ctx->global_lex), name, JS_UNINIT, isconst ? 0 : P_W);
            break;
        }
        case OP_INIT_GLOBAL_LEX: {
            JSValue name = cp[RD32()];
            JSProp *p = js_find_prop(ctx, OBJ(ctx->global_lex), name);
            if (p) p->val = sp[-1];
            sp--;
            break;
        }
        case OP_DELETE_GLOBAL: {
            JSValue name = cp[RD32()];
            SAVE();
            if (js_find_prop(ctx, OBJ(ctx->global_lex), name)) { *sp++ = JS_FALSE; break; }
            int r = js_delete_prop(ctx, ctx->global, name, false);
            if (r < 0) THROW();
            *sp++ = JS_BOOL(r);
            break;
        }
        case OP_CHECK_GLOBAL_DECL: pc += 5; break;

        case OP_GET_FIELD: {
            JSValue key = cp[RD32()];
            JSValue o = sp[-1];
            if (JS_IsObject(o) && OBJ(o)->h.cls == CL_OBJECT) {
                JSProp *p = js_find_prop(ctx, OBJ(o), key);
                if (p && !(p->flags & P_ACC)) { sp[-1] = p->val; break; }
            }
            SAVE();
            JSValue v = js_getp(ctx, o, key);
            CHECK(v);
            sp[-1] = v;
            break;
        }
        case OP_GET_FIELD_KEEP: {
            JSValue key = cp[RD32()];
            SAVE();
            JSValue v = js_getp(ctx, sp[-1], key);
            CHECK(v);
            *sp++ = v;
            break;
        }
        case OP_PUT_FIELD: {
            JSValue key = cp[RD32()];
            JSValue o = sp[-2];
            if (JS_IsObject(o) && OBJ(o)->h.cls == CL_OBJECT) {
                JSProp *p = js_find_prop(ctx, OBJ(o), key);
                if (p && (p->flags & (P_ACC | P_W)) == P_W) { p->val = sp[-1]; sp[-2] = sp[-1]; sp--; break; }
            }
            SAVE();
            if (js_setp(ctx, o, key, sp[-1], o, STRICT()) < 0) THROW();
            sp[-2] = sp[-1];
            sp--;
            break;
        }
        case OP_GET_ELEM: {
            SAVE();
            JSValue v = js_getv(ctx, sp[-2], sp[-1]);
            CHECK(v);
            sp[-2] = v;
            sp--;
            break;
        }
        case OP_GET_ELEM_KEEP: {
            SAVE();
            JSValue v = js_getv(ctx, sp[-2], sp[-1]);
            CHECK(v);
            sp[-1] = v;
            break;
        }
        case OP_PUT_ELEM: {
            SAVE();
            if (js_setv(ctx, sp[-3], sp[-2], sp[-1], STRICT()) < 0) THROW();
            sp[-3] = sp[-1];
            sp -= 2;
            break;
        }
        case OP_DELETE_FIELD: case OP_DELETE_ELEM: {
            SAVE();
            JSValue key;
            if (op == OP_DELETE_FIELD) key = cp[RD32()];
            else { key = js_to_key(ctx, sp[-1]); CHECK(key); sp--; }
            JSValue o = js_toobj(ctx, sp[-1]);
            CHECK(o);
            int r = js_delete_prop(ctx, o, key, STRICT());
            if (r < 0) THROW();
            sp[-1] = JS_BOOL(r);
            break;
        }
        case OP_GET_SUPER: case OP_PUT_SUPER: {
            SAVE();
            JSObject *home = f->func->u.f.home;
            JSValue thisv = op == OP_GET_SUPER ? sp[-2] : sp[-3];
            JSValue key = js_to_key(ctx, op == OP_GET_SUPER ? sp[-1] : sp[-2]);
            CHECK(key);
            if (!home) { js_throw_syntax(ctx, "'super' keyword unexpected here"); THROW(); }
            bool exc;
            JSObject *sup = js_get_proto_obj(ctx, JS_MKOBJ(home), &exc);
            if (exc) THROW();
            if (op == OP_GET_SUPER) {
                JSValue v = sup ? js_getp_recv(ctx, JS_MKOBJ(sup), key, thisv) : JS_UNDEFINED;
                CHECK(v);
                sp[-2] = v;
                sp--;
            } else {
                if (!sup) { js_throw_type(ctx, "Cannot set properties of null"); THROW(); }
                if (js_setp(ctx, JS_MKOBJ(sup), key, sp[-1], thisv, true) < 0) THROW();
                sp[-3] = sp[-1];
                sp -= 2;
            }
            break;
        }
        case OP_GET_PRIVATE: {
            SAVE();
            JSValue v = private_get(ctx, sp[-2], sp[-1]);
            CHECK(v);
            sp[-2] = v;
            sp--;
            break;
        }
        case OP_PUT_PRIVATE: {
            SAVE();
            if (private_put(ctx, sp[-3], sp[-2], sp[-1]) < 0) THROW();
            sp[-3] = sp[-1];
            sp -= 2;
            break;
        }
        case OP_DEFINE_PRIVATE: {
            SAVE();
            JSValue o = sp[-3], sym = sp[-2];
            if (!JS_IsObject(o)) { js_throw_type(ctx, "Cannot define a private field on a non-object"); THROW(); }
            if (js_find_prop(ctx, OBJ(o), sym)) { js_throw_type(ctx, "Cannot initialize a private field twice on the same object"); THROW(); }
            if (!js_add_prop(ctx, OBJ(o), sym, sp[-1], P_W)) THROW();
            sp -= 2;
            break;
        }
        case OP_PRIVATE_IN: {
            SAVE();
            JSValue sym = sp[-2], o = sp[-1];
            if (!JS_IsObject(o)) { js_throw_type(ctx, "Cannot use 'in' operator to search for a private field in a non-object"); THROW(); }
            bool found = false;
            for (JSObject *x = OBJ(o); x && !found; x = x->proto) if (js_find_prop(ctx, x, sym)) found = true;
            sp[-2] = JS_BOOL(found);
            sp--;
            break;
        }
        case OP_NEW_PRIVATE: {
            JSValue d = cp[RD32()];
            SAVE();
            JSSymbol *s = js_cell(ctx, T_SYMBOL, sizeof(JSSymbol));
            if (!s) THROW();
            s->desc = d;
            s->h.flags = SYM_PRIVATE;
            *sp++ = JS_MKSYM(s);
            break;
        }
        case OP_DEFINE_FIELD: {
            JSValue key = cp[RD32()];
            SAVE();
            if (js_create_data_prop(ctx, sp[-2], key, sp[-1]) < 0) THROW();
            sp--;
            break;
        }
        case OP_DEFINE_ELEM: {
            SAVE();
            JSValue key = js_to_key(ctx, sp[-2]);
            CHECK(key);
            if (js_create_data_prop(ctx, sp[-3], key, sp[-1]) < 0) THROW();
            sp -= 2;
            break;
        }
        case OP_DEFINE_METHOD: {
            JSValue key = cp[RD32()];
            uint32_t kind = RD8();
            SAVE();
            JSValue r = define_method(ctx, sp[-2], key, sp[-1], (int)kind);
            CHECK(r);
            sp--;
            break;
        }
        case OP_DEFINE_METHOD_ELEM: {
            uint32_t kind = RD8();
            SAVE();
            JSValue key = js_to_key(ctx, sp[-2]);
            CHECK(key);
            JSValue r = define_method(ctx, sp[-3], key, sp[-1], (int)kind);
            CHECK(r);
            sp -= 2;
            break;
        }
        case OP_SET_PROTO: {
            JSValue p = sp[-1];
            if (JS_IsObject(p) || JS_IsNull(p)) OBJ(sp[-2])->proto = JS_IsNull(p) ? NULL : OBJ(p);
            sp--;
            break;
        }
        case OP_COPY_DATA_PROPS: {
            SAVE();
            JSValue r = copy_data(ctx, sp[-2], sp[-1], JS_UNDEFINED);
            CHECK(r);
            sp--;
            break;
        }
        case OP_COPY_DATA_EXCL: {
            SAVE();
            JSValue r = copy_data(ctx, sp[-3], sp[-2], sp[-1]);
            CHECK(r);
            sp -= 2;
            break;
        }
        case OP_SET_NAME: {
            JSValue name = cp[RD32()];
            SAVE();
            js_set_func_name(ctx, sp[-1], name, NULL);
            break;
        }
        case OP_SET_NAME_ELEM: {
            SAVE();
            JSValue fn = sp[-1];
            if (js_is_cls(fn, CL_FUNCTION) && !JS_IsString(OBJ(fn)->u.f.b->name)) {
                JSValue key = js_to_key(ctx, sp[-2]);
                CHECK(key);
                sp[-2] = key;
                js_set_func_name(ctx, fn, key, NULL);
            }
            break;
        }
        case OP_SET_HOME:
            if (js_is_cls(sp[-1], CL_FUNCTION) && JS_IsObject(sp[-2])) OBJ(sp[-1])->u.f.home = OBJ(sp[-2]);
            break;

        /* ---- calls ---- */
        case OP_CALL: case OP_EVAL: {
            uint32_t argc = RD16();
            uint32_t nameidx = op == OP_CALL ? RD32() : 0xFFFFFFFFu;
            JSValue *base = sp - argc - 2;
            JSValue fn = base[0];
            if (op == OP_EVAL && fn == ctx->eval_fn) {
                SAVE();
                JSValue r = argc ? base[2] : JS_UNDEFINED;
                if (JS_IsString(r)) {
                    JSBytecode *eb = js_compile(ctx, r, js_strc(ctx, "eval"), 1, F_EVAL | (STRICT() ? F_STRICT : 0));
                    if (!eb) THROW();
                    JSValue c = js_new_closure(ctx, eb, NULL, NULL);
                    CHECK(c);
                    sp = base;
                    SAVE();
                    r = js_call_internal(ctx, c, ctx->global, 0, NULL, JS_UNDEFINED, 0);
                    CHECK(r);
                }
                sp = base;
                *sp++ = r;
                break;
            }
        do_call:
            if (JS_IsObject(fn)) {
                JSObject *fo = OBJ(fn);
                if (fo->h.cls == CL_FUNCTION) {
                    if (JS_UNLIKELY(fo->u.f.b->fflags & F_CLASSCTOR)) { SAVE(); js_throw_type(ctx, "Class constructor cannot be invoked without 'new'"); THROW(); }
                    f->pc = pc;
                    f->sp = base;
                    ctx->sp = sp;
                    if (!frame_push(ctx, base, (int)argc, fo, base[1], JS_UNDEFINED, 0)) { f->sp = sp; THROW(); }
                    LOAD();
                    break;
                }
                if (fo->h.cls == CL_BOUND && js_is_cls(fo->u.bf.target, CL_FUNCTION) && fo->u.bf.argc < 64 &&
                    sp + fo->u.bf.argc + 8 < ctx->stack_end) {
                    int ba = fo->u.bf.argc;
                    memmove(base + 2 + ba, base + 2, sizeof(JSValue) * argc);
                    for (int i = 0; i < ba; i++) base[2 + i] = fo->u.bf.args[i];
                    base[0] = fo->u.bf.target;
                    base[1] = fo->u.bf.this_val;
                    argc += (uint32_t)ba;
                    sp += ba;
                    fn = base[0];
                    goto do_call;
                }
                if (fo->h.cls == CL_NATIVE && (fo == ctx->fn_call || fo == ctx->fn_apply) && JS_IsObject(base[1]) &&
                    (OBJ(base[1])->h.cls == CL_FUNCTION || OBJ(base[1])->h.cls == CL_BOUND)) {
                    /* f.call(t, ...) / f.apply(t, array): the call itself, in this run (so it can pause) */
                    if (fo == ctx->fn_call) {
                        JSValue self = argc > 0 ? base[2] : JS_UNDEFINED;
                        if (argc > 0) { memmove(base + 2, base + 3, sizeof(JSValue) * (argc - 1)); argc--; sp--; }
                        base[0] = base[1];
                        base[1] = self;
                        fn = base[0];
                        goto do_call;
                    }
                    JSValue arr = argc > 1 ? base[3] : JS_UNDEFINED;
                    if (JS_IsNullish(arr) || js_fast_array(arr)) {
                        uint32_t n = JS_IsNullish(arr) ? 0 : OBJ(arr)->u.a.len;
                        if (base + 2 + n + 64 < ctx->stack_end) {
                            JSValue self = argc > 0 ? base[2] : JS_UNDEFINED;
                            base[0] = base[1];
                            base[1] = self;
                            if (n) {
                                JSObject *ao = OBJ(arr);
                                for (uint32_t i = 0; i < n; i++) { JSValue v = i < ao->u.a.cap ? ao->u.a.v[i] : JS_HOLE; base[2 + i] = v == JS_HOLE ? JS_UNDEFINED : v; }
                            }
                            argc = n;
                            sp = base + 2 + n;
                            fn = base[0];
                            goto do_call;
                        }
                    }
                }
                if (fo->h.cls == CL_NATIVE) {
                    SAVE();
                    ctx->can_wait = ctx->run_depth == 1 && ctx->native_depth == 0 && !ctx->no_suspend;
                    JSValue r = call_native(ctx, fo, base[1], (int)argc, base + 2, JS_UNDEFINED);
                    ctx->can_wait = false;
                    if (JS_UNLIKELY(r == JS_SUSPEND)) {
                        /* the native waits for the host (js_wait): resume after the call */
                        sp = base;
                        SAVE();
                        ctx->run_depth--;
                        return JS_SUSPEND;
                    }
                    CHECK(r);
                    sp = base;
                    *sp++ = r;
                    break;
                }
            }
            SAVE();
            if (!js_is_callable(fn)) { not_callable(ctx, fn, b, nameidx); THROW(); }
            {
                JSValue r = js_call_internal(ctx, fn, base[1], (int)argc, base + 2, JS_UNDEFINED, 0);
                CHECK(r);
                sp = base;
                *sp++ = r;
            }
            break;
        }
        case OP_CALL_SPREAD: case OP_NEW_SPREAD: {
            uint32_t nameidx = op == OP_CALL_SPREAD ? RD32() : 0xFFFFFFFFu;
            JSObject *arr = OBJ(sp[-1]);
            uint32_t n = arr->u.a.len;
            JSValue *base = sp - 3;
            if (base + 2 + n + 64 > ctx->stack_end) { SAVE(); js_throw_range(ctx, "Maximum call stack size exceeded"); THROW(); }
            sp--;
            for (uint32_t i = 0; i < n; i++) { JSValue v = i < arr->u.a.cap ? arr->u.a.v[i] : JS_HOLE; *sp++ = v == JS_HOLE ? JS_UNDEFINED : v; }
            if (op == OP_CALL_SPREAD) {
                JSValue fn = base[0];
                uint32_t argc = n;
                if (js_is_cls(fn, CL_FUNCTION) && !(OBJ(fn)->u.f.b->fflags & F_CLASSCTOR)) {
                    f->pc = pc; f->sp = base; ctx->sp = sp;
                    if (!frame_push(ctx, base, (int)argc, OBJ(fn), base[1], JS_UNDEFINED, 0)) { f->sp = sp; THROW(); }
                    LOAD();
                    break;
                }
                SAVE();
                if (!js_is_callable(fn)) { not_callable(ctx, fn, b, nameidx); THROW(); }
                JSValue r = js_call_internal(ctx, fn, base[1], (int)argc, base + 2, JS_UNDEFINED, 0);
                CHECK(r);
                sp = base;
                *sp++ = r;
                break;
            }
            /* NEW_SPREAD: [ctor nt args...] */
            {
                uint32_t argc = n;
                JSValue ctor = base[0], nt = base[1];
                goto do_new_with;
            do_new_with:
                SAVE();
                if (!js_is_ctor(ctor)) {
                    char d[64];
                    js_value_desc(ctx, ctor, d, sizeof d);
                    js_throw_type(ctx, "%s is not a constructor", d);
                    THROW();
                }
                if (js_is_cls(ctor, CL_FUNCTION)) {
                    f->pc = pc; f->sp = base; ctx->sp = sp;
                    if (!frame_push(ctx, base, (int)argc, OBJ(ctor), JS_UNDEFINED, nt, FR_CONSTRUCT)) { f->sp = sp; THROW(); }
                    LOAD();
                    break;
                }
                JSValue r = js_call_internal(ctx, ctor, JS_UNDEFINED, (int)argc, base + 2, nt, FR_CONSTRUCT);
                CHECK(r);
                sp = base;
                *sp++ = r;
            }
            break;
        }
        case OP_NEW: {
            uint32_t argc = RD16();
            JSValue *base = sp - argc - 2;
            JSValue ctor = base[0], nt = base[1];
            SAVE();
            if (!js_is_ctor(ctor)) {
                char d[64];
                js_value_desc(ctx, ctor, d, sizeof d);
                js_throw_type(ctx, "%s is not a constructor", d);
                THROW();
            }
            if (js_is_cls(ctor, CL_FUNCTION)) {
                f->pc = pc; f->sp = base; ctx->sp = sp;
                if (!frame_push(ctx, base, (int)argc, OBJ(ctor), JS_UNDEFINED, nt, FR_CONSTRUCT)) { f->sp = sp; THROW(); }
                LOAD();
                break;
            }
            JSValue r = js_call_internal(ctx, ctor, JS_UNDEFINED, (int)argc, base + 2, nt, FR_CONSTRUCT);
            CHECK(r);
            sp = base;
            *sp++ = r;
            break;
        }
        case OP_SUPER_CALL: case OP_SUPER_CALL_SPREAD: {
            uint32_t a = op == OP_SUPER_CALL ? RD16() : RD8();
            bool arrow = op == OP_SUPER_CALL ? (a & 0x8000) != 0 : (a & 1) != 0;
            uint32_t argc = op == OP_SUPER_CALL ? (a & 0x7FFF) : 0;
            JSValue active, nt;
            if (arrow) { active = sp[-1]; nt = sp[-2]; sp -= 2; }
            else { active = JS_MKOBJ(f->func); nt = f->new_target; }
            JSValue *args;
            JSValue spread_arr = JS_UNDEFINED;
            if (op == OP_SUPER_CALL_SPREAD) {
                spread_arr = sp[-1];
                sp--;
                JSObject *arr = OBJ(spread_arr);
                argc = arr->u.a.len;
                if (sp + argc + 64 > ctx->stack_end) { SAVE(); js_throw_range(ctx, "Maximum call stack size exceeded"); THROW(); }
                for (uint32_t i = 0; i < argc; i++) { JSValue v = i < arr->u.a.cap ? arr->u.a.v[i] : JS_HOLE; *sp++ = v == JS_HOLE ? JS_UNDEFINED : v; }
            }
            args = sp - argc;
            SAVE();
            JSObject *parent = OBJ(active)->proto;
            if (!parent || !js_is_ctor(JS_MKOBJ(parent))) { js_throw_type(ctx, "Super constructor is not a constructor"); THROW(); }
            JSValue r = js_construct_internal(ctx, JS_MKOBJ(parent), (int)argc, args, nt);
            CHECK(r);
            sp = args;
            *sp++ = r;
            break;
        }
        case OP_RETURN_UNDEF: *sp++ = JS_UNDEFINED; goto do_return;
        case OP_RETURN:
        do_return: {
            JSValue v = sp[-1];
            if (f->flags & FR_CONSTRUCT) {
                if (!JS_IsObject(v)) v = f->this_val;
            }
            if (f->flags & FR_GEN) {
                SAVE();
                bool absorbed;
                v = gen_finish(ctx, f, v, false, &absorbed);
            }
            JSValue *base = f->base;
            ctx->nframes--;
            if (ctx->nframes == stop) { ctx->sp = base; ctx->run_depth--; return v; }
            LOAD();
            sp = base;
            *sp++ = v;
            break;
        }

        /* ---- control ---- */
        case OP_JMP: { int32_t o = (int32_t)RD32(); pc += o; break; }
        case OP_JMP_FALSE: { int32_t o = (int32_t)RD32(); JSValue v = *--sp; if (!js_truthy(v)) pc += o; break; }
        case OP_JMP_TRUE: { int32_t o = (int32_t)RD32(); JSValue v = *--sp; if (js_truthy(v)) pc += o; break; }
        case OP_JMP_FALSE_KEEP: { int32_t o = (int32_t)RD32(); if (!js_truthy(sp[-1])) pc += o; else sp--; break; }
        case OP_JMP_TRUE_KEEP: { int32_t o = (int32_t)RD32(); if (js_truthy(sp[-1])) pc += o; else sp--; break; }
        case OP_JMP_NOTNULLISH_KEEP: { int32_t o = (int32_t)RD32(); if (!JS_IsNullish(sp[-1])) pc += o; else sp--; break; }
        case OP_JMP_NULLISH_UNDEF: { int32_t o = (int32_t)RD32(); if (JS_IsNullish(sp[-1])) { sp[-1] = JS_UNDEFINED; pc += o; } break; }
        case OP_JMP_NULLISH_UNDEF2: { int32_t o = (int32_t)RD32(); if (JS_IsNullish(sp[-2])) { sp -= 2; *sp++ = JS_UNDEFINED; pc += o; } break; }
        case OP_JMP_UNDEF_KEEP: { int32_t o = (int32_t)RD32(); if (JS_IsUndefined(sp[-1])) pc += o; else sp--; break; }
        case OP_THROW:
            SAVE();
            ctx->exception = sp[-1];
            ctx->has_exc = true;
            sp--;
            if (JS_IsObject(ctx->exception) && OBJ(ctx->exception)->h.cls == CL_ERROR && !js_find_prop(ctx, OBJ(ctx->exception), ctx->a[A_stack])) {
                JSValue e = ctx->exception;
                js_error_add_stack(ctx, e);
                ctx->exception = e;
                ctx->has_exc = true;
            }
            THROW();
        case OP_TRY: {
            int32_t o = (int32_t)RD32();
            uint32_t target = (uint32_t)((pc + o) - b->code);
            *sp++ = JS_MKMARK(MARK_CATCH | target);
            break;
        }
        case OP_END_TRY: sp--; break;
        case OP_GOSUB: {
            int32_t o = (int32_t)RD32();
            *sp++ = JS_MKMARK((uint32_t)(pc - b->code));
            pc += o;
            break;
        }
        case OP_RET: {
            JSValue m = *--sp;
            pc = b->code + ((uint32_t)m & 0x7FFFFFFFu);
            break;
        }
        case OP_THROW_REF: { JSValue m = cp[RD32()]; SAVE(); char t[120]; js_str_to_ascii(ctx, m, t, sizeof t); js_throw_ref(ctx, "%s", t); THROW(); }
        case OP_THROW_CONST: { pc += 4; SAVE(); js_throw_type(ctx, "Assignment to constant variable."); THROW(); }
        case OP_THROW_TYPE: { JSValue m = cp[RD32()]; SAVE(); char t[120]; js_str_to_ascii(ctx, m, t, sizeof t); js_throw_type(ctx, "%s", t); THROW(); }
        case OP_THROW_SYNTAX: { JSValue m = cp[RD32()]; SAVE(); char t[120]; js_str_to_ascii(ctx, m, t, sizeof t); js_throw_syntax(ctx, "%s", t); THROW(); }

        /* ---- operators ---- */
        case OP_ADD: {
            JSValue x = sp[-2], y = sp[-1];
            if (JS_IsNumber(x) && JS_IsNumber(y)) { sp[-2] = js_mknum(js_num(x) + js_num(y)); sp--; break; }
            SAVE();
            JSValue r = js_add(ctx, x, y);
            CHECK(r);
            sp[-2] = r;
            sp--;
            break;
        }
        case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW: {
            JSValue x = sp[-2], y = sp[-1];
            double a, c;
            if (JS_IsNumber(x) && JS_IsNumber(y)) { a = js_num(x); c = js_num(y); }
            else {
                SAVE();
                bool e = false;
                a = js_tonum(ctx, x, &e);
                if (e) THROW();
                c = js_tonum(ctx, y, &e);
                if (e) THROW();
            }
            double r;
            switch (op) {
            case OP_SUB: r = a - c; break;
            case OP_MUL: r = a * c; break;
            case OP_DIV: r = a / c; break;
            case OP_MOD: r = (isinf(c) && isfinite(a)) ? a : fmod(a, c); break;
            default:
                if (c != c || ((a == 1 || a == -1) && isinf(c))) r = NAN;
                else r = pow(a, c);
                break;
            }
            sp[-2] = js_mknum(r);
            sp--;
            break;
        }
        case OP_NEG: case OP_PLUS: case OP_INC: case OP_DEC: case OP_TO_NUMERIC: {
            JSValue x = sp[-1];
            double a;
            if (JS_IsNumber(x)) a = js_num(x);
            else {
                SAVE();
                bool e = false;
                a = js_tonum(ctx, x, &e);
                if (e) THROW();
            }
            if (op == OP_NEG) a = -a;
            else if (op == OP_INC) a += 1;
            else if (op == OP_DEC) a -= 1;
            sp[-1] = js_mknum(a);
            break;
        }
        case OP_NOT: sp[-1] = JS_BOOL(!js_truthy(sp[-1])); break;
        case OP_BNOT: {
            int32_t i;
            SAVE();
            if (js_toint32_e(ctx, sp[-1], &i) < 0) THROW();
            sp[-1] = js_mknum(~i);
            break;
        }
        case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SAR: case OP_SHR: {
            int32_t x, y;
            SAVE();
            if (js_toint32_e(ctx, sp[-2], &x) < 0 || js_toint32_e(ctx, sp[-1], &y) < 0) THROW();
            double r;
            switch (op) {
            case OP_BAND: r = x & y; break;
            case OP_BOR: r = x | y; break;
            case OP_BXOR: r = x ^ y; break;
            case OP_SHL: r = (int32_t)((uint32_t)x << (y & 31)); break;
            case OP_SAR: r = x >> (y & 31); break;
            default: r = (uint32_t)x >> (y & 31); break;
            }
            sp[-2] = js_mknum(r);
            sp--;
            break;
        }
        case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
            JSValue x = sp[-2], y = sp[-1];
            int r;
            if (JS_IsNumber(x) && JS_IsNumber(y)) {
                double a = js_num(x), c = js_num(y);
                r = op == OP_LT ? a < c : op == OP_LE ? a <= c : op == OP_GT ? a > c : a >= c;
            } else {
                SAVE();
                int t;
                if (op == OP_LT) { t = js_less(ctx, x, y, true); r = t == 1; }
                else if (op == OP_GT) { t = js_less(ctx, y, x, false); r = t == 1; }
                else if (op == OP_LE) { t = js_less(ctx, y, x, false); r = t == 0; }
                else { t = js_less(ctx, x, y, true); r = t == 0; }
                if (t < 0) THROW();
            }
            sp[-2] = JS_BOOL(r);
            sp--;
            break;
        }
        case OP_EQ: case OP_NE: {
            SAVE();
            int r = js_loose_eq(ctx, sp[-2], sp[-1]);
            if (r < 0) THROW();
            sp[-2] = JS_BOOL(op == OP_EQ ? r : !r);
            sp--;
            break;
        }
        case OP_SEQ: case OP_SNE: {
            bool r = js_strict_eq(ctx, sp[-2], sp[-1]);
            sp[-2] = JS_BOOL(op == OP_SEQ ? r : !r);
            sp--;
            break;
        }
        case OP_TYPEOF: sp[-1] = js_typeof(ctx, sp[-1]); break;
        case OP_IN: {
            SAVE();
            JSValue o = sp[-1];
            if (!JS_IsObject(o)) { js_throw_type(ctx, "Cannot use 'in' operator to search for a key in a non-object"); THROW(); }
            JSValue key = js_to_key(ctx, sp[-2]);
            CHECK(key);
            int r = js_has_prop(ctx, o, key);
            if (r < 0) THROW();
            sp[-2] = JS_BOOL(r);
            sp--;
            break;
        }
        case OP_INSTANCEOF: {
            SAVE();
            int r = js_instanceof(ctx, sp[-2], sp[-1]);
            if (r < 0) THROW();
            sp[-2] = JS_BOOL(r);
            sp--;
            break;
        }
        case OP_TO_STRING: {
            if (JS_IsString(sp[-1])) break;
            SAVE();
            JSValue s = js_tostr(ctx, sp[-1]);
            CHECK(s);
            sp[-1] = s;
            break;
        }
        case OP_TO_KEY: {
            SAVE();
            JSValue k = js_to_key(ctx, sp[-1]);
            CHECK(k);
            sp[-1] = k;
            break;
        }
        case OP_IS_UNDEF: sp[-1] = JS_BOOL(JS_IsUndefined(sp[-1])); break;
        case OP_IS_NULLISH: sp[-1] = JS_BOOL(JS_IsNullish(sp[-1])); break;

        /* ---- iteration ---- */
        case OP_FOR_IN_START: {
            SAVE();
            JSValue it = forin_start(ctx, sp[-1]);
            CHECK(it);
            sp[-1] = it;
            break;
        }
        case OP_FOR_IN_NEXT: {
            int32_t o = (int32_t)RD32();
            SAVE();
            JSValue k;
            int r = forin_next(ctx, sp[-1], &k);
            if (r < 0) THROW();
            if (!r) { pc += o; break; }
            *sp++ = k;
            break;
        }
        case OP_FOR_OF_START: {
            SAVE();
            JSValue obj = sp[-1];
            if (array_iter_pristine(ctx, obj)) { *sp++ = JS_MKMARK(ITER_FAST); break; }
            JSValue nx;
            JSValue it = js_get_iterator(ctx, obj, &nx, false);
            CHECK(it);
            sp[-1] = it;
            *sp++ = nx;
            break;
        }
        case OP_FOR_AWAIT_START: {
            SAVE();
            JSValue nx;
            JSValue it = js_get_iterator(ctx, sp[-1], &nx, true);
            CHECK(it);
            sp[-1] = it;
            *sp++ = nx;
            break;
        }
        case OP_FOR_OF_NEXT: {
            int32_t o = (int32_t)RD32();
            SAVE();
            bool done;
            JSValue v = iter_step_value(ctx, sp - 2, &done);
            CHECK(v);
            if (done) { pc += o; break; }
            *sp++ = v;
            break;
        }
        case OP_ITER_NEXT_RAW: {
            SAVE();
            JSValue r = js_callv(ctx, sp[-1], sp[-2], 0, NULL);
            CHECK(r);
            *sp++ = r;
            break;
        }
        case OP_ITER_RESULT: {
            int32_t o = (int32_t)RD32();
            SAVE();
            JSValue r = sp[-1];
            if (!JS_IsObject(r)) { js_throw_type(ctx, "Iterator result is not an object"); THROW(); }
            JSValue d = js_getp(ctx, r, ctx->a[A_done]);
            CHECK(d);
            if (js_truthy(d)) { sp--; pc += o; break; }
            JSValue v = js_getp(ctx, r, ctx->a[A_value]);
            CHECK(v);
            sp[-1] = v;
            break;
        }
        case OP_ITER_CLOSE: case OP_ITER_CLOSE_EXC: {
            SAVE();
            if (op == OP_ITER_CLOSE_EXC) {
                JSValue saved_exc = ctx->exception;
                bool had = ctx->has_exc;
                iter_close_v(ctx, sp[-2], sp[-1], true);
                if (ctx->killed) THROW();
                ctx->exception = saved_exc;
                ctx->has_exc = had;
            } else if (iter_close_v(ctx, sp[-2], sp[-1], false) < 0) THROW();
            sp -= 2;
            break;
        }
        case OP_ITER_STEP: {
            SAVE();
            if (sp[-1] == JS_TRUE) { *sp++ = JS_UNDEFINED; break; }
            bool done;
            JSValue v = iter_step_value(ctx, sp - 3, &done);
            if (JS_IsException(v)) { sp[-1] = JS_TRUE; THROW(); }
            if (done) { sp[-1] = JS_TRUE; v = JS_UNDEFINED; }
            *sp++ = v;
            break;
        }
        case OP_ITER_REST: {
            SAVE();
            JSValue arr = js_array_new(ctx, 0);
            CHECK(arr);
            *sp++ = arr;
            SAVE();
            if (sp[-2] != JS_TRUE) {
                for (;;) {
                    bool done;
                    JSValue v = iter_step_value(ctx, sp - 4, &done);
                    if (JS_IsException(v)) { sp[-2] = JS_TRUE; THROW(); }
                    if (done) break;
                    if (js_array_push_v(ctx, arr, v) < 0) THROW();
                }
                sp[-2] = JS_TRUE;
            }
            break;
        }
        case OP_ITER_END: {
            SAVE();
            if (sp[-1] != JS_TRUE && iter_close_v(ctx, sp[-3], sp[-2], false) < 0) THROW();
            sp -= 3;
            break;
        }
        case OP_REQUIRE_COERCIBLE:
            if (JS_IsNullish(sp[-1])) {
                SAVE();
                js_throw_type(ctx, "Cannot destructure '%s' as it is %s.", JS_IsNull(sp[-1]) ? "null" : "undefined", JS_IsNull(sp[-1]) ? "null" : "undefined");
                THROW();
            }
            break;

        /* ---- generators / async ---- */
        case OP_INITIAL_YIELD: {
            SAVE();
            JSBytecode *fb = f->b;
            bool async = (fb->fflags & F_ASYNC) != 0;
            JSValue pv = js_getp(ctx, JS_MKOBJ(f->func), ctx->a[A_prototype]);
            CHECK(pv);
            JSObject *go = js_obj_alloc(ctx, async ? CL_ASYNC_GEN : CL_GENERATOR,
                                        JS_IsObject(pv) ? OBJ(pv) : (async ? ctx->proto_async_gen : ctx->proto_generator));
            if (!go) THROW();
            JSGen *g = new_gen(ctx, go, async ? 2 : 0);
            if (!g) THROW();
            f->gen = go;
            if (!gen_save(ctx, f, sp, pc)) THROW();
            g->state = GS_START;
            if (async) {
                JSValue q = js_array_new(ctx, 0);
                CHECK(q);
                g->queue = q;
            }
            JSValue *base = f->base;
            ctx->nframes--;
            JSValue v = JS_MKOBJ(go);
            if (ctx->nframes == stop) { ctx->sp = base; ctx->run_depth--; return v; }
            LOAD();
            sp = base;
            *sp++ = v;
            break;
        }
        case OP_YIELD: {
            int32_t o = (int32_t)RD32();
            JSValue v = *--sp;
            SAVE();
            JSGen *g = f->gen->u.p;
            if (!gen_save(ctx, f, sp, pc)) THROW();
            g->state = GS_SUSPENDED_YIELD;
            g->ret_pc = o ? (uint32_t)((pc + o) - b->code) : 0;
            g->in_delegate = false;
            ctx->gen_raw = false;
            ctx->gen_done = false;
            JSValue *base = f->base;
            ctx->nframes--;
            ctx->sp = base;
            if (ctx->nframes == stop) { ctx->run_depth--; return v; }
            /* (generator frames are always entered from C) */
            LOAD();
            sp = base;
            *sp++ = v;
            break;
        }
        case OP_YIELD_STAR: {
            const uint8_t *start = pc - 1;
            int32_t o = (int32_t)RD32();
            SAVE();
            JSValue iter = sp[-3], recv = sp[-1];
            int mode = f->resume;
            f->resume = GR_NEXT;
            JSValue res;
            if (mode == GR_NEXT) res = js_callv(ctx, sp[-2], iter, 1, &recv);
            else {
                JSValue m = js_get_method(ctx, iter, ctx->a[mode == GR_THROW ? A_throw : A_return]);
                CHECK(m);
                if (JS_IsUndefined(m)) {
                    if (mode == GR_THROW) {
                        js_iter_close(ctx, iter, true);
                        js_throw_type(ctx, "The iterator does not provide a 'throw' method");
                        THROW();
                    }
                    /* return with the value */
                    sp -= 3;
                    *sp++ = recv;
                    if (o) { pc += o; break; }
                    goto do_return;
                }
                res = js_callv(ctx, m, iter, 1, &recv);
            }
            CHECK(res);
            if (!JS_IsObject(res)) { js_throw_type(ctx, "Iterator result is not an object"); THROW(); }
            JSValue d = js_getp(ctx, res, ctx->a[A_done]);
            CHECK(d);
            if (js_truthy(d)) {
                JSValue v = js_getp(ctx, res, ctx->a[A_value]);
                CHECK(v);
                sp -= 3;
                *sp++ = v;
                if (mode == GR_RETURN) { if (o) { pc += o; break; } goto do_return; }
                break;
            }
            /* yield the inner result as it is; resume here with the next value */
            sp--;
            JSGen *g = f->gen->u.p;
            if (!gen_save(ctx, f, sp, start)) THROW();
            g->state = GS_SUSPENDED_YIELD;
            g->in_delegate = true;
            g->ret_pc = 0;
            ctx->gen_raw = true;
            ctx->gen_done = false;
            JSValue *base = f->base;
            ctx->nframes--;
            ctx->sp = base;
            if (ctx->nframes == stop) { ctx->run_depth--; return res; }
            LOAD();
            sp = base;
            *sp++ = res;
            break;
        }
        case OP_ASYNC_YIELD_STAR: {
            pc += 4;
            SAVE();
            js_throw_syntax(ctx, "yield* in async generators is not supported");
            THROW();
        }
        case OP_AWAIT: {
            JSValue v = *--sp;
            SAVE();
            JSObject *go = f->gen;
            if (!go) { js_throw_syntax(ctx, "await is only valid in async functions"); THROW(); }
            JSValue p = js_promise_resolve_value(ctx, v);
            CHECK(p);
            JSGen *g = go->u.p;
            if (!gen_save(ctx, f, sp, pc)) THROW();
            g->state = GS_AWAIT;
            js_promise_then_native(ctx, p, NULL, NULL, JS_MKOBJ(go));
            JSValue *base = f->base;
            bool resumed = (f->flags & FR_RESUMED) != 0;
            JSValue rv = g->kind == 1 && !resumed ? g->promise : JS_UNDEFINED;
            if (g->kind == 2) ctx->gen_awaiting = true;
            ctx->nframes--;
            ctx->sp = base;
            if (ctx->nframes == stop) { ctx->run_depth--; return rv; }
            LOAD();
            sp = base;
            *sp++ = rv;
            break;
        }

        /* ---- classes ---- */
        case OP_CLASS: {
            uint32_t fl = RD8();
            SAVE();
            JSValue proto = JS_UNDEFINED;
            JSValue r = class_create(ctx, sp[-2], sp[-1], fl & 1, &proto);
            CHECK(r);
            sp[-2] = r;
            sp[-1] = proto;
            break;
        }
        case OP_SET_FIELDS:
            if (js_is_cls(sp[-2], CL_FUNCTION) && JS_IsObject(sp[-1])) OBJ(sp[-2])->u.f.fields = OBJ(sp[-1]);
            sp--;
            break;
        case OP_RUN_FIELDS: {
            JSValue fn = sp[-1], thisv = sp[-2];
            sp--;
            if (js_is_cls(fn, CL_FUNCTION) && OBJ(fn)->u.f.fields) {
                SAVE();
                JSValue r = js_callv(ctx, JS_MKOBJ(OBJ(fn)->u.f.fields), thisv, 0, NULL);
                CHECK(r);
            }
            break;
        }
        case OP_CHECK_THIS_INIT:
            if (sp[-1] != JS_UNINIT) { SAVE(); js_throw_ref(ctx, "Super constructor may only be called once"); THROW(); }
            sp--;
            break;
        case OP_CHECK_DERIVED_RETURN: {
            JSValue v = sp[-2], t = sp[-1];
            sp--;
            if (JS_IsObject(v)) break;
            SAVE();
            if (!JS_IsUndefined(v)) { js_throw_type(ctx, "Derived constructors may only return object or undefined"); THROW(); }
            if (t == JS_UNINIT) { js_throw_ref(ctx, "Must call super constructor in derived class before accessing 'this' or returning from derived constructor"); THROW(); }
            sp[-1] = t;
            break;
        }

        /* ---- misc ---- */
        case OP_ARGUMENTS: {
            SAVE();
            JSValue a = arguments_obj(ctx, f);
            CHECK(a);
            *sp++ = a;
            break;
        }
        case OP_REST: {
            uint32_t from = RD16();
            SAVE();
            uint32_t n = (int)from < f->argc ? (uint32_t)f->argc - from : 0;
            JSValue a = js_array_from_list(ctx, f->argv + from, n);
            CHECK(a);
            *sp++ = a;
            break;
        }
        case OP_IMPORT_CALL: {
            SAVE();
            if (js_is_callable(ctx->import_hook)) {
                /* the host loads it: hook(specifier, the importing script's name) -> promise */
                JSValue args[2] = { sp[-1], b->srcname };
                JSValue r = js_callv(ctx, ctx->import_hook, JS_UNDEFINED, 2, args);
                CHECK(r);
                sp[-1] = r;
                break;
            }
            JSValue res, rej;
            JSValue p = js_promise_new(ctx, &res, &rej);
            CHECK(p);
            JSValue e = js_new_error(ctx, JS_TYPE_ERROR, "Dynamic import() is not supported");
            js_callv(ctx, rej, JS_UNDEFINED, 1, &e);
            sp[-1] = p;
            break;
        }
        case OP_CHECK_OBJ:
            if (!JS_IsObject(sp[-1])) { SAVE(); js_throw_type(ctx, "Iterator result is not an object"); THROW(); }
            break;
        default:
            SAVE();
            js_throw_error(ctx, JS_ERROR, "internal error: bad opcode %d", op);
            THROW();
        }
        continue;

    exception:
        /* unwind to the nearest catch marker, frame by frame */
        for (;;) {
            if (!ctx->uncatchable) {
                JSValue *opbase = f->locals + b->nlocals;
                JSValue *p = sp - 1;
                for (; p >= opbase; p--) if (JS_IsMarker(*p) && ((uint32_t)*p & MARK_CATCH)) break;
                if (p >= opbase) {
                    uint32_t target = (uint32_t)*p & 0x7FFFFFFFu;
                    sp = p;
                    *sp++ = js_take_exception(ctx);
                    pc = b->code + target;
                    break;
                }
            }
            JSValue *base = f->base;
            bool absorbed = false;
            JSValue v = JS_EXCEPTION;
            if ((f->flags & FR_GEN) && !ctx->uncatchable) {
                f->sp = sp;
                v = gen_finish(ctx, f, JS_UNDEFINED, true, &absorbed);
            } else if (f->flags & FR_GEN) {
                JSGen *g = f->gen->u.p;
                g->state = GS_DONE;
            }
            ctx->nframes--;
            if (ctx->nframes == stop) {
                ctx->sp = base;
                ctx->run_depth--;
                return absorbed ? v : JS_EXCEPTION;
            }
            LOAD();
            if (absorbed) {
                sp = base;
                *sp++ = v;
                break;
            }
        }
    }
}

/* ==== generators (sync) ========================================================================================= */
JSValue js_gen_resume(JSContext *ctx, JSObject *go, JSValue v, int mode, bool *done) {
    JSGen *g = go->u.p;
    *done = true;
    if (!g) return js_throw_type(ctx, "not a generator");
    if (g->state == GS_RUNNING || g->state == GS_AWAIT) return js_throw_type(ctx, "Generator is already running");
    if (g->state == GS_DONE || (g->state == GS_START && mode != GR_NEXT)) {
        g->state = GS_DONE;
        if (mode == GR_THROW) return js_throw(ctx, v);
        return mode == GR_RETURN ? v : JS_UNDEFINED;
    }
    int stop = ctx->nframes;
    JSValue *saved_sp = ctx->sp;
    bool was_start = g->state == GS_START;
    bool deleg = g->in_delegate;
    JSFrame *f = gen_restore(ctx, go, FR_BOUNDARY);
    if (!f) return JS_EXCEPTION;
    if (!was_start) {
        if (deleg) { *f->sp++ = v; f->resume = (uint8_t)mode; }
        else if (mode == GR_NEXT) *f->sp++ = v;
        else if (mode == GR_THROW) { js_throw(ctx, v); ctx->resume_mode = GR_THROW; }
        else {
            *f->sp++ = v;
            if (g->ret_pc) f->pc = g->b->code + g->ret_pc;
            else ctx->resume_mode = GR_RETURN;
        }
    }
    ctx->sp = f->sp;
    ctx->gen_done = false;
    ctx->gen_raw = false;
    ctx->native_depth++;
    JSValue r = run(ctx, stop);
    ctx->native_depth--;
    after_nested(ctx);
    ctx->sp = saved_sp;
    if (JS_IsException(r)) { g->state = GS_DONE; return r; }
    *done = ctx->gen_done;
    return r;
}

/* ==== tasks and jobs ============================================================================================== */
static JSJob *job_new(JSContext *ctx, int kind, JSValue fn) {
    JSJob *j = calloc(1, sizeof *j);
    if (!j) { js_throw_oom(ctx); return NULL; }
    j->kind = kind;
    j->fn = fn;
    for (int i = 0; i < 4; i++) j->args[i] = JS_UNDEFINED;
    return j;
}

static void job_push(JSContext *ctx, JSJob *j) {
    j->next = NULL;
    if (ctx->jobs_tail) ctx->jobs_tail->next = j; else ctx->jobs = j;
    ctx->jobs_tail = j;
}

void js_enqueue(JSContext *ctx, JSValue fn, int argc, const JSValue *argv) {
    JSJob *j = job_new(ctx, JOB_CALL, fn);
    if (!j) return;
    for (int i = 0; i < argc && i < 4; i++) j->args[i] = argv[i];
    j->argc = argc > 4 ? 4 : argc;
    job_push(ctx, j);
}

void js_enqueue_job(JSContext *ctx, JSValue fn, int argc, const JSValue *argv) { js_enqueue(ctx, fn, argc, argv); }

void js_enqueue_reaction(JSContext *ctx, JSValue handler, JSValue arg, JSValue resolve, JSValue reject, bool rejected) {
    JSJob *j = job_new(ctx, JOB_REACTION, handler);
    if (!j) return;
    j->args[0] = arg;
    j->args[1] = resolve;
    j->args[2] = reject;
    j->args[3] = JS_BOOL(rejected);
    j->argc = 4;
    job_push(ctx, j);
}

void js_enqueue_thenable(JSContext *ctx, JSValue then, JSValue thenable, JSValue resolve, JSValue reject) {
    JSJob *j = job_new(ctx, JOB_THENABLE, then);
    if (!j) return;
    j->args[0] = thenable;
    j->args[1] = resolve;
    j->args[2] = reject;
    j->argc = 3;
    job_push(ctx, j);
}

bool js_has_jobs(JSContext *ctx) { return ctx->jobs != NULL || ctx->cur_job != NULL; }

static void report(JSContext *ctx, JSValue e, const char *prefix) {
    char *t = js_error_text(ctx, e);
    if (ctx->host.log) {
        char buf[600];
        snprintf(buf, sizeof buf, "%s%s", prefix, t ? t : "exception");
        ctx->host.log(ctx->host.ud, JS_LOG_ERROR, buf);
    }
    free(t);
}

/* push a frame for a bytecode function at the bottom of the frame stack */
static bool bottom_call(JSContext *ctx, JSValue fn, JSValue this_val, int argc, const JSValue *argv) {
    JSValue *base = ctx->stack;
    if (argc > 1000) argc = 1000;
    base[0] = fn;
    base[1] = this_val;
    for (int i = 0; i < argc; i++) base[2 + i] = argv[i];
    ctx->sp = base + 2 + argc;
    JSFrame *f = frame_push(ctx, base, argc, OBJ(fn), this_val, JS_UNDEFINED, 0);
    if (!f) { ctx->sp = ctx->stack; return false; }
    return true;
}

static void async_resume(JSContext *ctx, JSObject *go, JSValue v, bool threw);

/* settle the promise a reaction job was for */
static void settle(JSContext *ctx, JSJob *j, JSValue r, bool threw) {
    JSValue fn = threw ? j->args[2] : j->args[1];
    if (JS_IsUndefined(fn)) return;
    JSValue x = js_callv(ctx, fn, JS_UNDEFINED, 1, &r);
    if (JS_IsException(x)) js_take_exception(ctx);
}

/* start a job: true = frames were pushed (run them), false = done already */
static bool start_job(JSContext *ctx, JSJob *j) {
    JSValue r;
    switch (j->kind) {
    case JOB_TASK: case JOB_SCRIPT: {
        JSValue args = j->args[1];
        int argc = js_is_cls(args, CL_ARRAY) ? (int)OBJ(args)->u.a.len : 0;
        JSValue *argv = argc ? OBJ(args)->u.a.v : NULL;
        if (js_is_cls(j->fn, CL_FUNCTION) && !(OBJ(j->fn)->u.f.b->fflags & F_CLASSCTOR)) {
            if (bottom_call(ctx, j->fn, j->args[0], argc, argv)) return true;
            r = JS_EXCEPTION;
        } else r = js_callv(ctx, j->fn, j->args[0], argc, argv);
        if (JS_IsException(r)) { ctx->task_threw = true; if (!ctx->killed) report(ctx, js_take_exception(ctx), "Uncaught "); }
        return false;
    }
    case JOB_REACTION: {
        JSValue h = j->fn;
        bool rejected = j->args[3] == JS_TRUE;
        if (h == ctx->async_ok || h == ctx->async_fail) {
            async_resume(ctx, OBJ(j->args[1]), j->args[0], h == ctx->async_fail);
            return ctx->nframes > 0;
        }
        if (JS_IsUndefined(h)) { settle(ctx, j, j->args[0], rejected); return false; }
        if (js_is_cls(h, CL_FUNCTION) && !(OBJ(h)->u.f.b->fflags & F_CLASSCTOR)) {
            if (bottom_call(ctx, h, JS_UNDEFINED, 1, &j->args[0])) return true;
            settle(ctx, j, js_take_exception(ctx), true);
            return false;
        }
        r = js_callv(ctx, h, JS_UNDEFINED, 1, &j->args[0]);
        if (JS_IsException(r)) settle(ctx, j, js_take_exception(ctx), true);
        else settle(ctx, j, r, false);
        return false;
    }
    case JOB_THENABLE: {
        JSValue args[2] = { j->args[1], j->args[2] };
        r = js_callv(ctx, j->fn, j->args[0], 2, args);
        if (JS_IsException(r)) {
            JSValue e = js_take_exception(ctx);
            JSValue x = js_callv(ctx, j->args[2], JS_UNDEFINED, 1, &e);
            if (JS_IsException(x)) js_take_exception(ctx);
        }
        return false;
    }
    default:
        r = js_callv(ctx, j->fn, JS_UNDEFINED, j->argc, j->args);
        if (JS_IsException(r) && !ctx->killed) report(ctx, js_take_exception(ctx), "Uncaught ");
        return false;
    }
}

/* the frames of the current job finished with r */
static void complete_job(JSContext *ctx, JSJob *j, JSValue r, bool threw) {
    switch (j->kind) {
    case JOB_TASK: case JOB_SCRIPT:
        if (threw) { ctx->task_threw = true; if (!ctx->killed) report(ctx, r, "Uncaught "); }
        else ctx->resume_val = r;
        break;
    case JOB_REACTION:
        if (j->fn == ctx->async_ok || j->fn == ctx->async_fail) break;
        settle(ctx, j, r, threw);
        break;
    default:
        if (threw && !ctx->killed) report(ctx, r, "Uncaught ");
        break;
    }
}

static void reset_vm(JSContext *ctx) {
    ctx->nframes = 0;
    ctx->sp = ctx->stack;
    free(ctx->cur_job);
    ctx->cur_job = NULL;
    while (ctx->jobs) { JSJob *n = ctx->jobs->next; free(ctx->jobs); ctx->jobs = n; }
    ctx->jobs_tail = NULL;
    ctx->has_exc = false;
    ctx->exception = JS_UNDEFINED;
    ctx->uncatchable = false;
    ctx->resume_mode = 0;
    ctx->drive_pending = false;
    ctx->host_wait = ctx->host_push = ctx->can_wait = false;
    ctx->host_val = JS_UNDEFINED;
}

int js_drive(JSContext *ctx) {
    JS_GC_ENTER(ctx);
    ctx->slice_start_d = mono(ctx);
    ctx->slice_over = false;
    ctx->budget = BUDGET;
    int status = JS_DONE;
    for (;;) {
        if (ctx->cur_job && ctx->nframes > 0) {
            JSValue r = run(ctx, 0);
            if (r == JS_SUSPEND) { status = JS_SUSPENDED; break; }
            JSJob *j = ctx->cur_job;
            ctx->cur_job = NULL;
            ctx->sp = ctx->stack;
            if (ctx->killed) { free(j); status = JS_KILLED; break; }
            bool threw = JS_IsException(r);
            JSValue v = threw ? js_take_exception(ctx) : r;
            complete_job(ctx, j, v, threw);
            free(j);
        } else if (ctx->cur_job) {
            free(ctx->cur_job);
            ctx->cur_job = NULL;
        }
        if (ctx->killed) { status = JS_KILLED; break; }
        if (mono(ctx) - ctx->slice_start_d >= ctx->host.slice_ms && !ctx->no_suspend && ctx->jobs) { status = JS_SUSPENDED; break; }
        JSJob *j = ctx->jobs;
        if (!j) break;
        ctx->jobs = j->next;
        if (!ctx->jobs) ctx->jobs_tail = NULL;
        ctx->cur_job = j;
        if (!start_job(ctx, j)) {
            ctx->cur_job = NULL;
            free(j);
            ctx->sp = ctx->stack;
        }
    }
    if (status == JS_KILLED) {
        reset_vm(ctx);
        ctx->killed = false;
    }
    ctx->drive_pending = status == JS_SUSPENDED;
    if (status != JS_SUSPENDED) js_promise_report_unhandled(ctx);
    JS_GC_LEAVE(ctx);
    return status;
}

static int task_result(JSContext *ctx, int st) {
    if (st == JS_SUSPENDED || st == JS_KILLED) return st;
    return ctx->task_threw ? JS_THROWN : JS_DONE;
}

int js_eval_task(JSContext *ctx, const char *utf8, size_t len, const char *name, int line) {
    if (ctx->drive_pending || ctx->cur_job) return JS_SUSPENDED;
    JS_GC_ENTER(ctx);
    ctx->task_threw = false;
    ctx->resume_val = JS_UNDEFINED;
    ctx->task_t0 = mono(ctx);
    JSValue src = js_str_utf8(ctx, utf8, len);
    JSValue nm = js_str_utf8(ctx, name ? name : "script", strlen(name ? name : "script"));
    JSBytecode *bc = JS_IsException(src) || JS_IsException(nm) ? NULL : js_compile(ctx, src, nm, line, 0);
    if (!bc) {
        report(ctx, js_take_exception(ctx), "Uncaught ");
        JS_GC_LEAVE(ctx);
        return JS_THROWN;
    }
    JSValue fn = js_new_closure(ctx, bc, NULL, NULL);
    JSJob *j = JS_IsException(fn) ? NULL : job_new(ctx, JOB_SCRIPT, fn);
    if (!j) { JS_GC_LEAVE(ctx); return JS_THROWN; }
    j->args[0] = ctx->global;
    /* the task runs before the jobs already queued */
    j->next = ctx->jobs;
    ctx->jobs = j;
    if (!ctx->jobs_tail) ctx->jobs_tail = j;
    int st = js_drive(ctx);
    JS_GC_LEAVE(ctx);
    return task_result(ctx, st);
}

int js_call_task(JSContext *ctx, JSValue fn, JSValue this_val, int argc, const JSValue *argv) {
    if (ctx->drive_pending || ctx->cur_job) return JS_SUSPENDED;
    JS_GC_ENTER(ctx);
    ctx->task_threw = false;
    ctx->resume_val = JS_UNDEFINED;
    ctx->task_t0 = mono(ctx);
    JSJob *j = job_new(ctx, JOB_TASK, fn);
    if (!j) { JS_GC_LEAVE(ctx); return JS_THROWN; }
    j->args[0] = this_val;
    if (argc > 0) {
        JSValue a = js_array_from_list(ctx, argv, (uint32_t)argc);
        if (JS_IsException(a)) { free(j); JS_GC_LEAVE(ctx); return JS_THROWN; }
        j->args[1] = a;
    }
    j->next = ctx->jobs;
    ctx->jobs = j;
    if (!ctx->jobs_tail) ctx->jobs_tail = j;
    int st = js_drive(ctx);
    JS_GC_LEAVE(ctx);
    return task_result(ctx, st);
}

int js_resume(JSContext *ctx) {
    if (ctx->host_wait) return JS_SUSPENDED;
    if (!ctx->drive_pending) {
        if (!ctx->jobs) return JS_DONE;
        ctx->task_threw = false;      /* jobs queued by the host (promise callbacks) */
        ctx->task_t0 = mono(ctx);
    }
    return task_result(ctx, js_drive(ctx));
}

bool js_busy(JSContext *ctx) { return ctx->drive_pending; }

JSValue js_task_value(JSContext *ctx) { return ctx->task_threw ? JS_UNDEFINED : ctx->resume_val; }

JSValue js_compile_module(JSContext *ctx, const char *utf8, size_t len, const char *name, JSValue *requests) {
    JS_GC_ENTER(ctx);
    *requests = JS_UNDEFINED;
    JSValue src = js_str_utf8(ctx, utf8, len);
    JSValue nm = js_str_utf8(ctx, name ? name : "module", strlen(name ? name : "module"));
    JSBytecode *bc = JS_IsException(src) || JS_IsException(nm) ? NULL : js_compile_ex(ctx, src, nm, 1, F_MODULE, requests);
    JSValue fn = bc ? js_new_closure(ctx, bc, NULL, NULL) : JS_EXCEPTION;
    JS_GC_LEAVE(ctx);
    return fn;
}

void js_set_import_hook(JSContext *ctx, JSValue fn) { ctx->import_hook = fn; }

bool js_can_wait(JSContext *ctx) { return ctx->can_wait; }

JSValue js_wait(JSContext *ctx) {
    if (!ctx->can_wait) return JS_UNDEFINED;
    ctx->can_wait = false;
    ctx->host_wait = true;
    return JS_SUSPEND;
}

bool js_waiting(JSContext *ctx) { return ctx->host_wait; }

int js_resume_with(JSContext *ctx, JSValue result) {
    if (!ctx->host_wait) return js_resume(ctx);
    ctx->host_wait = false;
    ctx->host_push = true;
    ctx->host_val = result;
    return task_result(ctx, js_drive(ctx));
}

void js_kill(JSContext *ctx) {
    log_stopped_at(ctx);
    reset_vm(ctx);
    ctx->killed = false;
}

uint32_t js_task_ms(JSContext *ctx) { return (uint32_t)(mono(ctx) - ctx->task_t0); }

void js_run_jobs(JSContext *ctx) {
    bool save = ctx->no_suspend;
    ctx->no_suspend = true;
    js_drive(ctx);
    ctx->no_suspend = save;
}

JSValue js_eval(JSContext *ctx, const char *utf8, size_t len, const char *name) {
    JS_GC_ENTER(ctx);
    JSValue src = js_str_utf8(ctx, utf8, len);
    JSValue nm = js_str_utf8(ctx, name ? name : "eval", strlen(name ? name : "eval"));
    JSValue r = JS_EXCEPTION;
    if (!JS_IsException(src) && !JS_IsException(nm)) {
        JSBytecode *bc = js_compile(ctx, src, nm, 1, F_EVAL);
        if (bc) {
            JSValue fn = js_new_closure(ctx, bc, NULL, NULL);
            if (!JS_IsException(fn)) {
                bool save = ctx->no_suspend;
                ctx->no_suspend = true;
                ctx->slice_start_d = mono(ctx);
                ctx->budget = BUDGET;
                r = js_call_internal(ctx, fn, ctx->global, 0, NULL, JS_UNDEFINED, 0);
                ctx->no_suspend = save;
            }
        }
    }
    JS_GC_LEAVE(ctx);
    return r;
}

/* ==== async functions ============================================================================================ */
static void async_resume(JSContext *ctx, JSObject *go, JSValue v, bool threw) {
    JSGen *g = go->u.p;
    if (!g || g->state != GS_AWAIT) return;
    if (g->kind == 2) { js_async_gen_resume(ctx, go, v, threw); return; }
    ctx->sp = ctx->stack;
    JSFrame *f = gen_restore(ctx, go, FR_RESUMED);
    if (!f) { JSValue e = js_take_exception(ctx); js_callv(ctx, g->reject, JS_UNDEFINED, 1, &e); return; }
    if (threw) { js_throw(ctx, v); ctx->resume_mode = GR_THROW; }
    else *f->sp++ = v;
    ctx->sp = f->sp;
}

/* async generators: resume the frame nested (they run inside next() / job calls) */
void js_async_gen_resume(JSContext *ctx, JSObject *go, JSValue v, bool threw) {
    JSGen *g = go->u.p;
    int stop = ctx->nframes;
    JSValue *saved_sp = ctx->sp;
    JSFrame *f = gen_restore(ctx, go, FR_BOUNDARY | FR_RESUMED);
    if (!f) return;
    if (threw) { js_throw(ctx, v); ctx->resume_mode = GR_THROW; }
    else *f->sp++ = v;
    ctx->sp = f->sp;
    ctx->gen_done = ctx->gen_awaiting = false;
    ctx->native_depth++;
    JSValue r = run(ctx, stop);
    ctx->native_depth--;
    after_nested(ctx);
    ctx->sp = saved_sp;
    js_async_gen_step_done(ctx, go, r, JS_IsException(r), ctx->gen_done, ctx->gen_awaiting);
    (void)g;
}

JSValue js_async_gen_run(JSContext *ctx, JSObject *go, JSValue v, int mode, bool *done, bool *awaiting) {
    JSGen *g = go->u.p;
    *done = true;
    *awaiting = false;
    int stop = ctx->nframes;
    JSValue *saved_sp = ctx->sp;
    bool was_start = g->state == GS_START;
    JSFrame *f = gen_restore(ctx, go, FR_BOUNDARY | FR_RESUMED);
    if (!f) return JS_EXCEPTION;
    if (!was_start) {
        if (mode == GR_NEXT) *f->sp++ = v;
        else if (mode == GR_THROW) { js_throw(ctx, v); ctx->resume_mode = GR_THROW; }
        else { *f->sp++ = v; if (g->ret_pc) f->pc = g->b->code + g->ret_pc; else ctx->resume_mode = GR_RETURN; }
    }
    ctx->sp = f->sp;
    ctx->gen_done = ctx->gen_awaiting = false;
    ctx->native_depth++;
    JSValue r = run(ctx, stop);
    ctx->native_depth--;
    after_nested(ctx);
    ctx->sp = saved_sp;
    *done = ctx->gen_done;
    *awaiting = ctx->gen_awaiting;
    return r;
}

/* ==== stack traces and error text ================================================================================== */
void js_stack_trace(JSContext *ctx, JSSB *sb) {
    int shown = 0;
    for (int i = ctx->nframes - 1; i >= 0 && shown < 10; i--, shown++) {
        JSFrame *f = &ctx->frames[i];
        JSBytecode *b = f->b;
        if (!b) continue;
        sb_puts(sb, "\n    at ");
        if (JS_IsString(b->name) && js_strlen(b->name)) sb_putv(sb, b->name);
        else sb_puts(sb, (b->fflags & F_SCRIPT) ? "<script>" : "<anonymous>");
        sb_puts(sb, " (");
        if (JS_IsString(b->srcname)) sb_putv(sb, b->srcname);
        char ln[16];
        snprintf(ln, sizeof ln, ":%d)", js_line_of(b, (uint32_t)(f->pc - b->code)));
        sb_puts(sb, ln);
    }
}

char *js_error_text(JSContext *ctx, JSValue e) {
    JSValue s;
    if (JS_IsObject(e) && OBJ(e)->h.cls == CL_ERROR) {
        JSValue st = js_getp(ctx, e, ctx->a[A_stack]);
        if (!JS_IsException(st) && JS_IsString(st)) s = st;
        else s = js_error_to_string_v(ctx, e);
    } else s = js_tostr(ctx, e);
    if (JS_IsException(s)) {
        js_take_exception(ctx);
        return strdup("exception");
    }
    return js_str_to_utf8(ctx, s, NULL);
}

void js_report_exception(JSContext *ctx) {
    if (!ctx->has_exc) return;
    report(ctx, js_take_exception(ctx), "Uncaught ");
}

/* ==== the context ===================================================================================================== */
static JSValue fn_noop(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)ctx; (void)this_val; (void)argc; (void)argv; (void)magic;
    return JS_UNDEFINED;
}

static JSValue fn_throw_type_error(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)argv; (void)magic;
    return js_throw_type(ctx, "'caller', 'callee', and 'arguments' properties may not be accessed on strict mode functions or the arguments objects for calls to them");
}

static const char *atom_names[A_COUNT] = {
#define JS_ATOM_STR(n) #n,
    JS_ATOMS(JS_ATOM_STR)
#undef JS_ATOM_STR
};

static const char *wk_names[WK_COUNT] = { "Symbol.iterator", "Symbol.asyncIterator", "Symbol.hasInstance", "Symbol.toPrimitive",
    "Symbol.toStringTag", "Symbol.species", "Symbol.isConcatSpreadable", "Symbol.unscopables", "Symbol.match",
    "Symbol.matchAll", "Symbol.replace", "Symbol.search", "Symbol.split" };

JSContext *js_new_context(const js_host_t *host) {
    JSContext *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return NULL;
    ctx->host = *host;
    if (!ctx->host.slice_ms) ctx->host.slice_ms = 20;
    if (!ctx->host.nested_limit_ms) ctx->host.nested_limit_ms = 3000;
    js_gc_init(ctx);
    ctx->root_free = -1;
    ctx->stack = malloc(sizeof(JSValue) * STACK_VALUES);
    ctx->frames = malloc(sizeof(JSFrame) * MAX_FRAMES);
    if (!ctx->stack || !ctx->frames) { free(ctx->stack); free(ctx->frames); free(ctx); return NULL; }
    ctx->stack_end = ctx->stack + STACK_VALUES;
    ctx->sp = ctx->stack_hw = ctx->stack;
    JSValue *vals[] = { &ctx->exception, &ctx->empty_str, &ctx->symreg, &ctx->native_data, &ctx->native_nt, &ctx->global,
                        &ctx->global_lex, &ctx->throw_type_error, &ctx->array_values, &ctx->fn_has_instance,
                        &ctx->obj_to_string, &ctx->template_cache, &ctx->eval_fn, &ctx->async_ok, &ctx->async_fail,
                        &ctx->resume_val, &ctx->array_iter_next, &ctx->regexp_exec, &ctx->unhandled,
                        &ctx->td_enc_key, &ctx->sp_list_key, &ctx->sp_url_key, &ctx->intl_opts_key };
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++) *vals[i] = JS_UNDEFINED;
    for (int i = 0; i < A_COUNT; i++) ctx->a[i] = JS_UNDEFINED;
    for (int i = 0; i < WK_COUNT; i++) ctx->wk[i] = JS_UNDEFINED;
    ctx->budget = BUDGET;
    JS_GC_ENTER(ctx);
    ctx->no_gc++;
    ctx->empty_str = js_atom16(ctx, NULL, 0);
    for (int i = 0; i < A_COUNT; i++) ctx->a[i] = js_atomc(ctx, atom_names[i]);
    for (int i = 0; i < WK_COUNT; i++) {
        JSSymbol *s = js_cell(ctx, T_SYMBOL, sizeof(JSSymbol));
        if (!s) break;
        s->desc = js_atomc(ctx, wk_names[i]);
        ctx->wk[i] = JS_MKSYM(s);
    }
    ctx->proto_object = js_obj_alloc(ctx, CL_OBJECT, NULL);
    JSObject *fp = js_obj_alloc(ctx, CL_NATIVE, ctx->proto_object);
    fp->h.flags |= OF_LAZY;
    fp->u.n.fn = fn_noop;
    fp->u.n.data = JS_UNDEFINED;
    fp->u.n.name = ctx->empty_str;
    ctx->proto_function = fp;
    ctx->global = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
    ctx->global_lex = js_obj_new(ctx, CL_OBJECT, NULL);
    ctx->symreg = js_obj_new(ctx, CL_OBJECT, NULL);
    ctx->throw_type_error = js_mk_native(ctx, fn_throw_type_error, ctx->empty_str, 0, 0);
    OBJ(ctx->throw_type_error)->h.flags &= (uint8_t)~OF_EXT;
    double now = host->now_ms ? host->now_ms(host->ud) : 0;
    uint64_t seed = (uint64_t)now ^ 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)ctx;
    for (int i = 0; i < 4; i++) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; ctx->rnd[i] = (uint32_t)(seed >> 32) | 1; }
    js_init_lib_obj(ctx);
    js_init_lib_arr(ctx);
    js_init_lib_str(ctx);
    js_init_lib_num(ctx);
    js_init_lib_json(ctx);
    js_init_lib_coll(ctx);
    js_init_lib_misc(ctx);
    ctx->no_gc--;
    JS_GC_LEAVE(ctx);
    return ctx;
}

void js_free_context(JSContext *ctx) {
    if (!ctx) return;
    reset_vm(ctx);
    js_gc_free_all(ctx);
    free(ctx->atoms);
    free(ctx->stack);
    free(ctx->frames);
    free(ctx->classes);
    free(ctx->roots);
    free(ctx->pinned);
    free(ctx);
}

void *js_get_host_ud(JSContext *ctx) { return ctx->host.ud; }
JSValue js_global(JSContext *ctx) { return ctx->global; }
