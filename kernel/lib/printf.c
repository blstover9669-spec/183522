/* printf.c - vsnprintf and friends.
 *
 * Supports: %d %i %u %x %X %o %p %s %c %f %F %e %g %%, flags '-', '0', '+', ' ', '#',
 * width and precision (including '*'), length modifiers hh h l ll z t j.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <nova/sys.h>

typedef struct {
    char *buf;
    size_t size;
    size_t pos;
} outbuf_t;

static void put(outbuf_t *o, char c) {
    if (o->pos + 1 < o->size) o->buf[o->pos] = c;
    o->pos++;
}

static void put_pad(outbuf_t *o, char c, int n) {
    while (n-- > 0) put(o, c);
}

/* Emit a converted number/string body with sign/prefix, honouring width & flags. */
static void emit(outbuf_t *o, const char *prefix, const char *body, int body_len,
                 int width, int left, int zero, int prec_zeros) {
    int plen = (int)strlen(prefix);
    int total = plen + prec_zeros + body_len;
    int pad = width > total ? width - total : 0;
    if (!left && !zero) put_pad(o, ' ', pad);
    for (int i = 0; i < plen; i++) put(o, prefix[i]);
    if (!left && zero) put_pad(o, '0', pad);
    put_pad(o, '0', prec_zeros);
    for (int i = 0; i < body_len; i++) put(o, body[i]);
    if (left) put_pad(o, ' ', pad);
}

static int utoa_buf(unsigned long long v, int base, int upper, char *out) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) {
        tmp[n++] = digits[v % (unsigned)base];
        v /= (unsigned)base;
    }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

/* Format a double in fixed notation into out (no sign). Returns length. */
static int ftoa_fixed(double v, int prec, char *out, int outsz) {
    if (prec > 15) prec = 15;
    /* rounding */
    double r = 0.5;
    for (int i = 0; i < prec; i++) r /= 10.0;
    v += r;
    int n = 0;
    if (v >= 1e18) {
        /* too large for integer conversion: scale down */
        int e = 0;
        while (v >= 10.0) { v /= 10.0; e++; }
        int dig = (int)v;
        out[n++] = (char)('0' + dig);
        v -= dig;
        for (int i = 0; i < e && n < outsz - 2; i++) {
            v *= 10.0;
            dig = (int)v;
            if (dig > 9) dig = 9;
            out[n++] = (char)('0' + dig);
            v -= dig;
        }
    } else {
        unsigned long long ip = (unsigned long long)v;
        v -= (double)ip;
        n = utoa_buf(ip, 10, 0, out);
    }
    if (prec > 0 && n < outsz - 1) {
        out[n++] = '.';
        for (int i = 0; i < prec && n < outsz - 1; i++) {
            v *= 10.0;
            int dig = (int)v;
            if (dig > 9) dig = 9;
            if (dig < 0) dig = 0;
            out[n++] = (char)('0' + dig);
            v -= dig;
        }
    }
    return n;
}

