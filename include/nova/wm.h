/*
 * nova/wm.h - windows, events and timers.
 *
 * NovaOS is single-threaded and event driven. Your app creates windows and
 * installs an event handler; the kernel main loop calls the handler for
 * input, timers and repaint requests. Handlers must return quickly - never
 * block or busy-wait. Spread long work over EV_TIMER events.
 *
 * Painting model:
 *   - Every window owns a client surface (win_surface()). Draw into it in
 *     response to EV_PAINT, then return; the compositor copies it to screen.
 *   - win_invalidate() schedules an EV_PAINT before the next frame.
 *   - Alternatively draw at any time (e.g. on EV_TIMER) and call
 *     win_update() to push the changed area to the screen without EV_PAINT.
 *   - The surface pointer changes when the window is resized: always fetch it
 *     with win_surface() instead of caching it.
 */
#ifndef NOVA_WM_H
#define NOVA_WM_H

#include <nova/types.h>
#include <nova/gfx.h>
#include <nova/input.h>

typedef struct window window_t;
struct app_desc;

/* ---- events ------------------------------------------------------------- */
enum event_type {
    EV_NONE = 0,
    EV_PAINT,         /* redraw: x,y,w,h = invalid area (client coords); you may redraw everything */
    EV_KEY_DOWN,      /* key, mods, repeat                                       */
    EV_KEY_UP,        /* key, mods                                               */
    EV_CHAR,          /* ch = printable character (32..255), mods. Not sent with Ctrl/Alt held. */
    EV_MOUSE_MOVE,    /* x,y (client coords), buttons, mods                      */
    EV_MOUSE_DOWN,    /* x,y, button (which one), buttons, mods                  */
    EV_MOUSE_UP,      /* x,y, button, buttons, mods                              */
    EV_MOUSE_WHEEL,   /* x,y, wheel (+1 = scroll down / towards user, -1 = up)   */
    EV_DOUBLE_CLICK,  /* x,y, button - sent after the second EV_MOUSE_DOWN       */
    EV_MOUSE_LEAVE,   /* pointer left the client area                            */
    EV_TIMER,         /* timer_id                                                */
    EV_RESIZE,        /* w,h = new client size (surface already reallocated)     */
    EV_FOCUS,         /* window became the active window                         */
    EV_BLUR,          /* window lost activation                                  */
    EV_CLOSE_REQUEST, /* only with WIN_CONFIRM_CLOSE: user asked to close; call win_close() to accept */
    EV_CLOSE,         /* window is being destroyed: free your per-window state   */
    EV_THEME,         /* theme changed (a repaint is also scheduled)             */
    EV_USER,          /* custom event posted with win_post_event(); see data     */
};

typedef struct event {
    int type;
    int x, y;        /* mouse position in client coordinates / PAINT area origin */
    int w, h;        /* RESIZE new client size / PAINT area size                  */
    int button;      /* MOUSE_DOWN/UP/DOUBLE_CLICK: MOUSE_LEFT/RIGHT/MIDDLE       */
    int buttons;     /* mouse buttons currently held (bitmask)                    */
    int wheel;       /* MOUSE_WHEEL delta                                         */
    int key;         /* KEY_DOWN/KEY_UP keycode (KEY_* / ASCII)                   */
    int mods;        /* MOD_* modifier mask at the time of the event              */
    int ch;          /* CHAR: the character                                       */
    int repeat;      /* KEY_DOWN: nonzero for typematic auto-repeat               */
    int timer_id;    /* TIMER: id returned by win_set_timer()                     */
    intptr_t data;   /* USER: payload                                             */
} event_t;

typedef void (*win_event_fn)(window_t *win, const event_t *ev);

/* ---- window flags --------------------------------------------------------- */
#define WIN_RESIZABLE     0x0001  /* user can resize by dragging the border      */
#define WIN_NO_MAXIMIZE   0x0002  /* hide the maximize button                    */
#define WIN_NO_MINIMIZE   0x0004  /* hide the minimize button                    */
#define WIN_CENTER        0x0008  /* centre on screen (default: cascade)         */
#define WIN_CONFIRM_CLOSE 0x0010  /* close button sends EV_CLOSE_REQUEST          */
#define WIN_TOPMOST       0x0020  /* stays above normal windows                  */
#define WIN_NO_TASKBAR    0x0040  /* no taskbar button                           */
#define WIN_FRAMELESS     0x0080  /* no title bar / border                       */
#define WIN_POPUP         0x0100  /* frameless menu-like window above everything;
                                     closed automatically by a click outside it   */
