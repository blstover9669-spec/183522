/* sscanf.c - small vsscanf implementation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int vsscanf(const char *str, const char *fmt, va_list ap) {
    const char *s = str;
    int assigned = 0;
    while (*fmt) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*s)) s++;
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt) break;
            s++; fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            while (isspace((unsigned char)*s)) s++;
            if (*s != '%') break;
            s++; fmt++;
            continue;
        }
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }
        int width = 0;
        while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        int lng = 0;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') { if (*fmt == 'l') lng++; else if (*fmt == 'h') lng--; fmt++; }
        char conv = *fmt++;
        if (!conv) break;
        if (conv != 'c' && conv != '[' && conv != 'n')
            while (isspace((unsigned char)*s)) s++;
        if (conv == 'n') {
            if (!suppress) *va_arg(ap, int *) = (int)(s - str);
            continue;
        }
        if (!*s) break;
        char tmp[128];
        int maxw = width > 0 && width < (int)sizeof tmp ? width : (int)sizeof tmp - 1;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            int base = conv == 'd' || conv == 'u' ? 10 : conv == 'i' ? 0 : conv == 'o' ? 8 : 16;
            int n = 0;
            while (s[n] && n < maxw && !isspace((unsigned char)s[n])) { tmp[n] = s[n]; n++; }
            tmp[n] = 0;
            char *end;
            long long v = strtoll(tmp, &end, base);
            if (end == tmp) return assigned ? assigned : (assigned == 0 && !*s ? EOF : assigned);
            s += end - tmp;
            if (!suppress) {
                if (lng >= 2) *va_arg(ap, long long *) = v;
                else if (lng == 1) *va_arg(ap, long *) = (long)v;
                else if (lng == -1) *va_arg(ap, short *) = (short)v;
                else if (lng <= -2) *va_arg(ap, char *) = (char)v;
                else *va_arg(ap, int *) = (int)v;
                assigned++;
            }
            break;
        }
        case 'f': case 'e': case 'g': case 'E': case 'G': {
            int n = 0;
            while (s[n] && n < maxw && !isspace((unsigned char)s[n])) { tmp[n] = s[n]; n++; }
            tmp[n] = 0;
            char *end;
            double v = strtod(tmp, &end);
            if (end == tmp) return assigned;
            s += end - tmp;
            if (!suppress) {
                if (lng >= 1) *va_arg(ap, double *) = v;
                else *va_arg(ap, float *) = (float)v;
                assigned++;
            }
            break;
        }
        case 's': {
            char *out = suppress ? NULL : va_arg(ap, char *);
            int n = 0;
            int lim = width > 0 ? width : 0x7fffffff;
            while (*s && !isspace((unsigned char)*s) && n < lim) {
                if (out) out[n] = *s;
                n++; s++;
            }
            if (out) { out[n] = 0; assigned++; }
            break;
        }
        case 'c': {
            char *out = suppress ? NULL : va_arg(ap, char *);
            int cnt = width > 0 ? width : 1;
            for (int i = 0; i < cnt && *s; i++) { if (out) out[i] = *s; s++; }
            if (out) assigned++;
            break;
        }
        case '[': {
            int invert = 0;
            if (*fmt == '^') { invert = 1; fmt++; }
            const char *set = fmt;
            if (*fmt == ']') fmt++;
            while (*fmt && *fmt != ']') fmt++;
            int setlen = (int)(fmt - set);
            if (*fmt == ']') fmt++;
            char *out = suppress ? NULL : va_arg(ap, char *);
            int n = 0;
            int lim = width > 0 ? width : 0x7fffffff;
            while (*s && n < lim) {
                int in = memchr(set, *s, (size_t)setlen) != NULL;
                if (in == invert) break;
                if (out) out[n] = *s;
                n++; s++;
            }
            if (n == 0) return assigned;
            if (out) { out[n] = 0; assigned++; }
            break;
        }
        default:
            return assigned;
        }
    }
    return assigned;
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf(str, fmt, ap);
    va_end(ap);
    return n;
}
