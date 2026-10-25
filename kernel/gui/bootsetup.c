/*
 * bootsetup.c - "NovaOS Drive installation": the screen an ISO boot shows when
 * the splash animation ends (NovaOS 2.5).
 *
 * The splash icon moves to the top, "NovaOS Drive installation" appears under
 * it, the drive icon in the middle and under that the controls to create the
 * drive: the storage sizes on the left (sizes bigger than the hard disk are
 * greyed out), Install and, under it, Continue ISO.
 *
 *   Install       the screen changes to a progress bar at the top with the
 *                 percentage in its middle. When the drive is written: "Your
 *                 drive has been installed and ready to have files savable",
 *                 then the computer restarts. The ISO's GRUB menu finds the
 *                 drive signature (kernel/disk/diskfmt.h) and boots the
 *                 installed NovaOS by default, so the restart goes straight
 *                 into it. Only the chosen size of the disk is used.
 *   Continue ISO  NovaOS keeps running from the ISO (nothing is saved); when
 *                 the desktop is up the save file screen (OSaver) follows.
 *
 * Without a hard disk the screen says so; Continue ISO still works.
 *
 * Like the splash it runs before the window manager's main loop: it reads the
 * input queue itself, draws into the back buffer and presents what changed.
 * The installation is the kernel's installer back end (install_begin_sized /
 * install_step), the same one the Installer app drives. Keyboard: Up/Down pick
 * a size, Tab moves between the sizes and the buttons, Enter/Space presses the
 * focused button, Left/Right switch the hard disk when there are several.
 */
#include "wm_internal.h"
#include "bootscreen.h"
#include <nova/disk.h>
#include <nova/osaver.h>

/* OSaver's save file screen may not be part of the build: call it only if it is. */
extern __typeof__(osaver_boot_prompt) osaver_boot_prompt __attribute__((weak));

#define MAX_SIZES   12
#define MOVE_MS     650       /* icon: middle -> top */
#define FADE_MS     260
#define FRAME_MS    14
#define DONE_MS     3200      /* finish text before the restart */

enum { E_NONE = 0, E_SIZE = 1 /* + index */, E_INSTALL = 40, E_CONTINUE, E_PREV, E_NEXT };
enum { F_SIZES, F_INSTALL, F_CONTINUE };
enum { A_NONE, A_INSTALL, A_CONTINUE };

#define C_TEXT      RGB(240, 243, 255)
#define C_DIM       RGB(156, 166, 198)
#define C_OFF       RGB(84, 92, 122)
#define C_ACCENT    RGB(59, 130, 246)
#define C_WARN      RGB(244, 184, 96)

static struct {
    int w, h;
    double k;                  /* layout scale (1.0 at 768 lines)            */
    surface_t *base;           /* background + the icon at the top           */
    surface_t *scene;          /* base + title, drive, controls (no pointer) */
    surface_t *drive_img;      /* drive icon, drawn once                     */
    int disk, sel, nsizes;
    int hover, pressed, focus;
    bool focus_visible;
    int mx, my, buttons, acc_x, acc_y;
    rect_t logo, title, drive, label, panel, head, rows[MAX_SIZES], install, cont, prev, next, bar;
    bool shown, continued;
} B;

static int K(double v) { return (int)lround(v * B.k); }
static rect_t screen(void) { return rect_make(0, 0, B.w, B.h); }

/* ---- who gets the screen ------------------------------------------------------------- */
bool bootsetup_wanted(void) {
    const char *boot = sys_cmdline_get("boot");
    if (persist_active() || (boot && !strcmp(boot, "disk"))) return false;   /* installed drive */
    const char *s = sys_cmdline_get("setup");
    if (s) return strcmp(s, "0") != 0;
    if (sys_cmdline_get("nosetup")) return false;
    /* automated starts (tools/shot.py, autorun=App) go straight to the desktop */
    if (sys_cmdline_get("autorun") || g_test_mouse) return false;
    return true;
}

/* ---- disks and sizes --------------------------------------------------------------------- */
static bool disk_usable(int d) { return d >= 0 && install_check_disk(d, NULL, 0) == DISK_OK; }

