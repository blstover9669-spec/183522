/*
 * nova/net.h - networking for apps: network status, an asynchronous HTTP / HTTPS
 * client (HTTP/1.1 over TCP, TLS 1.2 and 1.3), a cookie jar, URL helpers and a
 * DEFLATE / zlib / gzip decompressor.
 *
 *   #include <nova/net.h>          (not included by nova.h)
 *
 * The network card (Intel PRO/1000 "e1000", VirtualBox's default adapter and
 * QEMU's -nic user,model=e1000) is set up at boot and configured by DHCP.
 *
 * EVERYTHING IS ASYNCHRONOUS. http_fetch() returns at once; the network work
 * runs in small slices from the main loop and your callback is called later,
 * from the main loop, like a timer callback. Never wait in a loop for a request
 * to finish - that freezes the whole OS (and the request, which needs the main
 * loop to make progress).
 *
 * See docs/API.md, section "Networking".
 */
#ifndef NOVA_NET_H
#define NOVA_NET_H

#include <nova/types.h>

/* ==== Network status ======================================================== */

/* true when a network card was found, its link is up and DHCP gave us an address
   (a DNS server and default gateway). http_fetch fails with HTTP_ERR_NO_NETWORK
   while this is false. */
bool        net_is_up(void);

/* One short line for status bars / Settings, e.g. "No network adapter",
   "Network cable unplugged", "Connecting...", "Connected (10.0.2.15)". */
const char *net_status_text(void);

typedef struct net_info {
    bool     adapter;            /* a supported network card was found            */
    bool     link_up;            /* cable connected                               */
    bool     configured;         /* DHCP finished: ip/netmask/gateway/dns are set */
    char     adapter_name[48];   /* e.g. "Intel PRO/1000 MT Desktop (82540EM)"    */
    uint8_t  mac[6];
    uint32_t ip, netmask, gateway, dns;   /* IPv4, host byte order: 10.0.2.15 = 0x0A00020F */
    uint32_t lease_seconds;      /* DHCP lease time                               */
    uint32_t rx_packets, tx_packets;
    uint32_t rx_bytes, tx_bytes;
} net_info_t;

void        net_get_info(net_info_t *out);
/* Bumps whenever the network status changes (link, DHCP) - poll it to refresh a UI. */
uint32_t    net_generation(void);
/* "10.0.2.15" (out needs 16 bytes). Returns out. */
char       *net_ip_to_str(uint32_t ip, char *out);

/* ==== HTTP / HTTPS client ===================================================
 *
 *   static void done(http_request_t *r, void *ctx) {
 *       if (http_error(r) == HTTP_OK)
 *           klog("%d %s, %u bytes", http_status(r), http_url(r), http_body_len(r));
 *       else
 *           klog("failed: %s", http_error_text(r));
 *       http_free(r);                       // allowed inside the callback
 *   }
 *   http_fetch("GET", "https://example.com/", NULL, 0, NULL, done, NULL);
 *
 * - "http://" and "https://" URLs (default ports 80 / 443, IPv4 only). A URL
 *   without a scheme is treated as http://.
 * - Redirects (301, 302, 303, 307, 308) are followed (up to 10); a 303, or a
 *   301/302 after a POST, turns into a GET. http_url() is the final URL.
 * - Chunked transfer encoding is decoded, and gzip / deflate bodies are
 *   decompressed ("Accept-Encoding: gzip, deflate" is sent): http_body() is
 *   always the plain content.
 * - Cookies are sent and stored automatically (see Cookies below).
 * - HTTPS: TLS 1.3 or TLS 1.2 (AES-128/256-GCM, ChaCha20-Poly1305; X25519 or
 *   P-256 ECDHE). The server's certificate chain is checked against the
 *   built-in root certificates, its dates against the clock and its names
 *   against the host (RSA PKCS#1 / PSS and ECDSA P-256 / P-384 signatures).
 *   A certificate that fails the check makes the request fail with
 *   HTTP_ERR_CERT (http_cert_error() says why) unless HTTP_F_ALLOW_BAD_CERT is
 *   set, which lets it continue with http_cert_ok() == false.
 * - The callback is called exactly once (never from inside http_fetch itself),
 *   unless the request is cancelled or freed first. Up to 8 requests run at the
 *   same time; more are queued.
 */
