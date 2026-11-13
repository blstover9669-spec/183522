/*
 * js_comp.c - NovaJS compiler: scope analysis and bytecode generation.
 *
 * Pass 1 (resolve) walks every function of the tree: declares parameters,
 * hoisted vars and functions, block-scoped let / const / class, and binds
 * every identifier to its declaration. A binding used by an inner function is
 * "captured": it lives in a heap box (JSVarRef) that closures share, and each
 * function between gets a closure-variable slot leading to it. Arrows reach
 * this / new.target / arguments / the active function through hidden bindings
 * of the nearest non-arrow function. Script top-level declarations are global
 * (looked up by name).
 *
 * Pass 2 (generate) emits stack-machine bytecode per function. break /
 * continue / return walk a control stack that knows how to leave each level
 * (pop a for-in iterator, close a for-of iterator, drop a catch marker, run a
 * finally block as a subroutine).
 */
#include "js_ast.h"

typedef struct Comp {
    JSContext *ctx;
    Parser *p;
    JSValue src, srcname;
    bool failed;
    char err[240];
    int err_line;
    JSValue a_args, a_this, a_nt, a_func, a_undefined, a_eval;
} Comp;

static void cerr(Comp *c, int line, const char *fmt, ...) {
    if (c->failed) return;
    c->failed = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
    c->err_line = line;
}

static void *calloc_p(Comp *c, size_t n) { return js_parser_alloc(c->p, n); }

/* ==== scopes and bindings ======================================================================== */
static Scope *new_scope(Comp *c, Scope *parent, Func *fn, int kind) {
    Scope *s = calloc_p(c, sizeof(Scope));
    if (!s) { static Scope d; return &d; }
    s->parent = parent;
    s->fn = fn;
    s->kind = (uint8_t)kind;
    return s;
}

static Binding *scope_find(Scope *s, JSValue name) {
    for (Binding *b = s->b; b; b = b->next) if (b->name == name) return b;
    return NULL;
}

static Binding *new_binding(Comp *c, Scope *s, JSValue name, int kind, int slot) {
    Binding *b = calloc_p(c, sizeof(Binding));
    if (!b) { static Binding d; return &d; }
    b->name = name;
    b->kind = (uint8_t)kind;
    b->fn = s->fn;
    b->scope = s;
    b->tdz = kind == BK_LET || kind == BK_CONST || kind == BK_CLASS;
    if (s->kind == SK_SCRIPT && kind != BK_HIDDEN && kind != BK_PARAM) { b->global = true; b->slot = -1; }
    else b->slot = slot >= 0 ? slot : s->fn->nlocals++;
    b->next = s->b;
    s->b = b;
    return b;
}

static bool lexical_kind(int k) { return k == BK_LET || k == BK_CONST || k == BK_CLASS; }

static Binding *declare(Comp *c, Scope *s, JSValue name, int kind, int line) {
    Binding *b = scope_find(s, name);
    if (b) {
        if (lexical_kind(kind) || lexical_kind(b->kind) || (kind == BK_FUNC && s->kind == SK_BLOCK && b->kind != BK_FUNC)) {
            if (kind == BK_FUNC && b->kind == BK_FUNC) return b;
            char nm[80];
            js_str_to_ascii(c->ctx, name, nm, sizeof nm);
            cerr(c, line, "Identifier '%s' has already been declared", nm);
            return b;
        }
        if (kind == BK_FUNC && b->kind == BK_VAR) b->kind = BK_FUNC;
        return b;
    }
    return new_binding(c, s, name, kind, -1);
}

static void declare_pattern(Comp *c, Scope *s, Node *n, int kind) {
    if (!n) return;
    switch (n->k) {
    case N_IDENT: declare(c, s, n->v, kind, n->line); break;
    case N_ASSIGN_PAT: declare_pattern(c, s, n->a, kind); break;
    case N_REST: declare_pattern(c, s, n->a, kind); break;
    case N_ARRAY_PAT: for (Node *e = n->a; e; e = e->next) declare_pattern(c, s, e, kind); break;
    case N_OBJECT_PAT: for (Node *pr = n->a; pr; pr = pr->next) declare_pattern(c, s, pr->b, kind); break;
    default: break;
    }
}

static Binding *hidden(Comp *c, Func *g, int which) {
    Binding **slotp = which == 0 ? &g->this_b : which == 1 ? &g->nt_b : which == 2 ? &g->func_b : &g->args_b;
    if (*slotp) return *slotp;
    JSValue nm = which == 0 ? c->a_this : which == 1 ? c->a_nt : which == 2 ? c->a_func : c->a_args;
    Binding *b = new_binding(c, g->scope, nm, BK_HIDDEN, -1);
    if (which == 0 && g->kind == FK_DERIVED) b->tdz = true;
    *slotp = b;
    return b;
}

static int cvar_index(Comp *c, Func *f, Binding *b) {
    for (int i = 0; i < f->ncv; i++) if (f->cv[i].b == b) return i;
    uint16_t src;
    if (!f->parent) { cerr(c, 0, "internal error: closure variable without parent"); return 0; }
    if (f->parent == b->fn) src = (uint16_t)(0x8000 | b->slot);
    else src = (uint16_t)cvar_index(c, f->parent, b);
    if (f->ncv >= f->cvcap) {
        int nc = f->cvcap ? f->cvcap * 2 : 8;
        CVar *n = calloc_p(c, sizeof(CVar) * (size_t)nc);
        if (!n) return 0;
        if (f->ncv) memcpy(n, f->cv, sizeof(CVar) * (size_t)f->ncv);
        f->cv = n;
        f->cvcap = nc;
    }
    f->cv[f->ncv].b = b;
    f->cv[f->ncv].src = src;
    return f->ncv++;
}

static void capture(Comp *c, Func *cur, Binding *b) {
    b->captured = true;
    b->scope->has_captured = true;
    cvar_index(c, cur, b);
}

static Func *this_func(Func *f) {
    while (f->kind == FK_ARROW && f->parent) f = f->parent;
    return f;
}

static Binding *lookup(Comp *c, Scope *s, JSValue name) {
    Func *cur = s->fn;
    for (Scope *x = s; x; x = x->parent) {
        Binding *b = scope_find(x, name);
        if (!b && name == c->a_args && x->kind == SK_FUNC && x->fn->kind != FK_ARROW && x->fn->kind != FK_FIELDS &&
            x->fn->kind != FK_STATIC_INIT) {
            b = hidden(c, x->fn, 3);
            x->fn->uses_args = true;
        }
        if (b) {
            if (!b->global && b->fn != cur) capture(c, cur, b);
            return b;
        }
    }
    return NULL;
}

/* this (0), new.target (1), the active function (2) for code in cur: NULL = the frame's own */
static Binding *special_ref(Comp *c, Scope *s, int which) {
    Func *cur = s->fn, *g = this_func(cur);
    if (g == cur && !(which == 0 && g->kind == FK_DERIVED)) return NULL;
    Binding *b = hidden(c, g, which);
    if (g != cur) capture(c, cur, b);
    return b;
}

/* ---- hoisting --------------------------------------------------------------------------------- */
static void hoist(Comp *c, Scope *fs, Node *n, bool top, bool single) {
    for (; n; n = single ? NULL : n->next) {
        switch (n->k) {
        case N_VAR:
            if (n->op == VK_VAR) for (Node *d = n->a; d; d = d->next) declare_pattern(c, fs, d->a, BK_VAR);
            break;
        case N_FUNC_DECL:
            if (top) {
                Binding *b = declare(c, fs, n->fn->name, BK_FUNC, n->line);
                (void)b;
                n->d = fs->fdecls;
                fs->fdecls = n;
            } else if (!(n->fn->flags & F_STRICT) && !(fs->fn->flags & F_STRICT) && !(n->fn->flags & (F_ASYNC | F_GEN))) {
                Binding *ex = scope_find(fs, n->fn->name);
                if (!ex || !lexical_kind(ex->kind)) {
                    declare(c, fs, n->fn->name, BK_VAR, n->line);
                    n->flags |= NF_ANNEXB;
                }
            }
            break;
        case N_BLOCK: hoist(c, fs, n->a, false, false); break;
        case N_IF: hoist(c, fs, n->b, false, true); hoist(c, fs, n->c, false, true); break;
        case N_FOR:
            if (n->a && n->a->k == N_VAR) hoist(c, fs, n->a, false, true);
            hoist(c, fs, n->d, false, true);
            break;
        case N_FORIN: case N_FOROF:
            if (n->a && n->a->k == N_VAR) hoist(c, fs, n->a, false, true);
            hoist(c, fs, n->d, false, true);
            break;
        case N_WHILE: case N_DOWHILE: case N_WITH: hoist(c, fs, n->d, false, true); break;
        case N_TRY: hoist(c, fs, n->a, false, true); hoist(c, fs, n->c, false, true); hoist(c, fs, n->d, false, true); break;
        case N_SWITCH: for (Node *cs = n->b; cs; cs = cs->next) hoist(c, fs, cs->b, false, false); break;
        case N_LABELED: hoist(c, fs, n->a, false, true); break;
        default: break;
        }
    }
}

/* lexical declarations directly in a statement list (blocks: also function declarations) */
static void declare_lex(Comp *c, Scope *s, Node *list, bool block) {
    for (Node *n = list; n; n = n->next) {
        if (n->k == N_VAR && n->op != VK_VAR) {
            for (Node *d = n->a; d; d = d->next) declare_pattern(c, s, d->a, n->op == VK_LET ? BK_LET : BK_CONST);
        } else if (n->k == N_CLASS_DECL && n->a->a) {
            declare(c, s, n->a->a->v, BK_CLASS, n->line);
        } else if (n->k == N_FUNC_DECL && block) {
            declare(c, s, n->fn->name, BK_FUNC, n->line);
            n->d = s->fdecls;
            s->fdecls = n;
        }
    }
}

static bool has_lex(Node *list) {
    for (Node *n = list; n; n = n->next)
        if ((n->k == N_VAR && n->op != VK_VAR) || n->k == N_CLASS_DECL || n->k == N_FUNC_DECL) return true;
    return false;
}

/* ---- resolve --------------------------------------------------------------------------------- */
static void res_expr(Comp *c, Scope *s, Node *e);
static void res_stmt(Comp *c, Scope *s, Node *n);
static void res_func(Comp *c, Func *f, Scope *outer);

static void res_list(Comp *c, Scope *s, Node *n) { for (; n; n = n->next) res_expr(c, s, n); }
static void res_stmts(Comp *c, Scope *s, Node *n) { for (; n; n = n->next) res_stmt(c, s, n); }

static void res_pattern(Comp *c, Scope *s, Node *n) {
    if (!n) return;
    switch (n->k) {
    case N_IDENT: n->res = lookup(c, s, n->v); break;
    case N_ASSIGN_PAT: res_pattern(c, s, n->a); res_expr(c, s, n->b); break;
    case N_REST: res_pattern(c, s, n->a); break;
    case N_ARRAY_PAT: for (Node *e = n->a; e; e = e->next) res_pattern(c, s, e); break;
    case N_OBJECT_PAT:
        for (Node *pr = n->a; pr; pr = pr->next) {
            if (pr->flags & NF_COMPUTED) res_expr(c, s, pr->a);
            res_pattern(c, s, pr->b);
        }
        break;
    case N_HOLE: break;
    default: res_expr(c, s, n); break;
    }
}

static void res_class(Comp *c, Scope *s, Node *cls) {
    Scope *K = new_scope(c, s, s->fn, SK_CLASS);
    cls->res = K;
    if (cls->a) cls->a->res = new_binding(c, K, cls->a->v, BK_CONST, -1);
    for (Node *el = cls->c; el; el = el->next) {
        if ((el->flags & NF_PRIVATE) && !scope_find(K, el->a->v)) new_binding(c, K, el->a->v, BK_PRIVATE, -1);
        if (el->op == PK_FIELD && (el->flags & NF_COMPUTED)) new_binding(c, K, el->v, BK_HIDDEN, -1);
    }
    if (cls->b) res_expr(c, K, cls->b);
    Func *inst = cls->d ? cls->d->fn : NULL, *stat = cls->d && cls->d->next ? cls->d->next->fn : NULL;
    for (Node *el = cls->c; el; el = el->next) {
        if (el->flags & NF_COMPUTED) res_expr(c, K, el->a);
        if (el->flags & NF_PRIVATE) el->res = lookup(c, K, el->a->v);
        if (el->op != PK_FIELD && el->b && el->b->k == N_FUNC) res_func(c, el->b->fn, K);
        if (el->op == PK_FIELD && (el->flags & NF_COMPUTED)) el->a->res = lookup(c, K, el->v);
    }
    if (cls->fn) res_func(c, cls->fn, K);
    if (inst) res_func(c, inst, K);
    if (stat) res_func(c, stat, K);
}

