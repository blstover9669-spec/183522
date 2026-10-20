/*
 * nova/shell.h - opening files and apps: file associations, shell_open(),
 * the Open With / Properties / Run dialogs, taskbar pins and recent items.
 *
 *   shell_open("/home/user/Documents/notes.txt");   // default app, or Open With
 *   shell_open("/home/user/Pictures");               // folders open in Files
 *   REGISTER_FILETYPE("note", "Sticky Notes", "Sticky note", ICON_NOTE);
 */
#ifndef NOVA_SHELL_H
#define NOVA_SHELL_H

#include <nova/types.h>
#include <nova/app.h>
#include <nova/wm.h>

/* ---- file types -------------------------------------------------------------------
 * An app declares the extensions it opens either with the descriptor field
 * app_desc_t.file_types ("txt,md") or with REGISTER_FILETYPE from any file.
 * ext "*" means "can open any file" (offered in Open With, never the automatic
 * default); ext "folder" means directories. REGISTER_FILETYPE_ALT registers an
 * alternative that is only the default when nothing else is installed.
 * Apps that are not installed are skipped. */
typedef struct filetype_desc {
    const char *ext;          /* "txt" (no dot), "*" or "folder"          */
    const char *app;          /* name of the app that opens it            */
    const char *description;  /* "Text document" (may be NULL)            */
    int icon;                 /* ICON_* from nova/icons.h (0 = automatic) */
    int alt;                  /* 1 = alternative, not a preferred default */
} filetype_desc_t;

#define _FT_CAT2(a, b) a##b
#define _FT_CAT(a, b) _FT_CAT2(a, b)
#define _REGISTER_FILETYPE(ext_, app_, desc_, icon_, alt_)                                   \
    static const filetype_desc_t _FT_CAT(_ft_desc_, __LINE__) = { ext_, app_, desc_, icon_, alt_ }; \
    static const filetype_desc_t *const _FT_CAT(_ft_ptr_, __LINE__)                           \
        __attribute__((used, section(".filetypes"), aligned(4))) = &_FT_CAT(_ft_desc_, __LINE__)
#define REGISTER_FILETYPE(ext, app, desc, icon)     _REGISTER_FILETYPE(ext, app, desc, icon, 0)
#define REGISTER_FILETYPE_ALT(ext, app, desc, icon) _REGISTER_FILETYPE(ext, app, desc, icon, 1)

/* Default app for a path (user choice, then registrations, then descriptors);
   NULL if no installed app handles it. Folders resolve to "Files". */
const app_desc_t *assoc_default_app(const char *path);
/* Installed apps able to open the path, default first, then the others,
   then "any file" apps. Returns the number found (may exceed max). */
int               assoc_apps(const char *path, const app_desc_t **out, int max);
/* Change the default app for an extension ("txt"; "folder" for directories).
   app_name NULL or "" resets to the built-in default. Persisted in
   /system/config/assoc. Returns false if the app is not installed. */
bool              assoc_set_default(const char *ext, const char *app_name);
/* The user's chosen default for ext, or NULL. */
const char       *assoc_user_default(const char *ext);
/* Known extensions (registered or declared by apps), sorted. Returns count. */
int               assoc_extensions(const char **out, int max);
/* Human readable type: "Text document", "Folder", "BMP file". */
const char       *filetype_description(const char *path);
/* Icon registered for an extension, or ICON_NONE (0). */
int               filetype_icon(const char *ext);

/* ---- opening -------------------------------------------------------------------------- */
/* Open a file with its default app, a folder in Files, or show Open With when
   no app is associated. Also accepts an app name ("Notepad"). Records the item
   in the recent list. Returns false if nothing could be opened. */
bool      shell_open(const char *path);
/* Open with a specific app (by name). */
bool      shell_open_with(const char *path, const char *app_name);
/* The Open With dialog: every capable app plus "Always use this app". */
window_t *shell_open_with_dialog(const char *path);
/* File / folder Properties dialog. */
window_t *shell_properties(const char *path);
/* Run dialog (Win+R): app name, file/folder path or a registered command. */
window_t *shell_run_dialog(void);
/* What the Run dialog does with its text. Returns false (and a message in
   err) if nothing matched. Command output is shown in a small window. */
bool      shell_run(const char *text, char *err, int errsz);

/* ---- taskbar pins (persisted in /system/config/pinned) ----------------------------------- */
int         shell_pin_count(void);
const char *shell_pin_get(int index);             /* app name */
bool        shell_is_pinned(const char *app_name);
bool        shell_set_pinned(const char *app_name, bool pinned);

/* ---- recent items (persisted in /system/config/recent) ------------------------------------ */
/* An item is an absolute path or "app:Name". Newest first. */
void        shell_recent_add(const char *item);
int         shell_recent_count(void);
const char *shell_recent_get(int index);
void        shell_recent_clear(void);

/* Bumps whenever pins, recent items or associations change (for UIs). */
uint32_t    shell_generation(void);

#endif
