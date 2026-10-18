/*
 * widgets.c - the NovaOS widget toolkit: buttons, labels, text boxes,
 * checkboxes, radio buttons, sliders, progress bars, list boxes, scroll bars,
 * dropdowns, tabs, menu bars, images, custom widgets and popup menus.
 */
#include "widget_internal.h"

static int blink_timer;
static void blink_tick(void *ctx);
static void blink_tick_fn(void *ctx) { blink_tick(ctx); }
bool ui_blink_on = true;

/* ---- helpers ------------------------------------------------------------------- */
static const font_t *wfont(widget_t *w) { return w->font ? w->font : FONT_UI; }

void ui_invalidate(widget_t *w) {
    if (!w || !w->win) return;
    win_invalidate_rect(w->win, w->r.x - 4, w->r.y - 4, w->r.w + 8, w->r.h + 8);
}

static void set_text_raw(widget_t *w, const char *t) {
    kfree(w->text);
    w->text = strdup(t ? t : "");
}

static bool alive(widget_t *w) { return w && !w->dead && w->win && !w->win->closing; }

static void fire(widget_t *w, widget_cb cb, void *ctx) {
    if (cb && alive(w)) cb(w, ctx);
}

void ui_fire(widget_t *w, widget_cb cb, void *ctx) { fire(w, cb, ctx); }
bool ui_alive(widget_t *w) { return alive(w); }
void ui_start_blink(void) { if (!blink_timer) blink_timer = timer_add(530, blink_tick_fn, NULL, true); }

static void blink_tick(void *ctx) {
    (void)ctx;
    ui_blink_on = !ui_blink_on;
    window_t *f = wm_focused;
    if (f && f->focus_widget && (f->focus_widget->type == UI_TEXTBOX || f->focus_widget->type == UI_TEXTAREA) && !f->focus_widget->dead)
        ui_invalidate(f->focus_widget);
}

static widget_t *widget_new(window_t *win, int type, int x, int y, int w, int h) {
    if (!win) return NULL;
    widget_t *wd = kcalloc(1, sizeof *wd);
    if (!wd) return NULL;
    wd->type = type;
    wd->win = win;
    wd->r = rect_make(x, y, w, h);
    wd->visible = true;
    wd->enabled = true;
    wd->text = strdup("");
    wd->hover_item = -1;
    wd->open_menu = -1;
    wd->align = ALIGN_LEFT | ALIGN_MIDDLE;
    /* append (draw order = creation order) */
    widget_t **pp = &win->widgets;
    while (*pp) pp = &(*pp)->next;
    *pp = wd;
    ui_invalidate(wd);
    return wd;
}

widget_t *ui_widget_new(window_t *win, int type, int x, int y, int w, int h) { return widget_new(win, type, x, y, w, h); }

static void widget_free(widget_t *w) {
    if (w->type == UI_TEXTAREA) textarea_free(w);
    kfree(w->text);
    kfree(w->placeholder);
    for (int i = 0; i < w->nitems; i++) kfree(w->items[i]);
    kfree(w->items);
    kfree(w->item_data);
    for (int i = 0; i < w->nmenus; i++) kfree(w->menus[i].items);
    kfree(w);
}

static void sweep_dead(window_t *win) {
    widget_t **pp = &win->widgets;
    while (*pp) {
        widget_t *w = *pp;
        if (w->dead) { *pp = w->next; widget_free(w); }
        else pp = &w->next;
    }
}

void ui_destroy_all(window_t *win) {
    widget_t *w = win->widgets;
    while (w) { widget_t *n = w->next; widget_free(w); w = n; }
    win->widgets = NULL;
    win->focus_widget = win->hover_widget = win->capture_widget = NULL;
}

static bool focusable(widget_t *w) {
    if (!w->visible || !w->enabled || w->dead) return false;
    switch (w->type) {
    case UI_BUTTON: case UI_TEXTBOX: case UI_CHECKBOX: case UI_RADIO: case UI_SLIDER:
    case UI_LISTBOX: case UI_DROPDOWN: case UI_TABS: case UI_CUSTOM: case UI_TEXTAREA:
        return true;
    default:
        return false;
    }
}

/* ---- creation ------------------------------------------------------------------- */
widget_t *ui_button(window_t *win, int x, int y, int w, int h, const char *label, widget_cb cb, void *ctx) {
    widget_t *wd = widget_new(win, UI_BUTTON, x, y, w, h);
    if (!wd) return NULL;
    set_text_raw(wd, label);
    wd->cb = cb; wd->ctx = ctx;
    return wd;
}

widget_t *ui_label(window_t *win, int x, int y, int w, int h, const char *text) {
    widget_t *wd = widget_new(win, UI_LABEL, x, y, w, h);
    if (!wd) return NULL;
    set_text_raw(wd, text);
    return wd;
}

widget_t *ui_textbox(window_t *win, int x, int y, int w, int h, const char *text) {
    widget_t *wd = widget_new(win, UI_TEXTBOX, x, y, w, h);
    if (!wd) return NULL;
    set_text_raw(wd, text);
    wd->cursor = wd->anchor = (int)strlen(wd->text);
    wd->maxlen = 1023;
    if (!blink_timer) blink_timer = timer_add(530, blink_tick, NULL, true);
    return wd;
}

widget_t *ui_checkbox(window_t *win, int x, int y, int w, int h, const char *label, bool checked, widget_cb cb, void *ctx) {
    widget_t *wd = widget_new(win, UI_CHECKBOX, x, y, w, h);
    if (!wd) return NULL;
    set_text_raw(wd, label);
    wd->value = checked;
    wd->cb = cb; wd->ctx = ctx;
    return wd;
}

widget_t *ui_radio(window_t *win, int x, int y, int w, int h, const char *label, int group, bool checked, widget_cb cb, void *ctx) {
    widget_t *wd = widget_new(win, UI_RADIO, x, y, w, h);
    if (!wd) return NULL;
    set_text_raw(wd, label);
    wd->group = group;
    wd->value = checked;
    wd->cb = cb; wd->ctx = ctx;
    return wd;
}

widget_t *ui_slider(window_t *win, int x, int y, int w, int h, int min, int max, int value, widget_cb cb, void *ctx) {
    widget_t *wd = widget_new(win, UI_SLIDER, x, y, w, h);
    if (!wd) return NULL;
    wd->min = min; wd->max = max > min ? max : min + 1;
    wd->value = CLAMP(value, wd->min, wd->max);
    wd->cb = cb; wd->ctx = ctx;
    return wd;
}

widget_t *ui_progress(window_t *win, int x, int y, int w, int h, int min, int max, int value) {
    widget_t *wd = widget_new(win, UI_PROGRESS, x, y, w, h);
    if (!wd) return NULL;
    wd->min = min; wd->max = max > min ? max : min + 1;
    wd->value = CLAMP(value, wd->min, wd->max);
    return wd;
}

widget_t *ui_listbox(window_t *win, int x, int y, int w, int h, widget_cb cb, void *ctx) {
    widget_t *wd = widget_new(win, UI_LISTBOX, x, y, w, h);
    if (!wd) return NULL;
    wd->value = -1;
    wd->cb = cb; wd->ctx = ctx;
    return wd;
}

widget_t *ui_scrollbar(window_t *win, int x, int y, int w, int h, bool vertical, widget_cb cb, void *ctx) {
    widget_t *wd = widget_new(win, UI_SCROLLBAR, x, y, w, h);
    if (!wd) return NULL;
    wd->group = vertical;
    wd->total = 100; wd->page = 10;
    wd->cb = cb; wd->ctx = ctx;
    return wd;
}

widget_t *ui_dropdown(window_t *win, int x, int y, int w, int h, widget_cb cb, void *ctx) {
    widget_t *wd = widget_new(win, UI_DROPDOWN, x, y, w, h);
    if (!wd) return NULL;
    wd->value = -1;
    wd->cb = cb; wd->ctx = ctx;
    return wd;
}

widget_t *ui_tabs(window_t *win, int x, int y, int w, int h, widget_cb cb, void *ctx) {
    widget_t *wd = widget_new(win, UI_TABS, x, y, w, h);
    if (!wd) return NULL;
    wd->value = -1;
    wd->cb = cb; wd->ctx = ctx;
    return wd;
}

widget_t *ui_image(window_t *win, int x, int y, int w, int h, const surface_t *img) {
    widget_t *wd = widget_new(win, UI_IMAGE, x, y, w, h);
    if (!wd) return NULL;
    wd->img = img;
    return wd;
}

widget_t *ui_menubar(window_t *win) {
    widget_t *wd = widget_new(win, UI_MENUBAR, 0, 0, win ? win->cw : 0, UI_MENUBAR_H);
    return wd;
}

