/* kmain.c - kernel entry: bring up the hardware, then hand over to the window manager. */
#include "kernel.h"

const char *sys_version(void) { return "NovaOS " NOVA_VERSION; }
const char *sys_edition(void) { return NOVA_EDITION; }
const char *sys_kernel_version(void) { return NOVA_KERNEL_VERSION " (" __DATE__ ")"; }

/* Last resort when there is no graphics mode: write to the VGA text buffer. */
static void text_mode_message(const char *msg) {
    volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
    for (int i = 0; i < 80 * 25; i++) vga[i] = 0x1F00 | ' ';
    for (int i = 0; msg[i] && i < 80 * 25; i++) vga[i] = (uint16_t)(0x1F00 | (uint8_t)msg[i]);
}

void selftest_run(void);

extern const initcall_t __initcalls_start[], __initcalls_end[];

static void run_initcalls(int stage) {
    for (const initcall_t *c = __initcalls_start; c < __initcalls_end; c++) {
        if (c->stage != stage || !c->fn) continue;
        klog("init: %s (stage %d)", c->name, stage);
        c->fn();
    }
}

/* crashtest=1: deliberately fault a while after boot to exercise the panic screen. */
static void crash_cb(void *ctx) {
    (void)ctx;
    klog("crashtest: dividing by zero");
    __asm__ volatile("xor %%ecx, %%ecx\n\tdiv %%ecx" : : : "eax", "ecx", "edx");
}

static void parse_resolution(int *w, int *h) {
    *w = *h = 0;
    const char *r = sys_cmdline_get("res");
    if (!r) return;
    char *x;
    long a = strtol(r, &x, 10);
    if (*x != 'x' && *x != 'X') return;
    long b = strtol(x + 1, NULL, 10);
    if (a >= 640 && a <= 4096 && b >= 480 && b <= 4096) { *w = (int)a; *h = (int)b; }
}

void kmain(uint32_t magic, uint32_t mbi) {
    stack_guard_init();
    serial_init();
    serial_write("\n");
    klog("NovaOS %s %s kernel %s booting", NOVA_VERSION, NOVA_EDITION, NOVA_KERNEL_VERSION);
    multiboot_parse(magic, mbi);
    cmdline_parse(g_boot.cmdline);
    klog("boot: loader \"%s\" cmdline \"%s\" drive %x", g_boot.loader, g_boot.cmdline, (unsigned)sys_boot_drive());

    gdt_init();
    idt_init();
    pic_init();
    fpu_init();
    cpu_detect();
    klog("cpu: %s (%s)", g_cpu_name, g_cpu_vendor);
    memory_init();
    acpi_init();          /* reads the BIOS data area in page 0: before paging */
    paging_init();
    pit_init(1000);
    speaker_init();
    __asm__ volatile("sti");
    pci_init();

    int rw, rh;
    parse_resolution(&rw, &rh);
    if (!fb_init(rw, rh)) {
        text_mode_message("NovaOS: no graphics framebuffer available. Please enable VBE/VESA graphics "
                          "(VirtualBox: VBoxVGA or VMSVGA).");
        kpanic("no framebuffer");
    }
    const char *tm = sys_cmdline_get("testmouse");
    g_test_mouse = tm && strcmp(tm, "0") != 0;
    const char *ms = sys_cmdline_get("mousespeed");
    if (ms) sys_set_mouse_speed(atoi(ms));

    theme_init();
    wm_init();
    splash_show();
    splash_progress(10, "Starting devices...");

    __asm__ volatile("cli");
    keyboard_init();
    mouse_init();
    i8042_enable_irqs();
    __asm__ volatile("sti");
    splash_progress(25, "Reading the clock...");
    rtc_init();
    srand(sys_time() ^ g_ticks);
    timers_init();
    run_initcalls(INIT_DRIVER);

    splash_progress(40, "Preparing files...");
    vfs_init();
    vfs_populate_defaults();
    run_initcalls(INIT_FS);

    splash_progress(60, "Loading apps...");
    apps_init();
    theme_render_current_wallpaper();

    splash_progress(80, "Building desktop...");
    desktop_init();
    taskbar_init();
    splash_finish();
    if (sys_cmdline_get("selftest")) selftest_run();
    if (sys_cmdline_get("crashtest")) timer_add(1500, crash_cb, NULL, false);
    run_initcalls(INIT_LATE);
    shell_autorun();
    klog("NOVA: desktop ready (%dx%d, %u MiB RAM, %d apps)", screen_width(), screen_height(),
         sys_mem_total() >> 20, app_count());
    wm_run();
}
