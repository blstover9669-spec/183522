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
 *   Continue ISO  NovaOS keeps running from the ISO (nothing is saved); the save
 *                 file screen (OSaver, osaver_boot_prompt) follows, and the
 *                 desktop when that closes.
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

enum { E_NONE = 0, E_SIZE = 1 /* + index */, E_INSTALL = 40, E_CONTINUE, E_PREV, E_NEXT,
       E_CANCEL = 60, E_HOWTO, E_RESTART, E_TAB /* + 0 VM, 1 USB */ };
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
    /* progress screen: Cancel; finished screen: text, buttons, tutorial (tabs VM / USB) */
    rect_t cancel, msg, howto, restart, tut, tabs[2];
    const font_t *msg_font;
    int fid;                   /* element with the keyboard focus there, E_NONE = none */
    int tab;
    bool tut_open;
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
    int cw = MAX(K(160), font_text_width(FONT_UI_LARGE, "Cancel") + K(56));
    B.cancel = rect_make((w - cw) / 2, B.bar.y + barh + K(28), cw, bh);
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

/* Wait for the next interrupt unless input is already queued. */
static void idle(void) {
    __asm__ volatile("cli");
    if (input_pending()) __asm__ volatile("sti");
    else __asm__ volatile("sti; hlt");
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
        idle();
    }
}

/* ---- the installation ------------------------------------------------------------------------------- */
#define DONE_TEXT   "Your drive has been installed and ready to have files savable"
#define HOWTO_TEXT  "How to start your drive"
#define TUT_ITEMS   4

/* The tutorial of the finished screen: tab 0 "VM" (VirtualBox), tab 1 "USB". */
typedef struct { const char *text; bool warn; } tut_item_t;
static const char *const tut_tabs[2] = { "VM", "USB" };
static const char *const tut_heads[2] = { "Starting your drive in VirtualBox", "NovaOS and USB sticks" };
static const tut_item_t tut_items[2][TUT_ITEMS] = {
    { { "Click Restart: the boot menu of the ISO starts your drive by itself after 2 seconds, even with "
        "NovaOS.iso still attached.", false },
      { "To start without the ISO: power off the VM, open Settings > Storage, select the optical drive with "
        "NovaOS.iso, choose \"Remove Disk from Virtual Drive\" and start the VM.", false },
      { "Keep the hard disk on the IDE controller: NovaOS does not find hard disks on SATA, SCSI or NVMe "
        "controllers.", false },
      { "To reinstall, pick NovaOS 2.5 Developer Preview in the boot menu within those 2 seconds.", false } },
    { { "NovaOS only reads IDE hard disks, so the installed drive can't run from a USB stick.", false },
      { "What works: write NovaOS.iso to a USB stick with balenaEtcher or Rufus (DD mode) and boot a PC with "
        "legacy BIOS/CSM from it with its boot menu key (often F12). NovaOS runs from the ISO and nothing is "
        "saved.", false },
      { "On a real computer choose Continue ISO, never Install: Install erases that computer's hard disk.", true },
      { "NovaOS has only been tested in virtual machines.", false } },
};

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
        idle();
    }
}

/* Part of B.scene to the screen, with the pointer if it is over that part. */
static void show_rect(rect_t r) {
    rect_t c = cursor_rect(B.mx, B.my, CURSOR_ARROW);
    bool pointer = rect_overlaps(r, c);
    if (pointer) r = rect_union(r, c);
    r = rect_intersect(r, screen());
    if (rect_empty(r)) return;
    gfx_blit_rect(wm_backbuf, r.x, r.y, B.scene, r.x, r.y, r.w, r.h);
    if (pointer) cursor_draw(wm_backbuf, B.mx, B.my, CURSOR_ARROW);
    present_rect(r);
}

static int hit_list(const rect_t *rs, const int *ids, int n, int x, int y) {
    for (int i = 0; i < n; i++)
        if (rect_contains(rs[i], x, y)) return ids[i];
    return E_NONE;
}

/* Pointer and keys of the progress and finished screens (elements rs/ids): returns the
   element clicked (pressed and released on it), else E_NONE. A pressed key ends the
   polling and comes back in *key / *mods (0: none). *dirty: hover or press changed. */
