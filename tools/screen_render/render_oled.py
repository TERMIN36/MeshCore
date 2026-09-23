"""Offline renders of the companion / repeater UI on a 128x64 SSD1306 OLED.

Fonts and icons are parsed from the firmware sources, and the drawing logic
mirrors SSD1306Display and the page layouts in examples/*/UITask.cpp, so the
output matches the device pixel for pixel. Page content is sample data.

Usage:  python tools/screen_render/render_oled.py [out_dir]
"""

import glob
import os
import re
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


def find_glcdfont():
    hits = glob.glob(os.path.join(ROOT, ".pio", "libdeps", "*", "Adafruit GFX Library", "glcdfont.c"))
    if not hits:
        sys.exit("glcdfont.c not found: build any OLED env once so PlatformIO fetches Adafruit GFX")
    return hits[0]


FONT = parse_c(find_glcdfont())[0]["font"]
_cyr_arr, _cyr_sc = parse_c(os.path.join(ROOT, "src", "helpers", "ui", "CyrillicGlyphs.inc"))
CYR_H = _cyr_sc["CYR_OLED_H"]
CYR_BASE = _cyr_sc["CYR_OLED_BASE"]
CYR_ADV = _cyr_arr["CYR_OLED_ADV"]
CYR_OFF = _cyr_arr["CYR_OLED_OFF"]
CYR_BITS = _cyr_arr["CYR_OLED_BITS"]
ICONS = parse_c(os.path.join(ROOT, "examples", "companion_radio", "ui-new", "icons.h"))[0]


def cyr_index(cp):
    if 0x0410 <= cp <= 0x042F: return cp - 0x0410
    if cp == 0x0401: return 32
    if 0x0430 <= cp <= 0x044F: return 33 + cp - 0x0430
    if cp == 0x0451: return 65
    return -1


def cyr_pixel(idx, x, y):
    w = CYR_ADV[idx]
    if x >= w or y >= CYR_H: return False
    rowb = (w + 7) >> 3
    return bool(CYR_BITS[CYR_OFF[idx] + y * rowb + (x >> 3)] & (0x80 >> (x & 7)))


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


