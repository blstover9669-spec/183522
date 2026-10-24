/*
 * icons.c - the system icon library (see include/nova/icons.h).
 *
 * Every icon is drawn on a 64-unit design grid. Icons are rendered
 * supersampled (4x below 24 px, 2x otherwise) onto a transparent surface and
 * box-filtered down, so for 16 and 32 px icons one design unit is exactly one
 * supersampled pixel and edges placed on multiples of 4 (16 px) or 2 (32 px)
 * units land on whole output pixels. Below 24 px a simplified, pixel-aligned
 * variant of each design is used (fewer details, 1 px outlines).
 */
#include "wm_internal.h"
#include <nova/icons.h>
#include <nova/shell.h>

typedef struct {
    surface_t *s;
    int x, y, size;      /* box in the target surface (supersampled pixels) */
    double u;            /* pixels per design unit                         */
    bool sm;             /* simplified small variant                       */
} ic_t;

/* ---- unit helpers -------------------------------------------------------------------- */
static int ix(const ic_t *c, double v) { return c->x + (int)floor(v * c->u + 0.5); }
static int iy(const ic_t *c, double v) { return c->y + (int)floor(v * c->u + 0.5); }
static int il(const ic_t *c, double v) { int r = (int)floor(v * c->u + 0.5); return r < 1 && v > 0 ? 1 : r; }

static void R(ic_t *c, double x, double y, double w, double h, uint32_t col) {
    int x0 = ix(c, x), y0 = iy(c, y);
    gfx_fill_rect(c->s, x0, y0, ix(c, x + w) - x0, iy(c, y + h) - y0, col);
}
static void RR(ic_t *c, double x, double y, double w, double h, double r, uint32_t col) {
    int x0 = ix(c, x), y0 = iy(c, y);
    gfx_fill_rounded_rect(c->s, x0, y0, ix(c, x + w) - x0, iy(c, y + h) - y0, il(c, r), col);
}
static void RRG(ic_t *c, double x, double y, double w, double h, double r, uint32_t top, uint32_t bot) {
    int x0 = ix(c, x), y0 = iy(c, y);
    gfx_fill_rounded_gradient_v(c->s, x0, y0, ix(c, x + w) - x0, iy(c, y + h) - y0, il(c, r), top, bot);
}
static void RG(ic_t *c, double x, double y, double w, double h, uint32_t top, uint32_t bot) {
    int x0 = ix(c, x), y0 = iy(c, y);
    gfx_gradient_v(c->s, x0, y0, ix(c, x + w) - x0, iy(c, y + h) - y0, top, bot);
}
static void POLY(ic_t *c, const double *xy, int n, uint32_t col) {
    point_t p[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) { p[i].x = ix(c, xy[2 * i]); p[i].y = iy(c, xy[2 * i + 1]); }
    gfx_fill_polygon(c->s, p, n, col);
}
static void CIRC(ic_t *c, double cx, double cy, double r, uint32_t col) {
    gfx_fill_circle_aa(c->s, c->x + cx * c->u, c->y + cy * c->u, r * c->u, col);
}
static void RING(ic_t *c, double cx, double cy, double r, double th, uint32_t col) {
    gfx_circle_aa(c->s, c->x + cx * c->u, c->y + cy * c->u, r * c->u, th * c->u, col);
}
/* Thick line as a quad, optional round caps. */
static void LINE(ic_t *c, double x0, double y0, double x1, double y1, double w, uint32_t col, bool caps) {
    double ddx = x1 - x0, ddy = y1 - y0, len = sqrt(ddx * ddx + ddy * ddy);
    if (len < 0.001) return;
    double nx = -ddy / len * w / 2, ny = ddx / len * w / 2;
    double q[8] = { x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny };
    POLY(c, q, 4, col);
    if (caps) { CIRC(c, x0, y0, w / 2, col); CIRC(c, x1, y1, w / 2, col); }
}
/* Cut a soft-edged transparent hole (only valid on a private transparent surface). */
static void PUNCH(ic_t *c, double cx, double cy, double r) {
    surface_t *s = c->s;
    double pcx = c->x + cx * c->u, pcy = c->y + cy * c->u, pr = r * c->u;
    int x0 = (int)(pcx - pr - 1), x1 = (int)(pcx + pr + 2), y0 = (int)(pcy - pr - 1), y1 = (int)(pcy + pr + 2);
    rect_t cl = s->clip;
    x0 = MAX(x0, cl.x); y0 = MAX(y0, cl.y); x1 = MIN(x1, cl.x + cl.w); y1 = MIN(y1, cl.y + cl.h);
    for (int y = y0; y < y1; y++) {
        uint32_t *row = surface_row(s, y);
        for (int x = x0; x < x1; x++) {
            double ddx = x + 0.5 - pcx, ddy = y + 0.5 - pcy;
            double cov = pr + 0.5 - sqrt(ddx * ddx + ddy * ddy);
            if (cov <= 0) continue;
            if (cov >= 1) { row[x] = 0; continue; }
            uint32_t a = (uint32_t)(COLOR_A(row[x]) * (1.0 - cov));
            row[x] = (row[x] & 0x00FFFFFFu) | (a << 24);
        }
    }
}

/* A question mark: arc (centre cx,cy radius r) + stem + dot, stroke width w. */
static void QMARK(ic_t *c, double cx, double cy, double r, double w, uint32_t col) {
    double prevx = 0, prevy = 0;
    for (int deg = -80, i = 0; deg <= 180; deg += 13, i++) {
        double a = deg * M_PI / 180, px = cx + r * sin(a), py = cy - r * cos(a);
        if (i) LINE(c, prevx, prevy, px, py, w, col, true);
        prevx = px; prevy = py;
    }
    LINE(c, cx, cy + r, cx, cy + r + r * 0.55, w, col, true);
    CIRC(c, cx, cy + r * 2.45, w * 0.68, col);
}

/* Gear outline polygon + hole. */
static void GEAR(ic_t *c, double cx, double cy, double ro, double ri, double hole, int teeth, double rot, uint32_t col) {
    double pts[128];
    int n = 0;
    double step = 2 * M_PI / teeth;
    double wo = step * 0.20, wi = step * 0.30;   /* half-widths of a tooth at the tip / root */
    for (int i = 0; i < teeth && n < 120; i++) {
        double a = rot + i * step;
        double as[4] = { a - wi, a - wo, a + wo, a + wi };
        double rs[4] = { ri, ro, ro, ri };
        for (int k = 0; k < 4; k++) { pts[n++] = cx + rs[k] * sin(as[k]); pts[n++] = cy - rs[k] * cos(as[k]); }
        double am = a + step / 2;   /* root midpoint keeps the root circle round */
        pts[n++] = cx + ri * sin(am);
        pts[n++] = cy - ri * cos(am);
    }
    POLY(c, pts, n / 2, col);
    CIRC(c, cx, cy, ri - 0.2, col);   /* fill the polygon's inner chords */
    if (hole > 0) PUNCH(c, cx, cy, hole);
}

