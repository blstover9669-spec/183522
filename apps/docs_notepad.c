/*
 * docs_notepad.c - "Notepad": a plain text editor.
 *
 * The editing surface is the toolkit's multi-line text area (caret, arrow /
 * Home / End / PgUp / PgDn navigation, mouse and Shift selection, clipboard,
 * undo/redo, scrollbars). Around it this file adds the document model:
 *
 *   - New / Open / Save / Save As through the VFS and the system file dialog,
 *     with "Save changes?" prompts before anything would discard edits
 *     (including closing the window: WIN_CONFIRM_CLOSE);
 *   - an unsaved-changes marker ('*') in the window title;
 *   - Find / Find next / Replace all / Go to line, Time/Date insertion;
 *   - View options (monospace or proportional font, zoom, line numbers,
 *     status bar) shown with check marks in the menus;
 *   - a status bar with the caret's line/column, selection size and counts.
 *
 * launch(arg) opens the file `arg` (a path that does not exist yet becomes
 * the target of the first Save). Opening a file that is already open in
 * another Notepad window focuses that window instead.
 *
 * All state is per window (notepad_t via win_set_userdata) and freed on
 * EV_CLOSE; the only global is the list of open editors used for that
 * "already open" check.
 */
#include <nova.h>

#define NP_W       680
#define NP_H       470
#define STATUS_H   26
#define NP_MAX_FILE (8u << 20)      /* refuse to open files above 8 MiB */
#define MSG_MS     3500             /* transient status-bar messages */

/* What to do once the current document has been saved or discarded. */
enum { ACT_NONE, ACT_NEW, ACT_OPEN, ACT_OPEN_PATH, ACT_CLOSE };

typedef struct notepad {
    window_t *win;
    widget_t *menubar, *area, *status;
    char path[VFS_PATH_MAX];          /* "" = untitled                         */
    char pending_path[VFS_PATH_MAX];  /* ACT_OPEN_PATH target / save-as target */
    int  pending;                     /* ACT_* waiting on a save / prompt      */
    bool mono, line_numbers, show_status, match_case;
    int  zoom;                        /* 0 = 100%, 1 = large                   */
    bool shown_modified;              /* modified state reflected in the title */
    bool menu_has_sel;                /* selection state reflected in menus    */
    char find_text[128];
    char replace_find[128];
    char msg[160];                    /* transient status message              */
    uint32_t msg_until;
    int  msg_timer;
    struct notepad *next;             /* list of open editors                  */
} notepad_t;

static notepad_t *g_editors;

static void np_do(notepad_t *np, int act);
static void np_build_menus(notepad_t *np);

/* ---- small helpers ------------------------------------------------------------------ */
static const char *np_name(notepad_t *np) { return np->path[0] ? vfs_basename(np->path) : "Untitled"; }

static const font_t *np_font(notepad_t *np) {
    if (np->mono) return np->zoom ? FONT_MONO_2X : FONT_MONO;
    return np->zoom ? FONT_UI_LARGE : FONT_UI;
}

static void np_update_title(notepad_t *np) {
    char t[VFS_NAME_MAX + 32];
    bool mod = ui_textarea_modified(np->area);
    snprintf(t, sizeof t, "%s%s - Notepad", mod ? "*" : "", np_name(np));
    win_set_title(np->win, t);
    np->shown_modified = mod;
}

