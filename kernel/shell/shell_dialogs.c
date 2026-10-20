/*
 * shell_dialogs.c - the Open With dialog, the Properties dialog and the
 * command output window used by the Run dialog.
 */
#include "shell/shell_internal.h"

/* ==== a list of apps with big icons (custom widget) ======================================== */
#define AL_ROW_H 44
#define AL_MAX   64

typedef struct {
    const app_desc_t *apps[AL_MAX];
    int n;
    int ncapable;          /* the first ncapable apps can open the file */
    int sel, hover, scroll;
    widget_cb on_activate;
    void *ctx;
} applist_t;

static int al_visible(widget_t *w) { return MAX(1, ui_rect(w).h / AL_ROW_H); }

static void al_clamp(widget_t *w, applist_t *l) {
    int maxs = MAX(0, l->n * AL_ROW_H - ui_rect(w).h);
    l->scroll = CLAMP(l->scroll, 0, maxs);
}

static void al_ensure(widget_t *w, applist_t *l) {
    rect_t r = ui_rect(w);
    if (l->sel < 0) return;
    int top = l->sel * AL_ROW_H, bot = top + AL_ROW_H;
    if (top < l->scroll) l->scroll = top;
    if (bot > l->scroll + r.h) l->scroll = bot - r.h;
    al_clamp(w, l);
}

static void al_draw(widget_t *w, surface_t *s, rect_t r) {
    applist_t *l = ui_get_ctx(w);
    const theme_t *t = theme_get();
    ui_draw_field(s, r, ui_has_focus(w) ? UI_STATE_FOCUSED : 0);
    rect_t old = s->clip;
    rect_t inner = rect_inset(r, 2);
    surface_set_clip(s, rect_intersect(old, inner));
    for (int i = 0; i < l->n; i++) {
        int y = r.y + 2 + i * AL_ROW_H - l->scroll;
        if (y + AL_ROW_H < r.y || y > r.y + r.h) continue;
        const app_desc_t *a = l->apps[i];
        bool sel = i == l->sel;
        if (sel) gfx_fill_rounded_rect(s, r.x + 4, y + 2, r.w - 8, AL_ROW_H - 4, 6, color_mix(t->field_bg, t->accent, t->dark ? 90 : 60));
        else if (i == l->hover) gfx_fill_rounded_rect(s, r.x + 4, y + 2, r.w - 8, AL_ROW_H - 4, 6, color_mix(t->field_bg, t->field_text, 16));
        if (i == l->ncapable && i > 0) gfx_hline(s, r.x + 12, y, r.w - 24, color_mix(t->field_bg, t->field_text, 40));
        app_draw_icon(s, a, r.x + 12, y + 6, 32);
        gfx_text_ellipsis(s, r.x + 54, y + 6, r.w - 66, a->name, t->field_text, FONT_UI_BOLD);
        if (a->description)
            gfx_text_ellipsis(s, r.x + 54, y + 23, r.w - 66, a->description, color_mix(t->field_text, t->field_bg, 100), FONT_UI);
    }
    s->clip = old;
}

static bool al_event(widget_t *w, const event_t *ev) {
    applist_t *l = ui_get_ctx(w);
    rect_t r = ui_rect(w);
    int idx = (ev->y - r.y - 2 + l->scroll) / AL_ROW_H;
    if (ev->y < r.y + 2 || idx < 0 || idx >= l->n) idx = -1;
    switch (ev->type) {
    case EV_MOUSE_MOVE:
        if (idx != l->hover) { l->hover = idx; ui_invalidate(w); }
        return true;
    case EV_MOUSE_DOWN:
        if (idx >= 0) { l->sel = idx; ui_invalidate(w); }
        return true;
    case EV_DOUBLE_CLICK:
        if (idx >= 0 && l->on_activate) { l->sel = idx; l->on_activate(w, l->ctx); }
        return true;
    case EV_MOUSE_WHEEL:
        l->scroll += ev->wheel * AL_ROW_H;
        al_clamp(w, l);
        ui_invalidate(w);
        return true;
    case EV_KEY_DOWN: {
        int s = l->sel;
        if (ev->key == KEY_DOWN) s++;
        else if (ev->key == KEY_UP) s--;
        else if (ev->key == KEY_PAGEDOWN) s += al_visible(w);
        else if (ev->key == KEY_PAGEUP) s -= al_visible(w);
        else if (ev->key == KEY_HOME) s = 0;
        else if (ev->key == KEY_END) s = l->n - 1;
        else if (ev->key == KEY_ENTER) { if (l->sel >= 0 && l->on_activate) l->on_activate(w, l->ctx); return true; }
        else return false;
        l->sel = CLAMP(s, 0, l->n - 1);
        al_ensure(w, l);
        ui_invalidate(w);
        return true;
    }
    default:
        return false;
    }
}

