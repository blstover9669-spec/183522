/*
 * indexer.c - keeps an index of every file and folder name in the VFS so the
 * Start menu, the Run dialog and the `locate` command can find files.
 *
 * Scanning never blocks: the tree is walked a bounded number of directories
 * per tick into a new index, which replaces the old one when complete. A
 * rescan starts when vfs_generation() changes (at most every few seconds).
 */
#include "services/svc_internal.h"

#define MAX_ENTRIES   8192
#define DIRS_PER_TICK 24
#define RESCAN_MS     3000

typedef struct {
    char *path;            /* heap string                       */
    uint16_t name_off;     /* offset of the basename in path    */
    bool is_dir;
} ient_t;

typedef struct {
    ient_t *e;
    int n, cap;
} index_t;

static index_t cur, bld;           /* current index / being built */
static char **stack;               /* directories still to scan    */
static int nstack, capstack;
static bool scanning, ready, running;
static uint32_t scan_gen, scan_started_ms, last_scan_ms;
static bool truncated;

static void index_free(index_t *ix) {
    for (int i = 0; i < ix->n; i++) kfree(ix->e[i].path);
    kfree(ix->e);
    memset(ix, 0, sizeof *ix);
}

static void stack_clear(void) {
    for (int i = 0; i < nstack; i++) kfree(stack[i]);
    nstack = 0;
}

static bool stack_push(const char *path) {
    if (nstack == capstack) {
        int nc = capstack ? capstack * 2 : 64;
        char **ns = krealloc(stack, sizeof *ns * (size_t)nc);
        if (!ns) return false;
        stack = ns;
        capstack = nc;
    }
    char *p = strdup(path);
    if (!p) return false;
    stack[nstack++] = p;
    return true;
}

static void bld_add(const char *path, bool is_dir) {
    if (bld.n >= MAX_ENTRIES) { truncated = true; return; }
    if (bld.n == bld.cap) {
        int nc = bld.cap ? bld.cap * 2 : 256;
        ient_t *ne = krealloc(bld.e, sizeof *ne * (size_t)nc);
        if (!ne) return;
        bld.e = ne;
        bld.cap = nc;
    }
    char *p = strdup(path);
    if (!p) return;
    const char *base = vfs_basename(p);
    bld.e[bld.n].path = p;
    bld.e[bld.n].name_off = (uint16_t)(base - p);
    bld.e[bld.n].is_dir = is_dir;
    bld.n++;
}

static void start_scan(void) {
    index_free(&bld);
    stack_clear();
    truncated = false;
    scan_gen = vfs_generation();
    scan_started_ms = last_scan_ms = sys_uptime_ms();
    scanning = stack_push("/");
}

static void scan_step(void) {
    for (int d = 0; d < DIRS_PER_TICK && nstack; d++) {
        char *dir = stack[--nstack];
        int n = vfs_list(dir, NULL, 0);
        if (n > 0) {
            vfs_dirent_t *ents = kcalloc((size_t)n, sizeof *ents);
            if (ents) {
                int got = MIN(vfs_list(dir, ents, n), n);
                for (int i = 0; i < got; i++) {
                    char path[VFS_PATH_MAX];
                    vfs_join(dir, ents[i].name, path);
                    bld_add(path, ents[i].is_dir);
                    if (ents[i].is_dir) stack_push(path);
                }
                kfree(ents);
            }
        }
        kfree(dir);
    }
    if (nstack) return;
    /* done: swap in the new index */
    scanning = false;
    index_free(&cur);
    cur = bld;
    memset(&bld, 0, sizeof bld);
    bool first = !ready;
    ready = true;
    if (first || truncated)
        log_info("indexer", "indexed %d files and folders in %u ms%s", cur.n, sys_uptime_ms() - scan_started_ms,
                 truncated ? " (limit reached)" : "");
}

static void indexer_tick(void) {
    if (scanning) { scan_step(); return; }
    if (vfs_generation() != scan_gen && sys_uptime_ms() - last_scan_ms >= RESCAN_MS) start_scan();
}

