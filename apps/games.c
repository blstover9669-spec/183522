/*
 * games.c - Games: every game in one window. All apps in the Games category
 * (CAT_GAMES) are shown here with their icon and name, and a click starts
 * one. The games are listed only here - not on the desktop, in the Start
 * menu or in its search (app_in_games() in kernel/gui/apps.c) - but a game
 * that runs gets its button next to the Start button like any app.
 */
#include <nova.h>

#define COLS      5
#define CELL_W    112
#define CELL_H    104
#define MARGIN    16
#define ICON      48
#define MAX_GAMES 32

typedef struct {
    const app_desc_t *games[MAX_GAMES];
    int n;
    int hover, pressed;
} games_t;

static bool is_game(const app_desc_t *a) {
    return a && a->category && !strcasecmp(a->category, CAT_GAMES) && strcasecmp(a->name, "Games") &&
           !(a->flags & APP_HIDDEN);
}

static rect_t cell(int i) {
    return rect_make(MARGIN + (i % COLS) * CELL_W, MARGIN + (i / COLS) * CELL_H, CELL_W, CELL_H);
}

static int cell_at(const games_t *g, int x, int y) {
    for (int i = 0; i < g->n; i++)
        if (rect_contains(rect_inset(cell(i), 4), x, y)) return i;
    return -1;
}

static void games_paint(window_t *win, const games_t *g) {
    surface_t *s = win_surface(win);
    const theme_t *t = theme_get();
    gfx_clear(s, t->window_bg);
    for (int i = 0; i < g->n; i++) {
        rect_t c = cell(i);
        if (i == g->hover)
            gfx_fill_rounded_rect(s, c.x + 4, c.y + 4, c.w - 8, c.h - 8, 10, color_alpha(t->window_text, t->dark ? 34 : 22));
        app_draw_icon(s, g->games[i], c.x + (c.w - ICON) / 2, c.y + 14, ICON);
        gfx_text_aligned(s, rect_make(c.x + 4, c.y + 24 + ICON, c.w - 8, 18), g->games[i]->name, t->window_text, FONT_UI,
                         ALIGN_CENTER);
    }
}

static void games_event(window_t *win, const event_t *ev) {
    games_t *g = win_get_userdata(win);
    if (!g) return;
    switch (ev->type) {
    case EV_PAINT:
        games_paint(win, g);
        break;
    case EV_MOUSE_MOVE: {
        int h = cell_at(g, ev->x, ev->y);
        if (h != g->hover) { g->hover = h; win_invalidate(win); }
        break;
    }
    case EV_MOUSE_LEAVE:
        if (g->hover >= 0) { g->hover = -1; win_invalidate(win); }
        break;
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_LEFT) g->pressed = cell_at(g, ev->x, ev->y);
        break;
    case EV_MOUSE_UP:
        if (ev->button == MOUSE_LEFT) {
            int h = cell_at(g, ev->x, ev->y);
            if (h >= 0 && h == g->pressed) app_launch(g->games[h], NULL);
            g->pressed = -1;
        }
        break;
    case EV_CLOSE:
        free(g);
        win_set_userdata(win, NULL);
        break;
    default:
        break;
    }
}

static void games_launch(const char *arg) {
    (void)arg;
    games_t *g = calloc(1, sizeof *g);
    if (!g) return;
    g->hover = g->pressed = -1;
    for (int i = 0; i < app_count() && g->n < MAX_GAMES; i++)     /* app_get() is sorted by name */
        if (is_game(app_get(i))) g->games[g->n++] = app_get(i);
    int rows = MAX(1, (g->n + COLS - 1) / COLS);
    window_t *w = win_create("Games", 2 * MARGIN + COLS * CELL_W, 2 * MARGIN + rows * CELL_H, WIN_CENTER);
    if (!w) { free(g); return; }
    win_set_userdata(w, g);
    win_set_event_handler(w, games_event);
}

static void games_icon(surface_t *s, int x, int y, int size) { icon_render(ICON_GAMES, s, x, y, size); }

REGISTER_APP(.name = "Games", .category = CAT_GAMES, .description = "All the games in one place",
             .launch = games_launch, .draw_icon = games_icon, .flags = APP_SINGLE);
