/*
 * htmlviewer.c - "HTML Viewer": opens web pages saved on this computer (.html, .htm).
 *
 * Pages go through the Web Browser's page pipeline (browser_net.c: tab_go, tab_tick,
 * tab_restyle, tab_follow_link, and the web engine in web_*.c) - the one its tabs
 * use - so a page looks the same here as in a browser tab and runs what the pipeline
 * runs, scripts included. The viewer keeps one tab_t of its own in a browser_t that
 * has no tabs: for such a host browser.c never touches the window (title, address
 * bar), browser_view() asks the viewer for its page area (the host_view hook), the
 * page gets the viewer's mouse and keys through tab_page_input() (scripts' event
 * handlers, form controls), and the viewer draws the page itself, repainting when
 * the pipeline says the page changed (paint_gen) or its layout, pictures or scroll
 * position change.
 *
 *   - the file name in the title; links between local pages (other files open in
 *     their own app, folders in Files); stylesheets and pictures load from disk;
 *     http(s) links open in the Web Browser;
 *   - toolbar: Reload, Zoom out, Zoom in, Open in Web Browser; File > Open...;
 *   - pages viewed here are not added to the Web Browser's history.
 */
#include "browser.h"

#define HV_W          880
#define HV_H          600
#define HV_TOOL_H     40
#define HV_STATUS_H   22
#define HV_SB         B_SB                     /* the pipeline's layout leaves room for one */
#define HV_TOP        (UI_MENUBAR_H + HV_TOOL_H)
#define HV_HOST_SLACK 4096                     /* browser.c keeps its per-window state here */

static const int zoom_steps[] = { 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200 };

typedef struct hv {
    window_t *win;
    browser_t *host;            /* the pipeline's browser: this window, no tabs     */
    tab_t *tab;                 /* the page; NULL until a file is opened             */
    widget_t *page, *zoom_lbl, *b_reload, *b_zout, *b_zin, *b_web;
    surface_t *pc;              /* the page drawn at its scroll position             */
    bool pc_valid;
    uint32_t sig;               /* what the window shows of the page (page_sig)      */
    int press_link;             /* link under the pointer when the button went down  */
    int drag, drag_off;         /* scrollbar thumb drag: 1 vertical, 2 horizontal    */
    uint32_t resize_at;         /* lay the page out again once resizing pauses       */
    char msg[160];              /* transient status bar message                      */
    uint32_t msg_until;
} hv_t;

/* A step of the page pipeline. Pages viewed here stay out of the Web Browser's history. */
#define PIPE(stmt) do { bool hist_ = bdata_history_on; bdata_history_on = false; stmt; bdata_history_on = hist_; } while (0)

/* ---- file: URLs ------------------------------------------------------------------------- */
static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* "/home/user/My page.html" -> "file:///home/user/My%20page.html" */
static void path_to_url(const char *path, char *out, size_t size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = strlcpy(out, "file://", size);
    for (const unsigned char *p = (const unsigned char *)path; *p && o + 4 < size; p++) {
        if (*p <= ' ' || *p >= 0x7F || *p == '%' || *p == '#' || *p == '?') {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        } else out[o++] = (char)*p;
    }
    out[o] = 0;
}

/* "file:///home/user/a%20b.html#top" -> "/home/user/a b.html"; false if not a file: URL */
static bool url_to_path(const char *url, char *out, size_t size) {
    if (strncasecmp(url, "file://", 7)) return false;
    const char *p = url + 7;
    if (*p != '/' && !(p = strchr(p, '/'))) return false;
    size_t o = 0;
    for (; *p && *p != '?' && *p != '#' && o + 1 < size; p++) {
        if (*p == '%' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0) { out[o++] = (char)(hexv(p[1]) * 16 + hexv(p[2])); p += 2; }
        else out[o++] = *p;
    }
    out[o] = 0;
    return o > 0;
}

/* ---- geometry ----------------------------------------------------------------------------- */
static bool has_hbar(hv_t *v) {
    tab_t *t = v->tab;
    return t && t->styled && t->pg.lay.width > win_width(v->win) - HV_SB;
}

/* the page area in client coordinates, without the scrollbars */
static rect_t page_view(hv_t *v) {
    int W = win_width(v->win), H = win_height(v->win);
    int h = H - HV_TOP - HV_STATUS_H - (has_hbar(v) ? HV_SB : 0);
    return rect_make(0, HV_TOP, MAX(0, W - HV_SB), MAX(0, h));
}
static rect_t vbar_rect(hv_t *v) { rect_t r = page_view(v); return rect_make(r.x + r.w, r.y, HV_SB, r.h); }
static rect_t hbar_rect(hv_t *v) { rect_t r = page_view(v); return rect_make(r.x, r.y + r.h, r.w, HV_SB); }

