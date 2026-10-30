/*
 * browser_net.c - how the Web Browser loads pages.
 *
 * A navigation starts an http_fetch (or reads a file:// file, or makes a
 * built-in about: page). The page shown stays until the response arrives;
 * then it is committed to the tab's history, parsed, its stylesheets are
 * fetched (rendering waits for them up to 3 seconds), styled and laid out,
 * and its images are fetched a few at a time and decoded one per timer tick.
 * Responses the browser cannot show become downloads into ~/Downloads.
 */
#include "browser.h"

/* ---- hosts whose bad certificate the user accepted (this session only) ------------- */
static char allowed[8][256];
static int nallowed;

static void host_of(const char *url, char *out, size_t size) {
    url_t u;
    out[0] = 0;
    if (url_parse(url, &u)) strlcpy(out, u.host, size);
}

static bool host_allowed(const char *url) {
    char h[256];
    host_of(url, h, sizeof h);
    if (!h[0]) return false;
    for (int i = 0; i < nallowed; i++) if (!strcmp(allowed[i], h)) return true;
    return false;
}

void browser_allow_host(const char *url) {
    char h[256];
    host_of(url, h, sizeof h);
    if (!h[0] || host_allowed(url)) return;
    strlcpy(allowed[nallowed % 8], h, sizeof allowed[0]);
    if (nallowed < 8) nallowed++;
}

