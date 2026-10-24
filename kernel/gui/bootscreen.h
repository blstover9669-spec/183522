/*
 * bootscreen.h - the screens NovaOS shows while it starts (internal):
 *
 *   splash.c     animated boot splash (icon rises, rainbow shimmer) and the
 *                loading bar of installed-drive boots
 *   bootsetup.c  "NovaOS Drive installation" (ISO boots, after the splash)
 *   nodrive.c    "No drive found" (the drive's System/home folders or the
 *                system files in /system are gone)
 *   sysfiles.c   the system files in /system (kernel, boot files, UI files):
 *                seeding from the built-in copies, the list of required files
 *   uifiles.c    loading the UI (fonts, wallpapers, themes, UI settings)
 *                from /system/ui at startup
 *
 * Boot order (kernel/core/kmain.c): splash background -> devices -> files
 * (installed drive: the saved files are restored) -> drive check -> splash
 * animation -> [installed: loading bar] load UI files, apps, desktop ->
 * [ISO: NovaOS Drive installation] -> desktop.
 */
#ifndef BOOTSCREEN_H
#define BOOTSCREEN_H

#include "kernel.h"

/* ---- splash.c ------------------------------------------------------------------------ */
#define SPLASH_BG_TOP    RGB(12, 14, 34)
#define SPLASH_BG_BOTTOM RGB(6, 7, 16)

/* Play the splash animation: the icon rises from the bottom to the middle, shimmers
   in rainbow colours, then returns to its colours. With noanimation=1 in
   /system/config/splash.cfg the icon just appears. Needs the VFS. */
void       splash_animate(void);
bool       splash_active(void);
/* The splash's clean background (screen size) and where the icon is. */
surface_t *splash_background(void);
rect_t     splash_logo_rect(void);
/* Move the icon (with its glow at glow_alpha 0..255) to x,y at size; draws and
   presents only the changed area. */
void       splash_place_logo(int x, int y, int size, int glow_alpha);
/* End the splash without fading (the NovaOS Drive installation took over). */
void       splash_release(void);
/* The desktop as it will look (paints the windows that exist, then captures). */
surface_t *splash_capture_desktop(void);
/* Cross-fade the screen from one full-screen image to another. */
void       splash_crossfade(const surface_t *from, const surface_t *to, int ms);
/* Wait (halting the CPU) until g_ticks reaches t. */
void       boot_sleep_until(uint32_t t);

/* ---- bootsetup.c --------------------------------------------------------------------- */
/* True when this boot shows the NovaOS Drive installation: ISO boots (GRUB, not
   from an installed drive). setup=1 forces it, setup=0 / nosetup skips it; automated
   starts (autorun=, testmouse=) skip it too. */
bool bootsetup_wanted(void);
/* Run the NovaOS Drive installation. Returns after "Continue ISO"; "Install"
   restarts the computer into the installed system. */
void bootsetup_run(void);
/* After the desktop is up: the save file screen (OSaver), if there is one. */
void bootsetup_after_desktop(void);

/* ---- nodrive.c ----------------------------------------------------------------------- */
/* Check the drive at startup and start watching it while NovaOS runs. */
void nodrive_check_startup(void);
/* Black screen, Nova icon, "No drive found" - forever. */
void nodrive_show(const char *why) NORETURN;

/* ---- sysfiles.c ---------------------------------------------------------------------- */
/* Write the system files into /system from the built-in copies (ISO boot / first boot). */
void sysfiles_seed(void);
/* Bring the files that describe the running system up to date (if they exist). */
void sysfiles_refresh(void);
/* NULL if the drive is complete, else the first missing path. */
const char *sysfiles_missing(void);

/* ---- uifiles.c ----------------------------------------------------------------------- */
/* Load fonts, wallpapers, themes and UI settings from /system/ui. */
void uifiles_load(void);

#endif
