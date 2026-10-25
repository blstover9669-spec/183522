/*
 * web_html.c - the Web Browser's HTML parser.
 *
 * Real pages are full of broken markup, so this is a forgiving tokenizer and
 * tree builder in the spirit of the HTML5 algorithm, without its full
 * complexity: implied <html>/<head>/<body>, implicitly closed <p>, <li>, <dt>,
 * <dd>, <option>, table rows and cells, implied <tr>, text and stray elements
 * inside tables moved in front of the table ("foster parenting"), end tags
 * that do not match anything ignored, raw-text elements (<script>, <style>,
 * <textarea>, <title>), comments, entities (named and numeric) and the
 * document charset (UTF-8 or Windows-1252, converted to 1252 for the page
 * fonts). Nesting is capped so the recursive layout never runs out of stack.
 */
#include "web.h"

/* ---- arena ----------------------------------------------------------------- */
struct web_chunk { web_chunk_t *next; size_t size, used; uint8_t data[]; };

void *wa_alloc(web_arena_t *a, size_t n) {
    n = (n + 7) & ~(size_t)7;
    web_chunk_t *c = a->head;
    if (!c || c->used + n > c->size) {
        size_t sz = n > 60000 ? n : 60000;
        c = malloc(sizeof *c + sz);
        if (!c) return NULL;
        c->size = sz;
        c->used = 0;
        c->next = a->head;
        a->head = c;
    }
    void *p = c->data + c->used;
    c->used += n;
    a->used += n;
    memset(p, 0, n);
    return p;
}

