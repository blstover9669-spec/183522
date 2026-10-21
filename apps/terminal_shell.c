/*
 * terminal_shell.c - "nsh", the NovaOS shell that runs inside Terminal.
 *
 * Command line features:
 *   - quoting ('single', "double" with $VAR expansion) and backslash escapes
 *   - $VAR, ${VAR}, $? (last status), ~ (home), * and ? wildcards
 *   - pipes (a | b), redirection (> file, >> file, < file)
 *   - command lists: a ; b, a && b, a || b, # comments
 *   - history expansion: !!, !N, !-N, !prefix
 *   - unknown commands fall back to launching an app with that name
 *
 * Commands write through tc_* helpers so the same code works for the
 * terminal, pipes and files. Nothing blocks: commands that take time (sleep,
 * timer, sl, matrix) start a terminal "job" driven by a window timer and the
 * rest of the command line is queued until the job ends.
 */
#include "terminal_term.h"
#include <nova/disk.h>

#define MAX_TOKS  256
#define MAX_ARGS  256
#define MAX_PIPE  8
#define CAPTURE_LIMIT (8u << 20)      /* biggest pipe / redirect buffer */

/* ======================================================================================
 * Output helpers
 * ==================================================================================== */
static void cap_append(tout_t *o, const char *s, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char b = (unsigned char)s[i];
        /* strip ANSI escape sequences: files and pipes get plain text */
        if (o->esc == 1) { o->esc = b == '[' ? 2 : 0; continue; }
        if (o->esc == 2) { if (b >= 0x40 && b <= 0x7E) o->esc = 0; continue; }
        if (b == 0x1B) { o->esc = 1; continue; }
        if (o->len + 2 > o->cap) {
            if ((uint32_t)o->cap >= CAPTURE_LIMIT) return;
            int nc = o->cap ? o->cap * 2 : 1024;
            char *nb = realloc(o->buf, (size_t)nc);
            if (!nb) return;
            o->buf = nb;
            o->cap = nc;
        }
        o->buf[o->len++] = (char)b;
        o->buf[o->len] = 0;
    }
}

void tc_write(tctx_t *c, const char *s, int n) {
    if (n <= 0) return;
    if (c->out->tty) term_write(c->t, s, n);
    else cap_append(c->out, s, n);
}

void tc_puts(tctx_t *c, const char *s) { tc_write(c, s, (int)strlen(s)); }

void tc_printf(tctx_t *c, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    tc_write(c, buf, n);
}

/* Errors always go to the terminal (there is no separate stderr to redirect). */
void tc_err(tctx_t *c, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    term_puts(c->t, T_BRED);
    term_puts(c->t, buf);
    term_puts(c->t, T_RESET "\n");
}

bool tc_tty(tctx_t *c) { return c->out->tty; }
int  tc_term_cols(tctx_t *c) { return c->out->tty ? c->t->cols : 80; }

/* Colour escape only when writing to the terminal. */
static const char *col(tctx_t *c, const char *esc) { return c->out->tty ? esc : ""; }

/* ======================================================================================
 * Paths and variables
 * ==================================================================================== */
int term_resolve(term_t *t, const char *path, char *out) {
    char tmp[VFS_PATH_MAX];
    if (!path || !*path) return VFS_EINVAL;
    if (path[0] == '~' && (path[1] == 0 || path[1] == '/')) {
        snprintf(tmp, sizeof tmp, "%s%s", TERM_HOME, path + 1);
        path = tmp;
    }
    if (strlen(path) >= VFS_PATH_MAX) return VFS_EINVAL;
    return vfs_normalize(t->cwd, path, out);
}

/* "/home/user/Documents" -> "~/Documents" */
static void pretty_path(const char *p, char *out, int size) {
    int hl = (int)strlen(TERM_HOME);
    if (!strncmp(p, TERM_HOME, (size_t)hl) && (p[hl] == 0 || p[hl] == '/'))
        snprintf(out, (size_t)size, "~%s", p + hl);
    else
        strlcpy(out, p, (size_t)size);
}

static bool valid_var_name(const char *s, int n) {
    if (n <= 0 || n >= 32 || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (int i = 1; i < n; i++) if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return false;
    return true;
}

const char *term_var_get(term_t *t, const char *name) {
    static char buf[64];
    if (!strcmp(name, "?")) { snprintf(buf, sizeof buf, "%d", t->status); return buf; }
    if (!strcmp(name, "PWD")) return t->cwd;
    if (!strcmp(name, "OLDPWD")) return t->oldpwd;
    if (!strcmp(name, "RANDOM")) { snprintf(buf, sizeof buf, "%d", (int)((unsigned)(rand() ^ sys_uptime_ms()) % 32768)); return buf; }
    if (!strcmp(name, "COLUMNS")) { snprintf(buf, sizeof buf, "%d", t->cols); return buf; }
    if (!strcmp(name, "LINES")) { snprintf(buf, sizeof buf, "%d", t->rows); return buf; }
    if (!strcmp(name, "$")) { snprintf(buf, sizeof buf, "%d", win_id(t->win)); return buf; }
    for (int i = 0; i < t->nvars; i++) if (!strcmp(t->vars[i].name, name)) return t->vars[i].value;
    return NULL;
}

static int var_set(term_t *t, const char *name, const char *value) {
    if (!strcmp(name, "PWD") || !strcmp(name, "OLDPWD") || !strcmp(name, "RANDOM")) return -1;
    for (int i = 0; i < t->nvars; i++)
        if (!strcmp(t->vars[i].name, name)) { strlcpy(t->vars[i].value, value, sizeof t->vars[i].value); return 0; }
    if (t->nvars >= TERM_MAX_VARS) return -1;
    strlcpy(t->vars[t->nvars].name, name, sizeof t->vars[0].name);
    strlcpy(t->vars[t->nvars].value, value, sizeof t->vars[0].value);
    t->nvars++;
    return 0;
}

static void var_unset(term_t *t, const char *name) {
    for (int i = 0; i < t->nvars; i++)
        if (!strcmp(t->vars[i].name, name)) {
            memmove(&t->vars[i], &t->vars[i + 1], (size_t)(t->nvars - i - 1) * sizeof t->vars[0]);
            t->nvars--;
            return;
        }
}

void term_shell_init(term_t *t) {
    strlcpy(t->cwd, TERM_HOME, sizeof t->cwd);
    strlcpy(t->oldpwd, TERM_HOME, sizeof t->oldpwd);
    var_set(t, "HOME", TERM_HOME);
    var_set(t, "USER", TERM_USER);
    var_set(t, "HOSTNAME", TERM_HOST);
    var_set(t, "SHELL", "/system/" TERM_SHELL_NAME);
    var_set(t, "TERM", "nova-256color");
    var_set(t, "OS", sys_version());
    t->status = 0;
}

void term_shell_free(term_t *t) { (void)t; }

/* ======================================================================================
 * Small utilities
 * ==================================================================================== */
static bool wild_match(const char *p, const char *s, bool icase) {
    const char *star = NULL, *ss = NULL;
    while (*s) {
        char a = *p, b = *s;
        if (icase) { a = (char)tolower((unsigned char)a); b = (char)tolower((unsigned char)b); }
        if (*p == '?' || (*p != '*' && a == b)) { p++; s++; }
        else if (*p == '*') { star = p++; ss = s; }
        else if (star) { p = star + 1; s = ++ss; }
        else return false;
    }
    while (*p == '*') p++;
    return !*p;
}

static int str_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Human readable size: 512, 1.2K, 3.4M */
static void human_size(uint32_t n, char *out, int size) {
    if (n < 1024) snprintf(out, (size_t)size, "%u", n);
    else if (n < 1024 * 1024) snprintf(out, (size_t)size, "%.1fK", n / 1024.0);
    else snprintf(out, (size_t)size, "%.1fM", n / (1024.0 * 1024.0));
}

static bool parse_int(const char *s, int *out) {
    if (!s || !*s) return false;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end) return false;
    *out = (int)v;
    return true;
}

/* Read a file (or report why not). */
static char *read_text(tctx_t *c, const char *cmd, const char *arg, uint32_t *size) {
    char path[VFS_PATH_MAX];
    if (term_resolve(c->t, arg, path)) { tc_err(c, "%s: %s: invalid path", cmd, arg); return NULL; }
    if (vfs_is_dir(path)) { tc_err(c, "%s: %s: Is a directory", cmd, arg); return NULL; }
    char *data = vfs_read_file(path, size);
    if (!data) tc_err(c, "%s: %s: No such file or directory", cmd, arg);
    return data;
}

static bool is_binary(const char *d, uint32_t n) {
    uint32_t lim = MIN(n, 4096u);
    for (uint32_t i = 0; i < lim; i++) if (d[i] == 0) return true;
    return false;
}

/* Option parsing helper: returns index of first non-option argument and
   fills a set of seen single-letter flags. Unknown flags are reported. */
static int get_flags(tctx_t *c, int argc, char **argv, const char *allowed, bool *flags) {
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "--")) { i++; break; }
        if (isdigit((unsigned char)a[1])) break;        /* negative numbers / -N */
        for (const char *p = a + 1; *p; p++) {
            const char *f = strchr(allowed, *p);
            if (!f) { tc_err(c, "%s: unknown option -%c", argv[0], *p); return -1; }
            flags[(unsigned char)*p] = true;
        }
    }
    return i;
}

/* Iterate over the lines of a buffer. */
typedef struct { const char *p, *end; } liter_t;
static bool next_line(liter_t *it, const char **ls, int *ln) {
    if (it->p >= it->end) return false;
    const char *s = it->p, *e = memchr(s, '\n', (size_t)(it->end - s));
    if (!e) e = it->end;
    *ls = s;
    *ln = (int)(e - s);
    if (*ln > 0 && s[*ln - 1] == '\r') (*ln)--;
    it->p = e < it->end ? e + 1 : e;
    return true;
}

/* Input source for filters (cat/grep/wc/head/tail/sort): each file argument,
   or standard input when there are none. */
typedef int (*filter_fn)(tctx_t *c, const char *name, const char *data, uint32_t len, void *ctx);
static int for_each_input(tctx_t *c, int argc, char **argv, int first, filter_fn fn, void *ctx) {
    int status = 0;
    if (first >= argc) {
        if (!c->has_in) { tc_err(c, "%s: no input (give a file name or pipe something in)", argv[0]); return 1; }
        return fn(c, NULL, c->in ? c->in : "", (uint32_t)c->in_len, ctx);
    }
    for (int i = first; i < argc; i++) {
        uint32_t size;
        char *data = read_text(c, argv[0], argv[i], &size);
        if (!data) { status = 1; continue; }
        if (fn(c, argv[i], data, size, ctx)) status = 1;
        free(data);
    }
    return status;
}

/* ======================================================================================
 * Command table (defined at the bottom)
 * ==================================================================================== */
enum { GRP_FILES, GRP_TEXT, GRP_SYSTEM, GRP_SHELL, GRP_FUN, GRP_COUNT };
static const char *const s_group_names[GRP_COUNT] = { "Files", "Text", "System", "Shell", "Fun" };

typedef struct {
    const char *name;
    tcmd_fn fn;
    int group;
    const char *usage;
    const char *help;
} tcmd_t;

/* the table itself is at the end of the file */
static int cmd_count(void);
static const tcmd_t *cmd_get(int i);
static const tcmd_t *find_cmd(const char *name);

/* Strip quotes/backslashes from a raw word (for highlighting / completion). */
static void unquote(const char *s, int n, char *out, int size) {
    int o = 0;
    char q = 0;
    for (int i = 0; i < n && o < size - 1; i++) {
        char ch = s[i];
        if (q) { if (ch == q) q = 0; else out[o++] = ch; continue; }
        if (ch == '\\' && i + 1 < n) { out[o++] = s[++i]; continue; }
        if (ch == '"' || ch == '\'') { q = ch; continue; }
        out[o++] = ch;
    }
    out[o] = 0;
}

bool term_shell_is_command(const char *word, int len) {
    char w[128];
    if (len <= 0 || len >= (int)sizeof w) return false;
    unquote(word, len, w, sizeof w);
    if (!*w) return false;
    if (find_cmd(w) || command_find(w)) return true;
    const char *eq = strchr(w, '=');
    if (eq && valid_var_name(w, (int)(eq - w))) return true;
    if (w[0] == '!') return true;
    return app_find(w) != NULL;
}

/* Edit distance (for "did you mean"). */
static int edit_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 31 || lb > 31) return 99;
    int d[32][32];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            d[i][j] = MIN(MIN(d[i - 1][j] + 1, d[i][j - 1] + 1), d[i - 1][j - 1] + cost);
        }
    return d[la][lb];
}

/* ======================================================================================
 * Tokenizer
 * ==================================================================================== */
enum { OP_NONE, OP_PIPE, OP_OUT, OP_APPEND, OP_IN };
typedef struct { char *s; bool glob; int op; } tok_t;

typedef struct { char *b; int n, cap; } sbuf_t;
static void sb_putc(sbuf_t *s, char ch) { if (s->n < s->cap - 1) s->b[s->n++] = ch; }
static void sb_puts(sbuf_t *s, const char *str) { while (*str) sb_putc(s, *str++); }

/* Expand $NAME / ${NAME} / $? at line[*i] (which is '$'). */
static void expand_var(term_t *t, const char *line, int *i, sbuf_t *out) {
    int j = *i + 1;
    char name[32];
    int n = 0;
    if (line[j] == '{') {
        j++;
        while (line[j] && line[j] != '}' && n < 31) name[n++] = line[j++];
        if (line[j] == '}') j++;
    } else if (line[j] == '?' || line[j] == '$') {
        name[n++] = line[j++];
    } else {
        while ((isalnum((unsigned char)line[j]) || line[j] == '_') && n < 31) name[n++] = line[j++];
    }
    name[n] = 0;
    if (!n) { sb_putc(out, '$'); *i += 1; return; }
    const char *v = term_var_get(t, name);
    if (v) sb_puts(out, v);
    *i = j;
}

static bool is_op_char(char ch) { return ch == '|' || ch == '>' || ch == '<'; }

static int tokenize(term_t *t, const char *line, tok_t *toks, int max, char *err, int errsz) {
    int n = 0, i = 0;
    char *wbuf = malloc(TERM_INPUT_MAX * 4);
    if (!wbuf) { snprintf(err, (size_t)errsz, "out of memory"); return -1; }
    while (line[i]) {
        while (line[i] == ' ' || line[i] == '\t') i++;
        if (!line[i]) break;
        if (n >= max) { snprintf(err, (size_t)errsz, "too many words"); goto fail; }
        if (is_op_char(line[i])) {
            int op = OP_PIPE;
            if (line[i] == '>') { op = OP_OUT; if (line[i + 1] == '>') { op = OP_APPEND; i++; } }
            else if (line[i] == '<') op = OP_IN;
            i++;
            toks[n++] = (tok_t){ NULL, false, op };
            continue;
        }
        sbuf_t w = { wbuf, 0, TERM_INPUT_MAX * 4 };
        bool glob = false;
        /* ~ at the start of a word */
        if (line[i] == '~' && (line[i + 1] == 0 || line[i + 1] == '/' || line[i + 1] == ' ' || is_op_char(line[i + 1]))) {
            sb_puts(&w, TERM_HOME);
            i++;
        }
        while (line[i] && line[i] != ' ' && line[i] != '\t' && !is_op_char(line[i])) {
            char ch = line[i];
            if (ch == '\\') {
                if (line[i + 1]) sb_putc(&w, line[i + 1]), i += 2;
                else i++;
            } else if (ch == '\'') {
                i++;
                while (line[i] && line[i] != '\'') sb_putc(&w, line[i++]);
                if (!line[i]) { snprintf(err, (size_t)errsz, "missing closing '"); goto fail; }
                i++;
            } else if (ch == '"') {
                i++;
                while (line[i] && line[i] != '"') {
                    if (line[i] == '\\' && line[i + 1] && strchr("\"\\$`", line[i + 1])) { sb_putc(&w, line[i + 1]); i += 2; }
                    else if (line[i] == '$') expand_var(t, line, &i, &w);
                    else sb_putc(&w, line[i++]);
                }
                if (!line[i]) { snprintf(err, (size_t)errsz, "missing closing \""); goto fail; }
                i++;
            } else if (ch == '$') {
                expand_var(t, line, &i, &w);
            } else {
                if (ch == '*' || ch == '?') glob = true;
                sb_putc(&w, ch);
                i++;
            }
        }
        w.b[w.n] = 0;
        char *d = strdup(w.b);
        if (!d) { snprintf(err, (size_t)errsz, "out of memory"); goto fail; }
        toks[n++] = (tok_t){ d, glob, OP_NONE };
    }
    free(wbuf);
    return n;
fail:
    free(wbuf);
    for (int k = 0; k < n; k++) free(toks[k].s);
    return -1;
}

