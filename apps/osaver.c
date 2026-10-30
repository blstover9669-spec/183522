/*
 * osaver.c - "OSaver": save your data as a number code (NovaOS 2.5, item 13).
 *
 *   Welcome screen   "Welcome to OSaver (OSsaver)", the text, a blue Start button
 *                    whose colour can be changed (Blue or one of the theme colours;
 *                    kept in /system/config/osaver)
 *   Start            makes a code of the current system and shows the warning with
 *                    Copy / View source with pretty print / View source with no
 *                    pretty print
 *
 * Making a code never blocks: the files are listed first (what changed compared with
 * a fresh system, see osaver_common.c), then the code is written in slices of a few
 * milliseconds per timer tick, straight into the file OSV_RAW_FILE (the device form,
 * with a hidden separator between the chunks of each key). Codes over 256 MB are
 * refused. Copy puts the code without the hidden separators on the clipboard, or into
 * OSV_COPY_FILE when it is too big for the clipboard. View source opens Notepad,
 * which only loads the part of the code that is on screen.
 *
 * Code layout (see kernel/osaver/codec.h):
 *     13224:<version>              version
 *     4351991:((<N>))              files: number of files saved (hidden and system files too)
 *     91235422:                    wallpaper (pretty print fills in "Custom" or the file target)
 *     3332198:((B..-W..-D..-F..-E..))   SystemData: boot IDs, wallpaper, file location IDs,
 *                                  files (settings, browser settings, app data, documents)
 */
#include "osaver_internal.h"

#define WEL_W   600
#define WEL_H   396
#define WARN_W  620
#define WARN_H  150
#define GEN_BUF (64u * 1024u)
#define RD_BUF  (256u * 1024u)
#define SLICE_MS 12

extern surface_t *g_wallpaper;      /* kernel/gui: the rendered wallpaper, screen size */

static const char *const WELCOME_TITLE = "Welcome to OSaver (OSsaver)";
static const char *const WELCOME_TEXT =
    "OSaver is an app to save your data including Apps save data, saving search history "
    "(can be turned off in browser settings), settings/browser settings, Wallpaper -- by "
    "using our number coding language! Click Start to enter in";
static const char *const WARNING_TEXT =
    "Saving and doing progress after this may be lost on restart or shutdown and turning on. "
    "Recommending to save new progress after save to avoid loss";

/* ======================================================================================
 * Making a code
 * ==================================================================================== */
typedef struct { char path[VFS_PATH_MAX]; int parent; bool keep; uint32_t id; } gdir_t;
typedef struct { char path[VFS_PATH_MAX]; int dir; uint32_t size; } gfile_t;

enum { G_SCAN, G_HEAD, G_DIRS, G_FILES, G_END, G_DONE, G_COPYFILE, G_FAILED };

typedef struct gen {
    int state;
    gdir_t *dirs;  int nd, capd;
    gfile_t *files; int nf, capf;
    int ndkept;
    uint64_t databytes;
    /* wallpaper */
    int wall_index;                     /* >= 0 built-in, -1 custom             */
    char wall_path[VFS_PATH_MAX];       /* custom: picture file ("" = none)     */
    uint8_t *wall_data; uint32_t wall_size;
    char wall_text[VFS_PATH_MAX];       /* pretty print: "Custom" / file target */
    char version[16];
    bool placeholder;
    uint64_t bound;                     /* upper bound of the code size         */
    /* output */
    osc_writer_t w;
    char *buf; uint32_t blen;
    uint32_t pos;                       /* bytes in the code file               */
    uint32_t head_dev, head_copy;       /* head length with / without hidden separators */
    /* progress */
    int di, fi;
    bool in_file;
    uint32_t fsize, foff;
    uint8_t *rbuf;
    uint32_t t0;
    uint64_t done_bytes;
    /* copy to file */
    uint32_t cpos;
    char err[200];
} gen_t;

static void gen_free(gen_t *g) {
    if (!g) return;
    free(g->dirs);
    free(g->files);
    free(g->wall_data);
    free(g->buf);
    free(g->rbuf);
    free(g);
}

static void gen_fail(gen_t *g, const char *msg) {
    if (g->state == G_FAILED) return;
    g->state = G_FAILED;
    strlcpy(g->err, msg, sizeof g->err);
    klog("osaver: %s", msg);
}

static int gen_flush(gen_t *g) {
    if (!g->blen || g->state == G_FAILED) return g->state == G_FAILED ? -1 : 0;
    int fd = vfs_open(OSV_RAW_FILE, VFS_O_WRITE | VFS_O_CREATE);
    int r = fd < 0 ? fd : vfs_seek(fd, (int32_t)g->pos, VFS_SEEK_SET);
    if (r >= 0) r = vfs_write(fd, g->buf, g->blen);
    if (fd >= 0) vfs_close(fd);
    if (r != (int)g->blen) { gen_fail(g, "There is not enough memory to make the code."); return -1; }
    g->pos += g->blen;
    g->blen = 0;
    return 0;
}

