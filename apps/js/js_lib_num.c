/*
 * js_lib_num.c - NovaJS built-ins: Number, Math and Date.
 *
 * NovaOS keeps the clock in UTC and has no time zones, so a Date's local
 * time is UTC (getTimezoneOffset() is 0).
 */
#include "jsi.h"

#define FN(name) static JSValue name(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic)
#define A(i) JS_ARG(i)

/* ==== Number =================================================================================== */
FN(num_ctor) {
    double d = 0;
    if (argc > 0) {
        bool e = false;
        d = js_tonum(ctx, argv[0], &e);
        if (e) return JS_EXCEPTION;
    }
    JSValue nt = js_new_target(ctx);
    if (JS_IsUndefined(nt)) return js_mknum(d);
    JSValue p = js_getp(ctx, nt, ctx->a[A_prototype]);
    if (JS_IsException(p)) return p;
    JSObject *o = js_obj_alloc(ctx, CL_NUMBER, JS_IsObject(p) ? OBJ(p) : ctx->proto_number);
    if (!o) return JS_EXCEPTION;
    o->u.prim = js_mknum(d);
    return JS_MKOBJ(o);
}

/* isFinite 0, isInteger 1, isNaN 2, isSafeInteger 3 */
FN(num_is) {
    JSValue v = A(0);
    if (!JS_IsNumber(v)) return JS_FALSE;
    double d = js_num(v);
    switch (magic) {
    case 0: return JS_BOOL(isfinite(d));
    case 1: return JS_BOOL(isfinite(d) && trunc(d) == d);
    case 2: return JS_BOOL(d != d);
    default: return JS_BOOL(isfinite(d) && trunc(d) == d && fabs(d) <= 9007199254740991.0);
    }
}

static int this_num(JSContext *ctx, JSValue v, double *out) {
    if (JS_IsNumber(v)) { *out = js_num(v); return 0; }
    if (js_is_cls(v, CL_NUMBER)) { *out = js_num(OBJ(v)->u.prim); return 0; }
    js_throw_type(ctx, "Number.prototype method called on incompatible receiver");
    return -1;
}

FN(nump_to_string) {
    double d;
    if (this_num(ctx, this_val, &d) < 0) return JS_EXCEPTION;
    int radix = 10;
    if (!JS_IsUndefined(A(0))) {
        bool e = false;
        double r = js_tointeger(ctx, A(0), &e);
        if (e) return JS_EXCEPTION;
        if (r < 2 || r > 36) return js_throw_range(ctx, "toString() radix must be between 2 and 36");
        radix = (int)r;
    }
    if (radix == 10) return js_num_to_str(ctx, d);
    char buf[2300];
    int n = js_dtoa_radix(d, radix, buf, sizeof buf);
    return js_str8(ctx, buf, (uint32_t)n);
}

FN(nump_to_locale) {
    double d;
    if (this_num(ctx, this_val, &d) < 0) return JS_EXCEPTION;
    if (!isfinite(d)) return js_num_to_str(ctx, d);
    /* en-US: grouping, at most 3 fraction digits */
    int maxf = 3, minf = 0;
    JSValue opts = A(1);
    if (JS_IsObject(opts)) {
        JSValue v = js_get(ctx, opts, "maximumFractionDigits");
        if (JS_IsException(v)) return v;
        if (JS_IsNumber(v)) maxf = (int)js_num(v);
        v = js_get(ctx, opts, "minimumFractionDigits");
        if (JS_IsException(v)) return v;
        if (JS_IsNumber(v)) { minf = (int)js_num(v); if (maxf < minf) maxf = minf; }
        if (maxf < 0) maxf = 0;
        if (maxf > 20) maxf = 20;
    }
    char tmp[400], out[500];
    js_dtoa_fixed(d, maxf, tmp, sizeof tmp);
    /* drop trailing zeros beyond minf */
    char *dot = strchr(tmp, '.');
    if (dot) {
        int n = (int)strlen(dot + 1);
        while (n > minf && dot[n] == '0') dot[n--] = 0;
        if (n == 0) *dot = 0;
    }
    int o = 0;
    const char *p = tmp;
    if (*p == '-') { out[o++] = '-'; p++; }
    const char *ie = strchr(p, '.');
    int il = ie ? (int)(ie - p) : (int)strlen(p);
    for (int i = 0; i < il; i++) {
        if (i && (il - i) % 3 == 0) out[o++] = ',';
        out[o++] = p[i];
    }
    if (ie) { strcpy(out + o, ie); o += (int)strlen(ie); }
    out[o] = 0;
    return js_str8(ctx, out, (uint32_t)o);
}

FN(nump_value_of) {
    double d;
    if (this_num(ctx, this_val, &d) < 0) return JS_EXCEPTION;
    return js_mknum(d);
}

