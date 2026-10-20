/*
 * utils_calendar.c - Calendar (Accessories).
 *
 * Month grid (ISO weeks, Monday first, week numbers), today highlighted,
 * previous/next month, jump to today, keyboard navigation and a side panel
 * with details of the selected day. Each day can carry a one-line note; notes
 * are kept in a plain text file (/home/user/Documents/Calendar.txt, one
 * "YYYY-MM-DD text" line per day) so they can also be edited in Notepad.
 *
 * The grid is a focusable custom widget, so arrow keys navigate the days
 * whenever the grid has focus and go to the note box when that is focused.
 */
#include <nova.h>

#define NOTES_PATH "/home/user/Documents/Calendar.txt"
#define MAX_NOTES  512
#define NOTE_LEN   120
#define PANEL_W    232
#define DEF_W      700
#define DEF_H      480

typedef struct { int y, m, d; char text[NOTE_LEN]; } cal_note_t;

enum { HIT_NONE = -1, HIT_PREV = -2, HIT_NEXT = -3, HIT_TODAY = -4 };

typedef struct {
    window_t *win;
    widget_t *grid, *note_box, *note_lbl;
    int view_y, view_m;              /* month shown */
    int sel_y, sel_m, sel_d;         /* selected day */
    int ty, tm, td;                  /* today (refreshed by a timer) */
    int hover;                       /* cell 0..41 or HIT_* */
    int pressed;
    cal_note_t *notes;
    int nnotes;
    uint32_t notes_gen;              /* vfs_generation() after our last load/save */
    bool loading;                    /* suppress the note box callback */
    int timer;
} cal_t;

/* ---- date maths --------------------------------------------------------------------- */
static bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int days_in_month(int y, int m) {
    static const int md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return md[m - 1] + (m == 2 && is_leap(y));
}
/* 0 = Sunday (Sakamoto's method, Gregorian calendar) */
static int weekday(int y, int m, int d) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
static int day_of_year(int y, int m, int d) {
    int n = d;
    for (int i = 1; i < m; i++) n += days_in_month(y, i);
    return n;
}
/* Days since a fixed epoch (for differences). */
static long day_number(int y, int m, int d) {
    long yy = y - 1;
    return yy * 365 + yy / 4 - yy / 100 + yy / 400 + day_of_year(y, m, d);
}
/* ISO 8601 week number. */
static int iso_week(int y, int m, int d) {
    int wd = (weekday(y, m, d) + 6) % 7;          /* Monday = 0 */
    int doy = day_of_year(y, m, d);
    int w = (doy - wd + 9) / 7;
    if (w < 1) {
        int py = y - 1;
        int pdoy = day_of_year(py, 12, 31);
        int pwd = (weekday(py, 12, 31) + 6) % 7;
        return (pdoy - pwd + 9) / 7;
    }
    if (w == 53) {
        int dec31 = (weekday(y, 12, 31) + 6) % 7;
        if (dec31 < 3) return 1;                  /* Dec 31 on Mon-Wed: week 1 of next year */
    }
    return w;
}
static void add_days(int *y, int *m, int *d, int delta) {
    *d += delta;
    while (*d < 1) { if (--*m < 1) { *m = 12; --*y; } *d += days_in_month(*y, *m); }
    while (*d > days_in_month(*y, *m)) { *d -= days_in_month(*y, *m); if (++*m > 12) { *m = 1; ++*y; } }
}

/* ---- notes -------------------------------------------------------------------------- */
static int note_cmp(const void *a, const void *b) {
    const cal_note_t *x = a, *y = b;
    if (x->y != y->y) return x->y - y->y;
    if (x->m != y->m) return x->m - y->m;
    return x->d - y->d;
}