static void res_expr(Comp *c, Scope *s, Node *e) {
    if (!e || c->failed) return;
    switch (e->k) {
    case N_IDENT: e->res = lookup(c, s, e->v); break;
    case N_THIS: e->res = special_ref(c, s, 0); break;
    case N_NEW_TARGET: e->res = special_ref(c, s, 1); break;
    case N_SUPER_MEMBER:
        e->res = special_ref(c, s, 0);
        if (e->flags & NF_COMPUTED) res_expr(c, s, e->a);
        break;
    case N_SUPER_CALL: {
        Func *g = this_func(s->fn);
        if (g->kind != FK_DERIVED) { cerr(c, e->line, "'super' keyword unexpected here"); return; }
        Binding *tb = hidden(c, g, 0);
        if (g != s->fn) { capture(c, s->fn, tb); capture(c, s->fn, hidden(c, g, 1)); capture(c, s->fn, hidden(c, g, 2)); }
        e->res = tb;
        res_list(c, s, e->b);
        break;
    }
    case N_FUNC: res_func(c, e->fn, s); break;
    case N_CLASS: res_class(c, s, e); break;
    case N_CALL:
        res_expr(c, s, e->a);
        res_list(c, s, e->b);
        if ((e->flags & NF_DIRECTEVAL) && e->a->res) e->flags &= ~(uint32_t)NF_DIRECTEVAL;
        break;
    case N_PRIVMEMBER: case N_PRIVIN:
        res_expr(c, s, e->a);
        e->res = lookup(c, s, e->v);
        if (!e->res) { char nm[80]; js_str_to_ascii(c->ctx, e->v, nm, sizeof nm); cerr(c, e->line, "Private field '%s' must be declared in an enclosing class", nm); }
        break;
    case N_TEMPLATE: res_list(c, s, e->b); break;
    case N_TAGGED: res_expr(c, s, e->a); res_list(c, s, e->b->b); break;
    case N_ARRAY: case N_SEQ: res_list(c, s, e->a); break;
    case N_OBJECT:
        for (Node *pr = e->a; pr; pr = pr->next) {
            if (pr->flags & NF_COMPUTED) res_expr(c, s, pr->a);
            if (pr->flags & NF_COVERINIT) { cerr(c, pr->line, "Invalid shorthand property initializer"); return; }
            res_expr(c, s, pr->b);
        }
        break;
    case N_ASSIGN: res_pattern(c, s, e->a); res_expr(c, s, e->b); break;
    case N_ARRAY_PAT: case N_OBJECT_PAT: case N_ASSIGN_PAT: case N_REST: res_pattern(c, s, e); break;
    case N_NEW: res_expr(c, s, e->a); res_list(c, s, e->b); break;
    default:
        res_expr(c, s, e->a);
        res_expr(c, s, e->b);
        res_expr(c, s, e->c);
        break;
    }
}

static void res_stmt(Comp *c, Scope *s, Node *n) {
    if (!n || c->failed) return;
    switch (n->k) {
    case N_VAR:
        for (Node *d = n->a; d; d = d->next) { res_pattern(c, s, d->a); res_expr(c, s, d->b); }
        break;
    case N_FUNC_DECL:
        res_func(c, n->fn, s);
        n->res = lookup(c, s, n->fn->name);
        break;
    case N_CLASS_DECL:
        res_class(c, s, n->a);
        n->res = n->a->a ? lookup(c, s, n->a->a->v) : NULL;
        break;
    case N_EXPR: case N_THROW: case N_RETURN: res_expr(c, s, n->a); break;
    case N_BLOCK:
        if (has_lex(n->a)) {
            Scope *b = new_scope(c, s, s->fn, SK_BLOCK);
            declare_lex(c, b, n->a, true);
            n->res = b;
            res_stmts(c, b, n->a);
        } else res_stmts(c, s, n->a);
        break;
    case N_IF: res_expr(c, s, n->a); res_stmt(c, s, n->b); res_stmt(c, s, n->c); break;
    case N_FOR: {
        Scope *h = s;
        if (n->a && n->a->k == N_VAR && n->a->op != VK_VAR) {
            h = new_scope(c, s, s->fn, SK_BLOCK);
            for (Node *d = n->a->a; d; d = d->next) declare_pattern(c, h, d->a, n->a->op == VK_LET ? BK_LET : BK_CONST);
            n->res = h;
        }
        if (n->a && n->a->k == N_VAR) res_stmt(c, h, n->a); else res_expr(c, h, n->a);
        res_expr(c, h, n->b);
        res_expr(c, h, n->c);
        res_stmt(c, h, n->d);
        break;
    }
    case N_FORIN: case N_FOROF: {
        Scope *h = s;
        if (n->a->k == N_VAR && n->a->op != VK_VAR) {
            h = new_scope(c, s, s->fn, SK_BLOCK);
            declare_pattern(c, h, n->a->a->a, n->a->op == VK_LET ? BK_LET : BK_CONST);
            n->res = h;
        }
        if (n->a->k == N_VAR) res_stmt(c, h, n->a); else res_pattern(c, h, n->a);
        res_expr(c, h, n->b);
        res_stmt(c, h, n->d);
        break;
    }
    case N_WHILE: case N_DOWHILE: res_expr(c, s, n->a); res_stmt(c, s, n->d); break;
    case N_SWITCH: {
        res_expr(c, s, n->a);
        Scope *b = new_scope(c, s, s->fn, SK_BLOCK);
        for (Node *cs = n->b; cs; cs = cs->next) declare_lex(c, b, cs->b, true);
        n->res = b;
        for (Node *cs = n->b; cs; cs = cs->next) { res_expr(c, b, cs->a); res_stmts(c, b, cs->b); }
        break;
    }
    case N_TRY:
        res_stmt(c, s, n->a);
        if (n->c) {
            Scope *cs = new_scope(c, s, s->fn, SK_CATCH);
            if (n->b) declare_pattern(c, cs, n->b, BK_CATCH);
            n->res = cs;
            res_pattern(c, cs, n->b);
            res_stmt(c, cs, n->c);
        }
        res_stmt(c, s, n->d);
        break;
    case N_LABELED: res_stmt(c, s, n->a); break;
    case N_WITH: cerr(c, n->line, "'with' statements are not supported"); break;
    case N_FIELD_DEF:
        if (n->flags & NF_PRIVATE) n->res = lookup(c, s, n->a->v);
        else if (n->flags & NF_COMPUTED) res_expr(c, s, n->a);
        res_expr(c, s, n->b);
        break;
    default: break;
    }
}

static void res_func(Comp *c, Func *f, Scope *outer) {
    if (c->failed) return;
    int nformals = 0;
    for (Node *p = f->params; p; p = p->next) if (p->k != N_REST) nformals++;
    f->nlocals = nformals;
    Scope *base = outer;
    if (f->is_expr_named && !JS_IsUndefined(f->name)) {
        Scope *ns = new_scope(c, outer, f, SK_BLOCK);
        f->self = new_binding(c, ns, f->name, BK_SELF, -1);
        base = ns;
    }
    Scope *S = new_scope(c, base, f, (f->kind == FK_SCRIPT || f->kind == FK_MODULE) ? SK_SCRIPT : SK_FUNC);
    f->scope = S;
    int pos = 0;
    for (Node *p = f->params; p; p = p->next) {
        if (p->k == N_IDENT) {
            Binding *b = scope_find(S, p->v);
            if (!b) b = new_binding(c, S, p->v, BK_PARAM, pos);
            else {
                if ((f->flags & F_STRICT) || !f->simple_params || (f->flags & F_ARROW)) { char nm[80]; js_str_to_ascii(c->ctx, p->v, nm, sizeof nm); cerr(c, p->line, "Duplicate parameter name not allowed in this context"); }
                b->slot = pos;
            }
        } else if (p->k == N_REST) declare_pattern(c, S, p->a, BK_PARAM);
        else declare_pattern(c, S, p, BK_PARAM);
        if (p->k != N_REST) pos++;
    }
    hoist(c, S, f->body, true, false);
    declare_lex(c, S, f->body, false);
    for (Node *p = f->params; p; p = p->next) res_pattern(c, S, p);
    if (f->kind == FK_DERIVED) hidden(c, f, 0);
    res_stmts(c, S, f->body);
}

/* ==== code generation ================================================================================= */
typedef struct Jmps { uint32_t *a; int n, cap; } Jmps;
enum { CT_LOOP = 0, CT_SWITCH, CT_BLOCK, CT_TRY, CT_FINALLY, CT_FOROF, CT_FORIN, CT_SUB };
typedef struct Ctl {
    struct Ctl *prev;
    int kind;
    JSValue labels[6];
    int nlabels;
    bool loop;          /* continue allowed */
    Jmps brk, cont, fin;
    int sp;
} Ctl;

typedef struct FS {
    Comp *c;
    Func *f;
    struct FS *parent;
    uint8_t *code;
    uint32_t len, cap;
    JSValue *cp;
    uint32_t ncp, cpcap;
    uint32_t *cph;      /* cp hash (index + 1) */
    uint32_t cphsize;
    int sp, max_sp;
    uint32_t *lines;
    uint32_t nlines, linecap;
    int cur_line, last_line;
    Ctl *ctl;
    Jmps *chain;        /* optional chain short-circuit jumps */
    JSValue pending_labels[6];
    int npending;
    int ret_slot;
    int completion;     /* script completion value slot (-1 none) */
    int free_tmp[16];
    int nfree_tmp;
} FS;

static void grow(FS *fs, uint32_t n) {
    if (fs->len + n <= fs->cap) return;
    uint32_t nc = fs->cap ? fs->cap * 2 : 256;
    while (nc < fs->len + n) nc *= 2;
    uint8_t *nb = realloc(fs->code, nc);
    if (!nb) { cerr(fs->c, 0, "out of memory"); fs->len = 0; return; }
    fs->code = nb;
    fs->cap = nc;
}

static void put8(FS *fs, uint32_t v) { grow(fs, 1); if (fs->c->failed) return; fs->code[fs->len++] = (uint8_t)v; }
static void put16(FS *fs, uint32_t v) { put8(fs, v & 0xFF); put8(fs, (v >> 8) & 0xFF); }
static void put32(FS *fs, uint32_t v) { put16(fs, v & 0xFFFF); put16(fs, v >> 16); }

static void add_sp(FS *fs, int d) {
    fs->sp += d;
    if (fs->sp > fs->max_sp) fs->max_sp = fs->sp;
}

static void mark_line(FS *fs) {
    if (fs->cur_line == fs->last_line || fs->cur_line <= 0) return;
    if (fs->nlines + 2 > fs->linecap) {
        uint32_t nc = fs->linecap ? fs->linecap * 2 : 64;
        uint32_t *n = realloc(fs->lines, sizeof(uint32_t) * nc);
        if (!n) return;
        fs->lines = n;
        fs->linecap = nc;
    }
    fs->lines[fs->nlines++] = fs->len;
    fs->lines[fs->nlines++] = (uint32_t)fs->cur_line;
    fs->last_line = fs->cur_line;
}

/* fixed stack effects; 99 = computed by the caller */
static int op_effect(int op) {
    switch (op) {
    case OP_PUSH_UNDEF: case OP_PUSH_NULL: case OP_PUSH_TRUE: case OP_PUSH_FALSE: case OP_PUSH_I8: case OP_PUSH_I32:
    case OP_PUSH_CONST: case OP_PUSH_THIS: case OP_PUSH_NEW_TARGET: case OP_PUSH_FUNC: case OP_PUSH_UNINIT:
    case OP_PUSH_EMPTY: case OP_PUSH_HOME: case OP_OBJECT_NEW: case OP_CLOSURE: case OP_REGEXP: case OP_TEMPLATE:
    case OP_DUP: case OP_OVER: case OP_GET_LOC: case OP_GET_LOC_CHK: case OP_GET_BOX: case OP_GET_BOX_CHK:
    case OP_GET_VREF: case OP_GET_VREF_CHK: case OP_GET_GLOBAL: case OP_GET_GLOBAL_TYPEOF: case OP_DELETE_GLOBAL:
    case OP_GET_FIELD_KEEP: case OP_NEW_PRIVATE: case OP_FOR_OF_START: case OP_FOR_AWAIT_START: case OP_ITER_STEP:
    case OP_ITER_REST: case OP_ARGUMENTS: case OP_REST: case OP_TRY: case OP_ITER_NEXT_RAW:
        return 1;
    case OP_DUP2: return 2;
    case OP_POP: case OP_NIP: case OP_PUT_LOC: case OP_PUT_LOC_CHK: case OP_PUT_BOX: case OP_PUT_BOX_CHK: case OP_INIT_BOX:
    case OP_PUT_VREF: case OP_PUT_VREF_CHK: case OP_PUT_GLOBAL: case OP_DEFINE_GLOBAL_FUNC: case OP_INIT_GLOBAL_LEX:
    case OP_PUT_FIELD: case OP_GET_ELEM: case OP_DELETE_ELEM: case OP_GET_SUPER: case OP_GET_PRIVATE: case OP_PRIVATE_IN:
    case OP_DEFINE_FIELD: case OP_DEFINE_METHOD: case OP_SET_PROTO: case OP_COPY_DATA_PROPS: case OP_ARRAY_PUSH:
    case OP_ARRAY_SPREAD: case OP_RETURN: case OP_JMP_FALSE: case OP_JMP_TRUE: case OP_JMP_FALSE_KEEP: case OP_JMP_TRUE_KEEP:
    case OP_JMP_NOTNULLISH_KEEP: case OP_THROW: case OP_END_TRY: case OP_RET:
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW: case OP_BAND: case OP_BOR: case OP_BXOR:
    case OP_SHL: case OP_SAR: case OP_SHR: case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_EQ: case OP_NE:
    case OP_SEQ: case OP_SNE: case OP_IN: case OP_INSTANCEOF: case OP_SET_FIELDS: case OP_RUN_FIELDS: case OP_CHECK_THIS_INIT:
    case OP_CHECK_DERIVED_RETURN:
        return -1;
    case OP_PUT_ELEM: case OP_PUT_SUPER: case OP_PUT_PRIVATE: case OP_DEFINE_PRIVATE: case OP_DEFINE_ELEM:
    case OP_DEFINE_METHOD_ELEM: case OP_COPY_DATA_EXCL: case OP_ITER_CLOSE: case OP_ITER_CLOSE_EXC: case OP_CALL_SPREAD: case OP_NEW_SPREAD:
    case OP_YIELD_STAR: case OP_ASYNC_YIELD_STAR:
        return -2;
    case OP_ITER_END: return -3;
    case OP_FOR_IN_NEXT: case OP_FOR_OF_NEXT: return 1;
    default: return 0;
    }
}