/* toFixed 0, toExponential 1, toPrecision 2 */
FN(nump_format) {
    double d;
    if (this_num(ctx, this_val, &d) < 0) return JS_EXCEPTION;
    bool e = false;
    char buf[1500];
    int n;
    if (magic == 0) {
        double f = js_tointeger(ctx, A(0), &e);
        if (e) return JS_EXCEPTION;
        if (f < 0 || f > 100) return js_throw_range(ctx, "toFixed() digits argument must be between 0 and 100");
        if (!isfinite(d)) return js_num_to_str(ctx, d);
        n = js_dtoa_fixed(d, (int)f, buf, sizeof buf);
    } else if (magic == 1) {
        double f = js_tointeger(ctx, A(0), &e);
        if (e) return JS_EXCEPTION;
        if (!isfinite(d)) return js_num_to_str(ctx, d);
        if (f < 0 || f > 100) return js_throw_range(ctx, "toExponential() argument must be between 0 and 100");
        n = js_dtoa_exp(d, JS_IsUndefined(A(0)) ? -1 : (int)f, buf, sizeof buf);
    } else {
        if (JS_IsUndefined(A(0))) return js_num_to_str(ctx, d);
        double p = js_tointeger(ctx, A(0), &e);
        if (e) return JS_EXCEPTION;
        if (!isfinite(d)) return js_num_to_str(ctx, d);
        if (p < 1 || p > 100) return js_throw_range(ctx, "toPrecision() argument must be between 1 and 100");
        n = js_dtoa_prec(d, (int)p, buf, sizeof buf);
    }
    return js_str8(ctx, buf, (uint32_t)n);
}

/* ==== Math =================================================================================== */
double js_random(JSContext *ctx) {
    /* xorshift128 */
    uint32_t *s = ctx->rnd;
    uint32_t t = s[3];
    uint32_t x = s[0];
    s[3] = s[2]; s[2] = s[1]; s[1] = x;
    t ^= t << 11;
    t ^= t >> 8;
    s[0] = t ^ x ^ (x >> 19);
    uint32_t u = s[0];
    t = s[3];
    x = s[0];
    s[3] = s[2]; s[2] = s[1]; s[1] = x;
    t ^= t << 11; t ^= t >> 8;
    s[0] = t ^ x ^ (x >> 19);
    uint64_t r = ((uint64_t)(u >> 5) << 26) | (s[0] >> 6);   /* 53 bits */
    return (double)r / 9007199254740992.0;
}

static double js_round(double x) {
    if (!isfinite(x) || x == trunc(x)) return x;
    double r = floor(x);
    if (x - r >= 0.5) r += 1;
    if (r == 0 && x < 0) return -0.0;
    return r;
}

static double cbrt_(double x) {
    if (x == 0 || !isfinite(x)) return x;
    double a = fabs(x);
    double r = pow(a, 1.0 / 3.0);
    r = r - (r * r * r - a) / (3 * r * r);
    double ri = round(r);
    if (ri * ri * ri == a) r = ri;
    return x < 0 ? -r : r;
}

static double log1p_(double x) {
    double u = 1 + x;
    if (u == 1) return x;
    return log(u) * x / (u - 1);
}

static double expm1_(double x) {
    if (fabs(x) < 1e-5) return x + x * x / 2 + x * x * x / 6;
    return exp(x) - 1;
}

FN(math_fn1) {
    bool e = false;
    double x = JS_IsNumber(A(0)) ? js_num(A(0)) : js_tonum(ctx, A(0), &e);
    if (e) return JS_EXCEPTION;
    double r;
    switch (magic) {
    case 0: r = fabs(x); break;
    case 1: r = acos(x); break;
    case 2: r = x < 1 ? NAN : log(x + sqrt(x * x - 1)); break;
    case 3: r = asin(x); break;
    case 4: r = x == 0 ? x : (isinf(x) ? x : copysign(log(fabs(x) + sqrt(x * x + 1)), x)); break;
    case 5: r = atan(x); break;
    case 6: r = (x == 0) ? x : 0.5 * log1p_(2 * x / (1 - x)); break;
    case 7: r = cbrt_(x); break;
    case 8: r = ceil(x); break;
    case 9: r = cos(x); break;
    case 10: r = cosh(x); break;
    case 11: r = exp(x); break;
    case 12: r = x == 0 ? x : expm1_(x); break;
    case 13: r = floor(x); break;
    case 14: r = (double)(float)x; break;
    case 15: r = log(x); break;
    case 16: r = x == 0 ? x : log1p_(x); break;
    case 17: r = log10(x); break;
    case 18: r = log2(x); break;
    case 19: r = js_round(x); break;
    case 20: r = x != x ? x : x > 0 ? 1 : x < 0 ? -1 : x; break;
    case 21: r = sin(x); break;
    case 22: r = sinh(x); break;
    case 23: r = sqrt(x); break;
    case 24: r = tan(x); break;
    case 25: r = tanh(x); break;
    case 26: r = trunc(x); break;
    case 27: { uint32_t u = js_d2u32(x); r = u ? __builtin_clz(u) : 32; break; }
    default: r = NAN; break;
    }
    if (r == 0 && (magic == 8 || magic == 13 || magic == 26) && signbit(x)) r = -0.0;
    return js_mknum(r);
}

FN(math_atan2) {
    bool e = false;
    double y = js_tonum(ctx, A(0), &e);
    if (e) return JS_EXCEPTION;
    double x = js_tonum(ctx, A(1), &e);
    if (e) return JS_EXCEPTION;
    return js_mknum(atan2(y, x));
}

