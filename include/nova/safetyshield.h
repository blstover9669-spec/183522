/*
 * nova/safetyshield.h - SafetyShield, the NovaOS threat scanner.
 *
 * The "safetyshield" service scans every file in the OS automatically every
 * minute (and on demand with safetyshield_scan()). Each file is checked
 * against a table of detection rules (kernel/services/safetyshield.c) and
 * gets a risk level. When a scan catches something (any level above Safe) a
 * notification appears: "SafetyShield detected a threat in your system".
 *
 * Detection rules are inactive for now, so every file is Safe. The kernel
 * command line option safetyshield_test=N (N = 1..6, the risk level) turns on
 * a built-in test rule that flags /tmp/safetyshield-test.txt, so the whole
 * detection path (log, list, notification) can be tested.
 *
 * Not included by <nova.h>: #include <nova/safetyshield.h>.
 */
#ifndef NOVA_SAFETYSHIELD_H
#define NOVA_SAFETYSHIELD_H

#include <nova/types.h>
#include <nova/vfs.h>

/* Risk levels, mildest first. */
enum {
    SS_RISK_SAFE = 0,
    SS_RISK_RISKY,
    SS_RISK_MODERATE,
    SS_RISK_HIGH,
    SS_RISK_TROJAN,
    SS_RISK_DANGEROUS,
    SS_RISK_SEVERE,
    SS_RISK_COUNT
};

const char *safetyshield_risk_name(int risk);         /* "Safe", "Risky" ...            */
const char *safetyshield_risk_description(int risk);  /* full description with examples */
const char *safetyshield_risk_summary(int risk);      /* first sentence of it            */
uint32_t    safetyshield_risk_color(int risk);

typedef struct safetyshield_status {
    bool     running;          /* the service is running                               */
    bool     scanning;         /* a scan is in progress                                */
    bool     manual;           /* the current / last scan was started by the user      */
    uint32_t scan_id;          /* increments every time a scan starts (0 = never)      */
    int      scanned;          /* files scanned by the current / last scan             */
    int      total;            /* files to scan in the current / last scan             */
    int      threats;          /* threats found by the current / last scan             */
    uint32_t last_scan_time;   /* sys_time() when the last scan finished, 0 = never    */
    uint32_t next_scan_ms;     /* sys_uptime_ms() of the next automatic scan           */
} safetyshield_status_t;

void     safetyshield_status(safetyshield_status_t *out);
/* Start a scan now: 0 = started, 1 = already scanning, -1 = service not running. */
int      safetyshield_scan(void);
/* Number of files in the whole OS (hidden and system files included). */
int      safetyshield_total_files(void);
/* Bumps on every change (scan progress, results) - poll it to refresh a UI. */
uint32_t safetyshield_generation(void);

/* Files of the most recent scan, most recently scanned first. */
typedef struct safetyshield_entry {
    char path[VFS_PATH_MAX];
    int  risk;                 /* SS_RISK_*                         */
    char rule[32];             /* name of the rule that matched, "" */
} safetyshield_entry_t;

int  safetyshield_recent_count(void);
bool safetyshield_recent_get(int index, safetyshield_entry_t *out);

#endif
