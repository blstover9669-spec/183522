/*
 * osaver_restore.c - loading an OSaver code (NovaOS 2.5, item 13 "Loading a code").
 *
 * A soft restart, not a reboot: a full-screen splash-style screen (the Nova icon
 * rises into the middle, then the loading bar shows the progress) covers the desktop
 * while the code is
 *
 *   1. checked completely (structure, every record, the files count, the check
 *      number) - a code that fails changes nothing;
 *   2. the open apps are closed (as a restart would);
 *   3. decoded into the real OS format and added back: folders (file location IDs)
 *      are created, and for each file: create it -> rename it to the exact name ->
 *      set the file type (its extension) -> write the text inside; an existing file
 *      with the same name is replaced;
 *   4. the restored settings are applied (theme, wallpaper - a custom picture comes
 *      out of SystemData -, taskbar pins, file types, screensaver).
 *
 * Everything runs in slices of a few milliseconds from the screen's timer, reading
 * the code in pieces (from memory or straight from a file), so even a 256 MB code
 * never blocks the system. Nothing can be written outside the places a code may
 * hold (see osv_path_saved): the kernel, boot files and UI files in /system are
 * never touched, so a code cannot make NovaOS unbootable.
 */
#include "osaver_internal.h"

#define PIECE      (256u * 1024u)     /* code read per step               */
#define STAGE      (256u * 1024u)     /* file data written per flush      */
#define WALL_MAX   (64u << 20)        /* largest custom wallpaper picture */
#define FRAME_MS   15
#define FADE_MS    280
#define RISE_MS    1000
#define MIN_MS     2600               /* the load screen shows at least this long */
#define WORK_MS    9

enum { P_CHECK, P_CLOSE, P_APPLY, P_POST, P_END, P_FAILED };

typedef struct { uint32_t id; char *path; } dmap_t;

typedef struct restore {
    /* source */
    char *text;                       /* memory source (owned copy), or NULL   */
    char path[VFS_PATH_MAX];          /* file source                           */
    uint32_t total, off;
    uint8_t *piece;
    osc_parser_t parser;
    int phase;
    void (*done)(void *, int);
    void *ctx;
    /* apply */
    dmap_t *dirs; int nd, capd;
    char cur[VFS_PATH_MAX];           /* file being written ("" = skipped)     */
    uint8_t *stage; uint32_t slen, cur_off;
    bool cur_wall;                    /* data goes to the wallpaper picture    */
    int wall_kind;                    /* -1 none, 0 built-in, 1 custom         */
    uint32_t wall_index;
    char wall_path[VFS_PATH_MAX];
    uint8_t *wall_data; uint32_t wall_size, wall_got;
    uint32_t nfiles, nskipped, ndirs;
    /* screen */
    window_t *win;
    int timer;
    uint32_t t0;
    surface_t *from;                  /* the screen when the restore started (fade in) */
    surface_t *glow;
    bool animate;
    int logo, glow_r;
    int icon_y;                       /* current icon top                      */
    int shown;                        /* bar value on screen, 0..1000          */
    bool bar_on;
    uint32_t work_done_at;
} restore_t;

static restore_t *R;

bool osv_restore_running(void) { return R != NULL; }

/* ======================================================================================
 * Reading the code
 * ==================================================================================== */
/* Feed the next slice of the code to the parser. Returns true at the end. */
static bool feed_slice(restore_t *r) {
    uint32_t start = sys_uptime_ms();
    while (r->off < r->total && sys_uptime_ms() - start < WORK_MS) {
        uint32_t n = MIN(64u * 1024u, r->total - r->off);
        const char *p;
        if (r->text) p = r->text + r->off;
        else {
            n = MIN(PIECE, r->total - r->off);
            int got = -1;
            int fd = vfs_open(r->path, VFS_O_READ);
            if (fd >= 0) {
                if (vfs_seek(fd, (int32_t)r->off, VFS_SEEK_SET) >= 0) got = vfs_read(fd, r->piece, n);
                vfs_close(fd);
            }
            if (got != (int)n) { r->parser.err = -1; strlcpy(r->parser.errmsg, "the code file could not be read", sizeof r->parser.errmsg); return true; }
            p = (const char *)r->piece;
        }
        if (osc_parser_feed(&r->parser, p, n)) return true;
        r->off += n;
    }
    return r->off >= r->total;
}

/* ======================================================================================
 * Applying: folders and files
 * ==================================================================================== */
