/* text.c - anti-aliased bitmap text rendering and measurement. Every call draws and
   measures with font_active(f): the font loaded from the drive when there is one. */
#include "kernel.h"
#include "font_internal.h"

int font_height(const font_t *f) { return font_active(f)->height; }
int font_ascent(const font_t *f) { return font_active(f)->ascent; }
bool font_is_mono(const font_t *f) { return font_active(f)->mono != 0; }

int font_char_width(const font_t *f, unsigned char c) {
    f = font_active(f);
    if (f->mono) return f->mono;
    return font_glyph(f, c)->adv;
}

int font_text_width_n(const font_t *f, const char *s, int n) {
    f = font_active(f);
    int w = 0;
    for (int i = 0; i < n && s[i]; i++) w += font_char_width(f, (unsigned char)s[i]);
    return w;
}

int font_text_width(const font_t *f, const char *s) {
    f = font_active(f);
    if (f->mono) return (int)strlen(s) * f->mono;
    int w = 0;
    for (; *s; s++) w += font_glyph(f, (unsigned char)*s)->adv;
    return w;
}

int font_hit_test(const font_t *f, const char *s, int px) {
    f = font_active(f);
    int x = 0, i = 0;
    for (; s[i]; i++) {
        int cw = font_char_width(f, (unsigned char)s[i]);
        if (px < x + cw / 2) return i;
        x += cw;
    }
    return i;
}

int gfx_char(surface_t *s, int x, int y, unsigned char ch, uint32_t c, const font_t *f) {
    f = font_active(f);
    const glyph_t *g = font_glyph(f, ch);
    int adv = f->mono ? f->mono : g->adv;
    if (!g->w || !g->h || ch == ' ') return adv;
    int gx = x + g->xoff, gy = y + g->yoff;
    rect_t r = rect_intersect(rect_make(gx, gy, g->w, g->h), s->clip);
    if (rect_empty(r)) return adv;
    uint32_t ca = COLOR_A(c);
    const uint8_t *bits = f->data + g->off;
    bool dst_alpha = s->flags & SURF_ALPHA;
    uint32_t rgb = c & 0x00FFFFFFu;
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        const uint8_t *src = bits + (yy - gy) * g->w + (r.x - gx);
        uint32_t *dst = s->pixels + (size_t)yy * s->pitch + r.x;
        for (int xx = 0; xx < r.w; xx++) {
            uint32_t a = src[xx];
            if (!a) continue;
            if (ca != 255) a = a * ca / 255;
            if (dst_alpha) {
                /* composite over a transparent surface */
                uint32_t d = dst[xx];
                uint32_t da = COLOR_A(d);
                if (da == 0) { dst[xx] = rgb | (a << 24); continue; }
                uint32_t oa = a + da * (255 - a) / 255;
                uint32_t wd = da * (255 - a) / 255;
                uint32_t rr = (COLOR_R(c) * a + COLOR_R(d) * wd) / oa;
                uint32_t gg = (COLOR_G(c) * a + COLOR_G(d) * wd) / oa;
                uint32_t bb = (COLOR_B(c) * a + COLOR_B(d) * wd) / oa;
                dst[xx] = RGBA(rr, gg, bb, oa);
            } else if (a >= 255) {
                dst[xx] = c | 0xFF000000u;
            } else {
                uint32_t d = dst[xx];
                uint32_t a1 = a + (a >> 7), ia = 256 - a1;
                uint32_t rb = (((c & 0xFF00FFu) * a1 + (d & 0xFF00FFu) * ia) >> 8) & 0xFF00FFu;
                uint32_t gg = (((c & 0x00FF00u) * a1 + (d & 0x00FF00u) * ia) >> 8) & 0x00FF00u;
                dst[xx] = 0xFF000000u | rb | gg;
            }
        }
    }
    return adv;
}

int gfx_text_n(surface_t *s, int x, int y, const char *str, int n, uint32_t c, const font_t *f) {
    f = font_active(f);
    int x0 = x;
    if (y >= s->clip.y + s->clip.h || y + f->height <= s->clip.y) return font_text_width_n(f, str, n);
    for (int i = 0; i < n && str[i]; i++) {
        if (x >= s->clip.x + s->clip.w) { x += font_text_width_n(f, str + i, n - i); break; }
        x += gfx_char(s, x, y, (unsigned char)str[i], c, f);
    }
    return x - x0;
}

