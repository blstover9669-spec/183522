/*
 * web_paint.c - draws a laid-out page (the display list made by web_layout.c)
 * and its form controls, and finds what is under the mouse.
 */
#include "web.h"

#define FIELD_BORDER RGB(118, 118, 118)
#define FOCUS_RING   RGB(0, 95, 204)

static void draw_text_item(surface_t *s, const web_item_t *it, int X, int Y, bool hover) {
    gfx_text_n(s, X, Y, it->text, it->len, it->color, it->font);
    int fh = it->h;
    int th = fh >= 26 ? 2 : 1;
    if ((it->deco & DECO_UNDERLINE) || hover) gfx_fill_rect(s, X, Y + it->ref + 1 + (fh >= 20), it->w, th, it->color);
    if (it->deco & DECO_STRIKE) gfx_fill_rect(s, X, Y + it->ref - it->ref * 3 / 10, it->w, th, it->color);
    if (it->deco & DECO_OVERLINE) gfx_fill_rect(s, X, Y, it->w, th, it->color);
}

static void draw_image(web_page_t *pg, surface_t *s, const web_item_t *it, int X, int Y) {
    if (it->ref < 0 || it->ref >= pg->nimgs) return;
    web_image_t *im = &pg->imgs[it->ref];
    if (!im->surf) {
        /* not loaded (yet): a faint frame keeps the space visible */
        gfx_rect(s, X, Y, it->w, it->h, RGBA(0, 0, 0, 40));
        return;
    }
    const surface_t *src = im->surf;
    if (src->w != it->w || src->h != it->h) {
        if (!im->scaled || im->scaled->w != it->w || im->scaled->h != it->h) {
            if (im->scaled) surface_destroy(im->scaled);
            im->scaled = NULL;
            if ((int64_t)it->w * it->h <= 16 * 1024 * 1024) im->scaled = surface_scaled(im->surf, it->w, it->h);
        }
        if (!im->scaled) {
            gfx_blit_scaled(s, X, Y, it->w, it->h, im->surf, 0, 0, im->surf->w, im->surf->h, false);
            return;
        }
        src = im->scaled;
    }
    if (src->flags & SURF_ALPHA) gfx_blit_alpha(s, X, Y, src, 255);
    else gfx_blit(s, X, Y, src);
}

static void draw_marker(surface_t *s, const web_item_t *it, int X, int Y) {
    int sz = it->w;
    switch (it->marker) {
    case LS_DISC: gfx_fill_circle_aa(s, X + sz / 2.0, Y + sz / 2.0, sz / 2.0, it->color); break;
    case LS_CIRCLE: gfx_circle_aa(s, X + sz / 2.0, Y + sz / 2.0, sz / 2.0 - 0.5, 1.0, it->color); break;
    default: gfx_fill_rect(s, X, Y, sz, sz, it->color); break;
    }
}

static void draw_imgalt(surface_t *s, const web_item_t *it, int X, int Y) {
    gfx_rect(s, X, Y, it->w, it->h, RGB(160, 160, 160));
    if (!it->text || !it->font) return;
    rect_t old = s->clip;
    surface_set_clip(s, rect_intersect(old, rect_make(X + 2, Y + 2, it->w - 4, it->h - 4)));
    gfx_text(s, X + 4, Y + 3, it->text, it->color, it->font);
    s->clip = old;
}

