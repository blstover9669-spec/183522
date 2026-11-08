/*
 * web_svg.c - draws <svg> elements of a page: paths and the basic shapes, groups,
 * <use> of symbols, fill / stroke colours, transforms, viewBox scaling.
 *
 * Shapes are flattened to polygons and filled with 4 sub-scanlines per pixel row
 * (nonzero or even-odd), strokes become one quad per segment plus round joins.
 * The result is cached per element and size, so repaints only blit it.
 */
#include "web.h"
#include <math.h>

#define SV_FP 256            /* fixed point: 1/256 px */
#define SV_MAX_EDGES 60000
#define SV_DEPTH 24

typedef struct { float a, b, c, d, e, f; } mat_t;      /* x' = a x + c y + e, y' = b x + d y + f */

typedef struct sedge { int x0, y0, x1, y1; int8_t dir; } sedge_t;

typedef struct spoly {
    sedge_t *e;
    int n, cap;
    float sx, sy, cx, cy;    /* subpath start / current point (device) */
    bool has;                /* a subpath is open */
    bool overflow;
} spoly_t;

/* drawing state inherited down the tree */
typedef struct sstate {
    uint32_t fill, stroke;   /* alpha 0 = none */
    float sw;                /* stroke width (user units) */
    bool evenodd;
    float opacity;
    uint32_t cur;            /* currentColor */
} sstate_t;

typedef struct sctx {
    surface_t *s;
    const web_doc_t *doc;
    int depth;
    int uses;                /* <use> expansions (bounded) */
    int in_use;              /* inside a <use>: colours come from the <use>, not the original */
    float vpw, vph;          /* the viewport (user units): 100% of a <use> */
} sctx_t;

static mat_t m_mul(mat_t m, mat_t n) {   /* m then n: apply n first, then m */
    mat_t r;
    r.a = m.a * n.a + m.c * n.b;
    r.b = m.b * n.a + m.d * n.b;
    r.c = m.a * n.c + m.c * n.d;
    r.d = m.b * n.c + m.d * n.d;
    r.e = m.a * n.e + m.c * n.f + m.e;
    r.f = m.b * n.e + m.d * n.f + m.f;
    return r;
}

static mat_t m_tr(float x, float y) { mat_t m = { 1, 0, 0, 1, x, y }; return m; }
static mat_t m_sc(float x, float y) { mat_t m = { x, 0, 0, y, 0, 0 }; return m; }

static void m_apply(const mat_t *m, float x, float y, float *ox, float *oy) {
    *ox = m->a * x + m->c * y + m->e;
    *oy = m->b * x + m->d * y + m->f;
}

/* ---- numbers ---------------------------------------------------------------------------- */
static const char *sv_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
    return p;
}

static bool sv_num(const char **pp, float *out) {
    const char *p = sv_ws(*pp);
    char *e;
    double v = strtod(p, &e);
    if (e == p) return false;
    /* strtod would take "1e" / hex: SVG numbers never contain 'x' */
    *out = (float)v;
    *pp = e;
    return true;
}

static float sv_attr_num(const web_node_t *n, const char *name, float def) {
    const char *a = web_attr(n, name);
    if (!a || !*a) return def;
    float v;
    const char *p = a;
    if (!sv_num(&p, &v)) return def;
    return v;
}

/* ---- polygons ----------------------------------------------------------------------------- */
static float sv_clampf(float v) {
    if (!(v == v)) return 0;                 /* NaN */
    return v < -1e6f ? -1e6f : v > 1e6f ? 1e6f : v;
}

static void p_edge(spoly_t *P, float x0, float y0, float x1, float y1) {
    /* (absurd transforms: keep the fixed-point coordinates in range) */
    x0 = sv_clampf(x0); y0 = sv_clampf(y0); x1 = sv_clampf(x1); y1 = sv_clampf(y1);
    int ix0 = (int)lroundf(x0 * SV_FP), iy0 = (int)lroundf(y0 * SV_FP);
    int ix1 = (int)lroundf(x1 * SV_FP), iy1 = (int)lroundf(y1 * SV_FP);
    if (iy0 == iy1) return;                 /* horizontal edges never cross a scanline */
    if (P->n >= P->cap) {
        if (P->cap >= SV_MAX_EDGES) { P->overflow = true; return; }
        int nc = P->cap ? P->cap * 2 : 64;
        sedge_t *ne = realloc(P->e, sizeof *ne * nc);
        if (!ne) { P->overflow = true; return; }
        P->e = ne;
        P->cap = nc;
    }
    sedge_t *e = &P->e[P->n++];
    if (iy0 < iy1) { e->x0 = ix0; e->y0 = iy0; e->x1 = ix1; e->y1 = iy1; e->dir = 1; }
    else { e->x0 = ix1; e->y0 = iy1; e->x1 = ix0; e->y1 = iy0; e->dir = -1; }
}

static void p_move(spoly_t *P, float x, float y) {
    if (P->has && (P->cx != P->sx || P->cy != P->sy)) p_edge(P, P->cx, P->cy, P->sx, P->sy);   /* fills close */
    P->sx = P->cx = x;
    P->sy = P->cy = y;
    P->has = true;
}