char *wa_strndup(web_arena_t *a, const char *s, size_t n) {
    char *p = wa_alloc(a, n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void wa_free(web_arena_t *a) {
    web_chunk_t *c = a->head;
    while (c) { web_chunk_t *n = c->next; free(c); c = n; }
    a->head = NULL;
    a->used = 0;
}

/* ---- tags ------------------------------------------------------------------- */
#define WEB_TAG_STR(id, s) s,
static const char *const tag_names[T_COUNT] = { "", WEB_TAGS(WEB_TAG_STR) };
#undef WEB_TAG_STR

const char *web_tag_name(int tag) { return tag > 0 && tag < T_COUNT ? tag_names[tag] : ""; }

static int tag_lookup(const char *s) {
    /* binary search: the list is sorted alphabetically */
    int lo = 1, hi = T_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(s, tag_names[mid]);
        if (!c) return mid;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return T_UNKNOWN;
}

/* ---- charsets ----------------------------------------------------------------- */
static const uint16_t cp80[32] = {
    0x20AC, 0x2192, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x2190, 0x017D, 0x2191,
    0x2193, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x2713, 0x017E, 0x0178 };
/* real Windows-1252 0x80-0x9F (for decoding 1252 pages; 0 = undefined) */
static const uint16_t w1252[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };
static const char latin_ext[] =
    "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";

/* -1 = drop the character entirely */
int web_cp_to_1252(uint32_t cp) {
    if (cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f') return (int)cp;
    if (cp < 0x20) return -1;
    if (cp < 0x7F) return (int)cp;
    if (cp >= 0xA0 && cp <= 0xFF) return cp == 0xAD ? -1 : (int)cp;
    for (int i = 0; i < 32; i++) if (cp80[i] == cp) return 0x80 + i;
    if (cp >= 0x100 && cp < 0x180) return (unsigned char)latin_ext[cp - 0x100];
    switch (cp) {
    case 0x200B: case 0x200C: case 0x200D: case 0x200E: case 0x200F: case 0xFEFF:
    case 0x2060: case 0x2061: case 0x2062: case 0x2063: case 0xFE0F: case 0xFE0E:
        return -1;
    case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004: case 0x2005:
    case 0x2006: case 0x2007: case 0x2008: case 0x2009: case 0x200A: case 0x205F:
    case 0x3000:
        return ' ';
    case 0x202F: return 0xA0;
    case 0x2010: case 0x2011: case 0x2012: case 0x2043: case 0x2212: case 0x2796: return '-';
    case 0x2015: return 0x97;
    case 0x201B: case 0x2032: case 0x02BC: case 0x02B9: return '\'';
    case 0x201F: case 0x2033: case 0x02BA: return '"';
    case 0x2024: return '.';
    case 0x2044: case 0x2215: return '/';
    case 0x2217: case 0x2605: case 0x2606: case 0x2731: return '*';
    case 0x2022 + 1: case 0x25CF: case 0x25AA: case 0x25AB: case 0x25E6: case 0x2219: case 0x22C5:
        return 0x95;
    case 0x21D2: case 0x27A1: case 0x2794: case 0x279C: case 0x27F6: case 0x2B95: case 0x25B6:
    case 0x25B8: case 0x25BA: case 0x276F: return 0x81;
    case 0x21D0: case 0x2B05: case 0x27F5: case 0x25C0: case 0x25C2: case 0x25C4: case 0x276E: return 0x8D;
    case 0x2B06: case 0x25B2: case 0x25B4: return 0x8F;
    case 0x2B07: case 0x25BC: case 0x25BE: return 0x90;
    case 0x2714: case 0x2705: case 0x2611: return 0x9D;
    case 0x2715: case 0x2716: case 0x2717: case 0x2718: case 0x274C: case 0x2A2F: return 'x';
    case 0x2264: return '<';
    case 0x2265: return '>';
    case 0x2260: return '#';
    case 0x2248: case 0x223C: return '~';
    case 0x00A0: return 0xA0;
    case 0x2116: return 'N';
    case 0x0131: return 'i';
    case 0x0218: return 'S';
    case 0x0219: return 's';
    case 0x021A: return 'T';
    case 0x021B: return 't';
    case 0x2190 + 4: return '-';   /* left right arrow */
    default: break;
    }
    if (cp >= 0x1F000 && cp < 0x20000) return -1;   /* emoji: nothing we can draw */
    if (cp >= 0x2600 && cp < 0x2800) return -1;     /* misc symbols / dingbats */
    return '?';
}

/* Decode one UTF-8 sequence at s (max n bytes); returns length, *cp = 0xFFFD if bad. */
static int utf8_dec(const unsigned char *s, size_t n, uint32_t *cp) {
    unsigned c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
    if (!len || (size_t)len > n) { *cp = 0xFFFD; return 1; }
    uint32_t v = c & (0x3F >> (len - 1));
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (s[i] & 0x3F);
    }
    *cp = v;
    return len;
}

static bool valid_utf8(const char *src, size_t len) {
    const unsigned char *s = (const unsigned char *)src;
    size_t i = 0;
    int high = 0, bad = 0;
    while (i < len && i < 65536) {
        if (s[i] < 0x80) { i++; continue; }
        uint32_t cp;
        int l = utf8_dec(s + i, len - i, &cp);
        if (cp == 0xFFFD && l == 1) bad++; else high++;
        i += l;
    }
    return bad == 0 || high > bad * 4;
}

static int cs_from_name(const char *p) {
    while (*p == ' ' || *p == '"' || *p == '\'' || *p == '=') p++;
    if (!strncasecmp(p, "utf-8", 5) || !strncasecmp(p, "utf8", 4)) return WEB_CS_UTF8;
    if (!strncasecmp(p, "iso-8859-1", 10) || !strncasecmp(p, "windows-1252", 12) ||
        !strncasecmp(p, "latin1", 6) || !strncasecmp(p, "us-ascii", 8) || !strncasecmp(p, "iso-8859-15", 11) ||
        !strncasecmp(p, "cp1252", 6))
        return WEB_CS_1252;
    return WEB_CS_AUTO;
}

int web_detect_charset(const char *src, size_t len, const char *content_type) {
    if (content_type) {
        const char *p = strcasestr(content_type, "charset");
        if (p) {
            int cs = cs_from_name(p + 7);
            if (cs != WEB_CS_AUTO) return cs;
        }
    }
    if (len >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF)
        return WEB_CS_UTF8;
    /* <meta charset=...> / content="text/html; charset=..." in the first 2 KB */
    size_t n = len < 2048 ? len : 2048;
    for (size_t i = 0; i + 8 < n; i++) {
        if ((src[i] | 0x20) == 'c' && !strncasecmp(src + i, "charset", 7)) {
            char tmp[24];
            size_t k = 0;
            for (size_t j = i + 7; j < n && k < sizeof tmp - 1 && src[j] != '>' && src[j] != ';' && src[j] != '/'; j++) tmp[k++] = src[j];
            tmp[k] = 0;
            int cs = cs_from_name(tmp);
            if (cs != WEB_CS_AUTO) return cs;
        }
    }
    return valid_utf8(src, len) ? WEB_CS_UTF8 : WEB_CS_1252;
}

char *web_to_1252(const char *src, size_t len, int cs, size_t *outlen) {
    if (cs == WEB_CS_AUTO) cs = web_detect_charset(src, len, NULL);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    const unsigned char *s = (const unsigned char *)src;
    size_t o = 0, i = 0;
    if (cs == WEB_CS_UTF8 && len >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) i = 3;
    while (i < len) {
        unsigned c = s[i];
        if (c < 0x80) {
            if (c == 0) c = ' ';
            out[o++] = (char)c;
            i++;
            continue;
        }
        uint32_t cp;
        if (cs == WEB_CS_UTF8) {
            i += utf8_dec(s + i, len - i, &cp);
        } else {
            cp = (c >= 0x80 && c < 0xA0) ? w1252[c - 0x80] : c;
            if (!cp) cp = '?';
            i++;
        }
        int b = web_cp_to_1252(cp);
        if (b >= 0) out[o++] = (char)b;
    }
    out[o] = 0;
    if (outlen) *outlen = o;
    return out;
}

void web_1252_to_ui(const char *in, char *out, size_t size) {
    size_t o = 0;
    for (; *in && o + 1 < size; in++) {
        unsigned char c = (unsigned char)*in;
        if (c >= 0x80 && c < 0xA0) {
            switch (c) {
            case 0x91: case 0x92: case 0x82: c = '\''; break;
            case 0x93: case 0x94: case 0x84: c = '"'; break;
            case 0x96: case 0x97: c = '-'; break;
            case 0x95: c = 0x95; break;                /* bullet: same slot in the UI font */
            case 0x85: c = 0x9E; break;                /* ellipsis */
            case 0x81: c = 0x93; break;                /* arrows */
            case 0x8D: c = 0x91; break;
            case 0x8F: c = 0x92; break;
            case 0x90: c = 0x94; break;
            case 0x9D: c = 0x96; break;                /* check mark */
            case 0x8A: c = 'S'; break;
            case 0x9A: c = 's'; break;
            case 0x8E: c = 'Z'; break;
            case 0x9E: c = 'z'; break;
            case 0x9F: c = 'Y'; break;
            case 0x80: c = 'E'; break;
            case 0x99: c = 'T'; break;
            default: c = '?'; break;
            }
        }
        out[o++] = (char)c;
    }
    out[o] = 0;
}

/* ---- entities --------------------------------------------------------------------- */
static const char latin1_names[] =
    "nbsp iexcl cent pound curren yen brvbar sect uml copy ordf laquo not shy reg macr "
    "deg plusmn sup2 sup3 acute micro para middot cedil sup1 ordm raquo frac14 frac12 frac34 iquest "
    "Agrave Aacute Acirc Atilde Auml Aring AElig Ccedil Egrave Eacute Ecirc Euml Igrave Iacute Icirc Iuml "
    "ETH Ntilde Ograve Oacute Ocirc Otilde Ouml times Oslash Ugrave Uacute Ucirc Uuml Yacute THORN szlig "
    "agrave aacute acirc atilde auml aring aelig ccedil egrave eacute ecirc euml igrave iacute icirc iuml "
    "eth ntilde ograve oacute ocirc otilde ouml divide oslash ugrave uacute ucirc uuml yacute thorn yuml ";

static const struct { const char *name; uint16_t cp; } entities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
    {"AMP", '&'}, {"LT", '<'}, {"GT", '>'}, {"QUOT", '"'},
    {"OElig", 0x152}, {"oelig", 0x153}, {"Scaron", 0x160}, {"scaron", 0x161}, {"Yuml", 0x178},
    {"fnof", 0x192}, {"circ", 0x2C6}, {"tilde", 0x2DC}, {"ensp", 0x2002}, {"emsp", 0x2003},
    {"thinsp", 0x2009}, {"zwnj", 0x200C}, {"zwj", 0x200D}, {"lrm", 0x200E}, {"rlm", 0x200F},
    {"ndash", 0x2013}, {"mdash", 0x2014}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"sbquo", 0x201A},
    {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bdquo", 0x201E}, {"dagger", 0x2020}, {"Dagger", 0x2021},
    {"bull", 0x2022}, {"hellip", 0x2026}, {"permil", 0x2030}, {"prime", 0x2032}, {"Prime", 0x2033},
    {"lsaquo", 0x2039}, {"rsaquo", 0x203A}, {"oline", 0x203E}, {"frasl", 0x2044}, {"euro", 0x20AC},
    {"trade", 0x2122}, {"TRADE", 0x2122}, {"larr", 0x2190}, {"uarr", 0x2191}, {"rarr", 0x2192}, {"darr", 0x2193},
    {"harr", 0x2194}, {"rArr", 0x21D2}, {"lArr", 0x21D0}, {"minus", 0x2212}, {"lowast", 0x2217},
    {"le", 0x2264}, {"ge", 0x2265}, {"ne", 0x2260}, {"asymp", 0x2248}, {"sim", 0x223C},
    {"middot", 0xB7}, {"centerdot", 0xB7}, {"nbsp", 0xA0}, {"NonBreakingSpace", 0xA0},
    {"check", 0x2713}, {"checkmark", 0x2713}, {"star", 0x2606}, {"starf", 0x2605}, {"bigstar", 0x2605},
    {"hyphen", 0x2010}, {"dash", 0x2010}, {"horbar", 0x2015}, {"quest", '?'}, {"excl", '!'},
    {"num", '#'}, {"dollar", '$'}, {"percnt", '%'}, {"lpar", '('}, {"rpar", ')'}, {"ast", '*'},
    {"plus", '+'}, {"comma", ','}, {"period", '.'}, {"sol", '/'}, {"colon", ':'}, {"semi", ';'},
    {"equals", '='}, {"commat", '@'}, {"lsqb", '['}, {"lbrack", '['}, {"bsol", '\\'}, {"rsqb", ']'},
    {"rbrack", ']'}, {"Hat", '^'}, {"lowbar", '_'}, {"grave", '`'}, {"lcub", '{'}, {"lbrace", '{'},
    {"verbar", '|'}, {"vert", '|'}, {"rcub", '}'}, {"rbrace", '}'}, {"Tab", '\t'}, {"NewLine", '\n'},
    {"times", 0xD7}, {"divide", 0xF7}, {"loz", 0x25CA}, {"spades", 0x2660}, {"clubs", 0x2663},
    {"hearts", 0x2665}, {"diams", 0x2666}, {"copy", 0xA9}, {"COPY", 0xA9}, {"reg", 0xAE}, {"REG", 0xAE},
};

/* Decode an entity at s (after '&'). Returns chars consumed (0 = not an entity), *cp set. */
static int entity(const char *s, size_t n, uint32_t *cp) {
    if (n < 2) return 0;
    if (s[0] == '#') {
        size_t i = 1;
        uint32_t v = 0;
        int digits = 0;
        if (i < n && (s[i] == 'x' || s[i] == 'X')) {
            i++;
            while (i < n && isxdigit((unsigned char)s[i]) && digits < 8) {
                v = v * 16 + (uint32_t)(isdigit((unsigned char)s[i]) ? s[i] - '0' : (tolower((unsigned char)s[i]) - 'a' + 10));
                i++; digits++;
            }
        } else {
            while (i < n && isdigit((unsigned char)s[i]) && digits < 9) { v = v * 10 + (uint32_t)(s[i] - '0'); i++; digits++; }
        }
        if (!digits) return 0;
        if (i < n && s[i] == ';') i++;
        if (v >= 0x80 && v < 0xA0 && w1252[v - 0x80]) v = w1252[v - 0x80];   /* &#150; etc. */
        if (v == 0 || v > 0x10FFFF) v = 0xFFFD;
        *cp = v;
        return (int)i;
    }
    char name[40];
    size_t k = 0;
    while (k < n && k < sizeof name - 1 && isalnum((unsigned char)s[k])) { name[k] = s[k]; k++; }
    name[k] = 0;
    if (!k) return 0;
    bool semi = k < n && s[k] == ';';
    for (int i = 0; i < ARRAY_SIZE(entities); i++)
        if (!strcmp(entities[i].name, name)) { *cp = entities[i].cp; return (int)k + (semi ? 1 : 0); }
    /* Latin-1 names */
    const char *p = latin1_names;
    int idx = 0;
    while (*p) {
        const char *e = strchr(p, ' ');
        if ((size_t)(e - p) == k && !strncmp(p, name, k)) { *cp = 0xA0 + idx; return (int)k + (semi ? 1 : 0); }
        p = e + 1;
        idx++;
    }
    /* legacy: "&copy2024" style prefixes without ';' for the common ones */
    if (!semi) {
        static const char *pref[] = { "amp", "lt", "gt", "quot", "nbsp", "copy", "reg" };
        static const uint16_t pcp[] = { '&', '<', '>', '"', 0xA0, 0xA9, 0xAE };
        for (int i = 0; i < 7; i++) {
            size_t l = strlen(pref[i]);
            if (k > l && !strncmp(name, pref[i], l)) { *cp = pcp[i]; return (int)l; }
        }
    }
    return 0;
}

/* Copy text with entities decoded (and 1252 conversion of entity code points). */
static char *decode_text(web_arena_t *a, const char *s, size_t n, int *outlen) {
    char *out = wa_alloc(a, n + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '&') {
            uint32_t cp;
            int l = entity(s + i + 1, n - i - 1, &cp);
            if (l) {
                int b = web_cp_to_1252(cp);
                if (b >= 0) out[o++] = (char)b;
                i += l;
                continue;
            }
        }
        out[o++] = s[i];
    }
    out[o] = 0;
    if (outlen) *outlen = (int)o;
    return out;
}

