/*
 * search.h - NovaOS Search, the Web Browser's own search engine (apps/search*.c).
 *
 *   search_text.c   words (lower case, accents folded, light stemming, stop
 *                   words) and reading a web page: title, description,
 *                   headings, text and links - a slice at a time
 *   search_index.c  the index: pages with their words, document frequencies,
 *                   ranking (BM25 over title / headings / address / text, title
 *                   and address boosts, PageRank over the crawled links),
 *                   snippets, saving and loading with a size cap
 *   search.c        the crawler (built-in seed list, breadth first, robots.txt,
 *                   per-site rate limits), pages the user visits, live discovery
 *                   while a search runs and the Browser://Search results page
 *
 * No other search service is used: pages are fetched by their addresses and
 * everything else happens here. Text is Windows-1252 (the page fonts' charset).
 * search_text.c and search_index.c also build on the host (-DSEARCH_HOST) for
 * tools/search_test.c.
 */
#ifndef SEARCH_H
#define SEARCH_H

#ifdef SEARCH_HOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
void    *vfs_read_file(const char *path, uint32_t *size);
int      vfs_write_file(const char *path, const void *data, uint32_t size);
int      vfs_mkdirs(const char *path);
uint32_t sys_time(void);
#else
#include <nova.h>
#endif

#define SRCH_WORD_MAX   24     /* longer words are not indexed              */
#define SRCH_TITLE_MAX  160
#define SRCH_DESC_MAX   240
#define SRCH_TEXT_MAX   480    /* text kept for snippets                    */
#define SRCH_ANCHOR_MAX 60
#define SRCH_LINKS_MAX  300    /* links read from one page                  */
#define SRCH_PAGE_WORDS 220    /* words kept per page: title / heading /
                                  address words and the most frequent ones */
#define SRCH_QWORDS     8      /* words used from a query                   */

/* ---- words (search_text.c) ---- */
/* The next word of s[0..n) from *pos, normalised into out (SRCH_WORD_MAX + 1
   bytes); false at the end. Stop words are skipped. */
bool srch_next_word(const char *s, uint32_t n, uint32_t *pos, char *out);
/* Normalise one word in place (lower case, accents folded, stemmed); false if
   it is not indexed (stop word, too short or too long). */
bool srch_norm_word(char *w);

/* ---- reading a page (search_text.c) ---- */
typedef struct srch_wf {     /* a word of the page and where it was seen */
    uint32_t hash;
    uint16_t t, h, u, b;     /* count in title, headings, address, text  */
    char w[SRCH_WORD_MAX + 1];
} srch_wf_t;

typedef struct srch_link { char *href; char anchor[SRCH_ANCHOR_MAX]; } srch_link_t;

typedef struct srch_page {
    const char *src;         /* the page (1252), not owned               */
    uint32_t len, pos;
    /* reading state */
    int raw;                 /* inside <script> / <style> ...: its tag   */
    int in_title, in_head, in_p, boiler;
    int link;                /* the <a> being read, -1                   */
    bool done;
    char cur[SRCH_WORD_MAX + 2];   /* the word being read (it may go on after an inline tag) */
    int cl;
    bool clong;              /* ... it is too long to be indexed          */
    int cfield;              /* ... where it started: 1 title, 2 heading, 0 text */
    /* results */
    char title[SRCH_TITLE_MAX], desc[SRCH_DESC_MAX], text[SRCH_TEXT_MAX], any[SRCH_TEXT_MAX];
    int tl, dl, xl, al;
    char base[512];          /* <base href>                               */
    bool noindex, nofollow;  /* <meta name=robots>                        */
    uint32_t words;          /* words of text (all of them)               */
    srch_wf_t *wf;           /* hash table of the page's words            */
    int nwf, wfcap;
    srch_link_t *links;
    int nlinks;
} srch_page_t;

