/*
 * web.h - internals of the Web Browser app (apps/browser.c): page fonts, the
 * HTML parser, CSS, layout and the PNG / JPEG / GIF decoders.
 *
 *   web_font.c     page fonts (Liberation Sans / Mono, unpacked on first use)
 *   web_html.c     tolerant HTML tokenizer + tree builder, entities, charsets
 *   web_css.c      CSS parser, selector matching, the cascade
 *   web_layout.c   block / inline / list / table layout into a display list
 *   web_image.c    PNG (zlib via nova/net.h inflate_buffer), JPEG, GIF decoders
 *
 * Everything here is synchronous and bounded; the browser calls it from timer
 * ticks and request callbacks (one page parse / layout / image per call).
 */
#ifndef WEB_H
#define WEB_H

#include <nova.h>
#include <nova/net.h>
#include "gfx/font_internal.h"

/* ==== arena (all of a document's nodes and strings) ======================== */
typedef struct web_chunk web_chunk_t;
typedef struct web_arena { web_chunk_t *head; size_t used; } web_arena_t;
void *wa_alloc(web_arena_t *a, size_t n);                 /* zeroed, 8-aligned */
char *wa_strndup(web_arena_t *a, const char *s, size_t n);
void  wa_free(web_arena_t *a);

/* ==== fonts ================================================================ */
#define WF_BOLD   1
#define WF_ITALIC 2
#define WF_MONO   4
typedef struct web_fontpack {
    uint8_t style, px, height, ascent;
    uint32_t zlen, rawlen;
    const char *b64;
} web_fontpack_t;
extern const web_fontpack_t web_fontpacks[];
extern const int web_fontpack_count;
/* A page font of about px pixels (WF_* flags). Never NULL (falls back to the
   system fonts if the page fonts cannot be unpacked). */
const font_t *web_font(int px, int flags);
bool          web_fonts_real(void);   /* false = using the system-font fallback */

/* ==== text / charsets ====================================================== */
/* Page text is stored as Windows-1252 bytes (what the page fonts cover). */
int   web_cp_to_1252(uint32_t cp);                  /* '?' when not representable */
enum { WEB_CS_AUTO = 0, WEB_CS_UTF8, WEB_CS_1252 };
int   web_detect_charset(const char *src, size_t len, const char *content_type);
char *web_to_1252(const char *src, size_t len, int cs, size_t *outlen);   /* malloc'd */
/* Unicode code point of a page-text byte (the 1252 variant the page fonts use). */
uint32_t web_1252_to_cp(int c);
/* Page text (1252) -> NovaOS system font charset, for window titles / tabs. */
void  web_1252_to_ui(const char *in, char *out, size_t size);

/* ==== DOM =================================================================== */
#define WEB_TAGS(X) \
    X(A,"a") X(ABBR,"abbr") X(ACRONYM,"acronym") X(ADDRESS,"address") X(AREA,"area") \
    X(ARTICLE,"article") X(ASIDE,"aside") X(AUDIO,"audio") X(B,"b") X(BASE,"base") \
    X(BASEFONT,"basefont") X(BDI,"bdi") X(BDO,"bdo") X(BIG,"big") X(BLOCKQUOTE,"blockquote") \
    X(BODY,"body") X(BR,"br") X(BUTTON,"button") X(CANVAS,"canvas") X(CAPTION,"caption") \
    X(CENTER,"center") X(CITE,"cite") X(CODE,"code") X(COL,"col") X(COLGROUP,"colgroup") \
    X(DATA,"data") X(DATALIST,"datalist") X(DD,"dd") X(DEL,"del") X(DETAILS,"details") \
    X(DFN,"dfn") X(DIALOG,"dialog") X(DIR,"dir") X(DIV,"div") X(DL,"dl") X(DT,"dt") \
    X(EM,"em") X(EMBED,"embed") X(FIELDSET,"fieldset") X(FIGCAPTION,"figcaption") \
    X(FIGURE,"figure") X(FONT,"font") X(FOOTER,"footer") X(FORM,"form") X(FRAME,"frame") \
    X(FRAMESET,"frameset") X(H1,"h1") X(H2,"h2") X(H3,"h3") X(H4,"h4") X(H5,"h5") X(H6,"h6") \
    X(HEAD,"head") X(HEADER,"header") X(HGROUP,"hgroup") X(HR,"hr") X(HTML,"html") X(I,"i") \
    X(IFRAME,"iframe") X(IMG,"img") X(INPUT,"input") X(INS,"ins") X(KBD,"kbd") \
    X(LABEL,"label") X(LEGEND,"legend") X(LI,"li") X(LINK,"link") X(MAIN,"main") X(MAP,"map") \
    X(MARK,"mark") X(MATH,"math") X(MENU,"menu") X(META,"meta") X(METER,"meter") X(NAV,"nav") \
    X(NOBR,"nobr") X(NOEMBED,"noembed") X(NOFRAMES,"noframes") X(NOSCRIPT,"noscript") \
    X(OBJECT,"object") X(OL,"ol") X(OPTGROUP,"optgroup") X(OPTION,"option") X(OUTPUT,"output") \
    X(P,"p") X(PARAM,"param") X(PICTURE,"picture") X(PLAINTEXT,"plaintext") X(PRE,"pre") \
    X(PROGRESS,"progress") X(Q,"q") X(RP,"rp") X(RT,"rt") X(RUBY,"ruby") X(S,"s") \
    X(SAMP,"samp") X(SCRIPT,"script") X(SEARCH,"search") X(SECTION,"section") X(SELECT,"select") \
    X(SLOT,"slot") X(SMALL,"small") X(SOURCE,"source") X(SPAN,"span") X(STRIKE,"strike") \
    X(STRONG,"strong") X(STYLE,"style") X(SUB,"sub") X(SUMMARY,"summary") X(SUP,"sup") \
    X(SVG,"svg") X(TABLE,"table") X(TBODY,"tbody") X(TD,"td") X(TEMPLATE,"template") \
    X(TEXTAREA,"textarea") X(TFOOT,"tfoot") X(TH,"th") X(THEAD,"thead") X(TIME,"time") \
    X(TITLE,"title") X(TR,"tr") X(TRACK,"track") X(TT,"tt") X(U,"u") X(UL,"ul") X(VAR,"var") \
    X(VIDEO,"video") X(WBR,"wbr") X(XMP,"xmp")
