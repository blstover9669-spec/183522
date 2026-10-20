/*
 * services.c - the Services app (System): every background service with its
 * state, startup type, uptime and description; Start / Stop / Restart, the
 * "Start automatically" setting and the service's own log.
 *
 * Everything comes from the service manager API in <nova/service.h>; the
 * window polls service_generation() so it follows changes made elsewhere
 * (e.g. `service stop notifyd` in the Terminal) within half a second.
 */
#include <nova.h>

#define SV_W 820
#define SV_H 600
#define ROW_H 32
#define HEAD_H 30
#define HDR_H 62          /* title area above the table */
#define DETAIL_H 104      /* details card below the table */
#define LOG_MAX 6144

typedef struct {
    window_t *win;
    widget_t *table, *b_start, *b_stop, *b_restart, *autostart, *log;
    int sel, scroll, hover;
    uint32_t gen;             /* service_generation() at the last refresh */
    char *logbuf;             /* text currently in the log pane            */
    int timer;
    int ticks;
} sv_t;

/* ---- helpers --------------------------------------------------------------------- */
static uint32_t muted(const theme_t *t) { return color_mix(t->window_text, t->window_bg, 110); }

static uint32_t state_color(int st) {
    switch (st) {
    case SVC_RUNNING:  return RGB(46, 184, 92);
    case SVC_STARTING: return RGB(240, 170, 30);
    case SVC_FAILED:   return RGB(226, 64, 64);
    default:           return RGB(150, 156, 168);
    }
}

static const char *state_label(int st) {
    switch (st) {
    case SVC_RUNNING:  return "Running";
    case SVC_STARTING: return "Starting";
    case SVC_FAILED:   return "Failed";
    default:           return "Stopped";
    }
}