static void emit(FS *fs, int op) {
    mark_line(fs);
    put8(fs, (uint32_t)op);
    add_sp(fs, op_effect(op));
}
static void emit8(FS *fs, int op, uint32_t a) { emit(fs, op); put8(fs, a); }
static void emit16(FS *fs, int op, uint32_t a) { emit(fs, op); put16(fs, a); }
static void emit32(FS *fs, int op, uint32_t a) { emit(fs, op); put32(fs, a); }

static uint32_t emit_jump(FS *fs, int op) {
    emit(fs, op);
    uint32_t at = fs->len;
    put32(fs, 0);
    return at;
}

static void patch_to(FS *fs, uint32_t at, uint32_t target) {
    if (fs->c->failed || at + 4 > fs->len) return;
    int32_t off = (int32_t)(target - (at + 4));
    memcpy(fs->code + at, &off, 4);
}

static void jmps_add(Jmps *j, uint32_t at) {
    if (j->n >= j->cap) {
        int nc = j->cap ? j->cap * 2 : 8;
        uint32_t *n = realloc(j->a, sizeof(uint32_t) * (size_t)nc);
        if (!n) return;
        j->a = n;
        j->cap = nc;
    }
    j->a[j->n++] = at;
}

static void jmps_patch(FS *fs, Jmps *j, uint32_t target) {
    for (int i = 0; i < j->n; i++) patch_to(fs, j->a[i], target);
    free(j->a);
    j->a = NULL;
    j->n = j->cap = 0;
}

static void emit_back_jump(FS *fs, int op, uint32_t target) {
    uint32_t at = emit_jump(fs, op);
    patch_to(fs, at, target);
}

/* ---- constants -------------------------------------------------------------------------------- */
static uint32_t cp_hash(JSValue v) { uint64_t x = v ^ (v >> 31); return (uint32_t)((x * 0x9E3779B97F4A7C15ull) >> 32); }

static uint32_t add_const(FS *fs, JSValue v) {
    if (fs->cph) {
        uint32_t m = fs->cphsize - 1, k = cp_hash(v) & m;
        while (fs->cph[k]) {
            if (fs->cp[fs->cph[k] - 1] == v) return fs->cph[k] - 1;
            k = (k + 1) & m;
        }
    }
    if (fs->ncp >= fs->cpcap) {
        uint32_t nc = fs->cpcap ? fs->cpcap * 2 : 16;
        JSValue *n = realloc(fs->cp, sizeof(JSValue) * nc);
        if (!n) { cerr(fs->c, 0, "out of memory"); return 0; }
        fs->cp = n;
        fs->cpcap = nc;
    }
    fs->cp[fs->ncp++] = v;
    if (fs->ncp * 2 > fs->cphsize) {
        uint32_t hs = fs->cphsize ? fs->cphsize * 2 : 64;
        uint32_t *h = calloc(hs, sizeof(uint32_t));
        if (h) {
            free(fs->cph);
            fs->cph = h;
            fs->cphsize = hs;
            for (uint32_t i = 0; i < fs->ncp; i++) {
                uint32_t k = cp_hash(fs->cp[i]) & (hs - 1);
                while (h[k]) k = (k + 1) & (hs - 1);
                h[k] = i + 1;
            }
        }
    } else if (fs->cph) {
        uint32_t m = fs->cphsize - 1, k = cp_hash(v) & m;
        while (fs->cph[k]) k = (k + 1) & m;
        fs->cph[k] = fs->ncp;
    }
    return fs->ncp - 1;
}

/* a unique constant (not shared): bytecode of nested functions, template objects */
static uint32_t add_const_unique(FS *fs, JSValue v) {
    if (fs->ncp >= fs->cpcap) {
        uint32_t nc = fs->cpcap ? fs->cpcap * 2 : 16;
        JSValue *n = realloc(fs->cp, sizeof(JSValue) * nc);
        if (!n) { cerr(fs->c, 0, "out of memory"); return 0; }
        fs->cp = n;
        fs->cpcap = nc;
    }
    fs->cp[fs->ncp++] = v;
    return fs->ncp - 1;
}

static void emit_num(FS *fs, double d) {
    if (d == (double)(int8_t)d && !(d == 0 && signbit(d))) emit8(fs, OP_PUSH_I8, (uint32_t)(uint8_t)(int8_t)d);
    else if (d >= -2147483648.0 && d <= 2147483647.0 && d == (double)(int32_t)d && !(d == 0 && signbit(d))) emit32(fs, OP_PUSH_I32, (uint32_t)(int32_t)d);
    else emit32(fs, OP_PUSH_CONST, add_const(fs, js_mknum(d)));
}

static void emit_str(FS *fs, JSValue s) {
    if (js_strlen(s) == 0) emit(fs, OP_PUSH_EMPTY);
    else emit32(fs, OP_PUSH_CONST, add_const(fs, s));
}

static int tmp_alloc(FS *fs) {
    if (fs->nfree_tmp) return fs->free_tmp[--fs->nfree_tmp];
    return fs->f->nlocals++;
}
static void tmp_free(FS *fs, int t) { if (fs->nfree_tmp < 16) fs->free_tmp[fs->nfree_tmp++] = t; }

/* ---- bindings ------------------------------------------------------------------------------------ */
enum { ST_PUT = 0, ST_SET, ST_INIT };

static int vref_of(FS *fs, Binding *b) {
    for (int i = 0; i < fs->f->ncv; i++) if (fs->f->cv[i].b == b) return i;
    cerr(fs->c, fs->cur_line, "internal error: unresolved closure variable");
    return 0;
}

static void gen_load(FS *fs, Binding *b, JSValue name, bool typeof_mode) {
    if (!b || b->global) {
        if (!b && name == fs->c->a_undefined) { emit(fs, OP_PUSH_UNDEF); return; }
        emit32(fs, typeof_mode ? OP_GET_GLOBAL_TYPEOF : OP_GET_GLOBAL, add_const(fs, name));
        return;
    }
    if (b->fn == fs->f) {
        if (b->captured) emit16(fs, b->tdz ? OP_GET_BOX_CHK : OP_GET_BOX, (uint32_t)b->slot);
        else emit16(fs, b->tdz ? OP_GET_LOC_CHK : OP_GET_LOC, (uint32_t)b->slot);
    } else emit16(fs, b->tdz ? OP_GET_VREF_CHK : OP_GET_VREF, (uint32_t)vref_of(fs, b));
}

/* the raw value (no TDZ check) of a binding (derived constructor's this) */
static void gen_load_raw(FS *fs, Binding *b) {
    if (b->fn == fs->f) emit16(fs, b->captured ? OP_GET_BOX : OP_GET_LOC, (uint32_t)b->slot);
    else emit16(fs, OP_GET_VREF, (uint32_t)vref_of(fs, b));
}

static void gen_store(FS *fs, Binding *b, JSValue name, int mode) {
    if (!b || b->global) {
        if (b && b->global && mode == ST_INIT && lexical_kind(b->kind)) { emit32(fs, OP_INIT_GLOBAL_LEX, add_const(fs, name)); return; }
        if (b && b->global && b->kind == BK_CONST && mode != ST_INIT) { emit32(fs, OP_THROW_CONST, add_const(fs, name)); if (mode == ST_PUT) emit(fs, OP_POP); return; }
        emit32(fs, mode == ST_SET ? OP_SET_GLOBAL : OP_PUT_GLOBAL, add_const(fs, name));
        return;
    }
    if (b->kind == BK_CONST && mode != ST_INIT) {
        emit32(fs, OP_THROW_CONST, add_const(fs, name));
        if (mode == ST_PUT) emit(fs, OP_POP);
        return;
    }
    if (b->kind == BK_SELF && mode != ST_INIT) {
        if (fs->f->flags & F_STRICT) emit32(fs, OP_THROW_CONST, add_const(fs, name));
        if (mode == ST_PUT) emit(fs, OP_POP);
        return;
    }
    bool chk = b->tdz && mode != ST_INIT;
    if (b->fn == fs->f) {
        if (b->captured) {
            if (mode == ST_INIT) emit16(fs, OP_INIT_BOX, (uint32_t)b->slot);
            else if (mode == ST_PUT) emit16(fs, chk ? OP_PUT_BOX_CHK : OP_PUT_BOX, (uint32_t)b->slot);
            else if (chk) { emit(fs, OP_DUP); emit16(fs, OP_PUT_BOX_CHK, (uint32_t)b->slot); }
            else emit16(fs, OP_SET_BOX, (uint32_t)b->slot);
        } else {
            if (mode == ST_INIT) emit16(fs, OP_PUT_LOC, (uint32_t)b->slot);
            else if (mode == ST_PUT) emit16(fs, chk ? OP_PUT_LOC_CHK : OP_PUT_LOC, (uint32_t)b->slot);
            else if (chk) { emit(fs, OP_DUP); emit16(fs, OP_PUT_LOC_CHK, (uint32_t)b->slot); }
            else emit16(fs, OP_SET_LOC, (uint32_t)b->slot);
        }
    } else {
        int i = vref_of(fs, b);
        if (mode == ST_PUT || mode == ST_INIT) emit16(fs, chk ? OP_PUT_VREF_CHK : OP_PUT_VREF, (uint32_t)i);
        else if (chk) { emit(fs, OP_DUP); emit16(fs, OP_PUT_VREF_CHK, (uint32_t)i); }
        else emit16(fs, OP_SET_VREF, (uint32_t)i);
    }
}

/* initialise the bindings of a scope at its entry (boxes, TDZ, function declarations) */
static void gen_func_closure(FS *fs, Func *f);

static void gen_scope_entry(FS *fs, Scope *s) {
    if (!s) return;
    for (Binding *b = s->b; b; b = b->next) {
        if (b->global || b->fn != fs->f) continue;
        if (b->kind == BK_PRIVATE || b->kind == BK_HIDDEN) {
            if (b->captured) emit16(fs, OP_NEW_BOX_UNDEF, (uint32_t)b->slot);
            continue;
        }
        if (b->captured) emit16(fs, b->tdz ? OP_NEW_BOX : OP_NEW_BOX_UNDEF, (uint32_t)b->slot);
        else if (b->tdz) { emit(fs, OP_PUSH_UNINIT); emit16(fs, OP_PUT_LOC, (uint32_t)b->slot); }
    }
    /* function declarations (in source order: the list is reversed) */
    Node *fd[256];
    int n = 0;
    for (Node *x = s->fdecls; x && n < 256; x = x->d) fd[n++] = x;
    for (int i = n - 1; i >= 0; i--) {
        Node *x = fd[i];
        gen_func_closure(fs, x->fn);
        Binding *b = scope_find(s, x->fn->name);
        gen_store(fs, b, x->fn->name, ST_INIT);
    }
}

/* ---- control stack ------------------------------------------------------------------------------- */
static void ctl_push(FS *fs, Ctl *c, int kind, bool loop) {
    memset(c, 0, sizeof *c);
    c->prev = fs->ctl;
    c->kind = kind;
    c->loop = loop;
    c->sp = fs->sp;
    if (kind == CT_LOOP || kind == CT_SWITCH || kind == CT_BLOCK || kind == CT_FOROF || kind == CT_FORIN) {
        memcpy(c->labels, fs->pending_labels, sizeof(JSValue) * (size_t)fs->npending);
        c->nlabels = fs->npending;
        fs->npending = 0;
    }
    fs->ctl = c;
}

static void ctl_pop(FS *fs) { fs->ctl = fs->ctl->prev; }

/* leave one level (break / continue / return through it) */
static void ctl_cleanup(FS *fs, Ctl *c) {
    int save = fs->sp;
    switch (c->kind) {
    case CT_TRY: emit(fs, OP_END_TRY); break;
    case CT_FINALLY:
        emit(fs, OP_END_TRY);
        emit(fs, OP_PUSH_UNDEF);
        jmps_add(&c->fin, emit_jump(fs, OP_GOSUB));
        emit(fs, OP_POP);
        break;
    case CT_FOROF: emit(fs, OP_ITER_CLOSE); break;
    case CT_FORIN: emit(fs, OP_POP); break;
    case CT_SUB: emit(fs, OP_POP); emit(fs, OP_POP); break;
    default: break;
    }
    (void)save;
}

static bool has_label(Ctl *c, JSValue l) {
    for (int i = 0; i < c->nlabels; i++) if (c->labels[i] == l) return true;
    return false;
}

static void gen_break(FS *fs, Node *n, bool cont) {
    Ctl *t = fs->ctl;
    for (; t; t = t->prev) {
        if (JS_IsUndefined(n->v)) {
            if (cont ? t->loop : (t->kind == CT_LOOP || t->kind == CT_SWITCH || t->kind == CT_FOROF || t->kind == CT_FORIN)) break;
        } else if (has_label(t, n->v) && (!cont || t->loop)) break;
    }
    if (!t) {
        if (!JS_IsUndefined(n->v)) cerr(fs->c, n->line, "Undefined label");
        else cerr(fs->c, n->line, cont ? "Illegal continue statement: no surrounding iteration statement" : "Illegal break statement");
        return;
    }
    int save = fs->sp;
    for (Ctl *x = fs->ctl; x != t; x = x->prev) ctl_cleanup(fs, x);
    if (cont) jmps_add(&t->cont, emit_jump(fs, OP_JMP));
    else {
        if (t->kind == CT_FOROF || t->kind == CT_FORIN) ctl_cleanup(fs, t);
        jmps_add(&t->brk, emit_jump(fs, OP_JMP));
    }
    fs->sp = save;
}