/* ---- queries ------------------------------------------------------------------------------- */
bool indexer_ready(void) { return ready; }
int  indexer_count(void) { return cur.n; }

void indexer_rescan(void) {
    if (!running) return;
    start_scan();
}

static bool word_start(const char *name, const char *hit) {
    if (hit == name) return true;
    char p = hit[-1];
    return p == ' ' || p == '_' || p == '-' || p == '.' || (islower((unsigned char)p) && isupper((unsigned char)hit[0]));
}

static int score_entry(const ient_t *e, const char *q, size_t ql) {
    const char *name = e->path + e->name_off;
    int s = 0;
    if (!strcasecmp(name, q)) s = 1000;
    else if (!strncasecmp(name, q, ql)) s = 700;
    else {
        const char *h = strcasestr(name, q);
        if (h) s = word_start(name, h) ? 450 : 260;
        else if (strcasestr(e->path, q)) s = 60;
        else return 0;
    }
    int depth = 0;
    for (const char *p = e->path; *p; p++) if (*p == '/') depth++;
    s -= depth * 6;
    if (!strncmp(e->path, "/home/", 6)) s += 60;
    else if (!strncmp(e->path, "/system/", 8)) s -= 80;
    if (e->is_dir) s += 10;
    return MAX(s, 1);
}

int indexer_search(const char *query, index_hit_t *out, int max) {
    if (!query || !out || max <= 0) return 0;
    while (*query == ' ') query++;
    size_t ql = strlen(query);
    if (!ql) return 0;
    int n = 0;
    for (int i = 0; i < cur.n; i++) {
        const ient_t *e = &cur.e[i];
        int s = score_entry(e, query, ql);
        if (s <= 0) continue;
        /* insertion into the sorted top-`max` list */
        int pos = n;
        while (pos > 0 && (out[pos - 1].score < s ||
                           (out[pos - 1].score == s && strcasecmp(out[pos - 1].path, e->path) > 0))) pos--;
        if (pos >= max) continue;
        int last = MIN(n, max - 1);
        for (int k = last; k > pos; k--) out[k] = out[k - 1];
        strlcpy(out[pos].path, e->path, sizeof out[pos].path);
        out[pos].is_dir = e->is_dir;
        out[pos].score = s;
        if (n < max) n++;
    }
    return n;
}

/* ---- service --------------------------------------------------------------------------------- */
static int indexer_start(void) {
    running = true;
    start_scan();
    return 0;
}

static void indexer_stop(void) {
    running = false;
    scanning = false;
    ready = false;
    stack_clear();
    index_free(&bld);
    index_free(&cur);
}

REGISTER_SERVICE(.name = "indexer", .display_name = "File Indexer",
                 .description = "Indexes file names so the Start menu and Run dialog can find files",
                 .autostart = true, .restart_on_failure = true,
                 .start = indexer_start, .stop = indexer_stop, .tick = indexer_tick, .tick_ms = 100,
                 .icon = ICON_SEARCH);

static int cmd_locate(int argc, char **argv, cmd_writer_t *out) {
    if (argc < 2) { cmd_puts(out, "usage: locate NAME - find files and folders by name\n"); return 2; }
    if (!running) { cmd_puts(out, "locate: the indexer service is not running (service start indexer)\n"); return 1; }
    char q[128] = "";
    for (int i = 1; i < argc; i++) { if (i > 1) strlcat(q, " ", sizeof q); strlcat(q, argv[i], sizeof q); }
    index_hit_t *hits = kcalloc(50, sizeof *hits);
    if (!hits) return 1;
    int n = indexer_search(q, hits, 50);
    for (int i = 0; i < n; i++) cmd_printf(out, "%s%s\n", hits[i].path, hits[i].is_dir ? "/" : "");
    if (!n) cmd_printf(out, "locate: nothing matches '%s'\n", q);
    kfree(hits);
    return n ? 0 : 1;
}
REGISTER_COMMAND("locate", "locate NAME - find files and folders by name (uses the file index)", cmd_locate);
