#pragma once

#include "MqttEnvelope.h"
#include "Packet.h"

#include <stdint.h>

// Same slot count as the firmware seen-cache. Hash identity is Packet::calculatePacketHash.
#ifndef MAX_PACKET_HASHES
#define MAX_PACKET_HASHES (128 + 32)
#endif

enum class MqttEnqueue : uint8_t { Skipped, Queued, Failed };

enum class MqttRx : uint8_t { Ignore, Duplicate, Packet };

struct MqttIdentity {
  uint8_t id[32];
  char name[MQTT_NODE_NAME_MAX + 1];
  bool has_time;
  uint32_t unix_time;
};

struct MqttRadioDesc {
  uint32_t freq_hz;
  uint32_t bw_hz;
  uint8_t sf;
  uint8_t cr;
  int8_t tx_dbm;
  bool has_ant;
  int16_t ant_cm;
  bool has_coords;
  int32_t lat_e7;
  int32_t lon_e7;
  bool forwarding;
  int16_t noise_floor;
  uint32_t tx_air_secs;
  uint32_t rx_air_secs;
  uint32_t uptime_secs;
  uint32_t tx_queue;
  uint16_t batt_mv;
  int16_t temp_c_x10;
  char firmware[MQTT_FIRMWARE_MAX + 1];
};

struct MqttCounters {
  uint32_t pkt_pub;
  uint32_t pkt_in;
  uint32_t mqtt_dup;
  uint32_t pub_err;
};

struct MqttLifetime {
  uint32_t connects;
  MqttCounters totals;
  uint32_t last_hello_unix;
  uint32_t last_heartbeat_unix;
  uint32_t last_hello_ms;
  uint32_t last_heartbeat_ms;
};

struct MqttOutbound {
  uint8_t type;
  bool has_hash;
  uint8_t hash[MAX_HASH_SIZE];
  uint16_t len;
  uint32_t epoch;
  uint8_t bytes[MQTT_ENVELOPE_MAX];
};

// Routing decisions for one repeater. No participant table and no broker I/O.
class MqttBridgePolicy {
public:
  static const int QUEUE_CAP = 4;
  static const uint32_t HEARTBEAT_MS = 60000;

  MqttBridgePolicy();

  void onSubscribed();
  void onDisconnected();
  bool online() const { return _online; }
  bool helloSent() const { return _hello_sent; }
  bool needsHello() const { return _online && !_hello_sent && !_hello_queued && !_hello_inflight; }

  MqttEnqueue enqueueHello(const MqttIdentity& id, const MqttRadioDesc& radio);
  MqttEnqueue enqueuePacket(const mesh::Packet* packet, const uint8_t* raw, uint16_t raw_len, const MqttIdentity& id);
  void pollHeartbeat(uint32_t now_ms, const MqttIdentity& id, const MqttRadioDesc& radio);

  bool popOutbound(MqttOutbound& out);
  // Put a popped item back at the head when the broker has not accepted it yet.
  void defer(const MqttOutbound& item);
  void onPublishOk(const MqttOutbound& item, uint32_t now_ms);
  void onPublishFail(const MqttOutbound& item);

  MqttRx receive(const uint8_t* env, size_t len, uint8_t* raw_out, uint16_t* raw_len);
  void commitReceived(const uint8_t hash[MAX_HASH_SIZE]);

  const MqttCounters& session() const { return _session; }
  const MqttLifetime& lifetime() const { return _life; }
  uint32_t sessionId() const { return _session_id; }
  uint32_t publishSeq() const { return _seq; }

  void formatStatus(char* reply, size_t cap, bool link_up) const;

private:
  struct SeenCache {
    uint8_t hashes[MAX_PACKET_HASHES * MAX_HASH_SIZE];
    int next;
    void clearAll();
    bool contains(const uint8_t hash[MAX_HASH_SIZE]) const;
    void insert(const uint8_t hash[MAX_HASH_SIZE]);
    void remove(const uint8_t hash[MAX_HASH_SIZE]);
  };

  MqttEnqueue push(MqttOutbound& item, bool count_packet_reject);
  void noteError();
  void fillPublisher(MqttOutbound& item, uint8_t type, const MqttIdentity& id, const uint8_t* body, uint16_t body_len);

  SeenCache _seen;
  MqttOutbound _queue[QUEUE_CAP];
  int _queued;
  bool _online;
  bool _hello_sent;
  bool _hello_queued;
  bool _hello_inflight;
  uint32_t _heartbeat_due_ms;
  uint32_t _seq;
  uint32_t _session_id;
  uint32_t _conn_epoch;
  MqttCounters _session;
  MqttLifetime _life;
};