/* ---- palette -------------------------------------------------------------------------- */
#define F_BACK   RGB(222, 150, 30)
#define F_TOP    RGB(255, 212, 98)
#define F_BOT    RGB(247, 178, 48)
#define F_EDGE   RGB(200, 128, 20)
#define F_EMBLEM RGBA(138, 78, 0, 215)
#define P_BORDER RGB(150, 158, 174)
#define P_FILL   RGB(253, 253, 254)
#define P_EAR    RGB(214, 220, 232)

/* ---- folders --------------------------------------------------------------------------- */
static void folder_emblem(ic_t *c, int kind) {
    uint32_t e = F_EMBLEM;
    if (c->sm) {
        switch (kind) {
        case ICON_FOLDER_DOCUMENTS:
            R(c, 20, 28, 24, 4, e); R(c, 20, 36, 24, 4, e); R(c, 20, 44, 16, 4, e);
            break;
        case ICON_FOLDER_PICTURES: {
            double m1[6] = { 16, 50, 28, 32, 40, 50 }, m2[6] = { 30, 50, 40, 38, 50, 50 };
            POLY(c, m1, 3, e); POLY(c, m2, 3, e);
            CIRC(c, 42, 31, 3.6, e);
            break;
        }
        case ICON_FOLDER_MUSIC:
            CIRC(c, 26, 46, 5, e); R(c, 28, 28, 4, 18, e); R(c, 28, 28, 14, 4, e);
            CIRC(c, 38, 44, 5, e); R(c, 40, 28, 4, 16, e);
            break;
        case ICON_FOLDER_DOWNLOADS: {
            R(c, 28, 28, 8, 10, e);
            double a[6] = { 20, 38, 44, 38, 32, 50 };
            POLY(c, a, 3, e);
            break;
        }
        case ICON_FOLDER_DESKTOP:
            RR(c, 20, 28, 24, 16, 2, e); R(c, 28, 44, 8, 4, e); R(c, 24, 48, 16, 4, e);
            break;
        case ICON_FOLDER_SYSTEM:
            GEAR(c, 32, 40, 11, 8, 3.5, 6, 0, e);
            break;
        case ICON_FOLDER_HOME: {
            double roof[6] = { 18, 40, 32, 27, 46, 40 };
            POLY(c, roof, 3, e);
            R(c, 22, 40, 20, 12, e);
            break;
        }
        default: break;
        }
        return;
    }
    switch (kind) {
    case ICON_FOLDER_DOCUMENTS:
        for (int i = 0; i < 4; i++) RR(c, 23, 32 + i * 4.5, i == 3 ? 11 : 18, 2.4, 1, e);
        break;
    case ICON_FOLDER_PICTURES: {
        RR(c, 21, 31, 22, 17, 2, e);
        RR(c, 23, 33, 18, 13, 1, RGBA(255, 230, 150, 255));
        double m[6] = { 23, 46, 30, 37, 37, 46 }, m2[6] = { 31, 46, 36, 40, 41, 46 };
        POLY(c, m, 3, e); POLY(c, m2, 3, e);
        CIRC(c, 37, 37, 2.2, e);
        break;
    }
    case ICON_FOLDER_MUSIC:
        CIRC(c, 28, 45, 3.4, e); R(c, 30.4, 31, 2.5, 14, e); R(c, 30.4, 31, 9, 3, e);
        CIRC(c, 37, 43, 3.4, e); R(c, 39.4, 31, 2.5, 12, e);
        break;
    case ICON_FOLDER_DOWNLOADS: {
        R(c, 30, 30, 4, 10, e);
        double a[6] = { 24, 39, 40, 39, 32, 47 };
        POLY(c, a, 3, e);
        R(c, 23, 48, 18, 2, e);
        break;
    }
    case ICON_FOLDER_DESKTOP:
        RR(c, 22, 31, 20, 13, 2, e); R(c, 30, 44, 4, 3, e); R(c, 26, 47, 12, 2, e);
        break;
    case ICON_FOLDER_SYSTEM:
        GEAR(c, 32, 40, 9, 6.5, 2.8, 8, 0, e);
        break;
    case ICON_FOLDER_HOME: {
        double roof[6] = { 21, 40, 32, 30, 43, 40 };
        POLY(c, roof, 3, e);
        R(c, 24.5, 40, 15, 9, e);
        break;
    }
    default: break;
    }
}

static void draw_folder(ic_t *c, int kind) {
    if (c->sm) {
        RR(c, 4, 8, 24, 12, 4, F_BACK);
        RR(c, 4, 12, 56, 44, 4, F_BACK);
        R(c, 8, 16, 48, 12, RGB(250, 250, 250));
        RR(c, 4, 24, 56, 32, 4, F_EDGE);
        RRG(c, 4, 24, 56, 28, 4, F_TOP, F_BOT);
        R(c, 8, 24, 48, 4, RGBA(255, 255, 255, 80));
    } else {
        RR(c, 4, 8, 24, 12, 4, F_BACK);
        RR(c, 4, 12, 56, 44, 5, F_BACK);
        R(c, 9, 16, 46, 22, RGB(205, 210, 220));
        R(c, 10, 17, 44, 21, RGB(252, 252, 252));
        RR(c, 4, 24, 56, 34, 5, F_EDGE);
        RRG(c, 4, 23, 56, 33.5, 5, F_TOP, F_BOT);
        R(c, 7, 24, 50, 1.3, RGBA(255, 255, 255, 130));
    }
    folder_emblem(c, kind);
}

/* ---- pages ----------------------------------------------------------------------------- */
static void draw_page_base(ic_t *c) {
    if (c->sm) {
        double outer[10] = { 12, 4, 40, 4, 52, 16, 52, 60, 12, 60 };
        POLY(c, outer, 5, P_BORDER);
        double inner[10] = { 16, 8, 38.4, 8, 48, 17.6, 48, 56, 16, 56 };
        POLY(c, inner, 5, P_FILL);
        double ear[6] = { 38, 6, 38, 18, 50, 18 };
        POLY(c, ear, 3, P_EAR);
        R(c, 36, 4, 4, 16, P_BORDER);
        R(c, 36, 16, 16, 4, P_BORDER);
        return;
    }
    double shadow[10] = { 15, 6, 42, 6, 54, 18, 54, 62, 15, 62 };
    POLY(c, shadow, 5, RGBA(0, 0, 0, 40));
    double outer[10] = { 13, 4, 41, 4, 53, 16, 53, 60, 13, 60 };
    POLY(c, outer, 5, P_BORDER);
    double inner[10] = { 14.5, 5.5, 40.4, 5.5, 51.5, 16.6, 51.5, 58.5, 14.5, 58.5 };
    POLY(c, inner, 5, P_FILL);
    double ear[6] = { 40, 5, 40, 17, 52, 17 };
    POLY(c, ear, 3, P_EAR);
    LINE(c, 40, 4.5, 40, 17, 1.4, P_BORDER, false);
    LINE(c, 40, 17, 52.5, 17, 1.4, P_BORDER, false);
}