static bool size_ok(int i) {
    return disk_usable(B.disk) && i >= 0 && i < B.nsizes && install_size_fits(B.disk, install_size_bytes(i));
}

static bool can_install(void) { return size_ok(B.sel); }

static void pick_size(void) {
    if (!size_ok(B.sel)) B.sel = disk_usable(B.disk) ? install_size_default(B.disk) : -1;
}

static void pick_disk(void) {
    B.disk = -1;
    for (int i = 0; i < disk_count() && B.disk < 0; i++)
        if (disk_usable(i) && install_size_default(i) >= 0) B.disk = i;
    for (int i = 0; i < disk_count() && B.disk < 0; i++)
        if (disk_usable(i)) B.disk = i;
    if (B.disk < 0 && disk_count() > 0) B.disk = 0;
    B.sel = -1;
    pick_size();
}

/* ---- layout -------------------------------------------------------------------------------- */
static void layout(void) {
    int w = B.w;
    int ls = K(72);
    B.logo = rect_make((w - ls) / 2, K(40), ls, ls);
    int th = font_height(FONT_UI_TITLE);
    B.title = rect_make(0, B.logo.y + ls + K(14), w, th);
    int dw = K(196), dh = K(112);
    B.drive = rect_make((w - dw) / 2, B.title.y + th + K(26), dw, dh);
    B.label = rect_make(0, B.drive.y + dh + K(12), w, 2 * font_height(FONT_UI) + 4);
    int rowh = MAX(22, K(28)), pad = K(22);
    int pw = MIN(w - 32, K(560)), listw = K(230);
    int ph = pad + K(30) + B.nsizes * rowh + pad;
    B.panel = rect_make((w - pw) / 2, B.label.y + B.label.h + K(12), pw, ph);
    B.head = rect_make(B.panel.x + pad, B.panel.y + pad, listw, K(24));
    for (int i = 0; i < B.nsizes; i++)
        B.rows[i] = rect_make(B.panel.x + pad, B.head.y + K(30) + i * rowh, listw, rowh - 4);
    int bw = MIN(K(220), pw - listw - 3 * pad), bh = MAX(32, K(44));
    int bx = B.panel.x + pw - pad - bw, mid = B.panel.y + ph / 2;
    B.install = rect_make(bx, mid - bh - K(8), bw, bh);
    B.cont = rect_make(bx, mid + K(8), bw, bh);
    int barw = MIN(w - K(120), K(720)), barh = MAX(26, K(34));
    B.bar = rect_make((w - barw) / 2, K(64), barw, barh);
}

/* ---- drawing ----------------------------------------------------------------------------------- */
/* The hard drive: a 3.5" disk seen from above (platter, arm, activity light). */
static surface_t *render_drive(int w, int h) {
    surface_t *s = surface_create_alpha(w, h);
    if (!s) return NULL;
    int r = MAX(3, h / 7);
    gfx_fill_rounded_gradient_v(s, 0, 0, w, h, r, RGB(206, 213, 226), RGB(132, 142, 160));
    gfx_rounded_rect(s, 0, 0, w, h, r, RGBA(30, 36, 52, 170));
    gfx_fill_rounded_rect(s, 2, 2, w - 4, h / 2 - 2, MAX(2, r - 2), RGBA(255, 255, 255, 30));
    int pr = h * 34 / 100, pcx = w * 38 / 100, pcy = h / 2;
    gfx_fill_circle_aa(s, pcx, pcy + 1, pr + 2, RGBA(40, 48, 66, 110));
    gfx_fill_circle_aa(s, pcx, pcy, pr, RGB(228, 233, 243));
    for (int i = 1; i <= 3; i++)
        gfx_circle_aa(s, pcx, pcy, pr * (0.38 + i * 0.17), MAX(1.0, h / 70.0), RGBA(120, 132, 156, 90));
    gfx_fill_circle_aa(s, pcx, pcy, MAX(2.0, pr * 0.2), RGB(92, 102, 124));
    gfx_fill_circle_aa(s, pcx, pcy, MAX(1.0, pr * 0.08), RGB(190, 198, 214));
    int ax = w * 80 / 100, ay = h * 24 / 100;
    gfx_thick_line(s, ax, ay, pcx + pr / 3, pcy - pr / 5, MAX(2, h / 13), RGB(72, 80, 98));
    gfx_fill_circle_aa(s, ax, ay, MAX(2.0, h / 10.0), RGB(72, 80, 98));
    gfx_fill_circle_aa(s, ax, ay, MAX(1.0, h / 26.0), RGB(150, 158, 176));
    gfx_fill_circle_aa(s, w * 86 / 100, h * 78 / 100, MAX(1.5, h / 15.0), RGB(80, 220, 120));
    for (int i = 0; i < 4; i++)   /* screws */
        gfx_fill_circle_aa(s, i & 1 ? w - h / 9 : h / 9, i & 2 ? h - h / 9 : h / 9, MAX(1.0, h / 28.0),
                           RGBA(70, 78, 96, 160));
    return s;
}

