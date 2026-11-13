/*
 * jsi.h - NovaJS internals shared by the engine's files.
 *
 *   js_gc.c      cell allocator, mark & sweep, roots, memory cap
 *   js_str.c     strings (UTF-16), ropes, atoms, string builder, UTF-8
 *   js_num.c     number <-> string (shortest round trip, exact rounding)
 *   js_obj.c     objects, properties, arrays, conversions, operators, errors
 *   js_lex.c     tokenizer          js_parse.c   parser -> AST
 *   js_comp.c    scopes + bytecode  js_vm.c      interpreter, calls, generators, tasks
 *   js_re.c      regular expressions (backtracking)
 *   js_lib*.c    the built-in library
 */
#ifndef NOVA_JSI_H
#define NOVA_JSI_H

#include "js.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define JS_COUNTOF(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define JS_LIKELY(x)   __builtin_expect(!!(x), 1)
#define JS_UNLIKELY(x) __builtin_expect(!!(x), 0)

typedef struct JSString JSString;
typedef struct JSRope JSRope;
typedef struct JSSymbol JSSymbol;
typedef struct JSObject JSObject;
typedef struct JSBytecode JSBytecode;
typedef struct JSVarRef JSVarRef;
typedef struct JSAccessor JSAccessor;
typedef struct JSFrame JSFrame;

/* ==== GC cells ============================================================================= */
enum { T_FREE = 0, T_OBJECT, T_STRING, T_ROPE, T_SYMBOL, T_BYTECODE, T_VARREF, T_ACCESSOR, T_BIGINT };
typedef struct GCHdr {
    uint8_t type, mark, flags, cls;
    uint32_t aux;          /* allocator: size class (small) or 0xFFFFFFFF (large) */
} GCHdr;

/* ==== strings ================================================================================ */
#define STR_ATOM    1
#define STR_HASHED  2
#define STR_IDXCHK  4      /* idx checked                        */
#define STR_ISIDX   8      /* canonical array index (in idx)     */
#define STR_SYMDESC 16
struct JSString { GCHdr h; uint32_t len, hash, idx; uint16_t s[]; };
struct JSRope   { GCHdr h; uint32_t len, depth; JSValue left, right; JSString *flat; };
#define SYM_PRIVATE    1
#define SYM_REGISTERED 2
struct JSSymbol { GCHdr h; JSValue desc; };

/* ==== objects ================================================================================= */
#define P_W   1
#define P_E   2
#define P_C   4
#define P_ACC 8           /* val is an accessor pair (internal pointer) */
typedef struct JSProp { JSValue key, val; uint32_t flags; } JSProp;
struct JSAccessor { GCHdr h; JSValue get, set; };
struct JSVarRef   { GCHdr h; JSValue v; };

enum {
    CL_OBJECT = 0, CL_ARRAY, CL_ARGUMENTS, CL_FUNCTION, CL_NATIVE, CL_BOUND, CL_ERROR,
    CL_BOOLEAN, CL_NUMBER, CL_STRING, CL_SYMBOL, CL_BIGINT, CL_DATE, CL_REGEXP,
    CL_MAP, CL_SET, CL_WEAKMAP, CL_WEAKSET, CL_WEAKREF, CL_FINREG, CL_PROMISE,
    CL_GENERATOR, CL_ASYNC_GEN, CL_ASYNC, CL_ARRAYBUFFER, CL_TYPEDARRAY, CL_DATAVIEW,
    CL_PROXY, CL_ITER, CL_FORIN, CL_HOST, CL_MODULE_NS, CL_COUNT
};
/* object flags (h.flags) */
#define OF_EXT      1     /* extensible                                  */
#define OF_LAZY     2     /* function: length / name / prototype not made yet */
#define OF_FROZEN   4     /* array elements read-only                    */
#define OF_SEALED   8     /* array elements not configurable             */
#define OF_SLOW     16    /* array: elements live in the property table  */
#define OF_PINNED   32    /* never collected                             */
#define OF_CTOR     64    /* callable with new                            */
#define OF_LENRO    128   /* array: length is not writable                */

typedef struct JSMapEnt { JSValue key, val; uint32_t hnext; } JSMapEnt;
typedef struct JSMap {
    JSMapEnt *e; uint32_t n, cap, live;  /* entries in insertion order (key HOLE = deleted) */
    uint32_t *h; uint32_t hsize;         /* hash heads (index + 1)                        */
    JSObject **iters; uint32_t niters, itcap;   /* live iterators (weak), fixed up on compaction */
    bool weak;
} JSMap;

typedef struct JSTypedArray {
    JSObject *buf;      /* the ArrayBuffer */
    uint32_t off, len;  /* byte offset, element count (DataView: byte length) */
    uint8_t type;       /* TA_* */
    bool track;         /* length follows the buffer (length-tracking) */
} JSTypedArray;
enum { TA_I8 = 0, TA_U8, TA_U8C, TA_I16, TA_U16, TA_I32, TA_U32, TA_F32, TA_F64, TA_BI64, TA_BU64, TA_COUNT };
typedef struct JSArrayBuffer { uint8_t *data; uint32_t len; bool detached; } JSArrayBuffer;

