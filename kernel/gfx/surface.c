/* surface.c - surfaces, colour helpers, primitives and blitting. */
#include "kernel.h"

/* ---- colour helpers ----------------------------------------------------------- */
uint32_t color_mix(uint32_t a, uint32_t b, int t) {
    if (t <= 0) return a;
    if (t >= 255) return b;
    uint32_t it = 255 - (uint32_t)t, ut = (uint32_t)t;
    uint32_t ra = COLOR_R(a), ga = COLOR_G(a), ba = COLOR_B(a), aa = COLOR_A(a);
    uint32_t rb = COLOR_R(b), gb = COLOR_G(b), bb = COLOR_B(b), ab = COLOR_A(b);
    return RGBA((ra * it + rb * ut) / 255, (ga * it + gb * ut) / 255, (ba * it + bb * ut) / 255,
                (aa * it + ab * ut) / 255);
}

uint32_t color_shade(uint32_t c, int amount) {
    if (amount >= 0) return color_alpha(color_mix(c | 0xFF000000u, COLOR_WHITE, amount), (int)COLOR_A(c));
    return color_alpha(color_mix(c | 0xFF000000u, COLOR_BLACK, -amount), (int)COLOR_A(c));
}

int color_luma(uint32_t c) {
    return (int)((COLOR_R(c) * 299 + COLOR_G(c) * 587 + COLOR_B(c) * 114) / 1000);
}

uint32_t color_hsv(int h, int s, int v) {
    h %= 360;
    if (h < 0) h += 360;
    int region = h / 60, rem = (h % 60) * 255 / 60;
    int p = v * (255 - s) / 255, q = v * (255 - (s * rem) / 255) / 255, t = v * (255 - (s * (255 - rem)) / 255) / 255;
    switch (region) {
    case 0: return RGB(v, t, p);
    case 1: return RGB(q, v, p);
    case 2: return RGB(p, v, t);
    case 3: return RGB(p, q, v);
    case 4: return RGB(t, p, v);
    default: return RGB(v, p, q);
    }
}

/* ---- blending ------------------------------------------------------------------- */
/* src over opaque dst, a = 0..255 */
static inline uint32_t blend_opaque(uint32_t dst, uint32_t src, uint32_t a) {
    uint32_t a1 = a + (a >> 7);            /* 0..256 */
    uint32_t ia = 256 - a1;
    uint32_t rb = (((src & 0xFF00FFu) * a1 + (dst & 0xFF00FFu) * ia) >> 8) & 0xFF00FFu;
    uint32_t g = (((src & 0x00FF00u) * a1 + (dst & 0x00FF00u) * ia) >> 8) & 0x00FF00u;
    return 0xFF000000u | rb | g;
}

/* src over dst where dst has its own alpha (non-premultiplied). */
static inline uint32_t blend_alpha(uint32_t dst, uint32_t src, uint32_t sa) {
    uint32_t da = COLOR_A(dst);
    if (da == 0) return color_alpha(src, (int)sa);
    if (da == 255) return blend_opaque(dst, src, sa);
    uint32_t oa = sa + da * (255 - sa) / 255;
    if (oa == 0) return 0;
    uint32_t wd = da * (255 - sa) / 255;
    uint32_t r = (COLOR_R(src) * sa + COLOR_R(dst) * wd) / oa;
    uint32_t g = (COLOR_G(src) * sa + COLOR_G(dst) * wd) / oa;
    uint32_t b = (COLOR_B(src) * sa + COLOR_B(dst) * wd) / oa;
    return RGBA(r, g, b, oa);
}

static inline void plot(surface_t *s, uint32_t *p, uint32_t c, uint32_t a) {
    if (a >= 255) *p = (s->flags & SURF_ALPHA) ? c : (c | 0xFF000000u);
    else if (a) *p = (s->flags & SURF_ALPHA) ? blend_alpha(*p, c, a) : blend_opaque(*p, c, a);
}

