/*
 * browser.c - the Web Browser app: window, tabs, toolbar, address bar,
 * bookmarks bar, the page view (scrolling, links, form controls), find in
 * page, zoom, the browser settings dialog and the app icon.
 *
 * Loading pages lives in browser_net.c, settings / history / bookmarks and
 * the built-in pages in browser_data.c, the web engine in web_*.c.
 */
#include "browser.h"

enum { HS_NONE = 0, HS_BACK, HS_FWD, HS_RELOAD, HS_HOME, HS_LOCK, HS_STAR, HS_MENU, HS_ZOOM, HS_NEWTAB,
       HS_FPREV, HS_FNEXT, HS_FCLOSE, HS_TAB = 100, HS_TABX = 200, HS_BM = 300 };

#define NOTE_TEXT "Don't type real passwords/bank details (hobby TLS)"

static const int zoom_steps[] = { 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200 };

typedef struct bview {   /* per-window state that browser.h does not need to know */
    surface_t *pc;       /* the page rendered at the current scroll position */
    bool pc_valid;
    int press_link;
    bool chrome_dirty;
    uint32_t resize_at;
    uint32_t status_until;
} bview_t;

static bview_t *V(browser_t *b) { return (bview_t *)(b + 1); }

static tab_t *cur(browser_t *b) { return b->ntabs ? b->tabs[b->cur] : NULL; }

bool browser_is_current(tab_t *t) { return cur(t->b) == t; }

/* ---- geometry ------------------------------------------------------------------------- */
static int top_h(void) { return B_TAB_H + B_TOOL_H + B_BM_H; }

static rect_t content_rect(browser_t *b) {
    int W = win_width(b->win), H = win_height(b->win);
    int bottom = H - B_STATUS_H - (b->find_open ? B_FIND_H : 0);
    int h = bottom - top_h();
    return rect_make(0, top_h(), W, h > 0 ? h : 0);
}