static void notes_load(cal_t *c) {
    c->nnotes = 0;
    uint32_t size = 0;
    char *data = vfs_read_file(NOTES_PATH, &size);
    if (data) {
        char *save = NULL;
        for (char *line = strtok_r(data, "\n", &save); line && c->nnotes < MAX_NOTES; line = strtok_r(NULL, "\n", &save)) {
            int y, m, d, n = 0;
            if (sscanf(line, "%d-%d-%d%n", &y, &m, &d, &n) < 3 || n <= 0) continue;
            if (y < 1 || m < 1 || m > 12 || d < 1 || d > 31) continue;
            const char *p = line + n;
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) continue;
            cal_note_t *nt = &c->notes[c->nnotes++];
            nt->y = y; nt->m = m; nt->d = d;
            strlcpy(nt->text, p, sizeof nt->text);
            int l = (int)strlen(nt->text);
            while (l > 0 && (nt->text[l - 1] == '\r' || nt->text[l - 1] == ' ')) nt->text[--l] = 0;
        }
        free(data);
        qsort(c->notes, (size_t)c->nnotes, sizeof(cal_note_t), note_cmp);
    }
    c->notes_gen = vfs_generation();
}

static void notes_save(cal_t *c) {
    int cap = c->nnotes * (NOTE_LEN + 16) + 64;
    char *buf = malloc((size_t)cap);
    if (!buf) return;
    int n = snprintf(buf, (size_t)cap, "# NovaOS Calendar notes: one \"YYYY-MM-DD text\" line per day\n");
    for (int i = 0; i < c->nnotes && n < cap; i++)
        n += snprintf(buf + n, (size_t)(cap - n), "%04d-%02d-%02d %s\n", c->notes[i].y, c->notes[i].m, c->notes[i].d,
                      c->notes[i].text);
    vfs_mkdirs("/home/user/Documents");
    vfs_write_file(NOTES_PATH, buf, (uint32_t)n);
    free(buf);
    c->notes_gen = vfs_generation();
}

static cal_note_t *note_find(cal_t *c, int y, int m, int d) {
    for (int i = 0; i < c->nnotes; i++)
        if (c->notes[i].y == y && c->notes[i].m == m && c->notes[i].d == d) return &c->notes[i];
    return NULL;
}

static void note_set(cal_t *c, int y, int m, int d, const char *text) {
    while (*text == ' ') text++;
    cal_note_t *nt = note_find(c, y, m, d);
    if (!*text) {
        if (nt) {
            int i = (int)(nt - c->notes);
            memmove(&c->notes[i], &c->notes[i + 1], sizeof(cal_note_t) * (size_t)(c->nnotes - i - 1));
            c->nnotes--;
        }
    } else {
        if (!nt) {
            if (c->nnotes >= MAX_NOTES) return;
            nt = &c->notes[c->nnotes++];
            nt->y = y; nt->m = m; nt->d = d;
        }
        strlcpy(nt->text, text, sizeof nt->text);
        qsort(c->notes, (size_t)c->nnotes, sizeof(cal_note_t), note_cmp);
    }
    notes_save(c);
}

static void notes_refresh_if_changed(cal_t *c) {
    if (vfs_generation() != c->notes_gen) notes_load(c);
}

/* ---- geometry (relative to the grid widget) ------------------------------------------- */
typedef struct { rect_t prev, next, today; int x0, y0, cw, ch, wk; } geo_t;

static geo_t geometry(rect_t r) {
    geo_t g;
    g.prev = rect_make(r.x + r.w - 74, r.y + 12, 32, 32);
    g.next = rect_make(r.x + r.w - 38, r.y + 12, 32, 32);
    g.today = rect_make(g.prev.x - 82, r.y + 13, 74, 30);
    g.wk = 30;
    g.x0 = r.x + g.wk;
    g.y0 = r.y + 88;
    g.cw = (r.w - g.wk) / 7;
    g.ch = (r.h - 88) / 6;
    return g;
}

/* First cell of the 6x7 grid (a Monday on or before the 1st). */
static void grid_start(cal_t *c, int *y, int *m, int *d) {
    *y = c->view_y; *m = c->view_m; *d = 1;
    int lead = (weekday(c->view_y, c->view_m, 1) + 6) % 7;
    add_days(y, m, d, -lead);
}

