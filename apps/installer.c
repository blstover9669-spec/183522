/*
 * installer.c - "Installer": install NovaOS onto a hard disk.
 *
 * A wizard: Welcome -> Choose disk -> Drive size -> Confirm -> Installing ->
 * Finished. The drive sizes are the same as on the NovaOS Drive installation
 * screen of ISO boots (100 MB ... 10 GB, install_size_*); sizes bigger than the
 * disk are greyed out and only the chosen size of the disk is used.
 * The heavy lifting is done by the kernel's installer back end
 * (<nova/disk.h>: install_begin / install_step); this app drives it from a
 * window timer in slices of ~12 ms so the desktop stays responsive, and
 * shows the progress.
 *
 * autorun=Installer:auto installs to the first suitable disk without
 * asking (used by automated tests) and logs "INSTALL: done" / "INSTALL: failed";
 * it takes the largest size that fits (the whole disk when none does).
 */
#include <nova.h>
#include <nova/disk.h>

#define W        660
#define H        450
#define SIDE_W   188
#define FOOT_H   60
#define PAD      28
#define CX       (SIDE_W + PAD)            /* content x       */
#define CW       (W - SIDE_W - 2 * PAD)    /* content width   */
#define CARD_H   68
#define LIST_Y   104
#define LIST_H   232

enum { PG_WELCOME, PG_DISK, PG_SIZE, PG_CONFIRM, PG_INSTALL, PG_DONE, PG_FAILED };
#define NSTEPS 6
#define TILE_GAP 12

typedef struct {
    window_t *win;
    int page;
    int sel;                    /* selected disk index, -1 = none */
    int size;                   /* selected drive size (install_size_*), -1 = none */
    int hover, size_hover;
    bool auto_mode;
    install_job_t *job;
    int timer;
    int result;                 /* last install_step result */
    uint32_t started;
    uint32_t finished_ms;
    double spin;
    char error[200];
    widget_t *back, *next, *cancel, *rescan, *copy, *list, *sizes, *progress, *shutdown;
} inst_t;

static const char *const step_names[] = { "Welcome", "Choose a disk", "Drive size", "Confirm", "Install", "Finish" };
static const char *const phase_list[] = { "Writing the boot loader", "Copying NovaOS", "Copying your files",
                                          "Preparing file storage", "Verifying", "Making the disk bootable" };

static void set_page(inst_t *in, int page);

/* ---- helpers ------------------------------------------------------------------------ */
/* A disk can take NovaOS when the back end accepts it and at least the smallest size fits. */
static bool disk_ok(int i) { return install_check_disk(i, NULL, 0) == DISK_OK && install_size_default(i) >= 0; }

static bool size_fits(const inst_t *in, int i) {
    return in->sel >= 0 && i >= 0 && i < install_size_count() && install_size_fits(in->sel, install_size_bytes(i));
}

static void pick_default_size(inst_t *in) {
    if (!size_fits(in, in->size)) in->size = in->sel >= 0 ? install_size_default(in->sel) : -1;
}

static void size_str(uint64_t bytes, char *out, int n) { disk_format_size(bytes, out, n); }

static uint32_t text_dim(const theme_t *t) { return color_mix(t->window_text, t->window_bg, 80); }

static void pick_default_disk(inst_t *in) {
    in->sel = -1;
    for (int i = 0; i < disk_count(); i++)
        if (disk_ok(i)) { in->sel = i; break; }
}

/* Hard drive drawing shared by the icon and the disk cards. */
static void draw_drive(surface_t *s, int x, int y, int w, int h, bool badge) {
    int r = MAX(2, h / 6);
    gfx_fill_rounded_gradient_v(s, x, y, w, h, r, RGB(196, 204, 218), RGB(128, 138, 156));
    gfx_rounded_rect(s, x, y, w, h, r, RGBA(40, 48, 64, 160));
    /* platter window */
    int pr = h * 32 / 100;
    int pcx = x + w * 38 / 100, pcy = y + h / 2;
    gfx_fill_circle_aa(s, pcx, pcy, pr + 1, RGBA(60, 70, 90, 120));
    gfx_fill_circle_aa(s, pcx, pcy, pr, RGB(226, 232, 242));
    gfx_circle_aa(s, pcx, pcy, pr * 0.62, MAX(1.0, h / 40.0), RGBA(120, 130, 150, 140));
    gfx_fill_circle_aa(s, pcx, pcy, MAX(1.5, pr * 0.18), RGB(90, 100, 120));
    /* actuator arm */
    gfx_thick_line(s, x + w * 78 / 100, y + h * 26 / 100, pcx + pr / 3, pcy - pr / 4, MAX(1, h / 14), RGB(70, 78, 96));
    gfx_fill_circle_aa(s, x + w * 78 / 100, y + h * 26 / 100, MAX(1.5, h / 11.0), RGB(70, 78, 96));
    /* activity LED */
    gfx_fill_circle_aa(s, x + w * 84 / 100, y + h * 76 / 100, MAX(1.2, h / 16.0), RGB(80, 220, 120));
    (void)badge;
}