typedef struct JSIter {   /* built-in iterators */
    JSValue target;
    uint32_t idx;
    uint8_t kind;         /* IT_KEYS / IT_VALUES / IT_ENTRIES */
    uint8_t type;         /* ITT_* */
    bool done;
    JSValue extra;        /* regexp string iterator: the string; for-in: key list */
} JSIter;
enum { IT_KEYS = 0, IT_VALUES, IT_ENTRIES };
enum { ITT_ARRAY = 0, ITT_MAP, ITT_SET, ITT_STRING, ITT_REGEXP, ITT_FORIN, ITT_TYPED };

struct JSObject {
    GCHdr h;              /* h.cls = CL_*, h.flags = OF_*            */
    JSObject *proto;
    JSProp *props;
    uint32_t nprops, pcap;
    uint32_t *hidx;       /* open-addressing index (prop index + 1)   */
    uint32_t hsize;
    union {
        struct { JSValue *v; uint32_t len, cap; } a;                  /* ARRAY, ARGUMENTS */
        struct { JSBytecode *b; JSVarRef **vr; JSObject *home; JSObject *fields; } f;   /* FUNCTION */
        struct { JSNativeFn fn; JSValue data; JSValue name; int16_t length, magic; } n; /* NATIVE */
        struct { JSValue target, this_val; JSValue *args; int argc; } bf;              /* BOUND */
        JSValue prim;                                                  /* BOOLEAN NUMBER STRING SYMBOL DATE */
        struct { void *opaque; int cls; } host;                        /* HOST */
        struct { JSValue target, handler; } px;                        /* PROXY */
        void *p;              /* JSMap, JSTypedArray, JSArrayBuffer, JSIter, regexp, promise, generator ... */
    } u;
};
#define OBJ(v)  ((JSObject *)JS_PTR(v))
#define STR(v)  ((JSString *)JS_PTR(v))
#define SYM(v)  ((JSSymbol *)JS_PTR(v))
#define JS_MKOBJ(p) JS_MKTAG(JS_TAG_OBJECT, (uintptr_t)(p))
#define JS_MKSTR(p) JS_MKTAG(JS_TAG_STRING, (uintptr_t)(p))
#define JS_MKSYM(p) JS_MKTAG(JS_TAG_SYMBOL, (uintptr_t)(p))
#define JS_MKINT(p) JS_MKTAG(JS_TAG_INTERNAL, (uintptr_t)(p))
#define JS_MKMARK(n) JS_MKTAG(JS_TAG_MARKER, (uint32_t)(n))
#define JS_IsInternal(v) (JS_TAGOF(v) == JS_TAG_INTERNAL)
#define JS_IsMarker(v)   (JS_TAGOF(v) == JS_TAG_MARKER)
#define JS_IsPtr(v)      (JS_TAGOF(v) >= JS_TAG_STRING && JS_TAGOF(v) <= JS_TAG_INTERNAL)

static inline bool js_is_cls(JSValue v, int cls) { return JS_IsObject(v) && OBJ(v)->h.cls == cls; }

/* ==== bytecode ================================================================================ */
#define F_STRICT     0x0001
#define F_ARROW      0x0002
#define F_GEN        0x0004
#define F_ASYNC      0x0008
#define F_METHOD     0x0010   /* method / accessor: no prototype, not a constructor */
#define F_CTOR       0x0020   /* class constructor (base) */
#define F_DERIVED    0x0040   /* class constructor of a derived class */
#define F_CLASSCTOR  0x0080   /* any class constructor: throws without new */
#define F_SCRIPT     0x0100   /* top-level script code */
#define F_ARGS       0x0200   /* creates an arguments object */
#define F_FIELDINIT  0x0400   /* class field initialiser */
#define F_MODULE     0x0800
#define F_EVAL       0x1000
#define F_NEEDTHIS   0x2000   /* sloppy function that reads this (box primitives / undefined) */
#define F_ACCESSOR   0x4000

struct JSBytecode {
    GCHdr h;
    uint8_t *code; uint32_t len;
    JSValue *cp; uint32_t ncp;
    uint16_t nparams, length, nlocals, stack_max, ncv;
    uint16_t *cv;               /* closure vars: bit 15 = from the parent's local box slot, else parent's cv index */
    uint32_t fflags;
    JSValue name;               /* string or UNDEFINED */
    JSValue srcname;            /* script name string */
    JSValue source;             /* whole script source (string) for Function.prototype.toString */
    uint32_t src_start, src_end;
    uint32_t *lines; uint32_t nlines;   /* (pc, line) pairs */
    int16_t this_slot, nt_slot, func_slot, args_slot;   /* hidden local slots (-1 = none) */
    int16_t rest_param;         /* index of the ...rest param, -1 none */
    JSValue *lnames;            /* local slot names (TDZ messages), nlocals entries */
};

