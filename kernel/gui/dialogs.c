/* dialogs.c - asynchronous message boxes, input boxes, file dialogs and toast notifications. */
#include "wm_internal.h"

/* ---- message box ----------------------------------------------------------------- */
typedef struct {
    ui_msgbox_cb cb;
    void *ctx;
    int type;
    int result;
    bool done;
    char *text;
    int text_h;
    void (*input_cb)(const char *, void *);   /* input box */
    widget_t *textbox;
} msgbox_t;

#define MB_W 420

static void msgbox_finish(window_t *win, int result) {
    msgbox_t *m = win_get_userdata(win);
    if (!m || m->done) return;
    m->result = result;
    m->done = true;
    bool cbs = !win->no_callbacks;
    ui_msgbox_cb cb = m->cb;
    void *ctx = m->ctx;
    void (*icb)(const char *, void *) = m->input_cb;
    char *txt = NULL;
    if (icb && result == MB_RESULT_OK && m->textbox) txt = strdup(ui_get_text(m->textbox));
    win_close(win);
    if (cbs) {
        if (icb) icb(result == MB_RESULT_OK ? txt : NULL, ctx);
        else if (cb) cb(result, ctx);
    }
    kfree(txt);
}

static void mb_btn(widget_t *w, void *ctx) {
    (void)ctx;
    msgbox_finish(ui_window(w), (int)ui_get_tag(w));
}

static int cancel_result(int type) {
    switch (type & 0x0F) {
    case MB_OK: return MB_RESULT_OK;
    case MB_YESNO: return MB_RESULT_NO;
    default: return MB_RESULT_CANCEL;
    }
}

static void msgbox_handler(window_t *win, const event_t *ev) {
    msgbox_t *m = win_get_userdata(win);
    if (!m) return;
    const theme_t *t = theme_get();
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        int bh = 60;
        gfx_fill_rect(s, 0, s->h - bh, s->w, bh, t->dark ? color_shade(t->window_bg, -12) : color_shade(t->window_bg, -6));
        gfx_hline(s, 0, s->h - bh, s->w, color_mix(t->window_bg, t->window_border, 70));
        if (!m->input_cb) {
            int icon = (m->type & 0xF0) == MB_ICON_WARNING ? UI_GLYPH_WARNING : (m->type & 0xF0) == MB_ICON_ERROR ? UI_GLYPH_ERROR
                     : (m->type & 0xF0) == MB_ICON_QUESTION ? UI_GLYPH_QUESTION : UI_GLYPH_INFO;
            ui_draw_glyph(s, icon, 22, 22, 32, t->accent);
            gfx_text_wrapped(s, rect_make(70, 26, MB_W - 92, m->text_h + 4), m->text, t->window_text, FONT_UI);
        } else {
            gfx_text_wrapped(s, rect_make(20, 18, MB_W - 40, m->text_h + 4), m->text, t->window_text, FONT_UI);
        }
        break;
    }
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESCAPE) msgbox_finish(win, cancel_result(m->type));
        else if (ev->key == KEY_ENTER) msgbox_finish(win, m->input_cb ? MB_RESULT_OK : ((m->type & 0x0F) == MB_YESNO || (m->type & 0x0F) == MB_YESNOCANCEL ? MB_RESULT_YES : MB_RESULT_OK));
        break;
    case EV_CLOSE:
        if (!m->done) {
            m->done = true;
            if (!win->no_callbacks) {
                if (m->input_cb) m->input_cb(NULL, m->ctx);
                else if (m->cb) m->cb(cancel_result(m->type), m->ctx);
            }
        }
        kfree(m->text);
        kfree(m);
        win_set_userdata(win, NULL);
        break;
    default:
        break;
    }
}

