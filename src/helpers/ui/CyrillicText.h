#pragma once

#include "CyrillicFont.h"

// Text layout for drivers whose Latin font is the classic 6x8 cell font
// (Adafruit GFX default, TFT_eSPI GLCD, LovyanGFX Font0, ...). Cyrillic is
// drawn from the OLED face scaled like the Latin font. All coordinates are
// physical pixels; the pen y is the top of the Latin cell.
class CyrCellText {
protected:
  virtual void cyrAscii(int x, int y, uint8_t c) = 0;
  virtual void cyrFill(int x, int y, int w, int h) = 0;

public:
  int pen_x = 0, pen_y = 0;
  int scale_x = 1, scale_y = 1;  // physical pixels per font pixel
  int pix_num = 1, pix_div = 1;  // extra rational scale, 1/1 leaves pixels unchanged
  int wrap_px = 0;               // wrap Latin at this x like GFX print(); 0 = never

  int pix(int v) const { return (int)((long)v * pix_num / pix_div); }

  static bool hasUtf8(const char* s) {
    for (; *s; s++) {
      if ((uint8_t)*s >= 0x80) return true;
    }
    return false;
  }

  int cyrLineStep() const { return pix((CYR_OLED.height + 1) * scale_y); }

  int cyrAdvance(uint32_t cp) const {
    if (cp < 32) return 0;
    if (cp < 0x80) return pix(6 * scale_x);
    int idx = cyrillicGlyphIndex(cp);
    return pix((idx >= 0 ? ::cyrAdvance(CYR_OLED, idx) : (CYR_OLED.height * 2) / 3) * scale_x);
  }

  int cyrWidth(const char* str) const {
    int w = 0;
    const char* p = str;
    while (*p) {
      uint32_t cp = 0;
      p = utf8Next(p, cp);
      w += cyrAdvance(cp);
    }
    return w;
  }

  void cyrPrint(const char* begin, const char* end) {
    int dy = pix((6 - (int)CYR_OLED.baseline) * scale_y);
    const char* p = begin;
    while (p < end && *p) {
      uint32_t cp = 0;
      const char* next = utf8Next(p, cp);
      if (next > end) next = end;
      if (cp == '\n') {
        pen_x = 0;
        pen_y += cyrLineStep();
      } else if (cp >= 32 && cp < 0x80) {
        if (wrap_px > 0 && pen_x + pix(6 * scale_x) > wrap_px) {
          pen_x = 0;
          pen_y += pix(8 * scale_y);
        }
        cyrAscii(pen_x, pen_y, (uint8_t)cp);
        pen_x += pix(6 * scale_x);
      } else if (cp >= 0x80) {
        int idx = cyrillicGlyphIndex(cp);
        drawGlyph(idx, pen_x, pen_y + dy);
        pen_x += cyrAdvance(cp);
      }
      p = next;
    }
  }

  void cyrPrint(const char* str) { cyrPrint(str, str + strlen(str)); }

  // Breaks at spaces, '-' and '/', like SSD1306Display::printWordWrap().
  const char* cyrWordWrap(const char* str, int max_px, int bottom_px) {
    int origin = pen_x;
    if (max_px < 1) max_px = 1;
    const char* p = str;
    while (*p && pen_y < bottom_px) {
      const char* line = p;
      int w = 0;
      const char* brk = nullptr;
      const char* end = p;
      while (*end && *end != '\n') {
        uint32_t cp = 0;
        const char* next = utf8Next(end, cp);
        int cw = cyrAdvance(cp);
        if (w + cw > max_px && end != line) break;
        w += cw;
        if (cp == ' ' || cp == '-' || cp == '/') brk = next;
        end = next;
      }
      const char* cut = end;
      if (*end && *end != '\n' && brk && brk > line) cut = brk;
      cyrPrint(line, cut);
      if (*cut == '\n' || *cut == ' ') cut++;
      if (cut == p) break;
      p = cut;
      if (*p) {
        int next_y = pen_y + cyrLineStep();
        if (next_y + pix(8 * scale_y) > bottom_px) break;
        pen_x = origin;
        pen_y = next_y;
      }
    }
    return p;
  }

private:
  void drawGlyph(int idx, int x, int y) {
    int w = idx >= 0 ? ::cyrAdvance(CYR_OLED, idx) : (CYR_OLED.height * 2) / 3;
    int h = CYR_OLED.height;
    if (w < 1) w = 1;
    for (int row = 0; row < h; row++) {
      int run = -1;
      for (int col = 0; col <= w; col++) {
        bool on = col < w && (idx >= 0 ? cyrPixel(CYR_OLED, idx, col, row)
                                       : (row == 0 || row == h - 1 || col == 0 || col == w - 1));
        if (on) {
          if (run < 0) run = col;
        } else if (run >= 0) {
          int x0 = x + pix(run * scale_x);
          int y0 = y + pix(row * scale_y);
          int fw = pix(col * scale_x) - pix(run * scale_x);
          int fh = pix((row + 1) * scale_y) - pix(row * scale_y);
          if (fw < 1) fw = 1;
          if (fh < 1) fh = 1;
          cyrFill(x0, y0, fw, fh);
          run = -1;
        }
      }
    }
  }
};
