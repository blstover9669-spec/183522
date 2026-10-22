/*
 * system_piano.c - "Piano": an on-screen piano for the sound card (or the PC speaker).
 *
 *   - 42 keys (C3 .. F6) played with the mouse (click, or drag across the keys
 *     for a glissando) or with the computer keyboard: A S D F G H J K L ; '
 *     are the white keys, W E T Y U O P the black keys (the usual "virtual
 *     piano" layout). Z / X move that keyboard zone an octave down / up.
 *   - Every key is its own synthesizer voice (<nova/audio.h>): with a sound card
 *     (AC'97 / Sound Blaster 16) chords sound, up to 8 notes at once, in the
 *     instrument chosen at the bottom (piano, organ, flute, chiptune, ...). On the
 *     PC speaker only one note sounds: the most recently pressed key that is still
 *     held ("last note priority"); releasing it falls back to the previous one.
 *   - Built-in songs (Ode to Joy, Twinkle Twinkle, Happy Birthday, Jingle
 *     Bells, Fur Elise) are played back from window timer events, never
 *     blocking: each EV_TIMER compares sys_uptime_ms() with the end of the
 *     current note and moves on. Tempo slider, keys light up as they play.
 *   - Practice mode: the song waits for you - play the glowing key (any
 *     octave counts) to move on; mistakes are counted.
 *   - Record: capture what you play (with its timing) and play it back.
 *   - A "piano roll" display scrolls the notes that were just played and,
 *     during a song, the ones coming up.
 *
 * Keys: 1-6 choose a song, Space plays / stops, Esc stops, Z / X octave.
 */
#include "system_common.h"

#define W        852
#define H        446

#define LO_MIDI  48                      /* C3 */
#define HI_MIDI  89                      /* F6 */
#define NWHITE   25
#define KW       32                      /* white key width   */
#define WH       200                     /* white key height  */
#define BW       20                      /* black key width   */
#define BH       124                     /* black key height  */
#define KX0      ((W - NWHITE * KW) / 2)
#define KY0      184
#define CASE_X   16
#define CASE_Y   170
#define CASE_W   (W - 32)
#define CASE_H   (KY0 + WH + 12 - CASE_Y)
#define FOOT_Y   (CASE_Y + CASE_H + 10)  /* bottom row: instrument and output device */
#define FOOT_H   28
#define INS_LX   CASE_X
#define INS_LW   78
#define INS_X    (INS_LX + INS_LW + 8)
#define INS_W    170

#define TB_Y     12
#define TB_H     30
/* toolbar, left to right: song | Play | Record | Practice | Tempo ---o--- 132 | Keys - C4 + | speaker */
#define TB_SONG_X   16
#define TB_SONG_W   184
#define TB_PLAY_X   (TB_SONG_X + TB_SONG_W + 8)
#define TB_PLAY_W   68
#define TB_REC_X    (TB_PLAY_X + TB_PLAY_W + 6)
#define TB_REC_W    74
#define TB_PRAC_X   (TB_REC_X + TB_REC_W + 14)
#define TB_PRAC_W   84
#define TB_TEMPO_LX (TB_PRAC_X + TB_PRAC_W + 10)     /* "Tempo" label */
#define TB_TEMPO_LW 44
#define TB_TEMPO_X  (TB_TEMPO_LX + TB_TEMPO_LW + 4)
#define TB_TEMPO_W  92
#define TB_TEMPO_VW 30                               /* the bpm value */
#define TB_KEYS_LX  (TB_TEMPO_X + TB_TEMPO_W + 4 + TB_TEMPO_VW + 8)
#define TB_KEYS_LW  34
#define TB_OCT_X    (TB_KEYS_LX + TB_KEYS_LW + 4)
#define TB_OCTB_W   26
#define TB_OCTL_W   36
#define TB_SND_W    34
#define TB_SND_X    (W - 16 - TB_SND_W)
#define DISP_X   16
#define DISP_Y   54
#define DISP_W   (W - 32)
#define DISP_H   104
#define INFO_W   200                     /* song / status column at the right */
#define ROLL_X   (DISP_X + 168)
#define ROLL_Y   (DISP_Y + 12)
#define ROLL_W   (DISP_W - 168 - INFO_W - 36)
#define ROLL_H   (DISP_H - 24)
#define ROLL_PX_PER_MS 0.11
#define ROLL_PAST_MS   2700

#define MAXHELD  24
#define HIST     96
#define REC_MAX  1024
#define SONG_MAX 128
#define FRAME_MS 33

#define SRC_USER 0
#define SRC_SONG 1

/* ======================================================================================
 * Songs
 * ==================================================================================== */
typedef struct { uint8_t midi; uint16_t dur; } snote_t;      /* midi 0 = rest; dur in ticks */
typedef struct {
    const char *name;
    int bpm;            /* default tempo (quarter notes per minute)          */
    int tpq;            /* ticks per quarter note                            */
    bool legato;        /* recordings: no gap between notes                  */
    int n;
    snote_t *notes;
} song_t;

/* Notation: NOTE[#|b]OCTAVE DURATION[.]  or  R DURATION[.]   (w h q e s = whole .. sixteenth) */
static const struct { const char *name; int bpm; const char *notes; } song_src[] = {
    { "Ode to Joy", 132,
      "E4q E4q F4q G4q G4q F4q E4q D4q C4q C4q D4q E4q E4q. D4e D4h "
      "E4q E4q F4q G4q G4q F4q E4q D4q C4q C4q D4q E4q D4q. C4e C4h "
      "D4q D4q E4q C4q D4q E4e F4e E4q C4q D4q E4e F4e E4q D4q C4q D4q G3h "
      "E4q E4q F4q G4q G4q F4q E4q D4q C4q C4q D4q E4q D4q. C4e C4h" },
    { "Twinkle Twinkle Little Star", 112,
      "C4q C4q G4q G4q A4q A4q G4h F4q F4q E4q E4q D4q D4q C4h "
      "G4q G4q F4q F4q E4q E4q D4h G4q G4q F4q F4q E4q E4q D4h "
      "C4q C4q G4q G4q A4q A4q G4h F4q F4q E4q E4q D4q D4q C4h" },
    { "Happy Birthday", 104,
      "G4e. G4s A4q G4q C5q B4h G4e. G4s A4q G4q D5q C5h "
      "G4e. G4s G5q E5q C5q B4q A4q F5e. F5s E5q C5q D5q C5h" },
    { "Jingle Bells", 160,
      "E4q E4q E4h E4q E4q E4h E4q G4q C4q. D4e E4w "
      "F4q F4q F4q. F4e F4q E4q E4q E4e E4e E4q D4q D4q E4q D4h G4h "
      "E4q E4q E4h E4q E4q E4h E4q G4q C4q. D4e E4w "
      "F4q F4q F4q F4q F4q E4q E4q E4e E4e G4q G4q F4q D4q C4w" },
    { "Fur Elise", 84,
      "E5s D#5s E5s D#5s E5s B4s D5s C5s A4e Rs C4s E4s A4s B4e Rs E4s G#4s B4s C5e Rs E4s "
      "E5s D#5s E5s D#5s E5s B4s D5s C5s A4e Rs C4s E4s A4s B4e Rs E4s C5s B4s A4q" },
};
#define NSONGS ARRAY_SIZE(song_src)

static song_t g_songs[NSONGS];
static snote_t g_song_notes[NSONGS][SONG_MAX];
static bool g_songs_ready;

/* instruments: synthesizer waveform and a volume that evens out their loudness */
static const struct { const char *name, *word; int wave, volume; } instruments[] = {
    { "Grand piano",        "piano",    SOUND_PIANO,    72 },
    { "Organ",              "organ",    SOUND_ORGAN,    50 },
    { "Flute (triangle)",   "flute",    SOUND_TRIANGLE, 70 },
    { "Chiptune (square)",  "chip",     SOUND_SQUARE,   34 },
    { "Pure tone (sine)",   "sine",     SOUND_SINE,     66 },
    { "Synth lead (saw)",   "saw",      SOUND_SAW,      38 },
};
#define NINSTR ARRAY_SIZE(instruments)

