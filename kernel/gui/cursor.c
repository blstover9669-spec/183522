/* cursor.c - mouse cursor shapes (ASCII art: X = black, . = white). */
#include "wm_internal.h"

typedef struct {
    const char *const *art;
    int rows;
    int hx, hy;          /* hotspot */
    bool outline;        /* auto-generate a white outline around black pixels */
} cursor_def_t;

static const char *const arrow_art[] = {
    "X",
    "XX",
    "X.X",
    "X..X",
    "X...X",
    "X....X",
    "X.....X",
    "X......X",
    "X.......X",
    "X........X",
    "X.........X",
    "X..........X",
    "X......XXXXX",
    "X...X..X",
    "X..XX..X",
    "X.X  X..X",
    "XX   X..X",
    "X     X..X",
    "      X..X",
    "       XX",
};

static const char *const ibeam_art[] = {
    "XXX XXX",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "XXX XXX",
};

static const char *const hand_art[] = {
    "     XX         ",
    "    X..X        ",
    "    X..X        ",
    "    X..X        ",
    "    X..XXX      ",
    "    X..X..XXX   ",
    "    X..X..X..XX ",
    " XX X..X..X..X.X",
    "X..XX........X.X",
    "X...X..........X",
    " X.............X",
    "  X............X",
    "  X...........X ",
    "   X..........X ",
    "   X.........X  ",
    "    X........X  ",
    "    X........X  ",
    "    XXXXXXXXXX  ",
};

static const char *const cross_art[] = {
    "       X       ",
    "       X       ",
    "       X       ",
    "       X       ",
    "       X       ",
    "               ",
    "               ",
    "XXXXX  X  XXXXX",
    "               ",
    "               ",
    "       X       ",
    "       X       ",
    "       X       ",
    "       X       ",
    "       X       ",
};

static const char *const move_art[] = {
    "        X        ",
    "       XXX       ",
    "      XXXXX      ",
    "        X        ",
    "        X        ",
    "  X     X     X  ",
    " XX     X     XX ",
    "XXXXXXXXXXXXXXXXX",
    " XX     X     XX ",
    "  X     X     X  ",
    "        X        ",
    "        X        ",
    "      XXXXX      ",
    "       XXX       ",
    "        X        ",
};

static const char *const wait_art[] = {
    "XXXXXXXXXXX",
    "X.........X",
    " X.......X ",
    "  X.XXX.X  ",
    "   X.X.X   ",
    "    X.X    ",
    "    X.X    ",
    "   X...X   ",
    "  X..X..X  ",
    " X..XXX..X ",
    "X.XXXXXXX.X",
    "XXXXXXXXXXX",
};

static const char *const resize_h_art[] = {
    "   X         X   ",
    "  XX         XX  ",
    " XXX         XXX ",
    "XXXXXXXXXXXXXXXXX",
    " XXX         XXX ",
    "  XX         XX  ",
    "   X         X   ",
};

static const char *const resize_v_art[] = {
    "   X   ",
    "  XXX  ",
    " XXXXX ",
    "XXXXXXX",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "   X   ",
    "XXXXXXX",
    " XXXXX ",
    "  XXX  ",
    "   X   ",
};

static const char *const resize_nwse_art[] = {
    "XXXXX        ",
    "XXXX         ",
    "XXX          ",
    "XX X         ",
    "X   X        ",
    "     X       ",
    "      X      ",
    "       X     ",
    "        X   X",
    "         X XX",
    "          XXX",
    "         XXXX",
    "        XXXXX",
};

static const char *const resize_nesw_art[] = {
    "        XXXXX",
    "         XXXX",
    "          XXX",
    "         X XX",
    "        X   X",
    "       X     ",
    "      X      ",
    "     X       ",
    "X   X        ",
    "XX X         ",
    "XXX          ",
    "XXXX         ",
    "XXXXX        ",
};

