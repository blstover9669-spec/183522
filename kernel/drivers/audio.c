/*
 * audio.c - NovaOS sound: a small software synthesizer and mixer, the public sound API
 * (<nova/audio.h> and the sound part of <nova/sys.h>) and the choice of output.
 *
 *   - Up to SOUND_MAX_VOICES voices, each a waveform (soft square, triangle, sine, saw,
 *     piano, organ) with an attack / hold (or decay) / release envelope, mixed into
 *     16-bit mono samples. Everything here is integer arithmetic: the mixer runs in the
 *     PIT interrupt, whose entry code does not save the FPU state.
 *   - With a sound card (AC'97, else Sound Blaster 16) the card driver's pump() is
 *     called every millisecond from the PIT interrupt and pulls freshly mixed samples
 *     into its DMA ring with audio_render(). The stream starts when a note starts and
 *     stops after IDLE_STOP_MS of silence. Nothing ever blocks the GUI.
 *   - Without a card the voices are timed in milliseconds and the newest note that is
 *     still held plays on the PC speaker (no volume, no envelopes).
 *   - The old PC-speaker API (sys_beep, speaker_tone, speaker_off, sys_play_notes) is
 *     one "legacy" channel: one note at a time, soft square wave, each new note gets a
 *     fresh voice so the previous one fades out instead of clicking.
 */
#include "audio_internal.h"

#define NV            SOUND_MAX_VOICES
#define ONE           (1 << 30)          /* envelope full scale (Q30)                    */
#define ENV_FLOOR     (ONE >> 10)        /* a fading note below -60 dB has ended          */
#define CHUNK         256                /* frames mixed per inner pass                   */
#define LEGACY_VOLUME 50
#define IDLE_STOP_MS  2000
#define MAX_NOTE_MS   60000

enum { V_OFF, V_ATTACK, V_HOLD, V_RELEASE };

typedef struct voice {
    uint8_t  state, wave;
    uint32_t gen;            /* changes on every start, part of the voice id          */
    uint32_t serial;         /* start order: voice stealing, PC speaker priority      */
    uint32_t mhz;            /* frequency in millihertz                                */
    uint32_t phase, inc;     /* oscillator phase and per-sample increment (2^32 = 1)   */
    int32_t  left;           /* time units until the release starts, -1 = held         */
    int32_t  env;            /* envelope level, Q30                                    */
    int32_t  astep;          /* attack: env increment per sample                       */
    int32_t  rstep, rlen;    /* release: decrement per sample, length in samples       */
    int32_t  dmul;           /* decay multiplier per sample (Q30), 0 = no decay        */
    int32_t  peak;           /* amplitude at full envelope, 0..9000                    */
} voice_t;

/* attack (microseconds) and release (milliseconds) per waveform */
static const struct { uint16_t attack_us, release_ms; } env_params[SOUND_NWAVES] = {
    [SOUND_SQUARE]   = { 3000, 15 },
    [SOUND_TRIANGLE] = { 4000, 30 },
    [SOUND_SINE]     = { 5000, 40 },
    [SOUND_SAW]      = { 3000, 20 },
    [SOUND_PIANO]    = { 1500, 140 },
    [SOUND_ORGAN]    = { 8000, 50 },
};

/* MIDI notes 108..119 (octave 9) in millihertz; lower octaves are shifted down */
static const uint32_t midi_top[12] = { 4186009, 4434922, 4698636, 4978032, 5274041, 5587652,
                                       5919911, 6271927, 6644875, 7040000, 7458620, 7902133 };

static voice_t voices[NV];
static uint32_t serial_ctr, gen_ctr;
static const sound_card_t *card;          /* NULL = PC speaker                          */
static int rate;                          /* card sample rate, 0 = PC speaker (ms units) */
static bool streaming;
static uint32_t idle_ms;
static bool sound_on = true;
static int master_pct = 80, master_mul = 205;
static int legacy_id;                     /* voice of the sys_beep / speaker_tone channel */
static uint32_t spk_mhz;                  /* what the PC speaker plays now              */
static volatile uint32_t underruns, streams;
static int16_t sine_tab[1025];

/* melody (sys_play_notes), timed in the same units as the voices */
static note_t mel[256];
static int mel_n, mel_pos;
static int32_t mel_left;
static bool mel_on;

