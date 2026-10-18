/* stdlib.h - NovaOS libc subset */
#ifndef _NOVA_STDLIB_H
#define _NOVA_STDLIB_H
#include <stddef.h>
#include <stdint.h>

#define RAND_MAX 0x7fffffff
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* Heap (backed by the kernel allocator; see nova/sys.h kmalloc & friends). */
void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t size);
void  free(void *p);

int        atoi(const char *s);
long       atol(const char *s);
long long  atoll(const char *s);
double     atof(const char *s);
long       strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long  strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double     strtod(const char *s, char **end);
float      strtof(const char *s, char **end);

int        abs(int x);
long       labs(long x);
long long  llabs(long long x);
typedef struct { int quot, rem; } div_t;
div_t      div(int a, int b);

int  rand(void);
void srand(unsigned int seed);
/* NovaOS extension: uniform integer in [lo, hi] inclusive. */
int  rand_range(int lo, int hi);

void  qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *));

/* Integer -> string helpers (NovaOS extensions). Return buf. */
char *itoa(int value, char *buf, int base);
char *utoa(unsigned value, char *buf, int base);

/* abort() triggers a kernel panic. */
void abort(void) __attribute__((noreturn));

#endif
