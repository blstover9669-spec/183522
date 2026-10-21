/*
 * utils_taskmgr.c - Task Manager (System). Also what Ctrl+Alt+Del opens.
 *
 * Tabs:
 *   Windows      - every open app window (icon, title, app, state, size) with
 *                  Switch to / End task / Run new task.
 *   Apps         - every installed app with its category, open window count and
 *                  description; Launch.
 *   Performance  - live heap memory graph and a CPU / activity graph.
 *
 * About the activity graph (honest measurement): the System Monitor service
 * (sysmon) samples once a second how much of the time the main loop spent
 * halted waiting for interrupts (idle) and how many input/window events it
 * handled. While that service runs, the graph shows its CPU busy share
 * (100 - idle %) with events per second overlaid, and the last 60 s of
 * history appear at once when the window opens.
 *
 * If the service is stopped (Services app), the window falls back to its own
 * estimate: a 1 ms probe timer counts how many 1 ms PIT ticks the loop actually
 * serviced, and ticks it missed because it was busy count as load. Work
 * shorter than 1 ms between ticks is invisible to that method, so it
 * under-reports light load; it is labelled accordingly. The probe only runs
 * in that fallback, because waking every millisecond would itself inflate the
 * CPU figure sysmon measures.
 */
#include <nova.h>

#define TM_W 660
#define TM_H 500
#define MAX_ROWS 128
#define HIST 60
#define ROW_H 28
#define HEAD_H 28
#define STATUS_H 28

enum { TAB_WINDOWS, TAB_APPS, TAB_PERF };

typedef struct {
    window_t *win;
    int tab;
    widget_t *tabs, *table;
    widget_t *b_switch, *b_end, *b_run, *b_launch;
    /* windows tab: window ids (stable, never reused) top of z-order first */
    int win_ids[MAX_ROWS];
    int nwin;
    int sel[2], scroll[2];
    int hover_row;
    int pending_end_id;
    /* sampling */
    int probe_timer, sample_timer;   /* probe_timer: 0 unless in fallback mode */
    uint32_t probes, last_sample_ms;
    bool use_sysmon;           /* history comes from the System Monitor service */
    uint32_t probe_off_ms;     /* when the probe was last stopped (0 = never ran)  */
    uint32_t svc_gen;          /* service_generation() when we last looked        */
    float mem_hist[HIST];      /* heap bytes in use, MiB                        */
    float load_hist[HIST];     /* 0..100: CPU busy (sysmon) or missed ticks     */
    float ev_hist[HIST];       /* events per second (sysmon only)               */
    int nhist;
    float cur_load;
    int cur_events;
} tm_t;

/* ---- helpers ----------------------------------------------------------------------- */
static uint32_t muted(const theme_t *t) { return color_mix(t->window_text, t->window_bg, 110); }

static void fmt_bytes(uint32_t b, char *out, int n) {
    if (b >= 1024u * 1024u) snprintf(out, n, "%.1f MiB", b / (1024.0 * 1024.0));
    else if (b >= 1024u) snprintf(out, n, "%u KiB", b / 1024u);
    else snprintf(out, n, "%u B", b);
}