#define DEF(a, hx, hy, ol) { a, ARRAY_SIZE(a), hx, hy, ol }
static const cursor_def_t defs[CURSOR_COUNT] = {
    [CURSOR_ARROW] = DEF(arrow_art, 0, 0, false),
    [CURSOR_IBEAM] = DEF(ibeam_art, 3, 8, true),
    [CURSOR_HAND] = DEF(hand_art, 5, 0, false),
    [CURSOR_CROSSHAIR] = DEF(cross_art, 7, 7, true),
    [CURSOR_MOVE] = DEF(move_art, 8, 7, true),
    [CURSOR_WAIT] = DEF(wait_art, 5, 6, true),
    [CURSOR_RESIZE_H] = DEF(resize_h_art, 8, 3, true),
    [CURSOR_RESIZE_V] = DEF(resize_v_art, 3, 8, true),
    [CURSOR_RESIZE_NWSE] = DEF(resize_nwse_art, 6, 6, true),
    [CURSOR_RESIZE_NESW] = DEF(resize_nesw_art, 6, 6, true),
};

/* Rasterised masks: 0 = transparent, 1 = black, 2 = white. */
#define CMAX 24
static uint8_t masks[CURSOR_COUNT][CMAX][CMAX];
static bool built;

static void build(void) {
    for (int c = 0; c < CURSOR_COUNT; c++) {
        const cursor_def_t *d = &defs[c];
        if (!d->art) continue;
        int ox = d->outline ? 1 : 0;
        for (int y = 0; y < d->rows; y++) {
            const char *row = d->art[y];
            for (int x = 0; row[x]; x++) {
                if (y + ox >= CMAX || x + ox >= CMAX) continue;
                if (row[x] == 'X') masks[c][y + ox][x + ox] = 1;
                else if (row[x] == '.') masks[c][y + ox][x + ox] = 2;
            }
        }
        if (d->outline) {
            uint8_t tmp[CMAX][CMAX];
            memcpy(tmp, masks[c], sizeof tmp);
            for (int y = 0; y < CMAX; y++)
                for (int x = 0; x < CMAX; x++) {
                    if (tmp[y][x]) continue;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int yy = y + dy, xx = x + dx;
                            if (yy >= 0 && xx >= 0 && yy < CMAX && xx < CMAX && tmp[yy][xx] == 1) masks[c][y][x] = 2;
                        }
                }
        }
    }
    built = true;
}

static void hotspot(int shape, int *hx, int *hy) {
    const cursor_def_t *d = &defs[shape];
    int o = d->outline ? 1 : 0;
    *hx = d->hx + o;
    *hy = d->hy + o;
}

rect_t cursor_rect(int x, int y, int shape) {
    if (shape < 0 || shape >= CURSOR_COUNT || !defs[shape].art) shape = CURSOR_ARROW;
    int hx, hy;
    hotspot(shape, &hx, &hy);
    return rect_make(x - hx, y - hy, CMAX + 2, CMAX + 3);
}

void cursor_draw(surface_t *dst, int x, int y, int shape) {
    if (!built) build();
    if (shape == CURSOR_NONE) return;
    if (shape < 0 || shape >= CURSOR_COUNT || !defs[shape].art) shape = CURSOR_ARROW;
    int hx, hy;
    hotspot(shape, &hx, &hy);
    int ox = x - hx, oy = y - hy;
    /* soft shadow */
    for (int yy = 0; yy < CMAX; yy++)
        for (int xx = 0; xx < CMAX; xx++)
            if (masks[shape][yy][xx]) gfx_pixel(dst, ox + xx + 1, oy + yy + 2, RGBA(0, 0, 0, 40));
    for (int yy = 0; yy < CMAX; yy++)
        for (int xx = 0; xx < CMAX; xx++) {
            uint8_t m = masks[shape][yy][xx];
            if (m == 1) gfx_pixel(dst, ox + xx, oy + yy, RGB(16, 16, 20));
            else if (m == 2) gfx_pixel(dst, ox + xx, oy + yy, RGB(255, 255, 255));
        }
}
