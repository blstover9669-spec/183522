/* speaker.c - PC speaker via PIT channel 2. Beeps/melodies are timed from the PIT IRQ. */
#include "kernel.h"

static volatile uint32_t off_at;          /* g_ticks when the current tone ends (0 = none) */
static const note_t *volatile melody;     /* current melody (copied) */
static note_t melody_buf[256];
static volatile int melody_len, melody_pos;
static bool sound_on = true;

static void tone_hw(uint32_t freq) {
    if (freq < 20 || freq > 20000) {
        outb(0x61, inb(0x61) & 0xFC);
        return;
    }
    uint32_t div = 1193182 / freq;
    outb(0x43, 0xB6);
    outb(0x42, div & 0xFF);
    outb(0x42, (div >> 8) & 0xFF);
    outb(0x61, inb(0x61) | 3);
}

void speaker_init(void) { outb(0x61, inb(0x61) & 0xFC); }

void speaker_tone(uint32_t freq) {
    if (!sound_on) return;
    uint32_t f = irq_save();
    melody = NULL;
    off_at = 0;
    tone_hw(freq);
    irq_restore(f);
}

void speaker_off(void) {
    uint32_t f = irq_save();
    melody = NULL;
    off_at = 0;
    tone_hw(0);
    irq_restore(f);
}

void sys_beep(uint32_t freq, uint32_t ms) {
    if (!sound_on) return;
    uint32_t f = irq_save();
    melody = NULL;
    tone_hw(freq);
    off_at = g_ticks + (ms ? ms : 1);
    irq_restore(f);
}

void sys_play_notes(const note_t *notes, int count) {
    if (!sound_on || count <= 0) return;
    if (count > 256) count = 256;
    uint32_t f = irq_save();
    memcpy(melody_buf, notes, sizeof(note_t) * (size_t)count);
    melody_len = count;
    melody_pos = 0;
    melody = melody_buf;
    tone_hw(melody_buf[0].freq);
    off_at = g_ticks + (melody_buf[0].ms ? melody_buf[0].ms : 1);
    irq_restore(f);
}

/* Called from the PIT IRQ every tick. */
void speaker_tick(void) {
    if (!off_at || (int32_t)(g_ticks - off_at) < 0) return;
    if (melody && melody_pos + 1 < melody_len) {
        melody_pos++;
        tone_hw(melody[melody_pos].freq);
        off_at = g_ticks + (melody[melody_pos].ms ? melody[melody_pos].ms : 1);
        return;
    }
    melody = NULL;
    off_at = 0;
    tone_hw(0);
}

bool sys_sound_enabled(void) { return sound_on; }
void sys_set_sound_enabled(bool on) {
    sound_on = on;
    if (!on) speaker_off();
}
