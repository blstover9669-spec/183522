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
 *   /system/apps/NAME.app       one small text file per bundled app (every app
 *                               in the app registry: name, category, description,
 *                               file types)
 *
 * An ISO start writes them from the built-in copies in the kernel. An installed
 * drive starts without them (the installer writes only the boot loader and the
 * kernel): its first start - the installation menu, kernel/gui/firststart.c -
 * writes them part by part (sysfiles_seed_part) and saves them to the drive, and
 * from then on they are restored with all other files at startup.
 *
 * The system files are protected (kernel/fs/vfs.c): only NovaOS's privileged
 * path writes them, and every such write updates the manifest,
 * /system/manifest.sha256 - the SHA-256 of each system file (kernel/crypto),
 * ending with a line that holds the SHA-256 of the manifest itself. An installed
 * drive checks every file against it at startup (sysfiles_verify), and every
 * later load of a system file (vfs_read_file) is checked again: a damaged file -
 * or a missing one - stops NovaOS with a kernel panic that names it ("A system
 * file is damaged: /system/ui/fonts/ui.font"). A missing app file in
 * /system/apps is "System requirement fail" instead (kernel/gui/reqfail.c,
 * checked first): /system/apps missing, no app file in it or one bundled app's
 * file missing.
 */
#include "kernel.h"
#include <nova/app.h>
#include "disk/diskfmt.h"
#include "gui/bootscreen.h"
#include "fs/lz.h"
#include "crypto/crypto.h"

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
    if (!old || strcmp(old, buf)) {
        vfs_privileged(true);
        vfs_write_file("/system/boot/boot.cfg", buf, (uint32_t)strlen(buf));
        vfs_privileged(false);
    }
    kfree(old);
}

#define APPS_DIR "/system/apps"

/* /system/apps/<app name>.app ('/' in a name would become '-') */
static void app_file_path(const app_desc_t *a, char *out) {
    char name[VFS_NAME_MAX - 4];
    strlcpy(name, a->name, sizeof name);
    for (char *p = name; *p; p++)
        if (*p == '/') *p = '-';
    snprintf(out, VFS_PATH_MAX, APPS_DIR "/%s.app", name);
}

static void seed_apps(void) {
    vfs_mkdirs(APPS_DIR);
    char path[VFS_PATH_MAX], buf[512];
    for (int i = 0; i < app_count(); i++) {
        const app_desc_t *a = app_get(i);
        app_file_path(a, path);
        int n = snprintf(buf, sizeof buf,
                         "# NovaOS bundled app - NovaOS starts only if every bundled app has its file here.\n"
                         "name=%s\ncategory=%s\ndescription=%s\nopens=%s\n",
                         a->name, a->category ? a->category : "", a->description ? a->description : "",
                         a->file_types ? a->file_types : "");
        vfs_write_file(path, buf, (uint32_t)MIN(n, (int)sizeof buf - 1));
    }
}

const char *sysfiles_apps_missing(void) {
    static char what[VFS_PATH_MAX + 16];
    if (!vfs_is_dir(APPS_DIR)) return APPS_DIR " is missing";
    char path[VFS_PATH_MAX];
    int found = 0;
    what[0] = 0;
    for (int i = 0; i < app_count(); i++) {
        app_file_path(app_get(i), path);
        if (vfs_exists(path) && !vfs_is_dir(path)) found++;
        else if (!what[0]) snprintf(what, sizeof what, "%s is missing", path);
    }
    if (!found) return "0 apps found in " APPS_DIR;
    return what[0] ? what : NULL;
}

/* ---- the manifest: SHA-256 of every system file ------------------------------------------ */
typedef struct { char path[VFS_PATH_MAX]; uint8_t hash[32]; } mf_entry_t;
static mf_entry_t *mf;
static int mf_n, mf_cap;
static bool mf_dirty;

static int mf_find(const char *path) {
    for (int i = 0; i < mf_n; i++)
        if (!strcmp(mf[i].path, path)) return i;
    return -1;
}

static void hex32(const uint8_t *h, char *out) {
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = d[h[i] >> 4]; out[2 * i + 1] = d[h[i] & 15]; }
    out[64] = 0;
}

