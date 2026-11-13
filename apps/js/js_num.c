/*
 * js_num.c - NovaJS number <-> string conversion, all exact:
 *
 *   js_dtoa        Number::toString: the shortest digits that read back as the
 *                  same double (free-format digit generation with big integers,
 *                  after Steele & White / Burger & Dybvig)
 *   js_dtoa_fixed / _prec / _exp   toFixed / toPrecision / toExponential from the
 *                  exact decimal expansion of the double (ties round up, as the
 *                  spec says)
 *   js_dtoa_radix  toString(radix): digits until the value is identified
 *   js_strtod16    decimal string -> correctly rounded double (fast exact path
 *                  for short inputs, big-integer division otherwise)
 */
#include "jsi.h"

/* ---- a small big-integer type (little-endian 32-bit words) ---------------------------------- */
#define BN_MAX 168
typedef struct { uint32_t w[BN_MAX]; int n; } BN;

static void bn_set(BN *a, uint64_t v) {
    a->n = 0;
    while (v) { a->w[a->n++] = (uint32_t)v; v >>= 32; }
}
static bool bn_zero(const BN *a) { return a->n == 0; }
static void bn_trim(BN *a) { while (a->n && !a->w[a->n - 1]) a->n--; }

static void bn_mul_small(BN *a, uint32_t m) {
    uint64_t c = 0;
    for (int i = 0; i < a->n; i++) { c += (uint64_t)a->w[i] * m; a->w[i] = (uint32_t)c; c >>= 32; }
    if (c && a->n < BN_MAX) a->w[a->n++] = (uint32_t)c;
}

static void bn_add_small(BN *a, uint32_t v) {
    uint64_t c = v;
    for (int i = 0; i < a->n && c; i++) { c += a->w[i]; a->w[i] = (uint32_t)c; c >>= 32; }
    if (c && a->n < BN_MAX) a->w[a->n++] = (uint32_t)c;
}

static void bn_mul_pow10(BN *a, int k) {
    while (k >= 9) { bn_mul_small(a, 1000000000u); k -= 9; }
    static const uint32_t p[9] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000 };
    if (k > 0) bn_mul_small(a, p[k]);
}

static void bn_mul_pow5(BN *a, int k) {
    while (k >= 13) { bn_mul_small(a, 1220703125u); k -= 13; }   /* 5^13 */
    uint32_t m = 1;
    while (k-- > 0) m *= 5;
    if (m > 1) bn_mul_small(a, m);
}

static void bn_shl(BN *a, int bits) {
    if (bn_zero(a) || bits <= 0) return;
    int ws = bits / 32, bs = bits % 32;
    if (a->n + ws + 1 > BN_MAX) { ws = BN_MAX - a->n - 1; if (ws < 0) return; }
    if (bs) {
        uint32_t carry = 0;
        for (int i = 0; i < a->n; i++) { uint32_t v = a->w[i]; a->w[i] = (v << bs) | carry; carry = v >> (32 - bs); }
        if (carry) a->w[a->n++] = carry;
    }
    if (ws) {
        memmove(a->w + ws, a->w, sizeof(uint32_t) * (size_t)a->n);
        memset(a->w, 0, sizeof(uint32_t) * (size_t)ws);
        a->n += ws;
    }
}