window_t *ui_message_box(window_t *owner, const char *title, const char *text, int type, ui_msgbox_cb cb, void *ctx) {
    msgbox_t *m = kcalloc(1, sizeof *m);
    if (!m) return NULL;
    m->cb = cb;
    m->ctx = ctx;
    m->type = type;
    m->text = strdup(text ? text : "");
    m->text_h = MAX(font_wrap_height(FONT_UI, m->text, MB_W - 92), font_height(FONT_UI));
    int h = MAX(m->text_h + 26, 54) + 26 + 60;
    window_t *w = win_create_dialog(owner, title ? title : "Message", MB_W, h, WIN_HIDDEN);
    if (!w) { kfree(m->text); kfree(m); return NULL; }
    win_set_userdata(w, m);
    win_set_event_handler(w, msgbox_handler);
    static const char *labels[4][3] = { { "OK" }, { "OK", "Cancel" }, { "Yes", "No" }, { "Yes", "No", "Cancel" } };
    static const int results[4][3] = { { MB_RESULT_OK }, { MB_RESULT_OK, MB_RESULT_CANCEL }, { MB_RESULT_YES, MB_RESULT_NO },
                                       { MB_RESULT_YES, MB_RESULT_NO, MB_RESULT_CANCEL } };
    int kind = type & 0x0F;
    if (kind > 3) kind = 0;
    int nb = kind == 0 ? 1 : kind == 3 ? 3 : 2;
    int bw = 96, gap = 8;
    int x = MB_W - 16 - nb * bw - (nb - 1) * gap;
    for (int i = 0; i < nb; i++) {
        widget_t *b = ui_button(w, x + i * (bw + gap), h - 46, bw, 32, labels[kind][i], mb_btn, NULL);
        ui_set_tag(b, results[kind][i]);
        if (i == 0) { ui_button_set_default(b, true); ui_focus(b); }
    }
    if (kind == 0 || (type & 0xF0) == MB_ICON_ERROR) sys_beep(880, 60);
    win_show(w);
    return w;
}

window_t *ui_input_box(window_t *owner, const char *title, const char *prompt, const char *initial, ui_input_cb cb, void *ctx) {
    msgbox_t *m = kcalloc(1, sizeof *m);
    if (!m) return NULL;
    m->input_cb = cb;
    m->ctx = ctx;
    m->type = MB_OKCANCEL;
    m->text = strdup(prompt ? prompt : "");
    m->text_h = MAX(font_wrap_height(FONT_UI, m->text, MB_W - 40), font_height(FONT_UI));
    int h = 18 + m->text_h + 10 + 32 + 20 + 60;
    window_t *w = win_create_dialog(owner, title ? title : "Input", MB_W, h, WIN_HIDDEN);
    if (!w) { kfree(m->text); kfree(m); return NULL; }
    win_set_userdata(w, m);
    win_set_event_handler(w, msgbox_handler);
    m->textbox = ui_textbox(w, 20, 18 + m->text_h + 10, MB_W - 40, 32, initial ? initial : "");
    ui_textbox_select_all(m->textbox);
    ui_focus(m->textbox);
    widget_t *ok = ui_button(w, MB_W - 16 - 96 * 2 - 8, h - 46, 96, 32, "OK", mb_btn, NULL);
    ui_set_tag(ok, MB_RESULT_OK);
    ui_button_set_default(ok, true);
    widget_t *cancel = ui_button(w, MB_W - 16 - 96, h - 46, 96, 32, "Cancel", mb_btn, NULL);
    ui_set_tag(cancel, MB_RESULT_CANCEL);
    win_show(w);
    return w;
}

/* ---- file dialog -------------------------------------------------------------------- */
typedef struct {
    int mode;
    char dir[VFS_PATH_MAX];
    char exts[128];
    ui_file_cb cb;
    void *ctx;
    bool done;
    widget_t *list, *name, *places, *pathlbl;
    vfs_dirent_t *ents;
    int nents;
} filedlg_t;

#define FD_W 600
#define FD_H 420

static bool ext_ok(filedlg_t *d, const char *name) {
    if (!d->exts[0]) return true;
    const char *e = vfs_extension(name);
    char buf[128];
    strlcpy(buf, d->exts, sizeof buf);
    char *save, *tok = strtok_r(buf, ",; ", &save);
    while (tok) {
        if (!strcasecmp(tok, e) || !strcmp(tok, "*")) return true;
        tok = strtok_r(NULL, ",; ", &save);
    }
    return false;
}

