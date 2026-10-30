/*
 * osaver_internal.h - shared by the OSaver files (NovaOS 2.5, items 13 and 14):
 *
 *   apps/osaver.c          the OSaver app: welcome screen, Start, the warning with
 *                          Copy / View source (pretty print / no pretty print), and
 *                          making a code (chunked over timer ticks, streamed to a file)
 *   apps/osaver_restore.c  loading a code: a soft restart with a splash-style loading
 *                          screen (validate everything first, then create the files)
 *   apps/osaver_prompt.c   "Do you have a save file?" (ISO boots, after Continue ISO)
 *   apps/osaver_common.c   what gets saved, the boot-default fingerprints, config,
 *                          the alert sound
 *
 * The number language itself is kernel/osaver/codec.[ch] (host-tested by
 * tools/osaver_codec_test.c).
 */
#ifndef OSAVER_INTERNAL_H
#define OSAVER_INTERNAL_H

#include <nova.h>
#include <nova/osaver.h>
#include "osaver/codec.h"

#define OSV_DIR          "/tmp/OSaver"
#define OSV_RAW_FILE     OSV_DIR "/OSaver code.txt"                 /* the code (device form)  */
#define OSV_PRETTY_FILE  OSV_DIR "/OSaver code (pretty print).txt"  /* View source, pretty     */
#define OSV_COPY_FILE    "/home/user/Documents/OSaver code.txt"     /* Copy fallback           */
#define OSV_CFG_FILE     "/system/config/osaver"                    /* startcolor=...          */
#define OSV_DEFAULTS     "/system/config/osaver.defaults"           /* boot-default fingerprints */
#define OSV_CLIP_MAX     (16u << 20)    /* bigger codes are copied to OSV_COPY_FILE         */
#define OSV_BLUE         RGB(30, 110, 235)                         /* the Start button's blue */

/* Splash colours (the same as the boot splash, kernel/gui/bootscreen.h). */
#define OSV_BG_TOP       RGB(12, 14, 34)
#define OSV_BG_BOTTOM    RGB(6, 7, 16)

/* ---- osaver_common.c ---------------------------------------------------------------- */
/* Does this path belong in a code? (not /tmp, not the system files in /system apart
   from /system/config, not OSaver's own files). Used when saving and when restoring. */
bool     osv_path_saved(const char *path, bool is_dir);
/* A valid single file or folder name for restoring (no '/', not "." or "..", fits). */
bool     osv_name_ok(const char *name);
/* Was this file / folder part of a fresh system (same path, size and contents)? */
bool     osv_default_file(const char *path, uint32_t size);   /* reads the file only when needed */
bool     osv_default_dir(const char *path);
/* The running version as "2.5" (from sys_version()). */
void     osv_version(char *out, int cap);
/* The file target of built-in wallpaper `index` for the pretty print. */
void     osv_wallpaper_target(int index, char *out, int cap);
/* This boot's ID (B record) and kind (0 = ISO, 1 = installed drive). */
uint32_t osv_boot_id(void);
uint32_t osv_boot_kind(void);
/* The system alert sound (sys_alert_sound when NovaOS has it, else a beep). */
void     osv_alert(void);
/* Start button colour: choice 0 = blue, 1..n = theme accent preset n-1. Saved in
   OSV_CFG_FILE. */
int      osv_start_choice(void);
void     osv_set_start_choice(int choice);
uint32_t osv_start_color(void);
/* Split "photo.jpg.exe" into name "photo.jpg" and type "exe" (".profile" has no type). */
void     osv_split_name(const char *full, char *name, int ncap, char *type, int tcap);
/* Splash-style background + the Nova icon, as the soft restart and the prompt draw it. */
void     osv_draw_backdrop(surface_t *s, rect_t r);
/* The splash loading bar (val 0..1000). */
void     osv_draw_bar(surface_t *s, rect_t bar, int val, int alpha);
/* Test hooks: osaver_test=log on the command line logs codes up to 8 KB to serial. */
bool     osv_test(const char *what);

/* ---- osaver_restore.c ----------------------------------------------------------------- */
/* Start a soft restart that loads a code from memory (text, copied) or from a file.
   done(ctx, result) runs when the desktop is back: result 0 = restored, < 0 = the code
   was not valid (nothing was changed). Returns 0 if it started, < 0 if a restore is
   already running. */
int      osv_restore_start(const char *text, uint32_t len, const char *path,
                           void (*done)(void *ctx, int result), void *ctx);
bool     osv_restore_running(void);

#endif
