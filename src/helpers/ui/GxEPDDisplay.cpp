
#include "GxEPDDisplay.h"
#include "CyrillicFont.h"

#ifdef EXP_PIN_BACKLIGHT
  #include <PCA9557.h>
  extern PCA9557 expander;
#endif

#ifndef DISPLAY_ROTATION
  #define DISPLAY_ROTATION 3
#endif

#ifndef EINK_FULL_REFRESH_EVERY
  #define EINK_FULL_REFRESH_EVERY 20   // partial updates leave ghosting, a full refresh clears it
#endif
#ifndef EINK_CLEAN_BLACK_FIRST
  #define EINK_CLEAN_BLACK_FIRST 0
#endif

#ifdef ESP32
  SPIClass SPI1 = SPIClass(FSPI);
#endif

// Color scheme
ColorVal UIColor::window_bkg = GxEPD_WHITE;
ColorVal UIColor::title_bkg = GxEPD_WHITE;
ColorVal UIColor::title_txt = GxEPD_BLACK;
ColorVal UIColor::primary_txt = GxEPD_BLACK;
ColorVal UIColor::secondary_txt = GxEPD_BLACK;
ColorVal UIColor::warning_txt = GxEPD_BLACK;
ColorVal UIColor::popup_bkg = GxEPD_WHITE;
ColorVal UIColor::popup_txt = GxEPD_BLACK;
ColorVal UIColor::corp_blue = GxEPD_BLACK;


bool GxEPDDisplay::begin() {
  display.epd2.selectSPI(SPI1, SPISettings(4000000, MSBFIRST, SPI_MODE0));
#ifdef ESP32
  SPI1.begin(PIN_DISPLAY_SCLK, PIN_DISPLAY_MISO, PIN_DISPLAY_MOSI, PIN_DISPLAY_CS);
#else
  SPI1.begin();
#endif
  display.init(115200, true, 2, false);
  display.setRotation(DISPLAY_ROTATION);
  setTextSize(1);  // Default to size 1
  display.setPartialWindow(0, 0, display.width(), display.height());

  display.fillScreen(GxEPD_WHITE);
  cleanScreen();   // wipes the image left from before power-up
  #if DISP_BACKLIGHT
  digitalWrite(DISP_BACKLIGHT, LOW);
  pinMode(DISP_BACKLIGHT, OUTPUT);
  #endif
  _init = true;
  return true;
}

void GxEPDDisplay::clean() {
  cleanScreen();
  last_display_crc_value = 0;
}

// Full white fill (~3.5 s per full refresh on SSD1680 panels). Unlike
// display(false), which drives the panel through the inverse of the current
// image, this leaves both controller buffers white, so the next partial update
// redraws the whole frame instead of only the pixels that changed.
// EINK_CLEAN_BLACK_FIRST adds a black fill before it: cleaner, twice as slow.
void GxEPDDisplay::cleanScreen() {
#if EINK_CLEAN_BLACK_FIRST
  display.clearScreen(0x00);
#endif
  display.clearScreen(0xFF);
  _partial_updates = 0;
}

void GxEPDDisplay::turnOn() {
  if (!_init) begin();
#if defined(DISP_BACKLIGHT) && !defined(BACKLIGHT_BTN)
  digitalWrite(DISP_BACKLIGHT, HIGH);
#elif defined(EXP_PIN_BACKLIGHT) && !defined(BACKLIGHT_BTN)
  expander.digitalWrite(EXP_PIN_BACKLIGHT, HIGH);
#endif
  _isOn = true;
}

void GxEPDDisplay::turnOff() {
#if defined(DISP_BACKLIGHT) && !defined(BACKLIGHT_BTN)
  digitalWrite(DISP_BACKLIGHT, LOW);
#elif defined(EXP_PIN_BACKLIGHT) && !defined(BACKLIGHT_BTN)
  expander.digitalWrite(EXP_PIN_BACKLIGHT, LOW);
#endif
  _isOn = false;
}

void GxEPDDisplay::clear() {
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display_crc.reset();
}

void GxEPDDisplay::startFrame(ColorVal bkg) {
  display.fillScreen(bkg);
  display.setTextColor(_curr_color = UIColor::primary_txt);
  display_crc.reset();
}

const CyrBitmapFont& GxEPDDisplay::cyrFace() const {
  return _textSize >= 2 ? CYR_EINK2 : CYR_EINK;
}

void GxEPDDisplay::drawCyrGlyph(int idx, int x, int y) {
  const CyrBitmapFont& font = cyrFace();
  int w = idx >= 0 ? cyrAdvance(font, idx) : (font.height * 2) / 3;
  int h = font.height;
  if (w < 1) w = 1;
  int top = y - font.baseline;
  for (int row = 0; row < h; row++) {
    int run = -1;
    for (int col = 0; col <= w; col++) {
      bool on = col < w && (idx >= 0 ? cyrPixel(font, idx, col, row)
                                     : (row == 0 || row == h - 1 || col == 0 || col == w - 1));
      if (on) {
        if (run < 0) run = col;
      } else if (run >= 0) {
        display.fillRect(x + run, top + row, col - run, 1, _curr_color);
        run = -1;
      }
    }
  }
}