static void draw_text_center(surface_t *s, rect_t r, const char *t, uint32_t c, const font_t *f) {
    gfx_text_aligned(s, r, t, c, f, ALIGN_CENTER | ALIGN_MIDDLE);
}

static void draw_disk_label(surface_t *s) {
    char line[160], note[96] = "";
    uint32_t nc = C_DIM;
    int lh = font_height(FONT_UI);
    B.prev = B.next = rect_make(0, 0, 0, 0);
    const disk_info_t *d = disk_get(B.disk);
    if (!d) {
        strlcpy(line, "No hard disk found", sizeof line);
    } else {
        char size[24];
        disk_format_size(d->sectors * 512, size, sizeof size);
        snprintf(line, sizeof line, "%s  \x95  %s  \x95  %s", d->model, size, d->location);
        char why[96] = "";
        if (install_check_disk(B.disk, why, sizeof why) != DISK_OK) {
            strlcpy(note, why, sizeof note);
            nc = C_WARN;
        } else if (install_size_default(B.disk) < 0) {
            snprintf(note, sizeof note, "The hard disk is smaller than %s", install_size_label(0));
            nc = C_WARN;
        } else {
            strlcpy(note, "Installing erases everything on this hard disk", sizeof note);
        }
    }
    rect_t l1 = rect_make(B.label.x, B.label.y, B.label.w, lh);
    draw_text_center(s, l1, line, d ? C_TEXT : C_WARN, d ? FONT_UI_BOLD : FONT_UI_LARGE);
    if (note[0]) draw_text_center(s, rect_make(B.label.x, B.label.y + lh + 4, B.label.w, lh), note, nc, FONT_UI);
    if (disk_count() > 1) {
        int tw = font_text_width(FONT_UI_BOLD, line), a = K(22);
        int cy = l1.y + lh / 2;
        B.prev = rect_make(B.w / 2 - tw / 2 - a - K(10), cy - a / 2, a, a);
        B.next = rect_make(B.w / 2 + tw / 2 + K(10), cy - a / 2, a, a);
        for (int i = 0; i < 2; i++) {
            rect_t r = i ? B.next : B.prev;
            bool hov = B.hover == (i ? E_NEXT : E_PREV);
            gfx_fill_circle_aa(s, r.x + a / 2.0, r.y + a / 2.0, a / 2.0, RGBA(255, 255, 255, hov ? 46 : 22));
            ui_draw_glyph(s, i ? UI_GLYPH_ARROW_RIGHT : UI_GLYPH_ARROW_LEFT, r.x + a / 4, r.y + a / 4, a / 2, C_TEXT);
        }
    }
}

