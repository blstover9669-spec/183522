/* startmenu.c - the Start menu: searchable, categorised app list plus power buttons. */
#include "wm_internal.h"

#define SM_W 460
#define SM_MAX_H 600
#define SEARCH_H 56
#define FOOTER_H 56
#define ROW_H 44
#define HDR_H 30

enum { ROW_HEADER, ROW_APP, ROW_FILE, ROW_RECENT, ROW_RUN };

typedef struct {
    int type;                /* ROW_* */
    const char *text;
    const app_desc_t *app;   /* ROW_APP, and ROW_RECENT for "app:Name" items */
    int y, h;
    int pidx;                /* ROW_FILE / ROW_RECENT: index into row_paths */
} smrow_t;

#define MAX_RECENT_ROWS 4
#define MAX_FILE_ROWS 5
static char row_paths[MAX_RECENT_ROWS + MAX_FILE_ROWS][VFS_PATH_MAX];
static int npaths;

static window_t *sm_win;
static smrow_t rows[256];
static int nrows;
static int scroll_y, content_h;
static int hover_row = -1, sel_row = -1;
static int hover_power = 0;     /* 1 restart, 2 shutdown, 3 run */
static char filter[48];
static uint32_t closed_serial;

bool startmenu_is_open(void) { return sm_win && win_is_valid(sm_win); }
bool startmenu_just_closed(void) { return closed_serial == wm_click_serial && wm_click_serial != 0; }

/* Search rank of an app for the current filter, lower is better (-1 = no match):
   0 exact name, 1 name starts with it, 2 a later word of the name starts with it,
   3 anywhere in the name, 4 in the description, 5 in the category. Typing "Files"
   must find the Files app before Notepad ("Edit plain text files"). */
static int app_rank(const app_desc_t *a) {
    size_t fl = strlen(filter);
    const char *n = a->name;
    if (!strcasecmp(n, filter)) return 0;
    if (!strncasecmp(n, filter, fl)) return 1;
    for (const char *p = n; *p; p++)
        if ((*p == ' ' || *p == '-') && !strncasecmp(p + 1, filter, fl)) return 2;
    if (strcasestr(n, filter)) return 3;
    if (a->description && strcasestr(a->description, filter)) return 4;
    if (a->category && strcasestr(a->category, filter)) return 5;
    return -1;
}

static void add_row(int type, const char *text, const app_desc_t *app, int pidx, int *y) {
    if (nrows >= 255) return;
    int h = type == ROW_HEADER ? HDR_H : ROW_H;
    rows[nrows++] = (smrow_t){ type, text, app, *y, h, pidx };
    *y += h;
}