static void cell_date(cal_t *c, int idx, int *y, int *m, int *d) {
    grid_start(c, y, m, d);
    add_days(y, m, d, idx);
}

/* ---- navigation --------------------------------------------------------------------- */
static void sync_note_box(cal_t *c) {
    cal_note_t *nt = note_find(c, c->sel_y, c->sel_m, c->sel_d);
    c->loading = true;
    ui_set_text(c->note_box, nt ? nt->text : "");
    c->loading = false;
}

static void select_day(cal_t *c, int y, int m, int d) {
    if (y < 1600) { y = 1600; m = 1; d = 1; }
    if (y > 9999) { y = 9999; m = 12; d = 31; }
    c->sel_y = y; c->sel_m = m; c->sel_d = d;
    c->view_y = y; c->view_m = m;
    sync_note_box(c);
    win_invalidate(c->win);
}

static void shift_month(cal_t *c, int delta) {
    int y = c->sel_y, m = c->sel_m + delta;
    while (m < 1) { m += 12; y--; }
    while (m > 12) { m -= 12; y++; }
    int d = MIN(c->sel_d, days_in_month(y, m));
    select_day(c, y, m, d);
}

static void go_today(cal_t *c) { select_day(c, c->ty, c->tm, c->td); }

/* ---- drawing ------------------------------------------------------------------------ */
static uint32_t muted(const theme_t *t) { return color_mix(t->window_text, t->window_bg, 110); }

static void draw_round_btn(surface_t *s, rect_t r, int glyph, bool hover, bool down) {
    const theme_t *t = theme_get();
    uint32_t bg = down ? color_mix(t->window_bg, t->window_text, 40) : hover ? color_mix(t->window_bg, t->window_text, 20) : 0;
    if (bg) gfx_fill_circle_aa(s, r.x + r.w / 2.0, r.y + r.h / 2.0, r.w / 2.0, bg);
    ui_draw_glyph(s, glyph, r.x + (r.w - 16) / 2, r.y + (r.h - 16) / 2, 16, t->window_text);
}