/* Expand a wildcard word into argv (sorted); keeps the word if nothing matches. */
static void glob_expand(term_t *t, const char *pat, char **argv, int *argc, int max) {
    const char *slash = strrchr(pat, '/');
    char dirpart[VFS_PATH_MAX] = "", dir[VFS_PATH_MAX];
    const char *base = pat;
    if (slash) {
        int dl = (int)(slash - pat) + 1;
        if (dl >= VFS_PATH_MAX) dl = VFS_PATH_MAX - 1;
        memcpy(dirpart, pat, (size_t)dl);
        dirpart[dl] = 0;
        base = slash + 1;
    }
    bool dir_has_glob = strpbrk(dirpart, "*?") != NULL;
    int start = *argc;
    if (!dir_has_glob && *base && term_resolve(t, dirpart[0] ? dirpart : ".", dir) == 0 && vfs_is_dir(dir)) {
        int total = vfs_list(dir, NULL, 0);
        if (total > 0) {
            vfs_dirent_t *ents = malloc((size_t)total * sizeof *ents);
            if (ents) {
                total = MIN(total, vfs_list(dir, ents, total));
                for (int i = 0; i < total && *argc < max; i++) {
                    if (ents[i].name[0] == '.' && base[0] != '.') continue;
                    if (!wild_match(base, ents[i].name, false)) continue;
                    char full[VFS_PATH_MAX];
                    snprintf(full, sizeof full, "%s%s", dirpart, ents[i].name);
                    char *d = strdup(full);
                    if (d) argv[(*argc)++] = d;
                }
                free(ents);
            }
        }
    }
    if (*argc > start) qsort(argv + start, (size_t)(*argc - start), sizeof(char *), str_cmp);
    else if (*argc < max) {
        char *d = strdup(pat);
        if (d) argv[(*argc)++] = d;
    }
}

/* ======================================================================================
 * Command lists: ;  &&  ||  and history expansion
 * ==================================================================================== */
