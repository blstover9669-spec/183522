/* speaker.c - PC speaker via PIT channel 2: the fallback sound output when there is no
   sound card (see audio.c, which owns the public sound API and decides what plays). */
#include "audio_internal.h"

static uint32_t cur_div;     /* PIT divisor currently programmed (0 = speaker off) */

void pcspk_set(uint32_t mhz) {
    if (mhz < 20000 || mhz > 20000000) {
        if (cur_div) outb(0x61, inb(0x61) & 0xFC);
        cur_div = 0;
        return;
    }
    uint32_t div = (uint32_t)((1193182ull * 1000 + mhz / 2) / mhz);
    if (div > 0xFFFF) div = 0xFFFF;
    if (div == cur_div) return;
    outb(0x43, 0xB6);
    outb(0x42, div & 0xFF);
    outb(0x42, (div >> 8) & 0xFF);
    if (!cur_div) outb(0x61, inb(0x61) | 3);
    cur_div = div;
}

void speaker_init(void) {
    outb(0x61, inb(0x61) & 0xFC);
    cur_div = 0;
}

/* Called from the PIT IRQ every millisecond. */
void speaker_tick(void) { audio_tick(); }
