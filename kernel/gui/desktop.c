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

static void rebuild(void) {
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
        if (!rect_overlaps(d->r, pr)) continue;
        bool sel = i == selected, hov = i == hovered;
        if (sel || hov) {
            uint32_t c = sel ? color_alpha(theme_get()->accent, 120) : RGBA(255, 255, 255, 40);
            gfx_fill_rounded_rect(s, d->r.x + 2, d->r.y + 2, d->r.w - 4, d->r.h - 4, 8, c);
            if (sel) gfx_rounded_rect(s, d->r.x + 2, d->r.y + 2, d->r.w - 4, d->r.h - 4, 8, color_alpha(theme_get()->accent, 220));
        }
        int ix = d->r.x + (d->r.w - ICON_SZ) / 2, iy = d->r.y + 8;
        if (d->app) app_draw_icon(s, d->app, ix, iy, ICON_SZ);
        else ui_draw_glyph(s, d->is_dir ? UI_GLYPH_FOLDER : UI_GLYPH_FILE, ix + 2, iy + 2, ICON_SZ - 4, 0);
        draw_label(s, d->r, d->name, sel);
    }
}

static int icon_at(int x, int y) {
    for (int i = 0; i < nicons; i++)
        if (rect_contains(rect_inset(icons[i].r, 2), x, y)) return i;
    return -1;
}

static void invalidate_icon(int i) {
    if (i >= 0 && i < nicons) win_invalidate_rect(g_desktop_win, icons[i].r.x, icons[i].r.y, icons[i].r.w, icons[i].r.h);
}

static void open_icon(int i) {
    if (i < 0 || i >= nicons) return;
    dicon_t *d = &icons[i];
    if (d->app) { app_launch(d->app, NULL); return; }
    char path[VFS_PATH_MAX];
    vfs_join(DESKTOP_DIR, d->name, path);
    if (d->is_dir) {
        if (!app_launch_by_name("Files", path) && !app_launch_by_name("File Manager", path))
            ui_message_box(NULL, "Desktop", "No file manager is installed.", MB_OK | MB_ICON_INFO, NULL, NULL);
    } else if (!app_open_file(path)) {
        char msg[200];
        snprintf(msg, sizeof msg, "There is no app associated with \"%s\".", d->name);
        ui_message_box(NULL, "Open file", msg, MB_OK | MB_ICON_WARNING, NULL, NULL);
    }
}

static void about_ok(int r, void *c) { (void)r; (void)c; }

static void desktop_menu(int item, void *ctx) {
    (void)ctx;
    switch (item) {
    case 0: rebuild(); win_invalidate(g_desktop_win); break;
    case 1: theme_set_wallpaper((theme_get()->wallpaper + 1) % (theme_wallpaper_count())); break;
    case 2: theme_set_dark(!theme_get()->dark); break;
    case 3:
        if (!app_launch_by_name("Settings", NULL))
            ui_message_box(NULL, "Settings", "The Settings app is not installed.", MB_OK, about_ok, NULL);
        break;
    case 5:
        if (!app_launch_by_name("About", NULL))
            ui_message_box(NULL, "About NovaOS", sys_version(), MB_OK, about_ok, NULL);
        break;
    default: break;
    }
}

static void icon_menu(int item, void *ctx) {
    int i = (int)(intptr_t)ctx;
    if (item == 0) open_icon(i);
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
            if (h >= 0) ui_popup_menu(win, ev->x, ev->y, "Open", icon_menu, (void *)(intptr_t)h);
            else ui_popup_menu(win, ev->x, ev->y, "Refresh|Next wallpaper|Toggle dark mode|Settings...|-|About NovaOS", desktop_menu, NULL);
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
        else break;
        s = CLAMP(s, 0, nicons - 1);
        invalidate_icon(selected);
        selected = s;
        invalidate_icon(selected);
        break;
    }
    case EV_TIMER:
        if (vfs_generation() != vfs_gen_seen) {
            int before = nicons;
            rebuild();
            (void)before;
            win_invalidate(win);
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
