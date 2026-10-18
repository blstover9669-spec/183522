/* stdlib.c - conversions, random numbers, sorting, heap wrappers. */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <assert.h>
#include <nova/sys.h>

void *malloc(size_t size) { return kmalloc(size); }
void *calloc(size_t n, size_t size) { return kcalloc(n, size); }
void *realloc(void *p, size_t size) { return krealloc(p, size); }
void free(void *p) { kfree(p); }

static int digit_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = *p == '-'; p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if ((base == 0 || base == 2) && p[0] == '0' && (p[1] == 'b' || p[1] == 'B') && (p[2] == '0' || p[2] == '1')) {
        p += 2;
        base = 2;
    } else if (base == 0) {
        base = (p[0] == '0') ? 8 : 10;
    }
    unsigned long long v = 0;
    const char *start = p;
    while (digit_val((unsigned char)*p) < base) {
        v = v * (unsigned)base + (unsigned)digit_val((unsigned char)*p);
        p++;
    }
    if (end) *end = (char *)(p == start ? s : p);
    return neg ? (unsigned long long)(-(long long)v) : v;
}

long long strtoll(const char *s, char **end, int base) {
    return (long long)strtoull(s, end, base);
}

long strtol(const char *s, char **end, int base) {
    long long v = strtoll(s, end, base);
    if (v > LONG_MAX) return LONG_MAX;
    if (v < LONG_MIN) return LONG_MIN;
    return (long)v;
}

unsigned long strtoul(const char *s, char **end, int base) {
    return (unsigned long)strtoull(s, end, base);
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }

/* Decimal mantissa accumulated as an integer, scaled once by a power of ten:
   exact for typical inputs ("12.5e2", "0.1"). */
static double pow10i(int e) {
    double r = 1.0, b = 10.0;
    int n = e < 0 ? -e : e;
    while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
    return r;
}

double strtod(const char *s, char **end) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = *p == '-'; p++; }
    if (!strncasecmp(p, "inf", 3)) {
        p += 3;
        if (!strncasecmp(p, "inity", 5)) p += 5;
        if (end) *end = (char *)p;
        return neg ? -__builtin_inf() : __builtin_inf();
    }
    if (!strncasecmp(p, "nan", 3)) {
        if (end) *end = (char *)(p + 3);
        return __builtin_nan("");
    }
    uint64_t mant = 0;
    int digits = 0, exp10 = 0, any = 0;
    while (isdigit((unsigned char)*p)) {
        if (digits < 19) { mant = mant * 10 + (uint64_t)(*p - '0'); if (mant) digits++; }
        else exp10++;
        p++;
        any = 1;
    }
    if (*p == '.') {
        p++;
        while (isdigit((unsigned char)*p)) {
            if (digits < 19) { mant = mant * 10 + (uint64_t)(*p - '0'); if (mant) digits++; exp10--; }
            p++;
            any = 1;
        }
    }
    if (any && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1;
        int eneg = 0;
        if (*q == '+' || *q == '-') { eneg = *q == '-'; q++; }
        if (isdigit((unsigned char)*q)) {
            int e = 0;
            while (isdigit((unsigned char)*q)) { if (e < 10000) e = e * 10 + (*q - '0'); q++; }
            exp10 += eneg ? -e : e;
            p = q;
        }
    }
    if (end) *end = (char *)(any ? p : s);
    double v = (double)mant;
    if (exp10 > 308) v = mant ? __builtin_inf() : 0.0;
    else if (exp10 < -340) v = 0.0;
    else if (exp10 < -300) { v /= pow10i(300); v /= pow10i(-exp10 - 300); }
    else if (exp10 < 0) v /= pow10i(-exp10);
    else if (exp10 > 0) v *= pow10i(exp10);
    return neg ? -v : v;
}

float strtof(const char *s, char **end) { return (float)strtod(s, end); }
double atof(const char *s) { return strtod(s, NULL); }

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }
div_t div(int a, int b) { div_t d = { a / b, a % b }; return d; }

/* xorshift32 PRNG */
static uint32_t rng_state = 2463534242u;
int rand(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return (int)(x & RAND_MAX);
}
void srand(unsigned int seed) { rng_state = seed ? seed : 2463534242u; }
int rand_range(int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int)((unsigned)rand() % (unsigned)(hi - lo + 1));
}

/* qsort: introsort-lite (quicksort with median-of-3 + insertion sort for small runs). */
static void swap_bytes(char *a, char *b, size_t n) {
    while (n >= 4) { uint32_t t = *(uint32_t *)a; *(uint32_t *)a = *(uint32_t *)b; *(uint32_t *)b = t; a += 4; b += 4; n -= 4; }
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}

static void qsort_r_impl(char *base, size_t n, size_t sz, int (*cmp)(const void *, const void *), int depth) {
    while (n > 12) {
        if (depth-- <= 0) {
            /* fall back to heap-free insertion sort on pathological input (rare) */
            break;
        }
        char *mid = base + (n / 2) * sz, *last = base + (n - 1) * sz;
        if (cmp(mid, base) < 0) swap_bytes(mid, base, sz);
        if (cmp(last, base) < 0) swap_bytes(last, base, sz);
        if (cmp(last, mid) < 0) swap_bytes(last, mid, sz);
        swap_bytes(mid, last - sz, sz);           /* pivot at n-2 */
        char *pivot = last - sz;
        char *i = base, *j = pivot;
        for (;;) {
            do { i += sz; } while (cmp(i, pivot) < 0);
            do { j -= sz; } while (j > base && cmp(j, pivot) > 0);
            if (i >= j) break;
            swap_bytes(i, j, sz);
        }
        swap_bytes(i, pivot, sz);
        size_t left = (size_t)(i - base) / sz;
        size_t right = n - left - 1;
        /* recurse into the smaller side, loop on the larger */
        if (left < right) {
            qsort_r_impl(base, left, sz, cmp, depth);
            base = i + sz;
            n = right;
        } else {
            qsort_r_impl(i + sz, right, sz, cmp, depth);
            n = left;
        }
    }
    /* insertion sort */
    for (size_t a = 1; a < n; a++) {
        for (size_t b = a; b > 0 && cmp(base + (b - 1) * sz, base + b * sz) > 0; b--)
            swap_bytes(base + (b - 1) * sz, base + b * sz, sz);
    }
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    if (n < 2 || size == 0) return;
    qsort_r_impl(base, n, size, cmp, 64);
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *)) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const char *e = (const char *)base + mid * size;
        int c = cmp(key, e);
        if (c == 0) return (void *)e;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

char *utoa(unsigned value, char *buf, int base) {
    char tmp[36];
    int n = 0;
    if (base < 2 || base > 36) base = 10;
    do { int d = (int)(value % (unsigned)base); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); value /= (unsigned)base; } while (value);
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return buf;
}

char *itoa(int value, char *buf, int base) {
    if (value < 0 && base == 10) {
        buf[0] = '-';
        utoa((unsigned)(-(value + 1)) + 1u, buf + 1, base);
        return buf;
    }
    return utoa((unsigned)value, buf, base);
}

void abort(void) { kpanic("abort() called"); }

void __assert_fail(const char *expr, const char *file, int line, const char *func) {
    kpanic("Assertion failed: %s\n  at %s:%d (%s)", expr, file, line, func);
}
