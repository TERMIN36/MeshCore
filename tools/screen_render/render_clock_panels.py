"""Calibri clock adapted to the other e-ink panels.

250x122 is already rendered. This sheet covers the remaining sizes:
  * 296x128  Heltec Vision Master E290
  * 200x200  ThinkNode M5, ThinkNode M1, LilyGo T-Echo
  * 192x176  LilyGo T-Echo Lite (GxEPD2_122_T61, rotation 0)
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_DIR = r"C:\Windows\Fonts"
VIEW = 3
TIME = "21:48"
DATE = "Пн, 24.09"
PAPER = (224, 224, 214)
INK = (28, 30, 34)
BEZEL = (46, 48, 52)
STATES = [
    ("both", "Groups and Chat", [("Groups", 5), ("Chat", 3)]),
    ("groups", "Groups only", [("Groups", 5)]),
    ("chat", "Chat only", [("Chat", 3)]),
    ("none", "No unread", []),
]
PANELS = [
    ("wide", "Mesh Pocket / Paper / E213  250x122", 250, 122),
    ("e290", "Heltec E290  296x128", 296, 128),
    ("square", "M5 / M1 / T-Echo  200x200", 200, 200),
    ("techo_lite", "T-Echo Lite  192x176", 192, 176),
]


def load(size):
    return ImageFont.truetype(os.path.join(FONT_DIR, "calibri.ttf"), size)


def text_box(fnt, text):
    b = fnt.getbbox(text)
    return b[2] - b[0], b[3] - b[1], b


def fit(text, max_w, max_h):
    lo, hi, best = 8, 600, 8
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


def render(width, height, items):
    pw, ph = width * VIEW, height * VIEW
    img = Image.new("RGB", (pw, ph), PAPER)
    dr = ImageDraw.Draw(img)
    header_cap = max(14, min(22, height // 8)) * VIEW
    hsize = fit(DATE, pw - 12, header_cap)
    hfnt = load(hsize)
    hw, hh, hbox = text_box(hfnt, DATE)
    hy = 5 * VIEW - hbox[1]
    dr.text(((pw - hw) // 2 - hbox[0], hy), DATE, font=hfnt, fill=INK)
    header_h = hh + 8 * VIEW

    footer_h = 0
    if items:
        label = footer_text(items)
        # Keep the counter a caption, not a second clock.
        cap = max(16, min(28, height // 7)) * VIEW
        fsize = fit(label, pw - 16, cap)
        fnt = load(fsize)
        w, h, box = text_box(fnt, label)
        if w > pw - 12:
            # Narrow panels: one counter per line, still centered.
            lines = ["%s: %d" % pair for pair in items]
            fsize = min(fit(line, pw - 16, cap) for line in lines)
            fnt = load(fsize)
            gap = 4 * VIEW
            sizes = [text_box(fnt, line) for line in lines]
            block = sum(s[1] for s in sizes) + gap * (len(lines) - 1)
            y = ph - 8 * VIEW - block
            for line, (w, h, box) in zip(lines, sizes):
                dr.text(((pw - w) // 2 - box[0], y - box[1]), line, font=fnt, fill=INK)
                y += h + gap
            footer_h = block + 14 * VIEW
        else:
            x = (pw - w) // 2 - box[0]
            y = ph - 8 * VIEW - h - box[1]
            dr.text((x, y), label, font=fnt, fill=INK)
            footer_h = h + 14 * VIEW

    size = fit(TIME, pw - 12, ph - footer_h - header_h - 8 * VIEW)
    fnt = load(size)
    w, h, box = text_box(fnt, TIME)
    area_top = header_h
    area_h = ph - footer_h - header_h
    dr.text(((pw - w) // 2 - box[0], area_top + (area_h - h) // 2 - box[1]), TIME, font=fnt, fill=INK)
    return img


def with_bezel(face):
    pad = 4 * VIEW
    img = Image.new("RGB", (face.width + pad * 2, face.height + pad * 2), BEZEL)
    img.paste(face, (pad, pad))
    return img


def main():
    out = os.path.join(ROOT, "docs", "screens", "clock_panels")
    os.makedirs(out, exist_ok=True)
    title_font = load(28)
    label_font = load(22)
    rows = []
    for key, title, w, h in PANELS:
        cells = []
        for state, state_label, items in STATES:
            panel = with_bezel(render(w, h, items))
            panel.save(os.path.join(out, "%s_%s.png" % (key, state)))
            cells.append((state_label, panel))
        rows.append((title, cells))

    gap, cap, head = 28, 36, 72
    row_w = max(sum(im.width for _, im in cells) + gap * (len(cells) + 1) for _, cells in rows)
    height = gap
    for title, cells in rows:
        cell_h = max(im.height for _, im in cells)
        height += head + cap + cell_h + gap
    sheet = Image.new("RGB", (row_w, height), (32, 34, 40))
    dr = ImageDraw.Draw(sheet)
    y = gap
    for title, cells in rows:
        dr.text((gap, y), title, font=title_font, fill=(240, 240, 240))
        y += head
        x = gap
        for state_label, im in cells:
            dr.text((x, y), state_label, font=label_font, fill=(220, 220, 220))
            sheet.paste(im, (x, y + cap))
            x += im.width + gap
        y += cap + max(im.height for _, im in cells) + gap
    gallery = os.path.join(ROOT, "docs", "screens", "gallery_clock_panels.png")
    sheet.save(gallery)
    print(gallery)


if __name__ == "__main__":
    main()