static tseg_t *seg_new(const char *s, int n, int conn) {
    while (n > 0 && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (n <= 0) return NULL;
    tseg_t *g = malloc(sizeof *g + (size_t)n + 1);
    if (!g) return NULL;
    g->next = NULL;
    g->conn = conn;
    memcpy(g->text, s, (size_t)n);
    g->text[n] = 0;
    return g;
}

/* Split a line into segments; returns the list (NULL if empty). */
static tseg_t *split_segments(const char *line) {
    tseg_t *head = NULL, **tail = &head;
    int start = 0, conn = 0, i = 0;
    char q = 0;
    for (;; i++) {
        char ch = line[i];
        if (ch && q) { if (ch == '\\' && q == '"' && line[i + 1]) i++; else if (ch == q) q = 0; continue; }
        if (ch == '\\' && line[i + 1]) { i++; continue; }
        if (ch == '\'' || ch == '"') { q = ch; continue; }
        bool comment = ch == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t' || line[i - 1] == ';');
        int seplen = 0, nconn = 0;
        if (!ch || comment) seplen = -1;
        else if (ch == ';') { seplen = 1; nconn = 0; }
        else if (ch == '&' && line[i + 1] == '&') { seplen = 2; nconn = 1; }
        else if (ch == '|' && line[i + 1] == '|') { seplen = 2; nconn = 2; }
        else if (ch == '&') { seplen = 1; nconn = 0; }
        if (!seplen) continue;
        tseg_t *g = seg_new(line + start, i - start, conn);
        if (g) { *tail = g; tail = &g->next; }
        if (seplen < 0) break;
        conn = nconn;
        i += seplen - 1;
        start = i + 1;
    }
    return head;
}

/* !! !N !-N !prefix. Returns false if an event was not found. */
static bool hist_expand(term_t *t, const char *in, char *out, int size, bool *changed, char *bad, int badsz) {
    int o = 0;
    char q = 0;
    *changed = false;
    for (int i = 0; in[i]; i++) {
        char ch = in[i];
        if (ch == '\'' && q != '"') q = q ? 0 : '\'';
        else if (ch == '"' && q != '\'') q = q ? 0 : '"';
        if (ch == '!' && q != '\'' && in[i + 1] && !strchr(" \t=(\"", in[i + 1])) {
            const char *rep = NULL;
            int j = i + 1;
            if (in[j] == '!') { j++; if (t->hist_n) rep = t->hist[t->hist_n - 1]; }
            else if (isdigit((unsigned char)in[j]) || (in[j] == '-' && isdigit((unsigned char)in[j + 1]))) {
                bool neg = in[j] == '-';
                if (neg) j++;
                int v = 0;
                while (isdigit((unsigned char)in[j])) v = v * 10 + (in[j++] - '0');
                int idx = neg ? t->hist_n - v : v - 1;
                if (idx >= 0 && idx < t->hist_n) rep = t->hist[idx];
            } else {
                int k = j;
                while (in[k] && !strchr(" \t;|&<>", in[k])) k++;
                for (int h = t->hist_n - 1; h >= 0; h--)
                    if (!strncmp(t->hist[h], in + j, (size_t)(k - j))) { rep = t->hist[h]; break; }
                j = k;
            }
            if (!rep) {
                int bl = MIN(j - i, badsz - 1);
                memcpy(bad, in + i, (size_t)bl);
                bad[bl] = 0;
                return false;
            }
            for (const char *r = rep; *r && o < size - 1; r++) out[o++] = *r;
            *changed = true;
            i = j - 1;
            continue;
        }
        if (o < size - 1) out[o++] = ch;
    }
    out[o] = 0;
    return true;
}

/* ======================================================================================
 * Executing a segment: pipeline of commands with redirections
 * ==================================================================================== */
typedef struct {
    char *argv[MAX_ARGS + 1];
    int argc;
    char *in_file, *out_file;
    bool append;
} pcmd_t;

static int launch_app(tctx_t *c, const app_desc_t *app, int argc, char **argv, int first);

/* System commands registered with REGISTER_COMMAND (service, notify, alarm, logs,
   assoc, ...; see <nova/service.h>) run when no shell built-in has the name.
   Their output goes through the normal tctx output, so pipes and > work. */
static void reg_write(cmd_writer_t *w, const char *s, int n) { tc_write((tctx_t *)w->ctx, s, n); }

static int run_registered(tctx_t *c, const command_desc_t *rc, int argc, char **argv) {
    (void)rc;
    cmd_writer_t w = { reg_write, c, tc_tty(c) ? CMD_ANSI : 0, c->t->cwd, tc_term_cols(c) };
    return command_exec(argc, argv, &w);
}

static int run_command(tctx_t *c, int argc, char **argv) {
    const tcmd_t *cmd = find_cmd(argv[0]);
    if (cmd) return cmd->fn(c, argc, argv);
    const command_desc_t *rc = command_find(argv[0]);
    if (rc) return run_registered(c, rc, argc, argv);
    const app_desc_t *app = app_find(argv[0]);
    if (app) return launch_app(c, app, argc, argv, 1);
    /* a script given by path */
    if (strchr(argv[0], '/')) {
        char path[VFS_PATH_MAX];
        if (term_resolve(c->t, argv[0], path) == 0 && vfs_exists(path) && !vfs_is_dir(path)) {
            char *sargv[2] = { "sh", argv[0] };
            return find_cmd("sh")->fn(c, 2, sargv);
        }
    }
    const char *best = NULL;
    int bd = 3;
    for (int i = 0; i < cmd_count(); i++) {
        int d = edit_distance(argv[0], cmd_get(i)->name);
        if (d < bd) { bd = d; best = cmd_get(i)->name; }
    }
    if (best && strlen(argv[0]) > 1)
        tc_err(c, "nsh: command not found: %s " T_RESET "(did you mean " T_BOLD "%s" T_RESET "?)", argv[0], best);
    else
        tc_err(c, "nsh: command not found: %s " T_RESET "(type " T_BOLD "help" T_RESET " for a list)", argv[0]);
    return 127;
}

static void run_segment(term_t *t, const char *text) {
    tok_t *toks = calloc(MAX_TOKS, sizeof *toks);
    pcmd_t *cmds = calloc(MAX_PIPE, sizeof *cmds);
    char err[96];
    if (!toks || !cmds) { term_puts(t, T_BRED "nsh: out of memory" T_RESET "\n"); free(toks); free(cmds); return; }
    int nt = tokenize(t, text, toks, MAX_TOKS, err, sizeof err);
    if (nt < 0) {
        term_printf(t, T_BRED "nsh: syntax error: %s" T_RESET "\n", err);
        t->status = 2;
        free(toks);
        free(cmds);
        return;
    }
    int nc = 1;
    bool syntax = false;
    for (int i = 0; i < nt && !syntax; i++) {
        pcmd_t *pc = &cmds[nc - 1];
        tok_t *k = &toks[i];
        if (k->op == OP_PIPE) {
            if (pc->argc == 0 || nc >= MAX_PIPE) { syntax = true; break; }
            nc++;
        } else if (k->op != OP_NONE) {
            if (i + 1 >= nt || toks[i + 1].op != OP_NONE) { syntax = true; break; }
            char *f = toks[++i].s;
            toks[i].s = NULL;
            if (k->op == OP_IN) { free(pc->in_file); pc->in_file = f; }
            else { free(pc->out_file); pc->out_file = f; pc->append = k->op == OP_APPEND; }
        } else if (pc->argc < MAX_ARGS) {
            /* calc gets its arguments verbatim: "calc 2 * 3" must not glob */
            bool noglob = pc->argc > 0 && !strcmp(pc->argv[0], "calc");
            if (k->glob && !noglob) {
                glob_expand(t, k->s, pc->argv, &pc->argc, MAX_ARGS);
                free(k->s);
            } else {
                pc->argv[pc->argc++] = k->s;
            }
            k->s = NULL;
        }
    }
    if (!syntax && cmds[nc - 1].argc == 0 && !(nc == 1 && cmds[0].out_file)) syntax = nt > 0;

    if (syntax) {
        term_puts(t, T_BRED "nsh: syntax error near '|', '<' or '>'" T_RESET "\n");
        t->status = 2;
    } else if (nc == 1 && cmds[0].argc == 0 && cmds[0].out_file) {
        /* "> file" truncates / creates the file */
        char path[VFS_PATH_MAX];
        if (term_resolve(t, cmds[0].out_file, path) || vfs_write_file(path, "", 0)) {
            term_printf(t, T_BRED "nsh: %s: cannot create file" T_RESET "\n", cmds[0].out_file);
            t->status = 1;
        } else t->status = 0;
    } else if (nc == 1 && cmds[0].argc == 1 && strchr(cmds[0].argv[0], '=') && !cmds[0].out_file &&
               valid_var_name(cmds[0].argv[0], (int)(strchr(cmds[0].argv[0], '=') - cmds[0].argv[0]))) {
        /* NAME=value */
        char *eq = strchr(cmds[0].argv[0], '=');
        *eq = 0;
        t->status = var_set(t, cmds[0].argv[0], eq + 1) ? 1 : 0;
        if (t->status) term_printf(t, T_BRED "nsh: cannot set %s" T_RESET "\n", cmds[0].argv[0]);
    } else {
        char *pipe_data = NULL;
        int pipe_len = 0;
        int status = 0;
        for (int i = 0; i < nc; i++) {
            pcmd_t *pc = &cmds[i];
            pc->argv[pc->argc] = NULL;
            tout_t out = { t, true, NULL, 0, 0, 0 };
            tctx_t c = { t, &out, NULL, 0, false };
            char *in_data = NULL;
            if (pc->in_file) {
                char path[VFS_PATH_MAX];
                uint32_t sz = 0;
                if (term_resolve(t, pc->in_file, path) == 0 && !vfs_is_dir(path)) in_data = vfs_read_file(path, &sz);
                if (!in_data) {
                    term_printf(t, T_BRED "nsh: %s: No such file or directory" T_RESET "\n", pc->in_file);
                    status = 1;
                    break;
                }
                c.in = in_data;
                c.in_len = (int)sz;
                c.has_in = true;
            } else if (i > 0) {
                c.in = pipe_data ? pipe_data : "";
                c.in_len = pipe_len;
                c.has_in = true;
            }
            bool capture = pc->out_file || i < nc - 1;
            out.tty = !capture;
            status = run_command(&c, pc->argc, pc->argv);
            free(in_data);
            free(pipe_data);
            pipe_data = NULL;
            pipe_len = 0;
            if (pc->out_file) {
                char path[VFS_PATH_MAX];
                int r = term_resolve(t, pc->out_file, path);
                if (!r && vfs_is_dir(path)) r = VFS_EISDIR;
                if (!r) r = pc->append ? vfs_append_file(path, out.buf ? out.buf : "", (uint32_t)out.len)
                                       : vfs_write_file(path, out.buf ? out.buf : "", (uint32_t)out.len);
                if (r) {
                    term_printf(t, T_BRED "nsh: %s: %s" T_RESET "\n", pc->out_file, vfs_strerror(r));
                    status = 1;
                }
                free(out.buf);
            } else if (i < nc - 1) {
                pipe_data = out.buf;
                pipe_len = out.len;
            }
        }
        free(pipe_data);
        t->status = status;
    }

    for (int i = 0; i < nt; i++) free(toks[i].s);
    for (int i = 0; i < nc; i++) {
        for (int a = 0; a < cmds[i].argc; a++) free(cmds[i].argv[a]);
        free(cmds[i].in_file);
        free(cmds[i].out_file);
    }
    free(toks);
    free(cmds);
}

/* ======================================================================================
 * Prompt, banner, submit
 * ==================================================================================== */
static void update_title(term_t *t) {
    char p[VFS_PATH_MAX], title[VFS_PATH_MAX + 16];
    pretty_path(t->cwd, p, sizeof p);
    snprintf(title, sizeof title, "Terminal - %s", p);
    const char *cur = win_get_title(t->win);
    if (!cur || strcmp(cur, title)) win_set_title(t->win, title);
}

void term_shell_prompt(term_t *t) {
    tline_t *l = &t->lines[(t->first + t->count - 1) % TERM_MAX_LINES];
    if (l->len > 0) term_write(t, "\n", 1);
    if (t->mode == TERM_MODE_CALC) {
        term_puts(t, T_RESET "\x1b[1;96mcalc" T_RESET "\x1b[90m>" T_RESET " ");
    } else {
        char p[VFS_PATH_MAX];
        pretty_path(t->cwd, p, sizeof p);
        term_printf(t, T_RESET "\x1b[1;94m%s" T_RESET "%s%%>" T_RESET " ",
                    p, t->status ? T_BRED : "\x1b[1;96m");
        update_title(t);
    }
    t->input_on = true;
}

void term_shell_banner(term_t *t) {
    term_puts(t, T_BOLD "NOVA CMD / V1.0" T_RESET "\n\n");
    term_print_logo(t);
    term_puts(t, "\n");
}

static void calc_line(term_t *t, const char *line) {
    while (*line == ' ') line++;
    if (!*line) return;
    if (!strcmp(line, "exit") || !strcmp(line, "quit") || !strcmp(line, "q")) {
        t->mode = TERM_MODE_SHELL;
        return;
    }
    double r;
    char err[96];
    if (term_calc_eval(line, t->calc_ans, &r, err, sizeof err) == 0) {
        char buf[64];
        term_calc_format(r, buf, sizeof buf);
        term_printf(t, T_GRAY "= " T_RESET "\x1b[1;93m%s" T_RESET "\n", buf);
        t->calc_ans = r;
    } else {
        term_printf(t, T_BRED "error: %s" T_RESET "\n", err);
    }
}

void term_shell_submit(term_t *t, const char *line) {
    if (t->mode == TERM_MODE_CALC) {
        calc_line(t, line);
        term_shell_prompt(t);
        return;
    }
    char *exp = malloc(TERM_INPUT_MAX * 2);
    if (!exp) { term_shell_prompt(t); return; }
    bool changed = false;
    char bad[64];
    if (!hist_expand(t, line, exp, TERM_INPUT_MAX * 2, &changed, bad, sizeof bad)) {
        term_printf(t, T_BRED "nsh: %s: event not found" T_RESET "\n", bad);
        t->status = 1;
        free(exp);
        term_shell_prompt(t);
        return;
    }
    if (changed) term_printf(t, T_GRAY "%s" T_RESET "\n", exp);
    term_history_add(t, exp);
    tseg_t *segs = split_segments(exp);
    free(exp);
    /* new segments run before anything still queued */
    if (segs) {
        tseg_t *last = segs;
        while (last->next) last = last->next;
        last->next = t->queue;
        t->queue = segs;
    }
    t->seg_budget = 2000;
    term_shell_resume(t);
}

void term_shell_resume(term_t *t) {
    while (t->queue && !t->job.active) {
        tseg_t *s = t->queue;
        t->queue = s->next;
        if (--t->seg_budget < 0) {
            term_puts(t, T_BRED "nsh: too many commands (runaway script?) - stopped" T_RESET "\n");
            free(s);
            while (t->queue) { tseg_t *n = t->queue->next; free(t->queue); t->queue = n; }
            t->status = 1;
            break;
        }
        bool run = s->conn == 0 || (s->conn == 1 && t->status == 0) || (s->conn == 2 && t->status != 0);
        if (run) {
            uint32_t t0 = sys_uptime_ms();
            run_segment(t, s->text);
            uint32_t dt = sys_uptime_ms() - t0;
            if (dt > 100) klog("terminal: '%.40s' took %u ms", s->text, dt);
        }
        free(s);
    }
    if (!t->job.active) term_shell_prompt(t);
}

/* ======================================================================================
 * Tab completion
 * ==================================================================================== */
#define MAX_CAND 512
typedef struct { char *name[MAX_CAND]; bool dir[MAX_CAND]; int n; } cands_t;

static void cand_add(cands_t *c, const char *name, bool dir) {
    if (c->n >= MAX_CAND) return;
    for (int i = 0; i < c->n; i++) if (!strcmp(c->name[i], name)) return;
    char *d = strdup(name);
    if (!d) return;
    c->name[c->n] = d;
    c->dir[c->n] = dir;
    c->n++;
}

static bool prefix_match(const char *s, const char *pre, bool icase) {
    size_t n = strlen(pre);
    return icase ? !strncasecmp(s, pre, n) : !strncmp(s, pre, n);
}

static void add_app_cands(cands_t *c, const char *pre, bool icase, bool underscores) {
    for (int i = 0; i < app_count(); i++) {
        const app_desc_t *a = app_get(i);
        if (!a || !a->name) continue;
        char nm[64];
        strlcpy(nm, a->name, sizeof nm);
        if (underscores) for (char *p = nm; *p; p++) if (*p == ' ') *p = '_';
        if (prefix_match(nm, pre, icase)) cand_add(c, nm, false);
    }
}

static void add_path_cands(term_t *t, cands_t *c, const char *dirpart, const char *base, bool icase, bool dirs_only) {
    char dir[VFS_PATH_MAX];
    if (term_resolve(t, dirpart[0] ? dirpart : ".", dir) || !vfs_is_dir(dir)) return;
    int total = vfs_list(dir, NULL, 0);
    if (total <= 0) return;
    vfs_dirent_t *ents = malloc((size_t)total * sizeof *ents);
    if (!ents) return;
    total = MIN(total, vfs_list(dir, ents, total));
    for (int i = 0; i < total; i++) {
        if (ents[i].name[0] == '.' && base[0] != '.') continue;
        if (dirs_only && !ents[i].is_dir) continue;
        if (prefix_match(ents[i].name, base, icase)) cand_add(c, ents[i].name, ents[i].is_dir);
    }
    free(ents);
}

static int escaped_copy(const char *s, char *out, int size) {
    int o = 0;
    for (; *s && o < size - 2; s++) {
        if (strchr(" \\'\"|;&<>$*?#", *s)) out[o++] = '\\';
        out[o++] = *s;
    }
    out[o] = 0;
    return o;
}

static void sort_cands(cands_t *c) {
    for (int i = 1; i < c->n; i++) {           /* insertion sort, keeps dir flags paired */
        char *nm = c->name[i];
        bool d = c->dir[i];
        int j = i - 1;
        while (j >= 0 && strcasecmp(c->name[j], nm) > 0) {
            c->name[j + 1] = c->name[j];
            c->dir[j + 1] = c->dir[j];
            j--;
        }
        c->name[j + 1] = nm;
        c->dir[j + 1] = d;
    }
}

static void list_cands(term_t *t, cands_t *c) {
    sort_cands(c);
    int w = 0;
    for (int i = 0; i < c->n; i++) w = MAX(w, (int)strlen(c->name[i]) + (c->dir[i] ? 1 : 0));
    w += 2;
    int per = MAX(1, t->cols / w);
    int rows = (c->n + per - 1) / per;
    t->input_on = false;
    term_write(t, t->in, t->in_len);
    term_write(t, "\n", 1);
    for (int r = 0; r < rows; r++) {
        for (int k = 0; k < per; k++) {
            int i = k * rows + r;
            if (i >= c->n) break;
            int len = (int)strlen(c->name[i]) + (c->dir[i] ? 1 : 0);
            if (c->dir[i]) term_printf(t, "\x1b[1;94m%s" T_RESET "/", c->name[i]);
            else term_puts(t, c->name[i]);
            if (k < per - 1 && i + rows < c->n) for (int s = len; s < w; s++) term_write(t, " ", 1);
        }
        term_write(t, "\n", 1);
    }
    term_shell_prompt(t);
}

void term_shell_complete(term_t *t) {
    if (t->mode == TERM_MODE_CALC) { sys_beep(440, 20); return; }
    int cur = t->in_cur, ws = cur;
    while (ws > 0) {
        char ch = t->in[ws - 1];
        if ((ch == ' ' || strchr("|;&<>", ch)) && !(ws >= 2 && t->in[ws - 2] == '\\')) break;
        ws--;
    }
    /* first word of this command, and is the cursor in command position? */
    int p = ws - 1;
    while (p >= 0 && t->in[p] == ' ') p--;
    bool cmdpos = p < 0 || strchr("|;&", t->in[p]);
    char cmd[64] = "";
    int argn = 0;
    if (!cmdpos) {
        int s = p;
        while (s > 0 && !strchr("|;&", t->in[s - 1])) s--;
        while (t->in[s] == ' ') s++;
        int e = s;
        while (e < ws && t->in[e] != ' ') e++;
        unquote(t->in + s, e - s, cmd, sizeof cmd);
        for (int k = s; k < ws; k++) if (t->in[k] == ' ' && t->in[k + 1] != ' ' && (k == 0 || t->in[k - 1] != '\\')) argn++;
    }

    char word[VFS_PATH_MAX];
    unquote(t->in + ws, cur - ws, word, sizeof word);
    /* position in the input where the replaced part starts (after the last '/') */
    int bstart = ws;
    for (int k = ws; k < cur; k++) if (t->in[k] == '/' && (k == 0 || t->in[k - 1] != '\\')) bstart = k + 1;
    char dirpart[VFS_PATH_MAX] = "";
    const char *base = word;
    char *sl = strrchr(word, '/');
    if (sl) {
        int dl = (int)(sl - word) + 1;
        memcpy(dirpart, word, (size_t)dl);
        dirpart[dl] = 0;
        base = sl + 1;
    }

    cands_t *c = calloc(1, sizeof *c);
    if (!c) return;
    for (int pass = 0; pass < 2 && c->n == 0; pass++) {
        bool icase = pass == 1;
        if (cmdpos && !sl) {
            for (int i = 0; i < cmd_count(); i++) if (prefix_match(cmd_get(i)->name, base, icase)) cand_add(c, cmd_get(i)->name, false);
            for (int i = 0; i < command_count(); i++) if (prefix_match(command_get(i)->name, base, icase)) cand_add(c, command_get(i)->name, false);
            add_app_cands(c, base, icase, true);
            if (c->n == 0) add_path_cands(t, c, dirpart, base, icase, false);
        } else if (!strcmp(cmd, "help") || !strcmp(cmd, "which")) {
            for (int i = 0; i < cmd_count(); i++) if (prefix_match(cmd_get(i)->name, base, icase)) cand_add(c, cmd_get(i)->name, false);
            for (int i = 0; i < command_count(); i++) if (prefix_match(command_get(i)->name, base, icase)) cand_add(c, command_get(i)->name, false);
            if (!strcmp(cmd, "which")) add_app_cands(c, base, icase, true);
        } else if (!strcmp(cmd, "color") || !strcmp(cmd, "theme")) {
            for (int i = 0; i < term_palette_count(); i++)
                if (prefix_match(term_palette(i)->name, base, icase)) cand_add(c, term_palette(i)->name, false);
            if (prefix_match("check", base, icase)) cand_add(c, "check", false);
            if (prefix_match("next", base, icase)) cand_add(c, "next", false);
        } else {
            if ((!strcmp(cmd, "open") || !strcmp(cmd, "run")) && argn <= 1 && !sl) add_app_cands(c, base, icase, false);
            bool dirs_only = !strcmp(cmd, "cd") || !strcmp(cmd, "rmdir") || !strcmp(cmd, "tree");
            add_path_cands(t, c, dirpart, base, icase, dirs_only);
        }
    }

    if (c->n == 0) {
        sys_beep(440, 25);
    } else {
        /* longest common prefix */
        bool icase = false;
        for (int i = 0; i < c->n; i++) if (!prefix_match(c->name[i], base, false)) icase = true;
        int lcp = (int)strlen(c->name[0]);
        for (int i = 1; i < c->n; i++) {
            int k = 0;
            while (k < lcp && c->name[i][k] &&
                   (icase ? tolower((unsigned char)c->name[i][k]) == tolower((unsigned char)c->name[0][k])
                          : c->name[i][k] == c->name[0][k])) k++;
            lcp = k;
        }
        char rep[VFS_PATH_MAX * 2];
        if (c->n == 1) {
            int n = escaped_copy(c->name[0], rep, (int)sizeof rep - 2);
            rep[n++] = c->dir[0] ? '/' : ' ';
            rep[n] = 0;
        } else {
            char pre[VFS_PATH_MAX];
            int l = MIN(lcp, (int)sizeof pre - 1);
            memcpy(pre, c->name[0], (size_t)l);
            pre[l] = 0;
            escaped_copy(pre, rep, (int)sizeof rep);
        }
        int blen = (int)strlen(base);
        /* progress = the input would change: unique match, a longer common
           prefix, or (case-insensitive matches) a case correction */
        bool progress = c->n == 1 || lcp > blen || (icase && lcp >= blen && strncmp(c->name[0], base, (size_t)blen));
        if (progress) {
            /* replace input[bstart..cur) with rep */
            int old = cur - bstart, add = (int)strlen(rep);
            if (t->in_len - old + add < TERM_INPUT_MAX - 1) {
                memmove(t->in + bstart + add, t->in + cur, (size_t)(t->in_len - cur));
                memcpy(t->in + bstart, rep, (size_t)add);
                t->in_len += add - old;
                t->in[t->in_len] = 0;
                t->in_cur = bstart + add;
            }
            t->tab_presses = -1;      /* the key handler increments this to 0 */
        } else if (t->tab_presses >= 1) {
            list_cands(t, c);
        } else {
            sys_beep(440, 25);
        }
    }
    for (int i = 0; i < c->n; i++) free(c->name[i]);
    free(c);
}

/* ======================================================================================
 * Built-in commands: files
 * ==================================================================================== */
static const char *file_color(const char *name, bool dir) {
    if (dir) return "\x1b[1;94m";
    const char *e = vfs_extension(name);
    if (!strcasecmp(e, "bmp") || !strcasecmp(e, "ppm") || !strcasecmp(e, "png") || !strcasecmp(e, "jpg")) return "\x1b[95m";
    if (!strcasecmp(e, "sh")) return "\x1b[1;92m";
    if (!strcasecmp(e, "wav") || !strcasecmp(e, "mid") || !strcasecmp(e, "mp3")) return "\x1b[96m";
    if (!strcasecmp(e, "zip") || !strcasecmp(e, "img") || !strcasecmp(e, "iso")) return "\x1b[91m";
    if (name[0] == '.') return "\x1b[90m";
    return "";
}

static void month_abbr(int m, char *out) {
    const char *n = sys_month_name(m);
    out[0] = n ? n[0] : '?';
    out[1] = n && n[0] ? n[1] : '?';
    out[2] = n && n[0] && n[1] ? n[2] : '?';
    out[3] = 0;
}

static void ls_long_entry(tctx_t *c, const char *name, bool dir, uint32_t size, uint32_t mtime, bool human) {
    datetime_t dt;
    sys_time_to_datetime(mtime, &dt);
    char mon[4], sz[16];
    month_abbr(dt.month, mon);
    if (dir) strlcpy(sz, "-", sizeof sz);
    else if (human) human_size(size, sz, sizeof sz);
    else snprintf(sz, sizeof sz, "%u", size);
    tc_printf(c, "%s%s" T_RESET "  %s  %8s  %s %2d %02d:%02d  %s%s" T_RESET "%s\n",
              col(c, dir ? T_BLUE : T_GRAY), dir ? "drwxr-xr-x" : "-rw-r--r--", TERM_USER, sz, mon, dt.day,
              dt.hour, dt.minute, col(c, file_color(name, dir)), name, dir && c->out->tty ? "/" : "");
}

static int cmd_ls(tctx_t *c, int argc, char **argv) {
    bool f[256] = { 0 };
    int i = get_flags(c, argc, argv, "laAh1", f);
    if (i < 0) return 2;
    bool lng = f['l'], all = f['a'] || f['A'], human = f['h'], one = f['1'] || !c->out->tty;
    int status = 0, npaths = argc - i;
    const char *def[1] = { "." };
    char **paths = npaths > 0 ? argv + i : (char **)def;
    if (npaths <= 0) npaths = 1;
    for (int pi = 0; pi < npaths; pi++) {
        char path[VFS_PATH_MAX];
        vfs_stat_t st;
        if (term_resolve(c->t, paths[pi], path) || vfs_stat(path, &st)) {
            tc_err(c, "ls: %s: No such file or directory", paths[pi]);
            status = 1;
            continue;
        }
        if (!st.is_dir) {
            if (lng) ls_long_entry(c, paths[pi], false, st.size, st.mtime, human);
            else tc_printf(c, "%s%s" T_RESET "\n", col(c, file_color(paths[pi], false)), paths[pi]);
            continue;
        }
        if (npaths > 1) tc_printf(c, "%s%s:" T_RESET "\n", col(c, T_BOLD), paths[pi]);
        int total = vfs_list(path, NULL, 0);
        vfs_dirent_t *ents = total > 0 ? malloc((size_t)total * sizeof *ents) : NULL;
        if (total > 0 && !ents) { tc_err(c, "ls: out of memory"); return 1; }
        if (ents) total = MIN(total, vfs_list(path, ents, total));
        int n = 0;
        for (int k = 0; k < total; k++) if (all || ents[k].name[0] != '.') ents[n++] = ents[k];
        if (lng) {
            uint32_t sum = 0;
            for (int k = 0; k < n; k++) sum += ents[k].size;
            char hs[24];
            if (sum < 1024) snprintf(hs, sizeof hs, "%u byte%s", sum, sum == 1 ? "" : "s");
            else if (sum < 1024 * 1024) snprintf(hs, sizeof hs, "%.1f KiB", sum / 1024.0);
            else snprintf(hs, sizeof hs, "%.1f MiB", sum / (1024.0 * 1024.0));
            tc_printf(c, "%s%d item%s, %s" T_RESET "\n", col(c, T_GRAY), n, n == 1 ? "" : "s", hs);
            for (int k = 0; k < n; k++) ls_long_entry(c, ents[k].name, ents[k].is_dir, ents[k].size, ents[k].mtime, human);
        } else if (one) {
            for (int k = 0; k < n; k++)
                tc_printf(c, "%s%s" T_RESET "%s\n", col(c, file_color(ents[k].name, ents[k].is_dir)), ents[k].name,
                          ents[k].is_dir && c->out->tty ? "/" : "");
        } else if (n > 0) {
            int w = 0;
            for (int k = 0; k < n; k++) w = MAX(w, (int)strlen(ents[k].name) + (ents[k].is_dir ? 1 : 0));
            w += 2;
            int per = MAX(1, tc_term_cols(c) / w), rows = (n + per - 1) / per;
            for (int r = 0; r < rows; r++) {
                for (int k = 0; k < per; k++) {
                    int e = k * rows + r;
                    if (e >= n) break;
                    int len = (int)strlen(ents[e].name) + (ents[e].is_dir ? 1 : 0);
                    tc_printf(c, "%s%s" T_RESET "%s", col(c, file_color(ents[e].name, ents[e].is_dir)), ents[e].name,
                              ents[e].is_dir ? "/" : "");
                    if (k < per - 1 && e + rows < n) for (int s = len; s < w; s++) tc_write(c, " ", 1);
                }
                tc_write(c, "\n", 1);
            }
        }
        free(ents);
        if (npaths > 1 && pi < npaths - 1) tc_write(c, "\n", 1);
    }
    return status;
}

static int cmd_cd(tctx_t *c, int argc, char **argv) {
    term_t *t = c->t;
    const char *target = argc > 1 ? argv[1] : TERM_HOME;
    bool dash = !strcmp(target, "-");
    if (dash) target = t->oldpwd;
    char path[VFS_PATH_MAX];
    if (term_resolve(t, target, path)) { tc_err(c, "cd: %s: invalid path", target); return 1; }
    if (!vfs_exists(path)) { tc_err(c, "cd: %s: No such file or directory", target); return 1; }
    if (!vfs_is_dir(path)) { tc_err(c, "cd: %s: Not a directory", target); return 1; }
    strlcpy(t->oldpwd, t->cwd, sizeof t->oldpwd);
    strlcpy(t->cwd, path, sizeof t->cwd);
    if (dash) tc_printf(c, "%s\n", path);
    return 0;
}

static int cmd_pwd(tctx_t *c, int argc, char **argv) {
    tc_printf(c, "%s\n", c->t->cwd);
    return 0;
}

typedef struct { bool number; int line; } cat_ctx_t;
static int cat_one(tctx_t *c, const char *name, const char *data, uint32_t len, void *ctx) {
    cat_ctx_t *cc = ctx;
    if (name && is_binary(data, len)) {
        tc_err(c, "cat: %s: binary file (%u bytes) - try " T_BOLD "hexdump %s", name, len, name);
        return 1;
    }
    if (!cc->number) { tc_write(c, data, (int)len); return 0; }
    liter_t it = { data, data + len };
    const char *ls;
    int ln;
    while (next_line(&it, &ls, &ln)) {
        tc_printf(c, "%s%6d" T_RESET "  ", col(c, T_GRAY), ++cc->line);
        tc_write(c, ls, ln);
        tc_write(c, "\n", 1);
    }
    return 0;
}

static int cmd_cat(tctx_t *c, int argc, char **argv) {
    bool f[256] = { 0 };
    int i = get_flags(c, argc, argv, "n", f);
    if (i < 0) return 2;
    cat_ctx_t cc = { f['n'], 0 };
    return for_each_input(c, argc, argv, i, cat_one, &cc);
}

static void echo_escapes(const char *s, char *out, int size) {
    int o = 0;
    for (; *s && o < size - 1; s++) {
        if (*s != '\\' || !s[1]) { out[o++] = *s; continue; }
        s++;
        switch (*s) {
        case 'n': out[o++] = '\n'; break;
        case 't': out[o++] = '\t'; break;
        case 'e': out[o++] = 0x1B; break;
        case 'a': out[o++] = '\a'; break;
        case '\\': out[o++] = '\\'; break;
        case '0': {
            int v = 0, k = 0;
            while (k < 3 && s[1] >= '0' && s[1] <= '7') { v = v * 8 + (*++s - '0'); k++; }
            out[o++] = (char)v;
            break;
        }
        case 'x': {
            int v = 0, k = 0;
            while (k < 2 && isxdigit((unsigned char)s[1])) {
                char h = *++s;
                v = v * 16 + (isdigit((unsigned char)h) ? h - '0' : (tolower((unsigned char)h) - 'a' + 10));
                k++;
            }
            out[o++] = (char)v;
            break;
        }
        default: out[o++] = '\\'; if (o < size - 1) out[o++] = *s; break;
        }
    }
    out[o] = 0;
}

static int cmd_echo(tctx_t *c, int argc, char **argv) {
    bool nl = true, esc = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        bool ok = true;
        for (const char *p = argv[i] + 1; *p; p++) if (!strchr("neE", *p)) ok = false;
        if (!ok) break;
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'n') nl = false;
            else if (*p == 'e') esc = true;
            else esc = false;
        }
    }
    for (; i < argc; i++) {
        if (esc) {
            char buf[TERM_INPUT_MAX];
            echo_escapes(argv[i], buf, sizeof buf);
            tc_puts(c, buf);
        } else {
            tc_puts(c, argv[i]);
        }
        if (i < argc - 1) tc_write(c, " ", 1);
    }
    if (nl) tc_write(c, "\n", 1);
    return 0;
}