static bool need_cleanup_for_return(FS *fs) {
    for (Ctl *x = fs->ctl; x; x = x->prev) if (x->kind == CT_FINALLY || x->kind == CT_FOROF) return true;
    return false;
}

static void gen_this(FS *fs, Binding *b);

/* the value to return is on the stack */
static void gen_return_value(FS *fs) {
    Func *f = fs->f;
    if (need_cleanup_for_return(fs)) {
        if (fs->ret_slot < 0) fs->ret_slot = f->nlocals++;
        emit16(fs, OP_PUT_LOC, (uint32_t)fs->ret_slot);
        int save = fs->sp;
        for (Ctl *x = fs->ctl; x; x = x->prev) ctl_cleanup(fs, x);
        emit16(fs, OP_GET_LOC, (uint32_t)fs->ret_slot);
        (void)save;
    }
    if (f->kind == FK_DERIVED) {
        gen_load_raw(fs, f->this_b);
        emit(fs, OP_CHECK_DERIVED_RETURN);
    }
    emit(fs, OP_RETURN);
}

/* ---- expressions ---------------------------------------------------------------------------------- */
static void gen_expr(FS *fs, Node *e);
static void gen_stmt(FS *fs, Node *n);
static void gen_destructure(FS *fs, Node *pat, int mode);
static void gen_class(FS *fs, Node *cls, JSValue name);

static bool anon_fn(Node *e) {
    if (e->k == N_FUNC) return JS_IsUndefined(e->fn->name);
    if (e->k == N_CLASS) return !e->a;
    return false;
}

/* gen e; if it is an anonymous function / class, name it */
static void gen_named(FS *fs, Node *e, JSValue name) {
    if (e->k == N_FUNC && JS_IsUndefined(e->fn->name) && JS_IsString(name)) {
        e->fn->name = name;
        gen_expr(fs, e);
        return;
    }
    if (e->k == N_CLASS && !e->a && JS_IsString(name)) { gen_class(fs, e, name); return; }
    gen_expr(fs, e);
}

static void gen_this(FS *fs, Binding *b) {
    if (!b) { emit(fs, OP_PUSH_THIS); return; }
    if (b->tdz) {
        if (b->fn == fs->f) emit16(fs, b->captured ? OP_GET_BOX_CHK : OP_GET_LOC_CHK, (uint32_t)b->slot);
        else emit16(fs, OP_GET_VREF_CHK, (uint32_t)vref_of(fs, b));
    } else gen_load_raw(fs, b);
}

static void gen_func_closure(FS *fs, Func *f);

static void gen_list_spread(FS *fs, Node *list, int *argc, bool *spread) {
    *spread = false;
    *argc = 0;
    for (Node *a = list; a; a = a->next) { if (a->k == N_SPREAD) *spread = true; (*argc)++; }
    if (*spread) {
        emit16(fs, OP_ARRAY_NEW, 0);
        add_sp(fs, 1);
        for (Node *a = list; a; a = a->next) {
            if (a->k == N_SPREAD) { gen_expr(fs, a->a); emit(fs, OP_ARRAY_SPREAD); }
            else { gen_expr(fs, a); emit(fs, OP_ARRAY_PUSH); }
        }
    } else for (Node *a = list; a; a = a->next) gen_expr(fs, a);
}

static void opt_check(FS *fs, Node *e) {
    if ((e->flags & NF_OPTIONAL) && fs->chain) jmps_add(fs->chain, emit_jump(fs, OP_JMP_NULLISH_UNDEF));
}

static uint32_t name_const(FS *fs, Node *callee) {
    if (callee->k == N_IDENT || callee->k == N_MEMBER || callee->k == N_PRIVMEMBER) return add_const(fs, callee->v);
    if (callee->k == N_SUPER_MEMBER && !(callee->flags & NF_COMPUTED)) return add_const(fs, callee->v);
    return 0xFFFFFFFFu;
}

static void gen_private_sym(FS *fs, Binding *b, JSValue name) { gen_load(fs, b, name, false); }

static void gen_call(FS *fs, Node *e) {
    Node *f = e->a;
    fs->cur_line = e->line;
    switch (f->k) {
    case N_MEMBER:
        gen_expr(fs, f->a);
        opt_check(fs, f);
        fs->cur_line = f->line;
        emit32(fs, OP_GET_FIELD_KEEP, add_const(fs, f->v));
        emit(fs, OP_SWAP);
        break;
    case N_INDEX:
        gen_expr(fs, f->a);
        opt_check(fs, f);
        gen_expr(fs, f->b);
        emit(fs, OP_GET_ELEM_KEEP);
        emit(fs, OP_SWAP);
        break;
    case N_PRIVMEMBER:
        gen_expr(fs, f->a);
        opt_check(fs, f);
        emit(fs, OP_DUP);
        gen_private_sym(fs, f->res, f->v);
        emit(fs, OP_GET_PRIVATE);
        emit(fs, OP_SWAP);
        break;
    case N_SUPER_MEMBER:
        gen_this(fs, f->res);
        emit(fs, OP_DUP);
        if (f->flags & NF_COMPUTED) { gen_expr(fs, f->a); emit(fs, OP_TO_KEY); }
        else emit_str(fs, f->v);
        emit(fs, OP_GET_SUPER);
        emit(fs, OP_SWAP);
        break;
    default:
        gen_expr(fs, f);
        emit(fs, OP_PUSH_UNDEF);
        break;
    }
    if ((e->flags & NF_OPTIONAL) && fs->chain) { jmps_add(fs->chain, emit_jump(fs, OP_JMP_NULLISH_UNDEF2)); }
    int argc;
    bool spread;
    gen_list_spread(fs, e->b, &argc, &spread);
    fs->cur_line = e->line;
    if (spread) { emit32(fs, OP_CALL_SPREAD, name_const(fs, f)); }
    else if (e->flags & NF_DIRECTEVAL) { emit16(fs, OP_EVAL, (uint32_t)argc); add_sp(fs, -(argc + 1)); }
    else { emit(fs, OP_CALL); put16(fs, (uint32_t)argc); put32(fs, name_const(fs, f)); add_sp(fs, -(argc + 1)); }
}

static void gen_template_obj(FS *fs, Node *t) {
    JSContext *ctx = fs->c->ctx;
    int n = 0;
    for (Node *q = t->a; q; q = q->next) n++;
    JSValue cooked = js_array_new(ctx, 0), raw = js_array_new(ctx, 0);
    if (JS_IsException(cooked) || JS_IsException(raw)) { cerr(fs->c, t->line, "out of memory"); return; }
    for (Node *q = t->a; q; q = q->next) {
        js_array_push_v(ctx, cooked, q->v);
        js_array_push_v(ctx, raw, q->v2);
    }
    OBJ(raw)->h.flags |= OF_FROZEN | OF_SEALED | OF_LENRO;
    OBJ(raw)->h.flags &= (uint8_t)~OF_EXT;
    js_add_prop(ctx, OBJ(cooked), ctx->a[A_raw], raw, 0);
    OBJ(cooked)->h.flags |= OF_FROZEN | OF_SEALED | OF_LENRO;
    OBJ(cooked)->h.flags &= (uint8_t)~OF_EXT;
    emit32(fs, OP_TEMPLATE, add_const_unique(fs, cooked));
    (void)n;
}

static void gen_tagged(FS *fs, Node *e) {
    Node *tag = e->a, *t = e->b;
    if (tag->k == N_MEMBER || tag->k == N_INDEX || tag->k == N_PRIVMEMBER) {
        Node call;
        memset(&call, 0, sizeof call);
        /* reuse the method-call prologue */
        gen_expr(fs, tag->a);
        if (tag->k == N_MEMBER) { emit32(fs, OP_GET_FIELD_KEEP, add_const(fs, tag->v)); }
        else if (tag->k == N_INDEX) { gen_expr(fs, tag->b); emit(fs, OP_GET_ELEM_KEEP); }
        else { emit(fs, OP_DUP); gen_private_sym(fs, tag->res, tag->v); emit(fs, OP_GET_PRIVATE); }
        emit(fs, OP_SWAP);
    } else {
        gen_expr(fs, tag);
        emit(fs, OP_PUSH_UNDEF);
    }
    gen_template_obj(fs, t);
    int argc = 1;
    for (Node *x = t->b; x; x = x->next) { gen_expr(fs, x); argc++; }
    emit(fs, OP_CALL);
    put16(fs, (uint32_t)argc);
    put32(fs, name_const(fs, tag));
    add_sp(fs, -(argc + 1));
}

static int binop(int op) {
    switch (op) {
    case '+': return OP_ADD; case '-': return OP_SUB; case '*': return OP_MUL; case '/': return OP_DIV; case '%': return OP_MOD;
    case TK_POW: return OP_POW; case '&': return OP_BAND; case '|': return OP_BOR; case '^': return OP_BXOR;
    case TK_SHL: return OP_SHL; case TK_SAR: return OP_SAR; case TK_SHR: return OP_SHR; case '<': return OP_LT;
    case '>': return OP_GT; case TK_LE: return OP_LE; case TK_GE: return OP_GE; case TK_EQ: return OP_EQ; case TK_NE: return OP_NE;
    case TK_SEQ: return OP_SEQ; case TK_SNE: return OP_SNE; case 'i': return OP_IN; case 'I': return OP_INSTANCEOF;
    default: return OP_NOP;
    }
}

/* push the reference parts of a target (obj / obj key / this key / obj sym); returns their count */
static int gen_ref(FS *fs, Node *t) {
    switch (t->k) {
    case N_MEMBER: gen_expr(fs, t->a); return 1;
    case N_INDEX: gen_expr(fs, t->a); gen_expr(fs, t->b); return 2;
    case N_PRIVMEMBER: gen_expr(fs, t->a); gen_private_sym(fs, t->res, t->v); return 2;
    case N_SUPER_MEMBER:
        gen_this(fs, t->res);
        if (t->flags & NF_COMPUTED) { gen_expr(fs, t->a); emit(fs, OP_TO_KEY); } else emit_str(fs, t->v);
        return 2;
    default: return 0;
    }
}

/* with the reference parts on the stack: duplicate them and read the value */
static void ref_get_keep(FS *fs, Node *t) {
    switch (t->k) {
    case N_MEMBER: emit(fs, OP_DUP); emit32(fs, OP_GET_FIELD, add_const(fs, t->v)); break;
    case N_INDEX: emit(fs, OP_DUP2); emit(fs, OP_GET_ELEM); break;
    case N_PRIVMEMBER: emit(fs, OP_DUP2); emit(fs, OP_GET_PRIVATE); break;
    case N_SUPER_MEMBER: emit(fs, OP_DUP2); emit(fs, OP_GET_SUPER); break;
    default: break;
    }
}

/* reference parts + value -> value */
static void ref_put(FS *fs, Node *t) {
    switch (t->k) {
    case N_MEMBER: emit32(fs, OP_PUT_FIELD, add_const(fs, t->v)); break;
    case N_INDEX: emit(fs, OP_PUT_ELEM); break;
    case N_PRIVMEMBER: emit(fs, OP_PUT_PRIVATE); break;
    case N_SUPER_MEMBER: emit(fs, OP_PUT_SUPER); break;
    default: break;
    }
}

static void gen_assign(FS *fs, Node *e, bool keep) {
    Node *t = e->a;
    int op = e->op;
    if (op == '=') {
        if (t->k == N_IDENT) {
            gen_named(fs, e->b, t->v);
            gen_store(fs, t->res, t->v, keep ? ST_SET : ST_PUT);
            return;
        }
        if (t->k == N_ARRAY_PAT || t->k == N_OBJECT_PAT) {
            gen_expr(fs, e->b);
            if (keep) emit(fs, OP_DUP);
            gen_destructure(fs, t, ST_PUT);
            return;
        }
        gen_ref(fs, t);
        gen_expr(fs, e->b);
        fs->cur_line = e->line;
        ref_put(fs, t);
        if (!keep) emit(fs, OP_POP);
        return;
    }
    if (op == TK_AND || op == TK_OR || op == TK_NULLISH) {
        int jop = op == TK_AND ? OP_JMP_FALSE_KEEP : op == TK_OR ? OP_JMP_TRUE_KEEP : OP_JMP_NOTNULLISH_KEEP;
        if (t->k == N_IDENT) {
            gen_load(fs, t->res, t->v, false);
            uint32_t j = emit_jump(fs, jop);
            gen_named(fs, e->b, t->v);
            gen_store(fs, t->res, t->v, ST_SET);
            patch_to(fs, j, fs->len);
            if (!keep) emit(fs, OP_POP);
            return;
        }
        int n = gen_ref(fs, t);
        ref_get_keep(fs, t);
        uint32_t j = emit_jump(fs, jop);
        gen_expr(fs, e->b);
        ref_put(fs, t);
        uint32_t j2 = emit_jump(fs, OP_JMP);
        add_sp(fs, n);
        patch_to(fs, j, fs->len);
        for (int i = 0; i < n; i++) emit(fs, OP_NIP);
        patch_to(fs, j2, fs->len);
        if (!keep) emit(fs, OP_POP);
        return;
    }
    if (t->k == N_IDENT) {
        gen_load(fs, t->res, t->v, false);
        gen_expr(fs, e->b);
        fs->cur_line = e->line;
        emit(fs, binop(op));
        gen_store(fs, t->res, t->v, keep ? ST_SET : ST_PUT);
        return;
    }
    gen_ref(fs, t);
    ref_get_keep(fs, t);
    gen_expr(fs, e->b);
    fs->cur_line = e->line;
    emit(fs, binop(op));
    ref_put(fs, t);
    if (!keep) emit(fs, OP_POP);
}

