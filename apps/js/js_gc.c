/*
 * js_gc.c - NovaJS memory: a size-class cell allocator, mark & sweep GC,
 * root handles, pinned objects and the per-context memory cap.
 *
 * Small cells (<= 512 bytes) live in 32 KB pages of one size class; bigger
 * ones are malloc'd on their own. Every page / large cell is a "segment" in a
 * sorted table, so any machine word can be checked for "points into a live
 * cell". That makes the conservative scan of the C stack possible: native
 * code may hold values in plain C variables across allocations. The VM stack
 * is scanned the same validated way up to its high-water mark, the frames,
 * realm, atoms, jobs, roots and pinned objects precisely.
 */
#include "jsi.h"

#define NSC 18
static const uint16_t sc_size[NSC] = { 16, 24, 32, 40, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512 };
#define PAGE_BYTES 32768

struct GCPage { uint8_t *mem; uint32_t cell, ncells, used; GCPage *next; uint8_t sc; };

static int size_class(size_t n) {
    for (int i = 0; i < NSC; i++) if (n <= sc_size[i]) return i;
    return -1;
}

/* ---- segments (sorted by address) ------------------------------------------------------- */
static int seg_find(JSContext *ctx, uintptr_t p) {
    int lo = 0, hi = ctx->nsegs - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        GCSeg *s = &ctx->segs[m];
        if (p < s->start) hi = m - 1;
        else if (p >= s->end) lo = m + 1;
        else return m;
    }
    return -1;
}

static bool seg_add(JSContext *ctx, uintptr_t start, uintptr_t end, GCPage *pg) {
    if (ctx->nsegs >= ctx->segcap) {
        int nc = ctx->segcap ? ctx->segcap * 2 : 256;
        GCSeg *n = realloc(ctx->segs, sizeof *n * (size_t)nc);
        if (!n) return false;
        ctx->segs = n;
        ctx->segcap = nc;
    }
    int lo = 0, hi = ctx->nsegs;
    while (lo < hi) { int m = (lo + hi) >> 1; if (ctx->segs[m].start < start) lo = m + 1; else hi = m; }
    memmove(&ctx->segs[lo + 1], &ctx->segs[lo], sizeof(GCSeg) * (size_t)(ctx->nsegs - lo));
    ctx->segs[lo].start = start;
    ctx->segs[lo].end = end;
    ctx->segs[lo].page = pg;
    ctx->nsegs++;
    return true;
}

static void seg_remove(JSContext *ctx, uintptr_t start) {
    int i = seg_find(ctx, start);
    if (i < 0) return;
    memmove(&ctx->segs[i], &ctx->segs[i + 1], sizeof(GCSeg) * (size_t)(ctx->nsegs - i - 1));
    ctx->nsegs--;
}

/* the live cell containing address p, or NULL */
static GCHdr *cell_at(JSContext *ctx, uintptr_t p) {
    int i = seg_find(ctx, p);
    if (i < 0) return NULL;
    GCSeg *s = &ctx->segs[i];
    GCHdr *h;
    if (s->page) {
        GCPage *pg = s->page;
        uint32_t idx = (uint32_t)((p - (uintptr_t)pg->mem) / pg->cell);
        if (idx >= pg->ncells) return NULL;
        h = (GCHdr *)(pg->mem + (size_t)idx * pg->cell);
    } else h = (GCHdr *)s->start;
    return h->type == T_FREE ? NULL : h;
}

/* ---- accounting ---------------------------------------------------------------------------- */
static void collect(JSContext *ctx);

static bool reserve(JSContext *ctx, size_t n) {
    if (ctx->in_gc) { ctx->mem_used += n; return true; }
    if (ctx->alloc_since + n > ctx->gc_threshold || ctx->mem_used + n > ctx->mem_limit) collect(ctx);
    if (ctx->mem_used + n > ctx->mem_limit) {
        ctx->oom = true;
        return false;
    }
    ctx->mem_used += n;
    ctx->alloc_since += n;
    return true;
}

