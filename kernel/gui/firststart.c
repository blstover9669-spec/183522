/*
 * firststart.c - the installation menu of an installed drive's first start
 * (NovaOS 2.5).
 *
 * A new drive holds only what it needs to boot: the NovaOS Drive installation
 * (and the Installer app) write the boot loader and the kernel, no files. So the
 * drive's first start (persist_first_start: no saved files yet, or only the files
 * the Installer copied) is an installation: after the splash and the loading bar
 * this menu runs instead of "No drive found" / "System requirement fail" - those
 * checks run after it has finished (kernel/core/kmain.c). Pages:
 *
 *   1. "This system now supports": HTML (with its new built-in app, HTML
 *      Viewer), CSS, JS, C, C++.                                      [Next]
 *   2. "Your account": Account name, Profile picture (the classic person icon,
 *      the built-in pictures drawn by NovaOS, or a picture from Pictures),
 *      Password (optional) and Confirm password, with a live preview of the
 *      picture and the name. Changeable later in Settings > Account.
 *                                                          [Back] [Install]
 *   3. "Installing NovaOS": a progress bar while the system files (/system:
 *      kernel, boot loader, UI files, /system/apps), the default folders and
 *      files and the account (the password only as a salted SHA-256 hash) are
 *      written and saved to the drive.
 *
 * Then the start screen follows (the splash cross-fade goes from this screen to
 * it). Nothing is saved before page 3 has finished, so a computer switched off
 * during the menu starts with the menu again.
 *
 * Like the NovaOS Drive installation it runs before the window manager's main
 * loop: it reads the input queue itself, draws into the back buffer and presents
 * it. Keyboard: Tab / Shift+Tab move between the fields and buttons, typing goes
 * into the focused field, Enter goes on (Next, the next field, Install), Esc goes
 * back, Left / Right pick a picture when the pictures have the focus.
 *
 * Automated starts (autorun=, testmouse=) without setup=1 do not show it: they
 * install with the defaults at once (account "User", the person icon, no
 * password). setup=0 does the same on any start. Log: "firststart: ready",
 * "firststart: page N (...)", "firststart: installing ...", "firststart: done".
 */
#include "wm_internal.h"
#include "bootscreen.h"
#include <nova/disk.h>
#include <nova/account.h>

#define MOVE_MS     650
#define FADE_MS     220
#define FRAME_MS    14
#define BLINK_MS    530
#define MAX_THUMBS  7
#define MAX_EL      24

enum { P_SUPPORTS, P_ACCOUNT, P_INSTALL };
enum { E_NONE, E_NEXT, E_BACK, E_INSTALL, E_NAME, E_PW, E_PW2, E_PICS, E_PICFOLDER,
       E_PIC = 40 /* + built-in picture */, E_THUMB = 60 /* + thumbnail */ };
enum { A_NONE, A_NEXT, A_BACK, A_INSTALL };

#define C_TEXT      RGB(240, 243, 255)
#define C_DIM       RGB(156, 166, 198)
#define C_OFF       RGB(104, 112, 142)
#define C_ACCENT    RGB(59, 130, 246)
#define C_WARN      RGB(244, 184, 96)

static struct {
    int w, h;
    double k;
    surface_t *base;                   /* background + the icon at the top      */
    surface_t *scene;                  /* the page without the pointer          */
    int page;
    int mx, my, buttons, acc_x, acc_y;
    int hover, pressed, focus;
    bool focus_visible;
    rect_t logo, title, dots, panel;
    rect_t el[MAX_EL];
    int el_id[MAX_EL], nel;
    /* account page */
    char name[ACCOUNT_NAME_MAX + 1];
    char pw[ACCOUNT_PASS_MAX + 1], pw2[ACCOUNT_PASS_MAX + 1];
    int pic;                           /* built-in picture, or E_THUMB-based: thumb + 100 */
    bool pics_open, pics_loaded;
    int nthumbs;
    char thumb_path[MAX_THUMBS][VFS_PATH_MAX];
    surface_t *thumb[MAX_THUMBS];      /* round, tile size                       */
    surface_t *preview;                /* the chosen file picture, preview size  */
    int preview_of;
    char error[96];
    uint32_t blink;
    /* layout of the account page */
    rect_t prev_pic, prev_name, lab_name, f_name, lab_pic, tiles[ACCOUNT_PIC_BUILTIN], folder, thumbs[MAX_THUMBS],
           lab_pw, f_pw, lab_pw2, f_pw2, err;
    rect_t next, back, install, bar, status;
    int tile;
} F;

static int K(double v) { return (int)lround(v * F.k); }
static rect_t screen(void) { return rect_make(0, 0, F.w, F.h); }
static int pic_is_thumb(int pic) { return pic >= 100; }

bool firststart_pending(void) { return persist_first_start(); }

/* ---- the work: system files, default files, account, save ------------------------------------- */
static const char *const step_text[] = {
    "Writing the system files", "Writing the UI files", "Writing the apps",
    "Writing the default folders and files", "Saving your account", "Saving to the drive",
};
static const int step_end[] = { 300, 440, 540, 640, 700, 1000 };
#define NSTEPS ((int)ARRAY_SIZE(step_text))

