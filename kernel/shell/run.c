/*
 * run.c - the Run dialog (Win+R, Start menu) and shell_run(): open an app by
 * name, a file or folder by path, or run a registered command. Suggestions
 * (history, apps, commands, path completion, indexed files) update as you
 * type; Up/Down pick one, Enter runs it.
 */
#include "shell/shell_internal.h"

#define RUN_W     480
#define LIST_Y    104
#define SUG_H     28
#define MAX_SUG   6
#define RUN_H     (LIST_Y + MAX_SUG * SUG_H + 14 + 60)
#define MAX_HIST  12

enum { SUG_HISTORY, SUG_APP, SUG_COMMAND, SUG_FILE, SUG_FOLDER };

typedef struct {
    char text[VFS_PATH_MAX];
    int kind;
    const app_desc_t *app;
} sug_t;

typedef struct {
    widget_t *tb;
    sug_t sug[MAX_SUG];
    int nsug, sel, hover;
} run_t;

static window_t *run_win;

/* ---- history ------------------------------------------------------------------------------ */
static char hist[MAX_HIST][VFS_PATH_MAX];
static int nhist;
static bool hist_loaded;

static void hist_load(void) {
    if (hist_loaded) return;
    hist_loaded = true;
    char *text = config_read("runhistory");
    if (!text) return;
    char *cur = text, *line;
    while ((line = config_next_line(&cur)) && nhist < MAX_HIST)
        if (*line && *line != '#') strlcpy(hist[nhist++], line, VFS_PATH_MAX);
    free(text);
}

static void hist_add(const char *s) {
    hist_load();
    int found = -1;
    for (int i = 0; i < nhist; i++) if (!strcmp(hist[i], s)) { found = i; break; }
    if (found < 0) found = nhist < MAX_HIST ? nhist++ : MAX_HIST - 1;
    memmove(hist[1], hist[0], (size_t)found * VFS_PATH_MAX);
    strlcpy(hist[0], s, VFS_PATH_MAX);
    char *buf = kmalloc(MAX_HIST * (VFS_PATH_MAX + 1) + 48);
    if (!buf) return;
    int n = snprintf(buf, 48, "# Run dialog history\n");
    for (int i = 0; i < nhist; i++) n += snprintf(buf + n, VFS_PATH_MAX + 2, "%s\n", hist[i]);
    config_write("runhistory", buf);
    kfree(buf);
}

/* ---- running ------------------------------------------------------------------------------- */
static void trim_copy(const char *in, char *out, size_t n) {
    while (*in == ' ' || *in == '\t') in++;
    strlcpy(out, in, n);
    size_t l = strlen(out);
    while (l && (out[l - 1] == ' ' || out[l - 1] == '\t' || out[l - 1] == '\n')) out[--l] = 0;
}

bool shell_run(const char *text, char *err, int errsz) {
    char t[VFS_PATH_MAX];
    trim_copy(text ? text : "", t, sizeof t);
    if (err && errsz) err[0] = 0;
    if (!t[0]) { if (err) snprintf(err, (size_t)errsz, "Type the name of an app, a file or a command."); return false; }
    /* a path */
    if (t[0] == '/' || t[0] == '~') {
        char path[VFS_PATH_MAX];
        cmd_resolve_path(NULL, t, path);
        if (vfs_exists(path) && shell_open(path)) { hist_add(t); return true; }
        if (err) snprintf(err, (size_t)errsz, "NovaOS cannot find \"%s\". Check the path and try again.", t);
        return false;
    }
    /* an app by name */
    const app_desc_t *a = app_find(t);
    if (a) { hist_add(t); return app_launch(a, NULL); }
    char first[64];
    size_t fl = strcspn(t, " \t");
    strlcpy(first, t, MIN(fl + 1, sizeof first));
    const char *rest = t + fl;
    while (*rest == ' ' || *rest == '\t') rest++;
    /* a registered command: output goes to a window */
    if (command_find(first)) {
        cmd_buffer_t b;
        cmd_buffer_init(&b);
        int status = command_run_line(t, &b.w);
        hist_add(t);
        if (b.len > 0) {
            char title[96];
            snprintf(title, sizeof title, "%s%s", t, status ? " (failed)" : "");
            shell_show_output(title, b.buf);
        }
        kfree(b.buf);
        return true;
    }
    /* "app argument" */
    a = app_find(first);
    if (a && *rest) {
        char arg[VFS_PATH_MAX];
        if (rest[0] != '/' && (rest[0] == '~' || vfs_exists(cmd_resolve_path(NULL, rest, arg))))
            cmd_resolve_path(NULL, rest, arg);
        else strlcpy(arg, rest, sizeof arg);
        hist_add(t);
        return app_launch(a, arg);
    }
    /* a file or folder in the home directory, then anywhere in the index */
    char path[VFS_PATH_MAX];
    if (vfs_normalize("/home/user", t, path) == 0 && vfs_exists(path) && shell_open(path)) { hist_add(t); return true; }
    index_hit_t hit;
    if (indexer_search(t, &hit, 1) == 1 && !strcasecmp(vfs_basename(hit.path), t) && shell_open(hit.path)) {
        hist_add(t);
        return true;
    }
    if (err) snprintf(err, (size_t)errsz, "NovaOS cannot find \"%s\". Make sure you typed the name correctly, then try again.", t);
    return false;
}

