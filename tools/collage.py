#!/usr/bin/env python3
"""collage.py - build a labelled grid of app screenshots (docs/screenshots/all-apps.png).

Each app screenshot is a full 1024x768 desktop with one app open (tools/shot.py with
autorun=<App>). The app's window is cut out by comparing the screenshot with an empty
desktop taken the same way (same wallpaper, icons and pointer position), so only what
the app drew is kept. Needs python3 + PIL.

    tools/collage.py --desktop shots/desktop.png --out docs/screenshots/all-apps.png \
        "Chess=shots/chess.png" "Paint=shots/paint.png" ...

An optional category after the name is printed under it: "Chess|Games=shots/chess.png".
"""
import argparse
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

FONT_DIRS = ["/usr/share/fonts/opentype/inter", "/usr/share/fonts/truetype/dejavu"]


def font(names, size):
    for d in FONT_DIRS:
        for n in names:
            p = os.path.join(d, n)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def window_box(shot, desktop, taskbar_h=48, margin=6):
    """Bounding box of everything that differs from the empty desktop (above the taskbar)."""
    w, h = shot.size
    area = (0, 0, w, h - taskbar_h)
    diff = ImageChops.difference(shot.crop(area), desktop.crop(area)).convert("L")
    # shot.py parks the pointer in the top-left corner; after a click it is elsewhere, so
    # the corner differs from the empty desktop without belonging to the app
    diff.paste(0, (0, 0, 24, 32))
    mask = diff.point(lambda v: 255 if v > 24 else 0).filter(ImageFilter.MinFilter(3))
    box = mask.getbbox()
    if not box:
        return (0, 0, w, h)
    x0, y0, x1, y1 = box
    return (max(0, x0 - margin), max(0, y0 - margin), min(w, x1 + margin), min(h - taskbar_h, y1 + margin))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--desktop", required=True, help="screenshot of the empty desktop")
    ap.add_argument("--out", required=True)
    ap.add_argument("--cols", type=int, default=6)
    ap.add_argument("--cell", default="360x270", help="thumbnail area per app, WxH")
    ap.add_argument("--title", default="NovaOS 1.5 Developer Preview - every app")
    ap.add_argument("items", nargs="+", help='"Name[|Category]=path.png"')
    args = ap.parse_args()

    cw, ch = (int(v) for v in args.cell.split("x"))
    gap, label_h, head_h = 18, 46, 78
    cols = args.cols
    rows = (len(args.items) + cols - 1) // cols
    W = cols * cw + (cols + 1) * gap
    H = head_h + rows * (ch + label_h) + (rows + 1) * gap

    bg = (24, 26, 38)
    card = (36, 39, 56)
    out = Image.new("RGB", (W, H), bg)
    d = ImageDraw.Draw(out)
    f_title = font(["Inter-Bold.otf", "DejaVuSans-Bold.ttf"], 34)
    f_name = font(["Inter-SemiBold.otf", "DejaVuSans-Bold.ttf"], 19)
    f_cat = font(["Inter-Regular.otf", "DejaVuSans.ttf"], 14)
    d.text((gap + 4, 22), args.title, font=f_title, fill=(240, 242, 250))
    sub = "%d apps, each screenshot taken in QEMU from the release build" % len(args.items)
    tw = d.textlength(sub, font=f_cat)
    d.text((W - gap - tw - 4, 38), sub, font=f_cat, fill=(150, 156, 180))

    desktop = Image.open(args.desktop).convert("RGB")
    for i, item in enumerate(args.items):
        label, path = item.split("=", 1)
        name, _, cat = label.partition("|")
        shot = Image.open(path).convert("RGB")
        win = shot.crop(window_box(shot, desktop))
        scale = min(cw / win.width, ch / win.height, 1.0)
        thumb = win.resize((max(1, round(win.width * scale)), max(1, round(win.height * scale))), Image.LANCZOS)
        r, c = divmod(i, cols)
        x = gap + c * (cw + gap)
        y = head_h + gap + r * (ch + label_h + gap)
        d.rounded_rectangle((x - 6, y - 6, x + cw + 6, y + ch + label_h), radius=10, fill=card)
        out.paste(thumb, (x + (cw - thumb.width) // 2, y + (ch - thumb.height) // 2))
        nw = d.textlength(name, font=f_name)
        if cat:
            cwid = d.textlength(cat, font=f_cat)
            total = nw + 10 + cwid
            d.text((x + (cw - total) / 2, y + ch + 12), name, font=f_name, fill=(240, 242, 250))
            d.text((x + (cw - total) / 2 + nw + 10, y + ch + 16), cat, font=f_cat, fill=(140, 170, 255))
        else:
            d.text((x + (cw - nw) / 2, y + ch + 12), name, font=f_name, fill=(240, 242, 250))
    out.save(args.out, optimize=True)
    print("%s: %dx%d, %d apps, %d bytes" % (args.out, W, H, len(args.items), os.path.getsize(args.out)))


if __name__ == "__main__":
    main()
