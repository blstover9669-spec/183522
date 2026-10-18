/*
 * nova/ui.h - widget toolkit, dialogs and themed drawing helpers.
 *
 * Widgets are retained objects attached to a window. The window manager
 * draws them after your EV_PAINT handler has drawn the background, and routes
 * mouse/keyboard events to them before your handler sees the event (events a
 * widget consumes are not passed on). Widgets are destroyed automatically
 * with their window.
 *
 * Coordinates are client-relative. Callbacks run from the main loop.
 */
#ifndef NOVA_UI_H
#define NOVA_UI_H

#include <nova/types.h>
#include <nova/gfx.h>
#include <nova/wm.h>

typedef struct widget widget_t;
typedef void (*widget_cb)(widget_t *w, void *ctx);
typedef void (*ui_menu_cb)(int item, void *ctx);

enum widget_type {
    UI_BUTTON = 1, UI_LABEL, UI_TEXTBOX, UI_CHECKBOX, UI_RADIO, UI_SLIDER,
    UI_PROGRESS, UI_LISTBOX, UI_SCROLLBAR, UI_DROPDOWN, UI_TABS, UI_MENUBAR,
    UI_IMAGE, UI_CUSTOM, UI_TEXTAREA
};

/* ---- creation ------------------------------------------------------------- */
widget_t *ui_button(window_t *win, int x, int y, int w, int h, const char *label,
                    widget_cb on_click, void *ctx);
widget_t *ui_label(window_t *win, int x, int y, int w, int h, const char *text);
widget_t *ui_textbox(window_t *win, int x, int y, int w, int h, const char *text);
widget_t *ui_checkbox(window_t *win, int x, int y, int w, int h, const char *label,
                      bool checked, widget_cb on_change, void *ctx);
/* Radio buttons with the same group number in a window are mutually exclusive. */
widget_t *ui_radio(window_t *win, int x, int y, int w, int h, const char *label,
                   int group, bool checked, widget_cb on_change, void *ctx);
widget_t *ui_slider(window_t *win, int x, int y, int w, int h, int min, int max, int value,
                    widget_cb on_change, void *ctx);
widget_t *ui_progress(window_t *win, int x, int y, int w, int h, int min, int max, int value);
widget_t *ui_listbox(window_t *win, int x, int y, int w, int h, widget_cb on_select, void *ctx);
widget_t *ui_scrollbar(window_t *win, int x, int y, int w, int h, bool vertical,
                       widget_cb on_change, void *ctx);
widget_t *ui_dropdown(window_t *win, int x, int y, int w, int h, widget_cb on_change, void *ctx);
widget_t *ui_tabs(window_t *win, int x, int y, int w, int h, widget_cb on_change, void *ctx);
/* Image widget: shows a surface (not copied - keep it alive), scaled to fit if needed. */
widget_t *ui_image(window_t *win, int x, int y, int w, int h, const surface_t *img);
/* Menu bar across the top of the client area (height UI_MENUBAR_H). */
widget_t *ui_menubar(window_t *win);
#define UI_MENUBAR_H 26
/* Add a menu: items separated by '|', "-" is a separator, a leading '!' greys an item.
   cb receives the item index (separators count). Returns menu index. */
int       ui_menubar_add(widget_t *menubar, const char *title, const char *items,
                         ui_menu_cb cb, void *ctx);
/* Custom widget: draw/event callbacks of your own (event returns true if consumed). */
typedef void (*ui_custom_draw_fn)(widget_t *w, surface_t *s, rect_t r);
typedef bool (*ui_custom_event_fn)(widget_t *w, const event_t *ev);
widget_t *ui_custom(window_t *win, int x, int y, int w, int h,
                    ui_custom_draw_fn draw, ui_custom_event_fn event, void *ctx);

/* ---- common operations ----------------------------------------------------------- */
void        ui_set_text(widget_t *w, const char *text);
const char *ui_get_text(widget_t *w);
/* value: checkbox/radio 0|1, slider/progress/scrollbar position,
   listbox/dropdown/tabs selected index (-1 = none). */
