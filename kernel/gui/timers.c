/* timers.c - window timers (EV_TIMER) and global callback timers, run from the main loop.
   A global timer whose function is app code belongs to the app that added it: it runs
   as that app (the app watchdog watches it), waits while the app is stuck and goes
   away when the watchdog closes the app. The timer list is a watchdog critical section. */
#include "wm_internal.h"
#include "core/watchdog.h"

typedef struct timer {
    int id;
    window_t *win;        /* NULL for global timers */
    timer_fn fn;
    void *ctx;
    uint32_t interval;
    uint32_t due;
    bool repeat;
    bool dead;
    const app_desc_t *app;   /* global timers: the app whose code fn is (NULL: NovaOS) */
    struct timer *next;
} ktimer_t;

static ktimer_t *timers;
static int next_timer_id = 1;

void timers_init(void) { timers = NULL; }

static ktimer_t *timer_new(uint32_t ms) {
    CRITICAL_SECTION();
    ktimer_t *t = kcalloc(1, sizeof *t);
    if (!t) return NULL;
    t->id = next_timer_id++;
    if (next_timer_id <= 0) next_timer_id = 1;
    t->interval = ms ? ms : 1;
    t->due = g_ticks + t->interval;
    t->next = timers;
    timers = t;
    return t;
}

int win_set_timer(window_t *win, uint32_t interval_ms) {
    if (!win || win->closing) return 0;
    ktimer_t *t = timer_new(interval_ms);
    if (!t) return 0;
    t->win = win;
    t->repeat = true;
    return t->id;
}

void win_kill_timer(window_t *win, int id) {
    CRITICAL_SECTION();
    for (ktimer_t *t = timers; t; t = t->next)
        if (t->win == win && (id <= 0 || t->id == id)) t->dead = true;
}

int timer_add(uint32_t ms, timer_fn fn, void *ctx, bool repeat) {
    if (!fn) return 0;
    ktimer_t *t = timer_new(ms);
    if (!t) return 0;
    t->fn = fn;
    t->ctx = ctx;
    t->repeat = repeat;
    t->app = wd_in_app_code((uintptr_t)fn) ? wm_current_app : NULL;
    return t->id;
}

void timer_cancel(int id) {
    CRITICAL_SECTION();
    for (ktimer_t *t = timers; t; t = t->next)
        if (t->id == id) t->dead = true;
}

void timers_remove_window(window_t *w) {
    CRITICAL_SECTION();
    for (ktimer_t *t = timers; t; t = t->next)
        if (t->win == w) t->dead = true;
}

void timers_remove_app(const app_desc_t *app) {
    CRITICAL_SECTION();
    for (ktimer_t *t = timers; t; t = t->next)
        if (!t->win && t->app == app) t->dead = true;
}

static void sweep(void) {
    CRITICAL_SECTION();
    ktimer_t **pp = &timers;
    while (*pp) {
        ktimer_t *t = *pp;
        if (t->dead) { *pp = t->next; kfree(t); }
        else pp = &t->next;
    }
}

void timers_run(void) {
    uint32_t now = g_ticks;
    /* Collect due timers first: callbacks may add/kill timers. */
    for (ktimer_t *t = timers; t; t = t->next) {
        if (t->dead || (int32_t)(now - t->due) < 0) continue;
        if (!t->win && t->app && wd_blocked(t->app)) { t->due = now + 20; continue; }   /* app stuck: later */
        if (t->repeat) {
            t->due += t->interval;
            if ((int32_t)(now - t->due) >= 0) t->due = now + t->interval;   /* fell behind: skip */
        } else {
            t->dead = true;
        }
        if (t->win) {
            if (t->win->closing || t->win->magic != WIN_MAGIC) { t->dead = true; continue; }
            event_t ev = { .type = EV_TIMER, .timer_id = t->id };
            wm_send(t->win, &ev);
        } else if (t->app) {
            const app_desc_t *saved = wm_current_app;
            wm_current_app = t->app;
            wd_cb_begin(t->app);
            t->fn(t->ctx);
            wd_cb_end();
            wm_current_app = saved;
        } else {
            t->fn(t->ctx);
        }
    }
    sweep();
}

uint32_t timers_next_due(void) {
    uint32_t best = 0;
    bool any = false;
    for (ktimer_t *t = timers; t; t = t->next) {
        if (t->dead) continue;
        if (!any || (int32_t)(t->due - best) < 0) { best = t->due; any = true; }
    }
    return any ? (best ? best : 1) : 0;
}
