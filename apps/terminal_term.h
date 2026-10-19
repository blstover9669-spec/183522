/*
 * terminal_term.h - internal interfaces shared by the Terminal app's files.
 *
 *   terminal_app.c    window, terminal emulator (scrollback, ANSI colours,
 *                     rendering, keyboard/mouse, selection, history, jobs)
 *   terminal_shell.c  "nsh": parser (quotes, pipes, redirection, ; && ||,
 *                     variables, globs, history expansion), completion and
 *                     the file / text / system built-in commands
 *   terminal_calc.c   expression evaluator for `calc`
 *   terminal_fun.c    neofetch, cowsay, fortune, sl, matrix
 *
 * Everything that is not static is prefixed term_ / tc_ so the symbols never
 * clash with other apps linked into the same kernel image.
 */
#ifndef TERMINAL_TERM_H
#define TERMINAL_TERM_H

#include <nova.h>

#define TERM_MAX_LINES  2000          /* scrollback (logical lines)            */
#define TERM_LINE_MAX   4096          /* longest logical line before a wrap    */
#define TERM_INPUT_MAX  1024          /* command line length                   */
#define TERM_HIST_MAX   200           /* remembered commands                   */
#define TERM_MAX_VROWS  192           /* visible rows (enough for 3000 px)     */
#define TERM_MAX_VARS   24            /* shell variables                       */

#define TERM_HOME       "/home/user"
#define TERM_USER       "user"
#define TERM_HOST       "nova"
#define TERM_HISTFILE   "/home/user/.nsh_history"
#define TERM_SHELL_NAME "nsh"
#define TERM_SHELL_VER  "1.0"

/* Colour indices stored in cells: 0-15 = ANSI palette, plus the defaults. */
#define TC_FG 16
#define TC_BG 17

/* Cell attributes. */
#define TA_BOLD  0x1
#define TA_UNDER 0x2
#define TA_DIM   0x4

/* ANSI escape sequences for command output (stripped when not a terminal). */
#define T_RESET   "\x1b[0m"
#define T_BOLD    "\x1b[1m"
#define T_DIM     "\x1b[2m"
#define T_UNDER   "\x1b[4m"
#define T_RED     "\x1b[31m"
#define T_GREEN   "\x1b[32m"
#define T_YELLOW  "\x1b[33m"
#define T_BLUE    "\x1b[34m"
#define T_MAGENTA "\x1b[35m"
#define T_CYAN    "\x1b[36m"
#define T_WHITE   "\x1b[37m"
#define T_GRAY    "\x1b[90m"
#define T_BRED    "\x1b[91m"
#define T_BGREEN  "\x1b[92m"
#define T_BYELLOW "\x1b[93m"
#define T_BBLUE   "\x1b[94m"
#define T_BMAGENTA "\x1b[95m"
#define T_BCYAN   "\x1b[96m"
#define T_BWHITE  "\x1b[97m"

typedef struct { uint8_t ch, fg, bg, attr; } tcell_t;
typedef struct { tcell_t *c; int len, cap; } tline_t;

typedef struct {
    const char *name;
    const char *desc;
    uint32_t pal[18];          /* 16 ANSI colours, default fg, default bg */
    uint32_t cursor;
    uint32_t selection;        /* drawn with alpha over the cells          */
} tpalette_t;

typedef struct term term_t;

/* An asynchronous "job": a command that keeps running over timer events
   (sleep, timer, sl, matrix). While a job is active the prompt is hidden;
   Ctrl+C (or Esc) cancels it. */
typedef struct tjob {
    bool active;
    int timer;                                  /* window timer driving it      */
    bool (*tick)(term_t *t);                    /* return false when finished   */
    void (*paint)(term_t *t, surface_t *s);     /* optional full-window overlay */
    void (*cleanup)(term_t *t, bool cancelled); /* optional: free data etc.     */
    uint32_t start, duration;
    int frame, value;
    void *data;
} tjob_t;

/* A queued command-line segment (see terminal_shell.c). */
typedef struct tseg {
    struct tseg *next;
    int conn;                  /* 0 = always, 1 = run if last ok (&&), 2 = if failed (||) */
    char text[];
} tseg_t;

typedef struct { char name[32]; char value[192]; } tvar_t;

typedef struct { int idx, off; } tvrow_t;

struct term {
    window_t *win;

    /* scrollback: ring of logical lines; wrapping happens when drawing */
    tline_t lines[TERM_MAX_LINES];
    int first, count;
    uint32_t abs_base;          /* absolute number of the line at `first` */
    int out_col;                /* write position in the last line        */
    uint8_t fg, bg, attr;       /* current SGR state                      */
    bool reverse;
    int esc, esc_par[8], esc_np;