static void fmt_duration(uint32_t ms, char *out, int n) {
    uint32_t s = ms / 1000;
    if (s >= 86400) snprintf(out, n, "%ud %02u:%02u", s / 86400, (s / 3600) % 24, (s / 60) % 60);
    else if (s >= 3600) snprintf(out, n, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(out, n, "%u:%02u", s / 60, s % 60);
}

static bool selected(sv_t *c, service_status_t *st) {
    return c->sel >= 0 && service_status_at(c->sel, st) == 0;
}

static int visible_rows(sv_t *c) { return MAX(1, (ui_rect(c->table).h - HEAD_H - 2) / ROW_H); }

/* A status dot with a soft halo (running services glow a little). */
static void status_dot(surface_t *s, int cx, int cy, int st) {
    uint32_t col = state_color(st);
    if (st == SVC_RUNNING || st == SVC_FAILED) gfx_fill_circle_aa(s, cx, cy, 6.5, color_alpha(col, 60));
    gfx_fill_circle_aa(s, cx, cy, 4.5, col);
    gfx_fill_circle_aa(s, cx - 1.2, cy - 1.4, 1.6, color_alpha(COLOR_WHITE, 110));
}

/* ---- refresh ------------------------------------------------------------------------- */
static void refresh_log(sv_t *c) {
    service_status_t st;
    char *buf = malloc(LOG_MAX);
    if (!buf) return;
    buf[0] = 0;
    if (selected(c, &st)) {
        int n = service_log_text(st.name, buf, LOG_MAX);
        if (n <= 0) snprintf(buf, LOG_MAX, "(no log entries for %s yet)\n", st.name);
    }
    if (c->logbuf && !strcmp(c->logbuf, buf)) { free(buf); return; }
    free(c->logbuf);
    c->logbuf = buf;
    ui_set_text(c->log, "");
    ui_textarea_append(c->log, buf);
}

static void update_controls(sv_t *c) {
    service_status_t st;
    bool have = selected(c, &st);
    bool up = have && (st.state == SVC_RUNNING || st.state == SVC_STARTING);
    ui_set_enabled(c->b_start, have && !up);
    ui_set_enabled(c->b_stop, have && st.state != SVC_STOPPED);
    ui_set_enabled(c->b_restart, have && up);
    ui_set_enabled(c->autostart, have);
    ui_set_value(c->autostart, have && st.autostart);
    ui_button_set_default(c->b_start, have && !up);
    ui_button_set_default(c->b_stop, false);
}

static void refresh(sv_t *c) {
    int n = service_count();
    if (c->sel >= n) c->sel = n - 1;
    if (c->sel < 0 && n > 0) c->sel = 0;
    c->scroll = CLAMP(c->scroll, 0, MAX(0, n - visible_rows(c)));
    c->gen = service_generation();
    update_controls(c);
    refresh_log(c);
    win_invalidate(c->win);
}

/* ---- actions ----------------------------------------------------------------------- */
static void report(sv_t *c, const char *verb, int r) {
    service_status_t st;
    if (!selected(c, &st) || r == 0) return;
    char msg[200];
    snprintf(msg, sizeof msg, "Could not %s \"%s\".%s%s", verb, st.display_name,
             st.last_error[0] ? "\n\n" : "", st.last_error);
    ui_message_box(c->win, "Services", msg, MB_OK | MB_ICON_ERROR, NULL, NULL);
}

static void do_start(sv_t *c) {
    service_status_t st;
    if (!selected(c, &st)) return;
    report(c, "start", service_start(st.name));
    refresh(c);
}

static void do_stop(sv_t *c) {
    service_status_t st;
    if (!selected(c, &st)) return;
    report(c, "stop", service_stop(st.name));
    refresh(c);
}

static void do_restart(sv_t *c) {
    service_status_t st;
    if (!selected(c, &st)) return;
    report(c, "restart", service_restart(st.name));
    refresh(c);
}

static void do_toggle(sv_t *c) {
    service_status_t st;
    if (!selected(c, &st)) return;
    if (st.state == SVC_RUNNING || st.state == SVC_STARTING) do_stop(c);
    else do_start(c);
}

static void set_autostart(sv_t *c, bool on) {
    service_status_t st;
    if (!selected(c, &st)) return;
    service_set_autostart(st.name, on);
    refresh(c);
}

static void on_start(widget_t *w, void *ctx) { (void)w; do_start(ctx); }
static void on_stop(widget_t *w, void *ctx) { (void)w; do_stop(ctx); }
static void on_restart(widget_t *w, void *ctx) { (void)w; do_restart(ctx); }
static void on_autostart(widget_t *w, void *ctx) { set_autostart(ctx, ui_get_value(w) != 0); }

/* ---- the table ----------------------------------------------------------------------- */
typedef struct { const char *title; int min_w; int weight; } col_t;
static const col_t COLS[] = {
    { "Service", 190, 0 }, { "Status", 100, 0 }, { "Startup", 96, 0 }, { "Uptime", 76, 0 }, { "Description", 120, 1 },
};
#define NCOLS 5

static void col_layout(int width, int *cx) {
    int fixed = 0;
    for (int i = 0; i < NCOLS; i++) fixed += COLS[i].min_w;
    int extra = MAX(0, width - fixed);
    cx[0] = 0;
    for (int i = 0; i < NCOLS; i++) cx[i + 1] = cx[i] + COLS[i].min_w + (COLS[i].weight ? extra : 0);
}

static void table_draw(widget_t *wd, surface_t *s, rect_t r) {
    sv_t *c = ui_get_ctx(wd);
    const theme_t *t = theme_get();
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 8, t->window_border);
    gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 7, t->field_bg);
    gfx_fill_rect(s, r.x + 1, r.y + 8, r.w - 2, HEAD_H - 8, t->panel_bg);
    gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, HEAD_H, 7, t->panel_bg);
    gfx_hline(s, r.x + 1, r.y + HEAD_H, r.w - 2, color_mix(t->panel_bg, t->window_border, 110));
    int cx[NCOLS + 1];
    col_layout(r.w - 2, cx);
    for (int i = 0; i < NCOLS; i++) {
        gfx_text(s, r.x + 1 + cx[i] + 12, r.y + (HEAD_H - font_height(FONT_UI_BOLD)) / 2, COLS[i].title, muted(t), FONT_UI_BOLD);
        if (i) gfx_vline(s, r.x + 1 + cx[i], r.y + 7, HEAD_H - 14, color_mix(t->panel_bg, t->window_border, 90));
    }
    int n = service_count(), vis = visible_rows(c);
    rect_t saved = s->clip;
    surface_set_clip(s, rect_intersect(s->clip, rect_make(r.x + 1, r.y + HEAD_H + 1, r.w - 2, r.h - HEAD_H - 2)));
    bool focused = ui_has_focus(wd) && win_is_focused(c->win);
    for (int k = 0; k <= vis && c->scroll + k < n; k++) {
        int row = c->scroll + k;
        service_status_t st;
        if (service_status_at(row, &st) != 0) continue;
        int y = r.y + HEAD_H + 1 + k * ROW_H;
        bool sel = row == c->sel;
        uint32_t fg = t->field_text, dim = color_mix(t->field_text, t->field_bg, 80);
        if (sel) {
            gfx_fill_rect(s, r.x + 1, y, r.w - 2, ROW_H, focused ? t->selection_bg : color_mix(t->field_bg, t->selection_bg, 90));
            if (focused) fg = dim = t->selection_text;
        } else if (row == c->hover) {
            gfx_fill_rect(s, r.x + 1, y, r.w - 2, ROW_H, color_mix(t->field_bg, t->field_text, 14));
        } else if (k & 1) {
            gfx_fill_rect(s, r.x + 1, y, r.w - 2, ROW_H, color_mix(t->field_bg, t->field_text, 5));
        }
        int ty = y + (ROW_H - font_height(FONT_UI)) / 2;
        /* service: status dot, icon, display name */
        int x = r.x + 1 + cx[0] + 12;
        status_dot(s, x + 5, y + ROW_H / 2, st.state);
        icon_draw(st.icon ? st.icon : ICON_SERVICE, s, x + 18, y + (ROW_H - 16) / 2, 16);
        gfx_text_ellipsis(s, x + 42, ty, cx[1] - cx[0] - 60, st.display_name, fg, FONT_UI_BOLD);
        /* status */
        x = r.x + 1 + cx[1] + 12;
        gfx_text(s, x, ty, state_label(st.state), sel && focused ? fg : st.state == SVC_STOPPED ? dim : state_color(st.state), FONT_UI_BOLD);
        /* startup */
        x = r.x + 1 + cx[2] + 12;
        gfx_text(s, x, ty, st.autostart ? "Automatic" : "Manual", dim, FONT_UI);
        /* uptime */
        x = r.x + 1 + cx[3] + 12;
        char up[24] = "-";
        if (st.state == SVC_RUNNING) fmt_duration(st.uptime_ms, up, sizeof up);
        gfx_text(s, x, ty, up, dim, FONT_UI);
        /* description */
        x = r.x + 1 + cx[4] + 12;
        gfx_text_ellipsis(s, x, ty, cx[5] - cx[4] - 22, st.description, dim, FONT_UI);
    }
    if (!n) gfx_text_aligned(s, rect_make(r.x, r.y + HEAD_H + 20, r.w, 20), "No services are registered", muted(t), FONT_UI, ALIGN_CENTER);
    if (n > vis) {
        int th = MAX(20, (r.h - HEAD_H - 8) * vis / n);
        int tyy = r.y + HEAD_H + 4 + (r.h - HEAD_H - 8 - th) * c->scroll / MAX(1, n - vis);
        gfx_fill_rounded_rect(s, r.x + r.w - 7, tyy, 4, th, 2, t->scrollbar_thumb);
    }
    surface_set_clip(s, saved);
}