static int do_step(int i) {
    switch (i) {
    case 0: sysfiles_seed_part(SYSFILES_BOOT); break;
    case 1: sysfiles_seed_part(SYSFILES_UI); break;
    case 2: sysfiles_seed_part(SYSFILES_APPS); break;
    case 3: vfs_write_default_files(true); break;
    case 4: {
        int r = account_set_name(F.name[0] ? F.name : "User");
        if (pic_is_thumb(F.pic) && F.pic - 100 < F.nthumbs) {
            if (account_set_picture_file(F.thumb_path[F.pic - 100]) != ACCOUNT_OK) account_set_picture(ACCOUNT_PIC_PERSON);
        } else {
            account_set_picture(pic_is_thumb(F.pic) ? ACCOUNT_PIC_PERSON : F.pic);
        }
        if (F.pw[0]) account_set_password(NULL, F.pw);
        memset(F.pw, 0, sizeof F.pw);
        memset(F.pw2, 0, sizeof F.pw2);
        return r;
    }
    case 5: return persist_save();
    default: break;
    }
    return 0;
}

/* ---- pointer, presenting (as in bootsetup.c) --------------------------------------------- */
static void present_rect(rect_t r) {
    r = rect_intersect(r, screen());
    if (!rect_empty(r)) fb_present(wm_backbuf, r);
}

static int cursor_shape(void) {
    int h = F.hover;
    if (h == E_NAME || h == E_PW || h == E_PW2) return CURSOR_IBEAM;
    return h != E_NONE ? CURSOR_HAND : CURSOR_ARROW;
}

static void show_all(bool pointer) {
    gfx_blit(wm_backbuf, 0, 0, F.scene);
    if (pointer) cursor_draw(wm_backbuf, F.mx, F.my, cursor_shape());
    present_rect(screen());
}

static void show_pointer_move(int ox, int oy, int oshape) {
    rect_t r = rect_intersect(rect_union(cursor_rect(ox, oy, oshape), cursor_rect(F.mx, F.my, cursor_shape())), screen());
    if (rect_empty(r)) return;
    gfx_blit_rect(wm_backbuf, r.x, r.y, F.scene, r.x, r.y, r.w, r.h);
    cursor_draw(wm_backbuf, F.mx, F.my, cursor_shape());
    present_rect(r);
}

static void motion(int dx, int dy) {
    if (!dx && !dy) return;
    if (g_test_mouse) {
        F.mx += dx;
        F.my += dy;
    } else {
        int mult = g_mouse_speed * 16 / 5;
        if (abs(dx) + abs(dy) > 10) mult = mult * 3 / 2;
        F.acc_x += dx * mult;
        F.acc_y += dy * mult;
        int mx = F.acc_x / 16, my = F.acc_y / 16;
        F.acc_x -= mx * 16;
        F.acc_y -= my * 16;
        F.mx += mx;
        F.my += my;
    }
    F.mx = CLAMP(F.mx, 0, F.w - 1);
    F.my = CLAMP(F.my, 0, F.h - 1);
}

static void idle(void) {
    __asm__ volatile("cli");
    if (input_pending()) __asm__ volatile("sti");
    else __asm__ volatile("sti; hlt");
}

/* ---- layout ---------------------------------------------------------------------------------- */
static void add_el(rect_t r, int id) {
    if (F.nel < MAX_EL) { F.el[F.nel] = r; F.el_id[F.nel++] = id; }
}

static int btn_h(void) { return MAX(32, K(44)); }
static int field_h(void) { return MAX(30, K(40)); }

