/*
 * extras_notes.c - "Sticky Notes" (Accessories): little coloured paper notes.
 *
 *   - every note is its own small frameless window: a coloured header strip
 *     (drag it to move the note) with "+" (new note), "..." (colour palette
 *     and delete) and a close button, a word-wrapped plain-text body and a
 *     resize grip in the bottom-right corner;
 *   - a self-contained text editor: caret, mouse / Shift selection, double
 *     click selects a word, Ctrl+Left/Right word jumps, Up/Down by visual
 *     line, Home/End, PgUp/PgDn, Ctrl+A/C/X/V, Ctrl+Z / Ctrl+Y undo/redo,
 *     wheel scrolling with a slim scroll indicator;
 *   - seven paper colours (Yellow, Green, Pink, Purple, Blue, Grey, Charcoal);
 *   - notes are saved automatically (a second after the last edit, after a
 *     move/resize/colour change and when closed) to /home/user/.notes/:
 *     note-N.txt holds the plain text, note-N.meta the colour and geometry.
 *     Closing an empty note removes its files;
 *   - launching the app restores every saved note that is not open yet (or
 *     creates a first note); launching it while notes are open creates a new
 *     note. Ctrl+N new note, Ctrl+D delete note.
 *   - any other text file (".note" files are registered with this app, see
 *     REGISTER_FILETYPE in kernel/shell/assoc.c) opens as a note that is
 *     saved back to that file; deleting such a note deletes the file.
 *
 * All editing state lives in a per-window struct; the only global is the
 * list of open notes (to avoid opening a note twice and to place new ones).
 */
#include <nova.h>

#define NOTES_DIR  "/home/user/.notes"
#define SN_W       260
#define SN_H       240
#define SN_MIN_W   180
#define SN_MIN_H   120
#define HDR_H      32
#define PAD_X      14
#define PAD_TOP    8
#define PAD_BOT    10
#define LINE_H     20
#define GRIP       16
#define PAL_H      48
#define MAX_TEXT   65536
#define UNDO_MAX   48
#define NFONT      FONT_UI

typedef struct { const char *name; uint32_t bg, hdr, ink; } note_color_t;
static const note_color_t g_colors[] = {
    { "Yellow",   0xFFFFF4A6u, 0xFFFFE470u, 0xFF3A3420u },
    { "Green",    0xFFD3F3C8u, 0xFFAEE39Eu, 0xFF22381Eu },
    { "Pink",     0xFFFFD6E8u, 0xFFFFB3D5u, 0xFF46222Fu },
    { "Purple",   0xFFE7D8FFu, 0xFFCFB6FFu, 0xFF30244Au },
    { "Blue",     0xFFD3EAFFu, 0xFFA9D3FFu, 0xFF1C2E46u },
    { "Grey",     0xFFE9E9EBu, 0xFFCFCFD4u, 0xFF2A2A2Eu },
    { "Charcoal", 0xFF45474Fu, 0xFF33353Cu, 0xFFEDEDF0u },
};
#define NCOLORS ARRAY_SIZE(g_colors)

enum { B_NONE = -1, B_NEW, B_MENU, B_CLOSE, B_COUNT };
enum { M_NONE, M_SELECT, M_MOVE, M_RESIZE, M_BUTTON, M_PALETTE };
enum { ED_NONE, ED_TYPE, ED_DELETE, ED_OTHER };

typedef struct { char *text; int len, caret; } snap_t;

typedef struct sticky_note {
    window_t *win;
    int id;                     /* note-N in NOTES_DIR; 0 for a file note */
    char *path;                 /* file note: the file it was opened from */
    char *text;
    int len, cap;
    int caret, anchor;
    int pref_x;                 /* remembered x for Up/Down, -1 = none   */
    int *ls;                    /* visual line starts                    */
    int nl, lcap;
    int wrap_w;                 /* width the layout was computed for     */
    int scroll;                 /* pixels                                */
    int color;
    bool dirty, meta_dirty, deleted;
    uint32_t edit_ms;
    bool blink_on;
    uint32_t blink_ms;
    int mode;
    int hover_btn, press_btn;
    int smx, smy;               /* mouse (screen) at the start of a move/resize */
    rect_t sframe;
    bool palette;
    int pal_hover, pal_press;   /* swatch index, NCOLORS = delete, -1 none */
    snap_t undo[UNDO_MAX], redo[UNDO_MAX];
    int nundo, nredo;
    int last_kind;
    uint32_t last_edit_ms;
    char title[48];
    struct sticky_note *next;
} sn_t;

static sn_t *g_notes;

static void note_new_from(sn_t *src);
static void note_request_delete(sn_t *n);

/* ---- files ------------------------------------------------------------------------- */
static void note_paths(int id, char *txt, char *meta) {
    if (txt) snprintf(txt, VFS_PATH_MAX, NOTES_DIR "/note-%d.txt", id);
    if (meta) snprintf(meta, VFS_PATH_MAX, NOTES_DIR "/note-%d.meta", id);
}

static bool note_blank(sn_t *n) {
    for (int i = 0; i < n->len; i++)
        if (!isspace((unsigned char)n->text[i])) return false;
    return true;
}

static void note_save_meta(sn_t *n) {
    char meta[VFS_PATH_MAX], buf[128];
    if (n->path) { n->meta_dirty = false; return; }    /* file notes keep no geometry */
    note_paths(n->id, NULL, meta);
    rect_t f = win_frame_rect(n->win);
    int len = snprintf(buf, sizeof buf, "color=%d\nx=%d\ny=%d\nw=%d\nh=%d\n", n->color, f.x, f.y,
                       win_width(n->win), win_height(n->win));
    vfs_write_file(meta, buf, (uint32_t)len);
    n->meta_dirty = false;
}

/* Write the note (text + meta). A blank note that was never saved stays unsaved. */
static void note_save(sn_t *n) {
    if (n->deleted) return;
    char txt[VFS_PATH_MAX];
    if (n->path) {              /* a file note: write the text back to its file */
        if (n->dirty) {
            int rc = vfs_write_file(n->path, n->text, (uint32_t)n->len);
            if (rc < 0) klog("notes: cannot save %s: %s", n->path, vfs_strerror(rc));
            else klog("notes: saved %s (%d bytes)", n->path, n->len);
        }
        n->dirty = n->meta_dirty = false;
        return;
    }
    note_paths(n->id, txt, NULL);
    if (note_blank(n) && !vfs_exists(txt)) { n->dirty = n->meta_dirty = false; return; }
    vfs_mkdirs(NOTES_DIR);
    if (n->dirty || !vfs_exists(txt)) {
        int rc = vfs_write_file(txt, n->text, (uint32_t)n->len);
        if (rc < 0) klog("notes: cannot save %s: %s", txt, vfs_strerror(rc));
        else klog("notes: saved note-%d (%d bytes)", n->id, n->len);
    }
    note_save_meta(n);
    n->dirty = false;
}