/* ---- tree building ------------------------------------------------------------------ */
#define MAXD 160

typedef struct parser {
    web_doc_t *d;
    const char *s;
    size_t n, i;
    web_node_t *stack[MAXD];
    int sp;
    web_node_t *form;           /* the open <form> (form element pointer) */
    bool in_body;
    bool after_head;
} parser_t;

static web_node_t *new_node(parser_t *p, int type) {
    web_node_t *n = wa_alloc(&p->d->ar, sizeof *n);
    if (!n) return NULL;
    n->type = (uint8_t)type;
    n->ctl = n->img = n->link = -1;
    return n;
}

static void append(web_node_t *parent, web_node_t *n) {
    n->parent = parent;
    n->prev = parent->last;
    if (parent->last) parent->last->next = n; else parent->first = n;
    parent->last = n;
    n->depth = parent->depth < 255 ? parent->depth + 1 : 255;
    if (n->type == WN_ELEM) {
        int idx = 1;
        for (web_node_t *q = n->prev; q; q = q->prev) if (q->type == WN_ELEM) { idx = q->index + 1; break; }
        n->index = idx;
    }
}

static void insert_before(web_node_t *ref, web_node_t *n) {
    web_node_t *parent = ref->parent;
    n->parent = parent;
    n->next = ref;
    n->prev = ref->prev;
    if (ref->prev) ref->prev->next = n; else parent->first = n;
    ref->prev = n;
    n->depth = ref->depth;
    n->index = ref->index;   /* approximate */
}