/* ---- time units: samples with a card, milliseconds on the PC speaker ---------------- */
static int32_t ms_units(uint32_t ms) {
    if (ms > MAX_NOTE_MS) ms = MAX_NOTE_MS;
    if (!rate) return (int32_t)ms;
    return (int32_t)(ms * (uint32_t)rate / 1000u);
}

static int32_t us_samples(uint32_t us) {
    int32_t n = rate ? (int32_t)(us * (uint32_t)rate / 1000000u) : 1;
    return n > 0 ? n : 1;
}

/* ---- voices -------------------------------------------------------------------------- */
static voice_t *voice_by_id(int id) {
    if (id <= 0) return NULL;
    voice_t *v = &voices[id & (NV - 1)];
    if (v->gen != ((uint32_t)id >> 3) || v->state == V_OFF) return NULL;
    return v;
}

static void voice_release(voice_t *v) {
    if (v->state == V_OFF || v->state == V_RELEASE) return;
    v->left = -1;
    if (!rate) { v->state = V_OFF; return; }
    v->rstep = v->env / v->rlen;
    if (v->rstep < 1) v->rstep = 1;
    v->state = V_RELEASE;
}

static voice_t *voice_alloc(void) {
    voice_t *best = NULL;
    for (int i = 0; i < NV; i++)
        if (voices[i].state == V_OFF) return &voices[i];
    for (int i = 0; i < NV; i++)          /* the quietest fading note */
        if (voices[i].state == V_RELEASE && (!best || voices[i].env < best->env)) best = &voices[i];
    if (best) return best;
    for (int i = 0; i < NV; i++)          /* else the oldest note */
        if (!best || (int32_t)(voices[i].serial - best->serial) < 0) best = &voices[i];
    return best;
}

/* interrupts off */
static int voice_start(uint32_t mhz, uint32_t ms, int volume, int wave) {
    if (mhz < 20000 || mhz > 20000000) return 0;
    if (wave < 0 || wave >= SOUND_NWAVES) wave = SOUND_SQUARE;
    voice_t *v = voice_alloc();
    int slot = (int)(v - voices);
    if (++gen_ctr >= (1u << 24)) gen_ctr = 1;
    v->gen = gen_ctr;
    v->serial = ++serial_ctr;
    v->wave = (uint8_t)wave;
    v->mhz = mhz;
    v->phase = 0;
    v->inc = rate ? (uint32_t)(((uint64_t)mhz << 32) / ((uint64_t)rate * 1000u)) : 0;
    v->left = ms ? ms_units(ms) : -1;
    if (v->left == 0) v->left = 1;
    v->peak = CLAMP(volume, 0, 100) * 90;
    v->env = 0;
    v->astep = ONE / us_samples(env_params[wave].attack_us);
    v->rlen = us_samples(env_params[wave].release_ms * 1000u);
    v->dmul = 0;
    if (wave == SOUND_PIANO && rate) {
        /* lower strings ring longer: time constant 0.5 s (high) .. 1.4 s (low) */
        uint32_t tau_ms = 400 + 120000 / (mhz / 1000 + 1);
        int32_t tau = ms_units(tau_ms);
        v->dmul = ONE - ONE / (tau > 1 ? tau : 2);
    }
    v->state = V_ATTACK;
    return (int)((v->gen << 3) | (uint32_t)slot);
}

static void legacy_play(uint32_t mhz, uint32_t ms) {
    voice_t *old = voice_by_id(legacy_id);
    if (old) voice_release(old);
    legacy_id = mhz ? voice_start(mhz, ms, LEGACY_VOLUME, SOUND_SQUARE) : 0;
}

static void melody_advance(void) {
    if (++mel_pos >= mel_n) { mel_on = false; return; }
    const note_t *nt = &mel[mel_pos];
    uint32_t ms = nt->ms ? nt->ms : 1;
    mel_left = ms_units(ms);
    if (mel_left <= 0) mel_left = 1;
    legacy_play((nt->freq >= 20 && nt->freq <= 20000) ? nt->freq * 1000u : 0, ms);
}

