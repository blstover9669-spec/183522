/*
 * graphics_viewer.c - "Image Viewer": views BMP images (1/4/8/16/24/32-bit
 * uncompressed, with alpha) and PPM files from the VFS.
 *
 * Features: fit to window / actual size / zoom 2%..3200% around the pointer,
 * drag to pan, previous / next image in the same folder (the list follows
 * changes to the folder), rotate, full screen and slideshow, set as
 * wallpaper, edit in Paint, checkerboard behind transparent images.
 * launch(arg) opens the given file (or the first image of a folder).
 *
 * Down-scaled views are first drawn with a quick bilinear blit and then
 * replaced by a high-quality area-averaged copy that is built a few rows per
 * timer tick, so zooming large pictures never stalls the system.
 *
 * At the first launch after boot, two procedural sample pictures are created
 * in /home/user/Pictures if they are missing (a 24-bit plasma and a 32-bit
 * picture with transparency), also a few rows per timer tick.
 *
 * All state lives in a per-window viewer_t, freed on EV_CLOSE.
 */
#include <nova.h>
#include "graphics_bmp.h"

#define IV_W        800
#define IV_H        580
#define TB_H        44
#define ST_H        24
#define BTN_W       34
#define BTN_H       32
#define MIN_ZOOM    0.02
#define MAX_ZOOM    32.0
#define WORK_TICK   5
#define WORK_BUDGET 10          /* ms of background work per tick       */
#define HQ_MAX_PX   (6u << 20)  /* largest cached down-scaled view       */
#define SLIDE_MS    3500
#define MSG_MS      4000
#define PICTURES    "/home/user/Pictures"
#define MAX_FILES   512

enum { B_OPEN, B_PREV, B_NEXT, B_ZOOM_OUT, B_ZOOM_IN, B_FIT, B_ACTUAL, B_ROT_L, B_ROT_R,
       B_SLIDE, B_WALL, B_EDIT, B_COUNT };
static const char *const g_btn_tips[B_COUNT] = {
    "Open a picture (Ctrl+O)", "Previous picture (PgUp, Left)", "Next picture (PgDn, Right, Space)",
    "Zoom out (-)", "Zoom in (+)", "Fit to window (0)", "Actual size (1)",
    "Rotate left (L)", "Rotate right (R)", "Slideshow (F5)", "Set as desktop wallpaper (W)",
    "Edit in Paint (E)",
};

typedef struct viewer {
    window_t *win;
    char path[VFS_PATH_MAX];          /* current file, "" = none        */
    char dir[VFS_PATH_MAX];
    char (*names)[VFS_NAME_MAX];      /* images in dir, sorted          */
    int nnames, cur;
    uint32_t list_gen;

    surface_t *img;
    gbmp_info_t info;
    uint32_t fsize;
    char err[200];
    int rot;                          /* quarter turns applied (view only) */

    double scale, offx, offy;         /* image top-left in stage coords */
    bool fit;

    /* high-quality down-scaled copy */
    surface_t *hq;
    int *hq_xmap;
    int hq_row;
    bool hq_done;

    int work_timer;

    /* sample generation (first launch) */
    int gen_idx, gen_row;
    surface_t *gen;

    /* ui */
    int hover, pressed;
    bool dragging;
    int drag_mx, drag_my;
    double drag_ox, drag_oy;
    int mx, my;
    bool fs, slideshow;
    int slide_timer;
    char msg[160];
    uint32_t msg_until;
    int msg_timer;
    uint32_t caption_until;           /* full screen: name + position overlay */
    bool msg_in_caption;              /* the message was raised in full screen */
} viewer_t;

static bool g_samples_checked;        /* first launch since boot handled */
static bool g_gen_running;            /* one window generates the samples */

static void iv_fit(viewer_t *v);
static void iv_open(viewer_t *v, const char *path);
static void work_ensure(viewer_t *v);

/* ======================================================================================
 * layout helpers
 * ====================================================================================== */
static int tb_h(const viewer_t *v) { return v->fs ? 0 : TB_H; }
static int st_h(const viewer_t *v) { return v->fs ? 0 : ST_H; }
static rect_t stage_rect(const viewer_t *v) {
    return rect_make(0, tb_h(v), win_width(v->win), win_height(v->win) - tb_h(v) - st_h(v));
}
static void stage_dirty(viewer_t *v) { rect_t r = stage_rect(v); win_invalidate_rect(v->win, r.x, r.y, r.w, r.h); }
static void status_dirty(viewer_t *v) {
    if (!v->fs) win_invalidate_rect(v->win, 0, win_height(v->win) - ST_H, win_width(v->win), ST_H);
}
static void toolbar_dirty(viewer_t *v) { if (!v->fs) win_invalidate_rect(v->win, 0, 0, win_width(v->win), TB_H); }

static void msg_timer_ensure(viewer_t *v) { if (!v->msg_timer) v->msg_timer = win_set_timer(v->win, 250); }

/* In full screen, show the picture's name and position for a moment. */
static void show_caption(viewer_t *v) {
    v->msg_in_caption = false;
    if (!v->fs || !v->img) return;
    v->caption_until = sys_uptime_ms() + 2500;
    msg_timer_ensure(v);
    stage_dirty(v);
}

static void iv_message(viewer_t *v, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void iv_message(viewer_t *v, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v->msg, sizeof v->msg, fmt, ap);
    va_end(ap);
    v->msg_until = sys_uptime_ms() + MSG_MS;
    msg_timer_ensure(v);
    status_dirty(v);
    show_caption(v);
    v->msg_in_caption = v->fs;
}

/* toolbar geometry: groups separated by gaps */
static rect_t btn_rect(const viewer_t *v, int b) {
    static const int gap_before[B_COUNT] = { 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0 };
    int W = win_width(v->win);
    if (b >= B_SLIDE) {                    /* right-aligned group */
        int x = W - 10 - (B_COUNT - b) * (BTN_W + 4) + 4;
        return rect_make(x, 6, BTN_W, BTN_H);
    }
    int x = 10;
    for (int i = 0; i < b; i++) x += BTN_W + 4 + (gap_before[i + 1] ? 13 : 0);
    return rect_make(x, 6, BTN_W, BTN_H);
}

static bool btn_enabled(const viewer_t *v, int b) {
    switch (b) {
    case B_OPEN: return true;
    case B_PREV: case B_NEXT: case B_SLIDE: return v->nnames > 1 || (b == B_SLIDE && v->img);
    case B_EDIT: return v->img && v->path[0] && app_find("Paint");
    default: return v->img != NULL;
    }
}

static int btn_hit(const viewer_t *v, int x, int y) {
    if (v->fs) return -1;
    for (int b = 0; b < B_COUNT; b++) if (rect_contains(btn_rect(v, b), x, y)) return b;
    return -1;
}

/* ======================================================================================
 * folder list
 * ====================================================================================== */
static void list_free(viewer_t *v) {
    free(v->names);
    v->names = NULL;
    v->nnames = 0;
    v->cur = -1;
}