/* ==== Open With ========================================================================== */
#define OW_W 420
#define OW_H 470

typedef struct {
    char path[VFS_PATH_MAX];
    char key[16];               /* extension or "folder" */
    applist_t list;
    widget_t *lw, *always, *all;
    const app_desc_t *capable[AL_MAX];
    int ncapable;
} openwith_t;

static void ow_fill(openwith_t *o, bool all) {
    applist_t *l = &o->list;
    const app_desc_t *keep = l->sel >= 0 && l->sel < l->n ? l->apps[l->sel] : NULL;
    l->n = 0;
    for (int i = 0; i < o->ncapable && l->n < AL_MAX; i++) l->apps[l->n++] = o->capable[i];
    l->ncapable = l->n;
    if (all || !l->n) {
        for (int i = 0; i < app_count() && l->n < AL_MAX; i++) {
            const app_desc_t *a = app_get(i);
            if (a->flags & APP_HIDDEN) continue;
            bool dup = false;
            for (int j = 0; j < l->ncapable; j++) if (l->apps[j] == a) dup = true;
            if (!dup) l->apps[l->n++] = a;
        }
    }
    l->sel = l->n ? 0 : -1;
    for (int i = 0; i < l->n; i++) if (l->apps[i] == keep) l->sel = i;
    l->scroll = 0;
    if (o->lw) { al_ensure(o->lw, l); ui_invalidate(o->lw); }
}

static void ow_open(window_t *win, openwith_t *o) {
    applist_t *l = &o->list;
    if (l->sel < 0 || l->sel >= l->n) { sys_beep(440, 60); return; }
    const app_desc_t *a = l->apps[l->sel];
    char path[VFS_PATH_MAX];
    strlcpy(path, o->path, sizeof path);
    if (o->key[0] && ui_get_value(o->always)) assoc_set_default(o->key, a->name);
    win_close(win);                 /* frees o */
    app_launch(a, path);
}

static void ow_activate(widget_t *w, void *ctx) { ow_open(ui_window(w), ctx); }
static void ow_btn_open(widget_t *w, void *ctx) { ow_open(ui_window(w), ctx); }
static void ow_btn_cancel(widget_t *w, void *ctx) { (void)ctx; win_close(ui_window(w)); }
static void ow_all_changed(widget_t *w, void *ctx) { ow_fill(ctx, ui_get_value(w)); }

static void ow_event(window_t *win, const event_t *ev) {
    openwith_t *o = win_get_userdata(win);
    if (!o) return;
    const theme_t *t = theme_get();
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        icon_draw(icon_for_path(o->path), s, 20, 16, 48);
        char buf[160];
        snprintf(buf, sizeof buf, "How do you want to open \"%s\"?", vfs_basename(o->path));
        gfx_text_ellipsis(s, 80, 20, s->w - 96, buf, t->window_text, FONT_UI_BOLD);
        snprintf(buf, sizeof buf, "%s - %s", filetype_description(o->path),
                 o->ncapable ? "pick an app below" : "no installed app is registered for this type");
        gfx_text_ellipsis(s, 80, 40, s->w - 96, buf, color_mix(t->window_text, t->window_bg, 100), FONT_UI);
        int bh = 60;
        gfx_fill_rect(s, 0, s->h - bh, s->w, bh, t->dark ? color_shade(t->window_bg, -12) : color_shade(t->window_bg, -6));
        gfx_hline(s, 0, s->h - bh, s->w, color_mix(t->window_bg, t->window_border, 70));
        break;
    }
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESCAPE) win_close(win);
        else if (ev->key == KEY_ENTER) ow_open(win, o);
        break;
    case EV_CLOSE:
        kfree(o);
        win_set_userdata(win, NULL);
        break;
    default: break;
    }
}

