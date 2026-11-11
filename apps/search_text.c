/*
 * search_text.c - NovaOS Search: words and reading web pages.
 *
 * A word is a run of letters and digits (Windows-1252). It is lower-cased,
 * accents are folded (e -> e for e with any accent, sz -> s ...), English
 * plurals and -ed / -ing endings are taken off (the first steps of Porter's
 * stemmer) and a short list of stop words is left out. Words of one letter
 * or longer than SRCH_WORD_MAX letters are not indexed.
 *
 * A page is read straight from its HTML (no DOM): the title, the meta
 * description, headings, the text (script, style and the like skipped), the
 * links with their text, <base href> and <meta name=robots>. Reading is
 * resumable so a big page can be read over several timer ticks.
 */
#include "search.h"

/* ---- letters ---------------------------------------------------------------------------- */
static unsigned char fold_tab[256];
static bool fold_ready;

static void fold_init(void) {
    for (int c = 0; c < 256; c++) {
        unsigned char f = 0;
        if (c >= 'a' && c <= 'z') f = (unsigned char)c;
        else if (c >= '0' && c <= '9') f = (unsigned char)c;
        else if (c >= 'A' && c <= 'Z') f = (unsigned char)(c + 32);
        else if ((c >= 0xC0 && c <= 0xC6) || (c >= 0xE0 && c <= 0xE6)) f = 'a';
        else if ((c >= 0xC8 && c <= 0xCB) || (c >= 0xE8 && c <= 0xEB)) f = 'e';
        else if ((c >= 0xCC && c <= 0xCF) || (c >= 0xEC && c <= 0xEF)) f = 'i';
        else if ((c >= 0xD2 && c <= 0xD6) || c == 0xD8 || (c >= 0xF2 && c <= 0xF6) || c == 0xF8) f = 'o';
        else if ((c >= 0xD9 && c <= 0xDC) || (c >= 0xF9 && c <= 0xFC)) f = 'u';
        else switch (c) {
            case 0x8A: case 0x9A: case 0xDF: f = 's'; break;
            case 0x8C: case 0x9C: f = 'o'; break;
            case 0x8E: case 0x9E: f = 'z'; break;
            case 0x9F: case 0xDD: case 0xFD: case 0xFF: f = 'y'; break;
            case 0xC7: case 0xE7: f = 'c'; break;
            case 0xD0: case 0xF0: f = 'd'; break;
            case 0xD1: case 0xF1: f = 'n'; break;
            case 0xDE: case 0xFE: f = 't'; break;
            default: break;
        }
        fold_tab[c] = f;
    }
    fold_ready = true;
}

static inline unsigned char fold(unsigned char c) { return fold_tab[c]; }

/* ---- stop words ------------------------------------------------------------------------- */
static const char *const stop_words[] = {
    "about", "after", "all", "also", "an", "and", "any", "are", "as", "at", "be", "been", "but", "by",
    "can", "did", "do", "does", "for", "from", "had", "has", "have", "he", "her", "his", "if", "in",
    "into", "is", "it", "its", "may", "more", "no", "not", "of", "on", "or", "our", "she", "so", "such",
    "than", "that", "the", "their", "them", "then", "there", "these", "they", "this", "those", "to",
    "was", "we", "were", "what", "when", "which", "who", "will", "with", "would", "you", "your",
};

static bool is_stop(const char *w) {
    int lo = 0, hi = (int)(sizeof stop_words / sizeof *stop_words) - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2, c = strcmp(w, stop_words[m]);
        if (!c) return true;
        if (c < 0) hi = m - 1; else lo = m + 1;
    }
    return false;
}

/* ---- Porter's stemmer, step 1 (plurals, -ed / -ing, -y) -------------------------------- */
static char *sb;     /* the word */
static int sk, sj;   /* end of the word, end of the stem being tested */

static bool cons(int i) {
    switch (sb[i]) {
    case 'a': case 'e': case 'i': case 'o': case 'u': return false;
    case 'y': return i == 0 ? true : !cons(i - 1);
    default: return true;
    }
}

/* the number of vowel-consonant sequences in sb[0..sj] */
static int measure(void) {
    int n = 0, i = 0;
    for (;;) { if (i > sj) return n; if (!cons(i)) break; i++; }
    i++;
    for (;;) {
        for (;;) { if (i > sj) return n; if (cons(i)) break; i++; }
        i++;
        n++;
        for (;;) { if (i > sj) return n; if (!cons(i)) break; i++; }
        i++;
    }
}

