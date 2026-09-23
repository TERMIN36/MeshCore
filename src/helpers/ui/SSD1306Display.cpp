#include "SSD1306Display.h"
#include "CyrillicFont.h"

bool SSD1306Display::i2c_probe(TwoWire& wire, uint8_t addr) {
  wire.beginTransmission(addr);
  uint8_t error = wire.endTransmission();
  return (error == 0);
}

// Color scheme
ColorVal UIColor::window_bkg = SSD1306_BLACK;
ColorVal UIColor::title_bkg = SSD1306_BLACK;
ColorVal UIColor::title_txt = SSD1306_WHITE;
ColorVal UIColor::primary_txt = SSD1306_WHITE;
ColorVal UIColor::secondary_txt = SSD1306_WHITE;
ColorVal UIColor::warning_txt = SSD1306_WHITE;
ColorVal UIColor::popup_bkg = SSD1306_BLACK;
ColorVal UIColor::popup_txt = SSD1306_WHITE;
ColorVal UIColor::corp_blue = SSD1306_WHITE;

bool SSD1306Display::begin() {
  if (!_isOn) {
    if (_peripher_power) _peripher_power->claim();
    _isOn = true;
  }
  #ifdef DISPLAY_ROTATION
  display.setRotation(DISPLAY_ROTATION);
  #endif
  return display.begin(SSD1306_SWITCHCAPVCC, DISPLAY_ADDRESS, true, false) && i2c_probe(Wire, DISPLAY_ADDRESS);
}

void SSD1306Display::turnOn() {
  if (!_isOn) {
    if (_peripher_power) _peripher_power->claim();
    _isOn = true;  // set before begin() to prevent double claim
    if (_peripher_power) begin();  // re-init display after power was cut
  }
  display.ssd1306_command(SSD1306_DISPLAYON);
}

void SSD1306Display::turnOff() {
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  if (_isOn) {
    if (_peripher_power) {
#if PIN_OLED_RESET >= 0
      digitalWrite(PIN_OLED_RESET, LOW);
#endif
      _peripher_power->release();
    }
    _isOn = false;
  }
}

void SSD1306Display::clear() {
  display.clearDisplay();
  display.display();
}

void SSD1306Display::startFrame(ColorVal bkg) {
  display.clearDisplay();  // TODO: apply 'bkg'
  _color = SSD1306_WHITE;
  display.setTextColor(_color);
  _textSize = 1;
  display.setTextSize(1);
  display.cp437(true);         // Use full 256 char 'Code Page 437' font
}

void SSD1306Display::setTextSize(int sz) {
  if (sz < 1) sz = 1;
  _textSize = sz;
  display.setTextSize(sz);
}

void SSD1306Display::setColor(ColorVal c) {
  _color = c;
  display.setTextColor(_color);
}

void SSD1306Display::setCursor(int x, int y) {
  _cx = x;
  _cy = y;
  display.setCursor(x, y);
}

int SSD1306Display::cyrLineStep() const {
  int scale = _textSize < 1 ? 1 : _textSize;
  return (CYR_OLED.height + 1) * scale;
}

void SSD1306Display::drawCyrGlyph(int idx, int x, int y) {
  int scale = _textSize < 1 ? 1 : _textSize;
  int w = idx >= 0 ? cyrAdvance(CYR_OLED, idx) : (CYR_OLED.height * 2) / 3;
  int h = CYR_OLED.height;
  if (w < 1) w = 1;
  for (int row = 0; row < h; row++) {
    for (int col = 0; col < w; col++) {
      bool on = idx >= 0 ? cyrPixel(CYR_OLED, idx, col, row)
                         : (row == 0 || row == h - 1 || col == 0 || col == w - 1);
      if (!on) continue;
      display.fillRect(x + col * scale, y + row * scale, scale, scale, _color);
    }
  }
}

void SSD1306Display::printSpan(const char* begin, const char* end) {
  int scale = _textSize < 1 ? 1 : _textSize;
  int dy = (6 - (int)CYR_OLED.baseline) * scale;
  const char* p = begin;
  while (p < end && *p) {
    uint32_t cp = 0;
    const char* next = utf8Next(p, cp);
    if (next > end) next = end;
    if (cp == '\n') {
      _cx = 0;
      _cy += cyrLineStep();
      display.setCursor(_cx, _cy);
    } else if (cp < 0x80) {
      display.setCursor(_cx, _cy);
      display.write((uint8_t)cp);
      _cx += 6 * scale;
    } else {
      int idx = cyrillicGlyphIndex(cp);
      drawCyrGlyph(idx, _cx, _cy + dy);
      int adv = idx >= 0 ? cyrAdvance(CYR_OLED, idx) : (CYR_OLED.height * 2) / 3;
      _cx += adv * scale;
    }
    p = next;
  }
  display.setCursor(_cx, _cy);
}

void SSD1306Display::print(const char* str) {
  printSpan(str, str + strlen(str));
}

void SSD1306Display::printWordWrap(const char* str, int max_width) {
  int origin = _cx;
  int scale = _textSize < 1 ? 1 : _textSize;
  int limit = display.width() - origin;
  if (limit < 1) limit = 1;
  if (max_width > limit) max_width = limit;
  const char* p = str;
  while (*p && _cy < height()) {
    const char* line = p;
    int width = 0;
    const char* brk = nullptr;
    const char* end = p;
    while (*end && *end != '\n') {
      uint32_t cp = 0;
      const char* next = utf8Next(end, cp);
      int idx = cyrillicGlyphIndex(cp);
      int cw = (idx >= 0 ? (int)cyrAdvance(CYR_OLED, idx) : 6) * scale;
      if (width + cw > max_width && end != line) break;
      width += cw;
      if (cp == ' ' || cp == '-' || cp == '/') brk = next;
      end = next;
    }
    const char* cut = end;
    if (*end && *end != '\n' && brk && brk > line) cut = brk;
    printSpan(line, cut);
    if (*cut == '\n' || *cut == ' ') cut++;
    if (cut == p) break;
    p = cut;
    if (*p) {
      int next_y = _cy + cyrLineStep();
      if (next_y >= height()) break;
      _cx = origin;
      _cy = next_y;
      display.setCursor(_cx, _cy);
    }
  }
}

void SSD1306Display::translateUTF8ToBlocks(char* dest, const char* src, size_t dest_size) {
  translateUtf8KeepCyrillic(dest, src, dest_size);
}

void SSD1306Display::fillRect(int x, int y, int w, int h) {
  display.fillRect(x, y, w, h, _color);
}

void SSD1306Display::drawRect(int x, int y, int w, int h) {
  display.drawRect(x, y, w, h, _color);
}

void SSD1306Display::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  display.drawBitmap(x, y, bits, w, h, _color);
}

uint16_t SSD1306Display::getTextWidth(const char* str) {
  bool utf8 = false;
  for (const char* p = str; *p; p++) {
    if ((uint8_t)*p >= 0x80) { utf8 = true; break; }
  }
  if (!utf8) {
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
    return w;
  }
  uint16_t w = 0;
  const char* p = str;
  while (*p) {
    uint32_t cp = 0;
    p = utf8Next(p, cp);
    if (cp == '\n') continue;
    int idx = cyrillicGlyphIndex(cp);
    int scale = _textSize < 1 ? 1 : _textSize;
    w += (idx >= 0 ? (int)cyrAdvance(CYR_OLED, idx) : 6) * scale;
  }
  return w;
}

void SSD1306Display::endFrame() {
  display.display();
}
