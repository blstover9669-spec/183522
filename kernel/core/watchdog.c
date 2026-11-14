/*
 * watchdog.c - the app watchdog: an app callback that does not return is suspended,
 * and NovaOS goes on.
 *
 * NovaOS is single-threaded: the GUI main loop (kernel/gui/wm.c: wm_run) calls the
 * apps' callbacks one after the other. The dispatchers (window events, painting,
 * timers, launching, the network's callbacks) bracket each call with wd_cb_begin()
 * and wd_cb_end(), so the watchdog knows which app's code runs and since when. The
 * timer interrupt (wd_tick, 1000 Hz) checks it: when one app callback has run for
 * WD_HANG_MS without returning, the interrupt suspends it - its register frame stays
 * on its stack, its FPU state is saved - and starts a new context that runs the
 * normal GUI main loop on a fresh part of the kernel stack (below the stuck frames,
 * WD_GAP lower). That loop shows "<App> is not responding" (Close the app / Wait).
 *
 *   Close the app  the stuck context is never resumed; the app's windows, timers and
 *                  pending network callbacks go away without calling the app again.
 *                  Its other memory is leaked (there is no record of what it owns).
 *   Wait           the stuck callback runs again in WD_SLICE_MS time slices that
 *                  alternate with the GUI loop. When it returns, its context ends (it
 *                  never returns into the dispatcher that called it: the GUI loop has
 *                  moved on), the dialog closes and the app works again. Still stuck
 *                  WD_REASK_MS later: the question again.
 *
 * Safety rules:
 *   - Nothing is ever suspended inside a critical section (preempt_disable() ..
 *     preempt_enable(): heap, VFS, disk, network, window manager): the switch waits
 *     until the counter is back to zero, then happens at once.
 *   - A switch also waits for a point where the app's own code runs (its instruction
 *     pointer is in the apps' code, or the critical section that just ended returns
 *     into app code), so kernel code between two critical sections is not cut either;
 *     after WD_FORCE_MS any point outside a critical section is taken.
 *   - While an app is stuck the GUI loop never calls it: its windows get no events
 *     (posted events are kept for later), are not painted, resized or destroyed, its
 *     timers and network callbacks wait. Apps sharing code with it (the Web Browser
 *     and the HTML Viewer share the web engine) are held the same way.
 *   - Kernel code outside app callbacks is not watched, and time spent inside one long
 *     critical section (>= WD_LONG_MS: a big copy, disk or network work) is not
 *     counted as the app's.
 * The GUI loop switches to a waiting app only at the top of its loop, and the
 * interrupt handlers themselves are unchanged (the switch happens after the EOI).
 * When no app is stuck any more, the GUI loop starts again at the top of the kernel
 * stack. Every stack is part of the 1 MiB boot stack, so the JavaScript engine's
 * conservative stack scan (which reads up to kernel_stack_top) stays correct.
 */
#include "kernel.h"
#include "core/watchdog.h"

#define WD_HANG_MS     5000
#define WD_SLICE_MS    20
#define WD_REASK_MS    10000
#define WD_FORCE_MS    500
#define WD_GAP         (256u * 1024)     /* room the stuck frames keep below them    */
#define WD_MIN_STACK   (256u * 1024)     /* smallest stack the GUI loop gets          */
#define WD_GUARD_WORDS 256               /* collision guard at the bottom of the gap  */
#define WD_GUARD_MAGIC 0x57A11ED5u
#define WD_MAX         4                 /* contexts: one GUI loop + stuck callbacks  */
#define WD_NEST        8

enum { C_FREE, C_GUI, C_FROZEN, C_WAITING, C_DONE };
enum { REQ_NONE, REQ_SUSPEND, REQ_SLICE_END };
enum { Y_PENDING = 1, Y_TO = 2 };        /* yield vector: EDX = kind, EAX = target    */

