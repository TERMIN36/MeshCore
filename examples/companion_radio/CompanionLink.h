#pragma once

#include <stddef.h>
#include <stdint.h>

#define LINK_MODE_BLE   0
#define LINK_MODE_WIFI  1
#define LINK_MODE_OFF   2
#define LINK_MODE_USB   3

// Stored Wi-Fi on a board without it, and stored BLE on a USB-only build,
// still name a channel this firmware can open.
inline uint8_t companionEffectiveLinkMode(uint8_t mode) {
#if !defined(BLE_PIN_CODE)
  if (mode != LINK_MODE_OFF && mode != LINK_MODE_USB) return LINK_MODE_USB;
#elif !defined(ESP32)
  if (mode == LINK_MODE_WIFI) return LINK_MODE_BLE;
#endif
  return mode;
}

inline uint8_t companionNextLinkMode(uint8_t mode) {
  mode = companionEffectiveLinkMode(mode);
#if defined(ESP32) && defined(BLE_PIN_CODE)
  if (mode == LINK_MODE_BLE) return LINK_MODE_WIFI;
  if (mode == LINK_MODE_WIFI) return LINK_MODE_USB;
  if (mode == LINK_MODE_USB) return LINK_MODE_OFF;
  return LINK_MODE_BLE;
#elif defined(BLE_PIN_CODE)
  if (mode == LINK_MODE_BLE) return LINK_MODE_USB;
  if (mode == LINK_MODE_USB) return LINK_MODE_OFF;
  return LINK_MODE_BLE;
#else
  return mode == LINK_MODE_USB ? LINK_MODE_OFF : LINK_MODE_USB;
#endif
}

// Bring up the saved phone link. Only one of Bluetooth, Wi-Fi, and USB stays up.
void companionApplyLink();
// True when the active phone link has a live session. USB and Wi-Fi do not count while BLE is selected.
bool companionPhoneConnected();
// True while a Wi-Fi station is up and the CPU must stay awake.
bool companionLinkHoldsCpu();
bool companionWifiAddress(char* dest, size_t cap);
// Any phone frame or an open screen restarts the idle curve.
void companionNoteLinkActivity();
// 100 ms growing smoothly to 800 ms over 20 s of quiet.
uint32_t companionIdlePeriodMs();