static int parse_song(const char *p, snote_t *out, int max) {
    static const int8_t semi[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* A B C D E F G */
    int n = 0;
    while (*p && n < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        int midi = 0;
        if (*p == 'R') {
            p++;
        } else if (*p >= 'A' && *p <= 'G') {
            int s = semi[*p - 'A'];
            p++;
            if (*p == '#') { s++; p++; } else if (*p == 'b') { s--; p++; }
            int oct = isdigit((unsigned char)*p) ? *p++ - '0' : 4;
            midi = (oct + 1) * 12 + s;
        } else {
            p++;
            continue;
        }
        int d = 4;
        switch (*p) {
        case 'w': d = 16; p++; break;
        case 'h': d = 8; p++; break;
        case 'q': d = 4; p++; break;
        case 'e': d = 2; p++; break;
        case 's': d = 1; p++; break;
        default: break;
        }
        if (*p == '.') { d = d * 3 / 2; p++; }
        out[n].midi = (uint8_t)CLAMP(midi, 0, 127);
        out[n].dur = (uint16_t)d;
        n++;
    }
    return n;
}

static void songs_init(void) {
    if (g_songs_ready) return;
    for (int i = 0; i < NSONGS; i++) {
        g_songs[i].name = song_src[i].name;
        g_songs[i].bpm = song_src[i].bpm;
        g_songs[i].tpq = 4;
        g_songs[i].legato = false;
        g_songs[i].notes = g_song_notes[i];
        g_songs[i].n = parse_song(song_src[i].notes, g_song_notes[i], SONG_MAX);
    }
    g_songs_ready = true;
}

/* ======================================================================================
 * State
 * ==================================================================================== */
typedef struct { int key; uint8_t midi; int voice; } held_t;      /* key -1 = mouse; voice 0 = silent */
typedef struct { uint8_t midi, src; uint32_t t0, t1; } hist_t;    /* t1 0 = still sounding */

typedef struct {
    window_t *win;
    int id;
    widget_t *songs, *play, *rec_btn, *practice, *tempo, *oct_dn, *oct_up, *sound, *instr;
    int instrument;                 /* index into instruments[]            */
    int song_voice;                 /* synthesizer voice of the song note  */
    int base;                       /* MIDI note of the 'A' key            */
    held_t held[MAXHELD];
    int nheld;
    int user_midi, song_midi, sound_midi, last_midi;
    int mouse_midi, hover_midi;
    /* song playback */
    int song;
    bool playing, practice_run;
    int pos;
    uint32_t note_t0;
    int correct, mistakes;
    int wrong_midi;
    uint32_t wrong_until;
    uint32_t jingle_at;
    /* piano roll */
    hist_t hist[HIST];
    int hist_head, hist_open[2];
    /* recording */
    bool recording, have_rec;
    uint32_t rec_t0, rec_last;
    int rec_cur, rec_n, rec_notes;
    snote_t *rec;
    song_t rec_song;
    /* misc */
    char msg[96];
    uint32_t msg_until;
    uint32_t last_frame;
    bool sound_on, keys_dirty;
} piano_t;

static const char map_keys[18] = { 'a', 'w', 's', 'e', 'd', 'f', 't', 'g', 'y', 'h', 'u', 'j', 'k', 'o', 'l', 'p', ';', '\'' };
static const char *const note_names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
static const int8_t white_of[12] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };
static const int8_t white_semi[7] = { 0, 2, 4, 5, 7, 9, 11 };

static bool is_black(int m) { return white_of[m % 12] < 0; }
static double midi_freq(int m) { return 440.0 * pow(2.0, (m - 69) / 12.0); }
static void midi_name(int m, char *out, int n) { snprintf(out, (size_t)n, "%s%d", note_names[m % 12], m / 12 - 1); }

static int map_offset(int key) {
    if (key >= 'A' && key <= 'Z') key += 32;
    for (int i = 0; i < 18; i++)
        if (map_keys[i] == key) return i;
    return -1;
}

static const song_t *cur_song(piano_t *st) {
    if (st->song >= 0 && st->song < NSONGS) return &g_songs[st->song];
    if (st->song == NSONGS && st->have_rec) return &st->rec_song;
    return NULL;
}

static uint32_t note_ms(piano_t *st, const song_t *sg, int ticks) {
    int bpm = st->tempo ? ui_get_value(st->tempo) : sg->bpm;
    if (bpm < 20) bpm = 20;
    uint32_t ms = (uint32_t)(ticks * 60000.0 / ((double)bpm * sg->tpq) + 0.5);
    return ms ? ms : 1;
}

static uint32_t note_gap(const song_t *sg, uint32_t dur) {
    if (sg->legato) return 0;
    uint32_t g = dur / 5;
    return g > 70 ? 70 : g;
}

static void set_msg(piano_t *st, const char *m, uint32_t ms) {
    strlcpy(st->msg, m, sizeof st->msg);
    st->msg_until = sys_uptime_ms() + ms;
    win_invalidate_rect(st->win, DISP_X, DISP_Y, DISP_W, DISP_H);
}

/* ---- geometry --------------------------------------------------------------------------- */
static int white_index(int m) { return (m / 12 - LO_MIDI / 12) * 7 + white_of[m % 12]; }

static rect_t key_rect(int m) {
    if (!is_black(m)) return rect_make(KX0 + white_index(m) * KW, KY0, KW, WH);
    static const int8_t off[12] = { 0, -3, 0, 3, 0, 0, -4, 0, 0, 0, 4, 0 };
    int cx = KX0 + (white_index(m - 1) + 1) * KW + off[m % 12];
    return rect_make(cx - BW / 2, KY0, BW, BH);
}

static int key_at(int x, int y) {
    if (y < KY0 || y >= KY0 + WH || x < KX0 || x >= KX0 + NWHITE * KW) return 0;
    for (int m = LO_MIDI; m <= HI_MIDI; m++)
        if (is_black(m) && rect_contains(key_rect(m), x, y)) return m;
    int i = (x - KX0) / KW;
    return LO_MIDI + (i / 7) * 12 + white_semi[i % 7];
}

static void inval_keys(piano_t *st) { win_invalidate_rect(st->win, CASE_X, CASE_Y, CASE_W, CASE_H); }
static void inval_disp(piano_t *st) { win_invalidate_rect(st->win, DISP_X, DISP_Y, DISP_W, DISP_H); }

/* ======================================================================================
 * Sound and note bookkeeping
 * ==================================================================================== */
/* Each held key and the current song note has its own voice. On the PC speaker (one
   note at a time, newest wins) a song note does not start while the user holds a key,
   so what the user plays keeps priority as before. */
static bool mono_output(void) { return sound_polyphony() <= 1; }

static int note_on(piano_t *st, int midi) {
    return sound_note_midi(midi, 0, instruments[st->instrument].volume, instruments[st->instrument].wave);
}

static void note_off(int *voice) {
    if (*voice) sound_release(*voice);
    *voice = 0;
}

static void update_sound(piano_t *st) {
    st->sound_midi = st->user_midi ? st->user_midi : st->song_midi;
}

/* start voices for notes that should sound but have none (after unmuting, or a song
   note held back on the PC speaker while keys were down) */
static void resound(piano_t *st) {
    if (!sys_sound_enabled()) return;
    for (int i = 0; i < st->nheld; i++)
        if (!st->held[i].voice) st->held[i].voice = note_on(st, st->held[i].midi);
    if (st->song_midi && !st->song_voice && !(mono_output() && st->nheld)) st->song_voice = note_on(st, st->song_midi);
}

/* sound was switched off: the synthesizer has let go of every voice */
static void forget_voices(piano_t *st) {
    for (int i = 0; i < st->nheld; i++) st->held[i].voice = 0;
    st->song_voice = 0;
}

static void hist_change(piano_t *st, int src, int midi, uint32_t now) {
    int o = st->hist_open[src];
    if (o >= 0 && st->hist[o].src == src && st->hist[o].t1 == 0) st->hist[o].t1 = now ? now : 1;
    st->hist_open[src] = -1;
    if (midi) {
        hist_t *h = &st->hist[st->hist_head];
        h->midi = (uint8_t)midi;
        h->src = (uint8_t)src;
        h->t0 = now;
        h->t1 = 0;
        st->hist_open[src] = st->hist_head;
        st->hist_head = (st->hist_head + 1) % HIST;
    }
}

