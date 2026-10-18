#!/usr/bin/env python3
"""
genfont.py - generate NovaOS's embedded anti-aliased bitmap fonts.

Renders system TrueType/OpenType fonts (Inter for the UI, DejaVu Sans Mono for
monospace text) with PIL into 8-bit coverage bitmaps and writes them out as a
C source file (kernel/gfx/font_data.c).  This is a dev-time tool: the generated
file is committed so a normal build does not need PIL or the fonts.

Character set (single byte, "NovaOS-1252"):
  0x20-0x7E  ASCII
  0x80-0x9F  symbols: box drawing, blocks, arrows, bullets ... (see EXTRA)
  0xA0-0xFF  Latin-1 supplement

Usage:  python3 tools/genfont.py [output.c]
"""
import sys
from PIL import Image, ImageFont, ImageDraw

INTER = "/usr/share/fonts/opentype/inter/"
DEJAVU = "/usr/share/fonts/truetype/dejavu/"

# Codepoints for the 0x80-0x9F block.
EXTRA = [
    0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524,  # 80-87 box
    0x252C, 0x2534, 0x253C, 0x2588, 0x2593, 0x2592, 0x2591, 0x2580,  # 88-8F box/blocks
    0x2584, 0x2190, 0x2191, 0x2192, 0x2193, 0x2022, 0x2713, 0x2717,  # 90-97 arrows etc
    0x266A, 0x2605, 0x2665, 0x2666, 0x2663, 0x2660, 0x2026, 0x25B6,  # 98-9F
]

def codepoint(i):
    if 0x20 <= i <= 0x7E or 0xA0 <= i <= 0xFF:
        return i
    if 0x80 <= i <= 0x9F:
        return EXTRA[i - 0x80]
    return None

FIRST, LAST = 0x20, 0xFF

def gamma(a, g):
    return int(round(255.0 * ((a / 255.0) ** g)))

class Glyph:
    def __init__(self, adv, xoff, yoff, w, h, data):
        self.adv, self.xoff, self.yoff, self.w, self.h, self.data = adv, xoff, yoff, w, h, data

_sigs = {}
def notdef_signature(font):
    if id(font) not in _sigs:
        m, _ = font.getmask2(chr(0x10FFFD), mode="L", anchor="ls")
        _sigs[id(font)] = (m.size, bytes(m))
    return _sigs[id(font)]

def render_glyph(fonts, ch, ascent, g, cell_w=None):
    """Render char with the first font in `fonts` that has it."""
    for font in fonts:
        sig = notdef_signature(font)
        mask, off = font.getmask2(ch, mode="L", anchor="ls")
        if (mask.size, bytes(mask)) == sig and ch != " ":
            continue
        w, h = mask.size
        data = bytes(gamma(b, g) for b in bytes(mask))
        adv = int(round(font.getlength(ch)))
        xoff, yoff = off[0], off[1] + ascent
        if cell_w is not None:
            # Monospace: centre glyph horizontally within the cell.
            adv = cell_w
            if w > 0:
                fadv = font.getlength(ch)
                xoff = int(round(off[0] + (cell_w - fadv) / 2.0))
        # trim empty rows/cols to save space
        if w and h:
            img = Image.frombytes("L", (w, h), data)
            bbox = img.getbbox()
            if bbox:
                img = img.crop(bbox)
                xoff += bbox[0]; yoff += bbox[1]
                w, h = img.size
                data = img.tobytes()
            else:
                w = h = 0; data = b""
        return Glyph(adv, xoff, yoff, w, h, data)
    return None

