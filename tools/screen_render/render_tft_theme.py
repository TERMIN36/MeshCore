"""Before/after renders of the color-TFT dark theme.

Logical 128x64 UI (the grid every color TFT scales), painted with the old light
palette and the dark palette now in LGFX / ST7735 / ST7789LCD / NV3001B.
Two panels: Heltec V4 / T-Deck ST7789 (320x240) and Tracker ST7735 (160x80).

Usage:  python tools/screen_render/render_tft_theme.py [out_dir]
"""

import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import render_oled as R

ROOT = R.ROOT
W, H = 128, 64

LIGHT = {
    "window": 0xFFFF, "title": 0x001F, "title_txt": 0xFFFF,
    "primary": 0x0000, "secondary": (18 << 11) | (36 << 5) | 18,
    "warning": 0xFD20, "popup": 0x07FF, "popup_txt": 0x0000, "corp": 0x001A,
}
DARK = {
    "window": 0x0000, "title": 0x001F, "title_txt": 0xFFFF,
    "primary": 0xFFFF, "secondary": (22 << 11) | (44 << 5) | 22,
    "warning": 0xFD20, "popup": 0x4A69, "popup_txt": 0xFFFF, "corp": 0x04DF,
}


def rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)


class Panel:
    def __init__(self, pal):
        self.pal = pal
        self.fb = [[pal["window"]] * W for _ in range(H)]
        self.color = pal["primary"]
        self.ts = 1
        self.cx = self.cy = 0

    def width(self): return W
    def height(self): return H
    def setColor(self, key): self.color = self.pal[key]
    def setTextSize(self, s): self.ts = max(1, s)
    def setCursor(self, x, y): self.cx, self.cy = x, y

    def fillRect(self, x, y, w, h):
        for yy in range(max(0, y), min(H, y + h)):
            row = self.fb[yy]
            for xx in range(max(0, x), min(W, x + w)):
                row[xx] = self.color

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
            line = R.FONT[c * 5 + i]
            for j in range(8):
                if line & 1: self.fillRect(x + i * s, y + j * s, s, s)
                line >>= 1

    def print(self, s):
        b = R.B(s)
        s = self.ts
        i = 0
        while i < len(b) and b[i]:
            cp, i = R.utf8_next(b, i)
            if cp == 10:
                self.cx, self.cy = 0, self.cy + 8 * s
            elif cp < 0x80:
                self._char(self.cx, self.cy, cp)
                self.cx += 6 * s
            else:
                idx = R.cyr_index(cp)
                f = R.CYR_OLED
                w = f.advance(idx)
                dy = (6 - f.base) * s
                for row in range(f.h):
                    for col in range(w):
                        on = f.pixel(idx, col, row) if idx >= 0 else (
                            row == 0 or row == f.h - 1 or col == 0 or col == w - 1)
                        if on: self.fillRect(self.cx + col * s, self.cy + dy + row * s, s, s)
                self.cx += w * s

    def getTextWidth(self, s):
        b, w, i = R.B(s), 0, 0
        while i < len(b):
            cp, i = R.utf8_next(b, i)
            if cp < 0x80: w += 6 * self.ts
            else:
                idx = R.cyr_index(cp)
                w += (R.CYR_OLED.adv[idx] if idx >= 0 else 6) * self.ts
        return w

    def drawTextCentered(self, mid, y, s):
        self.setCursor(mid - self.getTextWidth(s) // 2, y); self.print(s)

    def drawTextRightAlign(self, x, y, s):
        self.setCursor(x - self.getTextWidth(s), y); self.print(s)

    def drawTextLeftAlign(self, x, y, s):
        self.setCursor(x, y); self.print(s)

    def drawTextEllipsized(self, x, y, max_width, s):
        t = s
        ell = "..."
        if self.getTextWidth(t) > max_width:
            while t and self.getTextWidth(t + ell) > max_width:
                t = t[:-1]
            t += ell
        self.setCursor(x, y); self.print(t)

    def image(self, pw, ph, scale=2):
        src = Image.new("RGB", (W, H))
        src.putdata([rgb565(self.fb[y][x]) for y in range(H) for x in range(W)])
        panel = src.resize((pw, ph), Image.NEAREST)
        pad = 8
        img = Image.new("RGB", (pw * scale + pad * 2, ph * scale + pad * 2), (24, 26, 30))
        img.paste(panel.resize((pw * scale, ph * scale), Image.NEAREST), (pad, pad))
        return img


def header(d, page):
    pages = ["MSG", "Recent", "Neigh", "Radio", "Power", "Scan"]
    d.setColor("title"); d.fillRect(0, 0, W, 12)
    d.setTextSize(1); d.setColor("title_txt")
    d.setCursor(0, 2); d.print("Termin36")
    d.drawRect(W - 29, 0, 24, 10)
    d.fillRect(W - 5, 2, 3, 5)
    d.fillRect(W - 27, 2, 14, 6)
    x = W // 2 - 5 * (len(pages) - 1)
    d.setColor("title")
    for i, _ in enumerate(pages):
        if pages[i] == page: d.fillRect(x - 1, 13, 4, 4)
        else: d.fillRect(x, 14, 2, 2)
        x += 10


def scr_splash(d):
    d.setColor("corp")
    d.drawXbm((W - 128) // 2, 3, R.ICONS["meshcore_logo"], 128, 13)
    d.setColor("primary"); d.setTextSize(1)
    site = "https://meshcore.io"
    d.setCursor((W - d.getTextWidth(site)) // 2, 22); d.print(site)
    d.drawTextCentered(W // 2, 35, R.VERSION)
    d.setColor("secondary")
    d.drawTextCentered(W // 2, 48, "by Termin36")


def scr_home(d):
    header(d, "MSG")
    d.setColor("primary"); d.setTextSize(2)
    d.drawTextCentered(W // 2, 22, "MSG: 8/3")
    d.setTextSize(1)
    d.setColor("warning")
    d.drawTextCentered(W // 2, 43, "< Connected >")
    d.setColor("secondary")
    d.drawTextCentered(W // 2, 54, "21:17  Чт, 24.09")


def scr_message(d):
    d.setTextSize(1)
    d.setColor("secondary")
    origin = "(1) Алексей"
    d.drawTextEllipsized(0, 0, 78, origin)
    d.setColor("corp")
    d.drawTextRightAlign(W - 1, 0, "+2 3m")
    d.drawRect(0, 11, W, 1)
    d.setColor("primary")
    d.setCursor(0, 14)
    d.print("Привет! Встречаемся")
    d.setCursor(0, 26)
    d.print("у реки в 18:00,")
    d.setCursor(0, 38)
    d.print("возьми рацию.")


def scr_neighbors(d):
    header(d, "Neigh")
    rows = [("Repeater South", "+9"), ("Hill-2", "+5"), ("Мост", "-3"), ("Base Camp", "-9")]
    d.setTextSize(1)
    for i, (name, snr) in enumerate(rows):
        y = 20 + i * 11
        d.setColor("secondary" if i == 3 else "primary")
        tw = d.getTextWidth(snr)
        d.drawTextEllipsized(0, y, W - tw - 2, name)
        d.setCursor(W - tw - 1, y); d.print(snr)


def scr_radio(d):
    header(d, "Radio")
    d.setTextSize(1); d.setColor("primary")
    d.setCursor(0, 20); d.print("FQ: 869.525   SF: 11")
    d.setCursor(0, 31); d.print("BW: 250.00     CR: 5")
    d.setColor("secondary")
    d.setCursor(0, 42); d.print("TX: 22dBm  LNA: off")
    d.setColor("warning")
    d.setCursor(0, 53); d.print("Noise floor: -108")


def scr_alert(d):
    scr_radio(d)
    y, p = H // 3, 2
    d.setColor("popup"); d.fillRect(p, y, W - p * 2, y)
    d.setColor("popup_txt"); d.drawRect(p, y, W - p * 2, y)
    d.drawTextCentered(W // 2, y + 6, "Peak reset")


SCREENS = [
    ("splash", "Заставка", scr_splash),
    ("home", "Главная", scr_home),
    ("message", "Сообщение", scr_message),
    ("neighbors", "Соседи", scr_neighbors),
    ("radio", "Радио", scr_radio),
    ("alert", "Уведомление", scr_alert),
]

PANELS = [("st7789", "ST7789  320x240", 320, 240), ("st7735", "ST7735  160x80", 160, 80)]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs", "screens", "tft")
    os.makedirs(out, exist_ok=True)
    font = R.caption_font(22)
    tfont = R.caption_font(28)
    made = []
    for key, title, fn in SCREENS:
        pair = []
        for pal, tag in ((LIGHT, "до"), (DARK, "после")):
            d = Panel(pal); fn(d)
            im = d.image(320, 240, 2)
            im.save(os.path.join(out, "%s_%s.png" % (key, tag)))
            pair.append(im)
        gap, cap = 16, 32
        w = pair[0].width * 2 + gap * 3
        h = pair[0].height + cap + gap * 2
        sheet = Image.new("RGB", (w, h), (32, 34, 40))
        dr = ImageDraw.Draw(sheet)
        dr.text((gap, 8), "%s  —  до / после" % title, font=tfont, fill=(240, 240, 240))
        sheet.paste(pair[0], (gap, cap + gap))
        sheet.paste(pair[1], (gap * 2 + pair[0].width, cap + gap))
        sheet.save(os.path.join(out, "%s_before_after.png" % key))
        made.append((title, pair[0], pair[1]))

    # gallery: each screen as before | after, ST7789 on top of a ST7735 strip
    cell_w, cell_h = made[0][1].size
    small = []
    for key, title, fn in SCREENS:
        row = []
        for pal in (LIGHT, DARK):
            d = Panel(pal); fn(d)
            row.append(d.image(160, 80, 3))
        small.append(row)
    sw, sh = small[0][0].size
    cols = 2
    rows = len(SCREENS)
    gap, top, lab = 20, 64, 28
    gw = gap + cols * (cell_w + gap)
    gh = top + rows * (lab + cell_h + 8 + sh + gap)
    gal = Image.new("RGB", (gw, gh), (32, 34, 40))
    dr = ImageDraw.Draw(gal)
    dr.text((gap, 16), "Цветные TFT — тёмная тема (ST7789 и ST7735)", font=tfont, fill=(240, 240, 240))
    for i, (title, before, after) in enumerate(made):
        y = top + i * (lab + cell_h + 8 + sh + gap)
        dr.text((gap, y), "%s    слева светлая, справа тёмная" % title, font=font, fill=(220, 220, 220))
        gal.paste(before, (gap, y + lab))
        gal.paste(after, (gap * 2 + cell_w, y + lab))
        gal.paste(small[i][0], (gap, y + lab + cell_h + 8))
        gal.paste(small[i][1], (gap * 2 + cell_w, y + lab + cell_h + 8))
    gal.save(os.path.join(out, "gallery_before_after.png"))
    print("saved to", out)


if __name__ == "__main__":
    main()