int         ui_get_value(widget_t *w);
void        ui_set_value(widget_t *w, int value);    /* does not fire callbacks */
void        ui_set_range(widget_t *w, int min, int max);
void        ui_set_enabled(widget_t *w, bool enabled);
void        ui_set_visible(widget_t *w, bool visible);
bool        ui_is_enabled(widget_t *w);
bool        ui_is_visible(widget_t *w);
void        ui_move(widget_t *w, int x, int y, int width, int height);
rect_t      ui_rect(widget_t *w);
void        ui_focus(widget_t *w);
bool        ui_has_focus(widget_t *w);
void        ui_destroy(widget_t *w);
void        ui_set_callback(widget_t *w, widget_cb cb, void *ctx);
void       *ui_get_ctx(widget_t *w);
window_t   *ui_window(widget_t *w);
int         ui_type(widget_t *w);
void        ui_set_font(widget_t *w, const font_t *f);
void        ui_set_align(widget_t *w, int align);        /* labels: ALIGN_* flags */
void        ui_set_color(widget_t *w, uint32_t color);   /* label text colour (0 = theme) */
void        ui_set_tag(widget_t *w, intptr_t tag);        /* free-form user data */
intptr_t    ui_get_tag(widget_t *w);
void        ui_invalidate(widget_t *w);
/* Buttons: make this the default (accent coloured, triggered by Enter). */
void        ui_button_set_default(widget_t *w, bool is_default);

/* ---- text boxes ------------------------------------------------------------------------ */
void ui_textbox_set_placeholder(widget_t *w, const char *text);
void ui_textbox_set_password(widget_t *w, bool on);
void ui_textbox_set_maxlen(widget_t *w, int maxlen);
/* Called when Enter is pressed (the main callback fires on every edit). */
void ui_textbox_on_submit(widget_t *w, widget_cb cb, void *ctx);
void ui_textbox_select_all(widget_t *w);

/* ---- multi-line text areas ------------------------------------------------------------
 * A scrolling multi-line editor: caret, mouse/keyboard selection, clipboard,
 * undo/redo (Ctrl+Z / Ctrl+Y), Tab inserts a tab, optional line numbers.
 * Default font FONT_MONO (ui_set_font to change). ui_get_text/ui_set_text access
 * the whole text; the main callback (ui_set_callback) fires on every edit. */
widget_t   *ui_textarea(window_t *win, int x, int y, int w, int h, const char *text);
void        ui_textarea_set_readonly(widget_t *w, bool ro);
void        ui_textarea_set_line_numbers(widget_t *w, bool on);
void        ui_textarea_set_tab_width(widget_t *w, int spaces);
void        ui_textarea_insert(widget_t *w, const char *text);    /* at the caret, replaces selection */
void        ui_textarea_append(widget_t *w, const char *text);    /* at the end, scrolls to it (logs) */
int         ui_textarea_length(widget_t *w);
int         ui_textarea_line_count(widget_t *w);
void        ui_textarea_get_cursor(widget_t *w, int *line, int *col);   /* 0-based */
void        ui_textarea_set_cursor(widget_t *w, int line, int col);
void        ui_textarea_select(widget_t *w, int start, int end);       /* byte offsets */
void        ui_textarea_get_selection(widget_t *w, int *start, int *end);
/* Find text starting after the caret (wraps around); selects it. Returns false if not found. */
bool        ui_textarea_find(widget_t *w, const char *needle, bool match_case);
bool        ui_textarea_modified(widget_t *w);
void        ui_textarea_set_modified(widget_t *w, bool m);
void        ui_textarea_undo(widget_t *w);
void        ui_textarea_redo(widget_t *w);
/* Called when the caret moves (e.g. to show "Ln 3, Col 7" in a status bar). */
void        ui_textarea_on_cursor(widget_t *w, widget_cb cb, void *ctx);

/* ---- lists (listbox, dropdown, tabs) --------------------------------------------------- */
int         ui_list_add(widget_t *w, const char *text);   /* returns index */
void        ui_list_insert(widget_t *w, int index, const char *text);
void        ui_list_remove(widget_t *w, int index);
void        ui_list_clear(widget_t *w);
int         ui_list_count(widget_t *w);
const char *ui_list_get(widget_t *w, int index);
void        ui_list_set(widget_t *w, int index, const char *text);
void        ui_list_set_data(widget_t *w, int index, intptr_t data);
intptr_t    ui_list_get_data(widget_t *w, int index);
/* Listbox: double-click / Enter on an item. */
void        ui_list_on_activate(widget_t *w, widget_cb cb, void *ctx);
void        ui_list_scroll_to(widget_t *w, int index);
/* Listbox: optional per-item icon drawer (draws a 16x16 icon at x,y). */
typedef void (*ui_list_icon_fn)(widget_t *w, int index, surface_t *s, int x, int y);
void        ui_list_set_icon_fn(widget_t *w, ui_list_icon_fn fn);