rect_t browser_view(browser_t *b) {
    rect_t r = content_rect(b);
    r.w -= B_SB;
    tab_t *t = cur(b);
    if (t && t->styled && t->pg.lay.width > r.w) r.h -= B_SB;
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

static int tab_w(browser_t *b) {
    int avail = win_width(b->win) - 16 - 36;
    int w = b->ntabs ? avail / b->ntabs : 220;
    if (w > 220) w = 220;
    if (w < 48) w = 48;
    return w;
}

static rect_t tab_rect(browser_t *b, int i) { int w = tab_w(b); return rect_make(8 + i * w, 6, w - 2, B_TAB_H - 6); }
static rect_t tabx_rect(browser_t *b, int i) { rect_t r = tab_rect(b, i); return rect_make(r.x + r.w - 24, r.y + (r.h - 18) / 2, 18, 18); }
static rect_t newtab_rect(browser_t *b) { return rect_make(8 + b->ntabs * tab_w(b) + 4, 8, 26, 22); }

#define TB_Y (B_TAB_H + 4)
static rect_t btn_rect(browser_t *b, int hs) {
    int W = win_width(b->win);
    tab_t *t = cur(b);
    switch (hs) {
    case HS_BACK: return rect_make(6, TB_Y, 32, 32);
    case HS_FWD: return rect_make(40, TB_Y, 32, 32);
    case HS_RELOAD: return rect_make(74, TB_Y, 32, 32);
    case HS_HOME: return rect_make(108, TB_Y, 32, 32);
    case HS_LOCK: return rect_make(146, TB_Y + 4, 22, 24);
    case HS_ZOOM: return (t && t->zoom != 100) ? rect_make(W - 132, TB_Y + 4, 46, 24) : rect_make(0, 0, 0, 0);
    case HS_STAR: return rect_make(W - 80, TB_Y, 32, 32);
    case HS_MENU: return rect_make(W - 42, TB_Y, 32, 32);
    default: return rect_make(0, 0, 0, 0);
    }
}

static rect_t addr_rect(browser_t *b) {
    int W = win_width(b->win);
    tab_t *t = cur(b);
    int right = 84 + ((t && t->zoom != 100) ? 52 : 0);
    return rect_make(170, B_TAB_H + 5, MAX(80, W - 170 - right), 30);
}

static int find_y(browser_t *b) { return win_height(b->win) - B_STATUS_H - B_FIND_H; }
static rect_t find_btn(browser_t *b, int hs) {
    int y = find_y(b) + 5;
    if (hs == HS_FPREV) return rect_make(306, y, 28, 26);
    if (hs == HS_FNEXT) return rect_make(338, y, 28, 26);
    return rect_make(win_width(b->win) - 36, y, 26, 26);
}

/* bookmark i's rect, or empty if it does not fit */
static rect_t bm_rect(browser_t *b, int i) {
    int x = 8, y = B_TAB_H + B_TOOL_H + 2, W = win_width(b->win);
    char ui[100];
    for (int k = 0; k < bdata_nbm; k++) {
        web_1252_to_ui(bdata_bm[k].title, ui, sizeof ui);
        int w = MIN(170, 30 + font_text_width(FONT_UI, ui));
        if (x + w > W - 8) return rect_make(0, 0, 0, 0);
        if (k == i) return rect_make(x, y, w, B_BM_H - 4);
        x += w + 4;
    }
    return rect_make(0, 0, 0, 0);
}

static int hit(browser_t *b, int x, int y) {
    if (y < B_TAB_H) {
        for (int i = 0; i < b->ntabs; i++) {
            if (rect_contains(tabx_rect(b, i), x, y)) return HS_TABX + i;
            if (rect_contains(tab_rect(b, i), x, y)) return HS_TAB + i;
        }
        if (b->ntabs < B_MAX_TABS && rect_contains(newtab_rect(b), x, y)) return HS_NEWTAB;
        return HS_NONE;
    }
    if (y < B_TAB_H + B_TOOL_H) {
        static const int hs[] = { HS_BACK, HS_FWD, HS_RELOAD, HS_HOME, HS_LOCK, HS_ZOOM, HS_STAR, HS_MENU };
        for (int i = 0; i < ARRAY_SIZE(hs); i++) if (rect_contains(btn_rect(b, hs[i]), x, y)) return hs[i];
        return HS_NONE;
    }
    if (y < top_h()) {
        for (int i = 0; i < bdata_nbm; i++) if (rect_contains(bm_rect(b, i), x, y)) return HS_BM + i;
        return HS_NONE;
    }
    if (b->find_open && y >= find_y(b) && y < find_y(b) + B_FIND_H) {
        if (rect_contains(find_btn(b, HS_FPREV), x, y)) return HS_FPREV;
        if (rect_contains(find_btn(b, HS_FNEXT), x, y)) return HS_FNEXT;
        if (rect_contains(find_btn(b, HS_FCLOSE), x, y)) return HS_FCLOSE;
    }
    return HS_NONE;
}

/* ---- repainting ------------------------------------------------------------------------ */
static void page_dirty(browser_t *b) {
    V(b)->pc_valid = false;
    rect_t r = content_rect(b);
    win_invalidate_rect(b->win, r.x, r.y, r.w, r.h);
}

static void update_title(browser_t *b) {
    tab_t *t = cur(b);
    char title[200];
    if (t && t->title[0]) snprintf(title, sizeof title, "%s - Web Browser", t->title);
    else strlcpy(title, "Web Browser", sizeof title);
    if (strcmp(win_get_title(b->win), title)) win_set_title(b->win, title);
}

void browser_tab_changed(tab_t *t) {
    browser_t *b = t->b;
    V(b)->chrome_dirty = true;
    if (browser_is_current(t)) update_title(b);
}

void browser_page_changed(tab_t *t) {
    if (browser_is_current(t)) page_dirty(t->b);
}

void browser_set_address(browser_t *b, const char *url) {
    tab_t *t = cur(b);
    if (!url) url = t ? t->url : "";
    if (!strcmp(url, B_HOME_URL) || !strcmp(url, "about:blank")) url = "";
    char ui[B_URL_MAX];
    strlcpy(ui, url, sizeof ui);
    if (strcmp(ui_get_text(b->addr), ui)) ui_set_text(b->addr, ui);
}

static void set_status(browser_t *b, const char *msg) {
    strlcpy(b->status, msg, sizeof b->status);
    V(b)->status_until = sys_uptime_ms() + 4000;
    V(b)->chrome_dirty = true;
}

/* ---- tabs ---------------------------------------------------------------------------- */
static void switch_tab(browser_t *b, int i) {
    if (i < 0 || i >= b->ntabs) return;
    b->cur = i;
    tab_t *t = cur(b);
    browser_set_address(b, NULL);
    update_title(b);
    rect_t v = browser_view(b);
    if (t->styled && t->pg.lay.vw != v.w) tab_restyle(t);
    if (b->find_open) browser_find_update(b);
    page_dirty(b);
    win_invalidate(b->win);
}

tab_t *browser_new_tab(browser_t *b, const char *url, bool focus) {
    if (b->ntabs >= B_MAX_TABS) { set_status(b, "Close a tab to open another one"); return NULL; }
    tab_t *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->b = b;
    t->zoom = 100;
    t->focus_ctrl = t->hover_link = t->hl_cur = -1;
    int at = b->ntabs ? b->cur + 1 : 0;
    memmove(&b->tabs[at + 1], &b->tabs[at], sizeof(tab_t *) * (size_t)(b->ntabs - at));
    b->tabs[at] = t;
    b->ntabs++;
    if (at <= b->cur && b->ntabs > 1 && !focus) b->cur++;
    if (focus || b->ntabs == 1) b->cur = at;
    tab_go(t, url && *url ? url : B_HOME_URL, NAV_NEW);
    if (focus || b->ntabs == 1) {
        switch_tab(b, at);
        if (!url || !*url || !strcmp(url, B_HOME_URL)) { ui_focus(b->addr); ui_textbox_select_all(b->addr); }
        else ui_focus(b->content);
    }
    win_invalidate(b->win);
    return t;
}

static void close_tab(browser_t *b, int i) {
    if (i < 0 || i >= b->ntabs) return;
    tab_t *t = b->tabs[i];
    memmove(&b->tabs[i], &b->tabs[i + 1], sizeof(tab_t *) * (size_t)(b->ntabs - i - 1));
    b->ntabs--;
    tab_free(t);
    if (!b->ntabs) { win_close(b->win); return; }
    if (b->cur > i || b->cur >= b->ntabs) b->cur = MAX(0, b->cur - 1);
    switch_tab(b, b->cur);
}

/* ---- typed text -> URL ------------------------------------------------------------------ */
static bool looks_like_host(const char *s, size_t n, bool *ip, bool *port) {
    *ip = *port = false;
    size_t hl = 0;
    while (hl < n && s[hl] != '/' && s[hl] != '?' && s[hl] != '#') hl++;
    size_t colon = hl;
    for (size_t i = 0; i < hl; i++) if (s[i] == ':') { colon = i; break; }
    if (colon < hl) {
        if (colon + 1 >= hl) return false;
        for (size_t i = colon + 1; i < hl; i++) if (!isdigit((unsigned char)s[i])) return false;
        *port = true;
    }
    if (!colon) return false;
    if (colon == 9 && !strncasecmp(s, "localhost", 9)) return true;
    int dots = 0, digits_only = 1;
    for (size_t i = 0; i < colon; i++) {
        char c = s[i];
        if (c == '.') { dots++; if (i == 0 || s[i - 1] == '.') return false; continue; }
        if (!isalnum((unsigned char)c) && c != '-') return false;
        if (!isdigit((unsigned char)c)) digits_only = 0;
    }
    if (!dots || s[colon - 1] == '.') return false;
    if (digits_only) { *ip = dots == 3; return dots == 3; }
    /* the last label (top-level domain) is letters */
    size_t k = colon;
    while (k > 0 && s[k - 1] != '.') k--;
    if (colon - k < 2) return false;
    for (size_t i = k; i < colon; i++) if (!isalpha((unsigned char)s[i])) return false;
    return true;
}

const char *browser_input_to_url(const char *in, char *out, size_t size) {
    while (*in == ' ') in++;
    size_t n = strlen(in);
    while (n && in[n - 1] == ' ') n--;
    if (!n || n >= size - 16) return NULL;
    static const char *schemes[] = { "http://", "https://", "file:", "about:", "view-source:", "data:" };
    for (int i = 0; i < ARRAY_SIZE(schemes); i++)
        if (n >= strlen(schemes[i]) && !strncasecmp(in, schemes[i], strlen(schemes[i]))) {
            memcpy(out, in, n);
            out[n] = 0;
            return out;
        }
    if (in[0] == '/') { snprintf(out, size, "file://%.*s", (int)n, in); return out; }
    bool space = false;
    for (size_t i = 0; i < n; i++) if (in[i] == ' ') space = true;
    bool ip, port;
    if (!space && looks_like_host(in, n, &ip, &port)) {
        bool local = ip || port || !strncasecmp(in, "localhost", 9);
        snprintf(out, size, "%s%.*s", local ? "http://" : "https://", (int)n, in);
        if (!memchr(in, '/', n)) strlcat(out, "/", size);
        return out;
    }
    strlcpy(out, B_SEARCH_URL, size);
    char q[1024];
    size_t k = n < sizeof q - 1 ? n : sizeof q - 1;
    memcpy(q, in, k);
    q[k] = 0;
    web_urlencode(q, out, size);
    return out;
}

/* ---- toolbar glyphs ----------------------------------------------------------------------- */
static void stroke(surface_t *s, double x0, double y0, double x1, double y1, uint32_t c) {
    double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
    if (len < 0.01) return;
    double nx = -dy / len * 0.45, ny = dx / len * 0.45;
    gfx_line_aa(s, x0 + nx, y0 + ny, x1 + nx, y1 + ny, c);
    gfx_line_aa(s, x0 - nx, y0 - ny, x1 - nx, y1 - ny, c);
}

static void arc(surface_t *s, double cx, double cy, double r, double a0, double a1, uint32_t c) {
    int n = 18;
    double px = cx + r * cos(a0), py = cy + r * sin(a0);
    for (int i = 1; i <= n; i++) {
        double a = a0 + (a1 - a0) * i / n;
        double x = cx + r * cos(a), y = cy + r * sin(a);
        stroke(s, px, py, x, y, c);
        px = x;
        py = y;
    }
}

static void star_points(point_t *p, double cx, double cy, double ro, double ri) {
    for (int i = 0; i < 10; i++) {
        double a = -M_PI / 2 + i * M_PI / 5, r = (i & 1) ? ri : ro;
        p[i].x = (int)lround(cx + r * cos(a));
        p[i].y = (int)lround(cy + r * sin(a));
    }
}

static void draw_padlock(surface_t *s, int x, int y, uint32_t c) {
    arc(s, x + 6, y + 6, 3.6, M_PI, 2 * M_PI, c);
    stroke(s, x + 2.4, y + 6, x + 2.4, y + 8.5, c);
    stroke(s, x + 9.6, y + 6, x + 9.6, y + 8.5, c);
    gfx_fill_rounded_rect(s, x, y + 8, 12, 9, 2, c);
}

static void draw_button(browser_t *b, surface_t *s, int hs, bool enabled) {
    const theme_t *th = theme_get();
    rect_t r = btn_rect(b, hs);
    if (enabled && (b->hover == hs || b->press == hs))
        gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, b->press == hs ? th->button_pressed : th->button_hover);
    uint32_t c = enabled ? th->window_text : th->disabled_text;
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    tab_t *t = cur(b);
    switch (hs) {
    case HS_BACK:
        stroke(s, cx + 6, cy, cx - 6, cy, c);
        stroke(s, cx - 6, cy, cx - 1, cy - 5, c);
        stroke(s, cx - 6, cy, cx - 1, cy + 5, c);
        break;
    case HS_FWD:
        stroke(s, cx - 6, cy, cx + 6, cy, c);
        stroke(s, cx + 6, cy, cx + 1, cy - 5, c);
        stroke(s, cx + 6, cy, cx + 1, cy + 5, c);
        break;
    case HS_RELOAD:
        if (t && t->loading) {
            stroke(s, cx - 5, cy - 5, cx + 5, cy + 5, c);
            stroke(s, cx + 5, cy - 5, cx - 5, cy + 5, c);
        } else {
            arc(s, cx, cy, 6, -M_PI * 0.35, M_PI * 1.45, c);
            double ax = cx + 6 * cos(-M_PI * 0.35), ay = cy + 6 * sin(-M_PI * 0.35);
            gfx_fill_triangle(s, (int)ax - 4, (int)ay - 3, (int)ax + 3, (int)ay - 4, (int)ax + 1, (int)ay + 3, c);
        }
        break;
    case HS_HOME:
        stroke(s, cx - 8, cy, cx, cy - 7, c);
        stroke(s, cx, cy - 7, cx + 8, cy, c);
        stroke(s, cx - 5.5, cy - 2, cx - 5.5, cy + 7, c);
        stroke(s, cx + 5.5, cy - 2, cx + 5.5, cy + 7, c);
        stroke(s, cx - 5.5, cy + 7, cx + 5.5, cy + 7, c);
        gfx_fill_rect(s, (int)cx - 1, (int)cy + 2, 3, 5, c);
        break;
    case HS_STAR: {
        point_t p[10];
        star_points(p, cx, cy + 0.5, 8.5, 3.6);
        bool on = t && bdata_bookmark_find(t->url) >= 0;
        if (on) gfx_fill_polygon(s, p, 10, RGB(244, 180, 0));
        else for (int i = 0; i < 10; i++) stroke(s, p[i].x, p[i].y, p[(i + 1) % 10].x, p[(i + 1) % 10].y, c);
        break;
    }
    case HS_MENU:
        for (int i = -1; i <= 1; i++) stroke(s, cx - 7, cy + i * 5, cx + 7, cy + i * 5, c);
        break;
    case HS_LOCK:
        if (t && t->https && (!t->internal || !t->cert_ok)) {
            if (t->cert_ok) draw_padlock(s, r.x + 5, r.y + 3, th->dark ? RGB(150, 210, 160) : RGB(40, 120, 60));
            else {
                uint32_t red = RGB(214, 40, 40);
                draw_padlock(s, r.x + 5, r.y + 3, red);
                gfx_thick_line(s, r.x + 2, r.y + 21, r.x + 20, r.y + 3, 4, th->window_bg);
                stroke(s, r.x + 2.5, r.y + 20.5, r.x + 19.5, r.y + 3.5, red);
            }
        } else if (t && !strncasecmp(t->url, "http://", 7)) {
            ui_draw_glyph(s, UI_GLYPH_INFO, r.x + 3, r.y + 4, 16, th->disabled_text);
        }
        break;
    case HS_ZOOM: {
        if (!t || t->zoom == 100) break;
        gfx_rounded_rect(s, r.x, r.y, r.w, r.h, 5, th->window_border);
        char z[12];
        snprintf(z, sizeof z, "%d%%", t->zoom);
        gfx_text_aligned(s, r, z, th->window_text, FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
        break;
    }
    }
}

static void draw_globe(surface_t *s, int x, int y, uint32_t c) {
    gfx_circle_aa(s, x + 8, y + 8, 6.5, 1.2, c);
    gfx_line_aa(s, x + 1.5, y + 8, x + 14.5, y + 8, c);
    gfx_line_aa(s, x + 8, y + 1.5, x + 8, y + 14.5, c);
    for (int i = 0; i < 12; i++) {
        double a0 = -M_PI / 2 + i * M_PI / 12, a1 = a0 + M_PI / 12;
        gfx_line_aa(s, x + 8 + 3 * cos(a0), y + 8 + 6.5 * sin(a0), x + 8 + 3 * cos(a1), y + 8 + 6.5 * sin(a1), c);
        gfx_line_aa(s, x + 8 - 3 * cos(a0), y + 8 + 6.5 * sin(a0), x + 8 - 3 * cos(a1), y + 8 + 6.5 * sin(a1), c);
    }
}

static void draw_tabs(browser_t *b, surface_t *s) {
    const theme_t *th = theme_get();
    uint32_t strip = color_shade(th->window_bg, th->dark ? -16 : -24);
    gfx_fill_rect(s, 0, 0, s->w, B_TAB_H, strip);
    for (int i = 0; i < b->ntabs; i++) {
        tab_t *t = b->tabs[i];
        rect_t r = tab_rect(b, i);
        bool active = i == b->cur;
        if (active) {
            gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h + 8, 8, th->window_bg);
        } else {
            if (b->hover == HS_TAB + i || b->hover == HS_TABX + i)
                gfx_fill_rounded_rect(s, r.x + 2, r.y + 2, r.w - 4, r.h - 6, 6, color_shade(strip, th->dark ? 14 : 14));
            if (i + 1 != b->cur && i + 1 < b->ntabs) gfx_vline(s, r.x + r.w + 1, r.y + 6, r.h - 14, color_shade(strip, th->dark ? 30 : -30));
        }
        uint32_t tc = active ? th->window_text : color_mix(th->window_text, strip, 70);
        int ix = r.x + 10, iy = r.y + (r.h - 16) / 2;
        if (t->loading) {
            double a = b->spin * 0.35;
            gfx_circle_aa(s, ix + 8, iy + 8, 6, 2, color_alpha(th->accent, 50));
            for (int k = 0; k < 8; k++) {
                double a0 = a + k * 0.15, a1 = a0 + 0.15;
                gfx_line_aa(s, ix + 8 + 6 * cos(a0), iy + 8 + 6 * sin(a0), ix + 8 + 6 * cos(a1), iy + 8 + 6 * sin(a1), th->accent);
                gfx_line_aa(s, ix + 8 + 5 * cos(a0), iy + 8 + 5 * sin(a0), ix + 8 + 5 * cos(a1), iy + 8 + 5 * sin(a1), th->accent);
            }
        } else {
            draw_globe(s, ix, iy, tc);
        }
        int tw = r.w - 10 - 16 - 8 - 28;
        const char *title = t->title[0] ? t->title : (t->loading ? "Loading..." : "New Tab");
        if (tw > 8) gfx_text_ellipsis(s, ix + 22, r.y + (r.h - font_height(FONT_UI)) / 2, tw, title, tc, FONT_UI);
        if (r.w > 90 || active) {
            rect_t x = tabx_rect(b, i);
            if (b->hover == HS_TABX + i) gfx_fill_circle_aa(s, x.x + 9, x.y + 9, 9, color_alpha(th->window_text, 30));
            ui_draw_glyph(s, UI_GLYPH_CLOSE, x.x + 5, x.y + 5, 8, tc);
        }
    }
    if (b->ntabs < B_MAX_TABS) {
        rect_t r = newtab_rect(b);
        if (b->hover == HS_NEWTAB) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, color_shade(strip, 14));
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        gfx_fill_rect(s, cx - 6, cy - 1, 13, 2, th->window_text);
        gfx_fill_rect(s, cx - 1, cy - 6, 2, 13, th->window_text);
    }
}