static bool vowel_in_stem(void) { for (int i = 0; i <= sj; i++) if (!cons(i)) return true; return false; }
static bool double_c(int j) { return j >= 1 && sb[j] == sb[j - 1] && cons(j); }
static bool cvc(int i) {
    if (i < 2 || !cons(i) || cons(i - 1) || !cons(i - 2)) return false;
    return sb[i] != 'w' && sb[i] != 'x' && sb[i] != 'y';
}
static bool ends(const char *s) {
    int len = (int)strlen(s);
    if (len > sk + 1 || memcmp(sb + sk - len + 1, s, (size_t)len)) return false;
    sj = sk - len;
    return true;
}
static void setto(const char *s) {
    int len = (int)strlen(s);
    memcpy(sb + sj + 1, s, (size_t)len);
    sk = sj + len;
}

static void stem(char *w) {
    int n = (int)strlen(w);
    if (n <= 3) return;
    for (int i = 0; i < n; i++) if (w[i] < 'a' || w[i] > 'z') return;
    sb = w;
    sk = n - 1;
    /* step 1a */
    if (sb[sk] == 's') {
        if (ends("sses")) sk -= 2;
        else if (ends("ies")) setto("i");
        else if (sb[sk - 1] != 's' && sb[sk - 1] != 'u') sk--;
    }
    /* step 1b */
    if (ends("eed")) { if (measure() > 0) sk--; }
    else if ((ends("ed") || ends("ing")) && vowel_in_stem()) {
        sk = sj;
        if (ends("at")) setto("ate");
        else if (ends("bl")) setto("ble");
        else if (ends("iz")) setto("ize");
        else if (double_c(sk)) { sk--; if (sb[sk] == 'l' || sb[sk] == 's' || sb[sk] == 'z') sk++; }
        else { sj = sk; if (measure() == 1 && cvc(sk)) setto("e"); }
    }
    /* step 1c */
    if (ends("y") && vowel_in_stem()) sb[sk] = 'i';
    sb[sk + 1] = 0;
}

bool srch_norm_word(char *w) {
    if (!fold_ready) fold_init();
    int n = 0;
    for (int i = 0; w[i]; i++) {
        unsigned char f = fold((unsigned char)w[i]);
        if (!f || n >= SRCH_WORD_MAX) return false;
        w[n++] = (char)f;
    }
    w[n] = 0;
    if (n < 2 || is_stop(w)) return false;
    stem(w);
    return w[0] && w[1];
}

bool srch_next_word(const char *s, uint32_t n, uint32_t *pos, char *out) {
    if (!fold_ready) fold_init();
    uint32_t i = *pos;
    while (i < n) {
        while (i < n && !fold((unsigned char)s[i])) i++;
        uint32_t st = i;
        while (i < n && fold((unsigned char)s[i])) i++;
        if (i == st) break;
        if (i - st > SRCH_WORD_MAX || i - st < 2) continue;
        memcpy(out, s + st, i - st);
        out[i - st] = 0;
        if (srch_norm_word(out)) { *pos = i; return true; }
    }
    *pos = i;
    return false;
}

/* ---- entities --------------------------------------------------------------------------- */
static const struct { const char *n; unsigned char c; } ents[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", ' ' },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x99 }, { "mdash", 0x97 }, { "ndash", 0x96 },
    { "hellip", 0x85 }, { "lsquo", 0x91 }, { "rsquo", 0x92 }, { "ldquo", 0x93 }, { "rdquo", 0x94 },
    { "laquo", 0xAB }, { "raquo", 0xBB }, { "middot", 0xB7 }, { "bull", 0x95 }, { "euro", 0x80 },
    { "pound", 0xA3 }, { "yen", 0xA5 }, { "cent", 0xA2 }, { "sect", 0xA7 }, { "deg", 0xB0 },
    { "plusmn", 0xB1 }, { "times", 0xD7 }, { "divide", 0xF7 }, { "eacute", 0xE9 }, { "egrave", 0xE8 },
    { "ecirc", 0xEA }, { "euml", 0xEB }, { "aacute", 0xE1 }, { "agrave", 0xE0 }, { "acirc", 0xE2 },
    { "auml", 0xE4 }, { "aring", 0xE5 }, { "aelig", 0xE6 }, { "ccedil", 0xE7 }, { "iacute", 0xED },
    { "iuml", 0xEF }, { "ntilde", 0xF1 }, { "oacute", 0xF3 }, { "ouml", 0xF6 }, { "oslash", 0xF8 },
    { "uacute", 0xFA }, { "uuml", 0xFC }, { "szlig", 0xDF }, { "Auml", 0xC4 }, { "Ouml", 0xD6 },
    { "Uuml", 0xDC }, { "Eacute", 0xC9 }, { "thinsp", ' ' }, { "ensp", ' ' }, { "emsp", ' ' },
    { "shy", 0 }, { "zwj", 0 }, { "zwnj", 0 },
};

