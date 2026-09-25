"""Softer clock faces for the 2.13\" e-ink panel.

Drawn with grayscale antialiasing at 3x so the curves read smoothly.
The panel itself is still 1-bit; this sheet is for choosing the shape.
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

FACES = [
    ("01_segoe_light", "Segoe UI Light", "segoeuil.ttf"),
    ("02_segoe_semilight", "Segoe UI Semilight", "segoeuisl.ttf"),
    ("03_segoe", "Segoe UI", "segoeui.ttf"),
    ("04_segoe_semibold", "Segoe UI Semibold", "seguisb.ttf"),
    ("05_calibri_light", "Calibri Light", "calibril.ttf"),
    ("06_calibri", "Calibri", "calibri.ttf"),
    ("07_calibri_bold", "Calibri Bold", "calibrib.ttf"),
    ("08_candara", "Candara", "Candara.ttf"),
    ("09_corbel", "Corbel", "corbel.ttf"),
    ("10_bahnschrift", "Bahnschrift", "bahnschrift.ttf"),
]


def load(name, size):
    return ImageFont.truetype(os.path.join(FONT_DIR, name), size)


def text_size(font, text):
    b = font.getbbox(text)
    return b[2] - b[0], b[3] - b[1], b


def fit_size(filename, text, max_w, max_h):
    lo, hi, best = 8, 500, 8
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
    pw, ph = W * VIEW, H * VIEW
    img = Image.new("RGB", (pw, ph), PAPER)
    dr = ImageDraw.Draw(img)
    size = fit_size(filename, TIME, pw - 36, ph - 48)
    font = load(filename, size)
    w, h, box = text_size(font, TIME)
    x = (pw - w) // 2 - box[0]
    y = (ph - h) // 2 - box[1]
    dr.text((x, y), TIME, font=font, fill=INK)
    return img


def with_bezel(face):
    pad = 4 * VIEW
    img = Image.new("RGB", (face.width + pad * 2, face.height + pad * 2), BEZEL)
    img.paste(face, (pad, pad))
    return img


def main():
    out = os.path.join(ROOT, "docs", "screens", "clock_fonts_smooth")
    os.makedirs(out, exist_ok=True)
    missing = [name for _, _, name in FACES if not os.path.exists(os.path.join(FONT_DIR, name))]
    if missing:
        raise SystemExit("missing fonts: " + ", ".join(missing))

    panels = []
    for fname, label, fontfile in FACES:
        panel = with_bezel(render_face(fontfile))
        panel.save(os.path.join(out, fname + ".png"))
        panels.append((label, panel))

    font = load("segoeui.ttf", 22)
    title_font = load("segoeui.ttf", 28)
    cols = 2
    cw, ch = panels[0][1].size
    cap, gap, top = 36, 28, 64
    rows = (len(panels) + cols - 1) // cols
    sheet = Image.new("RGB", (gap + cols * (cw + gap), top + gap + rows * (ch + cap + gap)), (32, 34, 40))
    dr = ImageDraw.Draw(sheet)
    dr.text((gap, 16), "Smoother clock fonts — e-ink 250x122,  21:48", font=title_font, fill=(240, 240, 240))
    for i, (label, im) in enumerate(panels):
        x = gap + (i % cols) * (cw + gap)
        y = top + gap + (i // cols) * (ch + cap + gap)
        dr.text((x, y), "%d.  %s" % (i + 1, label), font=font, fill=(220, 220, 220))
        sheet.paste(im, (x, y + cap))
    gallery = os.path.join(ROOT, "docs", "screens", "gallery_clock_fonts_smooth.png")
    sheet.save(gallery)
    print(gallery)


if __name__ == "__main__":
    main()
