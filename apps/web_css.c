/*
 * web_css.c - CSS for the Web Browser: stylesheet parser, selectors and the
 * cascade that gives every element its computed style.
 *
 * Supported: style attributes, <style> blocks and linked stylesheets (plus
 * @import), @media (screen / print, min/max-width/height, orientation,
 * prefers-color-scheme), @supports / @layer contents; selectors with type,
 * universal, #id, .class, [attr], [attr=v] (~= |= ^= $= *=), :first-child,
 * :last-child, :only-child, :nth-child(), :link, :root, :empty, :checked,
 * :disabled, :not(), :is() / :where(), descendant, child, + and ~
 * combinators; specificity, !important and source order; custom properties
 * and var(); the properties the layout uses (display, colours, fonts, text,
 * box model, borders, lists, tables, white-space, ...). Shorthands are
 * expanded while parsing. Old presentational attributes (bgcolor, align,
 * <font> ...) are mapped to styles too.
 */
#include "web.h"

/* ---- properties ---------------------------------------------------------------- */
enum {
    P_NONE = 0, P_DISPLAY, P_COLOR, P_BG_COLOR, P_FONT_SIZE, P_FONT_WEIGHT, P_FONT_STYLE,
    P_FONT_FAMILY, P_TEXT_ALIGN, P_TEXT_DECO_LINE, P_TEXT_DECO_COLOR, P_TEXT_TRANSFORM,
    P_LINE_HEIGHT, P_WHITE_SPACE, P_LIST_TYPE, P_LIST_POS, P_VALIGN, P_VISIBILITY,
    P_MT, P_MR, P_MB, P_ML, P_PT, P_PR, P_PB, P_PL,
    P_BTW, P_BRW, P_BBW, P_BLW, P_BTS, P_BRS, P_BBS, P_BLS, P_BTC, P_BRC, P_BBC, P_BLC,
    P_WIDTH, P_HEIGHT, P_MAX_WIDTH, P_MIN_WIDTH, P_POSITION, P_LEFT, P_TOP, P_FLOAT,
    P_BOX_SIZING, P_OVERFLOW, P_TEXT_INDENT, P_BORDER_COLLAPSE, P_BORDER_SPACING,
    P_FLEX_DIRECTION, P_FLEX_WRAP, P_CLIP, P_OPACITY, P_CLEAR,
    P_FLEX_GROW, P_FLEX_SHRINK, P_FLEX_BASIS, P_ORDER, P_JUSTIFY_CONTENT, P_ALIGN_ITEMS, P_COLUMN_GAP, P_ROW_GAP,
    P_TRANSFORM,
    P_LONGHAND_COUNT,
    /* shorthands (only used while parsing / for var()) */
    P_MARGIN, P_PADDING, P_BORDER, P_BORDER_TOP, P_BORDER_RIGHT, P_BORDER_BOTTOM, P_BORDER_LEFT,
    P_BORDER_WIDTH, P_BORDER_STYLE, P_BORDER_COLOR, P_BACKGROUND, P_FONT, P_LIST_STYLE,
    P_TEXT_DECORATION, P_FLEX_FLOW, P_OVERFLOW_X, P_OVERFLOW_Y, P_INSET, P_CUSTOM, P_FLEX, P_GAP,
};

static const struct { const char *name; int id; } prop_names[] = {
    {"display", P_DISPLAY}, {"color", P_COLOR}, {"background-color", P_BG_COLOR},
    {"font-size", P_FONT_SIZE}, {"font-weight", P_FONT_WEIGHT}, {"font-style", P_FONT_STYLE},
    {"font-family", P_FONT_FAMILY}, {"text-align", P_TEXT_ALIGN}, {"text-decoration-line", P_TEXT_DECO_LINE},
    {"text-decoration-color", P_TEXT_DECO_COLOR}, {"text-transform", P_TEXT_TRANSFORM},
    {"line-height", P_LINE_HEIGHT}, {"white-space", P_WHITE_SPACE}, {"list-style-type", P_LIST_TYPE},
    {"list-style-position", P_LIST_POS}, {"vertical-align", P_VALIGN}, {"visibility", P_VISIBILITY},
    {"margin-top", P_MT}, {"margin-right", P_MR}, {"margin-bottom", P_MB}, {"margin-left", P_ML},
    {"padding-top", P_PT}, {"padding-right", P_PR}, {"padding-bottom", P_PB}, {"padding-left", P_PL},
    {"border-top-width", P_BTW}, {"border-right-width", P_BRW}, {"border-bottom-width", P_BBW},
    {"border-left-width", P_BLW}, {"border-top-style", P_BTS}, {"border-right-style", P_BRS},
    {"border-bottom-style", P_BBS}, {"border-left-style", P_BLS}, {"border-top-color", P_BTC},
    {"border-right-color", P_BRC}, {"border-bottom-color", P_BBC}, {"border-left-color", P_BLC},
    {"width", P_WIDTH}, {"height", P_HEIGHT}, {"max-width", P_MAX_WIDTH}, {"min-width", P_MIN_WIDTH},
    {"position", P_POSITION}, {"left", P_LEFT}, {"top", P_TOP}, {"float", P_FLOAT},
    {"box-sizing", P_BOX_SIZING}, {"overflow", P_OVERFLOW}, {"text-indent", P_TEXT_INDENT},
    {"border-collapse", P_BORDER_COLLAPSE}, {"border-spacing", P_BORDER_SPACING},
    {"flex-direction", P_FLEX_DIRECTION}, {"flex-wrap", P_FLEX_WRAP}, {"clip", P_CLIP},
    {"opacity", P_OPACITY}, {"clear", P_CLEAR},
    {"flex-grow", P_FLEX_GROW}, {"flex-shrink", P_FLEX_SHRINK}, {"flex-basis", P_FLEX_BASIS}, {"order", P_ORDER},
    {"justify-content", P_JUSTIFY_CONTENT}, {"align-items", P_ALIGN_ITEMS}, {"column-gap", P_COLUMN_GAP},
    {"flex", P_FLEX}, {"gap", P_GAP}, {"grid-column-gap", P_COLUMN_GAP}, {"grid-gap", P_GAP},
    {"row-gap", P_ROW_GAP}, {"grid-row-gap", P_ROW_GAP}, {"transform", P_TRANSFORM},
    {"inset-inline-start", P_LEFT}, {"inset-block-start", P_TOP},
    {"margin", P_MARGIN}, {"padding", P_PADDING}, {"border", P_BORDER}, {"border-top", P_BORDER_TOP},
    {"border-right", P_BORDER_RIGHT}, {"border-bottom", P_BORDER_BOTTOM}, {"border-left", P_BORDER_LEFT},
    {"border-width", P_BORDER_WIDTH}, {"border-style", P_BORDER_STYLE}, {"border-color", P_BORDER_COLOR},
    {"background", P_BACKGROUND}, {"font", P_FONT}, {"list-style", P_LIST_STYLE},
    {"text-decoration", P_TEXT_DECORATION}, {"flex-flow", P_FLEX_FLOW}, {"overflow-x", P_OVERFLOW_X},
    {"overflow-y", P_OVERFLOW_Y}, {"inset", P_INSET},
    {"margin-inline-start", P_ML}, {"margin-inline-end", P_MR}, {"padding-inline-start", P_PL},
    {"padding-inline-end", P_PR}, {"margin-block-start", P_MT}, {"margin-block-end", P_MB},
    {"padding-block-start", P_PT}, {"padding-block-end", P_PB}, {"inline-size", P_WIDTH},
    {"max-inline-size", P_MAX_WIDTH}, {"block-size", P_HEIGHT},
};

static int prop_lookup(const char *s, size_t n) {
    if (n >= 2 && s[0] == '-' && s[1] == '-') return P_CUSTOM;
    for (int i = 0; i < ARRAY_SIZE(prop_names); i++)
        if (strlen(prop_names[i].name) == n && !strncasecmp(prop_names[i].name, s, n)) return prop_names[i].id;
    return P_NONE;
}

/* ---- values ---------------------------------------------------------------------- */
enum { CV_NONE = 0, CV_KW, CV_LEN, CV_NUM, CV_COLOR, CV_INHERIT, CV_INITIAL, CV_RAW, CV_STR };
enum { UN_PX = 0, UN_EM, UN_REM, UN_PCT, UN_PT, UN_VW, UN_VH, UN_EX, UN_CH, UN_NONE };

/* keywords */
enum {
    K_NONE = 1, K_AUTO, K_NORMAL, K_BOLD, K_BOLDER, K_LIGHTER, K_ITALIC, K_OBLIQUE, K_BLOCK, K_INLINE,
    K_INLINE_BLOCK, K_LIST_ITEM, K_TABLE, K_INLINE_TABLE, K_TABLE_ROW_GROUP, K_TABLE_HEADER_GROUP,
    K_TABLE_FOOTER_GROUP, K_TABLE_ROW, K_TABLE_CELL, K_TABLE_CAPTION, K_TABLE_COLUMN,
    K_TABLE_COLUMN_GROUP, K_FLEX, K_INLINE_FLEX, K_GRID, K_INLINE_GRID, K_CONTENTS, K_FLOW_ROOT,
    K_LEFT, K_RIGHT, K_CENTER, K_JUSTIFY, K_START, K_END, K_UNDERLINE, K_LINE_THROUGH, K_OVERLINE,
    K_UPPERCASE, K_LOWERCASE, K_CAPITALIZE, K_PRE, K_NOWRAP, K_PRE_WRAP, K_PRE_LINE, K_BREAK_SPACES,
    K_DISC, K_CIRCLE, K_SQUARE, K_DECIMAL, K_DECIMAL_LEADING_ZERO, K_LOWER_ALPHA, K_UPPER_ALPHA,
    K_LOWER_LATIN, K_UPPER_LATIN, K_LOWER_ROMAN, K_UPPER_ROMAN, K_INSIDE, K_OUTSIDE,
    K_BASELINE, K_TOP, K_MIDDLE, K_BOTTOM, K_SUB, K_SUPER, K_TEXT_TOP, K_TEXT_BOTTOM,
    K_VISIBLE, K_HIDDEN, K_COLLAPSE, K_SCROLL, K_CLIP,
    K_STATIC, K_RELATIVE, K_ABSOLUTE, K_FIXED, K_STICKY, K_BORDER_BOX, K_CONTENT_BOX,
    K_SEPARATE, K_ROW, K_ROW_REVERSE, K_COLUMN, K_COLUMN_REVERSE, K_WRAP, K_WRAP_REVERSE,
    K_SOLID, K_DASHED, K_DOTTED, K_DOUBLE, K_GROOVE, K_RIDGE, K_INSET, K_OUTSET,
    K_THIN, K_MEDIUM, K_THICK, K_XX_SMALL, K_X_SMALL, K_SMALL, K_LARGE, K_X_LARGE, K_XX_LARGE,
    K_XXX_LARGE, K_SMALLER, K_LARGER, K_TRANSPARENT, K_CURRENTCOLOR, K_BOTH, K_SMALL_CAPS,
    K_WEBKIT_CENTER, K_MATCH_PARENT, K_MAX_CONTENT, K_MIN_CONTENT, K_FIT_CONTENT,
    K_SPACE_BETWEEN, K_SPACE_AROUND, K_SPACE_EVENLY, K_FLEX_START, K_FLEX_END, K_STRETCH,
};
static const char *const kw_names[] = {
    "", "none", "auto", "normal", "bold", "bolder", "lighter", "italic", "oblique", "block", "inline",
    "inline-block", "list-item", "table", "inline-table", "table-row-group", "table-header-group",
    "table-footer-group", "table-row", "table-cell", "table-caption", "table-column",
    "table-column-group", "flex", "inline-flex", "grid", "inline-grid", "contents", "flow-root",
    "left", "right", "center", "justify", "start", "end", "underline", "line-through", "overline",
    "uppercase", "lowercase", "capitalize", "pre", "nowrap", "pre-wrap", "pre-line", "break-spaces",
    "disc", "circle", "square", "decimal", "decimal-leading-zero", "lower-alpha", "upper-alpha",
    "lower-latin", "upper-latin", "lower-roman", "upper-roman", "inside", "outside",
    "baseline", "top", "middle", "bottom", "sub", "super", "text-top", "text-bottom",
    "visible", "hidden", "collapse", "scroll", "clip",
    "static", "relative", "absolute", "fixed", "sticky", "border-box", "content-box",
    "separate", "row", "row-reverse", "column", "column-reverse", "wrap", "wrap-reverse",
    "solid", "dashed", "dotted", "double", "groove", "ridge", "inset", "outset",
    "thin", "medium", "thick", "xx-small", "x-small", "small", "large", "x-large", "xx-large",
    "xxx-large", "smaller", "larger", "transparent", "currentcolor", "both", "small-caps",
    "-webkit-center", "match-parent", "max-content", "min-content", "fit-content",
    "space-between", "space-around", "space-evenly", "flex-start", "flex-end", "stretch",
};

static int kw_lookup(const char *s, size_t n) {
    for (int i = 1; i < ARRAY_SIZE(kw_names); i++)
        if (strlen(kw_names[i]) == n && !strncasecmp(kw_names[i], s, n)) return i;
    if (n == 15 && !strncasecmp(s, "-moz-center", 11)) return K_WEBKIT_CENTER;
    return 0;
}

typedef struct cval {
    uint8_t t, unit;
    int16_t kw;
    float f;
    uint32_t color;
    const char *raw;
} cval_t;

typedef struct decl {
    uint16_t prop;
    uint8_t important;
    cval_t v;
    const char *name;         /* custom properties */
} decl_t;

/* ---- colours --------------------------------------------------------------------------- */
static const char named_colors[] =
    "aliceblue f0f8ff antiquewhite faebd7 aqua 00ffff aquamarine 7fffd4 azure f0ffff beige f5f5dc "
    "bisque ffe4c4 black 000000 blanchedalmond ffebcd blue 0000ff blueviolet 8a2be2 brown a52a2a "
    "burlywood deb887 cadetblue 5f9ea0 chartreuse 7fff00 chocolate d2691e coral ff7f50 "
    "cornflowerblue 6495ed cornsilk fff8dc crimson dc143c cyan 00ffff darkblue 00008b darkcyan 008b8b "
    "darkgoldenrod b8860b darkgray a9a9a9 darkgreen 006400 darkgrey a9a9a9 darkkhaki bdb76b "
    "darkmagenta 8b008b darkolivegreen 556b2f darkorange ff8c00 darkorchid 9932cc darkred 8b0000 "
    "darksalmon e9967a darkseagreen 8fbc8f darkslateblue 483d8b darkslategray 2f4f4f "
    "darkslategrey 2f4f4f darkturquoise 00ced1 darkviolet 9400d3 deeppink ff1493 deepskyblue 00bfff "
    "dimgray 696969 dimgrey 696969 dodgerblue 1e90ff firebrick b22222 floralwhite fffaf0 "
    "forestgreen 228b22 fuchsia ff00ff gainsboro dcdcdc ghostwhite f8f8ff gold ffd700 goldenrod daa520 "
    "gray 808080 green 008000 greenyellow adff2f grey 808080 honeydew f0fff0 hotpink ff69b4 "
    "indianred cd5c5c indigo 4b0082 ivory fffff0 khaki f0e68c lavender e6e6fa lavenderblush fff0f5 "
    "lawngreen 7cfc00 lemonchiffon fffacd lightblue add8e6 lightcoral f08080 lightcyan e0ffff "
    "lightgoldenrodyellow fafad2 lightgray d3d3d3 lightgreen 90ee90 lightgrey d3d3d3 lightpink ffb6c1 "
    "lightsalmon ffa07a lightseagreen 20b2aa lightskyblue 87cefa lightslategray 778899 "
    "lightslategrey 778899 lightsteelblue b0c4de lightyellow ffffe0 lime 00ff00 limegreen 32cd32 "
    "linen faf0e6 magenta ff00ff maroon 800000 mediumaquamarine 66cdaa mediumblue 0000cd "
    "mediumorchid ba55d3 mediumpurple 9370db mediumseagreen 3cb371 mediumslateblue 7b68ee "
    "mediumspringgreen 00fa9a mediumturquoise 48d1cc mediumvioletred c71585 midnightblue 191970 "
    "mintcream f5fffa mistyrose ffe4e1 moccasin ffe4b5 navajowhite ffdead navy 000080 oldlace fdf5e6 "
    "olive 808000 olivedrab 6b8e23 orange ffa500 orangered ff4500 orchid da70d6 palegoldenrod eee8aa "
    "palegreen 98fb98 paleturquoise afeeee palevioletred db7093 papayawhip ffefd5 peachpuff ffdab9 "
    "peru cd853f pink ffc0cb plum dda0dd powderblue b0e0e6 purple 800080 rebeccapurple 663399 "
    "red ff0000 rosybrown bc8f8f royalblue 4169e1 saddlebrown 8b4513 salmon fa8072 sandybrown f4a460 "
    "seagreen 2e8b57 seashell fff5ee sienna a0522d silver c0c0c0 skyblue 87ceeb slateblue 6a5acd "
    "slategray 708090 slategrey 708090 snow fffafa springgreen 00ff7f steelblue 4682b4 tan d2b48c "
    "teal 008080 thistle d8bfd8 tomato ff6347 turquoise 40e0d0 violet ee82ee wheat f5deb3 "
    "white ffffff whitesmoke f5f5f5 yellow ffff00 yellowgreen 9acd32 canvas ffffff canvastext 000000 "
    "linktext 0000ee visitedtext 551a8b buttonface efefef buttontext 000000 buttonborder 767676 "
    "graytext 808080 highlight 3390ff highlighttext ffffff field ffffff fieldtext 000000 "
    "window ffffff windowtext 000000 ";

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static float parse_num(const char **sp, bool *ok) {
    const char *s = *sp;
    char *e;
    float v = (float)strtod(s, &e);
    *ok = e != s;
    *sp = e;
    return v;
}

