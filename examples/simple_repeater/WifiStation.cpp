#include "WifiStation.h"

#include <stdio.h>
#include <string.h>

#if defined(ESP32)
#include <WiFi.h>
#include <esp_pm.h>
#include <esp_wifi.h>
#endif

#if defined(ESP32)
static bool pm_used = false;

static void wifiCpu(bool on) {
#ifdef ESP32_CPU_FREQ
  uint32_t mhz = ESP32_CPU_FREQ;
  if (on && mhz < 160) mhz = 160;
  if (getCpuFrequencyMhz() != mhz) setCpuFrequencyMhz(mhz);
#else
  if (on && getCpuFrequencyMhz() < 160) setCpuFrequencyMhz(160);
#endif
}

static uint32_t pinnedMhz() {
  wifiCpu(true);
  uint32_t mhz = getCpuFrequencyMhz();
  if (mhz < 160) mhz = 160;
  return mhz;
}

static bool pmSet(uint32_t max_mhz, uint32_t min_mhz, bool light) {
#if defined(CONFIG_IDF_TARGET_ESP32)
  esp_pm_config_esp32_t cfg = {};
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
  esp_pm_config_esp32s2_t cfg = {};
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  esp_pm_config_esp32s3_t cfg = {};
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  esp_pm_config_esp32c3_t cfg = {};
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  esp_pm_config_esp32c6_t cfg = {};
#elif defined(CONFIG_IDF_TARGET_ESP32H2)
  esp_pm_config_esp32h2_t cfg = {};
#else
  (void)max_mhz;
  (void)min_mhz;
  (void)light;
  return false;
#endif
#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || \
    defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
  cfg.max_freq_mhz = (int)max_mhz;
  cfg.min_freq_mhz = (int)min_mhz;
  cfg.light_sleep_enable = light;
  esp_err_t err = esp_pm_configure(&cfg);
  if (light && err == ESP_ERR_INVALID_ARG && min_mhz < 80) return pmSet(max_mhz, 80, true);
  return err == ESP_OK;
#else
  return false;
#endif
}

static uint8_t savedMode(const NodePrefs* prefs) {
  if (!prefs) return WIFI_POWER_NONE;
  if (prefs->wifi_ps == WIFI_POWER_MODEM || prefs->wifi_ps == WIFI_POWER_LIGHT) return prefs->wifi_ps;
  return WIFI_POWER_NONE;
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
    applyPower();
    return;
  }
  // 80 MHz leaves the association up (ping answers) while TCP sessions rot.
  wifiCpu(true);
  // Set the sleep flag before STA starts, or the start event applies modem sleep.
  WiFi.setSleep(savedMode(_prefs) != WIFI_POWER_NONE);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (_prefs->wifi_wpass[0]) WiFi.begin(_prefs->wifi_ssid, _prefs->wifi_wpass);
  else WiFi.begin(_prefs->wifi_ssid);
  applyPower();
#else
  (void)_prefs;
#endif
}

void WifiStation::applyPower() {
#if defined(ESP32)
  if (!_prefs) return;
  uint8_t mode = savedMode(_prefs);
  bool station = _prefs->wifi_enabled && _prefs->wifi_ssid[0];
  if (!station) {
    if (pm_used) {
#ifdef ESP32_CPU_FREQ
      uint32_t mhz = ESP32_CPU_FREQ;
      if (mhz < 80) mhz = 80;
#else
      uint32_t mhz = 160;
#endif
      pmSet(mhz, mhz, false);
      pm_used = false;
    }
#ifndef WITH_ESPNOW_BRIDGE
    wifiCpu(false);
    WiFi.setSleep(WIFI_PS_NONE);
#endif
    _power = WIFI_POWER_NONE;
    return;
  }

  bool modem = mode != WIFI_POWER_NONE;
  WiFi.setSleep(modem ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
  if ((WiFi.getMode() & WIFI_MODE_STA) != 0) {
    esp_wifi_set_ps(modem ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
  }

  if (mode == WIFI_POWER_LIGHT) {
    pm_used = true;
    if (pmSet(160, 40, true)) {
      _power = WIFI_POWER_LIGHT;
      Serial.println("WiFi power: modem + light sleep");
      return;
    }
    mode = WIFI_POWER_MODEM;
    modem = true;
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    if ((WiFi.getMode() & WIFI_MODE_STA) != 0) esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    Serial.println("WiFi power: light sleep unsupported, modem only");
  }

  if (mode == WIFI_POWER_MODEM) {
    // Modem sleep drops the WiFi CPU-max lock. Pin the clock so TCP does not
    // fall back to the 80 MHz rate that leaves the association up and rots sockets.
    pm_used = true;
    uint32_t mhz = pinnedMhz();
    pmSet(mhz, mhz, false);
    _power = WIFI_POWER_MODEM;
    Serial.println("WiFi power: modem sleep");
    return;
  }

  if (pm_used) {
    uint32_t mhz = pinnedMhz();
    pmSet(mhz, mhz, false);
  }
  wifiCpu(true);
  _power = WIFI_POWER_NONE;
#else
  _power = WIFI_POWER_NONE;
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
