/*
 * nova/audio.h - sound output.
 *
 * NovaOS has a small software synthesizer: up to SOUND_MAX_VOICES notes sound at
 * once, each with its own waveform, volume and a short attack / release envelope
 * (so notes start and stop without clicks). The mix is streamed to the sound card:
 *
 *   - Intel ICH AC'97 (VirtualBox "ICH AC97", QEMU -device AC97), 48 kHz
 *   - Sound Blaster 16 (VirtualBox "SoundBlaster 16", QEMU -device sb16)
 *   - otherwise the PC speaker (one note at a time, no volume: the newest note
 *     that is still held sounds; VirtualBox does not emulate the PC speaker)
 *
 * The simple API in <nova/sys.h> (sys_beep, speaker_tone / speaker_off,
 * sys_play_notes, sys_sound_enabled) uses the same synthesizer: it is one
 * "legacy" channel that plays one note at a time with a soft square wave.
 * Everything here is non-blocking and respects the global sound on/off switch
 * (taskbar speaker icon, sys_set_sound_enabled()).
 */
#ifndef NOVA_AUDIO_H
#define NOVA_AUDIO_H

#include <nova/types.h>

#define SOUND_MAX_VOICES 8

/* waveforms */
#define SOUND_SQUARE   0   /* soft-edged square: the classic beep                 */
#define SOUND_TRIANGLE 1   /* mellow, flute-like                                  */
#define SOUND_SINE     2   /* pure tone                                           */
#define SOUND_SAW      3   /* bright, buzzy                                       */
#define SOUND_PIANO    4   /* struck string: bright attack that fades out         */
#define SOUND_ORGAN    5   /* sine with octave and twelfth drawbars, no fade      */
#define SOUND_NWAVES   6

/* Start a note. freq_hz 20..20000, ms = length (0 = until sound_release()),
   volume 0..100, waveform SOUND_*. Returns a voice id (> 0) for sound_release(),
   or 0 if nothing was started (sound off, bad frequency). When all voices are busy
   the oldest note is replaced. */
int  sound_note(uint32_t freq_hz, uint32_t ms, int volume, int waveform);
/* The same with a MIDI note number (60 = middle C, 69 = A 440 Hz), exactly tuned. */
int  sound_note_midi(int midi, uint32_t ms, int volume, int waveform);
/* Let a note go (it fades out over its release time). Ignores stale ids. */
void sound_release(int voice);
/* Is this note still sounding (including its release)? */
bool sound_active(int voice);
/* Release every note, including the sys_beep / sys_play_notes channel. */
void sound_stop_all(void);

/* Frequency of a MIDI note in millihertz (69 -> 440000). */
uint32_t sound_midi_mhz(int midi);

/* Output device: "Intel ICH AC'97", "Sound Blaster 16" or "PC speaker". */
const char *sys_sound_device_name(void);
/* Short form for status lines: "AC'97", "SB16", "PC speaker". */
const char *sound_device_short(void);
bool sound_has_card(void);          /* false = PC speaker fallback               */
int  sound_polyphony(void);         /* notes that can sound at once (1 = speaker) */
int  sound_sample_rate(void);       /* card output rate in Hz (0 = PC speaker)   */

/* Master volume 0..100 (default 80; the PC speaker has no volume). */
void sound_set_volume(int percent);
int  sound_get_volume(void);

#endif