static uint32_t hsl_to_rgb(float h, float s, float l) {
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    s = CLAMP(s, 0.0f, 1.0f);
    l = CLAMP(l, 0.0f, 1.0f);
    float c = (1 - fabsf(2 * l - 1)) * s;
    float x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1));
    float m = l - c / 2, r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; } else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; } else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
    return RGB((int)((r + m) * 255 + 0.5f), (int)((g + m) * 255 + 0.5f), (int)((b + m) * 255 + 0.5f));
}

static bool parse_color_n(const char *s, size_t n, uint32_t *out) {
    while (n && web_isspace((unsigned char)*s)) { s++; n--; }
    while (n && web_isspace((unsigned char)s[n - 1])) n--;
    if (!n) return false;
    if (s[0] == '#') {
        int d[8], k = (int)n - 1;
        if (k != 3 && k != 4 && k != 6 && k != 8) return false;
        for (int i = 0; i < k; i++) if ((d[i] = hexv((unsigned char)s[i + 1])) < 0) return false;
        if (k == 3 || k == 4) {
            uint32_t a = k == 4 ? (uint32_t)(d[3] * 17) : 255;
            *out = RGBA(d[0] * 17, d[1] * 17, d[2] * 17, a);
        } else {
            uint32_t a = k == 8 ? (uint32_t)(d[6] * 16 + d[7]) : 255;
            *out = RGBA(d[0] * 16 + d[1], d[2] * 16 + d[3], d[4] * 16 + d[5], a);
        }
        return true;
    }
    if ((n > 4 && !strncasecmp(s, "rgb", 3)) || (n > 4 && !strncasecmp(s, "hsl", 3))) {
        bool hsl = (s[0] | 0x20) == 'h';
        const char *p = memchr(s, '(', n);
        if (!p) return false;
        p++;
        float v[4] = { 0, 0, 0, 1 };
        bool pct[4] = { false };
        int cnt = 0;
        const char *end = s + n;
        while (p < end && cnt < 4) {
            while (p < end && (web_isspace((unsigned char)*p) || *p == ',' || *p == '/')) p++;
            if (p >= end || *p == ')') break;
            bool ok;
            v[cnt] = parse_num(&p, &ok);
            if (!ok) return false;
            if (p < end && *p == '%') { pct[cnt] = true; p++; }
            else if (p + 2 < end && !strncasecmp(p, "deg", 3)) p += 3;
            cnt++;
        }
        if (cnt < 3) return false;
        float a = pct[3] ? v[3] / 100.0f : v[3];
        int ai = (int)(CLAMP(a, 0.0f, 1.0f) * 255 + 0.5f);
        if (hsl) {
            uint32_t c = hsl_to_rgb(v[0], v[1] / 100.0f, v[2] / 100.0f);
            *out = color_alpha(c, ai);
        } else {
            int r = (int)(pct[0] ? v[0] * 2.55f : v[0]), g = (int)(pct[1] ? v[1] * 2.55f : v[1]),
                b = (int)(pct[2] ? v[2] * 2.55f : v[2]);
            *out = RGBA(CLAMP(r, 0, 255), CLAMP(g, 0, 255), CLAMP(b, 0, 255), ai);
        }
        return true;
    }
    if (n == 11 && !strncasecmp(s, "transparent", 11)) { *out = 0; return true; }
    if (n > 24) return false;
    const char *p = named_colors;
    while (*p) {
        const char *sp = strchr(p, ' ');
        if ((size_t)(sp - p) == n && !strncasecmp(p, s, n)) {
            uint32_t v = (uint32_t)strtoul(sp + 1, NULL, 16);
            *out = 0xFF000000u | v;
            return true;
        }
        p = strchr(sp + 1, ' ') + 1;
    }
    return false;
}

bool web_css_color(const char *s, uint32_t *out) { return parse_color_n(s, strlen(s), out); }

/* ---- value parsing ------------------------------------------------------------------------ */
static bool parse_len(const char *s, size_t n, cval_t *v) {
    while (n && web_isspace((unsigned char)*s)) { s++; n--; }
    while (n && web_isspace((unsigned char)s[n - 1])) n--;
    if (!n) return false;
    char buf[64];
    if (n >= sizeof buf) return false;
    memcpy(buf, s, n);
    buf[n] = 0;
    /* calc() / min() / max() / clamp(): rough evaluation */
    if (!strncasecmp(buf, "calc(", 5) || !strncasecmp(buf, "-webkit-calc(", 13)) {
        const char *p = strchr(buf, '(') + 1;
        float px = 0, pc = 0;
        bool any = false, haspct = false;
        int sign = 1;
        while (*p && *p != ')') {
            while (*p == ' ' || *p == '(') p++;
            if (*p == '+') { sign = 1; p++; continue; }
            if (*p == '-' && p[1] == ' ') { sign = -1; p++; continue; }
            bool ok;
            float f = parse_num(&p, &ok);
            if (!ok) { while (*p && *p != ' ' && *p != ')') p++; continue; }
            if (*p == '%') { pc += sign * f; haspct = true; p++; }
            else if (!strncasecmp(p, "px", 2)) { px += sign * f; p += 2; }
            else if (!strncasecmp(p, "rem", 3)) { px += sign * f * 16; p += 3; }
            else if (!strncasecmp(p, "em", 2)) { px += sign * f * 16; p += 2; }
            else if (!strncasecmp(p, "vw", 2)) { pc += sign * f; haspct = true; p += 2; }
            else if (*p == ' ' || *p == ')' || !*p) {
                /* "* 2" or "/ 2" */
            }
            any = true;
            while (*p && *p != ' ' && *p != ')') p++;
            sign = 1;
            /* multiplication / division by a number */
            while (*p == ' ') p++;
            if (*p == '*' || *p == '/') {
                char op = *p++;
                while (*p == ' ') p++;
                float m = parse_num(&p, &ok);
                if (ok && m != 0) { if (op == '*') { px *= m; pc *= m; } else { px /= m; pc /= m; } }
            }
        }
        if (!any) return false;
        v->t = CV_LEN;
        if (haspct) { v->unit = UN_PCT; v->f = pc; } else { v->unit = UN_PX; v->f = px; }
        return true;
    }
    if (!strncasecmp(buf, "min(", 4) || !strncasecmp(buf, "max(", 4) || !strncasecmp(buf, "clamp(", 6)) {
        bool isclamp = (buf[0] | 0x20) == 'c', ismax = (buf[1] | 0x20) == 'a';
        char *p = strchr(buf, '(') + 1;
        char *args[3];
        int na = 0, depth = 0;
        args[na++] = p;
        for (char *q = p; *q; q++) {
            if (*q == '(') depth++;
            else if (*q == ')') { if (!depth) { *q = 0; break; } depth--; }
            else if (*q == ',' && !depth && na < 3) { *q = 0; args[na++] = q + 1; }
        }
        int pick = isclamp ? (na > 1 ? 1 : 0) : (ismax ? na - 1 : 0);
        /* prefer a plain px value if one exists (it does not depend on the viewport) */
        if (!isclamp) for (int i = 0; i < na; i++) if (strstr(args[i], "px") && !strchr(args[i], '%') && !strstr(args[i], "vw")) { pick = i; break; }
        return parse_len(args[pick], strlen(args[pick]), v);
    }
    const char *p = buf;
    bool ok;
    float f = parse_num(&p, &ok);
    if (!ok) return false;
    v->t = CV_LEN;
    v->f = f;
    if (!*p) {
        if (f == 0) { v->unit = UN_PX; return true; }
        v->t = CV_NUM; v->unit = UN_NONE;
        return true;
    }
    if (!strcasecmp(p, "px")) v->unit = UN_PX;
    else if (!strcasecmp(p, "em")) v->unit = UN_EM;
    else if (!strcasecmp(p, "rem")) v->unit = UN_REM;
    else if (!strcmp(p, "%")) v->unit = UN_PCT;
    else if (!strcasecmp(p, "pt")) v->unit = UN_PT;
    else if (!strcasecmp(p, "pc")) { v->unit = UN_PT; v->f *= 12; }
    else if (!strcasecmp(p, "in")) { v->unit = UN_PX; v->f *= 96; }
    else if (!strcasecmp(p, "cm")) { v->unit = UN_PX; v->f *= 37.8f; }
    else if (!strcasecmp(p, "mm")) { v->unit = UN_PX; v->f *= 3.78f; }
    else if (!strcasecmp(p, "vw") || !strcasecmp(p, "vmin") || !strcasecmp(p, "dvw") || !strcasecmp(p, "svw")) v->unit = UN_VW;
    else if (!strcasecmp(p, "vh") || !strcasecmp(p, "vmax") || !strcasecmp(p, "dvh") || !strcasecmp(p, "svh") || !strcasecmp(p, "lvh")) v->unit = UN_VH;
    else if (!strcasecmp(p, "ex")) v->unit = UN_EX;
    else if (!strcasecmp(p, "ch")) v->unit = UN_CH;
    else if (!strcasecmp(p, "q")) { v->unit = UN_PX; v->f *= 0.945f; }
    else return false;
    return true;
}

/* one component: keyword, length, number or colour */
static bool parse_component(const char *s, size_t n, cval_t *v) {
    memset(v, 0, sizeof *v);
    while (n && web_isspace((unsigned char)*s)) { s++; n--; }
    while (n && web_isspace((unsigned char)s[n - 1])) n--;
    if (!n) return false;
    if (n == 7 && !strncasecmp(s, "inherit", 7)) { v->t = CV_INHERIT; return true; }
    if ((n == 7 && !strncasecmp(s, "initial", 7)) || (n == 5 && !strncasecmp(s, "unset", 5)) ||
        (n == 6 && !strncasecmp(s, "revert", 6)) || (n == 12 && !strncasecmp(s, "revert-layer", 12))) {
        v->t = CV_INITIAL; return true;
    }
    int k = kw_lookup(s, n);
    if (k) {
        if (k == K_TRANSPARENT) { v->t = CV_COLOR; v->color = 0; v->kw = (int16_t)k; return true; }
        v->t = CV_KW; v->kw = (int16_t)k;
        return true;
    }
    if (parse_len(s, n, v)) return true;
    uint32_t c;
    if (parse_color_n(s, n, &c)) { v->t = CV_COLOR; v->color = c; return true; }
    return false;
}

/* Split a value into whitespace-separated components (functions kept whole). */
#define MAXCOMP 12
static int split_components(const char *s, size_t n, const char **cs, size_t *cl, bool slash) {
    int cnt = 0;
    size_t i = 0;
    while (i < n && cnt < MAXCOMP) {
        while (i < n && (web_isspace((unsigned char)s[i]) || s[i] == ',')) i++;
        if (i >= n) break;
        if (slash && s[i] == '/') { cs[cnt] = s + i; cl[cnt] = 1; cnt++; i++; continue; }
        size_t st = i;
        int depth = 0;
        char q = 0;
        while (i < n) {
            char c = s[i];
            if (q) { if (c == q) q = 0; i++; continue; }
            if (c == '"' || c == '\'') { q = c; i++; continue; }
            if (c == '(') depth++;
            else if (c == ')') { if (depth) depth--; }
            else if (!depth && (web_isspace((unsigned char)c) || c == ',' || (slash && c == '/'))) break;
            i++;
        }
        cs[cnt] = s + st;
        cl[cnt] = i - st;
        cnt++;
    }
    return cnt;
}

/* Output buffer for parse_decl. */
typedef struct dout { decl_t *d; int n, cap; } dout_t;

static void out_add(dout_t *o, int prop, const cval_t *v, bool imp) {
    if (o->n >= o->cap) return;
    decl_t *d = &o->d[o->n++];
    memset(d, 0, sizeof *d);
    d->prop = (uint16_t)prop;
    d->important = imp;
    d->v = *v;
}

static bool is_border_style(int kw) {
    return kw == K_NONE || kw == K_HIDDEN || (kw >= K_SOLID && kw <= K_OUTSET);
}

static void four(dout_t *o, const int props[4], const char *s, size_t n, bool imp) {
    const char *cs[MAXCOMP];
    size_t cl[MAXCOMP];
    int c = split_components(s, n, cs, cl, false);
    if (c < 1) return;
    cval_t v[4];
    for (int i = 0; i < c && i < 4; i++) if (!parse_component(cs[i], cl[i], &v[i])) return;
    if (c == 1) v[1] = v[2] = v[3] = v[0];
    else if (c == 2) { v[2] = v[0]; v[3] = v[1]; }
    else if (c == 3) v[3] = v[1];
    for (int i = 0; i < 4; i++) out_add(o, props[i], &v[i], imp);
}

static const int P_MARGINS[4] = { P_MT, P_MR, P_MB, P_ML };
static const int P_PADDINGS[4] = { P_PT, P_PR, P_PB, P_PL };
static const int P_BWIDTHS[4] = { P_BTW, P_BRW, P_BBW, P_BLW };
static const int P_BSTYLES[4] = { P_BTS, P_BRS, P_BBS, P_BLS };
static const int P_BCOLORS[4] = { P_BTC, P_BRC, P_BBC, P_BLC };

static void border_side(dout_t *o, int side, const char *s, size_t n, bool imp) {
    const char *cs[MAXCOMP];
    size_t cl[MAXCOMP];
    int c = split_components(s, n, cs, cl, false);
    cval_t w = { .t = CV_KW, .kw = K_MEDIUM }, st = { .t = CV_KW, .kw = K_NONE }, col = { .t = CV_KW, .kw = K_CURRENTCOLOR };
    for (int i = 0; i < c; i++) {
        cval_t v;
        if (!parse_component(cs[i], cl[i], &v)) continue;
        if (v.t == CV_INHERIT || v.t == CV_INITIAL) { w = st = col = v; break; }
        if (v.t == CV_KW && is_border_style(v.kw)) st = v;
        else if (v.t == CV_LEN || (v.t == CV_KW && (v.kw == K_THIN || v.kw == K_MEDIUM || v.kw == K_THICK))) w = v;
        else if (v.t == CV_COLOR || (v.t == CV_KW && v.kw == K_CURRENTCOLOR)) col = v;
    }
    for (int k = 0; k < 4; k++) {
        if (side >= 0 && k != side) continue;
        out_add(o, P_BWIDTHS[k], &w, imp);
        out_add(o, P_BSTYLES[k], &st, imp);
        out_add(o, P_BCOLORS[k], &col, imp);
    }
}

static bool font_family_mono(const char *s, size_t n) {
    char buf[200];
    size_t k = n < sizeof buf - 1 ? n : sizeof buf - 1;
    memcpy(buf, s, k);
    buf[k] = 0;
    return strcasestr(buf, "mono") || strcasestr(buf, "courier") || strcasestr(buf, "consolas") ||
           strcasestr(buf, "menlo") || strcasestr(buf, "monaco") || strcasestr(buf, "fixed");
}