/* ---- icon ---------------------------------------------------------------------------- */
static void installer_icon(surface_t *s, int x, int y, int size) {
    int dw = size * 84 / 100, dh = size * 50 / 100;
    draw_drive(s, x + size * 4 / 100, y + size * 30 / 100, dw, dh, false);
    /* blue badge with a down arrow (bottom right) */
    int br = size * 22 / 100;
    int bx = x + size - br - size / 32, by = y + size - br - size / 32;
    gfx_fill_circle_aa(s, bx, by, br + MAX(1, size / 24), COLOR_WHITE);
    gfx_fill_circle_aa(s, bx, by, br, RGB(36, 118, 234));
    int aw = br * 9 / 10;
    int stem = MAX(2, br * 2 / 5);
    gfx_fill_rect(s, bx - stem / 2, by - br * 6 / 10, stem, br * 7 / 10, COLOR_WHITE);
    gfx_fill_triangle(s, bx - aw * 2 / 3, by, bx + aw * 2 / 3, by, bx, by + br * 6 / 10, COLOR_WHITE);
}

/* ---- disk list (custom widget) ------------------------------------------------------------ */
/* card height: CARD_H, smaller when four disks have to fit */
static int card_h(void) {
    int n = disk_count();
    if (n <= 0) return CARD_H;
    return MIN(CARD_H, (LIST_H - 8 * (n - 1)) / n);
}

static void list_draw(widget_t *w, surface_t *s, rect_t r) {
    inst_t *in = ui_get_ctx(w);
    const theme_t *t = theme_get();
    int n = disk_count(), ch = card_h();
    if (n == 0) {
        ui_draw_panel(s, r);
        ui_draw_glyph(s, UI_GLYPH_WARNING, r.x + 18, r.y + 18, 28, RGB(230, 160, 40));
        gfx_text(s, r.x + 58, r.y + 18, "No hard disks found", t->window_text, FONT_UI_BOLD);
        gfx_text_wrapped(s, rect_make(r.x + 58, r.y + 40, r.w - 76, r.h - 50),
                         "NovaOS needs an IDE hard disk to install to. In VirtualBox: shut down, open "
                         "Settings > Storage, add a new hard disk (VDI, 100 MB or more) to the IDE "
                         "controller, start the VM again and click Rescan.",
                         text_dim(t), FONT_UI);
        return;
    }
    for (int i = 0; i < n; i++) {
        const disk_info_t *d = disk_get(i);
        rect_t c = rect_make(r.x, r.y + i * (ch + 8), r.w, ch);
        if (c.y + c.h > r.y + r.h) break;
        bool ok = disk_ok(i), sel = i == in->sel, hov = i == in->hover && ok;
        uint32_t bg = sel ? color_mix(t->window_bg, t->accent, t->dark ? 60 : 36)
                          : hov ? color_mix(t->window_bg, t->accent, 16) : t->panel_bg;
        gfx_fill_rounded_rect(s, c.x, c.y, c.w, c.h, 8, bg);
        gfx_rounded_rect(s, c.x, c.y, c.w, c.h, 8, sel ? t->accent : t->window_border);
        if (sel) gfx_rounded_rect(s, c.x + 1, c.y + 1, c.w - 2, c.h - 2, 7, color_alpha(t->accent, 120));
        draw_drive(s, c.x + 14, c.y + (ch - 36) / 2, 56, 36, false);
        int ty = c.y + (ch - 44) / 2;
        uint32_t tc = ok ? t->window_text : t->disabled_text;
        char size[24], line[96];
        size_str(d->sectors * 512, size, sizeof size);
        gfx_text_ellipsis(s, c.x + 84, ty + 1, c.w - 84 - 90, d->model, tc, FONT_UI_BOLD);
        gfx_text_aligned(s, rect_make(c.x, ty - 1, c.w - 16, 20), size, tc, FONT_UI_LARGE, ALIGN_RIGHT);
        /* short status after the location; the long reasons would be cut off */
        char why[96] = "", need[24];
        int code = ok ? DISK_OK : install_check_disk(i, why, sizeof why);
        if (install_is_boot_disk(i)) {
            snprintf(line, sizeof line, "%s  \x95  System disk (in use)", d->location);
        } else if (code == DISK_ENOSPC || (code == DISK_OK && !ok)) {
            size_str(install_size_bytes(0), need, sizeof need);
            snprintf(line, sizeof line, "%s  \x95  Too small (needs %s)", d->location, need);
        } else if (why[0]) {
            snprintf(line, sizeof line, "%s  \x95  %s", d->location, why);
        } else if (install_disk_has_novaos(i)) {
            snprintf(line, sizeof line, "%s  \x95  NovaOS is installed here", d->location);
        } else {
            snprintf(line, sizeof line, "%s  \x95  IDE hard disk", d->location);
        }
        gfx_text_ellipsis(s, c.x + 84, ty + 24, c.w - 120, line, ok ? text_dim(t) : t->disabled_text, FONT_UI);
        if (sel) {
            gfx_fill_circle_aa(s, c.x + c.w - 22, ty + 32, 9, t->accent);
            ui_draw_glyph(s, UI_GLYPH_CHECK, c.x + c.w - 29, ty + 25, 14, t->accent_text);
        }
    }
}

static int list_hit(widget_t *w, int x, int y) {
    rect_t r = ui_rect(w);
    if (x < r.x || x >= r.x + r.w) return -1;
    int rel = y - r.y;
    if (rel < 0) return -1;
    int ch = card_h();
    int i = rel / (ch + 8);
    if (rel % (ch + 8) >= ch || i >= disk_count()) return -1;
    return i;
}

