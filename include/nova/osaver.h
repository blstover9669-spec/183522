/*
 * nova/osaver.h - OSaver: save the whole OS state as a number code and
 * restore it again (NovaOS 2.5, items 13 and 14).
 *
 * The number language:
 *   - numbers only, plus the structure marks  :  ((  ))  .
 *     (letters and dashes are allowed only inside SystemData)
 *   - words are split into chunks, each chunk has a number
 *     (vers = 132, ion = 24 -> version = 13224); the chunk table is
 *     prefix-free so a copied code (hidden separators removed) splits one
 *     way only
 *   - full-word numbers: files = 4351991, wallpaper = 91235422,
 *     SystemData = 3332198 (System = 333, Data = 2198)
 *
 * Boot-time prompt (item 14, ISO boots only, shown after "Continue ISO"):
 *   osaver_boot_prompt(done, ctx) shows "Do you have a save file?" with
 *   Yes / No. No calls done(ctx). Yes shows a text box to paste a code into;
 *   the instant checker validates it and then runs the restore (soft
 *   restart with a loading screen), after which done(ctx) is called too.
 *   The call returns immediately: everything is event driven.
 */
#ifndef NOVA_OSAVER_H
#define NOVA_OSAVER_H

#include <nova/types.h>

#define OSAVER_MAX_CODE (256u * 1024u * 1024u)   /* 256 MB hard limit */

/* Item 14: "Do you have a save file?" screen. Never blocks; done(ctx) is
   called exactly once when the screen closes (after No, or after a restore). */
void osaver_boot_prompt(void (*done)(void *ctx), void *ctx);

/* Item 13: start restoring the code stored in a file (soft restart with a
   loading screen; returns at once, work is chunked over timer ticks).
   done(ctx) (may be NULL) is called when the desktop is back. Returns 0 if
   the restore started, negative if the code is not valid. */
int  osaver_restore_file(const char *path, void (*done)(void *ctx), void *ctx);

/* Quick structural check of a code ("instant checker"): true if the text has
   the version / files / wallpaper / SystemData lines with numbers, :, - and
   (( )). */
bool osaver_check_code(const char *text, uint32_t len);

/* The Start button's colour on the OSaver welcome screen (a Settings page may offer
   it too): 0 = blue (default), 1..theme_accent_count() = theme accent preset n-1.
   Kept in /system/config/osaver ("startcolor=N"). */
int  osaver_start_choice(void);
void osaver_set_start_choice(int choice);

#endif