void srch_page_init(srch_page_t *p, const char *html, uint32_t len);
/* Read about `budget` more bytes; true when the whole page has been read. */
bool srch_page_feed(srch_page_t *p, uint32_t budget);
/* The words of the page's address (host and path) count as address words. */
void srch_page_url_words(srch_page_t *p, const char *url);
void srch_page_free(srch_page_t *p);
/* Hash of an address without its scheme and #fragment ("example.com/a?b"). */
uint32_t srch_url_key(const char *url);
/* Decode HTML entities of s[0..n) into out (1252); returns the length. */
size_t srch_entities(const char *s, size_t n, char *out, size_t size);

/* ---- the index (search_index.c) ---- */
enum { SD_VISITED = 1, SD_SEED = 2 };

typedef struct srch_hit { int doc; float score; } srch_hit_t;

typedef struct srch_query {
    char w[SRCH_QWORDS][SRCH_WORD_MAX + 1];   /* normalised words         */
    int n;
    char joined[64];         /* the query's letters and digits only ("fdroid") */
} srch_query_t;

typedef struct srch_doc {
    char *url, *title, *desc, *text;
    uint32_t *ent;           /* 2 per word: id << 8 | title count, heading | address << 8 | text << 16 */
    uint32_t *links;         /* srch_url_key of the links                 */
    uint16_t nent, nlinks;
    uint32_t key, len, time;
    float pr;                /* PageRank (sums to 1 over the index)       */
    uint8_t flags;           /* SD_*                                      */
} srch_doc_t;

void     sidx_set_cap(uint32_t bytes);
uint32_t sidx_bytes(void);               /* estimated size of the saved index */
int      sidx_count(void);
uint32_t sidx_generation(void);          /* bumps on every change              */
const srch_doc_t *sidx_doc(int doc);
int      sidx_find_key(uint32_t key);    /* -1 if not indexed                  */
/* Add a page that has been read (replaces the page with the same address);
   links = srch_url_key of the addresses it links to. Returns its number, or
   -1 (noindex, nothing in it). */
int      sidx_add(srch_page_t *p, const char *url, const uint32_t *links, int nlinks, uint8_t flags, uint32_t time);
void     sidx_remove(int doc);
void     sidx_clear(void);
bool     sidx_save(const char *path);
bool     sidx_load(const char *path);
/* PageRank: one step of the computation when the index changed; true while
   there is more to do. sidx_rank_all() does it all at once. */
bool     sidx_rank_step(void);
void     sidx_rank_all(void);
void     srch_query_parse(const char *q, srch_query_t *out);
/* Rank the pages for a query: the best `max` in hits, all matches counted in
   *total. */
int      sidx_query(const srch_query_t *q, srch_hit_t *hits, int max, int *total);
/* A snippet of the page (HTML, 1252) with the query's words in bold. */
void     sidx_snippet(int doc, const srch_query_t *q, char *out, size_t size);
/* HTML-escape s into out (1252); returns the length. */
size_t   srch_escape(const char *s, size_t n, char *out, size_t size);

/* ---- the browser side (search.c) ---- */
#define SEARCH_PAGE "Browser://Search"
struct tab;
bool  search_is_page(const char *url);
/* The results page for a Browser://Search?q=...&p=N address (HTML, 1252,
   malloc'd); its tab title goes into title. Starts live discovery when the
   index has few matches. */
char *search_page_html(const char *url, char *title, size_t tsize);
/* Changes while a live search finds pages: the results page shows them. */
uint32_t search_live_generation(void);
/* A page the user opened (raw bytes as received); indexed a little later. */
void  search_page_visited(const char *url, const char *body, uint32_t len, const char *ctype);
/* Called from the browser's timer: crawling, reading pages, ranking, saving.
   busy = a tab is loading (the crawler waits). */
void  search_tick(bool busy);
void  search_browser_closed(void);    /* the last browser window: save, stop */
void  search_clear(void);             /* settings: Clear search index         */
int   search_page_count(void);

#endif