static web_node_t *cur(parser_t *p) { return p->stack[p->sp - 1]; }

static void push(parser_t *p, web_node_t *n) {
    if (p->sp < MAXD) p->stack[p->sp++] = n;
}

static void pop_to(parser_t *p, int idx) {   /* pop stack[idx] and above */
    if (idx < 1) idx = 1;                    /* never pop the document */
    p->sp = idx;
}

static int find_open(parser_t *p, int tag, const int *stop_tags) {
    for (int i = p->sp - 1; i >= 1; i--) {
        int t = p->stack[i]->tag;
        if (t == tag) return i;
        if (stop_tags) for (const int *st = stop_tags; *st; st++) if (t == *st) return -1;
    }
    return -1;
}

static const int scope_stop[] = { T_TABLE, T_TD, T_TH, T_CAPTION, T_HTML, T_BUTTON, T_OBJECT, T_TEMPLATE, 0 };
static const int list_stop[] = { T_UL, T_OL, T_MENU, T_DIR, T_TABLE, T_TD, T_TH, T_HTML, 0 };
static const int dl_stop[] = { T_DL, T_TABLE, T_TD, T_TH, T_HTML, 0 };
static const int cell_stop[] = { T_TABLE, T_HTML, 0 };
static const int select_stop[] = { T_SELECT, T_HTML, 0 };

static bool closes_p(int t) {
    switch (t) {
    case T_ADDRESS: case T_ARTICLE: case T_ASIDE: case T_BLOCKQUOTE: case T_CENTER: case T_DETAILS:
    case T_DIALOG: case T_DIR: case T_DIV: case T_DL: case T_FIELDSET: case T_FIGCAPTION: case T_FIGURE:
    case T_FOOTER: case T_FORM: case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
    case T_HEADER: case T_HGROUP: case T_HR: case T_MAIN: case T_MENU: case T_NAV: case T_OL: case T_P:
    case T_PRE: case T_SECTION: case T_SUMMARY: case T_TABLE: case T_UL: case T_LI: case T_DD: case T_DT:
    case T_PLAINTEXT: case T_XMP: case T_SEARCH:
        return true;
    default:
        return false;
    }
}

static bool is_void(int t) {
    switch (t) {
    case T_AREA: case T_BASE: case T_BASEFONT: case T_BR: case T_COL: case T_EMBED: case T_HR:
    case T_IMG: case T_INPUT: case T_LINK: case T_META: case T_PARAM: case T_SOURCE: case T_TRACK:
    case T_WBR: case T_FRAME:
        return true;
    default:
        return false;
    }
}

static bool is_table_ctx(int t) {
    return t == T_TABLE || t == T_TBODY || t == T_THEAD || t == T_TFOOT || t == T_TR;
}

static bool head_tag(int t) {
    return t == T_TITLE || t == T_META || t == T_LINK || t == T_STYLE || t == T_SCRIPT || t == T_BASE ||
           t == T_TEMPLATE;
}

static void ensure_html(parser_t *p) {
    if (p->d->html) return;
    web_node_t *h = new_node(p, WN_ELEM);
    h->tag = T_HTML; h->name = "html";
    append(p->d->root, h);
    p->d->html = h;
    push(p, h);
}

static void ensure_head(parser_t *p) {
    ensure_html(p);
    if (p->d->head) return;
    web_node_t *h = new_node(p, WN_ELEM);
    h->tag = T_HEAD; h->name = "head";
    append(p->d->html, h);
    p->d->head = h;
}