static void note_remove_files(int id) {
    if (id <= 0) return;
    char txt[VFS_PATH_MAX], meta[VFS_PATH_MAX];
    note_paths(id, txt, meta);
    if (vfs_exists(txt)) vfs_remove(txt);
    if (vfs_exists(meta)) vfs_remove(meta);
}

/* "note-12.txt" -> 12, else -1 */
static int note_id_from_name(const char *name) {
    if (strncmp(name, "note-", 5)) return -1;
    const char *p = name + 5;
    if (!isdigit((unsigned char)*p)) return -1;
    int id = 0;
    while (isdigit((unsigned char)*p)) { id = id * 10 + (*p - '0'); p++; if (id > 1000000) return -1; }
    return strcmp(p, ".txt") ? -1 : id;
}

static sn_t *note_find(int id) {
    for (sn_t *n = g_notes; n; n = n->next)
        if (n->id == id) return n;
    return NULL;
}

static sn_t *note_find_path(const char *path) {
    for (sn_t *n = g_notes; n; n = n->next)
        if (n->path && !strcmp(n->path, path)) return n;
    return NULL;
}

static int note_next_id(void) {
    int best = 0;
    for (sn_t *n = g_notes; n; n = n->next) best = MAX(best, n->id);
    int cnt = vfs_list(NOTES_DIR, NULL, 0);
    if (cnt > 0) {
        vfs_dirent_t *e = malloc(sizeof(vfs_dirent_t) * (size_t)cnt);
        if (e) {
            int got = MIN(vfs_list(NOTES_DIR, e, cnt), cnt);
            for (int i = 0; i < got; i++) best = MAX(best, note_id_from_name(e[i].name));
            /* also skip ids that only have a .meta left behind */
            for (int i = 0; i < got; i++) {
                int id = 0;
                if (sscanf(e[i].name, "note-%d.meta", &id) == 1) best = MAX(best, id);
            }
            free(e);
        }
    }
    return best + 1;
}

/* ---- text layout --------------------------------------------------------------------- */
static rect_t text_rect(sn_t *n) {
    int W = win_width(n->win), H = win_height(n->win);
    return rect_make(PAD_X, HDR_H + PAD_TOP, W - 2 * PAD_X, H - HDR_H - PAD_TOP - PAD_BOT);
}

static bool add_line(sn_t *n, int start) {
    if (n->nl >= n->lcap) {
        int c = n->lcap ? n->lcap * 2 : 64;
        int *p = realloc(n->ls, sizeof(int) * (size_t)c);
        if (!p) return false;
        n->ls = p;
        n->lcap = c;
    }
    n->ls[n->nl++] = start;
    return true;
}

static void note_layout(sn_t *n) {
    int width = MAX(text_rect(n).w, 20);
    n->wrap_w = width;
    n->nl = 0;
    add_line(n, 0);
    int pos = 0;
    while (pos <= n->len) {
        int s = pos, x = 0, last_space = -1, j = s, brk = -1;
        while (j < n->len && n->text[j] != '\n') {
            unsigned char c = (unsigned char)n->text[j];
            int cw = font_char_width(NFONT, c);
            if (c != ' ' && x + cw > width && j > s) {
                brk = last_space >= s ? last_space + 1 : j;
                break;
            }
            if (c == ' ') last_space = j;
            x += cw;
            j++;
        }
        if (brk >= 0) {
            pos = brk;
            if (!add_line(n, pos)) break;
            continue;
        }
        if (j >= n->len) break;
        pos = j + 1;            /* hard line break */
        if (!add_line(n, pos)) break;
    }
}