widget_t *ui_custom(window_t *win, int x, int y, int w, int h, ui_custom_draw_fn draw, ui_custom_event_fn event, void *ctx) {
    widget_t *wd = widget_new(win, UI_CUSTOM, x, y, w, h);
    if (!wd) return NULL;
    wd->draw_fn = draw; wd->event_fn = event; wd->ctx = ctx;
    return wd;
}

int ui_menubar_add(widget_t *mb, const char *title, const char *items, ui_menu_cb cb, void *ctx) {
    if (!mb || mb->type != UI_MENUBAR || mb->nmenus >= MAX_MENUS) return -1;
    menu_def_t *m = &mb->menus[mb->nmenus];
    strlcpy(m->title, title, sizeof m->title);
    m->items = strdup(items ? items : "");
    m->cb = cb; m->ctx = ctx;
    int x = 4;
    for (int i = 0; i < mb->nmenus; i++) x = mb->menus[i].x + mb->menus[i].w;
    m->x = x;
    m->w = font_text_width(FONT_UI, title) + 20;
    ui_invalidate(mb);
    return mb->nmenus++;
}

/* ---- generic operations ----------------------------------------------------------- */
void ui_set_text(widget_t *w, const char *text) {
    if (!w) return;
    if (w->type == UI_TEXTAREA) { textarea_set_text(w, text); return; }
    set_text_raw(w, text);
    if (w->type == UI_TEXTBOX) {
        int n = (int)strlen(w->text);
        w->cursor = w->anchor = n;
        w->scroll_x = 0;
    }
    ui_invalidate(w);
}

const char *ui_get_text(widget_t *w) {
    if (!w) return "";
    if ((w->type == UI_DROPDOWN || w->type == UI_LISTBOX || w->type == UI_TABS) && w->value >= 0 && w->value < w->nitems)
        return w->items[w->value];
    return w->text ? w->text : "";
}

int ui_get_value(widget_t *w) { return w ? w->value : 0; }

static int scroll_max(widget_t *w) {
    if (w->type == UI_SCROLLBAR) return MAX(0, w->total - w->page);
    if (w->type == UI_LISTBOX) {
        int vis = (w->r.h - 4) / LIST_ROW_H;
        return MAX(0, w->nitems - vis);
    }
    return 0;
}

void ui_set_value(widget_t *w, int v) {
    if (!w) return;
    switch (w->type) {
    case UI_SLIDER: case UI_PROGRESS: v = CLAMP(v, w->min, w->max); break;
    case UI_SCROLLBAR: v = CLAMP(v, 0, scroll_max(w)); break;
    case UI_LISTBOX: case UI_DROPDOWN: case UI_TABS: v = (v < 0 || v >= w->nitems) ? -1 : v; break;
    case UI_CHECKBOX: case UI_RADIO: v = v ? 1 : 0; break;
    default: break;
    }
    if (w->type == UI_RADIO && v) {
        for (widget_t *o = w->win->widgets; o; o = o->next)
            if (o != w && o->type == UI_RADIO && o->group == w->group && o->value) { o->value = 0; ui_invalidate(o); }
    }
    w->value = v;
    if (w->type == UI_LISTBOX && v >= 0) ui_list_scroll_to(w, v);
    ui_invalidate(w);
}

void ui_set_range(widget_t *w, int min, int max) {
    if (!w) return;
    w->min = min;
    w->max = max > min ? max : min + 1;
    w->value = CLAMP(w->value, w->min, w->max);
    ui_invalidate(w);
}

void ui_set_enabled(widget_t *w, bool e) { if (w && w->enabled != e) { w->enabled = e; ui_invalidate(w); } }
void ui_set_visible(widget_t *w, bool v) {
    if (!w || w->visible == v) return;
    ui_invalidate(w);
    w->visible = v;
    if (!v && w->win->focus_widget == w) w->win->focus_widget = NULL;
}
bool ui_is_enabled(widget_t *w) { return w && w->enabled; }
bool ui_is_visible(widget_t *w) { return w && w->visible; }

void ui_move(widget_t *w, int x, int y, int width, int height) {
    if (!w) return;
    ui_invalidate(w);
    w->r = rect_make(x, y, width, height);
    if (w->type == UI_LISTBOX) w->scroll = CLAMP(w->scroll, 0, scroll_max(w));
    ui_invalidate(w);
}

rect_t ui_rect(widget_t *w) { return w ? w->r : rect_make(0, 0, 0, 0); }

void ui_focus(widget_t *w) {
    if (!w || !w->win) return;
    widget_t *old = w->win->focus_widget;
    if (old == w) return;
    w->win->focus_widget = w;
    if (old) ui_invalidate(old);
    ui_blink_on = true;
    ui_invalidate(w);
}

bool ui_has_focus(widget_t *w) { return w && w->win && w->win->focus_widget == w; }

void ui_destroy(widget_t *w) {
    if (!w || w->dead) return;
    if (w->popup && win_is_valid(w->popup)) win_close(w->popup);
    w->popup = NULL;
    ui_invalidate(w);
    w->dead = true;
    w->visible = false;
    window_t *win = w->win;
    if (win->focus_widget == w) win->focus_widget = NULL;
    if (win->hover_widget == w) win->hover_widget = NULL;
    if (win->capture_widget == w) win->capture_widget = NULL;
}

void ui_set_callback(widget_t *w, widget_cb cb, void *ctx) { if (w) { w->cb = cb; w->ctx = ctx; } }
void *ui_get_ctx(widget_t *w) { return w ? w->ctx : NULL; }
window_t *ui_window(widget_t *w) { return w ? w->win : NULL; }
int ui_type(widget_t *w) { return w ? w->type : 0; }
void ui_set_font(widget_t *w, const font_t *f) { if (w) { w->font = f; ui_invalidate(w); } }
void ui_set_align(widget_t *w, int a) { if (w) { w->align = a; ui_invalidate(w); } }
void ui_set_color(widget_t *w, uint32_t c) { if (w) { w->color = c; ui_invalidate(w); } }
void ui_set_tag(widget_t *w, intptr_t tag) { if (w) w->tag = tag; }
intptr_t ui_get_tag(widget_t *w) { return w ? w->tag : 0; }
void ui_button_set_default(widget_t *w, bool d) { if (w) { w->is_default = d; ui_invalidate(w); } }

void ui_textbox_set_placeholder(widget_t *w, const char *t) {
    if (!w) return;
    kfree(w->placeholder);
    w->placeholder = strdup(t ? t : "");
    ui_invalidate(w);
}
void ui_textbox_set_password(widget_t *w, bool on) { if (w) { w->password = on; ui_invalidate(w); } }
void ui_textbox_set_maxlen(widget_t *w, int n) { if (w) w->maxlen = n > 0 ? n : 1023; }
void ui_textbox_on_submit(widget_t *w, widget_cb cb, void *ctx) { if (w) { w->cb2 = cb; w->ctx2 = ctx; } }
void ui_textbox_select_all(widget_t *w) {
    if (!w) return;
    w->anchor = 0;
    w->cursor = (int)strlen(w->text);
    ui_invalidate(w);
}

/* ---- list items -------------------------------------------------------------------- */
void ui_list_insert(widget_t *w, int index, const char *text) {
    if (!w) return;
    if (w->nitems == w->cap) {
        int nc = w->cap ? w->cap * 2 : 16;
        char **ni = krealloc(w->items, sizeof(char *) * (size_t)nc);
        if (!ni) return;
        w->items = ni;
        intptr_t *nd = krealloc(w->item_data, sizeof(intptr_t) * (size_t)nc);
        if (!nd) return;
        w->item_data = nd;
        w->cap = nc;
    }
    index = CLAMP(index, 0, w->nitems);
    memmove(&w->items[index + 1], &w->items[index], sizeof(char *) * (size_t)(w->nitems - index));
    memmove(&w->item_data[index + 1], &w->item_data[index], sizeof(intptr_t) * (size_t)(w->nitems - index));
    w->items[index] = strdup(text ? text : "");
    w->item_data[index] = 0;
    w->nitems++;
    if (w->value >= index && w->value >= 0 && w->nitems > 1) w->value++;
    if (w->type == UI_TABS && w->value < 0) w->value = 0;
    ui_invalidate(w);
}

int ui_list_add(widget_t *w, const char *text) {
    if (!w) return -1;
    ui_list_insert(w, w->nitems, text);
    return w->nitems - 1;
}