static int bn_cmp(const BN *a, const BN *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

static void bn_add(BN *a, const BN *b) {
    uint64_t c = 0;
    int n = MAX(a->n, b->n);
    for (int i = 0; i < n; i++) {
        c += (uint64_t)(i < a->n ? a->w[i] : 0) + (i < b->n ? b->w[i] : 0);
        a->w[i] = (uint32_t)c;
        c >>= 32;
    }
    a->n = n;
    if (c && a->n < BN_MAX) a->w[a->n++] = (uint32_t)c;
}

static void bn_sub(BN *a, const BN *b) {   /* a >= b */
    int64_t c = 0;
    for (int i = 0; i < a->n; i++) {
        c += (int64_t)a->w[i] - (i < b->n ? b->w[i] : 0);
        a->w[i] = (uint32_t)c;
        c >>= 32;
    }
    bn_trim(a);
}

/* a + b compared with c */
static int bn_cmp_sum(const BN *a, const BN *b, const BN *c) {
    BN t = *a;
    bn_add(&t, b);
    return bn_cmp(&t, c);
}

static int bn_bitlen(const BN *a) {
    if (!a->n) return 0;
    return (a->n - 1) * 32 + (32 - __builtin_clz(a->w[a->n - 1]));
}

static bool bn_bit(const BN *a, int i) {
    int w = i / 32;
    return w < a->n && ((a->w[w] >> (i % 32)) & 1);
}

static uint32_t bn_divmod_small(BN *a, uint32_t d) {
    uint64_t r = 0;
    for (int i = a->n - 1; i >= 0; i--) {
        r = (r << 32) | a->w[i];
        a->w[i] = (uint32_t)(r / d);
        r %= d;
    }
    bn_trim(a);
    return (uint32_t)r;
}

/* decimal digits of a (destroyed) -> buf (no leading zeros), returns count */
static int bn_to_dec(BN *a, char *buf, int size) {
    char tmp[1400];
    int n = 0;
    while (!bn_zero(a) && n + 9 < (int)sizeof tmp) {
        uint32_t r = bn_divmod_small(a, 1000000000u);
        for (int k = 0; k < 9; k++) { tmp[n++] = (char)('0' + r % 10); r /= 10; }
    }
    while (n > 1 && tmp[n - 1] == '0') n--;
    if (!n) tmp[n++] = '0';
    int m = MIN(n, size - 1);
    for (int i = 0; i < m; i++) buf[i] = tmp[n - 1 - i];
    buf[m] = 0;
    return m;
}

/* ---- doubles as bits ----------------------------------------------------------------------- */
static uint64_t dbits(double d) { union { double d; uint64_t u; } x; x.d = d; return x.u; }
static double dfrom(uint64_t u) { union { double d; uint64_t u; } x; x.u = u; return x.d; }

/* v = f * 2^e, f < 2^53 */
static void decompose(double v, uint64_t *f, int *e, int *bexp) {
    uint64_t b = dbits(v);
    int be = (int)((b >> 52) & 0x7FF);
    uint64_t frac = b & 0xFFFFFFFFFFFFFull;
    *bexp = be;
    if (be == 0) { *f = frac; *e = -1074; }
    else { *f = frac | (1ull << 52); *e = be - 1075; }
}

/* ---- shortest digits ------------------------------------------------------------------------- */
/* digits of v > 0 (finite) into d (no trailing zeros); returns count, *point: v = 0.d1d2... x 10^point */
static int shortest(double v, char *d, int *point) {
    uint64_t f;
    int e, be;
    decompose(v, &f, &e, &be);
    bool even = (f & 1) == 0;
    BN r, s, mp, mm;
    if (e >= 0) {
        if (f != (1ull << 52)) {
            bn_set(&r, f); bn_shl(&r, e + 1);
            bn_set(&s, 2);
            bn_set(&mp, 1); bn_shl(&mp, e);
            mm = mp;
        } else {
            bn_set(&r, f); bn_shl(&r, e + 2);
            bn_set(&s, 4);
            bn_set(&mp, 1); bn_shl(&mp, e + 1);
            bn_set(&mm, 1); bn_shl(&mm, e);
        }
    } else {
        if (be <= 1 || f != (1ull << 52)) {
            bn_set(&r, f); bn_shl(&r, 1);
            bn_set(&s, 1); bn_shl(&s, 1 - e);
            bn_set(&mp, 1);
            bn_set(&mm, 1);
        } else {
            bn_set(&r, f); bn_shl(&r, 2);
            bn_set(&s, 1); bn_shl(&s, 2 - e);
            bn_set(&mp, 2);
            bn_set(&mm, 1);
        }
    }
    int k = (int)ceil(log10(v) - 1e-10);
    if (k >= 0) bn_mul_pow10(&s, k);
    else { bn_mul_pow10(&r, -k); bn_mul_pow10(&mp, -k); bn_mul_pow10(&mm, -k); }
    /* fixup */
    int c = bn_cmp_sum(&r, &mp, &s);
    if (even ? c >= 0 : c > 0) k++;
    else { bn_mul_small(&r, 10); bn_mul_small(&mp, 10); bn_mul_small(&mm, 10); }
    int n = 0;
    for (;;) {
        int dig = 0;
        while (bn_cmp(&r, &s) >= 0) { bn_sub(&r, &s); dig++; }
        int c1 = bn_cmp(&r, &mm);
        bool tc1 = even ? c1 <= 0 : c1 < 0;
        int c2 = bn_cmp_sum(&r, &mp, &s);
        bool tc2 = even ? c2 >= 0 : c2 > 0;
        if (!tc1 && !tc2 && n < 30) {
            d[n++] = (char)('0' + dig);
            bn_mul_small(&r, 10); bn_mul_small(&mp, 10); bn_mul_small(&mm, 10);
            continue;
        }
        if (tc1 && tc2) {
            BN t = r;
            bn_shl(&t, 1);
            int c3 = bn_cmp(&t, &s);
            if (c3 > 0 || (c3 == 0 && (dig & 1))) dig++;
        } else if (tc2) dig++;
        d[n++] = (char)('0' + dig);
        break;
    }
    /* a carry out of the last digit (cannot normally happen) */
    for (int i = n - 1; i > 0 && d[i] > '9'; i--) { d[i] = '0'; d[i - 1]++; }
    if (d[0] > '9') { d[0] = '1'; for (int i = 1; i < n; i++) d[i] = '0'; k++; }
    while (n > 1 && d[n - 1] == '0') n--;
    d[n] = 0;
    *point = k;
    return n;
}

static int put_int(char *buf, uint64_t v) {
    char t[24];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < n; i++) buf[i] = t[n - 1 - i];
    buf[n] = 0;
    return n;
}

