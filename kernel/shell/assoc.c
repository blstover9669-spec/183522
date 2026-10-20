/*
 * assoc.c - file type registry (REGISTER_FILETYPE, the ".filetypes" linker
 * section) and default-app resolution with user overrides persisted in
 * /system/config/assoc ("ext=App Name" per line).
 */
#include "shell/shell_internal.h"

extern const filetype_desc_t *const __filetypes_start[];
extern const filetype_desc_t *const __filetypes_end[];

/* Built-in associations. Apps that are not installed are skipped at lookup time. */
REGISTER_FILETYPE("folder", "Files", "Folder", ICON_FOLDER);
REGISTER_FILETYPE("txt", "Notepad", "Text document", ICON_FILE_TEXT);
REGISTER_FILETYPE("md", "Notepad", "Markdown document", ICON_FILE_TEXT);
REGISTER_FILETYPE("log", "Notepad", "Log file", ICON_FILE_LOG);
REGISTER_FILETYPE("cfg", "Notepad", "Configuration file", ICON_FILE_CONFIG);
REGISTER_FILETYPE("c", "Notepad", "C source file", ICON_FILE_CODE);
REGISTER_FILETYPE("h", "Notepad", "C header file", ICON_FILE_CODE);
REGISTER_FILETYPE("asm", "Notepad", "Assembly source", ICON_FILE_CODE);
REGISTER_FILETYPE("bmp", "Image Viewer", "Bitmap image", ICON_FILE_IMAGE);
REGISTER_FILETYPE_ALT("bmp", "Paint", "Bitmap image", ICON_FILE_IMAGE);
REGISTER_FILETYPE("ppm", "Image Viewer", "PPM image", ICON_FILE_IMAGE);
REGISTER_FILETYPE_ALT("ppm", "Paint", "PPM image", ICON_FILE_IMAGE);
REGISTER_FILETYPE("note", "Sticky Notes", "Sticky note", ICON_NOTE);
REGISTER_FILETYPE("*", "Hex Viewer", NULL, 0);

#define MAX_USER 64
static struct { char ext[16]; char app[48]; } user_defs[MAX_USER];
static int nuser;
static bool loaded;

static int nfiletypes(void) { return (int)(__filetypes_end - __filetypes_start); }

static void lower_ext(const char *in, char *out, size_t n) {
    size_t i = 0;
    for (; in && in[i] && i < n - 1; i++) out[i] = (char)tolower((unsigned char)in[i]);
    out[i] = 0;
}

static void load(void) {
    if (loaded) return;
    loaded = true;
    nuser = 0;
    char *text = config_read("assoc");
    if (!text) return;
    char *cur = text, *line;
    while ((line = config_next_line(&cur)) && nuser < MAX_USER) {
        if (!*line || *line == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        lower_ext(line, user_defs[nuser].ext, sizeof user_defs[nuser].ext);
        strlcpy(user_defs[nuser].app, eq + 1, sizeof user_defs[nuser].app);
        if (user_defs[nuser].ext[0] && user_defs[nuser].app[0]) nuser++;
    }
    free(text);
}

static void save(void) {
    char *buf = kmalloc(MAX_USER * 72 + 64);
    if (!buf) return;
    int n = snprintf(buf, 64, "# default apps: extension=App Name\n");
    for (int i = 0; i < nuser; i++) n += snprintf(buf + n, 72, "%s=%s\n", user_defs[i].ext, user_defs[i].app);
    config_write("assoc", buf);
    kfree(buf);
}

/* Reload on INIT_LATE: a persisted tree may have been restored at INIT_FS. */
static void assoc_late_init(void) { loaded = false; load(); }
REGISTER_INIT(INIT_LATE, assoc_late_init);

const char *assoc_user_default(const char *ext) {
    load();
    char e[16];
    lower_ext(ext, e, sizeof e);
    for (int i = 0; i < nuser; i++) if (!strcmp(user_defs[i].ext, e)) return user_defs[i].app;
    return NULL;
}

bool assoc_set_default(const char *ext, const char *app_name) {
    load();
    char e[16];
    lower_ext(ext, e, sizeof e);
    if (!e[0]) return false;
    int idx = -1;
    for (int i = 0; i < nuser; i++) if (!strcmp(user_defs[i].ext, e)) idx = i;
    if (!app_name || !*app_name) {
        if (idx >= 0) { user_defs[idx] = user_defs[--nuser]; save(); shell_bump(); }
        return true;
    }
    const app_desc_t *a = app_find(app_name);
    if (!a) return false;
    if (idx < 0) {
        if (nuser >= MAX_USER) return false;
        idx = nuser++;
        strlcpy(user_defs[idx].ext, e, sizeof user_defs[idx].ext);
    }
    strlcpy(user_defs[idx].app, a->name, sizeof user_defs[idx].app);
    save();
    shell_bump();
    log_info("assoc", ".%s files now open with %s", e, a->name);
    return true;
}

static bool list_has(const char *list, const char *ext) {
    if (!list || !*ext) return false;
    size_t el = strlen(ext);
    const char *p = list;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == ';') p++;
        const char *s = p;
        while (*p && *p != ',' && *p != ' ' && *p != ';') p++;
        if ((size_t)(p - s) == el && !strncasecmp(s, ext, el)) return true;
    }
    return false;
}

