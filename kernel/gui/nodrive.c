/*
 * nodrive.c - "No drive found" (NovaOS 2.5).
 *
 * The drive is the top folder that holds System, home and the rest (the root
 * of the file system). When it is gone - deleted by a virus, rm -r / ... - or
 * the system folder or one of the system files in it (kernel/fs/sysfiles.c) is
 * missing, NovaOS shows a black screen with the Nova icon in the middle and
 * "No drive found" under it (Liberation Sans, rendered at build time by
 * tools/genfont.py --nodrive into nodrive_font.c) and stays there; only
 * resetting the computer gets out of it.
 *
 * It is checked at startup (after the saved files of an installed drive are
 * restored, before anything else uses them) and, while NovaOS runs, every time
 * the files change (polled from a timer). On an installed drive the files are
 * saved first, like any other change: the drive stays gone after a restart.
 *
 * Everything this screen needs is built into the kernel: it never reads the
 * drive it is reporting as missing.
 */
#include "wm_internal.h"
#include "bootscreen.h"
#include <nova/disk.h>

#define WATCH_MS 250

static bool watching;          /* NovaOS is running: the startup check passed */
static uint32_t watched_gen;

/* NULL when the drive is there, else what is missing. */
static const char *drive_missing(void) {
    if (!vfs_is_dir("/system")) return "/system";
    if (!vfs_is_dir("/home")) return "/home";
    return sysfiles_missing();
}

void nodrive_show(const char *why) {
    klog("nodrive: No drive found (%s is missing)", why ? why : "the drive");
    __asm__ volatile("sti");
    /* an installed drive keeps what happened, like every other change to the files
       (at startup the files are as they were saved: nothing to save) */
    if (watching && persist_active()) persist_save();
    if (g_fb.addr && g_fb.width > 0 && g_fb.height > 0) {
        surface_t *s = surface_create(g_fb.width, g_fb.height);
        if (s) {
            gfx_fill_rect(s, 0, 0, s->w, s->h, RGB(0, 0, 0));
            int size = CLAMP(s->h / 6, 64, 200);
            int th = font_height(&font_nodrive), gap = size / 4;
            int y = (s->h - (size + gap + th)) / 2;
            ui_draw_logo(s, (s->w - size) / 2, y, size);
            gfx_text_aligned(s, rect_make(0, y + size + gap, s->w, th), "No drive found", RGB(236, 238, 242),
                             &font_nodrive, ALIGN_CENTER | ALIGN_MIDDLE);
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

static void watch(void *ctx) {
    (void)ctx;
    uint32_t g = vfs_generation();
    if (g == watched_gen) return;
    watched_gen = g;
    const char *m = drive_missing();
    if (m) nodrive_show(m);
}

void nodrive_check_startup(void) {
    const char *m = drive_missing();
    if (m) nodrive_show(m);
    watched_gen = vfs_generation();
    watching = true;
    timer_add(WATCH_MS, watch, NULL, true);
}