static void draw_bookmarks(browser_t *b, surface_t *s) {
    const theme_t *th = theme_get();
    int y = B_TAB_H + B_TOOL_H;
    gfx_hline(s, 0, top_h() - 1, s->w, th->window_border);
    if (!bdata_nbm) {
        gfx_text(s, 12, y + (B_BM_H - font_height(FONT_UI)) / 2 - 1, "Bookmarks appear here - click the star to add this page",
                 th->disabled_text, FONT_UI);
        return;
    }
    char ui[100];
    for (int i = 0; i < bdata_nbm; i++) {
        rect_t r = bm_rect(b, i);
        if (!r.w) break;
        if (b->hover == HS_BM + i) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 5, th->button_hover);
        draw_globe(s, r.x + 6, r.y + (r.h - 16) / 2, th->window_text);
        web_1252_to_ui(bdata_bm[i].title, ui, sizeof ui);
        gfx_text_ellipsis(s, r.x + 26, r.y + (r.h - font_height(FONT_UI)) / 2, r.w - 30, ui, th->window_text, FONT_UI);
    }
}

static void draw_find(browser_t *b, surface_t *s) {
    const theme_t *th = theme_get();
    int y = find_y(b);
    gfx_fill_rect(s, 0, y, s->w, B_FIND_H, th->panel_bg);
    gfx_hline(s, 0, y, s->w, th->window_border);
    gfx_text(s, 12, y + (B_FIND_H - font_height(FONT_UI)) / 2, "Find:", th->window_text, FONT_UI);
    int hs[2] = { HS_FPREV, HS_FNEXT };
    for (int i = 0; i < 2; i++) {
        rect_t r = find_btn(b, hs[i]);
        ui_draw_button(s, r, "", b->hover == hs[i] ? UI_STATE_HOVER : 0);
        ui_draw_glyph(s, i ? UI_GLYPH_ARROW_DOWN : UI_GLYPH_ARROW_UP, r.x + 8, r.y + 7, 12, th->window_text);
    }
    tab_t *t = cur(b);
    char msg[64] = "";
    const char *needle = ui_get_text(b->find_box);
    if (t && needle[0]) {
        if (t->nhl) snprintf(msg, sizeof msg, "%d of %d", t->hl_cur + 1, t->nhl);
        else strlcpy(msg, "Not found", sizeof msg);
    }
    gfx_text(s, 378, y + (B_FIND_H - font_height(FONT_UI)) / 2, msg, t && needle[0] && !t->nhl ? RGB(200, 50, 50) : th->window_text, FONT_UI);
    rect_t r = find_btn(b, HS_FCLOSE);
    if (b->hover == HS_FCLOSE) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 5, th->button_hover);
    ui_draw_glyph(s, UI_GLYPH_CLOSE, r.x + 8, r.y + 8, 10, th->window_text);
}

static void draw_status(browser_t *b, surface_t *s) {
    const theme_t *th = theme_get();
    int y = s->h - B_STATUS_H;
    gfx_fill_rect(s, 0, y, s->w, B_STATUS_H, th->panel_bg);
    gfx_hline(s, 0, y, s->w, th->window_border);
    int nw = font_text_width(FONT_UI, NOTE_TEXT);
    int nx = s->w - nw - 10;
    int ty = y + (B_STATUS_H - font_height(FONT_UI)) / 2;
    if (nx > 200) {
        ui_draw_glyph(s, UI_GLYPH_WARNING, nx - 20, y + 3, 15, RGB(220, 140, 0));
        gfx_text(s, nx, ty, NOTE_TEXT, th->dark ? RGB(255, 190, 90) : RGB(150, 80, 0), FONT_UI);
    } else nx = s->w;
    tab_t *t = cur(b);
    char msg[400] = "";
    if (t && t->hover_link >= 0 && t->hover_link < t->pg.nlinks) {
        const char *href = t->pg.links[t->hover_link].href;
        char abs[B_URL_MAX];
        if (!strncasecmp(href, "about:", 6) || !url_resolve(t->pg.base, href, abs, sizeof abs)) strlcpy(abs, href, sizeof abs);
        web_1252_to_ui(abs, msg, sizeof msg);
    } else if (b->status[0] && (int32_t)(sys_uptime_ms() - V(b)->status_until) < 0) {
        strlcpy(msg, b->status, sizeof msg);
    } else if (t && t->loading) {
        url_t u;
        const char *host = url_parse(t->req_url, &u) ? u.host : t->req_url;
        if (t->got) snprintf(msg, sizeof msg, "Loading %s - %u KB", host, (unsigned)(t->got / 1024));
        else snprintf(msg, sizeof msg, "Connecting to %s...", host);
    } else if (browser_download_status()) {
        strlcpy(msg, browser_download_status(), sizeof msg);
    } else if (t && t->styled) {
        int n = 0;
        for (int i = 0; i < t->pg.nimgs; i++) if (t->pg.imgs[i].state < IMG_OK) n++;
        if (n) snprintf(msg, sizeof msg, "Loading images (%d left)", n);
    }
    gfx_text_ellipsis(s, 10, ty, nx - 40, msg, th->window_text, FONT_UI);
}

