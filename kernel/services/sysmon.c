/*
 * sysmon.c - samples memory, CPU load (share of time the main loop was not
 * halted) and events per second once a second into a ring buffer for Task
 * Manager style graphs (sysmon_history()).
 */
#include "services/svc_internal.h"

static sysmon_sample_t ring[SYSMON_HISTORY];
static int nsamples;           /* total ever taken */
static uint32_t last_ms, last_idle, last_events;

static void baseline(void) {
    last_ms = sys_uptime_ms();
    last_idle = wm_stat_idle_ms;
    last_events = wm_stat_events;
}

static void sample(void) {
    uint32_t now = sys_uptime_ms(), idle = wm_stat_idle_ms, ev = wm_stat_events;
    uint32_t dt = now - last_ms;
    if (dt == 0) return;
    uint32_t di = idle - last_idle;
    if (di > dt) di = dt;
    sysmon_sample_t *s = &ring[nsamples % SYSMON_HISTORY];
    s->uptime_ms = now;
    s->mem_used = sys_mem_used();
    s->mem_total = sys_heap_size();
    s->cpu_percent = (int)(100 - di * 100 / dt);
    s->events_per_sec = (int)((ev - last_events) * 1000 / dt);
    s->fps = sys_fps();
    nsamples++;
    last_ms = now;
    last_idle = idle;
    last_events = ev;
}

int sysmon_history(sysmon_sample_t *out, int max) {
    int n = MIN(nsamples, SYSMON_HISTORY);
    if (n > max) n = max;
    int first = nsamples - n;
    for (int i = 0; i < n; i++) out[i] = ring[(first + i) % SYSMON_HISTORY];
    return n;
}

bool sysmon_latest(sysmon_sample_t *out) {
    if (!nsamples || !out) return false;
    *out = ring[(nsamples - 1) % SYSMON_HISTORY];
    return true;
}

static int sysmon_start(void) { baseline(); return 0; }

REGISTER_SERVICE(.name = "sysmon", .display_name = "System Monitor",
                 .description = "Samples memory, CPU load and events per second for graphs",
                 .autostart = true, .restart_on_failure = true,
                 .start = sysmon_start, .tick = sample, .tick_ms = 1000,
                 .icon = ICON_SERVICE);

/* `top`-like one-shot summary */
static int cmd_sysmon(int argc, char **argv, cmd_writer_t *out) {
    sysmon_sample_t s;
    if (!sysmon_latest(&s)) { cmd_puts(out, "sysmon: no samples yet (is the sysmon service running?)\n"); return 1; }
    cmd_printf(out, "CPU %3d%%   memory %u / %u KiB   %d events/s   %d fps\n", s.cpu_percent,
               s.mem_used >> 10, s.mem_total >> 10, s.events_per_sec, s.fps);
    return 0;
}
REGISTER_COMMAND("sysmon", "sysmon - show the latest CPU / memory / event sample", cmd_sysmon);