/* the spec's Number::toString layout of digits d (k of them) with point n */
static int layout(char *buf, const char *d, int k, int n) {
    int o = 0;
    if (k <= n && n <= 21) {
        memcpy(buf, d, (size_t)k); o = k;
        for (int i = 0; i < n - k; i++) buf[o++] = '0';
    } else if (0 < n && n <= 21) {
        memcpy(buf, d, (size_t)n); o = n;
        buf[o++] = '.';
        memcpy(buf + o, d + n, (size_t)(k - n)); o += k - n;
    } else if (-6 < n && n <= 0) {
        buf[o++] = '0'; buf[o++] = '.';
        for (int i = 0; i < -n; i++) buf[o++] = '0';
        memcpy(buf + o, d, (size_t)k); o += k;
    } else {
        buf[o++] = d[0];
        if (k > 1) { buf[o++] = '.'; memcpy(buf + o, d + 1, (size_t)(k - 1)); o += k - 1; }
        buf[o++] = 'e';
        int x = n - 1;
        buf[o++] = x < 0 ? '-' : '+';
        o += put_int(buf + o, (uint64_t)(x < 0 ? -x : x));
    }
    buf[o] = 0;
    return o;
}

int js_dtoa(double v, char *buf) {
    if (v != v) { strcpy(buf, "NaN"); return 3; }
    if (v == 0) { strcpy(buf, "0"); return 1; }
    int o = 0;
    if (v < 0) { buf[o++] = '-'; v = -v; }
    if (isinf(v)) { strcpy(buf + o, "Infinity"); return o + 8; }
    if (v < 9007199254740992.0 && v == floor(v)) return o + put_int(buf + o, (uint64_t)v);
    char d[40];
    int n;
    int k = shortest(v, d, &n);
    return o + layout(buf + o, d, k, n);
}

