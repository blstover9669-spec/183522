/*
 * terminal_app.c - "Terminal": a graphical terminal emulator for NovaOS.
 *
 * This file is the emulator half: the window, a scrollback of logical lines
 * (re-wrapped to the window width whenever it is drawn, so resizing reflows
 * the text), a small ANSI/SGR escape parser for colours, the cursor, mouse
 * selection + clipboard, scrolling, command-line editing, history and the
 * asynchronous "job" mechanism used by long-running commands. The shell
 * itself lives in terminal_shell.c.
 *
 * Every window has its own term_t (win_set_userdata), freed on EV_CLOSE, so
 * any number of terminals can be open at once. Nothing here blocks: long
 * running commands (sleep, timer, sl, matrix) are driven by window timers.
 */
#include "terminal_term.h"

#define PAD_X 8              /* left margin of the text                      */
#define PAD_Y 6              /* top margin                                   */
#define SB_W  12             /* reserved on the right for the scrollbar      */
#define INIT_COLS 88
#define INIT_ROWS 26
#define TERM_EV_CLOSE 0x7E57C105   /* EV_USER payload: close this window   */

/* ---- colour themes ---------------------------------------------------------------- */
#define C(r, g, b) RGB(r, g, b)
static const tpalette_t s_palettes[] = {
    { "nova", "NovaOS night blue (default)",
      { C(52, 57, 74), C(237, 106, 120), C(139, 213, 140), C(240, 200, 120),
        C(110, 160, 255), C(200, 140, 240), C(100, 210, 220), C(190, 197, 212),
        C(110, 118, 140), C(255, 130, 140), C(160, 235, 160), C(255, 220, 140),
        C(140, 185, 255), C(220, 170, 255), C(130, 230, 240), C(244, 246, 252),
        C(214, 221, 235), C(22, 24, 34) },
      C(130, 175, 255), RGBA(110, 160, 255, 90) },
    { "classic", "black and grey, like a VGA console",
      { C(12, 12, 12), C(197, 15, 31), C(19, 161, 14), C(193, 156, 0),
        C(0, 55, 218), C(136, 23, 152), C(58, 150, 221), C(204, 204, 204),
        C(118, 118, 118), C(231, 72, 86), C(22, 198, 12), C(249, 241, 165),
        C(59, 120, 255), C(180, 0, 158), C(97, 214, 214), C(242, 242, 242),
        C(204, 204, 204), C(12, 12, 12) },
      C(204, 204, 204), RGBA(200, 200, 200, 80) },
    { "light", "dark text on white paper",
      { C(56, 58, 66), C(214, 64, 58), C(56, 150, 60), C(176, 118, 0),
        C(50, 108, 230), C(160, 40, 160), C(0, 128, 170), C(120, 122, 130),
        C(150, 152, 160), C(236, 96, 84), C(80, 172, 80), C(200, 140, 20),
        C(84, 136, 250), C(186, 70, 186), C(20, 150, 196), C(28, 30, 36),
        C(46, 48, 56), C(250, 250, 251) },
      C(50, 108, 230), RGBA(50, 108, 230, 60) },
    { "solarized", "Solarized dark",
      { C(7, 54, 66), C(220, 50, 47), C(133, 153, 0), C(181, 137, 0),
        C(38, 139, 210), C(211, 54, 130), C(42, 161, 152), C(238, 232, 213),
        C(88, 110, 117), C(236, 90, 70), C(165, 185, 40), C(215, 170, 40),
        C(90, 170, 235), C(108, 113, 196), C(90, 195, 185), C(253, 246, 227),
        C(147, 161, 161), C(0, 43, 54) },
      C(147, 161, 161), RGBA(147, 161, 161, 70) },
    { "dracula", "Dracula purple",
      { C(33, 34, 44), C(255, 85, 85), C(80, 250, 123), C(241, 250, 140),
        C(189, 147, 249), C(255, 121, 198), C(139, 233, 253), C(248, 248, 242),
        C(98, 114, 164), C(255, 110, 110), C(105, 255, 148), C(255, 255, 165),
        C(214, 172, 255), C(255, 146, 223), C(164, 255, 255), C(255, 255, 255),
        C(248, 248, 242), C(40, 42, 54) },
      C(248, 248, 242), RGBA(98, 114, 164, 120) },
    { "gruvbox", "Gruvbox warm retro",
      { C(60, 56, 54), C(204, 36, 29), C(152, 151, 26), C(215, 153, 33),
        C(69, 133, 136), C(177, 98, 134), C(104, 157, 106), C(168, 153, 132),
        C(146, 131, 116), C(251, 73, 52), C(184, 187, 38), C(250, 189, 47),
        C(131, 165, 152), C(211, 134, 155), C(142, 192, 124), C(235, 219, 178),
        C(235, 219, 178), C(40, 40, 40) },
      C(235, 219, 178), RGBA(146, 131, 116, 100) },
    { "matrix", "green phosphor",
      { C(20, 50, 25), C(120, 220, 110), C(60, 230, 90), C(170, 240, 130),
        C(50, 180, 90), C(120, 210, 150), C(90, 220, 170), C(150, 220, 160),
        C(50, 110, 60), C(160, 255, 150), C(110, 255, 130), C(200, 255, 170),
        C(90, 220, 130), C(160, 240, 190), C(140, 250, 200), C(210, 255, 215),
        C(60, 230, 90), C(4, 14, 6) },
      C(80, 255, 110), RGBA(60, 230, 90, 70) },
    { "amber", "amber monochrome monitor",
      { C(60, 40, 10), C(255, 140, 40), C(255, 190, 60), C(255, 210, 90),
        C(220, 150, 30), C(255, 170, 80), C(255, 200, 110), C(240, 190, 120),
        C(130, 90, 30), C(255, 170, 80), C(255, 210, 100), C(255, 230, 140),
        C(250, 180, 60), C(255, 200, 120), C(255, 220, 150), C(255, 238, 200),
        C(255, 176, 0), C(22, 15, 4) },
      C(255, 190, 40), RGBA(255, 176, 0, 70) },
};
#undef C

/* New windows start with the theme last picked with `color`. */
static int s_default_theme = 0;

int term_palette_count(void) { return ARRAY_SIZE(s_palettes); }

const tpalette_t *term_palette(int index) {
    if (index < 0 || index >= ARRAY_SIZE(s_palettes)) index = 0;
    return &s_palettes[index];
}

