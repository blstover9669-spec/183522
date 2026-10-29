/*
 * http.c - asynchronous HTTP/1.1 client (http_fetch & co., see <nova/net.h>).
 *
 * Each request is a state machine stepped from the network tick:
 *   QUEUED -> RESOLVE (DNS) -> CONNECT (TCP, then the TLS handshake for https)
 *   -> SEND -> HEAD (status line + headers) -> BODY -> DONE
 * Bodies are de-chunked as they arrive and decompressed (gzip / deflate) when
 * complete. Redirects loop back to RESOLVE with the new URL. Up to MAX_ACTIVE
 * requests run at once. Callbacks are only ever called from http_tick's dispatch
 * loop, never from inside an API call, so they may free requests or start new ones.
 */
#include "net_internal.h"

#define MAX_ACTIVE     8
#define MAX_HEADER     (64 * 1024)
#define MAX_REDIRECTS  10
#define READ_PER_TICK  (128 * 1024)
#define REQ_MAGIC      0x48545450u

enum { P_QUEUED, P_RESOLVE, P_CONNECT, P_SEND, P_HEAD, P_BODY, P_DONE };
enum { B_NONE, B_LENGTH, B_CHUNKED, B_EOF };
enum { C_SIZE, C_DATA, C_DATA_END, C_TRAILER };

struct http_request {
    struct http_request *next;
    uint32_t magic;
    /* what to fetch */
    char     method[16];
    char    *url;
    uint8_t *rbody;
    uint32_t rbody_len;
    char    *ctype, *xheaders;
    uint32_t flags, max_body, timeout_ms;
    http_cb  on_done, on_progress;
    void    *ctx;
    /* progress */
    int      phase, error;
    char     errtext[200];
    url_t   *u;
    dns_query_t *dns;
    uint32_t ip;
    tcp_conn_t *tcp;
    tls_conn_t *tls;
    char    *req;
    uint32_t req_len, req_sent;
    char    *head;
    uint32_t head_len;
    /* response */
    int      status;
    char     reason[64];
    int      nhdr;
    char   **hname, **hval;
    uint8_t *body;
    uint32_t blen, bcap;
    int      bmode;
    uint32_t clen;
    bool     have_clen;
    int      cstate;
    uint32_t cleft;
    char     cline[64];
    uint32_t cline_len;
    uint32_t wire;
    int      redirects;
    bool     https, cert_ok;
    char     cert_error[160], cert_subject[128], cert_issuer[128], tlsinfo[96];
    uint32_t last_progress;
    bool     done_pending, progress_pending;
};

static http_request_t *reqs;

static bool valid(const http_request_t *r) { return r && r->magic == REQ_MAGIC; }

/* ---- helpers --------------------------------------------------------------------------- */
static char *sdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = kmalloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

static void free_response(http_request_t *r) {
    for (int i = 0; i < r->nhdr; i++) { kfree(r->hname[i]); kfree(r->hval[i]); }
    kfree(r->hname);
    kfree(r->hval);
    r->hname = r->hval = NULL;
    r->nhdr = 0;
    kfree(r->body);
    r->body = NULL;
    r->blen = r->bcap = 0;
    kfree(r->head);
    r->head = NULL;
    r->head_len = 0;
    r->status = 0;
    r->reason[0] = 0;
    r->have_clen = false;
    r->clen = 0;
    r->wire = 0;
    r->cstate = C_SIZE;
    r->cline_len = 0;
}

static void close_conn(http_request_t *r, bool abort) {
    if (r->tls) { tls_close(r->tls, abort); r->tls = NULL; r->tcp = NULL; }
    if (r->tcp) { tcp_close(r->tcp, abort); r->tcp = NULL; }
    if (r->dns) { dns_free(r->dns); r->dns = NULL; }
    kfree(r->req);
    r->req = NULL;
}

static void fail(http_request_t *r, int err, const char *fmt, ...) PRINTF_LIKE(3, 4);
static void fail(http_request_t *r, int err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->errtext, sizeof r->errtext, fmt, ap);
    va_end(ap);
    close_conn(r, true);
    r->error = err;
    r->phase = P_DONE;
    r->done_pending = true;
    NLOG("http: %s %s failed: %s", r->method, r->url, r->errtext);
}