static void grid_draw(widget_t *w, surface_t *s, rect_t r) {
    cal_t *c = ui_get_ctx(w);
    const theme_t *t = theme_get();
    geo_t g = geometry(r);
    char buf[64];

    /* header */
    snprintf(buf, sizeof buf, "%s", sys_month_name(c->view_m));
    int tw = gfx_text(s, r.x + 8, r.y + 10, buf, t->window_text, FONT_UI_TITLE);
    snprintf(buf, sizeof buf, "%d", c->view_y);
    gfx_text(s, r.x + 16 + tw, r.y + 10, buf, muted(t), FONT_UI_TITLE);
    draw_round_btn(s, g.prev, UI_GLYPH_ARROW_LEFT, c->hover == HIT_PREV, c->pressed == HIT_PREV);
    draw_round_btn(s, g.next, UI_GLYPH_ARROW_RIGHT, c->hover == HIT_NEXT, c->pressed == HIT_NEXT);
    int st = (c->hover == HIT_TODAY ? UI_STATE_HOVER : 0) | (c->pressed == HIT_TODAY ? UI_STATE_PRESSED : 0);
    bool on_today = c->view_y == c->ty && c->view_m == c->tm;
    if (on_today) st |= UI_STATE_DISABLED;
    ui_draw_button(s, g.today, "Today", st);

    /* weekday names */
    static const char *wd[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    for (int i = 0; i < 7; i++)
        gfx_text_aligned(s, rect_make(g.x0 + i * g.cw, r.y + 62, g.cw, 20), wd[i],
                         i >= 5 ? color_mix(t->accent, t->window_bg, 60) : muted(t), FONT_UI_BOLD, ALIGN_CENTER);
    gfx_hline(s, r.x, r.y + 84, r.w, color_mix(t->window_bg, t->window_border, 100));
    /* weekend column tint */
    gfx_fill_rect(s, g.x0 + 5 * g.cw, g.y0, 2 * g.cw, 6 * g.ch, color_mix(t->window_bg, t->window_text, 6));

    int y, m, d;
    grid_start(c, &y, &m, &d);
    bool focused = ui_has_focus(w) && win_is_focused(c->win);
    for (int i = 0; i < 42; i++) {
        int row = i / 7, col = i % 7;
        int cx = g.x0 + col * g.cw, cy = g.y0 + row * g.ch;
        if (col == 0) {
            snprintf(buf, sizeof buf, "%d", iso_week(y, m, d));
            gfx_text_aligned(s, rect_make(r.x, cy + 6, g.wk - 4, 18), buf,
                             color_mix(t->window_text, t->window_bg, 150), FONT_UI, ALIGN_RIGHT);
        }
        bool in_month = m == c->view_m;
        bool today = y == c->ty && m == c->tm && d == c->td;
        bool sel = y == c->sel_y && m == c->sel_m && d == c->sel_d;
        rect_t cell = rect_make(cx + 2, cy + 2, g.cw - 4, g.ch - 4);
        if (sel) {
            gfx_fill_rounded_rect(s, cell.x, cell.y, cell.w, cell.h, 8, color_mix(t->window_bg, t->accent, t->dark ? 60 : 38));
            gfx_rounded_rect(s, cell.x, cell.y, cell.w, cell.h, 8, color_alpha(t->accent, focused ? 230 : 120));
        } else if (i == c->hover) {
            gfx_fill_rounded_rect(s, cell.x, cell.y, cell.w, cell.h, 8, color_mix(t->window_bg, t->window_text, 16));
        }
        snprintf(buf, sizeof buf, "%d", d);
        int ny = cy + 6;
        uint32_t col_txt = in_month ? t->window_text : color_mix(t->window_text, t->window_bg, 170);
        if (today) {
            gfx_fill_circle_aa(s, cx + g.cw / 2.0, ny + 9.5, 13, t->accent);
            col_txt = t->accent_text;
        }
        gfx_text_aligned(s, rect_make(cx, ny + 1, g.cw, 18), buf, col_txt, today || sel ? FONT_UI_BOLD : FONT_UI, ALIGN_CENTER);
        cal_note_t *nt = note_find(c, y, m, d);
        if (nt) {
            if (g.ch >= 58 && g.cw >= 60) {
                int lx = cell.x + 6, ly = cy + 30;
                gfx_fill_rounded_rect(s, lx - 2, ly - 1, cell.w - 8, 18, 4, color_alpha(t->accent, in_month ? 50 : 25));
                gfx_text_ellipsis(s, lx + 2, ly, cell.w - 16, nt->text, in_month ? t->window_text : muted(t), FONT_UI);
            } else {
                gfx_fill_circle_aa(s, cx + g.cw / 2.0, ny + 27, 2.5, in_month ? t->accent : muted(t));
            }
        }
        add_days(&y, &m, &d, 1);
    }
}

static int grid_hit(cal_t *c, int x, int y) {
    rect_t r = ui_rect(c->grid);
    geo_t g = geometry(r);
    if (rect_contains(g.prev, x, y)) return HIT_PREV;
    if (rect_contains(g.next, x, y)) return HIT_NEXT;
    if (rect_contains(g.today, x, y)) return HIT_TODAY;
    if (x < g.x0 || y < g.y0 || x >= g.x0 + 7 * g.cw || y >= g.y0 + 6 * g.ch) return HIT_NONE;
    return ((y - g.y0) / g.ch) * 7 + (x - g.x0) / g.cw;
}

static bool grid_event(widget_t *w, const event_t *ev) {
    cal_t *c = ui_get_ctx(w);
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int h = grid_hit(c, ev->x, ev->y);
        if (h != c->hover) { c->hover = h; ui_invalidate(w); }
        return true;
    }
    case EV_MOUSE_DOWN:
        if (ev->button != MOUSE_LEFT) return true;
        c->pressed = grid_hit(c, ev->x, ev->y);
        if (c->pressed >= 0) {
            int y, m, d;
            cell_date(c, c->pressed, &y, &m, &d);
            select_day(c, y, m, d);
        }
        ui_invalidate(w);
        return true;
    case EV_DOUBLE_CLICK:
        if (grid_hit(c, ev->x, ev->y) >= 0) ui_focus(c->note_box);   /* double-click: edit the note */
        return true;
    case EV_MOUSE_UP: {
        int h = grid_hit(c, ev->x, ev->y);
        if (h == c->pressed) {
            if (h == HIT_PREV) shift_month(c, -1);
            else if (h == HIT_NEXT) shift_month(c, 1);
            else if (h == HIT_TODAY) go_today(c);
        }
        c->pressed = HIT_NONE;
        ui_invalidate(w);
        return true;
    }
    case EV_MOUSE_WHEEL:
        shift_month(c, ev->wheel > 0 ? 1 : -1);
        return true;
    case EV_KEY_DOWN: {
        int k = ev->key;
        int y = c->sel_y, m = c->sel_m, d = c->sel_d;
        if (k == KEY_LEFT) add_days(&y, &m, &d, -1);
        else if (k == KEY_RIGHT) add_days(&y, &m, &d, 1);
        else if (k == KEY_UP) add_days(&y, &m, &d, -7);
        else if (k == KEY_DOWN) add_days(&y, &m, &d, 7);
        else if (k == KEY_PAGEUP) { shift_month(c, (ev->mods & (MOD_SHIFT | MOD_CTRL)) ? -12 : -1); return true; }
        else if (k == KEY_PAGEDOWN) { shift_month(c, (ev->mods & (MOD_SHIFT | MOD_CTRL)) ? 12 : 1); return true; }
        else if (k == KEY_HOME || k == 't') { go_today(c); return true; }
        else if (k == KEY_ENTER) { ui_focus(c->note_box); return true; }
        else return false;
        select_day(c, y, m, d);
        return true;
    }
    default:
        return false;
    }
}

