/* desktop.c - the desktop: wallpaper plus icons for every app and for files in ~/Desktop. */
#include "wm_internal.h"

#define CELL_W 88
#define CELL_H 92
#define ICON_SZ 48
#define DESKTOP_DIR "/home/user/Desktop"

window_t *g_desktop_win;

typedef struct {
    const app_desc_t *app;      /* app icon, or NULL for a file */
    char name[VFS_NAME_MAX];
    bool is_dir;
    rect_t r;
} dicon_t;

static dicon_t *icons;
static int nicons, icons_cap;
static int selected = -1, hovered = -1;
static uint32_t vfs_gen_seen;

static void layout(void) {
    int x = 10, y = 10;
    int bottom = screen_height() - wm_taskbar_h - CELL_H;
    for (int i = 0; i < nicons; i++) {
        icons[i].r = rect_make(x, y, CELL_W, CELL_H);
        y += CELL_H;
        if (y > bottom) { y = 10; x += CELL_W; }
    }
}

static void add_icon(const app_desc_t *app, const char *name, bool is_dir) {
    if (nicons == icons_cap) {
        int nc = icons_cap ? icons_cap * 2 : 32;
        dicon_t *ni = krealloc(icons, sizeof(dicon_t) * (size_t)nc);
        if (!ni) return;
        icons = ni;
        icons_cap = nc;
    }
    dicon_t *d = &icons[nicons++];
    memset(d, 0, sizeof *d);
    d->app = app;
    d->is_dir = is_dir;
    strlcpy(d->name, name, sizeof d->name);
}

static uint32_t listing_sig;

