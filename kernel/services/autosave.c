/*
 * autosave.c - calls registered save callbacks every 60 seconds (and when the
 * service stops, e.g. at shutdown). The installer's disk persistence and apps
 * with unsaved state hook in with autosave_register().
 */
#include "services/svc_internal.h"
#include <nova/disk.h>

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

static int run_savers(void) {
    int n = 0;
    for (int i = 0; i < MAX_SAVERS; i++) {
        if (!savers[i].fn) continue;
        savers[i].fn(savers[i].ctx);
        n++;
    }
    last_run = sys_time();
    return n;
}

void autosave_now(void) {
    int n = run_savers();
    if (n) log_info("autosave", "saved (%d callback%s)", n, n == 1 ? "" : "s");
}

/* The periodic run stays quiet: a log line every minute would itself change
   /system/log/syslog (via logd) and keep the disk busy for nothing. */
static void autosave_tick(void) { run_savers(); }

uint32_t autosave_last_run(void) { return last_run; }

/* The installed system's disk persistence (kernel/disk/persist.c) saves on its
   own a few seconds after a change; hooking it in here also makes the autosave
   service's 60 s cycle and its final run at shutdown write the file system to
   disk: a background save while running (never blocks the GUI), a synchronous
   one while shutting down, so that changes made by services that stop before
   autosave (logd's last flush) still reach the disk. */
static int disk_saver;
static void save_to_disk(void *ctx) {
    (void)ctx;
    if (!persist_active()) return;
    if (svc_shutting_down()) {
        int r = persist_save();
        if (r != DISK_OK) log_warn("autosave", "disk save failed: %s", disk_strerror(r));
        return;
    }
    persist_info_t pi;
    persist_get_info(&pi);
    if (pi.dirty && !pi.saving) persist_save_async();
}

static int autosave_start(void) {
    running = true;
    if (!disk_saver && persist_active()) disk_saver = autosave_register("disk", save_to_disk, NULL);
    return 0;
}
static void autosave_stop(void) { if (running) autosave_now(); running = false; }

REGISTER_SERVICE(.name = "autosave", .display_name = "Autosave",
                 .description = "Runs registered save callbacks every 60 seconds and at shutdown",
                 .autostart = true, .restart_on_failure = true,
                 .start = autosave_start, .stop = autosave_stop, .tick = autosave_tick, .tick_ms = 60000,
                 .icon = ICON_DISK);
