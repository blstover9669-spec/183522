/*
 * watchdog.h - the app watchdog (kernel/core/watchdog.c) and the preempt-disable
 * counter that marks the kernel's critical sections.
 *
 * NovaOS runs every app callback on the one GUI main loop. The watchdog notices an
 * app callback that has not returned for about 5 seconds, suspends it (its registers,
 * stack and FPU state are kept) and lets the GUI main loop go on from a fresh part of
 * the kernel stack, with a "... is not responding" window (Close the app / Wait).
 *
 * Critical sections: code between preempt_disable() and preempt_enable() is never
 * suspended, switched away from or abandoned (the heap, the VFS, disk I/O, the
 * network stack, window-manager / compositor internals). A switch the timer wanted
 * meanwhile happens as soon as the counter drops back to zero. They nest, they do not
 * disable interrupts, and they must never contain an app callback.
 */
#ifndef NOVA_WATCHDOG_H
#define NOVA_WATCHDOG_H

#include "kernel.h"

extern volatile int wd_preempt;          /* > 0: inside a critical section            */
extern volatile int wd_pending;          /* the timer wants a switch                  */
extern uint32_t wd_cs_start;             /* g_ticks when the outermost section began  */
extern void *wd_cs_ra;                   /* where that section's function returns to  */
void wd_preempt_slow(void);              /* preempt_enable(): pending switch / long work */

#define WD_LONG_MS 200                   /* a section this long is "kernel work"      */

static inline __attribute__((always_inline)) void preempt_disable(void) {
    if (wd_preempt++ == 0) {
        wd_cs_start = g_ticks;
        wd_cs_ra = __builtin_return_address(0);
    }
    __asm__ volatile("" : : : "memory");
}

static inline __attribute__((always_inline)) void preempt_enable(void) {
    __asm__ volatile("" : : : "memory");
    if (--wd_preempt == 0 && (wd_pending || g_ticks - wd_cs_start >= WD_LONG_MS)) wd_preempt_slow();
}

static inline void wd_cs_end_(int *unused) { (void)unused; preempt_enable(); }
/* CRITICAL_SECTION(); at the top of a block: preempt_disable() now, preempt_enable()
   when the block is left (every return included). */
#define CRITICAL_SECTION() \
    preempt_disable(); int _wd_cs __attribute__((cleanup(wd_cs_end_), unused)) = 0

/* ---- app callbacks (the dispatchers: wm.c, timers.c, apps.c, the HTTP callbacks) ---- */
/* True if app's callbacks must not run now: the app (or an app sharing its code) is
   stuck in a suspended callback and this is not that callback's own context. Queue or
   drop the event instead. */
bool wd_blocked(const app_desc_t *app);
/* true while the app is stuck (its windows show "(Not responding)") */
bool wd_app_stuck(const app_desc_t *app);
/* Around every call from the main loop into an app's code; app != NULL. */
void wd_cb_begin(const app_desc_t *app);
void wd_cb_end(void);

/* ---- hooks of the main loop (kernel/gui/wm.c) ----------------------------------------- */
void wd_loop_top(void);                  /* top of every main-loop iteration           */
bool wd_run_waiting(void);               /* idle: give a waiting app a slice (true = ran) */
void wd_show_dialog(const app_desc_t *app);   /* bring its "not responding" window up */

/* ---- the timer interrupt (kernel/arch/cpu.c) ------------------------------------------ */
void    wd_tick(void);                   /* PIT IRQ, every millisecond                 */
regs_t *wd_switch_point(regs_t *r);      /* end of the PIT IRQ and of the yield vector */
#define WD_YIELD_VECTOR 0x81

/* code address inside the apps' code (linked after the kernel, kernel/zz_app_text.c) */
bool wd_in_app_code(uintptr_t addr);

extern const app_desc_t *wm_current_app;   /* kernel/gui/wm.c: the app whose code runs */

/* wm.c, for the watchdog */
void wm_app_windows_changed(const app_desc_t *app);    /* titles / contents repaint   */
void wm_close_app_windows(const app_desc_t *app);      /* no callbacks: Close the app */
void wm_requeue_posted(void);                          /* deliver held posted events  */
void timers_remove_app(const app_desc_t *app);         /* timers.c                    */
void http_forget_app(const app_desc_t *app);           /* kernel/net/http.c           */

#endif
