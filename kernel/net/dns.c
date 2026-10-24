/*
 * dns.c - DNS resolver (RFC 1035): A records over UDP to the DHCP-supplied server,
 * random query ids and source ports, 3 tries per server with a 2 s timeout, and a
 * small cache that honours the record TTL (failures are cached for 10 s).
 */
#include "net_internal.h"
#include "crypto/crypto.h"

struct dns_query {
    char     host[256];
    int      result;          /* DNS_PENDING / DNS_OK / error */
    uint32_t ip;
    uint16_t id, port;
    uint32_t sent;
    int      tries;
    bool     started;
    struct dns_query *next;
};

#define CACHE_N 48
static struct { char host[128]; uint32_t ip; uint32_t expires; int result; } cache[CACHE_N];
static dns_query_t *queries;

bool net_parse_ipv4(const char *s, uint32_t *ip) {
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (!isdigit((unsigned char)*s)) return false;
        uint32_t n = 0;
        int digits = 0;
        while (isdigit((unsigned char)*s)) { n = n * 10 + (uint32_t)(*s++ - '0'); if (++digits > 3) return false; }
        if (n > 255) return false;
        v = (v << 8) | n;
        if (part < 3 && *s++ != '.') return false;
    }
    if (*s) return false;
    *ip = v;
    return true;
}

static int cache_lookup(const char *host, uint32_t *ip) {
    uint32_t now = net_now();
    for (int i = 0; i < CACHE_N; i++) {
        if (!cache[i].host[0] || strcasecmp(cache[i].host, host)) continue;
        if ((int32_t)(cache[i].expires - now) <= 0) { cache[i].host[0] = 0; return DNS_PENDING; }
        *ip = cache[i].ip;
        return cache[i].result;
    }
    return DNS_PENDING;
}

static void cache_put(const char *host, uint32_t ip, int result, uint32_t ttl_s) {
    if (strlen(host) >= sizeof cache[0].host) return;
    if (ttl_s < 5) ttl_s = 5;
    if (ttl_s > 3600) ttl_s = 3600;
    int slot = 0;
    uint32_t now = net_now();
    for (int i = 0; i < CACHE_N; i++) {
        if (!cache[i].host[0] || !strcasecmp(cache[i].host, host)) { slot = i; break; }
        if ((int32_t)(cache[i].expires - cache[slot].expires) < 0) slot = i;
    }
    strlcpy(cache[slot].host, host, sizeof cache[slot].host);
    cache[slot].ip = ip;
    cache[slot].result = result;
    cache[slot].expires = now + ttl_s * 1000;
}

static void finish(dns_query_t *q, int result, uint32_t ip) {
    q->result = result;
    q->ip = ip;
    if (q->port) { udp_unbind(q->port); q->port = 0; }
}

/* Skip a (possibly compressed) name; returns the offset after it or 0 on error. */
static uint32_t skip_name(const uint8_t *m, uint32_t len, uint32_t off) {
    for (int guard = 0; guard < 128; guard++) {
        if (off >= len) return 0;
        uint8_t l = m[off];
        if (l == 0) return off + 1;
        if ((l & 0xC0) == 0xC0) return off + 2 <= len ? off + 2 : 0;
        if (l & 0xC0) return 0;
        off += 1u + l;
    }
    return 0;
}

static void dns_rx(void *ctx, uint32_t src, uint16_t sport, const uint8_t *m, uint32_t len) {
    dns_query_t *q = ctx;
    if (sport != 53 || len < 12 || rd16be(m) != q->id || !(m[2] & 0x80) || q->result != DNS_PENDING) return;
    if (src != g_netcfg.dns && src != g_netcfg.dns2) return;
    int rcode = m[3] & 15;
    if (rcode == 3) {                       /* NXDOMAIN */
        cache_put(q->host, 0, DNS_ENOTFOUND, 10);
        finish(q, DNS_ENOTFOUND, 0);
        return;
    }
    if (rcode != 0) { q->sent = net_now() - 10000; return; }    /* retry / next server now */
    uint16_t qd = rd16be(m + 4), an = rd16be(m + 6);
    uint32_t off = 12;
    for (int i = 0; i < qd; i++) {
        off = skip_name(m, len, off);
        if (!off || off + 4 > len) return;
        off += 4;
    }
    for (int i = 0; i < an; i++) {
        off = skip_name(m, len, off);
        if (!off || off + 10 > len) return;
        uint16_t type = rd16be(m + off), cls = rd16be(m + off + 2);
        uint32_t ttl = rd32be(m + off + 4);
        uint16_t rdlen = rd16be(m + off + 8);
        off += 10;
        if (off + rdlen > len) return;
        if (type == 1 && cls == 1 && rdlen == 4) {
            uint32_t ip = rd32be(m + off);
            cache_put(q->host, ip, DNS_OK, ttl);
            finish(q, DNS_OK, ip);
            return;
        }
        off += rdlen;
    }
    cache_put(q->host, 0, DNS_ENOTFOUND, 10);            /* no A record (only CNAME / IPv6) */
    finish(q, DNS_ENOTFOUND, 0);
}