static void build_rows(void) {
    nrows = 0;
    npaths = 0;
    int y = 0;
    /* recent items (no filter) */
    if (!filter[0]) {
        int nr = shell_recent_count(), shown = 0;
        for (int i = 0; i < nr && shown < MAX_RECENT_ROWS; i++) {
            const char *it = shell_recent_get(i);
            const app_desc_t *a = NULL;
            if (!strncmp(it, "app:", 4)) {
                a = app_find(it + 4);
                if (!a || (a->flags & APP_HIDDEN)) continue;
            }
            if (!shown) add_row(ROW_HEADER, "Recent", NULL, -1, &y);
            strlcpy(row_paths[npaths], it, VFS_PATH_MAX);
            add_row(ROW_RECENT, row_paths[npaths], a, npaths, &y);
            npaths++;
            shown++;
        }
    }
    if (!filter[0]) {
        /* apps by category */
        const char *cats[32];
        int nc = app_categories(cats, 32);
        if (nc > 32) nc = 32;
        for (int c = 0; c < nc; c++) {
            bool header_added = false;
            for (int i = 0; i < app_count() && nrows < 250; i++) {
                const app_desc_t *a = app_get(i);
                if (a->flags & APP_HIDDEN) continue;
                const char *ac = a->category ? a->category : "Other";
                if (strcasecmp(ac, cats[c])) continue;
                if (!header_added) { add_row(ROW_HEADER, cats[c], NULL, -1, &y); header_added = true; }
                add_row(ROW_APP, a->name, a, -1, &y);
            }
        }
    } else {
        /* matching apps, best match first (name before description), then by name */
        static const app_desc_t *found[200];
        static int rank[200];
        int nf = 0;
        for (int i = 0; i < app_count() && nf < 200; i++) {
            const app_desc_t *a = app_get(i);
            if (a->flags & APP_HIDDEN) continue;
            int rk = app_rank(a);
            if (rk < 0) continue;
            int j = nf++;
            while (j > 0 && (rank[j - 1] > rk || (rank[j - 1] == rk && strcasecmp(found[j - 1]->name, a->name) > 0))) {
                found[j] = found[j - 1];
                rank[j] = rank[j - 1];
                j--;
            }
            found[j] = a;
            rank[j] = rk;
        }
        for (int i = 0; i < nf && nrows < 240; i++) {
            if (!i) add_row(ROW_HEADER, "Apps", NULL, -1, &y);
            add_row(ROW_APP, found[i]->name, found[i], -1, &y);
        }
    }
    /* files (indexer) and commands when searching */
    if (filter[0]) {
        index_hit_t hits[MAX_FILE_ROWS];
        int n = strlen(filter) >= 2 ? indexer_search(filter, hits, MAX_FILE_ROWS) : 0;
        for (int i = 0; i < n; i++) {
            if (!i) add_row(ROW_HEADER, "Files and folders", NULL, -1, &y);
            strlcpy(row_paths[npaths], hits[i].path, VFS_PATH_MAX);
            add_row(ROW_FILE, row_paths[npaths], NULL, npaths, &y);
            npaths++;
        }
        char first[48];
        size_t fl = strcspn(filter, " ");
        strlcpy(first, filter, MIN(fl + 1, sizeof first));
        if (command_find(first) || filter[0] == '/') {
            add_row(ROW_HEADER, "Run", NULL, -1, &y);
            add_row(ROW_RUN, filter, NULL, -1, &y);
        }
    }
    content_h = y;
    sel_row = -1;
    for (int i = 0; i < nrows; i++) if (rows[i].type != ROW_HEADER) { if (filter[0]) sel_row = i; break; }
    scroll_y = 0;
}

static int list_h(window_t *w) { return win_height(w) - SEARCH_H - FOOTER_H; }

static void ensure_visible(window_t *w, int r) {
    if (r < 0) return;
    int lh = list_h(w);
    if (rows[r].y < scroll_y) scroll_y = rows[r].y - (r > 0 && rows[r - 1].type == ROW_HEADER ? HDR_H : 0);
    if (rows[r].y + rows[r].h > scroll_y + lh) scroll_y = rows[r].y + rows[r].h - lh;
    scroll_y = CLAMP(scroll_y, 0, MAX(0, content_h - lh));
}

/* anti-aliased arc; angles in degrees, 0 = top, clockwise; draws where angle is outside (gap0, gap1) */
static void arc(surface_t *s, double cx, double cy, double r, double th, double gap0, double gap1, uint32_t c) {
    int x0 = (int)(cx - r - th), x1 = (int)(cx + r + th) + 1, y0 = (int)(cy - r - th), y1 = (int)(cy + r + th) + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            double d = sqrt(dx * dx + dy * dy);
            double cov = MIN(r + th / 2 - d, d - (r - th / 2)) + 0.5;
            if (cov <= 0) continue;
            double ang = atan2(dx, -dy) * 180.0 / M_PI;
            if (ang < 0) ang += 360;
            if (ang > gap0 && ang < gap1) continue;
            gfx_pixel(s, x, y, color_alpha(c, (int)(MIN(cov, 1.0) * COLOR_A(c))));
        }
}