static int cmd_mkdir(tctx_t *c, int argc, char **argv) {
    bool f[256] = { 0 };
    int i = get_flags(c, argc, argv, "pv", f);
    if (i < 0) return 2;
    if (i >= argc) { tc_err(c, "usage: mkdir [-p] DIR..."); return 2; }
    int status = 0;
    for (; i < argc; i++) {
        char path[VFS_PATH_MAX];
        int r = term_resolve(c->t, argv[i], path);
        if (!r) r = f['p'] ? vfs_mkdirs(path) : vfs_mkdir(path);
        if (r == VFS_EEXIST && f['p'] && vfs_is_dir(path)) r = 0;
        if (r) { tc_err(c, "mkdir: %s: %s", argv[i], vfs_strerror(r)); status = 1; }
        else if (f['v']) tc_printf(c, "created directory '%s'\n", argv[i]);
    }
    return status;
}

static bool protected_path(const char *p) {
    return !strcmp(p, "/") || !strcmp(p, TERM_HOME) || !strcmp(p, "/home") || !strcmp(p, "/system");
}

static int cmd_rmdir(tctx_t *c, int argc, char **argv) {
    if (argc < 2) { tc_err(c, "usage: rmdir DIR..."); return 2; }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char path[VFS_PATH_MAX];
        int r = term_resolve(c->t, argv[i], path);
        if (!r && !vfs_exists(path)) r = VFS_ENOENT;
        if (!r && !vfs_is_dir(path)) r = VFS_ENOTDIR;
        if (!r && protected_path(path)) { tc_err(c, "rmdir: refusing to remove '%s'", path); status = 1; continue; }
        if (!r) r = vfs_remove(path);
        if (r) { tc_err(c, "rmdir: %s: %s", argv[i], vfs_strerror(r)); status = 1; }
    }
    return status;
}

static int cmd_rm(tctx_t *c, int argc, char **argv) {
    bool f[256] = { 0 };
    int i = get_flags(c, argc, argv, "rRfv", f);
    if (i < 0) return 2;
    bool rec = f['r'] || f['R'];
    if (i >= argc) { tc_err(c, "usage: rm [-r] [-f] FILE..."); return 2; }
    int status = 0;
    for (; i < argc; i++) {
        char path[VFS_PATH_MAX];
        if (term_resolve(c->t, argv[i], path)) { tc_err(c, "rm: %s: invalid path", argv[i]); status = 1; continue; }
        if (!vfs_exists(path)) {
            if (!f['f']) { tc_err(c, "rm: %s: No such file or directory", argv[i]); status = 1; }
            continue;
        }
        /* never delete /, home, /system, or the directory we are standing in */
        size_t pl = strlen(path);
        bool holds_cwd = !strncmp(c->t->cwd, path, pl) && (c->t->cwd[pl] == 0 || c->t->cwd[pl] == '/');
        if (protected_path(path) || holds_cwd) {
            tc_err(c, "rm: refusing to remove '%s'", path);
            status = 1;
            continue;
        }
        int r;
        if (vfs_is_dir(path)) {
            if (!rec) { tc_err(c, "rm: %s: is a directory (use rm -r)", argv[i]); status = 1; continue; }
            r = vfs_remove_recursive(path);
        } else {
            r = vfs_remove(path);
        }
        if (r) { tc_err(c, "rm: %s: %s", argv[i], vfs_strerror(r)); status = 1; }
        else if (f['v']) tc_printf(c, "removed '%s'\n", argv[i]);
    }
    return status;
}

static int cmd_touch(tctx_t *c, int argc, char **argv) {
    if (argc < 2) { tc_err(c, "usage: touch FILE..."); return 2; }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char path[VFS_PATH_MAX];
        int r = term_resolve(c->t, argv[i], path);
        if (!r && vfs_is_dir(path)) continue;
        if (!r) r = vfs_append_file(path, "", 0);      /* creates, or updates mtime */
        if (r) { tc_err(c, "touch: %s: %s", argv[i], vfs_strerror(r)); status = 1; }
    }
    return status;
}

/* cp and mv share the "SRC... DEST" logic. */
static int copy_or_move(tctx_t *c, int argc, char **argv, bool move) {
    const char *name = move ? "mv" : "cp";
    bool f[256] = { 0 };
    int i = get_flags(c, argc, argv, move ? "fv" : "rRfv", f);
    if (i < 0) return 2;
    bool rec = f['r'] || f['R'];
    int nsrc = argc - i - 1;
    if (nsrc < 1) { tc_err(c, "usage: %s %sSOURCE... DEST", name, move ? "" : "[-r] "); return 2; }
    char dest[VFS_PATH_MAX];
    if (term_resolve(c->t, argv[argc - 1], dest)) { tc_err(c, "%s: %s: invalid path", name, argv[argc - 1]); return 1; }
    bool dest_dir = vfs_is_dir(dest);
    if (nsrc > 1 && !dest_dir) { tc_err(c, "%s: target '%s' is not a directory", name, argv[argc - 1]); return 1; }
    int status = 0;
    for (int k = i; k < argc - 1; k++) {
        char src[VFS_PATH_MAX], to[VFS_PATH_MAX];
        if (term_resolve(c->t, argv[k], src) || !vfs_exists(src)) {
            tc_err(c, "%s: %s: No such file or directory", name, argv[k]);
            status = 1;
            continue;
        }
        if (dest_dir) vfs_join(dest, vfs_basename(src), to);
        else strlcpy(to, dest, sizeof to);
        if (!strcmp(src, to)) { tc_err(c, "%s: '%s' and '%s' are the same file", name, argv[k], to); status = 1; continue; }
        bool sdir = vfs_is_dir(src);
        if (sdir && !move && !rec) { tc_err(c, "%s: %s: is a directory (use cp -r)", name, argv[k]); status = 1; continue; }
        if (move && protected_path(src)) { tc_err(c, "mv: refusing to move '%s'", src); status = 1; continue; }
        size_t sl = strlen(src);
        if (sdir && !strncmp(to, src, sl) && to[sl] == '/') {
            tc_err(c, "%s: cannot %s '%s' into itself", name, move ? "move" : "copy", argv[k]);
            status = 1;
            continue;
        }
        int r = move ? vfs_rename(src, to) : vfs_copy(src, to);
        if (r) { tc_err(c, "%s: %s -> %s: %s", name, argv[k], to, vfs_strerror(r)); status = 1; }
        else {
            if (move && !strncmp(c->t->cwd, src, sl) && (c->t->cwd[sl] == 0 || c->t->cwd[sl] == '/')) {
                /* we moved the directory we are standing in: follow it */
                char ncwd[VFS_PATH_MAX];
                snprintf(ncwd, sizeof ncwd, "%s%s", to, c->t->cwd + sl);
                strlcpy(c->t->cwd, ncwd, sizeof c->t->cwd);
            }
            if (f['v']) tc_printf(c, "'%s' -> '%s'\n", argv[k], to);
        }
    }
    return status;
}

static int cmd_cp(tctx_t *c, int argc, char **argv) { return copy_or_move(c, argc, argv, false); }
static int cmd_mv(tctx_t *c, int argc, char **argv) { return copy_or_move(c, argc, argv, true); }

/* tree */
typedef struct { int dirs, files, shown; int max_depth; bool all; bool limited; } tree_ctx_t;
static void tree_walk(tctx_t *c, const char *path, char *prefix, int depth, tree_ctx_t *tc) {
    if (depth >= tc->max_depth) return;
    int total = vfs_list(path, NULL, 0);
    if (total <= 0) return;
    vfs_dirent_t *ents = malloc((size_t)total * sizeof *ents);
    if (!ents) return;
    total = MIN(total, vfs_list(path, ents, total));
    int n = 0;
    for (int k = 0; k < total; k++) if (tc->all || ents[k].name[0] != '.') ents[n++] = ents[k];
    int plen = (int)strlen(prefix);
    for (int k = 0; k < n; k++) {
        if (tc->shown >= 3000) { tc->limited = true; break; }
        tc->shown++;
        bool last = k == n - 1;
        tc_printf(c, "%s%s%s%s" T_RESET "\n", prefix, last ? "\x84\x80\x80 " : "\x86\x80\x80 ",
                  col(c, file_color(ents[k].name, ents[k].is_dir)), ents[k].name);
        if (ents[k].is_dir) {
            tc->dirs++;
            if (plen + 8 < 512) {
                strcpy(prefix + plen, last ? "    " : "\x81   ");
                char sub[VFS_PATH_MAX];
                vfs_join(path, ents[k].name, sub);
                tree_walk(c, sub, prefix, depth + 1, tc);
                prefix[plen] = 0;
            }
        } else {
            tc->files++;
        }
    }
    free(ents);
}

static int cmd_tree(tctx_t *c, int argc, char **argv) {
    tree_ctx_t tc = { 0, 0, 0, 32, false, false };
    const char *target = ".";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) tc.all = true;
        else if (!strcmp(argv[i], "-L") && i + 1 < argc) { if (!parse_int(argv[++i], &tc.max_depth) || tc.max_depth < 1) { tc_err(c, "tree: bad depth"); return 2; } }
        else if (argv[i][0] == '-') { tc_err(c, "usage: tree [-a] [-L depth] [DIR]"); return 2; }
        else target = argv[i];
    }
    char path[VFS_PATH_MAX];
    if (term_resolve(c->t, target, path) || !vfs_is_dir(path)) { tc_err(c, "tree: %s: not a directory", target); return 1; }
    tc_printf(c, "%s%s" T_RESET "\n", col(c, "\x1b[1;94m"), target);
    char *prefix = malloc(512);
    if (!prefix) return 1;
    prefix[0] = 0;
    tree_walk(c, path, prefix, 0, &tc);
    free(prefix);
    if (tc.limited) tc_printf(c, "%s... (output limited)" T_RESET "\n", col(c, T_GRAY));
    tc_printf(c, "\n%d director%s, %d file%s\n", tc.dirs, tc.dirs == 1 ? "y" : "ies", tc.files, tc.files == 1 ? "" : "s");
    return 0;
}

/* find [DIR] [-name PATTERN] [-type f|d] */
typedef struct { const char *pat; int type; int found; } find_ctx_t;
static void find_walk(tctx_t *c, const char *path, const char *shown, find_ctx_t *fc, int depth) {
    if (depth > 32 || fc->found > 5000) return;
    int total = vfs_list(path, NULL, 0);
    if (total <= 0) return;
    vfs_dirent_t *ents = malloc((size_t)total * sizeof *ents);
    if (!ents) return;
    total = MIN(total, vfs_list(path, ents, total));
    for (int k = 0; k < total; k++) {
        char sub[VFS_PATH_MAX], subshown[VFS_PATH_MAX];
        vfs_join(path, ents[k].name, sub);
        if (!strcmp(shown, "/")) snprintf(subshown, sizeof subshown, "/%s", ents[k].name);
        else snprintf(subshown, sizeof subshown, "%s/%s", shown, ents[k].name);
        bool type_ok = fc->type == 0 || (fc->type == 'd') == ents[k].is_dir;
        if (type_ok && (!fc->pat || wild_match(fc->pat, ents[k].name, true))) {
            tc_printf(c, "%s%s" T_RESET "\n", col(c, file_color(ents[k].name, ents[k].is_dir)), subshown);
            fc->found++;
        }
        if (ents[k].is_dir) find_walk(c, sub, subshown, fc, depth + 1);
    }
    free(ents);
}

