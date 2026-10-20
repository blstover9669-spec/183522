/*
 * alarmd.c - alarms at a time of day ("07:30") or in N minutes.
 *
 * When an alarm is due it shows a notification and plays a short chime on
 * the PC speaker. Pending alarms are kept in /system/config/alarms so they
 * survive a reboot on an installed system.
 */
#include "services/svc_internal.h"

#define MAX_ALARMS 32

static alarm_info_t alarms[MAX_ALARMS];
static int nalarms;
static int next_id = 1;
static bool loaded;

static int cmp_due(const void *a, const void *b) {
    const alarm_info_t *x = a, *y = b;
    return x->due < y->due ? -1 : x->due > y->due ? 1 : x->id - y->id;
}

static void save(void) {
    char *buf = kmalloc(MAX_ALARMS * 128 + 64);
    if (!buf) return;
    int n = snprintf(buf, 64, "# pending alarms: due-time(unix) message\n");
    for (int i = 0; i < nalarms; i++) n += snprintf(buf + n, 128, "%u %s\n", alarms[i].due, alarms[i].message);
    config_write("alarms", buf);
    kfree(buf);
}

static void load(void) {
    if (loaded) return;
    loaded = true;
    char *text = config_read("alarms");
    if (!text) return;
    char *cur = text, *line;
    while ((line = config_next_line(&cur)) && nalarms < MAX_ALARMS) {
        if (!*line || *line == '#') continue;
        char *end;
        unsigned long due = strtoul(line, &end, 10);
        if (end == line || !due) continue;
        while (*end == ' ') end++;
        alarm_info_t *a = &alarms[nalarms++];
        a->id = next_id++;
        a->due = (uint32_t)due;
        strlcpy(a->message, end, sizeof a->message);
    }
    free(text);
    qsort(alarms, (size_t)nalarms, sizeof alarms[0], cmp_due);
}

static int add(uint32_t due, const char *message) {
    load();
    if (nalarms >= MAX_ALARMS) return -1;
    alarm_info_t *a = &alarms[nalarms++];
    memset(a, 0, sizeof *a);
    a->id = next_id++;
    a->due = due;
    strlcpy(a->message, message && *message ? message : "Alarm", sizeof a->message);
    int id = a->id;
    qsort(alarms, (size_t)nalarms, sizeof alarms[0], cmp_due);
    save();
    datetime_t dt;
    sys_time_to_datetime(due, &dt);
    log_info("alarmd", "alarm %d set for %02d:%02d: %s", id, dt.hour, dt.minute, message && *message ? message : "Alarm");
    return id;
}

int alarm_add_at(int hour, int minute, const char *message) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return -1;
    datetime_t dt;
    sys_datetime(&dt);
    dt.hour = hour;
    dt.minute = minute;
    dt.second = 0;
    uint32_t due = sys_datetime_to_time(&dt);
    if (due <= sys_time()) due += 24 * 3600;      /* that time already passed today: tomorrow */
    return add(due, message);
}

int alarm_add_in(int minutes, const char *message) {
    if (minutes < 0 || minutes > 7 * 24 * 60) return -1;
    return add(sys_time() + (uint32_t)minutes * 60, message);
}

bool alarm_cancel(int id) {
    load();
    for (int i = 0; i < nalarms; i++) {
        if (alarms[i].id != id) continue;
        memmove(&alarms[i], &alarms[i + 1], sizeof alarms[0] * (size_t)(nalarms - i - 1));
        nalarms--;
        save();
        log_info("alarmd", "alarm %d cancelled", id);
        return true;
    }
    return false;
}

int alarm_count(void) { load(); return nalarms; }

bool alarm_get(int index, alarm_info_t *out) {
    load();
    if (index < 0 || index >= nalarms || !out) return false;
    *out = alarms[index];
    return true;
}

static void chime(void) {
    static const note_t notes[] = {
        { 1319, 120 }, { 0, 30 }, { 1047, 120 }, { 0, 30 }, { 1568, 220 }, { 0, 160 },
        { 1319, 120 }, { 0, 30 }, { 1047, 120 }, { 0, 30 }, { 1568, 320 },
    };
    sys_play_notes(notes, ARRAY_SIZE(notes));
}