static int poll_input(const rect_t *rs, const int *ids, int n, bool *dirty, int *key, int *mods) {
    input_event_t ev;
    int adx = 0, ady = 0, clicked = E_NONE;
    *key = 0;
    while (clicked == E_NONE && !*key && input_pop(&ev)) {
        if (ev.type == IN_MOUSE) {
            if (ev.buttons == B.buttons) { adx += ev.dx; ady += ev.dy; continue; }
            motion(adx + ev.dx, ady + ev.dy);
            adx = ady = 0;
            int was = B.buttons, h = hit_list(rs, ids, n, B.mx, B.my);
            B.buttons = ev.buttons;
            if ((ev.buttons & 1) && !(was & 1)) {
                B.pressed = h;
                *dirty = true;
            } else if (!(ev.buttons & 1) && (was & 1)) {
                if (B.pressed != E_NONE && B.pressed == h) clicked = h;
                B.pressed = E_NONE;
                *dirty = true;
            }
        } else if (ev.type == IN_KEY && ev.pressed) {
            *key = ev.key;
            *mods = ev.mods;
        }
    }
    if (adx || ady) motion(adx, ady);
    int h = hit_list(rs, ids, n, B.mx, B.my);
    if (h != B.hover) { B.hover = h; *dirty = true; }
    return clicked;
}

/* Tab / Shift+Tab: the next element of ids gets the keyboard focus. */
static void cycle_fid(const int *ids, int n, bool back) {
    int cur = -1;
    for (int i = 0; i < n; i++) if (ids[i] == B.fid) cur = i;
    if (cur < 0) cur = back ? 0 : n - 1;
    B.fid = ids[(cur + (back ? n - 1 : 1)) % n];
    B.focus_visible = true;
}

/* -- progress: the bar at the top, Cancel under it -- */
static void draw_progress(surface_t *s, int permille) {
    gfx_blit(s, 0, 0, B.base);
    draw_bar(s, permille);
    draw_button(s, B.cancel, "Cancel", false, true, E_CANCEL, B.fid == E_CANCEL);
}

/* -- finished: the text, "How to start your drive" (the tutorial) and Restart -- */
static int tut_pad(void) { return K(20); }
static int tut_indent(void) { return K(20); }
static int tut_tab_h(void) { return MAX(28, K(34)); }

/* Height of a tab's text (heading + items) wrapped to textw. */
static int tut_body_height(int t, int textw) {
    int h = font_height(FONT_UI_BOLD) + K(10);
    for (int i = 0; i < TUT_ITEMS; i++) h += (i ? K(8) : 0) + font_wrap_height(FONT_UI, tut_items[t][i].text, textw);
    return h;
}

static void layout_done(void) {
    B.msg_font = font_text_width(FONT_UI_TITLE, DONE_TEXT) <= B.w - 64 ? FONT_UI_TITLE : FONT_UI_LARGE;
    int mh = font_height(B.msg_font), bh = MAX(32, K(44)), gap = K(16);
    int bw1 = font_text_width(FONT_UI_LARGE, HOWTO_TEXT) + K(56);
    int bw2 = MAX(K(170), font_text_width(FONT_UI_LARGE, "Restart") + K(56));
    int top = B.bar.y + B.bar.h, avail = B.h - top;
    B.howto = B.tut = B.tabs[0] = B.tabs[1] = rect_make(0, 0, 0, 0);
    if (!B.tut_open) {
        int y = top + (avail - (mh + K(32) + bh)) / 2, row = bw1 + gap + bw2;
        B.msg = rect_make(16, y, B.w - 32, mh);
        B.howto = rect_make((B.w - row) / 2, y + mh + K(32), bw1, bh);
        B.restart = rect_make(B.howto.x + bw1 + gap, B.howto.y, bw2, bh);
        return;
    }
    /* the tutorial between the text and Restart; its size fits the longer tab */
    int pw = MIN(B.w - 32, K(780)), pad = tut_pad(), textw = pw - 2 * pad - tut_indent();
    int ph = pad + tut_tab_h() + K(18) + MAX(tut_body_height(0, textw), tut_body_height(1, textw)) + pad;
    int y = top + MAX(K(10), (avail - (mh + K(20) + ph + K(20) + bh)) / 2);
    B.msg = rect_make(16, y, B.w - 32, mh);
    B.tut = rect_make((B.w - pw) / 2, y + mh + K(20), pw, ph);
    int tw = MAX(K(120), font_text_width(FONT_UI_BOLD, "USB") + K(60));
    for (int i = 0; i < 2; i++) B.tabs[i] = rect_make(B.tut.x + pw / 2 - tw + i * tw, B.tut.y + pad, tw, tut_tab_h());
    B.restart = rect_make((B.w - bw2) / 2, B.tut.y + ph + K(20), bw2, bh);
}