/* Parse "prop: value" into longhand declarations. */
static void parse_decl(dout_t *o, int prop, const char *s, size_t n, bool imp) {
    while (n && web_isspace((unsigned char)*s)) { s++; n--; }
    while (n && web_isspace((unsigned char)s[n - 1])) n--;
    if (!n) return;
    cval_t v;
    memset(&v, 0, sizeof v);
    /* var() is resolved at cascade time */
    for (size_t i = 0; i + 4 <= n; i++)
        if (!strncasecmp(s + i, "var(", 4)) {
            v.t = CV_RAW;
            v.raw = s;            /* caller copies into the arena */
            v.f = (float)n;
            out_add(o, prop, &v, imp);
            return;
        }
    switch (prop) {
    case P_MARGIN: four(o, P_MARGINS, s, n, imp); return;
    case P_PADDING: four(o, P_PADDINGS, s, n, imp); return;
    case P_BORDER_WIDTH: four(o, P_BWIDTHS, s, n, imp); return;
    case P_BORDER_STYLE: four(o, P_BSTYLES, s, n, imp); return;
    case P_BORDER_COLOR: four(o, P_BCOLORS, s, n, imp); return;
    case P_BORDER: border_side(o, -1, s, n, imp); return;
    case P_BORDER_TOP: border_side(o, 0, s, n, imp); return;
    case P_BORDER_RIGHT: border_side(o, 1, s, n, imp); return;
    case P_BORDER_BOTTOM: border_side(o, 2, s, n, imp); return;
    case P_BORDER_LEFT: border_side(o, 3, s, n, imp); return;
    case P_INSET: {
        const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
        int c = split_components(s, n, cs, cl, false);
        if (c >= 1 && parse_component(cs[0], cl[0], &v)) out_add(o, P_TOP, &v, imp);
        if (c >= 4 && parse_component(cs[3], cl[3], &v)) out_add(o, P_LEFT, &v, imp);
        else if (c >= 2 && parse_component(cs[1], cl[1], &v)) out_add(o, P_LEFT, &v, imp);
        else if (c == 1) out_add(o, P_LEFT, &v, imp);
        return;
    }
    case P_BACKGROUND: {
        if (n == 4 && !strncasecmp(s, "none", 4)) { v.t = CV_COLOR; v.color = 0; out_add(o, P_BG_COLOR, &v, imp); return; }
        const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
        int c = split_components(s, n, cs, cl, false);
        cval_t col = { .t = CV_COLOR, .color = 0 };
        for (int i = 0; i < c; i++) {
            uint32_t cc;
            if (parse_color_n(cs[i], cl[i], &cc)) { col.color = cc; col.kw = 0; }
            else if (cl[i] == 7 && !strncasecmp(cs[i], "inherit", 7)) col.t = CV_INHERIT;
        }
        out_add(o, P_BG_COLOR, &col, imp);
        return;
    }
    case P_FONT: {
        const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
        int c = split_components(s, n, cs, cl, true);
        if (c == 1) {
            if (parse_component(cs[0], cl[0], &v) && (v.t == CV_INHERIT || v.t == CV_INITIAL)) {
                out_add(o, P_FONT_SIZE, &v, imp); out_add(o, P_FONT_WEIGHT, &v, imp);
                out_add(o, P_FONT_STYLE, &v, imp); out_add(o, P_LINE_HEIGHT, &v, imp);
            }
            return;   /* system fonts (caption, menu ...) */
        }
        cval_t weight = { .t = CV_KW, .kw = K_NORMAL }, style = { .t = CV_KW, .kw = K_NORMAL },
               lh = { .t = CV_KW, .kw = K_NORMAL };
        int i = 0;
        for (; i < c; i++) {
            if (!parse_component(cs[i], cl[i], &v)) break;
            if (v.t == CV_KW && (v.kw == K_BOLD || v.kw == K_BOLDER || v.kw == K_LIGHTER)) weight = v;
            else if (v.t == CV_NUM) weight = v;
            else if (v.t == CV_KW && (v.kw == K_ITALIC || v.kw == K_OBLIQUE)) style = v;
            else if (v.t == CV_KW && (v.kw == K_NORMAL || v.kw == K_SMALL_CAPS)) continue;
            else break;
        }
        if (i >= c || !parse_component(cs[i], cl[i], &v)) return;
        cval_t size = v;
        i++;
        if (i + 1 < c && cl[i] == 1 && cs[i][0] == '/') {
            if (parse_component(cs[i + 1], cl[i + 1], &v)) lh = v;
            i += 2;
        }
        out_add(o, P_FONT_SIZE, &size, imp);
        out_add(o, P_FONT_WEIGHT, &weight, imp);
        out_add(o, P_FONT_STYLE, &style, imp);
        out_add(o, P_LINE_HEIGHT, &lh, imp);
        if (i < c) {
            cval_t fam = { .t = CV_KW, .kw = font_family_mono(cs[i], (size_t)(s + n - cs[i])) ? 1 : 0 };
            fam.t = CV_STR;
            out_add(o, P_FONT_FAMILY, &fam, imp);
        }
        return;
    }
    case P_FONT_FAMILY: {
        cval_t fam = { .t = CV_STR, .kw = font_family_mono(s, n) ? 1 : 0 };
        if (n == 7 && !strncasecmp(s, "inherit", 7)) fam.t = CV_INHERIT;
        out_add(o, P_FONT_FAMILY, &fam, imp);
        return;
    }
    case P_LIST_STYLE: {
        const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
        int c = split_components(s, n, cs, cl, false);
        for (int i = 0; i < c; i++) {
            if (!parse_component(cs[i], cl[i], &v)) continue;
            if (v.t == CV_KW && (v.kw == K_INSIDE || v.kw == K_OUTSIDE)) out_add(o, P_LIST_POS, &v, imp);
            else if (v.t == CV_KW) out_add(o, P_LIST_TYPE, &v, imp);
        }
        return;
    }
    case P_TEXT_DECORATION: {
        const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
        int c = split_components(s, n, cs, cl, false);
        cval_t line = { .t = CV_KW, .kw = K_NONE };
        int lines = 0;
        for (int i = 0; i < c; i++) {
            if (!parse_component(cs[i], cl[i], &v)) continue;
            if (v.t == CV_KW && v.kw == K_UNDERLINE) lines |= DECO_UNDERLINE;
            else if (v.t == CV_KW && v.kw == K_LINE_THROUGH) lines |= DECO_STRIKE;
            else if (v.t == CV_KW && v.kw == K_OVERLINE) lines |= DECO_OVERLINE;
            else if (v.t == CV_COLOR) out_add(o, P_TEXT_DECO_COLOR, &v, imp);
            else if (v.t == CV_INHERIT) line = v;
        }
        if (line.t == CV_KW) { line.t = CV_NUM; line.f = (float)lines; }
        out_add(o, P_TEXT_DECO_LINE, &line, imp);
        return;
    }
    case P_TEXT_DECO_LINE: {
        const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
        int c = split_components(s, n, cs, cl, false);
        int lines = 0;
        for (int i = 0; i < c; i++) {
            if (!parse_component(cs[i], cl[i], &v)) continue;
            if (v.t == CV_KW && v.kw == K_UNDERLINE) lines |= DECO_UNDERLINE;
            else if (v.t == CV_KW && v.kw == K_LINE_THROUGH) lines |= DECO_STRIKE;
            else if (v.t == CV_KW && v.kw == K_OVERLINE) lines |= DECO_OVERLINE;
        }
        cval_t line = { .t = CV_NUM, .f = (float)lines };
        out_add(o, P_TEXT_DECO_LINE, &line, imp);
        return;
    }
    case P_FLEX_FLOW: {
        const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
        int c = split_components(s, n, cs, cl, false);
        for (int i = 0; i < c; i++) {
            if (!parse_component(cs[i], cl[i], &v) || v.t != CV_KW) continue;
            if (v.kw >= K_ROW && v.kw <= K_COLUMN_REVERSE) out_add(o, P_FLEX_DIRECTION, &v, imp);
            else out_add(o, P_FLEX_WRAP, &v, imp);
        }
        return;
    }
    case P_OVERFLOW_X: case P_OVERFLOW_Y: prop = P_OVERFLOW; break;
    case P_OVERFLOW: {
        const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
        int c = split_components(s, n, cs, cl, false);
        if (c >= 1 && parse_component(cs[0], cl[0], &v)) out_add(o, P_OVERFLOW, &v, imp);
        return;
    }
    case P_BORDER_SPACING: {
        const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
        int c = split_components(s, n, cs, cl, false);
        if (c >= 1 && parse_component(cs[0], cl[0], &v)) out_add(o, P_BORDER_SPACING, &v, imp);
        return;
    }
    case P_FLEX: {
        /* flex: none | auto | <grow> [<shrink>] [<basis>] | <basis> */
        const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
        int c = split_components(s, n, cs, cl, false);
        cval_t g = { .t = CV_NUM, .f = 0 }, sh = { .t = CV_NUM, .f = 1 }, b = { .t = CV_KW, .kw = K_AUTO };
        if (c == 1 && parse_component(cs[0], cl[0], &v) && v.t == CV_KW) {
            if (v.kw == K_AUTO) g.f = 1;
            else if (v.kw == K_NONE) sh.f = 0;
        } else if (c == 1 && (v.t == CV_INHERIT || v.t == CV_INITIAL)) {
            g = sh = b = v;
        } else {
            int nums = 0;
            bool basis = false;
            for (int i = 0; i < c && i < 3; i++) {
                bool plain = cl[i] > 0;
                for (size_t k = 0; k < cl[i]; k++)
                    if (!isdigit((unsigned char)cs[i][k]) && cs[i][k] != '.' && cs[i][k] != '-' && cs[i][k] != '+') plain = false;
                if (plain && nums < 2) {
                    float f = (float)strtod(cs[i], NULL);
                    if (nums++ == 0) g.f = f; else sh.f = f;
                } else if (parse_component(cs[i], cl[i], &v)) {
                    b = v;
                    basis = true;
                }
            }
            if (nums && !basis) b = (cval_t){ .t = CV_LEN, .unit = UN_PX, .f = 0 };
            if (!nums && basis) g.f = 1;
        }
        out_add(o, P_FLEX_GROW, &g, imp);
        out_add(o, P_FLEX_SHRINK, &sh, imp);
        out_add(o, P_FLEX_BASIS, &b, imp);
        return;
    }
    case P_TRANSFORM: {
        /* only one thing matters: is the element slid out of view (off-canvas menus)? */
        char buf[160];
        size_t k = 0;
        for (size_t i = 0; i < n && k < sizeof buf - 1; i++) if (!web_isspace((unsigned char)s[i])) buf[k++] = (char)tolower((unsigned char)s[i]);
        buf[k] = 0;
        bool off = false;
        const char *t = strstr(buf, "translate");
        if (t) {
            const char *a = strchr(t, '(');
            if (a) {
                a++;
                if (!strncmp(a, "calc(", 5)) a += 5;
                if (*a == '-') {
                    float f = (float)strtod(a + 1, NULL);
                    const char *u = a + 1;
                    while (*u && (isdigit((unsigned char)*u) || *u == '.')) u++;
                    if (*u == '%' && f >= 100) off = true;
                    else if (!strncmp(u, "px", 2) && f >= 500) off = true;
                    else if (!strncmp(u, "vw", 2) && f >= 100) off = true;
                }
            }
        }
        cval_t cv = { .t = CV_NUM, .f = off ? 1.0f : 0.0f };
        out_add(o, P_TRANSFORM, &cv, imp);
        return;
    }
    case P_GAP: {
        const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
        int c = split_components(s, n, cs, cl, false);
        if (c >= 1 && parse_component(cs[0], cl[0], &v)) out_add(o, P_ROW_GAP, &v, imp);
        if (c >= 1 && parse_component(cs[c >= 2 ? 1 : 0], cl[c >= 2 ? 1 : 0], &v)) out_add(o, P_COLUMN_GAP, &v, imp);
        return;
    }
    case P_CLIP: {
        /* only "rect(0 0 0 0)" style clipping matters (visually hidden text) */
        if (n > 5 && !strncasecmp(s, "rect(", 5)) {
            bool zero = true;
            for (size_t i = 5; i < n; i++) if (s[i] >= '1' && s[i] <= '9') zero = false;
            cval_t cv = { .t = CV_NUM, .f = zero ? 1.0f : 0.0f };
            out_add(o, P_CLIP, &cv, imp);
        }
        return;
    }
    default:
        break;
    }
    if (prop == P_VALIGN || prop == P_TEXT_INDENT || prop == P_LINE_HEIGHT || prop == P_FONT_SIZE ||
        prop == P_WIDTH || prop == P_HEIGHT) {
        /* single component, but allow e.g. "-webkit-fill-available" to fail cleanly */
    }
    if (!parse_component(s, n, &v)) return;
    out_add(o, prop, &v, imp);
}

/* ---- selectors -------------------------------------------------------------------------- */
enum { PS_FIRST = 1, PS_LAST = 2, PS_ONLY = 4, PS_LINK = 8, PS_ROOT = 16, PS_EMPTY = 32,
       PS_CHECKED = 64, PS_DISABLED = 128, PS_ENABLED = 256, PS_NEVER = 512, PS_NTH = 1024,
       PS_NTH_LAST = 2048 };

typedef struct attrsel { const char *name, *val; uint8_t op; uint8_t icase; } attrsel_t;
enum { AO_EXISTS = 0, AO_EQ, AO_WORD, AO_DASH, AO_PREFIX, AO_SUFFIX, AO_SUB };

typedef struct compound compound_t;
struct compound {
    uint16_t tag;
    uint8_t  any;             /* no type selector */
    uint8_t  comb;            /* combinator to the compound on the LEFT: 0, ' ', '>', '+', '~' */
    const char *tagname;      /* unknown element names */
    const char *id;
    const char **cls;
    uint8_t  ncls, nattrs, nnot, nis;
    attrsel_t *attrs;
    uint16_t pseudo;
    int16_t  nth_a, nth_b;
    compound_t *nots;         /* :not(...) - each must NOT match */
    compound_t *iss;          /* :is(...) - one must match */
};

typedef struct rule {
    compound_t *parts;        /* left to right */
    int      nparts;
    uint32_t spec;
    int      order;
    decl_t  *decls;
    int      ndecls;
    int      media;           /* index into the sheet's media list, -1 = always */
} rule_t;

typedef struct idx_node { const char *key; int rule; struct idx_node *next; } idx_node_t;
#define NBUCKETS 1024

struct web_sheet {
    web_arena_t ar;
    rule_t  *rules;
    int      nrules, cap;
    int      origin;
    char   **imports;
    int      nimports;
    char   **media;
    int      nmedia;
    idx_node_t *buckets[NBUCKETS];
    int     *universal;       /* rules without a key */
    int      nuniversal, ucap;
};

