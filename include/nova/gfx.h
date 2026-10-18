/*
 * nova/gfx.h - 2D graphics: surfaces, colours, primitives, images and text.
 *
 * Pixels are 32-bit ARGB (0xAARRGGBB). Every drawing call is clipped to the
 * surface's clip rectangle. Colours with alpha < 255 are alpha-blended over
 * the existing pixels; alpha 255 overwrites.
 *
 * Surfaces created with surface_create() are opaque (alpha ignored when they
 * are blitted with gfx_blit). surface_create_alpha() makes a transparent
 * surface whose alpha channel is honoured by gfx_blit_alpha() and in which
 * drawing composites "over" correctly (used for icons, sprites, overlays).
 */
#ifndef NOVA_GFX_H
#define NOVA_GFX_H

#include <nova/types.h>

/* ---- colours ------------------------------------------------------------ */
#define RGB(r, g, b)     (0xFF000000u | ((uint32_t)((r) & 255) << 16) | ((uint32_t)((g) & 255) << 8) | (uint32_t)((b) & 255))
#define RGBA(r, g, b, a) (((uint32_t)((a) & 255) << 24) | ((uint32_t)((r) & 255) << 16) | ((uint32_t)((g) & 255) << 8) | (uint32_t)((b) & 255))
#define COLOR_A(c) (((c) >> 24) & 255)
#define COLOR_R(c) (((c) >> 16) & 255)
#define COLOR_G(c) (((c) >> 8) & 255)
#define COLOR_B(c) ((c) & 255)

#define COLOR_BLACK       RGB(0, 0, 0)
#define COLOR_WHITE       RGB(255, 255, 255)
#define COLOR_TRANSPARENT 0x00000000u

/* Linear interpolation between two colours, t in 0..255 (0 = a, 255 = b). */
uint32_t color_mix(uint32_t a, uint32_t b, int t);
/* Lighten (amount > 0) or darken (amount < 0) a colour; amount in -255..255. */
uint32_t color_shade(uint32_t c, int amount);
/* Replace a colour's alpha. */
static inline uint32_t color_alpha(uint32_t c, int a) { return (c & 0x00FFFFFFu) | ((uint32_t)(a & 255) << 24); }
/* Perceived brightness 0..255. */
int color_luma(uint32_t c);
/* HSV (h 0..359, s,v 0..255) -> RGB colour. */
uint32_t color_hsv(int h, int s, int v);

/* ---- surfaces ----------------------------------------------------------- */
#define SURF_OWNED 0x1   /* pixels were allocated by surface_create* and are freed with it */
#define SURF_ALPHA 0x2   /* alpha channel is meaningful (transparent surface)          */

typedef struct surface {
    int w, h;          /* size in pixels                                  */
    int pitch;         /* distance between rows, in pixels (uint32_t)     */
    uint32_t *pixels;  /* pixel (x,y) is pixels[y * pitch + x]            */
    rect_t clip;       /* drawing is restricted to this rect              */
    uint32_t flags;    /* SURF_*                                          */
} surface_t;

surface_t *surface_create(int w, int h);          /* opaque, filled black            */
surface_t *surface_create_alpha(int w, int h);    /* transparent, SURF_ALPHA         */
void       surface_destroy(surface_t *s);
surface_t *surface_clone(const surface_t *s);
/* A view onto a sub-rectangle of another surface (shares pixels, nothing to free). */
surface_t  surface_sub(const surface_t *parent, rect_t r);
/* New surface containing src scaled to w x h (bilinear). */
surface_t *surface_scaled(const surface_t *src, int w, int h);
void       surface_set_clip(surface_t *s, rect_t r);   /* clipped to surface bounds */
void       surface_reset_clip(surface_t *s);
static inline uint32_t *surface_row(const surface_t *s, int y) { return s->pixels + (size_t)y * s->pitch; }