static int gen_sink(void *ctx, const char *s, uint32_t n) {
    gen_t *g = ctx;
    while (n) {
        uint32_t take = MIN(n, GEN_BUF - g->blen);
        memcpy(g->buf + g->blen, s, take);
        g->blen += take;
        s += take;
        n -= take;
        if (g->blen == GEN_BUF && gen_flush(g)) return -1;
    }
    if ((uint64_t)g->pos + g->blen > OSAVER_MAX_CODE) {
        gen_fail(g, "The code would be bigger than 256 MB, so it can't be made.");
        return -1;
    }
    return 0;
}

/* ---- listing what to save ---------------------------------------------------------- */
static int add_dir(gen_t *g, const char *path, int parent, bool keep) {
    if (g->nd == g->capd) {
        int nc = g->capd ? g->capd * 2 : 64;
        gdir_t *n = realloc(g->dirs, (size_t)nc * sizeof *n);
        if (!n) return -1;
        g->dirs = n;
        g->capd = nc;
    }
    gdir_t *d = &g->dirs[g->nd];
    strlcpy(d->path, path, sizeof d->path);
    d->parent = parent;
    d->keep = keep;
    d->id = 0;
    return g->nd++;
}

static bool add_file(gen_t *g, const char *path, int dir, uint32_t size) {
    if (g->nf == g->capf) {
        int nc = g->capf ? g->capf * 2 : 64;
        gfile_t *n = realloc(g->files, (size_t)nc * sizeof *n);
        if (!n) return false;
        g->files = n;
        g->capf = nc;
    }
    gfile_t *f = &g->files[g->nf++];
    strlcpy(f->path, path, sizeof f->path);
    f->dir = dir;
    f->size = size;
    g->databytes += size;
    return true;
}

/* The recent-items list that only says "OSaver was opened" is not something to save
   (otherwise opening OSaver on a fresh system would never give the placeholder). */
static bool only_osaver_recent(const char *path) {
    if (strcmp(path, "/system/config/recent")) return false;
    char *t = vfs_read_file(path, NULL);
    if (!t) return false;
    bool only = true;
    char *save = NULL;
    for (char *line = strtok_r(t, "\r\n", &save); line && only; line = strtok_r(NULL, "\r\n", &save))
        if (*line && *line != '#' && strcmp(line, "app:OSaver")) only = false;
    free(t);
    return only;
}

static void scan(gen_t *g, const char *dir, int parent, int depth) {
    if (depth > 64) return;
    int n = vfs_list(dir, NULL, 0);
    if (n <= 0) return;
    vfs_dirent_t *ents = malloc((size_t)n * sizeof *ents);
    if (!ents) return;
    n = vfs_list(dir, ents, n);
    for (int i = 0; i < n; i++) {
        char path[VFS_PATH_MAX];
        vfs_join(dir, ents[i].name, path);
        if (strlen(path) >= VFS_PATH_MAX - 1 || !osv_path_saved(path, ents[i].is_dir)) continue;
        if (ents[i].is_dir) {
            /* folders the user made (even empty ones) are saved; system folders only
               when they hold saved files */
            bool sys = !strncmp(path, "/system", 7) && (path[7] == 0 || path[7] == '/');
            int idx = add_dir(g, path, parent, !sys && !osv_default_dir(path));
            if (idx >= 0) scan(g, path, idx, depth + 1);
        } else if (!osv_default_file(path, ents[i].size) && !only_osaver_recent(path)) {
            add_file(g, path, parent, ents[i].size);
        }
    }
    free(ents);
}

static int digits(uint64_t v) { int n = 1; while (v >= 10) { v /= 10; n++; } return n; }
static uint64_t b62f(uint32_t n) { return 1 + (uint64_t)osc_b62_len(n); }     /* "-<base62>" */

/* The custom wallpaper's picture: the file chosen in Settings, else the wallpaper as
   it is on screen (saved as a BMP). */
static void find_wallpaper(gen_t *g) {
    const theme_t *t = theme_get();
    g->wall_index = t->wallpaper;
    if (t->wallpaper >= 0) {
        osv_wallpaper_target(t->wallpaper, g->wall_text, sizeof g->wall_text);
        return;
    }
    strlcpy(g->wall_text, "Custom", sizeof g->wall_text);
    char *cfg = vfs_read_file("/system/config/settings", NULL);
    if (cfg) {
        char *p = strstr(cfg, "\nwallimage=");
        if (p) {
            p += 11;
            char *e = strpbrk(p, "\r\n");
            if (e) *e = 0;
            if (*p == '/' && vfs_exists(p) && !vfs_is_dir(p)) strlcpy(g->wall_path, p, sizeof g->wall_path);
        }
        free(cfg);
    }
    if (g->wall_path[0]) {
        for (int i = 0; i < g->nf; i++)
            if (!strcmp(g->files[i].path, g->wall_path)) return;     /* saved as a file anyway */
        g->wall_data = vfs_read_file(g->wall_path, &g->wall_size);
        if (g->wall_data) return;
        g->wall_path[0] = 0;
    }
    if (g_wallpaper) {
        size_t sz = 0;
        g->wall_data = image_encode_bmp(g_wallpaper, &sz);
        g->wall_size = g->wall_data ? (uint32_t)sz : 0;
    }
}

