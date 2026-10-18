/* stdio.h - NovaOS libc subset (formatting only; printf goes to the serial log). */
#ifndef _NOVA_STDIO_H
#define _NOVA_STDIO_H
#include <stddef.h>
#include <stdarg.h>

#ifndef EOF
#define EOF (-1)
#endif

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vsprintf(char *buf, const char *fmt, va_list ap);
/* printf/puts write to the COM1 serial log (there is no console). */
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintf(const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);
/* Minimal sscanf: %d %i %u %x %o %ld %lu %lld %f %lf %s %c %[...] %n %% and literal matching. */
int sscanf(const char *str, const char *fmt, ...);
int vsscanf(const char *str, const char *fmt, va_list ap);

#endif
