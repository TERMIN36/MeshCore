"""Offline renders of the companion / repeater UI.

Two targets:
  * 128x64 SSD1306 OLED (Heltec V3/V4 and alike), mirrors SSD1306Display;
  * Heltec MeshPocket 2.13" e-ink (GxEPD2_213_B74, 250x122), mirrors GxEPDDisplay
    with the EINK_SCALE_* / EINK_*_OFFSET values from variants/mesh_pocket.

Fonts and icons are parsed from the firmware sources and the Adafruit GFX library,
and the page layouts follow examples/*/UITask.cpp, so the output matches the device
pixel for pixel. Page content is sample data.

Usage:  python tools/screen_render/render_oled.py [out_dir]
"""

import glob
import math
import os
import re
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
W, H = 128, 64
BLACK, WHITE = 0, 1


def parse_c(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    arrays = {}
    for m in re.finditer(r"(\w+)\s*\[[^\]]*\]\s*(?:PROGMEM)?\s*=\s*\{(.*?)\};", text, re.S):
        arrays[m.group(1)] = [int(v, 0) for v in re.findall(r"0[xX][0-9a-fA-F]+|\d+", m.group(2))]
    scalars = {m.group(1): int(m.group(2), 0)
               for m in re.finditer(r"\b(\w+)\s*=\s*(0[xX][0-9a-fA-F]+|\d+)\s*;", text)}
    return arrays, scalars


def find_gfx_file(rel):
    hits = glob.glob(os.path.join(ROOT, ".pio", "libdeps", "*", "Adafruit GFX Library", rel))
    if not hits:
        sys.exit(rel + " not found: build any display env once so PlatformIO fetches Adafruit GFX")
    return hits[0]


FONT = parse_c(find_gfx_file("glcdfont.c"))[0]["font"]


class CyrFont:
    def __init__(self, arrays, scalars, prefix):
        self.h = scalars[prefix + "_H"]
        self.base = scalars[prefix + "_BASE"]
        self.adv = arrays[prefix + "_ADV"]
        self.off = arrays[prefix + "_OFF"]
        self.bits = arrays[prefix + "_BITS"]

    def advance(self, idx):
        return self.adv[idx] if idx >= 0 else (self.h * 2) // 3

    def pixel(self, idx, x, y):
        w = self.adv[idx]
        if x >= w or y >= self.h: return False
        rowb = (w + 7) >> 3
        return bool(self.bits[self.off[idx] + y * rowb + (x >> 3)] & (0x80 >> (x & 7)))


_cyr = parse_c(os.path.join(ROOT, "src", "helpers", "ui", "CyrillicGlyphs.inc"))
CYR_OLED = CyrFont(*_cyr, "CYR_OLED")
CYR_EINK = CyrFont(*_cyr, "CYR_EINK")
CYR_EINK2 = CyrFont(*_cyr, "CYR_EINK2")
ICONS = parse_c(os.path.join(ROOT, "examples", "companion_radio", "ui-new", "icons.h"))[0]
with open(os.path.join(ROOT, "src", "helpers", "FirmwareVersion.h"), encoding="utf-8") as _f:
    _ver = dict(re.findall(r'#define\s+(MESHCORE_BASE_VERSION|FORK_VERSION)\s+"([^"]*)"', _f.read()))
VERSION = "%s-%s" % (_ver["MESHCORE_BASE_VERSION"], _ver["FORK_VERSION"])


def load_gfx_font(name):
    with open(find_gfx_file(os.path.join("Fonts", name + ".h")), encoding="utf-8", errors="replace") as f:
        text = re.sub(r"//[^\n]*", "", f.read())
    bm = re.search(name + r"Bitmaps\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", text, re.S).group(1)
    gl = re.search(name + r"Glyphs\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", text, re.S).group(1)
    tail = re.search(r"GFXfont\s+" + name + r"\s+PROGMEM\s*=\s*\{[^,]*,[^,]*,\s*(\w+),\s*(\w+),\s*(\w+)\s*\}", text)
    return {
        "bits": [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]+", bm)],
        "glyphs": [tuple(int(v) for v in g) for g in re.findall(r"\{\s*" + r",\s*".join([r"(-?\d+)"] * 6) + r"\s*\}", gl)],
        "first": int(tail.group(1), 0), "last": int(tail.group(2), 0), "yadv": int(tail.group(3), 0),
    }


def cyr_index(cp):
    if 0x0410 <= cp <= 0x042F: return cp - 0x0410
    if cp == 0x0401: return 32
    if 0x0430 <= cp <= 0x044F: return 33 + cp - 0x0430
    if cp == 0x0451: return 65
    return -1


def utf8_next(b, i):
    c = b[i]
    if c < 0x80: return c, i + 1
    cont = lambda k: i + k < len(b) and (b[i + k] & 0xC0) == 0x80
    if (c & 0xE0) == 0xC0 and cont(1):
        return ((c & 0x1F) << 6) | (b[i + 1] & 0x3F), i + 2
    if (c & 0xF0) == 0xE0 and cont(1) and cont(2):
        return ((c & 0x0F) << 12) | ((b[i + 1] & 0x3F) << 6) | (b[i + 2] & 0x3F), i + 3
    if (c & 0xF8) == 0xF0 and cont(1) and cont(2) and cont(3):
        return (((c & 0x07) << 18) | ((b[i + 1] & 0x3F) << 12) |
                ((b[i + 2] & 0x3F) << 6) | (b[i + 3] & 0x3F)), i + 4
    return c, i + 1


def B(s):
    return s.encode("utf-8") if isinstance(s, str) else bytes(s)


# Home pages compiled in for each target (ui-new HomePage enum).
PAGES = ["FIRST", "RECENT", "NEIGHBORS", "RADIO", "POWER", "SCAN", "FSCAN",
         "BLUETOOTH", "GPS", "BEACON", "SHUTDOWN"]
PAGES_NO_GPS = [p for p in PAGES if p not in ("GPS", "BEACON")]
UPSTREAM_PAGES = ["FIRST", "RECENT", "RADIO", "BLUETOOTH", "GPS", "SHUTDOWN"]


class Driver:
    """DisplayDriver helpers shared by every target."""
    legacy = False
    pages = PAGES

    def drawTextCentered(self, mid_x, y, s):
        self.setCursor(mid_x - self.getTextWidth(s) // 2, y); self.print(s)

    def drawTextRightAlign(self, x, y, s):
        self.setCursor(x - self.getTextWidth(s), y); self.print(s)

    def drawTextLeftAlign(self, x, y, s):
        self.setCursor(x, y); self.print(s)

    def drawTextEllipsized(self, x, y, max_width, s):
        t = B(s)[:255]
        if self.getTextWidth(t) <= max_width:
            self.setCursor(x, y); self.print(t); return
        ell = b"... " if self.getTextWidth(b"i") != self.getTextWidth(b"l") else b"..."
        ew, n = self.getTextWidth(ell), len(t)
        while n > 0 and self.getTextWidth(t[:n]) > max_width - ew:
            n -= 1
            if not self.legacy:
                while n > 0 and (t[n] & 0xC0) == 0x80: n -= 1
        self.setCursor(x, y); self.print(t[:n] + ell)

    def translate(self, s, dest_size=256):
        b, out = B(s), bytearray()
        if self.legacy:
            i = 0
            while i < len(b) and len(out) < dest_size - 1:
                c = b[i]
                if 32 <= c <= 126: out.append(c)
                elif c >= 0x80:
                    out.append(0xDB)
                    while i + 1 < len(b) and (b[i + 1] & 0xC0) == 0x80: i += 1
                i += 1
            return bytes(out)
        i = 0
        while i < len(b) and len(out) + 1 < dest_size:
            start = i
            cp, i = utf8_next(b, i)
            if 32 <= cp <= 126: out.append(cp)
            elif cyr_index(cp) >= 0:
                if len(out) + (i - start) >= dest_size: break
                out += b[start:i]
            else: out.append(32)
        return re.sub(rb" {2,}", b" ", bytes(out)).strip(b" ")

    def alert(self, text):
        self.setTextSize(1)
        w, h = self.width(), self.height()
        y, p = h // 3, h // 32
        self.setColor(BLACK); self.fillRect(p, y, w - p * 2, y)
        self.setColor(WHITE); self.drawRect(p, y, w - p * 2, y)
        self.drawTextCentered(w // 2, y + p * 3, text)


class Oled(Driver):
    """SSD1306Display on top of Adafruit_SSD1306. legacy=True mimics upstream (no Cyrillic)."""

    def __init__(self, legacy=False):
        self.legacy = legacy
        self.clear()
        self.color = WHITE
        self.ts = 1
        self.cx = self.cy = 0

    def width(self): return W
    def height(self): return H
    def clear(self): self.fb = [[0] * W for _ in range(H)]
    def setColor(self, c): self.color = c
    def setTextSize(self, s): self.ts = max(1, s)
    def setCursor(self, x, y): self.cx, self.cy = x, y

    def fillRect(self, x, y, w, h):
        for yy in range(max(0, y), min(H, y + h)):
            for xx in range(max(0, x), min(W, x + w)):
                self.fb[yy][xx] = self.color

    def drawRect(self, x, y, w, h):
        if w <= 0 or h <= 0: return
        self.fillRect(x, y, w, 1); self.fillRect(x, y + h - 1, w, 1)
        self.fillRect(x, y, 1, h); self.fillRect(x + w - 1, y, 1, h)

    def drawXbm(self, x, y, bits, w, h):
        stride = (w + 7) // 8
        for j in range(h):
            for i in range(w):
                if bits[j * stride + i // 8] & (0x80 >> (i & 7)):
                    self.fillRect(x + i, y + j, 1, 1)

    def _char(self, x, y, c):
        s = self.ts
        for i in range(5):
            line = FONT[c * 5 + i]
            for j in range(8):
                if line & 1: self.fillRect(x + i * s, y + j * s, s, s)
                line >>= 1

    def _gfx_write(self, x, y, c):
        s = self.ts
        if x + 6 * s > W:  # Adafruit GFX text wrap
            x, y = 0, y + 8 * s
        self._char(x, y, c)
        return x, y

    def _cyr_glyph(self, idx, x, y):
        s, f = self.ts, CYR_OLED
        w = f.advance(idx)
        for row in range(f.h):
            for col in range(w):
                on = f.pixel(idx, col, row) if idx >= 0 else (
                    row == 0 or row == f.h - 1 or col == 0 or col == w - 1)
                if on: self.fillRect(x + col * s, y + row * s, s, s)

    def _span(self, b):
        s = self.ts
        if self.legacy:
            for c in b:
                if c == 10: self.cx, self.cy = 0, self.cy + 8 * s
                elif c != 13:
                    self.cx, self.cy = self._gfx_write(self.cx, self.cy, c)
                    self.cx += 6 * s
            return
        dy = (6 - CYR_OLED.base) * s
        i = 0
        while i < len(b) and b[i]:
            cp, i = utf8_next(b, i)
            if cp == 10:
                self.cx, self.cy = 0, self.cy + (CYR_OLED.h + 1) * s
            elif cp < 0x80:
                self._gfx_write(self.cx, self.cy, cp)
                self.cx += 6 * s
            else:
                idx = cyr_index(cp)
                self._cyr_glyph(idx, self.cx, self.cy + dy)
                self.cx += CYR_OLED.advance(idx) * s

    def print(self, s): self._span(B(s))

    def _gfx_width(self, b):
        s, x, maxx = self.ts, 0, -1
        for c in b:
            if c == 10: x = 0; continue
            if c == 13: continue
            if x + 6 * s > W: x = 0
            maxx = max(maxx, x + 6 * s - 1)
            x += 6 * s
        return maxx + 1

    def getTextWidth(self, s):
        b = B(s)
        if self.legacy or all(c < 0x80 for c in b):
            return self._gfx_width(b)
        w, i = 0, 0
        while i < len(b):
            cp, i = utf8_next(b, i)
            if cp == 10: continue
            idx = cyr_index(cp)
            w += (CYR_OLED.adv[idx] if idx >= 0 else 6) * self.ts
        return w

    def printWordWrap(self, s, max_width):
        """Returns the byte offset of the first char that did not fit."""
        b = B(s)
        if self.legacy:
            self.print(b)
            return len(b)
        origin, sc = self.cx, self.ts
        max_width = min(max_width, max(1, W - origin))
        step = (CYR_OLED.h + 1) * sc
        p = 0
        while p < len(b) and self.cy < H:
            line, width, brk, end = p, 0, None, p
            while end < len(b) and b[end] != 10:
                cp, nxt = utf8_next(b, end)
                idx = cyr_index(cp)
                cw = (CYR_OLED.adv[idx] if idx >= 0 else 6) * sc
                if width + cw > max_width and end != line: break
                width += cw
                if cp in (32, 45, 47): brk = nxt
                end = nxt
            cut = end
            if end < len(b) and b[end] != 10 and brk and brk > line: cut = brk
            self._span(b[line:cut])
            if cut < len(b) and b[cut] in (10, 32): cut += 1
            if cut == p: break
            p = cut
            if p < len(b):
                if self.cy + step + 8 * sc > H: break
                self.cx, self.cy = origin, self.cy + step
        return p


# ------------------------------------------------------------ MeshPocket e-ink

def f32(v):
    return struct.unpack("f", struct.pack("f", v))[0]


def fmul(a, b):
    return f32(f32(a) * f32(b))


def pio_defines(variant):
    with open(os.path.join(ROOT, "variants", variant, "platformio.ini"), encoding="utf-8") as f:
        text = f.read()
    return dict(re.findall(r"^\s*-D\s+(\w+)=([^\s;]+)", text, re.M))


_pocket = pio_defines("mesh_pocket")
EINK_W, EINK_H = 250, 122          # GxEPD2_213_B74 visible area after setRotation(3)
EINK_SX = f32(float(_pocket["EINK_SCALE_X"].rstrip("f")))
EINK_SY = f32(float(_pocket["EINK_SCALE_Y"].rstrip("f")))
EINK_OX = float(_pocket["EINK_X_OFFSET"])
EINK_OY = float(_pocket["EINK_Y_OFFSET"])
EINK_LOGICAL_H = int(_pocket.get("EINK_LOGICAL_HEIGHT", 128))
_GFX = {}


def gfx_font(ts):
    name = {2: "FreeSansBold12pt7b", 3: "FreeSans18pt7b"}.get(ts, "FreeSans9pt7b")
    if name not in _GFX: _GFX[name] = load_gfx_font(name)
    return _GFX[name]


class Eink(Driver):
    """GxEPDDisplay on top of GxEPD2_BW: the logical grid is scaled onto the physical panel.
    fb holds physical pixels, 1 = ink (the OLED 'lit' colour maps to GxEPD_BLACK)."""
    pages = PAGES_NO_GPS

    def __init__(self):
        self.clear()
        self.color = WHITE
        self.ts = 1
        self.lx = self.ly = 0
        self.px = self.py = 0
        self.wrap = True

    def width(self): return 128
    def height(self): return EINK_LOGICAL_H
    def clear(self): self.fb = [[0] * EINK_W for _ in range(EINK_H)]
    def setColor(self, c): self.color = c
    def setTextSize(self, s): self.ts = max(1, s)

    @property
    def font(self): return gfx_font(self.ts)

    @property
    def cyr(self): return CYR_EINK2 if self.ts >= 2 else CYR_EINK

    def _baseline(self, y):
        cap = lambda f: -f["glyphs"][ord("H") - f["first"]][5]
        return int(fmul(y + EINK_OY, EINK_SY)) + cap(self.font) - cap(gfx_font(1))

    def setCursor(self, x, y):
        self.lx, self.ly = x, y
        self.px, self.py = int(fmul(x + EINK_OX, EINK_SX)), self._baseline(y)

    def _fill(self, x, y, w, h):
        for yy in range(max(0, y), min(EINK_H, y + h)):
            row = self.fb[yy]
            for xx in range(max(0, x), min(EINK_W, x + w)):
                row[xx] = self.color

    def fillRect(self, x, y, w, h):
        self._fill(int(fmul(x, EINK_SX)), int(fmul(y, EINK_SY)), int(fmul(w, EINK_SX)), int(fmul(h, EINK_SY)))

    def drawRect(self, x, y, w, h):
        x, y = int(fmul(x, EINK_SX)), int(fmul(y, EINK_SY))
        w, h = int(fmul(w, EINK_SX)), int(fmul(h, EINK_SY))
        if w <= 0 or h <= 0: return
        self._fill(x, y, w, 1); self._fill(x, y + h - 1, w, 1)
        self._fill(x, y, 1, h); self._fill(x + w - 1, y, 1, h)

    def drawXbm(self, x, y, bits, w, h):
        sx, sy = int(fmul(x, EINK_SX)), int(fmul(y, EINK_SY))
        stride = (w + 7) // 8
        for by in range(h):
            y1, y2 = sy + int(fmul(by, EINK_SY)), sy + int(fmul(by + 1, EINK_SY))
            for bx in range(w):
                if bits[by * stride + bx // 8] & (0x80 >> (bx & 7)):
                    x1, x2 = sx + int(fmul(bx, EINK_SX)), sx + int(fmul(bx + 1, EINK_SX))
                    self._fill(x1, y1, x2 - x1, y2 - y1)

    def _glyph(self, c):
        f = self.font
        return f["glyphs"][c - f["first"]] if f["first"] <= c <= f["last"] else None

    def _write(self, c):
        """Adafruit_GFX::write() with a GFXfont."""
        f = self.font
        if c == 10: self.px, self.py = 0, self.py + f["yadv"]; return
        if c == 13: return
        g = self._glyph(c)
        if not g: return
        off, gw, gh, xa, xo, yo = g
        if gw > 0 and gh > 0:
            if self.wrap and self.px + xo + gw > EINK_W:
                self.px, self.py = 0, self.py + f["yadv"]
            bits, bit, bo = f["bits"], 0, off
            for yy in range(gh):
                for xx in range(gw):
                    if not bit & 7:
                        byte = bits[bo]; bo += 1
                    if byte & 0x80: self._fill(self.px + xo + xx, self.py + yo + yy, 1, 1)
                    byte = (byte << 1) & 0xFF
                    bit += 1
        self.px += xa

    def _bounds_w(self, b):
        """Width reported by Adafruit_GFX::getTextBounds()."""
        x, minx, maxx = 0, 0x7FFF, -1
        for c in b:
            if c == 10: x = 0; continue
            if c == 13: continue
            g = self._glyph(c)
            if not g: continue
            off, gw, gh, xa, xo, yo = g
            if self.wrap and x + xo + gw > EINK_W: x = 0
            minx, maxx = min(minx, x + xo), max(maxx, x + xo + gw - 1)
            x += xa
        return maxx - minx + 1 if maxx >= minx else 0

    def _cyr_glyph(self, idx, x, y):
        f = self.cyr
        w = max(1, f.advance(idx))
        top = y - f.base
        for row in range(f.h):
            for col in range(w):
                on = f.pixel(idx, col, row) if idx >= 0 else (
                    row == 0 or row == f.h - 1 or col == 0 or col == w - 1)
                if on: self._fill(x + col, top + row, 1, 1)

    def _span(self, b):
        i = 0
        while i < len(b) and b[i]:
            if b[i] < 0x80:
                self._write(b[i]); i += 1
            else:
                cp, i = utf8_next(b, i)
                idx = cyr_index(cp)
                self._cyr_glyph(idx, self.px, self.py)
                self.px += self.cyr.advance(idx)

    def print(self, s): self._span(B(s))

    def getTextWidth(self, s):
        b = B(s)
        if all(c < 0x80 for c in b):
            return int(math.ceil(f32((self._bounds_w(b) + 1) / EINK_SX)))
        px, i = 0, 0
        while i < len(b):
            cp, i = utf8_next(b, i)
            px += self._cp_width(cp)
        return int(math.ceil(f32((px + 1) / EINK_SX)))

    def _cp_width(self, cp):
        if cp < 32 or cp == 10: return 0
        idx = cyr_index(cp)
        if idx >= 0: return self.cyr.adv[idx]
        if cp < 0x80:
            g = self._glyph(cp)
            return g[3] if g else 0
        return self.cyr.advance(-1)

    def _line_step(self):
        phys = max(self.font["yadv"], self.cyr.h + 2, 1)
        step = int(f32(phys / EINK_SY))
        if fmul(step, EINK_SY) < f32(phys - f32(0.01)): step += 1
        return max(1, step)

    def printWordWrap(self, s, max_width):
        b = B(s)
        origin, y = self.lx, self.ly
        limit = max(1, EINK_W - int(fmul(origin + EINK_OX, EINK_SX)))
        max_px = max(1, min(limit, int(f32(fmul(max_width, EINK_SX) + 0.5))))
        step = self._line_step()
        descent = max(0, self.cyr.h - self.cyr.base)
        self.wrap = False
        first, p = True, 0
        while p < len(b):
            baseline = self._baseline(y)
            if not first and (baseline < 0 or baseline + descent >= EINK_H): break
            line, width, brk, end = p, 0, None, p
            while end < len(b) and b[end] != 10:
                cp, nxt = utf8_next(b, end)
                cw = self._cp_width(cp)
                if width + cw > max_px and end != line: break
                width += cw
                if cp in (32, 45, 47): brk = nxt
                end = nxt
            cut = end
            if end < len(b) and b[end] != 10 and brk and brk > line: cut = brk
            self.setCursor(origin, y)
            self._span(b[line:cut])
            first = False
            if cut < len(b) and b[cut] in (10, 32): cut += 1
            if cut == p: break
            p = cut
            if p < len(b): y += step
        self.wrap = True
        self.lx, self.ly = origin, y
        return p


# ---------------------------------------------------------------- sample data

NODE_NAME = "Termin36"
BATT_MV = 3950
PRESS_LABEL = "long press"


def battery(d, mv, x_off=5):
    pct = max(0, min(100, ((mv - 3000) * 100) // 1200))
    ix, iw, ih = d.width() - 24 - x_off, 24, 10
    d.setColor(WHITE)
    d.drawRect(ix, 0, iw, ih)
    d.fillRect(ix + iw, ih // 4, 3, ih // 2)
    d.fillRect(ix + 2, 2, (pct * (iw - 4)) // 100, ih - 4)


def home_header(d, page, gps_on=True):
    w = d.width()
    d.setColor(BLACK); d.fillRect(0, 0, w, 12)
    d.setTextSize(1); d.setColor(WHITE)
    d.setCursor(0, 2); d.print(d.translate(NODE_NAME, 32))
    battery(d, BATT_MV)
    visible = UPSTREAM_PAGES if d.legacy else [p for p in d.pages if gps_on or p != "BEACON"]
    idx = visible.index(page)
    x = w // 2 - 5 * (len(visible) - 1)
    d.setColor(WHITE)
    for i in range(len(visible)):
        if i == idx: d.fillRect(x - 1, 13, 4, 4)
        else: d.fillRect(x, 14, 2, 2)
        x += 10


def fmt_age(secs):
    if secs < 60: return "%ds" % secs
    if secs < 3600: return "%dm" % (secs // 60)
    return "%dh" % (secs // 3600)


def name_list(d, rows, y0=20):
    d.setColor(WHITE); d.setTextSize(1)
    w = d.width()
    for i, (name, right) in enumerate(rows):
        y = y0 + i * 11
        tw = d.getTextWidth(right)
        d.drawTextEllipsized(0, y, w - tw - 1, d.translate(name, 32))
        d.setCursor(w - tw - 1, y); d.print(right)


def scr_home(d, connected=True, lang="ru", msg=8, unread=3):
    home_header(d, "FIRST")
    w = d.width()
    d.setColor(WHITE)
    d.setTextSize(2)
    d.drawTextCentered(w // 2, 22, "MSG: %d/%d" % (msg, unread))
    d.setTextSize(1)
    d.drawTextCentered(w // 2, 43, "< Connected >" if connected else "Pin:123456")
    day = "Чт, 24.09" if lang == "ru" else "Thu, 24.09"
    d.drawTextCentered(w // 2, d.height() - 11, "21:17  " + day)


def scr_splash(d, subtitle="by Termin36", repeater=False):
    w = d.width()
    d.setColor(WHITE)
    d.drawXbm((w - 128) // 2, 3, ICONS["meshcore_logo"], 128, 13)
    site = "https://meshcore.io"
    if repeater:
        d.drawTextCentered(w // 2, 22, site)
    else:
        d.setCursor((w - d.getTextWidth(site)) // 2, 22); d.print(site)
    d.drawTextCentered(w // 2, 35, VERSION)
    d.drawTextCentered(w // 2, 48, subtitle)


def scr_recent(d):
    home_header(d, "RECENT")
    name_list(d, [(n, fmt_age(s)) for n, s in
                  [("Alexey", 45), ("Repeater South", 180), ("Fox", 600), ("Base Camp North", 7200)]])


def scr_neighbors(d):
    home_header(d, "NEIGHBORS")
    name_list(d, [(n, "%+.0f" % snr) for n, snr in
                  [("Repeater South", 9.25), ("Hill-2", 4.75), ("A3F09C12", -3.25), ("Bridge", -8.75)]])


def scr_neighbors_empty(d):
    home_header(d, "NEIGHBORS")
    d.setColor(WHITE); d.setTextSize(1)
    d.drawTextCentered(d.width() // 2, 28, "no neighbors")
    d.drawTextCentered(d.width() // 2, 64 - 11, "poll: " + PRESS_LABEL)


def scr_radio(d, lna=False):
    home_header(d, "RADIO")
    d.setColor(WHITE); d.setTextSize(1)
    d.setCursor(0, 20); d.print("FQ: %06.3f   SF: %d" % (869.525, 11))
    d.setCursor(0, 31); d.print("BW: %03.2f     CR: %d" % (250.0, 5))
    d.setCursor(0, 42); d.print("TX: %ddBm  LNA: %s" % (22, "on" if lna else "off"))
    d.setCursor(0, 53); d.print("Noise floor: %d" % -108)


def scr_power(d, name, cpu, boost, lna, hold=None, gps_on=False):
    home_header(d, "POWER", gps_on)
    w = d.width()
    d.setTextSize(1)
    y = 18
    d.setColor(WHITE)
    d.drawTextLeftAlign(0, y, "Power")
    d.drawTextRightAlign(w - 1, y, name)
    y += 12
    d.drawTextLeftAlign(0, y, "CPU off" if cpu else "CPU on")
    d.drawTextRightAlign(w - 1, y, "RX on" if boost else "RX off")
    y += 12
    d.drawTextLeftAlign(0, y, "LNA on" if lna else "LNA off")
    y += 12
    if y + 8 <= d.height():
        d.drawTextCentered(w // 2, y, hold or ("mode: " + PRESS_LABEL))


def scr_scan(d, live=-112, nl=-109, peak=-98, pkt=False):
    home_header(d, "SCAN")
    w = d.width()
    d.setColor(WHITE); d.setTextSize(2)
    d.drawTextCentered(w // 2, 18, "%d" % live)
    d.setTextSize(1)
    if pkt: d.drawTextRightAlign(w - 1, 18, "PKT")
    d.drawTextLeftAlign(0, 36, "NL %d" % nl if nl else "NL --")
    d.drawTextRightAlign(w - 1, 36, "pk %d" % peak)
    bw = w - 4
    fill = ((max(-120, min(-50, live)) + 120) * bw) // 70
    d.drawRect(2, 48, bw, 8)
    if fill > 2: d.fillRect(3, 49, fill - 2, 6)


def scr_fscan_idle(d):
    home_header(d, "FSCAN")
    d.setTextSize(1); d.setColor(WHITE)
    d.drawTextCentered(d.width() // 2, 28, "find quiet freq")
    d.drawTextCentered(d.width() // 2, d.height() - 11, "scan: " + PRESS_LABEL)


def scr_fscan_run(d, i=7, n=17, freq=868.775):
    home_header(d, "FSCAN")
    w = d.width()
    d.setTextSize(1); d.setColor(WHITE)
    d.drawTextCentered(w // 2, 22, "scan %d/%d" % (i, n))
    d.drawTextCentered(w // 2, 36, "%06.3f" % freq)
    bw = w - 4
    fill = (i * bw) // n
    d.drawRect(2, 50, bw, 8)
    if fill > 2: d.fillRect(3, 51, fill - 2, 6)


def scr_fscan_done(d):
    home_header(d, "FSCAN")
    d.setTextSize(1); d.setColor(WHITE)
    cur = 869.525
    d.drawTextLeftAlign(0, 18, "now %06.3f" % cur)
    y = 29
    for f, r in [(869.275, -121), (868.775, -119), (869.525, -112)]:
        d.drawTextLeftAlign(0, y, "%06.3f  %d%s" % (f, r, " *" if abs(f - cur) < 0.001 else ""))
        y += 11


def scr_gps(d):
    home_header(d, "GPS")
    w = d.width()
    d.setTextSize(1); d.setColor(WHITE)
    y = 20
    d.drawTextLeftAlign(0, y, "module"); d.drawTextRightAlign(w - 1, y, "on"); y += 12
    d.drawTextLeftAlign(0, y, "fix yes"); d.drawTextRightAlign(w - 1, y, "sat 9"); y += 12
    d.drawTextLeftAlign(0, y, "pos"); d.drawTextRightAlign(w - 1, y, "%.4f %.4f" % (55.7558, 37.6173)); y += 12
    d.drawTextLeftAlign(0, y, "alt"); d.drawTextRightAlign(w - 1, y, "%.2f" % 156.0)


def scr_beacon(d, mode, target, pos, sent):
    home_header(d, "BEACON")
    w = d.width()
    kind, _, iv = mode.partition(" ")
    d.setTextSize(1); d.setColor(WHITE)
    y = 18
    d.drawTextLeftAlign(0, y, kind)
    if iv: d.drawTextRightAlign(w - 1, y, iv)
    y += 12
    d.drawTextEllipsized(0, y, w - 1, d.translate(target, 32)); y += 12
    d.drawTextEllipsized(0, y, w - 1, pos); y += 12
    d.drawTextLeftAlign(0, y, sent)


MSG_SHORT = "Привет! Встречаемся у реки в 18:00, возьми рацию."
MSG_LONG = "Привет! Встречаемся у старого моста в 18:00, возьми рацию и запасной аккумулятор."
MSG_LATIN = ("Meet at the old bridge at 18:00. Bring the radio, a spare battery, "
             "a flashlight and water. If you are late, write to the group chat.")


MSG_EMOJI = "Привет! 👋👋  Встречаемся у реки 🏞️ в 18:00 🔥🔥🔥 — возьми рацию 📻!"


def scr_message(d, text=MSG_SHORT, page=0, unread=2, origin="(1) Алексей Смирнов:"):
    w = d.width()
    if d.legacy:
        # upstream layout: 78-byte buffer, sender on its own row
        d.setCursor(0, 0); d.setTextSize(1); d.setColor(WHITE)
        d.print("Unread: %d" % unread)
        d.setCursor(w - d.getTextWidth("3m") - 2, 0); d.print("3m")
        d.drawRect(0, 11, w, 1)
        d.setCursor(0, 14); d.print(d.translate(origin, 40))
        d.setCursor(0, 25)
        d.printWordWrap(d.translate(text, 78), w)
        return
    compact = d.height() < 100
    msg = d.translate(text, 161)
    start, num = 0, 0
    d.setTextSize(1); d.setColor(WHITE)
    while True:
        d.clear()
        d.setCursor(0, 14 if compact else 25)
        rest = start + d.printWordWrap(msg[start:], w)
        nxt = rest if rest < len(msg) and rest > start else 0
        if num == page or not nxt: break
        start, num = nxt, num + 1
    tag = "p%d%s " % (num + 1, "+" if nxt else "") if (nxt or num) else ""
    if compact:
        right = tag + ("+%d " % (unread - 1) if unread > 1 else "") + "3m"
        rw = d.getTextWidth(right)
        d.setCursor(w - rw - 1, 0); d.print(right)
        d.drawTextEllipsized(0, 0, w - rw - 4, d.translate(origin, 62))
        d.drawRect(0, 11, w, 1)
    else:
        d.setCursor(0, 0); d.print("Unread: %d" % unread)
        right = tag + "3m"
        d.setCursor(w - d.getTextWidth(right) - 2, 0); d.print(right)
        d.drawRect(0, 11, w, 1)
        d.drawTextEllipsized(0, 14, w, d.translate(origin, 62))


def scr_repeater(d, name="Repeater South", lna=False):
    d.setCursor(0, 0); d.setTextSize(1); d.setColor(WHITE)
    d.print(name)
    battery(d, 4100)
    d.setCursor(0, 20); d.print("FREQ: %06.3f SF%d" % (869.525, 11))
    d.setCursor(0, 30); d.print("BW: %03.2f CR: %d" % (250.0, 5))
    d.setCursor(0, 40); d.print("LNA: %s  NF: %d" % ("on" if lna else "off", -110))


SCREENS = [
    ("01_splash", "Заставка Companion", lambda d: scr_splash(d)),
    ("02_message", "Сообщение на кириллице", scr_message),
    ("02a_message_long", "Сообщение на 160 байт", lambda d: scr_message(d, MSG_LONG, 0, 1)),
    ("02b_message_page1", "Листание: стр. 1", lambda d: scr_message(d, MSG_LATIN, 0, 1)),
    ("02c_message_page2", "Листание: стр. 2", lambda d: scr_message(d, MSG_LATIN, 1, 1)),
    ("02d_message_emoji", "Эмодзи -> пробелы", lambda d: scr_message(
        d, MSG_EMOJI, 0, 1, "(2) 🦊 Лиса 🦊:")),
    ("03_recent", "Recent adverts", scr_recent),
    ("04_neighbors", "Neighbors: соседи по SNR", scr_neighbors),
    ("05_neighbors_empty", "Neighbors: до опроса", scr_neighbors_empty),
    ("06_radio", "Radio: LNA и шум", scr_radio),
    ("07_power_eco_max", "Power: Eco max", lambda d: scr_power(d, "Eco max", True, False, False)),
    ("08_power_ble", "Power: BLE мешает сну", lambda d: scr_power(
        d, "Eco good", False, True, False, hold="BLE needs CPU on")),
    ("09_noise_scan", "Noise scan", scr_scan),
    ("10_fscan_idle", "Freq scan: ожидание", scr_fscan_idle),
    ("11_fscan_run", "Freq scan: идёт скан", scr_fscan_run),
    ("12_fscan_done", "Freq scan: результат", scr_fscan_done),
    ("13_gps", "GPS", scr_gps),
    ("14_beacon_group", "Beacon: группа", lambda d: scr_beacon(
        d, "group 30m", "Squad North", "55.7558  37.6173", "sent 4m ago")),
    ("15_beacon_chat_nofix", "Beacon: личка, нет фикса", lambda d: scr_beacon(
        d, "chat 15m", "Alexey", "no fix", "not sent")),
    ("16_beacon_alert", "Beacon: смена режима", lambda d: (scr_beacon(
        d, "group 30m", "Squad North", "55.7558  37.6173", "sent 4m ago"), d.alert("group 30m Squad"))),
    ("17_repeater_splash", "Заставка Repeater", lambda d: scr_splash(d, "Repeater by Termin36", True)),
    ("18_repeater", "Экран Repeater", scr_repeater),
]

# MeshPocket has no GPS, so its companion build has no GPS / Beacon pages.
EINK_SCREENS = [s for s in SCREENS if s[0][:2] not in ("13", "14", "15", "16")] + [
    ("19_alert", "Всплывающее уведомление", lambda d: (scr_scan(d), d.alert("Peak reset"))),
]

COMPARE = [
    ("cmp_message", "Сообщение", lambda d: scr_message(d, MSG_LONG)),
]

# ------------------------------------------------------------------- output

SCALE = 4
BG = (8, 10, 14)
OFF = (20, 24, 30)
ON = (205, 230, 255)

EINK_SCALE = 3
BEZEL = (46, 48, 52)
PAPER = (224, 224, 214)
INK = (30, 32, 36)


def to_image(d):
    pad = 3 * SCALE
    img = Image.new("RGB", (W * SCALE + pad * 2, H * SCALE + pad * 2), BG)
    dr = ImageDraw.Draw(img)
    for y in range(H):
        for x in range(W):
            x0, y0 = pad + x * SCALE, pad + y * SCALE
            dr.rectangle([x0, y0, x0 + SCALE - 2, y0 + SCALE - 2], fill=ON if d.fb[y][x] else OFF)
    return img


def to_image_eink(d):
    s, pad = EINK_SCALE, 4 * EINK_SCALE
    img = Image.new("RGB", (EINK_W * s + pad * 2, EINK_H * s + pad * 2), BEZEL)
    dr = ImageDraw.Draw(img)
    dr.rectangle([pad, pad, pad + EINK_W * s - 1, pad + EINK_H * s - 1], fill=PAPER)
    for y in range(EINK_H):
        for x in range(EINK_W):
            if d.fb[y][x]:
                x0, y0 = pad + x * s, pad + y * s
                dr.rectangle([x0, y0, x0 + s - 1, y0 + s - 1], fill=INK)
    return img


def caption_font(size):
    for name in ("segoeui.ttf", "arial.ttf", "DejaVuSans.ttf"):
        for base in (r"C:\Windows\Fonts", "/usr/share/fonts/truetype/dejavu", ""):
            try: return ImageFont.truetype(os.path.join(base, name), size)
            except OSError: pass
    return ImageFont.load_default()


def sheet(items, cols, title=None):
    font, tfont = caption_font(22), caption_font(28)
    cw, ch = items[0][1].size
    cap, gap = 36, 24
    top = 56 if title else 0
    rows = (len(items) + cols - 1) // cols
    img = Image.new("RGB", (gap + cols * (cw + gap), top + gap + rows * (ch + cap + gap)), (32, 34, 40))
    dr = ImageDraw.Draw(img)
    if title: dr.text((gap, 14), title, font=tfont, fill=(240, 240, 240))
    for i, (label, im) in enumerate(items):
        x = gap + (i % cols) * (cw + gap)
        y = top + gap + (i // cols) * (ch + cap + gap)
        dr.text((x, y), label, font=font, fill=(220, 220, 220))
        img.paste(im, (x, y + cap))
    return img


def parse_clock_face(text, name):
    raw = re.search(r"CLOCK_%s_BITS\[\][^{]*\{(.*?)\};" % name, text, re.S).group(1)
    bits = [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]+", raw)]
    glyphs = []
    body = re.search(r"CLOCK_%s_GLYPHS\[\][^{]*\{(.*?)\};" % name, text, re.S).group(1)
    for m in re.finditer(r"\{\s*(?:'([^']*)'|0x([0-9A-Fa-f]+))\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(-?\d+)\s*,\s*(\d+)\s*\}", body):
        ch = ord(m.group(1)) if m.group(1) is not None else int(m.group(2), 16)
        glyphs.append({"ch": ch, "w": int(m.group(3)), "h": int(m.group(4)),
                       "adv": int(m.group(5)), "top": int(m.group(6)), "off": int(m.group(7))})
    return {"bits": bits, "glyphs": {g["ch"]: g for g in glyphs}}


def clock_faces():
    with open(os.path.join(ROOT, "src", "helpers", "ui", "ClockFont.h"), encoding="utf-8") as f:
        text = f.read()
    return {n: parse_clock_face(text, n) for n in ("DIGIT_WIDE", "HEADER", "FOOTER")}


def clock_width(face, text):
    w, i, b = 0, 0, text.encode("utf-8")
    while i < len(b):
        cp, i = utf8_next(b, i)
        g = face["glyphs"].get(cp)
        if g: w += g["adv"]
    return w


def clock_ink(face, text):
    top, bot = 127, -127
    i, b = 0, text.encode("utf-8")
    while i < len(b):
        cp, i = utf8_next(b, i)
        g = face["glyphs"].get(cp)
        if not g or g["h"] == 0: continue
        top = min(top, g["top"])
        bot = max(bot, g["top"] + g["h"])
    if bot < top: return 0, 1
    return top, bot


def clock_blit(fb, face, x, y, text, Wd, Hd):
    i, b = 0, text.encode("utf-8")
    while i < len(b):
        cp, i = utf8_next(b, i)
        g = face["glyphs"].get(cp)
        if not g: continue
        rowb = (g["w"] + 7) >> 3
        for row in range(g["h"]):
            yy = y + g["top"] + row
            if yy < 0 or yy >= Hd: continue
            for col in range(g["w"]):
                xx = x + col
                if xx < 0 or xx >= Wd: continue
                bit = face["bits"][g["off"] + row * rowb + (col >> 3)] & (0x80 >> (col & 7))
                if bit: fb[yy][xx] = 1
        x += g["adv"]


def render_clock(faces, fw, fh, date, line1, line2):
    fb = [[0] * fw for _ in range(fh)]
    digits = faces["DIGIT_WIDE"]
    header_h = 0
    if date:
        topd, botd = clock_ink(faces["HEADER"], date)
        y = max(1, 4 - topd)
        clock_blit(fb, faces["HEADER"], (fw - clock_width(faces["HEADER"], date)) // 2, y, date, fw, fh)
        header_h = (botd - topd) + 8
    shown, stack = "", False
    if line1 and line2:
        both = line1 + "    " + line2
        stack = clock_width(faces["FOOTER"], both) > fw - 8
        shown = line1 if stack else both
    elif line1 or line2:
        shown = line1 or line2
    footer_h = 0
    if shown:
        top0, bot0 = clock_ink(faces["FOOTER"], shown)
        ink = bot0 - top0
        top1 = bot1 = 0
        if stack:
            top1, bot1 = clock_ink(faces["FOOTER"], line2)
            ink += 4 + (bot1 - top1)
        y = fh - 6 - ink
        if not stack:
            clock_blit(fb, faces["FOOTER"], (fw - clock_width(faces["FOOTER"], shown)) // 2, y - top0, shown, fw, fh)
        else:
            clock_blit(fb, faces["FOOTER"], (fw - clock_width(faces["FOOTER"], line1)) // 2, y - top0, line1, fw, fh)
            clock_blit(fb, faces["FOOTER"], (fw - clock_width(faces["FOOTER"], line2)) // 2,
                       y + (bot0 - top0) + 4 - top1, line2, fw, fh)
        footer_h = ink + 12
    top, bot = clock_ink(digits, "21:17")
    ink = bot - top
    area = fh - footer_h - header_h
    y = header_h + (area - ink) // 2 - top
    if y < 2: y = 2
    clock_blit(fb, digits, (fw - clock_width(digits, "21:17")) // 2, y, "21:17", fw, fh)
    return fb


class Fb:
    def __init__(self, fb): self.fb = fb


HOME_DATE = [
    ("home_date_0", "ru, подключен", lambda d: scr_home(d, True, "ru")),
    ("home_date_1", "en, подключен", lambda d: scr_home(d, True, "en")),
    ("home_date_2", "ru, PIN", lambda d: scr_home(d, False, "ru")),
    ("home_date_3", "en, PIN", lambda d: scr_home(d, False, "en")),
]


def render_set(out, subdir, screens, make, to_img, cols, title, gallery_name):
    os.makedirs(os.path.join(out, subdir), exist_ok=True)
    gallery = []
    for fname, label, fn in screens:
        d = make(); fn(d)
        im = to_img(d)
        im.save(os.path.join(out, subdir, fname + ".png"))
        gallery.append((label, im))
    sheet(gallery, cols, title).save(os.path.join(out, gallery_name))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs", "screens")
    render_set(out, "oled", SCREENS, Oled, to_image, 3,
               "MeshCore by Termin36 — OLED 128x64", "gallery.png")
    render_set(out, "meshpocket", EINK_SCREENS, Eink, to_image_eink, 3,
               "MeshCore by Termin36 — Heltec MeshPocket, e-ink 2.13\" 250x122", "gallery_meshpocket.png")

    date_items = []
    for fname, label, fn in HOME_DATE:
        d = Oled(); fn(d)
        im = to_image(d)
        im.save(os.path.join(out, "oled", fname + ".png"))
        date_items.append((label, im))

    faces = clock_faces()
    clocks = [
        ("20_clock", "ru, нет сообщений", "Чт, 24.09", "", ""),
        ("20_clock_msgs", "ru, группы и чаты", "Чт, 24.09", "Группы: 5", "Чаты: 3"),
        ("20_clock_en", "en, нет сообщений", "Thu, 24.09", "", ""),
        ("20_clock_en_msgs", "en, группы и чаты", "Thu, 24.09", "Groups: 5", "Chat: 3"),
    ]
    clock_items = []
    for fname, label, date, a, b in clocks:
        fb = render_clock(faces, EINK_W, EINK_H, date, a, b)
        im = to_image_eink(Fb(fb))
        im.save(os.path.join(out, "meshpocket", fname + ".png"))
        clock_items.append((label, im))

    # Two rows: OLED date line, then the e-ink clock. Cells differ in size, so
    # each row is its own sheet pasted onto one page.
    top = sheet(date_items, 4, "OLED 128×64 — время на главном экране")
    bot = sheet(clock_items, 4, "Электронные чернила — часы через минуту простоя")
    page = Image.new("RGB", (max(top.width, bot.width), top.height + bot.height), (32, 34, 40))
    page.paste(top, (0, 0))
    page.paste(bot, (0, top.height))
    page.save(os.path.join(out, "gallery_date.png"))

    for fname, label, fn in COMPARE:
        pair = []
        for legacy, tag in ((True, "оригинал"), (False, "форк")):
            d = Oled(legacy=legacy); fn(d)
            pair.append(("%s — %s" % (label, tag), to_image(d)))
        sheet(pair, 2).save(os.path.join(out, fname + ".png"))
    print("saved to", out)


if __name__ == "__main__":
    main()
