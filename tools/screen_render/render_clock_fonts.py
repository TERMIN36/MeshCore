"""Ten 1-bit clock faces for the 2.13\" e-ink panel (250x122).

Same time on every screen so the typeface is the only difference.
Output: docs/screens/clock_fonts/ and gallery_clock_fonts.png
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_DIR = r"C:\Windows\Fonts"
W, H = 250, 122
SCALE = 4          # supersample, then threshold — closer to a bitmap font
VIEW = 3           # nearest-neighbor zoom in the gallery
TIME = "21:48"
PAPER = (224, 224, 214)
INK = (28, 30, 34)
BEZEL = (46, 48, 52)

# Open fonts already on this machine (DejaVu, Liberation, Source Pro).
FACES = [
    ("01_dejavu_extralight", "DejaVu ExtraLight", "DejaVuSans-ExtraLight.ttf"),
    ("02_dejavu", "DejaVu Sans", "DejaVuSans.ttf"),
    ("03_dejavu_bold", "DejaVu Sans Bold", "DejaVuSans-Bold.ttf"),
    ("04_dejavu_cond_bold", "DejaVu Condensed Bold", "DejaVuSansCondensed-Bold.ttf"),
    ("05_dejavu_mono_bold", "DejaVu Mono Bold", "DejaVuSansMono-Bold.ttf"),
    ("06_liberation_narrow_bold", "Liberation Narrow Bold", "LiberationSansNarrow-Bold.ttf"),
    ("07_source_sans_bold", "Source Sans Bold", "SourceSansPro-Bold.ttf"),
    ("08_source_sans_black", "Source Sans Black", "SourceSansPro-Black.ttf"),
    ("09_source_serif_bold", "Source Serif Bold", "SourceSerifPro-Bold.ttf"),
    ("10_source_code_bold", "Source Code Bold", "SourceCodePro-Bold.ttf"),
]


def load(name, size):
    return ImageFont.truetype(os.path.join(FONT_DIR, name), size)


def text_size(font, text):
    b = font.getbbox(text)
    return b[2] - b[0], b[3] - b[1], b


def fit_size(filename, text, max_w, max_h):
    lo, hi = 8, 400
    best = 8
    while lo <= hi:
        mid = (lo + hi) // 2
        w, h, _ = text_size(load(filename, mid), text)
        if w <= max_w and h <= max_h:
            best = mid
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def render_face(filename):
    """Native 250x122, ink = 1. Margins keep the glyphs off the bezel."""
    px = SCALE
    big = Image.new("L", (W * px, H * px), 255)
    dr = ImageDraw.Draw(big)
    size = fit_size(filename, TIME, (W - 12) * px, (H - 16) * px)
    font = load(filename, size)
    w, h, box = text_size(font, TIME)
    x = (W * px - w) // 2 - box[0]
    y = (H * px - h) // 2 - box[1]
    dr.text((x, y), TIME, font=font, fill=0)
    small = big.resize((W, H), Image.Resampling.BOX)
    # Drop the gray fringe; keep stems that are mostly ink.
    return small.point(lambda p: 0 if p < 170 else 1, mode="1")


def to_panel(bw):
    s, pad = VIEW, 4 * VIEW
    img = Image.new("RGB", (W * s + pad * 2, H * s + pad * 2), BEZEL)
    dr = ImageDraw.Draw(img)
    dr.rectangle([pad, pad, pad + W * s - 1, pad + H * s - 1], fill=PAPER)
    pix = bw.load()
    for y in range(H):
        for x in range(W):
            if pix[x, y] == 0:
                x0, y0 = pad + x * s, pad + y * s
                dr.rectangle([x0, y0, x0 + s - 1, y0 + s - 1], fill=INK)
    return img


def caption_font(size):
    return ImageFont.truetype(os.path.join(FONT_DIR, "segoeui.ttf"), size)


def main():
    out = os.path.join(ROOT, "docs", "screens", "clock_fonts")
    os.makedirs(out, exist_ok=True)
    panels = []
    for fname, label, fontfile in FACES:
        panel = to_panel(render_face(fontfile))
        panel.save(os.path.join(out, fname + ".png"))
        panels.append((label, panel))

    font, title_font = caption_font(22), caption_font(28)
    cols = 2
    cw, ch = panels[0][1].size
    cap, gap, top = 36, 28, 64
    rows = (len(panels) + cols - 1) // cols
    sheet = Image.new("RGB", (gap + cols * (cw + gap), top + gap + rows * (ch + cap + gap)), (32, 34, 40))
    dr = ImageDraw.Draw(sheet)
    dr.text((gap, 16), "Clock fonts — e-ink 250x122,  21:48", font=title_font, fill=(240, 240, 240))
    for i, (label, im) in enumerate(panels):
        x = gap + (i % cols) * (cw + gap)
        y = top + gap + (i // cols) * (ch + cap + gap)
        dr.text((x, y), "%d.  %s" % (i + 1, label), font=font, fill=(220, 220, 220))
        sheet.paste(im, (x, y + cap))
    gallery = os.path.join(ROOT, "docs", "screens", "gallery_clock_fonts.png")
    sheet.save(gallery)
    print(gallery)


if __name__ == "__main__":
    main()
