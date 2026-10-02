#pragma once

#include <stddef.h>
#include <stdint.h>

// MQTT bridge envelope v1. The tunnel name is the MQTT topic, never a field here.
// Multi-byte integers are little-endian. An unknown version is rejected whole.

static const uint8_t MQTT_BRIDGE_VERSION = 1;
static const uint8_t MQTT_TYPE_PACKET = 1;
static const uint8_t MQTT_TYPE_HELLO = 2;
static const uint8_t MQTT_TYPE_HEARTBEAT = 3;

static const size_t MQTT_ENVELOPE_MAX = 360;
static const size_t MQTT_NODE_NAME_MAX = 31;
static const size_t MQTT_FIRMWARE_MAX = 31;
static const int16_t MQTT_TEMP_UNKNOWN = -32768;

enum class MqttDecode : uint8_t { Ok, BadVersion, Truncated };

struct MqttEnvelopeView {
  uint8_t version;
  uint8_t type;
  uint8_t id[32];
  char name[MQTT_NODE_NAME_MAX + 1];
  uint8_t name_len;
  uint32_t seq;
  bool has_time;
  uint32_t unix_time;
  const uint8_t* body;
  uint16_t body_len;
};

struct MqttHelloBody {
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
  uint32_t session_id;
  uint32_t pkt_pub;
  uint32_t pkt_in;
  uint32_t mqtt_dup;
  uint32_t pub_err;
  int16_t noise_floor;
  uint32_t tx_air_secs;
  uint32_t rx_air_secs;
  uint32_t uptime_secs;
  uint32_t tx_queue;
  uint16_t batt_mv;
  int16_t temp_c_x10;
  char firmware[MQTT_FIRMWARE_MAX + 1];
};

struct MqttHeartbeatBody {
  uint32_t session_id;
  uint32_t pkt_pub;
  uint32_t pkt_in;
  uint32_t mqtt_dup;
  uint32_t pub_err;
  int16_t noise_floor;
  uint32_t tx_air_secs;
  uint32_t rx_air_secs;
  uint32_t uptime_secs;
  uint32_t tx_queue;
  uint16_t batt_mv;
  int16_t temp_c_x10;
  char firmware[MQTT_FIRMWARE_MAX + 1];
  bool has_coords;
  int32_t lat_e7;
  int32_t lon_e7;
};

size_t mqttEncodeEnvelope(uint8_t* dest, size_t cap, uint8_t type, const uint8_t id[32], const char* name,
                          uint32_t seq, bool has_time, uint32_t unix_time, const uint8_t* body, uint16_t body_len);

size_t mqttEncodeHelloBody(uint8_t* dest, size_t cap, const MqttHelloBody& hello);
size_t mqttEncodeHeartbeatBody(uint8_t* dest, size_t cap, const MqttHeartbeatBody& beat);

bool mqttCaPemOk(const char* pem, size_t len);

MqttDecode mqttDecodeEnvelope(const uint8_t* data, size_t len, MqttEnvelopeView* out);
bool mqttDecodeHelloBody(const uint8_t* body, uint16_t len, MqttHelloBody* out);
bool mqttDecodeHeartbeatBody(const uint8_t* body, uint16_t len, MqttHeartbeatBody* out);