static void arc_gap_top(surface_t *s, double cx, double cy, double r, double th, double gap, uint32_t c) {
    int x0 = (int)(cx - r - th), x1 = (int)(cx + r + th) + 1, y0 = (int)(cy - r - th), y1 = (int)(cy + r + th) + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            double d = sqrt(dx * dx + dy * dy);
            double cov = MIN(r + th / 2 - d, d - (r - th / 2)) + 0.5;
            if (cov <= 0) continue;
            double ang = fabs(atan2(dx, -dy) * 180.0 / M_PI);
            if (ang < gap) continue;
            gfx_pixel(s, x, y, color_alpha(c, (int)(MIN(cov, 1.0) * COLOR_A(c))));
        }
}

static void draw_power_glyph(surface_t *s, int cx, int cy, uint32_t c) {
    /* ring with a gap at the top (angles 330..30 skipped) */
    arc_gap_top(s, cx, cy, 7.5, 1.7, 35, c);
    gfx_fill_rect(s, cx - 1, cy - 10, 2, 9, c);
}

static void draw_restart_glyph(surface_t *s, int cx, int cy, uint32_t c) {
    arc(s, cx, cy, 7.5, 1.7, 0, 50, c);
    gfx_fill_triangle(s, cx + 1, cy - 12, cx + 7, cy - 7, cx + 1, cy - 3, c);
}