static void p_line(spoly_t *P, float x, float y) {
    if (!P->has) { p_move(P, x, y); return; }
    p_edge(P, P->cx, P->cy, x, y);
    P->cx = x;
    P->cy = y;
}

static void p_close(spoly_t *P) {
    if (P->has && (P->cx != P->sx || P->cy != P->sy)) p_edge(P, P->cx, P->cy, P->sx, P->sy);
    P->cx = P->sx;
    P->cy = P->sy;
}

/* fill the polygon into s with colour c (its alpha times coverage) */
static void p_fill(surface_t *s, spoly_t *P, uint32_t c, bool evenodd) {
    if (P->has) p_close(P);
    if (!P->n || !COLOR_A(c)) return;
    int ymin = INT_MAX, ymax = INT_MIN, xmin = INT_MAX, xmax = INT_MIN;
    for (int i = 0; i < P->n; i++) {
        sedge_t *e = &P->e[i];
        ymin = MIN(ymin, e->y0); ymax = MAX(ymax, e->y1);
        xmin = MIN(xmin, MIN(e->x0, e->x1)); xmax = MAX(xmax, MAX(e->x0, e->x1));
    }
    int py0 = MAX(ymin / SV_FP - 1, s->clip.y), py1 = MIN(ymax / SV_FP + 1, s->clip.y + s->clip.h - 1);
    int px0 = MAX(xmin / SV_FP - 1, s->clip.x), px1 = MIN(xmax / SV_FP + 1, s->clip.x + s->clip.w - 1);
    if (py0 > py1 || px0 > px1) return;
    int w = px1 - px0 + 1;
    uint16_t *cov = malloc(sizeof(uint16_t) * (size_t)w);
    struct cross { int x; int8_t d; } *xs = malloc(sizeof *xs * (size_t)(P->n + 1));
    if (!cov || !xs) { free(cov); free(xs); return; }
    uint32_t ca = COLOR_A(c);
    for (int py = py0; py <= py1; py++) {
        memset(cov, 0, sizeof(uint16_t) * (size_t)w);
        bool any = false;
        for (int sub = 0; sub < 4; sub++) {
            int ys = py * SV_FP + SV_FP / 8 + sub * SV_FP / 4;
            int nx = 0;
            for (int i = 0; i < P->n; i++) {
                sedge_t *e = &P->e[i];
                if (ys < e->y0 || ys >= e->y1) continue;
                int64_t t = (int64_t)(ys - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0);
                xs[nx].x = e->x0 + (int)t;
                xs[nx].d = e->dir;
                nx++;
            }
            if (nx < 2) continue;
            /* insertion sort: few crossings per line */
            for (int i = 1; i < nx; i++) {
                struct cross k = xs[i];
                int j = i - 1;
                while (j >= 0 && xs[j].x > k.x) { xs[j + 1] = xs[j]; j--; }
                xs[j + 1] = k;
            }
            int wind = 0;
            for (int i = 0; i + 1 < nx; i++) {
                wind += evenodd ? 1 : xs[i].d;
                bool in = evenodd ? (wind & 1) : wind != 0;
                if (!in) continue;
                int l = xs[i].x, r = xs[i + 1].x;
                if (r <= l) continue;
                int pl = l >= 0 ? l / SV_FP : -((-l + SV_FP - 1) / SV_FP), pr = (r - 1) >= 0 ? (r - 1) / SV_FP : -1;
                for (int px = MAX(pl, px0); px <= pr && px <= px1; px++) {
                    int a = MAX(l, px * SV_FP), b = MIN(r, (px + 1) * SV_FP);
                    if (b > a) { cov[px - px0] = (uint16_t)(cov[px - px0] + (b - a)); any = true; }
                }
            }
        }
        if (!any) continue;
        for (int i = 0; i < w; i++) {
            if (!cov[i]) continue;
            uint32_t a = ca * cov[i] / (4 * SV_FP);
            if (a > ca) a = ca;
            if (a) gfx_pixel(s, px0 + i, py, color_alpha(c, (int)a));
        }
    }
    free(cov);
    free(xs);
}

/* ---- strokes: a quad per segment and round joins, all filled with nonzero ---------------- */
typedef struct pts { float *v; int n, cap; bool closed; } pts_t;   /* device points of one subpath */

static void pts_add(pts_t *p, float x, float y) {
    if (p->n >= p->cap) {
        int nc = p->cap ? p->cap * 2 : 32;
        if (nc > 200000) return;
        float *nv = realloc(p->v, sizeof(float) * 2 * nc);
        if (!nv) return;
        p->v = nv;
        p->cap = nc;
    }
    p->v[2 * p->n] = x;
    p->v[2 * p->n + 1] = y;
    p->n++;
}

static void stroke_disc(spoly_t *S, float cx, float cy, float r) {
    int k = r > 6 ? 16 : 8;
    for (int i = 0; i <= k; i++) {
        float a = -(float)i * 6.2831853f / k;        /* same turning direction as the quads */
        float x = cx + r * cosf(a), y = cy + r * sinf(a);
        if (!i) p_move(S, x, y); else p_line(S, x, y);
    }
    p_close(S);
    S->has = false;
}

