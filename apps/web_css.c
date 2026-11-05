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
    P_TRANSFORM, P_RIGHT, P_BOTTOM, P_MIN_HEIGHT, P_MAX_HEIGHT,
    P_RADIUS_TL, P_RADIUS_TR, P_RADIUS_BR, P_RADIUS_BL, P_ZINDEX, P_TRANSLATE,
    P_ALIGN_SELF, P_JUSTIFY_SELF, P_JUSTIFY_ITEMS, P_ALIGN_CONTENT, P_ASPECT_RATIO,
    P_GRID_COLS, P_GRID_ROWS, P_GRID_AREAS, P_GRID_AUTO_COLS, P_GRID_AUTO_ROWS, P_GRID_AUTO_FLOW,
    P_GRID_COL_START, P_GRID_COL_END, P_GRID_ROW_START, P_GRID_ROW_END, P_GRID_AREA_NAME,
    P_ISOLATION, P_MASK_IMAGE,
    P_LONGHAND_COUNT,
    /* shorthands (only used while parsing / for var()) */
    P_MARGIN, P_PADDING, P_BORDER, P_BORDER_TOP, P_BORDER_RIGHT, P_BORDER_BOTTOM, P_BORDER_LEFT,
    P_BORDER_WIDTH, P_BORDER_STYLE, P_BORDER_COLOR, P_BACKGROUND, P_FONT, P_LIST_STYLE,
    P_TEXT_DECORATION, P_FLEX_FLOW, P_OVERFLOW_X, P_OVERFLOW_Y, P_INSET, P_CUSTOM, P_FLEX, P_GAP,
    P_PADDING_INLINE, P_PADDING_BLOCK, P_MARGIN_INLINE, P_MARGIN_BLOCK, P_INSET_INLINE, P_INSET_BLOCK,
    P_RADIUS, P_PLACE_ITEMS, P_PLACE_SELF, P_PLACE_CONTENT, P_GRID_COLUMN, P_GRID_ROW, P_GRID_AREA,
    P_GRID_TEMPLATE, P_BORDER_INLINE, P_BORDER_BLOCK,
    P_BORDER_INLINE_WIDTH, P_BORDER_BLOCK_WIDTH, P_BORDER_INLINE_COLOR, P_BORDER_BLOCK_COLOR,
    P_BORDER_INLINE_STYLE, P_BORDER_BLOCK_STYLE,
    P_IGNORED,     /* known to browsers, no effect here (only @supports asks about it) */
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
    {"right", P_RIGHT}, {"bottom", P_BOTTOM}, {"inset-inline-end", P_RIGHT}, {"inset-block-end", P_BOTTOM},
    {"min-height", P_MIN_HEIGHT}, {"max-height", P_MAX_HEIGHT}, {"min-block-size", P_MIN_HEIGHT},
    {"max-block-size", P_MAX_HEIGHT}, {"min-inline-size", P_MIN_WIDTH},
    {"border-top-left-radius", P_RADIUS_TL}, {"border-top-right-radius", P_RADIUS_TR},
    {"border-bottom-right-radius", P_RADIUS_BR}, {"border-bottom-left-radius", P_RADIUS_BL},
    {"border-start-start-radius", P_RADIUS_TL}, {"border-start-end-radius", P_RADIUS_TR},
    {"border-end-end-radius", P_RADIUS_BR}, {"border-end-start-radius", P_RADIUS_BL},
    {"border-radius", P_RADIUS}, {"z-index", P_ZINDEX}, {"translate", P_TRANSLATE},
    {"align-self", P_ALIGN_SELF}, {"justify-self", P_JUSTIFY_SELF}, {"justify-items", P_JUSTIFY_ITEMS},
    {"align-content", P_ALIGN_CONTENT}, {"aspect-ratio", P_ASPECT_RATIO},
    {"place-items", P_PLACE_ITEMS}, {"place-self", P_PLACE_SELF}, {"place-content", P_PLACE_CONTENT},
    {"grid-template-columns", P_GRID_COLS}, {"grid-template-rows", P_GRID_ROWS},
    {"grid-template-areas", P_GRID_AREAS}, {"grid-auto-columns", P_GRID_AUTO_COLS},
    {"grid-auto-rows", P_GRID_AUTO_ROWS}, {"grid-auto-flow", P_GRID_AUTO_FLOW},
    {"grid-column-start", P_GRID_COL_START}, {"grid-column-end", P_GRID_COL_END},
    {"grid-row-start", P_GRID_ROW_START}, {"grid-row-end", P_GRID_ROW_END},
    {"grid-column", P_GRID_COLUMN}, {"grid-row", P_GRID_ROW}, {"grid-area", P_GRID_AREA},
    {"grid-template", P_GRID_TEMPLATE},
    {"padding-inline", P_PADDING_INLINE}, {"padding-block", P_PADDING_BLOCK},
    {"margin-inline", P_MARGIN_INLINE}, {"margin-block", P_MARGIN_BLOCK},
    {"inset-inline", P_INSET_INLINE}, {"inset-block", P_INSET_BLOCK},
    {"border-inline-start", P_BORDER_LEFT}, {"border-inline-end", P_BORDER_RIGHT},
    {"border-block-start", P_BORDER_TOP}, {"border-block-end", P_BORDER_BOTTOM},
    {"border-inline", P_BORDER_INLINE}, {"border-block", P_BORDER_BLOCK},
    {"border-inline-start-width", P_BLW}, {"border-inline-end-width", P_BRW},
    {"border-block-start-width", P_BTW}, {"border-block-end-width", P_BBW},
    {"border-inline-start-color", P_BLC}, {"border-inline-end-color", P_BRC},
    {"border-block-start-color", P_BTC}, {"border-block-end-color", P_BBC},
    {"border-inline-start-style", P_BLS}, {"border-inline-end-style", P_BRS},
    {"border-block-start-style", P_BTS}, {"border-block-end-style", P_BBS},
    {"border-inline-width", P_BORDER_INLINE_WIDTH}, {"border-block-width", P_BORDER_BLOCK_WIDTH},
    {"border-inline-color", P_BORDER_INLINE_COLOR}, {"border-block-color", P_BORDER_BLOCK_COLOR},
    {"border-inline-style", P_BORDER_INLINE_STYLE}, {"border-block-style", P_BORDER_BLOCK_STYLE},
    {"-webkit-tap-highlight-color", P_IGNORED}, {"transition-behavior", P_IGNORED}, {"isolation", P_ISOLATION},
    {"mask-image", P_MASK_IMAGE}, {"-webkit-mask-image", P_MASK_IMAGE}, {"mask", P_MASK_IMAGE}, {"-webkit-mask", P_MASK_IMAGE},
};

static int prop_lookup(const char *s, size_t n) {
    if (n >= 2 && s[0] == '-' && s[1] == '-') return P_CUSTOM;
    for (int i = 0; i < ARRAY_SIZE(prop_names); i++)
        if (strlen(prop_names[i].name) == n && !strncasecmp(prop_names[i].name, s, n)) return prop_names[i].id;
    return P_NONE;
}

/* ---- values ---------------------------------------------------------------------- */
enum { CV_NONE = 0, CV_KW, CV_LEN, CV_NUM, CV_COLOR, CV_INHERIT, CV_INITIAL, CV_RAW, CV_STR };
enum { UN_PX = 0, UN_EM, UN_REM, UN_PCT, UN_PT, UN_VW, UN_VH, UN_EX, UN_CH, UN_NONE, UN_VMIN, UN_VMAX,
       UN_LH, UN_CALC };
/* UN_CALC: calc() / min() / max() / clamp(), evaluated when the style is computed;
   cval_t.raw = the text, cval_t.color = its length */

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
/* A length unit: its UN_* and a factor for the units folded into another one. */
static bool unit_lookup(const char *u, size_t n, uint8_t *unit, float *mul) {
    static const struct { const char *s; uint8_t u; float m; } units[] = {
        {"px", UN_PX, 1}, {"em", UN_EM, 1}, {"rem", UN_REM, 1}, {"%", UN_PCT, 1}, {"pt", UN_PT, 1},
        {"pc", UN_PT, 12}, {"in", UN_PX, 96}, {"cm", UN_PX, 37.8f}, {"mm", UN_PX, 3.78f}, {"q", UN_PX, 0.945f},
        {"vw", UN_VW, 1}, {"svw", UN_VW, 1}, {"lvw", UN_VW, 1}, {"dvw", UN_VW, 1},
        {"vi", UN_VW, 1}, {"svi", UN_VW, 1}, {"lvi", UN_VW, 1}, {"dvi", UN_VW, 1},
        {"vh", UN_VH, 1}, {"svh", UN_VH, 1}, {"lvh", UN_VH, 1}, {"dvh", UN_VH, 1},
        {"vb", UN_VH, 1}, {"svb", UN_VH, 1}, {"lvb", UN_VH, 1}, {"dvb", UN_VH, 1},
        {"vmin", UN_VMIN, 1}, {"svmin", UN_VMIN, 1}, {"lvmin", UN_VMIN, 1}, {"dvmin", UN_VMIN, 1},
        {"vmax", UN_VMAX, 1}, {"svmax", UN_VMAX, 1}, {"lvmax", UN_VMAX, 1}, {"dvmax", UN_VMAX, 1},
        /* container query units: the viewport stands in for the container */
        {"cqw", UN_VW, 1}, {"cqi", UN_VW, 1}, {"cqh", UN_VH, 1}, {"cqb", UN_VH, 1},
        {"cqmin", UN_VMIN, 1}, {"cqmax", UN_VMAX, 1},
        {"ex", UN_EX, 1}, {"ch", UN_CH, 1}, {"lh", UN_LH, 1}, {"rlh", UN_REM, 1.2f},
        {"cap", UN_EM, 0.7f}, {"ic", UN_EM, 1}, {"rex", UN_REM, 0.5f}, {"rch", UN_REM, 0.5f},
    };
    for (int i = 0; i < ARRAY_SIZE(units); i++)
        if (strlen(units[i].s) == n && !strncasecmp(units[i].s, u, n)) { *unit = units[i].u; *mul = units[i].m; return true; }
    return false;
}