static void fd_icon(widget_t *w, int idx, surface_t *s, int x, int y) {
    filedlg_t *d = ui_get_ctx(w);
    intptr_t e = ui_list_get_data(w, idx);
    bool dir = e >= 0 && e < d->nents && d->ents[e].is_dir;
    ui_draw_glyph(s, dir ? UI_GLYPH_FOLDER : UI_GLYPH_FILE, x, y, 16, 0);
}

static void fd_refresh(filedlg_t *d) {
    kfree(d->ents);
    d->ents = NULL;
    d->nents = 0;
    int n = vfs_list(d->dir, NULL, 0);
    if (n > 0) {
        d->ents = kcalloc((size_t)n, sizeof(vfs_dirent_t));
        if (d->ents) d->nents = vfs_list(d->dir, d->ents, n);
        if (d->nents > n) d->nents = n;
    }
    ui_list_clear(d->list);
    for (int i = 0; i < d->nents; i++) {
        if (!d->ents[i].is_dir && (d->mode == UI_FILE_DIR || !ext_ok(d, d->ents[i].name))) continue;
        int idx = ui_list_add(d->list, d->ents[i].name);
        ui_list_set_data(d->list, idx, i);
    }
    ui_set_text(d->pathlbl, d->dir);
}

static void fd_finish(window_t *win, const char *path) {
    filedlg_t *d = win_get_userdata(win);
    if (!d || d->done) return;
    d->done = true;
    bool cbs = !win->no_callbacks;
    ui_file_cb cb = d->cb;
    void *ctx = d->ctx;
    char *p = path ? strdup(path) : NULL;
    win_close(win);
    if (cbs && cb) cb(p, ctx);
    kfree(p);
}

static void fd_enter(filedlg_t *d, const char *name) {
    char np[VFS_PATH_MAX];
    if (!strcmp(name, "..")) vfs_dirname(d->dir, np);
    else vfs_join(d->dir, name, np);
    if (vfs_is_dir(np)) {
        strlcpy(d->dir, np, sizeof d->dir);
        fd_refresh(d);
    }
}

static void fd_accept(window_t *win, filedlg_t *d) {
    char path[VFS_PATH_MAX];
    if (d->mode == UI_FILE_DIR) {
        int sel = ui_get_value(d->list);
        if (sel >= 0) vfs_join(d->dir, ui_list_get(d->list, sel), path);
        else strlcpy(path, d->dir, sizeof path);
        fd_finish(win, path);
        return;
    }
    const char *name = ui_get_text(d->name);
    if (!*name) return;
    if (vfs_normalize(d->dir, name, path) != 0) return;
    if (vfs_is_dir(path)) {
        strlcpy(d->dir, path, sizeof d->dir);
        ui_set_text(d->name, "");
        fd_refresh(d);
        return;
    }
    if (d->mode == UI_FILE_OPEN && !vfs_exists(path)) {
        ui_message_box(win, "Open", "That file does not exist.", MB_OK | MB_ICON_WARNING, NULL, NULL);
        return;
    }
    fd_finish(win, path);
}

static void fd_select(widget_t *w, void *ctx) {
    filedlg_t *d = ctx;
    int sel = ui_get_value(w);
    if (sel < 0) return;
    intptr_t e = ui_list_get_data(w, sel);
    if (e >= 0 && e < d->nents && !d->ents[e].is_dir) ui_set_text(d->name, d->ents[e].name);
}

static void fd_activate(widget_t *w, void *ctx) {
    filedlg_t *d = ctx;
    int sel = ui_get_value(w);
    if (sel < 0) return;
    intptr_t e = ui_list_get_data(w, sel);
    if (e < 0 || e >= d->nents) return;
    if (d->ents[e].is_dir) fd_enter(d, d->ents[e].name);
    else { ui_set_text(d->name, d->ents[e].name); fd_accept(ui_window(w), d); }
}

static const char *places[][2] = {
    { "Home", "/home/user" }, { "Desktop", "/home/user/Desktop" }, { "Documents", "/home/user/Documents" },
    { "Pictures", "/home/user/Pictures" }, { "Music", "/home/user/Music" }, { "Computer", "/" },
};