static int line_of(sn_t *n, int off) {
    int lo = 0, hi = n->nl - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (n->ls[mid] <= off) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* End offset of visual line k (where the caret goes with End). */
static int line_end(sn_t *n, int k) {
    if (k + 1 >= n->nl) return n->len;
    int e = n->ls[k + 1];
    char c = n->text[e - 1];
    return (c == '\n' || c == ' ') ? e - 1 : e;
}

/* Last offset drawn on line k (exclusive). */
static int line_draw_end(sn_t *n, int k) {
    if (k + 1 >= n->nl) return n->len;
    int e = n->ls[k + 1];
    return n->text[e - 1] == '\n' ? e - 1 : e;
}

static int text_w(sn_t *n, int a, int b) {
    int w = 0;
    for (int i = a; i < b; i++) w += font_char_width(NFONT, (unsigned char)n->text[i]);
    return w;
}

static int caret_x(sn_t *n, int off) {
    int k = line_of(n, off);
    return text_w(n, n->ls[k], off);
}

static int hit_line(sn_t *n, int k, int px) {
    int s = n->ls[k], e = line_end(n, k), x = 0;
    for (int i = s; i < e; i++) {
        int cw = font_char_width(NFONT, (unsigned char)n->text[i]);
        if (px < x + cw / 2) return i;
        x += cw;
    }
    return e;
}

static int offset_at(sn_t *n, int x, int y) {
    rect_t tr = text_rect(n);
    int k = (y - tr.y + n->scroll);
    k = k < 0 ? -1 : k / LINE_H;
    if (k < 0) return 0;
    if (k >= n->nl) return n->len;
    return hit_line(n, k, x - tr.x);
}

static int max_scroll(sn_t *n) {
    return MAX(0, n->nl * LINE_H - text_rect(n).h);
}

static void inval_body(sn_t *n) {
    win_invalidate_rect(n->win, 0, HDR_H, win_width(n->win), win_height(n->win) - HDR_H);
}

static void set_scroll(sn_t *n, int s) {
    s = CLAMP(s, 0, max_scroll(n));
    if (s != n->scroll) { n->scroll = s; inval_body(n); }
}

static void ensure_caret_visible(sn_t *n) {
    rect_t tr = text_rect(n);
    int y = line_of(n, n->caret) * LINE_H;
    if (y < n->scroll) set_scroll(n, y);
    else if (y + LINE_H > n->scroll + tr.h) set_scroll(n, y + LINE_H - tr.h);
    else set_scroll(n, n->scroll);
}

static void reset_blink(sn_t *n) {
    n->blink_on = true;
    n->blink_ms = sys_uptime_ms();
}

static void update_title(sn_t *n) {
    char t[48];
    int i = 0;
    while (i < n->len && isspace((unsigned char)n->text[i])) i++;
    int o = 0;
    while (i < n->len && n->text[i] != '\n' && o < 30) t[o++] = n->text[i++];
    while (o > 0 && t[o - 1] == ' ') o--;
    if (o == 30) { t[o++] = '.'; t[o++] = '.'; t[o++] = '.'; }
    t[o] = 0;
    if (!o) strlcpy(t, "Sticky Note", sizeof t);
    if (strcmp(t, n->title)) {
        strlcpy(n->title, t, sizeof n->title);
        win_set_title(n->win, t);
    }
}

/* ---- editing ----------------------------------------------------------------------------- */
static bool ensure_cap(sn_t *n, int need) {
    if (need + 1 <= n->cap) return true;
    int c = MAX(need + 1, MAX(n->cap * 2, 256));
    char *p = realloc(n->text, (size_t)c);
    if (!p) return false;
    n->text = p;
    n->cap = c;
    return true;
}

static void snap_free(snap_t *s) { free(s->text); s->text = NULL; }

static bool snap_take(sn_t *n, snap_t *s) {
    s->text = malloc((size_t)n->len + 1);
    if (!s->text) return false;
    memcpy(s->text, n->text, (size_t)n->len);
    s->text[n->len] = 0;
    s->len = n->len;
    s->caret = n->caret;
    return true;
}

static void stack_push(snap_t *st, int *cnt, snap_t s) {
    if (*cnt == UNDO_MAX) {
        snap_free(&st[0]);
        memmove(&st[0], &st[1], sizeof(snap_t) * (UNDO_MAX - 1));
        (*cnt)--;
    }
    st[(*cnt)++] = s;
}

static void begin_edit(sn_t *n, int kind) {
    uint32_t now = sys_uptime_ms();
    if (kind == ED_OTHER || kind != n->last_kind || now - n->last_edit_ms > 1500 || !n->nundo) {
        snap_t s;
        if (snap_take(n, &s)) stack_push(n->undo, &n->nundo, s);
        for (int i = 0; i < n->nredo; i++) snap_free(&n->redo[i]);
        n->nredo = 0;
    }
    n->last_kind = kind;
    n->last_edit_ms = now;
}

static void text_changed(sn_t *n) {
    note_layout(n);
    n->dirty = true;
    n->edit_ms = sys_uptime_ms();
    n->pref_x = -1;
    reset_blink(n);
    ensure_caret_visible(n);
    update_title(n);
    inval_body(n);
}

static void replace_range(sn_t *n, int a, int b, const char *s, int slen) {
    int keep = n->len - (b - a);
    if (keep + slen > MAX_TEXT) slen = MAX(0, MAX_TEXT - keep);
    if (!ensure_cap(n, keep + slen)) return;
    memmove(n->text + a + slen, n->text + b, (size_t)(n->len - b));
    if (slen) memcpy(n->text + a, s, (size_t)slen);
    n->len = keep + slen;
    n->text[n->len] = 0;
    n->caret = n->anchor = a + slen;
}

static void insert_text(sn_t *n, const char *s, int slen, int kind) {
    int a = MIN(n->caret, n->anchor), b = MAX(n->caret, n->anchor);
    if (!slen && a == b) return;
    if (n->len - (b - a) + slen > MAX_TEXT && slen) { sys_beep(440, 40); if (n->len - (b - a) >= MAX_TEXT) return; }
    begin_edit(n, kind);
    replace_range(n, a, b, s, slen);
    text_changed(n);
}

static void delete_range(sn_t *n, int a, int b, int kind) {
    if (a > b) { int t = a; a = b; b = t; }
    a = CLAMP(a, 0, n->len);
    b = CLAMP(b, 0, n->len);
    if (a == b) return;
    begin_edit(n, kind);
    replace_range(n, a, b, "", 0);
    text_changed(n);
}

static void restore_snap(sn_t *n, snap_t *s) {
    if (!ensure_cap(n, s->len)) return;
    memcpy(n->text, s->text, (size_t)s->len);
    n->len = s->len;
    n->text[n->len] = 0;
    n->caret = n->anchor = CLAMP(s->caret, 0, n->len);
    n->last_kind = ED_NONE;
    text_changed(n);
}

static void note_undo(sn_t *n, bool redo) {
    snap_t *from = redo ? n->redo : n->undo, *to = redo ? n->undo : n->redo;
    int *fc = redo ? &n->nredo : &n->nundo, *tc = redo ? &n->nundo : &n->nredo;
    if (!*fc) { sys_beep(440, 30); return; }
    snap_t cur;
    if (!snap_take(n, &cur)) return;
    stack_push(to, tc, cur);
    snap_t s = from[--(*fc)];
    restore_snap(n, &s);
    snap_free(&s);
}

static bool is_word(char c) { return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }

static int word_left(sn_t *n, int i) {
    while (i > 0 && !is_word(n->text[i - 1])) i--;
    while (i > 0 && is_word(n->text[i - 1])) i--;
    return i;
}

static int word_right(sn_t *n, int i) {
    while (i < n->len && !is_word(n->text[i])) i++;
    while (i < n->len && is_word(n->text[i])) i++;
    return i;
}

static void move_caret(sn_t *n, int off, bool extend, bool keep_pref) {
    n->caret = CLAMP(off, 0, n->len);
    if (!extend) n->anchor = n->caret;
    if (!keep_pref) n->pref_x = -1;
    reset_blink(n);
    ensure_caret_visible(n);
    inval_body(n);
}

static void move_vert(sn_t *n, int dl, bool extend) {
    int k = line_of(n, n->caret);
    if (n->pref_x < 0) n->pref_x = caret_x(n, n->caret);
    int t = k + dl;
    if (t < 0) { move_caret(n, 0, extend, true); return; }
    if (t >= n->nl) { move_caret(n, n->len, extend, true); return; }
    move_caret(n, hit_line(n, t, n->pref_x), extend, true);
}

static void copy_sel(sn_t *n) {
    int a = MIN(n->caret, n->anchor), b = MAX(n->caret, n->anchor);
    if (a == b) return;
    char *t = malloc((size_t)(b - a) + 1);
    if (!t) return;
    memcpy(t, n->text + a, (size_t)(b - a));
    t[b - a] = 0;
    clipboard_set(t);
    free(t);
}

static void paste(sn_t *n) {
    const char *c = clipboard_get();
    int cl = (int)strlen(c);
    if (!cl) return;
    char *buf = malloc((size_t)cl * 4 + 1);
    if (!buf) return;
    int o = 0;
    for (int i = 0; i < cl; i++) {
        unsigned char ch = (unsigned char)c[i];
        if (ch == '\r') continue;
        if (ch == '\t') { memcpy(buf + o, "    ", 4); o += 4; continue; }
        if (ch < 0x20 && ch != '\n') continue;
        buf[o++] = (char)ch;
    }
    insert_text(n, buf, o, ED_OTHER);
    free(buf);
}

/* ---- geometry of the header controls -------------------------------------------------- */
static rect_t btn_rect(sn_t *n, int b) {
    int W = win_width(n->win);
    switch (b) {
    case B_NEW: return rect_make(4, 2, 30, HDR_H - 4);
    case B_MENU: return rect_make(W - 4 - 30 - 30, 2, 30, HDR_H - 4);
    case B_CLOSE: return rect_make(W - 4 - 30, 2, 30, HDR_H - 4);
    default: return rect_make(0, 0, 0, 0);
    }
}

static int btn_at(sn_t *n, int x, int y) {
    for (int b = 0; b < B_COUNT; b++)
        if (rect_contains(btn_rect(n, b), x, y)) return b;
    return B_NONE;
}

static rect_t grip_rect(sn_t *n) {
    return rect_make(win_width(n->win) - GRIP, win_height(n->win) - GRIP, GRIP, GRIP);
}

/* palette: NCOLORS swatches and a delete button */
static int pal_step(sn_t *n) { return MIN(32, (win_width(n->win) - 20 - 40) / (int)NCOLORS); }

static rect_t pal_item_rect(sn_t *n, int i) {
    int step = pal_step(n);
    int y = HDR_H + (PAL_H - step) / 2;
    if (i == (int)NCOLORS) return rect_make(win_width(n->win) - 10 - 34, HDR_H + (PAL_H - 34) / 2, 34, 34);
    return rect_make(10 + i * step, y, step, step);
}

static int pal_at(sn_t *n, int x, int y) {
    if (!n->palette || y < HDR_H || y >= HDR_H + PAL_H) return -1;
    for (int i = 0; i <= (int)NCOLORS; i++)
        if (rect_contains(pal_item_rect(n, i), x, y)) return i;
    return -1;
}

static void inval_header(sn_t *n) {
    win_invalidate_rect(n->win, 0, 0, win_width(n->win), HDR_H + (n->palette ? PAL_H : 0));
}

/* ---- painting -------------------------------------------------------------------------------- */
static void draw_plus(surface_t *s, rect_t r, uint32_t c) {
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    gfx_fill_rect(s, cx - 6, cy - 1, 13, 2, c);
    gfx_fill_rect(s, cx - 1, cy - 6, 2, 13, c);
}

static void draw_dots(surface_t *s, rect_t r, uint32_t c) {
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    for (int i = -1; i <= 1; i++) gfx_fill_circle_aa(s, cx + i * 6, cy, 1.8, c);
}

static void draw_trash(surface_t *s, int cx, int cy, uint32_t c) {
    gfx_fill_rect(s, cx - 7, cy - 6, 14, 2, c);                  /* lid  */
    gfx_fill_rect(s, cx - 2, cy - 8, 4, 2, c);                   /* knob */
    point_t body[4] = { { cx - 6, cy - 3 }, { cx + 6, cy - 3 }, { cx + 5, cy + 8 }, { cx - 5, cy + 8 } };
    gfx_fill_polygon(s, body, 4, c);
    gfx_fill_rect(s, cx - 3, cy - 1, 1, 7, RGBA(255, 255, 255, 140));
    gfx_fill_rect(s, cx + 2, cy - 1, 1, 7, RGBA(255, 255, 255, 140));
}

static void note_paint(sn_t *n, surface_t *s) {
    const note_color_t *col = &g_colors[n->color];
    int W = s->w, H = s->h;
    bool focused = win_is_focused(n->win);
    bool darkpaper = color_luma(col->bg) < 128;
    uint32_t ink = col->ink;
    uint32_t soft = color_mix(ink, col->bg, 150);

    gfx_fill_rect(s, 0, 0, W, H, col->bg);
    /* header */
    gfx_fill_rect(s, 0, 0, W, HDR_H, col->hdr);
    gfx_hline(s, 0, HDR_H - 1, W, color_shade(col->hdr, darkpaper ? 12 : -14));
    for (int b = 0; b < B_COUNT; b++) {
        rect_t r = btn_rect(n, b);
        bool hov = n->hover_btn == b, prs = n->press_btn == b && hov && n->mode == M_BUTTON;
        bool active = b == B_MENU && n->palette;
        if (prs || active) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 5, color_alpha(ink, 46));
        else if (hov) gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 5, b == B_CLOSE ? RGB(232, 72, 72) : color_alpha(ink, 26));
        uint32_t gc = (focused || hov || active) ? ink : color_alpha(ink, 110);
        if (b == B_CLOSE && hov) gc = COLOR_WHITE;
        if (b == B_NEW) draw_plus(s, r, gc);
        else if (b == B_MENU) draw_dots(s, r, gc);
        else ui_draw_glyph(s, UI_GLYPH_CLOSE, r.x + (r.w - 12) / 2, r.y + (r.h - 12) / 2, 12, gc);
    }

    /* text */
    rect_t tr = text_rect(n);
    rect_t old = s->clip;
    surface_set_clip(s, rect_intersect(old, rect_make(tr.x - 2, tr.y, tr.w + 4, tr.h)));
    int a = MIN(n->caret, n->anchor), b = MAX(n->caret, n->anchor);
    uint32_t sel = darkpaper ? RGBA(120, 170, 255, 110) : color_alpha(color_shade(col->hdr, -70), 90);
    int first = n->scroll / LINE_H;
    int fy = (LINE_H - font_height(NFONT)) / 2;
    for (int k = first; k < n->nl; k++) {
        int y = tr.y + k * LINE_H - n->scroll;
        if (y >= tr.y + tr.h) break;
        int ls = n->ls[k], le = line_draw_end(n, k);
        if (a != b && b > ls && a <= le) {
            int sa = MAX(a, ls), sb = MIN(b, le);
            int sx = tr.x + text_w(n, ls, sa);
            int sw = text_w(n, sa, sb);
            if (b > le && k + 1 < n->nl) sw += 4;   /* selection continues on the next line */
            gfx_fill_rect(s, sx, y + 1, sw, LINE_H - 2, sel);
        }
        gfx_text_n(s, tr.x, y + fy, n->text + ls, le - ls, ink, NFONT);
    }
    if (!n->len) {
        gfx_text(s, tr.x, tr.y + fy, "Take a note...", soft, NFONT);
    }
    /* caret */
    if (focused && n->blink_on && n->mode != M_MOVE && n->mode != M_RESIZE) {
        int k = line_of(n, n->caret);
        int cx = tr.x + caret_x(n, n->caret);
        int cy = tr.y + k * LINE_H - n->scroll;
        gfx_fill_rect(s, cx, cy + 2, 2, LINE_H - 4, darkpaper ? RGB(255, 255, 255) : color_shade(ink, 10));
    }
    s->clip = old;

    /* scroll indicator */
    int content = n->nl * LINE_H;
    if (content > tr.h) {
        int th = MAX(20, tr.h * tr.h / content);
        int ty = tr.y + (int)((int64_t)(tr.h - th) * n->scroll / MAX(1, content - tr.h));
        gfx_fill_rounded_rect(s, W - 7, ty, 4, th, 2, color_alpha(ink, 70));
    }

    /* resize grip */
    rect_t g = grip_rect(n);
    uint32_t gc = color_alpha(ink, 80);
    for (int i = 0; i < 3; i++) {
        int d = 4 + i * 4;
        gfx_line_aa(s, g.x + g.w - 3 - d, g.y + g.h - 3, g.x + g.w - 3, g.y + g.h - 3 - d, gc);
    }

    /* palette */
    if (n->palette) {
        gfx_fill_rect(s, 0, HDR_H, W, PAL_H, color_mix(col->hdr, col->bg, 90));
        gfx_hline(s, 0, HDR_H + PAL_H - 1, W, color_shade(col->hdr, darkpaper ? 12 : -18));
        int step = pal_step(n);
        for (int i = 0; i < (int)NCOLORS; i++) {
            rect_t r = pal_item_rect(n, i);
            double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0, rad = step / 2.0 - 3;
            if (i == n->pal_hover) gfx_fill_circle_aa(s, cx, cy, rad + 2.5, color_alpha(ink, 60));
            gfx_fill_circle_aa(s, cx, cy, rad, color_shade(g_colors[i].hdr, -40));
            gfx_fill_circle_aa(s, cx, cy, rad - 1.2, g_colors[i].hdr);
            if (i == n->color) {
                uint32_t mc = g_colors[i].ink;
                ui_draw_glyph(s, UI_GLYPH_CHECK, (int)cx - 6, (int)cy - 6, 12, mc);
            }
        }
        rect_t d = pal_item_rect(n, NCOLORS);
        if (n->pal_hover == (int)NCOLORS) gfx_fill_rounded_rect(s, d.x, d.y, d.w, d.h, 6, RGBA(220, 60, 60, 60));
        draw_trash(s, d.x + d.w / 2, d.y + d.h / 2 + 1, n->pal_hover == (int)NCOLORS ? RGB(200, 40, 40) : ink);
    }

    /* paper edge */
    gfx_rect(s, 0, 0, W, H, color_shade(col->hdr, darkpaper ? 20 : -38));
}

