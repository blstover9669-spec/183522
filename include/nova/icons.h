/*
 * nova/icons.h - the system icon library.
 *
 * Hand-designed vector icons drawn with gfx primitives (no image files, no
 * emoji). Every icon is designed on a 64-unit grid and has a simplified,
 * pixel-aligned variant for small sizes (<= 20 px), so it stays crisp at
 * 16, 32 and 48 px. Icons are rendered supersampled onto a transparent
 * surface, box-filtered down and cached per (kind, size).
 *
 *   icon_draw(ICON_FOLDER_PICTURES, s, x, y, 32);
 *   icon_draw(icon_for_path("/home/user/notes.txt"), s, x, y, 16);
 */
#ifndef NOVA_ICONS_H
#define NOVA_ICONS_H

#include <nova/types.h>
#include <nova/gfx.h>

enum icon_kind {
    ICON_NONE = 0,
    /* folders */
    ICON_FOLDER,
    ICON_FOLDER_DOCUMENTS,
    ICON_FOLDER_PICTURES,
    ICON_FOLDER_MUSIC,
    ICON_FOLDER_DOWNLOADS,
    ICON_FOLDER_DESKTOP,
    ICON_FOLDER_SYSTEM,
    ICON_FOLDER_HOME,
    /* files */
    ICON_FILE_TEXT,
    ICON_FILE_CODE,
    ICON_FILE_IMAGE,
    ICON_FILE_AUDIO,
    ICON_FILE_ARCHIVE,
    ICON_FILE_BINARY,
    ICON_FILE_UNKNOWN,
    ICON_FILE_LOG,
    ICON_FILE_CONFIG,
    ICON_NOTE,
    ICON_PAINT,
    /* objects */
    ICON_APP,
    ICON_DISK,
    ICON_COMPUTER,
    ICON_TRASH,
    ICON_GEAR,
    ICON_SERVICE,
    ICON_SETTINGS,
    ICON_BELL,
    ICON_ALARM,
    ICON_CLOCK,
    ICON_SEARCH,
    ICON_RUN,
    ICON_PIN,
    ICON_TERMINAL,
    /* status */
    ICON_INFO,
    ICON_WARNING,
    ICON_ERROR,
    ICON_QUESTION,
    ICON_SUCCESS,
    /* security */
    ICON_SHIELD,          /* SafetyShield */
    ICON_COUNT
};

/* Draw an icon (cached, anti-aliased) with its top-left corner at (x, y).
   Any size from 8 to 256 works; 16, 32 and 48 are tuned. */
void             icon_draw(int kind, surface_t *dst, int x, int y, int size);
/* Same, faded: opacity 0..255. */
void             icon_draw_alpha(int kind, surface_t *dst, int x, int y, int size, int opacity);
/* The cached image (SURF_ALPHA). Do not free. NULL for ICON_NONE / bad sizes. */
const surface_t *icon_get(int kind, int size);
/* Vector-draw an icon directly at full resolution onto a TRANSPARENT surface
   (e.g. inside an app's draw_icon callback). No caching, no supersampling. */
void             icon_render(int kind, surface_t *s, int x, int y, int size);

/* Pick an icon for a path: special folders (Documents, Pictures, ...), other
   folders, then by file extension (registered file types first). */
int              icon_for_path(const char *path);
/* Icon for a bare extension ("txt"); ICON_FILE_UNKNOWN if not known. */
int              icon_for_ext(const char *ext);
/* "folder", "text", ... (for docs and the icon sheet). */
const char      *icon_name(int kind);

#endif