static void paint(browser_t *b) {
    surface_t *s = win_surface(b->win);
    const theme_t *th = theme_get();
    tab_t *t = cur(b);
    draw_tabs(b, s);
    gfx_fill_rect(s, 0, B_TAB_H, s->w, B_TOOL_H + B_BM_H, th->window_bg);
    draw_button(b, s, HS_BACK, t && t->hpos > 0);
    draw_button(b, s, HS_FWD, t && t->hpos + 1 < t->nhist);
    draw_button(b, s, HS_RELOAD, t != NULL);
    draw_button(b, s, HS_HOME, true);
    draw_button(b, s, HS_LOCK, true);
    draw_button(b, s, HS_ZOOM, true);
    draw_button(b, s, HS_STAR, t && strncmp(t->url, "about:", 6));
    draw_button(b, s, HS_MENU, true);
    draw_bookmarks(b, s);
    if (b->find_open) draw_find(b, s);
    draw_status(b, s);
}

/* ---- the page view (a custom widget) ---------------------------------------------------- */
static rect_t vbar_rect(browser_t *b) { rect_t c = content_rect(b), v = browser_view(b); return rect_make(c.x + c.w - B_SB, c.y, B_SB, v.h); }
static rect_t hbar_rect(browser_t *b) { rect_t v = browser_view(b); return rect_make(v.x, v.y + v.h, v.w, B_SB); }
static bool has_hbar(browser_t *b) { tab_t *t = cur(b); return t && t->styled && t->pg.lay.width > browser_view(b).w; }

static void thumb(int len, int total, int page, int pos, int *tp, int *tl) {
    *tl = MAX(24, (int)((int64_t)len * page / MAX(1, total)));
    if (*tl > len) *tl = len;
    int maxpos = total - page;
    *tp = maxpos > 0 ? (int)((int64_t)(len - *tl) * pos / maxpos) : 0;
}

static void content_draw(widget_t *w, surface_t *s, rect_t r) {
    browser_t *b = ui_get_ctx(w);
    bview_t *bv = V(b);
    tab_t *t = cur(b);
    rect_t v = browser_view(b);
    (void)r;
    if (v.w <= 0 || v.h <= 0) return;
    if (!bv->pc || bv->pc->w != v.w || bv->pc->h != v.h) {
        if (bv->pc) surface_destroy(bv->pc);
        bv->pc = surface_create(v.w, v.h);
        bv->pc_valid = false;
        if (!bv->pc) return;
    }
    if (!bv->pc_valid) {
        surface_reset_clip(bv->pc);
        if (t && t->styled) {
            web_paint_opts_t o;
            memset(&o, 0, sizeof o);
            o.hover_link = t->hover_link;
            o.focus_ctrl = t->focus_ctrl;
            o.caret_on = b->caret_on;
            o.hl_item = t->hl_item;
            o.hl_off = t->hl_off;
            o.hl_len = t->hl_len;
            o.nhl = t->nhl;
            o.hl_current = t->hl_cur;
            web_paint(&t->pg, bv->pc, rect_make(0, 0, v.w, v.h), t->sx, t->sy, &o);
        } else {
            gfx_fill_rect(bv->pc, 0, 0, v.w, v.h, RGB(255, 255, 255));
        }
        bv->pc_valid = true;
    }
    gfx_blit(s, v.x, v.y, bv->pc);
    int total = t && t->styled ? t->pg.lay.height : 0;
    ui_draw_scrollbar(s, vbar_rect(b), true, total, v.h, t ? t->sy : 0, b->drag == 1 ? UI_STATE_PRESSED : 0);
    if (has_hbar(b)) {
        ui_draw_scrollbar(s, hbar_rect(b), false, t->pg.lay.width, v.w, t->sx, b->drag == 2 ? UI_STATE_PRESSED : 0);
        gfx_fill_rect(s, v.x + v.w, v.y + v.h, B_SB, B_SB, theme_get()->scrollbar_bg);
    }
}

static void scroll_to(browser_t *b, int x, int y) {
    tab_t *t = cur(b);
    if (!t) return;
    int ox = t->sx, oy = t->sy;
    tab_scroll_to(t, x, y);
    if (t->sx != ox || t->sy != oy) page_dirty(b);
}

static void scroll_into_view(browser_t *b, int y, int h) {
    tab_t *t = cur(b);
    rect_t v = browser_view(b);
    if (y < t->sy + 20) scroll_to(b, t->sx, y - v.h / 3);
    else if (y + h > t->sy + v.h - 20) scroll_to(b, t->sx, y - v.h / 3);
}

/* ---- form controls ------------------------------------------------------------------------ */
static void select_pick(int item, void *ctx) {
    browser_t *b = ctx;
    tab_t *t = cur(b);
    if (!t || t->focus_ctrl < 0 || t->focus_ctrl >= t->pg.nctrls) return;
    web_ctrl_t *c = &t->pg.ctrls[t->focus_ctrl];
    if (c->type == C_SELECT && item >= 0 && item < c->nopts) { c->sel = item; page_dirty(b); }
}

static void ctrl_reset(tab_t *t, web_node_t *form) {
    for (int i = 0; i < t->pg.nctrls; i++) {
        web_ctrl_t *c = &t->pg.ctrls[i];
        if (c->form != form) continue;
        if (c->type == C_CHECKBOX || c->type == C_RADIO) c->checked = c->init_checked;
        else if (c->type == C_SELECT) c->sel = c->init_sel;
        else if (c->type == C_TEXT || c->type == C_PASSWORD || c->type == C_TEXTAREA) {
            web_ctrl_set_value(c, c->init_value ? c->init_value : "", -1);
            c->caret = c->vlen;
        }
    }
}

static bool is_text(const web_ctrl_t *c) { return c->type == C_TEXT || c->type == C_PASSWORD || c->type == C_TEXTAREA; }

static void ctrl_click(browser_t *b, tab_t *t, int idx, int dx, int dy) {
    web_ctrl_t *c = &t->pg.ctrls[idx];
    if (c->disabled) return;
    t->focus_ctrl = idx;
    b->caret_on = true;
    b->caret_t = sys_uptime_ms();
    switch (c->type) {
    case C_TEXT: case C_PASSWORD: {
        const font_t *f = c->font ? c->font : FONT_UI;
        if (c->type == C_PASSWORD) {
            int cw = font_char_width(f, 0x95);
            c->caret = cw ? (dx - c->x - 5 + c->scroll + cw / 2) / cw : c->vlen;
        } else c->caret = font_hit_test(f, c->value, dx - c->x - 5 + c->scroll);
        c->caret = CLAMP(c->caret, 0, c->vlen);
        break;
    }
    case C_TEXTAREA: {
        const font_t *f = c->font ? c->font : FONT_MONO;
        int line = (dy - c->y - 4) / MAX(1, font_height(f));
        const char *p = c->value;
        for (int l = 0; l < line && strchr(p, '\n'); l++) p = strchr(p, '\n') + 1;
        int len = (int)strcspn(p, "\n");
        char tmp[1024];
        int k = MIN(len, (int)sizeof tmp - 1);
        memcpy(tmp, p, (size_t)k);
        tmp[k] = 0;
        c->caret = (int)(p - c->value) + font_hit_test(f, tmp, dx - c->x - 5);
        break;
    }
    case C_CHECKBOX:
        c->checked = !c->checked;
        break;
    case C_RADIO:
        for (int i = 0; i < t->pg.nctrls; i++) {
            web_ctrl_t *o = &t->pg.ctrls[i];
            if (o != c && o->type == C_RADIO && o->form == c->form && o->name && c->name && !strcmp(o->name, c->name)) o->checked = false;
        }
        c->checked = true;
        break;
    case C_SELECT: {
        if (!c->nopts) break;
        size_t cap = 64;
        for (int i = 0; i < c->nopts; i++) cap += strlen(c->opt_text[i]) + 4;
        char *items = malloc(cap);
        if (!items) break;
        items[0] = 0;
        char ui[200];
        for (int i = 0; i < c->nopts; i++) {
            web_1252_to_ui(c->opt_text[i], ui, sizeof ui);
            for (char *p = ui; *p; p++) if (*p == '|') *p = '/';
            if (i) strcat(items, "|");
            if (!ui[0] || ui[0] == '-' || ui[0] == '!') strcat(items, " ");
            strcat(items, ui);
        }
        rect_t v = browser_view(b);
        ui_popup_menu(b->win, v.x + c->x - t->sx, v.y + c->y - t->sy + c->h, items, select_pick, b);
        free(items);
        break;
    }
    case C_SUBMIT: case C_IMAGE:
        tab_submit(t, idx);
        return;
    case C_RESET:
        ctrl_reset(t, c->form);
        break;
    default:
        break;
    }
    page_dirty(b);
}

static void ctrl_insert(web_ctrl_t *c, const char *s, int n) {
    if (c->readonly) return;
    if (c->maxlen > 0 && c->vlen + n > c->maxlen) n = c->maxlen - c->vlen;
    if (n <= 0) return;
    char *nv = malloc((size_t)c->vlen + (size_t)n + 1);
    if (!nv) return;
    memcpy(nv, c->value, (size_t)c->caret);
    memcpy(nv + c->caret, s, (size_t)n);
    memcpy(nv + c->caret + n, c->value + c->caret, (size_t)(c->vlen - c->caret) + 1);
    int caret = c->caret + n;
    web_ctrl_set_value(c, nv, c->vlen + n);
    c->caret = caret;
    free(nv);
}