/* ---- window events --------------------------------------------------------------------- */
static void set_color(sn_t *n, int c) {
    if (c < 0 || c >= (int)NCOLORS || c == n->color) return;
    n->color = c;
    n->meta_dirty = true;
    note_save(n);
    win_invalidate(n->win);
}

static void close_palette(sn_t *n) {
    if (!n->palette) return;
    n->palette = false;
    n->pal_hover = -1;
    win_invalidate(n->win);
}

static void update_cursor(sn_t *n, int x, int y) {
    int c = CURSOR_ARROW;
    if (rect_contains(grip_rect(n), x, y)) c = CURSOR_RESIZE_NWSE;
    else if (y >= HDR_H + (n->palette ? PAL_H : 0)) c = CURSOR_IBEAM;
    win_set_cursor(n->win, c);
}

static void note_mouse(sn_t *n, const event_t *ev) {
    window_t *win = n->win;
    switch (ev->type) {
    case EV_MOUSE_DOWN: {
        if (ev->button != MOUSE_LEFT) {
            if (ev->button == MOUSE_RIGHT && ev->y < HDR_H) { n->palette = !n->palette; n->pal_hover = -1; win_invalidate(win); }
            return;
        }
        int p = pal_at(n, ev->x, ev->y);
        if (p >= 0) { n->mode = M_PALETTE; n->pal_press = p; return; }
        int b = btn_at(n, ev->x, ev->y);
        if (b != B_NONE) {
            n->mode = M_BUTTON;
            n->press_btn = b;
            n->hover_btn = b;
            inval_header(n);
            return;
        }
        if (n->palette && ev->y < HDR_H + PAL_H) { if (ev->y >= HDR_H) return; }
        else if (n->palette) close_palette(n);
        if (rect_contains(grip_rect(n), ev->x, ev->y)) {
            n->mode = M_RESIZE;
            input_mouse_pos(&n->smx, &n->smy);
            n->sframe = rect_make(0, 0, win_width(win), win_height(win));
            return;
        }
        if (ev->y < HDR_H) {
            n->mode = M_MOVE;
            input_mouse_pos(&n->smx, &n->smy);
            n->sframe = win_frame_rect(win);
            return;
        }
        n->mode = M_SELECT;
        move_caret(n, offset_at(n, ev->x, ev->y), (ev->mods & MOD_SHIFT) != 0, false);
        return;
    }
    case EV_DOUBLE_CLICK:
        if (ev->button == MOUSE_LEFT && n->mode == M_SELECT) {
            int o = offset_at(n, ev->x, ev->y);
            int a = o, b = o;
            if (o < n->len && is_word(n->text[o])) { a = word_left(n, o + 1); b = word_right(n, o); }
            else if (o > 0 && is_word(n->text[o - 1])) { a = word_left(n, o); b = word_right(n, o - 1); }
            n->anchor = a;
            move_caret(n, b, true, false);
            n->mode = M_NONE;   /* don't extend the word selection on release */
        }
        return;
    case EV_MOUSE_MOVE: {
        int mx, my;
        if (n->mode == M_MOVE) {
            input_mouse_pos(&mx, &my);
            int nx = n->sframe.x + mx - n->smx, ny = n->sframe.y + my - n->smy;
            nx = CLAMP(nx, -(n->sframe.w - 60), screen_width() - 60);
            ny = CLAMP(ny, 0, screen_work_height() - HDR_H);
            rect_t f = win_frame_rect(win);
            if (nx != f.x || ny != f.y) win_move(win, nx, ny);
            return;
        }
        if (n->mode == M_RESIZE) {
            input_mouse_pos(&mx, &my);
            rect_t f = win_frame_rect(win);
            int nw = CLAMP(n->sframe.w + mx - n->smx, SN_MIN_W, screen_width() - f.x);
            int nh = CLAMP(n->sframe.h + my - n->smy, SN_MIN_H, screen_work_height() - f.y);
            if (nw != win_width(win) || nh != win_height(win)) win_resize(win, nw, nh);
            return;
        }
        if (n->mode == M_SELECT) {
            rect_t tr = text_rect(n);
            if (ev->y < tr.y) set_scroll(n, n->scroll - LINE_H / 2);
            else if (ev->y >= tr.y + tr.h) set_scroll(n, n->scroll + LINE_H / 2);
            int o = offset_at(n, ev->x, CLAMP(ev->y, tr.y, tr.y + tr.h - 1));
            if (o != n->caret) move_caret(n, o, true, false);
            return;
        }
        int hb = (n->mode == M_NONE || n->mode == M_BUTTON) ? btn_at(n, ev->x, ev->y) : B_NONE;
        if (hb != n->hover_btn) { n->hover_btn = hb; inval_header(n); }
        int ph = pal_at(n, ev->x, ev->y);
        if (ph != n->pal_hover) { n->pal_hover = ph; inval_header(n); }
        update_cursor(n, ev->x, ev->y);
        return;
    }
    case EV_MOUSE_UP: {
        if (ev->button != MOUSE_LEFT) return;
        int mode = n->mode;
        n->mode = M_NONE;
        if (mode == M_BUTTON) {
            int b = n->press_btn;
            n->press_btn = B_NONE;
            inval_header(n);
            if (btn_at(n, ev->x, ev->y) != b) return;
            if (b == B_NEW) note_new_from(n);
            else if (b == B_MENU) { n->palette = !n->palette; n->pal_hover = -1; win_invalidate(win); }
            else if (b == B_CLOSE) win_close(win);   /* EV_CLOSE saves */
            return;
        }
        if (mode == M_PALETTE) {
            int p = pal_at(n, ev->x, ev->y);
            if (p != n->pal_press || p < 0) return;
            if (p == (int)NCOLORS) { close_palette(n); note_request_delete(n); }
            else set_color(n, p);
            return;
        }
        if (mode == M_MOVE || mode == M_RESIZE) {
            note_save(n);
            reset_blink(n);
            inval_body(n);
        }
        return;
    }
    case EV_MOUSE_WHEEL:
        set_scroll(n, n->scroll + ev->wheel * LINE_H * 2);
        return;
    case EV_MOUSE_LEAVE:
        if (n->hover_btn != B_NONE || n->pal_hover >= 0) {
            n->hover_btn = B_NONE;
            n->pal_hover = -1;
            inval_header(n);
        }
        return;
    default:
        return;
    }
}