/* ---- suggestions ---------------------------------------------------------------------------- */
static bool sug_has(run_t *r, const char *text) {
    for (int i = 0; i < r->nsug; i++) if (!strcasecmp(r->sug[i].text, text)) return true;
    return false;
}

static void sug_add(run_t *r, const char *text, int kind, const app_desc_t *app) {
    if (r->nsug >= MAX_SUG || sug_has(r, text)) return;
    sug_t *s = &r->sug[r->nsug++];
    strlcpy(s->text, text, sizeof s->text);
    s->kind = kind;
    s->app = app;
}

static void complete_path(run_t *r, const char *q) {
    char full[VFS_PATH_MAX], dir[VFS_PATH_MAX];
    cmd_resolve_path(NULL, q, full);
    const char *prefix;
    size_t ql = strlen(q);
    if (ql && q[ql - 1] == '/') { strlcpy(dir, full, sizeof dir); prefix = ""; }
    else { vfs_dirname(full, dir); prefix = vfs_basename(full); }
    if (!vfs_is_dir(dir)) return;
    int n = vfs_list(dir, NULL, 0);
    if (n <= 0) return;
    vfs_dirent_t *ents = kcalloc((size_t)n, sizeof *ents);
    if (!ents) return;
    n = MIN(vfs_list(dir, ents, n), n);
    size_t pl = strlen(prefix);
    for (int i = 0; i < n && r->nsug < MAX_SUG; i++) {
        if (pl && strncasecmp(ents[i].name, prefix, pl)) continue;
        char p[VFS_PATH_MAX];
        vfs_join(dir, ents[i].name, p);
        if (ents[i].is_dir) strlcat(p, "/", sizeof p);
        sug_add(r, p, ents[i].is_dir ? SUG_FOLDER : SUG_FILE, NULL);
    }
    kfree(ents);
}

static void build_suggestions(run_t *r) {
    hist_load();
    r->nsug = 0;
    r->sel = -1;
    char q[VFS_PATH_MAX];
    trim_copy(ui_get_text(r->tb), q, sizeof q);
    size_t ql = strlen(q);
    if (!ql) {
        for (int i = 0; i < nhist; i++) sug_add(r, hist[i], SUG_HISTORY, NULL);
        return;
    }
    if (q[0] == '/' || q[0] == '~') { complete_path(r, q); return; }
    /* history: prefix matches */
    int nh = 0;
    for (int i = 0; i < nhist && nh < 2; i++)
        if (!strncasecmp(hist[i], q, ql) && strcasecmp(hist[i], q)) { sug_add(r, hist[i], SUG_HISTORY, NULL); nh++; }
    /* apps: prefix first, then substring */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < app_count() && r->nsug < MAX_SUG; i++) {
            const app_desc_t *a = app_get(i);
            if (a->flags & APP_HIDDEN) continue;
            bool pre = !strncasecmp(a->name, q, ql);
            if ((pass == 0 && pre) || (pass == 1 && !pre && strcasestr(a->name, q))) sug_add(r, a->name, SUG_APP, a);
        }
    /* commands */
    char first[64];
    size_t fl = strcspn(q, " ");
    strlcpy(first, q, MIN(fl + 1, sizeof first));
    if (!q[fl]) {
        for (int i = 0; i < command_count() && r->nsug < MAX_SUG; i++) {
            const command_desc_t *c = command_get(i);
            if (!strncasecmp(c->name, first, fl)) sug_add(r, c->name, SUG_COMMAND, NULL);
        }
    }
    /* files */
    if (r->nsug < MAX_SUG && ql >= 2) {
        index_hit_t hits[MAX_SUG];
        int n = indexer_search(q, hits, MAX_SUG);
        for (int i = 0; i < n; i++) sug_add(r, hits[i].path, hits[i].is_dir ? SUG_FOLDER : SUG_FILE, NULL);
    }
}

static rect_t list_rect(window_t *w) { return rect_make(70, LIST_Y, win_width(w) - 88, MAX_SUG * SUG_H); }

static int sug_at(window_t *w, run_t *r, int x, int y) {
    rect_t lr = list_rect(w);
    if (!rect_contains(lr, x, y)) return -1;
    int i = (y - lr.y) / SUG_H;
    return i < r->nsug ? i : -1;
}

