/*
 * logd.c - writes the system log to /system/log/syslog.
 *
 * New entries are appended in batches every few seconds (and on stop /
 * shutdown) so the filesystem does not change on every log line. The file is
 * kept below LOG_MAX bytes by dropping its oldest lines.
 */
#include "services/svc_internal.h"

#define LOG_DIR  "/system/log"
#define LOG_FILE "/system/log/syslog"
#define LOG_MAX  (96 * 1024)
#define LOG_KEEP (64 * 1024)

static uint32_t written_seq;

static void trim(void) {
    vfs_stat_t st;
    if (vfs_stat(LOG_FILE, &st) != 0 || st.size <= LOG_MAX) return;
    uint32_t size;
    char *data = vfs_read_file(LOG_FILE, &size);
    if (!data) return;
    uint32_t start = size > LOG_KEEP ? size - LOG_KEEP : 0;
    while (start < size && data[start] != '\n') start++;
    if (start < size) start++;
    vfs_write_file(LOG_FILE, data + start, size - start);
    free(data);
}

static void flush(void) {
    uint32_t last = syslog_last_seq();
    if (written_seq >= last) return;
    uint32_t first = MAX(written_seq + 1, syslog_first_seq());
    int cap = 16384, len = 0;
    char *buf = kmalloc((size_t)cap);
    if (!buf) return;
    for (uint32_t seq = first; seq <= last; seq++) {
        log_entry_t e;
        if (!syslog_get(seq, &e)) continue;
        if (cap - len < 200) {
            vfs_append_file(LOG_FILE, buf, (uint32_t)len);
            len = 0;
        }
        len += syslog_format(&e, buf + len, cap - len - 1);
        buf[len++] = '\n';
    }
    if (len) vfs_append_file(LOG_FILE, buf, (uint32_t)len);
    kfree(buf);
    written_seq = last;
    trim();
}

static int logd_start(void) {
    if (vfs_mkdirs(LOG_DIR) < 0 && !vfs_is_dir(LOG_DIR)) {
        service_fail("logd", "cannot create %s", LOG_DIR);
        return -1;
    }
    if (!vfs_exists(LOG_FILE)) vfs_create(LOG_FILE);
    /* first start: write everything still in the ring, including the boot messages */
    log_info("logd", "writing the system log to %s", LOG_FILE);
    flush();
    return 0;
}

static void logd_stop(void) { flush(); }
static void logd_tick(void) { flush(); }

REGISTER_SERVICE(.name = "logd", .display_name = "System Log",
                 .description = "Writes the system log to /system/log/syslog (size-limited)",
                 .autostart = true, .restart_on_failure = true,
                 .start = logd_start, .stop = logd_stop, .tick = logd_tick, .tick_ms = 5000,
                 .icon = ICON_FILE_LOG);
