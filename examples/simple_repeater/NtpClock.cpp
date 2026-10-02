#include "NtpClock.h"

#include <helpers/CommonCLI.h>
#include <stdio.h>
#include <string.h>

#if defined(ESP32)
#include <atomic>
#include <WiFi.h>
#include <esp_sntp.h>

static constexpr uint32_t NTP_SYNC_INTERVAL_MS = 60UL * 60UL * 1000UL;
static constexpr uint32_t NTP_HOLD_MS = 24UL * 60UL * 60UL * 1000UL;
static constexpr uint32_t NTP_MIN_UNIX = 1704067200UL;  // 2024-01-01 UTC

static std::atomic<uint32_t> g_sync_sec{0};
static std::atomic<uint32_t> g_sync_gen{0};

static void onNtpSync(struct timeval* tv) {
  if (!tv || tv->tv_sec < NTP_MIN_UNIX) return;
  g_sync_sec.store((uint32_t)tv->tv_sec, std::memory_order_relaxed);
  g_sync_gen.fetch_add(1, std::memory_order_release);
}
#endif

void NtpClock::begin(NodePrefs* prefs) {
  _prefs = prefs;
  apply();
}

void NtpClock::apply() {
  _restart = true;
}

bool NtpClock::poll(uint32_t* unix_time) {
#if defined(ESP32)
  bool wifi = WiFi.status() == WL_CONNECTED;
  bool want = _prefs && _prefs->ntp_enabled && _prefs->ntp_server[0] && wifi;
  if (_restart || !want) {
    if (_running) {
      sntp_stop();
      _running = false;
    }
    _restart = false;
  }
  if (want && !_running) {
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, _prefs->ntp_server);
    sntp_set_time_sync_notification_cb(onNtpSync);
    sntp_set_sync_interval(NTP_SYNC_INTERVAL_MS);
    sntp_init();
    _running = true;
  }

  uint32_t gen = g_sync_gen.load(std::memory_order_acquire);
  if (gen == _applied_gen) return false;
  _applied_gen = gen;
  uint32_t sec = g_sync_sec.load(std::memory_order_relaxed);
  _last_sync_ms = millis();
  if (_last_sync_ms == 0) _last_sync_ms = 1;
  if (unix_time) *unix_time = sec;
  return true;
#else
  (void)unix_time;
  return false;
#endif
}

bool NtpClock::requestSync() {
#if defined(ESP32)
  if (!_prefs || !_prefs->ntp_enabled || !_prefs->ntp_server[0]) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (_running) return sntp_restart();
  _restart = true;
  return true;
#else
  return false;
#endif
}

bool NtpClock::holdsClock() const {
#if defined(ESP32)
  if (!_prefs || !_prefs->ntp_enabled || !_last_sync_ms) return false;
  return (uint32_t)(millis() - _last_sync_ms) < NTP_HOLD_MS;
#else
  return false;
#endif
}

uint8_t NtpClock::state() const {
  if (!_prefs || !_prefs->ntp_enabled) return NTP_STATE_OFF;
#if defined(ESP32)
  if (holdsClock()) return NTP_STATE_SYNCED;
  if (WiFi.status() != WL_CONNECTED) return NTP_STATE_NO_WIFI;
  return NTP_STATE_WAIT;
#else
  return NTP_STATE_OFF;
#endif
}

void NtpClock::formatStatus(char* reply, size_t cap) const {
  if (!reply || cap == 0) return;
  if (!_prefs || !_prefs->ntp_enabled) {
    snprintf(reply, cap, "> off");
    return;
  }
  const char* server = _prefs->ntp_server[0] ? _prefs->ntp_server : "";
  const char* label = "wait";
  uint8_t now = state();
  if (now == NTP_STATE_SYNCED) label = "synced";
  else if (now == NTP_STATE_NO_WIFI) label = "no wifi";
  snprintf(reply, cap, "> on %s %s", server, label);
}
