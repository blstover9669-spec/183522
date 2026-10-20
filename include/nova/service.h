/*
 * nova/service.h - background services, system log, shell commands and the
 * APIs of the built-in services (notifications, screensaver, alarms,
 * autosave, file index, system monitor).
 *
 * A service is a set of callbacks driven by the main loop - like an app
 * without a window. It must never block: do periodic work in `tick`, which
 * the service manager calls every `tick_ms` from a timer.
 *
 *   static int  myd_start(void) { ...; return 0; }      // 0 = ok, else failed
 *   static void myd_stop(void)  { ... }
 *   static void myd_tick(void)  { ... }                 // every tick_ms
 *   REGISTER_SERVICE(.name = "myd", .display_name = "My Daemon",
 *                    .description = "Does things in the background",
 *                    .autostart = true, .start = myd_start, .stop = myd_stop,
 *                    .tick = myd_tick, .tick_ms = 1000);
 */
#ifndef NOVA_SERVICE_H
#define NOVA_SERVICE_H

#include <nova/types.h>
#include <nova/wm.h>

/* ==== service manager ================================================================== */
enum service_state { SVC_STOPPED = 0, SVC_STARTING, SVC_RUNNING, SVC_FAILED };

typedef struct service_desc {
    const char *name;            /* short unique id ("notifyd"), used by `service start NAME` */
    const char *display_name;    /* "Notifications"                                         */
    const char *description;     /* one line for the Services app                            */
    bool autostart;              /* default; the user's choice is kept in /system/config/services */
    bool restart_on_failure;     /* restart automatically (with back-off) after a failure     */
    int  (*start)(void);         /* 0 = running, nonzero = failed (see service_fail)          */
    void (*stop)(void);          /* release everything; may be NULL                           */
    void (*tick)(void);          /* optional periodic work, every tick_ms while running       */
    uint32_t tick_ms;
    int icon;                    /* ICON_* for UIs (0 = gear)                                 */
} service_desc_t;

#define _SVC_CAT2(a, b) a##b
#define _SVC_CAT(a, b) _SVC_CAT2(a, b)
#define REGISTER_SERVICE(...)                                                           \
    static const service_desc_t _SVC_CAT(_svc_desc_, __LINE__) = { __VA_ARGS__ };       \
    static const service_desc_t *const _SVC_CAT(_svc_ptr_, __LINE__)                    \
        __attribute__((used, section(".services"), aligned(4))) = &_SVC_CAT(_svc_desc_, __LINE__)

typedef struct service_status {
    const char *name, *display_name, *description;
    int      state;              /* SVC_*                                       */
    bool     autostart;          /* effective setting                           */
    bool     restart_on_failure;
    uint32_t started_ms;         /* sys_uptime_ms() at start (0 if not running) */
    uint32_t uptime_ms;          /* running time so far                          */
    uint32_t tick_ms;
    int      restarts;           /* automatic restarts after failures           */
    int      icon;
    char     last_error[96];     /* "" if none                                   */
} service_status_t;

int         service_count(void);
int         service_find(const char *name);                 /* index or -1          */
int         service_status_at(int index, service_status_t *out);   /* 0 or -1        */
int         service_status(const char *name, service_status_t *out);
/* Control. Return 0 on success, -1 unknown service, -2 failed to start. */
int         service_start(const char *name);
int         service_stop(const char *name);
int         service_restart(const char *name);
bool        service_is_running(const char *name);
/* Persisted in /system/config/services. Returns 0 or -1. */
int         service_set_autostart(const char *name, bool on);
/* Report a failure from inside a service (stops it; restarts it if configured). */
void        service_fail(const char *name, const char *fmt, ...) PRINTF_LIKE(2, 3);
const char *service_state_name(int state);                  /* "running" ...       */
/* Per-service log (the syslog lines whose tag is the service name). */
int         service_log_text(const char *name, char *buf, int size);
/* Bumps on every state change or log line (poll it to refresh a UI). */
uint32_t    service_generation(void);

/* ==== system log ========================================================================= */
#define LOG_INFO  0
#define LOG_WARN  1
#define LOG_ERROR 2

typedef struct log_entry {
    uint32_t seq;          /* 1, 2, 3 ... (never reused)        */
    uint32_t time;         /* sys_time() (seconds since 1970)   */
    uint32_t uptime_ms;
    int      level;        /* LOG_*                              */
    char     tag[16];      /* subsystem or service name          */
    char     msg[128];
} log_entry_t;

void log_info(const char *tag, const char *fmt, ...) PRINTF_LIKE(2, 3);
void log_warn(const char *tag, const char *fmt, ...) PRINTF_LIKE(2, 3);
void log_error(const char *tag, const char *fmt, ...) PRINTF_LIKE(2, 3);
void log_msg(int level, const char *tag, const char *fmt, ...) PRINTF_LIKE(3, 4);
void log_vmsg(int level, const char *tag, const char *fmt, va_list ap);
/* Entries are kept in a ring (newest SYSLOG_RING). Iterate by sequence number:
   syslog_first_seq() .. syslog_last_seq(); syslog_get() false if evicted. */
#define SYSLOG_RING 512
uint32_t syslog_first_seq(void);
uint32_t syslog_last_seq(void);
bool     syslog_get(uint32_t seq, log_entry_t *out);
/* "2026-10-19 07:12:03 INFO  [notifyd] message" */
int      syslog_format(const log_entry_t *e, char *buf, int size);
const char *log_level_name(int level);   /* "INFO" "WARN" "ERROR" */

/* ==== shell commands ======================================================================
 * Any file can add a command for the Terminal / Run dialog:
 *   static int cmd_hello(int argc, char **argv, cmd_writer_t *out) {
 *       cmd_printf(out, "Hello, %s!\n", argc > 1 ? argv[1] : "world");
 *       return 0;                                       // exit status
 *   }
 *   REGISTER_COMMAND("hello", "hello [NAME] - greet someone", cmd_hello);
 * Commands run in the main loop: they must return quickly. */
