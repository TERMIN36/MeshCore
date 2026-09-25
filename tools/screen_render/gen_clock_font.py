"""Rasterize the clock glyphs from Calibri into a 1-bit header.

Faces: short digits (122-128px panels), tall digits (176-200px panels),
and a footer small enough for "Groups: 5    Chat: 3" on a 192px row.
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT = r"C:\Windows\Fonts\calibri.ttf"
CHARS = "0123456789:- GroupsChat"
OUT = os.path.join(ROOT, "src", "helpers", "ui", "ClockFont.h")


def load(size):
    return ImageFont.truetype(FONT, size)


def ink_box(font, ch):
    probe = Image.new("L", (font.size * 4, font.size * 4), 255)
    d = ImageDraw.Draw(probe)
    d.text((font.size, font.size), ch, font=font, fill=0)
    return probe.point(lambda p: 0 if p < 170 else 255).getbbox()


def fit_footer():
    text = "Groups: 5    Chat: 3"
    lo, hi, best = 8, 40, 8
    while lo <= hi:
        mid = (lo + hi) // 2
        font = load(mid)
        b = font.getbbox(text)
        if b[2] - b[0] <= 180 and b[3] - b[1] <= 22:
            best = mid
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def raster(font, ch):
    """Tight ink bitmap, plus advance and the top of the ink relative to the font top."""
    if ch == " ":
        gap = max(font.size // 3, 2)
        return bytearray(), 0, 0, gap, 0
    scale = 4
    big = load(font.size * scale)
    canvas = Image.new("L", (big.size * 3, big.size * 3), 255)
    dr = ImageDraw.Draw(canvas)
    ox = oy = big.size // 2
    dr.text((ox, oy), ch, font=big, fill=0)
    small = canvas.resize((canvas.width // scale, canvas.height // scale), Image.Resampling.BOX)
    bw = small.point(lambda p: 0 if p < 180 else 255)
    pix = bw.load()
    x0 = y0 = x1 = y1 = None
    for y in range(bw.height):
        for x in range(bw.width):
            if pix[x, y] == 0:
                if x0 is None:
                    x0 = x1 = x
                    y0 = y1 = y
                else:
                    x0 = min(x0, x)
                    y0 = min(y0, y)
                    x1 = max(x1, x)
                    y1 = max(y1, y)
    if x0 is None:
        return bytearray(), 0, 0, max(font.size // 3, 2), 0
    x1 += 1
    y1 += 1
    w, h = x1 - x0, y1 - y0
    bits = bytearray()
    rowb = (w + 7) // 8
    pix = bw.load()
    for y in range(y0, y1):
        for col in range(rowb):
            byte = 0
            for b in range(8):
                x = x0 + col * 8 + b
                if x < x1 and pix[x, y] == 0:
                    byte |= 0x80 >> b
            bits.append(byte)
    advance = font.getlength(ch)
    advance = max(int(round(advance)), w)
    top = y0 - oy // scale
    return bits, w, h, advance, top


def emit_chars(name, font, chars):
    glyphs = []
    blob = bytearray()
    for ch in chars:
        bits, w, h, adv, top = raster(font, ch)
        glyphs.append((ch, w, h, adv, top, len(blob), bits))
        blob.extend(bits)
    lines = []
    lines.append("static const uint8_t %s_BITS[] PROGMEM = {" % name)
    for i in range(0, len(blob), 16):
        chunk = ", ".join("0x%02x" % b for b in blob[i:i + 16])
        lines.append("  " + chunk + ",")
    lines.append("};")
    lines.append("static const ClockGlyph %s_GLYPHS[] PROGMEM = {" % name)
    for ch, w, h, adv, top, off, _bits in glyphs:
        if ord(ch) < 128 and ch != "'":
            key = "'%s'" % (" " if ch == " " else ch)
        else:
            key = "0x%04X" % ord(ch)
        lines.append("  { %s, %d, %d, %d, %d, %d }," % (key, w, h, adv, top, off))
    lines.append("};")
    lines.append("static const ClockFace %s = { %d, %s_GLYPHS, %s_BITS };" % (
        name, len(glyphs), name, name))
    return "\n".join(lines), font.size


def emit_face(name, font):
    return emit_chars(name, font, CHARS)


def fit_digits(max_w, max_h):
    lo, hi, best = 8, 220, 8
    while lo <= hi:
        mid = (lo + hi) // 2
        font = load(mid)
        b = font.getbbox("21:48")
        if b[2] - b[0] <= max_w and b[3] - b[1] <= max_h:
            best = mid
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def main():
    # Short wide panels (122-128 tall) are limited by height. Square and
    # 192x176 panels are limited by the width of "21:48".
    short = load(fit_digits(236, 86))
    tall = load(fit_digits(184, 150))
    footer = load(fit_footer())
    a, sa = emit_face("CLOCK_DIGIT_WIDE", short)
    b, sb = emit_face("CLOCK_DIGIT_NARROW", tall)
    c, sc = emit_face("CLOCK_FOOTER", footer)
    text = """#pragma once

#include <Arduino.h>
#include <stdint.h>

// 1-bit Calibri rasters for the e-ink clock. Glyphs are tight ink boxes.
// top is the ink top relative to the draw origin used by the generator.

struct ClockGlyph {
  char ch;
  uint8_t w;
  uint8_t h;
  uint8_t advance;
  int8_t top;
  uint16_t offset;
};

struct ClockFace {
  uint8_t count;
  const ClockGlyph* glyphs;
  const uint8_t* bits;
};

%s

%s

%s
""" % (a, b, c)
    with open(OUT, "w", newline="\n") as f:
        f.write(text)
    print("short", sa, "tall", sb, "footer", sc, "bytes", os.path.getsize(OUT))


if __name__ == "__main__":
    main()
