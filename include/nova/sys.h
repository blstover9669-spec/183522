/*
 * nova/sys.h - kernel services: memory, logging, time, sound, power, info.
 */
#ifndef NOVA_SYS_H
#define NOVA_SYS_H

#include <nova/types.h>

/* ---- memory --------------------------------------------------------------- */
void  *kmalloc(size_t size);               /* 16-byte aligned; NULL on failure */
void  *kcalloc(size_t n, size_t size);     /* zeroed                           */
void  *krealloc(void *p, size_t size);
void   kfree(void *p);
size_t kmalloc_usable_size(void *p);

/* ---- logging (COM1 serial, 115200 8N1) --------------------------------------- */
/* klog prefixes the uptime and appends a newline if missing. Also kept in an
   in-memory ring buffer (sys_log_text()). */
void klog(const char *fmt, ...) PRINTF_LIKE(1, 2);
void kvlog(const char *fmt, va_list ap);
/* Raw serial output, no prefix. */
void serial_write(const char *s);
/* Copy of the recent kernel log (newest last). Returns the length written. */
int  sys_log_text(char *buf, int size);

/* Kernel panic: blue screen with message + register dump, also to serial. */
void kpanic(const char *fmt, ...) NORETURN PRINTF_LIKE(1, 2);

/* ---- system information ------------------------------------------------------- */
const char *sys_version(void);        /* "NovaOS 1.0"                    */
const char *sys_kernel_version(void); /* "1.0.0 (build date)"            */
const char *sys_cpu_name(void);       /* CPUID brand string              */
uint32_t    sys_uptime_ms(void);      /* milliseconds since boot         */
uint32_t    sys_mem_total(void);      /* usable RAM in bytes             */
uint32_t    sys_mem_free(void);       /* free kernel heap bytes          */
uint32_t    sys_mem_used(void);       /* allocated heap bytes            */
uint32_t    sys_heap_size(void);      /* total heap bytes                */
/* Kernel command line: value for key ("autorun" -> "About,Terminal") or NULL.
   For flags without '=' the value is "". */
const char *sys_cmdline_get(const char *key);
const char *sys_cmdline(void);
/* BIOS drive the bootloader was started from (0x80 = first hard disk, CD-ROMs are
   usually 0x9F/0xE0+), or -1 if unknown. Lets an installed system tell it booted
   from the hard disk. */
int         sys_boot_drive(void);
/* Frames composited per second (last second). */
int         sys_fps(void);

/* ---- time ------------------------------------------------------------------- */
typedef struct datetime {
    int year, month, day;        /* 2026, 1..12, 1..31  */
    int hour, minute, second;    /* 24h                 */
    int weekday;                 /* 0 = Sunday          */
} datetime_t;

void        sys_datetime(datetime_t *dt);      /* current local time (from the CMOS RTC) */
uint32_t    sys_time(void);                    /* seconds since 1970-01-01               */
void        sys_time_to_datetime(uint32_t t, datetime_t *dt);
uint32_t    sys_datetime_to_time(const datetime_t *dt);
const char *sys_month_name(int month);         /* 1..12 -> "January"  */
const char *sys_weekday_name(int wd);          /* 0..6  -> "Sunday"   */
/* Busy-wait microseconds. ONLY for drivers talking to hardware - never in apps. */
void        sys_udelay(uint32_t us);

/* ---- sound (PC speaker) ---------------------------------------------------------- */
void sys_beep(uint32_t freq_hz, uint32_t ms);   /* non-blocking               */
void speaker_tone(uint32_t freq_hz);            /* start a continuous tone    */
void speaker_off(void);
/* Play a simple melody (non-blocking). Each note: frequency (0 = rest) and duration. */
typedef struct note { uint16_t freq, ms; } note_t;
void sys_play_notes(const note_t *notes, int count);
bool sys_sound_enabled(void);
void sys_set_sound_enabled(bool on);

/* ---- power ---------------------------------------------------------------------- */
void sys_reboot(void) NORETURN;
void sys_shutdown(void) NORETURN;   /* tries ACPI/emulator ports, else "safe to power off" */
/* Registered callbacks run before reboot/shutdown (e.g. flush filesystem to disk). */
typedef void (*shutdown_hook_fn)(void);
void sys_add_shutdown_hook(shutdown_hook_fn fn);

/* ---- display mode ----------------------------------------------------------------- */
typedef struct display_info {
    int width, height;
    int bpp;              /* bits per pixel of the real framebuffer: 32, 24 or 16 */
    int pitch;            /* bytes per row                                         */
    int r_pos, g_pos, b_pos;  /* bit position of each channel                      */
    const char *source;   /* "multiboot" (mode set by the boot loader) or "bga"    */
} display_info_t;
void sys_display_info(display_info_t *out);

/* ---- input settings --------------------------------------------------------------- */
void sys_set_mouse_speed(int speed);   /* 1 (slow) .. 10 (fast), 5 = 1:1 */
int  sys_get_mouse_speed(void);
bool sys_mouse_has_wheel(void);
void sys_set_double_click_ms(int ms);
int  sys_get_double_click_ms(void);

/* ---- init hooks ------------------------------------------------------------------
 * Drivers and services register boot-time init functions from their own file,
 * without editing the kernel:  REGISTER_INIT(INIT_DRIVER, ata_init);
 * Stages run in this order during boot:
 *   INIT_DRIVER - after PCI, timers, keyboard/mouse; before the filesystem exists
 *   INIT_FS     - after the default VFS tree is created (load persisted files here)
 *   INIT_LATE   - after the desktop is up, just before autorun apps start
 * Init functions may use klog, kmalloc, timers, the VFS (from INIT_FS on), but
 * must not block for long. */
#define INIT_DRIVER 1
#define INIT_FS     2
#define INIT_LATE   3

typedef struct initcall {
    int stage;
    void (*fn)(void);
    const char *name;
} initcall_t;

#define _INIT_CAT2(a, b) a##b
#define _INIT_CAT(a, b) _INIT_CAT2(a, b)
#define REGISTER_INIT(stage_, fn_)                                                   \
    static const initcall_t _INIT_CAT(_initcall_, __LINE__)                          \
        __attribute__((used, section(".initcalls"), aligned(4))) = { stage_, fn_, #fn_ }

#endif
