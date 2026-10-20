/*
 * open.c - shell_open(): open files with their default app, folders in Files,
 * apps by name; recent items; taskbar pins.
 */
#include "shell/shell_internal.h"

/* ---- recent items ------------------------------------------------------------------------- */
#define MAX_RECENT 16

static char recent[MAX_RECENT][VFS_PATH_MAX];
static int nrecent;
static bool recent_loaded;

static void recent_save(void) {
    char *buf = kmalloc(MAX_RECENT * (VFS_PATH_MAX + 1) + 64);
    if (!buf) return;
    int n = snprintf(buf, 64, "# recent items, newest first\n");
    for (int i = 0; i < nrecent; i++) n += snprintf(buf + n, VFS_PATH_MAX + 2, "%s\n", recent[i]);
    config_write("recent", buf);
    kfree(buf);
}

static void recent_load(void) {
    if (recent_loaded) return;
    recent_loaded = true;
    nrecent = 0;
    char *text = config_read("recent");
    if (!text) return;
    char *cur = text, *line;
    while ((line = config_next_line(&cur)) && nrecent < MAX_RECENT)
        if (*line && *line != '#') strlcpy(recent[nrecent++], line, VFS_PATH_MAX);
    free(text);
}

void shell_recent_add(const char *item) {
    if (!item || !*item) return;
    recent_load();
    int found = -1;
    for (int i = 0; i < nrecent; i++) if (!strcmp(recent[i], item)) { found = i; break; }
    if (found == 0) return;
    if (found < 0) found = nrecent < MAX_RECENT ? nrecent++ : MAX_RECENT - 1;
    memmove(recent[1], recent[0], (size_t)found * VFS_PATH_MAX);
    strlcpy(recent[0], item, VFS_PATH_MAX);
    recent_save();
    shell_bump();
}

void shell_recent_add_app(const char *app_name) {
    char buf[VFS_PATH_MAX];
    snprintf(buf, sizeof buf, "app:%s", app_name);
    shell_recent_add(buf);
}

/* Drop entries whose file or app disappeared. */
static void recent_prune(void) {
    int j = 0;
    for (int i = 0; i < nrecent; i++) {
        bool ok = !strncmp(recent[i], "app:", 4) ? app_find(recent[i] + 4) != NULL : vfs_exists(recent[i]);
        if (ok) { if (i != j) strlcpy(recent[j], recent[i], VFS_PATH_MAX); j++; }
    }
    if (j != nrecent) { nrecent = j; recent_save(); }
}

int shell_recent_count(void) { recent_load(); recent_prune(); return nrecent; }
const char *shell_recent_get(int i) { recent_load(); return (i >= 0 && i < nrecent) ? recent[i] : NULL; }
void shell_recent_clear(void) { recent_load(); nrecent = 0; recent_save(); shell_bump(); }

/* Called by app_launch() for every launch: remember files, folders and apps. */
void shell_note_launch(const app_desc_t *app, const char *arg) {
    if (!app || (app->flags & APP_HIDDEN)) return;
    if (arg && arg[0] == '/') {
        if (vfs_is_dir(arg)) {
            const app_desc_t *fm = assoc_default_app(arg);
            if (fm == app) shell_recent_add(arg);
            else shell_recent_add_app(app->name);
            return;
        }
        if (vfs_exists(arg)) { shell_recent_add(arg); return; }
    }
    shell_recent_add_app(app->name);
}

/* ---- opening -------------------------------------------------------------------------------- */
static void info_box(const char *title, const char *fmt, const char *arg) {
    char msg[320];
    snprintf(msg, sizeof msg, fmt, arg);
    ui_message_box(NULL, title, msg, MB_OK | MB_ICON_WARNING, NULL, NULL);
}

bool shell_open_with(const char *path, const char *app_name) {
    const app_desc_t *a = app_find(app_name);
    if (!a) return false;
    char norm[VFS_PATH_MAX];
    if (!path || vfs_normalize(NULL, path, norm) != 0) return app_launch(a, NULL);
    return app_launch(a, norm);
}