static void sm_paint(window_t *win) {
    surface_t *s = win_surface(win);
    const theme_t *t = theme_get();
    uint32_t bg = t->menu_bg;
    gfx_clear(s, bg);
    /* search field */
    rect_t sr = rect_make(16, 14, s->w - 32, 32);
    gfx_fill_rounded_rect(s, sr.x, sr.y, sr.w, sr.h, 16, t->dark ? color_shade(bg, -14) : color_shade(bg, -10));
    gfx_circle_aa(s, sr.x + 20, sr.y + 15, 5, 1.5, color_mix(t->menu_text, bg, 110));
    gfx_line_aa(s, sr.x + 24, sr.y + 19, sr.x + 28, sr.y + 23, color_mix(t->menu_text, bg, 110));
    gfx_line_aa(s, sr.x + 24.5, sr.y + 19, sr.x + 28.5, sr.y + 23, color_mix(t->menu_text, bg, 110));
    if (filter[0]) {
        int tw = gfx_text(s, sr.x + 40, sr.y + (sr.h - font_height(FONT_UI)) / 2, filter, t->menu_text, FONT_UI);
        gfx_fill_rect(s, sr.x + 41 + tw, sr.y + 8, 1, 16, t->accent);
    } else {
        gfx_text(s, sr.x + 40, sr.y + (sr.h - font_height(FONT_UI)) / 2, "Type to search apps, files and commands", color_mix(t->menu_text, bg, 120), FONT_UI);
    }

    /* list */
    int ly = SEARCH_H, lh = list_h(win);
    rect_t old = s->clip;
    surface_set_clip(s, rect_intersect(old, rect_make(0, ly, s->w, lh)));
    if (!nrows) gfx_text_aligned(s, rect_make(0, ly + 30, s->w, 20), "Nothing found - press Enter to run it", color_mix(t->menu_text, bg, 110), FONT_UI, ALIGN_CENTER);
    uint32_t dim = color_mix(t->menu_text, bg, 100);
    for (int i = 0; i < nrows; i++) {
        smrow_t *r = &rows[i];
        int y = ly + r->y - scroll_y;
        if (y + r->h < ly || y > ly + lh) continue;
        if (r->type == ROW_HEADER) {
            gfx_text(s, 22, y + 10, r->text, t->accent, FONT_UI_BOLD);
            continue;
        }
        if (i == hover_row || i == sel_row)
            gfx_fill_rounded_rect(s, 10, y + 2, s->w - 20 - 8, r->h - 4, 6,
                                  i == sel_row ? color_mix(bg, t->accent, t->dark ? 70 : 45) : t->menu_hover);
        int iy = y + (r->h - 32) / 2;
        const char *title = r->text, *sub = NULL;
        char subbuf[VFS_PATH_MAX + 64];
        if (r->app) {
            app_draw_icon(s, r->app, 22, iy, 32);
            title = r->app->name;
            sub = r->type == ROW_RECENT ? "App" : r->app->description;
        } else if (r->type == ROW_RUN) {
            icon_draw(r->text[0] == '/' ? icon_for_path(r->text) : ICON_TERMINAL, s, 22, iy, 32);
            snprintf(subbuf, sizeof subbuf, "Run \"%s\"", r->text);
            title = subbuf;
            sub = r->text[0] == '/' ? "Open this path" : "Run this command (output opens in a window)";
        } else {
            const char *path = row_paths[r->pidx];
            icon_draw(icon_for_path(path), s, 22, iy, 32);
            title = !strcmp(path, "/") ? "/" : vfs_basename(path);
            char dir[VFS_PATH_MAX];
            vfs_dirname(path, dir);
            snprintf(subbuf, sizeof subbuf, "%s in %s", vfs_is_dir(path) ? "Folder" : filetype_description(path), dir);
            sub = subbuf;
        }
        gfx_text_ellipsis(s, 66, y + 5, s->w - 66 - 30, title, t->menu_text, FONT_UI_BOLD);
        if (sub) gfx_text_ellipsis(s, 66, y + 22, s->w - 66 - 30, sub, dim, FONT_UI);
    }
    s->clip = old;
    /* scrollbar */
    if (content_h > lh) {
        int th = MAX(30, lh * lh / content_h);
        int ty = ly + (lh - th) * scroll_y / MAX(1, content_h - lh);
        gfx_fill_rounded_rect(s, s->w - 10, ty + 2, 4, th - 4, 2, color_alpha(t->menu_text, 70));
    }

    /* footer */
    int fy = s->h - FOOTER_H;
    gfx_fill_rect(s, 0, fy, s->w, FOOTER_H, t->dark ? color_shade(bg, -10) : color_shade(bg, -5));
    gfx_hline(s, 0, fy, s->w, t->menu_separator);
    gfx_fill_circle_aa(s, 36, fy + 28, 16, t->accent);
    gfx_text_aligned(s, rect_make(20, fy + 12, 32, 32), "U", t->accent_text, FONT_UI_LARGE, ALIGN_CENTER | ALIGN_MIDDLE);
    gfx_text(s, 62, fy + 12, "User", t->menu_text, FONT_UI_BOLD);
    char ver[64];
    snprintf(ver, sizeof ver, "%s %s", sys_version(), sys_edition());
    gfx_text_ellipsis(s, 62, fy + 29, s->w - 186 - 62 - 8, ver, color_mix(t->menu_text, bg, 100), FONT_UI);
    rect_t rb = rect_make(s->w - 96, fy + 10, 38, 36), pb = rect_make(s->w - 52, fy + 10, 38, 36);
    rect_t runb = rect_make(s->w - 186, fy + 10, 82, 36);
    if (hover_power == 3) gfx_fill_rounded_rect(s, runb.x, runb.y, runb.w, runb.h, 6, t->menu_hover);
    icon_draw(ICON_RUN, s, runb.x + 10, runb.y + 10, 16);
    gfx_text(s, runb.x + 34, runb.y + (runb.h - font_height(FONT_UI)) / 2, "Run...", t->menu_text, FONT_UI);
    if (hover_power == 1) gfx_fill_rounded_rect(s, rb.x, rb.y, rb.w, rb.h, 6, t->menu_hover);
    if (hover_power == 2) gfx_fill_rounded_rect(s, pb.x, pb.y, pb.w, pb.h, 6, t->menu_hover);
    draw_restart_glyph(s, rb.x + rb.w / 2, rb.y + rb.h / 2, t->menu_text);
    draw_power_glyph(s, pb.x + pb.w / 2, pb.y + pb.h / 2 + 1, t->menu_text);
    gfx_rounded_rect(s, 0, 0, s->w, s->h, 10, color_mix(bg, t->window_border, 140));
}