/* (Re)read the images of v->dir and locate the current file. */
static void list_load(viewer_t *v) {
    list_free(v);
    v->list_gen = vfs_generation();
    int n = vfs_list(v->dir, NULL, 0);
    if (n <= 0) return;
    if (n > 4096) n = 4096;
    vfs_dirent_t *ents = malloc((size_t)n * sizeof *ents);
    if (!ents) return;
    n = MIN(vfs_list(v->dir, ents, n), n);
    v->names = malloc((size_t)MIN(n, MAX_FILES) * sizeof *v->names + 1);
    if (v->names) {
        for (int i = 0; i < n && v->nnames < MAX_FILES; i++)
            if (!ents[i].is_dir && gbmp_is_image_name(ents[i].name))
                strlcpy(v->names[v->nnames++], ents[i].name, VFS_NAME_MAX);
    }
    free(ents);
    const char *base = v->path[0] ? vfs_basename(v->path) : "";
    for (int i = 0; i < v->nnames; i++) if (!strcmp(v->names[i], base)) v->cur = i;
}

static void list_refresh_if_changed(viewer_t *v) {
    if (v->dir[0] && vfs_generation() != v->list_gen) list_load(v);
}

static void iv_step(viewer_t *v, int d) {
    list_refresh_if_changed(v);
    if (v->nnames <= 0) return;
    int i;
    if (v->cur < 0) i = d > 0 ? 0 : v->nnames - 1;
    else i = ((v->cur + d) % v->nnames + v->nnames) % v->nnames;
    if (i == v->cur && v->img) return;
    char p[VFS_PATH_MAX];
    vfs_join(v->dir, v->names[i], p);
    iv_open(v, p);
}

static void iv_first_last(viewer_t *v, bool last) {
    list_refresh_if_changed(v);
    if (v->nnames <= 0) return;
    char p[VFS_PATH_MAX];
    vfs_join(v->dir, v->names[last ? v->nnames - 1 : 0], p);
    iv_open(v, p);
}

/* ======================================================================================
 * zoom / pan
 * ====================================================================================== */
static void hq_reset(viewer_t *v) {
    surface_destroy(v->hq);
    v->hq = NULL;
    free(v->hq_xmap);
    v->hq_xmap = NULL;
    v->hq_done = false;
    v->hq_row = 0;
}

static void disp_size(const viewer_t *v, int *dw, int *dh) {
    *dw = MAX(1, (int)(v->img->w * v->scale + 0.5));
    *dh = MAX(1, (int)(v->img->h * v->scale + 0.5));
}

/* Keep the image inside the stage: centred when smaller, no gaps when larger. */
static void clamp_pan(viewer_t *v) {
    if (!v->img) return;
    rect_t st = stage_rect(v);
    int dw, dh;
    disp_size(v, &dw, &dh);
    if (dw <= st.w) v->offx = (st.w - dw) / 2;
    else v->offx = CLAMP(v->offx, (double)(st.w - dw), 0.0);
    if (dh <= st.h) v->offy = (st.h - dh) / 2;
    else v->offy = CLAMP(v->offy, (double)(st.h - dh), 0.0);
    v->offx = floor(v->offx);
    v->offy = floor(v->offy);
}

/* Start building the high-quality copy if the view is down-scaled. */
static void hq_start(viewer_t *v) {
    hq_reset(v);
    if (!v->img || v->scale >= 0.999) return;
    int dw, dh;
    disp_size(v, &dw, &dh);
    if ((uint32_t)dw * (uint32_t)dh > HQ_MAX_PX) return;
    v->hq = (v->img->flags & SURF_ALPHA) ? surface_create_alpha(dw, dh) : surface_create(dw, dh);
    v->hq_xmap = malloc((size_t)(dw + 1) * sizeof *v->hq_xmap);
    if (!v->hq || !v->hq_xmap) { hq_reset(v); return; }
    for (int x = 0; x <= dw; x++) v->hq_xmap[x] = (int)((int64_t)x * v->img->w / dw);
    work_ensure(v);
}

static void set_scale(viewer_t *v, double s, double ax, double ay) {
    if (!v->img) return;
    s = CLAMP(s, MIN_ZOOM, MAX_ZOOM);
    rect_t st = stage_rect(v);
    /* image point under the anchor stays put */
    double ix = (ax - st.x - v->offx) / v->scale, iy = (ay - st.y - v->offy) / v->scale;
    v->scale = s;
    v->fit = false;
    v->offx = ax - st.x - ix * s;
    v->offy = ay - st.y - iy * s;
    clamp_pan(v);
    hq_start(v);
    stage_dirty(v);
    status_dirty(v);
    toolbar_dirty(v);
}

static double fit_scale(const viewer_t *v) {
    rect_t st = stage_rect(v);
    if (!v->img || st.w <= 0 || st.h <= 0) return 1.0;
    double sx = (double)(st.w - (v->fs ? 0 : 24)) / v->img->w, sy = (double)(st.h - (v->fs ? 0 : 24)) / v->img->h;
    return CLAMP(fmin(sx, sy), MIN_ZOOM, MAX_ZOOM);
}

static void iv_fit(viewer_t *v) {
    if (!v->img) return;
    double s = fit_scale(v);
    bool changed = fabs(s - v->scale) > 1e-9 || !v->fit;
    v->scale = s;
    v->fit = true;
    clamp_pan(v);
    if (changed || (!v->hq && s < 0.999)) hq_start(v);
    stage_dirty(v);
    status_dirty(v);
    toolbar_dirty(v);
}

static void zoom_step(viewer_t *v, int dir, double ax, double ay) {
    if (!v->img) return;
    static const double steps[] = { 0.02, 0.03, 0.05, 0.0667, 0.1, 0.125, 0.167, 0.25, 0.333, 0.5, 0.667, 0.75,
                                    1, 1.5, 2, 3, 4, 6, 8, 12, 16, 24, 32 };
    int n = ARRAY_SIZE(steps);
    double s = v->scale;
    double t = s;
    if (dir > 0) { for (int i = 0; i < n; i++) if (steps[i] > s * 1.001) { t = steps[i]; break; } }
    else { for (int i = n - 1; i >= 0; i--) if (steps[i] < s * 0.999) { t = steps[i]; break; } }
    set_scale(v, t, ax, ay);
}

static void zoom_center(viewer_t *v, int dir) {
    rect_t st = stage_rect(v);
    zoom_step(v, dir, st.x + st.w / 2.0, st.y + st.h / 2.0);
}

/* ======================================================================================
 * loading
 * ====================================================================================== */
static void iv_update_title(viewer_t *v) {
    char t[VFS_NAME_MAX + 32];
    if (v->path[0]) snprintf(t, sizeof t, "%s - Image Viewer", vfs_basename(v->path));
    else snprintf(t, sizeof t, "Image Viewer");
    win_set_title(v->win, t);
}