static bool ctrl_key(browser_t *b, tab_t *t, const event_t *ev) {
    web_ctrl_t *c = &t->pg.ctrls[t->focus_ctrl];
    b->caret_on = true;
    b->caret_t = sys_uptime_ms();
    if (ev->type == EV_CHAR) {
        if (!is_text(c)) return false;
        char ch = (char)ev->ch;
        ctrl_insert(c, &ch, 1);
        page_dirty(b);
        return true;
    }
    bool ctrl = ev->mods & MOD_CTRL;
    switch (ev->key) {
    case KEY_ESCAPE: t->focus_ctrl = -1; page_dirty(b); return true;
    case KEY_SPACE:
        if (c->type == C_CHECKBOX || c->type == C_RADIO || c->type == C_SUBMIT || c->type == C_RESET || c->type == C_SELECT) {
            ctrl_click(b, t, t->focus_ctrl, c->x + 2, c->y + 2);
            return true;
        }
        return false;
    case KEY_ENTER:
        if (c->type == C_TEXTAREA) { ctrl_insert(c, "\n", 1); page_dirty(b); return true; }
        if (c->type == C_TEXT || c->type == C_PASSWORD || c->type == C_SUBMIT || c->type == C_IMAGE ||
            c->type == C_CHECKBOX || c->type == C_RADIO) { tab_submit(t, t->focus_ctrl); return true; }
        return false;
    default: break;
    }
    if (!is_text(c)) return false;
    switch (ev->key) {
    case KEY_LEFT: if (c->caret > 0) c->caret--; break;
    case KEY_RIGHT: if (c->caret < c->vlen) c->caret++; break;
    case KEY_HOME: c->caret = 0; break;
    case KEY_END: c->caret = c->vlen; break;
    case KEY_BACKSPACE:
        if (c->caret > 0 && !c->readonly) {
            memmove(c->value + c->caret - 1, c->value + c->caret, (size_t)(c->vlen - c->caret) + 1);
            c->vlen--;
            c->caret--;
        }
        break;
    case KEY_DELETE:
        if (c->caret < c->vlen && !c->readonly) {
            memmove(c->value + c->caret, c->value + c->caret + 1, (size_t)(c->vlen - c->caret));
            c->vlen--;
        }
        break;
    default:
        if (ctrl && ev->key == 'v') {
            const char *clip = clipboard_get();
            char buf[1024];
            size_t k = 0;
            for (; *clip && k < sizeof buf - 1; clip++)
                if (*clip != '\r' && (*clip != '\n' || c->type == C_TEXTAREA)) buf[k++] = *clip;
            ctrl_insert(c, buf, (int)k);
            break;
        }
        if (ctrl && ev->key == 'a') { c->caret = c->vlen; break; }
        return false;
    }
    page_dirty(b);
    return true;
}

/* ---- find in page ------------------------------------------------------------------------ */
static int lowc(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    return c;
}

static void hl_add(tab_t *t, int item, int off, int len) {
    if (t->nhl >= t->hlcap) {
        int nc = t->hlcap ? t->hlcap * 2 : 64;
        if (nc > 5000) return;
        int *a = realloc(t->hl_item, sizeof(int) * nc), *o, *l;
        if (!a) return;
        t->hl_item = a;
        o = realloc(t->hl_off, sizeof(int) * nc);
        if (!o) return;
        t->hl_off = o;
        l = realloc(t->hl_len, sizeof(int) * nc);
        if (!l) return;
        t->hl_len = l;
        t->hlcap = nc;
    }
    t->hl_item[t->nhl] = item;
    t->hl_off[t->nhl] = off;
    t->hl_len[t->nhl] = len;
    t->nhl++;
}

static void find_goto(browser_t *b, int k) {
    tab_t *t = cur(b);
    if (!t || !t->nhl) return;
    t->hl_cur = (k % t->nhl + t->nhl) % t->nhl;
    web_item_t *it = &t->pg.lay.items[t->hl_item[t->hl_cur]];
    scroll_into_view(b, it->y, it->h);
    rect_t v = browser_view(b);
    int x = it->x + font_text_width_n(it->font, it->text, t->hl_off[t->hl_cur]);
    if (x < t->sx || x > t->sx + v.w - 40) scroll_to(b, x - v.w / 2, t->sy);
    page_dirty(b);
}

void browser_find_update(browser_t *b) {
    tab_t *t = cur(b);
    if (!t) return;
    t->nhl = 0;
    t->hl_cur = -1;
    const char *needle = b->find_open ? ui_get_text(b->find_box) : "";
    int nl = (int)strlen(needle);
    if (nl && t->styled) {
        web_layout_t *L = &t->pg.lay;
        for (int i = 0; i < L->nitems; i++) {
            web_item_t *it = &L->items[i];
            if (it->kind != WI_TEXT || it->len < nl) continue;
            for (int k = 0; k + nl <= it->len; k++) {
                int j = 0;
                while (j < nl && lowc((unsigned char)it->text[k + j]) == lowc((unsigned char)needle[j])) j++;
                if (j == nl) { hl_add(t, i, k, nl); k += nl - 1; }
            }
        }
        /* the first match below the top of the view */
        for (int k = 0; k < t->nhl; k++)
            if (L->items[t->hl_item[k]].y >= t->sy) { t->hl_cur = k; break; }
        if (t->hl_cur < 0 && t->nhl) t->hl_cur = 0;
    }
    page_dirty(b);
    win_invalidate_rect(b->win, 0, find_y(b), win_width(b->win), B_FIND_H);
}

static void find_changed(widget_t *w, void *ctx) {
    (void)w;
    browser_t *b = ctx;
    browser_find_update(b);
    tab_t *t = cur(b);
    if (t && t->hl_cur >= 0) find_goto(b, t->hl_cur);
}

static void find_next(browser_t *b, int dir) {
    tab_t *t = cur(b);
    if (!t || !t->nhl) return;
    find_goto(b, t->hl_cur + dir);
    win_invalidate_rect(b->win, 0, find_y(b), win_width(b->win), B_FIND_H);
}

static void find_submit(widget_t *w, void *ctx) {
    (void)w;
    browser_t *b = ctx;
    find_next(b, (input_mods() & MOD_SHIFT) ? -1 : 1);
}

static void layout_widgets(browser_t *b);

static void find_show(browser_t *b, bool on) {
    b->find_open = on;
    ui_set_visible(b->find_box, on);
    layout_widgets(b);
    if (on) { ui_focus(b->find_box); ui_textbox_select_all(b->find_box); }
    else ui_focus(b->content);
    tab_t *t = cur(b);
    if (t) tab_scroll_to(t, t->sx, t->sy);
    browser_find_update(b);
    win_invalidate(b->win);
}

/* ---- zoom ------------------------------------------------------------------------------- */
static void zoom_step(browser_t *b, int dir) {
    tab_t *t = cur(b);
    if (!t) return;
    int i = 0;
    while (i < ARRAY_SIZE(zoom_steps) - 1 && zoom_steps[i] < t->zoom) i++;
    if (dir > 0 && i < ARRAY_SIZE(zoom_steps) - 1) i++;
    else if (dir < 0 && i > 0) i--;
    int z = dir == 0 ? 100 : zoom_steps[i];
    if (z == t->zoom) return;
    /* keep the same part of the page in view */
    int64_t rel = t->pg.lay.height ? (int64_t)t->sy * 10000 / t->pg.lay.height : 0;
    t->zoom = z;
    layout_widgets(b);
    tab_restyle(t);
    scroll_to(b, 0, (int)(rel * t->pg.lay.height / 10000));
    char msg[48];
    snprintf(msg, sizeof msg, "Zoom %d%%", z);
    set_status(b, msg);
    win_invalidate(b->win);
}

/* ---- menus, settings ---------------------------------------------------------------------- */
static void set_hist_cb(widget_t *w, void *ctx) {
    (void)ctx;
    bdata_history_on = ui_get_value(w) != 0;
    bdata_save_settings();
}

static void cookies_label(browser_t *b) {
    if (!b->settings || !win_is_valid(b->settings) || !b->set_cookies_lbl) return;
    char msg[80];
    int n = cookies_count();
    snprintf(msg, sizeof msg, n == 1 ? "1 cookie is saved." : "%d cookies are saved.", n);
    ui_set_text(b->set_cookies_lbl, msg);
}

static void clear_hist_cb(widget_t *w, void *ctx) {
    browser_t *b = ctx;
    bdata_history_clear();
    ui_set_text(w, "History cleared");
    ui_set_enabled(w, false);
    for (int i = 0; i < b->ntabs; i++)
        if (!strcmp(b->tabs[i]->url, "about:history")) tab_go(b->tabs[i], "about:history", NAV_RELOAD);
}

static void clear_cookies_cb(widget_t *w, void *ctx) {
    browser_t *b = ctx;
    (void)w;
    cookies_clear();
    cookies_label(b);
}

static void settings_close_cb(widget_t *w, void *ctx) { (void)ctx; win_close(ui_window(w)); }

static void open_settings(browser_t *b) {
    if (b->settings && win_is_valid(b->settings)) { win_focus(b->settings); return; }
    window_t *w = win_create_dialog(b->win, "Browser settings", 400, 262, 0);
    if (!w) return;
    b->settings = w;
    widget_t *l = ui_label(w, 18, 14, 360, 22, "History");
    ui_set_font(l, FONT_UI_BOLD);
    b->set_hist = ui_checkbox(w, 18, 40, 360, 22, "Save history", bdata_history_on, set_hist_cb, b);
    ui_button(w, 18, 70, 140, 30, "Clear history", clear_hist_cb, b);
    l = ui_label(w, 18, 118, 360, 22, "Cookies");
    ui_set_font(l, FONT_UI_BOLD);
    b->set_cookies_lbl = ui_label(w, 18, 142, 360, 20, "");
    cookies_label(b);
    ui_button(w, 18, 168, 140, 30, "Clear cookies", clear_cookies_cb, b);
    widget_t *c = ui_button(w, 300, 220, 84, 30, "Close", settings_close_cb, NULL);
    ui_button_set_default(c, true);
}