static void draw_page(ic_t *c, int kind) {
    draw_page_base(c);
    bool sm = c->sm;
    switch (kind) {
    case ICON_FILE_TEXT: {
        uint32_t col = RGB(126, 136, 156);
        if (sm) { R(c, 20, 24, 24, 4, col); R(c, 20, 32, 20, 4, col); R(c, 20, 40, 24, 4, col); R(c, 20, 48, 16, 4, col); }
        else for (int i = 0; i < 6; i++) RR(c, 20, 24 + i * 5.5, i == 5 ? 14 : (i % 2 ? 22 : 26), 2.2, 1, col);
        break;
    }
    case ICON_FILE_LOG: {
        static const uint32_t dots[] = { 0xFF3DB27Au, 0xFF3DB27Au, 0xFFE8A31Cu, 0xFF3DB27Au, 0xFFDD4B4Bu, 0xFF3DB27Au };
        uint32_t col = RGB(126, 136, 156);
        if (sm) {
            for (int i = 0; i < 4; i++) { R(c, 20, 24 + i * 8, 4, 4, dots[i + 1]); R(c, 28, 24 + i * 8, i % 2 ? 12 : 16, 4, col); }
        } else {
            for (int i = 0; i < 6; i++) {
                CIRC(c, 21.5, 25.1 + i * 5.5, 1.7, dots[i]);
                RR(c, 26, 24 + i * 5.5, i % 2 ? 16 : 20, 2.2, 1, col);
            }
        }
        break;
    }
    case ICON_FILE_CODE: {
        static const uint32_t cols[] = { 0xFF9B59D0u, 0xFF3D8BF0u, 0xFF3DB27Au, 0xFFE58A2Eu, 0xFF3D8BF0u, 0xFF9B59D0u };
        if (sm) {
            R(c, 20, 24, 12, 4, cols[0]); R(c, 24, 32, 16, 4, cols[1]); R(c, 24, 40, 12, 4, cols[2]); R(c, 20, 48, 16, 4, cols[3]);
        } else {
            static const int ind[] = { 0, 4, 8, 8, 4, 0 }, len[] = { 16, 18, 14, 10, 12, 8 };
            for (int i = 0; i < 6; i++) RR(c, 19 + ind[i], 23 + i * 5.5, len[i], 2.4, 1, cols[i]);
        }
        break;
    }
    case ICON_FILE_IMAGE:
        if (sm) {
            RG(c, 20, 24, 24, 24, RGB(100, 180, 250), RGB(180, 225, 255));
            double h1[6] = { 20, 48, 30, 34, 40, 48 }, h2[6] = { 30, 48, 38, 38, 44, 44 };
            POLY(c, h1, 3, RGB(64, 160, 84));
            POLY(c, h2, 3, RGB(46, 134, 70));
            R(c, 30, 44, 14, 4, RGB(46, 134, 70));
            CIRC(c, 38, 30, 3.5, RGB(255, 196, 40));
        } else {
            RRG(c, 19, 24, 28, 24, 2, RGB(110, 190, 255), RGB(190, 230, 255));
            double hill[6] = { 19, 48, 30, 33, 41, 48 }, hill2[6] = { 31, 48, 39, 38, 47, 48 };
            POLY(c, hill, 3, RGB(70, 170, 90));
            POLY(c, hill2, 3, RGB(50, 140, 75));
            CIRC(c, 40, 30, 3.2, RGB(255, 200, 40));
        }
        break;
    case ICON_FILE_AUDIO:
        if (sm) {
            CIRC(c, 25, 47, 5, RGB(240, 110, 60)); R(c, 26, 24, 4, 23, RGB(240, 110, 60));
            R(c, 26, 24, 16, 4, RGB(225, 80, 120));
            CIRC(c, 37, 43, 5, RGB(225, 80, 120)); R(c, 38, 24, 4, 19, RGB(225, 80, 120));
        } else {
            CIRC(c, 27, 45, 5, RGB(240, 110, 60));
            R(c, 30, 24, 3.5, 21, RGB(240, 110, 60));
            R(c, 30, 24, 13, 4, RGB(240, 110, 60));
            CIRC(c, 40, 42, 5, RGB(225, 80, 120));
            R(c, 43, 24, 3.5, 18, RGB(225, 80, 120));
        }
        break;
    case ICON_FILE_ARCHIVE:
        if (sm) {
            for (int i = 0; i < 5; i++) R(c, i % 2 ? 32 : 28, 8 + i * 4, 4, 4, RGB(120, 110, 100));
            R(c, 24, 32, 16, 16, RGB(150, 130, 100));
            R(c, 28, 40, 8, 4, RGB(250, 245, 235));
        } else {
            for (int i = 0; i < 7; i++) R(c, i % 2 ? 31 : 28, 7 + i * 4, 5, 3, RGB(120, 110, 100));
            RR(c, 26, 36, 12, 14, 2, RGB(150, 130, 100));
            RR(c, 29, 41, 6, 5, 1, RGB(250, 245, 235));
        }
        break;
    case ICON_FILE_BINARY:
        if (sm) {
            for (int r = 0; r < 3; r++)
                for (int k = 0; k < 3; k++)
                    if ((r * 7 + k * 3) % 5 < 3) R(c, 20 + k * 8, 24 + r * 8, 4, 4, (r + k) % 2 ? RGB(90, 120, 170) : RGB(52, 72, 116));
        } else {
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++)
                    if ((r * 7 + k * 3) % 5 < 3)
                        RR(c, 20 + k * 7, 24 + r * 7, 5, 5, 1, (r + k) % 2 ? RGB(90, 120, 170) : RGB(60, 80, 120));
        }
        break;
    case ICON_FILE_CONFIG: {
        uint32_t tr = RGB(150, 160, 180), kn = RGB(60, 130, 230);
        if (sm) {
            R(c, 20, 28, 24, 4, tr); R(c, 24, 24, 8, 12, kn);
            R(c, 20, 44, 24, 4, tr); R(c, 32, 40, 8, 12, kn);
        } else {
            for (int i = 0; i < 3; i++) {
                RR(c, 20, 26 + i * 10, 26, 2.4, 1, tr);
                double kx = i == 0 ? 26 : i == 1 ? 39 : 31;
                CIRC(c, kx, 27.2 + i * 10, 3.6, kn);
                CIRC(c, kx, 27.2 + i * 10, 1.6, COLOR_WHITE);
            }
        }
        break;
    }
    case ICON_FILE_UNKNOWN:
        if (sm) QMARK(c, 32, 29, 7, 5.5, RGB(110, 120, 140));
        else QMARK(c, 33, 29, 6.5, 4.2, RGB(120, 130, 150));
        break;
    default: break;
    }
}