static void iv_open(viewer_t *v, const char *path) {
    char full[VFS_PATH_MAX];
    if (vfs_normalize(NULL, path, full) < 0) strlcpy(full, path, sizeof full);
    if (vfs_is_dir(full)) {
        strlcpy(v->dir, full, sizeof v->dir);
        v->path[0] = 0;
        list_load(v);
        if (v->nnames > 0) { iv_step(v, 1); return; }
        surface_destroy(v->img);
        v->img = NULL;
        snprintf(v->err, sizeof v->err, "There are no pictures in %s", full);
        hq_reset(v);
        iv_update_title(v);
        win_invalidate(v->win);
        return;
    }
    gbmp_info_t info;
    const char *err = NULL;
    uint32_t fsize = 0;
    surface_t *img = gbmp_load(full, &info, &fsize, &err);
    strlcpy(v->path, full, sizeof v->path);
    char d[VFS_PATH_MAX];
    vfs_dirname(full, d);
    if (strcmp(d, v->dir) || vfs_generation() != v->list_gen) { strlcpy(v->dir, d, sizeof v->dir); list_load(v); }
    else {
        v->cur = -1;
        for (int i = 0; i < v->nnames; i++) if (!strcmp(v->names[i], vfs_basename(full))) v->cur = i;
    }
    surface_destroy(v->img);
    v->img = img;
    v->rot = 0;
    hq_reset(v);
    if (!img) {
        snprintf(v->err, sizeof v->err, "Cannot show \"%s\": %s.", vfs_basename(full), err ? err : "unknown error");
        klog("viewer: cannot open %s: %s", full, err ? err : "?");
    } else {
        v->err[0] = 0;
        v->info = info;
        v->fsize = fsize;
        v->scale = 1;
        iv_fit(v);
        show_caption(v);
        klog("viewer: opened %s (%dx%d, %s)", full, info.width, info.height, info.format);
    }
    iv_update_title(v);
    win_invalidate(v->win);
}

static void open_cb(const char *path, void *ctx) {
    viewer_t *v = ctx;
    if (path) iv_open(v, path);
}

static void iv_open_dialog(viewer_t *v) {
    ui_file_dialog(v->win, UI_FILE_OPEN, "Open picture", v->dir[0] ? v->dir : PICTURES, "bmp,dib,ppm", open_cb, v);
}

/* ======================================================================================
 * image actions
 * ====================================================================================== */
static void iv_rotate(viewer_t *v, bool right) {
    if (!v->img) return;
    surface_t *c = v->img;
    surface_t *n = (c->flags & SURF_ALPHA) ? surface_create_alpha(c->h, c->w) : surface_create(c->h, c->w);
    if (!n) { iv_message(v, "Not enough memory to rotate this picture"); return; }
    for (int y = 0; y < c->h; y++) {
        const uint32_t *row = surface_row(c, y);
        for (int x = 0; x < c->w; x++) {
            int nx = right ? c->h - 1 - y : y, ny = right ? x : c->w - 1 - x;
            surface_row(n, ny)[nx] = row[x];
        }
    }
    surface_destroy(c);
    v->img = n;
    v->rot = (v->rot + (right ? 1 : 3)) & 3;
    hq_reset(v);
    if (v->fit) { v->fit = false; iv_fit(v); }
    else { clamp_pan(v); hq_start(v); stage_dirty(v); status_dirty(v); }
}

static void iv_wallpaper(viewer_t *v) {
    if (!v->img) return;
    theme_set_wallpaper_image(v->img);
    iv_message(v, "\"%s\" is now the desktop wallpaper", v->path[0] ? vfs_basename(v->path) : "Picture");
}

static void iv_edit(viewer_t *v) {
    if (!v->img || !v->path[0]) return;
    if (!app_launch_by_name("Paint", v->path)) iv_message(v, "Paint is not installed");
}

static void iv_set_fullscreen(viewer_t *v, bool on) {
    if (on == win_is_fullscreen(v->win)) return;
    win_set_fullscreen(v->win, on);
}

static void iv_slideshow(viewer_t *v, bool on) {
    if (on && !v->img) return;
    v->slideshow = on;
    if (on) {
        if (!v->slide_timer) v->slide_timer = win_set_timer(v->win, SLIDE_MS);
        if (!win_is_fullscreen(v->win)) v->fit = true;      /* refitted on the resize */
        else iv_fit(v);
        iv_set_fullscreen(v, true);
    } else {
        if (v->slide_timer) { win_kill_timer(v->win, v->slide_timer); v->slide_timer = 0; }
        iv_set_fullscreen(v, false);
    }
    toolbar_dirty(v);
}

/* ======================================================================================
 * background work: high-quality scaling and sample pictures
 * ====================================================================================== */
/* Area-averaging (box filter) of output rows [y0, y1). */
static void hq_rows(viewer_t *v, int y0, int y1) {
    const surface_t *src = v->img;
    surface_t *dst = v->hq;
    bool alpha = src->flags & SURF_ALPHA;
    for (int oy = y0; oy < y1; oy++) {
        int sy0 = (int)((int64_t)oy * src->h / dst->h), sy1 = (int)((int64_t)(oy + 1) * src->h / dst->h);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > src->h) sy1 = src->h;
        uint32_t *out = surface_row(dst, oy);
        for (int ox = 0; ox < dst->w; ox++) {
            int sx0 = v->hq_xmap[ox], sx1 = v->hq_xmap[ox + 1];
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > src->w) sx1 = src->w;
            uint32_t sa = 0, sr = 0, sg = 0, sb = 0, n = 0;
            for (int y = sy0; y < sy1; y++) {
                const uint32_t *p = surface_row(src, y);
                for (int x = sx0; x < sx1; x++) {
                    uint32_t c = p[x];
                    if (alpha) {
                        uint32_t a = COLOR_A(c);
                        sa += a; sr += COLOR_R(c) * a; sg += COLOR_G(c) * a; sb += COLOR_B(c) * a;
                    } else {
                        sr += COLOR_R(c); sg += COLOR_G(c); sb += COLOR_B(c);
                    }
                    n++;
                }
            }
            if (!n) { out[ox] = 0; continue; }
            if (alpha) out[ox] = sa ? RGBA(sr / sa, sg / sa, sb / sa, sa / n) : 0;
            else out[ox] = RGB(sr / n, sg / n, sb / n);
        }
    }
}

/* ---- procedural sample pictures ---- */
#define SAMPLE_W 480
#define SAMPLE_H 320
static const char *const g_sample_names[] = { "Plasma.bmp", "Glass orbs.bmp" };
#define NSAMPLES ARRAY_SIZE(g_sample_names)

static int16_t g_sin[256];
static bool g_sin_ready;
static void sin_init(void) {
    if (g_sin_ready) return;
    for (int i = 0; i < 256; i++) g_sin[i] = (int16_t)lround(sin(i * 2 * M_PI / 256) * 127);
    g_sin_ready = true;
}
#define S8(i) (g_sin[(unsigned)(i) & 255])