static uint32_t hash_str(const char *s, char prefix) {
    uint32_t h = 2166136261u ^ (uint8_t)prefix;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

/* Parse one compound at *pp. */
static bool parse_compound(web_sheet_t *sh, const char **pp, const char *end, compound_t *c, uint32_t *spec);

static bool parse_sel_list_in_parens(web_sheet_t *sh, const char **pp, const char *end, compound_t **out, uint8_t *nout, uint32_t *maxspec) {
    /* p points after '('; reads simple compounds separated by ',' up to the matching ')' */
    const char *p = *pp;
    compound_t tmp[8];
    int n = 0;
    *maxspec = 0;
    while (p < end) {
        while (p < end && (web_isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= end) return false;
        if (*p == ')') { p++; break; }
        uint32_t sp = 0;
        compound_t c;
        if (!parse_compound(sh, &p, end, &c, &sp)) return false;
        /* complex selectors inside :not() / :is() are not supported */
        while (p < end && web_isspace((unsigned char)*p)) p++;
        if (p < end && *p != ',' && *p != ')') return false;
        if (n < 8) tmp[n++] = c;
        if (sp > *maxspec) *maxspec = sp;
    }
    *out = wa_alloc(&sh->ar, sizeof(compound_t) * (n ? n : 1));
    if (!*out) return false;
    memcpy(*out, tmp, sizeof(compound_t) * n);
    *nout = (uint8_t)n;
    *pp = p;
    return true;
}

static bool ident_char(int c) { return isalnum(c) || c == '-' || c == '_' || c >= 0x80 || c == '\\'; }

static const char *read_ident(web_sheet_t *sh, const char **pp, const char *end, bool lower) {
    const char *p = *pp;
    char buf[128];
    size_t k = 0;
    while (p < end && ident_char((unsigned char)*p)) {
        if (*p == '\\' && p + 1 < end) {
            p++;
            if (isxdigit((unsigned char)*p)) {        /* \31 a -> hex escape */
                unsigned v = 0; int d = 0;
                while (p < end && isxdigit((unsigned char)*p) && d < 6) { v = v * 16 + (unsigned)hexv((unsigned char)*p); p++; d++; }
                if (p < end && *p == ' ') p++;
                if (k < sizeof buf - 1) buf[k++] = (char)(v < 256 ? v : '?');
                continue;
            }
        }
        if (k < sizeof buf - 1) buf[k++] = lower ? (char)tolower((unsigned char)*p) : *p;
        p++;
    }
    *pp = p;
    if (!k) return NULL;
    return wa_strndup(&sh->ar, buf, k);
}

static bool parse_nth(const char *s, const char *e, int16_t *a, int16_t *b) {
    while (s < e && web_isspace((unsigned char)*s)) s++;
    while (e > s && web_isspace((unsigned char)e[-1])) e--;
    char buf[32];
    size_t n = (size_t)(e - s);
    if (n >= sizeof buf) return false;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) if (!web_isspace((unsigned char)s[i])) buf[k++] = (char)tolower((unsigned char)s[i]);
    buf[k] = 0;
    if (!strcmp(buf, "odd")) { *a = 2; *b = 1; return true; }
    if (!strcmp(buf, "even")) { *a = 2; *b = 0; return true; }
    char *np = strchr(buf, 'n');
    if (!np) { *a = 0; *b = (int16_t)atoi(buf); return true; }
    *np = 0;
    if (!buf[0] || !strcmp(buf, "+")) *a = 1;
    else if (!strcmp(buf, "-")) *a = -1;
    else *a = (int16_t)atoi(buf);
    *b = np[1] ? (int16_t)atoi(np + 1) : 0;
    return true;
}

static bool parse_compound(web_sheet_t *sh, const char **pp, const char *end, compound_t *c, uint32_t *spec) {
    const char *p = *pp;
    memset(c, 0, sizeof *c);
    c->any = 1;
    const char *cls[16];
    int ncls = 0;
    attrsel_t attrs[8];
    int nattrs = 0;
    bool any = false;
    if (p < end && *p == '*') { p++; any = true; }
    else if (p < end && ident_char((unsigned char)*p)) {
        const char *nm = read_ident(sh, &p, end, true);
        if (!nm) return false;
        /* namespace prefix "svg|rect": not supported */
        if (p < end && *p == '|') return false;
        c->any = 0;
        int t = 0;
        for (int i = 1; i < T_COUNT; i++) if (!strcmp(web_tag_name(i), nm)) { t = i; break; }
        c->tag = (uint16_t)t;
        c->tagname = nm;
        *spec += 1;
        any = true;
    }
    while (p < end) {
        char ch = *p;
        if (ch == '#') {
            p++;
            const char *id = read_ident(sh, &p, end, false);
            if (!id) return false;
            c->id = id;
            *spec += 1 << 16;
            any = true;
        } else if (ch == '.') {
            p++;
            const char *cl = read_ident(sh, &p, end, false);
            if (!cl) return false;
            if (ncls < 16) cls[ncls++] = cl;
            *spec += 1 << 8;
            any = true;
        } else if (ch == '[') {
            p++;
            while (p < end && web_isspace((unsigned char)*p)) p++;
            const char *nm = read_ident(sh, &p, end, true);
            if (!nm) return false;
            while (p < end && web_isspace((unsigned char)*p)) p++;
            attrsel_t a = { nm, NULL, AO_EXISTS, 0 };
            if (p < end && *p != ']') {
                char op = *p;
                if (op == '=') { a.op = AO_EQ; p++; }
                else if (p + 1 < end && p[1] == '=') {
                    a.op = op == '~' ? AO_WORD : op == '|' ? AO_DASH : op == '^' ? AO_PREFIX :
                           op == '$' ? AO_SUFFIX : op == '*' ? AO_SUB : 255;
                    if (a.op == 255) return false;
                    p += 2;
                } else return false;
                while (p < end && web_isspace((unsigned char)*p)) p++;
                if (p < end && (*p == '"' || *p == '\'')) {
                    char q = *p++;
                    const char *vs = p;
                    while (p < end && *p != q) p++;
                    a.val = wa_strndup(&sh->ar, vs, (size_t)(p - vs));
                    if (p < end) p++;
                } else {
                    const char *vs = p;
                    while (p < end && *p != ']' && !web_isspace((unsigned char)*p)) p++;
                    a.val = wa_strndup(&sh->ar, vs, (size_t)(p - vs));
                }
                while (p < end && web_isspace((unsigned char)*p)) p++;
                if (p < end && (*p == 'i' || *p == 'I')) { a.icase = 1; p++; }
                else if (p < end && (*p == 's' || *p == 'S')) p++;
                while (p < end && web_isspace((unsigned char)*p)) p++;
            }
            if (p >= end || *p != ']') return false;
            p++;
            if (nattrs < 8) attrs[nattrs++] = a;
            *spec += 1 << 8;
            any = true;
        } else if (ch == ':') {
            p++;
            bool elem = false;
            if (p < end && *p == ':') { p++; elem = true; }
            const char *nm = read_ident(sh, &p, end, true);
            if (!nm) return false;
            any = true;
            if (elem || !strcmp(nm, "before") || !strcmp(nm, "after") || !strcmp(nm, "first-line") ||
                !strcmp(nm, "first-letter")) {
                c->pseudo |= PS_NEVER;        /* generated content is not supported */
                if (p < end && *p == '(') {   /* ::part(x) etc */
                    int depth = 0;
                    while (p < end) { if (*p == '(') depth++; else if (*p == ')' && --depth == 0) { p++; break; } p++; }
                }
                *spec += 1;
                continue;
            }
            *spec += 1 << 8;
            if (p < end && *p == '(') {
                p++;
                if (!strcmp(nm, "not")) {
                    compound_t *lst; uint8_t n; uint32_t ms;
                    if (!parse_sel_list_in_parens(sh, &p, end, &lst, &n, &ms)) return false;
                    c->nots = lst; c->nnot = n;
                    *spec += ms - (1 << 8);
                } else if (!strcmp(nm, "is") || !strcmp(nm, "matches") || !strcmp(nm, "where") ||
                           !strcmp(nm, "-webkit-any") || !strcmp(nm, "-moz-any")) {
                    compound_t *lst; uint8_t n; uint32_t ms;
                    if (!parse_sel_list_in_parens(sh, &p, end, &lst, &n, &ms)) return false;
                    c->iss = lst; c->nis = n;
                    *spec += (strcmp(nm, "where") ? ms : 0) - (1 << 8);
                } else {
                    const char *as = p;
                    int depth = 1;
                    while (p < end && depth) { if (*p == '(') depth++; else if (*p == ')') depth--; if (depth) p++; }
                    const char *ae = p;
                    if (p < end) p++;
                    if (!strcmp(nm, "nth-child") || !strcmp(nm, "nth-last-child")) {
                        /* "2n+1 of S" not supported */
                        bool of = false;
                        for (const char *q = as; q + 2 < ae; q++) if (!strncmp(q, " of ", 4)) of = true;
                        if (of || !parse_nth(as, ae, &c->nth_a, &c->nth_b)) c->pseudo |= PS_NEVER;
                        else c->pseudo |= nm[4] == 'l' ? PS_NTH_LAST : PS_NTH;
                    } else {
                        c->pseudo |= PS_NEVER;   /* :nth-of-type, :lang, :has ... */
                    }
                }
                continue;
            }
            if (!strcmp(nm, "first-child")) c->pseudo |= PS_FIRST;
            else if (!strcmp(nm, "last-child")) c->pseudo |= PS_LAST;
            else if (!strcmp(nm, "only-child")) c->pseudo |= PS_ONLY;
            else if (!strcmp(nm, "link") || !strcmp(nm, "any-link") || !strcmp(nm, "-webkit-any-link")) c->pseudo |= PS_LINK;
            else if (!strcmp(nm, "root")) c->pseudo |= PS_ROOT;
            else if (!strcmp(nm, "empty")) c->pseudo |= PS_EMPTY;
            else if (!strcmp(nm, "checked")) c->pseudo |= PS_CHECKED;
            else if (!strcmp(nm, "disabled")) c->pseudo |= PS_DISABLED;
            else if (!strcmp(nm, "enabled")) c->pseudo |= PS_ENABLED;
            else if (!strcmp(nm, "first-of-type")) c->pseudo |= PS_NEVER;
            else c->pseudo |= PS_NEVER;      /* :hover, :focus, :visited, :active ... */
        } else {
            break;
        }
    }
    if (!any) return false;
    if (ncls) {
        c->cls = wa_alloc(&sh->ar, sizeof(char *) * ncls);
        if (!c->cls) return false;
        memcpy(c->cls, cls, sizeof(char *) * ncls);
        c->ncls = (uint8_t)ncls;
    }
    if (nattrs) {
        c->attrs = wa_alloc(&sh->ar, sizeof(attrsel_t) * nattrs);
        if (!c->attrs) return false;
        memcpy(c->attrs, attrs, sizeof(attrsel_t) * nattrs);
        c->nattrs = (uint8_t)nattrs;
    }
    *pp = p;
    return true;
}

/* Parse one complex selector [s, e). */
static bool parse_selector(web_sheet_t *sh, const char *s, const char *e, compound_t **parts, int *nparts, uint32_t *spec) {
    compound_t tmp[24];
    int n = 0;
    const char *p = s;
    *spec = 0;
    uint8_t comb = 0;
    while (p < e) {
        while (p < e && web_isspace((unsigned char)*p)) { p++; if (n) comb = comb ? comb : ' '; }
        if (p >= e) break;
        if (*p == '>' || *p == '+' || *p == '~') {
            if (!n) return false;
            comb = (uint8_t)*p;
            p++;
            continue;
        }
        if (n >= 24) return false;
        if (!parse_compound(sh, &p, e, &tmp[n], spec)) return false;
        tmp[n].comb = n ? (comb ? comb : ' ') : 0;
        comb = 0;
        n++;
    }
    if (!n || comb) return false;
    *parts = wa_alloc(&sh->ar, sizeof(compound_t) * n);
    if (!*parts) return false;
    memcpy(*parts, tmp, sizeof(compound_t) * n);
    *nparts = n;
    return true;
}

/* ---- sheet parsing ------------------------------------------------------------------------- */
static char *strip_comments(const char *css, size_t len, size_t *outlen) {
    char *o = malloc(len + 1);
    if (!o) return NULL;
    size_t k = 0;
    char q = 0;
    for (size_t i = 0; i < len; i++) {
        char c = css[i];
        if (q) {
            o[k++] = c;
            if (c == '\\' && i + 1 < len) { o[k++] = css[++i]; continue; }
            if (c == q || c == '\n') q = 0;
            continue;
        }
        if (c == '"' || c == '\'') { q = c; o[k++] = c; continue; }
        if (c == '/' && i + 1 < len && css[i + 1] == '*') {
            const char *e = NULL;
            for (size_t j = i + 2; j + 1 < len; j++) if (css[j] == '*' && css[j + 1] == '/') { e = css + j; break; }
            i = e ? (size_t)(e - css) + 1 : len;
            o[k++] = ' ';
            continue;
        }
        if (c == '<' && i + 3 < len && !strncmp(css + i, "<!--", 4)) { i += 3; continue; }
        if (c == '-' && i + 2 < len && !strncmp(css + i, "-->", 3)) { i += 2; continue; }
        o[k++] = c;
    }
    o[k] = 0;
    *outlen = k;
    return o;
}

/* Find the matching '}' for the block starting after '{' at p. */
static const char *block_end(const char *p, const char *end) {
    int depth = 1;
    char q = 0;
    for (; p < end; p++) {
        if (q) { if (*p == '\\') { p++; continue; } if (*p == q) q = 0; continue; }
        if (*p == '"' || *p == '\'') { q = *p; continue; }
        if (*p == '{') depth++;
        else if (*p == '}') { if (--depth == 0) return p; }
    }
    return end;
}

static int parse_decl_block(web_sheet_t *sh, const char *s, const char *e, decl_t **out) {
    decl_t tmp[128];
    dout_t o = { tmp, 0, ARRAY_SIZE(tmp) };
    const char *p = s;
    while (p < e) {
        while (p < e && (web_isspace((unsigned char)*p) || *p == ';')) p++;
        if (p >= e) break;
        /* find the end of this declaration (';' outside parens/strings) */
        const char *ds = p;
        int depth = 0;
        char q = 0;
        while (p < e) {
            char c = *p;
            if (q) { if (c == '\\') { p += 2; continue; } if (c == q) q = 0; p++; continue; }
            if (c == '"' || c == '\'') q = c;
            else if (c == '(') depth++;
            else if (c == ')') { if (depth) depth--; }
            else if (c == '{') { p = block_end(p + 1, e); if (p < e) p++; break; }   /* nested rule: skip */
            else if (c == ';' && !depth) break;
            p++;
        }
        const char *de = p;
        const char *colon = memchr(ds, ':', (size_t)(de - ds));
        if (!colon) continue;
        const char *ns = ds, *ne = colon;
        while (ne > ns && web_isspace((unsigned char)ne[-1])) ne--;
        int prop = prop_lookup(ns, (size_t)(ne - ns));
        if (prop == P_NONE) continue;
        const char *vs = colon + 1, *ve = de;
        while (ve > vs && web_isspace((unsigned char)ve[-1])) ve--;
        bool imp = false;
        /* !important */
        for (const char *b = ve - 1; b >= vs; b--) {
            if (*b == '!') {
                const char *w = b + 1;
                while (w < ve && web_isspace((unsigned char)*w)) w++;
                if ((size_t)(ve - w) == 9 && !strncasecmp(w, "important", 9)) { imp = true; ve = b; }
                break;
            }
            if (!isalpha((unsigned char)*b) && !web_isspace((unsigned char)*b)) break;
        }
        while (ve > vs && web_isspace((unsigned char)ve[-1])) ve--;
        while (vs < ve && web_isspace((unsigned char)*vs)) vs++;
        if (prop == P_CUSTOM) {
            if (o.n < o.cap) {
                decl_t *d = &o.d[o.n++];
                memset(d, 0, sizeof *d);
                d->prop = P_CUSTOM;
                d->important = imp;
                d->name = wa_strndup(&sh->ar, ns, (size_t)(ne - ns));
                d->v.t = CV_STR;
                d->v.raw = wa_strndup(&sh->ar, vs, (size_t)(ve - vs));
            }
            continue;
        }
        int before = o.n;
        parse_decl(&o, prop, vs, (size_t)(ve - vs), imp);
        for (int i = before; i < o.n; i++)
            if (o.d[i].v.t == CV_RAW) {
                o.d[i].v.raw = wa_strndup(&sh->ar, vs, (size_t)(ve - vs));
                o.d[i].v.f = 0;
            }
    }
    if (!o.n) { *out = NULL; return 0; }
    *out = wa_alloc(&sh->ar, sizeof(decl_t) * o.n);
    if (!*out) return 0;
    memcpy(*out, tmp, sizeof(decl_t) * o.n);
    return o.n;
}

static void index_rule(web_sheet_t *sh, int ri) {
    rule_t *r = &sh->rules[ri];
    compound_t *c = &r->parts[r->nparts - 1];
    const char *key = NULL;
    char prefix = 0;
    if (c->id) { key = c->id; prefix = '#'; }
    else if (c->ncls) { key = c->cls[0]; prefix = '.'; }
    else if (!c->any) { key = c->tagname; prefix = 't'; }
    if (!key) {
        if (sh->nuniversal >= sh->ucap) {
            int nc = sh->ucap ? sh->ucap * 2 : 64;
            int *nu = realloc(sh->universal, sizeof(int) * nc);
            if (!nu) return;
            sh->universal = nu;
            sh->ucap = nc;
        }
        sh->universal[sh->nuniversal++] = ri;
        return;
    }
    idx_node_t *in = wa_alloc(&sh->ar, sizeof *in);
    if (!in) return;
    uint32_t h = hash_str(key, prefix) & (NBUCKETS - 1);
    in->key = key;
    in->rule = ri;
    /* keep rules in order inside a bucket: append */
    idx_node_t **pp = &sh->buckets[h];
    while (*pp) pp = &(*pp)->next;
    *pp = in;
    (void)prefix;
}

static void add_rules(web_sheet_t *sh, const char *sel, const char *sele, decl_t *decls, int nd, int media) {
    /* split the selector list on top-level commas */
    const char *p = sel;
    while (p < sele) {
        const char *s = p;
        int depth = 0;
        while (p < sele && (depth || *p != ',')) {
            if (*p == '(') depth++;
            else if (*p == ')' && depth) depth--;
            p++;
        }
        compound_t *parts;
        int np;
        uint32_t spec;
        if (parse_selector(sh, s, p, &parts, &np, &spec)) {
            bool never = false;
            for (int i = 0; i < np; i++) if (parts[i].pseudo & PS_NEVER) never = true;
            if (!never) {
                if (sh->nrules >= sh->cap) {
                    int nc = sh->cap ? sh->cap * 2 : 256;
                    rule_t *nr = realloc(sh->rules, sizeof(rule_t) * nc);
                    if (!nr) return;
                    sh->rules = nr;
                    sh->cap = nc;
                }
                rule_t *r = &sh->rules[sh->nrules];
                r->parts = parts;
                r->nparts = np;
                r->spec = spec;
                r->order = sh->nrules;
                r->decls = decls;
                r->ndecls = nd;
                r->media = media;
                index_rule(sh, sh->nrules);
                sh->nrules++;
            }
        }
        if (p < sele) p++;
    }
}

static void parse_rules(web_sheet_t *sh, const char *p, const char *end, int media) {
    while (p < end) {
        while (p < end && (web_isspace((unsigned char)*p) || *p == ';' || *p == '}')) p++;
        if (p >= end) break;
        if (*p == '@') {
            const char *as = p + 1;
            while (p < end && *p != '{' && *p != ';') p++;
            const char *ae = p;
            size_t al = (size_t)(ae - as);
            if (p >= end) break;
            if (*p == ';') {
                if (al > 6 && !strncasecmp(as, "import", 6)) {
                    /* @import url("x") [media] ; */
                    const char *q = as + 6;
                    while (q < ae && web_isspace((unsigned char)*q)) q++;
                    const char *us, *ue;
                    if (q + 4 <= ae && !strncasecmp(q, "url(", 4)) {
                        q += 4;
                        while (q < ae && (*q == ' ' || *q == '"' || *q == '\'')) q++;
                        us = q;
                        while (q < ae && *q != ')' && *q != '"' && *q != '\'') q++;
                        ue = q;
                        while (q < ae && *q != ')') q++;
                        if (q < ae) q++;
                    } else if (q < ae && (*q == '"' || *q == '\'')) {
                        char qq = *q++;
                        us = q;
                        while (q < ae && *q != qq) q++;
                        ue = q;
                        if (q < ae) q++;
                    } else { p++; continue; }
                    while (q < ae && web_isspace((unsigned char)*q)) q++;
                    bool print = (size_t)(ae - q) >= 5 && !strncasecmp(q, "print", 5);
                    if (!print && sh->nimports < 16 && ue > us) {
                        char **ni = realloc(sh->imports, sizeof(char *) * (sh->nimports + 1));
                        if (ni) { sh->imports = ni; sh->imports[sh->nimports++] = wa_strndup(&sh->ar, us, (size_t)(ue - us)); }
                    }
                }
                p++;
                continue;
            }
            /* block at-rule */
            const char *bs = p + 1;
            const char *be = block_end(bs, end);
            if (al >= 5 && !strncasecmp(as, "media", 5)) {
                char *mq = wa_strndup(&sh->ar, as + 5, al - 5);
                char **nm = realloc(sh->media, sizeof(char *) * (sh->nmedia + 1));
                if (nm && mq) {
                    sh->media = nm;
                    /* nested @media: combine with " and " is rare; the inner one wins */
                    sh->media[sh->nmedia] = mq;
                    parse_rules(sh, bs, be, sh->nmedia++);
                }
            } else if ((al >= 8 && !strncasecmp(as, "supports", 8)) || (al >= 5 && !strncasecmp(as, "layer", 5)) ||
                       (al >= 8 && !strncasecmp(as, "document", 8)) || (al >= 5 && !strncasecmp(as, "scope", 5))) {
                parse_rules(sh, bs, be, media);
            }
            /* @font-face, @keyframes, @page, @container ...: skipped */
            p = be < end ? be + 1 : end;
            continue;
        }
        const char *ss = p;
        while (p < end && *p != '{') p++;
        if (p >= end) break;
        const char *se = p;
        const char *bs = p + 1, *be = block_end(bs, end);
        while (se > ss && web_isspace((unsigned char)se[-1])) se--;
        decl_t *decls;
        int nd = parse_decl_block(sh, bs, be, &decls);
        if (nd > 0) add_rules(sh, ss, se, decls, nd, media);
        p = be < end ? be + 1 : end;
    }
}

web_sheet_t *web_css_parse(const char *css, size_t len, int origin) {
    web_sheet_t *sh = calloc(1, sizeof *sh);
    if (!sh) return NULL;
    sh->origin = origin;
    size_t n;
    char *text = strip_comments(css, len, &n);
    if (!text) { free(sh); return NULL; }
    /* keep the text alive in the arena (declarations point into it for var()) */
    char *kept = wa_strndup(&sh->ar, text, n);
    free(text);
    if (kept) parse_rules(sh, kept, kept + n, -1);
    return sh;
}

void web_css_free(web_sheet_t *s) {
    if (!s) return;
    free(s->rules);
    free(s->imports);
    free(s->media);
    free(s->universal);
    wa_free(&s->ar);
    free(s);
}

int web_css_import_count(const web_sheet_t *s) { return s ? s->nimports : 0; }
const char *web_css_import(const web_sheet_t *s, int i) { return s && i >= 0 && i < s->nimports ? s->imports[i] : NULL; }
int web_css_rule_count(const web_sheet_t *s) { return s ? s->nrules : 0; }

/* ---- media queries ----------------------------------------------------------------------------- */
static bool media_one(const char *q, const web_cascade_t *c) {
    while (*q == ' ') q++;
    bool neg = false;
    if (!strncasecmp(q, "only ", 5)) q += 5;
    if (!strncasecmp(q, "not ", 4)) { neg = true; q += 4; }
    bool ok = true;
    while (*q) {
        while (*q == ' ') q++;
        if (!*q) break;
        if (!strncasecmp(q, "and", 3) && (q[3] == ' ' || q[3] == '(')) { q += 3; continue; }
        if (*q == '(') {
            const char *e = strchr(q, ')');
            if (!e) break;
            char feat[64], val[64];
            const char *colon = memchr(q, ':', (size_t)(e - q));
            size_t fl = (size_t)((colon ? colon : e) - (q + 1));
            if (fl >= sizeof feat) fl = sizeof feat - 1;
            memcpy(feat, q + 1, fl);
            feat[fl] = 0;
            for (size_t i = fl; i > 0 && feat[i - 1] == ' '; i--) feat[i - 1] = 0;
            char *fp = feat;
            while (*fp == ' ') fp++;
            val[0] = 0;
            if (colon) {
                const char *vs = colon + 1;
                while (*vs == ' ') vs++;
                size_t vl = (size_t)(e - vs);
                if (vl >= sizeof val) vl = sizeof val - 1;
                memcpy(val, vs, vl);
                val[vl] = 0;
            }
            float px = (float)atof(val);
            if (strstr(val, "em")) px *= 16;
            bool r = true;
            if (!strcasecmp(fp, "min-width")) r = c->vw >= px;
            else if (!strcasecmp(fp, "max-width")) r = c->vw <= px;
            else if (!strcasecmp(fp, "min-height")) r = c->vh >= px;
            else if (!strcasecmp(fp, "max-height")) r = c->vh <= px;
            else if (!strcasecmp(fp, "orientation")) r = !strncasecmp(val, "landscape", 9) ? c->vw >= c->vh : c->vw < c->vh;
            else if (!strcasecmp(fp, "prefers-color-scheme")) r = !strncasecmp(val, "light", 5);
            else if (!strcasecmp(fp, "prefers-reduced-motion")) r = !strncasecmp(val, "no-pref", 7);
            else if (!strcasecmp(fp, "hover")) r = !strncasecmp(val, "hover", 5) || !val[0];
            else if (!strcasecmp(fp, "pointer") || !strcasecmp(fp, "any-pointer")) r = !strncasecmp(val, "fine", 4) || !val[0];
            else if (strstr(fp, "device-pixel-ratio") || strstr(fp, "resolution")) r = strstr(fp, "max") != NULL;
            else if (!strcasecmp(fp, "min-device-width")) r = 1024 >= px;
            else if (!strcasecmp(fp, "max-device-width")) r = 1024 <= px;
            else if (!strcasecmp(fp, "color")) r = true;
            else if (strchr(fp, '<') || strchr(fp, '>')) {
                /* range syntax: (width >= 600px), (400px <= width <= 700px) */
                const char *w = strcasestr(fp, "width");
                float v = 0;
                for (const char *t = fp; *t; t++) if (isdigit((unsigned char)*t)) { v = (float)atof(t); break; }
                if (w) {
                    bool ge = strstr(fp, ">=") || (strchr(fp, '>') && w < strchr(fp, '>'));
                    if (strchr(fp, '<') && w < strchr(fp, '<')) r = c->vw <= v;
                    else if (ge) r = c->vw >= v;
                    else r = c->vw <= v;
                }
            } else r = false;
            ok = ok && r;
            q = e + 1;
            continue;
        }
        /* media type */
        if (!strncasecmp(q, "screen", 6) || !strncasecmp(q, "all", 3)) { q += (q[0] | 0x20) == 's' ? 6 : 3; continue; }
        ok = false;   /* print, speech, tv ... */
        while (*q && *q != ' ') q++;
    }
    return neg ? !ok : ok;
}

static bool media_match(const char *q, const web_cascade_t *c) {
    char buf[512];
    strlcpy(buf, q, sizeof buf);
    for (char *p = buf; *p; p++) if (web_isspace((unsigned char)*p)) *p = ' ';   /* newlines in media="" */
    char *save = NULL;
    for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save))
        if (media_one(t, c)) return true;
    return false;
}

