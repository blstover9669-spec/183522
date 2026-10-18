/*
 * nova/hw.h - low-level hardware access for drivers (port I/O, IRQs, PCI).
 * Apps normally do not need this.
 */
#ifndef NOVA_HW_H
#define NOVA_HW_H

#include <nova/types.h>

static inline void outb(uint16_t port, uint8_t v)  { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint8_t  inb(uint16_t port) { uint8_t v;  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint16_t inw(uint16_t port) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint32_t inl(uint16_t port) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void insw(uint16_t port, void *buf, uint32_t count) {
    __asm__ volatile("rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}
static inline void outsw(uint16_t port, const void *buf, uint32_t count) {
    __asm__ volatile("rep outsw" : "+S"(buf), "+c"(count) : "d"(port) : "memory");
}
static inline void io_wait(void) { outb(0x80, 0); }

/* Interrupt flag helpers. irq_save() disables interrupts and returns the old state. */
static inline uint32_t irq_save(void) {
    uint32_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) : : "memory");
    return f;
}
static inline void irq_restore(uint32_t f) {
    if (f & 0x200) __asm__ volatile("sti" : : : "memory");
}
static inline void cpu_cli(void) { __asm__ volatile("cli" : : : "memory"); }
static inline void cpu_sti(void) { __asm__ volatile("sti" : : : "memory"); }
static inline void cpu_hlt(void) { __asm__ volatile("hlt"); }

/* Register registers snapshot passed to interrupt handlers. */
typedef struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} regs_t;

typedef void (*irq_handler_t)(regs_t *r);
/* Install a handler for hardware IRQ 0..15 (unmasks it on the PIC). Handlers
   run with interrupts disabled and must be short; push work to the main loop. */
void irq_install(int irq, irq_handler_t handler);
void irq_uninstall(int irq);
void irq_mask(int irq);
void irq_unmask(int irq);

/* ---- PCI (configuration mechanism #1) ----------------------------------------- */
typedef struct pci_device {
    uint8_t  bus, slot, func;
    uint16_t vendor, device;
    uint8_t  class_code, subclass, prog_if, revision;
    uint8_t  irq_line;
    uint32_t bar[6];
} pci_device_t;

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint8_t  pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v);
void     pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t v);
/* Devices found at boot. */
int                 pci_device_count(void);
const pci_device_t *pci_get_device(int index);
const pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass);
const pci_device_t *pci_find_device(uint16_t vendor, uint16_t device);
/* Enable bus mastering + I/O + memory decoding for a device. */
void                pci_enable(const pci_device_t *dev);

#endif