/* key for a path: "folder" for directories, else the lower-case extension */
static void path_key(const char *path, char *out, size_t n) {
    if (vfs_is_dir(path)) { strlcpy(out, "folder", n); return; }
    lower_ext(vfs_extension(path), out, n);
}

bool assoc_app_handles(const app_desc_t *a, const char *path) {
    char key[16];
    path_key(path, key, sizeof key);
    if (!key[0]) return false;
    for (int i = 0; i < nfiletypes(); i++) {
        const filetype_desc_t *f = __filetypes_start[i];
        if (f && f->ext && f->app && !strcasecmp(f->ext, key) && app_find(f->app) == a) return true;
    }
    return strcmp(key, "folder") && list_has(a->file_types, key);
}

const app_desc_t *assoc_default_app(const char *path) {
    if (!path) return NULL;
    char key[16];
    path_key(path, key, sizeof key);
    if (!key[0]) return NULL;
    const char *u = assoc_user_default(key);
    const app_desc_t *a;
    if (u && (a = app_find(u))) return a;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nfiletypes(); i++) {
            const filetype_desc_t *f = __filetypes_start[i];
            if (!f || !f->ext || !f->app || f->alt != pass || strcasecmp(f->ext, key)) continue;
            if ((a = app_find(f->app))) return a;
        }
        if (pass == 0 && strcmp(key, "folder")) {
            for (int i = 0; i < app_count(); i++)
                if (list_has(app_get(i)->file_types, key)) return app_get(i);
        }
    }
    if (!strcmp(key, "folder")) return app_find("File Manager");
    return NULL;
}

static int add_unique(const app_desc_t **out, int n, int max, const app_desc_t *a) {
    if (!a) return n;
    for (int i = 0; i < n && i < max; i++) if (out[i] == a) return n;
    if (n < max) out[n] = a;
    return n + 1;
}

int assoc_apps(const char *path, const app_desc_t **out, int max) {
    char key[16];
    path_key(path, key, sizeof key);
    int n = 0;
    const app_desc_t *tmp[64];
    int cap = MIN(max, 64);
    if (!out) { out = tmp; cap = 64; }
    n = add_unique(out, n, cap, assoc_default_app(path));
    if (key[0]) {
        for (int i = 0; i < nfiletypes(); i++) {
            const filetype_desc_t *f = __filetypes_start[i];
            if (f && f->ext && f->app && !strcasecmp(f->ext, key)) n = add_unique(out, n, cap, app_find(f->app));
        }
        if (strcmp(key, "folder"))
            for (int i = 0; i < app_count(); i++)
                if (list_has(app_get(i)->file_types, key)) n = add_unique(out, n, cap, app_get(i));
    }
    if (!vfs_is_dir(path)) {
        for (int i = 0; i < nfiletypes(); i++) {
            const filetype_desc_t *f = __filetypes_start[i];
            if (f && f->ext && f->app && !strcmp(f->ext, "*")) n = add_unique(out, n, cap, app_find(f->app));
        }
    }
    return n;
}