static void run_go(window_t *win, run_t *r) {
    char text[VFS_PATH_MAX], err[200];
    strlcpy(text, ui_get_text(r->tb), sizeof text);
    if (shell_run(text, err, sizeof err)) {
        if (win_is_valid(win)) win_close(win);
        return;
    }
    ui_message_box(win, "Run", err, MB_OK | MB_ICON_WARNING, NULL, NULL);
}

static void pick(window_t *win, run_t *r, int i) {
    if (i < 0 || i >= r->nsug) return;
    r->sel = i;
    ui_set_text(r->tb, r->sug[i].text);
    win_invalidate(win);
}

static void on_edit(widget_t *w, void *ctx) {
    run_t *r = ctx;
    build_suggestions(r);
    win_invalidate(ui_window(w));
}

static void on_submit(widget_t *w, void *ctx) { run_go(ui_window(w), ctx); }
static void btn_ok(widget_t *w, void *ctx) { run_go(ui_window(w), ctx); }
static void btn_cancel(widget_t *w, void *ctx) { (void)ctx; win_close(ui_window(w)); }

static void browse_done(const char *path, void *ctx) {
    run_t *r = ctx;
    if (!path || !run_win || win_get_userdata(run_win) != r) return;
    ui_set_text(r->tb, path);
    build_suggestions(r);
    ui_focus(r->tb);
    win_invalidate(run_win);
}

static void btn_browse(widget_t *w, void *ctx) {
    ui_file_dialog(ui_window(w), UI_FILE_OPEN, "Browse", "/home/user", NULL, browse_done, ctx);
}

static const char *kind_label(const sug_t *s) {
    switch (s->kind) {
    case SUG_HISTORY: return "Recent";
    case SUG_APP: return "App";
    case SUG_COMMAND: return "Command";
    case SUG_FOLDER: return "Folder";
    default: return "File";
    }
}

static void run_paint(window_t *win, run_t *r) {
    surface_t *s = win_surface(win);
    const theme_t *t = theme_get();
    icon_draw(ICON_RUN, s, 18, 16, 32);
    gfx_text_wrapped(s, rect_make(66, 16, s->w - 84, 40),
                     "Type the name of an app, a file or folder, or a command, and NovaOS will open it for you.",
                     t->window_text, FONT_UI);
    gfx_text(s, 18, 70, "Open:", t->window_text, FONT_UI);
    rect_t lr = list_rect(win);
    uint32_t dim = color_mix(t->window_text, t->window_bg, 110);
    if (!r->nsug) {
        const char *msg = ui_get_text(r->tb)[0] ? "No suggestions - press Enter to run it anyway" : "Recently run items appear here";
        gfx_text(s, lr.x + 8, lr.y + 6, msg, dim, FONT_UI);
    }
    for (int i = 0; i < r->nsug; i++) {
        const sug_t *g = &r->sug[i];
        int y = lr.y + i * SUG_H;
        if (i == r->sel) gfx_fill_rounded_rect(s, lr.x, y + 1, lr.w, SUG_H - 2, 5, color_mix(t->window_bg, t->accent, t->dark ? 90 : 55));
        else if (i == r->hover) gfx_fill_rounded_rect(s, lr.x, y + 1, lr.w, SUG_H - 2, 5, color_mix(t->window_bg, t->window_text, 18));
        int ix = lr.x + 6, iy = y + (SUG_H - 16) / 2;
        if (g->kind == SUG_APP && g->app) app_draw_icon(s, g->app, ix, iy, 16);
        else if (g->kind == SUG_COMMAND) icon_draw(ICON_TERMINAL, s, ix, iy, 16);
        else if (g->kind == SUG_HISTORY) {
            const app_desc_t *a = app_find(g->text);
            if (a) app_draw_icon(s, a, ix, iy, 16);
            else icon_draw(g->text[0] == '/' ? icon_for_path(g->text) : ICON_CLOCK, s, ix, iy, 16);
        } else icon_draw(icon_for_path(g->text), s, ix, iy, 16);
        const char *lbl = kind_label(g);
        int lw = font_text_width(FONT_UI, lbl);
        gfx_text_ellipsis(s, ix + 24, y + (SUG_H - font_height(FONT_UI)) / 2, lr.w - 44 - lw, g->text, t->window_text, FONT_UI);
        gfx_text(s, lr.x + lr.w - lw - 8, y + (SUG_H - font_height(FONT_UI)) / 2, lbl, dim, FONT_UI);
    }
    int bh = 60;
    gfx_fill_rect(s, 0, s->h - bh, s->w, bh, t->dark ? color_shade(t->window_bg, -12) : color_shade(t->window_bg, -6));
    gfx_hline(s, 0, s->h - bh, s->w, color_mix(t->window_bg, t->window_border, 70));
}