JSValue js_num_to_str(JSContext *ctx, double d) {
    char buf[64];
    int n = js_dtoa(d, buf);
    return js_str8(ctx, buf, (uint32_t)n);
}

/* ---- exact decimal expansion ---------------------------------------------------------------------- */
/* all digits of v > 0: v = 0.d1d2...dn x 10^point; returns n */
static int exact_digits(double v, char *d, int size, int *point) {
    uint64_t f;
    int e, be;
    decompose(v, &f, &e, &be);
    BN a;
    bn_set(&a, f);
    int scale = 0;     /* v = a x 10^-scale */
    if (e >= 0) bn_shl(&a, e);
    else { bn_mul_pow5(&a, -e); scale = -e; }
    int n = bn_to_dec(&a, d, size);
    /* bn_to_dec trimmed nothing at the end except the digits it produced; count trailing zeros */
    int intdigits = n - scale;          /* digits before the point */
    *point = intdigits;
    while (n > 1 && d[n - 1] == '0') n--;
    d[n] = 0;
    return n;
}

/* round digit string d (n digits, point p) to keep `keep` digits (half up); returns new count, may bump p */
static int round_digits(char *d, int n, int keep, int *p) {
    if (keep >= n) return n;
    if (keep < 0) { d[0] = '0'; return 0; }
    bool up = d[keep] >= '5';
    n = keep;
    if (up) {
        int i = n - 1;
        while (i >= 0 && d[i] == '9') { d[i] = '0'; i--; }
        if (i >= 0) d[i]++;
        else { memmove(d + 1, d, (size_t)n); d[0] = '1'; n++; (*p)++; }
    }
    d[n] = 0;
    return n;
}

int js_dtoa_fixed(double v, int frac, char *buf, int size) {
    if (v != v) { strlcpy(buf, "NaN", (size_t)size); return 3; }
    if (fabs(v) >= 1e21) return js_dtoa(v, buf);
    int o = 0;
    if (v < 0) { buf[o++] = '-'; v = -v; }
    char d[1400];
    int p = 0, n = 0;
    if (v > 0) n = exact_digits(v, d, sizeof d - 2, &p);
    int keep = p + frac;
    if (v > 0) {
        if (keep < 0) n = 0;
        else if (keep == 0) { if (d[0] >= '5') { d[0] = '1'; n = 1; p++; } else n = 0; }
        else n = round_digits(d, n, keep, &p);
    }
    /* integer part */
    if (p <= 0 || n == 0) buf[o++] = '0';
    else for (int i = 0; i < p; i++) buf[o++] = i < n ? d[i] : '0';
    if (frac > 0) {
        buf[o++] = '.';
        for (int i = 0; i < frac; i++) {
            int idx = p + i;
            buf[o++] = (n && idx >= 0 && idx < n) ? d[idx] : '0';
        }
    }
    buf[o] = 0;
    (void)size;
    return o;
}

int js_dtoa_exp(double v, int frac, char *buf, int size) {
    if (v != v) { strlcpy(buf, "NaN", (size_t)size); return 3; }
    int o = 0;
    if (v < 0) { buf[o++] = '-'; v = -v; }
    if (isinf(v)) { strcpy(buf + o, "Infinity"); return o + 8; }
    char d[1400];
    int p = 0, n;
    if (v == 0) { d[0] = '0'; n = 1; p = 1; }
    else if (frac < 0) n = shortest(v, d, &p);
    else { n = exact_digits(v, d, sizeof d - 2, &p); n = round_digits(d, n, frac + 1, &p); }
    int nd = frac < 0 ? n : frac + 1;
    buf[o++] = d[0];
    if (nd > 1) {
        buf[o++] = '.';
        for (int i = 1; i < nd; i++) buf[o++] = i < n ? d[i] : '0';
    }
    buf[o++] = 'e';
    int x = v == 0 ? 0 : p - 1;
    buf[o++] = x < 0 ? '-' : '+';
    o += put_int(buf + o, (uint64_t)(x < 0 ? -x : x));
    buf[o] = 0;
    return o;
}

