/* shell_internal.h - internals shared by kernel/shell and kernel/services. */
#ifndef SHELL_INTERNAL_H
#define SHELL_INTERNAL_H

#include "gui/wm_internal.h"

#define CONFIG_DIR "/system/config"

/* /system/config/<name>: whole-file read (malloc'd, NUL-terminated, free()) / write. */
char *config_read(const char *name);
int   config_write(const char *name, const char *text);
/* Iterate the lines of a text buffer in place: returns the next line (NUL
   terminated, trailing CR/space stripped) or NULL. *cursor starts at the buffer. */
char *config_next_line(char **cursor);

/* shell bookkeeping */
void shell_bump(void);
void shell_recent_add_app(const char *app_name);

/* The Open With dialog's capable-app list (assoc.c). */
bool assoc_app_handles(const app_desc_t *a, const char *path);

/* Command output window used by the Run dialog (run.c). */
void shell_show_output(const char *title, const char *text);

#endif
