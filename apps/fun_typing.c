/*
 * fun_typing.c - Typing Tutor (Accessories).
 *
 * Type the text shown in the panel. Every character turns dark when it is
 * right and red (with a red underlay) when it is wrong; the caret glides from
 * letter to letter and the text scrolls a line at a time. Live statistics
 * above the text: net words per minute (correct characters / 5 per minute),
 * accuracy (every keystroke counts, also the ones fixed later with
 * Backspace), the clock (it starts with the first key and pauses while the
 * window is not focused), mistakes and the best speed for the lesson.
 *
 *   - Lessons: Home Row (words from the letters a s d f g h j k l), Words
 *     (common English words), Sentences (pangrams, proverbs, short prose),
 *     Numbers (digits and symbols). Ctrl+V practises the clipboard text, and
 *     the app opens text files passed as its argument (long texts are split
 *     into passages; Next Text moves on to the following passage);
 *   - an on-screen keyboard shows the key to press next, tinted per finger,
 *     with the Shift key on the opposite hand when a capital or symbol is
 *     needed; keys flash green or red as you type. The line under the
 *     keyboard names the finger to use and warns about Caps Lock;
 *   - at the end a result card shows the speed, accuracy, time, mistakes, raw
 *     speed, a WPM-over-time graph and the keys that went wrong most often.
 *     Best speeds per lesson are kept in ~/.fun_scores.
 *
 * Keys: type to play, Backspace fixes (Ctrl+Backspace a whole word), Esc or
 * Ctrl+R restarts the text, Ctrl+N / F2 new text, Enter on the result card
 * goes to the next text, Ctrl+1..4 picks a lesson, Ctrl+V types the
 * clipboard. Launch arguments: a file path, "home", "words", "sentences",
 * "numbers", or "text:<the text>".
 *
 * Everything runs from one window timer: 16 ms while something animates,
 * 100 ms while a text is being typed (clock, caret blink), none otherwise.
 */
#include "fun_common.h"

#define TT_W        680
#define TT_PAD      22
#define TT_HDR_Y    14
#define TT_HDR_H    30
#define TT_STATS_Y  56
#define TT_STATS_H  56
#define TT_PANEL_Y  124
#define TT_LINE_H   32
#define TT_VLINES   4
#define TT_PANEL_PX 20
#define TT_PANEL_PY 14
#define TT_PANEL_H  (2 * TT_PANEL_PY + TT_VLINES * TT_LINE_H)
#define TT_KB_W     600
#define TT_KEY_H    32
#define TT_ROW_H    36
#define TT_KB_Y     (TT_PANEL_Y + TT_PANEL_H + 18)
#define TT_KB_H     (5 * TT_ROW_H - (TT_ROW_H - TT_KEY_H))
#define TT_HINT_Y   (TT_KB_Y + TT_KB_H + 12)
#define TT_H        (TT_HINT_Y + 28)

#define TT_MAXLEN   1200            /* characters per passage                 */
#define TT_CUSTOM_MAX 32768         /* custom text kept for passages          */
#define TT_PASSAGE  320             /* custom text passage length (about)     */
#define TT_SAMPLES  300

#define FLASH_MS    260             /* key flash on the on-screen keyboard    */
#define GLIDE_MS    70              /* caret glide                            */
#define RESULT_MS   320             /* result card slides in                  */
#define GRAPH_MS    700             /* the graph draws itself                 */
#define BLINK_MS    530

#define TT_SKIP     '\x01'         /* typed[] marker: letter skipped with Space */

/* Why the last key did not move the caret (shown under the keyboard). */
enum { NOTE_NONE, NOTE_WORD_END, NOTE_DOUBLE_SPACE, NOTE_SKIPPED };

enum { LES_HOME, LES_WORDS, LES_SENT, LES_NUM, LES_CUSTOM };
static const char *const lesson_labels[4] = { "Home Row", "Words", "Sentences", "Numbers" };
static const char *const lesson_titles[5] = { "Home Row", "Words", "Sentences", "Numbers", "Custom text" };

/* ======================================================================================
 * Texts
 * ==================================================================================== */
static const char *const home_words[] = {
    "a", "as", "ad", "add", "adds", "all", "alas", "ask", "asks", "dad", "dads", "fad", "fads",
    "fall", "falls", "flag", "flags", "flash", "flask", "flasks", "gag", "gal", "gala", "galas",
    "gals", "glad", "glass", "had", "half", "hall", "halls", "has", "hash", "jag", "lad", "lads",
    "lag", "lags", "lash", "lass", "sad", "saga", "sag", "salad", "salads", "salsa", "shall",
    "slag", "slash", "dash", "gash", "ash", "sash", "aha", "alfalfa", "flak", "skald", "dahl",
};

static const char *const common_words[] = {
    "the", "of", "and", "to", "in", "is", "you", "that", "it", "he", "was", "for", "on", "are",
    "as", "with", "his", "they", "at", "be", "this", "have", "from", "or", "one", "had", "by",
    "word", "but", "not", "what", "all", "were", "we", "when", "your", "can", "said", "there",
    "use", "an", "each", "which", "she", "do", "how", "their", "if", "will", "up", "other",
    "about", "out", "many", "then", "them", "these", "so", "some", "her", "would", "make",
    "like", "him", "into", "time", "has", "look", "two", "more", "write", "go", "see", "number",
    "no", "way", "could", "people", "my", "than", "first", "water", "been", "call", "who",
    "now", "find", "long", "down", "day", "did", "get", "come", "made", "may", "part", "over",
    "new", "sound", "take", "only", "little", "work", "know", "place", "year", "live", "back",
    "give", "most", "very", "after", "thing", "our", "just", "name", "good", "sentence", "think",
    "say", "great", "where", "help", "through", "much", "before", "line", "right", "too",
    "mean", "old", "any", "same", "tell", "follow", "came", "want", "show", "also", "around",
    "form", "three", "small", "set", "put", "end", "does", "another", "well", "large", "must",
    "big", "even", "such", "because", "turn", "here", "why", "ask", "went", "read", "need",
    "land", "different", "home", "move", "try", "kind", "hand", "picture", "again", "change",
    "off", "play", "spell", "air", "away", "animal", "house", "point", "page", "letter",
    "answer", "found", "study", "still", "learn", "should", "world", "high", "every", "near",
    "add", "food", "between", "own", "below", "country", "plant", "last", "school", "keep",
    "tree", "never", "start", "city", "earth", "eye", "light", "thought", "head", "under",
    "story", "saw", "left", "few", "while", "along", "might", "close", "something", "seem",
    "next", "hard", "open", "example", "begin", "life", "always", "those", "both", "paper",
    "together", "got", "group", "often", "run", "important", "until", "side", "feet", "car",
    "mile", "night", "walk", "white", "sea", "began", "grow", "took", "river", "four", "carry",
    "state", "once", "book", "hear", "stop", "without", "second", "later", "idea", "enough",
    "eat", "face", "watch", "far", "really", "almost", "let", "above", "sometimes", "mountain",
    "cut", "young", "talk", "soon", "list", "song", "being", "leave", "family", "quick", "jump",
    "zero", "voice", "quiet", "box", "lazy", "fox", "jazz", "major", "extra", "puzzle",
};

static const char *const sentences[] = {
    "The quick brown fox jumps over the lazy dog.",
    "Pack my box with five dozen liquor jugs.",
    "How vexingly quick daft zebras jump!",
    "Sphinx of black quartz, judge my vow.",
    "Jackdaws love my big sphinx of quartz.",
    "Five quacking zephyrs jolt my wax bed.",
    "Waltz, bad nymph, for quick jigs vex.",
    "A journey of a thousand miles begins with a single step.",
    "Practice makes progress, so keep your fingers moving.",
    "The best way to predict the future is to invent it.",
    "Simple things should be simple, and complex things should be possible.",
    "Keep your eyes on the screen and let your fingers find the keys on their own.",
    "Rest your fingers lightly on the home row and return to it after every key.",
    "Slow and steady wins the race, but accuracy wins the long game.",
    "Bright stars shimmer above the quiet harbor while the boats sleep.",
    "Fresh bread, warm soup and good friends make any evening better.",
    "The old lighthouse keeper climbed the spiral stairs one last time.",
    "Learning to type well is a small investment that pays off every day.",
    "Rain drummed on the tin roof as the kettle began to whistle.",
    "Curiosity is the engine of achievement; never stop asking why.",
    "Good habits formed early make every later task a little easier.",
    "Measure twice and cut once, because wood is cheaper than time.",
    "The museum opened a new wing filled with ancient maps and globes.",
    "Our small hobby operating system draws every pixel on its own.",
    "Typing without looking at the keyboard feels strange at first, then natural.",
    "When in doubt, take a deep breath, relax your shoulders and keep going.",
    "A gentle breeze carried the smell of pine needles across the valley.",
    "The library was so quiet that you could hear the pages turn.",
    "Every expert was once a beginner who refused to give up.",
    "Autumn leaves drifted down and covered the path in red and gold.",
    "Water the plants on Monday, and remember to feed the goldfish.",
    "The train rolled through sleepy villages long after midnight.",
    "Clear code is written for people first and for computers second.",
    "She opened the window and watched the city wake up below.",
    "Small steps every day add up to big changes over a year.",
    "The chef tasted the sauce, frowned, and reached for more pepper.",
};