static int row_at(sv_t *c, int y) {
    rect_t r = ui_rect(c->table);
    if (y < r.y + HEAD_H + 1) return -1;
    int row = c->scroll + (y - r.y - HEAD_H - 1) / ROW_H;
    return row < service_count() ? row : -1;
}

static void ensure_visible(sv_t *c) {
    int vis = visible_rows(c);
    if (c->sel < c->scroll) c->scroll = c->sel;
    if (c->sel >= c->scroll + vis) c->scroll = c->sel - vis + 1;
    if (c->scroll < 0) c->scroll = 0;
}

static void select_row(sv_t *c, int row) {
    if (row == c->sel) return;
    c->sel = row;
    ensure_visible(c);
    update_controls(c);
    refresh_log(c);
    win_invalidate(c->win);
}

static void row_menu_cb(int item, void *ctx) {
    sv_t *c = ctx;
    service_status_t st;
    if (!selected(c, &st)) return;
    switch (item) {
    case 0: do_start(c); break;
    case 1: do_stop(c); break;
    case 2: do_restart(c); break;
    case 4: set_autostart(c, !st.autostart); break;
    case 6: {
        char cmd[64];
        snprintf(cmd, sizeof cmd, "logs %s", st.name);
        char err[128];
        shell_run(cmd, err, sizeof err);
        break;
    }
    default: break;
    }
}

