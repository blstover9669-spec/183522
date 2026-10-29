/*
 * netcore.c - the bottom of the network stack: the device, Ethernet, ARP, IPv4,
 * ICMP (ping) and UDP, plus the 1 ms main-loop timer that drives everything.
 *
 * Every tick: check the link (every 500 ms), take received frames from the card
 * (bounded), then step DHCP, DNS, TCP and HTTP, then send the ACKs TCP collected.
 * Nothing here waits; a tick is normally a few microseconds.
 */
#include "net_internal.h"
#include "crypto/crypto.h"

net_config_t g_netcfg;
static netdev_t *ndev;
static bool link_state;
static uint32_t generation = 1;
static uint32_t last_link_check;
static uint32_t rx_packets, tx_packets, rx_bytes, tx_bytes;
static uint16_t ip_ident;
static uint8_t txbuf[1600];
static bool in_tick;

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

uint32_t net_now(void) { return g_ticks; }
netdev_t *net_device(void) { return ndev; }
bool net_link_up(void) { return ndev && link_state; }
void net_changed(void) { generation++; }
uint32_t net_generation(void) { return generation; }
uint32_t net_our_ip(void) { return g_netcfg.configured ? g_netcfg.ip : 0; }

bool net_is_up(void) { return ndev && link_state && g_netcfg.configured; }

char *net_ip_to_str(uint32_t ip, char *out) {
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return out;
}

const char *net_status_text(void) {
    static char buf[48];
    if (!ndev) return "No network adapter";
    if (!link_state) return "Network cable unplugged";
    if (!g_netcfg.configured) return "Connecting...";
    char ip[16];
    snprintf(buf, sizeof buf, "Connected (%s)", net_ip_to_str(g_netcfg.ip, ip));
    return buf;
}

void net_get_info(net_info_t *o) {
    memset(o, 0, sizeof *o);
    if (!ndev) return;
    o->adapter = true;
    o->link_up = link_state;
    o->configured = g_netcfg.configured;
    strlcpy(o->adapter_name, ndev->name, sizeof o->adapter_name);
    memcpy(o->mac, ndev->mac, 6);
    if (g_netcfg.configured) {
        o->ip = g_netcfg.ip;
        o->netmask = g_netcfg.netmask;
        o->gateway = g_netcfg.gateway;
        o->dns = g_netcfg.dns;
        o->lease_seconds = g_netcfg.lease_s;
    }
    o->rx_packets = rx_packets;
    o->tx_packets = tx_packets;
    o->rx_bytes = rx_bytes;
    o->tx_bytes = tx_bytes;
}

void net_set_config(const net_config_t *cfg) {
    if (cfg) g_netcfg = *cfg;
    else memset(&g_netcfg, 0, sizeof g_netcfg);
    net_changed();
}