static const char *const number_lines[] = {
    "Order #4721 ships on 03/15 for $129.99 (tax included).",
    "Meet me at 7:45 p.m. in room 2B; bring 3 copies.",
    "The recipe needs 2 cups of flour, 1/2 cup of sugar & 3 eggs.",
    "Call 555-0198 or email help@example.com before 5 o'clock.",
    "In 1969, Apollo 11 landed on the Moon at 20:17 UTC.",
    "Set x = (a + b) * 2; then print x % 7 and y ^ 3.",
    "The score was 21-17 after 4 quarters and 2 overtimes!",
    "Save 25% on orders over $50 with the code NOVA-2026.",
    "Train 9 departs from platform 12 at 06:30 [15 min late].",
    "Pi is about 3.14159, and e is roughly 2.71828.",
    "The file is 4,096 bytes; the disk holds 64 MB, or 65,536 KB.",
    "Mix 3 parts water with 1 part juice: 750 ml + 250 ml = 1 l.",
    "if (count > 10 && flag != 0) { total += count; }",
    "Version 1.0.3 fixed 17 bugs and added 5 features <finally>.",
    "Room 404 is on floor 4, between rooms 402 and 406.",
    "The temperature fell from 18 to -3 degrees overnight.",
    "Use \"git log -n 5\" to see the last 5 commits.",
    "Invoice 2026-118: 12 x $7.50 = $90.00, due in 30 days.",
    "Her new password is 8 characters long: two digits, one ~ and one ?.",
    "The marathon is 42.195 km long, about 26.2 miles.",
};

/* ======================================================================================
 * On-screen keyboard (US layout). Widths in quarter keys; every row is 60 quarters.
 * ==================================================================================== */
enum { KS_NONE, KS_BACK, KS_TAB, KS_CAPS, KS_ENTER, KS_LSHIFT, KS_RSHIFT, KS_CTRL, KS_WIN, KS_ALT,
       KS_MENU, KS_SPACE };

typedef struct {
    char lo, hi;          /* unshifted / shifted character */
    uint8_t q;            /* width in quarter keys         */
    uint8_t finger;       /* 0-3 left little..index, 4-7 right index..little, 8 thumb */
    uint8_t special;      /* KS_*                          */
} tt_key_t;

#define K(l, h, f)     { l, h, 4, f, KS_NONE }
#define S(sp, q, f)    { 0, 0, q, f, sp }
static const tt_key_t kb_keys[] = {
    /* row 0 */
    K('`', '~', 0), K('1', '!', 0), K('2', '@', 1), K('3', '#', 2), K('4', '$', 3), K('5', '%', 3),
    K('6', '^', 4), K('7', '&', 4), K('8', '*', 5), K('9', '(', 6), K('0', ')', 7), K('-', '_', 7),
    K('=', '+', 7), S(KS_BACK, 8, 7),
    /* row 1 */
    S(KS_TAB, 6, 0), K('q', 'Q', 0), K('w', 'W', 1), K('e', 'E', 2), K('r', 'R', 3), K('t', 'T', 3),
    K('y', 'Y', 4), K('u', 'U', 4), K('i', 'I', 5), K('o', 'O', 6), K('p', 'P', 7), K('[', '{', 7),
    K(']', '}', 7), { '\\', '|', 6, 7, KS_NONE },
    /* row 2 */
    S(KS_CAPS, 7, 0), K('a', 'A', 0), K('s', 'S', 1), K('d', 'D', 2), K('f', 'F', 3), K('g', 'G', 3),
    K('h', 'H', 4), K('j', 'J', 4), K('k', 'K', 5), K('l', 'L', 6), K(';', ':', 7), K('\'', '"', 7),
    S(KS_ENTER, 9, 7),
    /* row 3 */
    S(KS_LSHIFT, 9, 0), K('z', 'Z', 0), K('x', 'X', 1), K('c', 'C', 2), K('v', 'V', 3), K('b', 'B', 3),
    K('n', 'N', 4), K('m', 'M', 4), K(',', '<', 5), K('.', '>', 6), K('/', '?', 7), S(KS_RSHIFT, 11, 7),
    /* row 4 */
    S(KS_CTRL, 5, 0), S(KS_WIN, 5, 0), S(KS_ALT, 5, 8), S(KS_SPACE, 25, 8), S(KS_ALT, 5, 8),
    S(KS_WIN, 5, 7), S(KS_MENU, 5, 7), S(KS_CTRL, 5, 7),
};
#undef K
#undef S
#define KB_NKEYS ARRAY_SIZE(kb_keys)
static const uint8_t kb_row_len[5] = { 14, 14, 13, 12, 8 };

static const char *const special_labels[] = {
    "", "\x91 Back", "Tab", "Caps", "Enter", "Shift", "Shift", "Ctrl", "Win", "Alt", "Fn", "",
};

static const char *const finger_names[9] = {
    "left little finger", "left ring finger", "left middle finger", "left index finger",
    "right index finger", "right middle finger", "right ring finger", "right little finger",
    "thumb",
};

static const uint32_t finger_cols[9] = {
    RGB(239, 83, 80), RGB(255, 152, 0), RGB(253, 200, 40), RGB(102, 187, 106),
    RGB(38, 166, 234), RGB(92, 107, 192), RGB(171, 71, 188), RGB(236, 64, 122),
    RGB(140, 150, 165),
};

/* Key rectangles relative to the keyboard's top-left corner, computed once. */
static rect_t kb_rects[ARRAY_SIZE(kb_keys)];
static int kb_lshift = -1, kb_rshift = -1;
static bool kb_ready;

static void kb_init(void) {
    if (kb_ready) return;
    int i = 0;
    for (int row = 0; row < 5; row++) {
        int q = 0;
        for (int k = 0; k < kb_row_len[row]; k++, i++) {
            int x0 = q * TT_KB_W / 60, x1 = (q + kb_keys[i].q) * TT_KB_W / 60;
            kb_rects[i] = rect_make(x0 + 2, row * TT_ROW_H, x1 - x0 - 4, TT_KEY_H);
            q += kb_keys[i].q;
            if (kb_keys[i].special == KS_LSHIFT) kb_lshift = i;
            if (kb_keys[i].special == KS_RSHIFT) kb_rshift = i;
        }
    }
    kb_ready = true;
}

/* The key producing character c (and whether Shift is needed), or -1. */
static int kb_find(char c, bool *shift) {
    *shift = false;
    if (c == ' ') {
        for (int i = 0; i < KB_NKEYS; i++)
            if (kb_keys[i].special == KS_SPACE) return i;
        return -1;
    }
    for (int i = 0; i < KB_NKEYS; i++) {
        if (kb_keys[i].special) continue;
        if (kb_keys[i].lo == c) return i;
        if (kb_keys[i].hi == c) { *shift = true; return i; }
    }
    return -1;
}

/* ======================================================================================
 * State
 * ==================================================================================== */
typedef struct {
    window_t *win;
    fn_rng_t rng;
    int lesson;
    char target[TT_MAXLEN + 1];
    int len;
    char typed[TT_MAXLEN];
    int ntyped;
    int16_t cx[TT_MAXLEN + 1], cl[TT_MAXLEN + 1];   /* layout: x in the text area, line */
    int nlines;

    bool started, paused, finished;
    uint32_t seg_start, acc_ms, end_ms, finish_t0, last_input;
    int keystrokes, kerrors;
    uint16_t miss[128];               /* mistakes per expected character */
    uint16_t samples[TT_SAMPLES];
    int nsamples;

    int best;                         /* best WPM of this lesson (0 = none) */
    bool record, first_result;
    int final_wpm, final_raw, final_acc;
    char tricky[64];

    int flash_key;
    bool flash_ok;
    uint32_t flash_t0;
    int note;                         /* NOTE_*                               */
    uint32_t reject_t0;               /* a key was rejected: red flash at the caret */
    double caret_fx, caret_fy;        /* glide start (text-area coordinates) */
    uint32_t caret_t0;
    double scroll, scroll_to;
    bool blink_on;
    int shown_sec, shown_wpm;

    int timer, timer_iv;
    fn_btn_t btn[4];                  /* Restart, New Text, Try Again, Next Text */
    rect_t seg_r, panel_r, kb_r;
    int seg_hover;

    char *custom;                     /* custom text (clipboard / file) */
    int custom_len, custom_pos, custom_next;
    char custom_name[48];
    int prev_pick;
} tt_t;