static void fmt_uptime(char *out, int n) {
    uint32_t s = sys_uptime_ms() / 1000;
    if (s >= 86400) snprintf(out, n, "%ud %02u:%02u:%02u", s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);
    else snprintf(out, n, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
}

static int row_count(tm_t *c) { return c->tab == TAB_WINDOWS ? c->nwin : (c->tab == TAB_APPS ? app_count() : 0); }

static void refresh_windows(tm_t *c) {
    window_t *list[MAX_ROWS];
    int n = win_list(list, MAX_ROWS);
    if (n > MAX_ROWS) n = MAX_ROWS;
    int old_sel_id = (c->sel[TAB_WINDOWS] >= 0 && c->sel[TAB_WINDOWS] < c->nwin) ? c->win_ids[c->sel[TAB_WINDOWS]] : -1;
    c->nwin = 0;
    for (int i = n - 1; i >= 0; i--) c->win_ids[c->nwin++] = win_id(list[i]);   /* topmost first */
    c->sel[TAB_WINDOWS] = -1;
    for (int i = 0; i < c->nwin; i++) if (c->win_ids[i] == old_sel_id) c->sel[TAB_WINDOWS] = i;
    int vis = MAX(1, (ui_rect(c->table).h - HEAD_H) / ROW_H);
    c->scroll[TAB_WINDOWS] = CLAMP(c->scroll[TAB_WINDOWS], 0, MAX(0, c->nwin - vis));
}

static window_t *selected_window(tm_t *c) {
    int s = c->sel[TAB_WINDOWS];
    if (s < 0 || s >= c->nwin) return NULL;
    return win_find_by_id(c->win_ids[s]);
}

static void update_buttons(tm_t *c) {
    bool w = c->tab == TAB_WINDOWS, a = c->tab == TAB_APPS;
    ui_set_visible(c->table, w || a);
    ui_set_visible(c->b_switch, w);
    ui_set_visible(c->b_end, w);
    ui_set_visible(c->b_run, w);
    ui_set_visible(c->b_launch, a);
    ui_set_enabled(c->b_switch, selected_window(c) != NULL);
    ui_set_enabled(c->b_end, selected_window(c) != NULL);
    ui_set_enabled(c->b_launch, c->sel[TAB_APPS] >= 0 && c->sel[TAB_APPS] < app_count());
}

/* ---- actions ------------------------------------------------------------------------ */
static void do_switch(tm_t *c) {
    window_t *w = selected_window(c);
    if (w) win_focus(w);
}

static void end_confirmed(int result, void *ctx) {
    tm_t *c = ctx;
    if (result != MB_RESULT_YES) return;
    window_t *w = win_find_by_id(c->pending_end_id);
    if (w && w != c->win) win_close(w);
    refresh_windows(c);
    update_buttons(c);
    win_invalidate(c->win);
}

static void do_end(tm_t *c) {
    window_t *w = selected_window(c);
    if (!w) return;
    if (w == c->win) { win_close(c->win); return; }   /* ending ourselves: nothing may touch c after this */
    if (win_flags(w) & WIN_CONFIRM_CLOSE) {
        /* the app wanted to confirm closing (unsaved changes?) - ask on its behalf */
        char msg[200];
        snprintf(msg, sizeof msg, "\"%s\" may have unsaved changes.\n\nEnd the task anyway?", win_get_title(w));
        c->pending_end_id = win_id(w);
        ui_message_box(c->win, "End task", msg, MB_YESNO | MB_ICON_WARNING, end_confirmed, c);
        return;
    }
    win_close(w);
    sys_beep(520, 30);
    refresh_windows(c);
    update_buttons(c);
    win_invalidate(c->win);
}

static void run_done(const char *text, void *ctx) {
    tm_t *c = ctx;
    if (!text || !*text) return;
    if (!app_launch_by_name(text, NULL)) {
        char msg[160];
        snprintf(msg, sizeof msg, "There is no app called \"%s\".", text);
        ui_message_box(c->win, "Run new task", msg, MB_OK | MB_ICON_ERROR, NULL, NULL);
    }
}

static void do_launch(tm_t *c) {
    int s = c->sel[TAB_APPS];
    if (s >= 0 && s < app_count()) app_launch(app_get(s), NULL);
}

static void on_switch(widget_t *w, void *ctx) { (void)w; do_switch(ctx); }
static void on_end(widget_t *w, void *ctx) { (void)w; do_end(ctx); }
static void on_run(widget_t *w, void *ctx) {
    ui_input_box(ui_window(w), "Run new task", "Type the name of an app (for example Notepad):", "", run_done, ctx);
}
static void on_launch(widget_t *w, void *ctx) { (void)w; do_launch(ctx); }

/* ---- table widget ------------------------------------------------------------------- */
typedef struct { const char *title; int w; } col_t;   /* w <= 0: share the rest */

static int columns(tm_t *c, int total, col_t *cols, int *x) {
    int n;
    if (c->tab == TAB_WINDOWS) {
        cols[0] = (col_t){ "Window", 0 }; cols[1] = (col_t){ "App", 130 };
        cols[2] = (col_t){ "Status", 100 }; cols[3] = (col_t){ "Size", 90 };
        n = 4;
    } else {
        cols[0] = (col_t){ "Name", 160 }; cols[1] = (col_t){ "Category", 110 };
        cols[2] = (col_t){ "Windows", 84 }; cols[3] = (col_t){ "Description", 0 };
        n = 4;
    }
    int fixed = 0;
    for (int i = 0; i < n; i++) fixed += cols[i].w > 0 ? cols[i].w : 0;
    int rest = MAX(60, total - fixed);
    int px = 0;
    for (int i = 0; i < n; i++) {
        x[i] = px;
        px += cols[i].w > 0 ? cols[i].w : rest;
    }
    x[n] = px;
    return n;
}

static void cell_text(tm_t *c, int row, int col, char *buf, int n, const app_desc_t **icon_app) {
    buf[0] = 0;
    if (c->tab == TAB_WINDOWS) {
        window_t *w = win_find_by_id(c->win_ids[row]);
        if (!w) { if (col == 0) strlcpy(buf, "(closed)", n); return; }
        const app_desc_t *app = win_get_app(w);
        switch (col) {
        case 0: strlcpy(buf, win_get_title(w), n); if (icon_app) *icon_app = app; break;
        case 1: strlcpy(buf, app ? app->name : "System", n); break;
        case 2:
            if (win_is_minimized(w)) strlcpy(buf, "Minimized", n);
            else if (win_is_fullscreen(w)) strlcpy(buf, "Fullscreen", n);
            else if (win_is_focused(w)) strlcpy(buf, "Active", n);
            else if (win_is_maximized(w)) strlcpy(buf, "Maximized", n);
            else strlcpy(buf, "Running", n);
            break;
        case 3: snprintf(buf, n, "%d x %d", win_width(w), win_height(w)); break;
        }
    } else {
        const app_desc_t *app = app_get(row);
        if (!app) return;
        switch (col) {
        case 0: strlcpy(buf, app->name, n); if (icon_app) *icon_app = app; break;
        case 1: strlcpy(buf, app->category ? app->category : "", n); break;
        case 2: {
            int k = app_window_count(app);
            if (k) snprintf(buf, n, "%d", k);
            else strlcpy(buf, "-", n);
            break;
        }
        case 3: strlcpy(buf, app->description ? app->description : "", n); break;
        }
    }
}

static void table_draw(widget_t *wd, surface_t *s, rect_t r) {
    tm_t *c = ui_get_ctx(wd);
    const theme_t *t = theme_get();
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 8, t->window_border);
    gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 7, t->field_bg);
    /* header */
    gfx_fill_rect(s, r.x + 1, r.y + 8, r.w - 2, HEAD_H - 8, t->panel_bg);
    gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, HEAD_H, 7, t->panel_bg);
    gfx_hline(s, r.x + 1, r.y + HEAD_H, r.w - 2, color_mix(t->panel_bg, t->window_border, 110));
    col_t cols[6];
    int cx[7];
    int n = columns(c, r.w - 2, cols, cx);
    for (int i = 0; i < n; i++) {
        gfx_text(s, r.x + 1 + cx[i] + 10, r.y + 6, cols[i].title, muted(t), FONT_UI_BOLD);
        if (i) gfx_vline(s, r.x + 1 + cx[i], r.y + 6, HEAD_H - 12, color_mix(t->panel_bg, t->window_border, 90));
    }
    int rows = row_count(c);
    int tab = c->tab == TAB_APPS ? 1 : 0;
    int vis = (r.h - HEAD_H - 2) / ROW_H;
    rect_t saved_clip = s->clip;   /* restore before returning: later widgets share the surface */
    surface_set_clip(s, rect_intersect(s->clip, rect_make(r.x + 1, r.y + HEAD_H + 1, r.w - 2, r.h - HEAD_H - 2)));
    bool focused = ui_has_focus(wd) && win_is_focused(c->win);
    for (int k = 0; k <= vis && c->scroll[tab] + k < rows; k++) {
        int row = c->scroll[tab] + k;
        int y = r.y + HEAD_H + 1 + k * ROW_H;
        bool sel = row == c->sel[tab];
        uint32_t fg = t->field_text;
        if (sel) {
            gfx_fill_rect(s, r.x + 1, y, r.w - 2, ROW_H, focused ? t->selection_bg : color_mix(t->field_bg, t->selection_bg, 90));
            if (focused) fg = t->selection_text;
        } else if (row == c->hover_row) {
            gfx_fill_rect(s, r.x + 1, y, r.w - 2, ROW_H, color_mix(t->field_bg, t->field_text, 14));
        }
        for (int col = 0; col < n; col++) {
            char buf[160];
            const app_desc_t *icon = NULL;
            cell_text(c, row, col, buf, sizeof buf, &icon);
            int x = r.x + 1 + cx[col] + 10;
            int maxw = cx[col + 1] - cx[col] - 18;
            if (col == 0) {
                if (icon) app_draw_icon(s, icon, x, y + (ROW_H - 18) / 2, 18);
                x += 26; maxw -= 26;
            }
            uint32_t tc = fg;
            if (!sel && col > 0) tc = color_mix(t->field_text, t->field_bg, 60);
            if (!sel && c->tab == TAB_WINDOWS && col == 2 && !strcmp(buf, "Active")) tc = t->accent;
            gfx_text_ellipsis(s, x, y + (ROW_H - font_height(FONT_UI)) / 2, maxw, buf, tc, col == 0 ? FONT_UI_BOLD : FONT_UI);
        }
    }
    if (!rows) gfx_text_aligned(s, rect_make(r.x, r.y + HEAD_H + 20, r.w, 20), "Nothing here", muted(t), FONT_UI, ALIGN_CENTER);
    /* scroll indicator */
    if (rows > vis && vis > 0) {
        int th = MAX(20, (r.h - HEAD_H - 8) * vis / rows);
        int ty = r.y + HEAD_H + 4 + (r.h - HEAD_H - 8 - th) * c->scroll[tab] / MAX(1, rows - vis);
        gfx_fill_rounded_rect(s, r.x + r.w - 7, ty, 4, th, 2, t->scrollbar_thumb);
    }
    surface_set_clip(s, saved_clip);
}