static void gen_update(FS *fs, Node *e, bool keep) {
    Node *t = e->a;
    int op = e->op == '+' ? OP_INC : OP_DEC;
    bool prefix = (e->flags & NF_PREFIX) || !keep;
    if (t->k == N_IDENT) {
        gen_load(fs, t->res, t->v, false);
        if (!prefix) { emit(fs, OP_TO_NUMERIC); emit(fs, OP_DUP); }
        emit(fs, op);
        gen_store(fs, t->res, t->v, prefix && keep ? ST_SET : ST_PUT);
        return;
    }
    gen_ref(fs, t);
    ref_get_keep(fs, t);
    if (prefix) {
        emit(fs, op);
        ref_put(fs, t);
        if (!keep) emit(fs, OP_POP);
        return;
    }
    int tmp = tmp_alloc(fs);
    emit(fs, OP_TO_NUMERIC);
    emit16(fs, OP_SET_LOC, (uint32_t)tmp);
    emit(fs, op);
    ref_put(fs, t);
    emit(fs, OP_POP);
    emit16(fs, OP_GET_LOC, (uint32_t)tmp);
    tmp_free(fs, tmp);
}

static void gen_delete(FS *fs, Node *a) {
    switch (a->k) {
    case N_MEMBER: gen_expr(fs, a->a); emit32(fs, OP_DELETE_FIELD, add_const(fs, a->v)); return;
    case N_INDEX: gen_expr(fs, a->a); gen_expr(fs, a->b); emit(fs, OP_DELETE_ELEM); return;
    case N_IDENT:
        if (!a->res || ((Binding *)a->res)->global) emit32(fs, OP_DELETE_GLOBAL, add_const(fs, a->v));
        else emit(fs, OP_PUSH_FALSE);
        return;
    case N_OPTCHAIN: {
        Jmps *save = fs->chain, j;
        memset(&j, 0, sizeof j);
        fs->chain = &j;
        Node *in = a->a;
        if (in->k == N_MEMBER) { gen_expr(fs, in->a); opt_check(fs, in); emit32(fs, OP_DELETE_FIELD, add_const(fs, in->v)); }
        else if (in->k == N_INDEX) { gen_expr(fs, in->a); opt_check(fs, in); gen_expr(fs, in->b); emit(fs, OP_DELETE_ELEM); }
        else { gen_expr(fs, in); emit(fs, OP_POP); emit(fs, OP_PUSH_TRUE); }
        uint32_t skip = emit_jump(fs, OP_JMP);
        jmps_patch(fs, &j, fs->len);
        emit(fs, OP_POP);
        emit(fs, OP_PUSH_TRUE);
        patch_to(fs, skip, fs->len);
        fs->chain = save;
        return;
    }
    case N_SUPER_MEMBER:
        emit32(fs, OP_THROW_REF, add_const(fs, js_atomc(fs->c->ctx, "Unsupported reference to 'super'")));
        emit(fs, OP_PUSH_TRUE);
        return;
    default: gen_expr(fs, a); emit(fs, OP_POP); emit(fs, OP_PUSH_TRUE); return;
    }
}

static void gen_object(FS *fs, Node *e) {
    emit(fs, OP_OBJECT_NEW);
    for (Node *pr = e->a; pr; pr = pr->next) {
        fs->cur_line = pr->line;
        switch (pr->op) {
        case PK_INIT:
            if (pr->flags & NF_COMPUTED) {
                gen_expr(fs, pr->a);
                emit(fs, OP_TO_KEY);
                gen_expr(fs, pr->b);
                if (anon_fn(pr->b)) emit(fs, OP_SET_NAME_ELEM);
                emit(fs, OP_DEFINE_ELEM);
            } else {
                gen_named(fs, pr->b, pr->a->v);
                emit32(fs, OP_DEFINE_FIELD, add_const(fs, pr->a->v));
            }
            break;
        case PK_PROTO:
            gen_expr(fs, pr->b);
            emit(fs, OP_SET_PROTO);
            break;
        case PK_SPREAD:
            gen_expr(fs, pr->b);
            emit(fs, OP_COPY_DATA_PROPS);
            break;
        default: {
            int kind = pr->op == PK_GET ? 1 : pr->op == PK_SET ? 2 : 0;
            if (pr->flags & NF_COMPUTED) {
                gen_expr(fs, pr->a);
                emit(fs, OP_TO_KEY);
                gen_func_closure(fs, pr->b->fn);
                emit8(fs, OP_DEFINE_METHOD_ELEM, (uint32_t)(kind | 4));
            } else {
                if (JS_IsUndefined(pr->b->fn->name)) pr->b->fn->name = pr->a->v;
                gen_func_closure(fs, pr->b->fn);
                emit(fs, OP_DEFINE_METHOD);
                put32(fs, add_const(fs, pr->a->v));
                put8(fs, (uint32_t)(kind | 4));
            }
            break;
        }
        }
    }
}

static void gen_array(FS *fs, Node *e) {
    int n = 0;
    bool simple = true;
    for (Node *x = e->a; x; x = x->next) { if (x->k == N_SPREAD || x->k == N_HOLE) simple = false; n++; }
    if (simple && n < 60000) {
        for (Node *x = e->a; x; x = x->next) gen_expr(fs, x);
        emit16(fs, OP_ARRAY_NEW, (uint32_t)n);
        add_sp(fs, 1 - n);
        return;
    }
    emit16(fs, OP_ARRAY_NEW, 0);
    add_sp(fs, 1);
    for (Node *x = e->a; x; x = x->next) {
        if (x->k == N_HOLE) emit(fs, OP_ARRAY_HOLE);
        else if (x->k == N_SPREAD) { gen_expr(fs, x->a); emit(fs, OP_ARRAY_SPREAD); }
        else { gen_expr(fs, x); emit(fs, OP_ARRAY_PUSH); }
    }
}

static void gen_yield_return_stub(FS *fs, uint32_t at) {
    /* YIELD's operand: where a gen.return() resumption goes (0 = the VM returns by itself) */
    if (!need_cleanup_for_return(fs) && fs->f->kind != FK_DERIVED) return;
    uint32_t skip = emit_jump(fs, OP_JMP);
    patch_to(fs, at, fs->len);
    int save = fs->sp;
    gen_return_value(fs);
    fs->sp = save;
    patch_to(fs, skip, fs->len);
}

static void gen_expr(FS *fs, Node *e) {
    if (fs->c->failed || !e) { if (!e) emit(fs, OP_PUSH_UNDEF); return; }
    int saved_line = fs->cur_line;
    if (e->line) fs->cur_line = e->line;
    switch (e->k) {
    case N_NUM:
        if (e->op == 1) emit(fs, OP_PUSH_NULL);
        else if (e->op == 2) emit(fs, OP_PUSH_FALSE);
        else if (e->op == 3) emit(fs, OP_PUSH_TRUE);
        else emit_num(fs, e->num);
        break;
    case N_BIGINT: emit_num(fs, e->num); break;
    case N_STR: emit_str(fs, e->v); break;
    case N_TEMPLATE: {
        Node *q = e->a, *x = e->b;
        emit_str(fs, q->v);
        for (; x; x = x->next) {
            q = q->next;
            gen_expr(fs, x);
            emit(fs, OP_TO_STRING);
            emit(fs, OP_ADD);
            if (q && js_strlen(q->v)) { emit_str(fs, q->v); emit(fs, OP_ADD); }
        }
        break;
    }
    case N_TAGGED: gen_tagged(fs, e); break;
    case N_REGEXP: {
        uint32_t i = add_const_unique(fs, e->v);
        add_const_unique(fs, e->v2);
        emit32(fs, OP_REGEXP, i);
        break;
    }
    case N_IDENT: gen_load(fs, e->res, e->v, false); break;
    case N_THIS: gen_this(fs, e->res); break;
    case N_NEW_TARGET:
        if (e->res) gen_load_raw(fs, e->res); else emit(fs, OP_PUSH_NEW_TARGET);
        break;
    case N_ARRAY: gen_array(fs, e); break;
    case N_OBJECT: gen_object(fs, e); break;
    case N_FUNC: gen_func_closure(fs, e->fn); break;
    case N_CLASS: gen_class(fs, e, JS_UNDEFINED); break;
    case N_UNARY:
        if (e->op == 'd') { gen_delete(fs, e->a); break; }
        if (e->op == 't' && e->a->k == N_IDENT) { gen_load(fs, e->a->res, e->a->v, true); emit(fs, OP_TYPEOF); break; }
        gen_expr(fs, e->a);
        switch (e->op) {
        case '-': emit(fs, OP_NEG); break;
        case '+': emit(fs, OP_PLUS); break;
        case '!': emit(fs, OP_NOT); break;
        case '~': emit(fs, OP_BNOT); break;
        case 't': emit(fs, OP_TYPEOF); break;
        case 'v': emit(fs, OP_POP); emit(fs, OP_PUSH_UNDEF); break;
        }
        break;
    case N_UPDATE: gen_update(fs, e, true); break;
    case N_BINARY:
        gen_expr(fs, e->a);
        gen_expr(fs, e->b);
        fs->cur_line = e->line;
        emit(fs, binop(e->op));
        break;
    case N_PRIVIN:
        gen_private_sym(fs, e->res, e->v);
        gen_expr(fs, e->a);
        emit(fs, OP_PRIVATE_IN);
        break;
    case N_LOGICAL: {
        gen_expr(fs, e->a);
        uint32_t j = emit_jump(fs, e->op == TK_AND ? OP_JMP_FALSE_KEEP : e->op == TK_OR ? OP_JMP_TRUE_KEEP : OP_JMP_NOTNULLISH_KEEP);
        gen_expr(fs, e->b);
        patch_to(fs, j, fs->len);
        break;
    }
    case N_ASSIGN: gen_assign(fs, e, true); break;
    case N_COND: {
        gen_expr(fs, e->a);
        uint32_t j = emit_jump(fs, OP_JMP_FALSE);
        gen_expr(fs, e->b);
        uint32_t j2 = emit_jump(fs, OP_JMP);
        add_sp(fs, -1);
        patch_to(fs, j, fs->len);
        gen_expr(fs, e->c);
        patch_to(fs, j2, fs->len);
        break;
    }
    case N_CALL: gen_call(fs, e); break;
    case N_SUPER_CALL: {
        Func *g = this_func(fs->f);
        int argc;
        bool spread;
        gen_list_spread(fs, e->b, &argc, &spread);
        if (g != fs->f) {
            /* inside an arrow: new.target and the constructor come from hidden bindings */
            gen_load_raw(fs, g->nt_b);
            gen_load_raw(fs, g->func_b);
            if (spread) { emit8(fs, OP_SUPER_CALL_SPREAD, 1); add_sp(fs, -2); }
            else { emit16(fs, OP_SUPER_CALL, (uint32_t)argc | 0x8000); add_sp(fs, -(argc + 2) + 1); }
        } else if (spread) emit8(fs, OP_SUPER_CALL_SPREAD, 0);
        else { emit16(fs, OP_SUPER_CALL, (uint32_t)argc); add_sp(fs, -argc + 1); }
        /* bind this (once), then run the field initialisers */
        emit(fs, OP_DUP);
        gen_load_raw(fs, e->res);
        emit(fs, OP_CHECK_THIS_INIT);
        {
            Binding *tb = e->res;
            if (tb->fn == fs->f) emit16(fs, tb->captured ? OP_INIT_BOX : OP_PUT_LOC, (uint32_t)tb->slot);
            else emit16(fs, OP_PUT_VREF, (uint32_t)vref_of(fs, tb));
        }
        if (g != fs->f) gen_load_raw(fs, g->func_b); else emit(fs, OP_PUSH_FUNC);
        emit(fs, OP_RUN_FIELDS);
        break;
    }
    case N_NEW: {
        gen_expr(fs, e->a);
        emit(fs, OP_DUP);
        int argc;
        bool spread;
        gen_list_spread(fs, e->b, &argc, &spread);
        fs->cur_line = e->line;
        if (spread) emit(fs, OP_NEW_SPREAD);
        else { emit16(fs, OP_NEW, (uint32_t)argc); add_sp(fs, -(argc + 1)); }
        break;
    }
    case N_MEMBER:
        gen_expr(fs, e->a);
        opt_check(fs, e);
        fs->cur_line = e->line;
        emit32(fs, OP_GET_FIELD, add_const(fs, e->v));
        break;
    case N_INDEX:
        gen_expr(fs, e->a);
        opt_check(fs, e);
        gen_expr(fs, e->b);
        fs->cur_line = e->line;
        emit(fs, OP_GET_ELEM);
        break;
    case N_PRIVMEMBER:
        gen_expr(fs, e->a);
        opt_check(fs, e);
        gen_private_sym(fs, e->res, e->v);
        emit(fs, OP_GET_PRIVATE);
        break;
    case N_SUPER_MEMBER:
        gen_this(fs, e->res);
        if (e->flags & NF_COMPUTED) { gen_expr(fs, e->a); emit(fs, OP_TO_KEY); } else emit_str(fs, e->v);
        emit(fs, OP_GET_SUPER);
        break;
    case N_OPTCHAIN: {
        Jmps *save = fs->chain, j;
        memset(&j, 0, sizeof j);
        fs->chain = &j;
        gen_expr(fs, e->a);
        jmps_patch(fs, &j, fs->len);
        fs->chain = save;
        break;
    }
    case N_SEQ:
        for (Node *x = e->a; x; x = x->next) { gen_expr(fs, x); if (x->next) emit(fs, OP_POP); }
        break;
    case N_YIELD:
        if (e->flags & NF_DELEGATE) {
            gen_expr(fs, e->a);
            if (fs->f->flags & F_ASYNC) emit(fs, OP_FOR_AWAIT_START); else emit(fs, OP_FOR_OF_START);
            emit(fs, OP_PUSH_UNDEF);
            uint32_t at = emit_jump(fs, (fs->f->flags & F_ASYNC) ? OP_ASYNC_YIELD_STAR : OP_YIELD_STAR);
            gen_yield_return_stub(fs, at);
        } else {
            if (e->a) gen_expr(fs, e->a); else emit(fs, OP_PUSH_UNDEF);
            if (fs->f->flags & F_ASYNC) emit(fs, OP_AWAIT);
            uint32_t at = emit_jump(fs, OP_YIELD);
            gen_yield_return_stub(fs, at);
        }
        break;
    case N_AWAIT:
        gen_expr(fs, e->a);
        emit(fs, OP_AWAIT);
        break;
    case N_IMPORT_CALL: gen_expr(fs, e->a); emit(fs, OP_IMPORT_CALL); break;
    case N_IMPORT_META: emit(fs, OP_OBJECT_NEW); break;
    case N_SPREAD: cerr(fs->c, e->line, "Unexpected spread"); break;
    case N_ARRAY_PAT: case N_OBJECT_PAT: case N_ASSIGN_PAT: cerr(fs->c, e->line, "Invalid destructuring assignment target"); break;
    case N_EMPTY: emit(fs, OP_PUSH_UNDEF); break;
    default: cerr(fs->c, e->line, "unsupported expression (%d)", e->k); break;
    }
    fs->cur_line = saved_line;
}

