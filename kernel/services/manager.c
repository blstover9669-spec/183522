/*
 * manager.c - the service manager.
 *
 * Services register with REGISTER_SERVICE (".services" linker section). The
 * manager starts the autostart ones once the desktop is up (INIT_LATE),
 * drives their tick callbacks from the timer system, restarts failed ones
 * with an exponential back-off, keeps a small log ring per service and stops
 * everything cleanly before reboot / power-off.
 */
#include "services/svc_internal.h"

extern const service_desc_t *const __services_start[];
extern const service_desc_t *const __services_end[];

#define SVC_LOG 40

typedef struct {
    const service_desc_t *d;
    int state;
    bool autostart;
    uint32_t started_ms;
    int tick_timer;
    int restart_timer;
    int restarts;
    uint32_t backoff_ms;
    char last_error[96];
    log_entry_t log[SVC_LOG];
    int log_n;               /* entries ever logged (ring index = n % SVC_LOG) */
} svc_t;

static svc_t *svcs;
static int nsvc;
static bool inited;
static uint32_t gen = 1;
static bool shutting_down;

static int cmp_svc(const void *a, const void *b) {
    return strcmp(((const svc_t *)a)->d->name, ((const svc_t *)b)->d->name);
}

static void ensure_init(void) {
    if (inited) return;
    inited = true;
    int n = (int)(__services_end - __services_start);
    svcs = kcalloc((size_t)MAX(n, 1), sizeof *svcs);
    if (!svcs) return;
    for (int i = 0; i < n; i++) {
        const service_desc_t *d = __services_start[i];
        if (!d || !d->name) continue;
        bool dup = false;
        for (int j = 0; j < nsvc; j++) if (!strcmp(svcs[j].d->name, d->name)) dup = true;
        if (dup) { klog("services: duplicate service \"%s\" ignored", d->name); continue; }
        svcs[nsvc].d = d;
        svcs[nsvc].autostart = d->autostart;
        svcs[nsvc].backoff_ms = 1000;
        nsvc++;
    }
    qsort(svcs, (size_t)nsvc, sizeof *svcs, cmp_svc);
}

static svc_t *find(const char *name) {
    ensure_init();
    if (!name) return NULL;
    for (int i = 0; i < nsvc; i++) if (!strcasecmp(svcs[i].d->name, name)) return &svcs[i];
    /* also accept the display name ("Notifications") */
    for (int i = 0; i < nsvc; i++)
        if (svcs[i].d->display_name && !strcasecmp(svcs[i].d->display_name, name)) return &svcs[i];
    return NULL;
}

int service_count(void) { ensure_init(); return nsvc; }
bool svc_shutting_down(void) { return shutting_down; }
int service_find(const char *name) { svc_t *s = find(name); return s ? (int)(s - svcs) : -1; }
uint32_t service_generation(void) { return gen; }

const char *service_state_name(int state) {
    switch (state) {
    case SVC_STARTING: return "starting";
    case SVC_RUNNING: return "running";
    case SVC_FAILED: return "failed";
    default: return "stopped";
    }
}

static void fill_status(svc_t *s, service_status_t *o) {
    memset(o, 0, sizeof *o);
    o->name = s->d->name;
    o->display_name = s->d->display_name ? s->d->display_name : s->d->name;
    o->description = s->d->description ? s->d->description : "";
    o->state = s->state;
    o->autostart = s->autostart;
    o->restart_on_failure = s->d->restart_on_failure;
    o->started_ms = s->state == SVC_RUNNING ? s->started_ms : 0;
    o->uptime_ms = s->state == SVC_RUNNING ? sys_uptime_ms() - s->started_ms : 0;
    o->tick_ms = s->d->tick ? s->d->tick_ms : 0;
    o->restarts = s->restarts;
    o->icon = s->d->icon;
    strlcpy(o->last_error, s->last_error, sizeof o->last_error);
}

int service_status_at(int index, service_status_t *out) {
    ensure_init();
    if (index < 0 || index >= nsvc || !out) return -1;
    fill_status(&svcs[index], out);
    return 0;
}

int service_status(const char *name, service_status_t *out) {
    svc_t *s = find(name);
    if (!s || !out) return -1;
    fill_status(s, out);
    return 0;
}

bool service_is_running(const char *name) {
    svc_t *s = find(name);
    return s && s->state == SVC_RUNNING;
}

/* ---- per-service log ---------------------------------------------------------------------- */
void svc_note_log(const log_entry_t *e) {
    if (!inited || !svcs) return;
    for (int i = 0; i < nsvc; i++) {
        if (strcmp(svcs[i].d->name, e->tag)) continue;
        svcs[i].log[svcs[i].log_n % SVC_LOG] = *e;
        svcs[i].log_n++;
        gen++;
        return;
    }
}

int service_log_text(const char *name, char *buf, int size) {
    svc_t *s = find(name);
    if (!buf || size <= 0) return 0;
    buf[0] = 0;
    if (!s) return 0;
    int n = 0;
    int first = MAX(0, s->log_n - SVC_LOG);
    for (int i = first; i < s->log_n && n < size - 1; i++) {
        const log_entry_t *e = &s->log[i % SVC_LOG];
        datetime_t dt;
        sys_time_to_datetime(e->time, &dt);
        n += snprintf(buf + n, (size_t)(size - n), "%02d:%02d:%02d %-5s %s\n", dt.hour, dt.minute, dt.second,
                      log_level_name(e->level), e->msg);
    }
    if (n >= size) n = size - 1;
    return n;
}