static void gen_scan(gen_t *g) {
    osv_version(g->version, sizeof g->version);
    scan(g, "/", -1, 0);
    /* folders needed by saved files, and folders that are not in a fresh system */
    for (int i = 0; i < g->nf; i++) if (g->files[i].dir >= 0) g->dirs[g->files[i].dir].keep = true;
    for (int i = g->nd - 1; i >= 0; i--)
        if (g->dirs[i].keep && g->dirs[i].parent >= 0) g->dirs[g->dirs[i].parent].keep = true;
    uint32_t id = 0;
    for (int i = 0; i < g->nd; i++) if (g->dirs[i].keep) g->dirs[i].id = ++id;
    g->ndkept = (int)id;
    find_wallpaper(g);
    g->placeholder = g->nf == 0 && g->ndkept == 0;

    char head[256];
    int hl = g->placeholder ? osc_placeholder(head, sizeof head, true)
                            : osc_head(head, sizeof head, g->version, g->nf, true);
    g->head_dev = (uint32_t)hl;
    g->head_copy = 0;
    for (int i = 0; i < hl; i++) if (head[i] != OSC_HS) g->head_copy++;
    uint64_t b = (uint64_t)hl + 2;
    if (!g->placeholder) {
        b += 1 + digits(osv_boot_id()) + 1 + digits(sys_time()) + 2 + 1 + digits((uint32_t)g->nf) +
             1 + digits((uint32_t)g->ndkept) + 1 + 10;
        if (g->wall_index >= 0) b += 3 + 1 + digits((uint32_t)g->wall_index) + b62f(64);
        else b += 3 + b62f((uint32_t)strlen(g->wall_path)) + 1 + digits(g->wall_size) + b62f(g->wall_size);
        for (int i = 0; i < g->nd; i++) {
            if (!g->dirs[i].keep) continue;
            b += 2 + digits(g->dirs[i].id) + 1 + 10 + b62f((uint32_t)strlen(vfs_basename(g->dirs[i].path)));
        }
        for (int i = 0; i < g->nf; i++) {
            const char *base = vfs_basename(g->files[i].path);
            b += 2 + 10 + b62f((uint32_t)strlen(base)) + b62f((uint32_t)strlen(base)) + 1 + 10 + b62f(g->files[i].size);
        }
        b += 2 + 10;
    }
    g->bound = b;
    klog("osaver: saving %d files, %d folders, %u bytes of data (code up to %u bytes)%s",
         g->nf, g->ndkept, (uint32_t)g->databytes, (uint32_t)MIN(b, 0xFFFFFFFFull),
         g->placeholder ? " - nothing saved yet: placeholder" : "");
    if (osv_test("log"))
        for (int i = 0; i < g->nf; i++) {
            uint32_t sz = 0;
            void *d = vfs_read_file(g->files[i].path, &sz);
            klog("osaver:   file %s (%u bytes, crc %08x)", g->files[i].path, g->files[i].size, d ? osc_crc32(0, d, sz) : 0);
            free(d);
        }
    if (b > OSAVER_MAX_CODE) { gen_fail(g, "The code would be bigger than 256 MB, so it can't be made."); return; }

    /* the code file: drop the old one, reserve the space in one go */
    vfs_mkdirs(OSV_DIR);
    vfs_write_file(OSV_RAW_FILE, "", 0);
    if (b > 64 * 1024) {
        int fd = vfs_open(OSV_RAW_FILE, VFS_O_WRITE);
        int r = fd < 0 ? fd : vfs_seek(fd, (int32_t)(b - 1), VFS_SEEK_SET);
        if (r >= 0) r = vfs_write(fd, "", 1);
        if (fd >= 0) vfs_close(fd);
        if (r != 1) { vfs_write_file(OSV_RAW_FILE, "", 0); gen_fail(g, "There is not enough memory to make the code."); return; }
        fd = vfs_open(OSV_RAW_FILE, VFS_O_WRITE | VFS_O_TRUNC);
        if (fd >= 0) vfs_close(fd);
    }
    g->state = G_HEAD;
}