static void update_buttons(inst_t *in);

static bool list_event(widget_t *w, const event_t *ev) {
    inst_t *in = ui_get_ctx(w);
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int h = list_hit(w, ev->x, ev->y);
        if (h != in->hover) { in->hover = h; ui_invalidate(w); }
        return true;
    }
    case EV_MOUSE_LEAVE:
        if (in->hover != -1) { in->hover = -1; ui_invalidate(w); }
        return false;
    case EV_MOUSE_DOWN: {
        int h = list_hit(w, ev->x, ev->y);
        if (h >= 0 && disk_ok(h)) { in->sel = h; ui_invalidate(w); update_buttons(in); }
        else if (h >= 0) sys_beep(220, 60);
        return true;
    }
    case EV_DOUBLE_CLICK: {
        int h = list_hit(w, ev->x, ev->y);
        if (h >= 0 && h == in->sel) set_page(in, PG_SIZE);
        return true;
    }
    case EV_KEY_DOWN:
        if (ev->key == KEY_UP || ev->key == KEY_DOWN) {
            int d = ev->key == KEY_UP ? -1 : 1;
            for (int i = in->sel + d; i >= 0 && i < disk_count(); i += d)
                if (disk_ok(i)) { in->sel = i; ui_invalidate(w); update_buttons(in); break; }
            return true;
        }
        return false;
    default:
        return false;
    }
}

/* ---- drive sizes (custom widget): a 3 x 3 grid of tiles ----------------------------------- */
static rect_t tile_rect(widget_t *w, int i) {
    rect_t r = ui_rect(w);
    int cols = 3, rows = (install_size_count() + cols - 1) / cols;
    int tw = (r.w - (cols - 1) * TILE_GAP) / cols, th = (r.h - (rows - 1) * TILE_GAP) / rows;
    return rect_make(r.x + (i % cols) * (tw + TILE_GAP), r.y + (i / cols) * (th + TILE_GAP), tw, th);
}

static void sizes_draw(widget_t *w, surface_t *s, rect_t r) {
    inst_t *in = ui_get_ctx(w);
    const theme_t *t = theme_get();
    (void)r;
    for (int i = 0; i < install_size_count(); i++) {
        rect_t c = tile_rect(w, i);
        bool ok = size_fits(in, i), sel = ok && i == in->size, hov = ok && i == in->size_hover;
        uint32_t bg = sel ? color_mix(t->window_bg, t->accent, t->dark ? 60 : 36)
                          : hov ? color_mix(t->window_bg, t->accent, 16) : t->panel_bg;
        gfx_fill_rounded_rect(s, c.x, c.y, c.w, c.h, 8, ok ? bg : color_mix(t->panel_bg, t->window_bg, 128));
        gfx_rounded_rect(s, c.x, c.y, c.w, c.h, 8, sel ? t->accent : t->window_border);
        if (sel) gfx_rounded_rect(s, c.x + 1, c.y + 1, c.w - 2, c.h - 2, 7, color_alpha(t->accent, 120));
        if (sel && ui_has_focus(w))
            gfx_rounded_rect(s, c.x - 3, c.y - 3, c.w + 6, c.h + 6, 10, color_alpha(t->accent, 150));
        gfx_text_aligned(s, rect_make(c.x, c.y, c.w, c.h - 14), install_size_label(i),
                         ok ? t->window_text : t->disabled_text, FONT_UI_LARGE, ALIGN_CENTER | ALIGN_MIDDLE);
        const char *sub = ok ? (sel ? "Selected" : "") : "Bigger than the disk";
        if (*sub) gfx_text_aligned(s, rect_make(c.x, c.y + c.h / 2 + 6, c.w, 18), sub,
                                   sel ? t->accent : t->disabled_text, FONT_UI, ALIGN_CENTER);
    }
}

static int sizes_hit(widget_t *w, int x, int y) {
    for (int i = 0; i < install_size_count(); i++)
        if (rect_contains(tile_rect(w, i), x, y)) return i;
    return -1;
}

static bool sizes_event(widget_t *w, const event_t *ev) {
    inst_t *in = ui_get_ctx(w);
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int h = sizes_hit(w, ev->x, ev->y);
        if (h != in->size_hover) { in->size_hover = h; ui_invalidate(w); }
        return true;
    }
    case EV_MOUSE_LEAVE:
        if (in->size_hover != -1) { in->size_hover = -1; ui_invalidate(w); }
        return false;
    case EV_MOUSE_DOWN: {
        int h = sizes_hit(w, ev->x, ev->y);
        if (h >= 0 && size_fits(in, h)) { in->size = h; ui_invalidate(w); update_buttons(in); win_invalidate(in->win); }
        else if (h >= 0) sys_beep(220, 60);
        return true;
    }
    case EV_DOUBLE_CLICK: {
        int h = sizes_hit(w, ev->x, ev->y);
        if (h >= 0 && h == in->size && size_fits(in, h)) set_page(in, PG_CONFIRM);
        return true;
    }
    case EV_KEY_DOWN: {
        int d = ev->key == KEY_LEFT ? -1 : ev->key == KEY_RIGHT ? 1 : ev->key == KEY_UP ? -3 : ev->key == KEY_DOWN ? 3 : 0;
        if (!d) return false;
        for (int i = in->size + d; i >= 0 && i < install_size_count(); i += d)
            if (size_fits(in, i)) { in->size = i; ui_invalidate(w); update_buttons(in); win_invalidate(in->win); break; }
        return true;
    }
    default:
        return false;
    }
}