#define WEB_TAG_ENUM(id, s) T_##id,
enum { T_UNKNOWN = 0, WEB_TAGS(WEB_TAG_ENUM) T_COUNT };
#undef WEB_TAG_ENUM
const char *web_tag_name(int tag);

enum { WN_DOC = 0, WN_ELEM, WN_TEXT };

typedef struct web_attr { const char *name, *value; } web_attr_t;
struct web_style;
typedef struct web_node web_node_t;
struct web_node {
    uint8_t  type;           /* WN_*                                        */
    uint8_t  depth;          /* nesting depth (capped)                      */
    uint16_t tag;            /* T_*                                         */
    const char *name;        /* lower-case tag name                         */
    char    *text;           /* WN_TEXT: 1252 text (not NUL-terminated)     */
    int      len;
    web_attr_t *attrs;
    int      nattrs;
    const char *id;          /* id attribute (or NULL)                      */
    const char **cls;        /* class names                                 */
    int      ncls;
    web_node_t *parent, *first, *last, *next, *prev;
    web_node_t *form;        /* controls: the form they belong to           */
    struct web_style *st;    /* computed style (elements; NULL = not shown) */
    int      ctl;            /* form control index or -1                    */
    int      img;            /* image index or -1                           */
    int      link;           /* link index or -1                            */
    uint32_t mgen;           /* measure cache generation                    */
    int      minw, maxw;     /* measure cache                               */
    int      index;          /* element index among siblings (1-based)      */
};

typedef struct web_doc {
    web_arena_t ar;
    web_node_t *root, *html, *head, *body;
    char     title[256];     /* 1252, collapsed whitespace                  */
    char     base[1024];     /* <base href> (raw) or ""                     */
    char     refresh[1024];  /* <meta http-equiv=refresh> URL or ""         */
    int      nelems;
    bool     quirks;         /* no / a legacy doctype: quirks mode          */
    char    *text;           /* the 1252 source the nodes point into        */
    size_t   textlen;
} web_doc_t;

/* Parse an HTML document (any bytes; charset WEB_CS_*; content_type may be NULL). */
web_doc_t  *web_parse_html(const char *src, size_t len, int charset);
void        web_doc_free(web_doc_t *d);
const char *web_attr(const web_node_t *n, const char *name);   /* NULL if absent */
bool        web_has_attr(const web_node_t *n, const char *name);
/* Text content of an element (collapsed whitespace) into out. */
void        web_text_content(const web_node_t *n, char *out, size_t size);
/* Decode HTML entities of an attribute/text into 1252 (in place safe). */

/* ==== CSS =================================================================== */
typedef struct web_sheet web_sheet_t;
/* origin: 0 = browser default sheet, 1 = author */
web_sheet_t *web_css_parse(const char *css, size_t len, int origin);
void         web_css_free(web_sheet_t *s);
int          web_css_import_count(const web_sheet_t *s);
const char  *web_css_import(const web_sheet_t *s, int i);     /* raw URL of an @import */
int          web_css_rule_count(const web_sheet_t *s);