static void gen_head(gen_t *g) {
    char head[256];
    if (g->placeholder) {
        int hl = osc_placeholder(head, sizeof head, true);
        gen_sink(g, head, (uint32_t)hl);
        if (g->state != G_FAILED) g->state = G_END;
        return;
    }
    int hl = osc_head(head, sizeof head, g->version, g->nf, true);
    if (gen_sink(g, head, (uint32_t)hl)) return;
    osc_w_init(&g->w, gen_sink, g);
    osc_writer_t *w = &g->w;
    osc_w_record(w, 'B', osv_boot_id());
    osc_w_num(w, sys_time());
    osc_w_num(w, osv_boot_kind());
    osc_w_num(w, (uint32_t)g->nf);
    osc_w_num(w, (uint32_t)g->ndkept);
    osc_w_num(w, (uint32_t)MIN(g->databytes, 0xFFFFFFFFull));
    if (g->wall_index >= 0) {
        const char *name = theme_wallpaper_name(g->wall_index);
        if (!name) name = "";
        osc_w_record(w, 'W', 0);
        osc_w_num(w, (uint32_t)g->wall_index);
        osc_w_bytes(w, name, (uint32_t)MIN(strlen(name), 64));
    } else {
        osc_w_record(w, 'W', 1);
        osc_w_bytes(w, g->wall_path, (uint32_t)strlen(g->wall_path));
        osc_w_num(w, g->wall_data ? g->wall_size : 0);
        osc_w_bytes(w, g->wall_data, g->wall_data ? g->wall_size : 0);
        free(g->wall_data);
        g->wall_data = NULL;
    }
    if (w->err) { gen_fail(g, g->err[0] ? g->err : "The code could not be written."); return; }
    if (g->state != G_FAILED) g->state = G_DIRS;
}

static void gen_dirs(gen_t *g) {
    osc_writer_t *w = &g->w;
    for (; g->di < g->nd && g->state == G_DIRS; g->di++) {
        gdir_t *d = &g->dirs[g->di];
        if (!d->keep) continue;
        const char *name = vfs_basename(d->path);
        osc_w_record(w, 'D', d->id);
        osc_w_num(w, d->parent >= 0 ? g->dirs[d->parent].id : 0);
        osc_w_bytes(w, name, (uint32_t)strlen(name));
    }
    if (g->state == G_DIRS) g->state = G_FILES;
}

/* One step of the files: a record's fields, or the next piece of its data. */
static void gen_file_step(gen_t *g) {
    osc_writer_t *w = &g->w;
    if (g->fi >= g->nf) { g->state = G_END; return; }
    gfile_t *f = &g->files[g->fi];
    if (!g->in_file) {
        char name[VFS_NAME_MAX], type[VFS_NAME_MAX];
        osv_split_name(vfs_basename(f->path), name, sizeof name, type, sizeof type);
        vfs_stat_t st;
        g->fsize = (vfs_stat(f->path, &st) == 0 && !st.is_dir) ? st.size : 0;
        g->foff = 0;
        osc_w_record(w, 'F', f->dir >= 0 ? g->dirs[f->dir].id : 0);
        osc_w_bytes(w, name, (uint32_t)strlen(name));
        osc_w_bytes(w, type, (uint32_t)strlen(type));
        osc_w_num(w, g->fsize);
        osc_w_data_begin(w);
        g->in_file = true;
    }
    if (g->foff < g->fsize) {
        uint32_t n = MIN(RD_BUF, g->fsize - g->foff);
        int got = 0;
        int fd = vfs_open(f->path, VFS_O_READ);
        if (fd >= 0) {
            if (vfs_seek(fd, (int32_t)g->foff, VFS_SEEK_SET) >= 0) got = vfs_read(fd, g->rbuf, n);
            vfs_close(fd);
        }
        if (got < 0) got = 0;
        if ((uint32_t)got < n) memset(g->rbuf + got, 0, n - (uint32_t)got);   /* the file shrank meanwhile */
        osc_w_data_part(w, g->rbuf, n);
        g->foff += n;
        g->done_bytes += n;
    }
    if (g->foff >= g->fsize) {
        osc_w_data_end(w);
        g->in_file = false;
        g->fi++;
    }
}

static void gen_log_code(gen_t *g) {
    if (!osv_test("log") || g->pos > 8192) return;
    char *code = vfs_read_file(OSV_RAW_FILE, NULL);
    if (!code) return;
    char line[208];
    int n = 0;
    for (char *p = code; ; p++) {
        if (*p == OSC_HS) continue;
        if (*p == '\n' || !*p || n == 200) {
            line[n] = 0;
            if (n) klog("OSAVER-CODE: %s", line);
            n = 0;
            if (!*p) break;
            if (*p == '\n') continue;
        }
        line[n++] = *p;
    }
    klog("OSAVER-CODE-END");
    free(code);
}

/* Run the code maker for one time slice. */
static void gen_run(gen_t *g) {
    uint32_t start = sys_uptime_ms();
    while (g->state != G_DONE && g->state != G_FAILED && g->state != G_COPYFILE) {
        switch (g->state) {
        case G_SCAN:  gen_scan(g); break;
        case G_HEAD:  gen_head(g); break;
        case G_DIRS:  gen_dirs(g); break;
        case G_FILES: gen_file_step(g); break;
        case G_END:
            if (!g->placeholder) {
                osc_w_end(&g->w);
                gen_sink(g, OSC_TAIL, 2);
            }
            if (g->state == G_FAILED) break;
            if (gen_flush(g)) break;
            g->state = G_DONE;
            klog("osaver: code made: %d files, %d folders, %u characters in %u ms", g->nf, g->ndkept,
                 g->pos, sys_uptime_ms() - g->t0);
            gen_log_code(g);
            break;
        }
        if (g->w.err && g->state != G_FAILED) gen_fail(g, "There is not enough memory to make the code.");
        if (sys_uptime_ms() - start >= SLICE_MS) break;
    }
}

