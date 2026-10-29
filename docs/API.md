# NovaOS Application API

This is the guide for writing NovaOS apps (and for extending the system). The
authoritative declarations are the headers in `include/`; everything below is
available after

```c
#include <nova.h>
```

* [1. How NovaOS runs your code](#1-how-novaos-runs-your-code)
* [2. A complete example app](#2-a-complete-example-app)
* [3. Registering an app](#3-registering-an-app)
* [4. Windows](#4-windows)
* [5. Events](#5-events)
* [6. Painting](#6-painting)
* [7. Timers and long-running work](#7-timers-and-long-running-work)
* [8. Graphics](#8-graphics)
* [9. Text and fonts](#9-text-and-fonts)
* [10. Widgets](#10-widgets)
* [11. Dialogs, notifications, clipboard](#11-dialogs-notifications-clipboard)
* [12. Theme](#12-theme)
* [13. Filesystem (VFS)](#13-filesystem-vfs)
* [14. System services](#14-system-services)
* [15. Hardware access (drivers)](#15-hardware-access-drivers)
* [16. libc / libm subset](#16-libc--libm-subset)
* [17. Kernel command line](#17-kernel-command-line)
* [18. Rules, conventions and gotchas](#18-rules-conventions-and-gotchas)
* [19. Background services, system log, commands](#19-background-services-system-log-commands)
* [20. Opening files, file types, pins, recent items](#20-opening-files-file-types-pins-recent-items)
* [21. System icons](#21-system-icons)
* [22. Sound](#22-sound)
* [23. Networking (HTTP, HTTPS, cookies, URLs)](#23-networking-http-https-cookies-urls)

---

## 1. How NovaOS runs your code

* 32-bit protected mode, one address space, everything in ring 0. There are no
  processes: an "app" is a set of C functions linked into the kernel.
* **Single-threaded and event driven.** The kernel main loop
  (`kernel/gui/wm.c: wm_run`) repeatedly: drains the input queue (filled by the
  keyboard/mouse IRQ handlers), fires due timers, delivers posted events,
  sends `EV_PAINT` to windows that need it, composites dirty screen areas from
  a RAM back buffer to the framebuffer, then `hlt`s until the next interrupt.
* Your app gets control only inside callbacks: `launch()`, window event
  handlers, widget callbacks, timer callbacks, dialog callbacks.
  **Callbacks must return quickly. Never block, never busy-wait, never sleep.**
  A loop that waits for something freezes the whole OS. Split long work into
  chunks driven by a timer (see section 7).
* The x87 FPU is on (53-bit double precision, every exception masked);
  `float`/`double` are fine in apps. Do not use the FPU in IRQ handlers. Maths
  errors never stop the system: an x87 floating-point error (#MF) is cleared,
  the exceptions are masked again and the code continues (with the usual
  IEEE results such as NaN or infinity - check for them before converting to
  an integer index); a `[fpu]` warning goes to the system log. The window
  manager also resets any x87 state left over between two callbacks.
* The kernel stack is 256 KiB and shared by everything: keep big buffers on
  the heap (`malloc`), not in local arrays.

Source layout: `apps/*.c` (apps, picked up automatically), `kernel/` (kernel,
drivers, GUI), `include/` (public API), `tools/` (font generator, test
harness), `docs/`.

---

## 2. A complete example app

`apps/counter.c` - a window with a big number, a button that increments it, a
timer that animates a progress ring, and per-window state (open it twice and
each window counts on its own). Drop the file in `apps/`, run `make`, done.

```c
#include <nova.h>

typedef struct {
    int count;
    double angle;          /* animation */
    widget_t *label;
} counter_t;

static void on_click(widget_t *w, void *ctx) {
    counter_t *c = ctx;
    char buf[32];
    snprintf(buf, sizeof buf, "Clicked %d times", ++c->count);
    ui_set_text(c->label, buf);
    win_invalidate(ui_window(w));          /* repaint the big number */
    sys_beep(880, 30);                     /* non-blocking */
}

static void counter_event(window_t *win, const event_t *ev) {
    counter_t *c = win_get_userdata(win);
    const theme_t *t = theme_get();
    switch (ev->type) {
    case EV_PAINT: {                        /* client area is pre-filled with t->window_bg */
        surface_t *s = win_surface(win);
        char num[16];
        snprintf(num, sizeof num, "%d", c->count);
        gfx_text_aligned(s, rect_make(0, 20, s->w, 70), num, t->window_text,
                         FONT_UI_HUGE, ALIGN_CENTER | ALIGN_MIDDLE);
        double a = c->angle;
        gfx_circle_aa(s, s->w / 2, 140, 18, 3, color_alpha(t->accent, 60));
        gfx_fill_circle_aa(s, s->w / 2 + 18 * cos(a), 140 + 18 * sin(a), 4, t->accent);
        break;
    }
    case EV_TIMER:
        c->angle += 0.15;
        win_invalidate_rect(win, win_width(win) / 2 - 30, 110, 60, 60);
        break;
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESCAPE) win_close(win);
        break;
    case EV_CLOSE:                          /* free per-window state; timers/widgets are freed for you */
        free(c);
        break;
    }
}

static void counter_launch(const char *arg) {
    counter_t *c = calloc(1, sizeof *c);
    window_t *win = win_create("Counter", 300, 260, WIN_CENTER);
    win_set_userdata(win, c);
    win_set_event_handler(win, counter_event);
    c->label = ui_label(win, 0, 175, 300, 20, "Not clicked yet");
    ui_set_align(c->label, ALIGN_CENTER | ALIGN_MIDDLE);
    widget_t *b = ui_button(win, 100, 205, 100, 32, "Click me", on_click, c);
    ui_button_set_default(b, true);         /* Enter presses it */
    win_set_timer(win, 33);                 /* ~30 fps animation */
}

static void counter_icon(surface_t *s, int x, int y, int size) {
    gfx_fill_rounded_rect(s, x + size / 10, y + size / 10, size * 8 / 10, size * 8 / 10,
                          size / 5, RGB(60, 130, 240));
    gfx_text_aligned(s, rect_make(x, y, size, size), "42", COLOR_WHITE,
                     size >= 64 ? FONT_UI_TITLE : FONT_UI_BOLD, ALIGN_CENTER | ALIGN_MIDDLE);
}

REGISTER_APP(.name = "Counter", .category = CAT_ACCESSORIES,
             .description = "Counts your clicks",
             .launch = counter_launch, .draw_icon = counter_icon);
```

`apps/hello.c` (the **About** app) and `apps/widgets_demo.c` (**Widget
Gallery**, every widget and dialog) are complete, working references.

---

## 3. Registering an app

```c
typedef struct app_desc {
    const char *name;         /* unique; app_launch_by_name("Name"), autorun=Name    */
    const char *category;     /* CAT_ACCESSORIES, CAT_GAMES, CAT_GRAPHICS, CAT_MULTIMEDIA,
                                 CAT_OFFICE, CAT_INTERNET, CAT_DEVELOPMENT, CAT_SYSTEM,
                                 CAT_UTILITIES - or any string                          */
    const char *description;  /* one line, shown in the Start menu                      */
    void (*launch)(const char *arg);                          /* required               */
    void (*draw_icon)(surface_t *s, int x, int y, int size);  /* optional               */
    const char *file_types;   /* "txt,md,log": extensions this app opens (no dots)     */
    uint32_t flags;           /* APP_HIDDEN | APP_NO_DESKTOP | APP_SINGLE               */
} app_desc_t;

REGISTER_APP(.name = "Notepad", .category = CAT_ACCESSORIES, .description = "...",
             .launch = notepad_launch, .draw_icon = notepad_icon, .file_types = "txt,md");
```

* `REGISTER_APP(...)` takes designated initialisers. It puts a pointer in the
  `.apps` linker section; no shared file needs editing. One file may register
  several apps.
* `launch(arg)`: `arg` is `NULL` when started from the desktop/Start menu, or
  a string (usually an absolute file path: the file manager and desktop call
  `app_open_file(path)` -> `launch(path)`; `autorun=App:arg` passes `arg`).
  Create your window(s) and return.
* Flags: `APP_HIDDEN` (not on desktop or Start menu - still launchable by
  name), `APP_NO_DESKTOP` (Start menu only), `APP_SINGLE` (launching again
  focuses the existing window instead).
* `draw_icon(s, x, y, size)`: draw the icon into the `size`x`size` box at
  `(x, y)` on a **transparent** surface. It is rendered at 2x and downsampled
  (anti-aliased) and cached per size; sizes used: 18 (title bar), 22
  (taskbar), 32 (Start menu), 48 (desktop). Draw relative to `size`. If
  `NULL`, a coloured tile with the first letter is generated.

Registry functions:

| Function | |
|---|---|
| `int app_count(void)` / `const app_desc_t *app_get(int i)` | enumerate (sorted by name) |
| `const app_desc_t *app_find(const char *name)` | case-insensitive; also matches with spaces removed or `_`/`-` for spaces |
| `bool app_launch(app, arg)` / `bool app_launch_by_name(name, arg)` | start an app |
| `const app_desc_t *app_for_file(path)` / `bool app_open_file(path)` | file associations |
| `int app_window_count(app)` | open windows of an app |
| `const surface_t *app_icon(app, size)` / `app_draw_icon(dst, app, x, y, size)` | cached icon |
| `int app_categories(const char **out, int max)` | categories in Start-menu order |

---

## 4. Windows

```c
window_t *win_create(const char *title, int client_w, int client_h, uint32_t flags);
```

Sizes are of the **client area**; the frame adds a 32 px title bar
(`WIN_TITLE_H`) and 1 px border (`WIN_BORDER`). Windows are placed cascading
from the top-left unless `WIN_CENTER`. A window created while your app is
running (in `launch` or any of your callbacks) belongs to your app (title-bar
and taskbar icon).

| Flag | Meaning |
|---|---|
| `WIN_RESIZABLE` | border can be dragged; enables the maximize button and edge snapping |
| `WIN_NO_MAXIMIZE`, `WIN_NO_MINIMIZE` | hide those buttons |
| `WIN_CENTER` | centre on screen |
| `WIN_CONFIRM_CLOSE` | X / Alt+F4 send `EV_CLOSE_REQUEST`; call `win_close()` yourself (e.g. after "Save changes?") |
| `WIN_TOPMOST` | above normal windows |
| `WIN_NO_TASKBAR` | no taskbar button |
| `WIN_FRAMELESS` | no decorations; client = whole window |
| `WIN_POPUP` | frameless menu-like window above everything, closed by any click outside it |
| `WIN_DIALOG` | no min/max buttons, no taskbar button |
| `WIN_HIDDEN` | create invisible; `win_show()` later |
| `WIN_NO_SHADOW` | no drop shadow |

Managing windows:

```c
void  win_set_event_handler(window_t *w, win_event_fn fn);  /* void fn(window_t*, const event_t*) */
void  win_set_userdata(window_t *w, void *p);   void *win_get_userdata(window_t *w);
surface_t *win_surface(window_t *w);            /* client surface - re-fetch after EV_RESIZE */
int   win_width(w), win_height(w);              /* client size */
void  win_set_title(w, "..."); const char *win_get_title(w);
void  win_close(w);         /* sends EV_CLOSE now, frees the window after the handler returns */
void  win_show(w), win_hide(w), win_focus(w), win_minimize(w), win_maximize(w), win_restore(w);
void  win_resize(w, cw, ch); void win_set_min_size(w, cw, ch); void win_move(w, x, y);
rect_t win_frame_rect(w), win_client_rect(w);   /* screen coordinates */
bool  win_is_focused(w), win_is_minimized(w), win_is_maximized(w);
bool  win_is_valid(w);      /* safe on stale pointers: is this window still open? */
void  win_set_cursor(w, CURSOR_IBEAM);          /* shape while over the client area */
void  win_set_fullscreen(w, true); bool win_is_fullscreen(w);  /* games, slideshows: no frame,
                                                   whole screen, taskbar hidden while active */
void  win_post_event(w, intptr_t data);         /* deliver EV_USER later from the main loop */
window_t *win_create_dialog(window_t *owner, title, cw, ch, flags);
```

* **Multiple windows per app:** keep all state in a struct hung off
  `win_set_userdata()`; never use globals for per-window state.
* **Dialogs:** `win_create_dialog(owner, ...)` centres over the owner and makes
  it modal-lite: clicks on the owner beep and focus the dialog. Closing the
  owner closes its dialogs (their callbacks are *not* called).
* Enumeration (task managers, taskbars): `int win_list(window_t **out, int max)`
  (normal app windows, bottom to top), `win_get_focused()`, `win_get_app(w)`,
  `win_id(w)` / `win_find_by_id(id)` (ids are never reused - store ids, not
  pointers, across callbacks), `win_flags(w)`, `win_get_owner(w)`.
* Screen: `screen_width()`, `screen_height()`, `screen_work_height()` (above
  the 48 px taskbar), `surface_t *screen_capture(void)` (free with
  `surface_destroy`).

Built-in window behaviour: drag by title bar; double-click title toggles
maximize; drag to the top edge = maximize, to the left/right edge = snap to
half screen (resizable windows); Alt+Tab cycles, Alt+F4 closes, the taskbar
button minimizes/restores, right-click a taskbar button for a menu.

---

## 5. Events

```c
typedef struct event {
    int type;        /* EV_*                                                     */
    int x, y;        /* mouse position in CLIENT coordinates; PAINT: area origin */
    int w, h;        /* RESIZE: new client size; PAINT: area size                */
    int button;      /* MOUSE_DOWN/UP/DOUBLE_CLICK: MOUSE_LEFT/RIGHT/MIDDLE      */
    int buttons;     /* buttons currently held (bitmask)                         */
    int wheel;       /* MOUSE_WHEEL: +1 = down/towards user, -1 = up             */
    int key;         /* KEY_DOWN/KEY_UP keycode                                  */
    int mods;        /* MOD_SHIFT | MOD_CTRL | MOD_ALT | MOD_WIN | MOD_CAPS | MOD_NUM */
    int ch;          /* CHAR: character                                          */
    int repeat;      /* KEY_DOWN: 1 when generated by key auto-repeat            */
    int timer_id;    /* TIMER                                                    */
    intptr_t data;   /* USER                                                     */
} event_t;
```

| Event | When | Fields |
|---|---|---|
| `EV_PAINT` | the window needs drawing | `x,y,w,h` = invalid area |
| `EV_KEY_DOWN` / `EV_KEY_UP` | key pressed / released (focused window) | `key`, `mods`, `repeat` |
| `EV_CHAR` | printable character typed (after `EV_KEY_DOWN`); **not** sent while Ctrl or Alt is held, not sent for Enter/Backspace/Tab/Esc | `ch`, `mods` |
| `EV_MOUSE_MOVE` | pointer moved over the client area, or anywhere while a button pressed in the client is held (mouse capture; coordinates can be negative / outside) | `x,y,buttons` |
| `EV_MOUSE_DOWN` / `EV_MOUSE_UP` | button pressed / released | `x,y,button,buttons` |
| `EV_DOUBLE_CLICK` | second click within the double-click time; follows the second `EV_MOUSE_DOWN` | `x,y,button` |
| `EV_MOUSE_WHEEL` | wheel over the client area | `x,y,wheel` |
| `EV_MOUSE_LEAVE` | pointer left the client area | |
| `EV_TIMER` | a timer from `win_set_timer` fired | `timer_id` |
| `EV_RESIZE` | client size changed (user, maximize, `win_resize`); a full repaint follows | `w,h` |
| `EV_FOCUS` / `EV_BLUR` | window activated / deactivated | |
| `EV_CLOSE_REQUEST` | X or Alt+F4 with `WIN_CONFIRM_CLOSE` | |
| `EV_CLOSE` | window is being destroyed: free your state. No events follow. | |
| `EV_THEME` | theme changed (a repaint is scheduled too) | |
| `EV_USER` | from `win_post_event` | `data` |

**Keycodes** (`nova/input.h`): keys that produce characters use the
*unshifted* ASCII value (`'a'`..`'z'`, `'0'`..`'9'`, `'-'`, `'['`, `' '` ...);
`KEY_ENTER` (13), `KEY_BACKSPACE` (8), `KEY_TAB` (9), `KEY_ESCAPE` (27), and
`KEY_UP/DOWN/LEFT/RIGHT, KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN,
KEY_INSERT, KEY_DELETE, KEY_F1..KEY_F12, KEY_LSHIFT ... KEY_RALT, KEY_LWIN,
KEY_MENU, KEY_CAPSLOCK, KEY_PRINTSCREEN, KEY_PAUSE`. Keypad keys arrive as
their main-keyboard equivalents. So: `Ctrl+S` = `EV_KEY_DOWN` with
`key == 's' && (mods & MOD_CTRL)`; typed text = `EV_CHAR`.

Polling helpers: `input_mods()`, `input_key_down(keycode)`,
`input_mouse_pos(&x, &y)` (screen coords), `input_mouse_buttons()`.

**Widgets see events first.** If a widget consumes an event (a click on a
button, keys while a text box has focus, Tab to move focus, Enter for a
default button), your handler does not get it.

System shortcuts handled before apps: Alt+Tab, Alt+F4, Win key / Ctrl+Esc
(Start menu), Ctrl+Alt+Del (launches "Task Manager" or "System Monitor" if
installed), PrintScreen with or without Alt / Ctrl / Shift, and Ctrl+Alt+S
(both save a screenshot to `~/Pictures` - `bool wm_screenshot(void)` in the
kernel; Ctrl+Alt+S is there because hosts such as Windows 11 often keep the
PrintScreen key for themselves, so VirtualBox never passes it on).

---

## 6. Painting

* Each window has a client surface (`win_surface(win)`, ARGB, `s->w x s->h`).
* Before `EV_PAINT` the WM clips the surface to the invalid area and fills it
  with `theme_get()->window_bg`; then your handler draws; then widgets are
  drawn on top; then the area is composited to the screen.
* `win_invalidate(win)` - schedule a full repaint (cheap to call many times;
  coalesced into one `EV_PAINT` per frame).
* `win_invalidate_rect(win, x, y, w, h)` - repaint only that area (the
  `EV_PAINT` event and the clip cover the union of invalid rects). Redrawing
  everything inside `EV_PAINT` is always correct; the clip limits the cost.
* `win_update(win, x, y, w, h)` - you already drew into the surface (e.g. in a
  timer or mouse handler, like a paint program); just push that area to the
  screen. No `EV_PAINT`, no pre-fill. `w <= 0` = whole client.
* Don't keep the `surface_t *` across events that can resize the window -
  always call `win_surface()` again. After `EV_RESIZE` a full `EV_PAINT`
  follows automatically.
* Read colours from `theme_get()` at paint time so dark mode / accent changes
  just work.

---

## 7. Timers and long-running work

```c
int  win_set_timer(window_t *win, uint32_t interval_ms);  /* repeating; EV_TIMER, id > 0 */
void win_kill_timer(window_t *win, int id);              /* id <= 0: all of the window's timers */
int  timer_add(uint32_t ms, timer_fn fn, void *ctx, bool repeat);  /* fn(ctx) from the main loop */
void timer_cancel(int id);
uint32_t sys_uptime_ms(void);
```

Window timers die with their window. Resolution is 1 ms (PIT at 1000 Hz); a
timer that falls behind skips missed ticks instead of bursting. For a game
loop use `win_set_timer(win, 16)` and compute movement from
`sys_uptime_ms()` deltas.

Long work (search a big file, render a fractal, copy many files): keep a
cursor in your state, do a bounded slice of work per `EV_TIMER` (a few ms),
`win_invalidate()` to show progress, and kill the timer when done.

---

## 8. Graphics

`nova/gfx.h`. Pixels are `uint32_t` ARGB (`0xAARRGGBB`). `RGB(r,g,b)`,
`RGBA(r,g,b,a)`, `COLOR_R/G/B/A(c)`, `color_alpha(c, a)`, `color_mix(a, b, t)`
(t 0..255), `color_shade(c, amount)` (+lighter / -darker), `color_luma(c)`,
`color_hsv(h, s, v)`.

```c
typedef struct surface { int w, h, pitch; uint32_t *pixels; rect_t clip; uint32_t flags; } surface_t;
surface_t *surface_create(w, h);        /* opaque, black                       */
surface_t *surface_create_alpha(w, h);  /* transparent, SURF_ALPHA             */
void surface_destroy(s);  surface_t *surface_clone(s);  surface_t *surface_scaled(s, w, h);
surface_t surface_sub(parent, rect);    /* view into another surface (no copy) */
void surface_set_clip(s, rect); void surface_reset_clip(s);
uint32_t *surface_row(s, y);            /* direct pixel access: row[x]         */
```

All drawing is clipped to `s->clip`. Colours with alpha < 255 blend;
alpha 255 overwrites. On `SURF_ALPHA` surfaces blending is proper
"over" compositing (used for icons and sprites).

Primitives: `gfx_clear, gfx_pixel, gfx_get_pixel, gfx_fill_rect, gfx_rect
(outline), gfx_hline, gfx_vline, gfx_line (Bresenham), gfx_line_aa (Wu,
double coords), gfx_thick_line, gfx_circle, gfx_fill_circle,
gfx_fill_circle_aa, gfx_circle_aa (ring with thickness), gfx_ellipse,
gfx_fill_ellipse, gfx_fill_rounded_rect / gfx_rounded_rect (anti-aliased
corners), gfx_gradient_v, gfx_gradient_h, gfx_fill_rounded_gradient_v,
gfx_fill_triangle, gfx_fill_polygon (even-odd), gfx_shadow` (soft shadow
around a rect).

Blitting: `gfx_blit(dst, x, y, src)` / `gfx_blit_rect(...)` (opaque copy),
`gfx_blit_alpha(dst, x, y, src, opacity)` / `gfx_blit_alpha_rect(...)`
(per-pixel alpha x opacity), `gfx_blit_scaled(dst, dx, dy, dw, dh, src, sx,
sy, sw, sh, smooth)` (nearest or bilinear).

Images: `surface_t *image_decode(data, size)` reads BMP (8/24/32-bit,
uncompressed) and PPM (P3/P6); `void *image_encode_bmp(s, &size)` writes a
24-bit BMP (free the buffer). Combine with `vfs_read_file`/`vfs_write_file`.

UI drawing helpers (themed, for custom controls): `ui_draw_button(s, rect,
label, state)`, `ui_draw_panel`, `ui_draw_field`, `ui_draw_checkbox`,
`ui_draw_separator`, `ui_draw_scrollbar`, `ui_draw_glyph(s, UI_GLYPH_*, x, y,
size, color)` (check, close, minimize, maximize, restore, arrows, info,
warning, error, question, folder, file) and `ui_draw_logo(s, x, y, size)`.
States: `UI_STATE_HOVER | PRESSED | FOCUSED | DISABLED | DEFAULT | CHECKED`.

---

## 9. Text and fonts

Anti-aliased bitmap fonts generated from Inter (UI) and DejaVu Sans Mono
(monospace) by `tools/genfont.py`:

| Font | Size | Use |
|---|---|---|
| `FONT_UI` | 13 px, line 17 | default UI text (proportional) |
| `FONT_UI_BOLD` | 13 px semibold | emphasis, headings in lists |
| `FONT_UI_LARGE` | 18 px semibold, line 23 | section titles |
| `FONT_UI_TITLE` | 28 px bold, line 35 | big titles |
| `FONT_UI_HUGE` | 52 px light, line 64 | clocks, numbers |
| `FONT_MONO` / `FONT_MONO_BOLD` | 8x16 cells | terminal, editors |
| `FONT_MONO_2X` | 16x32 cells | the 2x monospace variant |

```c
int  gfx_text(s, x, y, "text", color, font);          /* y = top of the line; returns width */
int  gfx_text_n(s, x, y, str, n, color, font);
int  gfx_char(s, x, y, ch, color, font);
void gfx_text_aligned(s, rect, str, color, font, ALIGN_CENTER | ALIGN_MIDDLE);
int  gfx_text_ellipsis(s, x, y, max_w, str, color, font);   /* "Long te..." */
int  gfx_text_wrapped(s, rect, str, color, font);           /* word wrap + '\n'; returns height */
int  gfx_text_scaled(s, x, y, str, color, font, scale);     /* chunky integer scaling */
int  font_height(f), font_ascent(f), font_char_width(f, c), font_text_width(f, s),
     font_text_width_n(f, s, n), font_hit_test(f, s, px), font_wrap_height(f, s, w);
bool font_is_mono(f);
```

Character set (one byte per character, `0x20-0xFF`): ASCII, Latin-1 in
`0xA0-0xFF` (e.g. `"\xE9"` = é, `"\xB0"` = °, `"\xA9"` = ©, `"\xD7"` = ×), and
symbols in `0x80-0x9F`:

| Byte | Glyph | Byte | Glyph | Byte | Glyph | Byte | Glyph |
|---|---|---|---|---|---|---|---|
| 80 | ─ | 88 | ┬ | 90 | ▄ | 98 | ♪ |
| 81 | │ | 89 | ┴ | 91 | ← | 99 | ★ |
| 82 | ┌ | 8A | ┼ | 92 | ↑ | 9A | ♥ |
| 83 | ┐ | 8B | █ | 93 | → | 9B | ♦ |
| 84 | └ | 8C | ▓ | 94 | ↓ | 9C | ♣ |
| 85 | ┘ | 8D | ▒ | 95 | • | 9D | ♠ |
| 86 | ├ | 8E | ░ | 96 | ✓ | 9E | … |
| 87 | ┤ | 8F | ▀ | 97 | ✗ | 9F | ▶ |

In the monospace fonts the box-drawing and block characters fill the cell
exactly, so they join up (terminal UIs, frames).

---

## 10. Widgets

`nova/ui.h`. Widgets belong to a window, use client coordinates, are drawn
after your `EV_PAINT`, get events before your handler, and are freed with the
window. Callbacks have the signature `void cb(widget_t *w, void *ctx)`.

| Constructor | Value (`ui_get_value`) | Callback fires when |
|---|---|---|
| `ui_button(win, x, y, w, h, label, cb, ctx)` | - | clicked / Space / Enter (default button) |
| `ui_label(win, x, y, w, h, text)` | - | - (wraps when too long; `ui_set_align`, `ui_set_font`, `ui_set_color`) |
| `ui_textbox(win, x, y, w, h, text)` | - (`ui_get_text`) | every edit (`ui_set_callback`); Enter -> `ui_textbox_on_submit` |
| `ui_textarea(win, x, y, w, h, text)` | - (`ui_get_text` = whole text) | every edit; caret moves -> `ui_textarea_on_cursor` |
| `ui_checkbox(win, x, y, w, h, label, checked, cb, ctx)` | 0/1 | toggled |
| `ui_radio(win, x, y, w, h, label, group, checked, cb, ctx)` | 0/1 | selected (others in the group clear) |
| `ui_slider(win, x, y, w, h, min, max, value, cb, ctx)` | position | moved (drag, wheel, arrows). Vertical if `h > w` |
| `ui_progress(win, x, y, w, h, min, max, value)` | position | - |
| `ui_listbox(win, x, y, w, h, cb, ctx)` | selected index / -1 | selection changes; double-click/Enter -> `ui_list_on_activate` |
| `ui_scrollbar(win, x, y, w, h, vertical, cb, ctx)` | offset | scrolled; set with `ui_scrollbar_set(w, total, page, pos)` |
| `ui_dropdown(win, x, y, w, h, cb, ctx)` | selected index | selection changes |
| `ui_tabs(win, x, y, w, h, cb, ctx)` | selected tab | tab clicked / arrows (just the tab strip; show/hide your page widgets in the callback) |
| `ui_menubar(win)` + `ui_menubar_add(mb, "File", "New\tCtrl+N\|Open...\|-\|Exit", menu_cb, ctx)` | - | `menu_cb(item_index, ctx)` |
| `ui_image(win, x, y, w, h, surface)` | - | - (surface not copied: keep it alive) |
| `ui_custom(win, x, y, w, h, draw_fn, event_fn, ctx)` | - | your `draw_fn(w, s, rect)` / `bool event_fn(w, ev)` |

Common: `ui_set_text / ui_get_text, ui_set_value (does not fire the callback),
ui_set_range, ui_set_enabled, ui_set_visible, ui_move, ui_rect, ui_focus,
ui_has_focus, ui_destroy, ui_set_callback, ui_get_ctx, ui_window, ui_type,
ui_set_tag / ui_get_tag, ui_invalidate, ui_button_set_default`.

Text boxes: single line, caret, mouse/Shift selection, Ctrl+A/C/X/V,
Ctrl+Left/Right word jumps, Home/End; `ui_textbox_set_placeholder`,
`ui_textbox_set_password`, `ui_textbox_set_maxlen`, `ui_textbox_select_all`.

Text areas (multi-line editor, default font `FONT_MONO`): scrollbars, mouse
and Shift selection, double-click selects a word, Ctrl+A/C/X/V, Ctrl+Z/Y
undo/redo, Tab inserts a tab (Shift+Tab still moves focus), Enter keeps the
indentation, smart Home, Ctrl+Home/End, PgUp/PgDn, Ctrl+Up/Down scroll, wheel
(+Shift = horizontal). Other Ctrl shortcuts (Ctrl+S, Ctrl+F...) reach your
handler. Functions: `ui_textarea_set_readonly, ui_textarea_set_line_numbers,
ui_textarea_set_tab_width, ui_textarea_insert` (at the caret),
`ui_textarea_append` (at the end - log views), `ui_textarea_length,
ui_textarea_line_count, ui_textarea_get_cursor / set_cursor` (0-based line,
column), `ui_textarea_select / get_selection` (byte offsets),
`ui_textarea_find(w, needle, match_case)`, `ui_textarea_modified /
set_modified, ui_textarea_undo / redo, ui_textarea_on_cursor`. CRLF is
normalised to LF. The Widget Gallery's "Text area" tab shows it with line
numbers and a Ln/Col status bar.

Lists (listbox, dropdown, tabs share these): `ui_list_add` (returns index),
`ui_list_insert, ui_list_remove, ui_list_clear, ui_list_count, ui_list_get,
ui_list_set, ui_list_set_data / ui_list_get_data` (an `intptr_t` per item),
`ui_list_scroll_to`, `ui_list_set_icon_fn(w, fn)` (draw a 16x16 icon per row:
`fn(w, index, s, x, y)`). Listboxes support wheel, scrollbar drag,
keyboard navigation and type-to-jump.

Menu item strings: items separated by `|`, `-` is a separator, a leading `!`
disables an item, `\t` separates a right-aligned shortcut hint. The callback
gets the item index (separators count).

Context menus: `ui_popup_menu(win, x, y, items, cb, ctx)` (client coords) or
`ui_popup_menu_at(owner, screen_x, screen_y, ...)`.

Keyboard: Tab / Shift+Tab move focus between focusable widgets, Enter
triggers the default button (`ui_button_set_default`), Space toggles
checkboxes/buttons.

---

## 11. Dialogs, notifications, clipboard

All dialogs are **asynchronous**: they return immediately and call your
callback later. Never wait for them in a loop.

```c
window_t *ui_message_box(owner, "Title", "Text", MB_YESNO | MB_ICON_QUESTION, cb, ctx);
        /* types MB_OK, MB_OKCANCEL, MB_YESNO, MB_YESNOCANCEL; icons MB_ICON_INFO/WARNING/ERROR/QUESTION
           cb(int result, ctx): MB_RESULT_OK / CANCEL / YES / NO (Esc or X = CANCEL, or NO for MB_YESNO) */
window_t *ui_input_box(owner, "Rename", "New name:", "old.txt", cb, ctx);
        /* cb(const char *text, ctx); text == NULL if cancelled */
window_t *ui_file_dialog(owner, UI_FILE_OPEN | UI_FILE_SAVE | UI_FILE_DIR, title_or_NULL,
                         start_dir_or_file, "txt,md" or NULL, cb, ctx);
        /* cb(const char *path, ctx); path == NULL if cancelled. Save mode returns the typed
           path whether or not it exists - ask before overwriting if you care. */
void ui_notify("Title", "Text");      /* toast in the bottom-right corner, auto-hides
                                         (= notify(title, text, ICON_INFO), see section 19) */
void clipboard_set(const char *text); const char *clipboard_get(void);  /* system text clipboard */
```

`owner` may be `NULL`. Dialog strings are copied. If the owner window closes
first, the dialog closes silently without calling back (so it is safe to free
your state in `EV_CLOSE`).

**Every error plays the alert sound** (`sys_alert_sound()`, section 14), and
the toolkit does it for you: message boxes with `MB_ICON_ERROR`, or with
`MB_ICON_WARNING` and only an OK button (a report that something failed, not
a question), notifications with `ICON_ERROR` (`notify(title, body,
ICON_ERROR)` - the alert replaces the usual notification click), and the file
dialog's own errors. So report errors with one of those; call
`sys_alert_sound()` yourself only for an error you show some other way (an
error text inside your window, an error page).

---

## 12. Theme

```c
const theme_t *theme_get(void);
/* user settings: dark, accent, wallpaper (index, -1 = custom image), wallpaper_color, shadows
   derived colours: window_bg, window_text, window_border, panel_bg, title_active/inactive,
   title_text(_inactive), button_bg/hover/pressed/border/text, field_bg/border/text,
   selection_bg/text, disabled_text, accent_hover, accent_text, menu_bg/text/hover/separator,
   taskbar_bg/text/hover/active, desktop_text, scrollbar_bg/thumb, link */
void theme_set_dark(bool);  void theme_set_accent(uint32_t);  void theme_set_shadows(bool);
void theme_set_wallpaper(int index);  void theme_set_wallpaper_color(uint32_t);
void theme_set_wallpaper_image(const surface_t *img);   /* copied, scaled to cover */
int  theme_wallpaper_count(void); const char *theme_wallpaper_name(int);
void theme_render_wallpaper(surface_t *s, int index);   /* thumbnails */
int  theme_accent_count(void); uint32_t theme_accent_preset(int); const char *theme_accent_name(int);
void theme_refresh(void);
```

Built-in wallpapers: Nova, Aurora, Sunset, Ocean, Midnight, Solid colour.
Setters repaint every window and send `EV_THEME`.

---

## 13. Filesystem (VFS)

`nova/vfs.h` - a RAM filesystem. Absolute `/`-separated paths, `.`/`..`
resolved, names < 64 bytes, paths < 256. Errors are negative `VFS_E*` codes
(`vfs_strerror(err)`).

```c
void *vfs_read_file(path, &size);          /* malloc'd, NUL-terminated; free() it */
int   vfs_write_file(path, data, size);    /* create/overwrite (parent must exist) */
int   vfs_append_file(path, data, size);
int   vfs_stat(path, &st);                 /* st.size, st.is_dir, st.mtime, st.ctime (unix time) */
bool  vfs_exists(path), vfs_is_dir(path);
int   vfs_mkdir(path), vfs_mkdirs(path), vfs_create(path);
int   vfs_remove(path)            /* file or empty dir */, vfs_remove_recursive(path);
int   vfs_rename(from, to)        /* move; refuses to move a dir into itself */, vfs_copy(from, to);
int   vfs_list(dir, vfs_dirent_t *out, int max);   /* returns total count; dirs first, then by name;
                                                      vfs_list(dir, NULL, 0) just counts */
int   vfs_open(path, VFS_O_READ|VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC|VFS_O_APPEND);
int   vfs_read(fd, buf, n), vfs_write(fd, buf, n), vfs_seek(fd, off, VFS_SEEK_SET|CUR|END),
      vfs_tell(fd), vfs_close(fd);
int   vfs_normalize(cwd, path, out); const char *vfs_basename(p); void vfs_dirname(p, out);
void  vfs_join(dir, name, out); const char *vfs_extension(p);    /* "txt" */
uint32_t vfs_generation(void);   /* bumps on every change - poll it to refresh views */
uint32_t vfs_total_bytes(void);
```

Default tree at boot: `/home/user/{Desktop,Documents,Pictures,Music,Downloads}`,
`/home/user/README.txt`, sample texts in `Documents`, sample BMPs in
`Pictures`, `/home/user/Desktop/Welcome.txt`, `/system/version.txt`,
`/system/motd.txt`, `/tmp`. Files in `/home/user/Desktop` appear as desktop
icons (double-click opens them via `app_open_file`).

**Boot splash setting:** `/system/config/splash.cfg`, a text file with one
`key=value` per line. `noanimation=1`: the splash shows the Nova icon in the
middle in its normal colours straight away (no rise, no rainbow shimmer);
`noanimation=0`, no such line or no file: the icon rises from the bottom and
shimmers. The kernel reads it once per start, after the files are loaded (on
an installed drive after the saved files are restored), so a change takes
effect at the next start. Settings writes it (*No animation on splash
screen*).

**Persistence (for disk drivers / installer):**

```c
int32_t vfs_serialize(uint8_t *buf, uint32_t cap);   /* buf == NULL -> size needed */
int     vfs_deserialize(const uint8_t *buf, uint32_t size);  /* replaces the tree; VFS_ECORRUPT on bad data */
void    vfs_set_sync_hook(void (*fn)(void), uint32_t delay_ms);  /* called from the main loop
                                                                    delay_ms after the last change */
void    sys_add_shutdown_hook(void (*fn)(void));     /* run before reboot / power-off */
```

Image format: `"NVFS"`, u32 version (1), u32 node count, u32 checksum, then
pre-order records `{u32 parent_index, u8 is_dir, u8 name_len, name, u32 mtime,
u32 ctime, u32 size, data}`. A self-test at boot round-trips the tree.

---

## 14. System services

`nova/sys.h`

* Memory: `kmalloc / kcalloc / krealloc / kfree / kmalloc_usable_size`
  (16-byte aligned; `malloc` & co. map to these). Multi-megabyte blocks are
  fine. `sys_mem_total()` (RAM), `sys_mem_free()`, `sys_mem_used()`,
  `sys_heap_size()`.
* Logging: `klog("fmt", ...)` -> COM1 serial (115200 8N1) with an uptime
  prefix + an in-memory ring (`sys_log_text(buf, size)` for a log viewer).
  `printf` also goes to serial. `kpanic("fmt", ...)` = blue screen with
  registers and a symbolised stack trace.
* Info: `sys_version()` ("NovaOS 1.5"), `sys_edition()` ("Developer
  Preview"), `sys_kernel_version()` ("1.5.0 (build date)"),
  `sys_cpu_name()`, `sys_uptime_ms()`, `sys_fps()` (compositor frames last
  second), `sys_cmdline()`, `sys_cmdline_get("key")`, `sys_boot_drive()`.
* Time: `sys_datetime(&dt)` (local time from the CMOS RTC: year, month 1-12,
  day, hour, minute, second, weekday 0=Sunday), `sys_time()` (seconds since
  1970), `sys_time_to_datetime`, `sys_datetime_to_time`, `sys_month_name(m)`,
  `sys_weekday_name(wd)`. `sys_udelay(us)` busy-waits - drivers only.
* Sound (sound card, else PC speaker - see [22. Sound](#22-sound)):
  `sys_beep(freq, ms)` (non-blocking), `speaker_tone(freq)` / `speaker_off()`,
  `sys_play_notes(const note_t *notes, int count)` (non-blocking melody;
  `note_t {uint16_t freq, ms;}`, freq 0 = rest), `sys_alert_sound()` (the
  error sound: two short falling tones, non-blocking; error message boxes and
  error notifications play it by themselves - see section 11; calls less than
  300 ms apart play once), `sys_sound_enabled()` /
  `sys_set_sound_enabled()` (taskbar toggle). Polyphonic notes with
  instruments: `sound_note(freq, ms, volume, waveform)`; the output device:
  `sys_sound_device_name()`.
* Power: `sys_reboot()`, `sys_shutdown()` (ACPI S5 from the DSDT, then the
  QEMU/Bochs/VirtualBox ports, then a "safe to turn off" screen).
* Input settings: `sys_set_mouse_speed(1..10)` (5 = 1:1), `sys_get_mouse_speed`,
  `sys_mouse_has_wheel`, `sys_set_double_click_ms`, `sys_get_double_click_ms`.

---

## 15. Hardware access (drivers)

**Init hooks.** A driver or background service registers boot-time
initialisation from its own file - no kernel edits:

```c
static void ata_init(void) { /* probe the controller, klog what you find */ }
REGISTER_INIT(INIT_DRIVER, ata_init);
static void persist_load(void) { /* read the saved VFS image, vfs_deserialize(...) */ }
REGISTER_INIT(INIT_FS, persist_load);
```

| Stage | Runs |
|---|---|
| `INIT_DRIVER` | after PCI enumeration, timers, keyboard/mouse; before the filesystem exists |
| `INIT_FS` | right after the default VFS tree is created, before apps and desktop (load persisted files here; desktop icons will reflect them) |
| `INIT_LATE` | after the desktop is up, just before `autorun` apps start |

The splash screen is showing during the first two stages; keep them short
(sub-second). Drivers can live in `kernel/drivers/` (picked up
automatically) or anywhere under `apps/`.

`#include <nova/hw.h>` (not included by `nova.h`):
`inb/inw/inl/outb/outw/outl, insw/outsw, io_wait`, `irq_save/irq_restore,
cpu_cli/cpu_sti/cpu_hlt`, `irq_install(irq, handler)` (IRQ 0-15, unmasks it;
handler gets `regs_t*`, runs with interrupts off, must be short - push work
to the main loop, e.g. set a flag polled by a timer), `irq_uninstall,
irq_mask, irq_unmask`, PCI: `pci_read32/16/8, pci_write32/16,
pci_device_count, pci_get_device, pci_find_class(class, subclass),
pci_find_device(vendor, device), pci_enable(dev)` (`pci_device_t` has
bus/slot/func, ids, class codes, `irq_line`, `bar[6]`).

**Hard disks, persistence, installer.** `#include <nova/disk.h>` (not
included by `nova.h`; the header documents every call):
`disk_count() / disk_get(i)` (IDE hard disks found at boot: model, size,
location; ATAPI CD drives are skipped), `disk_read / disk_write(disk, lba,
count, buf)` (512-byte sectors, polled PIO - keep each call to a few dozen
sectors inside an app callback), `disk_flush`, `disk_rescan`,
`disk_format_size`, `disk_strerror`. On an installed system (`boot=disk` on
the command line) the VFS is saved to the disk automatically;
`persist_active()`, `persist_get_info(&info)` (dirty, saving, last save,
capacity), `persist_save()` (synchronous - the Terminal's `sync` command uses it)
and `persist_save_async()`. The installer back end (`install_begin /
install_step / install_progress ...`) is what `apps/installer.c` drives from
a window timer. Disk layout: `kernel/disk/diskfmt.h`.

In use by the kernel: IRQ0 (PIT), IRQ1 + IRQ12 (i8042 keyboard/mouse), COM1
polled (no IRQ), IDE polled with device interrupts off (no IRQ), AC'97
polled from the PIT tick (no IRQ), Sound Blaster 16 (only when it is the
sound output: its IRQ, usually 5, and 16-bit ISA DMA channel, usually 5),
the e1000 network card polled from a 1 ms main-loop timer (no IRQ).
Free: IRQ 3, 4, 5 (without SB16), 7, 9, 10, 11, 14, 15.
Paging is off: physical addresses (MMIO BARs, ACPI tables) are directly
accessible. The heap occupies the largest RAM region above the kernel; there
is no physical page allocator - `kmalloc` large aligned buffers if a device
needs DMA memory (addresses are physical).

---

## 16. libc / libm subset

* `string.h`: mem{cpy,move,set,cmp,chr}, `memset32`, str{len,nlen,cpy,ncpy,lcpy,
  lcat,cat,ncat,cmp,ncmp,casecmp,ncasecmp,chr,rchr,str,casestr,spn,cspn,pbrk,
  tok,tok_r,dup,ndup,error}.
* `stdio.h`: `snprintf, vsnprintf, sprintf, vsprintf` (`%d %i %u %x %X %o %b
  %p %s %c %f %e %g %%`, flags `- 0 + space #`, width/precision incl. `*`,
  `hh h l ll z`), `sscanf/vsscanf`, `printf/puts/putchar` (serial).
* `stdlib.h`: `malloc, calloc, realloc, free, atoi, atol, atoll, atof,
  strtol, strtoul, strtoll, strtoull, strtod, strtof, abs, labs, llabs, div,
  rand, srand, rand_range(lo, hi), qsort, bsearch, itoa, utoa, abort`.
* `ctype.h`: `isalpha isdigit isalnum isspace isupper islower isprint ispunct
  isxdigit iscntrl isgraph isblank toupper tolower`.
* `math.h`: `sqrt cbrt sin cos tan asin acos atan atan2 sinh cosh tanh exp exp2
  log log2 log10 pow floor ceil round trunc fabs fmod hypot fmin fmax copysign
  modf frexp ldexp lround`, `f` variants, `M_PI` etc., `INFINITY, NAN, isnan,
  isinf`.
* `assert.h`, `limits.h`, plus the compiler's `stdint.h stddef.h stdbool.h
  stdarg.h float.h`. There is no `FILE*` I/O, no `time.h` (use `sys_time`).

---

## 17. Kernel command line

Set in GRUB (`make CMDLINE="..."`, or press `e` in the boot menu) or with
QEMU `-append`. Tokens are `key=value` or bare flags.

| Option | Effect |
|---|---|
| `autorun=App1,App2` | launch apps after the desktop appears (`_` or `+` for spaces; `App:arg` passes an argument) |
| `testmouse=1` | 1:1 mouse motion without acceleration and pointer starts at (0,0) - used by `tools/shot.py` |
| `nosplash` | skip the boot splash |
| `res=WxH` | resolution via BGA when available (e.g. `res=1280x720`) |
| `dark=1` | start in dark mode |
| `wallpaper=N` | start with built-in wallpaper N |
| `mousespeed=N` | mouse speed 1..10 |
| `startmenu` | open the Start menu after boot |
| `selftest=1` | run kernel self tests (libc, heap, VFS, time, cryptography test vectors, certificate checks) and log `SELFTEST:` lines |
| `crashtest=1` | trigger a divide error 1.5 s after boot (tests the panic screen) |
| `fputest=1` | raise an x87 floating-point error 2 s after boot (tests that it is not fatal: `fpu: warning: ...` in the log) |
| `screensaver=SECONDS` | screensaver idle timeout for this boot (e.g. `screensaver=5` for tests; `0` = never) |
| `safetyshield_test=N` | SafetyShield test rule: flags `/tmp/safetyshield-test.txt` with risk level N (1 Risky .. 6 Severe) and runs the first automatic scan after 5 s - tests the threat log, list and notification |
| `sound=ac97\|sb16\|speaker` | sound output: only probe that card, or use the PC speaker (default: AC'97, else SB16, else speaker) |
| `volume=N` | master volume 0..100 at boot (default 80) |

Serial markers useful for tests: `NOVA: desktop ready`, `NOVA: autorun done`,
`SELFTEST: done, N passed, M failed`, `*** KERNEL PANIC ***`.

---

## 18. Rules, conventions and gotchas

1. **Never block.** No waiting loops, no `sys_udelay` in apps, no polling
   hardware in a loop. Use timers and callbacks.
2. One file per app in `apps/` (or a directory `apps/name/*.c`); `static`
   everything except what you deliberately share. No edits to shared files.
3. Per-window state via `win_set_userdata`; free it on `EV_CLOSE`. Timers and
   widgets are cleaned up automatically.
4. Callbacks can close windows. After calling something that might close
   your window (a dialog result, `win_close`), don't touch the window's state
   again in the same callback; use `win_is_valid(w)` if unsure.
5. Store window ids (`win_id`) rather than raw pointers in long-lived places.
6. Re-fetch `win_surface()` after resizes; get colours from `theme_get()`.
7. Big buffers go on the heap; the stack is shared and finite.
8. Compile warning-free (`make WERROR=1` checks).
9. Log with `klog()`; read it with `tools/shot.py` (`serial.log`).
10. To debug a crash: the panic screen and the serial log show the exception,
    registers and a symbolised stack trace (`function+0xoffset`). For an
    address only, `addr2line -e build/kernel.elf 0xADDR`.

---

## 19. Background services, system log, commands

`nova/service.h` (included by `nova.h`).

**Services** are daemons without a window: a set of callbacks the service
manager drives from the main loop. Like apps they register from their own
file (`.services` linker section) and must never block - periodic work goes
in `tick`, which the manager calls every `tick_ms` from a timer.

```c
static int  myd_start(void) { /* allocate, hook in */ return 0; }   /* nonzero = failed */
static void myd_stop(void)  { /* release everything */ }
static void myd_tick(void)  { /* a little work, then return */ }
REGISTER_SERVICE(.name = "myd", .display_name = "My Daemon",
                 .description = "Does things in the background",
                 .autostart = true, .restart_on_failure = true,
                 .start = myd_start, .stop = myd_stop, .tick = myd_tick, .tick_ms = 1000,
                 .icon = ICON_SERVICE);
```

* States: `SVC_STOPPED`, `SVC_STARTING`, `SVC_RUNNING`, `SVC_FAILED`.
  `service_fail("myd", "fmt", ...)` from inside a service marks it failed (its
  `stop` runs); with `restart_on_failure` it is restarted after 1, 2, 4 ... 30 s.
* Autostart services start at `INIT_LATE` (desktop up, before `autorun`),
  `notifyd` first. All running services are stopped in reverse order before
  reboot / power-off (a shutdown hook).
* Control: `service_start / service_stop / service_restart(name)` (0, -1
  unknown, -2 failed to start), `service_is_running`, `service_count`,
  `service_find`, `service_status_at(i, &st)` / `service_status(name, &st)`
  (`service_status_t`: names, description, state, autostart, uptime_ms,
  tick_ms, restarts, icon, last_error), `service_state_name(state)`.
  Names match case-insensitively and may also be the display name.
* `service_set_autostart(name, on)` - the user's choice, persisted in
  `/system/config/services` (`name=on|off` lines).
* `service_log_text(name, buf, size)` - that service's recent log lines;
  `service_generation()` bumps on every state change or log line (poll it to
  refresh a UI, as the **Services** app does).

**System log.** `log_info / log_warn / log_error(tag, "fmt", ...)` (and
`log_msg(level, ...)`) add a timestamped, tagged entry to a 512-entry ring
and mirror it to serial (`[tag] message`). A service logs with its own name
as the tag, which also files the line in its per-service log. Read the ring
with `syslog_first_seq() .. syslog_last_seq()` + `syslog_get(seq, &e)` and
`syslog_format(&e, buf, size)` (`2026-10-20 03:22:34 INFO  [logd] ...`). The
`logd` service appends it to `/system/log/syslog` every 5 s (kept below
96 KB).

**Built-in services**

| Service | Display name | API |
|---|---|---|
| `notifyd` | Notifications | `int notify(title, body, ICON_*)` slides a toast in above the taskbar (stacks, newest at the bottom, auto-dismiss after 5 s - paused while hovered - click to dismiss); `notify_dismiss(id)`, `notify_history_count/get/clear` (last 50). Returns 0 when notifyd is stopped (still recorded). Toasts raised while the screensaver runs wait until it is dismissed. |
| `logd` | System Log | writes the syslog to `/system/log/syslog` |
| `screensaver` | Screensaver | the chosen screensaver style full screen after N idle seconds (default 300). `screensaver_set_timeout(s)` (persisted in `/system/config/screensaver`, 0 = never), `screensaver_get_timeout`, `screensaver_activate`, `screensaver_dismiss`, `screensaver_active`. Any key, click or mouse movement dismisses it; that input is swallowed, so a click never reaches the window underneath. Command line `screensaver=SECONDS` overrides the timeout for one boot (it is not saved). Styles (`#include <nova/screensaver.h>`): Nova Drop, Original (starfield + aurora with a clock), Bubbles, Bouncing App Logos, Lines; `screensaver_get_style` / `screensaver_set_style` (persisted as `style=` in the same file, Settings > Styles), `screensaver_style_count/name`, and `ss_anim_create(style, w, h, preview)` / `ss_anim_frame` / `ss_anim_interval` / `ss_anim_destroy` to draw a style into any surface (live previews). |
| `safetyshield` | SafetyShield | scans every file in the OS automatically every minute; a scan that catches something (risk above Safe) shows the notification "SafetyShield detected a threat in your system". Detection rules are inactive for now (everything is Safe); add them to `rules[]` in `kernel/services/safetyshield.c`. `#include <nova/safetyshield.h>`: `safetyshield_scan`, `safetyshield_status`, `safetyshield_total_files`, `safetyshield_recent_count/get` (the last scan's files and risks), `safetyshield_risk_name/description/color`, `safetyshield_generation`. Settings > SafetyShield shows it all. |
| `alarmd` | Alarms | `alarm_add_at(h, m, msg)` (next HH:MM), `alarm_add_in(minutes, msg)`, `alarm_cancel(id)`, `alarm_count/get`. Due alarms show a notification (alarm-clock icon) and play a chime (`sys_play_notes`). Persisted in `/system/config/alarms`. |
| `autosave` | Autosave | `autosave_register(name, fn, ctx)` - `fn(ctx)` runs every 60 s and when the service stops (shutdown); `autosave_unregister(id)`, `autosave_now()`, `autosave_last_run()`. On an installed system it registers a `disk` callback that starts a background `persist_save_async()` when files changed (synchronous `persist_save()` at shutdown). |
| `indexer` | File Indexer | indexes every file and folder name (rescans 3 s after the VFS changes, a few directories per tick). `indexer_search(query, hits, max)` (case-insensitive; exact, prefix and word-start matches rank first, files under /home before /system), `indexer_count`, `indexer_ready`, `indexer_rescan`. Used by the Start menu, the Run dialog and `locate`. |
| `sysmon` | System Monitor | one `sysmon_sample_t` per second (heap used/total, busy %, events/s, frames/s) in a 120-sample ring: `sysmon_history(out, max)` (oldest first), `sysmon_latest(&s)` - for Task Manager style graphs. |

**Commands.** Any file can add a command to the Terminal and the Run dialog
(`.commands` linker section). Commands run in the main loop, so they must
return quickly; they write through a `cmd_writer_t`:

```c
static int cmd_hello(int argc, char **argv, cmd_writer_t *out) {
    cmd_printf(out, "Hello, %s!\n", argc > 1 ? argv[1] : "world");
    return 0;                                   /* exit status */
}
REGISTER_COMMAND("hello", "hello [NAME] - greet someone", cmd_hello);
```

* `cmd_writer_t { write(w, s, n), ctx, flags, cwd, cols }`: `CMD_ANSI` in
  `flags` means the output understands ANSI colours; `cwd` is the directory for
  relative paths (`cmd_resolve_path(out, arg, buf)` handles `~` and relative
  paths); `cols` is the output width for tables (0 = unknown).
* `command_count / command_get(i)` (sorted), `command_find(name)`,
  `command_exec(argc, argv, out)`, `command_run_line(line, out)` (splits with
  quotes and `\` escapes; 127 = no such command), `cmd_write / cmd_puts /
  cmd_printf`, and `cmd_buffer_t` + `cmd_buffer_init(&b)` to collect output in a
  heap string (`free(b.buf)` after).
* The Run dialog runs a command line and shows its output in a small window.
  The Terminal runs them like its own commands (shell built-ins win a name
  clash, registry commands come before apps; pipes and `>` work, `help`
  lists them under *Desktop*, Tab completes them). Another shell dispatches
  names it does not know itself to the registry the same way:

```c
static void reg_write(cmd_writer_t *w, const char *s, int n) { my_shell_write(w->ctx, s, n); }
...
if (command_find(argv[0])) {
    cmd_writer_t w = { reg_write, my_ctx, is_tty ? CMD_ANSI : 0, my_cwd, is_tty ? my_cols : 0 };
    return command_exec(argc, argv, &w);
}
```

Built-in commands:

| Command | |
|---|---|
| `service list\|status\|start\|stop\|restart\|enable\|disable [NAME...]` | manage services (`systemctl` is an alias); `enable`/`disable` set autostart |
| `notify TITLE [BODY...]` | show a notification (quote a title with spaces) |
| `alarm HH:MM\|+MINUTES [MESSAGE]`, `alarm list`, `alarm cancel ID` | alarms |
| `logs [SERVICE] [-n LINES]` | the system log, or one service's entries |
| `open PATH\|APP [-w APP]` | `shell_open`, or open with a given app |
| `assoc [EXT [APP\|--reset]]` | list file types / show or change the default app |
| `pin [APP]`, `unpin APP`, `recent [clear]` | taskbar pins, recent items |
| `run [TEXT]` | Run dialog, or do what it would do with TEXT |
| `locate NAME`, `sysmon`, `screensaver [now\|off\|SECONDS]`, `commands [NAME]` | file search, monitor sample, screensaver, list commands |

---

## 20. Opening files, file types, pins, recent items

`nova/shell.h` (included by `nova.h`).

**File types.** An app declares the extensions it opens with
`.file_types = "txt,md"` in its descriptor, or any file registers one with

```c
REGISTER_FILETYPE("note", "Sticky Notes", "Sticky note", ICON_NOTE);    /* preferred default    */
REGISTER_FILETYPE_ALT("bmp", "Paint", "Bitmap image", ICON_FILE_IMAGE); /* only if no other     */
REGISTER_FILETYPE("*", "Hex Viewer", NULL, 0);                          /* offered for any file */
```

`ext` has no dot; `"folder"` means directories, `"*"` any file (offered in
Open With, never the automatic default). Registrations for apps that are not
installed are ignored. Built in: `.txt .md .log .cfg .c .h .asm` -> Notepad,
`.bmp .ppm` -> Image Viewer (alternative Paint), `.note` -> Sticky Notes, any
file -> Hex Viewer, folders -> Files.

| Function | |
|---|---|
| `assoc_default_app(path)` | the app that opens it: the user's choice, then registrations, then descriptors; NULL if none |
| `assoc_apps(path, out, max)` | every installed app able to open it (default first, then others, then "any file" apps) |
| `assoc_set_default(ext, app)` | change the default (`NULL`/`""` resets); persisted in `/system/config/assoc` (`ext=App` lines) |
| `assoc_user_default(ext)`, `assoc_extensions(out, max)` | the user's choice, all known extensions |
| `filetype_description(path)` / `filetype_icon(ext)` | "Text document" / `ICON_*` |

`app_for_file()` / `app_open_file()` (section 3) follow the same rules.

**Opening.** `shell_open(path)` opens a file with its default app, a folder in
Files, an app by name (`"Notepad"` or `"app:Notepad"`), and shows the
**Open With** dialog when nothing is associated; it accepts `~/...` and paths
relative to /home/user, and records the item in the recent list.
`shell_open_with(path, app_name)`, `shell_open_with_dialog(path)` (every
capable app, "Show all apps", "Always use this app to open .ext files"),
`shell_properties(path)` (type, location, size, dates, "Opens with" +
Change...), `shell_run_dialog()` (Win+R) and `shell_run(text, err, size)`
(what the Run dialog does: path, app name, "App argument", registered
command, or a file name found by the indexer).

The desktop opens file icons with `shell_open` on double-click; right-click a
file for Open / Open With... / Rename... / Delete / Properties, the background
for New Folder / New Text File / Refresh / wallpaper / dark mode / Settings.
**Files** does the same: double-click (or Enter) calls `shell_open` (untyped
files that are plain text go to Notepad instead of the Open With dialog), the
item menu has *Open With...* (`shell_open_with_dialog`) and *Properties*
(`shell_properties`), and its icons come from `icon_for_path`, so a type you
register with `REGISTER_FILETYPE(..., ICON_*)` shows its icon and your app's
badge there too. The Terminal's `open FILE` and the Run dialog also go
through `shell_open`.

**Taskbar pins** (persisted in `/system/config/pinned`, one app name per line;
default Files, Terminal, Notepad - missing apps are skipped):
`shell_pin_count()`, `shell_pin_get(i)`, `shell_is_pinned(app)`,
`shell_set_pinned(app, on)`. Pinned buttons sit right of the Start button
(click: start the app, or focus / minimise its window; a dot marks running
apps). Right-click a pin, a taskbar button or a Start menu entry to pin or
unpin.

**Recent items** (persisted in `/system/config/recent`, newest first, 16):
every `app_launch` records the file it opened or the app (`"app:Name"`);
`shell_recent_add(item)`, `shell_recent_count()`, `shell_recent_get(i)`,
`shell_recent_clear()`. The Start menu shows the newest four under *Recent*;
typing searches apps (best match first: exact name, name prefix, a word of
the name, anywhere in the name, then description and category - so "Files"
finds Files before Notepad's "Edit plain text files"), then files and
folders (indexer), then offers to run the text as a command. `shell_generation()` bumps whenever pins, recent items
or associations change.

---

## 21. System icons

`nova/icons.h` (included by `nova.h`). Hand-designed vector icons (no image
files, no emoji) drawn on a 64-unit grid with a simplified pixel-aligned
variant for small sizes, rendered supersampled and cached per (kind, size):
crisp at 16, 32 and 48 px, usable from 8 to 256.

```c
icon_draw(ICON_FOLDER_PICTURES, s, x, y, 32);                       /* top-left at (x, y) */
icon_draw(icon_for_path("/home/user/notes.txt"), s, x, y, 16);
icon_draw_alpha(ICON_BELL, s, x, y, 48, 128);                       /* faded */
```

Kinds: folders (`ICON_FOLDER`, `_DOCUMENTS`, `_PICTURES`, `_MUSIC`,
`_DOWNLOADS`, `_DESKTOP`, `_SYSTEM`, `_HOME`), files (`ICON_FILE_TEXT`,
`_CODE`, `_IMAGE`, `_AUDIO`, `_ARCHIVE`, `_BINARY`, `_UNKNOWN`, `_LOG`,
`_CONFIG`, `ICON_NOTE`, `ICON_PAINT`), objects (`ICON_APP`, `ICON_DISK`,
`ICON_COMPUTER`, `ICON_TRASH`, `ICON_GEAR`, `ICON_SERVICE`, `ICON_SETTINGS`,
`ICON_BELL`, `ICON_ALARM`, `ICON_CLOCK`, `ICON_SEARCH`, `ICON_RUN`, `ICON_PIN`,
`ICON_TERMINAL`) and status (`ICON_INFO`, `ICON_WARNING`, `ICON_ERROR`,
`ICON_QUESTION`, `ICON_SUCCESS`).

* `icon_for_path(path)`: special folders under /home/user (Documents,
  Pictures, Music, Downloads, Desktop, the home folder itself), /system,
  other folders, then the extension (registered file types first);
  `icon_for_ext("txt")`, `icon_name(kind)`.
* `icon_get(kind, size)` returns the cached `SURF_ALPHA` image;
  `icon_render(kind, s, x, y, size)` draws without caching (e.g. inside an
  app's own `draw_icon` to reuse a system icon as a badge).
* The hidden **Icon Library** app (`autorun=Icon_Library`) shows every icon
  at 48/32/16 px; a click toggles a dark background.

---

## 22. Sound

`nova/audio.h` (included by `nova.h`). NovaOS mixes sound in software: up to
8 notes (voices) at once, each with a waveform, a volume and a short attack /
release envelope, so notes start and stop without clicks. The mix (16-bit, 48
kHz on AC'97, 44.1 kHz on SB16) is streamed to the first card found:

| Output | Hardware | Notes |
|---|---|---|
| `Intel ICH AC'97` | PCI 8086:2415 and later ICH AC'97 (VirtualBox *ICH AC97*, QEMU `-device AC97`) | 8 voices, 48 kHz stereo |
| `Sound Blaster 16` | DSP 4.xx at 0x220/0x240 (VirtualBox *SoundBlaster 16*, QEMU `-device sb16`) | 8 voices, 44.1 kHz mono, 16-bit ISA DMA |
| `PC speaker` | PIT channel 2 (no card; VirtualBox does not emulate it) | one note at a time: the newest note that is still held sounds; no volume or envelopes |

The boot log says which one is used (`audio: output Intel ICH AC'97, 48000
Hz, 8 voices, volume 80%`). Everything is non-blocking: the drivers refill
their DMA rings from the 1 kHz timer interrupt (about 40 ms ahead), and the
stream stops after 2 s of silence. The global on/off switch (taskbar speaker
icon, `sys_set_sound_enabled`) applies to everything below.

```c
/* a C major chord on the piano voice, 1.5 s */
sound_note_midi(60, 1500, 70, SOUND_PIANO);
sound_note_midi(64, 1500, 70, SOUND_PIANO);
sound_note_midi(67, 1500, 70, SOUND_PIANO);

/* a held note: start on key down, release on key up */
int v = sound_note(440, 0, 60, SOUND_TRIANGLE);   /* ms 0 = until released */
...
sound_release(v);                                 /* fades out (release time) */
```

* `int sound_note(freq_hz, ms, volume, waveform)` starts a note: frequency
  20..20000 Hz, `ms` its length (0 = until `sound_release`), `volume`
  0..100, waveform `SOUND_SQUARE` (soft-edged square, the classic beep),
  `SOUND_TRIANGLE` (flute-like), `SOUND_SINE`, `SOUND_SAW` (bright),
  `SOUND_PIANO` (struck: bright attack, overtones and level fade over about
  a second), `SOUND_ORGAN` (sine + octave + twelfth). Returns a voice id
  (> 0), or 0 if nothing started (sound off, frequency out of range). When
  all 8 voices are busy the quietest fading note, else the oldest, is
  replaced.
* `sound_note_midi(midi, ms, volume, waveform)`: the same with an exactly
  tuned MIDI note number (60 = middle C, 69 = A 440 Hz);
  `sound_midi_mhz(midi)` gives its frequency in millihertz.
* `sound_release(id)` lets a note go (ignores stale ids, so it is safe to call
  after the note ended or was replaced); `sound_active(id)`;
  `sound_stop_all()` releases everything including the `sys_beep` /
  `sys_play_notes` channel.
* `sys_sound_device_name()` ("Intel ICH AC'97", "Sound Blaster 16", "PC
  speaker"), `sound_device_short()` ("AC'97", "SB16", "PC speaker"),
  `sound_has_card()`, `sound_polyphony()` (8, or 1 on the PC speaker),
  `sound_sample_rate()` (0 on the PC speaker). Show the device somewhere if
  sound matters to your app - Piano shows "Output: AC'97 - 8 notes at once".
* `sound_set_volume(0..100)` / `sound_get_volume()`: master volume (default
  80, or `volume=N` on the command line; the PC speaker has no volume). A
  single note at volume 100 peaks around -10 dBFS; louder mixes (chords) go
  through a soft limiter above -6 dBFS instead of clipping.
* The simple calls (`sys_beep`, `speaker_tone` / `speaker_off`,
  `sys_play_notes`) are one channel on the same synthesizer: one note at a
  time with the soft square wave at volume 55; a new beep or melody
  replaces the previous one (which fades out instead of clicking). Use them
  for UI feedback; use `sound_note` for music.
* Terminal: `sound` (output, format, stream statistics, mixer CPU use),
  `sound test` (arpeggio + chord), `sound on|off`, `sound volume N`.

**Writing another sound driver.** `kernel/drivers/audio_internal.h`: fill in a
`sound_card_t` (`name`, `short_name`, `rate`, `start`, `stop`, `pump`) and
return it from a probe called by `audio_init()` (`kernel/drivers/audio.c`).
`pump()` runs every millisecond from the PIT interrupt while the stream
plays: it calls `audio_render(int16_t *mono, frames)` to mix more samples
into the card's DMA ring and `audio_note_underrun()` if the card ran dry.
That code runs with interrupts off and must not use floating point (the
interrupt entry does not save the FPU state) - the mixer is integer-only.

**Testing with real audio.** QEMU can record what the guest plays into a WAV
file: `tools/shot.py ... --extra -audiodev wav,id=snd0,path=out.wav -device
AC97,audiodev=snd0` (or `-device sb16,audiodev=snd0`; the PC speaker:
`-machine pcspk-audiodev=snd0`). `tools/wavcheck.py out.wav` lists the
notes it hears (start, length, frequency); see docs/TESTING.md.

---

## 23. Networking (HTTP, HTTPS, cookies, URLs)

`#include <nova/net.h>` (not included by `nova.h`; the header documents every
call). NovaOS has its own network stack, written for NovaOS like everything
else: an Intel PRO/1000 (e1000) driver - VirtualBox's default *Intel PRO/1000
MT Desktop* adapter and QEMU's `-nic user,model=e1000` - Ethernet, ARP, IPv4,
ICMP, UDP, DHCP, DNS, TCP, TLS 1.2 / 1.3 and an HTTP/1.1 client. The card is
found at boot and configured by DHCP; on VirtualBox NAT and QEMU user
networking that is 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3.

**Everything is asynchronous.** The stack runs in small slices from a 1 ms
main-loop timer; `http_fetch` returns at once and your callback is called
later from the main loop, exactly once (never from inside `http_fetch`).
Never wait in a loop for a request - that freezes the OS and the request
itself.

**Network status.** `net_is_up()` (card found, cable connected, DHCP done),
`net_status_text()` ("No network adapter", "Network cable unplugged",
"Connecting...", "Connected (10.0.2.15)"), `net_get_info(&info)` (adapter
name, MAC, link, IP / netmask / gateway / DNS in host byte order, lease,
packet and byte counters), `net_generation()` (bumps on every link / DHCP
change - poll it to refresh a UI), `net_ip_to_str(ip, buf16)`.

**HTTP and HTTPS.**

```c
static void done(http_request_t *r, void *ctx) {
    if (http_error(r) == HTTP_OK)                       /* a response arrived: any status */
        klog("%d %s: %u bytes", http_status(r), http_url(r), http_body_len(r));
    else
        klog("failed: %s", http_error_text(r));         /* e.g. "Could not find the site x.org" */
    http_free(r);                                       /* allowed inside the callback */
}
http_fetch("GET", "https://example.com/", NULL, 0, NULL, done, NULL);

/* POST a form, with options */
http_options_t o = { .body = "q=nova", .body_len = 6,
                     .content_type = "application/x-www-form-urlencoded",
                     .timeout_ms = 20000, .on_progress = progress };
http_fetch_ex("https://example.com/search", &o, done, ctx);
```

* URLs: `http://` and `https://` (ports 80 / 443 unless given; no scheme =
  `http://`), IPv4 only. Requests send `Host`, a NovaOS `User-Agent`,
  `Accept`, `Accept-Language`, `Accept-Encoding: gzip, deflate`,
  `Connection: close` and cookies; `opt.headers` adds or overrides headers
  (`"Name: value\r\n"` lines).
* Responses: redirects 301 / 302 / 303 / 307 / 308 are followed (10 at most;
  a 303, or a 301 / 302 after a POST, becomes a GET); chunked transfer
  encoding is decoded and gzip / deflate bodies are decompressed, so
  `http_body()` is always the plain content (NUL-terminated, `http_body_len()`
  bytes; `http_take_body()` hands the buffer over to you). 1xx interim
  responses are skipped.
* Results: `http_done`, `http_error` (`HTTP_OK`, `HTTP_PENDING` or an
  `HTTP_ERR_*` code), `http_error_text` (a sentence for the user),
  `http_strerror(code)`, `http_status`, `http_reason`, `http_url` (final URL),
  `http_redirect_count`, `http_header(r, name)` / `http_header_count` /
  `http_header_at`, `http_progress(r, &received, &total)` (bytes on the wire;
  total 0 = unknown).
* Errors map onto browser error pages: `HTTP_ERR_NO_NETWORK` (no internet),
  `HTTP_ERR_DNS` (site not found), `HTTP_ERR_CONNECT` (refused / reset /
  unreachable), `HTTP_ERR_TIMEOUT` (no progress for `timeout_ms`, default
  30 s), `HTTP_ERR_TLS` / `HTTP_ERR_CERT` (secure connection failed / bad
  certificate), `HTTP_ERR_PROTOCOL`, `HTTP_ERR_REDIRECTS`, `HTTP_ERR_BAD_URL`,
  `HTTP_ERR_TOO_LARGE` (over `max_body`, default 64 MB), `HTTP_ERR_NO_MEMORY`,
  `HTTP_ERR_CANCELLED`.
* `opt.flags`: `HTTP_F_ALLOW_BAD_CERT` (continue when the certificate check
  fails - for a "continue anyway" button; `http_cert_ok()` stays false),
  `HTTP_F_NO_REDIRECT`, `HTTP_F_NO_COOKIES`, `HTTP_F_NO_DECODE` (keep gzip /
  deflate bodies compressed). `opt.on_progress(r, ctx)` is called when the
  headers arrive and as body bytes arrive.
* `http_cancel(r)` stops a request (no callback); `http_free(r)` cancels and
  frees (safe in callbacks). Up to 8 requests run at once, more are queued
  in order.

**HTTPS (TLS).** TLS 1.3 and TLS 1.2 with AES-128-GCM, AES-256-GCM and
ChaCha20-Poly1305; key exchange X25519 (offered first), P-256 or P-384;
server signatures RSA PKCS #1 v1.5 / PSS and ECDSA P-256 / P-384 with
SHA-256/384/512; SNI, ALPN `http/1.1`, extended master secret and downgrade
protection. The server's certificate chain is checked against 117 built-in
root certificates (the Mozilla CA list, `kernel/net/roots.c`, generated by
`tools/genroots.py`), its validity dates against the clock, CA constraints,
and its names (subjectAltName, one-label `*.` wildcards) against the host.
SHA-1 signed certificates are refused. There is no session resumption,
renegotiation or client certificate (an empty one is sent when a server
asks), and no revocation checking.

* Padlock information: `http_is_https(r)`, `http_cert_ok(r)` (verified),
  `http_cert_error(r)` (why not, e.g. "The certificate has expired", "The
  certificate belongs to example.org, not www.example.com", "The certificate
  is self-signed, so it is not trusted", "The certificate was issued by an
  unknown authority (...)"), `http_cert_subject(r)` (the site name in the
  certificate), `http_cert_issuer(r)` (e.g. "Let's Encrypt"),
  `http_tls_info(r)` (e.g. "TLS 1.3, AES_128_GCM_SHA256, X25519").
* The handshake's expensive steps (key generation, ECDH, each signature
  check) run one per network tick, so the desktop keeps running while a
  connection is set up.
* This is a hobby TLS implementation that nobody has audited: it is fine for
  reading the web, but don't type real passwords or bank details into
  NovaOS. A network that intercepts TLS (a company proxy, some sandboxes)
  makes every certificate fail the check, as it should.

**Cookies** (RFC 6265) are kept per site (host-only and `Domain=` cookies,
`Path`, `Secure`, `Expires` / `Max-Age`, at most 400 days), sent back with
every `http_fetch` to a matching URL and stored from every `Set-Cookie`
(unless `HTTP_F_NO_COOKIES`). Cookies with an expiry date are saved to
`/home/user/.cookies` (a tab-separated text file, written 2 s after a change
and at shut down) and so survive restarts on an installed system; session
cookies live until shut down. `cookies_count()`, `cookies_clear()` (memory
and file - the browser's *Clear cookies* button).

**URL helpers.** `url_parse(s, &u)` splits an absolute URL into `url_t`
`{scheme, host (lower case), port, path ("/" at least, with "?query", without
"#fragment")}` - "example.com/x" is taken as http; false for other schemes.
`url_resolve(base, ref, out, size)` resolves a link against the page's URL
(RFC 3986: absolute, `//host/x`, `/x`, `x`, `../x`, `?q`, `#f`); it passes the
RFC 3986 section 5.4 examples.

**Decompression.** `inflate_buffer(in, len, INFLATE_RAW | INFLATE_ZLIB |
INFLATE_GZIP | INFLATE_AUTO, &out, &out_len, max_out)` decodes DEFLATE, zlib
(PNG image data) and gzip into a `malloc`'d, NUL-terminated buffer; 0, or -1
corrupt, -2 larger than `max_out` (0 = 64 MB), -3 out of memory. It runs
synchronously: keep inputs to a few MB per callback.

**Terminal commands** (results of the asynchronous ones go to the system log:
`logs net`, and to serial as `[net] ...`):

| Command | |
|---|---|
| `net` | adapter, MAC, link, address, gateway, DNS, lease, packet counters, TCP connections |
| `dhcp [renew]` | the DHCP lease, or ask for an address again |
| `ping HOST [COUNT]` | ICMP echo requests |
| `nslookup HOST` | DNS lookup |
| `fetch [-k] [-v] [-n] [-d FORMDATA] URL [FILE]` | HTTP / HTTPS request: status, size, time, redirects, body SHA-256, TLS version / suite / certificate; `-k` accepts a bad certificate, `-v` logs the headers, `-n` does not follow redirects, `-d` POSTs a form, `FILE` saves the body |
| `cookies [clear]` | count or delete cookies |

**Inside the kernel.** `kernel/drivers/e1000.c` (card) -> `kernel/net/`
`netcore.c` (Ethernet, ARP, IPv4, ICMP, UDP, the tick), `dhcp.c`, `dns.c`
(cache honouring TTLs), `tcp.c` (client connections: 64 KiB receive window,
retransmission with backoff), `tls.c` + `x509.c` + `roots.c`, `http.c`,
`cookies.c`, `url.c`, `inflate.c`; `net_internal.h` has the internal
interfaces. `kernel/crypto/` holds SHA-1/2, HMAC / HKDF / the TLS PRF,
AES-GCM, ChaCha20-Poly1305, X25519, P-256 / P-384, RSA and the random number
generator (RDSEED / RDRAND when present, plus TSC jitter and packet timing,
hashed with SHA-256). `selftest=1` checks all of it against published test
vectors (FIPS 180-4, RFC 4231, RFC 5869, FIPS 197, the GCM test cases, RFC
8439, RFC 7748), a test certificate chain and the built-in roots.
