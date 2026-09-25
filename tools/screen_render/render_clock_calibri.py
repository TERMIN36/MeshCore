"""Calibri clock for the 2.13\" e-ink panel, with optional unread counters.

The footer is drawn only for counters that are present.
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_DIR = r"C:\Windows\Fonts"
W, H = 250, 122
VIEW = 3
TIME = "21:48"
PAPER = (224, 224, 214)
INK = (28, 30, 34)
BEZEL = (46, 48, 52)
FONT_FILE = "calibri.ttf"

SCREENS = [
    ("01_both", "Groups and Chat", [("Groups", 5), ("Chat", 3)]),
    ("02_groups", "Groups only", [("Groups", 5)]),
    ("03_chat", "Chat only", [("Chat", 3)]),
    ("04_none", "No unread", []),
]


def load(size):
    return ImageFont.truetype(os.path.join(FONT_DIR, FONT_FILE), size)


def text_box(fnt, text):
    b = fnt.getbbox(text)
    return b[2] - b[0], b[3] - b[1], b


def fit(text, max_w, max_h):
    lo, hi, best = 8, 500, 8
    while lo <= hi:
        mid = (lo + hi) // 2
        w, h, _ = text_box(load(mid), text)
        if w <= max_w and h <= max_h:
            best = mid
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def footer_text(items):
    return "    ".join("%s: %d" % (name, n) for name, n in items)


def render(items):
    pw, ph = W * VIEW, H * VIEW
    img = Image.new("RGB", (pw, ph), PAPER)
    dr = ImageDraw.Draw(img)

    footer_h = 0
    if items:
        label = footer_text(items)
        fsize = fit(label, pw - 24, 22 * VIEW)
        fnt = load(fsize)
        w, h, box = text_box(fnt, label)
        x = (pw - w) // 2 - box[0]
        y = ph - 8 * VIEW - h - box[1]
        dr.text((x, y), label, font=fnt, fill=INK)
        footer_h = h + 14 * VIEW

    size = fit(TIME, pw - 16, ph - footer_h - 12 * VIEW)
    fnt = load(size)
    w, h, box = text_box(fnt, TIME)
    area_h = ph - footer_h
    x = (pw - w) // 2 - box[0]
    y = (area_h - h) // 2 - box[1]
    dr.text((x, y), TIME, font=fnt, fill=INK)
    return img


def with_bezel(face):
    pad = 4 * VIEW
    img = Image.new("RGB", (face.width + pad * 2, face.height + pad * 2), BEZEL)
    img.paste(face, (pad, pad))
    return img


def main():
    out = os.path.join(ROOT, "docs", "screens", "clock_calibri")
    os.makedirs(out, exist_ok=True)
    label_font = load(22)
    title_font = load(28)
    panels = []
    for fname, label, items in SCREENS:
        panel = with_bezel(render(items))
        panel.save(os.path.join(out, fname + ".png"))
        panels.append((label, panel))

    cols = 2
    cw, ch = panels[0][1].size
    cap, gap, top = 36, 28, 64
    rows = (len(panels) + cols - 1) // cols
    sheet = Image.new("RGB", (gap + cols * (cw + gap), top + gap + rows * (ch + cap + gap)), (32, 34, 40))
    dr = ImageDraw.Draw(sheet)
    dr.text((gap, 16), "Calibri clock — e-ink 250x122", font=title_font, fill=(240, 240, 240))
    for i, (label, im) in enumerate(panels):
        x = gap + (i % cols) * (cw + gap)
        y = top + gap + (i // cols) * (ch + cap + gap)
        dr.text((x, y), "%d.  %s" % (i + 1, label), font=label_font, fill=(220, 220, 220))
        sheet.paste(im, (x, y + cap))
    gallery = os.path.join(ROOT, "docs", "screens", "gallery_clock_calibri.png")
    sheet.save(gallery)
    print(gallery)


if __name__ == "__main__":
    main()