static uint32_t listing_signature(void) {
    vfs_dirent_t ents[64];
    int n = vfs_list(DESKTOP_DIR, ents, 64);
    uint32_t h = 2166136261u ^ (uint32_t)n;
    for (int i = 0; i < n && i < 64; i++) {
        for (const char *p = ents[i].name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        h = (h ^ (ents[i].is_dir ? 0x5Au : 0xA5u)) * 16777619u;
    }
    return h;
}

static void rebuild(void) {
    listing_sig = listing_signature();
    nicons = 0;
    for (int i = 0; i < app_count(); i++) {
        const app_desc_t *a = app_get(i);
        if (a->flags & (APP_HIDDEN | APP_NO_DESKTOP)) continue;
        add_icon(a, a->name, false);
    }
    vfs_dirent_t ents[64];
    int n = vfs_list(DESKTOP_DIR, ents, 64);
    for (int i = 0; i < n && i < 64; i++) add_icon(NULL, ents[i].name, ents[i].is_dir);
    vfs_gen_seen = vfs_generation();
    if (selected >= nicons) selected = -1;
    layout();
}

/* Draw a label centred under the icon: up to two word-wrapped lines. */
static void draw_label(surface_t *s, rect_t r, const char *text, bool sel) {
    const font_t *f = FONT_UI;
    int maxw = r.w - 2;
    char line1[64], line2[64];
    int len = (int)strlen(text);
    int brk = len;
    if (font_text_width(f, text) > maxw) {
        /* break at the last space that fits; a single long word is ellipsised instead */
        int w = 0, last_space = -1;
        for (int i = 0; i < len; i++) {
            w += font_char_width(f, (unsigned char)text[i]);
            if (w > maxw) break;
            if (text[i] == ' ') last_space = i;
        }
        brk = last_space > 0 ? last_space : len;
    }
    strlcpy(line1, text, (size_t)MIN(brk + 1, (int)sizeof line1));
    const char *rest = text + brk;
    while (*rest == ' ') rest++;
    strlcpy(line2, rest, sizeof line2);
    int y = r.y + ICON_SZ + 14;
    const char *lines[2] = { line1, line2 };
    for (int i = 0; i < 2; i++) {
        if (!lines[i][0]) continue;
        int tw = MIN(font_text_width(f, lines[i]), maxw);
        int tx = r.x + (r.w - tw) / 2;
        if (!sel) {
            gfx_text_ellipsis(s, tx + 1, y + 1, maxw, lines[i], RGBA(0, 0, 0, 170), f);
            gfx_text_ellipsis(s, tx, y + 2, maxw, lines[i], RGBA(0, 0, 0, 60), f);
        }
        gfx_text_ellipsis(s, tx, y, maxw, lines[i], theme_get()->desktop_text, f);
        y += font_height(f);
    }
}

static void desktop_paint(window_t *win, const event_t *ev) {
    surface_t *s = win_surface(win);
    rect_t pr = rect_make(ev->x, ev->y, ev->w, ev->h);
    if (g_wallpaper) gfx_blit_rect(s, pr.x, pr.y, g_wallpaper, pr.x, pr.y, pr.w, pr.h);
    for (int i = 0; i < nicons; i++) {
        dicon_t *d = &icons[i];
        /* a two-line label's descenders reach a few pixels into the cell below */
        if (!rect_overlaps(rect_make(d->r.x, d->r.y, d->r.w, d->r.h + 16), pr)) continue;
        bool sel = i == selected, hov = i == hovered;
        if (sel || hov) {
            uint32_t c = sel ? color_alpha(theme_get()->accent, 120) : RGBA(255, 255, 255, 40);
            gfx_fill_rounded_rect(s, d->r.x + 2, d->r.y + 2, d->r.w - 4, d->r.h - 4, 8, c);
            if (sel) gfx_rounded_rect(s, d->r.x + 2, d->r.y + 2, d->r.w - 4, d->r.h - 4, 8, color_alpha(theme_get()->accent, 220));
        }
        int ix = d->r.x + (d->r.w - ICON_SZ) / 2, iy = d->r.y + 8;
        if (d->app) app_draw_icon(s, d->app, ix, iy, ICON_SZ);
        else {
            char path[VFS_PATH_MAX];
            vfs_join(DESKTOP_DIR, d->name, path);
            icon_draw(icon_for_path(path), s, ix, iy, ICON_SZ);
        }
        draw_label(s, d->r, d->name, sel);
    }
}

static int icon_at(int x, int y) {
    for (int i = 0; i < nicons; i++)
        if (rect_contains(rect_inset(icons[i].r, 2), x, y)) return i;
    return -1;
}

static void invalidate_icon(int i) {
    if (i >= 0 && i < nicons) win_invalidate_rect(g_desktop_win, icons[i].r.x, icons[i].r.y, icons[i].r.w, icons[i].r.h + 16);
}

static void open_icon(int i) {
    if (i < 0 || i >= nicons) return;
    dicon_t *d = &icons[i];
    if (d->app) { app_launch(d->app, NULL); return; }
    char path[VFS_PATH_MAX];
    vfs_join(DESKTOP_DIR, d->name, path);
    if (!shell_open(path) && !vfs_exists(path)) {
        char msg[200];
        snprintf(msg, sizeof msg, "\"%s\" no longer exists.", d->name);
        ui_message_box(NULL, "Open", msg, MB_OK | MB_ICON_WARNING, NULL, NULL);
        desktop_redraw();
    }
}

static void about_ok(int r, void *c) { (void)r; (void)c; }

/* the icon a context menu / dialog acts on (by name: the icon list may be rebuilt meanwhile) */
static char target_name[VFS_NAME_MAX];
static const app_desc_t *target_app;

static void select_by_name(const char *name) {
    for (int i = 0; i < nicons; i++)
        if (!icons[i].app && !strcmp(icons[i].name, name)) { invalidate_icon(selected); selected = i; invalidate_icon(i); return; }
}

static void error_box(const char *title, const char *what, int err) {
    char msg[200];
    snprintf(msg, sizeof msg, "%s: %s.", what, vfs_strerror(err));
    ui_message_box(NULL, title, msg, MB_OK | MB_ICON_ERROR, NULL, NULL);
}

static void unique_name(const char *base, const char *ext, char *out, size_t n) {
    char path[VFS_PATH_MAX];
    for (int k = 1; k < 1000; k++) {
        if (k == 1) snprintf(out, n, "%s%s", base, ext);
        else snprintf(out, n, "%s %d%s", base, k, ext);
        vfs_join(DESKTOP_DIR, out, path);
        if (!vfs_exists(path)) return;
    }
}

static bool valid_name(const char *name) {
    if (!name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
        ui_message_box(NULL, "Desktop", "That name is not valid. Names cannot be empty or contain '/'.", MB_OK | MB_ICON_WARNING, NULL, NULL);
        return false;
    }
    return true;
}

static void new_item_done(const char *text, void *ctx) {
    bool folder = (intptr_t)ctx == 1;
    if (!text || !valid_name(text)) return;
    char path[VFS_PATH_MAX];
    vfs_join(DESKTOP_DIR, text, path);
    if (vfs_exists(path)) { error_box(folder ? "New Folder" : "New Text File", text, VFS_EEXIST); return; }
    int r = folder ? vfs_mkdir(path) : vfs_create(path);
    if (r < 0) { error_box(folder ? "New Folder" : "New Text File", text, r); return; }
    desktop_redraw();
    select_by_name(text);
}

static void new_item(bool folder) {
    char name[VFS_NAME_MAX];
    unique_name(folder ? "New folder" : "New text file", folder ? "" : ".txt", name, sizeof name);
    ui_input_box(NULL, folder ? "New Folder" : "New Text File", folder ? "Name of the new folder:" : "Name of the new text file:",
                 name, new_item_done, (void *)(intptr_t)(folder ? 1 : 0));
}

static void rename_done(const char *text, void *ctx) {
    (void)ctx;
    if (!text || !strcmp(text, target_name) || !valid_name(text)) return;
    char from[VFS_PATH_MAX], to[VFS_PATH_MAX];
    vfs_join(DESKTOP_DIR, target_name, from);
    vfs_join(DESKTOP_DIR, text, to);
    if (vfs_exists(to)) { error_box("Rename", text, VFS_EEXIST); return; }
    int r = vfs_rename(from, to);
    if (r < 0) { error_box("Rename", target_name, r); return; }
    desktop_redraw();
    select_by_name(text);
}

static void delete_done(int result, void *ctx) {
    (void)ctx;
    if (result != MB_RESULT_YES) return;
    char path[VFS_PATH_MAX];
    vfs_join(DESKTOP_DIR, target_name, path);
    int r = vfs_remove_recursive(path);
    if (r < 0) error_box("Delete", target_name, r);
    selected = -1;
    desktop_redraw();
}

static void file_action(int action) {
    char path[VFS_PATH_MAX];
    vfs_join(DESKTOP_DIR, target_name, path);
    switch (action) {
    case 0: shell_open(path); break;
    case 1: shell_open_with_dialog(path); break;
    case 3: ui_input_box(NULL, "Rename", "New name:", target_name, rename_done, NULL); break;
    case 4: {
        char msg[200];
        snprintf(msg, sizeof msg, "Delete \"%s\"%s? This cannot be undone.", target_name,
                 vfs_is_dir(path) ? " and everything in it" : "");
        ui_message_box(NULL, "Delete", msg, MB_YESNO | MB_ICON_WARNING, delete_done, NULL);
        break;
    }
    case 6: shell_properties(path); break;
    default: break;
    }
}

static void desktop_menu(int item, void *ctx) {
    (void)ctx;
    switch (item) {
    case 0: new_item(true); break;
    case 1: new_item(false); break;
    case 3: rebuild(); win_invalidate(g_desktop_win); break;
    case 5: theme_set_wallpaper((theme_get()->wallpaper + 1) % (theme_wallpaper_count())); break;
    case 6: theme_set_dark(!theme_get()->dark); break;
    case 7:
        if (!app_launch_by_name("Settings", NULL))
            ui_message_box(NULL, "Settings", "The Settings app is not installed.", MB_OK, about_ok, NULL);
        break;
    case 9:
        if (!app_launch_by_name("About", NULL))
            ui_message_box(NULL, "About NovaOS", sys_version(), MB_OK, about_ok, NULL);
        break;
    default: break;
    }
}

static void icon_menu(int item, void *ctx) {
    (void)ctx;
    if (target_app) {
        if (item == 0) app_launch(target_app, NULL);
        else if (item == 1) shell_set_pinned(target_app->name, !shell_is_pinned(target_app->name));
        return;
    }
    file_action(item);
}

static void show_icon_menu(window_t *win, int i, int x, int y) {
    dicon_t *d = &icons[i];
    strlcpy(target_name, d->name, sizeof target_name);
    target_app = d->app;
    if (d->app)
        ui_popup_menu(win, x, y, shell_is_pinned(d->app->name) ? "Open|Unpin from taskbar" : "Open|Pin to taskbar", icon_menu, NULL);
    else
        ui_popup_menu(win, x, y, "Open|Open With...|-|Rename...|Delete|-|Properties", icon_menu, NULL);
}

static void desktop_handler(window_t *win, const event_t *ev) {
    switch (ev->type) {
    case EV_PAINT:
        desktop_paint(win, ev);
        break;
    case EV_MOUSE_MOVE: {
        int h = icon_at(ev->x, ev->y);
        if (h != hovered) { invalidate_icon(hovered); hovered = h; invalidate_icon(hovered); }
        break;
    }
    case EV_MOUSE_LEAVE:
        if (hovered >= 0) { invalidate_icon(hovered); hovered = -1; }
        break;
    case EV_MOUSE_DOWN: {
        int h = icon_at(ev->x, ev->y);
        if (h != selected) { invalidate_icon(selected); selected = h; invalidate_icon(selected); }
        if (ev->button == MOUSE_RIGHT) {
            if (h >= 0) show_icon_menu(win, h, ev->x, ev->y);
            else ui_popup_menu(win, ev->x, ev->y, "New Folder|New Text File|-|Refresh|-|Next wallpaper|Toggle dark mode|Settings...|-|About NovaOS",
                               desktop_menu, NULL);
        }
        break;
    }
    case EV_DOUBLE_CLICK:
        if (ev->button == MOUSE_LEFT) open_icon(icon_at(ev->x, ev->y));
        break;
    case EV_KEY_DOWN: {
        int rows = MAX(1, (screen_height() - wm_taskbar_h - 10) / CELL_H);
        int s = selected;
        if (ev->key == KEY_DOWN) s = s < 0 ? 0 : s + 1;
        else if (ev->key == KEY_UP) s = s < 0 ? 0 : s - 1;
        else if (ev->key == KEY_RIGHT) s = s < 0 ? 0 : s + rows;
        else if (ev->key == KEY_LEFT) s = s < 0 ? 0 : s - rows;
        else if (ev->key == KEY_ENTER) { open_icon(selected); break; }
        else if ((ev->key == KEY_DELETE || ev->key == KEY_F2) && selected >= 0 && selected < nicons && !icons[selected].app) {
            strlcpy(target_name, icons[selected].name, sizeof target_name);
            target_app = NULL;
            file_action(ev->key == KEY_DELETE ? 4 : 3);
            break;
        }
        else if (ev->key == KEY_MENU && selected >= 0 && selected < nicons) {
            show_icon_menu(win, selected, icons[selected].r.x + 44, icons[selected].r.y + 44);
            break;
        }
        else break;
        s = CLAMP(s, 0, nicons - 1);
        invalidate_icon(selected);
        selected = s;
        invalidate_icon(selected);
        break;
    }
    case EV_TIMER:
        if (vfs_generation() != vfs_gen_seen) {
            /* repaint only when ~/Desktop itself changed (not on every log/config write) */
            uint32_t sig = listing_signature();
            vfs_gen_seen = vfs_generation();
            if (sig != listing_sig) { rebuild(); win_invalidate(win); }
        }
        break;
    default:
        break;
    }
}

void desktop_wallpaper_changed(void) {
    if (g_desktop_win) win_invalidate(g_desktop_win);
}

void desktop_init(void) {
    const app_desc_t *saved = wm_current_app;
    wm_current_app = NULL;
    g_desktop_win = wm_create_ex("Desktop", screen_width(), screen_height(), WIN_FRAMELESS | WIN_NO_TASKBAR, LAYER_DESKTOP);
    wm_current_app = saved;
    if (!g_desktop_win) kpanic("cannot create desktop");
    win_move(g_desktop_win, 0, 0);
    win_set_event_handler(g_desktop_win, desktop_handler);
    rebuild();
    win_set_timer(g_desktop_win, 1000);
    wm_focus(g_desktop_win);
}

void desktop_redraw(void) {
    if (g_desktop_win) { rebuild(); win_invalidate(g_desktop_win); }
}
