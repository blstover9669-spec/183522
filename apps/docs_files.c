/*
 * docs_files.c - "Files": the NovaOS file manager.
 *
 *   +-----------------------------------------------------------------+
 *   | [<] [>] [^]  [ /home/user/Documents          ] [+] [ ## | == ]  |  toolbar
 *   +-----------+-----------------------------------------------------+
 *   | Places    |  icon grid or details list (name/size/type/date)    |
 *   |  Home     |  with its own scrollbar, hover, multi-selection,    |
 *   |  Desktop  |  rubber-band selection, context menus              |
 *   |  ...      |                                                     |
 *   +-----------+-----------------------------------------------------+
 *   | 12 items, 2 selected (3.4 KB)                        1.2 MB used |  status bar
 *   +-----------------------------------------------------------------+
 *
 * Features: address bar (Enter to go, Ctrl+L to focus), back / forward / up
 * with history, places sidebar, icon and details views (sortable columns),
 * folder and file-type icons from the system icon library (badged with the
 * app that opens the file), sizes and dates, double-click to open (folders
 * navigate, files open with shell_open: the default app from the file
 * association registry, or the Open With dialog), Open With..., new folder /
 * new text file, rename (F2), delete with confirmation (Del), cut / copy /
 * paste of files and folders (Ctrl+X / C / V, shared between Files windows
 * and done in timer-driven chunks), the system Properties dialog,
 * type-to-jump, and automatic refresh when the file system changes
 * (vfs_generation).
 *
 * Per-window state lives in files_t (win_set_userdata) and is freed on
 * EV_CLOSE. Global: the file clipboard (shared on purpose).
 */
#include <nova.h>

#define FM_W        800
#define FM_H        500
#define TOOLBAR_H   48
#define SIDEBAR_W   176
#define STATUS_H    26
#define HEADER_H    28
#define ROW_H       26
#define CELL_MIN_W  100
#define CELL_H      98
#define ICON_BIG    48
#define ICON_SMALL  20
#define SB_W        12
#define HIST_MAX    24
#define TB_BTN      32

enum { VIEW_ICONS, VIEW_DETAILS };
enum { SORT_NAME, SORT_SIZE, SORT_TYPE, SORT_DATE };

/* file kinds (sorting, thumbnails); the icons come from the system icon library */
enum { K_FOLDER, K_TEXT, K_CODE, K_IMAGE, K_AUDIO, K_ARCHIVE, K_BINARY, K_FILE, K_COUNT };

/* toolbar buttons */
enum { TB_NONE = -1, TB_BACK, TB_FWD, TB_UP, TB_NEWDIR, TB_VIEW_ICONS, TB_VIEW_LIST, TB_COUNT };

typedef struct {
    char     name[VFS_NAME_MAX];
    bool     is_dir;
    bool     sel;
    bool     thumb_tried;       /* image: thumbnail generation attempted */
    uint8_t  kind;
    uint8_t  icon;              /* ICON_* from the icon library (icon_for_path) */
    uint32_t size, mtime;
    surface_t *thumb;           /* image preview for the icon view (owned) */
} entry_t;

/* A paste in progress: processed a few items per timer tick. */
typedef struct {
    char (*src)[VFS_PATH_MAX];
    char (*made)[VFS_NAME_MAX];   /* names created in dest, selected afterwards */
    int  count, next, nmade, failed;
    bool cut;
    char dest[VFS_PATH_MAX];
    char err[128];
} paste_job_t;

typedef struct files {
    window_t *win;
    widget_t *addr, *view;
    char dir[VFS_PATH_MAX];
    entry_t *ents;
    int  nents;
    int  view_mode, sort, sort_desc;
    int  scroll;                    /* view scroll offset in pixels */
    int  focus, anchor;             /* keyboard cursor / Shift anchor (-1 = none) */
    int  hover;                     /* entry under the pointer (-1 = none) */
    int  tb_hover, tb_pressed;      /* toolbar button states */
    int  side_hover;
    /* rubber band (content coordinates) */
    bool banding;
    int  band_x0, band_y0, band_x1, band_y1;
    bool *band_base;                /* selection before the band started */
    bool sb_drag;
    int  sb_off;
    /* history */
    char back[HIST_MAX][VFS_PATH_MAX];
    int  nback;
    char fwd[HIST_MAX][VFS_PATH_MAX];
    int  nfwd;
    uint32_t gen;                   /* vfs_generation() of the last load */
    int  timer, job_timer, thumb_timer;
    char msg[160];
    uint32_t msg_until;
    /* data for pending dialogs */
    char pending_name[VFS_NAME_MAX];        /* rename source           */
    char (*pending_del)[VFS_PATH_MAX];      /* delete confirmation     */
    int  npending_del;
    char select_after[VFS_NAME_MAX];        /* select after next load  */
    paste_job_t *job;
} files_t;

/* ---- the file clipboard (shared by all Files windows) ------------------------------------ */
/* Copy/cut also put the paths on the system text clipboard; if that text changes
   afterwards (the user copied something else), the file clipboard is stale. */
static struct {
    char (*paths)[VFS_PATH_MAX];
    int  count;
    bool cut;
    char *text;
} g_fclip;

static bool fclip_valid(void) {
    return g_fclip.count > 0 && g_fclip.text && !strcmp(clipboard_get(), g_fclip.text);
}

static void fclip_clear(void) {
    free(g_fclip.paths);
    free(g_fclip.text);
    g_fclip.paths = NULL;
    g_fclip.text = NULL;
    g_fclip.count = 0;
}

static bool fclip_is_cut(const char *path) {
    if (!g_fclip.cut || !fclip_valid()) return false;
    for (int i = 0; i < g_fclip.count; i++)
        if (!strcmp(g_fclip.paths[i], path)) return true;
    return false;
}

/* ---- formatting helpers ----------------------------------------------------------------------- */
static void fmt_size(uint32_t n, char *buf, size_t sz) {
    if (n < 1024) snprintf(buf, sz, "%u byte%s", n, n == 1 ? "" : "s");
    else if (n < 100 * 1024) snprintf(buf, sz, "%.1f KB", n / 1024.0);
    else if (n < 1024 * 1024) snprintf(buf, sz, "%u KB", (n + 512) / 1024);
    else snprintf(buf, sz, "%.1f MB", n / (1024.0 * 1024.0));
}