static gen_t *gen_new(void) {
    gen_t *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->buf = malloc(GEN_BUF);
    g->rbuf = malloc(RD_BUF);
    if (!g->buf || !g->rbuf) { gen_free(g); return NULL; }
    g->state = G_SCAN;
    g->t0 = sys_uptime_ms();
    return g;
}

static int gen_progress(const gen_t *g) {     /* 0..1000 */
    if (g->state == G_DONE) return 1000;
    if (g->state == G_COPYFILE) return g->pos ? (int)((uint64_t)g->cpos * 1000 / g->pos) : 0;
    if (!g->bound) return 0;
    return (int)MIN(999, (uint64_t)(g->pos + g->blen) * 1000 / g->bound);
}

/* ======================================================================================
 * The warning
 * ==================================================================================== */
typedef struct osaver osaver_t;

typedef struct warn {
    window_t *win;
    osaver_t *app;
    gen_t *gen;
    widget_t *b_copy, *b_pretty, *b_raw;
    int timer;
    bool copy_close;            /* close when the copy to a file is finished */
} warn_t;

struct osaver {
    window_t *win;
    widget_t *start, *swatch;
    warn_t *warn;
    bool start_down;
    int swatch_hover;
};

static osaver_t *g_osaver;

static void warn_buttons(warn_t *wn) {
    bool ready = wn->gen && wn->gen->state == G_DONE;
    ui_set_enabled(wn->b_copy, ready);
    ui_set_enabled(wn->b_pretty, ready);
    ui_set_enabled(wn->b_raw, ready);
}

/* Copy form = the code without the hidden separators (only the head has them). */
static void copy_to_clipboard(warn_t *wn) {
    uint32_t size = 0;
    char *code = vfs_read_file(OSV_RAW_FILE, &size);
    if (!code) { ui_message_box(wn->win, "OSaver", "The code could not be read.", MB_OK | MB_ICON_ERROR, NULL, NULL); return; }
    uint32_t o = 0;
    for (uint32_t i = 0; i < size; i++) if (code[i] != OSC_HS) code[o++] = code[i];
    code[o] = 0;
    clipboard_set(code);
    free(code);
    klog("osaver: copied the code (%u characters) to the clipboard", o);
    ui_notify("OSaver", "The code was copied.");
    win_close(wn->win);
}

static void start_copy_file(warn_t *wn) {
    gen_t *g = wn->gen;
    char head[256];
    int hl = osc_head(head, sizeof head, g->version, g->nf, false);
    vfs_write_file(OSV_COPY_FILE, "", 0);
    uint32_t total = g->head_copy + (g->pos - g->head_dev);
    int fd = vfs_open(OSV_COPY_FILE, VFS_O_WRITE | VFS_O_CREATE);
    int r = fd < 0 ? fd : vfs_seek(fd, (int32_t)(total - 1), VFS_SEEK_SET);
    if (r >= 0) r = vfs_write(fd, "", 1);
    if (fd >= 0) vfs_close(fd);
    if (r != 1) {
        vfs_write_file(OSV_COPY_FILE, "", 0);
        ui_message_box(wn->win, "OSaver", "There is not enough memory to copy the code.", MB_OK | MB_ICON_ERROR, NULL, NULL);
        return;
    }
    fd = vfs_open(OSV_COPY_FILE, VFS_O_WRITE);
    if (fd >= 0) { vfs_write(fd, head, (uint32_t)hl); vfs_close(fd); }
    g->cpos = g->head_dev;            /* next byte of the code file to copy */
    g->state = G_COPYFILE;
    wn->copy_close = true;
    warn_buttons(wn);
    win_invalidate(wn->win);
}

/* Copy the rest of the code into OSV_COPY_FILE, a few MB per tick. */
static void copy_file_step(warn_t *wn) {
    gen_t *g = wn->gen;
    uint32_t start = sys_uptime_ms();
    while (g->cpos < g->pos && sys_uptime_ms() - start < SLICE_MS) {
        uint32_t n = MIN(RD_BUF, g->pos - g->cpos);
        int got = -1;
        int fd = vfs_open(OSV_RAW_FILE, VFS_O_READ);
        if (fd >= 0) { vfs_seek(fd, (int32_t)g->cpos, VFS_SEEK_SET); got = vfs_read(fd, g->rbuf, n); vfs_close(fd); }
        if (got != (int)n) break;
        fd = vfs_open(OSV_COPY_FILE, VFS_O_WRITE);
        if (fd >= 0) {
            vfs_seek(fd, (int32_t)(g->head_copy + (g->cpos - g->head_dev)), VFS_SEEK_SET);
            vfs_write(fd, g->rbuf, n);
            vfs_close(fd);
        }
        g->cpos += n;
    }
    if (g->cpos >= g->pos) {
        g->state = G_DONE;
        klog("osaver: the code (%u characters) is too big for the clipboard, copied to %s",
             g->head_copy + (g->pos - g->head_dev), OSV_COPY_FILE);
        ui_notify("OSaver", "The code is too big for the clipboard, so it was copied to "
                            "/home/user/Documents/OSaver code.txt");
        win_close(wn->win);
    }
}

