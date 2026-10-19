/*
 * autosave.c - calls registered save callbacks every 60 seconds (and when the
 * service stops, e.g. at shutdown). The installer's disk persistence and apps
 * with unsaved state hook in with autosave_register().
 */
#include "services/svc_internal.h"

#define MAX_SAVERS 16

static struct { int id; char name[32]; autosave_fn fn; void *ctx; } savers[MAX_SAVERS];
static int next_id = 1;
static uint32_t last_run;
static bool running;

int autosave_register(const char *name, autosave_fn fn, void *ctx) {
    if (!fn) return 0;
    for (int i = 0; i < MAX_SAVERS; i++) {
        if (savers[i].fn) continue;
        savers[i].id = next_id++;
        strlcpy(savers[i].name, name ? name : "?", sizeof savers[i].name);
        savers[i].fn = fn;
        savers[i].ctx = ctx;
        log_info("autosave", "registered \"%s\"", savers[i].name);
        return savers[i].id;
    }
    log_warn("autosave", "too many callbacks, \"%s\" not registered", name ? name : "?");
    return 0;
}

void autosave_unregister(int id) {
    for (int i = 0; i < MAX_SAVERS; i++)
        if (savers[i].fn && savers[i].id == id) { memset(&savers[i], 0, sizeof savers[i]); return; }
}

void autosave_now(void) {
    int n = 0;
    for (int i = 0; i < MAX_SAVERS; i++) {
        if (!savers[i].fn) continue;
        savers[i].fn(savers[i].ctx);
        n++;
    }
    last_run = sys_time();
    if (n) log_info("autosave", "saved (%d callback%s)", n, n == 1 ? "" : "s");
}

uint32_t autosave_last_run(void) { return last_run; }

static int autosave_start(void) { running = true; return 0; }
static void autosave_stop(void) { if (running) autosave_now(); running = false; }

REGISTER_SERVICE(.name = "autosave", .display_name = "Autosave",
                 .description = "Runs registered save callbacks every 60 seconds and at shutdown",
                 .autostart = true, .restart_on_failure = true,
                 .start = autosave_start, .stop = autosave_stop, .tick = autosave_now, .tick_ms = 60000,
                 .icon = ICON_DISK);