static const char *dir_path(restore_t *r, uint32_t id) {
    if (id == 0) return "/";
    if (id <= (uint32_t)r->nd && r->dirs[id - 1].id == id) return r->dirs[id - 1].path;
    for (int i = r->nd - 1; i >= 0; i--) if (r->dirs[i].id == id) return r->dirs[i].path;
    return NULL;
}

static void dir_add(restore_t *r, uint32_t id, const char *path) {
    if (r->nd == r->capd) {
        int nc = r->capd ? r->capd * 2 : 64;
        dmap_t *n = realloc(r->dirs, (size_t)nc * sizeof *n);
        if (!n) return;
        r->dirs = n;
        r->capd = nc;
    }
    r->dirs[r->nd].id = id;
    r->dirs[r->nd].path = path ? strdup(path) : NULL;
    r->nd++;
}

static void stage_flush(restore_t *r) {
    if (!r->slen) return;
    if (r->cur[0]) {
        int fd = vfs_open(r->cur, VFS_O_WRITE);
        int w = -1;
        if (fd >= 0) {
            if (vfs_seek(fd, (int32_t)r->cur_off, VFS_SEEK_SET) >= 0) w = vfs_write(fd, r->stage, r->slen);
            vfs_close(fd);
        }
        if (w != (int)r->slen) {
            klog("osaver: could not write %s (%s)", r->cur, w < 0 ? vfs_strerror(w) : "short write");
            r->cur[0] = 0;
            r->nskipped++;
        }
    }
    r->cur_off += r->slen;
    r->slen = 0;
}

/* create it -> rename it to the exact name -> set the file type -> (write the text) */
static void file_begin(restore_t *r, const osc_rec_t *rec) {
    r->cur[0] = 0;
    r->cur_off = 0;
    r->slen = 0;
    const char *dir = dir_path(r, rec->num[0]);
    const char *name = rec->str[0], *type = rec->str[1];
    char full[VFS_NAME_MAX + 8], final[VFS_PATH_MAX], mid[VFS_PATH_MAX], tmp[VFS_PATH_MAX];
    if (!dir || !osv_name_ok(name) || (type[0] && strchr(type, '/'))) { r->nskipped++; return; }
    if (type[0]) snprintf(full, sizeof full, "%s.%s", name, type);
    else strlcpy(full, name, sizeof full);
    if (!osv_name_ok(full) || strlen(dir) + strlen(full) + 2 >= VFS_PATH_MAX) { r->nskipped++; return; }
    vfs_join(dir, full, final);
    vfs_join(dir, name, mid);
    vfs_join(dir, ".osaver-new", tmp);
    if (!osv_path_saved(final, false) || !vfs_is_dir(dir) || vfs_is_dir(final)) {
        klog("osaver: skipped %s", final);
        r->nskipped++;
        return;
    }
    if (vfs_exists(tmp) && !vfs_is_dir(tmp)) vfs_remove(tmp);
    if (vfs_create(tmp) < 0) { r->nskipped++; return; }                 /* create it              */
    const char *at = tmp;
    if (type[0] && !vfs_exists(mid) && vfs_rename(tmp, mid) == 0) at = mid;   /* rename to the exact name */
    if (vfs_rename(at, final) < 0) {                                    /* set the file type      */
        vfs_remove(at);
        r->nskipped++;
        return;
    }
    strlcpy(r->cur, final, sizeof r->cur);
}

static int cb_rec(void *ctx, const osc_rec_t *rec) {
    restore_t *r = ctx;
    r->cur_wall = false;
    switch (rec->tag) {
    case 'B':
        klog("osaver: code from boot %u (%s), saved at %u: %u files, %u folders", rec->num[0],
             rec->num[2] ? "installed drive" : "ISO", rec->num[1], rec->num[3], rec->num[4]);
        break;
    case 'D': {
        const char *parent = dir_path(r, rec->num[1]);
        char path[VFS_PATH_MAX];
        bool ok = parent && osv_name_ok(rec->str[0]) && strlen(parent) + strlen(rec->str[0]) + 2 < VFS_PATH_MAX;
        if (ok) {
            vfs_join(parent, rec->str[0], path);
            ok = osv_path_saved(path, true) && (vfs_is_dir(path) || vfs_mkdirs(path) == 0);
        }
        dir_add(r, rec->num[0], ok ? path : NULL);
        if (ok) r->ndirs++;
        break;
    }
    case 'F':
        file_begin(r, rec);
        break;
    case 'W':
        r->wall_kind = (int)rec->num[0];
        if (rec->num[0] == 0) {
            r->wall_index = rec->num[1];
        } else {
            strlcpy(r->wall_path, rec->str[0], sizeof r->wall_path);
            r->wall_size = rec->size;
            r->wall_got = 0;
            free(r->wall_data);
            r->wall_data = rec->size && rec->size <= WALL_MAX ? malloc(rec->size) : NULL;
            r->cur_wall = true;
        }
        break;
    }
    return 0;
}

