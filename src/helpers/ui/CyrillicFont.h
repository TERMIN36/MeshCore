#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Proportional 1-bit Cyrillic. Index: А-Я (0..31), Ё (32), а-я (33..64), ё (65).
// OLED face is Tahoma Bold, sized to the 11px UI row. TFT faces match ArialMT.

struct CyrBitmapFont {
  uint8_t height;
  uint8_t baseline;
  const uint8_t* advance;
  const uint16_t* offset;
  const uint8_t* bits;
};

#include "CyrillicGlyphs.inc"

static const CyrBitmapFont CYR_OLED = {
  CYR_OLED_H, CYR_OLED_BASE, CYR_OLED_ADV, CYR_OLED_OFF, CYR_OLED_BITS
};
static const CyrBitmapFont CYR_TFT = {
  CYR_TFT_H, CYR_TFT_BASE, CYR_TFT_ADV, CYR_TFT_OFF, CYR_TFT_BITS
};
static const CyrBitmapFont CYR_TFT2 = {
  CYR_TFT2_H, CYR_TFT2_BASE, CYR_TFT2_ADV, CYR_TFT2_OFF, CYR_TFT2_BITS
};
static const CyrBitmapFont CYR_EINK = {
  CYR_EINK_H, CYR_EINK_BASE, CYR_EINK_ADV, CYR_EINK_OFF, CYR_EINK_BITS
};
static const CyrBitmapFont CYR_EINK2 = {
  CYR_EINK2_H, CYR_EINK2_BASE, CYR_EINK2_ADV, CYR_EINK2_OFF, CYR_EINK2_BITS
};

inline int cyrillicGlyphIndex(uint32_t cp) {
  if (cp >= 0x0410 && cp <= 0x042F) return (int)(cp - 0x0410);
  if (cp == 0x0401) return 32;
  if (cp >= 0x0430 && cp <= 0x044F) return 33 + (int)(cp - 0x0430);
  if (cp == 0x0451) return 65;
  return -1;
}

inline uint8_t cyrAdvance(const CyrBitmapFont& font, int idx) {
  return pgm_read_byte(font.advance + idx);
}

inline bool cyrPixel(const CyrBitmapFont& font, int idx, int x, int y) {
  uint8_t w = cyrAdvance(font, idx);
  if ((unsigned)x >= w || (unsigned)y >= font.height) return false;
  uint16_t off = pgm_read_word(font.offset + idx);
  uint8_t rowb = (w + 7) >> 3;
  uint8_t b = pgm_read_byte(font.bits + off + (uint16_t)y * rowb + (x >> 3));
  return (b & (uint8_t)(0x80 >> (x & 7))) != 0;
}

inline const char* utf8Next(const char* s, uint32_t& cp) {
  const uint8_t c = (uint8_t)s[0];
  if (c == 0) return s;
  if (c < 0x80) {
    cp = c;
    return s + 1;
  }
  if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
    cp = ((uint32_t)(c & 0x1F) << 6) | (uint8_t)(s[1] & 0x3F);
    return s + 2;
  }
  if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
    cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)((uint8_t)s[1] & 0x3F) << 6) | (uint8_t)(s[2] & 0x3F);
    return s + 3;
  }
  if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
    cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)((uint8_t)s[1] & 0x3F) << 12) |
         ((uint32_t)((uint8_t)s[2] & 0x3F) << 6) | (uint8_t)(s[3] & 0x3F);
    return s + 4;
  }
  cp = c;
  return s + 1;
}

inline void translateUtf8KeepCyrillic(char* dest, const char* src, size_t dest_size) {
  size_t j = 0;
  const char* p = src;
  while (*p && j + 1 < dest_size) {
    const char* start = p;
    uint32_t cp = 0;
    p = utf8Next(p, cp);
    if (cp >= 32 && cp <= 126) {
      dest[j++] = (char)cp;
    } else if (cyrillicGlyphIndex(cp) >= 0) {
      size_t n = (size_t)(p - start);
      if (j + n >= dest_size) break;
      memcpy(dest + j, start, n);
      j += n;
    } else if (cp >= 0x80) {
      dest[j++] = '\xDB';
    }
  }
  dest[j] = 0;
}