static void draw_row(surface_t *s, int i) {
    rect_t r = B.rows[i];
    bool ok = size_ok(i), sel = i == B.sel && ok, hov = B.hover == E_SIZE + i && ok;
    int rad = MIN(K(8), r.h / 2);
    if (sel) {
        gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, rad, RGBA(59, 130, 246, 92));
        gfx_rounded_rect(s, r.x, r.y, r.w, r.h, rad, RGBA(130, 176, 255, 170));
    } else if (hov) {
        gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, rad, RGBA(255, 255, 255, 22));
    }
    if (sel && B.focus == F_SIZES && B.focus_visible)
        gfx_rounded_rect(s, r.x - 2, r.y - 2, r.w + 4, r.h + 4, rad + 2, RGBA(170, 205, 255, 220));
    double cx = r.x + K(16), cy = r.y + r.h / 2.0, rr = MAX(5, K(7));
    if (sel) {
        gfx_fill_circle_aa(s, cx, cy, rr, C_ACCENT);
        gfx_circle_aa(s, cx, cy, rr, 1.4, RGBA(170, 205, 255, 230));
        gfx_fill_circle_aa(s, cx, cy, rr * 0.42, COLOR_WHITE);
    } else {
        gfx_circle_aa(s, cx, cy, rr, 1.4, ok ? RGBA(255, 255, 255, 190) : C_OFF);
    }
    int ty = r.y + (r.h - font_height(FONT_UI)) / 2;
    gfx_text(s, r.x + K(32), ty, install_size_label(i), ok ? C_TEXT : C_OFF, sel ? FONT_UI_BOLD : FONT_UI);
}

static void draw_button(surface_t *s, rect_t r, const char *label, bool primary, bool enabled, int id, bool focus) {
    int rad = MIN(K(10), r.h / 2);
    bool hov = enabled && B.hover == id, prs = hov && B.pressed == id;
    if (primary) {
        if (!enabled) {
            gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, rad, RGBA(255, 255, 255, 26));
        } else {
            uint32_t c = prs ? RGB(37, 99, 220) : hov ? RGB(92, 156, 255) : C_ACCENT;
            gfx_fill_rounded_gradient_v(s, r.x, r.y, r.w, r.h, rad, color_shade(c, 18), color_shade(c, -10));
        }
    } else {
        gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, rad, RGBA(255, 255, 255, prs ? 44 : hov ? 28 : 12));
        gfx_rounded_rect(s, r.x, r.y, r.w, r.h, rad, RGBA(255, 255, 255, enabled ? 96 : 36));
    }
    if (focus && enabled && B.focus_visible) {
        gfx_rounded_rect(s, r.x - 3, r.y - 3, r.w + 6, r.h + 6, rad + 3, RGBA(170, 205, 255, 230));
        gfx_rounded_rect(s, r.x - 2, r.y - 2, r.w + 4, r.h + 4, rad + 2, RGBA(170, 205, 255, 120));
    }
    draw_text_center(s, r, label, enabled ? COLOR_WHITE : C_OFF, FONT_UI_LARGE);
}

/* Everything but the pointer, into B.scene. */
static void draw_scene(void) {
    surface_t *s = B.scene;
    gfx_blit(s, 0, 0, B.base);
    draw_text_center(s, B.title, "NovaOS Drive installation", C_TEXT, FONT_UI_TITLE);
    if (B.drive_img) {
        gfx_shadow(s, B.drive.x, B.drive.y, B.drive.w, B.drive.h, K(14), K(6), 70);
        gfx_blit_alpha(s, B.drive.x, B.drive.y, B.drive_img, disk_get(B.disk) ? 255 : 90);
    }
    draw_disk_label(s);
    rect_t p = B.panel;
    gfx_fill_rounded_rect(s, p.x, p.y, p.w, p.h, K(16), RGBA(255, 255, 255, 12));
    gfx_rounded_rect(s, p.x, p.y, p.w, p.h, K(16), RGBA(255, 255, 255, 34));
    gfx_text(s, B.head.x + K(4), B.head.y + (B.head.h - font_height(FONT_UI_BOLD)) / 2, "Storage size", C_DIM,
             FONT_UI_BOLD);
    for (int i = 0; i < B.nsizes; i++) draw_row(s, i);
    draw_button(s, B.install, "Install", true, can_install(), E_INSTALL, B.focus == F_INSTALL);
    draw_button(s, B.cont, "Continue ISO", false, true, E_CONTINUE, B.focus == F_CONTINUE);
}

/* ---- presenting ---------------------------------------------------------------------------------- */
static void present_rect(rect_t r) {
    r = rect_intersect(r, screen());
    if (!rect_empty(r)) fb_present(wm_backbuf, r);
}