static int row_at(window_t *w, int x, int y) {
    (void)x;
    if (y < SEARCH_H || y >= SEARCH_H + list_h(w)) return -1;
    int cy = y - SEARCH_H + scroll_y;
    for (int i = 0; i < nrows; i++)
        if (rows[i].type != ROW_HEADER && cy >= rows[i].y && cy < rows[i].y + rows[i].h) return i;
    return -1;
}

static int power_at(window_t *w, int x, int y) {
    int fy = win_height(w) - FOOTER_H, W = win_width(w);
    if (y < fy + 10 || y >= fy + 46) return 0;
    if (x >= W - 96 && x < W - 58) return 1;
    if (x >= W - 52 && x < W - 14) return 2;
    if (x >= W - 186 && x < W - 104) return 3;
    return 0;
}

static void do_power(void *ctx) {
    if ((intptr_t)ctx == 1) sys_reboot();
    else sys_shutdown();
}

static void run_text(const char *text) {
    char err[200];
    if (!shell_run(text, err, sizeof err)) ui_message_box(NULL, "Run", err, MB_OK | MB_ICON_WARNING, NULL, NULL);
}

static void launch_row(window_t *w, int r) {
    if (r < 0 || r >= nrows || rows[r].type == ROW_HEADER) {
        if (r < 0 && filter[0] && !nrows) { char t[48]; strlcpy(t, filter, sizeof t); win_close(w); run_text(t); }
        return;
    }
    smrow_t row = rows[r];
    char path[VFS_PATH_MAX] = "";
    if (row.type == ROW_FILE || row.type == ROW_RECENT) strlcpy(path, row_paths[row.pidx], sizeof path);
    else if (row.type == ROW_RUN) strlcpy(path, row.text, sizeof path);
    win_close(w);
    switch (row.type) {
    case ROW_APP: app_launch(row.app, NULL); break;
    case ROW_RECENT: if (row.app) app_launch(row.app, NULL); else shell_open(path); break;
    case ROW_FILE: shell_open(path); break;
    case ROW_RUN: run_text(path); break;
    default: break;
    }
}

/* right-click context menu on a row (a nested popup: the Start menu stays open) */
static const app_desc_t *ctx_app;
static char ctx_path[VFS_PATH_MAX];

static void ctx_cb(int item, void *ctx) {
    (void)ctx;
    const app_desc_t *a = ctx_app;
    if (a) {
        if (item == 0) { startmenu_close(); app_launch(a, NULL); }
        else if (item == 1) shell_set_pinned(a->name, !shell_is_pinned(a->name));
        return;
    }
    if (!ctx_path[0]) return;
    char path[VFS_PATH_MAX];
    strlcpy(path, ctx_path, sizeof path);
    startmenu_close();
    if (item == 0) shell_open(path);
    else if (item == 1) shell_open_with_dialog(path);
    else if (item == 2) {
        char dir[VFS_PATH_MAX];
        vfs_dirname(path, dir);
        shell_open(dir);
    }
    else if (item == 4) shell_properties(path);
}

static void show_ctx(window_t *w, int r, int x, int y) {
    if (r < 0 || rows[r].type == ROW_HEADER || rows[r].type == ROW_RUN) return;
    ctx_app = rows[r].app;
    ctx_path[0] = 0;
    if (rows[r].pidx >= 0 && !rows[r].app) strlcpy(ctx_path, row_paths[rows[r].pidx], sizeof ctx_path);
    rect_t c = win_client_rect(w);
    if (ctx_app)
        ui_popup_menu_nested(w, c.x + x, c.y + y, shell_is_pinned(ctx_app->name) ? "Open|Unpin from taskbar" : "Open|Pin to taskbar", ctx_cb, NULL);
    else
        ui_popup_menu_nested(w, c.x + x, c.y + y, "Open|Open With...|Open file location|-|Properties", ctx_cb, NULL);
}