static bool has_xheader(const http_request_t *r, const char *name) {
    if (!r->xheaders) return false;
    size_t n = strlen(name);
    const char *p = r->xheaders;
    while (*p) {
        if (!strncasecmp(p, name, n) && p[n] == ':') return true;
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return false;
}

static bool build_request(http_request_t *r) {
    url_t *u = r->u;
    char *cookie = (r->flags & HTTP_F_NO_COOKIES) ? NULL : cookies_header_for(u);
    size_t cap = strlen(u->path) + strlen(u->host) + 600 + (cookie ? strlen(cookie) : 0) +
                 (r->xheaders ? strlen(r->xheaders) : 0) + (r->ctype ? strlen(r->ctype) : 0) + r->rbody_len;
    char *q = kmalloc(cap);
    if (!q) { kfree(cookie); return false; }
    size_t o = 0;
    bool def_port = (r->https && u->port == 443) || (!r->https && u->port == 80);
    o += (size_t)snprintf(q + o, cap - o, "%s %s HTTP/1.1\r\n", r->method, u->path);
    if (def_port) o += (size_t)snprintf(q + o, cap - o, "Host: %s\r\n", u->host);
    else o += (size_t)snprintf(q + o, cap - o, "Host: %s:%u\r\n", u->host, u->port);
    if (!has_xheader(r, "User-Agent"))
        o += (size_t)snprintf(q + o, cap - o, "User-Agent: Mozilla/5.0 (NovaOS 2.5; i686) NovaBrowser/2.5\r\n");
    if (!has_xheader(r, "Accept"))
        o += (size_t)snprintf(q + o, cap - o, "Accept: text/html,application/xhtml+xml,*/*;q=0.8\r\n");
    if (!has_xheader(r, "Accept-Language"))
        o += (size_t)snprintf(q + o, cap - o, "Accept-Language: en-US,en;q=0.8\r\n");
    if (!has_xheader(r, "Accept-Encoding"))
        o += (size_t)snprintf(q + o, cap - o, "Accept-Encoding: gzip, deflate\r\n");
    o += (size_t)snprintf(q + o, cap - o, "Connection: close\r\n");
    if (cookie) o += (size_t)snprintf(q + o, cap - o, "Cookie: %s\r\n", cookie);
    if (r->rbody_len || !strcmp(r->method, "POST") || !strcmp(r->method, "PUT")) {
        if (r->ctype) o += (size_t)snprintf(q + o, cap - o, "Content-Type: %s\r\n", r->ctype);
        o += (size_t)snprintf(q + o, cap - o, "Content-Length: %u\r\n", r->rbody_len);
    }
    if (r->xheaders) {
        o += strlcpy(q + o, r->xheaders, cap - o);
        if (o >= 2 && strncmp(q + o - 2, "\r\n", 2)) o += (size_t)snprintf(q + o, cap - o, "\r\n");
    }
    o += (size_t)snprintf(q + o, cap - o, "\r\n");
    if (r->rbody_len) { memcpy(q + o, r->rbody, r->rbody_len); o += r->rbody_len; }
    kfree(cookie);
    kfree(r->req);
    r->req = q;
    r->req_len = (uint32_t)o;
    r->req_sent = 0;
    return true;
}

/* (Re)start the connection for r->url. */
static void start(http_request_t *r) {
    if (!r->u) r->u = kmalloc(sizeof(url_t));
    if (!r->u) { fail(r, HTTP_ERR_NO_MEMORY, "Out of memory"); return; }
    if (!url_parse(r->url, r->u)) { fail(r, HTTP_ERR_BAD_URL, "\"%s\" is not a valid web address", r->url); return; }
    r->https = !strcmp(r->u->scheme, "https");
    r->cert_ok = false;
    r->cert_error[0] = r->cert_subject[0] = r->cert_issuer[0] = r->tlsinfo[0] = 0;
    if (!net_is_up()) { fail(r, HTTP_ERR_NO_NETWORK, "No internet connection (%s)", net_status_text()); return; }
    r->dns = dns_resolve(r->u->host);
    if (!r->dns) { fail(r, HTTP_ERR_NO_MEMORY, "Out of memory"); return; }
    r->phase = P_RESOLVE;
    r->last_progress = net_now();
}

static int active_count(void) {
    int n = 0;
    for (http_request_t *r = reqs; r; r = r->next) if (r->phase > P_QUEUED && r->phase < P_DONE) n++;
    return n;
}

/* ---- stream: plain TCP or TLS ---------------------------------------------------------------- */
enum { S_CONNECTING, S_OPEN, S_EOF, S_ERROR };

static int s_state(http_request_t *r) {
    if (r->tls) {
        switch (tls_state(r->tls)) {
        case TLSS_HANDSHAKE: return S_CONNECTING;
        case TLSS_OPEN: return S_OPEN;
        case TLSS_PEER_CLOSED: return S_EOF;
        default: return S_ERROR;
        }
    }
    switch (tcp_state(r->tcp)) {
    case TCPS_CONNECTING: return S_CONNECTING;
    case TCPS_OPEN: return S_OPEN;
    case TCPS_PEER_CLOSED: return S_EOF;
    default: return S_ERROR;
    }
}
static uint32_t s_readable(http_request_t *r) { return r->tls ? tls_readable(r->tls) : tcp_readable(r->tcp); }
static uint32_t s_read(http_request_t *r, void *b, uint32_t n) { return r->tls ? tls_read(r->tls, b, n) : tcp_read(r->tcp, b, n); }
static uint32_t s_write(http_request_t *r, const void *b, uint32_t n) { return r->tls ? tls_write(r->tls, b, n) : tcp_write(r->tcp, b, n); }

static const char *tcp_err_text(int e) {
    switch (e) {
    case TCP_EREFUSED: return "the connection was refused";
    case TCP_ERESET: return "the connection was reset";
    case TCP_ETIMEOUT: return "the connection timed out";
    default: return "the server cannot be reached";
    }
}

/* ---- response parsing -------------------------------------------------------------------------- */
static bool body_append(http_request_t *r, const uint8_t *d, uint32_t n) {
    if (!n) return true;
    uint32_t max = r->max_body;
    if ((uint64_t)r->blen + n > max) { fail(r, HTTP_ERR_TOO_LARGE, "The file is bigger than %u MB", max >> 20); return false; }
    if (r->blen + n + 1 > r->bcap) {
        uint64_t cap = r->bcap ? r->bcap : 16384;
        while (cap < (uint64_t)r->blen + n + 1) cap *= 2;
        if (cap > (uint64_t)max + 1) cap = (uint64_t)max + 1;
        uint8_t *nb = krealloc(r->body, (size_t)cap);
        if (!nb) { fail(r, HTTP_ERR_NO_MEMORY, "Out of memory"); return false; }
        r->body = nb;
        r->bcap = (uint32_t)cap;
    }
    memcpy(r->body + r->blen, d, n);
    r->blen += n;
    r->body[r->blen] = 0;
    return true;
}

static void complete(http_request_t *r);

/* Feed raw body bytes (after the headers). Returns false when the request ended. */
static bool feed_body(http_request_t *r, const uint8_t *d, uint32_t n) {
    r->wire += n;
    if (n) r->progress_pending = true;
    if (r->bmode == B_LENGTH) {
        uint32_t want = r->clen - r->blen;
        if (n > want) n = want;
        if (!body_append(r, d, n)) return false;
        if (r->blen >= r->clen) { complete(r); return false; }
        return true;
    }
    if (r->bmode == B_EOF) return body_append(r, d, n);
    /* chunked */
    uint32_t i = 0;
    while (i < n) {
        if (r->cstate == C_DATA) {
            uint32_t k = MIN(r->cleft, n - i);
            if (!body_append(r, d + i, k)) return false;
            i += k;
            r->cleft -= k;
            if (!r->cleft) { r->cstate = C_DATA_END; r->cline_len = 0; }
            continue;
        }
        char ch = (char)d[i++];
        if (ch != '\n') {
            if (ch != '\r' && r->cline_len < sizeof r->cline - 1) r->cline[r->cline_len++] = ch;
            else if (ch != '\r' && r->cstate == C_SIZE) { fail(r, HTTP_ERR_PROTOCOL, "Bad chunked encoding"); return false; }
            continue;
        }
        r->cline[r->cline_len] = 0;
        uint32_t len = r->cline_len;
        r->cline_len = 0;
        if (r->cstate == C_DATA_END) {
            if (len) { fail(r, HTTP_ERR_PROTOCOL, "Bad chunked encoding"); return false; }
            r->cstate = C_SIZE;
        } else if (r->cstate == C_SIZE) {
            if (!len) continue;                     /* tolerate stray blank lines */
            char *end;
            unsigned long sz = strtoul(r->cline, &end, 16);
            if (end == r->cline) { fail(r, HTTP_ERR_PROTOCOL, "Bad chunk size"); return false; }
            if (sz == 0) r->cstate = C_TRAILER;
            else { r->cleft = (uint32_t)sz; r->cstate = C_DATA; }
        } else {                                    /* trailer section ends with an empty line */
            if (!len) { complete(r); return false; }
        }
    }
    return true;
}

static void add_header(http_request_t *r, const char *name, size_t nl, const char *val, size_t vl) {
    char **hn = krealloc(r->hname, sizeof(char *) * (size_t)(r->nhdr + 1));
    if (!hn) return;
    r->hname = hn;
    char **hv = krealloc(r->hval, sizeof(char *) * (size_t)(r->nhdr + 1));
    if (!hv) return;
    r->hval = hv;
    char *a = kmalloc(nl + 1), *b = kmalloc(vl + 1);
    if (!a || !b) { kfree(a); kfree(b); return; }
    memcpy(a, name, nl); a[nl] = 0;
    memcpy(b, val, vl); b[vl] = 0;
    r->hname[r->nhdr] = a;
    r->hval[r->nhdr] = b;
    r->nhdr++;
}

/* Parse r->head[0..end) (status line + headers). false on a malformed response. */
static bool parse_head(http_request_t *r, uint32_t end) {
    char *h = r->head;
    h[end] = 0;
    char *line = h, *eol = strchr(line, '\n');
    if (!eol) return false;
    *eol = 0;
    if (eol > line && eol[-1] == '\r') eol[-1] = 0;
    if (strncmp(line, "HTTP/", 5)) return false;
    char *sp = strchr(line, ' ');
    if (!sp) return false;
    r->status = atoi(sp + 1);
    if (r->status < 100 || r->status > 999) return false;
    char *rs = strchr(sp + 1, ' ');
    strlcpy(r->reason, rs ? rs + 1 : "", sizeof r->reason);
    line = eol + 1;
    while (*line) {
        eol = strchr(line, '\n');
        if (eol) *eol = 0;
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[--ll] = 0;
        if (!ll) break;
        if ((line[0] == ' ' || line[0] == '\t') && r->nhdr) {      /* obsolete folding */
            char *prev = r->hval[r->nhdr - 1];
            size_t pl = strlen(prev);
            char *np = krealloc(prev, pl + ll + 1);
            if (np) { np[pl] = ' '; strcpy(np + pl + 1, line + 1); r->hval[r->nhdr - 1] = np; }
        } else {
            char *colon = strchr(line, ':');
            if (colon && colon != line) {
                char *v = colon + 1;
                while (*v == ' ' || *v == '\t') v++;
                size_t vl = strlen(v);
                while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
                add_header(r, line, (size_t)(colon - line), v, vl);
            }
        }
        if (!eol) break;
        line = eol + 1;
    }
    return true;
}

static bool token_in(const char *list, const char *tok) {
    size_t n = strlen(tok);
    for (const char *p = list; p && *p;) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!strncasecmp(p, tok, n) && (p[n] == 0 || p[n] == ',' || p[n] == ' ' || p[n] == ';')) return true;
        p = strchr(p, ',');
    }
    return false;
}