static int ftoa_exp(double v, int prec, int upper, char *out, int outsz) {
    int e = 0;
    if (v != 0.0) {
        while (v >= 10.0) { v /= 10.0; e++; }
        while (v < 1.0) { v *= 10.0; e--; }
    }
    /* rounding may push mantissa to 10.0 */
    double r = 0.5;
    for (int i = 0; i < prec; i++) r /= 10.0;
    if (v + r >= 10.0) { v /= 10.0; e++; }
    int n = ftoa_fixed(v, prec, out, outsz - 6);
    out[n++] = upper ? 'E' : 'e';
    out[n++] = e < 0 ? '-' : '+';
    if (e < 0) e = -e;
    if (e >= 100) out[n++] = (char)('0' + e / 100);
    out[n++] = (char)('0' + (e / 10) % 10);
    out[n++] = (char)('0' + e % 10);
    return n;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    outbuf_t o = { buf, size, 0 };
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { put(&o, *p); continue; }
        p++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else if (*p == '+') plus = 1;
            else if (*p == ' ') space = 1;
            else if (*p == '#') alt = 1;
            else break;
        }
        int width = 0;
        if (*p == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } p++; }
        else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') { prec = va_arg(ap, int); p++; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }
        int lng = 0; /* 0 int, 1 long, 2 long long, -1 short, -2 char */
        for (;; p++) {
            if (*p == 'l') lng++;
            else if (*p == 'h') lng--;
            else if (*p == 'z' || *p == 't' || *p == 'j') lng = (*p == 'j') ? 2 : 1;
            else break;
        }
        if (left) zero = 0;
        char body[64];
        char prefix[4] = "";
        int blen = 0;
        switch (*p) {
        case 'd': case 'i': {
            long long v;
            if (lng >= 2) v = va_arg(ap, long long);
            else if (lng == 1) v = va_arg(ap, long);
            else v = va_arg(ap, int);
            if (lng == -1) v = (short)v;
            if (lng <= -2) v = (signed char)v;
            unsigned long long u = v < 0 ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
            if (v < 0) strcpy(prefix, "-");
            else if (plus) strcpy(prefix, "+");
            else if (space) strcpy(prefix, " ");
            blen = (prec == 0 && u == 0) ? 0 : utoa_buf(u, 10, 0, body);
            int pz = prec > blen ? prec - blen : 0;
            emit(&o, prefix, body, blen, width, left, prec >= 0 ? 0 : zero, pz);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'b': {
            unsigned long long u;
            if (lng >= 2) u = va_arg(ap, unsigned long long);
            else if (lng == 1) u = va_arg(ap, unsigned long);
            else u = va_arg(ap, unsigned int);
            if (lng == -1) u = (unsigned short)u;
            if (lng <= -2) u = (unsigned char)u;
            int base = (*p == 'u') ? 10 : (*p == 'o') ? 8 : (*p == 'b') ? 2 : 16;
            if (alt && u && base == 16) strcpy(prefix, *p == 'X' ? "0X" : "0x");
            if (alt && base == 8) strcpy(prefix, "0");
            blen = (prec == 0 && u == 0) ? 0 : utoa_buf(u, base, *p == 'X', body);
            int pz = prec > blen ? prec - blen : 0;
            emit(&o, prefix, body, blen, width, left, prec >= 0 ? 0 : zero, pz);
            break;
        }
        case 'p': {
            uintptr_t u = (uintptr_t)va_arg(ap, void *);
            strcpy(prefix, "0x");
            blen = utoa_buf(u, 16, 0, body);
            int pz = 8 - blen;
            emit(&o, prefix, body, blen, width, left, 0, pz > 0 ? pz : 0);
            break;
        }
        case 'c': {
            body[0] = (char)va_arg(ap, int);
            emit(&o, "", body, 1, width, left, 0, 0);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = prec >= 0 ? (int)strnlen(s, (size_t)prec) : (int)strlen(s);
            int pad = width > len ? width - len : 0;
            if (!left) put_pad(&o, ' ', pad);
            for (int i = 0; i < len; i++) put(&o, s[i]);
            if (left) put_pad(&o, ' ', pad);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double v = va_arg(ap, double);
            if (prec < 0) prec = 6;
            if (v != v) { emit(&o, "", "nan", 3, width, left, 0, 0); break; }
            int neg = v < 0 || (v == 0 && (1.0 / v) < 0);
            if (neg) v = -v;
            if (neg) strcpy(prefix, "-");
            else if (plus) strcpy(prefix, "+");
            else if (space) strcpy(prefix, " ");
            if (v > 1.7976931348623157e308) { emit(&o, prefix, "inf", 3, width, left, 0, 0); break; }
            char c = *p;
            if (c == 'g' || c == 'G') {
                if (prec == 0) prec = 1;
                int e = 0;
                double t = v;
                if (t != 0.0) {
                    while (t >= 10.0) { t /= 10.0; e++; }
                    while (t < 1.0) { t *= 10.0; e--; }
                }
                if (e < -4 || e >= prec) {
                    blen = ftoa_exp(v, prec - 1, c == 'G', body, sizeof body);
                } else {
                    blen = ftoa_fixed(v, prec - 1 - e, body, sizeof body);
                }
                /* strip trailing zeros unless '#' */
                if (!alt && memchr(body, '.', (size_t)blen)) {
                    char *epos = memchr(body, c == 'G' ? 'E' : 'e', (size_t)blen);
                    int mant_end = epos ? (int)(epos - body) : blen;
                    int k = mant_end;
                    while (k > 0 && body[k - 1] == '0') k--;
                    if (k > 0 && body[k - 1] == '.') k--;
                    if (epos) memmove(body + k, epos, (size_t)(blen - mant_end));
                    blen = k + (blen - mant_end);
                }
            } else if (c == 'e' || c == 'E') {
                blen = ftoa_exp(v, prec, c == 'E', body, sizeof body);
            } else {
                blen = ftoa_fixed(v, prec, body, sizeof body);
            }
            emit(&o, prefix, body, blen, width, left, zero, 0);
            break;
        }
        case 'n': {
            int *ip = va_arg(ap, int *);
            if (ip) *ip = (int)o.pos;
            break;
        }
        case '%':
            put(&o, '%');
            break;
        case 0:
            p--;
            break;
        default:
            put(&o, '%');
            put(&o, *p);
            break;
        }
    }
    if (o.size) o.buf[o.pos < o.size ? o.pos : o.size - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, (size_t)-1 >> 1, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap) {
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    serial_write(tmp);
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s) {
    serial_write(s);
    serial_write("\n");
    return 1;
}

int putchar(int c) {
    char s[2] = { (char)c, 0 };
    serial_write(s);
    return c;
}