static void plasma_rows(surface_t *s, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        uint32_t *row = surface_row(s, y);
        for (int x = 0; x < s->w; x++) {
            int dx = x - s->w * 2 / 3, dy = y - s->h / 3;
            int d = (int)sqrt((double)(dx * dx + dy * dy));
            int val = S8(x * 2) + S8(y * 3 + 40) + S8((x + y) * 3 / 2) + S8(d * 4);   /* -508..508 */
            int idx = (val + 512) / 4;
            int r = 128 + S8(idx), g = 128 + S8(idx + 85), b = 128 + S8(idx + 170);
            /* soft vignette */
            int ex = x - s->w / 2, ey = y - s->h / 2;
            int vig = 255 - (ex * ex + ey * ey) * 90 / (s->w * s->w / 4 + s->h * s->h / 4);
            row[x] = RGB(r * vig / 255, g * vig / 255, b * vig / 255);
        }
    }
}

static void orbs_rows(surface_t *s, int y0, int y1) {
    static const struct { int x, y, r; uint32_t c; } orbs[] = {
        { 120, 200, 96, RGB(40, 120, 240) },  { 250, 120, 80, RGB(240, 60, 90) },
        { 360, 210, 88, RGB(60, 200, 120) },  { 300, 250, 46, RGB(255, 200, 40) },
        { 420, 80, 50, RGB(170, 90, 240) },   { 60, 70, 40, RGB(255, 140, 40) },
    };
    const double lx = -0.45, ly = -0.55, lz = 0.70;     /* light direction (normalised) */
    for (int y = y0; y < y1; y++) {
        uint32_t *row = surface_row(s, y);
        for (int x = 0; x < s->w; x++) {
            double ca = 0, cr = 0, cg = 0, cb = 0;   /* premultiplied accumulation, back to front */
            for (int i = 0; i < ARRAY_SIZE(orbs); i++) {
                double dx = x + 0.5 - orbs[i].x, dy = y + 0.5 - orbs[i].y, r = orbs[i].r;
                double d2 = dx * dx + dy * dy;
                if (d2 >= r * r) continue;
                double d = sqrt(d2);
                double nx = dx / r, ny = dy / r, nz = sqrt(fmax(0.0, 1 - d2 / (r * r)));
                double diff = fmax(0.0, nx * lx + ny * ly + nz * lz);
                double spec = pow(diff, 28) * 0.9;
                double shade = 0.30 + 0.75 * diff;
                double rr = fmin(255, COLOR_R(orbs[i].c) * shade + 255 * spec);
                double gg = fmin(255, COLOR_G(orbs[i].c) * shade + 255 * spec);
                double bb = fmin(255, COLOR_B(orbs[i].c) * shade + 255 * spec);
                double a = (0.55 + 0.4 * (1 - nz)) + spec * 0.5;     /* glassy: denser at the rim */
                if (a > 1) a = 1;
                if (r - d < 1.0) a *= (r - d);                       /* anti-aliased edge */
                ca = a + ca * (1 - a);
                cr = rr * a + cr * (1 - a);
                cg = gg * a + cg * (1 - a);
                cb = bb * a + cb * (1 - a);
            }
            if (ca <= 0.002) { row[x] = 0; continue; }
            row[x] = RGBA((int)(cr / ca), (int)(cg / ca), (int)(cb / ca), (int)(ca * 255 + 0.5));
        }
    }
}

static int next_missing_sample(int from) {
    for (int i = from; i < (int)NSAMPLES; i++) {
        char p[VFS_PATH_MAX];
        vfs_join(PICTURES, g_sample_names[i], p);
        if (!vfs_exists(p)) return i;
    }
    return -1;
}

static void gen_finish(viewer_t *v) {
    surface_destroy(v->gen);
    v->gen = NULL;
    v->gen_idx = -1;
    g_gen_running = false;
    g_samples_checked = true;
    if (!strcmp(v->dir, PICTURES) || !v->dir[0]) {
        if (!v->dir[0]) strlcpy(v->dir, PICTURES, sizeof v->dir);
        list_load(v);
        if (!v->img && !v->err[0]) iv_step(v, 1);
        toolbar_dirty(v);
        status_dirty(v);
        if (!v->img) win_invalidate(v->win);
    }
}

/* One slice of sample generation; returns false when finished. */
static bool gen_slice(viewer_t *v, uint32_t t0) {
    if (v->gen_idx < 0) return false;
    if (!v->gen) {
        sin_init();
        v->gen = v->gen_idx == 1 ? surface_create_alpha(SAMPLE_W, SAMPLE_H) : surface_create(SAMPLE_W, SAMPLE_H);
        v->gen_row = 0;
        if (!v->gen) { gen_finish(v); return false; }
    }
    while (v->gen_row < SAMPLE_H && sys_uptime_ms() - t0 < WORK_BUDGET) {
        int n = v->gen_idx == 1 ? 4 : 16;
        int y1 = MIN(SAMPLE_H, v->gen_row + n);
        if (v->gen_idx == 1) orbs_rows(v->gen, v->gen_row, y1);
        else plasma_rows(v->gen, v->gen_row, y1);
        v->gen_row = y1;
    }
    if (v->gen_row < SAMPLE_H) return true;
    char p[VFS_PATH_MAX];
    vfs_mkdirs(PICTURES);
    vfs_join(PICTURES, g_sample_names[v->gen_idx], p);
    int err = gbmp_save(p, v->gen, v->gen_idx == 1);
    klog("viewer: created sample picture %s (%s)", p, err < 0 ? vfs_strerror(err) : "ok");
    surface_destroy(v->gen);
    v->gen = NULL;
    v->gen_idx = next_missing_sample(v->gen_idx + 1);
    if (v->gen_idx < 0) { gen_finish(v); return false; }
    return true;
}

static void work_ensure(viewer_t *v) {
    if (!v->work_timer) v->work_timer = win_set_timer(v->win, WORK_TICK);
}

static void work_tick(viewer_t *v) {
    uint32_t t0 = sys_uptime_ms();
    bool more = false;
    if (v->hq && !v->hq_done) {
        int start = v->hq_row;
        while (v->hq_row < v->hq->h && sys_uptime_ms() - t0 < WORK_BUDGET) {
            int y1 = MIN(v->hq->h, v->hq_row + 4);
            hq_rows(v, v->hq_row, y1);
            v->hq_row = y1;
        }
        if (v->hq_row >= v->hq->h) {
            v->hq_done = true;
            if (start < v->hq->h) stage_dirty(v);
        } else {
            more = true;
        }
    }
    if (v->gen_idx >= 0 && sys_uptime_ms() - t0 < WORK_BUDGET) more |= gen_slice(v, t0);
    else if (v->gen_idx >= 0) more = true;
    if (!more && v->work_timer) { win_kill_timer(v->win, v->work_timer); v->work_timer = 0; }
}

/* ======================================================================================
 * painting
 * ====================================================================================== */