/* Headers complete: decide how the body is framed. */
static void headers_done(http_request_t *r) {
    if (!(r->flags & HTTP_F_NO_COOKIES))
        for (int i = 0; i < r->nhdr; i++)
            if (!strcasecmp(r->hname[i], "Set-Cookie")) cookies_store(r->u, r->hval[i]);
    const char *te = http_header(r, "Transfer-Encoding");
    const char *cl = http_header(r, "Content-Length");
    r->have_clen = false;
    if (!strcmp(r->method, "HEAD") || r->status == 204 || r->status == 304 || r->status < 200) r->bmode = B_NONE;
    else if (te && token_in(te, "chunked")) r->bmode = B_CHUNKED;
    else if (cl) {
        char *end;
        unsigned long v = strtoul(cl, &end, 10);
        if (end == cl) { r->bmode = B_EOF; }
        else { r->bmode = B_LENGTH; r->clen = (uint32_t)v; r->have_clen = true; }
    } else r->bmode = B_EOF;
    if (r->have_clen && r->clen > r->max_body) {
        fail(r, HTTP_ERR_TOO_LARGE, "The file is bigger than %u MB", r->max_body >> 20);
        return;
    }
    if (r->have_clen && r->clen) {
        r->body = kmalloc(r->clen + 1);
        if (!r->body) { fail(r, HTTP_ERR_NO_MEMORY, "Out of memory"); return; }
        r->bcap = r->clen + 1;
        r->body[0] = 0;
    }
    r->phase = P_BODY;
    r->progress_pending = true;
    if (r->bmode == B_NONE || (r->bmode == B_LENGTH && r->clen == 0)) complete(r);
}

