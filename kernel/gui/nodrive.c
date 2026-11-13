/*
 * nodrive.c - "No drive found" (NovaOS 2.5).
 *
 * Shown only when the drive NovaOS was installed on is really not there:
 *
 *   at startup   an installed system (boot=disk) whose drive is not found on
 *                the IDE controllers (detached, or moved to a SATA/AHCI, SCSI
 *                or NVMe controller NovaOS has no driver for) -
 *                reqfail_check_drive() (kernel/gui/reqfail.c) sends it here;
 *   after start  the drive stops responding: every few seconds NovaOS reads it
 *                (persist_probe, kernel/disk/persist.c - also counting failed
 *                saves); after WATCH_FAILS failures in a row (errors or time-outs)
 *                the drive is gone.
 *
 * A drive that is there but can't be read at startup, or is smaller than 4 MB, is
 * "System requirement fail" (reqfail.c). ISO starts never show it, and deleting
 * folders doesn't either: /system, /home and /home/user are protected
 * (kernel/fs/vfs.c), and damaged system files are a kernel panic
 * (kernel/fs/sysfiles.c).
 *
 * The screen: black, the Nova icon in the middle and "No drive found" under it
 * (Liberation Sans, rendered at build time by tools/genfont.py --nodrive into
 * nodrive_font.c); it stays there, only resetting the computer gets out. It
 * needs nothing from the drive: everything is built into the kernel.
 * "System requirement fail" is the same screen with its reason under the title
 * (bootfail_screen).
 */
#include "wm_internal.h"
#include "bootscreen.h"
#include <nova/disk.h>

#define WATCH_MS    3000
#define WATCH_FAILS 2

static int fails;

void bootfail_screen(const char *title, const char *reason) {
    __asm__ volatile("sti");
    if (g_fb.addr && g_fb.width > 0 && g_fb.height > 0) {
        surface_t *s = surface_create(g_fb.width, g_fb.height);
        if (s) {
            gfx_fill_rect(s, 0, 0, s->w, s->h, RGB(0, 0, 0));
            int size = CLAMP(s->h / 6, 64, 200);
            int th = font_height(&font_nodrive), gap = size / 4;
            int rh = reason ? font_height(&font_nodrive_small) : 0, rgap = reason ? th / 3 : 0;
            int y = (s->h - (size + gap + th + rgap + rh)) / 2;
            ui_draw_logo(s, (s->w - size) / 2, y, size);
            y += size + gap;
            gfx_text_aligned(s, rect_make(0, y, s->w, th), title, RGB(236, 238, 242), &font_nodrive,
                             ALIGN_CENTER | ALIGN_MIDDLE);
            if (reason)
                gfx_text_aligned(s, rect_make(0, y + th + rgap, s->w, rh), reason, RGB(168, 174, 188),
                                 &font_nodrive_small, ALIGN_CENTER | ALIGN_MIDDLE);
            fb_present(s, rect_make(0, 0, s->w, s->h));
            surface_destroy(s);
        }
    }
    /* stay here: keys and clicks do nothing, only a reset of the computer gets out */
    for (;;) {
        input_event_t ev;
        while (input_pop(&ev)) {}
        __asm__ volatile("sti; hlt");
    }
}

void nodrive_show(const char *why) {
    klog("nodrive: No drive found (%s)", why ? why : "the drive is not there");
    __asm__ volatile("sti");
    bootfail_screen("No drive found", NULL);
}

static void watch(void *ctx) {
    (void)ctx;
    int r = persist_probe();
    if (r == DISK_OK) { fails = 0; return; }
    fails++;
    klog("nodrive: the drive did not answer (%s), %d of %d", disk_strerror(r), fails, WATCH_FAILS);
    if (fails >= WATCH_FAILS) {
        char why[96];
        snprintf(why, sizeof why, "the drive stopped responding: %s", disk_strerror(r));
        nodrive_show(why);
    }
}

void nodrive_check_startup(void) {
    if (!persist_active()) return;                  /* ISO start: nothing to watch */
    fails = 0;
    timer_add(WATCH_MS, watch, NULL, true);
    klog("nodrive: watching the drive (a read every %u s)", WATCH_MS / 1000);
}