static void row_menu(sv_t *c, int x, int y) {
    service_status_t st;
    if (!selected(c, &st)) return;
    bool up = st.state == SVC_RUNNING || st.state == SVC_STARTING;
    char items[200];
    snprintf(items, sizeof items, "%sStart|%sStop|%sRestart|-|%s|-|Show full log",
             up ? "!" : "", st.state == SVC_STOPPED ? "!" : "", up ? "" : "!",
             st.autostart ? "Disable autostart" : "Enable autostart");
    ui_popup_menu(c->win, x, y, items, row_menu_cb, c);
}

static bool table_event(widget_t *wd, const event_t *ev) {
    sv_t *c = ui_get_ctx(wd);
    int n = service_count();
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int h = row_at(c, ev->y);
        if (h != c->hover) { c->hover = h; ui_invalidate(wd); }
        return true;
    }
    case EV_MOUSE_LEAVE:
        if (c->hover != -1) { c->hover = -1; ui_invalidate(wd); }
        return false;
    case EV_MOUSE_DOWN: {
        int row = row_at(c, ev->y);
        if (row >= 0) select_row(c, row);
        if (row >= 0 && ev->button == MOUSE_RIGHT) row_menu(c, ev->x, ev->y);
        return true;
    }
    case EV_DOUBLE_CLICK:
        if (row_at(c, ev->y) >= 0) do_toggle(c);
        return true;
    case EV_MOUSE_UP:
        return true;
    case EV_MOUSE_WHEEL:
        c->scroll = CLAMP(c->scroll + ev->wheel * 2, 0, MAX(0, n - visible_rows(c)));
        ui_invalidate(wd);
        return true;
    case EV_KEY_DOWN: {
        if (!n) return false;
        int k = ev->key, s = c->sel;
        if (k == KEY_DOWN) s++;
        else if (k == KEY_UP) s--;
        else if (k == KEY_HOME) s = 0;
        else if (k == KEY_END) s = n - 1;
        else if (k == KEY_PAGEDOWN) s += visible_rows(c);
        else if (k == KEY_PAGEUP) s -= visible_rows(c);
        else if (k == KEY_ENTER || k == ' ') { do_toggle(c); return true; }
        else return false;
        select_row(c, CLAMP(s, 0, n - 1));
        return true;
    }
    default:
        return false;
    }
}

