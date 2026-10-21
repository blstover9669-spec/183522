/*
 * extras_solitaire.c - Solitaire (Games): Klondike, draw one.
 *
 * Drag a card (or a run of face-up cards) between the tableau, the waste and
 * the foundations; every drop is checked against the Klondike rules and an
 * illegal drop flies back where it came from. Double-click a card to send it
 * to a foundation (or, failing that, to the first tableau column that takes
 * it); right-click or "Auto" plays every card that can go home. Click the
 * stock to turn one card; an empty stock recycles the waste. Unlimited undo,
 * hints, Windows-style scoring, a game clock and statistics kept in
 * ~/.solitaire. When the last tableau card is face up and the stock is empty
 * the game finishes itself; winning starts the classic bouncing-cards
 * cascade with a result card on top.
 *
 * Everything is drawn programmatically: the four suits are implicit vector
 * shapes rendered with 4x4 supersampling into small sprites; card faces
 * (indices, pips, court cards with crowns), the accent-coloured card back and
 * the felt are rendered once per card size into per-window caches and then
 * just blitted.
 *
 * Event driven only: a 16 ms window timer runs while something animates, a
 * slow one keeps the clock ticking, none when idle. Animation frames repaint a
 * list of small dirty rectangles straight into the client surface
 * (win_update), so several cards flying at once cost no more than their own
 * areas.
 *
 * Keys: F2 / Ctrl+N new game, Ctrl+Z / Backspace undo, Space / D turn a card,
 * H hint, A auto-play to the foundations, Esc cancel a drag.
 */
#include <nova.h>

#define SOL_W      760
#define SOL_H      600
#define SOL_MIN_W  560
#define SOL_MIN_H  440
#define TB_H       46          /* toolbar height                          */
#define MARGIN     16
#define SHM        12          /* repaint margin around cards (shadows)   */

#define NPILES   13
#define P_STOCK  0
#define P_WASTE  1
#define P_FOUND  2             /* 2..5 */
#define P_TAB    6             /* 6..12 */
#define IS_FOUND(p) ((p) >= P_FOUND && (p) < P_TAB)
#define IS_TAB(p)   ((p) >= P_TAB && (p) < NPILES)

#define FACEUP   0x80
#define CID(c)   ((c) & 0x3F)
#define SUIT(c)  (CID(c) / 13)
#define RANK(c)  (CID(c) % 13 + 1)
#define ISRED(c) (SUIT(c) == S_HEART || SUIT(c) == S_DIAMOND)
#define ISUP(c)  (((c) & FACEUP) != 0)

enum { S_SPADE, S_HEART, S_DIAMOND, S_CLUB };

#define UNDO_MAX   256
#define MAX_ANIM   64
#define MAX_DIRTY  24

#define COL_RED    RGB(206, 32, 46)
#define COL_BLACK  RGB(28, 30, 38)
#define COL_GLOW   RGB(255, 214, 64)

#define STATS_PATH "/home/user/.solitaire"

#define OV_W 344
#define OV_H 232

enum { BTN_NEW, BTN_UNDO, BTN_HINT, BTN_AUTO, NBTN };

typedef struct { uint8_t n; uint8_t c[52]; } pile_t;
typedef struct { pile_t p[NPILES]; int score, moves; } gstate_t;

typedef struct {
    int pile, idx;           /* where the card lies (it is hidden there until the anim ends) */
    uint8_t card;
    int x0, y0;              /* start position (top-left)                                  */
    uint32_t t0, dur;        /* t0 may lie in the future (staggered deal)                  */
    bool flip;               /* turns face up on the way                                   */
    bool live;               /* started: drawn at (x, y), progress t                      */
    bool fly;                /* moves (else: turns over in place)                         */
    int x, y;
    double t;
    rect_t last;             /* rect drawn (incl. shadow), for repainting                  */
} anim_t;

typedef struct { rect_t r; const char *label; bool hover, pressed, disabled, primary; } sbtn_t;

typedef struct { int played, won, best_score, best_time; } stats_t;

typedef struct {
    window_t *win;
    uint32_t rng;
    gstate_t st;
    gstate_t *undo;          /* ring buffer of UNDO_MAX snapshots */
    int undo_n, undo_head;

    /* layout */
    int W, H, cw, ch, gap, x0, top_y, tab_y, off_up, off_down, rad;
    const font_t *ifont;

    /* render caches (per card size / theme) */
    surface_t *face[52], *back, *felt;
    surface_t *sp_pip[4][2], *sp_corner[4][2], *sp_ace[4];
    int cache_cw, cache_ch;

    /* game */
    bool started, won, counted;
    uint32_t start_ms, end_ms;
    int shown_sec;

    struct {
        bool pending, active;
        int pile, idx, n;
        int mx0, my0, gx, gy, x, y;
        int16_t dy[52];
    } drag;

    anim_t anim[MAX_ANIM];
    int nanim;

    struct { bool on; int src, idx, dst; uint32_t t0; } hint;
    char msg[96];
    bool msg_on;
    uint32_t msg_t0;
    bool autoplay;
    uint32_t auto_next;

    struct {
        bool on, done, flying;
        surface_t *trail;
        int next, f, fcount[4];
        uint8_t card;
        double x, y, vx, vy;
        uint32_t t0, last;
        int acc;
    } cele;
    double ov_p;             /* result card slide-in progress 0..1 */
    struct { bool hover, pressed; } obtn;

    sbtn_t btn[NBTN];
    rect_t dirty[MAX_DIRTY];
    int ndirty;
    int timer, timer_iv;
    int cursor;

    stats_t stats;           /* snapshot shown on the result card */
    bool rec_time, rec_score;
    bool test_almost;        /* launch argument "almost": next deal is one move from a win (tests) */
} sol_t;

/* ---- small helpers ------------------------------------------------------------------ */
static uint32_t rnd(sol_t *g) {
    uint32_t x = g->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return g->rng = x;
}
static int rnd_n(sol_t *g, int n) { return n > 0 ? (int)(rnd(g) % (uint32_t)n) : 0; }