static void ensure_body(parser_t *p) {
    ensure_head(p);
    if (p->d->body) return;
    web_node_t *b = new_node(p, WN_ELEM);
    b->tag = T_BODY; b->name = "body";
    append(p->d->html, b);
    p->d->body = b;
    pop_to(p, 2);             /* document, html */
    push(p, b);
    p->in_body = true;
}

/* Where new body content goes: handles foster parenting for table contexts.
   Returns the parent and sets *before when the node must go before a table. */
static web_node_t *insertion_parent(parser_t *p, web_node_t **before) {
    *before = NULL;
    web_node_t *c = cur(p);
    if (is_table_ctx(c->tag)) {
        /* find the table and insert in front of it */
        for (int i = p->sp - 1; i >= 1; i--)
            if (p->stack[i]->tag == T_TABLE) {
                if (p->stack[i]->parent) { *before = p->stack[i]; return p->stack[i]->parent; }
                break;
            }
    }
    return c;
}

static void add_text(parser_t *p, const char *s, size_t n) {
    if (!n) return;
    bool ws = true;
    for (size_t i = 0; i < n; i++) if (!web_isspace((unsigned char)s[i])) { ws = false; break; }
    if (!p->in_body) {
        if (ws) return;
        ensure_body(p);
    }
    web_node_t *before;
    web_node_t *parent = insertion_parent(p, &before);
    if (before && ws) return;               /* whitespace inside table structure */
    if (!before && is_table_ctx(cur(p)->tag) && ws) return;
    if (parent->tag == T_SELECT && ws) return;
    /* merge with a previous text node */
    web_node_t *last = before ? before->prev : parent->last;
    int len;
    char *t = decode_text(&p->d->ar, s, n, &len);
    if (!t) return;
    if (last && last->type == WN_TEXT) {
        char *m = wa_alloc(&p->d->ar, (size_t)last->len + len + 1);
        if (!m) return;
        memcpy(m, last->text, last->len);
        memcpy(m + last->len, t, len);
        last->text = m;
        last->len += len;
        return;
    }
    web_node_t *tn = new_node(p, WN_TEXT);
    if (!tn) return;
    tn->text = t;
    tn->len = len;
    if (before) insert_before(before, tn); else append(parent, tn);
}

const char *web_attr(const web_node_t *n, const char *name) {
    if (!n) return NULL;
    for (int i = 0; i < n->nattrs; i++)
        if (!strcmp(n->attrs[i].name, name)) return n->attrs[i].value;
    return NULL;
}

bool web_has_attr(const web_node_t *n, const char *name) { return web_attr(n, name) != NULL; }

/* ---- tokenizer ----------------------------------------------------------------------- */
typedef struct tag_tok {
    char name[32];
    bool end, selfclose;
    web_attr_t attrs[48];
    int nattrs;
} tag_tok_t;

static bool name_char(int c) {
    return c && !web_isspace(c) && c != '>' && c != '/' && c != '=' && c != '"' && c != '\'' && c != '<';
}

/* Parse a tag at p->i (pointing at '<'). Returns false if it is not a tag. */
static bool read_tag(parser_t *p, tag_tok_t *t) {
    const char *s = p->s;
    size_t n = p->n, i = p->i + 1;
    t->end = false; t->selfclose = false; t->nattrs = 0;
    if (i < n && s[i] == '/') { t->end = true; i++; }
    if (i >= n || !isalpha((unsigned char)s[i])) return false;
    size_t k = 0;
    while (i < n && name_char((unsigned char)s[i])) {
        if (k < sizeof t->name - 1) t->name[k++] = (char)tolower((unsigned char)s[i]);
        i++;
    }
    t->name[k] = 0;
    /* attributes */
    while (i < n) {
        while (i < n && (web_isspace((unsigned char)s[i]) || s[i] == '/')) {
            if (s[i] == '/' && i + 1 < n && s[i + 1] == '>') t->selfclose = true;
            i++;
        }
        if (i >= n) break;
        if (s[i] == '>') { i++; break; }
        if (s[i] == '<') break;   /* broken tag: let the next one start here */
        size_t ns = i;
        while (i < n && name_char((unsigned char)s[i])) i++;
        if (i == ns) { i++; continue; }   /* stray quote / '=' */
        size_t ne = i;
        while (i < n && web_isspace((unsigned char)s[i])) i++;
        const char *vs = NULL;
        size_t vl = 0;
        if (i < n && s[i] == '=') {
            i++;
            while (i < n && web_isspace((unsigned char)s[i])) i++;
            if (i < n && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                vs = s + i;
                while (i < n && s[i] != q) i++;
                vl = (size_t)(s + i - vs);
                if (i < n) i++;
            } else {
                vs = s + i;
                while (i < n && !web_isspace((unsigned char)s[i]) && s[i] != '>') i++;
                vl = (size_t)(s + i - vs);
            }
        }
        if (t->nattrs < ARRAY_SIZE(t->attrs) && !t->end) {
            char *nm = wa_strndup(&p->d->ar, s + ns, ne - ns);
            if (!nm) break;
            for (char *c = nm; *c; c++) *c = (char)tolower((unsigned char)*c);
            bool dup = false;
            for (int a = 0; a < t->nattrs; a++) if (!strcmp(t->attrs[a].name, nm)) dup = true;
            if (!dup) {
                t->attrs[t->nattrs].name = nm;
                t->attrs[t->nattrs].value = vs ? decode_text(&p->d->ar, vs, vl, NULL) : "";
                t->nattrs++;
            }
        }
    }
    p->i = i;
    return true;
}