static void draw_tutorial(surface_t *s) {
    rect_t p = B.tut, t0 = B.tabs[0];
    gfx_fill_rounded_rect(s, p.x, p.y, p.w, p.h, K(16), RGBA(255, 255, 255, 12));
    gfx_rounded_rect(s, p.x, p.y, p.w, p.h, K(16), RGBA(255, 255, 255, 34));
    /* the tabs: a segmented control at the top */
    int rad = t0.h / 2, tw = 2 * t0.w;
    gfx_fill_rounded_rect(s, t0.x, t0.y, tw, t0.h, rad, RGBA(255, 255, 255, 16));
    gfx_rounded_rect(s, t0.x, t0.y, tw, t0.h, rad, RGBA(255, 255, 255, 40));
    for (int i = 0; i < 2; i++) {
        rect_t r = B.tabs[i];
        bool on = B.tab == i, hov = B.hover == E_TAB + i;
        if (on)
            gfx_fill_rounded_gradient_v(s, r.x + 2, r.y + 2, r.w - 4, r.h - 4, rad - 2, color_shade(C_ACCENT, 18),
                                        color_shade(C_ACCENT, -10));
        else if (hov)
            gfx_fill_rounded_rect(s, r.x + 2, r.y + 2, r.w - 4, r.h - 4, rad - 2, RGBA(255, 255, 255, 30));
        if (B.fid == E_TAB + i && B.focus_visible)
            gfx_rounded_rect(s, r.x - 1, r.y - 1, r.w + 2, r.h + 2, rad + 1, RGBA(170, 205, 255, 230));
        draw_text_center(s, r, tut_tabs[i], on || hov ? COLOR_WHITE : C_DIM, FONT_UI_BOLD);
    }
    int pad = tut_pad(), ind = tut_indent(), x = p.x + pad, textw = p.w - 2 * pad - ind;
    int y = t0.y + t0.h + K(18), lh = font_height(FONT_UI);
    gfx_text(s, x, y, tut_heads[B.tab], C_TEXT, FONT_UI_BOLD);
    y += font_height(FONT_UI_BOLD) + K(10);
    for (int i = 0; i < TUT_ITEMS; i++) {
        const tut_item_t *it = &tut_items[B.tab][i];
        if (i) y += K(8);
        if (it->warn) {
            int g = MIN(lh, MAX(12, K(16)));
            ui_draw_glyph(s, UI_GLYPH_WARNING, x, y + (lh - g) / 2, g, C_WARN);
        } else {
            gfx_fill_circle_aa(s, x + K(6), y + lh / 2.0, MAX(2.0, K(2.6)), C_DIM);
        }
        y += gfx_text_wrapped(s, rect_make(x + ind, y, textw, p.y + p.h - y), it->text, it->warn ? C_WARN : C_TEXT,
                              FONT_UI);
    }
}

static void draw_done(void) {
    surface_t *s = B.scene;
    gfx_blit(s, 0, 0, B.base);
    draw_bar(s, 1000);
    draw_text_center(s, B.msg, DONE_TEXT, C_TEXT, B.msg_font);
    if (B.tut_open) draw_tutorial(s);
    else draw_button(s, B.howto, HOWTO_TEXT, false, true, E_HOWTO, B.fid == E_HOWTO);
    draw_button(s, B.restart, "Restart", true, true, E_RESTART, B.fid == E_RESTART);
}

