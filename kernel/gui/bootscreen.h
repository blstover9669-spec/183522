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
 *                (kernel/fs/sysfiles.c)
 *   uifiles.c    loading the UI (fonts, wallpapers, themes, UI settings)
 *                from /system/ui at startup
 *
 * Boot order (kernel/core/kmain.c): splash background -> devices -> files
 * (installed drive: the saved files are restored; ISO: the system files are
 * written) -> drive check -> UI files loaded -> INIT_FS hooks -> splash animation -> [ISO: NovaOS Drive installation] ->
 * [installed: loading bar] apps, desktop -> desktop (-> ISO: save file screen).
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
/* After Continue ISO, once the desktop exists (before it fades in): the save file
   screen (OSaver's osaver_boot_prompt, if it is part of the build). */
void bootsetup_after_desktop(void);

/* ---- nodrive.c ----------------------------------------------------------------------- */
/* Check the drive at startup (No drive found if it is incomplete) and start watching
   it while NovaOS runs. */
void nodrive_check_startup(void);
/* Black screen, Nova icon, "No drive found" - forever. `why`: the missing path. */
void nodrive_show(const char *why) NORETURN;
/* Liberation Sans 30 px, ASCII (nodrive_font.c, tools/genfont.py --nodrive). */
extern const font_t font_nodrive;

/* ---- sysfiles.c (kernel/fs) ------------------------------------------------------------- */
/* Write the system files into /system from the built-in copies (ISO boot / first boot). */
void sysfiles_seed(void);
/* Bring the files that describe the running system (boot.cfg) up to date. */
void sysfiles_refresh(void);
/* NULL if the drive has all system files, else the first missing path. */
const char *sysfiles_missing(void);

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
