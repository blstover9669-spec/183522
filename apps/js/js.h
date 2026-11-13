/*
 * js.h - NovaJS, NovaOS's own JavaScript engine (written from scratch).
 *
 * Portable C: builds inside the kernel (freestanding, i386) and on a host with
 * gcc (tests/js). Pipeline: tokenizer -> parser (AST) -> scope analysis ->
 * bytecode -> stack VM. Mark-and-sweep GC with precise VM roots plus a
 * conservative scan of the C stack, a per-context memory cap, and an
 * instruction budget so a script never freezes the event loop:
 *
 *   js_eval_task / js_call_task start a task (a script, an event handler, a
 *   timer callback). They return JS_DONE, JS_THROWN (uncaught exception, already
 *   reported through host->log), or JS_SUSPENDED: the budget ran out and the
 *   task waits; call js_resume() from a later main-loop tick until it is not
 *   JS_SUSPENDED any more. Microtasks (promise jobs) run after every task.
 *
 * Values are NaN-boxed 64-bit words. Embedders keep values alive across ticks
 * with js_root_add (or by storing them in rooted objects).
 */
#ifndef NOVA_JS_H
#define NOVA_JS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint64_t JSValue;
typedef struct JSContext JSContext;

#define JS_TAG_SPECIAL  0xFFF9u
#define JS_TAG_STRING   0xFFFAu
#define JS_TAG_OBJECT   0xFFFBu
#define JS_TAG_SYMBOL   0xFFFCu
#define JS_TAG_BIGINT   0xFFFDu
#define JS_TAG_INTERNAL 0xFFFEu
#define JS_TAG_MARKER   0xFFFFu

#define JS_MKTAG(t, p)  (((uint64_t)(t) << 48) | (uint64_t)(p))
#define JS_TAGOF(v)     ((uint32_t)((v) >> 48))
#define JS_UNDEFINED    JS_MKTAG(JS_TAG_SPECIAL, 0)
#define JS_NULL         JS_MKTAG(JS_TAG_SPECIAL, 1)
#define JS_FALSE        JS_MKTAG(JS_TAG_SPECIAL, 2)
#define JS_TRUE         JS_MKTAG(JS_TAG_SPECIAL, 3)
#define JS_HOLE         JS_MKTAG(JS_TAG_SPECIAL, 4)   /* array hole / "not found" (internal) */
#define JS_UNINIT       JS_MKTAG(JS_TAG_SPECIAL, 5)   /* let/const before initialisation (internal) */
#define JS_EXCEPTION    JS_MKTAG(JS_TAG_SPECIAL, 6)   /* a call threw: the exception is pending */
#define JS_NAN_BITS     0x7FF8000000000000ull

#define JS_IsNumber(v)    ((v) < ((uint64_t)JS_TAG_SPECIAL << 48))
#define JS_IsUndefined(v) ((v) == JS_UNDEFINED)
#define JS_IsNull(v)      ((v) == JS_NULL)
#define JS_IsNullish(v)   ((v) == JS_UNDEFINED || (v) == JS_NULL)
#define JS_IsBool(v)      ((v) == JS_TRUE || (v) == JS_FALSE)
#define JS_IsString(v)    (JS_TAGOF(v) == JS_TAG_STRING)
#define JS_IsObject(v)    (JS_TAGOF(v) == JS_TAG_OBJECT)
#define JS_IsSymbol(v)    (JS_TAGOF(v) == JS_TAG_SYMBOL)
#define JS_IsException(v) ((v) == JS_EXCEPTION)
#define JS_PTR(v)         ((void *)(uintptr_t)((v) & 0xFFFFFFFFFFFFull))
#define JS_BOOL(b)        ((b) ? JS_TRUE : JS_FALSE)

static inline double js_num(JSValue v) { union { uint64_t u; double d; } x; x.u = v; return x.d; }
static inline JSValue js_mknum(double d) {
    union { double d; uint64_t u; } x;
    if (d != d) return JS_NAN_BITS;
    x.d = d;
    return x.u;
}