static double ease_out(double t) {
    t = CLAMP(t, 0.0, 1.0);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

static rect_t grow(rect_t r, int d) { return rect_make(r.x - d, r.y - d, r.w + 2 * d, r.h + 2 * d); }

static void fmt_time(char *buf, int size, int secs) {
    if (secs >= 3600) snprintf(buf, (size_t)size, "%d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60);
    else snprintf(buf, (size_t)size, "%d:%02d", secs / 60, secs % 60);
}

/* ---- statistics (~/.solitaire) ------------------------------------------------------- */
static void stats_load(stats_t *st) {
    memset(st, 0, sizeof *st);
    uint32_t size = 0;
    char *d = vfs_read_file(STATS_PATH, &size);
    if (!d) return;
    for (char *p = d; p && *p;) {
        if (!strncmp(p, "played=", 7)) st->played = atoi(p + 7);
        else if (!strncmp(p, "won=", 4)) st->won = atoi(p + 4);
        else if (!strncmp(p, "best_score=", 11)) st->best_score = atoi(p + 11);
        else if (!strncmp(p, "best_time=", 10)) st->best_time = atoi(p + 10);
        p = strchr(p, '\n');
        if (p) p++;
    }
    free(d);
}

static void stats_save(const stats_t *st) {
    char buf[160];
    int n = snprintf(buf, sizeof buf, "played=%d\nwon=%d\nbest_score=%d\nbest_time=%d\n",
                     st->played, st->won, st->best_score, st->best_time);
    vfs_write_file(STATS_PATH, buf, (uint32_t)n);
}

/* ---- vector suit shapes ---------------------------------------------------------------
 * Implicit shapes in unit coordinates (u, v in [-1, 1], v grows downwards). */
#define H_LX 0.47              /* heart lobe centre (mirrored) */
#define H_LY (-0.32)
#define H_R  0.50
#define H_PY 0.93              /* bottom point                 */

static double hp_quad[8];      /* right half of the heart's body: axis, lobe centre, tangent, tip */
static bool hp_ready;

static void heart_init(void) {
    if (hp_ready) return;
    double dx = 0 - H_LX, dy = H_PY - H_LY;
    double d = sqrt(dx * dx + dy * dy);
    double a = atan2(dy, dx) - acos(H_R / d);   /* outer tangent from the tip to the lobe */
    double q[8] = { 0, H_LY, H_LX, H_LY, H_LX + H_R * cos(a), H_LY + H_R * sin(a), 0, H_PY };
    memcpy(hp_quad, q, sizeof q);
    hp_ready = true;
}

static bool in_convex(double x, double y, const double *p, int n) {
    int sgn = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        double c = (p[2 * j] - p[2 * i]) * (y - p[2 * i + 1]) - (p[2 * j + 1] - p[2 * i + 1]) * (x - p[2 * i]);
        if (c > 1e-12) { if (sgn < 0) return false; sgn = 1; }
        else if (c < -1e-12) { if (sgn > 0) return false; sgn = -1; }
    }
    return true;
}

static bool heart_raw(double u, double v) {
    double au = fabs(u), dx = au - H_LX, dy = v - H_LY;
    if (dx * dx + dy * dy <= H_R * H_R) return true;
    return in_convex(au, v, hp_quad, 4);
}

static bool stem_inside(double u, double v, double top) {
    if (v < top || v > 0.95) return false;
    double t = (v - top) / (0.95 - top);
    return fabs(u) <= 0.07 + 0.30 * t * t;
}

static bool suit_inside(int suit, double u, double v) {
    switch (suit) {
    case S_HEART:
        return heart_raw(u, v + 0.055);
    case S_DIAMOND:
        return fabs(u) / 0.76 + fabs(v) / 0.97 <= 1.0;
    case S_SPADE:
        if (heart_raw(u / 0.86, -(v + 0.19) / 0.83)) return true;
        return stem_inside(u, v, 0.25);
    default: {   /* club */
        double vv = v + 0.05, au = fabs(u);
        const double r = 0.34;
        if (u * u + (vv + 0.50) * (vv + 0.50) <= r * r) return true;
        if ((au - 0.47) * (au - 0.47) + (vv - 0.08) * (vv - 0.08) <= r * r) return true;
        if (u * u + (vv + 0.05) * (vv + 0.05) <= 0.22 * 0.22) return true;
        return stem_inside(u, vv, 0.12);
    }
    }
}

/* ---- supersampled shape rasteriser ------------------------------------------------------ */
typedef bool (*inside_fn)(double x, double y, const void *ctx);

/* Fill the pixels of [x0,x1) x [y0,y1) that the shape covers, anti-aliased with
   q x q samples per pixel. Floating point is slow under emulation (QEMU), so a
   pixel whose four corners and centre agree is taken as fully in or out
   (corners are shared between neighbours); only edge pixels are supersampled. */
static void ss_fill_q(surface_t *s, int x0, int y0, int x1, int y1, inside_fn fn, const void *ctx, uint32_t col, int q) {
    x0 = MAX(x0, s->clip.x);
    y0 = MAX(y0, s->clip.y);
    x1 = MIN(x1, s->clip.x + s->clip.w);
    y1 = MIN(y1, s->clip.y + s->clip.h);
    int w = x1 - x0;
    if (w <= 0 || y1 <= y0) return;
    uint8_t *top = malloc((size_t)(2 * (w + 1)));
    if (!top) return;
    uint8_t *bot = top + w + 1;
    for (int i = 0; i <= w; i++) top[i] = fn(x0 + i, y0, ctx);
    int ca = COLOR_A(col), qq = q * q;
    for (int y = y0; y < y1; y++) {
        for (int i = 0; i <= w; i++) bot[i] = fn(x0 + i, y + 1, ctx);
        for (int x = x0; x < x1; x++) {
            int i = x - x0;
            int k = top[i] + top[i + 1] + bot[i] + bot[i + 1] + fn(x + 0.5, y + 0.5, ctx);
            int n;
            if (k == 0) continue;
            if (k == 5) n = qq;
            else {
                n = 0;
                for (int j = 0; j < q; j++)
                    for (int m = 0; m < q; m++)
                        if (fn(x + (m + 0.5) / q, y + (j + 0.5) / q, ctx)) n++;
            }
            if (n) gfx_pixel(s, x, y, color_alpha(col, ca * n / qq));
        }
        uint8_t *t = top;
        top = bot;
        bot = t;
    }
    free(top < bot ? top : bot);
}

static void ss_fill(surface_t *s, int x0, int y0, int x1, int y1, inside_fn fn, const void *ctx, uint32_t col) {
    ss_fill_q(s, x0, y0, x1, y1, fn, ctx, col, 4);
}

typedef struct { double cx, cy, half; int suit; bool flip; } suit_ctx_t;

static bool suit_px(double x, double y, const void *p) {
    const suit_ctx_t *c = p;
    double u = (x - c->cx) / c->half, v = (y - c->cy) / c->half;
    if (c->flip) v = -v;   /* the shapes are mirror symmetric, so this is a 180 degree turn */
    return suit_inside(c->suit, u, v);
}

static void draw_suit(surface_t *s, int suit, double cx, double cy, double size, bool flip, uint32_t col) {
    heart_init();
    suit_ctx_t c = { cx, cy, size / 2, suit, flip };
    ss_fill(s, (int)floor(cx - size / 2) - 1, (int)floor(cy - size / 2) - 1,
            (int)ceil(cx + size / 2) + 1, (int)ceil(cy + size / 2) + 1, suit_px, &c, col);
}

static surface_t *make_suit(int suit, int size, bool flip, uint32_t col) {
    if (size < 4) size = 4;
    surface_t *s = surface_create_alpha(size, size);
    if (s) draw_suit(s, suit, size / 2.0, size / 2.0, size, flip, col);
    return s;
}

typedef struct { const double *p; int n; double ox, oy, sc; } poly_ctx_t;

static bool poly_px(double x, double y, const void *pp) {
    const poly_ctx_t *c = pp;
    double u = (x - c->ox) / c->sc, v = (y - c->oy) / c->sc;
    bool in = false;
    for (int i = 0, j = c->n - 1; i < c->n; j = i++) {
        double xi = c->p[2 * i], yi = c->p[2 * i + 1], xj = c->p[2 * j], yj = c->p[2 * j + 1];
        if ((yi > v) != (yj > v) && u < (xj - xi) * (v - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

/* Polygon given in unit coordinates, placed at (ox, oy) and scaled by sc. */
static void ss_poly(surface_t *s, const double *p, int n, double ox, double oy, double sc, uint32_t col) {
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int i = 0; i < n; i++) {
        x0 = MIN(x0, p[2 * i]); x1 = MAX(x1, p[2 * i]);
        y0 = MIN(y0, p[2 * i + 1]); y1 = MAX(y1, p[2 * i + 1]);
    }
    poly_ctx_t c = { p, n, ox, oy, sc };
    ss_fill(s, (int)floor(ox + x0 * sc) - 1, (int)floor(oy + y0 * sc) - 1,
            (int)ceil(ox + x1 * sc) + 1, (int)ceil(oy + y1 * sc) + 1, poly_px, &c, col);
}

static void ss_rect(surface_t *s, double x0, double y0, double x1, double y1, double ox, double oy, double sc,
                    uint32_t col) {
    double p[8] = { x0, y0, x1, y0, x1, y1, x0, y1 };
    ss_poly(s, p, 4, ox, oy, sc, col);
}

/* Crowns for the court cards, centred at (cx, cy), w wide. */
static void draw_crown(surface_t *s, int rank, double cx, double cy, double w, bool red) {
    double sc = w / 2;
    uint32_t gold = RGB(236, 182, 46), gold_d = RGB(186, 128, 24);
    uint32_t gem1 = RGB(214, 40, 54), gem2 = RGB(44, 96, 210);
    if (rank == 13) {
        static const double k[] = { -0.9, 0.45, -0.9, -0.30, -0.45, 0.04, 0, -0.52, 0.45, 0.04, 0.9, -0.30, 0.9, 0.45 };
        ss_poly(s, k, 7, cx, cy, sc, gold);
        gfx_fill_circle_aa(s, cx - 0.9 * sc, cy - 0.36 * sc, 0.13 * sc, gold);
        gfx_fill_circle_aa(s, cx, cy - 0.60 * sc, 0.14 * sc, gold);
        gfx_fill_circle_aa(s, cx + 0.9 * sc, cy - 0.36 * sc, 0.13 * sc, gold);
        ss_rect(s, -0.9, 0.22, 0.9, 0.45, cx, cy, sc, gold_d);
        gfx_fill_circle_aa(s, cx - 0.45 * sc, cy + 0.335 * sc, 0.075 * sc, gem1);
        gfx_fill_circle_aa(s, cx, cy + 0.335 * sc, 0.075 * sc, gem2);
        gfx_fill_circle_aa(s, cx + 0.45 * sc, cy + 0.335 * sc, 0.075 * sc, gem1);
    } else if (rank == 12) {
        static const double q[] = { -0.85, 0.45, -0.95, -0.10, -0.58, 0.12, -0.42, -0.34, -0.16, 0.06, 0, -0.50,
                                    0.16, 0.06, 0.42, -0.34, 0.58, 0.12, 0.95, -0.10, 0.85, 0.45 };
        ss_poly(s, q, 11, cx, cy, sc, gold);
        static const double tips[] = { -0.95, -0.15, -0.42, -0.40, 0, -0.56, 0.42, -0.40, 0.95, -0.15 };
        for (int i = 0; i < 5; i++)
            gfx_fill_circle_aa(s, cx + tips[2 * i] * sc, cy + tips[2 * i + 1] * sc, 0.095 * sc, gold);
        ss_rect(s, -0.86, 0.24, 0.86, 0.45, cx, cy, sc, gold_d);
        gfx_fill_circle_aa(s, cx, cy + 0.345 * sc, 0.07 * sc, red ? gem2 : gem1);
    } else {
        /* jack: a cap with a feather */
        double d[2 * 14];
        for (int i = 0; i <= 12; i++) {
            double a = M_PI * i / 12;
            d[2 * i] = 0.78 * cos(a);
            d[2 * i + 1] = 0.26 - 0.66 * sin(a);
        }
        d[26] = 0.78; d[27] = 0.26;
        static const double fe[] = { 0.18, -0.20, 0.52, -0.62, 0.98, -0.86, 0.72, -0.46, 0.40, -0.12 };
        ss_poly(s, fe, 5, cx, cy, sc, red ? gem2 : gem1);
        ss_poly(s, d, 14, cx, cy, sc, gold);
        ss_rect(s, -0.92, 0.22, 0.92, 0.45, cx, cy, sc, gold_d);
        gfx_fill_circle_aa(s, cx, cy - 0.44 * sc, 0.11 * sc, gold_d);
    }
}

/* ---- layout ----------------------------------------------------------------------------- */
static int pad_x(const sol_t *g) { return MAX(3, g->cw / 16); }
static int pad_y(const sol_t *g) { return MAX(1, g->cw / 34); }

static void layout_buttons(sol_t *g) {
    static const char *const labels[NBTN] = { "New Game", "Undo", "Hint", "Auto" };
    int x = 10;
    for (int i = 0; i < NBTN; i++) {
        int w = MAX(58, font_text_width(FONT_UI, labels[i]) + 26);
        g->btn[i].r = rect_make(x, 8, w, 30);
        g->btn[i].label = labels[i];
        g->btn[i].primary = i == BTN_NEW;
        x += w + 6;
    }
}

static void layout(sol_t *g) {
    g->W = win_width(g->win);
    g->H = win_height(g->win);
    int cw = (g->W - 2 * MARGIN) * 100 / 796;
    int ch_max = (g->H - TB_H - 3 * MARGIN) * 10 / 32;
    if (cw * 7 / 5 > ch_max) cw = ch_max * 5 / 7;
    cw = CLAMP(cw, 46, 132);
    int ch = cw * 7 / 5;
    int gap = (g->W - 2 * MARGIN - 7 * cw) / 6;
    gap = CLAMP(gap, cw / 8, cw / 3);
    g->cw = cw;
    g->ch = ch;
    g->gap = gap;
    g->x0 = (g->W - 7 * cw - 6 * gap) / 2;
    g->top_y = TB_H + MAX(12, cw / 7);
    g->tab_y = g->top_y + ch + MAX(14, cw / 6);
    g->ifont = cw >= 112 ? FONT_UI_TITLE : cw >= 72 ? FONT_UI_LARGE : FONT_UI_BOLD;
    g->rad = MAX(4, cw / 11);
    g->off_up = MAX(ch / 5, pad_y(g) + font_ascent(g->ifont) + 5);
    g->off_down = MAX(5, ch / 12);
    layout_buttons(g);
}

static int col_x(const sol_t *g, int col) { return g->x0 + col * (g->cw + g->gap); }

static rect_t slot_rect(const sol_t *g, int pile) {
    int col = pile == P_STOCK ? 0 : pile == P_WASTE ? 1 : IS_FOUND(pile) ? 3 + pile - P_FOUND : pile - P_TAB;
    return rect_make(col_x(g, col), IS_TAB(pile) ? g->tab_y : g->top_y, g->cw, g->ch);
}

static rect_t felt_rect(const sol_t *g) { return rect_make(0, TB_H, g->W, g->H - TB_H); }

/* Offsets between cards of a tableau column, squeezed when the column would
   run off the bottom: face-down offsets shrink first, then face-up ones. */
static void col_offsets(const sol_t *g, int pile, int *od, int *ou) {
    const pile_t *p = &g->st.p[pile];
    int nd = 0, nu = 0;
    for (int i = 0; i < p->n - 1; i++) {
        if (ISUP(p->c[i])) nu++;
        else nd++;
    }
    int d = g->off_down, u = g->off_up;
    int avail = g->H - 8 - g->tab_y - g->ch;
    if (nd * d + nu * u > avail) {
        if (nd) d = MAX(3, MIN(d, (avail - nu * u) / nd));
        if (nu && nd * d + nu * u > avail) u = MAX(9, (avail - nd * d) / nu);
    }
    *od = d;
    *ou = u;
}

/* Rect of card idx of a pile (idx == n gives the spot for the next card). */
static rect_t card_rect(const sol_t *g, int pile, int idx) {
    rect_t r = slot_rect(g, pile);
    if (IS_TAB(pile) && idx > 0) {
        const pile_t *p = &g->st.p[pile];
        int od, ou, y = 0;
        col_offsets(g, pile, &od, &ou);
        for (int i = 0; i < idx && i < p->n; i++) y += ISUP(p->c[i]) ? ou : od;
        r.y += y;
    }
    return r;
}

/* Cards in a pile as currently shown (the win cascade empties the foundations). */
static int pile_count(const sol_t *g, int pile) {
    if (g->cele.on && IS_FOUND(pile)) return g->cele.fcount[pile - P_FOUND];
    return g->st.p[pile].n;
}

static bool is_hidden(const sol_t *g, int pile, int idx) {
    if (g->drag.active && pile == g->drag.pile && idx >= g->drag.idx) return true;
    for (int i = 0; i < g->nanim; i++)
        if (g->anim[i].pile == pile && g->anim[i].idx == idx) return true;
    return false;
}

/* ---- render caches -------------------------------------------------------------------------- */
static void free_surf(surface_t **s) {
    if (*s) surface_destroy(*s);
    *s = NULL;
}

static void free_card_cache(sol_t *g) {
    for (int i = 0; i < 52; i++) free_surf(&g->face[i]);
    for (int i = 0; i < 4; i++) {
        free_surf(&g->sp_pip[i][0]); free_surf(&g->sp_pip[i][1]);
        free_surf(&g->sp_corner[i][0]); free_surf(&g->sp_corner[i][1]);
        free_surf(&g->sp_ace[i]);
    }
    free_surf(&g->back);
    g->cache_cw = g->cache_ch = 0;
}

static int corner_suit_size(const sol_t *g) { return MAX(7, font_ascent(g->ifont) * 62 / 100); }
static int pip_size(const sol_t *g) { return MAX(6, MIN(g->cw * 18 / 100, (g->ch - 2 * g->off_up - 4) * 100 / 424)); }

static void ensure_cache(sol_t *g) {
    if (g->cache_cw == g->cw && g->cache_ch == g->ch) return;
    free_card_cache(g);
    g->cache_cw = g->cw;
    g->cache_ch = g->ch;
    int ps = pip_size(g), cs = corner_suit_size(g), as = MAX(12, g->cw * 50 / 100);
    for (int s = 0; s < 4; s++) {
        uint32_t col = (s == S_HEART || s == S_DIAMOND) ? COL_RED : COL_BLACK;
        g->sp_pip[s][0] = make_suit(s, ps, false, col);
        g->sp_pip[s][1] = make_suit(s, ps, true, col);
        g->sp_corner[s][0] = make_suit(s, cs, false, col);
        g->sp_corner[s][1] = make_suit(s, cs, true, col);
        g->sp_ace[s] = make_suit(s, as, false, col);
    }
}

static void card_base(surface_t *s, int w, int h, int r, uint32_t top, uint32_t bot) {
    gfx_fill_rounded_rect(s, 0, 0, w, h, r, RGBA(0, 0, 0, 118));
    gfx_fill_rounded_gradient_v(s, 1, 1, w - 2, h - 2, r - 1, top, bot);
}

static const char *rank_name(int rank) {
    static const char *const n[14] = { "", "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" };
    return n[CLAMP(rank, 0, 13)];
}

static void blit_rot180(surface_t *dst, const surface_t *src, int dx, int dy) {
    for (int y = 0; y < src->h; y++) {
        const uint32_t *row = surface_row(src, y);
        for (int x = 0; x < src->w; x++)
            if (COLOR_A(row[x])) gfx_pixel(dst, dx + src->w - 1 - x, dy + src->h - 1 - y, row[x]);
    }
}

static void blit_c(surface_t *dst, const surface_t *src, int cx, int cy) {
    if (src) gfx_blit_alpha(dst, cx - src->w / 2, cy - src->h / 2, src, 255);
}

/* pip positions: column 0..2, row 0..12 (6 = middle); rows below 6 are upside down */
static const uint8_t pip_tab[11][10][2] = {
    [2]  = { {1,0}, {1,12} },
    [3]  = { {1,0}, {1,6}, {1,12} },
    [4]  = { {0,0}, {2,0}, {0,12}, {2,12} },
    [5]  = { {0,0}, {2,0}, {1,6}, {0,12}, {2,12} },
    [6]  = { {0,0}, {2,0}, {0,6}, {2,6}, {0,12}, {2,12} },
    [7]  = { {0,0}, {2,0}, {1,3}, {0,6}, {2,6}, {0,12}, {2,12} },
    [8]  = { {0,0}, {2,0}, {1,3}, {0,6}, {2,6}, {1,9}, {0,12}, {2,12} },
    [9]  = { {0,0}, {2,0}, {0,4}, {2,4}, {1,6}, {0,8}, {2,8}, {0,12}, {2,12} },
    [10] = { {0,0}, {2,0}, {1,2}, {0,4}, {2,4}, {0,8}, {2,8}, {1,10}, {0,12}, {2,12} },
};

static surface_t *render_face(sol_t *g, int id) {
    int cw = g->cw, ch = g->ch;
    int suit = id / 13, rank = id % 13 + 1;
    bool red = suit == S_HEART || suit == S_DIAMOND;
    uint32_t col = red ? COL_RED : COL_BLACK;
    surface_t *s = surface_create_alpha(cw, ch);
    if (!s) return NULL;
    card_base(s, cw, ch, g->rad, RGB(255, 255, 255), RGB(243, 243, 238));

    /* indices: rank top-left, suit top-right, and the same turned around at the bottom */
    const font_t *f = g->ifont;
    const char *rs = rank_name(rank);
    int px = pad_x(g), py = pad_y(g);
    int tw = font_text_width(f, rs), th = font_height(f);
    gfx_text(s, px, py, rs, col, f);
    surface_t *tmp = surface_create_alpha(tw + 1, th);
    if (tmp) {
        gfx_text(tmp, 0, 0, rs, col, f);
        blit_rot180(s, tmp, cw - px - tmp->w, ch - py - tmp->h);
        surface_destroy(tmp);
    }
    surface_t *cs = g->sp_corner[suit][0], *csf = g->sp_corner[suit][1];
    if (cs && csf) {
        int asc = font_ascent(f), cap = asc * 72 / 100;
        int cy = py + asc - cap / 2 - cs->h / 2;
        gfx_blit_alpha(s, cw - px - cs->w, cy, cs, 255);
        gfx_blit_alpha(s, px, ch - cy - csf->h, csf, 255);
    }

    if (rank == 1) {
        blit_c(s, g->sp_ace[suit], cw / 2, ch / 2);
    } else if (rank <= 10) {
        int ps = g->sp_pip[suit][0] ? g->sp_pip[suit][0]->w : 8;
        int x0 = cw * 27 / 100, x1 = cw - x0;
        int y0 = g->off_up + ps / 2 + 2, y1 = ch - y0;
        for (int i = 0; i < rank; i++) {
            int c = pip_tab[rank][i][0], row = pip_tab[rank][i][1];
            int x = x0 + (x1 - x0) * c / 2, y = y0 + (y1 - y0) * row / 12;
            blit_c(s, g->sp_pip[suit][row > 6], x, y + (ps & 1 ? 0 : (row > 6 ? 1 : 0)));
        }
    } else {
        int fx = cw * 17 / 100, fy = g->off_up + 1;
        int fw = cw - 2 * fx, fh = ch - 2 * fy;
        uint32_t tint = red ? RGB(253, 238, 233) : RGB(233, 239, 251);
        uint32_t frame = red ? RGB(226, 146, 146) : RGB(146, 162, 204);
        int fr = MAX(3, g->rad - 2);
        gfx_fill_rounded_rect(s, fx, fy, fw, fh, fr, frame);
        gfx_fill_rounded_rect(s, fx + 1, fy + 1, fw - 2, fh - 2, fr - 1, tint);
        draw_crown(s, rank, cw / 2.0, fy + fh * 0.23, fw * 0.56, red);
        const font_t *lf = fh >= 66 ? FONT_UI_TITLE : FONT_UI_LARGE;
        gfx_text_aligned(s, rect_make(fx, fy + fh * 40 / 100, fw, fh * 32 / 100), rs, col, lf,
                         ALIGN_CENTER | ALIGN_MIDDLE);
        blit_c(s, g->sp_corner[suit][0], cw / 2, fy + fh * 83 / 100);
    }
    return s;
}

/* Anti-aliased coverage of a rounded rect at a pixel centre. */
static double rrect_cov(double x, double y, double rx, double ry, double rw, double rh, double r) {
    double cx = CLAMP(x, rx + r, rx + rw - r), cy = CLAMP(y, ry + r, ry + rh - r);
    double dx = x - cx, dy = y - cy;
    if (x < rx || y < ry || x > rx + rw || y > ry + rh) return 0;
    if (dx == 0 && dy == 0) return 1;
    double d = sqrt(dx * dx + dy * dy);
    return CLAMP(r - d + 0.5, 0.0, 1.0);
}

typedef struct { double cx, cy, r0, r1; } star_ctx_t;

static bool star_px(double x, double y, const void *p) {
    const star_ctx_t *c = p;
    double dx = fabs(x - c->cx), dy = fabs(y - c->cy);
    /* four-pointed star: concave diamond */
    double u = dx / c->r0, v = dy / c->r0;
    if (u > 1 || v > 1) return false;
    return sqrt(u) + sqrt(v) <= 1.0 + c->r1;
}

static surface_t *render_back(sol_t *g) {
    int cw = g->cw, ch = g->ch;
    surface_t *s = surface_create_alpha(cw, ch);
    if (!s) return NULL;
    const theme_t *t = theme_get();
    card_base(s, cw, ch, g->rad, RGB(252, 252, 252), RGB(238, 238, 236));
    uint32_t a = t->accent;
    uint32_t c1 = color_mix(a, COLOR_WHITE, 40), c2 = color_mix(a, COLOR_BLACK, 96);
    int m = MAX(3, cw / 15);
    double ix = m, iy = m, iw = cw - 2 * m, ih = ch - 2 * m, ir = MAX(2, g->rad - m / 2);
    double step = MAX(6.0, cw / 8.0);
    for (int y = m; y < ch - m; y++) {
        uint32_t base = color_mix(c1, c2, (int)((y - iy) * 255 / ih));
        uint32_t *row = surface_row(s, y);
        for (int x = m; x < cw - m; x++) {
            double cov = rrect_cov(x + 0.5, y + 0.5, ix, iy, iw, ih, ir);
            if (cov <= 0) continue;
            double u = fmod(x + y + 0.5 + 4000 * step, step), v = fmod(x - y + 0.5 + 4000 * step, step);
            double du = MIN(u, step - u), dv = MIN(v, step - v);
            double l = MAX(CLAMP(1.3 - du, 0.0, 1.0), CLAMP(1.3 - dv, 0.0, 1.0));
            uint32_t c = color_mix(base, COLOR_WHITE, (int)(l * 60));
            row[x] = color_mix(row[x] | 0xFF000000u, c, (int)(cov * 255)) | 0xFF000000u;
        }
    }
    gfx_rounded_rect(s, m + 3, m + 3, cw - 2 * m - 6, ch - 2 * m - 6, MAX(2, (int)ir - 2), RGBA(255, 255, 255, 120));
    /* emblem */
    double cx = cw / 2.0, cy = ch / 2.0, er = cw * 0.21;
    gfx_fill_circle_aa(s, cx, cy, er, color_mix(c2, c1, 60));
    gfx_circle_aa(s, cx, cy, er, 1.6, RGBA(255, 255, 255, 200));
    star_ctx_t sc = { cx, cy, er * 0.82, 0.0 };
    ss_fill(s, (int)(cx - er), (int)(cy - er), (int)(cx + er) + 1, (int)(cy + er) + 1, star_px, &sc,
            RGBA(255, 255, 255, 235));
    gfx_fill_circle_aa(s, cx, cy, er * 0.16, c2);
    return s;
}

static void build_felt(sol_t *g) {
    free_surf(&g->felt);
    int W = g->W, H = g->H;
    surface_t *s = surface_create(W, H);
    if (!s) return;
    bool dark = theme_get()->dark;
    uint32_t top = dark ? RGB(22, 84, 56) : RGB(30, 122, 76);
    uint32_t bot = dark ? RGB(11, 50, 33) : RGB(17, 86, 51);
    int fh = MAX(1, H - TB_H);
    int cx = W / 2, cy = TB_H + fh * 3 / 10;
    int rr = MAX(1, (W * W + fh * fh) / 4);
    for (int y = TB_H; y < H; y++) {
        uint32_t base = color_mix(top, bot, (y - TB_H) * 255 / fh);
        int br = COLOR_R(base), bg = COLOR_G(base), bb = COLOR_B(base);
        uint32_t *row = surface_row(s, y);
        int dy = y - cy;
        for (int x = 0; x < W; x++) {
            int dx = x - cx;
            int d64 = (int)(((int64_t)(dx * dx + dy * dy) * 64) / rr);
            uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
            h = (h ^ (h >> 13)) * 1274126177u;
            int delta = 16 - d64 * 40 / 64 + (int)((h >> 24) & 7) - 3;
            int r = CLAMP(br + delta * 3 / 4, 0, 255), gg = CLAMP(bg + delta, 0, 255), b = CLAMP(bb + delta * 3 / 4, 0, 255);
            row[x] = RGB(r, gg, b);
        }
    }
    g->felt = s;
}

static surface_t *card_surf(sol_t *g, uint8_t c) {
    ensure_cache(g);
    if (!ISUP(c)) {
        if (!g->back) g->back = render_back(g);
        return g->back;
    }
    int id = CID(c);
    if (id >= 52) return NULL;
    if (!g->face[id]) g->face[id] = render_face(g, id);
    return g->face[id];
}

static void draw_card(sol_t *g, surface_t *s, uint8_t c, int x, int y) {
    if (!rect_overlaps(rect_make(x, y, g->cw, g->ch), s->clip)) return;
    surface_t *cs = card_surf(g, c);
    if (cs) gfx_blit_alpha(s, x, y, cs, 255);
}

/* Card squeezed horizontally to w256/256 of its width around its centre (flip animation). */
static void draw_card_scaled(sol_t *g, surface_t *s, uint8_t c, int x, int y, int w256) {
    int w = g->cw * w256 / 256;
    if (w < 2) return;
    if (w >= g->cw) { draw_card(g, s, c, x, y); return; }
    int dx = x + (g->cw - w) / 2;
    if (!rect_overlaps(rect_make(dx, y, w, g->ch), s->clip)) return;
    surface_t *cs = card_surf(g, c);
    if (cs) gfx_blit_scaled(s, dx, y, w, g->ch, cs, 0, 0, cs->w, cs->h, true);
}

/* ---- dirty rectangles ---------------------------------------------------------------------- */
static void dirty_add(sol_t *g, rect_t r) {
    r = rect_intersect(r, rect_make(0, 0, g->W, g->H));
    if (rect_empty(r)) return;
    for (int i = 0; i < g->ndirty; i++)
        if (rect_overlaps(g->dirty[i], r)) {
            g->dirty[i] = rect_union(g->dirty[i], r);
            return;
        }
    if (g->ndirty < MAX_DIRTY) g->dirty[g->ndirty++] = r;
    else g->dirty[MAX_DIRTY - 1] = rect_union(g->dirty[MAX_DIRTY - 1], r);
}

static void dirty_card(sol_t *g, rect_t r) { dirty_add(g, grow(r, SHM)); }

static void dirty_pile(sol_t *g, int pile) {
    rect_t r = slot_rect(g, pile);
    if (IS_TAB(pile)) r.h = g->H - r.y;
    dirty_card(g, r);
}

static void dirty_toolbar(sol_t *g) { dirty_add(g, rect_make(0, 0, g->W, TB_H)); }

static void draw_all(sol_t *g, surface_t *s);

/* Repaint the collected rects straight into the client surface. Rects touching
   the bottom rows go through the normal EV_PAINT path, which also redraws the
   window's rounded bottom corners. */
static void dirty_flush(sol_t *g) {
    if (!g->ndirty) return;
    surface_t *s = win_surface(g->win);
    for (int i = 0; i < g->ndirty; i++) {
        rect_t r = g->dirty[i];
        if (r.y + r.h > g->H - 14) {
            win_invalidate_rect(g->win, r.x, r.y, r.w, r.h);
            continue;
        }
        surface_set_clip(s, r);
        draw_all(g, s);
        surface_reset_clip(s);
        win_update(g->win, r.x, r.y, r.w, r.h);
    }
    g->ndirty = 0;
}

/* ---- drawing ------------------------------------------------------------------------------------- */
static uint32_t muted(void) {
    const theme_t *t = theme_get();
    return color_mix(t->window_text, t->window_bg, 100);
}

static int elapsed_sec(const sol_t *g) {
    if (!g->started) return 0;
    uint32_t end = g->won ? g->end_ms : sys_uptime_ms();
    return (int)((end - g->start_ms) / 1000);
}

static void draw_toolbar(sol_t *g, surface_t *s) {
    if (!rect_overlaps(rect_make(0, 0, g->W, TB_H), s->clip)) return;
    const theme_t *t = theme_get();
    gfx_fill_rect(s, 0, 0, g->W, TB_H, t->window_bg);
    gfx_hline(s, 0, TB_H - 1, g->W, t->window_border);
    int bx1 = 0;
    for (int i = 0; i < NBTN; i++) {
        sbtn_t *b = &g->btn[i];
        int st = 0;
        if (b->primary) st |= UI_STATE_DEFAULT;
        if (b->disabled) st |= UI_STATE_DISABLED;
        else if (b->pressed && b->hover) st |= UI_STATE_PRESSED;
        else if (b->hover) st |= UI_STATE_HOVER;
        ui_draw_button(s, b->r, b->label, st);
        bx1 = b->r.x + b->r.w;
    }
    /* statistics, right aligned */
    char v[3][24];
    const char *lab[3] = { "Time", "Moves", "Score" };
    fmt_time(v[0], sizeof v[0], elapsed_sec(g));
    snprintf(v[1], sizeof v[1], "%d", g->st.moves);
    snprintf(v[2], sizeof v[2], "%d", g->st.score);
    int x = g->W - 14;
    for (int i = 0; i < 3; i++) {
        int vw = font_text_width(FONT_UI_BOLD, v[i]), lw = font_text_width(FONT_UI, lab[i]);
        int w = lw + 6 + MAX(vw, i == 0 ? font_text_width(FONT_UI_BOLD, "00:00") : vw);
        if (x - w < bx1 + 16) break;
        int vx = x - vw;
        gfx_text(s, vx, (TB_H - font_height(FONT_UI_BOLD)) / 2, v[i], t->window_text, FONT_UI_BOLD);
        gfx_text(s, x - w, (TB_H - font_height(FONT_UI)) / 2, lab[i], muted(), FONT_UI);
        x -= w + 20;
    }
}

static void draw_felt(sol_t *g, surface_t *s) {
    rect_t r = rect_intersect(s->clip, felt_rect(g));
    if (rect_empty(r)) return;
    if (!g->felt || g->felt->w != g->W || g->felt->h != g->H) build_felt(g);
    if (g->felt) gfx_blit_rect(s, r.x, r.y, g->felt, r.x, r.y, r.w, r.h);
    else gfx_fill_rect(s, r.x, r.y, r.w, r.h, RGB(24, 104, 64));
}

typedef struct { double cx, cy, r0, r1; } ring_ctx_t;

static bool recycle_px(double x, double y, const void *p) {
    const ring_ctx_t *c = p;
    double dx = x - c->cx, dy = y - c->cy, d = sqrt(dx * dx + dy * dy);
    double a = atan2(dy, dx);   /* -pi..pi, 0 = right, positive = down */
    if (d >= c->r0 && d <= c->r1 && !(a > -1.25 && a < -0.35)) return true;
    /* arrow head at the end of the arc (angle -0.35 ... pointing clockwise) */
    double ex = c->cx + cos(-1.25) * (c->r0 + c->r1) / 2, ey = c->cy + sin(-1.25) * (c->r0 + c->r1) / 2;
    double w = (c->r1 - c->r0) * 1.6;
    double tri[6] = { ex - w * 0.9, ey - w * 0.95, ex - w * 0.9, ey + w * 0.95, ex + w * 1.1, ey + 0.1 * w };
    return in_convex(x, y, tri, 3);
}

static void draw_slot(sol_t *g, surface_t *s, int pile) {
    rect_t r = slot_rect(g, pile);
    if (!rect_overlaps(grow(r, 2), s->clip)) return;
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, g->rad, RGBA(0, 0, 0, 34));
    gfx_rounded_rect(s, r.x, r.y, r.w, r.h, g->rad, RGBA(255, 255, 255, 64));
    uint32_t ink = RGBA(255, 255, 255, 70);
    if (pile == P_STOCK) {
        if (g->st.p[P_WASTE].n > 0 && !g->cele.on) {
            double rad = g->cw * 0.22;
            ring_ctx_t c = { r.x + r.w / 2.0, r.y + r.h / 2.0, rad - MAX(2.0, g->cw / 26.0), rad };
            ss_fill(s, (int)(c.cx - rad * 1.6), (int)(c.cy - rad * 1.6), (int)(c.cx + rad * 1.6),
                    (int)(c.cy + rad * 1.6), recycle_px, &c, RGBA(255, 255, 255, 110));
        }
    } else if (IS_FOUND(pile)) {
        gfx_text_aligned(s, r, "A", ink, g->cw >= 72 ? FONT_UI_TITLE : FONT_UI_LARGE, ALIGN_CENTER | ALIGN_MIDDLE);
    } else if (IS_TAB(pile)) {
        gfx_text_aligned(s, r, "K", RGBA(255, 255, 255, 40), g->cw >= 72 ? FONT_UI_TITLE : FONT_UI_LARGE,
                         ALIGN_CENTER | ALIGN_MIDDLE);
    }
}

static void pile_shadow(sol_t *g, surface_t *s, int x, int y, int w, int h) {
    if (!rect_overlaps(rect_make(x - 8, y - 8, w + 16, h + 16), s->clip)) return;
    gfx_shadow(s, x, y + 2, w, h, g->rad, 6, 46);
}

static void draw_pile(sol_t *g, surface_t *s, int pile) {
    const pile_t *p = &g->st.p[pile];
    int n = pile_count(g, pile);
    if (IS_TAB(pile)) {
        int vis = 0;
        while (vis < n && !is_hidden(g, pile, vis)) vis++;
        if (vis > 0) {
            rect_t a = card_rect(g, pile, 0), b = card_rect(g, pile, vis - 1);
            pile_shadow(g, s, a.x, a.y, g->cw, b.y + g->ch - a.y);
        }
        for (int i = 0; i < vis; i++) {
            rect_t r = card_rect(g, pile, i);
            draw_card(g, s, p->c[i], r.x, r.y);
        }
        return;
    }
    int top = n - 1;
    while (top >= 0 && is_hidden(g, pile, top)) top--;
    if (top < 0) return;
    rect_t r = slot_rect(g, pile);
    int layers = pile == P_STOCK ? (top >= 16 ? 3 : top >= 6 ? 2 : 1) : (top >= 12 ? 2 : 1);
    pile_shadow(g, s, r.x, r.y, g->cw + (layers - 1) * 2, g->ch + (layers - 1) * 2);
    for (int k = layers - 1; k > 0; k--) draw_card(g, s, pile == P_STOCK ? 0 : p->c[top - k], r.x + k * 2, r.y + k * 2);
    draw_card(g, s, p->c[top], r.x, r.y);
}

static void draw_board(sol_t *g, surface_t *s) {
    draw_felt(g, s);
    for (int p = 0; p < NPILES; p++) {
        if (rect_empty(rect_intersect(s->clip, grow(IS_TAB(p) ? rect_make(slot_rect(g, p).x, g->tab_y, g->cw, g->H)
                                                               : slot_rect(g, p), SHM))))
            continue;
        draw_slot(g, s, p);
        draw_pile(g, s, p);
    }
}

static rect_t hint_src_rect(sol_t *g) {
    if (g->hint.src == P_STOCK || g->hint.idx < 0) return slot_rect(g, g->hint.src);
    const pile_t *p = &g->st.p[g->hint.src];
    rect_t a = card_rect(g, g->hint.src, g->hint.idx), b = card_rect(g, g->hint.src, MAX(p->n - 1, g->hint.idx));
    return rect_union(a, b);
}

static rect_t hint_dst_rect(sol_t *g) {
    int d = g->hint.dst;
    if (d < 0) return rect_make(0, 0, 0, 0);
    const pile_t *p = &g->st.p[d];
    if (IS_TAB(d) && p->n > 0) return card_rect(g, d, p->n - 1);
    return slot_rect(g, d);
}

static void glow(sol_t *g, surface_t *s, rect_t r, int a) {
    if (rect_empty(r) || !rect_overlaps(grow(r, 8), s->clip)) return;
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, g->rad, color_alpha(COL_GLOW, a / 6));
    for (int i = 0; i < 6; i++) {
        int al = i < 2 ? a : a * (6 - i) / 5;
        gfx_rounded_rect(s, r.x - i, r.y - i, r.w + 2 * i, r.h + 2 * i, g->rad + i, color_alpha(COL_GLOW, al));
    }
}

static void draw_hint(sol_t *g, surface_t *s) {
    if (!g->hint.on) return;
    double ph = (sys_uptime_ms() - g->hint.t0) / 1000.0;
    int a = (int)(165 + 90 * sin(ph * 2 * M_PI * 1.4));
    glow(g, s, hint_src_rect(g), a);
    glow(g, s, hint_dst_rect(g), a);
}

static rect_t msg_rect(sol_t *g) {
    int w = font_text_width(FONT_UI, g->msg) + 36;
    return rect_make((g->W - w) / 2, g->H - 50, w, 30);
}

static void draw_msg(sol_t *g, surface_t *s) {
    if (!g->msg_on) return;
    rect_t r = msg_rect(g);
    if (!rect_overlaps(r, s->clip)) return;
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 15, RGBA(0, 0, 0, 175));
    gfx_text_aligned(s, r, g->msg, COLOR_WHITE, FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
}

/* ---- result card ---------------------------------------------------------------------------- */
static rect_t overlay_rect(sol_t *g) {
    int w = MIN(OV_W, g->W - 24), h = OV_H;
    int y = TB_H + (g->H - TB_H - h) / 2 + (int)((1 - ease_out(g->ov_p)) * 36);
    return rect_make((g->W - w) / 2, y, w, h);
}

static rect_t overlay_btn_rect(sol_t *g) {
    rect_t o = overlay_rect(g);
    return rect_make(o.x + (o.w - 140) / 2, o.y + o.h - 48, 140, 32);
}

static void draw_trophy(surface_t *s, double cx, double cy, double sz) {
    uint32_t gold = RGB(240, 186, 48), gold_d = RGB(196, 136, 26);
    double k = sz / 2;
    gfx_circle_aa(s, cx - 0.62 * k, cy - 0.38 * k, 0.26 * k, 0.11 * k, gold_d);
    gfx_circle_aa(s, cx + 0.62 * k, cy - 0.38 * k, 0.26 * k, 0.11 * k, gold_d);
    static const double cup[] = { -0.62, -0.78, 0.62, -0.78, 0.55, -0.25, 0.34, 0.05, 0.12, 0.18,
                                  -0.12, 0.18, -0.34, 0.05, -0.55, -0.25 };
    ss_poly(s, cup, 8, cx, cy, k, gold);
    ss_rect(s, -0.09, 0.16, 0.09, 0.52, cx, cy, k, gold_d);
    ss_rect(s, -0.42, 0.50, 0.42, 0.72, cx, cy, k, gold);
    ss_rect(s, -0.52, 0.70, 0.52, 0.86, cx, cy, k, gold_d);
    static const double shine[] = { -0.42, -0.68, -0.26, -0.68, -0.30, -0.22, -0.40, -0.30 };
    ss_poly(s, shine, 4, cx, cy, k, RGBA(255, 255, 255, 120));
}

static void draw_overlay(sol_t *g, surface_t *s) {
    if (!g->won || g->ov_p <= 0) return;
    rect_t o = overlay_rect(g);
    if (!rect_overlaps(grow(o, 20), s->clip)) return;
    const theme_t *t = theme_get();
    gfx_shadow(s, o.x, o.y, o.w, o.h, 14, 16, t->dark ? 160 : 110);
    gfx_fill_rounded_rect(s, o.x, o.y, o.w, o.h, 14, color_mix(t->panel_bg, t->window_border, 80));
    gfx_fill_rounded_rect(s, o.x + 1, o.y + 1, o.w - 2, o.h - 2, 13, t->panel_bg);
    gfx_fill_rounded_gradient_v(s, o.x + 1, o.y + 1, o.w - 2, 70, 13, color_alpha(t->accent, t->dark ? 60 : 40),
                                color_alpha(t->accent, 0));

    /* title row */
    const char *title = "You won!";
    int tw = font_text_width(FONT_UI_TITLE, title);
    int tx = o.x + (o.w - tw + 44) / 2;
    draw_trophy(s, tx - 26, o.y + 34, 38);
    gfx_text(s, tx, o.y + 16, title, t->window_text, FONT_UI_TITLE);

    /* three stat tiles */
    char v[3][24];
    const char *lab[3] = { "Score", "Time", "Moves" };
    snprintf(v[0], sizeof v[0], "%d", g->st.score);
    fmt_time(v[1], sizeof v[1], elapsed_sec(g));
    snprintf(v[2], sizeof v[2], "%d", g->st.moves);
    int tileW = (o.w - 48 - 16) / 3;
    for (int i = 0; i < 3; i++) {
        rect_t r = rect_make(o.x + 24 + i * (tileW + 8), o.y + 64, tileW, 54);
        bool rec = (i == 0 && g->rec_score) || (i == 1 && g->rec_time);
        uint32_t bc = rec ? color_mix(t->window_border, t->accent, 200) : t->window_border;
        gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 9, bc);
        gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 8,
                              t->dark ? color_mix(t->panel_bg, COLOR_WHITE, 10) : color_mix(t->panel_bg, t->window_bg, 128));
        gfx_text_aligned(s, rect_make(r.x, r.y + 5, r.w, 17), lab[i], muted(), FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
        gfx_text_aligned(s, rect_make(r.x, r.y + 22, r.w, 28), v[i], rec ? t->accent : t->window_text, FONT_UI_LARGE,
                         ALIGN_CENTER | ALIGN_MIDDLE);
    }

    /* records line */
    char line[128], bt[16];
    fmt_time(bt, sizeof bt, g->stats.best_time);
    if (g->rec_time && g->rec_score) snprintf(line, sizeof line, "New best score and best time!");
    else if (g->rec_time) snprintf(line, sizeof line, "New best time!");
    else if (g->rec_score) snprintf(line, sizeof line, "New best score!");
    else line[0] = 0;
    char line2[128];
    snprintf(line2, sizeof line2, "Games won: %d of %d   \x95   Best time %s", g->stats.won, g->stats.played, bt);
    int ly = o.y + 128;
    if (line[0]) {
        gfx_text_aligned(s, rect_make(o.x, ly, o.w, 18), line, t->accent, FONT_UI_BOLD, ALIGN_CENTER | ALIGN_MIDDLE);
        ly += 19;
    } else ly += 9;
    gfx_text_aligned(s, rect_make(o.x, ly, o.w, 18), line2, muted(), FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);

    int st = UI_STATE_DEFAULT;
    if (g->obtn.pressed && g->obtn.hover) st |= UI_STATE_PRESSED;
    else if (g->obtn.hover) st |= UI_STATE_HOVER;
    ui_draw_button(s, overlay_btn_rect(g), "Play Again", st);
}

/* ---- moving cards ---------------------------------------------------------------------------- */
/* Advance an animation to `now`: position and progress are stored so every
   paint until the next frame draws the card exactly where `last` says. */
static bool anim_update(sol_t *g, anim_t *a, uint32_t now) {
    int32_t el = (int32_t)(now - a->t0);
    if (el < 0) return false;
    double tt = a->dur ? (double)el / a->dur : 1.0;
    if (tt > 1) tt = 1;
    double e = ease_out(tt);
    rect_t d = card_rect(g, a->pile, a->idx);
    a->x = a->x0 + (int)lround((d.x - a->x0) * e);
    a->y = a->y0 + (int)lround((d.y - a->y0) * e);
    a->t = tt;
    a->live = true;
    return true;
}

static void draw_anims(sol_t *g, surface_t *s) {
    for (int i = 0; i < g->nanim; i++) {
        anim_t *a = &g->anim[i];
        if (!a->live) continue;
        int x = a->x, y = a->y;
        double t = a->t;
        if (!rect_overlaps(rect_make(x - SHM, y - SHM, g->cw + 2 * SHM, g->ch + 2 * SHM), s->clip)) continue;
        if (a->fly && t < 1) gfx_shadow(s, x, y + 3, g->cw, g->ch, g->rad, 9, 64);
        if (a->flip) {
            if (t < 0.5) draw_card_scaled(g, s, a->card & ~FACEUP, x, y, (int)((1 - 2 * t) * 256));
            else draw_card_scaled(g, s, a->card | FACEUP, x, y, (int)((2 * t - 1) * 256));
        } else {
            draw_card(g, s, a->card, x, y);
        }
    }
}

static rect_t drag_rect(sol_t *g) {
    int h = g->drag.dy[MAX(0, g->drag.n - 1)] + g->ch;
    return rect_make(g->drag.x, g->drag.y, g->cw, h);
}

static void draw_drag(sol_t *g, surface_t *s) {
    if (!g->drag.active) return;
    rect_t r = drag_rect(g);
    if (!rect_overlaps(grow(r, 16), s->clip)) return;
    gfx_shadow(s, r.x, r.y + 5, r.w, r.h, g->rad, 12, 80);
    const pile_t *p = &g->st.p[g->drag.pile];
    for (int k = 0; k < g->drag.n; k++) draw_card(g, s, p->c[g->drag.idx + k], g->drag.x, g->drag.y + g->drag.dy[k]);
}

static void draw_all(sol_t *g, surface_t *s) {
    if (s->clip.y < TB_H) draw_toolbar(g, s);
    if (s->clip.y + s->clip.h > TB_H) {
        if (g->cele.on && g->cele.trail) {
            rect_t r = rect_intersect(s->clip, felt_rect(g));
            if (!rect_empty(r)) gfx_blit_rect(s, r.x, r.y, g->cele.trail, r.x, r.y, r.w, r.h);
        } else {
            draw_board(g, s);
            draw_hint(g, s);
        }
        draw_msg(g, s);
        draw_overlay(g, s);
    }
    draw_drag(g, s);
    draw_anims(g, s);
}

/* ---- timer ------------------------------------------------------------------------------- */
static void update_timer(sol_t *g) {
    int want = 0;
    if (g->nanim || g->autoplay || g->hint.on || (g->won && !g->cele.on) || (g->cele.on && !g->cele.done) ||
        (g->won && g->ov_p < 1))
        want = 16;
    else if (g->msg_on) want = 100;
    else if (g->started && !g->won) want = 500;
    if (want == g->timer_iv) return;
    if (g->timer) win_kill_timer(g->win, g->timer);
    g->timer = want ? win_set_timer(g->win, (uint32_t)want) : 0;
    g->timer_iv = g->timer ? want : 0;
}

static void add_anim(sol_t *g, int pile, int idx, uint8_t card, int x0, int y0, uint32_t dur, bool flip, uint32_t delay) {
    if (g->nanim >= MAX_ANIM) return;   /* the card simply appears in place */
    anim_t *a = &g->anim[g->nanim++];
    a->pile = pile;
    a->idx = idx;
    a->card = card;
    a->x0 = x0;
    a->y0 = y0;
    a->t0 = sys_uptime_ms() + delay;
    a->dur = dur;
    a->flip = flip;
    a->x = x0;
    a->y = y0;
    a->t = 0;
    a->live = delay == 0;
    rect_t d = card_rect(g, pile, idx);
    a->fly = d.x != x0 || d.y != y0;
    a->last = a->live ? grow(rect_make(x0, y0, g->cw, g->ch), SHM) : rect_make(0, 0, 0, 0);
    dirty_add(g, a->last);
    update_timer(g);
}

static void finish_anims(sol_t *g) {
    for (int i = 0; i < g->nanim; i++) {
        dirty_add(g, g->anim[i].last);
        dirty_card(g, card_rect(g, g->anim[i].pile, g->anim[i].idx));
        if (IS_TAB(g->anim[i].pile)) dirty_pile(g, g->anim[i].pile);
    }
    g->nanim = 0;
}

/* ---- game logic ------------------------------------------------------------------------------ */
static void show_msg(sol_t *g, const char *text) {
    if (g->msg_on) dirty_add(g, grow(msg_rect(g), 2));
    strlcpy(g->msg, text, sizeof g->msg);
    g->msg_on = true;
    g->msg_t0 = sys_uptime_ms();
    dirty_add(g, grow(msg_rect(g), 2));
    update_timer(g);
}

static void hint_off(sol_t *g) {
    if (!g->hint.on) return;
    dirty_add(g, grow(hint_src_rect(g), 9));
    dirty_add(g, grow(hint_dst_rect(g), 9));
    g->hint.on = false;
}

static bool can_place(const sol_t *g, int dst, uint8_t c, int n) {
    const pile_t *d = &g->st.p[dst];
    if (IS_FOUND(dst)) {
        if (n != 1) return false;
        if (d->n == 0) return RANK(c) == 1;
        uint8_t t = d->c[d->n - 1];
        return SUIT(t) == SUIT(c) && RANK(c) == RANK(t) + 1;
    }
    if (IS_TAB(dst)) {
        if (d->n == 0) return RANK(c) == 13;
        uint8_t t = d->c[d->n - 1];
        return ISUP(t) && ISRED(t) != ISRED(c) && RANK(c) + 1 == RANK(t);
    }
    return false;
}

static void push_undo(sol_t *g) {
    if (!g->undo) return;
    g->undo[g->undo_head] = g->st;
    g->undo_head = (g->undo_head + 1) % UNDO_MAX;
    if (g->undo_n < UNDO_MAX) g->undo_n++;
}

static void start_clock(sol_t *g) {
    if (g->started) return;
    g->started = true;
    g->start_ms = sys_uptime_ms();
    g->shown_sec = 0;
    if (!g->counted) {
        g->counted = true;
        stats_t st;
        stats_load(&st);
        st.played++;
        stats_save(&st);
    }
}

static void check_win(sol_t *g) {
    for (int f = 0; f < 4; f++)
        if (g->st.p[P_FOUND + f].n != 13) return;
    g->won = true;
    g->autoplay = false;
    g->end_ms = sys_uptime_ms();
    g->ov_p = 0;
    int secs = elapsed_sec(g);
    stats_t st;
    stats_load(&st);
    if (st.played < st.won + 1) st.played = st.won + 1;
    st.won++;
    g->rec_score = g->st.score > st.best_score;
    g->rec_time = st.best_time <= 0 || secs < st.best_time;
    if (g->rec_score) st.best_score = g->st.score;
    if (g->rec_time) st.best_time = secs;
    stats_save(&st);
    g->stats = st;
    static const note_t fanfare[] = { { 523, 90 }, { 659, 90 }, { 784, 90 }, { 1047, 220 }, { 0, 60 }, { 784, 90 }, { 1047, 300 } };
    sys_play_notes(fanfare, ARRAY_SIZE(fanfare));
    klog("solitaire: won in %d s, %d moves, score %d", secs, g->st.moves, g->st.score);
    dirty_toolbar(g);
    update_timer(g);
}

/* Move cards idx.. of pile src onto dst (legality already checked). Turns over
   a newly exposed tableau card. The caller animates the moved cards. */
static void do_move(sol_t *g, int src, int idx, int dst) {
    push_undo(g);
    pile_t *ps = &g->st.p[src], *pd = &g->st.p[dst];
    int n = ps->n - idx;
    dirty_pile(g, src);
    dirty_pile(g, dst);
    for (int k = 0; k < n; k++) pd->c[pd->n++] = ps->c[idx + k] | FACEUP;
    ps->n = (uint8_t)idx;
    if (src == P_WASTE && IS_TAB(dst)) g->st.score += 5;
    if ((src == P_WASTE || IS_TAB(src)) && IS_FOUND(dst)) g->st.score += 10;
    if (IS_FOUND(src) && IS_TAB(dst)) g->st.score -= 15;
    if (g->st.score < 0) g->st.score = 0;
    g->st.moves++;
    start_clock(g);
    if (IS_TAB(src) && ps->n > 0 && !ISUP(ps->c[ps->n - 1])) {
        ps->c[ps->n - 1] |= FACEUP;
        g->st.score += 5;
        rect_t r = card_rect(g, src, ps->n - 1);
        add_anim(g, src, ps->n - 1, ps->c[ps->n - 1], r.x, r.y, 230, true, 0);
    }
    dirty_pile(g, src);
    dirty_pile(g, dst);
    dirty_toolbar(g);
    check_win(g);
}

static void move_animated(sol_t *g, int src, int idx, int dst, uint32_t dur) {
    int n = g->st.p[src].n - idx, dn0 = g->st.p[dst].n;
    int fx[52], fy[52];
    uint8_t cards[52];
    for (int k = 0; k < n; k++) {
        rect_t r = card_rect(g, src, idx + k);
        fx[k] = r.x;
        fy[k] = r.y;
        cards[k] = g->st.p[src].c[idx + k] | FACEUP;
    }
    do_move(g, src, idx, dst);
    for (int k = 0; k < n; k++) add_anim(g, dst, dn0 + k, cards[k], fx[k], fy[k], dur, false, 0);
}

/* Stock and waste empty, every tableau card face up: nothing can go wrong any more. */
static bool trivially_won(const sol_t *g) {
    if (g->st.p[P_STOCK].n || g->st.p[P_WASTE].n) return false;
    for (int p = P_TAB; p < NPILES; p++)
        for (int i = 0; i < g->st.p[p].n; i++)
            if (!ISUP(g->st.p[p].c[i])) return false;
    return true;
}

static void after_user_move(sol_t *g) {
    if (!g->won && !g->autoplay && trivially_won(g)) {
        g->autoplay = true;
        g->auto_next = sys_uptime_ms() + 300;
        show_msg(g, "Finishing the game...");
        update_timer(g);
    }
}

/* One card to a foundation (the lowest rank that can go). */
static bool auto_step(sol_t *g) {
    int best_p = -1, best_f = -1, best_rank = 99;
    for (int p = P_WASTE; p < NPILES; p++) {
        if (IS_FOUND(p)) continue;
        const pile_t *pp = &g->st.p[p];
        if (!pp->n) continue;
        uint8_t c = pp->c[pp->n - 1];
        if (!ISUP(c) || RANK(c) >= best_rank) continue;
        for (int f = P_FOUND; f < P_TAB; f++)
            if (can_place(g, f, c, 1)) {
                best_p = p;
                best_f = f;
                best_rank = RANK(c);
                break;
            }
    }
    if (best_p < 0) return false;
    move_animated(g, best_p, g->st.p[best_p].n - 1, best_f, 200);
    return true;
}

static void start_autoplay(sol_t *g) {
    if (g->won) return;
    finish_anims(g);
    hint_off(g);
    if (!auto_step(g)) {
        show_msg(g, "No card can go to the foundations right now");
        return;
    }
    g->autoplay = true;
    g->auto_next = sys_uptime_ms() + 90;
    update_timer(g);
}

static void stock_click(sol_t *g) {
    pile_t *s = &g->st.p[P_STOCK], *w = &g->st.p[P_WASTE];
    if (s->n > 0) {
        push_undo(g);
        uint8_t c = (uint8_t)(s->c[--s->n] | FACEUP);
        w->c[w->n++] = c;
        g->st.moves++;
        start_clock(g);
        rect_t sr = slot_rect(g, P_STOCK);
        add_anim(g, P_WASTE, w->n - 1, c, sr.x, sr.y, 180, true, 0);
    } else if (w->n > 0) {
        push_undo(g);
        while (w->n) s->c[s->n++] = w->c[--w->n] & (uint8_t)~FACEUP;
        g->st.score = MAX(0, g->st.score - 100);
        g->st.moves++;
        start_clock(g);
    } else {
        return;
    }
    dirty_pile(g, P_STOCK);
    dirty_pile(g, P_WASTE);
    dirty_toolbar(g);
}

static void stop_cele(sol_t *g) {
    free_surf(&g->cele.trail);
    memset(&g->cele, 0, sizeof g->cele);
}

static void cancel_drag(sol_t *g, bool fly_back) {
    if (g->drag.active) {
        dirty_card(g, drag_rect(g));
        if (fly_back) {
            g->drag.active = false;
            const pile_t *p = &g->st.p[g->drag.pile];
            for (int k = 0; k < g->drag.n; k++)
                add_anim(g, g->drag.pile, g->drag.idx + k, p->c[g->drag.idx + k], g->drag.x,
                         g->drag.y + g->drag.dy[k], 200, false, 0);
        }
        dirty_pile(g, g->drag.pile);
    }
    g->drag.active = g->drag.pending = false;
}

static void new_game(sol_t *g) {
    finish_anims(g);
    cancel_drag(g, false);
    stop_cele(g);
    g->hint.on = false;
    g->msg_on = false;
    g->autoplay = false;
    g->won = g->started = g->counted = false;
    g->rec_time = g->rec_score = false;
    g->ov_p = 0;
    g->obtn.hover = g->obtn.pressed = false;
    g->undo_n = g->undo_head = 0;
    memset(&g->st, 0, sizeof g->st);

    uint8_t deck[52];
    for (int i = 0; i < 52; i++) deck[i] = (uint8_t)i;
    for (int i = 51; i > 0; i--) {
        int j = rnd_n(g, i + 1);
        uint8_t t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
    if (g->test_almost) {
        /* test layout: the foundations hold A..Q, the kings wait on the tableau */
        g->test_almost = false;
        for (int su = 0; su < 4; su++) {
            pile_t *f = &g->st.p[P_FOUND + su], *t = &g->st.p[P_TAB + su];
            for (int r = 0; r < 12; r++) f->c[f->n++] = (uint8_t)((su * 13 + r) | FACEUP);
            t->c[t->n++] = (uint8_t)((su * 13 + 12) | FACEUP);
        }
        win_invalidate(g->win);
        update_timer(g);
        return;
    }
    int k = 0;
    rect_t sr = slot_rect(g, P_STOCK);
    for (int row = 0; row < 7; row++)
        for (int c = row; c < 7; c++) {
            pile_t *p = &g->st.p[P_TAB + c];
            uint8_t card = (uint8_t)(deck[k] | (row == c ? FACEUP : 0));
            p->c[p->n++] = card;
            add_anim(g, P_TAB + c, row, card, sr.x, sr.y, 240, row == c, (uint32_t)(k * 26));
            k++;
        }
    pile_t *st = &g->st.p[P_STOCK];
    while (k < 52) st->c[st->n++] = deck[k++];
    sys_beep(330, 20);
    win_invalidate(g->win);
    update_timer(g);
}

static void undo(sol_t *g) {
    if (g->won || g->undo_n == 0) {
        sys_beep(200, 30);
        return;
    }
    finish_anims(g);
    cancel_drag(g, false);
    hint_off(g);
    g->autoplay = false;
    g->undo_head = (g->undo_head + UNDO_MAX - 1) % UNDO_MAX;
    g->undo_n--;
    g->st = g->undo[g->undo_head];
    win_invalidate(g->win);
}

/* Find a useful move for the hint. */
static bool find_hint(sol_t *g, int *src, int *idx, int *dst) {
    /* 1: anything to a foundation */
    for (int p = P_WASTE; p < NPILES; p++) {
        if (IS_FOUND(p)) continue;
        const pile_t *pp = &g->st.p[p];
        if (!pp->n || !ISUP(pp->c[pp->n - 1])) continue;
        for (int f = P_FOUND; f < P_TAB; f++)
            if (can_place(g, f, pp->c[pp->n - 1], 1)) { *src = p; *idx = pp->n - 1; *dst = f; return true; }
    }
    /* 2: a tableau run that uncovers a face-down card (or frees a column for a king) */
    for (int p = P_TAB; p < NPILES; p++) {
        const pile_t *pp = &g->st.p[p];
        int i = 0;
        while (i < pp->n && !ISUP(pp->c[i])) i++;
        if (i >= pp->n || i == 0) continue;
        for (int d = P_TAB; d < NPILES; d++)
            if (d != p && can_place(g, d, pp->c[i], pp->n - i)) { *src = p; *idx = i; *dst = d; return true; }
    }
    /* 3: waste to tableau */
    const pile_t *w = &g->st.p[P_WASTE];
    if (w->n)
        for (int d = P_TAB; d < NPILES; d++)
            if (can_place(g, d, w->c[w->n - 1], 1)) { *src = P_WASTE; *idx = w->n - 1; *dst = d; return true; }
    /* 4: turn a card */
    if (g->st.p[P_STOCK].n || w->n) { *src = P_STOCK; *idx = -1; *dst = -1; return true; }
    return false;
}

static void do_hint(sol_t *g) {
    if (g->won) return;
    finish_anims(g);
    hint_off(g);
    int s, i, d;
    if (!find_hint(g, &s, &i, &d)) {
        show_msg(g, "No more moves. Undo, or start a new game.");
        return;
    }
    g->hint.on = true;
    g->hint.src = s;
    g->hint.idx = i;
    g->hint.dst = d;
    g->hint.t0 = sys_uptime_ms();
    if (s == P_STOCK) show_msg(g, g->st.p[P_STOCK].n ? "Turn a card from the stock" : "Turn the waste over");
    update_timer(g);
}

/* Double-click: a single card to its foundation, else (or for a run) to a tableau column. */
static bool smart_move(sol_t *g, int pile, int idx) {
    const pile_t *p = &g->st.p[pile];
    if (idx < 0 || idx >= p->n || !ISUP(p->c[idx])) return false;
    uint8_t c = p->c[idx];
    int n = p->n - idx, dst = -1;
    if (n == 1)
        for (int f = P_FOUND; f < P_TAB && dst < 0; f++)
            if (can_place(g, f, c, 1)) dst = f;
    if (dst < 0 && !IS_FOUND(pile))
        for (int d = P_TAB; d < NPILES && dst < 0; d++)
            if (d != pile && g->st.p[d].n && can_place(g, d, c, n)) dst = d;
    if (dst < 0 && RANK(c) == 13 && !(IS_TAB(pile) && idx == 0))
        for (int d = P_TAB; d < NPILES && dst < 0; d++)
            if (!g->st.p[d].n) dst = d;
    if (dst < 0) return false;
    move_animated(g, pile, idx, dst, 210);
    return true;
}

/* ---- win cascade -------------------------------------------------------------------------------- */
static void render_trail(sol_t *g) {
    free_surf(&g->cele.trail);
    g->cele.trail = surface_create(g->W, g->H);
    if (g->cele.trail) draw_board(g, g->cele.trail);
}

static void start_cele(sol_t *g) {
    memset(&g->cele, 0, sizeof g->cele);
    g->cele.on = true;
    for (int f = 0; f < 4; f++) g->cele.fcount[f] = g->st.p[P_FOUND + f].n;
    render_trail(g);
    g->cele.t0 = g->cele.last = sys_uptime_ms();
    hint_off(g);
    win_invalidate(g->win);
}

static void cele_step(sol_t *g) {
    surface_t *tr = g->cele.trail;
    if (!tr) { g->cele.done = true; return; }
    if (!g->cele.flying) {
        int f = -1;
        for (int k = 0; k < 4; k++) {
            int ff = (g->cele.f + k) % 4;
            if (g->cele.fcount[ff] > 0) { f = ff; break; }
        }
        if (f < 0) { g->cele.done = true; return; }
        g->cele.f = (f + 1) % 4;
        const pile_t *p = &g->st.p[P_FOUND + f];
        g->cele.card = p->c[--g->cele.fcount[f]] | FACEUP;
        rect_t sr = slot_rect(g, P_FOUND + f);
        /* show the card underneath on the foundation */
        surface_set_clip(tr, grow(sr, 3));
        draw_felt(g, tr);
        draw_slot(g, tr, P_FOUND + f);
        if (g->cele.fcount[f] > 0) draw_card(g, tr, p->c[g->cele.fcount[f] - 1], sr.x, sr.y);
        surface_reset_clip(tr);
        dirty_add(g, grow(sr, 3));
        g->cele.x = sr.x;
        g->cele.y = sr.y;
        g->cele.vx = (rnd_n(g, 2) ? 1 : -1) * (3.0 + rnd_n(g, 50) / 10.0);
        g->cele.vy = -(1.0 + rnd_n(g, 70) / 10.0);
        g->cele.flying = true;
    }
    g->cele.vy += 0.42;
    g->cele.x += g->cele.vx;
    g->cele.y += g->cele.vy;
    double floor_y = g->H - g->ch;
    if (g->cele.y > floor_y) {
        g->cele.y = floor_y;
        g->cele.vy = -g->cele.vy * 0.80;
        if (g->cele.vy > -1.2) g->cele.vy = 0;
    }
    int x = (int)g->cele.x, y = (int)g->cele.y;
    surface_set_clip(tr, felt_rect(g));
    draw_card(g, tr, g->cele.card, x, y);
    surface_reset_clip(tr);
    dirty_add(g, rect_make(x, y, g->cw, g->ch));
    if (x + g->cw < 0 || x > g->W) g->cele.flying = false;
}

static void cele_tick(sol_t *g, uint32_t now) {
    if (!g->cele.on) return;
    if (!g->cele.done) {
        g->cele.acc += (int)(now - g->cele.last);
        g->cele.last = now;
        int steps = 0;
        while (g->cele.acc >= 9 && steps < 8 && !g->cele.done) {
            g->cele.acc -= 9;
            steps++;
            cele_step(g);
        }
        if (g->cele.acc > 200) g->cele.acc = 0;
    }
    if (g->ov_p < 1) {
        double p = ((int32_t)(now - g->cele.t0) - 650) / 320.0;
        p = CLAMP(p, 0.0, 1.0);
        if (p != g->ov_p) {
            rect_t a = grow(overlay_rect(g), 20);
            g->ov_p = p;
            dirty_add(g, rect_union(a, grow(overlay_rect(g), 20)));
        }
    }
}

/* ---- per-frame work ------------------------------------------------------------------------ */
static void tick(sol_t *g) {
    uint32_t now = sys_uptime_ms();
    for (int i = 0; i < g->nanim; i++) {
        anim_t *a = &g->anim[i];
        if (!anim_update(g, a, now)) continue;
        rect_t cur = grow(rect_make(a->x, a->y, g->cw, g->ch), SHM);
        dirty_add(g, a->last);
        dirty_add(g, cur);
        a->last = cur;
        if (a->t >= 1) {
            dirty_card(g, card_rect(g, a->pile, a->idx));
            g->anim[i] = g->anim[--g->nanim];
            i--;
        }
    }
    if (g->autoplay && (int32_t)(now - g->auto_next) >= 0) {
        if (auto_step(g)) g->auto_next = now + 90;
        else g->autoplay = false;
    }
    if (g->hint.on) {
        if (now - g->hint.t0 > 2400) hint_off(g);
        else {
            dirty_add(g, grow(hint_src_rect(g), 9));
            dirty_add(g, grow(hint_dst_rect(g), 9));
        }
    }
    if (g->msg_on && now - g->msg_t0 > 2600) {
        dirty_add(g, grow(msg_rect(g), 2));
        g->msg_on = false;
    }
    if (g->won && !g->cele.on && g->nanim == 0) start_cele(g);
    cele_tick(g, now);
    if (g->started && !g->won) {
        int sec = elapsed_sec(g);
        if (sec != g->shown_sec) {
            g->shown_sec = sec;
            dirty_toolbar(g);
        }
    }
    update_timer(g);
}

/* ---- hit testing ------------------------------------------------------------------------------ */
static bool hit_card(sol_t *g, int x, int y, int *pile, int *idx) {
    for (int p = P_WASTE; p < P_TAB; p++) {
        int n = g->st.p[p].n;
        if (n && rect_contains(slot_rect(g, p), x, y)) { *pile = p; *idx = n - 1; return true; }
    }
    for (int p = P_TAB; p < NPILES; p++) {
        int n = g->st.p[p].n;
        for (int i = n - 1; i >= 0; i--)
            if (rect_contains(card_rect(g, p, i), x, y)) { *pile = p; *idx = i; return true; }
    }
    return false;
}

static void set_cursor(sol_t *g, int c) {
    if (g->cursor == c) return;
    g->cursor = c;
    win_set_cursor(g->win, c);
}

static void update_buttons(sol_t *g) {
    bool d[NBTN] = { false, g->won || g->undo_n == 0, g->won, g->won };
    for (int i = 0; i < NBTN; i++)
        if (g->btn[i].disabled != d[i]) {
            g->btn[i].disabled = d[i];
            if (d[i]) g->btn[i].hover = g->btn[i].pressed = false;
            dirty_toolbar(g);
        }
}

static void button_action(sol_t *g, int i) {
    switch (i) {
    case BTN_NEW: new_game(g); break;
    case BTN_UNDO: undo(g); break;
    case BTN_HINT: do_hint(g); break;
    case BTN_AUTO: start_autoplay(g); break;
    }
}

static void drop(sol_t *g) {
    rect_t dr = rect_make(g->drag.x, g->drag.y, g->cw, g->ch);
    int src = g->drag.pile, idx = g->drag.idx, n = g->drag.n;
    uint8_t first = g->st.p[src].c[idx];
    int best = -1, best_area = 0;
    for (int p = P_FOUND; p < NPILES; p++) {
        if (p == src) continue;
        const pile_t *pp = &g->st.p[p];
        rect_t tr = slot_rect(g, p);
        if (IS_TAB(p) && pp->n) tr = rect_union(card_rect(g, p, pp->n - 1), card_rect(g, p, pp->n));
        rect_t in = rect_intersect(dr, tr);
        int area = in.w * in.h;
        if (area > best_area && can_place(g, p, first, n)) {
            best = p;
            best_area = area;
        }
    }
    int fx = g->drag.x, fy = g->drag.y;
    int16_t dy[52];
    memcpy(dy, g->drag.dy, sizeof dy);
    dirty_card(g, drag_rect(g));
    g->drag.active = g->drag.pending = false;
    if (best >= 0) {
        int dn0 = g->st.p[best].n;
        uint8_t cards[52];
        for (int k = 0; k < n; k++) cards[k] = g->st.p[src].c[idx + k] | FACEUP;
        do_move(g, src, idx, best);
        for (int k = 0; k < n; k++) add_anim(g, best, dn0 + k, cards[k], fx, fy + dy[k], 110, false, 0);
        sys_beep(1400, 4);
        after_user_move(g);
    } else {
        const pile_t *p = &g->st.p[src];
        for (int k = 0; k < n; k++) add_anim(g, src, idx + k, p->c[idx + k], fx, fy + dy[k], 220, false, 0);
        dirty_pile(g, src);
    }
}

static void game_mouse_down(sol_t *g, int x, int y) {
    finish_anims(g);
    g->autoplay = false;
    hint_off(g);
    if (rect_contains(slot_rect(g, P_STOCK), x, y)) {
        stock_click(g);
        return;
    }
    int pile, idx;
    if (!hit_card(g, x, y, &pile, &idx)) return;
    const pile_t *p = &g->st.p[pile];
    if (!ISUP(p->c[idx])) {
        if (IS_TAB(pile) && idx == p->n - 1) {   /* (only after an undo of a turn-over) */
            push_undo(g);
            g->st.p[pile].c[idx] |= FACEUP;
            g->st.score += 5;
            rect_t r = card_rect(g, pile, idx);
            add_anim(g, pile, idx, g->st.p[pile].c[idx], r.x, r.y, 230, true, 0);
            dirty_toolbar(g);
        }
        return;
    }
    rect_t r = card_rect(g, pile, idx);
    g->drag.pending = true;
    g->drag.active = false;
    g->drag.pile = pile;
    g->drag.idx = idx;
    g->drag.n = p->n - idx;
    g->drag.mx0 = x;
    g->drag.my0 = y;
    g->drag.gx = x - r.x;
    g->drag.gy = y - r.y;
    g->drag.x = r.x;
    g->drag.y = r.y;
    for (int k = 0; k < g->drag.n; k++) g->drag.dy[k] = (int16_t)(card_rect(g, pile, idx + k).y - r.y);
}

static void hover_cursor(sol_t *g, int x, int y) {
    int c = CURSOR_ARROW, pile, idx;
    if (!g->won && y >= TB_H) {
        if (rect_contains(slot_rect(g, P_STOCK), x, y) && (g->st.p[P_STOCK].n || g->st.p[P_WASTE].n)) c = CURSOR_HAND;
        else if (hit_card(g, x, y, &pile, &idx) && ISUP(g->st.p[pile].c[idx])) c = CURSOR_HAND;
    }
    set_cursor(g, c);
}

static void mouse_move(sol_t *g, const event_t *ev) {
    int x = ev->x, y = ev->y;
    for (int i = 0; i < NBTN; i++) {
        sbtn_t *b = &g->btn[i];
        bool in = !b->disabled && rect_contains(b->r, x, y);
        if (in != b->hover) { b->hover = in; dirty_toolbar(g); }
    }
    if (g->won && g->ov_p >= 1) {
        bool in = rect_contains(overlay_btn_rect(g), x, y);
        if (in != g->obtn.hover) { g->obtn.hover = in; dirty_add(g, grow(overlay_btn_rect(g), 4)); }
    }
    if (g->drag.pending && !g->drag.active && (abs(x - g->drag.mx0) > 3 || abs(y - g->drag.my0) > 3)) {
        g->drag.active = true;
        dirty_card(g, drag_rect(g));
    }
    if (g->drag.active) {
        dirty_card(g, drag_rect(g));
        g->drag.x = x - g->drag.gx;
        g->drag.y = y - g->drag.gy;
        dirty_card(g, drag_rect(g));
        set_cursor(g, CURSOR_MOVE);
        return;
    }
    if (ev->buttons) return;
    hover_cursor(g, x, y);
}

static void mouse_down(sol_t *g, const event_t *ev) {
    for (int i = 0; i < NBTN; i++) {
        sbtn_t *b = &g->btn[i];
        if (!b->disabled && rect_contains(b->r, ev->x, ev->y)) {
            if (ev->button == MOUSE_LEFT) { b->pressed = b->hover = true; dirty_toolbar(g); }
            return;
        }
    }
    if (ev->y < TB_H) return;
    if (g->won) {
        if (g->ov_p >= 1 && ev->button == MOUSE_LEFT && rect_contains(overlay_btn_rect(g), ev->x, ev->y)) {
            g->obtn.pressed = g->obtn.hover = true;
            dirty_add(g, grow(overlay_btn_rect(g), 4));
        }
        return;
    }
    if (ev->button == MOUSE_RIGHT) {
        cancel_drag(g, true);
        start_autoplay(g);
        return;
    }
    if (ev->button == MOUSE_LEFT) game_mouse_down(g, ev->x, ev->y);
}

static void mouse_up(sol_t *g, const event_t *ev) {
    if (ev->button != MOUSE_LEFT) return;
    for (int i = 0; i < NBTN; i++) {
        sbtn_t *b = &g->btn[i];
        if (b->pressed) {
            b->pressed = false;
            dirty_toolbar(g);
            if (rect_contains(b->r, ev->x, ev->y) && !b->disabled) button_action(g, i);
            return;
        }
    }
    if (g->obtn.pressed) {
        g->obtn.pressed = false;
        dirty_add(g, grow(overlay_btn_rect(g), 4));
        if (g->won && rect_contains(overlay_btn_rect(g), ev->x, ev->y)) new_game(g);
        return;
    }
    if (g->drag.active) drop(g);
    g->drag.pending = false;
    hover_cursor(g, ev->x, ev->y);
}

static void double_click(sol_t *g, const event_t *ev) {
    if (ev->button != MOUSE_LEFT || ev->y < TB_H || g->won) return;
    if (g->drag.active) cancel_drag(g, false);
    g->drag.pending = false;
    int pile, idx;
    if (!hit_card(g, ev->x, ev->y, &pile, &idx) || IS_FOUND(pile)) return;
    finish_anims(g);
    if (smart_move(g, pile, idx)) after_user_move(g);
}

static void key_down(sol_t *g, const event_t *ev) {
    bool ctrl = ev->mods & MOD_CTRL;
    if (ev->key == KEY_F2 || (ctrl && ev->key == 'n')) { new_game(g); return; }
    if (g->won) {
        if (ev->key == KEY_ENTER || ev->key == ' ' || ev->key == 'n') new_game(g);
        return;
    }
    if ((ctrl && ev->key == 'z') || ev->key == KEY_BACKSPACE) { undo(g); return; }
    if (ctrl) return;
    switch (ev->key) {
    case ' ':
    case 'd':
        finish_anims(g);
        hint_off(g);
        g->autoplay = false;
        cancel_drag(g, true);
        stock_click(g);
        break;
    case 'h': do_hint(g); break;
    case 'a': start_autoplay(g); break;
    case KEY_ESCAPE: cancel_drag(g, true); break;
    default: break;
    }
}

/* ---- window --------------------------------------------------------------------------------- */
static void free_all(sol_t *g) {
    free_card_cache(g);
    free_surf(&g->felt);
    free_surf(&g->cele.trail);
    free(g->undo);
    free(g);
}

static void sol_event(window_t *win, const event_t *ev) {
    sol_t *g = win_get_userdata(win);
    if (!g) return;
    switch (ev->type) {
    case EV_PAINT: {
        surface_t *s = win_surface(win);
        draw_all(g, s);
        return;
    }
    case EV_RESIZE: {
        finish_anims(g);
        cancel_drag(g, false);
        layout(g);
        if (g->cw != g->cache_cw || g->ch != g->cache_ch) free_card_cache(g);
        free_surf(&g->felt);
        if (g->cele.on) render_trail(g);
        g->ndirty = 0;
        win_invalidate(win);
        return;
    }
    case EV_THEME:
        free_surf(&g->back);
        free_surf(&g->felt);
        if (g->cele.on) render_trail(g);
        win_invalidate(win);
        return;
    case EV_MOUSE_MOVE: mouse_move(g, ev); break;
    case EV_MOUSE_DOWN: mouse_down(g, ev); break;
    case EV_MOUSE_UP: mouse_up(g, ev); break;
    case EV_DOUBLE_CLICK: double_click(g, ev); break;
    case EV_MOUSE_LEAVE:
        for (int i = 0; i < NBTN; i++)
            if (g->btn[i].hover && !g->btn[i].pressed) { g->btn[i].hover = false; dirty_toolbar(g); }
        if (g->obtn.hover && !g->obtn.pressed) { g->obtn.hover = false; dirty_add(g, grow(overlay_btn_rect(g), 4)); }
        break;
    case EV_BLUR:
        if (g->drag.active) cancel_drag(g, true);
        g->drag.pending = false;
        break;
    case EV_KEY_DOWN: key_down(g, ev); break;
    case EV_TIMER: tick(g); break;
    case EV_CLOSE:
        win_set_userdata(win, NULL);
        free_all(g);
        return;
    default:
        return;
    }
    if (!win_is_valid(win) || win_get_userdata(win) != g) return;
    update_buttons(g);
    update_timer(g);
    dirty_flush(g);
}

/* Launch argument (tests): "almost" deals a game one move from a win,
   "seed:N" fixes the shuffle. */
static void sol_launch(const char *arg) {
    sol_t *g = calloc(1, sizeof *g);
    if (!g) return;
    g->undo = malloc(sizeof(gstate_t) * UNDO_MAX);
    window_t *win = win_create("Solitaire", SOL_W, SOL_H, WIN_CENTER | WIN_RESIZABLE);
    if (!win) {
        free(g->undo);
        free(g);
        return;
    }
    g->win = win;
    win_set_min_size(win, SOL_MIN_W, SOL_MIN_H);
    g->rng = (sys_uptime_ms() + 77u) * 2654435761u ^ sys_time() * 2246822519u ^ ((uint32_t)win_id(win) + 1u) * 3266489917u;
    if (arg && !strncmp(arg, "seed:", 5)) g->rng = (uint32_t)strtoul(arg + 5, NULL, 10) * 2654435761u + 12345u;
    if (arg && !strcmp(arg, "almost")) g->test_almost = true;
    if (!g->rng) g->rng = 0x9E3779B9u;
    g->cursor = -1;
    heart_init();
    layout(g);
    win_set_userdata(win, g);
    win_set_event_handler(win, sol_event);
    new_game(g);
    update_buttons(g);
    klog("solitaire: window %d, cards %dx%d", win_id(win), g->cw, g->ch);
}

/* ---- icon: two fanned cards, a blue back behind a white card with a heart ----------------
 * Built from polygons (fast integer scanline fills) with anti-aliased edges
 * drawn over their outlines; the icon is rendered at 2x and downsampled by
 * the system, which smooths the rest. */
#define IC_MAXP 64

typedef struct { double cx, cy, ca, sa; } xform_t;

static void xf_apply(const xform_t *t, double u, double v, double *x, double *y) {
    *x = t->cx + u * t->ca - v * t->sa;
    *y = t->cy + u * t->sa + v * t->ca;
}

static void poly_aa(surface_t *s, const double *p, int n, uint32_t col, bool outline) {
    point_t pts[IC_MAXP] = { { 0, 0 } };
    if (n < 3) return;
    if (n > IC_MAXP) n = IC_MAXP;
    for (int i = 0; i < n; i++) {
        pts[i].x = (int)lround(p[2 * i]);
        pts[i].y = (int)lround(p[2 * i + 1]);
    }
    gfx_fill_polygon(s, pts, n, col);
    if (outline)
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            gfx_line_aa(s, p[2 * i], p[2 * i + 1], p[2 * j], p[2 * j + 1], col);
        }
}

/* Rounded rect (half sizes hw, hh, radius r, local units = pixels) under a transform. */
static int rrect_poly(const xform_t *t, double hw, double hh, double r, double *out) {
    static const int sx[4] = { 1, -1, -1, 1 }, sy[4] = { 1, 1, -1, -1 };
    int n = 0;
    r = MAX(0.0, MIN(r, MIN(hw, hh)));
    for (int c = 0; c < 4; c++) {
        double ccx = sx[c] * (hw - r), ccy = sy[c] * (hh - r);
        for (int i = 0; i <= 5; i++) {
            double a = (c * 90 + i * 18) * M_PI / 180;
            xf_apply(t, ccx + r * cos(a), ccy + r * sin(a), &out[2 * n], &out[2 * n + 1]);
            n++;
        }
    }
    return n;
}

/* Heart outline (unit shape scaled by k) under a transform. */
static int heart_poly(const xform_t *t, double k, double *out) {
    heart_init();
    int n = 0;
    double off = 0.055;   /* same vertical centring as suit_inside */
    double ta = atan2(hp_quad[5] - H_LY, hp_quad[4] - H_LX);
    double na = atan2(-sqrt(H_R * H_R - H_LX * H_LX), -H_LX);
    double arc[2 * 14];
    for (int i = 0; i < 14; i++) {
        double a = ta + (na - ta) * i / 13.0;
        arc[2 * i] = H_LX + H_R * cos(a);
        arc[2 * i + 1] = H_LY + H_R * sin(a);
    }
    xf_apply(t, 0, (H_PY - off) * k, &out[0], &out[1]);
    n = 1;
    for (int i = 0; i < 14; i++, n++) xf_apply(t, arc[2 * i] * k, (arc[2 * i + 1] - off) * k, &out[2 * n], &out[2 * n + 1]);
    for (int i = 13; i >= 0; i--, n++) xf_apply(t, -arc[2 * i] * k, (arc[2 * i + 1] - off) * k, &out[2 * n], &out[2 * n + 1]);
    return n;
}

static void sol_icon(surface_t *s, int x, int y, int size) {
    double z = size, e = MAX(1.0, z / 48);
    double p[2 * IC_MAXP];
    int n;
    double a1 = -13 * M_PI / 180, a2 = 9 * M_PI / 180;
    xform_t tb = { x + 0.36 * z, y + 0.45 * z, cos(a1), sin(a1) };
    xform_t tf = { x + 0.62 * z, y + 0.54 * z, cos(a2), sin(a2) };
    xform_t tsh = { tf.cx + 0.02 * z, tf.cy + 0.03 * z, tf.ca, tf.sa };
    double bw = 0.25 * z, bh = 0.345 * z, fw = 0.265 * z, fh = 0.365 * z, r = 0.062 * z;

    /* back card: edge, white border, blue panel, inner line */
    n = rrect_poly(&tb, bw, bh, r, p);
    poly_aa(s, p, n, RGB(120, 128, 140), true);
    n = rrect_poly(&tb, bw - e, bh - e, r - e, p);
    poly_aa(s, p, n, RGB(250, 250, 250), true);
    double m = 0.055 * z;
    n = rrect_poly(&tb, bw - m, bh - m, r - m * 0.6, p);
    poly_aa(s, p, n, RGB(46, 112, 222), true);
    double l = 0.085 * z;
    n = rrect_poly(&tb, bw - l, bh - l, MAX(1.0, r - l * 0.8), p);
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        gfx_line_aa(s, p[2 * i], p[2 * i + 1], p[2 * j], p[2 * j + 1], RGB(150, 188, 242));
        if (z >= 64) gfx_line_aa(s, p[2 * i] + 0.5, p[2 * i + 1] + 0.5, p[2 * j] + 0.5, p[2 * j + 1] + 0.5, RGB(150, 188, 242));
    }

    /* front card: soft shadow, edge, face, heart */
    n = rrect_poly(&tsh, fw + 1, fh + 1, r + 1, p);
    poly_aa(s, p, n, RGBA(0, 0, 0, 50), false);
    n = rrect_poly(&tf, fw, fh, r, p);
    poly_aa(s, p, n, RGB(110, 116, 128), true);
    n = rrect_poly(&tf, fw - e, fh - e, r - e, p);
    poly_aa(s, p, n, RGB(255, 255, 255), true);
    n = heart_poly(&tf, 0.175 * z, p);
    poly_aa(s, p, n, COL_RED, true);
}

REGISTER_APP(.name = "Solitaire", .category = CAT_GAMES,
             .description = "Klondike patience: drag the cards home to the foundations",
             .launch = sol_launch, .draw_icon = sol_icon);