void ui_list_remove(widget_t *w, int i) {
    if (!w || i < 0 || i >= w->nitems) return;
    kfree(w->items[i]);
    memmove(&w->items[i], &w->items[i + 1], sizeof(char *) * (size_t)(w->nitems - i - 1));
    memmove(&w->item_data[i], &w->item_data[i + 1], sizeof(intptr_t) * (size_t)(w->nitems - i - 1));
    w->nitems--;
    if (w->value == i) w->value = -1;
    else if (w->value > i) w->value--;
    w->scroll = CLAMP(w->scroll, 0, scroll_max(w));
    ui_invalidate(w);
}

void ui_list_clear(widget_t *w) {
    if (!w) return;
    for (int i = 0; i < w->nitems; i++) kfree(w->items[i]);
    w->nitems = 0;
    w->value = -1;
    w->scroll = 0;
    w->hover_item = -1;
    ui_invalidate(w);
}

int ui_list_count(widget_t *w) { return w ? w->nitems : 0; }
const char *ui_list_get(widget_t *w, int i) { return (w && i >= 0 && i < w->nitems) ? w->items[i] : NULL; }
void ui_list_set(widget_t *w, int i, const char *t) {
    if (!w || i < 0 || i >= w->nitems) return;
    kfree(w->items[i]);
    w->items[i] = strdup(t ? t : "");
    ui_invalidate(w);
}
void ui_list_set_data(widget_t *w, int i, intptr_t d) { if (w && i >= 0 && i < w->nitems) w->item_data[i] = d; }
intptr_t ui_list_get_data(widget_t *w, int i) { return (w && i >= 0 && i < w->nitems) ? w->item_data[i] : 0; }
void ui_list_on_activate(widget_t *w, widget_cb cb, void *ctx) { if (w) { w->cb2 = cb; w->ctx2 = ctx; } }
void ui_list_set_icon_fn(widget_t *w, ui_list_icon_fn fn) { if (w) { w->icon_fn = fn; ui_invalidate(w); } }

void ui_list_scroll_to(widget_t *w, int i) {
    if (!w || w->type != UI_LISTBOX) return;
    int vis = MAX(1, (w->r.h - 4) / LIST_ROW_H);
    if (i < w->scroll) w->scroll = i;
    else if (i >= w->scroll + vis) w->scroll = i - vis + 1;
    w->scroll = CLAMP(w->scroll, 0, scroll_max(w));
    ui_invalidate(w);
}

void ui_scrollbar_set(widget_t *w, int total, int page, int pos) {
    if (!w) return;
    w->total = MAX(0, total);
    w->page = MAX(1, page);
    w->value = CLAMP(pos, 0, scroll_max(w));
    ui_invalidate(w);
}

/* ---- clipboard ------------------------------------------------------------------------ */
static char *clip_text;
void clipboard_set(const char *t) { kfree(clip_text); clip_text = strdup(t ? t : ""); }
const char *clipboard_get(void) { return clip_text ? clip_text : ""; }

/* ---- popup menus ------------------------------------------------------------------------ */
/* Popup closes are recorded so a click on the opener does not immediately reopen it. */
static uint32_t popup_closed_serial;
static widget_t *popup_closed_owner;
static int popup_closed_menu = -1;

static void note_popup_closed(widget_t *owner, int menu) {
    popup_closed_owner = owner;
    popup_closed_serial = wm_click_serial;
    popup_closed_menu = menu;
}

typedef struct {
    char *items;          /* copy, '|' separated */
    int count;
    int hover;
    ui_menu_cb cb;
    void *ctx;
    widget_t *owner_widget;   /* menubar/dropdown that opened it */
    int check;            /* dropdown: index to mark as selected */
} popup_menu_t;

#define MENU_ITEM_H 30
#define MENU_SEP_H 9
#define MENU_PAD 6

static const char *menu_item(const char *items, int idx, int *len) {
    const char *p = items;
    for (int i = 0; i < idx; i++) {
        p = strchr(p, '|');
        if (!p) { *len = 0; return ""; }
        p++;
    }
    const char *e = strchr(p, '|');
    *len = e ? (int)(e - p) : (int)strlen(p);
    return p;
}

static int menu_count(const char *items) {
    if (!items || !*items) return 0;
    int n = 1;
    for (const char *p = items; *p; p++) if (*p == '|') n++;
    return n;
}

static bool item_is_sep(const char *s, int len) { return len == 1 && s[0] == '-'; }

static int menu_item_y(popup_menu_t *m, int idx) {
    int y = MENU_PAD;
    for (int i = 0; i < idx; i++) {
        int len;
        const char *s = menu_item(m->items, i, &len);
        y += item_is_sep(s, len) ? MENU_SEP_H : MENU_ITEM_H;
    }
    return y;
}

static int menu_hit(popup_menu_t *m, int y) {
    int yy = MENU_PAD;
    for (int i = 0; i < m->count; i++) {
        int len;
        const char *s = menu_item(m->items, i, &len);
        int h = item_is_sep(s, len) ? MENU_SEP_H : MENU_ITEM_H;
        if (y >= yy && y < yy + h) return (item_is_sep(s, len) || (len && s[0] == '!')) ? -1 : i;
        yy += h;
    }
    return -1;
}

static void menu_select(window_t *win, popup_menu_t *m, int idx) {
    ui_menu_cb cb = m->cb;
    void *ctx = m->ctx;
    widget_t *ow = m->owner_widget;
    m->cb = NULL;
    win_close(win);
    if (ow && ow->type == UI_DROPDOWN && alive(ow)) {
        ow->value = idx;
        ui_invalidate(ow);
        fire(ow, ow->cb, ow->ctx);
        return;
    }
    if (cb) cb(idx, ctx);
}

static void menu_step(popup_menu_t *m, int dir) {
    int i = m->hover;
    for (int n = 0; n < m->count; n++) {
        i = (i + dir + m->count) % m->count;
        int len;
        const char *s = menu_item(m->items, i, &len);
        if (!item_is_sep(s, len) && !(len && s[0] == '!')) { m->hover = i; return; }
    }
}

static void menu_handler(window_t *win, const event_t *ev) {
    popup_menu_t *m = win_get_userdata(win);
    if (!m) return;
    const theme_t *t = theme_get();
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        gfx_clear(s, t->menu_bg);
        gfx_rounded_rect(s, 0, 0, s->w, s->h, 8, color_mix(t->menu_bg, t->window_border, 140));
        for (int i = 0; i < m->count; i++) {
            int len;
            const char *it = menu_item(m->items, i, &len);
            int y = menu_item_y(m, i);
            if (item_is_sep(it, len)) { ui_draw_separator(s, 10, y + MENU_SEP_H / 2, s->w - 20); continue; }
            bool disabled = len && it[0] == '!';
            if (disabled) { it++; len--; }
            if (i == m->hover && !disabled) gfx_fill_rounded_rect(s, 4, y, s->w - 8, MENU_ITEM_H, 5, t->menu_hover);
            uint32_t fg = disabled ? t->disabled_text : t->menu_text;
            if (m->check == i) ui_draw_glyph(s, UI_GLYPH_CHECK, 12, y + 9, 12, t->accent);
            /* "Label\tShortcut" support */
            const char *tab = memchr(it, '\t', (size_t)len);
            int l1 = tab ? (int)(tab - it) : len;
            gfx_text_n(s, 32, y + (MENU_ITEM_H - font_height(FONT_UI)) / 2, it, l1, fg, FONT_UI);
            if (tab) {
                int l2 = len - l1 - 1;
                int tw = font_text_width_n(FONT_UI, tab + 1, l2);
                gfx_text_n(s, s->w - 16 - tw, y + (MENU_ITEM_H - font_height(FONT_UI)) / 2, tab + 1, l2, t->disabled_text, FONT_UI);
            }
        }
        break;
    }
    case EV_MOUSE_MOVE: {
        int h = menu_hit(m, ev->y);
        if (ev->x < 0 || ev->x >= win_width(win)) h = -1;
        if (h != m->hover) { m->hover = h; win_invalidate(win); }
        break;
    }
    case EV_MOUSE_LEAVE:
        if (m->hover != -1) { m->hover = -1; win_invalidate(win); }
        break;
    case EV_MOUSE_UP: {
        int h = menu_hit(m, ev->y);
        if (h >= 0 && ev->x >= 0 && ev->x < win_width(win)) menu_select(win, m, h);
        break;
    }
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESCAPE) win_close(win);
        else if (ev->key == KEY_DOWN) { menu_step(m, 1); win_invalidate(win); }
        else if (ev->key == KEY_UP) { menu_step(m, -1); win_invalidate(win); }
        else if (ev->key == KEY_ENTER && m->hover >= 0) menu_select(win, m, m->hover);
        break;
    case EV_CLOSE:
        if (m->owner_widget && alive(m->owner_widget)) {
            widget_t *ow = m->owner_widget;
            note_popup_closed(ow, ow->type == UI_MENUBAR ? ow->open_menu : -1);
            if (ow->type == UI_MENUBAR) { ow->open_menu = -1; ow->popup = NULL; ui_invalidate(ow); }
            if (ow->type == UI_DROPDOWN) { ow->popup = NULL; ow->pressed = false; ui_invalidate(ow); }
        }
        kfree(m->items);
        kfree(m);
        win_set_userdata(win, NULL);
        break;
    default:
        break;
    }
}