static void move_sel(window_t *w, int dir) {
    int r = sel_row;
    for (int n = 0; n < nrows; n++) {
        r += dir;
        if (r < 0) r = nrows - 1;
        if (r >= nrows) r = 0;
        if (rows[r].type != ROW_HEADER) break;
    }
    if (nrows && rows[r].type != ROW_HEADER) { sel_row = r; ensure_visible(w, r); win_invalidate(w); }
}

static void sm_handler(window_t *win, const event_t *ev) {
    switch (ev->type) {
    case EV_PAINT:
        sm_paint(win);
        break;
    case EV_MOUSE_MOVE: {
        int r = row_at(win, ev->x, ev->y), p = power_at(win, ev->x, ev->y);
        if (r != hover_row || p != hover_power) { hover_row = r; hover_power = p; win_invalidate(win); }
        break;
    }
    case EV_MOUSE_LEAVE:
        hover_row = -1; hover_power = 0;
        win_invalidate(win);
        break;
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_RIGHT) show_ctx(win, row_at(win, ev->x, ev->y), ev->x, ev->y);
        break;
    case EV_MOUSE_UP: {
        if (ev->button != MOUSE_LEFT) break;
        int p = power_at(win, ev->x, ev->y);
        if (p == 3) { win_close(win); shell_run_dialog(); break; }
        if (p) {
            win_close(win);
            /* let the menu disappear first */
            timer_add(60, do_power, (void *)(intptr_t)p, false);
            break;
        }
        int r = row_at(win, ev->x, ev->y);
        if (r >= 0) launch_row(win, r);
        break;
    }
    case EV_MOUSE_WHEEL:
        scroll_y = CLAMP(scroll_y + ev->wheel * ROW_H, 0, MAX(0, content_h - list_h(win)));
        hover_row = row_at(win, ev->x, ev->y);
        win_invalidate(win);
        break;
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESCAPE) { win_close(win); break; }
        if (ev->key == KEY_DOWN) { move_sel(win, 1); break; }
        if (ev->key == KEY_UP) { move_sel(win, -1); break; }
        if (ev->key == KEY_ENTER) { launch_row(win, sel_row); break; }
        if (ev->key == KEY_BACKSPACE && filter[0]) {
            filter[strlen(filter) - 1] = 0;
            build_rows();
            win_invalidate(win);
        }
        break;
    case EV_CHAR:
        if (ev->ch >= 32 && ev->ch < 127 && strlen(filter) < sizeof filter - 1) {
            size_t n = strlen(filter);
            filter[n] = (char)ev->ch;
            filter[n + 1] = 0;
            build_rows();
            win_invalidate(win);
        }
        break;
    case EV_CLOSE:
        closed_serial = wm_click_serial;
        sm_win = NULL;
        if (g_taskbar_win) win_invalidate(g_taskbar_win);
        break;
    default:
        break;
    }
}

void startmenu_open(void) {
    if (startmenu_is_open()) return;
    wm_close_popups(NULL);
    filter[0] = 0;
    hover_row = -1;
    hover_power = 0;
    build_rows();
    int h = MIN(SM_MAX_H, screen_height() - wm_taskbar_h - 20);
    h = MIN(h, SEARCH_H + FOOTER_H + MAX(content_h, 200) + 8);
    const app_desc_t *saved = wm_current_app;
    wm_current_app = NULL;
    sm_win = wm_create_ex("Start", SM_W, h, WIN_POPUP | WIN_NO_TASKBAR, LAYER_POPUP);
    wm_current_app = saved;
    if (!sm_win) return;
    sm_win->radius = 10;
    win_set_event_handler(sm_win, sm_handler);
    win_move(sm_win, 8, screen_height() - wm_taskbar_h - h - 8);
    if (g_taskbar_win) win_invalidate(g_taskbar_win);
}

void startmenu_close(void) {
    if (startmenu_is_open()) win_close(sm_win);
}

void startmenu_toggle(void) {
    if (startmenu_is_open()) startmenu_close();
    else startmenu_open();
}