int filetype_icon(const char *ext) {
    if (!ext || !*ext) return 0;
    for (int i = 0; i < nfiletypes(); i++) {
        const filetype_desc_t *f = __filetypes_start[i];
        if (f && f->ext && f->icon && !strcasecmp(f->ext, ext)) return f->icon;
    }
    return 0;
}

const char *filetype_description(const char *path) {
    static char buf[48];
    if (!path) return "File";
    if (vfs_is_dir(path)) return "Folder";
    const char *ext = vfs_extension(path);
    if (!*ext) return "File";
    for (int i = 0; i < nfiletypes(); i++) {
        const filetype_desc_t *f = __filetypes_start[i];
        if (f && f->ext && f->description && !strcasecmp(f->ext, ext)) return f->description;
    }
    static const char *const table[][2] = {
        { "text", "Text document" }, { "ini", "Settings file" }, { "conf", "Settings file" },
        { "json", "JSON file" }, { "xml", "XML document" }, { "html", "Web page" }, { "css", "Style sheet" },
        { "py", "Python script" }, { "sh", "Shell script" }, { "js", "JavaScript file" },
        { "cpp", "C++ source file" }, { "s", "Assembly source" }, { "png", "PNG image" },
        { "jpg", "JPEG image" }, { "gif", "GIF image" }, { "wav", "WAV audio" }, { "mid", "MIDI music" },
        { "mp3", "MP3 audio" }, { "zip", "ZIP archive" }, { "tar", "TAR archive" }, { "gz", "GZip archive" },
        { "bin", "Binary file" }, { "img", "Disk image" }, { "iso", "Disc image" }, { "elf", "Program" },
    };
    for (int i = 0; i < ARRAY_SIZE(table); i++)
        if (!strcasecmp(ext, table[i][0])) return table[i][1];
    char up[16];
    int n = 0;
    for (; ext[n] && n < 15; n++) up[n] = (char)toupper((unsigned char)ext[n]);
    up[n] = 0;
    snprintf(buf, sizeof buf, "%s file", up);
    return buf;
}

static int cmp_str(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int assoc_extensions(const char **out, int max) {
    static char exts[128][16];
    int n = 0;
    #define ADD_EXT(e, len) do {                                                     \
        char tmp_[16]; size_t l_ = MIN((size_t)(len), sizeof tmp_ - 1);            \
        for (size_t k_ = 0; k_ < l_; k_++) tmp_[k_] = (char)tolower((unsigned char)(e)[k_]); \
        tmp_[l_] = 0;                                                               \
        bool dup_ = !tmp_[0] || !strcmp(tmp_, "*");                                 \
        for (int j_ = 0; j_ < n && !dup_; j_++) if (!strcmp(exts[j_], tmp_)) dup_ = true; \
        if (!dup_ && n < 128) strlcpy(exts[n++], tmp_, 16);                         \
    } while (0)
    for (int i = 0; i < nfiletypes(); i++) {
        const filetype_desc_t *f = __filetypes_start[i];
        if (f && f->ext) ADD_EXT(f->ext, strlen(f->ext));
    }
    for (int i = 0; i < app_count(); i++) {
        const char *p = app_get(i)->file_types;
        while (p && *p) {
            while (*p == ',' || *p == ' ' || *p == ';') p++;
            const char *s = p;
            while (*p && *p != ',' && *p != ' ' && *p != ';') p++;
            if (p > s) ADD_EXT(s, p - s);
        }
    }
    load();
    for (int i = 0; i < nuser; i++) ADD_EXT(user_defs[i].ext, strlen(user_defs[i].ext));
    #undef ADD_EXT
    qsort(exts, (size_t)n, sizeof exts[0], cmp_str);
    for (int i = 0; i < n && i < max; i++) out[i] = exts[i];
    return n;
}