/* ---- checksums ------------------------------------------------------------------------- */
uint32_t csum_partial(const void *data, uint32_t len, uint32_t sum) {
    const uint8_t *p = data;
    while (len >= 2) { sum += (uint32_t)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (uint32_t)(p[0] << 8);
    return sum;
}

uint16_t csum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint32_t csum_pseudo(uint32_t src, uint32_t dst, uint8_t proto, uint32_t len) {
    return (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + len;
}

/* ---- Ethernet ------------------------------------------------------------------------------ */
static bool eth_send(const uint8_t dst[6], uint16_t type, uint8_t *frame, uint32_t payload_len) {
    memcpy(frame, dst, 6);
    memcpy(frame + 6, ndev->mac, 6);
    wr16be(frame + 12, type);
    uint32_t len = 14 + payload_len;
    if (!ndev->send(frame, len)) return false;
    tx_packets++;
    tx_bytes += len;
    return true;
}

/* ---- ARP ------------------------------------------------------------------------------------- */
#define ARP_N       16
#define ARP_PENDING 8
typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  state;          /* 0 free, 1 resolving, 2 known */
    uint8_t  tries;
    uint32_t time;           /* learned / last request */
} arp_entry_t;
static arp_entry_t arp[ARP_N];

typedef struct {
    bool     used;
    uint32_t hop;
    uint32_t time;
    uint16_t len;
    uint8_t  frame[1514];
} arp_pending_t;
static arp_pending_t *pend;   /* ARP_PENDING entries */

static void arp_send(uint16_t op, const uint8_t *tha, uint32_t tpa, const uint8_t *eth_dst) {
    uint8_t f[64];
    memset(f, 0, sizeof f);
    uint8_t *a = f + 14;
    wr16be(a, 1);            /* Ethernet */
    wr16be(a + 2, 0x0800);   /* IPv4 */
    a[4] = 6;
    a[5] = 4;
    wr16be(a + 6, op);
    memcpy(a + 8, ndev->mac, 6);
    wr32be(a + 14, net_our_ip());
    memcpy(a + 18, tha, 6);
    wr32be(a + 24, tpa);
    eth_send(eth_dst, 0x0806, f, 28);
}

static arp_entry_t *arp_find(uint32_t ip) {
    for (int i = 0; i < ARP_N; i++)
        if (arp[i].state && arp[i].ip == ip) return &arp[i];
    return NULL;
}

static arp_entry_t *arp_slot(uint32_t ip) {
    arp_entry_t *e = arp_find(ip);
    if (e) return e;
    arp_entry_t *old = &arp[0];
    for (int i = 0; i < ARP_N; i++) {
        if (!arp[i].state) return &arp[i];
        if ((int32_t)(arp[i].time - old->time) < 0) old = &arp[i];
    }
    return old;
}

static void arp_learn(uint32_t ip, const uint8_t *mac) {
    if (!ip) return;
    arp_entry_t *e = arp_slot(ip);
    e->ip = ip;
    memcpy(e->mac, mac, 6);
    e->state = 2;
    e->tries = 0;
    e->time = net_now();
    if (!pend) return;
    for (int i = 0; i < ARP_PENDING; i++) {
        if (!pend[i].used || pend[i].hop != ip) continue;
        memcpy(pend[i].frame, mac, 6);
        if (ndev->send(pend[i].frame, pend[i].len)) { tx_packets++; tx_bytes += pend[i].len; }
        pend[i].used = false;
    }
}

static void arp_input(const uint8_t *a, uint32_t len) {
    if (len < 28 || rd16be(a) != 1 || rd16be(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4) return;
    uint16_t op = rd16be(a + 6);
    uint32_t spa = rd32be(a + 14), tpa = rd32be(a + 24);
    uint32_t me = net_our_ip();
    if (arp_find(spa) || (me && tpa == me)) arp_learn(spa, a + 8);
    if (op == 1 && me && tpa == me) arp_send(2, a + 8, spa, a + 8);
}

static void arp_tick(void) {
    uint32_t now = net_now();
    for (int i = 0; i < ARP_N; i++) {
        arp_entry_t *e = &arp[i];
        if (e->state == 1 && now - e->time >= 1000) {
            if (++e->tries > 3) { e->state = 0; continue; }
            e->time = now;
            static const uint8_t zero[6];
            arp_send(1, zero, e->ip, BCAST);
        } else if (e->state == 2 && now - e->time > 10 * 60 * 1000) {
            e->state = 0;                     /* forget after 10 minutes */
        }
    }
    if (pend)
        for (int i = 0; i < ARP_PENDING; i++)
            if (pend[i].used && now - pend[i].time > 3000) pend[i].used = false;
}

/* Send a frame whose IPv4 next hop is `hop`; frame has room for the Ethernet header. */
static bool send_via(uint32_t hop, uint8_t *frame, uint32_t payload_len) {
    arp_entry_t *e = arp_find(hop);
    if (e && e->state == 2) return eth_send(e->mac, 0x0800, frame, payload_len);
    if (!pend) pend = kcalloc(ARP_PENDING, sizeof *pend);
    if (!pend) return false;
    int slot = -1;
    for (int i = 0; i < ARP_PENDING; i++) if (!pend[i].used) { slot = i; break; }
    if (slot < 0) return false;
    memcpy(frame + 6, ndev->mac, 6);
    wr16be(frame + 12, 0x0800);
    pend[slot].used = true;
    pend[slot].hop = hop;
    pend[slot].time = net_now();
    pend[slot].len = (uint16_t)(14 + payload_len);
    memcpy(pend[slot].frame, frame, 14 + payload_len);
    if (!e) {
        e = arp_slot(hop);
        e->ip = hop;
        e->state = 1;
        e->tries = 0;
        e->time = net_now();
        static const uint8_t zero[6];
        arp_send(1, zero, hop, BCAST);
    }
    return true;
}

/* ---- IPv4 ----------------------------------------------------------------------------------- */
bool ip_send(uint32_t dst, uint8_t proto, const void *payload, uint32_t len) {
    if (!ndev || !link_state || len > ETH_MTU - 20) return false;
    uint8_t *f = txbuf;
    uint8_t *ip = f + 14;
    ip[0] = 0x45;
    ip[1] = 0;
    wr16be(ip + 2, (uint16_t)(20 + len));
    wr16be(ip + 4, ip_ident++);
    wr16be(ip + 6, 0x4000);            /* don't fragment */
    ip[8] = 64;
    ip[9] = proto;
    wr16be(ip + 10, 0);
    wr32be(ip + 12, net_our_ip());
    wr32be(ip + 16, dst);
    wr16be(ip + 10, csum_fold(csum_partial(ip, 20, 0)));
    memcpy(ip + 20, payload, len);
    uint32_t plen = 20 + len;
    if (dst == 0xFFFFFFFFu || !g_netcfg.configured) return eth_send(BCAST, 0x0800, f, plen);
    uint32_t hop = dst;
    uint32_t mask = g_netcfg.netmask ? g_netcfg.netmask : 0xFFFFFF00u;
    if ((dst & mask) != (g_netcfg.ip & mask)) {
        if (!g_netcfg.gateway) return false;
        hop = g_netcfg.gateway;
    }
    if ((dst | mask) == 0xFFFFFFFFu) return eth_send(BCAST, 0x0800, f, plen);   /* subnet broadcast */
    return send_via(hop, f, plen);
}

/* ---- ICMP ----------------------------------------------------------------------------------- */
#define PING_N 8
static struct { bool used; uint32_t dst; uint16_t id, seq; uint32_t sent; ping_fn fn; void *ctx; } pings[PING_N];

bool icmp_ping(uint32_t dst, uint16_t id, uint16_t seq, ping_fn fn, void *ctx) {
    int slot = -1;
    for (int i = 0; i < PING_N; i++) if (!pings[i].used || net_now() - pings[i].sent > 10000) { slot = i; break; }
    if (slot < 0) return false;
    uint8_t m[8 + 32];
    m[0] = 8;
    m[1] = 0;
    wr16be(m + 2, 0);
    wr16be(m + 4, id);
    wr16be(m + 6, seq);
    for (int i = 0; i < 32; i++) m[8 + i] = (uint8_t)('a' + i % 23);
    wr16be(m + 2, csum_fold(csum_partial(m, sizeof m, 0)));
    pings[slot].used = true;
    pings[slot].dst = dst;
    pings[slot].id = id;
    pings[slot].seq = seq;
    pings[slot].sent = net_now();
    pings[slot].fn = fn;
    pings[slot].ctx = ctx;
    return ip_send(dst, IP_PROTO_ICMP, m, sizeof m);
}

void icmp_cancel(void *ctx) {
    for (int i = 0; i < PING_N; i++) if (pings[i].ctx == ctx) pings[i].used = false;
}

static void icmp_input(uint32_t src, int ttl, const uint8_t *m, uint32_t len) {
    if (len < 8 || csum_fold(csum_partial(m, len, 0)) != 0) return;
    if (m[0] == 8 && m[1] == 0) {                       /* echo request -> reply */
        uint8_t *r = kmalloc(len);
        if (!r) return;
        memcpy(r, m, len);
        r[0] = 0;
        wr16be(r + 2, 0);
        wr16be(r + 2, csum_fold(csum_partial(r, len, 0)));
        ip_send(src, IP_PROTO_ICMP, r, len);
        kfree(r);
    } else if (m[0] == 0) {                             /* echo reply */
        uint16_t id = rd16be(m + 4), seq = rd16be(m + 6);
        for (int i = 0; i < PING_N; i++) {
            if (!pings[i].used || pings[i].id != id || pings[i].seq != seq) continue;
            pings[i].used = false;
            if (pings[i].fn) pings[i].fn(pings[i].ctx, src, seq, net_now() - pings[i].sent, ttl);
            break;
        }
    }
}

/* ---- UDP ------------------------------------------------------------------------------------ */
#define UDP_N 16
static struct { uint16_t port; udp_rx_fn fn; void *ctx; } udp_binds[UDP_N];

bool udp_bind(uint16_t port, udp_rx_fn fn, void *ctx) {
    for (int i = 0; i < UDP_N; i++) if (udp_binds[i].port == port) return false;
    for (int i = 0; i < UDP_N; i++)
        if (!udp_binds[i].port) { udp_binds[i].port = port; udp_binds[i].fn = fn; udp_binds[i].ctx = ctx; return true; }
    return false;
}

void udp_unbind(uint16_t port) {
    for (int i = 0; i < UDP_N; i++) if (udp_binds[i].port == port) udp_binds[i].port = 0;
}

uint16_t net_ephemeral_port(void) {
    for (;;) {
        uint16_t p = (uint16_t)(49152 + random_u32() % 16384);
        bool used = false;
        for (int i = 0; i < UDP_N; i++) if (udp_binds[i].port == p) used = true;
        if (!used) return p;
    }
}

bool udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, uint32_t len) {
    if (len > ETH_MTU - 28) return false;
    uint8_t *u = kmalloc(8 + len);
    if (!u) return false;
    wr16be(u, sport);
    wr16be(u + 2, dport);
    wr16be(u + 4, (uint16_t)(8 + len));
    wr16be(u + 6, 0);
    memcpy(u + 8, data, len);
    uint16_t cs = csum_fold(csum_partial(u, 8 + len, csum_pseudo(net_our_ip(), dst, IP_PROTO_UDP, 8 + len)));
    wr16be(u + 6, cs ? cs : 0xFFFF);
    bool ok = ip_send(dst, IP_PROTO_UDP, u, 8 + len);
    kfree(u);
    return ok;
}

static void udp_input(uint32_t src, uint32_t dst, const uint8_t *u, uint32_t len) {
    if (len < 8) return;
    uint16_t ulen = rd16be(u + 4);
    if (ulen < 8 || ulen > len) return;
    if (rd16be(u + 6) && csum_fold(csum_partial(u, ulen, csum_pseudo(src, dst, IP_PROTO_UDP, ulen))) != 0) return;
    uint16_t dport = rd16be(u + 2);
    for (int i = 0; i < UDP_N; i++)
        if (udp_binds[i].port == dport) { udp_binds[i].fn(udp_binds[i].ctx, src, rd16be(u), u + 8, ulen - 8u); return; }
}

static void ip_input(const uint8_t *ip, uint32_t len) {
    if (len < 20 || (ip[0] >> 4) != 4) return;
    uint32_t hl = (ip[0] & 15u) * 4;
    uint32_t tl = rd16be(ip + 2);
    if (hl < 20 || tl < hl || tl > len) return;
    if (csum_fold(csum_partial(ip, hl, 0)) != 0) return;
    if (rd16be(ip + 6) & 0x3FFF) return;                /* fragments are not supported */
    uint32_t src = rd32be(ip + 12), dst = rd32be(ip + 16);
    uint32_t me = net_our_ip();
    if (me && dst != me && dst != 0xFFFFFFFFu && (dst | g_netcfg.netmask) != 0xFFFFFFFFu) return;
    const uint8_t *p = ip + hl;
    uint32_t plen = tl - hl;
    switch (ip[9]) {
    case IP_PROTO_ICMP: icmp_input(src, ip[8], p, plen); break;
    case IP_PROTO_UDP:  udp_input(src, dst, p, plen); break;
    case IP_PROTO_TCP:  if (me && dst == me) tcp_input(src, dst, p, plen); break;
    }
}

static void eth_input(const uint8_t *f, uint32_t len) {
    rx_packets++;
    rx_bytes += len;
    random_add_timing();
    if (len < 14) return;
    uint16_t type = rd16be(f + 12);
    if (type == 0x0806) arp_input(f + 14, len - 14);
    else if (type == 0x0800) ip_input(f + 14, len - 14);
}

/* ---- the tick ---------------------------------------------------------------------------------- */
static void net_tick(void *ctx) {
    (void)ctx;
    if (in_tick) return;
    in_tick = true;
    if (!ndev) {                     /* no card: only finish HTTP requests (with "no network") */
        http_tick();
        in_tick = false;
        return;
    }
    uint32_t now = net_now();
    if (now - last_link_check >= 500 || !last_link_check) {
        last_link_check = now ? now : 1;
        bool up = ndev->link_up();
        if (up != link_state) {
            link_state = up;
            NLOG("link %s (%s)", up ? "up" : "down", ndev->name);
            net_changed();
            if (up) dhcp_start();
            else { dhcp_stop(); net_set_config(NULL); }
        }
    }
    for (int round = 0; round < 4; round++)
        if (ndev->poll(eth_input, 32) < 32) break;
    arp_tick();
    dhcp_tick();
    dns_tick();
    tcp_tick();
    http_tick();
    tcp_flush();
    in_tick = false;
}

/* The 1 ms tick runs once a card is registered, or once an HTTP request is made
   without one (so its callback still reports HTTP_ERR_NO_NETWORK). */
void net_start_tick(void) {
    static int timer;
    if (!timer) timer = timer_add(1, net_tick, NULL, true);
}

void net_register_device(netdev_t *d) {
    if (ndev) return;
    ndev = d;
    net_changed();
    net_start_tick();
}

/* Weak defaults so the stack links while layers are being added. */
__attribute__((weak)) void tcp_input(uint32_t s, uint32_t d, const uint8_t *g, uint32_t l) { (void)s; (void)d; (void)g; (void)l; }
__attribute__((weak)) void tcp_tick(void) {}
__attribute__((weak)) void tcp_flush(void) {}
__attribute__((weak)) void http_tick(void) {}
__attribute__((weak)) int tcp_count(void) { return 0; }
