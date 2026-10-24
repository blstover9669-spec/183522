/*
 * safetyshield.c - SafetyShield, the NovaOS threat scanner (see nova/safetyshield.h).
 *
 * A scan walks the whole VFS from "/" and checks every file: it reads the
 * file in chunks (hashing all of it and keeping the first bytes), then runs
 * the detection rules below on it. Nothing blocks: a step timer does a few
 * milliseconds of work at a time, and the scan is paced over about two
 * seconds so the progress bar in Settings visibly fills and a background
 * scan never slows the desktop down.
 *
 * Scans start automatically every minute, or from safetyshield_scan() (the
 * Scan button in Settings > SafetyShield). When a scan catches something
 * (any risk above Safe) the notification "SafetyShield detected a threat in
 * your system" appears - once per scan, and for automatic scans only when a
 * threat is new (not already found by the previous scan).
 *
 * ADDING DETECTION RULES: write a function that looks at an ss_file_t
 * (path, name, extension, size, first bytes, hash of the whole content) and
 * returns the risk it finds (SS_RISK_SAFE = no match), then add it to
 * rules[]. The scanner takes the highest risk any rule reports. For now the
 * only rule is the test rule, which is off unless the kernel command line
 * has safetyshield_test=N (N = risk level 1..6): it creates
 * /tmp/safetyshield-test.txt and flags it, and the first scan runs after
 * 5 s instead of a minute.
 */
#include "services/svc_internal.h"
#include <nova/safetyshield.h>

#define SCAN_INTERVAL_MS 60000     /* automatic scan every minute               */
#define TEST_FIRST_MS    5000      /* first scan with safetyshield_test=N        */
#define STEP_MS          20        /* step timer while scanning                  */
#define STEP_BUDGET_MS   3         /* work per step                              */
#define MIN_SCAN_MS      2000      /* pace: a scan takes at least this long      */
#define CHUNK            16384     /* bytes read per vfs_read                    */
#define HEAD_MAX         4096      /* first bytes kept for the rules             */
#define MAX_RECENT       4096      /* entries kept in the recent list            */
#define MAX_THREATS      256       /* threats remembered between scans           */

#define TEST_FILE        "/tmp/safetyshield-test.txt"
#define TEST_SIGNATURE   "NOVAOS-SAFETYSHIELD-TEST-SIGNATURE"

/* ==== risk levels ======================================================================= */
static const char *const risk_names[SS_RISK_COUNT] = {
    "Safe", "Risky", "Moderate", "High", "Trojan", "Dangerous", "Severe",
};

static const char *const risk_summaries[SS_RISK_COUNT] = {
    "No threat found.",
    "Not harmful by itself, but comes from an unknown source or could be misused.",
    "Unwanted software that hurts privacy or performance but doesn't directly steal data or damage files.",
    "Programs or code designed to steal sensitive information, damage files, or heavily compromise system security.",
    "Malware disguised as a useful or legitimate program, which tricks you into running it and then secretly "
    "does harm in the background.",
    "Actively harmful malware that damages or deletes files or takes over parts of the system, but doesn't "
    "spread on its own.",
    "Widespread, highly dangerous, or automated malware that poses an immediate threat to your system or "
    "personal data.",
};

static const char *const risk_descriptions[SS_RISK_COUNT] = {
    "No threat found. The file is normal and does nothing harmful. Examples: documents, pictures, notes, "
    "NovaOS system files and apps.",
    "Not harmful by itself, but comes from an unknown source or could be misused. Examples: unknown programs "
    "or scripts, files with misleading names or double extensions (photo.jpg.exe), tools that change system "
    "settings.",
    "Unwanted software that hurts privacy or performance but doesn't directly steal data or damage files. "
    "Examples: adware, browser hijackers, potentially unwanted programs (PUPs), tracking tools.",
    "Programs or code designed to steal sensitive information, damage files, or heavily compromise system "
    "security. Examples: Keyloggers, credential-theft tools, spyware, banking trojans, or tools that attempt "
    "to turn off your antivirus features.",
    "Malware disguised as a useful or legitimate program, which tricks you into running it and then secretly "
    "does harm in the background. Examples: fake game cracks, fake installers or updates, email attachments "
    "pretending to be documents, remote-access trojans (RATs).",
    "Actively harmful malware that damages or deletes files or takes over parts of the system, but doesn't "
    "spread on its own. Examples: file-deleting viruses, droppers that install more malware, crypto-miners "
    "that hijack the CPU, scripts that sabotage system settings.",
    "Widespread, highly dangerous, or automated malware that poses an immediate threat to your system or "
    "personal data. Examples: Active ransomware, backdoor trojans, rootkits, or worm viruses that can spread "
    "through networks without any user input.",
};

