/* apps.c - application registry (the ".apps" linker section), launching and icon cache. */
#include "wm_internal.h"

extern const app_desc_t *const __apps_start[];
extern const app_desc_t *const __apps_end[];

static const app_desc_t **apps;
static int napps;

static int cmp_app(const void *a, const void *b) {
    const app_desc_t *x = *(const app_desc_t *const *)a, *y = *(const app_desc_t *const *)b;
    return strcasecmp(x->name, y->name);
}

void apps_init(void) {
    int n = (int)(__apps_end - __apps_start);
    apps = kcalloc((size_t)MAX(n, 1), sizeof *apps);
    napps = 0;
    for (int i = 0; i < n; i++) {
        const app_desc_t *a = __apps_start[i];
        if (!a || !a->name || !a->launch) continue;
        if (app_find(a->name)) { klog("apps: duplicate app name \"%s\" ignored", a->name); continue; }
        apps[napps++] = a;
    }
    qsort(apps, (size_t)napps, sizeof *apps, cmp_app);
    klog("apps: %d registered", napps);
    for (int i = 0; i < napps; i++)
        klog("  app: %-20s [%s]", apps[i]->name, apps[i]->category ? apps[i]->category : "-");
}

int app_count(void) { return napps; }
const app_desc_t *app_get(int i) { return (i >= 0 && i < napps) ? apps[i] : NULL; }

const app_desc_t *app_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < napps; i++)
        if (!strcasecmp(apps[i]->name, name)) return apps[i];
    /* also accept names with spaces replaced by '_' or removed ("TaskManager") */
    for (int i = 0; i < napps; i++) {
        const char *a = apps[i]->name, *b = name;
        while (*a && *b) {
            if (*a == ' ') { a++; if (*b == '_' || *b == '-') b++; continue; }
            if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) break;
            a++; b++;
        }
        if (!*a && !*b) return apps[i];
    }
    return NULL;
}

int app_window_count(const app_desc_t *app) {
    int n = 0;
    for (window_t *w = wm_bottom; w; w = w->above)
        if (w->app == app && !w->closing && w->layer == LAYER_NORMAL) n++;
    return n;
}

bool app_launch(const app_desc_t *app, const char *arg) {
    if (!app || !app->launch) return false;
    if (app->flags & APP_SINGLE) {
        for (window_t *w = wm_top; w; w = w->below)
            if (w->app == app && !w->closing && w->layer == LAYER_NORMAL && !w->owner) {
                win_focus(w);
                return true;
            }
    }
    klog("apps: launching %s%s%s", app->name, arg ? " " : "", arg ? arg : "");
    const app_desc_t *saved = wm_current_app;
    wm_current_app = app;
    app->launch(arg);
    wm_current_app = saved;
    return true;
}

bool app_launch_by_name(const char *name, const char *arg) {
    const app_desc_t *a = app_find(name);
    if (!a) { klog("apps: no app named \"%s\"", name ? name : "(null)"); return false; }
    return app_launch(a, arg);
}

static bool type_matches(const char *list, const char *ext) {
    if (!list || !*ext) return false;
    const char *p = list;
    size_t el = strlen(ext);
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == ';') p++;
        const char *s = p;
        while (*p && *p != ',' && *p != ' ' && *p != ';') p++;
        if ((size_t)(p - s) == el && !strncasecmp(s, ext, el)) return true;
    }
    return false;
}

const app_desc_t *app_for_file(const char *path) {
    const char *ext = vfs_extension(path);
    for (int i = 0; i < napps; i++)
        if (type_matches(apps[i]->file_types, ext)) return apps[i];
    return NULL;
}

bool app_open_file(const char *path) {
    const app_desc_t *a = app_for_file(path);
    if (!a) return false;
    return app_launch(a, path);
}

