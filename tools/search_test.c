/*
 * search_test.c - host-side tests for NovaOS Search's words, page reading,
 * index and ranking (apps/search_text.c and apps/search_index.c, compiled
 * unchanged).
 *
 *   gcc -O2 -Wall -Wextra -DSEARCH_HOST -Iapps -o /tmp/search_test \
 *       tools/search_test.c apps/search_text.c apps/search_index.c -lm
 *   /tmp/search_test            # prints "ALL PASSED" and exits 0
 */
#include "search.h"
#include <time.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the few OS calls the index uses ---------------------------------------------------- */
void *vfs_read_file(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = 0; *size = (uint32_t)n; }
    return b;
}
int vfs_write_file(const char *path, const void *data, uint32_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, size, f);
    fclose(f);
    return w == size ? 0 : -1;
}
int vfs_mkdirs(const char *path) { (void)path; return 0; }
uint32_t sys_time(void) { return 1000; }

/* ---- helpers -------------------------------------------------------------------------- */
static char *words(const char *s) {
    static char out[512];
    out[0] = 0;
    uint32_t pos = 0;
    char w[SRCH_WORD_MAX + 1];
    while (srch_next_word(s, (uint32_t)strlen(s), &pos, w)) {
        if (out[0]) strcat(out, " ");
        strcat(out, w);
    }
    return out;
}

static int add_page(const char *url, const char *html, uint8_t flags, const char *const *links, int nl) {
    srch_page_t p;
    srch_page_init(&p, html, (uint32_t)strlen(html));
    while (!srch_page_feed(&p, 64)) {}
    srch_page_url_words(&p, url);
    uint32_t keys[16];
    for (int i = 0; i < nl && i < 16; i++) keys[i] = srch_url_key(links[i]);
    int d = sidx_add(&p, url, keys, nl, flags, (uint32_t)time(NULL));
    srch_page_free(&p);
    return d;
}

static const char *top(const char *q, int rank) {
    srch_query_t qq;
    srch_query_parse(q, &qq);
    srch_hit_t h[20];
    int total, n = sidx_query(&qq, h, 20, &total);
    if (rank >= n) return "(none)";
    return sidx_doc(h[rank].doc)->url;
}

static int total_for(const char *q) {
    srch_query_t qq;
    srch_query_parse(q, &qq);
    srch_hit_t h[20];
    int total;
    sidx_query(&qq, h, 20, &total);
    return total;
}