static void note_key(sn_t *n, const event_t *ev) {
    bool sh = (ev->mods & MOD_SHIFT) != 0, ctrl = (ev->mods & MOD_CTRL) != 0;
    if (ev->mods & MOD_ALT) return;
    int a = MIN(n->caret, n->anchor), b = MAX(n->caret, n->anchor);
    if (ctrl) {
        switch (ev->key) {
        case 'a': n->anchor = 0; move_caret(n, n->len, true, false); return;
        case 'c': copy_sel(n); return;
        case 'x': if (a != b) { copy_sel(n); delete_range(n, a, b, ED_OTHER); } return;
        case 'v': paste(n); return;
        case 'z': note_undo(n, sh); return;
        case 'y': note_undo(n, true); return;
        case 'n': note_new_from(n); return;
        case 'd': note_request_delete(n); return;
        case 's': note_save(n); return;
        case KEY_LEFT: move_caret(n, word_left(n, n->caret), sh, false); return;
        case KEY_RIGHT: move_caret(n, word_right(n, n->caret), sh, false); return;
        case KEY_HOME: move_caret(n, 0, sh, false); return;
        case KEY_END: move_caret(n, n->len, sh, false); return;
        case KEY_UP: set_scroll(n, n->scroll - LINE_H); return;
        case KEY_DOWN: set_scroll(n, n->scroll + LINE_H); return;
        case KEY_BACKSPACE:
            if (a != b) delete_range(n, a, b, ED_DELETE);
            else delete_range(n, word_left(n, n->caret), n->caret, ED_DELETE);
            return;
        case KEY_DELETE:
            if (a != b) delete_range(n, a, b, ED_DELETE);
            else delete_range(n, n->caret, word_right(n, n->caret), ED_DELETE);
            return;
        default:
            return;
        }
    }
    switch (ev->key) {
    case KEY_LEFT:
        if (a != b && !sh) move_caret(n, a, false, false);
        else move_caret(n, n->caret - 1, sh, false);
        return;
    case KEY_RIGHT:
        if (a != b && !sh) move_caret(n, b, false, false);
        else move_caret(n, n->caret + 1, sh, false);
        return;
    case KEY_UP: move_vert(n, -1, sh); return;
    case KEY_DOWN: move_vert(n, 1, sh); return;
    case KEY_PAGEUP: move_vert(n, -MAX(1, text_rect(n).h / LINE_H - 1), sh); return;
    case KEY_PAGEDOWN: move_vert(n, MAX(1, text_rect(n).h / LINE_H - 1), sh); return;
    case KEY_HOME: move_caret(n, n->ls[line_of(n, n->caret)], sh, false); return;
    case KEY_END: move_caret(n, line_end(n, line_of(n, n->caret)), sh, false); return;
    case KEY_ENTER: insert_text(n, "\n", 1, ED_OTHER); return;
    case KEY_TAB: insert_text(n, "    ", 4, ED_TYPE); return;
    case KEY_BACKSPACE:
        if (a != b) delete_range(n, a, b, ED_DELETE);
        else if (n->caret > 0) delete_range(n, n->caret - 1, n->caret, ED_DELETE);
        return;
    case KEY_DELETE:
        if (a != b) delete_range(n, a, b, ED_DELETE);
        else if (n->caret < n->len) delete_range(n, n->caret, n->caret + 1, ED_DELETE);
        return;
    case KEY_ESCAPE:
        if (n->palette) close_palette(n);
        else if (a != b) { n->anchor = n->caret; inval_body(n); }
        return;
    default:
        return;
    }
}

