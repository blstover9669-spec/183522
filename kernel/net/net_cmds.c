/*
 * net_cmds.c - Terminal / Run dialog commands for checking the network:
 *   net                  adapter, link, address, gateway, DNS, packet counters
 *   ping HOST [COUNT]    ICMP echo (results in the system log: logs net)
 *   nslookup HOST        DNS lookup (result in the system log)
 *   dhcp [renew]         the DHCP lease
 *   fetch URL [FILE]     HTTP / HTTPS download (result in the system log)
 *   cookies [clear]      the cookie jar
 * Commands must return at once, so ping / nslookup start the work and report
 * through log lines ("[net] ..." on the serial log too).
 */
#include "net_internal.h"
#include "crypto/crypto.h"

static int cmd_net(int argc, char **argv, cmd_writer_t *out) {
    (void)argc; (void)argv;
    net_info_t i;
    net_get_info(&i);
    cmd_printf(out, "Status:   %s\n", net_status_text());
    if (!i.adapter) return 1;
    char a[16], b[16], c[16], d[16];
    cmd_printf(out, "Adapter:  %s\n", i.adapter_name);
    cmd_printf(out, "MAC:      %02x:%02x:%02x:%02x:%02x:%02x\n", i.mac[0], i.mac[1], i.mac[2], i.mac[3], i.mac[4], i.mac[5]);
    cmd_printf(out, "Link:     %s\n", i.link_up ? "up" : "down");
    if (i.configured) {
        cmd_printf(out, "Address:  %s  netmask %s\n", net_ip_to_str(i.ip, a), net_ip_to_str(i.netmask, b));
        cmd_printf(out, "Gateway:  %s  DNS %s\n", net_ip_to_str(i.gateway, c), net_ip_to_str(i.dns, d));
        cmd_printf(out, "Lease:    %u s\n", i.lease_seconds);
    }
    cmd_printf(out, "Received: %u packets, %u bytes\n", i.rx_packets, i.rx_bytes);
    cmd_printf(out, "Sent:     %u packets, %u bytes\n", i.tx_packets, i.tx_bytes);
    cmd_printf(out, "TCP:      %d connection(s)\n", tcp_count());
    return 0;
}
REGISTER_COMMAND("net", "net - network status (adapter, address, gateway, DNS)", cmd_net);

/* ---- ping ------------------------------------------------------------------------------- */
typedef struct {
    dns_query_t *q;
    char host[128];
    uint32_t ip;
    int count, sent, received;
    uint16_t id;
    uint32_t next;
    int timer;
} ping_job_t;

static void ping_reply(void *ctx, uint32_t from, uint16_t seq, uint32_t rtt, int ttl) {
    ping_job_t *j = ctx;
    char a[16];
    j->received++;
    NLOG("ping %s: reply from %s seq=%u ttl=%d time=%u ms", j->host, net_ip_to_str(from, a), seq, ttl, rtt);
}

static void ping_step(void *ctx) {
    ping_job_t *j = ctx;
    uint32_t now = net_now();
    if (j->q) {
        int r = dns_result(j->q, &j->ip);
        if (r == DNS_PENDING) return;
        dns_free(j->q);
        j->q = NULL;
        if (r != DNS_OK) {
            NLOG("ping %s: %s", j->host, r == DNS_ENOTFOUND ? "unknown host" : "DNS failed");
            timer_cancel(j->timer);
            kfree(j);
            return;
        }
        j->next = now;
    }
    if (j->sent < j->count && (int32_t)(now - j->next) >= 0) {
        j->sent++;
        if (!icmp_ping(j->ip, j->id, (uint16_t)j->sent, ping_reply, j))
            NLOG("ping %s: could not send", j->host);
        j->next = now + 1000;
    }
    if (j->sent >= j->count && (int32_t)(now - j->next) >= 1000) {
        NLOG("ping %s: %d sent, %d received", j->host, j->sent, j->received);
        timer_cancel(j->timer);
        icmp_cancel(j);
        kfree(j);
    }
}