void *js_malloc(JSContext *ctx, size_t n) {
    if (!n) n = 1;
    if (!reserve(ctx, n)) { js_throw_oom(ctx); return NULL; }
    void *p = malloc(n);
    if (!p) { ctx->mem_used -= n; js_throw_oom(ctx); }
    return p;
}

void *js_mallocz(JSContext *ctx, size_t n) {
    void *p = js_malloc(ctx, n);
    if (p) memset(p, 0, n);
    return p;
}

void *js_realloc(JSContext *ctx, void *p, size_t old, size_t n) {
    if (!p) return js_malloc(ctx, n);
    if (n > old && !reserve(ctx, n - old)) { js_throw_oom(ctx); return NULL; }
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        if (n > old) ctx->mem_used -= n - old;
        js_throw_oom(ctx);
        return NULL;
    }
    if (n < old) ctx->mem_used -= old - n;
    return q;
}

void js_mfree(JSContext *ctx, void *p, size_t n) {
    if (!p) return;
    free(p);
    ctx->mem_used = ctx->mem_used >= n ? ctx->mem_used - n : 0;
}

/* ---- cells ------------------------------------------------------------------------------------- */
static bool new_page(JSContext *ctx, int sc) {
    GCPage *pg = malloc(sizeof *pg);
    uint8_t *mem = malloc(PAGE_BYTES);
    if (!pg || !mem) { free(pg); free(mem); return false; }
    pg->mem = mem;
    pg->cell = sc_size[sc];
    pg->ncells = PAGE_BYTES / pg->cell;
    pg->used = 0;
    pg->sc = (uint8_t)sc;
    if (!seg_add(ctx, (uintptr_t)mem, (uintptr_t)mem + (size_t)pg->ncells * pg->cell, pg)) { free(pg); free(mem); return false; }
    pg->next = ctx->pages[sc];
    ctx->pages[sc] = pg;
    /* thread the cells onto the free list (in address order) */
    void *head = ctx->free_cells[sc];
    for (int i = (int)pg->ncells - 1; i >= 0; i--) {
        GCHdr *h = (GCHdr *)(mem + (size_t)i * pg->cell);
        h->type = T_FREE;
        h->mark = 0;
        h->aux = (uint32_t)sc;
        *(void **)(h + 1) = head;
        head = h;
    }
    ctx->free_cells[sc] = head;
    ctx->mem_used += PAGE_BYTES;
    return true;
}

void *js_cell(JSContext *ctx, int type, size_t size) {
    int sc = size_class(size);
    GCHdr *h;
    if (sc >= 0) {
        size_t cs = sc_size[sc];
        if (!ctx->in_gc && ctx->alloc_since + cs > ctx->gc_threshold) collect(ctx);
        if (!ctx->free_cells[sc]) {
            if (ctx->mem_used + PAGE_BYTES > ctx->mem_limit && !ctx->in_gc) {
                collect(ctx);
                if (!ctx->free_cells[sc] && ctx->mem_used + PAGE_BYTES > ctx->mem_limit) {
                    ctx->oom = true;
                    js_throw_oom(ctx);
                    return NULL;
                }
            }
            if (!ctx->free_cells[sc] && !new_page(ctx, sc)) { js_throw_oom(ctx); return NULL; }
        }
        h = ctx->free_cells[sc];
        ctx->free_cells[sc] = *(void **)(h + 1);
        memset(h, 0, cs);
        h->aux = (uint32_t)sc;
        ctx->alloc_since += cs;
    } else {
        if (!reserve(ctx, size + 16)) { js_throw_oom(ctx); return NULL; }
        h = malloc(size);
        if (!h) { ctx->mem_used -= size + 16; js_throw_oom(ctx); return NULL; }
        memset(h, 0, size);
        if (ctx->nlarge >= ctx->largecap) {
            int nc = ctx->largecap ? ctx->largecap * 2 : 64;
            GCHdr **n = realloc(ctx->large, sizeof *n * (size_t)nc);
            if (!n) { free(h); js_throw_oom(ctx); return NULL; }
            ctx->large = n;
            ctx->largecap = nc;
        }
        if (!seg_add(ctx, (uintptr_t)h, (uintptr_t)h + size, NULL)) { free(h); js_throw_oom(ctx); return NULL; }
        ctx->large[ctx->nlarge++] = h;
        h->aux = 0xFFFFFFFFu;     /* large: the size is in the segment table */
    }
    h->type = (uint8_t)type;
    return h;
}