static int cp_1252(uint32_t cp) {
    static const uint16_t hi[32] = {   /* 0x80..0x9F */
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
        0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
    };
    if (cp < 0x80 || (cp >= 0xA0 && cp < 0x100)) return (int)cp;
    for (int i = 0; i < 32; i++) if (hi[i] && hi[i] == cp) return 0x80 + i;
    if ((cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000) return ' ';
    return '?';
}

size_t srch_entities(const char *s, size_t n, char *out, size_t size) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < size; i++) {
        if (s[i] != '&') { out[o++] = s[i]; continue; }
        size_t j = i + 1;
        int c = -1;
        if (j < n && s[j] == '#') {
            uint32_t v = 0;
            bool hex = j + 1 < n && (s[j + 1] == 'x' || s[j + 1] == 'X');
            j += hex ? 2 : 1;
            size_t st = j;
            for (; j < n && j - st < 8; j++) {
                char d = s[j];
                if (d >= '0' && d <= '9') v = v * (hex ? 16 : 10) + (uint32_t)(d - '0');
                else if (hex && ((d | 32) >= 'a' && (d | 32) <= 'f')) v = v * 16 + (uint32_t)((d | 32) - 'a' + 10);
                else break;
            }
            if (j > st) c = cp_1252(v);
        } else {
            size_t st = j;
            while (j < n && j - st < 8 && ((s[j] | 32) >= 'a' && (s[j] | 32) <= 'z')) j++;
            for (size_t e = 0; e < sizeof ents / sizeof *ents; e++)
                if (strlen(ents[e].n) == j - st && !memcmp(ents[e].n, s + st, j - st)) { c = ents[e].c; break; }
        }
        if (c < 0) { out[o++] = '&'; continue; }
        if (j < n && s[j] == ';') j++;
        if (c) out[o++] = (char)c;
        i = j - 1;
    }
    out[o] = 0;
    return o;
}

/* ---- a page's words --------------------------------------------------------------------- */
static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static srch_wf_t *wf_get(srch_page_t *p, const char *w) {
    if (p->nwf * 10 >= p->wfcap * 7) {
        if (p->nwf >= 20000) return NULL;            /* enough words for one page */
        int nc = p->wfcap ? p->wfcap * 2 : 512;
        srch_wf_t *nt = calloc((size_t)nc, sizeof *nt);
        if (!nt) return NULL;
        for (int i = 0; i < p->wfcap; i++) {
            if (!p->wf[i].w[0]) continue;
            uint32_t k = p->wf[i].hash & (uint32_t)(nc - 1);
            while (nt[k].w[0]) k = (k + 1) & (uint32_t)(nc - 1);
            nt[k] = p->wf[i];
        }
        free(p->wf);
        p->wf = nt;
        p->wfcap = nc;
    }
    uint32_t h = hash_str(w), k = h & (uint32_t)(p->wfcap - 1);
    while (p->wf[k].w[0]) {
        if (p->wf[k].hash == h && !strcmp(p->wf[k].w, w)) return &p->wf[k];
        k = (k + 1) & (uint32_t)(p->wfcap - 1);
    }
    srch_wf_t *e = &p->wf[k];
    e->hash = h;
    strcpy(e->w, w);
    p->nwf++;
    return e;
}

static void count_word(srch_page_t *p, char *w, int field) {
    if (!srch_norm_word(w)) return;
    srch_wf_t *e = wf_get(p, w);
    if (!e) return;
    if (field == 1) { if (e->t < 65535) e->t++; return; }
    if (field == 3) { if (e->u < 65535) e->u++; return; }
    if (e->b < 65535) e->b++;
    if (field == 2 && e->h < 65535) e->h++;
}

static void word_end(srch_page_t *p) {
    if (p->cl && !p->clong) {
        p->cur[p->cl] = 0;
        if (p->cfield != 1) p->words++;
        count_word(p, p->cur, p->cfield);
    }
    p->cl = 0;
    p->clong = false;
}