static void rec_change(piano_t *st, int midi, uint32_t now);

static void set_user_note(piano_t *st, int midi) {
    if (midi == st->user_midi) return;
    uint32_t now = sys_uptime_ms();
    hist_change(st, SRC_USER, midi, now);
    if (st->recording) rec_change(st, midi, now);
    st->user_midi = midi;
    if (midi) st->last_midi = midi;
    update_sound(st);
    inval_keys(st);
    inval_disp(st);
}

static void set_song_note(piano_t *st, int midi) {
    if (midi == st->song_midi) return;
    hist_change(st, SRC_SONG, midi, sys_uptime_ms());
    st->song_midi = midi;
    note_off(&st->song_voice);
    if (midi && !(mono_output() && st->nheld)) st->song_voice = note_on(st, midi);
    if (midi && !st->user_midi) st->last_midi = midi;
    update_sound(st);
    inval_keys(st);
    inval_disp(st);
}

static void refresh_user_note(piano_t *st) {
    set_user_note(st, st->nheld ? st->held[st->nheld - 1].midi : 0);
}

static void release_key(piano_t *st, int key) {
    for (int i = 0; i < st->nheld; i++) {
        if (st->held[i].key != key) continue;
        note_off(&st->held[i].voice);
        memmove(&st->held[i], &st->held[i + 1], sizeof(held_t) * (size_t)(st->nheld - i - 1));
        st->nheld--;
        break;
    }
    if (!st->nheld) resound(st);          /* PC speaker: the song note comes back */
    refresh_user_note(st);
}

static void release_all(piano_t *st) {
    for (int i = 0; i < st->nheld; i++) note_off(&st->held[i].voice);
    st->nheld = 0;
    st->mouse_midi = 0;
    resound(st);
    refresh_user_note(st);
}

static void song_stop(piano_t *st, const char *why);
static void practice_check(piano_t *st, int midi);

static void press_key(piano_t *st, int key, int midi) {
    if (midi < 1 || midi > 127) return;
    for (int i = 0; i < st->nheld; i++)
        if (st->held[i].key == key) {
            note_off(&st->held[i].voice);
            memmove(&st->held[i], &st->held[i + 1], sizeof(held_t) * (size_t)(st->nheld - i - 1));
            st->nheld--;
            break;
        }
    if (st->nheld >= MAXHELD) {
        note_off(&st->held[0].voice);
        memmove(&st->held[0], &st->held[1], sizeof(held_t) * (MAXHELD - 1));
        st->nheld--;
    }
    /* the same note held by another key (mouse and keyboard) is struck again, not doubled */
    for (int i = 0; i < st->nheld; i++)
        if (st->held[i].midi == midi) note_off(&st->held[i].voice);
    st->held[st->nheld].key = key;
    st->held[st->nheld].midi = (uint8_t)midi;
    st->held[st->nheld].voice = note_on(st, midi);
    st->nheld++;
    /* a key pressed again while it is already sounding re-strikes it */
    if (st->user_midi == midi) set_user_note(st, 0);
    refresh_user_note(st);
    practice_check(st, midi);
}

static bool user_holds(piano_t *st, int midi) {
    for (int i = 0; i < st->nheld; i++)
        if (st->held[i].midi == midi) return true;
    return false;
}

/* ======================================================================================
 * Recording
 * ==================================================================================== */
static void rec_change(piano_t *st, int midi, uint32_t now) {
    if (!st->rec) return;
    uint32_t d = now - st->rec_last;
    if (st->rec_n > 0 || st->rec_cur) {               /* leading silence is skipped */
        if (st->rec_n >= REC_MAX) {
            st->recording = false;
            ui_set_text(st->rec_btn, "Record");
            set_msg(st, "Recording is full", 3000);
            return;
        }
        uint32_t cap = st->rec_cur ? 60000 : 3000;
        st->rec[st->rec_n].midi = (uint8_t)st->rec_cur;
        st->rec[st->rec_n].dur = (uint16_t)CLAMP(d, 1u, cap);
        st->rec_n++;
    }
    if (midi) st->rec_notes++;
    st->rec_cur = midi;
    st->rec_last = now;
}

static void rec_start(piano_t *st) {
    if (st->playing) song_stop(st, NULL);
    if (!st->rec) st->rec = malloc(sizeof(snote_t) * REC_MAX);
    if (!st->rec) { set_msg(st, "Not enough memory to record", 3000); return; }
    /* the take being recorded replaces the previous one */
    if (st->song == NSONGS) st->song = 0;
    st->have_rec = false;
    if (ui_list_count(st->songs) > NSONGS) ui_list_remove(st->songs, NSONGS);
    ui_set_value(st->songs, st->song);
    st->recording = true;
    st->rec_n = 0;
    st->rec_cur = st->user_midi;
    st->rec_notes = st->user_midi ? 1 : 0;
    st->rec_t0 = st->rec_last = sys_uptime_ms();
    ui_set_text(st->rec_btn, "Stop rec");
    set_msg(st, "Recording - play something!", 2500);
    klog("piano: recording");
}

static void rec_stop(piano_t *st) {
    if (!st->recording) return;
    rec_change(st, 0, sys_uptime_ms());
    st->recording = false;
    while (st->rec_n > 0 && st->rec[st->rec_n - 1].midi == 0) st->rec_n--;
    ui_set_text(st->rec_btn, "Record");
    int notes = 0;
    for (int i = 0; i < st->rec_n; i++) notes += st->rec[i].midi != 0;
    if (!notes) { set_msg(st, "Nothing was recorded", 3000); return; }
    st->rec_song.name = "My recording";
    st->rec_song.bpm = 120;
    st->rec_song.tpq = 500;           /* at 120 bpm one tick = 1 ms */
    st->rec_song.legato = true;
    st->rec_song.n = st->rec_n;
    st->rec_song.notes = st->rec;
    st->have_rec = true;
    if (ui_list_count(st->songs) <= NSONGS) ui_list_add(st->songs, "My recording");
    st->song = NSONGS;
    ui_set_value(st->songs, NSONGS);
    ui_set_value(st->tempo, 120);
    char b[96];
    snprintf(b, sizeof b, "Recorded %d notes - press Play (Space) to hear them", notes);
    set_msg(st, b, 4000);
    klog("piano: recorded %d notes", notes);
    win_invalidate(st->win);
}

/* ======================================================================================
 * Song playback (driven by EV_TIMER)
 * ==================================================================================== */
static int next_sounding(const song_t *sg, int i) {
    while (i < sg->n && sg->notes[i].midi == 0) i++;
    return i;
}

static void song_start(piano_t *st) {
    if (st->recording) rec_stop(st);
    const song_t *sg = cur_song(st);
    if (!sg || sg->n <= 0) return;
    st->playing = true;
    st->practice_run = ui_get_value(st->practice) != 0;
    st->correct = st->mistakes = 0;
    st->note_t0 = sys_uptime_ms();
    st->msg_until = 0;
    st->jingle_at = 0;
    if (st->practice_run) {
        st->pos = next_sounding(sg, 0);
        set_song_note(st, 0);
    } else {
        st->pos = 0;
        set_song_note(st, sg->notes[0].midi);
    }
    ui_set_text(st->play, "Stop");
    klog("piano: %s \"%s\" (%d notes, %d bpm)", st->practice_run ? "practising" : "playing", sg->name, sg->n,
         ui_get_value(st->tempo));
    win_invalidate(st->win);
}

static void song_stop(piano_t *st, const char *why) {
    if (!st->playing) return;
    st->playing = false;
    set_song_note(st, 0);
    ui_set_text(st->play, "Play");
    if (why) set_msg(st, why, 4000);
    win_invalidate(st->win);
}

