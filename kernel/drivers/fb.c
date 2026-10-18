/*
 * fb.c - linear framebuffer setup and presentation.
 *
 * Preferred: the VBE framebuffer GRUB set up for us (multiboot info).
 * Fallback: program the Bochs/QEMU/VirtualBox "BGA" (VBE DISPI) interface via
 * ports 0x1CE/0x1CF and find the LFB from the PCI display device's BAR.
 */
#include "kernel.h"

framebuffer_t g_fb;

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF
#define VBE_DISPI_INDEX_ID     0
#define VBE_DISPI_INDEX_XRES   1
#define VBE_DISPI_INDEX_YRES   2
#define VBE_DISPI_INDEX_BPP    3
#define VBE_DISPI_INDEX_ENABLE 4
#define VBE_DISPI_INDEX_BANK   5
#define VBE_DISPI_INDEX_VIRT_WIDTH  6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7
#define VBE_DISPI_INDEX_X_OFFSET 8
#define VBE_DISPI_INDEX_Y_OFFSET 9
#define VBE_DISPI_ENABLED      0x01
#define VBE_DISPI_LFB_ENABLED  0x40

static void bga_write(uint16_t idx, uint16_t v) {
    outw(VBE_DISPI_IOPORT_INDEX, idx);
    outw(VBE_DISPI_IOPORT_DATA, v);
}

static uint16_t bga_read(uint16_t idx) {
    outw(VBE_DISPI_IOPORT_INDEX, idx);
    return inw(VBE_DISPI_IOPORT_DATA);
}

bool bga_available(void) {
    uint16_t id = bga_read(VBE_DISPI_INDEX_ID);
    return id >= 0xB0C0 && id <= 0xB0CF;
}

bool bga_set_mode(int w, int h, int bpp) {
    bga_write(VBE_DISPI_INDEX_ENABLE, 0);
    bga_write(VBE_DISPI_INDEX_XRES, (uint16_t)w);
    bga_write(VBE_DISPI_INDEX_YRES, (uint16_t)h);
    bga_write(VBE_DISPI_INDEX_BPP, (uint16_t)bpp);
    bga_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)w);
    bga_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    bga_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
    return bga_read(VBE_DISPI_INDEX_XRES) == w && bga_read(VBE_DISPI_INDEX_YRES) == h &&
           bga_read(VBE_DISPI_INDEX_BPP) == bpp;
}

/* Find the linear framebuffer address from the PCI display adapter. */
static uint32_t find_lfb(void) {
    const pci_device_t *d = pci_find_class(0x03, 0x00);
    if (!d) d = pci_find_class(0x03, 0x80);
    if (d) {
        pci_enable(d);
        for (int i = 0; i < 6; i++) {
            uint32_t bar = d->bar[i];
            if (bar && !(bar & 1)) {
                /* memory BAR: check its size, the LFB is the big one (>= 4 MiB) */
                uint8_t off = (uint8_t)(0x10 + i * 4);
                pci_write32(d->bus, d->slot, d->func, off, 0xFFFFFFFF);
                uint32_t sz = pci_read32(d->bus, d->slot, d->func, off);
                pci_write32(d->bus, d->slot, d->func, off, bar);
                sz = ~(sz & ~0xFu) + 1;
                if (sz >= 4 * 1024 * 1024) return bar & ~0xFu;
            }
        }
    }
    return 0xFD000000u;   /* QEMU default */
}

