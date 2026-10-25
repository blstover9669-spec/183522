/*
 * nova/screensaver.h - screensaver styles and their animations.
 *
 * The screensaver service (nova/service.h: screensaver_set_timeout,
 * screensaver_activate ...) shows the chosen style full screen. The style is
 * persisted with the timeout in /system/config/screensaver ("style=<key>").
 *
 *   1. Nova Drop           the Nova icon drops from the top centre and bounces,
 *                          each bounce smaller; gone after 4 s, then again
 *   2. Original            3D stars + aurora with the clock (the NovaOS 1.5 one)
 *   3. Bubbles             shiny glass bubbles float up and wobble
 *   4. Bouncing App Logos  the apps' icons bounce around the screen
 *   5. Lines               like the classic Pipes: lines grow, turn at right
 *                          angles, clear and restart when the screen is full
 *
 * Animations draw into any surface size, so Settings can show a live preview:
 *
 *   ss_anim_t *a = ss_anim_create(style, 248, 186, true);
 *   every ss_anim_interval(a) ms:  n = ss_anim_frame(a, surf, first, rects, 16);
 *   ss_anim_destroy(a);
 *
 * Not included by <nova.h>: #include <nova/screensaver.h>.
 */
#ifndef NOVA_SCREENSAVER_H
#define NOVA_SCREENSAVER_H

#include <nova/types.h>
#include <nova/gfx.h>

enum {
    SS_STYLE_NOVA_DROP = 0,
    SS_STYLE_ORIGINAL,
    SS_STYLE_BUBBLES,
    SS_STYLE_LOGOS,
    SS_STYLE_LINES,
    SS_STYLE_COUNT
};

int         screensaver_style_count(void);
const char *screensaver_style_name(int style);    /* "Nova Drop" ...                      */
const char *screensaver_style_key(int style);     /* "nova-drop" ... (config file value)  */
int         screensaver_get_style(void);
void        screensaver_set_style(int style);     /* persisted                            */

typedef struct ss_anim ss_anim_t;

/* An animation of `style` for a w x h surface. preview: scale everything to the
   surface (a live thumbnail); otherwise it is the full-screen screensaver. */
ss_anim_t *ss_anim_create(int style, int w, int h, bool preview);
/* Advance to the current time and draw into dst (w x h). full: dst does not hold
   the previous frame (first frame, or it was overwritten). Writes the changed
   areas to dirty (at most max, merged when there are more) and returns their count. */
int        ss_anim_frame(ss_anim_t *a, surface_t *dst, bool full, rect_t *dirty, int max);
int        ss_anim_interval(const ss_anim_t *a);  /* ms between frames */
void       ss_anim_destroy(ss_anim_t *a);

#endif
