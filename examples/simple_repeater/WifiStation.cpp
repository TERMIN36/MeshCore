#include "WifiStation.h"

#include <stdio.h>
#include <string.h>
#include <helpers/ESP32Board.h>

#if defined(ESP32)
#include <WiFi.h>
#include <esp_pm.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/syscon_reg.h>
#if __has_include(<soc/system_reg.h>)
#include <soc/system_reg.h>
#else
#include <soc/dport_reg.h>
#include <soc/dport_access.h>
#define SYSTEM_WIFI_CLK_EN_REG DPORT_WIFI_CLK_EN_REG
#define SYSTEM_WIFI_CLK_WIFI_BT_COMMON_M DPORT_WIFI_CLK_WIFI_BT_COMMON_M
#define SYSTEM_BT_BASEBAND_EN DPORT_BT_BASEBAND_EN
#define SYSTEM_BT_LC_EN DPORT_BT_LC_EN
// Classic ESP32 keeps Wi-Fi and Bluetooth in one power domain.
#ifndef RTC_CNTL_BT_FORCE_PD
#define RTC_CNTL_BT_FORCE_PD 0
#define RTC_CNTL_BT_FORCE_PU 0
#define RTC_CNTL_BT_FORCE_ISO 0
#define RTC_CNTL_BT_FORCE_NOISO 0
#endif
#endif

static inline void wifiClkSet(uint32_t mask) {
#if defined(CONFIG_IDF_TARGET_ESP32)
  DPORT_SET_PERI_REG_MASK(SYSTEM_WIFI_CLK_EN_REG, mask);
#else
  REG_SET_BIT(SYSTEM_WIFI_CLK_EN_REG, mask);
#endif
}

static inline void wifiClkClr(uint32_t mask) {
#if defined(CONFIG_IDF_TARGET_ESP32)
  DPORT_CLEAR_PERI_REG_MASK(SYSTEM_WIFI_CLK_EN_REG, mask);
#else
  REG_CLR_BIT(SYSTEM_WIFI_CLK_EN_REG, mask);
#endif
}

extern "C" {
void phy_get_romfunc_addr(void);
void phy_close_rf(void);
#if !defined(CONFIG_IDF_TARGET_ESP32)
void phy_xpd_tsens(void);
#endif
}
#if !defined(CONFIG_FREERTOS_UNICORE)
#include <esp_ipc.h>
#endif
#endif

#if defined(ESP32)
static bool pm_used = false;
static bool pm_configured = false;
// Set only after this process starts STA. A later boot with Wi-Fi already
// saved off must not restart again: that was the main-screen / dead-button loop.
static bool sta_started = false;
static esp_pm_lock_handle_t cpu_max_lock = nullptr;

#if !defined(CONFIG_FREERTOS_UNICORE)
static void ipcNop(void*) {}
#endif

// Keep a CPU_MAX lock so the idle hook cannot apply a pending frequency
// switch. That switch stalls the other core, and only the RESET pin recovers.
static bool holdCpuMax() {
  if (!cpu_max_lock) {
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "wifi-off", &cpu_max_lock) != ESP_OK) {
      cpu_max_lock = nullptr;
      return false;
    }
  }
  return esp_pm_lock_acquire(cpu_max_lock) == ESP_OK;
}

static void wakeOtherCore() {
#if !defined(CONFIG_FREERTOS_UNICORE)
  uint32_t other = xPortGetCoreID() == 0 ? 1 : 0;
  esp_ipc_call(other, ipcNop, nullptr);
#endif
}

// phy_close_rf has to run while the modem clocks are still on. Cutting the
// power domain first latches the analog front-end in the last bias, which
// draws more than Wi-Fi modem sleep and cannot be reached afterwards.
static void closeRadioFrontEnd() {
  REG_CLR_BIT(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_WIFI_FORCE_PD | RTC_CNTL_BT_FORCE_PD);
  delay(10);
  wifiClkSet(SYSTEM_WIFI_CLK_WIFI_BT_COMMON_M | SYSTEM_BT_BASEBAND_EN | SYSTEM_BT_LC_EN);
  REG_CLR_BIT(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_WIFI_FORCE_ISO | RTC_CNTL_BT_FORCE_ISO);
  phy_get_romfunc_addr();
  phy_close_rf();
#if !defined(CONFIG_IDF_TARGET_ESP32)
  phy_xpd_tsens();
#endif
}