static void show_all(bool pointer) {
    gfx_blit(wm_backbuf, 0, 0, B.scene);
    if (pointer) cursor_draw(wm_backbuf, B.mx, B.my, CURSOR_ARROW);
    present_rect(screen());
}

static void show_pointer_move(int ox, int oy) {
    rect_t r = rect_intersect(rect_union(cursor_rect(ox, oy, CURSOR_ARROW), cursor_rect(B.mx, B.my, CURSOR_ARROW)),
                              screen());
    if (rect_empty(r)) return;
    gfx_blit_rect(wm_backbuf, r.x, r.y, B.scene, r.x, r.y, r.w, r.h);
    cursor_draw(wm_backbuf, B.mx, B.my, CURSOR_ARROW);
    present_rect(r);
}

/* ---- input --------------------------------------------------------------------------------------- */
static int hit(int x, int y) {
    for (int i = 0; i < B.nsizes; i++)
        if (rect_contains(B.rows[i], x, y)) return E_SIZE + i;
    if (rect_contains(B.install, x, y)) return E_INSTALL;
    if (rect_contains(B.cont, x, y)) return E_CONTINUE;
    if (rect_contains(B.prev, x, y)) return E_PREV;
    if (rect_contains(B.next, x, y)) return E_NEXT;
    return E_NONE;
}

/* The same pointer motion as the window manager (kernel/gui/wm.c). */
static void motion(int dx, int dy) {
    if (!dx && !dy) return;
    if (g_test_mouse) {
        B.mx += dx;
        B.my += dy;
    } else {
        int mult = g_mouse_speed * 16 / 5;
        if (abs(dx) + abs(dy) > 10) mult = mult * 3 / 2;
        B.acc_x += dx * mult;
        B.acc_y += dy * mult;
        int mx = B.acc_x / 16, my = B.acc_y / 16;
        B.acc_x -= mx * 16;
        B.acc_y -= my * 16;
        B.mx += mx;
        B.my += my;
    }
    B.mx = CLAMP(B.mx, 0, B.w - 1);
    B.my = CLAMP(B.my, 0, B.h - 1);
}

static void switch_disk(int dir) {
    int n = disk_count();
    if (n < 2) return;
    B.disk = ((B.disk < 0 ? 0 : B.disk) + dir + n) % n;
    B.sel = -1;
    pick_size();
    klog("bootsetup: hard disk %d selected", B.disk);
}

static void move_selection(int dir) {
    for (int i = B.sel + dir; i >= 0 && i < B.nsizes; i += dir)
        if (size_ok(i)) { B.sel = i; return; }
}

static void cycle_focus(bool back) {
    static const int order[] = { F_SIZES, F_INSTALL, F_CONTINUE };
    int cur = 0;
    for (int i = 0; i < 3; i++) if (order[i] == B.focus) cur = i;
    for (int n = 0; n < 3; n++) {
        cur = (cur + (back ? 2 : 1)) % 3;
        int f = order[cur];
        if ((f == F_INSTALL && !can_install()) || (f == F_SIZES && B.sel < 0)) continue;
        B.focus = f;
        return;
    }
}

