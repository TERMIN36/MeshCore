#include "E290Display.h"

#include "../../MeshCore.h"

// Color scheme
ColorVal UIColor::window_bkg = WHITE;
ColorVal UIColor::title_bkg = WHITE;
ColorVal UIColor::title_txt = BLACK;
ColorVal UIColor::primary_txt = BLACK;
ColorVal UIColor::secondary_txt = BLACK;
ColorVal UIColor::warning_txt = BLACK;
ColorVal UIColor::popup_bkg = WHITE;
ColorVal UIColor::popup_txt = BLACK;
ColorVal UIColor::corp_blue = BLACK;

bool E290Display::begin() {
  if (_init) return true;

  powerOn();
  display.begin();

  // Set to landscape mode rotated 180 degrees
  display.setRotation(3);

  _init = true;
  _isOn = true;

  clear();
  display.fastmodeOn(); // Enable fast mode for quicker (partial) updates

  return true;
}

void E290Display::powerOn() {
  if (_periph_power) {
    _periph_power->claim();
  } else {
#ifdef PIN_VEXT_EN
    pinMode(PIN_VEXT_EN, OUTPUT);
    digitalWrite(PIN_VEXT_EN, PIN_VEXT_EN_ACTIVE);
#endif
  }
  delay(50);                      // Allow power to stabilize
}

void E290Display::powerOff() {
  if (_periph_power) {
    _periph_power->release();
  } else {
#ifdef PIN_VEXT_EN
    digitalWrite(PIN_VEXT_EN, !PIN_VEXT_EN_ACTIVE); // Turn off power
#endif
  }
}

void E290Display::turnOn() {
  if (!_init) begin();
  else if (!_isOn) {
    powerOn();
    display.fastmodeOn();  // Reinitialize display controller after power was cut
  }
  _isOn = true;
}

void E290Display::turnOff() {
  if (_isOn) {
    powerOff();
    _isOn = false;
  }
}

void E290Display::clear() {
  display.clear();
}

void E290Display::startFrame(ColorVal bkg) {
  display_crc.reset();

  // Fill screen with white first to ensure clean background
  display.fillRect(0, 0, width(), height(), WHITE);
  if (bkg == 0) {
    // Fill with black if light background requested (inverted for e-ink)
    display.fillRect(0, 0, width(), height(), BLACK);
  }
  _color = UIColor::primary_txt;
  display.setTextColor(_color);
}

void E290Display::setTextSize(int sz) {
  display_crc.update<int>(sz);
  if (sz < 1) sz = 1;
  scale_x = scale_y = sz;
  // The library handles text size internally
  display.setTextSize(sz);
}

void E290Display::setColor(ColorVal c) {
  _color = c;
  display_crc.update<ColorVal>(c);
  display.setTextColor(_color);
}

void E290Display::setCursor(int x, int y) {
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  display.setCursor(x, y);
}

void E290Display::cyrAscii(int x, int y, uint8_t c) {
  display.drawChar(x, y, c, _color, _color, scale_x);
}

void E290Display::cyrFill(int x, int y, int w, int h) {
  display.fillRect(x, y, w, h, _color);
}

void E290Display::print(const char *str) {
  display_crc.update<char>(str, strlen(str));
  if (!hasUtf8(str)) {
    display.print(str);
    return;
  }
  pen_x = display.getCursorX();
  pen_y = display.getCursorY();
  wrap_px = width();
  cyrPrint(str);
  display.setCursor(pen_x, pen_y);
}

const char* E290Display::printWordWrap(const char *str, int max_width) {
  display_crc.update<char>(str, strlen(str));
  display_crc.update<int>(max_width);
  pen_x = display.getCursorX();
  pen_y = display.getCursorY();
  wrap_px = 0;
  int limit = width() - pen_x;
  const char* rest = cyrWordWrap(str, max_width < limit ? max_width : limit, height());
  display.setCursor(pen_x, pen_y);
  return rest;
}

void E290Display::translateUTF8ToBlocks(char *dest, const char *src, size_t dest_size) {
  translateUtf8KeepCyrillic(dest, src, dest_size);
}

void E290Display::fillRect(int x, int y, int w, int h) {
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  display_crc.update<int>(w);
  display_crc.update<int>(h);
  display.fillRect(x, y, w, h, _color);
}

void E290Display::drawRect(int x, int y, int w, int h) {
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  display_crc.update<int>(w);
  display_crc.update<int>(h);
  display.drawRect(x, y, w, h, _color);
}

void E290Display::drawXbm(int x, int y, const uint8_t *bits, int w, int h) {
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  display_crc.update<int>(w);
  display_crc.update<int>(h);
  display_crc.update<uint8_t>(bits, w * h / 8);

  // Width in bytes for bitmap processing
  uint16_t widthInBytes = (w + 7) / 8;

  // Process the bitmap row by row
  for (int by = 0; by < h; by++) {
    // Scan across the row bit by bit
    for (int bx = 0; bx < w; bx++) {
      // Get the current bit using MSB ordering (like GxEPDDisplay)
      uint16_t byteOffset = (by * widthInBytes) + (bx / 8);
      uint8_t bitMask = 0x80 >> (bx & 7);
      bool bitSet = bits[byteOffset] & bitMask;

      // If the bit is set, draw the pixel
      if (bitSet) {
        display.drawPixel(x + bx, y + by, _color);
      }
    }
  }
}

uint16_t E290Display::getTextWidth(const char *str) {
  if (hasUtf8(str)) return cyrWidth(str);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
  return w;
}

void E290Display::endFrame() {
  uint32_t crc = display_crc.finalize();
  if (crc != last_display_crc_value) {
    display.update();
    last_display_crc_value = crc;
  }
}