/* ---- layout + painting ------------------------------------------------------------------ */
static int table_height(sv_t *c) {
    int H = win_height(c->win);
    int want = HEAD_H + 2 + MAX(service_count(), 3) * ROW_H;
    int maxh = H - HDR_H - DETAIL_H - 150;
    return MAX(HEAD_H + 2 + 3 * ROW_H, MIN(want, maxh));
}

static int detail_y(sv_t *c) { return HDR_H + table_height(c) + 10; }

static void layout(sv_t *c) {
    int W = win_width(c->win), H = win_height(c->win);
    ui_move(c->table, 12, HDR_H, W - 24, table_height(c));
    int dy = detail_y(c);
    int bx = W - 24 - 3 * 92 - 2 * 8;
    ui_move(c->b_start, bx, dy + 16, 92, 32);
    ui_move(c->b_stop, bx + 100, dy + 16, 92, 32);
    ui_move(c->b_restart, bx + 200, dy + 16, 92, 32);
    ui_move(c->autostart, bx, dy + 60, 3 * 92 + 16, 26);
    int ly = dy + DETAIL_H + 34;
    ui_move(c->log, 12, ly, W - 24, MAX(60, H - ly - 12));
}

static void paint(sv_t *c) {
    surface_t *s = win_surface(c->win);
    const theme_t *t = theme_get();
    int W = s->w;
    gfx_clear(s, t->window_bg);

    /* title + summary */
    icon_draw(ICON_SERVICE, s, 16, 14, 32);
    gfx_text(s, 58, 12, "Services", t->window_text, FONT_UI_LARGE);
    int n = service_count(), running = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        service_status_t st;
        if (service_status_at(i, &st) != 0) continue;
        if (st.state == SVC_RUNNING) running++;
        else if (st.state == SVC_FAILED) failed++;
    }
    char sum[128];
    snprintf(sum, sizeof sum, "%d services: %d running, %d stopped", n, running, n - running - failed);
    if (failed) snprintf(sum + strlen(sum), sizeof sum - strlen(sum), ", %d failed", failed);
    gfx_text(s, 59, 36, sum, muted(t), FONT_UI);
    /* legend on the right */
    static const int legend[] = { SVC_RUNNING, SVC_STOPPED, SVC_FAILED };
    int lx = W - 16;
    for (int i = 2; i >= 0; i--) {
        const char *lbl = state_label(legend[i]);
        lx -= font_text_width(FONT_UI, lbl);
        gfx_text(s, lx, 24, lbl, muted(t), FONT_UI);
        lx -= 16;
        status_dot(s, lx + 5, 24 + font_height(FONT_UI) / 2, legend[i]);
        lx -= 14;
    }

    /* details card */
    int dy = detail_y(c);
    rect_t card = rect_make(12, dy, W - 24, DETAIL_H);
    ui_draw_panel(s, card);
    service_status_t st;
    if (selected(c, &st)) {
        icon_draw(st.icon ? st.icon : ICON_SERVICE, s, card.x + 16, card.y + 16, 48);
        int tx = card.x + 80, maxw = ui_rect(c->b_start).x - tx - 16;
        int nw = gfx_text_ellipsis(s, tx, card.y + 12, maxw, st.display_name, t->window_text, FONT_UI_LARGE);
        char tag[48];
        snprintf(tag, sizeof tag, "(%s)", st.name);
        if (nw + 10 < maxw) gfx_text_ellipsis(s, tx + nw + 8, card.y + 16, maxw - nw - 8, tag, muted(t), FONT_UI);
        gfx_text_ellipsis(s, tx, card.y + 38, maxw, st.description, muted(t), FONT_UI);
        /* state line */
        char line[200], up[24];
        int sx = tx;
        status_dot(s, sx + 5, card.y + 66 + font_height(FONT_UI) / 2, st.state);
        sx += 16;
        sx += gfx_text(s, sx, card.y + 66, state_label(st.state), st.state == SVC_STOPPED ? t->window_text : state_color(st.state), FONT_UI_BOLD);
        line[0] = 0;
        if (st.state == SVC_RUNNING) {
            fmt_duration(st.uptime_ms, up, sizeof up);
            snprintf(line, sizeof line, " for %s", up);
        }
        if (st.tick_ms) snprintf(line + strlen(line), sizeof line - strlen(line), "  -  tick %u %s",
                                 st.tick_ms >= 1000 ? st.tick_ms / 1000 : st.tick_ms, st.tick_ms >= 1000 ? "s" : "ms");
        if (st.restart_on_failure) strlcat(line, "  -  auto-restart", sizeof line);
        if (st.restarts) snprintf(line + strlen(line), sizeof line - strlen(line), " (%d so far)", st.restarts);
        gfx_text_ellipsis(s, sx, card.y + 66, card.x + maxw + 80 - sx, line, muted(t), FONT_UI);
        if (st.last_error[0]) {
            char err[140];
            snprintf(err, sizeof err, "Last error: %s", st.last_error);
            gfx_text_ellipsis(s, tx, card.y + 84, maxw, err, state_color(SVC_FAILED), FONT_UI);
        } else if (st.state == SVC_RUNNING) {
            datetime_t dt;
            sys_time_to_datetime(sys_time() - st.uptime_ms / 1000, &dt);
            snprintf(line, sizeof line, "Started at %02d:%02d:%02d", dt.hour, dt.minute, dt.second);
            gfx_text_ellipsis(s, tx, card.y + 84, maxw, line, muted(t), FONT_UI);
        }
    } else {
        gfx_text_aligned(s, card, "Select a service", muted(t), FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
    }

    /* log heading */
    int ly = dy + DETAIL_H + 12;
    gfx_text(s, 14, ly, "Log", t->window_text, FONT_UI_BOLD);
    if (selected(c, &st)) {
        char h[96];
        snprintf(h, sizeof h, "entries tagged [%s] - full system log: logs in the Terminal", st.name);
        gfx_text_ellipsis(s, 46, ly, W - 60, h, muted(t), FONT_UI);
    }
}