/* ---- primitives --------------------------------------------------------- */
void     gfx_clear(surface_t *s, uint32_t c);                         /* fill clip rect */
void     gfx_pixel(surface_t *s, int x, int y, uint32_t c);
uint32_t gfx_get_pixel(const surface_t *s, int x, int y);              /* 0 if outside   */
void     gfx_fill_rect(surface_t *s, int x, int y, int w, int h, uint32_t c);
void     gfx_rect(surface_t *s, int x, int y, int w, int h, uint32_t c); /* 1px outline   */
void     gfx_hline(surface_t *s, int x, int y, int w, uint32_t c);
void     gfx_vline(surface_t *s, int x, int y, int h, uint32_t c);
void     gfx_line(surface_t *s, int x0, int y0, int x1, int y1, uint32_t c);      /* Bresenham */
void     gfx_line_aa(surface_t *s, double x0, double y0, double x1, double y1, uint32_t c); /* Wu */
void     gfx_thick_line(surface_t *s, int x0, int y0, int x1, int y1, int thickness, uint32_t c);
void     gfx_circle(surface_t *s, int cx, int cy, int r, uint32_t c);
void     gfx_fill_circle(surface_t *s, int cx, int cy, int r, uint32_t c);
void     gfx_fill_circle_aa(surface_t *s, double cx, double cy, double r, uint32_t c);
void     gfx_circle_aa(surface_t *s, double cx, double cy, double r, double thickness, uint32_t c);
void     gfx_ellipse(surface_t *s, int cx, int cy, int rx, int ry, uint32_t c);
void     gfx_fill_ellipse(surface_t *s, int cx, int cy, int rx, int ry, uint32_t c);
/* Rounded rectangles with anti-aliased corners. */
void     gfx_fill_rounded_rect(surface_t *s, int x, int y, int w, int h, int r, uint32_t c);
void     gfx_rounded_rect(surface_t *s, int x, int y, int w, int h, int r, uint32_t c);
void     gfx_gradient_v(surface_t *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void     gfx_gradient_h(surface_t *s, int x, int y, int w, int h, uint32_t left, uint32_t right);
void     gfx_fill_rounded_gradient_v(surface_t *s, int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom);
void     gfx_fill_triangle(surface_t *s, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c);
void     gfx_fill_polygon(surface_t *s, const point_t *pts, int n, uint32_t c);   /* even-odd */
/* Soft drop shadow for a (rounded) rectangle, drawn around/under it. */
void     gfx_shadow(surface_t *s, int x, int y, int w, int h, int radius, int spread, int alpha);

/* ---- blitting ----------------------------------------------------------- */
/* Opaque copy of all of src to (dx,dy). */
void gfx_blit(surface_t *dst, int dx, int dy, const surface_t *src);
/* Opaque copy of src rect (sx,sy,w,h). */
void gfx_blit_rect(surface_t *dst, int dx, int dy, const surface_t *src, int sx, int sy, int w, int h);
/* Alpha-blended copy honouring src per-pixel alpha, scaled by opacity (0..255). */
void gfx_blit_alpha(surface_t *dst, int dx, int dy, const surface_t *src, int opacity);
void gfx_blit_alpha_rect(surface_t *dst, int dx, int dy, const surface_t *src,
                         int sx, int sy, int w, int h, int opacity);
/* Scaled copy of src rect into dst rect. smooth = bilinear, else nearest. Honours alpha if src has SURF_ALPHA. */
void gfx_blit_scaled(surface_t *dst, int dx, int dy, int dw, int dh,
                     const surface_t *src, int sx, int sy, int sw, int sh, bool smooth);

/* ---- text --------------------------------------------------------------- */
/*
 * Fonts are anti-aliased bitmap fonts covering bytes 0x20-0xFF:
 * ASCII, 0x80-0x9F symbols (see docs/API.md), 0xA0-0xFF Latin-1.
 * Text y coordinates are the TOP of the line box (font_height() tall).
 */
typedef struct font font_t;

extern const font_t font_mono;       /*  8x16 monospace (terminal, editors) */
extern const font_t font_mono_bold;  /*  8x16 monospace bold                */
extern const font_t font_mono_2x;    /* 16x32 monospace (the 2x variant)    */
extern const font_t font_ui;         /* proportional UI font, 13px (Inter)  */
extern const font_t font_ui_bold;    /* semibold UI font, 13px              */
extern const font_t font_ui_large;   /* semibold 18px headings              */
extern const font_t font_ui_title;   /* bold 28px titles                    */
extern const font_t font_ui_huge;    /* light 52px (clocks, splash)         */

#define FONT_MONO      (&font_mono)
#define FONT_MONO_BOLD (&font_mono_bold)
#define FONT_MONO_2X   (&font_mono_2x)
#define FONT_UI        (&font_ui)
#define FONT_UI_BOLD   (&font_ui_bold)
#define FONT_UI_LARGE  (&font_ui_large)
#define FONT_UI_TITLE  (&font_ui_title)
#define FONT_UI_HUGE   (&font_ui_huge)

/* Alignment flags for gfx_text_aligned / labels. */
#define ALIGN_LEFT    0x00
#define ALIGN_CENTER  0x01
#define ALIGN_RIGHT   0x02
#define ALIGN_TOP     0x00
#define ALIGN_MIDDLE  0x10
#define ALIGN_BOTTOM  0x20

int  font_height(const font_t *f);                 /* line height in pixels         */
int  font_ascent(const font_t *f);                 /* baseline offset from line top */
int  font_char_width(const font_t *f, unsigned char c);
int  font_text_width(const font_t *f, const char *s);
int  font_text_width_n(const font_t *f, const char *s, int n);
bool font_is_mono(const font_t *f);
/* Index of the character boundary closest to pixel offset px (for caret placement). */
int  font_hit_test(const font_t *f, const char *s, int px);
/* Height needed to draw s word-wrapped into width w. */
int  font_wrap_height(const font_t *f, const char *s, int w);

/* Draw text; returns the advance width in pixels. '\n' is not interpreted. */
int  gfx_text(surface_t *s, int x, int y, const char *str, uint32_t c, const font_t *f);
int  gfx_text_n(surface_t *s, int x, int y, const char *str, int n, uint32_t c, const font_t *f);
int  gfx_char(surface_t *s, int x, int y, unsigned char ch, uint32_t c, const font_t *f);
/* Integer-scaled text (chunky pixels, e.g. scale 2 = 2x). */
int  gfx_text_scaled(surface_t *s, int x, int y, const char *str, uint32_t c, const font_t *f, int scale);
/* Text aligned inside r (ALIGN_* flags). Single line. */
void gfx_text_aligned(surface_t *s, rect_t r, const char *str, uint32_t c, const font_t *f, int align);
/* Text truncated with "..." to fit max_w. Returns width drawn. */
int  gfx_text_ellipsis(surface_t *s, int x, int y, int max_w, const char *str, uint32_t c, const font_t *f);
/* Word-wrapped text inside r, honouring '\n'. Returns the height used. */
int  gfx_text_wrapped(surface_t *s, rect_t r, const char *str, uint32_t c, const font_t *f);

/* ---- images ------------------------------------------------------------- */
/* Decode a BMP (24/32-bit, uncompressed) or binary PPM (P6) from memory. NULL on error. */
surface_t *image_decode(const void *data, size_t size);
/* Encode a surface as a 24-bit BMP. Returns kmalloc'd buffer (free()), sets *out_size. */
void *image_encode_bmp(const surface_t *s, size_t *out_size);

#endif
