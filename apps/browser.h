/*
 * browser.h - internals of the Web Browser app (apps/browser*.c).
 *
 *   browser.c       window, tabs, toolbar, address bar, bookmarks bar, page
 *                   view, input, find in page, zoom, menus, the app icon
 *   browser_net.c   navigation: requests, redirects into the history, page
 *                   loading (HTML, text, images), stylesheets, images,
 *                   forms, downloads, error and certificate warning pages
 *   browser_data.c  browser settings, history, bookmarks and the built-in
 *                   pages (home, history)
 *   browser_js.c    JavaScript on pages (NovaJS, apps/js): the DOM, events,
 *                   timers, fetch, storage; page input (also for other apps
 *                   that show pages through this pipeline)
 *   browser_jsrt.js the JavaScript half of it (built into the kernel)
 */
#ifndef BROWSER_H
#define BROWSER_H

#include "web.h"

#define B_MAX_TABS   16
#define B_TAB_H      34
#define B_TOOL_H     40
#define B_BM_H       28
#define B_STATUS_H   22
#define B_FIND_H     36
#define B_SB         14
#define B_URL_MAX    2048

#define B_HOME_URL   "about:home"
#define B_SEARCH_URL "Browser://Search?q="   /* NovaOS Search (search*.c) */
#define B_DOWNLOADS  "/home/user/Downloads"

enum { NAV_NEW = 0, NAV_RELOAD, NAV_HIST, NAV_REPLACE };
enum { SUB_CSS = 1, SUB_IMG };

typedef struct hent {
    char *url;
    char *post;              /* form data of a POST (re-sent on reload)  */
    uint32_t postlen;
    int sx, sy;              /* scroll position when we left it          */
    uint32_t doc_gen;        /* the page (tab gen) it belongs to: scripts' history entries */
} hent_t;

typedef struct css_ent {
    char *url;               /* NULL for <style> blocks                  */
    char *text;              /* <style> text (until parsed)              */
    char *media;             /* media attribute ("(prefers-color-scheme: dark)") or NULL */
    web_sheet_t *sheet;
    int state;               /* 0 waiting, 1 loading, 2 done             */
    int depth;               /* @import nesting                          */
    uint64_t key;            /* cascade order: an @import sorts before its sheet */
} css_ent_t;

struct tab;
struct dl;
typedef struct sub {
    struct tab *t;
    int kind, idx;
    uint32_t gen;
    http_request_t *req;
} sub_t;

typedef struct rctx { struct tab *t; struct dl *dl; } rctx_t;

#define B_MAX_SUBS 6

typedef struct tab {
    struct browser *b;
    char url[B_URL_MAX];     /* URL of the page shown                     */
    char title[160];         /* system-font charset                       */
    hent_t *hist;
    int nhist, hpos;
    /* the request for the next page */
    http_request_t *req;
    rctx_t *req_ctx;
    char req_url[B_URL_MAX];
    int req_mode;
    char *req_post;
    uint32_t req_postlen;
    uint32_t req_flags;
    bool req_source;         /* show it as page source                    */
    bool loading;
    uint32_t load_start;
    uint32_t net_wait;       /* waiting for DHCP: give up at this time   */
    uint32_t got, total;     /* progress                                  */
    /* the page */
    web_page_t pg;
    uint32_t gen;            /* bumps with every new page                  */
    bool is_html, styled, internal;
    css_ent_t *css;
    int ncss, csscap;
    uint32_t css_deadline;
    sub_t *subs[B_MAX_SUBS];
    int img_next;
    uint32_t img_bytes;
    bool need_layout;
    uint32_t layout_at;
    char frag[256];          /* scroll to this anchor after layout        */
    char refresh[B_URL_MAX]; /* <meta http-equiv=refresh>                 */
    uint32_t refresh_at;
    int sx, sy;
    uint32_t dl_gen, dl_sig, dl_shown;   /* Browser://Downloads: what it shows, when it was made */
    uint32_t srch_gen, srch_shown;       /* Browser://Search: the live search it shows, when */
    /* security of the page shown */
    bool https, cert_ok;
    char cert_subject[128], cert_issuer[128], tls_info[96], cert_error[160];
    /* source */
    char *src;
    uint32_t srclen;
    /* interaction */
    int focus_ctrl, hover_link;
    /* find in page */
    int *hl_item, *hl_off, *hl_len;
    int nhl, hlcap, hl_cur;
    int zoom;
    struct bjs *js;          /* the page's scripts (browser_js.c), NULL = none */
    int press_ctl;           /* control the mouse button went down on      */
} tab_t;

typedef struct browser {
    window_t *win;
    tab_t *tabs[B_MAX_TABS];
    int ntabs, cur;
    widget_t *addr, *content, *find_box;
    bool find_open;
    int hover, press;        /* toolbar / tab strip hot spots             */
    bool caret_on;
    uint32_t caret_t;
    int spin;
    int drag;                /* 1 vertical thumb, 2 horizontal thumb      */
    int drag_off;
    char status[300];
    window_t *settings;
    widget_t *set_hist, *set_cookies_lbl, *set_search_lbl;
    /* another app hosting the page pipeline (HTML Viewer): the page area it shows
       pages in (client coordinates, without scrollbars); NULL = the browser's own */
    rect_t (*host_view)(struct browser *b);
} browser_t;

/* ---- shared state (browser_data.c) ---- */
typedef struct bookmark { char url[B_URL_MAX]; char title[96]; } bookmark_t;
typedef struct hitem { char url[B_URL_MAX]; char title[96]; uint32_t time; } hitem_t;

