/*
 * search.c - NovaOS Search: the crawler, live discovery, pages the user
 * visits and the Browser://Search results page.
 *
 * Crawling runs while a browser window is open, from its timer: breadth first
 * from a built-in list of well-known sites, one request at a time (none while
 * a tab is loading), at most one request every 3 s per site (or the site's
 * Crawl-delay), robots.txt (groups for NovaOS-Search or *, Allow / Disallow
 * with * and $, longest match wins) and <meta name=robots> are obeyed, no
 * cookies are sent or kept and the User-Agent is "NovaOS-Search/2.5". Pages
 * are read a slice per tick so the window stays smooth.
 *
 * A search with few results also looks on the web right away: the Wikipedia
 * article with the query as its title, <query>.com / .org and waiting links
 * whose text or address has the query's words; the results page updates as
 * they arrive.
 *
 * Files: /home/user/.search/index (see search_index.c) and
 * /home/user/.search/frontier (depth<TAB>url<TAB>link text per line). The index
 * is kept below 8 MB on an installed system and 3 MB when running from the CD
 * (everything is in memory then).
 *
 * Test machines whose network intercepts TLS can boot with
 * search_allow_bad_certs=1; otherwise the crawler skips sites whose
 * certificate fails, except those the user chose "Continue anyway" for.
 */
#include "browser.h"
#include "search.h"
#include <nova/disk.h>

#define SDIR        "/home/user/.search"
#define IDX_PATH    SDIR "/index"
#define FRONT_PATH  SDIR "/frontier"
#define UA_HEADER   "User-Agent: NovaOS-Search/2.5\r\n"
#define CAP_DISK    (8u << 20)
#define CAP_RAM     (3u << 20)
#define FRONT_MAX   3000
#define FRONT_SAVE  2000
#define HOST_MAX    256
#define HOST_QUEUE  120         /* addresses waiting per site                */
#define HOST_PAGES  150         /* pages fetched per site and session        */
#define MAX_DEPTH   3
#define DELAY_MS    3000        /* between two requests to one site          */
#define LIVE_DELAY  1000
#define MAX_BODY    (1536u << 10)
#define SLICE       (24u << 10) /* HTML read per timer tick                  */
#define SAVE_MS     180000
#define REQ_MAX     5           /* two crawling, up to three live            */
#define CRAWL_REQS  2
#define LIVE_REQS   3
#define JOB_MAX     6
#define LIVE_MAX    10
#define LIVE_MS     25000
#define PER_PAGE    10
#define MAX_HITS    200

static const char *const seeds[] = {
    "https://en.wikipedia.org/wiki/Main_Page", "https://www.python.org/", "https://www.kernel.org/",
    "https://www.gnu.org/", "https://www.debian.org/", "https://www.mozilla.org/en-US/", "https://www.w3.org/",
    "https://www.ietf.org/", "https://www.apache.org/", "https://www.sqlite.org/", "https://www.rust-lang.org/",
    "https://go.dev/", "https://www.php.net/", "https://www.ruby-lang.org/en/", "https://www.lua.org/",
    "https://www.gutenberg.org/", "https://archive.org/", "https://www.nasa.gov/", "https://www.bbc.com/news",
    "https://lite.cnn.com/", "https://text.npr.org/", "https://news.ycombinator.com/", "https://lwn.net/",
    "https://www.eff.org/", "https://www.fsf.org/", "https://f-droid.org/", "https://www.gimp.org/",
    "https://www.libreoffice.org/", "https://www.videolan.org/", "https://git-scm.com/", "https://wiki.osdev.org/Main_Page",
    "https://www.haiku-os.org/", "https://www.freebsd.org/", "https://www.openbsd.org/", "https://www.netbsd.org/",
    "https://www.raspberrypi.com/", "https://www.arduino.cc/", "https://www.who.int/", "https://www.un.org/en/",
    "https://www.britannica.com/", "https://www.weather.gov/", "https://www.cdc.gov/", "https://www.loc.gov/",
    "https://www.si.edu/", "https://ocw.mit.edu/", "https://www.iana.org/", "https://letsencrypt.org/",
    "https://www.qemu.org/", "https://www.virtualbox.org/", "https://www.nasm.us/", "https://gcc.gnu.org/",
    "https://llvm.org/", "https://www.vim.org/", "https://www.blender.org/", "https://inkscape.org/",
    "https://www.wikimedia.org/", "https://www.postgresql.org/", "https://www.perl.org/", "https://www.haskell.org/",
    "https://kotlinlang.org/", "https://developer.mozilla.org/en-US/", "https://docs.python.org/3/",
    "https://www.reactos.org/", "https://ubuntu.com/", "https://example.com/",
};

/* ---- sites --------------------------------------------------------------------------------- */
enum { R_NONE = 0, R_FETCHING, R_READY, R_FAILED };
typedef struct srule { bool allow; uint16_t len; char *pat; } srule_t;
typedef struct shost {
    uint32_t hash;
    char origin[96];         /* "https://en.wikipedia.org"                       */
    uint8_t robots;
    bool busy;               /* a request to it is running                       */
    uint32_t retry_ms, next_ms, delay_ms, used;
    srule_t *rules;
    int nrules;
    uint16_t pages, fails;
} shost_t;

typedef struct fent { char *url, *anchor; uint32_t hhash; uint8_t depth; } fent_t;

enum { Q_PAGE = 1, Q_ROBOTS };
typedef struct creq {
    http_request_t *r;
    int kind, host;
    uint8_t depth;
    bool live, checked;
    char *url;
} creq_t;

typedef struct job {
    bool used, live;
    uint8_t depth, flags;
    char *html, *url;
    uint32_t len;
    srch_page_t p;
} job_t;

typedef struct lcand { char *url; uint8_t state; } lcand_t;   /* 0 waiting, 1 asked, 2 done */

static shost_t hosts[HOST_MAX];
static int nhosts;
static fent_t *front;
static int nfront;
static uint16_t qcount[1024];      /* addresses waiting per site (by hash, roughly) */
static uint32_t *seen, seencap, nseen;
static creq_t reqs[REQ_MAX];
static job_t jobs[JOB_MAX];
static bool started, insecure;
static uint32_t first_tick, next_crawl, last_save, saved_gen, use_clock, pending_until;
static char pending[200];          /* a search made before the network was up */
static struct {
    bool active;
    char text[200];
    srch_query_t q;
    uint32_t start, gen;
    lcand_t cand[LIVE_MAX];
    int ncand;
} live;