int js_dtoa_prec(double v, int prec, char *buf, int size) {
    if (v != v) { strlcpy(buf, "NaN", (size_t)size); return 3; }
    int o = 0;
    if (v < 0) { buf[o++] = '-'; v = -v; }
    if (isinf(v)) { strcpy(buf + o, "Infinity"); return o + 8; }
    char d[1400];
    int p, n;
    if (v == 0) { memset(d, '0', (size_t)prec); n = prec; p = 1; }
    else { n = exact_digits(v, d, sizeof d - 2, &p); n = round_digits(d, n, prec, &p); }
    for (int i = n; i < prec; i++) d[i] = '0';
    d[prec] = 0;
    int e = p - 1;
    if (e < -6 || e >= prec) {
        buf[o++] = d[0];
        if (prec > 1) { buf[o++] = '.'; memcpy(buf + o, d + 1, (size_t)(prec - 1)); o += prec - 1; }
        buf[o++] = 'e';
        buf[o++] = e < 0 ? '-' : '+';
        o += put_int(buf + o, (uint64_t)(e < 0 ? -e : e));
    } else if (e >= 0) {
        memcpy(buf + o, d, (size_t)(e + 1)); o += e + 1;
        if (prec > e + 1) { buf[o++] = '.'; memcpy(buf + o, d + e + 1, (size_t)(prec - e - 1)); o += prec - e - 1; }
    } else {
        buf[o++] = '0'; buf[o++] = '.';
        for (int i = 0; i < -e - 1; i++) buf[o++] = '0';
        memcpy(buf + o, d, (size_t)prec); o += prec;
    }
    buf[o] = 0;
    return o;
}

/* ---- other radixes ----------------------------------------------------------------------------- */
int js_dtoa_radix(double v, int radix, char *out, int size) {
    static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (v != v) { strlcpy(out, "NaN", (size_t)size); return 3; }
    if (isinf(v)) { strlcpy(out, v < 0 ? "-Infinity" : "Infinity", (size_t)size); return (int)strlen(out); }
    bool neg = v < 0;
    if (neg) v = -v;
    char buf[2200];
    int icur = 1100, fcur = 1100;
    double integer = floor(v), fraction = v - integer;
    double next = dfrom(dbits(v) + 1);
    double delta = 0.5 * (next - v);
    double tiny = dfrom(1);
    if (delta < tiny || delta != delta) delta = tiny;
    if (v == 0) delta = tiny;
    if (fraction >= delta) {
        buf[fcur++] = '.';
        do {
            fraction *= radix;
            delta *= radix;
            int digit = (int)fraction;
            buf[fcur++] = chars[digit];
            fraction -= digit;
            if (fraction > 0.5 || (fraction == 0.5 && (digit & 1))) {
                if (fraction + delta > 1) {
                    /* round up, with carries through the digits written */
                    for (;;) {
                        fcur--;
                        if (fcur == 1100) { integer += 1; break; }
                        char c = buf[fcur];
                        int dg = c > '9' ? c - 'a' + 10 : c - '0';
                        if (dg + 1 < radix) { buf[fcur++] = chars[dg + 1]; break; }
                    }
                    break;
                }
            }
        } while (fraction >= delta && fcur < 2190);
    }
    while (integer / radix >= 9007199254740992.0 && icur > 2) { integer /= radix; buf[--icur] = '0'; }
    do {
        double rem = fmod(integer, radix);
        buf[--icur] = chars[(int)rem];
        integer = (integer - rem) / radix;
    } while (integer > 0 && icur > 1);
    if (neg) buf[--icur] = '-';
    int n = fcur - icur;
    if (n > size - 1) n = size - 1;
    memcpy(out, buf + icur, (size_t)n);
    out[n] = 0;
    return n;
}