/* ---- calc() ----
   Values are linear combinations px + pc% (pc of the percentage base); a plain number
   has num set. The context resolves font-relative and viewport units (NULL: defaults,
   for checking the syntax while parsing). */
typedef struct cterm { float px, pc; bool num; } cterm_t;
typedef struct cenv { int em, rem, vw, vh, zoom, lh; } cenv_t;
static bool calc_expr(const cenv_t *e, const char **pp, const char *end, cterm_t *out, int depth);

static const char *c_ws(const char *p, const char *end) {
    while (p < end && web_isspace((unsigned char)*p)) p++;
    return p;
}

static bool calc_args(const cenv_t *e, const char **pp, const char *end, cterm_t *args, int *nargs, int depth) {
    /* after "fn(": comma-separated expressions up to ")" */
    const char *p = *pp;
    int n = 0;
    for (;;) {
        cterm_t t;
        if (!calc_expr(e, &p, end, &t, depth + 1)) return false;
        if (n < 8) args[n++] = t;
        p = c_ws(p, end);
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == ')') { p++; break; }
        return false;
    }
    *pp = p;
    *nargs = n;
    return true;
}

/* compare mixed px / % terms with the viewport width standing in for the base */
static float c_est(const cenv_t *e, const cterm_t *t) { return t->px + t->pc * (float)(e ? e->vw : 1024) / 100.0f; }

static bool calc_factor(const cenv_t *e, const char **pp, const char *end, cterm_t *out, int depth) {
    const char *p = c_ws(*pp, end);
    if (depth > 24 || p >= end) return false;
    memset(out, 0, sizeof *out);
    if (*p == '(') {
        p++;
        if (!calc_expr(e, &p, end, out, depth + 1)) return false;
        p = c_ws(p, end);
        if (p >= end || *p != ')') return false;
        *pp = p + 1;
        return true;
    }
    if (*p == '-' && p + 1 < end && (p[1] == '(' || isalpha((unsigned char)p[1]))) {
        /* -(...) / -var-substituted function */
        p++;
        if (!calc_factor(e, &p, end, out, depth + 1)) return false;
        out->px = -out->px; out->pc = -out->pc;
        *pp = p;
        return true;
    }
    if (isalpha((unsigned char)*p)) {
        const char *ns = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '-')) p++;
        size_t nl = (size_t)(p - ns);
        if (p >= end || *p != '(') {
            /* constants */
            if (nl == 2 && !strncasecmp(ns, "pi", 2)) { out->num = true; out->px = 3.14159265f; *pp = p; return true; }
            if (nl == 1 && (*ns == 'e' || *ns == 'E')) { out->num = true; out->px = 2.7182818f; *pp = p; return true; }
            return false;
        }
        p++;
        cterm_t a[8];
        int na;
        if (!calc_args(e, &p, end, a, &na, depth)) return false;
        if ((nl == 4 && !strncasecmp(ns, "calc", 4)) || (nl == 12 && !strncasecmp(ns, "-webkit-calc", 12)) ||
            (nl == 9 && !strncasecmp(ns, "-moz-calc", 9))) {
            if (na != 1) return false;
            *out = a[0];
        } else if ((nl == 3 && (!strncasecmp(ns, "min", 3) || !strncasecmp(ns, "max", 3)))) {
            bool mx = (ns[1] | 0x20) == 'a';
            int best = 0;
            for (int i = 1; i < na; i++) {
                float v = c_est(e, &a[i]), b = c_est(e, &a[best]);
                if (mx ? v > b : v < b) best = i;
            }
            *out = a[best];
        } else if (nl == 5 && !strncasecmp(ns, "clamp", 5)) {
            if (na != 3) return false;
            float lo = c_est(e, &a[0]), v = c_est(e, &a[1]), hi = c_est(e, &a[2]);
            *out = v < lo ? a[0] : v > hi ? a[2] : a[1];
            if (lo > hi) *out = a[0];
        } else if (nl == 3 && !strncasecmp(ns, "abs", 3) && na == 1) {
            *out = a[0];
            if (c_est(e, out) < 0) { out->px = -out->px; out->pc = -out->pc; }
        } else if (nl == 5 && !strncasecmp(ns, "round", 5) && na >= 1) {
            *out = a[na >= 2 ? (na == 3 ? 1 : 0) : 0];
        } else {
            return false;    /* sin(), sqrt(), anchor(), ... */
        }
        *pp = p;
        return true;
    }
    bool ok;
    const char *q = p;
    float f = parse_num(&q, &ok);
    if (!ok || q > end) return false;
    const char *us = q;
    while (q < end && (isalpha((unsigned char)*q) || *q == '%')) q++;
    *pp = q;
    if (q == us) { out->num = true; out->px = f; return true; }
    uint8_t unit;
    float mul;
    if (!unit_lookup(us, (size_t)(q - us), &unit, &mul)) return false;
    f *= mul;
    int em = e ? e->em : 16, rem = e ? e->rem : 16, vw = e ? e->vw : 1024, vh = e ? e->vh : 768;
    int zoom = e ? e->zoom : 100;
    switch (unit) {
    case UN_PX: out->px = f * zoom / 100.0f; break;
    case UN_PT: out->px = f * 4.0f / 3.0f * zoom / 100.0f; break;
    case UN_EM: out->px = f * em; break;
    case UN_REM: out->px = f * rem; break;
    case UN_EX: case UN_CH: out->px = f * em * 0.5f; break;
    case UN_LH: out->px = f * (e && e->lh > 0 ? e->lh : em * 1.2f); break;
    case UN_PCT: out->pc = f; break;
    case UN_VW: out->px = f * vw / 100.0f; break;
    case UN_VH: out->px = f * vh / 100.0f; break;
    case UN_VMIN: out->px = f * MIN(vw, vh) / 100.0f; break;
    case UN_VMAX: out->px = f * MAX(vw, vh) / 100.0f; break;
    default: return false;
    }
    return true;
}

static bool calc_term(const cenv_t *e, const char **pp, const char *end, cterm_t *out, int depth) {
    if (!calc_factor(e, pp, end, out, depth)) return false;
    for (;;) {
        const char *p = c_ws(*pp, end);
        if (p >= end || (*p != '*' && *p != '/')) return true;
        char op = *p++;
        cterm_t r;
        if (!calc_factor(e, &p, end, &r, depth)) return false;
        *pp = p;
        if (op == '*') {
            if (r.num) { out->px *= r.px; out->pc *= r.px; }
            else if (out->num) { float k = out->px; *out = r; out->px *= k; out->pc *= k; }
            else return false;
        } else {
            if (!r.num || r.px == 0) return false;
            out->px /= r.px; out->pc /= r.px;
        }
    }
}

static bool calc_expr(const cenv_t *e, const char **pp, const char *end, cterm_t *out, int depth) {
    if (!calc_term(e, pp, end, out, depth)) return false;
    for (;;) {
        const char *p = c_ws(*pp, end);
        /* "a - b" (white space round the operator; "a -b" is read the same way) */
        if (p >= end || (*p != '+' && *p != '-') || (p == *pp && *p == '-')) return true;
        char op = *p++;
        cterm_t r;
        if (!calc_term(e, &p, end, &r, depth)) return false;
        *pp = p;
        if (r.num != out->num && !(r.px == 0 && r.pc == 0) && !(out->px == 0 && out->pc == 0)) return false;
        if (op == '+') { out->px += r.px; out->pc += r.pc; }
        else { out->px -= r.px; out->pc -= r.pc; }
        out->num = out->num && r.num;
    }
}

/* evaluate a whole calc()/min()/max()/clamp() value */
static bool calc_eval(const cenv_t *e, const char *s, size_t n, cterm_t *out) {
    const char *p = s, *end = s + n;
    if (!calc_factor(e, &p, end, out, 0)) return false;
    return c_ws(p, end) == end;
}

static bool is_calc_fn(const char *s, size_t n) {
    return (n > 5 && !strncasecmp(s, "calc(", 5)) || (n > 4 && !strncasecmp(s, "min(", 4)) ||
           (n > 4 && !strncasecmp(s, "max(", 4)) || (n > 6 && !strncasecmp(s, "clamp(", 6)) ||
           (n > 13 && !strncasecmp(s, "-webkit-calc(", 13)) || (n > 10 && !strncasecmp(s, "-moz-calc(", 10)) ||
           (n > 4 && !strncasecmp(s, "abs(", 4)) || (n > 6 && !strncasecmp(s, "round(", 6));
}

