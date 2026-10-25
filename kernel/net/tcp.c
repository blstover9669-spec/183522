/*
 * tcp.c - TCP client connections (RFC 793 / 9293).
 *
 * Active open only (NovaOS never listens). 64 KiB receive ring per connection
 * (advertised window = free space, no window scaling), 32 KiB send buffer,
 * MSS option, go-back-N retransmission with exponential backoff (1 s .. 30 s,
 * 8 tries), delayed ACKs collected during a tick and sent by tcp_flush(),
 * window updates when the application frees buffer space, zero-window probes,
 * a short TIME_WAIT. Out-of-order segments are dropped and re-ACKed (both
 * VirtualBox NAT and QEMU user networking deliver in order; the sender
 * retransmits anything we drop).
 *
 * Users poll: tcp_state / tcp_readable / tcp_read / tcp_write. tcp_close()
 * hands the connection back; the stack finishes the close handshake itself.
 */
#include "net_internal.h"
#include "crypto/crypto.h"

enum { ST_SYN_SENT, ST_ESTABLISHED, ST_FIN_WAIT_1, ST_FIN_WAIT_2, ST_CLOSE_WAIT, ST_CLOSING,
       ST_LAST_ACK, ST_TIME_WAIT, ST_CLOSED };

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

#define RCAP   (64 * 1024)
#define SCAP   (32 * 1024)
#define RTO_MIN   1000
#define RTO_MAX   30000
#define MAX_RETRIES 8
#define SYN_RETRIES 4

struct tcp_conn {
    struct tcp_conn *next;
    int      st;
    int      err;
    bool     released;            /* tcp_close() was called */
    uint32_t rip;
    uint16_t rport, lport;
    /* send side */
    uint32_t iss, snd_una, snd_nxt, snd_wnd;
    uint8_t *sbuf;                /* sbuf[0] is the byte at snd_una (after the SYN) */
    uint32_t slen;
    bool     fin_queued, fin_sent;
    uint16_t mss;
    uint32_t rto, rto_due;        /* rto_due 0 = timer off */
    int      retries;
    /* receive side */
    uint32_t rcv_nxt;
    uint8_t *rbuf;
    uint32_t rhead, rlen;
    bool     fin_rcvd;
    bool     ack_now;
    uint32_t adv_wnd;             /* window we last advertised */
    uint32_t until;               /* TIME_WAIT / linger deadline */
};

static tcp_conn_t *conns;

int tcp_count(void) {
    int n = 0;
    for (tcp_conn_t *c = conns; c; c = c->next) if (c->st != ST_CLOSED) n++;
    return n;
}

static inline bool seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline bool seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

static uint32_t rfree(tcp_conn_t *c) { return RCAP - c->rlen; }

static void send_seg(tcp_conn_t *c, uint32_t seq, uint8_t flags, const uint8_t *data, uint32_t len) {
    uint8_t seg[24 + 1460];
    uint32_t hl = (flags & F_SYN) ? 24 : 20;
    if (len > c->mss) len = c->mss;
    wr16be(seg, c->lport);
    wr16be(seg + 2, c->rport);
    wr32be(seg + 4, seq);
    wr32be(seg + 8, (flags & F_ACK) ? c->rcv_nxt : 0);
    seg[12] = (uint8_t)((hl / 4) << 4);
    seg[13] = flags;
    uint32_t wnd = rfree(c);
    if (c->released) wnd = RCAP;
    if (wnd > 65535) wnd = 65535;
    wr16be(seg + 14, (uint16_t)wnd);
    wr16be(seg + 16, 0);
    wr16be(seg + 18, 0);
    if (hl == 24) { seg[20] = 2; seg[21] = 4; wr16be(seg + 22, 1460); }
    if (len) memcpy(seg + hl, data, len);
    uint32_t tl = hl + len;
    wr16be(seg + 16, csum_fold(csum_partial(seg, tl, csum_pseudo(net_our_ip(), c->rip, IP_PROTO_TCP, tl))));
    if (flags & F_ACK) { c->adv_wnd = wnd; c->ack_now = false; }
    ip_send(c->rip, IP_PROTO_TCP, seg, tl);
}

static void send_rst_for(uint32_t src, const uint8_t *seg, uint32_t len) {
    /* answer a segment for an unknown connection with RST */
    uint8_t flags = seg[13];
    if (flags & F_RST) return;
    uint32_t hl = (uint32_t)(seg[12] >> 4) * 4;
    uint32_t dlen = len - hl + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0);
    uint8_t r[20];
    memset(r, 0, sizeof r);
    wr16be(r, rd16be(seg + 2));
    wr16be(r + 2, rd16be(seg));
    if (flags & F_ACK) { wr32be(r + 4, rd32be(seg + 8)); r[13] = F_RST; }
    else { wr32be(r + 8, rd32be(seg + 4) + dlen); r[13] = F_RST | F_ACK; }
    r[12] = 5 << 4;
    wr16be(r + 16, csum_fold(csum_partial(r, 20, csum_pseudo(net_our_ip(), src, IP_PROTO_TCP, 20))));
    ip_send(src, IP_PROTO_TCP, r, 20);
}