static void layout(void) {
    int w = F.w, ls = K(64);
    F.logo = rect_make((w - ls) / 2, K(26), ls, ls);
    int th = font_height(FONT_UI_TITLE);
    F.title = rect_make(0, F.logo.y + ls + K(12), w, th);
    F.dots = rect_make(0, F.title.y + th + K(10), w, MAX(6, K(8)));
    int pw = MIN(w - 32, K(720)), pad = K(24), top = F.dots.y + F.dots.h + K(22);
    F.nel = 0;
    int bw = MAX(K(150), font_text_width(FONT_UI_LARGE, "Install") + K(64)), bh = btn_h();
    if (F.page == P_SUPPORTS) {
        int rowh = K(58);
        F.panel = rect_make((w - pw) / 2, top, pw, pad + 5 * rowh + pad);
        F.next = rect_make(F.panel.x + pw - bw, F.panel.y + F.panel.h + K(22), bw, bh);
        add_el(F.next, E_NEXT);
        return;
    }
    if (F.page == P_INSTALL) {
        F.panel = rect_make((w - pw) / 2, top, pw, K(150));
        int barw = pw - 2 * K(48), barh = MAX(26, K(34));
        F.bar = rect_make(F.panel.x + K(48), F.panel.y + K(40), barw, barh);
        F.status = rect_make(F.panel.x, F.bar.y + barh + K(20), pw, font_height(FONT_UI_LARGE));
        return;
    }
    /* the account page: the preview on the left, the fields on the right */
    int colw = K(196), x = (w - pw) / 2, y = top + pad;
    int rx = x + pad + colw + K(24), rw = x + pw - pad - rx, lh = font_height(FONT_UI_BOLD), fh = field_h();
    int ps = K(124);
    F.prev_pic = rect_make(x + pad + (colw - ps) / 2, y + K(12), ps, ps);
    F.prev_name = rect_make(x + pad, F.prev_pic.y + ps + K(14), colw, font_height(FONT_UI_LARGE));
    F.lab_name = rect_make(rx, y, rw, lh);
    F.f_name = rect_make(rx, y + lh + K(6), rw, fh);
    y = F.f_name.y + fh + K(16);
    F.lab_pic = rect_make(rx, y, rw, lh);
    int ts = K(44), gap = K(10), ty = y + lh + K(6);
    for (int i = 0; i < ACCOUNT_PIC_BUILTIN; i++) F.tiles[i] = rect_make(rx + i * (ts + gap), ty, ts, ts);
    int fx = rx + ACCOUNT_PIC_BUILTIN * (ts + gap) + K(4);
    F.folder = rect_make(fx, ty + (ts - K(34)) / 2, MIN(rx + rw - fx, K(112)), K(34));
    y = ty + ts + K(10);
    for (int i = 0; i < MAX_THUMBS; i++) F.thumbs[i] = rect_make(rx + i * (ts + gap), y, ts, ts);
    y += ts + K(16);
    F.lab_pw = rect_make(rx, y, rw, lh);
    F.f_pw = rect_make(rx, y + lh + K(6), rw, fh);
    y = F.f_pw.y + fh + K(14);
    F.lab_pw2 = rect_make(rx, y, rw, lh);
    F.f_pw2 = rect_make(rx, y + lh + K(6), rw, fh);
    F.err = rect_make(rx, F.f_pw2.y + fh + K(10), rw, font_height(FONT_UI_BOLD));
    F.panel = rect_make(x, top, pw, F.err.y + F.err.h + pad - top);
    F.install = rect_make(x + pw - bw, F.panel.y + F.panel.h + K(22), bw, bh);
    int bbw = MAX(K(120), font_text_width(FONT_UI_LARGE, "Back") + K(64));
    F.back = rect_make(F.install.x - K(14) - bbw, F.install.y, bbw, bh);
    add_el(F.f_name, E_NAME);
    for (int i = 0; i < ACCOUNT_PIC_BUILTIN; i++) add_el(F.tiles[i], E_PIC + i);
    add_el(F.folder, E_PICFOLDER);
    if (F.pics_open)
        for (int i = 0; i < F.nthumbs; i++) add_el(F.thumbs[i], E_THUMB + i);
    add_el(F.f_pw, E_PW);
    add_el(F.f_pw2, E_PW2);
    add_el(F.back, E_BACK);
    add_el(F.install, E_INSTALL);
}

/* ---- drawing ----------------------------------------------------------------------------------- */
static void text_center(surface_t *s, rect_t r, const char *t, uint32_t c, const font_t *f) {
    gfx_text_aligned(s, r, t, c, f, ALIGN_CENTER | ALIGN_MIDDLE);
}

static void draw_button(surface_t *s, rect_t r, const char *label, bool primary, int id) {
    int rad = MIN(K(10), r.h / 2);
    bool hov = F.hover == id, prs = hov && F.pressed == id;
    if (primary) {
        uint32_t c = prs ? RGB(37, 99, 220) : hov ? RGB(92, 156, 255) : C_ACCENT;
        gfx_fill_rounded_gradient_v(s, r.x, r.y, r.w, r.h, rad, color_shade(c, 18), color_shade(c, -10));
    } else {
        gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, rad, RGBA(255, 255, 255, prs ? 44 : hov ? 28 : 12));
        gfx_rounded_rect(s, r.x, r.y, r.w, r.h, rad, RGBA(255, 255, 255, 96));
    }
    if (F.focus == id && F.focus_visible)
        gfx_rounded_rect(s, r.x - 3, r.y - 3, r.w + 6, r.h + 6, rad + 3, RGBA(170, 205, 255, 230));
    text_center(s, r, label, COLOR_WHITE, FONT_UI_LARGE);
}

static void draw_panel(surface_t *s) {
    rect_t p = F.panel;
    gfx_fill_rounded_rect(s, p.x, p.y, p.w, p.h, K(16), RGBA(255, 255, 255, 12));
    gfx_rounded_rect(s, p.x, p.y, p.w, p.h, K(16), RGBA(255, 255, 255, 34));
}

static void draw_dots(surface_t *s) {
    int d = F.dots.h, gap = K(8), wide = d * 3, total = 2 * d + wide + 2 * gap, x = (F.w - total) / 2;
    for (int i = 0; i < 3; i++) {
        int dw = i == F.page ? wide : d;
        gfx_fill_rounded_rect(s, x, F.dots.y, dw, d, d / 2, i == F.page ? C_ACCENT : RGBA(255, 255, 255, 60));
        x += dw + gap;
    }
}

/* A file-type tile: rounded square in the type's colour with its extension. */
static void type_tile(surface_t *s, int x, int y, int size, const char *ext, uint32_t c, uint32_t tc) {
    gfx_fill_rounded_gradient_v(s, x, y, size, size, size / 4, color_shade(c, 22), color_shade(c, -12));
    gfx_fill_rounded_rect(s, x + 2, y + 2, size - 4, size / 2 - 2, size / 4 - 2, RGBA(255, 255, 255, 30));
    text_center(s, rect_make(x, y, size, size), ext, tc, FONT_UI_BOLD);
}

