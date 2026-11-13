/*
 * js_lib_obj.c - NovaJS built-ins: Object, Function, Symbol, Error and its
 * subclasses, Boolean, Reflect, Proxy and the global functions (parseInt,
 * parseFloat, isNaN, isFinite, eval, URI encoding, escape / unescape).
 */
#include "jsi.h"

#define FN(name) static JSValue name(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic)
#define A(i) JS_ARG(i)

static JSValue to_obj_arg(JSContext *ctx, JSValue v) { return js_toobj(ctx, v); }

/* ==== Object ============================================================================ */
FN(obj_ctor) {
    JSValue nt = js_new_target(ctx);
    if (!JS_IsUndefined(nt) && nt != JS_MKOBJ(ctx->ctor_object)) {
        JSValue p = js_getp(ctx, nt, ctx->a[A_prototype]);
        if (JS_IsException(p)) return p;
        return js_obj_new(ctx, CL_OBJECT, JS_IsObject(p) ? OBJ(p) : ctx->proto_object);
    }
    JSValue v = A(0);
    if (JS_IsNullish(v)) return js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
    return js_toobj(ctx, v);
}

FN(obj_keys) {
    JSValue o = to_obj_arg(ctx, A(0));
    if (JS_IsException(o)) return o;
    JSValue keys = js_own_keys(ctx, o, OK_STRINGS | OK_ENUM);
    if (JS_IsException(keys) || magic == 0) return keys;
    /* values (1) / entries (2) */
    JSValue out = js_array_new(ctx, 0);
    if (JS_IsException(out)) return out;
    uint32_t n = OBJ(keys)->u.a.len;
    for (uint32_t i = 0; i < n; i++) {
        JSValue k = OBJ(keys)->u.a.v[i];
        JSValue v = js_getp(ctx, o, k);
        if (JS_IsException(v)) return v;
        if (magic == 2) {
            JSValue pair[2] = { k, v };
            v = js_array_from_list(ctx, pair, 2);
            if (JS_IsException(v)) return v;
        }
        if (js_array_push_v(ctx, out, v) < 0) return JS_EXCEPTION;
    }
    return out;
}

FN(obj_assign) {
    JSValue t = js_toobj(ctx, A(0));
    if (JS_IsException(t)) return t;
    for (int i = 1; i < argc; i++) {
        if (JS_IsNullish(argv[i])) continue;
        JSValue s = js_toobj(ctx, argv[i]);
        if (JS_IsException(s)) return s;
        JSValue keys = js_own_keys(ctx, s, OK_STRINGS | OK_SYMBOLS);
        if (JS_IsException(keys)) return keys;
        for (uint32_t k = 0; k < OBJ(keys)->u.a.len; k++) {
            JSValue key = OBJ(keys)->u.a.v[k];
            JSDesc d;
            int r = js_get_own_desc(ctx, s, key, &d);
            if (r < 0) return JS_EXCEPTION;
            if (!r || !(d.flags & P_E)) continue;
            JSValue v = js_getp(ctx, s, key);
            if (JS_IsException(v)) return v;
            if (js_setp(ctx, t, key, v, t, true) < 0) return JS_EXCEPTION;
        }
    }
    return t;
}

static int define_props(JSContext *ctx, JSValue o, JSValue props) {
    JSValue p = js_toobj(ctx, props);
    if (JS_IsException(p)) return -1;
    JSValue keys = js_own_keys(ctx, p, OK_STRINGS | OK_SYMBOLS);
    if (JS_IsException(keys)) return -1;
    uint32_t n = OBJ(keys)->u.a.len;
    JSDesc *ds = n ? calloc(n, sizeof(JSDesc)) : NULL;
    JSValue *ks = n ? calloc(n, sizeof(JSValue)) : NULL;
    if (n && (!ds || !ks)) { free(ds); free(ks); js_throw_oom(ctx); return -1; }
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        JSValue k = OBJ(keys)->u.a.v[i];
        JSDesc pd;
        int r = js_get_own_desc(ctx, p, k, &pd);
        if (r < 0) goto fail;
        if (!r || !(pd.flags & P_E)) continue;
        JSValue dv = js_getp(ctx, p, k);
        if (JS_IsException(dv) || js_obj_to_desc(ctx, dv, &ds[m]) < 0) goto fail;
        ks[m++] = k;
    }
    for (uint32_t i = 0; i < m; i++) {
        ds[i].flags |= DP_THROW;
        if (js_define_own(ctx, o, ks[i], &ds[i]) < 0) goto fail;
    }
    free(ds);
    free(ks);
    return 0;
fail:
    free(ds);
    free(ks);
    return -1;
}

FN(obj_create) {
    JSValue p = A(0);
    if (!JS_IsObject(p) && !JS_IsNull(p)) return js_throw_type(ctx, "Object prototype may only be an Object or null");
    JSValue o = js_obj_new(ctx, CL_OBJECT, JS_IsNull(p) ? NULL : OBJ(p));
    if (JS_IsException(o)) return o;
    if (!JS_IsUndefined(A(1)) && define_props(ctx, o, A(1)) < 0) return JS_EXCEPTION;
    return o;
}

FN(obj_define_property) {
    JSValue o = A(0);
    if (!JS_IsObject(o)) return js_throw_type(ctx, "Object.defineProperty called on non-object");
    JSValue k = js_to_key(ctx, A(1));
    if (JS_IsException(k)) return k;
    JSDesc d;
    if (js_obj_to_desc(ctx, A(2), &d) < 0) return JS_EXCEPTION;
    d.flags |= DP_THROW;
    if (js_define_own(ctx, o, k, &d) < 0) return JS_EXCEPTION;
    return o;
}

FN(obj_define_properties) {
    JSValue o = A(0);
    if (!JS_IsObject(o)) return js_throw_type(ctx, "Object.defineProperties called on non-object");
    if (define_props(ctx, o, A(1)) < 0) return JS_EXCEPTION;
    return o;
}

FN(obj_get_own_names) {
    JSValue o = to_obj_arg(ctx, A(0));
    if (JS_IsException(o)) return o;
    return js_own_keys(ctx, o, magic ? OK_SYMBOLS : OK_STRINGS);
}

FN(obj_gopd) {
    JSValue o = to_obj_arg(ctx, A(0));
    if (JS_IsException(o)) return o;
    JSValue k = js_to_key(ctx, A(1));
    if (JS_IsException(k)) return k;
    JSDesc d;
    int r = js_get_own_desc(ctx, o, k, &d);
    if (r < 0) return JS_EXCEPTION;
    if (!r) return JS_UNDEFINED;
    return js_desc_to_obj(ctx, &d);
}

FN(obj_gopds) {
    JSValue o = to_obj_arg(ctx, A(0));
    if (JS_IsException(o)) return o;
    JSValue keys = js_own_keys(ctx, o, OK_STRINGS | OK_SYMBOLS);
    if (JS_IsException(keys)) return keys;
    JSValue out = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
    if (JS_IsException(out)) return out;
    for (uint32_t i = 0; i < OBJ(keys)->u.a.len; i++) {
        JSValue k = OBJ(keys)->u.a.v[i];
        JSDesc d;
        int r = js_get_own_desc(ctx, o, k, &d);
        if (r < 0) return JS_EXCEPTION;
        if (!r) continue;
        JSValue dv = js_desc_to_obj(ctx, &d);
        if (JS_IsException(dv) || js_create_data_prop(ctx, out, k, dv) < 0) return JS_EXCEPTION;
    }
    return out;
}

FN(obj_get_proto) {
    JSValue o = to_obj_arg(ctx, A(0));
    if (JS_IsException(o)) return o;
    return js_get_prototype(ctx, o);
}

