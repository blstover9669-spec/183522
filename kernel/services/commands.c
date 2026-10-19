/*
 * commands.c - the shell command registry (REGISTER_COMMAND, ".commands"
 * linker section), a small command-line tokenizer and output writers.
 */
#include "services/svc_internal.h"

extern const command_desc_t *const __commands_start[];
extern const command_desc_t *const __commands_end[];

static const command_desc_t **cmds;
static int ncmds;
static bool inited;

static int cmp_cmd(const void *a, const void *b) {
    return strcmp((*(const command_desc_t *const *)a)->name, (*(const command_desc_t *const *)b)->name);
}

static void ensure_init(void) {
    if (inited) return;
    inited = true;
    int n = (int)(__commands_end - __commands_start);
    cmds = kcalloc((size_t)MAX(n, 1), sizeof *cmds);
    if (!cmds) return;
    for (int i = 0; i < n; i++) {
        const command_desc_t *c = __commands_start[i];
        if (!c || !c->name || !c->fn) continue;
        bool dup = false;
        for (int j = 0; j < ncmds; j++) if (!strcmp(cmds[j]->name, c->name)) dup = true;
        if (!dup) cmds[ncmds++] = c;
    }
    qsort(cmds, (size_t)ncmds, sizeof *cmds, cmp_cmd);
}

int command_count(void) { ensure_init(); return ncmds; }
const command_desc_t *command_get(int i) { ensure_init(); return (i >= 0 && i < ncmds) ? cmds[i] : NULL; }

const command_desc_t *command_find(const char *name) {
    ensure_init();
    if (!name) return NULL;
    for (int i = 0; i < ncmds; i++) if (!strcmp(cmds[i]->name, name)) return cmds[i];
    for (int i = 0; i < ncmds; i++) if (!strcasecmp(cmds[i]->name, name)) return cmds[i];
    return NULL;
}

int command_exec(int argc, char **argv, cmd_writer_t *out) {
    if (argc < 1 || !argv || !argv[0]) return 0;
    const command_desc_t *c = command_find(argv[0]);
    if (!c) return 127;
    return c->fn(argc, argv, out);
}

#define MAX_ARGS 32

int command_run_line(const char *line, cmd_writer_t *out) {
    if (!line) return 0;
    size_t len = strlen(line);
    char *buf = kmalloc(len + 1);
    if (!buf) return 1;
    char *argv[MAX_ARGS + 1];
    int argc = 0;
    const char *p = line;
    char *o = buf;
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = o;
        char q = 0;
        while (*p) {
            char ch = *p;
            if (q) {
                if (ch == q) { q = 0; p++; continue; }
                if (ch == '\\' && q == '"' && p[1]) { *o++ = p[1]; p += 2; continue; }
                *o++ = ch; p++;
                continue;
            }
            if (ch == ' ' || ch == '\t') break;
            if (ch == '"' || ch == '\'') { q = ch; p++; continue; }
            if (ch == '\\' && p[1]) { *o++ = p[1]; p += 2; continue; }
            *o++ = ch; p++;
        }
        *o++ = 0;
    }
    argv[argc] = NULL;
    int r = argc ? command_exec(argc, argv, out) : 0;
    kfree(buf);
    return r;
}

void cmd_write(cmd_writer_t *out, const char *s, int n) {
    if (out && out->write && s && n > 0) out->write(out, s, n);
}

void cmd_puts(cmd_writer_t *out, const char *s) { if (s) cmd_write(out, s, (int)strlen(s)); }

void cmd_printf(cmd_writer_t *out, const char *fmt, ...) {
    char small[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < (int)sizeof small) { cmd_write(out, small, n); return; }
    char *big = kmalloc((size_t)n + 1);
    if (!big) { cmd_write(out, small, (int)sizeof small - 1); return; }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    cmd_write(out, big, n);
    kfree(big);
}

static void buffer_write(cmd_writer_t *w, const char *s, int n) {
    cmd_buffer_t *b = (cmd_buffer_t *)w;
    if (b->len + n + 1 > b->cap) {
        int nc = MAX(b->cap * 2, b->len + n + 256);
        char *nb = krealloc(b->buf, (size_t)nc);
        if (!nb) return;
        b->buf = nb;
        b->cap = nc;
    }
    memcpy(b->buf + b->len, s, (size_t)n);
    b->len += n;
    b->buf[b->len] = 0;
}

void cmd_buffer_init(cmd_buffer_t *b) {
    memset(b, 0, sizeof *b);
    b->w.write = buffer_write;
    b->w.ctx = b;
}

/* `help`-like listing of registered commands (the Terminal has its own help). */
static int cmd_commands(int argc, char **argv, cmd_writer_t *out) {
    if (argc > 1) {
        const command_desc_t *c = command_find(argv[1]);
        if (!c) { cmd_printf(out, "commands: no command named '%s'\n", argv[1]); return 1; }
        cmd_printf(out, "%s\n", c->help ? c->help : c->name);
        return 0;
    }
    cmd_puts(out, "Registered commands:\n");
    for (int i = 0; i < command_count(); i++) {
        const command_desc_t *c = command_get(i);
        const char *h = c->help ? c->help : "";
        const char *nl = strchr(h, '\n');
        int hl = nl ? (int)(nl - h) : (int)strlen(h);
        cmd_printf(out, "  %-10s %.*s\n", c->name, hl, h);
    }
    return 0;
}
REGISTER_COMMAND("commands", "commands [NAME] - list the registered system commands", cmd_commands);