static uint32_t tt_elapsed(tt_t *g) {
    if (!g->started) return 0;
    if (g->finished) return g->end_ms;
    return g->acc_ms + (g->paused ? 0 : sys_uptime_ms() - g->seg_start);
}

static int tt_correct(tt_t *g) {
    int n = 0;
    for (int i = 0; i < g->ntyped; i++) n += g->typed[i] == g->target[i];
    return n;
}

static int tt_wpm_at(tt_t *g, uint32_t ms) {
    if (ms < 1000) return -1;
    return (int)(tt_correct(g) * 12000.0 / ms + 0.5);
}

static int tt_accuracy(tt_t *g) {
    if (!g->keystrokes) return -1;
    return (g->keystrokes - g->kerrors) * 100 / g->keystrokes;
}

/* ---- invalidation -------------------------------------------------------------------- */
static void tt_inval_panel(tt_t *g) {
    win_invalidate_rect(g->win, g->panel_r.x - 2, g->panel_r.y - 2, g->panel_r.w + 4, g->panel_r.h + 6);
}
static void tt_inval_kb(tt_t *g) {
    win_invalidate_rect(g->win, g->kb_r.x - 12, g->kb_r.y - 12, g->kb_r.w + 24, g->kb_r.h + 24);
}
static void tt_inval_stats(tt_t *g) { win_invalidate_rect(g->win, 0, TT_STATS_Y - 2, TT_W, TT_STATS_H + 6); }
static void tt_inval_hint(tt_t *g) { win_invalidate_rect(g->win, 0, TT_HINT_Y - 2, TT_W, 30); }

/* ---- timer ------------------------------------------------------------------------- */
static bool tt_animating(tt_t *g, uint32_t now) {
    if (g->flash_key >= 0 && now - g->flash_t0 < FLASH_MS + 20) return true;
    if (now - g->reject_t0 < FLASH_MS + 20) return true;
    if (now - g->caret_t0 < GLIDE_MS + 20) return true;
    if (fabs(g->scroll - g->scroll_to) > 0.3) return true;
    if (g->finished && now - g->finish_t0 < RESULT_MS + GRAPH_MS + 40) return true;
    return false;
}

static void tt_update_timer(tt_t *g) {
    uint32_t now = sys_uptime_ms();
    int iv = tt_animating(g, now) ? 16 : (!g->finished && win_is_focused(g->win) ? 100 : 0);
    if (iv == g->timer_iv) return;
    if (g->timer > 0) win_kill_timer(g->win, g->timer);
    g->timer = iv ? win_set_timer(g->win, (uint32_t)iv) : 0;
    g->timer_iv = iv;
}

/* ======================================================================================
 * Building texts
 * ==================================================================================== */
static void tt_append(tt_t *g, const char *s) {
    while (*s && g->len < TT_MAXLEN) g->target[g->len++] = *s++;
    g->target[g->len] = 0;
}

/* Keep printable ASCII, turn white space into single spaces. Returns the new length. */
static int tt_sanitize(char *d, const char *s, int max) {
    int n = 0;
    bool space = false;
    for (; *s && n < max; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\t' || c == '\n' || c == '\r' || c == ' ') { space = n > 0; continue; }
        if (c < 32 || c > 126) continue;
        if (space && n < max - 1) d[n++] = ' ';
        space = false;
        d[n++] = (char)c;
    }
    d[n] = 0;
    return n;
}

static void tt_pick_words(tt_t *g, const char *const *list, int count, int nwords) {
    int picked[32], n = 0;
    nwords = MIN(nwords, 32);
    for (int i = 0; i < nwords; i++) {
        int k, tries = 0;
        bool dup;
        do {   /* no word twice in one text (as far as the list allows) */
            k = fn_rand_n(&g->rng, count);
            dup = false;
            for (int j = 0; j < n; j++) dup |= picked[j] == k;
        } while (dup && ++tries < 40);
        picked[n++] = k;
        if (i) tt_append(g, " ");
        tt_append(g, list[k]);
    }
}

static void tt_pick_lines(tt_t *g, const char *const *list, int count, int minlen, int maxn) {
    int used[8], nused = 0;
    while (nused < maxn && (g->len < minlen || nused == 0)) {
        int k, tries = 0;
        bool dup;
        do {
            k = fn_rand_n(&g->rng, count);
            dup = (nused == 0 && k == g->prev_pick);
            for (int j = 0; j < nused; j++) dup |= used[j] == k;
        } while (dup && ++tries < 50);
        if (nused == 0) g->prev_pick = k;
        used[nused++] = k;
        if (g->len) tt_append(g, " ");
        tt_append(g, list[k]);
    }
}

/* Next passage of the custom text, starting at custom_pos (ends at a word boundary). */
static void tt_custom_passage(tt_t *g) {
    if (!g->custom || g->custom_len == 0) return;
    if (g->custom_pos >= g->custom_len) g->custom_pos = 0;
    int start = g->custom_pos, end = g->custom_len;
    if (end - start > TT_PASSAGE + 80) {
        /* prefer the end of a sentence after TT_PASSAGE characters, else a space */
        int cut = -1;
        for (int i = start + TT_PASSAGE - 120; i < start + TT_PASSAGE + 80 && i < end; i++) {
            char c = g->custom[i];
            if ((c == '.' || c == '!' || c == '?') && g->custom[i + 1] == ' ') { cut = i + 1; if (i >= start + TT_PASSAGE) break; }
        }
        if (cut < 0) {
            cut = start + TT_PASSAGE;
            while (cut > start + 1 && g->custom[cut] != ' ') cut--;
        }
        end = cut;
    }
    int n = MIN(end - start, TT_MAXLEN);
    memcpy(g->target, g->custom + start, (size_t)n);
    g->len = n;
    while (g->len > 0 && g->target[g->len - 1] == ' ') g->len--;
    g->target[g->len] = 0;
    g->custom_next = start + n;
    while (g->custom_next < g->custom_len && g->custom[g->custom_next] == ' ') g->custom_next++;
}

static void tt_layout(tt_t *g) {
    const font_t *f = FONT_UI_LARGE;
    int maxw = g->panel_r.w - 2 * TT_PANEL_PX;
    int x = 0, line = 0, i = 0;
    while (i < g->len) {
        if (g->target[i] == ' ') {
            g->cx[i] = (int16_t)x;
            g->cl[i] = (int16_t)line;
            x += font_char_width(f, ' ');
            i++;
            continue;
        }
        int j = i, ww = 0;
        while (j < g->len && g->target[j] != ' ') ww += font_char_width(f, (unsigned char)g->target[j++]);
        if (x > 0 && x + ww > maxw) { x = 0; line++; }
        for (int k = i; k < j; k++) {
            int cw = font_char_width(f, (unsigned char)g->target[k]);
            if (x > 0 && x + cw > maxw) { x = 0; line++; }    /* a word longer than a line */
            g->cx[k] = (int16_t)x;
            g->cl[k] = (int16_t)line;
            x += cw;
        }
        i = j;
    }
    g->cx[g->len] = (int16_t)x;
    g->cl[g->len] = (int16_t)line;
    g->nlines = line + 1;
}

static void tt_scroll_target(tt_t *g) {
    int line = g->cl[MIN(g->ntyped, g->len)];
    int top = CLAMP(line - 1, 0, MAX(0, g->nlines - TT_VLINES));
    g->scroll_to = top * TT_LINE_H;
}

/* Start the current target text from scratch. */
static void tt_reset(tt_t *g) {
    g->ntyped = 0;
    g->started = g->paused = g->finished = false;
    g->acc_ms = g->end_ms = 0;
    g->keystrokes = g->kerrors = 0;
    memset(g->miss, 0, sizeof g->miss);
    g->nsamples = 0;
    g->record = g->first_result = false;
    g->flash_key = -1;
    g->note = NOTE_NONE;
    g->reject_t0 = sys_uptime_ms() - 10000;
    g->caret_t0 = 0;
    g->caret_fx = g->caret_fy = 0;
    g->scroll = g->scroll_to = 0;
    g->last_input = sys_uptime_ms();
    g->shown_sec = g->shown_wpm = -2;
    g->btn[2].hidden = g->btn[3].hidden = true;
    fn_btns_reset(g->win, g->btn, 4);
    tt_layout(g);
    win_invalidate(g->win);
    tt_update_timer(g);
}

static void tt_load_best(tt_t *g) {
    char key[32];
    snprintf(key, sizeof key, "typing_best_%d", g->lesson);
    g->best = fn_score_get(key, 0);
}

static void tt_new_text(tt_t *g) {
    g->len = 0;
    g->target[0] = 0;
    switch (g->lesson) {
    case LES_HOME:  tt_pick_words(g, home_words, ARRAY_SIZE(home_words), 16); break;
    case LES_WORDS: tt_pick_words(g, common_words, ARRAY_SIZE(common_words), 24); break;
    case LES_SENT:  tt_pick_lines(g, sentences, ARRAY_SIZE(sentences), 130, 3); break;
    case LES_NUM:   tt_pick_lines(g, number_lines, ARRAY_SIZE(number_lines), 90, 2); break;
    case LES_CUSTOM:
        if (g->custom_next >= g->custom_len) g->custom_next = 0;
        g->custom_pos = g->custom_next;
        tt_custom_passage(g);
        break;
    }
    if (g->len == 0) tt_append(g, sentences[0]);
    tt_load_best(g);
    tt_reset(g);
}

