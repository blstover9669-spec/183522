/*
 * terminal_fun.c - the lighter side of Terminal: neofetch, cowsay, fortune
 * and two animations (sl, matrix) that run as non-blocking terminal jobs:
 * a window timer advances a frame and the job's paint callback draws it over
 * the terminal. Any key (or Ctrl+C) stops them.
 */
#include "terminal_term.h"

#define PAD_X 8
#define PAD_Y 6

/* ======================================================================================
 * neofetch
 * ==================================================================================== */
/* A block-letter "N" with a nova sparkle; 0x8B = full block, 0x8F = upper half,
   0x90 = lower half, 0x99 = star. */
static const char *const s_logo[] = {
    "               \x99",
    "  \x90\x90\x90         \x90\x90\x90",
    "  \x8B\x8B\x8B\x8B\x90       \x8B\x8B\x8B",
    "  \x8B\x8B\x8B\x8B\x8B\x8B\x90     \x8B\x8B\x8B",
    "  \x8B\x8B\x8B \x8F\x8B\x8B\x8B\x90   \x8B\x8B\x8B",
    "  \x8B\x8B\x8B   \x8F\x8B\x8B\x8B\x90 \x8B\x8B\x8B",
    "  \x8B\x8B\x8B     \x8F\x8B\x8B\x8B\x8B\x8B\x8B",
    "  \x8B\x8B\x8B       \x8F\x8B\x8B\x8B\x8B",
    "  \x8F\x8F\x8F         \x8F\x8F\x8F",
    "",
    "  N o v a O S",
};
static const char *const s_logo_color[] = {
    "\x1b[1;93m", "\x1b[1;96m", "\x1b[96m", "\x1b[1;94m", "\x1b[94m",
    "\x1b[1;95m", "\x1b[95m", "\x1b[35m", "\x1b[35m", "", "\x1b[1;97m",
};
#define LOGO_W 20

/* The big coloured "N" (logo rows up to the wordmark), used by the startup banner. */
void term_print_logo(term_t *t) {
    for (int r = 0; r < 9; r++) term_printf(t, "%s%s" T_RESET "\n", s_logo_color[r], s_logo[r]);
}

static void trim_copy(const char *s, char *out, int size) {
    while (*s == ' ') s++;
    strlcpy(out, s, (size_t)size);
    int n = (int)strlen(out);
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
}

int term_cmd_neofetch(tctx_t *c, int argc, char **argv) {
    term_t *t = c->t;
    const theme_t *th = theme_get();
    char info[18][200];
    int ni = 0;
#define KV(key, ...) do { \
        char v_[160]; snprintf(v_, sizeof v_, __VA_ARGS__); \
        snprintf(info[ni++], sizeof info[0], "\x1b[1;96m%s\x1b[0m: %s", key, v_); } while (0)

    snprintf(info[ni++], sizeof info[0], "\x1b[1;96m%s\x1b[0m@\x1b[1;96m%s\x1b[0m", TERM_USER, TERM_HOST);
    snprintf(info[ni++], sizeof info[0], "%s", "---------");
    KV("OS", "%s %s i686", sys_version(), sys_edition());
    KV("Kernel", "%s", sys_kernel_version());
    uint32_t s = sys_uptime_ms() / 1000;
    if (s >= 86400) KV("Uptime", "%u days, %u hours, %u mins", s / 86400, (s / 3600) % 24, (s / 60) % 60);
    else if (s >= 3600) KV("Uptime", "%u hours, %u mins", s / 3600, (s / 60) % 60);
    else KV("Uptime", "%u mins, %u secs", s / 60, s % 60);
    KV("Apps", "%d installed", app_count());
    KV("Shell", "%s %s", TERM_SHELL_NAME, TERM_SHELL_VER);
    KV("Resolution", "%dx%d", screen_width(), screen_height());
    const char *accent = NULL;
    for (int i = 0; i < theme_accent_count(); i++) if (theme_accent_preset(i) == th->accent) accent = theme_accent_name(i);
    char acc[24];
    if (!accent) { snprintf(acc, sizeof acc, "#%06X", th->accent & 0xFFFFFF); accent = acc; }
    KV("Theme", "%s, %s accent", th->dark ? "Dark" : "Light", accent);
    KV("Terminal", "Terminal (%s, %dx%d)", term_palette(t->theme)->name, t->cols, t->rows);
    char cpu[64];
    trim_copy(sys_cpu_name(), cpu, sizeof cpu);
    KV("CPU", "%s", cpu[0] ? cpu : "unknown");
    uint32_t total = sys_mem_total(), fr = sys_mem_free();
    KV("Memory", "%u MiB / %u MiB", (total > fr ? total - fr : 0) >> 20, total >> 20);
    KV("Files", "%u KiB in RAM disk", vfs_total_bytes() >> 10);
    int nw = win_list(NULL, 0);
    KV("Windows", "%d open", nw);
    info[ni++][0] = 0;
    {
        char *p = info[ni++];
        int o = 0;
        for (int i = 0; i < 8; i++) o += snprintf(p + o, sizeof info[0] - (size_t)o, "\x1b[4%dm   ", i);
        snprintf(p + o, sizeof info[0] - (size_t)o, "\x1b[0m");
        p = info[ni++];
        o = 0;
        for (int i = 0; i < 8; i++) o += snprintf(p + o, sizeof info[0] - (size_t)o, "\x1b[10%dm   ", i);
        snprintf(p + o, sizeof info[0] - (size_t)o, "\x1b[0m");
    }
