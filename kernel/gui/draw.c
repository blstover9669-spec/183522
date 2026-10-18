/* draw.c - themed drawing helpers and vector glyphs used by the UI. */
#include "wm_internal.h"

void ui_draw_button(surface_t *s, rect_t r, const char *label, int state) {
    const theme_t *t = theme_get();
    uint32_t bg, border, fg;
    if (state & UI_STATE_DEFAULT) {
        bg = t->accent;
        if (state & UI_STATE_PRESSED) bg = color_shade(t->accent, -35);
        else if (state & UI_STATE_HOVER) bg = t->accent_hover;
        border = color_shade(bg, -25);
        fg = t->accent_text;
    } else {
        bg = t->button_bg;
        if (state & UI_STATE_PRESSED) bg = t->button_pressed;
        else if (state & UI_STATE_HOVER) bg = t->button_hover;
        border = t->button_border;
        fg = t->button_text;
    }
    if (state & UI_STATE_DISABLED) {
        bg = color_mix(bg, t->window_bg, 140);
        border = color_mix(border, t->window_bg, 120);
        fg = t->disabled_text;
    }
    int rad = MIN(6, r.h / 2);
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, rad, border);
    /* slightly darker bottom edge for depth */
    gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, rad - 1, bg);
    if (!(state & (UI_STATE_PRESSED | UI_STATE_DEFAULT | UI_STATE_DISABLED)))
        gfx_hline(s, r.x + rad, r.y + r.h - 1, r.w - 2 * rad, color_shade(border, t->dark ? -20 : -18));
    if ((state & UI_STATE_FOCUSED) && !(state & UI_STATE_DISABLED))
        gfx_rounded_rect(s, r.x - 2, r.y - 2, r.w + 4, r.h + 4, rad + 2, color_alpha(t->accent, 170));
    if (label && *label) {
        rect_t tr = r;
        if (state & UI_STATE_PRESSED) tr.y += 1;
        gfx_text_aligned(s, tr, label, fg, FONT_UI, ALIGN_CENTER | ALIGN_MIDDLE);
    }
}

void ui_draw_panel(surface_t *s, rect_t r) {
    const theme_t *t = theme_get();
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 8, color_mix(t->panel_bg, t->window_border, 70));
    gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 7, t->panel_bg);
}

void ui_draw_field(surface_t *s, rect_t r, int state) {
    const theme_t *t = theme_get();
    uint32_t border = (state & UI_STATE_FOCUSED) ? t->accent : ((state & UI_STATE_HOVER) ? color_shade(t->field_border, t->dark ? 25 : -25) : t->field_border);
    uint32_t bg = (state & UI_STATE_DISABLED) ? color_mix(t->field_bg, t->window_bg, 160) : t->field_bg;
    gfx_fill_rounded_rect(s, r.x, r.y, r.w, r.h, 5, border);
    gfx_fill_rounded_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 4, bg);
    if (state & UI_STATE_FOCUSED) {
        gfx_fill_rect(s, r.x + 3, r.y + r.h - 2, r.w - 6, 2, t->accent);
    }
}

void ui_draw_checkbox(surface_t *s, int x, int y, int state) {
    const theme_t *t = theme_get();
    bool checked = state & UI_STATE_CHECKED;
    if (checked) {
        uint32_t bg = (state & UI_STATE_DISABLED) ? t->disabled_text : ((state & UI_STATE_HOVER) ? t->accent_hover : t->accent);
        gfx_fill_rounded_rect(s, x, y, 18, 18, 4, bg);
        ui_draw_glyph(s, UI_GLYPH_CHECK, x + 3, y + 3, 12, t->accent_text);
    } else {
        uint32_t border = (state & UI_STATE_HOVER) ? color_shade(t->field_border, t->dark ? 40 : -45) : t->field_border;
        gfx_fill_rounded_rect(s, x, y, 18, 18, 4, border);
        gfx_fill_rounded_rect(s, x + 1, y + 1, 16, 16, 3, t->field_bg);
    }
    if (state & UI_STATE_FOCUSED) gfx_rounded_rect(s, x - 2, y - 2, 22, 22, 6, color_alpha(t->accent, 170));
}

void ui_draw_separator(surface_t *s, int x, int y, int w) {
    gfx_hline(s, x, y, w, theme_get()->menu_separator);
}