/* Skip raw text until </name> (case-insensitive); returns the text span. */
static void raw_text(parser_t *p, const char *name, const char **ts, size_t *tl) {
    size_t nl = strlen(name);
    size_t i = p->i;
    *ts = p->s + i;
    while (i < p->n) {
        if (p->s[i] == '<' && i + 2 + nl <= p->n && p->s[i + 1] == '/' && !strncasecmp(p->s + i + 2, name, nl) &&
            (i + 2 + nl == p->n || !isalnum((unsigned char)p->s[i + 2 + nl]))) {
            *tl = i - p->i;
            /* skip the end tag */
            while (i < p->n && p->s[i] != '>') i++;
            p->i = i < p->n ? i + 1 : i;
            return;
        }
        i++;
    }
    *tl = i - p->i;
    p->i = i;
}

/* Skip an element's whole subtree (svg, math, template): nested same-name tags counted. */
static void skip_subtree(parser_t *p, const char *name) {
    size_t nl = strlen(name);
    int depth = 1;
    size_t i = p->i;
    while (i < p->n && depth > 0) {
        if (p->s[i] == '<') {
            bool end = i + 1 < p->n && p->s[i + 1] == '/';
            size_t j = i + (end ? 2 : 1);
            if (j + nl <= p->n && !strncasecmp(p->s + j, name, nl) && (j + nl == p->n || !isalnum((unsigned char)p->s[j + nl]) )) {
                /* find the end of this tag, note self-closing */
                size_t k = j + nl;
                while (k < p->n && p->s[k] != '>') k++;
                bool selfc = k > 0 && p->s[k - 1] == '/';
                if (end) depth--; else if (!selfc) depth++;
                i = k < p->n ? k + 1 : k;
                continue;
            }
        }
        i++;
    }
    p->i = i;
}

static void set_attrs(parser_t *p, web_node_t *e, tag_tok_t *t) {
    if (!t->nattrs) return;
    e->attrs = wa_alloc(&p->d->ar, sizeof(web_attr_t) * t->nattrs);
    if (!e->attrs) return;
    memcpy(e->attrs, t->attrs, sizeof(web_attr_t) * t->nattrs);
    e->nattrs = t->nattrs;
    e->id = web_attr(e, "id");
    if (e->id && !*e->id) e->id = NULL;
    const char *c = web_attr(e, "class");
    if (c && *c) {
        int cnt = 0;
        for (const char *q = c; *q;) {
            while (*q && web_isspace((unsigned char)*q)) q++;
            if (!*q) break;
            cnt++;
            while (*q && !web_isspace((unsigned char)*q)) q++;
        }
        if (cnt) {
            e->cls = wa_alloc(&p->d->ar, sizeof(char *) * cnt);
            if (!e->cls) return;
            int k = 0;
            for (const char *q = c; *q && k < cnt;) {
                while (*q && web_isspace((unsigned char)*q)) q++;
                const char *st = q;
                while (*q && !web_isspace((unsigned char)*q)) q++;
                if (q > st) e->cls[k++] = wa_strndup(&p->d->ar, st, (size_t)(q - st));
            }
            e->ncls = k;
        }
    }
}

