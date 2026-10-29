/*
 * sysfiles.c - the system files on the drive (NovaOS 2.5).
 *
 * /system holds the kernel, the boot files and the UI files as real files:
 *
 *   /system/boot/kernel.bin     the kernel image - exactly what the installer
 *                               writes to a hard disk and the disk loader starts
 *                               (packed with NVLZ, fs/lz.h)
 *   /system/boot/mbr.bin        the boot loader: stage 1 (the MBR boot code) ...
 *   /system/boot/stage2.bin     ... and stage 2 (bootloader/mbr.asm, stage2.asm)
 *   /system/boot/boot.cfg       boot settings: how this start happened (loader,
 *                               kernel command line, display); rewritten at
 *                               every start
 *   /system/ui/...              fonts, themes, wallpapers, UI settings
 *                               (kernel/gui/uifiles.c), loaded at startup
 *
 * An ISO start - and the first start of an installed drive whose files were not
 * copied - writes them from the built-in copies in the kernel. An installed drive
 * keeps them with all other files (the installer copies them along), so there
 * they are restored at startup. If any of them is missing the drive is not
 * complete: "No drive found" (kernel/gui/nodrive.c).
 */
#include "kernel.h"
#include "disk/diskfmt.h"
#include "gui/bootscreen.h"
#include "fs/lz.h"

static const char *const boot_files[] = {
    "/system/boot/kernel.bin", "/system/boot/mbr.bin", "/system/boot/stage2.bin", "/system/boot/boot.cfg",
};

static void boot_cfg(void) {
    char buf[640];
    const char *cmd = sys_cmdline();
    const char *boot = sys_cmdline_get("boot");
    bool disk = boot && !strcmp(boot, "disk");
    snprintf(buf, sizeof buf,
             "# NovaOS boot settings - how NovaOS started this time (rewritten at every start).\n"
             "# The boot loader is /system/boot/mbr.bin + stage2.bin, the kernel /system/boot/kernel.bin.\n"
             "loader=%s\n"
             "medium=%s\n"
             "cmdline=%s\n"
             "display=%dx%dx%d\n",
             g_boot.loader[0] ? g_boot.loader : "unknown", disk ? "installed drive" : "ISO (CD)",
             cmd ? cmd : "", g_fb.width, g_fb.height, g_fb.bpp);
    void *old = vfs_read_file("/system/boot/boot.cfg", NULL);
    if (!old || strcmp(old, buf)) vfs_write_file("/system/boot/boot.cfg", buf, (uint32_t)strlen(buf));
    kfree(old);
}

void sysfiles_seed(void) {
    uint32_t t0 = g_ticks;
    vfs_mkdirs("/system/boot");
    uint32_t klen = 0, plen = 0;
    uint8_t *k = install_kernel_image(&klen);
    uint8_t *packed = k ? nvlz_pack(k, klen, &plen) : NULL;
    if (packed) vfs_write_file("/system/boot/kernel.bin", packed, plen);
    else if (k) vfs_write_file("/system/boot/kernel.bin", k, klen);
    kfree(packed);
    kfree(k);
    vfs_write_file("/system/boot/mbr.bin", bootblob_mbr, (uint32_t)(bootblob_mbr_end - bootblob_mbr));
    vfs_write_file("/system/boot/stage2.bin", bootblob_stage2, (uint32_t)(bootblob_stage2_end - bootblob_stage2));
    boot_cfg();
    uifiles_seed();
    klog("sysfiles: wrote the system files (kernel %u bytes, packed %u) in %u ms", klen, plen, g_ticks - t0);
}

void sysfiles_refresh(void) {
    if (vfs_exists("/system/boot/boot.cfg")) boot_cfg();
}

const char *sysfiles_missing(void) {
    static char path[VFS_PATH_MAX];
    for (int i = 0; i < ARRAY_SIZE(boot_files); i++)
        if (!vfs_exists(boot_files[i]) || vfs_is_dir(boot_files[i])) return boot_files[i];
    for (int i = 0; i < uifiles_count(); i++) {
        uifiles_path(i, path);
        if (!vfs_exists(path) || vfs_is_dir(path)) return path;
    }
    return NULL;
}