void ui_draw_scrollbar(surface_t *s, rect_t r, bool vertical, int total, int page, int pos, int state) {
    const theme_t *t = theme_get();
    gfx_fill_rect(s, r.x, r.y, r.w, r.h, t->scrollbar_bg);
    if (total <= page || total <= 0) return;
    int len = vertical ? r.h : r.w;
    int thick = vertical ? r.w : r.h;
    int tl = MAX(24, (int)((int64_t)len * page / total));
    if (tl > len) tl = len;
    int maxpos = total - page;
    int tp = maxpos > 0 ? (int)((int64_t)(len - tl) * pos / maxpos) : 0;
    uint32_t c = t->scrollbar_thumb;
    if (state & UI_STATE_PRESSED) c = color_shade(c, t->dark ? 40 : -40);
    else if (state & UI_STATE_HOVER) c = color_shade(c, t->dark ? 20 : -20);
    int m = thick >= 10 ? 3 : 1;
    if (vertical) gfx_fill_rounded_rect(s, r.x + m, r.y + tp + 2, r.w - 2 * m, tl - 4, (r.w - 2 * m) / 2, c);
    else gfx_fill_rounded_rect(s, r.x + tp + 2, r.y + m, tl - 4, r.h - 2 * m, (r.h - 2 * m) / 2, c);
}

/* AA polyline with thickness th */
static void stroke(surface_t *s, double x0, double y0, double x1, double y1, double th, uint32_t c) {
    double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
    if (len < 0.01) return;
    double nx = -dy / len, ny = dx / len;
    int n = (int)ceil(th * 2);
    for (int i = 0; i < n; i++) {
        double o = -th / 2 + (th * (i + 0.5)) / n;
        gfx_line_aa(s, x0 + nx * o, y0 + ny * o, x1 + nx * o, y1 + ny * o, color_alpha(c, (int)(COLOR_A(c) * 0.75)));
    }
}

