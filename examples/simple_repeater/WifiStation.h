#pragma once

#include <helpers/CommonCLI.h>
#include <stddef.h>
#include <stdint.h>

// Joins the access point stored in NodePrefs. MQTT rides this link.
class WifiStation {
public:
  void begin(NodePrefs* prefs);
  void apply();
  // Applies NodePrefs::wifi_ps without rejoining the access point.
  void applyPower();
  // Joins again after the access point drops. Does not change sleep or stop the driver.
  void maintain();
  // Mode that is running now. Light falls back to modem when the chip cannot sleep.
  uint8_t powerMode() const { return _power; }
  bool copyAddress(char* dest, size_t cap) const;
  // Associated AP signal, in dBm. False when the station is down or the value is not a real RSSI.
  bool copyRssi(int8_t& dbm) const;

private:
  void armJoinGrace();

  NodePrefs* _prefs = nullptr;
  uint8_t _power = WIFI_POWER_NONE;
  uint32_t _down_since = 0;
  uint32_t _next_join = 0;
};