static int cmd_find(tctx_t *c, int argc, char **argv) {
    find_ctx_t fc = { NULL, 0, 0 };
    const char *target = ".";
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-name") || !strcmp(argv[i], "-iname")) && i + 1 < argc) fc.pat = argv[++i];
        else if (!strcmp(argv[i], "-type") && i + 1 < argc) { i++; fc.type = argv[i][0] == 'd' ? 'd' : 'f'; }
        else if (argv[i][0] == '-') { tc_err(c, "usage: find [DIR] [-name PATTERN] [-type f|d]"); return 2; }
        else target = argv[i];
    }
    char path[VFS_PATH_MAX];
    if (term_resolve(c->t, target, path) || !vfs_is_dir(path)) { tc_err(c, "find: %s: not a directory", target); return 1; }
    find_walk(c, path, target, &fc, 0);
    return fc.found ? 0 : 1;
}

static int cmd_stat(tctx_t *c, int argc, char **argv) {
    if (argc < 2) { tc_err(c, "usage: stat FILE..."); return 2; }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char path[VFS_PATH_MAX];
        vfs_stat_t st;
        if (term_resolve(c->t, argv[i], path) || vfs_stat(path, &st)) {
            tc_err(c, "stat: %s: No such file or directory", argv[i]);
            status = 1;
            continue;
        }
        datetime_t m, cr;
        sys_time_to_datetime(st.mtime, &m);
        sys_time_to_datetime(st.ctime, &cr);
        tc_printf(c, "%s  File:%s %s\n", col(c, T_GRAY), col(c, T_RESET), path);
        if (st.is_dir) {
            int n = vfs_list(path, NULL, 0);
            tc_printf(c, "%s  Type:%s directory, %d entr%s\n", col(c, T_GRAY), col(c, T_RESET), n, n == 1 ? "y" : "ies");
        } else {
            const app_desc_t *a = app_for_file(path);
            tc_printf(c, "%s  Type:%s file, %u bytes%s%s\n", col(c, T_GRAY), col(c, T_RESET), st.size,
                      a ? ", opens with " : "", a ? a->name : "");
        }
        tc_printf(c, "%sModify:%s %04d-%02d-%02d %02d:%02d:%02d\n", col(c, T_GRAY), col(c, T_RESET), m.year, m.month, m.day, m.hour, m.minute, m.second);
        tc_printf(c, "%s Birth:%s %04d-%02d-%02d %02d:%02d:%02d\n", col(c, T_GRAY), col(c, T_RESET), cr.year, cr.month, cr.day, cr.hour, cr.minute, cr.second);
    }
    return status;
}

/* hexdump [-n BYTES] [-s OFFSET] FILE */
static int cmd_hexdump(tctx_t *c, int argc, char **argv) {
    int limit = 4096, skip = 0;
    const char *file = NULL;
    bool explicit_limit = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) { if (!parse_int(argv[++i], &limit) || limit < 0) { tc_err(c, "hexdump: bad length"); return 2; } explicit_limit = true; }
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) { if (!parse_int(argv[++i], &skip) || skip < 0) { tc_err(c, "hexdump: bad offset"); return 2; } }
        else if (!strcmp(argv[i], "-C")) continue;
        else if (argv[i][0] == '-' && argv[i][1]) { tc_err(c, "usage: hexdump [-n BYTES] [-s OFFSET] FILE"); return 2; }
        else file = argv[i];
    }
    const unsigned char *d;
    uint32_t len;
    char *data = NULL;
    if (file) {
        data = read_text(c, "hexdump", file, &len);
        if (!data) return 1;
        d = (const unsigned char *)data;
    } else if (c->has_in) {
        d = (const unsigned char *)(c->in ? c->in : "");
        len = (uint32_t)c->in_len;
    } else {
        tc_err(c, "usage: hexdump [-n BYTES] [-s OFFSET] FILE");
        return 2;
    }
    uint32_t start = MIN((uint32_t)skip, len);
    uint32_t end = MIN(len, start + (uint32_t)limit);
    bool tty = c->out->tty;
    static const char hexd[] = "0123456789abcdef";
    /* formatted by hand (no printf per byte): 190 KB dumps in a few tens of ms */
    char *line = malloc(320);              /* worst case (all zero bytes, colour) is 251 */
    if (!line) { free(data); return 1; }
    for (uint32_t off = start; off < end; off += 16) {
        int o = 0;
        if (tty) { memcpy(line + o, T_GRAY, 5); o += 5; }
        for (int sh = 28; sh >= 0; sh -= 4) line[o++] = hexd[(off >> sh) & 15];
        if (tty) { memcpy(line + o, T_RESET, 4); o += 4; }
        line[o++] = ' ';
        line[o++] = ' ';
        char ascii[17];
        int a = 0;
        for (int k = 0; k < 16; k++) {
            if (off + (uint32_t)k < end) {
                unsigned char b = d[off + k];
                if (tty && b == 0) { memcpy(line + o, T_GRAY "00" T_RESET, 11); o += 11; }
                else { line[o++] = hexd[b >> 4]; line[o++] = hexd[b & 15]; }
                line[o++] = ' ';
                ascii[a++] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
            } else {
                line[o++] = ' '; line[o++] = ' '; line[o++] = ' ';
            }
            if (k == 7) line[o++] = ' ';
        }
        line[o++] = ' ';
        if (tty) { memcpy(line + o, T_GRAY "|" T_CYAN, 11); o += 11; }
        else line[o++] = '|';
        memcpy(line + o, ascii, (size_t)a);
        o += a;
        if (tty) { memcpy(line + o, T_GRAY "|" T_RESET, 10); o += 10; }
        else line[o++] = '|';
        line[o++] = '\n';
        tc_write(c, line, o);
    }
    free(line);
    if (end < len && !explicit_limit)
        tc_printf(c, "%s... %u more bytes (use -n %u to see everything)" T_RESET "\n", col(c, T_GRAY), len - end, len);
    else
        tc_printf(c, "%s%08x" T_RESET "\n", col(c, T_GRAY), end);
    free(data);
    return 0;
}

/* ======================================================================================
 * Built-in commands: text filters
 * ==================================================================================== */
typedef struct { uint32_t l, w, b; uint32_t tl, tw, tb; bool fl, fw, fb; int files; } wc_ctx_t;
static int wc_one(tctx_t *c, const char *name, const char *data, uint32_t len, void *ctx) {
    wc_ctx_t *w = ctx;
    uint32_t lines = 0, words = 0;
    bool in_word = false;
    for (uint32_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];
        if (ch == '\n') lines++;
        if (isspace(ch)) in_word = false;
        else if (!in_word) { in_word = true; words++; }
    }
    w->tl += lines; w->tw += words; w->tb += len; w->files++;
    if (w->fl) tc_printf(c, "%7u ", lines);
    if (w->fw) tc_printf(c, "%7u ", words);
    if (w->fb) tc_printf(c, "%7u ", len);
    tc_printf(c, "%s\n", name ? name : "");
    return 0;
}

static int cmd_wc(tctx_t *c, int argc, char **argv) {
    bool f[256] = { 0 };
    int i = get_flags(c, argc, argv, "lwc", f);
    if (i < 0) return 2;
    wc_ctx_t w = { 0 };
    w.fl = f['l']; w.fw = f['w']; w.fb = f['c'];
    if (!w.fl && !w.fw && !w.fb) w.fl = w.fw = w.fb = true;
    int st = for_each_input(c, argc, argv, i, wc_one, &w);
    if (w.files > 1) {
        if (w.fl) tc_printf(c, "%7u ", w.tl);
        if (w.fw) tc_printf(c, "%7u ", w.tw);
        if (w.fb) tc_printf(c, "%7u ", w.tb);
        tc_puts(c, "total\n");
    }
    return st;
}

/* --- a small regular expression matcher for grep: . * ^ $ and literals ---
   (after Kernighan & Pike); returns the end of the match or NULL. */
static bool ch_eq(char a, char b, bool icase) {
    return icase ? tolower((unsigned char)a) == tolower((unsigned char)b) : a == b;
}
static const char *re_here(const char *re, const char *s, const char *end, bool icase);
static const char *re_star(char ch, const char *re, const char *s, const char *end, bool icase) {
    const char *t = s;
    while (t < end && (ch == '.' || ch_eq(*t, ch, icase))) t++;     /* greedy */
    for (; t >= s; t--) {
        const char *r = re_here(re, t, end, icase);
        if (r) return r;
    }
    return NULL;
}
static const char *re_here(const char *re, const char *s, const char *end, bool icase) {
    for (;;) {
        if (!re[0]) return s;
        char ch = re[0];
        int skip = 1;
        if (ch == '\\' && re[1]) { ch = re[1]; skip = 2; }
        if (re[skip] == '*') {
            if (skip == 2) {       /* escaped literal followed by * */
                const char *t = s;
                while (t < end && ch_eq(*t, ch, icase)) t++;
                for (; t >= s; t--) { const char *r = re_here(re + 3, t, end, icase); if (r) return r; }
                return NULL;
            }
            return re_star(ch, re + 2, s, end, icase);
        }
        if (skip == 1 && ch == '$' && !re[1]) return s == end ? s : NULL;
        if (s < end && ((skip == 1 && ch == '.') || ch_eq(*s, ch, icase))) { re += skip; s++; continue; }
        return NULL;
    }
}
/* Find the first match in [s, end). Returns start (and *mend) or NULL. */
static const char *re_search(const char *re, const char *s, const char *end, const char **mend, bool icase) {
    if (re[0] == '^') { const char *r = re_here(re + 1, s, end, icase); if (r) *mend = r; return r ? s : NULL; }
    for (const char *p = s; p <= end; p++) {
        const char *r = re_here(re, p, end, icase);
        if (r) { *mend = r; return p; }
        if (p == end) break;
    }
    return NULL;
}

typedef struct { const char *pat; bool icase, number, invert, count, names, quiet; int matches; } grep_ctx_t;
static int grep_one(tctx_t *c, const char *name, const char *data, uint32_t len, void *ctx) {
    grep_ctx_t *g = ctx;
    if (name && is_binary(data, len)) return 0;
    liter_t it = { data, data + len };
    const char *ls;
    int ln, lineno = 0, cnt = 0;
    while (next_line(&it, &ls, &ln)) {
        lineno++;
        const char *le = ls + ln, *me;
        const char *ms = re_search(g->pat, ls, le, &me, g->icase);
        bool hit = ms != NULL;
        if (hit == g->invert) continue;
        cnt++;
        if (g->count) continue;
        if (g->names && name) tc_printf(c, "%s%s%s:" T_RESET, col(c, T_MAGENTA), name, col(c, T_GRAY));
        if (g->number) tc_printf(c, "%s%d%s:" T_RESET, col(c, T_GREEN), lineno, col(c, T_GRAY));
        if (!hit || !c->out->tty) { tc_write(c, ls, ln); tc_write(c, "\n", 1); continue; }
        /* highlight every match on the line */
        const char *p = ls;
        while (ms && p <= le) {
            tc_write(c, p, (int)(ms - p));
            if (me == ms) { if (ms < le) tc_write(c, ms, 1); p = ms + 1; }
            else { tc_puts(c, "\x1b[1;91m"); tc_write(c, ms, (int)(me - ms)); tc_puts(c, T_RESET); p = me; }
            if (p > le || g->pat[0] == '^') break;
            ms = re_search(g->pat, p, le, &me, g->icase);
        }
        if (p < le) tc_write(c, p, (int)(le - p));
        tc_write(c, "\n", 1);
    }
    if (g->count) {
        if (g->names && name) tc_printf(c, "%s%s%s:" T_RESET, col(c, T_MAGENTA), name, col(c, T_GRAY));
        tc_printf(c, "%d\n", cnt);
    }
    g->matches += cnt;
    return 0;
}

static void grep_tree(tctx_t *c, const char *path, const char *shown, grep_ctx_t *g, int depth) {
    if (depth > 24) return;
    int total = vfs_list(path, NULL, 0);
    if (total <= 0) return;
    vfs_dirent_t *ents = malloc((size_t)total * sizeof *ents);
    if (!ents) return;
    total = MIN(total, vfs_list(path, ents, total));
    for (int k = 0; k < total; k++) {
        char sub[VFS_PATH_MAX], subshown[VFS_PATH_MAX];
        vfs_join(path, ents[k].name, sub);
        snprintf(subshown, sizeof subshown, "%s%s%s", shown, shown[strlen(shown) - 1] == '/' ? "" : "/", ents[k].name);
        if (ents[k].is_dir) { grep_tree(c, sub, subshown, g, depth + 1); continue; }
        uint32_t size;
        char *data = vfs_read_file(sub, &size);
        if (!data) continue;
        grep_one(c, subshown, data, size, g);
        free(data);
    }
    free(ents);
}

static int cmd_grep(tctx_t *c, int argc, char **argv) {
    bool f[256] = { 0 };
    int i = get_flags(c, argc, argv, "invcrRHh", f);
    if (i < 0) return 2;
    if (i >= argc) { tc_err(c, "usage: grep [-i] [-n] [-v] [-c] [-r] PATTERN [FILE...]"); return 2; }
    grep_ctx_t g = { argv[i], f['i'], f['n'], f['v'], f['c'], false, false, 0 };
    i++;
    if (f['r'] || f['R']) {
        g.names = !f['h'];
        const char *def[1] = { "." };
        char **dirs = i < argc ? argv + i : (char **)def;
        int nd = i < argc ? argc - i : 1;
        for (int k = 0; k < nd; k++) {
            char path[VFS_PATH_MAX];
            if (term_resolve(c->t, dirs[k], path) || !vfs_exists(path)) { tc_err(c, "grep: %s: No such file or directory", dirs[k]); continue; }
            if (vfs_is_dir(path)) grep_tree(c, path, dirs[k], &g, 0);
            else {
                uint32_t size;
                char *data = vfs_read_file(path, &size);
                if (data) { grep_one(c, dirs[k], data, size, &g); free(data); }
            }
        }
        return g.matches ? 0 : 1;
    }
    g.names = (argc - i > 1 || f['H']) && !f['h'];
    int st = for_each_input(c, argc, argv, i, grep_one, &g);
    if (st) return 2;
    return g.matches ? 0 : 1;
}

/* head / tail: -n N or -N */
static int parse_count(tctx_t *c, int argc, char **argv, int *n) {
    int i = 1;
    *n = 10;
    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            if (!parse_int(argv[i + 1], n) || *n < 0) { tc_err(c, "%s: bad line count '%s'", argv[0], argv[i + 1]); return -1; }
            i += 2;
        } else if (isdigit((unsigned char)argv[i][1])) {
            if (!parse_int(argv[i] + 1, n)) { tc_err(c, "%s: bad line count '%s'", argv[0], argv[i]); return -1; }
            i++;
        } else {
            tc_err(c, "usage: %s [-n N] [FILE...]", argv[0]);
            return -1;
        }
    }
    return i;
}

typedef struct { int n; bool tail; bool names; int nfile; } ht_ctx_t;
static int ht_one(tctx_t *c, const char *name, const char *data, uint32_t len, void *ctx) {
    ht_ctx_t *h = ctx;
    if (h->names && name) tc_printf(c, "%s%s==> %s <==" T_RESET "\n", h->nfile++ ? "\n" : "", col(c, T_BOLD), name);
    if (!h->tail) {
        liter_t it = { data, data + len };
        const char *ls;
        int ln, k = 0;
        while (k < h->n && next_line(&it, &ls, &ln)) { tc_write(c, ls, ln); tc_write(c, "\n", 1); k++; }
        return 0;
    }
    /* tail: find the start of the last n lines */
    uint32_t end = len;
    if (end > 0 && data[end - 1] == '\n') end--;
    int k = 0;
    uint32_t p = end;
    while (p > 0) {
        if (data[p - 1] == '\n' && ++k >= h->n) break;
        p--;
    }
    if (h->n == 0) return 0;
    tc_write(c, data + p, (int)(len - p));
    if (len > 0 && data[len - 1] != '\n') tc_write(c, "\n", 1);
    return 0;
}

static int cmd_head(tctx_t *c, int argc, char **argv) {
    ht_ctx_t h = { 10, false, false, 0 };
    int i = parse_count(c, argc, argv, &h.n);
    if (i < 0) return 2;
    h.names = argc - i > 1;
    return for_each_input(c, argc, argv, i, ht_one, &h);
}

static int cmd_tail(tctx_t *c, int argc, char **argv) {
    ht_ctx_t h = { 10, true, false, 0 };
    int i = parse_count(c, argc, argv, &h.n);
    if (i < 0) return 2;
    h.names = argc - i > 1;
    return for_each_input(c, argc, argv, i, ht_one, &h);
}