void GxEPDDisplay::setTextSize(int sz) {
  if (sz < 1) sz = 1;
  _textSize = sz;
  display_crc.update<int>(sz);
  switch(sz) {
    case 1:  // Small
      display.setFont(&FreeSans9pt7b);
      break;
    case 2:  // Medium Bold
      display.setFont(&FreeSansBold12pt7b);
      break;
    case 3:  // Large
      display.setFont(&FreeSans18pt7b);
      break;
    default:
      display.setFont(&FreeSans9pt7b);
      break;
  }
}

void GxEPDDisplay::setColor(ColorVal c) {
  display_crc.update<ColorVal> (c);
  display.setTextColor(_curr_color = c);
}

static const GFXfont* latinFace(int textSize) {
  switch (textSize) {
    case 2: return &FreeSansBold12pt7b;
    case 3: return &FreeSans18pt7b;
    default: return &FreeSans9pt7b;
  }
}

static int capHeightPx(const GFXfont* font) {
  GFXfont face;
  memcpy_P(&face, font, sizeof(face));
  GFXglyph glyph;
  memcpy_P(&glyph, face.glyph + ('H' - face.first), sizeof(glyph));
  return -glyph.yOffset;
}

// EINK_Y_OFFSET places the size 1 baseline; larger faces drop by their extra cap
// height so that y stays the top of the text for every size.
int GxEPDDisplay::baselinePx(int y) const {
  return (int)((y + offset_y) * scale_y) + capHeightPx(latinFace(_textSize)) - capHeightPx(&FreeSans9pt7b);
}

void GxEPDDisplay::setCursor(int x, int y) {
  _lx = x;
  _ly = y;
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  display.setCursor((x+offset_x)*scale_x, baselinePx(y));
}

static int latinAdvance(const GFXfont* font, uint32_t cp) {
  if (cp < 32 || cp > 255) return 0;
  GFXfont face;
  memcpy_P(&face, font, sizeof(face));
  if ((uint16_t)cp < face.first || (uint16_t)cp > face.last) return 0;
  GFXglyph glyph;
  memcpy_P(&glyph, face.glyph + ((uint16_t)cp - face.first), sizeof(glyph));
  return glyph.xAdvance;
}

int GxEPDDisplay::codepointWidthPx(uint32_t cp) const {
  if (cp < 32 || cp == '\n') return 0;
  int idx = cyrillicGlyphIndex(cp);
  if (idx >= 0) return cyrAdvance(cyrFace(), idx);
  if (cp < 0x80) return latinAdvance(latinFace(_textSize), cp);
  return (cyrFace().height * 2) / 3;
}

int GxEPDDisplay::textLineStep() const {
  GFXfont face;
  memcpy_P(&face, latinFace(_textSize), sizeof(face));
  int phys = face.yAdvance;
  int cyr = (int)cyrFace().height + 2;
  if (cyr > phys) phys = cyr;
  if (phys < 1) phys = 1;
  int step = (int)(phys / scale_y);
  if ((float)step * scale_y < (float)phys - 0.01f) step++;
  if (step < 1) step = 1;
  return step;
}

void GxEPDDisplay::printSpan(const char* begin, const char* end) {
  if (!begin || end < begin) return;
  display_crc.update<char>(begin, (size_t)(end - begin));
  const char* p = begin;
  while (p < end && *p) {
    if ((uint8_t)*p < 0x80) {
      char buf[64];
      int n = 0;
      while (p < end && *p && (uint8_t)*p < 0x80 && n < (int)sizeof(buf) - 1) {
        buf[n++] = *p++;
      }
      buf[n] = 0;
      display.print(buf);
    } else {
      uint32_t cp = 0;
      const char* next = utf8Next(p, cp);
      if (next > end) next = end;
      int idx = cyrillicGlyphIndex(cp);
      int x = display.getCursorX();
      int y = display.getCursorY();
      drawCyrGlyph(idx, x, y);
      int adv = idx >= 0 ? cyrAdvance(cyrFace(), idx) : (cyrFace().height * 2) / 3;
      display.setCursor(x + adv, y);
      p = next;
    }
  }
}

void GxEPDDisplay::print(const char* str) {
  printSpan(str, str + strlen(str));
}