static void on_copy(widget_t *w, void *ctx) {
    warn_t *wn = ctx;
    gen_t *g = wn->gen;
    if (!g || g->state != G_DONE) return;
    uint32_t copy_len = g->head_copy + (g->pos - g->head_dev);
    if (copy_len > OSV_CLIP_MAX) start_copy_file(wn);
    else copy_to_clipboard(wn);
}

/* Pretty print: parse the head of the code, then osc_pretty(). */
static void on_pretty(widget_t *w, void *ctx) {
    warn_t *wn = ctx;
    gen_t *g = wn->gen;
    if (!g || g->state != G_DONE) return;
    char head[512];
    int fd = vfs_open(OSV_RAW_FILE, VFS_O_READ);
    int n = fd >= 0 ? vfs_read(fd, head, sizeof head) : -1;
    if (fd >= 0) vfs_close(fd);
    if (n <= 0) return;
    static osc_parser_t p;              /* big: not on the stack */
    osc_parser_init(&p, NULL);
    osc_parser_feed(&p, head, (uint32_t)(g->placeholder ? (uint32_t)n : MIN((uint32_t)n, g->head_dev)));
    char wall[VFS_PATH_MAX];
    if (g->placeholder) osv_wallpaper_target(theme_get()->wallpaper, wall, sizeof wall);
    else strlcpy(wall, g->wall_text, sizeof wall);
    char out[1024];
    int len = osc_pretty(out, sizeof out, &p, wall);
    if (len < 0) { klog("osaver: pretty print failed: %s", osc_parser_error(&p) ? osc_parser_error(&p) : "?"); return; }
    vfs_mkdirs(OSV_DIR);
    vfs_write_file(OSV_PRETTY_FILE, out, (uint32_t)len);
    app_launch_by_name("Notepad", OSV_PRETTY_FILE);
}

static void on_raw(widget_t *w, void *ctx) {
    warn_t *wn = ctx;
    if (!wn->gen || wn->gen->state != G_DONE) return;
    app_launch_by_name("Notepad", OSV_RAW_FILE);
}

static void warn_event(window_t *win, const event_t *ev) {
    warn_t *wn = win_get_userdata(win);
    if (!wn) return;
    const theme_t *t = theme_get();
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        ui_draw_glyph(s, UI_GLYPH_WARNING, 22, 22, 32, RGB(240, 170, 30));
        gfx_text_wrapped(s, rect_make(70, 22, WARN_W - 92, 50), WARNING_TEXT, t->window_text, FONT_UI);
        gen_t *g = wn->gen;
        if (g && g->state != G_DONE && g->state != G_FAILED) {
            rect_t b = rect_make(70, 76, WARN_W - 92, 6);
            gfx_fill_rounded_rect(s, b.x, b.y, b.w, b.h, 3, color_mix(t->window_bg, t->window_text, 40));
            int fw = (int)((int64_t)b.w * gen_progress(g) / 1000);
            if (fw >= 6) gfx_fill_rounded_rect(s, b.x, b.y, fw, b.h, 3, t->accent);
        }
        break;
    }
    case EV_TIMER: {
        gen_t *g = wn->gen;
        if (!g) break;
        if (g->state == G_COPYFILE) { copy_file_step(wn); if (!win_is_valid(win)) return; win_invalidate(win); break; }
        if (g->state != G_DONE && g->state != G_FAILED) {
            gen_run(g);
            win_invalidate_rect(win, 60, 66, WARN_W - 60, 24);
            if (g->state == G_DONE) {
                warn_buttons(wn);
                win_invalidate(win);
            }
        }
        if (g->state == G_FAILED) {     /* (also when it failed right in open_warning) */
            char msg[240];
            strlcpy(msg, g->err, sizeof msg);
            window_t *owner = wn->app ? wn->app->win : NULL;
            win_close(win);
            ui_message_box(owner, "OSaver", msg, MB_OK | MB_ICON_ERROR, NULL, NULL);
            return;
        }
        break;
    }
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESCAPE) win_close(win);
        break;
    case EV_CLOSE:
        if (wn->app && wn->app->warn == wn) wn->app->warn = NULL;
        gen_free(wn->gen);
        win_set_userdata(win, NULL);
        free(wn);
        break;
    default:
        break;
    }
}