static uint32_t hash_s(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h ? h : 1;
}

static bool origin_of(const char *url, char *out, size_t size, char *path, size_t psize) {
    url_t *u = malloc(sizeof *u);
    if (!u) return false;
    bool ok = url_parse(url, u);
    if (ok) {
        bool def = (!strcmp(u->scheme, "https") && u->port == 443) || (!strcmp(u->scheme, "http") && u->port == 80);
        if (def) snprintf(out, size, "%s://%s", u->scheme, u->host);
        else snprintf(out, size, "%s://%s:%u", u->scheme, u->host, (unsigned)u->port);
        if (path) strlcpy(path, u->path, psize);
    }
    free(u);
    return ok && strlen(out) < size - 1;
}

static shost_t *host_find(uint32_t h) {
    for (int i = 0; i < nhosts; i++) if (hosts[i].hash == h) return &hosts[i];
    return NULL;
}

static void host_reset(shost_t *h) {
    for (int i = 0; i < h->nrules; i++) free(h->rules[i].pat);
    free(h->rules);
    memset(h, 0, sizeof *h);
}

static shost_t *host_get(const char *url) {
    char origin[96];
    if (!origin_of(url, origin, sizeof origin, NULL, 0)) return NULL;
    uint32_t hh = hash_s(origin);
    shost_t *h = host_find(hh);
    if (!h) {
        if (nhosts < HOST_MAX) h = &hosts[nhosts++];
        else {
            /* the site not used for the longest time */
            for (int i = 0; i < nhosts; i++)
                if (!hosts[i].busy && (!h || hosts[i].used < h->used)) h = &hosts[i];
            if (!h) return NULL;
            host_reset(h);
        }
        h->hash = hh;
        strlcpy(h->origin, origin, sizeof h->origin);
        h->delay_ms = DELAY_MS;
    }
    h->used = ++use_clock;
    return h;
}

/* ---- robots.txt ---------------------------------------------------------------------------- */
static bool pmatch(const char *pat, const char *s, int depth) {
    for (; *pat; pat++) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return true;
            if (depth > 6) return false;
            for (; *s; s++) if (pmatch(pat, s, depth + 1)) return true;
            return pmatch(pat, s, depth + 1);
        }
        if (*pat == '$' && !pat[1]) return !*s;
        if (*s != *pat) return false;
        s++;
    }
    return true;
}

static bool robots_ok(const shost_t *h, const char *path) {
    int best = -1;
    bool allow = true;
    for (int i = 0; i < h->nrules; i++) {
        const srule_t *r = &h->rules[i];
        if (r->len < best || (r->len == best && !r->allow)) continue;
        if (pmatch(r->pat, path, 0)) { best = r->len; allow = r->allow; }
    }
    return allow;
}

static void robots_parse(shost_t *h, const char *txt, uint32_t len) {
    srule_t *list[2] = { NULL, NULL };
    int n[2] = { 0, 0 }, cap[2] = { 0, 0 };
    uint32_t delay[2] = { 0, 0 };
    bool mine = false, star = false, agents = false, any_mine = false;
    const char *s = txt, *end = txt + len;
    while (s < end) {
        const char *e = s;
        while (e < end && *e != '\n' && *e != '\r') e++;
        char line[300];
        size_t l = (size_t)(e - s) < sizeof line - 1 ? (size_t)(e - s) : sizeof line - 1;
        memcpy(line, s, l);
        line[l] = 0;
        s = e + 1;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *key = line, *val = colon + 1;
        while (*key == ' ' || *key == '\t') key++;
        for (char *k = key + strlen(key); k > key && (k[-1] == ' ' || k[-1] == '\t'); ) *--k = 0;
        while (*val == ' ' || *val == '\t') val++;
        for (char *k = val + strlen(val); k > val && (k[-1] == ' ' || k[-1] == '\t'); ) *--k = 0;
        if (!strcasecmp(key, "user-agent")) {
            if (!agents) { mine = star = false; agents = true; }
            if (strcasestr(val, "novaos")) { mine = true; any_mine = true; }
            else if (!strcmp(val, "*")) star = true;
            continue;
        }
        bool al = !strcasecmp(key, "allow"), dis = !strcasecmp(key, "disallow");
        if (al || dis || !strcasecmp(key, "crawl-delay")) agents = false;
        if (!strcasecmp(key, "crawl-delay")) {
            uint32_t d = (uint32_t)atoi(val);
            if (mine) delay[1] = d;
            if (star) delay[0] = d;
            continue;
        }
        if ((!al && !dis) || !val[0] || strlen(val) > 200) continue;
        for (int g = 0; g < 2; g++) {
            if (!(g ? mine : star) || n[g] >= 1000) continue;
            if (n[g] >= cap[g]) {
                int nc = cap[g] ? cap[g] * 2 : 32;
                srule_t *nl = realloc(list[g], sizeof *nl * (size_t)nc);
                if (!nl) continue;
                list[g] = nl;
                cap[g] = nc;
            }
            list[g][n[g]].allow = al;
            list[g][n[g]].len = (uint16_t)strlen(val);
            list[g][n[g]].pat = strdup(val);
            if (list[g][n[g]].pat) n[g]++;
        }
    }
    int use = any_mine ? 1 : 0;
    for (int i = 0; i < n[!use]; i++) free(list[!use][i].pat);
    free(list[!use]);
    for (int i = 0; i < h->nrules; i++) free(h->rules[i].pat);
    free(h->rules);
    h->rules = list[use];
    h->nrules = n[use];
    uint32_t d = delay[use] * 1000;
    h->delay_ms = d < DELAY_MS ? DELAY_MS : d > 30000 ? 30000 : d;
}

/* ---- addresses waiting (the frontier) ------------------------------------------------------ */
static bool seen_add(uint32_t key);

static void seen_rebuild(void) {
    memset(seen, 0, sizeof *seen * seencap);
    nseen = 0;
    for (int i = 0; i < sidx_count(); i++) seen_add(sidx_doc(i)->key);
    for (int i = 0; i < nfront; i++) seen_add(srch_url_key(front[i].url));
}