/* evaluate for side effects only */
static void gen_expr_discard(FS *fs, Node *e) {
    if (e->k == N_ASSIGN) { int l = fs->cur_line; fs->cur_line = e->line; gen_assign(fs, e, false); fs->cur_line = l; return; }
    if (e->k == N_UPDATE) { gen_update(fs, e, false); return; }
    gen_expr(fs, e);
    emit(fs, OP_POP);
}

/* ---- destructuring ---------------------------------------------------------------------------------- */
static void gen_target_store(FS *fs, Node *t, int mode) {
    if (t->k == N_IDENT) { gen_store(fs, t->res, t->v, mode); return; }
    if (t->k == N_ARRAY_PAT || t->k == N_OBJECT_PAT || t->k == N_ASSIGN_PAT) { gen_destructure(fs, t, mode); return; }
    /* member target: value is on the stack; evaluate the reference, then move the value up */
    int n = gen_ref(fs, t);
    if (n == 1) emit(fs, OP_SWAP);
    else if (n == 2) emit(fs, OP_ROT3L);
    else { cerr(fs->c, t->line, "Invalid destructuring assignment target"); return; }
    ref_put(fs, t);
    emit(fs, OP_POP);
}

static void gen_destructure(FS *fs, Node *pat, int mode) {
    switch (pat->k) {
    case N_IDENT: gen_store(fs, pat->res, pat->v, mode); return;
    case N_ASSIGN_PAT: {
        emit(fs, OP_DUP);
        emit(fs, OP_IS_UNDEF);
        uint32_t j = emit_jump(fs, OP_JMP_FALSE);
        emit(fs, OP_POP);
        gen_named(fs, pat->b, pat->a->k == N_IDENT ? pat->a->v : JS_UNDEFINED);
        patch_to(fs, j, fs->len);
        gen_target_store(fs, pat->a, mode);
        return;
    }
    case N_ARRAY_PAT:
        emit(fs, OP_FOR_OF_START);
        emit(fs, OP_PUSH_FALSE);
        for (Node *el = pat->a; el; el = el->next) {
            if (el->k == N_HOLE) { emit(fs, OP_ITER_STEP); emit(fs, OP_POP); continue; }
            if (el->k == N_REST) { emit(fs, OP_ITER_REST); gen_target_store(fs, el->a, mode); continue; }
            emit(fs, OP_ITER_STEP);
            gen_target_store(fs, el, mode);
        }
        emit(fs, OP_ITER_END);
        return;
    case N_OBJECT_PAT: {
        emit(fs, OP_REQUIRE_COERCIBLE);
        bool rest = false;
        int nkeys = 0;
        for (Node *pr = pat->a; pr; pr = pr->next) { if (pr->op == PK_REST) rest = true; else nkeys++; }
        int ktmp[64];
        int nk = 0;
        for (Node *pr = pat->a; pr; pr = pr->next) {
            if (pr->op == PK_REST) {
                /* [v] -> {} with the remaining own enumerable properties */
                emit(fs, OP_OBJECT_NEW);
                emit(fs, OP_SWAP);
                emit16(fs, OP_ARRAY_NEW, 0);
                add_sp(fs, 1);
                int ki = 0;
                for (Node *q = pat->a; q; q = q->next) {
                    if (q->op == PK_REST) continue;
                    if (q->flags & NF_COMPUTED) { if (ki < nk) emit16(fs, OP_GET_LOC, (uint32_t)ktmp[ki]); ki++; }
                    else emit_str(fs, q->a->v);
                    emit(fs, OP_ARRAY_PUSH);
                }
                emit(fs, OP_COPY_DATA_EXCL);
                gen_target_store(fs, pr->b, mode);
                for (int i = 0; i < nk; i++) tmp_free(fs, ktmp[i]);
                return;
            }
            emit(fs, OP_DUP);
            if (pr->flags & NF_COMPUTED) {
                gen_expr(fs, pr->a);
                emit(fs, OP_TO_KEY);
                if (rest && nk < 64) { ktmp[nk] = tmp_alloc(fs); emit16(fs, OP_SET_LOC, (uint32_t)ktmp[nk]); nk++; }
                emit(fs, OP_GET_ELEM);
            } else emit32(fs, OP_GET_FIELD, add_const(fs, pr->a->v));
            Node *t = pr->b;
            if (t->k == N_ASSIGN_PAT && t->a->k == N_IDENT) {
                emit(fs, OP_DUP);
                emit(fs, OP_IS_UNDEF);
                uint32_t j = emit_jump(fs, OP_JMP_FALSE);
                emit(fs, OP_POP);
                gen_named(fs, t->b, t->a->v);
                patch_to(fs, j, fs->len);
                gen_store(fs, t->a->res, t->a->v, mode);
            } else gen_target_store(fs, t, mode);
        }
        (void)nkeys;
        emit(fs, OP_POP);
        return;
    }
    default:
        gen_target_store(fs, pat, mode);
        return;
    }
}

/* ---- classes ------------------------------------------------------------------------------------------- */
static JSBytecode *compile_func(Comp *c, Func *f, FS *parent);

static void gen_func_closure(FS *fs, Func *f) {
    if (!f->bc) f->bc = compile_func(fs->c, f, fs);
    if (fs->c->failed || !f->bc) { emit(fs, OP_PUSH_UNDEF); return; }
    emit32(fs, OP_CLOSURE, add_const_unique(fs, JS_MKINT(f->bc)));
}

static Func *default_ctor(Comp *c, Node *cls, JSValue name) {
    Func *f = calloc_p(c, sizeof(Func));
    if (!f) return NULL;
    f->kind = cls->b ? FK_DERIVED : FK_CTOR;
    f->flags = F_STRICT | F_CLASSCTOR | (cls->b ? F_DERIVED : F_CTOR);
    f->name = name;
    f->simple_params = true;
    f->start = cls->pos;
    f->end = (uint32_t)cls->num;
    f->line = cls->line;
    f->cls = cls;
    f->fields = cls->d ? cls->d->fn : NULL;
    f->rest_slot = -2;   /* marks "default derived constructor: forward the arguments" */
    return f;
}

static void gen_class(FS *fs, Node *cls, JSValue name) {
    Comp *c = fs->c;
    Scope *K = cls->res;
    JSValue cname = cls->a ? cls->a->v : name;
    gen_scope_entry(fs, K);
    /* private names: one symbol each per class evaluation */
    for (Binding *b = K ? K->b : NULL; b; b = b->next) {
        if (b->kind != BK_PRIVATE) continue;
        emit32(fs, OP_NEW_PRIVATE, add_const(fs, b->name));
        if (b->captured) emit16(fs, OP_INIT_BOX, (uint32_t)b->slot); else emit16(fs, OP_PUT_LOC, (uint32_t)b->slot);
    }
    if (cls->b) gen_expr(fs, cls->b); else emit(fs, OP_PUSH_UNDEF);
    Func *ctor = cls->fn;
    if (!ctor) {
        ctor = default_ctor(c, cls, cname);
        if (!ctor) return;
        ctor->parent = fs->f;
        cls->fn = ctor;
        res_func(c, ctor, K);
    } else ctor->name = cname;
    if (JS_IsUndefined(ctor->name)) ctor->name = c->ctx->empty_str;
    gen_func_closure(fs, ctor);
    emit8(fs, OP_CLASS, cls->b ? 1 : 0);            /* [heritage ctor] -> [ctor proto] */
    add_sp(fs, 0);
    for (Node *el = cls->c; el; el = el->next) {
        fs->cur_line = el->line;
        bool st = (el->flags & NF_STATIC) != 0;
        if (el->op == PK_FIELD) {
            if (el->flags & NF_COMPUTED) {
                gen_expr(fs, el->a);
                emit(fs, OP_TO_KEY);
                Binding *kb = scope_find(K, el->v);
                gen_store(fs, kb, el->v, ST_INIT);
            }
            continue;
        }
        int kind = el->op == PK_GET ? 1 : el->op == PK_SET ? 2 : 0;
        if (st) emit(fs, OP_SWAP);
        if (el->flags & NF_PRIVATE) {
            gen_private_sym(fs, el->res, el->a->v);
            Func *mf = el->b->fn;
            if (JS_IsUndefined(mf->name)) mf->name = el->a->v;
            gen_func_closure(fs, mf);
            emit8(fs, OP_DEFINE_METHOD_ELEM, (uint32_t)kind);
        } else if (el->flags & NF_COMPUTED) {
            gen_expr(fs, el->a);
            emit(fs, OP_TO_KEY);
            gen_func_closure(fs, el->b->fn);
            emit8(fs, OP_DEFINE_METHOD_ELEM, (uint32_t)kind);
        } else {
            Func *mf = el->b->fn;
            if (JS_IsUndefined(mf->name)) mf->name = el->a->v;
            gen_func_closure(fs, mf);
            emit(fs, OP_DEFINE_METHOD);
            put32(fs, add_const(fs, el->a->v));
            put8(fs, (uint32_t)kind);
        }
        if (st) emit(fs, OP_SWAP);
    }
    Func *inst = cls->d ? cls->d->fn : NULL, *stat = cls->d && cls->d->next ? cls->d->next->fn : NULL;
    if (inst) {
        /* [ctor proto]: the initialiser's home is the prototype */
        emit(fs, OP_SWAP);                 /* proto ctor */
        emit(fs, OP_OVER);                 /* proto ctor proto */
        gen_func_closure(fs, inst);        /* proto ctor proto fn */
        emit(fs, OP_SET_HOME);             /* proto ctor proto fn */
        emit(fs, OP_NIP);                  /* proto ctor fn */
        emit(fs, OP_SET_FIELDS);           /* proto ctor */
        emit(fs, OP_SWAP);                 /* ctor proto */
    }
    emit(fs, OP_POP);                      /* ctor */
    if (cls->a && cls->a->res) { emit(fs, OP_DUP); gen_store(fs, cls->a->res, cls->a->v, ST_INIT); }
    if (stat) {
        emit(fs, OP_DUP);                  /* ctor ctor */
        gen_func_closure(fs, stat);        /* ctor ctor fn */
        emit(fs, OP_SET_HOME);             /* ctor ctor fn (home = ctor) */
        emit(fs, OP_SWAP);                 /* ctor fn ctor */
        emit(fs, OP_CALL);
        put16(fs, 0);
        put32(fs, 0xFFFFFFFFu);
        add_sp(fs, -1);
        emit(fs, OP_POP);
    }
}

/* ---- statements ---------------------------------------------------------------------------------------- */
static void gen_block_body(FS *fs, Node *list) { for (Node *s = list; s; s = s->next) gen_stmt(fs, s); }

static void gen_var(FS *fs, Node *n) {
    int mode = ST_INIT;
    for (Node *d = n->a; d; d = d->next) {
        fs->cur_line = d->line ? d->line : n->line;
        if (n->op == VK_VAR) {
            if (!d->b) continue;
            if (d->a->k == N_IDENT) { gen_named(fs, d->b, d->a->v); gen_store(fs, d->a->res, d->a->v, ST_PUT); }
            else { gen_expr(fs, d->b); gen_destructure(fs, d->a, ST_PUT); }
            continue;
        }
        if (d->b) {
            if (d->a->k == N_IDENT) gen_named(fs, d->b, d->a->v);
            else gen_expr(fs, d->b);
        } else emit(fs, OP_PUSH_UNDEF);
        gen_destructure(fs, d->a, mode);
    }
}

static void loop_end(FS *fs, Ctl *ctl, uint32_t cont_target, uint32_t brk_target) {
    jmps_patch(fs, &ctl->cont, cont_target);
    jmps_patch(fs, &ctl->brk, brk_target);
}

static void copy_boxes(FS *fs, Scope *s) {
    if (!s || !s->has_captured) return;
    for (Binding *b = s->b; b; b = b->next) if (b->captured && b->fn == fs->f) emit16(fs, OP_COPY_BOX, (uint32_t)b->slot);
}