static void fmt_date(uint32_t t, char *buf, size_t sz) {
    if (!t) { snprintf(buf, sz, "-"); return; }
    datetime_t dt, now;
    sys_time_to_datetime(t, &dt);
    sys_datetime(&now);
    if (dt.year == now.year && dt.month == now.month && dt.day == now.day)
        snprintf(buf, sz, "Today, %02d:%02d", dt.hour, dt.minute);
    else
        snprintf(buf, sz, "%04d-%02d-%02d %02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute);
}

static bool ext_in(const char *ext, const char *list) {
    size_t el = strlen(ext);
    if (!el) return false;
    const char *p = list;
    while (*p) {
        const char *s = p;
        while (*p && *p != ',') p++;
        if ((size_t)(p - s) == el && !strncasecmp(s, ext, el)) return true;
        if (*p) p++;
    }
    return false;
}

static const char EXT_TEXT[]    = "txt,text,log,ini,cfg,conf,nfo,md,csv,readme,me";
static const char EXT_CODE[]    = "c,h,cpp,hpp,asm,s,py,js,sh,json,xml,html,htm,css,lua,bas,mk,ld";
static const char EXT_IMAGE[]   = "bmp,ppm,pgm,pbm,png,jpg,jpeg,gif,ico,tga,img_";
static const char EXT_AUDIO[]   = "wav,mid,midi,mp3,ogg,mod,snd,voc,song";
static const char EXT_ARCHIVE[] = "zip,tar,gz,tgz,bz2,xz,7z,rar,iso,nvfs";
static const char EXT_BINARY[]  = "elf,bin,exe,com,sys,dat,o,a,so,dll,img";

static int kind_for(const char *name, bool is_dir) {
    if (is_dir) return K_FOLDER;
    const char *e = vfs_extension(name);
    if (ext_in(e, EXT_TEXT)) return K_TEXT;
    if (ext_in(e, EXT_CODE)) return K_CODE;
    if (ext_in(e, EXT_IMAGE)) return K_IMAGE;
    if (ext_in(e, EXT_AUDIO)) return K_AUDIO;
    if (ext_in(e, EXT_ARCHIVE)) return K_ARCHIVE;
    if (ext_in(e, EXT_BINARY)) return K_BINARY;
    return K_FILE;
}

static const char *type_name(const entry_t *e, char *buf, size_t sz) {
    if (e->is_dir) return "Folder";
    static const char *table[][2] = {
        { "txt", "Text document" }, { "text", "Text document" }, { "md", "Markdown document" },
        { "log", "Log file" }, { "ini", "Settings file" }, { "cfg", "Settings file" },
        { "conf", "Settings file" }, { "csv", "CSV spreadsheet" }, { "nfo", "Info file" },
        { "c", "C source file" }, { "h", "C header file" }, { "cpp", "C++ source file" },
        { "asm", "Assembly source" }, { "s", "Assembly source" }, { "py", "Python script" },
        { "sh", "Shell script" }, { "js", "JavaScript file" }, { "json", "JSON file" },
        { "xml", "XML document" }, { "html", "Web page" }, { "htm", "Web page" }, { "css", "Style sheet" },
        { "bmp", "Bitmap image" }, { "ppm", "PPM image" }, { "png", "PNG image" }, { "jpg", "JPEG image" },
        { "jpeg", "JPEG image" }, { "gif", "GIF image" }, { "ico", "Icon" },
        { "wav", "WAV audio" }, { "mid", "MIDI music" }, { "midi", "MIDI music" }, { "mp3", "MP3 audio" },
        { "zip", "ZIP archive" }, { "tar", "TAR archive" }, { "gz", "GZip archive" }, { "iso", "Disc image" },
        { "elf", "Program" }, { "exe", "Program" }, { "bin", "Binary file" }, { "img", "Disk image" },
    };
    const char *ext = vfs_extension(e->name);
    for (int i = 0; i < ARRAY_SIZE(table); i++)
        if (!strcasecmp(ext, table[i][0])) return table[i][1];
    if (!*ext) return "File";
    /* types registered by apps (REGISTER_FILETYPE), else "EXT file"; the
       directory part only has to be one that is not a folder */
    char probe[VFS_NAME_MAX + 8];
    snprintf(probe, sizeof probe, "/\x01/%s", e->name);
    strlcpy(buf, filetype_description(probe), sz);
    return buf;
}

/* ---- icons -------------------------------------------------------------------------------- */
/* Files and folders use the system icon library (nova/icons.h): the same
   hand-drawn icons as the desktop, the Open With and Properties dialogs. */
static void blit_icon(surface_t *s, int x, int y, int size, int icon, int opacity) {
    icon_draw_alpha(icon, s, x, y, size, opacity);
}

#define UX(v) (x + (int)((v) * u + 0.5))
#define UY(v) (y + (int)((v) * u + 0.5))
#define UL(v) ((int)((v) * u + 0.5))

/* The app's own icon: a blue folder with papers (see fm_app_icon). */
static void draw_app_folder(surface_t *s, int x, int y, int size) {
    double u = size / 64.0;
    uint32_t back = RGB(40, 110, 220), top = RGB(110, 180, 255), bot = RGB(55, 135, 240);
    gfx_fill_rounded_rect(s, UX(4), UY(8), UL(24), UL(12), UL(4), back);
    gfx_fill_rounded_rect(s, UX(4), UY(12), UL(56), UL(44), UL(5), back);
    gfx_fill_rect(s, UX(9), UY(16), UL(46), UL(22), RGB(205, 210, 220));
    gfx_fill_rect(s, UX(10), UY(17), UL(44), UL(21), RGB(252, 252, 252));
    for (int i = 0; i < 3; i++) gfx_fill_rect(s, UX(15), UY(21 + i * 4), UL(i == 2 ? 20 : 32), UL(1.8), RGB(170, 185, 210));
    gfx_fill_rounded_gradient_v(s, UX(4), UY(23), UL(56), UL(35), UL(5), top, bot);
    gfx_fill_rect(s, UX(6), UY(24), UL(52), UL(1.2) > 0 ? UL(1.2) : 1, RGBA(255, 255, 255, 120));
}

/* ---- paths and names ------------------------------------------------------------------------ */
static void entry_path(files_t *f, int i, char *out) { vfs_join(f->dir, f->ents[i].name, out); }

static int entry_icon_id(files_t *f, int i) {
    entry_t *e = &f->ents[i];
    return e->icon ? e->icon : (e->is_dir ? ICON_FOLDER : ICON_FILE_UNKNOWN);
}

static bool path_inside(const char *path, const char *dir) {   /* path == dir or below it */
    size_t n = strlen(dir);
    if (strncmp(path, dir, n) != 0) return false;
    return path[n] == 0 || path[n] == '/' || (n == 1 && dir[0] == '/');
}

/* A name not yet used in `dir`: "name", "name (2)", ... or "name - Copy", "name - Copy (2)", ... */
static void unique_name(const char *dir, const char *name, bool is_dir, bool copy_style, char *out) {
    char path[VFS_PATH_MAX];
    if (!copy_style) {
        vfs_join(dir, name, path);
        if (!vfs_exists(path)) { strlcpy(out, name, VFS_NAME_MAX); return; }
    }
    char stem[VFS_NAME_MAX], ext[VFS_NAME_MAX] = "";
    strlcpy(stem, name, sizeof stem);
    char *dot = strrchr(stem, '.');
    if (!is_dir && dot && dot != stem) { strlcpy(ext, dot, sizeof ext); *dot = 0; }
    for (int i = 1; i < 1000; i++) {
        char suffix[24];
        if (copy_style) snprintf(suffix, sizeof suffix, i == 1 ? " - Copy" : " - Copy (%d)", i);
        else snprintf(suffix, sizeof suffix, " (%d)", i + 1);
        int room = VFS_NAME_MAX - 1 - (int)strlen(suffix) - (int)strlen(ext);
        if (room < 1) room = 1;
        char cand[VFS_NAME_MAX];
        snprintf(cand, sizeof cand, "%.*s%s%s", room, stem, suffix, ext);
        vfs_join(dir, cand, path);
        if (!vfs_exists(path)) { strlcpy(out, cand, VFS_NAME_MAX); return; }
    }
    strlcpy(out, name, VFS_NAME_MAX);
}

static bool valid_name(const char *n, char *why, size_t wsz) {
    if (!*n) { snprintf(why, wsz, "The name cannot be empty."); return false; }
    if (strchr(n, '/')) { snprintf(why, wsz, "A name cannot contain the character \"/\"."); return false; }
    if (!strcmp(n, ".") || !strcmp(n, "..")) { snprintf(why, wsz, "\"%s\" is not a valid name.", n); return false; }
    if (strlen(n) >= VFS_NAME_MAX) { snprintf(why, wsz, "The name is too long (at most %d characters).", VFS_NAME_MAX - 1); return false; }
    return true;
}

/* Trim leading and trailing spaces in place. */
static char *trim(char *s) {
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
    return s;
}

/* ---- geometry ---------------------------------------------------------------------------- */
static rect_t fm_area(files_t *f) {            /* scrolling item area (client coords) */
    rect_t r = ui_rect(f->view);
    if (f->view_mode == VIEW_DETAILS) { r.y += HEADER_H; r.h -= HEADER_H; }
    r.w -= SB_W;
    return r;
}

static int fm_icon_cols(files_t *f, int *cell_w) {
    rect_t a = fm_area(f);
    int avail = MAX(CELL_MIN_W, a.w - 16);
    int cols = MAX(1, avail / CELL_MIN_W);
    if (cell_w) *cell_w = avail / cols;
    return cols;
}

static int fm_content_h(files_t *f) {
    if (f->view_mode == VIEW_DETAILS) return f->nents * ROW_H + 4;
    int cols = fm_icon_cols(f, NULL);
    return ((f->nents + cols - 1) / cols) * CELL_H + 16;
}

static int fm_max_scroll(files_t *f) { return MAX(0, fm_content_h(f) - fm_area(f).h); }

static void fm_clamp_scroll(files_t *f) { f->scroll = CLAMP(f->scroll, 0, fm_max_scroll(f)); }

/* Item rect in content coordinates (no scroll applied, relative to the area origin). */
static rect_t fm_item_content(files_t *f, int i) {
    if (f->view_mode == VIEW_DETAILS) return rect_make(0, 2 + i * ROW_H, fm_area(f).w, ROW_H);
    int cw;
    int cols = fm_icon_cols(f, &cw);
    return rect_make(8 + (i % cols) * cw, 8 + (i / cols) * CELL_H, cw, CELL_H);
}

static rect_t fm_item_rect(files_t *f, int i) {   /* client coordinates */
    rect_t a = fm_area(f), c = fm_item_content(f, i);
    return rect_make(a.x + c.x, a.y + c.y - f->scroll, c.w, c.h);
}

/* The part of an item that reacts to the mouse (icons: the highlight box). */
static rect_t fm_item_hitbox(files_t *f, int i) {
    rect_t r = fm_item_rect(f, i);
    if (f->view_mode == VIEW_ICONS) return rect_inset(r, 3);
    return r;
}

static int fm_hit(files_t *f, int x, int y) {
    rect_t a = fm_area(f);
    if (!rect_contains(a, x, y)) return -1;
    int idx;
    if (f->view_mode == VIEW_DETAILS) {
        int cy = y - a.y - 2 + f->scroll;
        if (cy < 0) return -1;
        idx = cy / ROW_H;
    } else {
        int cw;
        int cols = fm_icon_cols(f, &cw);
        int cx = x - a.x - 8, cy = y - a.y - 8 + f->scroll;
        if (cx < 0 || cy < 0 || cx / cw >= cols) return -1;
        idx = (cy / CELL_H) * cols + cx / cw;
    }
    if (idx < 0 || idx >= f->nents) return -1;
    return rect_contains(fm_item_hitbox(f, idx), x, y) ? idx : -1;
}

static void fm_invalidate_item(files_t *f, int i) {
    if (i < 0 || i >= f->nents) return;
    rect_t r = rect_intersect(fm_item_rect(f, i), fm_area(f));
    if (!rect_empty(r)) win_invalidate_rect(f->win, r.x, r.y, r.w, r.h);
}

static void fm_ensure_visible(files_t *f, int i) {
    if (i < 0 || i >= f->nents) return;
    rect_t a = fm_area(f), c = fm_item_content(f, i);
    if (c.y - 4 < f->scroll) f->scroll = c.y - 4;
    if (c.y + c.h + 4 > f->scroll + a.h) f->scroll = c.y + c.h + 4 - a.h;
    fm_clamp_scroll(f);
}

/* details-view columns */
typedef struct { int name_x, name_w, size_x, size_w, type_x, type_w, date_x, date_w; } cols_t;

static cols_t fm_columns(files_t *f) {
    rect_t a = fm_area(f);
    cols_t c;
    c.size_w = 84; c.type_w = 132; c.date_w = 136;
    int name_w = a.w - c.size_w - c.type_w - c.date_w;
    if (name_w < 170) { name_w += c.type_w; c.type_w = 0; }
    if (name_w < 170) { name_w += c.date_w; c.date_w = 0; }
    c.name_x = a.x; c.name_w = name_w;
    c.size_x = c.name_x + c.name_w;
    c.type_x = c.size_x + c.size_w;
    c.date_x = c.type_x + c.type_w;
    return c;
}

/* toolbar layout */
static rect_t tb_rect(files_t *f, int b) {
    int w = win_width(f->win);
    switch (b) {
    case TB_BACK:       return rect_make(8, 8, TB_BTN, TB_BTN);
    case TB_FWD:        return rect_make(44, 8, TB_BTN, TB_BTN);
    case TB_UP:         return rect_make(80, 8, TB_BTN, TB_BTN);
    case TB_NEWDIR:     return rect_make(w - 8 - 64 - 10 - TB_BTN, 8, TB_BTN, TB_BTN);
    case TB_VIEW_ICONS: return rect_make(w - 8 - 64, 8, 32, TB_BTN);
    case TB_VIEW_LIST:  return rect_make(w - 8 - 32, 8, 32, TB_BTN);
    default:            return rect_make(0, 0, 0, 0);
    }
}

static int tb_hit(files_t *f, int x, int y) {
    for (int b = 0; b < TB_COUNT; b++)
        if (rect_contains(tb_rect(f, b), x, y)) return b;
    return TB_NONE;
}

static bool tb_enabled(files_t *f, int b) {
    switch (b) {
    case TB_BACK: return f->nback > 0;
    case TB_FWD:  return f->nfwd > 0;
    case TB_UP:   return strcmp(f->dir, "/") != 0;
    default:      return true;
    }
}

/* sidebar places */
typedef struct { const char *label, *path; int icon; } place_t;
static const place_t g_places[] = {
    { "Home",      "/home/user",           ICON_FOLDER_HOME },
    { "Desktop",   "/home/user/Desktop",   ICON_FOLDER_DESKTOP },
    { "Documents", "/home/user/Documents", ICON_FOLDER_DOCUMENTS },
    { "Downloads", "/home/user/Downloads", ICON_FOLDER_DOWNLOADS },
    { "Music",     "/home/user/Music",     ICON_FOLDER_MUSIC },
    { "Pictures",  "/home/user/Pictures",  ICON_FOLDER_PICTURES },
    { "Computer",  "/",                    ICON_COMPUTER },
    { "System",    "/system",              ICON_FOLDER_SYSTEM },
    { "Temporary", "/tmp",                 ICON_FOLDER },
};
#define NPLACES_HOME 6     /* the first six are under "Places", the rest under "This computer" */
#define SIDE_ROW 30

static rect_t side_row_rect(files_t *f, int i) {
    int y = TOOLBAR_H + 34 + i * SIDE_ROW + (i >= NPLACES_HOME ? 34 : 0);
    return rect_make(8, y, SIDEBAR_W - 16, SIDE_ROW - 2);
}

static int side_hit(files_t *f, int x, int y) {
    for (int i = 0; i < ARRAY_SIZE(g_places); i++)
        if (rect_contains(side_row_rect(f, i), x, y) && vfs_is_dir(g_places[i].path)) return i;
    return -1;
}

/* ---- selection ------------------------------------------------------------------------------- */
static int fm_sel_count(files_t *f) {
    int n = 0;
    for (int i = 0; i < f->nents; i++) n += f->ents[i].sel;
    return n;
}

static int fm_first_sel(files_t *f) {
    for (int i = 0; i < f->nents; i++) if (f->ents[i].sel) return i;
    return -1;
}

static void fm_status_dirty(files_t *f) {
    win_invalidate_rect(f->win, 0, win_height(f->win) - STATUS_H, win_width(f->win), STATUS_H);
}

static void fm_select_only(files_t *f, int i) {
    for (int k = 0; k < f->nents; k++) f->ents[k].sel = (k == i);
    if (i >= 0) { f->focus = f->anchor = i; fm_ensure_visible(f, i); }
    ui_invalidate(f->view);
    fm_status_dirty(f);
}

static void fm_clear_sel(files_t *f) {
    for (int k = 0; k < f->nents; k++) f->ents[k].sel = false;
    ui_invalidate(f->view);
    fm_status_dirty(f);
}

static void fm_select_range(files_t *f, int a, int b, bool add) {
    if (f->nents == 0) return;
    if (a > b) { int t = a; a = b; b = t; }
    for (int k = 0; k < f->nents; k++) {
        bool in = k >= a && k <= b;
        f->ents[k].sel = add ? (f->ents[k].sel || in) : in;
    }
    ui_invalidate(f->view);
    fm_status_dirty(f);
}

static void fm_message(files_t *f, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void fm_message(files_t *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->msg, sizeof f->msg, fmt, ap);
    va_end(ap);
    f->msg_until = sys_uptime_ms() + 4000;
    fm_status_dirty(f);
}

static void fm_error(files_t *f, const char *title, const char *fmt, ...) PRINTF_LIKE(3, 4);
static void fm_error(files_t *f, const char *title, const char *fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ui_message_box(f->win, title, buf, MB_OK | MB_ICON_ERROR, NULL, NULL);
}

/* ---- loading -------------------------------------------------------------------------------- */
static int g_sort_key, g_sort_desc;

static int ent_cmp(const void *pa, const void *pb) {
    const entry_t *a = pa, *b = pb;
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;     /* folders first, always */
    int r = 0;
    switch (g_sort_key) {
    case SORT_SIZE: r = a->size < b->size ? -1 : a->size > b->size; break;
    case SORT_DATE: r = a->mtime < b->mtime ? -1 : a->mtime > b->mtime; break;
    case SORT_TYPE: {
        char ba[32], bb[32];
        r = strcasecmp(type_name(a, ba, sizeof ba), type_name(b, bb, sizeof bb));
        break;
    }
    default: break;
    }
    if (g_sort_desc) r = -r;
    if (!r) {           /* ties (and the name sort itself) by name */
        r = strcasecmp(a->name, b->name);
        if (!r) r = strcmp(a->name, b->name);
        if (g_sort_key == SORT_NAME && g_sort_desc) r = -r;
    }
    return r;
}

static const char *fm_dir_label(const char *dir) {
    if (!strcmp(dir, "/")) return "Computer";
    if (!strcmp(dir, "/home/user")) return "Home";
    return vfs_basename(dir);
}

static void fm_update_title(files_t *f) {
    char t[VFS_NAME_MAX + 16];
    snprintf(t, sizeof t, "%s - Files", fm_dir_label(f->dir));
    win_set_title(f->win, t);
}

/* ---- image thumbnails ------------------------------------------------------------------------ */
#define THUMB_W 64          /* thumbnails fit a 64 x 48 box (the icon area) */
#define THUMB_H 48
#define THUMB_MAX_FILE (4u << 20)

/* Decode an image file and shrink it to fit THUMB_W x THUMB_H (repeated halving, then a
   final bilinear step, so large pictures are properly averaged). */
static surface_t *make_thumb(const char *path, uint32_t size) {
    if (size == 0 || size > THUMB_MAX_FILE) return NULL;
    const char *ext = vfs_extension(path);
    if (strcasecmp(ext, "bmp") && strcasecmp(ext, "ppm")) return NULL;   /* what image_decode reads */
    uint32_t n = 0;
    void *data = vfs_read_file(path, &n);
    if (!data) return NULL;
    surface_t *img = image_decode(data, n);
    free(data);
    if (!img || img->w <= 0 || img->h <= 0) { if (img) surface_destroy(img); return NULL; }
    int tw = THUMB_W, th = MAX(1, img->h * THUMB_W / img->w);
    if (th > THUMB_H) { th = THUMB_H; tw = MAX(1, img->w * THUMB_H / img->h); }
    while (img->w >= tw * 2 && img->h >= th * 2) {
        surface_t *half = surface_scaled(img, img->w / 2, img->h / 2);
        surface_destroy(img);
        if (!half) return NULL;
        img = half;
    }
    surface_t *t = surface_scaled(img, tw, th);
    surface_destroy(img);
    return t;
}

static void free_thumbs(entry_t *ents, int n) {
    for (int i = 0; i < n; i++)
        if (ents[i].thumb) { surface_destroy(ents[i].thumb); ents[i].thumb = NULL; }
}

static bool thumbs_pending(files_t *f) {
    if (f->view_mode != VIEW_ICONS) return false;
    for (int i = 0; i < f->nents; i++)
        if (f->ents[i].kind == K_IMAGE && !f->ents[i].thumb_tried) return true;
    return false;
}

static void fm_thumbs_kick(files_t *f) {
    if (!f->thumb_timer && thumbs_pending(f)) f->thumb_timer = win_set_timer(f->win, 20);
}

/* (Re)read the directory. Keeps the selection (by name) and the scroll offset;
   select_after, if set, replaces the selection with that one entry. */
static void fm_load(files_t *f) {
    /* the folder may have been deleted or renamed: fall back to the nearest parent */
    while (!vfs_is_dir(f->dir) && strcmp(f->dir, "/")) {
        char up[VFS_PATH_MAX];
        vfs_dirname(f->dir, up);
        strlcpy(f->dir, up, sizeof f->dir);
        ui_set_text(f->addr, f->dir);
        fm_update_title(f);
    }
    /* remember selection and focus by name */
    int nsel = fm_sel_count(f);
    char (*selnames)[VFS_NAME_MAX] = nsel ? malloc((size_t)nsel * VFS_NAME_MAX) : NULL;
    int k = 0;
    if (selnames)
        for (int i = 0; i < f->nents; i++)
            if (f->ents[i].sel) strlcpy(selnames[k++], f->ents[i].name, VFS_NAME_MAX);
    char focus_name[VFS_NAME_MAX] = "";
    if (f->focus >= 0 && f->focus < f->nents) strlcpy(focus_name, f->ents[f->focus].name, sizeof focus_name);

    entry_t *old = f->ents;
    int nold = f->nents;
    f->ents = NULL;
    f->nents = 0;
    free(f->band_base);
    f->band_base = NULL;
    f->banding = false;
    int n = vfs_list(f->dir, NULL, 0);
    if (n > 0) {
        vfs_dirent_t *tmp = malloc(sizeof(vfs_dirent_t) * (size_t)n);
        f->ents = calloc((size_t)n, sizeof(entry_t));
        if (tmp && f->ents) {
            int got = MIN(vfs_list(f->dir, tmp, n), n);
            for (int i = 0; i < got; i++) {
                entry_t *e = &f->ents[i];
                strlcpy(e->name, tmp[i].name, sizeof e->name);
                e->is_dir = tmp[i].is_dir;
                e->size = tmp[i].size;
                e->mtime = tmp[i].mtime;
                e->kind = (uint8_t)kind_for(e->name, e->is_dir);
                char ep[VFS_PATH_MAX];
                vfs_join(f->dir, e->name, ep);
                e->icon = (uint8_t)icon_for_path(ep);
            }
            f->nents = got;
        }
        free(tmp);
    }
    g_sort_key = f->sort;
    g_sort_desc = f->sort_desc;
    if (f->nents > 1) qsort(f->ents, (size_t)f->nents, sizeof(entry_t), ent_cmp);

    f->focus = -1;
    f->hover = -1;
    if (f->select_after[0]) {
        for (int i = 0; i < f->nents; i++)
            if (!strcmp(f->ents[i].name, f->select_after)) { f->ents[i].sel = true; f->focus = f->anchor = i; }
        f->select_after[0] = 0;
    } else {
        for (int i = 0; i < f->nents; i++) {
            for (int j = 0; j < k; j++)
                if (!strcmp(f->ents[i].name, selnames[j])) { f->ents[i].sel = true; break; }
            if (focus_name[0] && !strcmp(f->ents[i].name, focus_name)) f->focus = i;
        }
    }
    free(selnames);
    /* keep thumbnails of unchanged images */
    for (int i = 0; i < f->nents; i++) {
        entry_t *e = &f->ents[i];
        if (e->kind != K_IMAGE) continue;
        for (int j = 0; j < nold; j++)
            if ((old[j].thumb || old[j].thumb_tried) && !strcmp(old[j].name, e->name) &&
                old[j].size == e->size && old[j].mtime == e->mtime) {
                e->thumb = old[j].thumb;
                e->thumb_tried = old[j].thumb_tried;
                old[j].thumb = NULL;
                break;
            }
    }
    free_thumbs(old, nold);
    free(old);
    if (f->anchor >= f->nents) f->anchor = -1;
    fm_clamp_scroll(f);
    if (f->focus >= 0) fm_ensure_visible(f, f->focus);
    f->gen = vfs_generation();
    fm_thumbs_kick(f);
    win_invalidate(f->win);
}

static void fm_set_dir(files_t *f, const char *path) {
    strlcpy(f->dir, path, sizeof f->dir);
    f->scroll = 0;
    f->focus = f->anchor = -1;
    free_thumbs(f->ents, f->nents);
    free(f->ents);
    f->ents = NULL;
    f->nents = 0;
    ui_set_text(f->addr, f->dir);
    fm_update_title(f);
    fm_load(f);
}

static void hist_push(char (*stack)[VFS_PATH_MAX], int *n, const char *path) {
    if (*n == HIST_MAX) { memmove(stack, stack + 1, sizeof stack[0] * (HIST_MAX - 1)); (*n)--; }
    strlcpy(stack[(*n)++], path, VFS_PATH_MAX);
}

/* Go to a folder (records history). Returns false if it is not a folder. */
static bool fm_navigate(files_t *f, const char *path) {
    char norm[VFS_PATH_MAX];
    if (vfs_normalize(f->dir, path, norm) != 0 || !vfs_is_dir(norm)) return false;
    if (strcmp(norm, f->dir) != 0) {
        hist_push(f->back, &f->nback, f->dir);
        f->nfwd = 0;
    }
    fm_set_dir(f, norm);
    return true;
}

static void fm_go_back(files_t *f) {
    while (f->nback > 0) {
        char p[VFS_PATH_MAX];
        strlcpy(p, f->back[--f->nback], sizeof p);
        if (!vfs_is_dir(p)) continue;
        hist_push(f->fwd, &f->nfwd, f->dir);
        fm_set_dir(f, p);
        return;
    }
    win_invalidate(f->win);
}

static void fm_go_forward(files_t *f) {
    while (f->nfwd > 0) {
        char p[VFS_PATH_MAX];
        strlcpy(p, f->fwd[--f->nfwd], sizeof p);
        if (!vfs_is_dir(p)) continue;
        hist_push(f->back, &f->nback, f->dir);
        fm_set_dir(f, p);
        return;
    }
    win_invalidate(f->win);
}

static void fm_go_up(files_t *f) {
    if (!strcmp(f->dir, "/")) return;
    char up[VFS_PATH_MAX];
    vfs_dirname(f->dir, up);
    strlcpy(f->select_after, vfs_basename(f->dir), sizeof f->select_after);   /* highlight where we came from */
    if (!fm_navigate(f, up)) f->select_after[0] = 0;
}

/* ---- opening ------------------------------------------------------------------------------ */
static bool looks_textual(const char *path) {
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return false;
    unsigned char buf[512];
    int n = vfs_read(fd, buf, sizeof buf);
    vfs_close(fd);
    if (n < 0) return false;
    int bad = 0;
    for (int i = 0; i < n; i++) {
        if (buf[i] == 0) return false;
        if (buf[i] < 32 && buf[i] != '\n' && buf[i] != '\r' && buf[i] != '\t') bad++;
    }
    return bad * 20 <= n;
}

/* Open a file the way the rest of the desktop does (shell_open: the user's
   default app for the type, else the registered / declared app, else the
   Open With dialog). Untyped files that are plain text go straight to
   Notepad instead of asking. */
static void fm_open_file(files_t *f, const char *path) {
    (void)f;
    if (!assoc_default_app(path) && looks_textual(path) && shell_open_with(path, "Notepad")) return;
    shell_open(path);
}

static void fm_open_with(files_t *f) {
    int i = fm_first_sel(f);
    if (i < 0 || f->ents[i].is_dir) return;
    char p[VFS_PATH_MAX];
    entry_path(f, i, p);
    shell_open_with_dialog(p);
}

static void fm_open_entry(files_t *f, int i) {
    if (i < 0 || i >= f->nents) return;
    char p[VFS_PATH_MAX];
    entry_path(f, i, p);
    if (f->ents[i].is_dir) fm_navigate(f, p);
    else fm_open_file(f, p);
}

static void fm_open_selection(files_t *f) {
    int n = fm_sel_count(f);
    if (n == 0) { if (f->focus >= 0) fm_open_entry(f, f->focus); return; }
    if (n == 1) { fm_open_entry(f, fm_first_sel(f)); return; }
    /* several: open every selected file (folders would replace each other) */
    char (*paths)[VFS_PATH_MAX] = malloc((size_t)n * VFS_PATH_MAX);
    if (!paths) return;
    int k = 0;
    for (int i = 0; i < f->nents; i++)
        if (f->ents[i].sel && !f->ents[i].is_dir) entry_path(f, i, paths[k++]);
    for (int i = 0; i < k; i++) fm_open_file(f, paths[i]);   /* the list may reload meanwhile */
    free(paths);
}

/* ---- file operations ------------------------------------------------------------------------ */
static void fm_copy_selection(files_t *f, bool cut) {
    int n = fm_sel_count(f);
    if (!n) return;
    fclip_clear();
    g_fclip.paths = malloc((size_t)n * VFS_PATH_MAX);
    g_fclip.text = malloc((size_t)n * VFS_PATH_MAX + 1);
    if (!g_fclip.paths || !g_fclip.text) { fclip_clear(); return; }
    g_fclip.text[0] = 0;
    for (int i = 0; i < f->nents; i++) {
        if (!f->ents[i].sel) continue;
        entry_path(f, i, g_fclip.paths[g_fclip.count]);
        if (g_fclip.count) strcat(g_fclip.text, "\n");
        strcat(g_fclip.text, g_fclip.paths[g_fclip.count]);
        g_fclip.count++;
    }
    g_fclip.cut = cut;
    clipboard_set(g_fclip.text);
    fm_message(f, "%s %d item%s - paste with Ctrl+V", cut ? "Cut" : "Copied", n, n == 1 ? "" : "s");
    ui_invalidate(f->view);     /* cut items are drawn faded */
}

static void fm_job_free(files_t *f) {
    if (!f->job) return;
    free(f->job->src);
    free(f->job->made);
    free(f->job);
    f->job = NULL;
    if (f->job_timer) win_kill_timer(f->win, f->job_timer);
    f->job_timer = 0;
}

static void fm_job_finish(files_t *f) {
    paste_job_t *j = f->job;
    bool was_cut = j->cut;
    int made = j->nmade, failed = j->failed;
    char err[128];
    strlcpy(err, j->err, sizeof err);
    /* reload, then select what was pasted */
    if (!strcmp(j->dest, f->dir)) {
        fm_load(f);
        for (int i = 0; i < f->nents; i++) {
            f->ents[i].sel = false;
            for (int m = 0; m < j->nmade; m++)
                if (!strcmp(f->ents[i].name, j->made[m])) { f->ents[i].sel = true; f->focus = f->anchor = i; }
        }
        if (f->focus >= 0) fm_ensure_visible(f, f->focus);
    }
    fm_job_free(f);
    if (was_cut && !failed) fclip_clear();
    if (failed) fm_error(f, "Paste", "%d item%s could not be pasted.%s%s", failed, failed == 1 ? "" : "s",
                         err[0] ? "\n\n" : "", err);
    else fm_message(f, "%s %d item%s", was_cut ? "Moved" : "Copied", made, made == 1 ? "" : "s");
    ui_invalidate(f->view);
    fm_status_dirty(f);
}

/* Do a time-bounded slice of the paste (never more than ~8 ms per call). */
static void fm_job_step(files_t *f) {
    paste_job_t *j = f->job;
    if (!j) return;
    uint32_t t0 = sys_uptime_ms();
    while (j->next < j->count && sys_uptime_ms() - t0 < 8) {
        const char *src = j->src[j->next++];
        if (!vfs_exists(src)) { j->failed++; snprintf(j->err, sizeof j->err, "\"%s\" no longer exists.", vfs_basename(src)); continue; }
        const char *base = vfs_basename(src);
        bool is_dir = vfs_is_dir(src);
        char srcdir[VFS_PATH_MAX];
        vfs_dirname(src, srcdir);
        bool same_dir = !strcmp(srcdir, j->dest);
        if (j->cut && same_dir) { strlcpy(j->made[j->nmade++], base, VFS_NAME_MAX); continue; }
        if (is_dir && path_inside(j->dest, src)) {
            j->failed++;
            snprintf(j->err, sizeof j->err, "A folder cannot be pasted into itself.");
            continue;
        }
        char name[VFS_NAME_MAX], dst[VFS_PATH_MAX];
        unique_name(j->dest, base, is_dir, same_dir, name);
        vfs_join(j->dest, name, dst);
        int err = j->cut ? vfs_rename(src, dst) : vfs_copy(src, dst);
        if (err < 0) {
            j->failed++;
            snprintf(j->err, sizeof j->err, "\"%s\": %s.", base, vfs_strerror(err));
        } else {
            strlcpy(j->made[j->nmade++], name, VFS_NAME_MAX);
        }
    }
    if (j->next >= j->count) fm_job_finish(f);
    else fm_message(f, "%s %d of %d...", j->cut ? "Moving" : "Copying", j->next, j->count);
}

static void fm_paste(files_t *f) {
    if (f->job) { fm_message(f, "A paste is already in progress"); return; }
    if (!fclip_valid()) { fm_message(f, "Nothing to paste - copy or cut some files first"); return; }
    paste_job_t *j = calloc(1, sizeof *j);
    if (!j) return;
    j->count = g_fclip.count;
    j->cut = g_fclip.cut;
    j->src = malloc((size_t)j->count * VFS_PATH_MAX);
    j->made = malloc((size_t)j->count * VFS_NAME_MAX);
    if (!j->src || !j->made) { free(j->src); free(j->made); free(j); return; }
    memcpy(j->src, g_fclip.paths, (size_t)j->count * VFS_PATH_MAX);
    strlcpy(j->dest, f->dir, sizeof j->dest);
    f->job = j;
    fm_job_step(f);                         /* small pastes finish right here */
    if (f->job) f->job_timer = win_set_timer(f->win, 15);
}

/* delete */
static void fm_delete_cb(int r, void *ctx) {
    files_t *f = ctx;
    if (r == MB_RESULT_YES && f->pending_del) {
        int ok = 0, failed = 0, last_err = 0;
        for (int i = 0; i < f->npending_del; i++) {
            const char *p = f->pending_del[i];
            int err = vfs_is_dir(p) ? vfs_remove_recursive(p) : vfs_remove(p);
            if (err < 0) { failed++; last_err = err; } else ok++;
        }
        fm_load(f);
        if (failed) fm_error(f, "Delete", "%d item%s could not be deleted:\n%s.", failed, failed == 1 ? "" : "s", vfs_strerror(last_err));
        else fm_message(f, "Deleted %d item%s", ok, ok == 1 ? "" : "s");
    }
    free(f->pending_del);
    f->pending_del = NULL;
    f->npending_del = 0;
}

static void fm_delete_selection(files_t *f) {
    int n = fm_sel_count(f);
    if (!n || f->pending_del) return;
    f->pending_del = malloc((size_t)n * VFS_PATH_MAX);
    if (!f->pending_del) return;
    f->npending_del = 0;
    int one = -1;
    for (int i = 0; i < f->nents; i++)
        if (f->ents[i].sel) { entry_path(f, i, f->pending_del[f->npending_del++]); one = i; }
    char msg[VFS_NAME_MAX + 200];
    if (n == 1 && f->ents[one].is_dir)
        snprintf(msg, sizeof msg, "Are you sure you want to permanently delete the folder \"%s\" and everything in it?",
                 f->ents[one].name);
    else if (n == 1)
        snprintf(msg, sizeof msg, "Are you sure you want to permanently delete \"%s\"?", f->ents[one].name);
    else
        snprintf(msg, sizeof msg, "Are you sure you want to permanently delete these %d items?", n);
    ui_message_box(f->win, "Delete", msg, MB_YESNO | MB_ICON_WARNING, fm_delete_cb, f);
}

/* rename */
static void fm_rename_cb(const char *text, void *ctx) {
    files_t *f = ctx;
    if (!text) return;
    char buf[VFS_PATH_MAX];
    strlcpy(buf, text, sizeof buf);
    char *name = trim(buf), why[128];
    if (!strcmp(name, f->pending_name)) return;
    if (!valid_name(name, why, sizeof why)) { fm_error(f, "Rename", "%s", why); return; }
    char from[VFS_PATH_MAX], to[VFS_PATH_MAX];
    vfs_join(f->dir, f->pending_name, from);
    vfs_join(f->dir, name, to);
    if (vfs_exists(to) && strcasecmp(name, f->pending_name) != 0) {
        fm_error(f, "Rename", "There is already an item named \"%s\" in this folder.", name);
        return;
    }
    int err = vfs_rename(from, to);
    if (err < 0) { fm_error(f, "Rename", "Could not rename \"%s\":\n%s.", f->pending_name, vfs_strerror(err)); return; }
    for (int i = 0; i < f->nents; i++) f->ents[i].sel = false;
    strlcpy(f->select_after, name, sizeof f->select_after);
    fm_load(f);
    fm_message(f, "Renamed to \"%s\"", name);
}

static void fm_rename_selection(files_t *f) {
    if (fm_sel_count(f) != 1) return;
    int i = fm_first_sel(f);
    strlcpy(f->pending_name, f->ents[i].name, sizeof f->pending_name);
    char prompt[VFS_NAME_MAX + 32];
    snprintf(prompt, sizeof prompt, "New name for \"%s\":", f->ents[i].name);
    ui_input_box(f->win, "Rename", prompt, f->ents[i].name, fm_rename_cb, f);
}

/* new folder / new file */
static void fm_create(files_t *f, const char *text, bool dir) {
    if (!text) return;
    char buf[VFS_PATH_MAX];
    strlcpy(buf, text, sizeof buf);
    char *name = trim(buf), why[128];
    const char *what = dir ? "New folder" : "New text file";
    if (!valid_name(name, why, sizeof why)) { fm_error(f, what, "%s", why); return; }
    char p[VFS_PATH_MAX];
    vfs_join(f->dir, name, p);
    if (vfs_exists(p)) { fm_error(f, what, "There is already an item named \"%s\" in this folder.", name); return; }
    int err = dir ? vfs_mkdir(p) : vfs_write_file(p, "", 0);
    if (err < 0) { fm_error(f, what, "Could not create \"%s\":\n%s.", name, vfs_strerror(err)); return; }
    for (int i = 0; i < f->nents; i++) f->ents[i].sel = false;
    strlcpy(f->select_after, name, sizeof f->select_after);
    fm_load(f);
    fm_message(f, "Created \"%s\"", name);
}

static void fm_newdir_cb(const char *text, void *ctx) { fm_create(ctx, text, true); }
static void fm_newfile_cb(const char *text, void *ctx) { fm_create(ctx, text, false); }

static void fm_new_folder(files_t *f) {
    char name[VFS_NAME_MAX];
    unique_name(f->dir, "New folder", true, false, name);
    ui_input_box(f->win, "New folder", "Name of the new folder:", name, fm_newdir_cb, f);
}

static void fm_new_file(files_t *f) {
    char name[VFS_NAME_MAX];
    unique_name(f->dir, "New text file.txt", false, false, name);
    ui_input_box(f->win, "New text file", "Name of the new file:", name, fm_newfile_cb, f);
}

/* properties */
static void dir_usage(const char *path, uint32_t *bytes, int *files, int *dirs, int depth) {
    if (depth > 24) return;
    int n = vfs_list(path, NULL, 0);
    if (n <= 0) return;
    vfs_dirent_t *ents = malloc(sizeof(vfs_dirent_t) * (size_t)n);
    if (!ents) return;
    n = MIN(vfs_list(path, ents, n), n);
    for (int i = 0; i < n; i++) {
        if (ents[i].is_dir) {
            char sub[VFS_PATH_MAX];
            vfs_join(path, ents[i].name, sub);
            (*dirs)++;
            dir_usage(sub, bytes, files, dirs, depth + 1);
        } else {
            (*files)++;
            *bytes += ents[i].size;
        }
    }
    free(ents);
}

/* One item (or the current folder): the system Properties dialog, which also
   shows and changes the app that opens the file type. Several: a summary. */
static void fm_properties(files_t *f) {
    int n = fm_sel_count(f);
    if (n <= 1) {
        char p[VFS_PATH_MAX];
        if (n == 0) strlcpy(p, f->dir, sizeof p);
        else entry_path(f, fm_first_sel(f), p);
        shell_properties(p);
        return;
    }
    char msg[700], sz[32];
    uint32_t bytes = 0;
    int files = 0, dirs = 0;
    for (int i = 0; i < f->nents; i++) {
        if (!f->ents[i].sel) continue;
        if (f->ents[i].is_dir) {
            char p[VFS_PATH_MAX];
            entry_path(f, i, p);
            dirs++;
            dir_usage(p, &bytes, &files, &dirs, 0);
        } else { files++; bytes += f->ents[i].size; }
    }
    fmt_size(bytes, sz, sizeof sz);
    snprintf(msg, sizeof msg, "%d items selected\nIncluding:  %d file%s, %d folder%s\nTotal size:  %s (%u bytes)\nLocation:  %s",
             n, files, files == 1 ? "" : "s", dirs, dirs == 1 ? "" : "s", sz, bytes, f->dir);
    ui_message_box(f->win, "Properties", msg, MB_OK | MB_ICON_INFO, NULL, NULL);
}

static void fm_set_view(files_t *f, int mode) {
    if (mode == f->view_mode) return;
    f->view_mode = mode;
    fm_thumbs_kick(f);
    fm_clamp_scroll(f);
    if (f->focus >= 0) fm_ensure_visible(f, f->focus);
    win_invalidate(f->win);
}

static void fm_set_sort(files_t *f, int key, bool toggle) {
    if (toggle && f->sort == key) f->sort_desc = !f->sort_desc;
    else { f->sort = key; f->sort_desc = (key == SORT_SIZE || key == SORT_DATE); }
    fm_load(f);
}

/* ---- context menus --------------------------------------------------------------------------- */
static void fm_item_menu_cb(int item, void *ctx) {
    files_t *f = ctx;
    int i = fm_first_sel(f);
    switch (item) {
    case 0: fm_open_selection(f); break;
    case 1:
        if (i >= 0 && f->ents[i].is_dir) {
            char p[VFS_PATH_MAX];
            entry_path(f, i, p);
            app_launch_by_name("Files", p);
        } else {
            fm_open_with(f);
        }
        break;
    case 3: fm_copy_selection(f, true); break;
    case 4: fm_copy_selection(f, false); break;
    case 6: fm_rename_selection(f); break;
    case 7: fm_delete_selection(f); break;
    case 9: fm_properties(f); break;
    default: break;
    }
}

static void fm_bg_menu_cb(int item, void *ctx) {
    files_t *f = ctx;
    switch (item) {
    case 0: fm_set_view(f, VIEW_ICONS); break;
    case 1: fm_set_view(f, VIEW_DETAILS); break;
    case 3: fm_set_sort(f, SORT_NAME, false); break;
    case 4: fm_set_sort(f, SORT_SIZE, false); break;
    case 5: fm_set_sort(f, SORT_TYPE, false); break;
    case 6: fm_set_sort(f, SORT_DATE, false); break;
    case 8: fm_paste(f); break;
    case 10: fm_new_folder(f); break;
    case 11: fm_new_file(f); break;
    case 13: fm_select_range(f, 0, f->nents - 1, false); break;
    case 14: fm_properties(f); break;
    case 15: app_launch_by_name("Files", f->dir); break;
    default: break;
    }
}

static void fm_context_menu(files_t *f, int x, int y, int idx) {
    char items[400];
    if (idx >= 0) {
        int n = fm_sel_count(f);
        bool dir = f->ents[idx].is_dir;
        snprintf(items, sizeof items, "Open\tEnter|%s|-|Cut\tCtrl+X|Copy\tCtrl+C|-|%sRename\tF2|Delete\tDel|-|Properties",
                 n > 1 ? "!Open With..." : dir ? "Open in new window" : "Open With...",
                 n > 1 ? "!" : "");
        ui_popup_menu(f->win, x, y, items, fm_item_menu_cb, f);
    } else {
        const char *ck = "\t\x96";
        snprintf(items, sizeof items,
                 "View as icons%s|View as list%s|-|Sort by name%s|Sort by size%s|Sort by type%s|Sort by date%s|-|"
                 "%sPaste\tCtrl+V|-|New folder\tCtrl+Shift+N|New text file|-|Select all\tCtrl+A|Properties|Open in new window",
                 f->view_mode == VIEW_ICONS ? ck : "", f->view_mode == VIEW_DETAILS ? ck : "",
                 f->sort == SORT_NAME ? ck : "", f->sort == SORT_SIZE ? ck : "",
                 f->sort == SORT_TYPE ? ck : "", f->sort == SORT_DATE ? ck : "",
                 fclip_valid() && !f->job ? "" : "!");
        ui_popup_menu(f->win, x, y, items, fm_bg_menu_cb, f);
    }
}

/* ---- view widget: painting -------------------------------------------------------------------- */
/* Draw a name centred under an icon on at most two lines. */
static void draw_name_2lines(surface_t *s, int cx, int y, int max_w, const char *name, uint32_t c) {
    const font_t *f = FONT_UI;
    int lh = font_height(f);
    int n = (int)strlen(name);
    int w = font_text_width(f, name);
    if (w <= max_w) { gfx_text(s, cx - w / 2, y, name, c, f); return; }
    int fit = 0;
    while (fit < n && font_text_width_n(f, name, fit + 1) <= max_w) fit++;
    if (fit == 0) fit = 1;
    /* break after the last space that fits, else after '-' / '_', else before
       the extension's '.', else wherever the line is full */
    int brk = fit;
    static const char *prefs[3] = { " ", "-_", "." };
    for (int pass = 0, found = 0; pass < 3 && !found; pass++)
        for (int i = fit; i > fit / 3; i--)
            if (strchr(prefs[pass], name[pass == 2 ? i : i - 1]) && name[pass == 2 ? i : i - 1]) {
                brk = i;
                found = 1;
                break;
            }
    int w1 = font_text_width_n(f, name, brk);
    gfx_text_n(s, cx - w1 / 2, y, name, brk, c, f);
    const char *rest = name + brk;
    while (*rest == ' ') rest++;
    int w2 = font_text_width(f, rest);
    if (w2 <= max_w) gfx_text(s, cx - w2 / 2, y + lh, rest, c, f);
    else gfx_text_ellipsis(s, cx - max_w / 2, y + lh, max_w, rest, c, f);
}

static void view_draw(widget_t *w, surface_t *s, rect_t r) {
    files_t *f = ui_get_ctx(w);
    const theme_t *t = theme_get();
    rect_t old = s->clip;
    surface_set_clip(s, rect_intersect(old, r));
    gfx_fill_rect(s, r.x, r.y, r.w, r.h, t->field_bg);
    bool focused = win_is_focused(f->win) && ui_has_focus(f->view);
    uint32_t muted = color_mix(t->field_text, t->field_bg, 110);
    uint32_t sel_bg = focused ? color_alpha(t->accent, 70) : color_mix(t->field_bg, t->field_text, 30);
    uint32_t sel_border = focused ? color_alpha(t->accent, 170) : color_mix(t->field_bg, t->field_text, 60);
    uint32_t hover_bg = color_mix(t->field_bg, t->field_text, 14);
    rect_t a = fm_area(f);
    char p[VFS_PATH_MAX];

    surface_set_clip(s, rect_intersect(old, rect_intersect(r, rect_make(a.x, a.y, a.w + SB_W, a.h))));
    if (f->view_mode == VIEW_ICONS) {
        for (int i = 0; i < f->nents; i++) {
            rect_t ir = fm_item_rect(f, i);
            if (ir.y + ir.h < a.y || ir.y > a.y + a.h) continue;
            if (!rect_overlaps(ir, s->clip)) continue;
            entry_t *e = &f->ents[i];
            rect_t hb = rect_inset(ir, 3);
            if (e->sel) {
                gfx_fill_rounded_rect(s, hb.x, hb.y, hb.w, hb.h, 6, sel_bg);
                gfx_rounded_rect(s, hb.x, hb.y, hb.w, hb.h, 6, sel_border);
            } else if (i == f->hover) {
                gfx_fill_rounded_rect(s, hb.x, hb.y, hb.w, hb.h, 6, hover_bg);
            }
            if (i == f->focus && focused && !e->sel)
                gfx_rounded_rect(s, hb.x, hb.y, hb.w, hb.h, 6, color_alpha(t->accent, 120));
            entry_path(f, i, p);
            int ix = ir.x + (ir.w - ICON_BIG) / 2, iy = ir.y + 7;
            bool cut = fclip_is_cut(p);
            if (e->thumb) {      /* image preview in a photo-style frame */
                surface_t *th = e->thumb;
                int tx = ix + (ICON_BIG - th->w) / 2, ty = iy + (ICON_BIG - th->h) / 2;
                gfx_fill_rect(s, tx - 3, ty - 2, th->w + 6, th->h + 6, RGBA(0, 0, 0, 40));
                gfx_fill_rect(s, tx - 3, ty - 3, th->w + 6, th->h + 6, RGB(250, 250, 250));
                gfx_rect(s, tx - 3, ty - 3, th->w + 6, th->h + 6, RGB(200, 204, 214));
                if (cut) gfx_blit_alpha(s, tx, ty, th, 110);
                else gfx_blit(s, tx, ty, th);
            } else {
                blit_icon(s, ix, iy, ICON_BIG, entry_icon_id(f, i), cut ? 110 : 255);
            }
            if (!e->is_dir && e->kind != K_TEXT) {     /* badge: the app that opens it */
                const app_desc_t *app = assoc_default_app(p);
                if (app) {
                    const surface_t *bi = app_icon(app, 20);
                    if (bi) gfx_blit_alpha(s, ix + ICON_BIG - 16, iy + ICON_BIG - 18, bi, cut ? 110 : 255);
                }
            }
            draw_name_2lines(s, ir.x + ir.w / 2, ir.y + 60, ir.w - 12, e->name, cut ? muted : t->field_text);
        }
    } else {
        cols_t c = fm_columns(f);
        for (int i = 0; i < f->nents; i++) {
            rect_t ir = fm_item_rect(f, i);
            if (ir.y + ir.h < a.y || ir.y > a.y + a.h) continue;
            if (!rect_overlaps(ir, s->clip)) continue;
            entry_t *e = &f->ents[i];
            uint32_t fg = t->field_text, fg2 = muted;
            rect_t hb = rect_make(ir.x + 4, ir.y + 1, ir.w - 8, ir.h - 2);
            if (e->sel) {
                gfx_fill_rounded_rect(s, hb.x, hb.y, hb.w, hb.h, 5, focused ? t->selection_bg : color_mix(t->field_bg, t->field_text, 36));
                if (focused) { fg = t->selection_text; fg2 = color_mix(t->selection_text, t->selection_bg, 60); }
            } else if (i == f->hover) {
                gfx_fill_rounded_rect(s, hb.x, hb.y, hb.w, hb.h, 5, hover_bg);
            } else if (i % 2) {
                gfx_fill_rect(s, hb.x, hb.y, hb.w, hb.h, color_mix(t->field_bg, t->field_text, 5));
            }
            if (i == f->focus && focused && !e->sel) gfx_rounded_rect(s, hb.x, hb.y, hb.w, hb.h, 5, color_alpha(t->accent, 140));
            entry_path(f, i, p);
            bool cut = fclip_is_cut(p);
            if (cut) fg = muted;
            int ty = ir.y + (ROW_H - font_height(FONT_UI)) / 2;
            blit_icon(s, c.name_x + 12, ir.y + (ROW_H - ICON_SMALL) / 2, ICON_SMALL, entry_icon_id(f, i), cut ? 110 : 255);
            gfx_text_ellipsis(s, c.name_x + 40, ty, c.name_w - 48, e->name, fg, FONT_UI);
            char buf[48];
            if (!e->is_dir) {
                fmt_size(e->size, buf, sizeof buf);
                int tw = font_text_width(FONT_UI, buf);
                gfx_text(s, c.size_x + c.size_w - 14 - tw, ty, buf, fg2, FONT_UI);
            }
            if (c.type_w) {
                char tb[32];
                gfx_text_ellipsis(s, c.type_x + 10, ty, c.type_w - 16, type_name(e, tb, sizeof tb), fg2, FONT_UI);
            }
            if (c.date_w) {
                fmt_date(e->mtime, buf, sizeof buf);
                gfx_text_ellipsis(s, c.date_x + 10, ty, c.date_w - 14, buf, fg2, FONT_UI);
            }
        }
    }
    /* rubber band */
    if (f->banding) {
        int x0 = MIN(f->band_x0, f->band_x1), x1 = MAX(f->band_x0, f->band_x1);
        int y0 = MIN(f->band_y0, f->band_y1), y1 = MAX(f->band_y0, f->band_y1);
        rect_t br = rect_make(a.x + x0, a.y + y0 - f->scroll, x1 - x0 + 1, y1 - y0 + 1);
        gfx_fill_rect(s, br.x, br.y, br.w, br.h, color_alpha(t->accent, 45));
        gfx_rect(s, br.x, br.y, br.w, br.h, color_alpha(t->accent, 200));
    }
    surface_set_clip(s, rect_intersect(old, r));

    /* empty folder */
    if (f->nents == 0) {
        rect_t er = rect_make(a.x, a.y + 40, a.w, 24);
        gfx_text_aligned(s, er, "This folder is empty", muted, FONT_UI_LARGE, ALIGN_CENTER | ALIGN_MIDDLE);
        gfx_text_aligned(s, rect_make(a.x, a.y + 66, a.w, 20), "Right-click to create a folder or a text file, or paste with Ctrl+V.",
                         muted, FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
    }

    /* details header (drawn last so rows scroll underneath) */
    if (f->view_mode == VIEW_DETAILS) {
        cols_t c = fm_columns(f);
        uint32_t hbg = t->dark ? color_shade(t->field_bg, 8) : color_shade(t->field_bg, -6);
        uint32_t line = color_mix(t->field_bg, t->window_border, 120);
        gfx_fill_rect(s, r.x, r.y, r.w, HEADER_H, hbg);
        gfx_hline(s, r.x, r.y + HEADER_H - 1, r.w, line);
        static const char *titles[4] = { "Name", "Size", "Type", "Modified" };
        int xs[4] = { c.name_x, c.size_x, c.type_x, c.date_x }, ws[4] = { c.name_w, c.size_w, c.type_w, c.date_w };
        static const int keys[4] = { SORT_NAME, SORT_SIZE, SORT_TYPE, SORT_DATE };
        for (int k = 0; k < 4; k++) {
            if (!ws[k]) continue;
            bool active = f->sort == keys[k];
            int ty = r.y + (HEADER_H - font_height(FONT_UI_BOLD)) / 2;
            int tw = font_text_width(FONT_UI_BOLD, titles[k]);
            int tx = k == 1 ? xs[k] + ws[k] - 14 - tw : xs[k] + (k == 0 ? 40 : 10);
            gfx_text(s, tx, ty, titles[k], active ? t->field_text : muted, FONT_UI_BOLD);
            if (active) {
                int gx = k == 1 ? tx - 14 : tx + tw + 6;
                ui_draw_glyph(s, f->sort_desc ? UI_GLYPH_ARROW_DOWN : UI_GLYPH_ARROW_UP, gx, r.y + HEADER_H / 2 - 5, 10, t->accent);
            }
            if (k > 0) gfx_vline(s, xs[k], r.y + 6, HEADER_H - 12, line);
        }
    }

    /* scrollbar */
    int total = fm_content_h(f);
    if (total > a.h) ui_draw_scrollbar(s, rect_make(a.x + a.w, a.y, SB_W, a.h), true, total, a.h, f->scroll,
                                       f->sb_drag ? UI_STATE_PRESSED : 0);
    s->clip = old;
}

/* ---- view widget: mouse ------------------------------------------------------------------------ */
static void sb_thumb(int len, int total, int page, int pos, int *tp, int *tl) {
    *tl = total > 0 ? MAX(24, (int)((int64_t)len * page / total)) : len;
    if (*tl > len) *tl = len;
    int maxpos = total - page;
    *tp = maxpos > 0 ? (int)((int64_t)(len - *tl) * pos / maxpos) : 0;
}

static void band_update(files_t *f) {
    int x0 = MIN(f->band_x0, f->band_x1), x1 = MAX(f->band_x0, f->band_x1);
    int y0 = MIN(f->band_y0, f->band_y1), y1 = MAX(f->band_y0, f->band_y1);
    rect_t br = rect_make(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    for (int i = 0; i < f->nents; i++) {
        rect_t c = fm_item_content(f, i);
        if (f->view_mode == VIEW_ICONS) c = rect_inset(c, 3);
        bool in = rect_overlaps(br, c);
        bool base = f->band_base ? f->band_base[i] : false;
        f->ents[i].sel = base ? !in : in;     /* Ctrl+band toggles */
    }
    ui_invalidate(f->view);
    fm_status_dirty(f);
}

static bool view_event(widget_t *w, const event_t *ev) {
    files_t *f = ui_get_ctx(w);
    rect_t r = ui_rect(w), a = fm_area(f);
    rect_t sbr = rect_make(a.x + a.w, a.y, SB_W, a.h);
    int total = fm_content_h(f);
    switch (ev->type) {
    case EV_MOUSE_DOWN: {
        bool ctrl = ev->mods & MOD_CTRL, shift = ev->mods & MOD_SHIFT;
        if (ev->button == MOUSE_LEFT && total > a.h && rect_contains(sbr, ev->x, ev->y)) {
            int tp, tl;
            sb_thumb(a.h, total, a.h, f->scroll, &tp, &tl);
            int rel = ev->y - a.y;
            if (rel >= tp && rel < tp + tl) { f->sb_drag = true; f->sb_off = rel - tp; }
            else f->scroll += rel < tp ? -a.h : a.h;
            fm_clamp_scroll(f);
            ui_invalidate(w);
            return true;
        }
        if (f->view_mode == VIEW_DETAILS && ev->y < r.y + HEADER_H) {   /* column header: sort */
            if (ev->button != MOUSE_LEFT) return true;
            cols_t c = fm_columns(f);
            int key = SORT_NAME;
            if (ev->x >= c.size_x && ev->x < c.size_x + c.size_w) key = SORT_SIZE;
            else if (c.type_w && ev->x >= c.type_x && ev->x < c.type_x + c.type_w) key = SORT_TYPE;
            else if (c.date_w && ev->x >= c.date_x) key = SORT_DATE;
            fm_set_sort(f, key, true);
            return true;
        }
        int idx = fm_hit(f, ev->x, ev->y);
        if (ev->button == MOUSE_RIGHT) {
            if (idx >= 0 && !f->ents[idx].sel) fm_select_only(f, idx);
            if (idx >= 0) f->focus = idx;
            if (idx < 0) fm_clear_sel(f);   /* clear */
            fm_context_menu(f, ev->x, ev->y, idx);
            return true;
        }
        if (ev->button != MOUSE_LEFT) return true;
        if (idx >= 0) {
            if (ctrl) {
                f->ents[idx].sel = !f->ents[idx].sel;
                f->focus = f->anchor = idx;
            } else if (shift && f->anchor >= 0) {
                fm_select_range(f, f->anchor, idx, false);
                f->focus = idx;
            } else {
                fm_select_only(f, idx);
            }
            ui_invalidate(w);
            fm_status_dirty(f);
        } else {
            /* empty space: start a rubber band */
            if (!ctrl) for (int i = 0; i < f->nents; i++) f->ents[i].sel = false;
            free(f->band_base);
            f->band_base = NULL;
            if (ctrl && f->nents) {
                f->band_base = malloc((size_t)f->nents * sizeof(bool));
                if (f->band_base) for (int i = 0; i < f->nents; i++) f->band_base[i] = f->ents[i].sel;
            }
            f->banding = true;
            f->band_x0 = f->band_x1 = ev->x - a.x;
            f->band_y0 = f->band_y1 = ev->y - a.y + f->scroll;
            ui_invalidate(w);
            fm_status_dirty(f);
        }
        return true;
    }
    case EV_MOUSE_MOVE:
        if (f->sb_drag) {
            int tp, tl;
            sb_thumb(a.h, total, a.h, f->scroll, &tp, &tl);
            int maxpos = total - a.h;
            if (a.h - tl > 0 && maxpos > 0)
                f->scroll = (int)((int64_t)(ev->y - a.y - f->sb_off) * maxpos / (a.h - tl));
            fm_clamp_scroll(f);
            ui_invalidate(w);
            return true;
        }
        if (f->banding) {
            /* auto-scroll while dragging past the top/bottom edge */
            if (ev->y < a.y) f->scroll -= MIN(40, a.y - ev->y);
            else if (ev->y > a.y + a.h) f->scroll += MIN(40, ev->y - a.y - a.h);
            fm_clamp_scroll(f);
            f->band_x1 = CLAMP(ev->x, a.x, a.x + a.w - 1) - a.x;
            f->band_y1 = CLAMP(ev->y, a.y, a.y + a.h - 1) - a.y + f->scroll;
            band_update(f);
            return true;
        }
        {
            int h = rect_contains(r, ev->x, ev->y) ? fm_hit(f, ev->x, ev->y) : -1;
            if (h != f->hover) {
                fm_invalidate_item(f, f->hover);
                f->hover = h;
                fm_invalidate_item(f, h);
            }
        }
        return true;
    case EV_MOUSE_UP:
        if (f->banding) {
            f->banding = false;
            free(f->band_base);
            f->band_base = NULL;
            ui_invalidate(w);
        }
        if (f->sb_drag) { f->sb_drag = false; ui_invalidate(w); }
        return true;
    case EV_DOUBLE_CLICK:
        if (ev->button == MOUSE_LEFT) {
            int idx = fm_hit(f, ev->x, ev->y);
            if (idx >= 0) fm_open_entry(f, idx);
        }
        return true;
    case EV_MOUSE_WHEEL:
        f->scroll += ev->wheel * (f->view_mode == VIEW_DETAILS ? ROW_H * 3 : 60);
        fm_clamp_scroll(f);
        {
            int h = fm_hit(f, ev->x, ev->y);
            f->hover = h;
        }
        ui_invalidate(w);
        return true;
    default:
        return false;   /* keys are handled by the window (also when the toolbar was clicked) */
    }
}

/* ---- keyboard --------------------------------------------------------------------------------- */
static void fm_move_focus(files_t *f, int to, bool shift, bool ctrl) {
    if (!f->nents) return;
    to = CLAMP(to, 0, f->nents - 1);
    if (shift) {
        if (f->anchor < 0) f->anchor = f->focus >= 0 ? f->focus : to;
        fm_select_range(f, f->anchor, to, false);
        f->focus = to;
        fm_ensure_visible(f, to);
    } else if (ctrl) {
        f->focus = to;
        fm_ensure_visible(f, to);
        ui_invalidate(f->view);
    } else {
        fm_select_only(f, to);
    }
}

static void fm_key(files_t *f, const event_t *ev) {
    bool ctrl = ev->mods & MOD_CTRL, shift = ev->mods & MOD_SHIFT, alt = ev->mods & MOD_ALT;
    if (ui_has_focus(f->addr)) {     /* the address bar keeps most keys for itself */
        if (ev->key == KEY_ESCAPE) { ui_set_text(f->addr, f->dir); ui_focus(f->view); }
        else if (ev->key == KEY_DOWN) ui_focus(f->view);
        return;
    }
    int cols = f->view_mode == VIEW_ICONS ? fm_icon_cols(f, NULL) : 1;
    int page_rows = MAX(1, fm_area(f).h / (f->view_mode == VIEW_ICONS ? CELL_H : ROW_H));
    int cur = f->focus;
    if (alt) {
        if (ev->key == KEY_LEFT) fm_go_back(f);
        else if (ev->key == KEY_RIGHT) fm_go_forward(f);
        else if (ev->key == KEY_UP) fm_go_up(f);
        return;
    }
    if (ctrl) {
        switch (ev->key) {
        case 'a': fm_select_range(f, 0, f->nents - 1, false); return;
        case 'c': fm_copy_selection(f, false); return;
        case 'x': fm_copy_selection(f, true); return;
        case 'v': fm_paste(f); return;
        case 'n': if (shift) fm_new_folder(f); else app_launch_by_name("Files", f->dir); return;
        case 'l': ui_focus(f->addr); ui_textbox_select_all(f->addr); return;
        case 'r': fm_load(f); return;
        case '1': fm_set_view(f, VIEW_ICONS); return;
        case '2': fm_set_view(f, VIEW_DETAILS); return;
        default: break;
        }
    }
    switch (ev->key) {
    case KEY_LEFT:     if (cols > 1) fm_move_focus(f, cur < 0 ? 0 : cur - 1, shift, ctrl); break;
    case KEY_RIGHT:    if (cols > 1) fm_move_focus(f, cur < 0 ? 0 : cur + 1, shift, ctrl); break;
    case KEY_UP:       fm_move_focus(f, cur < 0 ? 0 : cur - cols, shift, ctrl); break;
    case KEY_DOWN:     fm_move_focus(f, cur < 0 ? 0 : cur + cols, shift, ctrl); break;
    case KEY_PAGEUP:   fm_move_focus(f, cur < 0 ? 0 : cur - cols * page_rows, shift, ctrl); break;
    case KEY_PAGEDOWN: fm_move_focus(f, cur < 0 ? 0 : cur + cols * page_rows, shift, ctrl); break;
    case KEY_HOME:     fm_move_focus(f, 0, shift, ctrl); break;
    case KEY_END:      fm_move_focus(f, f->nents - 1, shift, ctrl); break;
    case KEY_SPACE:
        if (ctrl && cur >= 0) { f->ents[cur].sel = !f->ents[cur].sel; f->anchor = cur; ui_invalidate(f->view); fm_status_dirty(f); }
        break;
    case KEY_ENTER:     fm_open_selection(f); break;
    case KEY_BACKSPACE: fm_go_up(f); break;
    case KEY_DELETE:    fm_delete_selection(f); break;
    case KEY_F2:        fm_rename_selection(f); break;
    case KEY_F5:        fm_load(f); fm_message(f, "Refreshed"); break;
    case KEY_ESCAPE:
        if (fclip_valid() && g_fclip.cut) { fclip_clear(); clipboard_set(""); ui_invalidate(f->view); fm_message(f, "Cut cancelled"); }
        else fm_clear_sel(f);
        break;
    default: break;
    }
}

/* Type-to-jump: select the next entry starting with the typed character. */
static void fm_char(files_t *f, int ch) {
    if (ui_has_focus(f->addr) || ch <= ' ' || !f->nents) return;
    int start = f->focus < 0 ? 0 : f->focus + 1;
    for (int n = 0; n < f->nents; n++) {
        int i = (start + n) % f->nents;
        if (tolower((unsigned char)f->ents[i].name[0]) == tolower(ch)) { fm_select_only(f, i); return; }
    }
}

/* ---- window painting ------------------------------------------------------------------------- */
static void draw_tb_button(files_t *f, surface_t *s, int b) {
    const theme_t *t = theme_get();
    rect_t r = tb_rect(f, b);
    bool en = tb_enabled(f, b);
    bool hov = en && f->tb_hover == b, prs = hov && f->tb_pressed == b;
    if (prs) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, color_mix(t->window_bg, t->window_text, 40));
    else if (hov) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, color_mix(t->window_bg, t->window_text, 20));
    uint32_t c = en ? t->window_text : color_mix(t->window_text, t->window_bg, 170);
    int g = -1;
    if (b == TB_BACK) g = UI_GLYPH_ARROW_LEFT;
    else if (b == TB_FWD) g = UI_GLYPH_ARROW_RIGHT;
    else if (b == TB_UP) g = UI_GLYPH_ARROW_UP;
    if (g >= 0) { ui_draw_glyph(s, g, r.x + 9, r.y + 9, 14, c); return; }
    if (b == TB_NEWDIR) {
        blit_icon(s, r.x + 5, r.y + 6, 20, ICON_FOLDER, 255);
        int px = r.x + 22, py = r.y + 21;
        gfx_fill_circle_aa(s, px, py, 6.5, t->accent);
        gfx_fill_rect(s, px - 3, py - 1, 7, 2, t->accent_text);
        gfx_fill_rect(s, px - 1, py - 3, 2, 7, t->accent_text);
    }
}

static void draw_view_toggle(files_t *f, surface_t *s) {
    const theme_t *t = theme_get();
    rect_t a = tb_rect(f, TB_VIEW_ICONS), b = tb_rect(f, TB_VIEW_LIST);
    rect_t all = rect_make(a.x, a.y, b.x + b.w - a.x, a.h);
    gfx_fill_rounded_rect(s, all.x, all.y, all.w, all.h, 7, color_mix(t->window_bg, t->window_border, 90));
    gfx_fill_rounded_rect(s, all.x + 1, all.y + 1, all.w - 2, all.h - 2, 6, t->dark ? color_shade(t->window_bg, 10) : color_shade(t->window_bg, -8));
    rect_t act = f->view_mode == VIEW_ICONS ? a : b;
    gfx_fill_rounded_rect(s, act.x + 3, act.y + 3, act.w - 6, act.h - 6, 5, t->field_bg);
    gfx_rounded_rect(s, act.x + 3, act.y + 3, act.w - 6, act.h - 6, 5, color_mix(t->field_bg, t->window_border, 120));
    if (f->tb_hover == TB_VIEW_ICONS && f->view_mode != VIEW_ICONS)
        gfx_fill_rounded_rect(s, a.x + 3, a.y + 3, a.w - 6, a.h - 6, 5, color_mix(t->window_bg, t->window_text, 20));
    if (f->tb_hover == TB_VIEW_LIST && f->view_mode != VIEW_DETAILS)
        gfx_fill_rounded_rect(s, b.x + 3, b.y + 3, b.w - 6, b.h - 6, 5, color_mix(t->window_bg, t->window_text, 20));
    uint32_t ci = f->view_mode == VIEW_ICONS ? t->accent : t->window_text;
    uint32_t cl = f->view_mode == VIEW_DETAILS ? t->accent : t->window_text;
    /* grid glyph */
    for (int yy = 0; yy < 2; yy++)
        for (int xx = 0; xx < 2; xx++)
            gfx_fill_rounded_rect(s, a.x + 10 + xx * 7, a.y + 10 + yy * 7, 5, 5, 1, ci);
    /* list glyph */
    for (int yy = 0; yy < 3; yy++) {
        gfx_fill_rect(s, b.x + 9, b.y + 11 + yy * 4, 2, 2, cl);
        gfx_fill_rect(s, b.x + 13, b.y + 11 + yy * 4, 10, 2, cl);
    }
}

static void fm_paint(files_t *f) {
    surface_t *s = win_surface(f->win);
    const theme_t *t = theme_get();
    int w = s->w, h = s->h;
    uint32_t line = color_mix(t->window_bg, t->window_border, 90);
    uint32_t muted = color_mix(t->window_text, t->window_bg, 90);

    /* toolbar */
    gfx_hline(s, 0, TOOLBAR_H - 1, w, line);
    for (int b = TB_BACK; b <= TB_NEWDIR; b++) draw_tb_button(f, s, b);
    draw_view_toggle(f, s);

    /* sidebar */
    uint32_t side_bg = t->dark ? color_shade(t->window_bg, -10) : color_shade(t->window_bg, -5);
    gfx_fill_rect(s, 0, TOOLBAR_H, SIDEBAR_W, h - TOOLBAR_H - STATUS_H, side_bg);
    gfx_vline(s, SIDEBAR_W - 1, TOOLBAR_H, h - TOOLBAR_H - STATUS_H, line);
    gfx_text(s, 16, TOOLBAR_H + 12, "Places", muted, FONT_UI_BOLD);
    gfx_text(s, 16, side_row_rect(f, NPLACES_HOME).y - 22, "This computer", muted, FONT_UI_BOLD);
    for (int i = 0; i < ARRAY_SIZE(g_places); i++) {
        if (!vfs_is_dir(g_places[i].path)) continue;
        rect_t r = side_row_rect(f, i);
        bool cur = !strcmp(f->dir, g_places[i].path);
        if (cur) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, color_mix(side_bg, t->accent, 50));
        else if (i == f->side_hover) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, color_mix(side_bg, t->window_text, 18));
        blit_icon(s, r.x + 8, r.y + (r.h - 18) / 2, 18, g_places[i].icon, 255);
        gfx_text_ellipsis(s, r.x + 34, r.y + (r.h - font_height(FONT_UI)) / 2, r.w - 40, g_places[i].label,
                          t->window_text, cur ? FONT_UI_BOLD : FONT_UI);
    }

    /* status bar */
    int sy = h - STATUS_H;
    gfx_fill_rect(s, 0, sy, w, STATUS_H, t->dark ? color_shade(t->window_bg, -10) : color_shade(t->window_bg, -6));
    gfx_hline(s, 0, sy, w, line);
    int ty = sy + 1 + (STATUS_H - 1 - font_height(FONT_UI)) / 2;
    char left[160], right[160];
    int nsel = fm_sel_count(f);
    if (nsel) {
        uint32_t bytes = 0;
        int ndirs = 0;
        for (int i = 0; i < f->nents; i++)
            if (f->ents[i].sel) { if (f->ents[i].is_dir) ndirs++; else bytes += f->ents[i].size; }
        char sz[32];
        fmt_size(bytes, sz, sizeof sz);
        if (ndirs == nsel) snprintf(left, sizeof left, "%d item%s, %d selected", f->nents, f->nents == 1 ? "" : "s", nsel);
        else snprintf(left, sizeof left, "%d item%s, %d selected (%s)", f->nents, f->nents == 1 ? "" : "s", nsel, sz);
    } else {
        snprintf(left, sizeof left, "%d item%s", f->nents, f->nents == 1 ? "" : "s");
    }
    if (f->msg[0]) strlcpy(right, f->msg, sizeof right);
    else {
        char sz[32];
        fmt_size(vfs_total_bytes(), sz, sizeof sz);
        snprintf(right, sizeof right, "%s in all files", sz);
    }
    int lw = gfx_text(s, 12, ty, left, t->window_text, FONT_UI);
    int rw = font_text_width(FONT_UI, right);
    int rx = MAX(12 + lw + 24, w - 12 - rw);
    gfx_text_ellipsis(s, rx, ty, w - 12 - rx, right, f->msg[0] ? t->window_text : muted, FONT_UI);
}