static bool seen_add(uint32_t key) {
    if (!seencap) {
        seen = calloc(16384, sizeof *seen);
        if (!seen) return true;
        seencap = 16384;
    }
    if ((nseen + 1) * 10 > seencap * 7) {
        if (seencap < (1u << 17)) {
            uint32_t nc = seencap * 2, *ns = calloc(nc, sizeof *ns);
            if (ns) {
                for (uint32_t i = 0; i < seencap; i++) {
                    if (!seen[i]) continue;
                    uint32_t k = seen[i] & (nc - 1);
                    while (ns[k]) k = (k + 1) & (nc - 1);
                    ns[k] = seen[i];
                }
                free(seen);
                seen = ns;
                seencap = nc;
            }
        } else seen_rebuild();
        if ((nseen + 1) * 10 > seencap * 7) return false;
    }
    uint32_t k = key & (seencap - 1);
    while (seen[k]) {
        if (seen[k] == key) return false;
        k = (k + 1) & (seencap - 1);
    }
    seen[k] = key;
    nseen++;
    return true;
}

static bool ends_with(const char *s, size_t n, const char *suffix) {
    size_t l = strlen(suffix);
    return n >= l && !strncasecmp(s + n - l, suffix, l);
}

/* worth crawling: a web page (not a picture, a download, a log-in ...), no query string */
static bool crawlable(const char *url, bool query_ok) {
    if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)) return false;
    size_t n = strlen(url);
    if (n > 300 || (!query_ok && strchr(url, '?'))) return false;
    const char *host = strstr(url, "://") + 3, *path = host + strcspn(host, "/?#");
    size_t pl = strcspn(path, "?#");
    static const char *const skip_ext[] = {
        ".jpg", ".jpeg", ".png", ".gif", ".svg", ".webp", ".ico", ".bmp", ".pdf", ".zip", ".gz", ".tgz", ".bz2", ".xz",
        ".7z", ".rar", ".exe", ".msi", ".dmg", ".iso", ".apk", ".deb", ".rpm", ".mp3", ".mp4", ".avi", ".mkv", ".mov",
        ".wav", ".ogg", ".webm", ".css", ".js", ".json", ".xml", ".rss", ".atom", ".woff", ".woff2", ".ttf", ".tar",
        ".jar", ".bin", ".img", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".txt", ".csv", ".sig", ".asc",
    };
    for (size_t i = 0; i < sizeof skip_ext / sizeof *skip_ext; i++) if (ends_with(path, pl, skip_ext[i])) return false;
    static const char *const traps[] = { "login", "logout", "signin", "signup", "sign_in", "register", "/edit", "/cart", "/search" };
    for (size_t i = 0; i < sizeof traps / sizeof *traps; i++)
        if (strcasestr(path, traps[i]) && (size_t)(strcasestr(path, traps[i]) - path) < pl) return false;
    /* Wikipedia & co: articles of the English sites only, no Special: / Talk: ... pages */
    size_t hl = (size_t)(path - host);
    if (ends_with(host, (size_t)(path - host), "wikidata.org")) return false;     /* data items, not articles */
    static const char *const wikis[] = { ".wikipedia.org", ".wiktionary.org", ".wikibooks.org", ".wikiquote.org",
                                         ".wikinews.org", ".wikivoyage.org", ".wikisource.org", ".wikiversity.org" };
    for (size_t i = 0; i < sizeof wikis / sizeof *wikis; i++)
        if (ends_with(host, hl, wikis[i])) {
            size_t ll = strcspn(host, ".");
            if (!(ll == 2 && !strncmp(host, "en", 2)) && !(ll == 3 && !strncmp(host, "www", 3))) return false;
            if (!strncmp(path, "/wiki/", 6) && memchr(path + 6, ':', pl > 6 ? pl - 6 : 0)) return false;
            if (strncmp(path, "/wiki/", 6) && pl > 1) return false;
        }
    return true;
}

static void front_add(const char *url, const char *anchor, uint8_t depth) {
    if (nfront >= FRONT_MAX) return;
    char origin[96];
    if (!origin_of(url, origin, sizeof origin, NULL, 0)) return;
    uint32_t hh = hash_s(origin);
    if (qcount[hh & 1023] >= HOST_QUEUE) return;
    shost_t *h = host_find(hh);
    if (h && (h->pages >= HOST_PAGES || h->fails >= 5)) return;
    if (!front) { front = calloc(FRONT_MAX, sizeof *front); if (!front) return; }
    fent_t *f = &front[nfront];
    f->url = strdup(url);
    f->anchor = strdup(anchor ? anchor : "");
    if (!f->url || !f->anchor) { free(f->url); free(f->anchor); return; }
    f->hhash = hh;
    f->depth = depth;
    qcount[hh & 1023]++;
    nfront++;
}

static void front_drop(int i) {
    if (qcount[front[i].hhash & 1023]) qcount[front[i].hhash & 1023]--;
    free(front[i].url);
    free(front[i].anchor);
    memmove(&front[i], &front[i + 1], sizeof *front * (size_t)(nfront - i - 1));
    nfront--;
}

static void enqueue(const char *url, const char *anchor, uint8_t depth) {
    if (!crawlable(url, false)) return;
    uint32_t key = srch_url_key(url);
    if (sidx_find_key(key) >= 0 || !seen_add(key)) return;
    front_add(url, anchor, depth);
}

static void seed(void) {
    for (size_t i = 0; i < sizeof seeds / sizeof *seeds; i++) {
        uint32_t key = srch_url_key(seeds[i]);
        if (sidx_find_key(key) >= 0) continue;
        seen_add(key);
        front_add(seeds[i], "", 0);
    }
}

/* ---- reading pages ----------------------------------------------------------------------- */
static void job_free(job_t *j) {
    srch_page_free(&j->p);
    free(j->html);
    free(j->url);
    memset(j, 0, sizeof *j);
}