static size_t large_size(JSContext *ctx, GCHdr *h) {
    int i = seg_find(ctx, (uintptr_t)h);
    return i >= 0 ? (size_t)(ctx->segs[i].end - ctx->segs[i].start) : 0;
}

/* ---- marking ------------------------------------------------------------------------------------ */
static void push_mark(JSContext *ctx, GCHdr *h) {
    if (ctx->msp >= ctx->mcap) {
        int nc = ctx->mcap ? ctx->mcap * 2 : 4096;
        JSValue *n = realloc(ctx->mark_stack, sizeof *n * (size_t)nc);
        if (!n) return;     /* (cells stay unscanned: better than a crash) */
        ctx->mark_stack = n;
        ctx->mcap = nc;
    }
    ctx->mark_stack[ctx->msp++] = (JSValue)(uintptr_t)h;
}

void js_gc_mark_cell(JSContext *ctx, void *cell) {
    GCHdr *h = cell;
    if (!h || h->mark || h->type == T_FREE) return;
    h->mark = 1;
    push_mark(ctx, h);
}

void js_gc_mark_value(JSContext *ctx, JSValue v) {
    uint32_t t = JS_TAGOF(v);
    if (t >= JS_TAG_STRING && t <= JS_TAG_INTERNAL) js_gc_mark_cell(ctx, JS_PTR(v));
}

void js_mark(JSContext *ctx, JSValue v) { js_gc_mark_value(ctx, v); }

/* a value that may be stale: only marked when it points at a live cell */
static void mark_checked(JSContext *ctx, JSValue v) {
    uint32_t t = JS_TAGOF(v);
    if (t < JS_TAG_STRING || t > JS_TAG_INTERNAL) return;
    GCHdr *h = cell_at(ctx, (uintptr_t)JS_PTR(v));
    if (h && h == JS_PTR(v)) js_gc_mark_cell(ctx, h);
}

static void mark_word(JSContext *ctx, uintptr_t w) {
    GCHdr *h = cell_at(ctx, w);
    if (h) js_gc_mark_cell(ctx, h);
#if UINTPTR_MAX > 0xFFFFFFFFu
    uintptr_t w2 = w & 0xFFFFFFFFFFFFull;
    if (w2 != w && (h = cell_at(ctx, w2))) js_gc_mark_cell(ctx, h);
#endif
}

static void mark_props(JSContext *ctx, JSObject *o) {
    for (uint32_t i = 0; i < o->nprops; i++) {
        js_gc_mark_value(ctx, o->props[i].key);
        js_gc_mark_value(ctx, o->props[i].val);
    }
}