static void draw_checker(surface_t *s, rect_t r, rect_t clip) {
    rect_t c = rect_intersect(r, clip);
    if (rect_empty(c)) return;
    const int q = 8;
    for (int y = c.y - ((c.y - r.y) % q); y < c.y + c.h; y += q)
        for (int x = c.x - ((c.x - r.x) % q); x < c.x + c.w; x += q) {
            bool dark = (((x - r.x) / q) + ((y - r.y) / q)) & 1;
            gfx_fill_rect(s, x, y, q, q, dark ? RGB(200, 200, 204) : RGB(244, 244, 246));
        }
}

/* small vector glyphs for the toolbar, drawn in a 20x20 box at (x,y) */
static void glyph_magnifier(surface_t *s, int x, int y, uint32_t c, int sign) {
    gfx_circle_aa(s, x + 8.5, y + 8.5, 6.5, 1.8, c);
    gfx_thick_line(s, x + 13, y + 13, x + 18, y + 18, 3, c);
    gfx_fill_rect(s, x + 5, y + 8, 7, 2, c);
    if (sign > 0) gfx_fill_rect(s, x + 7, y + 6, 2, 6, c);
}

static void glyph_rotate(surface_t *s, int x, int y, uint32_t c, bool clockwise) {
    bool right = !clockwise;      /* the arc below is drawn mirrored for anticlockwise */
    double cx = x + 10, cy = y + 11, r = 6.5;
    for (int a = 40; a <= 300; a += 5) {
        double t = (right ? -a + 180 : a) * M_PI / 180.0;
        gfx_fill_circle_aa(s, cx + r * cos(t), cy - r * sin(t), 1.0, c);
    }
    double t = (right ? -40 + 180 : 40) * M_PI / 180.0;
    double ex = cx + r * cos(t), ey = cy - r * sin(t);
    int dir = right ? -1 : 1;
    gfx_fill_triangle(s, (int)(ex - 4 * dir), (int)ey - 5, (int)(ex + 4 * dir), (int)ey - 2, (int)(ex - 1 * dir), (int)ey + 3, c);
}

static void draw_btn_glyph(surface_t *s, int b, rect_t r, uint32_t c) {
    int x = r.x + (r.w - 20) / 2, y = r.y + (r.h - 20) / 2;
    switch (b) {
    case B_OPEN: ui_draw_glyph(s, UI_GLYPH_FOLDER, x, y, 20, c); break;
    case B_PREV: ui_draw_glyph(s, UI_GLYPH_ARROW_LEFT, x + 2, y + 2, 16, c); break;
    case B_NEXT: ui_draw_glyph(s, UI_GLYPH_ARROW_RIGHT, x + 2, y + 2, 16, c); break;
    case B_ZOOM_OUT: glyph_magnifier(s, x, y, c, -1); break;
    case B_ZOOM_IN: glyph_magnifier(s, x, y, c, 1); break;
    case B_FIT:
        for (int k = 0; k < 4; k++) {
            int cx = (k & 1) ? x + 17 : x + 2, cy = (k & 2) ? y + 17 : y + 2;
            int sx = (k & 1) ? -1 : 1, sy = (k & 2) ? -1 : 1;
            gfx_fill_rect(s, sx > 0 ? cx : cx - 5, cy - (sy > 0 ? 0 : 1), 6, 2, c);
            gfx_fill_rect(s, cx - (sx > 0 ? 0 : 1), sy > 0 ? cy : cy - 5, 2, 6, c);
        }
        gfx_rect(s, x + 6, y + 6, 8, 8, c);
        break;
    case B_ACTUAL:
        gfx_text_aligned(s, rect_make(x - 4, y, 28, 20), "1:1", c, FONT_UI_BOLD, ALIGN_CENTER | ALIGN_MIDDLE);
        break;
    case B_ROT_L: glyph_rotate(s, x, y, c, false); break;   /* arrow head top left: anticlockwise */
    case B_ROT_R: glyph_rotate(s, x, y, c, true); break;    /* arrow head top right: clockwise    */
    case B_SLIDE:
        gfx_rounded_rect(s, x + 1, y + 3, 18, 14, 3, c);
        gfx_fill_triangle(s, x + 8, y + 6, x + 8, y + 14, x + 14, y + 10, c);
        break;
    case B_WALL:
        gfx_rounded_rect(s, x + 1, y + 2, 18, 13, 2, c);
        gfx_fill_triangle(s, x + 4, y + 13, x + 9, y + 7, x + 13, y + 13, c);
        gfx_fill_triangle(s, x + 10, y + 13, x + 13, y + 9, x + 16, y + 13, c);
        gfx_fill_rect(s, x + 8, y + 15, 4, 2, c);
        gfx_fill_rect(s, x + 5, y + 17, 10, 2, c);
        break;
    case B_EDIT:
        gfx_thick_line(s, x + 5, y + 15, x + 15, y + 5, 4, c);
        gfx_fill_triangle(s, x + 2, y + 18, x + 3, y + 13, x + 7, y + 17, c);
        break;
    }
}

static void draw_toolbar(viewer_t *v, surface_t *s) {
    const theme_t *t = theme_get();
    int W = s->w;
    uint32_t line = color_mix(t->window_bg, t->window_border, 90);
    gfx_hline(s, 0, TB_H - 1, W, line);
    for (int b = 0; b < B_COUNT; b++) {
        rect_t r = btn_rect(v, b);
        bool en = btn_enabled(v, b);
        bool active = b == B_SLIDE && v->slideshow;
        if (en && (b == v->pressed && b == v->hover))
            gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, color_mix(t->window_bg, t->window_text, 46));
        else if (en && b == v->hover)
            gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, color_mix(t->window_bg, t->window_text, 24));
        else if (active || (b == B_FIT && v->fit && v->img))
            gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, color_mix(t->window_bg, t->accent, 50));
        draw_btn_glyph(s, b, r, en ? t->window_text : t->disabled_text);
    }
    /* separators between the groups */
    int seps[] = { B_PREV, B_ZOOM_OUT, B_ROT_L };
    for (int i = 0; i < 3; i++) {
        rect_t r = btn_rect(v, seps[i]);
        gfx_vline(s, r.x - 8, 12, TB_H - 24, line);
    }
    /* file name between the groups */
    int x0 = btn_rect(v, B_ROT_R).x + BTN_W + 16, x1 = btn_rect(v, B_SLIDE).x - 16;
    if (x1 - x0 > 40 && v->path[0]) {
        const char *name = vfs_basename(v->path);
        int tw = font_text_width(FONT_UI_BOLD, name);
        int tx = MAX(x0, x0 + (x1 - x0 - tw) / 2);
        gfx_text_ellipsis(s, tx, (TB_H - font_height(FONT_UI_BOLD)) / 2, x1 - tx, name, t->window_text, FONT_UI_BOLD);
    }
}

