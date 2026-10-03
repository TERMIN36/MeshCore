#pragma once

#include <helpers/CommonCLI.h>
#include <stddef.h>

// Joins the access point stored in NodePrefs. MQTT rides this link.
class WifiStation {
public:
  void begin(NodePrefs* prefs);
  void apply();
  // Applies NodePrefs::wifi_ps without rejoining the access point.
  void applyPower();
  // Mode that is running now. Light falls back to modem when the chip cannot sleep.
  uint8_t powerMode() const { return _power; }
  bool copyAddress(char* dest, size_t cap) const;

private:
  NodePrefs* _prefs = nullptr;
  uint8_t _power = WIFI_POWER_NONE;
};