/* ==== frames ==================================================================================== */
#define FR_BOUNDARY  1     /* entered from C: run() returns when this frame returns */
#define FR_CONSTRUCT 2
#define FR_GEN       4     /* generator / async frame (state saved in gen) */
#define FR_CONT      8     /* call cont() with the completion */
#define FR_RESUMED   16    /* a generator / async frame restored from its saved state */
typedef void (*JSCont)(JSContext *ctx, JSValue v, bool threw, void *data, JSValue dv);
struct JSFrame {
    JSValue *base;          /* stack position of the callee (reset on return) */
    JSValue *argv;
    JSValue *locals;
    JSValue *sp;            /* saved operand stack pointer */
    const uint8_t *pc;
    JSObject *func;
    JSBytecode *b;
    JSValue this_val, new_target;
    int argc;
    uint16_t flags;
    uint8_t resume;         /* generator resume mode (GR_*) */
    JSCont cont; void *cont_data; JSValue cont_val;
    JSObject *gen;          /* generator / async object owning the frame */
};
enum { GR_NEXT = 0, GR_THROW, GR_RETURN };

/* generator / async function state */
enum { GS_START = 0, GS_SUSPENDED_YIELD, GS_RUNNING, GS_DONE, GS_AWAIT };
typedef struct JSGen {
    uint8_t state, kind;           /* kind: 0 generator, 1 async function, 2 async generator */
    JSObject *func;
    JSBytecode *b;
    JSValue this_val, new_target;
    JSValue *saved; uint32_t nsaved, savecap;    /* locals + operand stack while suspended */
    uint32_t ret_pc;                /* where gen.return() resumes (0 = return at once) */
    bool in_delegate;               /* suspended inside yield* */
    uint32_t argc, nlocals_off, sp_off;  /* layout of saved */
    uint32_t pc;                    /* offset */
    JSValue promise, resolve, reject;    /* async: the result promise and its functions */
    JSValue queue;                  /* async generator: array of pending requests */
    JSValue delegate;               /* (unused) */
} JSGen;

/* ==== the context ================================================================================= */
typedef struct GCPage GCPage;
typedef struct GCSeg { uintptr_t start, end; GCPage *page; } GCSeg;
enum { JOB_CALL = 0, JOB_REACTION, JOB_THENABLE, JOB_TASK, JOB_SCRIPT };
typedef struct JSJob { JSValue fn; JSValue args[4]; int argc; int kind; struct JSJob *next; } JSJob;
typedef struct js_class_rec { js_class_t c; } js_class_rec;

enum {   /* well-known symbols */
    WK_ITERATOR = 0, WK_ASYNC_ITERATOR, WK_HAS_INSTANCE, WK_TO_PRIMITIVE, WK_TO_STRING_TAG,
    WK_SPECIES, WK_IS_CONCAT_SPREADABLE, WK_UNSCOPABLES, WK_MATCH, WK_MATCH_ALL, WK_REPLACE,
    WK_SEARCH, WK_SPLIT, WK_COUNT
};

/* frequently used atoms */
#define JS_ATOMS(X) X(length) X(prototype) X(constructor) X(name) X(message) X(stack) X(toString) \
    X(valueOf) X(value) X(done) X(next) X(get) X(set) X(writable) X(enumerable) X(configurable) \
    X(caller) X(callee) X(arguments) X(return) X(throw) X(then) X(default) X(index) X(input) \
    X(groups) X(lastIndex) X(source) X(flags) X(global) X(cause) X(errors) X(join) X(toJSON) \
    X(undefined) X(null) X(true) X(false) X(object) X(function) X(number) X(string) X(boolean) \
    X(symbol) X(bigint) X(NaN) X(Infinity) X(anonymous) X(raw) X(size) X(add) X(has) X(proxy) \
    X(revoke) X(apply) X(construct) X(defineProperty) X(deleteProperty) X(getOwnPropertyDescriptor) \
    X(getPrototypeOf) X(setPrototypeOf) X(isExtensible) X(preventExtensions) X(ownKeys) X(exec) \
    X(dotAll) X(hasIndices) X(ignoreCase) X(multiline) X(sticky) X(unicode) X(unicodeSets) X(status) X(reason) \
    X(fulfilled) X(rejected) X(empty) X(__proto__) X(async) X(await) X(yield) X(let) X(static) \
    X(of) X(as) X(from) X(target) X(meta) X(eval) X(fileName) X(lineNumber) X(toISOString) X(indices) X(byteLength)
#define JS_ATOM_ENUM(n) A_##n,
enum { JS_ATOMS(JS_ATOM_ENUM) A_COUNT };
#undef JS_ATOM_ENUM

