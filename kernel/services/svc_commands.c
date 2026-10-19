/* svc_commands.c - `service` / `systemctl`, `notify` and `logs` commands. */
#include "services/svc_internal.h"

#define C_RESET(o)  (((o) && ((o)->flags & CMD_ANSI)) ? "\x1b[0m" : "")
#define C_GREEN(o)  (((o) && ((o)->flags & CMD_ANSI)) ? "\x1b[92m" : "")
#define C_RED(o)    (((o) && ((o)->flags & CMD_ANSI)) ? "\x1b[91m" : "")
#define C_GRAY(o)   (((o) && ((o)->flags & CMD_ANSI)) ? "\x1b[90m" : "")
#define C_YELLOW(o) (((o) && ((o)->flags & CMD_ANSI)) ? "\x1b[93m" : "")
#define C_BOLD(o)   (((o) && ((o)->flags & CMD_ANSI)) ? "\x1b[1m" : "")

static const char *state_color(cmd_writer_t *o, int st) {
    return st == SVC_RUNNING ? C_GREEN(o) : st == SVC_FAILED ? C_RED(o) : st == SVC_STARTING ? C_YELLOW(o) : C_GRAY(o);
}

static void fmt_uptime(uint32_t ms, char *buf, int n) {
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(buf, (size_t)n, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(buf, (size_t)n, "%u:%02u", s / 60, s % 60);
}

static void list_services(cmd_writer_t *out) {
    cmd_printf(out, "%s%-10s %-9s %-9s %-8s %s%s\n", C_BOLD(out), "SERVICE", "STATE", "AUTOSTART", "UPTIME", "DESCRIPTION", C_RESET(out));
    for (int i = 0; i < service_count(); i++) {
        service_status_t st;
        service_status_at(i, &st);
        char up[16] = "-";
        if (st.state == SVC_RUNNING) fmt_uptime(st.uptime_ms, up, sizeof up);
        cmd_printf(out, "%-10s %s%-9s%s %-9s %-8s %s\n", st.name, state_color(out, st.state), service_state_name(st.state),
                   C_RESET(out), st.autostart ? "yes" : "no", up, st.description);
    }
}

static int show_status(cmd_writer_t *out, const char *name) {
    service_status_t st;
    if (service_status(name, &st) != 0) { cmd_printf(out, "service: unknown service '%s'\n", name); return 1; }
    char up[16] = "-";
    if (st.state == SVC_RUNNING) fmt_uptime(st.uptime_ms, up, sizeof up);
    cmd_printf(out, "%s%s%s - %s\n", C_BOLD(out), st.display_name, C_RESET(out), st.description);
    cmd_printf(out, "   Name: %s\n", st.name);
    cmd_printf(out, "  State: %s%s%s%s%s\n", state_color(out, st.state), service_state_name(st.state), C_RESET(out),
               st.state == SVC_RUNNING ? " for " : "", st.state == SVC_RUNNING ? up : "");
    cmd_printf(out, "  Start: %s at boot, %s on failure\n", st.autostart ? "automatically" : "manually",
               st.restart_on_failure ? "restarted" : "not restarted");
    if (st.tick_ms) cmd_printf(out, "   Tick: every %u ms\n", st.tick_ms);
    if (st.restarts) cmd_printf(out, "Restarts: %d\n", st.restarts);
    if (st.last_error[0]) cmd_printf(out, "  Error: %s%s%s\n", C_RED(out), st.last_error, C_RESET(out));
    char *log = kmalloc(4096);
    if (log) {
        int n = service_log_text(st.name, log, 4096);
        if (n > 0) {
            /* last 8 lines */
            int lines = 0, pos = n;
            while (pos > 0 && lines <= 8) { pos--; if (log[pos] == '\n' && pos != n - 1) lines++; }
            if (lines > 8) pos++;
            cmd_printf(out, "\n%sRecent log:%s\n%s", C_BOLD(out), C_RESET(out), log + pos);
        }
        kfree(log);
    }
    return 0;
}

static int cmd_service(int argc, char **argv, cmd_writer_t *out) {
    if (argc < 2 || !strcmp(argv[1], "list") || !strcmp(argv[1], "ls")) { list_services(out); return 0; }
    const char *verb = argv[1];
    if (!strcmp(verb, "help") || !strcmp(verb, "--help")) {
        cmd_printf(out, "usage: %s list | status NAME | start NAME | stop NAME | restart NAME | enable NAME | disable NAME\n", argv[0]);
        return 0;
    }
    if (argc < 3) {
        if (!strcmp(verb, "status")) { list_services(out); return 0; }
        cmd_printf(out, "%s: %s needs a service name (try '%s list')\n", argv[0], verb, argv[0]);
        return 2;
    }
    int status = 0;
    for (int i = 2; i < argc; i++) {
        const char *name = argv[i];
        if (service_find(name) < 0) { cmd_printf(out, "%s: unknown service '%s'\n", argv[0], name); status = 1; continue; }
        int r = 0;
        if (!strcmp(verb, "start")) r = service_start(name);
        else if (!strcmp(verb, "stop")) r = service_stop(name);
        else if (!strcmp(verb, "restart")) r = service_restart(name);
        else if (!strcmp(verb, "enable")) r = service_set_autostart(name, true);
        else if (!strcmp(verb, "disable")) r = service_set_autostart(name, false);
        else if (!strcmp(verb, "status")) { status |= show_status(out, name); continue; }
        else { cmd_printf(out, "%s: unknown action '%s'\n", argv[0], verb); return 2; }
        service_status_t st;
        service_status(name, &st);
        if (r == 0) {
            if (!strcmp(verb, "enable") || !strcmp(verb, "disable"))
                cmd_printf(out, "%s: autostart %s\n", st.name, st.autostart ? "enabled" : "disabled");
            else
                cmd_printf(out, "%s: %s%s%s\n", st.name, state_color(out, st.state), service_state_name(st.state), C_RESET(out));
        } else {
            cmd_printf(out, "%s: %sfailed%s%s%s\n", st.name, C_RED(out), C_RESET(out), st.last_error[0] ? " - " : "", st.last_error);
            status = 1;
        }
    }
    return status;
}
REGISTER_COMMAND("service", "service list|status|start|stop|restart|enable|disable [NAME] - manage background services", cmd_service);
REGISTER_COMMAND("systemctl", "systemctl list|status|start|stop|restart|enable|disable [NAME] - same as service", cmd_service);

static int cmd_notify(int argc, char **argv, cmd_writer_t *out) {
    if (argc < 2) { cmd_puts(out, "usage: notify TITLE [BODY...]\n"); return 2; }
    char body[192] = "";
    for (int i = 2; i < argc; i++) {
        if (i > 2) strlcat(body, " ", sizeof body);
        strlcat(body, argv[i], sizeof body);
    }
    int id = notify(argv[1], body, ICON_BELL);
    if (!id) { cmd_puts(out, "notify: the notifyd service is not running (service start notifyd)\n"); return 1; }
    return 0;
}
REGISTER_COMMAND("notify", "notify TITLE [BODY...] - show a desktop notification", cmd_notify);

static int cmd_logs(int argc, char **argv, cmd_writer_t *out) {
    int lines = 40;
    const char *svc = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) lines = atoi(argv[++i]);
        else svc = argv[i];
    }
    if (lines <= 0) lines = 40;
    char tag[16] = "";
    if (svc) {
        service_status_t st;
        if (service_status(svc, &st) == 0) strlcpy(tag, st.name, sizeof tag);
        else strlcpy(tag, svc, sizeof tag);
    }
    uint32_t first = syslog_first_seq(), last = syslog_last_seq();
    /* walk back to find the first of the last `lines` matching entries */
    uint32_t start = last + 1;
    int found = 0;
    for (uint32_t seq = last; seq >= first && seq > 0 && found < lines; seq--) {
        log_entry_t e;
        if (!syslog_get(seq, &e)) break;
        if (tag[0] && strcasecmp(e.tag, tag)) continue;
        start = seq;
        found++;
    }
    if (!found) { cmd_printf(out, "logs: no entries%s%s\n", tag[0] ? " for " : "", tag); return tag[0] ? 1 : 0; }
    for (uint32_t seq = start; seq <= last; seq++) {
        log_entry_t e;
        if (!syslog_get(seq, &e) || (tag[0] && strcasecmp(e.tag, tag))) continue;
        char line[256];
        syslog_format(&e, line, sizeof line);
        const char *col = e.level == LOG_ERROR ? C_RED(out) : e.level == LOG_WARN ? C_YELLOW(out) : "";
        cmd_printf(out, "%s%s%s\n", col, line, *col ? C_RESET(out) : "");
    }
    return 0;
}
REGISTER_COMMAND("logs", "logs [SERVICE] [-n LINES] - show the system log (or one service's entries)", cmd_logs);
