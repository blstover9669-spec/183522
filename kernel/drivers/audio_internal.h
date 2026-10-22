/* audio_internal.h - interface between the software mixer (audio.c), the sound card
   drivers (ac97.c, sb16.c) and the PC speaker (speaker.c). Not part of the app API. */
#ifndef AUDIO_INTERNAL_H
#define AUDIO_INTERNAL_H

#include "kernel.h"
#include <nova/audio.h>

/* A sound card driver. All callbacks run with interrupts disabled (from the 1 kHz
   PIT interrupt), must be short and must not use the FPU (the interrupt entry does
   not save x87 state): the mixer is integer-only for the same reason. */
typedef struct sound_card {
    const char *name;           /* "Intel ICH AC'97"                                  */
    const char *short_name;     /* "AC'97"                                            */
    int rate;                   /* output sample rate in Hz                            */
    void (*start)(void);        /* (re)start streaming from a silent, reset state      */
    void (*stop)(void);         /* stop streaming                                      */
    void (*pump)(void);         /* every millisecond while streaming: keep the DMA ring
                                   filled by calling audio_render()                    */
} sound_card_t;

/* Render `frames` mono samples of the current mix (IRQ context). */
void audio_render(int16_t *out, int frames);
/* Called by drivers when the card ran out of data (DMA halted / caught up). */
void audio_note_underrun(void);

/* Card probes, tried in this order by audio_init(); NULL if the card is absent. */
const sound_card_t *ac97_probe(void);
const sound_card_t *sb16_probe(void);

/* PC speaker hardware (speaker.c): play a square wave of mhz millihertz, 0 = off. */
void pcspk_set(uint32_t mhz);
/* 1 kHz tick from the PIT interrupt (speaker.c forwards speaker_tick() here). */
void audio_tick(void);

/* Busy-wait helper for drivers (works with interrupts off). */
static inline void audio_delay_us(uint32_t us) { while (us--) io_wait(); }

#endif