static int hexv(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static bool unhex32(const char *s, uint8_t *out) {
    for (int i = 0; i < 32; i++) {
        int a = hexv(s[2 * i]), b = a < 0 ? -1 : hexv(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return true;
}

static int mf_add(const char *path) {
    int i = mf_find(path);
    if (i >= 0) return i;
    if (mf_n == mf_cap) {
        int nc = mf_cap ? mf_cap * 2 : 64;
        mf_entry_t *n = krealloc(mf, (size_t)nc * sizeof *mf);
        if (!n) return -1;
        mf = n;
        mf_cap = nc;
    }
    strlcpy(mf[mf_n].path, path, sizeof mf[mf_n].path);
    return mf_n++;
}

/* NovaOS wrote a system file (vfs_write_file on the privileged path). */
void sysfiles_note_write(const char *path, const void *data, uint32_t len) {
    int i = mf_add(path);
    if (i < 0) return;
    sha256(data, len, mf[i].hash);
    mf_dirty = true;
}

/* The manifest's last line: "end <SHA-256 of everything before it>". */
static bool manifest_ok(const char *text, uint32_t len) {
    const char *end = NULL;
    for (uint32_t i = 0; i + 4 <= len; i++)
        if ((i == 0 || text[i - 1] == '\n') && !strncmp(text + i, "end ", 4)) end = text + i;
    uint8_t want[32], got[32];
    if (!end || (uint32_t)(text + len - end) < 4 + 64 || !unhex32(end + 4, want)) return false;
    sha256(text, (size_t)(end - text), got);
    return !memcmp(want, got, 32);
}

static void manifest_save(void) {
    size_t cap = 256 + (size_t)mf_n * (VFS_PATH_MAX + 70);
    char *buf = kmalloc(cap);
    if (!buf) return;
    int n = snprintf(buf, cap, "# NovaOS system files - the SHA-256 of each, written by NovaOS whenever it writes them.\n"
                               "# A system file that does not match stops NovaOS (kernel panic).\n");
    char h[65];
    for (int i = 0; i < mf_n; i++) {
        hex32(mf[i].hash, h);
        n += snprintf(buf + n, cap - (size_t)n, "%s  %s\n", h, mf[i].path);
    }
    uint8_t self[32];
    sha256(buf, (size_t)n, self);
    hex32(self, h);
    n += snprintf(buf + n, cap - (size_t)n, "end %s\n", h);
    vfs_privileged(true);
    vfs_write_file(SYSFILES_MANIFEST, buf, (uint32_t)n);
    vfs_privileged(false);
    kfree(buf);
    mf_dirty = false;
}

static void damaged(const char *path, bool missing) NORETURN;
static void damaged(const char *path, bool missing) {
    klog("sysfiles: %s system file: %s", missing ? "missing" : "damaged", path);
    kpanic("A system file is %s: %s", missing ? "missing" : "damaged", path);
    for (;;) {}
}

/* A system file was loaded (vfs_read_file): it must match the manifest. */
void sysfiles_check_loaded(const char *path, const void *data, uint32_t len) {
    if (!strcmp(path, SYSFILES_MANIFEST)) {
        if (!manifest_ok(data, len)) damaged(path, false);
        return;
    }
    int i = mf_find(path);
    if (i < 0) return;                       /* not in the manifest: nothing to compare */
    uint8_t h[32];
    sha256(data, len, h);
    if (memcmp(h, mf[i].hash, 32)) damaged(path, false);
}

/* Read the manifest of an installed drive into memory. */
static bool manifest_load(void) {
    uint32_t len = 0;
    char *text = vfs_read_file(SYSFILES_MANIFEST, &len);       /* checked: damaged -> panic */
    if (!text) return false;
    mf_n = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == '#' || !strncmp(line, "end ", 4)) continue;
        uint8_t h[32];
        if (strlen(line) < 67 || !unhex32(line, h) || line[64] != ' ' || line[65] != ' ')
            damaged(SYSFILES_MANIFEST, false);
        int i = mf_add(line + 66);
        if (i >= 0) memcpy(mf[i].hash, h, 32);
    }
    kfree(text);
    mf_dirty = false;
    return true;
}

static void hash_tree(const char *dir) {
    vfs_dirent_t *e = kmalloc(128 * sizeof *e);
    if (!e) return;
    int n = vfs_list(dir, e, 128);
    for (int i = 0; i < n && i < 128; i++) {
        char p[VFS_PATH_MAX];
        vfs_join(dir, e[i].name, p);
        if (e[i].is_dir) { hash_tree(p); continue; }
        uint32_t len = 0;
        void *d = vfs_read_file(p, &len);
        if (d) sysfiles_note_write(p, d, len);
        kfree(d);
    }
    kfree(e);
}

void sysfiles_verify(void) {
    uint32_t t0 = g_ticks;
    if (!manifest_load()) {
        /* a drive from before the manifest: make one from its system files as they are */
        klog("sysfiles: no manifest yet - making one from the system files on the drive");
        mf_n = 0;
        hash_tree("/system/boot");
        hash_tree("/system/ui");
        hash_tree("/system/apps");
        manifest_save();
        return;
    }
    for (int i = 0; i < mf_n; i++) {
        uint32_t len = 0;
        void *d = vfs_read_file(mf[i].path, &len);           /* checked when it is loaded */
        if (!d) {
            if (!strncmp(mf[i].path, "/system/apps/", 13)) reqfail_show("A required bundle app is missing", mf[i].path);
            damaged(mf[i].path, true);
        }
        kfree(d);
    }
    klog("sysfiles: %d system files match the manifest (SHA-256) in %u ms", mf_n, g_ticks - t0);
}

static uint32_t seeded_klen, seeded_plen;

void sysfiles_seed_part(int part) {
    vfs_privileged(true);
    switch (part) {
    case SYSFILES_BOOT: {
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
        seeded_klen = klen;
        seeded_plen = plen;
        break;
    }
    case SYSFILES_UI:
        uifiles_seed();
        break;
    case SYSFILES_APPS:
        seed_apps();
        break;
    default:
        break;
    }
    vfs_privileged(false);
    if (mf_dirty) manifest_save();
}

void sysfiles_seed(void) {
    uint32_t t0 = g_ticks;
    for (int i = 0; i < SYSFILES_PARTS; i++) sysfiles_seed_part(i);
    klog("sysfiles: wrote the system files (kernel %u bytes, packed %u, %d app files) in %u ms", seeded_klen,
         seeded_plen, app_count(), g_ticks - t0);
}

void sysfiles_refresh(void) {
    if (vfs_exists("/system/boot/boot.cfg")) boot_cfg();
    if (mf_dirty) manifest_save();
}
