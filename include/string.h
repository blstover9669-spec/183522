/* string.h - NovaOS libc subset */
#ifndef _NOVA_STRING_H
#define _NOVA_STRING_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);
/* NovaOS extension: fill n 32-bit words with value v (fast pixel fill). */
void  *memset32(void *dst, uint32_t v, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
size_t strlcpy(char *dst, const char *src, size_t size);   /* always NUL-terminates */
size_t strlcat(char *dst, const char *src, size_t size);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
char  *strcasestr(const char *hay, const char *needle);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strpbrk(const char *s, const char *accept);
char  *strtok(char *s, const char *delim);
char  *strtok_r(char *s, const char *delim, char **save);
char  *strdup(const char *s);                /* kmalloc'd; free() it */
char  *strndup(const char *s, size_t n);
char  *strerror(int err);

#ifdef __cplusplus
}
#endif
#endif