static int cat_rank(const char *c) {
    static const char *order[] = { CAT_ACCESSORIES, CAT_OFFICE, CAT_INTERNET, CAT_GRAPHICS, CAT_MULTIMEDIA,
                                   CAT_GAMES, CAT_DEVELOPMENT, CAT_UTILITIES, CAT_SYSTEM };
    for (int i = 0; i < ARRAY_SIZE(order); i++) if (!strcasecmp(order[i], c)) return i;
    return 100;
}

int app_categories(const char **out, int max) {
    int n = 0;
    for (int i = 0; i < napps; i++) {
        if (apps[i]->flags & APP_HIDDEN) continue;
        const char *c = apps[i]->category ? apps[i]->category : "Other";
        bool seen = false;
        for (int j = 0; j < n && j < max; j++) if (!strcasecmp(out[j], c)) seen = true;
        if (!seen) { if (n < max) out[n] = c; n++; }
    }
    int m = MIN(n, max);
    /* sort by preferred order, then name */
    for (int a = 1; a < m; a++)
        for (int b = a; b > 0; b--) {
            int ra = cat_rank(out[b - 1]), rb = cat_rank(out[b]);
            if (ra > rb || (ra == rb && strcasecmp(out[b - 1], out[b]) > 0)) {
                const char *t = out[b]; out[b] = out[b - 1]; out[b - 1] = t;
            } else break;
        }
    return n;
}

/* ---- icons ------------------------------------------------------------------------- */
void default_icon(surface_t *s, int x, int y, int size, const char *name) {
    uint32_t h = 0;
    for (const char *p = name; p && *p; p++) h = h * 31 + (uint8_t)*p;
    uint32_t c1 = color_hsv((int)(h % 360), 150, 230), c2 = color_hsv((int)((h + 40) % 360), 180, 170);
    int m = size / 12;
    gfx_fill_rounded_gradient_v(s, x + m, y + m, size - 2 * m, size - 2 * m, size / 5, c1, c2);
    char letter[2] = { name && name[0] ? (char)toupper((unsigned char)name[0]) : '?', 0 };
    const font_t *f = size >= 80 ? FONT_UI_HUGE : size >= 40 ? FONT_UI_TITLE : size >= 24 ? FONT_UI_LARGE : FONT_UI_BOLD;
    gfx_text_aligned(s, rect_make(x, y, size, size), letter, COLOR_WHITE, f, ALIGN_CENTER | ALIGN_MIDDLE);
}

typedef struct icon_entry {
    const app_desc_t *app;
    int size;
    bool dark;
    surface_t *img;
    struct icon_entry *next;
} icon_entry_t;

static icon_entry_t *icon_cache;

void app_icon_cache_clear(void) {
    while (icon_cache) {
        icon_entry_t *e = icon_cache;
        icon_cache = e->next;
        surface_destroy(e->img);
        kfree(e);
    }
}

const surface_t *app_icon(const app_desc_t *app, int size) {
    if (!app || size <= 0 || size > 256) return NULL;
    for (icon_entry_t *e = icon_cache; e; e = e->next)
        if (e->app == app && e->size == size) return e->img;
    /* render at 2x into a transparent surface, then area-downsample for anti-aliasing */
    int big = size * 2;
    surface_t *hi = surface_create_alpha(big, big);
    if (!hi) return NULL;
    if (app->draw_icon) app->draw_icon(hi, 0, 0, big);
    else default_icon(hi, 0, 0, big, app->name);
    surface_reset_clip(hi);
    surface_t *img = surface_scaled(hi, size, size);
    surface_destroy(hi);
    if (!img) return NULL;
    icon_entry_t *e = kcalloc(1, sizeof *e);
    if (!e) { surface_destroy(img); return NULL; }
    e->app = app;
    e->size = size;
    e->img = img;
    e->next = icon_cache;
    icon_cache = e;
    return img;
}

void app_draw_icon(surface_t *dst, const app_desc_t *app, int x, int y, int size) {
    const surface_t *img = app_icon(app, size);
    if (img) gfx_blit_alpha(dst, x, y, img, 255);
}
