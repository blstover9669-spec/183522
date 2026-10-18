/* cpu.c - GDT, IDT, PIC, PIT, FPU and CPUID. */
#include "kernel.h"

/* ---- GDT: flat 4 GiB code + data segments, ring 0 only -------------------- */
typedef struct PACKED { uint16_t limit_lo, base_lo; uint8_t base_mid, access, gran, base_hi; } gdt_entry_t;
typedef struct PACKED { uint16_t limit; uint32_t base; } gdt_ptr_t;

static gdt_entry_t gdt[3];
static gdt_ptr_t gdt_ptr;
extern void gdt_flush(uint32_t);

static void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[i].base_lo = base & 0xFFFF;
    gdt[i].base_mid = (base >> 16) & 0xFF;
    gdt[i].base_hi = (base >> 24) & 0xFF;
    gdt[i].limit_lo = limit & 0xFFFF;
    gdt[i].gran = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[i].access = access;
}

void gdt_init(void) {
    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);   /* 0x08 code */
    gdt_set(2, 0, 0xFFFFFFFF, 0x92, 0xCF);   /* 0x10 data */
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint32_t)&gdt;
    gdt_flush((uint32_t)&gdt_ptr);
}

/* ---- IDT -------------------------------------------------------------------- */
typedef struct PACKED { uint16_t off_lo, sel; uint8_t zero, flags; uint16_t off_hi; } idt_entry_t;
typedef struct PACKED { uint16_t limit; uint32_t base; } idt_ptr_t;

static idt_entry_t idt[256];
static idt_ptr_t idt_ptr;
extern uint32_t isr_stub_table[48];

void idt_set_gate(int n, uint32_t handler, uint16_t sel, uint8_t flags) {
    idt[n].off_lo = handler & 0xFFFF;
    idt[n].off_hi = (handler >> 16) & 0xFFFF;
    idt[n].sel = sel;
    idt[n].zero = 0;
    idt[n].flags = flags;
}

void idt_init(void) {
    memset(idt, 0, sizeof idt);
    for (int i = 0; i < 48; i++) idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E);
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint32_t)&idt;
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
}

/* ---- 8259 PIC ------------------------------------------------------------------ */
#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

static uint16_t pic_mask = 0xFFFB;   /* everything masked except the cascade (IRQ2) */

static void pic_write_mask(void) {
    outb(PIC1_DATA, pic_mask & 0xFF);
    outb(PIC2_DATA, (pic_mask >> 8) & 0xFF);
}

void pic_init(void) {
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 32); io_wait();      /* master vectors 32..39 */
    outb(PIC2_DATA, 40); io_wait();      /* slave vectors 40..47  */
    outb(PIC1_DATA, 4); io_wait();
    outb(PIC2_DATA, 2); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
    pic_write_mask();
}

void pic_eoi(int irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

void irq_mask(int irq) {
    pic_mask |= (uint16_t)(1u << irq);
    pic_write_mask();
}

void irq_unmask(int irq) {
    pic_mask &= (uint16_t)~(1u << irq);
    if (irq >= 8) pic_mask &= (uint16_t)~(1u << 2);
    pic_write_mask();
}

/* ---- interrupt dispatch ------------------------------------------------------------ */
static irq_handler_t irq_handlers[16];

void irq_install(int irq, irq_handler_t h) {
    if (irq < 0 || irq > 15) return;
    irq_handlers[irq] = h;
    irq_unmask(irq);
}

void irq_uninstall(int irq) {
    if (irq < 0 || irq > 15) return;
    irq_mask(irq);
    irq_handlers[irq] = NULL;
}

static const char *exception_names[32] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "BOUND range exceeded",
    "Invalid opcode", "Device not available", "Double fault", "Coprocessor overrun",
    "Invalid TSS", "Segment not present", "Stack-segment fault", "General protection fault",
    "Page fault", "Reserved", "x87 floating-point error", "Alignment check", "Machine check",
    "SIMD floating-point", "Virtualization", "Control protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor injection", "VMM communication",
    "Security", "Reserved"
};

