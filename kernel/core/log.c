/* log.c - COM1 serial port driver and kernel log (serial + in-memory ring). */
#include "kernel.h"

#define COM1 0x3F8

static bool serial_ok;

void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* disable interrupts      */
    outb(COM1 + 3, 0x80);   /* DLAB on                 */
    outb(COM1 + 0, 0x01);   /* divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1                     */
    outb(COM1 + 2, 0xC7);   /* FIFO on, clear, 14-byte */
    outb(COM1 + 4, 0x0B);   /* DTR, RTS, OUT2          */
    /* loopback self-test */
    outb(COM1 + 4, 0x1E);
    outb(COM1 + 0, 0xAE);
    serial_ok = inb(COM1 + 0) == 0xAE;
    outb(COM1 + 4, 0x0F);
}

bool serial_present(void) { return serial_ok; }

void serial_putc(char c) {
    if (!serial_ok) return;
    int spins = 100000;
    while (!(inb(COM1 + 5) & 0x20) && --spins) {}
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

/* ---- ring buffer of recent log text ------------------------------------------------ */
#define LOG_RING 16384
static char log_ring[LOG_RING];
static uint32_t log_head;     /* total bytes ever written */

static void ring_write(const char *s) {
    while (*s) log_ring[log_head++ % LOG_RING] = *s++;
}

int sys_log_text(char *buf, int size) {
    if (size <= 0) return 0;
    uint32_t avail = log_head < LOG_RING ? log_head : LOG_RING;
    uint32_t n = avail < (uint32_t)(size - 1) ? avail : (uint32_t)(size - 1);
    uint32_t start = log_head - n;
    for (uint32_t i = 0; i < n; i++) buf[i] = log_ring[(start + i) % LOG_RING];
    buf[n] = 0;
    return (int)n;
}

void kvlog(const char *fmt, va_list ap) {
    char line[600];
    uint32_t t = g_ticks;
    int n = snprintf(line, sizeof line, "[%5u.%03u] ", t / 1000, t % 1000);
    n += vsnprintf(line + n, sizeof line - (size_t)n - 2, fmt, ap);
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    if (n > 0 && line[n - 1] != '\n') { line[n++] = '\n'; line[n] = 0; }
    uint32_t f = irq_save();
    serial_write(line);
    ring_write(line);
    irq_restore(f);
}

void klog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvlog(fmt, ap);
    va_end(ap);
}
