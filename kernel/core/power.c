/*
 * power.c - reboot, shutdown (ACPI S5 + emulator ports), the ACPI power button
 * and the power-off screen.
 *
 * ACPI power button (fixed-feature model): VirtualBox's "ACPI Shutdown" (and
 * closing the VM window with "Send the shutdown signal") and QEMU's
 * system_powerdown set PWRBTN_STS (bit 8) in PM1a_STS. At INIT_DRIVER time we
 * switch the chipset to ACPI mode (SMI_CMD <- ACPI_ENABLE), clear a stale
 * PWRBTN_STS, set PWRBTN_EN in PM1a_EN (and PM1b), then poll PM1a_STS from a
 * 100 ms WM timer. A press is acknowledged (write-1-to-clear) and starts the
 * same graceful shutdown as Start > Shut down (shutdown hooks save the files
 * on installed systems). The SCI IRQ (FADT SCI_INT, normally 9) stays masked
 * at the PIC: polling needs no handler, so an SCI that is asserted until the
 * next poll does nothing. A driver that ever unmasks that IRQ for a shared PCI
 * device must call acpi_poll_events() from its handler.
 * Without ACPI (no RSDP / FADT / PM1 event block) nothing is touched.
 * The kernel option acpibutton=0 turns the power button off.
 */
#include "kernel.h"

/* ---- ACPI: just enough to find PM1 control blocks and \_S5 sleep type ------- */
static uint32_t pm1a_cnt, pm1b_cnt, smi_cmd;
static uint32_t pm1a_evt, pm1b_evt;        /* PM1x_EVT_BLK: STS at +0, EN at +len/2 */
static uint8_t pm1_evt_len;
static uint16_t sci_irq;
static uint32_t fadt_flags;
static uint8_t acpi_enable_val;
static uint16_t slp_typa, slp_typb;
static bool acpi_ok;

#define PM1_PWRBTN      0x0100   /* PWRBTN_STS / PWRBTN_EN (bit 8) */
#define PM1_SCI_EN      0x0001   /* PM1_CNT: ACPI mode             */
#define FADT_PWR_BUTTON (1u << 4) /* power button is a control-method device */

typedef struct PACKED { char sig[4]; uint32_t len; uint8_t rev, csum; char oem[6], oemtab[8]; uint32_t oemrev, creator, crev; } sdt_hdr_t;

static bool checksum_ok(const void *p, uint32_t len) {
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++) s = (uint8_t)(s + ((const uint8_t *)p)[i]);
    return s == 0;
}

static const uint8_t *find_rsdp(void) {
    uintptr_t bda = 0x40E;
    __asm__("" : "+r"(bda));   /* hide the low constant address from -Warray-bounds */
    uint32_t ebda = (uint32_t)(*(volatile uint16_t *)bda) << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000) {
        for (uint32_t a = ebda; a < ebda + 1024; a += 16)
            if (!memcmp((const void *)a, "RSD PTR ", 8) && checksum_ok((const void *)a, 20)) return (const uint8_t *)a;
    }
    for (uint32_t a = 0xE0000; a < 0x100000; a += 16)
        if (!memcmp((const void *)a, "RSD PTR ", 8) && checksum_ok((const void *)a, 20)) return (const uint8_t *)a;
    return NULL;
}