static char *dupstr(const char *s) {
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

/* ---- data: and file: URLs ------------------------------------------------------------ */
static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static int hexd(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* data:[type][;base64],payload -> malloc'd bytes */
static uint8_t *data_url(const char *url, uint32_t *len, char *ctype, size_t csize) {
    const char *comma = strchr(url, ',');
    if (!comma) return NULL;
    const char *meta = url + 5;
    size_t ml = (size_t)(comma - meta);
    bool b64 = ml >= 7 && !strncasecmp(comma - 7, ";base64", 7);
    if (ctype) {
        size_t k = 0;
        while (k < ml && meta[k] != ';' && k < csize - 1) { ctype[k] = meta[k]; k++; }
        ctype[k] = 0;
    }
    const char *p = comma + 1;
    size_t n = strlen(p);
    uint8_t *out = malloc(n + 1);
    if (!out) return NULL;
    uint32_t o = 0;
    if (b64) {
        uint32_t acc = 0;
        int bits = 0;
        for (size_t i = 0; i < n; i++) {
            int c = (unsigned char)p[i];
            if (c == '%' && i + 2 < n && hexd(p[i + 1]) >= 0) { c = hexd(p[i + 1]) * 16 + hexd(p[i + 2]); i += 2; }
            int v = b64v(c);
            if (v < 0) continue;
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) { bits -= 8; out[o++] = (uint8_t)(acc >> bits); }
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            if (p[i] == '%' && i + 2 < n && hexd(p[i + 1]) >= 0 && hexd(p[i + 2]) >= 0) {
                out[o++] = (uint8_t)(hexd(p[i + 1]) * 16 + hexd(p[i + 2]));
                i += 2;
            } else out[o++] = (uint8_t)p[i];
        }
    }
    out[o] = 0;
    *len = o;
    return out;
}

/* file:///path -> VFS path (%-decoded) */
static bool file_path(const char *url, char *out, size_t size) {
    if (strncasecmp(url, "file://", 7)) return false;
    const char *p = url + 7;
    if (*p != '/') { p = strchr(p, '/'); if (!p) return false; }
    size_t o = 0;
    for (; *p && *p != '?' && *p != '#' && o + 1 < size; p++) {
        if (*p == '%' && hexd(p[1]) >= 0 && hexd(p[2]) >= 0) { out[o++] = (char)(hexd(p[1]) * 16 + hexd(p[2])); p += 2; }
        else out[o++] = *p;
    }
    out[o] = 0;
    return o > 0;
}

static uint8_t *read_local(const char *url, uint32_t *len) {
    char path[512];
    if (!file_path(url, path, sizeof path) || vfs_is_dir(path)) return NULL;
    size_t sz;
    uint8_t *d = vfs_read_file(path, &sz);
    if (d) *len = (uint32_t)sz;
    return d;
}

/* ---- kinds of content ----------------------------------------------------------------- */
enum { K_HTML = 1, K_TEXT, K_IMAGE, K_DOWNLOAD };

static bool image_magic(const uint8_t *b, uint32_t n) {
    if (n >= 8 && !memcmp(b, "\x89PNG", 4)) return true;
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return true;
    if (n >= 6 && !memcmp(b, "GIF8", 4)) return true;
    if (n >= 2 && b[0] == 'B' && b[1] == 'M') return true;
    return false;
}

static int classify(const char *ctype, const uint8_t *b, uint32_t n, const char *url) {
    char ct[96];
    size_t k = 0;
    if (ctype) while (ctype[k] && ctype[k] != ';' && ctype[k] != ' ' && k < sizeof ct - 1) { ct[k] = (char)tolower((unsigned char)ctype[k]); k++; }
    ct[k] = 0;
    if (ct[0]) {
        if (!strcmp(ct, "text/html") || !strcmp(ct, "application/xhtml+xml")) return K_HTML;
        if (!strncmp(ct, "image/", 6)) {
            if (!strcmp(ct, "image/png") || !strcmp(ct, "image/jpeg") || !strcmp(ct, "image/jpg") ||
                !strcmp(ct, "image/gif") || !strcmp(ct, "image/bmp") || !strcmp(ct, "image/x-ms-bmp") ||
                !strcmp(ct, "image/pjpeg"))
                return K_IMAGE;
            return image_magic(b, n) ? K_IMAGE : K_DOWNLOAD;
        }
        if (!strncmp(ct, "text/", 5) || strstr(ct, "json") || strstr(ct, "javascript") || strstr(ct, "xml"))
            return K_TEXT;
        if (strcmp(ct, "application/octet-stream") && strcmp(ct, "unknown/unknown")) return K_DOWNLOAD;
    }
    /* no (useful) type: look at the name and the bytes */
    const char *ext = NULL;
    if (url) {
        char path[512];
        const char *p = url;
        if (file_path(url, path, sizeof path)) p = path;
        const char *q = strrchr(p, '/');
        q = q ? q + 1 : p;
        ext = strrchr(q, '.');
        if (ext) ext++;
    }
    if (ext) {
        if (!strncasecmp(ext, "htm", 3) || !strncasecmp(ext, "xhtml", 5)) return K_HTML;
        if (!strncasecmp(ext, "png", 3) || !strncasecmp(ext, "jpg", 3) || !strncasecmp(ext, "jpeg", 4) ||
            !strncasecmp(ext, "gif", 3) || !strncasecmp(ext, "bmp", 3))
            if (image_magic(b, n)) return K_IMAGE;
        static const char *txt[] = { "txt", "md", "log", "cfg", "c", "h", "css", "js", "json", "xml", "csv", "asm", "py", "sh", "ini" };
        for (int i = 0; i < ARRAY_SIZE(txt); i++) if (!strcasecmp(ext, txt[i])) return K_TEXT;
    }
    if (image_magic(b, n)) return K_IMAGE;
    uint32_t lim = n < 512 ? n : 512, bin = 0, i = 0;
    while (i < lim && web_isspace(b[i])) i++;
    if (i < lim && b[i] == '<') return K_HTML;
    for (i = 0; i < lim; i++) if (b[i] < 9 || (b[i] > 13 && b[i] < 32)) bin++;
    return bin * 20 > lim ? K_DOWNLOAD : K_TEXT;
}

/* ---- downloads ------------------------------------------------------------------------- */
typedef struct dl {
    bool active;
    http_request_t *req;
    char name[64];
    uint32_t got, total;
} dl_t;
static dl_t dls[8];

int browser_downloads_active(void) {
    int n = 0;
    for (int i = 0; i < 8; i++) if (dls[i].active) n++;
    return n;
}

const char *browser_download_status(void) {
    static char buf[128];
    for (int i = 0; i < 8; i++) {
        dl_t *d = &dls[i];
        if (!d->active) continue;
        if (d->total) snprintf(buf, sizeof buf, "Downloading %s - %u%%", d->name, (unsigned)((uint64_t)d->got * 100 / d->total));
        else snprintf(buf, sizeof buf, "Downloading %s - %u KB", d->name, (unsigned)(d->got / 1024));
        return buf;
    }
    return NULL;
}

static void clean_name(const char *in, char *out, size_t size) {
    size_t o = 0;
    for (; *in && o + 1 < size; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '/' || c == '\\' || c == ':' || c == '"' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|' || c < 32)
            c = '_';
        out[o++] = (char)c;
    }
    out[o] = 0;
    while (o && (out[o - 1] == ' ' || out[o - 1] == '.')) out[--o] = 0;
    if (!out[0] || !strcmp(out, ".")) strlcpy(out, "download", size);
}

static void download_name(const char *cd, const char *url, char *out, size_t size) {
    char tmp[256] = "";
    if (cd) {
        const char *f = strcasestr(cd, "filename=");
        if (f) {
            f += 9;
            char q = (*f == '"' || *f == '\'') ? *f++ : 0;
            size_t k = 0;
            while (f[k] && (q ? f[k] != q : (f[k] != ';' && f[k] != ' ')) && k < sizeof tmp - 1) { tmp[k] = f[k]; k++; }
            tmp[k] = 0;
        }
    }
    if (!tmp[0] && url) {
        const char *e = url + strcspn(url, "?#");
        const char *s = e;
        while (s > url && s[-1] != '/') s--;
        size_t k = 0;
        for (const char *p = s; p < e && k < sizeof tmp - 1; p++) {
            if (*p == '%' && p + 2 < e && hexd(p[1]) >= 0 && hexd(p[2]) >= 0) { tmp[k++] = (char)(hexd(p[1]) * 16 + hexd(p[2])); p += 2; }
            else tmp[k++] = *p;
        }
        tmp[k] = 0;
    }
    char clean[256];
    clean_name(tmp, clean, sizeof clean);
    if (strlen(clean) > 60) {
        const char *ext = strrchr(clean, '.');
        char e[16] = "";
        if (ext && strlen(ext) < sizeof e) strlcpy(e, ext, sizeof e);
        clean[60 - strlen(e)] = 0;
        strlcat(clean, e, sizeof clean);
    }
    strlcpy(out, clean, size);
}

static void save_download(const char *name, const uint8_t *data, uint32_t len) {
    if (!vfs_is_dir(B_DOWNLOADS)) vfs_mkdirs(B_DOWNLOADS);
    char path[320], base[96], ext[32] = "";
    strlcpy(base, name, sizeof base);
    char *dot = strrchr(base, '.');
    if (dot && dot != base && strlen(dot) < sizeof ext) { strlcpy(ext, dot, sizeof ext); *dot = 0; }
    snprintf(path, sizeof path, "%s/%s", B_DOWNLOADS, name);
    for (int i = 1; vfs_exists(path) && i < 1000; i++) snprintf(path, sizeof path, "%s/%s (%d)%s", B_DOWNLOADS, base, i, ext);
    int r = vfs_write_file(path, data, len);
    char msg[200];
    if (r < 0) {
        snprintf(msg, sizeof msg, "%s could not be saved: %s", name, vfs_strerror(r));
        notify("Download failed", msg, ICON_ERROR);
    } else {
        snprintf(msg, sizeof msg, "%s saved in Downloads", vfs_basename(path));
        notify("Download complete", msg, ICON_FOLDER_DOWNLOADS);
    }
    klog("browser: download %s -> %s (%u bytes, %d)", name, path, (unsigned)len, r);
}

static void dl_finish(dl_t *d, http_request_t *r) {
    if (http_error(r) != HTTP_OK) {
        char msg[200];
        snprintf(msg, sizeof msg, "%s: %s", d->name, http_error_text(r));
        notify("Download failed", msg, ICON_ERROR);
    } else {
        uint32_t len = 0;
        uint8_t *body = http_take_body(r, &len);
        save_download(d->name, body ? body : (const uint8_t *)"", len);
        free(body);
    }
    http_free(r);
    d->active = false;
    d->req = NULL;
}

static bool wants_download(http_request_t *r) {
    const char *cd = http_header(r, "Content-Disposition");
    if (cd && strcasestr(cd, "attachment")) return true;
    const char *ct = http_header(r, "Content-Type");
    if (!ct) return false;
    return classify(ct, (const uint8_t *)"", 0, NULL) == K_DOWNLOAD;
}

/* ---- the page's subresource requests ------------------------------------------------------ */
static void pump(tab_t *t);

static void subs_cancel(tab_t *t) {
    for (int i = 0; i < B_MAX_SUBS; i++) {
        sub_t *s = t->subs[i];
        if (!s) continue;
        http_free(s->req);
        free(s);
        t->subs[i] = NULL;
    }
}

static int subs_free_slot(tab_t *t) {
    for (int i = 0; i < B_MAX_SUBS; i++) if (!t->subs[i]) return i;
    return -1;
}

static void page_clear(tab_t *t) {
    subs_cancel(t);
    web_page_free_resources(&t->pg);
    for (int i = 0; i < t->ncss; i++) {
        free(t->css[i].url);
        free(t->css[i].text);
        web_css_free(t->css[i].sheet);
    }
    free(t->css);
    t->css = NULL;
    t->ncss = t->csscap = 0;
    free(t->src);
    t->src = NULL;
    t->srclen = 0;
    t->styled = t->is_html = t->internal = false;
    t->img_next = 0;
    t->img_bytes = 0;
    t->need_layout = false;
    t->nhl = 0;
    t->hl_cur = -1;
    t->focus_ctrl = -1;
    t->hover_link = -1;
    t->refresh[0] = 0;
    t->frag[0] = 0;
    t->gen++;
}

/* order keys: 4 x 16 bits, an @import sorts right before the sheet that imports it */
static int css_add(tab_t *t, const char *url, const char *text, int depth) {
    if (t->ncss >= 96) return -1;
    if (t->ncss >= t->csscap) {
        int nc = t->csscap ? t->csscap * 2 : 16;
        css_ent_t *n = realloc(t->css, sizeof *n * nc);
        if (!n) return -1;
        t->css = n;
        t->csscap = nc;
    }
    css_ent_t *e = &t->css[t->ncss];
    memset(e, 0, sizeof *e);
    e->url = url ? dupstr(url) : NULL;
    e->text = text ? dupstr(text) : NULL;
    e->depth = depth;
    return t->ncss++;
}

static void css_parsed(tab_t *t, int i, const char *text, size_t len, const char *base) {
    css_ent_t *e = &t->css[i];
    e->sheet = web_css_parse(text, len, 1);
    e->state = 2;
    if (!e->sheet || e->depth >= 3) return;
    int n = web_css_import_count(e->sheet);
    for (int k = 0; k < n && k < 16; k++) {
        char abs[B_URL_MAX];
        if (!url_resolve(base, web_css_import(e->sheet, k), abs, sizeof abs)) continue;
        int depth = t->css[i].depth + 1;
        int j = css_add(t, abs, NULL, depth);
        if (j < 0) break;
        /* key: the parent's key with level `depth` (0xFFFF there) replaced by k */
        int shift = 48 - 16 * depth;
        uint64_t mask = (uint64_t)0xFFFF << shift;
        t->css[j].key = (t->css[i].key & ~mask) | ((uint64_t)k << shift);
    }
}

static void css_gather(tab_t *t, web_node_t *n, int depth) {
    if (depth > 60) return;
    for (web_node_t *c = n->first; c; c = c->next) {
        if (c->type != WN_ELEM) continue;
        if (c->tag == T_STYLE) {
            const char *media = web_attr(c, "media");
            if (media && strcasestr(media, "print") && !strcasestr(media, "screen") && !strcasestr(media, "all")) continue;
            size_t len = 0;
            for (web_node_t *x = c->first; x; x = x->next) if (x->type == WN_TEXT) len += (size_t)x->len;
            char *text = malloc(len + 1);
            if (!text) continue;
            size_t o = 0;
            for (web_node_t *x = c->first; x; x = x->next) if (x->type == WN_TEXT) { memcpy(text + o, x->text, (size_t)x->len); o += (size_t)x->len; }
            text[o] = 0;
            int i = css_add(t, NULL, NULL, 0);
            if (i >= 0) {
                t->css[i].key = ((uint64_t)i << 48) | 0xFFFFFFFFFFFFull;
                css_parsed(t, i, text, o, t->pg.base);
            }
            free(text);
            continue;
        }
        if (c->tag == T_LINK) {
            const char *rel = web_attr(c, "rel"), *href = web_attr(c, "href"), *media = web_attr(c, "media");
            if (!rel || !href || !*href) continue;
            bool sheet = false;
            for (const char *p = rel; *p;) {
                while (*p == ' ') p++;
                const char *s = p;
                while (*p && *p != ' ') p++;
                if (p - s == 10 && !strncasecmp(s, "stylesheet", 10)) sheet = true;
                if (p - s == 9 && !strncasecmp(s, "alternate", 9)) { sheet = false; break; }
            }
            if (!sheet) continue;
            if (media && strcasestr(media, "print") && !strcasestr(media, "screen") && !strcasestr(media, "all")) continue;
            char abs[B_URL_MAX];
            if (!url_resolve(t->pg.base, href, abs, sizeof abs)) continue;
            int i = css_add(t, abs, NULL, 0);
            if (i >= 0) t->css[i].key = ((uint64_t)i << 48) | 0xFFFFFFFFFFFFull;
            continue;
        }
        if (c->tag == T_SVG || c->tag == T_TEMPLATE) continue;
        css_gather(t, c, depth + 1);
    }
}

static bool css_pending(tab_t *t) {
    for (int i = 0; i < t->ncss; i++) if (t->css[i].state != 2) return true;
    return false;
}

static int img_pending(tab_t *t) {
    int n = 0;
    for (int i = 0; i < t->pg.nimgs; i++) if (t->pg.imgs[i].state < IMG_OK) n++;
    return n;
}

static void sub_done(http_request_t *r, void *ctx) {
    sub_t *s = ctx;
    tab_t *t = s->t;
    for (int i = 0; i < B_MAX_SUBS; i++) if (t->subs[i] == s) t->subs[i] = NULL;
    bool ok = http_error(r) == HTTP_OK && http_status(r) < 400;
    if (s->kind == SUB_CSS && s->idx < t->ncss) {
        css_ent_t *e = &t->css[s->idx];
        if (ok) {
            uint32_t len = http_body_len(r);
            const char *body = (const char *)http_body(r);
            css_parsed(t, s->idx, body ? body : "", len, http_url(r));
        } else {
            e->state = 2;
        }
        if (t->styled && !css_pending(t)) tab_restyle(t);
    } else if (s->kind == SUB_IMG && s->idx < t->pg.nimgs) {
        web_image_t *im = &t->pg.imgs[s->idx];
        im->req = NULL;
        if (ok) {
            uint32_t len = 0;
            im->data = http_take_body(r, &len);
            im->len = len;
            im->state = im->data ? IMG_DECODE : IMG_FAILED;
        } else {
            im->state = IMG_FAILED;
            t->need_layout = true;
            t->layout_at = sys_uptime_ms() + 200;
        }
    }
    http_free(r);
    free(s);
    pump(t);
    if (!t->styled && t->pg.doc && !css_pending(t)) tab_restyle(t);
    browser_tab_changed(t);
}

static bool start_sub(tab_t *t, int kind, int idx, const char *url) {
    int slot = subs_free_slot(t);
    if (slot < 0) return false;
    sub_t *s = calloc(1, sizeof *s);
    if (!s) return false;
    s->t = t;
    s->kind = kind;
    s->idx = idx;
    s->gen = t->gen;
    http_options_t o;
    memset(&o, 0, sizeof o);
    o.flags = host_allowed(url) ? HTTP_F_ALLOW_BAD_CERT : 0;
    o.max_body = kind == SUB_CSS ? 4u << 20 : 16u << 20;
    o.timeout_ms = 20000;
    s->req = http_fetch_ex(url, &o, sub_done, s);
    if (!s->req) { free(s); return false; }
    t->subs[slot] = s;
    return true;
}

/* local (file:, data:) resources are read at once */
static uint8_t *local_resource(const char *url, uint32_t *len) {
    if (!strncasecmp(url, "data:", 5)) return data_url(url, len, NULL, 0);
    if (!strncasecmp(url, "file:", 5)) return read_local(url, len);
    return NULL;
}

static bool is_local(const char *url) { return !strncasecmp(url, "data:", 5) || !strncasecmp(url, "file:", 5); }

static void pump(tab_t *t) {
    if (!t->pg.doc) return;
    for (int i = 0; i < t->ncss; i++) {
        css_ent_t *e = &t->css[i];
        if (e->state != 0 || !e->url) { if (!e->url && e->state == 0) e->state = 2; continue; }
        if (is_local(e->url)) {
            uint32_t len = 0;
            uint8_t *d = local_resource(e->url, &len);
            e->state = 2;
            if (d) { css_parsed(t, i, (const char *)d, len, e->url); free(d); }
            continue;
        }
        if (strncasecmp(e->url, "http", 4)) { e->state = 2; continue; }
        if (!start_sub(t, SUB_CSS, i, e->url)) return;
        e->state = 1;
    }
    if (!t->styled) return;
    for (; t->img_next < t->pg.nimgs; t->img_next++) {
        web_image_t *im = &t->pg.imgs[t->img_next];
        if (im->state != IMG_PENDING) continue;
        if (t->img_next >= 300) { im->state = IMG_FAILED; continue; }
        if (is_local(im->url)) {
            uint32_t len = 0;
            im->data = local_resource(im->url, &len);
            im->len = len;
            im->state = im->data ? IMG_DECODE : IMG_FAILED;
            continue;
        }
        if (strncasecmp(im->url, "http", 4)) { im->state = IMG_FAILED; continue; }
        if (!start_sub(t, SUB_IMG, t->img_next, im->url)) return;
        im->state = IMG_LOADING;
    }
}

/* ---- styling and layout ------------------------------------------------------------- */
static const css_ent_t *sort_css;
static int key_cmp_idx(const void *a, const void *b) {
    uint64_t x = sort_css[*(const int *)a].key, y = sort_css[*(const int *)b].key;
    return x < y ? -1 : x > y;
}

void tab_restyle(tab_t *t) {
    if (!t->pg.doc) return;
    int order[96], n = 0;
    for (int i = 0; i < t->ncss && n < 96; i++) if (t->css[i].sheet) order[n++] = i;
    sort_css = t->css;
    if (n > 1) qsort(order, (size_t)n, sizeof(int), key_cmp_idx);
    web_sheet_t *sheets[96];
    for (int i = 0; i < n; i++) sheets[i] = t->css[order[i]].sheet;
    rect_t v = browser_view(t->b);
    web_cascade_t c = { sheets, n, v.w, v.h, t->zoom };
    t->pg.zoom = t->zoom;
    uint32_t t0 = sys_uptime_ms();
    web_style_doc(t->pg.doc, &c);
    uint32_t t1 = sys_uptime_ms();
    bool first = !t->styled;
    t->styled = true;
    tab_relayout(t);
    if (first) {
        klog("browser: %s: %d elements, %d stylesheets, style %u ms, layout %d ms, %d items",
             t->url, t->pg.doc->nelems, n, (unsigned)(t1 - t0), t->pg.lay.ms, t->pg.lay.nitems);
        pump(t);
    }
}

void tab_relayout(tab_t *t) {
    if (!t->styled || !t->pg.doc) return;
    rect_t v = browser_view(t->b);
    web_layout(&t->pg, v.w, v.h);
    if (t->frag[0]) {
        int y = web_anchor_y(&t->pg, t->frag);
        if (y >= 0) t->sy = y;
    }
    tab_scroll_to(t, t->sx, t->sy);
    if (t->b->find_open && browser_is_current(t)) browser_find_update(t->b);
    browser_page_changed(t);
}

void tab_scroll_to(tab_t *t, int x, int y) {
    rect_t v = browser_view(t->b);
    int maxy = t->pg.lay.height - v.h, maxx = t->pg.lay.width - v.w;
    if (y > maxy) y = maxy;
    if (y < 0) y = 0;
    if (x > maxx) x = maxx;
    if (x < 0) x = 0;
    t->sx = x;
    t->sy = y;
}

static void decode_one(tab_t *t) {
    for (int i = 0; i < t->pg.nimgs; i++) {
        web_image_t *im = &t->pg.imgs[i];
        if (im->state != IMG_DECODE) continue;
        uint32_t t0 = sys_uptime_ms();
        surface_t *s = im->data ? web_image_decode(im->data, im->len) : NULL;
        free(im->data);
        im->data = NULL;
        if (s && (s->w > 2048 || s->h > 2048)) {
            int w = s->w, h = s->h;
            if (w >= h) { h = (int)((int64_t)h * 2048 / w); w = 2048; } else { w = (int)((int64_t)w * 2048 / h); h = 2048; }
            surface_t *sc = surface_scaled(s, MAX(1, w), MAX(1, h));
            surface_destroy(s);
            s = sc;
        }
        if (s) {
            uint32_t bytes = (uint32_t)s->w * (uint32_t)s->h * 4;
            if (t->img_bytes + bytes > (64u << 20)) { surface_destroy(s); s = NULL; }
            else t->img_bytes += bytes;
        }
        im->surf = s;
        im->state = s ? IMG_OK : IMG_FAILED;
        uint32_t ms = sys_uptime_ms() - t0;
        if (ms > 100) klog("browser: decoding %s took %u ms", im->url, (unsigned)ms);
        t->need_layout = true;
        t->layout_at = sys_uptime_ms() + 120;
        browser_tab_changed(t);
        return;
    }
}

void tab_tick(tab_t *t) {
    uint32_t now = sys_uptime_ms();
    if (t->net_wait && !t->req && (net_is_up() || (int32_t)(now - t->net_wait) >= 0)) {
        char url[B_URL_MAX];
        strlcpy(url, t->req_url, sizeof url);
        uint32_t deadline = t->net_wait;
        tab_go(t, url, t->req_mode);
        t->net_wait = deadline;      /* a failure after the deadline shows the error page */
    }
    if (t->pg.doc && !t->styled && t->css_deadline && (int32_t)(now - t->css_deadline) >= 0) tab_restyle(t);
    if (t->styled) decode_one(t);
    if (t->need_layout && (int32_t)(now - t->layout_at) >= 0) {
        t->need_layout = false;
        tab_relayout(t);
        if (!img_pending(t) && !css_pending(t)) t->frag[0] = 0;
    }
    if (t->refresh[0] && (int32_t)(now - t->refresh_at) >= 0) {
        char url[B_URL_MAX];
        strlcpy(url, t->refresh, sizeof url);
        t->refresh[0] = 0;
        tab_go(t, url, NAV_REPLACE);
    }
}

/* ---- committing a page --------------------------------------------------------------------- */
static void hist_free_from(tab_t *t, int from) {
    for (int i = from; i < t->nhist; i++) { free(t->hist[i].url); free(t->hist[i].post); }
    if (from < t->nhist) t->nhist = from;
}

static void commit(tab_t *t, const char *url) {
    int mode = t->req_mode;
    if (mode == NAV_NEW || t->nhist == 0) {
        hist_free_from(t, t->hpos + 1);
        if (t->nhist >= 100) {
            free(t->hist[0].url); free(t->hist[0].post);
            memmove(t->hist, t->hist + 1, sizeof *t->hist * (size_t)(t->nhist - 1));
            t->nhist--;
        }
        hent_t *nh = realloc(t->hist, sizeof *nh * (size_t)(t->nhist + 1));
        if (!nh) return;
        t->hist = nh;
        hent_t *e = &t->hist[t->nhist++];
        memset(e, 0, sizeof *e);
        e->url = dupstr(url);
        if (t->req_post) {
            e->post = malloc(t->req_postlen + 1);
            if (e->post) { memcpy(e->post, t->req_post, t->req_postlen); e->postlen = t->req_postlen; }
        }
        t->hpos = t->nhist - 1;
        t->sx = t->sy = 0;
    } else {
        hent_t *e = &t->hist[t->hpos];
        free(e->url);
        e->url = dupstr(url);
        if (mode == NAV_HIST) { t->sx = e->sx; t->sy = e->sy; }
        else if (mode == NAV_REPLACE) t->sx = t->sy = 0;
    }
    strlcpy(t->url, url, sizeof t->url);
    const char *h = strchr(url, '#');
    if (h && mode != NAV_HIST && mode != NAV_RELOAD) strlcpy(t->frag, h + 1, sizeof t->frag);
    browser_set_address(t->b, NULL);
}

/* parse html (any charset) as the tab's new page */
static void show_html(tab_t *t, const char *url, char *body, uint32_t len, const char *ctype, bool internal) {
    page_clear(t);
    t->internal = internal;
    t->is_html = true;
    t->src = body;          /* kept for "View page source" */
    t->srclen = len;
    int cs = internal ? WEB_CS_1252 : web_detect_charset(body, len, ctype);
    t->pg.doc = web_parse_html(body, len, cs);
    t->pg.zoom = t->zoom;
    if (!t->pg.doc) {
        t->title[0] = 0;
        browser_tab_changed(t);
        return;
    }
    strlcpy(t->pg.base, url, sizeof t->pg.base);
    if (t->pg.doc->base[0]) {
        char abs[B_URL_MAX];
        if (url_resolve(url, t->pg.doc->base, abs, sizeof abs)) strlcpy(t->pg.base, abs, sizeof t->pg.base);
    }
    web_page_collect(&t->pg);
    if (t->pg.doc->title[0]) web_1252_to_ui(t->pg.doc->title, t->title, sizeof t->title);
    else {
        url_t u;
        if (!internal && url_parse(url, &u)) strlcpy(t->title, u.host, sizeof t->title);
        else {
            const char *n = strrchr(url, '/');
            strlcpy(t->title, n && n[1] ? n + 1 : url, sizeof t->title);
        }
    }
    if (t->pg.doc->refresh[0] && !internal) {
        char abs[B_URL_MAX];
        if (url_resolve(t->pg.base, t->pg.doc->refresh, abs, sizeof abs)) {
            strlcpy(t->refresh, abs, sizeof t->refresh);
            t->refresh_at = sys_uptime_ms() + 1500;
        }
    }
    css_gather(t, t->pg.doc->root, 0);
    pump(t);
    if (!css_pending(t)) tab_restyle(t);
    else t->css_deadline = sys_uptime_ms() + 3000;
    browser_tab_changed(t);
}

static void clear_security(tab_t *t) {
    t->https = t->cert_ok = false;
    t->cert_subject[0] = t->cert_issuer[0] = t->tls_info[0] = t->cert_error[0] = 0;
}

void tab_show_internal(tab_t *t, const char *url, char *html) {
    if (!html) return;
    commit(t, url);
    clear_security(t);
    show_html(t, url, html, (uint32_t)strlen(html), NULL, true);
}

static void error_page(tab_t *t, http_request_t *r, int err) {
    const char *title, *text;
    char detail[256] = "";
    bool cert = false;
    switch (err) {
    case HTTP_ERR_NO_NETWORK:
        title = "No internet";
        text = "NovaOS is not connected to a network. Check that the virtual machine has a network adapter "
               "(Intel PRO/1000) and that it is connected.";
        strlcpy(detail, net_status_text(), sizeof detail);
        break;
    case HTTP_ERR_DNS:
        title = "Site not found";
        text = "The address of this site could not be found. Check the web address for typing mistakes.";
        break;
    case HTTP_ERR_TLS:
        title = "Secure connection failed";
        text = "A secure (HTTPS) connection to this site could not be made.";
        strlcpy(detail, http_error_text(r), sizeof detail);
        break;
    case HTTP_ERR_CERT:
        title = "Warning: this site's certificate is not valid";
        text = "The site's security certificate could not be checked, so the connection may not be private. "
               "Someone could be pretending to be this site to steal your information.";
        strlcpy(detail, http_cert_error(r), sizeof detail);
        cert = true;
        break;
    case HTTP_ERR_CONNECT:
        title = "Can't connect to this site";
        text = "The site refused the connection or could not be reached.";
        break;
    case HTTP_ERR_TIMEOUT:
        title = "The site took too long to respond";
        text = "The connection timed out. The site may be busy or offline.";
        break;
    case HTTP_ERR_BAD_URL:
        title = "This address is not valid";
        text = "The web address could not be understood.";
        break;
    case HTTP_ERR_REDIRECTS:
        title = "This page isn't redirecting properly";
        text = "The site sent the browser round in a circle of redirects.";
        break;
    case HTTP_ERR_TOO_LARGE:
        title = "The page is too large";
        text = "The page is bigger than the browser can load.";
        break;
    default:
        title = "The page could not be loaded";
        text = "Something went wrong while loading this page.";
        strlcpy(detail, http_error_text(r), sizeof detail);
        break;
    }
    klog("browser: %s: %s (%s)", t->req_url, title, http_error_text(r));
    char url[B_URL_MAX];
    strlcpy(url, t->req_url, sizeof url);
    char *html = bpage_error(title, text, detail, url, cert);
    commit(t, url);
    clear_security(t);
    if (html) show_html(t, url, html, (uint32_t)strlen(html), NULL, true);
    if (cert) {   /* the warning page shows the red "not secure" padlock */
        t->https = true;
        t->cert_ok = false;
        strlcpy(t->cert_error, detail, sizeof t->cert_error);
    }
}

/* a response for the tab arrived */
static void deliver(tab_t *t, const char *url, uint8_t *body, uint32_t len, const char *ctype, int status) {
    if (t->req_source) {
        char vs[B_URL_MAX];
        snprintf(vs, sizeof vs, "view-source:%s", url);
        char *html = bpage_source(url, (const char *)body, len);
        free(body);
        commit(t, vs);
        if (html) show_html(t, vs, html, (uint32_t)strlen(html), NULL, true);
        snprintf(t->title, sizeof t->title, "Source of %s", url);
        browser_tab_changed(t);
        return;
    }
    int kind = classify(ctype, body, len, url);
    if (kind == K_DOWNLOAD) {
        char name[96];
        download_name(NULL, url, name, sizeof name);
        save_download(name, body, len);
        free(body);
        browser_tab_changed(t);
        return;
    }
    if (status >= 400 && len == 0) {
        char title[64];
        snprintf(title, sizeof title, "Error %d", status);
        char *html = bpage_error(title, "The server could not show this page.", NULL, url, false);
        free(body);
        commit(t, url);
        if (html) show_html(t, url, html, (uint32_t)strlen(html), NULL, true);
        return;
    }
    commit(t, url);
    if (kind == K_HTML) {
        show_html(t, url, (char *)body, len, ctype, false);
    } else if (kind == K_TEXT) {
        char *html = bpage_text((const char *)body, len);
        if (html) {
            show_html(t, url, html, (uint32_t)strlen(html), NULL, true);
            t->internal = false;
            free(t->src);
            t->src = (char *)body;     /* the source is the text itself */
            t->srclen = len;
            const char *n = strrchr(url, '/');
            strlcpy(t->title, n && n[1] ? n + 1 : url, sizeof t->title);
        } else free(body);
    } else {
        char *html = bpage_image(url);
        if (html) {
            show_html(t, url, html, (uint32_t)strlen(html), NULL, true);
            t->internal = false;
            for (int i = 0; i < t->pg.nimgs; i++)
                if (t->pg.imgs[i].state == IMG_PENDING) {
                    t->pg.imgs[i].data = body;
                    t->pg.imgs[i].len = len;
                    t->pg.imgs[i].state = IMG_DECODE;
                    body = NULL;
                    break;
                }
        }
        free(body);
    }
    if (bdata_history_on && strncmp(url, "about:", 6)) {
        char t1252[160];
        strlcpy(t1252, t->pg.doc && t->pg.doc->title[0] ? t->pg.doc->title : t->title, sizeof t1252);
        bdata_history_add(url, t1252);
    }
    browser_tab_changed(t);
}

static void doc_progress(http_request_t *r, void *ctx) {
    rctx_t *rc = ctx;
    uint32_t got = 0, total = 0;
    http_progress(r, &got, &total);
    if (rc->dl) {
        rc->dl->got = got;
        rc->dl->total = total;
        return;
    }
    tab_t *t = rc->t;
    t->got = got;
    t->total = total;
    int st = http_status(r);
    if (!t->req_source && st >= 200 && st < 300 && wants_download(r)) {
        dl_t *d = NULL;
        for (int i = 0; i < 8; i++) if (!dls[i].active) { d = &dls[i]; break; }
        if (d) {
            memset(d, 0, sizeof *d);
            d->active = true;
            d->req = r;
            download_name(http_header(r, "Content-Disposition"), http_url(r), d->name, sizeof d->name);
            d->got = got;
            d->total = total;
            rc->t = NULL;
            rc->dl = d;
            t->req = NULL;
            t->req_ctx = NULL;
            t->loading = false;
            if (browser_is_current(t)) browser_set_address(t->b, NULL);   /* the page stays */
            klog("browser: downloading %s as %s", http_url(r), d->name);
        }
    }
    browser_tab_changed(t);
}

static void doc_done(http_request_t *r, void *ctx) {
    rctx_t *rc = ctx;
    if (rc->dl) { dl_finish(rc->dl, r); free(rc); return; }
    tab_t *t = rc->t;
    free(rc);
    t->req = NULL;
    t->req_ctx = NULL;
    t->loading = false;
    int err = http_error(r);
    if (err == HTTP_ERR_NO_NETWORK) {
        /* the network is still being set up (DHCP): wait for it a little */
        net_info_t ni;
        net_get_info(&ni);
        uint32_t now = sys_uptime_ms();
        if (ni.adapter && ni.link_up && !ni.configured && (!t->net_wait || (int32_t)(now - t->net_wait) < 0)) {
            if (!t->net_wait) t->net_wait = now + 15000;
            t->loading = true;
            http_free(r);
            browser_tab_changed(t);
            return;
        }
    }
    t->net_wait = 0;
    if (err != HTTP_OK) {
        error_page(t, r, err);
        http_free(r);
        browser_tab_changed(t);
        return;
    }
    int status = http_status(r);
    if (status == 204 || status == 205) { http_free(r); browser_tab_changed(t); return; }
    char *url = dupstr(http_url(r));
    char ctype[128];
    const char *ct = http_header(r, "Content-Type");
    strlcpy(ctype, ct ? ct : "", sizeof ctype);
    /* keep the fragment of the requested URL */
    const char *hash = strchr(t->req_url, '#');
    if (url && hash && !strchr(url, '#') && strlen(url) + strlen(hash) < B_URL_MAX) {
        char *u2 = malloc(strlen(url) + strlen(hash) + 1);
        if (u2) { strcpy(u2, url); strcat(u2, hash); free(url); url = u2; }
    }
    bool https = http_is_https(r);
    t->https = https;
    t->cert_ok = http_cert_ok(r);
    strlcpy(t->cert_subject, http_cert_subject(r), sizeof t->cert_subject);
    strlcpy(t->cert_issuer, http_cert_issuer(r), sizeof t->cert_issuer);
    strlcpy(t->tls_info, http_tls_info(r), sizeof t->tls_info);
    strlcpy(t->cert_error, http_cert_error(r), sizeof t->cert_error);
    uint32_t len = 0;
    uint8_t *body = http_take_body(r, &len);
    if (!body) { body = calloc(1, 1); len = 0; }
    http_free(r);
    klog("browser: %d %s (%u bytes, %s)", status, url ? url : "?", (unsigned)len, ctype);
    if (url && body) deliver(t, url, body, len, ctype, status);
    else free(body);
    free(url);
}

/* ---- navigation ------------------------------------------------------------------------- */
void tab_stop(tab_t *t) {
    t->net_wait = 0;
    if (t->req) { http_free(t->req); t->req = NULL; }
    free(t->req_ctx);
    t->req_ctx = NULL;
    t->loading = false;
}

static void about_page(tab_t *t, const char *url) {
    const char *what = url + 6;
    if (!strncmp(what, "retry", 5) || !strncmp(what, "continue", 8)) {
        /* the error page's URL is the address that failed */
        char again[B_URL_MAX];
        strlcpy(again, t->url, sizeof again);
        if (!strncmp(what, "continue", 8)) browser_allow_host(again);
        if (strncmp(again, "about:", 6)) tab_go(t, again, NAV_REPLACE);
        return;
    }
    if (!strncmp(what, "back", 4)) {
        if (t->hpos > 0) { t->hpos--; tab_go(t, t->hist[t->hpos].url, NAV_HIST); }
        else tab_go(t, B_HOME_URL, NAV_REPLACE);
        return;
    }
    char *html = NULL;
    if (!strncmp(what, "history", 7)) html = bpage_history();
    else if (!strncmp(what, "blank", 5)) html = dupstr("<html><head><title>New Tab</title></head><body></body></html>");
    else html = bpage_home();
    tab_show_internal(t, !strncmp(what, "history", 7) ? "about:history" : !strncmp(what, "blank", 5) ? "about:blank" : B_HOME_URL, html);
    if (!strncmp(what, "home", 4) || !what[0]) strlcpy(t->title, "New Tab", sizeof t->title);
    browser_tab_changed(t);
}

static void start(tab_t *t, const char *url, int mode, const char *post, uint32_t postlen, bool source) {
    tab_stop(t);
    if (t->nhist && t->hpos < t->nhist && mode != NAV_HIST) { t->hist[t->hpos].sx = t->sx; t->hist[t->hpos].sy = t->sy; }
    t->req_mode = mode;
    t->req_source = source;
    strlcpy(t->req_url, url, sizeof t->req_url);
    free(t->req_post);
    t->req_post = NULL;
    t->req_postlen = 0;
    if (post) {
        t->req_post = malloc(postlen + 1);
        if (t->req_post) { memcpy(t->req_post, post, postlen); t->req_post[postlen] = 0; t->req_postlen = postlen; }
    }
    if (!strncasecmp(url, "about:", 6)) { about_page(t, url); return; }
    if (!strncasecmp(url, "view-source:", 12)) { start(t, url + 12, mode, NULL, 0, true); return; }
    if (!strncasecmp(url, "file:", 5) || !strncasecmp(url, "data:", 5)) {
        uint32_t len = 0;
        char ctype[96] = "";
        uint8_t *d = !strncasecmp(url, "data:", 5) ? data_url(url, &len, ctype, sizeof ctype) : read_local(url, &len);
        if (!d) {
            char *html = bpage_error("File not found", "This file could not be opened.", NULL, url, false);
            commit(t, url);
            clear_security(t);
            if (html) show_html(t, url, html, (uint32_t)strlen(html), NULL, true);
            return;
        }
        clear_security(t);
        deliver(t, url, d, len, ctype[0] ? ctype : NULL, 200);
        return;
    }
    if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)) {
        char *html = bpage_error("This address is not supported", "The browser can only open web (http, https) and file addresses.", NULL, url, false);
        commit(t, url);
        clear_security(t);
        if (html) show_html(t, url, html, (uint32_t)strlen(html), NULL, true);
        return;
    }
    rctx_t *rc = calloc(1, sizeof *rc);
    if (!rc) return;
    rc->t = t;
    http_options_t o;
    memset(&o, 0, sizeof o);
    if (post) {
        o.method = "POST";
        o.body = t->req_post;
        o.body_len = t->req_postlen;
        o.content_type = "application/x-www-form-urlencoded";
    }
    o.flags = host_allowed(url) ? HTTP_F_ALLOW_BAD_CERT : 0;
    o.on_progress = doc_progress;
    /* the fragment is not sent */
    char u[B_URL_MAX];
    strlcpy(u, url, sizeof u);
    char *hash = strchr(u, '#');
    if (hash) *hash = 0;
    t->req = http_fetch_ex(u, &o, doc_done, rc);
    if (!t->req) { free(rc); return; }
    t->req_ctx = rc;
    t->loading = true;
    t->load_start = sys_uptime_ms();
    t->got = t->total = 0;
    browser_tab_changed(t);
}