    /* view */
    const font_t *font;
    int cw, chh;                /* cell size                              */
    int cols, rows;
    int scroll;                 /* visual rows scrolled up from the bottom */
    int total_rows, top_row;
    tvrow_t vrow[TERM_MAX_VROWS];
    rect_t cursor_rect;         /* last drawn cursor (for blinking)        */

    /* command line being edited (drawn after the prompt on the last line) */
    char in[TERM_INPUT_MAX];
    int in_len, in_cur;
    bool input_on;
    uint8_t in_fg[TERM_INPUT_MAX];
    char *hist[TERM_HIST_MAX];
    int hist_n, hist_pos;
    char *hist_saved;
    int tab_presses;

    /* cursor blink */
    bool blink_on, focused;
    int blink_timer;

    /* selection (absolute line, column) and scrollbar dragging */
    bool sel_drag, has_sel, sel_moved;
    uint32_t sa_line, sb_line;
    int sa_col, sb_col;
    bool sb_drag, sb_hover;
    int sb_grab;

    int theme;

    /* shell state */
    char cwd[VFS_PATH_MAX], oldpwd[VFS_PATH_MAX];
    int status;
    tvar_t vars[TERM_MAX_VARS];
    int nvars;
    tseg_t *queue;
    int seg_budget;
    tjob_t job;
    int mode;                   /* TERM_MODE_SHELL or TERM_MODE_CALC        */
    double calc_ans;            /* last calc result ("ans")                 */
};

#define TERM_MODE_SHELL 0
#define TERM_MODE_CALC  1

/* ---- command I/O ---------------------------------------------------------------
 * Commands write to a tout_t: either the terminal (tty) or a capture buffer
 * (pipes, > and >> redirection). Errors always go to the terminal. */
typedef struct {
    term_t *t;
    bool tty;
    char *buf;
    int len, cap;
    int esc;                    /* escape-stripping state for captures */
} tout_t;

typedef struct {
    term_t *t;
    tout_t *out;
    const char *in;             /* standard input from a pipe or '<' */
    int in_len;
    bool has_in;
} tctx_t;

typedef int (*tcmd_fn)(tctx_t *c, int argc, char **argv);

/* ---- terminal_app.c --------------------------------------------------------- */
void term_write(term_t *t, const char *s, int n);
void term_puts(term_t *t, const char *s);
void term_printf(term_t *t, const char *fmt, ...) PRINTF_LIKE(2, 3);
void term_clear(term_t *t);
void term_invalidate(term_t *t);
int  term_palette_count(void);
const tpalette_t *term_palette(int index);
void term_set_theme(term_t *t, int index);
void term_job_start(term_t *t, int interval_ms, bool (*tick)(term_t *),
                    void (*paint)(term_t *, surface_t *), void (*cleanup)(term_t *, bool));
void term_job_stop(term_t *t, bool cancelled);
void term_request_close(term_t *t);
void term_history_clear(term_t *t);
void term_history_add(term_t *t, const char *line);
int  term_input_line(term_t *t);       /* 1 if the shell is waiting for input */

/* ---- terminal_shell.c ------------------------------------------------------- */
void term_shell_init(term_t *t);
void term_shell_free(term_t *t);
void term_shell_banner(term_t *t);
void term_shell_prompt(term_t *t);
void term_shell_submit(term_t *t, const char *line);
void term_shell_resume(term_t *t);
void term_shell_complete(term_t *t);
bool term_shell_is_command(const char *word, int len);
int  term_resolve(term_t *t, const char *path, char *out);
const char *term_var_get(term_t *t, const char *name);

void tc_write(tctx_t *c, const char *s, int n);
void tc_puts(tctx_t *c, const char *s);
void tc_printf(tctx_t *c, const char *fmt, ...) PRINTF_LIKE(2, 3);
void tc_err(tctx_t *c, const char *fmt, ...) PRINTF_LIKE(2, 3);
bool tc_tty(tctx_t *c);
int  tc_term_cols(tctx_t *c);

/* ---- terminal_calc.c -------------------------------------------------------- */
/* Evaluate an arithmetic expression. Returns 0 and *result, or -1 with a
   message in err. `ans` is the value of the "ans" variable. */
int  term_calc_eval(const char *expr, double ans, double *result, char *err, int errsz);
void term_calc_format(double v, char *out, int size);
int  term_cmd_calc(tctx_t *c, int argc, char **argv);

/* ---- terminal_fun.c --------------------------------------------------------- */
int term_cmd_neofetch(tctx_t *c, int argc, char **argv);
int term_cmd_cowsay(tctx_t *c, int argc, char **argv);
int term_cmd_fortune(tctx_t *c, int argc, char **argv);
int term_cmd_sl(tctx_t *c, int argc, char **argv);
int term_cmd_matrix(tctx_t *c, int argc, char **argv);

#endif