bool web_css_media(const char *query, int vw, int vh) {
    if (!query || !*query) return true;
    web_cascade_t c;
    memset(&c, 0, sizeof c);
    c.vw = vw;
    c.vh = vh;
    c.zoom = 100;
    return media_match(query, &c);
}

/* ---- matching ----------------------------------------------------------------------------------- */
static web_node_t *prev_elem(const web_node_t *n) {
    for (web_node_t *p = n->prev; p; p = p->prev) if (p->type == WN_ELEM) return p;
    return NULL;
}
static web_node_t *next_elem(const web_node_t *n) {
    for (web_node_t *p = n->next; p; p = p->next) if (p->type == WN_ELEM) return p;
    return NULL;
}

static bool has_class(const web_node_t *n, const char *c) {
    for (int i = 0; i < n->ncls; i++) if (!strcmp(n->cls[i], c)) return true;
    return false;
}

static bool attr_match(const web_node_t *n, const attrsel_t *a) {
    const char *v = web_attr(n, a->name);
    if (!v) return false;
    if (a->op == AO_EXISTS) return true;
    size_t vl = strlen(v), al = strlen(a->val);
    int (*cmp)(const char *, const char *, size_t) = a->icase ? strncasecmp : strncmp;
    switch (a->op) {
    case AO_EQ: return vl == al && !cmp(v, a->val, al);
    case AO_PREFIX: return al && vl >= al && !cmp(v, a->val, al);
    case AO_SUFFIX: return al && vl >= al && !cmp(v + vl - al, a->val, al);
    case AO_SUB:
        if (!al) return false;
        for (size_t i = 0; i + al <= vl; i++) if (!cmp(v + i, a->val, al)) return true;
        return false;
    case AO_DASH: return (vl == al || (vl > al && v[al] == '-')) && !cmp(v, a->val, al);
    case AO_WORD:
        for (const char *p = v; *p;) {
            while (*p && web_isspace((unsigned char)*p)) p++;
            const char *s = p;
            while (*p && !web_isspace((unsigned char)*p)) p++;
            if ((size_t)(p - s) == al && !cmp(s, a->val, al)) return true;
        }
        return false;
    }
    return false;
}

static int child_count_from_end(const web_node_t *n) {
    int k = 1;
    for (web_node_t *p = next_elem(n); p; p = next_elem(p)) k++;
    return k;
}

static bool nth_ok(int a, int b, int idx) {
    if (!a) return idx == b;
    int d = idx - b;
    return d % a == 0 && d / a >= 0;
}

static bool compound_match(const compound_t *c, const web_node_t *n) {
    if (n->type != WN_ELEM) return false;
    if (!c->any) {
        if (c->tag) { if (n->tag != c->tag) return false; }
        else if (!n->name || strcmp(n->name, c->tagname)) return false;
    }
    if (c->id && (!n->id || strcmp(n->id, c->id))) return false;
    for (int i = 0; i < c->ncls; i++) if (!has_class(n, c->cls[i])) return false;
    for (int i = 0; i < c->nattrs; i++) if (!attr_match(n, &c->attrs[i])) return false;
    if (c->pseudo) {
        uint16_t ps = c->pseudo;
        if (ps & PS_NEVER) return false;
        if ((ps & (PS_FIRST | PS_ONLY)) && prev_elem(n)) return false;
        if ((ps & (PS_LAST | PS_ONLY)) && next_elem(n)) return false;
        if ((ps & PS_LINK) && !((n->tag == T_A || n->tag == T_AREA) && web_attr(n, "href"))) return false;
        if ((ps & PS_ROOT) && n->tag != T_HTML) return false;
        if ((ps & PS_EMPTY) && n->first) return false;
        if ((ps & PS_CHECKED) && !web_attr(n, "checked") && !web_attr(n, "selected")) return false;
        if ((ps & PS_DISABLED) && !web_attr(n, "disabled")) return false;
        if ((ps & PS_ENABLED) && web_attr(n, "disabled")) return false;
        if ((ps & PS_NTH) && !nth_ok(c->nth_a, c->nth_b, n->index)) return false;
        if ((ps & PS_NTH_LAST) && !nth_ok(c->nth_a, c->nth_b, child_count_from_end(n))) return false;
    }
    for (int i = 0; i < c->nnot; i++) if (compound_match(&c->nots[i], n)) return false;
    if (c->nis) {
        bool any = false;
        for (int i = 0; i < c->nis && !any; i++) any = compound_match(&c->iss[i], n);
        if (!any) return false;
    }
    return true;
}

/* match parts[0..k] with parts[k] at n */
static bool match_from(const compound_t *parts, int k, const web_node_t *n) {
    if (!compound_match(&parts[k], n)) return false;
    if (k == 0) return true;
    switch (parts[k].comb) {
    case '>':
        return n->parent && n->parent->type == WN_ELEM && match_from(parts, k - 1, n->parent);
    case '+': {
        web_node_t *p = prev_elem(n);
        return p && match_from(parts, k - 1, p);
    }
    case '~':
        for (web_node_t *p = prev_elem(n); p; p = prev_elem(p)) if (match_from(parts, k - 1, p)) return true;
        return false;
    default:
        for (web_node_t *p = n->parent; p && p->type == WN_ELEM; p = p->parent)
            if (match_from(parts, k - 1, p)) return true;
        return false;
    }
}

/* ---- the default stylesheet ------------------------------------------------------------------- */
static const char ua_css[] =
    "html,address,blockquote,body,dd,div,dl,dt,fieldset,form,frameset,h1,h2,h3,h4,h5,h6,noframes,"
    "ol,p,ul,center,dir,hr,menu,pre,article,aside,footer,header,hgroup,main,nav,section,figure,"
    "figcaption,details,summary,legend,dialog,search,optgroup,xmp,plaintext,listing{display:block}"
    "li{display:list-item}"
    "head,script,style,title,meta,link,base,template,noembed,param,datalist,area,map,svg,math,canvas,"
    "video,audio,object,embed,iframe,source,track,rp,frame,col,colgroup,dialog:not([open]),[hidden],"
    "input[type=hidden],option,select optgroup{display:none}"
    "table{display:table;border-spacing:2px;box-sizing:border-box}"
    "tr{display:table-row;vertical-align:inherit}thead{display:table-header-group;vertical-align:middle}"
    "tbody{display:table-row-group;vertical-align:middle}tfoot{display:table-footer-group;vertical-align:middle}"
    "td,th{display:table-cell;padding:1px;vertical-align:inherit}th{font-weight:bold;text-align:center}"
    "caption{display:table-caption;text-align:center}"
    "body{margin:8px;line-height:normal}"
    "h1{font-size:2em;margin:.67em 0;font-weight:bold}"
    "h2{font-size:1.5em;margin:.83em 0;font-weight:bold}"
    "h3{font-size:1.17em;margin:1em 0;font-weight:bold}"
    "h4{margin:1.33em 0;font-weight:bold}"
    "h5{font-size:.83em;margin:1.67em 0;font-weight:bold}"
    "h6{font-size:.67em;margin:2.33em 0;font-weight:bold}"
    "p,dl{margin:1em 0}"
    "blockquote,figure{margin:1em 40px}"
    "ul,ol,menu,dir{margin:1em 0;padding-left:40px}"
    "ol{list-style-type:decimal}"
    "ul ul,ol ul,ul ol,ol ol,ul menu,ul dir{margin-top:0;margin-bottom:0}"
    "ul ul,ol ul,menu ul,ul menu{list-style-type:circle}"
    "ul ul ul,ul ol ul,ol ul ul,ol ol ul{list-style-type:square}"
    "dd{margin-left:40px}"
    "b,strong{font-weight:bold}"
    "i,em,cite,var,dfn,address{font-style:italic}"
    "pre,code,kbd,samp,tt,xmp,plaintext,listing{font-family:monospace}"
    "pre,xmp,plaintext,listing{white-space:pre;margin:1em 0}"
    "u,ins{text-decoration:underline}"
    "s,strike,del{text-decoration:line-through}"
    "big{font-size:larger}small,sub,sup{font-size:smaller}"
    "sub{vertical-align:sub}sup{vertical-align:super}"
    "center{text-align:-webkit-center}"
    "hr{border-style:solid;border-width:1px;border-color:#aaa;margin:.5em auto;height:0}"
    "a:link{color:#0000ee;text-decoration:underline}"
    "mark{background-color:yellow;color:black}"
    "fieldset{margin:0 2px;padding:.35em .75em .625em;border:2px groove #c0c0c0}"
    "legend{padding:0 2px;font-weight:bold}"
    "nobr{white-space:nowrap}"
    "textarea{white-space:pre-wrap}"
    "button,input,select,textarea{display:inline-block;font-size:13px}"
    "noscript{display:block}";

