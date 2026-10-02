#pragma once

#include <stddef.h>
#include <stdint.h>

class NodePrefs;

namespace mesh {
class Packet;
class RTCClock;
}

// MeshCoreTel site uplink. Publishes the JSON status and packet topics that
// mqtt.meshcoretel.ru expects. The repeater's own MQTT bridge stays a separate client.
enum class TelLink : uint8_t {
  Off = 0,
  NeedIata,
  WaitWifi,
  WaitTime,
  Connecting,
  Up,
  Retry
};

struct TelView {
  bool enabled;
  bool time_ok;
  uint32_t unix_time;
  char iata[8];
  char host[64];
  char user[32];
  char pass[40];
  uint16_t port;
  char name[32];
  char model[48];
  char firmware[40];
  float freq;
  float bw;
  uint8_t sf;
  uint8_t cr;
  int battery_mv;
  uint32_t uptime_secs;
  uint16_t error_flags;
  uint32_t queue_len;
  int noise_floor;
  uint32_t tx_air_secs;
  uint32_t rx_air_secs;
  uint32_t recv_errors;
};

class TelUplink {
public:
  void begin(NodePrefs* prefs, mesh::RTCClock* rtc, const uint8_t pub_key[32]);
  void applyConfig();
  void loop(const TelView& view);
  void onRadio(const mesh::Packet* packet, bool is_tx, int rssi, float snr, int score, int duration);
  TelLink phase() const;
  bool blocksSleep() const;

private:
#if defined(ESP32)
  NodePrefs* _prefs = nullptr;
  mesh::RTCClock* _rtc = nullptr;
  void* _lock = nullptr;
  void* _client = nullptr;
  void* _task = nullptr;
  bool _task_started = false;
  volatile bool _up = false;
  volatile bool _stopping = false;
  volatile bool _need_restart = false;
  volatile bool _announced = false;
  uint8_t _failures = 0;
  uint32_t _cooldown_until = 0;
  uint32_t _last_status_ms = 0;
  char _device_id[65]{};
  char _live_iata[8]{};
  char _live_host[64]{};
  char _live_user[32]{};
  char _live_pass[40]{};
  uint16_t _live_port = 0;
  char _client_id[48]{};
  char _status_topic[128]{};
  char _packets_topic[128]{};
  char _offline[640]{};
  TelView _view{};

  void ensureTask();
  void pump();
  void stopClient();
  bool startClient(const TelView& view);
  bool publishText(const char* topic, const char* payload, bool retain);
  void publishStatus(const TelView& view);
  void handleEvent(int32_t event_id, void* event_data);
  void refreshTopics(const char* iata);

  static void taskMain(void* arg);
  static void eventThunk(void* arg, const char* base, int32_t event_id, void* event_data);
#endif
};
