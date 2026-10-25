/*
 * net_internal.h - the network stack's internal interfaces (not part of the app API;
 * apps use <nova/net.h>).
 *
 * Layout: kernel/drivers/e1000.c (network card) -> netcore.c (Ethernet, ARP, IPv4,
 * ICMP, UDP, the poll timer) -> dhcp.c, dns.c, tcp.c -> tls.c / x509.c -> http.c,
 * cookies.c. Everything runs from one main-loop timer (net_tick, every 1 ms):
 * there are no threads and nothing waits; every layer is a state machine that
 * is polled ("stepped") from the tick.
 */
#ifndef NET_INTERNAL_H
#define NET_INTERNAL_H

#include "kernel.h"
#include <nova/net.h>

/* ---- byte order ---------------------------------------------------------------- */
static inline uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint16_t rd16be(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void wr16be(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* ---- network device (a driver registers one) --------------------------------------- */
typedef void (*net_rx_fn)(const uint8_t *frame, uint32_t len);
typedef struct netdev {
    char     name[48];
    uint8_t  mac[6];
    bool   (*link_up)(void);
    /* queue one Ethernet frame (without FCS, 14..1514 bytes); false if the ring is full */
    bool   (*send)(const void *frame, uint32_t len);
    /* hand up to `budget` received frames to rx(); returns how many */
    int    (*poll)(net_rx_fn rx, int budget);
} netdev_t;

void net_register_device(netdev_t *dev);

/* ---- core (netcore.c) ------------------------------------------------------------- */
#define ETH_MTU      1500
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

typedef struct net_config {
    bool     configured;
    uint32_t ip, netmask, gateway, dns, dns2;   /* host byte order */
    uint32_t lease_s;
} net_config_t;

extern net_config_t g_netcfg;
netdev_t *net_device(void);
bool      net_link_up(void);
void      net_set_config(const net_config_t *cfg);     /* from DHCP; NULL = unconfigure */
void      net_changed(void);                           /* bump net_generation */
uint32_t  net_now(void);                               /* milliseconds (g_ticks) */
uint32_t  csum_partial(const void *data, uint32_t len, uint32_t sum);   /* running 16-bit sum */
uint16_t  csum_fold(uint32_t sum);                                    /* final ~sum (network order bytes) */
uint32_t  csum_pseudo(uint32_t src, uint32_t dst, uint8_t proto, uint32_t len);
uint32_t  net_our_ip(void);
/* Send an IPv4 packet (payload after the IP header). dst in host byte order.
   src = our address (0.0.0.0 while unconfigured). Returns false if it could not be
   queued (no route, ARP pending queue full, ring full). */
bool      ip_send(uint32_t dst, uint8_t proto, const void *payload, uint32_t len);

/* UDP */
typedef void (*udp_rx_fn)(void *ctx, uint32_t src_ip, uint16_t src_port, const uint8_t *data, uint32_t len);
bool      udp_bind(uint16_t port, udp_rx_fn fn, void *ctx);
void      udp_unbind(uint16_t port);
bool      udp_send(uint32_t dst, uint16_t src_port, uint16_t dst_port, const void *data, uint32_t len);
uint16_t  net_ephemeral_port(void);

/* ICMP echo (ping) */
typedef void (*ping_fn)(void *ctx, uint32_t from, uint16_t seq, uint32_t rtt_ms, int ttl);
bool      icmp_ping(uint32_t dst, uint16_t id, uint16_t seq, ping_fn fn, void *ctx);
void      icmp_cancel(void *ctx);     /* forget outstanding pings with this ctx */

/* Hooks called by netcore */
void dhcp_start(void);          /* link came up */
void dhcp_stop(void);           /* link went down */
void dhcp_tick(void);
void dns_tick(void);
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, uint32_t len);
void tcp_tick(void);
void http_tick(void);
void tcp_flush(void);           /* send pending ACKs at the end of a tick */

/* ---- DNS (dns.c) ------------------------------------------------------------------ */
#define DNS_OK         0
#define DNS_PENDING    1
#define DNS_ENOTFOUND -1        /* NXDOMAIN or no A record */
#define DNS_EFAIL     -2        /* no answer / server failure / no network */
typedef struct dns_query dns_query_t;
/* Start resolving a host name (or a dotted IPv4 address). Poll with dns_result();
   free with dns_free() (also cancels). */
dns_query_t *dns_resolve(const char *host);
int          dns_result(dns_query_t *q, uint32_t *ip);   /* DNS_OK / DNS_PENDING / error */
void         dns_free(dns_query_t *q);
bool         net_parse_ipv4(const char *s, uint32_t *ip);

/* ---- TCP (tcp.c) ------------------------------------------------------------------ */
typedef struct tcp_conn tcp_conn_t;
enum {
    TCPS_CONNECTING = 0,        /* SYN sent                                  */
    TCPS_OPEN,                  /* established                               */
    TCPS_PEER_CLOSED,           /* the peer sent FIN (read what is buffered) */
    TCPS_ERROR,                 /* refused / reset / timed out: tcp_error()  */
};
#define TCP_EREFUSED  -1
#define TCP_ERESET    -2
#define TCP_ETIMEOUT  -3
#define TCP_ENOROUTE  -4

tcp_conn_t *tcp_open(uint32_t ip, uint16_t port);   /* NULL: no memory / no network */
int         tcp_state(tcp_conn_t *c);
int         tcp_error(tcp_conn_t *c);
uint32_t    tcp_readable(tcp_conn_t *c);
uint32_t    tcp_read(tcp_conn_t *c, void *buf, uint32_t max);
uint32_t    tcp_peek(tcp_conn_t *c, void *buf, uint32_t max);
uint32_t    tcp_writable(tcp_conn_t *c);
uint32_t    tcp_write(tcp_conn_t *c, const void *data, uint32_t len);   /* bytes accepted */
/* Give the connection back: sends FIN (or RST when abort) and forgets the handle;
   the stack finishes the close on its own. */
void        tcp_close(tcp_conn_t *c, bool abort);
int         tcp_count(void);

/* ---- TLS (tls.c) ------------------------------------------------------------------ */
typedef struct tls_conn tls_conn_t;
enum { TLSS_HANDSHAKE = 0, TLSS_OPEN, TLSS_PEER_CLOSED, TLSS_ERROR };
#define TLS_ECERT   -1          /* certificate rejected (tls_cert_error says why)        */
#define TLS_EFAIL   -2          /* handshake / protocol / decryption failure             */
#define TLS_ETCP    -3          /* the TCP connection failed (tcp_error of the socket)   */
#define TLS_F_ALLOW_BAD_CERT 1
/* Start a TLS client on an open (or opening) TCP connection; takes ownership of tcp. */
tls_conn_t *tls_open(tcp_conn_t *tcp, const char *host, uint32_t flags);
void        tls_step(tls_conn_t *t);         /* drive the handshake / records (http_tick) */
int         tls_state(tls_conn_t *t);
int         tls_error(tls_conn_t *t);
int         tls_tcp_error(tls_conn_t *t);
const char *tls_error_text(tls_conn_t *t);
uint32_t    tls_readable(tls_conn_t *t);
uint32_t    tls_read(tls_conn_t *t, void *buf, uint32_t max);
uint32_t    tls_writable(tls_conn_t *t);
uint32_t    tls_write(tls_conn_t *t, const void *data, uint32_t len);
bool        tls_cert_ok(tls_conn_t *t);
const char *tls_cert_error(tls_conn_t *t);
const char *tls_cert_subject(tls_conn_t *t);
const char *tls_cert_issuer(tls_conn_t *t);
const char *tls_info(tls_conn_t *t);
void        tls_close(tls_conn_t *t, bool abort);

/* ---- HTTP (http.c) / cookies (cookies.c) -------------------------------------------- */
/* "Cookie: a=b; c=d" value for a request (malloc'd, NULL if none) */
char *cookies_header_for(const url_t *u);
/* store one Set-Cookie header value received for URL u */
void  cookies_store(const url_t *u, const char *set_cookie);
void  cookies_init(void);

/* ---- logging ------------------------------------------------------------------------ */
#define NLOG(...)  log_info("net", __VA_ARGS__)
#define NWARN(...) log_warn("net", __VA_ARGS__)

#endif