void ui_draw_glyph(surface_t *s, int glyph, int x, int y, int size, uint32_t c) {
    double sz = size;
    switch (glyph) {
    case UI_GLYPH_CHECK:
        stroke(s, x + sz * 0.08, y + sz * 0.52, x + sz * 0.38, y + sz * 0.82, sz / 7 + 0.6, c);
        stroke(s, x + sz * 0.38, y + sz * 0.82, x + sz * 0.92, y + sz * 0.18, sz / 7 + 0.6, c);
        break;
    case UI_GLYPH_CLOSE:
        gfx_line_aa(s, x, y, x + sz - 1, y + sz - 1, c);
        gfx_line_aa(s, x + sz - 1, y, x, y + sz - 1, c);
        gfx_line_aa(s, x + 0.5, y, x + sz - 0.5, y + sz - 1, color_alpha(c, 90));
        gfx_line_aa(s, x + sz - 1.5, y, x - 0.5, y + sz - 1, color_alpha(c, 90));
        break;
    case UI_GLYPH_MINIMIZE:
        gfx_hline(s, x, y + size / 2, size, c);
        break;
    case UI_GLYPH_MAXIMIZE:
        gfx_rounded_rect(s, x, y, size, size, 2, c);
        break;
    case UI_GLYPH_RESTORE:
        gfx_rounded_rect(s, x, y + 2, size - 2, size - 2, 2, c);
        gfx_hline(s, x + 3, y, size - 4, c);
        gfx_vline(s, x + size - 1, y + 1, size - 4, c);
        gfx_pixel(s, x + 2, y, color_alpha(c, 120));
        gfx_pixel(s, x + size - 1, y + size - 3, color_alpha(c, 120));
        break;
    case UI_GLYPH_ARROW_UP:
        stroke(s, x + sz * 0.15, y + sz * 0.68, x + sz * 0.5, y + sz * 0.32, 1.4, c);
        stroke(s, x + sz * 0.5, y + sz * 0.32, x + sz * 0.85, y + sz * 0.68, 1.4, c);
        break;
    case UI_GLYPH_ARROW_DOWN:
        stroke(s, x + sz * 0.15, y + sz * 0.35, x + sz * 0.5, y + sz * 0.70, 1.4, c);
        stroke(s, x + sz * 0.5, y + sz * 0.70, x + sz * 0.85, y + sz * 0.35, 1.4, c);
        break;
    case UI_GLYPH_ARROW_LEFT:
        stroke(s, x + sz * 0.65, y + sz * 0.15, x + sz * 0.30, y + sz * 0.5, 1.4, c);
        stroke(s, x + sz * 0.30, y + sz * 0.5, x + sz * 0.65, y + sz * 0.85, 1.4, c);
        break;
    case UI_GLYPH_ARROW_RIGHT:
        stroke(s, x + sz * 0.35, y + sz * 0.15, x + sz * 0.70, y + sz * 0.5, 1.4, c);
        stroke(s, x + sz * 0.70, y + sz * 0.5, x + sz * 0.35, y + sz * 0.85, 1.4, c);
        break;
    case UI_GLYPH_INFO: case UI_GLYPH_ERROR: case UI_GLYPH_QUESTION: {
        uint32_t bg = glyph == UI_GLYPH_ERROR ? RGB(220, 50, 50) : glyph == UI_GLYPH_QUESTION ? RGB(60, 130, 230) : RGB(40, 120, 220);
        if (glyph == UI_GLYPH_INFO) bg = theme_get()->accent;
        gfx_fill_circle_aa(s, x + sz / 2, y + sz / 2, sz / 2 - 0.5, bg);
        const char *txt = glyph == UI_GLYPH_ERROR ? "\xD7" : glyph == UI_GLYPH_QUESTION ? "?" : "i";
        const font_t *f = size >= 28 ? FONT_UI_LARGE : FONT_UI_BOLD;
        gfx_text_aligned(s, rect_make(x, y, size, size), txt, COLOR_WHITE, f, ALIGN_CENTER | ALIGN_MIDDLE);
        break;
    }
    case UI_GLYPH_WARNING: {
        point_t p[3] = { { x + size / 2, y + 1 }, { x + size - 1, y + size - 2 }, { x, y + size - 2 } };
        gfx_fill_polygon(s, p, 3, RGB(240, 170, 20));
        gfx_line_aa(s, p[0].x, p[0].y, p[1].x, p[1].y, RGB(240, 170, 20));
        gfx_line_aa(s, p[0].x, p[0].y, p[2].x, p[2].y, RGB(240, 170, 20));
        const font_t *f = size >= 28 ? FONT_UI_LARGE : FONT_UI_BOLD;
        gfx_text_aligned(s, rect_make(x, y + size / 8, size, size), "!", RGB(40, 30, 0), f, ALIGN_CENTER | ALIGN_MIDDLE);
        break;
    }
    case UI_GLYPH_FOLDER: {
        uint32_t back = RGB(230, 170, 40), front = RGB(250, 200, 70);
        int h = size * 3 / 4, top = y + (size - h) / 2;
        gfx_fill_rounded_rect(s, x, top, size * 2 / 5, h / 4 + 2, 2, back);
        gfx_fill_rounded_rect(s, x, top + h / 6, size, h - h / 6, 2, back);
        gfx_fill_rounded_rect(s, x, top + h / 3, size, h - h / 3, 2, front);
        break;
    }
    case UI_GLYPH_FILE: {
        int w = size * 3 / 4, ox = x + (size - w) / 2, fold = size / 4;
        point_t p[5] = { { ox, y }, { ox + w - fold, y }, { ox + w, y + fold }, { ox + w, y + size }, { ox, y + size } };
        gfx_fill_polygon(s, p, 5, RGB(250, 251, 253));
        gfx_line(s, ox, y, ox + w - fold, y, RGB(150, 156, 168));
        gfx_line(s, ox + w - fold, y, ox + w, y + fold, RGB(150, 156, 168));
        gfx_line(s, ox + w, y + fold, ox + w, y + size - 1, RGB(150, 156, 168));
        gfx_line(s, ox, y + size - 1, ox + w, y + size - 1, RGB(150, 156, 168));
        gfx_line(s, ox, y, ox, y + size - 1, RGB(150, 156, 168));
        gfx_fill_triangle(s, ox + w - fold, y, ox + w - fold, y + fold, ox + w, y + fold, RGB(210, 214, 222));
        break;
    }
    default:
        break;
    }
}