static void ensure_visible(tm_t *c, int tab) {
    int vis = MAX(1, (ui_rect(c->table).h - HEAD_H - 2) / ROW_H);
    if (c->sel[tab] < c->scroll[tab]) c->scroll[tab] = c->sel[tab];
    if (c->sel[tab] >= c->scroll[tab] + vis) c->scroll[tab] = c->sel[tab] - vis + 1;
    if (c->scroll[tab] < 0) c->scroll[tab] = 0;
}

static int table_row_at(tm_t *c, int y) {
    rect_t r = ui_rect(c->table);
    int tab = c->tab == TAB_APPS ? 1 : 0;
    if (y < r.y + HEAD_H + 1) return -1;
    int row = c->scroll[tab] + (y - r.y - HEAD_H - 1) / ROW_H;
    return row < row_count(c) ? row : -1;
}

/* Right-click menu on a row (the popup is owned by our window, so c is alive). */
static void row_menu(int item, void *ctx) {
    tm_t *c = ctx;
    if (c->tab == TAB_WINDOWS) {
        if (item == 0) do_switch(c);
        else if (item == 2) do_end(c);
    } else if (c->tab == TAB_APPS && item == 0) {
        do_launch(c);
    }
}

static bool table_event(widget_t *wd, const event_t *ev) {
    tm_t *c = ui_get_ctx(wd);
    int tab = c->tab == TAB_APPS ? 1 : 0;
    int rows = row_count(c);
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int h = table_row_at(c, ev->y);
        if (h != c->hover_row) { c->hover_row = h; ui_invalidate(wd); }
        return true;
    }
    case EV_MOUSE_DOWN: {
        int row = table_row_at(c, ev->y);
        if (row >= 0) c->sel[tab] = row;
        update_buttons(c);
        ui_invalidate(wd);
        if (row >= 0 && ev->button == MOUSE_RIGHT) {
            if (c->tab == TAB_WINDOWS)
                ui_popup_menu(c->win, ev->x, ev->y, "Switch to|-|End task\tDel", row_menu, c);
            else
                ui_popup_menu(c->win, ev->x, ev->y, "Launch", row_menu, c);
        }
        return true;
    }
    case EV_DOUBLE_CLICK:
        if (table_row_at(c, ev->y) >= 0) {
            if (c->tab == TAB_WINDOWS) do_switch(c);
            else do_launch(c);
        }
        return true;
    case EV_MOUSE_UP:
        return true;
    case EV_MOUSE_WHEEL: {
        int vis = MAX(1, (ui_rect(wd).h - HEAD_H - 2) / ROW_H);
        c->scroll[tab] = CLAMP(c->scroll[tab] + ev->wheel * 2, 0, MAX(0, rows - vis));
        ui_invalidate(wd);
        return true;
    }
    case EV_KEY_DOWN: {
        int k = ev->key, s = c->sel[tab];
        int vis = MAX(1, (ui_rect(wd).h - HEAD_H - 2) / ROW_H);
        if (!rows) return false;
        if (k == KEY_DOWN) s = s < 0 ? 0 : s + 1;
        else if (k == KEY_UP) s = s < 0 ? 0 : s - 1;
        else if (k == KEY_PAGEDOWN) s += vis;
        else if (k == KEY_PAGEUP) s -= vis;
        else if (k == KEY_HOME) s = 0;
        else if (k == KEY_END) s = rows - 1;
        else if (k == KEY_ENTER) { if (c->tab == TAB_WINDOWS) do_switch(c); else do_launch(c); return true; }
        else if (k == KEY_DELETE && c->tab == TAB_WINDOWS) { do_end(c); return true; }
        else return false;
        c->sel[tab] = CLAMP(s, 0, rows - 1);
        ensure_visible(c, tab);
        update_buttons(c);
        ui_invalidate(wd);
        return true;
    }
    default:
        return false;
    }
}