static void start_tag(parser_t *p, tag_tok_t *t) {
    web_doc_t *d = p->d;
    int tag = tag_lookup(t->name);
    if (tag == T_HTML) { ensure_html(p); return; }
    if (tag == T_HEAD) { ensure_head(p); return; }
    if (tag == T_BODY) {
        if (!d->body) {
            ensure_body(p);
            set_attrs(p, d->body, t);
        } else if (!d->body->nattrs) {
            set_attrs(p, d->body, t);
        }
        return;
    }
    if (tag == T_FRAMESET || tag == T_FRAME) {
        /* no frames: show a link to each frame instead */
        if (tag == T_FRAME) {
            const char *src = NULL;
            for (int i = 0; i < t->nattrs; i++) if (!strcmp(t->attrs[i].name, "src")) src = t->attrs[i].value;
            if (src) {
                ensure_body(p);
                web_node_t *a = new_node(p, WN_ELEM);
                a->tag = T_A; a->name = "a";
                web_attr_t *at = wa_alloc(&d->ar, sizeof *at);
                if (!a || !at) return;
                at->name = "href"; at->value = src;
                a->attrs = at; a->nattrs = 1;
                web_node_t *dv = new_node(p, WN_ELEM);
                dv->tag = T_DIV; dv->name = "div";
                append(cur(p), dv);
                append(dv, a);
                web_node_t *tx = new_node(p, WN_TEXT);
                char buf[300];
                snprintf(buf, sizeof buf, "Frame: %s", src);
                tx->text = wa_strndup(&d->ar, buf, strlen(buf));
                tx->len = (int)strlen(buf);
                append(a, tx);
            }
        }
        return;
    }

    bool to_head = !p->in_body && head_tag(tag);
    if (to_head) {
        ensure_head(p);
    } else {
        ensure_body(p);
        web_node_t *c = cur(p);
        /* implied end tags */
        if (closes_p(tag)) {
            int i = find_open(p, T_P, scope_stop);
            if (i > 0) pop_to(p, i);
        }
        switch (tag) {
        case T_LI: { int i = find_open(p, T_LI, list_stop); if (i > 0) pop_to(p, i); break; }
        case T_DD: case T_DT: {
            int i = find_open(p, T_DD, dl_stop); if (i > 0) pop_to(p, i);
            i = find_open(p, T_DT, dl_stop); if (i > 0) pop_to(p, i);
            break;
        }
        case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
            c = cur(p);
            if (c->tag >= T_H1 && c->tag <= T_H6) pop_to(p, p->sp - 1);
            break;
        case T_OPTION: if (cur(p)->tag == T_OPTION) pop_to(p, p->sp - 1); break;
        case T_OPTGROUP:
            if (cur(p)->tag == T_OPTION) pop_to(p, p->sp - 1);
            if (cur(p)->tag == T_OPTGROUP) pop_to(p, p->sp - 1);
            break;
        case T_SELECT: { int i = find_open(p, T_SELECT, scope_stop); if (i > 0) pop_to(p, i); break; }
        case T_A: { int i = find_open(p, T_A, scope_stop); if (i > 0) pop_to(p, i); break; }
        case T_BUTTON: { int i = find_open(p, T_BUTTON, scope_stop); if (i > 0) pop_to(p, i); break; }
        case T_NOBR: { int i = find_open(p, T_NOBR, scope_stop); if (i > 0) pop_to(p, i); break; }
        case T_TR: {
            int ti = find_open(p, T_TABLE, NULL);
            if (ti < 0) return;                       /* stray <tr> */
            int gi = -1;
            for (int i = p->sp - 1; i > ti; i--) {
                int tt = p->stack[i]->tag;
                if (tt == T_TBODY || tt == T_THEAD || tt == T_TFOOT) { gi = i; break; }
            }
            pop_to(p, (gi > 0 ? gi : ti) + 1);
            break;
        }
        case T_TD: case T_TH: {
            int ti = find_open(p, T_TABLE, NULL);
            if (ti < 0) return;                       /* stray cell */
            int ri = find_open(p, T_TR, cell_stop);
            if (ri < 0) {
                /* implied <tr> */
                int gi = -1;
                for (int i = p->sp - 1; i > ti; i--) {
                    int tt = p->stack[i]->tag;
                    if (tt == T_TBODY || tt == T_THEAD || tt == T_TFOOT) { gi = i; break; }
                }
                pop_to(p, (gi > 0 ? gi : ti) + 1);
                web_node_t *tr = new_node(p, WN_ELEM);
                tr->tag = T_TR; tr->name = "tr";
                append(cur(p), tr);
                push(p, tr);
            } else {
                pop_to(p, ri + 1);
            }
            break;
        }
        case T_TBODY: case T_THEAD: case T_TFOOT: case T_CAPTION: case T_COLGROUP: {
            int ti = find_open(p, T_TABLE, NULL);
            if (ti < 0) return;
            pop_to(p, ti + 1);
            break;
        }
        case T_COL: {
            int ti = find_open(p, T_TABLE, NULL);
            if (ti < 0) return;
            break;
        }
        case T_FORM:
            if (p->form) return;                       /* nested forms are ignored */
            break;
        default:
            break;
        }
    }

    web_node_t *e = new_node(p, WN_ELEM);
    if (!e) return;
    e->tag = (uint16_t)tag;
    e->name = tag ? tag_names[tag] : wa_strndup(&d->ar, t->name, strlen(t->name));
    set_attrs(p, e, t);
    d->nelems++;

    web_node_t *before = NULL, *parent;
    if (to_head) parent = d->head;
    else if (tag == T_TR || tag == T_TD || tag == T_TH || tag == T_TBODY || tag == T_THEAD || tag == T_TFOOT ||
             tag == T_CAPTION || tag == T_COL || tag == T_COLGROUP)
        parent = cur(p);
    else if (tag == T_FORM && is_table_ctx(cur(p)->tag)) parent = cur(p);
    else if (tag == T_INPUT && is_table_ctx(cur(p)->tag)) {
        const char *ty = web_attr(e, "type");
        parent = (ty && !strcasecmp(ty, "hidden")) ? cur(p) : insertion_parent(p, &before);
    } else parent = insertion_parent(p, &before);
    if (before) insert_before(before, e); else append(parent, e);

    if (tag == T_INPUT || tag == T_BUTTON || tag == T_SELECT || tag == T_TEXTAREA) e->form = p->form;

    if (tag == T_FORM) {
        p->form = e;
        if (is_table_ctx(parent->tag)) return;       /* forms in tables stay empty */
    }
    if (tag == T_BASE && !d->base[0]) {
        const char *h = web_attr(e, "href");
        if (h) strlcpy(d->base, h, sizeof d->base);
    }
    if (tag == T_META && !d->refresh[0]) {
        const char *he = web_attr(e, "http-equiv");
        const char *co = web_attr(e, "content");
        if (he && co && !strcasecmp(he, "refresh")) {
            const char *u = strcasestr(co, "url=");
            if (u) {
                u += 4;
                while (*u == ' ' || *u == '\'' || *u == '"') u++;
                strlcpy(d->refresh, u, sizeof d->refresh);
                size_t l = strlen(d->refresh);
                while (l && (d->refresh[l - 1] == '\'' || d->refresh[l - 1] == '"' || d->refresh[l - 1] == ' ')) d->refresh[--l] = 0;
            }
        }
    }

    /* raw text elements */
    if (tag == T_SCRIPT || tag == T_STYLE || tag == T_TITLE || tag == T_TEXTAREA || tag == T_XMP ||
        tag == T_IFRAME || tag == T_NOEMBED || tag == T_NOFRAMES) {
        if (t->selfclose && (tag == T_SCRIPT || tag == T_IFRAME)) return;
        const char *ts;
        size_t tl;
        raw_text(p, t->name, &ts, &tl);
        if (tag == T_SCRIPT || tag == T_IFRAME || tag == T_NOEMBED || tag == T_NOFRAMES) return;
        if (tag == T_TEXTAREA && tl && ts[0] == '\n') { ts++; tl--; }
        if (tl) {
            web_node_t *tn = new_node(p, WN_TEXT);
            if (!tn) return;
            if (tag == T_STYLE) { tn->text = wa_strndup(&d->ar, ts, tl); tn->len = (int)tl; }
            else tn->text = decode_text(&d->ar, ts, tl, &tn->len);
            append(e, tn);
        }
        if (tag == T_TITLE && !d->title[0]) web_text_content(e, d->title, sizeof d->title);
        return;
    }
    if (tag == T_SVG || tag == T_MATH || tag == T_TEMPLATE) {
        if (!t->selfclose) skip_subtree(p, t->name);
        return;
    }
    if (tag == T_PLAINTEXT) {
        add_text(p, p->s + p->i, p->n - p->i);
        p->i = p->n;
        return;
    }
    if (is_void(tag) || (t->selfclose && tag == T_UNKNOWN)) return;
    if (to_head) return;                 /* other head elements have no children */
    push(p, e);
    /* a newline right after <pre> / <listing> is ignored */
    if (tag == T_PRE && p->i < p->n && p->s[p->i] == '\n') p->i++;
    else if (tag == T_PRE && p->i + 1 < p->n && p->s[p->i] == '\r' && p->s[p->i + 1] == '\n') p->i += 2;
}