static void job_add(char *html, uint32_t len, const char *url, uint8_t depth, uint8_t flags, bool lv) {
    job_t *j = NULL;
    for (int i = 0; i < JOB_MAX && !j; i++) if (!jobs[i].used) j = &jobs[i];
    if (!j) {
        /* full: a crawled page makes room (not one the user visited) */
        for (int i = 0; i < JOB_MAX && !j; i++) if (!(jobs[i].flags & SD_VISITED) && !jobs[i].live) j = &jobs[i];
        if (!j) j = &jobs[0];
        job_free(j);
    }
    j->url = strdup(url);
    if (!j->url) { free(html); return; }
    j->used = true;
    j->html = html;
    j->len = len;
    j->depth = depth;
    j->flags = flags;
    j->live = lv;
    srch_page_init(&j->p, html, len);
}

static bool same_site(const char *a, const char *b) {
    const char *x = strstr(a, "://"), *y = strstr(b, "://");
    if (!x || !y) return false;
    x += 3;
    y += 3;
    size_t lx = strcspn(x, "/?#"), ly = strcspn(y, "/?#");
    return lx == ly && !strncasecmp(x, y, lx);
}

static void job_finish(job_t *j) {
    srch_page_t *p = &j->p;
    char *base = malloc(B_URL_MAX), *abs = malloc(B_URL_MAX);
    uint32_t *keys = malloc(sizeof *keys * SRCH_LINKS_MAX);
    int nk = 0;
    /* the page's own name for itself (Wikipedia's redirects, ?utm= copies ...) */
    if (base && abs && p->canon[0] && url_resolve(j->url, p->canon, abs, B_URL_MAX)) {
        abs[strcspn(abs, "#")] = 0;
        if (same_site(abs, j->url) && (!strncasecmp(abs, "http://", 7) || !strncasecmp(abs, "https://", 8))) {
            char *u = strdup(abs);
            if (u) { free(j->url); j->url = u; seen_add(srch_url_key(u)); }
        }
    }
    srch_page_url_words(p, j->url);
    if (base && abs && keys) {
        strlcpy(base, j->url, B_URL_MAX);
        if (p->base[0] && url_resolve(j->url, p->base, abs, B_URL_MAX)) strlcpy(base, abs, B_URL_MAX);
        for (int i = 0; i < p->nlinks && nk < SRCH_LINKS_MAX; i++) {
            if (!url_resolve(base, p->links[i].href, abs, B_URL_MAX)) continue;
            abs[strcspn(abs, "#")] = 0;
            if (strncasecmp(abs, "http://", 7) && strncasecmp(abs, "https://", 8)) continue;
            keys[nk++] = srch_url_key(abs);
            if (!p->nofollow && !p->noindex && j->depth < MAX_DEPTH) enqueue(abs, p->links[i].anchor, (uint8_t)(j->depth + 1));
        }
    }
    int d = sidx_add(p, j->url, keys, nk, j->flags, sys_time());
    klog("search: %s %s (%u words, %d links)%s", d >= 0 ? "indexed" : "skipped", j->url, (unsigned)p->words, p->nlinks,
         j->flags & SD_VISITED ? " visited" : j->live ? " live" : "");
    free(base);
    free(abs);
    free(keys);
    if (live.active && d >= 0) live.gen++;
    job_free(j);
}

/* one slice of the oldest page waiting (visited and live pages first) */
static bool read_some(void) {
    job_t *j = NULL;
    for (int i = 0; i < JOB_MAX; i++)
        if (jobs[i].used && (!j || ((jobs[i].flags & SD_VISITED) || jobs[i].live) > ((j->flags & SD_VISITED) || j->live))) j = &jobs[i];
    if (!j) return false;
    if (srch_page_feed(&j->p, SLICE)) job_finish(j);
    return true;
}

/* ---- requests ---------------------------------------------------------------------------- */
bool browser_host_allowed(const char *url) __attribute__((weak));
static bool cert_ok_to_skip(const char *url) { return insecure || (browser_host_allowed && browser_host_allowed(url)); }

static void req_done(http_request_t *r, void *ctx);
static void req_progress(http_request_t *r, void *ctx);
static void live_cand_done(const char *url);

static int req_count(bool lv) {
    int n = 0;
    for (int i = 0; i < REQ_MAX; i++) if (reqs[i].r && reqs[i].live == lv) n++;
    return n;
}

static bool req_start(int kind, shost_t *h, const char *url, uint8_t depth, bool lv) {
    creq_t *c = NULL;
    for (int i = 0; i < REQ_MAX && !c; i++) if (!reqs[i].r) c = &reqs[i];
    if (!c) return false;
    memset(c, 0, sizeof *c);
    c->url = strdup(url);
    if (!c->url) return false;
    c->kind = kind;
    c->host = (int)(h - hosts);
    c->depth = depth;
    c->live = lv;
    http_options_t o;
    memset(&o, 0, sizeof o);
    o.headers = UA_HEADER;
    o.flags = HTTP_F_NO_COOKIES | (cert_ok_to_skip(url) ? HTTP_F_ALLOW_BAD_CERT : 0);
    o.max_body = kind == Q_ROBOTS ? 256u << 10 : MAX_BODY;
    o.timeout_ms = 20000;
    if (kind == Q_PAGE) o.on_progress = req_progress;
    c->r = http_fetch_ex(url, &o, req_done, c);
    if (!c->r) { free(c->url); c->url = NULL; return false; }
    h->busy = true;
    if (kind == Q_ROBOTS) h->robots = R_FETCHING;
    return true;
}

static void req_end(creq_t *c) {
    shost_t *h = &hosts[c->host];
    h->busy = false;
    h->next_ms = sys_uptime_ms() + (c->live ? LIVE_DELAY : h->delay_ms);
    if (c->r) http_free(c->r);
    free(c->url);
    memset(c, 0, sizeof *c);
}

static bool is_html(const char *ct) {
    if (!ct) return false;
    while (*ct == ' ') ct++;
    return !strncasecmp(ct, "text/html", 9) || !strncasecmp(ct, "application/xhtml", 17);
}

static void req_progress(http_request_t *r, void *ctx) {
    creq_t *c = ctx;
    if (c->checked || http_status(r) <= 0) return;
    c->checked = true;
    if (http_status(r) != 200 || !is_html(http_header(r, "Content-Type"))) {
        /* not a page: stop downloading it */
        if (c->live) live_cand_done(c->url);
        hosts[c->host].pages++;
        http_cancel(r);
        req_end(c);
    }
}

static void live_cand_done(const char *url) {
    for (int i = 0; i < live.ncand; i++)
        if (live.cand[i].url && !strcmp(live.cand[i].url, url)) live.cand[i].state = 2;
}