#undef KV
    bool logo = tc_term_cols(c) >= LOGO_W + 44;
    int nl = ARRAY_SIZE(s_logo);
    int rows = logo ? MAX(nl, ni) : ni;
    tc_puts(c, "\n");
    for (int r = 0; r < rows; r++) {
        if (logo) {
            const char *l = r < nl ? s_logo[r] : "";
            tc_printf(c, "%s%s\x1b[0m", r < nl ? s_logo_color[r] : "", l);
            for (int k = (int)strlen(l); k < LOGO_W; k++) tc_write(c, " ", 1);
        }
        if (r < ni) tc_puts(c, info[r]);
        tc_write(c, "\n", 1);
    }
    return 0;
}

/* ======================================================================================
 * cowsay
 * ==================================================================================== */
int term_cmd_cowsay(tctx_t *c, int argc, char **argv) {
    char *text = malloc(4096);
    if (!text) return 1;
    text[0] = 0;
    if (argc > 1) {
        for (int i = 1; i < argc; i++) { if (i > 1) strlcat(text, " ", 4096); strlcat(text, argv[i], 4096); }
    } else if (c->has_in && c->in_len > 0) {
        int n = MIN(c->in_len, 4095);
        memcpy(text, c->in, (size_t)n);
        text[n] = 0;
    } else {
        strlcpy(text, "Moo! Try: fortune | cowsay", 4096);
    }
    /* word-wrap to 40 columns */
    enum { WRAP = 40, MAXL = 64 };
    char lines[MAXL][WRAP + 1];
    int nl = 0, cur = 0;
    lines[0][0] = 0;
    char *save = NULL;
    for (char *w = strtok_r(text, " \t\r\n", &save); w && nl < MAXL; w = strtok_r(NULL, " \t\r\n", &save)) {
        int wl = (int)strlen(w);
        while (wl > 0 && nl < MAXL) {
            if (cur > 0 && cur + 1 + wl > WRAP) { nl++; cur = 0; if (nl >= MAXL) break; lines[nl][0] = 0; }
            if (cur > 0) { lines[nl][cur++] = ' '; lines[nl][cur] = 0; }
            int take = MIN(wl, WRAP - cur);
            memcpy(lines[nl] + cur, w, (size_t)take);
            cur += take;
            lines[nl][cur] = 0;
            w += take;
            wl -= take;
            if (wl > 0) { nl++; cur = 0; if (nl < MAXL) lines[nl][0] = 0; }
        }
    }
    if (cur > 0 || nl == 0) nl++;
    if (nl > MAXL) nl = MAXL;
    int width = 0;
    for (int i = 0; i < nl; i++) width = MAX(width, (int)strlen(lines[i]));
    tc_write(c, " ", 1);
    for (int i = 0; i < width + 2; i++) tc_write(c, "_", 1);
    tc_write(c, "\n", 1);
    for (int i = 0; i < nl; i++) {
        char l = '|', r = '|';
        if (nl == 1) { l = '<'; r = '>'; }
        else if (i == 0) { l = '/'; r = '\\'; }
        else if (i == nl - 1) { l = '\\'; r = '/'; }
        tc_printf(c, "%c \x1b[1m%-*s\x1b[0m %c\n", l, width, lines[i], r);
    }
    tc_write(c, " ", 1);
    for (int i = 0; i < width + 2; i++) tc_write(c, "-", 1);
    tc_write(c, "\n", 1);
    tc_puts(c, "        \\   \x1b[93m^__^\x1b[0m\n"
               "         \\  \x1b[93m(oo)\\_______\x1b[0m\n"
               "            \x1b[93m(__)\\       )\\/\\\x1b[0m\n"
               "                \x1b[93m||----w |\x1b[0m\n"
               "                \x1b[93m||     ||\x1b[0m\n");
    free(text);
    return 0;
}