static const uint32_t risk_colors[SS_RISK_COUNT] = {
    RGB(30, 158, 88), RGB(212, 160, 16), RGB(236, 128, 28), RGB(226, 78, 46),
    RGB(204, 40, 84), RGB(168, 28, 40), RGB(118, 26, 128),
};

static int risk_ok(int r) { return r >= 0 && r < SS_RISK_COUNT; }
const char *safetyshield_risk_name(int r) { return risk_ok(r) ? risk_names[r] : "?"; }
const char *safetyshield_risk_description(int r) { return risk_ok(r) ? risk_descriptions[r] : ""; }
const char *safetyshield_risk_summary(int r) { return risk_ok(r) ? risk_summaries[r] : ""; }
uint32_t safetyshield_risk_color(int r) { return risk_ok(r) ? risk_colors[r] : RGB(128, 128, 128); }

/* ==== detection rules ===================================================================== */
typedef struct ss_file {
    const char    *path;       /* "/home/user/Downloads/photo.jpg.exe"           */
    const char    *name;       /* "photo.jpg.exe"                                */
    const char    *ext;        /* "exe" ("" if none)                             */
    uint32_t       size;       /* bytes                                          */
    const uint8_t *head;       /* the first head_len bytes of the file           */
    uint32_t       head_len;
    uint32_t       hash;       /* FNV-1a (32-bit) of the whole content            */
} ss_file_t;

typedef struct ss_rule {
    const char *name;                       /* shown in the log and the recent list */
    int (*check)(const ss_file_t *f);       /* SS_RISK_* found, SS_RISK_SAFE = no match */
} ss_rule_t;

static int test_risk;      /* safetyshield_test=N on the command line (0 = off) */

static bool head_contains(const ss_file_t *f, const char *needle) {
    uint32_t n = (uint32_t)strlen(needle);
    if (!n || f->head_len < n) return false;
    for (uint32_t i = 0; i + n <= f->head_len; i++)
        if (f->head[i] == (uint8_t)needle[0] && !memcmp(f->head + i, needle, n)) return true;
    return false;
}

/* Test rule (only with safetyshield_test=N): the SafetyShield test signature. */
static int rule_test_signature(const ss_file_t *f) {
    return test_risk > 0 && head_contains(f, TEST_SIGNATURE) ? test_risk : SS_RISK_SAFE;
}

static const ss_rule_t rules[] = {
    { "Test signature", rule_test_signature },
    /* Detection rules are inactive for now (every file is Safe). Add them here, e.g.
       { "Double extension", rule_double_extension },   (photo.jpg.exe -> SS_RISK_RISKY) */
};
#define NRULES ((int)(sizeof rules / sizeof rules[0]))

/* ==== state ================================================================================== */
typedef struct { char *path; uint8_t risk, rule; } rec_t;   /* rule: index + 1, 0 = none */

static bool running, scanning, manual, notified;
static uint32_t scan_id, gen;
static int scanned, total, threats;
static uint32_t last_scan_time, next_scan_ms, scan_start_ms;
static int step_timer;

static char **dirs;  static int ndirs, capdirs;      /* directories still to walk      */
static char **queue; static int nqueue, capqueue;    /* files of the walked directories */

/* the file being read */
static char *cur_path;
static int cur_fd = -1;
static uint32_t cur_size, cur_hash, cur_head_len;
static uint8_t *cur_head, *chunk;

static rec_t recent[MAX_RECENT];
static int rec_head, rec_n;

static uint32_t prev_threats[MAX_THREATS], cur_threats[MAX_THREATS];
static int nprev, ncur;

/* ==== small helpers ========================================================================== */
static bool push(char ***arr, int *n, int *cap, const char *s) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 64;
        char **na = krealloc(*arr, sizeof *na * (size_t)nc);
        if (!na) return false;
        *arr = na;
        *cap = nc;
    }
    char *p = strdup(s);
    if (!p) return false;
    (*arr)[(*n)++] = p;
    return true;
}

static void clear_list(char **arr, int *n) {
    for (int i = 0; i < *n; i++) kfree(arr[i]);
    *n = 0;
}