static void song_tick(piano_t *st, uint32_t now) {
    if (!st->playing || st->practice_run) return;
    const song_t *sg = cur_song(st);
    if (!sg) { song_stop(st, NULL); return; }
    for (int guard = 0; guard < 64 && st->playing; guard++) {
        uint32_t dur = note_ms(st, sg, sg->notes[st->pos].dur);
        uint32_t end = st->note_t0 + dur;
        if (st->song_midi && (int32_t)(now - (end - note_gap(sg, dur))) >= 0) set_song_note(st, 0);
        if ((int32_t)(now - end) < 0) break;
        st->pos++;
        /* far behind (e.g. the machine was busy): carry on from now instead of racing */
        st->note_t0 = (now - end > 400) ? now : end;
        if (st->pos >= sg->n) {
            song_stop(st, "Finished - press Play to hear it again");
            klog("piano: song finished");
            break;
        }
        set_song_note(st, sg->notes[st->pos].midi);
    }
}

static void practice_check(piano_t *st, int midi) {
    if (!st->playing || !st->practice_run) return;
    const song_t *sg = cur_song(st);
    if (!sg || st->pos >= sg->n) return;
    int want = sg->notes[st->pos].midi;
    if (midi % 12 == want % 12) {
        st->correct++;
        st->pos = next_sounding(sg, st->pos + 1);
        st->note_t0 = sys_uptime_ms();
        if (st->pos >= sg->n) {
            char b[96];
            if (st->mistakes == 0) snprintf(b, sizeof b, "Perfect! %d notes, no mistakes", st->correct);
            else snprintf(b, sizeof b, "Well done! %d notes, %d mistake%s", st->correct, st->mistakes,
                          st->mistakes == 1 ? "" : "s");
            song_stop(st, b);
            st->jingle_at = sys_uptime_ms() + 450;
            klog("piano: practice done: %d notes, %d mistakes", st->correct, st->mistakes);
        }
    } else {
        st->mistakes++;
        st->wrong_midi = midi;
        st->wrong_until = sys_uptime_ms() + 350;
    }
    inval_keys(st);
    inval_disp(st);
}

static void select_song(piano_t *st, int i) {
    int max = st->have_rec ? NSONGS : NSONGS - 1;
    if (i < 0 || i > max) return;
    if (st->recording) rec_stop(st);
    song_stop(st, NULL);
    st->song = i;
    ui_set_value(st->songs, i);
    const song_t *sg = cur_song(st);
    if (sg) ui_set_value(st->tempo, sg->bpm);
    st->msg_until = 0;
    win_invalidate(st->win);
}

static void set_base(piano_t *st, int base) {
    base = CLAMP(base, LO_MIDI, HI_MIDI - 17);
    if (base == st->base) return;
    st->base = base;
    ui_set_enabled(st->oct_dn, base > LO_MIDI);
    ui_set_enabled(st->oct_up, base < HI_MIDI - 17);
    win_invalidate(st->win);
}

/* ---- widget callbacks ------------------------------------------------------------------- */
/* Space / Enter mean Play / Stop: after a click on another control hand the keyboard focus
   back to the Play button, so a focused checkbox, dropdown or "+" does not swallow them. */
static void refocus(piano_t *st) {
    if (st->play) ui_focus(st->play);
}

static void on_play(widget_t *w, void *ctx) {
    piano_t *st = ctx;
    (void)w;
    if (st->playing) song_stop(st, NULL);
    else song_start(st);
}

static void on_rec(widget_t *w, void *ctx) {
    piano_t *st = ctx;
    (void)w;
    if (st->recording) { rec_stop(st); refocus(st); }
    else rec_start(st);
}

static void on_song(widget_t *w, void *ctx) {
    select_song(ctx, ui_get_value(w));
    refocus(ctx);
}

static void on_tempo(widget_t *w, void *ctx) {
    piano_t *st = ctx;
    (void)w;
    win_invalidate_rect(st->win, 0, 0, W, DISP_Y);
    inval_disp(st);
}

static void on_practice(widget_t *w, void *ctx) {
    piano_t *st = ctx;
    (void)w;
    if (st->playing) { song_stop(st, NULL); song_start(st); }
    refocus(st);
    inval_disp(st);
}

static void on_instr(widget_t *w, void *ctx) {
    piano_t *st = ctx;
    st->instrument = CLAMP(ui_get_value(w), 0, (int)NINSTR - 1);
    refocus(st);
}

static void on_oct_dn(widget_t *w, void *ctx) { (void)w; set_base(ctx, ((piano_t *)ctx)->base - 12); refocus(ctx); }
static void on_oct_up(widget_t *w, void *ctx) { (void)w; set_base(ctx, ((piano_t *)ctx)->base + 12); refocus(ctx); }

/* speaker on/off toggle (custom widget) */
static void speaker_glyph(surface_t *s, double x, double y, double sz, uint32_t c, bool on) {
    double u = sz / 20.0;
    point_t p[6] = {
        { (int)lround(x + 2 * u), (int)lround(y + 7 * u) },  { (int)lround(x + 6 * u), (int)lround(y + 7 * u) },
        { (int)lround(x + 11 * u), (int)lround(y + 2.5 * u) }, { (int)lround(x + 11 * u), (int)lround(y + 17.5 * u) },
        { (int)lround(x + 6 * u), (int)lround(y + 13 * u) }, { (int)lround(x + 2 * u), (int)lround(y + 13 * u) },
    };
    gfx_fill_polygon(s, p, 6, c);
    if (on) {
        sys_arc(s, x + 11 * u, y + 10 * u, 3.6 * u, -0.85, 0.85, 1.7 * u, c);
        sys_arc(s, x + 11 * u, y + 10 * u, 7.2 * u, -0.85, 0.85, 1.7 * u, c);
    } else {
        sys_stroke(s, x + 14 * u, y + 7 * u, x + 19 * u, y + 13 * u, 1.9 * u, c);
        sys_stroke(s, x + 19 * u, y + 7 * u, x + 14 * u, y + 13 * u, 1.9 * u, c);
    }
}

static void sound_draw(widget_t *w, surface_t *s, rect_t r) {
    piano_t *st = ui_get_ctx(w);
    const theme_t *t = theme_get();
    int state = sys_mouse_in(st->win, r, NULL, NULL) ? UI_STATE_HOVER : 0;
    if (ui_has_focus(w) && win_is_focused(st->win)) state |= UI_STATE_FOCUSED;
    ui_draw_button(s, r, "", state);
    bool on = sys_sound_enabled();
    speaker_glyph(s, r.x + (r.w - 18) / 2.0, r.y + (r.h - 18) / 2.0, 18, on ? t->button_text : RGB(220, 70, 60), on);
}

static void toggle_sound(piano_t *st) {
    bool on = !sys_sound_enabled();
    sys_set_sound_enabled(on);
    if (on) resound(st);                 /* held notes sound again after unmuting */
    else forget_voices(st);
    if (on && !st->nheld && !st->song_midi) sys_beep(880, 40);
    st->sound_on = on;
    ui_invalidate(st->sound);
    inval_disp(st);
}

static bool sound_event(widget_t *w, const event_t *ev) {
    piano_t *st = ui_get_ctx(w);
    switch (ev->type) {
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_LEFT) { toggle_sound(st); refocus(st); }
        return true;
    case EV_MOUSE_UP:
        return true;
    case EV_MOUSE_MOVE:
        ui_invalidate(w);
        return true;
    case EV_KEY_DOWN:
        if (!(ev->mods & (MOD_CTRL | MOD_ALT)) && (ev->key == KEY_SPACE || ev->key == KEY_ENTER)) {
            toggle_sound(st);
            return true;
        }
        return false;
    default:
        return false;
    }
}

/* ======================================================================================
 * Painting
 * ==================================================================================== */
static uint32_t pitch_color(int m) { return color_hsv(((m % 12) * 30 + 200) % 360, 150, 255); }

static void fmt_time(char *b, int n, uint32_t ms) {
    uint32_t s = ms / 1000;
    snprintf(b, (size_t)n, "%u:%02u", s / 60, s % 60);
}

/* pitch range shown by the piano roll: the song's notes plus whatever was just played,
   at least an octave and a bit, so neighbouring notes are visibly apart */
static int g_roll_lo = 55, g_roll_hi = 79;

