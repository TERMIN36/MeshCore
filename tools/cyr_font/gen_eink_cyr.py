"""Generates the CYR_EINK / CYR_EINK2 faces in src/helpers/ui/CyrillicGlyphs.inc.

GxEPDDisplay prints Latin with FreeSans9pt7b (text size 1) and FreeSansBold12pt7b
(size 2). The Cyrillic faces are rendered from a Helvetica-style TTF without
anti-aliasing, at the pixel size where the cap and x-heights match those GFX
fonts, so mixed Latin/Cyrillic lines share weight, height and baseline.
Glyph row BASE is the bottom row of a capital, like row 0 of a GFXfont glyph.

Usage:  python tools/cyr_font/gen_eink_cyr.py
"""

import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
INC = os.path.join(ROOT, "src", "helpers", "ui", "CyrillicGlyphs.inc")
FONT_DIRS = [r"C:\Windows\Fonts", "/usr/share/fonts/truetype/freefont",
             "/usr/share/fonts/truetype/liberation", "/usr/share/fonts/truetype/liberation2", ""]

# prefix, TTF candidates (best match first), cap height, x-height of the GFX font
FACES = [
    ("CYR_EINK", ["FreeSans.ttf", "LiberationSans-Regular.ttf", "arial.ttf"], 13, 10),
    ("CYR_EINK2", ["FreeSansBold.ttf", "LiberationSans-Bold.ttf", "arialbd.ttf"], 18, 13),
]

# Font index order: А-Я (0..31), Ё (32), а-я (33..64), ё (65).
ORDER = [chr(c) for c in range(0x410, 0x430)] + ["Ё"] + [chr(c) for c in range(0x430, 0x450)] + ["ё"]


def load_ttf(names):
    for name in names:
        for base in FONT_DIRS:
            path = os.path.join(base, name)
            if os.path.isfile(path):
                return path
    raise SystemExit("none of %s found" % ", ".join(names))


def render(font, ch):
    """Returns (advance, {(x, y)}) with y relative to the baseline (-1 = row above it)."""
    size = int(font.size * 3) + 8
    img = Image.new("1", (size * 2, size * 2), 0)
    dr = ImageDraw.Draw(img)
    dr.fontmode = "1"
    ox, oy = size // 2, size
    dr.text((ox, oy), ch, font=font, fill=1, anchor="ls")
    px = img.load()
    ink = {(x - ox, y - oy) for y in range(img.height) for x in range(img.width) if px[x, y]}
    return int(round(font.getlength(ch))), ink


def ink_height(font, ch):
    ys = [y for _, y in render(font, ch)[1]]
    return max(ys) - min(ys) + 1


def pick_font(path, cap, xh):
    best = None
    for tenth in range(80, 400):
        font = ImageFont.truetype(path, tenth / 10)
        err = abs(ink_height(font, "Н") - cap) * 4 + abs(ink_height(font, "н") - xh)
        if best is None or err < best[0]:
            best = (err, font)
        if err == 0:
            break
    return best[1]


def build(prefix, path, cap, xh):
    font = pick_font(path, cap, xh)
    glyphs = []
    for ch in ORDER:
        adv, ink = render(font, ch)
        left = min((x for x, _ in ink), default=0)
        shift = -left if left < 0 else 0
        right = max((x for x, _ in ink), default=-1) + shift
        glyphs.append((max(adv + shift, right + 1, 1), {(x + shift, y) for x, y in ink}))
    cap_bottom = max(y for _, y in render(font, "Н")[1])
    top = min(y for _, ink in glyphs for _, y in ink)
    bottom = max(y for _, ink in glyphs for _, y in ink)
    height, base = bottom - top + 1, cap_bottom - top
    advs, offs, bits = [], [], []
    for w, ink in glyphs:
        advs.append(w)
        offs.append(len(bits))
        rowb = (w + 7) >> 3
        for row in range(height):
            line = [0] * rowb
            for x in range(w):
                if (x, row + top) in ink:
                    line[x >> 3] |= 0x80 >> (x & 7)
            bits += line
    n = len(ORDER)
    print("%s: %s @ %.1fpx, H=%d BASE=%d, %d bytes" % (prefix, os.path.basename(path), font.size, height, base, len(bits)))
    return (
        "static const uint8_t %s_H = %d;\n" % (prefix, height)
        + "static const uint8_t %s_BASE = %d;\n" % (prefix, base)
        + "static const uint8_t %s_ADV[%d] PROGMEM = {\n%s\n};\n" % (prefix, n, hex_block(advs, "0x%02X", 33))
        + "static const uint16_t %s_OFF[%d] PROGMEM = {\n%s\n};\n" % (prefix, n, hex_block(offs, "0x%04X", 12))
        + "static const uint8_t %s_BITS[%d] PROGMEM = {\n%s\n};\n" % (prefix, len(bits), hex_block(bits, "0x%02X", 16))
    )


def hex_block(values, fmt, per_line):
    items = [fmt % v for v in values]
    return ",\n".join("  " + ", ".join(items[i:i + per_line]) for i in range(0, len(items), per_line))


def replace_block(text, prefix, block, nl):
    start = text.index("static const uint8_t %s_H" % prefix)
    end = text.index("%s_BITS[" % prefix, start)
    end = text.index("};", end) + 2
    if text.startswith(nl, end):
        end += len(nl)
    return text[:start] + block.replace("\n", nl) + text[end:]


def main():
    with open(INC, encoding="utf-8", newline="") as f:
        text = f.read()
    nl = "\r\n" if "\r\n" in text else "\n"
    for prefix, names, cap, xh in FACES:
        text = replace_block(text, prefix, build(prefix, load_ttf(names), cap, xh), nl)
    with open(INC, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print("wrote", INC)


if __name__ == "__main__":
    main()