/* ---- control ---------------------------------------------------------------------------- */
static void tick_cb(void *ctx) {
    svc_t *s = ctx;
    if (s->state == SVC_RUNNING && s->d->tick) s->d->tick();
}

static void restart_cb(void *ctx) {
    svc_t *s = ctx;
    s->restart_timer = 0;
    if (s->state != SVC_FAILED || shutting_down) return;
    s->restarts++;
    log_warn(s->d->name, "restarting after failure (attempt %d)", s->restarts);
    service_start(s->d->name);
}

static void schedule_restart(svc_t *s) {
    if (!s->d->restart_on_failure || shutting_down || s->restart_timer) return;
    s->restart_timer = timer_add(s->backoff_ms, restart_cb, s, false);
    s->backoff_ms = MIN(s->backoff_ms * 2, 30000u);
}

static void mark_failed(svc_t *s, const char *why) {
    if (s->tick_timer) { timer_cancel(s->tick_timer); s->tick_timer = 0; }
    bool was_up = s->state == SVC_RUNNING;
    s->state = SVC_FAILED;
    if (why && *why) strlcpy(s->last_error, why, sizeof s->last_error);
    if (was_up && s->d->stop) s->d->stop();
    log_error(s->d->name, "failed: %s", s->last_error[0] ? s->last_error : "unknown error");
    gen++;
    schedule_restart(s);
}

int service_start(const char *name) {
    svc_t *s = find(name);
    if (!s) return -1;
    if (s->state == SVC_RUNNING || s->state == SVC_STARTING) return 0;
    if (s->restart_timer) { timer_cancel(s->restart_timer); s->restart_timer = 0; }
    s->state = SVC_STARTING;
    gen++;
    int r = s->d->start ? s->d->start() : 0;
    if (s->state == SVC_FAILED) return -2;      /* service_fail() was called inside start() */
    if (r != 0) {
        char why[96];
        if (!s->last_error[0]) snprintf(why, sizeof why, "start returned %d", r);
        else strlcpy(why, s->last_error, sizeof why);
        s->state = SVC_STARTING;               /* not "running": mark_failed must not call stop() */
        mark_failed(s, why);
        return -2;
    }
    s->state = SVC_RUNNING;
    s->started_ms = sys_uptime_ms();
    s->last_error[0] = 0;
    if (s->d->tick && s->d->tick_ms) s->tick_timer = timer_add(s->d->tick_ms, tick_cb, s, true);
    log_info(s->d->name, "started");
    gen++;
    return 0;
}

int service_stop(const char *name) {
    svc_t *s = find(name);
    if (!s) return -1;
    if (s->restart_timer) { timer_cancel(s->restart_timer); s->restart_timer = 0; }
    if (s->state == SVC_STOPPED) return 0;
    if (s->tick_timer) { timer_cancel(s->tick_timer); s->tick_timer = 0; }
    bool was_up = s->state == SVC_RUNNING;
    s->state = SVC_STOPPED;
    s->backoff_ms = 1000;
    if (was_up && s->d->stop) s->d->stop();
    log_info(s->d->name, "stopped");
    gen++;
    return 0;
}

int service_restart(const char *name) {
    svc_t *s = find(name);
    if (!s) return -1;
    service_stop(name);
    return service_start(name);
}

void service_fail(const char *name, const char *fmt, ...) {
    svc_t *s = find(name);
    if (!s) return;
    char why[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    if (s->state == SVC_STOPPED || s->state == SVC_FAILED) return;
    mark_failed(s, why);
}

/* ---- autostart settings ---------------------------------------------------------------------- */
static void save_settings(void) {
    char *buf = kmalloc((size_t)nsvc * 48 + 80);
    if (!buf) return;
    int n = snprintf(buf, 80, "# service autostart: name=on|off\n");
    for (int i = 0; i < nsvc; i++) n += snprintf(buf + n, 48, "%s=%s\n", svcs[i].d->name, svcs[i].autostart ? "on" : "off");
    config_write("services", buf);
    kfree(buf);
}

static void load_settings(void) {
    char *text = config_read("services");
    if (!text) return;
    char *cur = text, *line;
    while ((line = config_next_line(&cur))) {
        char *eq = strchr(line, '=');
        if (!eq || *line == '#') continue;
        *eq = 0;
        svc_t *s = find(line);
        if (s) s->autostart = !strcasecmp(eq + 1, "on") || !strcmp(eq + 1, "1") || !strcasecmp(eq + 1, "yes");
    }
    free(text);
}

int service_set_autostart(const char *name, bool on) {
    svc_t *s = find(name);
    if (!s) return -1;
    if (s->autostart != on) {
        s->autostart = on;
        save_settings();
        log_info(s->d->name, "autostart %s", on ? "enabled" : "disabled");
        gen++;
    }
    return 0;
}

static void services_shutdown(void) {
    shutting_down = true;
    for (int i = nsvc - 1; i >= 0; i--)
        if (svcs[i].state == SVC_RUNNING || svcs[i].state == SVC_FAILED) service_stop(svcs[i].d->name);
    klog("services: all stopped");
}

static void services_autostart(void) {
    ensure_init();
    load_settings();
    sys_add_shutdown_hook(services_shutdown);
    int started = 0;
    /* notifyd first so that other services can notify while starting */
    svc_t *nd = find("notifyd");
    if (nd && nd->autostart && service_start("notifyd") == 0) started++;
    for (int i = 0; i < nsvc; i++)
        if (svcs[i].autostart && svcs[i].state == SVC_STOPPED && service_start(svcs[i].d->name) == 0) started++;
    klog("services: %d registered, %d started", nsvc, started);
}
REGISTER_INIT(INIT_LATE, services_autostart);