static void view_source(browser_t *b) {
    tab_t *t = cur(b);
    if (!t || !strncmp(t->url, "view-source:", 12) || !strncmp(t->url, "about:", 6)) return;
    char url[B_URL_MAX + 16];
    snprintf(url, sizeof url, "view-source:%s", t->url);
    tab_t *n = browser_new_tab(b, NULL, true);
    if (!n) return;
    if (t->src && strncasecmp(t->url, "http", 4)) { tab_go(n, url, NAV_REPLACE); return; }
    if (t->src) {
        /* show the copy we have instead of loading the page again */
        char *html = bpage_source(t->url, t->src, t->srclen);
        n->req_mode = NAV_REPLACE;     /* replaces the new tab's start page */
        tab_show_internal(n, url, html);
        n->internal = true;
        snprintf(n->title, sizeof n->title, "Source of %s", t->url);
        browser_set_address(b, url);
        browser_tab_changed(n);
    } else tab_go(n, url, NAV_REPLACE);
    ui_focus(b->content);
}

static void toggle_bookmark(browser_t *b) {
    tab_t *t = cur(b);
    if (!t || !strncmp(t->url, "about:", 6)) return;
    int i = bdata_bookmark_find(t->url);
    if (i >= 0) { bdata_bookmark_remove(i); set_status(b, "Bookmark removed"); }
    else {
        bdata_bookmark_add(t->url, t->pg.doc && t->pg.doc->title[0] ? t->pg.doc->title : t->url);
        set_status(b, "Page added to the bookmarks bar");
    }
    win_invalidate(b->win);
}

static void open_downloads(void) {
    if (!vfs_is_dir(B_DOWNLOADS)) vfs_mkdirs(B_DOWNLOADS);
    shell_open(B_DOWNLOADS);
}

enum { M_NEWTAB = 0, M_HISTORY, M_DOWNLOADS, M_SEP1, M_FIND, M_ZIN, M_ZOUT, M_ZRESET, M_SOURCE, M_SEP2, M_SETTINGS };

static void menu_cb(int item, void *ctx) {
    browser_t *b = ctx;
    switch (item) {
    case M_NEWTAB: browser_new_tab(b, NULL, true); break;
    case M_HISTORY: browser_new_tab(b, "about:history", true); break;
    case M_DOWNLOADS: open_downloads(); break;
    case M_FIND: find_show(b, true); break;
    case M_ZIN: zoom_step(b, 1); break;
    case M_ZOUT: zoom_step(b, -1); break;
    case M_ZRESET: zoom_step(b, 0); break;
    case M_SOURCE: view_source(b); break;
    case M_SETTINGS: open_settings(b); break;
    }
}

static int bm_menu_index;
static void bm_menu_cb(int item, void *ctx) {
    browser_t *b = ctx;
    int i = bm_menu_index;
    if (i < 0 || i >= bdata_nbm) return;
    char url[B_URL_MAX];
    strlcpy(url, bdata_bm[i].url, sizeof url);
    if (item == 0 && cur(b)) tab_go(cur(b), url, NAV_NEW);
    else if (item == 1) browser_new_tab(b, url, true);
    else if (item == 3) { bdata_bookmark_remove(i); win_invalidate(b->win); }
}

static void lock_info(browser_t *b) {
    tab_t *t = cur(b);
    if (!t) return;
    char msg[700];
    if (t->https && (!t->internal || !t->cert_ok)) {
        if (t->cert_ok)
            snprintf(msg, sizeof msg, "This site uses a secure connection (HTTPS).\n\nCertificate issued to: %s\nIssued by: %s\n%s\n\n" NOTE_TEXT,
                     t->cert_subject[0] ? t->cert_subject : "?", t->cert_issuer[0] ? t->cert_issuer : "?", t->tls_info);
        else
            snprintf(msg, sizeof msg, "The certificate of this site is not valid: %s\n\nYou chose to continue anyway, so anything you "
                     "send to this site could be read or changed by others.", t->cert_error[0] ? t->cert_error : "unknown problem");
        ui_message_box(b->win, t->cert_ok ? "Connection is secure" : "Connection is not secure", msg,
                       MB_OK | (t->cert_ok ? MB_ICON_INFO : MB_ICON_WARNING), NULL, NULL);
    } else if (!strncasecmp(t->url, "http://", 7)) {
        ui_message_box(b->win, "Connection is not secure",
                       "This site does not use a secure connection (HTTPS). Information you send to it could be read by others.",
                       MB_OK | MB_ICON_WARNING, NULL, NULL);
    }
}

/* ---- actions -------------------------------------------------------------------------------- */
static void go_back(browser_t *b) {
    tab_t *t = cur(b);
    if (!t || t->hpos <= 0) return;
    t->hist[t->hpos].sx = t->sx;
    t->hist[t->hpos].sy = t->sy;
    t->hpos--;
    tab_go(t, t->hist[t->hpos].url, NAV_HIST);
    browser_set_address(b, t->hist[t->hpos].url);
    win_invalidate(b->win);
}

static void go_forward(browser_t *b) {
    tab_t *t = cur(b);
    if (!t || t->hpos + 1 >= t->nhist) return;
    t->hist[t->hpos].sx = t->sx;
    t->hist[t->hpos].sy = t->sy;
    t->hpos++;
    tab_go(t, t->hist[t->hpos].url, NAV_HIST);
    browser_set_address(b, t->hist[t->hpos].url);
    win_invalidate(b->win);
}

static void reload_or_stop(browser_t *b) {
    tab_t *t = cur(b);
    if (!t) return;
    if (t->loading) { tab_stop(t); win_invalidate(b->win); return; }
    if (t->nhist) tab_go(t, t->hist[t->hpos].url, NAV_RELOAD);
}

static void activate(browser_t *b, int hs, int button) {
    if (hs >= HS_BM && hs < HS_BM + 100) {
        int i = hs - HS_BM;
        if (i >= bdata_nbm) return;
        if (button == MOUSE_MIDDLE || (input_mods() & MOD_CTRL)) browser_new_tab(b, bdata_bm[i].url, false);
        else if (cur(b)) { tab_go(cur(b), bdata_bm[i].url, NAV_NEW); ui_focus(b->content); }
        return;
    }
    if (hs >= HS_TABX && hs < HS_TABX + B_MAX_TABS) { close_tab(b, hs - HS_TABX); return; }
    if (hs >= HS_TAB && hs < HS_TAB + B_MAX_TABS) {
        if (button == MOUSE_MIDDLE) close_tab(b, hs - HS_TAB);
        else switch_tab(b, hs - HS_TAB);
        return;
    }
    switch (hs) {
    case HS_BACK: go_back(b); break;
    case HS_FWD: go_forward(b); break;
    case HS_RELOAD: reload_or_stop(b); break;
    case HS_HOME: if (cur(b)) { tab_go(cur(b), B_HOME_URL, NAV_NEW); } break;
    case HS_NEWTAB: browser_new_tab(b, NULL, true); break;
    case HS_STAR: toggle_bookmark(b); break;
    case HS_LOCK: lock_info(b); break;
    case HS_ZOOM: zoom_step(b, 0); break;
    case HS_MENU: {
        rect_t r = btn_rect(b, HS_MENU);
        ui_popup_menu(b->win, r.x + r.w - 200, r.y + r.h,
                      "New tab\tCtrl+T|History\tCtrl+H|Downloads|-|Find in page\tCtrl+F|Zoom in\tCtrl++|Zoom out\tCtrl+-|"
                      "Actual size\tCtrl+0|View page source\tCtrl+U|-|Settings", menu_cb, b);
        break;
    }
    case HS_FPREV: find_next(b, -1); break;
    case HS_FNEXT: find_next(b, 1); break;
    case HS_FCLOSE: find_show(b, false); break;
    }
}

/* ---- page view input ---------------------------------------------------------------------- */
static bool content_key(browser_t *b, tab_t *t, const event_t *ev) {
    if (t->focus_ctrl >= 0 && t->focus_ctrl < t->pg.nctrls && !(ev->mods & MOD_ALT)) {
        if (ctrl_key(b, t, ev)) return true;
        if (ev->type == EV_CHAR) return false;
        if (is_text(&t->pg.ctrls[t->focus_ctrl]) && !(ev->mods & MOD_CTRL) &&
            (ev->key == KEY_UP || ev->key == KEY_DOWN || ev->key == KEY_SPACE)) return true;
    }
    if (ev->type != EV_KEY_DOWN || (ev->mods & (MOD_CTRL | MOD_ALT))) return false;
    rect_t v = browser_view(b);
    switch (ev->key) {
    case KEY_UP: scroll_to(b, t->sx, t->sy - 40); return true;
    case KEY_DOWN: scroll_to(b, t->sx, t->sy + 40); return true;
    case KEY_LEFT: scroll_to(b, t->sx - 40, t->sy); return true;
    case KEY_RIGHT: scroll_to(b, t->sx + 40, t->sy); return true;
    case KEY_PAGEUP: scroll_to(b, t->sx, t->sy - (v.h - 40)); return true;
    case KEY_PAGEDOWN: scroll_to(b, t->sx, t->sy + (v.h - 40)); return true;
    case KEY_SPACE: scroll_to(b, t->sx, t->sy + ((ev->mods & MOD_SHIFT) ? -(v.h - 40) : (v.h - 40))); return true;
    case KEY_HOME: scroll_to(b, 0, 0); return true;
    case KEY_END: scroll_to(b, t->sx, t->pg.lay.height); return true;
    case KEY_BACKSPACE: go_back(b); return true;
    default: return false;
    }
}