/* ---- window ---------------------------------------------------------------------------- */
static void sv_event(window_t *win, const event_t *ev) {
    sv_t *c = win_get_userdata(win);
    if (!c) return;
    switch (ev->type) {
    case EV_PAINT:
        paint(c);
        break;
    case EV_TIMER:
        if (win_is_minimized(win)) break;
        if (service_generation() != c->gen) { refresh(c); break; }
        if (++c->ticks % 2 == 0) {
            /* uptimes */
            ui_invalidate(c->table);
            win_invalidate_rect(win, 0, detail_y(c), win_width(win), DETAIL_H);
        }
        break;
    case EV_RESIZE:
        layout(c);
        c->scroll = CLAMP(c->scroll, 0, MAX(0, service_count() - visible_rows(c)));
        break;
    case EV_FOCUS:
    case EV_BLUR:
        ui_invalidate(c->table);
        break;
    case EV_KEY_DOWN:
        if (ev->key == KEY_F5) refresh(c);
        break;
    case EV_CLOSE:
        win_set_userdata(win, NULL);
        free(c->logbuf);
        free(c);
        break;
    default:
        break;
    }
}

static void sv_launch(const char *arg) {
    sv_t *c = calloc(1, sizeof *c);
    if (!c) return;
    window_t *win = win_create("Services", SV_W, SV_H, WIN_RESIZABLE | WIN_CENTER);
    if (!win) { free(c); return; }
    c->win = win;
    c->sel = -1;
    c->hover = -1;
    win_set_userdata(win, c);
    win_set_event_handler(win, sv_event);
    win_set_min_size(win, 660, 500);

    c->table = ui_custom(win, 12, HDR_H, 100, 100, table_draw, table_event, c);
    c->b_start = ui_button(win, 0, 0, 10, 10, "Start", on_start, c);
    c->b_stop = ui_button(win, 0, 0, 10, 10, "Stop", on_stop, c);
    c->b_restart = ui_button(win, 0, 0, 10, 10, "Restart", on_restart, c);
    c->autostart = ui_checkbox(win, 0, 0, 10, 10, "Start automatically at boot", false, on_autostart, c);
    c->log = ui_textarea(win, 0, 0, 10, 10, "");
    ui_textarea_set_readonly(c->log, true);
    layout(c);

    /* "Services:notifyd" selects a service */
    if (arg && *arg) c->sel = service_find(arg);
    refresh(c);
    ensure_visible(c);
    ui_focus(c->table);
    c->timer = win_set_timer(win, 500);
}