static void send_query(dns_query_t *q) {
    uint8_t m[300];
    memset(m, 0, 12);
    q->id = (uint16_t)random_u32();
    wr16be(m, q->id);
    m[2] = 0x01;                      /* recursion desired */
    wr16be(m + 4, 1);
    uint32_t o = 12;
    const char *s = q->host;
    while (*s) {
        const char *dot = strchr(s, '.');
        uint32_t l = dot ? (uint32_t)(dot - s) : (uint32_t)strlen(s);
        if (l == 0 || l > 63 || o + l + 6 > sizeof m) { finish(q, DNS_ENOTFOUND, 0); return; }
        m[o++] = (uint8_t)l;
        memcpy(m + o, s, l);
        o += l;
        s += l;
        if (*s == '.') s++;
    }
    m[o++] = 0;
    wr16be(m + o, 1); o += 2;         /* A */
    wr16be(m + o, 1); o += 2;         /* IN */
    if (!q->port) {
        for (int t = 0; t < 8 && !q->port; t++) {
            uint16_t p = net_ephemeral_port();
            if (udp_bind(p, dns_rx, q)) q->port = p;
        }
        if (!q->port) { finish(q, DNS_EFAIL, 0); return; }
    }
    uint32_t server = (q->tries >= 3 && g_netcfg.dns2) ? g_netcfg.dns2 : g_netcfg.dns;
    udp_send(server, q->port, 53, m, o);
    q->sent = net_now();
    q->tries++;
}

dns_query_t *dns_resolve(const char *host) {
    dns_query_t *q = kcalloc(1, sizeof *q);
    if (!q) return NULL;
    strlcpy(q->host, host, sizeof q->host);
    size_t n = strlen(q->host);
    if (n && q->host[n - 1] == '.') q->host[n - 1] = 0;
    for (char *p = q->host; *p; p++) *p = (char)tolower((unsigned char)*p);
    q->result = DNS_PENDING;
    uint32_t ip;
    if (net_parse_ipv4(q->host, &ip)) { q->result = DNS_OK; q->ip = ip; }
    else if (!q->host[0] || strlen(host) > 253) q->result = DNS_ENOTFOUND;
    else if (cache_lookup(q->host, &ip) != DNS_PENDING) { q->result = cache_lookup(q->host, &q->ip); }
    q->next = queries;
    queries = q;
    return q;
}

int dns_result(dns_query_t *q, uint32_t *ip) {
    if (q->result == DNS_OK && ip) *ip = q->ip;
    return q->result;
}

void dns_free(dns_query_t *q) {
    if (!q) return;
    for (dns_query_t **pp = &queries; *pp; pp = &(*pp)->next)
        if (*pp == q) { *pp = q->next; break; }
    if (q->port) udp_unbind(q->port);
    kfree(q);
}

void dns_tick(void) {
    uint32_t now = net_now();
    for (dns_query_t *q = queries; q; q = q->next) {
        if (q->result != DNS_PENDING) continue;
        if (!net_is_up()) { finish(q, DNS_EFAIL, 0); continue; }
        if (!q->started) { q->started = true; send_query(q); continue; }
        if (now - q->sent >= 2000) {
            int max = g_netcfg.dns2 ? 6 : 4;
            if (q->tries >= max) { NWARN("DNS: no answer for %s", q->host); finish(q, DNS_EFAIL, 0); }
            else send_query(q);
        }
    }
}