void term_set_theme(term_t *t, int index) {
    if (index < 0 || index >= ARRAY_SIZE(s_palettes)) return;
    t->theme = index;
    s_default_theme = index;
    term_invalidate(t);
}

void term_invalidate(term_t *t) {
    if (t && t->win) win_invalidate(t->win);
}

/* ---- scrollback ------------------------------------------------------------------ */
static tline_t *line_at(term_t *t, int i) { return &t->lines[(t->first + i) % TERM_MAX_LINES]; }
static tline_t *last_line(term_t *t) { return line_at(t, t->count - 1); }

static bool line_reserve(tline_t *l, int n) {
    if (n <= l->cap) return true;
    int nc = l->cap ? l->cap * 2 : 32;
    while (nc < n) nc *= 2;
    tcell_t *c = realloc(l->c, (size_t)nc * sizeof *c);
    if (!c) return false;
    l->c = c;
    l->cap = nc;
    return true;
}

static void term_newline(term_t *t) {
    if (t->count == TERM_MAX_LINES) {          /* drop the oldest line */
        tline_t *l = &t->lines[t->first];
        free(l->c);
        l->c = NULL;
        l->len = l->cap = 0;
        t->first = (t->first + 1) % TERM_MAX_LINES;
        t->count--;
        t->abs_base++;
    }
    tline_t *n = line_at(t, t->count);
    n->len = 0;
    t->count++;
    t->out_col = 0;
}

void term_clear(term_t *t) {
    for (int i = 0; i < t->count; i++) {
        tline_t *l = line_at(t, i);
        free(l->c);
        l->c = NULL;
        l->len = l->cap = 0;
    }
    t->abs_base += (uint32_t)t->count;
    t->first = 0;
    t->count = 1;
    t->out_col = 0;
    t->scroll = 0;
    t->has_sel = false;
    term_invalidate(t);
}

static void put_cell(term_t *t, uint8_t ch) {
    if (t->out_col >= TERM_LINE_MAX) term_newline(t);
    tline_t *l = last_line(t);
    if (!line_reserve(l, t->out_col + 1)) return;     /* out of memory: drop */
    while (l->len < t->out_col) l->c[l->len++] = (tcell_t){ ' ', TC_FG, TC_BG, 0 };
    tcell_t c = { ch, t->fg, t->bg, t->attr };
    if (t->reverse) { uint8_t x = c.fg; c.fg = c.bg; c.bg = x; }
    l->c[t->out_col++] = c;
    if (t->out_col > l->len) l->len = t->out_col;
}

/* Select Graphic Rendition: colours and attributes. */
static void apply_sgr(term_t *t) {
    if (t->esc_np == 0) { t->esc_par[0] = 0; t->esc_np = 1; }
    for (int i = 0; i < t->esc_np; i++) {
        int p = t->esc_par[i];
        if (p == 0) { t->fg = TC_FG; t->bg = TC_BG; t->attr = 0; t->reverse = false; }
        else if (p == 1) t->attr |= TA_BOLD;
        else if (p == 2) t->attr |= TA_DIM;
        else if (p == 4) t->attr |= TA_UNDER;
        else if (p == 7) t->reverse = true;
        else if (p == 22) t->attr &= (uint8_t)~(TA_BOLD | TA_DIM);
        else if (p == 24) t->attr &= (uint8_t)~TA_UNDER;
        else if (p == 27) t->reverse = false;
        else if (p >= 30 && p <= 37) t->fg = (uint8_t)(p - 30);
        else if (p == 39) t->fg = TC_FG;
        else if (p >= 40 && p <= 47) t->bg = (uint8_t)(p - 40);
        else if (p == 49) t->bg = TC_BG;
        else if (p >= 90 && p <= 97) t->fg = (uint8_t)(p - 90 + 8);
        else if (p >= 100 && p <= 107) t->bg = (uint8_t)(p - 100 + 8);
        else if ((p == 38 || p == 48) && i + 2 < t->esc_np && t->esc_par[i + 1] == 5) {
            int n = t->esc_par[i + 2];
            if (n >= 0 && n < 16) { if (p == 38) t->fg = (uint8_t)n; else t->bg = (uint8_t)n; }
            i += 2;
        }
    }
}

/* Write bytes to the terminal: handles \n \r \t \b \a and ESC [ ... m/J/K. */
void term_write(term_t *t, const char *s, int n) {
    if (!t || !s) return;
    int before = 0;
    bool keep_view = t->scroll > 0;
    if (keep_view) before = t->total_rows;
    for (int i = 0; i < n; i++) {
        unsigned char b = (unsigned char)s[i];
        if (t->esc == 1) {
            if (b == '[') { t->esc = 2; t->esc_np = 0; t->esc_par[0] = 0; }
            else t->esc = 0;
            continue;
        }
        if (t->esc == 2) {
            if (b >= '0' && b <= '9') {
                if (t->esc_np == 0) { t->esc_np = 1; t->esc_par[0] = 0; }
                int *p = &t->esc_par[t->esc_np - 1];
                if (*p < 10000) *p = *p * 10 + (b - '0');
            } else if (b == ';') {
                if (t->esc_np == 0) { t->esc_np = 1; t->esc_par[0] = 0; }
                if (t->esc_np < 8) t->esc_par[t->esc_np++] = 0;
            } else if (b >= 0x40 && b <= 0x7E) {
                if (b == 'm') apply_sgr(t);
                else if (b == 'J') term_clear(t);
                else if (b == 'K') { tline_t *l = last_line(t); if (l->len > t->out_col) l->len = t->out_col; }
                t->esc = 0;
            } else if (b < 0x20 || b > 0x7E) {
                t->esc = 0;
            }
            continue;
        }
        switch (b) {
        case 0x1B: t->esc = 1; break;
        case '\n': term_newline(t); break;
        case '\r': t->out_col = 0; break;
        case '\t': do put_cell(t, ' '); while (t->out_col % 8); break;
        case '\b': if (t->out_col > 0) t->out_col--; break;
        case '\a': sys_beep(880, 60); break;
        default:
            if (b >= 0x20) put_cell(t, b);
            break;
        }
    }
    if (keep_view) {
        /* keep the scrolled-back view still while output arrives */
        int total = 0;
        for (int i = 0; i < t->count; i++) {
            int len = line_at(t, i)->len;
            total += len <= 0 ? 1 : (len + t->cols - 1) / t->cols;
        }
        if (total > before && before > 0) t->scroll += total - before;
    }
    term_invalidate(t);
}