/* ---- the host (embedder) ------------------------------------------------------------- */
enum { JS_LOG_LOG = 0, JS_LOG_INFO, JS_LOG_WARN, JS_LOG_ERROR, JS_LOG_DEBUG };
typedef struct js_host {
    void  (*log)(void *ud, int level, const char *utf8);   /* console.* and uncaught errors */
    double (*now_ms)(void *ud);       /* wall clock, ms since 1970 (Date.now)          */
    double (*mono_ms)(void *ud);      /* monotonic ms (performance.now, time limits)    */
    void  *ud;
    size_t mem_limit;                 /* bytes for this context's heap, 0 = 64 MB        */
    uint32_t slice_ms;                /* run at most this long before yielding (0 = 20)  */
    uint32_t nested_limit_ms;         /* code that cannot yield (inside a native call):
                                         stopped after this long (0 = 3000)              */
    uintptr_t stack_top;              /* highest address of the C stack the embedder's code
                                         runs on (the GC scans it for values held in C);
                                         0 = from the outermost js_* call down           */
} js_host_t;

JSContext *js_new_context(const js_host_t *host);
void       js_free_context(JSContext *ctx);
void      *js_get_host_ud(JSContext *ctx);
JSValue    js_global(JSContext *ctx);

/* ---- tasks ----------------------------------------------------------------------------- */
enum { JS_DONE = 0, JS_THROWN = 1, JS_SUSPENDED = 2, JS_KILLED = 3 };
int  js_eval_task(JSContext *ctx, const char *utf8, size_t len, const char *name, int line);
int  js_call_task(JSContext *ctx, JSValue fn, JSValue this_val, int argc, const JSValue *argv);
int  js_resume(JSContext *ctx);
bool js_busy(JSContext *ctx);              /* a task is suspended (call js_resume)      */
JSValue js_task_value(JSContext *ctx);     /* what the last finished task returned      */
void js_kill(JSContext *ctx);              /* stop the suspended task (runaway script)  */
uint32_t js_task_ms(JSContext *ctx);       /* how long the current task has been running */
/* Natives that wait for the user (alert / confirm / prompt) without blocking the event
   loop: when js_can_wait(ctx) is true, a native may return js_wait(ctx); the task then
   suspends (js_busy and js_waiting are true, js_resume does nothing) until the host
   calls js_resume_with(ctx, result) - the native's return value. When js_can_wait is
   false (the native was called from other native code) it must return at once. */
bool    js_can_wait(JSContext *ctx);
JSValue js_wait(JSContext *ctx);
bool    js_waiting(JSContext *ctx);
int     js_resume_with(JSContext *ctx, JSValue result);
/* Synchronous evaluation (tests / small snippets). Returns the completion value or
   JS_EXCEPTION. Code that runs out of budget here is stopped. */
JSValue js_eval(JSContext *ctx, const char *utf8, size_t len, const char *name);
/* run pending microtasks to completion (synchronously) */
void js_run_jobs(JSContext *ctx);
bool js_has_jobs(JSContext *ctx);

/* ---- exceptions -------------------------------------------------------------------------- */
enum { JS_ERROR = 0, JS_TYPE_ERROR, JS_RANGE_ERROR, JS_REFERENCE_ERROR, JS_SYNTAX_ERROR,
       JS_EVAL_ERROR, JS_URI_ERROR, JS_AGGREGATE_ERROR, JS_NERRORS };