/* Returns an action; *dirty when the scene has to be redrawn. */
static int handle(const input_event_t *ev, bool *dirty, int *adx, int *ady) {
    if (ev->type == IN_MOUSE) {
        if (ev->buttons == B.buttons) { *adx += ev->dx; *ady += ev->dy; return A_NONE; }
        motion(*adx + ev->dx, *ady + ev->dy);
        *adx = *ady = 0;
        int was = B.buttons;
        B.buttons = ev->buttons;
        int h = hit(B.mx, B.my);
        if (h != B.hover) { B.hover = h; *dirty = true; }
        if ((ev->buttons & 1) && !(was & 1)) {                /* left button down */
            B.pressed = h;
            *dirty = true;
            if (h >= E_SIZE && h < E_SIZE + B.nsizes) {
                if (size_ok(h - E_SIZE)) { B.sel = h - E_SIZE; B.focus = F_SIZES; }
                else sys_beep(220, 60);
            }
        } else if (!(ev->buttons & 1) && (was & 1)) {         /* left button up */
            int p = B.pressed;
            B.pressed = E_NONE;
            *dirty = true;
            if (p != h) return A_NONE;
            if (p == E_INSTALL && can_install()) return A_INSTALL;
            if (p == E_CONTINUE) return A_CONTINUE;
            if (p == E_PREV) switch_disk(-1);
            if (p == E_NEXT) switch_disk(1);
        }
        return A_NONE;
    }
    if (ev->type != IN_KEY) return A_NONE;
    if (*adx || *ady) { motion(*adx, *ady); *adx = *ady = 0; }
    if (!ev->pressed) return A_NONE;
    *dirty = true;
    B.focus_visible = true;
    switch (ev->key) {
    case KEY_UP: case KEY_DOWN:
        if (B.sel < 0) pick_size();
        else move_selection(ev->key == KEY_UP ? -1 : 1);
        if (B.sel >= 0) B.focus = F_SIZES;
        break;
    case KEY_LEFT: case KEY_RIGHT:
        switch_disk(ev->key == KEY_LEFT ? -1 : 1);
        break;
    case KEY_TAB:
        cycle_focus(ev->mods & MOD_SHIFT);
        break;
    case KEY_ENTER: case KEY_SPACE:
        if (B.focus == F_INSTALL && can_install()) return A_INSTALL;
        if (B.focus == F_CONTINUE) return A_CONTINUE;
        break;
    default:
        break;
    }
    return A_NONE;
}

static int interact(void) {
    B.hover = hit(B.mx, B.my);
    draw_scene();
    show_all(true);
    for (;;) {
        input_event_t ev;
        bool dirty = false;
        int ox = B.mx, oy = B.my, adx = 0, ady = 0, action = A_NONE;
        while (action == A_NONE && input_pop(&ev)) action = handle(&ev, &dirty, &adx, &ady);
        if (adx || ady) motion(adx, ady);
        int h = hit(B.mx, B.my);
        if (h != B.hover) { B.hover = h; dirty = true; }
        if (action != A_NONE) {
            B.hover = B.pressed = E_NONE;
            draw_scene();
            show_all(false);
            return action;
        }
        if (dirty) { draw_scene(); show_all(true); }
        else if (ox != B.mx || oy != B.my) show_pointer_move(ox, oy);
        __asm__ volatile("cli");
        if (input_pending()) __asm__ volatile("sti");
        else __asm__ volatile("sti; hlt");
    }
}

/* ---- the installation ------------------------------------------------------------------------------- */
static void draw_bar(surface_t *s, int permille) {
    rect_t b = B.bar;
    gfx_blit_rect(s, b.x - 4, b.y - 4, B.base, b.x - 4, b.y - 4, b.w + 8, b.h + 8);
    int rad = b.h / 2;
    gfx_fill_rounded_rect(s, b.x, b.y, b.w, b.h, rad, RGBA(255, 255, 255, 26));
    gfx_rounded_rect(s, b.x, b.y, b.w, b.h, rad, RGBA(255, 255, 255, 44));
    int fw = (int)((int64_t)b.w * CLAMP(permille, 0, 1000) / 1000);
    if (fw >= b.h) {
        gfx_fill_rounded_gradient_v(s, b.x, b.y, fw, b.h, rad, RGB(150, 120, 255), RGB(60, 190, 240));
    } else if (fw > 0) {
        /* shorter than the rounded end: a clipped piece of it */
        surface_t sub = surface_sub(s, rect_make(b.x, b.y, fw, b.h));
        gfx_fill_rounded_gradient_v(&sub, 0, 0, b.h, b.h, rad, RGB(150, 120, 255), RGB(60, 190, 240));
    }
    char pct[8];
    snprintf(pct, sizeof pct, "%d%%", CLAMP(permille, 0, 1000) / 10);
    draw_text_center(s, b, pct, COLOR_WHITE, FONT_UI_BOLD);
}

