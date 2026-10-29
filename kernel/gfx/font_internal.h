/* font_internal.h - layout of the generated font tables (see tools/genfont.py). */
#ifndef FONT_INTERNAL_H
#define FONT_INTERNAL_H

#include <stdint.h>
#include <nova/gfx.h>

typedef struct glyph {
    uint8_t  adv;     /* advance width                         */
    int8_t   xoff;    /* bitmap left relative to pen x         */
    int8_t   yoff;    /* bitmap top relative to the line top   */
    uint8_t  w, h;    /* bitmap size                           */
    uint32_t off;     /* offset of the 8-bit coverage bitmap   */
} glyph_t;

struct font {
    const char    *name;
    int            height;   /* line height */
    int            ascent;   /* baseline    */
    int            mono;     /* fixed advance, 0 = proportional */
    int            first;    /* first byte value (0x20) */
    int            count;
    const glyph_t *glyphs;
    const uint8_t *data;
};

/* Fonts loaded from the drive (/system/ui/fonts, kernel/gui/uifiles.c) replace the
   built-in tables: text.c draws and measures with font_active(f). */
#define FONT_MAP_MAX 12
extern const struct font *font_map_from[FONT_MAP_MAX], *font_map_to[FONT_MAP_MAX];
extern int font_map_n;
static inline const struct font *font_active(const struct font *f) {
    for (int i = 0; i < font_map_n; i++)
        if (font_map_from[i] == f) return font_map_to[i];
    return f;
}

static inline const glyph_t *font_glyph(const struct font *f, unsigned char c) {
    int i = (int)c - f->first;
    if (i < 0 || i >= f->count) i = '?' - f->first;
    return &f->glyphs[i];
}

#endif