struct JSContext {
    js_host_t host;
    /* heap */
    GCPage *pages[24];            /* pages per size class (list heads)   */
    void *free_cells[24];
    GCSeg *segs; int nsegs, segcap;
    GCHdr **large; int nlarge, largecap;
    size_t mem_used, mem_limit, gc_threshold, alloc_since;
    uint32_t gc_count;
    bool in_gc, oom, oom_throwing;
    int no_gc;                    /* > 0 while compiling (the AST holds values) */
    uintptr_t stack_base;         /* C stack top at the outermost API entry (conservative scan) */
    int api_depth;
    JSValue *mark_stack; int msp, mcap;
    JSObject **weaks; int nweak, weakcap;     /* weak maps / sets seen while marking */
    JSObject **wrefs; int nwrefs, wrefcap;    /* weak refs seen while marking        */
    JSObject **pinned; int npinned, pincap;
    /* atoms */
    JSString **atoms; uint32_t natoms, atomcap;
    JSValue a[A_COUNT];
    JSValue wk[WK_COUNT];         /* well-known symbols */
    JSValue empty_str;
    JSValue symreg;               /* Symbol.for registry (object) */
    /* VM */
    JSValue *stack, *stack_end, *sp, *stack_hw;
    JSFrame *frames; int nframes, framecap;
    int run_depth;                /* nested run() calls */
    int native_depth;
    JSValue exception;
    bool has_exc, uncatchable;
    int32_t budget;
    uint32_t slice_start, task_start;
    bool task_active, suspended, killed;
    int task_frames;              /* frame count when the task started (0) */
    JSValue native_data, native_nt;
    /* realm */
    JSValue global, global_lex;
    JSObject *proto_object, *proto_function, *proto_array, *proto_error, *proto_errors[JS_NERRORS];
    JSObject *proto_string, *proto_number, *proto_boolean, *proto_symbol, *proto_bigint;
    JSObject *proto_iterator, *proto_array_iter, *proto_map_iter, *proto_set_iter, *proto_string_iter;
    JSObject *proto_regexp_iter, *proto_generator, *proto_async_gen, *proto_gen_func, *proto_async_func;
    JSObject *proto_async_gen_func, *proto_promise, *proto_regexp, *proto_date, *proto_map, *proto_set;
    JSObject *proto_weakmap, *proto_weakset, *proto_weakref, *proto_arraybuffer, *proto_dataview;
    JSObject *proto_typed, *proto_ta[TA_COUNT], *proto_async_from_sync, *proto_finreg, *proto_async_iter;
    JSObject *ctor_promise, *ctor_array, *ctor_object, *ctor_typed[TA_COUNT], *ctor_url;
    JSObject *proto_textenc, *proto_textdec, *proto_url, *proto_usp, *proto_intl_nf, *proto_intl_dtf;
    JSObject *proto_intl_coll, *proto_intl_pr, *proto_intl_rtf;
    JSObject *ctor_regexp;        /* (last of the JSObject * block the GC walks) */
    JSValue throw_type_error;     /* %ThrowTypeError% */
    JSValue array_values;         /* Array.prototype.values (arguments[@@iterator]) */
    JSValue fn_has_instance;      /* Function.prototype[@@hasInstance] */
    JSValue obj_to_string;        /* Object.prototype.toString */
    /* jobs */
    JSJob *jobs, *jobs_tail, *cur_job;
    bool drive_pending, no_suspend, task_threw, gen_raw, gen_done, gen_awaiting;
    int resume_mode;
    JSValue resume_val, array_iter_next, regexp_exec, unhandled;
    JSValue td_enc_key, sp_list_key, sp_url_key, intl_opts_key;
    int url_class;
    double slice_t, slice_start_d, task_t0;
    JSObject *join_stack[64]; int njoin;    /* Array.prototype.join cycle check */
    int task_status;
    JSValue eval_fn;              /* the realm's eval (direct eval check) */
    JSValue async_ok, async_fail; /* the native reaction functions resuming async frames */
    /* host */
    js_class_rec *classes; int nclasses;
    JSValue *roots; int nroots, rootcap, root_free;
    JSValue template_cache;       /* (unused) */
    uint32_t rnd[4];
    double time_base;
    void *re_cache;
    int eval_line;
};

/* ==== js_gc.c ===================================================================================== */
void   *js_cell(JSContext *ctx, int type, size_t size);    /* zeroed cell, NULL = out of memory (exception set) */
void   *js_malloc(JSContext *ctx, size_t n);
void   *js_mallocz(JSContext *ctx, size_t n);
void   *js_realloc(JSContext *ctx, void *p, size_t old, size_t n);
void    js_mfree(JSContext *ctx, void *p, size_t n);
void    js_gc_mark_value(JSContext *ctx, JSValue v);
void    js_gc_mark_cell(JSContext *ctx, void *cell);
void    js_gc_init(JSContext *ctx);
void    js_gc_free_all(JSContext *ctx);
void    js_maybe_gc(JSContext *ctx);
#define JS_GC_ENTER(ctx) uintptr_t js__sb_; int js__enter_ = js_api_enter(ctx, (uintptr_t)&js__sb_)
#define JS_GC_LEAVE(ctx) js_api_leave(ctx, js__enter_)
int     js_api_enter(JSContext *ctx, uintptr_t sb);
void    js_api_leave(JSContext *ctx, int prev);