static void popup_on_destroy(window_t *w) {
    (void)w;
}

static window_t *popup_menu_create(window_t *owner, int sx, int sy, const char *items, ui_menu_cb cb, void *ctx,
                                   widget_t *ow, int min_w, int check) {
    popup_menu_t *m = kcalloc(1, sizeof *m);
    if (!m) return NULL;
    m->items = strdup(items ? items : "");
    m->count = menu_count(m->items);
    m->hover = -1;
    m->cb = cb;
    m->ctx = ctx;
    m->owner_widget = ow;
    m->check = check;
    int w = MAX(min_w, 160), h = MENU_PAD * 2;
    for (int i = 0; i < m->count; i++) {
        int len;
        const char *s = menu_item(m->items, i, &len);
        if (item_is_sep(s, len)) { h += MENU_SEP_H; continue; }
        h += MENU_ITEM_H;
        const char *tab = memchr(s, '\t', (size_t)len);
        int tw = font_text_width_n(FONT_UI, s, len) + (tab ? 40 : 0);
        w = MAX(w, tw + 56);
    }
    w = MIN(w, screen_width());
    if (sx + w > screen_width()) sx = screen_width() - w - 2;
    if (sy + h > screen_height() - wm_taskbar_h) sy = MAX(0, sy - h - (ow ? ow->r.h : 0));
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    const app_desc_t *saved = wm_current_app;
    if (owner) wm_current_app = owner->app;
    window_t *pw = wm_create_ex("menu", w, h, WIN_POPUP | WIN_NO_TASKBAR, LAYER_POPUP);
    wm_current_app = saved;
    if (!pw) { kfree(m->items); kfree(m); return NULL; }
    pw->owner = owner;
    pw->on_destroy = popup_on_destroy;
    win_set_userdata(pw, m);
    win_set_event_handler(pw, menu_handler);
    win_move(pw, sx, sy);
    return pw;
}

window_t *ui_popup_menu_at(window_t *owner, int sx, int sy, const char *items, ui_menu_cb cb, void *ctx) {
    wm_close_popups(NULL);
    return popup_menu_create(owner, sx, sy, items, cb, ctx, NULL, 0, -1);
}

window_t *ui_popup_menu(window_t *win, int x, int y, const char *items, ui_menu_cb cb, void *ctx) {
    rect_t c = win_client_rect(win);
    return ui_popup_menu_at(win, c.x + x, c.y + y, items, cb, ctx);
}

static void menubar_open(widget_t *mb, int idx) {
    if (mb->popup && win_is_valid(mb->popup)) win_close(mb->popup);
    mb->popup = NULL;
    menu_def_t *m = &mb->menus[idx];
    rect_t c = win_client_rect(mb->win);
    mb->open_menu = idx;
    window_t *pw = popup_menu_create(mb->win, c.x + mb->r.x + m->x, c.y + mb->r.y + mb->r.h, m->items, m->cb, m->ctx, mb, 0, -1);
    mb->popup = pw;
    if (!pw) mb->open_menu = -1;
    ui_invalidate(mb);
}

static void dropdown_open(widget_t *dd) {
    if (!dd->nitems) return;
    size_t len = 0;
    for (int i = 0; i < dd->nitems; i++) len += strlen(dd->items[i]) + 1;
    char *buf = kmalloc(len + 1);
    if (!buf) return;
    buf[0] = 0;
    for (int i = 0; i < dd->nitems; i++) {
        if (i) strcat(buf, "|");
        strcat(buf, dd->items[i][0] == '-' && !dd->items[i][1] ? " -" : dd->items[i]);
    }
    rect_t c = win_client_rect(dd->win);
    dd->popup = popup_menu_create(dd->win, c.x + dd->r.x, c.y + dd->r.y + dd->r.h + 2, buf, NULL, NULL, dd, dd->r.w, dd->value);
    kfree(buf);
}

/* ---- painting ----------------------------------------------------------------------- */
static int widget_state(widget_t *w) {
    int st = 0;
    if (w->hover) st |= UI_STATE_HOVER;
    if (w->pressed && w->hover) st |= UI_STATE_PRESSED;
    if (w->win->focus_widget == w && w->win == wm_focused) st |= UI_STATE_FOCUSED;
    if (!w->enabled) st |= UI_STATE_DISABLED;
    return st;
}

static void paint_textbox(surface_t *s, widget_t *w) {
    const theme_t *t = theme_get();
    int st = widget_state(w);
    rect_t r = w->r;
    ui_draw_field(s, r, st & (UI_STATE_FOCUSED | UI_STATE_HOVER | UI_STATE_DISABLED));
    const font_t *f = wfont(w);
    rect_t inner = rect_make(r.x + 8, r.y + 2, r.w - 16, r.h - 4);
    rect_t old = s->clip;
    surface_set_clip(s, rect_intersect(old, inner));
    int ty = r.y + (r.h - font_height(f)) / 2;
    const char *txt = w->text;
    char *masked = NULL;
    if (w->password) {
        int n = (int)strlen(txt);
        masked = kmalloc((size_t)n + 1);
        if (masked) { memset(masked, '*', (size_t)n); masked[n] = 0; txt = masked; }
    }
    bool focused = st & UI_STATE_FOCUSED;
    if (!*txt && w->placeholder && !focused) {
        gfx_text(s, inner.x, ty, w->placeholder, t->disabled_text, f);
    } else {
        int x0 = inner.x - w->scroll_x;
        int a = MIN(w->cursor, w->anchor), b = MAX(w->cursor, w->anchor);
        if (a != b && focused) {
            int sx = x0 + font_text_width_n(f, txt, a);
            int sw = font_text_width_n(f, txt + a, b - a);
            gfx_fill_rect(s, sx, r.y + 4, sw, r.h - 8, color_alpha(t->selection_bg, 110));
        }
        gfx_text(s, x0, ty, txt, w->enabled ? t->field_text : t->disabled_text, f);
        if (focused && ui_blink_on) {
            int cx = x0 + font_text_width_n(f, txt, w->cursor);
            gfx_fill_rect(s, cx, ty, 1, font_height(f), t->field_text);
        }
    }
    s->clip = old;
    kfree(masked);
}

static int list_visible_rows(widget_t *w) { return MAX(1, (w->r.h - 4) / LIST_ROW_H); }

static void paint_listbox(surface_t *s, widget_t *w) {
    const theme_t *t = theme_get();
    rect_t r = w->r;
    ui_draw_field(s, r, (widget_state(w) & UI_STATE_FOCUSED) ? 0 : 0);
    int vis = list_visible_rows(w);
    bool sb = w->nitems > vis;
    int iw = r.w - 4 - (sb ? SB_W : 0);
    rect_t old = s->clip;
    surface_set_clip(s, rect_intersect(old, rect_make(r.x + 2, r.y + 2, r.w - 4, r.h - 4)));
    bool focused = w->win->focus_widget == w && w->win == wm_focused;
    for (int i = 0; i < vis + 1 && w->scroll + i < w->nitems; i++) {
        int idx = w->scroll + i;
        int y = r.y + 2 + i * LIST_ROW_H;
        uint32_t fg = t->field_text;
        if (idx == w->value) {
            gfx_fill_rounded_rect(s, r.x + 4, y + 1, iw - 4, LIST_ROW_H - 2, 4,
                                  focused ? t->selection_bg : color_mix(t->field_bg, t->selection_bg, 90));
            fg = focused ? t->selection_text : t->field_text;
        } else if (idx == w->hover_item) {
            gfx_fill_rounded_rect(s, r.x + 4, y + 1, iw - 4, LIST_ROW_H - 2, 4, color_mix(t->field_bg, t->field_text, 18));
        }
        int tx = r.x + 12;
        if (w->icon_fn) { w->icon_fn(w, idx, s, tx, y + (LIST_ROW_H - 16) / 2); tx += 24; }
        gfx_text_ellipsis(s, tx, y + (LIST_ROW_H - font_height(wfont(w))) / 2, r.x + iw - tx - 4, w->items[idx], fg, wfont(w));
    }
    s->clip = old;
    if (sb) ui_draw_scrollbar(s, rect_make(r.x + r.w - SB_W - 2, r.y + 2, SB_W, r.h - 4), true, w->nitems, vis, w->scroll,
                              w->sb_drag ? UI_STATE_PRESSED : 0);
}