typedef struct wd_ctx {
    int state;
    regs_t *frame;                       /* saved register frame (not running)        */
    uint8_t fpu[108];                    /* fnsave area                               */
    bool fpu_valid;
    const app_desc_t *cur_app;           /* wm_current_app while switched out         */
    uintptr_t base;                      /* top of its stack                          */
    /* app callbacks running in it */
    int depth;
    const app_desc_t *apps[WD_NEST];     /* outermost first                           */
    uint32_t cb_start, exempt, serial;
    /* stuck */
    const app_desc_t *blame;             /* the app named in the dialog (innermost)    */
    const app_desc_t *stuck[WD_NEST];    /* every app with frames on its stack        */
    int nstuck;
    uint32_t since, wait_since, slice_start;
    uintptr_t guard;                     /* collision guard words (below its frames)  */
    int dlg;                             /* dialog window id, 0 = none                */
    widget_t *dlg_text, *dlg_status, *dlg_wait;
    bool want_close, shown;
} wd_ctx_t;

volatile int wd_preempt;
volatile int wd_pending;
uint32_t wd_cs_start;
void *wd_cs_ra;

static wd_ctx_t ctxs[WD_MAX];
static wd_ctx_t *cur = &ctxs[0], *gui = &ctxs[0];
static bool ready;
static volatile int req;                 /* REQ_*, set by the timer interrupt          */
static uint32_t req_at, req_serial;
static uint32_t gui_since;               /* when the GUI loop got the CPU back         */
static uint32_t gave_up_serial;          /* no rescue possible for this callback       */
static volatile bool gave_up_log;        /* ... to be logged by the GUI loop           */
static int nstuck;                       /* contexts FROZEN or WAITING                 */
static uint32_t hangs;

extern const app_desc_t *wm_current_app;            /* kernel/gui/wm.c          */
extern uint32_t kernel_stack_bottom[];              /* boot/boot.asm            */
extern char kernel_stack_top[];
extern void kernel_text_end(void);                  /* kernel/zz_app_text.c     */
extern const app_desc_t *const __apps_start[];      /* linker.ld                */

#define STACK_TOP    ((uintptr_t)kernel_stack_top)
#define STACK_FLOOR  ((uintptr_t)kernel_stack_bottom + 4096)   /* above stack_guard words */

bool wd_in_app_code(uintptr_t a) {
    return a >= (uintptr_t)kernel_text_end && a < (uintptr_t)__apps_start;
}

/* ---- apps that share code are held together ----------------------------------------- */
static const char *const families[][2] = { { "Web Browser", "HTML Viewer" } };

static bool related(const app_desc_t *a, const app_desc_t *b) {
    if (a == b) return true;
    if (!a || !b || !a->name || !b->name) return false;
    for (int i = 0; i < ARRAY_SIZE(families); i++) {
        bool ia = !strcmp(a->name, families[i][0]) || !strcmp(a->name, families[i][1]);
        bool ib = !strcmp(b->name, families[i][0]) || !strcmp(b->name, families[i][1]);
        if (ia && ib) return true;
    }
    return false;
}

static bool is_stuck(const wd_ctx_t *c) { return c->state == C_FROZEN || c->state == C_WAITING; }

static bool holds(const wd_ctx_t *c, const app_desc_t *app) {
    for (int i = 0; i < c->nstuck; i++) if (related(c->stuck[i], app)) return true;
    return false;
}

bool wd_app_stuck(const app_desc_t *app) {
    if (!app || !nstuck) return false;
    for (int i = 0; i < WD_MAX; i++) if (is_stuck(&ctxs[i]) && holds(&ctxs[i], app)) return true;
    return false;
}

bool wd_blocked(const app_desc_t *app) {
    if (!app || !nstuck) return false;
    for (int i = 0; i < WD_MAX; i++) {
        wd_ctx_t *c = &ctxs[i];
        if (c != cur && is_stuck(c) && holds(c, app)) return true;
    }
    return false;
}

/* ---- app callbacks -------------------------------------------------------------------- */
static void yield(int kind, wd_ctx_t *to) {
    __asm__ volatile("int %0" : : "i"(WD_YIELD_VECTOR), "a"(to), "d"(kind) : "memory");
}