void web_paint_ctrl(web_page_t *pg, web_ctrl_t *c, surface_t *s, int x, int y, bool focused, bool caret) {
    (void)pg;
    const font_t *f = c->font ? c->font : FONT_UI;
    int w = c->w, h = c->h;
    int fh = font_height(f);
    uint32_t text = c->disabled ? RGB(140, 140, 140) : RGB(16, 16, 16);
    switch (c->type) {
    case C_TEXT: case C_PASSWORD: case C_TEXTAREA: {
        gfx_fill_rect(s, x, y, w, h, c->disabled ? RGB(240, 240, 240) : RGB(255, 255, 255));
        gfx_rect(s, x, y, w, h, focused ? FOCUS_RING : FIELD_BORDER);
        if (focused) gfx_rect(s, x + 1, y + 1, w - 2, h - 2, FOCUS_RING);
        rect_t old = s->clip;
        surface_set_clip(s, rect_intersect(old, rect_make(x + 3, y + 2, w - 6, h - 4)));
        if (c->type == C_TEXTAREA) {
            int ly = y + 4, line = 0, cl = 0, cc = 0;
            const char *v = c->value ? c->value : "";
            /* find the caret's line and column */
            for (int i = 0; i < c->caret && v[i]; i++) { if (v[i] == '\n') { cl++; cc = 0; } else cc++; }
            int first = cl - (h - 8) / fh + 1;
            if (first < 0) first = 0;
            const char *p = v;
            while (*p || line == cl) {
                const char *e = strchr(p, '\n');
                int len = e ? (int)(e - p) : (int)strlen(p);
                if (line >= first) {
                    gfx_text_n(s, x + 5, ly, p, len, text, f);
                    if (focused && caret && line == cl) gfx_fill_rect(s, x + 5 + font_text_width_n(f, p, cc), ly, 1, fh, RGB(0, 0, 0));
                    ly += fh;
                    if (ly > y + h) break;
                }
                line++;
                if (!e) break;
                p = e + 1;
            }
        } else {
            char *shown = c->value ? c->value : "";
            char stars[512];
            if (c->type == C_PASSWORD) {
                int n = c->vlen < (int)sizeof stars - 1 ? c->vlen : (int)sizeof stars - 1;
                memset(stars, 0x95, (size_t)n);
                stars[n] = 0;
                shown = stars;
            }
            int cx = font_text_width_n(f, shown, c->caret);
            int avail = w - 10;
            if (cx - c->scroll > avail) c->scroll = cx - avail;
            if (cx < c->scroll) c->scroll = cx;
            if (c->scroll < 0) c->scroll = 0;
            int ty = y + (h - fh) / 2;
            if (!c->vlen && !focused) {
                const char *ph = web_attr(c->node, "placeholder");
                if (ph) gfx_text(s, x + 5, ty, ph, RGB(117, 117, 117), f);
            }
            gfx_text(s, x + 5 - c->scroll, ty, shown, text, f);
            if (focused && caret) gfx_fill_rect(s, x + 5 + cx - c->scroll, ty, 1, fh, RGB(0, 0, 0));
        }
        s->clip = old;
        break;
    }
    case C_CHECKBOX: case C_RADIO: {
        int sz = MIN(w, h) - 4;
        int bx = x + (w - sz) / 2, by = y + (h - sz) / 2;
        if (c->type == C_CHECKBOX) {
            if (c->checked) {
                gfx_fill_rounded_rect(s, bx, by, sz, sz, 2, FOCUS_RING);
                gfx_thick_line(s, bx + sz * 22 / 100, by + sz / 2, bx + sz * 42 / 100, by + sz * 72 / 100, MAX(1, sz / 7), RGB(255, 255, 255));
                gfx_thick_line(s, bx + sz * 42 / 100, by + sz * 72 / 100, bx + sz * 78 / 100, by + sz * 28 / 100, MAX(1, sz / 7), RGB(255, 255, 255));
            } else {
                gfx_fill_rounded_rect(s, bx, by, sz, sz, 2, FIELD_BORDER);
                gfx_fill_rounded_rect(s, bx + 1, by + 1, sz - 2, sz - 2, 1, RGB(255, 255, 255));
            }
        } else {
            double r = sz / 2.0;
            gfx_fill_circle_aa(s, bx + r, by + r, r, c->checked ? FOCUS_RING : FIELD_BORDER);
            gfx_fill_circle_aa(s, bx + r, by + r, r - 1.2, RGB(255, 255, 255));
            if (c->checked) gfx_fill_circle_aa(s, bx + r, by + r, r * 0.5, FOCUS_RING);
        }
        if (focused) gfx_rounded_rect(s, bx - 2, by - 2, sz + 4, sz + 4, 3, FOCUS_RING);
        break;
    }
    case C_SELECT: {
        gfx_fill_rounded_rect(s, x, y, w, h, 3, FIELD_BORDER);
        gfx_fill_rounded_rect(s, x + 1, y + 1, w - 2, h - 2, 2, RGB(255, 255, 255));
        if (focused) gfx_rounded_rect(s, x, y, w, h, 3, FOCUS_RING);
        const char *lab = c->sel >= 0 && c->sel < c->nopts ? c->opt_text[c->sel] : "";
        rect_t old = s->clip;
        surface_set_clip(s, rect_intersect(old, rect_make(x + 2, y, w - 22, h)));
        gfx_text(s, x + 6, y + (h - fh) / 2, lab, text, f);
        s->clip = old;
        int ax = x + w - 14, ay = y + h / 2 - 2;
        gfx_fill_triangle(s, ax - 4, ay, ax + 4, ay, ax, ay + 5, RGB(60, 60, 60));
        break;
    }
    default: {   /* buttons */
        if (c->type == C_IMAGE && c->img >= 0 && c->img < pg->nimgs && pg->imgs[c->img].surf) {
            web_image_t *im = &pg->imgs[c->img];
            gfx_blit_scaled(s, x, y, w, h, im->surf, 0, 0, im->surf->w, im->surf->h, true);
            break;
        }
        gfx_fill_rounded_rect(s, x, y, w, h, 3, FIELD_BORDER);
        gfx_fill_rounded_rect(s, x + 1, y + 1, w - 2, h - 2, 2, c->disabled ? RGB(245, 245, 245) : RGB(239, 239, 239));
        if (focused) gfx_rounded_rect(s, x, y, w, h, 3, FOCUS_RING);
        rect_t old = s->clip;
        surface_set_clip(s, rect_intersect(old, rect_make(x + 2, y, w - 4, h)));
        int tw = font_text_width(f, c->label);
        gfx_text(s, x + (w - tw) / 2, y + (h - fh) / 2, c->label, text, f);
        s->clip = old;
        break;
    }
    }
}