/* ---- window events ------------------------------------------------------------------------ */
static void fm_layout(files_t *f, int w, int h) {
    rect_t nd = tb_rect(f, TB_NEWDIR);
    ui_move(f->addr, 124, 8, MAX(80, nd.x - 10 - 124), TB_BTN);
    ui_move(f->view, SIDEBAR_W, TOOLBAR_H, w - SIDEBAR_W, h - TOOLBAR_H - STATUS_H);
    fm_clamp_scroll(f);
}

static void fm_tb_action(files_t *f, int b) {
    switch (b) {
    case TB_BACK: fm_go_back(f); break;
    case TB_FWD: fm_go_forward(f); break;
    case TB_UP: fm_go_up(f); break;
    case TB_NEWDIR: fm_new_folder(f); break;
    case TB_VIEW_ICONS: fm_set_view(f, VIEW_ICONS); break;
    case TB_VIEW_LIST: fm_set_view(f, VIEW_DETAILS); break;
    default: break;
    }
}

static void fm_addr_submit(widget_t *w, void *ctx) {
    files_t *f = ctx;
    char text[VFS_PATH_MAX], norm[VFS_PATH_MAX];
    strlcpy(text, ui_get_text(w), sizeof text);
    char *p = trim(text);
    if (!*p) { ui_set_text(w, f->dir); return; }
    if (*p == '~') {     /* "~" and "~/x" mean the home folder */
        char tmp[VFS_PATH_MAX];
        snprintf(tmp, sizeof tmp, "/home/user%s", p + 1);
        strlcpy(text, tmp, sizeof text);
        p = text;
    }
    if (vfs_normalize(f->dir, p, norm) != 0) { fm_error(f, "Files", "\"%s\" is not a valid path.", p); return; }
    if (vfs_is_dir(norm)) { fm_navigate(f, norm); ui_focus(f->view); return; }
    if (vfs_exists(norm)) {     /* a file: show it in its folder, then open it */
        char dir[VFS_PATH_MAX];
        vfs_dirname(norm, dir);
        strlcpy(f->select_after, vfs_basename(norm), sizeof f->select_after);
        fm_navigate(f, dir);
        ui_focus(f->view);
        fm_open_file(f, norm);
        return;
    }
    fm_error(f, "Files", "Cannot find \"%s\".\nCheck the spelling and try again.", norm);
    ui_set_text(w, f->dir);
}

