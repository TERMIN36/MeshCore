#pragma once

#include "helpers/CommonCLI.h"
#include "helpers/bridges/MqttBridgePolicy.h"

// Repeater MQTT bridge. On platforms without WiFi the methods are empty.
class MqttBridge {
public:
  void begin(NodePrefs* prefs, mesh::PacketManager* mgr, mesh::RTCClock* rtc, const uint8_t pub_key[32]);
  void loop();
  void onRadioTx(mesh::Packet* packet);
  void applyConfig();
  bool isUp() const;
  bool blocksSleep() const;
  void formatStatus(char* reply, size_t cap);
  bool setCaCert(const char* pem, size_t len);
  bool hasCaCert() const;
  void setRadioMetrics(int16_t noise_floor, uint32_t tx_air_secs, uint32_t rx_air_secs, uint32_t uptime_secs,
                       uint32_t tx_queue, uint16_t batt_mv, int16_t temp_c_x10, const char* firmware);

private:
#if defined(ESP32)
  struct Config {
    bool enabled;
    bool tls;
    bool ca_ready;
    uint16_t port;
    char tunnel[65];
    char host[64];
    char user[32];
    char pass[40];
    uint32_t generation;
  };

  struct Inbound {
    uint16_t len;
    uint8_t bytes[MQTT_ENVELOPE_MAX];
  };

  NodePrefs* _prefs = nullptr;
  mesh::PacketManager* _mgr = nullptr;
  mesh::RTCClock* _rtc = nullptr;
  uint8_t _pub[32]{};
  MqttBridgePolicy _policy;
  Config _cfg{};
  Config _live{};
  char _client_id[16]{};
  void* _lock = nullptr;
  void* _queue = nullptr;
  void* _client = nullptr;
  void* _task = nullptr;
  volatile bool _link_up = false;
  volatile bool _broker_up = false;
  volatile bool _sub_due = false;
  volatile bool _sub_pending = false;
  bool _task_started = false;
  // QoS 1 acks in flight. The client outbox holds 4. A publish is written
  // immediately; these slots only wait for PUBACK and restart a stuck session.
  static const int PUB_CAP = 4;
  struct PubSlot {
    int id;
    uint32_t since;
    MqttOutbound item;
  };
  PubSlot _inflight[PUB_CAP]{};
  int _inflight_n = 0;
  int _early_pub_id = 0;
  uint32_t _last_subscribe_ms = 0;
  uint32_t _cooldown_until = 0;
  int16_t _noise_floor = 0;
  uint32_t _tx_air_secs = 0;
  uint32_t _rx_air_secs = 0;
  uint32_t _uptime_secs = 0;
  uint32_t _tx_queue = 0;
  uint16_t _batt_mv = 0;
  int16_t _temp_c_x10 = MQTT_TEMP_UNKNOWN;
  char _firmware[MQTT_FIRMWARE_MAX + 1]{};
  char* _ca_live = nullptr;
  size_t _ca_live_len = 0;
  char* _ca_pending = nullptr;
  size_t _ca_pending_len = 0;
  bool _ca_pending_valid = false;

  void pump();
  void kick();
  void publishOne();
  void notePublished(int msg_id);
  void dropInflightLocked();
  int findPubLocked(int msg_id) const;
  void removePubLocked(int index);
  bool publishStuckLocked(uint32_t now) const;
  void restartClient();
  bool subscribeTunnel();
  void acceptInbound(const uint8_t* data, uint16_t len);
  void handleEvent(int32_t event_id, void* event_data);
  void fillIdentity(MqttIdentity& id) const;
  void fillRadio(MqttRadioDesc& radio) const;
  bool configured(const Config& cfg) const;
  void startClient(const Config& cfg);
  void stopClient();
  void ensureTask();
  void commitCa();
  bool caReadyLocked() const;

  static void taskMain(void* arg);
  static void eventThunk(void* arg, const char* base, int32_t event_id, void* event_data);
#endif
};
