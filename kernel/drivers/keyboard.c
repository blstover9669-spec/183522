/*
 * keyboard.c - PS/2 keyboard (scancode set 1 via i8042 translation), US layout,
 * plus the shared i8042 IRQ routing for keyboard + mouse bytes.
 */
#include "kernel.h"

extern void mouse_byte(uint8_t b);

static int mods;              /* MOD_* */
static bool e0;               /* last byte was the 0xE0 prefix */
static int e1_skip;           /* bytes left of a Pause (E1) sequence */
static uint8_t key_down_map[0x180 / 8 + 64];

/* Set 1 make codes -> keycode (unshifted) */
static const uint16_t sc_normal[128] = {
    0, KEY_ESCAPE, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', KEY_BACKSPACE, KEY_TAB,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', KEY_ENTER, KEY_LCTRL, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', KEY_LSHIFT, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', KEY_RSHIFT, '*', KEY_LALT, ' ', KEY_CAPSLOCK, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5,
    KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_NUMLOCK, KEY_SCROLLLOCK, KEY_HOME, KEY_UP, KEY_PAGEUP, '-', KEY_LEFT, '5', KEY_RIGHT, '+', KEY_END,
    KEY_DOWN, KEY_PAGEDOWN, KEY_INSERT, KEY_DELETE, 0, 0, '\\', KEY_F11, KEY_F12, 0, 0, 0, 0, 0, 0, 0,
};

/* keypad scancodes 0x47..0x53 with Num Lock on -> digits */
static const char keypad_num[13] = { '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.' };

static const char shifted[128] = {
    ['1'] = '!', ['2'] = '@', ['3'] = '#', ['4'] = '$', ['5'] = '%', ['6'] = '^', ['7'] = '&',
    ['8'] = '*', ['9'] = '(', ['0'] = ')', ['-'] = '_', ['='] = '+', ['['] = '{', [']'] = '}',
    [';'] = ':', ['\''] = '"', ['`'] = '~', ['\\'] = '|', [','] = '<', ['.'] = '>', ['/'] = '?',
};

static void set_key(int key, bool down) {
    if (key <= 0 || key >= (int)sizeof(key_down_map) * 8) return;
    if (down) key_down_map[key / 8] |= (uint8_t)(1 << (key % 8));
    else key_down_map[key / 8] &= (uint8_t)~(1 << (key % 8));
}

int input_key_down(int key) {
    if (key <= 0 || key >= (int)sizeof(key_down_map) * 8) return 0;
    return (key_down_map[key / 8] >> (key % 8)) & 1;
}

int input_mods(void) { return mods; }