static void fm_event(window_t *win, const event_t *ev) {
    files_t *f = win_get_userdata(win);
    if (!f) return;
    switch (ev->type) {
    case EV_PAINT:
        fm_paint(f);
        break;
    case EV_RESIZE:
        fm_layout(f, ev->w, ev->h);
        break;
    case EV_MOUSE_MOVE: {
        int b = tb_hit(f, ev->x, ev->y), sh = side_hit(f, ev->x, ev->y);
        if (b != f->tb_hover) { f->tb_hover = b; win_invalidate_rect(win, 0, 0, win_width(win), TOOLBAR_H); }
        if (sh != f->side_hover) { f->side_hover = sh; win_invalidate_rect(win, 0, TOOLBAR_H, SIDEBAR_W, win_height(win) - TOOLBAR_H - STATUS_H); }
        if (f->hover >= 0) { fm_invalidate_item(f, f->hover); f->hover = -1; }
        break;
    }
    case EV_MOUSE_LEAVE:
        if (f->tb_hover != TB_NONE || f->side_hover >= 0 || f->hover >= 0) {
            f->tb_hover = TB_NONE;
            f->side_hover = -1;
            f->hover = -1;
            win_invalidate(win);
        }
        break;
    case EV_MOUSE_DOWN:
        if (ev->button != MOUSE_LEFT) break;
        f->tb_pressed = tb_hit(f, ev->x, ev->y);
        if (f->tb_pressed != TB_NONE) win_invalidate_rect(win, 0, 0, win_width(win), TOOLBAR_H);
        else {
            int sh = side_hit(f, ev->x, ev->y);
            if (sh >= 0) fm_navigate(f, g_places[sh].path);
        }
        ui_focus(f->view);
        break;
    case EV_MOUSE_UP:
        if (f->tb_pressed != TB_NONE) {
            int b = f->tb_pressed;
            f->tb_pressed = TB_NONE;
            win_invalidate_rect(win, 0, 0, win_width(win), TOOLBAR_H);
            if (tb_hit(f, ev->x, ev->y) == b && tb_enabled(f, b)) fm_tb_action(f, b);
        }
        break;
    case EV_KEY_DOWN:
        fm_key(f, ev);
        break;
    case EV_CHAR:
        if (!(ev->mods & (MOD_CTRL | MOD_ALT))) fm_char(f, ev->ch);
        break;
    case EV_TIMER:
        if (ev->timer_id == f->job_timer) { fm_job_step(f); break; }
        if (ev->timer_id == f->thumb_timer) {    /* one thumbnail per tick, visible ones first */
            int pick = -1;
            rect_t a = fm_area(f);
            for (int i = 0; i < f->nents && pick < 0; i++) {
                rect_t ir = fm_item_rect(f, i);
                if (f->ents[i].kind == K_IMAGE && !f->ents[i].thumb_tried && ir.y < a.y + a.h && ir.y + ir.h > a.y) pick = i;
            }
            for (int i = 0; i < f->nents && pick < 0; i++)
                if (f->ents[i].kind == K_IMAGE && !f->ents[i].thumb_tried) pick = i;
            if (pick < 0 || f->view_mode != VIEW_ICONS) { win_kill_timer(win, f->thumb_timer); f->thumb_timer = 0; break; }
            char p[VFS_PATH_MAX];
            entry_path(f, pick, p);
            f->ents[pick].thumb = make_thumb(p, f->ents[pick].size);
            f->ents[pick].thumb_tried = true;
            fm_invalidate_item(f, pick);
            break;
        }
        if (!f->job && vfs_generation() != f->gen) fm_load(f);          /* changed elsewhere */
        if (f->msg[0] && sys_uptime_ms() >= f->msg_until) { f->msg[0] = 0; fm_status_dirty(f); }
        if (g_fclip.count && !fclip_valid()) ui_invalidate(f->view);   /* cut marks went stale */
        break;
    case EV_FOCUS:
        if (!ui_has_focus(f->addr)) ui_focus(f->view);
        ui_invalidate(f->view);
        break;
    case EV_BLUR:
        ui_invalidate(f->view);
        break;
    case EV_CLOSE:
        fm_job_free(f);
        free_thumbs(f->ents, f->nents);
        free(f->ents);
        free(f->band_base);
        free(f->pending_del);
        win_set_userdata(win, NULL);
        free(f);
        break;
    default:
        break;
    }
}