/* quirks mode (no or a legacy doctype): tables do not inherit text properties */
static const char ua_quirks_css[] =
    "table{font-size:initial;font-weight:initial;font-style:initial;line-height:initial;"
    "white-space:initial;text-align:initial}";

static web_sheet_t *ua_sheet, *ua_quirks;

/* ---- the cascade --------------------------------------------------------------------------------- */
typedef struct mdecl { const decl_t *d; uint64_t key; } mdecl_t;

typedef struct cctx {
    const web_cascade_t *c;
    web_doc_t *doc;
    bool *media_ok[64];       /* per author sheet, then UA last */
    mdecl_t *m;
    int nm, mcap;
    int root_fs;
    uint32_t body_link;       /* <body link=...> */
    bool has_body_link;
    int depth;
} cctx_t;

static void m_add(cctx_t *cx, const decl_t *d, uint64_t key) {
    if (cx->nm >= cx->mcap) {
        int nc = cx->mcap ? cx->mcap * 2 : 256;
        mdecl_t *nm = realloc(cx->m, sizeof(mdecl_t) * nc);
        if (!nm) return;
        cx->m = nm;
        cx->mcap = nc;
    }
    cx->m[cx->nm].d = d;
    cx->m[cx->nm].key = key;
    cx->nm++;
}

static int mcmp(const void *a, const void *b) {
    uint64_t x = ((const mdecl_t *)a)->key, y = ((const mdecl_t *)b)->key;
    return x < y ? -1 : x > y;
}

/* origin ranks: 0 UA, 1 presentational hints, 2 author, 3 inline style */
static uint64_t make_key(const decl_t *d, int origin, uint32_t spec, int order) {
    uint64_t imp = d->important ? 1 : 0;
    uint64_t orank = (uint64_t)origin;
    if (imp && origin == 0) orank = 4;   /* UA !important beats everything */
    return (imp << 62) | (orank << 58) | ((uint64_t)(spec & 0xFFFFFF) << 30) | (uint64_t)(order & 0x3FFFFFFF);
}

static void match_sheet(cctx_t *cx, web_sheet_t *sh, int origin, int sheet_no, const web_node_t *n, bool *mok) {
    if (!sh) return;
    /* candidates: id bucket, class buckets, tag bucket, universal */
    int base_order = sheet_no << 20;
    const char *keys[20];
    char kp[20];
    int nk = 0;
    if (n->id) { keys[nk] = n->id; kp[nk++] = '#'; }
    for (int i = 0; i < n->ncls && nk < 18; i++) { keys[nk] = n->cls[i]; kp[nk++] = '.'; }
    if (n->name) { keys[nk] = n->name; kp[nk++] = 't'; }
    for (int k = 0; k < nk; k++) {
        uint32_t h = hash_str(keys[k], kp[k]) & (NBUCKETS - 1);
        for (idx_node_t *in = sh->buckets[h]; in; in = in->next) {
            if (strcmp(in->key, keys[k])) continue;
            const rule_t *r = &sh->rules[in->rule];
            /* make sure the rule's key kind matches (a class "p" vs tag "p") */
            const compound_t *last = &r->parts[r->nparts - 1];
            char kind = last->id ? '#' : last->ncls ? '.' : 't';
            if (kind != kp[k]) continue;
            if (kind == '.' && strcmp(last->cls[0], keys[k])) continue;
            /* a rule with several of the element's classes would be found twice: only
               accept it through its first class */
            if (r->media >= 0 && mok && !mok[r->media]) continue;
            if (!match_from(r->parts, r->nparts - 1, n)) continue;
            for (int i = 0; i < r->ndecls; i++) m_add(cx, &r->decls[i], make_key(&r->decls[i], origin, r->spec, base_order + r->order));
        }
    }
    for (int u = 0; u < sh->nuniversal; u++) {
        const rule_t *r = &sh->rules[sh->universal[u]];
        if (r->media >= 0 && mok && !mok[r->media]) continue;
        if (!match_from(r->parts, r->nparts - 1, n)) continue;
        for (int i = 0; i < r->ndecls; i++) m_add(cx, &r->decls[i], make_key(&r->decls[i], origin, r->spec, base_order + r->order));
    }
}

/* ---- applying values ---------------------------------------------------------------------------- */
static int to_px(cctx_t *cx, const web_style_t *st, const web_style_t *parent, const cval_t *v, int pct_base, bool for_font) {
    float f = v->f;
    int zoom = cx->c->zoom;
    switch (v->unit) {
    case UN_PX: return (int)lroundf(f * zoom / 100.0f);
    case UN_PT: return (int)lroundf(f * 4.0f / 3.0f * zoom / 100.0f);
    case UN_EM: return (int)lroundf(f * (for_font ? parent->fsize : st->fsize));
    case UN_EX: case UN_CH: return (int)lroundf(f * (for_font ? parent->fsize : st->fsize) * 0.5f);
    case UN_REM: return (int)lroundf(f * cx->root_fs);
    case UN_PCT: return (int)lroundf(f * pct_base / 100.0f);
    case UN_VW: return (int)lroundf(f * cx->c->vw / 100.0f);
    case UN_VH: return (int)lroundf(f * cx->c->vh / 100.0f);
    default: return (int)lroundf(f);
    }
}

static web_len_t to_len(cctx_t *cx, const web_style_t *st, const web_style_t *parent, const cval_t *v, bool allow_auto) {
    web_len_t l = { 0, U_PX };
    if (v->t == CV_KW) {
        if (v->kw == K_AUTO && allow_auto) { l.u = U_AUTO; return l; }
        if (v->kw == K_NONE) { l.u = U_NONE; return l; }
        if (v->kw == K_MAX_CONTENT || v->kw == K_FIT_CONTENT || v->kw == K_MIN_CONTENT) { l.u = U_AUTO; return l; }
        l.u = allow_auto ? U_AUTO : U_PX;
        return l;
    }
    if (v->t == CV_NUM) { l.v = (int16_t)CLAMP(lroundf(v->f * cx->c->zoom / 100.0f), -30000, 30000); return l; }
    if (v->t != CV_LEN) { l.u = allow_auto ? U_AUTO : U_PX; return l; }
    if (v->unit == UN_PCT) { l.u = U_PCT; l.v = (int16_t)CLAMP(lroundf(v->f * 10), -30000, 30000); return l; }   /* tenths of a percent */
    l.v = (int16_t)CLAMP(to_px(cx, st, parent, v, 0, false), -30000, 30000);
    return l;
}

static int font_kw_size(int kw, int parent) {
    switch (kw) {
    case K_XX_SMALL: return 9;
    case K_X_SMALL: return 10;
    case K_SMALL: return 13;
    case K_MEDIUM: return 16;
    case K_LARGE: return 18;
    case K_X_LARGE: return 24;
    case K_XX_LARGE: return 32;
    case K_XXX_LARGE: return 48;
    default: return parent;
    }
}

static int bwidth(cctx_t *cx, const web_style_t *st, const web_style_t *parent, const cval_t *v) {
    if (v->t == CV_KW) {
        int w = v->kw == K_THIN ? 1 : v->kw == K_THICK ? 5 : 3;
        return MAX(1, w * cx->c->zoom / 100);
    }
    if (v->t == CV_LEN) {
        int w = to_px(cx, st, parent, v, 0, false);
        if (w <= 0 && v->f > 0) w = 1;
        return CLAMP(w, 0, 200);
    }
    return 0;
}

static void apply_decl(cctx_t *cx, web_node_t *n, web_style_t *st, const web_style_t *ps, const decl_t *d, int pass);

static const char *var_lookup(const web_style_t *st, const char *name, size_t nl) {
    for (const web_var_t *v = st->vars; v; v = v->next)
        if (strlen(v->name) == nl && !strncmp(v->name, name, nl)) return v->value;
    return NULL;
}

/* Substitute var() in s into out (size). */
static bool subst_vars(const web_style_t *st, const char *s, char *out, size_t size, int depth) {
    size_t o = 0;
    if (depth > 6) return false;
    while (*s && o + 1 < size) {
        if (!strncasecmp(s, "var(", 4)) {
            const char *p = s + 4;
            while (*p == ' ') p++;
            const char *ns = p;
            while (*p && *p != ',' && *p != ')' && *p != ' ') p++;
            size_t nl = (size_t)(p - ns);
            while (*p == ' ') p++;
            const char *fb = NULL, *fe = NULL;
            if (*p == ',') {
                fb = p + 1;
                int dpt = 0;
                const char *q = fb;
                while (*q && (dpt || *q != ')')) { if (*q == '(') dpt++; else if (*q == ')') dpt--; q++; }
                fe = q;
                p = q;
            }
            /* skip to the closing paren */
            int dpt = 0;
            while (*p && (dpt || *p != ')')) { if (*p == '(') dpt++; else if (*p == ')') dpt--; p++; }
            if (*p == ')') p++;
            const char *val = var_lookup(st, ns, nl);
            char tmp[512];
            if (val) {
                if (!subst_vars(st, val, tmp, sizeof tmp, depth + 1)) return false;
            } else if (fb) {
                size_t fl = (size_t)(fe - fb);
                char fbuf[512];
                if (fl >= sizeof fbuf) fl = sizeof fbuf - 1;
                memcpy(fbuf, fb, fl);
                fbuf[fl] = 0;
                if (!subst_vars(st, fbuf, tmp, sizeof tmp, depth + 1)) return false;
            } else return false;
            size_t tl = strlen(tmp);
            if (o + tl >= size) return false;
            memcpy(out + o, tmp, tl);
            o += tl;
            s = p;
            continue;
        }
        out[o++] = *s++;
    }
    out[o] = 0;
    return true;
}

static void apply_raw(cctx_t *cx, web_node_t *n, web_style_t *st, const web_style_t *ps, const decl_t *d, int pass) {
    char buf[1024];
    if (!subst_vars(st, d->v.raw, buf, sizeof buf, 0)) return;
    decl_t tmp[16];
    dout_t o = { tmp, 0, 16 };
    /* the raw text may still contain var() if substitution produced it: avoid loops */
    if (strcasestr(buf, "var(")) return;
    parse_decl(&o, d->prop, buf, strlen(buf), d->important);
    for (int i = 0; i < o.n; i++) if (tmp[i].v.t != CV_RAW) apply_decl(cx, n, st, ps, &tmp[i], pass);
}

static uint8_t display_kw(int kw) {
    switch (kw) {
    case K_NONE: return D_NONE;
    case K_INLINE: return D_INLINE;
    case K_BLOCK: case K_FLOW_ROOT: return D_BLOCK;
    case K_LIST_ITEM: return D_LIST_ITEM;
    case K_INLINE_BLOCK: return D_INLINE_BLOCK;
    case K_TABLE: return D_TABLE;
    case K_INLINE_TABLE: return D_INLINE_TABLE;
    case K_TABLE_ROW_GROUP: return D_ROW_GROUP;
    case K_TABLE_HEADER_GROUP: return D_HEADER_GROUP;
    case K_TABLE_FOOTER_GROUP: return D_FOOTER_GROUP;
    case K_TABLE_ROW: return D_ROW;
    case K_TABLE_CELL: return D_CELL;
    case K_TABLE_CAPTION: return D_CAPTION;
    case K_TABLE_COLUMN: return D_COLUMN;
    case K_TABLE_COLUMN_GROUP: return D_COLUMN_GROUP;
    case K_FLEX: return D_FLEX;
    case K_INLINE_FLEX: return D_INLINE_FLEX;
    case K_GRID: return D_GRID;
    case K_INLINE_GRID: return D_INLINE_BLOCK;
    case K_CONTENTS: return D_CONTENTS;
    default: return 255;
    }
}

