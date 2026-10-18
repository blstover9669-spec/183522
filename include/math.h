/* math.h - NovaOS libm subset (x87 FPU based, double precision). */
#ifndef _NOVA_MATH_H
#define _NOVA_MATH_H

#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_E         2.7182818284590452354
#define M_SQRT2     1.41421356237309504880
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402

#define INFINITY    (__builtin_inff())
#define NAN         (__builtin_nanf(""))
#define HUGE_VAL    (__builtin_huge_val())

#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)

double sqrt(double x);
double cbrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);
double fabs(double x);
double fmod(double x, double y);
double hypot(double x, double y);
double fmin(double a, double b);
double fmax(double a, double b);
double copysign(double x, double y);
double modf(double x, double *ip);
double frexp(double x, int *e);
double ldexp(double x, int e);
long   lround(double x);

/* float variants (computed in double) */
static inline float sqrtf(float x)  { return (float)sqrt(x); }
static inline float sinf(float x)   { return (float)sin(x); }
static inline float cosf(float x)   { return (float)cos(x); }
static inline float tanf(float x)   { return (float)tan(x); }
static inline float atanf(float x)  { return (float)atan(x); }
static inline float atan2f(float y, float x) { return (float)atan2(y, x); }
static inline float asinf(float x)  { return (float)asin(x); }
static inline float acosf(float x)  { return (float)acos(x); }
static inline float powf(float x, float y) { return (float)pow(x, y); }
static inline float expf(float x)   { return (float)exp(x); }
static inline float logf(float x)   { return (float)log(x); }
static inline float log10f(float x) { return (float)log10(x); }
static inline float floorf(float x) { return (float)floor(x); }
static inline float ceilf(float x)  { return (float)ceil(x); }
static inline float roundf(float x) { return (float)round(x); }
static inline float truncf(float x) { return (float)trunc(x); }
static inline float fabsf(float x)  { return x < 0 ? -x : x; }
static inline float fmodf(float x, float y) { return (float)fmod(x, y); }
static inline float hypotf(float x, float y) { return (float)hypot(x, y); }
static inline float fminf(float a, float b) { return a < b ? a : b; }
static inline float fmaxf(float a, float b) { return a > b ? a : b; }

#endif