static void set_error(tcp_conn_t *c, int err) {
    if (!c->err) c->err = err;
    c->st = ST_CLOSED;
    c->rto_due = 0;
}

/* Send whatever the window allows: new data, then the FIN. */
static void output(tcp_conn_t *c) {
    if (c->st == ST_SYN_SENT || c->st == ST_CLOSED || c->st == ST_TIME_WAIT) return;
    uint32_t wnd = c->snd_wnd;
    if (wnd > SCAP) wnd = SCAP;
    for (;;) {
        uint32_t off = c->snd_nxt - c->snd_una;           /* bytes already in flight */
        if (c->fin_sent) break;
        if (off < c->slen) {
            if (off >= wnd) {
                /* zero window: probe with one byte when the timer fires */
                if (!c->rto_due) c->rto_due = net_now() + c->rto;
                break;
            }
            uint32_t n = MIN(c->slen - off, MIN((uint32_t)c->mss, wnd - off));
            send_seg(c, c->snd_nxt, F_ACK | F_PSH, c->sbuf + off, n);
            c->snd_nxt += n;
        } else if (c->fin_queued && (c->st == ST_ESTABLISHED || c->st == ST_CLOSE_WAIT)) {
            send_seg(c, c->snd_nxt, F_ACK | F_FIN, NULL, 0);
            c->snd_nxt++;
            c->fin_sent = true;
            c->st = (c->st == ST_ESTABLISHED) ? ST_FIN_WAIT_1 : ST_LAST_ACK;
        } else break;
        if (!c->rto_due) c->rto_due = net_now() + c->rto;
    }
}

static void conn_free(tcp_conn_t *c) {
    for (tcp_conn_t **pp = &conns; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; break; }
    kfree(c->sbuf);
    kfree(c->rbuf);
    kfree(c);
}

tcp_conn_t *tcp_open(uint32_t ip, uint16_t port) {
    if (!net_is_up()) return NULL;
    tcp_conn_t *c = kcalloc(1, sizeof *c);
    if (!c) return NULL;
    c->sbuf = kmalloc(SCAP);
    c->rbuf = kmalloc(RCAP);
    if (!c->sbuf || !c->rbuf) { kfree(c->sbuf); kfree(c->rbuf); kfree(c); return NULL; }
    c->rip = ip;
    c->rport = port;
    for (;;) {
        c->lport = (uint16_t)(49152 + random_u32() % 16384);
        bool used = false;
        for (tcp_conn_t *o = conns; o; o = o->next) if (o->lport == c->lport) used = true;
        if (!used) break;
    }
    c->iss = random_u32();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    c->mss = 536;
    c->rto = RTO_MIN;
    c->st = ST_SYN_SENT;
    c->next = conns;
    conns = c;
    send_seg(c, c->iss, F_SYN, NULL, 0);
    c->mss = 1460;                 /* until the peer tells us otherwise */
    c->rto_due = net_now() + c->rto;
    return c;
}

int tcp_state(tcp_conn_t *c) {
    if (c->err) return TCPS_ERROR;
    if (c->st == ST_SYN_SENT) return TCPS_CONNECTING;
    if (c->fin_rcvd || c->st == ST_CLOSED) return TCPS_PEER_CLOSED;
    return TCPS_OPEN;
}

int tcp_error(tcp_conn_t *c) { return c->err; }
uint32_t tcp_readable(tcp_conn_t *c) { return c->rlen; }

static uint32_t copy_out(tcp_conn_t *c, void *buf, uint32_t max, bool consume) {
    uint32_t n = MIN(max, c->rlen);
    uint8_t *o = buf;
    uint32_t first = MIN(n, RCAP - c->rhead);
    memcpy(o, c->rbuf + c->rhead, first);
    if (n > first) memcpy(o + first, c->rbuf, n - first);
    if (consume) {
        c->rhead = (c->rhead + n) % RCAP;
        c->rlen -= n;
        /* window update when a lot of space opened up */
        uint32_t f = rfree(c);
        if (f > 65535) f = 65535;
        if (n && (c->adv_wnd < c->mss || f >= c->adv_wnd + RCAP / 4) && c->st != ST_SYN_SENT && c->st != ST_CLOSED)
            c->ack_now = true;
    }
    return n;
}

uint32_t tcp_read(tcp_conn_t *c, void *buf, uint32_t max) { return copy_out(c, buf, max, true); }
uint32_t tcp_peek(tcp_conn_t *c, void *buf, uint32_t max) { return copy_out(c, buf, max, false); }