static void draw_supports(surface_t *s) {
    static const struct { const char *ext, *text; uint32_t c, tc; } rows[5] = {
        { "html", "HTML (with its new built-in app, HTML Viewer)", RGB(236, 100, 52), COLOR_WHITE },
        { "css", "CSS", RGB(52, 118, 232), COLOR_WHITE },
        { "js", "JS", RGB(240, 204, 48), RGB(40, 36, 20) },
        { "c", "C", RGB(92, 112, 176), COLOR_WHITE },
        { "c++", "C++", RGB(132, 88, 214), COLOR_WHITE },
    };
    draw_panel(s);
    int pad = K(24), rowh = K(58), ts = K(42);
    for (int i = 0; i < 5; i++) {
        int y = F.panel.y + pad + i * rowh;
        if (i) gfx_hline(s, F.panel.x + pad, F.panel.x + F.panel.w - pad, y, RGBA(255, 255, 255, 22));
        type_tile(s, F.panel.x + pad + K(6), y + (rowh - ts) / 2, ts, rows[i].ext, rows[i].c, rows[i].tc);
        gfx_text_aligned(s, rect_make(F.panel.x + pad + K(6) + ts + K(20), y, F.panel.w, rowh), rows[i].text, C_TEXT,
                         FONT_UI_LARGE, ALIGN_LEFT | ALIGN_MIDDLE);
    }
    draw_button(s, F.next, "Next", true, E_NEXT);
}

static void draw_field(surface_t *s, rect_t r, const char *text, bool password, const char *placeholder, int id) {
    bool focus = F.focus == id;
    int rad = MIN(K(10), r.h / 2);
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, rad, RGBA(255, 255, 255, focus ? 30 : F.hover == id ? 24 : 16));
    gfx_rounded_rect(s, r.x, r.y, r.w, r.h, rad, focus ? RGBA(130, 176, 255, 235) : RGBA(255, 255, 255, 70));
    int tx = r.x + K(14), cy = r.y + r.h / 2, len = (int)strlen(text), caret = tx;
    rect_t old = s->clip;
    surface_set_clip(s, rect_intersect(old, rect_make(r.x + 4, r.y + 2, r.w - 8, r.h - 4)));
    if (!len) {
        if (placeholder) gfx_text_aligned(s, rect_make(tx, r.y, r.w, r.h), placeholder, C_OFF, FONT_UI_LARGE,
                                          ALIGN_LEFT | ALIGN_MIDDLE);
    } else if (password) {
        double dr = MAX(3.0, K(4)), step = dr * 2 + K(5);
        for (int i = 0; i < len; i++) gfx_fill_circle_aa(s, tx + dr + i * step, cy, dr, C_TEXT);
        caret = (int)(tx + len * step);
    } else {
        int tw = font_text_width(FONT_UI_LARGE, text), avail = r.w - K(28);
        int x = tw > avail ? tx - (tw - avail) : tx;
        gfx_text_aligned(s, rect_make(x, r.y, tw + 4, r.h), text, C_TEXT, FONT_UI_LARGE, ALIGN_LEFT | ALIGN_MIDDLE);
        caret = x + tw + 1;
    }
    if (focus && F.blink % 2 == 0) gfx_fill_rect(s, caret, cy - K(10), 2, K(20), C_TEXT);
    s->clip = old;
}

static void draw_tile_ring(surface_t *s, rect_t r, bool sel, bool hov) {
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0, rr = r.w / 2.0 + K(4);
    if (sel) gfx_circle_aa(s, cx, cy, rr, MAX(2.0, K(2.6)), RGB(130, 176, 255));
    else if (hov) gfx_circle_aa(s, cx, cy, rr, 1.5, RGBA(255, 255, 255, 140));
}