/* ======================================================================================
 * fortune
 * ==================================================================================== */
static const char *const s_fortunes[] = {
    "There are only two hard things in computer science: cache invalidation,\nnaming things, and off-by-one errors.",
    "Talk is cheap. Show me the code.\n    -- Linus Torvalds",
    "Programs must be written for people to read, and only incidentally\nfor machines to execute.\n    -- Harold Abelson",
    "Simplicity is prerequisite for reliability.\n    -- Edsger W. Dijkstra",
    "The best way to predict the future is to invent it.\n    -- Alan Kay",
    "Any sufficiently advanced technology is indistinguishable from magic.\n    -- Arthur C. Clarke",
    "Premature optimization is the root of all evil.\n    -- Donald Knuth",
    "It works on my machine. And now my machine is NovaOS.",
    "UNIX is basically a simple operating system, but you have to be a genius\nto understand the simplicity.\n    -- Dennis Ritchie",
    "Debugging is twice as hard as writing the code in the first place.\n    -- Brian Kernighan",
    "A ship in port is safe, but that's not what ships are built for.\n    -- Grace Hopper",
    "First, solve the problem. Then, write the code.\n    -- John Johnson",
    "Weeks of coding can save you hours of planning.",
    "There is no place like 127.0.0.1",
    "To understand recursion, you must first understand recursion.",
    "In theory, there is no difference between theory and practice.\nIn practice, there is.",
    "The computer was born to solve problems that did not exist before.\n    -- Bill Gates",
    "Measuring programming progress by lines of code is like measuring\naircraft building progress by weight.\n    -- Bill Gates",
    "One man's crappy software is another man's full-time job.\n    -- Jessica Gaston",
    "Every star you see at night is bigger and brighter than our sun.\nA nova is a star that suddenly becomes much brighter. Be a nova today.",
    "Keep calm and hlt.",
    "640K ought to be enough for anybody. NovaOS politely disagrees.",
    "You will find a missing semicolon in your near future.",
    "Good judgement comes from experience. Experience comes from bad judgement.",
    "The cheapest, fastest and most reliable components are those that\naren't there.\n    -- Gordon Bell",
    "Have you tried turning it off and on again? (Type: reboot)",
};

int term_cmd_fortune(tctx_t *c, int argc, char **argv) {
    int n = ARRAY_SIZE(s_fortunes);
    int i = (int)(((unsigned)rand() ^ sys_uptime_ms() * 2654435761u) % (unsigned)n);
    tc_puts(c, s_fortunes[i]);
    tc_write(c, "\n", 1);
    return 0;
}

/* ======================================================================================
 * Animations (overlay jobs)
 * ==================================================================================== */
/* Draw one string on the cell grid; out-of-range columns are clipped. */
static void grid_text(term_t *t, surface_t *s, int col0, int row, const char *str, uint32_t color) {
    if (row < 0 || row >= t->rows) return;
    for (int i = 0; str[i]; i++) {
        int x = col0 + i;
        if (x < 0 || x >= t->cols || str[i] == ' ') continue;
        gfx_char(s, PAD_X + x * t->cw, PAD_Y + row * t->chh, (unsigned char)str[i], color, t->font);
    }
}