static int cmd_ping(int argc, char **argv, cmd_writer_t *out) {
    if (argc < 2) { cmd_puts(out, "usage: ping HOST [COUNT]\n"); return 2; }
    if (!net_is_up()) { cmd_printf(out, "ping: %s\n", net_status_text()); return 1; }
    ping_job_t *j = kcalloc(1, sizeof *j);
    if (!j) return 1;
    strlcpy(j->host, argv[1], sizeof j->host);
    j->count = argc > 2 ? CLAMP(atoi(argv[2]), 1, 100) : 4;
    j->id = (uint16_t)(net_now() ^ 0x4e56);
    j->q = dns_resolve(j->host);
    j->timer = timer_add(20, ping_step, j, true);
    cmd_printf(out, "Pinging %s %d times - replies go to the system log (logs net)\n", j->host, j->count);
    return 0;
}
REGISTER_COMMAND("ping", "ping HOST [COUNT] - send ICMP echo requests (results: logs net)", cmd_ping);

/* ---- nslookup ----------------------------------------------------------------------------- */
typedef struct { dns_query_t *q; char host[128]; int timer; uint32_t start; } ns_job_t;

static void ns_step(void *ctx) {
    ns_job_t *j = ctx;
    uint32_t ip;
    int r = dns_result(j->q, &ip);
    if (r == DNS_PENDING) return;
    char a[16];
    if (r == DNS_OK) NLOG("nslookup %s: %s (%u ms)", j->host, net_ip_to_str(ip, a), net_now() - j->start);
    else NLOG("nslookup %s: %s", j->host, r == DNS_ENOTFOUND ? "not found" : "no answer from the DNS server");
    dns_free(j->q);
    timer_cancel(j->timer);
    kfree(j);
}

static int cmd_nslookup(int argc, char **argv, cmd_writer_t *out) {
    if (argc < 2) { cmd_puts(out, "usage: nslookup HOST\n"); return 2; }
    if (!net_is_up()) { cmd_printf(out, "nslookup: %s\n", net_status_text()); return 1; }
    ns_job_t *j = kcalloc(1, sizeof *j);
    if (!j) return 1;
    strlcpy(j->host, argv[1], sizeof j->host);
    j->q = dns_resolve(j->host);
    j->start = net_now();
    if (!j->q) { kfree(j); return 1; }
    j->timer = timer_add(20, ns_step, j, true);
    cmd_printf(out, "Looking up %s - the answer goes to the system log (logs net)\n", j->host);
    return 0;
}
REGISTER_COMMAND("nslookup", "nslookup HOST - look up a host name with DNS (result: logs net)", cmd_nslookup);

/* ---- dhcp --------------------------------------------------------------------------------- */
static int cmd_dhcp(int argc, char **argv, cmd_writer_t *out) {
    if (argc > 1 && !strcmp(argv[1], "renew")) {
        if (!net_link_up()) { cmd_printf(out, "dhcp: %s\n", net_status_text()); return 1; }
        dhcp_start();
        cmd_puts(out, "Asking the DHCP server for an address again (logs net)\n");
        return 0;
    }
    net_info_t i;
    net_get_info(&i);
    if (!i.configured) { cmd_printf(out, "%s\n", net_status_text()); return 1; }
    char a[16], b[16], c[16], d[16];
    cmd_printf(out, "%s/%s gateway %s DNS %s lease %u s\n", net_ip_to_str(i.ip, a), net_ip_to_str(i.netmask, b),
               net_ip_to_str(i.gateway, c), net_ip_to_str(i.dns, d), i.lease_seconds);
    return 0;
}
REGISTER_COMMAND("dhcp", "dhcp [renew] - show the DHCP lease, or ask for an address again", cmd_dhcp);

/* ---- fetch -------------------------------------------------------------------------------- */
typedef struct { char path[256]; uint32_t start; bool verbose; } fetch_job_t;