static void draw_account(surface_t *s) {
    draw_panel(s);
    /* preview */
    rect_t p = F.prev_pic;
    gfx_fill_circle_aa(s, p.x + p.w / 2.0, p.y + p.h / 2.0, p.w / 2.0 + K(4), RGBA(255, 255, 255, 50));
    if (pic_is_thumb(F.pic) && F.preview) gfx_blit_alpha(s, p.x, p.y, F.preview, 255);
    else account_draw_builtin(pic_is_thumb(F.pic) ? ACCOUNT_PIC_PERSON : F.pic, s, p.x, p.y, p.w);
    if (F.name[0]) {
        char shown[ACCOUNT_NAME_MAX + 4];
        strlcpy(shown, F.name, sizeof shown);
        gfx_text_aligned(s, F.prev_name, shown, C_TEXT, FONT_UI_LARGE, ALIGN_CENTER | ALIGN_MIDDLE);
    } else {
        gfx_text_aligned(s, F.prev_name, "Account name", C_OFF, FONT_UI_LARGE, ALIGN_CENTER | ALIGN_MIDDLE);
    }
    /* fields */
    gfx_text(s, F.lab_name.x + 2, F.lab_name.y, "Account name", C_DIM, FONT_UI_BOLD);
    draw_field(s, F.f_name, F.name, false, "Type a name", E_NAME);
    gfx_text(s, F.lab_pic.x + 2, F.lab_pic.y, "Profile picture", C_DIM, FONT_UI_BOLD);
    for (int i = 0; i < ACCOUNT_PIC_BUILTIN; i++) {
        rect_t t = F.tiles[i];
        account_draw_builtin(i, s, t.x, t.y, t.w);
        draw_tile_ring(s, t, F.pic == i, F.hover == E_PIC + i);
    }
    if (F.focus == E_PICS && F.focus_visible) {
        rect_t a = F.tiles[0], b = F.folder;
        gfx_rounded_rect(s, a.x - K(8), a.y - K(8), b.x + b.w - a.x + K(16), a.h + K(16), K(12), RGBA(170, 205, 255, 200));
    }
    rect_t f = F.folder;
    bool fh = F.hover == E_PICFOLDER;
    gfx_fill_rounded_rect(s, f.x, f.y, f.w, f.h, f.h / 2, RGBA(255, 255, 255, F.pics_open ? 46 : fh ? 30 : 14));
    gfx_rounded_rect(s, f.x, f.y, f.w, f.h, f.h / 2, RGBA(255, 255, 255, F.pics_open ? 150 : 80));
    int is = f.h - K(12);
    icon_draw(ICON_FOLDER_PICTURES, s, f.x + K(10), f.y + (f.h - is) / 2, is);
    gfx_text_aligned(s, rect_make(f.x + K(14) + is, f.y, f.w - is - K(18), f.h), "Pictures", C_TEXT, FONT_UI_BOLD,
                     ALIGN_LEFT | ALIGN_MIDDLE);
    if (F.pics_open) {
        if (!F.nthumbs) {
            rect_t r = F.thumbs[0];
            gfx_text_aligned(s, rect_make(r.x + 2, r.y, F.f_name.w, r.h), "There are no pictures in Pictures", C_OFF,
                             FONT_UI, ALIGN_LEFT | ALIGN_MIDDLE);
        }
        for (int i = 0; i < F.nthumbs; i++) {
            rect_t r = F.thumbs[i];
            if (F.thumb[i]) gfx_blit_alpha(s, r.x, r.y, F.thumb[i], 255);
            draw_tile_ring(s, r, F.pic == 100 + i, F.hover == E_THUMB + i);
        }
    }
    gfx_text(s, F.lab_pw.x + 2, F.lab_pw.y, "Password (optional)", C_DIM, FONT_UI_BOLD);
    draw_field(s, F.f_pw, F.pw, true, "No password", E_PW);
    gfx_text(s, F.lab_pw2.x + 2, F.lab_pw2.y, "Confirm password", C_DIM, FONT_UI_BOLD);
    draw_field(s, F.f_pw2, F.pw2, true, NULL, E_PW2);
    if (F.error[0]) {
        int g = font_height(FONT_UI_BOLD);
        ui_draw_glyph(s, UI_GLYPH_WARNING, F.err.x + 2, F.err.y, g, C_WARN);
        gfx_text(s, F.err.x + g + K(8), F.err.y, F.error, C_WARN, FONT_UI_BOLD);
    } else {
        gfx_text(s, F.err.x + 2, F.err.y, "You can change your account later in Settings.", C_OFF, FONT_UI);
    }
    draw_button(s, F.back, "Back", false, E_BACK);
    draw_button(s, F.install, "Install", true, E_INSTALL);
}

static void draw_bar(surface_t *s, int permille, const char *status) {
    rect_t b = F.bar;
    int rad = b.h / 2;
    gfx_fill_rounded_rect(s, b.x, b.y, b.w, b.h, rad, RGBA(255, 255, 255, 26));
    gfx_rounded_rect(s, b.x, b.y, b.w, b.h, rad, RGBA(255, 255, 255, 44));
    int fw = (int)((int64_t)b.w * CLAMP(permille, 0, 1000) / 1000);
    if (fw >= b.h) {
        gfx_fill_rounded_gradient_v(s, b.x, b.y, fw, b.h, rad, RGB(150, 120, 255), RGB(60, 190, 240));
    } else if (fw > 0) {
        surface_t sub = surface_sub(s, rect_make(b.x, b.y, fw, b.h));
        gfx_fill_rounded_gradient_v(&sub, 0, 0, b.h, b.h, rad, RGB(150, 120, 255), RGB(60, 190, 240));
    }
    char pct[8];
    snprintf(pct, sizeof pct, "%d%%", CLAMP(permille, 0, 1000) / 10);
    text_center(s, b, pct, COLOR_WHITE, FONT_UI_BOLD);
    text_center(s, F.status, status, C_DIM, FONT_UI_LARGE);
}

static const char *page_title(void) {
    return F.page == P_SUPPORTS ? "This system now supports" : F.page == P_ACCOUNT ? "Your account" : "Installing NovaOS";
}

static void draw_scene_status(int permille, const char *status) {
    surface_t *s = F.scene;
    gfx_blit(s, 0, 0, F.base);
    text_center(s, F.title, page_title(), C_TEXT, FONT_UI_TITLE);
    draw_dots(s);
    if (F.page == P_SUPPORTS) draw_supports(s);
    else if (F.page == P_ACCOUNT) draw_account(s);
    else { draw_panel(s); draw_bar(s, permille, status); }
}

static void draw_scene(void) { draw_scene_status(0, ""); }