static void alarmd_tick(void) {
    uint32_t now = sys_time();
    bool changed = false;
    while (nalarms && alarms[0].due <= now) {
        alarm_info_t a = alarms[0];
        memmove(&alarms[0], &alarms[1], sizeof alarms[0] * (size_t)(nalarms - 1));
        nalarms--;
        changed = true;
        datetime_t dt;
        sys_time_to_datetime(a.due, &dt);
        char title[48];
        snprintf(title, sizeof title, "Alarm - %02d:%02d", dt.hour, dt.minute);
        log_info("alarmd", "alarm %d: %s", a.id, a.message);
        notify(title, a.message, ICON_ALARM);
        chime();
    }
    if (changed) save();
}

static int alarmd_start(void) {
    loaded = false;
    nalarms = 0;
    load();
    if (nalarms) log_info("alarmd", "%d pending alarm%s", nalarms, nalarms == 1 ? "" : "s");
    return 0;
}

REGISTER_SERVICE(.name = "alarmd", .display_name = "Alarms",
                 .description = "Rings alarms at a set time with a notification and a chime",
                 .autostart = true, .restart_on_failure = true,
                 .start = alarmd_start, .tick = alarmd_tick, .tick_ms = 1000,
                 .icon = ICON_ALARM);

/* ---- `alarm` command ------------------------------------------------------------------------ */
static void join_args(int argc, char **argv, int from, char *out, size_t n) {
    out[0] = 0;
    for (int i = from; i < argc; i++) {
        if (i > from) strlcat(out, " ", n);
        strlcat(out, argv[i], n);
    }
}

static int cmd_alarm(int argc, char **argv, cmd_writer_t *out) {
    if (argc < 2 || !strcmp(argv[1], "list")) {
        int n = alarm_count();
        if (!n) cmd_puts(out, "No alarms set.\n");
        for (int i = 0; i < n; i++) {
            alarm_info_t a;
            alarm_get(i, &a);
            datetime_t dt;
            sys_time_to_datetime(a.due, &dt);
            uint32_t now = sys_time(), left = a.due > now ? a.due - now : 0;
            cmd_printf(out, "  #%-3d %02d:%02d  (in %u:%02u)  %s\n", a.id, dt.hour, dt.minute, left / 3600, (left / 60) % 60, a.message);
        }
        if (argc < 2) cmd_puts(out, "usage: alarm HH:MM MESSAGE | alarm +MINUTES MESSAGE | alarm cancel ID | alarm list\n");
        return 0;
    }
    if (!strcmp(argv[1], "cancel") || !strcmp(argv[1], "rm") || !strcmp(argv[1], "delete")) {
        if (argc < 3) { cmd_puts(out, "usage: alarm cancel ID\n"); return 2; }
        int id = atoi(argv[2][0] == '#' ? argv[2] + 1 : argv[2]);
        if (!alarm_cancel(id)) { cmd_printf(out, "alarm: no alarm #%d\n", id); return 1; }
        cmd_printf(out, "alarm #%d cancelled\n", id);
        return 0;
    }
    char msg[96];
    join_args(argc, argv, 2, msg, sizeof msg);
    const char *when = argv[1];
    int id;
    if (when[0] == '+' || (isdigit((unsigned char)when[0]) && !strchr(when, ':'))) {
        int minutes = atoi(when[0] == '+' ? when + 1 : when);
        size_t wl = strlen(when);
        if (wl && (when[wl - 1] == 'h' || when[wl - 1] == 'H')) minutes *= 60;
        if (minutes <= 0) { cmd_puts(out, "alarm: expected +MINUTES (e.g. +5) or HH:MM\n"); return 2; }
        id = alarm_add_in(minutes, msg);
    } else {
        int h = -1, m = -1;
        if (sscanf(when, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
            cmd_puts(out, "alarm: expected HH:MM (e.g. 07:30) or +MINUTES\n");
            return 2;
        }
        id = alarm_add_at(h, m, msg);
    }
    if (id < 0) { cmd_puts(out, "alarm: could not add the alarm (too many?)\n"); return 1; }
    alarm_info_t a = { 0 };
    for (int i = 0; i < alarm_count(); i++) { alarm_get(i, &a); if (a.id == id) break; }
    datetime_t dt;
    sys_time_to_datetime(a.due, &dt);
    cmd_printf(out, "Alarm #%d set for %02d:%02d%s%s\n", id, dt.hour, dt.minute, msg[0] ? ": " : "", msg);
    if (!service_is_running("alarmd")) cmd_puts(out, "note: the alarmd service is stopped - start it with 'service start alarmd'\n");
    return 0;
}
REGISTER_COMMAND("alarm", "alarm HH:MM|+MINUTES [MESSAGE] - set an alarm (also: alarm list, alarm cancel ID)", cmd_alarm);
