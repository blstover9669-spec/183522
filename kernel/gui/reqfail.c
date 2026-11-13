/*
 * reqfail.c - "System requirement fail" (NovaOS 2.5).
 *
 * The computer started, but NovaOS cannot boot on it. The screen looks like
 * "No drive found" (nodrive.c): black, the Nova icon in the middle, then
 * "System requirement fail" and the reason under it in Liberation Sans. It stays
 * there; only resetting the computer gets out. Both checks run at startup
 * (kernel/core/kmain.c):
 *
 *   "Requires a readable COS:// drive (at least 4 MB)"
 *       COS:// is the drive NovaOS starts from. On an installed-drive boot
 *       (boot=disk) the drive NovaOS was installed on must be read at startup
 *       without errors (kernel/disk/persist.c), and the disk must have 4 MB at
 *       least. When the drive is not found at all (on the IDE controllers -
 *       NovaOS has no SATA/AHCI, SCSI or NVMe driver) that is "No drive found"
 *       instead (nodrive.c). ISO boots start from the CD and pass.
 *   "A required bundle app is missing"
 *       /system/apps holds one small file per bundled app (kernel/fs/sysfiles.c).
 *       The boot fails when /system/apps is missing, no app file is found in it
 *       or the file of a bundled app is missing. It is checked after the drive
 *       check, so with /system itself gone "No drive found" comes first.
 *
 * Each case logs "reqfail: System requirement fail - REASON (DETAIL)"; the
 * checks that pass log "reqfail: COS:// ..." and "reqfail: N bundle apps ...".
 */
#include "kernel.h"
#include "bootscreen.h"
#include <nova/disk.h>
#include <nova/app.h>

#define COS_MIN_BYTES (4u * 1024 * 1024)

static const char R_COS[] = "Requires a readable COS:// drive (at least 4 MB)";
static const char R_APPS[] = "A required bundle app is missing";

void reqfail_show(const char *reason, const char *detail) {
    klog("reqfail: System requirement fail - %s (%s)", reason, detail ? detail : "");
    bootfail_screen("System requirement fail", reason);
}

void reqfail_check_drive(void) {
    const char *boot = sys_cmdline_get("boot");
    if (!boot || strcmp(boot, "disk")) {
        klog("reqfail: COS:// is the CD (ISO boot)");
        return;
    }
    char detail[128], size[24];
    persist_info_t pi;
    persist_get_info(&pi);
    int st = persist_boot_status();
    const disk_info_t *d = pi.active ? disk_get(pi.disk) : NULL;
    if (st == DISK_ENODEV || !d) {
        const char *ids = sys_cmdline_get("diskid");
        snprintf(detail, sizeof detail, "the installed drive (id %s) was not found on the IDE controllers (%d hard disk%s)",
                 ids ? ids : "?", disk_count(), disk_count() == 1 ? "" : "s");
        nodrive_show(detail);
    }
    if (st != DISK_OK) {
        snprintf(detail, sizeof detail, "reading disk %d (%s) failed: %s", pi.disk, d->model, disk_strerror(st));
        reqfail_show(R_COS, detail);
    }
    disk_format_size(d->sectors * 512, size, sizeof size);
    if (d->sectors * 512 < COS_MIN_BYTES) {
        snprintf(detail, sizeof detail, "disk %d (%s) has only %s", pi.disk, d->model, size);
        reqfail_show(R_COS, detail);
    }
    klog("reqfail: COS:// drive ok (disk %d, %s, %s)", pi.disk, d->model, size);
}

void reqfail_check_apps(void) {
    const char *missing = sysfiles_apps_missing();
    if (missing) reqfail_show(R_APPS, missing);
    klog("reqfail: %d bundle apps found in /system/apps", app_count());
}