void term_puts(term_t *t, const char *s) { term_write(t, s, (int)strlen(s)); }

void term_printf(term_t *t, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    if (n > 0) term_write(t, buf, n);
}

int term_input_line(term_t *t) { return t->input_on ? 1 : 0; }

/* ---- layout (wrapping logical lines into screen rows) -------------------------- */
static void term_metrics(term_t *t) {
    t->cw = font_char_width(t->font, 'M');
    t->chh = font_height(t->font);
    int w = win_width(t->win), h = win_height(t->win);
    t->cols = MAX(8, (w - PAD_X - SB_W) / t->cw);
    t->rows = CLAMP((h - PAD_Y * 2) / t->chh, 1, TERM_MAX_VROWS);
}

/* Characters shown on line i; the last line also shows the command line. */
static int disp_len(term_t *t, int i) {
    tline_t *l = line_at(t, i);
    if (i == t->count - 1 && t->input_on) return l->len + t->in_len;
    return l->len;
}

static bool disp_cell(term_t *t, int i, int pos, tcell_t *out) {
    tline_t *l = line_at(t, i);
    if (pos < l->len) { *out = l->c[pos]; return true; }
    if (i == t->count - 1 && t->input_on) {
        int k = pos - l->len;
        if (k >= 0 && k < t->in_len) {
            *out = (tcell_t){ (uint8_t)t->in[k], t->in_fg[k], TC_BG, 0 };
            return true;
        }
    }
    return false;
}

static int line_rows(term_t *t, int i) {
    int n = disp_len(t, i);
    if (i == t->count - 1 && t->input_on) n = MAX(n, line_at(t, i)->len + t->in_cur + 1);
    return n <= 0 ? 1 : (n + t->cols - 1) / t->cols;
}

static void term_layout(term_t *t) {
    int total = 0;
    for (int i = 0; i < t->count; i++) total += line_rows(t, i);
    t->total_rows = total;
    t->scroll = CLAMP(t->scroll, 0, MAX(0, total - t->rows));
    int top = MAX(0, total - t->rows - t->scroll);
    t->top_row = top;
    int row = 0, i = 0;
    while (i < t->count) {
        int r = line_rows(t, i);
        if (row + r > top) break;
        row += r;
        i++;
    }
    int sub = top - row;
    for (int v = 0; v < t->rows; v++) {
        if (i >= t->count) { t->vrow[v].idx = -1; t->vrow[v].off = 0; continue; }
        t->vrow[v].idx = i;
        t->vrow[v].off = sub * t->cols;
        if (++sub >= line_rows(t, i)) { sub = 0; i++; }
    }
}

/* Colour the command line as it is typed: known commands green, unknown red,
   quoted strings yellow, $variables magenta, operators cyan. */
static void input_colors(term_t *t) {
    const char *s = t->in;
    int n = t->in_len, i = 0;
    bool cmdpos = true;
    while (i < n) {
        char c = s[i];
        if (c == ' ') { t->in_fg[i++] = TC_FG; continue; }
        if (strchr("|;&<>", c)) {
            t->in_fg[i++] = 6;
            if (c != '<' && c != '>') cmdpos = true;
            continue;
        }
        int j = i;
        char q = 0;
        while (j < n) {
            char ch = s[j];
            if (q) { if (ch == q) q = 0; j++; continue; }
            if (ch == '\\') { j += 2; continue; }
            if (ch == '"' || ch == '\'') { q = ch; j++; continue; }
            if (ch == ' ' || strchr("|;&<>", ch)) break;
            j++;
        }
        if (j > n) j = n;
        uint8_t base = TC_FG;
        if (cmdpos) base = term_shell_is_command(s + i, j - i) ? 10 : 9;
        q = 0;
        bool var = false;
        for (int k = i; k < j; k++) {
            char ch = s[k];
            if (q) { t->in_fg[k] = (q == '"' && var) ? 5 : 3; if (ch == q) q = 0; continue; }
            if (ch == '"' || ch == '\'') { q = ch; t->in_fg[k] = 3; var = false; continue; }
            if (ch == '$') var = true;
            else if (var && !(isalnum((unsigned char)ch) || ch == '_' || ch == '?' || ch == '{' || ch == '}')) var = false;
            t->in_fg[k] = var ? 5 : base;
        }
        cmdpos = false;
        i = j;
    }
}

/* ---- painting ------------------------------------------------------------------------ */
static bool sel_contains(term_t *t, uint32_t line, int col) {
    if (!t->has_sel) return false;
    uint32_t l0 = t->sa_line, l1 = t->sb_line;
    int c0 = t->sa_col, c1 = t->sb_col;
    if (l0 > l1 || (l0 == l1 && c0 > c1)) {
        uint32_t tl = l0; l0 = l1; l1 = tl;
        int tc = c0; c0 = c1; c1 = tc;
    }
    if (line < l0 || line > l1) return false;
    if (line == l0 && col < c0) return false;
    if (line == l1 && col >= c1) return false;
    return true;
}

static rect_t sb_track(term_t *t) {
    return rect_make(win_width(t->win) - SB_W + 3, 4, 6, win_height(t->win) - 8);
}

static rect_t sb_thumb(term_t *t) {
    rect_t tr = sb_track(t);
    if (t->total_rows <= t->rows || tr.h <= 0) return rect_make(0, 0, 0, 0);
    int th = MAX(24, tr.h * t->rows / t->total_rows);
    if (th > tr.h) th = tr.h;
    int range = t->total_rows - t->rows;
    int ty = tr.y + (int)((long long)(tr.h - th) * t->top_row / range);
    return rect_make(tr.x, ty, tr.w, th);
}