/* ---- performance tab ---------------------------------------------------------------- */
static double graph_x(rect_t r, int n, int i) { return r.x + 2 + (HIST - n + i) * ((double)(r.w - 4) / (HIST - 1)); }
static double graph_y(rect_t r, float val, float maxv) {
    double v = val / maxv;
    if (v > 1) v = 1;
    if (v < 0) v = 0;
    return r.y + r.h - 3 - v * (r.h - 6);
}

/* Area graph of vals (0..maxv) in r, newest sample on the right. If ov is not
   NULL it is drawn on top as a thin line on its own scale (0..ovmax). */
static void draw_graph(surface_t *s, rect_t r, const float *vals, int n, float maxv, uint32_t col,
                       const float *ov, float ovmax, uint32_t ovcol) {
    const theme_t *t = theme_get();
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, color_mix(t->panel_bg, t->window_border, 60));
    gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 5, t->field_bg);
    for (int i = 1; i < 4; i++) gfx_hline(s, r.x + 2, r.y + r.h * i / 4, r.w - 4, color_mix(t->field_bg, t->window_border, 50));
    for (int i = 1; i < 6; i++) gfx_vline(s, r.x + r.w * i / 6, r.y + 2, r.h - 4, color_mix(t->field_bg, t->window_border, 30));
    if (n < 1 || maxv <= 0) return;
    double px = 0, py = 0;
    for (int i = 0; i < n; i++) {
        double x = graph_x(r, n, i), y = graph_y(r, vals[i], maxv);
        /* filled column under the line */
        int x0 = (int)(i ? px : x), x1 = (int)x;
        for (int xx = i ? x0 + 1 : x0; xx <= x1; xx++) {   /* +1: don't fill a column twice */
            double f = x1 > x0 ? (double)(xx - x0) / (x1 - x0) : 1;
            int yy = (int)(i ? py + (y - py) * f : y);
            gfx_vline(s, xx, yy, r.y + r.h - 2 - yy, color_alpha(col, 46));
        }
        if (i) {
            gfx_line_aa(s, px, py, x, y, col);
            gfx_line_aa(s, px, py - 1, x, y - 1, col);
        }
        px = x; py = y;
    }
    gfx_fill_circle_aa(s, px, py, 3, col);
    if (ov && ovmax > 0) {
        for (int i = 1; i < n; i++)
            gfx_line_aa(s, graph_x(r, n, i - 1), graph_y(r, ov[i - 1], ovmax), graph_x(r, n, i), graph_y(r, ov[i], ovmax), ovcol);
        gfx_fill_circle_aa(s, graph_x(r, n, n - 1), graph_y(r, ov[n - 1], ovmax), 2, ovcol);
    }
}