FN(math_pow) {
    bool e = false;
    double x = js_tonum(ctx, A(0), &e);
    if (e) return JS_EXCEPTION;
    double y = js_tonum(ctx, A(1), &e);
    if (e) return JS_EXCEPTION;
    if (y != y || ((x == 1 || x == -1) && isinf(y))) return js_mknum(NAN);
    return js_mknum(pow(x, y));
}

FN(math_imul) {
    int32_t a, b;
    if (js_toint32_e(ctx, A(0), &a) < 0 || js_toint32_e(ctx, A(1), &b) < 0) return JS_EXCEPTION;
    return js_mknum((int32_t)((uint32_t)a * (uint32_t)b));
}

/* max 0, min 1 */
FN(math_minmax) {
    double r = magic ? INFINITY : -INFINITY;
    bool nan = false;
    for (int i = 0; i < argc; i++) {
        bool e = false;
        double x = JS_IsNumber(argv[i]) ? js_num(argv[i]) : js_tonum(ctx, argv[i], &e);
        if (e) return JS_EXCEPTION;
        if (x != x) nan = true;
        else if (magic ? (x < r || (x == 0 && r == 0 && signbit(x))) : (x > r || (x == 0 && r == 0 && !signbit(x)))) r = x;
    }
    return js_mknum(nan ? NAN : r);
}

FN(math_hypot) {
    double s = 0, m = 0;
    bool inf = false, nan = false;
    double *v = argc ? malloc(sizeof(double) * (size_t)argc) : NULL;
    if (argc && !v) return js_throw_oom(ctx);
    for (int i = 0; i < argc; i++) {
        bool e = false;
        v[i] = js_tonum(ctx, argv[i], &e);
        if (e) { free(v); return JS_EXCEPTION; }
        if (isinf(v[i])) inf = true;
        if (v[i] != v[i]) nan = true;
        if (fabs(v[i]) > m) m = fabs(v[i]);
    }
    if (inf) { free(v); return js_mknum(INFINITY); }
    if (nan) { free(v); return js_mknum(NAN); }
    if (m == 0) { free(v); return js_mknum(0); }
    for (int i = 0; i < argc; i++) { double t = v[i] / m; s += t * t; }
    free(v);
    return js_mknum(m * sqrt(s));
}

FN(math_random) { return js_mknum(js_random(ctx)); }

/* ==== Date ======================================================================================= */
#define MS_DAY 86400000.0

double js_now(JSContext *ctx) {
    return ctx->host.now_ms ? floor(ctx->host.now_ms(ctx->host.ud)) : 0;
}