window_t *shell_open_with_dialog(const char *path) {
    if (!path) return NULL;
    openwith_t *o = kcalloc(1, sizeof *o);
    if (!o) return NULL;
    if (vfs_normalize(NULL, path, o->path) != 0) strlcpy(o->path, path, sizeof o->path);
    if (vfs_is_dir(o->path)) strlcpy(o->key, "folder", sizeof o->key);
    else {
        const char *e = vfs_extension(o->path);
        for (int i = 0; e[i] && i < 15; i++) o->key[i] = (char)tolower((unsigned char)e[i]);
    }
    o->ncapable = MIN(assoc_apps(o->path, o->capable, AL_MAX), AL_MAX);
    o->list.hover = -1;
    o->list.on_activate = ow_activate;
    o->list.ctx = o;
    ow_fill(o, false);
    const app_desc_t *saved = wm_current_app;
    wm_current_app = NULL;
    window_t *w = win_create("Open with", OW_W, OW_H, WIN_CENTER | WIN_DIALOG | WIN_HIDDEN);
    wm_current_app = saved;
    if (!w) { kfree(o); return NULL; }
    win_set_userdata(w, o);
    win_set_event_handler(w, ow_event);
    o->lw = ui_custom(w, 16, 78, OW_W - 32, 250, al_draw, al_event, &o->list);
    o->all = ui_checkbox(w, 16, 338, OW_W - 32, 24, "Show all apps", o->ncapable == 0, ow_all_changed, o);
    if (o->ncapable == 0) ui_set_enabled(o->all, false);
    char lbl[96];
    if (!strcmp(o->key, "folder")) snprintf(lbl, sizeof lbl, "Always use this app to open folders");
    else if (o->key[0]) snprintf(lbl, sizeof lbl, "Always use this app to open .%s files", o->key);
    else snprintf(lbl, sizeof lbl, "Always use this app (files without an extension)");
    o->always = ui_checkbox(w, 16, 366, OW_W - 32, 24, lbl, false, NULL, NULL);
    if (!o->key[0]) ui_set_enabled(o->always, false);
    widget_t *ok = ui_button(w, OW_W - 16 - 96 * 2 - 8, OW_H - 46, 96, 32, "Open", ow_btn_open, o);
    ui_button_set_default(ok, true);
    ui_button(w, OW_W - 16 - 96, OW_H - 46, 96, 32, "Cancel", ow_btn_cancel, o);
    ui_focus(o->lw);
    al_ensure(o->lw, &o->list);
    win_show(w);
    return w;
}

/* ==== Properties ======================================================================== */
#define PR_W 400

typedef struct {
    char path[VFS_PATH_MAX];
    vfs_stat_t st;
    bool ok;
    int nfiles, ndirs;
    uint64_t total;
    widget_t *change;
} props_t;

static void count_tree(const char *dir, props_t *p, int depth) {
    if (depth > 24 || p->nfiles + p->ndirs > 20000) return;
    int n = vfs_list(dir, NULL, 0);
    if (n <= 0) return;
    vfs_dirent_t *ents = kcalloc((size_t)n, sizeof *ents);
    if (!ents) return;
    n = MIN(vfs_list(dir, ents, n), n);
    for (int i = 0; i < n; i++) {
        char path[VFS_PATH_MAX];
        vfs_join(dir, ents[i].name, path);
        if (ents[i].is_dir) { p->ndirs++; count_tree(path, p, depth + 1); }
        else {
            vfs_stat_t st;
            p->nfiles++;
            if (vfs_stat(path, &st) == 0) p->total += st.size;
        }
    }
    kfree(ents);
}