static bool content_event(widget_t *w, const event_t *ev) {
    browser_t *b = ui_get_ctx(w);
    bview_t *bv = V(b);
    tab_t *t = cur(b);
    if (!t) return false;
    rect_t v = browser_view(b);
    switch (ev->type) {
    case EV_MOUSE_WHEEL:
        if (ev->mods & MOD_CTRL) { zoom_step(b, ev->wheel < 0 ? 1 : -1); return true; }
        if (ev->mods & MOD_SHIFT) scroll_to(b, t->sx + ev->wheel * 60, t->sy);
        else scroll_to(b, t->sx, t->sy + ev->wheel * 60);
        return true;
    case EV_MOUSE_DOWN: {
        rect_t vb = vbar_rect(b), hb = hbar_rect(b);
        int total = t->styled ? t->pg.lay.height : 0;
        if (rect_contains(vb, ev->x, ev->y)) {
            int tp, tl;
            thumb(vb.h, total, v.h, t->sy, &tp, &tl);
            int y = ev->y - vb.y;
            if (total > v.h && y >= tp && y < tp + tl) { b->drag = 1; b->drag_off = y - tp; }
            else if (total > v.h) scroll_to(b, t->sx, t->sy + (y < tp ? -(v.h - 40) : (v.h - 40)));
            page_dirty(b);
            return true;
        }
        if (has_hbar(b) && rect_contains(hb, ev->x, ev->y)) {
            int tp, tl;
            thumb(hb.w, t->pg.lay.width, v.w, t->sx, &tp, &tl);
            int x = ev->x - hb.x;
            if (x >= tp && x < tp + tl) { b->drag = 2; b->drag_off = x - tp; }
            else scroll_to(b, t->sx + (x < tp ? -(v.w - 40) : (v.w - 40)), t->sy);
            page_dirty(b);
            return true;
        }
        if (!rect_contains(v, ev->x, ev->y) || !t->styled) return true;
        int dx = ev->x - v.x + t->sx, dy = ev->y - v.y + t->sy;
        bv->press_link = -1;
        if (ev->button == MOUSE_LEFT) {
            int c = web_hit_ctrl(&t->pg, dx, dy);
            if (c >= 0) { ctrl_click(b, t, c, dx, dy); return true; }
            if (t->focus_ctrl >= 0) { t->focus_ctrl = -1; page_dirty(b); }
            bv->press_link = web_hit_link(&t->pg, dx, dy);
        } else if (ev->button == MOUSE_MIDDLE) {
            int l = web_hit_link(&t->pg, dx, dy);
            if (l >= 0) tab_follow_link(t, l, true);
        }
        return true;
    }
    case EV_MOUSE_UP:
        if (b->drag) { b->drag = 0; page_dirty(b); return true; }
        if (ev->button == MOUSE_LEFT && bv->press_link >= 0 && t->styled) {
            int dx = ev->x - v.x + t->sx, dy = ev->y - v.y + t->sy;
            int l = web_hit_link(&t->pg, dx, dy);
            int pl = bv->press_link;
            bv->press_link = -1;
            if (l == pl) {
                t->hover_link = -1;
                tab_follow_link(t, l, (ev->mods & MOD_CTRL) != 0);
                win_invalidate(b->win);
            }
        }
        return true;
    case EV_MOUSE_MOVE: {
        if (b->drag == 1) {
            rect_t vb = vbar_rect(b);
            int total = t->pg.lay.height, tp, tl;
            thumb(vb.h, total, v.h, t->sy, &tp, &tl);
            int range = vb.h - tl;
            int pos = ev->y - vb.y - b->drag_off;
            if (range > 0) scroll_to(b, t->sx, (int)((int64_t)pos * (total - v.h) / range));
            return true;
        }
        if (b->drag == 2) {
            rect_t hb = hbar_rect(b);
            int total = t->pg.lay.width, tp, tl;
            thumb(hb.w, total, v.w, t->sx, &tp, &tl);
            int range = hb.w - tl;
            int pos = ev->x - hb.x - b->drag_off;
            if (range > 0) scroll_to(b, (int)((int64_t)pos * (total - v.w) / range), t->sy);
            return true;
        }
        if (b->hover != HS_NONE) {   /* the pointer left the toolbar for the page */
            b->hover = HS_NONE;
            win_invalidate_rect(b->win, 0, 0, win_width(b->win), top_h());
        }
        int l = -1, cursor = CURSOR_ARROW;
        if (rect_contains(v, ev->x, ev->y) && t->styled) {
            int dx = ev->x - v.x + t->sx, dy = ev->y - v.y + t->sy;
            int c = web_hit_ctrl(&t->pg, dx, dy);
            if (c >= 0) cursor = is_text(&t->pg.ctrls[c]) ? CURSOR_IBEAM : CURSOR_ARROW;
            else {
                l = web_hit_link(&t->pg, dx, dy);
                if (l >= 0) cursor = CURSOR_HAND;
            }
        }
        win_set_cursor(b->win, cursor);
        if (l != t->hover_link) {
            t->hover_link = l;
            page_dirty(b);
            win_invalidate_rect(b->win, 0, win_height(b->win) - B_STATUS_H, win_width(b->win), B_STATUS_H);
        }
        return true;
    }
    case EV_MOUSE_LEAVE:
        if (t->hover_link >= 0) {
            t->hover_link = -1;
            page_dirty(b);
            win_invalidate_rect(b->win, 0, win_height(b->win) - B_STATUS_H, win_width(b->win), B_STATUS_H);
        }
        return false;
    case EV_KEY_DOWN: case EV_CHAR:
        return content_key(b, t, ev);
    default:
        return false;
    }
}

/* ---- window --------------------------------------------------------------------------------- */
static void layout_widgets(browser_t *b) {
    rect_t a = addr_rect(b);
    ui_move(b->addr, a.x, a.y, a.w, a.h);
    rect_t c = content_rect(b);
    ui_move(b->content, c.x, c.y, c.w, c.h);
    ui_move(b->find_box, 52, find_y(b) + 5, 246, 26);
}

static void addr_submit(widget_t *w, void *ctx) {
    browser_t *b = ctx;
    tab_t *t = cur(b);
    char url[B_URL_MAX];
    if (!t || !browser_input_to_url(ui_get_text(w), url, sizeof url)) return;
    tab_go(t, url, NAV_NEW);
    ui_focus(b->content);
    win_invalidate(b->win);
}

static bool window_keys(browser_t *b, const event_t *ev) {
    int k = ev->key;
    bool ctrl = ev->mods & MOD_CTRL, alt = ev->mods & MOD_ALT, shift = ev->mods & MOD_SHIFT;
    tab_t *t = cur(b);
    if (ctrl && !alt) {
        switch (k) {
        case 't': browser_new_tab(b, NULL, true); return true;
        case 'w': close_tab(b, b->cur); return true;
        case 'l': ui_focus(b->addr); ui_textbox_select_all(b->addr); return true;
        case 'r': reload_or_stop(b); return true;
        case 'f': find_show(b, true); return true;
        case 'h': browser_new_tab(b, "about:history", true); return true;
        case 'd': toggle_bookmark(b); return true;
        case 'u': view_source(b); return true;
        case '=': case '+': zoom_step(b, 1); return true;
        case '-': zoom_step(b, -1); return true;
        case '0': zoom_step(b, 0); return true;
        case KEY_TAB: if (b->ntabs > 1) switch_tab(b, (b->cur + (shift ? b->ntabs - 1 : 1)) % b->ntabs); return true;
        case KEY_PAGEDOWN: if (b->ntabs > 1) switch_tab(b, (b->cur + 1) % b->ntabs); return true;
        case KEY_PAGEUP: if (b->ntabs > 1) switch_tab(b, (b->cur + b->ntabs - 1) % b->ntabs); return true;
        default: break;
        }
        if (k >= '1' && k <= '9') { switch_tab(b, k == '9' ? b->ntabs - 1 : MIN(k - '1', b->ntabs - 1)); return true; }
        return false;
    }
    if (alt && !ctrl) {
        if (k == KEY_LEFT) { go_back(b); return true; }
        if (k == KEY_RIGHT) { go_forward(b); return true; }
        if (k == KEY_HOME) { if (t) tab_go(t, B_HOME_URL, NAV_NEW); return true; }
        if (k == 'd') { ui_focus(b->addr); ui_textbox_select_all(b->addr); return true; }
        return false;
    }
    switch (k) {
    case KEY_F5: reload_or_stop(b); return true;
    case KEY_F6: ui_focus(b->addr); ui_textbox_select_all(b->addr); return true;
    case KEY_F3: if (b->find_open) find_next(b, shift ? -1 : 1); else find_show(b, true); return true;
    case KEY_ESCAPE:
        if (b->find_open) { find_show(b, false); return true; }
        if (t && t->loading) { tab_stop(t); win_invalidate(b->win); return true; }
        if (ui_has_focus(b->addr)) { browser_set_address(b, NULL); ui_focus(b->content); return true; }
        return false;
    default: return false;
    }
}