/* ---- synthesis (IRQ context, integer only) -------------------------------------------- */
static inline int32_t wsin(uint32_t ph) {
    uint32_t i = ph >> 22;
    int32_t f = (int32_t)((ph >> 6) & 0xFFFF);
    int32_t a = sine_tab[i], b = sine_tab[i + 1];
    return a + (((b - a) * f) >> 16);
}

static inline int32_t wtri(uint32_t ph) {
    int32_t p = (int32_t)(ph >> 15);              /* 0..131071 */
    return p < 65536 ? p - 32768 : 98303 - p;
}

static inline int32_t wave_sample(const voice_t *v) {
    uint32_t ph = v->phase;
    switch (v->wave) {
    case SOUND_SQUARE: {                          /* square with sloped edges (1/8 period) */
        int32_t t = wtri(ph) * 4;
        return CLAMP(t, -32767, 32767);
    }
    case SOUND_TRIANGLE: return wtri(ph);
    case SOUND_SINE:     return wsin(ph);
    case SOUND_SAW:      return (int32_t)(ph >> 16) - 32768;
    case SOUND_PIANO: {                           /* overtones fade with the envelope */
        int32_t bright = v->env >> 15;
        int32_t h = (wsin(ph * 2) * 9000 + wsin(ph * 3) * 4000) >> 15;
        return (wsin(ph) * 21000 + h * bright) >> 15;
    }
    case SOUND_ORGAN:
        return (wsin(ph) * 17000 + wsin(ph * 2) * 9000 + wsin(ph * 3) * 5000) >> 15;
    default:
        return 0;
    }
}

static void voice_mix(voice_t *v, int32_t *acc, int n) {
    for (int i = 0; i < n; i++) {
        switch (v->state) {
        case V_ATTACK:
            v->env += v->astep;
            if (v->env >= ONE) { v->env = ONE; v->state = V_HOLD; }
            break;
        case V_HOLD:
            if (v->dmul) {
                v->env = (int32_t)(((int64_t)v->env * v->dmul) >> 30);
                if (v->env < ENV_FLOOR) { v->state = V_OFF; return; }
            }
            break;
        case V_RELEASE:
            v->env -= v->rstep;
            if (v->env <= 0) { v->env = 0; v->state = V_OFF; return; }
            break;
        default:
            return;
        }
        if (v->left > 0 && --v->left == 0) voice_release(v);
        int32_t amp = (v->peak * (v->env >> 15)) >> 15;
        acc[i] += (wave_sample(v) * amp) >> 15;
        v->phase += v->inc;
    }
}

void audio_render(int16_t *out, int frames) {
    int32_t acc[CHUNK];
    while (frames > 0) {
        while (mel_on && mel_left <= 0) melody_advance();
        int n = frames < CHUNK ? frames : CHUNK;
        if (mel_on && mel_left < n) n = (int)mel_left;
        memset(acc, 0, sizeof(int32_t) * (size_t)n);
        for (int k = 0; k < NV; k++)
            if (voices[k].state != V_OFF) voice_mix(&voices[k], acc, n);
        for (int i = 0; i < n; i++) {
            int32_t s = (acc[i] * master_mul) >> 8;
            out[i] = (int16_t)CLAMP(s, -32767, 32767);
        }
        if (mel_on) mel_left -= n;
        out += n;
        frames -= n;
    }
}

void audio_note_underrun(void) { underruns++; }

/* ---- the 1 kHz tick -------------------------------------------------------------------- */
static void speaker_update(void) {
    if (card) return;
    voice_t *best = NULL;
    for (int k = 0; k < NV; k++) {
        voice_t *v = &voices[k];
        if (v->state != V_OFF && (!best || (int32_t)(v->serial - best->serial) > 0)) best = v;
    }
    uint32_t mhz = best ? best->mhz : 0;
    if (mhz != spk_mhz) {
        spk_mhz = mhz;
        pcspk_set(mhz);
    }
}

static bool any_active(void) {
    if (mel_on) return true;
    for (int k = 0; k < NV; k++)
        if (voices[k].state != V_OFF) return true;
    return false;
}