/* ==== js_str.c ===================================================================================== */
JSString *js_str_alloc(JSContext *ctx, uint32_t len);
JSValue   js_str16(JSContext *ctx, const uint16_t *s, uint32_t len);
JSValue   js_str8(JSContext *ctx, const char *s, uint32_t len);      /* Latin-1 bytes */
JSValue   js_str_utf8(JSContext *ctx, const char *s, size_t len);
JSValue   js_strc(JSContext *ctx, const char *ascii);
JSString *js_flat(JSContext *ctx, JSValue s);      /* flatten a rope; NULL = OOM */
uint32_t  js_strlen(JSValue s);
JSValue   js_concat(JSContext *ctx, JSValue a, JSValue b);
JSValue   js_substr(JSContext *ctx, JSValue s, uint32_t start, uint32_t end);
int       js_strcmp(JSContext *ctx, JSValue a, JSValue b);      /* code unit order */
bool      js_streq(JSContext *ctx, JSValue a, JSValue b);
bool      js_str_eq_ascii(JSContext *ctx, JSValue s, const char *a);
uint32_t  js_str_hash(JSString *s);
JSValue   js_atom(JSContext *ctx, JSValue s);         /* intern (s: string) */
JSValue   js_atom16(JSContext *ctx, const uint16_t *s, uint32_t len);
JSValue   js_atomc(JSContext *ctx, const char *ascii);
bool      js_atom_index(JSValue atom, uint32_t *idx);  /* canonical array index? */
JSValue   js_atom_from_index(JSContext *ctx, uint32_t i);
void      js_atoms_sweep(JSContext *ctx);
char     *js_str_to_utf8(JSContext *ctx, JSValue s, size_t *len);   /* malloc'd (plain malloc) */
void      js_str_to_ascii(JSContext *ctx, JSValue s, char *out, size_t size);  /* for messages */
int32_t   js_str_index_of(JSContext *ctx, JSString *h, JSString *n, uint32_t from);
int32_t   js_str_last_index_of(JSContext *ctx, JSString *h, JSString *n, uint32_t from);
uint32_t  js_utf8_decode(const uint8_t *s, size_t n, uint32_t *cp);
int       js_utf8_encode(uint32_t cp, char *out);
uint32_t  js_case_upper(uint32_t c);
uint32_t  js_case_lower(uint32_t c);
bool      js_is_space(uint32_t c);
bool      js_is_id_start(uint32_t c);
bool      js_is_id_part(uint32_t c);

/* string builder */
typedef struct JSSB { JSContext *ctx; uint16_t *s; uint32_t len, cap; bool err; } JSSB;
void      sb_init(JSContext *ctx, JSSB *b);
void      sb_putc(JSSB *b, uint32_t c);           /* code unit (or code point > 0xFFFF as a pair) */
void      sb_put16(JSSB *b, const uint16_t *s, uint32_t n);
void      sb_puts(JSSB *b, const char *ascii);
void      sb_putv(JSSB *b, JSValue str);          /* a string value */
JSValue   sb_finish(JSSB *b);                     /* -> string or JS_EXCEPTION */
void      sb_free(JSSB *b);

/* ==== js_num.c ====================================================================================== */
int       js_dtoa(double d, char *buf);                     /* shortest round trip, JS formatting */
int       js_dtoa_radix(double d, int radix, char *buf, int size);
int       js_dtoa_fixed(double d, int frac, char *buf, int size);
int       js_dtoa_prec(double d, int prec, char *buf, int size);
int       js_dtoa_exp(double d, int frac, char *buf, int size);   /* frac < 0: as many as needed */
double    js_strtod16(const uint16_t *s, uint32_t n, uint32_t *used, bool allow_trailing);  /* decimal float */
double    js_str_to_number(JSContext *ctx, JSString *s);    /* StringToNumber (NaN on junk) */
JSValue   js_num_to_str(JSContext *ctx, double d);

