/* bootinfo.c - copy what we need out of the multiboot structures; command line. */
#include "kernel.h"

boot_info_t g_boot;

void multiboot_parse(uint32_t magic, uint32_t mbi_addr) {
    memset(&g_boot, 0, sizeof g_boot);
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC || !mbi_addr) {
        klog("boot: no multiboot info (magic=%08x)", magic);
        g_boot.mem_upper_kb = 64 * 1024;
        return;
    }
    const multiboot_info_t *mbi = (const multiboot_info_t *)mbi_addr;
    if (mbi->flags & (1 << 0)) {
        g_boot.mem_lower_kb = mbi->mem_lower;
        g_boot.mem_upper_kb = mbi->mem_upper;
    }
    if (mbi->flags & (1 << 1)) {
        g_boot.boot_device = mbi->boot_device;
        g_boot.has_boot_device = true;
    }
    if ((mbi->flags & (1 << 2)) && mbi->cmdline)
        strlcpy(g_boot.cmdline, (const char *)mbi->cmdline, sizeof g_boot.cmdline);
    if ((mbi->flags & (1 << 9)) && mbi->boot_loader_name)
        strlcpy(g_boot.loader, (const char *)mbi->boot_loader_name, sizeof g_boot.loader);
    if (mbi->flags & (1 << 6)) {
        uint32_t p = mbi->mmap_addr, end = mbi->mmap_addr + mbi->mmap_length;
        while (p < end && g_boot.mmap_count < 64) {
            const multiboot_mmap_entry_t *e = (const multiboot_mmap_entry_t *)p;
            g_boot.mmap[g_boot.mmap_count].addr = e->addr;
            g_boot.mmap[g_boot.mmap_count].len = e->len;
            g_boot.mmap[g_boot.mmap_count].type = e->type;
            g_boot.mmap_count++;
            p += e->size + 4;
        }
    }
    if (mbi->flags & (1 << 12)) {
        g_boot.fb_addr = mbi->framebuffer_addr;
        g_boot.fb_pitch = mbi->framebuffer_pitch;
        g_boot.fb_width = mbi->framebuffer_width;
        g_boot.fb_height = mbi->framebuffer_height;
        g_boot.fb_bpp = mbi->framebuffer_bpp;
        g_boot.fb_type = mbi->framebuffer_type;
        g_boot.red_pos = mbi->red_pos;
        g_boot.red_size = mbi->red_size;
        g_boot.green_pos = mbi->green_pos;
        g_boot.green_size = mbi->green_size;
        g_boot.blue_pos = mbi->blue_pos;
        g_boot.blue_size = mbi->blue_size;
        /* type 1 = direct RGB; type 2 = EGA text (no graphics) */
        g_boot.has_fb = g_boot.fb_type == 1 && g_boot.fb_addr && g_boot.fb_width >= 640 &&
                        (g_boot.fb_bpp == 32 || g_boot.fb_bpp == 24 || g_boot.fb_bpp == 16);
    }
}

/* ---- kernel command line: whitespace separated key=value tokens ---------------- */
#define MAX_ARGS 32
static struct { char key[32]; char val[160]; } args[MAX_ARGS];
static int nargs;

void cmdline_parse(const char *cmdline) {
    nargs = 0;
    const char *p = cmdline;
    bool first = true;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        int len = (int)(p - tok);
        /* GRUB / QEMU pass the kernel path as the first token: skip it */
        if (first && !memchr(tok, '=', (size_t)len) && (memchr(tok, '/', (size_t)len) || memchr(tok, '.', (size_t)len))) {
            first = false;
            continue;
        }
        first = false;
        if (nargs >= MAX_ARGS) break;
        const char *eq = memchr(tok, '=', (size_t)len);
        int klen = eq ? (int)(eq - tok) : len;
        if (klen >= (int)sizeof args[0].key) klen = sizeof args[0].key - 1;
        memcpy(args[nargs].key, tok, (size_t)klen);
        args[nargs].key[klen] = 0;
        if (eq) {
            int vlen = len - (int)(eq - tok) - 1;
            if (vlen >= (int)sizeof args[0].val) vlen = sizeof args[0].val - 1;
            memcpy(args[nargs].val, eq + 1, (size_t)vlen);
            args[nargs].val[vlen] = 0;
        } else {
            args[nargs].val[0] = 0;
        }
        nargs++;
    }
}

const char *sys_cmdline_get(const char *key) {
    for (int i = nargs - 1; i >= 0; i--)
        if (!strcmp(args[i].key, key)) return args[i].val;
    return NULL;
}

const char *sys_cmdline(void) { return g_boot.cmdline; }

int sys_boot_drive(void) { return g_boot.has_boot_device ? (int)(g_boot.boot_device >> 24) : -1; }