enum { D_NONE = 0, D_INLINE, D_BLOCK, D_LIST_ITEM, D_INLINE_BLOCK, D_TABLE, D_INLINE_TABLE,
       D_ROW_GROUP, D_HEADER_GROUP, D_FOOTER_GROUP, D_ROW, D_CELL, D_CAPTION, D_COLUMN,
       D_COLUMN_GROUP, D_FLEX, D_INLINE_FLEX, D_GRID, D_CONTENTS };
enum { WS_NORMAL = 0, WS_PRE, WS_NOWRAP, WS_PRE_WRAP, WS_PRE_LINE };
enum { TA_LEFT = 0, TA_RIGHT, TA_CENTER, TA_JUSTIFY };
enum { VA_BASELINE = 0, VA_TOP, VA_MIDDLE, VA_BOTTOM, VA_SUB, VA_SUPER, VA_TEXT_TOP, VA_TEXT_BOTTOM };
enum { LS_DISC = 0, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_LOWER_ALPHA, LS_UPPER_ALPHA,
       LS_LOWER_ROMAN, LS_UPPER_ROMAN, LS_NONE };
enum { TT_NONE = 0, TT_UPPER, TT_LOWER, TT_CAPITALIZE };
enum { POS_STATIC = 0, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED, POS_STICKY };
enum { JC_START = 0, JC_CENTER, JC_END, JC_BETWEEN, JC_AROUND, JC_EVENLY };
enum { AI_STRETCH = 0, AI_START, AI_CENTER, AI_END, AI_BASELINE };
#define DECO_UNDERLINE 1
#define DECO_STRIKE    2
#define DECO_OVERLINE  4

enum { U_AUTO = 0, U_PX, U_PCT, U_NONE };     /* resolved lengths */
typedef struct web_len { int16_t v; uint8_t u; } web_len_t;

typedef struct web_var { const char *name, *value; const struct web_var *next; } web_var_t;

typedef struct web_style {
    uint8_t  display, visibility_hidden, ws, align, valign, list, transform, position;
    uint8_t  bold, italic, mono, deco, floating, border_box, overflow_hidden, align_legacy;
    uint8_t  collapse, flex_column, flex_wrap, list_inside, clear;   /* clear: 1 left, 2 right, 3 both */
    uint8_t  justify, align_items;     /* flex containers: JC_* / AI_*      */
    int16_t  order;                    /* flex items                        */
    uint16_t grow, shrink;             /* flex items, x100                  */
    web_len_t basis;                   /* flex-basis (U_AUTO = auto)        */
    int16_t  gap, rgap;                /* flex containers: column / row gap, px */
    uint8_t  offscreen;                /* transform moves it out of view    */
    int16_t  fsize;          /* px (zoomed)                     */
    int16_t  lh;             /* line height px, 0 = normal      */
    int16_t  indent;         /* text-indent px                  */
    uint32_t color;
    uint32_t bg;             /* alpha 0 = transparent           */
    uint32_t deco_color;
    web_len_t margin[4], padding[4];   /* top, right, bottom, left */
    int16_t  bw[4];          /* border widths px (0 when style none) */
    uint32_t bc[4];
    web_len_t width, height, maxw, minw, left, top;
    int16_t  spacing;        /* table border-spacing            */
    const web_var_t *vars;   /* custom properties (inherited)   */
} web_style_t;

typedef struct web_cascade {
    web_sheet_t **sheets;    /* author sheets in document order */
    int nsheets;
    int vw, vh;              /* viewport size                   */
    int zoom;                /* percent                         */
} web_cascade_t;

/* Compute n->st for every element of the document. */
void web_style_doc(web_doc_t *d, const web_cascade_t *c);
/* Does a media query list ("screen and (min-width: 600px)", "print") apply? */
bool web_css_media(const char *query, int vw, int vh);
/* Parse a CSS colour ("red", "#abc", "rgb(..)"). false if not a colour. */
bool web_css_color(const char *s, uint32_t *out);

/* ==== page resources used by layout ======================================== */
enum { IMG_PENDING = 0, IMG_LOADING, IMG_DECODE, IMG_OK, IMG_FAILED };
typedef struct web_image {
    char      *url;           /* absolute URL (malloc'd)                            */
    int        state;
    surface_t *surf;          /* decoded image (SURF_ALPHA if it had transparency) */
    surface_t *scaled;        /* cached copy at the size it was last drawn         */
    uint8_t   *data;          /* raw bytes waiting to be decoded                    */
    uint32_t   len;
    void      *req;           /* in-flight request (browser)                        */
} web_image_t;

enum { C_TEXT = 1, C_PASSWORD, C_CHECKBOX, C_RADIO, C_SUBMIT, C_RESET, C_BUTTON,
       C_SELECT, C_TEXTAREA, C_HIDDEN, C_IMAGE, C_FILE };