int gfx_text(surface_t *s, int x, int y, const char *str, uint32_t c, const font_t *f) {
    return gfx_text_n(s, x, y, str, 0x7fffffff, c, f);
}

int gfx_text_scaled(surface_t *s, int x, int y, const char *str, uint32_t c, const font_t *f, int scale) {
    f = font_active(f);
    if (scale <= 1) return gfx_text(s, x, y, str, c, f);
    int x0 = x;
    for (; *str; str++) {
        const glyph_t *g = font_glyph(f, (unsigned char)*str);
        const uint8_t *bits = f->data + g->off;
        for (int yy = 0; yy < g->h; yy++)
            for (int xx = 0; xx < g->w; xx++) {
                uint32_t a = bits[yy * g->w + xx];
                if (a < 96) continue;   /* crisp, chunky look */
                gfx_fill_rect(s, x + (g->xoff + xx) * scale, y + (g->yoff + yy) * scale, scale, scale, c);
            }
        x += (f->mono ? f->mono : g->adv) * scale;
    }
    return x - x0;
}

void gfx_text_aligned(surface_t *s, rect_t r, const char *str, uint32_t c, const font_t *f, int align) {
    f = font_active(f);
    int tw = font_text_width(f, str);
    int x = r.x, y = r.y;
    if (align & ALIGN_CENTER) x = r.x + (r.w - tw) / 2;
    else if (align & ALIGN_RIGHT) x = r.x + r.w - tw;
    if (align & ALIGN_MIDDLE) y = r.y + (r.h - f->height) / 2;
    else if (align & ALIGN_BOTTOM) y = r.y + r.h - f->height;
    rect_t old = s->clip;
    surface_set_clip(s, rect_intersect(old, r));
    gfx_text(s, x, y, str, c, f);
    s->clip = old;
}

int gfx_text_ellipsis(surface_t *s, int x, int y, int max_w, const char *str, uint32_t c, const font_t *f) {
    f = font_active(f);
    int tw = font_text_width(f, str);
    if (tw <= max_w) return gfx_text(s, x, y, str, c, f);
    int dots = font_text_width(f, "...");
    int w = 0, n = 0;
    while (str[n]) {
        int cw = font_char_width(f, (unsigned char)str[n]);
        if (w + cw + dots > max_w) break;
        w += cw;
        n++;
    }
    gfx_text_n(s, x, y, str, n, c, f);
    gfx_text(s, x + w, y, "...", c, f);
    return w + dots;
}

/* Word wrapping: calls emit(line, len) for each line. Returns number of lines. */
typedef void (*line_fn)(const char *line, int len, int idx, void *ctx);

static int wrap_text(const font_t *f, const char *str, int width, line_fn fn, void *ctx) {
    int lines = 0;
    const char *p = str;
    for (;;) {
        int w = 0, i = 0, brk = -1;
        while (p[i] && p[i] != '\n') {
            int cw = font_char_width(f, (unsigned char)p[i]);
            if (w + cw > width && i > 0) break;
            if (p[i] == ' ') brk = i;
            w += cw;
            i++;
        }
        int len = i, next = i;
        if (p[i] == '\n') next = i + 1;
        else if (p[i] && brk > 0) { len = brk; next = brk + 1; }   /* wrap at the last space */
        if (fn) fn(p, len, lines, ctx);
        lines++;
        if (!p[i]) break;
        p += next;
    }
    return lines;
}

int font_wrap_height(const font_t *f, const char *s, int w) {
    f = font_active(f);
    return wrap_text(f, s, w, NULL, NULL) * f->height;
}

typedef struct { surface_t *s; rect_t r; uint32_t c; const font_t *f; } wrap_ctx_t;

static void draw_line(const char *line, int len, int idx, void *ctx) {
    wrap_ctx_t *w = ctx;
    gfx_text_n(w->s, w->r.x, w->r.y + idx * w->f->height, line, len, w->c, w->f);
}

int gfx_text_wrapped(surface_t *s, rect_t r, const char *str, uint32_t c, const font_t *f) {
    f = font_active(f);
    wrap_ctx_t ctx = { s, r, c, f };
    rect_t old = s->clip;
    surface_set_clip(s, rect_intersect(old, r));
    int n = wrap_text(f, str, r.w, draw_line, &ctx);
    s->clip = old;
    return n * f->height;
}