void acpi_init(void) {
    const uint8_t *rsdp = find_rsdp();
    if (!rsdp) { klog("acpi: no RSDP"); return; }
    const sdt_hdr_t *rsdt = (const sdt_hdr_t *)(uintptr_t)(*(const uint32_t *)(rsdp + 16));
    if (!rsdt || memcmp(rsdt->sig, "RSDT", 4)) { klog("acpi: no RSDT"); return; }
    int n = (int)(rsdt->len - sizeof(sdt_hdr_t)) / 4;
    const uint32_t *ents = (const uint32_t *)(rsdt + 1);
    const uint8_t *fadt = NULL;
    for (int i = 0; i < n; i++) {
        const sdt_hdr_t *h = (const sdt_hdr_t *)(uintptr_t)ents[i];
        if (h && !memcmp(h->sig, "FACP", 4)) { fadt = (const uint8_t *)h; break; }
    }
    if (!fadt) { klog("acpi: no FADT"); return; }
    const sdt_hdr_t *dsdt = (const sdt_hdr_t *)(uintptr_t)(*(const uint32_t *)(fadt + 40));
    uint32_t flen = ((const sdt_hdr_t *)fadt)->len;
    sci_irq = *(const uint16_t *)(fadt + 46);
    smi_cmd = *(const uint32_t *)(fadt + 48);
    acpi_enable_val = fadt[52];
    pm1a_evt = *(const uint32_t *)(fadt + 56);
    pm1b_evt = *(const uint32_t *)(fadt + 60);
    pm1a_cnt = *(const uint32_t *)(fadt + 64);
    pm1b_cnt = *(const uint32_t *)(fadt + 68);
    pm1_evt_len = flen > 88 ? fadt[88] : 0;
    fadt_flags = flen >= 116 ? *(const uint32_t *)(fadt + 112) : 0;
    /* ACPI 2.0+ extended blocks (generic address structures), used only when the
       32-bit fields are empty and the block is in I/O space (space id 1). */
    if (flen >= 196) {
        static const uint16_t xoff[4] = { 148, 160, 172, 184 };
        uint32_t *dst[4] = { &pm1a_evt, &pm1b_evt, &pm1a_cnt, &pm1b_cnt };
        for (int i = 0; i < 4; i++) {
            const uint8_t *g = fadt + xoff[i];
            uint64_t addr = *(const uint64_t *)(g + 4);
            if (!*dst[i] && g[0] == 1 && addr && addr < 0x10000) *dst[i] = (uint32_t)addr;
        }
    }
    if (dsdt && !memcmp(dsdt->sig, "DSDT", 4)) {
        const uint8_t *p = (const uint8_t *)(dsdt + 1);
        const uint8_t *end = (const uint8_t *)dsdt + dsdt->len;
        for (; p + 4 < end; p++) {
            if (memcmp(p, "_S5_", 4)) continue;
            if ((p[-1] == 0x08 || (p[-2] == 0x08 && p[-1] == '\\')) && p[4] == 0x12) {
                const uint8_t *q = p + 5;
                q += ((*q & 0xC0) >> 6) + 2;     /* PkgLength + NumElements */
                if (*q == 0x0A) q++;
                slp_typa = (uint16_t)(*q << 10);
                q++;
                if (*q == 0x0A) q++;
                slp_typb = (uint16_t)(*q << 10);
                acpi_ok = pm1a_cnt != 0;
            }
            break;
        }
    }
    klog("acpi: pm1a=%x pm1b=%x smi=%x s5=%x/%x %s", pm1a_cnt, pm1b_cnt, smi_cmd,
         slp_typa >> 10, slp_typb >> 10, acpi_ok ? "ok" : "unusable");
    klog("acpi: pm1a_evt=%x pm1b_evt=%x evt_len=%u sci=%u flags=%x", pm1a_evt, pm1b_evt,
         (unsigned)pm1_evt_len, (unsigned)sci_irq, fadt_flags);
}

/* Switch the chipset from legacy (SMI) mode to ACPI mode if it is not there yet. */
static bool acpi_enable_mode(void) {
    if (!pm1a_cnt) return false;
    if (inw((uint16_t)pm1a_cnt) & PM1_SCI_EN) return true;
    if (!smi_cmd || !acpi_enable_val) return false;
    outb((uint16_t)smi_cmd, acpi_enable_val);
    for (int i = 0; i < 300 && !(inw((uint16_t)pm1a_cnt) & PM1_SCI_EN); i++) sys_udelay(1000);
    return (inw((uint16_t)pm1a_cnt) & PM1_SCI_EN) != 0;
}

bool acpi_shutdown(void) {
    if (!acpi_ok) return false;
    acpi_enable_mode();
    outw((uint16_t)pm1a_cnt, slp_typa | (1 << 13));
    if (pm1b_cnt) outw((uint16_t)pm1b_cnt, slp_typb | (1 << 13));
    for (int i = 0; i < 100; i++) sys_udelay(1000);
    return false;   /* still running: it did not work */
}

/* ---- hooks ---------------------------------------------------------------------- */
static shutdown_hook_fn hooks[8];
static int nhooks;

void sys_add_shutdown_hook(shutdown_hook_fn fn) {
    if (nhooks < 8) hooks[nhooks++] = fn;
}

static void run_hooks(void) {
    for (int i = 0; i < nhooks; i++) hooks[i]();
}

