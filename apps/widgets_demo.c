/*
 * widgets_demo.c - "Widget Gallery": shows every toolkit widget and dialog.
 * Doubles as a test bed for the UI toolkit and a copy-paste reference.
 */
#include <nova.h>

typedef struct {
    widget_t *tabs, *slider, *progress, *text, *pass, *list, *drop, *status, *sb, *check;
    widget_t *page[2][32];
    int npage[2];
} gallery_t;

static void set_status(gallery_t *g, const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ui_set_text(g->status, buf);
}

static void show_page(gallery_t *g, int p) {
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < g->npage[i]; j++) ui_set_visible(g->page[i][j], i == p);
}

static widget_t *add(gallery_t *g, int page, widget_t *w) {
    if (w && g->npage[page] < 32) g->page[page][g->npage[page]++] = w;
    return w;
}

static void on_tab(widget_t *w, void *ctx) { show_page(ctx, ui_get_value(w)); win_invalidate(ui_window(w)); }
static void on_slider(widget_t *w, void *ctx) {
    gallery_t *g = ctx;
    ui_set_value(g->progress, ui_get_value(w));
    set_status(g, "Slider: %d", ui_get_value(w));
}
static void on_button(widget_t *w, void *ctx) { set_status(ctx, "Clicked \"%s\"", ui_get_text(w)); sys_beep(700, 50); }
static void on_check(widget_t *w, void *ctx) { set_status(ctx, "%s: %s", ui_get_text(w), ui_get_value(w) ? "on" : "off"); }
static void on_text(widget_t *w, void *ctx) { set_status(ctx, "Text: %s", ui_get_text(w)); }
static void on_submit(widget_t *w, void *ctx) { set_status(ctx, "Submitted: %s", ui_get_text(w)); }
static void on_list(widget_t *w, void *ctx) { set_status(ctx, "Selected: %s", ui_get_text(w)); }
static void on_list_act(widget_t *w, void *ctx) { set_status(ctx, "Activated: %s", ui_get_text(w)); }
static void on_drop(widget_t *w, void *ctx) { set_status(ctx, "Dropdown: %s", ui_get_text(w)); }
static void on_sb(widget_t *w, void *ctx) { set_status(ctx, "Scrollbar: %d", ui_get_value(w)); }

static void input_done(const char *text, void *ctx) {
    if (text) set_status(ctx, "Input box returned \"%s\"", text);
    else set_status(ctx, "Input box cancelled");
}
static void on_input(widget_t *w, void *ctx) { ui_input_box(ui_window(w), "Your name", "What is your name?", "Nova", input_done, ctx); }

static void file_done(const char *path, void *ctx) { set_status(ctx, "File dialog: %s", path ? path : "(cancelled)"); }
static void on_file(widget_t *w, void *ctx) { ui_file_dialog(ui_window(w), UI_FILE_OPEN, NULL, "/home/user/Documents", NULL, file_done, ctx); }

static void msg_done(int r, void *ctx) { set_status(ctx, "Message box result: %d", r); }
static void on_msg(widget_t *w, void *ctx) {
    ui_message_box(ui_window(w), "Question", "Do you want to save the changes to \"Untitled\" before closing?",
                   MB_YESNOCANCEL | MB_ICON_WARNING, msg_done, ctx);
}
static void on_notify(widget_t *w, void *ctx) { (void)w; (void)ctx; ui_notify("Widget Gallery", "This is a toast notification."); }

static void menu_cb(int item, void *ctx) { set_status(ctx, "Menu item %d", item); }
static void file_menu(int item, void *ctx) {
    gallery_t *g = ctx;
    if (item == 4) { win_close(win_get_focused()); return; }
    set_status(g, "File menu item %d", item);
}

static void gallery_event(window_t *win, const event_t *ev) {
    gallery_t *g = win_get_userdata(win);
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        const theme_t *t = theme_get();
        gfx_fill_rect(s, 0, s->h - 28, s->w, 28, t->panel_bg);
        gfx_hline(s, 0, s->h - 28, s->w, color_mix(t->window_bg, t->window_border, 80));
        break;
    }
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_RIGHT)
            ui_popup_menu(win, ev->x, ev->y, "Cut\tCtrl+X|Copy\tCtrl+C|Paste\tCtrl+V|-|!Disabled item|Select all", menu_cb, g);
        break;
    case EV_RESIZE:
        ui_move(g->status, 10, ev->h - 26, ev->w - 20, 24);
        break;
    case EV_CLOSE:
        free(g);
        break;
    default:
        break;
    }
}