/* plot with coverage (0..255) applied to colour alpha */
static inline void plot_cov(surface_t *s, int x, int y, uint32_t c, uint32_t cov) {
    if (x < s->clip.x || y < s->clip.y || x >= s->clip.x + s->clip.w || y >= s->clip.y + s->clip.h) return;
    uint32_t a = COLOR_A(c) * cov / 255;
    plot(s, &s->pixels[(size_t)y * s->pitch + x], c, a);
}

/* ---- surfaces ------------------------------------------------------------------- */
static surface_t *surface_alloc(int w, int h, uint32_t fill, uint32_t flags) {
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    surface_t *s = kmalloc(sizeof *s);
    if (!s) return NULL;
    s->pixels = kmalloc((size_t)w * (size_t)h * 4);
    if (!s->pixels) { kfree(s); return NULL; }
    s->w = w; s->h = h; s->pitch = w;
    s->clip = rect_make(0, 0, w, h);
    s->flags = SURF_OWNED | flags;
    memset32(s->pixels, fill, (size_t)w * (size_t)h);
    return s;
}

surface_t *surface_create(int w, int h) { return surface_alloc(w, h, 0xFF000000u, 0); }
surface_t *surface_create_alpha(int w, int h) { return surface_alloc(w, h, 0, SURF_ALPHA); }

void surface_destroy(surface_t *s) {
    if (!s) return;
    if (s->flags & SURF_OWNED) kfree(s->pixels);
    kfree(s);
}

surface_t *surface_clone(const surface_t *src) {
    surface_t *s = surface_alloc(src->w, src->h, 0, src->flags & SURF_ALPHA);
    if (!s) return NULL;
    for (int y = 0; y < src->h; y++) memcpy(surface_row(s, y), surface_row(src, y), (size_t)src->w * 4);
    return s;
}

surface_t surface_sub(const surface_t *parent, rect_t r) {
    surface_t s;
    r = rect_intersect(r, rect_make(0, 0, parent->w, parent->h));
    s.w = r.w; s.h = r.h;
    s.pitch = parent->pitch;
    s.pixels = parent->pixels + (size_t)r.y * parent->pitch + r.x;
    s.clip = rect_make(0, 0, r.w, r.h);
    s.flags = parent->flags & SURF_ALPHA;
    return s;
}

void surface_set_clip(surface_t *s, rect_t r) { s->clip = rect_intersect(r, rect_make(0, 0, s->w, s->h)); }
void surface_reset_clip(surface_t *s) { s->clip = rect_make(0, 0, s->w, s->h); }

/* ---- primitives ------------------------------------------------------------------- */
void gfx_pixel(surface_t *s, int x, int y, uint32_t c) { plot_cov(s, x, y, c, 255); }

uint32_t gfx_get_pixel(const surface_t *s, int x, int y) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    return s->pixels[(size_t)y * s->pitch + x];
}

void gfx_fill_rect(surface_t *s, int x, int y, int w, int h, uint32_t c) {
    rect_t r = rect_intersect(rect_make(x, y, w, h), s->clip);
    if (rect_empty(r)) return;
    uint32_t a = COLOR_A(c);
    if (a == 0) return;
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        uint32_t *p = s->pixels + (size_t)yy * s->pitch + r.x;
        if (a == 255) {
            memset32(p, (s->flags & SURF_ALPHA) ? c : (c | 0xFF000000u), (size_t)r.w);
        } else if (s->flags & SURF_ALPHA) {
            for (int i = 0; i < r.w; i++) p[i] = blend_alpha(p[i], c, a);
        } else {
            for (int i = 0; i < r.w; i++) p[i] = blend_opaque(p[i], c, a);
        }
    }
}

void gfx_clear(surface_t *s, uint32_t c) {
    rect_t r = s->clip;
    for (int yy = r.y; yy < r.y + r.h; yy++)
        memset32(s->pixels + (size_t)yy * s->pitch + r.x, c, (size_t)r.w);
}

void gfx_hline(surface_t *s, int x, int y, int w, uint32_t c) { gfx_fill_rect(s, x, y, w, 1, c); }
void gfx_vline(surface_t *s, int x, int y, int h, uint32_t c) { gfx_fill_rect(s, x, y, 1, h, c); }