static double day_of(double t) { return floor(t / MS_DAY); }
static double time_in_day(double t) { double r = fmod(t, MS_DAY); return r < 0 ? r + MS_DAY : r; }
static double days_in_year(double y) {
    if (fmod(y, 4) != 0) return 365;
    if (fmod(y, 100) != 0) return 366;
    if (fmod(y, 400) != 0) return 365;
    return 366;
}
static double day_from_year(double y) { return 365 * (y - 1970) + floor((y - 1969) / 4) - floor((y - 1901) / 100) + floor((y - 1601) / 400); }
static double year_from_time(double t) {
    double y = floor(t / (MS_DAY * 365.2425)) + 1970;
    while (day_from_year(y) * MS_DAY > t) y--;
    while (day_from_year(y + 1) * MS_DAY <= t) y++;
    return y;
}
static bool leap(double y) { return days_in_year(y) == 366; }
static const int cum_days[2][13] = { { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
                                     { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 } };

static void split_date(double t, int *y, int *mo, int *d, int *h, int *mi, int *s, int *ms, int *wd) {
    double yy = year_from_time(t);
    int L = leap(yy) ? 1 : 0;
    int doy = (int)(day_of(t) - day_from_year(yy));
    int m = 0;
    while (m < 11 && doy >= cum_days[L][m + 1]) m++;
    *y = (int)yy;
    *mo = m;
    *d = doy - cum_days[L][m] + 1;
    double tid = time_in_day(t);
    *h = (int)(tid / 3600000.0);
    *mi = (int)fmod(floor(tid / 60000.0), 60);
    *s = (int)fmod(floor(tid / 1000.0), 60);
    *ms = (int)fmod(tid, 1000);
    double w = fmod(day_of(t) + 4, 7);
    if (w < 0) w += 7;
    *wd = (int)w;
}

static double make_day(double y, double m, double d) {
    if (!isfinite(y) || !isfinite(m) || !isfinite(d)) return NAN;
    y = trunc(y); m = trunc(m); d = trunc(d);
    double ym = y + floor(m / 12);
    double mn = fmod(m, 12);
    if (mn < 0) mn += 12;
    if (fabs(ym) > 400000) return NAN;
    double day = day_from_year(ym) + cum_days[leap(ym) ? 1 : 0][(int)mn];
    return day + d - 1;
}

static double make_time(double h, double m, double s, double ms) {
    if (!isfinite(h) || !isfinite(m) || !isfinite(s) || !isfinite(ms)) return NAN;
    return trunc(h) * 3600000.0 + trunc(m) * 60000.0 + trunc(s) * 1000.0 + trunc(ms);
}

static double time_clip(double t) {
    if (!isfinite(t) || fabs(t) > 8.64e15) return NAN;
    return trunc(t) + 0.0;
}

static const char *wd_names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *mo_names[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* kind: 0 toString, 1 toDateString, 2 toTimeString, 3 toISOString, 4 toUTCString, 5 locale, 6 locale date, 7 locale time */
JSValue js_date_to_string(JSContext *ctx, double t, int kind) {
    if (t != t) {
        if (kind == 3) return js_throw_range(ctx, "Invalid time value");
        return js_strc(ctx, "Invalid Date");
    }
    int y, mo, d, h, mi, s, ms, wd;
    split_date(t, &y, &mo, &d, &h, &mi, &s, &ms, &wd);
    char buf[100], ys[16];
    if (y >= 0) snprintf(ys, sizeof ys, "%04d", y); else snprintf(ys, sizeof ys, "-%06d", -y);
    int h12 = h % 12 == 0 ? 12 : h % 12;
    switch (kind) {
    case 0: snprintf(buf, sizeof buf, "%s %s %02d %s %02d:%02d:%02d GMT+0000 (Coordinated Universal Time)", wd_names[wd], mo_names[mo], d, ys, h, mi, s); break;
    case 1: snprintf(buf, sizeof buf, "%s %s %02d %s", wd_names[wd], mo_names[mo], d, ys); break;
    case 2: snprintf(buf, sizeof buf, "%02d:%02d:%02d GMT+0000 (Coordinated Universal Time)", h, mi, s); break;
    case 3:
        if (y >= 0 && y <= 9999) snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", y, mo + 1, d, h, mi, s, ms);
        else snprintf(buf, sizeof buf, "%c%06d-%02d-%02dT%02d:%02d:%02d.%03dZ", y < 0 ? '-' : '+', y < 0 ? -y : y, mo + 1, d, h, mi, s, ms);
        break;
    case 4: snprintf(buf, sizeof buf, "%s, %02d %s %s %02d:%02d:%02d GMT", wd_names[wd], d, mo_names[mo], ys, h, mi, s); break;
    case 5: snprintf(buf, sizeof buf, "%d/%d/%d, %d:%02d:%02d %s", mo + 1, d, y, h12, mi, s, h < 12 ? "AM" : "PM"); break;
    case 6: snprintf(buf, sizeof buf, "%d/%d/%d", mo + 1, d, y); break;
    default: snprintf(buf, sizeof buf, "%d:%02d:%02d %s", h12, mi, s, h < 12 ? "AM" : "PM"); break;
    }
    return js_strc(ctx, buf);
}

/* ---- parsing ---- */
static int rd_digits(const uint16_t *s, uint32_t n, uint32_t *i, int max, int *v) {
    int k = 0;
    *v = 0;
    while (*i < n && k < max && s[*i] >= '0' && s[*i] <= '9') { *v = *v * 10 + (s[*i] - '0'); (*i)++; k++; }
    return k;
}

static double parse_iso(const uint16_t *s, uint32_t n) {
    uint32_t i = 0;
    int y, mo = 1, d = 1, h = 0, mi = 0, sec = 0, ms = 0;
    int sign = 1;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        sign = s[i] == '-' ? -1 : 1;
        i++;
        if (rd_digits(s, n, &i, 6, &y) != 6) return NAN;
        if (sign < 0 && y == 0) return NAN;
    } else if (rd_digits(s, n, &i, 4, &y) != 4) return NAN;
    y *= sign;
    bool date_only = true, has_tz = false;
    double tz = 0;
    if (i < n && s[i] == '-') {
        i++;
        if (rd_digits(s, n, &i, 2, &mo) != 2) return NAN;
        if (i < n && s[i] == '-') { i++; if (rd_digits(s, n, &i, 2, &d) != 2) return NAN; }
    }
    if (i < n && (s[i] == 'T' || s[i] == 't' || s[i] == ' ')) {
        i++;
        date_only = false;
        if (rd_digits(s, n, &i, 2, &h) != 2 || i >= n || s[i] != ':') return NAN;
        i++;
        if (rd_digits(s, n, &i, 2, &mi) != 2) return NAN;
        if (i < n && s[i] == ':') {
            i++;
            if (rd_digits(s, n, &i, 2, &sec) != 2) return NAN;
            if (i < n && (s[i] == '.' || s[i] == ',')) {
                i++;
                int k = 0, v = 0;
                uint32_t st = i;
                while (i < n && s[i] >= '0' && s[i] <= '9') { if (k < 3) { v = v * 10 + (s[i] - '0'); k++; } i++; }
                if (i == st) return NAN;
                while (k < 3) { v *= 10; k++; }
                ms = v;
            }
        }
        if (i < n && (s[i] == 'Z' || s[i] == 'z')) { has_tz = true; i++; }
        else if (i < n && (s[i] == '+' || s[i] == '-')) {
            int sg = s[i] == '-' ? -1 : 1, th, tm = 0;
            i++;
            if (rd_digits(s, n, &i, 2, &th) != 2) return NAN;
            if (i < n && s[i] == ':') i++;
            rd_digits(s, n, &i, 2, &tm);
            tz = sg * (th * 60 + tm) * 60000.0;
            has_tz = true;
        }
    }
    if (i != n) return NAN;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 24 || mi > 59 || sec > 59 || (h == 24 && (mi || sec || ms))) return NAN;
    (void)date_only;
    double t = make_day(y, mo - 1, d) * MS_DAY + make_time(h, mi, sec, ms);
    if (has_tz) t -= tz;
    return t;
}

