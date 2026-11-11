/*
 * bootscreen.h - the screens NovaOS shows while it starts (internal):
 *
 *   splash.c     animated boot splash (icon rises, rainbow shimmer) and the
 *                loading bar of installed-drive boots
 *   bootsetup.c  "NovaOS Drive installation" (ISO boots, after the splash)
 *   nodrive.c    "No drive found" (the drive's System/home folders or the
 *                system files in /system are gone)
 *   reqfail.c    "System requirement fail" (no readable COS:// drive of at
 *                least 4 MB on an installed-drive boot, or a bundled app's file
 *                in /system/apps is missing)
 *   firststart.c the installation menu of an installed drive's first start (the
 *                drive starts without system files): "This system now supports",
 *                the account, then it writes the system files and saves
 *   startscreen.c the start screen (time, account name and picture, Start or the
 *                password) - every start, before the desktop
 *   sysfiles.c   the system files in /system (kernel, boot files, UI files):
 *                seeding from the built-in copies, the list of required files
 *                (kernel/fs/sysfiles.c)
 *   uifiles.c    loading the UI (fonts, wallpapers, themes, UI settings)
 *                from /system/ui at startup
 *
 * Boot order (kernel/core/kmain.c): splash background -> devices -> app
 * registry -> files (installed drive: the saved files are restored, then the
 * COS:// drive check; ISO: the system files are written) -> drive check (No drive
 * found) -> bundle app check (/system/apps) -> UI files loaded -> INIT_FS hooks
 * -> splash animation -> [ISO: NovaOS Drive installation] -> [installed: loading
 * bar; first start: the installation menu instead of the drive and bundle app
 * checks, which run after it] -> desktop -> [ISO: save file screen] -> start
 * screen -> the desktop opens.
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
/* The loading bar goes away (the installation menu of a first start takes over). */
void       splash_bar_hide(void);

/* Boot hooks on the command line: 1 = key=1 (forced on), 0 = key=0, -1 = not given. */
int        boot_flag(const char *key);
/* Automated starts (autorun=, testmouse= - every tools/shot.py --kernel run). */
bool       boot_automated(void);

/* ---- bootsetup.c --------------------------------------------------------------------- */
/* True when this boot shows the NovaOS Drive installation: ISO boots (GRUB, not
   from an installed drive). setup=1 forces it, setup=0 / nosetup skips it; automated
   starts (autorun=, testmouse=) skip it too. */
bool bootsetup_wanted(void);
/* Run the NovaOS Drive installation. Returns after "Continue ISO"; after "Install"
   the finished screen's Restart restarts the computer into the installed system
   (Cancel or a failed installation come back to the screen). */
void bootsetup_run(void);
/* After Continue ISO, once the desktop exists (before it fades in): the save file
   screen (OSaver's osaver_boot_prompt, if it is part of the build). */
void bootsetup_after_desktop(void);

/* ---- firststart.c --------------------------------------------------------------------- */
/* True on an installed drive's first start: it holds no saved files yet (or only the
   files the Installer app copied), so it has no system files. */
bool firststart_pending(void);
/* The installation menu (after the splash animation): "This system now supports",
   the account (name, picture, optional password), then installing - the system files,
   the default folders and files and the account are written and saved to the drive.
   Automated starts (autorun=, testmouse=) without setup=1 install with the defaults
   (account "User", no password) without showing it. Returns true if it was shown
   (its last screen is on the display). The drive checks run afterwards (kmain). */
bool firststart_run(void);

/* ---- startscreen.c --------------------------------------------------------------------- */
/* Decide whether this start shows the start screen (startscreen=, setup=1, automated
   starts) - before the INIT_LATE hooks, which may wait for it. */
void startscreen_prepare(void);
/* Show it now (the desktop exists); the desktop opens when it is passed. */
void startscreen_show(void);

/* ---- nodrive.c ----------------------------------------------------------------------- */
/* Check the drive at startup (No drive found if it is incomplete) and start watching
   it while NovaOS runs. */
void nodrive_check_startup(void);
/* Black screen, Nova icon, "No drive found" - forever. `why`: the missing path. */
void nodrive_show(const char *why) NORETURN;
/* The black screen with the Nova icon, `title` and (if not NULL) `reason` under it,
   forever - shared by "No drive found" and "System requirement fail". */
void bootfail_screen(const char *title, const char *reason) NORETURN;
/* Liberation Sans 30 px / 20 px, ASCII (nodrive_font.c, tools/genfont.py --nodrive). */
extern const font_t font_nodrive, font_nodrive_small;

/* ---- reqfail.c ----------------------------------------------------------------------- */
/* After the saved files are restored: an installed-drive boot (boot=disk) needs its
   COS:// drive found on the IDE controllers, read without errors and at least 4 MB,
   else "System requirement fail" - forever. ISO boots pass. */
void reqfail_check_drive(void);
/* After the drive check: every bundled app needs its file in /system/apps. */
void reqfail_check_apps(void);
/* Black screen, Nova icon, "System requirement fail" and `reason` - forever.
   `detail` only goes to the log. */
void reqfail_show(const char *reason, const char *detail) NORETURN;

/* ---- sysfiles.c (kernel/fs) ------------------------------------------------------------- */
/* Write the system files into /system from the built-in copies (ISO boot / a drive's
   first start): all of them, or one part (SYSFILES_BOOT kernel + boot loader + boot.cfg,
   SYSFILES_UI /system/ui, SYSFILES_APPS /system/apps). */
enum { SYSFILES_BOOT, SYSFILES_UI, SYSFILES_APPS, SYSFILES_PARTS };
void sysfiles_seed(void);
void sysfiles_seed_part(int part);
/* The default home folders and files (kernel/fs/vfs.c): only_missing keeps files that
   are there already. */
void vfs_write_default_files(bool only_missing);
/* Bring the files that describe the running system (boot.cfg) up to date. */
void sysfiles_refresh(void);
/* NULL if the drive has all system files, else the first missing path. */
const char *sysfiles_missing(void);
/* NULL if /system/apps has the file of every bundled app (app registry), else what is
   missing ("/system/apps is missing", "0 apps found in /system/apps", the path). */
const char *sysfiles_apps_missing(void);

/* ---- uifiles.c ---------------------------------------------------------------------------- */
/* Write /system/ui (fonts, themes, wallpapers, UI settings) from the built-in copies. */
void uifiles_seed(void);
/* Load fonts, wallpapers, themes and UI settings from /system/ui. */
void uifiles_load(void);
/* The UI files the drive must have: uifiles_path(0 .. uifiles_count()-1, out). */
int  uifiles_count(void);
void uifiles_path(int i, char *out);
/* theme.c: draw wallpaper `index` from its loaded recipe (false: none loaded) and
   put the loaded theme's colours over the built-in ones. */
bool uifiles_render_wallpaper(surface_t *s, int index);
void uifiles_theme_override(theme_t *t);
/* theme.c: the built-in colours of light / dark mode (the seed of the theme files). */
void theme_builtin_colors(bool dark, theme_t *out);

#endif