static int done_elements(rect_t *rs, int *ids) {
    int n = 0;
    if (B.tut_open) {
        for (int i = 0; i < 2; i++) { rs[n] = B.tabs[i]; ids[n++] = E_TAB + i; }
    } else {
        rs[n] = B.howto; ids[n++] = E_HOWTO;
    }
    rs[n] = B.restart; ids[n++] = E_RESTART;
    return n;
}

/* The drive is installed: stays here until Restart, which restarts the computer into it. */
static void finished(void) {
    B.tut_open = false;
    B.tab = 0;
    B.hover = B.pressed = B.fid = E_NONE;
    layout_done();
    surface_t *from = surface_clone(wm_backbuf);
    draw_done();
    static const note_t chime[] = { { 784, 90 }, { 988, 90 }, { 1319, 180 } };
    sys_play_notes(chime, 3);
    if (from) splash_crossfade(from, B.scene, FADE_MS);
    surface_destroy(from);
    show_all(true);
    klog("bootsetup: finished screen (%s, Restart)", HOWTO_TEXT);
    for (;;) {
        rect_t rs[3];
        int ids[3], n = done_elements(rs, ids), key = 0, mods = 0, ox = B.mx, oy = B.my;
        bool dirty = false;
        int a = poll_input(rs, ids, n, &dirty, &key, &mods);
        if (key) {
            dirty = true;
            B.focus_visible = true;
            if (key == KEY_TAB) cycle_fid(ids, n, mods & MOD_SHIFT);
            else if (key == KEY_ENTER || key == KEY_SPACE) a = B.fid;
            else if ((key == KEY_LEFT || key == KEY_RIGHT) && B.tut_open) a = E_TAB + (key == KEY_RIGHT);
        }
        if (a == E_RESTART) {
            klog("bootsetup: Restart - restarting into the installed drive");
            gfx_blit(wm_backbuf, 0, 0, B.scene);
            present_rect(screen());
            sys_reboot();
        }
        if (a == E_HOWTO) {
            B.tut_open = true;
            if (B.fid == E_HOWTO) B.fid = E_TAB;
            layout_done();
            klog("bootsetup: %s (%s tab)", HOWTO_TEXT, tut_tabs[B.tab]);
        } else if ((a == E_TAB || a == E_TAB + 1) && B.tab != a - E_TAB) {
            B.tab = a - E_TAB;
            if (B.fid == E_TAB || B.fid == E_TAB + 1) B.fid = a;
            klog("bootsetup: tutorial %s tab", tut_tabs[B.tab]);
        }
        if (a != E_NONE) {
            dirty = true;
            n = done_elements(rs, ids);
            B.hover = hit_list(rs, ids, n, B.mx, B.my);
        }
        if (dirty) { draw_done(); show_all(true); }
        else if (ox != B.mx || oy != B.my) show_pointer_move(ox, oy);
        idle();
    }
}

/* Back to the NovaOS Drive installation screen (after Cancel or a failed installation). */
static void back_to_setup(surface_t *setup_base) {
    surface_t *from = surface_clone(wm_backbuf);
    B.base = setup_base;
    B.hover = B.pressed = B.fid = E_NONE;
    draw_scene();
    if (from) splash_crossfade(from, B.scene, FADE_MS);
    surface_destroy(from);
}

/* Install to B.disk with the chosen size. When it worked the finished screen follows (its
   Restart restarts the computer); returns to the NovaOS Drive installation screen when the
   installation failed or was cancelled. */
