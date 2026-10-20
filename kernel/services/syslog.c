/* syslog.c - the system log: tagged, timestamped entries in a ring, mirrored to serial. */
#include "services/svc_internal.h"

static log_entry_t ring[SYSLOG_RING];
static uint32_t next_seq = 1;

const char *log_level_name(int level) {
    return level == LOG_ERROR ? "ERROR" : level == LOG_WARN ? "WARN" : "INFO";
}

void log_vmsg(int level, const char *tag, const char *fmt, va_list ap) {
    log_entry_t *e = &ring[(next_seq - 1) % SYSLOG_RING];
    memset(e, 0, sizeof *e);
    e->seq = next_seq++;
    e->time = sys_time();
    e->uptime_ms = sys_uptime_ms();
    e->level = CLAMP(level, LOG_INFO, LOG_ERROR);
    strlcpy(e->tag, tag && *tag ? tag : "system", sizeof e->tag);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    if (e->level == LOG_INFO) klog("[%s] %s", e->tag, e->msg);
    else klog("[%s] %s: %s", e->tag, log_level_name(e->level), e->msg);
    svc_note_log(e);
}

void log_msg(int level, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vmsg(level, tag, fmt, ap);
    va_end(ap);
}

void log_info(const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vmsg(LOG_INFO, tag, fmt, ap);
    va_end(ap);
}

void log_warn(const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vmsg(LOG_WARN, tag, fmt, ap);
    va_end(ap);
}

void log_error(const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vmsg(LOG_ERROR, tag, fmt, ap);
    va_end(ap);
}

uint32_t syslog_last_seq(void) { return next_seq - 1; }
uint32_t syslog_first_seq(void) { return next_seq > SYSLOG_RING ? next_seq - SYSLOG_RING : 1; }

bool syslog_get(uint32_t seq, log_entry_t *out) {
    if (seq < syslog_first_seq() || seq >= next_seq || !seq) return false;
    *out = ring[(seq - 1) % SYSLOG_RING];
    return true;
}

int syslog_format(const log_entry_t *e, char *buf, int size) {
    datetime_t dt;
    sys_time_to_datetime(e->time, &dt);
    return snprintf(buf, (size_t)size, "%04d-%02d-%02d %02d:%02d:%02d %-5s [%s] %s", dt.year, dt.month, dt.day,
                    dt.hour, dt.minute, dt.second, log_level_name(e->level), e->tag, e->msg);
}