void gfx_rect(surface_t *s, int x, int y, int w, int h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    gfx_fill_rect(s, x, y, w, 1, c);
    if (h > 1) gfx_fill_rect(s, x, y + h - 1, w, 1, c);
    if (h > 2) {
        gfx_fill_rect(s, x, y + 1, 1, h - 2, c);
        if (w > 1) gfx_fill_rect(s, x + w - 1, y + 1, 1, h - 2, c);
    }
}

void gfx_line(surface_t *s, int x0, int y0, int x1, int y1, uint32_t c) {
    if (y0 == y1) { if (x1 < x0) { int t = x0; x0 = x1; x1 = t; } gfx_fill_rect(s, x0, y0, x1 - x0 + 1, 1, c); return; }
    if (x0 == x1) { if (y1 < y0) { int t = y0; y0 = y1; y1 = t; } gfx_fill_rect(s, x0, y0, 1, y1 - y0 + 1, c); return; }
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 100000; guard++) {
        plot_cov(s, x0, y0, c, 255);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Xiaolin Wu anti-aliased line. */
static inline double fpart(double x) { return x - floor(x); }
void gfx_line_aa(surface_t *s, double x0, double y0, double x1, double y1, uint32_t c) {
    bool steep = fabs(y1 - y0) > fabs(x1 - x0);
    double t;
    if (steep) { t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    double dx = x1 - x0, dy = y1 - y0;
    double grad = dx == 0.0 ? 1.0 : dy / dx;
    int xs = (int)floor(x0 + 0.5), xe = (int)floor(x1 + 0.5);
    double y = y0 + grad * (xs - x0);
    if (xe - xs > 20000) return;
    for (int x = xs; x <= xe; x++) {
        int yi = (int)floor(y);
        uint32_t c2 = (uint32_t)(fpart(y) * 255.0);
        uint32_t c1 = 255 - c2;
        if (steep) { plot_cov(s, yi, x, c, c1); plot_cov(s, yi + 1, x, c, c2); }
        else { plot_cov(s, x, yi, c, c1); plot_cov(s, x, yi + 1, c, c2); }
        y += grad;
    }
}

void gfx_thick_line(surface_t *s, int x0, int y0, int x1, int y1, int th, uint32_t c) {
    if (th <= 1) { gfx_line(s, x0, y0, x1, y1, c); return; }
    double dx = x1 - x0, dy = y1 - y0;
    double len = sqrt(dx * dx + dy * dy);
    if (len < 0.001) { gfx_fill_circle(s, x0, y0, th / 2, c); return; }
    double nx = -dy / len * th / 2.0, ny = dx / len * th / 2.0;
    point_t p[4] = {
        { (int)lround(x0 + nx), (int)lround(y0 + ny) }, { (int)lround(x1 + nx), (int)lround(y1 + ny) },
        { (int)lround(x1 - nx), (int)lround(y1 - ny) }, { (int)lround(x0 - nx), (int)lround(y0 - ny) },
    };
    gfx_fill_polygon(s, p, 4, c);
    gfx_fill_circle(s, x0, y0, th / 2, c);
    gfx_fill_circle(s, x1, y1, th / 2, c);
}

void gfx_circle(surface_t *s, int cx, int cy, int r, uint32_t c) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        plot_cov(s, cx + x, cy + y, c, 255); plot_cov(s, cx - x, cy + y, c, 255);
        plot_cov(s, cx + x, cy - y, c, 255); plot_cov(s, cx - x, cy - y, c, 255);
        if (x != y) {
            plot_cov(s, cx + y, cy + x, c, 255); plot_cov(s, cx - y, cy + x, c, 255);
            plot_cov(s, cx + y, cy - x, c, 255); plot_cov(s, cx - y, cy - x, c, 255);
        }
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void gfx_fill_circle(surface_t *s, int cx, int cy, int r, uint32_t c) {
    if (r < 0) return;
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrt((double)(r * r - dy * dy) + 0.25);
        gfx_fill_rect(s, cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}

void gfx_fill_circle_aa(surface_t *s, double cx, double cy, double r, uint32_t c) {
    if (r <= 0) return;
    int y0 = (int)floor(cy - r - 1), y1 = (int)ceil(cy + r + 1);
    y0 = MAX(y0, s->clip.y);
    y1 = MIN(y1, s->clip.y + s->clip.h - 1);
    double ro = r + 0.5, ri = r - 0.5;
    for (int y = y0; y <= y1; y++) {
        double dy = y + 0.5 - cy;
        double d2 = dy * dy;
        if (d2 > ro * ro) continue;
        double xo = sqrt(ro * ro - d2);
        double xi = (ri > 0 && d2 < ri * ri) ? sqrt(ri * ri - d2) : 0.0;
        int xa = (int)floor(cx - xo), xb = (int)ceil(cx + xo);
        int ia = (int)ceil(cx - xi), ib = (int)floor(cx + xi) - 1;   /* solid span [ia, ib] */
        if (xi <= 0) { ia = 1; ib = 0; }
        for (int x = xa; x <= xb; x++) {
            if (x == ia && ib >= ia) {
                gfx_fill_rect(s, ia, y, ib - ia + 1, 1, c);
                x = ib;
                continue;
            }
            double dx = x + 0.5 - cx;
            double d = sqrt(dx * dx + d2);
            double cov = r + 0.5 - d;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            plot_cov(s, x, y, c, (uint32_t)(cov * 255));
        }
    }
}

void gfx_circle_aa(surface_t *s, double cx, double cy, double r, double th, uint32_t c) {
    double ro = r + th / 2, ri = r - th / 2;
    int y0 = (int)floor(cy - ro - 1), y1 = (int)ceil(cy + ro + 1);
    int x0 = (int)floor(cx - ro - 1), x1 = (int)ceil(cx + ro + 1);
    y0 = MAX(y0, s->clip.y); y1 = MIN(y1, s->clip.y + s->clip.h - 1);
    x0 = MAX(x0, s->clip.x); x1 = MIN(x1, s->clip.x + s->clip.w - 1);
    for (int y = y0; y <= y1; y++) {
        double dy = y + 0.5 - cy;
        for (int x = x0; x <= x1; x++) {
            double dx = x + 0.5 - cx;
            double d = sqrt(dx * dx + dy * dy);
            double cov = MIN(ro - d, d - ri) + 0.5;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            plot_cov(s, x, y, c, (uint32_t)(cov * 255));
        }
    }
}

void gfx_ellipse(surface_t *s, int cx, int cy, int rx, int ry, uint32_t c) {
    if (rx <= 0 || ry <= 0) return;
    int n = (rx + ry) * 4;
    int px = cx + rx, py = cy;
    for (int i = 1; i <= n; i++) {
        double a = 2 * M_PI * i / n;
        int x = cx + (int)lround(rx * cos(a)), y = cy + (int)lround(ry * sin(a));
        gfx_line(s, px, py, x, y, c);
        px = x; py = y;
    }
}

void gfx_fill_ellipse(surface_t *s, int cx, int cy, int rx, int ry, uint32_t c) {
    if (rx <= 0 || ry <= 0) return;
    for (int dy = -ry; dy <= ry; dy++) {
        double t = 1.0 - (double)(dy * dy) / ((double)ry * ry);
        int dx = (int)(rx * sqrt(t < 0 ? 0 : t) + 0.5);
        gfx_fill_rect(s, cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}

/* Coverage of a pixel against a circle corner (cx,cy,r) - pixel centre based. */
static inline uint32_t corner_cov(double px, double py, double cx, double cy, double r) {
    double dx = px - cx, dy = py - cy;
    double d = sqrt(dx * dx + dy * dy);
    double cov = r + 0.5 - d;
    if (cov <= 0) return 0;
    if (cov >= 1) return 255;
    return (uint32_t)(cov * 255);
}

/* Fill a rounded rect with a per-row colour function (solid or gradient). */
static void fill_rounded_rows(surface_t *s, int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    for (int yy = 0; yy < h; yy++) {
        int py = y + yy;
        if (py < s->clip.y || py >= s->clip.y + s->clip.h) continue;
        uint32_t c = (top == bottom || h == 1) ? top : color_mix(top, bottom, yy * 255 / (h - 1));
        int inset = 0;
        double ccy = 0;
        bool in_corner = false;
        if (yy < r) { in_corner = true; ccy = y + r; }
        else if (yy >= h - r) { in_corner = true; ccy = y + h - r; }
        if (!in_corner) { gfx_fill_rect(s, x, py, w, 1, c); continue; }
        /* corner rows: AA pixels near the edges, solid middle */
        double pyc = py + 0.5;
        double dy = pyc - ccy;
        double span = sqrt(MAX(0.0, (double)r * r - dy * dy));
        inset = (int)floor(r - span);
        if (inset < 0) inset = 0;
        int solid0 = x + inset + 1, solid1 = x + w - inset - 1;   /* exclusive */
        for (int px = x; px < solid0 && px < x + w; px++) {
            uint32_t cov = corner_cov(px + 0.5, pyc, x + r, ccy, r);
            if (cov) plot_cov(s, px, py, c, cov);
        }
        if (solid1 > solid0) gfx_fill_rect(s, solid0, py, solid1 - solid0, 1, c);
        for (int px = MAX(solid1, solid0); px < x + w; px++) {
            uint32_t cov = corner_cov(px + 0.5, pyc, x + w - r, ccy, r);
            if (cov) plot_cov(s, px, py, c, cov);
        }
    }
}

void gfx_fill_rounded_rect(surface_t *s, int x, int y, int w, int h, int r, uint32_t c) {
    if (r <= 0) { gfx_fill_rect(s, x, y, w, h, c); return; }
    fill_rounded_rows(s, x, y, w, h, r, c, c);
}

void gfx_fill_rounded_gradient_v(surface_t *s, int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom) {
    fill_rounded_rows(s, x, y, w, h, r, top, bottom);
}

void gfx_rounded_rect(surface_t *s, int x, int y, int w, int h, int r, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    if (r <= 0) { gfx_rect(s, x, y, w, h, c); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    gfx_fill_rect(s, x + r, y, w - 2 * r, 1, c);
    gfx_fill_rect(s, x + r, y + h - 1, w - 2 * r, 1, c);
    gfx_fill_rect(s, x, y + r, 1, h - 2 * r, c);
    gfx_fill_rect(s, x + w - 1, y + r, 1, h - 2 * r, c);
    /* AA quarter rings at the corners (1px thick ring with centre line at radius r-0.5) */
    double rr = r - 0.5;
    for (int yy = 0; yy < r; yy++) {
        for (int xx = 0; xx < r; xx++) {
            double dx = r - (xx + 0.5), dy = r - (yy + 0.5);
            double d = sqrt(dx * dx + dy * dy);
            double cov = 1.0 - fabs(d - rr);
            if (cov <= 0) continue;
            uint32_t cv = (uint32_t)(MIN(cov, 1.0) * 255);
            plot_cov(s, x + xx, y + yy, c, cv);
            plot_cov(s, x + w - 1 - xx, y + yy, c, cv);
            plot_cov(s, x + xx, y + h - 1 - yy, c, cv);
            plot_cov(s, x + w - 1 - xx, y + h - 1 - yy, c, cv);
        }
    }
}

void gfx_gradient_v(surface_t *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    for (int i = 0; i < h; i++) {
        uint32_t c = h > 1 ? color_mix(top, bottom, i * 255 / (h - 1)) : top;
        gfx_fill_rect(s, x, y + i, w, 1, c);
    }
}

void gfx_gradient_h(surface_t *s, int x, int y, int w, int h, uint32_t left, uint32_t right) {
    for (int i = 0; i < w; i++) {
        uint32_t c = w > 1 ? color_mix(left, right, i * 255 / (w - 1)) : left;
        gfx_fill_rect(s, x + i, y, 1, h, c);
    }
}

/* Even-odd scanline polygon fill (pixel centres). */
void gfx_fill_polygon(surface_t *s, const point_t *pts, int n, uint32_t c) {
    if (n < 3) return;
    int miny = pts[0].y, maxy = pts[0].y;
    for (int i = 1; i < n; i++) { miny = MIN(miny, pts[i].y); maxy = MAX(maxy, pts[i].y); }
    miny = MAX(miny, s->clip.y);
    maxy = MIN(maxy, s->clip.y + s->clip.h - 1);
    int xs[64];
    for (int y = miny; y <= maxy; y++) {
        int cnt = 0;
        double yc = y + 0.5;
        for (int i = 0, j = n - 1; i < n && cnt < 64; j = i++) {
            double yi = pts[i].y, yj = pts[j].y;
            if ((yi <= yc && yj > yc) || (yj <= yc && yi > yc)) {
                double x = pts[i].x + (yc - yi) * (pts[j].x - pts[i].x) / (yj - yi);
                xs[cnt++] = (int)floor(x + 0.5);
            }
        }
        /* sort intersections */
        for (int a = 1; a < cnt; a++)
            for (int b = a; b > 0 && xs[b - 1] > xs[b]; b--) { int t = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = t; }
        for (int k = 0; k + 1 < cnt; k += 2)
            if (xs[k + 1] > xs[k]) gfx_fill_rect(s, xs[k], y, xs[k + 1] - xs[k], 1, c);
    }
}

void gfx_fill_triangle(surface_t *s, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) {
    point_t p[3] = { { x0, y0 }, { x1, y1 }, { x2, y2 } };
    gfx_fill_polygon(s, p, 3, c);
}

/* ---- soft shadows ----------------------------------------------------------------------- */
#define SH_MAX 64
static uint8_t sh_lut[SH_MAX][SH_MAX];
static int sh_r = -1, sh_spread = -1, sh_alpha = -1;

static void shadow_lut(int radius, int spread, int alpha) {
    if (radius == sh_r && spread == sh_spread && alpha == sh_alpha) return;
    sh_r = radius; sh_spread = spread; sh_alpha = alpha;
    for (int dy = 0; dy < SH_MAX; dy++)
        for (int dx = 0; dx < SH_MAX; dx++) {
            double d = sqrt((double)dx * dx + (double)dy * dy) - radius;   /* distance outside */
            double t = (d + radius * 0.35) / (spread + radius * 0.35);
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            double f = (1 - t) * (1 - t);
            f = f * f * (3 - 2 * f) * 0.6 + f * 0.4;   /* smooth falloff */
            sh_lut[dy][dx] = (uint8_t)(alpha * f);
        }
}

/* Shadow for rect (x,y,w,h); pixels covered by the opaque rounded rect `occ`
   (drawn afterwards by the caller) are skipped. */
void gfx_shadow_occluded(surface_t *s, int x, int y, int w, int h, int radius, int spread, int alpha,
                         rect_t occ, int occ_radius) {
    if (spread + radius >= SH_MAX) spread = SH_MAX - 1 - radius;
    if (spread <= 0) return;
    shadow_lut(radius, spread, alpha);
    rect_t outer = rect_intersect(rect_make(x - spread, y - spread, w + 2 * spread, h + 2 * spread), s->clip);
    if (rect_empty(outer)) return;
    int ix0 = x + radius, ix1 = x + w - radius;      /* inner (straight edge) span */
    int iy0 = y + radius, iy1 = y + h - radius;
    for (int py = outer.y; py < outer.y + outer.h; py++) {
        int dy = py < iy0 ? iy0 - py : (py >= iy1 ? py - iy1 + 1 : 0);
        if (dy >= SH_MAX) continue;
        uint32_t *row = s->pixels + (size_t)py * s->pitch;
        /* skip the part of the row the occluder covers; only its rounded corners
           need shadow underneath */
        int skip0 = 0x7fffffff, skip1 = 0x7fffffff;
        if (py >= occ.y && py < occ.y + occ.h) {
            if (py < occ.y + occ_radius || py >= occ.y + occ.h - occ_radius) { skip0 = occ.x + occ_radius; skip1 = occ.x + occ.w - occ_radius; }
            else { skip0 = occ.x; skip1 = occ.x + occ.w; }
        }
        for (int px = outer.x; px < outer.x + outer.w; px++) {
            if (px >= skip0 && px < skip1) { px = skip1 - 1; continue; }
            int dx = px < ix0 ? ix0 - px : (px >= ix1 ? px - ix1 + 1 : 0);
            if (dx >= SH_MAX) continue;
            uint32_t a = sh_lut[dy][dx];
            if (a) row[px] = blend_opaque(row[px], 0xFF000000u, a);
        }
    }
}

void gfx_shadow(surface_t *s, int x, int y, int w, int h, int radius, int spread, int alpha) {
    gfx_shadow_occluded(s, x, y, w, h, radius, spread, alpha, rect_make(x, y, w, h), radius);
}

/* ---- blitting ------------------------------------------------------------------------ */
/* Clip a blit of size w x h from (sx,sy) to (dx,dy); returns false if nothing remains. */
static bool clip_blit(const surface_t *dst, const surface_t *src, int *dx, int *dy, int *sx, int *sy, int *w, int *h) {
    if (*sx < 0) { *dx -= *sx; *w += *sx; *sx = 0; }
    if (*sy < 0) { *dy -= *sy; *h += *sy; *sy = 0; }
    if (*sx + *w > src->w) *w = src->w - *sx;
    if (*sy + *h > src->h) *h = src->h - *sy;
    rect_t c = dst->clip;
    if (*dx < c.x) { int d = c.x - *dx; *dx += d; *sx += d; *w -= d; }
    if (*dy < c.y) { int d = c.y - *dy; *dy += d; *sy += d; *h -= d; }
    if (*dx + *w > c.x + c.w) *w = c.x + c.w - *dx;
    if (*dy + *h > c.y + c.h) *h = c.y + c.h - *dy;
    return *w > 0 && *h > 0;
}

void gfx_blit_rect(surface_t *dst, int dx, int dy, const surface_t *src, int sx, int sy, int w, int h) {
    if (!clip_blit(dst, src, &dx, &dy, &sx, &sy, &w, &h)) return;
    for (int y = 0; y < h; y++)
        memcpy(dst->pixels + (size_t)(dy + y) * dst->pitch + dx,
               src->pixels + (size_t)(sy + y) * src->pitch + sx, (size_t)w * 4);
}

void gfx_blit(surface_t *dst, int dx, int dy, const surface_t *src) {
    gfx_blit_rect(dst, dx, dy, src, 0, 0, src->w, src->h);
}

void gfx_blit_alpha_rect(surface_t *dst, int dx, int dy, const surface_t *src, int sx, int sy, int w, int h, int opacity) {
    if (opacity <= 0) return;
    if (!clip_blit(dst, src, &dx, &dy, &sx, &sy, &w, &h)) return;
    bool dst_alpha = dst->flags & SURF_ALPHA;
    for (int y = 0; y < h; y++) {
        uint32_t *d = dst->pixels + (size_t)(dy + y) * dst->pitch + dx;
        const uint32_t *s = src->pixels + (size_t)(sy + y) * src->pitch + sx;
        for (int x = 0; x < w; x++) {
            uint32_t c = s[x];
            uint32_t a = COLOR_A(c);
            if (opacity < 255) a = a * (uint32_t)opacity / 255;
            if (!a) continue;
            if (a == 255 && !dst_alpha) d[x] = c;
            else if (dst_alpha) d[x] = blend_alpha(d[x], c, a);
            else d[x] = blend_opaque(d[x], c, a);
        }
    }
}

void gfx_blit_alpha(surface_t *dst, int dx, int dy, const surface_t *src, int opacity) {
    gfx_blit_alpha_rect(dst, dx, dy, src, 0, 0, src->w, src->h, opacity);
}

void gfx_blit_scaled(surface_t *dst, int dx, int dy, int dw, int dh,
                     const surface_t *src, int sx, int sy, int sw, int sh, bool smooth) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    rect_t r = rect_intersect(rect_make(dx, dy, dw, dh), dst->clip);
    if (rect_empty(r)) return;
    bool use_alpha = src->flags & SURF_ALPHA;
    uint32_t fx = (uint32_t)(((uint64_t)sw << 16) / (uint32_t)dw);
    uint32_t fy = (uint32_t)(((uint64_t)sh << 16) / (uint32_t)dh);
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *d = dst->pixels + (size_t)y * dst->pitch;
        if (!smooth) {
            int syy = sy + (int)(((uint32_t)(y - dy) * fy) >> 16);
            if (syy >= src->h) syy = src->h - 1;
            const uint32_t *srow = src->pixels + (size_t)syy * src->pitch;
            for (int x = r.x; x < r.x + r.w; x++) {
                int sxx = sx + (int)(((uint32_t)(x - dx) * fx) >> 16);
                if (sxx >= src->w) sxx = src->w - 1;
                uint32_t c = srow[sxx];
                if (use_alpha) plot(dst, &d[x], c, COLOR_A(c));
                else d[x] = c | 0xFF000000u;
            }
        } else {
            /* bilinear, sample centres */
            int32_t vy = (int32_t)(((int64_t)(y - dy) * fy) + (fy >> 1)) - 32768;
            if (vy < 0) vy = 0;
            int y0 = sy + (vy >> 16), y1 = MIN(y0 + 1, sy + sh - 1);
            y0 = MIN(y0, src->h - 1); y1 = MIN(y1, src->h - 1);
            uint32_t wy = (vy >> 8) & 255;
            const uint32_t *r0 = src->pixels + (size_t)y0 * src->pitch;
            const uint32_t *r1 = src->pixels + (size_t)y1 * src->pitch;
            for (int x = r.x; x < r.x + r.w; x++) {
                int32_t vx = (int32_t)(((int64_t)(x - dx) * fx) + (fx >> 1)) - 32768;
                if (vx < 0) vx = 0;
                int x0 = sx + (vx >> 16), x1 = MIN(x0 + 1, sx + sw - 1);
                x0 = MIN(x0, src->w - 1); x1 = MIN(x1, src->w - 1);
                uint32_t wx = (vx >> 8) & 255;
                uint32_t top = color_mix(r0[x0], r0[x1], (int)wx);
                uint32_t bot = color_mix(r1[x0], r1[x1], (int)wx);
                uint32_t c = color_mix(top, bot, (int)wy);
                if (use_alpha) plot(dst, &d[x], c, COLOR_A(c));
                else d[x] = c | 0xFF000000u;
            }
        }
    }
}

/* Area-averaging downscale (premultiplied), bilinear for upscaling. */
surface_t *surface_scaled(const surface_t *src, int w, int h) {
    surface_t *d = (src->flags & SURF_ALPHA) ? surface_create_alpha(w, h) : surface_create(w, h);
    if (!d) return NULL;
    if (w >= src->w || h >= src->h) {
        if (src->flags & SURF_ALPHA) {
            /* copy straight (no blending onto transparent) */
            surface_t tmp = *d;
            tmp.flags &= ~SURF_ALPHA;
            gfx_blit_scaled(&tmp, 0, 0, w, h, src, 0, 0, src->w, src->h, true);
        } else {
            gfx_blit_scaled(d, 0, 0, w, h, src, 0, 0, src->w, src->h, true);
        }
        return d;
    }
    for (int y = 0; y < h; y++) {
        int sy0 = y * src->h / h, sy1 = MAX(sy0 + 1, (y + 1) * src->h / h);
        for (int x = 0; x < w; x++) {
            int sx0 = x * src->w / w, sx1 = MAX(sx0 + 1, (x + 1) * src->w / w);
            uint32_t ar = 0, ag = 0, ab = 0, aa = 0, n = 0;
            for (int yy = sy0; yy < sy1; yy++) {
                const uint32_t *row = surface_row(src, yy);
                for (int xx = sx0; xx < sx1; xx++) {
                    uint32_t c = row[xx];
                    uint32_t a = (src->flags & SURF_ALPHA) ? COLOR_A(c) : 255;
                    ar += COLOR_R(c) * a; ag += COLOR_G(c) * a; ab += COLOR_B(c) * a; aa += a; n++;
                }
            }
            uint32_t out = 0;
            if (aa) out = RGBA(ar / aa, ag / aa, ab / aa, aa / n);
            surface_row(d, y)[x] = (src->flags & SURF_ALPHA) ? out : (out | 0xFF000000u);
        }
    }
    return d;
}
