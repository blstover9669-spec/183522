/*
 * osaver_prompt.c - "Do you have a save file?" (NovaOS 2.5, item 14).
 *
 * ISO boots only: the NovaOS Drive installation (kernel/gui/bootsetup.c) calls
 * osaver_boot_prompt() after "Continue ISO", because nothing is saved on an ISO.
 * A full screen in the splash style with the question and Yes / No:
 *
 *   No   closes the screen: on to the desktop.
 *   Yes  shows a text box to paste (Ctrl+V) or type the code into. The instant
 *        checker looks at the text after every change: as soon as it is a whole,
 *        valid code (numbers, ':', '-', '(( ))' ... see kernel/osaver/codec.h) the
 *        restore starts by itself (the soft restart in osaver_restore.c). A paste,
 *        or Enter, that is not a valid code - or typing that can no longer become
 *        one - shows "That's not a valid save code" and plays the alert sound.
 *
 * Never blocks: done(ctx) is called once, when the screen closes.
 * Test hook: osaver_test=prompt on the command line opens the screen after boot.
 */
#include "osaver_internal.h"

#define BIG_TEXT   (4u << 20)       /* above this the full check runs in the restore */
#define BOX_W      640
#define BOX_H      176
#define BTN_W      124
#define BTN_H      44

enum { CHK_INCOMPLETE, CHK_VALID, CHK_INVALID };
enum { M_TYPE, M_PASTE, M_ENTER };

typedef struct prompt {
    window_t *win;
    void (*done)(void *);
    void *ctx;
    bool box;                     /* Yes was clicked: the text box is shown */
    char *buf;
    uint32_t len, cap;
    bool all_sel;
    bool error;                   /* "That's not a valid save code" shown   */
    bool restoring;
    int hover;                    /* 0 none, 1 Yes, 2 No, 3 box             */
    int focus;                    /* keyboard focus: 1 Yes, 2 No            */
    bool blink;
    int timer;
} prompt_t;

static prompt_t *P;

static const char *const QUESTION = "Do you have a save file?";
static const char *const NOT_VALID = "That's not a valid save code";

/* ---- layout ------------------------------------------------------------------------- */
static int logo_size(void) { return CLAMP(screen_height() / 7, 64, 160); }
static int cx(void) { return screen_width() / 2; }
static int top_y(void) { return MAX(40, screen_height() / 2 - 300); }
static int title_y(void) { return top_y() + logo_size() + 36; }
static rect_t yes_rect(void) { return rect_make(cx() - BTN_W - 10, title_y() + 64, BTN_W, BTN_H); }
static rect_t no_rect(void)  { return rect_make(cx() + 10, title_y() + 64, BTN_W, BTN_H); }
static rect_t box_rect(void) { return rect_make(cx() - BOX_W / 2, title_y() + 64 + BTN_H + 36, BOX_W, BOX_H); }
static rect_t err_rect(void) { rect_t b = box_rect(); return rect_make(b.x, b.y + b.h + 16, b.w, 28); }

/* ---- drawing ------------------------------------------------------------------------- */
static void draw_button(surface_t *s, rect_t r, const char *label, bool primary, bool hover, bool focus) {
    uint32_t c = primary ? osv_start_color() : RGBA(255, 255, 255, 34);
    if (hover) c = primary ? color_shade(c, 16) : RGBA(255, 255, 255, 56);
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, r.h / 2, c);
    if (!primary) gfx_rounded_rect(s, r.x, r.y, r.w, r.h, r.h / 2, RGBA(255, 255, 255, 90));
    if (focus) gfx_rounded_rect(s, r.x - 3, r.y - 3, r.w + 6, r.h + 6, r.h / 2 + 3, RGBA(255, 255, 255, 150));
    gfx_text_aligned(s, r, label, COLOR_WHITE, FONT_UI_LARGE, ALIGN_CENTER | ALIGN_MIDDLE);
}

typedef struct { uint32_t start, n; } row_t;