int main(void) {
    /* words */
    CHECK(!strcmp(words("The Running Dogs' houses, caf\xe9 cr\xe8me: Linux-kernel 2026!"),
                  "run dog house cafe creme linux kernel 2026"), "words: %s", words("The Running Dogs' houses, caf\xe9 cr\xe8me: Linux-kernel 2026!"));
    CHECK(!strcmp(words("ponies caresses agreed hopping filing happy"), "poni caress agree hop file happi"),
          "stemming: %s", words("ponies caresses agreed hopping filing happy"));
    CHECK(!strcmp(words("a I x of to supercalifragilisticexpialidocious"), ""), "short / stop / long: %s",
          words("a I x of to supercalifragilisticexpialidocious"));
    char ent[64];
    srch_entities("&amp;&lt;caf&eacute;&#233;&#x2014;&bogus;", 41, ent, sizeof ent);
    CHECK(!strcmp(ent, "&<caf\xe9\xe9\x97&bogus;"), "entities: %s", ent);

    /* reading a page */
    const char *html =
        "<!doctype html><html><head><title>NovaOS &amp; friends</title>"
        "<meta name=\"description\" content=\"A hobby &quot;OS&quot; written in C.\">"
        "<base href=\"https://example.org/docs/\"><style>p{color:red}</style>"
        "<script>var hidden = 'scriptword';</script></head><body>"
        "<nav><a href=\"/home\">Home</a> <a href=\"#top\">Top</a> <a href=\"javascript:x()\">JS</a></nav>"
        "<h1>Kernel <em>design</em></h1><!-- comment word -->"
        "<p>The kernel is <b>small</b> and the windows are fast. Kernel kernel.</p>"
        "<p>See <a href='guide.html'>the guide</a> and <a href=\"https://other.net/x\" >other site</a>.</p>"
        "</body></html>";
    srch_page_t p;
    srch_page_init(&p, html, (uint32_t)strlen(html));
    int slices = 0;
    while (!srch_page_feed(&p, 40)) slices++;
    CHECK(slices > 3, "page read in slices: %d", slices);
    CHECK(!strcmp(p.title, "NovaOS & friends"), "title: %s", p.title);
    CHECK(!strcmp(p.desc, "A hobby \"OS\" written in C."), "desc: %s", p.desc);
    CHECK(!strcmp(p.base, "https://example.org/docs/"), "base: %s", p.base);
    CHECK(strstr(p.text, "The kernel is small and the windows are fast.") == p.text, "text: %s", p.text);
    CHECK(p.nlinks == 3, "links: %d", p.nlinks);
    CHECK(p.nlinks == 3 && !strcmp(p.links[0].href, "/home") && !strcmp(p.links[1].href, "guide.html") &&
          !strcmp(p.links[1].anchor, "the guide") && !strcmp(p.links[2].anchor, "other site"), "link details");
    bool script = false, kernel_ok = false, comment = false, design_head = false;
    for (int i = 0; i < p.wfcap; i++) {
        if (!p.wf[i].w[0]) continue;
        if (!strcmp(p.wf[i].w, "scriptword")) script = true;
        if (!strcmp(p.wf[i].w, "comment")) comment = true;
        if (!strcmp(p.wf[i].w, "kernel") && p.wf[i].b == 4 && p.wf[i].h == 1) kernel_ok = true;
        if (!strcmp(p.wf[i].w, "design") && p.wf[i].h == 1) design_head = true;
    }
    CHECK(!script && !comment, "script / comment words are skipped");
    CHECK(kernel_ok, "kernel counted 4 times in the text, once in a heading");
    CHECK(design_head, "a word across an inline tag in a heading");
    srch_page_free(&p);

    srch_page_init(&p, "<meta name=robots content='NOINDEX, follow'><p>secret</p>", 56);
    while (!srch_page_feed(&p, 1000)) {}
    CHECK(p.noindex && !p.nofollow, "meta robots");
    srch_page_free(&p);

    /* keys */
    CHECK(srch_url_key("https://Example.com:443/a#x") == srch_url_key("http://example.com/a"), "url key ignores scheme, port, fragment");
    CHECK(srch_url_key("https://example.com") == srch_url_key("https://example.com/"), "url key: empty path is /");
    CHECK(srch_url_key("https://example.com/a") != srch_url_key("https://example.com/b"), "url key differs");

    /* the index and ranking */
    sidx_clear();
    const char *py_links[] = { "https://www.python.org/" };
    add_page("https://www.python.org/", "<title>Welcome to Python.org</title><p>The official home of the Python programming language.</p>", 0, NULL, 0);
    add_page("https://blog.example.com/snakes", "<title>Snakes of the world</title><p>Pythons and boas are snakes. A python can be long.</p>", 0, py_links, 1);
    add_page("https://docs.example.com/py", "<title>Learning programming</title><p>We use python for teaching programming to beginners.</p>", 0, py_links, 1);
    add_page("https://news.example.com/", "<title>News</title><p>Nothing about snakes or languages today.</p>", 0, NULL, 0);
    CHECK(sidx_count() == 4, "4 pages: %d", sidx_count());
    sidx_rank_all();
    CHECK(!strcmp(top("python", 0), "https://www.python.org/"), "python -> python.org first: %s", top("python", 0));
    CHECK(total_for("python") == 3, "python matches 3 pages: %d", total_for("python"));
    CHECK(!strcmp(top("snakes", 0), "https://blog.example.com/snakes"), "snakes: title match first: %s", top("snakes", 0));
    CHECK(!strcmp(top("python programming", 0), "https://www.python.org/") || !strcmp(top("python programming", 0), "https://docs.example.com/py"),
          "python programming: a page with both words first: %s", top("python programming", 0));
    CHECK(strcmp(top("python programming", 0), "https://blog.example.com/snakes") && strcmp(top("python programming", 1), "https://blog.example.com/snakes"),
          "pages with all the words before pages with some");
    CHECK(total_for("zebra") == 0, "no results");
    CHECK(!strcmp(top("news", 0), "https://news.example.com/"), "address word: %s", top("news", 0));

    /* PageRank: the page linked from the others ranks higher than an equal one */
    sidx_clear();
    const char *to_a[] = { "https://a.example/" };
    add_page("https://a.example/", "<title>Gardening tips</title><p>Tomatoes need sun.</p>", 0, NULL, 0);
    add_page("https://b.example/", "<title>Gardening tips</title><p>Tomatoes need sun.</p>", 0, NULL, 0);
    for (int i = 0; i < 5; i++) {
        char u[64];
        snprintf(u, sizeof u, "https://fan%d.example/", i);
        add_page(u, "<title>Fan page</title><p>We like a site about plants.</p>", 0, to_a, 1);
    }
    sidx_rank_all();
    const srch_doc_t *da = sidx_doc(sidx_find_key(srch_url_key("https://a.example/")));
    const srch_doc_t *db = sidx_doc(sidx_find_key(srch_url_key("https://b.example/")));
    CHECK(da && db && da->pr > db->pr * 2, "PageRank: linked page %.4f > %.4f", da ? da->pr : 0, db ? db->pr : 0);
    CHECK(!strcmp(top("gardening tomatoes", 0), "https://a.example/"), "PageRank breaks the tie: %s", top("gardening tomatoes", 0));
    float sum = 0;
    for (int i = 0; i < sidx_count(); i++) sum += sidx_doc(i)->pr;
    CHECK(sum > 0.99f && sum < 1.01f, "PageRank sums to 1: %f", sum);

    /* snippets */
    srch_query_t q;
    srch_query_parse("tomatoes", &q);
    char snip[600];
    sidx_snippet(sidx_find_key(srch_url_key("https://a.example/")), &q, snip, sizeof snip);
    CHECK(strstr(snip, "<b>Tomatoes</b> need sun.") != NULL, "snippet: %s", snip);

    /* replacing a page, removing one */
    int n0 = sidx_count();
    add_page("https://a.example/", "<title>Gardening tips</title><p>Tomatoes need lots of sun and water.</p>", SD_VISITED, NULL, 0);
    CHECK(sidx_count() == n0, "same address replaces: %d", sidx_count());
    sidx_remove(sidx_find_key(srch_url_key("https://b.example/")));
    CHECK(sidx_count() == n0 - 1 && sidx_find_key(srch_url_key("https://b.example/")) < 0, "remove");

    /* saving and loading */
    sidx_rank_all();
    srch_query_parse("gardening tomatoes", &q);
    srch_hit_t h1[10], h2[10];
    int t1, t2, k1 = sidx_query(&q, h1, 10, &t1);
    char before[256];
    snprintf(before, sizeof before, "%s", sidx_doc(h1[0].doc)->url);
    float s1 = h1[0].score;
    CHECK(sidx_save("/tmp/search_test_index"), "save");
    sidx_clear();
    CHECK(sidx_count() == 0, "cleared");
    CHECK(sidx_load("/tmp/search_test_index"), "load");
    int k2 = sidx_query(&q, h2, 10, &t2);
    CHECK(sidx_count() == n0 - 1 && k1 == k2 && t1 == t2, "loaded: %d pages, %d/%d hits", sidx_count(), k2, t2);
    CHECK(k2 && !strcmp(before, sidx_doc(h2[0].doc)->url) && fabsf(h2[0].score - s1) < 1e-4f, "same ranking after loading");
    CHECK(sidx_doc(sidx_find_key(srch_url_key("https://a.example/")))->flags & SD_VISITED, "flags kept");
    uint32_t sz = 0;
    char *bad = vfs_read_file("/tmp/search_test_index", &sz);
    if (bad) {
        vfs_write_file("/tmp/search_test_index_cut", bad, sz / 2);
        CHECK(!sidx_load("/tmp/search_test_index_cut") && sidx_count() == 0, "a cut file is refused");
        free(bad);
    }

    /* the size cap: crawled pages go before visited ones */
    sidx_clear();
    add_page("https://mine.example/", "<title>My visited page</title><p>Visited words stay.</p>", SD_VISITED, NULL, 0);
    for (int i = 0; i < 300; i++) {
        char u[64], t[200];
        snprintf(u, sizeof u, "https://crawl%d.example/page", i);
        snprintf(t, sizeof t, "<title>Crawled page %d</title><p>Some crawled text number %d with words w%da w%db.</p>", i, i, i, i);
        add_page(u, t, 0, NULL, 0);
    }
    uint32_t full = sidx_bytes();
    sidx_set_cap(full / 3);
    add_page("https://last.example/", "<title>Last</title><p>The last page.</p>", 0, NULL, 0);
    CHECK(sidx_bytes() <= full / 3, "cap: %u <= %u", sidx_bytes(), full / 3);
    CHECK(sidx_find_key(srch_url_key("https://mine.example/")) >= 0, "visited page kept");
    CHECK(sidx_find_key(srch_url_key("https://last.example/")) >= 0, "new page kept");
    CHECK(sidx_count() < 150, "pages dropped: %d left", sidx_count());
    sidx_set_cap(4u << 20);

    /* speed: 3000 pages, a query */
    sidx_clear();
    char page[4096];
    clock_t c0 = clock();
    for (int i = 0; i < 3000; i++) {
        char u[64];
        snprintf(u, sizeof u, "https://site%d.example/p%d", i % 97, i);
        int o = snprintf(page, sizeof page, "<title>Page %d about topic%d</title><p>", i, i % 50);
        for (int k = 0; k < 150; k++) o += snprintf(page + o, sizeof page - (size_t)o, "word%d ", (i * 7 + k * 13) % 2000);
        snprintf(page + o, sizeof page - (size_t)o, "</p>");
        add_page(u, page, 0, NULL, 0);
    }
    clock_t c1 = clock();
    sidx_rank_all();
    srch_query_parse("topic7 word42", &q);
    srch_hit_t hh[10];
    int tt;
    clock_t c2 = clock();
    sidx_query(&q, hh, 10, &tt);
    clock_t c3 = clock();
    printf("3000 pages: %.0f ms to add, query %.2f ms, %d hits, index %u KB\n", (c1 - c0) * 1000.0 / CLOCKS_PER_SEC,
           (c3 - c2) * 1000.0 / CLOCKS_PER_SEC, tt, sidx_bytes() / 1024);
    CHECK(tt > 0, "speed test query finds pages");

    if (failures) { printf("%d of %d checks FAILED\n", failures, checks); return 1; }
    printf("ALL PASSED (%d checks)\n", checks);
    return 0;
}