const char* GxEPDDisplay::printWordWrap(const char* str, int max_width) {
  int origin = _lx;
  int y = _ly;
  int origin_px = (int)((origin + offset_x) * scale_x);
  int limit = display.width() - origin_px;
  if (limit < 1) limit = 1;
  int max_px = (int)(max_width * scale_x + 0.5f);
  if (max_px > limit) max_px = limit;
  if (max_px < 1) max_px = 1;
  int step = textLineStep();
  int descent = (int)cyrFace().height - (int)cyrFace().baseline;
  if (descent < 0) descent = 0;
  // Lines are already split to the panel width. Leave GFX wrap off so a
  // one-pixel rounding difference cannot insert an extra break mid-line.
  display.setTextWrap(false);

  bool first = true;
  const char* p = str;
  while (*p) {
    int baseline = baselinePx(y);
    if (!first && (baseline < 0 || baseline + descent >= display.height())) break;

    const char* line = p;
    int width = 0;
    const char* brk = nullptr;
    const char* end = p;
    while (*end && *end != '\n') {
      uint32_t cp = 0;
      const char* next = utf8Next(end, cp);
      int cw = codepointWidthPx(cp);
      if (width + cw > max_px && end != line) break;
      width += cw;
      if (cp == ' ' || cp == '-' || cp == '/') brk = next;
      end = next;
    }
    const char* cut = end;
    if (*end && *end != '\n' && brk && brk > line) cut = brk;
    setCursor(origin, y);
    printSpan(line, cut);
    first = false;
    if (*cut == '\n' || *cut == ' ') cut++;
    if (cut == p) break;
    p = cut;
    if (*p) y += step;
  }
  display.setTextWrap(true);
  _lx = origin;
  _ly = y;
  return p;
}

void GxEPDDisplay::translateUTF8ToBlocks(char* dest, const char* src, size_t dest_size) {
  translateUtf8KeepCyrillic(dest, src, dest_size);
}

void GxEPDDisplay::fillRect(int x, int y, int w, int h) {
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  display_crc.update<int>(w);
  display_crc.update<int>(h);
  display.fillRect(x*scale_x, y*scale_y, w*scale_x, h*scale_y, _curr_color);
}

void GxEPDDisplay::drawRect(int x, int y, int w, int h) {
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  display_crc.update<int>(w);
  display_crc.update<int>(h);
  display.drawRect(x*scale_x, y*scale_y, w*scale_x, h*scale_y, _curr_color);
}

void GxEPDDisplay::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  display_crc.update<int>(w);
  display_crc.update<int>(h);
  display_crc.update<uint8_t>(bits, w * h / 8);
  // Calculate the base position in display coordinates
  uint16_t startX = x * scale_x;
  uint16_t startY = y * scale_y;
  
  // Width in bytes for bitmap processing
  uint16_t widthInBytes = (w + 7) / 8;
  
  // Process the bitmap row by row
  for (uint16_t by = 0; by < h; by++) {
    // Calculate the target y-coordinates for this logical row
    int y1 = startY + (int)(by * scale_y);
    int y2 = startY + (int)((by + 1) * scale_y);
    int block_h = y2 - y1;
    
    // Scan across the row bit by bit
    for (uint16_t bx = 0; bx < w; bx++) {
      // Calculate the target x-coordinates for this logical column
      int x1 = startX + (int)(bx * scale_x);
      int x2 = startX + (int)((bx + 1) * scale_x);
      int block_w = x2 - x1;
      
      // Get the current bit
      uint16_t byteOffset = (by * widthInBytes) + (bx / 8);
      uint8_t bitMask = 0x80 >> (bx & 7);
      bool bitSet = pgm_read_byte(bits + byteOffset) & bitMask;
      
      // If the bit is set, draw a block of pixels
      if (bitSet) {
        // Draw the block as a filled rectangle
        display.fillRect(x1, y1, block_w, block_h, _curr_color);
      }
    }
  }
}

int GxEPDDisplay::frameWidth() { return display.width(); }

int GxEPDDisplay::frameHeight() { return display.height(); }

void GxEPDDisplay::blit1(int x, int y, int w, int h, const uint8_t* bits) {
  display_crc.update<int>(x);
  display_crc.update<int>(y);
  display_crc.update<int>(w);
  display_crc.update<int>(h);
  if (w > 0 && h > 0) display_crc.update<uint8_t>(bits, ((w + 7) / 8) * h);
  uint16_t widthInBytes = (w + 7) / 8;
  for (int by = 0; by < h; by++) {
    int run = -1;
    for (int bx = 0; bx <= w; bx++) {
      bool on = false;
      if (bx < w) {
        uint8_t b = pgm_read_byte(bits + by * widthInBytes + (bx >> 3));
        on = (b & (uint8_t)(0x80 >> (bx & 7))) != 0;
      }
      if (on) {
        if (run < 0) run = bx;
      } else if (run >= 0) {
        display.fillRect(x + run, y + by, bx - run, 1, _curr_color);
        run = -1;
      }
    }
  }
}

uint16_t GxEPDDisplay::getTextWidth(const char* str) {
  bool utf8 = false;
  for (const char* q = str; *q; q++) {
    if ((uint8_t)*q >= 0x80) { utf8 = true; break; }
  }
  if (!utf8) {
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
    return ceil((w + 1) / scale_x);
  }
  // Ink bounds of Latin runs would drop spaces; sum advances as printSpan() moves the cursor.
  int px = 0;
  const char* p = str;
  while (*p) {
    uint32_t cp = 0;
    p = utf8Next(p, cp);
    px += codepointWidthPx(cp);
  }
  return ceil((px + 1) / scale_x);
}

void GxEPDDisplay::endFrame() {
  uint32_t crc = display_crc.finalize();
  if (crc != last_display_crc_value) {
    if (++_partial_updates >= EINK_FULL_REFRESH_EVERY) cleanScreen();
    display.display(true);
    last_display_crc_value = crc;
  }
}