void wd_cb_begin(const app_desc_t *app) {
    wd_ctx_t *c = cur;
    if (c->depth < WD_NEST) c->apps[c->depth] = app;
    if (c->depth++ == 0) {
        c->exempt = 0;
        c->cb_start = g_ticks;
        c->serial++;
    }
}

void wd_cb_end(void) {
    wd_ctx_t *c = cur;
    if (c->depth > 0) c->depth--;
    if (c->depth == 0 && c->state == C_WAITING) {
        /* the stuck callback returned: this context ends here, the GUI loop (which
           has long moved on from the dispatcher below us) carries on */
        if (wd_preempt) kpanic("watchdog: callback returned inside a critical section");
        c->state = C_DONE;
        for (;;) yield(Y_TO, gui);
    }
}

/* preempt_enable() reached zero with a switch pending, or after long kernel work */
void wd_preempt_slow(void) {
    uint32_t d = g_ticks - wd_cs_start;
    if (d >= WD_LONG_MS && cur->depth > 0) cur->exempt += d;   /* kernel work: not the app's */
    uint32_t fl;
    __asm__ volatile("pushf; pop %0" : "=r"(fl));
    if (wd_pending && (fl & 0x200)) {            /* never from an interrupt handler */
        wd_pending = 0;
        if (req) yield(Y_PENDING, NULL);
    }
}

/* ---- the timer interrupt ---------------------------------------------------------- */
void wd_tick(void) {
    if (!ready || req) return;
    wd_ctx_t *c = cur;
    uint32_t now = g_ticks;
    if (c->state == C_GUI) {
        if (c->depth <= 0 || gave_up_serial == c->serial) return;
        uint32_t el = now - c->cb_start - c->exempt;
        if (wd_preempt > 0) {                      /* the running section counts once it ends */
            uint32_t cs = now - wd_cs_start;
            el = el > cs ? el - cs : 0;
        }
        if (el < WD_HANG_MS) return;
        req = REQ_SUSPEND;
        req_serial = c->serial;
        req_at = now;
    } else if (c->state == C_WAITING) {
        if (now - c->slice_start < WD_SLICE_MS) return;
        req = REQ_SLICE_END;
        req_serial = c->serial;
        req_at = now;
    }
}

static void check_guards(void) {
    for (int i = 0; i < WD_MAX; i++) {
        wd_ctx_t *c = &ctxs[i];
        if (!is_stuck(c) || !c->guard) continue;
        const uint32_t *g = (const uint32_t *)c->guard;
        if (g[0] != WD_GUARD_MAGIC || g[WD_GUARD_WORDS / 2] != WD_GUARD_MAGIC || g[WD_GUARD_WORDS - 1] != WD_GUARD_MAGIC)
            kpanic("watchdog: the stuck app \"%s\" used more than %u KiB of stack more and ran into "
                   "the stack of the GUI loop", c->blame ? c->blame->name : "?", WD_GAP / 1024);
    }
}

#define FPU_CW_DEFAULT 0x027F

static regs_t *switch_to(regs_t *r, wd_ctx_t *t) {
    wd_ctx_t *c = cur;
    check_guards();
    c->frame = r;
    c->cur_app = wm_current_app;
    if (c->state != C_DONE && c->state != C_FREE) {
        __asm__ volatile("fnsave %0" : "=m"(c->fpu));
        c->fpu_valid = true;
    }
    if (t->fpu_valid) {
        __asm__ volatile("frstor %0" : : "m"(t->fpu));
    } else {
        uint16_t cw = FPU_CW_DEFAULT;
        __asm__ volatile("fninit\n\tfldcw %0" : : "m"(cw));
    }
    wm_current_app = t->cur_app;
    cur = t;
    req = REQ_NONE;
    wd_pending = 0;
    if (t->state == C_WAITING) t->slice_start = g_ticks;
    if (t == gui) gui_since = g_ticks;
    return t->frame;
}

static void NORETURN gui_main(void) {
    wm_run();
}

