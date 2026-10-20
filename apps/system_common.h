/*
 * system_common.h - helpers shared by the System apps (apps/system_*.c:
 * Settings, Welcome, Piano):
 *
 *   - a tiny "key=value" settings store in /system/config/settings (next to the
 *     core's own config files; read-modify-write, so each app only touches its
 *     own keys);
 *   - CPUID queries for the About page;
 *   - hover tracking for ui_custom() widgets;
 *   - a few small drawing helpers (rounded "tile" icons, a check mark).
 *
 * Everything is static inline so each app can include it without
 * unused-function warnings.
 */
#ifndef SYSTEM_COMMON_H
#define SYSTEM_COMMON_H

#include <nova.h>

#define SYS_CFG_DIR  "/system/config"
#define SYS_CFG_PATH SYS_CFG_DIR "/settings"
#define SYS_CFG_MAX  32

/* ---- settings store ----------------------------------------------------------------- */
typedef struct { char key[24]; char val[128]; } sys_cfg_entry_t;
typedef struct { int n; sys_cfg_entry_t e[SYS_CFG_MAX]; } sys_cfg_t;

static inline void sys_cfg_load(sys_cfg_t *c) {
    c->n = 0;
    uint32_t size = 0;
    char *d = vfs_read_file(SYS_CFG_PATH, &size);
    if (!d) return;
    char *save = NULL;
    for (char *line = strtok_r(d, "\r\n", &save); line && c->n < SYS_CFG_MAX; line = strtok_r(NULL, "\r\n", &save)) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '#' || !*line) continue;
        char *eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        *eq = 0;
        strlcpy(c->e[c->n].key, line, sizeof c->e[0].key);
        strlcpy(c->e[c->n].val, eq + 1, sizeof c->e[0].val);
        c->n++;
    }
    free(d);
}

static inline const char *sys_cfg_get(const sys_cfg_t *c, const char *key) {
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->e[i].key, key)) return c->e[i].val;
    return NULL;
}

static inline int sys_cfg_get_int(const sys_cfg_t *c, const char *key, int def) {
    const char *v = sys_cfg_get(c, key);
    return (v && *v) ? (int)strtol(v, NULL, 0) : def;
}

static inline void sys_cfg_set(sys_cfg_t *c, const char *key, const char *val) {
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->e[i].key, key)) { strlcpy(c->e[i].val, val, sizeof c->e[0].val); return; }
    if (c->n >= SYS_CFG_MAX) return;
    strlcpy(c->e[c->n].key, key, sizeof c->e[0].key);
    strlcpy(c->e[c->n].val, val, sizeof c->e[0].val);
    c->n++;
}

static inline void sys_cfg_set_int(sys_cfg_t *c, const char *key, int v) {
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    sys_cfg_set(c, key, b);
}

static inline void sys_cfg_set_hex(sys_cfg_t *c, const char *key, uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "0x%06X", (unsigned)(v & 0xFFFFFF));
    sys_cfg_set(c, key, b);
}

static inline int sys_cfg_save(const sys_cfg_t *c) {
    char *buf = malloc(SYS_CFG_MAX * 160 + 64);
    if (!buf) return VFS_ENOMEM;
    int n = snprintf(buf, 64, "# NovaOS settings (written by Settings)\n");
    for (int i = 0; i < c->n; i++)
        n += snprintf(buf + n, 160, "%s=%s\n", c->e[i].key, c->e[i].val);
    vfs_mkdirs(SYS_CFG_DIR);
    int r = vfs_write_file(SYS_CFG_PATH, buf, (uint32_t)n);
    free(buf);
    return r;
}

/* Update a single key in the settings file (other keys are kept). */
static inline void sys_cfg_put(const char *key, const char *val) {
    sys_cfg_t *c = malloc(sizeof *c);
    if (!c) return;
    sys_cfg_load(c);
    const char *old = sys_cfg_get(c, key);
    if (!old || strcmp(old, val)) {
        sys_cfg_set(c, key, val);
        sys_cfg_save(c);
    }
    free(c);
}

static inline const char *sys_cfg_read(const char *key, char *out, int n) {
    sys_cfg_t *c = malloc(sizeof *c);
    if (!c) return NULL;
    sys_cfg_load(c);
    const char *v = sys_cfg_get(c, key);
    const char *r = NULL;
    if (v) { strlcpy(out, v, (size_t)n); r = out; }
    free(c);
    return r;
}

/* ---- CPUID ------------------------------------------------------------------------------ */
static inline void sys_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

typedef struct {
    char vendor[13];        /* "GenuineIntel", "AuthenticAMD" ...           */
    char hypervisor[13];    /* "VBoxVBoxVBox", "TCGTCGTCGTCG", "" if none   */
    int  family, model, stepping;
    uint32_t max_leaf;
    char features[160];     /* "FPU TSC MMX SSE SSE2 ..."                    */
} sys_cpu_info_t;