static void draw_note(ic_t *c) {
    uint32_t y1 = RGB(255, 228, 96), y2 = RGB(252, 214, 66), ear = RGB(226, 178, 40), line = RGBA(170, 125, 20, 200);
    if (c->sm) {
        double outer[10] = { 8, 8, 56, 8, 56, 40, 40, 56, 8, 56 };
        POLY(c, outer, 5, RGB(206, 160, 30));
        double inner[10] = { 12, 12, 52, 12, 52, 38, 38, 52, 12, 52 };
        POLY(c, inner, 5, y1);
        R(c, 12, 12, 40, 4, y2);
        double e[6] = { 56, 40, 40, 40, 40, 56 };
        POLY(c, e, 3, ear);
        R(c, 16, 24, 32, 4, line); R(c, 16, 32, 20, 4, line);
        return;
    }
    double sh[10] = { 10, 11, 58, 11, 58, 44, 44, 58, 10, 58 };
    POLY(c, sh, 5, RGBA(0, 0, 0, 40));
    double body[10] = { 8, 8, 56, 8, 56, 42, 42, 56, 8, 56 };
    POLY(c, body, 5, y1);
    R(c, 8, 8, 48, 9, y2);
    double e[6] = { 56, 42, 42, 42, 42, 56 };
    POLY(c, e, 3, ear);
    for (int i = 0; i < 3; i++) RR(c, 15, 25 + i * 7, i == 2 ? 18 : 32, 2.6, 1, line);
}

static void draw_paint(ic_t *c) {
    uint32_t wood = RGB(238, 204, 152), edge = RGB(196, 154, 98);
    CIRC(c, 28, 34, 25, edge);
    CIRC(c, 28, 34, c->sm ? 21.5 : 23, wood);
    PUNCH(c, 17, 45, c->sm ? 5 : 5.5);
    if (c->sm) {
        CIRC(c, 16, 30, 5.5, RGB(226, 60, 60));
        CIRC(c, 27, 17, 5.5, RGB(250, 190, 30));
        CIRC(c, 41, 24, 5.5, RGB(50, 120, 230));
    } else {
        CIRC(c, 16, 28, 5, RGB(226, 60, 60));
        CIRC(c, 26, 19, 5, RGB(250, 196, 40));
        CIRC(c, 38, 20, 5, RGB(60, 170, 90));
        CIRC(c, 42, 32, 5, RGB(50, 120, 230));
    }
    /* brush */
    LINE(c, 44, 46, 61, 25, c->sm ? 6 : 5, RGB(150, 94, 52), true);
    LINE(c, 38, 53, 45, 45, c->sm ? 7 : 6.5, RGB(190, 196, 208), false);
    double tip[8] = { 33, 50, 41, 56, 34, 61, 29, 60 };
    POLY(c, tip, 4, RGB(226, 60, 60));
}

/* ---- objects ---------------------------------------------------------------------------- */
static void draw_app(ic_t *c) {
    if (c->sm) {
        RR(c, 4, 8, 56, 48, 4, RGB(42, 96, 196));
        R(c, 8, 20, 48, 32, RGB(250, 251, 253));
        R(c, 44, 12, 4, 4, RGBA(255, 255, 255, 200));
        R(c, 12, 24, 16, 8, RGB(120, 170, 240)); R(c, 32, 24, 20, 8, RGB(190, 205, 225));
        R(c, 12, 36, 16, 12, RGB(190, 205, 225)); R(c, 32, 36, 20, 12, RGB(120, 170, 240));
        return;
    }
    RR(c, 6, 10, 52, 44, 6, RGB(36, 84, 180));
    RRG(c, 6, 10, 52, 14, 6, RGB(84, 150, 250), RGB(48, 110, 220));
    R(c, 8, 20, 48, 32, RGB(250, 251, 253));
    RR(c, 8, 20, 48, 32, 4, RGB(250, 251, 253));
    R(c, 8, 20, 48, 4, RGB(250, 251, 253));
    for (int i = 0; i < 3; i++) CIRC(c, 47 - i * 6, 15, 1.8, RGBA(255, 255, 255, 220));
    RR(c, 13, 25, 17, 11, 2, RGB(110, 165, 245));
    RR(c, 34, 25, 17, 11, 2, RGB(250, 170, 70));
    RR(c, 13, 39, 17, 9, 2, RGB(90, 195, 130));
    RR(c, 34, 39, 17, 9, 2, RGB(170, 120, 230));
}

static void draw_disk(ic_t *c) {
    if (c->sm) {
        RR(c, 4, 16, 56, 32, 4, RGB(100, 108, 124));
        RRG(c, 8, 20, 48, 16, 2, RGB(232, 236, 242), RGB(196, 202, 214));
        R(c, 8, 36, 48, 8, RGB(150, 158, 174));
        R(c, 44, 36, 8, 4, RGB(60, 210, 110));
        return;
    }
    RR(c, 4, 18, 56, 32, 6, RGB(92, 100, 116));
    RRG(c, 6, 20, 52, 18, 5, RGB(236, 239, 245), RGB(198, 204, 216));
    RR(c, 6, 36, 52, 12, 5, RGB(150, 158, 174));
    R(c, 6, 36, 52, 4, RGB(150, 158, 174));
    for (int i = 0; i < 4; i++) R(c, 12 + i * 5, 40, 2.4, 5, RGB(112, 120, 136));
    CIRC(c, 49, 42, 2.6, RGB(60, 210, 110));
    CIRC(c, 49, 42, 4.5, RGBA(60, 210, 110, 60));
}

static void draw_computer(ic_t *c) {
    if (c->sm) {
        RR(c, 4, 8, 56, 36, 4, RGB(54, 60, 74));
        RG(c, 8, 12, 48, 28, RGB(96, 176, 255), RGB(40, 100, 220));
        R(c, 28, 44, 8, 8, RGB(130, 136, 150));
        R(c, 16, 52, 32, 4, RGB(90, 96, 110));
        return;
    }
    RR(c, 4, 8, 56, 38, 5, RGB(54, 60, 74));
    RRG(c, 8, 12, 48, 30, 2, RGB(90, 170, 255), RGB(40, 100, 220));
    double gl[8] = { 8, 12, 30, 12, 18, 42, 8, 42 };
    POLY(c, gl, 4, RGBA(255, 255, 255, 30));
    R(c, 26, 46, 12, 8, RGB(130, 136, 150));
    RR(c, 16, 53, 32, 5, 2, RGB(90, 96, 110));
}