/* Smallest "nice" value (10, 20, 50, 100, 200, ...) >= v. */
static float nice_ceil(float v) {
    float m = 10;
    for (;;) {
        if (m >= v) return m;
        if (m * 2 >= v) return m * 2;
        if (m * 5 >= v) return m * 5;
        m *= 10;
    }
}

/* A small coloured square and a label; returns the x after it. */
static int legend(surface_t *s, int x, int y, uint32_t col, const char *label, uint32_t tc) {
    gfx_fill_rounded_rect(s, x, y + (font_height(FONT_UI) - 10) / 2, 10, 10, 2, col);
    return x + 15 + gfx_text(s, x + 15, y, label, tc, FONT_UI) + 14;
}

static void paint_perf(tm_t *c, surface_t *s) {
    const theme_t *t = theme_get();
    int W = s->w, H = s->h;
    char buf[96], b2[32], b3[32];
    int x = 12, w = W - 24;
    int top = 52, bottom = H - STATUS_H - 8;
    int card_h = (bottom - top - 10) / 2;
    uint32_t mem_col = RGB(150, 90, 230), load_col = t->accent;

    /* memory card */
    rect_t m = rect_make(x, top, w, card_h);
    ui_draw_panel(s, m);
    uint32_t used = sys_mem_used(), heap = sys_heap_size();
    gfx_text(s, m.x + 16, m.y + 12, "Memory", t->window_text, FONT_UI_LARGE);
    fmt_bytes(used, b2, sizeof b2);
    gfx_text(s, m.x + 16, m.y + 40, b2, mem_col, FONT_UI_TITLE);
    fmt_bytes(heap, b3, sizeof b3);
    snprintf(buf, sizeof buf, "in use of %s heap", b3);
    gfx_text(s, m.x + 16, m.y + 78, buf, muted(t), FONT_UI);
    fmt_bytes(heap > used ? heap - used : 0, b3, sizeof b3);
    snprintf(buf, sizeof buf, "Free: %s", b3);
    gfx_text(s, m.x + 16, m.y + 96, buf, muted(t), FONT_UI);
    fmt_bytes(sys_mem_total(), b3, sizeof b3);
    snprintf(buf, sizeof buf, "RAM: %s", b3);
    gfx_text(s, m.x + 16, m.y + 114, buf, muted(t), FONT_UI);
    /* usage bar */
    int bw = 150, by = m.y + m.h - 24;
    double frac = heap ? (double)used / heap : 0;
    gfx_fill_rounded_rect(s, m.x + 16, by, bw, 8, 4, color_mix(t->panel_bg, t->window_text, 26));
    gfx_fill_rounded_rect(s, m.x + 16, by, MAX(8, (int)(bw * frac)), 8, 4, mem_col);
    snprintf(buf, sizeof buf, "%.1f%%", frac * 100);
    gfx_text(s, m.x + 22 + bw, by - 5, buf, muted(t), FONT_UI);
    /* graph auto-scales to a round number of MiB above the peak */
    float peak = 1;
    for (int i = 0; i < c->nhist; i++) if (c->mem_hist[i] > peak) peak = c->mem_hist[i];
    float scale = 4;
    while (scale < peak * 1.25f) scale *= 2;
    rect_t g = rect_make(m.x + 220, m.y + 30, m.w - 236, m.h - 46);
    draw_graph(s, g, c->mem_hist, c->nhist, scale, mem_col, NULL, 0, 0);
    snprintf(buf, sizeof buf, "%.0f MiB", scale);
    gfx_text_aligned(s, rect_make(g.x, m.y + 10, g.w, 18), buf, muted(t), FONT_UI, ALIGN_RIGHT);
    gfx_text(s, g.x, m.y + 10, "Heap in use, last 60 s", muted(t), FONT_UI);

    /* activity card */
    rect_t a = rect_make(x, top + card_h + 10, w, card_h);
    ui_draw_panel(s, a);
    int ly = a.y + 78;
    gfx_text(s, a.x + 16, a.y + 12, c->use_sysmon ? "CPU" : "Event loop", t->window_text, FONT_UI_LARGE);
    bool waiting = c->use_sysmon && !c->nhist;   /* service just started: no sample yet */
    if (waiting) strlcpy(buf, "--", sizeof buf);
    else snprintf(buf, sizeof buf, "%.0f%%", c->cur_load);
    gfx_text(s, a.x + 16, a.y + 40, buf, load_col, FONT_UI_TITLE);
    gfx_text(s, a.x + 16, ly, waiting ? "waiting for a sample" : c->use_sysmon ? "busy (not idle) time" : "load (missed 1 ms ticks)",
             muted(t), FONT_UI);
    ly += 18;
    if (waiting) strlcpy(buf, "Events: --", sizeof buf);
    else if (c->use_sysmon) snprintf(buf, sizeof buf, "Events: %d per second", c->cur_events);
    else strlcpy(buf, "Events: n/a (sysmon off)", sizeof buf);
    gfx_text_ellipsis(s, a.x + 16, ly, 196, buf, muted(t), FONT_UI);
    ly += 18;
    snprintf(buf, sizeof buf, "Screen updates: %d fps", sys_fps());
    gfx_text(s, a.x + 16, ly, buf, muted(t), FONT_UI);
    ly += 18;
    fmt_uptime(b2, sizeof b2);
    snprintf(buf, sizeof buf, "Uptime: %s", b2);
    gfx_text(s, a.x + 16, ly, buf, muted(t), FONT_UI);
    ly += 18;
    window_t *list[1];
    int nw = win_list(list, 0);
    snprintf(buf, sizeof buf, "%d window%s, %d apps", nw, nw == 1 ? "" : "s", app_count());
    if (ly + font_height(FONT_UI) <= a.y + a.h - 8) gfx_text(s, a.x + 16, ly, buf, muted(t), FONT_UI);

    rect_t g2 = rect_make(a.x + 220, a.y + 30, a.w - 236, a.h - 46);
    if (c->use_sysmon) {
        /* events/s on its own round scale above the busiest second shown */
        float evmax = 0;
        for (int i = 0; i < c->nhist; i++) if (c->ev_hist[i] > evmax) evmax = c->ev_hist[i];
        evmax = nice_ceil(evmax * 1.1f);
        uint32_t ev_col = RGB(240, 140, 40);
        draw_graph(s, g2, c->load_hist, c->nhist, 100, load_col, c->ev_hist, evmax, ev_col);
        int lx = legend(s, g2.x, a.y + 10, load_col, "CPU busy", muted(t));
        legend(s, lx, a.y + 10, ev_col, "Events/s", muted(t));
        snprintf(buf, sizeof buf, "100%%  |  %.0f/s", evmax);
        gfx_text_aligned(s, rect_make(g2.x, a.y + 10, g2.w, 18), buf, muted(t), FONT_UI, ALIGN_RIGHT);
    } else {
        draw_graph(s, g2, c->load_hist, c->nhist, 100, load_col, NULL, 0, 0);
        gfx_text(s, g2.x, a.y + 10, "Share of ticks the loop was busy", muted(t), FONT_UI);
        gfx_text_aligned(s, rect_make(g2.x, a.y + 10, g2.w, 18), "100%", muted(t), FONT_UI, ALIGN_RIGHT);
    }
}