static void fmt_size(uint64_t b, char *buf, int n) {
    char grp[32];
    /* thousands separators for the exact count */
    char raw[24];
    snprintf(raw, sizeof raw, "%llu", (unsigned long long)b);
    int len = (int)strlen(raw), o = 0;
    for (int i = 0; i < len; i++) {
        grp[o++] = raw[i];
        if ((len - i - 1) % 3 == 0 && i != len - 1) grp[o++] = ',';
    }
    grp[o] = 0;
    if (b < 1024) snprintf(buf, (size_t)n, "%s bytes", grp);
    else if (b < 1024 * 1024) snprintf(buf, (size_t)n, "%.1f KB (%s bytes)", b / 1024.0, grp);
    else snprintf(buf, (size_t)n, "%.2f MB (%s bytes)", b / (1024.0 * 1024.0), grp);
}

static void fmt_time(uint32_t t, char *buf, int n) {
    if (!t) { snprintf(buf, (size_t)n, "-"); return; }
    datetime_t dt;
    sys_time_to_datetime(t, &dt);
    snprintf(buf, (size_t)n, "%s, %d %s %d, %02d:%02d:%02d", sys_weekday_name(dt.weekday), dt.day,
             sys_month_name(dt.month), dt.year, dt.hour, dt.minute, dt.second);
}

#define PR_ROW 26

static int prop_row(surface_t *s, int y, const char *label, const char *value) {
    const theme_t *t = theme_get();
    gfx_text(s, 20, y, label, color_mix(t->window_text, t->window_bg, 100), FONT_UI);
    gfx_text_ellipsis(s, 116, y, s->w - 132, value, t->window_text, FONT_UI);
    return PR_ROW;
}

/* y of the "Opens with" row */
static int props_open_y(const props_t *p) {
    return 94 + PR_ROW * (p->ok && p->st.is_dir ? 3 : 2) + 14 + 2 * PR_ROW + 14;
}

static void props_change(widget_t *w, void *ctx) {
    props_t *p = ctx;
    shell_open_with_dialog(p->path);
    win_close(ui_window(w));
}

static void props_ok(widget_t *w, void *ctx) { (void)ctx; win_close(ui_window(w)); }

static void props_event(window_t *win, const event_t *ev) {
    props_t *p = win_get_userdata(win);
    if (!p) return;
    const theme_t *t = theme_get();
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        icon_draw(icon_for_path(p->path), s, 20, 18, 48);
        const char *name = !strcmp(p->path, "/") ? "/" : vfs_basename(p->path);
        gfx_text_ellipsis(s, 82, 20, s->w - 100, name, t->window_text, FONT_UI_LARGE);
        gfx_text_ellipsis(s, 82, 46, s->w - 100, filetype_description(p->path), color_mix(t->window_text, t->window_bg, 100), FONT_UI);
        ui_draw_separator(s, 16, 80, s->w - 32);
        int y = 94;
        char buf[200], dir[VFS_PATH_MAX];
        vfs_dirname(p->path, dir);
        y += prop_row(s, y, "Location", dir);
        if (!p->ok) { prop_row(s, y, "Status", "This item no longer exists."); break; }
        if (p->st.is_dir) {
            fmt_size(p->total, buf, sizeof buf);
            y += prop_row(s, y, "Size", buf);
            snprintf(buf, sizeof buf, "%d file%s, %d folder%s", p->nfiles, p->nfiles == 1 ? "" : "s", p->ndirs, p->ndirs == 1 ? "" : "s");
            y += prop_row(s, y, "Contains", buf);
        } else {
            fmt_size(p->st.size, buf, sizeof buf);
            y += prop_row(s, y, "Size", buf);
        }
        ui_draw_separator(s, 16, y + 2, s->w - 32);
        y += 14;
        fmt_time(p->st.ctime, buf, sizeof buf);
        y += prop_row(s, y, "Created", buf);
        fmt_time(p->st.mtime, buf, sizeof buf);
        y += prop_row(s, y, "Modified", buf);
        ui_draw_separator(s, 16, y + 2, s->w - 32);
        y += 14;
        const app_desc_t *a = assoc_default_app(p->path);
        gfx_text(s, 20, y + 6, "Opens with", color_mix(t->window_text, t->window_bg, 100), FONT_UI);
        if (a) {
            app_draw_icon(s, a, 116, y + 2, 24);
            gfx_text_ellipsis(s, 148, y + 6, s->w - 148 - 120, a->name, t->window_text, FONT_UI_BOLD);
        } else {
            gfx_text(s, 116, y + 6, "No app (asks every time)", t->window_text, FONT_UI);
        }
        int bh = 60;
        gfx_fill_rect(s, 0, s->h - bh, s->w, bh, t->dark ? color_shade(t->window_bg, -12) : color_shade(t->window_bg, -6));
        gfx_hline(s, 0, s->h - bh, s->w, color_mix(t->window_bg, t->window_border, 70));
        break;
    }
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESCAPE || ev->key == KEY_ENTER) win_close(win);
        break;
    case EV_CLOSE:
        kfree(p);
        win_set_userdata(win, NULL);
        break;
    default: break;
    }
}

