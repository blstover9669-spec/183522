/*
 * nova/input.h - keycodes, modifier and mouse button constants.
 *
 * Keycodes for keys that produce a character are the UNSHIFTED ASCII value
 * ('a'..'z', '0'..'9', '-', '=', '[', ...). The shifted / caps-locked
 * character arrives separately in an EV_CHAR event. Non-character keys use
 * the KEY_* values >= 0x100 below (plus the few ASCII control codes).
 */
#ifndef NOVA_INPUT_H
#define NOVA_INPUT_H

enum {
    KEY_NONE      = 0,
    KEY_BACKSPACE = 8,
    KEY_TAB       = 9,
    KEY_ENTER     = 13,
    KEY_ESCAPE    = 27,
    KEY_SPACE     = 32,

    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN,
    KEY_INSERT, KEY_DELETE,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL, KEY_LALT, KEY_RALT,
    KEY_LWIN, KEY_RWIN, KEY_MENU,
    KEY_CAPSLOCK, KEY_NUMLOCK, KEY_SCROLLLOCK,
    KEY_PRINTSCREEN, KEY_PAUSE,
    /* Keypad keys are reported as their main-keyboard equivalents
       (digits / arrows depending on Num Lock, KEY_ENTER, '+', '-', '*', '/'). */
};

/* Modifier bits (event_t.mods). */
#define MOD_SHIFT 0x01
#define MOD_CTRL  0x02
#define MOD_ALT   0x04
#define MOD_WIN   0x08
#define MOD_CAPS  0x10   /* caps lock is on */
#define MOD_NUM   0x20   /* num lock is on  */

/* Mouse buttons (event_t.button / event_t.buttons bitmask). */
#define MOUSE_LEFT   0x1
#define MOUSE_RIGHT  0x2
#define MOUSE_MIDDLE 0x4

/* Cursor shapes for win_set_cursor(). */
enum {
    CURSOR_ARROW = 0,
    CURSOR_IBEAM,
    CURSOR_HAND,
    CURSOR_CROSSHAIR,
    CURSOR_MOVE,
    CURSOR_WAIT,
    CURSOR_RESIZE_H,     /* <-> */
    CURSOR_RESIZE_V,     /* up/down */
    CURSOR_RESIZE_NWSE,  /* \ diagonal */
    CURSOR_RESIZE_NESW,  /* / diagonal */
    CURSOR_NONE,         /* hidden */
    CURSOR_COUNT
};

/* Current keyboard modifier state (MOD_* mask) and mouse state, screen coords. */
int  input_mods(void);
void input_mouse_pos(int *x, int *y);
int  input_mouse_buttons(void);
/* Is the given keycode currently held down? */
int  input_key_down(int keycode);

#endif