/* ---- painting ------------------------------------------------------------------------------- */
static void paint_sidebar(inst_t *in, surface_t *s, const theme_t *t) {
    uint32_t top = color_mix(RGB(22, 30, 58), t->accent, 70), bot = RGB(14, 18, 36);
    gfx_gradient_v(s, 0, 0, SIDE_W, H, top, bot);
    /* soft decorative glow */
    gfx_fill_circle_aa(s, SIDE_W - 20, H - 60, 90, RGBA(255, 255, 255, 10));
    gfx_fill_circle_aa(s, 30, H - 10, 60, RGBA(255, 255, 255, 8));
    ui_draw_logo(s, 22, 24, 40);
    gfx_text(s, 72, 26, "NovaOS", RGB(244, 247, 255), FONT_UI_LARGE);
    gfx_text(s, 72, 49, "Setup", RGB(170, 182, 214), FONT_UI);

    int cur = in->page;
    if (cur == PG_FAILED) cur = PG_INSTALL;
    for (int i = 0; i < NSTEPS; i++) {
        int y = 104 + i * 40;
        bool done = i < cur || (in->page == PG_DONE && i == PG_DONE);
        bool active = i == cur && in->page != PG_DONE;
        if (active) gfx_fill_rounded_rect(s, 12, y - 8, SIDE_W - 24, 34, 8, RGBA(255, 255, 255, 30));
        int cx = 32, cy = y + 9;
        if (done) {
            gfx_fill_circle_aa(s, cx, cy, 10, RGB(72, 200, 130));
            ui_draw_glyph(s, UI_GLYPH_CHECK, cx - 7, cy - 7, 14, COLOR_WHITE);
        } else if (active) {
            gfx_fill_circle_aa(s, cx, cy, 10, t->accent);
            char num[4];
            snprintf(num, sizeof num, "%d", i + 1);
            gfx_text_aligned(s, rect_make(cx - 10, cy - 10, 20, 20), num, COLOR_WHITE, FONT_UI_BOLD, ALIGN_CENTER | ALIGN_MIDDLE);
        } else {
            gfx_circle_aa(s, cx, cy, 9.5, 1.5, RGBA(255, 255, 255, 90));
            char num[4];
            snprintf(num, sizeof num, "%d", i + 1);
            gfx_text_aligned(s, rect_make(cx - 10, cy - 10, 20, 20), num, RGBA(255, 255, 255, 140), FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
        }
        uint32_t c = active ? RGB(255, 255, 255) : done ? RGB(200, 210, 232) : RGB(140, 152, 184);
        gfx_text(s, 52, y + 1, step_names[i], c, active ? FONT_UI_BOLD : FONT_UI);
    }
    gfx_text(s, 22, H - 34, sys_version(), RGB(120, 132, 168), FONT_UI);
}

static int paint_title(surface_t *s, const theme_t *t, const char *title, const char *sub) {
    gfx_text(s, CX, 26, title, t->window_text, FONT_UI_TITLE);
    int y = 70;
    if (sub) y += gfx_text_wrapped(s, rect_make(CX, y, CW, 60), sub, text_dim(t), FONT_UI) + 4;
    return y;
}

/* A bullet point; long text wraps. Returns the vertical space used, so the
   next bullet starts below all of this one's lines. */
static int bullet(surface_t *s, const theme_t *t, int y, const char *text) {
    gfx_fill_circle_aa(s, CX + 10, y + 8, 3, t->accent);
    int h = gfx_text_wrapped(s, rect_make(CX + 24, y, CW - 24, 80), text, t->window_text, FONT_UI);
    return MAX(h, font_height(FONT_UI)) + 5;
}

static void paint_welcome(inst_t *in, surface_t *s, const theme_t *t) {
    int y = paint_title(s, t, "Install NovaOS",
                        "This wizard copies NovaOS onto a hard disk. Afterwards your computer starts "
                        "NovaOS straight from the disk - no CD needed - and your files are kept when "
                        "you restart or shut down.");
    /* hero illustration: CD -> arrow -> disk */
    int hy = y + 14;
    ui_draw_panel(s, rect_make(CX, hy, CW, 86));
    int ccx = CX + 92, ccy = hy + 43;
    gfx_fill_circle_aa(s, ccx, ccy, 32, RGB(210, 216, 230));
    for (int i = 0; i < 6; i++)
        gfx_circle_aa(s, ccx, ccy, 14 + i * 3, 1.2, color_hsv(200 + i * 25, 90, 242));
    gfx_fill_circle_aa(s, ccx, ccy, 9, t->panel_bg);
    gfx_circle_aa(s, ccx, ccy, 9, 1, RGBA(0, 0, 0, 60));
    int ax0 = ccx + 50, ax1 = CX + CW - 150;
    for (int x = ax0; x < ax1 - 10; x += 12) gfx_fill_circle_aa(s, x, ccy, 2.5, color_alpha(t->accent, 200));
    gfx_fill_triangle(s, ax1 - 12, ccy - 8, ax1 - 12, ccy + 8, ax1, ccy, t->accent);
    draw_drive(s, CX + CW - 130, ccy - 24, 84, 48, false);

    y = hy + 86 + 14;
    gfx_text(s, CX, y, "Before you start", t->window_text, FONT_UI_BOLD);
    y += 22;
    char req[128];
    snprintf(req, sizeof req, "You need a hard disk of at least %s. You choose how much of it NovaOS uses "
             "(%s to %s).", install_size_label(0), install_size_label(0), install_size_label(install_size_count() - 1));
    y += bullet(s, t, y, req);
    y += bullet(s, t, y, "Everything on the disk you choose will be erased.");
    y += bullet(s, t, y, "Installing takes less than a minute.");
    if (persist_active()) {
        y += 2;
        ui_draw_glyph(s, UI_GLYPH_INFO, CX, y, 18, t->accent);
        gfx_text_wrapped(s, rect_make(CX + 26, y, CW - 26, H - FOOT_H - y),
                         "NovaOS is running from a hard disk. You can install it on another disk.",
                         text_dim(t), FONT_UI);
    }
    (void)in;
}

static void paint_confirm(inst_t *in, surface_t *s, const theme_t *t) {
    const disk_info_t *d = disk_get(in->sel);
    int y = paint_title(s, t, "Ready to install", "Please check the details below. Click Install to start.");
    if (!d) return;
    char size[24], buf[160];
    size_str(d->sectors * 512, size, sizeof size);

    /* warning banner */
    rect_t wb = rect_make(CX, y + 4, CW, 62);
    gfx_fill_rounded_rect(s, wb.x, wb.y, wb.w, wb.h, 8, t->dark ? RGB(74, 52, 20) : RGB(255, 243, 222));
    gfx_rounded_rect(s, wb.x, wb.y, wb.w, wb.h, 8, t->dark ? RGB(150, 104, 40) : RGB(240, 190, 110));
    ui_draw_glyph(s, UI_GLYPH_WARNING, wb.x + 14, wb.y + 16, 30, RGB(232, 150, 30));
    snprintf(buf, sizeof buf, "All data on %s (%s) will be erased.", d->model, size);
    gfx_text_ellipsis(s, wb.x + 56, wb.y + 13, wb.w - 70, buf, t->window_text, FONT_UI_BOLD);
    gfx_text(s, wb.x + 56, wb.y + 33, "This cannot be undone.", text_dim(t), FONT_UI);

    /* summary */
    y = wb.y + wb.h + 18;
    ui_draw_panel(s, rect_make(CX, y, CW, 172));
    int ly = y + 14;
    const int kx = CX + 16, vx = CX + 150;
    struct { const char *k; char v[96]; } rows[6];
    int nr = 0;
    rows[nr].k = "Disk";
    snprintf(rows[nr++].v, sizeof rows[0].v, "%s, %s", d->model, size);
    rows[nr].k = "Drive size";
    if (in->size >= 0) snprintf(rows[nr++].v, sizeof rows[0].v, "%s of the %s disk", install_size_label(in->size), size);
    else snprintf(rows[nr++].v, sizeof rows[0].v, "The whole disk (%s)", size);
    rows[nr].k = "Location";
    snprintf(rows[nr++].v, sizeof rows[0].v, "%s (IDE)", d->location);
    char sys[24];
    size_str(install_system_bytes(), sys, sizeof sys);
    rows[nr].k = "System";
    snprintf(rows[nr++].v, sizeof rows[0].v, "%s + boot loader (%s)", sys_version(), sys);
    rows[nr].k = "Your files";
    if (ui_get_value(in->copy)) {
        char fs[24];
        size_str(vfs_total_bytes(), fs, sizeof fs);
        snprintf(rows[nr++].v, sizeof rows[0].v, "Copied to the new system (%s)", fs);
    } else {
        snprintf(rows[nr++].v, sizeof rows[0].v, "Start with the default files");
    }
    rows[nr].k = "Saving";
    snprintf(rows[nr++].v, sizeof rows[0].v, "Automatic, a few seconds after changes");
    for (int i = 0; i < nr; i++) {
        gfx_text(s, kx, ly + i * 26, rows[i].k, text_dim(t), FONT_UI);
        gfx_text_ellipsis(s, vx, ly + i * 26, CX + CW - vx - 14, rows[i].v, t->window_text, FONT_UI);
        if (i < nr - 1) gfx_hline(s, kx, CX + CW - 16, ly + i * 26 + 21, color_alpha(t->window_border, 120));
    }
}

static void spinner(surface_t *s, int cx, int cy, double a, uint32_t c) {
    for (int i = 0; i < 8; i++) {
        double ang = a + i * M_PI / 4;
        int alpha = 40 + i * 26;
        gfx_fill_circle_aa(s, cx + 7 * cos(ang), cy + 7 * sin(ang), 1.8, color_alpha(c, alpha > 255 ? 255 : alpha));
    }
}

static void paint_install(inst_t *in, surface_t *s, const theme_t *t) {
    paint_title(s, t, "Installing NovaOS", "Please wait while NovaOS is copied to the disk. Do not turn off the computer.");
    int p = install_progress(in->job);
    const char *phase = install_phase(in->job);
    char buf[128], a[24], b[24];
    gfx_text(s, CX, 128, phase, t->window_text, FONT_UI_BOLD);
    snprintf(buf, sizeof buf, "%d%%", p / 10);
    gfx_text_aligned(s, rect_make(CX, 124, CW, 22), buf, t->accent, FONT_UI_LARGE, ALIGN_RIGHT);
    size_str(install_bytes_done(in->job), a, sizeof a);
    size_str(install_bytes_total(in->job), b, sizeof b);
    snprintf(buf, sizeof buf, "%s of %s", a, b);
    gfx_text(s, CX, 180, buf, text_dim(t), FONT_UI);

    /* phase checklist */
    int cur = 6;              /* "Finished" */
    for (int k = 0; k < 6; k++)
        if (!strcmp(phase, phase_list[k])) cur = k;
    int y = 214;
    for (int i = 0; i < 6; i++) {
        int state = i < cur ? 2 : i == cur ? 1 : 0;     /* 0 pending, 1 active, 2 done */
        bool skipped = i == 2 && !ui_get_value(in->copy);
        int cy = y + i * 28 + 9;
        if (skipped) {
            gfx_circle_aa(s, CX + 9, cy, 7.5, 1.2, t->disabled_text);
            gfx_hline(s, CX + 5, CX + 13, cy, t->disabled_text);
        } else if (state == 2) {
            gfx_fill_circle_aa(s, CX + 9, cy, 9, RGB(60, 186, 116));
            ui_draw_glyph(s, UI_GLYPH_CHECK, CX + 3, cy - 6, 12, COLOR_WHITE);
        } else if (state == 1) {
            spinner(s, CX + 9, cy, in->spin, t->accent);
        } else {
            gfx_circle_aa(s, CX + 9, cy, 7.5, 1.2, color_mix(t->window_border, t->window_text, 40));
        }
        uint32_t c = skipped ? t->disabled_text : state == 1 ? t->window_text : state == 2 ? text_dim(t)
                                                                                    : t->disabled_text;
        gfx_text(s, CX + 28, cy - 8, phase_list[i], c, state == 1 ? FONT_UI_BOLD : FONT_UI);
    }
}

static void paint_done(inst_t *in, surface_t *s, const theme_t *t) {
    int cx = CX + 34, cy = 60;
    gfx_fill_circle_aa(s, cx, cy, 30, RGB(60, 186, 116));
    gfx_fill_circle_aa(s, cx, cy, 36, RGBA(60, 186, 116, 40));
    ui_draw_glyph(s, UI_GLYPH_CHECK, cx - 16, cy - 16, 32, COLOR_WHITE);
    gfx_text(s, CX + 84, 34, "NovaOS is installed", t->window_text, FONT_UI_TITLE);
    const disk_info_t *d = disk_get(in->sel);
    char buf[160];
    snprintf(buf, sizeof buf, "Installed on %s in %u.%u seconds.", d ? d->model : "the disk",
             in->finished_ms / 1000, (in->finished_ms % 1000) / 100);
    gfx_text(s, CX + 86, 72, buf, text_dim(t), FONT_UI);

    int y = 124;
    gfx_text(s, CX, y, "To start NovaOS from the hard disk:", t->window_text, FONT_UI_BOLD);
    y += 30;
    static const char *steps[] = {
        "Click Restart now. The computer starts NovaOS from the hard disk (with the NovaOS CD "
        "still inserted, its boot menu picks the installed drive by itself).",
        "Your files are saved to the disk automatically, and before every restart or shut down.",
    };
    for (int i = 0; i < ARRAY_SIZE(steps); i++) {
        gfx_fill_circle_aa(s, CX + 11, y + 9, 11, color_mix(t->window_bg, t->accent, 50));
        char num[4];
        snprintf(num, sizeof num, "%d", i + 1);
        gfx_text_aligned(s, rect_make(CX, y - 2, 22, 22), num, t->window_text, FONT_UI_BOLD, ALIGN_CENTER | ALIGN_MIDDLE);
        int hgt = gfx_text_wrapped(s, rect_make(CX + 34, y, CW - 34, 60), steps[i], t->window_text, FONT_UI);
        y += MAX(hgt, 20) + 16;
    }
}

static void paint_failed(inst_t *in, surface_t *s, const theme_t *t) {
    int cx = CX + 34, cy = 60;
    gfx_fill_circle_aa(s, cx, cy, 30, RGB(220, 70, 70));
    ui_draw_glyph(s, UI_GLYPH_CLOSE, cx - 14, cy - 14, 28, COLOR_WHITE);
    gfx_text(s, CX + 84, 34, "Installation failed", t->window_text, FONT_UI_TITLE);
    gfx_text(s, CX + 86, 72, "The disk was not made bootable.", text_dim(t), FONT_UI);
    ui_draw_panel(s, rect_make(CX, 120, CW, 90));
    gfx_text_wrapped(s, rect_make(CX + 16, 134, CW - 32, 64), in->error, t->window_text, FONT_UI);
    gfx_text_wrapped(s, rect_make(CX, 230, CW, 60),
                     "Click Back to try again or to choose another disk. If the problem persists, check that "
                     "the virtual disk is attached to the IDE controller and is not read-only.",
                     text_dim(t), FONT_UI);
}

static void installer_paint(inst_t *in) {
    surface_t *s = win_surface(in->win);
    const theme_t *t = theme_get();
    paint_sidebar(in, s, t);
    switch (in->page) {
    case PG_WELCOME: paint_welcome(in, s, t); break;
    case PG_DISK: paint_title(s, t, "Choose a disk", "Select the hard disk to install NovaOS on."); break;
    case PG_SIZE: {
        const disk_info_t *d = disk_get(in->sel);
        char sub[160], ds[24] = "";
        if (d) size_str(d->sectors * 512, ds, sizeof ds);
        snprintf(sub, sizeof sub, "Choose how much of the disk (%s) NovaOS uses. Sizes bigger than the disk are "
                 "greyed out.", ds);
        paint_title(s, t, "Drive size", sub);
        break;
    }
    case PG_CONFIRM: paint_confirm(in, s, t); break;
    case PG_INSTALL: paint_install(in, s, t); break;
    case PG_DONE: paint_done(in, s, t); break;
    case PG_FAILED: paint_failed(in, s, t); break;
    }
    /* footer */
    gfx_fill_rect(s, SIDE_W, H - FOOT_H, W - SIDE_W, FOOT_H, color_mix(t->window_bg, t->panel_bg, 160));
    gfx_hline(s, SIDE_W, W, H - FOOT_H, t->window_border);
}

/* ---- navigation ------------------------------------------------------------------------- */
static void update_buttons(inst_t *in) {
    bool installing = in->page == PG_INSTALL;
    ui_set_visible(in->back, in->page == PG_DISK || in->page == PG_SIZE || in->page == PG_CONFIRM ||
                             in->page == PG_FAILED);
    ui_set_visible(in->next, in->page != PG_FAILED);
    ui_set_visible(in->cancel, !installing);
    ui_set_visible(in->rescan, in->page == PG_DISK);
    ui_set_visible(in->copy, in->page == PG_DISK);
    ui_set_visible(in->list, in->page == PG_DISK);
    ui_set_visible(in->sizes, in->page == PG_SIZE);
    ui_set_visible(in->progress, installing);
    ui_set_visible(in->shutdown, in->page == PG_DONE);
    ui_set_text(in->cancel, in->page == PG_DONE ? "Close" : "Cancel");
    switch (in->page) {
    case PG_WELCOME: ui_set_text(in->next, "Next"); ui_set_enabled(in->next, true); break;
    case PG_DISK: ui_set_text(in->next, "Next"); ui_set_enabled(in->next, in->sel >= 0 && disk_ok(in->sel)); break;
    case PG_SIZE: ui_set_text(in->next, "Next"); ui_set_enabled(in->next, size_fits(in, in->size)); break;
    case PG_CONFIRM: ui_set_text(in->next, "Install"); ui_set_enabled(in->next, true); break;
    case PG_INSTALL: ui_set_text(in->next, "Next"); ui_set_enabled(in->next, false); break;
    case PG_DONE: ui_set_text(in->next, "Restart now"); ui_set_enabled(in->next, true); break;
    default: break;
    }
    win_invalidate(in->win);
}

static void install_tick(inst_t *in) {
    if (!in->job) return;
    int r = install_step(in->job, 12);
    in->spin += 0.35;
    ui_set_value(in->progress, install_progress(in->job));
    win_invalidate_rect(in->win, SIDE_W, 110, W - SIDE_W, H - FOOT_H - 110);
    if (r == 0) return;
    in->result = r;
    in->finished_ms = sys_uptime_ms() - in->started;
    win_kill_timer(in->win, in->timer);
    in->timer = 0;
    if (r == 1) {
        klog("INSTALL: done in %u ms", in->finished_ms);
        static const note_t chime[] = { { 784, 90 }, { 988, 90 }, { 1319, 180 } };
        sys_play_notes(chime, 3);
        set_page(in, PG_DONE);
    } else {
        strlcpy(in->error, install_error(in->job), sizeof in->error);
        klog("INSTALL: failed: %s", in->error);
        set_page(in, PG_FAILED);
    }
    install_free(in->job);
    in->job = NULL;
}

static void start_install(inst_t *in) {
    char err[160] = "";
    /* only the chosen size is used; automatic installs on a disk smaller than every size use all of it */
    uint64_t bytes = size_fits(in, in->size) ? install_size_bytes(in->size) : 0;
    in->job = install_begin_sized(in->sel, bytes, ui_get_value(in->copy) != 0, err, sizeof err);
    if (!in->job) {
        snprintf(in->error, sizeof in->error, "%s", err[0] ? err : "The installation could not be prepared.");
        klog("INSTALL: failed: %s", in->error);
        set_page(in, PG_FAILED);
        return;
    }
    in->started = sys_uptime_ms();
    in->timer = win_set_timer(in->win, 16);
    set_page(in, PG_INSTALL);
}

static void set_page(inst_t *in, int page) {
    if (page == PG_DISK && in->page != PG_SIZE) {
        if (in->sel < 0 || !disk_ok(in->sel)) pick_default_disk(in);
    }
    if (page == PG_SIZE) pick_default_size(in);
    in->page = page;
    update_buttons(in);
    if (page == PG_DISK) ui_focus(in->list);
    else if (page == PG_SIZE) ui_focus(in->sizes);
    else if (page != PG_INSTALL && ui_is_visible(in->next) && ui_is_enabled(in->next)) ui_focus(in->next);
    if (page == PG_INSTALL || page == PG_DONE) ui_button_set_default(in->next, true);
}

static void on_next(widget_t *w, void *ctx) {
    inst_t *in = ctx;
    (void)w;
    switch (in->page) {
    case PG_WELCOME: set_page(in, PG_DISK); break;
    case PG_DISK: if (in->sel >= 0 && disk_ok(in->sel)) set_page(in, PG_SIZE); break;
    case PG_SIZE: if (size_fits(in, in->size)) set_page(in, PG_CONFIRM); break;
    case PG_CONFIRM: start_install(in); break;
    case PG_DONE: sys_reboot(); break;
    default: break;
    }
}

static void on_back(widget_t *w, void *ctx) {
    inst_t *in = ctx;
    (void)w;
    if (in->page == PG_DISK) set_page(in, PG_WELCOME);
    else if (in->page == PG_SIZE || in->page == PG_FAILED) set_page(in, PG_DISK);
    else if (in->page == PG_CONFIRM) set_page(in, PG_SIZE);
}

static void on_cancel(widget_t *w, void *ctx) {
    inst_t *in = ctx;
    (void)w;
    if (in->page != PG_INSTALL) win_close(in->win);
}

static void on_shutdown(widget_t *w, void *ctx) {
    (void)w;
    (void)ctx;
    sys_shutdown();
}

static void on_rescan(widget_t *w, void *ctx) {
    inst_t *in = ctx;
    (void)w;
    int n = disk_rescan();
    pick_default_disk(in);
    char msg[64];
    snprintf(msg, sizeof msg, n == 1 ? "Found 1 hard disk." : "Found %d hard disks.", n);
    ui_notify("Installer", msg);
    update_buttons(in);
}

static void on_copy(widget_t *w, void *ctx) {
    (void)w;
    (void)ctx;
}

/* ---- window ------------------------------------------------------------------------------ */
static void installer_event(window_t *win, const event_t *ev) {
    inst_t *in = win_get_userdata(win);
    switch (ev->type) {
    case EV_PAINT:
        installer_paint(in);
        break;
    case EV_TIMER:
        install_tick(in);
        break;
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESCAPE && in->page != PG_INSTALL) win_close(win);
        break;
    case EV_CLOSE_REQUEST:
        if (in->page == PG_INSTALL) {
            sys_beep(220, 80);
            ui_notify("Installer", "Please wait until the installation has finished.");
        } else {
            win_close(win);
        }
        break;
    case EV_CLOSE:
        install_free(in->job);
        free(in);
        break;
    default:
        break;
    }
}

