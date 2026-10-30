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
#define B_SEARCH_FORM "https://lite.duckduckgo.com/lite/"
#define B_SEARCH_URL B_SEARCH_FORM "?q="
#define B_DOWNLOADS  "/home/user/Downloads"

enum { NAV_NEW = 0, NAV_RELOAD, NAV_HIST, NAV_REPLACE };
enum { SUB_CSS = 1, SUB_IMG };

typedef struct hent {
    char *url;
    char *post;              /* form data of a POST (re-sent on reload)  */
    uint32_t postlen;
    int sx, sy;              /* scroll position when we left it          */
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
    widget_t *set_hist, *set_cookies_lbl;
} browser_t;

/* ---- shared state (browser_data.c) ---- */
typedef struct bookmark { char url[B_URL_MAX]; char title[96]; } bookmark_t;
typedef struct hitem { char url[B_URL_MAX]; char title[96]; uint32_t time; } hitem_t;

extern bool        bdata_history_on;
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
int  browser_downloads_active(void);
const char *browser_download_status(void);
/* "about:..." pages and their actions */
void tab_show_internal(tab_t *t, const char *url, char *html);

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