static void tt_set_lesson(tt_t *g, int lesson) {
    if (lesson == LES_CUSTOM && !g->custom) return;
    g->lesson = lesson;
    if (lesson != LES_CUSTOM) fn_score_set("typing_lesson", lesson);
    tt_new_text(g);
}

/* Install a custom text (clipboard, file, argument). Returns false if nothing typeable. */
static bool tt_set_custom(tt_t *g, const char *text, const char *name) {
    if (!text) return false;
    int max = MIN((int)strlen(text), TT_CUSTOM_MAX);
    char *buf = malloc((size_t)max + 2);
    if (!buf) return false;
    char *tmp = malloc((size_t)max + 1);
    if (!tmp) { free(buf); return false; }
    memcpy(tmp, text, (size_t)max);
    tmp[max] = 0;
    int n = tt_sanitize(buf, tmp, max);
    free(tmp);
    if (n == 0) { free(buf); return false; }
    free(g->custom);
    g->custom = buf;
    g->custom_len = n;
    g->custom_pos = g->custom_next = 0;
    strlcpy(g->custom_name, name ? name : "Custom text", sizeof g->custom_name);
    g->lesson = LES_CUSTOM;
    tt_new_text(g);
    return true;
}

/* ======================================================================================
 * Typing
 * ==================================================================================== */
static void tt_caret_pos(tt_t *g, double *x, double *y, uint32_t now) {
    int i = MIN(g->ntyped, g->len);
    double tx = g->cx[i], ty = g->cl[i] * TT_LINE_H;
    uint32_t el = now - g->caret_t0;
    if (el < GLIDE_MS) {
        double p = fn_ease_out((double)el / GLIDE_MS);
        tx = g->caret_fx + (tx - g->caret_fx) * p;
        ty = g->caret_fy + (ty - g->caret_fy) * p;
    }
    *x = tx;
    *y = ty;
}

/* Call before ntyped changes: the caret glides from where it is now. */
static void tt_caret_moving(tt_t *g) {
    uint32_t now = sys_uptime_ms();
    tt_caret_pos(g, &g->caret_fx, &g->caret_fy, now);
    g->caret_t0 = now;
    g->last_input = now;
}

static void tt_build_tricky(tt_t *g) {
    g->tricky[0] = 0;
    uint16_t cnt[128];
    memcpy(cnt, g->miss, sizeof cnt);
    int n = 0, len = 0;
    for (int k = 0; k < 3; k++) {
        int best = -1;
        for (int c = 32; c < 127; c++)
            if (cnt[c] && (best < 0 || cnt[c] > cnt[best])) best = c;
        if (best < 0) break;
        cnt[best] = 0;
        char name[8];
        if (best == ' ') strlcpy(name, "Space", sizeof name);
        else snprintf(name, sizeof name, "%c", best >= 'a' && best <= 'z' ? best - 32 : best);
        len += snprintf(g->tricky + len, sizeof g->tricky - (size_t)len, "%s%s", n ? "   " : "", name);
        n++;
    }
}

static void tt_finish(tt_t *g) {
    uint32_t now = sys_uptime_ms();
    g->end_ms = g->acc_ms + (g->paused ? 0 : now - g->seg_start);
    if (g->end_ms < 1) g->end_ms = 1;
    g->finished = true;
    g->finish_t0 = now;
    double ms = g->end_ms;
    g->final_wpm = (int)(tt_correct(g) * 12000.0 / ms + 0.5);
    g->final_raw = (int)(g->ntyped * 12000.0 / ms + 0.5);
    g->final_acc = tt_accuracy(g);
    if (g->nsamples < TT_SAMPLES) g->samples[g->nsamples++] = (uint16_t)MIN(g->final_wpm, 999);
    tt_build_tricky(g);
    if (g->final_wpm > g->best) {
        g->record = g->best > 0;
        g->first_result = g->best == 0;
        g->best = g->final_wpm;
        char key[32];
        snprintf(key, sizeof key, "typing_best_%d", g->lesson);
        fn_score_set(key, g->best);
    }
    g->btn[2].hidden = g->btn[3].hidden = false;
    static const note_t tune[] = { { 659, 80 }, { 784, 80 }, { 1047, 160 } };
    static const note_t fanfare[] = { { 523, 80 }, { 659, 80 }, { 784, 80 }, { 1047, 120 }, { 0, 40 }, { 1319, 220 } };
    if (g->record) sys_play_notes(fanfare, ARRAY_SIZE(fanfare));
    else sys_play_notes(tune, ARRAY_SIZE(tune));
    win_invalidate(g->win);
}

static void tt_count_miss(tt_t *g, char want) {
    g->kerrors++;
    if ((unsigned char)want < 128 && g->miss[(unsigned char)want] < 60000) g->miss[(unsigned char)want]++;
    sys_beep(165, 18);
}

/*
 * One typed character. Besides plain right / wrong there are two word rules, so a
 * single slip does not turn the rest of the text red:
 *   - a letter where the word has ended (Space expected) is rejected: it counts as
 *     a mistake, but the caret stays put;
 *   - Space in the middle of a word skips the rest of the word (those letters count
 *     as missed) and continues with the next word. Space at the start of a word
 *     (a double space) is rejected like an extra letter.
 */
static void tt_type_char(tt_t *g, char c) {
    if (g->finished || g->ntyped >= g->len) return;
    uint32_t now = sys_uptime_ms();
    if (!g->started) {
        g->started = true;
        g->seg_start = now;
        g->acc_ms = 0;
    } else if (g->paused) {
        g->paused = false;
        g->seg_start = now;
    }
    char want = g->target[g->ntyped];
    bool ok = c == want, advance = true;
    int skip_to = -1;
    g->keystrokes++;
    g->note = NOTE_NONE;
    if (!ok && want == ' ') {
        advance = false;
        g->note = NOTE_WORD_END;
    } else if (!ok && c == ' ') {
        if (g->ntyped == 0 || g->target[g->ntyped - 1] == ' ') {
            advance = false;
            g->note = NOTE_DOUBLE_SPACE;
        } else {
            for (int k = g->ntyped + 1; k < g->len; k++)
                if (g->target[k] == ' ') { skip_to = k; break; }
            if (skip_to >= 0) g->note = NOTE_SKIPPED;
        }
    }
    if (!ok) tt_count_miss(g, want);
    if (advance) {
        tt_caret_moving(g);
        if (skip_to >= 0) {
            while (g->ntyped < skip_to) g->typed[g->ntyped++] = TT_SKIP;
            g->typed[g->ntyped++] = ' ';
        } else {
            g->typed[g->ntyped++] = c;
        }
    } else {
        g->reject_t0 = now;
        g->last_input = now;
    }
    bool shift;
    g->flash_key = kb_find(c, &shift);
    g->flash_ok = ok;
    g->flash_t0 = now;
    tt_scroll_target(g);
    tt_inval_panel(g);
    tt_inval_kb(g);
    tt_inval_stats(g);
    tt_inval_hint(g);
    if (g->ntyped >= g->len) tt_finish(g);
    tt_update_timer(g);
}

static void tt_backspace(tt_t *g, bool word) {
    if (g->finished || g->ntyped == 0) return;
    tt_caret_moving(g);
    if (word) {
        while (g->ntyped > 0 && g->target[g->ntyped - 1] == ' ') g->ntyped--;
        while (g->ntyped > 0 && g->target[g->ntyped - 1] != ' ') g->ntyped--;
    } else {
        g->ntyped--;
        /* a word skipped with one Space comes back with one Backspace */
        while (g->ntyped > 0 && g->typed[g->ntyped - 1] == TT_SKIP) g->ntyped--;
    }
    g->note = NOTE_NONE;
    for (int i = 0; i < KB_NKEYS; i++)
        if (kb_keys[i].special == KS_BACK) g->flash_key = i;
    g->flash_ok = true;
    g->flash_t0 = sys_uptime_ms();
    tt_scroll_target(g);
    tt_inval_panel(g);
    tt_inval_kb(g);
    tt_inval_stats(g);
    tt_inval_hint(g);
    tt_update_timer(g);
}

static void tt_paste(tt_t *g) {
    const char *clip = clipboard_get();
    if (!tt_set_custom(g, clip, "Clipboard text")) {
        ui_notify("Typing Tutor", "The clipboard has no text to practise.");
        return;
    }
}

/* ======================================================================================
 * Painting
 * ==================================================================================== */
static uint32_t tt_red(void) { return theme_get()->dark ? RGB(255, 107, 99) : RGB(214, 48, 49); }
static uint32_t tt_green(void) { return theme_get()->dark ? RGB(92, 200, 110) : RGB(46, 160, 67); }