/* ---- pictures from Pictures ------------------------------------------------------------------- */
static void load_thumbs(void) {
    if (F.pics_loaded) return;
    F.pics_loaded = true;
    vfs_dirent_t *ents = kmalloc(64 * sizeof *ents);
    if (!ents) return;
    int n = vfs_list("/home/user/Pictures", ents, 64);
    int ts = F.tiles[0].w;
    for (int i = 0; i < n && i < 64 && F.nthumbs < MAX_THUMBS; i++) {
        const char *ext = vfs_extension(ents[i].name);
        if (ents[i].is_dir || (strcasecmp(ext, "bmp") && strcasecmp(ext, "ppm"))) continue;
        char path[VFS_PATH_MAX];
        vfs_join("/home/user/Pictures", ents[i].name, path);
        uint32_t size = 0;
        void *data = vfs_read_file(path, &size);
        surface_t *img = data ? image_decode(data, size) : NULL;
        kfree(data);
        if (!img) continue;
        surface_t *t = surface_create_alpha(ts, ts);
        if (t) account_draw_image(img, t, 0, 0, ts);
        surface_destroy(img);
        if (!t) continue;
        strlcpy(F.thumb_path[F.nthumbs], path, VFS_PATH_MAX);
        F.thumb[F.nthumbs++] = t;
    }
    kfree(ents);
    klog("firststart: %d pictures in Pictures", F.nthumbs);
}

static void choose_thumb(int i) {
    if (i < 0 || i >= F.nthumbs) return;
    F.pic = 100 + i;
    if (F.preview_of == i && F.preview) return;
    surface_destroy(F.preview);
    F.preview = NULL;
    uint32_t size = 0;
    void *data = vfs_read_file(F.thumb_path[i], &size);
    surface_t *img = data ? image_decode(data, size) : NULL;
    kfree(data);
    if (img) {
        F.preview = surface_create_alpha(F.prev_pic.w, F.prev_pic.w);
        if (F.preview) account_draw_image(img, F.preview, 0, 0, F.prev_pic.w);
        surface_destroy(img);
    }
    F.preview_of = i;
}

/* ---- input ------------------------------------------------------------------------------------ */
static int hit(int x, int y) {
    for (int i = 0; i < F.nel; i++)
        if (rect_contains(F.el[i], x, y)) return F.el_id[i];
    return E_NONE;
}

static char *field_of(int id, int *max) {
    if (id == E_NAME) { *max = ACCOUNT_NAME_MAX; return F.name; }
    if (id == E_PW) { *max = ACCOUNT_PASS_MAX; return F.pw; }
    if (id == E_PW2) { *max = ACCOUNT_PASS_MAX; return F.pw2; }
    return NULL;
}

static const int focus_order[] = { E_NAME, E_PICS, E_PW, E_PW2, E_BACK, E_INSTALL };
#define NFOCUS ((int)ARRAY_SIZE(focus_order))

static void cycle_focus(bool back) {
    if (F.page != P_ACCOUNT) { F.focus = E_NEXT; return; }
    int cur = 0;
    for (int i = 0; i < NFOCUS; i++) if (focus_order[i] == F.focus) cur = i;
    F.focus = focus_order[(cur + (back ? NFOCUS - 1 : 1)) % NFOCUS];
}

/* Left / Right on the pictures: through the built-in ones, then the shown thumbnails. */
static void step_picture(int dir) {
    int n = ACCOUNT_PIC_BUILTIN + (F.pics_open ? F.nthumbs : 0);
    int cur = pic_is_thumb(F.pic) ? ACCOUNT_PIC_BUILTIN + F.pic - 100 : F.pic;
    cur = (cur + dir + n) % n;
    if (cur < ACCOUNT_PIC_BUILTIN) F.pic = cur;
    else choose_thumb(cur - ACCOUNT_PIC_BUILTIN);
}

static void toggle_pictures(void) {
    F.pics_open = !F.pics_open;
    if (F.pics_open) load_thumbs();
    layout();
}

static bool validate(void) {
    char name[ACCOUNT_NAME_MAX + 1];
    const char *p = F.name;
    while (*p == ' ') p++;
    strlcpy(name, p, sizeof name);
    int len = (int)strlen(name);
    while (len && name[len - 1] == ' ') name[--len] = 0;
    if (!len) { strlcpy(F.error, "Type an account name", sizeof F.error); F.focus = E_NAME; return false; }
    if (strcmp(F.pw, F.pw2)) {
        strlcpy(F.error, "The passwords do not match", sizeof F.error);
        F.pw2[0] = 0;
        F.focus = E_PW2;
        return false;
    }
    strlcpy(F.name, name, sizeof F.name);
    F.error[0] = 0;
    return true;
}

/* The action of an element that was clicked or pressed with Enter / Space. */
static int activate(int id) {
    if (id == E_NEXT) return A_NEXT;
    if (id == E_BACK) return A_BACK;
    if (id == E_INSTALL) return A_INSTALL;
    if (id >= E_PIC && id < E_PIC + ACCOUNT_PIC_BUILTIN) { F.pic = id - E_PIC; F.focus = E_PICS; }
    else if (id >= E_THUMB && id < E_THUMB + MAX_THUMBS) { choose_thumb(id - E_THUMB); F.focus = E_PICS; }
    else if (id == E_PICFOLDER) { toggle_pictures(); F.focus = E_PICS; }
    else if (id == E_NAME || id == E_PW || id == E_PW2) F.focus = id;
    return A_NONE;
}

