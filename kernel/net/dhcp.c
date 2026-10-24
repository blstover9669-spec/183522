/*
 * dhcp.c - DHCP client (RFC 2131): DISCOVER -> OFFER -> REQUEST -> ACK, renewal at
 * half the lease, and a fresh start when the lease runs out or the server says NAK.
 * VirtualBox NAT and QEMU user networking both answer with 10.0.2.15/24, gateway
 * 10.0.2.2 and DNS 10.0.2.3.
 */
#include "net_internal.h"
#include "crypto/crypto.h"

enum { D_IDLE, D_SELECTING, D_REQUESTING, D_BOUND, D_RENEWING };

static int state = D_IDLE;
static uint32_t xid;
static uint32_t next_send, retry_ms, tries;
static uint32_t offered_ip, server_id;
static uint32_t bound_at, lease_s, t1_s, t2_s;
static net_config_t offer;
static bool bound_port;

static void dhcp_rx(void *ctx, uint32_t src, uint16_t sport, const uint8_t *d, uint32_t len);

static void send_msg(int type) {
    netdev_t *dev = net_device();
    uint8_t *m = kcalloc(1, 548);
    if (!m) return;
    m[0] = 1;                 /* BOOTREQUEST */
    m[1] = 1;                 /* Ethernet */
    m[2] = 6;
    wr32be(m + 4, xid);
    wr16be(m + 8, (uint16_t)MIN(0xFFFFu, (net_now() - bound_at) / 1000));
    if (state == D_RENEWING) wr32be(m + 12, g_netcfg.ip);    /* ciaddr */
    else wr16be(m + 10, 0x8000);                             /* broadcast replies please */
    memcpy(m + 28, dev->mac, 6);
    uint8_t *o = m + 236;
    wr32be(o, 0x63825363);    /* magic cookie */
    o += 4;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, dev->mac, 6); o += 6;          /* client id */
    *o++ = 12; *o++ = 6; memcpy(o, "novaos", 6); o += 6;                    /* host name */
    if (type == 3 && state == D_REQUESTING) {
        *o++ = 50; *o++ = 4; wr32be(o, offered_ip); o += 4;                 /* requested IP */
        *o++ = 54; *o++ = 4; wr32be(o, server_id); o += 4;                  /* server id */
    }
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;          /* want: mask, router, DNS, lease */
    *o++ = 57; *o++ = 2; wr16be(o, 576); o += 2;
    *o++ = 255;
    uint32_t len = (uint32_t)(o - m);
    if (len < 300) len = 300;
    uint32_t dst = 0xFFFFFFFFu;
    udp_send(dst, 68, 67, m, len);
    kfree(m);
}

static void begin(void) {
    xid = random_u32();
    state = D_SELECTING;
    tries = 0;
    retry_ms = 2000;
    next_send = net_now();
    bound_at = net_now();
}

void dhcp_start(void) {
    if (!bound_port) bound_port = udp_bind(68, dhcp_rx, NULL);
    NLOG("DHCP: asking for an address");
    begin();
}

void dhcp_stop(void) {
    state = D_IDLE;
}

static void dhcp_rx(void *ctx, uint32_t src, uint16_t sport, const uint8_t *m, uint32_t len) {
    (void)ctx; (void)src;
    if (sport != 67 || len < 240 || m[0] != 2 || rd32be(m + 4) != xid || rd32be(m + 236) != 0x63825363) return;
    if (memcmp(m + 28, net_device()->mac, 6) != 0) return;
    int type = 0;
    net_config_t c = { 0 };
    uint32_t sid = 0, lease = 0;
    c.ip = rd32be(m + 16);
    for (uint32_t i = 240; i < len;) {
        uint8_t code = m[i];
        if (code == 0) { i++; continue; }
        if (code == 255 || i + 1 >= len) break;
        uint8_t l = m[i + 1];
        const uint8_t *v = m + i + 2;
        if (i + 2 + l > len) break;
        switch (code) {
        case 53: if (l >= 1) type = v[0]; break;
        case 1:  if (l >= 4) c.netmask = rd32be(v); break;
        case 3:  if (l >= 4) c.gateway = rd32be(v); break;
        case 6:  if (l >= 4) c.dns = rd32be(v); if (l >= 8) c.dns2 = rd32be(v + 4); break;
        case 51: if (l >= 4) lease = rd32be(v); break;
        case 54: if (l >= 4) sid = rd32be(v); break;
        }
        i += 2u + l;
    }
    if (type == 2 && state == D_SELECTING) {            /* OFFER */
        offered_ip = c.ip;
        server_id = sid ? sid : src;
        offer = c;
        state = D_REQUESTING;
        tries = 0;
        retry_ms = 2000;
        send_msg(3);
        next_send = net_now() + retry_ms;
    } else if (type == 5 && (state == D_REQUESTING || state == D_RENEWING)) {   /* ACK */
        if (!c.netmask) c.netmask = offer.netmask ? offer.netmask : 0xFFFFFF00u;
        if (!c.gateway) c.gateway = offer.gateway;
        if (!c.dns) { c.dns = offer.dns; c.dns2 = offer.dns2; }
        if (!c.dns) c.dns = c.gateway;
        c.lease_s = lease ? lease : 86400;
        c.configured = true;
        bool was = g_netcfg.configured && g_netcfg.ip == c.ip;
        if (sid) server_id = sid;
        state = D_BOUND;
        bound_at = net_now();
        lease_s = c.lease_s;
        t1_s = lease_s / 2;
        t2_s = lease_s / 8 * 7;
        net_set_config(&c);
        if (!was) {
            char a[16], g[16], d[16];
            int bits = __builtin_popcount(c.netmask);
            NLOG("DHCP: address %s/%d, gateway %s, DNS %s, lease %u s", net_ip_to_str(c.ip, a), bits,
                 net_ip_to_str(c.gateway, g), net_ip_to_str(c.dns, d), c.lease_s);
        }
    } else if (type == 6) {                             /* NAK */
        NWARN("DHCP: server refused the address, starting over");
        net_set_config(NULL);
        begin();
    }
}

void dhcp_tick(void) {
    uint32_t now = net_now();
    switch (state) {
    case D_SELECTING:
        if ((int32_t)(now - next_send) >= 0) {
            send_msg(1);
            tries++;
            next_send = now + retry_ms;
            if (retry_ms < 16000) retry_ms *= 2;
        }
        break;
    case D_REQUESTING:
        if ((int32_t)(now - next_send) >= 0) {
            if (++tries > 3) { begin(); break; }
            send_msg(3);
            next_send = now + retry_ms;
        }
        break;
    case D_BOUND:
        if ((now - bound_at) / 1000 >= t1_s) {
            state = D_RENEWING;
            xid = random_u32();
            next_send = now;
            retry_ms = 10000;
        }
        break;
    case D_RENEWING:
        if ((now - bound_at) / 1000 >= lease_s) {
            NWARN("DHCP: lease expired");
            net_set_config(NULL);
            begin();
        } else if ((int32_t)(now - next_send) >= 0) {
            send_msg(3);
            next_send = now + retry_ms;
        }
        break;
    }
}