static void fd_place(widget_t *w, void *ctx) {
    filedlg_t *d = ctx;
    int sel = ui_get_value(w);
    if (sel < 0 || sel >= ARRAY_SIZE(places)) return;
    if (!vfs_is_dir(places[sel][1])) return;
    strlcpy(d->dir, places[sel][1], sizeof d->dir);
    fd_refresh(d);
}

static void fd_up(widget_t *w, void *ctx) { (void)w; fd_enter(ctx, ".."); }
static void fd_ok(widget_t *w, void *ctx) { fd_accept(ui_window(w), ctx); }
static void fd_cancel(widget_t *w, void *ctx) { (void)ctx; fd_finish(ui_window(w), NULL); }
static void fd_submit(widget_t *w, void *ctx) { fd_accept(ui_window(w), ctx); }

static void fd_newfolder_cb(const char *name, void *ctx) {
    filedlg_t *d = ctx;
    if (!name || !*name) return;
    char p[VFS_PATH_MAX];
    vfs_join(d->dir, name, p);
    vfs_mkdir(p);
    fd_refresh(d);
}

static void fd_newfolder(widget_t *w, void *ctx) {
    ui_input_box(ui_window(w), "New folder", "Folder name:", "New Folder", fd_newfolder_cb, ctx);
}

static void filedlg_handler(window_t *win, const event_t *ev) {
    filedlg_t *d = win_get_userdata(win);
    if (!d) return;
    const theme_t *t = theme_get();
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        gfx_fill_rect(s, 0, 0, 150, s->h - 60, t->dark ? color_shade(t->window_bg, -10) : color_shade(t->window_bg, -5));
        gfx_fill_rect(s, 0, s->h - 60, s->w, 60, t->dark ? color_shade(t->window_bg, -12) : color_shade(t->window_bg, -6));
        gfx_hline(s, 0, s->h - 60, s->w, color_mix(t->window_bg, t->window_border, 70));
        gfx_text(s, 14, 12, "Places", color_mix(t->window_text, t->window_bg, 90), FONT_UI_BOLD);
        if (d->mode != UI_FILE_DIR) gfx_text(s, 16, s->h - 40, "Name:", t->window_text, FONT_UI);
        break;
    }
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESCAPE) fd_finish(win, NULL);
        else if (ev->key == KEY_BACKSPACE) fd_enter(d, "..");
        break;
    case EV_CLOSE:
        if (!d->done) {
            d->done = true;
            if (!win->no_callbacks && d->cb) d->cb(NULL, d->ctx);
        }
        kfree(d->ents);
        kfree(d);
        win_set_userdata(win, NULL);
        break;
    default:
        break;
    }
}

window_t *ui_file_dialog(window_t *owner, int mode, const char *title, const char *start, const char *exts,
                         ui_file_cb cb, void *ctx) {
    filedlg_t *d = kcalloc(1, sizeof *d);
    if (!d) return NULL;
    d->mode = mode;
    d->cb = cb;
    d->ctx = ctx;
    if (exts) strlcpy(d->exts, exts, sizeof d->exts);
    char initial_name[VFS_NAME_MAX] = "";
    if (start && *start) {
        char norm[VFS_PATH_MAX];
        if (vfs_normalize(NULL, start, norm) == 0) {
            if (vfs_is_dir(norm)) strlcpy(d->dir, norm, sizeof d->dir);
            else {
                vfs_dirname(norm, d->dir);
                strlcpy(initial_name, vfs_basename(norm), sizeof initial_name);
                if (!vfs_is_dir(d->dir)) strlcpy(d->dir, "/home/user", sizeof d->dir);
            }
        }
    }
    if (!d->dir[0]) strlcpy(d->dir, "/home/user/Documents", sizeof d->dir);
    if (!vfs_is_dir(d->dir)) strlcpy(d->dir, "/", sizeof d->dir);
    if (!title) title = mode == UI_FILE_SAVE ? "Save As" : mode == UI_FILE_DIR ? "Choose Folder" : "Open";
    window_t *w = win_create_dialog(owner, title, FD_W, FD_H, WIN_HIDDEN);
    if (!w) { kfree(d); return NULL; }
    win_set_userdata(w, d);
    win_set_event_handler(w, filedlg_handler);

    d->places = ui_listbox(w, 8, 34, 134, FD_H - 60 - 42, fd_place, d);
    for (int i = 0; i < ARRAY_SIZE(places); i++) ui_list_add(d->places, places[i][0]);
    ui_button(w, 160, 10, 36, 30, "\x92", fd_up, d);
    ui_button(w, FD_W - 108, 10, 96, 30, "New folder", fd_newfolder, d);
    d->pathlbl = ui_label(w, 206, 10, FD_W - 206 - 116, 30, d->dir);
    d->list = ui_listbox(w, 160, 48, FD_W - 172, FD_H - 60 - 58, fd_select, d);
    ui_list_on_activate(d->list, fd_activate, d);
    ui_list_set_icon_fn(d->list, fd_icon);
    ui_set_callback(d->list, fd_select, d);
    if (mode != UI_FILE_DIR) {
        d->name = ui_textbox(w, 64, FD_H - 46, FD_W - 64 - 16 - 96 * 2 - 16, 32, initial_name);
        ui_textbox_on_submit(d->name, fd_submit, d);
    }
    widget_t *ok = ui_button(w, FD_W - 16 - 96 * 2 - 8, FD_H - 46, 96, 32,
                             mode == UI_FILE_SAVE ? "Save" : mode == UI_FILE_DIR ? "Choose" : "Open", fd_ok, d);
    ui_button_set_default(ok, true);
    ui_button(w, FD_W - 16 - 96, FD_H - 46, 96, 32, "Cancel", fd_cancel, d);
    fd_refresh(d);
    ui_focus(d->name ? d->name : d->list);
    if (d->name) ui_textbox_select_all(d->name);
    win_show(w);
    return w;
}

