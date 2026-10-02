#include "MqttEnvelope.h"

#include <string.h>

static void putU16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void putU32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void putI16(uint8_t* p, int16_t v) { putU16(p, (uint16_t)v); }
static void putI32(uint8_t* p, int32_t v) { putU32(p, (uint32_t)v); }

static uint16_t getU16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t getU32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int16_t getI16(const uint8_t* p) { return (int16_t)getU16(p); }
static int32_t getI32(const uint8_t* p) { return (int32_t)getU32(p); }

static uint8_t copyBounded(char* dest, const char* src, size_t cap) {
  size_t n = 0;
  if (src) {
    while (src[n] && n < cap) n++;
  }
  if (n) memcpy(dest, src, n);
  dest[n] = 0;
  return (uint8_t)n;
}

static uint8_t copyName(char* dest, const char* name) {
  return copyBounded(dest, name, MQTT_NODE_NAME_MAX);
}

static bool putFirmware(uint8_t* dest, size_t* i, const char* firmware) {
  char buf[MQTT_FIRMWARE_MAX + 1];
  uint8_t n = copyBounded(buf, firmware, MQTT_FIRMWARE_MAX);
  dest[(*i)++] = n;
  if (n) {
    memcpy(&dest[*i], buf, n);
    *i += n;
  }
  return true;
}

static bool takeFirmware(const uint8_t* body, uint16_t len, size_t* i, char* dest) {
  if (*i >= len) return false;
  uint8_t n = body[(*i)++];
  if (n > MQTT_FIRMWARE_MAX || *i + n > len) return false;
  if (n) memcpy(dest, &body[*i], n);
  dest[n] = 0;
  *i += n;
  return true;
}

size_t mqttEncodeEnvelope(uint8_t* dest, size_t cap, uint8_t type, const uint8_t id[32], const char* name,
                          uint32_t seq, bool has_time, uint32_t unix_time, const uint8_t* body, uint16_t body_len) {
  if (!dest || !id) return 0;
  if (body_len && !body) return 0;
  char name_buf[MQTT_NODE_NAME_MAX + 1];
  uint8_t name_len = copyName(name_buf, name);
  size_t need = 1 + 1 + 32 + 1 + name_len + 4 + 1 + (has_time ? 4 : 0) + 2 + body_len;
  if (need > cap || need > MQTT_ENVELOPE_MAX) return 0;

  size_t i = 0;
  dest[i++] = MQTT_BRIDGE_VERSION;
  dest[i++] = type;
  memcpy(&dest[i], id, 32);
  i += 32;
  dest[i++] = name_len;
  if (name_len) {
    memcpy(&dest[i], name_buf, name_len);
    i += name_len;
  }
  putU32(&dest[i], seq);
  i += 4;
  dest[i++] = has_time ? 1 : 0;
  if (has_time) {
    putU32(&dest[i], unix_time);
    i += 4;
  }
  putU16(&dest[i], body_len);
  i += 2;
  if (body_len) {
    memcpy(&dest[i], body, body_len);
    i += body_len;
  }
  return i;
}

size_t mqttEncodeHelloBody(uint8_t* dest, size_t cap, const MqttHelloBody& hello) {
  char firmware[MQTT_FIRMWARE_MAX + 1];
  uint8_t firmware_len = copyBounded(firmware, hello.firmware, MQTT_FIRMWARE_MAX);
  size_t need = 4 + 4 + 1 + 1 + 1 + 1 + (hello.has_ant ? 2 : 0) + (hello.has_coords ? 8 : 0) + 1 + 4 + 16 + 2 + 4 + 4 +
                4 + 4 + 2 + 2 + 1 + firmware_len;
  if (!dest || need > cap) return 0;
  size_t i = 0;
  putU32(&dest[i], hello.freq_hz); i += 4;
  putU32(&dest[i], hello.bw_hz); i += 4;
  dest[i++] = hello.sf;
  dest[i++] = hello.cr;
  dest[i++] = (uint8_t)hello.tx_dbm;
  uint8_t flags = 0;
  if (hello.has_ant) flags |= 0x01;
  if (hello.has_coords) flags |= 0x02;
  dest[i++] = flags;
  if (hello.has_ant) {
    putI16(&dest[i], hello.ant_cm);
    i += 2;
  }
  if (hello.has_coords) {
    putI32(&dest[i], hello.lat_e7); i += 4;
    putI32(&dest[i], hello.lon_e7); i += 4;
  }
  dest[i++] = hello.forwarding ? 1 : 0;
  putU32(&dest[i], hello.session_id); i += 4;
  putU32(&dest[i], hello.pkt_pub); i += 4;
  putU32(&dest[i], hello.pkt_in); i += 4;
  putU32(&dest[i], hello.mqtt_dup); i += 4;
  putU32(&dest[i], hello.pub_err); i += 4;
  putI16(&dest[i], hello.noise_floor); i += 2;
  putU32(&dest[i], hello.tx_air_secs); i += 4;
  putU32(&dest[i], hello.rx_air_secs); i += 4;
  putU32(&dest[i], hello.uptime_secs); i += 4;
  putU32(&dest[i], hello.tx_queue); i += 4;
  putU16(&dest[i], hello.batt_mv); i += 2;
  putI16(&dest[i], hello.temp_c_x10); i += 2;
  putFirmware(dest, &i, firmware);
  return i;
}