static void browser_event(window_t *win, const event_t *ev) {
    browser_t *b = win_get_userdata(win);
    if (!b) return;
    bview_t *bv = V(b);
    switch (ev->type) {
    case EV_PAINT:
        paint(b);
        break;
    case EV_RESIZE:
        layout_widgets(b);
        bv->pc_valid = false;
        bv->resize_at = sys_uptime_ms() + 150;
        break;
    case EV_TIMER: {
        uint32_t now = sys_uptime_ms();
        bool loading = false;
        for (int i = 0; i < b->ntabs && i < B_MAX_TABS; i++) {
            tab_t *t = b->tabs[i];
            tab_tick(t);
            if (!win_is_valid(win)) return;
            if (t->loading) loading = true;
        }
        if (loading) { b->spin++; win_invalidate_rect(win, 0, 0, win_width(win), B_TAB_H); }
        if (bv->resize_at && (int32_t)(now - bv->resize_at) >= 0) {
            bv->resize_at = 0;
            tab_t *t = cur(b);
            if (t && t->styled) tab_restyle(t);
        }
        tab_t *t = cur(b);
        if (t && t->focus_ctrl >= 0 && t->focus_ctrl < t->pg.nctrls && is_text(&t->pg.ctrls[t->focus_ctrl]) &&
            ui_has_focus(b->content) && now - b->caret_t >= 530) {
            b->caret_t = now;
            b->caret_on = !b->caret_on;
            web_ctrl_t *c = &t->pg.ctrls[t->focus_ctrl];
            rect_t v = browser_view(b);
            bv->pc_valid = false;
            win_invalidate_rect(win, v.x + c->x - t->sx, v.y + c->y - t->sy, c->w, c->h);
        }
        if (bv->chrome_dirty || browser_downloads_active() || (b->status[0] && (int32_t)(now - bv->status_until) >= 0)) {
            if (b->status[0] && (int32_t)(now - bv->status_until) >= 0) b->status[0] = 0;
            bv->chrome_dirty = false;
            win_invalidate_rect(win, 0, 0, win_width(win), B_TAB_H + B_TOOL_H);
            win_invalidate_rect(win, 0, win_height(win) - B_STATUS_H, win_width(win), B_STATUS_H);
        }
        break;
    }
    case EV_MOUSE_MOVE: {
        int h = hit(b, ev->x, ev->y);
        if (h != b->hover) { b->hover = h; win_invalidate_rect(win, 0, 0, win_width(win), top_h()); if (b->find_open) win_invalidate_rect(win, 0, find_y(b), win_width(win), B_FIND_H); }
        win_set_cursor(win, CURSOR_ARROW);
        break;
    }
    case EV_MOUSE_LEAVE:
        if (b->hover) { b->hover = HS_NONE; win_invalidate(win); }
        break;
    case EV_MOUSE_DOWN: {
        int h = hit(b, ev->x, ev->y);
        if (ev->button == MOUSE_RIGHT && h >= HS_BM && h < HS_BM + 100) {
            bm_menu_index = h - HS_BM;
            ui_popup_menu(win, ev->x, ev->y, "Open|Open in new tab|-|Remove", bm_menu_cb, b);
            break;
        }
        b->press = h;
        win_invalidate_rect(win, 0, 0, win_width(win), top_h());
        break;
    }
    case EV_MOUSE_UP: {
        int h = hit(b, ev->x, ev->y);
        int p = b->press;
        b->press = HS_NONE;
        if (h == p && h != HS_NONE) activate(b, h, ev->button);
        if (win_is_valid(win)) win_invalidate_rect(win, 0, 0, win_width(win), top_h());
        break;
    }
    case EV_DOUBLE_CLICK:
        if (ev->y < B_TAB_H && hit(b, ev->x, ev->y) == HS_NONE) browser_new_tab(b, NULL, true);
        break;
    case EV_KEY_DOWN:
        window_keys(b, ev);
        break;
    case EV_THEME:
        bv->pc_valid = false;
        break;
    case EV_CLOSE:
        for (int i = 0; i < b->ntabs; i++) tab_free(b->tabs[i]);
        b->ntabs = 0;
        if (bv->pc) surface_destroy(bv->pc);
        free(b);
        win_set_userdata(win, NULL);
        break;
    }
}

static void browser_launch(const char *arg) {
    bdata_load();
    browser_t *b = calloc(1, sizeof(browser_t) + sizeof(bview_t));
    if (!b) return;
    V(b)->press_link = -1;
    window_t *win = win_create("Web Browser", 980, 640, WIN_RESIZABLE | WIN_CENTER);
    if (!win) { free(b); return; }
    b->win = win;
    win_set_userdata(win, b);
    win_set_event_handler(win, browser_event);
    win_set_min_size(win, 560, 360);
    b->content = ui_custom(win, 0, 0, 10, 10, content_draw, content_event, b);
    b->addr = ui_textbox(win, 0, 0, 10, 10, "");
    ui_textbox_set_placeholder(b->addr, "Type a site or search");
    ui_textbox_on_submit(b->addr, addr_submit, b);
    b->find_box = ui_textbox(win, 0, 0, 10, 10, "");
    ui_set_callback(b->find_box, find_changed, b);
    ui_textbox_on_submit(b->find_box, find_submit, b);
    ui_set_visible(b->find_box, false);
    layout_widgets(b);
    win_set_timer(win, 40);
    char url[B_URL_MAX];
    const char *start = NULL;
    if (arg && *arg) {
        if (arg[0] == '/') { snprintf(url, sizeof url, "file://%s", arg); start = url; }
        else start = browser_input_to_url(arg, url, sizeof url);
    }
    browser_new_tab(b, start, true);
}

/* ---- icon: a blue globe with an orbit ---------------------------------------------------- */
static void aa_stroke(surface_t *s, double x0, double y0, double x1, double y1, double th, uint32_t c) {
    double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
    if (len < 0.01) return;
    double nx = -dy / len, ny = dx / len;
    int n = (int)ceil(th * 1.5);
    if (n < 1) n = 1;
    for (int i = 0; i < n; i++) {
        double o = -th / 2 + th * (i + 0.5) / n;
        gfx_line_aa(s, x0 + nx * o, y0 + ny * o, x1 + nx * o, y1 + ny * o, c);
    }
}

/* the orbit: an ellipse tilted by `tilt`; front = the half nearer the viewer */
static void orbit(surface_t *s, double cx, double cy, double rx, double ry, double tilt, double th, uint32_t c, bool front) {
    int steps = 64;
    for (int i = 0; i < steps; i++) {
        double a0 = i * 2 * M_PI / steps, a1 = a0 + 2 * M_PI / steps;
        bool f = sin((a0 + a1) / 2) > 0;
        if (f != front) continue;
        double x0 = rx * cos(a0), y0 = ry * sin(a0), x1 = rx * cos(a1), y1 = ry * sin(a1);
        aa_stroke(s, cx + x0 * cos(tilt) - y0 * sin(tilt), cy + x0 * sin(tilt) + y0 * cos(tilt),
                  cx + x1 * cos(tilt) - y1 * sin(tilt), cy + x1 * sin(tilt) + y1 * cos(tilt), th, c);
    }
}

static void browser_icon(surface_t *s, int x, int y, int size) {
    double S = size, cx = x + S / 2, cy = y + S / 2, r = S * 0.38;
    double rx = S * 0.48, ry = S * 0.15, tilt = -0.42, oth = MAX(1.2, S / 20.0);
    /* the far half of the orbit, behind the globe */
    orbit(s, cx, cy, rx, ry, tilt, oth * 0.8, RGBA(230, 160, 40, 200), false);
    gfx_fill_circle_aa(s, cx, cy + S * 0.025, r, RGBA(0, 0, 0, 55));
    gfx_fill_circle_aa(s, cx, cy, r, RGB(26, 100, 214));
    gfx_fill_circle_aa(s, cx - r * 0.12, cy - r * 0.14, r * 0.86, RGB(44, 130, 240));
    gfx_fill_circle_aa(s, cx - r * 0.30, cy - r * 0.34, r * 0.46, RGBA(150, 205, 255, 90));
    uint32_t ln = RGBA(225, 242, 255, 220);
    double th = MAX(1.0, S / 34.0);
    /* meridians and parallels */
    for (int i = 0; i < 32; i++) {
        double a0 = -M_PI / 2 + i * M_PI / 32, a1 = a0 + M_PI / 32;
        for (int sgn = -1; sgn <= 1; sgn += 2)
            aa_stroke(s, cx + sgn * r * 0.45 * cos(a0), cy + r * sin(a0), cx + sgn * r * 0.45 * cos(a1), cy + r * sin(a1), th, ln);
    }
    aa_stroke(s, cx, cy - r, cx, cy + r, th, ln);
    aa_stroke(s, cx - r, cy, cx + r, cy, th, ln);
    double py = r * 0.52, hw = sqrt(r * r - py * py);
    aa_stroke(s, cx - hw, cy - py, cx + hw, cy - py, th, ln);
    aa_stroke(s, cx - hw, cy + py, cx + hw, cy + py, th, ln);
    /* the near half of the orbit, in front, with a small star on it */
    orbit(s, cx, cy, rx, ry, tilt, oth, RGB(255, 196, 60), true);
    double sa = 0.55, sx = rx * cos(sa), sy = ry * sin(sa);
    double px = cx + sx * cos(tilt) - sy * sin(tilt), pyy = cy + sx * sin(tilt) + sy * cos(tilt);
    gfx_fill_circle_aa(s, px, pyy, S * 0.075, RGB(255, 240, 170));
    gfx_fill_circle_aa(s, px, pyy, S * 0.04, RGB(255, 255, 255));
}

REGISTER_APP(.name = "Web Browser", .category = CAT_INTERNET,
             .description = "Browse the web: tabs, bookmarks, history and downloads",
             .launch = browser_launch, .draw_icon = browser_icon,
             .file_types = "html,htm");
