#pragma once

#include <stddef.h>
#include <stdint.h>

class NodePrefs;

enum : uint8_t {
  NTP_STATE_OFF = 0,
  NTP_STATE_NO_WIFI = 1,
  NTP_STATE_WAIT = 2,
  NTP_STATE_SYNCED = 3
};

// Polls one NTP server over the station Wi-Fi link and reports each new UTC sample.
class NtpClock {
public:
  void begin(NodePrefs* prefs);
  void apply();
  bool poll(uint32_t* unix_time);
  bool requestSync();
  bool holdsClock() const;
  uint8_t state() const;
  void formatStatus(char* reply, size_t cap) const;

private:
  NodePrefs* _prefs = nullptr;
  uint32_t _applied_gen = 0;
  uint32_t _last_sync_ms = 0;
  bool _running = false;
  bool _restart = false;
};