static void req_done(http_request_t *r, void *ctx) {
    creq_t *c = ctx;
    shost_t *h = &hosts[c->host];
    int err = http_error(r), st = http_status(r);
    if (c->kind == Q_ROBOTS) {
        if (err == HTTP_OK && st == 200) {
            robots_parse(h, (const char *)http_body(r), http_body_len(r));
            h->robots = R_READY;
        } else if (err == HTTP_OK && st >= 400 && st < 500) {
            h->robots = R_READY;               /* no robots.txt: everything is allowed */
        } else {
            h->robots = R_FAILED;              /* a server or network error: not now */
            h->retry_ms = sys_uptime_ms() + 600000;
            h->fails++;
            klog("search: robots.txt of %s: %s", h->origin, err != HTTP_OK ? http_error_text(r) : "server error");
        }
        req_end(c);
        return;
    }
    if (c->live) live_cand_done(c->url);
    h->pages++;
    if (err != HTTP_OK) {
        h->fails += err == HTTP_ERR_CERT ? 5 : 1;
        klog("search: %s: %s", c->url, http_error_text(r));
    } else if (st == 200 && is_html(http_header(r, "Content-Type"))) {
        const char *final = http_url(r);
        seen_add(srch_url_key(final));
        uint32_t len = 0;
        uint8_t *body = http_take_body(r, &len);
        if (body) {
            size_t n = 0;
            int cs = web_detect_charset((const char *)body, len, http_header(r, "Content-Type"));
            char *html = web_to_1252((const char *)body, len, cs, &n);
            free(body);
            if (html) job_add(html, (uint32_t)n, final, c->depth, c->depth == 0 && !c->live ? SD_SEED : 0, c->live);
        }
    }
    req_end(c);
}

static void cancel_all(void) {
    for (int i = 0; i < REQ_MAX; i++)
        if (reqs[i].r) {
            http_free(reqs[i].r);
            reqs[i].r = NULL;
            req_end(&reqs[i]);
        }
}

/* ---- crawling ---------------------------------------------------------------------------- */
static void crawl(uint32_t now) {
    if ((int32_t)(now - next_crawl) < 0 || req_count(false) >= CRAWL_REQS) return;
    /* the pages fetched are read first */
    for (int i = 0; i < JOB_MAX; i++) if (jobs[i].used && !jobs[i].live && !(jobs[i].flags & SD_VISITED)) return;
    next_crawl = now + 1000;
    if (!nfront) return;
    char path[512];
    bool robots_asked = false;
    for (int i = 0; i < nfront && i < 300; i++) {
        fent_t *f = &front[i];
        shost_t *h = host_find(f->hhash);
        if (!h || h->robots == R_NONE || (h->robots == R_FAILED && (int32_t)(now - h->retry_ms) >= 0)) {
            if (robots_asked) continue;
            robots_asked = true;
            if (!h) h = host_get(f->url);
            if (!h || h->busy) continue;
            if (h->fails >= 5) { front_drop(i--); continue; }
            char rurl[160];
            snprintf(rurl, sizeof rurl, "%s/robots.txt", h->origin);
            if (req_start(Q_ROBOTS, h, rurl, 0, false)) { next_crawl = now + 300; return; }
            continue;
        }
        if (h->robots != R_READY || h->busy || (int32_t)(now - h->next_ms) < 0) continue;
        char origin[96];
        if (h->fails >= 5 || h->pages >= HOST_PAGES || !origin_of(f->url, origin, sizeof origin, path, sizeof path) ||
            !robots_ok(h, path)) {
            front_drop(i--);
            continue;
        }
        h->used = ++use_clock;
        if (req_start(Q_PAGE, h, f->url, f->depth, false)) {
            front_drop(i);
            next_crawl = now + 400;
        }
        return;
    }
}

/* ---- live discovery ---------------------------------------------------------------------- */
static void live_add(const char *url) {
    if (live.ncand >= LIVE_MAX) return;
    uint32_t key = srch_url_key(url);
    if (sidx_find_key(key) >= 0) return;
    for (int i = 0; i < live.ncand; i++) if (srch_url_key(live.cand[i].url) == key) return;
    live.cand[live.ncand].url = strdup(url);
    if (live.cand[live.ncand].url) live.cand[live.ncand++].state = 0;
}

static void live_reset(void) {
    for (int i = 0; i < live.ncand; i++) free(live.cand[i].url);
    live.ncand = 0;
    live.active = false;
}

/* Wikipedia's title for the query: "linux kernel" -> "Linux_kernel" (all words: "Linux_Kernel") */
static void wiki_url(const char *q, bool all_words, char *out, size_t size) {
    char t[160];
    size_t n = 0;
    bool start = true;
    for (const char *s = q; *s && n < sizeof t - 1; s++) {
        char c = *s;
        if (c == ' ') { if (n && t[n - 1] != '_') t[n++] = '_'; start = true; continue; }
        if (start && (n == 0 || all_words) && c >= 'a' && c <= 'z') c = (char)(c - 32);
        start = false;
        t[n++] = c;
    }
    while (n && t[n - 1] == '_') n--;
    t[n] = 0;
    strlcpy(out, "https://en.wikipedia.org/wiki/", size);
    web_urlencode(t, out, size);
}

static void live_start(const char *text, const srch_query_t *q) {
    live_reset();
    live.active = true;
    strlcpy(live.text, text, sizeof live.text);
    live.q = *q;
    live.start = sys_uptime_ms();
    char url[B_URL_MAX];
    wiki_url(text, false, url, sizeof url);
    live_add(url);
    if (strchr(text, ' ')) { wiki_url(text, true, url, sizeof url); live_add(url); }
    size_t jl = strlen(q->joined);
    if (q->n == 1 && jl >= 3 && jl <= 30) {
        snprintf(url, sizeof url, "https://www.%s.com/", q->joined);
        live_add(url);
        snprintf(url, sizeof url, "https://%s.org/", q->joined);
        live_add(url);
    }
    /* links waiting to be crawled whose text or address has all the words */
    for (int i = 0; i < nfront && live.ncand < LIVE_MAX; i++) {
        char hay[400];
        snprintf(hay, sizeof hay, "%s %s", front[i].anchor, front[i].url);
        uint32_t pos = 0, n = (uint32_t)strlen(hay);
        char w[SRCH_WORD_MAX + 1];
        unsigned found = 0;
        while (srch_next_word(hay, n, &pos, w))
            for (int k = 0; k < q->n; k++) if (!strcmp(w, q->w[k])) found |= 1u << k;
        if (q->n && found == (1u << q->n) - 1) {
            live_add(front[i].url);
            front_drop(i--);
        }
    }
    klog("search: looking on the web for \"%s\" (%d addresses)", text, live.ncand);
    live.gen++;
}