extern void panic_exception(regs_t *r, const char *name) NORETURN;

void isr_dispatch(regs_t *r) {
    if (r->int_no < 32) {
        panic_exception(r, exception_names[r->int_no]);
    }
    int irq = (int)r->int_no - 32;
    if (irq == 7 || irq == 15) {
        /* spurious IRQ check via ISR register */
        uint16_t port = irq == 7 ? PIC1_CMD : PIC2_CMD;
        outb(port, 0x0B);
        if (!(inb(port) & 0x80)) {
            if (irq == 15) outb(PIC1_CMD, 0x20);
            return;
        }
    }
    if (irq >= 0 && irq < 16 && irq_handlers[irq]) irq_handlers[irq](r);
    pic_eoi(irq);
}

/* ---- PIT -------------------------------------------------------------------------- */
volatile uint32_t g_ticks;
static uint32_t pit_hz = 1000;
static uint32_t pit_acc;       /* fractional ms accumulator (for non-1000 Hz) */

static void pit_irq(regs_t *r) {
    (void)r;
    pit_acc += 1000;
    while (pit_acc >= pit_hz) { pit_acc -= pit_hz; g_ticks++; }
    speaker_tick();
}

void pit_init(uint32_t hz) {
    pit_hz = hz;
    uint32_t div = 1193182 / hz;
    outb(0x43, 0x36);
    outb(0x40, div & 0xFF);
    outb(0x40, (div >> 8) & 0xFF);
    irq_install(0, pit_irq);
}

uint32_t sys_uptime_ms(void) { return g_ticks; }

/* Busy wait using the PIT channel 0 counter is fiddly; use the tick counter
   when interrupts are on and an io_wait loop otherwise (~1us per port write). */
void sys_udelay(uint32_t us) {
    uint32_t f;
    __asm__ volatile("pushf; pop %0" : "=r"(f));
    if ((f & 0x200) && us >= 2000) {
        uint32_t end = g_ticks + (us + 999) / 1000;
        while ((int32_t)(g_ticks - end) < 0) __asm__ volatile("hlt");
        return;
    }
    while (us--) io_wait();
}

/* ---- FPU ---------------------------------------------------------------------------- */
void fpu_init(void) {
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1u << 2);   /* EM: no emulation          */
    cr0 |= (1u << 1);    /* MP: monitor coprocessor   */
    cr0 |= (1u << 5);    /* NE: native FPU exceptions */
    cr0 &= ~(1u << 3);   /* TS: clear task switched   */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile("fninit");
    /* All FPU exceptions masked, round to nearest, precision control = 53-bit
       (double): results match IEEE double arithmetic like on SSE machines. */
    uint16_t cw = 0x027F;
    __asm__ volatile("fldcw %0" : : "m"(cw));
}

/* ---- CPUID ------------------------------------------------------------------------------ */
char g_cpu_name[49];
char g_cpu_vendor[13];

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

void cpu_detect(void) {
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    memcpy(g_cpu_vendor, &b, 4);
    memcpy(g_cpu_vendor + 4, &d, 4);
    memcpy(g_cpu_vendor + 8, &c, 4);
    g_cpu_vendor[12] = 0;
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        uint32_t *p = (uint32_t *)g_cpu_name;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid(leaf, &a, &b, &c, &d);
            *p++ = a; *p++ = b; *p++ = c; *p++ = d;
        }
        g_cpu_name[48] = 0;
        /* trim leading spaces */
        char *s = g_cpu_name;
        while (*s == ' ') s++;
        if (s != g_cpu_name) memmove(g_cpu_name, s, strlen(s) + 1);
    } else {
        strlcpy(g_cpu_name, g_cpu_vendor, sizeof g_cpu_name);
    }
}

const char *sys_cpu_name(void) { return g_cpu_name; }