static void draw_status(viewer_t *v, surface_t *s) {
    const theme_t *t = theme_get();
    int W = s->w, sy = s->h - ST_H;
    uint32_t line = color_mix(t->window_bg, t->window_border, 90);
    uint32_t muted = color_mix(t->window_text, t->window_bg, 90);
    gfx_fill_rect(s, 0, sy, W, ST_H, t->dark ? color_shade(t->window_bg, -10) : color_shade(t->window_bg, -6));
    gfx_hline(s, 0, sy, W, line);
    int ty = sy + 1 + (ST_H - 1 - font_height(FONT_UI)) / 2;
    char right[64] = "", left[200];
    if (v->img) {
        if (v->nnames > 0 && v->cur >= 0) snprintf(right, sizeof right, "%d / %d      %d%%", v->cur + 1, v->nnames, (int)(v->scale * 100 + 0.5));
        else snprintf(right, sizeof right, "%d%%", (int)(v->scale * 100 + 0.5));
    }
    int rw = font_text_width(FONT_UI, right);
    gfx_text(s, W - 12 - rw, ty, right, muted, FONT_UI);
    int maxw = W - 36 - rw;
    if (v->msg[0]) { gfx_text_ellipsis(s, 12, ty, maxw, v->msg, t->window_text, FONT_UI_BOLD); return; }
    if (v->hover >= 0) { gfx_text_ellipsis(s, 12, ty, maxw, g_btn_tips[v->hover], muted, FONT_UI); return; }
    if (!v->img) {
        gfx_text_ellipsis(s, 12, ty, maxw, v->gen_idx >= 0 ? "Creating sample pictures..." : "No picture", muted, FONT_UI);
        return;
    }
    char sz[24];
    if (v->fsize >= (1u << 20)) snprintf(sz, sizeof sz, "%u.%u MB", v->fsize >> 20, ((v->fsize & 0xFFFFF) * 10) >> 20);
    else snprintf(sz, sizeof sz, "%u KB", (v->fsize + 1023) / 1024);
    snprintf(left, sizeof left, "%d x %d pixels    %s    %s%s", v->img->w, v->img->h, v->info.format, sz,
             v->rot ? "    (rotated)" : "");
    gfx_text_ellipsis(s, 12, ty, maxw, left, t->window_text, FONT_UI);
}

