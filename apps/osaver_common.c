/*
 * osaver_common.c - OSaver helpers shared by the app, the restore and the boot
 * prompt: which files belong in a code, fingerprints of a fresh system's files
 * (so unchanged boot defaults are not saved), the Start button colour setting,
 * the version string, the alert sound and the splash-style drawing.
 */
#include "osaver_internal.h"
#include <nova/disk.h>

/* ======================================================================================
 * What gets saved
 * ==================================================================================== */
static bool under(const char *path, const char *dir) {
    size_t n = strlen(dir);
    return !strncmp(path, dir, n) && (path[n] == 0 || path[n] == '/');
}

bool osv_path_saved(const char *path, bool is_dir) {
    if (!path || path[0] != '/') return false;
    if (!strcmp(path, "/")) return is_dir;
    if (under(path, "/tmp")) return false;
    if (under(path, "/system")) {
        /* the kernel, boot files, UI files, logs: part of NovaOS itself, never in a code */
        if (!strcmp(path, "/system")) return is_dir;
        if (!under(path, "/system/config")) return false;
        if (!strcmp(path, OSV_DEFAULTS)) return false;
        return true;
    }
    if (!strcmp(path, OSV_COPY_FILE)) return false;
    return true;
}

bool osv_name_ok(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (n == 0 || n >= VFS_NAME_MAX) return false;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (size_t i = 0; i < n; i++)
        if (name[i] == '/' || (unsigned char)name[i] < 0x20) return false;
    return true;
}

void osv_split_name(const char *full, char *name, int ncap, char *type, int tcap) {
    const char *dot = strrchr(full, '.');
    if (dot && dot != full && dot[1]) {
        int nl = (int)(dot - full);
        if (nl >= ncap) nl = ncap - 1;
        memcpy(name, full, (size_t)nl);
        name[nl] = 0;
        strlcpy(type, dot + 1, (size_t)tcap);
    } else {
        strlcpy(name, full, (size_t)ncap);
        type[0] = 0;
    }
}

/* ======================================================================================
 * Fingerprints of a fresh system ("boot defaults")
 *
 * On an ISO boot the files right after start are exactly what every fresh NovaOS has,
 * so they are fingerprinted (path, size, CRC-32) at INIT_FS and written to
 * OSV_DEFAULTS. A drive installed from the ISO carries that file along; on installed
 * drives it is read back. Files that still match are left out of codes, which keeps
 * codes short (a code with a changed setting and a note is a few hundred characters).
 * ==================================================================================== */
typedef struct { uint32_t pcrc; uint16_t plen; uint8_t is_dir, pad; uint32_t size, dcrc; } fp_t;

static fp_t    *g_fp;
static uint32_t g_nfp, g_capfp;
static uint32_t g_boot_id, g_boot_kind;

static void fp_add(const char *path, bool is_dir, uint32_t size, uint32_t dcrc) {
    if (g_nfp == g_capfp) {
        uint32_t nc = g_capfp ? g_capfp * 2 : 64;
        fp_t *n = realloc(g_fp, nc * sizeof *n);
        if (!n) return;
        g_fp = n;
        g_capfp = nc;
    }
    fp_t *f = &g_fp[g_nfp++];
    memset(f, 0, sizeof *f);
    f->pcrc = osc_crc32(0, path, (uint32_t)strlen(path));
    f->plen = (uint16_t)strlen(path);
    f->is_dir = is_dir;
    f->size = size;
    f->dcrc = dcrc;
}

static int fp_cmp(const void *a, const void *b) {
    const fp_t *x = a, *y = b;
    if (x->pcrc != y->pcrc) return x->pcrc < y->pcrc ? -1 : 1;
    return (int)x->plen - (int)y->plen;
}

static const fp_t *fp_find(const char *path) {
    if (!g_nfp) return NULL;
    fp_t key;
    memset(&key, 0, sizeof key);
    key.pcrc = osc_crc32(0, path, (uint32_t)strlen(path));
    key.plen = (uint16_t)strlen(path);
    return bsearch(&key, g_fp, g_nfp, sizeof *g_fp, fp_cmp);
}

static void fp_walk(const char *dir, int depth) {
    if (depth > 64) return;
    int n = vfs_list(dir, NULL, 0);
    if (n <= 0) return;
    vfs_dirent_t *ents = malloc((size_t)n * sizeof *ents);
    if (!ents) return;
    n = vfs_list(dir, ents, n);
    for (int i = 0; i < n; i++) {
        char path[VFS_PATH_MAX];
        vfs_join(dir, ents[i].name, path);
        if (!osv_path_saved(path, ents[i].is_dir)) continue;
        if (ents[i].is_dir) {
            fp_add(path, true, 0, 0);
            fp_walk(path, depth + 1);
        } else {
            uint32_t size = 0;
            void *d = vfs_read_file(path, &size);
            if (!d) continue;
            fp_add(path, false, size, osc_crc32(0, d, size));
            free(d);
        }
    }
    free(ents);
}