static void stroke_path(spoly_t *S, const pts_t *p, float hw) {
    for (int i = 0; i + 1 < p->n; i++) {
        float x0 = p->v[2 * i], y0 = p->v[2 * i + 1], x1 = p->v[2 * i + 2], y1 = p->v[2 * i + 3];
        float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
        if (len < 1e-4f) continue;
        float nx = -dy / len * hw, ny = dx / len * hw;
        p_move(S, x0 + nx, y0 + ny);
        p_line(S, x1 + nx, y1 + ny);
        p_line(S, x1 - nx, y1 - ny);
        p_line(S, x0 - nx, y0 - ny);
        p_close(S);
        S->has = false;
        if (i + 2 < p->n || p->closed) stroke_disc(S, x1, y1, hw);
    }
}

/* ---- path data -------------------------------------------------------------------------------- */
typedef struct pbuild {
    spoly_t *P;              /* fill polygon (device) */
    pts_t *sub;              /* stroke subpaths (device), or NULL */
    int nsub, subcap;
    mat_t m;
    float tol;               /* curve segments per device pixel of size */
} pbuild_t;

static pts_t *pb_newsub(pbuild_t *b) {
    if (!b->sub && b->subcap < 0) return NULL;
    if (b->nsub >= b->subcap) {
        int nc = b->subcap ? b->subcap * 2 : 8;
        if (nc > 4096) return NULL;
        pts_t *ns = realloc(b->sub, sizeof *ns * nc);
        if (!ns) return NULL;
        b->sub = ns;
        b->subcap = nc;
    }
    pts_t *p = &b->sub[b->nsub++];
    memset(p, 0, sizeof *p);
    return p;
}

static void pb_move(pbuild_t *b, float x, float y, pts_t **cur) {
    float dx, dy;
    m_apply(&b->m, x, y, &dx, &dy);
    p_move(b->P, dx, dy);
    if (b->subcap >= 0) { *cur = pb_newsub(b); if (*cur) pts_add(*cur, dx, dy); }
}

static void pb_line(pbuild_t *b, float x, float y, pts_t **cur) {
    float dx, dy;
    m_apply(&b->m, x, y, &dx, &dy);
    p_line(b->P, dx, dy);
    if (*cur) pts_add(*cur, dx, dy);
}

static int curve_steps(pbuild_t *b, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) {
    float l = fabsf(x1 - x0) + fabsf(y1 - y0) + fabsf(x2 - x1) + fabsf(y2 - y1) + fabsf(x3 - x2) + fabsf(y3 - y2);
    float scale = sqrtf(fabsf(b->m.a * b->m.d - b->m.b * b->m.c));
    float f = l * scale / 3.0f;
    if (!(f < 38.0f)) return 40;               /* (also NaN / huge) */
    return (int)f + 2;
}

static void pb_cubic(pbuild_t *b, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, pts_t **cur) {
    int n = curve_steps(b, x0, y0, x1, y1, x2, y2, x3, y3);
    for (int i = 1; i <= n; i++) {
        float t = (float)i / n, u = 1 - t;
        float x = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
        float y = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
        pb_line(b, x, y, cur);
    }
}

static void pb_quad(pbuild_t *b, float x0, float y0, float x1, float y1, float x2, float y2, pts_t **cur) {
    int n = curve_steps(b, x0, y0, x1, y1, x1, y1, x2, y2);
    for (int i = 1; i <= n; i++) {
        float t = (float)i / n, u = 1 - t;
        pb_line(b, u * u * x0 + 2 * u * t * x1 + t * t * x2, u * u * y0 + 2 * u * t * y1 + t * t * y2, cur);
    }
}

/* elliptical arc (SVG endpoint parameterisation) */
static void pb_arc(pbuild_t *b, float x0, float y0, float rx, float ry, float rot, bool large, bool sweep,
                   float x1, float y1, pts_t **cur) {
    rx = fabsf(rx); ry = fabsf(ry);
    if (rx < 1e-6f || ry < 1e-6f || (x0 == x1 && y0 == y1)) { pb_line(b, x1, y1, cur); return; }
    float phi = rot * 3.14159265f / 180.0f, cp = cosf(phi), sp = sinf(phi);
    float dx = (x0 - x1) / 2, dy = (y0 - y1) / 2;
    float x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
    float lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if (lam > 1) { float s = sqrtf(lam); rx *= s; ry *= s; }
    float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    float co = den > 0 && num > 0 ? sqrtf(num / den) : 0;
    if (large == sweep) co = -co;
    float cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
    float cx = cp * cxp - sp * cyp + (x0 + x1) / 2, cy = sp * cxp + cp * cyp + (y0 + y1) / 2;
    float a1 = atan2f((y1p - cyp) / ry, (x1p - cxp) / rx);
    float a2 = atan2f((-y1p - cyp) / ry, (-x1p - cxp) / rx);
    float da = a2 - a1;
    if (sweep && da < 0) da += 6.2831853f;
    else if (!sweep && da > 0) da -= 6.2831853f;
    float scale = sqrtf(fabsf(b->m.a * b->m.d - b->m.b * b->m.c));
    float fn = fabsf(da) * MAX(rx, ry) * scale / 3.0f;
    int n = !(fn < 58.0f) ? 60 : (int)fn + 2;
    for (int i = 1; i <= n; i++) {
        float a = a1 + da * i / n;
        float ex = rx * cosf(a), ey = ry * sinf(a);
        pb_line(b, cp * ex - sp * ey + cx, sp * ex + cp * ey + cy, cur);
    }
}