static void draw_trash(ic_t *c) {
    uint32_t lid = RGB(96, 108, 130), body1 = RGB(178, 188, 206), body2 = RGB(136, 148, 170), rib = RGB(110, 122, 146);
    if (c->sm) {
        R(c, 24, 4, 16, 4, lid);
        R(c, 8, 8, 48, 8, lid);
        double out[8] = { 12, 20, 52, 20, 48, 60, 16, 60 };
        POLY(c, out, 4, lid);
        double in[8] = { 16, 20, 48, 20, 44.6, 56, 19.4, 56 };
        POLY(c, in, 4, body1);
        R(c, 24, 24, 4, 28, rib); R(c, 36, 24, 4, 28, rib);
        return;
    }
    LINE(c, 25.5, 12, 25.5, 6, 3, lid, false);
    LINE(c, 24, 6.5, 40, 6.5, 3, lid, false);
    LINE(c, 38.5, 6, 38.5, 12, 3, lid, false);
    RR(c, 8, 11, 48, 8, 3, lid);
    double out[8] = { 12, 20, 52, 20, 47.5, 59, 16.5, 59 };
    POLY(c, out, 4, body2);
    double in[8] = { 14, 20, 50, 20, 46, 57, 18, 57 };
    POLY(c, in, 4, body1);
    for (int i = 0; i < 3; i++) RR(c, 22.5 + i * 8, 25, 3, 27, 1.5, rib);
}

static void draw_gear_icon(ic_t *c) {
    if (c->sm) {
        GEAR(c, 32, 32, 29, 21, 8, 8, M_PI / 8, RGB(118, 130, 152));
        return;
    }
    GEAR(c, 32, 32, 28, 21, 0, 9, 0, RGB(108, 120, 144));
    CIRC(c, 32, 32, 15, RGB(150, 162, 184));
    PUNCH(c, 32, 32, 8);
}

static void draw_service(ic_t *c) {
    if (c->sm) {
        GEAR(c, 24, 24, 22, 15, 6, 6, 0, RGB(108, 124, 154));
        GEAR(c, 46, 46, 16, 10, 4, 6, M_PI / 6, RGB(240, 140, 30));
        return;
    }
    GEAR(c, 25, 25, 22, 16, 0, 8, 0, RGB(100, 116, 148));
    CIRC(c, 25, 25, 11, RGB(138, 154, 182));
    PUNCH(c, 25, 25, 5.5);
    GEAR(c, 46, 46, 15, 10.5, 0, 7, M_PI / 7, RGB(232, 128, 24));
    CIRC(c, 46, 46, 7, RGB(250, 176, 80));
    PUNCH(c, 46, 46, 3.5);
}

static void draw_settings(ic_t *c) {
    RRG(c, 4, 4, 56, 56, c->sm ? 8 : 12, RGB(98, 116, 148), RGB(60, 74, 100));
    uint32_t tr = RGBA(255, 255, 255, 110), kn = COLOR_WHITE;
    if (c->sm) {
        R(c, 12, 20, 40, 4, tr); R(c, 12, 40, 40, 4, tr);
        RR(c, 32, 14, 12, 16, 3, kn); RR(c, 16, 34, 12, 16, 3, kn);
        return;
    }
    double ys[3] = { 19, 32, 45 }, xs[3] = { 38, 22, 44 };
    for (int i = 0; i < 3; i++) {
        RR(c, 13, ys[i] - 1.5, 38, 3, 1.5, tr);
        RR(c, 13, ys[i] - 1.5, xs[i] - 13, 3, 1.5, RGB(120, 200, 255));
        CIRC(c, xs[i], ys[i], 5, kn);
    }
}

static void draw_bell(ic_t *c) {
    uint32_t g1 = RGB(252, 204, 64), g2 = RGB(228, 160, 24), rim = RGB(214, 146, 18), cl = RGB(184, 120, 12);
    if (c->sm) {
        R(c, 28, 4, 8, 6, rim);
        CIRC(c, 32, 26, 15, g1);
        double body[16] = { 17, 26, 47, 26, 48, 38, 56, 46, 56, 48, 8, 48, 8, 46, 16, 38 };
        POLY(c, body, 8, g1);
        R(c, 8, 44, 48, 6, rim);
        RR(c, 26, 50, 12, 8, 4, cl);
        return;
    }
    CIRC(c, 32, 9, 4, rim);
    CIRC(c, 32, 28, 15.5, g1);
    double body[16] = { 16.5, 28, 47.5, 28, 49, 40, 55, 46, 55, 48, 9, 48, 9, 46, 15, 40 };
    POLY(c, body, 8, g1);
    double shade[10] = { 32, 13, 47.5, 28, 49, 40, 55, 46, 32, 46 };
    POLY(c, shade, 5, color_alpha(g2, 70));
    RR(c, 7, 44, 50, 7, 3.5, rim);
    CIRC(c, 32, 54.5, 5, cl);
    LINE(c, 22, 22, 20, 36, 2.5, RGBA(255, 255, 255, 140), true);
}

static void draw_alarm(ic_t *c) {
    uint32_t red = RGB(222, 64, 60), dark = RGB(170, 40, 40), face = RGB(252, 252, 252), hand = RGB(40, 44, 56);
    if (c->sm) {
        CIRC(c, 14, 14, 9, dark); CIRC(c, 50, 14, 9, dark);
        LINE(c, 18, 50, 12, 60, 6, dark, false); LINE(c, 46, 50, 52, 60, 6, dark, false);
        CIRC(c, 32, 34, 24, red);
        CIRC(c, 32, 34, 17, face);
        R(c, 30, 22, 4, 14, hand); R(c, 30, 32, 12, 4, hand);
        return;
    }
    CIRC(c, 14, 14, 9, dark); CIRC(c, 50, 14, 9, dark);
    LINE(c, 32, 10, 32, 14, 4, dark, false);
    LINE(c, 19, 50, 13, 59, 4.5, dark, true); LINE(c, 45, 50, 51, 59, 4.5, dark, true);
    CIRC(c, 32, 35, 23, red);
    CIRC(c, 32, 35, 18, face);
    for (int i = 0; i < 12; i++) {
        double a = i * M_PI / 6, r0 = i % 3 ? 15.5 : 13.5;
        LINE(c, 32 + r0 * sin(a), 35 - r0 * cos(a), 32 + 16.5 * sin(a), 35 - 16.5 * cos(a), i % 3 ? 1.2 : 2, RGB(150, 156, 170), false);
    }
    LINE(c, 32, 35, 32, 23, 3, hand, true);
    LINE(c, 32, 35, 41, 39, 3, hand, true);
    CIRC(c, 32, 35, 2.5, red);
}