static void paint_slider(surface_t *s, widget_t *w) {
    const theme_t *t = theme_get();
    rect_t r = w->r;
    bool vert = r.h > r.w;
    int range = w->max - w->min;
    int st = widget_state(w);
    uint32_t acc = w->enabled ? t->accent : t->disabled_text;
    uint32_t track = color_mix(t->field_border, t->window_bg, 60);
    if (!vert) {
        int x0 = r.x + 10, x1 = r.x + r.w - 10, cy = r.y + r.h / 2;
        int px = x0 + (int)((int64_t)(x1 - x0) * (w->value - w->min) / range);
        gfx_fill_rounded_rect(s, x0, cy - 2, x1 - x0, 4, 2, track);
        gfx_fill_rounded_rect(s, x0, cy - 2, px - x0, 4, 2, acc);
        gfx_fill_circle_aa(s, px + 0.5, cy + 0.5, 9.5, color_mix(t->field_border, t->window_bg, 40));
        gfx_fill_circle_aa(s, px + 0.5, cy + 0.5, 8.5, t->field_bg);
        gfx_fill_circle_aa(s, px + 0.5, cy + 0.5, (st & (UI_STATE_HOVER | UI_STATE_PRESSED)) ? 6 : 5, acc);
        if (st & UI_STATE_FOCUSED) gfx_circle_aa(s, px + 0.5, cy + 0.5, 11.5, 1.2, color_alpha(t->accent, 150));
    } else {
        int y0 = r.y + 10, y1 = r.y + r.h - 10, cx = r.x + r.w / 2;
        int py = y1 - (int)((int64_t)(y1 - y0) * (w->value - w->min) / range);
        gfx_fill_rounded_rect(s, cx - 2, y0, 4, y1 - y0, 2, track);
        gfx_fill_rounded_rect(s, cx - 2, py, 4, y1 - py, 2, acc);
        gfx_fill_circle_aa(s, cx + 0.5, py + 0.5, 9.5, color_mix(t->field_border, t->window_bg, 40));
        gfx_fill_circle_aa(s, cx + 0.5, py + 0.5, 8.5, t->field_bg);
        gfx_fill_circle_aa(s, cx + 0.5, py + 0.5, (st & (UI_STATE_HOVER | UI_STATE_PRESSED)) ? 6 : 5, acc);
    }
}

static void paint_widget(surface_t *s, widget_t *w) {
    const theme_t *t = theme_get();
    rect_t r = w->r;
    int st = widget_state(w);
    switch (w->type) {
    case UI_LABEL: {
        uint32_t c = w->color ? w->color : (w->enabled ? t->window_text : t->disabled_text);
        const font_t *f = wfont(w);
        if (!strchr(w->text, '\n') && font_text_width(f, w->text) <= r.w)
            gfx_text_aligned(s, r, w->text, c, f, w->align);
        else
            gfx_text_wrapped(s, r, w->text, c, f);
        break;
    }
    case UI_BUTTON:
        ui_draw_button(s, r, w->text, st | (w->is_default ? UI_STATE_DEFAULT : 0));
        break;
    case UI_CHECKBOX:
        ui_draw_checkbox(s, r.x, r.y + (r.h - 18) / 2, (st & ~UI_STATE_PRESSED) | (w->value ? UI_STATE_CHECKED : 0));
        gfx_text_aligned(s, rect_make(r.x + 26, r.y, r.w - 26, r.h), w->text, w->enabled ? t->window_text : t->disabled_text, wfont(w), ALIGN_LEFT | ALIGN_MIDDLE);
        break;
    case UI_RADIO: {
        double cx = r.x + 9, cy = r.y + r.h / 2.0;
        if (w->value) {
            gfx_fill_circle_aa(s, cx, cy, 9, w->enabled ? t->accent : t->disabled_text);
            gfx_fill_circle_aa(s, cx, cy, (st & UI_STATE_HOVER) ? 4.5 : 3.8, t->accent_text);
        } else {
            gfx_fill_circle_aa(s, cx, cy, 9, (st & UI_STATE_HOVER) ? color_shade(t->field_border, t->dark ? 40 : -45) : t->field_border);
            gfx_fill_circle_aa(s, cx, cy, 8, t->field_bg);
        }
        if (st & UI_STATE_FOCUSED) gfx_circle_aa(s, cx, cy, 11.5, 1.2, color_alpha(t->accent, 150));
        gfx_text_aligned(s, rect_make(r.x + 26, r.y, r.w - 26, r.h), w->text, w->enabled ? t->window_text : t->disabled_text, wfont(w), ALIGN_LEFT | ALIGN_MIDDLE);
        break;
    }
    case UI_SLIDER:
        paint_slider(s, w);
        break;
    case UI_PROGRESS: {
        int range = w->max - w->min;
        int fw = (int)((int64_t)r.w * (w->value - w->min) / range);
        int rad = MIN(r.h / 2, 4);
        gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, rad, color_mix(t->field_border, t->window_bg, 80));
        if (fw > 0) gfx_fill_rounded_rect(s, r.x, r.y, MAX(fw, rad * 2), r.h, rad, t->accent);
        break;
    }
    case UI_TEXTBOX:
        paint_textbox(s, w);
        break;
    case UI_LISTBOX:
        paint_listbox(s, w);
        break;
    case UI_SCROLLBAR:
        ui_draw_scrollbar(s, r, w->group, w->total, w->page, w->value, (w->sb_drag ? UI_STATE_PRESSED : 0) | (w->hover ? UI_STATE_HOVER : 0));
        break;
    case UI_DROPDOWN: {
        ui_draw_button(s, r, NULL, st);
        const char *txt = (w->value >= 0 && w->value < w->nitems) ? w->items[w->value] : "";
        gfx_text_ellipsis(s, r.x + 10, r.y + (r.h - font_height(wfont(w))) / 2, r.w - 36, txt,
                          w->enabled ? t->button_text : t->disabled_text, wfont(w));
        ui_draw_glyph(s, UI_GLYPH_ARROW_DOWN, r.x + r.w - 24, r.y + (r.h - 12) / 2, 12, t->button_text);
        break;
    }
    case UI_TABS: {
        int x = r.x;
        gfx_hline(s, r.x, r.y + r.h - 1, r.w, color_mix(t->window_bg, t->window_border, 120));
        for (int i = 0; i < w->nitems; i++) {
            const font_t *f = (i == w->value) ? FONT_UI_BOLD : FONT_UI;
            int tw = font_text_width(f, w->items[i]) + 28;
            if (i == w->hover_item && i != w->value)
                gfx_fill_rounded_rect(s, x + 2, r.y + 3, tw - 4, r.h - 7, 5, color_mix(t->window_bg, t->window_text, 16));
            gfx_text_aligned(s, rect_make(x, r.y, tw, r.h - 2), w->items[i], i == w->value ? t->window_text : color_mix(t->window_text, t->window_bg, 80), f, ALIGN_CENTER | ALIGN_MIDDLE);
            if (i == w->value) gfx_fill_rounded_rect(s, x + 10, r.y + r.h - 3, tw - 20, 3, 1, t->accent);
            x += tw;
        }
        break;
    }
    case UI_MENUBAR: {
        w->r.w = w->win->cw;
        r = w->r;
        gfx_fill_rect(s, r.x, r.y, r.w, r.h, t->window_bg);
        gfx_hline(s, r.x, r.y + r.h - 1, r.w, color_mix(t->window_bg, t->window_border, 90));
        for (int i = 0; i < w->nmenus; i++) {
            menu_def_t *m = &w->menus[i];
            if (i == w->open_menu || i == w->hover_item)
                gfx_fill_rounded_rect(s, r.x + m->x, r.y + 2, m->w, r.h - 5, 4,
                                      i == w->open_menu ? color_mix(t->window_bg, t->accent, 60) : color_mix(t->window_bg, t->window_text, 20));
            gfx_text_aligned(s, rect_make(r.x + m->x, r.y, m->w, r.h - 1), m->title, t->window_text, FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
        }
        break;
    }
    case UI_IMAGE:
        if (w->img) {
            int iw = w->img->w, ih = w->img->h;
            if (iw <= r.w && ih <= r.h) {
                if (w->img->flags & SURF_ALPHA) gfx_blit_alpha(s, r.x + (r.w - iw) / 2, r.y + (r.h - ih) / 2, w->img, 255);
                else gfx_blit(s, r.x + (r.w - iw) / 2, r.y + (r.h - ih) / 2, w->img);
            } else {
                int dw = r.w, dh = (int)((int64_t)ih * r.w / iw);
                if (dh > r.h) { dh = r.h; dw = (int)((int64_t)iw * r.h / ih); }
                gfx_blit_scaled(s, r.x + (r.w - dw) / 2, r.y + (r.h - dh) / 2, dw, dh, w->img, 0, 0, iw, ih, true);
            }
        }
        break;
    case UI_CUSTOM:
        if (w->draw_fn) w->draw_fn(w, s, r);
        break;
    case UI_TEXTAREA:
        textarea_paint(s, w);
        break;
    default:
        break;
    }
}