static void path_data(pbuild_t *b, const char *d) {
    const char *p = d;
    char cmd = 0;
    float cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0;   /* current, subpath start, last control */
    char last = 0;
    pts_t *cur = NULL;
    for (int guard = 0; guard < 200000; guard++) {
        p = sv_ws(p);
        if (!*p) break;
        if (isalpha((unsigned char)*p)) { cmd = *p++; }
        else if (!cmd) break;
        bool rel = islower((unsigned char)cmd);
        char C = (char)toupper((unsigned char)cmd);
        float v[7];
        switch (C) {
        case 'Z':
            p_close(b->P);
            if (cur) { cur->closed = true; if (cur->n) pts_add(cur, cur->v[0], cur->v[1]); }
            cur = NULL;
            cx = sx; cy = sy;
            last = 'Z';
            cmd = 0;          /* a number after Z starts nothing */
            continue;
        case 'M': case 'L': case 'T': {
            if (!sv_num(&p, &v[0]) || !sv_num(&p, &v[1])) return;
            if (rel) { v[0] += cx; v[1] += cy; }
            if (C == 'M') {
                pb_move(b, v[0], v[1], &cur);
                sx = v[0]; sy = v[1];
                cmd = rel ? 'l' : 'L';       /* following pairs are lines */
            } else if (C == 'L') pb_line(b, v[0], v[1], &cur);
            else {
                float qx = (last == 'Q' || last == 'T') ? 2 * cx - lcx : cx, qy = (last == 'Q' || last == 'T') ? 2 * cy - lcy : cy;
                pb_quad(b, cx, cy, qx, qy, v[0], v[1], &cur);
                lcx = qx; lcy = qy;
            }
            cx = v[0]; cy = v[1];
            last = C;
            break;
        }
        case 'H': case 'V':
            if (!sv_num(&p, &v[0])) return;
            if (C == 'H') cx = rel ? cx + v[0] : v[0];
            else cy = rel ? cy + v[0] : v[0];
            pb_line(b, cx, cy, &cur);
            last = C;
            break;
        case 'C': case 'S': case 'Q': {
            int k = C == 'C' ? 6 : 4;
            for (int i = 0; i < k; i++) if (!sv_num(&p, &v[i])) return;
            if (rel) for (int i = 0; i < k; i += 2) { v[i] += cx; v[i + 1] += cy; }
            if (C == 'C') {
                pb_cubic(b, cx, cy, v[0], v[1], v[2], v[3], v[4], v[5], &cur);
                lcx = v[2]; lcy = v[3]; cx = v[4]; cy = v[5];
            } else if (C == 'S') {
                float c1x = (last == 'C' || last == 'S') ? 2 * cx - lcx : cx, c1y = (last == 'C' || last == 'S') ? 2 * cy - lcy : cy;
                pb_cubic(b, cx, cy, c1x, c1y, v[0], v[1], v[2], v[3], &cur);
                lcx = v[0]; lcy = v[1]; cx = v[2]; cy = v[3];
            } else {
                pb_quad(b, cx, cy, v[0], v[1], v[2], v[3], &cur);
                lcx = v[0]; lcy = v[1]; cx = v[2]; cy = v[3];
            }
            last = C;
            break;
        }
        case 'A': {
            if (!sv_num(&p, &v[0]) || !sv_num(&p, &v[1]) || !sv_num(&p, &v[2])) return;
            /* the two flags may be written without separators ("a1 1 0 01 5 5") */
            int fl[2];
            for (int i = 0; i < 2; i++) {
                p = sv_ws(p);
                if (*p != '0' && *p != '1') return;
                fl[i] = *p++ - '0';
            }
            if (!sv_num(&p, &v[5]) || !sv_num(&p, &v[6])) return;
            if (rel) { v[5] += cx; v[6] += cy; }
            pb_arc(b, cx, cy, v[0], v[1], v[2], fl[0], fl[1], v[5], v[6], &cur);
            cx = v[5]; cy = v[6];
            last = 'A';
            break;
        }
        default:
            return;
        }
        if (b->P->overflow) return;
    }
}