typedef struct cmd_writer cmd_writer_t;
struct cmd_writer {
    void (*write)(cmd_writer_t *w, const char *s, int n);
    void *ctx;
    int flags;                 /* CMD_ANSI: the output understands ANSI colours */
    const char *cwd;           /* directory for relative paths (NULL = /home/user) */
    int cols;                  /* output width in characters for tables (0 = unknown) */
};
#define CMD_ANSI 0x1

typedef int (*cmd_fn)(int argc, char **argv, cmd_writer_t *out);
typedef struct command_desc {
    const char *name;
    const char *help;          /* "usage - description" (first line is the summary) */
    cmd_fn fn;
} command_desc_t;

#define _CMD_CAT2(a, b) a##b
#define _CMD_CAT(a, b) _CMD_CAT2(a, b)
#define REGISTER_COMMAND(name_, help_, fn_)                                              \
    static const command_desc_t _CMD_CAT(_cmd_desc_, __LINE__) = { name_, help_, fn_ };  \
    static const command_desc_t *const _CMD_CAT(_cmd_ptr_, __LINE__)                     \
        __attribute__((used, section(".commands"), aligned(4))) = &_CMD_CAT(_cmd_desc_, __LINE__)

int                   command_count(void);
const command_desc_t *command_get(int index);         /* sorted by name */
const command_desc_t *command_find(const char *name);
/* Run argv / a whole command line (quotes '...' "..." and \ escapes). Returns
   the exit status, or 127 if the command does not exist. */
int  command_exec(int argc, char **argv, cmd_writer_t *out);
int  command_run_line(const char *line, cmd_writer_t *out);
void cmd_write(cmd_writer_t *out, const char *s, int n);
void cmd_puts(cmd_writer_t *out, const char *s);
void cmd_printf(cmd_writer_t *out, const char *fmt, ...) PRINTF_LIKE(2, 3);
/* Resolve a command argument to an absolute path: "~" = /home/user, relative
   paths start at out->cwd. buf has VFS_PATH_MAX bytes. Returns buf. */
char *cmd_resolve_path(cmd_writer_t *out, const char *path, char *buf);
/* A writer that collects output into a growing heap string (free(*buf) after). */
typedef struct { cmd_writer_t w; char *buf; int len, cap; } cmd_buffer_t;
void cmd_buffer_init(cmd_buffer_t *b);

/* ==== notifyd: toast notifications ========================================================== */
typedef struct notification {
    int      id;
    uint32_t time;             /* sys_time() */
    int      icon;             /* ICON_*     */
    char     title[64];
    char     body[192];
} notification_t;

/* Show a toast (bottom-right, above the taskbar; stacks, auto-dismisses, click
   dismisses). Returns its id (> 0), 0 if notifyd is stopped (still recorded in
   the history). ui_notify() is notify(title, text, ICON_INFO). */
int  notify(const char *title, const char *body, int icon_kind);
void notify_dismiss(int id);
int  notify_history_count(void);                    /* newest first */
bool notify_history_get(int index, notification_t *out);
void notify_history_clear(void);

/* ==== screensaver ============================================================================= */
/* Starts after `seconds` without input (default 300; 0 = never). Persisted in
   /system/config/screensaver. Kernel command line: screensaver=SECONDS. */
void screensaver_set_timeout(int seconds);
int  screensaver_get_timeout(void);
void screensaver_activate(void);     /* start now (if the service runs)  */
void screensaver_dismiss(void);
bool screensaver_active(void);

/* ==== alarmd ==================================================================================== */
typedef struct alarm_info {
    int      id;
    uint32_t due;              /* sys_time() when it rings */
    char     message[96];
} alarm_info_t;

int  alarm_add_at(int hour, int minute, const char *message);   /* next HH:MM; id or -1 */
int  alarm_add_in(int minutes, const char *message);            /* id or -1             */
bool alarm_cancel(int id);
int  alarm_count(void);
bool alarm_get(int index, alarm_info_t *out);                   /* soonest first */

/* ==== autosave ================================================================================== */
/* Registered callbacks run every autosave interval (60 s) while the service
   runs, and from autosave_now() / shutdown. */
typedef void (*autosave_fn)(void *ctx);
int      autosave_register(const char *name, autosave_fn fn, void *ctx);   /* id > 0 */
void     autosave_unregister(int id);
void     autosave_now(void);
uint32_t autosave_last_run(void);    /* sys_time() of the last run, 0 = never */

/* ==== indexer: file search ======================================================================= */
typedef struct index_hit {
    char path[256];
    bool is_dir;
    int  score;                /* higher = better match */
} index_hit_t;

/* Search file and folder names (case-insensitive substring, prefix matches
   first). Returns the number of hits written. */
int  indexer_search(const char *query, index_hit_t *out, int max);
int  indexer_count(void);            /* indexed entries   */
bool indexer_ready(void);            /* first scan done   */
void indexer_rescan(void);

/* ==== sysmon: system monitor samples ============================================================= */
typedef struct sysmon_sample {
    uint32_t uptime_ms;
    uint32_t mem_used, mem_total;    /* heap bytes                   */
    int      cpu_percent;            /* busy (non-idle) time, 0..100 */
    int      events_per_sec;         /* input + window events         */
    int      fps;                    /* composited frames             */
} sysmon_sample_t;

#define SYSMON_HISTORY 120
/* Copy up to max samples, oldest first (one per second). Returns the count. */
int  sysmon_history(sysmon_sample_t *out, int max);
bool sysmon_latest(sysmon_sample_t *out);

#endif