/* sort [-r] [-n] [-u] */
typedef struct { bool rev, num, uniq; char **lines; int n, cap; } sort_ctx_t;
static bool s_sort_num;
static int sort_cmp(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    if (s_sort_num) {
        double dx = atof(x), dy = atof(y);
        if (dx < dy) return -1;
        if (dx > dy) return 1;
    }
    return strcmp(x, y);
}
static int sort_collect(tctx_t *c, const char *name, const char *data, uint32_t len, void *ctx) {
    sort_ctx_t *s = ctx;
    liter_t it = { data, data + len };
    const char *ls;
    int ln;
    while (next_line(&it, &ls, &ln)) {
        if (s->n >= s->cap) {
            int nc = s->cap ? s->cap * 2 : 256;
            if (nc > 200000) return 1;
            char **nl = realloc(s->lines, (size_t)nc * sizeof(char *));
            if (!nl) return 1;
            s->lines = nl;
            s->cap = nc;
        }
        char *d = strndup(ls, (size_t)ln);
        if (!d) return 1;
        s->lines[s->n++] = d;
    }
    return 0;
}
static int cmd_sort(tctx_t *c, int argc, char **argv) {
    bool f[256] = { 0 };
    int i = get_flags(c, argc, argv, "rnu", f);
    if (i < 0) return 2;
    sort_ctx_t s = { f['r'], f['n'], f['u'], NULL, 0, 0 };
    int st = for_each_input(c, argc, argv, i, sort_collect, &s);
    s_sort_num = s.num;
    if (s.n) qsort(s.lines, (size_t)s.n, sizeof(char *), sort_cmp);
    for (int k = 0; k < s.n; k++) {
        int idx = s.rev ? s.n - 1 - k : k;
        int prev = s.rev ? idx + 1 : idx - 1;
        if (s.uniq && k > 0 && !strcmp(s.lines[idx], s.lines[prev])) continue;
        tc_puts(c, s.lines[idx]);
        tc_write(c, "\n", 1);
    }
    for (int k = 0; k < s.n; k++) free(s.lines[k]);
    free(s.lines);
    return st;
}

/* ======================================================================================
 * Built-in commands: system
 * ==================================================================================== */
static int cmd_date(tctx_t *c, int argc, char **argv) {
    datetime_t dt;
    sys_datetime(&dt);
    tc_printf(c, "%s, %d %s %d  %02d:%02d:%02d\n", sys_weekday_name(dt.weekday), dt.day, sys_month_name(dt.month),
              dt.year, dt.hour, dt.minute, dt.second);
    return 0;
}

static int cmd_time(tctx_t *c, int argc, char **argv) {
    datetime_t dt;
    sys_datetime(&dt);
    tc_printf(c, "%02d:%02d:%02d\n", dt.hour, dt.minute, dt.second);
    return 0;
}

static int days_in_month(int y, int m) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[(m - 1) % 12];
}

static int cmd_cal(tctx_t *c, int argc, char **argv) {
    datetime_t now;
    sys_datetime(&now);
    int m = now.month, y = now.year;
    if (argc == 2 && (!parse_int(argv[1], &y) || y < 1970 || y > 2100)) { tc_err(c, "usage: cal [MONTH YEAR]"); return 2; }
    if (argc == 2) m = 1;
    if (argc >= 3 && (!parse_int(argv[1], &m) || !parse_int(argv[2], &y) || m < 1 || m > 12 || y < 1970 || y > 2100)) {
        tc_err(c, "usage: cal [MONTH YEAR]");
        return 2;
    }
    datetime_t first = { y, m, 1, 12, 0, 0, 0 };
    uint32_t t0 = sys_datetime_to_time(&first);
    datetime_t f2;
    sys_time_to_datetime(t0, &f2);
    int wd = f2.weekday;
    char title[40];
    snprintf(title, sizeof title, "%s %d", sys_month_name(m), y);
    int pad = (20 - (int)strlen(title)) / 2;
    tc_printf(c, "%*s%s%s" T_RESET "\n", pad > 0 ? pad : 0, "", col(c, T_BOLD), title);
    tc_printf(c, "%sSu Mo Tu We Th Fr Sa" T_RESET "\n", col(c, T_GRAY));
    for (int k = 0; k < wd; k++) tc_puts(c, "   ");
    int dim = days_in_month(y, m);
    for (int d = 1; d <= dim; d++) {
        bool today = d == now.day && m == now.month && y == now.year;
        if (today && c->out->tty) tc_printf(c, "\x1b[7m%2d" T_RESET, d);
        else tc_printf(c, "%2d", d);
        if ((wd + d) % 7 == 0) tc_write(c, "\n", 1);
        else if (d < dim) tc_write(c, " ", 1);
    }
    if ((wd + dim) % 7) tc_write(c, "\n", 1);
    return 0;
}

static void fmt_uptime(char *out, int size) {
    uint32_t s = sys_uptime_ms() / 1000;
    uint32_t d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60;
    if (d) snprintf(out, (size_t)size, "%u day%s, %u:%02u", d, d == 1 ? "" : "s", h, m);
    else if (h) snprintf(out, (size_t)size, "%u:%02u", h, m);
    else snprintf(out, (size_t)size, "%u min, %u sec", m, s % 60);
}

static int cmd_uptime(tctx_t *c, int argc, char **argv) {
    datetime_t dt;
    sys_datetime(&dt);
    char up[48];
    fmt_uptime(up, sizeof up);
    int nw = win_list(NULL, 0);
    tc_printf(c, " %02d:%02d:%02d up %s,  %d window%s,  %d fps\n", dt.hour, dt.minute, dt.second, up, nw, nw == 1 ? "" : "s", sys_fps());
    return 0;
}

static void bar(tctx_t *c, int width, uint32_t used, uint32_t total) {
    int fill = total ? (int)((uint64_t)used * (uint32_t)width / total) : 0;
    if (used && !fill) fill = 1;
    int pct = total ? (int)((uint64_t)used * 100 / total) : 0;
    const char *color = pct > 85 ? T_BRED : pct > 60 ? T_BYELLOW : T_BGREEN;
    tc_printf(c, "%s", col(c, color));
    for (int k = 0; k < width; k++) tc_puts(c, k < fill ? "\x8B" : (c->out->tty ? T_GRAY "\x8E" : "."));
    tc_printf(c, "%s %3d%%", col(c, T_RESET), pct);
}

static int cmd_free(tctx_t *c, int argc, char **argv) {
    bool kib = argc > 1 && !strcmp(argv[1], "-k");
    uint32_t total = sys_mem_total(), heap = sys_heap_size(), used = sys_mem_used(), hfree = sys_mem_free();
    uint32_t div = kib ? 1024 : 1024 * 1024;
    const char *unit = kib ? "KiB" : "MiB";
    tc_printf(c, "%s%-8s %10s %10s %10s" T_RESET "\n", col(c, T_GRAY), "", "total", "used", "free");
    uint32_t ram_used = total > hfree ? total - hfree : 0;
    tc_printf(c, "%-8s %6u %s %6u %s %6u %s\n", "Mem:", total / div, unit, ram_used / div, unit, hfree / div, unit);
    tc_printf(c, "%-8s %6u %s %6u %s %6u %s\n", "Heap:", heap / div, unit, used / div, unit, hfree / div, unit);
    char fsz[16];
    uint32_t fbytes = vfs_total_bytes();
    if (kib) snprintf(fsz, sizeof fsz, "%6u KiB", fbytes / 1024);
    else if (fbytes < 1024 * 1024) snprintf(fsz, sizeof fsz, "%6u KiB", fbytes / 1024);
    else snprintf(fsz, sizeof fsz, "%6.1f MiB", fbytes / (1024.0 * 1024.0));
    tc_printf(c, "%-8s %s  %s(in the RAM disk)" T_RESET "\n", "Files:", fsz, col(c, T_GRAY));
    tc_puts(c, "\nheap  ");
    bar(c, 30, used, heap ? heap : 1);
    tc_write(c, "\n", 1);
    return 0;
}

/* ---- disks and persistence (<nova/disk.h>) -------------------------------------------- */
static void ago(uint32_t then, char *out, int size) {
    uint32_t now = sys_time(), d = now > then ? now - then : 0;
    if (!then) snprintf(out, (size_t)size, "never");
    else if (d < 2) snprintf(out, (size_t)size, "just now");
    else if (d < 120) snprintf(out, (size_t)size, "%u seconds ago", d);
    else if (d < 7200) snprintf(out, (size_t)size, "%u minutes ago", d / 60);
    else snprintf(out, (size_t)size, "%u hours ago", d / 3600);
}

static int cmd_sync(tctx_t *c, int argc, char **argv) {
    if (!persist_active()) {
        tc_printf(c, "%sNothing to sync:%s NovaOS is running from the CD, so your files live in memory only.\n",
                  col(c, T_BYELLOW), col(c, T_RESET));
        tc_printf(c, "%sInstall NovaOS to a hard disk (Installer app, or 'open Installer') to keep them across restarts.%s\n",
                  col(c, T_GRAY), col(c, T_RESET));
        return 0;
    }
    persist_info_t before;
    persist_get_info(&before);
    uint32_t t0 = sys_uptime_ms();
    int r = persist_save();
    uint32_t ms = sys_uptime_ms() - t0;
    if (r != DISK_OK) {
        tc_err(c, "sync: could not save to disk %d: %s", before.disk, disk_strerror(r));
        return 1;
    }
    persist_info_t pi;
    persist_get_info(&pi);
    char sz[16];
    human_size(pi.used, sz, sizeof sz);
    if (!before.dirty && !before.saving)
        tc_printf(c, "%sAlready saved:%s all files are on disk %d (%sB).\n", col(c, T_BGREEN), col(c, T_RESET), pi.disk, sz);
    else
        tc_printf(c, "%sSaved:%s all files written to disk %d (%sB in %u ms).\n", col(c, T_BGREEN), col(c, T_RESET), pi.disk, sz, ms);
    return 0;
}

static int cmd_df(tctx_t *c, int argc, char **argv) {
    bool f[256] = { 0 };
    if (get_flags(c, argc, argv, "hk", f) < 0) return 2;
    bool human = f['h'] || !f['k'];
    persist_info_t pi;
    persist_get_info(&pi);
    uint32_t files = vfs_total_bytes();
    /* the RAM disk can grow until the heap is full */
    uint32_t ram_size = files + sys_mem_free();
    char a[16], b[16], d[16];
    tc_printf(c, "%s%-14s %8s %8s %8s %5s  %s" T_RESET "\n", col(c, T_GRAY), "Filesystem", human ? "Size" : "1K-blocks",
              "Used", "Avail", "Use%", "Mounted on");
#define DF_ROW(name, size, used, mnt) do { \
        uint32_t s_ = (size), u_ = MIN((used), s_), v_ = s_ - u_; \
        if (human) { human_size(s_, a, sizeof a); human_size(u_, b, sizeof b); human_size(v_, d, sizeof d); } \
        else { snprintf(a, sizeof a, "%u", s_ / 1024); snprintf(b, sizeof b, "%u", u_ / 1024); snprintf(d, sizeof d, "%u", v_ / 1024); } \
        tc_printf(c, "%-14s %8s %8s %8s %4u%%  %s\n", (name), a, b, d, s_ ? (unsigned)((uint64_t)u_ * 100 / s_) : 0u, (mnt)); \
    } while (0)
    DF_ROW("ramfs", ram_size, files, "/");
    if (pi.active) {
        char dev[16];
        snprintf(dev, sizeof dev, "disk%d", pi.disk);
        DF_ROW(dev, pi.capacity, pi.used, "(saved copy of /)");
    }
#undef DF_ROW
    tc_write(c, "\n", 1);
    if (pi.active) {
        char when[32];
        ago(pi.last_save, when, sizeof when);
        tc_printf(c, "Auto-saved to disk %d; last save %s%s.\n", pi.disk, when,
                  pi.saving ? " (saving now)" : pi.dirty ? " (changes pending)" : "");
    } else {
        tc_printf(c, "%sRunning from the CD: files are kept in RAM and lost at restart (see 'disks').%s\n", col(c, T_GRAY), col(c, T_RESET));
    }
    return 0;
}

static int cmd_disks(tctx_t *c, int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-r")) disk_rescan();
    int n = disk_count();
    if (!n) {
        tc_puts(c, "No hard disks found.\n");
        tc_printf(c, "%sNovaOS sees IDE disks only: in VirtualBox add a disk to the IDE controller (64 MB or more).%s\n",
                  col(c, T_GRAY), col(c, T_RESET));
        return 0;
    }
    tc_printf(c, "%s%-4s %-16s %-20s %8s  %s" T_RESET "\n", col(c, T_GRAY), "DISK", "LOCATION", "MODEL", "SIZE", "STATUS");
    for (int i = 0; i < n; i++) {
        const disk_info_t *di = disk_get(i);
        if (!di) continue;
        char sz[16];
        disk_format_size(di->sectors * DISK_SECTOR_SIZE, sz, sizeof sz);
        const char *status, *sc;
        if (install_is_boot_disk(i)) { status = "running NovaOS (boot disk)"; sc = T_BGREEN; }
        else if (install_disk_has_novaos(i)) { status = "NovaOS installed"; sc = T_BCYAN; }
        else if (di->sectors * DISK_SECTOR_SIZE < INSTALL_MIN_BYTES) { status = "too small to install"; sc = T_GRAY; }
        else { status = "free for the Installer"; sc = T_GRAY; }
        tc_printf(c, "%-4d %-16s %-20.20s %8s  %s%s%s\n", i, di->location ? di->location : "?", di->model, sz,
                  col(c, sc), status, col(c, T_RESET));
    }
    return 0;
}

static int cmd_ps(tctx_t *c, int argc, char **argv) {
    window_t *list[128];
    int n = MIN(win_list(list, 128), 128);
    tc_printf(c, "%s%5s  %-16s %-11s %-10s %s" T_RESET "\n", col(c, T_GRAY), "ID", "APP", "STATE", "SIZE", "TITLE");
    window_t *focus = win_get_focused();
    for (int i = n - 1; i >= 0; i--) {
        window_t *w = list[i];
        const app_desc_t *a = win_get_app(w);
        const char *state = win_is_minimized(w) ? "minimized" : w == focus ? "focused" : win_is_maximized(w) ? "maximized" : "open";
        char size[16];
        snprintf(size, sizeof size, "%dx%d", win_width(w), win_height(w));
        bool me = w == c->t->win;
        tc_printf(c, "%s%5d%s  %-16.16s %s%-11s%s %-10s %s%s\n", col(c, T_BOLD), win_id(w), col(c, T_RESET),
                  a && a->name ? a->name : "-", col(c, w == focus ? T_GREEN : (win_is_minimized(w) ? T_GRAY : "")), state,
                  col(c, T_RESET), size, win_get_title(w) ? win_get_title(w) : "", me ? (c->out->tty ? T_GRAY "  (this terminal)" T_RESET : "  (this terminal)") : "");
    }
    tc_printf(c, "%s%d window%s" T_RESET "\n", col(c, T_GRAY), n, n == 1 ? "" : "s");
    return 0;
}

static int cmd_kill(tctx_t *c, int argc, char **argv) {
    if (argc < 2) { tc_err(c, "usage: kill WINDOW_ID...   (see ps)"); return 2; }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-9") || !strcmp(a, "-KILL") || !strcmp(a, "-TERM")) continue;
        int id;
        if (!parse_int(a, &id)) { tc_err(c, "kill: %s: not a window id", a); status = 1; continue; }
        window_t *w = win_find_by_id(id);
        if (!w || !win_is_valid(w)) { tc_err(c, "kill: (%d) - no such window", id); status = 1; continue; }
        char title[64];
        strlcpy(title, win_get_title(w) ? win_get_title(w) : "", sizeof title);
        if (w == c->t->win) {
            tc_printf(c, "closing this terminal...\n");
            term_request_close(c->t);
        } else {
            win_close(w);
            tc_printf(c, "closed window %d (%s)\n", id, title);
        }
    }
    return status;
}

static int cmd_killall(tctx_t *c, int argc, char **argv) {
    if (argc < 2) { tc_err(c, "usage: killall APP"); return 2; }
    char name[96] = "";
    for (int i = 1; i < argc; i++) { if (i > 1) strlcat(name, " ", sizeof name); strlcat(name, argv[i], sizeof name); }
    const app_desc_t *app = app_find(name);
    if (!app) { tc_err(c, "killall: no app named '%s'", name); return 1; }
    window_t *list[128];
    int n = MIN(win_list(list, 128), 128), closed = 0;
    bool self = false;
    for (int i = 0; i < n; i++) {
        if (win_get_app(list[i]) != app) continue;
        if (list[i] == c->t->win) { self = true; continue; }
        win_close(list[i]);
        closed++;
    }
    tc_printf(c, "closed %d window%s of %s\n", closed, closed == 1 ? "" : "s", app->name);
    if (self) term_request_close(c->t);
    return closed || self ? 0 : 1;
}