/* a register frame on a fresh stack that "returns" into gui_main */
static regs_t *fabricate(uintptr_t top) {
    uint32_t *ret = (uint32_t *)((top & ~(uintptr_t)15) - 4);   /* entry: esp+4 16-aligned */
    *ret = 0;                                                    /* ends stack traces       */
    regs_t *f = (regs_t *)((uint8_t *)ret - offsetof(regs_t, useresp));
    memset(f, 0, offsetof(regs_t, useresp));
    f->gs = f->fs = f->es = f->ds = 0x10;
    f->eip = (uint32_t)gui_main;
    f->cs = 0x08;
    f->eflags = 0x202;
    return f;
}

static wd_ctx_t *new_ctx(uintptr_t top) {
    for (int i = 0; i < WD_MAX; i++) {
        wd_ctx_t *c = &ctxs[i];
        if (c->state != C_FREE || c == cur) continue;
        memset(c, 0, sizeof *c);
        c->state = C_GUI;
        c->base = top;
        c->frame = fabricate(top);
        return c;
    }
    return NULL;
}

/* The GUI loop's current callback has hung: suspend it, start a new GUI loop below it. */
static regs_t *suspend(regs_t *r) {
    wd_ctx_t *s = cur;
    uintptr_t top = ((uintptr_t)r - WD_GAP) & ~(uintptr_t)15;
    wd_ctx_t *n = top > STACK_FLOOR + WD_MIN_STACK ? new_ctx(top) : NULL;
    if (!n) {
        gave_up_serial = s->serial;
        gave_up_log = true;
        req = REQ_NONE;
        return r;
    }
    s->state = C_FROZEN;
    s->since = g_ticks;
    s->nstuck = MIN(s->depth, WD_NEST);
    for (int i = 0; i < s->nstuck; i++) s->stuck[i] = s->apps[i];
    s->blame = s->stuck[s->nstuck - 1];
    s->guard = top;
    for (int i = 0; i < WD_GUARD_WORDS; i++) ((uint32_t *)top)[i] = WD_GUARD_MAGIC;
    s->dlg = 0;
    s->shown = false;
    s->want_close = false;
    nstuck++;
    hangs++;
    gui = n;
    return switch_to(r, n);
}

static bool safe_point(regs_t *r) {
    uintptr_t at = r->int_no == WD_YIELD_VECTOR ? (uintptr_t)wd_cs_ra : r->eip;
    if (!wd_in_app_code(at)) return false;
    if (cur->state != C_WAITING) return true;
    /* a waiting context only stops in its own app's code (not inside another app's
       callback it called, which the GUI loop could enter too) */
    int d = MIN(cur->depth, WD_NEST);
    return d > 0 && holds(cur, cur->apps[d - 1]);
}

regs_t *wd_switch_point(regs_t *r) {
    if (r->int_no == WD_YIELD_VECTOR && r->edx == Y_TO) {
        wd_ctx_t *t = (wd_ctx_t *)r->eax;
        if (!t || t == cur || t->state == C_FREE || t->state == C_FROZEN || t->state == C_DONE) return r;
        return switch_to(r, t);
    }
    if (req == REQ_NONE) return r;
    if (wd_preempt > 0 || !(r->eflags & 0x200)) { wd_pending = 1; return r; }
    if (req == REQ_SUSPEND) {
        if (cur->state != C_GUI || cur->depth <= 0 || cur->serial != req_serial) { req = REQ_NONE; return r; }
        if (!safe_point(r) && g_ticks - req_at < WD_FORCE_MS) return r;
        return suspend(r);
    }
    if (req == REQ_SLICE_END) {
        if (cur->state != C_WAITING) { req = REQ_NONE; return r; }
        if (!safe_point(r) && g_ticks - req_at < WD_FORCE_MS) return r;
        return switch_to(r, gui);
    }
    req = REQ_NONE;
    return r;
}

/* ---- the "not responding" window ------------------------------------------------------- */
static const char *app_name(const wd_ctx_t *c) {
    return c->blame && c->blame->name ? c->blame->name : "An app";
}

