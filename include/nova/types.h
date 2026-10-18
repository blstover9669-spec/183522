/* nova/types.h - basic types and helper macros shared by the whole system. */
#ifndef NOVA_TYPES_H
#define NOVA_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define UNUSED(x) ((void)(x))

#define PACKED      __attribute__((packed))
#define NORETURN    __attribute__((noreturn))
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))

/* A rectangle: top-left corner plus size. Empty when w <= 0 or h <= 0. */
typedef struct rect {
    int x, y, w, h;
} rect_t;

typedef struct point {
    int x, y;
} point_t;

static inline rect_t rect_make(int x, int y, int w, int h) {
    rect_t r = { x, y, w, h };
    return r;
}
static inline bool rect_empty(rect_t r) { return r.w <= 0 || r.h <= 0; }
static inline bool rect_contains(rect_t r, int px, int py) {
    return px >= r.x && py >= r.y && px < r.x + r.w && py < r.y + r.h;
}
static inline rect_t rect_intersect(rect_t a, rect_t b) {
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
    rect_t r = { x0, y0, x1 - x0, y1 - y0 };
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}
static inline rect_t rect_union(rect_t a, rect_t b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
    int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
    rect_t r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}
static inline bool rect_overlaps(rect_t a, rect_t b) {
    return !rect_empty(rect_intersect(a, b));
}
static inline rect_t rect_inset(rect_t r, int d) {
    rect_t o = { r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d };
    return o;
}

#endif