static int cmd_apps(tctx_t *c, int argc, char **argv) {
    const char *cats[32];
    int nc = app_categories(cats, 32);
    int total = 0;
    for (int k = 0; k <= nc; k++) {
        bool header = false;
        for (int i = 0; i < app_count(); i++) {
            const app_desc_t *a = app_get(i);
            if (!a || !a->name) continue;
            bool hidden = (a->flags & APP_HIDDEN) != 0;
            bool in_cat = k < nc ? (!hidden && a->category && !strcmp(a->category, cats[k])) : hidden;
            if (!in_cat) continue;
            if (!header) {
                tc_printf(c, "%s%s%s" T_RESET "\n", total ? "\n" : "", col(c, "\x1b[1;96m"), k < nc ? cats[k] : "Hidden");
                header = true;
            }
            tc_printf(c, "  %s%-18s" T_RESET " %s%s" T_RESET "\n", col(c, T_BOLD), a->name, col(c, T_GRAY), a->description ? a->description : "");
            total++;
        }
    }
    tc_printf(c, "\n%s%d apps. Start one with " T_RESET "%sopen <name>%s or just type its name." T_RESET "\n",
              col(c, T_GRAY), total, col(c, T_BOLD), col(c, T_RESET T_GRAY));
    return 0;
}

/* Launch an app; extra arguments are joined (paths resolved to absolute). */
static int launch_app(tctx_t *c, const app_desc_t *app, int argc, char **argv, int first) {
    char arg[VFS_PATH_MAX * 2] = "";
    if (first < argc) {
        if (argc - first == 1) {
            char path[VFS_PATH_MAX], parent[VFS_PATH_MAX];
            if (argv[first][0] != '-' && term_resolve(c->t, argv[first], path) == 0) {
                vfs_dirname(path, parent);
                if (vfs_exists(path) || (strchr(argv[first], '.') && vfs_is_dir(parent))) strlcpy(arg, path, sizeof arg);
            }
        }
        if (!arg[0])
            for (int i = first; i < argc; i++) { if (i > first) strlcat(arg, " ", sizeof arg); strlcat(arg, argv[i], sizeof arg); }
    }
    /* a new Terminal started from this one opens in the same directory */
    if (!arg[0] && app->name && !strcmp(app->name, "Terminal")) strlcpy(arg, c->t->cwd, sizeof arg);
    if (!app_launch(app, arg[0] ? arg : NULL)) { tc_err(c, "open: could not start %s", app->name); return 1; }
    tc_printf(c, "%sstarted " T_RESET "%s%s%s%s%s" T_RESET "\n", col(c, T_GRAY), col(c, T_BOLD), app->name, col(c, T_RESET T_GRAY),
              arg[0] ? " " : "", arg);
    return 0;
}

static int cmd_open(tctx_t *c, int argc, char **argv) {
    const char *usage = "usage: %s APP [ARG] | %s FILE|DIR [-w APP]   (see apps)";
    if (argc < 2) { tc_err(c, usage, argv[0], argv[0]); return 2; }
    /* open FILE -w APP / open -w APP FILE: a specific app for a file */
    const char *with = NULL, *target = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w") && i + 1 < argc) with = argv[++i];
        else if (!target) target = argv[i];
    }
    if (with) {
        const app_desc_t *wa = app_find(with);
        if (!wa) { tc_err(c, "%s: no app named '%s' (try " T_BOLD "apps" T_RESET T_BRED ")", argv[0], with); return 1; }
        if (!target) return launch_app(c, wa, 0, NULL, 0);
        char *av[2] = { argv[0], (char *)target };
        return launch_app(c, wa, 2, av, 1);
    }
    const app_desc_t *app = app_find(argv[1]);
    if (app) return launch_app(c, app, argc, argv, 2);
    /* "open Widget Gallery" */
    char name[96] = "";
    for (int i = 1; i < argc; i++) { if (i > 1) strlcat(name, " ", sizeof name); strlcat(name, argv[i], sizeof name); }
    app = app_find(name);
    if (app) return launch_app(c, app, 0, NULL, 0);
    char path[VFS_PATH_MAX];
    if (term_resolve(c->t, argv[1], path) == 0 && vfs_exists(path)) {
        const app_desc_t *a = assoc_default_app(path);
        if (a) { char *av[2] = { argv[0], path }; return launch_app(c, a, 2, av, 1); }
        if (vfs_is_dir(path)) { tc_err(c, "%s: no file manager is installed", argv[0]); return 1; }
        /* no default app: the system Open With dialog lets the user pick one */
        if (!shell_open(path)) { tc_err(c, "%s: no app can open '%s'", argv[0], argv[1]); return 1; }
        tc_printf(c, "%schoose an app for %s%s%s in the Open With window" T_RESET "\n", col(c, T_GRAY),
                  col(c, T_RESET T_BOLD), vfs_basename(path), col(c, T_RESET T_GRAY));
        return 0;
    }
    tc_err(c, "%s: no app or file named '%s' (try " T_BOLD "apps" T_RESET T_BRED ")", argv[0], name);
    return 1;
}

static int cmd_whoami(tctx_t *c, int argc, char **argv) { tc_puts(c, TERM_USER "\n"); return 0; }
static int cmd_hostname(tctx_t *c, int argc, char **argv) { tc_puts(c, TERM_HOST "\n"); return 0; }

static int cmd_uname(tctx_t *c, int argc, char **argv) {
    bool f[256] = { 0 };
    int i = get_flags(c, argc, argv, "asnrvmo", f);
    if (i < 0) return 2;
    if (f['a']) f['s'] = f['n'] = f['r'] = f['v'] = f['m'] = f['o'] = true;
    if (!f['s'] && !f['n'] && !f['r'] && !f['v'] && !f['m'] && !f['o']) f['s'] = true;
    char rel[64];
    strlcpy(rel, sys_kernel_version(), sizeof rel);
    char *sp = strchr(rel, ' ');
    const char *build = sp ? sp + 1 : "";
    if (sp) *sp = 0;
    const char *parts[6];
    int n = 0;
    if (f['s']) parts[n++] = "NovaOS";
    if (f['n']) parts[n++] = TERM_HOST;
    if (f['r']) parts[n++] = rel;
    if (f['v']) parts[n++] = *build ? build : "#1";
    if (f['m']) parts[n++] = "i686";
    if (f['o']) parts[n++] = "NovaOS";
    for (int k = 0; k < n; k++) tc_printf(c, "%s%s", parts[k], k < n - 1 ? " " : "\n");
    return 0;
}

static int cmd_dmesg(tctx_t *c, int argc, char **argv) {
    int lines = 0;
    if (argc >= 3 && !strcmp(argv[1], "-n")) parse_int(argv[2], &lines);
    int size = 64 * 1024;
    char *buf = malloc((size_t)size);
    if (!buf) return 1;
    int n = sys_log_text(buf, size);
    if (n < 0) n = 0;
    if (n >= size) n = size - 1;
    buf[n] = 0;
    const char *start = buf;
    if (lines > 0) {
        int k = 0;
        const char *p = buf + n;
        while (p > buf) { if (p[-1] == '\n' && ++k > lines) break; p--; }
        start = p;
    }
    liter_t it = { start, buf + n };
    const char *ls;
    int ln;
    while (next_line(&it, &ls, &ln)) {
        /* "[   1.234] message": dim the timestamp */
        const char *rb = ln > 0 && ls[0] == '[' ? memchr(ls, ']', (size_t)ln) : NULL;
        if (rb) {
            int tl = (int)(rb - ls) + 1;
            tc_printf(c, "%s", col(c, T_GREEN));
            tc_write(c, ls, tl);
            tc_printf(c, "%s", col(c, T_RESET));
            tc_write(c, ls + tl, ln - tl);
        } else {
            tc_write(c, ls, ln);
        }
        tc_write(c, "\n", 1);
    }
    free(buf);
    return 0;
}

static int cmd_history(tctx_t *c, int argc, char **argv) {
    term_t *t = c->t;
    if (argc > 1 && !strcmp(argv[1], "-c")) { term_history_clear(t); return 0; }
    int n = t->hist_n, from = 0;
    int want;
    if (argc > 1 && parse_int(argv[1], &want) && want >= 0 && want < n) from = n - want;
    for (int i = from; i < n; i++) tc_printf(c, "%s%5d" T_RESET "  %s\n", col(c, T_GRAY), i + 1, t->hist[i]);
    return 0;
}

static int cmd_clear(tctx_t *c, int argc, char **argv) {
    if (c->out->tty) term_clear(c->t);
    else tc_puts(c, "\x1b[2J");
    return 0;
}

/* "color check": which colour depth the screen is really running at. */
static int color_check(tctx_t *c) {
    display_info_t d;
    sys_display_info(&d);
    const char *b = col(c, T_BOLD), *g = col(c, T_GRAY);
    const char *depth = d.bpp == 32 ? "true colour, 16.7 million colours + 8 spare bits"
                      : d.bpp == 24 ? "true colour, 16.7 million colours"
                      : d.bpp == 16 ? "high colour, 65,536 colours"
                      : "unknown";
    const char *by = !strcmp(d.source, "bga") ? "NovaOS's own Bochs/BGA display driver"
                   : sys_cmdline_get("boot") && !strcmp(sys_cmdline_get("boot"), "disk")
                       ? "NovaOS disk loader (VESA VBE)" : "GRUB (VESA VBE)";
    tc_printf(c, "%sDisplay colour check" T_RESET "\n", b);
    tc_printf(c, "  %sColour bits:%s  %s%d-bit" T_RESET " (%s)\n", g, T_RESET, col(c, T_BGREEN), d.bpp, depth);
    tc_printf(c, "  %sResolution:%s   %d x %d\n", g, T_RESET, d.width, d.height);
    if (d.bpp >= 24)
        tc_printf(c, "  %sPixel layout:%s %s 8:8:8 (red bits %d-%d, green %d-%d, blue %d-%d)\n", g, T_RESET,
                  d.bpp == 32 ? "XRGB" : "RGB", d.r_pos, d.r_pos + 7, d.g_pos, d.g_pos + 7, d.b_pos, d.b_pos + 7);
    else
        tc_printf(c, "  %sPixel layout:%s RGB 5:6:5\n", g, T_RESET);
    tc_printf(c, "  %sBytes/row:%s    %d\n", g, T_RESET, d.pitch);
    tc_printf(c, "  %sMode set by:%s  %s\n", g, T_RESET, by);
    tc_printf(c, "  %sNovaOS draws in 32-bit ARGB and converts when it copies to the screen." T_RESET "\n", g);
    return 0;
}

static int cmd_color(tctx_t *c, int argc, char **argv) {
    term_t *t = c->t;
    int n = term_palette_count();
    if (argc < 2) {
        tc_printf(c, "%sThemes" T_RESET " (use " T_BOLD "color <name>" T_RESET "):\n", col(c, T_BOLD));
        for (int i = 0; i < n; i++) {
            const tpalette_t *p = term_palette(i);
            tc_printf(c, "  %s%c %-10s" T_RESET " %s%s" T_RESET "\n", col(c, i == t->theme ? T_BGREEN : ""),
                      i == t->theme ? '*' : ' ', p->name, col(c, T_GRAY), p->desc);
        }
        if (c->out->tty) {
            tc_puts(c, "\n  ");
            for (int i = 0; i < 8; i++) tc_printf(c, "\x1b[%dm\x8B\x8B\x8B", 30 + i);
            tc_puts(c, T_RESET "\n  ");
            for (int i = 0; i < 8; i++) tc_printf(c, "\x1b[%dm\x8B\x8B\x8B", 90 + i);
            tc_puts(c, T_RESET "\n");
        }
        return 0;
    }
    if (!strcasecmp(argv[1], "check")) return color_check(c);
    int idx = -1;
    if (!strcmp(argv[1], "next")) idx = (t->theme + 1) % n;
    else for (int i = 0; i < n; i++) if (!strcasecmp(term_palette(i)->name, argv[1])) idx = i;
    if (idx < 0) { tc_err(c, "color: unknown theme '%s' (run color to list them)", argv[1]); return 1; }
    term_set_theme(t, idx);
    tc_printf(c, "theme set to %s%s" T_RESET "\n", col(c, T_BOLD), term_palette(idx)->name);
    return 0;
}

static int cmd_beep(tctx_t *c, int argc, char **argv) {
    int freq = 880, ms = 150;
    if (argc > 1 && (!parse_int(argv[1], &freq) || freq < 20 || freq > 20000)) { tc_err(c, "beep: frequency must be 20..20000 Hz"); return 2; }
    if (argc > 2 && (!parse_int(argv[2], &ms) || ms < 1 || ms > 5000)) { tc_err(c, "beep: duration must be 1..5000 ms"); return 2; }
    if (!sys_sound_enabled()) tc_printf(c, "%s(sound is muted - click the speaker icon in the taskbar)" T_RESET "\n", col(c, T_GRAY));
    sys_beep((uint32_t)freq, (uint32_t)ms);
    return 0;
}

static void power_cb(void *ctx) {
    if ((intptr_t)ctx == 1) sys_reboot();
    else sys_shutdown();
}

static int cmd_reboot(tctx_t *c, int argc, char **argv) {
    bool reboot = !strcmp(argv[0], "reboot");
    term_printf(c->t, "\x1b[1;93m%s...\x1b[0m\n", reboot ? "Rebooting" : "Shutting down");
    timer_add(600, power_cb, (void *)(intptr_t)(reboot ? 1 : 0), false);
    return 0;
}

static int cmd_env(tctx_t *c, int argc, char **argv) {
    term_t *t = c->t;
    tc_printf(c, "%sPWD%s=%s\n", col(c, T_CYAN), col(c, T_RESET), t->cwd);
    tc_printf(c, "%sOLDPWD%s=%s\n", col(c, T_CYAN), col(c, T_RESET), t->oldpwd);
    for (int i = 0; i < t->nvars; i++) tc_printf(c, "%s%s%s=%s\n", col(c, T_CYAN), t->vars[i].name, col(c, T_RESET), t->vars[i].value);
    return 0;
}

static int cmd_export(tctx_t *c, int argc, char **argv) {
    if (argc < 2) return cmd_env(c, argc, argv);
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        int nl = eq ? (int)(eq - argv[i]) : (int)strlen(argv[i]);
        if (!valid_var_name(argv[i], nl)) { tc_err(c, "export: '%s': not a valid name", argv[i]); status = 1; continue; }
        if (!eq) continue;
        *eq = 0;
        if (var_set(c->t, argv[i], eq + 1)) { tc_err(c, "export: cannot set %s", argv[i]); status = 1; }
    }
    return status;
}

static int cmd_unset(tctx_t *c, int argc, char **argv) {
    for (int i = 1; i < argc; i++) var_unset(c->t, argv[i]);
    return 0;
}

static int cmd_which(tctx_t *c, int argc, char **argv) {
    if (argc < 2) { tc_err(c, "usage: which COMMAND..."); return 2; }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        const app_desc_t *a;
        if (find_cmd(argv[i])) tc_printf(c, "%s: shell built-in\n", argv[i]);
        else if (command_find(argv[i])) tc_printf(c, "%s: system command\n", argv[i]);
        else if ((a = app_find(argv[i]))) tc_printf(c, "%s: application \"%s\" (%s)\n", argv[i], a->name, a->category ? a->category : "-");
        else { tc_err(c, "which: %s: not found", argv[i]); status = 1; }
    }
    return status;
}

static int cmd_sh(tctx_t *c, int argc, char **argv) {
    if (argc < 2) { tc_err(c, "usage: sh SCRIPT"); return 2; }
    /* the script's commands are queued and run after this pipeline, so their
       output cannot be piped or redirected */
    if (!c->out->tty || c->has_in) { tc_err(c, "%s: scripts cannot be used in pipes or redirections", argv[0]); return 1; }
    uint32_t size;
    char *data = read_text(c, argv[0], argv[1], &size);
    if (!data) return 1;
    if (is_binary(data, size)) { tc_err(c, "%s: %s: not a text file", argv[0], argv[1]); free(data); return 1; }
    /* queue the script's lines in front of whatever is still pending */
    tseg_t *head = NULL, **tail = &head;
    liter_t it = { data, data + size };
    const char *ls;
    int ln;
    while (next_line(&it, &ls, &ln)) {
        char line[TERM_INPUT_MAX];
        int n = MIN(ln, TERM_INPUT_MAX - 1);
        memcpy(line, ls, (size_t)n);
        line[n] = 0;
        tseg_t *segs = split_segments(line);
        while (segs) {
            *tail = segs;
            tail = &segs->next;
            segs = segs->next;
        }
    }
    free(data);
    *tail = c->t->queue;
    c->t->queue = head;
    return 0;
}