static int key(const input_event_t *ev) {
    F.focus_visible = true;
    int k = ev->key;
    bool shift = ev->mods & MOD_SHIFT;
    if (k == KEY_TAB) { cycle_focus(shift); return A_NONE; }
    if (F.page == P_SUPPORTS) {
        if (k == KEY_ENTER || k == KEY_SPACE) return A_NEXT;
        return A_NONE;
    }
    if (k == KEY_ESCAPE) return A_BACK;
    int max;
    char *f = field_of(F.focus, &max);
    if (k == KEY_ENTER) {
        if (F.focus == E_NAME) { F.focus = E_PW; return A_NONE; }
        if (F.focus == E_PW) { F.focus = E_PW2; return A_NONE; }
        if (F.focus == E_PICS) { toggle_pictures(); return A_NONE; }
        if (F.focus == E_BACK) return A_BACK;
        return A_INSTALL;
    }
    if (f) {
        int len = (int)strlen(f);
        if (k == KEY_BACKSPACE) { if (len) f[len - 1] = 0; return A_NONE; }
        if (ev->ch >= 32 && ev->ch < 127 && !(ev->mods & (MOD_CTRL | MOD_ALT)) && len < max &&
            !(F.focus == E_NAME && (ev->ch == '=' || (ev->ch == ' ' && !len)))) {
            f[len] = (char)ev->ch;
            f[len + 1] = 0;
            if (F.error[0]) F.error[0] = 0;
        }
        return A_NONE;
    }
    if (F.focus == E_PICS) {
        if (k == KEY_LEFT || k == KEY_RIGHT) step_picture(k == KEY_LEFT ? -1 : 1);
        else if (k == KEY_SPACE) toggle_pictures();
        return A_NONE;
    }
    if (k == KEY_SPACE) return activate(F.focus);
    return A_NONE;
}

/* One page until it is left: returns A_NEXT / A_BACK / A_INSTALL. */
static int interact(void) {
    layout();
    F.hover = hit(F.mx, F.my);
    draw_scene();
    show_all(true);
    for (;;) {
        input_event_t ev;
        bool dirty = false;
        int ox = F.mx, oy = F.my, oshape = cursor_shape(), adx = 0, ady = 0, action = A_NONE;
        while (action == A_NONE && input_pop(&ev)) {
            if (ev.type == IN_MOUSE) {
                if (ev.buttons == F.buttons) { adx += ev.dx; ady += ev.dy; continue; }
                motion(adx + ev.dx, ady + ev.dy);
                adx = ady = 0;
                int was = F.buttons, h = hit(F.mx, F.my);
                F.buttons = ev.buttons;
                if (h != F.hover) { F.hover = h; dirty = true; }
                if ((ev.buttons & 1) && !(was & 1)) {
                    F.pressed = h;
                    F.focus_visible = false;
                    if (h == E_NAME || h == E_PW || h == E_PW2) F.focus = h;
                    dirty = true;
                } else if (!(ev.buttons & 1) && (was & 1)) {
                    int p = F.pressed;
                    F.pressed = E_NONE;
                    dirty = true;
                    if (p != E_NONE && p == h) action = activate(p);
                }
            } else if (ev.type == IN_KEY && ev.pressed) {
                if (adx || ady) { motion(adx, ady); adx = ady = 0; }
                action = key(&ev);
                F.blink = 0;
                dirty = true;
            }
        }
        if (adx || ady) motion(adx, ady);
        if (action == A_INSTALL && !validate()) { action = A_NONE; dirty = true; }
        if (action != A_NONE) {
            F.hover = F.pressed = E_NONE;
            return action;
        }
        uint32_t b = g_ticks / BLINK_MS;
        if (F.page == P_ACCOUNT && (F.focus == E_NAME || F.focus == E_PW || F.focus == E_PW2) && b != F.blink) {
            F.blink = b;
            dirty = true;
        }
        layout();
        int h = hit(F.mx, F.my);
        if (h != F.hover) { F.hover = h; dirty = true; }
        if (dirty) { draw_scene(); show_all(true); }
        else if (ox != F.mx || oy != F.my || oshape != cursor_shape()) show_pointer_move(ox, oy, oshape);
        idle();
    }
}

/* ---- pages, transitions ---------------------------------------------------------------------------- */
static void go_page(int page) {
    surface_t *from = surface_clone(wm_backbuf);
    F.page = page;
    F.focus = page == P_ACCOUNT ? E_NAME : page == P_SUPPORTS ? E_NEXT : E_NONE;
    F.focus_visible = false;
    F.hover = F.pressed = E_NONE;
    layout();
    draw_scene();
    if (from) splash_crossfade(from, F.scene, FADE_MS);
    surface_destroy(from);
    klog("firststart: page %d (%s)", page + 1, page_title());
}

static double ease_in_out(double t) { return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2; }

/* The splash icon moves from the middle to the top (and loses its glow). */
static void move_icon_up(void) {
    if (!splash_active()) {
        gfx_gradient_v(wm_backbuf, 0, 0, F.w, F.h, SPLASH_BG_TOP, SPLASH_BG_BOTTOM);
        ui_draw_logo(wm_backbuf, F.logo.x, F.logo.y, F.logo.w);
        present_rect(screen());
        return;
    }
    rect_t from = splash_logo_rect();
    uint32_t t0 = g_ticks, next = t0;
    for (;;) {
        uint32_t el = g_ticks - t0;
        double t = el >= MOVE_MS ? 1.0 : (double)el / MOVE_MS, e = ease_in_out(t);
        int sz = (int)lround(from.w + (F.logo.w - from.w) * e);
        int x = (int)lround(from.x + (F.logo.x - from.x) * e);
        int y = (int)lround(from.y + (F.logo.y - from.y) * e);
        splash_place_logo(x, y, sz, (int)lround(255 * (1.0 - e)));
        if (t >= 1.0) break;
        next += FRAME_MS;
        if ((int32_t)(next - g_ticks) < 0) next = g_ticks;
        boot_sleep_until(next);
    }
}

