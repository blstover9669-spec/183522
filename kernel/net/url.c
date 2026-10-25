/* url.c - URL parsing and relative reference resolution (RFC 3986, with the usual
   browser leniency: no scheme means http://, spaces and control bytes are %-encoded). */
#include "net_internal.h"

static bool has_scheme(const char *s, size_t *len) {
    if (!isalpha((unsigned char)s[0])) return false;
    size_t i = 1;
    while (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.') i++;
    if (s[i] != ':') return false;
    if (len) *len = i;
    return true;
}

/* copy with %-encoding of bytes a URL may not contain literally */
static bool put_encoded(char *out, size_t cap, size_t *o, const char *s, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch <= 0x20 || ch >= 0x7F || ch == '"' || ch == '<' || ch == '>' || ch == '`') {
            if (*o + 3 >= cap) return false;
            out[(*o)++] = '%';
            out[(*o)++] = hex[ch >> 4];
            out[(*o)++] = hex[ch & 15];
        } else {
            if (*o + 1 >= cap) return false;
            out[(*o)++] = (char)ch;
        }
    }
    out[*o] = 0;
    return true;
}

bool url_parse(const char *s, url_t *u) {
    memset(u, 0, sizeof *u);
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (!n) return false;
    const char *p = s, *end = s + n;
    size_t sl;
    if (has_scheme(s, &sl) && sl + 2 < n && s[sl + 1] == '/' && s[sl + 2] == '/') {
        if (sl >= sizeof u->scheme) return false;
        for (size_t i = 0; i < sl; i++) u->scheme[i] = (char)tolower((unsigned char)s[i]);
        p = s + sl + 3;
    } else if (has_scheme(s, &sl) && !isdigit((unsigned char)s[sl + 1])) {
        return false;                          /* mailto:, javascript:, ... (host:port is fine) */
    } else {
        strcpy(u->scheme, "http");
        if (p[0] == '/' && p[1] == '/') p += 2;
    }
    if (strcmp(u->scheme, "http") && strcmp(u->scheme, "https")) return false;
    const char *auth = p;
    while (p < end && *p != '/' && *p != '?' && *p != '#') p++;
    const char *auth_end = p;
    for (const char *q = auth; q < auth_end; q++) if (*q == '@') auth = q + 1;   /* drop user:pass@ */
    if (auth < auth_end && *auth == '[') return false;                         /* IPv6 literal */
    const char *colon = NULL;
    for (const char *q = auth; q < auth_end; q++) if (*q == ':') colon = q;
    const char *host_end = colon ? colon : auth_end;
    size_t hl = (size_t)(host_end - auth);
    if (hl == 0 || hl >= sizeof u->host) return false;
    for (size_t i = 0; i < hl; i++) {
        char ch = (char)tolower((unsigned char)auth[i]);
        if (!(isalnum((unsigned char)ch) || ch == '-' || ch == '.' || ch == '_')) return false;
        u->host[i] = ch;
    }
    if (u->host[hl - 1] == '.') u->host[hl - 1] = 0;
    u->port = strcmp(u->scheme, "https") ? 80 : 443;
    if (colon && colon + 1 < auth_end) {
        uint32_t port = 0;
        for (const char *q = colon + 1; q < auth_end; q++) {
            if (!isdigit((unsigned char)*q)) return false;
            port = port * 10 + (uint32_t)(*q - '0');
            if (port > 65535) return false;
        }
        if (!port) return false;
        u->port = (uint16_t)port;
    }
    const char *path = p, *path_end = p;
    while (path_end < end && *path_end != '#') path_end++;
    size_t o = 0;
    if (path == path_end || *path != '/') { u->path[0] = '/'; u->path[1] = 0; o = 1; }
    if (!put_encoded(u->path, sizeof u->path, &o, path, (size_t)(path_end - path))) return false;
    return true;
}