void web_paint(web_page_t *pg, surface_t *s, rect_t view, int sx, int sy, const web_paint_opts_t *o) {
    web_layout_t *L = &pg->lay;
    rect_t old = s->clip;
    surface_set_clip(s, rect_intersect(old, view));
    gfx_fill_rect(s, view.x, view.y, view.w, view.h, L->bg);
    int y0 = sy, y1 = sy + view.h, x0 = sx, x1 = sx + view.w;
    int hk = 0;
    for (int i = 0; i < L->nitems; i++) {
        const web_item_t *it = &L->items[i];
        if (it->y >= y1 || it->y + it->h <= y0 || it->x >= x1 || it->x + it->w <= x0) {
            if (o && o->nhl) while (hk < o->nhl && o->hl_item[hk] <= i) hk++;
            continue;
        }
        int X = view.x + it->x - sx, Y = view.y + it->y - sy;
        switch (it->kind) {
        case WI_RECT:
            gfx_fill_rect(s, X, Y, it->w, it->h, it->color);
            break;
        case WI_TEXT:
            if (o && o->nhl) {
                while (hk < o->nhl && o->hl_item[hk] < i) hk++;
                for (int k = hk; k < o->nhl && o->hl_item[k] == i; k++) {
                    int hx = font_text_width_n(it->font, it->text, o->hl_off[k]);
                    int hw = font_text_width_n(it->font, it->text + o->hl_off[k], o->hl_len[k]);
                    gfx_fill_rect(s, X + hx, Y, hw, it->h, k == o->hl_current ? RGB(255, 150, 50) : RGB(255, 230, 80));
                }
            }
            draw_text_item(s, it, X, Y, o && it->link >= 0 && it->link == o->hover_link && !(it->deco & DECO_UNDERLINE));
            break;
        case WI_IMAGE:
            draw_image(pg, s, it, X, Y);
            break;
        case WI_IMGALT:
            draw_imgalt(s, it, X, Y);
            break;
        case WI_MARKER:
            draw_marker(s, it, X, Y);
            break;
        case WI_CTRL:
            if (it->ref >= 0 && it->ref < pg->nctrls)
                web_paint_ctrl(pg, &pg->ctrls[it->ref], s, X, Y, o && o->focus_ctrl == it->ref, o && o->caret_on);
            break;
        }
    }
    s->clip = old;
}

int web_hit_link(web_page_t *pg, int x, int y) {
    web_layout_t *L = &pg->lay;
    for (int i = L->nitems - 1; i >= 0; i--) {
        const web_item_t *it = &L->items[i];
        if (it->link < 0) continue;
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) return it->link;
    }
    return -1;
}

int web_hit_ctrl(web_page_t *pg, int x, int y) {
    for (int i = 0; i < pg->nctrls; i++) {
        const web_ctrl_t *c = &pg->ctrls[i];
        if (c->w > 0 && x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h) return i;
    }
    return -1;
}
