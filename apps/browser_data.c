/*
 * browser_data.c - the Web Browser's settings, history and bookmarks, and
 * the pages it makes itself (home page, history, error pages, page source).
 *
 *   /system/config/browser.cfg      history=1|0
 *   /home/user/.browser/history     time<TAB>url<TAB>title per line, oldest first
 *   /home/user/.browser/bookmarks   url<TAB>title per line
 *   /home/user/.browser/downloads   id state time got total name path url reason (tabs)
 *
 * Titles are kept in the page text charset (Windows-1252).
 */
#include "browser.h"
#include <nova/safetyshield.h>

/* SafetyShield's per-file checks may not be built in (yet): weak references */
void safetyshield_check_file(const char *path) __attribute__((weak));
int  safetyshield_file_risk(const char *path) __attribute__((weak));

#define CFG_PATH  "/system/config/browser.cfg"
#define DATA_DIR  "/home/user/.browser"
#define HIST_PATH DATA_DIR "/history"
#define BM_PATH   DATA_DIR "/bookmarks"
#define DL_PATH   DATA_DIR "/downloads"
#define HIST_MAX  500
#define BM_MAX    64
#define DL_MAX    100

bool        bdata_history_on = true;
bookmark_t *bdata_bm;
int         bdata_nbm;
hitem_t    *bdata_hist;
int         bdata_nhist;
bdl_t      *bdata_dl;
int         bdata_ndl;
uint32_t    bdata_dl_gen = 1;
static int  dl_next_id = 1;
static bool loaded;

/* ---- small string builder ---------------------------------------------------- */
static void sb_reserve(char **buf, size_t *len, size_t *cap, size_t more) {
    if (*len + more + 1 <= *cap) return;
    size_t nc = *cap ? *cap : 1024;
    while (nc < *len + more + 1) nc *= 2;
    char *nb = realloc(*buf, nc);
    if (!nb) return;
    *buf = nb;
    *cap = nc;
}

void bhtml_add(char **buf, size_t *len, size_t *cap, const char *s) {
    size_t n = strlen(s);
    sb_reserve(buf, len, cap, n);
    if (*len + n + 1 > *cap) return;
    memcpy(*buf + *len, s, n + 1);
    *len += n;
}

void bhtml_escape(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    sb_reserve(buf, len, cap, n + n / 8 + 16);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        const char *rep = c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : c == '"' ? "&quot;" : NULL;
        if (rep) { bhtml_add(buf, len, cap, rep); continue; }
        if (c == 0) c = ' ';
        sb_reserve(buf, len, cap, 1);
        if (*len + 2 > *cap) return;
        (*buf)[(*len)++] = c;
        (*buf)[*len] = 0;
    }
}

/* ---- files ---------------------------------------------------------------- */
static void ensure_dir(void) {
    if (!vfs_is_dir(DATA_DIR)) vfs_mkdirs(DATA_DIR);
}

static void save_history(void) {
    ensure_dir();
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char line[64];
    for (int i = 0; i < bdata_nhist; i++) {
        snprintf(line, sizeof line, "%u\t", (unsigned)bdata_hist[i].time);
        bhtml_add(&buf, &len, &cap, line);
        bhtml_add(&buf, &len, &cap, bdata_hist[i].url);
        bhtml_add(&buf, &len, &cap, "\t");
        bhtml_add(&buf, &len, &cap, bdata_hist[i].title);
        bhtml_add(&buf, &len, &cap, "\n");
    }
    vfs_write_file(HIST_PATH, buf ? buf : "", len);
    free(buf);
}

static void save_bookmarks(void) {
    ensure_dir();
    char *buf = NULL;
    size_t len = 0, cap = 0;
    for (int i = 0; i < bdata_nbm; i++) {
        bhtml_add(&buf, &len, &cap, bdata_bm[i].url);
        bhtml_add(&buf, &len, &cap, "\t");
        bhtml_add(&buf, &len, &cap, bdata_bm[i].title);
        bhtml_add(&buf, &len, &cap, "\n");
    }
    vfs_write_file(BM_PATH, buf ? buf : "", len);
    free(buf);
}