void audio_tick(void) {
    if (!card) {
        for (int k = 0; k < NV; k++) {
            voice_t *v = &voices[k];
            if (v->state != V_OFF && v->left > 0 && --v->left == 0) v->state = V_OFF;
        }
        if (mel_on) {
            mel_left--;
            while (mel_on && mel_left <= 0) melody_advance();
        }
        speaker_update();
        return;
    }
    bool active = any_active();
    if (!streaming) {
        if (!active) return;
        streaming = true;
        streams++;
        idle_ms = 0;
        card->start();
        return;
    }
    card->pump();
    if (active) {
        idle_ms = 0;
    } else if (++idle_ms >= IDLE_STOP_MS) {
        card->stop();
        streaming = false;
    }
}

/* ---- public API -------------------------------------------------------------------------- */
static bool freq_ok(uint32_t hz) { return hz >= 20 && hz <= 20000; }

void sys_beep(uint32_t freq, uint32_t ms) {
    if (!sound_on) return;
    uint32_t f = irq_save();
    mel_on = false;
    legacy_play(freq_ok(freq) ? freq * 1000u : 0, ms ? ms : 1);
    speaker_update();
    irq_restore(f);
}

void speaker_tone(uint32_t freq) {
    if (!sound_on) return;
    uint32_t f = irq_save();
    mel_on = false;
    legacy_play(freq_ok(freq) ? freq * 1000u : 0, 0);
    speaker_update();
    irq_restore(f);
}

void speaker_off(void) {
    uint32_t f = irq_save();
    mel_on = false;
    legacy_play(0, 0);
    speaker_update();
    irq_restore(f);
}

void sys_play_notes(const note_t *notes, int count) {
    if (!sound_on || count <= 0 || !notes) return;
    if (count > (int)ARRAY_SIZE(mel)) count = (int)ARRAY_SIZE(mel);
    uint32_t f = irq_save();
    memcpy(mel, notes, sizeof(note_t) * (size_t)count);
    mel_n = count;
    mel_pos = -1;
    mel_left = 0;
    mel_on = true;
    legacy_play(0, 0);
    if (!card) {                          /* the PC speaker starts right away */
        melody_advance();
        speaker_update();
    }
    irq_restore(f);
}

bool sys_sound_enabled(void) { return sound_on; }

void sys_set_sound_enabled(bool on) {
    sound_on = on;
    if (!on) sound_stop_all();
}

uint32_t sound_midi_mhz(int midi) {
    if (midi < 0) return 0;
    if (midi > 127) midi = 127;
    int shift = 9 - midi / 12;
    uint32_t top = midi_top[midi % 12];
    if (shift <= 0) return top << -shift;
    return (top + (1u << (shift - 1))) >> shift;
}

static int note_mhz(uint32_t mhz, uint32_t ms, int volume, int waveform) {
    if (!sound_on || mhz < 20000 || mhz > 20000000) return 0;
    uint32_t f = irq_save();
    int id = voice_start(mhz, ms, volume, waveform);
    speaker_update();
    irq_restore(f);
    return id;
}

int sound_note(uint32_t freq_hz, uint32_t ms, int volume, int waveform) {
    if (!freq_ok(freq_hz)) return 0;
    return note_mhz(freq_hz * 1000u, ms, volume, waveform);
}

int sound_note_midi(int midi, uint32_t ms, int volume, int waveform) {
    if (midi < 0 || midi > 127) return 0;
    return note_mhz(sound_midi_mhz(midi), ms, volume, waveform);
}

void sound_release(int id) {
    uint32_t f = irq_save();
    voice_t *v = voice_by_id(id);
    if (v) voice_release(v);
    speaker_update();
    irq_restore(f);
}

bool sound_active(int id) {
    uint32_t f = irq_save();
    bool a = voice_by_id(id) != NULL;
    irq_restore(f);
    return a;
}

void sound_stop_all(void) {
    uint32_t f = irq_save();
    mel_on = false;
    for (int k = 0; k < NV; k++) voice_release(&voices[k]);
    legacy_id = 0;
    speaker_update();
    irq_restore(f);
}

const char *sys_sound_device_name(void) { return card ? card->name : "PC speaker"; }
const char *sound_device_short(void) { return card ? card->short_name : "PC speaker"; }
bool sound_has_card(void) { return card != NULL; }
int  sound_polyphony(void) { return card ? NV : 1; }
int  sound_sample_rate(void) { return rate; }

void sound_set_volume(int pct) {
    pct = CLAMP(pct, 0, 100);
    master_pct = pct;
    master_mul = (pct * 256 + 50) / 100;
}