/* Big centred text (two lines at most) under the bar area. */
static void draw_message(surface_t *s, const char *line1, const char *line2, uint32_t c2) {
    const font_t *f = font_text_width(FONT_UI_TITLE, line1) <= B.w - 64 ? FONT_UI_TITLE : FONT_UI_LARGE;
    int fh = font_height(f), y = B.h / 2 - fh;
    draw_text_center(s, rect_make(0, y, B.w, fh), line1, C_TEXT, f);
    if (line2) draw_text_center(s, rect_make(16, y + fh + K(12), B.w - 32, font_height(FONT_UI) * 3), line2, c2, FONT_UI);
}

static void wait_click_or_key(void) {
    input_event_t ev;
    while (input_pop(&ev)) {}
    int buttons = B.buttons;
    for (;;) {
        while (input_pop(&ev)) {
            if (ev.type == IN_KEY && ev.pressed) return;
            if (ev.type == IN_MOUSE) {
                bool up = (buttons & 1) && !(ev.buttons & 1);
                buttons = ev.buttons;
                if (up) { B.buttons = buttons; return; }
            }
        }
        __asm__ volatile("cli");
        if (input_pending()) __asm__ volatile("sti");
        else __asm__ volatile("sti; hlt");
    }
}

/* Install to B.disk with the chosen size. Restarts the computer when it worked;
   returns after showing the error when it did not. */
static void run_install(void) {
    uint64_t bytes = install_size_bytes(B.sel);
    const disk_info_t *d = disk_get(B.disk);
    klog("bootsetup: installing a %s drive on disk %d (%s)", install_size_label(B.sel), B.disk, d ? d->model : "?");
    char err[160] = "";
    install_job_t *job = install_begin_sized(B.disk, bytes, true, err, sizeof err);

    /* the screen changes to a progress bar at the top (on the plain background) */
    surface_t *prog = surface_create(B.w, B.h);
    if (!prog) { install_free(job); return; }
    surface_t *bg = splash_background();
    if (bg) gfx_blit(prog, 0, 0, bg);
    else gfx_gradient_v(prog, 0, 0, B.w, B.h, SPLASH_BG_TOP, SPLASH_BG_BOTTOM);
    surface_t *saved_base = B.base;
    B.base = prog;                     /* draw_bar restores from B.base */
    surface_t *frame = surface_clone(prog);
    if (frame) {
        draw_bar(frame, 0);
        splash_crossfade(B.scene, frame, FADE_MS);
    }
    int result = job ? 0 : DISK_EINVAL, shown = -1;
    uint32_t t0 = g_ticks;
    while (job && result == 0) {
        result = install_step(job, 25);
        int p = result == 1 ? 1000 : install_progress(job);
        if (p / 10 != shown / 10 || result) {
            shown = p;
            draw_bar(wm_backbuf, p);
            present_rect(rect_make(B.bar.x - 4, B.bar.y - 4, B.bar.w + 8, B.bar.h + 8));
        }
        input_event_t ev;
        while (input_pop(&ev)) {}       /* nothing to click while it installs */
    }
    if (result == 1) {
        klog("bootsetup: drive installed in %u ms - restarting into it", g_ticks - t0);
        install_free(job);
        draw_message(wm_backbuf, "Your drive has been installed and ready to have files savable", NULL, 0);
        present_rect(screen());
        static const note_t chime[] = { { 784, 90 }, { 988, 90 }, { 1319, 180 } };
        sys_play_notes(chime, 3);
        boot_sleep_until(g_ticks + DONE_MS);
        sys_reboot();
    }
    char why[200];
    snprintf(why, sizeof why, "%s", job ? install_error(job) : err);
    if (!why[0]) strlcpy(why, "The installation could not be started.", sizeof why);
    klog("bootsetup: installation failed: %s", why);
    install_free(job);
    sys_beep(220, 200);
    draw_message(wm_backbuf, "The drive could not be installed", why, C_WARN);
    draw_text_center(wm_backbuf, rect_make(0, B.h - K(80), B.w, font_height(FONT_UI)), "Click to go back", C_DIM, FONT_UI);
    present_rect(screen());
    wait_click_or_key();
    B.base = saved_base;
    surface_t *from = surface_clone(wm_backbuf);
    draw_scene();
    if (from) splash_crossfade(from, B.scene, FADE_MS);
    surface_destroy(from);
    surface_destroy(frame);
    surface_destroy(prog);
}