static void save_downloads(void) {
    ensure_dir();
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char line[96];
    for (int i = 0; i < bdata_ndl; i++) {
        bdl_t *d = &bdata_dl[i];
        snprintf(line, sizeof line, "%d\t%d\t%u\t%u\t%u\t", d->id, d->state, (unsigned)d->time, (unsigned)d->got, (unsigned)d->total);
        bhtml_add(&buf, &len, &cap, line);
        const char *f[4] = { d->name, d->path, d->url, d->reason };
        for (int k = 0; k < 4; k++) {
            /* (tabs / newlines cannot be in the fields) */
            char tmp[520];
            strlcpy(tmp, f[k], sizeof tmp);
            for (char *c = tmp; *c; c++) if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
            bhtml_add(&buf, &len, &cap, tmp);
            bhtml_add(&buf, &len, &cap, k < 3 ? "\t" : "\n");
        }
    }
    vfs_write_file(DL_PATH, buf ? buf : "", len);
    free(buf);
}

static void load_downloads(char *text) {
    char *save = NULL;
    for (char *ln = strtok_r(text, "\n", &save); ln && bdata_ndl < DL_MAX; ln = strtok_r(NULL, "\n", &save)) {
        char *f[9];
        int n = 0;
        f[n++] = ln;
        for (char *c = ln; *c && n < 9; c++) if (*c == '\t') { *c = 0; f[n++] = c + 1; }
        if (n < 8) continue;
        bdl_t *d = &bdata_dl[bdata_ndl++];
        memset(d, 0, sizeof *d);
        d->id = atoi(f[0]);
        d->state = (uint8_t)atoi(f[1]);
        d->time = (uint32_t)strtoul(f[2], NULL, 10);
        d->got = (uint32_t)strtoul(f[3], NULL, 10);
        d->total = (uint32_t)strtoul(f[4], NULL, 10);
        strlcpy(d->name, f[5], sizeof d->name);
        strlcpy(d->path, f[6], sizeof d->path);
        strlcpy(d->url, f[7], sizeof d->url);
        strlcpy(d->reason, n > 8 ? f[8] : "", sizeof d->reason);
        if (d->state > BDL_CANCELLED) d->state = BDL_FAILED;
        if (d->state == BDL_ACTIVE) { d->state = BDL_FAILED; strlcpy(d->reason, "Interrupted", sizeof d->reason); }
        if (d->id >= dl_next_id) dl_next_id = d->id + 1;
    }
}

bdl_t *bdata_dl_add(const char *name, const char *url) {
    bdata_load();
    if (!bdata_dl) return NULL;
    if (bdata_ndl >= DL_MAX) {
        /* the oldest finished entry makes room */
        int k = 0;
        while (k < bdata_ndl && bdata_dl[k].state == BDL_ACTIVE) k++;
        if (k >= bdata_ndl) return NULL;
        memmove(&bdata_dl[k], &bdata_dl[k + 1], sizeof *bdata_dl * (size_t)(bdata_ndl - k - 1));
        bdata_ndl--;
    }
    bdl_t *d = &bdata_dl[bdata_ndl++];
    memset(d, 0, sizeof *d);
    d->id = dl_next_id++;
    d->state = BDL_ACTIVE;
    d->time = sys_time();
    strlcpy(d->name, name ? name : "download", sizeof d->name);
    strlcpy(d->url, url ? url : "", sizeof d->url);
    bdata_dl_changed(true);
    return d;
}

bdl_t *bdata_dl_find(int id) {
    for (int i = 0; i < bdata_ndl; i++) if (bdata_dl[i].id == id) return &bdata_dl[i];
    return NULL;
}

void bdata_dl_remove(int id) {
    for (int i = 0; i < bdata_ndl; i++)
        if (bdata_dl[i].id == id) {
            memmove(&bdata_dl[i], &bdata_dl[i + 1], sizeof *bdata_dl * (size_t)(bdata_ndl - i - 1));
            bdata_ndl--;
            bdata_dl_changed(true);
            return;
        }
}

void bdata_dl_clear(void) {
    int k = 0;
    for (int i = 0; i < bdata_ndl; i++) if (bdata_dl[i].state == BDL_ACTIVE) bdata_dl[k++] = bdata_dl[i];
    bdata_ndl = k;
    bdata_dl_changed(true);
}

void bdata_dl_changed(bool save) {
    bdata_dl_gen++;
    if (save) save_downloads();
}

uint32_t bdata_dl_risk_sig(void) {
    uint32_t h = 2166136261u;
    if (!safetyshield_file_risk) return 0;
    for (int i = 0; i < bdata_ndl; i++)
        if (bdata_dl[i].state == BDL_DONE && bdata_dl[i].path[0])
            h = (h ^ (uint32_t)(safetyshield_file_risk(bdata_dl[i].path) + 2)) * 16777619u;
    return h;
}