/* browser_t.host_view: the page area the pipeline lays pages out for */
static rect_t host_view(browser_t *b) {
    hv_t *v = win_get_userdata(b->win);
    return v ? page_view(v) : rect_make(0, 0, 0, 0);
}

static bool is_text(const web_ctrl_t *c) { return c->type == C_TEXT || c->type == C_PASSWORD || c->type == C_TEXTAREA; }

static void thumb(int len, int total, int page, int pos, int *tp, int *tl) {
    *tl = MAX(24, (int)((int64_t)len * page / MAX(1, total)));
    if (*tl > len) *tl = len;
    int maxpos = total - page;
    *tp = maxpos > 0 ? (int)((int64_t)(len - *tl) * pos / maxpos) : 0;
}

/* keep the scroll position inside the page (it may have become shorter) */
static void clamp_scroll(hv_t *v) {
    tab_t *t = v->tab;
    if (!t) return;
    rect_t r = page_view(v);
    t->sy = CLAMP(t->sy, 0, MAX(0, t->pg.lay.height - r.h));
    t->sx = CLAMP(t->sx, 0, MAX(0, t->pg.lay.width - r.w));
}

/* What is on screen: the page, its layout, its pictures, the scroll position and the
   pipeline's paint_gen (control values, focus, caret - what scripts change without a
   new layout). The pipeline changes them from its own steps (pictures decoded,
   stylesheets, scripts), so the timer compares this every tick and repaints on a change. */
static uint32_t page_sig(hv_t *v) {
    tab_t *t = v->tab;
    uint32_t h = 2166136261u;
#define MIX(x) (h = (h ^ (uint32_t)(x)) * 16777619u)
    if (!t) return h;
    const web_layout_t *L = &t->pg.lay;
    MIX(t->gen); MIX(t->styled); MIX(t->sx); MIX(t->sy); MIX(t->hover_link + 1); MIX(t->zoom);
    MIX(t->paint_gen); MIX(t->focus_ctrl + 1);
    MIX(L->nitems); MIX(L->width); MIX(L->height); MIX(L->bg);
    for (int i = 0; i < L->nitems; i++) {
        const web_item_t *it = &L->items[i];
        MIX(it->kind); MIX(it->x); MIX(it->y); MIX(it->w); MIX(it->h);
        MIX(it->color); MIX(it->len); MIX((uintptr_t)it->text); MIX(it->ref);
    }
    for (int i = 0; i < t->pg.nimgs; i++) { MIX(t->pg.imgs[i].state); MIX((uintptr_t)t->pg.imgs[i].surf); }
#undef MIX
    return h;
}

/* ---- window chrome ------------------------------------------------------------------------ */
static void status_dirty(hv_t *v) {
    win_invalidate_rect(v->win, 0, win_height(v->win) - HV_STATUS_H, win_width(v->win), HV_STATUS_H);
}

static void set_msg(hv_t *v, const char *m) {
    strlcpy(v->msg, m, sizeof v->msg);
    v->msg_until = sys_uptime_ms() + 4000;
    status_dirty(v);
}

/* the title (the file shown), the zoom and which buttons work */
static void sync_chrome(hv_t *v) {
    tab_t *t = v->tab;
    char title[VFS_NAME_MAX + 32], path[VFS_PATH_MAX];
    if (!t) strlcpy(title, "HTML Viewer", sizeof title);
    else if (url_to_path(t->url, path, sizeof path)) snprintf(title, sizeof title, "%s - HTML Viewer", vfs_basename(path));
    else snprintf(title, sizeof title, "%s - HTML Viewer", t->title[0] ? t->title : "Page");
    if (strcmp(win_get_title(v->win), title)) win_set_title(v->win, title);
    char z[16];
    snprintf(z, sizeof z, "%d%%", t ? t->zoom : 100);
    if (strcmp(ui_get_text(v->zoom_lbl), z)) ui_set_text(v->zoom_lbl, z);
    ui_set_enabled(v->b_reload, t != NULL);
    ui_set_enabled(v->b_web, t != NULL);
    ui_set_enabled(v->b_zout, t && t->zoom > zoom_steps[0]);
    ui_set_enabled(v->b_zin, t && t->zoom < zoom_steps[ARRAY_SIZE(zoom_steps) - 1]);
}