static void mark_object(JSContext *ctx, JSObject *o) {
    if (o->proto) js_gc_mark_cell(ctx, o->proto);
    if (o->h.flags & OF_LAZY) { /* nothing extra */ }
    mark_props(ctx, o);
    switch (o->h.cls) {
    case CL_ARRAY: case CL_ARGUMENTS:
        for (uint32_t i = 0; i < o->u.a.len; i++) js_gc_mark_value(ctx, o->u.a.v[i]);
        break;
    case CL_FUNCTION:
        js_gc_mark_cell(ctx, o->u.f.b);
        if (o->u.f.vr && o->u.f.b) for (int i = 0; i < o->u.f.b->ncv; i++) if (o->u.f.vr[i]) js_gc_mark_cell(ctx, o->u.f.vr[i]);
        if (o->u.f.home) js_gc_mark_cell(ctx, o->u.f.home);
        if (o->u.f.fields) js_gc_mark_cell(ctx, o->u.f.fields);
        break;
    case CL_NATIVE:
        js_gc_mark_value(ctx, o->u.n.data);
        js_gc_mark_value(ctx, o->u.n.name);
        break;
    case CL_BOUND:
        js_gc_mark_value(ctx, o->u.bf.target);
        js_gc_mark_value(ctx, o->u.bf.this_val);
        for (int i = 0; i < o->u.bf.argc; i++) js_gc_mark_value(ctx, o->u.bf.args[i]);
        break;
    case CL_BOOLEAN: case CL_NUMBER: case CL_STRING: case CL_SYMBOL: case CL_DATE: case CL_BIGINT:
        js_gc_mark_value(ctx, o->u.prim);
        break;
    case CL_REGEXP: {
        JSRegExpObj *r = o->u.p;
        if (r) js_gc_mark_value(ctx, r->source);
        break;
    }
    case CL_MAP: case CL_SET:
        if (o->u.p) js_map_mark(ctx, o->u.p);
        break;
    case CL_WEAKMAP: case CL_WEAKSET:
        if (ctx->nweak < 4096) {
            if (ctx->nweak >= ctx->weakcap) {
                int nc = ctx->weakcap ? ctx->weakcap * 2 : 16;
                JSObject **n = realloc(ctx->weaks, sizeof *n * (size_t)nc);
                if (n) { ctx->weaks = n; ctx->weakcap = nc; }
            }
            if (ctx->nweak < ctx->weakcap) ctx->weaks[ctx->nweak++] = o;
        }
        break;
    case CL_WEAKREF:
        if (ctx->nwrefs >= ctx->wrefcap) {
            int nc = ctx->wrefcap ? ctx->wrefcap * 2 : 16;
            JSObject **n = realloc(ctx->wrefs, sizeof *n * (size_t)nc);
            if (n) { ctx->wrefs = n; ctx->wrefcap = nc; }
        }
        if (ctx->nwrefs < ctx->wrefcap) ctx->wrefs[ctx->nwrefs++] = o;
        else js_gc_mark_value(ctx, o->u.prim);
        break;
    case CL_PROMISE:
        if (o->u.p) js_promise_mark(ctx, o->u.p);
        break;
    case CL_GENERATOR: case CL_ASYNC_GEN: case CL_ASYNC:
        if (o->u.p) js_gen_mark(ctx, o->u.p);
        break;
    case CL_TYPEDARRAY: case CL_DATAVIEW: {
        JSTypedArray *ta = o->u.p;
        if (ta && ta->buf) js_gc_mark_cell(ctx, ta->buf);
        break;
    }
    case CL_PROXY:
        js_gc_mark_value(ctx, o->u.px.target);
        js_gc_mark_value(ctx, o->u.px.handler);
        break;
    case CL_ITER: case CL_FORIN: {
        JSIter *it = o->u.p;
        if (it) { js_gc_mark_value(ctx, it->target); js_gc_mark_value(ctx, it->extra); }
        break;
    }
    case CL_HOST: {
        int c = o->u.host.cls;
        if (c > 0 && c <= ctx->nclasses && ctx->classes[c - 1].c.mark && o->u.host.opaque)
            ctx->classes[c - 1].c.mark(ctx, o->u.host.opaque);
        break;
    }
    default:
        break;
    }
}