static void tt_card(surface_t *s, rect_t r, int rad) {
    const theme_t *t = theme_get();
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, rad, t->window_border);
    gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, rad - 1, fn_card_bg());
}

static void tt_paint_stats(tt_t *g, surface_t *s) {
    const theme_t *t = theme_get();
    int n = 5, gap = 10, cw = (TT_W - 2 * TT_PAD - (n - 1) * gap) / n;
    char buf[32];
    uint32_t el = tt_elapsed(g);
    bool running = g->started && !g->finished;
    rect_t r = rect_make(TT_PAD, TT_STATS_Y, cw, TT_STATS_H);

    int wpm = g->finished ? g->final_wpm : tt_wpm_at(g, el);
    if (wpm >= 0) snprintf(buf, sizeof buf, "%d", wpm);
    else strlcpy(buf, "-", sizeof buf);
    fn_stat_card(s, r, "WPM", buf, running ? t->accent : t->window_text, running);
    r.x += cw + gap;

    int acc = tt_accuracy(g);
    if (acc >= 0) snprintf(buf, sizeof buf, "%d%%", acc);
    else strlcpy(buf, "-", sizeof buf);
    uint32_t acol = acc < 0 ? t->window_text : acc >= 95 ? tt_green() : acc >= 85 ? t->window_text : tt_red();
    fn_stat_card(s, r, "Accuracy", buf, acol, false);
    r.x += cw + gap;

    fn_fmt_time(buf, sizeof buf, el);
    fn_stat_card(s, r, g->paused ? "Time (paused)" : "Time", buf, t->window_text, false);
    r.x += cw + gap;

    snprintf(buf, sizeof buf, "%d", g->kerrors);
    fn_stat_card(s, r, "Mistakes", buf, g->kerrors ? tt_red() : t->window_text, false);
    r.x += cw + gap;
    r.w = TT_W - TT_PAD - r.x;

    if (g->best) snprintf(buf, sizeof buf, "%d WPM", g->best);
    else strlcpy(buf, "-", sizeof buf);
    fn_stat_card(s, r, "Best", buf, fn_muted(), false);
}

static void tt_paint_text(tt_t *g, surface_t *s, uint32_t now) {
    const theme_t *t = theme_get();
    const font_t *f = FONT_UI_LARGE;
    rect_t p = g->panel_r;
    tt_card(s, p, 12);

    /* text area: lines scroll under the panel's top and bottom padding */
    rect_t saved = s->clip;
    s->clip = rect_intersect(saved, rect_make(p.x + 1, p.y + 10, p.w - 2, p.h - 20));

    uint32_t ok_col = t->window_text;
    uint32_t todo_col = t->dark ? color_mix(t->window_text, fn_card_bg(), 130) : color_mix(t->window_text, fn_card_bg(), 150);
    uint32_t err_col = tt_red();
    uint32_t err_bg = color_alpha(tt_red(), t->dark ? 60 : 42);
    int ox = p.x + TT_PANEL_PX, oy = p.y + TT_PANEL_PY - (int)lround(g->scroll);
    int fh = font_height(f), ty = (TT_LINE_H - fh) / 2;
    int first = (int)(g->scroll / TT_LINE_H) - 1, last = first + TT_VLINES + 2;

    /* mistakes: one red underlay per run of wrong characters on a line */
    for (int i = 0; i < g->ntyped; i++) {
        if (g->typed[i] == g->target[i]) continue;
        int j = i;
        while (j + 1 < g->ntyped && g->typed[j + 1] != g->target[j + 1] && g->cl[j + 1] == g->cl[i]) j++;
        int ln = g->cl[i];
        if (ln >= first && ln <= last) {
            int x0 = ox + g->cx[i], y = oy + ln * TT_LINE_H;
            int x1 = ox + g->cx[j] + MAX(font_char_width(f, (unsigned char)g->target[j]), 7);
            gfx_fill_rounded_rect(s, x0 - 2, y + 3, x1 - x0 + 4, TT_LINE_H - 6, 5, err_bg);
            gfx_fill_rect(s, x0, y + TT_LINE_H - 6, x1 - x0, 2, err_col);
        }
        i = j;
    }
    for (int i = 0; i < g->len; i++) {
        int ln = g->cl[i];
        if (ln < first || ln > last) continue;
        char c = g->target[i];
        if (c == ' ') continue;
        uint32_t col = i >= g->ntyped ? todo_col : g->typed[i] == c ? ok_col : err_col;
        gfx_char(s, ox + g->cx[i], oy + ln * TT_LINE_H + ty, (unsigned char)c, col, f);
    }

    /* caret (with a red flash behind it when a key was rejected) */
    bool focused = win_is_focused(g->win);
    if (!g->finished && focused) {
        double cxp, cyp;
        tt_caret_pos(g, &cxp, &cyp, now);
        uint32_t rj = now - g->reject_t0;
        if (rj < FLASH_MS) {
            int a = (int)((t->dark ? 150 : 120) * (1.0 - (double)rj / FLASH_MS));
            int x = ox + (int)lround(cxp), y = oy + (int)lround(cyp);
            int w = MAX(font_char_width(f, (unsigned char)g->target[MIN(g->ntyped, g->len - 1)]), 10);
            gfx_fill_rounded_rect(s, x - 3, y + 3, w + 6, TT_LINE_H - 6, 5, color_alpha(err_col, a));
        }
        uint32_t since = now - g->last_input;
        bool on = since < BLINK_MS || ((since / BLINK_MS) & 1) == 0;
        if (on || now - g->caret_t0 < GLIDE_MS) {
            int x = ox + (int)lround(cxp) - 1, y = oy + (int)lround(cyp) + 4;
            gfx_fill_rounded_rect(s, x, y, 3, TT_LINE_H - 8, 1, t->accent);
        }
    }
    s->clip = saved;

    /* progress along the bottom edge */
    int pw = p.w - 2 * TT_PANEL_PX;
    int px = p.x + TT_PANEL_PX, py = p.y + p.h - 7;
    gfx_fill_rounded_rect(s, px, py, pw, 3, 1, color_alpha(t->window_text, t->dark ? 30 : 18));
    int done = g->len ? pw * MIN(g->ntyped, g->len) / g->len : 0;
    if (done > 0) gfx_fill_rounded_rect(s, px, py, MAX(done, 3), 3, 1, t->accent);

    /* not focused: pause veil */
    if (!focused && !g->finished) {
        gfx_fill_rounded_rect(s, p.x + 1, p.y + 1, p.w - 2, p.h - 2, 11, color_alpha(fn_card_bg(), 225));
        const char *l1 = g->started ? "Paused" : "Ready when you are";
        const char *l2 = g->started ? "Click here to continue - the clock is stopped" : "Click here, then start typing";
        fn_text_c(s, p.x, p.y + p.h / 2 - 26, p.w, l1, t->window_text, FONT_UI_LARGE);
        fn_text_c(s, p.x, p.y + p.h / 2 + 2, p.w, l2, fn_muted(), FONT_UI);
    }
}