int sound_get_volume(void) { return master_pct; }

/* ---- init --------------------------------------------------------------------------------- */
/* Sound Blaster 16 support lives in sb16.c; without it there is no SB16 probe. */
__attribute__((weak)) const sound_card_t *sb16_probe(void) { return NULL; }

static uint32_t logged_underruns;

static void audio_report(void *ctx) {
    (void)ctx;
    uint32_t u = underruns;
    if (u != logged_underruns) {
        klog("audio: %u buffer underrun%s so far (%u streams)", u, u == 1 ? "" : "s", streams);
        logged_underruns = u;
    }
}

static void audio_init(void) {
    for (int i = 0; i <= 1024; i++) sine_tab[i] = (int16_t)lround(32767.0 * sin(i * (2.0 * M_PI / 1024.0)));
    const char *vol = sys_cmdline_get("volume");
    if (vol && *vol) sound_set_volume(atoi(vol));
    const char *opt = sys_cmdline_get("sound");
    bool any = !opt || !*opt || !strcmp(opt, "auto");
    const sound_card_t *c = NULL;
    if (any || !strcmp(opt, "ac97")) c = ac97_probe();
    if (!c && (any || !strcmp(opt, "sb16"))) c = sb16_probe();
    if (c) {
        uint32_t f = irq_save();
        for (int k = 0; k < NV; k++) voices[k].state = V_OFF;
        mel_on = false;
        legacy_id = 0;
        spk_mhz = 0;
        pcspk_set(0);
        rate = c->rate;
        card = c;
        irq_restore(f);
        klog("audio: output %s, %d Hz, %d voices, volume %d%%", c->name, rate, NV, master_pct);
    } else {
        klog("audio: output PC speaker (%s)", opt && !any ? "sound= option" : "no AC'97 or Sound Blaster 16 found");
    }
    timer_add(3000, audio_report, NULL, true);
}
REGISTER_INIT(INIT_DRIVER, audio_init);

/* ---- "sound" shell command ------------------------------------------------------------------- */
static void test_chord(void *ctx) {
    (void)ctx;
    static const int chord[4] = { 60, 64, 67, 72 };
    for (int i = 0; i < 4; i++) sound_note_midi(chord[i], 1400, 45, SOUND_PIANO);
}

static int cmd_sound(int argc, char **argv, cmd_writer_t *out) {
    if (argc >= 2 && !strcmp(argv[1], "test")) {
        if (!sound_on) { cmd_puts(out, "sound: sound is off (sound on, or click the speaker icon)\n"); return 1; }
        static const note_t arp[] = { { 523, 110 }, { 659, 110 }, { 784, 110 }, { 1047, 220 } };
        sys_play_notes(arp, ARRAY_SIZE(arp));
        timer_add(600, test_chord, NULL, false);
        cmd_printf(out, "Playing an arpeggio and a C major chord on the %s\n", sys_sound_device_name());
        return 0;
    }
    if (argc >= 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        sys_set_sound_enabled(argv[1][1] == 'n');
    } else if (argc >= 3 && !strcmp(argv[1], "volume")) {
        char *end;
        long v = strtol(argv[2], &end, 10);
        if (*end || v < 0 || v > 100) { cmd_puts(out, "sound: volume must be 0..100\n"); return 2; }
        sound_set_volume((int)v);
    } else if (argc >= 2) {
        cmd_puts(out, "usage: sound [test | on | off | volume 0..100]\n");
        return 2;
    }
    cmd_printf(out, "Output:  %s\n", sys_sound_device_name());
    if (card)
        cmd_printf(out, "Format:  %d Hz, 16-bit, %d voices, volume %d%%\n", rate, NV, master_pct);
    else
        cmd_puts(out, "Format:  one note at a time (VirtualBox: Settings > Audio > ICH AC97)\n");
    cmd_printf(out, "Sound:   %s\n", sound_on ? "on" : "off (muted)");
    if (card) cmd_printf(out, "Stream:  %s, started %u times, %u underruns\n", streaming ? "playing" : "idle", streams, underruns);
    return 0;
}
REGISTER_COMMAND("sound", "sound [test | on | off | volume 0..100] - show the sound output, play a test chord", cmd_sound);