/* --- sl: an original little steam train crosses the window -------------------- */
static const char *const s_smoke[2][3] = {
    { "         (@@)  (   )  (@@)   ( )",
      "     (   )   (@)                ",
      "    (@)                         " },
    { "        (   )  (@@)   (  )  (@@)",
      "      (@@)   (   )              ",
      "    ( )                         " },
};
static const char *const s_train[] = {
    "    _||_                                                       ",
    " ___|  |_______        ______________     ______________      ",
    "|  ____    ___ |______|  []  []  [] |___|  []  []  [] |      ",
    "| |NOVA|  |_|  |      |   EXPRESS   |   |   EXPRESS   |      ",
    "|_|____|_______|======|_____________|===|_____________|      ",
    " (O)~~(O)~~(O)  \\_/     (O)     (O)       (O)     (O)        ",
};
#define TRAIN_W 62
#define TRAIN_H (3 + ARRAY_SIZE(s_train))

static bool sl_tick(term_t *t) {
    t->job.value--;                          /* x position of the train's left edge */
    return t->job.value + TRAIN_W > 0;
}

static void sl_paint(term_t *t, surface_t *s) {
    const tpalette_t *p = term_palette(t->theme);
    int x = t->job.value;
    int top = MAX(0, (t->rows - TRAIN_H) / 2);
    int f = (t->job.frame / 4) % 2;
    for (int r = 0; r < 3; r++) grid_text(t, s, x, top + r, s_smoke[f][r], p->pal[8]);
    static const char spokes[4] = { '|', '/', '-', '\\' };
    char spoke = spokes[(t->job.frame / 2) % 4];
    for (int r = 0; r < ARRAY_SIZE(s_train); r++) {
        char line[TRAIN_W + 8];
        strlcpy(line, s_train[r], sizeof line);
        bool wheels = r == ARRAY_SIZE(s_train) - 1;
        if (wheels) for (char *q = line; *q; q++) if (*q == 'O') *q = spoke;
        uint32_t color = wheels ? p->pal[11] : p->pal[TC_FG];
        grid_text(t, s, x, top + 3 + r, line, color);
        /* highlight the lettering */
        for (int i = 0; line[i]; i++)
            if (isupper((unsigned char)line[i]) && !wheels) {
                int cx = x + i;
                if (cx >= 0 && cx < t->cols && top + 3 + r < t->rows)
                    gfx_char(s, PAD_X + cx * t->cw, PAD_Y + (top + 3 + r) * t->chh, (unsigned char)line[i], p->pal[10], t->font);
            }
    }
    /* rails */
    int rail = top + TRAIN_H;
    if (rail < t->rows)
        for (int cx = 0; cx < t->cols; cx++)
            gfx_char(s, PAD_X + cx * t->cw, PAD_Y + rail * t->chh, (cx + t->job.frame) % 3 ? 0x80 : 0x89, p->pal[8], t->font);
}

int term_cmd_sl(tctx_t *c, int argc, char **argv) {
    if (!tc_tty(c)) { tc_puts(c, "choo choo\n"); return 0; }
    static const note_t whistle[] = { { 660, 90 }, { 0, 50 }, { 660, 90 }, { 0, 50 }, { 880, 260 } };
    term_job_start(c->t, 35, sl_tick, sl_paint, NULL);
    c->t->job.value = c->t->cols;
    sys_play_notes(whistle, ARRAY_SIZE(whistle));
    return 0;
}

/* --- matrix: digital rain ----------------------------------------------------------- */
typedef struct {
    int cols, rows;
    float *head, *speed;
    int *len;
    uint8_t *grid;
} rain_t;

static uint8_t rain_char(void) {
    static const char set[] = "0123456789012345ABCDEFGHIJKLMNOPQRSTUVWXYZ@#$%&*+=<>?:;|";
    return (uint8_t)set[rand_range(0, (int)sizeof set - 2)];
}