/* ---- decimal string -> double ----------------------------------------------------------------------- */
static const double p10[23] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14,
                                1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };

/* n x 2^e2 (+ sticky bits below n) -> correctly rounded double (positive) */
static double bn_to_double(const BN *n, int e2, bool sticky) {
    int L = bn_bitlen(n);
    if (!L) return 0;
    int lead = L - 1 + e2;            /* exponent of the leading bit */
    if (lead > 1023) return INFINITY;
    int keep = 53;
    if (lead < -1022) keep = 53 - (-1022 - lead);
    if (keep < 0) return 0;           /* below half the smallest denormal */
    /* take the top `keep` bits of n, round half to even with everything below */
    int cut = L - keep;               /* bits below the kept ones */
    if (cut < 0) cut = 0;
    uint64_t m = 0;
    for (int i = L - 1; i >= cut && i >= 0; i--) m = (m << 1) | (bn_bit(n, i) ? 1 : 0);
    bool half = cut >= 1 && bn_bit(n, cut - 1);
    bool rest = sticky;
    for (int i = cut - 2; i >= 0 && !rest; i--) if (bn_bit(n, i)) rest = true;
    if (half && (rest || (m & 1))) m++;
    if (keep == 0) m = half && rest ? 1 : 0;
    /* m x 2^(cut + e2) */
    int ex = cut + e2;
    if (m == 0) return 0;
    if (m >> 53) { m >>= 1; ex++; }
    /* assemble: value = m x 2^ex */
    int top = 63 - __builtin_clzll(m);      /* bit index of the leading one */
    int be = top + ex + 1023;
    if (be >= 2047) return INFINITY;
    if (be <= 0) {
        /* denormal: m is already aligned so that ex == -1074 */
        int sh = ex + 1074;
        uint64_t frac = sh >= 0 ? m << sh : m >> -sh;
        return dfrom(frac);
    }
    uint64_t frac = (m << (52 - top)) & 0xFFFFFFFFFFFFFull;
    return dfrom(((uint64_t)be << 52) | frac);
}

#define MAXDIG 800