typedef struct http_request http_request_t;
typedef void (*http_cb)(http_request_t *req, void *ctx);

/* http_error() values */
enum {
    HTTP_PENDING        =   1,   /* still running                                         */
    HTTP_OK             =   0,   /* a response arrived - ANY status code, see http_status */
    HTTP_ERR_NO_NETWORK =  -1,   /* no network card, cable unplugged or no address (DHCP) */
    HTTP_ERR_DNS        =  -2,   /* the host name could not be found ("site not found")   */
    HTTP_ERR_CONNECT    =  -3,   /* connection refused, reset or host unreachable         */
    HTTP_ERR_TIMEOUT    =  -4,   /* no progress for timeout_ms                            */
    HTTP_ERR_TLS        =  -5,   /* secure connection failed (handshake / protocol error) */
    HTTP_ERR_CERT       =  -6,   /* the certificate is not valid for this site            */
    HTTP_ERR_PROTOCOL   =  -7,   /* malformed HTTP response                               */
    HTTP_ERR_REDIRECTS  =  -8,   /* too many redirects                                    */
    HTTP_ERR_BAD_URL    =  -9,   /* the URL cannot be parsed or its scheme is unsupported */
    HTTP_ERR_TOO_LARGE  = -10,   /* body bigger than max_body                             */
    HTTP_ERR_NO_MEMORY  = -11,
    HTTP_ERR_CANCELLED  = -12,   /* http_cancel()                                         */
};

/* http_options_t.flags */
#define HTTP_F_ALLOW_BAD_CERT  0x01  /* continue when the certificate check fails (the user
                                        chose "continue anyway"); http_cert_ok() stays false */
#define HTTP_F_NO_REDIRECT     0x02  /* return 3xx responses instead of following them      */
#define HTTP_F_NO_COOKIES      0x04  /* neither send nor store cookies                      */
#define HTTP_F_NO_DECODE       0x08  /* keep a gzip / deflate body compressed               */

typedef struct http_options {
    const char *method;          /* NULL: "GET", or "POST" when body_len > 0          */
    const void *body;            /* request body (copied), or NULL                    */
    uint32_t    body_len;
    const char *content_type;    /* of the body, e.g. "application/x-www-form-urlencoded" */
    const char *headers;         /* extra request headers "Name: value\r\n"..., or NULL */
    uint32_t    flags;           /* HTTP_F_*                                          */
    uint32_t    max_body;        /* 0 = 64 MB                                         */
    uint32_t    timeout_ms;      /* 0 = 30 s without any progress                     */
    http_cb     on_progress;     /* optional: called (main loop) when the response
                                    headers arrive and when more body bytes arrive    */
} http_options_t;

/* Start a request. body may be NULL (body_len 0). Returns NULL only when out of
   memory; every other failure (bad URL, no network ...) is reported through the
   callback. The request is yours: call http_free() when you are done with it. */
http_request_t *http_fetch(const char *method, const char *url,
                           const char *body, uint32_t body_len, const char *content_type,
                           http_cb on_done, void *ctx);
/* The same with all options (opt may be NULL). */
http_request_t *http_fetch_ex(const char *url, const http_options_t *opt,
                              http_cb on_done, void *ctx);

/* Stop a running request: the callback will not be called, http_error() becomes
   HTTP_ERR_CANCELLED. You still http_free() it. No-op on a finished request. */
void http_cancel(http_request_t *req);
/* Free a request (cancels it first if it is still running). Safe inside its own
   callbacks. NULL is ignored. */
void http_free(http_request_t *req);