/* The response is complete: follow a redirect, or decode the body and finish. */
static void complete(http_request_t *r) {
    close_conn(r, false);
    int s = r->status;
    const char *loc = http_header(r, "Location");
    if ((s == 301 || s == 302 || s == 303 || s == 307 || s == 308) && loc && !(r->flags & HTTP_F_NO_REDIRECT)) {
        if (++r->redirects > MAX_REDIRECTS) { fail(r, HTTP_ERR_REDIRECTS, "Too many redirects"); return; }
        char *nu = kmalloc(4096);
        if (!nu) { fail(r, HTTP_ERR_NO_MEMORY, "Out of memory"); return; }
        if (!url_resolve(r->url, loc, nu, 4096)) { kfree(nu); fail(r, HTTP_ERR_BAD_URL, "Bad redirect"); return; }
        NLOG("http: %d redirect %s -> %s", s, r->url, nu);
        kfree(r->url);
        r->url = nu;
        if ((s == 303 && strcmp(r->method, "HEAD")) || ((s == 301 || s == 302) && !strcmp(r->method, "POST"))) {
            strcpy(r->method, "GET");
            kfree(r->rbody);
            r->rbody = NULL;
            r->rbody_len = 0;
            kfree(r->ctype);
            r->ctype = NULL;
        }
        free_response(r);
        start(r);
        return;
    }
    const char *ce = http_header(r, "Content-Encoding");
    if (ce && r->blen && !(r->flags & HTTP_F_NO_DECODE)) {
        int fmt = -1;
        if (token_in(ce, "gzip") || token_in(ce, "x-gzip")) fmt = INFLATE_GZIP;
        else if (token_in(ce, "deflate")) fmt = INFLATE_AUTO;
        if (fmt >= 0) {
            uint8_t *out;
            uint32_t olen;
            int e = inflate_buffer(r->body, r->blen, fmt, &out, &olen, r->max_body);
            if (e == -2) { fail(r, HTTP_ERR_TOO_LARGE, "The file is bigger than %u MB", r->max_body >> 20); return; }
            if (e) { fail(r, HTTP_ERR_PROTOCOL, "The compressed content is damaged"); return; }
            kfree(r->body);
            r->body = out;
            r->blen = olen;
            r->bcap = olen + 1;
        }
    }
    r->error = HTTP_OK;
    r->phase = P_DONE;
    r->done_pending = true;
    NLOG("http: %s %s -> %d %s, %u bytes%s", r->method, r->url, r->status, r->reason, r->blen,
         r->https ? (r->cert_ok ? " (secure)" : " (certificate NOT verified)") : "");
}

