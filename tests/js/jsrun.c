/*
 * jsrun.c - host test runner for NovaJS (the engine in apps/js).
 *
 *   make jstest                    build and run every test in tests/js
 *   build-jstest/jsrun FILE.js...  run some (harness.js is loaded first)
 *   build-jstest/jsrun -e 'code'   evaluate and print the result
 *
 * Each test file prints through print() / console.log and reports failures
 * with the harness's assert helpers; the runner exits non-zero if any test
 * file failed, threw, or left __fails > 0. setTimeout uses a fake clock: when
 * the script and its jobs are done, timers run in time order.
 */
#include "../../apps/js/js.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_errors;

static void host_log(void *ud, int level, const char *msg) {
    (void)ud;
    if (level == JS_LOG_ERROR) { fprintf(stdout, "ERROR: %s\n", msg); g_errors++; }
    else fprintf(stdout, "%s\n", msg);
}

static double now_ms(void *ud) {
    (void)ud;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double mono_ms(void *ud) {
    (void)ud;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static JSValue fn_print(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        char *s = js_to_cstring(ctx, argv[i], NULL);
        if (!s) return JS_EXCEPTION;
        fputs(s, stdout);
        if (i + 1 < argc) fputc(' ', stdout);
        free(s);
    }
    fputc('\n', stdout);
    if (magic == 1) g_errors++;
    return JS_UNDEFINED;
}

/* fake timers */
typedef struct Timer { int root; double at; int id; bool interval; double every; } Timer;
static Timer timers[4096];
static int ntimers, next_id = 1;
static double fake_now;

static JSValue fn_set_timeout(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val;
    if (ntimers >= 4096 || argc < 1) return js_new_string(ctx, "0");
    double d = argc > 1 ? js_to_number(ctx, argv[1], NULL) : 0;
    if (!(d >= 0)) d = 0;
    Timer *t = &timers[ntimers++];
    t->root = js_root_add(ctx, argv[0]);
    t->at = fake_now + d;
    t->id = next_id++;
    t->interval = magic == 1;
    t->every = d < 1 ? 1 : d;
    return js_mknum(t->id);
}

static JSValue fn_clear_timeout(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)magic;
    if (argc < 1) return JS_UNDEFINED;
    int id = (int)js_to_number(ctx, argv[0], NULL);
    for (int i = 0; i < ntimers; i++)
        if (timers[i].id == id) { js_root_free(ctx, timers[i].root); timers[i] = timers[--ntimers]; break; }
    return JS_UNDEFINED;
}

static int drive(JSContext *ctx, int st) {
    while (st == JS_SUSPENDED) st = js_resume(ctx);
    return st;
}

static void run_timers(JSContext *ctx) {
    int guard = 0;
    while (ntimers && guard++ < 100000) {
        int best = 0;
        for (int i = 1; i < ntimers; i++) if (timers[i].at < timers[best].at || (timers[i].at == timers[best].at && timers[i].id < timers[best].id)) best = i;
        Timer t = timers[best];
        fake_now = t.at;
        JSValue fn = js_root_get(ctx, t.root);
        if (t.interval) timers[best].at += t.every;
        else { timers[best] = timers[--ntimers]; }
        drive(ctx, js_call_task(ctx, fn, JS_UNDEFINED, 0, NULL));
        if (!t.interval) js_root_free(ctx, t.root);
    }
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t r = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[r] = 0;
    *len = r;
    return b;
}

static JSContext *make_ctx(void) {
    volatile int top_marker = 0;
    js_host_t h;
    memset(&h, 0, sizeof h);
    h.log = host_log;
    h.now_ms = now_ms;
    h.mono_ms = mono_ms;
    h.mem_limit = 512u << 20;
    h.slice_ms = 50;
    h.nested_limit_ms = 20000;
    h.stack_top = (uintptr_t)&top_marker + 4096;
    JSContext *ctx = js_new_context(&h);
    if (!ctx) return NULL;
    JSValue g = js_global(ctx);
    js_def_func(ctx, g, "print", fn_print, 1, 0);
    JSValue con = js_new_object(ctx);
    js_def_func(ctx, con, "log", fn_print, 1, 0);
    js_def_func(ctx, con, "info", fn_print, 1, 0);
    js_def_func(ctx, con, "warn", fn_print, 1, 0);
    js_def_func(ctx, con, "error", fn_print, 1, 1);
    js_def_value(ctx, g, "console", con);
    js_def_func(ctx, g, "setTimeout", fn_set_timeout, 2, 0);
    js_def_func(ctx, g, "setInterval", fn_set_timeout, 2, 1);
    js_def_func(ctx, g, "clearTimeout", fn_clear_timeout, 1, 0);
    js_def_func(ctx, g, "clearInterval", fn_clear_timeout, 1, 0);
    return ctx;
}

static int run_file(const char *harness, const char *path) {
    JSContext *ctx = make_ctx();
    if (!ctx) { printf("FAIL %s: no context\n", path); return 1; }
    ntimers = 0;
    fake_now = 0;
    g_errors = 0;
    int bad = 0;
    if (harness) {
        size_t hl;
        char *h = read_file(harness, &hl);
        if (h) { if (drive(ctx, js_eval_task(ctx, h, hl, "harness.js", 1)) != JS_DONE) bad = 1; free(h); }
    }
    size_t len;
    char *src = read_file(path, &len);
    if (!src) { printf("FAIL %s: cannot read\n", path); js_free_context(ctx); return 1; }
    double t0 = mono_ms(NULL);
    int st = drive(ctx, js_eval_task(ctx, src, len, path, 1));
    free(src);
    if (st != JS_DONE) bad = 1;
    run_timers(ctx);
    JSValue fails = js_get(ctx, js_global(ctx), "__fails");
    int nf = JS_IsNumber(fails) ? (int)js_num(fails) : 0;
    if (nf > 0 || g_errors) bad = 1;
    double ms = mono_ms(NULL) - t0;
    JSValue passes = js_get(ctx, js_global(ctx), "__passes");
    printf("%s %s (%d checks, %.0f ms, %u KB, %u GCs)\n", bad ? "FAIL" : "ok  ", path,
           JS_IsNumber(passes) ? (int)js_num(passes) : 0, ms, (unsigned)(js_mem_used(ctx) / 1024), js_gc_count(ctx));
    js_free_context(ctx);
    return bad;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
#ifdef JSRUN_X87
    /* like the kernel (kernel/arch/cpu.c): x87 rounds to 53 bits */
    unsigned short cw = 0x027F;
    __asm__ volatile("fldcw %0" : : "m"(cw));
#endif
    if (argc >= 3 && !strcmp(argv[1], "-e")) {
        JSContext *ctx = make_ctx();
        JSValue r = js_eval(ctx, argv[2], strlen(argv[2]), "eval");
        if (JS_IsException(r)) { js_report_exception(ctx); js_free_context(ctx); return 1; }
        js_run_jobs(ctx);
        run_timers(ctx);
        char *s = js_to_cstring(ctx, r, NULL);
        printf("%s\n", s ? s : "?");
        free(s);
        js_free_context(ctx);
        return 0;
    }
    const char *harness = NULL;
    int fails = 0, n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") && i + 1 < argc) { harness = argv[++i]; continue; }
        fails += run_file(harness, argv[i]);
        n++;
    }
    printf("%d of %d test files passed\n", n - fails, n);
    return fails ? 1 : 0;
}