void srch_page_url_words(srch_page_t *p, const char *url) {
    if (!fold_ready) fold_init();
    const char *s = strstr(url, "://");
    s = s ? s + 3 : url;
    static const char *const skip[] = { "www", "com", "org", "net", "html", "htm", "php", "index", "https", "http" };
    char w[SRCH_WORD_MAX + 2];
    while (*s && *s != '#' && *s != '?') {
        while (*s && *s != '#' && *s != '?' && !fold((unsigned char)*s)) s++;
        int n = 0;
        bool lng = false;
        while (*s && fold((unsigned char)*s)) { if (n <= SRCH_WORD_MAX) w[n++] = *s; else lng = true; s++; }
        if (!n || lng || n > SRCH_WORD_MAX) continue;
        w[n] = 0;
        bool sk = false;
        for (size_t i = 0; i < sizeof skip / sizeof *skip; i++) if (!strcasecmp(w, skip[i])) sk = true;
        if (!sk) count_word(p, w, 3);
    }
}

/* ---- reading the HTML ------------------------------------------------------------------- */
static void buf_put(char *buf, int *len, int cap, unsigned char c) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == 0xA0) {
        if (*len && buf[*len - 1] != ' ' && *len < cap - 1) buf[(*len)++] = ' ';
    } else if (*len < cap - 1 && c >= ' ') buf[(*len)++] = (char)c;
    buf[*len] = 0;
}

static void text_char(srch_page_t *p, unsigned char c) {
    unsigned char f = fold(c);
    if (f) {
        if (!p->cl) p->cfield = p->in_title ? 1 : p->in_head ? 2 : 0;
        if (p->cl < SRCH_WORD_MAX) p->cur[p->cl++] = (char)c;
        else p->clong = true;
    } else if (p->cl) word_end(p);
    if (p->in_title) { buf_put(p->title, &p->tl, SRCH_TITLE_MAX, c); return; }
    if (p->link >= 0 && p->link < p->nlinks) {
        srch_link_t *l = &p->links[p->link];
        int al = (int)strlen(l->anchor);
        buf_put(l->anchor, &al, SRCH_ANCHOR_MAX, c);
    }
    if (p->boiler) return;
    if (p->in_p) buf_put(p->text, &p->xl, SRCH_TEXT_MAX, c);
    buf_put(p->any, &p->al, SRCH_TEXT_MAX, c);
}

static void boundary(srch_page_t *p) {
    if (p->cl) word_end(p);
    if (p->in_title) return;
    if (p->in_p && !p->boiler) buf_put(p->text, &p->xl, SRCH_TEXT_MAX, ' ');
    if (!p->boiler) buf_put(p->any, &p->al, SRCH_TEXT_MAX, ' ');
}

static bool tag_in(const char *t, const char *const *list, int n) {
    for (int i = 0; i < n; i++) if (!strcmp(t, list[i])) return true;
    return false;
}

static const char *const raw_tags[] = { "script", "style", "noscript", "template", "svg", "math", "textarea", "select", "iframe", "object", "canvas" };
static const char *const inline_tags[] = { "a", "abbr", "b", "bdi", "bdo", "cite", "code", "data", "dfn", "em", "font", "i", "kbd",
                                           "mark", "q", "s", "samp", "small", "span", "strike", "strong", "sub", "sup", "time", "tt",
                                           "u", "var", "wbr", "label", "nobr" };
static const char *const boiler_tags[] = { "nav", "footer", "header", "aside", "form", "menu" };