static void live_pump(uint32_t now) {
    bool waiting = false;
    char path[512], origin[96];
    for (int i = 0; i < live.ncand; i++) {
        lcand_t *lc = &live.cand[i];
        if (lc->state == 1) waiting = true;
        if (lc->state) continue;
        waiting = true;
        if (req_count(true) >= LIVE_REQS) continue;
        shost_t *h = host_get(lc->url);
        if (!h || h->fails >= 5 || h->robots == R_FAILED) { lc->state = 2; continue; }
        if (h->busy) continue;
        if (h->robots == R_NONE) {
            char rurl[160];
            snprintf(rurl, sizeof rurl, "%s/robots.txt", h->origin);
            req_start(Q_ROBOTS, h, rurl, 0, true);
            continue;
        }
        if (h->robots != R_READY || (int32_t)(now - h->next_ms) < 0) continue;
        if (!origin_of(lc->url, origin, sizeof origin, path, sizeof path) || !robots_ok(h, path)) { lc->state = 2; continue; }
        if (req_start(Q_PAGE, h, lc->url, 1, true)) lc->state = 1;
    }
    bool jobs_live = false;
    for (int i = 0; i < JOB_MAX; i++) if (jobs[i].used && jobs[i].live) jobs_live = true;
    if ((!waiting && !jobs_live && !req_count(true)) || (int32_t)(now - live.start) > LIVE_MS) {
        live.active = false;
        live.gen++;
        klog("search: done looking for \"%s\"", live.text);
    }
}

/* ---- saving ------------------------------------------------------------------------------ */
static void save_all(void) {
    uint32_t t0 = sys_uptime_ms();
    if (!vfs_is_dir(SDIR)) vfs_mkdirs(SDIR);
    bool ok = sidx_save(IDX_PATH);
    char *b = NULL;
    size_t len = 0, cap = 0;
    char line[48];
    for (int i = 0; i < nfront && i < FRONT_SAVE; i++) {
        snprintf(line, sizeof line, "%u\t", (unsigned)front[i].depth);
        bhtml_add(&b, &len, &cap, line);
        bhtml_add(&b, &len, &cap, front[i].url);
        bhtml_add(&b, &len, &cap, "\t");
        bhtml_add(&b, &len, &cap, front[i].anchor);
        bhtml_add(&b, &len, &cap, "\n");
    }
    vfs_write_file(FRONT_PATH, b ? b : "", (uint32_t)len);
    free(b);
    saved_gen = sidx_generation();
    last_save = sys_uptime_ms();
    klog("search: saved %d pages (%u KB)%s in %u ms", sidx_count(), sidx_bytes() / 1024, ok ? "" : " - FAILED",
         (unsigned)(last_save - t0));
}

static void load_frontier(void) {
    uint32_t size = 0;
    char *d = vfs_read_file(FRONT_PATH, &size);
    if (!d) return;
    char *save = NULL;
    for (char *ln = strtok_r(d, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        char *t1 = strchr(ln, '\t');
        if (!t1) continue;
        *t1 = 0;
        char *url = t1 + 1, *t2 = strchr(url, '\t');
        if (t2) *t2 = 0;
        uint32_t key = srch_url_key(url);
        if (!crawlable(url, true) || sidx_find_key(key) >= 0 || !seen_add(key)) continue;
        front_add(url, t2 ? t2 + 1 : "", (uint8_t)atoi(ln));
    }
    free(d);
}

static void start_up(void) {
    if (started) return;
    started = true;
    uint32_t t0 = sys_uptime_ms();
    const char *v = sys_cmdline_get("search_allow_bad_certs");
    insecure = v && *v == '1';
    sidx_set_cap(persist_active() ? CAP_DISK : CAP_RAM);
    sidx_load(IDX_PATH);
    load_frontier();
    if (!nfront) seed();
    saved_gen = sidx_generation();
    last_save = sys_uptime_ms();
    klog("search: %d pages in the index (%u KB), %d addresses waiting, ready in %u ms", sidx_count(),
         sidx_bytes() / 1024, nfront, (unsigned)(last_save - t0));
}

/* ---- the browser side -------------------------------------------------------------------- */
void search_tick(bool busy) {
    uint32_t now = sys_uptime_ms();
    if (!started) {
        if (!first_tick) first_tick = now ? now : 1;
        if ((int32_t)(now - first_tick) < 1500) return;     /* after the first page is up */
        start_up();
    }
    if (!read_some()) sidx_rank_step();
    uint32_t took = sys_uptime_ms() - now;
    static uint32_t slow_logged;
    if (took > 40 && (int32_t)(now - slow_logged) > 10000) {
        slow_logged = now;
        klog("search: a timer tick took %u ms", (unsigned)took);
    }
    if (pending[0] && (net_is_up() || (int32_t)(now - pending_until) >= 0)) {
        if (net_is_up()) {
            srch_query_t q;
            srch_query_parse(pending, &q);
            live_start(pending, &q);
        }
        pending[0] = 0;
    }
    if (live.active) live_pump(now);
    if (!busy && net_is_up()) crawl(now);
    if (sidx_generation() != saved_gen && (int32_t)(now - last_save) >= SAVE_MS) save_all();
}

void search_browser_closed(void) {
    if (!started) return;
    cancel_all();
    while (read_some()) {}
    live_reset();
    if (sidx_generation() != saved_gen) save_all();
}

void search_clear(void) {
    start_up();
    cancel_all();
    for (int i = 0; i < JOB_MAX; i++) if (jobs[i].used) job_free(&jobs[i]);
    live_reset();
    sidx_clear();
    while (nfront) front_drop(nfront - 1);
    memset(qcount, 0, sizeof qcount);
    for (int i = 0; i < nhosts; i++) { hosts[i].pages = 0; hosts[i].fails = 0; }
    if (seen) memset(seen, 0, sizeof *seen * seencap);
    nseen = 0;
    vfs_remove(IDX_PATH);
    vfs_remove(FRONT_PATH);
    seed();
    saved_gen = sidx_generation();
    live.gen++;
    klog("search: index cleared");
}

int search_page_count(void) {
    start_up();
    return sidx_count();
}

uint32_t search_live_generation(void) { return live.gen; }

bool search_is_page(const char *url) {
    return url && !strncasecmp(url, SEARCH_PAGE, strlen(SEARCH_PAGE)) &&
           (!url[strlen(SEARCH_PAGE)] || url[strlen(SEARCH_PAGE)] == '?' || url[strlen(SEARCH_PAGE)] == '/');
}

void search_page_visited(const char *url, const char *body, uint32_t len, const char *ctype) {
    if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)) return;
    start_up();
    if (len > MAX_BODY) len = MAX_BODY;
    size_t n = 0;
    char *html = web_to_1252(body, len, web_detect_charset(body, len, ctype), &n);
    if (!html) return;
    char u[B_URL_MAX];
    strlcpy(u, url, sizeof u);
    u[strcspn(u, "#")] = 0;
    seen_add(srch_url_key(u));
    job_add(html, (uint32_t)n, u, 0, SD_VISITED, false);
}