/* --- results (valid until http_free) --- */
bool        http_done(const http_request_t *r);       /* finished, with a response or an error */
int         http_error(const http_request_t *r);      /* HTTP_PENDING, HTTP_OK or HTTP_ERR_*   */
const char *http_error_text(const http_request_t *r); /* "" when OK; a sentence otherwise      */
const char *http_strerror(int err);                   /* generic text for an HTTP_ERR_* code   */
int         http_status(const http_request_t *r);     /* 200, 404 ...; 0 until headers arrive  */
const char *http_reason(const http_request_t *r);     /* "OK", "Not Found" ... ("" if none)    */
const char *http_url(const http_request_t *r);        /* final URL after redirects             */
int         http_redirect_count(const http_request_t *r);
/* A response header (name case-insensitive; the first one if repeated), or NULL. */
const char *http_header(const http_request_t *r, const char *name);
/* All response headers: index 0..count-1. */
int         http_header_count(const http_request_t *r);
bool        http_header_at(const http_request_t *r, int index, const char **name, const char **value);
/* The decoded body (de-chunked, decompressed), with a NUL after the last byte so
   text can be used as a C string. NULL / 0 before any body arrived. */
const uint8_t *http_body(const http_request_t *r);
uint32_t    http_body_len(const http_request_t *r);
/* Take ownership of the body buffer (free() it yourself); the request then has
   no body. Avoids a copy for big downloads. */
uint8_t    *http_take_body(http_request_t *r, uint32_t *len);
/* Body bytes received so far and the expected total (Content-Length; 0 = unknown).
   These count bytes on the wire (compressed size for gzip bodies). */
void        http_progress(const http_request_t *r, uint32_t *received, uint32_t *total);

/* --- security (padlock) --- */
bool        http_is_https(const http_request_t *r);   /* the final URL was https://            */
bool        http_cert_ok(const http_request_t *r);    /* HTTPS and the certificate verified    */
const char *http_cert_error(const http_request_t *r); /* "" or why the certificate was refused,
                                                         e.g. "The certificate has expired"  */
const char *http_cert_subject(const http_request_t *r); /* site name in the certificate ("" if none) */
const char *http_cert_issuer(const http_request_t *r);  /* who issued it, e.g. "Let's Encrypt"  */
const char *http_tls_info(const http_request_t *r);     /* e.g. "TLS 1.3, AES_128_GCM_SHA256, X25519";
                                                           "" for plain HTTP                 */

/* ==== Cookies ================================================================
 * Kept per site (domain / path rules of RFC 6265), sent back automatically with
 * every http_fetch to that site, and saved across restarts in /home/user/.cookies
 * (cookies without an expiry date live until shut down). */
void cookies_clear(void);                 /* delete every cookie (memory and file) */
int  cookies_count(void);

/* ==== URL helpers ============================================================ */
typedef struct url {
    char     scheme[8];          /* "http" / "https" (lower case)               */
    char     host[256];          /* lower case, no port                         */
    uint16_t port;               /* explicit or the scheme's default            */
    char     path[2048];         /* path + "?query", at least "/"; no #fragment */
} url_t;
/* Parse an absolute URL ("example.com/x" is taken as http://example.com/x).
   false if it cannot be parsed or is not http / https. */
bool url_parse(const char *s, url_t *out);
/* Resolve a link (absolute, "//host/x", "/x", "x", "?q", "#f", "../x") against
   the URL of the page it is on (RFC 3986). false if out is too small. */
bool url_resolve(const char *base, const char *ref, char *out, size_t size);

/* ==== Decompression (gzip, zlib, raw DEFLATE) ================================ */
#define INFLATE_RAW   0          /* raw DEFLATE (RFC 1951)                       */
#define INFLATE_ZLIB  1          /* zlib stream (RFC 1950) - e.g. PNG image data */
#define INFLATE_GZIP  2          /* gzip file / Content-Encoding (RFC 1952)      */
#define INFLATE_AUTO  3          /* gzip or zlib by the header, else raw          */
/* Decompress into a malloc'd buffer (*out, *out_len; a NUL is appended, not
   counted). max_out caps the output (0 = 64 MB). Returns 0, or -1 for corrupt or
   truncated data, -2 when the output would exceed max_out, -3 out of memory.
   Runs synchronously: keep inputs to a few MB inside one callback. */
int  inflate_buffer(const void *in, uint32_t in_len, int format,
                    uint8_t **out, uint32_t *out_len, uint32_t max_out);

#endif