static bool parse_len(const char *s, size_t n, cval_t *v) {
    while (n && web_isspace((unsigned char)*s)) { s++; n--; }
    while (n && web_isspace((unsigned char)s[n - 1])) n--;
    if (!n) return false;
    if (is_calc_fn(s, n)) {
        cterm_t t;
        if (!calc_eval(NULL, s, n, &t)) return false;
        if (t.num) { v->t = CV_NUM; v->unit = UN_NONE; v->f = t.px; return true; }
        v->t = CV_LEN;
        v->unit = UN_CALC;
        v->raw = s;
        v->color = (uint32_t)n;
        v->f = t.px;
        return true;
    }
    char buf[64];
    if (n >= sizeof buf) return false;
    memcpy(buf, s, n);
    buf[n] = 0;
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
    float mul;
    if (!unit_lookup(p, strlen(p), &v->unit, &mul)) return false;
    v->f *= mul;
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

static void two(dout_t *o, int pa, int pb, const char *s, size_t n, bool imp) {
    const char *cs[MAXCOMP];
    size_t cl[MAXCOMP];
    int c = split_components(s, n, cs, cl, false);
    if (c < 1 || c > 2) return;
    cval_t a, b;
    if (!parse_component(cs[0], cl[0], &a)) return;
    b = a;
    if (c == 2 && !parse_component(cs[1], cl[1], &b)) return;
    out_add(o, pa, &a, imp);
    out_add(o, pb, &b, imp);
}

/* box alignment keywords (align-items, justify-self, ...): the K_* to use, 0 for a
   CSS-wide keyword, -1 if not understood */
static int align_kw(const char *s, size_t n) {
    const char *cs[MAXCOMP];
    size_t cl[MAXCOMP];
    int c = split_components(s, n, cs, cl, false), k = -1;
    for (int i = 0; i < c; i++) {
        const char *w = cs[i];
        size_t l = cl[i];
#define IS(x) (l == sizeof(x) - 1 && !strncasecmp(w, x, l))
        if (IS("safe") || IS("unsafe") || IS("first") || IS("legacy")) continue;
        if (IS("last")) continue;
        if (IS("inherit") || IS("initial") || IS("unset") || IS("revert") || IS("revert-layer")) return c == 1 ? 0 : -1;
        if (IS("auto")) k = K_AUTO;
        else if (IS("normal")) k = K_NORMAL;
        else if (IS("stretch")) k = K_STRETCH;
        else if (IS("center")) k = K_CENTER;
        else if (IS("start") || IS("self-start") || IS("flex-start") || IS("left") || IS("top")) k = K_START;
        else if (IS("end") || IS("self-end") || IS("flex-end") || IS("right") || IS("bottom")) k = K_END;
        else if (IS("baseline")) k = K_BASELINE;
        else if (IS("space-between")) k = K_SPACE_BETWEEN;
        else if (IS("space-around")) k = K_SPACE_AROUND;
        else if (IS("space-evenly")) k = K_SPACE_EVENLY;
        else return -1;
#undef IS
    }
    return k;
}

/* grid-row-start etc.: auto | <int> | span <int> | <name>; CV_NUM with the encoded line
   (0 auto, n, -n, GSPAN + n), or CV_STR for a name */
static bool grid_line(const char *s, size_t n, cval_t *v) {
    const char *cs[MAXCOMP];
    size_t cl[MAXCOMP];
    int c = split_components(s, n, cs, cl, false);
    memset(v, 0, sizeof *v);
    v->t = CV_NUM;
    if (c < 1) return false;
    bool span = false;
    int num = 0;
    const char *name = NULL;
    size_t namel = 0;
    for (int i = 0; i < c; i++) {
        if (cl[i] == 4 && !strncasecmp(cs[i], "auto", 4)) { if (c == 1) return true; continue; }
        if (cl[i] == 4 && !strncasecmp(cs[i], "span", 4)) { span = true; continue; }
        char *e;
        long x = strtol(cs[i], &e, 10);
        if (e == cs[i] + cl[i] && cl[i]) { num = (int)CLAMP(x, -999, 999); continue; }
        if (isalpha((unsigned char)cs[i][0]) || cs[i][0] == '_' || cs[i][0] == '-') { name = cs[i]; namel = cl[i]; continue; }
        return false;
    }
    if (name && !num && !span) { v->t = CV_STR; v->raw = name; v->f = (float)namel; return true; }
    if (span) v->f = (float)(GSPAN + (num > 0 ? num : 1));
    else v->f = (float)num;
    return true;
}

static const int P_INSETS[4] = { P_TOP, P_RIGHT, P_BOTTOM, P_LEFT };
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
    /* var() and env() are resolved at cascade time */
    for (size_t i = 0; i + 4 <= n; i++)
        if (!strncasecmp(s + i, "var(", 4) || !strncasecmp(s + i, "env(", 4)) {
            v.t = CV_RAW;
            v.raw = s;            /* caller copies into the arena */
            v.f = (float)n;
            out_add(o, prop, &v, imp);
            return;
        }
    switch (prop) {
    case P_INSET: four(o, P_INSETS, s, n, imp); return;
    case P_PADDING_INLINE: two(o, P_PL, P_PR, s, n, imp); return;
    case P_PADDING_BLOCK: two(o, P_PT, P_PB, s, n, imp); return;
    case P_MARGIN_INLINE: two(o, P_ML, P_MR, s, n, imp); return;
    case P_MARGIN_BLOCK: two(o, P_MT, P_MB, s, n, imp); return;
    case P_INSET_INLINE: two(o, P_LEFT, P_RIGHT, s, n, imp); return;
    case P_INSET_BLOCK: two(o, P_TOP, P_BOTTOM, s, n, imp); return;
    case P_BORDER_INLINE_WIDTH: two(o, P_BLW, P_BRW, s, n, imp); return;
    case P_BORDER_BLOCK_WIDTH: two(o, P_BTW, P_BBW, s, n, imp); return;
    case P_BORDER_INLINE_COLOR: two(o, P_BLC, P_BRC, s, n, imp); return;
    case P_BORDER_BLOCK_COLOR: two(o, P_BTC, P_BBC, s, n, imp); return;
    case P_BORDER_INLINE_STYLE: two(o, P_BLS, P_BRS, s, n, imp); return;
    case P_BORDER_BLOCK_STYLE: two(o, P_BTS, P_BBS, s, n, imp); return;
    case P_BORDER_INLINE: border_side(o, 3, s, n, imp); border_side(o, 1, s, n, imp); return;
    case P_BORDER_BLOCK: border_side(o, 0, s, n, imp); border_side(o, 2, s, n, imp); return;
    case P_RADIUS: {
        const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
        int c = split_components(s, n, cs, cl, true), k = 0;
        cval_t r[4];
        for (int i = 0; i < c && k < 4; i++) {
            if (cl[i] == 1 && cs[i][0] == '/') break;     /* vertical radii: ignored */
            if (!parse_component(cs[i], cl[i], &r[k]) || (r[k].t != CV_LEN && r[k].t != CV_INHERIT && r[k].t != CV_INITIAL)) return;
            k++;
        }
        if (!k) return;
        if (k == 1) r[1] = r[2] = r[3] = r[0];
        else if (k == 2) { r[2] = r[0]; r[3] = r[1]; }
        else if (k == 3) r[3] = r[1];
        for (int i = 0; i < 4; i++) out_add(o, P_RADIUS_TL + i, &r[i], imp);
        return;
    }
    case P_RADIUS_TL: case P_RADIUS_TR: case P_RADIUS_BR: case P_RADIUS_BL: {
        const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
        int c = split_components(s, n, cs, cl, false);
        if (c >= 1 && parse_component(cs[0], cl[0], &v)) out_add(o, prop, &v, imp);
        return;
    }
    case P_ALIGN_ITEMS: case P_ALIGN_SELF: case P_JUSTIFY_SELF: case P_JUSTIFY_ITEMS:
    case P_ALIGN_CONTENT: case P_JUSTIFY_CONTENT: {
        int k = align_kw(s, n);
        if (k < 0) return;
        if (k == 0) { if (!parse_component(s, n, &v) || (v.t != CV_INHERIT && v.t != CV_INITIAL)) return; }
        else { v.t = CV_KW; v.kw = (int16_t)k; }
        out_add(o, prop, &v, imp);
        return;
    }
    case P_PLACE_ITEMS: case P_PLACE_SELF: case P_PLACE_CONTENT: {
        const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
        int c = split_components(s, n, cs, cl, false);
        static const int pa[3] = { P_ALIGN_ITEMS, P_ALIGN_SELF, P_ALIGN_CONTENT }, pj[3] = { P_JUSTIFY_ITEMS, P_JUSTIFY_SELF, P_JUSTIFY_CONTENT };
        int w = prop == P_PLACE_ITEMS ? 0 : prop == P_PLACE_SELF ? 1 : 2;
        if (c < 1) return;
        /* "first baseline" / "safe center" take two words: split on the last one */
        size_t al = c >= 2 ? (size_t)(cs[c - 1] - s) : n;
        if (c >= 3 || (c == 2 && (align_kw(cs[0], cl[0]) <= 0 || align_kw(cs[1], cl[1]) <= 0))) al = n;
        int ka = align_kw(s, al), kj = al < n ? align_kw(s + al, n - al) : ka;
        if (ka <= 0 || kj <= 0) return;
        v.t = CV_KW; v.kw = (int16_t)ka; out_add(o, pa[w], &v, imp);
        v.kw = (int16_t)kj; out_add(o, pj[w], &v, imp);
        return;
    }
    case P_MASK_IMAGE:
        /* only whether there is a mask matters (none / a url or gradient) */
        if (n == 4 && !strncasecmp(s, "none", 4)) { v.t = CV_NUM; v.f = 0; }
        else if (!parse_component(s, n, &v) || (v.t != CV_INHERIT && v.t != CV_INITIAL)) { v.t = CV_NUM; v.f = 1; }
        out_add(o, prop, &v, imp);
        return;
    case P_ISOLATION:
        if (n == 7 && !strncasecmp(s, "isolate", 7)) { v.t = CV_NUM; v.f = 1; }
        else if (n == 4 && !strncasecmp(s, "auto", 4)) { v.t = CV_NUM; v.f = 0; }
        else if (!parse_component(s, n, &v) || (v.t != CV_INHERIT && v.t != CV_INITIAL)) return;
        out_add(o, prop, &v, imp);
        return;
    case P_ZINDEX: case P_ORDER: case P_FLEX_GROW: case P_FLEX_SHRINK:
        if (!parse_component(s, n, &v)) return;
        if (v.t == CV_LEN && v.unit != UN_PX) return;     /* plain numbers only */
        out_add(o, prop, &v, imp);
        return;
    case P_ASPECT_RATIO: {
        const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
        int c = split_components(s, n, cs, cl, true);
        float num[2] = { 0, 1 };
        int k = 0;
        bool slash = false;
        for (int i = 0; i < c; i++) {
            if (cl[i] == 4 && !strncasecmp(cs[i], "auto", 4)) continue;
            if (cl[i] == 1 && cs[i][0] == '/') { slash = true; continue; }
            if (!parse_component(cs[i], cl[i], &v) || (v.t != CV_NUM && !(v.t == CV_LEN && v.f == 0))) return;
            if (k < 2) num[k++] = v.f;
        }
        if (!k) { v.t = CV_KW; v.kw = K_AUTO; out_add(o, prop, &v, imp); return; }
        if (k == 2 && !slash) return;
        v.t = CV_NUM;
        v.f = num[1] > 0 ? num[0] / num[1] : 0;
        out_add(o, prop, &v, imp);
        return;
    }
    case P_GRID_COLS: case P_GRID_ROWS: case P_GRID_AUTO_COLS: case P_GRID_AUTO_ROWS: case P_GRID_AREAS:
    case P_TRANSFORM: case P_TRANSLATE:
        /* parsed when the style is computed (lengths need the element's font size) */
        v.t = CV_STR;
        v.raw = s;
        v.f = (float)n;
        out_add(o, prop, &v, imp);
        return;
    case P_GRID_TEMPLATE: {
        /* [ <areas+rows> | <rows> ] / <columns> */
        const char *sl = NULL;
        int depth = 0;
        char q = 0;
        for (size_t i = 0; i < n; i++) {
            char ch = s[i];
            if (q) { if (ch == q) q = 0; continue; }
            if (ch == '"' || ch == '\'') q = ch;
            else if (ch == '(' || ch == '[') depth++;
            else if ((ch == ')' || ch == ']') && depth) depth--;
            else if (ch == '/' && !depth) { sl = s + i; break; }
        }
        if (n == 4 && !strncasecmp(s, "none", 4)) {
            v.t = CV_STR; v.raw = s; v.f = 4;
            out_add(o, P_GRID_ROWS, &v, imp); out_add(o, P_GRID_COLS, &v, imp); out_add(o, P_GRID_AREAS, &v, imp);
            return;
        }
        if (!sl) return;
        v.t = CV_STR; v.raw = s; v.f = (float)(sl - s);
        out_add(o, P_GRID_ROWS, &v, imp);          /* the row part may hold area strings */
        if (memchr(s, '"', (size_t)(sl - s))) out_add(o, P_GRID_AREAS, &v, imp);
        v.raw = sl + 1; v.f = (float)(s + n - sl - 1);
        out_add(o, P_GRID_COLS, &v, imp);
        return;
    }
    case P_GRID_AUTO_FLOW: {
        const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
        int c = split_components(s, n, cs, cl, false), f = 0;
        for (int i = 0; i < c; i++) {
            if (cl[i] == 6 && !strncasecmp(cs[i], "column", 6)) f |= 1;
            else if (cl[i] == 5 && !strncasecmp(cs[i], "dense", 5)) f |= 2;
            else if (!(cl[i] == 3 && !strncasecmp(cs[i], "row", 3))) return;
        }
        v.t = CV_NUM; v.f = (float)f;
        out_add(o, prop, &v, imp);
        return;
    }
    case P_GRID_COL_START: case P_GRID_COL_END: case P_GRID_ROW_START: case P_GRID_ROW_END:
        if (grid_line(s, n, &v)) out_add(o, prop, &v, imp);
        return;
    case P_GRID_COLUMN: case P_GRID_ROW: case P_GRID_AREA: {
        /* a / b [/ c / d] */
        const char *part[4]; size_t pl[4];
        int k = 0;
        const char *ps = s;
        for (size_t i = 0; i <= n && k < 4; i++)
            if (i == n || s[i] == '/') { part[k] = ps; pl[k] = (size_t)(s + i - ps); k++; ps = s + i + 1; }
        cval_t lv[4];
        for (int i = 0; i < k; i++) if (!grid_line(part[i], pl[i], &lv[i])) return;
        if (prop == P_GRID_AREA && k == 1 && lv[0].t == CV_STR) {
            /* grid-area: <name> */
            out_add(o, P_GRID_AREA_NAME, &lv[0], imp);
            cval_t a = { .t = CV_NUM, .f = 0 };
            out_add(o, P_GRID_ROW_START, &a, imp); out_add(o, P_GRID_COL_START, &a, imp);
            out_add(o, P_GRID_ROW_END, &a, imp); out_add(o, P_GRID_COL_END, &a, imp);
            return;
        }
        cval_t none = { .t = CV_STR, .raw = NULL };
        if (prop == P_GRID_AREA) {
            /* row-start / column-start / row-end / column-end */
            static const int ids[4] = { P_GRID_ROW_START, P_GRID_COL_START, P_GRID_ROW_END, P_GRID_COL_END };
            out_add(o, P_GRID_AREA_NAME, &none, imp);
            for (int i = 0; i < 4; i++) {
                cval_t x = i < k ? lv[i] : (cval_t){ .t = CV_NUM, .f = 0 };
                if (x.t == CV_STR) x = (cval_t){ .t = CV_NUM, .f = 0 };
                out_add(o, ids[i], &x, imp);
            }
            return;
        }
        int a = prop == P_GRID_COLUMN ? P_GRID_COL_START : P_GRID_ROW_START;
        cval_t x0 = lv[0], x1 = k >= 2 ? lv[1] : (cval_t){ .t = CV_NUM, .f = 0 };
        if (x0.t == CV_STR) x0 = (cval_t){ .t = CV_NUM, .f = 0 };
        if (x1.t == CV_STR) x1 = (cval_t){ .t = CV_NUM, .f = 0 };
        out_add(o, a, &x0, imp);
        out_add(o, a + 1, &x1, imp);
        out_add(o, P_GRID_AREA_NAME, &none, imp);
        return;
    }
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
       PS_NTH_LAST = 2048, PS_ALLIS = 4096 /* every entry of `iss` must match (several :is()) */ };

typedef struct attrsel { const char *name, *val; uint8_t op; uint8_t icase; } attrsel_t;
enum { AO_EXISTS = 0, AO_EQ, AO_WORD, AO_DASH, AO_PREFIX, AO_SUFFIX, AO_SUB };

typedef struct compound compound_t;
/* a complex selector inside :is() / :where() / :not() */
typedef struct subsel { compound_t *parts; int nparts; } subsel_t;
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
    subsel_t *nots;           /* :not(...) - none may match */
    subsel_t *iss;            /* :is(...) / :where(...) - one must match */
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
    bool     sel_unsup;       /* the last selector parsed used something unknown */
    int      nest;            /* :is() nesting while parsing */
};

