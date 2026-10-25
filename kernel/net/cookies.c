/*
 * cookies.c - the cookie jar (RFC 6265).
 *
 * Set-Cookie headers from responses are stored per site (host-only or domain
 * cookies, path, Secure, Expires / Max-Age); http.c asks for the Cookie header of
 * every request. Persistent cookies (with an expiry date) are saved to
 * /home/user/.cookies two seconds after a change and loaded on first use, so they
 * survive restarts on an installed system (the file system is saved to disk).
 * Session cookies live until shut down.
 */
#include "net_internal.h"

#define COOKIE_FILE "/home/user/.cookies"
#define MAX_COOKIES 1000
#define MAX_PER_DOMAIN 60

typedef struct cookie {
    struct cookie *next;
    char    *name, *value, *domain, *path;
    uint32_t expires;          /* unix time; 0 = session cookie */
    uint32_t created;
    bool     host_only, secure, http_only;
} cookie_t;

static cookie_t *jar;
static int njar;
static bool loaded, dirty;
static int save_timer;

static void cookie_free(cookie_t *c) {
    kfree(c->name);
    kfree(c->value);
    kfree(c->domain);
    kfree(c->path);
    kfree(c);
}

static char *dupn(const char *s, size_t n) {
    char *p = kmalloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static bool domain_match(const char *host, const char *domain) {
    if (!strcasecmp(host, domain)) return true;
    size_t hl = strlen(host), dl = strlen(domain);
    return hl > dl && host[hl - dl - 1] == '.' && !strcasecmp(host + hl - dl, domain);
}

static bool path_match(const char *req, const char *cpath) {
    size_t cl = strlen(cpath);
    if (strncmp(req, cpath, cl)) return false;
    return req[cl] == 0 || req[cl] == '/' || req[cl] == '?' || cpath[cl - 1] == '/';
}

static void save_now(void *ctx) {
    (void)ctx;
    save_timer = 0;
    if (!dirty) return;
    dirty = false;
    uint32_t now = sys_time();
    size_t cap = 256, len = 0;
    for (cookie_t *c = jar; c; c = c->next)
        cap += strlen(c->name) + strlen(c->value) + strlen(c->domain) + strlen(c->path) + 48;
    char *buf = kmalloc(cap);
    if (!buf) return;
    len += (size_t)snprintf(buf, cap, "# NovaOS cookies: domain host-only path secure httponly expires name value\n");
    int n = 0;
    for (cookie_t *c = jar; c; c = c->next) {
        if (!c->expires || c->expires <= now) continue;
        len += (size_t)snprintf(buf + len, cap - len, "%s\t%d\t%s\t%d\t%d\t%u\t%s\t%s\n", c->domain, c->host_only,
                                c->path, c->secure, c->http_only, c->expires, c->name, c->value);
        n++;
    }
    if (n == 0) { vfs_remove(COOKIE_FILE); kfree(buf); return; }
    if (vfs_is_dir("/home/user")) vfs_write_file(COOKIE_FILE, buf, (uint32_t)len);
    kfree(buf);
}

static void mark_dirty(void) {
    dirty = true;
    if (!save_timer) save_timer = timer_add(2000, save_now, NULL, false);
}

static void unlink_cookie(cookie_t **pp) {
    cookie_t *c = *pp;
    *pp = c->next;
    cookie_free(c);
    njar--;
}

static void add_cookie(cookie_t *nc) {
    /* replace a cookie with the same name / domain / path */
    for (cookie_t **pp = &jar; *pp; pp = &(*pp)->next) {
        cookie_t *c = *pp;
        if (!strcmp(c->name, nc->name) && !strcasecmp(c->domain, nc->domain) && !strcmp(c->path, nc->path)) {
            nc->created = c->created;
            unlink_cookie(pp);
            break;
        }
    }
    /* limits: oldest goes first */
    int per = 0;
    for (cookie_t *c = jar; c; c = c->next) if (!strcasecmp(c->domain, nc->domain)) per++;
    while (per >= MAX_PER_DOMAIN || njar >= MAX_COOKIES) {
        cookie_t **oldest = NULL;
        for (cookie_t **pp = &jar; *pp; pp = &(*pp)->next)
            if ((per < MAX_PER_DOMAIN || !strcasecmp((*pp)->domain, nc->domain)) &&
                (!oldest || (*pp)->created < (*oldest)->created)) oldest = pp;
        if (!oldest) break;
        if (!strcasecmp((*oldest)->domain, nc->domain)) per--;
        unlink_cookie(oldest);
    }
    nc->next = jar;
    jar = nc;
    njar++;
}

static void load(void) {
    if (loaded) return;
    loaded = true;
    uint32_t size;
    char *data = vfs_read_file(COOKIE_FILE, &size);
    if (!data) return;
    uint32_t now = sys_time();
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == '#') continue;
        char *f[8];
        int nf = 0;
        char *p = line;
        while (nf < 8) {
            f[nf++] = p;
            char *t = strchr(p, '\t');
            if (!t) break;
            *t = 0;
            p = t + 1;
        }
        if (nf != 8) continue;
        uint32_t exp = (uint32_t)strtoul(f[5], NULL, 10);
        if (exp <= now) continue;
        cookie_t *c = kcalloc(1, sizeof *c);
        if (!c) break;
        c->domain = dupn(f[0], strlen(f[0]));
        c->host_only = atoi(f[1]) != 0;
        c->path = dupn(f[2], strlen(f[2]));
        c->secure = atoi(f[3]) != 0;
        c->http_only = atoi(f[4]) != 0;
        c->expires = exp;
        c->name = dupn(f[6], strlen(f[6]));
        c->value = dupn(f[7], strlen(f[7]));
        c->created = now;
        if (!c->domain || !c->path || !c->name || !c->value) { cookie_free(c); continue; }
        add_cookie(c);
    }
    kfree(data);
}