// The IDF helper is refcounted and skips the registers when the count is
// already zero, which is every boot that did not start the driver. Write the
// same bits rtc_init uses, and gate the modem clocks, leaving the RNG clock.
static void releaseModemPower() {
  closeRadioFrontEnd();
  REG_SET_BIT(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_WIFI_FORCE_ISO | RTC_CNTL_BT_FORCE_ISO);
  REG_SET_BIT(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_WIFI_FORCE_PD | RTC_CNTL_BT_FORCE_PD);
  REG_CLR_BIT(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_WIFI_FORCE_PU | RTC_CNTL_BT_FORCE_PU);
  REG_CLR_BIT(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_WIFI_FORCE_NOISO | RTC_CNTL_BT_FORCE_NOISO);
  wifiClkClr(SYSTEM_WIFI_CLK_WIFI_BT_COMMON_M | SYSTEM_BT_BASEBAND_EN | SYSTEM_BT_LC_EN);
  REG_CLR_BIT(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_XTL_FORCE_PU | RTC_CNTL_BBPLL_FORCE_PU |
                                     RTC_CNTL_BBPLL_I2C_FORCE_PU | RTC_CNTL_BB_I2C_FORCE_PU);
#ifdef SYSTEM_CPU_WAIT_MODE_FORCE_ON
  REG_CLR_BIT(SYSTEM_CPU_PER_CONF_REG, SYSTEM_CPU_WAIT_MODE_FORCE_ON);
#endif
}

static void allowModemPower() {
  REG_CLR_BIT(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_WIFI_FORCE_PD | RTC_CNTL_BT_FORCE_PD);
  REG_CLR_BIT(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_WIFI_FORCE_ISO | RTC_CNTL_BT_FORCE_ISO);
}

static void wifiCpu(bool on) {
#ifdef ESP32_CPU_FREQ
  uint32_t mhz = ESP32_CPU_FREQ;
  if (on && mhz < 160) mhz = 160;
#else
  uint32_t mhz = on ? 160 : 80;
#endif
  // Once esp_pm owns the table, setCpuFrequencyMhz skips the other core's
  // tick compare. The next switch then waits forever and the watchdog reboots.
  if (pm_configured || getCpuFrequencyMhz() == mhz) return;
  setCpuFrequencyMhz(mhz);
}

static uint32_t pinnedMhz() {
  wifiCpu(true);
  uint32_t mhz = getCpuFrequencyMhz();
  if (mhz < 160) mhz = 160;
  return mhz;
}

static bool pmSet(uint32_t max_mhz, uint32_t min_mhz, bool light);

// Bucket the clock that is already running. Do not round 40 up to 80: the
// next idle would then switch frequency while the other core is inside the
// idle hook, the stall never completes, and the chip reboots.
static uint32_t runningMhz() {
  uint32_t mhz = getCpuFrequencyMhz();
  if (mhz >= 240) return 240;
  if (mhz >= 160) return 160;
  if (mhz >= 80) return 80;
  return 40;
}