static void grab_tls_info(http_request_t *r) {
    if (!r->tls) return;
    r->cert_ok = tls_cert_ok(r->tls);
    strlcpy(r->cert_error, tls_cert_error(r->tls), sizeof r->cert_error);
    strlcpy(r->cert_subject, tls_cert_subject(r->tls), sizeof r->cert_subject);
    strlcpy(r->cert_issuer, tls_cert_issuer(r->tls), sizeof r->cert_issuer);
    strlcpy(r->tlsinfo, tls_info(r->tls), sizeof r->tlsinfo);
}

/* ---- the state machine ------------------------------------------------------------------------- */
static void step(http_request_t *r) {
    uint32_t now = net_now();
    if (r->phase == P_QUEUED || r->phase == P_DONE) return;
    if (r->tls) tls_step(r->tls);
    if (now - r->last_progress > r->timeout_ms) {
        if (r->phase == P_RESOLVE) fail(r, HTTP_ERR_DNS, "The DNS server did not answer for %s", r->u->host);
        else if (r->phase == P_CONNECT) fail(r, HTTP_ERR_CONNECT, "Could not connect to %s: the connection timed out", r->u->host);
        else fail(r, HTTP_ERR_TIMEOUT, "%s took too long to respond", r->u->host);
        return;
    }
    switch (r->phase) {
    case P_RESOLVE: {
        int res = dns_result(r->dns, &r->ip);
        if (res == DNS_PENDING) return;
        dns_free(r->dns);
        r->dns = NULL;
        if (res == DNS_ENOTFOUND) { fail(r, HTTP_ERR_DNS, "Could not find the site %s", r->u->host); return; }
        if (res != DNS_OK) {
            if (!net_is_up()) fail(r, HTTP_ERR_NO_NETWORK, "No internet connection (%s)", net_status_text());
            else fail(r, HTTP_ERR_DNS, "The DNS server did not answer for %s", r->u->host);
            return;
        }
        r->tcp = tcp_open(r->ip, r->u->port);
        if (!r->tcp) { fail(r, HTTP_ERR_NO_NETWORK, "No internet connection (%s)", net_status_text()); return; }
        if (r->https) {
            r->tls = tls_open(r->tcp, r->u->host, (r->flags & HTTP_F_ALLOW_BAD_CERT) ? TLS_F_ALLOW_BAD_CERT : 0);
            if (!r->tls) { fail(r, HTTP_ERR_NO_MEMORY, "Out of memory"); return; }
        }
        r->phase = P_CONNECT;
        r->last_progress = now;
        return;
    }
    case P_CONNECT: {
        int st = s_state(r);
        if (st == S_CONNECTING) return;
        if (st == S_ERROR || st == S_EOF) {
            if (r->tls) {
                grab_tls_info(r);
                int e = tls_error(r->tls);
                if (e == TLS_ECERT) fail(r, HTTP_ERR_CERT, "%s", r->cert_error[0] ? r->cert_error : "The certificate is not valid");
                else if (e == TLS_ETCP) fail(r, HTTP_ERR_CONNECT, "Could not connect to %s: %s", r->u->host, tcp_err_text(tls_tcp_error(r->tls)));
                else fail(r, HTTP_ERR_TLS, "Secure connection to %s failed: %s", r->u->host, tls_error_text(r->tls));
            } else {
                fail(r, HTTP_ERR_CONNECT, "Could not connect to %s: %s", r->u->host, tcp_err_text(tcp_error(r->tcp)));
            }
            return;
        }
        grab_tls_info(r);
        if (!build_request(r)) { fail(r, HTTP_ERR_NO_MEMORY, "Out of memory"); return; }
        r->phase = P_SEND;
        r->last_progress = now;
    }
    /* fall through */
    case P_SEND: {
        if (s_state(r) == S_ERROR) { fail(r, HTTP_ERR_CONNECT, "The connection to %s broke", r->u->host); return; }
        uint32_t n = s_write(r, r->req + r->req_sent, r->req_len - r->req_sent);
        if (n) { r->req_sent += n; r->last_progress = now; }
        if (r->req_sent < r->req_len) return;
        kfree(r->req);
        r->req = NULL;
        r->head = kmalloc(MAX_HEADER + 1);
        if (!r->head) { fail(r, HTTP_ERR_NO_MEMORY, "Out of memory"); return; }
        r->head_len = 0;
        r->phase = P_HEAD;
    }
    /* fall through */
    case P_HEAD:
        for (;;) {
            uint32_t room = MAX_HEADER - r->head_len;
            uint32_t n = room ? s_read(r, r->head + r->head_len, room) : 0;
            if (n) r->last_progress = now;
            uint32_t old = r->head_len > 3 ? r->head_len - 3 : 0;
            r->head_len += n;
            /* find the end of the header block */
            uint32_t end = 0, skip = 0;
            for (uint32_t i = old; i + 1 < r->head_len; i++) {
                if (r->head[i] == '\n' && r->head[i + 1] == '\n') { end = i + 1; skip = 1; break; }
                if (i + 3 < r->head_len + 0 && r->head[i] == '\r' && r->head[i + 1] == '\n' &&
                    r->head[i + 2] == '\r' && r->head[i + 3] == '\n') { end = i + 2; skip = 2; break; }
            }
            if (!end) {
                if (r->head_len >= MAX_HEADER) { fail(r, HTTP_ERR_PROTOCOL, "The response headers are too long"); return; }
                int st = s_state(r);
                if (st == S_EOF && !s_readable(r)) {
                    fail(r, HTTP_ERR_PROTOCOL, r->head_len ? "The server sent a broken response" : "The server closed the connection without answering");
                    return;
                }
                if (st == S_ERROR) { fail(r, HTTP_ERR_CONNECT, "The connection to %s broke", r->u->host); return; }
                if (!n) return;
                continue;
            }
            uint32_t rest_off = end + skip;
            uint32_t rest_len = r->head_len - rest_off;
            uint8_t *rest = NULL;
            if (rest_len) {
                rest = kmalloc(rest_len);
                if (!rest) { fail(r, HTTP_ERR_NO_MEMORY, "Out of memory"); return; }
                memcpy(rest, r->head + rest_off, rest_len);
            }
            if (!parse_head(r, end)) { kfree(rest); fail(r, HTTP_ERR_PROTOCOL, "The server's answer is not HTTP"); return; }
            if (r->status >= 100 && r->status < 200 && r->status != 101) {
                /* informational (100 Continue, 103 Early Hints): drop it, read the next head */
                for (int i = 0; i < r->nhdr; i++) { kfree(r->hname[i]); kfree(r->hval[i]); }
                r->nhdr = 0;
                if (rest_len) memcpy(r->head, rest, rest_len);
                r->head_len = rest_len;
                kfree(rest);
                continue;
            }
            kfree(r->head);
            r->head = NULL;
            headers_done(r);
            if (r->phase != P_BODY) { kfree(rest); return; }
            bool more = feed_body(r, rest, rest_len);
            kfree(rest);
            if (!more) return;
            break;
        }
    /* fall through */
    case P_BODY: {
        uint8_t *buf = kmalloc(16384);
        if (!buf) return;
        uint32_t total = 0;
        while (r->phase == P_BODY && total < READ_PER_TICK) {
            uint32_t n = s_read(r, buf, 16384);
            if (!n) break;
            total += n;
            r->last_progress = now;
            if (!feed_body(r, buf, n)) break;
        }
        kfree(buf);
        if (r->phase != P_BODY) return;
        int st = s_state(r);
        if (st == S_EOF && !s_readable(r)) {
            if (r->bmode == B_EOF) complete(r);
            else fail(r, HTTP_ERR_PROTOCOL, "The connection closed before the whole page arrived (%u bytes)", r->blen);
        } else if (st == S_ERROR) {
            if (r->bmode == B_EOF && r->blen) complete(r);     /* many servers just reset at the end */
            else fail(r, HTTP_ERR_CONNECT, "The connection to %s broke", r->u->host);
        }
        return;
    }
    }
}