static void open_warning(osaver_t *app) {
    if (app->warn && win_is_valid(app->warn->win)) { win_focus(app->warn->win); return; }
    warn_t *wn = calloc(1, sizeof *wn);
    if (!wn) return;
    wn->gen = gen_new();
    if (!wn->gen) { free(wn); return; }
    window_t *win = win_create_dialog(app->win, "OSaver", WARN_W, WARN_H, 0);
    if (!win) { gen_free(wn->gen); free(wn); return; }
    wn->win = win;
    wn->app = app;
    app->warn = wn;
    win_set_userdata(win, wn);
    win_set_event_handler(win, warn_event);
    int y = WARN_H - 52;
    wn->b_copy = ui_button(win, 22, y, 110, 32, "Copy", on_copy, wn);
    wn->b_pretty = ui_button(win, 144, y, 216, 32, "View source with pretty print", on_pretty, wn);
    wn->b_raw = ui_button(win, 372, y, WARN_W - 394, 32, "View source with no pretty print", on_raw, wn);
    warn_buttons(wn);
    wn->timer = win_set_timer(win, 10);
    gen_run(wn->gen);             /* small systems are done right away */
    if (wn->gen->state == G_DONE) warn_buttons(wn);
}

/* ======================================================================================
 * Welcome screen
 * ==================================================================================== */
static bool mouse_over(window_t *win, rect_t r) {
    int x, y;
    input_mouse_pos(&x, &y);
    rect_t cr = win_client_rect(win);
    return rect_contains(r, x - cr.x, y - cr.y);
}

static void start_draw(widget_t *w, surface_t *s, rect_t r) {
    osaver_t *app = ui_get_ctx(w);
    uint32_t c = osv_start_color();
    bool hover = mouse_over(app->win, r);
    if (app->start_down && hover) c = color_shade(c, -22);
    else if (hover) c = color_shade(c, 16);
    gfx_shadow(s, r.x, r.y, r.w, r.h, r.h / 2, 8, 50);
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, r.h / 2, c);
    gfx_fill_rounded_rect(s, r.x + 2, r.y + 1, r.w - 4, r.h / 2 - 1, r.h / 2 - 2, RGBA(255, 255, 255, 26));
    uint32_t text = color_luma(c) > 170 ? RGB(20, 24, 32) : COLOR_WHITE;
    gfx_text_aligned(s, r, "Start", text, FONT_UI_LARGE, ALIGN_CENTER | ALIGN_MIDDLE);
    if (ui_has_focus(w)) gfx_rounded_rect(s, r.x - 3, r.y - 3, r.w + 6, r.h + 6, r.h / 2 + 3, color_alpha(c, 140));
}

static bool start_event(widget_t *w, const event_t *ev) {
    osaver_t *app = ui_get_ctx(w);
    rect_t r = ui_rect(w);
    switch (ev->type) {
    case EV_MOUSE_DOWN:
        if (ev->button != MOUSE_LEFT) return true;
        app->start_down = true;
        ui_invalidate(w);
        return true;
    case EV_MOUSE_MOVE:
        ui_invalidate(w);
        return true;
    case EV_MOUSE_UP:
        if (app->start_down) {
            app->start_down = false;
            ui_invalidate(w);
            if (rect_contains(r, ev->x, ev->y)) open_warning(app);
        }
        return true;
    case EV_KEY_DOWN:
        if (ev->key == KEY_ENTER || ev->key == ' ') { open_warning(app); return true; }
        return false;
    default:
        return false;
    }
}

/* Colour swatches for the Start button: Blue, then the theme's colours. */
#define SW_SIZE 20
#define SW_GAP  8
static int swatch_count(void) { return 1 + theme_accent_count(); }
static uint32_t swatch_color(int i) { return i == 0 ? OSV_BLUE : (theme_accent_preset(i - 1) | 0xFF000000u); }
static int swatch_at(rect_t r, int x, int y) {
    if (y < r.y || y >= r.y + r.h) return -1;
    int i = (x - r.x) / (SW_SIZE + SW_GAP);
    if (x < r.x || i >= swatch_count()) return -1;
    return (x - r.x) % (SW_SIZE + SW_GAP) < SW_SIZE ? i : -1;
}

static void swatch_draw(widget_t *w, surface_t *s, rect_t r) {
    osaver_t *app = ui_get_ctx(w);
    const theme_t *t = theme_get();
    int sel = osv_start_choice();
    for (int i = 0; i < swatch_count(); i++) {
        int x = r.x + i * (SW_SIZE + SW_GAP) + SW_SIZE / 2, y = r.y + r.h / 2;
        uint32_t c = swatch_color(i);
        if (i == sel) gfx_circle_aa(s, x, y, SW_SIZE / 2 + 3, 2, t->window_text);
        else if (i == app->swatch_hover) gfx_circle_aa(s, x, y, SW_SIZE / 2 + 3, 1, color_mix(t->window_bg, t->window_text, 110));
        gfx_fill_circle_aa(s, x, y, SW_SIZE / 2, c);
    }
}