static void draw_clock(ic_t *c) {
    uint32_t ring = RGB(56, 110, 210), face = RGB(252, 252, 252), hand = RGB(40, 44, 56);
    if (c->sm) {
        CIRC(c, 32, 32, 28, ring);
        CIRC(c, 32, 32, 22, face);
        R(c, 30, 16, 4, 18, hand); R(c, 30, 30, 14, 4, hand);
        return;
    }
    CIRC(c, 32, 32, 28, ring);
    CIRC(c, 32, 32, 23, face);
    for (int i = 0; i < 12; i++) {
        double a = i * M_PI / 6, r0 = i % 3 ? 19.5 : 17;
        LINE(c, 32 + r0 * sin(a), 32 - r0 * cos(a), 32 + 21 * sin(a), 32 - 21 * cos(a), i % 3 ? 1.3 : 2.4, RGB(150, 156, 170), false);
    }
    LINE(c, 32, 32, 32, 16, 3.2, hand, true);
    LINE(c, 32, 32, 43, 38, 3.2, hand, true);
    CIRC(c, 32, 32, 3, ring);
}

static void draw_search(ic_t *c) {
    uint32_t dark = RGB(92, 104, 130);
    CIRC(c, 26, 26, 15, RGBA(170, 210, 255, 170));
    RING(c, 26, 26, 16, c->sm ? 7 : 6, dark);
    LINE(c, 38, 38, 55, 55, c->sm ? 9 : 8, dark, true);
    if (!c->sm) LINE(c, 19, 20, 22, 16, 2.5, RGBA(255, 255, 255, 200), true);
}

static void draw_run(ic_t *c) {
    RRG(c, 4, 4, 56, 56, c->sm ? 8 : 12, RGB(70, 150, 255), RGB(40, 100, 220));
    uint32_t w = COLOR_WHITE;
    if (c->sm) {
        R(c, 12, 28, 4, 24, w); R(c, 12, 48, 24, 4, w); R(c, 12, 28, 12, 4, w); R(c, 32, 40, 4, 12, w);
        LINE(c, 24, 40, 46, 18, 6, w, false);
        double head[6] = { 32, 14, 50, 14, 50, 32 };
        POLY(c, head, 3, w);
        return;
    }
    double box[20] = { 13, 26, 28, 26, 28, 30.5, 17.5, 30.5, 17.5, 46.5, 33.5, 46.5, 33.5, 36, 38, 36, 38, 51, 13, 51 };
    POLY(c, box, 10, w);
    LINE(c, 25, 39, 45, 19, 5, w, true);
    double head[6] = { 34, 14, 50, 14, 50, 30 };
    POLY(c, head, 3, w);
}

static void draw_pin(ic_t *c) {
    uint32_t r1 = RGB(232, 72, 72), r2 = RGB(186, 44, 44), metal = RGB(140, 148, 162);
    if (c->sm) {
        R(c, 30, 36, 4, 24, metal);
        R(c, 20, 4, 24, 8, r2);
        R(c, 24, 12, 16, 16, r1);
        R(c, 12, 28, 40, 8, r2);
        return;
    }
    LINE(c, 32, 36, 32, 60, 3, metal, true);
    RR(c, 20, 4, 24, 9, 4, r2);
    double body[8] = { 24, 12, 40, 12, 42, 28, 22, 28 };
    POLY(c, body, 4, r1);
    RR(c, 12, 27, 40, 9, 4.5, r2);
    LINE(c, 27, 15, 26.5, 25, 2.2, RGBA(255, 255, 255, 120), true);
}

static void draw_terminal(ic_t *c) {
    if (c->sm) {
        RR(c, 4, 8, 56, 48, 4, RGB(104, 112, 128));
        R(c, 8, 12, 48, 40, RGB(36, 40, 50));
        R(c, 8, 12, 48, 4, RGB(70, 76, 90));
        LINE(c, 14, 24, 24, 33, 5.5, RGB(90, 220, 120), false);
        LINE(c, 24, 31, 14, 40, 5.5, RGB(90, 220, 120), false);
        R(c, 28, 40, 16, 4, RGB(220, 226, 236));
        return;
    }
    RR(c, 3, 7, 58, 50, 7, RGB(104, 112, 128));
    RR(c, 4, 8, 56, 48, 6, RGB(32, 36, 46));
    RR(c, 4, 8, 56, 10, 6, RGB(66, 72, 88));
    R(c, 4, 14, 56, 4, RGB(66, 72, 88));
    for (int i = 0; i < 3; i++) CIRC(c, 10 + i * 5, 13, 1.7, i == 0 ? RGB(240, 90, 80) : i == 1 ? RGB(240, 190, 60) : RGB(90, 200, 100));
    LINE(c, 13, 27, 22, 34, 3.5, RGB(90, 220, 120), true);
    LINE(c, 22, 34, 13, 41, 3.5, RGB(90, 220, 120), true);
    RR(c, 26, 39.5, 14, 3.5, 1.5, RGB(220, 226, 236));
}

/* ---- status ------------------------------------------------------------------------------ */
static void draw_status(ic_t *c, int kind) {
    uint32_t w = COLOR_WHITE;
    switch (kind) {
    case ICON_INFO:
        CIRC(c, 32, 32, 28, RGB(40, 120, 220));
        if (c->sm) { RR(c, 28, 12, 8, 8, 4, w); R(c, 28, 24, 8, 26, w); }
        else { CIRC(c, 32, 19.5, 4.5, w); RR(c, 28, 27, 8, 21, 2, w); }
        break;
    case ICON_QUESTION:
        CIRC(c, 32, 32, 28, RGB(70, 132, 232));
        QMARK(c, 32, 23, 8.5, c->sm ? 7 : 6.5, w);
        break;
    case ICON_ERROR:
        CIRC(c, 32, 32, 28, RGB(220, 52, 52));
        LINE(c, 21, 21, 43, 43, c->sm ? 8 : 7, w, !c->sm);
        LINE(c, 43, 21, 21, 43, c->sm ? 8 : 7, w, !c->sm);
        break;
    case ICON_SUCCESS:
        CIRC(c, 32, 32, 28, RGB(40, 168, 90));
        LINE(c, 18, 33, 28, 43, c->sm ? 8 : 7, w, true);
        LINE(c, 28, 43, 46, 22, c->sm ? 8 : 7, w, true);
        break;
    case ICON_WARNING: {
        uint32_t amber = RGB(246, 182, 30), edge = RGB(214, 140, 10), ink = RGB(56, 40, 0);
        if (c->sm) {
            double t[6] = { 32, 2, 63, 60, 1, 60 };
            POLY(c, t, 3, edge);
            double t2[6] = { 32, 9, 57, 56, 7, 56 };
            POLY(c, t2, 3, amber);
            R(c, 28, 24, 8, 16, ink); R(c, 28, 44, 8, 8, ink);
        } else {
            double t[6] = { 32, 5, 60, 55, 4, 55 };
            POLY(c, t, 3, edge);
            CIRC(c, 32, 8, 3.5, edge); CIRC(c, 57, 53, 3.5, edge); CIRC(c, 7, 53, 3.5, edge);
            double t2[6] = { 32, 9.5, 56.5, 53, 7.5, 53 };
            POLY(c, t2, 3, amber);
            RR(c, 29, 22, 6, 19, 3, ink);
            CIRC(c, 32, 47, 3.5, ink);
        }
        break;
    }
    default: break;
    }
}

