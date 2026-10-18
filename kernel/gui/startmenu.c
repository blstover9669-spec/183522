/* startmenu.c - the Start menu: searchable, categorised app list plus power buttons. */
#include "wm_internal.h"

#define SM_W 460
#define SM_MAX_H 600
#define SEARCH_H 56
#define FOOTER_H 56
#define ROW_H 44
#define HDR_H 30

typedef struct {
    int type;                /* 0 header, 1 app */
    const char *text;
    const app_desc_t *app;
    int y, h;
} smrow_t;

static window_t *sm_win;
static smrow_t rows[256];
static int nrows;
static int scroll_y, content_h;
static int hover_row = -1, sel_row = -1;
static int hover_power = 0;     /* 1 restart, 2 shutdown */
static char filter[48];
static uint32_t closed_serial;

bool startmenu_is_open(void) { return sm_win && win_is_valid(sm_win); }
bool startmenu_just_closed(void) { return closed_serial == wm_click_serial && wm_click_serial != 0; }

static bool matches(const app_desc_t *a) {
    if (!filter[0]) return true;
    return strcasestr(a->name, filter) || (a->description && strcasestr(a->description, filter)) ||
           (a->category && strcasestr(a->category, filter));
}

static void build_rows(void) {
    nrows = 0;
    const char *cats[32];
    int nc = app_categories(cats, 32);
    if (nc > 32) nc = 32;
    int y = 0;
    for (int c = 0; c < nc; c++) {
        bool header_added = false;
        for (int i = 0; i < app_count() && nrows < 255; i++) {
            const app_desc_t *a = app_get(i);
            if (a->flags & APP_HIDDEN) continue;
            const char *ac = a->category ? a->category : "Other";
            if (strcasecmp(ac, cats[c]) || !matches(a)) continue;
            if (!header_added && !filter[0]) {
                rows[nrows++] = (smrow_t){ 0, cats[c], NULL, y, HDR_H };
                y += HDR_H;
                header_added = true;
            }
            rows[nrows++] = (smrow_t){ 1, a->name, a, y, ROW_H };
            y += ROW_H;
        }
    }
    content_h = y;
    sel_row = -1;
    for (int i = 0; i < nrows; i++) if (rows[i].type == 1) { if (filter[0]) sel_row = i; break; }
    scroll_y = 0;
}

static int list_h(window_t *w) { return win_height(w) - SEARCH_H - FOOTER_H; }

static void ensure_visible(window_t *w, int r) {
    if (r < 0) return;
    int lh = list_h(w);
    if (rows[r].y < scroll_y) scroll_y = rows[r].y - (r > 0 && rows[r - 1].type == 0 ? HDR_H : 0);
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
        gfx_text(s, sr.x + 40, sr.y + (sr.h - font_height(FONT_UI)) / 2, "Type to search apps", color_mix(t->menu_text, bg, 120), FONT_UI);
    }

    /* list */
    int ly = SEARCH_H, lh = list_h(win);
    rect_t old = s->clip;
    surface_set_clip(s, rect_intersect(old, rect_make(0, ly, s->w, lh)));
    if (!nrows) gfx_text_aligned(s, rect_make(0, ly + 30, s->w, 20), "No apps found", color_mix(t->menu_text, bg, 110), FONT_UI, ALIGN_CENTER);
    for (int i = 0; i < nrows; i++) {
        smrow_t *r = &rows[i];
        int y = ly + r->y - scroll_y;
        if (y + r->h < ly || y > ly + lh) continue;
        if (r->type == 0) {
            gfx_text(s, 22, y + 10, r->text, t->accent, FONT_UI_BOLD);
            continue;
        }
        if (i == hover_row || i == sel_row)
            gfx_fill_rounded_rect(s, 10, y + 2, s->w - 20 - 8, r->h - 4, 6,
                                  i == sel_row ? color_mix(bg, t->accent, t->dark ? 70 : 45) : t->menu_hover);
        app_draw_icon(s, r->app, 22, y + (r->h - 32) / 2, 32);
        gfx_text(s, 66, y + 5, r->app->name, t->menu_text, FONT_UI_BOLD);
        if (r->app->description)
            gfx_text_ellipsis(s, 66, y + 22, s->w - 66 - 30, r->app->description, color_mix(t->menu_text, bg, 100), FONT_UI);
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
    gfx_text(s, 62, fy + 29, sys_version(), color_mix(t->menu_text, bg, 100), FONT_UI);
    rect_t rb = rect_make(s->w - 96, fy + 10, 38, 36), pb = rect_make(s->w - 52, fy + 10, 38, 36);
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
        if (rows[i].type == 1 && cy >= rows[i].y && cy < rows[i].y + rows[i].h) return i;
    return -1;
}

static int power_at(window_t *w, int x, int y) {
    int fy = win_height(w) - FOOTER_H, W = win_width(w);
    if (y < fy + 10 || y >= fy + 46) return 0;
    if (x >= W - 96 && x < W - 58) return 1;
    if (x >= W - 52 && x < W - 14) return 2;
    return 0;
}

static void do_power(void *ctx) {
    if ((intptr_t)ctx == 1) sys_reboot();
    else sys_shutdown();
}

static void launch_row(window_t *w, int r) {
    if (r < 0 || r >= nrows || rows[r].type != 1) return;
    const app_desc_t *a = rows[r].app;
    win_close(w);
    app_launch(a, NULL);
}

static void move_sel(window_t *w, int dir) {
    int r = sel_row;
    for (int n = 0; n < nrows; n++) {
        r += dir;
        if (r < 0) r = nrows - 1;
        if (r >= nrows) r = 0;
        if (rows[r].type == 1) break;
    }
    if (nrows && rows[r].type == 1) { sel_row = r; ensure_visible(w, r); win_invalidate(w); }
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
    case EV_MOUSE_UP: {
        if (ev->button != MOUSE_LEFT) break;
        int p = power_at(win, ev->x, ev->y);
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