static void page_dirty(hv_t *v) {
    v->pc_valid = false;
    ui_invalidate(v->page);
}

/* The page's input (scripts' event handlers, form controls): x, y = the pointer in
   document coordinates. true = the page used the event. */
static bool page_input(hv_t *v, const event_t *ev, int x, int y) {
    bool used;
    PIPE(used = tab_page_input(v->tab, ev, x, y));
    if (used) page_dirty(v);
    return used;
}

/* after the pipeline changed the page: clamp, retitle, repaint */
static void page_changed(hv_t *v) {
    clamp_scroll(v);
    v->sig = page_sig(v);
    sync_chrome(v);
    page_dirty(v);
    status_dirty(v);
}

static void scroll_to(hv_t *v, int x, int y) {
    tab_t *t = v->tab;
    if (!t) return;
    int ox = t->sx, oy = t->sy;
    t->sx = x;
    t->sy = y;
    clamp_scroll(v);
    if (t->sx != ox || t->sy != oy) page_dirty(v);
}

/* ---- loading ----------------------------------------------------------------------------- */
static void go(hv_t *v, const char *url, int mode) {
    if (!v->tab) {
        tab_t *t = calloc(1, sizeof *t);
        if (!t) return;
        t->b = v->host;
        t->zoom = 100;
        t->focus_ctrl = t->hover_link = t->hl_cur = -1;
        v->tab = t;
    }
    PIPE(tab_go(v->tab, url, mode));
    page_changed(v);
}

/* open a file (a path or a file: URL) */
static void open_file(hv_t *v, const char *arg) {
    char url[B_URL_MAX], path[VFS_PATH_MAX];
    if (!strncasecmp(arg, "file:", 5)) strlcpy(url, arg, sizeof url);
    else {
        if (vfs_normalize(NULL, arg, path) != 0) strlcpy(path, arg, sizeof path);
        path_to_url(path, url, sizeof url);
    }
    go(v, url, NAV_NEW);
    ui_focus(v->page);
}

static void reload(hv_t *v) {
    if (!v->tab || !v->tab->url[0]) return;
    char url[B_URL_MAX];
    strlcpy(url, v->tab->url, sizeof url);
    go(v, url, NAV_RELOAD);
}

static void open_in_browser(hv_t *v) {
    if (v->tab && v->tab->url[0]) app_launch_by_name("Web Browser", v->tab->url);
}

static void open_cb(const char *path, void *ctx) {
    if (path) open_file(ctx, path);
}

static void open_dialog(hv_t *v) {
    char start[VFS_PATH_MAX], path[VFS_PATH_MAX];
    if (v->tab && url_to_path(v->tab->url, path, sizeof path)) vfs_dirname(path, start);
    else strlcpy(start, "/home/user", sizeof start);
    ui_file_dialog(v->win, UI_FILE_OPEN, "Open", start, "html,htm", open_cb, v);
}

static void zoom_step(hv_t *v, int dir) {
    tab_t *t = v->tab;
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
    PIPE(tab_restyle(t));
    t->sy = (int)(rel * t->pg.lay.height / 10000);
    page_changed(v);
}

/* files the pipeline shows like a browser does; any other file opens in its own app */
static bool shown_here(const char *path) {
    static const char *const ext[] = { "html", "htm", "xhtml", "txt", "png", "jpg", "jpeg", "gif", "bmp", "svg" };
    const char *e = vfs_extension(path);
    for (int i = 0; i < ARRAY_SIZE(ext); i++) if (!strcasecmp(e, ext[i])) return true;
    return false;
}

static void follow_link(hv_t *v, int link, bool new_window) {
    tab_t *t = v->tab;
    if (!t || link < 0 || link >= t->pg.nlinks) return;
    const char *href = t->pg.links[link].href, *target = t->pg.links[link].target;
    while (*href == ' ' || *href == '\t' || *href == '\n') href++;
    char abs[B_URL_MAX], path[VFS_PATH_MAX];
    if (!url_resolve(t->pg.base, href, abs, sizeof abs)) return;
    static const char *const web[] = { "http://", "https://", "about:", "browser:", "view-source:" };
    for (int i = 0; i < ARRAY_SIZE(web); i++)
        if (!strncasecmp(abs, web[i], strlen(web[i]))) {
            app_launch_by_name("Web Browser", abs);
            set_msg(v, "Opened in the Web Browser");
            return;
        }
    if (url_to_path(abs, path, sizeof path)) {
        if (vfs_is_dir(path) || !shown_here(path)) { shell_open(path); return; }
        if (new_window || (target && !strcasecmp(target, "_blank"))) { app_launch_by_name("HTML Viewer", abs); return; }
    }
    PIPE(tab_follow_link(t, link, false));      /* local pages, #anchors, mailto:, data: ... */
    page_changed(v);
}

