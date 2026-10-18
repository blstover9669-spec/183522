/* widget_internal.h - widget object layout shared by widgets.c and textarea.c */
#ifndef WIDGET_INTERNAL_H
#define WIDGET_INTERNAL_H

#include "wm_internal.h"

#define LIST_ROW_H 26
#define SB_W 12
#define MAX_MENUS 10

typedef struct menu_def {
    char title[32];
    char *items;           /* '|' separated */
    ui_menu_cb cb;
    void *ctx;
    int x, w;              /* title position within the bar */
} menu_def_t;

struct widget {
    int type;
    window_t *win;
    rect_t r;
    bool visible, enabled, dead;
    char *text;
    widget_cb cb;  void *ctx;     /* main callback (click / change / select)  */
    widget_cb cb2; void *ctx2;    /* submit (textbox) / activate (listbox)     */
    int value, min, max;
    int total, page;              /* scrollbar */
    int group;                    /* radio group */
    const font_t *font;
    int align;
    uint32_t color;
    intptr_t tag;
    bool hover, pressed, is_default;
    /* text box */
    int cursor, anchor, scroll_x, maxlen;
    bool password;
    char *placeholder;
    /* lists */
    char **items;
    intptr_t *item_data;
    int nitems, cap;
    int scroll, hover_item;
    ui_list_icon_fn icon_fn;
    bool sb_drag;
    int sb_drag_off;
    /* menubar */
    menu_def_t menus[MAX_MENUS];
    int nmenus, open_menu;
    window_t *popup;
    /* image / custom */
    const surface_t *img;
    ui_custom_draw_fn draw_fn;
    ui_custom_event_fn event_fn;
    void *ext;                    /* type-specific extension (textarea) */
    struct widget *next;
};


extern bool ui_blink_on;
void      ui_fire(widget_t *w, widget_cb cb, void *ctx);
bool      ui_alive(widget_t *w);
widget_t *ui_widget_new(window_t *win, int type, int x, int y, int w, int h);
void      ui_start_blink(void);

/* textarea.c */
void textarea_paint(surface_t *s, widget_t *w);
bool textarea_event(widget_t *w, const event_t *ev);
void textarea_free(widget_t *w);
void textarea_set_text(widget_t *w, const char *text);
bool textarea_wants_tab(widget_t *w);

#endif