static int cmd_exit(tctx_t *c, int argc, char **argv) {
    term_request_close(c->t);
    return 0;
}

static int cmd_true(tctx_t *c, int argc, char **argv) { return 0; }
static int cmd_false(tctx_t *c, int argc, char **argv) { return 1; }

static int cmd_help(tctx_t *c, int argc, char **argv) {
    if (argc > 1) {
        const tcmd_t *cmd = find_cmd(argv[1]);
        const command_desc_t *rc = cmd ? NULL : command_find(argv[1]);
        if (rc) {
            const char *h = rc->help ? rc->help : "", *dash = strstr(h, " - ");
            if (dash) tc_printf(c, "%susage:%s %.*s\n       %s\n", col(c, T_GRAY), col(c, T_RESET), (int)(dash - h), h, dash + 3);
            else tc_printf(c, "%s\n", h);
            return 0;
        }
        if (!cmd) {
            const app_desc_t *a = app_find(argv[1]);
            if (a) { tc_printf(c, "%s%s%s is an application: %s\n", col(c, T_BOLD), a->name, col(c, T_RESET), a->description ? a->description : ""); return 0; }
            tc_err(c, "help: no command named '%s'", argv[1]);
            return 1;
        }
        tc_printf(c, "%susage:%s %s\n", col(c, T_GRAY), col(c, T_RESET), cmd->usage);
        tc_printf(c, "       %s\n", cmd->help);
        return 0;
    }
    tc_printf(c, "%s\x99 nsh %s%s - the NovaOS shell. Built-in commands:\n\n", col(c, "\x1b[1;94m"), TERM_SHELL_VER, col(c, T_RESET));
    int cols = MAX(40, tc_term_cols(c) - 1);
    for (int g = 0; g < GRP_COUNT; g++) {
        tc_printf(c, "%s%-9s" T_RESET, col(c, "\x1b[1;96m"), s_group_names[g]);
        int x = 9;
        for (int i = 0; i < cmd_count(); i++) {
            const tcmd_t *cmd = cmd_get(i);
            if (cmd->group != g) continue;
            int w = (int)strlen(cmd->name) + 2;
            if (x + w > cols && x > 9) { tc_puts(c, "\n         "); x = 9; }
            tc_printf(c, "%s%s" T_RESET "  ", col(c, T_BOLD), cmd->name);
            x += w;
        }
        tc_write(c, "\n", 1);
    }
    /* system commands from the registry (REGISTER_COMMAND) not shadowed by a built-in */
    tc_printf(c, "%s%-9s" T_RESET, col(c, "\x1b[1;96m"), "Desktop");
    int x = 9;
    for (int i = 0; i < command_count(); i++) {
        const command_desc_t *rc = command_get(i);
        if (find_cmd(rc->name)) continue;
        int w = (int)strlen(rc->name) + 2;
        if (x + w > cols && x > 9) { tc_puts(c, "\n         "); x = 9; }
        tc_printf(c, "%s%s" T_RESET "  ", col(c, T_BOLD), rc->name);
        x += w;
    }
    tc_write(c, "\n", 1);
    const char *g = col(c, T_GRAY), *b = col(c, T_RESET T_BOLD), *r = col(c, T_RESET T_GRAY);
    tc_printf(c, "\n%sType %shelp <command>%s for details. Apps start by name (%sapps%s lists them)." T_RESET "\n", g, b, r, b, r);
    tc_printf(c, "%sSyntax:  a | b   > file   >> file   < file   a ; b   a && b   a || b" T_RESET "\n", g);
    tc_printf(c, "%s         $VAR   ~   *.txt   !!   !N   'quotes'   # comment" T_RESET "\n", g);
    tc_printf(c, "%sKeys:    Tab completes, Up/Down history, Ctrl+C cancels, Ctrl+L clears," T_RESET "\n", g);
    tc_printf(c, "%s         Shift+PgUp scrolls, drag selects + copies, right-click menu." T_RESET "\n", g);
    return 0;
}

/* ---- sleep & timer: asynchronous jobs -------------------------------------------- */
static bool sleep_tick(term_t *t) { return sys_uptime_ms() - t->job.start < t->job.duration; }

static bool parse_seconds(const char *s, uint32_t *ms) {
    char *end;
    double v = strtod(s, &end);
    if (end == s) return false;
    if (*end == 's') end++;
    else if (*end == 'm') { v *= 60; end++; }
    if (*end || v < 0 || v > 86400) return false;
    *ms = (uint32_t)(v * 1000.0 + 0.5);
    return true;
}

static int cmd_sleep(tctx_t *c, int argc, char **argv) {
    uint32_t ms;
    if (argc < 2 || !parse_seconds(argv[1], &ms)) { tc_err(c, "usage: sleep SECONDS"); return 2; }
    if (!c->out->tty || ms == 0) return 0;          /* in a pipe: nothing to wait for */
    term_job_start(c->t, 50, sleep_tick, NULL, NULL);
    c->t->job.duration = ms;
    return 0;
}

static bool timer_tick(term_t *t) {
    uint32_t el = sys_uptime_ms() - t->job.start, dur = t->job.duration;
    if (el > dur) el = dur;
    const int W = MAX(10, MIN(40, t->cols - 24));
    int fill = dur ? (int)((uint64_t)el * (uint32_t)W / dur) : W;
    char buf[256];
    int o = snprintf(buf, sizeof buf, "\r\x1b[K\x1b[1;96m\x95" T_RESET " [\x1b[92m");
    for (int k = 0; k < W && o < (int)sizeof buf - 40; k++) {
        if (k == fill) o += snprintf(buf + o, sizeof buf - (size_t)o, "\x1b[90m");
        buf[o++] = k < fill ? '\x8B' : '\x8E';
    }
    uint32_t left = dur - el;
    o += snprintf(buf + o, sizeof buf - (size_t)o, T_RESET "] %u.%us left ", left / 1000, (left % 1000) / 100);
    term_write(t, buf, o);
    if (el >= dur) {
        term_printf(t, "\r\x1b[K\x1b[1;92m\x96" T_RESET " Time's up! (%u.%u s)\n", dur / 1000, (dur % 1000) / 100);
        static const note_t ding[] = { { 1047, 120 }, { 0, 40 }, { 1319, 120 }, { 0, 40 }, { 1568, 220 } };
        sys_play_notes(ding, ARRAY_SIZE(ding));
        ui_notify("Terminal", "Timer finished");
        return false;
    }
    return true;
}

static int cmd_timer(tctx_t *c, int argc, char **argv) {
    uint32_t ms;
    if (argc < 2 || !parse_seconds(argv[1], &ms) || ms == 0) { tc_err(c, "usage: timer SECONDS   (e.g. timer 10, timer 1.5m)"); return 2; }
    if (!c->out->tty) { tc_err(c, "timer: needs the terminal"); return 1; }
    term_job_start(c->t, 100, timer_tick, NULL, NULL);
    c->t->job.duration = ms;
    timer_tick(c->t);
    return 0;
}

/* ======================================================================================
 * The command table
 * ==================================================================================== */
static const tcmd_t s_cmds[] = {
    /* files */
    { "ls", cmd_ls, GRP_FILES, "ls [-l] [-a] [-h] [-1] [PATH...]", "List directory contents (-l long format, -a show hidden files, -h human sizes)." },
    { "cd", cmd_cd, GRP_FILES, "cd [DIR | - | ~]", "Change the current directory (no argument: home, '-': previous directory)." },
    { "pwd", cmd_pwd, GRP_FILES, "pwd", "Print the current directory." },
    { "cat", cmd_cat, GRP_FILES, "cat [-n] [FILE...]", "Print files (or standard input); -n numbers the lines." },
    { "mkdir", cmd_mkdir, GRP_FILES, "mkdir [-p] DIR...", "Create directories (-p: create parents too, no error if it exists)." },
    { "rmdir", cmd_rmdir, GRP_FILES, "rmdir DIR...", "Remove empty directories." },
    { "rm", cmd_rm, GRP_FILES, "rm [-r] [-f] [-v] PATH...", "Remove files (-r: directories and their contents, -f: ignore missing files)." },
    { "touch", cmd_touch, GRP_FILES, "touch FILE...", "Create empty files or update their modification time." },
    { "cp", cmd_cp, GRP_FILES, "cp [-r] [-v] SOURCE... DEST", "Copy files (-r: copy directories recursively)." },
    { "mv", cmd_mv, GRP_FILES, "mv [-v] SOURCE... DEST", "Move or rename files and directories." },
    { "tree", cmd_tree, GRP_FILES, "tree [-a] [-L DEPTH] [DIR]", "Show a directory tree." },
    { "find", cmd_find, GRP_FILES, "find [DIR] [-name PATTERN] [-type f|d]", "Search for files by name (wildcards * and ?, case-insensitive)." },
    { "stat", cmd_stat, GRP_FILES, "stat PATH...", "Show file details: size, type, times, associated app." },
    { "hexdump", cmd_hexdump, GRP_FILES, "hexdump [-n BYTES] [-s OFFSET] FILE", "Show a file as hex and ASCII (first 4 KiB unless -n is given)." },
    { "open", cmd_open, GRP_FILES, "open APP [ARG] | open FILE|DIR [-w APP]", "Start an app, or open a file with its default app (-w: with another app; Open With asks when none is set)." },
    /* text */
    { "echo", cmd_echo, GRP_TEXT, "echo [-n] [-e] TEXT...", "Print text (-n: no newline, -e: interpret \\n \\t \\e escapes). Use > or >> to write files." },
    { "grep", cmd_grep, GRP_TEXT, "grep [-i] [-n] [-v] [-c] [-r] PATTERN [FILE...]", "Print lines matching a pattern (supports . * ^ $). -i ignore case, -n line numbers, -v invert, -c count, -r recursive." },
    { "wc", cmd_wc, GRP_TEXT, "wc [-l] [-w] [-c] [FILE...]", "Count lines, words and bytes." },
    { "head", cmd_head, GRP_TEXT, "head [-n N] [FILE...]", "Print the first N (10) lines." },
    { "tail", cmd_tail, GRP_TEXT, "tail [-n N] [FILE...]", "Print the last N (10) lines." },
    { "sort", cmd_sort, GRP_TEXT, "sort [-r] [-n] [-u] [FILE...]", "Sort lines (-r reverse, -n numeric, -u unique)." },
    { "calc", term_cmd_calc, GRP_TEXT, "calc [EXPRESSION]", "Calculator: + - * / % ^ ! ( ), sqrt sin cos tan log ln exp abs floor ceil round min max, pi e ans. No argument: interactive mode." },
    /* system */
    { "date", cmd_date, GRP_SYSTEM, "date", "Show the date and time." },
    { "time", cmd_time, GRP_SYSTEM, "time", "Show the current time." },
    { "cal", cmd_cal, GRP_SYSTEM, "cal [MONTH YEAR | YEAR]", "Show a calendar (today is highlighted)." },
    { "uptime", cmd_uptime, GRP_SYSTEM, "uptime", "Show how long the system has been running." },
    { "free", cmd_free, GRP_SYSTEM, "free [-k]", "Show memory usage (-k: in KiB)." },
    { "mem", cmd_free, GRP_SYSTEM, "mem [-k]", "Show memory usage (same as free)." },
    { "df", cmd_df, GRP_SYSTEM, "df [-h] [-k]", "Show file system space: the RAM disk and, on an installed system, the saved copy on the hard disk." },
    { "disks", cmd_disks, GRP_SYSTEM, "disks [-r]", "List the IDE hard disks (model, size, NovaOS installed?); -r probes the controllers again." },
    { "sync", cmd_sync, GRP_SYSTEM, "sync", "Save all files to the hard disk now (installed systems save automatically too)." },
    { "ps", cmd_ps, GRP_SYSTEM, "ps", "List open windows with their ids (the 'processes' of NovaOS)." },
    { "kill", cmd_kill, GRP_SYSTEM, "kill WINDOW_ID...", "Close windows by id (see ps)." },
    { "killall", cmd_killall, GRP_SYSTEM, "killall APP", "Close every window of an app." },
    { "apps", cmd_apps, GRP_SYSTEM, "apps", "List installed applications by category." },
    { "run", cmd_open, GRP_SYSTEM, "run APP [ARG]", "Start an app (same as open)." },
    { "whoami", cmd_whoami, GRP_SYSTEM, "whoami", "Print the user name." },
    { "hostname", cmd_hostname, GRP_SYSTEM, "hostname", "Print the computer's name." },
    { "uname", cmd_uname, GRP_SYSTEM, "uname [-a] [-s] [-n] [-r] [-m]", "Print system information." },
    { "neofetch", term_cmd_neofetch, GRP_SYSTEM, "neofetch", "Show the NovaOS logo and system information." },
    { "dmesg", cmd_dmesg, GRP_SYSTEM, "dmesg [-n LINES]", "Show the kernel log." },
    { "beep", cmd_beep, GRP_SYSTEM, "beep [FREQ_HZ] [MS]", "Beep the PC speaker." },
    { "reboot", cmd_reboot, GRP_SYSTEM, "reboot", "Restart the computer." },
    { "shutdown", cmd_reboot, GRP_SYSTEM, "shutdown", "Power off the computer." },
    /* shell */
    { "help", cmd_help, GRP_SHELL, "help [COMMAND]", "Show this list, or details about one command." },
    { "history", cmd_history, GRP_SHELL, "history [N] | history -c", "Show the command history (!N runs entry N, !! the last one); -c clears it." },
    { "clear", cmd_clear, GRP_SHELL, "clear", "Clear the screen (also Ctrl+L)." },
    { "color", cmd_color, GRP_SHELL, "color [THEME | next | check]", "List or switch terminal colour themes; color check shows the screen colour depth." },
    { "env", cmd_env, GRP_SHELL, "env", "List shell variables." },
    { "export", cmd_export, GRP_SHELL, "export NAME=VALUE...", "Set shell variables (NAME=VALUE alone works too)." },
    { "unset", cmd_unset, GRP_SHELL, "unset NAME...", "Remove shell variables." },
    { "which", cmd_which, GRP_SHELL, "which COMMAND...", "Tell whether a name is a built-in or an app." },
    { "sh", cmd_sh, GRP_SHELL, "sh SCRIPT", "Run the commands in a script file." },
    { "source", cmd_sh, GRP_SHELL, "source SCRIPT", "Run the commands in a script file (same as sh)." },
    { "sleep", cmd_sleep, GRP_SHELL, "sleep SECONDS", "Wait without blocking the system (Ctrl+C cancels)." },
    { "timer", cmd_timer, GRP_SHELL, "timer SECONDS", "Countdown with a live progress bar and a chime (Ctrl+C cancels)." },
    { "true", cmd_true, GRP_SHELL, "true", "Do nothing, successfully." },
    { "false", cmd_false, GRP_SHELL, "false", "Do nothing, unsuccessfully." },
    { "exit", cmd_exit, GRP_SHELL, "exit", "Close this terminal (also Ctrl+D on an empty line)." },
    /* fun */
    { "cowsay", term_cmd_cowsay, GRP_FUN, "cowsay [TEXT]", "A cow says something (reads standard input if no text)." },
    { "fortune", term_cmd_fortune, GRP_FUN, "fortune", "Print a random saying (try: fortune | cowsay)." },
    { "sl", term_cmd_sl, GRP_FUN, "sl", "Steam locomotive - for when you mistype ls." },
    { "matrix", term_cmd_matrix, GRP_FUN, "matrix", "Digital rain. Press any key to wake up." },
};

static int cmd_count(void) { return ARRAY_SIZE(s_cmds); }
static const tcmd_t *cmd_get(int i) { return &s_cmds[i]; }
static const tcmd_t *find_cmd(const char *name) {
    for (int i = 0; i < cmd_count(); i++) if (!strcmp(s_cmds[i].name, name)) return &s_cmds[i];
    return NULL;
}