/* ---- attributes / styles ----------------------------------------------------------------------- */
/* a paint value: none, currentColor, a colour, url(#gradient) (its first stop colour) */
static bool sv_paint(sctx_t *cx, const char *v, uint32_t cur, uint32_t *out) {
    while (*v == ' ') v++;
    if (!strncasecmp(v, "none", 4) || !strncasecmp(v, "transparent", 11)) { *out = 0; return true; }
    if (!strncasecmp(v, "currentcolor", 12)) { *out = cur; return true; }
    if (!strncasecmp(v, "url(", 4)) {
        const char *h = strchr(v, '#');
        if (h && cx->doc) {
            char id[128];
            size_t k = 0;
            for (h++; *h && *h != ')' && *h != '"' && *h != '\'' && k < sizeof id - 1; h++) id[k++] = *h;
            id[k] = 0;
            for (const web_node_t *n = cx->doc->root; n;) {
                if (n->type == WN_ELEM && n->id && !strcmp(n->id, id)) {
                    for (const web_node_t *s = n->first; s; s = s->next)
                        if (s->type == WN_ELEM && s->name && !strcmp(s->name, "stop")) {
                            const char *sc = web_attr(s, "stop-color");
                            uint32_t c;
                            if (sc && web_css_color(sc, &c)) { *out = c; return true; }
                            const char *st = web_attr(s, "style");
                            if (st && (st = strstr(st, "stop-color:"))) {
                                char buf[64];
                                strlcpy(buf, st + 11, sizeof buf);
                                char *e = strchr(buf, ';');
                                if (e) *e = 0;
                                if (web_css_color(buf, &c)) { *out = c; return true; }
                            }
                        }
                    break;
                }
                if (n->first) n = n->first;
                else { while (n && !n->next) n = n->parent; if (n) n = n->next; }
            }
        }
        /* a fallback colour may follow the url() */
        const char *f = strchr(v, ')');
        if (f && web_css_color(f + 1, out)) return true;
        *out = cur;
        return true;
    }
    return web_css_color(v, out);
}

/* an opacity value in 0..1 (NaN and junk: 0) */
static float sv_unit(const char *s) {
    float v = (float)atof(s);
    if (!(v >= 0)) return 0;
    return v > 1 ? 1 : v;
}

/* presentation attributes and style="" of one element */
static bool sv_style(sctx_t *cx, const web_node_t *n, sstate_t *st) {
    if (n->st && !cx->in_use) st->cur = n->st->color;
    const char *props[] = { "fill", "stroke", "stroke-width", "fill-rule", "opacity", "fill-opacity",
                            "stroke-opacity", "display", "visibility", "color" };
    char vals[10][64];
    bool have[10] = { 0 };
    for (int i = 0; i < 10; i++) {
        const char *a = web_attr(n, props[i]);
        if (a) { strlcpy(vals[i], a, sizeof vals[i]); have[i] = true; }
    }
    const char *style = web_attr(n, "style");
    if (style) {
        /* "fill: red; stroke-width: 2" */
        const char *p = style;
        while (*p) {
            while (*p == ' ' || *p == ';') p++;
            const char *ns = p;
            while (*p && *p != ':' && *p != ';') p++;
            if (*p != ':') { while (*p && *p != ';') p++; continue; }
            size_t nl = (size_t)(p - ns);
            while (nl && ns[nl - 1] == ' ') nl--;
            p++;
            while (*p == ' ') p++;
            const char *vs = p;
            while (*p && *p != ';') p++;
            size_t vl = (size_t)(p - vs);
            for (int i = 0; i < 10; i++)
                if (strlen(props[i]) == nl && !strncasecmp(props[i], ns, nl)) {
                    if (vl >= sizeof vals[i]) vl = sizeof vals[i] - 1;
                    memcpy(vals[i], vs, vl);
                    vals[i][vl] = 0;
                    have[i] = true;
                }
        }
    }
    /* CSS fill set on this element beats its fill="" attribute */
    if (n->st && n->st->fill_own && n->st->fill_mode) {
        have[0] = false;
        st->fill = n->st->fill_mode == 1 ? n->st->fill : n->st->fill_mode == 2 ? st->cur : 0;
    }
    if (have[7] && !strncasecmp(vals[7], "none", 4)) return false;
    if (have[8] && (!strncasecmp(vals[8], "hidden", 6) || !strncasecmp(vals[8], "collapse", 8))) return false;
    if (have[9]) { uint32_t c; if (web_css_color(vals[9], &c)) st->cur = c; }
    uint32_t c;
    if (have[0] && sv_paint(cx, vals[0], st->cur, &c)) st->fill = c;
    if (have[1] && sv_paint(cx, vals[1], st->cur, &c)) st->stroke = c;
    if (have[2]) { float w = (float)atof(vals[2]); if (w >= 0 && w < 1e5f) st->sw = w; }
    if (have[3]) st->evenodd = !strncasecmp(vals[3], "evenodd", 7);
    if (have[4]) st->opacity *= sv_unit(vals[4]);
    if (have[5]) { float o = sv_unit(vals[5]); st->fill = color_alpha(st->fill, (int)(COLOR_A(st->fill) * o)); }
    if (have[6]) { float o = sv_unit(vals[6]); st->stroke = color_alpha(st->stroke, (int)(COLOR_A(st->stroke) * o)); }
    return true;
}