JSValue js_throw(JSContext *ctx, JSValue v);                              /* -> JS_EXCEPTION */
JSValue js_throw_error(JSContext *ctx, int type, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
JSValue js_new_error(JSContext *ctx, int type, const char *msg);
JSValue js_take_exception(JSContext *ctx);       /* the pending exception (cleared)  */
bool    js_has_exception(JSContext *ctx);
/* "Uncaught TypeError: x is not a function (script.js:12)" -> malloc'd UTF-8 */
char   *js_error_text(JSContext *ctx, JSValue exc);
void    js_report_exception(JSContext *ctx);     /* take + log as an uncaught error  */

/* ---- values ------------------------------------------------------------------------------ */
JSValue js_new_string(JSContext *ctx, const char *utf8);
JSValue js_new_string_len(JSContext *ctx, const char *utf8, size_t len);
JSValue js_new_string16(JSContext *ctx, const uint16_t *s, size_t len);
JSValue js_new_string_latin1(JSContext *ctx, const char *s, size_t len);
/* -> malloc'd UTF-8 (NUL-terminated), NULL + exception on error. len may be NULL. */
char   *js_to_cstring(JSContext *ctx, JSValue v, size_t *len);
/* string contents as UTF-16 code units: valid until the next allocation */
const uint16_t *js_string_chars(JSContext *ctx, JSValue s, uint32_t *len);
JSValue js_to_string(JSContext *ctx, JSValue v);
double  js_to_number(JSContext *ctx, JSValue v, bool *exc);   /* exc may be NULL */
int32_t js_to_int32(JSContext *ctx, JSValue v);
bool    js_truthy(JSValue v);
bool    js_is_function(JSContext *ctx, JSValue v);
bool    js_is_array(JSContext *ctx, JSValue v);
bool    js_strict_equals(JSContext *ctx, JSValue a, JSValue b);
const char *js_typeof_name(JSContext *ctx, JSValue v);

/* ---- objects ------------------------------------------------------------------------------ */
JSValue js_new_object(JSContext *ctx);
JSValue js_new_object_proto(JSContext *ctx, JSValue proto);
JSValue js_new_array(JSContext *ctx, uint32_t len);
JSValue js_get(JSContext *ctx, JSValue obj, const char *name);
JSValue js_get_value(JSContext *ctx, JSValue obj, JSValue key);
JSValue js_get_index(JSContext *ctx, JSValue obj, uint32_t i);
int     js_set(JSContext *ctx, JSValue obj, const char *name, JSValue v);          /* -1 = exception */
int     js_set_value(JSContext *ctx, JSValue obj, JSValue key, JSValue v);
int     js_set_index(JSContext *ctx, JSValue obj, uint32_t i, JSValue v);
int     js_define(JSContext *ctx, JSValue obj, const char *name, JSValue v, int flags);   /* JS_PROP_* */
bool    js_has(JSContext *ctx, JSValue obj, const char *name);
int     js_delete(JSContext *ctx, JSValue obj, const char *name);
uint32_t js_array_length(JSContext *ctx, JSValue arr);
int     js_array_push(JSContext *ctx, JSValue arr, JSValue v);
JSValue js_get_prototype(JSContext *ctx, JSValue obj);
int     js_set_prototype(JSContext *ctx, JSValue obj, JSValue proto);
/* own enumerable string keys -> new array (Object.keys) */
JSValue js_object_keys(JSContext *ctx, JSValue obj);
#define JS_PROP_WRITABLE     1
#define JS_PROP_ENUMERABLE   2
#define JS_PROP_CONFIGURABLE 4
#define JS_PROP_DEFAULT      7
#define JS_PROP_HIDDEN       5   /* writable + configurable, not enumerable (methods) */

/* ---- functions ----------------------------------------------------------------------------- */
typedef JSValue (*JSNativeFn)(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic);
JSValue js_new_function(JSContext *ctx, JSNativeFn fn, const char *name, int length, int magic);
/* a native function with a private data value (js_func_data() inside the call) */
JSValue js_new_function_data(JSContext *ctx, JSNativeFn fn, const char *name, int length, int magic, JSValue data);
JSValue js_func_data(JSContext *ctx);           /* data of the running native function */
JSValue js_new_target(JSContext *ctx);          /* new.target of the running native (undefined = plain call) */
void    js_def_func(JSContext *ctx, JSValue obj, const char *name, JSNativeFn fn, int length, int magic);
void    js_def_getset(JSContext *ctx, JSValue obj, const char *name, JSNativeFn get, JSNativeFn set, int magic);
void    js_def_value(JSContext *ctx, JSValue obj, const char *name, JSValue v);   /* hidden + writable */
JSValue js_call(JSContext *ctx, JSValue fn, JSValue this_val, int argc, const JSValue *argv);
JSValue js_construct(JSContext *ctx, JSValue fn, int argc, const JSValue *argv);
#define JS_ARG(i) ((i) < argc ? argv[i] : JS_UNDEFINED)

/* ---- host objects (DOM wrappers etc.) ------------------------------------------------------ */
typedef struct js_class {
    const char *name;
    void (*finalize)(JSContext *ctx, void *opaque);   /* may be NULL                 */
    void (*mark)(JSContext *ctx, void *opaque);       /* mark JSValues the opaque holds (js_mark) */
    /* optional named / indexed properties ("exotic" objects like dataset, localStorage).
       key is a property key (string or symbol). get: 1 = *v set, 0 = not ours (ordinary
       lookup), -1 = exception. set: 1 = handled, 0 = ordinary, -1 = exception.
       del: 1 = deleted, 0 = ordinary. keys: an array of extra own keys or undefined. */
    int (*get)(JSContext *ctx, JSValue obj, JSValue key, JSValue *v);
    int (*set)(JSContext *ctx, JSValue obj, JSValue key, JSValue v);
    int (*del)(JSContext *ctx, JSValue obj, JSValue key);
    JSValue (*keys)(JSContext *ctx, JSValue obj);
} js_class_t;
int     js_new_class(JSContext *ctx, const js_class_t *cls);           /* -> class id (>= 1) */
JSValue js_new_host_object(JSContext *ctx, int class_id, JSValue proto, void *opaque);
void   *js_host_opaque(JSValue v, int class_id);                        /* NULL if not that class */
int     js_host_class(JSValue v);                                       /* 0 = not a host object */
void    js_set_host_opaque(JSValue v, void *opaque);
void    js_mark(JSContext *ctx, JSValue v);                             /* from js_class.mark */
/* pinned objects are never collected (until js_unpin) */
void    js_pin(JSContext *ctx, JSValue v);
void    js_unpin(JSContext *ctx, JSValue v);

/* ---- roots for values kept in C ------------------------------------------------------------- */
int     js_root_add(JSContext *ctx, JSValue v);       /* -> handle (>= 0), -1 out of memory */
JSValue js_root_get(JSContext *ctx, int h);
void    js_root_set(JSContext *ctx, int h, JSValue v);
void    js_root_free(JSContext *ctx, int h);

/* ---- promises (for host APIs like fetch) ---------------------------------------------------- */
/* a new pending promise; *resolve / *reject are functions to call later (root them) */
JSValue js_new_promise(JSContext *ctx, JSValue *resolve, JSValue *reject);
/* queue a host callback as a job (runs at the next job checkpoint) */
void    js_enqueue_job(JSContext *ctx, JSValue fn, int argc, const JSValue *argv);

/* ---- typed data ------------------------------------------------------------------------------ */
JSValue  js_new_uint8array(JSContext *ctx, const uint8_t *data, size_t len);   /* copies */
JSValue  js_new_arraybuffer(JSContext *ctx, const uint8_t *data, size_t len);
/* bytes of an ArrayBuffer / typed array / DataView (NULL if not one) */
uint8_t *js_get_bytes(JSContext *ctx, JSValue v, size_t *len);

/* ---- JSON ------------------------------------------------------------------------------------- */
JSValue js_json_parse(JSContext *ctx, const char *utf8, size_t len);
JSValue js_json_stringify(JSContext *ctx, JSValue v);    /* string or undefined */

/* ---- memory / statistics ---------------------------------------------------------------------- */
void   js_gc(JSContext *ctx);
size_t js_mem_used(JSContext *ctx);
uint32_t js_gc_count(JSContext *ctx);

#endif