static void term_paint(term_t *t) {
    surface_t *s = win_surface(t->win);
    const tpalette_t *p = term_palette(t->theme);
    gfx_fill_rect(s, 0, 0, s->w, s->h, p->pal[TC_BG]);
    if (t->job.active && t->job.paint) { t->job.paint(t, s); return; }

    if (t->input_on) input_colors(t);
    term_layout(t);
    const font_t *bold = t->font == FONT_MONO ? FONT_MONO_BOLD : t->font;
    t->cursor_rect = rect_make(0, 0, 0, 0);

    for (int v = 0; v < t->rows; v++) {
        int idx = t->vrow[v].idx;
        if (idx < 0) continue;
        int off = t->vrow[v].off, n = disp_len(t, idx);
        uint32_t abs = t->abs_base + (uint32_t)idx;
        int y = PAD_Y + v * t->chh;
        for (int col = 0; col < t->cols; col++) {
            int pos = off + col, x = PAD_X + col * t->cw;
            bool sel = sel_contains(t, abs, pos);
            tcell_t c;
            if (pos >= n || !disp_cell(t, idx, pos, &c)) {
                /* show a selected line break as one highlighted cell */
                if (sel && pos == n) gfx_fill_rect(s, x, y, t->cw, t->chh, p->selection);
                break;
            }
            uint32_t fg = p->pal[c.fg < 18 ? c.fg : TC_FG];
            if (c.bg != TC_BG && c.bg < 18) gfx_fill_rect(s, x, y, t->cw, t->chh, p->pal[c.bg]);
            if (sel) gfx_fill_rect(s, x, y, t->cw, t->chh, p->selection);
            if (c.attr & TA_DIM) fg = color_mix(fg, p->pal[TC_BG], 110);
            if (c.ch != ' ') gfx_char(s, x, y, c.ch, fg, (c.attr & TA_BOLD) ? bold : t->font);
            if (c.attr & TA_UNDER) gfx_hline(s, x, y + t->chh - 2, t->cw, fg);
        }
    }

    /* cursor */
    if (t->input_on) {
        int idx = t->count - 1;
        int pos = last_line(t)->len + t->in_cur;
        int roff = (pos / t->cols) * t->cols, col = pos % t->cols;
        for (int v = 0; v < t->rows; v++) {
            if (t->vrow[v].idx != idx || t->vrow[v].off != roff) continue;
            int x = PAD_X + col * t->cw, y = PAD_Y + v * t->chh;
            t->cursor_rect = rect_make(x, y, t->cw, t->chh);
            if (t->focused && t->blink_on) {
                gfx_fill_rect(s, x, y, t->cw, t->chh, p->cursor);
                tcell_t c;
                if (disp_cell(t, idx, pos, &c) && c.ch != ' ')
                    gfx_char(s, x, y, c.ch, p->pal[TC_BG], t->font);
            } else if (!t->focused) {
                gfx_rect(s, x, y, t->cw, t->chh, color_alpha(p->cursor, 200));
            }
            break;
        }
    }

    /* overlay scrollbar */
    rect_t th = sb_thumb(t);
    if (!rect_empty(th)) {
        bool hot = t->sb_hover || t->sb_drag;
        if (hot) {
            rect_t tr = sb_track(t);
            gfx_fill_rounded_rect(s, tr.x - 1, tr.y, tr.w + 2, tr.h, 4, color_alpha(p->pal[TC_FG], 24));
        }
        gfx_fill_rounded_rect(s, th.x, th.y, th.w, th.h, 3, color_alpha(p->pal[TC_FG], hot ? 150 : 80));
    }
}

/* ---- jobs ------------------------------------------------------------------------- */
static void blink_reset(term_t *t);

static void free_queue(term_t *t) {
    while (t->queue) {
        tseg_t *n = t->queue->next;
        free(t->queue);
        t->queue = n;
    }
}

void term_job_start(term_t *t, int interval_ms, bool (*tick)(term_t *),
                    void (*paint)(term_t *, surface_t *), void (*cleanup)(term_t *, bool)) {
    tjob_t *j = &t->job;
    if (j->active) term_job_stop(t, true);
    j->active = true;
    j->tick = tick;
    j->paint = paint;
    j->cleanup = cleanup;
    j->start = sys_uptime_ms();
    j->frame = 0;
    j->timer = win_set_timer(t->win, (uint32_t)MAX(10, interval_ms));
    t->input_on = false;
    term_invalidate(t);
}

/* Stop the running job; with `cancelled` (Ctrl+C) the rest of the command
   line is dropped. Either way the shell continues / shows the prompt. */
static void job_end(term_t *t, bool cancelled, bool resume) {
    tjob_t *j = &t->job;
    if (!j->active) return;
    j->active = false;
    if (j->timer) win_kill_timer(t->win, j->timer);
    j->timer = 0;
    if (j->cleanup) j->cleanup(t, cancelled);
    j->data = NULL;
    j->paint = NULL;
    if (!resume) return;
    if (cancelled) {
        term_puts(t, T_RESET "^C\n");
        t->status = 130;
        free_queue(t);
    }
    term_shell_resume(t);
    if (t->input_on) blink_reset(t);
}

void term_job_stop(term_t *t, bool cancelled) { job_end(t, cancelled, true); }

void term_request_close(term_t *t) { win_post_event(t->win, TERM_EV_CLOSE); }

/* ---- history (shared file, per-window list) --------------------------------------- */
static void hist_push(term_t *t, const char *line) {
    if (t->hist_n == TERM_HIST_MAX) {
        free(t->hist[0]);
        memmove(t->hist, t->hist + 1, (TERM_HIST_MAX - 1) * sizeof(char *));
        t->hist_n--;
    }
    char *d = strdup(line);
    if (d) t->hist[t->hist_n++] = d;
}

static void hist_load(term_t *t) {
    uint32_t size = 0;
    char *data = vfs_read_file(TERM_HISTFILE, &size);
    if (!data) return;
    char *save = NULL;
    for (char *ln = strtok_r(data, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save))
        if (*ln && strlen(ln) < TERM_INPUT_MAX) hist_push(t, ln);
    free(data);
    t->hist_pos = t->hist_n;
}

void term_history_add(term_t *t, const char *line) {
    t->hist_pos = t->hist_n;
    if (!*line || line[0] == ' ') return;
    if (t->hist_n && !strcmp(t->hist[t->hist_n - 1], line)) return;
    hist_push(t, line);
    t->hist_pos = t->hist_n;
    vfs_stat_t st;
    if (vfs_stat(TERM_HISTFILE, &st) == 0 && st.size > 24 * 1024) {
        /* keep the file bounded: rewrite it from the in-memory list */
        int total = 0;
        for (int i = 0; i < t->hist_n; i++) total += (int)strlen(t->hist[i]) + 1;
        char *buf = malloc((size_t)total + 1);
        if (!buf) return;
        int p = 0;
        for (int i = 0; i < t->hist_n; i++) {
            int l = (int)strlen(t->hist[i]);
            memcpy(buf + p, t->hist[i], (size_t)l);
            p += l;
            buf[p++] = '\n';
        }
        vfs_write_file(TERM_HISTFILE, buf, (uint32_t)p);
        free(buf);
    } else {
        char buf[TERM_INPUT_MAX + 2];
        int n = snprintf(buf, sizeof buf, "%s\n", line);
        vfs_append_file(TERM_HISTFILE, buf, (uint32_t)MIN(n, (int)sizeof buf - 1));
    }
}