window_t *shell_properties(const char *path) {
    if (!path) return NULL;
    props_t *p = kcalloc(1, sizeof *p);
    if (!p) return NULL;
    if (vfs_normalize(NULL, path, p->path) != 0) strlcpy(p->path, path, sizeof p->path);
    p->ok = vfs_stat(p->path, &p->st) == 0;
    if (p->ok && p->st.is_dir) count_tree(p->path, p, 0);
    int h = props_open_y(p) + 40 + 66;
    char title[96];
    snprintf(title, sizeof title, "%s Properties", !strcmp(p->path, "/") ? "/" : vfs_basename(p->path));
    const app_desc_t *saved = wm_current_app;
    wm_current_app = NULL;
    window_t *w = win_create(title, PR_W, h, WIN_CENTER | WIN_DIALOG | WIN_HIDDEN);
    wm_current_app = saved;
    if (!w) { kfree(p); return NULL; }
    win_set_userdata(w, p);
    win_set_event_handler(w, props_event);
    if (p->ok && strcmp(p->path, "/")) p->change = ui_button(w, PR_W - 116, props_open_y(p), 100, 28, "Change...", props_change, p);
    widget_t *ok = ui_button(w, PR_W - 16 - 96, h - 46, 96, 32, "OK", props_ok, p);
    ui_button_set_default(ok, true);
    win_show(w);
    return w;
}

/* ==== command output window ============================================================ */
typedef struct { widget_t *ta; } outwin_t;

static void out_event(window_t *win, const event_t *ev) {
    outwin_t *o = win_get_userdata(win);
    if (!o) return;
    switch (ev->type) {
    case EV_RESIZE:
        ui_move(o->ta, 0, 0, ev->w, ev->h);
        break;
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESCAPE) win_close(win);
        break;
    case EV_CLOSE:
        kfree(o);
        win_set_userdata(win, NULL);
        break;
    default: break;
    }
}

void shell_show_output(const char *title, const char *text) {
    outwin_t *o = kcalloc(1, sizeof *o);
    if (!o) return;
    /* size to the text: up to 90 columns x 24 lines */
    int lines = 1, col = 0, maxcol = 20;
    for (const char *p = text; p && *p; p++) {
        if (*p == '\n') { lines++; col = 0; } else if (++col > maxcol) maxcol = col;
    }
    int cw = MIN(MAX(maxcol, 40), 112) * 8 + 24, ch = MIN(MAX(lines, 4), 26) * 16 + 12;
    const app_desc_t *saved = wm_current_app;
    wm_current_app = shell_run_app();
    window_t *w = win_create(title ? title : "Output", cw, ch, WIN_CENTER | WIN_RESIZABLE);
    wm_current_app = saved;
    if (!w) { kfree(o); return; }
    win_set_userdata(w, o);
    win_set_event_handler(w, out_event);
    win_set_min_size(w, 240, 120);
    o->ta = ui_textarea(w, 0, 0, cw, ch, text ? text : "");
    ui_textarea_set_readonly(o->ta, true);
    ui_focus(o->ta);
}