/* ---- the screen ---------------------------------------------------------------------------------------- */
static double ease_in_out(double t) { return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2; }

/* The splash icon moves from the middle to the top (and loses its glow). */
static void move_icon_up(void) {
    if (!splash_active()) {
        gfx_gradient_v(wm_backbuf, 0, 0, B.w, B.h, SPLASH_BG_TOP, SPLASH_BG_BOTTOM);
        ui_draw_logo(wm_backbuf, B.logo.x, B.logo.y, B.logo.w);
        present_rect(screen());
        return;
    }
    rect_t from = splash_logo_rect();
    uint32_t t0 = g_ticks, next = t0;
    for (;;) {
        uint32_t el = g_ticks - t0;
        double t = el >= MOVE_MS ? 1.0 : (double)el / MOVE_MS, e = ease_in_out(t);
        int sz = (int)lround(from.w + (B.logo.w - from.w) * e);
        int x = (int)lround(from.x + (B.logo.x - from.x) * e);
        int y = (int)lround(from.y + (B.logo.y - from.y) * e);
        splash_place_logo(x, y, sz, (int)lround(255 * (1.0 - e)));
        if (t >= 1.0) break;
        next += FRAME_MS;
        if ((int32_t)(next - g_ticks) < 0) next = g_ticks;
        boot_sleep_until(next);
    }
}

void bootsetup_run(void) {
    if (!wm_backbuf) return;
    memset(&B, 0, sizeof B);
    B.w = wm_backbuf->w;
    B.h = wm_backbuf->h;
    B.k = CLAMP(B.h / 768.0, 0.72, 1.6);
    B.nsizes = MIN(install_size_count(), MAX_SIZES);
    B.mx = wm_mouse_x;
    B.my = wm_mouse_y;
    B.buttons = wm_mouse_buttons;
    B.focus = F_SIZES;
    pick_disk();
    layout();
    klog("bootsetup: NovaOS Drive installation (%d hard disk%s, disk %d, size %s)", disk_count(),
         disk_count() == 1 ? "" : "s", B.disk, B.sel >= 0 ? install_size_label(B.sel) : "-");

    move_icon_up();
    B.base = surface_clone(wm_backbuf);
    B.scene = surface_clone(wm_backbuf);
    B.drive_img = render_drive(B.drive.w, B.drive.h);
    if (!B.base || !B.scene) {
        klog("bootsetup: out of memory - continuing from the ISO");
        surface_destroy(B.base);
        surface_destroy(B.scene);
        surface_destroy(B.drive_img);
        B.continued = true;
        return;
    }
    draw_scene();
    splash_crossfade(B.base, B.scene, FADE_MS);
    B.shown = true;
    klog("bootsetup: ready");

    for (;;) {
        int a = interact();
        if (a == A_CONTINUE) break;
        if (a == A_INSTALL) run_install();      /* returns only if it failed */
    }
    klog("bootsetup: Continue ISO");
    B.continued = true;
    /* leave the screen without the pointer; the desktop fades in over it */
    gfx_blit(wm_backbuf, 0, 0, B.scene);
    present_rect(screen());
    wm_mouse_x = B.mx;
    wm_mouse_y = B.my;
    wm_mouse_buttons = B.buttons;
    surface_destroy(B.base);
    surface_destroy(B.scene);
    surface_destroy(B.drive_img);
    B.base = B.scene = B.drive_img = NULL;
}

/* ---- after the desktop: the save file screen (OSaver, item 14) ---------------------------------------- */
static void prompt_done(void *ctx) {
    (void)ctx;
    klog("bootsetup: save file screen closed");
}

static void prompt_start(void *ctx) {
    (void)ctx;
    if (osaver_boot_prompt) {
        klog("bootsetup: save file screen");
        osaver_boot_prompt(prompt_done, NULL);
    } else {
        klog("bootsetup: no save file screen in this build");
    }
}

void bootsetup_after_desktop(void) {
    if (B.continued) timer_add(250, prompt_start, NULL, false);
}