/* ---- the results page -------------------------------------------------------------------- */
static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 32;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* the value of parameter `name` in the address, decoded (UTF-8 -> 1252) */
static void param(const char *url, const char *name, char *out, size_t size) {
    out[0] = 0;
    const char *q = strchr(url, '?');
    size_t nl = strlen(name);
    while (q) {
        q++;
        if (!strncmp(q, name, nl) && q[nl] == '=') {
            const char *v = q + nl + 1;
            char raw[600];
            size_t n = 0;
            for (; *v && *v != '&' && *v != '#' && n < sizeof raw - 1; v++) {
                if (*v == '+') raw[n++] = ' ';
                else if (*v == '%' && hexv(v[1]) >= 0 && hexv(v[2]) >= 0) { raw[n++] = (char)(hexv(v[1]) * 16 + hexv(v[2])); v += 2; }
                else raw[n++] = *v;
            }
            size_t on = 0;
            char *c = web_to_1252(raw, n, WEB_CS_AUTO, &on);
            if (!c) return;
            /* one line, single spaces */
            size_t o = 0;
            for (size_t i = 0; i < on && o + 1 < size; i++) {
                char ch = (unsigned char)c[i] < ' ' ? ' ' : c[i];
                if (ch == ' ' && (!o || out[o - 1] == ' ')) continue;
                out[o++] = ch;
            }
            while (o && out[o - 1] == ' ') o--;
            out[o] = 0;
            free(c);
            return;
        }
        q = strchr(q, '&');
    }
}

static const char page_style[] =
    "body{margin:0;font-family:sans-serif;color:#202124;background:#fff}"
    ".top{display:flex;align-items:center;padding:16px 28px;border-bottom:1px solid #e3e6ea}"
    ".logo{font-size:23px;font-weight:bold;color:#1a56c9;text-decoration:none;margin-right:22px;white-space:nowrap}"
    ".logo span{color:#e37400}"
    "form{display:flex;flex:1;max-width:600px}"
    ".q{flex:1;font-size:16px;padding:7px 14px;border:1px solid #c4c9d0;border-radius:20px}"
    ".go{flex:none;width:86px;height:34px;margin-left:8px;font-size:15px;border:0;border-radius:17px;background:#1a56c9;color:#fff}"
    ".main{padding:6px 28px 30px 28px;max-width:720px}"
    ".stats{color:#70757a;font-size:13px;margin:12px 0 18px 0}"
    ".live{color:#1a56c9;font-size:13px;margin:-8px 0 16px 0}"
    ".r{margin:0 0 22px 0}"
    ".r a{font-size:19px;color:#1a0dab;text-decoration:none}"
    ".u{color:#0d652d;font-size:13px;margin:3px 0}"
    ".s{color:#4d5156;font-size:14px;line-height:1.45}"
    ".pages{margin:26px 0 10px 0;font-size:15px}"
    ".pages a,.pages b{margin-right:14px}.pages a{color:#1a0dab}"
    ".none p{font-size:15px;line-height:1.5}"
    ".home{text-align:center;padding-top:80px}"
    ".home .logo{font-size:40px;margin:0}"
    ".home form{margin:28px auto 0 auto;max-width:560px}"
    ".foot{color:#70757a;font-size:12px;border-top:1px solid #e3e6ea;padding:14px 28px}";

static void add_box(char **b, size_t *len, size_t *cap, const char *q) {
    bhtml_add(b, len, cap, "<form action=\"" SEARCH_PAGE "\" method=\"get\"><input class=q type=\"text\" name=\"q\" size=\"40\" value=\"");
    bhtml_escape(b, len, cap, q, strlen(q));
    bhtml_add(b, len, cap, "\" placeholder=\"Search with NovaOS Search\"><input class=go type=\"submit\" value=\"Search\"></form>");
}

static void add_footer(char **b, size_t *len, size_t *cap) {
    char tmp[200];
    snprintf(tmp, sizeof tmp, "<div class=foot>NovaOS Search \x95 %d page%s in the index (%u KB) \x95 %d address%s waiting to be crawled \x95 "
             "its own crawler, index and ranking</div>", sidx_count(), sidx_count() == 1 ? "" : "s", sidx_bytes() / 1024,
             nfront, nfront == 1 ? "" : "es");
    bhtml_add(b, len, cap, tmp);
}

/* "en.wikipedia.org > wiki > Linux_kernel" */
static void pretty_url(const char *url, char *out, size_t size) {
    const char *s = strstr(url, "://");
    s = s ? s + 3 : url;
    size_t o = 0;
    for (; *s && *s != '?' && *s != '#' && o + 4 < size; s++) {
        if (*s == '/') {
            if (!s[1] || s[1] == '?' || s[1] == '#') break;
            memcpy(out + o, " \x9b ", 3);
            o += 3;
        } else out[o++] = *s;
    }
    out[o] = 0;
    if (o > 90) strlcpy(out + 87, "...", size - 87);
}