static void draw_box(prompt_t *p, surface_t *s) {
    rect_t b = box_rect();
    gfx_fill_rounded_rect(s, b.x, b.y, b.w, b.h, 10, RGBA(0, 0, 0, 90));
    gfx_rounded_rect(s, b.x, b.y, b.w, b.h, 10, p->error ? RGB(255, 120, 120) : color_alpha(osv_start_color(), 230));
    const font_t *f = FONT_MONO;
    int cw = font_char_width(f, 'M'), lh = font_height(f);
    int cols = MAX(1, (b.w - 24) / cw), rows = MAX(1, (b.h - 20) / lh);
    row_t rr[32];
    int nr = 0;
    if (rows > 32) rows = 32;
    bool big = p->len > 65536;
    if (!big) {
        /* wrap from the start (newlines break rows), keep the last `rows` rows */
        uint32_t i = 0, rs = 0;
        int total = 0;
        while (1) {
            bool end = i >= p->len;
            if (end || p->buf[i] == '\n' || i - rs == (uint32_t)cols) {
                row_t row = { rs, i - rs };
                rr[total % rows] = row;
                total++;
                if (end) break;
                rs = (p->buf[i] == '\n') ? i + 1 : i;
                i = rs;
                continue;
            }
            i++;
        }
        nr = MIN(total, rows);
        if (total > rows) {        /* rotate the ring into order */
            row_t tmp[32];
            for (int k = 0; k < rows; k++) tmp[k] = rr[(total + k) % rows];
            memcpy(rr, tmp, sizeof(row_t) * (size_t)rows);
        }
    } else {
        uint32_t span = (uint32_t)(rows * cols);
        uint32_t st = p->len > span ? p->len - span : 0;
        st += (p->len - st) % (uint32_t)cols;               /* the last row ends at the end */
        for (uint32_t o = st; o < p->len && nr < rows; o += (uint32_t)cols) {
            rr[nr].start = o;
            rr[nr].n = MIN((uint32_t)cols, p->len - o);
            nr++;
        }
        if (!nr) { rr[0].start = p->len; rr[0].n = 0; nr = 1; }
    }
    int x0 = b.x + 12, y0 = b.y + 10;
    rect_t old = s->clip;
    surface_set_clip(s, rect_intersect(old, rect_make(b.x + 4, b.y + 4, b.w - 8, b.h - 8)));
    for (int k = 0; k < nr; k++) {
        int y = y0 + k * lh;
        if (p->all_sel && p->len && rr[k].n)
            gfx_fill_rect(s, x0, y, (int)rr[k].n * cw, lh, color_alpha(osv_start_color(), 150));
        for (uint32_t j = 0; j < rr[k].n; j++) {
            unsigned char c = (unsigned char)p->buf[rr[k].start + j];
            if (c == '\n') c = ' ';
            if (c > ' ') gfx_char(s, x0 + (int)j * cw, y, c, COLOR_WHITE, f);
        }
    }
    if (!p->restoring && p->blink) {
        int k = nr - 1;
        int x = x0 + (int)rr[k].n * cw, y = y0 + k * lh;
        gfx_fill_rect(s, x, y, 2, lh, COLOR_WHITE);
    }
    s->clip = old;
}

static void prompt_paint(prompt_t *p, surface_t *s) {
    osv_draw_backdrop(s, rect_make(0, 0, s->w, s->h));
    int ls = logo_size();
    ui_draw_logo(s, cx() - ls / 2, top_y(), ls);
    gfx_text_aligned(s, rect_make(0, title_y(), s->w, 40), QUESTION, COLOR_WHITE, FONT_UI_TITLE, ALIGN_CENTER | ALIGN_MIDDLE);
    draw_button(s, yes_rect(), "Yes", true, p->hover == 1, p->focus == 1 && !p->box);
    draw_button(s, no_rect(), "No", false, p->hover == 2, p->focus == 2 && !p->box);
    if (p->box) draw_box(p, s);
    if (p->error)
        gfx_text_aligned(s, err_rect(), NOT_VALID, RGB(255, 130, 130), FONT_UI_LARGE, ALIGN_CENTER | ALIGN_MIDDLE);
}

/* ---- the instant checker ------------------------------------------------------------ */
/* While typing, a code counts only once it is complete (so "13224:2" is not taken
   for version 2 before the ".5" is typed); a paste or Enter takes it as it is. */
static int classify(const char *t, uint32_t n, int mode) {
    if (n > OSAVER_MAX_CODE) return CHK_INVALID;
    if (n > BIG_TEXT) return osaver_check_code(t, n) ? CHK_VALID : CHK_INVALID;
    static osc_parser_t ps;
    osc_parser_init(&ps, NULL);
    if (osc_parser_feed(&ps, t, n)) return CHK_INVALID;
    if (!osc_parser_complete(&ps)) {
        if (mode != M_TYPE && osc_parser_finish(&ps) == 0) return CHK_VALID;
        return CHK_INCOMPLETE;
    }
    return osc_parser_finish(&ps) == 0 ? CHK_VALID : CHK_INVALID;
}