uint32_t tcp_writable(tcp_conn_t *c) {
    if (c->err || c->fin_queued || c->st == ST_CLOSED) return 0;
    return SCAP - c->slen;
}

uint32_t tcp_write(tcp_conn_t *c, const void *data, uint32_t len) {
    uint32_t n = MIN(len, tcp_writable(c));
    if (!n) return 0;
    memcpy(c->sbuf + c->slen, data, n);
    c->slen += n;
    output(c);
    return n;
}

void tcp_close(tcp_conn_t *c, bool abort) {
    if (!c) return;
    c->released = true;
    if (abort || c->err || c->st == ST_SYN_SENT || c->st == ST_CLOSED) {
        if (!c->err && c->st != ST_CLOSED && c->st != ST_SYN_SENT) send_seg(c, c->snd_nxt, F_RST | F_ACK, NULL, 0);
        conn_free(c);
        return;
    }
    c->rlen = 0;                   /* nobody will read it */
    c->fin_queued = true;
    c->until = net_now() + 15000;  /* give up on a clean close after 15 s */
    output(c);
}

/* ---- input ------------------------------------------------------------------------------ */
static void process_ack(tcp_conn_t *c, uint32_t ack, uint32_t wnd) {
    if (seq_lt(c->snd_una, ack) && seq_le(ack, c->snd_nxt)) {
        uint32_t acked = ack - c->snd_una;
        uint32_t data = acked;
        bool fin_acked = c->fin_sent && ack == c->snd_nxt;
        if (fin_acked) data--;
        if (data > c->slen) data = c->slen;
        memmove(c->sbuf, c->sbuf + data, c->slen - data);
        c->slen -= data;
        c->snd_una = ack;
        c->retries = 0;
        c->rto = RTO_MIN;
        c->rto_due = (c->snd_una == c->snd_nxt) ? 0 : net_now() + c->rto;
        if (fin_acked) {
            if (c->st == ST_FIN_WAIT_1) c->st = ST_FIN_WAIT_2;
            else if (c->st == ST_CLOSING) { c->st = ST_TIME_WAIT; c->until = net_now() + 2000; }
            else if (c->st == ST_LAST_ACK) { c->st = ST_CLOSED; }
        }
    }
    if (seq_le(c->snd_una, ack)) c->snd_wnd = wnd;
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, uint32_t len) {
    if (len < 20) return;
    uint32_t hl = (uint32_t)(seg[12] >> 4) * 4;
    if (hl < 20 || hl > len) return;
    if (csum_fold(csum_partial(seg, len, csum_pseudo(src, dst, IP_PROTO_TCP, len))) != 0) return;
    uint16_t sport = rd16be(seg), dport = rd16be(seg + 2);
    uint32_t seq = rd32be(seg + 4), ack = rd32be(seg + 8);
    uint8_t flags = seg[13];
    uint32_t wnd = rd16be(seg + 14);
    const uint8_t *data = seg + hl;
    uint32_t dlen = len - hl;

    tcp_conn_t *c = conns;
    while (c && !(c->rip == src && c->rport == sport && c->lport == dport)) c = c->next;
    if (!c || c->st == ST_CLOSED) { send_rst_for(src, seg, len); return; }

    if (c->st == ST_SYN_SENT) {
        if ((flags & F_ACK) && ack != c->iss + 1) {
            if (!(flags & F_RST)) send_rst_for(src, seg, len);
            return;
        }
        if (flags & F_RST) { if (flags & F_ACK) set_error(c, TCP_EREFUSED); return; }
        if (!(flags & F_SYN) || !(flags & F_ACK)) return;
        c->rcv_nxt = seq + 1;
        c->snd_una = ack;
        c->snd_wnd = wnd;
        c->mss = 536;
        for (uint32_t i = 20; i < hl;) {           /* options: MSS */
            uint8_t k = seg[i];
            if (k == 0) break;
            if (k == 1) { i++; continue; }
            if (i + 1 >= hl || seg[i + 1] < 2) break;
            if (k == 2 && seg[i + 1] == 4 && i + 4 <= hl) c->mss = (uint16_t)MIN(1460, MAX(64, rd16be(seg + i + 2)));
            i += seg[i + 1];
        }
        c->st = ST_ESTABLISHED;
        c->retries = 0;
        c->rto = RTO_MIN;
        c->rto_due = 0;
        send_seg(c, c->snd_nxt, F_ACK, NULL, 0);
        output(c);
        return;
    }

    /* acceptability: we only take in-order data; anything else is answered with an ACK */
    if (flags & F_RST) {
        uint32_t w = MAX(rfree(c), 1u);
        if (seq_le(c->rcv_nxt, seq) && seq_lt(seq, c->rcv_nxt + w)) {
            if (c->st == ST_TIME_WAIT || c->st == ST_LAST_ACK || c->released) c->st = ST_CLOSED;
            else set_error(c, TCP_ERESET);
        }
        return;
    }
    if (flags & F_SYN) {                           /* a retransmitted SYN-ACK: our ACK got lost */
        c->ack_now = true;
        return;
    }
    if (!(flags & F_ACK)) return;

    /* trim data we already have */
    if (seq_lt(seq, c->rcv_nxt)) {
        uint32_t dup = c->rcv_nxt - seq;
        if (dup > dlen) {
            if (!(flags & F_FIN) || dup > dlen + 1) { c->ack_now = true; process_ack(c, ack, wnd); return; }
            dup = dlen;
        }
        data += dup;
        dlen -= dup;
        seq += dup;
    }
    if (seq != c->rcv_nxt) {                       /* out of order: drop, re-ACK */
        c->ack_now = true;
        process_ack(c, ack, wnd);
        return;
    }
    process_ack(c, ack, wnd);
    if (c->st == ST_CLOSED) return;

    if (dlen) {
        if (c->fin_rcvd || c->st == ST_TIME_WAIT) {
            dlen = 0;
        } else if (c->released) {
            c->rcv_nxt += dlen;                    /* nobody reads: swallow */
        } else {
            uint32_t n = MIN(dlen, rfree(c));
            uint32_t pos = (c->rhead + c->rlen) % RCAP;
            uint32_t first = MIN(n, RCAP - pos);
            memcpy(c->rbuf + pos, data, first);
            if (n > first) memcpy(c->rbuf, data + first, n - first);
            c->rlen += n;
            c->rcv_nxt += n;
            if (n < dlen) flags &= (uint8_t)~F_FIN; /* did not take it all: the FIN comes again */
        }
        c->ack_now = true;
    }
    if ((flags & F_FIN) && !c->fin_rcvd) {
        c->fin_rcvd = true;
        c->rcv_nxt++;
        c->ack_now = true;
        switch (c->st) {
        case ST_ESTABLISHED: c->st = ST_CLOSE_WAIT; break;
        case ST_FIN_WAIT_1:  c->st = ST_CLOSING; break;
        case ST_FIN_WAIT_2:  c->st = ST_TIME_WAIT; c->until = net_now() + 2000; break;
        }
    }
    output(c);
}

