/*
 * browser_data.c - the Web Browser's settings, history and bookmarks, and
 * the pages it makes itself (home page, history, error pages, page source).
 *
 *   /system/config/browser.cfg      history=1|0
 *   /home/user/.browser/history     time<TAB>url<TAB>title per line, oldest first
 *   /home/user/.browser/bookmarks   url<TAB>title per line
 *
 * Titles are kept in the page text charset (Windows-1252).
 */
#include "browser.h"

#define CFG_PATH  "/system/config/browser.cfg"
#define DATA_DIR  "/home/user/.browser"
#define HIST_PATH DATA_DIR "/history"
#define BM_PATH   DATA_DIR "/bookmarks"
#define HIST_MAX  500
#define BM_MAX    64

bool        bdata_history_on = true;
bookmark_t *bdata_bm;
int         bdata_nbm;
hitem_t    *bdata_hist;
int         bdata_nhist;
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
}

void bdata_history_add(const char *url, const char *title) {
    if (!bdata_history_on || !bdata_hist || !url[0]) return;
    if (!strncmp(url, "about:", 6) || !strncmp(url, "view-source:", 12)) return;
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