void tab_go(tab_t *t, const char *url, int mode) {
    const char *post = NULL;
    uint32_t postlen = 0;
    if ((mode == NAV_RELOAD || mode == NAV_HIST) && t->hpos < t->nhist && t->hist[t->hpos].post) {
        post = t->hist[t->hpos].post;
        postlen = t->hist[t->hpos].postlen;
    }
    start(t, url, mode, post, postlen, false);
}

void tab_post(tab_t *t, const char *url, const char *body, uint32_t len) {
    start(t, url, NAV_NEW, body ? body : "", len, false);
}

void tab_free(tab_t *t) {
    tab_stop(t);
    page_clear(t);
    hist_free_from(t, 0);
    free(t->hist);
    free(t->req_post);
    free(t->hl_item);
    free(t->hl_off);
    free(t->hl_len);
    free(t);
}

/* ---- links and forms -------------------------------------------------------------------- */
static bool same_doc(const char *a, const char *b) {
    size_t la = strcspn(a, "#"), lb = strcspn(b, "#");
    return la == lb && !strncmp(a, b, la);
}

void tab_follow_link(tab_t *t, int link, bool new_tab) {
    if (link < 0 || link >= t->pg.nlinks) return;
    const char *href = t->pg.links[link].href;
    while (*href == ' ' || *href == '\t' || *href == '\n') href++;
    if (!strncasecmp(href, "javascript:", 11)) return;
    if (!strncasecmp(href, "mailto:", 7) || !strncasecmp(href, "tel:", 4)) {
        snprintf(t->b->status, sizeof t->b->status, "%s links cannot be opened in NovaOS", !strncasecmp(href, "tel:", 4) ? "Phone" : "E-mail");
        browser_tab_changed(t);
        return;
    }
    char abs[B_URL_MAX];
    if (!strncasecmp(href, "about:", 6)) strlcpy(abs, href, sizeof abs);
    else if (!url_resolve(t->pg.base, href, abs, sizeof abs)) return;
    const char *target = t->pg.links[link].target;
    if (new_tab || (target && !strcasecmp(target, "_blank"))) { browser_new_tab(t->b, abs, !new_tab); return; }
    const char *hash = strchr(abs, '#');
    if (hash && same_doc(abs, t->url) && !t->internal) {
        int y = web_anchor_y(&t->pg, hash + 1);
        if (t->nhist) { t->hist[t->hpos].sx = t->sx; t->hist[t->hpos].sy = t->sy; }
        t->req_mode = NAV_NEW;
        free(t->req_post);
        t->req_post = NULL;
        t->req_postlen = 0;
        /* a new history entry without reloading */
        char keep[B_URL_MAX];
        strlcpy(keep, abs, sizeof keep);
        hist_free_from(t, t->hpos + 1);
        hent_t *nh = realloc(t->hist, sizeof *nh * (size_t)(t->nhist + 1));
        if (nh) {
            t->hist = nh;
            memset(&t->hist[t->nhist], 0, sizeof *nh);
            t->hist[t->nhist].url = dupstr(keep);
            t->hpos = t->nhist++;
        }
        strlcpy(t->url, keep, sizeof t->url);
        tab_scroll_to(t, 0, y >= 0 ? y : 0);
        browser_set_address(t->b, NULL);
        browser_page_changed(t);
        browser_tab_changed(t);
        return;
    }
    tab_go(t, abs, NAV_NEW);
}