void cookies_init(void) { load(); }

int cookies_count(void) {
    load();
    return njar;
}

void cookies_clear(void) {
    load();
    while (jar) unlink_cookie(&jar);
    njar = 0;
    dirty = false;
    vfs_remove(COOKIE_FILE);
    NLOG("cookies cleared");
}

/* ---- dates (RFC 6265 5.1.1, lenient) ------------------------------------------------------ */
static uint32_t parse_date(const char *s) {
    int day = -1, mon = -1, year = -1, hh = -1, mm = -1, ss = -1;
    static const char *months = "janfebmaraprmayjunjulaugsepoctnovdec";
    while (*s) {
        while (*s && !isalnum((unsigned char)*s) && *s != ':') s++;
        const char *t = s;
        while (*s && (isalnum((unsigned char)*s) || *s == ':')) s++;
        size_t n = (size_t)(s - t);
        if (!n) continue;
        int a, b, c;
        if (hh < 0 && memchr(t, ':', n) && sscanf(t, "%d:%d:%d", &a, &b, &c) == 3) { hh = a; mm = b; ss = c; continue; }
        if (day < 0 && n <= 2 && isdigit((unsigned char)t[0])) { day = atoi(t); continue; }
        if (mon < 0 && n >= 3 && isalpha((unsigned char)t[0])) {
            for (int i = 0; i < 12; i++)
                if (!strncasecmp(t, months + i * 3, 3)) { mon = i + 1; break; }
            if (mon > 0) continue;
        }
        if (year < 0 && n >= 2 && n <= 4 && isdigit((unsigned char)t[0])) {
            year = atoi(t);
            if (year < 70) year += 2000;
            else if (year < 100) year += 1900;
        }
    }
    if (day < 1 || day > 31 || mon < 1 || year < 1601 || hh < 0 || hh > 23 || mm > 59 || ss > 59) return 0;
    if (year < 1970) return 1;
    if (year > 2105) year = 2105;
    datetime_t dt = { 0 };
    dt.year = year;
    dt.month = mon;
    dt.day = day;
    dt.hour = hh;
    dt.minute = mm;
    dt.second = ss;
    return sys_datetime_to_time(&dt);
}

/* ---- Set-Cookie ----------------------------------------------------------------------------- */
static void trim(const char **s, const char **e) {
    while (*s < *e && isspace((unsigned char)**s)) (*s)++;
    while (*e > *s && isspace((unsigned char)(*e)[-1])) (*e)--;
}