void http_tick(void) {
    for (http_request_t *r = reqs; r; r = r->next) {
        if (r->phase == P_QUEUED && active_count() < MAX_ACTIVE) start(r);
        else step(r);
    }
    /* callbacks: restart the scan after each one (a callback may free or add requests) */
    for (;;) {
        http_request_t *r = reqs;
        while (r && !r->progress_pending && !r->done_pending) r = r->next;
        if (!r) break;
        if (r->progress_pending) {
            r->progress_pending = false;
            if (r->on_progress && !r->done_pending) { r->on_progress(r, r->ctx); continue; }
        }
        if (r->done_pending) {
            r->done_pending = false;
            if (r->on_done) r->on_done(r, r->ctx);
        }
    }
}

/* ---- API ---------------------------------------------------------------------------------------- */
http_request_t *http_fetch_ex(const char *url, const http_options_t *opt, http_cb on_done, void *ctx) {
    http_options_t none = { 0 };
    if (!opt) opt = &none;
    http_request_t *r = kcalloc(1, sizeof *r);
    if (!r) return NULL;
    r->magic = REQ_MAGIC;
    r->url = sdup(url ? url : "");
    const char *m = opt->method ? opt->method : (opt->body_len ? "POST" : "GET");
    strlcpy(r->method, m, sizeof r->method);
    for (char *p = r->method; *p; p++) *p = (char)toupper((unsigned char)*p);
    if (opt->body_len && opt->body) {
        r->rbody = kmalloc(opt->body_len);
        if (r->rbody) { memcpy(r->rbody, opt->body, opt->body_len); r->rbody_len = opt->body_len; }
    }
    r->ctype = sdup(opt->content_type);
    r->xheaders = sdup(opt->headers);
    r->flags = opt->flags;
    r->max_body = opt->max_body ? opt->max_body : 64u << 20;
    r->timeout_ms = opt->timeout_ms ? opt->timeout_ms : 30000;
    r->on_done = on_done;
    r->on_progress = opt->on_progress;
    r->ctx = ctx;
    r->error = HTTP_PENDING;
    r->phase = P_QUEUED;
    r->cstate = C_SIZE;
    if (!r->url || (opt->body_len && !r->rbody)) {
        r->phase = P_DONE;
        r->error = HTTP_ERR_NO_MEMORY;
        strcpy(r->errtext, "Out of memory");
        r->done_pending = true;
    }
    net_start_tick();
    /* append (FIFO) */
    http_request_t **pp = &reqs;
    while (*pp) pp = &(*pp)->next;
    *pp = r;
    return r;
}