static void fetch_done(http_request_t *r, void *ctx) {
    fetch_job_t *j = ctx;
    uint32_t ms = net_now() - j->start;
    if (http_error(r) != HTTP_OK) {
        NLOG("fetch: FAILED (%s) after %u ms: %s", http_strerror(http_error(r)), ms, http_error_text(r));
        if (http_cert_error(r)[0]) NLOG("fetch: certificate: %s", http_cert_error(r));
    } else {
        uint8_t h[32];
        char hex[65];
        sha256(http_body(r) ? http_body(r) : (const uint8_t *)"", http_body_len(r), h);
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", h[i]);
        NLOG("fetch: %d %s, %u bytes in %u ms, %d redirect(s), final URL %s", http_status(r), http_reason(r),
             http_body_len(r), ms, http_redirect_count(r), http_url(r));
        NLOG("fetch: body sha256 %s", hex);
        if (http_is_https(r))
            NLOG("fetch: %s; certificate %s; subject \"%s\", issuer \"%s\"", http_tls_info(r),
                 http_cert_ok(r) ? "verified" : http_cert_error(r), http_cert_subject(r), http_cert_issuer(r));
        if (j->verbose)
            for (int i = 0; i < http_header_count(r); i++) {
                const char *n, *v;
                if (http_header_at(r, i, &n, &v)) NLOG("fetch: header %s: %s", n, v);
            }
        if (j->path[0]) {
            int e = vfs_write_file(j->path, http_body(r) ? http_body(r) : (const uint8_t *)"", http_body_len(r));
            if (e < 0) NLOG("fetch: could not save %s: %s", j->path, vfs_strerror(e));
            else NLOG("fetch: saved %s", j->path);
        } else if (http_body_len(r)) {
            char line[121];
            uint32_t n = MIN(http_body_len(r), sizeof line - 1);
            for (uint32_t i = 0; i < n; i++) {
                uint8_t ch = http_body(r)[i];
                line[i] = (ch >= 32 && ch < 127) ? (char)ch : ' ';
            }
            line[n] = 0;
            NLOG("fetch: body starts: %s", line);
        }
    }
    http_free(r);
    kfree(j);
}

static int cmd_fetch(int argc, char **argv, cmd_writer_t *out) {
    const char *url = NULL, *file = NULL;
    int copies = 1;
    http_options_t o = { 0 };
    fetch_job_t *j = kcalloc(1, sizeof *j);
    if (!j) return 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k")) o.flags |= HTTP_F_ALLOW_BAD_CERT;
        else if (!strcmp(argv[i], "-v")) j->verbose = true;
        else if (!strcmp(argv[i], "-n")) o.flags |= HTTP_F_NO_REDIRECT;
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) { int v = atoi(argv[++i]); copies = CLAMP(v, 1, 32); }
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) { o.body = argv[++i]; o.body_len = (uint32_t)strlen(argv[i]);
                                                           o.content_type = "application/x-www-form-urlencoded"; }
        else if (!url) url = argv[i];
        else if (!file) file = argv[i];
    }
    if (!url) {
        cmd_puts(out, "usage: fetch [-k] [-v] [-n] [-r N] [-d FORMDATA] URL [FILE]\n"
                      "  -k accept a bad certificate, -v log the headers, -n don't follow redirects,\n"
                      "  -r start N copies at once, -d send a POST body; FILE saves the body\n");
        kfree(j);
        return 2;
    }
    if (file) cmd_resolve_path(out, file, j->path);
    j->start = net_now();
    for (int k = 1; k < copies; k++) {
        fetch_job_t *c = kmalloc(sizeof *c);
        if (!c) break;
        *c = *j;
        if (!http_fetch_ex(url, &o, fetch_done, c)) { kfree(c); break; }
    }
    if (!http_fetch_ex(url, &o, fetch_done, j)) { kfree(j); cmd_puts(out, "fetch: out of memory\n"); return 1; }
    cmd_printf(out, "Fetching %s%s - the result goes to the system log (logs net)\n", url, copies > 1 ? " (several times)" : "");
    return 0;
}
REGISTER_COMMAND("fetch", "fetch [-k] [-v] [-r N] URL [FILE] - download a web page (result: logs net)", cmd_fetch);

/* ---- cookies ------------------------------------------------------------------------------ */
static int cmd_cookies(int argc, char **argv, cmd_writer_t *out) {
    if (argc > 1 && !strcmp(argv[1], "clear")) {
        cookies_clear();
        cmd_puts(out, "All cookies deleted\n");
        return 0;
    }
    cmd_printf(out, "%d cookie(s) stored (cookies clear deletes them)\n", cookies_count());
    return 0;
}
REGISTER_COMMAND("cookies", "cookies [clear] - count or delete the browser cookies", cmd_cookies);