static void np_message(notepad_t *np, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void np_message(notepad_t *np, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(np->msg, sizeof np->msg, fmt, ap);
    va_end(ap);
    np->msg_until = sys_uptime_ms() + MSG_MS;
    if (!np->msg_timer) np->msg_timer = win_set_timer(np->win, 500);
    ui_invalidate(np->status);
}

static void np_error(notepad_t *np, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void np_error(notepad_t *np, const char *fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ui_message_box(np->win, "Notepad", buf, MB_OK | MB_ICON_ERROR, NULL, NULL);
}

static bool np_has_selection(notepad_t *np) {
    int a, b;
    ui_textarea_get_selection(np->area, &a, &b);
    return a != b;
}

static notepad_t *np_find_open(const char *path) {
    for (notepad_t *e = g_editors; e; e = e->next)
        if (e->path[0] && !strcmp(e->path, path)) return e;
    return NULL;
}

/* ---- layout --------------------------------------------------------------------------- */
/* The text area is laid out edge to edge: its 1 px side borders sit just outside
   the client area, its top border hides under the menu bar and its bottom border
   (and focus underline) under the status bar, which are both created after it
   and therefore paint over it. */
static void np_layout(notepad_t *np, int w, int h) {
    int top = UI_MENUBAR_H - 2;
    int bottom = np->show_status ? h - STATUS_H + 2 : h + 2;
    ui_move(np->area, -1, top, w + 2, MAX(40, bottom - top));
    /* a hidden status bar keeps a zero-height rect so it still repairs the corners */
    if (np->show_status) ui_move(np->status, 0, h - STATUS_H, w, STATUS_H);
    else ui_move(np->status, 0, h, w, 0);
}

static void np_apply_view(notepad_t *np) {
    ui_set_font(np->area, np_font(np));
    ui_textarea_set_line_numbers(np->area, np->line_numbers);
    np_layout(np, win_width(np->win), win_height(np->win));
    np_build_menus(np);
    win_invalidate(np->win);
}

/* ---- document --------------------------------------------------------------------- */
static void np_set_document(notepad_t *np, const char *path, const char *text) {
    strlcpy(np->path, path ? path : "", sizeof np->path);
    ui_set_text(np->area, text ? text : "");
    ui_textarea_set_modified(np->area, false);
    ui_textarea_set_cursor(np->area, 0, 0);
    ui_focus(np->area);
    np_update_title(np);
    ui_invalidate(np->status);
}

/* Load `path` into this window. Returns false (after telling the user) on error. */
static bool np_load(notepad_t *np, const char *path) {
    vfs_stat_t st;
    int err = vfs_stat(path, &st);
    if (err < 0) { np_error(np, "Cannot open \"%s\":\n%s.", path, vfs_strerror(err)); return false; }
    if (st.is_dir) { np_error(np, "\"%s\" is a folder, not a text file.", path); return false; }
    if (st.size > NP_MAX_FILE) {
        np_error(np, "\"%s\" is too large to open in Notepad (%u KB).", vfs_basename(path), st.size >> 10);
        return false;
    }
    uint32_t size = 0;
    char *data = vfs_read_file(path, &size);
    if (!data) { np_error(np, "Cannot read \"%s\".", path); return false; }
    /* The editor works on C strings: show NUL bytes (binary files) as spaces. */
    bool binary = false;
    for (uint32_t i = 0; i < size; i++)
        if (data[i] == 0) { data[i] = ' '; binary = true; }
    np_set_document(np, path, data);
    free(data);
    if (binary) np_message(np, "This file contains binary data; NUL bytes are shown as spaces.");
    else np_message(np, "Opened %s (%u bytes)", path, size);
    klog("notepad: opened %s (%u bytes)", path, size);
    return true;
}

/* Write the editor contents to `path`. */
static bool np_write(notepad_t *np, const char *path) {
    const char *text = ui_get_text(np->area);
    int len = ui_textarea_length(np->area);
    int err = vfs_write_file(path, text, (uint32_t)len);
    if (err < 0) {
        np_error(np, "Could not save \"%s\":\n%s.", path, vfs_strerror(err));
        return false;
    }
    if (path != np->path) strlcpy(np->path, path, sizeof np->path);
    ui_textarea_set_modified(np->area, false);
    np_update_title(np);
    np_message(np, "Saved %s (%d bytes)", np->path, len);
    klog("notepad: saved %s (%d bytes)", np->path, len);
    return true;
}

/* ---- save / save as (asynchronous, then continue with `act`) --------------------------- */
static void np_overwrite_cb(int r, void *ctx) {
    notepad_t *np = ctx;
    int act = np->pending;
    np->pending = ACT_NONE;
    if (r == MB_RESULT_YES && np_write(np, np->pending_path)) np_do(np, act);
}

static void np_saveas_cb(const char *path, void *ctx) {
    notepad_t *np = ctx;
    if (!path) { np->pending = ACT_NONE; return; }
    char full[VFS_PATH_MAX];
    strlcpy(full, path, sizeof full);
    /* no extension typed: default to .txt */
    if (!strchr(vfs_basename(full), '.') && strlen(full) + 4 < sizeof full) strlcat(full, ".txt", sizeof full);
    if (vfs_is_dir(full)) { np_error(np, "\"%s\" is a folder.", full); np->pending = ACT_NONE; return; }
    if (vfs_exists(full) && strcmp(full, np->path) != 0) {
        strlcpy(np->pending_path, full, sizeof np->pending_path);
        char msg[VFS_PATH_MAX + 64];
        snprintf(msg, sizeof msg, "\"%s\" already exists.\nDo you want to replace it?", vfs_basename(full));
        ui_message_box(np->win, "Save as", msg, MB_YESNO | MB_ICON_WARNING, np_overwrite_cb, np);
        return;
    }
    int act = np->pending;
    np->pending = ACT_NONE;
    if (np_write(np, full)) np_do(np, act);
}

static void np_save_as(notepad_t *np, int then) {
    char start[VFS_PATH_MAX];
    if (np->path[0]) strlcpy(start, np->path, sizeof start);
    else strlcpy(start, vfs_is_dir("/home/user/Documents") ? "/home/user/Documents/Untitled.txt" : "/Untitled.txt", sizeof start);
    np->pending = then;
    ui_file_dialog(np->win, UI_FILE_SAVE, "Save as", start, NULL, np_saveas_cb, np);
}

static void np_save(notepad_t *np, int then) {
    if (!np->path[0]) { np_save_as(np, then); return; }
    if (np_write(np, np->path)) np_do(np, then);
}

/* ---- "Save changes?" before discarding ------------------------------------------------ */
static void np_confirm_cb(int r, void *ctx) {
    notepad_t *np = ctx;
    int act = np->pending;
    np->pending = ACT_NONE;
    if (r == MB_RESULT_YES) np_save(np, act);
    else if (r == MB_RESULT_NO) np_do(np, act);
}

/* Run `act` now if nothing would be lost, otherwise ask first. */
static void np_confirm(notepad_t *np, int act) {
    if (!ui_textarea_modified(np->area)) { np_do(np, act); return; }
    np->pending = act;
    char msg[VFS_NAME_MAX + 64];
    snprintf(msg, sizeof msg, "Do you want to save the changes to \"%s\"?", np_name(np));
    ui_message_box(np->win, "Notepad", msg, MB_YESNOCANCEL | MB_ICON_WARNING, np_confirm_cb, np);
}

static void np_open_cb(const char *path, void *ctx) {
    notepad_t *np = ctx;
    if (path) np_load(np, path);
}

static void np_open_dialog(notepad_t *np) {
    char start[VFS_PATH_MAX];
    if (np->path[0]) vfs_dirname(np->path, start);
    else strlcpy(start, vfs_is_dir("/home/user/Documents") ? "/home/user/Documents" : "/", sizeof start);
    ui_file_dialog(np->win, UI_FILE_OPEN, "Open", start, NULL, np_open_cb, np);
}

/* Perform a document action. ACT_CLOSE frees np: never touch it afterwards. */
static void np_do(notepad_t *np, int act) {
    switch (act) {
    case ACT_NEW:       np_set_document(np, NULL, ""); np_message(np, "New document"); break;
    case ACT_OPEN:      np_open_dialog(np); break;
    case ACT_OPEN_PATH: np_load(np, np->pending_path); break;
    case ACT_CLOSE:     win_close(np->win); break;
    default: break;
    }
}

/* ---- edit commands ----------------------------------------------------------------------- */
static void np_copy(notepad_t *np) {
    int a, b;
    ui_textarea_get_selection(np->area, &a, &b);
    if (a == b) return;
    char *c = strndup(ui_get_text(np->area) + a, (size_t)(b - a));
    if (c) { clipboard_set(c); free(c); }
}

static void np_delete_sel(notepad_t *np) {
    if (np_has_selection(np)) ui_textarea_insert(np->area, "");   /* replaces the selection */
}

static void np_delete_key(notepad_t *np) {
    int a, b;
    ui_textarea_get_selection(np->area, &a, &b);
    if (a == b) {
        if (a >= ui_textarea_length(np->area)) return;
        ui_textarea_select(np->area, a, a + 1);
    }
    ui_textarea_insert(np->area, "");
}

static void np_insert_datetime(notepad_t *np) {
    datetime_t dt;
    sys_datetime(&dt);
    char buf[40];
    snprintf(buf, sizeof buf, "%02d:%02d %04d-%02d-%02d", dt.hour, dt.minute, dt.year, dt.month, dt.day);
    ui_textarea_insert(np->area, buf);
}

/* ---- search ------------------------------------------------------------------------------ */
static void np_find_next(notepad_t *np);

static void np_find_cb(const char *text, void *ctx) {
    notepad_t *np = ctx;
    if (!text || !*text) return;
    strlcpy(np->find_text, text, sizeof np->find_text);
    np_find_next(np);
}

static void np_find_dialog(notepad_t *np) {
    /* seed with the selection if it is a short single-line snippet */
    int a, b;
    ui_textarea_get_selection(np->area, &a, &b);
    if (b > a && b - a < (int)sizeof np->find_text) {
        const char *t = ui_get_text(np->area);
        if (!memchr(t + a, '\n', (size_t)(b - a))) {
            memcpy(np->find_text, t + a, (size_t)(b - a));
            np->find_text[b - a] = 0;
        }
    }
    ui_input_box(np->win, "Find", np->match_case ? "Find what (match case):" : "Find what:",
                 np->find_text, np_find_cb, np);
}

static void np_find_next(notepad_t *np) {
    if (!np->find_text[0]) { np_find_dialog(np); return; }
    if (ui_textarea_find(np->area, np->find_text, np->match_case)) {
        int ln, col;
        ui_textarea_get_cursor(np->area, &ln, &col);
        np_message(np, "Found \"%s\" on line %d", np->find_text, ln + 1);
    } else {
        char msg[200];
        snprintf(msg, sizeof msg, "Cannot find \"%s\".", np->find_text);
        ui_message_box(np->win, "Notepad", msg, MB_OK | MB_ICON_INFO, NULL, NULL);
    }
}

static bool np_match_at(const char *t, const char *f, int n, bool mc) {
    return mc ? !strncmp(t, f, (size_t)n) : !strncasecmp(t, f, (size_t)n);
}

/* Replace every occurrence as one undoable edit. Returns the count. */
static int np_replace_all(notepad_t *np, const char *find, const char *repl) {
    const char *t = ui_get_text(np->area);
    int len = ui_textarea_length(np->area);
    int fl = (int)strlen(find), rl = (int)strlen(repl);
    if (fl == 0 || len < fl) return 0;
    int count = 0;
    for (int i = 0; i + fl <= len; ) {
        if (np_match_at(t + i, find, fl, np->match_case)) { count++; i += fl; }
        else i++;
    }
    if (!count) return 0;
    char *out = malloc((size_t)len + (size_t)count * (size_t)MAX(0, rl - fl) + 1);
    if (!out) return 0;
    int o = 0;
    for (int i = 0; i < len; ) {
        if (i + fl <= len && np_match_at(t + i, find, fl, np->match_case)) {
            memcpy(out + o, repl, (size_t)rl);
            o += rl;
            i += fl;
        } else {
            out[o++] = t[i++];
        }
    }
    out[o] = 0;
    int ln, col;
    ui_textarea_get_cursor(np->area, &ln, &col);
    ui_textarea_select(np->area, 0, len);
    ui_textarea_insert(np->area, out);      /* t is stale from here on */
    free(out);
    ui_textarea_set_cursor(np->area, ln, col);
    return count;
}

static void np_replace_with_cb(const char *text, void *ctx) {
    notepad_t *np = ctx;
    if (!text) return;
    int n = np_replace_all(np, np->replace_find, text);
    if (n) np_message(np, "Replaced %d occurrence%s of \"%s\"", n, n == 1 ? "" : "s", np->replace_find);
    else {
        char msg[200];
        snprintf(msg, sizeof msg, "Cannot find \"%s\".", np->replace_find);
        ui_message_box(np->win, "Replace", msg, MB_OK | MB_ICON_INFO, NULL, NULL);
    }
}

static void np_replace_find_cb(const char *text, void *ctx) {
    notepad_t *np = ctx;
    if (!text || !*text) return;
    strlcpy(np->replace_find, text, sizeof np->replace_find);
    strlcpy(np->find_text, text, sizeof np->find_text);
    char prompt[200];
    snprintf(prompt, sizeof prompt, "Replace every \"%s\" with:", text);
    ui_input_box(np->win, "Replace", prompt, "", np_replace_with_cb, np);
}

static void np_replace_dialog(notepad_t *np) {
    ui_input_box(np->win, "Replace", "Find what:", np->find_text, np_replace_find_cb, np);
}

static void np_goto_cb(const char *text, void *ctx) {
    notepad_t *np = ctx;
    if (!text) return;
    int n = atoi(text);
    int lines = ui_textarea_line_count(np->area);
    if (n < 1 || n > lines) {
        char msg[96];
        snprintf(msg, sizeof msg, "The line number must be between 1 and %d.", lines);
        ui_message_box(np->win, "Go to line", msg, MB_OK | MB_ICON_WARNING, NULL, NULL);
        return;
    }
    ui_textarea_set_cursor(np->area, n - 1, 0);
    ui_focus(np->area);
}

static void np_goto_dialog(notepad_t *np) {
    int ln, col;
    ui_textarea_get_cursor(np->area, &ln, &col);
    char prompt[64], init[16];
    snprintf(prompt, sizeof prompt, "Line number (1 - %d):", ui_textarea_line_count(np->area));
    snprintf(init, sizeof init, "%d", ln + 1);
    ui_input_box(np->win, "Go to line", prompt, init, np_goto_cb, np);
}

/* ---- menus ---------------------------------------------------------------------------- */
static void np_new_window(void) { app_launch_by_name("Notepad", NULL); }

static void np_file_menu(int item, void *ctx) {
    notepad_t *np = ctx;
    switch (item) {
    case 0: np_confirm(np, ACT_NEW); break;
    case 1: np_new_window(); break;
    case 2: np_confirm(np, ACT_OPEN); break;
    case 3: np_save(np, ACT_NONE); break;
    case 4: np_save_as(np, ACT_NONE); break;
    case 6: np_confirm(np, ACT_CLOSE); break;   /* may free np */
    default: break;
    }
}

static void np_edit_menu(int item, void *ctx) {
    notepad_t *np = ctx;
    switch (item) {
    case 0: ui_textarea_undo(np->area); break;
    case 1: ui_textarea_redo(np->area); break;
    case 3: np_copy(np); np_delete_sel(np); break;
    case 4: np_copy(np); break;
    case 5: ui_textarea_insert(np->area, clipboard_get()); break;
    case 6: np_delete_key(np); break;
    case 8: ui_textarea_select(np->area, 0, ui_textarea_length(np->area)); break;
    case 9: np_insert_datetime(np); break;
    default: break;
    }
    ui_focus(np->area);
}

static void np_search_menu(int item, void *ctx) {
    notepad_t *np = ctx;
    switch (item) {
    case 0: np_find_dialog(np); break;
    case 1: np_find_next(np); break;
    case 2: np_replace_dialog(np); break;
    case 3: np_goto_dialog(np); break;
    case 5: np->match_case = !np->match_case; np_build_menus(np); break;
    default: break;
    }
}

static void np_zoom(notepad_t *np, int z) {
    z = CLAMP(z, 0, 1);
    if (z == np->zoom) return;
    np->zoom = z;
    np_apply_view(np);
}

static void np_view_menu(int item, void *ctx) {
    notepad_t *np = ctx;
    switch (item) {
    case 0: np->mono = true; break;
    case 1: np->mono = false; break;
    case 3: np->zoom = 1; break;
    case 4: np->zoom = 0; break;
    case 6: np->line_numbers = !np->line_numbers; break;
    case 7: np->show_status = !np->show_status; break;
    default: return;
    }
    np_apply_view(np);
}

static void np_help_menu(int item, void *ctx) {
    notepad_t *np = ctx;
    if (item == 0)
        ui_message_box(np->win, "About Notepad",
                       "Notepad for NovaOS\n\nA simple text editor. Files are kept in the NovaOS "
                       "file system; use Files to browse them.",
                       MB_OK | MB_ICON_INFO, NULL, NULL);
}

/* (Re)create the menu bar so check marks and enabled items reflect the state. */
static void np_build_menus(notepad_t *np) {
    if (np->menubar) ui_destroy(np->menubar);
    widget_t *mb = np->menubar = ui_menubar(np->win);
    if (!mb) return;
    bool sel = np_has_selection(np);
    np->menu_has_sel = sel;
    ui_menubar_add(mb, "File",
                   "New\tCtrl+N|New window\tCtrl+Shift+N|Open...\tCtrl+O|Save\tCtrl+S|"
                   "Save as...\tCtrl+Shift+S|-|Close\tCtrl+W", np_file_menu, np);
    char edit[256];
    snprintf(edit, sizeof edit,
             "Undo\tCtrl+Z|Redo\tCtrl+Y|-|%sCut\tCtrl+X|%sCopy\tCtrl+C|Paste\tCtrl+V|%sDelete\tDel|-|"
             "Select all\tCtrl+A|Time/Date\tF5", sel ? "" : "!", sel ? "" : "!", sel ? "" : "!");
    ui_menubar_add(mb, "Edit", edit, np_edit_menu, np);
    char search[200];
    snprintf(search, sizeof search,
             "Find...\tCtrl+F|Find next\tF3|Replace...\tCtrl+H|Go to line...\tCtrl+G|-|Match case%s",
             np->match_case ? "\t\x96" : "");
    ui_menubar_add(mb, "Search", search, np_search_menu, np);
    char view[256];
    snprintf(view, sizeof view,
             "Monospace font%s|Proportional font%s|-|%sZoom in\tCtrl++|%sZoom out\tCtrl+-|-|Line numbers%s|Status bar%s",
             np->mono ? "\t\x96" : "", np->mono ? "" : "\t\x96",
             np->zoom ? "!" : "", np->zoom ? "" : "!",
             np->line_numbers ? "\t\x96" : "", np->show_status ? "\t\x96" : "");
    ui_menubar_add(mb, "View", view, np_view_menu, np);
    ui_menubar_add(mb, "Help", "About Notepad", np_help_menu, np);
}

/* ---- text area callbacks ----------------------------------------------------------- */
static void np_on_edit(widget_t *w, void *ctx) {
    notepad_t *np = ctx;
    if (ui_textarea_modified(w) != np->shown_modified) np_update_title(np);
    ui_invalidate(np->status);
}

static void np_on_cursor(widget_t *w, void *ctx) {
    notepad_t *np = ctx;
    ui_invalidate(np->status);
    if (np_has_selection(np) != np->menu_has_sel) np_build_menus(np);
    if (ui_textarea_modified(w) != np->shown_modified) np_update_title(np);
}

/* The text area's rounded border (accent coloured while focused) is pushed just
   outside the visible client edges, but a few anti-aliased corner pixels still
   peek in. Repaint them with the colour found just inside the editor. Called
   from the status bar's draw function, which always runs after the text area
   has painted (the two overlap, so they are invalidated together). */
static void np_fix_corners(notepad_t *np, surface_t *s) {
    rect_t r = ui_rect(np->area);
    int w = s->w;
    int xs[6] = { 0, 1, 2, w - 3, w - 2, w - 1 };
    for (int i = 0; i < 6; i++) {
        int x = xs[i];
        uint32_t top = gfx_get_pixel(s, x, r.y + 9) | 0xFF000000u;
        uint32_t bot = gfx_get_pixel(s, x, r.y + r.h - 11) | 0xFF000000u;
        for (int y = r.y + 2; y <= r.y + 5; y++) gfx_pixel(s, x, y, top);
        for (int y = r.y + r.h - 7; y <= r.y + r.h - 1; y++) gfx_pixel(s, x, y, bot);
    }
}

/* ---- status bar (a disabled custom widget: drawn on top, never takes focus) ----------- */
static void np_status_draw(widget_t *w, surface_t *s, rect_t r) {
    notepad_t *np = ui_get_ctx(w);
    const theme_t *t = theme_get();
    np_fix_corners(np, s);
    if (r.h <= 0) return;          /* status bar hidden: corner repair only */
    uint32_t bg = t->dark ? color_shade(t->window_bg, -10) : color_shade(t->window_bg, -6);
    uint32_t line = color_mix(t->window_bg, t->window_border, 90);
    uint32_t muted = color_mix(t->window_text, t->window_bg, 70);
    gfx_fill_rect(s, r.x, r.y, r.w, r.h, bg);
    gfx_hline(s, r.x, r.y, r.w, line);
    int ty = r.y + 1 + (r.h - 1 - font_height(FONT_UI)) / 2;

    char cells[4][64];
    int ln = 0, col = 0, a = 0, b = 0;
    ui_textarea_get_cursor(np->area, &ln, &col);
    ui_textarea_get_selection(np->area, &a, &b);
    if (b > a) snprintf(cells[0], sizeof cells[0], "Ln %d, Col %d  (%d selected)", ln + 1, col + 1, b - a);
    else snprintf(cells[0], sizeof cells[0], "Ln %d, Col %d", ln + 1, col + 1);
    int lines = ui_textarea_line_count(np->area), chars = ui_textarea_length(np->area);
    snprintf(cells[1], sizeof cells[1], "%d line%s, %d char%s", lines, lines == 1 ? "" : "s", chars, chars == 1 ? "" : "s");
    snprintf(cells[2], sizeof cells[2], "%s", np->zoom ? (np->mono ? "200%" : "140%") : "100%");
    snprintf(cells[3], sizeof cells[3], "%s", np->mono ? "Plain text" : "Plain text (proportional)");

    /* right-aligned cells separated by hairlines */
    int x = r.x + r.w - 14;
    for (int i = 3; i >= 0; i--) {
        int tw = font_text_width(FONT_UI, cells[i]);
        int cw = i == 0 ? MAX(tw, font_text_width(FONT_UI, "Ln 9999, Col 999")) : tw;
        x -= cw;
        if (x < r.x + 120) break;
        gfx_text(s, x, ty, cells[i], i == 0 ? t->window_text : muted, FONT_UI);
        x -= 12;
        gfx_vline(s, x, r.y + 6, r.h - 11, line);
        x -= 13;
    }
    /* left: transient message, or the file location */
    const char *left = np->msg[0] ? np->msg : (np->path[0] ? np->path : "Untitled - not saved yet");
    gfx_text_ellipsis(s, r.x + 12, ty, MAX(0, x - r.x - 12), left, np->msg[0] ? t->window_text : muted, FONT_UI);
}

/* ---- window --------------------------------------------------------------------------- */
static void np_key(notepad_t *np, const event_t *ev) {
    bool ctrl = ev->mods & MOD_CTRL, shift = ev->mods & MOD_SHIFT;
    if (ctrl && !(ev->mods & MOD_ALT)) {
        switch (ev->key) {
        case 'n': if (shift) np_new_window(); else np_confirm(np, ACT_NEW); return;
        case 'o': np_confirm(np, ACT_OPEN); return;
        case 's': if (shift) np_save_as(np, ACT_NONE); else np_save(np, ACT_NONE); return;
        case 'w': np_confirm(np, ACT_CLOSE); return;   /* may free np */
        case 'f': np_find_dialog(np); return;
        case 'h': np_replace_dialog(np); return;
        case 'g': np_goto_dialog(np); return;
        case '=': case '+': np_zoom(np, np->zoom + 1); return;
        case '-': np_zoom(np, np->zoom - 1); return;
        case '0': np_zoom(np, 0); return;
        default: return;
        }
    }
    switch (ev->key) {
    case KEY_F3: np_find_next(np); break;
    case KEY_F5: np_insert_datetime(np); break;
    default: break;
    }
}

static void np_event(window_t *win, const event_t *ev) {
    notepad_t *np = win_get_userdata(win);
    if (!np) return;
    switch (ev->type) {
    case EV_RESIZE:
        np_layout(np, ev->w, ev->h);
        break;
    case EV_KEY_DOWN:
        np_key(np, ev);
        break;
    case EV_TIMER:
        if (ev->timer_id == np->msg_timer && sys_uptime_ms() >= np->msg_until) {
            np->msg[0] = 0;
            win_kill_timer(win, np->msg_timer);
            np->msg_timer = 0;
            ui_invalidate(np->status);
        }
        break;
    case EV_FOCUS:
        ui_focus(np->area);
        break;
    case EV_CLOSE_REQUEST:
        np_confirm(np, ACT_CLOSE);        /* may free np */
        break;
    case EV_CLOSE:
        for (notepad_t **pp = &g_editors; *pp; pp = &(*pp)->next)
            if (*pp == np) { *pp = np->next; break; }
        win_set_userdata(win, NULL);
        free(np);
        break;
    default:
        break;
    }
}

static void np_launch(const char *arg) {
    char path[VFS_PATH_MAX] = "";
    if (arg && *arg && vfs_normalize(NULL, arg, path) != 0) path[0] = 0;
    if (path[0] && vfs_is_dir(path)) path[0] = 0;
    if (path[0]) {
        notepad_t *open = np_find_open(path);
        if (open && win_is_valid(open->win)) { win_focus(open->win); return; }
    }

    notepad_t *np = calloc(1, sizeof *np);
    if (!np) return;
    np->mono = true;
    np->show_status = true;
    window_t *win = win_create("Untitled - Notepad", NP_W, NP_H, WIN_RESIZABLE | WIN_CONFIRM_CLOSE);
    if (!win) { free(np); return; }
    np->win = win;
    win_set_userdata(win, np);
    win_set_event_handler(win, np_event);
    win_set_min_size(win, 360, 200);

    /* creation order matters: the menu bar and status bar paint over the text area's edges */
    np->area = ui_textarea(win, 0, UI_MENUBAR_H, NP_W, NP_H - UI_MENUBAR_H - STATUS_H, "");
    np->status = ui_custom(win, 0, NP_H - STATUS_H, NP_W, STATUS_H, np_status_draw, NULL, np);
    ui_set_enabled(np->status, false);     /* passive: clicks fall through, never focused */
    if (!np->area || !np->status) { win_close(win); return; }
    ui_set_callback(np->area, np_on_edit, np);
    ui_textarea_on_cursor(np->area, np_on_cursor, np);
    ui_textarea_set_tab_width(np->area, 4);
    np->next = g_editors;
    g_editors = np;
    np_apply_view(np);
    np_set_document(np, NULL, "");

    if (path[0]) {
        if (vfs_exists(path)) np_load(np, path);
        else {
            /* a new file: remember the name, it is created on the first save */
            strlcpy(np->path, path, sizeof np->path);
            np_update_title(np);
            np_message(np, "%s does not exist yet; it will be created when you save.", path);
        }
    }
}

/* Icon: a spiral-bound note pad with ruled lines and a pencil. */
static void np_icon(surface_t *s, int x, int y, int size) {
    double u = size / 64.0;
#define U(v) ((int)((v) * u + 0.5))
    /* page shadow + page */
    gfx_fill_rounded_rect(s, x + U(11), y + U(8), U(40), U(52), U(6), RGBA(0, 0, 0, 50));
    gfx_fill_rounded_rect(s, x + U(10), y + U(6), U(40), U(52), U(6), RGB(196, 202, 214));
    gfx_fill_rounded_rect(s, x + U(11), y + U(7), U(38), U(50), U(5), RGB(252, 252, 250));
    /* blue binding band */
    gfx_fill_rounded_gradient_v(s, x + U(10), y + U(6), U(40), U(13), U(6), RGB(80, 150, 245), RGB(40, 105, 215));
    gfx_fill_rect(s, x + U(10), y + U(14), U(40), U(5), RGB(40, 105, 215));
    /* spiral rings */
    for (int i = 0; i < 5; i++) {
        int cx = x + U(16 + i * 7);
        gfx_fill_rounded_rect(s, cx - U(1.5), y + U(2), U(3), U(10), U(1.5), RGB(70, 76, 90));
        gfx_fill_circle_aa(s, cx, y + U(13), U(1.6), RGB(20, 50, 110));
    }
    /* ruled text lines */
    static const int len[] = { 30, 26, 30, 18, 24, 14 };
    for (int i = 0; i < 6; i++)
        gfx_fill_rounded_rect(s, x + U(15), y + U(24 + i * 5.5), U(len[i]), U(2.2), U(1), RGB(150, 165, 190));
    /* pencil, drawn as a rotated polygon (45 degrees, tip bottom-left) */
    double cx = x + 44 * u, cy = y + 44 * u;   /* centre of the pencil body */
    double dx = 0.7071, dy = -0.7071;           /* along the pencil (towards the eraser) */
    double nx = 0.7071, ny = 0.7071;            /* across */
    double hw = 4.2 * u;
#define P(a, b) { (int)(cx + dx * (a) * u + nx * (b)), (int)(cy + dy * (a) * u + ny * (b)) }
    point_t body[4] = { P(-10, -hw), P(14, -hw), P(14, hw), P(-10, hw) };
    gfx_fill_polygon(s, body, 4, RGB(255, 190, 40));
    point_t stripe[4] = { P(-10, -hw * 0.25), P(14, -hw * 0.25), P(14, hw * 0.25), P(-10, hw * 0.25) };
    gfx_fill_polygon(s, stripe, 4, RGB(240, 150, 20));
    point_t band[4] = { P(14, -hw), P(17, -hw), P(17, hw), P(14, hw) };
    gfx_fill_polygon(s, band, 4, RGB(190, 195, 205));
    point_t eraser[4] = { P(17, -hw), P(21, -hw), P(21, hw), P(17, hw) };
    gfx_fill_polygon(s, eraser, 4, RGB(240, 120, 140));
    point_t wood[3] = { P(-10, -hw), P(-10, hw), P(-19, 0) };
    gfx_fill_polygon(s, wood, 3, RGB(245, 215, 170));
    point_t lead[3] = { P(-15.5, -hw * 0.38), P(-15.5, hw * 0.38), P(-19, 0) };
    gfx_fill_polygon(s, lead, 3, RGB(50, 50, 60));
#undef P
#undef U
}

REGISTER_APP(.name = "Notepad", .category = CAT_ACCESSORIES,
             .description = "Edit plain text files",
             .launch = np_launch, .draw_icon = np_icon,
             .file_types = "txt,text,log,ini,cfg,conf,nfo,md,json,xml,c,h,asm,s,py");