/* ---- toast notifications ------------------------------------------------------------- */
typedef struct { char title[64]; char text[160]; int timer; } toast_t;

#define TOAST_W 320
#define TOAST_H 74

static void restack_toasts(void) {
    int y = screen_height() - wm_taskbar_h - 12;
    for (window_t *w = wm_top; w; w = w->below) {
        if (w->closing || w->layer != LAYER_POPUP || strcmp(w->title, "toast")) continue;
        y -= w->frame.h;
        win_move(w, screen_width() - TOAST_W - 12, y);
        y -= 10;
    }
}

static void restack_cb(void *ctx) { (void)ctx; restack_toasts(); }

static void toast_handler(window_t *win, const event_t *ev) {
    toast_t *t = win_get_userdata(win);
    if (!t) return;
    const theme_t *th = theme_get();
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        gfx_clear(s, th->menu_bg);
        gfx_rounded_rect(s, 0, 0, s->w, s->h, 8, color_mix(th->menu_bg, th->window_border, 140));
        gfx_fill_rounded_rect(s, 12, 14, 4, s->h - 28, 2, th->accent);
        gfx_text_ellipsis(s, 28, 14, s->w - 40, t->title, th->menu_text, FONT_UI_BOLD);
        gfx_text_wrapped(s, rect_make(28, 36, s->w - 40, s->h - 40), t->text, color_mix(th->menu_text, th->menu_bg, 70), FONT_UI);
        break;
    }
    case EV_TIMER: case EV_MOUSE_DOWN:
        win_close(win);
        break;
    case EV_CLOSE:
        kfree(t);
        win_set_userdata(win, NULL);
        timer_add(1, restack_cb, NULL, false);
        break;
    default:
        break;
    }
}

void ui_notify(const char *title, const char *text) {
    if (!screen_width()) return;
    toast_t *t = kcalloc(1, sizeof *t);
    if (!t) return;
    strlcpy(t->title, title ? title : "", sizeof t->title);
    strlcpy(t->text, text ? text : "", sizeof t->text);
    const app_desc_t *saved = wm_current_app;
    wm_current_app = NULL;
    window_t *w = wm_create_ex("toast", TOAST_W, TOAST_H, WIN_FRAMELESS | WIN_NO_TASKBAR, LAYER_POPUP);
    wm_current_app = saved;
    if (!w) { kfree(t); return; }
    w->radius = 8;
    win_set_userdata(w, t);
    win_set_event_handler(w, toast_handler);
    win_set_timer(w, 4500);
    restack_toasts();
    sys_beep(1200, 25);
}