static void note_free(sn_t *n) {
    for (sn_t **pp = &g_notes; *pp; pp = &(*pp)->next)
        if (*pp == n) { *pp = n->next; break; }
    for (int i = 0; i < n->nundo; i++) snap_free(&n->undo[i]);
    for (int i = 0; i < n->nredo; i++) snap_free(&n->redo[i]);
    free(n->text);
    free(n->ls);
    free(n->path);
    free(n);
}

static void note_event(window_t *win, const event_t *ev) {
    sn_t *n = win_get_userdata(win);
    if (!n) return;
    switch (ev->type) {
    case EV_PAINT:
        note_paint(n, win_surface(win));
        break;
    case EV_MOUSE_DOWN: case EV_MOUSE_UP: case EV_MOUSE_MOVE: case EV_DOUBLE_CLICK:
    case EV_MOUSE_WHEEL: case EV_MOUSE_LEAVE:
        note_mouse(n, ev);
        break;
    case EV_KEY_DOWN:
        note_key(n, ev);
        break;
    case EV_CHAR:
        if (ev->ch >= 0x20 && ev->ch <= 0xFF && ev->ch != 0x7F) {
            char c = (char)ev->ch;
            insert_text(n, &c, 1, ev->ch == ' ' ? ED_OTHER : ED_TYPE);
        }
        break;
    case EV_RESIZE:
        note_layout(n);
        n->scroll = CLAMP(n->scroll, 0, max_scroll(n));
        n->meta_dirty = true;
        break;
    case EV_TIMER: {
        uint32_t now = sys_uptime_ms();
        if (win_is_focused(win) && now - n->blink_ms >= 530) {
            n->blink_on = !n->blink_on;
            n->blink_ms = now;
            rect_t tr = text_rect(n);
            int k = line_of(n, n->caret);
            int cx = tr.x + caret_x(n, n->caret), cy = tr.y + k * LINE_H - n->scroll;
            win_invalidate_rect(win, cx - 1, cy, 4, LINE_H);
        }
        if (n->dirty && now - n->edit_ms >= 1000) note_save(n);
        break;
    }
    case EV_FOCUS: case EV_BLUR:
        reset_blink(n);
        if (ev->type == EV_BLUR) {
            if (n->dirty || n->meta_dirty) note_save(n);
            n->hover_btn = B_NONE;
            n->palette = false;
            n->pal_hover = -1;
        }
        win_invalidate(win);
        break;
    case EV_CLOSE:
        if (!n->deleted) {
            if (n->path) note_save(n);
            else if (note_blank(n)) note_remove_files(n->id);
            else { n->dirty = true; note_save(n); }
        }
        win_set_userdata(win, NULL);
        note_free(n);
        break;
    default:
        break;
    }
}