void term_history_clear(term_t *t) {
    for (int i = 0; i < t->hist_n; i++) free(t->hist[i]);
    t->hist_n = t->hist_pos = 0;
    vfs_write_file(TERM_HISTFILE, "", 0);
}

/* ---- command line editing ---------------------------------------------------------- */
static void blink_reset(term_t *t) {
    t->blink_on = true;
    if (t->blink_timer) win_kill_timer(t->win, t->blink_timer);
    t->blink_timer = win_set_timer(t->win, 530);
}

static void input_set(term_t *t, const char *s) {
    int n = (int)strlen(s);
    if (n > TERM_INPUT_MAX - 1) n = TERM_INPUT_MAX - 1;
    memcpy(t->in, s, (size_t)n);
    t->in[n] = 0;
    t->in_len = t->in_cur = n;
}

static void input_insert(term_t *t, const char *s, int n) {
    if (n > TERM_INPUT_MAX - 1 - t->in_len) n = TERM_INPUT_MAX - 1 - t->in_len;
    if (n <= 0) { sys_beep(440, 30); return; }
    memmove(t->in + t->in_cur + n, t->in + t->in_cur, (size_t)(t->in_len - t->in_cur));
    memcpy(t->in + t->in_cur, s, (size_t)n);
    t->in_len += n;
    t->in_cur += n;
    t->in[t->in_len] = 0;
}

static void input_delete(term_t *t, int from, int to) {
    from = CLAMP(from, 0, t->in_len);
    to = CLAMP(to, 0, t->in_len);
    if (to <= from) return;
    memmove(t->in + from, t->in + to, (size_t)(t->in_len - to));
    t->in_len -= to - from;
    t->in[t->in_len] = 0;
    if (t->in_cur > to) t->in_cur -= to - from;
    else if (t->in_cur > from) t->in_cur = from;
}

static int word_left(term_t *t, int p) {
    while (p > 0 && t->in[p - 1] == ' ') p--;
    while (p > 0 && t->in[p - 1] != ' ' && t->in[p - 1] != '/') p--;
    return p;
}

static int word_right(term_t *t, int p) {
    while (p < t->in_len && (t->in[p] == ' ' || t->in[p] == '/')) p++;
    while (p < t->in_len && t->in[p] != ' ' && t->in[p] != '/') p++;
    return p;
}

static void paste_text(term_t *t, const char *text) {
    if (!t->input_on || !text) return;
    /* multi-line text becomes one command line: lines joined with "; ",
       blank lines and the indentation of continuation lines dropped */
    char buf[TERM_INPUT_MAX];
    int n = 0;
    bool sep = false;
    for (const char *p = text; *p && n < TERM_INPUT_MAX - 3; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r') continue;
        if (c == '\n') { if (n > 0) sep = true; continue; }
        if (c == '\t') c = ' ';
        if (c < 0x20) continue;
        if (sep) {
            if (c == ' ') continue;
            while (n > 0 && buf[n - 1] == ' ') n--;
            buf[n++] = ';';
            buf[n++] = ' ';
            sep = false;
        }
        buf[n++] = (char)c;
    }
    input_insert(t, buf, n);
    t->scroll = 0;
    term_invalidate(t);
}

static void hist_show(term_t *t, int pos) {
    if (pos < 0 || pos > t->hist_n) return;
    if (t->hist_pos == t->hist_n && pos != t->hist_n) {
        free(t->hist_saved);
        t->hist_saved = strdup(t->in);
    }
    t->hist_pos = pos;
    if (pos == t->hist_n) input_set(t, t->hist_saved ? t->hist_saved : "");
    else input_set(t, t->hist[pos]);
}

static void term_enter(term_t *t) {
    char *line = strdup(t->in);
    term_write(t, t->in, t->in_len);
    term_write(t, "\n", 1);
    t->in_len = t->in_cur = 0;
    t->in[0] = 0;
    free(t->hist_saved);
    t->hist_saved = NULL;
    t->hist_pos = t->hist_n;
    t->input_on = false;
    t->has_sel = false;
    t->scroll = 0;
    if (line) {
        term_shell_submit(t, line);
        free(line);
    } else {
        term_shell_prompt(t);
    }
}

/* ---- selection / clipboard --------------------------------------------------------- */
static char *sel_text(term_t *t) {
    if (!t->has_sel) return NULL;
    uint32_t l0 = t->sa_line, l1 = t->sb_line;
    int c0 = t->sa_col, c1 = t->sb_col;
    if (l0 > l1 || (l0 == l1 && c0 > c1)) {
        uint32_t tl = l0; l0 = l1; l1 = tl;
        int tc = c0; c0 = c1; c1 = tc;
    }
    int cap = 256, len = 0;
    char *out = malloc((size_t)cap);
    if (!out) return NULL;
    for (uint32_t L = l0; L <= l1; L++) {
        if (L < t->abs_base || L >= t->abs_base + (uint32_t)t->count) continue;
        int idx = (int)(L - t->abs_base);
        int n = disp_len(t, idx);
        int from = L == l0 ? c0 : 0, to = L == l1 ? MIN(c1, n) : n;
        int start = len;
        for (int p = from; p < to; p++) {
            tcell_t c;
            if (!disp_cell(t, idx, p, &c)) break;
            if (len + 2 >= cap) {
                cap *= 2;
                char *nb = realloc(out, (size_t)cap);
                if (!nb) { free(out); return NULL; }
                out = nb;
            }
            out[len++] = (char)c.ch;
        }
        while (len > start && out[len - 1] == ' ') len--;
        if (L != l1) {
            if (len + 2 >= cap) {
                cap *= 2;
                char *nb = realloc(out, (size_t)cap);
                if (!nb) { free(out); return NULL; }
                out = nb;
            }
            out[len++] = '\n';
        }
    }
    out[len] = 0;
    return out;
}

static bool copy_selection(term_t *t) {
    char *s = sel_text(t);
    if (!s) return false;
    bool ok = *s != 0;
    if (ok) clipboard_set(s);
    free(s);
    return ok;
}

static void select_all(term_t *t) {
    t->sa_line = t->abs_base;
    t->sa_col = 0;
    t->sb_line = t->abs_base + (uint32_t)t->count - 1;
    t->sb_col = disp_len(t, t->count - 1);
    t->has_sel = true;
    term_invalidate(t);
}

