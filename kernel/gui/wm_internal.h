/* wm_internal.h - window manager internals shared by the files in kernel/gui */
#ifndef WM_INTERNAL_H
#define WM_INTERNAL_H

#include "kernel.h"

#define WIN_MAGIC 0x57494E44u   /* 'WIND' */

enum { LAYER_DESKTOP = 0, LAYER_NORMAL = 1, LAYER_TOPMOST = 2, LAYER_POPUP = 3 };

/* hit-test results */
enum {
    HT_NONE = 0, HT_CLIENT, HT_TITLE, HT_CLOSE, HT_MAXIMIZE, HT_MINIMIZE,
    HT_LEFT, HT_RIGHT, HT_TOP, HT_BOTTOM, HT_TOPLEFT, HT_TOPRIGHT, HT_BOTTOMLEFT, HT_BOTTOMRIGHT,
    HT_BORDER
};

struct widget;

struct window {
    uint32_t magic;
    int id;
    uint32_t flags;
    int layer;
    rect_t frame;              /* screen rect including decorations */
    int cw, ch;                /* client size */
    int min_cw, min_ch;
    rect_t normal_frame;       /* restore rect when maximized */
    bool visible, minimized, maximized;
    bool closing;              /* EV_CLOSE sent; destroyed at end of loop iteration */
    bool no_callbacks;         /* dialogs: owner died, do not fire callbacks */
    char title[128];
    surface_t *surf;           /* whole window */
    surface_t client;          /* view onto the client area of surf */
    int cx, cy;                /* client origin inside surf */
    win_event_fn handler;
    void *userdata;
    const app_desc_t *app;
    window_t *owner;
    window_t *modal;           /* dialog blocking this window */
    window_t *below, *above;   /* z-order (bottom -> top) */
    bool need_paint;
    rect_t paint_rect;         /* client coords */
    bool deco_dirty;
    int cursor;
    int widget_cursor;         /* cursor requested by the widget under the mouse (0 = none) */
    int hover_ht;              /* title button under the mouse */
    int pressed_ht;
    int radius;                /* corner radius used by the compositor */
    int opacity;               /* 0..255; < 255 while fading in */
    bool fullscreen;           /* covers the whole screen, no decorations */
    rect_t fs_saved_frame;     /* frame before going fullscreen */
    bool fs_saved_max;
    uint32_t fade_start;       /* g_ticks when the fade-in began */
    bool mouse_inside;
    /* widgets */
    struct widget *widgets;
    struct widget *focus_widget;
    struct widget *hover_widget;
    struct widget *capture_widget;
    /* internal hooks for shell windows */
    void (*on_destroy)(window_t *w);
};

extern window_t *wm_bottom, *wm_top;          /* z-order list ends */
extern window_t *wm_focused;
extern surface_t *wm_backbuf;
extern const app_desc_t *wm_current_app;      /* app context for new windows */
extern int wm_mouse_x, wm_mouse_y, wm_mouse_buttons;
extern int wm_taskbar_h;
extern uint32_t wm_click_serial;              /* incremented on every mouse-down */

/* WM core */
void      wm_send(window_t *w, const event_t *ev);
void      wm_raise(window_t *w);
rect_t    wm_visual_rect(window_t *w);         /* frame + shadow */
void      wm_invalidate_window(window_t *w);   /* whole visual rect to screen */
void      wm_invalidate_client_rect(window_t *w, rect_t r);  /* client coords -> screen */
void      wm_redraw_decor(window_t *w);
window_t *wm_window_at(int x, int y);
void      wm_close_popups(window_t *except);
bool      wm_any_popup(void);
window_t *wm_create_ex(const char *title, int cw, int ch, uint32_t flags, int layer);
void      wm_set_frame(window_t *w, rect_t frame);
void      wm_focus(window_t *w);
void      wm_update_taskbar_visibility(void);
void      wm_taskbar_changed(void);
uint32_t  wm_frame_serial(void);
bool      wm_screenshot(void);

/* timers */
void      timers_remove_window(window_t *w);

/* widgets (widgets.c) */
bool      ui_dispatch(window_t *w, const event_t *ev);   /* true if consumed */
void      ui_paint_all(window_t *w);
void      ui_destroy_all(window_t *w);
bool      ui_wants_tab(window_t *w);
window_t *ui_popup_menu_nested(window_t *owner, int sx, int sy, const char *items, ui_menu_cb cb, void *ctx);

/* cursor (cursor.c) */
void      cursor_draw(surface_t *dst, int x, int y, int shape);
rect_t    cursor_rect(int x, int y, int shape);
extern int wm_cursor_shape;

/* desktop / shell */
extern window_t *g_desktop_win, *g_taskbar_win;
extern surface_t *g_wallpaper;               /* rendered wallpaper, screen size */
void      theme_render_current_wallpaper(void);
void      desktop_wallpaper_changed(void);
void      taskbar_wallpaper_changed(void);
void      startmenu_open(void);
void      shell_autorun(void);
bool      startmenu_just_closed(void);
void      shell_note_launch(const app_desc_t *app, const char *arg);   /* kernel/shell/open.c */

/* apps */
void      app_icon_cache_clear(void);
void      default_icon(surface_t *s, int x, int y, int size, const char *name);

/* misc */
void      notify_init(void);

#endif