static void close_prompt(prompt_t *p) {
    void (*done)(void *) = p->done;
    void *ctx = p->ctx;
    window_t *win = p->win;
    P = NULL;
    free(p->buf);
    free(p);
    if (win_is_valid(win)) { win_set_userdata(win, NULL); win_close(win); }
    if (done) done(ctx);
}

static void show_error(prompt_t *p, bool sound) {
    if (sound) osv_alert();
    if (!p->error) klog("osaver: That's not a valid save code (%u characters)", p->len);
    p->error = true;
    win_invalidate(p->win);
}

static void restored(void *ctx, int result) {
    prompt_t *p = ctx;
    if (P != p) return;
    p->restoring = false;
    if (result == 0) { close_prompt(p); return; }
    show_error(p, true);
}

static void check(prompt_t *p, int mode) {
    if (p->restoring) return;
    if (!p->len) {
        if (p->error) { p->error = false; win_invalidate(p->win); }
        if (mode != M_TYPE) show_error(p, true);
        return;
    }
    int st = classify(p->buf, p->len, mode);
    if (st == CHK_VALID) {
        klog("osaver: save code accepted (%u characters), restoring", p->len);
        p->error = false;
        p->restoring = true;
        win_invalidate(p->win);
        if (osv_restore_start(p->buf, p->len, NULL, restored, p) != 0) {
            p->restoring = false;
            show_error(p, true);
        }
        return;
    }
    if (st == CHK_INVALID || mode != M_TYPE) {
        show_error(p, mode != M_TYPE || !p->error);
        return;
    }
    if (p->error) { p->error = false; win_invalidate(p->win); }
}

/* ---- editing ---------------------------------------------------------------------------- */
static bool reserve(prompt_t *p, uint32_t extra) {
    if (p->len + extra + 1 <= p->cap) return true;
    uint32_t nc = p->cap ? p->cap : 1024;
    while (nc < p->len + extra + 1) nc *= 2;
    char *n = realloc(p->buf, nc);
    if (!n) return false;
    p->buf = n;
    p->cap = nc;
    return true;
}

static void insert(prompt_t *p, const char *s, uint32_t n) {
    if (p->all_sel) { p->len = 0; p->all_sel = false; }
    if (!reserve(p, n)) return;
    for (uint32_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == OSC_HS || c == '\r' || c == 0) continue;      /* hidden separators mean nothing */
        p->buf[p->len++] = c;
    }
    p->buf[p->len] = 0;
}

static void on_yes(prompt_t *p) {
    if (p->box) return;
    p->box = true;
    p->blink = true;
    klog("osaver: Do you have a save file? Yes");
    win_invalidate(p->win);
}

static void on_no(prompt_t *p) {
    if (p->restoring) return;
    klog("osaver: Do you have a save file? No");
    close_prompt(p);
}

static void box_key(prompt_t *p, const event_t *ev) {
    bool ctrl = ev->mods & MOD_CTRL;
    if (ctrl) {
        if (ev->key == 'v') {
            const char *c = clipboard_get();
            insert(p, c, (uint32_t)strlen(c));
            win_invalidate(p->win);
            check(p, M_PASTE);
        } else if (ev->key == 'a') {
            p->all_sel = p->len > 0;
            win_invalidate(p->win);
        }
        return;
    }
    switch (ev->key) {
    case KEY_BACKSPACE:
    case KEY_DELETE:
        if (p->all_sel) { p->len = 0; p->all_sel = false; }
        else if (ev->key == KEY_BACKSPACE && p->len) p->len--;
        else return;
        if (p->buf) p->buf[p->len] = 0;
        win_invalidate(p->win);
        check(p, M_TYPE);
        break;
    case KEY_ENTER:
        check(p, M_ENTER);
        break;
    case KEY_ESCAPE:
        if (p->all_sel) { p->all_sel = false; win_invalidate(p->win); }
        break;
    default:
        break;
    }
}