bool bdata_is_dl_page(const char *url) {
    return url && !strncasecmp(url, "browser://downloads", 19);
}

void bdata_save_settings(void) {
    if (!vfs_is_dir("/system/config")) vfs_mkdirs("/system/config");
    const char *s = bdata_history_on ? "history=1\n" : "history=0\n";
    vfs_write_file(CFG_PATH, s, strlen(s));
}

/* split "a\tb\tc" lines; calls fn with up to 3 fields */
static void each_line(char *text, void (*fn)(char **f, int n)) {
    char *save = NULL;
    for (char *ln = strtok_r(text, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        char *f[3] = { ln, NULL, NULL };
        int n = 1;
        for (char *p = ln; *p && n < 3; p++) if (*p == '\t') { *p = 0; f[n++] = p + 1; }
        size_t l = strlen(f[n - 1]);
        if (l && f[n - 1][l - 1] == '\r') f[n - 1][l - 1] = 0;
        fn(f, n);
    }
}

static void load_hist_line(char **f, int n) {
    if (n < 2 || bdata_nhist >= HIST_MAX) return;
    hitem_t *h = &bdata_hist[bdata_nhist++];
    h->time = (uint32_t)strtoul(f[0], NULL, 10);
    strlcpy(h->url, f[1], sizeof h->url);
    strlcpy(h->title, n > 2 ? f[2] : "", sizeof h->title);
}

static void load_bm_line(char **f, int n) {
    if (bdata_nbm >= BM_MAX || !f[0][0]) return;
    bookmark_t *b = &bdata_bm[bdata_nbm++];
    strlcpy(b->url, f[0], sizeof b->url);
    strlcpy(b->title, n > 1 ? f[1] : "", sizeof b->title);
}

void bdata_load(void) {
    if (loaded) return;
    loaded = true;
    bdata_hist = calloc(HIST_MAX, sizeof *bdata_hist);
    bdata_bm = calloc(BM_MAX, sizeof *bdata_bm);
    bdata_dl = calloc(DL_MAX, sizeof *bdata_dl);
    size_t sz;
    char *cfg = vfs_read_file(CFG_PATH, &sz);
    if (cfg) {
        if (strstr(cfg, "history=0")) bdata_history_on = false;
        free(cfg);
    }
    if (bdata_hist) {
        char *h = vfs_read_file(HIST_PATH, &sz);
        if (h) { each_line(h, load_hist_line); free(h); }
    }
    if (bdata_bm) {
        char *b = vfs_read_file(BM_PATH, &sz);
        if (b) { each_line(b, load_bm_line); free(b); }
    }
    if (bdata_dl) {
        char *d = vfs_read_file(DL_PATH, &sz);
        if (d) { load_downloads(d); free(d); }
    }
}

void bdata_history_add(const char *url, const char *title) {
    if (!bdata_history_on || !bdata_hist || !url[0]) return;
    if (!strncmp(url, "about:", 6) || !strncmp(url, "view-source:", 12) || !strncasecmp(url, "browser:", 8)) return;
    if (bdata_nhist && !strcmp(bdata_hist[bdata_nhist - 1].url, url)) {
        strlcpy(bdata_hist[bdata_nhist - 1].title, title, sizeof bdata_hist[0].title);
        bdata_hist[bdata_nhist - 1].time = sys_time();
    } else {
        if (bdata_nhist >= HIST_MAX) {
            memmove(bdata_hist, bdata_hist + 1, sizeof *bdata_hist * (HIST_MAX - 1));
            bdata_nhist--;
        }
        hitem_t *h = &bdata_hist[bdata_nhist++];
        strlcpy(h->url, url, sizeof h->url);
        strlcpy(h->title, title, sizeof h->title);
        h->time = sys_time();
    }
    save_history();
}

void bdata_history_clear(void) {
    bdata_nhist = 0;
    if (vfs_exists(HIST_PATH)) vfs_remove(HIST_PATH);
}

int bdata_bookmark_find(const char *url) {
    for (int i = 0; i < bdata_nbm; i++) if (!strcmp(bdata_bm[i].url, url)) return i;
    return -1;
}

void bdata_bookmark_add(const char *url, const char *title) {
    if (!bdata_bm || bdata_nbm >= BM_MAX || bdata_bookmark_find(url) >= 0) return;
    bookmark_t *b = &bdata_bm[bdata_nbm++];
    strlcpy(b->url, url, sizeof b->url);
    strlcpy(b->title, title && *title ? title : url, sizeof b->title);
    save_bookmarks();
}

void bdata_bookmark_remove(int i) {
    if (i < 0 || i >= bdata_nbm) return;
    memmove(&bdata_bm[i], &bdata_bm[i + 1], sizeof *bdata_bm * (size_t)(bdata_nbm - i - 1));
    bdata_nbm--;
    save_bookmarks();
}

/* ---- built-in pages ---------------------------------------------------------------- */
static const char page_css[] =
    "body{font-family:sans-serif;background:#f3f5f9;color:#1f2430;margin:0}"
    ".wrap{max-width:640px;margin:0 auto;padding:48px 24px}"
    "h1{font-size:28px;font-weight:bold;margin:0 0 12px 0}"
    "h2{font-size:16px;color:#5a6270;margin:28px 0 8px 0}"
    "p{font-size:15px;line-height:1.45}"
    "a{color:#1a56c9}"
    ".detail{color:#6b7280;font-size:13px}"
    ".actions{margin-top:28px}"
    ".btn{display:inline-block;background:#1a56c9;color:white;padding:8px 18px;text-decoration:none;margin-right:10px}"
    ".plain{display:inline-block;padding:8px 6px;color:#1a56c9}"
    ".warn{border-left:4px solid #d93025;padding-left:16px}"
    ".warn h1{color:#c5221f}";

char *bpage_home(void) {
    char *b = NULL;
    size_t len = 0, cap = 0;
    bhtml_add(&b, &len, &cap, "<!doctype html><html><head><title>New Tab</title><style>");
    bhtml_add(&b, &len, &cap, page_css);
    bhtml_add(&b, &len, &cap,
        ".home{text-align:center;padding-top:90px}"
        ".home h1{font-size:34px;color:#1a56c9;margin-bottom:26px}"
        ".home input[type=text]{font-size:16px;padding:6px}"
        ".home input[type=submit]{font-size:15px;padding:5px 14px}"
        "</style></head><body><div class=\"wrap home\"><h1>Web Browser</h1>"
        "<form action=\"" B_SEARCH_FORM "\" method=\"get\">");
    bhtml_add(&b, &len, &cap,
        "<input type=\"text\" name=\"q\" size=\"40\" placeholder=\"Search the web\"> "
        "<input type=\"submit\" value=\"Search\"></form></div></body></html>");
    return b;
}

char *bpage_history(void) {
    char *b = NULL;
    size_t len = 0, cap = 0;
    bhtml_add(&b, &len, &cap, "<!doctype html><html><head><title>History</title><style>");
    bhtml_add(&b, &len, &cap, page_css);
    bhtml_add(&b, &len, &cap, "ul{list-style:none;padding:0;margin:0}li{padding:3px 0;font-size:14px}"
                              ".t{color:#6b7280;display:inline-block;width:52px}</style></head><body><div class=wrap><h1>History</h1>");
    if (!bdata_history_on) bhtml_add(&b, &len, &cap, "<p class=detail>History is turned off in the browser settings.</p>");
    if (!bdata_nhist) bhtml_add(&b, &len, &cap, "<p>No pages visited yet.</p>");
    int last_day = -1;
    char tmp[160];
    for (int i = bdata_nhist - 1; i >= 0; i--) {
        hitem_t *h = &bdata_hist[i];
        datetime_t dt;
        sys_time_to_datetime(h->time, &dt);
        int day = dt.year * 400 + dt.month * 32 + dt.day;
        if (day != last_day) {
            if (last_day >= 0) bhtml_add(&b, &len, &cap, "</ul>");
            snprintf(tmp, sizeof tmp, "<h2>%s, %s %d, %d</h2><ul>", sys_weekday_name(dt.weekday),
                     sys_month_name(dt.month), dt.day, dt.year);
            bhtml_add(&b, &len, &cap, tmp);
            last_day = day;
        }
        snprintf(tmp, sizeof tmp, "<li><span class=t>%02d:%02d</span> <a href=\"", dt.hour, dt.minute);
        bhtml_add(&b, &len, &cap, tmp);
        bhtml_escape(&b, &len, &cap, h->url, strlen(h->url));
        bhtml_add(&b, &len, &cap, "\">");
        const char *t = h->title[0] ? h->title : h->url;
        bhtml_escape(&b, &len, &cap, t, strlen(t));
        bhtml_add(&b, &len, &cap, "</a></li>");
    }
    if (last_day >= 0) bhtml_add(&b, &len, &cap, "</ul>");
    bhtml_add(&b, &len, &cap, "</div></body></html>");
    return b;
}

static void fmt_size(uint32_t n, char *out, size_t size) {
    if (n < 1048576) snprintf(out, size, "%u KB", (unsigned)((n + 1023) / 1024));
    else snprintf(out, size, "%u.%u MB", (unsigned)(n / 1048576), (unsigned)((uint64_t)(n % 1048576) * 10 / 1048576));
}

char *bpage_downloads(void) {
    bdata_load();
    char *b = NULL;
    size_t len = 0, cap = 0;
    char tmp[600], s1[24], s2[24];
    bhtml_add(&b, &len, &cap, "<!doctype html><html><head><title>Downloads</title><style>");
    bhtml_add(&b, &len, &cap, page_css);
    bhtml_add(&b, &len, &cap,
        ".wrap{max-width:720px;padding-top:28px}"
        ".top{display:flex;align-items:center;justify-content:space-between;margin-bottom:18px}"
        ".top h1{margin:0}.clear{font-size:14px;text-decoration:none}"
        ".item{display:flex;align-items:flex-start;gap:14px;background:white;border:1px solid #dde2ea;border-radius:10px;"
        "padding:14px 16px;margin-bottom:12px}"
        ".icon{flex:none;width:32px;height:38px}"
        ".info{flex:1;min-width:0}"
        ".name{font-size:15px;font-weight:bold;color:#1a56c9;text-decoration:none}"
        ".gone{font-size:15px;font-weight:bold;color:#80868b;text-decoration:line-through}"
        ".src{font-size:12px;color:#6b7280;margin-top:3px;overflow:hidden;white-space:nowrap}"
        ".st{font-size:13px;color:#3c4043;margin-top:8px}"
        ".bar{height:6px;background:#e3e7ee;border-radius:3px;margin:8px 0 4px 0;max-width:420px}"
        ".fill{height:6px;background:#1a56c9;border-radius:3px}"
        ".risk{font-size:13px;margin-top:8px;font-weight:bold}"
        ".acts{margin-top:8px;font-size:13px}.acts a{margin-right:16px;text-decoration:none}"
        ".x{flex:none;font-size:20px;color:#80868b;text-decoration:none;line-height:20px}"
        ".none{color:#6b7280;font-size:15px;margin-top:40px;text-align:center}"
        "</style></head><body><div class=wrap><div class=top><h1>Downloads</h1>");
    if (bdata_ndl) bhtml_add(&b, &len, &cap, "<a class=clear href=\"browser://downloads/clear\">Clear all</a>");
    bhtml_add(&b, &len, &cap, "</div>");
    if (!bdata_ndl) bhtml_add(&b, &len, &cap, "<p class=none>Files you download appear here.</p>");
    for (int i = bdata_ndl - 1; i >= 0; i--) {
        bdl_t *d = &bdata_dl[i];
        bool exists = d->state == BDL_DONE && d->path[0] && vfs_exists(d->path);
        bhtml_add(&b, &len, &cap,
            "<div class=item><svg class=icon viewBox=\"0 0 32 38\"><path d=\"M3 1h18l8 8v27a1 1 0 0 1-1 1H3a1 1 0 0 1-1-1V2a1 1 0 0 1 1-1z\" "
            "fill=\"#f1f4f9\" stroke=\"#8a94a6\" stroke-width=\"1.5\"/><path d=\"M21 1v8h8\" fill=\"#dde2ea\" stroke=\"#8a94a6\" stroke-width=\"1.5\"/>"
            "<path d=\"M8 18h16M8 23h16M8 28h11\" stroke=\"#8a94a6\" stroke-width=\"1.5\"/></svg><div class=info>");
        const char *shown = d->path[0] ? vfs_basename(d->path) : d->name;
        if (exists) {
            snprintf(tmp, sizeof tmp, "<a class=name href=\"browser://downloads/open?%d\">", d->id);
            bhtml_add(&b, &len, &cap, tmp);
            bhtml_escape(&b, &len, &cap, shown, strlen(shown));
            bhtml_add(&b, &len, &cap, "</a>");
        } else {
            bhtml_add(&b, &len, &cap, d->state == BDL_ACTIVE ? "<span class=name>" :
                                      d->state == BDL_FAILED ? "<span class=name style=\"color:#3c4043\">" : "<span class=gone>");
            bhtml_escape(&b, &len, &cap, shown, strlen(shown));
            bhtml_add(&b, &len, &cap, "</span>");
        }
        bhtml_add(&b, &len, &cap, "<div class=src>");
        bhtml_escape(&b, &len, &cap, d->url, strlen(d->url) > 110 ? 110 : strlen(d->url));
        if (strlen(d->url) > 110) bhtml_add(&b, &len, &cap, "...");
        bhtml_add(&b, &len, &cap, "</div>");
        switch (d->state) {
        case BDL_ACTIVE: {
            int pct = d->total ? (int)((uint64_t)d->got * 100 / d->total) : 0;
            fmt_size(d->got, s1, sizeof s1);
            if (d->total) {
                fmt_size(d->total, s2, sizeof s2);
                snprintf(tmp, sizeof tmp, "<div class=bar><div class=fill style=\"width:%d%%\"></div></div>"
                         "<div class=st>%s of %s</div>", pct, s1, s2);
            } else snprintf(tmp, sizeof tmp, "<div class=bar></div><div class=st>%s</div>", s1);
            bhtml_add(&b, &len, &cap, tmp);
            break;
        }
        case BDL_DONE:
            if (exists) {
                fmt_size(d->got, s1, sizeof s1);
                snprintf(tmp, sizeof tmp, "<div class=st>Done - %s</div>", s1);
            } else strlcpy(tmp, "<div class=st>Deleted</div>", sizeof tmp);
            bhtml_add(&b, &len, &cap, tmp);
            break;
        case BDL_FAILED:
            bhtml_add(&b, &len, &cap, "<div class=st style=\"color:#c5221f\">Failed \x96 ");
            bhtml_escape(&b, &len, &cap, d->reason[0] ? d->reason : "Network error", strlen(d->reason[0] ? d->reason : "Network error"));
            bhtml_add(&b, &len, &cap, "</div>");
            break;
        default:
            bhtml_add(&b, &len, &cap, "<div class=st>Cancelled</div>");
            break;
        }
        /* SafetyShield's verdict on anything that is not safe (like a dangerous-download warning) */
        int risk = exists && safetyshield_file_risk ? safetyshield_file_risk(d->path) : -1;
        if (risk > SS_RISK_SAFE && risk < SS_RISK_COUNT) {
            uint32_t c = safetyshield_risk_color(risk);
            snprintf(tmp, sizeof tmp, "<div class=risk style=\"color:#%06x\">SafetyShield: %s \x96 ", (unsigned)(c & 0xFFFFFF),
                     safetyshield_risk_name(risk));
            bhtml_add(&b, &len, &cap, tmp);
            const char *sum = safetyshield_risk_summary(risk);
            if (sum) bhtml_escape(&b, &len, &cap, sum, strlen(sum));
            bhtml_add(&b, &len, &cap, "</div>");
        }
        bhtml_add(&b, &len, &cap, "<div class=acts>");
        if (d->state == BDL_ACTIVE) snprintf(tmp, sizeof tmp, "<a href=\"browser://downloads/cancel?%d\">Cancel</a>", d->id);
        else if (d->state == BDL_FAILED || d->state == BDL_CANCELLED) snprintf(tmp, sizeof tmp, "<a href=\"browser://downloads/retry?%d\">Retry</a>", d->id);
        else tmp[0] = 0;
        bhtml_add(&b, &len, &cap, tmp);
        bhtml_add(&b, &len, &cap, "<a href=\"browser://downloads/show\">Show in folder</a></div></div>");
        snprintf(tmp, sizeof tmp, "<a class=x href=\"browser://downloads/remove?%d\" title=\"Remove from list\">\xd7</a></div>", d->id);
        bhtml_add(&b, &len, &cap, tmp);
    }
    bhtml_add(&b, &len, &cap, "</div></body></html>");
    return b;
}

char *bpage_error(const char *title, const char *text, const char *detail, const char *url, bool cert) {
    char *b = NULL;
    size_t len = 0, cap = 0;
    bhtml_add(&b, &len, &cap, "<!doctype html><html><head><title>");
    bhtml_escape(&b, &len, &cap, title, strlen(title));
    bhtml_add(&b, &len, &cap, "</title><style>");
    bhtml_add(&b, &len, &cap, page_css);
    bhtml_add(&b, &len, &cap, "</style></head><body><div class=wrap>");
    if (cert) bhtml_add(&b, &len, &cap, "<div class=warn>");
    bhtml_add(&b, &len, &cap, "<h1>");
    bhtml_escape(&b, &len, &cap, title, strlen(title));
    bhtml_add(&b, &len, &cap, "</h1><p>");
    bhtml_escape(&b, &len, &cap, text, strlen(text));
    bhtml_add(&b, &len, &cap, "</p>");
    if (detail && *detail) {
        bhtml_add(&b, &len, &cap, "<p class=detail>");
        bhtml_escape(&b, &len, &cap, detail, strlen(detail));
        bhtml_add(&b, &len, &cap, "</p>");
    }
    if (url && *url) {
        bhtml_add(&b, &len, &cap, "<p class=detail>");
        bhtml_escape(&b, &len, &cap, url, strlen(url));
        bhtml_add(&b, &len, &cap, "</p>");
    }
    if (cert) {
        bhtml_add(&b, &len, &cap, "</div><div class=actions><a class=btn href=\"about:back\">Go back</a>"
                                  "<a class=plain href=\"about:continue\">Continue anyway (not safe)</a></div>");
    } else {
        bhtml_add(&b, &len, &cap, "<div class=actions><a class=btn href=\"about:retry\">Try again</a></div>");
    }
    bhtml_add(&b, &len, &cap, "</div></body></html>");
    return b;
}

#define SHOW_MAX (2u << 20)   /* text shown in a text / source view */

char *bpage_source(const char *url, const char *src, uint32_t len) {
    char *b = NULL;
    size_t bl = 0, cap = 0;
    bool cut = len > SHOW_MAX;
    if (cut) len = SHOW_MAX;
    bhtml_add(&b, &bl, &cap, "<!doctype html><html><head><title>Source of ");
    bhtml_escape(&b, &bl, &cap, url, strlen(url));
    bhtml_add(&b, &bl, &cap, "</title></head><body style=\"margin:8px;background:white\">"
                             "<pre style=\"white-space:pre-wrap;font-size:13px;margin:0\">");
    size_t tl = 0;
    char *t = web_to_1252(src ? src : "", len, WEB_CS_AUTO, &tl);
    if (t) { bhtml_escape(&b, &bl, &cap, t, tl); free(t); }
    bhtml_add(&b, &bl, &cap, "</pre>");
    if (cut) bhtml_add(&b, &bl, &cap, "<p><i>The source is too long to show all of it: only the first 2 MB are shown.</i></p>");
    bhtml_add(&b, &bl, &cap, "</body></html>");
    return b;
}

char *bpage_text(const char *src, uint32_t len) {
    char *b = NULL;
    size_t bl = 0, cap = 0;
    bool cut = len > SHOW_MAX;
    if (cut) len = SHOW_MAX;
    bhtml_add(&b, &bl, &cap, "<!doctype html><html><body style=\"margin:8px\"><pre style=\"white-space:pre-wrap;margin:0\">");
    size_t tl = 0;
    char *t = web_to_1252(src ? src : "", len, WEB_CS_AUTO, &tl);
    if (t) { bhtml_escape(&b, &bl, &cap, t, tl); free(t); }
    bhtml_add(&b, &bl, &cap, "</pre>");
    if (cut) bhtml_add(&b, &bl, &cap, "<p><i>The file is too long to show all of it: only the first 2 MB are shown.</i></p>");
    bhtml_add(&b, &bl, &cap, "</body></html>");
    return b;
}

char *bpage_image(const char *url) {
    char *b = NULL;
    size_t bl = 0, cap = 0;
    bhtml_add(&b, &bl, &cap, "<!doctype html><html><head><title>");
    const char *name = strrchr(url, '/');
    name = name && name[1] ? name + 1 : url;
    bhtml_escape(&b, &bl, &cap, name, strlen(name));
    bhtml_add(&b, &bl, &cap, "</title></head><body style=\"margin:0;background:#2b2b2b;text-align:center\"><img src=\"");
    bhtml_escape(&b, &bl, &cap, url, strlen(url));
    bhtml_add(&b, &bl, &cap, "\" style=\"max-width:100%\"></body></html>");
    return b;
}