/* ---- window --------------------------------------------------------------------------- */
static void layout(tm_t *c) {
    int W = win_width(c->win), H = win_height(c->win);
    ui_move(c->tabs, 12, 8, W - 24, 34);
    int by = H - STATUS_H - 44;
    ui_move(c->table, 12, 52, W - 24, by - 52 - 8);
    ui_move(c->b_end, W - 12 - 110, by, 110, 32);
    ui_move(c->b_switch, W - 12 - 110 - 8 - 110, by, 110, 32);
    ui_move(c->b_run, 12, by, 140, 32);
    ui_move(c->b_launch, W - 12 - 110, by, 110, 32);
}

static void show_tab(tm_t *c, int tab) {
    c->tab = tab;
    c->hover_row = -1;
    if (tab == TAB_WINDOWS) refresh_windows(c);
    update_buttons(c);
    if (tab != TAB_PERF) ui_focus(c->table);
    win_invalidate(c->win);
}

static void on_tab(widget_t *w, void *ctx) { show_tab(ctx, ui_get_value(w)); }

/* Use the System Monitor service whenever it runs - even before its first
   sample, so the probe never adds its own 1000 timer events/s to that sample. */
static bool sysmon_ok(void) { return service_is_running("sysmon"); }

/* Copy the last 60 s of sysmon samples. Samples older than that, or from an
   earlier run of the service (stopped and started again since), are left out
   so the graph never stretches stale data across the time axis. */
