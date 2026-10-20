/* config.c - tiny helpers for the text files in /system/config. */
#include "shell/shell_internal.h"

char *config_read(const char *name) {
    char path[VFS_PATH_MAX];
    snprintf(path, sizeof path, CONFIG_DIR "/%s", name);
    uint32_t size;
    return vfs_read_file(path, &size);
}

int config_write(const char *name, const char *text) {
    char path[VFS_PATH_MAX];
    vfs_mkdirs(CONFIG_DIR);
    snprintf(path, sizeof path, CONFIG_DIR "/%s", name);
    return vfs_write_file(path, text, (uint32_t)strlen(text));
}

char *config_next_line(char **cursor) {
    char *p = *cursor;
    if (!p || !*p) return NULL;
    char *e = strchr(p, '\n');
    if (e) { *e = 0; *cursor = e + 1; }
    else *cursor = p + strlen(p);
    size_t n = strlen(p);
    while (n && (p[n - 1] == '\r' || p[n - 1] == ' ' || p[n - 1] == '\t')) p[--n] = 0;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static uint32_t g_shell_gen = 1;
void shell_bump(void) { g_shell_gen++; }
uint32_t shell_generation(void) { return g_shell_gen; }
