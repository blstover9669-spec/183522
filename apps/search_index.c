/*
 * search_index.c - NovaOS Search: the index and the ranking.
 *
 * Every page keeps its words in a sorted list (word number, how often it is
 * in the title, the headings, the address and the text) - at most
 * SRCH_PAGE_WORDS words: all title / heading / address words and the most
 * frequent text words. Each word has a document frequency. A query looks its
 * words up in every page's list (binary search), so there are no posting lists
 * to keep in step: adding and removing pages is cheap.
 *
 * Score = BM25F (title x3, address x2.5, headings x1.5, text x1; k1 1.2, b 0.75)
 *       x (all query words in the title: 1.5) x (all in the address: 1.6)
 *       x (the query is the site's name: 3 for its home page, 1.4 inside)
 *       x (1 + 0.35 ln(1 + PageRank x pages)), and pages missing query words
 * are ranked after the pages that have them all.
 *
 * Saved as /home/user/.search/index (binary, little endian):
 *   "NSI1" | u32 words | words: u8 length, bytes | u32 pages |
 *   pages: u32 key, time, length, PageRank bits | u8 flags | u16 url length,
 *   url | u8 title length, title | u8 description length, description |
 *   u16 text length, text | u16 words: varint word, u8 title, u8 heading,
 *   u8 address, varint text | u16 links: u32 keys
 */
#include "search.h"

#define K1 1.2f
#define BM 0.75f
#define DAMP 0.85f
#define PR_STEPS 16
#define MAX_LINKS 64

typedef struct sterm { uint32_t off, df; } sterm_t;

static srch_doc_t *docs;
static int ndocs, dcap;
static uint32_t gen, cap_bytes = 4u << 20, est_bytes, term_bytes;
static uint64_t total_len;
static sterm_t *terms;
static int nterms, tcap;
static char *tstr;
static uint32_t tused, tstrcap;
static uint32_t *thash, thcap;     /* word number + 1, 0 = empty */
static int *khash;                 /* page key -> page number + 1 */
static uint32_t khcap, khgen = ~0u;
static bool rank_dirty;