static void roll_range(piano_t *st, const song_t *sg, uint32_t now) {
    int lo = 127, hi = 0;
    if (sg)
        for (int i = 0; i < sg->n; i++)
            if (sg->notes[i].midi) { lo = MIN(lo, sg->notes[i].midi); hi = MAX(hi, sg->notes[i].midi); }
    for (int i = 0; i < HIST; i++) {
        hist_t *h = &st->hist[i];
        if (!h->midi || (h->t1 && now - h->t1 > ROLL_PAST_MS + 200)) continue;
        lo = MIN(lo, h->midi);
        hi = MAX(hi, h->midi);
    }
    if (lo > hi) { lo = st->base; hi = st->base + 12; }
    lo -= 2;
    hi += 2;
    while (hi - lo < 16) { lo--; hi++; }
    g_roll_lo = lo;
    g_roll_hi = hi;
}

static int roll_y(int m) {
    m = CLAMP(m, g_roll_lo, g_roll_hi);
    return ROLL_Y + ROLL_H - 6 - (m - g_roll_lo) * (ROLL_H - 12) / (g_roll_hi - g_roll_lo);
}

static void roll_bar(surface_t *s, double x0, double x1, int m, uint32_t c, bool glow) {
    int xa = (int)floor(x0), xb = (int)ceil(x1);
    if (xb - xa < 4) xb = xa + 4;
    int y = roll_y(m) - 2;
    if (glow) gfx_fill_rounded_rect(s, xa - 2, y - 2, xb - xa + 4, 9, 4, color_alpha(c, 60));
    gfx_fill_rounded_rect(s, xa, y, xb - xa, 5, 2, c);
}

static void paint_roll(piano_t *st, surface_t *s, uint32_t now) {
    const theme_t *t = theme_get();
    rect_t rr = rect_make(ROLL_X, ROLL_Y, ROLL_W, ROLL_H);
    gfx_fill_rounded_rect(s, rr.x, rr.y, rr.w, rr.h, 8, RGBA(255, 255, 255, 12));
    rect_t old = s->clip;
    s->clip = rect_intersect(old, rr);
    const song_t *sg = cur_song(st);
    roll_range(st, sg, now);
    int c0 = (g_roll_lo + 11) / 12 * 12;                 /* first C in range */
    for (int m = c0; m <= g_roll_hi; m += 12) gfx_hline(s, rr.x + 6, roll_y(m), rr.w - 12, RGBA(255, 255, 255, 18));
    double ph = rr.x + rr.w * 0.62;

    /* coming up */
    if (st->playing && sg) {
        if (st->practice_run) {
            double x = ph;
            for (int i = st->pos; i < sg->n && x < rr.x + rr.w; i++) {
                uint32_t d = note_ms(st, sg, sg->notes[i].dur);
                double w = d * ROLL_PX_PER_MS;
                if (sg->notes[i].midi) {
                    bool first = i == st->pos;
                    double pulse = 0.5 + 0.5 * sin(now / 140.0);
                    uint32_t c = first ? color_mix(t->accent, COLOR_WHITE, (int)(pulse * 90))
                                       : color_alpha(pitch_color(sg->notes[i].midi), 110);
                    roll_bar(s, x, x + MAX(4.0, w - note_gap(sg, d) * ROLL_PX_PER_MS), sg->notes[i].midi, c, first);
                }
                x += w;
            }
        } else {
            uint32_t tt = st->note_t0;
            for (int i = st->pos; i < sg->n; i++) {
                uint32_t d = note_ms(st, sg, sg->notes[i].dur);
                if (i > st->pos && sg->notes[i].midi) {
                    double x0 = ph + ((double)tt - now) * ROLL_PX_PER_MS;
                    double x1 = ph + ((double)(tt + d - note_gap(sg, d)) - now) * ROLL_PX_PER_MS;
                    roll_bar(s, x0, x1, sg->notes[i].midi, color_alpha(pitch_color(sg->notes[i].midi), 110), false);
                }
                tt += d;
                if (tt > now + 2500) break;
            }
        }
    }
    /* just played */
    for (int i = 0; i < HIST; i++) {
        hist_t *h = &st->hist[i];
        if (!h->midi) continue;
        uint32_t t1 = h->t1 ? h->t1 : now;
        if (now - t1 > ROLL_PAST_MS + 200 || (int32_t)(t1 - h->t0) < 0) continue;
        double x0 = ph - (double)(now - h->t0) * ROLL_PX_PER_MS;
        double x1 = ph - (double)(now - t1) * ROLL_PX_PER_MS;
        uint32_t c = h->src == SRC_USER ? color_mix(t->accent, COLOR_WHITE, 70) : pitch_color(h->midi);
        roll_bar(s, x0, x1, h->midi, c, h->t1 == 0);
    }
    /* playhead */
    gfx_fill_rect(s, (int)ph - 1, rr.y + 4, 2, rr.h - 8, color_alpha(t->accent, 210));
    gfx_fill_rect(s, (int)ph - 3, rr.y + 4, 6, rr.h - 8, color_alpha(t->accent, 40));
    gfx_fill_triangle(s, (int)ph - 5, rr.y + 2, (int)ph + 5, rr.y + 2, (int)ph, rr.y + 8, t->accent);
    /* fade at the left edge */
    for (int i = 0; i < 24; i++) gfx_vline(s, rr.x + i, rr.y, rr.h, RGBA(22, 25, 34, 230 - i * 9));
    for (int m = c0; m <= g_roll_hi; m += 12) {
        char b[8];
        midi_name(m, b, sizeof b);
        int ly = roll_y(m) - 15;
        if (ly < rr.y + 1) ly = roll_y(m) + 2;
        gfx_text(s, rr.x + 6, ly, b, RGBA(255, 255, 255, 80), FONT_UI);
    }
    s->clip = old;
}