/* ---- security ---------------------------------------------------------------------------- */
/* SafetyShield: a two-tone blue shield with a white check mark. */
static void draw_shield(ic_t *c) {
    uint32_t rim = RGB(20, 92, 180), lite = RGB(70, 170, 252), deep = RGB(36, 128, 232), w = COLOR_WHITE;
    if (c->sm) {
        double out[16] = { 32, 2, 60, 10, 60, 32, 52, 48, 32, 62, 12, 48, 4, 32, 4, 10 };
        POLY(c, out, 8, rim);
        double in[16] = { 32, 7, 55, 13.5, 55, 31, 48.5, 45, 32, 57, 15.5, 45, 9, 31, 9, 13.5 };
        POLY(c, in, 8, deep);
        double half[12] = { 32, 7, 32, 57, 15.5, 45, 9, 31, 9, 13.5, 32, 7 };
        POLY(c, half, 6, lite);
        LINE(c, 18, 32, 28, 42, 7, w, false);
        LINE(c, 26, 44, 46, 22, 7, w, false);
        return;
    }
    double out[26] = { 32, 3, 44, 8, 56, 10, 56, 30, 54.5, 39, 50, 47, 43, 54, 32, 61,
                       21, 54, 14, 47, 9.5, 39, 8, 30, 8, 10 };
    POLY(c, out, 13, rim);
    CIRC(c, 32, 4.2, 1.6, rim);
    double in[26] = { 32, 7.5, 43, 12, 52, 13.5, 52, 30, 50.6, 38, 46.6, 45, 40.5, 51, 32, 56.5,
                      23.5, 51, 17.4, 45, 13.4, 38, 12, 30, 12, 13.5 };
    POLY(c, in, 13, deep);
    double half[18] = { 32, 7.5, 32, 56.5, 23.5, 51, 17.4, 45, 13.4, 38, 12, 30, 12, 13.5, 21, 12, 32, 7.5 };
    POLY(c, half, 9, lite);
    double gloss[10] = { 32, 9.5, 32, 30, 14, 30, 14, 15, 22, 13.5 };
    POLY(c, gloss, 5, RGBA(255, 255, 255, 40));
    LINE(c, 20.5, 32, 28.5, 40.5, 5.2, w, true);
    LINE(c, 28.5, 40.5, 44, 22.5, 5.2, w, true);
}

/* ---- dispatch ----------------------------------------------------------------------------- */
static void render(int kind, surface_t *s, int x, int y, int box, int final_size) {
    ic_t c = { s, x, y, box, box / 64.0, final_size < 24 };
    switch (kind) {
    case ICON_FOLDER: case ICON_FOLDER_DOCUMENTS: case ICON_FOLDER_PICTURES: case ICON_FOLDER_MUSIC:
    case ICON_FOLDER_DOWNLOADS: case ICON_FOLDER_DESKTOP: case ICON_FOLDER_SYSTEM: case ICON_FOLDER_HOME:
        draw_folder(&c, kind);
        break;
    case ICON_FILE_TEXT: case ICON_FILE_CODE: case ICON_FILE_IMAGE: case ICON_FILE_AUDIO: case ICON_FILE_ARCHIVE:
    case ICON_FILE_BINARY: case ICON_FILE_UNKNOWN: case ICON_FILE_LOG: case ICON_FILE_CONFIG:
        draw_page(&c, kind);
        break;
    case ICON_NOTE: draw_note(&c); break;
    case ICON_PAINT: draw_paint(&c); break;
    case ICON_APP: draw_app(&c); break;
    case ICON_DISK: draw_disk(&c); break;
    case ICON_COMPUTER: draw_computer(&c); break;
    case ICON_TRASH: draw_trash(&c); break;
    case ICON_GEAR: draw_gear_icon(&c); break;
    case ICON_SERVICE: draw_service(&c); break;
    case ICON_SETTINGS: draw_settings(&c); break;
    case ICON_BELL: draw_bell(&c); break;
    case ICON_ALARM: draw_alarm(&c); break;
    case ICON_CLOCK: draw_clock(&c); break;
    case ICON_SEARCH: draw_search(&c); break;
    case ICON_RUN: draw_run(&c); break;
    case ICON_PIN: draw_pin(&c); break;
    case ICON_TERMINAL: draw_terminal(&c); break;
    case ICON_INFO: case ICON_WARNING: case ICON_ERROR: case ICON_QUESTION: case ICON_SUCCESS:
        draw_status(&c, kind);
        break;
    case ICON_SHIELD: draw_shield(&c); break;
    default: break;
    }
}

void icon_render(int kind, surface_t *s, int x, int y, int size) {
    if (!s || size <= 0) return;
    render(kind, s, x, y, size, size);
}

/* ---- cache --------------------------------------------------------------------------------- */
typedef struct icache { int kind, size; surface_t *img; struct icache *next; } icache_t;
static icache_t *cache;

const surface_t *icon_get(int kind, int size) {
    if (kind <= ICON_NONE || kind >= ICON_COUNT || size < 4 || size > 256) return NULL;
    for (icache_t *e = cache; e; e = e->next)
        if (e->kind == kind && e->size == size) return e->img;
    int ss = size < 24 ? 4 : size <= 128 ? 2 : 1;
    int big = size * ss;
    surface_t *hi = surface_create_alpha(big, big);
    if (!hi) return NULL;
    render(kind, hi, 0, 0, big, size);
    surface_reset_clip(hi);
    surface_t *img = ss > 1 ? surface_scaled(hi, size, size) : hi;
    if (img != hi) surface_destroy(hi);
    if (!img) return NULL;
    icache_t *e = kcalloc(1, sizeof *e);
    if (!e) { surface_destroy(img); return NULL; }
    e->kind = kind;
    e->size = size;
    e->img = img;
    e->next = cache;
    cache = e;
    return img;
}

void icon_draw(int kind, surface_t *dst, int x, int y, int size) {
    const surface_t *img = icon_get(kind, size);
    if (img) gfx_blit_alpha(dst, x, y, img, 255);
}