static int cb_data(void *ctx, const uint8_t *p, uint32_t n) {
    restore_t *r = ctx;
    if (r->cur_wall) {
        if (r->wall_data && r->wall_got + n <= r->wall_size) memcpy(r->wall_data + r->wall_got, p, n);
        r->wall_got += n;
        return 0;
    }
    while (n) {
        uint32_t take = MIN(n, STAGE - r->slen);
        memcpy(r->stage + r->slen, p, take);
        r->slen += take;
        p += take;
        n -= take;
        if (r->slen == STAGE) stage_flush(r);
    }
    return 0;
}

static int cb_rec_end(void *ctx, const osc_rec_t *rec) {
    restore_t *r = ctx;
    if (rec->tag == 'F') {
        stage_flush(r);
        if (r->cur[0]) r->nfiles++;
        r->cur[0] = 0;
    }
    r->cur_wall = false;
    return 0;
}

/* ======================================================================================
 * After the files: apply what was restored (a real start would read these files)
 * ==================================================================================== */
extern const initcall_t __initcalls_start[], __initcalls_end[];

/* Run a boot-time init function again by its registered name (Settings applies
   /system/config/settings, the shell reloads pins / recent items / file types). */
static bool rerun_init(const char *name) {
    for (const initcall_t *c = __initcalls_start; c < __initcalls_end; c++) {
        if (c->fn && c->name && !strcmp(c->name, name)) { c->fn(); return true; }
    }
    return false;
}

static int cfg_int(const char *cfg, const char *key, int def) {
    char k[40];
    snprintf(k, sizeof k, "\n%s=", key);
    const char *p = strstr(cfg, k);
    return p ? (int)strtol(p + strlen(k), NULL, 0) : def;
}

static void apply_settings(void) {
    if (rerun_init("settings_boot")) return;
    /* no Settings app: the theme by hand */
    char *raw = vfs_read_file("/system/config/settings", NULL);
    if (!raw) return;
    size_t n = strlen(raw);
    char *cfg = malloc(n + 2);
    if (cfg) {
        cfg[0] = '\n';
        memcpy(cfg + 1, raw, n + 1);
        int dark = cfg_int(cfg, "dark", -1), wp = cfg_int(cfg, "wallpaper", -2);
        if (dark >= 0 && (dark != 0) != theme_get()->dark) theme_set_dark(dark != 0);
        if (strstr(cfg, "\naccent=")) theme_set_accent((uint32_t)cfg_int(cfg, "accent", 0) & 0xFFFFFF);
        if (wp >= 0 && wp < theme_wallpaper_count() && wp != theme_get()->wallpaper) theme_set_wallpaper(wp);
        free(cfg);
    }
    free(raw);
}

static void apply_wallpaper(restore_t *r) {
    if (r->wall_kind == 0) {
        if ((int)r->wall_index < theme_wallpaper_count() && theme_get()->wallpaper != (int)r->wall_index)
            theme_set_wallpaper((int)r->wall_index);
    } else if (r->wall_kind == 1) {
        void *data = r->wall_data;
        uint32_t size = r->wall_data ? r->wall_got : 0;
        bool own = false;
        if (r->wall_path[0] == '/' && osv_path_saved(r->wall_path, false)) {
            if (data && size) {
                char dir[VFS_PATH_MAX];
                vfs_dirname(r->wall_path, dir);
                if (!vfs_is_dir(r->wall_path) && vfs_mkdirs(dir) == 0) vfs_write_file(r->wall_path, data, size);
            } else if (!data) {
                data = vfs_read_file(r->wall_path, &size);       /* restored as a file */
                own = true;
            }
        }
        surface_t *img = data && size ? image_decode(data, size) : NULL;
        if (img) {
            theme_set_wallpaper_image(img);
            surface_destroy(img);
        }
        if (own) free(data);
    }
}