bool shell_open(const char *path) {
    if (!path) return false;
    while (*path == ' ') path++;
    if (!*path) return false;
    if (!strncmp(path, "app:", 4)) return app_launch_by_name(path + 4, NULL);
    char norm[VFS_PATH_MAX];
    if (path[0] == '~' && (path[1] == 0 || path[1] == '/')) {
        char tmp[VFS_PATH_MAX];
        snprintf(tmp, sizeof tmp, "/home/user%s", path + 1);
        if (vfs_normalize(NULL, tmp, norm) != 0) return false;
    } else if (path[0] == '/') {
        if (vfs_normalize(NULL, path, norm) != 0) return false;
    } else {
        const app_desc_t *a = app_find(path);
        if (a) return app_launch(a, NULL);
        if (vfs_normalize("/home/user", path, norm) != 0) return false;
    }
    if (!vfs_exists(norm)) {
        const app_desc_t *a = app_find(path);
        if (a) return app_launch(a, NULL);
        log_warn("shell", "open: %s does not exist", norm);
        return false;
    }
    const app_desc_t *a = assoc_default_app(norm);
    if (vfs_is_dir(norm)) {
        if (!a) { info_box("Open folder", "No file manager is installed to open \"%s\".", vfs_basename(norm)); return false; }
        return app_launch(a, norm);
    }
    if (a) return app_launch(a, norm);
    return shell_open_with_dialog(norm) != NULL;
}

/* ---- taskbar pins ----------------------------------------------------------------------------- */
#define MAX_PINS 16

static char pins[MAX_PINS][48];
static int npins;
static bool pins_loaded;

static void pins_save(void) {
    char buf[MAX_PINS * 50 + 64];
    int n = snprintf(buf, 64, "# apps pinned to the taskbar, in order\n");
    for (int i = 0; i < npins; i++) n += snprintf(buf + n, 50, "%s\n", pins[i]);
    config_write("pinned", buf);
}

static void pins_load(void) {
    if (pins_loaded) return;
    pins_loaded = true;
    npins = 0;
    char *text = config_read("pinned");
    if (text) {
        char *cur = text, *line;
        while ((line = config_next_line(&cur)) && npins < MAX_PINS)
            if (*line && *line != '#') strlcpy(pins[npins++], line, sizeof pins[0]);
        free(text);
        return;
    }
    static const char *const defaults[] = { "Files", "Terminal", "Notepad" };
    for (int i = 0; i < ARRAY_SIZE(defaults); i++) strlcpy(pins[npins++], defaults[i], sizeof pins[0]);
}

/* Reload at INIT_LATE: a persisted tree may have been restored at INIT_FS. */
static void shell_late_init(void) { pins_loaded = recent_loaded = false; pins_load(); recent_load(); }
REGISTER_INIT(INIT_LATE, shell_late_init);

/* Only installed apps count. */
int shell_pin_count(void) {
    pins_load();
    int n = 0;
    for (int i = 0; i < npins; i++) if (app_find(pins[i])) n++;
    return n;
}

const char *shell_pin_get(int index) {
    pins_load();
    for (int i = 0; i < npins; i++) {
        const app_desc_t *a = app_find(pins[i]);
        if (a && index-- == 0) return a->name;
    }
    return NULL;
}

bool shell_is_pinned(const char *app_name) {
    pins_load();
    const app_desc_t *a = app_find(app_name);
    if (!a) return false;
    for (int i = 0; i < npins; i++) if (app_find(pins[i]) == a) return true;
    return false;
}

bool shell_set_pinned(const char *app_name, bool pinned) {
    pins_load();
    const app_desc_t *a = app_find(app_name);
    if (!a) return false;
    int idx = -1;
    for (int i = 0; i < npins; i++) if (app_find(pins[i]) == a) { idx = i; break; }
    if (pinned) {
        if (idx >= 0) return true;
        if (npins >= MAX_PINS) return false;
        strlcpy(pins[npins++], a->name, sizeof pins[0]);
    } else {
        if (idx < 0) return true;
        memmove(pins[idx], pins[idx + 1], sizeof pins[0] * (size_t)(npins - idx - 1));
        npins--;
    }
    pins_save();
    shell_bump();
    taskbar_refresh();
    log_info("shell", "%s %s the taskbar", a->name, pinned ? "pinned to" : "unpinned from");
    return true;
}