FN(obj_set_proto) {
    JSValue o = A(0), p = A(1);
    if (JS_IsNullish(o)) return js_throw_type(ctx, "Object.setPrototypeOf called on null or undefined");
    if (!JS_IsObject(p) && !JS_IsNull(p)) return js_throw_type(ctx, "Object prototype may only be an Object or null");
    if (!JS_IsObject(o)) return o;
    if (js_set_proto_obj(ctx, o, JS_IsNull(p) ? NULL : OBJ(p), true) < 0) return JS_EXCEPTION;
    return o;
}

/* freeze (magic 1) / seal (0) */
static int set_integrity(JSContext *ctx, JSValue o, bool freeze) {
    if (js_prevent_ext(ctx, o) < 0) return -1;
    JSObject *obj = OBJ(o);
    if ((obj->h.cls == CL_ARRAY || obj->h.cls == CL_ARGUMENTS) && !(obj->h.flags & OF_SLOW)) {
        obj->h.flags |= OF_SEALED | (freeze ? (OF_FROZEN | OF_LENRO) : 0);
    }
    if (obj->h.cls == CL_PROXY || obj->h.cls == CL_TYPEDARRAY) {
        JSValue keys = js_own_keys(ctx, o, OK_STRINGS | OK_SYMBOLS);
        if (JS_IsException(keys)) return -1;
        for (uint32_t i = 0; i < OBJ(keys)->u.a.len; i++) {
            JSValue k = OBJ(keys)->u.a.v[i];
            JSDesc d = { JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, DP_HAS_C | DP_THROW };
            if (freeze) {
                JSDesc cur;
                int r = js_get_own_desc(ctx, o, k, &cur);
                if (r < 0) return -1;
                if (r && !(cur.flags & P_ACC)) d.flags |= DP_HAS_W;
            }
            if (js_define_own(ctx, o, k, &d) < 0) return -1;
        }
        return 0;
    }
    js_materialize(ctx, obj);
    for (uint32_t i = 0; i < obj->nprops; i++) {
        obj->props[i].flags &= ~(uint32_t)P_C;
        if (freeze && !(obj->props[i].flags & P_ACC)) obj->props[i].flags &= ~(uint32_t)P_W;
    }
    if (obj->h.cls == CL_ARRAY && freeze) obj->h.flags |= OF_LENRO;
    return 0;
}

FN(obj_freeze) {
    if (!JS_IsObject(A(0))) return A(0);
    if (set_integrity(ctx, A(0), magic == 1) < 0) return JS_EXCEPTION;
    return A(0);
}

static int test_integrity(JSContext *ctx, JSValue o, bool frozen) {
    int e = js_is_extensible(ctx, o);
    if (e != 0) return e < 0 ? -1 : 0;
    JSObject *obj = OBJ(o);
    if ((obj->h.cls == CL_ARRAY || obj->h.cls == CL_ARGUMENTS) && !(obj->h.flags & OF_SLOW)) {
        bool any = false;
        for (uint32_t i = 0; i < MIN(obj->u.a.len, obj->u.a.cap); i++) if (obj->u.a.v[i] != JS_HOLE) { any = true; break; }
        if (any && !(obj->h.flags & OF_SEALED)) return 0;
        if (any && frozen && !(obj->h.flags & OF_FROZEN)) return 0;
        if (frozen && obj->h.cls == CL_ARRAY && !(obj->h.flags & (OF_LENRO | OF_FROZEN))) return 0;
    }
    JSValue keys = js_own_keys(ctx, o, OK_STRINGS | OK_SYMBOLS);
    if (JS_IsException(keys)) return -1;
    for (uint32_t i = 0; i < OBJ(keys)->u.a.len; i++) {
        JSDesc d;
        int r = js_get_own_desc(ctx, o, OBJ(keys)->u.a.v[i], &d);
        if (r < 0) return -1;
        if (!r) continue;
        if (d.flags & P_C) return 0;
        if (frozen && !(d.flags & P_ACC) && (d.flags & P_W)) return 0;
    }
    return 1;
}

FN(obj_is_frozen) {
    if (!JS_IsObject(A(0))) return JS_TRUE;
    int r = test_integrity(ctx, A(0), magic == 1);
    return r < 0 ? JS_EXCEPTION : JS_BOOL(r);
}

FN(obj_prevent_ext) {
    if (!JS_IsObject(A(0))) return A(0);
    int r = js_prevent_ext(ctx, A(0));
    if (r < 0) return JS_EXCEPTION;
    if (!r) return js_throw_type(ctx, "Cannot prevent extensions");
    return A(0);
}

FN(obj_is_ext) {
    if (!JS_IsObject(A(0))) return JS_FALSE;
    int r = js_is_extensible(ctx, A(0));
    return r < 0 ? JS_EXCEPTION : JS_BOOL(r);
}

FN(obj_is) { return JS_BOOL(js_same_value(A(0), A(1), false) || (JS_IsString(A(0)) && JS_IsString(A(1)) && js_streq(ctx, A(0), A(1)))); }

FN(obj_from_entries) {
    JSValue out = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
    if (JS_IsException(out)) return out;
    JSValue nx;
    JSValue it = js_get_iterator(ctx, A(0), &nx, false);
    if (JS_IsException(it)) return it;
    for (;;) {
        bool done;
        JSValue e = js_iter_step(ctx, it, nx, &done);
        if (JS_IsException(e)) return e;
        if (done) break;
        if (!JS_IsObject(e)) { js_iter_close(ctx, it, true); return js_throw_type(ctx, "Iterator value is not an entry object"); }
        JSValue k = js_geti(ctx, e, 0);
        if (JS_IsException(k)) return k;
        JSValue v = js_geti(ctx, e, 1);
        if (JS_IsException(v)) return v;
        k = js_to_key(ctx, k);
        if (JS_IsException(k) || js_create_data_prop(ctx, out, k, v) < 0) return JS_EXCEPTION;
    }
    return out;
}

FN(obj_has_own) {
    JSValue o = to_obj_arg(ctx, A(0));
    if (JS_IsException(o)) return o;
    JSValue k = js_to_key(ctx, A(1));
    if (JS_IsException(k)) return k;
    int r = js_get_own_desc(ctx, o, k, NULL);
    return r < 0 ? JS_EXCEPTION : JS_BOOL(r);
}

FN(obj_group_by) {
    /* Object.groupBy (0) / Map.groupBy (1, in the Map library) */
    JSValue out = js_obj_new(ctx, CL_OBJECT, NULL);
    if (JS_IsException(out)) return out;
    if (!js_is_callable(A(1))) return js_throw_type(ctx, "callback is not a function");
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
        key = js_to_key(ctx, key);
        if (JS_IsException(key)) return key;
        JSValue g = js_getp(ctx, out, key);
        if (JS_IsException(g)) return g;
        if (JS_IsUndefined(g)) {
            g = js_array_new(ctx, 0);
            if (JS_IsException(g) || js_create_data_prop(ctx, out, key, g) < 0) return JS_EXCEPTION;
        }
        if (js_array_push_v(ctx, g, v) < 0) return JS_EXCEPTION;
    }
    return out;
}

/* Object.prototype */
FN(objp_has_own) {
    JSValue k = js_to_key(ctx, A(0));
    if (JS_IsException(k)) return k;
    JSValue o = js_toobj(ctx, this_val);
    if (JS_IsException(o)) return o;
    int r = js_get_own_desc(ctx, o, k, NULL);
    return r < 0 ? JS_EXCEPTION : JS_BOOL(r);
}