static void add_pair(char **buf, size_t *len, size_t *cap, const char *name, const char *value) {
    size_t need = strlen(name) * 9 + strlen(value) * 9 + 4;
    if (*len + need + 1 > *cap) {
        size_t nc = (*cap ? *cap * 2 : 1024) + need;
        char *nb = realloc(*buf, nc);
        if (!nb) return;
        *buf = nb;
        *cap = nc;
        (*buf)[*len] = 0;
    }
    if (*len) { strcat(*buf, "&"); (*len)++; }
    web_urlencode(name, *buf, *cap);
    strcat(*buf, "=");
    web_urlencode(value, *buf, *cap);
    *len = strlen(*buf);
}

void tab_submit(tab_t *t, int ctl) {
    if (ctl < 0 || ctl >= t->pg.nctrls) return;
    web_ctrl_t *from = &t->pg.ctrls[ctl];
    web_node_t *form = from->form;
    if (!form) return;
    web_ctrl_t *submitter = (from->type == C_SUBMIT || from->type == C_IMAGE) ? from : NULL;
    if (!submitter)   /* implicit submission (Enter in a text field): the form's first submit button */
        for (int i = 0; i < t->pg.nctrls; i++)
            if (t->pg.ctrls[i].form == form && (t->pg.ctrls[i].type == C_SUBMIT || t->pg.ctrls[i].type == C_IMAGE)) {
                submitter = &t->pg.ctrls[i];
                break;
            }
    char *data = calloc(1, 1024);
    size_t len = 0, cap = 1024;
    if (!data) return;
    for (int i = 0; i < t->pg.nctrls; i++) {
        web_ctrl_t *c = &t->pg.ctrls[i];
        if (c->form != form || !c->name || !c->name[0] || c->disabled) continue;
        switch (c->type) {
        case C_TEXT: case C_PASSWORD: case C_HIDDEN: case C_TEXTAREA:
            add_pair(&data, &len, &cap, c->name, c->value ? c->value : "");
            break;
        case C_CHECKBOX: case C_RADIO:
            if (c->checked) add_pair(&data, &len, &cap, c->name, c->value ? c->value : "on");
            break;
        case C_SELECT:
            if (c->nopts && c->sel >= 0 && c->sel < c->nopts) add_pair(&data, &len, &cap, c->name, c->opt_val[c->sel]);
            break;
        case C_SUBMIT:
            if (c == submitter) add_pair(&data, &len, &cap, c->name, c->value ? c->value : "");
            break;
        case C_IMAGE:
            if (c == submitter) {
                char nm[200];
                snprintf(nm, sizeof nm, "%s.x", c->name);
                add_pair(&data, &len, &cap, nm, "0");
                snprintf(nm, sizeof nm, "%s.y", c->name);
                add_pair(&data, &len, &cap, nm, "0");
            }
            break;
        default:
            break;
        }
    }
    const char *action = web_attr(form, "action");
    const char *method = web_attr(form, "method");
    char abs[B_URL_MAX];
    if (!action || !*action) strlcpy(abs, t->url, sizeof abs);
    else if (!url_resolve(t->pg.base, action, abs, sizeof abs)) { free(data); return; }
    abs[strcspn(abs, "#")] = 0;
    if (method && !strcasecmp(method, "post") && strncasecmp(abs, "about:", 6)) {
        klog("browser: POST %s (%u bytes)", abs, (unsigned)len);
        tab_post(t, abs, data, (uint32_t)len);
    } else {
        abs[strcspn(abs, "?")] = 0;
        size_t al = strlen(abs);
        char *u = malloc(al + len + 2);
        if (u) {
            memcpy(u, abs, al);
            u[al] = '?';
            memcpy(u + al + 1, data, len + 1);
            if (strlen(u) < B_URL_MAX) tab_go(t, u, NAV_NEW);
            free(u);
        }
    }
    free(data);
}