/* an attribute of the tag text t[0..n): its decoded value in out */
static bool attr(const char *t, size_t n, const char *name, char *out, size_t size) {
    size_t nl = strlen(name), i = 0;
    while (i < n) {
        while (i < n && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r' || t[i] == '/')) i++;
        size_t ns = i;
        while (i < n && t[i] != '=' && t[i] != ' ' && t[i] != '\t' && t[i] != '\n' && t[i] != '\r' && t[i] != '/') i++;
        size_t ne = i;
        while (i < n && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++;
        size_t vs = i, ve = i;
        if (i < n && t[i] == '=') {
            i++;
            while (i < n && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++;
            if (i < n && (t[i] == '"' || t[i] == '\'')) {
                char q = t[i++];
                vs = i;
                while (i < n && t[i] != q) i++;
                ve = i;
                if (i < n) i++;
            } else {
                vs = i;
                while (i < n && t[i] != ' ' && t[i] != '\t' && t[i] != '\n' && t[i] != '\r') i++;
                ve = i;
            }
        }
        if (ne - ns == nl && !strncasecmp(t + ns, name, nl)) {
            srch_entities(t + vs, ve - vs, out, size);
            return true;
        }
        if (ne == ns && i == ns) i++;
    }
    return false;
}

static void open_tag(srch_page_t *p, const char *name, const char *at, size_t an) {
    if (!strcmp(name, "title")) { if (!p->tl) p->in_title = 1; return; }
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) { p->in_head++; return; }
    if (!strcmp(name, "p")) { p->in_p = 1; return; }
    if (tag_in(name, boiler_tags, (int)(sizeof boiler_tags / sizeof *boiler_tags))) { p->boiler++; return; }
    if (!strcmp(name, "a")) {
        p->link = -1;
        char href[1024];
        if (p->nlinks >= SRCH_LINKS_MAX || !attr(at, an, "href", href, sizeof href)) return;
        char *h = href;
        while (*h == ' ' || *h == '\t' || *h == '\n') h++;
        if (!*h || *h == '#' || !strncasecmp(h, "javascript:", 11) || !strncasecmp(h, "mailto:", 7) || !strncasecmp(h, "tel:", 4)) return;
        if (!p->links) { p->links = calloc(SRCH_LINKS_MAX, sizeof *p->links); if (!p->links) return; }
        p->links[p->nlinks].href = strdup(h);
        if (!p->links[p->nlinks].href) return;
        p->links[p->nlinks].anchor[0] = 0;
        p->link = p->nlinks++;
        return;
    }
    if (!strcmp(name, "base")) { if (!p->base[0]) attr(at, an, "href", p->base, sizeof p->base); return; }
    if (!strcmp(name, "meta")) {
        char nm[64] = "", content[SRCH_DESC_MAX * 2] = "";
        if (!attr(at, an, "name", nm, sizeof nm)) attr(at, an, "property", nm, sizeof nm);
        if (!nm[0] || !attr(at, an, "content", content, sizeof content)) return;
        if ((!strcasecmp(nm, "description") || !strcasecmp(nm, "og:description")) && !p->dl)
            for (char *c = content; *c; c++) buf_put(p->desc, &p->dl, SRCH_DESC_MAX, (unsigned char)*c);
        if (!strcasecmp(nm, "robots") || !strcasecmp(nm, "novaos-search")) {
            for (char *c = content; *c; c++) *c = (char)((*c >= 'A' && *c <= 'Z') ? *c + 32 : *c);
            if (strstr(content, "noindex") || strstr(content, "none")) p->noindex = true;
            if (strstr(content, "nofollow") || strstr(content, "none")) p->nofollow = true;
        }
    }
}

static void close_tag(srch_page_t *p, const char *name) {
    if (!strcmp(name, "title")) { p->in_title = 0; return; }
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) { if (p->in_head) p->in_head--; return; }
    if (!strcmp(name, "p")) { p->in_p = 0; return; }
    if (tag_in(name, boiler_tags, (int)(sizeof boiler_tags / sizeof *boiler_tags))) { if (p->boiler) p->boiler--; return; }
    if (!strcmp(name, "a")) p->link = -1;
}

void srch_page_init(srch_page_t *p, const char *html, uint32_t len) {
    if (!fold_ready) fold_init();
    memset(p, 0, sizeof *p);
    p->src = html;
    p->len = len;
    p->raw = -1;
    p->link = -1;
}

static const char *find_ci(const char *s, const char *e, const char *pat) {
    size_t n = strlen(pat);
    for (; s + n <= e; s++) if (!strncasecmp(s, pat, n)) return s;
    return NULL;
}

bool srch_page_feed(srch_page_t *p, uint32_t budget) {
    if (p->done) return true;
    const char *s = p->src, *end = p->src + p->len;
    uint32_t stop = p->pos + budget;
    while (p->pos < p->len && p->pos < stop) {
        const char *c = s + p->pos;
        if (p->raw >= 0) {
            /* skip to the end of <script> etc. */
            char pat[16];
            snprintf(pat, sizeof pat, "</%s", raw_tags[p->raw]);
            const char *e = find_ci(c, end, pat);
            p->pos = e ? (uint32_t)(e - s) : p->len;
            p->raw = -1;
            continue;
        }
        if (*c != '<') {
            /* text up to the next tag (64 KB at a time) */
            const char *e = memchr(c, '<', (size_t)(end - c));
            if (!e) e = end;
            if (e - c > 65536) e = c + 65536;
            char buf[1024];
            for (const char *q = c; q < e;) {
                size_t chunk = (size_t)(e - q) > 700 ? 700 : (size_t)(e - q);
                /* do not split an entity */
                if (q + chunk < e) for (size_t k = chunk > 10 ? chunk - 10 : 0; k < chunk; k++) if (q[k] == '&') { chunk = k ? k : chunk; break; }
                size_t n = srch_entities(q, chunk, buf, sizeof buf);
                for (size_t k = 0; k < n; k++) text_char(p, (unsigned char)buf[k]);
                q += chunk;
            }
            p->pos = (uint32_t)(e - s);
            continue;
        }
        /* markup */
        if (c + 4 <= end && !memcmp(c, "<!--", 4)) {
            const char *e = find_ci(c + 4, end, "-->");
            p->pos = e ? (uint32_t)(e - s) + 3 : p->len;
            continue;
        }
        if (c + 1 < end && (c[1] == '!' || c[1] == '?')) {
            const char *e = memchr(c, '>', (size_t)(end - c));
            p->pos = e ? (uint32_t)(e - s) + 1 : p->len;
            continue;
        }
        bool closing = c + 1 < end && c[1] == '/';
        const char *q = c + (closing ? 2 : 1);
        char name[16];
        int nl = 0;
        while (q < end && (((*q | 32) >= 'a' && (*q | 32) <= 'z') || (*q >= '0' && *q <= '9') || *q == '-' || *q == ':')) {
            if (nl < (int)sizeof name - 1) name[nl++] = (char)((*q >= 'A' && *q <= 'Z') ? *q + 32 : *q);
            q++;
        }
        name[nl] = 0;
        if (!nl) { text_char(p, '<'); p->pos++; continue; }
        /* the end of the tag (quotes respected) */
        const char *a = q, *e = q;
        char quote = 0;
        for (; e < end; e++) {
            if (quote) { if (*e == quote) quote = 0; }
            else if (*e == '"' || *e == '\'') { if (e > a && (e[-1] == '=' || e[-1] == ' ')) quote = *e; }
            else if (*e == '>') break;
        }
        if (e >= end) { p->pos = p->len; break; }
        p->pos = (uint32_t)(e - s) + 1;
        if (!tag_in(name, inline_tags, (int)(sizeof inline_tags / sizeof *inline_tags))) boundary(p);
        if (closing) { close_tag(p, name); continue; }
        open_tag(p, name, a, (size_t)(e - a));
        for (int i = 0; i < (int)(sizeof raw_tags / sizeof *raw_tags); i++)
            if (!strcmp(name, raw_tags[i]) && e[-1] != '/') { p->raw = i; break; }
    }
    if (p->pos < p->len) return false;
    word_end(p);
    while (p->tl && p->title[p->tl - 1] == ' ') p->title[--p->tl] = 0;
    while (p->dl && p->desc[p->dl - 1] == ' ') p->desc[--p->dl] = 0;
    while (p->xl && p->text[p->xl - 1] == ' ') p->text[--p->xl] = 0;
    while (p->al && p->any[p->al - 1] == ' ') p->any[--p->al] = 0;
    if (p->xl < 80 && p->al > p->xl) { memcpy(p->text, p->any, (size_t)p->al + 1); p->xl = p->al; }
    for (int i = 0; i < p->nlinks; i++) {
        int n = (int)strlen(p->links[i].anchor);
        while (n && p->links[i].anchor[n - 1] == ' ') p->links[i].anchor[--n] = 0;
    }
    p->done = true;
    return true;
}

void srch_page_free(srch_page_t *p) {
    for (int i = 0; i < p->nlinks; i++) free(p->links[i].href);
    free(p->links);
    free(p->wf);
    p->links = NULL;
    p->wf = NULL;
    p->nlinks = p->nwf = p->wfcap = 0;
}

uint32_t srch_url_key(const char *url) {
    const char *s = strstr(url, "://");
    s = s ? s + 3 : url;
    uint32_t h = 2166136261u;
    const char *hs = s;
    while (*s && *s != '/' && *s != '?' && *s != '#') s++;
    const char *he = s;
    /* no default port */
    if (he - hs > 3 && !memcmp(he - 3, ":80", 3)) he -= 3;
    else if (he - hs > 4 && !memcmp(he - 4, ":443", 4)) he -= 4;
    for (const char *q = hs; q < he; q++) h = (h ^ (unsigned char)((*q >= 'A' && *q <= 'Z') ? *q + 32 : *q)) * 16777619u;
    if (!*s || *s == '#' || *s == '?') h = (h ^ '/') * 16777619u;
    for (; *s && *s != '#'; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h ? h : 1;
}
