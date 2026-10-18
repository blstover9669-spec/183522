/*
 * mouse.c - PS/2 mouse on the i8042 aux port, with IntelliMouse wheel support.
 * Packets are assembled in the IRQ handler and pushed to the input queue as
 * relative motion; the window manager applies speed and moves the cursor.
 */
#include "kernel.h"

extern void i8042_cmd(uint8_t c);
extern void i8042_data(uint8_t d);
extern int  i8042_read(void);
extern void i8042_flush(void);

bool g_mouse_wheel;
static int packet_size = 3;
static uint8_t pkt[4];
static int pkt_idx;
static uint32_t last_byte_tick;

/* Send a byte to the mouse and wait for its ACK (0xFA). Returns the ack or -1. */
static int mouse_write(uint8_t b) {
    for (int attempt = 0; attempt < 3; attempt++) {
        i8042_cmd(0xD4);
        i8042_data(b);
        int r = i8042_read();
        if (r == 0xFA) return r;
        if (r != 0xFE) return r;   /* 0xFE = resend */
    }
    return -1;
}

static void set_rate(int rate) {
    mouse_write(0xF3);
    mouse_write((uint8_t)rate);
}

void mouse_init(void) {
    i8042_cmd(0xA8);              /* enable aux port */
    i8042_flush();
    /* make sure aux IRQ + clock are enabled in the config byte */
    i8042_cmd(0x20);
    int cfg = i8042_read();
    if (cfg >= 0) {
        cfg |= 0x02;
        cfg &= ~0x20;
        i8042_cmd(0x60);
        i8042_data((uint8_t)cfg);
    }
    int ack = mouse_write(0xF6);  /* set defaults */
    /* IntelliMouse magic sequence: sample rates 200, 100, 80 then read the ID */
    set_rate(200);
    set_rate(100);
    set_rate(80);
    mouse_write(0xF2);
    int id = i8042_read();
    g_mouse_wheel = (id == 3 || id == 4);
    packet_size = g_mouse_wheel ? 4 : 3;
    set_rate(100);
    mouse_write(0xE8);            /* resolution */
    mouse_write(0x02);            /* 4 counts/mm */
    mouse_write(0xE6);            /* scaling 1:1 */
    int en = mouse_write(0xF4);   /* enable data reporting */
    i8042_flush();
    pkt_idx = 0;
    klog("mouse: ack=%02x id=%d wheel=%s enable=%02x", ack & 0xFF, id, g_mouse_wheel ? "yes" : "no", en & 0xFF);
}

/* Called from the i8042 IRQ handler for every aux byte. */
void mouse_byte(uint8_t b) {
    /* if a packet stalls (lost byte), restart assembly */
    if (pkt_idx && g_ticks - last_byte_tick > 40) pkt_idx = 0;
    last_byte_tick = g_ticks;
    if (pkt_idx == 0 && !(b & 0x08)) return;    /* bit 3 is always set in byte 0: resync */
    pkt[pkt_idx++] = b;
    if (pkt_idx < packet_size) return;
    pkt_idx = 0;
    if (pkt[0] & 0xC0) return;                  /* overflow: drop */
    int dx = (int)pkt[1] - ((pkt[0] << 4) & 0x100);
    int dy = (int)pkt[2] - ((pkt[0] << 3) & 0x100);
    int dz = 0;
    if (packet_size == 4) {
        dz = (int8_t)(pkt[3] << 4) >> 4;        /* low nibble, signed */
    }
    input_event_t ev = { .type = IN_MOUSE, .buttons = (uint8_t)(pkt[0] & 7),
                         .dx = (int16_t)dx, .dy = (int16_t)-dy, .dz = (int16_t)dz };
    input_push(&ev);
}

/* ---- settings ------------------------------------------------------------------- */
int g_mouse_speed = 5;
bool g_test_mouse;

void sys_set_mouse_speed(int s) { g_mouse_speed = CLAMP(s, 1, 10); }
int sys_get_mouse_speed(void) { return g_mouse_speed; }
bool sys_mouse_has_wheel(void) { return g_mouse_wheel; }