void ui_paint_all(window_t *win) {
    sweep_dead(win);
    surface_t *s = &win->client;
    for (widget_t *w = win->widgets; w; w = w->next) {
        if (!w->visible || w->dead) continue;
        if (!rect_overlaps(rect_inset(w->r, -4), s->clip)) continue;
        paint_widget(s, w);
    }
}

/* ---- text editing -------------------------------------------------------------------- */
static void tb_ensure_visible(widget_t *w) {
    const font_t *f = wfont(w);
    int inner = w->r.w - 16;
    int cx = font_text_width_n(f, w->text, w->cursor);
    if (cx - w->scroll_x > inner - 2) w->scroll_x = cx - inner + 2;
    if (cx - w->scroll_x < 0) w->scroll_x = cx;
    if (w->scroll_x < 0) w->scroll_x = 0;
}

static void tb_delete_sel(widget_t *w) {
    int a = MIN(w->cursor, w->anchor), b = MAX(w->cursor, w->anchor);
    if (a == b) return;
    memmove(w->text + a, w->text + b, strlen(w->text + b) + 1);
    w->cursor = w->anchor = a;
}

static void tb_insert(widget_t *w, const char *s) {
    tb_delete_sel(w);
    int len = (int)strlen(w->text), n = (int)strlen(s);
    if (len + n > w->maxlen) n = w->maxlen - len;
    if (n <= 0) return;
    char *nt = kmalloc((size_t)len + (size_t)n + 1);
    if (!nt) return;
    memcpy(nt, w->text, (size_t)w->cursor);
    memcpy(nt + w->cursor, s, (size_t)n);
    memcpy(nt + w->cursor + n, w->text + w->cursor, (size_t)(len - w->cursor) + 1);
    kfree(w->text);
    w->text = nt;
    w->cursor += n;
    w->anchor = w->cursor;
}

static int word_left(const char *t, int i) {
    while (i > 0 && t[i - 1] == ' ') i--;
    while (i > 0 && t[i - 1] != ' ') i--;
    return i;
}

static int word_right(const char *t, int i) {
    int n = (int)strlen(t);
    while (i < n && t[i] != ' ') i++;
    while (i < n && t[i] == ' ') i++;
    return i;
}

static bool textbox_key(widget_t *w, const event_t *ev) {
    int len = (int)strlen(w->text);
    bool shift = ev->mods & MOD_SHIFT, ctrl = ev->mods & MOD_CTRL;
    bool changed = false, moved = true;
    if (ev->type == EV_CHAR) {
        char s[2] = { (char)ev->ch, 0 };
        tb_insert(w, s);
        changed = true;
    } else {
        switch (ev->key) {
        case KEY_LEFT:
            if (!shift && !ctrl && w->cursor != w->anchor) w->cursor = MIN(w->cursor, w->anchor);
            else w->cursor = ctrl ? word_left(w->text, w->cursor) : MAX(0, w->cursor - 1);
            break;
        case KEY_RIGHT:
            if (!shift && !ctrl && w->cursor != w->anchor) w->cursor = MAX(w->cursor, w->anchor);
            else w->cursor = ctrl ? word_right(w->text, w->cursor) : MIN(len, w->cursor + 1);
            break;
        case KEY_HOME: w->cursor = 0; break;
        case KEY_END: w->cursor = len; break;
        case KEY_BACKSPACE:
            if (w->cursor != w->anchor) tb_delete_sel(w);
            else if (w->cursor > 0) {
                int from = ctrl ? word_left(w->text, w->cursor) : w->cursor - 1;
                memmove(w->text + from, w->text + w->cursor, (size_t)(len - w->cursor) + 1);
                w->cursor = from;
            }
            w->anchor = w->cursor;
            changed = true;
            break;
        case KEY_DELETE:
            if (w->cursor != w->anchor) tb_delete_sel(w);
            else if (w->cursor < len) {
                int to = ctrl ? word_right(w->text, w->cursor) : w->cursor + 1;
                memmove(w->text + w->cursor, w->text + to, (size_t)(len - to) + 1);
            }
            w->anchor = w->cursor;
            changed = true;
            break;
        case KEY_ENTER:
            if (!w->cb2) return false;   /* let the window / default button handle it */
            fire(w, w->cb2, w->ctx2);
            return true;
        default:
            moved = false;
            if (ctrl && (ev->key == 'a')) { w->anchor = 0; w->cursor = len; moved = true; break; }
            if (ctrl && (ev->key == 'c' || ev->key == 'x')) {
                int a = MIN(w->cursor, w->anchor), b = MAX(w->cursor, w->anchor);
                if (a != b && !w->password) {
                    char *c = strndup(w->text + a, (size_t)(b - a));
                    clipboard_set(c);
                    kfree(c);
                    if (ev->key == 'x') { tb_delete_sel(w); changed = true; }
                }
                moved = true;
                break;
            }
            if (ctrl && ev->key == 'v') {
                const char *c = clipboard_get();
                char buf[1024];
                int n = 0;
                for (int i = 0; c[i] && n < 1023; i++) if (c[i] != '\n' && c[i] != '\r') buf[n++] = c[i] == '\t' ? ' ' : c[i];
                buf[n] = 0;
                tb_insert(w, buf);
                changed = true;
                moved = true;
                break;
            }
            return false;
        }
        bool nav = ev->key == KEY_LEFT || ev->key == KEY_RIGHT || ev->key == KEY_HOME || ev->key == KEY_END;
        if (nav && !shift) w->anchor = w->cursor;
        (void)moved;
    }
    ui_blink_on = true;
    tb_ensure_visible(w);
    ui_invalidate(w);
    if (changed) fire(w, w->cb, w->ctx);
    return true;
}

/* ---- event routing ---------------------------------------------------------------------- */
static widget_t *widget_at(window_t *win, int x, int y) {
    widget_t *hit = NULL;
    for (widget_t *w = win->widgets; w; w = w->next)
        if (w->visible && !w->dead && rect_contains(w->r, x, y)) hit = w;
    return hit;
}

static void set_hover(window_t *win, widget_t *w) {
    widget_t *old = win->hover_widget;
    if (old == w) return;
    if (old) { old->hover = false; old->hover_item = old->type == UI_MENUBAR && old->open_menu >= 0 ? old->hover_item : -1; ui_invalidate(old); }
    win->hover_widget = w;
    if (w) { w->hover = true; ui_invalidate(w); }
    win->widget_cursor = (w && w->enabled && (w->type == UI_TEXTBOX || w->type == UI_TEXTAREA)) ? CURSOR_IBEAM : 0;
}

static int slider_value_at(widget_t *w, int x, int y) {
    bool vert = w->r.h > w->r.w;
    int range = w->max - w->min;
    if (!vert) {
        int x0 = w->r.x + 10, x1 = w->r.x + w->r.w - 10;
        if (x1 <= x0) return w->min;
        return w->min + (int)(((int64_t)(x - x0) * range + (x1 - x0) / 2) / (x1 - x0));
    }
    int y0 = w->r.y + 10, y1 = w->r.y + w->r.h - 10;
    if (y1 <= y0) return w->min;
    return w->min + (int)(((int64_t)(y1 - y) * range + (y1 - y0) / 2) / (y1 - y0));
}

static void slider_set(widget_t *w, int v) {
    v = CLAMP(v, w->min, w->max);
    if (v == w->value) return;
    w->value = v;
    ui_invalidate(w);
    fire(w, w->cb, w->ctx);
}

/* scrollbar geometry helpers: track rect, thumb pos/len */
static void sb_geom(widget_t *w, rect_t tr, bool vert, int total, int page, int pos, int *tp, int *tl) {
    int len = vert ? tr.h : tr.w;
    *tl = total > 0 ? MAX(24, (int)((int64_t)len * page / total)) : len;
    if (*tl > len) *tl = len;
    int maxpos = total - page;
    *tp = maxpos > 0 ? (int)((int64_t)(len - *tl) * pos / maxpos) : 0;
    (void)w;
}