static void pull_sysmon(tm_t *c) {
    sysmon_sample_t h[HIST];
    int n = sysmon_history(h, HIST);
    uint32_t now = sys_uptime_ms();
    service_status_t st;
    uint32_t started = service_status("sysmon", &st) == 0 ? st.started_ms : 0;
    int first = 0;
    for (int i = 0; i < n; i++) {
        int32_t since_probe = (int32_t)(h[i].uptime_ms - c->probe_off_ms);
        if (now - h[i].uptime_ms > HIST * 1000u + 500u) first = i + 1;        /* too old       */
        if ((int32_t)(h[i].uptime_ms - started) <= 0) first = i + 1;           /* previous run  */
        /* this second overlapped our own fallback probe: it measured the probe */
        if (c->probe_off_ms && since_probe >= 0 && since_probe < 900) first = i + 1;
    }
    c->nhist = 0;
    for (int i = first; i < n; i++) {
        c->mem_hist[c->nhist] = h[i].mem_used / (1024.0f * 1024.0f);
        c->load_hist[c->nhist] = (float)CLAMP(h[i].cpu_percent, 0, 100);
        c->ev_hist[c->nhist] = (float)MAX(h[i].events_per_sec, 0);
        c->nhist++;
    }
    if (c->nhist) {
        c->cur_load = c->load_hist[c->nhist - 1];
        c->cur_events = (int)c->ev_hist[c->nhist - 1];
    }
}

/* Fallback: one sample from our own 1 ms probe timer. */
static void probe_sample(tm_t *c) {
    uint32_t now = sys_uptime_ms();
    uint32_t el = now - c->last_sample_ms;
    float load = 0;
    if (el > 0) {
        load = 100.0f * (1.0f - (float)c->probes / (float)el);
        if (load < 0) load = 0;
        if (load > 100) load = 100;
    }
    c->probes = 0;
    c->last_sample_ms = now;
    c->cur_load = load;
    if (c->nhist == HIST) {
        memmove(c->mem_hist, c->mem_hist + 1, sizeof(float) * (HIST - 1));
        memmove(c->load_hist, c->load_hist + 1, sizeof(float) * (HIST - 1));
        c->nhist--;
    }
    c->mem_hist[c->nhist] = sys_mem_used() / (1024.0f * 1024.0f);
    c->load_hist[c->nhist] = load;
    c->ev_hist[c->nhist] = 0;
    c->nhist++;
}

/* Pick the data source (again, if the service was started or stopped since). */
static void set_source(tm_t *c, bool sysmon) {
    c->use_sysmon = sysmon;
    c->nhist = 0;
    c->cur_load = 0;
    c->cur_events = 0;
    if (sysmon) {
        if (c->probe_timer > 0) {
            win_kill_timer(c->win, c->probe_timer);   /* only if > 0: 0 would kill all our timers */
            c->probe_off_ms = sys_uptime_ms();
        }
        c->probe_timer = 0;
        pull_sysmon(c);
    } else {
        if (c->probe_timer <= 0) c->probe_timer = win_set_timer(c->win, 1);
        c->probes = 0;
        c->last_sample_ms = sys_uptime_ms();
        c->mem_hist[0] = sys_mem_used() / (1024.0f * 1024.0f);   /* memory graph starts at once */
        c->load_hist[0] = 0;
        c->ev_hist[0] = 0;
        c->nhist = 1;
    }
}

static void take_sample(tm_t *c) {
    c->svc_gen = service_generation();
    bool sm = sysmon_ok();
    if (sm != c->use_sysmon) { set_source(c, sm); return; }
    if (sm) pull_sysmon(c);
    else probe_sample(c);
}

static void tm_event(window_t *win, const event_t *ev) {
    tm_t *c = win_get_userdata(win);
    if (!c) return;
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        const theme_t *t = theme_get();
        if (c->tab == TAB_PERF) paint_perf(c, s);
        /* status bar */
        int H = s->h, W = s->w;
        gfx_fill_rect(s, 0, H - STATUS_H, W, STATUS_H, t->panel_bg);
        gfx_hline(s, 0, H - STATUS_H, W, color_mix(t->panel_bg, t->window_border, 80));
        char up[32], used[24], buf[160];
        fmt_uptime(up, sizeof up);
        fmt_bytes(sys_mem_used(), used, sizeof used);
        window_t *dummy[1];
        snprintf(buf, sizeof buf, "Windows: %d    Apps: %d    Memory: %s    %s: %.0f%%    Uptime: %s",
                 win_list(dummy, 0), app_count(), used, c->use_sysmon ? "CPU" : "Load", c->cur_load, up);
        gfx_text_ellipsis(s, 12, H - STATUS_H + (STATUS_H - font_height(FONT_UI)) / 2, W - 24, buf, muted(t), FONT_UI);
        break;
    }
    case EV_TIMER:
        if (c->probe_timer > 0 && ev->timer_id == c->probe_timer) {
            c->probes++;
            /* services changed (cheap check): switch back within a tick if sysmon started */
            if (service_generation() != c->svc_gen) {
                c->svc_gen = service_generation();
                if (sysmon_ok()) {
                    set_source(c, true);
                    if (c->tab == TAB_PERF) win_invalidate(win);
                }
            }
            break;
        }
        if (ev->timer_id == c->sample_timer) {
            take_sample(c);
            if (win_is_minimized(win)) break;
            if (c->tab == TAB_WINDOWS) {
                refresh_windows(c);
                update_buttons(c);
                ui_invalidate(c->table);
            } else if (c->tab == TAB_APPS) {
                ui_invalidate(c->table);   /* window counts */
            } else {
                win_invalidate(win);
            }
            win_invalidate_rect(win, 0, win_height(win) - STATUS_H, win_width(win), STATUS_H);
        }
        break;
    case EV_RESIZE:
        layout(c);
        break;
    case EV_FOCUS:
    case EV_BLUR:
        if (c->tab == TAB_WINDOWS) refresh_windows(c);
        update_buttons(c);
        win_invalidate(win);
        break;
    case EV_MOUSE_MOVE:
    case EV_MOUSE_LEAVE:
        if (c->hover_row != -1) { c->hover_row = -1; ui_invalidate(c->table); }
        break;
    case EV_KEY_DOWN:
        if ((ev->mods & MOD_CTRL) && ev->key == KEY_TAB) {
            int nt = (c->tab + ((ev->mods & MOD_SHIFT) ? 2 : 1)) % 3;
            ui_set_value(c->tabs, nt);
            show_tab(c, nt);
        } else if ((ev->mods & MOD_CTRL) && ev->key >= '1' && ev->key <= '3') {
            ui_set_value(c->tabs, ev->key - '1');
            show_tab(c, ev->key - '1');
        } else if (ev->key == KEY_F5) {
            refresh_windows(c);
            win_invalidate(win);
        }
        break;
    case EV_CLOSE:
        win_set_userdata(win, NULL);
        free(c);
        break;
    default:
        break;
    }
}

