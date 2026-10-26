/* panic.c - kernel panic "blue screen", drawn straight to the framebuffer. */
#include "kernel.h"
#include "gfx/font_internal.h"

/* symbol table generated at link time (tools/gensyms.py) */
typedef struct { uint32_t addr; uint32_t name; } ksym_t;
extern const uint32_t ksyms_count;
extern const ksym_t ksyms_table[];
extern const char ksyms_names[];

/* Format addr as "function+0xoff" if the symbol table knows it. */
static void symbolize(uint32_t addr, char *out, size_t n) {
    int lo = 0, hi = (int)ksyms_count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ksyms_table[mid].addr <= addr) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best >= 0 && addr - ksyms_table[best].addr < 0x10000)
        snprintf(out, n, "%08x  %s+0x%x", addr, ksyms_names + ksyms_table[best].name, addr - ksyms_table[best].addr);
    else
        snprintf(out, n, "%08x", addr);
}

void panic_symbolize(uint32_t addr, char *out, size_t n) { symbolize(addr, out, n); }

static int pan_x, pan_y;
static bool in_panic;

static void pput(int x, int y, uint32_t c) {
    if (!g_fb.addr || x < 0 || y < 0 || x >= g_fb.width || y >= g_fb.height) return;
    uint8_t *p = g_fb.addr + (uint32_t)y * g_fb.pitch;
    uint8_t r = COLOR_R(c), g = COLOR_G(c), b = COLOR_B(c);
    if (g_fb.bpp == 32) {
        ((uint32_t *)p)[x] = ((uint32_t)r << g_fb.r_pos) | ((uint32_t)g << g_fb.g_pos) | ((uint32_t)b << g_fb.b_pos);
    } else if (g_fb.bpp == 24) {
        p += x * 3;
        p[g_fb.r_pos / 8] = r; p[g_fb.g_pos / 8] = g; p[g_fb.b_pos / 8] = b;
    } else if (g_fb.bpp == 16) {
        ((uint16_t *)p)[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

static void pfill(uint32_t c) {
    for (int y = 0; y < g_fb.height; y++)
        for (int x = 0; x < g_fb.width; x++) pput(x, y, c);
}

static void pchar(const struct font *f, unsigned char ch, uint32_t fg, uint32_t bg) {
    const glyph_t *g = font_glyph(f, ch);
    for (int y = 0; y < g->h; y++)
        for (int x = 0; x < g->w; x++) {
            int a = f->data[g->off + (uint32_t)(y * g->w + x)];
            if (a) pput(pan_x + g->xoff + x, pan_y + g->yoff + y, color_mix(bg, fg, a));
        }
    pan_x += f->mono ? f->mono : g->adv;
}

static void ptext(const struct font *f, const char *s, uint32_t fg, uint32_t bg) {
    int x0 = pan_x;
    for (; *s; s++) {
        if (*s == '\n') { pan_x = x0; pan_y += f->height; continue; }
        pchar(f, (unsigned char)*s, fg, bg);
    }
}

#define PBG RGB(16, 52, 120)
#define PFG RGB(240, 244, 255)

static void panic_screen(const char *msg, regs_t *r) {
    if (!g_fb.addr) return;
    pfill(PBG);
    int left = g_fb.width > 900 ? 120 : 40;
    pan_x = left; pan_y = 70;
    ptext(&font_ui_huge, ":(", PFG, PBG);
    pan_x = left; pan_y = 160;
    ptext(&font_ui_large, "NovaOS ran into a problem and had to stop.", PFG, PBG);
    pan_x = left; pan_y = 200;
    ptext(&font_mono, msg, RGB(255, 220, 120), PBG);
    pan_y += 32;
    char buf[160];
    if (r) {
        uint32_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        snprintf(buf, sizeof buf, "EAX=%08x  EBX=%08x  ECX=%08x  EDX=%08x", r->eax, r->ebx, r->ecx, r->edx);
        pan_x = left; ptext(&font_mono, buf, PFG, PBG); pan_y += 16;
        snprintf(buf, sizeof buf, "ESI=%08x  EDI=%08x  EBP=%08x  ESP=%08x", r->esi, r->edi, r->ebp, r->esp_dummy + 20);
        pan_x = left; ptext(&font_mono, buf, PFG, PBG); pan_y += 16;
        snprintf(buf, sizeof buf, "EIP=%08x  CS=%04x  EFLAGS=%08x  ERR=%08x  CR2=%08x", r->eip, r->cs, r->eflags, r->err_code, cr2);
        pan_x = left; ptext(&font_mono, buf, PFG, PBG); pan_y += 24;
    }
    pan_x = left;
    ptext(&font_mono, "Stack trace:", PFG, PBG);
    pan_y += 16;
    uint32_t *ebp = r ? (uint32_t *)r->ebp : (uint32_t *)__builtin_frame_address(0);
    char sym[96];
    if (r) {
        symbolize(r->eip, sym, sizeof sym);
        snprintf(buf, sizeof buf, "  %s", sym);
        pan_x = left; ptext(&font_mono, buf, PFG, PBG); pan_y += 16;
    }
    for (int i = 0; i < 12 && ebp && (uint32_t)ebp > 0x100000 && (uint32_t)ebp < 0x10000000; i++) {
        uint32_t ret = ebp[1];
        if (!ret) break;
        symbolize(ret, sym, sizeof sym);
        snprintf(buf, sizeof buf, "  %s", sym);
        pan_x = left; ptext(&font_mono, buf, PFG, PBG); pan_y += 16;
        ebp = (uint32_t *)ebp[0];
    }
    pan_y += 16;
    pan_x = left;
    ptext(&font_ui, "Press any key to restart.", PFG, PBG);
}

static void serial_trace(regs_t *r) {
    uint32_t *ebp = r ? (uint32_t *)r->ebp : (uint32_t *)__builtin_frame_address(0);
    char buf[128], sym[96];
    serial_write("Stack trace:\n");
    if (r) { symbolize(r->eip, sym, sizeof sym); snprintf(buf, sizeof buf, "  %s\n", sym); serial_write(buf); }
    for (int i = 0; i < 16 && ebp && (uint32_t)ebp > 0x100000 && (uint32_t)ebp < 0x10000000; i++) {
        if (!ebp[1]) break;
        symbolize(ebp[1], sym, sizeof sym);
        snprintf(buf, sizeof buf, "  %s\n", sym);
        serial_write(buf);
        ebp = (uint32_t *)ebp[0];
    }
}

static void NORETURN panic_wait_reboot(void) {
    /* drain then wait for a key press by polling the controller */
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) inb(0x60);
    for (;;) {
        if (inb(0x64) & 1) {
            uint8_t st = inb(0x64);
            uint8_t sc = inb(0x60);
            if (!(st & 0x20) && !(sc & 0x80)) sys_reboot();
        }
        io_wait();
    }
}

static void do_panic(const char *msg, regs_t *r) {
    __asm__ volatile("cli");
    if (in_panic) {
        serial_write("\n*** double panic ***\n");
        for (;;) __asm__ volatile("hlt");
    }
    in_panic = true;
    serial_write("\n\n*** KERNEL PANIC ***\n");
    serial_write(msg);
    serial_write("\n");
    if (r) {
        char buf[200];
        uint32_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        snprintf(buf, sizeof buf,
                 "EAX=%08x EBX=%08x ECX=%08x EDX=%08x\nESI=%08x EDI=%08x EBP=%08x\n"
                 "EIP=%08x CS=%04x EFLAGS=%08x ERR=%08x CR2=%08x\n",
                 r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp,
                 r->eip, r->cs, r->eflags, r->err_code, cr2);
        serial_write(buf);
    }
    serial_trace(r);
    panic_screen(msg, r);
    panic_wait_reboot();
}

void kpanic(const char *fmt, ...) {
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    do_panic(msg, NULL);
    for (;;) {}
}

void panic_exception(regs_t *r, const char *name) {
    char msg[200];
    if (r->int_no == 14) {
        uint32_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        const char *what = cr2 < 4096 ? "NULL pointer dereference"
                         : (r->err_code & 1) && (r->err_code & 2) ? "write to read-only memory"
                         : "invalid memory access";
        snprintf(msg, sizeof msg, "Page fault: %s (%s address %08x) at EIP=%08x", what,
                 (r->err_code & 2) ? "writing" : "reading", cr2, r->eip);
    } else {
        snprintf(msg, sizeof msg, "CPU exception %u: %s at EIP=%08x", r->int_no, name, r->eip);
    }
    do_panic(msg, r);
    for (;;) {}
}
