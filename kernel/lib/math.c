/* math.c - libm subset implemented with x87 FPU instructions. */
#include <math.h>
#include <stdint.h>

double fabs(double x) { return x < 0 ? -x : (x == 0 ? 0.0 : x); }

double sqrt(double x) {
    double r;
    __asm__("fsqrt" : "=t"(r) : "0"(x));
    return r;
}

double cbrt(double x) {
    if (x == 0.0) return 0.0;
    double r = pow(fabs(x), 1.0 / 3.0);
    return x < 0 ? -r : r;
}

/* fsin/fcos are only accurate for |x| < 2^63, and best for small |x|: reduce first. */
static double reduce_angle(double x) {
    if (x > 1e6 || x < -1e6) return fmod(x, 2.0 * M_PI);
    return x;
}

double sin(double x) {
    double r;
    x = reduce_angle(x);
    __asm__("fsin" : "=t"(r) : "0"(x));
    return r;
}

double cos(double x) {
    double r;
    x = reduce_angle(x);
    __asm__("fcos" : "=t"(r) : "0"(x));
    return r;
}

double tan(double x) {
    double r, one;
    x = reduce_angle(x);
    __asm__("fptan" : "=t"(one), "=u"(r) : "0"(x));
    (void)one;
    return r;
}

double atan2(double y, double x) {
    double r;
    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

double atan(double x) { return atan2(x, 1.0); }

double asin(double x) {
    if (x > 1.0 || x < -1.0) return NAN;
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) {
    if (x > 1.0 || x < -1.0) return NAN;
    return atan2(sqrt(1.0 - x * x), x);
}

/* log2(x) via fyl2x */
double log2(double x) {
    if (x < 0) return NAN;
    if (x == 0) return -INFINITY;
    double r;
    __asm__("fld1\n\tfxch\n\tfyl2x" : "=t"(r) : "0"(x));
    return r;
}

double log(double x) { return log2(x) * M_LN2; }
double log10(double x) { return log2(x) * (M_LN2 / M_LN10); }

/* 2^x = 2^int * 2^frac, with f2xm1 for the fraction (|frac| <= 1) and fscale. */
double exp2(double x) {
    if (x != x) return x;
    if (x > 1024) return INFINITY;
    if (x < -1080) return 0.0;
    double ip = round(x);
    double fp = x - ip;
    double r;
    __asm__("f2xm1" : "=t"(r) : "0"(fp));
    r += 1.0;
    double out;
    __asm__("fscale" : "=t"(out) : "0"(r), "u"(ip));
    return out;
}

double exp(double x) { return exp2(x * 1.4426950408889634074); }

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 0.0) return y > 0 ? 0.0 : INFINITY;
    if (x == 1.0) return 1.0;
    if (x < 0) {
        /* only integer exponents are defined for negative bases */
        double yi = trunc(y);
        if (yi != y) return NAN;
        double r = exp2(y * log2(-x));
        long long iy = (long long)yi;
        return (iy & 1) ? -r : r;
    }
    /* exact for small integer powers */
    if (y == trunc(y) && y > 0 && y <= 64) {
        double r = 1.0, b = x;
        int e = (int)y;
        while (e) { if (e & 1) r *= b; b *= b; e >>= 1; }
        return r;
    }
    return exp2(y * log2(x));
}

/* Rounding with a temporary control-word change. mode: 0 nearest, 1 down, 2 up, 3 truncate */
static double round_mode(double x, int mode) {
    uint16_t cw, ncw;
    double r;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    ncw = (uint16_t)((cw & ~0x0C00) | (mode << 10));
    __asm__ volatile("fldcw %0" : : "m"(ncw));
    __asm__ volatile("frndint" : "=t"(r) : "0"(x));
    __asm__ volatile("fldcw %0" : : "m"(cw));
    return r;
}

double floor(double x) { return round_mode(x, 1); }
double ceil(double x) { return round_mode(x, 2); }
double trunc(double x) { return round_mode(x, 3); }
double round(double x) { return x >= 0 ? floor(x + 0.5) : ceil(x - 0.5); }
long lround(double x) { return (long)round(x); }

double fmod(double x, double y) {
    double r;
    if (y == 0.0) return NAN;
    __asm__("1: fprem\n\t"
            "fnstsw %%ax\n\t"
            "sahf\n\t"
            "jp 1b"
            : "=t"(r) : "0"(x), "u"(y) : "ax", "cc");
    return r;
}

double hypot(double x, double y) { return sqrt(x * x + y * y); }
double fmin(double a, double b) { return a < b ? a : b; }
double fmax(double a, double b) { return a > b ? a : b; }
double copysign(double x, double y) {
    x = fabs(x);
    return signbit(y) ? -x : x;
}

double modf(double x, double *ip) {
    double i = trunc(x);
    if (ip) *ip = i;
    return x - i;
}

double frexp(double x, int *e) {
    *e = 0;
    if (x == 0.0 || x != x) return x;
    double a = fabs(x);
    while (a >= 1.0) { a *= 0.5; (*e)++; }
    while (a < 0.5) { a *= 2.0; (*e)--; }
    return x < 0 ? -a : a;
}

double ldexp(double x, int e) {
    double r;
    double de = e;
    __asm__("fscale" : "=t"(r) : "0"(x), "u"(de));
    return r;
}

double sinh(double x) { double e = exp(x); return (e - 1.0 / e) * 0.5; }
double cosh(double x) { double e = exp(x); return (e + 1.0 / e) * 0.5; }
double tanh(double x) {
    if (x > 20) return 1.0;
    if (x < -20) return -1.0;
    double e = exp(2 * x);
    return (e - 1) / (e + 1);
}