static void post_apply(restore_t *r) {
    apply_settings();
    apply_wallpaper(r);
    rerun_init("shell_late_init");                 /* taskbar pins, recent items */
    rerun_init("assoc_late_init");                 /* file types                 */
    if (service_is_running("screensaver")) service_restart("screensaver");
    klog("osaver: restore done: %u files, %u folders%s%u skipped, in %u ms", r->nfiles, r->ndirs,
         r->nskipped ? ", " : ", ", r->nskipped, sys_uptime_ms() - r->t0);
}

/* Close the apps, as a restart would (not the desktop, not our own screens). */
static void close_apps(void) {
    window_t *list[64];
    int n = win_list(list, 64);
    if (n > 64) n = 64;
    for (int i = n - 1; i >= 0; i--)
        if (win_is_valid(list[i]) && list[i] != (R ? R->win : NULL)) win_close(list[i]);
}

/* ======================================================================================
 * The load screen
 * ==================================================================================== */
static rect_t logo_rect(restore_t *r, int y) {
    int w = win_width(r->win);
    return rect_make((w - r->logo) / 2, y, r->logo, r->logo);
}

static rect_t glow_rect(restore_t *r, int y) {
    rect_t l = logo_rect(r, y);
    return rect_make(l.x + l.w / 2 - r->glow_r, l.y + l.h / 2 - r->glow_r, 2 * r->glow_r, 2 * r->glow_r);
}

static int final_y(restore_t *r) { return (win_height(r->win) - r->logo) / 2; }

static rect_t bar_rect(restore_t *r) {
    int bw = MAX(200, r->logo * 2), bh = 8;
    return rect_make((win_width(r->win) - bw) / 2, final_y(r) + r->logo + r->logo / 2, bw, bh);
}

static surface_t *make_glow(int rad) {
    surface_t *g = surface_create_alpha(2 * rad, 2 * rad);
    if (!g) return NULL;
    for (int y = 0; y < 2 * rad; y++) {
        uint32_t *row = surface_row(g, y);
        for (int x = 0; x < 2 * rad; x++) {
            double dx = x + 0.5 - rad, dy = y + 0.5 - rad;
            double d = sqrt(dx * dx + dy * dy) / rad;
            double f = d >= 1.0 ? 0 : (1.0 - d) * (1.0 - d);
            row[x] = color_alpha(RGB(100, 110, 255), (int)(64 * f));
        }
    }
    return g;
}

static int progress(restore_t *r) {        /* 0..1000: checking 30 %, adding back 70 % */
    uint32_t t = r->total ? r->total : 1;
    switch (r->phase) {
    case P_CHECK: return (int)((uint64_t)r->off * 300 / t);
    case P_CLOSE: return 300;
    case P_APPLY: return 300 + (int)((uint64_t)r->off * 680 / t);
    default:      return 1000;
    }
}

static void screen_paint(restore_t *r, surface_t *s) {
    rect_t all = rect_make(0, 0, s->w, s->h);
    osv_draw_backdrop(s, all);
    uint32_t el = sys_uptime_ms() - r->t0;
    if (r->icon_y < s->h) {
        if (r->glow) gfx_blit_alpha(s, glow_rect(r, r->icon_y).x, glow_rect(r, r->icon_y).y, r->glow, 255);
        ui_draw_logo(s, (s->w - r->logo) / 2, r->icon_y, r->logo);
    }
    if (r->bar_on) osv_draw_bar(s, bar_rect(r), r->shown, 255);
    if (r->from && el < FADE_MS) {
        /* fade in over the screen as it was */
        int a = 255 - (int)(el * 255 / FADE_MS);
        rect_t c = rect_intersect(s->clip, all);
        for (int y = c.y; y < c.y + c.h; y++) {
            uint32_t *d = surface_row(s, y);
            const uint32_t *p = surface_row(r->from, y);
            for (int x = c.x; x < c.x + c.w; x++) {
                uint32_t c0 = d[x], c1 = p[x];
                uint32_t rb = ((c0 & 0xFF00FFu) * (uint32_t)(255 - a) + (c1 & 0xFF00FFu) * (uint32_t)a) >> 8;
                uint32_t g = ((c0 & 0x00FF00u) * (uint32_t)(255 - a) + (c1 & 0x00FF00u) * (uint32_t)a) >> 8;
                d[x] = 0xFF000000u | (rb & 0xFF00FFu) | (g & 0x00FF00u);
            }
        }
    }
}