static mat_t sv_transform(const char *t, mat_t m) {
    const char *p = t;
    for (int guard = 0; guard < 32 && *p; guard++) {
        p = sv_ws(p);
        const char *ns = p;
        while (*p && *p != '(') p++;
        if (!*p) break;
        size_t nl = (size_t)(p - ns);
        while (nl && ns[nl - 1] == ' ') nl--;
        p++;
        float a[6] = { 0 };
        int k = 0;
        while (k < 6 && sv_num(&p, &a[k])) k++;
        p = sv_ws(p);
        if (*p == ')') p++;
        mat_t t2 = { 1, 0, 0, 1, 0, 0 };
        if (nl == 6 && !strncmp(ns, "matrix", 6) && k == 6) { t2.a = a[0]; t2.b = a[1]; t2.c = a[2]; t2.d = a[3]; t2.e = a[4]; t2.f = a[5]; }
        else if (nl == 9 && !strncmp(ns, "translate", 9)) t2 = m_tr(a[0], k > 1 ? a[1] : 0);
        else if (nl == 5 && !strncmp(ns, "scale", 5)) t2 = m_sc(a[0], k > 1 ? a[1] : a[0]);
        else if (nl == 6 && !strncmp(ns, "rotate", 6)) {
            float r = a[0] * 3.14159265f / 180.0f, c = cosf(r), s = sinf(r);
            mat_t rm = { c, s, -s, c, 0, 0 };
            if (k == 3) t2 = m_mul(m_tr(a[1], a[2]), m_mul(rm, m_tr(-a[1], -a[2])));
            else t2 = rm;
        } else if (nl == 5 && !strncmp(ns, "skewX", 5)) { t2.c = tanf(a[0] * 3.14159265f / 180.0f); }
        else if (nl == 5 && !strncmp(ns, "skewY", 5)) { t2.b = tanf(a[0] * 3.14159265f / 180.0f); }
        m = m_mul(m, t2);
    }
    return m;
}

/* viewBox + preserveAspectRatio mapping of a viewport (x, y, w, h user units) */
static mat_t sv_viewbox(const web_node_t *n, float x, float y, float w, float h, mat_t m) {
    const char *vb = web_attr(n, "viewbox");
    float v[4];
    const char *p = vb;
    if (!vb || !sv_num(&p, &v[0]) || !sv_num(&p, &v[1]) || !sv_num(&p, &v[2]) || !sv_num(&p, &v[3]) || v[2] <= 0 || v[3] <= 0)
        return m_mul(m, m_tr(x, y));
    const char *par = web_attr(n, "preserveaspectratio");
    float sx = w / v[2], sy = h / v[3];
    if (par && !strncmp(par, "none", 4)) return m_mul(m, m_mul(m_tr(x, y), m_mul(m_sc(sx, sy), m_tr(-v[0], -v[1]))));
    bool slice = par && strstr(par, "slice");
    float s = slice ? MAX(sx, sy) : MIN(sx, sy);
    float ax = 0.5f, ay = 0.5f;
    if (par) {
        if (strstr(par, "xMin")) ax = 0; else if (strstr(par, "xMax")) ax = 1;
        if (strstr(par, "YMin")) ay = 0; else if (strstr(par, "YMax")) ay = 1;
    }
    float tx = x + (w - v[2] * s) * ax, ty = y + (h - v[3] * s) * ay;
    return m_mul(m, m_mul(m_tr(tx, ty), m_mul(m_sc(s, s), m_tr(-v[0], -v[1]))));
}

/* the user-unit size of a viewport: its viewBox, else its width / height */
static void sv_vbsize(const web_node_t *n, float w, float h, float *vw, float *vh) {
    const char *vb = web_attr(n, "viewbox");
    float v[4];
    const char *p = vb;
    if (vb && sv_num(&p, &v[0]) && sv_num(&p, &v[1]) && sv_num(&p, &v[2]) && sv_num(&p, &v[3]) && v[2] > 0 && v[3] > 0) {
        *vw = v[2];
        *vh = v[3];
    } else {
        *vw = w;
        *vh = h;
    }
}

static const web_node_t *sv_find_id(const web_doc_t *d, const char *id) {
    if (!d || !id || !*id) return NULL;
    for (const web_node_t *n = d->root; n;) {
        if (n->type == WN_ELEM && n->id && !strcmp(n->id, id)) return n;
        if (n->first) n = n->first;
        else { while (n && !n->next) n = n->parent; if (n) n = n->next; }
    }
    return NULL;
}

/* ---- drawing ---------------------------------------------------------------------------------- */
static void sv_node(sctx_t *cx, const web_node_t *n, mat_t m, sstate_t st);

static void sv_children(sctx_t *cx, const web_node_t *n, mat_t m, const sstate_t *st) {
    for (const web_node_t *c = n->first; c; c = c->next)
        if (c->type == WN_ELEM) sv_node(cx, c, m, *st);
}