static int sb_pos_from_mouse(rect_t tr, bool vert, int total, int page, int m, int off) {
    int len = vert ? tr.h : tr.w;
    int tl = total > 0 ? MAX(24, (int)((int64_t)len * page / total)) : len;
    if (tl > len) tl = len;
    int maxpos = total - page;
    if (len - tl <= 0 || maxpos <= 0) return 0;
    int p = m - (vert ? tr.y : tr.x) - off;
    return CLAMP((int)((int64_t)p * maxpos / (len - tl)), 0, maxpos);
}

static rect_t list_sb_rect(widget_t *w) { return rect_make(w->r.x + w->r.w - SB_W - 2, w->r.y + 2, SB_W, w->r.h - 4); }

static void list_select(widget_t *w, int idx, bool notify) {
    idx = CLAMP(idx, 0, w->nitems - 1);
    if (w->nitems == 0) return;
    if (idx == w->value) { ui_list_scroll_to(w, idx); return; }
    w->value = idx;
    ui_list_scroll_to(w, idx);
    ui_invalidate(w);
    if (notify) fire(w, w->cb, w->ctx);
}

static bool list_mouse(widget_t *w, const event_t *ev) {
    int vis = list_visible_rows(w);
    bool sb = w->nitems > vis;
    rect_t sr = list_sb_rect(w);
    switch (ev->type) {
    case EV_MOUSE_DOWN:
        if (sb && rect_contains(sr, ev->x, ev->y)) {
            int tp, tl;
            sb_geom(w, sr, true, w->nitems, vis, w->scroll, &tp, &tl);
            int rel = ev->y - sr.y;
            if (rel >= tp && rel < tp + tl) { w->sb_drag = true; w->sb_drag_off = rel - tp; }
            else { w->scroll = CLAMP(w->scroll + (rel < tp ? -vis : vis), 0, scroll_max(w)); }
            ui_invalidate(w);
            return true;
        }
        if (ev->button == MOUSE_LEFT || ev->button == MOUSE_RIGHT) {
            int idx = w->scroll + (ev->y - w->r.y - 2) / LIST_ROW_H;
            if (idx >= 0 && idx < w->nitems) list_select(w, idx, true);
        }
        return true;
    case EV_MOUSE_MOVE:
        if (w->sb_drag) {
            w->scroll = sb_pos_from_mouse(sr, true, w->nitems, vis, ev->y, w->sb_drag_off);
            ui_invalidate(w);
            return true;
        } else {
            int idx = w->scroll + (ev->y - w->r.y - 2) / LIST_ROW_H;
            if (sb && ev->x >= sr.x) idx = -1;
            if (idx >= w->nitems || !rect_contains(w->r, ev->x, ev->y)) idx = -1;
            if (idx != w->hover_item) { w->hover_item = idx; ui_invalidate(w); }
        }
        return true;
    case EV_MOUSE_UP:
        w->sb_drag = false;
        ui_invalidate(w);
        return true;
    case EV_DOUBLE_CLICK:
        if (!(sb && ev->x >= sr.x) && w->value >= 0) fire(w, w->cb2, w->ctx2);
        return true;
    case EV_MOUSE_WHEEL:
        w->scroll = CLAMP(w->scroll + ev->wheel * 3, 0, scroll_max(w));
        ui_invalidate(w);
        return true;
    default:
        return false;
    }
}

static bool widget_mouse(widget_t *w, const event_t *ev) {
    window_t *win = w->win;
    switch (w->type) {
    case UI_BUTTON:
        if (ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT) { w->pressed = true; ui_invalidate(w); }
        else if (ev->type == EV_MOUSE_UP && ev->button == MOUSE_LEFT) {
            bool click = w->pressed && rect_contains(w->r, ev->x, ev->y);
            w->pressed = false;
            ui_invalidate(w);
            if (click) fire(w, w->cb, w->ctx);
        } else if (ev->type == EV_MOUSE_MOVE) {
            bool in = rect_contains(w->r, ev->x, ev->y);
            if (in != w->hover) { w->hover = in; ui_invalidate(w); }
        }
        return true;
    case UI_CHECKBOX: case UI_RADIO:
        if (ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT) w->pressed = true;
        else if (ev->type == EV_MOUSE_UP && ev->button == MOUSE_LEFT) {
            bool click = w->pressed && rect_contains(w->r, ev->x, ev->y);
            w->pressed = false;
            if (click) {
                if (w->type == UI_CHECKBOX) { w->value = !w->value; ui_invalidate(w); fire(w, w->cb, w->ctx); }
                else if (!w->value) { ui_set_value(w, 1); fire(w, w->cb, w->ctx); }
            }
        }
        return true;
    case UI_SLIDER:
        if (ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT) { w->pressed = true; slider_set(w, slider_value_at(w, ev->x, ev->y)); }
        else if (ev->type == EV_MOUSE_MOVE && w->pressed) slider_set(w, slider_value_at(w, ev->x, ev->y));
        else if (ev->type == EV_MOUSE_UP) { w->pressed = false; ui_invalidate(w); }
        else if (ev->type == EV_MOUSE_WHEEL) {
            int step = MAX(1, (w->max - w->min) / 20);
            slider_set(w, w->value - ev->wheel * step);
        }
        return true;
    case UI_SCROLLBAR: {
        bool vert = w->group;
        if (ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT) {
            int tp, tl;
            sb_geom(w, w->r, vert, w->total, w->page, w->value, &tp, &tl);
            int rel = vert ? ev->y - w->r.y : ev->x - w->r.x;
            if (rel >= tp && rel < tp + tl) { w->sb_drag = true; w->sb_drag_off = rel - tp; }
            else {
                int v = CLAMP(w->value + (rel < tp ? -w->page : w->page), 0, scroll_max(w));
                if (v != w->value) { w->value = v; fire(w, w->cb, w->ctx); }
            }
            ui_invalidate(w);
        } else if (ev->type == EV_MOUSE_MOVE && w->sb_drag) {
            int v = sb_pos_from_mouse(w->r, vert, w->total, w->page, vert ? ev->y : ev->x, w->sb_drag_off);
            if (v != w->value) { w->value = v; ui_invalidate(w); fire(w, w->cb, w->ctx); }
        } else if (ev->type == EV_MOUSE_UP) {
            w->sb_drag = false;
            ui_invalidate(w);
        } else if (ev->type == EV_MOUSE_WHEEL) {
            int step = MAX(1, w->page / 8);
            int v = CLAMP(w->value + ev->wheel * step, 0, scroll_max(w));
            if (v != w->value) { w->value = v; ui_invalidate(w); fire(w, w->cb, w->ctx); }
        }
        return true;
    }
    case UI_TEXTBOX:
        if (ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT) {
            int pos = font_hit_test(wfont(w), w->text, ev->x - (w->r.x + 8) + w->scroll_x);
            w->cursor = pos;
            if (!(ev->mods & MOD_SHIFT)) w->anchor = pos;
            w->pressed = true;
            ui_blink_on = true;
            ui_invalidate(w);
        } else if (ev->type == EV_MOUSE_MOVE && w->pressed) {
            int pos = font_hit_test(wfont(w), w->text, ev->x - (w->r.x + 8) + w->scroll_x);
            if (pos != w->cursor) { w->cursor = pos; tb_ensure_visible(w); ui_invalidate(w); }
        } else if (ev->type == EV_MOUSE_UP) {
            w->pressed = false;
        } else if (ev->type == EV_DOUBLE_CLICK) {
            ui_textbox_select_all(w);
        }
        return true;
    case UI_LISTBOX:
        return list_mouse(w, ev);
    case UI_DROPDOWN:
        if (ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT) {
            if (popup_closed_owner == w && popup_closed_serial == wm_click_serial) return true;
            w->pressed = true;
            ui_invalidate(w);
            dropdown_open(w);
        } else if (ev->type == EV_MOUSE_WHEEL && w->nitems) {
            int v = CLAMP(w->value + ev->wheel, 0, w->nitems - 1);
            if (v != w->value) { w->value = v; ui_invalidate(w); fire(w, w->cb, w->ctx); }
        }
        return true;
    case UI_TABS: {
        int x = w->r.x, hit = -1;
        for (int i = 0; i < w->nitems; i++) {
            const font_t *f = (i == w->value) ? FONT_UI_BOLD : FONT_UI;
            int tw = font_text_width(f, w->items[i]) + 28;
            if (ev->x >= x && ev->x < x + tw) hit = i;
            x += tw;
        }
        if (ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT && hit >= 0 && hit != w->value) {
            w->value = hit;
            ui_invalidate(w);
            fire(w, w->cb, w->ctx);
        } else if (ev->type == EV_MOUSE_MOVE && hit != w->hover_item) {
            w->hover_item = hit;
            ui_invalidate(w);
        }
        return true;
    }
    case UI_MENUBAR: {
        int hit = -1;
        for (int i = 0; i < w->nmenus; i++)
            if (ev->x >= w->r.x + w->menus[i].x && ev->x < w->r.x + w->menus[i].x + w->menus[i].w) hit = i;
        if (ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT && hit >= 0) {
            if (popup_closed_owner == w && popup_closed_serial == wm_click_serial && popup_closed_menu == hit) return true;
            menubar_open(w, hit);
        } else if (ev->type == EV_MOUSE_MOVE) {
            if (hit != w->hover_item) { w->hover_item = hit; ui_invalidate(w); }
            if (w->open_menu >= 0 && hit >= 0 && hit != w->open_menu) menubar_open(w, hit);
        }
        return true;
    }
    case UI_CUSTOM:
        return w->event_fn ? w->event_fn(w, ev) : false;
    case UI_TEXTAREA:
        return textarea_event(w, ev);
    default:
        (void)win;
        return false;
    }
}

