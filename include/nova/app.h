/*
 * nova/app.h - application registry.
 *
 * Every app is one (or more) .c files under apps/. It describes itself with
 * REGISTER_APP(...), which places a pointer to an app_desc_t in the ".apps"
 * linker section. The desktop, Start menu, terminal and file manager discover
 * apps from there - no shared file needs editing to add an app.
 *
 *   static void my_launch(const char *arg) { ... win_create(...) ... }
 *   static void my_icon(surface_t *s, int x, int y, int size) { ... }
 *   REGISTER_APP(.name = "Clock", .category = "Accessories",
 *                .description = "Analog clock", .launch = my_launch,
 *                .draw_icon = my_icon);
 */
#ifndef NOVA_APP_H
#define NOVA_APP_H

#include <nova/types.h>
#include <nova/gfx.h>

/* App categories used by the Start menu (any string works; these are sorted first). */
#define CAT_ACCESSORIES "Accessories"
#define CAT_GAMES       "Games"
#define CAT_GRAPHICS    "Graphics"
#define CAT_MULTIMEDIA  "Multimedia"
#define CAT_OFFICE      "Office"
#define CAT_INTERNET    "Internet"
#define CAT_DEVELOPMENT "Development"
#define CAT_SYSTEM      "System"
#define CAT_UTILITIES   "Utilities"

/* app_desc_t.flags */
#define APP_HIDDEN      0x1   /* not shown on the desktop or in the Start menu  */
#define APP_NO_DESKTOP  0x2   /* Start menu only, no desktop icon               */
#define APP_SINGLE      0x4   /* launching again focuses the existing window    */

typedef struct app_desc {
    const char *name;         /* unique display name, used by app_launch_by_name()  */
    const char *category;     /* CAT_* or any string                                 */
    const char *description;  /* one-line description (tooltips, Start menu)         */
    /* Called to start the app. arg is NULL or a string (often a file path).
       Typically creates a window and returns immediately. */
    void (*launch)(const char *arg);
    /* Draw a size x size icon with top-left at (x, y). Draw on a transparent
       background; the result is cached and anti-aliased (rendered at 2x then
       downsampled), so plain gfx primitives look smooth. May be NULL. */
    void (*draw_icon)(surface_t *s, int x, int y, int size);
    const char *file_types;   /* optional "txt,md,log" - extensions this app opens   */
    uint32_t flags;           /* APP_*                                               */
} app_desc_t;

#define _APP_CAT2(a, b) a##b
#define _APP_CAT(a, b) _APP_CAT2(a, b)
/* REGISTER_APP(.name = ..., .launch = ..., ...) - designated initialisers. */
#define REGISTER_APP(...)                                                        \
    static const app_desc_t _APP_CAT(_app_desc_, __LINE__) = { __VA_ARGS__ };    \
    static const app_desc_t *const _APP_CAT(_app_ptr_, __LINE__)                 \
        __attribute__((used, section(".apps"), aligned(4))) = &_APP_CAT(_app_desc_, __LINE__)

int               app_count(void);                 /* registered apps            */
const app_desc_t *app_get(int index);              /* sorted by name             */
const app_desc_t *app_find(const char *name);      /* case-insensitive           */
bool              app_launch(const app_desc_t *app, const char *arg);
bool              app_launch_by_name(const char *name, const char *arg);
/* App registered for a file's extension (first match by name order), or NULL. */
const app_desc_t *app_for_file(const char *path);
/* Open a file with its associated app. Returns false if none. */
bool              app_open_file(const char *path);
/* Number of windows currently open for an app. */
int               app_window_count(const app_desc_t *app);
/* Cached, anti-aliased icon image (SURF_ALPHA) at the given size. Do not free. */
const surface_t  *app_icon(const app_desc_t *app, int size);
void              app_draw_icon(surface_t *dst, const app_desc_t *app, int x, int y, int size);
/* Distinct categories of visible apps, Start-menu order. Returns count. */
int               app_categories(const char **out, int max);

#endif
