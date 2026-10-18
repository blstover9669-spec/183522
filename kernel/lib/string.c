/* string.c - memory and string functions for the NovaOS libc subset. */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

void *memcpy(void *dst, const void *src, size_t n) {
    void *ret = dst;
    uint32_t d0, d1, d2;
    /* copy dwords then the remaining bytes */
    __asm__ volatile(
        "rep movsl\n\t"
        "movl %4, %%ecx\n\t"
        "rep movsb"
        : "=&c"(d0), "=&D"(d1), "=&S"(d2)
        : "0"(n >> 2), "g"(n & 3), "1"(dst), "2"(src)
        : "memory");
    return ret;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) return memcpy(dst, src, n);
    /* overlapping, copy backwards */
    uint32_t d0, d1, d2;
    __asm__ volatile(
        "std\n\t"
        "rep movsb\n\t"
        "cld"
        : "=&c"(d0), "=&D"(d1), "=&S"(d2)
        : "0"(n), "1"(d + n - 1), "2"(s + n - 1)
        : "memory");
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint32_t v = (uint8_t)c;
    v |= v << 8;
    v |= v << 16;
    uint32_t d0, d1;
    __asm__ volatile(
        "rep stosl\n\t"
        "movl %3, %%ecx\n\t"
        "rep stosb"
        : "=&c"(d0), "=&D"(d1)
        : "a"(v), "g"(n & 3), "0"(n >> 2), "1"(dst)
        : "memory");
    return dst;
}

void *memset32(void *dst, uint32_t v, size_t n) {
    uint32_t d0, d1;
    __asm__ volatile("rep stosl" : "=&c"(d0), "=&D"(d1) : "a"(v), "0"(n), "1"(dst) : "memory");
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (unsigned char)c) return (void *)(p + i);
    return NULL;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}

size_t strlcat(char *dst, const char *src, size_t size) {
    size_t dl = strnlen(dst, size);
    if (dl == size) return size + strlen(src);
    return dl + strlcpy(dst + dl, src, size - dl);
}

char *strcat(char *dst, const char *src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    size_t i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    d[i] = 0;
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[i]);
        if (ca != cb || !ca) return ca - cb;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c) {
    const char *r = NULL;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}

char *strstr(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return (char *)hay;
    for (; *hay; hay++)
        if (*hay == *needle && !strncmp(hay, needle, n)) return (char *)hay;
    return NULL;
}

char *strcasestr(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return (char *)hay;
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return (char *)hay;
    return NULL;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) n++;
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) n++;
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++)
        if (strchr(accept, *s)) return (char *)s;
    return NULL;
}

char *strtok_r(char *s, const char *delim, char **save) {
    if (!s) s = *save;
    if (!s) return NULL;
    s += strspn(s, delim);
    if (!*s) { *save = NULL; return NULL; }
    char *end = s + strcspn(s, delim);
    if (*end) { *end = 0; *save = end + 1; }
    else *save = NULL;
    return s;
}

char *strtok(char *s, const char *delim) {
    static char *save;
    return strtok_r(s, delim, &save);
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *d = malloc(len + 1);
    if (d) { memcpy(d, s, len); d[len] = 0; }
    return d;
}

char *strerror(int err) {
    switch (err) {
    case 0: return "Success";
    case 1: return "Operation not permitted";
    case 2: return "No such file or directory";
    case 12: return "Out of memory";
    case 22: return "Invalid argument";
    default: return "Unknown error";
    }
}