/* ---- creating / restoring notes ------------------------------------------------------------ */
static sn_t *note_open(int id, const char *text, int color, int x, int y, int w, int h, bool place) {
    sn_t *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->id = id;
    n->color = CLAMP(color, 0, (int)NCOLORS - 1);
    n->pref_x = -1;
    n->hover_btn = n->press_btn = B_NONE;
    n->pal_hover = n->pal_press = -1;
    int len = text ? (int)strlen(text) : 0;
    if (len > MAX_TEXT) len = MAX_TEXT;
    if (!ensure_cap(n, len)) { free(n); return NULL; }
    if (len) memcpy(n->text, text, (size_t)len);
    n->len = len;
    n->text[len] = 0;
    n->caret = n->anchor = len;
    w = CLAMP(w, SN_MIN_W, screen_width() - 2);
    h = CLAMP(h, SN_MIN_H, screen_work_height() - 2);
    window_t *win = win_create("Sticky Note", w, h, WIN_FRAMELESS);
    if (!win) { free(n->text); free(n); return NULL; }
    n->win = win;
    win_set_userdata(win, n);
    win_set_event_handler(win, note_event);
    win_set_cursor(win, CURSOR_IBEAM);
    if (place) {
        x = CLAMP(x, -(w - 60), screen_width() - 60);
        y = CLAMP(y, 0, screen_work_height() - HDR_H);
        win_move(win, x, y);
    }
    note_layout(n);
    ensure_caret_visible(n);
    update_title(n);
    reset_blink(n);
    win_set_timer(win, 100);
    n->next = g_notes;
    g_notes = n;
    win_invalidate(win);
    return n;
}

/* Default spot for a brand new note: along the right side of the screen. */
static void default_pos(int w, int h, int *x, int *y) {
    int k = 0;
    for (sn_t *n = g_notes; n; n = n->next) k++;
    k %= 8;
    *x = screen_width() - w - 40 - k * 26;
    *y = 30 + k * 26;
    if (*y + h > screen_work_height()) *y = MAX(0, screen_work_height() - h - 10);
}

static sn_t *note_create_new(int color, int x, int y, bool have_pos) {
    int id = note_next_id();
    if (!have_pos) default_pos(SN_W, SN_H, &x, &y);
    sn_t *n = note_open(id, "", color, x, y, SN_W, SN_H, true);
    if (n) {
        klog("notes: new note-%d", id);
        win_focus(n->win);
    }
    return n;
}

static void note_new_from(sn_t *src) {
    rect_t f = win_frame_rect(src->win);
    int x = f.x + 28, y = f.y + 28;
    if (x + SN_W > screen_width()) x = MAX(0, f.x - SN_W - 12);
    if (y + SN_H + HDR_H > screen_work_height()) y = 20;
    note_create_new(src->color, x, y, true);
}

static void parse_meta(const char *m, int *color, int *x, int *y, int *w, int *h) {
    const char *p = m;
    while (p && *p) {
        int v;
        if (sscanf(p, "color=%d", &v) == 1) *color = v;
        else if (sscanf(p, "x=%d", &v) == 1) *x = v;
        else if (sscanf(p, "y=%d", &v) == 1) *y = v;
        else if (sscanf(p, "w=%d", &v) == 1) *w = v;
        else if (sscanf(p, "h=%d", &v) == 1) *h = v;
        p = strchr(p, '\n');
        if (p) p++;
    }
}

/* Read a text file as note text: plain text only (CRs and control characters
   dropped, tabs expanded). NULL if it cannot be read. */
static char *read_note_text(const char *path) {
    uint32_t size = 0;
    char *text = vfs_read_file(path, &size);
    if (!text) return NULL;
    char *clean = malloc((size_t)size * 4 + 1);
    int o = 0;
    if (clean) {
        for (uint32_t i = 0; i < size && o < MAX_TEXT; i++) {
            unsigned char c = (unsigned char)text[i];
            if (c == '\r' || (c < 0x20 && c != '\n' && c != '\t')) continue;
            if (c == '\t') { memcpy(clean + o, "    ", 4); o += 4; continue; }
            clean[o++] = (char)c;
        }
        clean[o] = 0;
    }
    free(text);
    return clean;
}

/* Open any text file (e.g. a .note from Files) as a note saved back to it. */
static sn_t *note_open_file(const char *path) {
    sn_t *ex = note_find_path(path);
    if (ex) return ex;
    char *clean = read_note_text(path);
    if (!clean) return NULL;
    char *dup = strdup(path);
    if (!dup) { free(clean); return NULL; }
    int x, y;
    default_pos(SN_W, SN_H, &x, &y);
    sn_t *n = note_open(0, clean, 0, x, y, SN_W, SN_H, true);
    free(clean);
    if (!n) { free(dup); return NULL; }
    n->path = dup;
    klog("notes: opened %s", path);
    return n;
}

