/*
 * graphics_bmp.h - BMP reading/writing shared by the NovaOS graphics apps
 * (Paint, Image Viewer, Fractal Explorer).
 *
 * The decoder handles uncompressed Windows bitmaps: 1/4/8-bit palettised,
 * 16-bit (5-5-5 or BI_BITFIELDS masks), 24-bit and 32-bit (BGRX, BGRA or
 * arbitrary BI_BITFIELDS / BI_ALPHABITFIELDS masks), bottom-up or top-down,
 * with any of the common header versions (core, info, v2-v5). RLE, JPEG and
 * PNG payloads are rejected with a readable message.
 *
 * The encoder writes 24-bit BI_RGB files, or 32-bit BITMAPV4 files with an
 * alpha mask when the alpha channel should be kept.
 */
#ifndef GRAPHICS_BMP_H
#define GRAPHICS_BMP_H

#include <nova.h>

typedef struct gbmp_info {
    int  width, height;   /* pixels                                     */
    int  bpp;             /* bits per pixel as stored in the file       */
    bool top_down;        /* rows stored top to bottom                  */
    bool has_alpha;       /* a meaningful alpha channel was found       */
    char format[40];      /* human readable, e.g. "24-bit RGB"          */
} gbmp_info_t;

/* Decode a BMP from memory. Returns a new surface (SURF_ALPHA when the file has
   real transparency) or NULL; *err (if given) receives a static message. */
surface_t *gbmp_decode(const void *data, size_t size, gbmp_info_t *info, const char **err);

/* Read and decode an image file (BMP, or PPM through the system decoder). */
surface_t *gbmp_load(const char *path, gbmp_info_t *info, uint32_t *file_size, const char **err);

/* Encode a surface: 24-bit (keep_alpha false) or 32-bit with alpha. free() the result. */
void *gbmp_encode(const surface_t *s, bool keep_alpha, size_t *out_size);

/* Encode and write a file. Returns 0 or a negative VFS error (VFS_ENOMEM if encoding failed). */
int gbmp_save(const char *path, const surface_t *s, bool keep_alpha);

/* True for file names the graphics apps can open ("bmp", "dib", "ppm"). */
bool gbmp_is_image_name(const char *name);

#endif