static void fp_save(void) {
    uint32_t bytes = 8 + g_nfp * (uint32_t)sizeof(fp_t);
    uint8_t *buf = malloc(bytes);
    if (!buf) return;
    memcpy(buf, "OSVD", 4);
    memcpy(buf + 4, &g_nfp, 4);
    if (g_nfp) memcpy(buf + 8, g_fp, g_nfp * sizeof(fp_t));
    vfs_mkdirs("/system/config");
    vfs_write_file(OSV_DEFAULTS, buf, bytes);
    free(buf);
}

static void fp_load(void) {
    uint32_t size = 0;
    uint8_t *buf = vfs_read_file(OSV_DEFAULTS, &size);
    if (!buf) return;
    uint32_t n = 0;
    if (size >= 8 && !memcmp(buf, "OSVD", 4)) memcpy(&n, buf + 4, 4);
    if (n && size == 8 + n * sizeof(fp_t)) {
        g_fp = malloc(n * sizeof(fp_t));
        if (g_fp) {
            memcpy(g_fp, buf + 8, n * sizeof(fp_t));
            g_nfp = g_capfp = n;
        }
    }
    free(buf);
}

static void osv_init(void) {
    const char *did = sys_cmdline_get("diskid");
    g_boot_kind = persist_active() ? 1 : 0;
    g_boot_id = did ? (uint32_t)strtoul(did, NULL, 16) : 0;
    if (!g_boot_id) g_boot_id = (uint32_t)rand() * 2654435761u ^ sys_time() ^ (sys_uptime_ms() << 16);
    if (!g_boot_id) g_boot_id = 1;
    uint32_t t0 = sys_uptime_ms();
    if (g_boot_kind == 0) {
        fp_walk("/", 0);
        if (g_nfp) qsort(g_fp, g_nfp, sizeof *g_fp, fp_cmp);
        fp_save();
    } else {
        fp_load();
    }
    klog("osaver: %u boot-default fingerprints (%s) in %u ms, boot id %u", g_nfp,
         g_boot_kind ? "from the drive" : "fresh system", sys_uptime_ms() - t0, g_boot_id);
}
REGISTER_INIT(INIT_FS, osv_init);

bool osv_default_dir(const char *path) {
    const fp_t *f = fp_find(path);
    return f && f->is_dir;
}

/* Files apps create the same way on every fresh system the first time they run
   (Image Viewer's sample pictures). Matched by path, size and CRC-32, so a changed
   file is always saved. */
static const struct { const char *path; uint32_t size, crc; } generated[] = {
    { "/home/user/Pictures/Plasma.bmp",     460854, 0x318d79fau },
    { "/home/user/Pictures/Glass orbs.bmp", 614522, 0xae5bf954u },
};

bool osv_default_file(const char *path, uint32_t size) {
    const fp_t *f = fp_find(path);
    uint32_t want = 0;
    if (f && !f->is_dir && f->size == size) want = f->dcrc;
    else {
        size_t i = 0;
        for (; i < sizeof generated / sizeof generated[0]; i++)
            if (generated[i].size == size && !strcmp(generated[i].path, path)) break;
        if (i == sizeof generated / sizeof generated[0]) return false;
        want = generated[i].crc;
    }
    uint32_t got = 0;
    void *d = vfs_read_file(path, &got);
    if (!d) return false;
    bool same = got == size && osc_crc32(0, d, got) == want;
    free(d);
    return same;
}

uint32_t osv_boot_id(void) { return g_boot_id; }
uint32_t osv_boot_kind(void) { return g_boot_kind; }

/* ======================================================================================
 * Version, wallpaper target
 * ==================================================================================== */
void osv_version(char *out, int cap) {
    const char *v = sys_version();                 /* "NovaOS 2.5" */
    const char *p = v;
    while (*p && !(*p >= '0' && *p <= '9')) p++;
    int n = 0;
    while (p[n] && ((p[n] >= '0' && p[n] <= '9') || (p[n] == '.' && p[n + 1] >= '0' && p[n + 1] <= '9'))) n++;
    if (n == 0 || n >= cap) { strlcpy(out, "1.0", (size_t)cap); return; }
    memcpy(out, p, (size_t)n);
    out[n] = 0;
}