def box_glyph(i, cw, chh):
    """Pixel-exact box drawing / block glyphs for monospace fonts."""
    cp = codepoint(i)
    img = Image.new("L", (cw, chh), 0)
    d = ImageDraw.Draw(img)
    mx, my = cw // 2, chh // 2
    t = max(1, cw // 8)
    def h(x0, x1): d.rectangle([x0, my - t // 2, x1, my - t // 2 + t - 1], fill=255)
    def v(y0, y1): d.rectangle([mx - t // 2, y0, mx - t // 2 + t - 1, y1], fill=255)
    L, R, T, B = 0, cw - 1, 0, chh - 1
    if cp == 0x2500: h(L, R)
    elif cp == 0x2502: v(T, B)
    elif cp == 0x250C: h(mx, R); v(my, B)
    elif cp == 0x2510: h(L, mx); v(my, B)
    elif cp == 0x2514: h(mx, R); v(T, my)
    elif cp == 0x2518: h(L, mx); v(T, my)
    elif cp == 0x251C: v(T, B); h(mx, R)
    elif cp == 0x2524: v(T, B); h(L, mx)
    elif cp == 0x252C: h(L, R); v(my, B)
    elif cp == 0x2534: h(L, R); v(T, my)
    elif cp == 0x253C: h(L, R); v(T, B)
    elif cp == 0x2588: d.rectangle([L, T, R, B], fill=255)
    elif cp in (0x2593, 0x2592, 0x2591):
        lvl = {0x2593: 190, 0x2592: 128, 0x2591: 64}[cp]
        for y in range(chh):
            for x in range(cw):
                if (x + y) % 2 == 0 or cp == 0x2593:
                    img.putpixel((x, y), lvl if cp != 0x2593 else (255 if (x + y) % 2 == 0 else 128))
    elif cp == 0x2580: d.rectangle([L, T, R, my - 1], fill=255)
    elif cp == 0x2584: d.rectangle([L, my, R, B], fill=255)
    else:
        return None
    bbox = img.getbbox()
    if not bbox:
        return Glyph(cw, 0, 0, 0, 0, b"")
    crop = img.crop(bbox)
    return Glyph(cw, bbox[0], bbox[1], crop.size[0], crop.size[1], crop.tobytes())

def make_font(cname, paths, size, height, ascent, mono_cell=None, g=0.85):
    fonts = [ImageFont.truetype(p, size) for p in paths]
    glyphs = []
    for i in range(FIRST, LAST + 1):
        cp = codepoint(i)
        gl = None
        if cp is not None and mono_cell is not None:
            gl = box_glyph(i, mono_cell, height)
        if gl is None and cp is not None:
            gl = render_glyph(fonts, chr(cp), ascent, g, mono_cell)
        if gl is None:
            # Unknown: draw an outlined box.
            w = mono_cell or max(4, size // 2)
            bh = ascent - 2
            img = Image.new("L", (w - 2, bh), 0)
            ImageDraw.Draw(img).rectangle([0, 0, w - 3, bh - 1], outline=160)
            gl = Glyph(w, 1, 2, w - 2, bh, img.tobytes())
        glyphs.append(gl)
    return dict(cname=cname, height=height, ascent=ascent,
                mono=mono_cell or 0, glyphs=glyphs, size=size)

FONTS = [
    # cname,          files,                                                    px, line, ascent, mono
    ("font_mono",     [DEJAVU + "DejaVuSansMono.ttf", DEJAVU + "DejaVuSans.ttf"], 13, 16, 12, 8),
    ("font_mono_2x",  [DEJAVU + "DejaVuSansMono.ttf", DEJAVU + "DejaVuSans.ttf"], 26, 32, 24, 16),
    ("font_mono_bold",[DEJAVU + "DejaVuSansMono-Bold.ttf", DEJAVU + "DejaVuSans-Bold.ttf"], 13, 16, 12, 8),
    ("font_ui",       [INTER + "Inter-Regular.otf", DEJAVU + "DejaVuSans.ttf"],    13, 17, 13, None),
    ("font_ui_bold",  [INTER + "Inter-SemiBold.otf", DEJAVU + "DejaVuSans-Bold.ttf"], 13, 17, 13, None),
    ("font_ui_large", [INTER + "Inter-SemiBold.otf", DEJAVU + "DejaVuSans-Bold.ttf"], 18, 23, 18, None),
    ("font_ui_title", [INTER + "Inter-Bold.otf", DEJAVU + "DejaVuSans-Bold.ttf"],     28, 35, 28, None),
    ("font_ui_huge",  [INTER + "Inter-Light.otf", DEJAVU + "DejaVuSans.ttf"],         52, 64, 51, None),
]

def emit(out):
    w = out.write
    w("/* Generated by tools/genfont.py - DO NOT EDIT. */\n")
    w("/* Glyphs rendered from Inter (SIL OFL 1.1) and DejaVu fonts (Bitstream Vera license). */\n")
    w('#include "font_internal.h"\n\n')
    total = 0
    for name, paths, px, line, asc, mono in FONTS:
        f = make_font(name, paths, px, line, asc, mono)
        blob = bytearray()
        w("static const glyph_t %s_glyphs[%d] = {\n" % (name, len(f["glyphs"])))
        for gl in f["glyphs"]:
            w("  {%d,%d,%d,%d,%d,%d},\n" % (gl.adv, gl.xoff, gl.yoff, gl.w, gl.h, len(blob)))
            blob += gl.data
        w("};\n")
        w("static const uint8_t %s_data[%d] = {\n" % (name, max(1, len(blob))))
        for i in range(0, len(blob), 24):
            w("  " + ",".join(str(b) for b in blob[i:i + 24]) + ",\n")
        if not blob:
            w("  0\n")
        w("};\n")
        w("const font_t %s = { \"%s\", %d, %d, %d, %d, %d, %s_glyphs, %s_data };\n\n" %
          (name, name, f["height"], f["ascent"], f["mono"], FIRST, LAST - FIRST + 1, name, name))
        total += len(blob)
        sys.stderr.write("%-16s %3dpx line=%d bytes=%d\n" % (name, px, line, len(blob)))
    sys.stderr.write("total glyph bytes: %d\n" % total)

if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "kernel/gfx/font_data.c"
    with open(path, "w") as fh:
        emit(fh)