static void installer_launch(const char *arg) {
    inst_t *in = calloc(1, sizeof *in);
    if (!in) return;
    window_t *win = win_create("NovaOS Installer", W, H, WIN_CENTER | WIN_CONFIRM_CLOSE | WIN_NO_MAXIMIZE);
    if (!win) { free(in); return; }
    in->win = win;
    in->hover = -1;
    in->size = -1;
    in->size_hover = -1;
    in->auto_mode = arg && !strcmp(arg, "auto");
    win_set_userdata(win, in);
    win_set_event_handler(win, installer_event);

    int by = H - FOOT_H + 14;
    in->cancel = ui_button(win, CX, by, 96, 32, "Cancel", on_cancel, in);
    in->back = ui_button(win, W - PAD - 96 - 10 - 120, by, 96, 32, "Back", on_back, in);
    in->next = ui_button(win, W - PAD - 120, by, 120, 32, "Next", on_next, in);
    in->shutdown = ui_button(win, W - PAD - 120 - 10 - 110, by, 110, 32, "Shut down", on_shutdown, in);
    ui_button_set_default(in->next, true);
    in->list = ui_custom(win, CX, LIST_Y, CW, LIST_H, list_draw, list_event, in);
    in->sizes = ui_custom(win, CX, 132, CW, 228, sizes_draw, sizes_event, in);
    in->rescan = ui_button(win, CX + CW - 96, H - FOOT_H - 44, 96, 30, "Rescan", on_rescan, in);
    in->copy = ui_checkbox(win, CX, H - FOOT_H - 40, CW - 110, 24, "Copy my current files to the new system", true,
                           on_copy, in);
    in->progress = ui_progress(win, CX, 152, CW, 14, 0, 1000, 0);

    pick_default_disk(in);
    set_page(in, PG_WELCOME);

    if (in->auto_mode) {
        /* the first disk the back end accepts, even if it is smaller than every size */
        if (in->sel < 0)
            for (int i = 0; i < disk_count() && in->sel < 0; i++)
                if (install_check_disk(i, NULL, 0) == DISK_OK) in->sel = i;
        if (in->sel < 0) {
            klog("INSTALL: failed: no suitable disk");
            set_page(in, PG_DISK);
            return;
        }
        in->size = install_size_default(in->sel);
        in->page = PG_SIZE;
        update_buttons(in);
        set_page(in, PG_CONFIRM);
        start_install(in);
    }
}

REGISTER_APP(.name = "Installer", .category = CAT_SYSTEM,
             .description = "Install NovaOS on a hard disk",
             .launch = installer_launch, .draw_icon = installer_icon,
             .flags = APP_SINGLE);