/* The pipeline opened a tab of its own (target=_blank on other links, a script's
   window.open): open that address in a window of its own instead. */
static void adopt_tabs(hv_t *v) {
    browser_t *b = v->host;
    while (b->ntabs > 0) {
        tab_t *t = b->tabs[--b->ntabs];
        b->tabs[b->ntabs] = NULL;
        char url[B_URL_MAX];
        strlcpy(url, t->req_url[0] ? t->req_url : t->url, sizeof url);
        tab_free(t);
        if (url[0] && strncasecmp(url, "about:", 6))
            app_launch_by_name(!strncasecmp(url, "file:", 5) ? "HTML Viewer" : "Web Browser", url);
    }
    b->cur = 0;
    sync_chrome(v);
}

/* ---- the page view (a custom widget) ------------------------------------------------------- */
static void page_draw(widget_t *w, surface_t *s, rect_t clip) {
    hv_t *v = ui_get_ctx(w);
    const theme_t *th = theme_get();
    tab_t *t = v->tab;
    (void)clip;
    if (!t) {                                 /* nothing open yet */
        rect_t a = ui_rect(w);
        int cy = a.y + a.h / 2;
        icon_draw(ICON_FILE_HTML, s, a.x + a.w / 2 - 24, cy - 70, 48);
        gfx_text_aligned(s, rect_make(a.x, cy - 14, a.w, 26), "No page open", th->window_text, FONT_UI_LARGE,
                         ALIGN_CENTER | ALIGN_MIDDLE);
        gfx_text_aligned(s, rect_make(a.x, cy + 14, a.w, 20), "Open an .html file with File > Open...",
                         color_mix(th->window_text, th->window_bg, 80), FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
        return;
    }
    rect_t r = page_view(v);
    if (r.w <= 0 || r.h <= 0) return;
    if (!v->pc || v->pc->w != r.w || v->pc->h != r.h) {
        if (v->pc) surface_destroy(v->pc);
        v->pc = surface_create(r.w, r.h);
        v->pc_valid = false;
        if (!v->pc) return;
    }
    if (!v->pc_valid) {
        surface_reset_clip(v->pc);
        if (t->styled) {
            web_paint_opts_t o;
            memset(&o, 0, sizeof o);
            o.hover_link = t->hover_link;
            o.focus_ctrl = t->focus_ctrl;
            o.caret_on = v->host->caret_on;
            o.hl_current = -1;
            web_paint(&t->pg, v->pc, rect_make(0, 0, r.w, r.h), t->sx, t->sy, &o);
        } else {
            gfx_fill_rect(v->pc, 0, 0, r.w, r.h, RGB(255, 255, 255));
        }
        v->pc_valid = true;
    }
    gfx_blit(s, r.x, r.y, v->pc);
    ui_draw_scrollbar(s, vbar_rect(v), true, t->styled ? t->pg.lay.height : 0, r.h, t->sy,
                      v->drag == 1 ? UI_STATE_PRESSED : 0);
    if (has_hbar(v)) {
        ui_draw_scrollbar(s, hbar_rect(v), false, t->pg.lay.width, r.w, t->sx, v->drag == 2 ? UI_STATE_PRESSED : 0);
        gfx_fill_rect(s, r.x + r.w, r.y + r.h, HV_SB, HV_SB, th->scrollbar_bg);
    }
}

static void set_hover(hv_t *v, int link) {
    tab_t *t = v->tab;
    if (!t || link == t->hover_link) return;
    t->hover_link = link;
    page_dirty(v);
    status_dirty(v);
}

static bool page_keys(hv_t *v, const event_t *ev) {
    tab_t *t = v->tab;
    if (ev->type != EV_KEY_DOWN || (ev->mods & (MOD_CTRL | MOD_ALT))) return false;
    int page = page_view(v).h - 40;
    switch (ev->key) {
    case KEY_UP:       scroll_to(v, t->sx, t->sy - 40); return true;
    case KEY_DOWN:     scroll_to(v, t->sx, t->sy + 40); return true;
    case KEY_LEFT:     scroll_to(v, t->sx - 40, t->sy); return true;
    case KEY_RIGHT:    scroll_to(v, t->sx + 40, t->sy); return true;
    case KEY_PAGEUP:   scroll_to(v, t->sx, t->sy - page); return true;
    case KEY_PAGEDOWN: scroll_to(v, t->sx, t->sy + page); return true;
    case KEY_SPACE:    scroll_to(v, t->sx, t->sy + ((ev->mods & MOD_SHIFT) ? -page : page)); return true;
    case KEY_HOME:     scroll_to(v, 0, 0); return true;
    case KEY_END:      scroll_to(v, t->sx, t->pg.lay.height); return true;
    default:           return false;
    }
}

static bool page_event(widget_t *w, const event_t *ev) {
    hv_t *v = ui_get_ctx(w);
    tab_t *t = v->tab;
    if (!t) return false;
    rect_t r = page_view(v);
    bool in = rect_contains(r, ev->x, ev->y) && t->styled;
    int dx = ev->x - r.x + t->sx, dy = ev->y - r.y + t->sy;     /* document coordinates */
    switch (ev->type) {
    case EV_MOUSE_WHEEL:
        if (ev->mods & MOD_CTRL) { zoom_step(v, ev->wheel < 0 ? 1 : -1); return true; }
        if (in && page_input(v, ev, dx, dy)) return true;         /* the page scrolled something itself */
        if (ev->mods & MOD_SHIFT) scroll_to(v, t->sx + ev->wheel * 60, t->sy);
        else scroll_to(v, t->sx, t->sy + ev->wheel * 60);
        return true;
    case EV_MOUSE_DOWN: {
        ui_focus(w);
        rect_t vb = vbar_rect(v), hb = hbar_rect(v);
        int total = t->styled ? t->pg.lay.height : 0, tp, tl;
        if (rect_contains(vb, ev->x, ev->y)) {
            thumb(vb.h, total, r.h, t->sy, &tp, &tl);
            int y = ev->y - vb.y;
            if (total > r.h && y >= tp && y < tp + tl) { v->drag = 1; v->drag_off = y - tp; }
            else if (total > r.h) scroll_to(v, t->sx, t->sy + (y < tp ? -(r.h - 40) : (r.h - 40)));
            page_dirty(v);
            return true;
        }
        if (has_hbar(v) && rect_contains(hb, ev->x, ev->y)) {
            thumb(hb.w, t->pg.lay.width, r.w, t->sx, &tp, &tl);
            int x = ev->x - hb.x;
            if (x >= tp && x < tp + tl) { v->drag = 2; v->drag_off = x - tp; }
            else scroll_to(v, t->sx + (x < tp ? -(r.w - 40) : (r.w - 40)), t->sy);
            page_dirty(v);
            return true;
        }
        v->press_link = -1;
        if (!in) return true;
        if (ev->button == MOUSE_LEFT) {
            if (page_input(v, ev, dx, dy)) return true;           /* a form control */
            v->press_link = web_hit_link(&t->pg, dx, dy);
        } else if (ev->button == MOUSE_MIDDLE) {
            int l = web_hit_link(&t->pg, dx, dy);
            if (l >= 0) follow_link(v, l, true);
        }
        return true;
    }
    case EV_MOUSE_UP:
        if (v->drag) { v->drag = 0; page_dirty(v); return true; }
        if (ev->button == MOUSE_LEFT && in && page_input(v, ev, dx, dy)) { v->press_link = -1; return true; }
        if (ev->button == MOUSE_LEFT && v->press_link >= 0 && t->styled) {
            int l = web_hit_link(&t->pg, dx, dy), pl = v->press_link;
            v->press_link = -1;
            if (l == pl) {
                set_hover(v, -1);
                follow_link(v, l, (ev->mods & MOD_CTRL) != 0);
            }
        }
        return true;
    case EV_MOUSE_MOVE: {
        int tp, tl;
        if (v->drag == 1) {
            rect_t vb = vbar_rect(v);
            int total = t->pg.lay.height;
            thumb(vb.h, total, r.h, t->sy, &tp, &tl);
            if (vb.h - tl > 0) scroll_to(v, t->sx, (int)((int64_t)(ev->y - vb.y - v->drag_off) * (total - r.h) / (vb.h - tl)));
            return true;
        }
        if (v->drag == 2) {
            rect_t hb = hbar_rect(v);
            int total = t->pg.lay.width;
            thumb(hb.w, total, r.w, t->sx, &tp, &tl);
            if (hb.w - tl > 0) scroll_to(v, (int)((int64_t)(ev->x - hb.x - v->drag_off) * (total - r.w) / (hb.w - tl)), t->sy);
            return true;
        }
        int l = -1, cursor = CURSOR_ARROW;
        if (in) {
            page_input(v, ev, dx, dy);                            /* (the page's scripts: mouse moves) */
            int c = web_hit_ctrl(&t->pg, dx, dy);
            if (c >= 0) cursor = is_text(&t->pg.ctrls[c]) ? CURSOR_IBEAM : CURSOR_ARROW;
            else if ((l = web_hit_link(&t->pg, dx, dy)) >= 0) cursor = CURSOR_HAND;
        }
        win_set_cursor(v->win, cursor);
        set_hover(v, l);
        return true;
    }
    case EV_MOUSE_LEAVE:
        page_input(v, ev, 0, 0);
        set_hover(v, -1);
        return false;
    case EV_DOUBLE_CLICK:
        if (in) page_input(v, ev, dx, dy);
        return true;
    case EV_KEY_DOWN: case EV_CHAR:
        if (page_input(v, ev, 0, 0)) return true;                 /* typing into a control, a script's keys */
        return page_keys(v, ev);
    case EV_KEY_UP:
        page_input(v, ev, 0, 0);
        return false;
    default:
        return false;
    }
}

/* ---- window --------------------------------------------------------------------------------- */
static void layout(hv_t *v) {
    int W = win_width(v->win), y = UI_MENUBAR_H + 6;
    ui_move(v->b_reload, 8, y, 76, 28);
    ui_move(v->b_zout, 96, y, 84, 28);
    ui_move(v->zoom_lbl, 180, y, 52, 28);
    ui_move(v->b_zin, 232, y, 76, 28);
    ui_move(v->b_web, MAX(320, W - 8 - 168), y, 168, 28);
    ui_move(v->page, 0, HV_TOP, W, MAX(0, win_height(v->win) - HV_TOP - HV_STATUS_H));
}

static void paint_chrome(hv_t *v) {
    surface_t *s = win_surface(v->win);
    const theme_t *th = theme_get();
    uint32_t line = color_mix(th->window_bg, th->window_border, 90);
    gfx_hline(s, 0, HV_TOP - 1, s->w, line);
    /* status bar: the link under the pointer, a message, or where the page is */
    int y = s->h - HV_STATUS_H;
    gfx_fill_rect(s, 0, y, s->w, HV_STATUS_H, th->dark ? color_shade(th->window_bg, -10) : color_shade(th->window_bg, -6));
    gfx_hline(s, 0, y, s->w, line);
    tab_t *t = v->tab;
    char text[B_URL_MAX], ui[B_URL_MAX];
    uint32_t col = th->window_text;
    if (t && t->hover_link >= 0 && t->hover_link < t->pg.nlinks &&
        url_resolve(t->pg.base, t->pg.links[t->hover_link].href, text, sizeof text)) {
        if (!url_to_path(text, ui, sizeof ui)) web_1252_to_ui(text, ui, sizeof ui);
    } else if (v->msg[0]) {
        strlcpy(ui, v->msg, sizeof ui);
    } else {
        col = color_mix(th->window_text, th->window_bg, 70);
        if (!t) strlcpy(ui, "No page open", sizeof ui);
        else if (t->loading) strlcpy(ui, "Loading...", sizeof ui);
        else if (!url_to_path(t->url, ui, sizeof ui)) strlcpy(ui, t->url, sizeof ui);
    }
    gfx_text_ellipsis(s, 10, y + 1 + (HV_STATUS_H - 1 - font_height(FONT_UI)) / 2, s->w - 20, ui, col, FONT_UI);
}

static void file_menu(int item, void *ctx) {
    hv_t *v = ctx;
    switch (item) {
    case 0: open_dialog(v); break;
    case 2: reload(v); break;
    case 3: open_in_browser(v); break;
    case 5: win_close(v->win); break;   /* frees v */
    default: break;
    }
}

static void on_reload(widget_t *w, void *ctx) { (void)w; reload(ctx); }
static void on_zoom_out(widget_t *w, void *ctx) { (void)w; zoom_step(ctx, -1); }
static void on_zoom_in(widget_t *w, void *ctx) { (void)w; zoom_step(ctx, 1); }
static void on_web(widget_t *w, void *ctx) { (void)w; open_in_browser(ctx); }

static void window_keys(hv_t *v, const event_t *ev) {
    bool ctrl = ev->mods & MOD_CTRL, alt = ev->mods & MOD_ALT;
    if (ctrl && !alt) {
        switch (ev->key) {
        case 'o': open_dialog(v); break;
        case 'w': win_close(v->win); break;   /* frees v */
        case 'r': reload(v); break;
        case '=': case '+': zoom_step(v, 1); break;
        case '-': zoom_step(v, -1); break;
        case '0': zoom_step(v, 0); break;
        default: break;
        }
    } else if (!alt && ev->key == KEY_F5) {
        reload(v);
    }
}

static void hv_event(window_t *win, const event_t *ev) {
    hv_t *v = win_get_userdata(win);
    if (!v) return;
    switch (ev->type) {
    case EV_PAINT:
        paint_chrome(v);
        break;
    case EV_RESIZE:
        layout(v);
        clamp_scroll(v);
        v->pc_valid = false;
        v->resize_at = sys_uptime_ms() + 150;
        break;
    case EV_TIMER: {
        uint32_t now = sys_uptime_ms();
        if (v->tab) PIPE(tab_tick(v->tab));
        if (!win_is_valid(win)) return;
        if (v->host->ntabs) adopt_tabs(v);
        if (v->host->status[0]) {                /* e.g. "E-mail links cannot be opened in NovaOS" */
            set_msg(v, v->host->status);
            v->host->status[0] = 0;
        }
        if (v->resize_at && (int32_t)(now - v->resize_at) >= 0) {
            v->resize_at = 0;
            if (v->tab && v->tab->styled) PIPE(tab_restyle(v->tab));
        }
        if (v->msg[0] && (int32_t)(now - v->msg_until) >= 0) {
            v->msg[0] = 0;
            status_dirty(v);
        }
        tab_t *t = v->tab;                       /* the caret of a text field blinks */
        if (t && t->focus_ctrl >= 0 && t->focus_ctrl < t->pg.nctrls && is_text(&t->pg.ctrls[t->focus_ctrl]) &&
            ui_has_focus(v->page) && now - v->host->caret_t >= 530) {
            v->host->caret_t = now;
            v->host->caret_on = !v->host->caret_on;
            web_ctrl_t *c = &t->pg.ctrls[t->focus_ctrl];
            rect_t r = page_view(v);
            v->pc_valid = false;
            win_invalidate_rect(win, r.x + c->x - t->sx, r.y + c->y - t->sy, c->w, c->h);
        }
        if (page_sig(v) != v->sig) page_changed(v);
        break;
    }
    case EV_KEY_DOWN:
        window_keys(v, ev);
        break;
    case EV_THEME:
        v->pc_valid = false;
        break;
    case EV_CLOSE:
        if (v->tab) tab_free(v->tab);
        for (int i = 0; i < v->host->ntabs; i++) tab_free(v->host->tabs[i]);
        if (v->pc) surface_destroy(v->pc);
        free(v->host);
        free(v);
        win_set_userdata(win, NULL);
        break;
    default:
        break;
    }
}

static void hv_launch(const char *arg) {
    bdata_load();
    hv_t *v = calloc(1, sizeof *v);
    browser_t *host = calloc(1, sizeof(browser_t) + HV_HOST_SLACK);
    window_t *win = v && host ? win_create("HTML Viewer", HV_W, HV_H, WIN_RESIZABLE) : NULL;
    if (!win) { free(v); free(host); return; }
    v->win = win;
    v->host = host;
    host->win = win;
    host->host_view = host_view;           /* pages are laid out for the viewer's page area */
    v->press_link = -1;
    win_set_userdata(win, v);
    win_set_event_handler(win, hv_event);
    win_set_min_size(win, 500, 260);
    ui_menubar_add(ui_menubar(win), "File", "Open...\tCtrl+O|-|Reload\tF5|Open in Web Browser|-|Close\tCtrl+W",
                   file_menu, v);
    v->b_reload = ui_button(win, 0, 0, 10, 10, "Reload", on_reload, v);
    v->b_zout = ui_button(win, 0, 0, 10, 10, "Zoom out", on_zoom_out, v);
    v->zoom_lbl = ui_label(win, 0, 0, 10, 10, "100%");
    ui_set_align(v->zoom_lbl, ALIGN_CENTER | ALIGN_MIDDLE);
    v->b_zin = ui_button(win, 0, 0, 10, 10, "Zoom in", on_zoom_in, v);
    v->b_web = ui_button(win, 0, 0, 10, 10, "Open in Web Browser", on_web, v);
    v->page = ui_custom(win, 0, 0, 10, 10, page_draw, page_event, v);
    layout(v);
    sync_chrome(v);
    win_set_timer(win, 40);
    if (arg && *arg) open_file(v, arg);
}

/* ---- icon: a web page in a window with an orange </> badge ----------------------------------- */
static void stroke(surface_t *s, double x0, double y0, double x1, double y1, double th, uint32_t c) {
    double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
    if (len < 0.01) return;
    double nx = -dy / len * th / 2, ny = dx / len * th / 2;
    point_t p[4] = { { (int)lround(x0 + nx), (int)lround(y0 + ny) }, { (int)lround(x1 + nx), (int)lround(y1 + ny) },
                     { (int)lround(x1 - nx), (int)lround(y1 - ny) }, { (int)lround(x0 - nx), (int)lround(y0 - ny) } };
    gfx_fill_polygon(s, p, 4, c);
    gfx_fill_circle_aa(s, x0, y0, th / 2, c);
    gfx_fill_circle_aa(s, x1, y1, th / 2, c);
}

static void hv_icon(surface_t *s, int x, int y, int size) {
    double u = size / 64.0;
#define U(v) ((int)((v) * u + 0.5))
#define D(v) ((v) * u)
    uint32_t orange = RGB(228, 77, 38);
    /* the window: shadow, frame, white page, orange title strip with three dots */
    gfx_fill_rounded_rect(s, x + U(5), y + U(9), U(50), U(45), U(7), RGBA(0, 0, 0, 55));
    gfx_fill_rounded_rect(s, x + U(4), y + U(7), U(50), U(45), U(7), RGB(186, 194, 210));
    gfx_fill_rounded_rect(s, x + U(5), y + U(8), U(48), U(43), U(6), RGB(252, 252, 253));
    gfx_fill_rounded_gradient_v(s, x + U(4), y + U(7), U(50), U(13), U(7), RGB(255, 138, 82), orange);
    gfx_fill_rect(s, x + U(4), y + U(14), U(50), U(6), orange);
    for (int i = 0; i < 3; i++) gfx_fill_circle_aa(s, x + D(11 + i * 5.5), y + D(13.5), D(1.7), RGBA(255, 255, 255, 230));
    /* the page: a heading, a picture and lines of text */
    gfx_fill_rounded_rect(s, x + U(10), y + U(25), U(26), U(4.5), U(2), RGB(62, 70, 92));
    gfx_fill_rounded_gradient_v(s, x + U(10), y + U(33), U(14), U(12), U(2), RGB(110, 190, 255), RGB(190, 228, 255));
    point_t hill[3] = { { x + U(10), y + U(45) }, { x + U(16), y + U(37) }, { x + U(23), y + U(45) } };
    gfx_fill_polygon(s, hill, 3, RGB(66, 160, 90));
    static const int len[] = { 18, 13, 16 };
    for (int i = 0; i < 3; i++)
        gfx_fill_rounded_rect(s, x + U(27), y + U(34 + i * 5), U(len[i]), U(2.6), U(1.2), RGB(150, 162, 186));
    /* the badge */
    double cx = x + D(47), cy = y + D(47), r = D(12.5);
    gfx_fill_circle_aa(s, cx, cy + D(1), r + D(1.5), RGBA(0, 0, 0, 50));
    gfx_fill_circle_aa(s, cx, cy, r + D(1.6), RGB(255, 255, 255));
    gfx_fill_circle_aa(s, cx, cy, r, orange);
    double th = MAX(1.2, D(2.6));
    uint32_t w = RGB(255, 255, 255);
    stroke(s, cx - D(4), cy - D(5), cx - D(8.5), cy, th, w);
    stroke(s, cx - D(8.5), cy, cx - D(4), cy + D(5), th, w);
    stroke(s, cx + D(4), cy - D(5), cx + D(8.5), cy, th, w);
    stroke(s, cx + D(8.5), cy, cx + D(4), cy + D(5), th, w);
    stroke(s, cx + D(1.6), cy - D(6.5), cx - D(1.6), cy + D(6.5), th, w);
#undef U
#undef D
}

REGISTER_APP(.name = "HTML Viewer", .category = CAT_INTERNET,
             .description = "View HTML pages saved on this computer",
             .launch = hv_launch, .draw_icon = hv_icon, .file_types = "html,htm");

/* .html / .htm open here; the Web Browser (its .file_types) stays in Open With */
REGISTER_FILETYPE("html", "HTML Viewer", "HTML document", ICON_FILE_HTML);
REGISTER_FILETYPE("htm", "HTML Viewer", "HTML document", ICON_FILE_HTML);