static void restartForIdleClock() {
  bool locked = holdCpuMax();
  if (locked) wakeOtherCore();
  uint32_t pinned = runningMhz();
  if (locked) pmSet(pinned, pinned, false);
  if ((WiFi.getMode() & WIFI_MODE_STA) != 0) {
    WiFi.persistent(false);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  // The notice is already on the glass. Do not draw again: closing the radio
  // breaks the I2C transfer after the first page, which clears only the header.
  releaseModemPower();
  esp_restart();
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
  if (min_mhz > max_mhz) return false;
  cfg.max_freq_mhz = (int)max_mhz;
  cfg.min_freq_mhz = (int)min_mhz;
  cfg.light_sleep_enable = light;
  if (esp_pm_configure(&cfg) != ESP_OK) return false;
  pm_configured = true;
  return true;
#else
  return false;
#endif
}

// 160 MHz is rejected when the crystal register is not an exact divisor of 160
// (a 240 MHz reading does that). Retrying the same maximum never enables sleep.
// 80 MHz is the combination automatic light sleep already uses on this core.
// The minimum has to be the crystal itself: 40 MHz, or 32 MHz when that is real.
static bool pmLight(uint32_t* max_out) {
  const uint32_t maxima[] = {160, 80, 240};
  const uint32_t minima[] = {ESP32Board::lightSleepXtalMhz(), 80};
  for (uint32_t max_mhz : maxima) {
    for (uint32_t min_mhz : minima) {
      if (!pmSet(max_mhz, min_mhz, true)) continue;
      if (max_out) *max_out = max_mhz;
      return true;
    }
  }
  return false;
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
    _down_since = 0;
    _next_join = 0;
    // Only a station started in this process. The boot after the restart has
    // the flag clear, skips this, and can sleep at the idle clock.
    if (sta_started) restartForIdleClock();
    // This boot did not start STA. Force the modem domain and its clocks off.
    // A software restart does not restore the cold-boot power switches.
    releaseModemPower();
    applyPower();
    sta_started = false;
    return;
  }
  // Do not raise the clock before the driver starts. After Wi-Fi was off the
  // other core is idle; an 80→160 switch then waits for a tick update that
  // core cannot send, and the chip reboots. applyPower asks esp_pm for 160 MHz
  // after WiFi.begin, while the Wi-Fi task is running and can finish the switch.
  // A manual setCpuFrequencyMhz here is the same deadlock.
  allowModemPower();
  if (!pm_configured && savedMode(_prefs) != WIFI_POWER_LIGHT) {
    // Cold boot, modem or no sleep: 80 MHz leaves the association up while
    // TCP sessions rot. Light sleep owns the clock through esp_pm; jumping
    // to 160 MHz first is the switch this core rejects.
    wifiCpu(true);
  }
  WiFi.persistent(false);
  WiFi.setSleep(savedMode(_prefs) != WIFI_POWER_NONE);
  WiFi.mode(WIFI_STA);
  sta_started = true;
  WiFi.setAutoReconnect(true);
  // The driver's own retry gets this long before maintain() calls connect again.
  armJoinGrace();
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
    // The station is already stopped. Do not retune esp_pm here.
    if (!pm_used || (WiFi.getMode() & WIFI_MODE_STA) == 0) WiFi.setSleep(WIFI_PS_NONE);
#ifdef WITH_ESPNOW_BRIDGE
    if (pm_used && pmSet(runningMhz(), runningMhz(), false)) pm_used = false;
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
    uint32_t max_mhz = 0;
    if (pmLight(&max_mhz)) {
      _power = WIFI_POWER_LIGHT;
      Serial.printf("WiFi power: modem + light sleep, max %u MHz\n", (unsigned)max_mhz);
      return;
    }
    mode = WIFI_POWER_MODEM;
    modem = true;
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    if ((WiFi.getMode() & WIFI_MODE_STA) != 0) esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    Serial.printf("WiFi power: light sleep unsupported, modem only (xtal %u MHz)\n",
                  (unsigned)getXtalFrequencyMhz());
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

void WifiStation::armJoinGrace() {
#if defined(ESP32)
  _down_since = millis();
  if (_down_since == 0) _down_since = 1;
  _next_join = _down_since + 12000;
#else
  _down_since = 0;
  _next_join = 0;
#endif
}

void WifiStation::maintain() {
#if defined(ESP32)
  if (!_prefs || !_prefs->wifi_enabled || !_prefs->wifi_ssid[0]) {
    _down_since = 0;
    _next_join = 0;
    return;
  }
  uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    _down_since = 0;
    _next_join = 0;
    return;
  }
  // Mode is gone. apply() is the path that starts STA, not the path that
  // stops the driver and restarts for the idle clock.
  if ((WiFi.getMode() & WIFI_MODE_STA) == 0) {
    if (_next_join && (int32_t)(_next_join - now) > 0) return;
    Serial.println("WiFi: station down, joining");
    apply();
    return;
  }
  if (!_down_since) _down_since = now ? now : 1;
  if ((uint32_t)(now - _down_since) < 12000) return;
  if (_next_join && (int32_t)(_next_join - now) > 0) return;

  // WiFi.reconnect() disconnects first and returns without connect() when the
  // station is already down. esp_wifi_connect() uses the SSID from begin().
  esp_err_t err = esp_wifi_connect();
  bool busy = err == ESP_ERR_WIFI_CONN;
#ifdef ESP_ERR_WIFI_STATE
  if (err == ESP_ERR_WIFI_STATE) busy = true;
#endif
  if (busy && (uint32_t)(now - _down_since) >= 45000) {
    Serial.println("WiFi: reconnect stuck, retrying");
    esp_wifi_disconnect();
    err = esp_wifi_connect();
    // The new attempt gets its own window. Disconnecting again on the next
    // pass would abort it before the handshake can finish.
    _down_since = now ? now : 1;
  } else if (err == ESP_OK) {
    Serial.println("WiFi: reconnect");
  } else if (!busy) {
    Serial.printf("WiFi: reconnect failed (%d)\n", (int)err);
  }
  _next_join = now + 15000;
#else
  _down_since = 0;
  _next_join = 0;
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

bool WifiStation::copyRssi(int8_t& dbm) const {
#if defined(ESP32)
  if (WiFi.status() != WL_CONNECTED) return false;
  int rssi = WiFi.RSSI();
  if (rssi >= 0 || rssi < -120) return false;
  dbm = (int8_t)rssi;
  return true;
#else
  (void)dbm;
  return false;
#endif
}