static int month_of(const uint16_t *w, uint32_t n) {
    static const char *m[12] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
    if (n < 3) return -1;
    for (int k = 0; k < 12; k++)
        if ((w[0] | 0x20) == m[k][0] && (w[1] | 0x20) == m[k][1] && (w[2] | 0x20) == m[k][2]) return k;
    return -1;
}

/* fallback: "Wed Nov 11 2026 10:00:00 GMT+0000", "11 Nov 2026", "Nov 11, 2026 3:04 PM", "2026/11/11", "11/11/2026" */
static double parse_loose(const uint16_t *s, uint32_t n) {
    int nums[8], nn = 0, month = -1, h = -1, mi = 0, sec = 0;
    bool pm = false, am = false;
    double tz = 0;
    int ymd[3] = { -1, -1, -1 };
    bool slash = false;
    for (uint32_t i = 0; i < n;) {
        uint32_t c = s[i];
        if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') {
            uint32_t st = i;
            while (i < n && (((s[i] | 0x20) >= 'a' && (s[i] | 0x20) <= 'z') || s[i] == '.')) i++;
            uint32_t len = i - st;
            int mo = month_of(s + st, len);
            if (mo >= 0 && month < 0) month = mo;
            else if (len == 2 && (s[st] | 0x20) == 'p' && (s[st + 1] | 0x20) == 'm') pm = true;
            else if (len == 2 && (s[st] | 0x20) == 'a' && (s[st + 1] | 0x20) == 'm') am = true;
            else if ((len == 3 && (s[st] | 0x20) == 'g') || (len == 3 && (s[st] | 0x20) == 'u') || (len == 1 && (s[st] | 0x20) == 'z')) {
                if (i < n && (s[i] == '+' || s[i] == '-')) {
                    int sg = s[i] == '-' ? -1 : 1, v = 0, k = 0;
                    i++;
                    while (i < n && s[i] >= '0' && s[i] <= '9' && k < 4) { v = v * 10 + (s[i] - '0'); i++; k++; }
                    if (i < n && s[i] == ':') { i++; while (i < n && s[i] >= '0' && s[i] <= '9' && k < 4) { v = v * 10 + (s[i] - '0'); i++; k++; } }
                    if (k <= 2) v *= 100;
                    tz = sg * ((v / 100) * 60 + v % 100) * 60000.0;
                }
            }
            continue;
        }
        if (c >= '0' && c <= '9') {
            int v = 0;
            uint32_t st = i;
            while (i < n && s[i] >= '0' && s[i] <= '9') { if (v < 100000000) v = v * 10 + (s[i] - '0'); i++; }
            if (i < n && s[i] == ':') {
                h = v;
                i++;
                rd_digits(s, n, &i, 2, &mi);
                if (i < n && s[i] == ':') { i++; rd_digits(s, n, &i, 2, &sec); }
                if (i < n && s[i] == '.') { i++; int x; rd_digits(s, n, &i, 9, &x); }
                continue;
            }
            if (i < n && (s[i] == '/' || s[i] == '-') && st == i - (i - st)) {
                int k = 0;
                ymd[k++] = v;
                while (i < n && (s[i] == '/' || s[i] == '-') && k < 3) {
                    i++;
                    int w = 0;
                    if (!rd_digits(s, n, &i, 9, &w)) break;
                    ymd[k++] = w;
                }
                slash = true;
                continue;
            }
            if (nn < 8) nums[nn++] = v;
            continue;
        }
        if ((c == '+' || c == '-') && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9' && h >= 0) {
            int sg = c == '-' ? -1 : 1, v = 0, k = 0;
            i++;
            while (i < n && s[i] >= '0' && s[i] <= '9' && k < 4) { v = v * 10 + (s[i] - '0'); i++; k++; }
            tz = sg * ((v / 100) * 60 + v % 100) * 60000.0;
            continue;
        }
        i++;
    }
    int y = -1, mo = month, d = -1;
    if (slash) {
        if (ymd[0] > 31) { y = ymd[0]; mo = ymd[1] - 1; d = ymd[2] > 0 ? ymd[2] : 1; }
        else { mo = ymd[0] - 1; d = ymd[1]; y = ymd[2]; }
        if (y >= 0 && y < 50 && !(ymd[0] > 31)) y += 2000; else if (y >= 50 && y < 100) y += 1900;
    } else {
        for (int k = 0; k < nn; k++) {
            if (nums[k] > 31 || (d >= 0 && y < 0)) { if (y < 0) y = nums[k]; }
            else if (d < 0) d = nums[k];
        }
    }
    if (y < 0 || mo < 0 || mo > 11) return NAN;
    if (d < 0) d = 1;
    if (h < 0) h = 0;
    if (pm && h < 12) h += 12;
    if (am && h == 12) h = 0;
    return make_day(y, mo, d) * MS_DAY + make_time(h, mi, sec, 0) - tz;
}

