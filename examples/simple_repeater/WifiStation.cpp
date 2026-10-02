#include "WifiStation.h"

#include <helpers/CommonCLI.h>
#include <stdio.h>
#include <string.h>

#if defined(ESP32)
#include <WiFi.h>
#include <esp_wifi.h>
#endif

#if defined(ESP32)
static void wifiCpu(bool on) {
#ifdef ESP32_CPU_FREQ
  uint32_t mhz = ESP32_CPU_FREQ;
  if (on && mhz < 160) mhz = 160;
  if (getCpuFrequencyMhz() != mhz) setCpuFrequencyMhz(mhz);
#else
  if (on && getCpuFrequencyMhz() < 160) setCpuFrequencyMhz(160);
#endif
}
#endif

void WifiStation::begin(NodePrefs* prefs) {
  _prefs = prefs;
  apply();
}

void WifiStation::apply() {
#if defined(ESP32)
  if (!_prefs) return;
  if (!_prefs->wifi_enabled || !_prefs->wifi_ssid[0]) {
    WiFi.disconnect(true);
#ifndef WITH_ESPNOW_BRIDGE
    WiFi.mode(WIFI_OFF);
    wifiCpu(false);
#endif
    return;
  }
  // 80 MHz leaves the association up (ping answers) while TCP sessions rot.
  wifiCpu(true);
  // Set the sleep flag before STA starts, or the start event applies modem sleep.
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (_prefs->wifi_wpass[0]) WiFi.begin(_prefs->wifi_ssid, _prefs->wifi_wpass);
  else WiFi.begin(_prefs->wifi_ssid);
  esp_wifi_set_ps(WIFI_PS_NONE);
#else
  (void)_prefs;
#endif
}

bool WifiStation::copyAddress(char* dest, size_t cap) const {
  if (!dest || cap == 0) return false;
  dest[0] = 0;
#if defined(ESP32)
  if (WiFi.status() != WL_CONNECTED) return false;
  IPAddress ip = WiFi.localIP();
  if (ip == IPAddress((uint32_t)0)) return false;
  snprintf(dest, cap, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return dest[0] != 0;
#else
  return false;
#endif
}