static void scan_cell(JSContext *ctx, GCHdr *h) {
    switch (h->type) {
    case T_OBJECT: mark_object(ctx, (JSObject *)h); break;
    case T_ROPE: {
        JSRope *r = (JSRope *)h;
        js_gc_mark_value(ctx, r->left);
        js_gc_mark_value(ctx, r->right);
        if (r->flat) js_gc_mark_cell(ctx, r->flat);
        break;
    }
    case T_SYMBOL: js_gc_mark_value(ctx, ((JSSymbol *)h)->desc); break;
    case T_BYTECODE: js_bytecode_mark(ctx, (JSBytecode *)h); break;
    case T_VARREF: js_gc_mark_value(ctx, ((JSVarRef *)h)->v); break;
    case T_ACCESSOR: js_gc_mark_value(ctx, ((JSAccessor *)h)->get); js_gc_mark_value(ctx, ((JSAccessor *)h)->set); break;
    default: break;
    }
}

static void drain(JSContext *ctx) {
    while (ctx->msp > 0) {
        GCHdr *h = (GCHdr *)(uintptr_t)ctx->mark_stack[--ctx->msp];
        scan_cell(ctx, h);
    }
}

static void __attribute__((noinline)) scan_c_stack(JSContext *ctx) {
    uintptr_t top = ctx->host.stack_top ? ctx->host.stack_top : ctx->stack_base;
    if (!top) return;
    __builtin_unwind_init();
    volatile uintptr_t here = 0;
    uintptr_t p = (uintptr_t)&here & ~(uintptr_t)(sizeof(uintptr_t) - 1);
    p -= 64 * sizeof(uintptr_t);   /* (registers spilled just below) */
    for (; p + sizeof(uintptr_t) <= top; p += sizeof(uintptr_t)) mark_word(ctx, *(volatile uintptr_t *)p);
}

static void mark_roots(JSContext *ctx) {
    /* the VM stack: everything up to the high-water mark, validated */
    JSValue *hw = ctx->stack_hw > ctx->sp ? ctx->stack_hw : ctx->sp;
    if (ctx->stack) for (JSValue *v = ctx->stack; v < hw && v < ctx->stack_end; v++) mark_checked(ctx, *v);
    js_vm_mark(ctx);
    for (int i = 0; i < A_COUNT; i++) js_gc_mark_value(ctx, ctx->a[i]);
    for (int i = 0; i < WK_COUNT; i++) js_gc_mark_value(ctx, ctx->wk[i]);
    js_gc_mark_value(ctx, ctx->empty_str);
    js_gc_mark_value(ctx, ctx->symreg);
    js_gc_mark_value(ctx, ctx->global);
    js_gc_mark_value(ctx, ctx->global_lex);
    js_gc_mark_value(ctx, ctx->exception);
    js_gc_mark_value(ctx, ctx->native_data);
    js_gc_mark_value(ctx, ctx->native_nt);
    js_gc_mark_value(ctx, ctx->throw_type_error);
    js_gc_mark_value(ctx, ctx->array_values);
    js_gc_mark_value(ctx, ctx->fn_has_instance);
    js_gc_mark_value(ctx, ctx->obj_to_string);
    js_gc_mark_value(ctx, ctx->template_cache);
    JSObject **protos = &ctx->proto_object;
    for (JSObject **p = protos; p <= &ctx->ctor_regexp; p++) if (*p) js_gc_mark_cell(ctx, *p);
    for (JSJob *j = ctx->jobs; j; j = j->next) {
        js_gc_mark_value(ctx, j->fn);
        for (int i = 0; i < 4; i++) js_gc_mark_value(ctx, j->args[i]);
    }
    if (ctx->cur_job) {
        js_gc_mark_value(ctx, ctx->cur_job->fn);
        for (int i = 0; i < 4; i++) js_gc_mark_value(ctx, ctx->cur_job->args[i]);
    }
    js_gc_mark_value(ctx, ctx->eval_fn);
    js_gc_mark_value(ctx, ctx->array_iter_next);
    js_gc_mark_value(ctx, ctx->regexp_exec);
    js_gc_mark_value(ctx, ctx->unhandled);
    js_gc_mark_value(ctx, ctx->td_enc_key);
    js_gc_mark_value(ctx, ctx->sp_list_key);
    js_gc_mark_value(ctx, ctx->sp_url_key);
    js_gc_mark_value(ctx, ctx->intl_opts_key);
    js_gc_mark_value(ctx, ctx->async_ok);
    js_gc_mark_value(ctx, ctx->async_fail);
    for (int i = 0; i < ctx->nroots; i++) if (!JS_IsMarker(ctx->roots[i])) js_gc_mark_value(ctx, ctx->roots[i]);
    for (int i = 0; i < ctx->npinned; i++) js_gc_mark_cell(ctx, ctx->pinned[i]);
    scan_c_stack(ctx);
}