/* Map a client point to (absolute line, column). */
static void hit_test(term_t *t, int x, int y, uint32_t *line, int *col) {
    term_layout(t);
    int v = (y - PAD_Y) / t->chh;
    if (y < PAD_Y) v = 0;
    v = CLAMP(v, 0, t->rows - 1);
    int c = (x - PAD_X + t->cw / 2) / t->cw;
    if (x < PAD_X) c = 0;
    c = CLAMP(c, 0, t->cols);
    while (v > 0 && t->vrow[v].idx < 0) { v--; c = t->cols; }
    int idx = t->vrow[v].idx;
    if (idx < 0) { *line = t->abs_base; *col = 0; return; }
    int pos = t->vrow[v].off + c;
    int n = disp_len(t, idx);
    if (pos > n) pos = n;
    *line = t->abs_base + (uint32_t)idx;
    *col = pos;
}

static bool word_char(uint8_t c) { return c > ' ' && !strchr("\"'`()[]{}<>|;,", c); }

static void select_word(term_t *t, int x, int y) {
    uint32_t L;
    int col;
    hit_test(t, x, y, &L, &col);
    int idx = (int)(L - t->abs_base);
    if (idx < 0 || idx >= t->count) return;
    int n = disp_len(t, idx), a = col, b = col;
    tcell_t c;
    if (a < n && disp_cell(t, idx, a, &c) && !word_char(c.ch)) return;
    while (a > 0 && disp_cell(t, idx, a - 1, &c) && word_char(c.ch)) a--;
    while (b < n && disp_cell(t, idx, b, &c) && word_char(c.ch)) b++;
    if (b <= a) return;
    t->sa_line = t->sb_line = L;
    t->sa_col = a;
    t->sb_col = b;
    t->has_sel = true;
    copy_selection(t);
    term_invalidate(t);
}

/* ---- zoom ------------------------------------------------------------------------------ */
static void set_font(term_t *t, const font_t *f) {
    if (t->font == f) return;
    t->font = f;
    term_metrics(t);
    term_invalidate(t);
}

/* ---- context menu ------------------------------------------------------------------- */
static term_t *term_from_id(intptr_t id) {
    window_t *w = win_find_by_id((int)id);
    return w ? win_get_userdata(w) : NULL;
}

static void menu_cb(int item, void *ctx) {
    term_t *t = term_from_id((intptr_t)ctx);
    if (!t) return;
    switch (item) {
    case 0: copy_selection(t); break;
    case 1: paste_text(t, clipboard_get()); break;
    case 2: select_all(t); break;
    case 4:
        term_clear(t);
        if (t->input_on) term_shell_prompt(t);
        break;
    case 6: set_font(t, FONT_MONO_2X); break;
    case 7: set_font(t, FONT_MONO); break;
    case 9: app_launch_by_name("Terminal", t->cwd); break;
    case 10: term_request_close(t); break;
    default: break;
    }
}

static void show_menu(term_t *t, int x, int y) {
    char items[256];
    snprintf(items, sizeof items,
             "%sCopy\tCtrl+Shift+C|Paste\tCtrl+Shift+V|Select All|-|Clear\tCtrl+L|-|"
             "%sLarger Text\tCtrl+=|%sSmaller Text\tCtrl+-|-|New Terminal|Close Terminal",
             t->has_sel ? "" : "!", t->font == FONT_MONO_2X ? "!" : "", t->font == FONT_MONO ? "!" : "");
    ui_popup_menu(t->win, x, y, items, menu_cb, (void *)(intptr_t)win_id(t->win));
}

/* ---- keyboard -------------------------------------------------------------------------- */
static void scroll_by(term_t *t, int rows) {
    term_layout(t);
    t->scroll = CLAMP(t->scroll + rows, 0, MAX(0, t->total_rows - t->rows));
    term_invalidate(t);
}