static double date_parse(JSContext *ctx, JSValue sv) {
    JSString *s = js_flat(ctx, sv);
    if (!s) return NAN;
    uint32_t a = 0, b = s->len;
    while (a < b && js_is_space(s->s[a])) a++;
    while (b > a && js_is_space(s->s[b - 1])) b--;
    double t = parse_iso(s->s + a, b - a);
    if (t != t) t = parse_loose(s->s + a, b - a);
    return time_clip(t);
}

static int this_time(JSContext *ctx, JSValue v, double *t) {
    if (!js_is_cls(v, CL_DATE)) { js_throw_type(ctx, "this is not a Date object."); return -1; }
    *t = js_num(OBJ(v)->u.prim);
    return 0;
}

FN(date_ctor) {
    JSValue nt = js_new_target(ctx);
    if (JS_IsUndefined(nt)) return js_date_to_string(ctx, js_now(ctx), 0);
    double t;
    if (argc == 0) t = js_now(ctx);
    else if (argc == 1) {
        JSValue v = argv[0];
        if (js_is_cls(v, CL_DATE)) t = js_num(OBJ(v)->u.prim);
        else {
            v = js_to_primitive(ctx, v, 0);
            if (JS_IsException(v)) return v;
            if (JS_IsString(v)) t = date_parse(ctx, v);
            else { bool e = false; t = js_tonum(ctx, v, &e); if (e) return JS_EXCEPTION; }
        }
        t = time_clip(t);
    } else {
        double f[7] = { 0, 0, 1, 0, 0, 0, 0 };
        for (int i = 0; i < argc && i < 7; i++) { bool e = false; f[i] = js_tonum(ctx, argv[i], &e); if (e) return JS_EXCEPTION; }
        if (f[0] == f[0]) { double yi = trunc(f[0]); if (yi >= 0 && yi <= 99) f[0] = 1900 + yi; }
        t = time_clip(make_day(f[0], f[1], f[2]) * MS_DAY + make_time(f[3], f[4], f[5], f[6]));
    }
    JSValue p = js_getp(ctx, nt, ctx->a[A_prototype]);
    if (JS_IsException(p)) return p;
    JSObject *o = js_obj_alloc(ctx, CL_DATE, JS_IsObject(p) ? OBJ(p) : ctx->proto_date);
    if (!o) return JS_EXCEPTION;
    o->u.prim = js_mknum(t);
    return JS_MKOBJ(o);
}

FN(date_now) { return js_mknum(js_now(ctx)); }

FN(date_parse_fn) {
    JSValue s = js_tostr(ctx, A(0));
    if (JS_IsException(s)) return s;
    return js_mknum(date_parse(ctx, s));
}

FN(date_utc) {
    double f[7] = { NAN, 0, 1, 0, 0, 0, 0 };
    for (int i = 0; i < argc && i < 7; i++) { bool e = false; f[i] = js_tonum(ctx, argv[i], &e); if (e) return JS_EXCEPTION; }
    if (f[0] == f[0]) { double yi = trunc(f[0]); if (yi >= 0 && yi <= 99) f[0] = 1900 + yi; }
    return js_mknum(time_clip(make_day(f[0], f[1], f[2]) * MS_DAY + make_time(f[3], f[4], f[5], f[6])));
}

/* getters: 0 FullYear 1 Month 2 Date 3 Day 4 Hours 5 Minutes 6 Seconds 7 Milliseconds 8 Time 9 TimezoneOffset 10 Year */
FN(datep_get) {
    double t;
    if (this_time(ctx, this_val, &t) < 0) return JS_EXCEPTION;
    if (magic == 8) return js_mknum(t);
    if (t != t) return js_mknum(NAN);
    if (magic == 9) return js_mknum(0);
    int y, mo, d, h, mi, s, ms, wd;
    split_date(t, &y, &mo, &d, &h, &mi, &s, &ms, &wd);
    int v[11] = { y, mo, d, wd, h, mi, s, ms, 0, 0, y - 1900 };
    return js_mknum(v[magic]);
}