/* pass 0: font-size and custom properties; pass 1: everything else */
static void apply_decl(cctx_t *cx, web_node_t *n, web_style_t *st, const web_style_t *ps, const decl_t *d, int pass) {
    int p = d->prop;
    const cval_t *v = &d->v;
    bool font_pass = p == P_FONT_SIZE || p == P_CUSTOM;
    if ((pass == 0) != font_pass) return;
    if (v->t == CV_RAW) { apply_raw(cx, n, st, ps, d, pass); return; }
    if (p == P_CUSTOM) {
        web_var_t *nv = wa_alloc(&cx->doc->ar, sizeof *nv);
        if (!nv) return;
        nv->name = d->name;
        nv->value = d->v.raw;
        nv->next = st->vars;
        st->vars = nv;
        return;
    }
    bool inherit = v->t == CV_INHERIT, initial = v->t == CV_INITIAL;
    switch (p) {
    case P_DISPLAY:
        if (inherit) st->display = ps->display;
        else if (initial) st->display = D_INLINE;
        else if (v->t == CV_KW) { uint8_t dd = display_kw(v->kw); if (dd != 255) st->display = dd; }
        break;
    case P_COLOR:
        if (inherit || initial) st->color = initial ? 0xFF000000u : ps->color;
        else if (v->t == CV_COLOR) st->color = v->color;
        else if (v->t == CV_KW && v->kw == K_CURRENTCOLOR) st->color = ps->color;
        break;
    case P_BG_COLOR:
        if (inherit) st->bg = ps->bg;
        else if (initial) st->bg = 0;
        else if (v->t == CV_COLOR) st->bg = v->color;
        else if (v->t == CV_KW && v->kw == K_CURRENTCOLOR) st->bg = st->color;
        break;
    case P_FONT_SIZE: {
        int fs = st->fsize;
        if (inherit || initial) fs = initial ? 16 * cx->c->zoom / 100 : ps->fsize;
        else if (v->t == CV_KW) {
            if (v->kw == K_SMALLER) fs = (int)lroundf(ps->fsize / 1.2f);
            else if (v->kw == K_LARGER) fs = (int)lroundf(ps->fsize * 1.2f);
            else fs = font_kw_size(v->kw, ps->fsize * 100 / cx->c->zoom) * cx->c->zoom / 100;
        } else if (v->t == CV_LEN) {
            fs = to_px(cx, st, ps, v, ps->fsize, true);
        } else if (v->t == CV_NUM && v->f > 0) {
            fs = (int)lroundf(v->f * cx->c->zoom / 100.0f);
        }
        st->fsize = (int16_t)CLAMP(fs, 1, 400);
        if (n->tag == T_HTML) cx->root_fs = st->fsize;
        break;
    }
    case P_FONT_WEIGHT:
        if (inherit) st->bold = ps->bold;
        else if (initial) st->bold = 0;
        else if (v->t == CV_KW) {
            if (v->kw == K_BOLD) st->bold = 1;
            else if (v->kw == K_BOLDER) st->bold = 1;
            else if (v->kw == K_NORMAL || v->kw == K_LIGHTER) st->bold = 0;
        } else if (v->t == CV_NUM) st->bold = v->f >= 600;
        break;
    case P_FONT_STYLE:
        if (inherit) st->italic = ps->italic;
        else if (initial) st->italic = 0;
        else if (v->t == CV_KW) st->italic = v->kw == K_ITALIC || v->kw == K_OBLIQUE;
        break;
    case P_FONT_FAMILY:
        if (inherit) st->mono = ps->mono;
        else if (v->t == CV_STR) st->mono = (uint8_t)v->kw;
        break;
    case P_TEXT_ALIGN:
        if (inherit) { st->align = ps->align; st->align_legacy = ps->align_legacy; }
        else if (initial) { st->align = TA_LEFT; st->align_legacy = 0; }
        else if (v->t == CV_KW) {
            st->align_legacy = 0;
            switch (v->kw) {
            case K_LEFT: case K_START: st->align = TA_LEFT; break;
            case K_RIGHT: case K_END: st->align = TA_RIGHT; break;
            case K_CENTER: st->align = TA_CENTER; break;
            case K_WEBKIT_CENTER: st->align = TA_CENTER; st->align_legacy = 1; break;
            case K_JUSTIFY: st->align = TA_JUSTIFY; break;
            case K_MATCH_PARENT: st->align = ps->align; break;
            default: break;
            }
        }
        break;
    case P_TEXT_DECO_LINE:
        if (inherit) st->deco = ps->deco;
        else if (initial) st->deco = 0;
        else if (v->t == CV_NUM) {
            int lines = (int)v->f;
            if (!lines) st->deco = 0;      /* "none" removes decorations the element would draw */
            else st->deco |= (uint8_t)lines;
            st->deco_color = st->color;
        }
        break;
    case P_TEXT_DECO_COLOR:
        if (v->t == CV_COLOR) st->deco_color = v->color;
        break;
    case P_TEXT_TRANSFORM:
        if (inherit) st->transform = ps->transform;
        else if (v->t == CV_KW) st->transform = v->kw == K_UPPERCASE ? TT_UPPER : v->kw == K_LOWERCASE ? TT_LOWER :
                                                v->kw == K_CAPITALIZE ? TT_CAPITALIZE : TT_NONE;
        else if (initial) st->transform = TT_NONE;
        break;
    case P_LINE_HEIGHT:
        if (inherit) st->lh = ps->lh;
        else if (initial || (v->t == CV_KW && v->kw == K_NORMAL)) st->lh = 0;
        else if (v->t == CV_NUM) st->lh = (int16_t)-CLAMP((int)lroundf(v->f * 100), 1, 2000);   /* negative = factor x100 */
        else if (v->t == CV_LEN) st->lh = (int16_t)CLAMP(to_px(cx, st, ps, v, st->fsize, false), 0, 2000);
        break;
    case P_WHITE_SPACE:
        if (inherit) st->ws = ps->ws;
        else if (initial) st->ws = WS_NORMAL;
        else if (v->t == CV_KW) {
            switch (v->kw) {
            case K_NORMAL: st->ws = WS_NORMAL; break;
            case K_PRE: st->ws = WS_PRE; break;
            case K_NOWRAP: st->ws = WS_NOWRAP; break;
            case K_PRE_WRAP: case K_BREAK_SPACES: st->ws = WS_PRE_WRAP; break;
            case K_PRE_LINE: st->ws = WS_PRE_LINE; break;
            default: break;
            }
        }
        break;
    case P_LIST_TYPE:
        if (inherit) st->list = ps->list;
        else if (initial) st->list = LS_DISC;
        else if (v->t == CV_KW) {
            switch (v->kw) {
            case K_NONE: st->list = LS_NONE; break;
            case K_DISC: st->list = LS_DISC; break;
            case K_CIRCLE: st->list = LS_CIRCLE; break;
            case K_SQUARE: st->list = LS_SQUARE; break;
            case K_DECIMAL: case K_DECIMAL_LEADING_ZERO: st->list = LS_DECIMAL; break;
            case K_LOWER_ALPHA: case K_LOWER_LATIN: st->list = LS_LOWER_ALPHA; break;
            case K_UPPER_ALPHA: case K_UPPER_LATIN: st->list = LS_UPPER_ALPHA; break;
            case K_LOWER_ROMAN: st->list = LS_LOWER_ROMAN; break;
            case K_UPPER_ROMAN: st->list = LS_UPPER_ROMAN; break;
            default: break;
            }
        }
        break;
    case P_LIST_POS:
        if (inherit) st->list_inside = ps->list_inside;
        else if (v->t == CV_KW) st->list_inside = v->kw == K_INSIDE;
        break;
    case P_VALIGN:
        if (inherit) st->valign = ps->valign;
        else if (initial) st->valign = VA_BASELINE;
        else if (v->t == CV_KW) {
            switch (v->kw) {
            case K_BASELINE: st->valign = VA_BASELINE; break;
            case K_TOP: st->valign = VA_TOP; break;
            case K_MIDDLE: st->valign = VA_MIDDLE; break;
            case K_BOTTOM: st->valign = VA_BOTTOM; break;
            case K_SUB: st->valign = VA_SUB; break;
            case K_SUPER: st->valign = VA_SUPER; break;
            case K_TEXT_TOP: st->valign = VA_TEXT_TOP; break;
            case K_TEXT_BOTTOM: st->valign = VA_TEXT_BOTTOM; break;
            default: break;
            }
        }
        break;
    case P_VISIBILITY:
        if (inherit) st->visibility_hidden = ps->visibility_hidden;
        else if (initial) st->visibility_hidden = 0;
        else if (v->t == CV_KW) st->visibility_hidden = v->kw == K_HIDDEN || v->kw == K_COLLAPSE;
        break;
    case P_MT: case P_MR: case P_MB: case P_ML: {
        int i = p - P_MT;
        if (inherit) st->margin[i] = ps->margin[i];
        else if (initial) st->margin[i] = (web_len_t){ 0, U_PX };
        else st->margin[i] = to_len(cx, st, ps, v, true);
        break;
    }
    case P_PT: case P_PR: case P_PB: case P_PL: {
        int i = p - P_PT;
        if (inherit) st->padding[i] = ps->padding[i];
        else if (initial) st->padding[i] = (web_len_t){ 0, U_PX };
        else { web_len_t l = to_len(cx, st, ps, v, false); if (l.v < 0) l.v = 0; st->padding[i] = l; }
        break;
    }
    case P_BTW: case P_BRW: case P_BBW: case P_BLW: {
        int i = p - P_BTW;
        if (inherit) st->bw[i] = ps->bw[i];
        else if (initial) st->bw[i] = 0;
        else st->bw[i] = (int16_t)bwidth(cx, st, ps, v);
        break;
    }
    case P_BTS: case P_BRS: case P_BBS: case P_BLS: {
        /* a border width only counts when the side has a style: remembered in
           the top bits of `spacing` until the style is complete */
        int i = p - P_BTS;
        if (v->t == CV_KW && v->kw != K_NONE && v->kw != K_HIDDEN) st->spacing |= (int16_t)(0x1000 << i);
        else if (v->t == CV_KW || initial) st->spacing &= (int16_t)~(0x1000 << i);
        else if (inherit) st->spacing = (int16_t)((st->spacing & ~(0x1000 << i)) | (ps->spacing & (0x1000 << i)));
        break;
    }
    case P_BTC: case P_BRC: case P_BBC: case P_BLC: {
        int i = p - P_BTC;
        if (v->t == CV_COLOR) st->bc[i] = v->color;
        else if ((v->t == CV_KW && v->kw == K_CURRENTCOLOR) || initial) st->bc[i] = 0x00000001;   /* = current colour */
        else if (inherit) st->bc[i] = ps->bc[i];
        break;
    }
    case P_WIDTH: st->width = inherit ? ps->width : initial ? (web_len_t){ 0, U_AUTO } : to_len(cx, st, ps, v, true); break;
    case P_HEIGHT: st->height = inherit ? ps->height : initial ? (web_len_t){ 0, U_AUTO } : to_len(cx, st, ps, v, true); break;
    case P_MAX_WIDTH: st->maxw = inherit ? ps->maxw : initial ? (web_len_t){ 0, U_NONE } : to_len(cx, st, ps, v, true);
        if (st->maxw.u == U_AUTO) st->maxw.u = U_NONE;
        break;
    case P_MIN_WIDTH: st->minw = inherit ? ps->minw : initial ? (web_len_t){ 0, U_AUTO } : to_len(cx, st, ps, v, true); break;
    case P_LEFT: st->left = to_len(cx, st, ps, v, true); break;
    case P_TOP: st->top = to_len(cx, st, ps, v, true); break;
    case P_POSITION:
        if (v->t == CV_KW) st->position = v->kw == K_RELATIVE ? POS_RELATIVE : v->kw == K_ABSOLUTE ? POS_ABSOLUTE :
                                          v->kw == K_FIXED ? POS_FIXED : v->kw == K_STICKY ? POS_STICKY : POS_STATIC;
        else if (initial) st->position = POS_STATIC;
        break;
    case P_FLOAT:
        if (v->t == CV_KW) st->floating = v->kw == K_LEFT ? 1 : v->kw == K_RIGHT ? 2 : 0;
        else if (initial) st->floating = 0;
        break;
    case P_CLEAR:
        if (v->t == CV_KW) st->clear = v->kw == K_LEFT ? 1 : v->kw == K_RIGHT ? 2 : v->kw == K_BOTH ? 3 : 0;
        break;
    case P_BOX_SIZING:
        if (v->t == CV_KW) st->border_box = v->kw == K_BORDER_BOX;
        else if (inherit) st->border_box = ps->border_box;
        break;
    case P_OVERFLOW:
        if (v->t == CV_KW) st->overflow_hidden = v->kw == K_HIDDEN || v->kw == K_CLIP;
        break;
    case P_TEXT_INDENT:
        if (inherit) st->indent = ps->indent;
        else if (v->t == CV_LEN && v->unit != UN_PCT) st->indent = (int16_t)CLAMP(to_px(cx, st, ps, v, 0, false), -3000, 3000);
        else if (initial) st->indent = 0;
        break;
    case P_BORDER_COLLAPSE:
        if (v->t == CV_KW) st->collapse = v->kw == K_COLLAPSE;
        else if (inherit) st->collapse = ps->collapse;
        break;
    case P_BORDER_SPACING:
        if (v->t == CV_LEN) { int sp = to_px(cx, st, ps, v, 0, false); st->spacing = (int16_t)((st->spacing & 0xF000) | CLAMP(sp, 0, 0xFFF)); }
        else if (inherit) st->spacing = (int16_t)((st->spacing & 0xF000) | (ps->spacing & 0xFFF));
        break;
    case P_FLEX_DIRECTION:
        if (v->t == CV_KW) st->flex_column = v->kw == K_COLUMN || v->kw == K_COLUMN_REVERSE;
        break;
    case P_FLEX_WRAP:
        if (v->t == CV_KW) st->flex_wrap = v->kw == K_WRAP || v->kw == K_WRAP_REVERSE;
        break;
    case P_TRANSFORM:
        if (v->t == CV_NUM) st->offscreen = v->f > 0;
        else if (initial || (v->t == CV_KW && v->kw == K_NONE)) st->offscreen = 0;
        break;
    case P_FLEX_GROW:
        if (v->t == CV_NUM || v->t == CV_LEN) st->grow = (uint16_t)CLAMP((int)(v->f * 100 + 0.5f), 0, 60000);
        else if (initial) st->grow = 0;
        break;
    case P_FLEX_SHRINK:
        if (v->t == CV_NUM || v->t == CV_LEN) st->shrink = (uint16_t)CLAMP((int)(v->f * 100 + 0.5f), 0, 60000);
        else if (initial) st->shrink = 100;
        break;
    case P_FLEX_BASIS:
        if (v->t == CV_KW && v->kw == K_CONTENT_BOX) st->basis.u = U_AUTO;
        else st->basis = initial ? (web_len_t){ 0, U_AUTO } : to_len(cx, st, ps, v, true);
        if (st->basis.u == U_NONE) st->basis.u = U_AUTO;
        break;
    case P_ORDER:
        if (v->t == CV_NUM || v->t == CV_LEN) st->order = (int16_t)CLAMP((int)v->f, -1000, 1000);
        else if (initial) st->order = 0;
        break;
    case P_JUSTIFY_CONTENT:
        if (v->t == CV_KW) {
            switch (v->kw) {
            case K_CENTER: st->justify = JC_CENTER; break;
            case K_FLEX_END: case K_END: case K_RIGHT: st->justify = JC_END; break;
            case K_SPACE_BETWEEN: st->justify = JC_BETWEEN; break;
            case K_SPACE_AROUND: st->justify = JC_AROUND; break;
            case K_SPACE_EVENLY: st->justify = JC_EVENLY; break;
            default: st->justify = JC_START; break;
            }
        } else if (initial) st->justify = JC_START;
        break;
    case P_ALIGN_ITEMS:
        if (v->t == CV_KW) {
            switch (v->kw) {
            case K_CENTER: st->align_items = AI_CENTER; break;
            case K_FLEX_START: case K_START: case K_TOP: st->align_items = AI_START; break;
            case K_FLEX_END: case K_END: case K_BOTTOM: st->align_items = AI_END; break;
            case K_BASELINE: st->align_items = AI_BASELINE; break;
            default: st->align_items = AI_STRETCH; break;
            }
        } else if (initial) st->align_items = AI_STRETCH;
        break;
    case P_COLUMN_GAP: case P_ROW_GAP: {
        int16_t *g = p == P_COLUMN_GAP ? &st->gap : &st->rgap;
        if (v->t == CV_LEN && v->unit != UN_PCT) *g = (int16_t)CLAMP(to_px(cx, st, ps, v, 0, false), 0, 2000);
        else if (initial || (v->t == CV_KW && v->kw == K_NORMAL)) *g = 0;
        break;
    }
    case P_CLIP:
        if (v->t == CV_NUM && v->f > 0) st->visibility_hidden = 2;   /* clipped away (screen-reader text) */
        break;
    case P_OPACITY:   /* "0" parses as a length */
        if ((v->t == CV_NUM || v->t == CV_LEN) && v->f <= 0.01f) st->visibility_hidden = 3;
        else if (v->t == CV_NUM || v->t == CV_LEN) { if (st->visibility_hidden == 3) st->visibility_hidden = 0; }
        break;
    default:
        break;
    }
}

/* ---- presentational attributes ------------------------------------------------------------------- */
static bool attr_len(const char *v, cval_t *out) {
    if (!v) return false;
    while (*v == ' ') v++;
    char *e;
    double d = strtod(v, &e);
    if (e == v) return false;
    memset(out, 0, sizeof *out);
    out->t = CV_LEN;
    out->f = (float)d;
    out->unit = (*e == '%') ? UN_PCT : UN_PX;
    return true;
}

static void hint(cctx_t *cx, web_node_t *n, web_style_t *st, const web_style_t *ps, int prop, const cval_t *v) {
    decl_t d;
    memset(&d, 0, sizeof d);
    d.prop = (uint16_t)prop;
    d.v = *v;
    apply_decl(cx, n, st, ps, &d, prop == P_FONT_SIZE ? 0 : 1);
}

static void hint_color(cctx_t *cx, web_node_t *n, web_style_t *st, const web_style_t *ps, int prop, const char *val) {
    if (!val) return;
    uint32_t c;
    cval_t v;
    memset(&v, 0, sizeof v);
    if (parse_color_n(val, strlen(val), &c)) { v.t = CV_COLOR; v.color = c; hint(cx, n, st, ps, prop, &v); return; }
    /* legacy colour values without '#' ("ff0000") */
    size_t l = strlen(val);
    if (l == 6 || l == 3) {
        char buf[8] = "#";
        strlcpy(buf + 1, val, sizeof buf - 1);
        if (parse_color_n(buf, strlen(buf), &c)) { v.t = CV_COLOR; v.color = c; hint(cx, n, st, ps, prop, &v); }
    }
}

static void hint_align(cctx_t *cx, web_node_t *n, web_style_t *st, const web_style_t *ps, const char *a, bool legacy) {
    if (!a) return;
    cval_t v = { .t = CV_KW };
    if (!strcasecmp(a, "center") || !strcasecmp(a, "middle")) v.kw = legacy ? K_WEBKIT_CENTER : K_CENTER;
    else if (!strcasecmp(a, "right")) v.kw = K_RIGHT;
    else if (!strcasecmp(a, "left")) v.kw = K_LEFT;
    else if (!strcasecmp(a, "justify")) v.kw = K_JUSTIFY;
    else return;
    hint(cx, n, st, ps, P_TEXT_ALIGN, &v);
}