static void term_key(term_t *t, const event_t *ev) {
    int k = ev->key, m = ev->mods;
    bool ctrl = (m & MOD_CTRL) != 0, shift = (m & MOD_SHIFT) != 0;
    t->eat_char = false;

    /* keys that work at any time */
    if (k == KEY_PAGEUP) { scroll_by(t, shift ? 1 : t->rows - 1); return; }
    if (k == KEY_PAGEDOWN) { scroll_by(t, shift ? -1 : -(t->rows - 1)); return; }
    if (shift && !ctrl && k == KEY_UP) { scroll_by(t, 1); return; }
    if (shift && !ctrl && k == KEY_DOWN) { scroll_by(t, -1); return; }
    if (ctrl && k == KEY_HOME) { scroll_by(t, 1 << 20); return; }
    if (ctrl && k == KEY_END) { t->scroll = 0; term_invalidate(t); return; }
    if (ctrl && shift && k == 'c') { copy_selection(t); return; }
    if (ctrl && shift && k == 'v') { paste_text(t, clipboard_get()); return; }
    if (ctrl && (k == '=' || k == '+')) { set_font(t, FONT_MONO_2X); return; }
    if (ctrl && k == '-') { set_font(t, FONT_MONO); return; }
    if (ctrl && k == '0') { set_font(t, FONT_MONO); return; }

    if (t->job.active) {
        /* Ctrl+C / Esc interrupt; any other key simply ends an animation */
        bool overlay = t->job.paint != NULL;
        if ((ctrl && k == 'c') || k == KEY_ESCAPE) term_job_stop(t, true);
        else if (overlay && !ctrl && k != KEY_NONE && (k < KEY_LSHIFT || k > KEY_SCROLLLOCK)) {
            term_job_stop(t, false);
            t->eat_char = true;          /* the key's character must not reach the prompt */
        }
        return;
    }
    if (!t->input_on) return;
    if (k >= KEY_LSHIFT && k <= KEY_SCROLLLOCK) return;     /* bare modifiers */

    if (k != KEY_TAB) t->tab_presses = 0;
    bool edit = true;
    if (ctrl) {
        switch (k) {
        case 'c':
            if (t->has_sel && copy_selection(t)) { t->has_sel = false; term_invalidate(t); return; }
            term_write(t, t->in, t->in_len);
            term_puts(t, T_GRAY "^C" T_RESET "\n");
            t->in_len = t->in_cur = 0;
            t->in[0] = 0;
            t->hist_pos = t->hist_n;
            t->status = 130;
            t->mode = TERM_MODE_SHELL;          /* Ctrl+C also leaves interactive calc */
            term_shell_prompt(t);
            break;
        case 'v': paste_text(t, clipboard_get()); break;
        case 'l':
            term_clear(t);
            term_shell_prompt(t);
            break;
        case 'a': t->in_cur = 0; break;
        case 'e': t->in_cur = t->in_len; break;
        case 'u': input_delete(t, 0, t->in_cur); break;
        case 'k': input_delete(t, t->in_cur, t->in_len); break;
        case 'w': input_delete(t, word_left(t, t->in_cur), t->in_cur); break;
        case 'd':
            if (t->in_len == 0 && t->mode == TERM_MODE_CALC) {
                term_puts(t, "exit\n");
                t->mode = TERM_MODE_SHELL;
                term_shell_prompt(t);
            } else if (t->in_len == 0) {
                term_puts(t, "exit\n");
                term_request_close(t);
            } else {
                input_delete(t, t->in_cur, t->in_cur + 1);
            }
            break;
        case KEY_LEFT: t->in_cur = word_left(t, t->in_cur); break;
        case KEY_RIGHT: t->in_cur = word_right(t, t->in_cur); break;
        case KEY_BACKSPACE: input_delete(t, word_left(t, t->in_cur), t->in_cur); break;
        case KEY_DELETE: input_delete(t, t->in_cur, word_right(t, t->in_cur)); break;
        default: edit = false; break;
        }
    } else {
        switch (k) {
        case KEY_ENTER: term_enter(t); return;
        case KEY_BACKSPACE: if (t->in_cur > 0) input_delete(t, t->in_cur - 1, t->in_cur); break;
        case KEY_DELETE: input_delete(t, t->in_cur, t->in_cur + 1); break;
        case KEY_LEFT: if (t->in_cur > 0) t->in_cur--; break;
        case KEY_RIGHT: if (t->in_cur < t->in_len) t->in_cur++; break;
        case KEY_HOME: t->in_cur = 0; break;
        case KEY_END: t->in_cur = t->in_len; break;
        case KEY_UP: if (t->hist_pos > 0) hist_show(t, t->hist_pos - 1); else sys_beep(440, 20); break;
        case KEY_DOWN: if (t->hist_pos < t->hist_n) hist_show(t, t->hist_pos + 1); break;
        case KEY_ESCAPE: t->in_len = t->in_cur = 0; t->in[0] = 0; t->hist_pos = t->hist_n; break;
        case KEY_TAB:
            term_shell_complete(t);
            t->tab_presses++;
            break;
        default: edit = false; break;
        }
    }
    if (edit) {
        t->scroll = 0;
        t->has_sel = false;
        blink_reset(t);
        term_invalidate(t);
    }
}

static void term_char(term_t *t, int ch) {
    if (t->eat_char) { t->eat_char = false; return; }
    if (!t->input_on || t->job.active) return;
    if (ch < 0x20 || ch > 0xFF || ch == 0x7F) return;
    char c = (char)ch;
    input_insert(t, &c, 1);
    t->tab_presses = 0;
    t->scroll = 0;
    t->has_sel = false;
    blink_reset(t);
    term_invalidate(t);
}

/* ---- mouse ----------------------------------------------------------------------------- */
static void sb_drag_to(term_t *t, int y) {
    rect_t tr = sb_track(t), th = sb_thumb(t);
    int range = t->total_rows - t->rows;
    if (range <= 0 || tr.h - th.h <= 0) return;
    int ty = CLAMP(y - t->sb_grab, tr.y, tr.y + tr.h - th.h);
    int top = (int)((long long)(ty - tr.y) * range / (tr.h - th.h));
    t->scroll = CLAMP(range - top, 0, range);
    term_invalidate(t);
}

static void term_mouse(term_t *t, const event_t *ev) {
    if (t->job.active && t->job.paint) return;
    int w = win_width(t->win);
    switch (ev->type) {
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_RIGHT) { show_menu(t, ev->x, ev->y); return; }
        if (ev->button == MOUSE_MIDDLE) { paste_text(t, clipboard_get()); return; }
        if (ev->button != MOUSE_LEFT) return;
        term_layout(t);
        if (ev->x >= w - SB_W && t->total_rows > t->rows) {
            rect_t th = sb_thumb(t);
            t->sb_drag = true;
            t->sb_grab = rect_contains(th, th.x, ev->y) ? ev->y - th.y : th.h / 2;
            sb_drag_to(t, ev->y);
            return;
        }
        hit_test(t, ev->x, ev->y, &t->sa_line, &t->sa_col);
        t->sb_line = t->sa_line;
        t->sb_col = t->sa_col;
        t->sel_drag = true;
        t->sel_moved = false;
        if (t->has_sel) { t->has_sel = false; term_invalidate(t); }
        break;
    case EV_MOUSE_MOVE: {
        bool hover = t->sb_drag || (ev->x >= w - SB_W && ev->x < w && ev->y >= 0 && !t->sel_drag);
        if (hover != t->sb_hover) {
            t->sb_hover = hover;
            win_set_cursor(t->win, hover ? CURSOR_ARROW : CURSOR_IBEAM);
            term_invalidate(t);
        }
        if (t->sb_drag) { sb_drag_to(t, ev->y); return; }
        if (!t->sel_drag) return;
        if (ev->y < 0) scroll_by(t, 1);
        else if (ev->y >= win_height(t->win)) scroll_by(t, -1);
        hit_test(t, ev->x, ev->y, &t->sb_line, &t->sb_col);
        t->has_sel = t->sb_line != t->sa_line || t->sb_col != t->sa_col;
        if (t->has_sel) t->sel_moved = true;
        term_invalidate(t);
        break;
    }
    case EV_MOUSE_UP:
        if (ev->button != MOUSE_LEFT) return;
        if (t->sb_drag) { t->sb_drag = false; term_invalidate(t); return; }
        if (!t->sel_drag) return;
        t->sel_drag = false;
        if (t->has_sel) {
            copy_selection(t);                   /* copy-on-select, like X11 terminals */
        } else if (!t->sel_moved && t->input_on && t->sa_line == t->abs_base + (uint32_t)t->count - 1) {
            /* a plain click on the command line moves the caret */
            int base = last_line(t)->len;
            if (t->sa_col >= base) { t->in_cur = CLAMP(t->sa_col - base, 0, t->in_len); blink_reset(t); term_invalidate(t); }
        }
        break;
    case EV_DOUBLE_CLICK:
        if (ev->button == MOUSE_LEFT && ev->x < w - SB_W) { t->sel_drag = false; select_word(t, ev->x, ev->y); }
        break;
    case EV_MOUSE_WHEEL:
        scroll_by(t, -3 * ev->wheel);
        break;
    case EV_MOUSE_LEAVE:
        if (t->sb_hover) {
            t->sb_hover = false;
            win_set_cursor(t->win, CURSOR_IBEAM);
            term_invalidate(t);
        }
        break;
    default:
        break;
    }
}