http_request_t *http_fetch(const char *method, const char *url, const char *body, uint32_t body_len,
                           const char *content_type, http_cb on_done, void *ctx) {
    http_options_t o = { 0 };
    o.method = method;
    o.body = body;
    o.body_len = body ? body_len : 0;
    o.content_type = content_type;
    return http_fetch_ex(url, &o, on_done, ctx);
}

void http_cancel(http_request_t *r) {
    if (!valid(r) || r->phase == P_DONE) return;
    close_conn(r, true);
    r->phase = P_DONE;
    r->error = HTTP_ERR_CANCELLED;
    strcpy(r->errtext, "Cancelled");
    r->done_pending = r->progress_pending = false;
}

void http_free(http_request_t *r) {
    if (!valid(r)) return;
    close_conn(r, true);
    for (http_request_t **pp = &reqs; *pp; pp = &(*pp)->next)
        if (*pp == r) { *pp = r->next; break; }
    free_response(r);
    kfree(r->url);
    kfree(r->rbody);
    kfree(r->ctype);
    kfree(r->xheaders);
    kfree(r->u);
    r->magic = 0;
    kfree(r);
}

bool http_done(const http_request_t *r) { return valid(r) && r->phase == P_DONE; }
int http_error(const http_request_t *r) { return valid(r) ? r->error : HTTP_ERR_CANCELLED; }
const char *http_error_text(const http_request_t *r) {
    if (!valid(r) || r->error == HTTP_OK || r->error == HTTP_PENDING) return "";
    return r->errtext[0] ? r->errtext : http_strerror(r->error);
}