static wd_ctx_t *ctx_of(intptr_t i) {
    return i >= 0 && i < WD_MAX && is_stuck(&ctxs[i]) ? &ctxs[i] : NULL;
}

static void dlg_close(wd_ctx_t *c);

static void dlg_update(wd_ctx_t *c) {
    if (!c->dlg_status) return;
    bool waiting = c->state == C_WAITING;
    char buf[160];
    if (waiting) snprintf(buf, sizeof buf, "Waiting for %s to respond...", app_name(c));
    else snprintf(buf, sizeof buf, "You can wait for it, or close it: what is not saved in it is lost.");
    ui_set_text(c->dlg_status, buf);
    if (c->dlg_wait) ui_set_enabled(c->dlg_wait, !waiting);
}

static void on_wait(widget_t *w, void *ctx) {
    (void)w;
    wd_ctx_t *c = ctx_of((intptr_t)ctx);
    if (!c || c->state != C_FROZEN) return;
    c->state = C_WAITING;
    c->wait_since = g_ticks;
    klog("watchdog: Wait - %s gets time slices of %u ms", app_name(c), WD_SLICE_MS);
    dlg_update(c);
}

static void on_close_app(widget_t *w, void *ctx) {
    (void)w;
    wd_ctx_t *c = ctx_of((intptr_t)ctx);
    if (c) c->want_close = true;
}

static void dlg_handler(window_t *win, const event_t *ev) {
    intptr_t i = (intptr_t)win_get_userdata(win);
    wd_ctx_t *c = i >= 0 && i < WD_MAX ? &ctxs[i] : NULL;
    switch (ev->type) {
    case EV_PAINT:
        ui_draw_glyph(win_surface(win), UI_GLYPH_WARNING, 22, 22, 32, theme_get()->accent);
        break;
    case EV_CLOSE_REQUEST:                       /* X or Esc: Wait, and the question goes away */
        on_wait(NULL, (void *)i);
        if (c) dlg_close(c);
        break;
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESCAPE) { on_wait(NULL, (void *)i); if (c) dlg_close(c); }
        break;
    case EV_CLOSE:
        if (c && c->dlg == win_id(win)) { c->dlg = 0; c->dlg_text = c->dlg_status = c->dlg_wait = NULL; }
        break;
    default:
        break;
    }
}

static void dlg_open(wd_ctx_t *c) {
    window_t *w = c->dlg ? win_find_by_id(c->dlg) : NULL;
    if (w) { win_focus(w); dlg_update(c); return; }
    const app_desc_t *saved = wm_current_app;
    wm_current_app = NULL;                       /* a NovaOS window, never held */
    char title[160], text[160];
    snprintf(title, sizeof title, "%s is not responding", app_name(c));
    snprintf(text, sizeof text, "%s is not responding.", app_name(c));
    const int cw = 440, ch = 150;
    w = win_create(title, cw, ch, WIN_CENTER | WIN_CONFIRM_CLOSE | WIN_DIALOG);
    wm_current_app = saved;
    if (!w) return;
    intptr_t idx = c - ctxs;
    win_set_userdata(w, (void *)idx);
    win_set_event_handler(w, dlg_handler);
    c->dlg = win_id(w);
    c->dlg_text = ui_label(w, 70, 22, cw - 90, 22, text);
    if (c->dlg_text) ui_set_font(c->dlg_text, FONT_UI_BOLD);
    c->dlg_status = ui_label(w, 70, 48, cw - 90, 40, "");
    ui_button(w, cw - 270, ch - 46, 140, 30, "Close the app", on_close_app, (void *)idx);
    c->dlg_wait = ui_button(w, cw - 120, ch - 46, 100, 30, "Wait", on_wait, (void *)idx);
    dlg_update(c);
    win_focus(w);
}

static void dlg_close(wd_ctx_t *c) {
    window_t *w = c->dlg ? win_find_by_id(c->dlg) : NULL;
    c->dlg = 0;
    c->dlg_text = c->dlg_status = c->dlg_wait = NULL;
    if (w) win_close(w);
}

