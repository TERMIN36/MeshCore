#pragma once

#include <stdint.h>

#ifndef LORA_FREQ
#define LORA_FREQ 869.618
#endif
#ifndef LORA_BW
#define LORA_BW 62.5
#endif
#ifndef LORA_SF
#define LORA_SF 8
#endif
#ifndef LORA_CR
#define LORA_CR 5
#endif

// Named radio profiles. "Normal" is the network this node was on before a
// special profile. The first switch away remembers those parameters. If nothing
// was remembered, Normal falls back to the build preset.
// Long and HiNoise share the repeat whitelist frequency so companions may repeat.
// HiNoise uses a narrow channel and a stronger code for wideband noise.

#define RADIO_PROFILE_LONG_FREQ 869.495f
#define RADIO_PROFILE_LONG_BW   62.5f
#define RADIO_PROFILE_LONG_SF   11
#define RADIO_PROFILE_LONG_CR   5

#define RADIO_PROFILE_NOISE_FREQ 869.495f
#define RADIO_PROFILE_NOISE_BW   31.25f
#define RADIO_PROFILE_NOISE_SF   10
#define RADIO_PROFILE_NOISE_CR   7

enum RadioProfileId : uint8_t {
  RADIO_PROFILE_CUSTOM = 0,
  RADIO_PROFILE_NORMAL = 1,
  RADIO_PROFILE_LONG = 2,
  RADIO_PROFILE_HINOISE = 3,
};

inline bool radioNear(float a, float b) {
  float d = a - b;
  if (d < 0) d = -d;
  return d < 0.02f;
}

inline bool radioIsLong(float freq, float bw, uint8_t sf, uint8_t cr) {
  return radioNear(freq, RADIO_PROFILE_LONG_FREQ) && radioNear(bw, RADIO_PROFILE_LONG_BW) &&
         sf == RADIO_PROFILE_LONG_SF && cr == RADIO_PROFILE_LONG_CR;
}

inline bool radioIsHiNoise(float freq, float bw, uint8_t sf, uint8_t cr) {
  return radioNear(freq, RADIO_PROFILE_NOISE_FREQ) && radioNear(bw, RADIO_PROFILE_NOISE_BW) &&
         sf == RADIO_PROFILE_NOISE_SF && cr == RADIO_PROFILE_NOISE_CR;
}

inline bool radioMatchesSaved(float freq, float bw, uint8_t sf, uint8_t cr,
                              float home_freq, float home_bw, uint8_t home_sf, uint8_t home_cr) {
  if (!(home_freq >= 150.0f && home_bw >= 7.0f && home_sf >= 5 && home_cr >= 5)) return false;
  return radioNear(freq, home_freq) && radioNear(bw, home_bw) && sf == home_sf && cr == home_cr;
}

inline uint8_t radioProfileOf(float freq, float bw, uint8_t sf, uint8_t cr,
                              float home_freq, float home_bw, uint8_t home_sf, uint8_t home_cr) {
  if (radioIsHiNoise(freq, bw, sf, cr)) return RADIO_PROFILE_HINOISE;
  if (radioIsLong(freq, bw, sf, cr)) return RADIO_PROFILE_LONG;
  if (radioMatchesSaved(freq, bw, sf, cr, home_freq, home_bw, home_sf, home_cr)) return RADIO_PROFILE_NORMAL;
  bool saved = home_freq >= 150.0f && home_bw >= 7.0f && home_sf >= 5 && home_cr >= 5;
  if (!saved && radioNear(freq, (float)LORA_FREQ) && radioNear(bw, (float)LORA_BW) &&
      sf == (uint8_t)LORA_SF && cr == (uint8_t)LORA_CR) {
    return RADIO_PROFILE_NORMAL;
  }
  return RADIO_PROFILE_CUSTOM;
}

inline uint8_t radioNextProfile(uint8_t current) {
  if (current == RADIO_PROFILE_LONG) return RADIO_PROFILE_HINOISE;
  if (current == RADIO_PROFILE_HINOISE) return RADIO_PROFILE_NORMAL;
  return RADIO_PROFILE_LONG;
}

// Returns true when a companion should repeat (Long and HiNoise).
inline bool applyRadioProfileChoice(uint8_t which, float& freq, float& bw, uint8_t& sf, uint8_t& cr,
                                   float& home_freq, float& home_bw, uint8_t& home_sf, uint8_t& home_cr) {
  if (which == RADIO_PROFILE_LONG || which == RADIO_PROFILE_HINOISE) {
    if (!radioIsLong(freq, bw, sf, cr) && !radioIsHiNoise(freq, bw, sf, cr)) {
      home_freq = freq;
      home_bw = bw;
      home_sf = sf;
      home_cr = cr;
    }
    if (which == RADIO_PROFILE_HINOISE) {
      freq = RADIO_PROFILE_NOISE_FREQ;
      bw = RADIO_PROFILE_NOISE_BW;
      sf = RADIO_PROFILE_NOISE_SF;
      cr = RADIO_PROFILE_NOISE_CR;
    } else {
      freq = RADIO_PROFILE_LONG_FREQ;
      bw = RADIO_PROFILE_LONG_BW;
      sf = RADIO_PROFILE_LONG_SF;
      cr = RADIO_PROFILE_LONG_CR;
    }
    return true;
  }
  if (home_freq >= 150.0f && home_bw >= 7.0f && home_sf >= 5 && home_cr >= 5) {
    freq = home_freq;
    bw = home_bw;
    sf = home_sf;
    cr = home_cr;
  } else {
    freq = (float)LORA_FREQ;
    bw = (float)LORA_BW;
    sf = (uint8_t)LORA_SF;
    cr = (uint8_t)LORA_CR;
  }
  return false;
}

inline const char* radioProfileLabel(uint8_t id) {
  if (id == RADIO_PROFILE_LONG) return "Long";
  if (id == RADIO_PROFILE_HINOISE) return "HiNoise";
  if (id == RADIO_PROFILE_NORMAL) return "Normal";
  return "Custom";
}

inline const char* radioProfileAlert(uint8_t id) {
  if (id == RADIO_PROFILE_LONG) return "Long range";
  if (id == RADIO_PROFILE_HINOISE) return "HiNoise";
  return "Normal";
}