static inline void sys_cpu_info(sys_cpu_info_t *ci) {
    uint32_t a, b, c, d;
    memset(ci, 0, sizeof *ci);
    sys_cpuid(0, &a, &b, &c, &d);
    ci->max_leaf = a;
    memcpy(ci->vendor, &b, 4);
    memcpy(ci->vendor + 4, &d, 4);
    memcpy(ci->vendor + 8, &c, 4);
    ci->vendor[12] = 0;
    if (ci->max_leaf < 1) return;
    sys_cpuid(1, &a, &b, &c, &d);
    int fam = (a >> 8) & 15, mod = (a >> 4) & 15;
    if (fam == 15) fam += (a >> 20) & 255;
    if (fam == 6 || fam >= 15) mod += ((a >> 16) & 15) << 4;
    ci->family = fam;
    ci->model = mod;
    ci->stepping = a & 15;
    static const struct { uint8_t reg, bit; const char *name; } fl[] = {
        { 3, 0, "FPU" }, { 3, 4, "TSC" }, { 3, 6, "PAE" }, { 3, 9, "APIC" }, { 3, 15, "CMOV" },
        { 3, 23, "MMX" }, { 3, 25, "SSE" }, { 3, 26, "SSE2" }, { 2, 0, "SSE3" }, { 2, 9, "SSSE3" },
        { 2, 19, "SSE4.1" }, { 2, 20, "SSE4.2" }, { 2, 23, "POPCNT" }, { 2, 25, "AES" },
        { 2, 28, "AVX" }, { 2, 5, "VMX" },
    };
    int n = 0;
    for (int i = 0; i < (int)(sizeof fl / sizeof fl[0]); i++) {
        uint32_t r = fl[i].reg == 3 ? d : c;
        if (r & (1u << fl[i].bit))
            n += snprintf(ci->features + n, sizeof ci->features - (size_t)n, "%s%s", n ? " " : "", fl[i].name);
    }
    if (c & (1u << 31)) {   /* running under a hypervisor */
        sys_cpuid(0x40000000, &a, &b, &c, &d);
        memcpy(ci->hypervisor, &b, 4);
        memcpy(ci->hypervisor + 4, &c, 4);
        memcpy(ci->hypervisor + 8, &d, 4);
        ci->hypervisor[12] = 0;
        for (int i = 0; i < 12; i++)
            if ((unsigned char)ci->hypervisor[i] < 32 || (unsigned char)ci->hypervisor[i] > 126) ci->hypervisor[i] = 0;
        if (!ci->hypervisor[0]) strlcpy(ci->hypervisor, "unknown", sizeof ci->hypervisor);
    }
}

/* Friendly name for a hypervisor signature. */
static inline const char *sys_hypervisor_name(const char *sig) {
    if (!sig || !*sig) return NULL;
    if (!strncmp(sig, "VBoxVBoxVBox", 12)) return "Oracle VirtualBox";
    if (!strncmp(sig, "TCGTCGTCGTCG", 12)) return "QEMU (TCG emulation)";
    if (!strncmp(sig, "KVMKVMKVM", 9)) return "KVM";
    if (!strncmp(sig, "VMwareVMware", 12)) return "VMware";
    if (!strncmp(sig, "Microsoft Hv", 12)) return "Microsoft Hyper-V";
    if (!strncmp(sig, "XenVMMXenVMM", 12)) return "Xen";
    if (!strncmp(sig, " lrpepyh  vr", 12)) return "Parallels";
    if (!strncmp(sig, "bhyve bhyve ", 12)) return "bhyve";
    return sig;
}

/* ---- custom widget hover ---------------------------------------------------------------- */
/* Pointer position in the client coordinates of win; false if it is outside rect r. */
static inline bool sys_mouse_in(window_t *win, rect_t r, int *mx, int *my) {
    int x, y;
    input_mouse_pos(&x, &y);
    rect_t cr = win_client_rect(win);
    x -= cr.x;
    y -= cr.y;
    if (mx) *mx = x;
    if (my) *my = y;
    return rect_contains(r, x, y);
}

/* ---- drawing helpers ---------------------------------------------------------------------- */
static inline uint32_t sys_text_dim(const theme_t *t) { return color_mix(t->window_text, t->window_bg, 95); }

/* Rounded tile with a vertical gradient (icon backgrounds). */
static inline void sys_tile(surface_t *s, int x, int y, int size, uint32_t top, uint32_t bottom) {
    int r = size * 22 / 100;
    gfx_fill_rounded_gradient_v(s, x, y, size, size, r, top, bottom);
}

/* Anti-aliased check mark inside a size x size box. */
static inline void sys_check(surface_t *s, int x, int y, int size, uint32_t c) {
    double t = size / 7.0;
    if (t < 1.2) t = 1.2;
    double x0 = x + size * 0.18, y0 = y + size * 0.52;
    double x1 = x + size * 0.42, y1 = y + size * 0.76;
    double x2 = x + size * 0.84, y2 = y + size * 0.28;
    int steps = size * 2;
    for (int i = 0; i <= steps; i++) {
        double f = (double)i / steps;
        gfx_fill_circle_aa(s, x0 + (x1 - x0) * f, y0 + (y1 - y0) * f, t / 2, c);
        gfx_fill_circle_aa(s, x1 + (x2 - x1) * f, y1 + (y2 - y1) * f, t / 2, c);
    }
}

/* Thick anti-aliased stroke made of discs (round caps). */
static inline void sys_stroke(surface_t *s, double x0, double y0, double x1, double y1, double w, uint32_t c) {
    double dx = x1 - x0, dy = y1 - y0;
    double len = sqrt(dx * dx + dy * dy);
    int steps = (int)(len * 1.5) + 1;
    for (int i = 0; i <= steps; i++) {
        double f = (double)i / steps;
        gfx_fill_circle_aa(s, x0 + dx * f, y0 + dy * f, w / 2, c);
    }
}

/* Arc (angles in radians, 0 = right, clockwise because y grows down). */
static inline void sys_arc(surface_t *s, double cx, double cy, double r, double a0, double a1, double w, uint32_t c) {
    int steps = (int)(fabs(a1 - a0) * r * 1.5) + 2;
    for (int i = 0; i <= steps; i++) {
        double a = a0 + (a1 - a0) * i / steps;
        gfx_fill_circle_aa(s, cx + r * cos(a), cy + r * sin(a), w / 2, c);
    }
}

#endif