static void relative_text(cal_t *c, char *buf, int n) {
    long diff = day_number(c->sel_y, c->sel_m, c->sel_d) - day_number(c->ty, c->tm, c->td);
    if (diff == 0) strlcpy(buf, "Today", n);
    else if (diff == 1) strlcpy(buf, "Tomorrow", n);
    else if (diff == -1) strlcpy(buf, "Yesterday", n);
    else if (diff > 0) snprintf(buf, n, "In %ld days", diff);
    else snprintf(buf, n, "%ld days ago", -diff);
}

static void paint_panel(cal_t *c, surface_t *s) {
    const theme_t *t = theme_get();
    int W = s->w, H = s->h;
    int px = W - PANEL_W - 12, py = 12, pw = PANEL_W, ph = H - 24;
    ui_draw_panel(s, rect_make(px, py, pw, ph));
    char buf[96];
    int x = px + 18;
    int wd = weekday(c->sel_y, c->sel_m, c->sel_d);
    gfx_text(s, x, py + 16, sys_weekday_name(wd), t->accent, FONT_UI_LARGE);
    snprintf(buf, sizeof buf, "%d", c->sel_d);
    gfx_text(s, x - 2, py + 38, buf, t->window_text, FONT_UI_HUGE);
    snprintf(buf, sizeof buf, "%s %d", sys_month_name(c->sel_m), c->sel_y);
    gfx_text(s, x, py + 104, buf, t->window_text, FONT_UI_BOLD);
    gfx_hline(s, px + 14, py + 132, pw - 28, color_mix(t->panel_bg, t->window_border, 80));

    int iy = py + 144;
    relative_text(c, buf, sizeof buf);
    gfx_text(s, x, iy, buf, t->window_text, FONT_UI);
    snprintf(buf, sizeof buf, "Week %d  \x95  Day %d of %d", iso_week(c->sel_y, c->sel_m, c->sel_d),
             day_of_year(c->sel_y, c->sel_m, c->sel_d), is_leap(c->sel_y) ? 366 : 365);
    gfx_text(s, x, iy + 20, buf, muted(t), FONT_UI);

    /* the note label + text box are widgets at iy + 48 .. iy + 100 */
    int uy = iy + 112;
    gfx_text(s, x, uy, "Upcoming", t->window_text, FONT_UI_BOLD);
    uy += 22;
    int shown = 0;
    long today_n = day_number(c->ty, c->tm, c->td);
    for (int i = 0; i < c->nnotes && uy + 34 <= py + ph - 8; i++) {
        cal_note_t *nt = &c->notes[i];
        if (day_number(nt->y, nt->m, nt->d) < today_n) continue;
        if (nt->y != c->ty)
            snprintf(buf, sizeof buf, "%.3s %d %.3s %d", sys_weekday_name(weekday(nt->y, nt->m, nt->d)), nt->d,
                     sys_month_name(nt->m), nt->y);
        else
            snprintf(buf, sizeof buf, "%.3s %d %.3s", sys_weekday_name(weekday(nt->y, nt->m, nt->d)), nt->d,
                     sys_month_name(nt->m));
        gfx_fill_circle_aa(s, x + 3, uy + 8, 3, t->accent);
        gfx_text(s, x + 12, uy, buf, muted(t), FONT_UI);
        gfx_text_ellipsis(s, x + 12, uy + 16, pw - 48, nt->text, t->window_text, FONT_UI);
        uy += 38;
        shown++;
    }
    if (!shown) gfx_text(s, x, uy, "No upcoming notes", muted(t), FONT_UI);
}