/* Icon: a slate tile holding a service list - three rows with status lights
   (two green, one red) - and an orange gear badge in the corner. */
static void gear(surface_t *s, double cx, double cy, double r, int teeth, uint32_t col, uint32_t hole) {
    point_t pts[64];
    int n = 0;
    for (int i = 0; i < teeth * 4 && n < 64; i++) {
        double a0 = (i / 4.0) * 2 * M_PI / teeth;
        double rr = (i % 4 == 1 || i % 4 == 2) ? r : r * 0.74;
        double a = a0 + ((i % 4 == 0 || i % 4 == 1) ? -0.05 : 0.05) * 2 * M_PI / teeth * 2.2;
        pts[n++] = (point_t){ (int)lround(cx + rr * sin(a)), (int)lround(cy - rr * cos(a)) };
    }
    gfx_fill_polygon(s, pts, n, col);
    gfx_fill_circle_aa(s, cx, cy, r * 0.78, col);
    gfx_fill_circle_aa(s, cx, cy, r * 0.32, hole);
}

static void sv_icon(surface_t *s, int x, int y, int size) {
    double u = size / 48.0;
    int bx = x + (int)(4 * u), by = y + (int)(5 * u), bw = (int)(38 * u), bh = (int)(36 * u);
    /* shadow + tile */
    gfx_fill_rounded_rect(s, bx, by + (int)(1.5 * u), bw, bh, (int)(7 * u), RGBA(0, 0, 0, 60));
    gfx_fill_rounded_gradient_v(s, bx, by, bw, bh, (int)(7 * u), RGB(70, 86, 116), RGB(38, 46, 66));
    /* rows */
    static const uint32_t lights[3] = { RGB(70, 220, 120), RGB(70, 220, 120), RGB(255, 90, 90) };
    static const int len[3] = { 20, 15, 18 };
    for (int i = 0; i < 3; i++) {
        double ry = by + (8.5 + i * 9.5) * u;
        gfx_fill_rounded_rect(s, bx + (int)(4 * u), (int)(ry - 3.5 * u), bw - (int)(8 * u), (int)(7 * u), (int)(2 * u),
                              RGBA(255, 255, 255, 26));
        gfx_fill_circle_aa(s, bx + 9 * u, ry, 2.6 * u, lights[i]);
        gfx_fill_rounded_rect(s, bx + (int)(15 * u), (int)(ry - 1.2 * u), (int)(len[i] * u), (int)(2.6 * u), (int)(1.2 * u),
                              RGBA(225, 232, 245, 220));
    }
    /* gear badge */
    double gx = x + 37 * u, gy = y + 37 * u;
    gfx_fill_circle_aa(s, gx, gy + 0.8 * u, 10.5 * u, RGBA(0, 0, 0, 50));
    gear(s, gx, gy, 10.5 * u, 8, RGB(255, 255, 255), RGB(255, 255, 255));
    gear(s, gx, gy, 9.0 * u, 8, RGB(247, 146, 40), RGB(255, 255, 255));
}

REGISTER_APP(.name = "Services", .category = CAT_SYSTEM,
             .description = "Start, stop and configure background services",
             .launch = sv_launch, .draw_icon = sv_icon, .flags = APP_SINGLE);