static void rain_reset(rain_t *m, int x, bool initial) {
    m->head[x] = initial ? (float)-rand_range(0, m->rows * 2) : (float)-rand_range(0, m->rows / 2);
    m->speed[x] = 0.25f + rand_range(0, 75) / 100.0f;
    m->len[x] = rand_range(4, MAX(5, m->rows - 2));
}

static bool rain_tick(term_t *t) {
    rain_t *m = t->job.data;
    if (!m) return false;
    for (int x = 0; x < m->cols; x++) {
        float before = m->head[x];
        m->head[x] += m->speed[x];
        int hr = (int)m->head[x];
        if ((int)before != hr && hr >= 0 && hr < m->rows) m->grid[hr * m->cols + x] = rain_char();
        if (m->head[x] - m->len[x] > m->rows) rain_reset(m, x, false);
    }
    for (int k = 0; k < m->cols / 3 + 1; k++)
        m->grid[rand_range(0, m->rows * m->cols - 1)] = rain_char();
    return true;
}

static void rain_paint(term_t *t, surface_t *s) {
    rain_t *m = t->job.data;
    gfx_fill_rect(s, 0, 0, s->w, s->h, RGB(0, 6, 2));
    if (!m) return;
    int cols = MIN(m->cols, t->cols), rows = MIN(m->rows, t->rows);
    for (int x = 0; x < cols; x++) {
        int head = (int)m->head[x];
        for (int k = 0; k <= m->len[x]; k++) {
            int y = head - k;
            if (y < 0 || y >= rows) continue;
            uint32_t color;
            if (k == 0) color = RGB(210, 255, 215);
            else {
                int fade = k * 230 / (m->len[x] + 1);
                color = color_mix(RGB(40, 255, 90), RGB(0, 40, 10), fade);
            }
            gfx_char(s, PAD_X + x * t->cw, PAD_Y + y * t->chh, m->grid[y * m->cols + x], color, t->font);
        }
    }
    const char *hint = "press any key to wake up";
    int hl = (int)strlen(hint);
    if (t->job.frame < 80 && t->cols > hl + 4) {
        int x = (t->cols - hl) / 2, y = t->rows - 2;
        gfx_fill_rounded_rect(s, PAD_X + (x - 2) * t->cw, PAD_Y + y * t->chh - 4, (hl + 4) * t->cw, t->chh + 8, 6,
                              RGBA(0, 20, 6, 230));
        gfx_rounded_rect(s, PAD_X + (x - 2) * t->cw, PAD_Y + y * t->chh - 4, (hl + 4) * t->cw, t->chh + 8, 6,
                         RGB(40, 160, 70));
        grid_text(t, s, x, y, hint, RGB(150, 255, 160));
    }
}

static void rain_cleanup(term_t *t, bool cancelled) {
    rain_t *m = t->job.data;
    if (!m) return;
    free(m->head);
    free(m->speed);
    free(m->len);
    free(m->grid);
    free(m);
    t->job.data = NULL;
}

int term_cmd_matrix(tctx_t *c, int argc, char **argv) {
    if (!tc_tty(c)) { tc_puts(c, "wake up, Neo...\n"); return 0; }
    term_t *t = c->t;
    rain_t *m = calloc(1, sizeof *m);
    if (!m) return 1;
    m->cols = t->cols;
    m->rows = t->rows;
    m->head = malloc(sizeof(float) * (size_t)m->cols);
    m->speed = malloc(sizeof(float) * (size_t)m->cols);
    m->len = malloc(sizeof(int) * (size_t)m->cols);
    m->grid = malloc((size_t)(m->cols * m->rows));
    if (!m->head || !m->speed || !m->len || !m->grid) {
        free(m->head); free(m->speed); free(m->len); free(m->grid); free(m);
        tc_err(c, "matrix: out of memory");
        return 1;
    }
    for (int i = 0; i < m->cols * m->rows; i++) m->grid[i] = rain_char();
    for (int x = 0; x < m->cols; x++) rain_reset(m, x, true);
    term_job_start(t, 50, rain_tick, rain_paint, rain_cleanup);
    t->job.data = m;
    return 0;
}