/* ephemerons: a weak map's value is live while its key is */
static void mark_weak(JSContext *ctx) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < ctx->nweak; i++) {
            JSMap *m = ctx->weaks[i]->u.p;
            if (!m) continue;
            for (uint32_t k = 0; k < m->n; k++) {
                JSMapEnt *e = &m->e[k];
                if (e->key == JS_HOLE) continue;
                GCHdr *kh = JS_PTR(e->key);
                if (kh->mark && JS_IsPtr(e->val)) {
                    GCHdr *vh = JS_PTR(e->val);
                    if (!vh->mark) { js_gc_mark_cell(ctx, vh); changed = true; }
                }
            }
        }
        drain(ctx);
    }
    for (int i = 0; i < ctx->nweak; i++) if (ctx->weaks[i]->u.p) js_map_sweep_weak(ctx, ctx->weaks[i]->u.p);
    ctx->nweak = 0;
    for (int i = 0; i < ctx->nwrefs; i++) {
        JSObject *o = ctx->wrefs[i];
        if (JS_IsPtr(o->u.prim) && !((GCHdr *)JS_PTR(o->u.prim))->mark) o->u.prim = JS_UNDEFINED;
    }
    ctx->nwrefs = 0;
}

/* ---- sweeping ------------------------------------------------------------------------------------ */
void js_host_finalize(JSContext *ctx, JSObject *o) {
    int c = o->u.host.cls;
    if (c > 0 && c <= ctx->nclasses && ctx->classes[c - 1].c.finalize && o->u.host.opaque)
        ctx->classes[c - 1].c.finalize(ctx, o->u.host.opaque);
    o->u.host.opaque = NULL;
}

static void finalize_object(JSContext *ctx, JSObject *o) {
    js_mfree(ctx, o->props, sizeof(JSProp) * o->pcap);
    js_mfree(ctx, o->hidx, sizeof(uint32_t) * o->hsize);
    switch (o->h.cls) {
    case CL_ARRAY: case CL_ARGUMENTS: js_mfree(ctx, o->u.a.v, sizeof(JSValue) * o->u.a.cap); break;
    case CL_FUNCTION: if (o->u.f.vr && o->u.f.b) js_mfree(ctx, o->u.f.vr, sizeof(JSVarRef *) * o->u.f.b->ncv); break;
    case CL_BOUND: js_mfree(ctx, o->u.bf.args, sizeof(JSValue) * (size_t)o->u.bf.argc); break;
    case CL_REGEXP: {
        JSRegExpObj *r = o->u.p;
        if (r) { js_re_free(ctx, r->re); js_mfree(ctx, r, sizeof *r); }
        break;
    }
    case CL_MAP: case CL_SET: case CL_WEAKMAP: case CL_WEAKSET: if (o->u.p) js_map_free(ctx, o->u.p); break;
    case CL_PROMISE: if (o->u.p) js_promise_free(ctx, o->u.p); break;
    case CL_GENERATOR: case CL_ASYNC_GEN: case CL_ASYNC: if (o->u.p) js_gen_free(ctx, o->u.p); break;
    case CL_ARRAYBUFFER: {
        JSArrayBuffer *ab = o->u.p;
        if (ab) { js_mfree(ctx, ab->data, ab->len); js_mfree(ctx, ab, sizeof *ab); }
        break;
    }
    case CL_TYPEDARRAY: case CL_DATAVIEW: js_mfree(ctx, o->u.p, sizeof(JSTypedArray)); break;
    case CL_ITER: case CL_FORIN: js_mfree(ctx, o->u.p, sizeof(JSIter)); break;
    case CL_HOST: js_host_finalize(ctx, o); break;
    default: break;
    }
}

