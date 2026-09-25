"""Flip-clock faces for the 2.13\" e-ink panel (250x122).

Each digit sits on its own flap with a hinge through the middle.
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_DIR = r"C:\Windows\Fonts"
W, H = 250, 122
VIEW = 3
TIME = "2148"
PAPER = (224, 224, 214)
INK = (28, 30, 34)
IVORY = (236, 232, 220)
CARD = (32, 34, 38)
HOUSING = (22, 22, 24)
BEZEL = (46, 48, 52)
FONT_FILE = "seguisb.ttf"

# name, label, housing, card, digit, radius, gap (0 = hairline hinge)
VARIANTS = [
    ("01_ivory_line", "Ivory, hairline", PAPER, IVORY, INK, 2, 0),
    ("02_ivory_gap", "Ivory, gap", PAPER, IVORY, INK, 2, 4),
    ("03_ivory_dark", "Ivory on black", HOUSING, IVORY, INK, 2, 3),
    ("04_black_line", "Black, hairline", PAPER, CARD, IVORY, 2, 0),
    ("05_black_gap", "Black, gap", PAPER, CARD, IVORY, 2, 4),
    ("06_black_round", "Black, rounded", HOUSING, CARD, IVORY, 8, 4),
    ("07_ivory_round", "Ivory, rounded", HOUSING, IVORY, INK, 8, 4),
    ("08_cut_digits", "Cut digits", PAPER, None, INK, 0, 5),
]


def font(size):
    return ImageFont.truetype(os.path.join(FONT_DIR, FONT_FILE), size)


def text_box(fnt, text):
    b = fnt.getbbox(text)
    return b[2] - b[0], b[3] - b[1], b


def fit_digit(max_w, max_h):
    lo, hi, best = 8, 400, 8
    while lo <= hi:
        mid = (lo + hi) // 2
        w, h, _ = text_box(font(mid), "8")
        if w <= max_w and h <= max_h:
            best = mid
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def render(housing, card, digit, radius, gap):
    pw, ph = W * VIEW, H * VIEW
    img = Image.new("RGB", (pw, ph), housing)
    dr = ImageDraw.Draw(img)

    margin_x, margin_y = 8 * VIEW, 10 * VIEW
    colon_w = 14 * VIEW
    between = 5 * VIEW
    card_w = (pw - margin_x * 2 - between * 3 - colon_w) // 4
    card_h = ph - margin_y * 2
    y0 = (ph - card_h) // 2
    hinge = max(VIEW, gap * VIEW) if gap else max(2, VIEW // 2)

    size = fit_digit(card_w - 8 * VIEW, card_h - 10 * VIEW)
    fnt = font(size)

    def card_x(i):
        shift = colon_w + between if i >= 2 else 0
        return margin_x + i * (card_w + between) + shift

    for i, ch in enumerate(TIME):
        x = card_x(i)
        glyph = Image.new("RGBA", (card_w, card_h), (0, 0, 0, 0))
        gd = ImageDraw.Draw(glyph)
        if card is not None:
            gd.rounded_rectangle([0, 0, card_w - 1, card_h - 1], radius * VIEW, fill=card + (255,))
        w, h, box = text_box(fnt, ch)
        gd.text(((card_w - w) // 2 - box[0], (card_h - h) // 2 - box[1]), ch, font=fnt, fill=digit + (255,))
        # Cut the hinge out of both the card and the digit.
        mid = card_h // 2
        gd.rectangle([0, mid - hinge // 2, card_w, mid + (hinge - hinge // 2)], fill=(0, 0, 0, 0))
        img.paste(glyph, (x, y0), glyph)

    # Colon sits in the hinge gap between the hour and minute pairs.
    cx = card_x(1) + card_w + between + colon_w // 2
    cy = ph // 2
    dot = max(3 * VIEW, 6)
    span = card_h // 5
    for dy in (-span, span):
        dr.ellipse([cx - dot, cy + dy - dot, cx + dot, cy + dy + dot], fill=digit)

    return img


def with_bezel(face):
    pad = 4 * VIEW
    img = Image.new("RGB", (face.width + pad * 2, face.height + pad * 2), BEZEL)
    img.paste(face, (pad, pad))
    return img


def main():
    out = os.path.join(ROOT, "docs", "screens", "clock_flip")
    os.makedirs(out, exist_ok=True)
    label_font = font(22)
    title_font = font(28)
    panels = []
    for fname, label, housing, card, digit, radius, gap in VARIANTS:
        panel = with_bezel(render(housing, card, digit, radius, gap))
        panel.save(os.path.join(out, fname + ".png"))
        panels.append((label, panel))

    cols = 2
    cw, ch = panels[0][1].size
    cap, gap_px, top = 36, 28, 64
    rows = (len(panels) + cols - 1) // cols
    sheet = Image.new("RGB", (gap_px + cols * (cw + gap_px), top + gap_px + rows * (ch + cap + gap_px)), (32, 34, 40))
    dr = ImageDraw.Draw(sheet)
    dr.text((gap_px, 16), "Flip clock — e-ink 250x122,  21:48", font=title_font, fill=(240, 240, 240))
    for i, (label, im) in enumerate(panels):
        x = gap_px + (i % cols) * (cw + gap_px)
        y = top + gap_px + (i // cols) * (ch + cap + gap_px)
        dr.text((x, y), "%d.  %s" % (i + 1, label), font=label_font, fill=(220, 220, 220))
        sheet.paste(im, (x, y + cap))
    gallery = os.path.join(ROOT, "docs", "screens", "gallery_clock_flip.png")
    sheet.save(gallery)
    print(gallery)


if __name__ == "__main__":
    main()
