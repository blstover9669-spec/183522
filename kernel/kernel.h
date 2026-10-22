/* kernel.h - internal kernel interfaces (not part of the public app API). */
#ifndef KERNEL_H
#define KERNEL_H

#include <nova.h>
#include <nova/hw.h>

#define NOVA_VERSION        "1.5"
#define NOVA_EDITION        "Developer Preview"
#define NOVA_KERNEL_VERSION "1.5.0"

/* ---- multiboot ------------------------------------------------------------ */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

typedef struct PACKED multiboot_info {
    uint32_t flags;
    uint32_t mem_lower, mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr;
    uint32_t drives_length, drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info, vbe_mode_info;
    uint16_t vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width, framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
} multiboot_info_t;

typedef struct PACKED multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} multiboot_mmap_entry_t;

/* Parsed boot information (copied out of the multiboot structures early). */
typedef struct boot_info {
    char     cmdline[512];
    char     loader[64];
    int      mmap_count;
    struct { uint64_t addr, len; uint32_t type; } mmap[64];
    uint32_t mem_lower_kb, mem_upper_kb;
    uint32_t boot_device;     /* multiboot boot_device (BIOS drive in the top byte) */
    bool     has_boot_device;
    bool     has_fb;
    uint64_t fb_addr;
    uint32_t fb_pitch, fb_width, fb_height;
    uint8_t  fb_bpp, fb_type;
    uint8_t  red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
} boot_info_t;

extern boot_info_t g_boot;
void multiboot_parse(uint32_t magic, uint32_t mbi_addr);
void cmdline_parse(const char *cmdline);

/* ---- CPU ------------------------------------------------------------------- */
void gdt_init(void);
void idt_init(void);
void idt_set_gate(int n, uint32_t handler, uint16_t sel, uint8_t flags);
void pic_init(void);
void pic_eoi(int irq);
void pit_init(uint32_t hz);
void fpu_init(void);
void cpu_detect(void);
void paging_init(void);
void stack_guard_init(void);
void stack_guard_check(void);
void isr_dump_regs(regs_t *r);

extern volatile uint32_t g_ticks;     /* milliseconds since PIT start */

/* ---- serial / log ---------------------------------------------------------------- */
void serial_init(void);
void serial_putc(char c);
bool serial_present(void);

/* ---- memory ------------------------------------------------------------------- */
void heap_init(uintptr_t start, size_t size);
void memory_init(void);
bool heap_check(void);            /* integrity check (debug) */
extern uint32_t g_mem_total;

/* ---- framebuffer ------------------------------------------------------------------- */
typedef struct framebuffer {
    uint8_t *addr;
    uint32_t pitch;      /* bytes per row */
    int width, height;
    int bpp;             /* 32, 24, 16 */
    int r_pos, g_pos, b_pos;
    const char *source;  /* "multiboot" / "bga" */
} framebuffer_t;

extern framebuffer_t g_fb;
bool fb_init(int want_w, int want_h);
void fb_present(const surface_t *src, rect_t r);   /* copy rect of back buffer to LFB */
bool bga_available(void);
bool bga_set_mode(int w, int h, int bpp);

/* ---- devices ------------------------------------------------------------------------ */
void pci_init(void);
void rtc_init(void);
void rtc_read(datetime_t *dt);
void speaker_init(void);
void speaker_tick(void);     /* from PIT IRQ */
void keyboard_init(void);
void keyboard_save_bios_flags(void);   /* before paging_init() */
void mouse_init(void);
void acpi_init(void);
bool acpi_shutdown(void);
void acpi_poll_events(void);    /* IRQ-safe: acknowledge ACPI power button presses */

/* ---- raw input queue (filled by IRQ handlers, drained by the WM) -------------------- */
typedef struct input_event {
    uint8_t type;       /* IN_KEY, IN_MOUSE */
    uint8_t pressed;    /* key: 1 down 0 up */
    uint8_t repeat;
    uint8_t buttons;    /* mouse */
    int16_t dx, dy, dz; /* mouse */
    uint16_t key;       /* keycode */
    uint8_t ch;         /* translated char (0 if none) */
    uint8_t mods;
} input_event_t;

#define IN_KEY   1
#define IN_MOUSE 2

void input_push(const input_event_t *ev);    /* IRQ context */
bool input_pop(input_event_t *ev);           /* main loop   */
bool input_pending(void);
void input_set_mods(int mods);
void input_set_key_state(int key, bool down);
extern bool g_test_mouse;                    /* testmouse=1: 1:1 motion, no accel */
extern int g_mouse_speed;
extern bool g_mouse_wheel;

/* ---- GUI internals --------------------------------------------------------------- */
void gfx_shadow_occluded(surface_t *s, int x, int y, int w, int h, int radius, int spread, int alpha,
                         rect_t occ, int occ_radius);
void wm_init(void);
void wm_run(void) NORETURN;
void wm_invalidate_screen(rect_t r);
void wm_invalidate_all(void);
void splash_show(void);
void splash_progress(int percent, const char *msg);
void splash_finish(void);
void shell_autorun(void);
void keyboard_init(void);
void i8042_enable_irqs(void);
void desktop_init(void);
void desktop_redraw(void);
void taskbar_init(void);
void taskbar_refresh(void);
void startmenu_toggle(void);
void startmenu_close(void);
bool startmenu_is_open(void);
void theme_init(void);
void theme_render_current_wallpaper(void);
void apps_init(void);
void vfs_init(void);
void vfs_populate_defaults(void);
void vfs_tick(void);
void timers_init(void);
void timers_run(void);
uint32_t timers_next_due(void);
void power_screen(const char *title, const char *msg);

extern char g_cpu_name[49];
extern char g_cpu_vendor[13];

#endif