static bool swatch_event(widget_t *w, const event_t *ev) {
    osaver_t *app = ui_get_ctx(w);
    rect_t r = ui_rect(w);
    int i = swatch_at(r, ev->x, ev->y);
    switch (ev->type) {
    case EV_MOUSE_MOVE:
        if (i != app->swatch_hover) { app->swatch_hover = i; ui_invalidate(w); }
        return true;
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_LEFT && i >= 0) {
            osv_set_start_choice(i);
            ui_invalidate(w);
            ui_invalidate(app->start);
        }
        return true;
    case EV_KEY_DOWN:
        if (ev->key == KEY_LEFT || ev->key == KEY_RIGHT) {
            int n = swatch_count(), c = osv_start_choice();
            osv_set_start_choice((c + (ev->key == KEY_RIGHT ? 1 : n - 1)) % n);
            ui_invalidate(w);
            ui_invalidate(app->start);
            return true;
        }
        return false;
    default:
        return false;
    }
}

static void osaver_icon(surface_t *s, int x, int y, int size);

static void osaver_event(window_t *win, const event_t *ev) {
    osaver_t *app = win_get_userdata(win);
    if (!app) return;
    const theme_t *t = theme_get();
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        int w = win_width(win);
        osaver_icon(s, 28, 26, 56);
        gfx_text(s, 100, 34, WELCOME_TITLE, t->window_text, FONT_UI_TITLE);
        gfx_text_wrapped(s, rect_make(32, 104, w - 64, 120), WELCOME_TEXT, t->window_text, FONT_UI_LARGE);
        rect_t sr = ui_rect(app->swatch);
        const char *label = "Start button colour";
        gfx_text(s, sr.x - 14 - font_text_width(FONT_UI, label), sr.y + (sr.h - font_height(FONT_UI)) / 2,
                 label, color_mix(t->window_text, t->window_bg, 80), FONT_UI);
        break;
    }
    case EV_MOUSE_MOVE:
    case EV_MOUSE_LEAVE:
        if (app->swatch_hover >= 0) { app->swatch_hover = -1; ui_invalidate(app->swatch); }
        ui_invalidate(app->start);
        break;
    case EV_THEME:
        ui_invalidate(app->start);
        break;
    case EV_FOCUS:
        ui_focus(app->start);
        break;
    case EV_CLOSE:
        if (g_osaver == app) g_osaver = NULL;
        win_set_userdata(win, NULL);
        free(app);
        break;
    default:
        break;
    }
}

static void osaver_launch(const char *arg) {
    (void)arg;
    if (g_osaver && win_is_valid(g_osaver->win)) { win_focus(g_osaver->win); return; }
    osaver_t *app = calloc(1, sizeof *app);
    if (!app) return;
    window_t *win = win_create("OSaver", WEL_W, WEL_H, WIN_CENTER);
    if (!win) { free(app); return; }
    app->win = win;
    app->swatch_hover = -1;
    g_osaver = app;
    win_set_userdata(win, app);
    win_set_event_handler(win, osaver_event);
    app->start = ui_custom(win, (WEL_W - 176) / 2, 250, 176, 48, start_draw, start_event, app);
    int sw = swatch_count() * (SW_SIZE + SW_GAP) - SW_GAP;
    int lw = font_text_width(FONT_UI, "Start button colour") + 14;
    app->swatch = ui_custom(win, (WEL_W - sw - lw) / 2 + lw, WEL_H - 50, sw, SW_SIZE + 8, swatch_draw, swatch_event, app);
    ui_focus(app->start);
}

/* Icon: a blue tile with a white "save" disk carrying the number code. */
static void osaver_icon(surface_t *s, int x, int y, int size) {
    double u = size / 64.0;
#define U(v) ((int)((v) * u + 0.5))
    gfx_fill_rounded_gradient_v(s, x + U(4), y + U(4), U(56), U(56), U(13), RGB(64, 140, 255), RGB(24, 88, 214));
    gfx_fill_rounded_rect(s, x + U(6), y + U(6), U(52), U(24), U(11), RGBA(255, 255, 255, 30));
    /* disk */
    gfx_fill_rounded_rect(s, x + U(15), y + U(13), U(34), U(38), U(5), RGB(250, 252, 255));
    gfx_fill_rect(s, x + U(22), y + U(13), U(18), U(11), RGB(170, 196, 240));
    gfx_fill_rect(s, x + U(34), y + U(15), U(4), U(7), RGB(250, 252, 255));
    /* label with the number code */
    gfx_fill_rounded_rect(s, x + U(19), y + U(30), U(26), U(17), U(3), RGB(30, 110, 235));
    if (size >= 40) gfx_text_aligned(s, rect_make(x + U(19), y + U(30), U(26), U(17)), "132", COLOR_WHITE,
                                     size >= 96 ? FONT_UI_LARGE : FONT_UI_BOLD, ALIGN_CENTER | ALIGN_MIDDLE);
    else for (int i = 0; i < 3; i++)
        gfx_fill_rect(s, x + U(23 + i * 7), y + U(36), U(4), U(5), COLOR_WHITE);
#undef U
}

REGISTER_APP(.name = "OSaver", .category = CAT_SYSTEM,
             .description = "Save your data as a number code",
             .launch = osaver_launch, .draw_icon = osaver_icon, .flags = APP_SINGLE);