static uint32_t hash_str(const char *s, char prefix) {
    uint32_t h = 2166136261u ^ (uint8_t)prefix;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

/* Parse one compound at *pp. */
static bool parse_compound(web_sheet_t *sh, const char **pp, const char *end, compound_t *c, uint32_t *spec);

static bool parse_selector(web_sheet_t *sh, const char *s, const char *e, compound_t **parts, int *nparts, uint32_t *spec);

/* the selector list of :is() / :where() / :not(), *pp just after the '(';
   forgiving: arguments that do not parse are dropped (unless none parse) */
static bool parse_sel_list_in_parens(web_sheet_t *sh, const char **pp, const char *end, subsel_t **out, uint8_t *nout, uint32_t *maxspec) {
    const char *p = *pp, *close = p;
    int depth = 1;
    char q = 0;
    for (; close < end; close++) {
        char ch = *close;
        if (q) { if (ch == '\\') { close++; continue; } if (ch == q) q = 0; continue; }
        if (ch == '"' || ch == '\'') q = ch;
        else if (ch == '\\') close++;
        else if (ch == '(') depth++;
        else if (ch == ')' && --depth == 0) break;
    }
    if (close >= end || sh->nest > 8) return false;
    subsel_t tmp[16];
    int n = 0, args = 0;
    *maxspec = 0;
    sh->nest++;
    const char *s = p;
    while (s < close) {
        const char *e = s;
        int d = 0;
        while (e < close && (d || *e != ',')) {
            if (*e == '\\' && e + 1 < close) { e += 2; continue; }
            if (*e == '(') d++;
            else if (*e == ')') d--;
            e++;
        }
        const char *a = s, *b = e;
        while (a < b && web_isspace((unsigned char)*a)) a++;
        while (b > a && web_isspace((unsigned char)b[-1])) b--;
        args++;
        compound_t *parts;
        int np;
        uint32_t sp = 0;
        if (b > a && parse_selector(sh, a, b, &parts, &np, &sp)) {
            if (n < 16) { tmp[n].parts = parts; tmp[n].nparts = np; n++; }
            if (sp > *maxspec) *maxspec = sp;
        }
        s = e < close ? e + 1 : close;
    }
    sh->nest--;
    if (!n && args) return false;
    *out = wa_alloc(&sh->ar, sizeof(subsel_t) * (n ? n : 1));
    if (!*out) return false;
    memcpy(*out, tmp, sizeof(subsel_t) * n);
    *nout = (uint8_t)n;
    *pp = close + 1;
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
                if (elem && strcmp(nm, "before") && strcmp(nm, "after") && strcmp(nm, "first-line") &&
                    strcmp(nm, "first-letter") && strcmp(nm, "placeholder") && strcmp(nm, "selection") &&
                    strcmp(nm, "marker") && strcmp(nm, "backdrop") && strncmp(nm, "-webkit-", 8) &&
                    strncmp(nm, "-moz-", 5))
                    sh->sel_unsup = true;     /* ::picker(), ::part() ... (for @supports selector()) */
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
                    subsel_t *lst; uint8_t n; uint32_t ms;
                    if (!parse_sel_list_in_parens(sh, &p, end, &lst, &n, &ms)) return false;
                    if (c->nnot) {
                        /* :not(A):not(B) = none of A, B may match: one list */
                        if (c->nnot + n > 64) return false;
                        subsel_t *all = wa_alloc(&sh->ar, sizeof(subsel_t) * (size_t)(c->nnot + n));
                        if (!all) return false;
                        memcpy(all, c->nots, sizeof(subsel_t) * c->nnot);
                        memcpy(all + c->nnot, lst, sizeof(subsel_t) * n);
                        c->nots = all;
                        c->nnot = (uint8_t)(c->nnot + n);
                    } else {
                        c->nots = lst; c->nnot = n;
                    }
                    *spec += ms - (1 << 8);
                } else if (!strcmp(nm, "is") || !strcmp(nm, "matches") || !strcmp(nm, "where") ||
                           !strcmp(nm, "-webkit-any") || !strcmp(nm, "-moz-any")) {
                    subsel_t *lst; uint8_t n; uint32_t ms;
                    if (!parse_sel_list_in_parens(sh, &p, end, &lst, &n, &ms)) return false;
                    if (!c->nis) {
                        c->iss = lst; c->nis = n;
                    } else {
                        /* several :is() / :where(): each list must match - every list becomes
                           one entry (a compound holding it) of an all-must-match list */
                        compound_t *w = wa_alloc(&sh->ar, sizeof(compound_t) * 2);
                        if (!w || c->nis >= 32) return false;
                        memset(w, 0, sizeof(compound_t) * 2);
                        int old = (c->pseudo & PS_ALLIS) ? c->nis : 1;
                        subsel_t *arr = wa_alloc(&sh->ar, sizeof(subsel_t) * (size_t)(old + 1));
                        if (!arr) return false;
                        if (c->pseudo & PS_ALLIS) memcpy(arr, c->iss, sizeof(subsel_t) * (size_t)old);
                        else {
                            w[0].any = 1; w[0].iss = c->iss; w[0].nis = c->nis;
                            arr[0].parts = &w[0]; arr[0].nparts = 1;
                        }
                        w[1].any = 1; w[1].iss = lst; w[1].nis = n;
                        arr[old].parts = &w[1]; arr[old].nparts = 1;
                        c->iss = arr;
                        c->nis = (uint8_t)(old + 1);
                        c->pseudo |= PS_ALLIS;
                    }
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
                        if (strcmp(nm, "nth-of-type") && strcmp(nm, "nth-last-of-type") && strcmp(nm, "lang") &&
                            strcmp(nm, "dir")) sh->sel_unsup = true;
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
            else {
                c->pseudo |= PS_NEVER;      /* :hover, :focus, :visited, :active ... */
                static const char *const known[] = {
                    "hover", "focus", "active", "visited", "focus-visible", "focus-within", "target",
                    "last-of-type", "only-of-type", "required", "optional", "invalid", "valid",
                    "read-only", "read-write", "placeholder-shown", "default", "indeterminate",
                    "fullscreen", "defined", "autofill", "in-range", "out-of-range", "scope",
                };
                bool k = !strncmp(nm, "-webkit-", 8) || !strncmp(nm, "-moz-", 5);
                for (int i = 0; i < ARRAY_SIZE(known) && !k; i++) if (!strcmp(nm, known[i])) k = true;
                if (!k) sh->sel_unsup = true;
            }
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
        if (prop == P_NONE || prop == P_IGNORED) continue;
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
        /* values that keep their text: copy it (NUL-terminated) into the arena */
        for (int i = before; i < o.n; i++) {
            cval_t *cv = &o.d[i].v;
            if (cv->t == CV_RAW) {
                cv->raw = wa_strndup(&sh->ar, vs, (size_t)(ve - vs));
                cv->f = 0;
            } else if (cv->t == CV_STR && cv->raw) {
                size_t l = (size_t)cv->f;
                cv->raw = wa_strndup(&sh->ar, cv->raw, l);
            } else if (cv->t == CV_LEN && cv->unit == UN_CALC && cv->raw) {
                cv->raw = wa_strndup(&sh->ar, cv->raw, cv->color);
            }
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

/* ---- @supports ------------------------------------------------------------------------------------- */
static const char *match_paren(const char *p, const char *e) {
    /* p at '(': the matching ')' (or e) */
    int depth = 0;
    char q = 0;
    for (; p < e; p++) {
        if (q) { if (*p == '\\') { p++; continue; } if (*p == q) q = 0; continue; }
        if (*p == '"' || *p == '\'') { q = *p; continue; }
        if (*p == '(') depth++;
        else if (*p == ')' && --depth == 0) return p;
    }
    return e;
}

static bool word_at(const char *p, const char *e, const char *w) {
    size_t n = strlen(w);
    return (size_t)(e - p) > n && !strncasecmp(p, w, n) && (web_isspace((unsigned char)p[n]) || p[n] == '(');
}

/* "prop: value" - supported when the property is known and the value understood */
static bool supports_decl(const char *s, const char *e) {
    const char *colon = memchr(s, ':', (size_t)(e - s));
    if (!colon) return false;
    const char *ns = c_ws(s, colon), *ne = colon;
    while (ne > ns && web_isspace((unsigned char)ne[-1])) ne--;
    const char *vs = c_ws(colon + 1, e), *ve = e;
    while (ve > vs && web_isspace((unsigned char)ve[-1])) ve--;
    if (ve <= vs || ne <= ns) return false;
    int prop = prop_lookup(ns, (size_t)(ne - ns));
    if (prop == P_CUSTOM || prop == P_IGNORED) return true;
    if (prop == P_NONE) return false;
    decl_t tmp[64];
    dout_t o = { tmp, 0, ARRAY_SIZE(tmp) };
    parse_decl(&o, prop, vs, (size_t)(ve - vs), false);
    return o.n > 0;
}

static bool supports_cond(web_sheet_t *sh, const char **pp, const char *e, int depth);

static bool supports_in_parens(web_sheet_t *sh, const char **pp, const char *e, int depth) {
    const char *p = c_ws(*pp, e);
    if (p < e && *p == '(') {
        const char *q = match_paren(p, e);
        const char *is = p + 1, *ie = q;
        *pp = q < e ? q + 1 : e;
        const char *t = c_ws(is, ie);
        if (t < ie && (*t == '(' || word_at(t, ie, "not") || word_at(t, ie, "selector") || word_at(t, ie, "font-tech") ||
                       word_at(t, ie, "font-format"))) {
            const char *pp2 = t;
            bool r = supports_cond(sh, &pp2, ie, depth + 1);
            return r && c_ws(pp2, ie) == ie;
        }
        return supports_decl(is, ie);
    }
    if (word_at(p, e, "selector") && p[8] == '(') {
        const char *q = match_paren(p + 8, e);
        compound_t *parts;
        int np;
        uint32_t spec;
        sh->sel_unsup = false;
        const char *ss = c_ws(p + 9, q), *se = q;
        while (se > ss && web_isspace((unsigned char)se[-1])) se--;
        bool ok = parse_selector(sh, ss, se, &parts, &np, &spec) && !sh->sel_unsup;
        *pp = q < e ? q + 1 : e;
        return ok;
    }
    /* anything else (font-tech(), unknown functions): false */
    while (p < e && *p != '(' && !web_isspace((unsigned char)*p)) p++;
    if (p < e && *p == '(') { const char *q = match_paren(p, e); p = q < e ? q + 1 : e; }
    *pp = p;
    return false;
}

static bool supports_cond(web_sheet_t *sh, const char **pp, const char *e, int depth) {
    const char *p = c_ws(*pp, e);
    if (depth > 16) { *pp = e; return false; }
    if (word_at(p, e, "not")) {
        p += 3;
        bool r = !supports_in_parens(sh, &p, e, depth);
        *pp = p;
        return r;
    }
    bool r = supports_in_parens(sh, &p, e, depth);
    for (;;) {
        p = c_ws(p, e);
        if (word_at(p, e, "and")) { p += 3; bool b = supports_in_parens(sh, &p, e, depth); r = r && b; }
        else if (word_at(p, e, "or")) { p += 2; bool b = supports_in_parens(sh, &p, e, depth); r = r || b; }
        else break;
    }
    *pp = p;
    return r;
}

static bool supports_match(web_sheet_t *sh, const char *s, const char *e) {
    const char *p = s;
    bool r = supports_cond(sh, &p, e, 0);
    return r && c_ws(p, e) == e;
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
            } else if (al >= 8 && !strncasecmp(as, "supports", 8)) {
                if (supports_match(sh, as + 8, ae)) parse_rules(sh, bs, be, media);
            } else if ((al >= 5 && !strncasecmp(as, "layer", 5)) || (al >= 8 && !strncasecmp(as, "document", 8)) ||
                       (al >= 5 && !strncasecmp(as, "scope", 5))) {
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

static bool match_from(const compound_t *parts, int k, const web_node_t *n);

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
    for (int i = 0; i < c->nnot; i++) if (match_from(c->nots[i].parts, c->nots[i].nparts - 1, n)) return false;
    if (c->nis) {
        if (c->pseudo & PS_ALLIS) {
            for (int i = 0; i < c->nis; i++) if (!match_from(c->iss[i].parts, c->iss[i].nparts - 1, n)) return false;
        } else {
            bool any = false;
            for (int i = 0; i < c->nis && !any; i++) any = match_from(c->iss[i].parts, c->iss[i].nparts - 1, n);
            if (!any) return false;
        }
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
    "noscript{display:block}"
    "details:not([open])>:not(summary){display:none}summary{display:block}";

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
    /* keys: the id, every class (utility-class pages use dozens), the tag name */
    int nk = n->ncls + 2;
    for (int k = 0; k < nk; k++) {
        const char *key;
        char kp;
        if (k == 0) { key = n->id; kp = '#'; }
        else if (k <= n->ncls) { key = n->cls[k - 1]; kp = '.'; }
        else { key = n->name; kp = 't'; }
        if (!key) continue;
        uint32_t h = hash_str(key, kp) & (NBUCKETS - 1);
        for (idx_node_t *in = sh->buckets[h]; in; in = in->next) {
            if (strcmp(in->key, key)) continue;
            const rule_t *r = &sh->rules[in->rule];
            /* make sure the rule's key kind matches (a class "p" vs tag "p") */
            const compound_t *last = &r->parts[r->nparts - 1];
            char kind = last->id ? '#' : last->ncls ? '.' : 't';
            if (kind != kp) continue;
            if (kind == '.' && strcmp(last->cls[0], key)) continue;
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
static int line_px(const web_style_t *st) {
    return st->lh > 0 ? st->lh : st->lh < 0 ? -st->lh * st->fsize / 100 : st->fsize * 6 / 5;
}

static void cenv_init(cctx_t *cx, const web_style_t *st, const web_style_t *parent, bool for_font, cenv_t *e) {
    e->em = for_font ? parent->fsize : st->fsize;
    e->rem = cx->root_fs;
    e->vw = cx->c->vw;
    e->vh = cx->c->vh;
    e->zoom = cx->c->zoom;
    e->lh = line_px(for_font ? parent : st);
}

/* a calc() value: px + pc percent */
static bool calc_value(cctx_t *cx, const web_style_t *st, const web_style_t *parent, const cval_t *v, bool for_font, cterm_t *t) {
    cenv_t e;
    cenv_init(cx, st, parent, for_font, &e);
    return v->raw && calc_eval(&e, v->raw, v->color, t);
}

static int to_px(cctx_t *cx, const web_style_t *st, const web_style_t *parent, const cval_t *v, int pct_base, bool for_font) {
    float f = v->f;
    int zoom = cx->c->zoom;
    switch (v->unit) {
    case UN_VMIN: return (int)lroundf(f * MIN(cx->c->vw, cx->c->vh) / 100.0f);
    case UN_VMAX: return (int)lroundf(f * MAX(cx->c->vw, cx->c->vh) / 100.0f);
    case UN_LH: return (int)lroundf(f * line_px(for_font ? parent : st));
    case UN_CALC: {
        cterm_t t;
        if (!calc_value(cx, st, parent, v, for_font, &t)) return 0;
        return (int)lroundf(t.px + t.pc * pct_base / 100.0f);
    }
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
    if (v->unit == UN_CALC) {
        cterm_t t;
        if (!calc_value(cx, st, parent, v, false, &t)) { l.u = allow_auto ? U_AUTO : U_PX; return l; }
        l.v = (int16_t)CLAMP(lroundf(t.px), -30000, 30000);
        if (fabsf(t.pc) >= 0.05f) {
            l.pc = (int16_t)CLAMP(lroundf(t.pc * 10), -30000, 30000);
            if (l.v == 0) { l.u = U_PCT; l.v = l.pc; l.pc = 0; }
            else l.u = U_CALC;
        }
        return l;
    }
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
        if (strlen(v->name) == nl && !strncmp(v->name, name, nl)) {
            /* "initial" is the guaranteed-invalid value: var() takes its fallback
               (the light-dark() polyfill relies on it) */
            const char *s = v->value;
            while (*s == ' ') s++;
            if (!strncasecmp(s, "initial", 7)) {
                const char *e = s + 7;
                while (*e == ' ') e++;
                if (!*e) return NULL;
            }
            return v->value;
        }
    return NULL;
}

/* Substitute var() in s into out (size). */
static bool subst_vars(const web_style_t *st, const char *s, char *out, size_t size, int depth) {
    size_t o = 0;
    if (depth > 6) return false;
    while (*s && o + 1 < size) {
        if (!strncasecmp(s, "env(", 4)) {
            /* env(name[, fallback]): no safe areas / keyboard here - the fallback or 0px */
            const char *q = s + 4, *fb = NULL;
            int dpt = 0;
            while (*q && (dpt || *q != ')')) {
                if (*q == '(') dpt++;
                else if (*q == ')') dpt--;
                else if (*q == ',' && !dpt && !fb) fb = q + 1;
                q++;
            }
            char tmp[256], fbuf[256];
            if (fb) {
                size_t fl = (size_t)(q - fb);
                if (fl >= sizeof fbuf) fl = sizeof fbuf - 1;
                memcpy(fbuf, fb, fl);
                fbuf[fl] = 0;
                if (!subst_vars(st, fbuf, tmp, sizeof tmp, depth + 1)) return false;
            } else strcpy(tmp, "0px");
            size_t tl = strlen(tmp);
            if (o + tl >= size) return false;
            memcpy(out + o, tmp, tl);
            o += tl;
            s = *q ? q + 1 : q;
            continue;
        }
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


/* ---- transforms ---- */
/* one translate argument: adds to *px / *pc (percent of the box's own size) */
static bool tr_arg(cctx_t *cx, const web_style_t *st, const web_style_t *ps, const char *s, size_t n, float *px, float *pc) {
    cval_t v;
    memset(&v, 0, sizeof v);
    if (!parse_len(s, n, &v)) return false;
    if (v.t == CV_NUM) return v.f == 0;
    if (v.unit == UN_PCT) { *pc += v.f; return true; }
    if (v.unit == UN_CALC) {
        cterm_t t;
        if (!calc_value(cx, st, ps, &v, false, &t)) return false;
        *px += t.px; *pc += t.pc;
        return true;
    }
    *px += (float)to_px(cx, st, ps, &v, 0, false);
    return true;
}

static web_len_t mk_len(float px, float pc) {
    web_len_t l = { (int16_t)CLAMP(lroundf(px), -30000, 30000), U_PX, 0 };
    if (fabsf(pc) >= 0.05f) {
        int16_t t = (int16_t)CLAMP(lroundf(pc * 10), -30000, 30000);
        if (l.v == 0) { l.u = U_PCT; l.v = t; }
        else { l.u = U_CALC; l.pc = t; }
    }
    return l;
}

/* transform: translate*() and matrix() move the box; scale / rotate are ignored */
static void apply_transform(cctx_t *cx, web_style_t *st, const web_style_t *ps, const char *s, size_t n, bool prop_translate) {
    float xp = 0, xc = 0, yp = 0, yc = 0;
    const char *e = s + n, *p = c_ws(s, e);
    if (!(e - p == 4 && !strncasecmp(p, "none", 4))) {
        if (prop_translate) {
            const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
            int c = split_components(p, (size_t)(e - p), cs, cl, false);
            if (c >= 1 && !tr_arg(cx, st, ps, cs[0], cl[0], &xp, &xc)) return;
            if (c >= 2 && !tr_arg(cx, st, ps, cs[1], cl[1], &yp, &yc)) return;
        } else {
            while (p < e) {
                p = c_ws(p, e);
                const char *ns = p;
                while (p < e && *p != '(' && !web_isspace((unsigned char)*p)) p++;
                size_t nl = (size_t)(p - ns);
                if (p >= e || *p != '(') break;
                const char *q = match_paren(p, e);
                const char *as = p + 1;
                const char *args[16]; size_t al[16];
                int na = 0;
                const char *a0 = as;
                int depth = 0;
                for (const char *t = as; t <= q && na < 16; t++) {
                    if (t < q && *t == '(') depth++;
                    else if (t < q && *t == ')') depth--;
                    else if (t == q || (*t == ',' && !depth)) { args[na] = a0; al[na] = (size_t)(t - a0); na++; a0 = t + 1; }
                }
                if ((nl == 9 && !strncasecmp(ns, "translate", 9)) || (nl == 11 && !strncasecmp(ns, "translate3d", 11))) {
                    if (na >= 1) tr_arg(cx, st, ps, args[0], al[0], &xp, &xc);
                    if (na >= 2) tr_arg(cx, st, ps, args[1], al[1], &yp, &yc);
                } else if (nl == 10 && !strncasecmp(ns, "translateX", 10) && na >= 1) tr_arg(cx, st, ps, args[0], al[0], &xp, &xc);
                else if (nl == 10 && !strncasecmp(ns, "translateY", 10) && na >= 1) tr_arg(cx, st, ps, args[0], al[0], &yp, &yc);
                else if (nl == 6 && !strncasecmp(ns, "matrix", 6) && na == 6) {
                    xp += (float)atof(args[4]) * cx->c->zoom / 100.0f;
                    yp += (float)atof(args[5]) * cx->c->zoom / 100.0f;
                } else if (nl == 8 && !strncasecmp(ns, "matrix3d", 8) && na == 16) {
                    xp += (float)atof(args[12]) * cx->c->zoom / 100.0f;
                    yp += (float)atof(args[13]) * cx->c->zoom / 100.0f;
                }
                p = q < e ? q + 1 : e;
            }
        }
    }
    st->tx = mk_len(xp, xc);
    st->ty = mk_len(yp, yc);
    /* slid out of view (off-canvas menus, dropdowns waiting for a script) */
    st->offscreen = xc <= -100 || xp <= -500 || yc <= -100 || yp <= -500 || xp + xc * cx->c->vw / 100 <= -cx->c->vw;
}

/* ---- grid templates ---- */
static bool track_kind(cctx_t *cx, const web_style_t *st, const web_style_t *ps, const char *s, size_t n, uint8_t *k, int16_t *val) {
    while (n && web_isspace((unsigned char)*s)) { s++; n--; }
    while (n && web_isspace((unsigned char)s[n - 1])) n--;
    *val = 0;
    if (n == 4 && !strncasecmp(s, "auto", 4)) { *k = GT_AUTO; return true; }
    if (n == 11 && !strncasecmp(s, "min-content", 11)) { *k = GT_MIN; return true; }
    if (n == 11 && !strncasecmp(s, "max-content", 11)) { *k = GT_MAX; return true; }
    if (n > 2 && !strncasecmp(s + n - 2, "fr", 2)) {
        char *e;
        double f = strtod(s, &e);
        if (e != s + n - 2 || f < 0) return false;
        *k = GT_FR;
        *val = (int16_t)CLAMP(lround(f * 100), 0, 30000);
        return true;
    }
    cval_t v;
    memset(&v, 0, sizeof v);
    if (!parse_len(s, n, &v)) return false;
    if (v.t == CV_NUM) { if (v.f != 0) return false; *k = GT_PX; return true; }
    if (v.unit == UN_PCT) { *k = GT_PCT; *val = (int16_t)CLAMP(lroundf(v.f * 10), 0, 30000); return true; }
    if (v.unit == UN_CALC) {
        cterm_t t;
        if (!calc_value(cx, st, ps, &v, false, &t)) return false;
        if (fabsf(t.pc) >= 0.05f && fabsf(t.px) < 0.5f) { *k = GT_PCT; *val = (int16_t)CLAMP(lroundf(t.pc * 10), 0, 30000); }
        else { *k = GT_PX; *val = (int16_t)CLAMP(lroundf(t.px), 0, 30000); }
        return true;
    }
    *k = GT_PX;
    *val = (int16_t)CLAMP(to_px(cx, st, ps, &v, 0, false), 0, 30000);
    return true;
}

static bool track_one(cctx_t *cx, const web_style_t *st, const web_style_t *ps, const char *s, size_t n, web_track_t *t) {
    memset(t, 0, sizeof *t);
    if (n > 7 && !strncasecmp(s, "minmax(", 7) && s[n - 1] == ')') {
        const char *a = s + 7, *e = s + n - 1, *comma = NULL;
        int depth = 0;
        for (const char *q = a; q < e; q++) {
            if (*q == '(') depth++;
            else if (*q == ')') depth--;
            else if (*q == ',' && !depth) { comma = q; break; }
        }
        if (!comma) return false;
        if (!track_kind(cx, st, ps, a, (size_t)(comma - a), &t->lo, &t->lv)) return false;
        if (!track_kind(cx, st, ps, comma + 1, (size_t)(e - comma - 1), &t->hi, &t->hv)) return false;
        if (t->lo == GT_FR) { t->lo = GT_AUTO; t->lv = 0; }
        return true;
    }
    if (n > 12 && !strncasecmp(s, "fit-content(", 12) && s[n - 1] == ')') {
        uint8_t k;
        if (!track_kind(cx, st, ps, s + 12, n - 13, &k, &t->hv) || (k != GT_PX && k != GT_PCT)) return false;
        t->lo = GT_AUTO;
        t->hi = GT_FIT;
        return true;
    }
    if (!track_kind(cx, st, ps, s, n, &t->lo, &t->lv)) return false;
    t->hi = t->lo;
    t->hv = t->lv;
    if (t->lo == GT_FR) { t->lo = GT_AUTO; t->lv = 0; }   /* <flex> = minmax(auto, <flex>) */
    return true;
}

#define MAX_TRACKS 64
/* a track list into out[]; *rep / *nrep: the auto-fill / auto-fit repeat (count 0 = none) */
static int parse_tracks(cctx_t *cx, const web_style_t *st, const web_style_t *ps, const char *s, size_t n,
                        web_track_t *out, int max, int16_t *rep, int16_t *nrep, int depth) {
    const char *p = s, *e = s + n;
    int cnt = 0;
    if (depth > 3) return -1;
    while (p < e) {
        p = c_ws(p, e);
        if (p >= e) break;
        if (*p == '[') {                       /* line names */
            while (p < e && *p != ']') p++;
            if (p < e) p++;
            continue;
        }
        const char *ts = p;
        int dpt = 0;
        while (p < e && (dpt || !web_isspace((unsigned char)*p))) {
            if (*p == '(') dpt++;
            else if (*p == ')') dpt--;
            p++;
        }
        size_t tl = (size_t)(p - ts);
        if (tl > 7 && !strncasecmp(ts, "repeat(", 7) && ts[tl - 1] == ')') {
            const char *a = ts + 7, *ae = ts + tl - 1, *comma = memchr(a, ',', (size_t)(ae - a));
            if (!comma) return -1;
            const char *cs = c_ws(a, comma);
            size_t cn = (size_t)(comma - cs);
            while (cn && web_isspace((unsigned char)cs[cn - 1])) cn--;
            web_track_t sub[MAX_TRACKS];
            int16_t r1, r2;
            int k = parse_tracks(cx, st, ps, comma + 1, (size_t)(ae - comma - 1), sub, MAX_TRACKS, &r1, &r2, depth + 1);
            if (k <= 0) return -1;
            if ((cn == 9 && !strncasecmp(cs, "auto-fill", 9)) || (cn == 8 && !strncasecmp(cs, "auto-fit", 8))) {
                if (*nrep || depth) return -1;
                *rep = (int16_t)cnt;
                *nrep = (int16_t)k;
                for (int i = 0; i < k && cnt < max; i++) out[cnt++] = sub[i];
            } else {
                int times = atoi(cs);
                if (times < 1) return -1;
                for (int t = 0; t < times && cnt < max; t++)
                    for (int i = 0; i < k && cnt < max; i++) out[cnt++] = sub[i];
            }
            continue;
        }
        if (cnt >= max) continue;
        if (!track_one(cx, st, ps, ts, tl, &out[cnt])) return -1;
        cnt++;
    }
    return cnt;
}

static web_grid_t *grid_mut(cctx_t *cx, web_style_t *st) {
    web_grid_t *g = wa_alloc(&cx->doc->ar, sizeof *g);
    if (!g) return NULL;
    if (st->grid) *g = *st->grid;
    else {
        g->auto_col = (web_track_t){ GT_AUTO, GT_AUTO, 0, 0 };
        g->auto_row = g->auto_col;
    }
    st->grid = g;
    return g;
}

static void apply_grid(cctx_t *cx, web_style_t *st, const web_style_t *ps, int prop, const cval_t *v) {
    if (v->t == CV_INITIAL || v->t == CV_INHERIT) {
        if (!st->grid && v->t == CV_INITIAL) return;
        web_grid_t *g = grid_mut(cx, st);
        if (!g) return;
        const web_grid_t *src = v->t == CV_INHERIT ? ps->grid : NULL;
        if (prop == P_GRID_COLS) { g->cols = src ? src->cols : NULL; g->ncols = src ? src->ncols : 0; g->col_nrep = src ? src->col_nrep : 0; g->col_rep = src ? src->col_rep : 0; }
        else if (prop == P_GRID_ROWS) { g->rows = src ? src->rows : NULL; g->nrows = src ? src->nrows : 0; g->row_nrep = src ? src->row_nrep : 0; g->row_rep = src ? src->row_rep : 0; }
        else if (prop == P_GRID_AREAS) { g->areas = src ? src->areas : NULL; g->area_rows = src ? src->area_rows : 0; g->area_cols = src ? src->area_cols : 0; }
        return;
    }
    if (v->t != CV_STR || !v->raw) return;
    const char *s = v->raw;
    size_t n = (size_t)v->f;
    while (n && web_isspace((unsigned char)*s)) { s++; n--; }
    while (n && web_isspace((unsigned char)s[n - 1])) n--;
    bool none = n == 4 && !strncasecmp(s, "none", 4);
    if (prop == P_GRID_AREAS) {
        const char *rows[32]; size_t rl[32];
        int nr = 0, nc = 0;
        if (!none) {
            for (size_t i = 0; i < n && nr < 32; i++) {
                if (s[i] != '"' && s[i] != '\'') continue;
                char q = s[i];
                size_t j = i + 1;
                while (j < n && s[j] != q) j++;
                rows[nr] = s + i + 1; rl[nr] = j - i - 1; nr++;
                i = j;
            }
            for (int r = 0; r < nr; r++) {
                const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
                int c = split_components(rows[r], rl[r], cs, cl, false);
                if (c > nc) nc = c;
            }
            if (!nr || !nc) return;
        }
        web_grid_t *g = grid_mut(cx, st);
        if (!g) return;
        g->areas = NULL;
        g->area_rows = g->area_cols = 0;
        if (none) return;
        const char **names = wa_alloc(&cx->doc->ar, sizeof(char *) * (size_t)(nr * nc));
        if (!names) return;
        for (int r = 0; r < nr; r++) {
            const char *cs[MAXCOMP]; size_t cl[MAXCOMP];
            int c = split_components(rows[r], rl[r], cs, cl, false);
            for (int k = 0; k < c && k < nc; k++)
                names[r * nc + k] = cs[k][0] == '.' ? NULL : wa_strndup(&cx->doc->ar, cs[k], cl[k]);
        }
        g->areas = names;
        g->area_rows = (int16_t)nr;
        g->area_cols = (int16_t)nc;
        return;
    }
    web_track_t tr[MAX_TRACKS];
    int16_t rep = 0, nrep = 0;
    int k = 0;
    if (!none) {
        /* grid-template rows with area strings: drop the strings */
        char buf[512];
        size_t bl = 0;
        for (size_t i = 0; i < n && bl < sizeof buf - 1; i++) {
            if (s[i] == '"' || s[i] == '\'') { char q = s[i++]; while (i < n && s[i] != q) i++; buf[bl++] = ' '; continue; }
            buf[bl++] = s[i];
        }
        buf[bl] = 0;
        k = parse_tracks(cx, st, ps, buf, bl, tr, MAX_TRACKS, &rep, &nrep, 0);
        if (k < 0) return;
        if (prop == P_GRID_AUTO_COLS || prop == P_GRID_AUTO_ROWS) {
            if (k < 1) return;
        }
    }
    web_grid_t *g = grid_mut(cx, st);
    if (!g) return;
    if (prop == P_GRID_AUTO_COLS) { g->auto_col = k ? tr[0] : (web_track_t){ GT_AUTO, GT_AUTO, 0, 0 }; return; }
    if (prop == P_GRID_AUTO_ROWS) { g->auto_row = k ? tr[0] : (web_track_t){ GT_AUTO, GT_AUTO, 0, 0 }; return; }
    web_track_t *copy = NULL;
    if (k) {
        copy = wa_alloc(&cx->doc->ar, sizeof(web_track_t) * (size_t)k);
        if (!copy) return;
        memcpy(copy, tr, sizeof(web_track_t) * (size_t)k);
    }
    if (prop == P_GRID_COLS) { g->cols = copy; g->ncols = (int16_t)k; g->col_rep = rep; g->col_nrep = nrep; }
    else { g->rows = copy; g->nrows = (int16_t)k; g->row_rep = rep; g->row_nrep = nrep; }
}

static uint8_t ai_of(int kw) {
    switch (kw) {
    case K_CENTER: return AI_CENTER;
    case K_START: case K_FLEX_START: case K_LEFT: case K_TOP: return AI_START;
    case K_END: case K_FLEX_END: case K_RIGHT: case K_BOTTOM: return AI_END;
    case K_BASELINE: return AI_BASELINE;
    default: return AI_STRETCH;
    }
}

static uint8_t jc_of(int kw) {
    switch (kw) {
    case K_CENTER: return JC_CENTER;
    case K_FLEX_END: case K_END: case K_RIGHT: return JC_END;
    case K_SPACE_BETWEEN: return JC_BETWEEN;
    case K_SPACE_AROUND: return JC_AROUND;
    case K_SPACE_EVENLY: return JC_EVENLY;
    default: return JC_START;
    }
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
        if (v->t == CV_KW) {
            st->overflow_hidden = v->kw == K_HIDDEN || v->kw == K_CLIP;
            st->ovf_scroll = v->kw == K_AUTO || v->kw == K_SCROLL;
        } else if (initial) st->overflow_hidden = st->ovf_scroll = 0;
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
    case P_RIGHT: st->right = initial ? (web_len_t){ 0, U_AUTO } : to_len(cx, st, ps, v, true); break;
    case P_BOTTOM: st->bottom = initial ? (web_len_t){ 0, U_AUTO } : to_len(cx, st, ps, v, true); break;
    case P_MIN_HEIGHT: st->minh = inherit ? ps->minh : initial ? (web_len_t){ 0, U_AUTO } : to_len(cx, st, ps, v, true); break;
    case P_MAX_HEIGHT:
        st->maxh = inherit ? ps->maxh : initial ? (web_len_t){ 0, U_NONE } : to_len(cx, st, ps, v, true);
        if (st->maxh.u == U_AUTO) st->maxh.u = U_NONE;
        break;
    case P_RADIUS_TL: case P_RADIUS_TR: case P_RADIUS_BR: case P_RADIUS_BL: {
        int i = p - P_RADIUS_TL, r = 0;
        if (inherit) r = ps->radius[i];
        else if (v->t == CV_LEN && v->unit == UN_PCT) r = -CLAMP((int)lroundf(v->f * 10), 0, 500);   /* < 0: tenths of a percent */
        else if (v->t == CV_LEN) r = CLAMP(to_px(cx, st, ps, v, 0, false), 0, 30000);
        st->radius[i] = (int16_t)r;
        break;
    }
    case P_ZINDEX:
        if (v->t == CV_NUM || v->t == CV_LEN) { st->z = (int16_t)CLAMP(lroundf(v->f), -30000, 30000); st->z_set = 1; }
        else if (initial || v->t == CV_KW) st->z_set = 0;
        break;
    case P_ALIGN_SELF: case P_JUSTIFY_SELF: case P_JUSTIFY_ITEMS: {
        uint8_t *dst = p == P_ALIGN_SELF ? &st->align_self : p == P_JUSTIFY_SELF ? &st->justify_self : &st->justify_items;
        if (inherit) *dst = p == P_ALIGN_SELF ? ps->align_self : p == P_JUSTIFY_SELF ? ps->justify_self : ps->justify_items;
        else if (initial || (v->t == CV_KW && (v->kw == K_AUTO || v->kw == K_NORMAL))) *dst = 0;
        else if (v->t == CV_KW) *dst = (uint8_t)(ai_of(v->kw) + 1);
        break;
    }
    case P_ALIGN_CONTENT:
        if (initial || (v->t == CV_KW && v->kw == K_NORMAL)) st->align_content = 0;
        else if (v->t == CV_KW) st->align_content = v->kw == K_STRETCH ? 0 : (uint8_t)(jc_of(v->kw) + 1);
        break;
    case P_ASPECT_RATIO:
        if (v->t == CV_NUM && v->f > 0) { st->aspect = (uint16_t)CLAMP(lroundf(v->f * 1000), 1, 60000); st->aspect_set = 1; }
        else st->aspect_set = 0;
        break;
    case P_GRID_COLS: case P_GRID_ROWS: case P_GRID_AUTO_COLS: case P_GRID_AUTO_ROWS: case P_GRID_AREAS:
        apply_grid(cx, st, ps, p, v);
        break;
    case P_GRID_AUTO_FLOW:
        if (v->t == CV_NUM) st->grid_flow = (uint8_t)v->f;
        else if (initial) st->grid_flow = 0;
        break;
    case P_GRID_COL_START: case P_GRID_COL_END: case P_GRID_ROW_START: case P_GRID_ROW_END: {
        int16_t *dst = p == P_GRID_COL_START ? &st->gcs : p == P_GRID_COL_END ? &st->gce : p == P_GRID_ROW_START ? &st->grs : &st->gre;
        *dst = v->t == CV_NUM ? (int16_t)v->f : 0;
        break;
    }
    case P_ISOLATION:
        if (v->t == CV_NUM) st->isolate = v->f > 0;
        else if (initial) st->isolate = 0;
        break;
    case P_MASK_IMAGE:
        if (v->t == CV_NUM) st->masked = v->f > 0;
        else if (initial) st->masked = 0;
        break;
    case P_GRID_AREA_NAME:
        st->garea = v->t == CV_STR && v->raw ? wa_strndup(&cx->doc->ar, v->raw, (size_t)v->f) : NULL;
        break;
    case P_TRANSFORM: case P_TRANSLATE:
        if (initial) { st->tx = st->ty = (web_len_t){ 0, U_PX }; st->offscreen = 0; }
        else if (v->t == CV_STR && v->raw) apply_transform(cx, st, ps, v->raw, (size_t)v->f, p == P_TRANSLATE);
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
/* The declarations of an element's style="" attribute, parsed once into the
   document's arena: they (and the custom properties children inherit from
   them) must live as long as the document. */
static int inline_decls(cctx_t *cx, web_node_t *n, const char *style, decl_t **decls) {
    if (n->ninl < 0) {
        web_sheet_t tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.ar = cx->doc->ar;             /* allocate in the document's arena */
        decl_t *d = NULL;
        char *text = wa_strndup(&tmp.ar, style, strlen(style));
        int nd = text ? parse_decl_block(&tmp, text, text + strlen(text), &d) : 0;
        cx->doc->ar = tmp.ar;
        n->inl = d;
        n->ninl = (int16_t)(nd > 32000 ? 32000 : nd);
    }
    *decls = n->inl;
    return n->ninl;
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
    const char *sa = web_attr(n, "style");
    if (sa && *sa) {
        decl_t *decls = NULL;
        int nd = inline_decls(cx, n, sa, &decls);
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