/* RFC 3986 5.2.4 */
static void remove_dots(char *path) {
    char *in = path, *out = path;
    while (*in) {
        if (!strncmp(in, "../", 3)) in += 3;
        else if (!strncmp(in, "./", 2)) in += 2;
        else if (!strncmp(in, "/./", 3)) in += 2;
        else if (!strcmp(in, "/.")) { in[1] = 0; }
        else if (!strncmp(in, "/../", 4) || !strcmp(in, "/..")) {
            if (in[3] == '/') in += 3; else { in += 2; *in = '/'; }
            while (out > path && *--out != '/') {}
            *out = 0;
        } else if (!strcmp(in, ".") || !strcmp(in, "..")) {
            *in = 0;
        } else {
            do { *out++ = *in++; } while (*in && *in != '/');
        }
    }
    *out = 0;
}

/* remove dot segments from the path part of p ("/a/../b?q#f"), keeping ?query#fragment */
static void normalize_path(char *p) {
    char *qs = p + strcspn(p, "?#");
    char *tail = NULL;
    if (*qs) {
        tail = kmalloc(strlen(qs) + 1);
        if (!tail) return;
        strcpy(tail, qs);
        *qs = 0;
    }
    remove_dots(p);
    if (!p[0]) strcpy(p, "/");
    if (tail) { strcat(p, tail); kfree(tail); }
}

bool url_resolve(const char *base, const char *ref, char *out, size_t size) {
    /* clean the reference: trim, drop tabs/newlines (as browsers do) */
    char *r = kmalloc(strlen(ref) + 1);
    if (!r) return false;
    size_t rl = 0;
    while (*ref && isspace((unsigned char)*ref)) ref++;
    for (; *ref; ref++) if (*ref != '\t' && *ref != '\n' && *ref != '\r') r[rl++] = *ref;
    while (rl && r[rl - 1] == ' ') rl--;
    r[rl] = 0;
    bool ok = false;
    size_t sl;
    if (has_scheme(r, &sl)) {
        ok = strlcpy(out, r, size) < size;
        kfree(r);
        return ok;
    }
    /* split the base: scheme://authority path?query#frag */
    const char *b = base;
    const char *bs_end = strstr(b, "://");
    if (!bs_end) { kfree(r); return false; }
    const char *auth_end = bs_end + 3;
    while (*auth_end && *auth_end != '/' && *auth_end != '?' && *auth_end != '#') auth_end++;
    const char *bpath = auth_end, *bq = bpath;
    while (*bq && *bq != '?' && *bq != '#') bq++;
    const char *bq_end = bq;
    while (*bq_end && *bq_end != '#') bq_end++;

    size_t need = strlen(base) + rl + 8;
    char *buf = kmalloc(need);
    if (!buf) { kfree(r); return false; }
    size_t prefix = (size_t)(auth_end - base);            /* "scheme://authority" */
    if (r[0] == '/' && r[1] == '/') {
        size_t sch = (size_t)(bs_end - base) + 1;          /* "scheme:" */
        memcpy(buf, base, sch);
        strcpy(buf + sch, r);
        char *pstart = buf + sch + 2;
        pstart += strcspn(pstart, "/?#");
        if (*pstart == '/') normalize_path(pstart);
    } else if (r[0] == 0) {
        memcpy(buf, base, (size_t)(bq_end - base));
        buf[bq_end - base] = 0;
    } else if (r[0] == '#') {
        memcpy(buf, base, (size_t)(bq_end - base));
        strcpy(buf + (bq_end - base), r);
    } else if (r[0] == '?') {
        memcpy(buf, base, (size_t)(bq - base));
        if (bq == bpath) buf[(bq - base)] = '/', strcpy(buf + (bq - base) + 1, r);
        else strcpy(buf + (bq - base), r);
    } else {
        memcpy(buf, base, prefix);
        char *pp = buf + prefix;
        if (r[0] == '/') {
            strcpy(pp, r);
        } else {
            /* merge: base path up to its last '/' + reference */
            size_t dir = 0;
            for (const char *q = bpath; q < bq; q++) if (*q == '/') dir = (size_t)(q - bpath) + 1;
            if (dir == 0) { pp[0] = '/'; dir = 1; }
            else memcpy(pp, bpath, dir);
            strcpy(pp + dir, r);
        }
        normalize_path(pp);
    }
    ok = strlcpy(out, buf, size) < size;
    kfree(buf);
    kfree(r);
    return ok;
}