static void layout(cal_t *c) {
    int W = win_width(c->win), H = win_height(c->win);
    ui_move(c->grid, 12, 8, W - PANEL_W - 36, H - 20);
    int px = W - PANEL_W - 12, x = px + 18;
    int iy = 12 + 144;
    ui_move(c->note_lbl, x, iy + 46, PANEL_W - 36, 20);
    ui_move(c->note_box, x, iy + 68, PANEL_W - 36, 30);
}

static void on_note_edit(widget_t *w, void *ctx) {
    cal_t *c = ctx;
    if (c->loading) return;
    note_set(c, c->sel_y, c->sel_m, c->sel_d, ui_get_text(w));
    win_invalidate(c->win);
}

static void on_note_submit(widget_t *w, void *ctx) {
    cal_t *c = ctx;
    (void)w;
    ui_focus(c->grid);
    win_invalidate(c->win);
}

static void refresh_today(cal_t *c, bool force) {
    datetime_t dt;
    sys_datetime(&dt);
    if (force || dt.year != c->ty || dt.month != c->tm || dt.day != c->td) {
        c->ty = dt.year; c->tm = dt.month; c->td = dt.day;
        win_invalidate(c->win);
    }
}

static void cal_event(window_t *win, const event_t *ev) {
    cal_t *c = win_get_userdata(win);
    if (!c) return;
    switch (ev->type) {
    case EV_PAINT:
        paint_panel(c, win_surface(win));
        break;
    case EV_RESIZE:
        layout(c);
        break;
    case EV_TIMER:
        refresh_today(c, false);   /* midnight rollover */
        break;
    case EV_FOCUS:
        notes_refresh_if_changed(c);   /* another window may have edited the notes */
        sync_note_box(c);
        win_invalidate(win);
        break;
    case EV_BLUR:
        win_invalidate(win);
        break;
    case EV_MOUSE_MOVE:
    case EV_MOUSE_LEAVE:
        if (c->hover != HIT_NONE) { c->hover = HIT_NONE; ui_invalidate(c->grid); }
        break;
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESCAPE) ui_focus(c->grid);
        else if (ev->key == KEY_F5) { notes_load(c); sync_note_box(c); win_invalidate(win); }
        break;
    case EV_CLOSE:
        win_set_userdata(win, NULL);
        free(c->notes);
        free(c);
        break;
    default:
        break;
    }
}