static bool widget_key(widget_t *w, const event_t *ev) {
    if (w->type == UI_TEXTAREA) return textarea_event(w, ev);
    if (w->type == UI_TEXTBOX) {
        if (ev->type == EV_CHAR || ev->type == EV_KEY_DOWN) return textbox_key(w, ev);
        return false;
    }
    if (ev->type != EV_KEY_DOWN) return w->type == UI_CUSTOM && w->event_fn ? w->event_fn(w, ev) : false;
    int k = ev->key;
    switch (w->type) {
    case UI_BUTTON:
        if (k == KEY_SPACE || k == KEY_ENTER) { fire(w, w->cb, w->ctx); return true; }
        return false;
    case UI_CHECKBOX:
        if (k == KEY_SPACE) { w->value = !w->value; ui_invalidate(w); fire(w, w->cb, w->ctx); return true; }
        return false;
    case UI_RADIO:
        if (k == KEY_SPACE && !w->value) { ui_set_value(w, 1); fire(w, w->cb, w->ctx); return true; }
        return false;
    case UI_SLIDER: {
        int step = MAX(1, (w->max - w->min) / 20);
        if (k == KEY_LEFT || k == KEY_DOWN) { slider_set(w, w->value - step); return true; }
        if (k == KEY_RIGHT || k == KEY_UP) { slider_set(w, w->value + step); return true; }
        if (k == KEY_HOME) { slider_set(w, w->min); return true; }
        if (k == KEY_END) { slider_set(w, w->max); return true; }
        return false;
    }
    case UI_LISTBOX: {
        int vis = list_visible_rows(w);
        if (!w->nitems) return false;
        int v = w->value;
        if (k == KEY_DOWN) v = v < 0 ? 0 : v + 1;
        else if (k == KEY_UP) v = v < 0 ? 0 : v - 1;
        else if (k == KEY_PAGEDOWN) v += vis;
        else if (k == KEY_PAGEUP) v -= vis;
        else if (k == KEY_HOME) v = 0;
        else if (k == KEY_END) v = w->nitems - 1;
        else if (k == KEY_ENTER) { if (w->value >= 0) fire(w, w->cb2, w->ctx2); return true; }
        else if (k < 128 && isalnum(k)) {
            for (int n = 1; n <= w->nitems; n++) {
                int i = (MAX(w->value, 0) + n) % w->nitems;
                if (tolower((unsigned char)w->items[i][0]) == k) { v = i; break; }
            }
        } else return false;
        list_select(w, v, true);
        return true;
    }
    case UI_DROPDOWN:
        if (k == KEY_DOWN || k == KEY_UP) {
            if (!w->nitems) return true;
            int v = CLAMP(w->value + (k == KEY_DOWN ? 1 : -1), 0, w->nitems - 1);
            if (v != w->value) { w->value = v; ui_invalidate(w); fire(w, w->cb, w->ctx); }
            return true;
        }
        if (k == KEY_SPACE || k == KEY_ENTER) { dropdown_open(w); return true; }
        return false;
    case UI_TABS:
        if ((k == KEY_LEFT || k == KEY_RIGHT) && w->nitems) {
            int v = CLAMP(w->value + (k == KEY_RIGHT ? 1 : -1), 0, w->nitems - 1);
            if (v != w->value) { w->value = v; ui_invalidate(w); fire(w, w->cb, w->ctx); }
            return true;
        }
        return false;
    case UI_CUSTOM:
        return w->event_fn ? w->event_fn(w, ev) : false;
    default:
        return false;
    }
}

static void focus_cycle(window_t *win, bool back) {
    widget_t *list[128];
    int n = 0, cur = -1;
    for (widget_t *w = win->widgets; w && n < 128; w = w->next) {
        if (!focusable(w)) continue;
        if (w == win->focus_widget) cur = n;
        list[n++] = w;
    }
    if (!n) return;
    int next = cur < 0 ? (back ? n - 1 : 0) : (cur + (back ? n - 1 : 1)) % n;
    if (list[next]->type == UI_TEXTBOX) ui_textbox_select_all(list[next]);
    ui_focus(list[next]);
}

bool ui_wants_tab(window_t *win) {
    for (widget_t *w = win->widgets; w; w = w->next) if (focusable(w)) return true;
    return false;
}

bool ui_dispatch(window_t *win, const event_t *ev) {
    /* note: dead widgets are only freed in ui_paint_all(), never during event
       dispatch, because callbacks can re-enter dispatch for the same window */
    if (!win->widgets) return false;
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        widget_t *cap = win->capture_widget;
        if (cap && !cap->dead) return widget_mouse(cap, ev);
        widget_t *w = widget_at(win, ev->x, ev->y);
        if (w && !w->enabled) w = NULL;
        set_hover(win, w);
        if (w) return widget_mouse(w, ev);
        return false;
    }
    case EV_MOUSE_LEAVE:
        if (win->hover_widget && win->hover_widget->type == UI_LISTBOX) { win->hover_widget->hover_item = -1; }
        if (win->hover_widget && win->hover_widget->type == UI_TABS) { win->hover_widget->hover_item = -1; }
        if (win->hover_widget && win->hover_widget->type == UI_MENUBAR && win->hover_widget->open_menu < 0) { win->hover_widget->hover_item = -1; }
        set_hover(win, NULL);
        return false;
    case EV_MOUSE_DOWN: {
        widget_t *w = widget_at(win, ev->x, ev->y);
        if (!w || !w->enabled) return false;
        if (focusable(w)) ui_focus(w);
        win->capture_widget = w;
        set_hover(win, w);
        return widget_mouse(w, ev);
    }
    case EV_MOUSE_UP: {
        widget_t *cap = win->capture_widget;
        win->capture_widget = NULL;
        if (cap && !cap->dead) {
            widget_mouse(cap, ev);
            if (alive(cap)) set_hover(win, rect_contains(cap->r, ev->x, ev->y) ? cap : NULL);
            return true;
        }
        return false;
    }
    case EV_DOUBLE_CLICK: case EV_MOUSE_WHEEL: {
        widget_t *w = widget_at(win, ev->x, ev->y);
        if (!w || !w->enabled) return false;
        return widget_mouse(w, ev);
    }
    case EV_KEY_DOWN: case EV_CHAR: case EV_KEY_UP: {
        widget_t *f = win->focus_widget;
        if (f && (f->dead || !f->visible || !f->enabled)) f = NULL;
        if (ev->type == EV_KEY_DOWN && ev->key == KEY_TAB && !(ev->mods & (MOD_CTRL | MOD_ALT)) && ui_wants_tab(win) &&
            !(f && f->type == UI_TEXTAREA && textarea_wants_tab(f) && !(ev->mods & MOD_SHIFT))) {
            focus_cycle(win, ev->mods & MOD_SHIFT);
            return true;
        }
        if (f && widget_key(f, ev)) return true;
        if (ev->type == EV_KEY_DOWN && ev->key == KEY_ENTER) {
            for (widget_t *w = win->widgets; w; w = w->next)
                if (w->type == UI_BUTTON && w->is_default && w->visible && w->enabled && !w->dead) { fire(w, w->cb, w->ctx); return true; }
        }
        return false;
    }
    case EV_FOCUS: case EV_BLUR:
        if (win->focus_widget) ui_invalidate(win->focus_widget);
        return false;
    default:
        return false;
    }
}
