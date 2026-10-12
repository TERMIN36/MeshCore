#pragma once

#include <stddef.h>
#include <stdint.h>

#define LINK_MODE_BLE   0
#define LINK_MODE_WIFI  1
#define LINK_MODE_OFF   2

// Bring up the saved phone link. Bluetooth and Wi-Fi are not left on together.
void companionApplyLink();
// True while a Wi-Fi station is up and the CPU must stay awake.
bool companionLinkHoldsCpu();
bool companionWifiAddress(char* dest, size_t cap);
// Any phone frame or an open screen restarts the idle curve.
void companionNoteLinkActivity();
// 100 ms growing smoothly to 800 ms over 20 s of quiet.
uint32_t companionIdlePeriodMs();