/* ==== js_obj.c ====================================================================================== */
JSObject *js_obj_alloc(JSContext *ctx, int cls, JSObject *proto);
JSValue   js_obj_new(JSContext *ctx, int cls, JSObject *proto);   /* -> object value or JS_EXCEPTION */
JSProp   *js_find_prop(JSContext *ctx, JSObject *o, JSValue key);
JSProp   *js_add_prop(JSContext *ctx, JSObject *o, JSValue key, JSValue v, uint32_t flags);
int       js_del_prop(JSContext *ctx, JSObject *o, JSValue key);
void      js_materialize(JSContext *ctx, JSObject *o);   /* lazy function properties */
JSValue   js_to_key(JSContext *ctx, JSValue v);          /* ToPropertyKey -> atom / symbol / EXCEPTION */
JSValue   js_getp(JSContext *ctx, JSValue obj, JSValue key);                  /* key: atom or symbol */
JSValue   js_getp_recv(JSContext *ctx, JSValue obj, JSValue key, JSValue recv);
JSValue   js_geti(JSContext *ctx, JSValue obj, uint32_t i);
JSValue   js_getv(JSContext *ctx, JSValue obj, JSValue key);                  /* any key value */
int       js_setp(JSContext *ctx, JSValue obj, JSValue key, JSValue v, JSValue recv, bool strict);
int       js_seti(JSContext *ctx, JSValue obj, uint32_t i, JSValue v);
int       js_setv(JSContext *ctx, JSValue obj, JSValue key, JSValue v, bool strict);
#define DP_HAS_VALUE 0x100
#define DP_HAS_GET   0x200
#define DP_HAS_SET   0x400
#define DP_HAS_W     0x800
#define DP_HAS_E     0x1000
#define DP_HAS_C     0x2000
#define DP_THROW     0x4000
#define DP_DATA_ALL  (DP_HAS_VALUE | DP_HAS_W | DP_HAS_E | DP_HAS_C)
typedef struct JSDesc { JSValue value, get, set; uint32_t flags; } JSDesc;  /* flags: P_* + DP_HAS_* */
int       js_define_own(JSContext *ctx, JSValue obj, JSValue key, const JSDesc *d);   /* 1 ok, 0 refused, -1 exc */
int       js_define_value(JSContext *ctx, JSValue obj, JSValue key, JSValue v, uint32_t pflags);  /* DP data, throws on failure */
int       js_create_data_prop(JSContext *ctx, JSValue obj, JSValue key, JSValue v);   /* CreateDataPropertyOrThrow */
int       js_get_own_desc(JSContext *ctx, JSValue obj, JSValue key, JSDesc *d);      /* 1 found, 0 no, -1 exc */
int       js_has_prop(JSContext *ctx, JSValue obj, JSValue key);                     /* 1/0/-1 */
int       js_delete_prop(JSContext *ctx, JSValue obj, JSValue key, bool strict);    /* 1 deleted, 0 no, -1 exc */
JSValue   js_own_keys(JSContext *ctx, JSValue obj, int flags);                     /* array of keys */
JSValue   js_desc_to_obj(JSContext *ctx, const JSDesc *d);
int       js_obj_to_desc(JSContext *ctx, JSValue o, JSDesc *d);                      /* ToPropertyDescriptor */
JSValue   js_ta_get(JSContext *ctx, JSObject *o, uint32_t i);                        /* typed array element */
int       js_ta_set(JSContext *ctx, JSObject *o, uint32_t i, JSValue v);
uint32_t  js_ta_length(JSObject *o);
int       js_ta_elem_size(int type);
JSValue   js_proxy_call(JSContext *ctx, JSObject *p, JSValue this_val, int argc, JSValue *argv);
JSValue   js_proxy_construct(JSContext *ctx, JSObject *p, int argc, JSValue *argv, JSValue nt);
#define OK_STRINGS 1
#define OK_SYMBOLS 2
#define OK_ENUM    4      /* only enumerable */
#define OK_PRIVATE 8
JSObject *js_get_proto_obj(JSContext *ctx, JSValue obj, bool *exc);
int       js_set_proto_obj(JSContext *ctx, JSValue obj, JSObject *proto, bool throw);
int       js_prevent_ext(JSContext *ctx, JSValue obj);
int       js_is_extensible(JSContext *ctx, JSValue obj);
JSValue   js_def_native(JSContext *ctx, JSValue obj, const char *name, JSNativeFn fn, int len, int magic);
JSValue   js_mk_native(JSContext *ctx, JSNativeFn fn, JSValue name, int len, int magic);
void      js_def_accessor(JSContext *ctx, JSValue obj, JSValue key, JSNativeFn get, JSNativeFn set, int magic, uint32_t flags);
void      js_set_func_name(JSContext *ctx, JSValue fn, JSValue key, const char *prefix);
/* arrays */
JSValue   js_array_new(JSContext *ctx, uint32_t len);
int       js_array_push_v(JSContext *ctx, JSValue arr, JSValue v);
bool      js_fast_array(JSValue v);
int       js_array_set_length(JSContext *ctx, JSObject *a, uint32_t len);
int       js_array_ensure(JSContext *ctx, JSObject *a, uint32_t cap);
void      js_array_to_slow(JSContext *ctx, JSObject *a);
JSValue   js_array_from_list(JSContext *ctx, const JSValue *v, uint32_t n);
int64_t   js_length_of(JSContext *ctx, JSValue obj);      /* LengthOfArrayLike, -1 exc */
/* conversions */
JSValue   js_to_primitive(JSContext *ctx, JSValue v, int hint);   /* 0 default, 1 number, 2 string */
double    js_tonum(JSContext *ctx, JSValue v, bool *exc);
JSValue   js_tonumeric(JSContext *ctx, JSValue v);
JSValue   js_tostr(JSContext *ctx, JSValue v);                     /* string or EXCEPTION */
JSValue   js_toobj(JSContext *ctx, JSValue v);
int       js_toint32_e(JSContext *ctx, JSValue v, int32_t *out);
int       js_toindex(JSContext *ctx, JSValue v, double *out);
double    js_tointeger(JSContext *ctx, JSValue v, bool *exc);      /* ToIntegerOrInfinity */
int32_t   js_d2i32(double d);
uint32_t  js_d2u32(double d);
bool      js_is_callable(JSValue v);
bool      js_is_ctor(JSValue v);
int       js_is_array_v(JSContext *ctx, JSValue v);                /* IsArray (proxies), -1 exc */
bool      js_same_value(JSValue a, JSValue b, bool zero);           /* SameValue / SameValueZero */
bool      js_strict_eq(JSContext *ctx, JSValue a, JSValue b);
int       js_loose_eq(JSContext *ctx, JSValue a, JSValue b);       /* -1 exc */
int       js_less(JSContext *ctx, JSValue a, JSValue b, bool left_first);   /* 1 true, 0 false/undefined, -1 exc */
JSValue   js_add(JSContext *ctx, JSValue a, JSValue b);
JSValue   js_typeof(JSContext *ctx, JSValue v);
int       js_instanceof(JSContext *ctx, JSValue v, JSValue ctor);
int       js_ordinary_has_instance(JSContext *ctx, JSValue ctor, JSValue v);
JSValue   js_species_ctor(JSContext *ctx, JSValue obj, JSObject *def);
JSValue   js_get_method(JSContext *ctx, JSValue v, JSValue key);   /* undefined if nullish, exc if not callable */
JSValue   js_invoke(JSContext *ctx, JSValue obj, JSValue key, int argc, JSValue *argv);
const char *js_class_name(int cls);
/* errors */
JSValue   js_throw_type(JSContext *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
JSValue   js_throw_range(JSContext *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
JSValue   js_throw_ref(JSContext *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
JSValue   js_throw_syntax(JSContext *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
JSValue   js_throw_oom(JSContext *ctx);
const char *js_error_type_name(int t);
JSValue   js_make_error(JSContext *ctx, int type, JSValue msg);
void      js_error_add_stack(JSContext *ctx, JSValue err);
void      js_value_desc(JSContext *ctx, JSValue v, char *out, size_t size);   /* short description for messages */
/* host classes */
void      js_host_finalize(JSContext *ctx, JSObject *o);

/* ==== js_vm.c ========================================================================================= */
JSValue   js_call_internal(JSContext *ctx, JSValue fn, JSValue this_val, int argc, JSValue *argv, JSValue new_target, int flags);
JSValue   js_callv(JSContext *ctx, JSValue fn, JSValue this_val, int argc, JSValue *argv);
JSValue   js_new_closure(JSContext *ctx, JSBytecode *b, JSVarRef **vr, JSObject *home);
JSValue   js_run_bytecode_top(JSContext *ctx, JSBytecode *b);   /* sync: run a script function */
void      js_enqueue(JSContext *ctx, JSValue fn, int argc, const JSValue *argv);
void      js_enqueue_reaction(JSContext *ctx, JSValue handler, JSValue arg, JSValue resolve, JSValue reject, bool rejected);
void      js_enqueue_thenable(JSContext *ctx, JSValue then, JSValue thenable, JSValue resolve, JSValue reject);
JSValue   js_construct_internal(JSContext *ctx, JSValue fn, int argc, JSValue *argv, JSValue new_target);
void      js_vm_mark(JSContext *ctx);
int       js_drive(JSContext *ctx);
JSValue   js_gen_resume(JSContext *ctx, JSObject *g, JSValue v, int mode, bool *done);
void      js_gen_mark(JSContext *ctx, JSGen *g);
void      js_gen_free(JSContext *ctx, JSGen *g);
int       js_line_of(JSBytecode *b, uint32_t pc);
void      js_stack_trace(JSContext *ctx, JSSB *sb);
bool      js_check_stack(JSContext *ctx);
JSValue   js_async_func_start(JSContext *ctx, JSObject *fn, JSValue this_val, int argc, JSValue *argv);
void      js_async_gen_resume(JSContext *ctx, JSObject *go, JSValue v, bool threw);
JSValue   js_async_gen_run(JSContext *ctx, JSObject *go, JSValue v, int mode, bool *done, bool *awaiting);
void      js_async_gen_step_done(JSContext *ctx, JSObject *go, JSValue r, bool threw, bool done, bool awaiting);

/* ==== compiler (js_parse.c / js_comp.c) ==================================================================== */
/* compile source (UTF-16) to a script function; NULL + SyntaxError on failure */
JSBytecode *js_compile(JSContext *ctx, JSValue src, JSValue name, int line, uint32_t fflags);
/* new Function(params, body) */
JSBytecode *js_compile_function(JSContext *ctx, JSValue params, JSValue body, uint32_t kind);
void      js_bytecode_free(JSContext *ctx, JSBytecode *b);
void      js_bytecode_mark(JSContext *ctx, JSBytecode *b);

/* ==== iterators (js_lib_arr.c) ========================================================================== */
JSValue   js_get_iterator(JSContext *ctx, JSValue obj, JSValue *next, bool async);
JSValue   js_iter_step(JSContext *ctx, JSValue it, JSValue next, bool *done);   /* -> value */
int       js_iter_close(JSContext *ctx, JSValue it, bool throwing);
JSValue   js_iter_result(JSContext *ctx, JSValue v, bool done);
JSValue   js_iterable_to_list(JSContext *ctx, JSValue v);    /* array */
JSValue   js_new_iter(JSContext *ctx, JSObject *proto, JSValue target, int type, int kind);
void      js_map_mark(JSContext *ctx, JSMap *m);
void      js_map_free(JSContext *ctx, JSMap *m);
void      js_map_sweep_weak(JSContext *ctx, JSMap *m);

/* ==== promises (js_lib_coll.c) ============================================================================== */
JSValue   js_promise_new(JSContext *ctx, JSValue *resolve, JSValue *reject);
JSValue   js_promise_resolve_value(JSContext *ctx, JSValue v);     /* PromiseResolve(%Promise%, v) */
/* ok / fail called with the value (js_func_data() = data); both NULL: data is an async function / generator to resume */
void      js_promise_then_native(JSContext *ctx, JSValue p, JSNativeFn ok, JSNativeFn fail, JSValue data);
void      js_promise_mark(JSContext *ctx, void *p);
void      js_promise_free(JSContext *ctx, void *p);
void      js_promise_report_unhandled(JSContext *ctx);

/* ==== regexp (js_re.c) ========================================================================================= */
typedef struct JSRegExp JSRegExp;
JSRegExp *js_re_compile(JSContext *ctx, const uint16_t *pat, uint32_t plen, uint32_t flags, char *err, int errsize);
void      js_re_free(JSContext *ctx, JSRegExp *re);
int       js_re_ncaps(JSRegExp *re);
uint32_t  js_re_get_flags(JSRegExp *re);
void      js_re_set_flags(JSRegExp *re, uint32_t flags);
JSValue   js_re_group_names(JSContext *ctx, JSRegExp *re);    /* array of names (or undefined per group) */
/* match at index; caps[2*ncaps] filled (-1 = unmatched); 1 match, 0 none, -1 error (stack / exception) */
int       js_re_exec(JSContext *ctx, JSRegExp *re, const uint16_t *s, uint32_t len, uint32_t start, int32_t *caps);
#define RE_G 1
#define RE_I 2
#define RE_M 4
#define RE_S 8
#define RE_U 16
#define RE_Y 32
#define RE_D 64
#define RE_V 128
typedef struct JSRegExpObj { JSRegExp *re; JSValue source; uint32_t flags; } JSRegExpObj;
JSValue   js_regexp_create(JSContext *ctx, JSValue pattern, JSValue flags);
JSValue   js_regexp_exec_v(JSContext *ctx, JSValue rx, JSValue s);

/* ==== library setup ============================================================================================= */
void js_init_lib_obj(JSContext *ctx);
void js_init_lib_arr(JSContext *ctx);
void js_init_lib_str(JSContext *ctx);
void js_init_lib_num(JSContext *ctx);
void js_init_lib_json(JSContext *ctx);
void js_init_lib_coll(JSContext *ctx);
void js_init_lib_misc(JSContext *ctx);
JSObject *js_new_ctor(JSContext *ctx, const char *name, JSNativeFn fn, int len, JSObject *proto, int magic);
typedef struct JSFuncDef { const char *name; JSNativeFn fn; int16_t len, magic; } JSFuncDef;
void js_def_funcs(JSContext *ctx, JSValue obj, const JSFuncDef *d, int n);
JSValue js_json_quote(JSContext *ctx, JSValue s);
double js_now(JSContext *ctx);
JSValue js_date_to_string(JSContext *ctx, double t, int kind);
double  js_random(JSContext *ctx);
JSValue js_error_to_string_v(JSContext *ctx, JSValue e);

#define THIS_OBJ(cls) (js_is_cls(this_val, cls) ? OBJ(this_val) : NULL)

#endif