static void fm_launch(const char *arg) {
    char start[VFS_PATH_MAX] = "/home/user", select[VFS_NAME_MAX] = "";
    if (arg && *arg) {
        char norm[VFS_PATH_MAX];
        if (vfs_normalize(NULL, arg, norm) == 0) {
            if (vfs_is_dir(norm)) strlcpy(start, norm, sizeof start);
            else if (vfs_exists(norm)) {          /* a file: open its folder with it selected */
                vfs_dirname(norm, start);
                strlcpy(select, vfs_basename(norm), sizeof select);
            }
        }
    }
    if (!vfs_is_dir(start)) strlcpy(start, "/", sizeof start);

    files_t *f = calloc(1, sizeof *f);
    if (!f) return;
    window_t *win = win_create("Files", FM_W, FM_H, WIN_RESIZABLE);
    if (!win) { free(f); return; }
    f->win = win;
    f->focus = f->anchor = f->hover = -1;
    f->tb_hover = f->tb_pressed = TB_NONE;
    f->side_hover = -1;
    f->view_mode = VIEW_ICONS;
    win_set_userdata(win, f);
    win_set_event_handler(win, fm_event);
    win_set_min_size(win, 520, 300);

    f->addr = ui_textbox(win, 124, 8, 400, TB_BTN, start);
    ui_textbox_set_maxlen(f->addr, VFS_PATH_MAX - 1);
    ui_textbox_set_placeholder(f->addr, "Type a folder path and press Enter");
    ui_textbox_on_submit(f->addr, fm_addr_submit, f);
    f->view = ui_custom(win, SIDEBAR_W, TOOLBAR_H, FM_W - SIDEBAR_W, FM_H - TOOLBAR_H - STATUS_H, view_draw, view_event, f);
    if (!f->addr || !f->view) { win_close(win); return; }
    fm_layout(f, FM_W, FM_H);
    strlcpy(f->select_after, select, sizeof f->select_after);
    fm_set_dir(f, start);
    ui_focus(f->view);
    f->timer = win_set_timer(win, 500);
}

/* App icon: a blue folder with papers. */
static void fm_app_icon(surface_t *s, int x, int y, int size) {
    draw_app_folder(s, x, y + size / 32, size);
    double u = size / 64.0;
    /* a small magnifying glass badge */
    gfx_fill_circle_aa(s, UX(44), UY(43), 9.5 * u, RGBA(255, 255, 255, 235));
    gfx_circle_aa(s, UX(44), UY(43), 7.5 * u, 2.6 * u, RGB(30, 80, 170));
    gfx_thick_line(s, UX(49.5), UY(48.5), UX(56), UY(55), UL(3.4) > 0 ? UL(3.4) : 1, RGB(30, 80, 170));
}

REGISTER_APP(.name = "Files", .category = CAT_SYSTEM,
             .description = "Browse and manage your files and folders",
             .launch = fm_launch, .draw_icon = fm_app_icon);