static void presentational(cctx_t *cx, web_node_t *n, web_style_t *st, const web_style_t *ps) {
    const char *a;
    cval_t v;
    switch (n->tag) {
    case T_BODY:
        hint_color(cx, n, st, ps, P_BG_COLOR, web_attr(n, "bgcolor"));
        hint_color(cx, n, st, ps, P_COLOR, web_attr(n, "text"));
        if ((a = web_attr(n, "link"))) { uint32_t c; if (parse_color_n(a, strlen(a), &c)) { cx->body_link = c; cx->has_body_link = true; } }
        if ((a = web_attr(n, "marginwidth")) || (a = web_attr(n, "leftmargin"))) {
            if (attr_len(a, &v)) { hint(cx, n, st, ps, P_ML, &v); hint(cx, n, st, ps, P_MR, &v); }
        }
        if ((a = web_attr(n, "marginheight")) || (a = web_attr(n, "topmargin"))) {
            if (attr_len(a, &v)) { hint(cx, n, st, ps, P_MT, &v); hint(cx, n, st, ps, P_MB, &v); }
        }
        break;
    case T_A:
        if (cx->has_body_link && web_attr(n, "href")) { v = (cval_t){ .t = CV_COLOR, .color = cx->body_link }; hint(cx, n, st, ps, P_COLOR, &v); }
        break;
    case T_FONT:
        hint_color(cx, n, st, ps, P_COLOR, web_attr(n, "color"));
        if ((a = web_attr(n, "face"))) { v = (cval_t){ .t = CV_STR, .kw = font_family_mono(a, strlen(a)) }; hint(cx, n, st, ps, P_FONT_FAMILY, &v); }
        break;
    case T_TABLE: {
        hint_color(cx, n, st, ps, P_BG_COLOR, web_attr(n, "bgcolor"));
        if (attr_len(web_attr(n, "width"), &v)) hint(cx, n, st, ps, P_WIDTH, &v);
        if (attr_len(web_attr(n, "height"), &v)) hint(cx, n, st, ps, P_HEIGHT, &v);
        a = web_attr(n, "align");
        if (a && !strcasecmp(a, "center")) {
            v = (cval_t){ .t = CV_KW, .kw = K_AUTO };
            hint(cx, n, st, ps, P_ML, &v); hint(cx, n, st, ps, P_MR, &v);
        } else if (a && (!strcasecmp(a, "right") || !strcasecmp(a, "left"))) {
            v = (cval_t){ .t = CV_KW, .kw = (int16_t)(!strcasecmp(a, "right") ? K_RIGHT : K_LEFT) };
            hint(cx, n, st, ps, P_FLOAT, &v);
        }
        a = web_attr(n, "border");
        if (a) {
            int bw = *a ? atoi(a) : 1;
            if (bw > 0) {
                v = (cval_t){ .t = CV_LEN, .unit = UN_PX, .f = (float)bw };
                for (int i = 0; i < 4; i++) hint(cx, n, st, ps, P_BWIDTHS[i], &v);
                v = (cval_t){ .t = CV_KW, .kw = K_OUTSET };
                for (int i = 0; i < 4; i++) hint(cx, n, st, ps, P_BSTYLES[i], &v);
                v = (cval_t){ .t = CV_COLOR, .color = RGB(128, 128, 128) };
                for (int i = 0; i < 4; i++) hint(cx, n, st, ps, P_BCOLORS[i], &v);
            }
        }
        if (attr_len(web_attr(n, "cellspacing"), &v)) hint(cx, n, st, ps, P_BORDER_SPACING, &v);
        break;
    }
    case T_TR: case T_TBODY: case T_THEAD: case T_TFOOT:
        hint_color(cx, n, st, ps, P_BG_COLOR, web_attr(n, "bgcolor"));
        hint_align(cx, n, st, ps, web_attr(n, "align"), false);
        if ((a = web_attr(n, "valign"))) {
            v = (cval_t){ .t = CV_KW, .kw = (int16_t)(!strcasecmp(a, "top") ? K_TOP : !strcasecmp(a, "bottom") ? K_BOTTOM : !strcasecmp(a, "baseline") ? K_BASELINE : K_MIDDLE) };
            hint(cx, n, st, ps, P_VALIGN, &v);
        }
        break;
    case T_TD: case T_TH: {
        hint_color(cx, n, st, ps, P_BG_COLOR, web_attr(n, "bgcolor"));
        hint_align(cx, n, st, ps, web_attr(n, "align"), false);
        if ((a = web_attr(n, "valign"))) {
            v = (cval_t){ .t = CV_KW, .kw = (int16_t)(!strcasecmp(a, "top") ? K_TOP : !strcasecmp(a, "bottom") ? K_BOTTOM : !strcasecmp(a, "baseline") ? K_BASELINE : K_MIDDLE) };
            hint(cx, n, st, ps, P_VALIGN, &v);
        }
        if (attr_len(web_attr(n, "width"), &v)) hint(cx, n, st, ps, P_WIDTH, &v);
        if (attr_len(web_attr(n, "height"), &v)) hint(cx, n, st, ps, P_HEIGHT, &v);
        if (web_attr(n, "nowrap")) { v = (cval_t){ .t = CV_KW, .kw = K_NOWRAP }; hint(cx, n, st, ps, P_WHITE_SPACE, &v); }
        /* the table's cellpadding and border */
        web_node_t *t = n->parent;
        while (t && t->tag != T_TABLE) t = t->parent;
        if (t) {
            if (attr_len(web_attr(t, "cellpadding"), &v))
                for (int i = 0; i < 4; i++) hint(cx, n, st, ps, P_PADDINGS[i], &v);
            a = web_attr(t, "border");
            if (a && (!*a || atoi(a) > 0)) {
                v = (cval_t){ .t = CV_LEN, .unit = UN_PX, .f = 1 };
                for (int i = 0; i < 4; i++) hint(cx, n, st, ps, P_BWIDTHS[i], &v);
                v = (cval_t){ .t = CV_KW, .kw = K_INSET };
                for (int i = 0; i < 4; i++) hint(cx, n, st, ps, P_BSTYLES[i], &v);
                v = (cval_t){ .t = CV_COLOR, .color = RGB(128, 128, 128) };
                for (int i = 0; i < 4; i++) hint(cx, n, st, ps, P_BCOLORS[i], &v);
            }
        }
        break;
    }
    case T_DIV: case T_P: case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: case T_CAPTION:
    case T_LEGEND:
        hint_align(cx, n, st, ps, web_attr(n, "align"), n->tag == T_DIV);
        break;
    case T_IMG: case T_INPUT: case T_OBJECT: case T_EMBED: case T_IFRAME: case T_VIDEO: case T_CANVAS:
        if (attr_len(web_attr(n, "width"), &v)) hint(cx, n, st, ps, P_WIDTH, &v);
        if (attr_len(web_attr(n, "height"), &v)) hint(cx, n, st, ps, P_HEIGHT, &v);
        if (n->tag == T_IMG) {
            if ((a = web_attr(n, "border")) && atoi(a) > 0) {
                v = (cval_t){ .t = CV_LEN, .unit = UN_PX, .f = (float)atoi(a) };
                for (int i = 0; i < 4; i++) hint(cx, n, st, ps, P_BWIDTHS[i], &v);
                v = (cval_t){ .t = CV_KW, .kw = K_SOLID };
                for (int i = 0; i < 4; i++) hint(cx, n, st, ps, P_BSTYLES[i], &v);
            }
            if ((a = web_attr(n, "hspace")) && attr_len(a, &v)) { hint(cx, n, st, ps, P_ML, &v); hint(cx, n, st, ps, P_MR, &v); }
            if ((a = web_attr(n, "vspace")) && attr_len(a, &v)) { hint(cx, n, st, ps, P_MT, &v); hint(cx, n, st, ps, P_MB, &v); }
            if ((a = web_attr(n, "align"))) {
                if (!strcasecmp(a, "middle") || !strcasecmp(a, "absmiddle") || !strcasecmp(a, "center")) { v = (cval_t){ .t = CV_KW, .kw = K_MIDDLE }; hint(cx, n, st, ps, P_VALIGN, &v); }
                else if (!strcasecmp(a, "top")) { v = (cval_t){ .t = CV_KW, .kw = K_TOP }; hint(cx, n, st, ps, P_VALIGN, &v); }
                else if (!strcasecmp(a, "left") || !strcasecmp(a, "right")) {
                    v = (cval_t){ .t = CV_KW, .kw = (int16_t)(!strcasecmp(a, "right") ? K_RIGHT : K_LEFT) };
                    hint(cx, n, st, ps, P_FLOAT, &v);
                }
            }
        }
        break;
    case T_HR: {
        if (attr_len(web_attr(n, "width"), &v)) hint(cx, n, st, ps, P_WIDTH, &v);
        if ((a = web_attr(n, "size")) && atoi(a) > 1) {
            v = (cval_t){ .t = CV_LEN, .unit = UN_PX, .f = (float)(atoi(a) - 2) };
            hint(cx, n, st, ps, P_HEIGHT, &v);
        }
        a = web_attr(n, "color");
        if (a) {
            hint_color(cx, n, st, ps, P_BG_COLOR, a);
            for (int i = 0; i < 4; i++) hint_color(cx, n, st, ps, P_BCOLORS[i], a);
        }
        a = web_attr(n, "align");
        if (a) {
            v = (cval_t){ .t = CV_KW, .kw = K_AUTO };
            cval_t z = { .t = CV_LEN, .unit = UN_PX, .f = 0 };
            if (!strcasecmp(a, "left")) { hint(cx, n, st, ps, P_ML, &z); hint(cx, n, st, ps, P_MR, &v); }
            else if (!strcasecmp(a, "right")) { hint(cx, n, st, ps, P_ML, &v); hint(cx, n, st, ps, P_MR, &z); }
        }
        break;
    }
    case T_UL: case T_OL: case T_LI:
        if ((a = web_attr(n, "type"))) {
            int kw = 0;
            if (!strcmp(a, "1")) kw = K_DECIMAL;
            else if (!strcmp(a, "a")) kw = K_LOWER_ALPHA;
            else if (!strcmp(a, "A")) kw = K_UPPER_ALPHA;
            else if (!strcmp(a, "i")) kw = K_LOWER_ROMAN;
            else if (!strcmp(a, "I")) kw = K_UPPER_ROMAN;
            else if (!strcasecmp(a, "disc")) kw = K_DISC;
            else if (!strcasecmp(a, "circle")) kw = K_CIRCLE;
            else if (!strcasecmp(a, "square")) kw = K_SQUARE;
            else if (!strcasecmp(a, "none")) kw = K_NONE;
            if (kw) { v = (cval_t){ .t = CV_KW, .kw = (int16_t)kw }; hint(cx, n, st, ps, P_LIST_TYPE, &v); }
        }
        break;
    case T_PRE:
        break;
    default:
        break;
    }
}

/* ---- per-element style ------------------------------------------------------------------------------ */
static web_sheet_t *inline_sheet_parse_decls(cctx_t *cx, const char *style, decl_t **decls, int *nd) {
    web_sheet_t *tmp = calloc(1, sizeof *tmp);
    if (!tmp) return NULL;
    char *text = wa_strndup(&tmp->ar, style, strlen(style));
    *nd = text ? parse_decl_block(tmp, text, text + strlen(text), decls) : 0;
    (void)cx;
    return tmp;
}

static void style_element(cctx_t *cx, web_node_t *n, const web_style_t *ps) {
    web_style_t *st = wa_alloc(&cx->doc->ar, sizeof *st);
    if (!st) { n->st = NULL; return; }
    /* inherited */
    st->color = ps->color;
    st->fsize = ps->fsize;
    st->lh = ps->lh;
    st->bold = ps->bold;
    st->italic = ps->italic;
    st->mono = ps->mono;
    st->align = ps->align;
    st->align_legacy = ps->align_legacy;
    st->ws = ps->ws;
    st->list = ps->list;
    st->list_inside = ps->list_inside;
    st->transform = ps->transform;
    st->visibility_hidden = ps->visibility_hidden == 1 ? 1 : 0;
    st->deco = ps->deco;
    st->deco_color = ps->deco_color;
    st->indent = ps->indent;
    st->collapse = ps->collapse;
    st->border_box = 0;
    st->vars = ps->vars;
    st->valign = (n->tag == T_TD || n->tag == T_TH || n->tag == T_TR) ? ps->valign : VA_BASELINE;
    if (n->tag == T_TD || n->tag == T_TH) { if (st->valign == VA_BASELINE) st->valign = VA_MIDDLE; }
    st->spacing = ps->spacing & 0x0FFF;   /* border-spacing inherits; border styles do not */
    /* initial values */
    st->display = D_INLINE;
    st->width.u = U_AUTO; st->height.u = U_AUTO; st->maxw.u = U_NONE; st->minw.u = U_AUTO;
    st->left.u = U_AUTO; st->top.u = U_AUTO;
    for (int i = 0; i < 4; i++) {
        st->bc[i] = 0x00000001;   /* current colour */
        st->margin[i] = st->padding[i] = (web_len_t){ 0, U_PX };
    }
    st->shrink = 100;
    st->basis.u = U_AUTO;

    /* collect matching declarations */
    cx->nm = 0;
    match_sheet(cx, ua_sheet, 0, 0, n, NULL);
    if (cx->doc->quirks) match_sheet(cx, ua_quirks, 0, 0, n, NULL);
    for (int i = 0; i < cx->c->nsheets && i < 63; i++)
        match_sheet(cx, cx->c->sheets[i], 2, i + 1, n, cx->media_ok[i]);
    web_sheet_t *inl = NULL;
    const char *sa = web_attr(n, "style");
    if (sa && *sa) {
        decl_t *decls = NULL;
        int nd = 0;
        inl = inline_sheet_parse_decls(cx, sa, &decls, &nd);
        for (int i = 0; i < nd; i++) m_add(cx, &decls[i], make_key(&decls[i], 3, 0, i));
    }
    if (cx->nm > 1) qsort(cx->m, cx->nm, sizeof(mdecl_t), mcmp);

    /* the presentational hints rank between UA and author rules: apply UA decls first */
    int split = 0;
    while (split < cx->nm && ((cx->m[split].key >> 58) & 0xF) == 0 && !(cx->m[split].key >> 62)) split++;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < split; i++) apply_decl(cx, n, st, ps, cx->m[i].d, pass);
        if (pass == 1) presentational(cx, n, st, ps);
        else {
            /* <font size> is a font-size hint */
            if (n->tag == T_FONT) {
                const char *a = web_attr(n, "size");
                if (a && *a) {
                    static const int sizes[8] = { 10, 10, 13, 16, 18, 24, 32, 48 };
                    int k = atoi(a);
                    if (*a == '+' || *a == '-') k = 3 + k;
                    k = CLAMP(k, 1, 7);
                    st->fsize = (int16_t)(sizes[k] * cx->c->zoom / 100);
                }
            }
        }
        for (int i = split; i < cx->nm; i++) apply_decl(cx, n, st, ps, cx->m[i].d, pass);
    }
    if (inl) web_css_free(inl);

    /* finish borders: a width only counts with a style */
    for (int i = 0; i < 4; i++) {
        if (!(st->spacing & (0x1000 << i))) st->bw[i] = 0;
        if (st->bc[i] == 0x00000001) st->bc[i] = st->color;
    }
    st->spacing &= 0x0FFF;
    if (n->tag == T_TABLE && st->collapse) st->spacing = 0;
    if (st->deco && !st->deco_color) st->deco_color = st->color;
    /* elements that are hidden off-screen for screen readers */
    if ((st->position == POS_ABSOLUTE || st->position == POS_FIXED) &&
        ((st->left.u == U_PX && st->left.v < -500) || (st->top.u == U_PX && st->top.v < -500)))
        st->display = D_NONE;
    if (st->visibility_hidden >= 2 || (st->overflow_hidden && st->width.u == U_PX && st->width.v <= 1 &&
                                       st->height.u == U_PX && st->height.v <= 1))
        st->display = D_NONE;
    if (st->visibility_hidden == 3 && (st->position == POS_ABSOLUTE || st->position == POS_FIXED)) st->display = D_NONE;
    /* out-of-flow boxes that are hidden or slid out of view (dropdowns, off-canvas menus) take no room */
    if ((st->position == POS_ABSOLUTE || st->position == POS_FIXED) &&
        (st->offscreen || st->visibility_hidden == 1 ||
         (st->left.u == U_PCT && st->left.v <= -1000) || (st->top.u == U_PCT && st->top.v <= -1000)))
        st->display = D_NONE;
    if (st->visibility_hidden > 1) st->visibility_hidden = 0;
    if (n->tag == T_INPUT) {
        const char *ty = web_attr(n, "type");
        if (ty && !strcasecmp(ty, "hidden")) st->display = D_NONE;
    }
    n->st = st;
}

static void style_rec(cctx_t *cx, web_node_t *n, const web_style_t *ps) {
    for (web_node_t *c = n->first; c; c = c->next) {
        if (c->type != WN_ELEM) continue;
        style_element(cx, c, ps);
        if (!c->st) continue;
        if (c->st->display == D_NONE) continue;   /* nothing inside is shown */
        style_rec(cx, c, c->st);
    }
}

void web_style_doc(web_doc_t *d, const web_cascade_t *c) {
    if (!d || !d->root) return;
    if (!ua_sheet) ua_sheet = web_css_parse(ua_css, strlen(ua_css), 0);
    if (!ua_quirks) ua_quirks = web_css_parse(ua_quirks_css, strlen(ua_quirks_css), 0);
    cctx_t cx;
    memset(&cx, 0, sizeof cx);
    cx.c = c;
    cx.doc = d;
    cx.root_fs = 16 * c->zoom / 100;
    for (int i = 0; i < c->nsheets && i < 63; i++) {
        web_sheet_t *sh = c->sheets[i];
        if (!sh || !sh->nmedia) continue;
        cx.media_ok[i] = malloc(sizeof(bool) * sh->nmedia);
        if (!cx.media_ok[i]) continue;
        for (int k = 0; k < sh->nmedia; k++) cx.media_ok[i][k] = media_match(sh->media[k], c);
    }
    web_style_t root;
    memset(&root, 0, sizeof root);
    root.color = 0xFF000000u;
    root.fsize = (int16_t)(16 * c->zoom / 100);
    root.display = D_BLOCK;
    root.spacing = 0;
    root.width.u = U_AUTO; root.height.u = U_AUTO; root.maxw.u = U_NONE;
    style_rec(&cx, d->root, &root);
    for (int i = 0; i < 64; i++) free(cx.media_ok[i]);
    free(cx.m);
}