static void gen_for(FS *fs, Node *n) {
    Scope *h = n->res;
    gen_scope_entry(fs, h);
    if (n->a) {
        if (n->a->k == N_VAR) gen_var(fs, n->a);
        else gen_expr_discard(fs, n->a);
    }
    Ctl ctl;
    ctl_push(fs, &ctl, CT_LOOP, true);
    copy_boxes(fs, h);
    uint32_t top = fs->len;
    uint32_t jexit = 0;
    bool has_test = n->b != NULL;
    if (has_test) { gen_expr(fs, n->b); jexit = emit_jump(fs, OP_JMP_FALSE); }
    gen_stmt(fs, n->d);
    uint32_t cont = fs->len;
    copy_boxes(fs, h);
    if (n->c) gen_expr_discard(fs, n->c);
    emit(fs, OP_LOOP_HINT);
    emit_back_jump(fs, OP_JMP, top);
    uint32_t end = fs->len;
    if (has_test) patch_to(fs, jexit, end);
    loop_end(fs, &ctl, cont, end);
    ctl_pop(fs);
}

static void gen_for_in_of(FS *fs, Node *n) {
    bool of = n->k == N_FOROF;
    bool aw = (n->flags & NF_AWAIT) != 0;
    Scope *h = n->res;
    /* the right side is evaluated with the loop bindings in TDZ */
    gen_scope_entry(fs, h);
    gen_expr(fs, n->b);
    if (of) emit(fs, aw ? OP_FOR_AWAIT_START : OP_FOR_OF_START);
    else emit(fs, OP_FOR_IN_START);
    Ctl ctl;
    ctl_push(fs, &ctl, of ? CT_FOROF : CT_FORIN, true);
    /* a throw out of a for-of body closes the iterator */
    Ctl tctl;
    uint32_t jtry = 0;
    bool guard = of && !aw;
    uint32_t top = fs->len;
    uint32_t jexit;
    if (aw) {
        emit(fs, OP_ITER_NEXT_RAW);
        emit(fs, OP_AWAIT);
        jexit = emit_jump(fs, OP_ITER_RESULT);
    } else jexit = emit_jump(fs, of ? OP_FOR_OF_NEXT : OP_FOR_IN_NEXT);
    /* fresh bindings each iteration */
    if (h) for (Binding *b = h->b; b; b = b->next) if (b->captured && b->fn == fs->f) emit16(fs, b->tdz ? OP_NEW_BOX : OP_NEW_BOX_UNDEF, (uint32_t)b->slot);
    Node *lhs = n->a;
    if (lhs->k == N_VAR) gen_destructure(fs, lhs->a->a, lhs->op == VK_VAR ? ST_PUT : ST_INIT);
    else gen_target_store(fs, lhs, ST_PUT);
    /* a throw out of a for-of body closes the iterator */
    if (guard) { jtry = emit_jump(fs, OP_TRY); ctl_push(fs, &tctl, CT_TRY, false); }
    gen_stmt(fs, n->d);
    if (guard) {
        emit(fs, OP_END_TRY);
        ctl_pop(fs);
    }
    uint32_t cont = fs->len;
    emit(fs, OP_LOOP_HINT);
    emit_back_jump(fs, OP_JMP, top);
    if (guard) {
        /* the exception path: [iter next exc] */
        patch_to(fs, jtry, fs->len);
        int save = fs->sp;
        fs->sp = ctl.sp + 1;
        add_sp(fs, 0);
        emit(fs, OP_ROT3L);          /* next exc iter */
        emit(fs, OP_ROT3L);          /* exc iter next */
        emit(fs, OP_ITER_CLOSE_EXC);
        emit(fs, OP_THROW);
        fs->sp = save;
    }
    /* the loop ended: the iterator (for-of: and next) is still on the stack */
    uint32_t done = fs->len;
    patch_to(fs, jexit, done);
    fs->sp = ctl.sp;
    if (of) { emit(fs, OP_POP); emit(fs, OP_POP); }
    else emit(fs, OP_POP);
    uint32_t end = fs->len;
    /* continue jumps back to the top (where next() is called) */
    jmps_patch(fs, &ctl.cont, cont);
    jmps_patch(fs, &ctl.brk, end);
    ctl_pop(fs);
}

static void gen_try(FS *fs, Node *n) {
    bool has_fin = n->d != NULL;
    Ctl fin, cat;
    uint32_t jfin = 0, jcatch = 0;
    int base = fs->sp;
    if (has_fin) { jfin = emit_jump(fs, OP_TRY); ctl_push(fs, &fin, CT_FINALLY, false); }
    if (n->c) { jcatch = emit_jump(fs, OP_TRY); ctl_push(fs, &cat, CT_TRY, false); }
    gen_stmt(fs, n->a);
    uint32_t jok = 0;
    if (n->c) {
        emit(fs, OP_END_TRY);
        ctl_pop(fs);
        jok = emit_jump(fs, OP_JMP);
        /* catch: [ (fin marker) exc ] */
        patch_to(fs, jcatch, fs->len);
        fs->sp = base + (has_fin ? 1 : 0) + 1;
        if (fs->sp > fs->max_sp) fs->max_sp = fs->sp;
        Scope *cs = n->res;
        gen_scope_entry(fs, cs);
        if (n->b) gen_destructure(fs, n->b, ST_INIT);
        else emit(fs, OP_POP);
        gen_stmt(fs, n->c);
        patch_to(fs, jok, fs->len);
    }
    if (has_fin) {
        emit(fs, OP_END_TRY);
        ctl_pop(fs);
        emit(fs, OP_PUSH_UNDEF);
        jmps_add(&fin.fin, emit_jump(fs, OP_GOSUB));
        emit(fs, OP_POP);
        uint32_t jend = emit_jump(fs, OP_JMP);
        /* exception path: [exc] */
        patch_to(fs, jfin, fs->len);
        fs->sp = base + 1;
        jmps_add(&fin.fin, emit_jump(fs, OP_GOSUB));
        emit(fs, OP_THROW);
        /* the finally subroutine: [x retaddr] */
        uint32_t sub = fs->len;
        fs->sp = base + 2;
        if (fs->sp > fs->max_sp) fs->max_sp = fs->sp;
        Ctl sctl;
        ctl_push(fs, &sctl, CT_SUB, false);
        gen_stmt(fs, n->d);
        ctl_pop(fs);
        emit(fs, OP_RET);
        jmps_patch(fs, &fin.fin, sub);
        patch_to(fs, jend, fs->len);
        fs->sp = base;
    } else fs->sp = base;
}

static void gen_switch(FS *fs, Node *n) {
    gen_expr(fs, n->a);
    int tmp = tmp_alloc(fs);
    emit16(fs, OP_PUT_LOC, (uint32_t)tmp);
    gen_scope_entry(fs, n->res);
    Ctl ctl;
    ctl_push(fs, &ctl, CT_SWITCH, false);
    int ncase = 0;
    for (Node *cs = n->b; cs; cs = cs->next) ncase++;
    uint32_t *jumps = calloc((size_t)ncase + 1, sizeof(uint32_t));
    if (!jumps) { cerr(fs->c, n->line, "out of memory"); return; }
    int i = 0;
    Node *def = NULL;
    for (Node *cs = n->b; cs; cs = cs->next, i++) {
        if (!cs->a) { def = cs; continue; }
        emit16(fs, OP_GET_LOC, (uint32_t)tmp);
        gen_expr(fs, cs->a);
        emit(fs, OP_SEQ);
        jumps[i] = emit_jump(fs, OP_JMP_TRUE);
    }
    uint32_t jdef = emit_jump(fs, OP_JMP);
    i = 0;
    for (Node *cs = n->b; cs; cs = cs->next, i++) {
        if (cs == def) patch_to(fs, jdef, fs->len);
        else patch_to(fs, jumps[i], fs->len);
        gen_block_body(fs, cs->b);
    }
    uint32_t end = fs->len;
    if (!def) patch_to(fs, jdef, end);
    free(jumps);
    jmps_patch(fs, &ctl.brk, end);
    ctl_pop(fs);
    tmp_free(fs, tmp);
}

static void gen_stmt(FS *fs, Node *n) {
    if (fs->c->failed || !n) return;
    if (n->line) fs->cur_line = n->line;
    switch (n->k) {
    case N_EMPTY: case N_DEBUGGER: break;
    case N_EXPR:
        if (fs->completion >= 0) { gen_expr(fs, n->a); emit16(fs, OP_PUT_LOC, (uint32_t)fs->completion); }
        else gen_expr_discard(fs, n->a);
        break;
    case N_VAR: gen_var(fs, n); break;
    case N_FUNC_DECL:
        if (n->flags & NF_ANNEXB) {
            /* copy the block function to the function-level var */
            Binding *vb = NULL;
            for (Scope *s = fs->f->scope; s && !vb; s = NULL) vb = scope_find(s, n->fn->name);
            gen_load(fs, n->res, n->fn->name, false);
            gen_store(fs, vb, n->fn->name, ST_PUT);
        }
        break;
    case N_CLASS_DECL:
        gen_class(fs, n->a, JS_UNDEFINED);
        gen_store(fs, n->res, n->a->a->v, ST_INIT);
        break;
    case N_BLOCK:
        if (fs->npending) {
            Ctl ctl;
            ctl_push(fs, &ctl, CT_BLOCK, false);
            gen_scope_entry(fs, n->res);
            gen_block_body(fs, n->a);
            jmps_patch(fs, &ctl.brk, fs->len);
            ctl_pop(fs);
        } else {
            gen_scope_entry(fs, n->res);
            gen_block_body(fs, n->a);
        }
        break;
    case N_IF: {
        gen_expr(fs, n->a);
        uint32_t j = emit_jump(fs, OP_JMP_FALSE);
        gen_stmt(fs, n->b);
        if (n->c) {
            uint32_t j2 = emit_jump(fs, OP_JMP);
            patch_to(fs, j, fs->len);
            gen_stmt(fs, n->c);
            patch_to(fs, j2, fs->len);
        } else patch_to(fs, j, fs->len);
        break;
    }
    case N_WHILE: {
        Ctl ctl;
        ctl_push(fs, &ctl, CT_LOOP, true);
        uint32_t top = fs->len;
        gen_expr(fs, n->a);
        uint32_t j = emit_jump(fs, OP_JMP_FALSE);
        gen_stmt(fs, n->d);
        emit(fs, OP_LOOP_HINT);
        emit_back_jump(fs, OP_JMP, top);
        uint32_t end = fs->len;
        patch_to(fs, j, end);
        loop_end(fs, &ctl, top, end);
        ctl_pop(fs);
        break;
    }
    case N_DOWHILE: {
        Ctl ctl;
        ctl_push(fs, &ctl, CT_LOOP, true);
        uint32_t top = fs->len;
        gen_stmt(fs, n->d);
        uint32_t cont = fs->len;
        gen_expr(fs, n->a);
        emit(fs, OP_LOOP_HINT);
        emit_back_jump(fs, OP_JMP_TRUE, top);
        loop_end(fs, &ctl, cont, fs->len);
        ctl_pop(fs);
        break;
    }
    case N_FOR: gen_for(fs, n); break;
    case N_FORIN: case N_FOROF: gen_for_in_of(fs, n); break;
    case N_CONTINUE: gen_break(fs, n, true); break;
    case N_BREAK: gen_break(fs, n, false); break;
    case N_RETURN:
        if (n->a) gen_expr(fs, n->a); else emit(fs, OP_PUSH_UNDEF);
        if ((fs->f->flags & F_ASYNC) && (fs->f->flags & F_GEN) && n->a) emit(fs, OP_AWAIT);
        gen_return_value(fs);
        break;
    case N_THROW:
        gen_expr(fs, n->a);
        fs->cur_line = n->line;
        emit(fs, OP_THROW);
        break;
    case N_TRY: gen_try(fs, n); break;
    case N_SWITCH: gen_switch(fs, n); break;
    case N_LABELED: {
        if (fs->npending < 6) fs->pending_labels[fs->npending++] = n->v;
        Node *s = n->a;
        if (s->k == N_FOR || s->k == N_WHILE || s->k == N_DOWHILE || s->k == N_FORIN || s->k == N_FOROF || s->k == N_LABELED) gen_stmt(fs, s);
        else {
            Ctl ctl;
            ctl_push(fs, &ctl, CT_BLOCK, false);
            gen_stmt(fs, s);
            jmps_patch(fs, &ctl.brk, fs->len);
            ctl_pop(fs);
        }
        fs->npending = 0;
        break;
    }
    case N_FIELD_DEF: {
        /* class field initialiser: this.key = value (define semantics) */
        emit(fs, OP_PUSH_THIS);
        if (n->flags & NF_PRIVATE) {
            gen_private_sym(fs, n->res, n->a->v);
            if (n->b) gen_named(fs, n->b, n->a->v); else emit(fs, OP_PUSH_UNDEF);
            emit(fs, OP_DEFINE_PRIVATE);
        } else if (n->flags & NF_COMPUTED) {
            gen_expr(fs, n->a);
            if (n->b) gen_expr(fs, n->b); else emit(fs, OP_PUSH_UNDEF);
            if (n->b && anon_fn(n->b)) emit(fs, OP_SET_NAME_ELEM);
            emit(fs, OP_DEFINE_ELEM);
        } else {
            if (n->b) gen_named(fs, n->b, n->a->v); else emit(fs, OP_PUSH_UNDEF);
            emit32(fs, OP_DEFINE_FIELD, add_const(fs, n->a->v));
        }
        emit(fs, OP_POP);
        break;
    }
    default:
        cerr(fs->c, n->line, "unsupported statement (%d)", n->k);
        break;
    }
}