char *search_page_html(const char *url) {
    start_up();
    char q[400], pg[16];
    param(url, "q", q, sizeof q);
    param(url, "p", pg, sizeof pg);
    int page = atoi(pg);
    if (page < 1) page = 1;
    char *b = NULL;
    size_t len = 0, cap = 0;
    char tmp[1024];
    bhtml_add(&b, &len, &cap, "<!doctype html><html><head><title>");
    if (q[0]) { bhtml_escape(&b, &len, &cap, q, strlen(q)); bhtml_add(&b, &len, &cap, " - "); }
    bhtml_add(&b, &len, &cap, "NovaOS Search</title><style>");
    bhtml_add(&b, &len, &cap, page_style);
    bhtml_add(&b, &len, &cap, "</style></head><body>");
    if (!q[0]) {
        bhtml_add(&b, &len, &cap, "<div class=home><a class=logo href=\"" SEARCH_PAGE "\">NovaOS <span>Search</span></a>");
        add_box(&b, &len, &cap, "");
        snprintf(tmp, sizeof tmp, "<p class=stats>%d page%s in the index. It grows while the browser is open: NovaOS Search "
                 "crawls the web itself and adds the pages you visit.</p></div>", sidx_count(), sidx_count() == 1 ? "" : "s");
        bhtml_add(&b, &len, &cap, tmp);
        add_footer(&b, &len, &cap);
        bhtml_add(&b, &len, &cap, "</body></html>");
        return b;
    }
    srch_query_t qq;
    srch_query_parse(q, &qq);
    srch_hit_t *hits = malloc(sizeof *hits * MAX_HITS);
    int total = 0, n = 0;
    uint32_t t0 = sys_uptime_ms();
    if (hits) n = sidx_query(&qq, hits, MAX_HITS, &total);
    uint32_t ms = sys_uptime_ms() - t0;
    /* few results: look on the web too (once per query, again after two minutes) */
    bool same = !strcmp(live.text, q);
    if (total < 5 && qq.n && (!same || (!live.active && (int32_t)(sys_uptime_ms() - live.start) > 120000))) {
        if (net_is_up()) live_start(q, &qq);
        else {
            /* the network may still be starting (DHCP): look as soon as it is up */
            strlcpy(pending, q, sizeof pending);
            pending_until = sys_uptime_ms() + 20000;
        }
    }
    bool looking = live.active && !strcmp(live.text, q);
    bhtml_add(&b, &len, &cap, "<div class=top><a class=logo href=\"" SEARCH_PAGE "\">NovaOS <span>Search</span></a>");
    add_box(&b, &len, &cap, q);
    bhtml_add(&b, &len, &cap, "</div><div class=main>");
    if (total) {
        snprintf(tmp, sizeof tmp, "<div class=stats>%s%d result%s (%u.%02u seconds)</div>", total > PER_PAGE ? "About " : "",
                 total, total == 1 ? "" : "s", (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 10));
        bhtml_add(&b, &len, &cap, tmp);
    }
    if (looking) bhtml_add(&b, &len, &cap, "<div class=live>Searching the web for more results... this page updates by itself.</div>");
    int first = (page - 1) * PER_PAGE;
    for (int i = first; i < n && i < first + PER_PAGE; i++) {
        const srch_doc_t *d = sidx_doc(hits[i].doc);
        bhtml_add(&b, &len, &cap, "<div class=r><a href=\"");
        bhtml_escape(&b, &len, &cap, d->url, strlen(d->url));
        bhtml_add(&b, &len, &cap, "\">");
        const char *title = d->title[0] ? d->title : d->url;
        bhtml_escape(&b, &len, &cap, title, strlen(title));
        bhtml_add(&b, &len, &cap, "</a><div class=u>");
        pretty_url(d->url, tmp, sizeof tmp);
        bhtml_escape(&b, &len, &cap, tmp, strlen(tmp));
        bhtml_add(&b, &len, &cap, "</div><div class=s>");
        sidx_snippet(hits[i].doc, &qq, tmp, sizeof tmp);
        bhtml_add(&b, &len, &cap, tmp);
        bhtml_add(&b, &len, &cap, "</div></div>");
    }
    if (!total) {
        bhtml_add(&b, &len, &cap, "<div class=none><p>No results for <b>");
        bhtml_escape(&b, &len, &cap, q, strlen(q));
        bhtml_add(&b, &len, &cap, "</b>.</p>");
        if (looking) bhtml_add(&b, &len, &cap, "<p>NovaOS Search is looking for it on the web now.</p>");
        else if (!net_is_up()) bhtml_add(&b, &len, &cap, "<p>You are offline, so only pages already in the index can be found.</p>");
        snprintf(tmp, sizeof tmp, "<p>Check the spelling, or try other or fewer words. The index is small (%d pages) and grows "
                 "while the browser is open.</p></div>", sidx_count());
        bhtml_add(&b, &len, &cap, tmp);
    }
    int pages = (n + PER_PAGE - 1) / PER_PAGE;
    if (pages > 1) {
        char enc[900] = "";
        web_urlencode(q, enc, sizeof enc);
        bhtml_add(&b, &len, &cap, "<div class=pages>");
        if (page > 1) {
            snprintf(tmp, sizeof tmp, "<a href=\"" SEARCH_PAGE "?q=%s&amp;p=%d\">Previous</a>", enc, page - 1);
            bhtml_add(&b, &len, &cap, tmp);
        }
        for (int p = 1; p <= pages; p++) {
            if (p == page) snprintf(tmp, sizeof tmp, "<b>%d</b>", p);
            else snprintf(tmp, sizeof tmp, "<a href=\"" SEARCH_PAGE "?q=%s&amp;p=%d\">%d</a>", enc, p, p);
            bhtml_add(&b, &len, &cap, tmp);
        }
        if (page < pages) {
            snprintf(tmp, sizeof tmp, "<a href=\"" SEARCH_PAGE "?q=%s&amp;p=%d\">Next</a>", enc, page + 1);
            bhtml_add(&b, &len, &cap, tmp);
        }
        bhtml_add(&b, &len, &cap, "</div>");
    }
    bhtml_add(&b, &len, &cap, "</div>");
    add_footer(&b, &len, &cap);
    bhtml_add(&b, &len, &cap, "</body></html>");
    free(hits);
    return b;
}