static void tt_paint_kb(tt_t *g, surface_t *s, uint32_t now) {
    const theme_t *t = theme_get();
    rect_t kr = g->kb_r;
    uint32_t plate = t->dark ? color_mix(t->window_bg, COLOR_BLACK, 70) : color_mix(t->window_bg, t->window_border, 90);
    gfx_fill_rounded_rect(s, kr.x - 10, kr.y - 10, kr.w + 20, kr.h + 20, 14, plate);

    int next = -1, shift_key = -1;
    bool shift = false;
    if (!g->finished && g->ntyped < g->len) {
        next = kb_find(g->target[g->ntyped], &shift);
        if (next >= 0 && shift) shift_key = kb_keys[next].finger <= 3 ? kb_rshift : kb_lshift;
    }
    bool caps = (input_mods() & MOD_CAPS) != 0;
    uint32_t key_bg = t->dark ? color_shade(t->button_bg, 6) : t->button_bg;

    for (int i = 0; i < KB_NKEYS; i++) {
        const tt_key_t *k = &kb_keys[i];
        rect_t r = kb_rects[i];
        r.x += kr.x;
        r.y += kr.y;
        bool hl = i == next || i == shift_key;
        int fl = -1;
        if (i == g->flash_key && now - g->flash_t0 < FLASH_MS) fl = (int)(now - g->flash_t0);
        if (fl >= 0 && fl < 90) r.y += 1;   /* pressed */

        bool neutral = k->special == KS_CTRL || k->special == KS_WIN || k->special == KS_ALT || k->special == KS_MENU;
        uint32_t fc = finger_cols[k->finger];
        uint32_t bg = neutral ? color_mix(key_bg, t->window_border, 60) : color_mix(key_bg, fc, t->dark ? 52 : 40);
        uint32_t label = t->window_text;

        gfx_fill_rounded_rect(s, r.x, r.y + 2, r.w, r.h, 6, RGBA(0, 0, 0, t->dark ? 110 : 40));
        if (hl) {
            gfx_fill_rounded_rect(s, r.x - 3, r.y - 3, r.w + 6, r.h + 6, 9, color_alpha(t->accent, 70));
            gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, color_shade(t->accent, -30));
            gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 5, t->accent);
            label = t->accent_text;
        } else {
            gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, color_mix(bg, COLOR_BLACK, t->dark ? 60 : 30));
            gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 5, bg);
            /* finger stripe along the bottom */
            if (!neutral) gfx_fill_rect(s, r.x + 5, r.y + r.h - 4, r.w - 10, 2, color_alpha(fc, t->dark ? 170 : 200));
        }
        if (fl >= 0) {
            int a = (int)(170 * (1.0 - (double)fl / FLASH_MS));
            uint32_t fcol = k->special == KS_BACK ? t->window_text : g->flash_ok ? tt_green() : tt_red();
            gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 6, color_alpha(fcol, a));
            if (a > 85) label = COLOR_WHITE;
        }

        if (k->special) {
            const char *lab = special_labels[k->special];
            if (*lab) {
                bool left = k->special == KS_TAB || k->special == KS_CAPS || k->special == KS_LSHIFT ||
                            (k->special == KS_CTRL && r.x < kr.x + 20);
                int align = (left ? ALIGN_LEFT : (k->special == KS_BACK || k->special == KS_ENTER ||
                                                  k->special == KS_RSHIFT ? ALIGN_RIGHT : ALIGN_CENTER)) | ALIGN_MIDDLE;
                gfx_text_aligned(s, rect_make(r.x + 7, r.y, r.w - 14, r.h - 2), lab,
                                 hl ? label : color_mix(label, bg, 70), FONT_UI, align);
            }
            if (k->special == KS_CAPS) {
                gfx_fill_circle_aa(s, r.x + r.w - 9, r.y + 8, 2.6,
                                   caps ? RGB(76, 217, 100) : color_alpha(t->window_text, 50));
            }
        } else if (k->lo >= 'a' && k->lo <= 'z') {
            char str[2] = { (char)(k->lo - 32), 0 };
            gfx_text_aligned(s, rect_make(r.x, r.y, r.w, r.h - 2), str, label, FONT_UI_BOLD, ALIGN_CENTER | ALIGN_MIDDLE);
            if (k->lo == 'f' || k->lo == 'j')   /* home-row bumps */
                gfx_fill_rounded_rect(s, r.x + r.w / 2 - 5, r.y + r.h - 8, 10, 2, 1, color_alpha(label, 150));
        } else {
            char hi[2] = { k->hi, 0 }, lo[2] = { k->lo, 0 };
            uint32_t hic = hl && shift ? label : color_mix(label, hl ? t->accent : bg, 90);
            uint32_t loc = hl && shift ? color_mix(label, t->accent, 90) : label;
            gfx_text_aligned(s, rect_make(r.x, r.y + 1, r.w, 15), hi, hic, FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
            gfx_text_aligned(s, rect_make(r.x, r.y + 14, r.w, 15), lo, loc, FONT_UI_BOLD, ALIGN_CENTER | ALIGN_MIDDLE);
        }
    }
}

static void tt_key_name(char c, char *out, int size) {
    if (c == ' ') strlcpy(out, "Space", (size_t)size);
    else if (c >= 'a' && c <= 'z') snprintf(out, (size_t)size, "%c", c - 32);
    else snprintf(out, (size_t)size, "%c", c);
}

static void tt_paint_hint(tt_t *g, surface_t *s) {
    char buf[160];
    uint32_t col = fn_muted();
    const font_t *f = FONT_UI;
    bool caps = (input_mods() & MOD_CAPS) != 0;
    if (g->finished) {
        strlcpy(buf, "Enter: next text      Esc: try this one again      Ctrl+1-4: change lesson", sizeof buf);
    } else if (caps) {
        strlcpy(buf, "Caps Lock is on - letters come out as capitals", sizeof buf);
        col = tt_red();
        f = FONT_UI_BOLD;
    } else if (g->note == NOTE_WORD_END) {
        strlcpy(buf, "This word is finished - press Space for the next one", sizeof buf);
        col = tt_red();
    } else if (g->note == NOTE_DOUBLE_SPACE) {
        strlcpy(buf, "One space is enough - type the next word", sizeof buf);
        col = tt_red();
    } else if (g->note == NOTE_SKIPPED) {
        strlcpy(buf, "Skipped to the next word - the missed letters count as mistakes", sizeof buf);
        col = tt_red();
    } else if (g->ntyped > 0 && g->typed[g->ntyped - 1] != g->target[g->ntyped - 1]) {
        strlcpy(buf, "Oops - Backspace fixes the mistake, or just keep going", sizeof buf);
        col = tt_red();
    } else if (!g->started) {
        strlcpy(buf, "Start typing - the clock starts with your first key.   Ctrl+N new text, Ctrl+V your own text",
                sizeof buf);
    } else {
        bool shift;
        char c = g->target[g->ntyped];
        int k = kb_find(c, &shift);
        char name[8];
        tt_key_name(shift && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c, name, sizeof name);
        if (k < 0) {
            snprintf(buf, sizeof buf, "Next: %s", name);
        } else if (shift) {
            int sk = kb_keys[k].finger <= 3 ? kb_rshift : kb_lshift;
            snprintf(buf, sizeof buf, "Next: %s with your %s, holding Shift with your %s", name,
                     finger_names[kb_keys[k].finger], finger_names[kb_keys[sk].finger]);
        } else {
            snprintf(buf, sizeof buf, "Next: %s with your %s", name, finger_names[kb_keys[k].finger]);
        }
    }
    fn_text_c(s, 0, TT_HINT_Y + 4, TT_W, buf, col, f);
}

static void tt_paint_graph(tt_t *g, surface_t *s, rect_t r, double reveal) {
    const theme_t *t = theme_get();
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 8, color_alpha(t->window_text, t->dark ? 14 : 9));
    int n = g->nsamples;
    if (n < 2) {
        gfx_text_aligned(s, r, "Type a longer text to see your speed over time", fn_muted(), FONT_UI,
                         ALIGN_CENTER | ALIGN_MIDDLE);
        return;
    }
    int maxv = 20;
    for (int i = 0; i < n; i++) maxv = MAX(maxv, g->samples[i]);
    maxv = (maxv + 9) / 10 * 10;
    int gx = r.x + 30, gy = r.y + 8, gw = r.w - 40, gh = r.h - 16;
    /* grid: half and full scale */
    uint32_t grid = color_alpha(t->window_text, t->dark ? 34 : 24);
    gfx_hline(s, gx, gy, gw, grid);
    gfx_hline(s, gx, gy + gh / 2, gw, grid);
    gfx_hline(s, gx, gy + gh, gw, color_alpha(t->window_text, t->dark ? 60 : 44));
    char lab[12];
    snprintf(lab, sizeof lab, "%d", maxv);
    gfx_text_aligned(s, rect_make(r.x + 2, gy - 8, 24, 16), lab, fn_muted(), FONT_UI, ALIGN_RIGHT | ALIGN_MIDDLE);
    gfx_text_aligned(s, rect_make(r.x + 2, gy + gh - 8, 24, 16), "0", fn_muted(), FONT_UI, ALIGN_RIGHT | ALIGN_MIDDLE);

    point_t pts[TT_SAMPLES + 2];
    double xs[TT_SAMPLES], ys[TT_SAMPLES];
    for (int i = 0; i < n; i++) {
        xs[i] = gx + (double)gw * i / (n - 1);
        ys[i] = gy + gh - (double)gh * g->samples[i] / maxv;
    }
    double limit = gx + gw * CLAMP(reveal, 0.0, 1.0);
    /* area under the curve */
    int np = 0;
    for (int i = 0; i < n && xs[i] <= limit + 0.5; i++) pts[np++] = (point_t){ (int)lround(xs[i]), (int)lround(ys[i]) };
    if (np >= 2) {
        int lx = pts[np - 1].x;
        pts[np++] = (point_t){ lx, gy + gh };
        pts[np++] = (point_t){ pts[0].x, gy + gh };
        gfx_fill_polygon(s, pts, np, color_alpha(t->accent, t->dark ? 60 : 44));
    }
    uint32_t line = t->accent | 0xFF000000u;
    for (int i = 1; i < n; i++) {
        if (xs[i - 1] > limit) break;
        double x1 = xs[i], y1 = ys[i];
        if (x1 > limit) {   /* partially revealed segment */
            double k = (limit - xs[i - 1]) / (xs[i] - xs[i - 1]);
            x1 = limit;
            y1 = ys[i - 1] + (ys[i] - ys[i - 1]) * k;
        }
        fn_stroke(s, xs[i - 1], ys[i - 1], x1, y1, 2.2, line);
    }
    if (reveal >= 1) gfx_fill_circle_aa(s, xs[n - 1], ys[n - 1], 3.5, line);
}