/* Page 3: the bar moves through the steps while they run. */
static void animate_bar(int *shown, int to, const char *status) {
    int from = *shown;
    uint32_t t0 = g_ticks, dur = 120 + (uint32_t)abs(to - from) / 3;
    for (;;) {
        uint32_t el = g_ticks - t0;
        double t = el >= dur ? 1.0 : (double)el / dur, e = t * t * (3 - 2 * t);
        *shown = from + (int)lround((to - from) * e);
        draw_scene_status(*shown, status);
        gfx_blit_rect(wm_backbuf, F.panel.x, F.panel.y, F.scene, F.panel.x, F.panel.y, F.panel.w, F.panel.h);
        present_rect(F.panel);
        if (t >= 1.0) break;
        boot_sleep_until(g_ticks + FRAME_MS);
    }
}

static void install_ui(void) {
    go_page(P_INSTALL);
    uint32_t t0 = g_ticks;
    int shown = 0, prev = 0;
    klog("firststart: installing (account \"%s\", %s, %s)", F.name,
         pic_is_thumb(F.pic) ? "a picture from Pictures" : account_builtin_name(F.pic), F.pw[0] ? "password" : "no password");
    for (int i = 0; i < NSTEPS; i++) {
        animate_bar(&shown, prev + (step_end[i] - prev) / 3, step_text[i]);
        int r = do_step(i);
        if (i == NSTEPS - 1 && r != DISK_OK) {
            klog("firststart: saving to the drive failed: %s", disk_strerror(r));
            char msg[128];
            snprintf(msg, sizeof msg, "Saving to the drive failed (%s) - NovaOS tries again later", disk_strerror(r));
            animate_bar(&shown, 1000, msg);
            boot_sleep_until(g_ticks + 2500);
        }
        animate_bar(&shown, step_end[i], step_text[i]);
        prev = step_end[i];
    }
    animate_bar(&shown, 1000, "Done");
    klog("firststart: done in %u ms - the system files, the default folders and files and the account are on the drive",
         g_ticks - t0);
    boot_sleep_until(g_ticks + 450);
}

static void install_auto(void) {
    uint32_t t0 = g_ticks;
    strlcpy(F.name, "User", sizeof F.name);
    F.pic = ACCOUNT_PIC_PERSON;
    for (int i = 0; i < NSTEPS; i++) {
        int r = do_step(i);
        if (i == NSTEPS - 1 && r != DISK_OK) klog("firststart: saving to the drive failed: %s", disk_strerror(r));
    }
    klog("firststart: done in %u ms (automated start: account \"User\", no password)", g_ticks - t0);
}

static bool menu_wanted(void) {
    int f = boot_flag("setup");
    if (f >= 0) return f == 1;
    return !boot_automated();
}

bool firststart_run(void) {
    memset(&F, 0, sizeof F);
    F.preview_of = -1;
    if (!menu_wanted() || !wm_backbuf) {
        klog("firststart: the drive's first start - automated start, installing with the defaults");
        install_auto();
        return false;
    }
    F.w = wm_backbuf->w;
    F.h = wm_backbuf->h;
    F.k = CLAMP(F.h / 768.0, 0.72, 1.6);
    F.mx = wm_mouse_x;
    F.my = wm_mouse_y;
    F.buttons = wm_mouse_buttons;
    F.pic = ACCOUNT_PIC_PERSON;
    F.page = P_SUPPORTS;
    layout();
    klog("firststart: the drive's first start - installation menu");
    splash_bar_hide();
    move_icon_up();
    F.base = surface_clone(wm_backbuf);
    F.scene = surface_clone(wm_backbuf);
    if (!F.base || !F.scene) {
        surface_destroy(F.base);
        surface_destroy(F.scene);
        klog("firststart: out of memory - installing with the defaults");
        install_auto();
        return false;
    }
    F.focus = E_NEXT;
    draw_scene();
    splash_crossfade(F.base, F.scene, FADE_MS);
    klog("firststart: ready (page 1, %s)", page_title());
    for (;;) {
        int a = interact();
        if (a == A_NEXT) go_page(P_ACCOUNT);
        else if (a == A_BACK && F.page == P_ACCOUNT) go_page(P_SUPPORTS);
        else if (a == A_INSTALL) break;
    }
    install_ui();
    /* the last frame stays on screen: the splash fades from it to the start screen */
    gfx_blit(wm_backbuf, 0, 0, F.scene);
    present_rect(screen());
    wm_mouse_x = F.mx;
    wm_mouse_y = F.my;
    wm_mouse_buttons = F.buttons;
    surface_destroy(F.base);
    surface_destroy(F.scene);
    surface_destroy(F.preview);
    for (int i = 0; i < F.nthumbs; i++) surface_destroy(F.thumb[i]);
    F.base = F.scene = F.preview = NULL;
    return true;
}
