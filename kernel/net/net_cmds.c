/*
 * net_cmds.c - Terminal / Run dialog commands for checking the network:
 *   net                  adapter, link, address, gateway, DNS, packet counters
 *   ping HOST [COUNT]    ICMP echo (results in the system log: logs net)
 *   nslookup HOST        DNS lookup (result in the system log)
 * Commands must return at once, so ping / nslookup start the work and report
 * through log lines ("[net] ..." on the serial log too).
 */
#include "net_internal.h"

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