FN(objp_is_proto_of) {
    if (!JS_IsObject(A(0))) return JS_FALSE;
    JSValue o = js_toobj(ctx, this_val);
    if (JS_IsException(o)) return o;
    bool exc;
    JSObject *p = js_get_proto_obj(ctx, A(0), &exc);
    while (p && !exc) {
        if (p == OBJ(o)) return JS_TRUE;
        p = js_get_proto_obj(ctx, JS_MKOBJ(p), &exc);
    }
    return exc ? JS_EXCEPTION : JS_FALSE;
}

FN(objp_prop_enum) {
    JSValue k = js_to_key(ctx, A(0));
    if (JS_IsException(k)) return k;
    JSValue o = js_toobj(ctx, this_val);
    if (JS_IsException(o)) return o;
    JSDesc d;
    int r = js_get_own_desc(ctx, o, k, &d);
    if (r < 0) return JS_EXCEPTION;
    return JS_BOOL(r && (d.flags & P_E));
}

FN(objp_to_string) {
    if (JS_IsUndefined(this_val)) return js_strc(ctx, "[object Undefined]");
    if (JS_IsNull(this_val)) return js_strc(ctx, "[object Null]");
    JSValue o = js_toobj(ctx, this_val);
    if (JS_IsException(o)) return o;
    const char *tag = "Object";
    int ia = js_is_array_v(ctx, o);
    if (ia < 0) return JS_EXCEPTION;
    if (ia) tag = "Array";
    else switch (OBJ(o)->h.cls) {
        case CL_ARGUMENTS: tag = "Arguments"; break;
        case CL_FUNCTION: case CL_NATIVE: case CL_BOUND: tag = "Function"; break;
        case CL_ERROR: tag = "Error"; break;
        case CL_BOOLEAN: tag = "Boolean"; break;
        case CL_NUMBER: tag = "Number"; break;
        case CL_STRING: tag = "String"; break;
        case CL_DATE: tag = "Date"; break;
        case CL_REGEXP: tag = "RegExp"; break;
        case CL_PROXY: if (js_is_callable(o)) tag = "Function"; break;
        default: break;
    }
    JSValue t = js_getp(ctx, o, ctx->wk[WK_TO_STRING_TAG]);
    if (JS_IsException(t)) return t;
    JSSB b;
    sb_init(ctx, &b);
    sb_puts(&b, "[object ");
    if (JS_IsString(t)) sb_putv(&b, t); else sb_puts(&b, tag);
    sb_putc(&b, ']');
    return sb_finish(&b);
}

FN(objp_to_locale) { return js_invoke(ctx, this_val, ctx->a[A_toString], 0, NULL); }
FN(objp_value_of) { return js_toobj(ctx, this_val); }

FN(objp_proto_get) {
    JSValue o = js_toobj(ctx, this_val);
    if (JS_IsException(o)) return o;
    return js_get_prototype(ctx, o);
}