#define WIN_DIALOG        0x0200  /* dialog: no min/max buttons, no taskbar button */
#define WIN_HIDDEN        0x0400  /* create hidden; call win_show()              */
#define WIN_NO_SHADOW     0x0800  /* no drop shadow                              */
#define WIN_FIXED         0       /* (default) fixed size                        */

/* Decoration metrics (for layout maths). */
#define WIN_TITLE_H  32
#define WIN_BORDER   1

/* ---- window API ----------------------------------------------------------- */
window_t   *win_create(const char *title, int client_w, int client_h, uint32_t flags);
/* Create a dialog owned by `owner` (centred over it; owner ignores input while open). */
window_t   *win_create_dialog(window_t *owner, const char *title, int client_w, int client_h, uint32_t flags);
void        win_set_event_handler(window_t *win, win_event_fn fn);
void        win_set_userdata(window_t *win, void *data);
void       *win_get_userdata(window_t *win);
surface_t  *win_surface(window_t *win);
int         win_width(window_t *win);     /* client width  */
int         win_height(window_t *win);    /* client height */
void        win_invalidate(window_t *win);
void        win_invalidate_rect(window_t *win, int x, int y, int w, int h);
void        win_update(window_t *win, int x, int y, int w, int h);   /* w or h <= 0: whole client */
void        win_set_title(window_t *win, const char *title);
const char *win_get_title(window_t *win);
void        win_close(window_t *win);     /* sends EV_CLOSE, destroys after the handler returns */
void        win_show(window_t *win);
void        win_hide(window_t *win);
void        win_resize(window_t *win, int client_w, int client_h);
void        win_set_min_size(window_t *win, int client_w, int client_h);
void        win_move(window_t *win, int screen_x, int screen_y);   /* frame top-left */
rect_t      win_frame_rect(window_t *win);                         /* screen coords  */
rect_t      win_client_rect(window_t *win);                        /* screen coords  */
void        win_focus(window_t *win);     /* raise + activate (restores if minimized) */
void        win_minimize(window_t *win);
void        win_maximize(window_t *win);
void        win_restore(window_t *win);
bool        win_is_focused(window_t *win);
bool        win_is_minimized(window_t *win);
bool        win_is_maximized(window_t *win);
bool        win_is_valid(window_t *win);  /* still open? (safe on stale pointers) */
void        win_set_cursor(window_t *win, int cursor);   /* CURSOR_* while over the client */
uint32_t    win_flags(window_t *win);
const struct app_desc *win_get_app(window_t *win);
window_t   *win_get_owner(window_t *win);
/* Queue an EV_USER event (data = payload) for delivery from the main loop. */
void        win_post_event(window_t *win, intptr_t data);

/* Timers: repeating, delivered as EV_TIMER. Return id > 0 (0 on failure). */
int         win_set_timer(window_t *win, uint32_t interval_ms);
void        win_kill_timer(window_t *win, int timer_id);   /* timer_id <= 0: all timers of win */

/* Window enumeration (for task managers / taskbars): normal app windows in
   z-order, bottom first. Returns count (may exceed max). */
int         win_list(window_t **out, int max);
window_t   *win_get_focused(void);
int         win_id(window_t *win);
window_t   *win_find_by_id(int id);

/* ---- global timers (not tied to a window), run from the main loop -------- */
typedef void (*timer_fn)(void *ctx);
int  timer_add(uint32_t ms, timer_fn fn, void *ctx, bool repeat);  /* id > 0 */
void timer_cancel(int id);

/* ---- screen --------------------------------------------------------------- */
int  screen_width(void);
int  screen_height(void);
int  screen_work_height(void);   /* height above the taskbar */
/* Copy of the current screen contents (for screenshots). Free with surface_destroy. */
surface_t *screen_capture(void);

#endif