void tcp_flush(void) {
    for (tcp_conn_t *c = conns; c; c = c->next)
        if (c->ack_now && c->st != ST_SYN_SENT && c->st != ST_CLOSED) send_seg(c, c->snd_nxt, F_ACK, NULL, 0);
}

void tcp_tick(void) {
    uint32_t now = net_now();
    for (tcp_conn_t *c = conns, *next; c; c = next) {
        next = c->next;
        if (c->released && (c->st == ST_CLOSED || ((c->st == ST_TIME_WAIT || c->until) && (int32_t)(now - c->until) >= 0))) {
            if (c->st != ST_CLOSED && c->st != ST_TIME_WAIT) send_seg(c, c->snd_nxt, F_RST | F_ACK, NULL, 0);
            conn_free(c);
            continue;
        }
        if (c->st == ST_CLOSED || !c->rto_due || (int32_t)(now - c->rto_due) < 0) continue;
        /* retransmission timeout */
        c->retries++;
        if (c->st == ST_SYN_SENT) {
            if (c->retries > SYN_RETRIES) { set_error(c, TCP_ETIMEOUT); continue; }
            c->rto = MIN(c->rto * 2, (uint32_t)RTO_MAX);
            send_seg(c, c->iss, F_SYN, NULL, 0);
            c->rto_due = now + c->rto;
            continue;
        }
        if (c->retries > MAX_RETRIES) { set_error(c, TCP_ETIMEOUT); continue; }
        c->rto = MIN(c->rto * 2, (uint32_t)RTO_MAX);
        c->rto_due = 0;
        if (c->snd_una == c->snd_nxt && c->slen && c->snd_wnd == 0) {
            send_seg(c, c->snd_una, F_ACK, c->sbuf, 1);       /* zero-window probe */
            c->rto_due = now + c->rto;
            continue;
        }
        /* go back: resend from the oldest unacknowledged byte */
        if (c->fin_sent && c->snd_una != c->snd_nxt) {
            c->fin_sent = false;
            if (c->st == ST_FIN_WAIT_1) c->st = ST_ESTABLISHED;
            else if (c->st == ST_LAST_ACK) c->st = ST_CLOSE_WAIT;
            else if (c->st == ST_CLOSING) c->st = ST_CLOSE_WAIT;
        }
        c->snd_nxt = c->snd_una;
        output(c);
        if (!c->rto_due && c->snd_una != c->snd_nxt) c->rto_due = now + c->rto;
    }
}