static void paint_display(piano_t *st, surface_t *s, uint32_t now) {
    const theme_t *t = theme_get();
    rect_t d = rect_make(DISP_X, DISP_Y, DISP_W, DISP_H);
    gfx_shadow(s, d.x, d.y, d.w, d.h, 10, 6, t->dark ? 80 : 45);
    gfx_fill_rounded_gradient_v(s, d.x, d.y, d.w, d.h, 10, RGB(34, 38, 52), RGB(18, 20, 28));
    gfx_rounded_rect(s, d.x, d.y, d.w, d.h, 10, RGBA(255, 255, 255, 26));
    uint32_t white = RGB(240, 242, 248), dim = RGB(150, 156, 172);

    /* left: the note that sounds */
    int lx = d.x + 18;
    int shown = st->sound_midi ? st->sound_midi : (st->user_midi ? st->user_midi : st->song_midi);
    bool live = shown != 0;
    if (!shown) shown = st->last_midi;
    gfx_text(s, lx, d.y + 12, live ? "NOW PLAYING" : "LAST NOTE", RGBA(255, 255, 255, 110), FONT_UI_BOLD);
    if (shown) {
        char nm[8], fq[24];
        midi_name(shown, nm, sizeof nm);
        int tw = gfx_text(s, lx, d.y + 30, nm, live ? white : RGB(120, 126, 142), FONT_UI_TITLE);
        if (live) gfx_fill_circle_aa(s, lx + tw + 10, d.y + 48, 4, t->accent);
        snprintf(fq, sizeof fq, "%.1f Hz", midi_freq(shown));
        gfx_text(s, lx, d.y + 66, fq, dim, FONT_UI);
    } else {
        gfx_text(s, lx, d.y + 30, "--", RGB(120, 126, 142), FONT_UI_TITLE);
        gfx_text(s, lx, d.y + 66, "Press a key", dim, FONT_UI);
    }
    if (!sys_sound_enabled()) gfx_text(s, lx, d.y + 82, "Sound is off", RGB(255, 176, 64), FONT_UI_BOLD);

    paint_roll(st, s, now);

    /* right: song, status, progress */
    int rx = d.x + d.w - INFO_W - 18, rw = INFO_W;
    const song_t *sg = cur_song(st);
    char status[96], timeb[40];
    status[0] = timeb[0] = 0;
    double prog = 0;
    uint32_t dotc = 0;
    if (st->recording) {
        fmt_time(timeb, sizeof timeb, now - st->rec_t0);
        snprintf(status, sizeof status, "Recording  %s", timeb);
        timeb[0] = 0;
        snprintf(timeb, sizeof timeb, "%d note%s", st->rec_notes, st->rec_notes == 1 ? "" : "s");
        dotc = ((now / 500) & 1) ? RGB(255, 70, 70) : RGB(150, 40, 40);
    } else if (st->playing && sg && st->practice_run) {
        int total = 0, done = 0;
        for (int i = 0; i < sg->n; i++)
            if (sg->notes[i].midi) { total++; if (i < st->pos) done++; }
        snprintf(status, sizeof status, "Practice: note %d of %d", MIN(done + 1, total), total);
        snprintf(timeb, sizeof timeb, "%d mistake%s", st->mistakes, st->mistakes == 1 ? "" : "s");
        prog = total ? (double)done / total : 0;
        dotc = t->accent;
    } else if (st->playing && sg) {
        uint32_t total = 0, el = 0;
        for (int i = 0; i < sg->n; i++) {
            uint32_t dd = note_ms(st, sg, sg->notes[i].dur);
            if (i < st->pos) el += dd;
            total += dd;
        }
        el += MIN(now - st->note_t0, note_ms(st, sg, sg->notes[MIN(st->pos, sg->n - 1)].dur));
        char a[16], b[16];
        fmt_time(a, sizeof a, el);
        fmt_time(b, sizeof b, total);
        snprintf(status, sizeof status, "Playing");
        snprintf(timeb, sizeof timeb, "%s / %s", a, b);
        prog = total ? (double)el / total : 0;
        dotc = RGB(80, 220, 120);
    } else if (sg) {
        uint32_t total = 0;
        for (int i = 0; i < sg->n; i++) total += note_ms(st, sg, sg->notes[i].dur);
        char b[16];
        fmt_time(b, sizeof b, total);
        snprintf(status, sizeof status, ui_get_value(st->practice) ? "Ready to practise" : "Ready");
        snprintf(timeb, sizeof timeb, "Length %s", b);
    }
    if (st->msg_until && (int32_t)(now - st->msg_until) < 0) strlcpy(status, st->msg, sizeof status);

    int ty = d.y + 10;
    int nx = rx;
    if (dotc) { gfx_fill_circle_aa(s, rx + 4, ty + 9, 4, dotc); nx += 14; }
    gfx_text_ellipsis(s, nx, ty, rw - (nx - rx), sg ? sg->name : "Free play", white, FONT_UI_BOLD);
    /* status may be a longer message: wrap it over two lines */
    rect_t sr = rect_make(rx, ty + 20, rw, 2 * font_height(FONT_UI));
    rect_t old = s->clip;
    s->clip = rect_intersect(old, sr);
    gfx_text_wrapped(s, sr, status, dim, FONT_UI);
    s->clip = old;
    int py = d.y + 70;
    gfx_fill_rounded_rect(s, rx, py, rw, 6, 3, RGBA(255, 255, 255, 30));
    int pw = (int)(rw * CLAMP(prog, 0.0, 1.0));
    if (pw > 0) gfx_fill_rounded_rect(s, rx, py, MAX(pw, 6), 6, 3, t->accent);
    gfx_text(s, rx, py + 10, timeb, dim, FONT_UI);
}

static void paint_white_key(piano_t *st, surface_t *s, int m, uint32_t now, int expect) {
    const theme_t *t = theme_get();
    rect_t r = key_rect(m);
    int x = r.x + 1, w = r.w - 2, y = r.y, h = r.h;
    bool pressed = user_holds(st, m) || st->mouse_midi == m;
    bool song = st->song_midi == m;
    bool wrong = st->wrong_midi == m && (int32_t)(now - st->wrong_until) < 0;
    uint32_t top = RGB(252, 252, 253), bot = RGB(226, 228, 235);
    if (wrong) { top = RGB(255, 214, 214); bot = RGB(240, 110, 110); }
    else if (pressed) { top = color_mix(COLOR_WHITE, t->accent, 120); bot = color_mix(COLOR_WHITE, t->accent, 200); }
    else if (song) { top = color_mix(COLOR_WHITE, t->accent, 60); bot = color_mix(COLOR_WHITE, t->accent, 130); }
    else if (st->hover_midi == m) { top = RGB(255, 255, 255); bot = color_mix(RGB(226, 228, 235), t->accent, 30); }
    gfx_fill_rounded_gradient_v(s, x, y - 8, w, h + 8, 5, top, bot);
    if (pressed || song || wrong) {
        gfx_gradient_v(s, x, y, w, 14, RGBA(0, 0, 0, 60), RGBA(0, 0, 0, 0));
    } else {
        gfx_fill_rect(s, x + 2, y + h - 9, w - 4, 1, RGBA(0, 0, 0, 22));
        gfx_fill_rect(s, x + 2, y + h - 8, w - 4, 1, RGBA(255, 255, 255, 200));
    }
    if (expect == m) {
        double pulse = 0.5 + 0.5 * sin(now / 140.0);
        gfx_rounded_rect(s, x + 1, y + 1, w - 2, h - 2, 5, t->accent);
        gfx_rounded_rect(s, x + 2, y + 2, w - 4, h - 4, 4, color_alpha(t->accent, 150));
        gfx_fill_circle_aa(s, x + w / 2.0, y + h - 66, 5 + 2 * pulse, color_alpha(t->accent, (int)(140 + 110 * pulse)));
    }
    int off = m - st->base;
    uint32_t lab = pressed || song ? color_mix(t->accent, COLOR_BLACK, 90) : RGB(120, 124, 136);
    if (off >= 0 && off < 18) {
        char k[2] = { (char)toupper((unsigned char)map_keys[off]), 0 };
        gfx_text_aligned(s, rect_make(x, y + h - 30, w, 18), k, expect == m ? t->accent : lab, FONT_UI_BOLD, ALIGN_CENTER);
    }
    if (m % 12 == 0) {
        char nm[8];
        midi_name(m, nm, sizeof nm);
        gfx_text_aligned(s, rect_make(x, y + h - 50, w, 18), nm, RGB(160, 164, 176), FONT_UI, ALIGN_CENTER);
    }
}

static void paint_black_key(piano_t *st, surface_t *s, int m, uint32_t now, int expect) {
    const theme_t *t = theme_get();
    rect_t r = key_rect(m);
    bool pressed = user_holds(st, m) || st->mouse_midi == m;
    bool song = st->song_midi == m;
    bool wrong = st->wrong_midi == m && (int32_t)(now - st->wrong_until) < 0;
    gfx_fill_rounded_rect(s, r.x + 2, r.y, r.w + 1, r.h + 4, 4, RGBA(0, 0, 0, 70));
    uint32_t top = RGB(62, 64, 74), bot = RGB(16, 17, 22);
    if (wrong) { top = RGB(230, 90, 90); bot = RGB(120, 24, 24); }
    else if (pressed) { top = color_mix(t->accent, COLOR_BLACK, 40); bot = color_mix(t->accent, COLOR_BLACK, 150); }
    else if (song) { top = color_mix(t->accent, COLOR_BLACK, 90); bot = color_mix(t->accent, COLOR_BLACK, 190); }
    else if (st->hover_midi == m) { top = RGB(84, 86, 98); bot = RGB(26, 27, 34); }
    gfx_fill_rounded_gradient_v(s, r.x, r.y - 8, r.w, r.h + 8, 4, top, bot);
    int lip = pressed || song ? 5 : 12;
    gfx_fill_rounded_rect(s, r.x + 2, r.y + r.h - lip - 2, r.w - 4, lip, 3, color_alpha(color_shade(top, 30), 200));
    gfx_vline(s, r.x + 2, r.y + 2, r.h - lip - 6, RGBA(255, 255, 255, 34));
    if (expect == m) {
        double pulse = 0.5 + 0.5 * sin(now / 140.0);
        gfx_rounded_rect(s, r.x - 2, r.y - 2, r.w + 4, r.h + 4, 5, t->accent);
        gfx_fill_circle_aa(s, r.x + r.w / 2.0, r.y + r.h - 44, 4 + 2 * pulse, color_alpha(t->accent, (int)(150 + 100 * pulse)));
    }
    int off = m - st->base;
    if (off >= 0 && off < 18) {
        char k[2] = { (char)toupper((unsigned char)map_keys[off]), 0 };
        gfx_text_aligned(s, rect_make(r.x, r.y + r.h - lip - 22, r.w, 18), k, expect == m ? t->accent : RGB(200, 204, 214),
                         FONT_UI_BOLD, ALIGN_CENTER);
    }
}