static void draw_empty(viewer_t *v, surface_t *s, rect_t st) {
    const theme_t *t = theme_get();
    bool dark_stage = t->dark || v->fs;
    uint32_t ink = dark_stage ? RGB(200, 204, 212) : RGB(90, 96, 108);
    int cx = st.x + st.w / 2, cy = st.y + st.h / 2 - 30;
    /* picture glyph */
    gfx_rounded_rect(s, cx - 40, cy - 30, 80, 60, 8, ink);
    gfx_rounded_rect(s, cx - 39, cy - 29, 78, 58, 7, ink);
    gfx_fill_triangle(s, cx - 30, cy + 22, cx - 8, cy - 6, cx + 10, cy + 22, ink);
    gfx_fill_triangle(s, cx + 2, cy + 22, cx + 16, cy + 4, cx + 30, cy + 22, ink);
    gfx_fill_circle_aa(s, cx + 18, cy - 12, 6, ink);
    const char *l1 = v->err[0] ? v->err : (v->gen_idx >= 0 ? "Creating sample pictures..." : "No picture open");
    const char *l2 = v->err[0] ? "Choose another picture with Open (Ctrl+O)." : "Click Open or press Ctrl+O to choose a BMP picture.";
    gfx_text_aligned(s, rect_make(st.x + 20, cy + 44, st.w - 40, 24), l1, dark_stage ? COLOR_WHITE : t->window_text,
                     FONT_UI_LARGE, ALIGN_CENTER | ALIGN_MIDDLE);
    gfx_text_aligned(s, rect_make(st.x + 20, cy + 72, st.w - 40, 20), l2, ink, FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
}

/* full-screen overlay: "name    3 / 7" (or the latest message) in a dark pill */
static void draw_caption(viewer_t *v, surface_t *s, rect_t st) {
    char text[VFS_NAME_MAX + 200];
    if (v->msg[0] && v->msg_in_caption) strlcpy(text, v->msg, sizeof text);
    else if (v->nnames > 0 && v->cur >= 0)
        snprintf(text, sizeof text, "%s    %d / %d", v->path[0] ? vfs_basename(v->path) : "", v->cur + 1, v->nnames);
    else strlcpy(text, v->path[0] ? vfs_basename(v->path) : "", sizeof text);
    if (!text[0]) return;
    int tw = MIN(font_text_width(FONT_UI_BOLD, text), st.w - 80);
    int pw = tw + 36, ph = 34;
    int px = st.x + (st.w - pw) / 2, py = st.y + st.h - ph - 28;
    gfx_fill_rounded_rect(s, px, py, pw, ph, ph / 2, RGBA(16, 18, 24, 190));
    gfx_text_ellipsis(s, px + 18, py + (ph - font_height(FONT_UI_BOLD)) / 2, tw, text, COLOR_WHITE, FONT_UI_BOLD);
}

static void iv_paint(viewer_t *v) {
    surface_t *s = win_surface(v->win);
    const theme_t *t = theme_get();
    rect_t st = stage_rect(v);
    rect_t old = s->clip;
    rect_t sc = rect_intersect(old, st);
    if (!rect_empty(sc)) {
        s->clip = sc;
        uint32_t stage_bg = v->fs ? RGB(0, 0, 0) : (t->dark ? RGB(22, 23, 27) : RGB(212, 217, 226));
        gfx_fill_rect(s, st.x, st.y, st.w, st.h, stage_bg);
        if (v->img) {
            int dw, dh;
            disp_size(v, &dw, &dh);
            int ox = st.x + (int)v->offx, oy = st.y + (int)v->offy;
            if (!v->fs && dw < st.w && dh < st.h) gfx_shadow(s, ox, oy + 2, dw, dh, 0, 10, t->dark ? 140 : 70);
            bool alpha = v->img->flags & SURF_ALPHA;
            if (alpha) draw_checker(s, rect_make(ox, oy, dw, dh), sc);
            if (v->hq && v->hq_done && v->hq->w == dw && v->hq->h == dh) {
                if (alpha) gfx_blit_alpha(s, ox, oy, v->hq, 255);
                else gfx_blit(s, ox, oy, v->hq);
            } else if (dw == v->img->w && dh == v->img->h) {
                if (alpha) gfx_blit_alpha(s, ox, oy, v->img, 255);
                else gfx_blit(s, ox, oy, v->img);
            } else {
                /* pixel-exact blocks when zoomed in for inspection, smooth when fitted */
                gfx_blit_scaled(s, ox, oy, dw, dh, v->img, 0, 0, v->img->w, v->img->h, v->scale < 2.0 || v->fit);
            }
            if (v->fs && v->caption_until) draw_caption(v, s, st);
        } else {
            draw_empty(v, s, st);
        }
        s->clip = old;
    }
    if (!v->fs) {
        draw_toolbar(v, s);
        draw_status(v, s);
    }
}

/* ======================================================================================
 * input
 * ====================================================================================== */
static void iv_action(viewer_t *v, int b) {
    if (b < 0 || !btn_enabled(v, b)) return;
    switch (b) {
    case B_OPEN: iv_open_dialog(v); break;
    case B_PREV: iv_step(v, -1); break;
    case B_NEXT: iv_step(v, 1); break;
    case B_ZOOM_OUT: zoom_center(v, -1); break;
    case B_ZOOM_IN: zoom_center(v, 1); break;
    case B_FIT: iv_fit(v); break;
    case B_ACTUAL: {
        rect_t st = stage_rect(v);
        set_scale(v, 1.0, st.x + st.w / 2.0, st.y + st.h / 2.0);
        break;
    }
    case B_ROT_L: iv_rotate(v, false); break;
    case B_ROT_R: iv_rotate(v, true); break;
    case B_SLIDE: iv_slideshow(v, !v->slideshow); break;
    case B_WALL: iv_wallpaper(v); break;
    case B_EDIT: iv_edit(v); break;
    }
    toolbar_dirty(v);
}

static bool can_pan(const viewer_t *v) {
    if (!v->img) return false;
    rect_t st = stage_rect(v);
    int dw, dh;
    disp_size(v, &dw, &dh);
    return dw > st.w || dh > st.h;
}

static void menu_cb(int item, void *ctx) {
    viewer_t *v = ctx;
    static const int map[] = { B_OPEN, -1, B_PREV, B_NEXT, -1, B_ZOOM_IN, B_ZOOM_OUT, B_FIT, B_ACTUAL, -1,
                               B_ROT_L, B_ROT_R, -1, B_SLIDE, B_WALL, B_EDIT };
    if (item >= 0 && item < ARRAY_SIZE(map)) iv_action(v, map[item]);
}

static void iv_context_menu(viewer_t *v, int x, int y) {
    char items[400];
    snprintf(items, sizeof items,
             "Open...\tCtrl+O|-|%sPrevious\tPgUp|%sNext\tPgDn|-|%sZoom in\t+|%sZoom out\t-|%sFit to window\t0|%sActual size\t1|-|"
             "%sRotate left\tL|%sRotate right\tR|-|%s%s\tF5|%sSet as wallpaper\tW|%sEdit in Paint\tE",
             btn_enabled(v, B_PREV) ? "" : "!", btn_enabled(v, B_NEXT) ? "" : "!",
             v->img ? "" : "!", v->img ? "" : "!", v->img ? "" : "!", v->img ? "" : "!",
             v->img ? "" : "!", v->img ? "" : "!",
             btn_enabled(v, B_SLIDE) ? "" : "!", v->slideshow ? "Stop slideshow" : "Slideshow",
             v->img ? "" : "!", btn_enabled(v, B_EDIT) ? "" : "!");
    ui_popup_menu(v->win, x, y, items, menu_cb, v);
}

static void iv_mouse_down(viewer_t *v, const event_t *ev) {
    int b = btn_hit(v, ev->x, ev->y);
    if (b >= 0) {
        if (ev->button == MOUSE_LEFT && btn_enabled(v, b)) { v->pressed = b; toolbar_dirty(v); }
        return;
    }
    if (!rect_contains(stage_rect(v), ev->x, ev->y)) return;
    if (ev->button == MOUSE_RIGHT) { iv_context_menu(v, ev->x, ev->y); return; }
    if ((ev->button == MOUSE_LEFT || ev->button == MOUSE_MIDDLE) && can_pan(v)) {
        v->dragging = true;
        v->drag_mx = ev->x;
        v->drag_my = ev->y;
        v->drag_ox = v->offx;
        v->drag_oy = v->offy;
        win_set_cursor(v->win, CURSOR_MOVE);
    }
}

static void iv_mouse_move(viewer_t *v, const event_t *ev) {
    v->mx = ev->x;
    v->my = ev->y;
    if (v->dragging) {
        if (!(ev->buttons & (MOUSE_LEFT | MOUSE_MIDDLE))) { v->dragging = false; return; }
        v->offx = v->drag_ox + (ev->x - v->drag_mx);
        v->offy = v->drag_oy + (ev->y - v->drag_my);
        clamp_pan(v);
        stage_dirty(v);
        return;
    }
    int h = btn_hit(v, ev->x, ev->y);
    if (h != v->hover) { v->hover = h; toolbar_dirty(v); status_dirty(v); }
    bool in = rect_contains(stage_rect(v), ev->x, ev->y);
    win_set_cursor(v->win, in && can_pan(v) ? CURSOR_HAND : CURSOR_ARROW);
}

static void iv_mouse_up(viewer_t *v, const event_t *ev) {
    if (v->dragging) {
        v->dragging = false;
        win_set_cursor(v->win, can_pan(v) ? CURSOR_HAND : CURSOR_ARROW);
        return;
    }
    int p = v->pressed;
    v->pressed = -1;
    if (p >= 0) {
        toolbar_dirty(v);
        if (ev->button == MOUSE_LEFT && btn_hit(v, ev->x, ev->y) == p) iv_action(v, p);
    }
}

static void iv_key(viewer_t *v, const event_t *ev) {
    int k = ev->key;
    bool shift = ev->mods & MOD_SHIFT;
    if (ev->mods & MOD_CTRL) {
        switch (k) {
        case 'o': iv_open_dialog(v); break;
        case 'w': win_close(v->win); break;
        case '=': case '+': zoom_center(v, 1); break;
        case '-': zoom_center(v, -1); break;
        case '0': iv_fit(v); break;
        case 'e': iv_edit(v); break;
        }
        return;
    }
    if (ev->mods & MOD_ALT) return;
    bool pan = can_pan(v);
    int step = 60;
    switch (k) {
    case KEY_PAGEUP: case KEY_BACKSPACE: iv_step(v, -1); break;
    case KEY_PAGEDOWN: case ' ': iv_step(v, 1); break;
    case KEY_LEFT:
        if (pan) { v->offx += step; clamp_pan(v); stage_dirty(v); } else iv_step(v, -1);
        break;
    case KEY_RIGHT:
        if (pan) { v->offx -= step; clamp_pan(v); stage_dirty(v); } else iv_step(v, 1);
        break;
    case KEY_UP: if (pan) { v->offy += step; clamp_pan(v); stage_dirty(v); } break;
    case KEY_DOWN: if (pan) { v->offy -= step; clamp_pan(v); stage_dirty(v); } break;
    case KEY_HOME: iv_first_last(v, false); break;
    case KEY_END: iv_first_last(v, true); break;
    case '=': case '+': zoom_center(v, 1); break;
    case '-': zoom_center(v, -1); break;
    case '0': case 'f': iv_fit(v); break;
    case '1': iv_action(v, B_ACTUAL); break;
    case 'r': iv_rotate(v, !shift); break;
    case 'l': iv_rotate(v, false); break;
    case 'w': iv_wallpaper(v); break;
    case 'e': iv_edit(v); break;
    case KEY_F5: iv_slideshow(v, !v->slideshow); break;
    case KEY_F11: iv_set_fullscreen(v, !win_is_fullscreen(v->win)); break;
    case KEY_ESCAPE:
        if (v->slideshow) iv_slideshow(v, false);
        else if (win_is_fullscreen(v->win)) iv_set_fullscreen(v, false);
        break;
    }
}

static void iv_free(viewer_t *v) {
    if (v->gen_idx >= 0 || v->gen) g_gen_running = false;   /* another window may finish it later */
    surface_destroy(v->gen);
    surface_destroy(v->img);
    hq_reset(v);
    list_free(v);
    free(v);
}

static void iv_event(window_t *win, const event_t *ev) {
    viewer_t *v = win_get_userdata(win);
    if (!v) return;
    switch (ev->type) {
    case EV_PAINT: iv_paint(v); break;
    case EV_RESIZE:
        if (win_is_fullscreen(win) && !v->fs) { v->fs = true; show_caption(v); }
        v->fs = win_is_fullscreen(win);
        if (v->fit) { v->fit = false; iv_fit(v); }
        else { clamp_pan(v); stage_dirty(v); }
        if (!v->fs && v->slideshow) iv_slideshow(v, false);
        break;
    case EV_TIMER:
        if (ev->timer_id == v->work_timer) work_tick(v);
        else if (ev->timer_id == v->slide_timer) iv_step(v, 1);
        else if (ev->timer_id == v->msg_timer) {
            uint32_t now = sys_uptime_ms();
            if (v->msg[0] && now >= v->msg_until) { v->msg[0] = 0; status_dirty(v); }
            if (v->caption_until && now >= v->caption_until) { v->caption_until = 0; stage_dirty(v); }
            if (!v->msg[0] && !v->caption_until) { win_kill_timer(win, v->msg_timer); v->msg_timer = 0; }
        }
        break;
    case EV_MOUSE_DOWN: iv_mouse_down(v, ev); break;
    case EV_MOUSE_MOVE: iv_mouse_move(v, ev); break;
    case EV_MOUSE_UP: iv_mouse_up(v, ev); break;
    case EV_DOUBLE_CLICK:
        if (ev->button == MOUSE_LEFT && rect_contains(stage_rect(v), ev->x, ev->y) && v->img) {
            if (v->fit && fabs(v->scale - 1.0) > 1e-6) set_scale(v, 1.0, ev->x, ev->y);
            else iv_fit(v);
        }
        break;
    case EV_MOUSE_WHEEL:
        if (rect_contains(stage_rect(v), ev->x, ev->y)) zoom_step(v, -ev->wheel, ev->x, ev->y);
        break;
    case EV_MOUSE_LEAVE:
        if (v->hover >= 0) { v->hover = -1; toolbar_dirty(v); status_dirty(v); }
        break;
    case EV_BLUR:
        v->dragging = false;
        if (v->pressed >= 0) { v->pressed = -1; toolbar_dirty(v); }
        break;
    case EV_KEY_DOWN: iv_key(v, ev); break;
    case EV_CLOSE:
        win_set_userdata(win, NULL);
        iv_free(v);
        break;
    }
}

static void viewer_launch(const char *arg) {
    viewer_t *v = calloc(1, sizeof *v);
    if (!v) return;
    window_t *win = win_create("Image Viewer", IV_W, IV_H, WIN_RESIZABLE);
    if (!win) { free(v); return; }
    v->win = win;
    v->hover = v->pressed = -1;
    v->cur = -1;
    v->gen_idx = -1;
    v->scale = 1;
    v->fit = true;
    win_set_userdata(win, v);
    win_set_event_handler(win, iv_event);
    win_set_min_size(win, 560, 300);

    /* first launch since boot: create the sample pictures that are missing */
    if (!g_samples_checked && !g_gen_running) {
        v->gen_idx = next_missing_sample(0);
        if (v->gen_idx >= 0) { g_gen_running = true; work_ensure(v); }
        else g_samples_checked = true;
    }

    if (arg && *arg) {
        iv_open(v, arg);
    } else {
        strlcpy(v->dir, PICTURES, sizeof v->dir);
        list_load(v);
        if (v->nnames > 0) iv_step(v, 1);
    }
    iv_update_title(v);
}

/* App icon: a framed landscape photo with a sun, mountains and a lake. */
static void viewer_icon(surface_t *s, int x, int y, int size) {
    int S = size;
    int fx = x + S * 8 / 100, fy = y + S * 14 / 100, fw = S * 84 / 100, fh = S * 72 / 100;
    int r = MAX(2, S / 12);
    gfx_fill_rounded_rect(s, fx, fy + S / 40, fw, fh, r, RGBA(0, 0, 0, 60));
    gfx_fill_rounded_rect(s, fx, fy, fw, fh, r, RGB(248, 250, 252));
    int b = MAX(2, S * 7 / 100);
    int ix = fx + b, iy = fy + b, iw = fw - 2 * b, ih = fh - 2 * b;
    gfx_fill_rounded_gradient_v(s, ix, iy, iw, ih, MAX(1, r / 2), RGB(80, 160, 245), RGB(190, 226, 255));
    surface_t sub = surface_sub(s, rect_make(ix, iy, iw, ih));
    sub.clip = rect_intersect(sub.clip, rect_make(s->clip.x - ix, s->clip.y - iy, s->clip.w, s->clip.h));
    gfx_fill_circle_aa(&sub, iw * 0.74, ih * 0.28, ih * 0.13, RGB(255, 214, 70));
    gfx_fill_triangle(&sub, -iw / 10, ih * 82 / 100, iw * 30 / 100, ih * 30 / 100, iw * 70 / 100, ih * 82 / 100, RGB(52, 120, 92));
    gfx_fill_triangle(&sub, iw * 22 / 100, ih * 30 / 100 + ih / 8, iw * 30 / 100, ih * 30 / 100, iw * 38 / 100, ih * 30 / 100 + ih / 8, RGB(240, 246, 250));
    gfx_fill_triangle(&sub, iw * 40 / 100, ih * 82 / 100, iw * 72 / 100, ih * 44 / 100, iw * 110 / 100, ih * 82 / 100, RGB(34, 96, 74));
    gfx_fill_rect(&sub, 0, ih * 80 / 100, iw, ih - ih * 80 / 100, RGB(40, 120, 200));
    gfx_fill_rect(&sub, iw / 8, ih * 87 / 100, iw / 3, MAX(1, ih / 30), RGBA(255, 255, 255, 120));
}

REGISTER_APP(.name = "Image Viewer", .category = CAT_GRAPHICS,
             .description = "View BMP pictures",
             .launch = viewer_launch, .draw_icon = viewer_icon,
             .file_types = "bmp,dib,ppm");