bool fb_init(int want_w, int want_h) {
    memset(&g_fb, 0, sizeof g_fb);
    bool have_mb = g_boot.has_fb;
    bool bga = bga_available();
    bool want_specific = want_w > 0 && want_h > 0;
    if (g_boot.fb_addr)
        klog("fb: bootloader framebuffer %ux%ux%u type %u pitch %u at %p%s", g_boot.fb_width, g_boot.fb_height,
             g_boot.fb_bpp, g_boot.fb_type, g_boot.fb_pitch, (void *)(uintptr_t)g_boot.fb_addr, have_mb ? "" : " (unusable)");

    if (have_mb && !(want_specific && bga && ((uint32_t)want_w != g_boot.fb_width || (uint32_t)want_h != g_boot.fb_height))) {
        g_fb.addr = (uint8_t *)(uintptr_t)g_boot.fb_addr;
        g_fb.pitch = g_boot.fb_pitch;
        g_fb.width = (int)g_boot.fb_width;
        g_fb.height = (int)g_boot.fb_height;
        g_fb.bpp = g_boot.fb_bpp;
        g_fb.r_pos = g_boot.red_pos;
        g_fb.g_pos = g_boot.green_pos;
        g_fb.b_pos = g_boot.blue_pos;
        if (g_fb.bpp != 16 && g_boot.red_size == 0) { g_fb.r_pos = 16; g_fb.g_pos = 8; g_fb.b_pos = 0; }
        g_fb.source = "multiboot";
    } else if (bga) {
        int w = want_specific ? want_w : 1024, h = want_specific ? want_h : 768;
        if (!bga_set_mode(w, h, 32)) {
            w = 1024; h = 768;
            if (!bga_set_mode(w, h, 32)) { w = 800; h = 600; bga_set_mode(w, h, 32); }
        }
        g_fb.addr = (uint8_t *)(uintptr_t)(have_mb ? (uint32_t)g_boot.fb_addr : find_lfb());
        g_fb.width = w;
        g_fb.height = h;
        g_fb.bpp = 32;
        g_fb.pitch = (uint32_t)w * 4;
        g_fb.r_pos = 16; g_fb.g_pos = 8; g_fb.b_pos = 0;
        g_fb.source = "bga";
    } else {
        klog("fb: no usable framebuffer (no multiboot fb, no BGA)");
        return false;
    }
    klog("fb: %dx%dx%d pitch %u at %p via %s", g_fb.width, g_fb.height, g_fb.bpp,
         g_fb.pitch, (void *)g_fb.addr, g_fb.source);
    return true;
}

/* Copy rectangle r of the back buffer (same size as the screen) to the LFB. */
void fb_present(const surface_t *src, rect_t r) {
    r = rect_intersect(r, rect_make(0, 0, g_fb.width, g_fb.height));
    r = rect_intersect(r, rect_make(0, 0, src->w, src->h));
    if (rect_empty(r)) return;
    if (g_fb.bpp == 32 && g_fb.r_pos == 16 && g_fb.g_pos == 8 && g_fb.b_pos == 0) {
        for (int y = r.y; y < r.y + r.h; y++) {
            memcpy(g_fb.addr + (uint32_t)y * g_fb.pitch + (uint32_t)r.x * 4,
                   src->pixels + (size_t)y * src->pitch + r.x, (size_t)r.w * 4);
        }
    } else if (g_fb.bpp == 32) {
        for (int y = r.y; y < r.y + r.h; y++) {
            uint32_t *d = (uint32_t *)(g_fb.addr + (uint32_t)y * g_fb.pitch) + r.x;
            const uint32_t *s = src->pixels + (size_t)y * src->pitch + r.x;
            for (int x = 0; x < r.w; x++) {
                uint32_t c = s[x];
                d[x] = (COLOR_R(c) << g_fb.r_pos) | (COLOR_G(c) << g_fb.g_pos) | (COLOR_B(c) << g_fb.b_pos);
            }
        }
    } else if (g_fb.bpp == 24) {
        int ri = g_fb.r_pos / 8, gi = g_fb.g_pos / 8, bi = g_fb.b_pos / 8;
        for (int y = r.y; y < r.y + r.h; y++) {
            uint8_t *d = g_fb.addr + (uint32_t)y * g_fb.pitch + (uint32_t)r.x * 3;
            const uint32_t *s = src->pixels + (size_t)y * src->pitch + r.x;
            for (int x = 0; x < r.w; x++, d += 3) {
                uint32_t c = s[x];
                d[ri] = (uint8_t)COLOR_R(c); d[gi] = (uint8_t)COLOR_G(c); d[bi] = (uint8_t)COLOR_B(c);
            }
        }
    } else if (g_fb.bpp == 16) {
        for (int y = r.y; y < r.y + r.h; y++) {
            uint16_t *d = (uint16_t *)(g_fb.addr + (uint32_t)y * g_fb.pitch) + r.x;
            const uint32_t *s = src->pixels + (size_t)y * src->pitch + r.x;
            for (int x = 0; x < r.w; x++) {
                uint32_t c = s[x];
                d[x] = (uint16_t)(((COLOR_R(c) >> 3) << 11) | ((COLOR_G(c) >> 2) << 5) | (COLOR_B(c) >> 3));
            }
        }
    }
}