static void end_tag(parser_t *p, tag_tok_t *t) {
    int tag = tag_lookup(t->name);
    if (tag == T_BODY || tag == T_HTML || tag == T_HEAD) return;
    if (tag == T_BR) {               /* </br> acts like <br> */
        tag_tok_t bt;
        memset(&bt, 0, sizeof bt);
        strcpy(bt.name, "br");
        start_tag(p, &bt);
        return;
    }
    if (tag == T_FORM) {
        p->form = NULL;
        int i = find_open(p, T_FORM, scope_stop);
        if (i > 0) pop_to(p, i);
        return;
    }
    const int *stop = scope_stop;
    if (tag == T_TABLE) stop = NULL;
    else if (tag == T_TR || tag == T_TD || tag == T_TH || tag == T_TBODY || tag == T_THEAD || tag == T_TFOOT ||
             tag == T_CAPTION) stop = cell_stop;
    else if (tag == T_LI) stop = list_stop;
    else if (tag == T_OPTION || tag == T_OPTGROUP) stop = select_stop;
    for (int i = p->sp - 1; i >= 1; i--) {
        web_node_t *n = p->stack[i];
        bool match = tag ? n->tag == tag : (n->tag == T_UNKNOWN && n->name && !strcmp(n->name, t->name));
        if (match) {
            if (tag == T_TD || tag == T_TH) {
                /* the cell itself is popped; keep the row */
            }
            pop_to(p, i);
            return;
        }
        if (stop) {
            bool hit = false;
            for (const int *st = stop; *st; st++) if (n->tag == *st) hit = true;
            if (hit) return;
        }
        if (n->tag == T_BODY || n->tag == T_HTML) return;
    }
}

web_doc_t *web_parse_html(const char *src, size_t len, int charset) {
    web_doc_t *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    size_t tl;
    char *text = web_to_1252(src, len, charset, &tl);
    if (!text) { free(d); return NULL; }
    parser_t *p = calloc(1, sizeof *p);
    tag_tok_t *t = malloc(sizeof *t);
    if (!p || !t) { free(p); free(t); free(text); free(d); return NULL; }
    p->d = d;
    p->s = text;
    p->n = tl;
    d->root = new_node(p, WN_DOC);
    if (!d->root) { free(p); free(t); free(text); free(d); return NULL; }
    p->stack[0] = d->root;
    p->sp = 1;
    ensure_head(p);

    size_t text_start = 0;
    while (p->i < p->n) {
        const char *s = p->s;
        size_t i = p->i;
        if (s[i] != '<') { p->i++; continue; }
        /* flush text before the '<' */
        size_t ts = text_start;
        if (i + 3 < p->n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
            add_text(p, s + ts, i - ts);
            const char *e = NULL;
            for (size_t k = i + 4; k + 2 < p->n; k++)
                if (s[k] == '-' && s[k + 1] == '-' && s[k + 2] == '>') { e = s + k + 3; break; }
            p->i = e ? (size_t)(e - s) : p->n;
            text_start = p->i;
            continue;
        }
        if (i + 1 < p->n && (s[i + 1] == '!' || s[i + 1] == '?')) {
            add_text(p, s + ts, i - ts);
            size_t k = i + 2;
            if (i + 8 < p->n && !strncmp(s + i, "<![CDATA[", 9)) {
                const char *e = strstr(s + i + 9, "]]>");
                k = e ? (size_t)(e - s) + 2 : p->n;
            }
            while (k < p->n && s[k] != '>') k++;
            p->i = k < p->n ? k + 1 : k;
            text_start = p->i;
            continue;
        }
        size_t save = p->i;
        if (read_tag(p, t)) {
            add_text(p, s + ts, i - ts);
            if (t->end) end_tag(p, t); else start_tag(p, t);
            text_start = p->i;
        } else {
            p->i = save + 1;   /* a literal '<' */
        }
    }
    add_text(p, p->s + text_start, p->n - text_start);
    ensure_body(p);
    free(p);
    free(t);
    free(text);
    return d;
}

void web_doc_free(web_doc_t *d) {
    if (!d) return;
    wa_free(&d->ar);
    free(d);
}

static void text_rec(const web_node_t *n, char *out, size_t size, size_t *o, bool *sp) {
    for (const web_node_t *c = n->first; c; c = c->next) {
        if (c->type == WN_TEXT) {
            for (int i = 0; i < c->len && *o + 1 < size; i++) {
                unsigned char ch = (unsigned char)c->text[i];
                if (web_isspace(ch)) { *sp = true; continue; }
                if (*sp && *o) out[(*o)++] = ' ';
                if (*o + 1 >= size) break;
                *sp = false;
                out[(*o)++] = (char)ch;
            }
        } else if (c->type == WN_ELEM && c->tag != T_SCRIPT && c->tag != T_STYLE) {
            if (c->tag == T_BR) *sp = true;
            text_rec(c, out, size, o, sp);
        }
    }
}

void web_text_content(const web_node_t *n, char *out, size_t size) {
    size_t o = 0;
    bool sp = false;
    if (!size) return;
    text_rec(n, out, size, &o, &sp);
    out[o] = 0;
}