static void run_event(window_t *win, const event_t *ev) {
    run_t *r = win_get_userdata(win);
    if (!r) return;
    switch (ev->type) {
    case EV_PAINT:
        run_paint(win, r);
        break;
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESCAPE) { win_close(win); break; }
        if (ev->key == KEY_DOWN && r->nsug) pick(win, r, r->sel < 0 ? 0 : (r->sel + 1) % r->nsug);
        else if (ev->key == KEY_UP && r->nsug) pick(win, r, r->sel <= 0 ? r->nsug - 1 : r->sel - 1);
        break;
    case EV_MOUSE_MOVE: {
        int h = sug_at(win, r, ev->x, ev->y);
        if (h != r->hover) { r->hover = h; win_invalidate_rect(win, 0, LIST_Y, win_width(win), MAX_SUG * SUG_H); }
        break;
    }
    case EV_MOUSE_LEAVE:
        if (r->hover >= 0) { r->hover = -1; win_invalidate(win); }
        break;
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_LEFT) {
            int i = sug_at(win, r, ev->x, ev->y);
            if (i >= 0) {
                bool folder = r->sug[i].kind == SUG_FOLDER && r->sug[i].text[0] == '/';
                pick(win, r, i);
                ui_focus(r->tb);
                if (folder) { build_suggestions(r); win_invalidate(win); }   /* keep completing inside it */
            }
        }
        break;
    case EV_DOUBLE_CLICK:
        if (sug_at(win, r, ev->x, ev->y) >= 0) run_go(win, r);
        break;
    case EV_CLOSE:
        if (run_win == win) run_win = NULL;
        kfree(r);
        win_set_userdata(win, NULL);
        break;
    default: break;
    }
}

/* A hidden "Run" app gives the dialog (and command output windows) an icon in
   the taskbar; launching it by name opens the dialog. */
static void run_app_launch(const char *arg) { (void)arg; shell_run_dialog(); }
static void run_app_icon(surface_t *s, int x, int y, int size) { icon_render(ICON_RUN, s, x, y, size); }
REGISTER_APP(.name = "Run", .category = CAT_SYSTEM, .description = "Open an app, file, folder or command by name",
             .launch = run_app_launch, .draw_icon = run_app_icon, .flags = APP_HIDDEN | APP_SINGLE);

const app_desc_t *shell_run_app(void) { return app_find("Run"); }

window_t *shell_run_dialog(void) {
    if (run_win && win_is_valid(run_win)) { win_focus(run_win); return run_win; }
    run_t *r = kcalloc(1, sizeof *r);
    if (!r) return NULL;
    r->sel = r->hover = -1;
    const app_desc_t *saved = wm_current_app;
    wm_current_app = shell_run_app();
    window_t *w = win_create("Run", RUN_W, RUN_H, WIN_NO_MAXIMIZE | WIN_NO_MINIMIZE | WIN_HIDDEN);
    wm_current_app = saved;
    if (!w) { kfree(r); return NULL; }
    run_win = w;
    win_set_userdata(w, r);
    win_set_event_handler(w, run_event);
    rect_t f = win_frame_rect(w);
    win_move(w, 12, screen_height() - wm_taskbar_h - f.h - 12);
    r->tb = ui_textbox(w, 70, 64, RUN_W - 88, 30, "");
    ui_textbox_set_placeholder(r->tb, "e.g. Notepad, /home/user/Documents, notify Hi there");
    ui_set_callback(r->tb, on_edit, r);
    ui_textbox_on_submit(r->tb, on_submit, r);
    widget_t *ok = ui_button(w, RUN_W - 16 - 96 * 3 - 16, RUN_H - 46, 96, 32, "OK", btn_ok, r);
    ui_button_set_default(ok, true);
    ui_button(w, RUN_W - 16 - 96 * 2 - 8, RUN_H - 46, 96, 32, "Cancel", btn_cancel, r);
    ui_button(w, RUN_W - 16 - 96, RUN_H - 46, 96, 32, "Browse...", btn_browse, r);
    build_suggestions(r);
    ui_focus(r->tb);
    win_show(w);
    return w;
}

static int cmd_run(int argc, char **argv, cmd_writer_t *out) {
    if (argc < 2) { shell_run_dialog(); return 0; }
    char line[VFS_PATH_MAX] = "", err[200];
    for (int i = 1; i < argc; i++) { if (i > 1) strlcat(line, " ", sizeof line); strlcat(line, argv[i], sizeof line); }
    if (command_find(argv[1])) { cmd_puts(out, "run: use the command directly\n"); return 2; }
    if (!shell_run(line, err, sizeof err)) { cmd_printf(out, "run: %s\n", err); return 1; }
    return 0;
}
REGISTER_COMMAND("run", "run [TEXT] - open the Run dialog, or open an app / file / folder like it does", cmd_run);
