/*
 * paging.c - identity-mapped paging, used only for protection:
 *   - page 0 (addresses 0..4095) is not mapped, so NULL pointer accesses fault
 *     immediately with a symbolised stack trace instead of silently corrupting
 *     memory;
 *   - kernel code and read-only data are write-protected (CR0.WP), catching
 *     writes to string literals / const tables;
 * Everything else (RAM, framebuffer, MMIO) stays identity mapped, 4 MiB pages
 * above the first 4 MiB.
 */
#include "kernel.h"

static uint32_t page_dir[1024] __attribute__((aligned(4096)));
static uint32_t low_pt[1024] __attribute__((aligned(4096)));
extern uint8_t __kernel_start[], __ro_end[];

#define PG_PRESENT 0x001
#define PG_RW      0x002
#define PG_PS      0x080

void paging_init(void) {
    uint32_t ro0 = (uint32_t)__kernel_start, ro1 = (uint32_t)__ro_end;
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t addr = i << 12;
        uint32_t flags = PG_PRESENT | PG_RW;
        if (addr >= ro0 && addr < ro1) flags = PG_PRESENT;
        low_pt[i] = addr | flags;
    }
    low_pt[0] = 0;   /* NULL page */
    page_dir[0] = (uint32_t)low_pt | PG_PRESENT | PG_RW;
    for (uint32_t i = 1; i < 1024; i++) page_dir[i] = (i << 22) | PG_PRESENT | PG_RW | PG_PS;

    uint32_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 1u << 4;                                   /* PSE: 4 MiB pages */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    __asm__ volatile("mov %0, %%cr3" : : "r"(page_dir) : "memory");
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1u << 31) | (1u << 16);                   /* PG + WP */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    klog("paging: on (NULL page unmapped, kernel text %p-%p read-only)", (void *)ro0, (void *)ro1);
}

/* ---- kernel stack overflow detection ------------------------------------------------- */
extern uint32_t kernel_stack_bottom[];
#define GUARD_WORDS 1024
#define GUARD_MAGIC 0x57AC6E44u

void stack_guard_init(void) {
    for (int i = 0; i < GUARD_WORDS; i++) kernel_stack_bottom[i] = GUARD_MAGIC;
}

void stack_guard_check(void) {
    if (kernel_stack_bottom[0] != GUARD_MAGIC || kernel_stack_bottom[GUARD_WORDS - 1] != GUARD_MAGIC)
        kpanic("Kernel stack overflow: something used more than 1 MiB of stack.\n"
               "Move large local arrays to the heap (malloc) and avoid deep recursion.");
}