static void paint_keyboard(piano_t *st, surface_t *s, uint32_t now) {
    const theme_t *t = theme_get();
    gfx_shadow(s, CASE_X, CASE_Y, CASE_W, CASE_H, 12, 8, t->dark ? 90 : 60);
    gfx_fill_rounded_gradient_v(s, CASE_X, CASE_Y, CASE_W, CASE_H, 12, RGB(50, 52, 62), RGB(20, 21, 26));
    gfx_hline(s, CASE_X + 12, CASE_Y + 1, CASE_W - 24, RGBA(255, 255, 255, 46));
    int expect = 0;
    const song_t *sg = cur_song(st);
    if (st->playing && st->practice_run && sg && st->pos < sg->n) expect = sg->notes[st->pos].midi;
    for (int m = LO_MIDI; m <= HI_MIDI; m++)
        if (!is_black(m)) paint_white_key(st, s, m, now, expect);
    /* red felt strip */
    gfx_gradient_v(s, KX0 - 2, KY0 - 8, NWHITE * KW + 4, 8, RGB(160, 34, 50), RGB(96, 16, 28));
    gfx_hline(s, KX0 - 2, KY0 - 1, NWHITE * KW + 4, RGBA(0, 0, 0, 90));
    for (int m = LO_MIDI; m <= HI_MIDI; m++)
        if (is_black(m)) paint_black_key(st, s, m, now, expect);
    /* the keys the computer keyboard plays */
    rect_t a = key_rect(st->base), b = key_rect(MIN(st->base + 17, HI_MIDI));
    int zx = a.x + 2, zw = b.x + b.w - 2 - zx;
    gfx_fill_rounded_rect(s, zx, KY0 + WH + 4, zw, 4, 2, color_alpha(t->accent, 230));
}

static void paint_toolbar(piano_t *st, surface_t *s) {
    const theme_t *t = theme_get();
    uint32_t dim = sys_text_dim(t);
    gfx_text_aligned(s, rect_make(TB_TEMPO_LX, TB_Y, TB_TEMPO_LW, TB_H), "Tempo", dim, FONT_UI, ALIGN_RIGHT | ALIGN_MIDDLE);
    char b[16];
    snprintf(b, sizeof b, "%d", ui_get_value(st->tempo));
    gfx_text_aligned(s, rect_make(TB_TEMPO_X + TB_TEMPO_W + 4, TB_Y, TB_TEMPO_VW, TB_H), b, t->window_text, FONT_UI_BOLD,
                     ALIGN_LEFT | ALIGN_MIDDLE);
    gfx_text_aligned(s, rect_make(TB_KEYS_LX, TB_Y, TB_KEYS_LW, TB_H), "Keys", dim, FONT_UI, ALIGN_RIGHT | ALIGN_MIDDLE);
    midi_name(st->base, b, sizeof b);
    gfx_text_aligned(s, rect_make(TB_OCT_X + TB_OCTB_W, TB_Y, TB_OCTL_W, TB_H), b, t->window_text, FONT_UI_BOLD,
                     ALIGN_CENTER | ALIGN_MIDDLE);
}

/* bottom row: "Instrument [dropdown]" ... "Output: AC'97 - 8 notes at once" */
static void paint_footer(piano_t *st, surface_t *s) {
    (void)st;
    const theme_t *t = theme_get();
    uint32_t dim = sys_text_dim(t);
    gfx_text_aligned(s, rect_make(INS_LX, FOOT_Y, INS_LW, FOOT_H), "Instrument", dim, FONT_UI, ALIGN_LEFT | ALIGN_MIDDLE);
    char out[96];
    int poly = sound_polyphony();
    if (poly > 1) snprintf(out, sizeof out, "%s - %d notes at once", sound_device_short(), poly);
    else snprintf(out, sizeof out, "%s - one note at a time", sound_device_short());
    int ox = INS_X + INS_W + 16, ow = CASE_X + CASE_W - ox;
    int tw = font_text_width(FONT_UI, "Output:");
    int vw = font_text_width(FONT_UI_BOLD, out);
    int x0 = ox + ow - (tw + 6 + vw);
    if (x0 < ox) x0 = ox;
    gfx_text_aligned(s, rect_make(x0, FOOT_Y, tw, FOOT_H), "Output:", dim, FONT_UI, ALIGN_LEFT | ALIGN_MIDDLE);
    gfx_text_aligned(s, rect_make(x0 + tw + 6, FOOT_Y, ox + ow - (x0 + tw + 6), FOOT_H), out,
                     sound_has_card() ? t->window_text : RGB(214, 140, 40), FONT_UI_BOLD, ALIGN_LEFT | ALIGN_MIDDLE);
}

static void paint(piano_t *st, surface_t *s) {
    uint32_t now = sys_uptime_ms();
    paint_toolbar(st, s);
    paint_display(st, s, now);
    paint_keyboard(st, s, now);
    paint_footer(st, s);
}

/* ======================================================================================
 * Events
 * ==================================================================================== */
static void tick(piano_t *st) {
    uint32_t now = sys_uptime_ms();
    song_tick(st, now);
    if (st->jingle_at && (int32_t)(now - st->jingle_at) >= 0) {
        st->jingle_at = 0;
        if (!st->user_midi && !st->song_midi) {
            static const note_t tada[] = { { 523, 90 }, { 659, 90 }, { 784, 90 }, { 1047, 260 } };
            sys_play_notes(tada, ARRAY_SIZE(tada));
        }
    }
    bool snd = sys_sound_enabled();
    if (snd != st->sound_on) {            /* muted / unmuted elsewhere (taskbar, Settings) */
        st->sound_on = snd;
        if (snd) resound(st);             /* a held note sounds again */
        else forget_voices(st);
        if (st->sound) ui_invalidate(st->sound);
        inval_disp(st);
    }
    if (now - st->last_frame < FRAME_MS) return;
    bool hist = false;
    for (int i = 0; i < HIST && !hist; i++) {
        hist_t *h = &st->hist[i];
        if (h->midi && (h->t1 == 0 || now - h->t1 < ROLL_PAST_MS + 250)) hist = true;
    }
    bool msg = st->msg_until && now - st->msg_until < 200;
    if (hist || st->playing || st->recording || msg) {
        inval_disp(st);
        st->last_frame = now;
    }
    bool wrong = st->wrong_until && now - st->wrong_until < 200;
    if ((st->playing && st->practice_run) || wrong) {
        inval_keys(st);
        st->last_frame = now;
    }
}

static void piano_event(window_t *win, const event_t *ev) {
    piano_t *st = win_get_userdata(win);
    if (!st) return;
    switch (ev->type) {
    case EV_PAINT:
        paint(st, win_surface(win));
        break;
    case EV_TIMER:
        tick(st);
        break;
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_LEFT) {
            int m = key_at(ev->x, ev->y);
            if (m) {
                st->mouse_midi = m;
                press_key(st, -1, m);
            }
        }
        break;
    case EV_MOUSE_MOVE: {
        int m = key_at(ev->x, ev->y);
        if (st->mouse_midi && (ev->buttons & 1)) {
            if (m && m != st->mouse_midi) {          /* glissando */
                st->mouse_midi = m;
                press_key(st, -1, m);
            } else if (!m) {
                st->mouse_midi = 0;
                release_key(st, -1);
            }
        }
        if (m != st->hover_midi) {
            st->hover_midi = m;
            win_set_cursor(win, m ? CURSOR_HAND : CURSOR_ARROW);
            inval_keys(st);
        }
        break;
    }
    case EV_MOUSE_UP:
        if (ev->button == MOUSE_LEFT && st->mouse_midi) {
            st->mouse_midi = 0;
            release_key(st, -1);
        }
        break;
    case EV_MOUSE_LEAVE:
        if (st->hover_midi) { st->hover_midi = 0; inval_keys(st); }
        break;
    case EV_KEY_DOWN: {
        if (ev->mods & (MOD_CTRL | MOD_ALT | MOD_WIN)) break;
        int off = map_offset(ev->key);
        if (off >= 0) {
            if (!ev->repeat) press_key(st, ev->key, st->base + off);
            break;
        }
        if (ev->repeat) break;
        int k = ev->key;
        if (k == 'z' || k == 'Z') set_base(st, st->base - 12);
        else if (k == 'x' || k == 'X') set_base(st, st->base + 12);
        else if (k == KEY_SPACE) on_play(NULL, st);
        else if (k >= '1' && k <= '9') select_song(st, k - '1');
        else if (k == KEY_ESCAPE) {
            if (st->recording) rec_stop(st);
            else if (st->playing) song_stop(st, NULL);
            release_all(st);
        }
        break;
    }
    case EV_KEY_UP: {
        int off = map_offset(ev->key);
        if (off >= 0) release_key(st, ev->key);
        break;
    }
    case EV_BLUR:
        /* key-up events now go to another window: let go of everything */
        release_all(st);
        break;
    case EV_CLOSE:
        if (st->recording) st->recording = false;
        for (int i = 0; i < st->nheld; i++) note_off(&st->held[i].voice);
        note_off(&st->song_voice);
        st->nheld = 0;
        st->user_midi = st->song_midi = 0;
        free(st->rec);
        win_set_userdata(win, NULL);
        free(st);
        break;
    default:
        break;
    }
}