const char *http_strerror(int e) {
    switch (e) {
    case HTTP_OK: return "";
    case HTTP_PENDING: return "Loading";
    case HTTP_ERR_NO_NETWORK: return "No internet connection";
    case HTTP_ERR_DNS: return "Site not found";
    case HTTP_ERR_CONNECT: return "Could not connect to the site";
    case HTTP_ERR_TIMEOUT: return "The site took too long to respond";
    case HTTP_ERR_TLS: return "Secure connection failed";
    case HTTP_ERR_CERT: return "The site's security certificate is not valid";
    case HTTP_ERR_PROTOCOL: return "The site sent an invalid response";
    case HTTP_ERR_REDIRECTS: return "Too many redirects";
    case HTTP_ERR_BAD_URL: return "Invalid web address";
    case HTTP_ERR_TOO_LARGE: return "The file is too large";
    case HTTP_ERR_NO_MEMORY: return "Out of memory";
    case HTTP_ERR_CANCELLED: return "Cancelled";
    default: return "Error";
    }
}

int http_status(const http_request_t *r) { return valid(r) ? r->status : 0; }
const char *http_reason(const http_request_t *r) { return valid(r) ? r->reason : ""; }
const char *http_url(const http_request_t *r) { return valid(r) && r->url ? r->url : ""; }
int http_redirect_count(const http_request_t *r) { return valid(r) ? r->redirects : 0; }

const char *http_header(const http_request_t *r, const char *name) {
    if (!valid(r) || !name) return NULL;
    for (int i = 0; i < r->nhdr; i++)
        if (!strcasecmp(r->hname[i], name)) return r->hval[i];
    return NULL;
}

int http_header_count(const http_request_t *r) { return valid(r) ? r->nhdr : 0; }
bool http_header_at(const http_request_t *r, int i, const char **name, const char **value) {
    if (!valid(r) || i < 0 || i >= r->nhdr) return false;
    if (name) *name = r->hname[i];
    if (value) *value = r->hval[i];
    return true;
}

const uint8_t *http_body(const http_request_t *r) { return valid(r) ? r->body : NULL; }
uint32_t http_body_len(const http_request_t *r) { return valid(r) ? r->blen : 0; }
uint8_t *http_take_body(http_request_t *r, uint32_t *len) {
    if (!valid(r)) { if (len) *len = 0; return NULL; }
    uint8_t *b = r->body;
    if (len) *len = r->blen;
    r->body = NULL;
    r->blen = r->bcap = 0;
    return b;
}

void http_progress(const http_request_t *r, uint32_t *received, uint32_t *total) {
    if (received) *received = valid(r) ? r->wire : 0;
    if (total) *total = valid(r) && r->have_clen ? r->clen : 0;
}

bool http_is_https(const http_request_t *r) { return valid(r) && r->https; }
bool http_cert_ok(const http_request_t *r) { return valid(r) && r->https && r->cert_ok; }
const char *http_cert_error(const http_request_t *r) { return valid(r) ? r->cert_error : ""; }
const char *http_cert_subject(const http_request_t *r) { return valid(r) ? r->cert_subject : ""; }
const char *http_cert_issuer(const http_request_t *r) { return valid(r) ? r->cert_issuer : ""; }
const char *http_tls_info(const http_request_t *r) { return valid(r) ? r->tlsinfo : ""; }