static void gallery_launch(const char *arg) {
    (void)arg;
    gallery_t *g = calloc(1, sizeof *g);
    if (!g) return;
    window_t *win = win_create("Widget Gallery", 560, 470, WIN_RESIZABLE);
    if (!win) { free(g); return; }
    win_set_userdata(win, g);
    win_set_event_handler(win, gallery_event);

    widget_t *mb = ui_menubar(win);
    ui_menubar_add(mb, "File", "New\tCtrl+N|Open...\tCtrl+O|Save\tCtrl+S|-|Close", file_menu, g);
    ui_menubar_add(mb, "Edit", "Undo|Redo|-|Cut|Copy|Paste", menu_cb, g);
    ui_menubar_add(mb, "Help", "About", menu_cb, g);

    g->tabs = ui_tabs(win, 10, UI_MENUBAR_H + 6, 540, 34, on_tab, g);
    ui_list_add(g->tabs, "Controls");
    ui_list_add(g->tabs, "Lists & dialogs");

    int y = UI_MENUBAR_H + 54;
    /* page 0 */
    add(g, 0, ui_label(win, 20, y, 120, 24, "Buttons"));
    add(g, 0, ui_button(win, 150, y - 4, 100, 32, "Normal", on_button, g));
    widget_t *def = add(g, 0, ui_button(win, 260, y - 4, 100, 32, "Default", on_button, g));
    ui_button_set_default(def, true);
    widget_t *dis = add(g, 0, ui_button(win, 370, y - 4, 100, 32, "Disabled", on_button, g));
    ui_set_enabled(dis, false);
    y += 44;
    add(g, 0, ui_label(win, 20, y, 120, 24, "Checkbox"));
    g->check = add(g, 0, ui_checkbox(win, 150, y, 160, 24, "Enable magic", true, on_check, g));
    add(g, 0, ui_checkbox(win, 320, y, 160, 24, "Another option", false, on_check, g));
    y += 36;
    add(g, 0, ui_label(win, 20, y, 120, 24, "Radio"));
    add(g, 0, ui_radio(win, 150, y, 100, 24, "Small", 1, false, on_check, g));
    add(g, 0, ui_radio(win, 250, y, 100, 24, "Medium", 1, true, on_check, g));
    add(g, 0, ui_radio(win, 350, y, 100, 24, "Large", 1, false, on_check, g));
    y += 36;
    add(g, 0, ui_label(win, 20, y, 120, 24, "Slider"));
    g->slider = add(g, 0, ui_slider(win, 144, y, 330, 24, 0, 100, 40, on_slider, g));
    y += 36;
    add(g, 0, ui_label(win, 20, y, 120, 24, "Progress"));
    g->progress = add(g, 0, ui_progress(win, 150, y + 8, 318, 8, 0, 100, 40));
    y += 34;
    add(g, 0, ui_label(win, 20, y, 120, 24, "Text box"));
    g->text = add(g, 0, ui_textbox(win, 150, y - 4, 318, 32, ""));
    ui_textbox_set_placeholder(g->text, "Type something and press Enter");
    ui_set_callback(g->text, on_text, g);
    ui_textbox_on_submit(g->text, on_submit, g);
    y += 42;
    add(g, 0, ui_label(win, 20, y, 120, 24, "Password"));
    g->pass = add(g, 0, ui_textbox(win, 150, y - 4, 318, 32, "secret"));
    ui_textbox_set_password(g->pass, true);
    y += 42;
    widget_t *wrap = add(g, 0, ui_label(win, 20, y, 520, 40,
        "Labels wrap long text automatically. Right-click anywhere in the window for a context menu."));
    ui_set_color(wrap, RGB(120, 124, 134));

    /* page 1 */
    y = UI_MENUBAR_H + 54;
    g->list = add(g, 1, ui_listbox(win, 20, y, 240, 220, on_list, g));
    static const char *fruits[] = { "Apple", "Banana", "Cherry", "Date", "Elderberry", "Fig", "Grape", "Honeydew",
                                    "Kiwi", "Lemon", "Mango", "Nectarine", "Orange", "Papaya", "Quince" };
    for (int i = 0; i < ARRAY_SIZE(fruits); i++) ui_list_add(g->list, fruits[i]);
    ui_list_on_activate(g->list, on_list_act, g);
    g->drop = add(g, 1, ui_dropdown(win, 280, y, 200, 32, on_drop, g));
    ui_list_add(g->drop, "Light");
    ui_list_add(g->drop, "Dark");
    ui_list_add(g->drop, "System default");
    ui_set_value(g->drop, 0);
    add(g, 1, ui_button(win, 280, y + 46, 200, 32, "Input box...", on_input, g));
    add(g, 1, ui_button(win, 280, y + 86, 200, 32, "Open file...", on_file, g));
    add(g, 1, ui_button(win, 280, y + 126, 200, 32, "Message box...", on_msg, g));
    add(g, 1, ui_button(win, 280, y + 166, 200, 32, "Notification", on_notify, g));
    g->sb = add(g, 1, ui_scrollbar(win, 500, y, 14, 220, true, on_sb, g));
    ui_scrollbar_set(g->sb, 1000, 200, 300);

    g->status = ui_label(win, 10, 470 - 26, 540, 24, "Ready.");
    show_page(g, 0);
}

static void gallery_icon(surface_t *s, int x, int y, int size) {
    int m = size / 10, r = size / 6;
    gfx_fill_rounded_gradient_v(s, x + m, y + m, size - 2 * m, size - 2 * m, r, RGB(255, 170, 60), RGB(235, 90, 70));
    int bx = x + size / 4, bw = size / 2, bh = size / 9;
    gfx_fill_rounded_rect(s, bx, y + size * 3 / 10, bw, bh, bh / 2, COLOR_WHITE);
    gfx_fill_rounded_rect(s, bx, y + size * 9 / 20, bw * 2 / 3, bh, bh / 2, RGBA(255, 255, 255, 200));
    gfx_fill_circle_aa(s, bx + bw * 0.25, y + size * 0.68, size / 12.0, COLOR_WHITE);
    gfx_fill_rounded_rect(s, bx + bw / 2, y + size * 0.68 - bh / 2, bw / 2, bh, bh / 2, RGBA(255, 255, 255, 200));
}

REGISTER_APP(.name = "Widget Gallery", .category = CAT_DEVELOPMENT,
             .description = "Every UI toolkit widget in one window",
             .launch = gallery_launch, .draw_icon = gallery_icon);