static void sv_shape(sctx_t *cx, const web_node_t *n, const char *tag, mat_t m, const sstate_t *st) {
    bool fill = COLOR_A(st->fill) > 0, stroke = COLOR_A(st->stroke) > 0 && st->sw > 0;
    if (!fill && !stroke) return;
    spoly_t P;
    memset(&P, 0, sizeof P);
    pbuild_t b;
    memset(&b, 0, sizeof b);
    b.P = &P;
    b.m = m;
    b.subcap = stroke ? 0 : -1;
    pts_t *cur = NULL;
    if (!strcmp(tag, "path")) {
        const char *d = web_attr(n, "d");
        if (d) path_data(&b, d);
    } else if (!strcmp(tag, "rect")) {
        float x = sv_attr_num(n, "x", 0), y = sv_attr_num(n, "y", 0), w = sv_attr_num(n, "width", 0), h = sv_attr_num(n, "height", 0);
        float rx = sv_attr_num(n, "rx", -1), ry = sv_attr_num(n, "ry", -1);
        if (rx < 0) rx = ry;
        if (ry < 0) ry = rx;
        if (rx < 0) rx = ry = 0;
        rx = MIN(rx, w / 2); ry = MIN(ry, h / 2);
        if (w > 0 && h > 0) {
            if (rx > 0 && ry > 0) {
                pb_move(&b, x + rx, y, &cur);
                pb_line(&b, x + w - rx, y, &cur);
                pb_arc(&b, x + w - rx, y, rx, ry, 0, false, true, x + w, y + ry, &cur);
                pb_line(&b, x + w, y + h - ry, &cur);
                pb_arc(&b, x + w, y + h - ry, rx, ry, 0, false, true, x + w - rx, y + h, &cur);
                pb_line(&b, x + rx, y + h, &cur);
                pb_arc(&b, x + rx, y + h, rx, ry, 0, false, true, x, y + h - ry, &cur);
                pb_line(&b, x, y + ry, &cur);
                pb_arc(&b, x, y + ry, rx, ry, 0, false, true, x + rx, y, &cur);
            } else {
                pb_move(&b, x, y, &cur);
                pb_line(&b, x + w, y, &cur);
                pb_line(&b, x + w, y + h, &cur);
                pb_line(&b, x, y + h, &cur);
                pb_line(&b, x, y, &cur);
            }
            if (cur) cur->closed = true;
        }
    } else if (!strcmp(tag, "circle") || !strcmp(tag, "ellipse")) {
        float ccx = sv_attr_num(n, "cx", 0), ccy = sv_attr_num(n, "cy", 0);
        float rx = tag[0] == 'c' ? sv_attr_num(n, "r", 0) : sv_attr_num(n, "rx", 0);
        float ry = tag[0] == 'c' ? rx : sv_attr_num(n, "ry", 0);
        if (rx > 0 && ry > 0) {
            pb_move(&b, ccx + rx, ccy, &cur);
            pb_arc(&b, ccx + rx, ccy, rx, ry, 0, false, true, ccx - rx, ccy, &cur);
            pb_arc(&b, ccx - rx, ccy, rx, ry, 0, false, true, ccx + rx, ccy, &cur);
            if (cur) cur->closed = true;
        }
    } else if (!strcmp(tag, "line")) {
        pb_move(&b, sv_attr_num(n, "x1", 0), sv_attr_num(n, "y1", 0), &cur);
        pb_line(&b, sv_attr_num(n, "x2", 0), sv_attr_num(n, "y2", 0), &cur);
        fill = false;
    } else if (!strcmp(tag, "polyline") || !strcmp(tag, "polygon")) {
        const char *p = web_attr(n, "points");
        float x, y;
        bool first = true;
        while (p && sv_num(&p, &x) && sv_num(&p, &y)) {
            if (first) pb_move(&b, x, y, &cur); else pb_line(&b, x, y, &cur);
            first = false;
        }
        if (tag[4] == 'g' && cur) { cur->closed = true; if (cur->n) pts_add(cur, cur->v[0], cur->v[1]); }
    }
    if (fill && !P.overflow) {
        uint32_t c = color_alpha(st->fill, (int)(COLOR_A(st->fill) * st->opacity));
        p_fill(cx->s, &P, c, st->evenodd);
    }
    if (stroke && b.sub) {
        float scale = sqrtf(fabsf(m.a * m.d - m.b * m.c));
        float hw = st->sw * scale / 2;
        if (hw < 0.35f) hw = 0.35f;
        spoly_t S;
        memset(&S, 0, sizeof S);
        for (int i = 0; i < b.nsub; i++) stroke_path(&S, &b.sub[i], hw);
        uint32_t c = color_alpha(st->stroke, (int)(COLOR_A(st->stroke) * st->opacity));
        if (!S.overflow) p_fill(cx->s, &S, c, false);
        free(S.e);
    }
    for (int i = 0; i < b.nsub; i++) free(b.sub[i].v);
    free(b.sub);
    free(P.e);
}