void cookies_store(const url_t *u, const char *sc) {
    load();
    const char *end = sc + strcspn(sc, ";");
    const char *eq = memchr(sc, '=', (size_t)(end - sc));
    if (!eq) return;
    const char *ns = sc, *ne = eq, *vs = eq + 1, *ve = end;
    trim(&ns, &ne);
    trim(&vs, &ve);
    if (ne == ns || ne - ns > 256 || ve - vs > 4096) return;
    if (ve - vs >= 2 && *vs == '"' && ve[-1] == '"') { vs++; ve--; }
    for (const char *p = ns; p < ne; p++) if (*p == '\t' || *p == '\n') return;
    for (const char *p = vs; p < ve; p++) if (*p == '\t' || *p == '\n') return;

    char domain[256] = "", path[1024] = "";
    bool secure = false, http_only = false, have_maxage = false;
    long maxage = 0;
    uint32_t expires = 0;
    const char *a = end;
    while (*a == ';') {
        a++;
        const char *ae = a + strcspn(a, ";");
        const char *aeq = memchr(a, '=', (size_t)(ae - a));
        const char *ks = a, *ke = aeq ? aeq : ae, *avs = aeq ? aeq + 1 : ae, *ave = ae;
        trim(&ks, &ke);
        trim(&avs, &ave);
        size_t kl = (size_t)(ke - ks), vl = (size_t)(ave - avs);
        char val[256];
        size_t cl = MIN(vl, sizeof val - 1);
        memcpy(val, avs, cl);
        val[cl] = 0;
        if (kl == 7 && !strncasecmp(ks, "expires", 7)) { if (!have_maxage) expires = parse_date(val); }
        else if (kl == 7 && !strncasecmp(ks, "max-age", 7)) {
            char *ep;
            long v = strtol(val, &ep, 10);
            if (ep != val) { have_maxage = true; maxage = v; }
        } else if (kl == 6 && !strncasecmp(ks, "domain", 6) && vl) {
            const char *d = val;
            if (*d == '.') d++;
            strlcpy(domain, d, sizeof domain);
            for (char *p = domain; *p; p++) *p = (char)tolower((unsigned char)*p);
        } else if (kl == 4 && !strncasecmp(ks, "path", 4) && vl && val[0] == '/') {
            strlcpy(path, val, sizeof path);
        } else if (kl == 6 && !strncasecmp(ks, "secure", 6)) secure = true;
        else if (kl == 8 && !strncasecmp(ks, "httponly", 8)) http_only = true;
        a = ae;
    }
    uint32_t now = sys_time();
    bool remove = false;
    if (have_maxage) {
        if (maxage <= 0) remove = true;
        else expires = now + (uint32_t)MIN(maxage, 400L * 86400);
    } else if (expires) {
        if (expires <= now) remove = true;
        else if (expires > now + 400u * 86400) expires = now + 400u * 86400;
    }
    bool host_only = !domain[0];
    if (host_only) strlcpy(domain, u->host, sizeof domain);
    else {
        if (!domain_match(u->host, domain) || !strchr(domain, '.')) return;   /* foreign domain / a TLD */
    }
    if (!path[0]) {                                     /* default path: the request's directory */
        const char *q = u->path + strcspn(u->path, "?");
        const char *slash = NULL;
        for (const char *p = u->path; p < q; p++) if (*p == '/') slash = p;
        size_t pl = slash && slash != u->path ? (size_t)(slash - u->path) : 1;
        memcpy(path, u->path, MIN(pl, sizeof path - 1));
        path[MIN(pl, sizeof path - 1)] = 0;
    }
    if (secure && strcmp(u->scheme, "https")) return;  /* only secure sites set Secure cookies */
    if (remove) {
        for (cookie_t **pp = &jar; *pp; pp = &(*pp)->next) {
            cookie_t *c = *pp;
            if ((size_t)(ne - ns) == strlen(c->name) && !strncmp(c->name, ns, (size_t)(ne - ns)) &&
                !strcasecmp(c->domain, domain) && !strcmp(c->path, path)) {
                bool persistent = c->expires != 0;
                unlink_cookie(pp);
                if (persistent) mark_dirty();
                break;
            }
        }
        return;
    }
    cookie_t *c = kcalloc(1, sizeof *c);
    if (!c) return;
    c->name = dupn(ns, (size_t)(ne - ns));
    c->value = dupn(vs, (size_t)(ve - vs));
    c->domain = dupn(domain, strlen(domain));
    c->path = dupn(path, strlen(path));
    if (!c->name || !c->value || !c->domain || !c->path) { cookie_free(c); return; }
    c->expires = expires;
    c->created = now;
    c->host_only = host_only;
    c->secure = secure;
    c->http_only = http_only;
    add_cookie(c);
    if (expires) mark_dirty();
}

char *cookies_header_for(const url_t *u) {
    load();
    uint32_t now = sys_time();
    bool https = !strcmp(u->scheme, "https");
    /* collect matches, longest path first */
    cookie_t *m[64];
    int n = 0;
    for (cookie_t **pp = &jar; *pp;) {
        cookie_t *c = *pp;
        if (c->expires && c->expires <= now) { unlink_cookie(pp); mark_dirty(); continue; }
        pp = &c->next;
        if (c->host_only ? strcasecmp(u->host, c->domain) : !domain_match(u->host, c->domain)) continue;
        if (!path_match(u->path, c->path) || (c->secure && !https)) continue;
        if (n < ARRAY_SIZE(m)) m[n++] = c;
    }
    if (!n) return NULL;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strlen(m[j]->path) > strlen(m[j - 1]->path); j--) {
            cookie_t *t = m[j]; m[j] = m[j - 1]; m[j - 1] = t;
        }
    size_t cap = 1;
    for (int i = 0; i < n; i++) cap += strlen(m[i]->name) + strlen(m[i]->value) + 3;
    char *h = kmalloc(cap);
    if (!h) return NULL;
    size_t o = 0;
    for (int i = 0; i < n; i++)
        o += (size_t)snprintf(h + o, cap - o, "%s%s=%s", i ? "; " : "", m[i]->name, m[i]->value);
    return h;
}

/* Save pending changes before power off. Registered at INIT_DRIVER so the hook runs
   before the file system's own shutdown save (registered at INIT_FS). */
static void cookies_shutdown(void) { if (dirty) save_now(NULL); }
static void cookies_boot(void) { sys_add_shutdown_hook(cookies_shutdown); }
REGISTER_INIT(INIT_DRIVER, cookies_boot);