static void kb_byte(uint8_t sc) {
    if (e1_skip) { e1_skip--; return; }
    if (sc == 0xE1) { e1_skip = 5; input_event_t ev = { .type = IN_KEY, .pressed = 1, .key = KEY_PAUSE, .mods = (uint8_t)mods }; input_push(&ev); return; }
    if (sc == 0xE0) { e0 = true; return; }
    if (sc == 0xFA || sc == 0xFE || sc == 0x00 || sc == 0xFF) { e0 = false; return; }   /* ack / errors (0xAA is LShift release, keep it) */

    bool release = sc & 0x80;
    uint8_t code = sc & 0x7F;
    int key = 0;
    char ch = 0;

    if (e0) {
        e0 = false;
        switch (code) {
        case 0x1C: key = KEY_ENTER; break;
        case 0x1D: key = KEY_RCTRL; break;
        case 0x2A: case 0x36: return;   /* fake shifts around PrintScreen etc. */
        case 0x35: key = '/'; ch = '/'; break;
        case 0x37: key = KEY_PRINTSCREEN; break;
        case 0x38: key = KEY_RALT; break;
        case 0x47: key = KEY_HOME; break;
        case 0x48: key = KEY_UP; break;
        case 0x49: key = KEY_PAGEUP; break;
        case 0x4B: key = KEY_LEFT; break;
        case 0x4D: key = KEY_RIGHT; break;
        case 0x4F: key = KEY_END; break;
        case 0x50: key = KEY_DOWN; break;
        case 0x51: key = KEY_PAGEDOWN; break;
        case 0x52: key = KEY_INSERT; break;
        case 0x53: key = KEY_DELETE; break;
        case 0x5B: key = KEY_LWIN; break;
        case 0x5C: key = KEY_RWIN; break;
        case 0x5D: key = KEY_MENU; break;
        default: return;
        }
    } else {
        key = sc_normal[code];
        /* keypad block 0x47..0x53 (not E0-prefixed) */
        if (code >= 0x47 && code <= 0x53) {
            char kp = keypad_num[code - 0x47];
            if (kp == '-' || kp == '+') { key = kp; ch = kp; }
            else if (mods & MOD_NUM) { key = kp; ch = kp; }
            else if (kp == '5') key = 0;
            else if (kp == '.') key = KEY_DELETE;
        } else if (code == 0x37) {
            key = '*'; ch = '*';
        }
        if (!key) return;
    }

    /* modifiers */
    bool was_down = input_key_down(key);
    set_key(key, !release);
    switch (key) {
    case KEY_LSHIFT: case KEY_RSHIFT:
        if (input_key_down(KEY_LSHIFT) || input_key_down(KEY_RSHIFT)) mods |= MOD_SHIFT; else mods &= ~MOD_SHIFT;
        break;
    case KEY_LCTRL: case KEY_RCTRL:
        if (input_key_down(KEY_LCTRL) || input_key_down(KEY_RCTRL)) mods |= MOD_CTRL; else mods &= ~MOD_CTRL;
        break;
    case KEY_LALT: case KEY_RALT:
        if (input_key_down(KEY_LALT) || input_key_down(KEY_RALT)) mods |= MOD_ALT; else mods &= ~MOD_ALT;
        break;
    case KEY_LWIN: case KEY_RWIN:
        if (input_key_down(KEY_LWIN) || input_key_down(KEY_RWIN)) mods |= MOD_WIN; else mods &= ~MOD_WIN;
        break;
    case KEY_CAPSLOCK: if (!release && !was_down) mods ^= MOD_CAPS; break;
    case KEY_NUMLOCK: if (!release && !was_down) mods ^= MOD_NUM; break;
    default: break;
    }

    /* character translation */
    if (!release && !ch && key >= 32 && key < 127) {
        ch = (char)key;
        bool shift = mods & MOD_SHIFT;
        if (ch >= 'a' && ch <= 'z') {
            if (shift ^ !!(mods & MOD_CAPS)) ch = (char)(ch - 32);
        } else if (shift && shifted[(int)ch]) {
            ch = shifted[(int)ch];
        }
    }
    if (release) ch = 0;

    input_event_t ev = { .type = IN_KEY, .pressed = (uint8_t)!release, .repeat = (uint8_t)(!release && was_down),
                         .key = (uint16_t)key, .ch = (uint8_t)ch, .mods = (uint8_t)mods };
    input_push(&ev);
}

/* i8042 IRQ (1 and 12): drain the output buffer, routing aux bytes to the mouse. */
static void i8042_irq(regs_t *r) {
    (void)r;
    for (int i = 0; i < 32; i++) {
        uint8_t st = inb(0x64);
        if (!(st & 1)) break;
        uint8_t b = inb(0x60);
        if (st & 0x20) mouse_byte(b);
        else kb_byte(b);
    }
}

/* ---- controller helpers (used by mouse.c too) -------------------------------------- */
bool i8042_wait_write(void) {
    for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return true;
    return false;
}

bool i8042_wait_read(void) {
    for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return true;
    return false;
}

void i8042_cmd(uint8_t c) { i8042_wait_write(); outb(0x64, c); }
void i8042_data(uint8_t d) { i8042_wait_write(); outb(0x60, d); }
int i8042_read(void) { return i8042_wait_read() ? inb(0x60) : -1; }

void i8042_flush(void) {
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) { inb(0x60); io_wait(); }
}

static void kb_send(uint8_t b) {
    i8042_data(b);
    for (int i = 0; i < 3; i++) {
        int r = i8042_read();
        if (r == 0xFA || r < 0) break;
    }
}

/* Controller + keyboard init. Call with interrupts disabled, before mouse_init(). */
void keyboard_init(void) {
    mods = 0;
    i8042_cmd(0xAD);            /* disable keyboard port */
    i8042_cmd(0xA7);            /* disable aux port      */
    i8042_flush();
    i8042_cmd(0x20);            /* read config byte      */
    int cfg = i8042_read();
    if (cfg < 0) cfg = 0x47;
    cfg |= 0x01 | 0x02;         /* IRQ1 + IRQ12          */
    cfg |= 0x40;                /* set-1 translation     */
    cfg &= ~0x30;               /* enable both clocks    */
    i8042_cmd(0x60);
    i8042_data((uint8_t)cfg);
    i8042_cmd(0xAE);            /* enable keyboard port  */
    i8042_flush();
    /* typematic: 250 ms delay, ~30 chars/s */
    kb_send(0xF3);
    kb_send(0x00);
    kb_send(0xF4);              /* enable scanning       */
    i8042_flush();
    klog("keyboard: ready (cfg %02x)", cfg);
}

/* Install the shared IRQ handler once both devices are configured. */
void i8042_enable_irqs(void) {
    i8042_flush();
    irq_install(1, i8042_irq);
    irq_install(12, i8042_irq);
}