FN(objp_proto_set) {
    if (JS_IsNullish(this_val)) return js_throw_type(ctx, "Object.prototype.__proto__ called on null or undefined");
    JSValue p = A(0);
    if ((!JS_IsObject(p) && !JS_IsNull(p)) || !JS_IsObject(this_val)) return JS_UNDEFINED;
    if (js_set_proto_obj(ctx, this_val, JS_IsNull(p) ? NULL : OBJ(p), true) < 0) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

FN(objp_define_getter) {
    JSValue o = js_toobj(ctx, this_val);
    if (JS_IsException(o)) return o;
    if (!js_is_callable(A(1))) return js_throw_type(ctx, "Object.prototype.__define%sGetter__: Expecting function", magic ? "S" : "");
    JSValue k = js_to_key(ctx, A(0));
    if (JS_IsException(k)) return k;
    JSDesc d = { JS_UNDEFINED, magic ? JS_UNDEFINED : A(1), magic ? A(1) : JS_UNDEFINED,
                 (magic ? DP_HAS_SET : DP_HAS_GET) | DP_HAS_E | DP_HAS_C | P_E | P_C | DP_THROW };
    if (js_define_own(ctx, o, k, &d) < 0) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

FN(objp_lookup_getter) {
    JSValue o = js_toobj(ctx, this_val);
    if (JS_IsException(o)) return o;
    JSValue k = js_to_key(ctx, A(0));
    if (JS_IsException(k)) return k;
    bool exc;
    for (JSObject *p = OBJ(o); p; p = js_get_proto_obj(ctx, JS_MKOBJ(p), &exc)) {
        JSDesc d;
        int r = js_get_own_desc(ctx, JS_MKOBJ(p), k, &d);
        if (r < 0) return JS_EXCEPTION;
        if (r) return (d.flags & P_ACC) ? (magic ? d.set : d.get) : JS_UNDEFINED;
        if (exc) return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

/* ==== Function ============================================================================= */
static JSValue make_function(JSContext *ctx, int argc, JSValue *argv, const char *kind) {
    JSSB b;
    sb_init(ctx, &b);
    sb_puts(&b, "(");
    sb_puts(&b, kind);
    sb_puts(&b, " anonymous(");
    for (int i = 0; i < argc - 1; i++) {
        JSValue s = js_tostr(ctx, argv[i]);
        if (JS_IsException(s)) { sb_free(&b); return s; }
        if (i) sb_putc(&b, ',');
        sb_putv(&b, s);
    }
    sb_puts(&b, "\n) {\n");
    if (argc > 0) {
        JSValue s = js_tostr(ctx, argv[argc - 1]);
        if (JS_IsException(s)) { sb_free(&b); return s; }
        sb_putv(&b, s);
    }
    sb_puts(&b, "\n})");
    JSValue src = sb_finish(&b);
    if (JS_IsException(src)) return src;
    JSBytecode *bc = js_compile(ctx, src, js_strc(ctx, "anonymous"), 1, F_EVAL);
    if (!bc) return JS_EXCEPTION;
    JSValue c = js_new_closure(ctx, bc, NULL, NULL);
    if (JS_IsException(c)) return c;
    return js_callv(ctx, c, ctx->global, 0, NULL);
}

FN(func_ctor) {
    static const char *kinds[] = { "function", "function*", "async function", "async function*" };
    return make_function(ctx, argc, argv, kinds[magic & 3]);
}

FN(funcp_call) {
    if (!js_is_callable(this_val)) return js_throw_type(ctx, "Function.prototype.call called on non-function");
    return js_callv(ctx, this_val, A(0), argc > 0 ? argc - 1 : 0, argv + 1);
}

FN(funcp_apply) {
    if (!js_is_callable(this_val)) return js_throw_type(ctx, "Function.prototype.apply was called on a non-function");
    JSValue arr = A(1);
    if (JS_IsNullish(arr)) return js_callv(ctx, this_val, A(0), 0, NULL);
    if (!JS_IsObject(arr)) return js_throw_type(ctx, "CreateListFromArrayLike called on non-object");
    if (js_fast_array(arr)) {
        JSObject *a = OBJ(arr);
        uint32_t n = a->u.a.len;
        if (n > 65536) return js_throw_range(ctx, "Too many arguments in function call");
        JSValue small[16], *args = n <= 16 ? small : malloc(sizeof(JSValue) * n);
        if (!args) return js_throw_oom(ctx);
        for (uint32_t i = 0; i < n; i++) { JSValue v = i < a->u.a.cap ? a->u.a.v[i] : JS_HOLE; args[i] = v == JS_HOLE ? JS_UNDEFINED : v; }
        JSValue r = js_callv(ctx, this_val, A(0), (int)n, args);
        if (args != small) free(args);
        return r;
    }
    int64_t n = js_length_of(ctx, arr);
    if (n < 0) return JS_EXCEPTION;
    if (n > 65536) return js_throw_range(ctx, "Too many arguments in function call");
    JSValue *args = malloc(sizeof(JSValue) * (size_t)(n ? n : 1));
    if (!args) return js_throw_oom(ctx);
    for (int64_t i = 0; i < n; i++) {
        args[i] = js_geti(ctx, arr, (uint32_t)i);
        if (JS_IsException(args[i])) { free(args); return JS_EXCEPTION; }
    }
    JSValue r = js_callv(ctx, this_val, A(0), (int)n, args);
    free(args);
    return r;
}

FN(funcp_bind) {
    if (!js_is_callable(this_val)) return js_throw_type(ctx, "Bind must be called on a function");
    JSObject *o = js_obj_alloc(ctx, CL_BOUND, OBJ(this_val)->proto);
    if (!o) return JS_EXCEPTION;
    JSValue bv = JS_MKOBJ(o);
    o->u.bf.target = this_val;
    o->u.bf.this_val = A(0);
    int n = argc > 1 ? argc - 1 : 0;
    if (n) {
        o->u.bf.args = js_malloc(ctx, sizeof(JSValue) * (size_t)n);
        if (!o->u.bf.args) return JS_EXCEPTION;
        memcpy(o->u.bf.args, argv + 1, sizeof(JSValue) * (size_t)n);
        o->u.bf.argc = n;
    }
    double len = 0;
    JSDesc d;
    int r = js_get_own_desc(ctx, this_val, ctx->a[A_length], &d);
    if (r < 0) return JS_EXCEPTION;
    if (r) {
        JSValue lv = js_getp(ctx, this_val, ctx->a[A_length]);
        if (JS_IsException(lv)) return lv;
        if (JS_IsNumber(lv)) {
            double l = js_num(lv);
            if (isinf(l)) len = l > 0 ? l : 0;
            else if (l == l) { l = trunc(l); len = l - n > 0 ? l - n : 0; }
        }
    }
    js_add_prop(ctx, o, ctx->a[A_length], js_mknum(len), P_C);
    JSValue nm = js_getp(ctx, this_val, ctx->a[A_name]);
    if (JS_IsException(nm)) return nm;
    JSSB b;
    sb_init(ctx, &b);
    sb_puts(&b, "bound ");
    if (JS_IsString(nm)) sb_putv(&b, nm);
    JSValue s = sb_finish(&b);
    if (JS_IsException(s)) return s;
    js_add_prop(ctx, o, ctx->a[A_name], s, P_C);
    return bv;
}

FN(funcp_to_string) {
    JSValue f = this_val;
    if (js_is_cls(f, CL_FUNCTION)) {
        JSBytecode *b = OBJ(f)->u.f.b;
        if (JS_IsString(b->source) && b->src_end > b->src_start && !(b->fflags & F_FIELDINIT))
            return js_substr(ctx, b->source, b->src_start, b->src_end);
    }
    if (js_is_callable(f)) {
        JSValue nm = js_getp(ctx, f, ctx->a[A_name]);
        if (JS_IsException(nm)) return nm;
        JSSB b;
        sb_init(ctx, &b);
        sb_puts(&b, "function ");
        if (JS_IsString(nm)) sb_putv(&b, nm);
        sb_puts(&b, "() { [native code] }");
        return sb_finish(&b);
    }
    return js_throw_type(ctx, "Function.prototype.toString requires that 'this' be a Function");
}

FN(funcp_has_instance) {
    int r = js_ordinary_has_instance(ctx, this_val, A(0));
    return r < 0 ? JS_EXCEPTION : JS_BOOL(r);
}

/* ==== Symbol ======================================================================================== */
static JSValue new_symbol(JSContext *ctx, JSValue desc) {
    JSSymbol *s = js_cell(ctx, T_SYMBOL, sizeof(JSSymbol));
    if (!s) return JS_EXCEPTION;
    s->desc = desc;
    return JS_MKSYM(s);
}

FN(sym_ctor) {
    if (!JS_IsUndefined(js_new_target(ctx))) return js_throw_type(ctx, "Symbol is not a constructor");
    JSValue d = JS_UNDEFINED;
    if (!JS_IsUndefined(A(0))) { d = js_tostr(ctx, A(0)); if (JS_IsException(d)) return d; }
    return new_symbol(ctx, d);
}

FN(sym_for) {
    JSValue k = js_tostr(ctx, A(0));
    if (JS_IsException(k)) return k;
    k = js_atom(ctx, k);
    if (JS_IsException(k)) return k;
    JSProp *p = js_find_prop(ctx, OBJ(ctx->symreg), k);
    if (p) return p->val;
    JSValue s = new_symbol(ctx, k);
    if (JS_IsException(s)) return s;
    SYM(s)->h.flags |= SYM_REGISTERED;
    js_add_prop(ctx, OBJ(ctx->symreg), k, s, P_W);
    return s;
}

FN(sym_key_for) {
    if (!JS_IsSymbol(A(0))) return js_throw_type(ctx, "Symbol.keyFor: not a symbol");
    return (SYM(A(0))->h.flags & SYM_REGISTERED) ? SYM(A(0))->desc : JS_UNDEFINED;
}

static JSValue this_symbol(JSContext *ctx, JSValue v) {
    if (JS_IsSymbol(v)) return v;
    if (js_is_cls(v, CL_SYMBOL)) return OBJ(v)->u.prim;
    return js_throw_type(ctx, "not a symbol");
}

FN(symp_to_string) {
    JSValue s = this_symbol(ctx, this_val);
    if (JS_IsException(s)) return s;
    JSSB b;
    sb_init(ctx, &b);
    sb_puts(&b, "Symbol(");
    if (JS_IsString(SYM(s)->desc)) sb_putv(&b, SYM(s)->desc);
    sb_putc(&b, ')');
    return sb_finish(&b);
}

FN(symp_value_of) { return this_symbol(ctx, this_val); }

FN(symp_desc) {
    JSValue s = this_symbol(ctx, this_val);
    if (JS_IsException(s)) return s;
    return SYM(s)->desc;
}

/* ==== Error ============================================================================================= */
static JSValue err_ctor_common(JSContext *ctx, int type, JSValue msg, JSValue opts) {
    JSValue nt = js_new_target(ctx);
    JSObject *proto = ctx->proto_errors[type];
    if (!JS_IsUndefined(nt)) {
        JSValue p = js_getp(ctx, nt, ctx->a[A_prototype]);
        if (JS_IsException(p)) return p;
        if (JS_IsObject(p)) proto = OBJ(p);
    }
    JSObject *o = js_obj_alloc(ctx, CL_ERROR, proto);
    if (!o) return JS_EXCEPTION;
    JSValue e = JS_MKOBJ(o);
    if (!JS_IsUndefined(msg)) {
        JSValue s = js_tostr(ctx, msg);
        if (JS_IsException(s)) return s;
        js_add_prop(ctx, o, ctx->a[A_message], s, P_W | P_C);
    }
    if (JS_IsObject(opts)) {
        int h = js_has_prop(ctx, opts, ctx->a[A_cause]);
        if (h < 0) return JS_EXCEPTION;
        if (h) {
            JSValue c = js_getp(ctx, opts, ctx->a[A_cause]);
            if (JS_IsException(c)) return c;
            js_add_prop(ctx, o, ctx->a[A_cause], c, P_W | P_C);
        }
    }
    js_error_add_stack(ctx, e);
    return e;
}

FN(err_ctor) {
    if (magic == JS_AGGREGATE_ERROR) {
        JSValue e = err_ctor_common(ctx, magic, A(1), A(2));
        if (JS_IsException(e)) return e;
        JSValue list = js_iterable_to_list(ctx, A(0));
        if (JS_IsException(list)) return list;
        js_add_prop(ctx, OBJ(e), ctx->a[A_errors], list, P_W | P_C);
        return e;
    }
    return err_ctor_common(ctx, magic, A(0), A(1));
}

JSValue js_error_to_string_v(JSContext *ctx, JSValue e) {
    if (!JS_IsObject(e)) return js_throw_type(ctx, "Error.prototype.toString called on non-object");
    JSValue n = js_getp(ctx, e, ctx->a[A_name]);
    if (JS_IsException(n)) return n;
    n = JS_IsUndefined(n) ? js_strc(ctx, "Error") : js_tostr(ctx, n);
    if (JS_IsException(n)) return n;
    JSValue m = js_getp(ctx, e, ctx->a[A_message]);
    if (JS_IsException(m)) return m;
    m = JS_IsUndefined(m) ? ctx->empty_str : js_tostr(ctx, m);
    if (JS_IsException(m)) return m;
    if (!js_strlen(n)) return m;
    if (!js_strlen(m)) return n;
    JSSB b;
    sb_init(ctx, &b);
    sb_putv(&b, n);
    sb_puts(&b, ": ");
    sb_putv(&b, m);
    return sb_finish(&b);
}

FN(errp_to_string) { return js_error_to_string_v(ctx, this_val); }

FN(err_capture) {
    if (!JS_IsObject(A(0))) return js_throw_type(ctx, "Invalid argument");
    JSSB b;
    sb_init(ctx, &b);
    JSValue s = js_error_to_string_v(ctx, A(0));
    if (JS_IsException(s)) { js_take_exception(ctx); s = js_strc(ctx, "Error"); }
    sb_putv(&b, s);
    js_stack_trace(ctx, &b);
    JSValue st = sb_finish(&b);
    if (JS_IsException(st)) return st;
    if (js_define_value(ctx, A(0), ctx->a[A_stack], st, P_W | P_C) < 0) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* ==== Boolean ============================================================================================ */
FN(bool_ctor) {
    JSValue v = JS_BOOL(js_truthy(A(0)));
    JSValue nt = js_new_target(ctx);
    if (JS_IsUndefined(nt)) return v;
    JSValue p = js_getp(ctx, nt, ctx->a[A_prototype]);
    if (JS_IsException(p)) return p;
    JSObject *o = js_obj_alloc(ctx, CL_BOOLEAN, JS_IsObject(p) ? OBJ(p) : ctx->proto_boolean);
    if (!o) return JS_EXCEPTION;
    o->u.prim = v;
    return JS_MKOBJ(o);
}

static JSValue this_bool(JSContext *ctx, JSValue v) {
    if (JS_IsBool(v)) return v;
    if (js_is_cls(v, CL_BOOLEAN)) return OBJ(v)->u.prim;
    return js_throw_type(ctx, "not a boolean");
}

FN(boolp_to_string) {
    JSValue b = this_bool(ctx, this_val);
    if (JS_IsException(b)) return b;
    return ctx->a[b == JS_TRUE ? A_true : A_false];
}

FN(boolp_value_of) { return this_bool(ctx, this_val); }

/* ==== Reflect ============================================================================================= */
static bool need_obj(JSContext *ctx, JSValue v) {
    if (JS_IsObject(v)) return true;
    js_throw_type(ctx, "Reflect method called on non-object");
    return false;
}

FN(refl_apply) {
    if (!js_is_callable(A(0))) return js_throw_type(ctx, "Reflect.apply target is not callable");
    JSValue args[2] = { A(1), A(2) };
    if (!JS_IsObject(A(2))) return js_throw_type(ctx, "CreateListFromArrayLike called on non-object");
    return funcp_apply(ctx, A(0), 2, args, 0);
}

FN(refl_construct) {
    JSValue t = A(0), nt = argc > 2 ? argv[2] : t;
    if (!js_is_ctor(t) || !js_is_ctor(nt)) return js_throw_type(ctx, "Reflect.construct target is not a constructor");
    JSValue list = A(1);
    if (!JS_IsObject(list)) return js_throw_type(ctx, "CreateListFromArrayLike called on non-object");
    int64_t n = js_length_of(ctx, list);
    if (n < 0) return JS_EXCEPTION;
    JSValue *args = malloc(sizeof(JSValue) * (size_t)(n ? n : 1));
    if (!args) return js_throw_oom(ctx);
    for (int64_t i = 0; i < n; i++) { args[i] = js_geti(ctx, list, (uint32_t)i); if (JS_IsException(args[i])) { free(args); return JS_EXCEPTION; } }
    JSValue r = js_construct_internal(ctx, t, (int)n, args, nt);
    free(args);
    return r;
}

FN(refl_define) {
    if (!need_obj(ctx, A(0))) return JS_EXCEPTION;
    JSValue k = js_to_key(ctx, A(1));
    if (JS_IsException(k)) return k;
    JSDesc d;
    if (js_obj_to_desc(ctx, A(2), &d) < 0) return JS_EXCEPTION;
    int r = js_define_own(ctx, A(0), k, &d);
    return r < 0 ? JS_EXCEPTION : JS_BOOL(r);
}

FN(refl_delete) {
    if (!need_obj(ctx, A(0))) return JS_EXCEPTION;
    JSValue k = js_to_key(ctx, A(1));
    if (JS_IsException(k)) return k;
    int r = js_delete_prop(ctx, A(0), k, false);
    return r < 0 ? JS_EXCEPTION : JS_BOOL(r);
}

FN(refl_get) {
    if (!need_obj(ctx, A(0))) return JS_EXCEPTION;
    JSValue k = js_to_key(ctx, A(1));
    if (JS_IsException(k)) return k;
    return js_getp_recv(ctx, A(0), k, argc > 2 ? argv[2] : A(0));
}

FN(refl_gopd) {
    if (!need_obj(ctx, A(0))) return JS_EXCEPTION;
    return obj_gopd(ctx, this_val, argc, argv, 0);
}

FN(refl_get_proto) {
    if (!need_obj(ctx, A(0))) return JS_EXCEPTION;
    return js_get_prototype(ctx, A(0));
}

FN(refl_has) {
    if (!need_obj(ctx, A(0))) return JS_EXCEPTION;
    JSValue k = js_to_key(ctx, A(1));
    if (JS_IsException(k)) return k;
    int r = js_has_prop(ctx, A(0), k);
    return r < 0 ? JS_EXCEPTION : JS_BOOL(r);
}

FN(refl_is_ext) {
    if (!need_obj(ctx, A(0))) return JS_EXCEPTION;
    int r = js_is_extensible(ctx, A(0));
    return r < 0 ? JS_EXCEPTION : JS_BOOL(r);
}

FN(refl_own_keys) {
    if (!need_obj(ctx, A(0))) return JS_EXCEPTION;
    return js_own_keys(ctx, A(0), OK_STRINGS | OK_SYMBOLS);
}

FN(refl_prevent_ext) {
    if (!need_obj(ctx, A(0))) return JS_EXCEPTION;
    int r = js_prevent_ext(ctx, A(0));
    return r < 0 ? JS_EXCEPTION : JS_BOOL(r);
}

FN(refl_set) {
    if (!need_obj(ctx, A(0))) return JS_EXCEPTION;
    JSValue k = js_to_key(ctx, A(1));
    if (JS_IsException(k)) return k;
    int r = js_setp(ctx, A(0), k, A(2), argc > 3 ? argv[3] : A(0), false);
    return r < 0 ? JS_EXCEPTION : JS_BOOL(r);
}

FN(refl_set_proto) {
    if (!need_obj(ctx, A(0))) return JS_EXCEPTION;
    JSValue p = A(1);
    if (!JS_IsObject(p) && !JS_IsNull(p)) return js_throw_type(ctx, "Object prototype may only be an Object or null");
    int r = js_set_proto_obj(ctx, A(0), JS_IsNull(p) ? NULL : OBJ(p), false);
    return r < 0 ? JS_EXCEPTION : JS_BOOL(r);
}

/* ==== Proxy ============================================================================================== */
FN(proxy_ctor) {
    if (JS_IsUndefined(js_new_target(ctx))) return js_throw_type(ctx, "Constructor Proxy requires 'new'");
    if (!JS_IsObject(A(0)) || !JS_IsObject(A(1))) return js_throw_type(ctx, "Cannot create proxy with a non-object as target or handler");
    JSObject *o = js_obj_alloc(ctx, CL_PROXY, NULL);
    if (!o) return JS_EXCEPTION;
    o->u.px.target = A(0);
    o->u.px.handler = A(1);
    return JS_MKOBJ(o);
}

FN(proxy_revoke_fn) {
    JSValue p = js_func_data(ctx);
    if (js_is_cls(p, CL_PROXY)) { OBJ(p)->u.px.handler = JS_NULL; OBJ(p)->u.px.target = JS_NULL; }
    return JS_UNDEFINED;
}

FN(proxy_revocable) {
    JSValue nt = ctx->native_nt;
    ctx->native_nt = JS_TRUE;
    JSValue p = proxy_ctor(ctx, this_val, argc, argv, 0);
    ctx->native_nt = nt;
    if (JS_IsException(p)) return p;
    JSValue r = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
    if (JS_IsException(r)) return r;
    JSValue rv = js_mk_native(ctx, proxy_revoke_fn, ctx->empty_str, 0, 0);
    if (JS_IsException(rv)) return rv;
    OBJ(rv)->u.n.data = p;
    js_add_prop(ctx, OBJ(r), ctx->a[A_proxy], p, P_W | P_E | P_C);
    js_add_prop(ctx, OBJ(r), ctx->a[A_revoke], rv, P_W | P_E | P_C);
    return r;
}

/* ==== globals ============================================================================================= */
FN(g_parse_int) {
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    bool e = false;
    int32_t radix = 0;
    if (!JS_IsUndefined(A(1))) { double d = js_tonum(ctx, A(1), &e); if (e) return JS_EXCEPTION; radix = js_d2i32(d); }
    JSString *str = js_flat(ctx, s);
    if (!str) return JS_EXCEPTION;
    const uint16_t *p = str->s;
    uint32_t n = str->len, i = 0;
    while (i < n && js_is_space(p[i])) i++;
    bool neg = false;
    if (i < n && (p[i] == '+' || p[i] == '-')) { neg = p[i] == '-'; i++; }
    bool strip = true;
    if (radix != 0) {
        if (radix < 2 || radix > 36) return js_mknum(NAN);
        if (radix != 16) strip = false;
    } else radix = 10;
    if (strip && i + 1 < n && p[i] == '0' && (p[i + 1] | 0x20) == 'x') { i += 2; radix = 16; }
    uint32_t start = i;
    double v = 0;
    for (; i < n; i++) {
        int c = p[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') d = (c | 0x20) - 'a' + 10;
        else break;
        if (d >= radix) break;
        v = v * radix + d;
    }
    if (i == start) return js_mknum(NAN);
    if (radix == 10 && v >= 9007199254740992.0) {
        /* exact decimal for big numbers */
        uint32_t used;
        v = js_strtod16(p + start, i - start, &used, true);
    }
    return js_mknum(neg ? -v : v);
}

FN(g_parse_float) {
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    JSString *str = js_flat(ctx, s);
    if (!str) return JS_EXCEPTION;
    const uint16_t *p = str->s;
    uint32_t n = str->len, i = 0;
    while (i < n && js_is_space(p[i])) i++;
    uint32_t k = i;
    bool neg = false;
    if (k < n && (p[k] == '+' || p[k] == '-')) { neg = p[k] == '-'; k++; }
    static const char inf[] = "Infinity";
    if (n - k >= 8) {
        bool m = true;
        for (int j = 0; j < 8; j++) if (p[k + (uint32_t)j] != (uint8_t)inf[j]) { m = false; break; }
        if (m) return js_mknum(neg ? -INFINITY : INFINITY);
    }
    uint32_t used = 0;
    double v = js_strtod16(p + i, n - i, &used, true);
    if (!used) return js_mknum(NAN);
    return js_mknum(v);
}

FN(g_is_nan) {
    bool e = false;
    double d = js_tonum(ctx, A(0), &e);
    if (e) return JS_EXCEPTION;
    return JS_BOOL(magic ? isfinite(d) : d != d);
}

FN(g_eval) {
    JSValue x = A(0);
    if (!JS_IsString(x)) return x;
    JSBytecode *b = js_compile(ctx, x, js_strc(ctx, "eval"), 1, F_EVAL);
    if (!b) return JS_EXCEPTION;
    JSValue c = js_new_closure(ctx, b, NULL, NULL);
    if (JS_IsException(c)) return c;
    return js_callv(ctx, c, ctx->global, 0, NULL);
}

static const char *uri_reserved = ";/?:@&=+$,#";
static const char *uri_unescaped_extra = "-_.!~*'()";

static bool in_set(uint32_t c, const char *set) { return c < 128 && c && strchr(set, (int)c); }
static bool alnum(uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

FN(g_encode_uri) {
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    JSString *str = js_flat(ctx, s);
    if (!str) return JS_EXCEPTION;
    static const char hex[] = "0123456789ABCDEF";
    JSSB b;
    sb_init(ctx, &b);
    for (uint32_t i = 0; i < str->len; i++) {
        uint32_t c = str->s[i];
        if (alnum(c) || in_set(c, uri_unescaped_extra) || (magic == 0 && in_set(c, uri_reserved))) { sb_putc(&b, c); continue; }
        if (c >= 0xDC00 && c < 0xE000) { sb_free(&b); return js_throw_error(ctx, JS_URI_ERROR, "URI malformed"); }
        if (c >= 0xD800 && c < 0xDC00) {
            if (i + 1 >= str->len || str->s[i + 1] < 0xDC00 || str->s[i + 1] >= 0xE000) { sb_free(&b); return js_throw_error(ctx, JS_URI_ERROR, "URI malformed"); }
            c = 0x10000 + ((c - 0xD800) << 10) + (str->s[i + 1] - 0xDC00);
            i++;
        }
        char u[4];
        int n = js_utf8_encode(c, u);
        for (int k = 0; k < n; k++) { sb_putc(&b, '%'); sb_putc(&b, (uint8_t)hex[(uint8_t)u[k] >> 4]); sb_putc(&b, (uint8_t)hex[(uint8_t)u[k] & 15]); }
    }
    return sb_finish(&b);
}

static int hexv(uint32_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? (int)(c - 'a' + 10) : -1;
}

FN(g_decode_uri) {
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    JSString *str = js_flat(ctx, s);
    if (!str) return JS_EXCEPTION;
    JSSB b;
    sb_init(ctx, &b);
    for (uint32_t i = 0; i < str->len; i++) {
        uint32_t c = str->s[i];
        if (c != '%') { sb_putc(&b, c); continue; }
        if (i + 2 >= str->len || hexv(str->s[i + 1]) < 0 || hexv(str->s[i + 2]) < 0) goto bad;
        uint32_t byte = (uint32_t)(hexv(str->s[i + 1]) * 16 + hexv(str->s[i + 2]));
        uint32_t start = i;
        i += 2;
        if (byte < 0x80) {
            if (magic == 0 && in_set(byte, uri_reserved)) { for (uint32_t k = start; k <= i; k++) sb_putc(&b, str->s[k]); }
            else sb_putc(&b, byte);
            continue;
        }
        int n = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 0;
        if (!n) goto bad;
        uint8_t u[4];
        u[0] = (uint8_t)byte;
        for (int k = 1; k < n; k++) {
            if (i + 3 > str->len - 0 || str->s[i + 1] != '%' || hexv(str->s[i + 2]) < 0 || hexv(str->s[i + 3]) < 0) goto bad;
            u[k] = (uint8_t)(hexv(str->s[i + 2]) * 16 + hexv(str->s[i + 3]));
            if ((u[k] & 0xC0) != 0x80) goto bad;
            i += 3;
        }
        uint32_t cp;
        js_utf8_decode(u, (size_t)n, &cp);
        if (cp == 0xFFFD && !(u[0] == 0xEF && u[1] == 0xBF && u[2] == 0xBD)) goto bad;
        if (cp >= 0xD800 && cp < 0xE000) goto bad;
        sb_putc(&b, cp);
    }
    return sb_finish(&b);
bad:
    sb_free(&b);
    return js_throw_error(ctx, JS_URI_ERROR, "URI malformed");
}

FN(g_escape) {
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    JSString *str = js_flat(ctx, s);
    if (!str) return JS_EXCEPTION;
    static const char hex[] = "0123456789ABCDEF";
    JSSB b;
    sb_init(ctx, &b);
    for (uint32_t i = 0; i < str->len; i++) {
        uint32_t c = str->s[i];
        if (alnum(c) || in_set(c, "@*_+-./")) sb_putc(&b, c);
        else if (c < 256) { sb_putc(&b, '%'); sb_putc(&b, (uint8_t)hex[c >> 4]); sb_putc(&b, (uint8_t)hex[c & 15]); }
        else { sb_puts(&b, "%u"); for (int k = 12; k >= 0; k -= 4) sb_putc(&b, (uint8_t)hex[(c >> k) & 15]); }
    }
    return sb_finish(&b);
}

FN(g_unescape) {
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    JSString *str = js_flat(ctx, s);
    if (!str) return JS_EXCEPTION;
    JSSB b;
    sb_init(ctx, &b);
    for (uint32_t i = 0; i < str->len; i++) {
        uint32_t c = str->s[i];
        if (c == '%') {
            if (i + 5 < str->len + 0 && str->s[i + 1] == 'u') {
                int h[4], ok = 1;
                for (int k = 0; k < 4; k++) { h[k] = hexv(str->s[i + 2 + (uint32_t)k]); if (h[k] < 0) ok = 0; }
                if (ok) { sb_putc(&b, (uint32_t)(h[0] << 12 | h[1] << 8 | h[2] << 4 | h[3])); i += 5; continue; }
            }
            if (i + 2 < str->len && hexv(str->s[i + 1]) >= 0 && hexv(str->s[i + 2]) >= 0) {
                sb_putc(&b, (uint32_t)(hexv(str->s[i + 1]) * 16 + hexv(str->s[i + 2])));
                i += 2;
                continue;
            }
        }
        sb_putc(&b, c);
    }
    return sb_finish(&b);
}

/* ==== setup ================================================================================================= */
static const JSFuncDef obj_static[] = {
    { "keys", obj_keys, 1, 0 }, { "values", obj_keys, 1, 1 }, { "entries", obj_keys, 1, 2 },
    { "assign", obj_assign, 2, 0 }, { "create", obj_create, 2, 0 }, { "defineProperty", obj_define_property, 3, 0 },
    { "defineProperties", obj_define_properties, 2, 0 }, { "getOwnPropertyNames", obj_get_own_names, 1, 0 },
    { "getOwnPropertySymbols", obj_get_own_names, 1, 1 }, { "getOwnPropertyDescriptor", obj_gopd, 2, 0 },
    { "getOwnPropertyDescriptors", obj_gopds, 1, 0 }, { "getPrototypeOf", obj_get_proto, 1, 0 },
    { "setPrototypeOf", obj_set_proto, 2, 0 }, { "freeze", obj_freeze, 1, 1 }, { "seal", obj_freeze, 1, 0 },
    { "isFrozen", obj_is_frozen, 1, 1 }, { "isSealed", obj_is_frozen, 1, 0 }, { "preventExtensions", obj_prevent_ext, 1, 0 },
    { "isExtensible", obj_is_ext, 1, 0 }, { "is", obj_is, 2, 0 }, { "fromEntries", obj_from_entries, 1, 0 },
    { "hasOwn", obj_has_own, 2, 0 }, { "groupBy", obj_group_by, 2, 0 },
};
static const JSFuncDef objp_fns[] = {
    { "hasOwnProperty", objp_has_own, 1, 0 }, { "isPrototypeOf", objp_is_proto_of, 1, 0 },
    { "propertyIsEnumerable", objp_prop_enum, 1, 0 }, { "toString", objp_to_string, 0, 0 },
    { "toLocaleString", objp_to_locale, 0, 0 }, { "valueOf", objp_value_of, 0, 0 },
    { "__defineGetter__", objp_define_getter, 2, 0 }, { "__defineSetter__", objp_define_getter, 2, 1 },
    { "__lookupGetter__", objp_lookup_getter, 1, 0 }, { "__lookupSetter__", objp_lookup_getter, 1, 1 },
};
static const JSFuncDef funcp_fns[] = {
    { "call", funcp_call, 1, 0 }, { "apply", funcp_apply, 2, 0 }, { "bind", funcp_bind, 1, 0 },
    { "toString", funcp_to_string, 0, 0 },
};
static const JSFuncDef refl_fns[] = {
    { "apply", refl_apply, 3, 0 }, { "construct", refl_construct, 2, 0 }, { "defineProperty", refl_define, 3, 0 },
    { "deleteProperty", refl_delete, 2, 0 }, { "get", refl_get, 2, 0 }, { "getOwnPropertyDescriptor", refl_gopd, 2, 0 },
    { "getPrototypeOf", refl_get_proto, 1, 0 }, { "has", refl_has, 2, 0 }, { "isExtensible", refl_is_ext, 1, 0 },
    { "ownKeys", refl_own_keys, 1, 0 }, { "preventExtensions", refl_prevent_ext, 1, 0 }, { "set", refl_set, 3, 0 },
    { "setPrototypeOf", refl_set_proto, 2, 0 },
};
static const JSFuncDef global_fns[] = {
    { "parseInt", g_parse_int, 2, 0 }, { "parseFloat", g_parse_float, 1, 0 }, { "isNaN", g_is_nan, 1, 0 },
    { "isFinite", g_is_nan, 1, 1 }, { "encodeURIComponent", g_encode_uri, 1, 1 }, { "encodeURI", g_encode_uri, 1, 0 },
    { "decodeURIComponent", g_decode_uri, 1, 1 }, { "decodeURI", g_decode_uri, 1, 0 }, { "escape", g_escape, 1, 0 },
    { "unescape", g_unescape, 1, 0 },
};

static void tag(JSContext *ctx, JSObject *o, const char *t) {
    js_add_prop(ctx, o, ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, t), P_C);
}

void js_init_lib_obj(JSContext *ctx) {
    JSValue g = ctx->global;
    JSValue op = JS_MKOBJ(ctx->proto_object);
    /* Object */
    JSObject *oc = js_new_ctor(ctx, "Object", obj_ctor, 1, ctx->proto_object, 0);
    ctx->ctor_object = oc;
    js_def_funcs(ctx, JS_MKOBJ(oc), obj_static, JS_COUNTOF(obj_static));
    js_def_funcs(ctx, op, objp_fns, JS_COUNTOF(objp_fns));
    js_def_accessor(ctx, op, ctx->a[A___proto__], objp_proto_get, objp_proto_set, 0, P_C);
    JSProp *ts = js_find_prop(ctx, ctx->proto_object, ctx->a[A_toString]);
    if (ts) ctx->obj_to_string = ts->val;
    /* Function */
    JSObject *fc = js_new_ctor(ctx, "Function", func_ctor, 1, ctx->proto_function, 0);
    (void)fc;
    JSValue fp = JS_MKOBJ(ctx->proto_function);
    js_def_funcs(ctx, fp, funcp_fns, JS_COUNTOF(funcp_fns));
    ctx->fn_has_instance = js_def_native(ctx, fp, "@hasInstance", funcp_has_instance, 1, 0);
    js_def_accessor(ctx, fp, ctx->a[A_caller], NULL, NULL, 0, P_C);
    {
        JSProp *p = js_find_prop(ctx, ctx->proto_function, ctx->a[A_caller]);
        if (p) { JSAccessor *a = JS_PTR(p->val); a->get = a->set = ctx->throw_type_error; }
        js_def_accessor(ctx, fp, ctx->a[A_arguments], NULL, NULL, 0, P_C);
        p = js_find_prop(ctx, ctx->proto_function, ctx->a[A_arguments]);
        if (p) { JSAccessor *a = JS_PTR(p->val); a->get = a->set = ctx->throw_type_error; }
    }
    /* generator / async function constructors and their prototypes */
    ctx->proto_gen_func = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_function);
    ctx->proto_async_func = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_function);
    ctx->proto_async_gen_func = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_function);
    tag(ctx, ctx->proto_gen_func, "GeneratorFunction");
    tag(ctx, ctx->proto_async_func, "AsyncFunction");
    tag(ctx, ctx->proto_async_gen_func, "AsyncGeneratorFunction");
    static const char *fnames[3] = { "GeneratorFunction", "AsyncFunction", "AsyncGeneratorFunction" };
    JSObject *fprotos[3] = { ctx->proto_gen_func, ctx->proto_async_func, ctx->proto_async_gen_func };
    int fmagic[3] = { 1, 2, 3 };
    for (int i = 0; i < 3; i++) {
        JSValue c = js_mk_native(ctx, func_ctor, js_atomc(ctx, fnames[i]), 1, fmagic[i]);
        OBJ(c)->h.flags |= OF_CTOR;
        OBJ(c)->proto = fc;
        js_materialize(ctx, OBJ(c));
        js_add_prop(ctx, OBJ(c), ctx->a[A_prototype], JS_MKOBJ(fprotos[i]), 0);
        js_add_prop(ctx, fprotos[i], ctx->a[A_constructor], c, P_C);
    }
    /* Symbol */
    ctx->proto_symbol = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
    JSObject *sc = js_new_ctor(ctx, "Symbol", sym_ctor, 0, ctx->proto_symbol, 0);
    sc->h.flags &= (uint8_t)~OF_CTOR;
    sc->h.flags |= OF_CTOR;   /* (callable with new only to throw the right error) */
    JSValue scv = JS_MKOBJ(sc);
    js_def_native(ctx, scv, "for", sym_for, 1, 0);
    js_def_native(ctx, scv, "keyFor", sym_key_for, 1, 0);
    static const char *wkn[WK_COUNT] = { "iterator", "asyncIterator", "hasInstance", "toPrimitive", "toStringTag", "species",
                                         "isConcatSpreadable", "unscopables", "match", "matchAll", "replace", "search", "split" };
    for (int i = 0; i < WK_COUNT; i++) js_add_prop(ctx, sc, js_atomc(ctx, wkn[i]), ctx->wk[i], 0);
    JSValue spv = JS_MKOBJ(ctx->proto_symbol);
    js_def_native(ctx, spv, "toString", symp_to_string, 0, 0);
    js_def_native(ctx, spv, "valueOf", symp_value_of, 0, 0);
    js_def_native(ctx, spv, "@toPrimitive", symp_value_of, 1, 0);
    js_def_accessor(ctx, spv, js_atomc(ctx, "description"), symp_desc, NULL, 0, P_C);
    tag(ctx, ctx->proto_symbol, "Symbol");
    /* Errors */
    static const char *en[JS_NERRORS] = { "Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError", "EvalError", "URIError", "AggregateError" };
    ctx->proto_error = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
    ctx->proto_errors[0] = ctx->proto_error;
    JSObject *ec = NULL;
    for (int i = 0; i < JS_NERRORS; i++) {
        JSObject *p = i == 0 ? ctx->proto_error : js_obj_alloc(ctx, CL_OBJECT, ctx->proto_error);
        ctx->proto_errors[i] = p;
        JSObject *c = js_new_ctor(ctx, en[i], err_ctor, i == JS_AGGREGATE_ERROR ? 2 : 1, p, i);
        if (i == 0) ec = c; else c->proto = ec;
        js_add_prop(ctx, p, ctx->a[A_name], js_atomc(ctx, en[i]), P_W | P_C);
        js_add_prop(ctx, p, ctx->a[A_message], ctx->empty_str, P_W | P_C);
    }
    js_def_native(ctx, JS_MKOBJ(ctx->proto_error), "toString", errp_to_string, 0, 0);
    js_def_native(ctx, JS_MKOBJ(ec), "captureStackTrace", err_capture, 1, 0);
    js_add_prop(ctx, ec, js_atomc(ctx, "stackTraceLimit"), js_mknum(10), P_W | P_E | P_C);
    /* Boolean */
    ctx->proto_boolean = js_obj_alloc(ctx, CL_BOOLEAN, ctx->proto_object);
    ctx->proto_boolean->u.prim = JS_FALSE;
    js_new_ctor(ctx, "Boolean", bool_ctor, 1, ctx->proto_boolean, 0);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_boolean), "toString", boolp_to_string, 0, 0);
    js_def_native(ctx, JS_MKOBJ(ctx->proto_boolean), "valueOf", boolp_value_of, 0, 0);
    /* Reflect */
    JSValue refl = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
    js_def_funcs(ctx, refl, refl_fns, JS_COUNTOF(refl_fns));
    tag(ctx, OBJ(refl), "Reflect");
    js_add_prop(ctx, OBJ(g), js_atomc(ctx, "Reflect"), refl, P_W | P_C);
    /* Proxy */
    JSObject *pc = js_new_ctor(ctx, "Proxy", proxy_ctor, 2, NULL, 0);
    js_def_native(ctx, JS_MKOBJ(pc), "revocable", proxy_revocable, 2, 0);
    /* globals */
    js_def_funcs(ctx, g, global_fns, JS_COUNTOF(global_fns));
    ctx->eval_fn = js_def_native(ctx, g, "eval", g_eval, 1, 0);
    js_add_prop(ctx, OBJ(g), js_atomc(ctx, "globalThis"), g, P_W | P_C);
    js_add_prop(ctx, OBJ(g), ctx->a[A_NaN], js_mknum(NAN), 0);
    js_add_prop(ctx, OBJ(g), ctx->a[A_Infinity], js_mknum(INFINITY), 0);
    js_add_prop(ctx, OBJ(g), ctx->a[A_undefined], JS_UNDEFINED, 0);
}