/* setters: 0 FullYear(y,m,d) 1 Month(m,d) 2 Date(d) 3 Hours(h,m,s,ms) 4 Minutes(m,s,ms) 5 Seconds(s,ms) 6 Milliseconds(ms) 7 Time 8 Year */
FN(datep_set) {
    double t;
    if (this_time(ctx, this_val, &t) < 0) return JS_EXCEPTION;
    double a[4];
    int n = argc < 4 ? argc : 4;
    for (int i = 0; i < 4; i++) a[i] = NAN;
    for (int i = 0; i < n; i++) { bool e = false; a[i] = js_tonum(ctx, argv[i], &e); if (e) return JS_EXCEPTION; }
    if (magic == 7) { t = time_clip(a[0]); OBJ(this_val)->u.prim = js_mknum(t); return js_mknum(t); }
    if (t != t) {
        if (magic != 0 && magic != 8) return js_mknum(NAN);
        t = 0;
    }
    int y, mo, d, h, mi, s, ms, wd;
    split_date(t, &y, &mo, &d, &h, &mi, &s, &ms, &wd);
    double Y = y, M = mo, D = d, H = h, MI = mi, S = s, MS = ms;
    switch (magic) {
    case 0: Y = a[0]; if (n > 1) M = a[1]; if (n > 2) D = a[2]; break;
    case 1: M = a[0]; if (n > 1) D = a[1]; break;
    case 2: D = a[0]; break;
    case 3: H = a[0]; if (n > 1) MI = a[1]; if (n > 2) S = a[2]; if (n > 3) MS = a[3]; break;
    case 4: MI = a[0]; if (n > 1) S = a[1]; if (n > 2) MS = a[2]; break;
    case 5: S = a[0]; if (n > 1) MS = a[1]; break;
    case 6: MS = a[0]; break;
    case 8: Y = a[0]; if (Y == Y) { double yi = trunc(Y); if (yi >= 0 && yi <= 99) Y = 1900 + yi; } break;
    }
    if (n == 0) { Y = M = D = NAN; }
    t = time_clip(make_day(Y, M, D) * MS_DAY + make_time(H, MI, S, MS));
    OBJ(this_val)->u.prim = js_mknum(t);
    return js_mknum(t);
}

FN(datep_string) {
    double t;
    if (this_time(ctx, this_val, &t) < 0) return JS_EXCEPTION;
    return js_date_to_string(ctx, t, magic);
}

FN(datep_to_json) {
    JSValue o = js_toobj(ctx, this_val);
    if (JS_IsException(o)) return o;
    JSValue tv = js_to_primitive(ctx, o, 1);
    if (JS_IsException(tv)) return tv;
    if (JS_IsNumber(tv) && !isfinite(js_num(tv))) return JS_NULL;
    return js_invoke(ctx, o, ctx->a[A_toISOString], 0, NULL);
}

FN(datep_to_prim) {
    if (!JS_IsObject(this_val)) return js_throw_type(ctx, "Date.prototype[Symbol.toPrimitive] called on non-object");
    int hint = 0;
    if (js_str_eq_ascii(ctx, A(0), "number")) hint = 1;
    else if (js_str_eq_ascii(ctx, A(0), "string") || js_str_eq_ascii(ctx, A(0), "default")) hint = 2;
    else return js_throw_type(ctx, "Invalid hint");
    int order[2] = { hint == 1 ? A_valueOf : A_toString, hint == 1 ? A_toString : A_valueOf };
    for (int i = 0; i < 2; i++) {
        JSValue f = js_getp(ctx, this_val, ctx->a[order[i]]);
        if (JS_IsException(f)) return f;
        if (js_is_callable(f)) {
            JSValue r = js_callv(ctx, f, this_val, 0, NULL);
            if (JS_IsException(r) || !JS_IsObject(r)) return r;
        }
    }
    return js_throw_type(ctx, "Cannot convert object to primitive value");
}