static const char *tt_rating(tt_t *g) {
    if (g->final_acc < 85) return "Slow down a little";
    if (g->final_wpm >= 60 && g->final_acc >= 95) return "Outstanding!";
    if (g->final_wpm >= 40 && g->final_acc >= 95) return "Excellent!";
    if (g->final_acc >= 92) return "Well done!";
    return "Good effort";
}

static void tt_paint_result(tt_t *g, surface_t *s, uint32_t now) {
    const theme_t *t = theme_get();
    int32_t el = (int32_t)(now - g->finish_t0);
    double p = fn_ease_out(el / (double)RESULT_MS);
    rect_t area = rect_make(TT_PAD - 6, TT_PANEL_Y - 12, TT_W - 2 * TT_PAD + 12, TT_KB_Y + TT_KB_H + 24 - TT_PANEL_Y);
    int cw = 420, ch = 330;
    gfx_fill_rounded_rect(s, area.x, area.y, area.w, area.h, 14, RGBA(0, 0, 0, (int)(80 * p)));
    rect_t c = rect_make(area.x + (area.w - cw) / 2, area.y + (area.h - ch) / 2 + (int)((1 - p) * 24), cw, ch);
    gfx_shadow(s, c.x, c.y, c.w, c.h, 12, 14, (int)((t->dark ? 150 : 90) * p));
    gfx_fill_rounded_rect(s, c.x, c.y, c.w, c.h, 12, color_mix(t->panel_bg, t->window_border, 60));
    gfx_fill_rounded_rect(s, c.x + 1, c.y + 1, c.w - 2, c.h - 2, 11, t->panel_bg);
    char buf[96];
    fn_text_c(s, c.x, c.y + 14, c.w, tt_rating(g), t->window_text, FONT_UI_TITLE);

    /* big number + "WPM" */
    snprintf(buf, sizeof buf, "%d", g->final_wpm);
    int nw = font_text_width(FONT_UI_HUGE, buf), uw = font_text_width(FONT_UI_LARGE, "WPM");
    int bx = c.x + (c.w - nw - 8 - uw) / 2, by = c.y + 50;
    gfx_text(s, bx, by, buf, t->accent, FONT_UI_HUGE);
    gfx_text(s, bx + nw + 8, by + font_ascent(FONT_UI_HUGE) - font_ascent(FONT_UI_LARGE), "WPM", fn_muted(), FONT_UI_LARGE);

    /* four small stats */
    const char *labels[4] = { "Accuracy", "Time", "Mistakes", "Raw speed" };
    char vals[4][24];
    snprintf(vals[0], sizeof vals[0], "%d%%", g->final_acc < 0 ? 0 : g->final_acc);
    fn_fmt_time(vals[1], sizeof vals[1], g->end_ms);
    snprintf(vals[2], sizeof vals[2], "%d", g->kerrors);
    snprintf(vals[3], sizeof vals[3], "%d WPM", g->final_raw);
    uint32_t vcols[4] = { g->final_acc >= 95 ? tt_green() : g->final_acc >= 85 ? t->window_text : tt_red(),
                          t->window_text, g->kerrors ? tt_red() : t->window_text, t->window_text };
    int colw = (c.w - 40) / 4;
    for (int i = 0; i < 4; i++) {
        int x = c.x + 20 + i * colw;
        fn_text_c(s, x, c.y + 120, colw, labels[i], fn_muted(), FONT_UI);
        fn_text_c(s, x, c.y + 137, colw, vals[i], vcols[i], FONT_UI_LARGE);
        if (i) gfx_vline(s, x, c.y + 124, 34, color_alpha(t->window_text, 30));
    }

    double reveal = (el - RESULT_MS * 0.6) / (double)GRAPH_MS;
    tt_paint_graph(g, s, rect_make(c.x + 20, c.y + 170, c.w - 40, 64), reveal);

    /* record / best + tricky keys */
    if (g->record) {
        snprintf(buf, sizeof buf, "New record for %s!", lesson_titles[g->lesson]);
        fn_text_c(s, c.x, c.y + 240, c.w, buf, tt_green(), FONT_UI_BOLD);
    } else if (g->first_result) {
        snprintf(buf, sizeof buf, "First result for %s saved - now beat it!", lesson_titles[g->lesson]);
        fn_text_c(s, c.x, c.y + 240, c.w, buf, tt_green(), FONT_UI_BOLD);
    } else {
        snprintf(buf, sizeof buf, "Best for %s: %d WPM", lesson_titles[g->lesson], g->best);
        fn_text_c(s, c.x, c.y + 240, c.w, buf, fn_muted(), FONT_UI);
    }
    if (g->tricky[0]) snprintf(buf, sizeof buf, "Keys to practise:  %s", g->tricky);
    else strlcpy(buf, "No mistakes at all - perfect!", sizeof buf);
    fn_text_c(s, c.x, c.y + 258, c.w, buf, g->tricky[0] ? t->window_text : tt_green(), FONT_UI);

    int bw = 128, gap = 12, y = c.y + c.h - 50;
    g->btn[2].r = rect_make(c.x + c.w / 2 - gap / 2 - bw, y, bw, 34);
    g->btn[3].r = rect_make(c.x + c.w / 2 + gap / 2, y, bw, 34);
    fn_btn_draw(s, &g->btn[2]);
    fn_btn_draw(s, &g->btn[3]);
}

static void tt_paint(tt_t *g, surface_t *s) {
    const theme_t *t = theme_get();
    uint32_t now = sys_uptime_ms();
    fn_seg_draw(s, g->seg_r, lesson_labels, 4, g->lesson < LES_CUSTOM ? g->lesson : -1, g->seg_hover);
    fn_btn_draw(s, &g->btn[0]);
    fn_btn_draw(s, &g->btn[1]);
    tt_paint_stats(g, s);
    tt_paint_text(g, s, now);
    if (g->lesson == LES_CUSTOM) {
        /* name of the custom text above the panel's right edge */
        char buf[80];
        int npass = g->custom_len > 0 ? 1 + (g->custom_len - 1) / TT_PASSAGE : 1;
        if (npass > 1) snprintf(buf, sizeof buf, "%s - %d%%", g->custom_name, g->custom_pos * 100 / MAX(1, g->custom_len));
        else strlcpy(buf, g->custom_name, sizeof buf);
        int w = font_text_width(FONT_UI, buf) + 16;
        rect_t r = rect_make(g->panel_r.x + g->panel_r.w - w - 12, g->panel_r.y - 9, w, 18);
        gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 9, t->accent);
        gfx_text_aligned(s, r, buf, t->accent_text, FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
    }
    tt_paint_kb(g, s, now);
    tt_paint_hint(g, s);
    if (g->finished) tt_paint_result(g, s, now);
}

/* ======================================================================================
 * Events
 * ==================================================================================== */
static void tt_restart(tt_t *g) { tt_reset(g); }

static void tt_mouse(tt_t *g, const event_t *ev) {
    bool used = false;
    int fired = fn_btns_event(g->win, g->btn, 4, ev, &used);
    if (fired == 0 || fired == 2) { tt_restart(g); return; }
    if (fired == 1 || fired == 3) { tt_new_text(g); return; }
    if (used) return;
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int sh = fn_seg_hit(g->seg_r, 4, ev->x, ev->y);
        if (sh != g->seg_hover) {
            g->seg_hover = sh;
            win_invalidate_rect(g->win, g->seg_r.x, g->seg_r.y, g->seg_r.w, g->seg_r.h);
        }
        break;
    }
    case EV_MOUSE_LEAVE:
        if (g->seg_hover >= 0) {
            g->seg_hover = -1;
            win_invalidate_rect(g->win, g->seg_r.x, g->seg_r.y, g->seg_r.w, g->seg_r.h);
        }
        break;
    case EV_MOUSE_DOWN: {
        if (ev->button != MOUSE_LEFT) break;
        int sg = fn_seg_hit(g->seg_r, 4, ev->x, ev->y);
        if (sg >= 0) tt_set_lesson(g, sg);
        break;
    }
    default:
        break;
    }
}

static void tt_key(tt_t *g, const event_t *ev) {
    int k = ev->key;
    if (k == KEY_CAPSLOCK) { tt_inval_kb(g); tt_inval_hint(g); return; }
    if (ev->mods & MOD_CTRL) {
        if (k >= '1' && k <= '4') tt_set_lesson(g, k - '1');
        else if (k == 'n') tt_new_text(g);
        else if (k == 'r') tt_restart(g);
        else if (k == 'v') tt_paste(g);
        else if (k == KEY_BACKSPACE) tt_backspace(g, true);
        return;
    }
    if (ev->mods & MOD_ALT) return;
    switch (k) {
    case KEY_F2: tt_new_text(g); break;
    case KEY_F5: tt_restart(g); break;
    case KEY_ESCAPE:
        if (g->started || g->finished) tt_restart(g);
        break;
    case KEY_ENTER:
        if (g->finished) tt_new_text(g);
        break;
    case KEY_BACKSPACE:
        tt_backspace(g, false);
        break;
    default:
        break;
    }
}