void icon_draw_alpha(int kind, surface_t *dst, int x, int y, int size, int opacity) {
    const surface_t *img = icon_get(kind, size);
    if (img) gfx_blit_alpha(dst, x, y, img, opacity);
}

/* ---- mapping ------------------------------------------------------------------------------- */
static const char *const names[ICON_COUNT] = {
    "none", "folder", "folder-documents", "folder-pictures", "folder-music", "folder-downloads",
    "folder-desktop", "folder-system", "folder-home", "text", "code", "image", "audio", "archive",
    "binary", "unknown", "log", "config", "note", "paint", "app", "disk", "computer", "trash", "gear",
    "service", "settings", "bell", "alarm", "clock", "search", "run", "pin", "terminal", "info",
    "warning", "error", "question", "success", "shield",
};

const char *icon_name(int kind) { return (kind >= 0 && kind < ICON_COUNT) ? names[kind] : "?"; }

int icon_for_ext(const char *ext) {
    static const struct { const char *exts; int icon; } table[] = {
        { "txt,text,md,nfo,readme,rtf", ICON_FILE_TEXT },
        { "log", ICON_FILE_LOG },
        { "cfg,conf,ini,config,json,yaml,yml,toml", ICON_FILE_CONFIG },
        { "c,h,cpp,hpp,cc,asm,s,py,js,sh,xml,html,htm,css,rs,go,java,lua,mk", ICON_FILE_CODE },
        { "bmp,ppm,png,jpg,jpeg,gif,ico,tga", ICON_FILE_IMAGE },
        { "wav,mid,midi,mp3,ogg,flac,snd", ICON_FILE_AUDIO },
        { "zip,tar,gz,tgz,bz2,xz,7z,rar", ICON_FILE_ARCHIVE },
        { "bin,elf,exe,img,iso,o,dat,sys,com", ICON_FILE_BINARY },
        { "note", ICON_NOTE },
        { "nvp,paint", ICON_PAINT },
    };
    if (!ext || !*ext) return ICON_FILE_UNKNOWN;
    int reg = filetype_icon(ext);
    if (reg > ICON_NONE && reg < ICON_COUNT) return reg;
    size_t el = strlen(ext);
    for (int i = 0; i < ARRAY_SIZE(table); i++) {
        const char *p = table[i].exts;
        while (*p) {
            const char *s = p;
            while (*p && *p != ',') p++;
            if ((size_t)(p - s) == el && !strncasecmp(s, ext, el)) return table[i].icon;
            if (*p == ',') p++;
        }
    }
    return ICON_FILE_UNKNOWN;
}

int icon_for_path(const char *path) {
    if (!path || !*path) return ICON_FILE_UNKNOWN;
    char norm[VFS_PATH_MAX];
    if (vfs_normalize(NULL, path, norm) != 0) strlcpy(norm, path, sizeof norm);
    if (vfs_is_dir(norm)) {
        static const struct { const char *path; int icon; } special[] = {
            { "/home/user", ICON_FOLDER_HOME }, { "/home/user/Documents", ICON_FOLDER_DOCUMENTS },
            { "/home/user/Pictures", ICON_FOLDER_PICTURES }, { "/home/user/Music", ICON_FOLDER_MUSIC },
            { "/home/user/Downloads", ICON_FOLDER_DOWNLOADS }, { "/home/user/Desktop", ICON_FOLDER_DESKTOP },
            { "/system", ICON_FOLDER_SYSTEM }, { "/", ICON_COMPUTER },
        };
        for (int i = 0; i < ARRAY_SIZE(special); i++)
            if (!strcmp(norm, special[i].path)) return special[i].icon;
        if (!strncmp(norm, "/system/", 8)) return ICON_FOLDER_SYSTEM;
        return ICON_FOLDER;
    }
    const char *base = vfs_basename(norm);
    if (!strcasecmp(base, "syslog") || !strcasecmp(base, "messages")) return ICON_FILE_LOG;
    const char *ext = vfs_extension(norm);
    if (!*ext && !strncmp(norm, "/system/config/", 15)) return ICON_FILE_CONFIG;
    return icon_for_ext(ext);
}

/* ---- Icon Library: a hidden app that shows every icon at 48/32/16 (for checking) ---------- */
#define SHEET_COLS 5
#define CELL_W 178
#define CELL_H 74

typedef struct { bool dark; } sheet_t;

static void sheet_event(window_t *win, const event_t *ev) {
    sheet_t *sh = win_get_userdata(win);
    if (!sh) return;
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        uint32_t bg = sh->dark ? RGB(32, 34, 42) : RGB(250, 250, 252), fg = sh->dark ? RGB(230, 232, 240) : RGB(40, 44, 56);
        gfx_clear(s, bg);
        gfx_text(s, 12, 8, "NovaOS icon library - 48 / 32 / 16 px (click to toggle the background)", fg, FONT_UI_BOLD);
        for (int k = 1; k < ICON_COUNT; k++) {
            int i = k - 1, cx = 8 + (i % SHEET_COLS) * CELL_W, cy = 32 + (i / SHEET_COLS) * CELL_H;
            icon_draw(k, s, cx, cy + 4, 48);
            icon_draw(k, s, cx + 54, cy + 20, 32);
            icon_draw(k, s, cx + 92, cy + 36, 16);
            gfx_text_ellipsis(s, cx + 92, cy + 12, CELL_W - 96, icon_name(k), color_mix(fg, bg, 60), FONT_UI);
        }
        break;
    }
    case EV_MOUSE_DOWN:
        sh->dark = !sh->dark;
        win_invalidate(win);
        break;
    case EV_CLOSE:
        kfree(sh);
        win_set_userdata(win, NULL);
        break;
    default: break;
    }
}

static void sheet_launch(const char *arg) {
    sheet_t *sh = kcalloc(1, sizeof *sh);
    if (!sh) return;
    sh->dark = arg && !strcmp(arg, "dark");
    int rows = (ICON_COUNT - 1 + SHEET_COLS - 1) / SHEET_COLS;
    window_t *w = win_create("Icon Library", SHEET_COLS * CELL_W + 16, 40 + rows * CELL_H, WIN_CENTER);
    if (!w) { kfree(sh); return; }
    win_set_userdata(w, sh);
    win_set_event_handler(w, sheet_event);
}

static void sheet_icon(surface_t *s, int x, int y, int size) {
    icon_render(ICON_FOLDER_PICTURES, s, x, y, size);
}

REGISTER_APP(.name = "Icon Library", .category = CAT_DEVELOPMENT, .description = "Every system icon at every size",
             .launch = sheet_launch, .draw_icon = sheet_icon, .flags = APP_HIDDEN | APP_SINGLE);