static void run_install(void) {
    uint64_t bytes = install_size_bytes(B.sel);
    const disk_info_t *d = disk_get(B.disk);
    klog("bootsetup: installing a %s drive on disk %d (%s)", install_size_label(B.sel), B.disk, d ? d->model : "?");
    char err[160] = "";
    install_job_t *job = install_begin_sized(B.disk, bytes, true, err, sizeof err);

    /* the screen changes to a progress bar at the top (on the plain background), Cancel under it */
    surface_t *prog = surface_create(B.w, B.h), *frame = surface_create(B.w, B.h);
    if (!prog || !frame) {
        surface_destroy(prog);
        surface_destroy(frame);
        install_free(job);
        return;
    }
    surface_t *bg = splash_background();
    if (bg) gfx_blit(prog, 0, 0, bg);
    else gfx_gradient_v(prog, 0, 0, B.w, B.h, SPLASH_BG_TOP, SPLASH_BG_BOTTOM);
    surface_t *saved_base = B.base;
    B.base = prog;                     /* draw_bar restores from B.base */
    /* no keyboard focus on Cancel until Tab: the Enter that started the installation
       (or its key repeat) must not cancel it */
    B.hover = B.pressed = B.fid = E_NONE;
    draw_progress(frame, 0);
    splash_crossfade(B.scene, frame, FADE_MS);
    gfx_blit(B.scene, 0, 0, frame);
    surface_destroy(frame);
    B.hover = rect_contains(B.cancel, B.mx, B.my) ? E_CANCEL : E_NONE;
    draw_progress(B.scene, 0);
    show_all(true);

    static const int cancel_id[] = { E_CANCEL };
    int result = job ? 0 : DISK_EINVAL, shown = 0;
    bool cancel = false;
    uint32_t t0 = g_ticks;
    while (job && result == 0) {
        result = install_step(job, 25);
        if (result) break;
        int p = install_progress(job), key = 0, mods = 0, ox = B.mx, oy = B.my;
        bool dirty = false;
        int a = poll_input(&B.cancel, cancel_id, 1, &dirty, &key, &mods);
        if (key == KEY_ESCAPE || ((key == KEY_ENTER || key == KEY_SPACE) && B.fid == E_CANCEL)) a = E_CANCEL;
        else if (key == KEY_TAB) { B.fid = E_CANCEL; B.focus_visible = true; dirty = true; }
        if (a == E_CANCEL) { cancel = true; break; }
        if (dirty) {
            shown = p;
            draw_progress(B.scene, p);
            show_all(true);
            continue;
        }
        if (p / 10 != shown / 10) {
            shown = p;
            draw_bar(B.scene, p);
            show_rect(rect_make(B.bar.x - 4, B.bar.y - 4, B.bar.w + 8, B.bar.h + 8));
        }
        if (ox != B.mx || oy != B.my) show_pointer_move(ox, oy);
    }
    if (cancel) {
        klog("bootsetup: Cancel at %d%% - stopping the installation", install_progress(job) / 10);
        install_cancel(job);
        install_free(job);
        back_to_setup(saved_base);
        surface_destroy(prog);
        return;
    }
    if (result == 1) {
        klog("bootsetup: drive installed in %u ms", g_ticks - t0);
        install_free(job);
        finished();                    /* does not return: Restart */
    }
    char why[200];
    snprintf(why, sizeof why, "%s", job ? install_error(job) : err);
    if (!why[0]) strlcpy(why, "The installation could not be started.", sizeof why);
    klog("bootsetup: installation failed: %s", why);
    install_free(job);
    sys_beep(220, 200);
    gfx_blit(wm_backbuf, 0, 0, B.base);
    draw_bar(wm_backbuf, shown);
    draw_message(wm_backbuf, "The drive could not be installed", why, C_WARN);
    draw_text_center(wm_backbuf, rect_make(0, B.h - K(80), B.w, font_height(FONT_UI)), "Click to go back", C_DIM, FONT_UI);
    present_rect(screen());
    wait_click_or_key();
    back_to_setup(saved_base);
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

/* ---- the save file screen (OSaver, item 14) ------------------------------------------------------------ */
static void prompt_done(void *ctx) {
    (void)ctx;
    klog("bootsetup: save file screen closed");
}

/* Called when the desktop exists but before it fades in: after Continue ISO the save
   file screen is the next thing on screen (the fade goes straight from this screen to
   it), and the desktop shows when it closes. */
void bootsetup_after_desktop(void) {
    if (!B.continued) return;
    if (!osaver_boot_prompt) {
        klog("bootsetup: no save file screen in this build");
        return;
    }
    klog("bootsetup: save file screen");
    osaver_boot_prompt(prompt_done, NULL);
    /* it is faded in together with the desktop, not on its own afterwards */
    for (window_t *w = wm_bottom; w; w = w->above)
        if (w->opacity < 255) w->opacity = 255;
}
