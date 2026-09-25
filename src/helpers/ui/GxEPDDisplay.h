#pragma once

#include <SPI.h>
#include <Wire.h>

#define ENABLE_GxEPD2_GFX 0

#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <GxEPD2_4C.h>
#include <GxEPD2_7C.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <CRC32.h>

#include "DisplayDriver.h"

struct CyrBitmapFont;

// Logical grid height the UI lays out against; EINK_SCALE_Y maps it onto the panel.
#ifndef EINK_LOGICAL_HEIGHT
  #define EINK_LOGICAL_HEIGHT 128
#endif

class GxEPDDisplay : public DisplayDriver {

#if defined(EINK_DISPLAY_MODEL)
  GxEPD2_BW<EINK_DISPLAY_MODEL, EINK_DISPLAY_MODEL::HEIGHT> display;
  const float scale_x  = EINK_SCALE_X; 
  const float scale_y  = EINK_SCALE_Y;
  const float offset_x = EINK_X_OFFSET;
  const float offset_y = EINK_Y_OFFSET;
#else
  GxEPD2_BW<GxEPD2_150_BN, 200> display;
  const float scale_x  = 1.5625f;
  const float scale_y  = 1.5625f;
  const float offset_x = 0;
  const float offset_y = 10;
#endif
  bool _init = false;
  bool _isOn = false;
  int _textSize = 1;
  int _lx = 0, _ly = 0;
  uint16_t _curr_color;

  void drawCyrGlyph(int idx, int x, int y);
  const CyrBitmapFont& cyrFace() const;
  void printSpan(const char* begin, const char* end);
  int codepointWidthPx(uint32_t cp) const;
  int textLineStep() const;
  int baselinePx(int y) const;
  void cleanScreen();
  CRC32 display_crc;
  int last_display_crc_value = 0;
  uint16_t _partial_updates = 0;   // since the last full refresh

public:
#if defined(EINK_DISPLAY_MODEL)
  GxEPDDisplay() : DisplayDriver(128, EINK_LOGICAL_HEIGHT), display(EINK_DISPLAY_MODEL(PIN_DISPLAY_CS, PIN_DISPLAY_DC, PIN_DISPLAY_RST, PIN_DISPLAY_BUSY)) {}
#else
  GxEPDDisplay() : DisplayDriver(128, 128), display(GxEPD2_150_BN(DISP_CS, DISP_DC, DISP_RST, DISP_BUSY)) {}
#endif

  bool begin();

  bool isOn() override { return _isOn; }
  bool isEink() override { return true; }
  void clean() override;
  void turnOn() override;
  void turnOff() override;
  void clear() override;
  void startFrame(ColorVal bkg = UIColor::window_bkg) override;
  void setTextSize(int sz) override;
  void setColor(ColorVal c) override;
  void setCursor(int x, int y) override;
  void print(const char* str) override;
  const char* printWordWrap(const char* str, int max_width) override;
  void translateUTF8ToBlocks(char* dest, const char* src, size_t dest_size) override;
  void fillRect(int x, int y, int w, int h) override;
  void drawRect(int x, int y, int w, int h) override;
  void drawXbm(int x, int y, const uint8_t* bits, int w, int h) override;
  int frameWidth() override;
  int frameHeight() override;
  void blit1(int x, int y, int w, int h, const uint8_t* bits) override;
  uint16_t getTextWidth(const char* str) override;
  void endFrame() override;
};