static void finalize(JSContext *ctx, GCHdr *h) {
    if (h->type == T_OBJECT) finalize_object(ctx, (JSObject *)h);
    else if (h->type == T_BYTECODE) js_bytecode_free(ctx, (JSBytecode *)h);
}

static void sweep(JSContext *ctx) {
    for (int sc = 0; sc < NSC; sc++) {
        void *head = NULL;
        GCPage **pp = &ctx->pages[sc];
        while (*pp) {
            GCPage *pg = *pp;
            uint32_t used = 0;
            void *page_head = head;
            for (int i = (int)pg->ncells - 1; i >= 0; i--) {
                GCHdr *h = (GCHdr *)(pg->mem + (size_t)i * pg->cell);
                if (h->type != T_FREE) {
                    if (h->mark) { h->mark = 0; used++; continue; }
                    finalize(ctx, h);
                    h->type = T_FREE;
                }
                *(void **)(h + 1) = head;
                head = h;
            }
            pg->used = used;
            if (!used && ctx->mem_used > ctx->mem_limit / 4) {
                /* give an empty page back (its cells were just threaded: unthread them) */
                head = page_head;
                *pp = pg->next;
                seg_remove(ctx, (uintptr_t)pg->mem);
                free(pg->mem);
                free(pg);
                ctx->mem_used -= PAGE_BYTES;
                continue;
            }
            pp = &pg->next;
        }
        ctx->free_cells[sc] = head;
    }
    int n = 0;
    for (int i = 0; i < ctx->nlarge; i++) {
        GCHdr *h = ctx->large[i];
        if (h->mark) { h->mark = 0; ctx->large[n++] = h; continue; }
        size_t sz = large_size(ctx, h);
        finalize(ctx, h);
        seg_remove(ctx, (uintptr_t)h);
        free(h);
        ctx->mem_used -= sz + 16;
    }
    ctx->nlarge = n;
}

static void collect(JSContext *ctx) {
    if (ctx->in_gc || ctx->no_gc) return;
    ctx->in_gc = true;
    ctx->msp = 0;
    ctx->nweak = ctx->nwrefs = 0;
    mark_roots(ctx);
    drain(ctx);
    mark_weak(ctx);
    js_atoms_sweep(ctx);
    sweep(ctx);
    ctx->gc_count++;
    ctx->alloc_since = 0;
    size_t live = ctx->mem_used;
    ctx->gc_threshold = MAX((size_t)(4u << 20), live);
    if (ctx->gc_threshold > ctx->mem_limit / 2) ctx->gc_threshold = MAX((size_t)(2u << 20), ctx->mem_limit / 8);
    ctx->in_gc = false;
}

void js_gc(JSContext *ctx) {
    JS_GC_ENTER(ctx);
    collect(ctx);
    JS_GC_LEAVE(ctx);
}

void js_maybe_gc(JSContext *ctx) {
    if (ctx->alloc_since > ctx->gc_threshold) collect(ctx);
}

size_t js_mem_used(JSContext *ctx) { return ctx->mem_used; }
uint32_t js_gc_count(JSContext *ctx) { return ctx->gc_count; }

int js_api_enter(JSContext *ctx, uintptr_t sb) {
    int prev = ctx->api_depth++;
    if (!prev) ctx->stack_base = sb + 256;
    return prev;
}

void js_api_leave(JSContext *ctx, int prev) {
    ctx->api_depth = prev;
    if (!prev) ctx->stack_base = 0;
}