extern bool        bdata_history_on;
extern bool        bdata_js_on;      /* run JavaScript on pages */
extern bookmark_t *bdata_bm;
extern int         bdata_nbm;
extern hitem_t    *bdata_hist;
extern int         bdata_nhist;

void bdata_load(void);
void bdata_save_settings(void);
void bdata_history_add(const char *url, const char *title);
void bdata_history_clear(void);
int  bdata_bookmark_find(const char *url);
void bdata_bookmark_add(const char *url, const char *title);
void bdata_bookmark_remove(int i);
/* the downloads list (Browser://Downloads), oldest first, kept in ~/.browser/downloads */
#define B_DL_PAGE "Browser://Downloads"
enum { BDL_ACTIVE = 0, BDL_DONE, BDL_FAILED, BDL_CANCELLED };
typedef struct bdl {
    int      id;
    uint8_t  state;          /* BDL_*                                     */
    uint32_t time;           /* sys_time() it started                     */
    uint32_t got, total;     /* bytes so far / expected (0 = unknown)     */
    char     name[96];       /* the file's name                           */
    char     path[256];      /* where it was saved ("" until then)        */
    char     url[512];       /* where it came from                        */
    char     reason[96];     /* why it failed                             */
} bdl_t;
extern bdl_t   *bdata_dl;
extern int      bdata_ndl;
extern uint32_t bdata_dl_gen;    /* bumps on every change (progress too) */
bdl_t *bdata_dl_add(const char *name, const char *url);   /* a new active entry */
bdl_t *bdata_dl_find(int id);
void   bdata_dl_remove(int id);
void   bdata_dl_clear(void);         /* every entry that is not running */
void   bdata_dl_changed(bool save);  /* save = the state changed (not just progress) */
uint32_t bdata_dl_risk_sig(void);    /* changes when SafetyShield's verdicts on them do */
bool   bdata_is_dl_page(const char *url);
char  *bpage_downloads(void);
/* built-in pages (HTML in the page fonts' 1252 charset); malloc'd */
char *bpage_home(void);
char *bpage_history(void);
char *bpage_error(const char *title, const char *text, const char *detail, const char *url, bool cert);
char *bpage_source(const char *url, const char *src, uint32_t len);
char *bpage_text(const char *src, uint32_t len);
char *bpage_image(const char *url);
void  bhtml_escape(char **buf, size_t *len, size_t *cap, const char *s, size_t n);
void  bhtml_add(char **buf, size_t *len, size_t *cap, const char *s);

/* ---- browser_net.c ---- */
void tab_go(tab_t *t, const char *url, int mode);
void tab_post(tab_t *t, const char *url, const char *body, uint32_t len);
void tab_stop(tab_t *t);
void tab_free(tab_t *t);
void tab_tick(tab_t *t);
void tab_relayout(tab_t *t);
void tab_restyle(tab_t *t);
void tab_submit(tab_t *t, int ctl);
void tab_follow_link(tab_t *t, int link, bool new_tab);
void tab_scroll_to(tab_t *t, int x, int y);
void browser_allow_host(const char *url);
bool browser_host_allowed(const char *url);   /* the user chose "Continue anyway" for its site */
int  browser_downloads_active(void);
const char *browser_download_status(void);
void browser_dl_cancel(int id);
void browser_dl_retry(int id);
void browser_dl_start(tab_t *t, const char *url, const char *name);
/* "about:..." pages and their actions */
void tab_show_internal(tab_t *t, const char *url, char *html);
/* for browser_js.c: a same-document history entry (pushState, #fragment), going back /
   forward (same-document entries: popstate, no reload), stylesheets again after
   scripts changed <style> / <link>, loading new pictures, submitting a form */
void tab_push_url(tab_t *t, const char *url, bool replace);
void tab_history_go(tab_t *t, int delta);
void tab_css_reload(tab_t *t);
void tab_load_resources(tab_t *t);
void tab_submit_form(tab_t *t, web_node_t *form, int submitter);
void tab_follow_link_now(tab_t *t, int link);

/* ---- browser_js.c ---- */
/* Page input for the app showing the page: x, y = the pointer in document coordinates
   (mouse events), ev->x / ev->y are not used. Runs the page's scripts' handlers and what
   form controls do (focus, typing, check boxes, lists, buttons); links stay with the
   app (tab_follow_link). true = the page used the event (no scrolling for keys). */
bool tab_page_input(tab_t *t, const event_t *ev, int x, int y);
/* the page's scripts (browser_net.c calls these) */
void bjs_page(tab_t *t, int charset);       /* a new HTML page was parsed: start its scripts */
void bjs_free(tab_t *t);                    /* the page goes away                          */
void bjs_tick(tab_t *t);                    /* timers, loading, events (from tab_tick)     */
bool bjs_link(tab_t *t, int link);          /* a link the user clicked: true = scripts have it */
void bjs_styled(tab_t *t);                  /* the page was styled and laid out again      */
void bjs_sync(tab_t *t);                    /* before styling: the page follows the document */
void bjs_popstate(tab_t *t, const char *old_url);   /* back / forward inside the page        */

/* ---- browser.c ---- */
tab_t *browser_new_tab(browser_t *b, const char *url, bool focus);
void   browser_tab_changed(tab_t *t);     /* title / URL / state changed: repaint chrome */
void   browser_page_changed(tab_t *t);    /* new layout: repaint the page              */
void   browser_set_address(browser_t *b, const char *url);
rect_t browser_view(browser_t *b);        /* page area (client coords, without scrollbars) */
bool   browser_is_current(tab_t *t);
void   browser_find_update(browser_t *b);
const char *browser_input_to_url(const char *in, char *out, size_t size);

#endif