static sn_t *note_restore(int id) {
    sn_t *ex = note_find(id);
    if (ex) return ex;
    char txt[VFS_PATH_MAX], meta[VFS_PATH_MAX];
    note_paths(id, txt, meta);
    uint32_t size = 0;
    char *clean = read_note_text(txt);
    if (!clean) return NULL;
    int color = 0, x = 0, y = 0, w = SN_W, h = SN_H;
    bool place = false;
    char *m = vfs_read_file(meta, &size);
    if (m) {
        x = INT32_MIN;
        parse_meta(m, &color, &x, &y, &w, &h);
        place = x != INT32_MIN;
        free(m);
    }
    if (!place) { default_pos(w, h, &x, &y); place = true; }
    sn_t *n = note_open(id, clean, color, x, y, w, h, place);
    free(clean);
    return n;
}

/* Restore every saved note that is not open. Returns how many were opened. */
static int notes_restore_all(void) {
    int cnt = vfs_list(NOTES_DIR, NULL, 0);
    if (cnt <= 0) return 0;
    vfs_dirent_t *e = malloc(sizeof(vfs_dirent_t) * (size_t)cnt);
    if (!e) return 0;
    int got = MIN(vfs_list(NOTES_DIR, e, cnt), cnt), opened = 0;
    int ids[256], nids = 0;
    for (int i = 0; i < got && nids < 256; i++) {
        int id = note_id_from_name(e[i].name);
        if (id > 0 && !e[i].is_dir && !note_find(id)) ids[nids++] = id;
    }
    free(e);
    /* oldest first, so the newest note ends up on top */
    for (int i = 1; i < nids; i++)
        for (int j = i; j > 0 && ids[j - 1] > ids[j]; j--) { int t = ids[j]; ids[j] = ids[j - 1]; ids[j - 1] = t; }
    for (int i = 0; i < nids; i++)
        if (note_restore(ids[i])) opened++;
    klog("notes: restored %d note%s", opened, opened == 1 ? "" : "s");
    return opened;
}

/* ---- delete -------------------------------------------------------------------------------- */
static void on_delete_answer(int result, void *ctx) {
    sn_t *n = ctx;
    if (result != MB_RESULT_YES) return;
    n->deleted = true;
    if (n->path) { vfs_remove(n->path); klog("notes: deleted %s", n->path); }
    else { note_remove_files(n->id); klog("notes: deleted note-%d", n->id); }
    win_close(n->win);
}

static void note_request_delete(sn_t *n) {
    if (note_blank(n) && !n->path) {
        n->deleted = true;
        note_remove_files(n->id);
        win_close(n->win);
        return;
    }
    ui_message_box(n->win, "Delete note", n->path ? "Delete this note and its file?\nIts text will be gone for good."
                                                  : "Delete this note?\nIts text will be gone for good.",
                   MB_YESNO | MB_ICON_QUESTION, on_delete_answer, n);
}

/* ---- launch ------------------------------------------------------------------------------- */
static void notes_launch(const char *arg) {
    if (arg && *arg) {
        if (!strcmp(arg, "new")) { note_create_new(0, 0, 0, false); return; }
        int id = note_id_from_name(vfs_basename(arg));
        char dir[VFS_PATH_MAX];
        vfs_dirname(arg, dir);
        if (id > 0 && !strcmp(dir, NOTES_DIR)) {
            sn_t *n = note_restore(id);
            if (n) { win_focus(n->win); return; }
        } else if (vfs_exists(arg) && !vfs_is_dir(arg)) {
            sn_t *n = note_open_file(arg);
            if (n) { win_focus(n->win); return; }
        }
    }
    if (g_notes) {
        /* notes are already up: launching again means "another note" */
        sn_t *last = g_notes;
        note_new_from(last);
        return;
    }
    if (!notes_restore_all()) note_create_new(0, 0, 0, false);
}

/* Icon: a yellow note with a folded corner over a tilted blue one. */
static void notes_icon(surface_t *s, int x, int y, int size) {
    double S = size;
    point_t back[4] = {
        { x + (int)(S * 0.30), y + (int)(S * 0.06) }, { x + (int)(S * 0.94), y + (int)(S * 0.16) },
        { x + (int)(S * 0.84), y + (int)(S * 0.78) }, { x + (int)(S * 0.20), y + (int)(S * 0.68) },
    };
    gfx_fill_polygon(s, back, 4, RGB(120, 186, 245));
    int fx = x + (int)(S * 0.08), fy = y + (int)(S * 0.22), fw = (int)(S * 0.70), fh = (int)(S * 0.70);
    int fold = (int)(S * 0.20);
    /* shadow */
    point_t sh[5] = { { fx + 2, fy + 3 }, { fx + fw + 2, fy + 3 }, { fx + fw + 2, fy + fh - fold + 3 },
                      { fx + fw - fold + 2, fy + fh + 3 }, { fx + 2, fy + fh + 3 } };
    gfx_fill_polygon(s, sh, 5, RGBA(0, 0, 0, 60));
    point_t paper[5] = { { fx, fy }, { fx + fw, fy }, { fx + fw, fy + fh - fold },
                         { fx + fw - fold, fy + fh }, { fx, fy + fh } };
    gfx_fill_polygon(s, paper, 5, RGB(255, 236, 120));
    gfx_fill_rect(s, fx, fy, fw, (int)(S * 0.12), RGB(255, 214, 72));
    /* folded corner */
    gfx_fill_triangle(s, fx + fw, fy + fh - fold, fx + fw - fold, fy + fh, fx + fw - fold, fy + fh - fold,
                      RGB(222, 190, 70));
    /* scribbled lines */
    int lx = fx + (int)(S * 0.09), lw = fw - (int)(S * 0.18);
    int lh = MAX(1, size / 24);
    for (int i = 0; i < 3; i++) {
        int ly = fy + (int)(S * 0.24) + i * (int)(S * 0.12);
        int w = i == 2 ? lw * 5 / 9 : lw;
        gfx_fill_rounded_rect(s, lx, ly, w, lh + 1, (lh + 1) / 2, RGB(150, 120, 40));
    }
}

REGISTER_APP(.name = "Sticky Notes", .category = CAT_ACCESSORIES,
             .description = "Little coloured notes that stay on your desktop",
             .launch = notes_launch, .draw_icon = notes_icon);