/* ---- full-screen message ---------------------------------------------------------- */
void power_screen(const char *title, const char *msg) {
    surface_t *s = surface_create(g_fb.width, g_fb.height);
    if (!s) return;
    gfx_gradient_v(s, 0, 0, s->w, s->h, RGB(18, 24, 48), RGB(6, 8, 18));
    int cx = s->w / 2, cy = s->h / 2;
    gfx_fill_circle_aa(s, cx, cy - 90, 34, RGB(90, 120, 255));
    gfx_circle_aa(s, cx, cy - 90, 18, 4, RGB(255, 255, 255));
    gfx_fill_rect(s, cx - 2, cy - 118, 4, 26, RGB(90, 120, 255));
    gfx_fill_rect(s, cx - 2, cy - 114, 4, 22, RGB(255, 255, 255));
    gfx_text_aligned(s, rect_make(0, cy - 30, s->w, 40), title, RGB(240, 244, 255), FONT_UI_TITLE, ALIGN_CENTER | ALIGN_MIDDLE);
    gfx_text_aligned(s, rect_make(0, cy + 16, s->w, 24), msg, RGB(160, 170, 200), FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
    fb_present(s, rect_make(0, 0, s->w, s->h));
    surface_destroy(s);
}

void sys_reboot(void) {
    klog("power: rebooting");
    run_hooks();
    __asm__ volatile("cli");
    /* 8042 reset line */
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {}
    outb(0x64, 0xFE);
    for (int i = 0; i < 100000; i++) io_wait();
    /* PCI reset control register */
    outb(0xCF9, 0x02);
    io_wait();
    outb(0xCF9, 0x06);
    for (int i = 0; i < 100000; i++) io_wait();
    /* triple fault */
    struct PACKED { uint16_t limit; uint32_t base; } null_idt = { 0, 0 };
    __asm__ volatile("lidt %0; int $3" : : "m"(null_idt));
    for (;;) __asm__ volatile("hlt");
}

/* ---- ACPI power button -------------------------------------------------------------- */
static bool pwrbtn_on;                  /* PWRBTN_EN set, polling */
static volatile bool pwrbtn_pending;    /* a press was acknowledged, shutdown not started yet */
static bool shutting_down;

static uint16_t evt_en_port(uint32_t evt) { return (uint16_t)(evt + pm1_evt_len / 2); }

/* Acknowledge a pending power button press on one PM1 event block. */
static bool pwrbtn_check(uint32_t evt) {
    if (!evt) return false;
    uint16_t sts = inw((uint16_t)evt);
    if (sts == 0xFFFF || !(sts & PM1_PWRBTN)) return false;
    outw((uint16_t)evt, PM1_PWRBTN);            /* write 1 to clear */
    return true;
}

/* Check and acknowledge PM1 events. IRQ-safe (port I/O and a flag only): the
   shutdown itself starts from the main loop, in the 100 ms timer below. */
void acpi_poll_events(void) {
    if (!pwrbtn_on) return;
    bool a = pwrbtn_check(pm1a_evt);
    bool b = pwrbtn_check(pm1b_evt);
    if (a || b) pwrbtn_pending = true;
}

static void pwrbtn_timer(void *ctx) {
    (void)ctx;
    acpi_poll_events();
    if (!pwrbtn_pending || shutting_down) return;
    klog("power: ACPI power button pressed - shutting down");
    sys_shutdown();          /* same path as Start > Shut down; never returns */
}

static bool pwrbtn_enable(uint32_t evt) {
    if (!evt) return true;
    outw((uint16_t)evt, PM1_PWRBTN);            /* drop a stale press */
    if (inw((uint16_t)evt) & PM1_PWRBTN) {
        klog("acpi: power button status at %x does not clear - not using it", evt);
        return false;
    }
    uint16_t en = evt_en_port(evt);
    outw(en, (uint16_t)(inw(en) | PM1_PWRBTN));
    return true;
}

static void acpi_power_button_init(void) {
    const char *opt = sys_cmdline_get("acpibutton");
    if (opt && !strcmp(opt, "0")) { klog("acpi: power button disabled (acpibutton=0)"); return; }
    if (!pm1a_evt || pm1_evt_len < 4 || (pm1_evt_len & 1)) {
        klog("acpi: no PM1 event block - power button not available");
        return;
    }
    if (fadt_flags & FADT_PWR_BUTTON)
        klog("acpi: FADT says the power button is a control-method device; trying the fixed one anyway");
    bool mode = acpi_enable_mode();
    if (!pwrbtn_enable(pm1a_evt)) return;
    if (pm1b_evt && !pwrbtn_enable(pm1b_evt)) pm1b_evt = 0;
    pwrbtn_on = true;
    timer_add(100, pwrbtn_timer, NULL, true);
    klog("acpi: power button enabled (PM1a_EVT %x%s, ACPI mode %s, polled every 100 ms, SCI IRQ %u masked)",
         pm1a_evt, pm1b_evt ? " + PM1b" : "", mode ? "on" : "unavailable", (unsigned)sci_irq);
}
REGISTER_INIT(INIT_DRIVER, acpi_power_button_init);

void sys_shutdown(void) {
    shutting_down = true;
    klog("power: shutting down");
    run_hooks();
    if (g_fb.addr) power_screen("Shutting down...", "Please wait");
    __asm__ volatile("cli");
    acpi_shutdown();
    outw(0x604, 0x2000);    /* QEMU (newer, PIIX4 PM) */
    outw(0xB004, 0x2000);   /* Bochs / older QEMU     */
    outw(0x4004, 0x3400);   /* VirtualBox             */
    outw(0x600, 0x34);      /* Cloud Hypervisor       */
    for (int i = 0; i < 200000; i++) io_wait();
    klog("power: still running - showing the power-off screen");
    if (g_fb.addr) power_screen("It is now safe to turn off your computer.", "NovaOS has shut down.");
    for (;;) __asm__ volatile("cli; hlt");
}
