/*
 * nova/theme.h - global look & feel.
 *
 * Read colours from theme_get() each time you paint, so your app follows
 * dark/light mode and accent changes. Setters repaint every window (and send
 * EV_THEME to each one).
 */
#ifndef NOVA_THEME_H
#define NOVA_THEME_H

#include <nova/types.h>
#include <nova/gfx.h>

typedef struct theme {
    /* user settings */
    bool     dark;              /* dark mode                                   */
    uint32_t accent;            /* accent colour (selection, focus, buttons)    */
    int      wallpaper;         /* built-in wallpaper index, or -1 = custom image */
    uint32_t wallpaper_color;   /* colour for the "Solid colour" wallpaper      */
    int      font_scale;        /* reserved (always 1)                         */
    bool     shadows;           /* window drop shadows                         */

    /* derived colours (recomputed by the setters) */
    uint32_t window_bg;         /* client area background                      */
    uint32_t window_text;
    uint32_t window_border;
    uint32_t panel_bg;          /* slightly contrasting panels / cards          */
    uint32_t title_active;      /* title bar background (focused window)       */
    uint32_t title_inactive;
    uint32_t title_text;
    uint32_t title_text_inactive;
    uint32_t button_bg, button_hover, button_pressed, button_border, button_text;
    uint32_t field_bg, field_border, field_text;   /* text boxes, lists         */
    uint32_t selection_bg, selection_text;
    uint32_t disabled_text;
    uint32_t accent_hover, accent_text;            /* text drawn on accent      */
    uint32_t menu_bg, menu_text, menu_hover, menu_separator;
    uint32_t taskbar_bg, taskbar_text, taskbar_hover, taskbar_active;
    uint32_t desktop_text;                          /* icon labels               */
    uint32_t scrollbar_bg, scrollbar_thumb;
    uint32_t link;
} theme_t;

const theme_t *theme_get(void);
void        theme_set_dark(bool dark);
void        theme_set_accent(uint32_t accent);
void        theme_set_wallpaper(int index);
void        theme_set_wallpaper_color(uint32_t c);
/* Use an image as wallpaper (copied and scaled to cover the screen). */
void        theme_set_wallpaper_image(const surface_t *img);
void        theme_set_shadows(bool on);
int         theme_wallpaper_count(void);
const char *theme_wallpaper_name(int index);
/* Render built-in wallpaper `index` into s (thumbnails for a settings app). */
void        theme_render_wallpaper(surface_t *s, int index);
/* Accent presets for settings UIs. */
int         theme_accent_count(void);
uint32_t    theme_accent_preset(int index);
const char *theme_accent_name(int index);
/* Force a full repaint of every window and the desktop. */
void        theme_refresh(void);

#endif