static void tt_tick(tt_t *g) {
    uint32_t now = sys_uptime_ms();
    if (g->started && !g->finished && !g->paused) {
        uint32_t el = tt_elapsed(g);
        while ((uint32_t)(g->nsamples + 1) * 1000 <= el && g->nsamples < TT_SAMPLES - 1) {
            int w = tt_wpm_at(g, (uint32_t)(g->nsamples + 1) * 1000);
            g->samples[g->nsamples++] = (uint16_t)CLAMP(w, 0, 999);
        }
        int sec = (int)(el / 1000), wpm = tt_wpm_at(g, el);
        if (sec != g->shown_sec || wpm != g->shown_wpm) {
            g->shown_sec = sec;
            g->shown_wpm = wpm;
            tt_inval_stats(g);
        }
    }
    uint32_t since = now - g->last_input;
    bool on = since < BLINK_MS || ((since / BLINK_MS) & 1) == 0;
    if (on != g->blink_on) {
        g->blink_on = on;
        tt_inval_panel(g);
    }
    if (g->flash_key >= 0 && now - g->flash_t0 < FLASH_MS + 40) tt_inval_kb(g);
    if (now - g->caret_t0 < GLIDE_MS + 30 || now - g->reject_t0 < FLASH_MS + 40) tt_inval_panel(g);
    if (fabs(g->scroll - g->scroll_to) > 0.3) {
        g->scroll += (g->scroll_to - g->scroll) * 0.3;
        if (fabs(g->scroll - g->scroll_to) < 0.6) g->scroll = g->scroll_to;
        tt_inval_panel(g);
    }
    if (g->finished && now - g->finish_t0 < RESULT_MS + GRAPH_MS + 60)
        win_invalidate_rect(g->win, 0, TT_PANEL_Y - 14, TT_W, TT_KB_Y + TT_KB_H + 30 - TT_PANEL_Y);
    tt_update_timer(g);
}

static void tt_event(window_t *win, const event_t *ev) {
    tt_t *g = win_get_userdata(win);
    if (!g) return;
    switch (ev->type) {
    case EV_PAINT:
        tt_paint(g, win_surface(win));
        break;
    case EV_TIMER:
        tt_tick(g);
        break;
    case EV_MOUSE_MOVE: case EV_MOUSE_DOWN: case EV_MOUSE_UP: case EV_MOUSE_LEAVE: case EV_DOUBLE_CLICK:
        tt_mouse(g, ev);
        break;
    case EV_KEY_DOWN:
        tt_key(g, ev);
        break;
    case EV_CHAR:
        if (ev->ch >= 32 && ev->ch <= 126) tt_type_char(g, (char)ev->ch);
        break;
    case EV_FOCUS:
        if (g->paused) {
            g->paused = false;
            g->seg_start = sys_uptime_ms();
        }
        g->last_input = sys_uptime_ms();
        win_invalidate(win);
        tt_update_timer(g);
        break;
    case EV_BLUR:
        if (g->started && !g->finished && !g->paused) {
            g->acc_ms += sys_uptime_ms() - g->seg_start;
            g->paused = true;
        }
        fn_btns_reset(win, g->btn, 4);
        win_invalidate(win);
        tt_update_timer(g);
        break;
    case EV_THEME:
        win_invalidate(win);
        break;
    case EV_CLOSE:
        free(g->custom);
        free(g);
        win_set_userdata(win, NULL);
        break;
    default:
        break;
    }
}

static void tt_launch(const char *arg) {
    kb_init();
    tt_t *g = calloc(1, sizeof *g);
    if (!g) return;
    fn_rng_seed(&g->rng, (uint32_t)(uintptr_t)g);
    g->seg_hover = -1;
    g->flash_key = -1;
    g->prev_pick = -1;
    g->seg_r = rect_make(TT_PAD, TT_HDR_Y, 392, TT_HDR_H);
    g->btn[1] = (fn_btn_t){ .r = rect_make(TT_W - TT_PAD - 104, TT_HDR_Y, 104, TT_HDR_H), .label = "New Text" };
    g->btn[0] = (fn_btn_t){ .r = rect_make(TT_W - TT_PAD - 104 - 8 - 88, TT_HDR_Y, 88, TT_HDR_H), .label = "Restart" };
    g->btn[2] = (fn_btn_t){ .label = "Try Again", .hidden = true };
    g->btn[3] = (fn_btn_t){ .label = "Next Text", .primary = true, .hidden = true };
    g->panel_r = rect_make(TT_PAD, TT_PANEL_Y, TT_W - 2 * TT_PAD, TT_PANEL_H);
    g->kb_r = rect_make((TT_W - TT_KB_W) / 2, TT_KB_Y, TT_KB_W, TT_KB_H);

    const app_desc_t *me = app_find("Typing Tutor");
    uint32_t flags = me && app_window_count(me) > 0 ? 0 : WIN_CENTER;
    g->win = win_create("Typing Tutor", TT_W, TT_H, flags);
    if (!g->win) { free(g); return; }
    win_set_userdata(g->win, g);
    win_set_event_handler(g->win, tt_event);
    win_set_cursor(g->win, CURSOR_ARROW);

    g->lesson = CLAMP(fn_score_get("typing_lesson", LES_SENT), 0, LES_NUM);
    if (arg && *arg) {
        if (!strcasecmp(arg, "home")) g->lesson = LES_HOME;
        else if (!strcasecmp(arg, "words")) g->lesson = LES_WORDS;
        else if (!strcasecmp(arg, "sentences")) g->lesson = LES_SENT;
        else if (!strcasecmp(arg, "numbers")) g->lesson = LES_NUM;
        else if (!strncmp(arg, "text:", 5)) {
            if (tt_set_custom(g, arg + 5, "Custom text")) return;
        } else if (vfs_exists(arg) && !vfs_is_dir(arg)) {
            uint32_t size = 0;
            char *d = vfs_read_file(arg, &size);
            bool ok = d && tt_set_custom(g, d, vfs_basename(arg));
            free(d);
            if (ok) return;
        }
    }
    tt_new_text(g);
}

/* Icon: a sheet of paper with a typed line (green, a red mistake, the caret) above a
   blue keyboard with one key lit. */
static void tt_icon(surface_t *s, int x, int y, int size) {
    double u = size / 48.0;
#define P(v) ((int)lround((v) * u))
    /* paper */
    gfx_fill_rounded_rect(s, x + P(9), y + P(3), P(30), P(24), P(3), RGBA(0, 0, 0, 60));
    gfx_fill_rounded_rect(s, x + P(9), y + P(2), P(30), P(24), P(3), RGB(200, 208, 220));
    gfx_fill_rounded_rect(s, x + P(10), y + P(3), P(28), P(22), P(2), COLOR_WHITE);
    gfx_fill_rounded_rect(s, x + P(13), y + P(6), P(22), P(3), P(1), RGB(60, 70, 88));
    gfx_fill_rounded_rect(s, x + P(13), y + P(12), P(9), P(3), P(1), RGB(46, 160, 67));
    gfx_fill_rounded_rect(s, x + P(23), y + P(12), P(5), P(3), P(1), RGB(229, 57, 53));
    gfx_fill_rect(s, x + P(29), y + P(10.5), MAX(1, P(1.5)), P(6), RGB(30, 110, 229));
    /* keyboard */
    gfx_fill_rounded_rect(s, x + P(2), y + P(20) + 1, P(44), P(25), P(5), RGBA(0, 0, 0, 70));
    gfx_fill_rounded_gradient_v(s, x + P(2), y + P(19), P(44), P(25), P(5), RGB(74, 144, 245), RGB(30, 86, 196));
    double kw = 4.6, kh = 3.8, pitch = 5.8, top = 22.5;
    for (int row = 0; row < 3; row++) {
        double off = 4.6 + row * 1.5;
        int nkeys = 7 - (row == 2);
        for (int k = 0; k < nkeys; k++) {
            double kx = off + k * pitch, ky = top + row * 5.0;
            uint32_t col = (row == 1 && k == 3) ? RGB(255, 193, 7) : RGB(240, 245, 252);
            gfx_fill_rounded_rect(s, x + P(kx), y + P(ky), MAX(1, P(kw)), MAX(1, P(kh)), MAX(1, P(1)), col);
        }
    }
    gfx_fill_rounded_rect(s, x + P(14), y + P(37.6), P(20), MAX(1, P(3.4)), MAX(1, P(1)), RGB(240, 245, 252));
#undef P
}

REGISTER_APP(.name = "Typing Tutor", .category = CAT_ACCESSORIES,
             .description = "Practise touch typing with live WPM and accuracy",
             .launch = tt_launch, .draw_icon = tt_icon);