class Oled:
    """SSD1306Display on top of Adafruit_SSD1306. legacy=True mimics upstream (no Cyrillic)."""

    def __init__(self, legacy=False):
        self.legacy = legacy
        self.fb = [[0] * W for _ in range(H)]
        self.color = WHITE
        self.ts = 1
        self.cx = self.cy = 0

    def width(self): return W
    def height(self): return H
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
        s = self.ts
        w = CYR_ADV[idx] if idx >= 0 else (CYR_H * 2) // 3
        for row in range(CYR_H):
            for col in range(w):
                on = cyr_pixel(idx, col, row) if idx >= 0 else (
                    row == 0 or row == CYR_H - 1 or col == 0 or col == w - 1)
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
        dy = (6 - CYR_BASE) * s
        i = 0
        while i < len(b) and b[i]:
            cp, i = utf8_next(b, i)
            if cp == 10:
                self.cx, self.cy = 0, self.cy + (CYR_H + 1) * s
            elif cp < 0x80:
                self._gfx_write(self.cx, self.cy, cp)
                self.cx += 6 * s
            else:
                idx = cyr_index(cp)
                self._cyr_glyph(idx, self.cx, self.cy + dy)
                self.cx += (CYR_ADV[idx] if idx >= 0 else (CYR_H * 2) // 3) * s

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
            w += (CYR_ADV[idx] if idx >= 0 else 6) * self.ts
        return w

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
            elif cp >= 0x80: out.append(0xDB)
        return bytes(out)

    def printWordWrap(self, s, max_width):
        if self.legacy:
            return self.print(s)
        b = B(s)
        origin, sc = self.cx, self.ts
        max_width = min(max_width, max(1, W - origin))
        step = (CYR_H + 1) * sc
        p = 0
        while p < len(b) and self.cy < H:
            line, width, brk, end = p, 0, None, p
            while end < len(b) and b[end] != 10:
                cp, nxt = utf8_next(b, end)
                idx = cyr_index(cp)
                cw = (CYR_ADV[idx] if idx >= 0 else 6) * sc
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
                if self.cy + step >= H: break
                self.cx, self.cy = origin, self.cy + step

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

    def alert(self, text):
        self.setTextSize(1)
        y, p = H // 3, H // 32
        self.setColor(BLACK); self.fillRect(p, y, W - p * 2, y)
        self.setColor(WHITE); self.drawRect(p, y, W - p * 2, y)
        self.drawTextCentered(W // 2, y + p * 3, text)


# ---------------------------------------------------------------- sample data

NODE_NAME = "Termin36"
BATT_MV = 3950
PRESS_LABEL = "long press"
PAGES = ["FIRST", "RECENT", "NEIGHBORS", "RADIO", "POWER", "SCAN", "FSCAN",
         "BLUETOOTH", "ADVERT", "GPS", "BEACON", "SHUTDOWN"]
UPSTREAM_PAGES = ["FIRST", "RECENT", "RADIO", "BLUETOOTH", "ADVERT", "GPS", "SHUTDOWN"]


def battery(d, mv, x_off=5):
    pct = max(0, min(100, ((mv - 3000) * 100) // 1200))
    ix, iw, ih = W - 24 - x_off, 24, 10
    d.setColor(WHITE)
    d.drawRect(ix, 0, iw, ih)
    d.fillRect(ix + iw, ih // 4, 3, ih // 2)
    d.fillRect(ix + 2, 2, (pct * (iw - 4)) // 100, ih - 4)


def home_header(d, page, gps_on=True):
    d.setColor(BLACK); d.fillRect(0, 0, W, 12)
    d.setTextSize(1); d.setColor(WHITE)
    d.setCursor(0, 2); d.print(d.translate(NODE_NAME, 32))
    battery(d, BATT_MV)
    visible = UPSTREAM_PAGES if d.legacy else [p for p in PAGES if gps_on or p != "BEACON"]
    idx = visible.index(page)
    x = W // 2 - 5 * (len(visible) - 1)
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
    for i, (name, right) in enumerate(rows):
        y = y0 + i * 11
        tw = d.getTextWidth(right)
        d.drawTextEllipsized(0, y, W - tw - 1, d.translate(name, 32))
        d.setCursor(W - tw - 1, y); d.print(right)


def scr_splash(d, subtitle="by Termin36", repeater=False):
    d.setColor(WHITE)
    d.drawXbm(0, 3, ICONS["meshcore_logo"], 128, 13)
    site = "https://meshcore.io"
    if repeater:
        d.drawTextCentered(W // 2, 22, site)
    else:
        d.setCursor((W - d.getTextWidth(site)) // 2, 22); d.print(site)
    d.drawTextCentered(W // 2, 35, "v1.17.1")
    d.drawTextCentered(W // 2, 48, subtitle)


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
    d.drawTextCentered(W // 2, 28, "no neighbors")
    d.drawTextCentered(W // 2, 64 - 11, "poll: " + PRESS_LABEL)


def scr_radio(d, lna=False):
    home_header(d, "RADIO")
    d.setColor(WHITE); d.setTextSize(1)
    d.setCursor(0, 20); d.print("FQ: %06.3f   SF: %d" % (869.525, 11))
    d.setCursor(0, 31); d.print("BW: %03.2f     CR: %d" % (250.0, 5))
    d.setCursor(0, 42); d.print("TX: %ddBm  LNA: %s" % (22, "on" if lna else "off"))
    d.setCursor(0, 53); d.print("Noise floor: %d" % -108)


def scr_power(d, name, cpu, boost, lna, hold=None, gps_on=False):
    home_header(d, "POWER", gps_on)
    d.setTextSize(1)
    y = 18
    d.setColor(WHITE)
    d.drawTextLeftAlign(0, y, "Power")
    d.drawTextRightAlign(W - 1, y, name)
    y += 12
    d.drawTextLeftAlign(0, y, "CPU off" if cpu else "CPU on")
    d.drawTextRightAlign(W - 1, y, "RX on" if boost else "RX off")
    y += 12
    d.drawTextLeftAlign(0, y, "LNA on" if lna else "LNA off")
    y += 12
    if y + 8 <= H:
        d.drawTextCentered(W // 2, y, hold or ("mode: " + PRESS_LABEL))


def scr_scan(d, live=-112, nl=-109, peak=-98, pkt=False):
    home_header(d, "SCAN")
    d.setColor(WHITE); d.setTextSize(2)
    d.drawTextCentered(W // 2, 18, "%d" % live)
    d.setTextSize(1)
    if pkt: d.drawTextRightAlign(W - 1, 18, "PKT")
    d.drawTextLeftAlign(0, 36, "NL %d" % nl if nl else "NL --")
    d.drawTextRightAlign(W - 1, 36, "pk %d" % peak)
    bw = W - 4
    fill = ((max(-120, min(-50, live)) + 120) * bw) // 70
    d.drawRect(2, 48, bw, 8)
    if fill > 2: d.fillRect(3, 49, fill - 2, 6)


def scr_fscan_idle(d):
    home_header(d, "FSCAN")
    d.setTextSize(1); d.setColor(WHITE)
    d.drawTextCentered(W // 2, 28, "find quiet freq")
    d.drawTextCentered(W // 2, H - 11, "scan: " + PRESS_LABEL)


def scr_fscan_run(d, i=7, n=17, freq=868.775):
    home_header(d, "FSCAN")
    d.setTextSize(1); d.setColor(WHITE)
    d.drawTextCentered(W // 2, 22, "scan %d/%d" % (i, n))
    d.drawTextCentered(W // 2, 36, "%06.3f" % freq)
    bw = W - 4
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
    d.setTextSize(1); d.setColor(WHITE)
    y = 20
    d.drawTextLeftAlign(0, y, "module"); d.drawTextRightAlign(W - 1, y, "on"); y += 12
    d.drawTextLeftAlign(0, y, "fix yes"); d.drawTextRightAlign(W - 1, y, "sat 9"); y += 12
    d.drawTextLeftAlign(0, y, "pos"); d.drawTextRightAlign(W - 1, y, "%.4f %.4f" % (55.7558, 37.6173)); y += 12
    d.drawTextLeftAlign(0, y, "alt"); d.drawTextRightAlign(W - 1, y, "%.2f" % 156.0)


def scr_beacon(d, mode, target, pos, sent):
    home_header(d, "BEACON")
    kind, _, iv = mode.partition(" ")
    d.setTextSize(1); d.setColor(WHITE)
    y = 18
    d.drawTextLeftAlign(0, y, kind)
    if iv: d.drawTextRightAlign(W - 1, y, iv)
    y += 12
    d.drawTextEllipsized(0, y, W - 1, d.translate(target, 32)); y += 12
    d.drawTextEllipsized(0, y, W - 1, pos); y += 12
    d.drawTextLeftAlign(0, y, sent)


def scr_message(d):
    d.setCursor(0, 0); d.setTextSize(1); d.setColor(WHITE)
    d.print("Unread: 2")
    d.setCursor(W - d.getTextWidth("3m") - 2, 0); d.print("3m")
    d.drawRect(0, 11, W, 1)
    origin = d.translate("(1) Алексей Смирнов:", 40)
    if d.legacy:
        d.setCursor(0, 14); d.print(origin)
    else:
        d.drawTextEllipsized(0, 14, W, origin)
    d.setCursor(0, 25)
    d.printWordWrap(d.translate("Привет! Встречаемся у реки в 18:00, возьми рацию.", 160), W)


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

COMPARE = [
    ("cmp_message", "Сообщение", scr_message),
]

# ------------------------------------------------------------------- output

SCALE = 4
BG = (8, 10, 14)
OFF = (20, 24, 30)
ON = (205, 230, 255)


def to_image(d):
    pad = 3 * SCALE
    img = Image.new("RGB", (W * SCALE + pad * 2, H * SCALE + pad * 2), BG)
    dr = ImageDraw.Draw(img)
    for y in range(H):
        for x in range(W):
            x0, y0 = pad + x * SCALE, pad + y * SCALE
            dr.rectangle([x0, y0, x0 + SCALE - 2, y0 + SCALE - 2], fill=ON if d.fb[y][x] else OFF)
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


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs", "screens")
    os.makedirs(os.path.join(out, "oled"), exist_ok=True)
    gallery = []
    for fname, label, fn in SCREENS:
        d = Oled(); fn(d)
        im = to_image(d)
        im.save(os.path.join(out, "oled", fname + ".png"))
        gallery.append((label, im))
    sheet(gallery, 3, "MeshCore by Termin36 — OLED 128x64").save(os.path.join(out, "gallery.png"))

    for fname, label, fn in COMPARE:
        pair = []
        for legacy, tag in ((True, "оригинал"), (False, "форк")):
            d = Oled(legacy=legacy); fn(d)
            pair.append(("%s — %s" % (label, tag), to_image(d)))
        sheet(pair, 2).save(os.path.join(out, fname + ".png"))
    print("saved to", out)


if __name__ == "__main__":
    main()