double js_strtod16(const uint16_t *s, uint32_t n, uint32_t *used, bool allow_trailing) {
    uint32_t i = 0;
    bool neg = false;
    (void)allow_trailing;
    if (i < n && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    char dig[MAXDIG + 2];
    int nd = 0;
    bool sticky = false, any = false;
    int dexp = 0;               /* value = digits x 10^dexp */
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        any = true;
        if (nd == 0 && s[i] == '0') { i++; continue; }
        if (nd < MAXDIG) dig[nd++] = (char)s[i];
        else { dexp++; if (s[i] != '0') sticky = true; }
        i++;
    }
    if (i < n && s[i] == '.') {
        uint32_t save = i;
        i++;
        bool fd = false;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            fd = true;
            if (nd == 0 && s[i] == '0') { dexp--; i++; continue; }
            if (nd < MAXDIG) { dig[nd++] = (char)s[i]; dexp--; }
            else if (s[i] != '0') sticky = true;
            i++;
        }
        if (fd) any = true;
        else if (!any) i = save;
    }
    if (!any) { *used = 0; return NAN; }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        uint32_t j = i + 1;
        bool eneg = false;
        if (j < n && (s[j] == '+' || s[j] == '-')) { eneg = s[j] == '-'; j++; }
        if (j < n && s[j] >= '0' && s[j] <= '9') {
            int ev = 0;
            while (j < n && s[j] >= '0' && s[j] <= '9') { if (ev < 100000) ev = ev * 10 + (s[j] - '0'); j++; }
            dexp += eneg ? -ev : ev;
            i = j;
        }
    }
    *used = i;
    double r;
    if (nd == 0) r = 0;
    else if (nd + dexp > 310) r = INFINITY;
    else if (nd + dexp < -330) r = 0;
    else if (nd <= 15 && !sticky && dexp >= -22 && dexp <= 22 + (15 - nd)) {
        uint64_t m = 0;
        for (int k = 0; k < nd; k++) m = m * 10 + (uint64_t)(dig[k] - '0');
        double x = (double)m;
        if (dexp < 0) r = x / p10[-dexp];
        else if (dexp <= 22) r = x * p10[dexp];
        else { x *= p10[dexp - 22]; r = x * 1e22; }
    } else {
        BN M;
        M.n = 0;
        for (int k = 0; k < nd; k++) {
            if (!M.n) bn_set(&M, (uint64_t)(dig[k] - '0'));
            else { bn_mul_small(&M, 10); bn_add_small(&M, (uint32_t)(dig[k] - '0')); }
        }
        if (sticky) { bn_mul_small(&M, 10); bn_add_small(&M, 1); dexp--; }
        if (dexp >= 0) {
            bn_mul_pow10(&M, dexp);
            r = bn_to_double(&M, 0, false);
        } else {
            BN D;
            bn_set(&D, 1);
            bn_mul_pow10(&D, -dexp);
            int sh = 58 + bn_bitlen(&D) - bn_bitlen(&M);
            if (sh < 0) sh = 0;
            BN A = M;
            bn_shl(&A, sh);
            /* q = A / D by shift and subtract */
            BN q;
            q.n = 0;
            int top = bn_bitlen(&A) - bn_bitlen(&D);
            for (int b = top; b >= 0; b--) {
                BN t = D;
                bn_shl(&t, b);
                if (bn_cmp(&t, &A) <= 0) {
                    bn_sub(&A, &t);
                    int w = b / 32;
                    while (q.n <= w) q.w[q.n++] = 0;
                    q.w[w] |= 1u << (b % 32);
                }
            }
            r = bn_to_double(&q, -sh, !bn_zero(&A));
        }
    }
    return neg ? -r : r;
}

static bool ws(uint16_t c) { return js_is_space(c); }

double js_str_to_number(JSContext *ctx, JSString *str) {
    (void)ctx;
    const uint16_t *s = str->s;
    uint32_t n = str->len, i = 0;
    while (i < n && ws(s[i])) i++;
    while (n > i && ws(s[n - 1])) n--;
    if (i == n) return 0;
    s += i;
    n -= i;
    if (n > 2 && s[0] == '0' && (s[1] | 0x20) != 'e' && ((s[1] | 0x20) == 'x' || (s[1] | 0x20) == 'o' || (s[1] | 0x20) == 'b')) {
        int base = (s[1] | 0x20) == 'x' ? 16 : (s[1] | 0x20) == 'o' ? 8 : 2;
        double v = 0;
        for (uint32_t k = 2; k < n; k++) {
            int c = s[k], d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') d = (c | 0x20) - 'a' + 10;
            else return NAN;
            if (d >= base) return NAN;
            v = v * base + d;
        }
        if (v >= 9007199254740992.0) {   /* exact: re-read with big integers */
            BN a;
            a.n = 0;
            for (uint32_t k = 2; k < n; k++) {
                int c = s[k];
                int d = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
                if (!a.n) bn_set(&a, (uint64_t)d);
                else { bn_mul_small(&a, (uint32_t)base); bn_add_small(&a, (uint32_t)d); }
            }
            v = bn_to_double(&a, 0, false);
        }
        return v;
    }
    uint32_t k = 0;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') { neg = s[0] == '-'; k = 1; }
    if (n - k == 8) {
        static const char inf[] = "Infinity";
        bool m = true;
        for (int j = 0; j < 8; j++) if (s[k + (uint32_t)j] != (uint8_t)inf[j]) { m = false; break; }
        if (m) return neg ? -INFINITY : INFINITY;
    }
    uint32_t used = 0;
    double v = js_strtod16(s, n, &used, false);
    if (used != n) return NAN;
    return v;
}