/* ---- scrollbars ---------------------------------------------------------------- */
void ui_scrollbar_set(widget_t *w, int total, int page, int pos);  /* content size, visible size, offset */

/* ---- popup / context menus --------------------------------------------------------- */
/* Show a menu at client coords (x,y) of win. Items like ui_menubar_add. */
window_t *ui_popup_menu(window_t *win, int x, int y, const char *items, ui_menu_cb cb, void *ctx);
/* Same, at absolute screen coordinates (owner may be NULL). */
window_t *ui_popup_menu_at(window_t *owner, int sx, int sy, const char *items, ui_menu_cb cb, void *ctx);

/* ---- dialogs (asynchronous: they return immediately, the callback fires later) ---- */
#define MB_OK          0
#define MB_OKCANCEL    1
#define MB_YESNO       2
#define MB_YESNOCANCEL 3
#define MB_ICON_INFO     0x00
#define MB_ICON_WARNING  0x10
#define MB_ICON_ERROR    0x20
#define MB_ICON_QUESTION 0x30

#define MB_RESULT_OK     1
#define MB_RESULT_CANCEL 2
#define MB_RESULT_YES    3
#define MB_RESULT_NO     4

typedef void (*ui_msgbox_cb)(int result, void *ctx);
typedef void (*ui_input_cb)(const char *text, void *ctx);   /* text == NULL if cancelled */
typedef void (*ui_file_cb)(const char *path, void *ctx);    /* path == NULL if cancelled */

/* owner may be NULL. cb may be NULL. Closing the dialog with X gives CANCEL (or NO). */
window_t *ui_message_box(window_t *owner, const char *title, const char *text, int type,
                         ui_msgbox_cb cb, void *ctx);
window_t *ui_input_box(window_t *owner, const char *title, const char *prompt,
                       const char *initial, ui_input_cb cb, void *ctx);
#define UI_FILE_OPEN 0
#define UI_FILE_SAVE 1
#define UI_FILE_DIR  2   /* choose a directory */
/* exts: comma separated filter ("txt,md") or NULL for all files. start: dir or file path. */
window_t *ui_file_dialog(window_t *owner, int mode, const char *title, const char *start,
                         const char *exts, ui_file_cb cb, void *ctx);
/* Toast notification in the bottom-right corner (disappears after a few seconds). */
void      ui_notify(const char *title, const char *text);
/* Dialog callbacks are NOT called if the owner window is closed first. */

/* ---- clipboard (text) ----------------------------------------------------------------- */
void        clipboard_set(const char *text);
const char *clipboard_get(void);    /* never NULL ("" when empty) */

/* ---- themed drawing helpers for custom controls -------------------------------------- */
#define UI_STATE_HOVER    0x1
#define UI_STATE_PRESSED  0x2
#define UI_STATE_FOCUSED  0x4
#define UI_STATE_DISABLED 0x8
#define UI_STATE_DEFAULT  0x10   /* accent coloured (default button) */
#define UI_STATE_CHECKED  0x20

void ui_draw_button(surface_t *s, rect_t r, const char *label, int state);
void ui_draw_panel(surface_t *s, rect_t r);            /* raised card                */
void ui_draw_field(surface_t *s, rect_t r, int state); /* text-field style background */
void ui_draw_checkbox(surface_t *s, int x, int y, int state);   /* 16x16 box          */
void ui_draw_separator(surface_t *s, int x, int y, int w);
void ui_draw_scrollbar(surface_t *s, rect_t r, bool vertical, int total, int page, int pos, int state);
/* Small vector glyphs used by the UI. */
enum { UI_GLYPH_CHECK, UI_GLYPH_CLOSE, UI_GLYPH_MINIMIZE, UI_GLYPH_MAXIMIZE, UI_GLYPH_RESTORE,
       UI_GLYPH_ARROW_UP, UI_GLYPH_ARROW_DOWN, UI_GLYPH_ARROW_LEFT, UI_GLYPH_ARROW_RIGHT,
       UI_GLYPH_INFO, UI_GLYPH_WARNING, UI_GLYPH_ERROR, UI_GLYPH_QUESTION,
       UI_GLYPH_FOLDER, UI_GLYPH_FILE };
void ui_draw_glyph(surface_t *s, int glyph, int x, int y, int size, uint32_t color);
/* The NovaOS logo (gradient tile with a star) in a size x size box. */
void ui_draw_logo(surface_t *s, int x, int y, int size);

#endif