/* ==== setup ===================================================================================== */
void js_init_lib_num(JSContext *ctx) {
    ctx->proto_number = js_obj_alloc(ctx, CL_NUMBER, ctx->proto_object);
    ctx->proto_number->u.prim = js_mknum(0);
    JSObject *nc = js_new_ctor(ctx, "Number", num_ctor, 1, ctx->proto_number, 0);
    JSValue ncv = JS_MKOBJ(nc);
    static const struct { const char *n; double v; } consts[] = {
        { "EPSILON", 2.220446049250313e-16 }, { "MAX_SAFE_INTEGER", 9007199254740991.0 }, { "MIN_SAFE_INTEGER", -9007199254740991.0 },
        { "MAX_VALUE", 1.7976931348623157e308 }, { "MIN_VALUE", 5e-324 }, { "NEGATIVE_INFINITY", -INFINITY }, { "POSITIVE_INFINITY", INFINITY },
    };
    for (int i = 0; i < JS_COUNTOF(consts); i++) js_add_prop(ctx, nc, js_atomc(ctx, consts[i].n), js_mknum(consts[i].v), 0);
    js_add_prop(ctx, nc, ctx->a[A_NaN], js_mknum(NAN), 0);
    js_def_native(ctx, ncv, "isFinite", num_is, 1, 0);
    js_def_native(ctx, ncv, "isInteger", num_is, 1, 1);
    js_def_native(ctx, ncv, "isNaN", num_is, 1, 2);
    js_def_native(ctx, ncv, "isSafeInteger", num_is, 1, 3);
    JSValue g = ctx->global;
    JSProp *pf = js_find_prop(ctx, OBJ(g), js_atomc(ctx, "parseFloat"));
    JSProp *pi = js_find_prop(ctx, OBJ(g), js_atomc(ctx, "parseInt"));
    if (pf) js_add_prop(ctx, nc, js_atomc(ctx, "parseFloat"), pf->val, P_W | P_C);
    if (pi) js_add_prop(ctx, nc, js_atomc(ctx, "parseInt"), pi->val, P_W | P_C);
    JSValue np = JS_MKOBJ(ctx->proto_number);
    js_def_native(ctx, np, "toString", nump_to_string, 1, 0);
    js_def_native(ctx, np, "toLocaleString", nump_to_locale, 0, 0);
    js_def_native(ctx, np, "valueOf", nump_value_of, 0, 0);
    js_def_native(ctx, np, "toFixed", nump_format, 1, 0);
    js_def_native(ctx, np, "toExponential", nump_format, 1, 1);
    js_def_native(ctx, np, "toPrecision", nump_format, 1, 2);
    /* Math */
    JSValue m = js_obj_new(ctx, CL_OBJECT, ctx->proto_object);
    static const char *f1[] = { "abs", "acos", "acosh", "asin", "asinh", "atan", "atanh", "cbrt", "ceil", "cos", "cosh", "exp",
                                "expm1", "floor", "fround", "log", "log1p", "log10", "log2", "round", "sign", "sin", "sinh", "sqrt",
                                "tan", "tanh", "trunc", "clz32" };
    for (int i = 0; i < JS_COUNTOF(f1); i++) js_def_native(ctx, m, f1[i], math_fn1, 1, i);
    js_def_native(ctx, m, "atan2", math_atan2, 2, 0);
    js_def_native(ctx, m, "pow", math_pow, 2, 0);
    js_def_native(ctx, m, "imul", math_imul, 2, 0);
    js_def_native(ctx, m, "max", math_minmax, 2, 0);
    js_def_native(ctx, m, "min", math_minmax, 2, 1);
    js_def_native(ctx, m, "hypot", math_hypot, 2, 0);
    js_def_native(ctx, m, "random", math_random, 0, 0);
    static const struct { const char *n; double v; } mc[] = {
        { "E", 2.718281828459045 }, { "LN10", 2.302585092994046 }, { "LN2", 0.6931471805599453 }, { "LOG10E", 0.4342944819032518 },
        { "LOG2E", 1.4426950408889634 }, { "PI", 3.141592653589793 }, { "SQRT1_2", 0.7071067811865476 }, { "SQRT2", 1.4142135623730951 },
    };
    for (int i = 0; i < JS_COUNTOF(mc); i++) js_add_prop(ctx, OBJ(m), js_atomc(ctx, mc[i].n), js_mknum(mc[i].v), 0);
    js_add_prop(ctx, OBJ(m), ctx->wk[WK_TO_STRING_TAG], js_atomc(ctx, "Math"), P_C);
    js_add_prop(ctx, OBJ(g), js_atomc(ctx, "Math"), m, P_W | P_C);
    /* Date */
    ctx->proto_date = js_obj_alloc(ctx, CL_OBJECT, ctx->proto_object);
    JSObject *dc = js_new_ctor(ctx, "Date", date_ctor, 7, ctx->proto_date, 0);
    JSValue dcv = JS_MKOBJ(dc);
    js_def_native(ctx, dcv, "now", date_now, 0, 0);
    js_def_native(ctx, dcv, "parse", date_parse_fn, 1, 0);
    js_def_native(ctx, dcv, "UTC", date_utc, 7, 0);
    JSValue dp = JS_MKOBJ(ctx->proto_date);
    static const char *gn[11] = { "FullYear", "Month", "Date", "Day", "Hours", "Minutes", "Seconds", "Milliseconds", "Time", "TimezoneOffset", "Year" };
    for (int i = 0; i < 11; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, "get%s", gn[i]);
        js_def_native(ctx, dp, nm, datep_get, 0, i);
        if (i < 8 && i != 3) { snprintf(nm, sizeof nm, "getUTC%s", gn[i]); js_def_native(ctx, dp, nm, datep_get, 0, i); }
        if (i == 3) js_def_native(ctx, dp, "getUTCDay", datep_get, 0, 3);
    }
    js_def_native(ctx, dp, "valueOf", datep_get, 0, 8);
    static const char *sn[9] = { "FullYear", "Month", "Date", "Hours", "Minutes", "Seconds", "Milliseconds", "Time", "Year" };
    static const int sl[9] = { 3, 2, 1, 4, 3, 2, 1, 1, 1 };
    for (int i = 0; i < 9; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, "set%s", sn[i]);
        js_def_native(ctx, dp, nm, datep_set, sl[i], i);
        if (i < 7) { snprintf(nm, sizeof nm, "setUTC%s", sn[i]); js_def_native(ctx, dp, nm, datep_set, sl[i], i); }
    }
    static const char *tn[8] = { "toString", "toDateString", "toTimeString", "toISOString", "toUTCString", "toLocaleString",
                                 "toLocaleDateString", "toLocaleTimeString" };
    for (int i = 0; i < 8; i++) js_def_native(ctx, dp, tn[i], datep_string, 0, i);
    js_def_native(ctx, dp, "toGMTString", datep_string, 0, 4);
    js_def_native(ctx, dp, "toJSON", datep_to_json, 1, 0);
    js_def_native(ctx, dp, "@toPrimitive", datep_to_prim, 1, 0);
}