/* ---- functions ---------------------------------------------------------------------------------------- */
static void gen_prologue(FS *fs) {
    Func *f = fs->f;
    Scope *S = f->scope;
    /* box captured simple parameters (they arrive in their slots) */
    for (Binding *b = S->b; b; b = b->next) {
        if (b->kind != BK_PARAM || !b->captured || b->fn != f) continue;
        bool simple = false;
        int pos = 0;
        for (Node *p = f->params; p; p = p->next, pos++) if (p->k == N_IDENT && p->v == b->name) simple = true;
        if (simple) {
            emit16(fs, OP_GET_LOC, (uint32_t)b->slot);
            emit16(fs, OP_NEW_BOX, (uint32_t)b->slot);
            emit16(fs, OP_INIT_BOX, (uint32_t)b->slot);
        } else emit16(fs, OP_NEW_BOX_UNDEF, (uint32_t)b->slot);
    }
    /* captured vars, TDZ for the top-level lexical bindings */
    for (Binding *b = S->b; b; b = b->next) {
        if (b->global || b->fn != f || b->kind == BK_PARAM || b->kind == BK_HIDDEN) continue;
        if (b->captured) emit16(fs, b->tdz ? OP_NEW_BOX : OP_NEW_BOX_UNDEF, (uint32_t)b->slot);
        else if (b->tdz) { emit(fs, OP_PUSH_UNINIT); emit16(fs, OP_PUT_LOC, (uint32_t)b->slot); }
    }
    /* hidden bindings */
    if (f->this_b) {
        Binding *b = f->this_b;
        if (b->captured) emit16(fs, OP_NEW_BOX, (uint32_t)b->slot);
        if (f->kind == FK_DERIVED) emit(fs, OP_PUSH_UNINIT); else emit(fs, OP_PUSH_THIS);
        emit16(fs, b->captured ? OP_INIT_BOX : OP_PUT_LOC, (uint32_t)b->slot);
    }
    Binding *hb[3] = { f->nt_b, f->func_b, f->args_b };
    for (int i = 0; i < 3; i++) {
        Binding *b = hb[i];
        if (!b) continue;
        if (b->captured) emit16(fs, OP_NEW_BOX, (uint32_t)b->slot);
        emit(fs, i == 0 ? OP_PUSH_NEW_TARGET : i == 1 ? OP_PUSH_FUNC : OP_ARGUMENTS);
        emit16(fs, b->captured ? OP_INIT_BOX : OP_PUT_LOC, (uint32_t)b->slot);
    }
    if (f->self) {
        Binding *b = f->self;
        if (b->captured) emit16(fs, OP_NEW_BOX, (uint32_t)b->slot);
        emit(fs, OP_PUSH_FUNC);
        emit16(fs, b->captured ? OP_INIT_BOX : OP_PUT_LOC, (uint32_t)b->slot);
        if (b->scope->has_captured && !b->captured) { }
    }
    /* parameters with defaults / patterns / rest */
    int pos = 0;
    for (Node *p = f->params; p; p = p->next) {
        fs->cur_line = p->line;
        if (p->k == N_IDENT) { pos++; continue; }
        if (p->k == N_REST) {
            emit16(fs, OP_REST, (uint32_t)pos);
            gen_destructure(fs, p->a, ST_INIT);
            continue;
        }
        emit16(fs, OP_GET_LOC, (uint32_t)pos);
        gen_destructure(fs, p, ST_INIT);
        pos++;
    }
    /* script globals */
    if (f->kind == FK_SCRIPT) {
        for (Binding *b = S->b; b; b = b->next) {
            if (!b->global) continue;
            if (lexical_kind(b->kind)) {
                emit(fs, OP_DEFINE_GLOBAL_LEX);
                put32(fs, add_const(fs, b->name));
                put8(fs, b->kind == BK_CONST ? 1 : 0);
            } else if (b->kind == BK_VAR) emit32(fs, OP_DEFINE_GLOBAL_VAR, add_const(fs, b->name));
        }
        Node *fd[512];
        int n = 0;
        for (Node *x = S->fdecls; x && n < 512; x = x->d) fd[n++] = x;
        for (int i = n - 1; i >= 0; i--) {
            gen_func_closure(fs, fd[i]->fn);
            emit32(fs, OP_DEFINE_GLOBAL_FUNC, add_const(fs, fd[i]->fn->name));
        }
    } else {
        Node *fd[512];
        int n = 0;
        for (Node *x = S->fdecls; x && n < 512; x = x->d) fd[n++] = x;
        for (int i = n - 1; i >= 0; i--) {
            gen_func_closure(fs, fd[i]->fn);
            gen_store(fs, scope_find(S, fd[i]->fn->name), fd[i]->fn->name, ST_INIT);
        }
    }
    /* base class constructor: instance fields first */
    if (f->kind == FK_CTOR && f->fields) {
        emit(fs, OP_PUSH_THIS);
        emit(fs, OP_PUSH_FUNC);
        emit(fs, OP_RUN_FIELDS);
        emit(fs, OP_POP);
    }
    if (f->kind == FK_DERIVED && f->rest_slot == -2) {
        /* default derived constructor: super(...arguments) */
        emit16(fs, OP_REST, 0);
        emit8(fs, OP_SUPER_CALL_SPREAD, 0);
        emit(fs, OP_DUP);
        gen_load_raw(fs, f->this_b);
        emit(fs, OP_CHECK_THIS_INIT);
        emit16(fs, f->this_b->captured ? OP_INIT_BOX : OP_PUT_LOC, (uint32_t)f->this_b->slot);
        emit(fs, OP_PUSH_FUNC);
        emit(fs, OP_RUN_FIELDS);
        emit(fs, OP_POP);
    }
    if ((f->flags & F_GEN)) emit(fs, OP_INITIAL_YIELD);
}

static JSBytecode *finish(FS *fs) {
    Comp *c = fs->c;
    JSContext *ctx = c->ctx;
    Func *f = fs->f;
    JSBytecode *b = js_cell(ctx, T_BYTECODE, sizeof(JSBytecode));
    if (!b) { cerr(c, f->line, "out of memory"); return NULL; }
    b->code = malloc(fs->len ? fs->len : 1);
    b->cp = fs->ncp ? malloc(sizeof(JSValue) * fs->ncp) : NULL;
    b->cv = f->ncv ? malloc(sizeof(uint16_t) * (size_t)f->ncv) : NULL;
    b->lines = fs->nlines ? malloc(sizeof(uint32_t) * fs->nlines) : NULL;
    b->lnames = f->nlocals ? calloc((size_t)f->nlocals, sizeof(JSValue)) : NULL;
    if (!b->code || (fs->ncp && !b->cp) || (f->ncv && !b->cv) || (fs->nlines && !b->lines) || (f->nlocals && !b->lnames)) {
        cerr(c, f->line, "out of memory");
        return NULL;
    }
    memcpy(b->code, fs->code, fs->len);
    b->len = fs->len;
    if (fs->ncp) memcpy(b->cp, fs->cp, sizeof(JSValue) * fs->ncp);
    b->ncp = fs->ncp;
    for (int i = 0; i < f->ncv; i++) b->cv[i] = f->cv[i].src;
    b->ncv = (uint16_t)f->ncv;
    if (fs->nlines) memcpy(b->lines, fs->lines, sizeof(uint32_t) * fs->nlines);
    b->nlines = fs->nlines;
    int nformals = 0;
    for (Node *p = f->params; p; p = p->next) if (p->k != N_REST) nformals++;
    b->nparams = (uint16_t)nformals;
    b->length = (uint16_t)f->length;
    b->nlocals = (uint16_t)f->nlocals;
    b->stack_max = (uint16_t)(fs->max_sp + 4);
    b->fflags = f->flags;
    if (f->kind == FK_ARROW) b->fflags |= F_ARROW;
    if (f->kind == FK_METHOD || f->kind == FK_GETTER || f->kind == FK_SETTER) b->fflags |= F_METHOD;
    if (f->kind == FK_GETTER || f->kind == FK_SETTER) b->fflags |= F_ACCESSOR;
    if (f->kind == FK_FIELDS || f->kind == FK_STATIC_INIT) b->fflags |= F_FIELDINIT | F_METHOD;
    if (f->uses_args || f->args_b) b->fflags |= F_ARGS;
    b->name = JS_IsUndefined(f->name) ? JS_UNDEFINED : f->name;
    b->srcname = c->srcname;
    b->source = c->src;
    b->src_start = f->start;
    b->src_end = f->end;
    b->this_slot = (int16_t)(f->this_b ? f->this_b->slot : -1);
    b->nt_slot = b->func_slot = b->args_slot = -1;
    b->rest_param = -1;
    for (Scope *s = f->scope; s; s = s->parent) {
        if (s->fn != f) break;
        for (Binding *x = s->b; x; x = x->next) if (x->slot >= 0 && x->slot < f->nlocals) b->lnames[x->slot] = x->name;
    }
    return b;
}

/* the lexical names of nested block scopes are filled as they are met (TDZ messages) */
static void name_scopes(Func *f, JSBytecode *b, Node *n);

static JSBytecode *compile_func(Comp *c, Func *f, FS *parent) {
    FS fs;
    memset(&fs, 0, sizeof fs);
    fs.c = c;
    fs.f = f;
    fs.parent = parent;
    fs.ret_slot = -1;
    fs.completion = -1;
    fs.cur_line = f->line;
    if (f->kind == FK_SCRIPT && (f->flags & F_EVAL)) {
        fs.completion = f->nlocals++;
    }
    gen_prologue(&fs);
    if (fs.completion >= 0) { emit(&fs, OP_PUSH_UNDEF); emit16(&fs, OP_PUT_LOC, (uint32_t)fs.completion); }
    gen_block_body(&fs, f->body);
    /* implicit return */
    if (fs.completion >= 0) emit16(&fs, OP_GET_LOC, (uint32_t)fs.completion);
    else emit(&fs, OP_PUSH_UNDEF);
    if (f->kind == FK_DERIVED) { gen_load_raw(&fs, f->this_b); emit(&fs, OP_CHECK_DERIVED_RETURN); }
    emit(&fs, OP_RETURN);
    JSBytecode *b = c->failed ? NULL : finish(&fs);
    if (b) name_scopes(f, b, f->body);
    free(fs.code);
    free(fs.cp);
    free(fs.cph);
    free(fs.lines);
    return b;
}

static void name_scope(Func *f, JSBytecode *b, Scope *s) {
    if (!s) return;
    for (Binding *x = s->b; x; x = x->next)
        if (x->fn == f && x->slot >= 0 && x->slot < b->nlocals) b->lnames[x->slot] = x->name;
}

static void name_scopes(Func *f, JSBytecode *b, Node *n) {
    for (; n; n = n->next) {
        switch (n->k) {
        case N_BLOCK: name_scope(f, b, n->res); name_scopes(f, b, n->a); break;
        case N_FOR: case N_FORIN: case N_FOROF: name_scope(f, b, n->res); name_scopes(f, b, n->d); break;
        case N_SWITCH: name_scope(f, b, n->res); for (Node *cs = n->b; cs; cs = cs->next) name_scopes(f, b, cs->b); break;
        case N_TRY: name_scopes(f, b, n->a); name_scope(f, b, n->res); name_scopes(f, b, n->c); name_scopes(f, b, n->d); break;
        case N_IF: name_scopes(f, b, n->b); name_scopes(f, b, n->c); break;
        case N_WHILE: case N_DOWHILE: case N_LABELED: name_scopes(f, b, n->k == N_LABELED ? n->a : n->d); break;
        default: break;
        }
        if (n->k == N_IF || n->k == N_LABELED || n->k == N_WHILE || n->k == N_DOWHILE) { /* single statements handled above */ }
    }
}

/* ==== entry points ======================================================================================== */
static void comp_init(Comp *c, JSContext *ctx) {
    memset(c, 0, sizeof *c);
    c->ctx = ctx;
    c->a_args = ctx->a[A_arguments];
    c->a_this = js_atomc(ctx, "<this>");
    c->a_nt = js_atomc(ctx, "<new.target>");
    c->a_func = js_atomc(ctx, "<function>");
    c->a_undefined = ctx->a[A_undefined];
    c->a_eval = ctx->a[A_eval];
}

JSBytecode *js_compile(JSContext *ctx, JSValue src, JSValue name, int line, uint32_t fflags) {
    JSString *s = js_flat(ctx, src);
    if (!s) return NULL;
    ctx->no_gc++;
    Parser *p = NULL;
    Func *f = js_parse_program(ctx, &p, s->s, s->len, line, fflags);
    JSBytecode *b = NULL;
    if (f) {
        Comp c;
        comp_init(&c, ctx);
        c.p = p;
        c.src = JS_MKSTR(s);
        c.srcname = name;
        res_func(&c, f, NULL);
        if (!c.failed) b = compile_func(&c, f, NULL);
        if (c.failed) {
            js_throw_syntax(ctx, "%s (line %d)", c.err, c.err_line);
            b = NULL;
        }
    }
    js_parser_free(p);
    ctx->no_gc--;
    return b;
}

void js_bytecode_free(JSContext *ctx, JSBytecode *b) {
    (void)ctx;
    free(b->code);
    free(b->cp);
    free(b->cv);
    free(b->lines);
    free(b->lnames);
    b->code = NULL;
    b->cp = NULL;
    b->cv = NULL;
    b->lines = NULL;
    b->lnames = NULL;
}

void js_bytecode_mark(JSContext *ctx, JSBytecode *b) {
    for (uint32_t i = 0; i < b->ncp; i++) js_gc_mark_value(ctx, b->cp[i]);
    js_gc_mark_value(ctx, b->name);
    js_gc_mark_value(ctx, b->srcname);
    js_gc_mark_value(ctx, b->source);
    if (b->lnames) for (uint32_t i = 0; i < b->nlocals; i++) js_gc_mark_value(ctx, b->lnames[i]);
}

int js_line_of(JSBytecode *b, uint32_t pc) {
    int line = 0;
    for (uint32_t i = 0; i + 1 < b->nlines; i += 2) {
        if (b->lines[i] > pc) break;
        line = (int)b->lines[i + 1];
    }
    return line;
}