/* "Solid colour" -> "solidcolour" for loose file name matching. */
static void squash(const char *s, char *out, int cap) {
    int n = 0;
    for (; *s && n < cap - 1; s++) {
        char c = *s;
        if (c == ' ' || c == '_' || c == '-') continue;
        out[n++] = (char)tolower((unsigned char)c);
    }
    out[n] = 0;
}

void osv_wallpaper_target(int index, char *out, int cap) {
    if (index < 0) { strlcpy(out, "Custom", (size_t)cap); return; }
    const char *name = theme_wallpaper_name(index);
    if (!name) name = "Nova";
    char want[64];
    squash(name, want, sizeof want);
    /* item 9: the UI files (wallpapers) are real files under /system */
    static const char *const dirs[] = { "/system/ui/wallpapers", "/system/wallpapers", "/system/ui" };
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        int n = vfs_list(dirs[d], NULL, 0);
        if (n <= 0) continue;
        vfs_dirent_t *ents = malloc((size_t)n * sizeof *ents);
        if (!ents) continue;
        n = vfs_list(dirs[d], ents, n);
        for (int i = 0; i < n; i++) {
            if (ents[i].is_dir) continue;
            char base[VFS_NAME_MAX], type[VFS_NAME_MAX], sq[VFS_NAME_MAX];
            osv_split_name(ents[i].name, base, sizeof base, type, sizeof type);
            squash(base, sq, sizeof sq);
            if (!strcmp(sq, want)) {
                vfs_join(dirs[d], ents[i].name, out);
                free(ents);
                return;
            }
        }
        free(ents);
    }
    strlcpy(out, name, (size_t)cap);
}

/* ======================================================================================
 * Alert sound: agent A's sys_alert_sound() when the kernel has it (weak reference, so
 * this file links either way), else the classic two-tone beep.
 * ==================================================================================== */
extern void osv_sys_alert_sound(void) __asm__("sys_alert_sound") __attribute__((weak));

void osv_alert(void) {
    if (osv_sys_alert_sound) { osv_sys_alert_sound(); return; }
    static const note_t notes[] = { { 880, 90 }, { 0, 30 }, { 660, 140 } };
    sys_play_notes(notes, 3);
}

/* ======================================================================================
 * Start button colour (welcome screen): /system/config/osaver "startcolor=N"
 * ==================================================================================== */
static int g_choice = 0;
static bool g_choice_loaded;

int osv_start_choice(void) {
    if (!g_choice_loaded) {
        g_choice_loaded = true;
        char *t = vfs_read_file(OSV_CFG_FILE, NULL);
        if (t) {
            char *p = strstr(t, "startcolor=");
            if (p) g_choice = (int)strtol(p + 11, NULL, 10);
            free(t);
        }
        if (g_choice < -1 || g_choice > theme_accent_count()) g_choice = 0;
    }
    return g_choice;
}

void osv_set_start_choice(int choice) {
    if (choice < -1 || choice > theme_accent_count()) choice = 0;
    g_choice = choice;
    g_choice_loaded = true;
    char buf[48];
    int n = snprintf(buf, sizeof buf, "startcolor=%d\n", choice);
    vfs_mkdirs("/system/config");
    vfs_write_file(OSV_CFG_FILE, buf, (uint32_t)n);
}

uint32_t osv_start_color(void) {
    int c = osv_start_choice();
    if (c < 0) return theme_get()->accent | 0xFF000000u;
    if (c == 0) return OSV_BLUE;
    return theme_accent_preset(c - 1) | 0xFF000000u;
}

/* ======================================================================================
 * Drawing
 * ==================================================================================== */
void osv_draw_backdrop(surface_t *s, rect_t r) {
    gfx_gradient_v(s, r.x, r.y, r.w, r.h, OSV_BG_TOP, OSV_BG_BOTTOM);
}

void osv_draw_bar(surface_t *s, rect_t b, int val, int alpha) {
    gfx_fill_rounded_rect(s, b.x, b.y + 1, b.w, b.h - 2, (b.h - 2) / 2, RGBA(255, 255, 255, 30 * alpha / 255));
    int fw = (int)((int64_t)b.w * CLAMP(val, 0, 1000) / 1000);
    if (fw >= b.h - 2)
        gfx_fill_rounded_gradient_v(s, b.x, b.y + 1, fw, b.h - 2, (b.h - 2) / 2, RGB(150, 120, 255), RGB(60, 190, 240));
}

bool osv_test(const char *what) {
    const char *t = sys_cmdline_get("osaver_test");
    return t && strstr(t, what) != NULL;
}