static void finish(restore_t *r, int result) {
    void (*done)(void *, int) = r->done;
    void *ctx = r->ctx;
    window_t *win = r->win;
    R = NULL;
    free(r->text);
    free(r->piece);
    free(r->stage);
    free(r->wall_data);
    for (int i = 0; i < r->nd; i++) free(r->dirs[i].path);
    free(r->dirs);
    surface_destroy(r->from);
    surface_destroy(r->glow);
    free(r);
    if (win && win_is_valid(win)) {
        win_set_userdata(win, NULL);
        win_close(win);
    }
    if (done) done(ctx, result);
}

static void work(restore_t *r) {
    switch (r->phase) {
    case P_CHECK:
        if (!feed_slice(r)) break;
        if (osc_parser_finish(&r->parser)) {
            klog("osaver: not a valid save code: %s", osc_parser_error(&r->parser));
            r->phase = P_FAILED;
            break;
        }
        r->phase = P_CLOSE;
        break;
    case P_CLOSE: {
        close_apps();
        osc_cb_t cb = { r, NULL, cb_rec, cb_data, cb_rec_end };
        osc_parser_init(&r->parser, &cb);
        r->off = 0;
        r->phase = P_APPLY;
        break;
    }
    case P_APPLY:
        if (!feed_slice(r)) break;
        if (osc_parser_finish(&r->parser)) {        /* cannot happen: it passed the check */
            klog("osaver: code changed while restoring: %s", osc_parser_error(&r->parser));
        }
        stage_flush(r);
        r->phase = P_POST;
        r->work_done_at = sys_uptime_ms();
        break;
    default:
        break;
    }
}

static void screen_event(window_t *win, const event_t *ev) {
    restore_t *r = win_get_userdata(win);
    if (!r) return;
    switch (ev->type) {
    case EV_PAINT:
        screen_paint(r, win_surface(win));
        break;
    case EV_TIMER: {
        uint32_t el = sys_uptime_ms() - r->t0;
        int h = win_height(win);
        /* the icon rises from the bottom into the middle, easing into place */
        int fy = final_y(r), y = fy;
        if (r->animate && el < FADE_MS + RISE_MS) {
            double t = el <= FADE_MS ? 0 : (double)(el - FADE_MS) / RISE_MS;
            double e = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
            y = (int)lround(h + 4 + (fy - (h + 4)) * e);
        }
        if (y != r->icon_y) {
            rect_t a = rect_union(glow_rect(r, r->icon_y), glow_rect(r, y));
            r->icon_y = y;
            win_invalidate_rect(win, a.x, a.y, a.w, a.h);
        }
        if (el < FADE_MS + 20) win_invalidate(win);
        bool risen = !r->animate || el >= FADE_MS + RISE_MS;
        if (risen && !r->bar_on) { r->bar_on = true; rect_t b = bar_rect(r); win_invalidate_rect(win, b.x, b.y, b.w, b.h); }
        /* the work, a slice per tick */
        if (r->phase < P_POST) work(r);
        if (r->phase == P_FAILED) { finish(r, -1); return; }
        /* the bar follows the progress, and takes MIN_MS in all */
        if (r->bar_on) {
            int target = progress(r);
            int timecap = (int)MIN(1000u, el * 1000u / MIN_MS);
            if (target > timecap) target = timecap;
            int v = r->shown + MAX(1, (target - r->shown) / 4);
            if (v > target) v = target;
            if (v != r->shown) {
                r->shown = v;
                rect_t b = bar_rect(r);
                win_invalidate_rect(win, b.x, b.y, b.w, b.h);
            }
            if (r->phase == P_POST && r->shown >= 1000) {
                post_apply(r);
                r->phase = P_END;
                r->work_done_at = sys_uptime_ms();
            }
        }
        if (r->phase == P_END && sys_uptime_ms() - r->work_done_at >= 150) { finish(r, 0); return; }
        break;
    }
    case EV_CLOSE:
        if (R == r) {           /* closed from outside: stop, report failure */
            win_set_userdata(win, NULL);
            r->win = NULL;
            finish(r, -1);
        }
        break;
    default:
        break;
    }
}