static uint32_t fnv(uint32_t h, const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint32_t threat_key(const char *path, int risk) {
    return fnv(2166136261u ^ (uint32_t)risk, (const uint8_t *)path, (uint32_t)strlen(path));
}

static void recent_clear(void) {
    for (int i = 0; i < MAX_RECENT; i++) { kfree(recent[i].path); recent[i].path = NULL; }
    rec_head = rec_n = 0;
}

static void recent_add(const char *path, int risk, int rule) {
    rec_t *r = &recent[rec_head];
    kfree(r->path);
    r->path = strdup(path);
    r->risk = (uint8_t)risk;
    r->rule = (uint8_t)rule;
    rec_head = (rec_head + 1) % MAX_RECENT;
    if (rec_n < MAX_RECENT) rec_n++;
}

int safetyshield_recent_count(void) { return rec_n; }

bool safetyshield_recent_get(int index, safetyshield_entry_t *out) {
    if (index < 0 || index >= rec_n || !out) return false;
    const rec_t *r = &recent[((rec_head - 1 - index) % MAX_RECENT + MAX_RECENT) % MAX_RECENT];
    strlcpy(out->path, r->path ? r->path : "", sizeof out->path);
    out->risk = r->risk;
    strlcpy(out->rule, r->rule ? rules[r->rule - 1].name : "", sizeof out->rule);
    return true;
}

uint32_t safetyshield_generation(void) { return gen; }

/* ==== counting every file ===================================================================== */
static int count_files(const char *dir, int depth) {
    int n = vfs_list(dir, NULL, 0);
    if (n <= 0) return 0;
    vfs_dirent_t *e = kcalloc((size_t)n, sizeof *e);
    if (!e) return 0;
    int got = MIN(vfs_list(dir, e, n), n), files = 0;
    for (int i = 0; i < got; i++) {
        if (!e[i].is_dir) { files++; continue; }
        if (depth < 64) {
            char path[VFS_PATH_MAX];
            vfs_join(dir, e[i].name, path);
            files += count_files(path, depth + 1);
        }
    }
    kfree(e);
    return files;
}

int safetyshield_total_files(void) {
    static uint32_t cached_gen = 0xFFFFFFFFu;
    static int cached;
    if (vfs_generation() != cached_gen) {
        cached = count_files("/", 0);
        cached_gen = vfs_generation();
    }
    return cached;
}

/* ==== scanning ================================================================================== */
static void close_current(void) {
    if (cur_fd >= 0) vfs_close(cur_fd);
    cur_fd = -1;
    kfree(cur_path);
    cur_path = NULL;
}

static void reset_walk(void) {
    close_current();
    clear_list(dirs, &ndirs);
    clear_list(queue, &nqueue);
}

static void report_threat(const char *path, int risk, int rule) {
    threats++;
    uint32_t key = threat_key(path, risk);
    if (ncur < MAX_THREATS) cur_threats[ncur++] = key;
    bool known = false;
    for (int i = 0; i < nprev; i++) if (prev_threats[i] == key) { known = true; break; }
    log_warn("safetyshield", "%s: %s (rule: %s)", risk_names[risk], path, rule ? rules[rule - 1].name : "-");
    if (!notified && (manual || !known)) {
        notified = true;
        notify("SafetyShield", "SafetyShield detected a threat in your system", ICON_SHIELD);
    }
}

/* All bytes read: run the rules. */
static void finish_file(void) {
    ss_file_t f;
    f.path = cur_path;
    f.name = vfs_basename(cur_path);
    f.ext = vfs_extension(cur_path);
    f.size = cur_size;
    f.head = cur_head;
    f.head_len = cur_head_len;
    f.hash = cur_hash;
    int risk = SS_RISK_SAFE, rule = 0;
    for (int i = 0; i < NRULES; i++) {
        int r = rules[i].check(&f);
        if (r > risk && r < SS_RISK_COUNT) { risk = r; rule = i + 1; }
    }
    recent_add(cur_path, risk, rule);
    if (risk > SS_RISK_SAFE) report_threat(cur_path, risk, rule);
    scanned++;
    close_current();
}

static void finish_scan(void) {
    scanning = false;
    if (step_timer) { timer_cancel(step_timer); step_timer = 0; }
    reset_walk();
    total = scanned;
    last_scan_time = sys_time();
    memcpy(prev_threats, cur_threats, sizeof(uint32_t) * (size_t)ncur);
    nprev = ncur;
    log_info("safetyshield", "scan finished: %d files in %u ms, %d threat%s found", scanned,
             sys_uptime_ms() - scan_start_ms, threats, threats == 1 ? "" : "s");
    gen++;
}

/* Take the next file: from the queue, else walk the next directory. false = nothing left. */
static bool next_file(void) {
    while (!nqueue) {
        if (!ndirs) return false;
        char *dir = dirs[--ndirs];
        int n = vfs_list(dir, NULL, 0);
        if (n > 0) {
            vfs_dirent_t *e = kcalloc((size_t)n, sizeof *e);
            if (e) {
                int got = MIN(vfs_list(dir, e, n), n);
                /* queue files in reverse so they are scanned in name order; subdirectories after */
                for (int i = got - 1; i >= 0; i--) {
                    char path[VFS_PATH_MAX];
                    vfs_join(dir, e[i].name, path);
                    if (e[i].is_dir) push(&dirs, &ndirs, &capdirs, path);
                    else push(&queue, &nqueue, &capqueue, path);
                }
                kfree(e);
            }
        }
        kfree(dir);
    }
    cur_path = queue[--nqueue];
    cur_fd = vfs_open(cur_path, VFS_O_READ);
    cur_size = 0;
    cur_hash = 2166136261u;
    cur_head_len = 0;
    vfs_stat_t st;
    if (vfs_stat(cur_path, &st) == 0) cur_size = st.size;
    return true;
}

static void scan_step(void *ctx) {
    (void)ctx;
    if (!scanning) return;
    uint32_t t0 = g_ticks;
    /* pace: by now at most total * elapsed / MIN_SCAN_MS files (so a scan takes about 2 s or more) */
    int allowed = (int)((int64_t)total * (sys_uptime_ms() - scan_start_ms + STEP_MS) / MIN_SCAN_MS);
    while (g_ticks - t0 < STEP_BUDGET_MS) {
        if (!cur_path) {
            if (scanned >= MAX(allowed, 1)) break;
            if (!next_file()) { finish_scan(); return; }
        }
        if (cur_fd < 0) {                    /* vanished meanwhile: counts as scanned, not listed */
            scanned++;
            close_current();
            continue;
        }
        int r = vfs_read(cur_fd, chunk, CHUNK);
        if (r > 0) {
            cur_hash = fnv(cur_hash, chunk, (uint32_t)r);
            if (cur_head_len < HEAD_MAX) {
                uint32_t k = MIN((uint32_t)r, HEAD_MAX - cur_head_len);
                memcpy(cur_head + cur_head_len, chunk, k);
                cur_head_len += k;
            }
            continue;
        }
        finish_file();
    }
    if (scanned > total) total = scanned;     /* files added during the scan */
    gen++;
}

static void start_scan(bool by_user) {
    reset_walk();
    recent_clear();
    scanning = true;
    manual = by_user;
    notified = false;
    scan_id++;
    scanned = threats = 0;
    ncur = 0;
    total = safetyshield_total_files();
    scan_start_ms = sys_uptime_ms();
    next_scan_ms = scan_start_ms + SCAN_INTERVAL_MS;
    push(&dirs, &ndirs, &capdirs, "/");
    if (!step_timer) step_timer = timer_add(STEP_MS, scan_step, NULL, true);
    log_info("safetyshield", "%s scan started: %d files", by_user ? "manual" : "automatic", total);
    gen++;
}

int safetyshield_scan(void) {
    if (!running) return -1;
    if (scanning) return 1;
    start_scan(true);
    return 0;
}

void safetyshield_status(safetyshield_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->running = running;
    out->scanning = scanning;
    out->manual = manual;
    out->scan_id = scan_id;
    out->scanned = scanned;
    out->total = MAX(total, scanned);
    out->threats = threats;
    out->last_scan_time = last_scan_time;
    out->next_scan_ms = next_scan_ms;
}

/* ==== service ==================================================================================== */
static int ss_start(void) {
    if (!cur_head) cur_head = kmalloc(HEAD_MAX);
    if (!chunk) chunk = kmalloc(CHUNK);
    if (!cur_head || !chunk) return -1;
    const char *t = sys_cmdline_get("safetyshield_test");
    test_risk = 0;
    if (t && *t) {
        test_risk = atoi(t);
        if (test_risk <= SS_RISK_SAFE || test_risk >= SS_RISK_COUNT) test_risk = SS_RISK_TROJAN;
        static const char body[] = "NovaOS SafetyShield test file - harmless.\n" TEST_SIGNATURE "\n";
        vfs_mkdirs("/tmp");
        vfs_write_file(TEST_FILE, body, (uint32_t)strlen(body));
    }
    running = true;
    next_scan_ms = sys_uptime_ms() + (test_risk ? TEST_FIRST_MS : SCAN_INTERVAL_MS);
    log_info("safetyshield", "protection on, %d rule%s, automatic scan every %d s%s", NRULES, NRULES == 1 ? "" : "s",
             SCAN_INTERVAL_MS / 1000, test_risk ? " (test rule active)" : "");
    gen++;
    return 0;
}

static void ss_stop(void) {
    if (step_timer) { timer_cancel(step_timer); step_timer = 0; }
    reset_walk();
    scanning = false;
    running = false;
    gen++;
}

static void ss_tick(void) {
    if (!scanning && (int32_t)(sys_uptime_ms() - next_scan_ms) >= 0) start_scan(false);
}

REGISTER_SERVICE(.name = "safetyshield", .display_name = "SafetyShield",
                 .description = "Scans every file for threats automatically every minute",
                 .autostart = true, .restart_on_failure = true,
                 .start = ss_start, .stop = ss_stop, .tick = ss_tick, .tick_ms = 1000,
                 .icon = ICON_SHIELD);