/* ---- window lifecycle ------------------------------------------------------------------- */
static void term_destroy(term_t *t) {
    job_end(t, true, false);
    free_queue(t);
    term_shell_free(t);
    for (int i = 0; i < TERM_MAX_LINES; i++) free(t->lines[i].c);
    for (int i = 0; i < t->hist_n; i++) free(t->hist[i]);
    free(t->hist_saved);
    free(t);
}

static void term_event(window_t *win, const event_t *ev) {
    term_t *t = win_get_userdata(win);
    if (!t) return;
    switch (ev->type) {
    case EV_PAINT:
        term_paint(t);
        break;
    case EV_KEY_DOWN:
        term_key(t, ev);
        break;
    case EV_CHAR:
        term_char(t, ev->ch);
        break;
    case EV_MOUSE_DOWN: case EV_MOUSE_MOVE: case EV_MOUSE_UP:
    case EV_DOUBLE_CLICK: case EV_MOUSE_WHEEL: case EV_MOUSE_LEAVE:
        term_mouse(t, ev);
        break;
    case EV_TIMER:
        if (ev->timer_id == t->job.timer && t->job.active) {
            t->job.frame++;
            if (!t->job.tick || !t->job.tick(t)) term_job_stop(t, false);
            else if (t->job.paint) term_invalidate(t);
        } else if (ev->timer_id == t->blink_timer) {
            t->blink_on = !t->blink_on;
            if (!t->focused) t->blink_on = true;
            if (!rect_empty(t->cursor_rect))
                win_invalidate_rect(win, t->cursor_rect.x, t->cursor_rect.y, t->cursor_rect.w, t->cursor_rect.h);
        }
        break;
    case EV_RESIZE:
        term_metrics(t);
        break;
    case EV_FOCUS:
        t->focused = true;
        blink_reset(t);
        term_invalidate(t);
        break;
    case EV_BLUR:
        t->focused = false;
        t->sel_drag = t->sb_drag = false;
        term_invalidate(t);
        break;
    case EV_USER:
        if (ev->data == TERM_EV_CLOSE) win_close(win);    /* EV_CLOSE frees t */
        break;
    case EV_CLOSE:
        win_set_userdata(win, NULL);
        term_destroy(t);
        break;
    default:
        break;
    }
}

static void term_launch(const char *arg) {
    term_t *t = calloc(1, sizeof *t);
    if (!t) return;
    t->font = FONT_MONO;
    int cw = font_char_width(FONT_MONO, 'M'), ch = font_height(FONT_MONO);
    window_t *win = win_create("Terminal", PAD_X + INIT_COLS * cw + SB_W, PAD_Y * 2 + INIT_ROWS * ch, WIN_RESIZABLE);
    if (!win) { free(t); return; }
    t->win = win;
    t->theme = s_default_theme;
    t->count = 1;
    t->fg = TC_FG;
    t->bg = TC_BG;
    t->focused = win_is_focused(win);
    win_set_userdata(win, t);
    win_set_event_handler(win, term_event);
    win_set_min_size(win, 320, 160);
    win_set_cursor(win, CURSOR_IBEAM);
    term_metrics(t);
    blink_reset(t);
    hist_load(t);
    term_shell_init(t);

    /* argument: a directory to start in, a script to run, or a command line */
    const char *run = NULL;
    if (arg && *arg) {
        char path[VFS_PATH_MAX];
        if (term_resolve(t, arg, path) == 0 && vfs_is_dir(path)) {
            strlcpy(t->oldpwd, t->cwd, sizeof t->oldpwd);
            strlcpy(t->cwd, path, sizeof t->cwd);
        } else {
            run = arg;
        }
    }
    term_shell_banner(t);
    term_shell_prompt(t);
    if (run) {
        char line[TERM_INPUT_MAX];
        if (vfs_exists(run) && !vfs_is_dir(run)) snprintf(line, sizeof line, "sh \"%s\"", run);
        else strlcpy(line, run, sizeof line);
        input_set(t, line);
        term_enter(t);
    }
    klog("terminal: window %d opened", win_id(win));
}

/* Icon: a dark console window with a green prompt chevron and a cursor. */
static void term_icon(surface_t *s, int x, int y, int size) {
    int m = size / 16;
    int bx = x + m, by = y + size * 2 / 16, bw = size - 2 * m, bh = size * 12 / 16;
    int r = size / 8;
    gfx_fill_rounded_rect(s, bx, by, bw, bh, r, RGB(78, 86, 110));
    int bar = size * 3 / 16;
    gfx_fill_rounded_gradient_v(s, bx + 1, by + bar, bw - 2, bh - bar - 1, r - 1, RGB(36, 40, 56), RGB(16, 18, 26));
    if (size >= 28) {
        int dr = MAX(1, size / 40), dy = by + bar / 2;
        gfx_fill_circle_aa(s, bx + r + dr, dy, dr + 0.5, RGB(255, 96, 92));
        gfx_fill_circle_aa(s, bx + r + dr * 4, dy, dr + 0.5, RGB(255, 190, 50));
        gfx_fill_circle_aa(s, bx + r + dr * 7, dy, dr + 0.5, RGB(60, 200, 90));
    }
    int th = MAX(2, size / 12);
    int cx = bx + bw * 2 / 10, cy = by + bar + (bh - bar) / 2;
    int d = (bh - bar) / 4;
    gfx_thick_line(s, cx, cy - d, cx + d, cy, th, RGB(110, 230, 140));
    gfx_thick_line(s, cx + d, cy, cx, cy + d, th, RGB(110, 230, 140));
    gfx_fill_rect(s, cx + d + size / 10, cy + d - th / 2, size / 4, th, RGB(225, 232, 245));
}

REGISTER_APP(.name = "Terminal", .category = CAT_SYSTEM,
             .description = "Command-line shell with colours, history and completion",
             .launch = term_launch, .draw_icon = term_icon, .file_types = "sh");