size_t mqttEncodeHeartbeatBody(uint8_t* dest, size_t cap, const MqttHeartbeatBody& beat) {
  char firmware[MQTT_FIRMWARE_MAX + 1];
  uint8_t firmware_len = copyBounded(firmware, beat.firmware, MQTT_FIRMWARE_MAX);
  size_t need = 20 + 2 + 4 + 4 + 4 + 4 + 2 + 2 + 1 + firmware_len + 1 + (beat.has_coords ? 8 : 0);
  if (!dest || need > cap) return 0;
  putU32(&dest[0], beat.session_id);
  putU32(&dest[4], beat.pkt_pub);
  putU32(&dest[8], beat.pkt_in);
  putU32(&dest[12], beat.mqtt_dup);
  putU32(&dest[16], beat.pub_err);
  putI16(&dest[20], beat.noise_floor);
  putU32(&dest[22], beat.tx_air_secs);
  putU32(&dest[26], beat.rx_air_secs);
  putU32(&dest[30], beat.uptime_secs);
  putU32(&dest[34], beat.tx_queue);
  putU16(&dest[38], beat.batt_mv);
  putI16(&dest[40], beat.temp_c_x10);
  size_t i = 42;
  putFirmware(dest, &i, firmware);
  dest[i++] = beat.has_coords ? 0x02 : 0;
  if (beat.has_coords) {
    putI32(&dest[i], beat.lat_e7); i += 4;
    putI32(&dest[i], beat.lon_e7); i += 4;
  }
  return i;
}

static bool findToken(const char* hay, size_t n, const char* needle, size_t* at) {
  size_t m = 0;
  while (needle[m]) m++;
  if (m == 0 || m > n) return false;
  for (size_t i = 0; i + m <= n; i++) {
    if (memcmp(hay + i, needle, m) == 0) {
      if (at) *at = i;
      return true;
    }
  }
  return false;
}

bool mqttCaPemOk(const char* pem, size_t len) {
  if (!pem || len < 54 || len > 4096) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)pem[i];
    if (c == 0 || c > 126) return false;
  }
  size_t begin_at = 0;
  size_t end_at = 0;
  if (!findToken(pem, len, "-----BEGIN CERTIFICATE-----", &begin_at)) return false;
  if (!findToken(pem, len, "-----END CERTIFICATE-----", &end_at)) return false;
  return begin_at < end_at;
}