void js_gc_init(JSContext *ctx) {
    ctx->mem_limit = ctx->host.mem_limit ? ctx->host.mem_limit : (64u << 20);
    ctx->gc_threshold = 4u << 20;
}

void js_gc_free_all(JSContext *ctx) {
    ctx->in_gc = true;
    for (int sc = 0; sc < NSC; sc++) {
        GCPage *pg = ctx->pages[sc];
        while (pg) {
            for (uint32_t i = 0; i < pg->ncells; i++) {
                GCHdr *h = (GCHdr *)(pg->mem + (size_t)i * pg->cell);
                if (h->type != T_FREE) finalize(ctx, h);
            }
            GCPage *nx = pg->next;
            free(pg->mem);
            free(pg);
            pg = nx;
        }
        ctx->pages[sc] = NULL;
        ctx->free_cells[sc] = NULL;
    }
    for (int i = 0; i < ctx->nlarge; i++) { finalize(ctx, ctx->large[i]); free(ctx->large[i]); }
    free(ctx->large);
    free(ctx->segs);
    free(ctx->mark_stack);
    free(ctx->weaks);
    free(ctx->wrefs);
    ctx->large = NULL;
    ctx->segs = NULL;
    ctx->nlarge = ctx->nsegs = 0;
}

/* ---- roots and pins ------------------------------------------------------------------------------- */
int js_root_add(JSContext *ctx, JSValue v) {
    if (ctx->root_free >= 0) {
        int h = ctx->root_free;
        ctx->root_free = (int)(int32_t)(uint32_t)ctx->roots[h] ;
        ctx->roots[h] = v;
        return h;
    }
    if (ctx->nroots >= ctx->rootcap) {
        int nc = ctx->rootcap ? ctx->rootcap * 2 : 64;
        JSValue *n = realloc(ctx->roots, sizeof *n * (size_t)nc);
        if (!n) return -1;
        ctx->roots = n;
        ctx->rootcap = nc;
    }
    ctx->roots[ctx->nroots] = v;
    return ctx->nroots++;
}

JSValue js_root_get(JSContext *ctx, int h) {
    return h >= 0 && h < ctx->nroots && !JS_IsMarker(ctx->roots[h]) ? ctx->roots[h] : JS_UNDEFINED;
}

void js_root_set(JSContext *ctx, int h, JSValue v) {
    if (h >= 0 && h < ctx->nroots && !JS_IsMarker(ctx->roots[h])) ctx->roots[h] = v;
}

void js_root_free(JSContext *ctx, int h) {
    if (h < 0 || h >= ctx->nroots || JS_IsMarker(ctx->roots[h])) return;
    ctx->roots[h] = JS_MKMARK((uint32_t)ctx->root_free);
    ctx->root_free = h;
}

void js_pin(JSContext *ctx, JSValue v) {
    if (!JS_IsObject(v)) return;
    JSObject *o = OBJ(v);
    if (o->h.flags & OF_PINNED) return;
    if (ctx->npinned >= ctx->pincap) {
        int nc = ctx->pincap ? ctx->pincap * 2 : 64;
        JSObject **n = realloc(ctx->pinned, sizeof *n * (size_t)nc);
        if (!n) return;
        ctx->pinned = n;
        ctx->pincap = nc;
    }
    o->h.flags |= OF_PINNED;
    ctx->pinned[ctx->npinned++] = o;
}

void js_unpin(JSContext *ctx, JSValue v) {
    if (!JS_IsObject(v)) return;
    JSObject *o = OBJ(v);
    if (!(o->h.flags & OF_PINNED)) return;
    o->h.flags &= (uint8_t)~OF_PINNED;
    for (int i = 0; i < ctx->npinned; i++)
        if (ctx->pinned[i] == o) { ctx->pinned[i] = ctx->pinned[--ctx->npinned]; break; }
}