static void piano_launch(const char *arg) {
    songs_init();
    piano_t *st = calloc(1, sizeof *st);
    if (!st) return;
    window_t *win = win_create("Piano", W, H, 0);
    if (!win) { free(st); return; }
    st->win = win;
    st->id = win_id(win);
    st->base = 60;
    st->hist_open[0] = st->hist_open[1] = -1;
    st->sound_on = sys_sound_enabled();
    win_set_userdata(win, st);
    win_set_event_handler(win, piano_event);

    st->songs = ui_dropdown(win, TB_SONG_X, TB_Y, TB_SONG_W, TB_H, on_song, st);
    for (int i = 0; i < NSONGS; i++) ui_list_add(st->songs, g_songs[i].name);
    st->play = ui_button(win, TB_PLAY_X, TB_Y, TB_PLAY_W, TB_H, "Play", on_play, st);
    ui_button_set_default(st->play, true);
    st->rec_btn = ui_button(win, TB_REC_X, TB_Y, TB_REC_W, TB_H, "Record", on_rec, st);
    st->practice = ui_checkbox(win, TB_PRAC_X, TB_Y + 3, TB_PRAC_W, 24, "Practice", false, on_practice, st);
    st->tempo = ui_slider(win, TB_TEMPO_X, TB_Y, TB_TEMPO_W, TB_H, 40, 240, g_songs[0].bpm, on_tempo, st);
    st->oct_dn = ui_button(win, TB_OCT_X, TB_Y, TB_OCTB_W, TB_H, "-", on_oct_dn, st);
    st->oct_up = ui_button(win, TB_OCT_X + TB_OCTB_W + TB_OCTL_W, TB_Y, TB_OCTB_W, TB_H, "+", on_oct_up, st);
    st->sound = ui_custom(win, TB_SND_X, TB_Y, TB_SND_W, TB_H, sound_draw, sound_event, st);
    st->instr = ui_dropdown(win, INS_X, FOOT_Y, INS_W, FOOT_H, on_instr, st);
    for (int i = 0; i < (int)NINSTR; i++) ui_list_add(st->instr, instruments[i].name);

    /* optional argument (autorun=Piano:ARG): a song number or the first word of its name,
       plus "play" or "practice" to start right away - e.g. "2play", "jingle", "practice" */
    int song = 0;
    if (arg && *arg) {
        if (isdigit((unsigned char)arg[0])) song = atoi(arg) - 1;
        for (int i = 0; i < NSONGS; i++) {
            char word[24];
            strlcpy(word, g_songs[i].name, sizeof word);
            char *sp = strchr(word, ' ');
            if (sp) *sp = 0;
            if (strcasestr(arg, word)) { song = i; break; }
        }
    }
    /* ... and an instrument: "organ", "flute", "chip", "sine", "saw" */
    for (int i = 0; arg && i < (int)NINSTR; i++)
        if (strcasestr(arg, instruments[i].word)) st->instrument = i;
    ui_set_value(st->instr, st->instrument);
    st->base = 0;
    set_base(st, 60);
    select_song(st, CLAMP(song, 0, NSONGS - 1));
    ui_focus(st->play);
    win_set_timer(win, 10);
    if (arg && strcasestr(arg, "practi")) ui_set_value(st->practice, 1);
    if (arg && (strcasestr(arg, "play") || strcasestr(arg, "practi"))) song_start(st);
}

/* ---- icon: a little keyboard under a pair of beamed eighth notes, on a violet tile ----- */
static void piano_icon(surface_t *s, int x, int y, int size) {
    double u = size / 48.0;
    int m = (int)lround(2 * u);
    sys_tile(s, x + m, y + m, size - 2 * m, RGB(132, 108, 255), RGB(66, 44, 178));
    bool small = size < 28;
    double kx = x + 7 * u, kw = 34 * u;
    double ky = y + (small ? 15 : 24) * u, kh = (small ? 26 : 18) * u;
    int nk = small ? 4 : 5;
    /* white keys panel with a soft shadow */
    gfx_fill_rounded_rect(s, (int)lround(kx), (int)lround(ky + 1.5 * u), (int)lround(kw), (int)lround(kh), (int)lround(2.5 * u),
                          RGBA(20, 10, 60, 90));
    gfx_fill_rounded_rect(s, (int)lround(kx), (int)lround(ky), (int)lround(kw), (int)lround(kh), (int)lround(2.5 * u),
                          COLOR_WHITE);
    double ww = kw / nk;
    for (int i = 1; i < nk; i++)
        gfx_fill_rect(s, (int)lround(kx + i * ww - 0.6 * u), (int)lround(ky), MAX(1, (int)lround(1.2 * u)), (int)lround(kh),
                      RGB(150, 140, 200));
    /* black keys between white keys 1-2, 2-3 (and 4-5 on the big icon) */
    static const int bpos[3] = { 1, 2, 4 };
    for (int i = 0; i < 3; i++) {
        if (bpos[i] >= nk) continue;
        double bx = kx + bpos[i] * ww - ww * 0.32;
        gfx_fill_rounded_rect(s, (int)lround(bx), (int)lround(ky), (int)lround(ww * 0.64), (int)lround(kh * 0.6),
                              (int)lround(1.2 * u), RGB(34, 28, 62));
    }
    gfx_fill_rect(s, (int)lround(kx), (int)lround(ky), (int)lround(kw), MAX(1, (int)lround(1.6 * u)), RGB(214, 60, 84));
    if (small) return;
    /* beamed eighth notes */
    uint32_t ink = COLOR_WHITE;
    gfx_fill_ellipse(s, (int)lround(x + 15.5 * u), (int)lround(y + 17 * u), (int)lround(3.6 * u), (int)lround(2.8 * u), ink);
    gfx_fill_ellipse(s, (int)lround(x + 29.5 * u), (int)lround(y + 14.5 * u), (int)lround(3.6 * u), (int)lround(2.8 * u), ink);
    sys_stroke(s, x + 18.4 * u, y + 17 * u, x + 18.4 * u, y + 6.5 * u, 2.0 * u, ink);
    sys_stroke(s, x + 32.4 * u, y + 14.5 * u, x + 32.4 * u, y + 4.5 * u, 2.0 * u, ink);
    point_t beam[4] = {
        { (int)lround(x + 17.4 * u), (int)lround(y + 5.6 * u) }, { (int)lround(x + 33.4 * u), (int)lround(y + 3.4 * u) },
        { (int)lround(x + 33.4 * u), (int)lround(y + 7.4 * u) }, { (int)lround(x + 17.4 * u), (int)lround(y + 9.6 * u) },
    };
    gfx_fill_polygon(s, beam, 4, ink);
}

REGISTER_APP(.name = "Piano", .category = CAT_MULTIMEDIA,
             .description = "Play the piano with the mouse or keyboard, or let it play a song",
             .launch = piano_launch, .draw_icon = piano_icon);