static void ask(wd_ctx_t *c) {
    c->state = C_FROZEN;
    c->shown = true;
    dlg_open(c);
    sys_alert_sound();
}

void wd_show_dialog(const app_desc_t *app) {
    for (int i = 0; i < WD_MAX; i++)
        if (is_stuck(&ctxs[i]) && holds(&ctxs[i], app) && ctxs[i].shown) { dlg_open(&ctxs[i]); return; }
}

/* ---- the GUI loop's side ---------------------------------------------------------------- */
static void unstick(wd_ctx_t *c) {
    nstuck--;
    dlg_close(c);
    wm_requeue_posted();
    wm_app_windows_changed(NULL);
}

static void close_app(wd_ctx_t *c) {
    const app_desc_t *apps[WD_NEST];
    int n = c->nstuck;
    memcpy(apps, c->stuck, sizeof apps);
    uint32_t secs = (g_ticks - c->since) / 1000;
    klog("watchdog: Close the app - %s closed (stuck for %u s); its suspended callback is "
         "dropped and the memory it still holds is not freed", app_name(c), secs + WD_HANG_MS / 1000);
    c->state = C_FREE;
    unstick(c);
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int k = 0; k < i; k++) if (apps[k] == apps[i]) dup = true;
        if (dup || !apps[i]) continue;
        wm_close_app_windows(apps[i]);
        timers_remove_app(apps[i]);
        http_forget_app(apps[i]);
    }
    klog("watchdog: heap check after closing it: %s", heap_check() ? "ok" : "DAMAGED");
}

void wd_loop_top(void) {
    if (!ready) {                                /* the first main loop, on the boot stack */
        gui->state = C_GUI;
        gui->base = STACK_TOP;
        gui_since = g_ticks;
        ready = true;
    }
    if (gave_up_log) {
        gave_up_log = false;
        klog("watchdog: an app callback did not return for %u s, but there is no room for another "
             "GUI loop (%d apps are stuck already)", WD_HANG_MS / 1000, nstuck);
    }
    if (!nstuck && gui->base == STACK_TOP) return;
    for (int i = 0; i < WD_MAX; i++) {
        wd_ctx_t *c = &ctxs[i];
        if (c->state == C_FROZEN && !c->shown) {
            klog("watchdog: %s is not responding (a callback has run for %u s) - suspended, "
                 "the desktop goes on (hang #%u)", app_name(c), (g_ticks - c->cb_start) / 1000, hangs);
            wm_app_windows_changed(NULL);
            ask(c);
        } else if (c->state == C_DONE) {
            klog("watchdog: %s responds again (its callback returned after %u s)", app_name(c),
                 (g_ticks - c->cb_start) / 1000);
            c->state = C_FREE;
            unstick(c);
        } else if (is_stuck(c) && c->want_close) {
            close_app(c);
        } else if (c->state == C_WAITING && g_ticks - c->wait_since >= WD_REASK_MS) {
            klog("watchdog: %s is still not responding", app_name(c));
            ask(c);
        }
    }
    /* time slices for waiting apps (round robin) */
    if (g_ticks - gui_since >= WD_SLICE_MS && wd_run_waiting()) return;
    /* nothing stuck any more: the GUI loop starts again at the top of the stack */
    if (!nstuck && gui->base != STACK_TOP) {
        wd_ctx_t *n = new_ctx(STACK_TOP);
        if (!n) return;
        wd_ctx_t *old = gui;
        klog("watchdog: back to one main loop");
        gui = n;
        old->state = C_FREE;
        yield(Y_TO, n);                          /* never returns */
    }
}

bool wd_run_waiting(void) {
    static int rr;
    if (!nstuck) return false;
    for (int k = 0; k < WD_MAX; k++) {
        wd_ctx_t *c = &ctxs[(rr + k) % WD_MAX];
        if (c->state != C_WAITING || c->want_close) continue;
        rr = (int)(c - ctxs) + 1;
        yield(Y_TO, c);                          /* back here when its slice is over */
        return true;
    }
    return false;
}