typedef struct web_ctrl {
    web_node_t *node, *form;
    int      type;
    char    *name;            /* malloc'd (1252) or NULL                        */
    char    *value;           /* malloc'd, editable (1252)                     */
    int      vlen, vcap, caret, scroll;
    char    *init_value;      /* for reset                                      */
    bool     checked, init_checked, disabled, readonly;
    int      sel, init_sel;   /* select: chosen option                          */
    int      nopts;
    char   **opt_text, **opt_val;
    char     label[96];       /* buttons (1252)                                 */
    int      size, rows, cols, maxlen;
    int      img;             /* C_IMAGE: image index                           */
    int      x, y, w, h;      /* layout rect (document coordinates; w 0 = hidden) */
    const font_t *font;
    uint32_t color;
} web_ctrl_t;

typedef struct web_link { web_node_t *node; const char *href; const char *target; } web_link_t;

/* ==== layout / display list ================================================= */
enum { WI_RECT = 1, WI_TEXT, WI_IMAGE, WI_CTRL, WI_MARKER, WI_IMGALT };
typedef struct web_item {
    uint8_t  kind, deco, marker;
    int      link;            /* link index or -1                    */
    int      x, y, w, h;      /* document coordinates                */
    uint32_t color;
    const font_t *font;       /* WI_TEXT / WI_MARKER / WI_IMGALT      */
    const char *text;         /* WI_TEXT / WI_IMGALT (1252)           */
    int      len;
    int      ref;             /* image / control index; WI_TEXT: baseline offset */
} web_item_t;

typedef struct web_anchor { const char *name; int y, mark; } web_anchor_t;

typedef struct web_layout {
    web_item_t *items;
    int      nitems, cap;
    int      width, height;   /* document size                       */
    uint32_t bg;              /* page background                     */
    web_anchor_t *anchors;    /* elements with an id / <a name>      */
    int      nanchors, acap;
    web_arena_t ar;           /* text runs                           */
    int      ms;              /* time the last layout took           */
    int      vw;              /* viewport width it was made for      */
} web_layout_t;

typedef struct web_page {
    web_doc_t   *doc;
    web_image_t *imgs;   int nimgs, icap;
    web_ctrl_t  *ctrls;  int nctrls, ccap;
    web_link_t  *links;  int nlinks, lcap;
    web_layout_t lay;
    int          zoom;        /* percent                                    */
    char         base[2048];  /* URL links and images are resolved against  */
} web_page_t;

/* Collect links, <img> and form controls of a parsed document into the page
   (image URLs resolved against pg->base, which the caller sets first). */
void web_page_collect(web_page_t *pg);
void web_page_free_resources(web_page_t *pg);
/* Lay out the styled document for a viewport size. */
void web_layout(web_page_t *pg, int width, int height);
void web_layout_free(web_layout_t *L);
/* y of an anchor (#name), -1 if none. */
int  web_anchor_y(web_page_t *pg, const char *name);
/* Paint the part of the layout visible in view (doc coords sx,sy at view's top-left). */
typedef struct web_paint_opts {
    int hover_link;
    int focus_ctrl;
    bool caret_on;
    /* find-in-page highlights: item index + byte range */
    const int *hl_item, *hl_off, *hl_len;
    int nhl, hl_current;
} web_paint_opts_t;
void web_paint(web_page_t *pg, surface_t *s, rect_t view, int sx, int sy, const web_paint_opts_t *o);
/* Hit test (document coordinates): link index, control index (or -1). */
int  web_hit_link(web_page_t *pg, int x, int y);
int  web_hit_ctrl(web_page_t *pg, int x, int y);
/* Draw one form control at (x, y) of s (used by web_paint). */
void web_paint_ctrl(web_page_t *pg, web_ctrl_t *c, surface_t *s, int x, int y, bool focused, bool caret);
/* Replace a control's value (len < 0: strlen). */
void web_ctrl_set_value(web_ctrl_t *c, const char *s, int len);
/* URL-encode 1252 text as UTF-8 (form submission). Appends to out. */
void web_urlencode(const char *s, char *out, size_t size);

/* ==== images ================================================================= */
/* Decode PNG / JPEG / GIF (first frame) / BMP. NULL if unsupported or broken. */
surface_t *web_image_decode(const uint8_t *data, uint32_t len);
surface_t *web_png_decode(const uint8_t *data, uint32_t len);
surface_t *web_jpeg_decode(const uint8_t *data, uint32_t len);
surface_t *web_gif_decode(const uint8_t *data, uint32_t len);

/* ==== small helpers ========================================================== */
#ifndef lroundf
#define lroundf(x) lround((double)(x))
#endif
static inline bool web_isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

#endif