static void sv_node(sctx_t *cx, const web_node_t *n, mat_t m, sstate_t st) {
    if (cx->depth > SV_DEPTH || !n->name) return;
    const char *tag = n->name;
    if (!strcmp(tag, "defs") || !strcmp(tag, "symbol") || !strcmp(tag, "title") || !strcmp(tag, "desc") ||
        !strcmp(tag, "clippath") || !strcmp(tag, "mask") || !strcmp(tag, "lineargradient") ||
        !strcmp(tag, "radialgradient") || !strcmp(tag, "pattern") || !strcmp(tag, "marker") ||
        !strcmp(tag, "style") || !strcmp(tag, "script") || !strcmp(tag, "filter") || !strcmp(tag, "metadata"))
        return;
    if (!sv_style(cx, n, &st)) return;
    const char *t = web_attr(n, "transform");
    if (t) m = sv_transform(t, m);
    cx->depth++;
    if (!strcmp(tag, "g") || !strcmp(tag, "a") || !strcmp(tag, "switch")) {
        sv_children(cx, n, m, &st);
    } else if (!strcmp(tag, "svg")) {
        /* a nested viewport */
        float x = sv_attr_num(n, "x", 0), y = sv_attr_num(n, "y", 0);
        float w = sv_attr_num(n, "width", cx->vpw), h = sv_attr_num(n, "height", cx->vph);
        float ow = cx->vpw, oh = cx->vph;
        sv_vbsize(n, w, h, &cx->vpw, &cx->vph);
        if (w > 0 && h > 0) sv_children(cx, n, sv_viewbox(n, x, y, w, h, m), &st);
        else sv_children(cx, n, m_mul(m, m_tr(x, y)), &st);
        cx->vpw = ow; cx->vph = oh;
    } else if (!strcmp(tag, "use")) {
        const char *href = web_attr(n, "href");
        if (!href) href = web_attr(n, "xlink:href");
        if (href && *href == '#' && cx->uses < 400) {
            cx->uses++;
            const web_node_t *ref = sv_find_id(cx->doc, href + 1);
            float x = sv_attr_num(n, "x", 0), y = sv_attr_num(n, "y", 0);
            mat_t mu = m_mul(m, m_tr(x, y));
            if (ref && ref->name && !strcmp(ref->name, "symbol")) {
                /* a symbol: its own viewport, 100% of the current one by default */
                float w = sv_attr_num(n, "width", cx->vpw), h = sv_attr_num(n, "height", cx->vph);
                sstate_t s2 = st;
                cx->in_use++;
                if (w > 0 && h > 0 && sv_style(cx, ref, &s2)) {
                    float ow = cx->vpw, oh = cx->vph;
                    sv_vbsize(ref, w, h, &cx->vpw, &cx->vph);
                    sv_children(cx, ref, sv_viewbox(ref, 0, 0, w, h, mu), &s2);
                    cx->vpw = ow; cx->vph = oh;
                }
                cx->in_use--;
            } else if (ref && ref != n) {
                /* (a <use> pointing at one of its ancestors would recurse: depth-bounded) */
                cx->in_use++;
                sv_node(cx, ref, mu, st);
                cx->in_use--;
            }
        }
    } else {
        sv_shape(cx, n, tag, m, &st);
    }
    cx->depth--;
}

/* Draw the <svg> element n into a w x h surface (alpha). */
static surface_t *svg_render(const web_doc_t *doc, const web_node_t *n, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0 || (int64_t)w * h > 4 * 1024 * 1024) return NULL;
    surface_t *s = surface_create_alpha(w, h);
    if (!s) return NULL;
    sctx_t cx;
    memset(&cx, 0, sizeof cx);
    cx.s = s;
    cx.doc = doc;
    sstate_t st = { 0xFF000000u, 0, 1, false, 1, color };
    if (!sv_style(&cx, n, &st)) return s;
    st.cur = color;
    sv_vbsize(n, (float)w, (float)h, &cx.vpw, &cx.vph);
    mat_t m = sv_viewbox(n, 0, 0, (float)w, (float)h, (mat_t){ 1, 0, 0, 1, 0, 0 });
    const char *t = web_attr(n, "transform");
    if (t) m = sv_transform(t, m);
    sv_children(&cx, n, m, &st);
    return s;
}

/* ---- cache ------------------------------------------------------------------------------------- */
typedef struct svgc { const web_node_t *n; int w, h; uint32_t color; surface_t *s; uint32_t used; } svgc_t;
typedef struct web_svgcache { svgc_t e[256]; int n; uint32_t tick; const web_doc_t *doc; } web_svgcache_t;

void web_svg_free_cache(web_page_t *pg) {
    web_svgcache_t *c = pg->svgcache;
    if (!c) return;
    for (int i = 0; i < c->n; i++) if (c->e[i].s) surface_destroy(c->e[i].s);
    free(c);
    pg->svgcache = NULL;
}

void web_svg_paint(web_page_t *pg, surface_t *s, const web_node_t *n, int x, int y, int w, int h, uint32_t color, int opacity) {
    if (!n || w <= 0 || h <= 0) return;
    web_svgcache_t *c = pg->svgcache;
    if (c && c->doc != pg->doc) { web_svg_free_cache(pg); c = NULL; }
    if (!c) {
        c = calloc(1, sizeof *c);
        if (!c) return;
        c->doc = pg->doc;
        pg->svgcache = c;
    }
    c->tick++;
    svgc_t *e = NULL;
    for (int i = 0; i < c->n; i++)
        if (c->e[i].n == n && c->e[i].w == w && c->e[i].h == h && c->e[i].color == color) { e = &c->e[i]; break; }
    if (!e) {
        if (c->n < (int)ARRAY_SIZE(c->e)) e = &c->e[c->n++];
        else {
            e = &c->e[0];
            for (int i = 1; i < c->n; i++) if (c->e[i].used < e->used) e = &c->e[i];
            if (e->s) surface_destroy(e->s);
        }
        e->n = n; e->w = w; e->h = h; e->color = color;
        e->s = svg_render(pg->doc, n, w, h, color);
    }
    e->used = c->tick;
    if (e->s) gfx_blit_alpha(s, x, y, e->s, opacity);
}