static void cal_launch(const char *arg) {
    cal_t *c = calloc(1, sizeof *c);
    if (!c) return;
    c->notes = calloc(MAX_NOTES, sizeof(cal_note_t));
    if (!c->notes) { free(c); return; }
    window_t *win = win_create("Calendar", DEF_W, DEF_H, WIN_RESIZABLE);
    if (!win) { free(c->notes); free(c); return; }
    c->win = win;
    c->hover = c->pressed = HIT_NONE;
    win_set_userdata(win, c);
    win_set_event_handler(win, cal_event);
    win_set_min_size(win, 560, 420);
    notes_load(c);
    refresh_today(c, true);

    c->grid = ui_custom(win, 12, 8, 100, 100, grid_draw, grid_event, c);
    c->note_lbl = ui_label(win, 0, 0, 10, 10, "Note for this day");
    ui_set_font(c->note_lbl, FONT_UI_BOLD);
    c->note_box = ui_textbox(win, 0, 0, 10, 10, "");
    ui_textbox_set_placeholder(c->note_box, "Add a note...");
    ui_textbox_set_maxlen(c->note_box, NOTE_LEN - 1);
    ui_set_callback(c->note_box, on_note_edit, c);
    ui_textbox_on_submit(c->note_box, on_note_submit, c);
    layout(c);

    int y = c->ty, m = c->tm, d = c->td;
    if (arg) {   /* "YYYY-MM-DD" opens that day */
        int ay, am, ad;
        if (sscanf(arg, "%d-%d-%d", &ay, &am, &ad) == 3 && am >= 1 && am <= 12 && ad >= 1 && ad <= days_in_month(ay, am)) {
            y = ay; m = am; d = ad;
        }
    }
    select_day(c, y, m, d);
    ui_focus(c->grid);
    c->timer = win_set_timer(win, 1000);
}

/* Icon: a tear-off calendar page with binder rings and a day grid. */
static void cal_icon(surface_t *s, int x, int y, int size) {
    double u = size / 48.0;
    int bx = x + (int)(5 * u), by = y + (int)(7 * u), bw = (int)(38 * u), bh = (int)(37 * u), r = (int)(6 * u);
    gfx_fill_rounded_rect(s, bx, by + (int)(1.5 * u), bw, bh, r, RGBA(0, 0, 0, 50));
    gfx_fill_rounded_rect(s, bx, by, bw, bh, r, RGB(250, 251, 253));
    gfx_fill_rounded_gradient_v(s, bx, by, bw, (int)(12 * u), r, RGB(250, 92, 80), RGB(226, 60, 56));
    gfx_fill_rect(s, bx, by + (int)(8 * u), bw, (int)(4 * u), RGB(226, 60, 56));
    /* binder rings */
    for (int i = 0; i < 2; i++) {
        int rx = bx + (int)((i ? 27 : 9) * u);
        gfx_fill_rounded_rect(s, rx, y + (int)(3 * u), (int)(3 * u), (int)(8 * u), (int)(1.5 * u), RGB(70, 76, 90));
    }
    /* day grid 4 x 3, one highlighted */
    int gx = bx + (int)(5 * u), gy = by + (int)(16 * u), cw = (int)(6 * u), ch = (int)(5 * u);
    int sx = (int)(1.9 * u) + cw, sy = (int)(1.8 * u) + ch;
    for (int r2 = 0; r2 < 3; r2++)
        for (int col = 0; col < 4; col++) {
            uint32_t cc = (r2 == 1 && col == 2) ? RGB(60, 120, 230) : RGB(196, 202, 214);
            gfx_fill_rounded_rect(s, gx + col * sx, gy + r2 * sy, cw, ch, (int)(1.2 * u) + 1, cc);
        }
}

REGISTER_APP(.name = "Calendar", .category = CAT_ACCESSORIES,
             .description = "Month calendar with notes for each day",
             .launch = cal_launch, .draw_icon = cal_icon);