int osv_restore_start(const char *text, uint32_t len, const char *path,
                      void (*done)(void *ctx, int result), void *ctx) {
    if (R) return -1;
    restore_t *r = calloc(1, sizeof *r);
    if (!r) return -2;
    r->stage = malloc(STAGE);
    if (text) {
        r->text = malloc(len + 1);
        if (r->text) { memcpy(r->text, text, len); r->text[len] = 0; }
        r->total = len;
    } else {
        vfs_stat_t st;
        strlcpy(r->path, path ? path : "", sizeof r->path);
        r->piece = malloc(PIECE);
        r->total = (vfs_stat(r->path, &st) == 0 && !st.is_dir) ? st.size : 0;
    }
    if (!r->stage || (text && !r->text) || (!text && !r->piece) || r->total > OSAVER_MAX_CODE) {
        free(r->stage); free(r->text); free(r->piece); free(r);
        return -2;
    }
    r->done = done;
    r->ctx = ctx;
    r->wall_kind = -1;
    r->phase = P_CHECK;
    osc_parser_init(&r->parser, NULL);
    r->t0 = sys_uptime_ms();
    char *cfg = vfs_read_file("/system/config/splash.cfg", NULL);
    r->animate = !(cfg && strstr(cfg, "noanimation=1"));
    free(cfg);
    r->from = screen_capture();
    window_t *win = win_create("NovaOS", screen_width(), screen_height(),
                               WIN_FRAMELESS | WIN_NO_TASKBAR | WIN_NO_SHADOW | WIN_CONFIRM_CLOSE);
    if (!win) {
        surface_destroy(r->from);
        free(r->stage); free(r->text); free(r->piece); free(r);
        return -2;
    }
    r->win = win;
    R = r;
    r->logo = CLAMP(screen_height() / 6, 64, 200);
    r->glow_r = r->logo * 7 / 5;
    r->glow = make_glow(r->glow_r);
    r->icon_y = r->animate ? screen_height() + 4 : final_y(r);
    win_set_userdata(win, r);
    win_set_event_handler(win, screen_event);
    win_set_fullscreen(win, true);
    win_set_cursor(win, CURSOR_NONE);
    r->timer = win_set_timer(win, FRAME_MS);
    klog("osaver: soft restart: loading a code of %u characters%s%s", r->total,
         r->text ? "" : " from ", r->text ? "" : r->path);
    return 0;
}

/* ======================================================================================
 * Public API
 * ==================================================================================== */
/* Quick structural look at a big code: the head parses, the rest uses only the
   characters a code may have, and it ends with "))". The restore checks it all. */
static bool quick_check(const char *t, uint32_t n) {
    static osc_parser_t p;
    osc_parser_init(&p, NULL);
    if (osc_parser_feed(&p, t, MIN(n, 65536u))) return false;
    for (uint32_t i = 65536; i < n; i++) {
        char c = t[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' ||
            c == ':' || c == '.' || c == '(' || c == ')' || c == ' ' || c == '\n' || c == '\r' ||
            c == '\t' || c == OSC_HS) continue;
        return false;
    }
    uint32_t e = n;
    while (e && (t[e - 1] == ' ' || t[e - 1] == '\n' || t[e - 1] == '\r' || t[e - 1] == '\t')) e--;
    return e >= 2 && t[e - 1] == ')' && t[e - 2] == ')';
}

bool osaver_check_code(const char *text, uint32_t len) {
    if (!text || !len || len > OSAVER_MAX_CODE) return false;
    if (len <= (4u << 20)) return osc_validate(text, len, NULL, 0);
    return quick_check(text, len);
}

typedef struct { void (*done)(void *); void *ctx; } file_done_t;

static void file_done(void *ctx, int result) {
    file_done_t *f = ctx;
    (void)result;
    if (f->done) f->done(f->ctx);
    free(f);
}

int osaver_restore_file(const char *path, void (*done)(void *ctx), void *ctx) {
    vfs_stat_t st;
    if (!path || vfs_stat(path, &st) < 0 || st.is_dir || !st.size || st.size > OSAVER_MAX_CODE) return -1;
    uint32_t n = MIN(st.size, 65536u);
    char *head = malloc(n);
    if (!head) return -2;
    int fd = vfs_open(path, VFS_O_READ);
    int got = fd >= 0 ? vfs_read(fd, head, n) : -1;
    if (fd >= 0) vfs_close(fd);
    static osc_parser_t p;
    osc_parser_init(&p, NULL);
    bool ok = got == (int)n && osc_parser_feed(&p, head, n) == 0;
    free(head);
    if (!ok) return -1;
    file_done_t *f = calloc(1, sizeof *f);
    if (!f) return -2;
    f->done = done;
    f->ctx = ctx;
    int r = osv_restore_start(NULL, 0, path, file_done, f);
    if (r) free(f);
    return r;
}