static uint32_t hstr(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

/* ---- words ------------------------------------------------------------------------------ */
static int term_find(const char *w) {
    if (!thcap) return -1;
    uint32_t k = hstr(w) & (thcap - 1);
    while (thash[k]) {
        uint32_t id = thash[k] - 1;
        if (!strcmp(tstr + terms[id].off, w)) return (int)id;
        k = (k + 1) & (thcap - 1);
    }
    return -1;
}

static bool th_grow(void) {
    uint32_t nc = thcap ? thcap * 2 : 4096;
    uint32_t *nt = calloc(nc, sizeof *nt);
    if (!nt) return false;
    for (int i = 0; i < nterms; i++) {
        uint32_t k = hstr(tstr + terms[i].off) & (nc - 1);
        while (nt[k]) k = (k + 1) & (nc - 1);
        nt[k] = (uint32_t)i + 1;
    }
    free(thash);
    thash = nt;
    thcap = nc;
    return true;
}

static int term_add(const char *w) {
    int id = term_find(w);
    if (id >= 0) return id;
    if ((uint32_t)(nterms + 1) * 2 > thcap && !th_grow()) return -1;
    if (nterms >= tcap) {
        int nc = tcap ? tcap * 2 : 4096;
        sterm_t *nt = realloc(terms, sizeof *nt * (size_t)nc);
        if (!nt) return -1;
        terms = nt;
        tcap = nc;
    }
    uint32_t len = (uint32_t)strlen(w) + 1;
    if (tused + len > tstrcap) {
        uint32_t nc = tstrcap ? tstrcap * 2 : 65536;
        while (nc < tused + len) nc *= 2;
        char *ns = realloc(tstr, nc);
        if (!ns) return -1;
        tstr = ns;
        tstrcap = nc;
    }
    memcpy(tstr + tused, w, len);
    terms[nterms].off = tused;
    terms[nterms].df = 0;
    tused += len;
    uint32_t k = hstr(w) & (thcap - 1);
    while (thash[k]) k = (k + 1) & (thcap - 1);
    thash[k] = (uint32_t)nterms + 1;
    return nterms++;
}

static void df_inc(uint32_t id) { if (!terms[id].df++) term_bytes += (uint32_t)strlen(tstr + terms[id].off) + 2; }
static void df_dec(uint32_t id) { if (terms[id].df && !--terms[id].df) term_bytes -= (uint32_t)strlen(tstr + terms[id].off) + 2; }

/* ---- pages ------------------------------------------------------------------------------ */
static uint32_t doc_bytes(const srch_doc_t *d) {
    return 32 + (uint32_t)(strlen(d->url) + strlen(d->title) + strlen(d->desc) + strlen(d->text)) +
           d->nent * 5u + d->nlinks * 4u;
}

void sidx_set_cap(uint32_t bytes) { cap_bytes = bytes; }
uint32_t sidx_bytes(void) { return est_bytes + term_bytes + 12; }
int sidx_count(void) { return ndocs; }
uint32_t sidx_generation(void) { return gen; }
const srch_doc_t *sidx_doc(int doc) { return doc >= 0 && doc < ndocs ? &docs[doc] : NULL; }

static void kh_build(void) {
    uint32_t nc = 1024;
    while (nc < (uint32_t)ndocs * 2 + 2) nc *= 2;
    if (nc != khcap) {
        int *nk = malloc(sizeof *nk * nc);
        if (!nk) { khcap = 0; return; }
        free(khash);
        khash = nk;
        khcap = nc;
    }
    memset(khash, 0, sizeof *khash * khcap);
    for (int i = 0; i < ndocs; i++) {
        uint32_t k = docs[i].key & (khcap - 1);
        while (khash[k]) k = (k + 1) & (khcap - 1);
        khash[k] = i + 1;
    }
    khgen = gen;
}

int sidx_find_key(uint32_t key) {
    if (khgen != gen || !khcap) kh_build();
    if (!khcap) {
        for (int i = 0; i < ndocs; i++) if (docs[i].key == key) return i;
        return -1;
    }
    uint32_t k = key & (khcap - 1);
    while (khash[k]) {
        if (docs[khash[k] - 1].key == key) return khash[k] - 1;
        k = (k + 1) & (khcap - 1);
    }
    return -1;
}

static void doc_free(srch_doc_t *d) {
    for (int i = 0; i < d->nent; i++) df_dec(d->ent[2 * i] >> 8);
    free(d->url); free(d->title); free(d->desc); free(d->text); free(d->ent); free(d->links);
}

void sidx_remove(int doc) {
    if (doc < 0 || doc >= ndocs) return;
    srch_doc_t *d = &docs[doc];
    est_bytes -= doc_bytes(d);
    total_len -= d->len;
    doc_free(d);
    docs[doc] = docs[ndocs - 1];
    ndocs--;
    gen++;
    rank_dirty = true;
}

void sidx_clear(void) {
    for (int i = 0; i < ndocs; i++) {
        srch_doc_t *d = &docs[i];
        free(d->url); free(d->title); free(d->desc); free(d->text); free(d->ent); free(d->links);
    }
    free(docs); free(terms); free(tstr); free(thash); free(khash);
    docs = NULL; terms = NULL; tstr = NULL; thash = NULL; khash = NULL;
    ndocs = dcap = nterms = tcap = 0;
    tused = tstrcap = thcap = khcap = 0;
    est_bytes = term_bytes = 0;
    total_len = 0;
    gen++;
    khgen = ~0u;
    rank_dirty = false;
}

/* the page to drop when the index is over its size: crawled before visited,
   then the lowest PageRank, then the oldest */
static int victim(uint32_t keep) {
    int best = -1;
    float bs = 0;
    for (int i = 0; i < ndocs; i++) {
        const srch_doc_t *d = &docs[i];
        if (d->key == keep) continue;
        float s = d->pr * (float)ndocs + (d->flags & SD_VISITED ? 1000.0f : 0) + (d->flags & SD_SEED ? 3.0f : 0);
        if (best < 0 || s < bs || (s == bs && d->time < docs[best].time)) { best = i; bs = s; }
    }
    return best;
}

typedef struct cand { const srch_wf_t *w; uint32_t pri; } cand_t;
static int cand_cmp(const void *a, const void *b) {
    uint32_t x = ((const cand_t *)a)->pri, y = ((const cand_t *)b)->pri;
    return x < y ? 1 : x > y ? -1 : strcmp(((const cand_t *)a)->w->w, ((const cand_t *)b)->w->w);
}
static int ent_cmp(const void *a, const void *b) {
    uint32_t x = ((const uint32_t *)a)[0] >> 8, y = ((const uint32_t *)b)[0] >> 8;
    return x < y ? -1 : x > y;
}
static int u32_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static char *dup_s(const char *s) {
    char *d = malloc(strlen(s) + 1);
    if (d) strcpy(d, s);
    return d;
}

static int add_doc(srch_doc_t *nd) {
    if (ndocs >= dcap) {
        int nc = dcap ? dcap * 2 : 256;
        srch_doc_t *n = realloc(docs, sizeof *n * (size_t)nc);
        if (!n) return -1;
        docs = n;
        dcap = nc;
    }
    docs[ndocs] = *nd;
    est_bytes += doc_bytes(nd);
    total_len += nd->len;
    gen++;
    rank_dirty = true;
    return ndocs++;
}

int sidx_add(srch_page_t *p, const char *url, const uint32_t *links, int nlinks, uint8_t flags, uint32_t time) {
    uint32_t key = srch_url_key(url);
    int old = sidx_find_key(key);
    if (old >= 0) {
        flags |= docs[old].flags;
        sidx_remove(old);
    }
    if (p->noindex || !p->nwf) return -1;
    /* the words kept: title / heading / address words first, then by frequency */
    cand_t *c = malloc(sizeof *c * (size_t)p->nwf);
    if (!c) return -1;
    int nc = 0;
    for (int i = 0; i < p->wfcap; i++) {
        const srch_wf_t *w = &p->wf[i];
        if (!w->w[0]) continue;
        uint32_t f = (uint32_t)w->b + 3u * w->t + 2u * w->h + 2u * w->u;
        c[nc].w = w;
        c[nc].pri = (w->t || w->h || w->u ? 0x80000000u : 0) | (f > 0x7FFFFFFF ? 0x7FFFFFFF : f);
        nc++;
    }
    qsort(c, (size_t)nc, sizeof *c, cand_cmp);
    if (nc > SRCH_PAGE_WORDS) nc = SRCH_PAGE_WORDS;
    srch_doc_t d;
    memset(&d, 0, sizeof d);
    d.ent = malloc(sizeof *d.ent * 2 * (size_t)(nc ? nc : 1));
    int nl = nlinks < MAX_LINKS * 4 ? nlinks : MAX_LINKS * 4;
    d.links = malloc(sizeof *d.links * (size_t)(nl ? nl : 1));
    d.url = dup_s(url);
    d.title = dup_s(p->title);
    d.desc = dup_s(p->desc);
    d.text = dup_s(strcmp(p->text, p->desc) ? p->text : "");
    if (!d.ent || !d.links || !d.url || !d.title || !d.desc || !d.text) {
        free(c); free(d.ent); free(d.links); free(d.url); free(d.title); free(d.desc); free(d.text);
        return -1;
    }
    int n = 0;
    for (int i = 0; i < nc; i++) {
        int id = term_add(c[i].w->w);
        if (id < 0) continue;
        const srch_wf_t *w = c[i].w;
        d.ent[2 * n] = (uint32_t)id << 8 | (w->t > 255 ? 255 : w->t);
        d.ent[2 * n + 1] = (uint32_t)(w->h > 255 ? 255 : w->h) | (uint32_t)(w->u > 255 ? 255 : w->u) << 8 |
                           (uint32_t)w->b << 16;
        df_inc((uint32_t)id);
        n++;
    }
    free(c);
    qsort(d.ent, (size_t)n, 2 * sizeof *d.ent, ent_cmp);
    d.nent = (uint16_t)n;
    /* links: distinct, not to itself */
    int k = 0;
    for (int i = 0; i < nl; i++) if (links[i] != key) d.links[k++] = links[i];
    qsort(d.links, (size_t)k, sizeof *d.links, u32_cmp);
    int u = 0;
    for (int i = 0; i < k && u < MAX_LINKS; i++) if (!u || d.links[u - 1] != d.links[i]) d.links[u++] = d.links[i];
    d.nlinks = (uint16_t)u;
    d.key = key;
    d.len = p->words + 1;
    d.time = time;
    d.flags = flags;
    d.pr = ndocs ? 1.0f / (float)(ndocs + 1) : 1.0f;
    if (add_doc(&d) < 0) {
        for (int i = 0; i < n; i++) df_dec(d.ent[2 * i] >> 8);
        free(d.ent); free(d.links); free(d.url); free(d.title); free(d.desc); free(d.text);
        return -1;
    }
    while (sidx_bytes() > cap_bytes && ndocs > 1) {
        int v = victim(key);
        if (v < 0) break;
        sidx_remove(v);
    }
    return sidx_find_key(key);
}

/* ---- PageRank --------------------------------------------------------------------------- */
static float *pr_a, *pr_b;
static int *lk_off, *lk;
static int pr_iter = -1, pr_n;
static uint32_t pr_gen;

static void pr_stop(void) {
    free(pr_a); free(pr_b); free(lk_off); free(lk);
    pr_a = pr_b = NULL;
    lk_off = lk = NULL;
    pr_iter = -1;
}

bool sidx_rank_step(void) {
    if (pr_iter >= 0 && pr_gen != gen) pr_stop();          /* the index changed: start again */
    if (pr_iter < 0) {
        if (!rank_dirty || ndocs < 2) return false;
        int nl = 0;
        for (int i = 0; i < ndocs; i++) nl += docs[i].nlinks;
        pr_n = ndocs;
        pr_a = malloc(sizeof *pr_a * (size_t)pr_n);
        pr_b = malloc(sizeof *pr_b * (size_t)pr_n);
        lk_off = malloc(sizeof *lk_off * (size_t)(pr_n + 1));
        lk = malloc(sizeof *lk * (size_t)(nl ? nl : 1));
        if (!pr_a || !pr_b || !lk_off || !lk) { pr_stop(); return false; }
        int k = 0;
        for (int i = 0; i < pr_n; i++) {
            lk_off[i] = k;
            for (int j = 0; j < docs[i].nlinks; j++) {
                int t = sidx_find_key(docs[i].links[j]);
                if (t >= 0 && t != i) lk[k++] = t;
            }
            pr_a[i] = 1.0f / (float)pr_n;
        }
        lk_off[pr_n] = k;
        pr_gen = gen;
        pr_iter = 0;
        return true;
    }
    float dangling = 0;
    for (int i = 0; i < pr_n; i++) if (lk_off[i + 1] == lk_off[i]) dangling += pr_a[i];
    float base = (1.0f - DAMP) / (float)pr_n + DAMP * dangling / (float)pr_n;
    for (int i = 0; i < pr_n; i++) pr_b[i] = base;
    for (int i = 0; i < pr_n; i++) {
        int n = lk_off[i + 1] - lk_off[i];
        if (!n) continue;
        float share = DAMP * pr_a[i] / (float)n;
        for (int j = lk_off[i]; j < lk_off[i + 1]; j++) pr_b[lk[j]] += share;
    }
    float *t = pr_a;
    pr_a = pr_b;
    pr_b = t;
    if (++pr_iter < PR_STEPS) return true;
    for (int i = 0; i < pr_n; i++) docs[i].pr = pr_a[i];
    pr_stop();
    rank_dirty = false;
    return false;
}

void sidx_rank_all(void) {
    while (sidx_rank_step()) {}
}

/* ---- queries ---------------------------------------------------------------------------- */
void srch_query_parse(const char *q, srch_query_t *out) {
    memset(out, 0, sizeof *out);
    uint32_t pos = 0, n = (uint32_t)strlen(q);
    char w[SRCH_WORD_MAX + 1];
    while (out->n < SRCH_QWORDS && srch_next_word(q, n, &pos, w)) {
        bool dup = false;
        for (int i = 0; i < out->n; i++) if (!strcmp(out->w[i], w)) dup = true;
        if (!dup) strcpy(out->w[out->n++], w);
    }
    /* the query's letters and digits in lower case ("F-Droid" -> "fdroid") */
    size_t j = 0;
    for (const char *s = q; *s && j < sizeof out->joined - 1; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out->joined[j++] = (char)c;
        else if (c >= 'A' && c <= 'Z') out->joined[j++] = (char)(c + 32);
    }
    out->joined[j] = 0;
}

/* the name of the page's site: "python" for www.python.org, "bbc" for www.bbc.co.uk */
static void site_name(const char *url, char *out, size_t size, bool *root) {
    const char *s = strstr(url, "://");
    s = s ? s + 3 : url;
    const char *e = s;
    while (*e && *e != '/' && *e != ':' && *e != '?' && *e != '#') e++;
    const char *p = e;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    *root = !*p || !strcmp(p, "/") || *p == '?' || *p == '#';
    /* the labels */
    const char *lab[8];
    int nlab = 0;
    lab[nlab++] = s;
    for (const char *q = s; q < e && nlab < 8; q++) if (*q == '.') lab[nlab++] = q + 1;
    int pick = nlab >= 2 ? nlab - 2 : 0;
    if (nlab >= 3 && e - lab[nlab - 1] == 2 && lab[nlab - 1] - lab[nlab - 2] <= 4) pick = nlab - 3;   /* co.uk */
    const char *a = lab[pick], *b = pick + 1 < nlab ? lab[pick + 1] - 1 : e;
    size_t n = 0;
    for (const char *q = a; q < b && n + 1 < size; q++)
        if ((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9')) out[n++] = *q;
        else if (*q >= 'A' && *q <= 'Z') out[n++] = (char)(*q + 32);
    out[n] = 0;
}

static int hit_cmp(const void *a, const void *b) {
    float x = ((const srch_hit_t *)a)->score, y = ((const srch_hit_t *)b)->score;
    return x < y ? 1 : x > y ? -1 : ((const srch_hit_t *)a)->doc - ((const srch_hit_t *)b)->doc;
}

int sidx_query(const srch_query_t *q, srch_hit_t *hits, int max, int *total) {
    *total = 0;
    if (!q->n || !ndocs) return 0;
    int id[SRCH_QWORDS], nk = 0;
    float idf[SRCH_QWORDS];
    for (int i = 0; i < q->n; i++) {
        id[i] = term_find(q->w[i]);
        float df = id[i] >= 0 ? (float)terms[id[i]].df : 0;
        idf[i] = logf(1.0f + ((float)ndocs - df + 0.5f) / (df + 0.5f));
        if (id[i] >= 0 && df > 0) nk++;
    }
    if (!nk) return 0;
    float avg = (float)total_len / (float)ndocs;
    if (avg < 1) avg = 1;
    srch_hit_t *all = malloc(sizeof *all * (size_t)ndocs);
    if (!all) return 0;
    int n = 0;
    for (int d = 0; d < ndocs; d++) {
        const srch_doc_t *doc = &docs[d];
        float s = 0, norm = 1.0f - BM + BM * (float)doc->len / avg;
        int matched = 0;
        bool in_title = true, in_url = true;
        for (int i = 0; i < q->n; i++) {
            if (id[i] < 0) continue;
            int lo = 0, hi = doc->nent - 1, at = -1;
            uint32_t want = (uint32_t)id[i];
            while (lo <= hi) {
                int m = (lo + hi) / 2;
                uint32_t v = doc->ent[2 * m] >> 8;
                if (v == want) { at = m; break; }
                if (v < want) lo = m + 1; else hi = m - 1;
            }
            if (at < 0) { in_title = in_url = false; continue; }
            uint32_t a = doc->ent[2 * at], b = doc->ent[2 * at + 1];
            float t = (float)(a & 255), h = (float)(b & 255), u = (float)((b >> 8) & 255), x = (float)(b >> 16);
            float tf = 3.0f * t + 1.5f * h + 2.5f * u + x;
            s += idf[i] * tf * (K1 + 1) / (tf + K1 * norm);
            matched++;
            if (!t) in_title = false;
            if (!u) in_url = false;
        }
        if (!matched) continue;
        if (matched < nk) s *= 0.3f * (float)matched / (float)nk;
        else {
            if (in_title) s *= 1.5f;
            if (in_url) s *= 1.6f;
        }
        if (q->joined[0]) {
            char site[64];
            bool root;
            site_name(doc->url, site, sizeof site, &root);
            if (!strcmp(site, q->joined)) s *= root ? 3.0f : 1.4f;
        }
        float prn = doc->pr * (float)ndocs, boost = 1.0f + 0.35f * logf(1.0f + prn);
        s *= boost > 2.5f ? 2.5f : boost;
        all[n].doc = d;
        all[n].score = s;
        n++;
    }
    qsort(all, (size_t)n, sizeof *all, hit_cmp);
    int k = n < max ? n : max;
    memcpy(hits, all, sizeof *hits * (size_t)k);
    free(all);
    *total = n;
    return k;
}

/* ---- snippets ----------------------------------------------------------------------------- */
size_t srch_escape(const char *s, size_t n, char *out, size_t size) {
    size_t o = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        const char *r = NULL;
        switch (s[i]) {
        case '&': r = "&amp;"; break;
        case '<': r = "&lt;"; break;
        case '>': r = "&gt;"; break;
        case '"': r = "&quot;"; break;
        default: break;
        }
        if (r) {
            size_t l = strlen(r);
            if (o + l + 1 > size) break;
            memcpy(out + o, r, l);
            o += l;
        } else {
            if (o + 2 > size) break;
            out[o++] = s[i];
        }
    }
    if (size) out[o < size ? o : size - 1] = 0;
    return o;
}

#define SNIP_LEN 180

void sidx_snippet(int doc, const srch_query_t *q, char *out, size_t size) {
    out[0] = 0;
    if (doc < 0 || doc >= ndocs || size < 16) return;
    const srch_doc_t *d = &docs[doc];
    char src[SRCH_DESC_MAX + SRCH_TEXT_MAX + 8];
    snprintf(src, sizeof src, "%s%s%s", d->desc, d->desc[0] && d->text[0] ? " \x97 " : "", d->text);
    size_t n = strlen(src);
    if (!n) return;
    /* the words and which query word each one is */
    enum { MAXW = 160 };
    int ws[MAXW], we[MAXW], wm[MAXW], nw = 0;
    for (size_t i = 0; i < n && nw < MAXW;) {
        unsigned char c = (unsigned char)src[i];
        bool letter = (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z') || c >= 0xC0 ||
                      c == 0x8A || c == 0x8C || c == 0x8E || c == 0x9A || c == 0x9C || c == 0x9E || c == 0x9F;
        if (!letter) { i++; continue; }
        size_t st = i;
        while (i < n) {
            c = (unsigned char)src[i];
            if (!((c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z') || c >= 0xC0 ||
                  c == 0x8A || c == 0x8C || c == 0x8E || c == 0x9A || c == 0x9C || c == 0x9E || c == 0x9F)) break;
            i++;
        }
        ws[nw] = (int)st;
        we[nw] = (int)i;
        wm[nw] = -1;
        if (i - st <= SRCH_WORD_MAX) {
            char w[SRCH_WORD_MAX + 1];
            memcpy(w, src + st, i - st);
            w[i - st] = 0;
            if (srch_norm_word(w))
                for (int k = 0; k < q->n; k++) if (!strcmp(w, q->w[k])) { wm[nw] = k; break; }
        }
        nw++;
    }
    /* the window of about SNIP_LEN characters with the most different query words */
    int best = 0, bscore = -1;
    for (int a = 0; a < nw; a++) {
        unsigned mask = 0;
        int cnt = 0;
        for (int b = a; b < nw && we[b] - ws[a] <= SNIP_LEN; b++)
            if (wm[b] >= 0) { if (!(mask & (1u << wm[b]))) cnt += 4; else cnt++; mask |= 1u << wm[b]; }
        if (cnt > bscore) { bscore = cnt; best = a; }
        if (ws[a] > (int)n - SNIP_LEN / 2 && bscore >= 0) break;
    }
    int from = nw ? ws[best] : 0;
    if (bscore <= 0) from = 0;
    /* start a little earlier when the window is near the end */
    if (from > 0 && (int)n - from < SNIP_LEN) {
        int want = (int)n - SNIP_LEN;
        for (int a = 0; a < nw; a++) if (ws[a] >= want) { if (ws[a] < from) from = ws[a]; break; }
    }
    int to = from + SNIP_LEN;
    if (to >= (int)n) to = (int)n;
    else { while (to > from && src[to] != ' ') to--; if (to == from) to = from + SNIP_LEN; }
    size_t o = 0;
    if (from > 0 && o + 4 < size) { memcpy(out, "... ", 4); o = 4; }
    int pos = from;
    for (int k = 0; k < nw && o + 16 < size; k++) {
        if (ws[k] < from || we[k] > to) continue;
        if (wm[k] < 0) continue;
        o += srch_escape(src + pos, (size_t)(ws[k] - pos), out + o, size - o);
        if (o + 4 < size) { memcpy(out + o, "<b>", 3); o += 3; }
        o += srch_escape(src + ws[k], (size_t)(we[k] - ws[k]), out + o, size - o);
        if (o + 5 < size) { memcpy(out + o, "</b>", 4); o += 4; }
        pos = we[k];
    }
    if (pos < to && o + 1 < size) o += srch_escape(src + pos, (size_t)(to - pos), out + o, size - o);
    if (to < (int)n && o + 5 < size) { memcpy(out + o, " ...", 4); o += 4; }
    out[o < size ? o : size - 1] = 0;
}

/* ---- saving and loading ------------------------------------------------------------------- */
typedef struct obuf { uint8_t *b; size_t n, cap; bool bad; } obuf_t;

static void put(obuf_t *o, const void *p, size_t n) {
    if (o->bad) return;
    if (o->n + n > o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 1 << 20;
        while (nc < o->n + n) nc *= 2;
        uint8_t *nb = realloc(o->b, nc);
        if (!nb) { o->bad = true; return; }
        o->b = nb;
        o->cap = nc;
    }
    memcpy(o->b + o->n, p, n);
    o->n += n;
}
static void put32(obuf_t *o, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; put(o, b, 4); }
static void put16(obuf_t *o, uint32_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; put(o, b, 2); }
static void put8(obuf_t *o, uint32_t v) { uint8_t b = (uint8_t)v; put(o, &b, 1); }
static void putv(obuf_t *o, uint32_t v) { while (v >= 0x80) { put8(o, (v & 0x7F) | 0x80); v >>= 7; } put8(o, v); }
static void puts8(obuf_t *o, const char *s) { size_t n = strlen(s); if (n > 255) n = 255; put8(o, (uint32_t)n); put(o, s, n); }
static void puts16(obuf_t *o, const char *s) { size_t n = strlen(s); if (n > 65535) n = 65535; put16(o, (uint32_t)n); put(o, s, n); }

bool sidx_save(const char *path) {
    obuf_t o = { 0 };
    /* the words still used, numbered again in order */
    uint32_t *map = malloc(sizeof *map * (size_t)(nterms ? nterms : 1));
    if (!map) return false;
    uint32_t live = 0;
    for (int i = 0; i < nterms; i++) map[i] = terms[i].df ? live++ : ~0u;
    put(&o, "NSI1", 4);
    put32(&o, live);
    for (int i = 0; i < nterms; i++) if (terms[i].df) puts8(&o, tstr + terms[i].off);
    put32(&o, (uint32_t)ndocs);
    for (int i = 0; i < ndocs; i++) {
        const srch_doc_t *d = &docs[i];
        uint32_t prb;
        memcpy(&prb, &d->pr, 4);
        put32(&o, d->key); put32(&o, d->time); put32(&o, d->len); put32(&o, prb); put8(&o, d->flags);
        puts16(&o, d->url); puts8(&o, d->title); puts8(&o, d->desc); puts16(&o, d->text);
        /* (the new numbers keep the order: map is increasing) */
        put16(&o, d->nent);
        uint32_t prev = 0;
        for (int k = 0; k < d->nent; k++) {
            uint32_t a = d->ent[2 * k], b = d->ent[2 * k + 1], nid = map[a >> 8];
            putv(&o, nid - prev);
            prev = nid;
            put8(&o, a & 255); put8(&o, b & 255); put8(&o, (b >> 8) & 255); putv(&o, b >> 16);
        }
        put16(&o, d->nlinks);
        for (int k = 0; k < d->nlinks; k++) put32(&o, d->links[k]);
    }
    bool ok = !o.bad;
    free(map);
    if (ok) ok = vfs_write_file(path, o.b, (uint32_t)o.n) >= 0;
    free(o.b);
    return ok;
}

typedef struct ibuf { const uint8_t *b; size_t n, p; bool bad; } ibuf_t;
static const uint8_t *get(ibuf_t *i, size_t n) {
    if (i->bad || i->p + n > i->n) { i->bad = true; return NULL; }
    const uint8_t *r = i->b + i->p;
    i->p += n;
    return r;
}
static uint32_t get32(ibuf_t *i) { const uint8_t *b = get(i, 4); return b ? b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24 : 0; }
static uint32_t get16(ibuf_t *i) { const uint8_t *b = get(i, 2); return b ? (uint32_t)(b[0] | b[1] << 8) : 0; }
static uint32_t get8(ibuf_t *i) { const uint8_t *b = get(i, 1); return b ? b[0] : 0; }
static uint32_t getv(ibuf_t *i) {
    uint32_t v = 0;
    for (int s = 0; s < 35; s += 7) { uint32_t c = get8(i); v |= (c & 0x7F) << s; if (!(c & 0x80)) return v; }
    i->bad = true;
    return 0;
}
static char *gets_n(ibuf_t *i, size_t n) {
    const uint8_t *b = get(i, n);
    char *s = b ? malloc(n + 1) : NULL;
    if (!s) { i->bad = true; return NULL; }
    memcpy(s, b, n);
    s[n] = 0;
    return s;
}

bool sidx_load(const char *path) {
    uint32_t size = 0;
    uint8_t *data = vfs_read_file(path, &size);
    if (!data) return false;
    sidx_clear();
    ibuf_t in = { data, size, 0, false };
    const uint8_t *m = get(&in, 4);
    bool ok = m && !memcmp(m, "NSI1", 4);
    uint32_t nt = ok ? get32(&in) : 0;
    int *ids = ok && nt < 4000000 ? malloc(sizeof *ids * (nt ? nt : 1)) : NULL;
    if (!ids) ok = false;
    for (uint32_t i = 0; ok && i < nt; i++) {
        uint32_t l = get8(&in);
        const uint8_t *w = get(&in, l);
        if (!w || !l || l > SRCH_WORD_MAX) { ok = false; break; }
        char buf[SRCH_WORD_MAX + 1];
        memcpy(buf, w, l);
        buf[l] = 0;
        ids[i] = term_add(buf);
        if (ids[i] < 0) ok = false;
    }
    uint32_t nd = ok ? get32(&in) : 0;
    if (nd > 200000) ok = false;
    for (uint32_t i = 0; ok && i < nd; i++) {
        srch_doc_t d;
        memset(&d, 0, sizeof d);
        d.key = get32(&in); d.time = get32(&in); d.len = get32(&in);
        uint32_t prb = get32(&in);
        memcpy(&d.pr, &prb, 4);
        d.flags = (uint8_t)get8(&in);
        d.url = gets_n(&in, get16(&in));
        d.title = gets_n(&in, get8(&in));
        d.desc = gets_n(&in, get8(&in));
        d.text = gets_n(&in, get16(&in));
        uint32_t ne = get16(&in);
        if (ne > SRCH_PAGE_WORDS) in.bad = true;
        d.ent = in.bad ? NULL : malloc(sizeof *d.ent * 2 * (ne ? ne : 1));
        uint32_t prev = 0;
        int k = 0;
        for (uint32_t j = 0; d.ent && j < ne && !in.bad; j++) {
            uint32_t nid = prev + getv(&in);
            prev = nid;
            uint32_t t = get8(&in), h = get8(&in), u = get8(&in), x = getv(&in);
            if (nid >= nt || x > 65535) { in.bad = true; break; }
            d.ent[2 * k] = (uint32_t)ids[nid] << 8 | t;
            d.ent[2 * k + 1] = h | u << 8 | x << 16;
            df_inc((uint32_t)ids[nid]);
            k++;
        }
        d.nent = (uint16_t)k;
        uint32_t nl = get16(&in);
        if (nl > MAX_LINKS) in.bad = true;
        d.links = in.bad ? NULL : malloc(sizeof *d.links * (nl ? nl : 1));
        for (uint32_t j = 0; d.links && j < nl; j++) d.links[j] = get32(&in);
        d.nlinks = (uint16_t)nl;
        if (in.bad || !d.url || !d.title || !d.desc || !d.text || !d.ent || !d.links || !(d.pr >= 0 && d.pr <= 1)) {
            for (int j = 0; j < k && d.ent; j++) df_dec(d.ent[2 * j] >> 8);
            free(d.url); free(d.title); free(d.desc); free(d.text); free(d.ent); free(d.links);
            ok = false;
            break;
        }
        qsort(d.ent, (size_t)k, 2 * sizeof *d.ent, ent_cmp);
        if (add_doc(&d) < 0) {
            for (int j = 0; j < k; j++) df_dec(d.ent[2 * j] >> 8);
            free(d.url); free(d.title); free(d.desc); free(d.text); free(d.ent); free(d.links);
            ok = false;
            break;
        }
    }
    free(ids);
    free(data);
    if (!ok) { sidx_clear(); return false; }
    rank_dirty = false;
    return true;
}