static void tm_launch(const char *arg) {
    tm_t *c = calloc(1, sizeof *c);
    if (!c) return;
    window_t *win = win_create("Task Manager", TM_W, TM_H, WIN_RESIZABLE | WIN_CENTER);
    if (!win) { free(c); return; }
    c->win = win;
    c->sel[0] = c->sel[1] = -1;
    c->hover_row = -1;
    win_set_userdata(win, c);
    win_set_event_handler(win, tm_event);
    win_set_min_size(win, 560, 420);

    c->tabs = ui_tabs(win, 12, 8, TM_W - 24, 34, on_tab, c);
    ui_list_add(c->tabs, "Windows");
    ui_list_add(c->tabs, "Apps");
    ui_list_add(c->tabs, "Performance");
    c->table = ui_custom(win, 12, 52, 100, 100, table_draw, table_event, c);
    c->b_run = ui_button(win, 0, 0, 10, 10, "Run new task...", on_run, c);
    c->b_switch = ui_button(win, 0, 0, 10, 10, "Switch to", on_switch, c);
    c->b_end = ui_button(win, 0, 0, 10, 10, "End task", on_end, c);
    ui_button_set_default(c->b_end, true);
    c->b_launch = ui_button(win, 0, 0, 10, 10, "Launch", on_launch, c);
    ui_button_set_default(c->b_launch, true);
    layout(c);

    c->sample_timer = win_set_timer(win, 1000);
    c->svc_gen = service_generation();
    set_source(c, sysmon_ok());

    int tab = TAB_WINDOWS;
    if (arg && !strcasecmp(arg, "apps")) tab = TAB_APPS;
    else if (arg && (!strcasecmp(arg, "performance") || !strcasecmp(arg, "perf"))) tab = TAB_PERF;
    ui_set_value(c->tabs, tab);
    show_tab(c, tab);
}

/* Icon: a dark monitor-style tile with a green activity line and bar meters. */
static void tm_icon(surface_t *s, int x, int y, int size) {
    double u = size / 48.0;
    int bx = x + (int)(4 * u), by = y + (int)(6 * u), bw = (int)(40 * u), bh = (int)(36 * u);
    gfx_fill_rounded_rect(s, bx, by + (int)(1.5 * u), bw, bh, (int)(7 * u), RGBA(0, 0, 0, 55));
    gfx_fill_rounded_gradient_v(s, bx, by, bw, bh, (int)(7 * u), RGB(52, 60, 82), RGB(28, 32, 46));
    /* bars */
    static const int hgt[4] = { 9, 15, 11, 19 };
    for (int i = 0; i < 4; i++) {
        int bxx = bx + (int)((6 + i * 8) * u), bhh = (int)(hgt[i] * u);
        gfx_fill_rounded_rect(s, bxx, by + bh - (int)(5 * u) - bhh, (int)(5 * u), bhh, (int)(1.5 * u),
                              color_alpha(RGB(110, 160, 255), 150));
    }
    /* pulse line */
    double pts[7][2] = { { 5, 20 }, { 13, 20 }, { 17, 11 }, { 23, 27 }, { 28, 16 }, { 31, 20 }, { 37, 20 } };
    for (int i = 0; i < 6; i++) {
        double x0 = bx + pts[i][0] * u, y0 = by + pts[i][1] * u, x1 = bx + pts[i + 1][0] * u, y1 = by + pts[i + 1][1] * u;
        for (double o = -1.4 * u; o <= 1.4 * u; o += 0.7)
            gfx_line_aa(s, x0, y0 + o, x1, y1 + o, RGB(90, 230, 140));
    }
}

REGISTER_APP(.name = "Task Manager", .category = CAT_SYSTEM,
             .description = "Open windows, installed apps, memory and activity",
             .launch = tm_launch, .draw_icon = tm_icon);