MqttDecode mqttDecodeEnvelope(const uint8_t* data, size_t len, MqttEnvelopeView* out) {
  if (!data || !out || len < 1) return MqttDecode::Truncated;
  if (data[0] != MQTT_BRIDGE_VERSION) return MqttDecode::BadVersion;
  size_t i = 1;
  if (i >= len) return MqttDecode::Truncated;
  uint8_t type = data[i++];
  if (i + 32 + 1 > len) return MqttDecode::Truncated;
  memcpy(out->id, &data[i], 32);
  i += 32;
  uint8_t name_len = data[i++];
  if (name_len > MQTT_NODE_NAME_MAX || i + name_len + 4 + 1 + 2 > len) return MqttDecode::Truncated;
  memcpy(out->name, &data[i], name_len);
  out->name[name_len] = 0;
  out->name_len = name_len;
  i += name_len;
  out->seq = getU32(&data[i]);
  i += 4;
  uint8_t time_flag = data[i++];
  if (time_flag > 1) return MqttDecode::Truncated;
  out->has_time = time_flag == 1;
  out->unix_time = 0;
  if (out->has_time) {
    if (i + 4 + 2 > len) return MqttDecode::Truncated;
    out->unix_time = getU32(&data[i]);
    i += 4;
  }
  if (i + 2 > len) return MqttDecode::Truncated;
  uint16_t body_len = getU16(&data[i]);
  i += 2;
  if (i + body_len != len) return MqttDecode::Truncated;
  out->version = MQTT_BRIDGE_VERSION;
  out->type = type;
  out->body = body_len ? &data[i] : nullptr;
  out->body_len = body_len;
  return MqttDecode::Ok;
}

bool mqttDecodeHelloBody(const uint8_t* body, uint16_t len, MqttHelloBody* out) {
  if (!body || !out || len < 16) return false;
  size_t i = 0;
  out->freq_hz = getU32(&body[i]); i += 4;
  out->bw_hz = getU32(&body[i]); i += 4;
  out->sf = body[i++];
  out->cr = body[i++];
  out->tx_dbm = (int8_t)body[i++];
  uint8_t flags = body[i++];
  out->has_ant = (flags & 0x01) != 0;
  out->has_coords = (flags & 0x02) != 0;
  out->ant_cm = 0;
  out->lat_e7 = 0;
  out->lon_e7 = 0;
  if (out->has_ant) {
    if (i + 2 > len) return false;
    out->ant_cm = getI16(&body[i]);
    i += 2;
  }
  if (out->has_coords) {
    if (i + 8 > len) return false;
    out->lat_e7 = getI32(&body[i]); i += 4;
    out->lon_e7 = getI32(&body[i]); i += 4;
  }
  if (i + 1 + 20 + 10 + 8 + 4 + 1 > len) return false;
  out->forwarding = body[i++] != 0;
  out->session_id = getU32(&body[i]); i += 4;
  out->pkt_pub = getU32(&body[i]); i += 4;
  out->pkt_in = getU32(&body[i]); i += 4;
  out->mqtt_dup = getU32(&body[i]); i += 4;
  out->pub_err = getU32(&body[i]); i += 4;
  out->noise_floor = getI16(&body[i]); i += 2;
  out->tx_air_secs = getU32(&body[i]); i += 4;
  out->rx_air_secs = getU32(&body[i]); i += 4;
  out->uptime_secs = getU32(&body[i]); i += 4;
  out->tx_queue = getU32(&body[i]); i += 4;
  out->batt_mv = getU16(&body[i]); i += 2;
  out->temp_c_x10 = getI16(&body[i]); i += 2;
  if (!takeFirmware(body, len, &i, out->firmware)) return false;
  return i == len;
}

bool mqttDecodeHeartbeatBody(const uint8_t* body, uint16_t len, MqttHeartbeatBody* out) {
  if (!body || !out || len < 44) return false;
  out->session_id = getU32(&body[0]);
  out->pkt_pub = getU32(&body[4]);
  out->pkt_in = getU32(&body[8]);
  out->mqtt_dup = getU32(&body[12]);
  out->pub_err = getU32(&body[16]);
  out->noise_floor = getI16(&body[20]);
  out->tx_air_secs = getU32(&body[22]);
  out->rx_air_secs = getU32(&body[26]);
  out->uptime_secs = getU32(&body[30]);
  out->tx_queue = getU32(&body[34]);
  out->batt_mv = getU16(&body[38]);
  out->temp_c_x10 = getI16(&body[40]);
  size_t i = 42;
  if (!takeFirmware(body, len, &i, out->firmware)) return false;
  if (i >= len) return false;
  uint8_t flags = body[i++];
  out->has_coords = (flags & 0x02) != 0;
  out->lat_e7 = 0;
  out->lon_e7 = 0;
  if (out->has_coords) {
    if (i + 8 > len) return false;
    out->lat_e7 = getI32(&body[i]); i += 4;
    out->lon_e7 = getI32(&body[i]); i += 4;
  }
  return i == len;
}