static void prompt_event(window_t *win, const event_t *ev) {
    prompt_t *p = win_get_userdata(win);
    if (!p) return;
    switch (ev->type) {
    case EV_PAINT:
        prompt_paint(p, win_surface(win));
        break;
    case EV_MOUSE_MOVE: {
        int h = rect_contains(yes_rect(), ev->x, ev->y) ? 1 : rect_contains(no_rect(), ev->x, ev->y) ? 2 :
                (p->box && rect_contains(box_rect(), ev->x, ev->y)) ? 3 : 0;
        if (h != p->hover) { p->hover = h; win_invalidate(win); }
        win_set_cursor(win, h == 3 ? CURSOR_IBEAM : h ? CURSOR_HAND : CURSOR_ARROW);
        break;
    }
    case EV_MOUSE_DOWN:
        if (ev->button != MOUSE_LEFT || p->restoring) break;
        if (rect_contains(yes_rect(), ev->x, ev->y)) on_yes(p);
        else if (rect_contains(no_rect(), ev->x, ev->y)) on_no(p);
        break;
    case EV_CHAR:
        if (!p->box || p->restoring || ev->ch < 32 || ev->ch > 126) break;
        {
            char c = (char)ev->ch;
            insert(p, &c, 1);
            p->blink = true;
            win_invalidate(win);
            check(p, M_TYPE);
        }
        break;
    case EV_KEY_DOWN:
        if (p->restoring) break;
        if (p->box) { box_key(p, ev); break; }
        if (ev->key == KEY_LEFT || ev->key == KEY_RIGHT || ev->key == KEY_TAB) {
            p->focus = p->focus == 1 ? 2 : 1;
            win_invalidate(win);
        } else if (ev->key == KEY_ENTER || ev->key == ' ') {
            if (p->focus == 2) on_no(p); else on_yes(p);
        }
        break;
    case EV_TIMER:
        if (p->box) {
            p->blink = !p->blink;
            rect_t b = box_rect();
            win_invalidate_rect(win, b.x, b.y, b.w, b.h);
        }
        break;
    case EV_CLOSE_REQUEST:          /* Alt+F4 = No */
        on_no(p);
        break;
    case EV_CLOSE:
        if (P == p) {               /* closed from outside */
            P = NULL;
            void (*done)(void *) = p->done;
            void *ctx = p->ctx;
            free(p->buf);
            free(p);
            win_set_userdata(win, NULL);
            if (done) done(ctx);
        }
        break;
    default:
        break;
    }
}

void osaver_boot_prompt(void (*done)(void *ctx), void *ctx) {
    if (P) { if (done) done(ctx); return; }
    prompt_t *p = calloc(1, sizeof *p);
    if (!p) { if (done) done(ctx); return; }
    window_t *win = win_create("NovaOS", screen_width(), screen_height(),
                               WIN_FRAMELESS | WIN_NO_TASKBAR | WIN_NO_SHADOW | WIN_CONFIRM_CLOSE);
    if (!win) { free(p); if (done) done(ctx); return; }
    p->win = win;
    p->done = done;
    p->ctx = ctx;
    p->focus = 1;
    P = p;
    win_set_userdata(win, p);
    win_set_event_handler(win, prompt_event);
    win_set_fullscreen(win, true);
    p->timer = win_set_timer(win, 530);
    klog("osaver: Do you have a save file?");
}

/* osaver_test=prompt (or promptN: after N seconds): open the screen after boot (tests). */
static void test_prompt(void *ctx) { (void)ctx; osaver_boot_prompt(NULL, NULL); }
/* osaver_test=restorefileN: after N seconds load the code in OSV_COPY_FILE
   (osaver_restore_file, the code read from a file in pieces). */
static void test_restore_done(void *ctx) { (void)ctx; klog("osaver: test restore from file finished"); }
static void test_restore(void *ctx) {
    (void)ctx;
    int r = osaver_restore_file(OSV_COPY_FILE, test_restore_done, NULL);
    klog("osaver: test restore from %s: %d", OSV_COPY_FILE, r);
}
static void prompt_test_init(void) {
    const char *t = sys_cmdline_get("osaver_test");
    const char *p = t ? strstr(t, "prompt") : NULL;
    if (p) timer_add(400 + 1000 * (uint32_t)atoi(p + 6), test_prompt, NULL, false);
    p = t ? strstr(t, "restorefile") : NULL;
    if (p) timer_add(400 + 1000 * (uint32_t)atoi(p + 11), test_restore, NULL, false);
}
REGISTER_INIT(INIT_LATE, prompt_test_init);
