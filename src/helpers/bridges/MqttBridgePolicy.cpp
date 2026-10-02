#include "MqttBridgePolicy.h"

#include <stdio.h>
#include <string.h>

void MqttBridgePolicy::SeenCache::clearAll() {
  memset(hashes, 0, sizeof(hashes));
  next = 0;
}

bool MqttBridgePolicy::SeenCache::contains(const uint8_t hash[MAX_HASH_SIZE]) const {
  const uint8_t* slot = hashes;
  for (int i = 0; i < MAX_PACKET_HASHES; i++, slot += MAX_HASH_SIZE) {
    if (memcmp(hash, slot, MAX_HASH_SIZE) == 0) return true;
  }
  return false;
}

void MqttBridgePolicy::SeenCache::insert(const uint8_t hash[MAX_HASH_SIZE]) {
  memcpy(&hashes[next * MAX_HASH_SIZE], hash, MAX_HASH_SIZE);
  next = (next + 1) % MAX_PACKET_HASHES;
}

void MqttBridgePolicy::SeenCache::remove(const uint8_t hash[MAX_HASH_SIZE]) {
  uint8_t* slot = hashes;
  for (int i = 0; i < MAX_PACKET_HASHES; i++, slot += MAX_HASH_SIZE) {
    if (memcmp(hash, slot, MAX_HASH_SIZE) == 0) {
      memset(slot, 0, MAX_HASH_SIZE);
      return;
    }
  }
}

MqttBridgePolicy::MqttBridgePolicy() {
  _seen.clearAll();
  _queued = 0;
  _online = false;
  _hello_sent = false;
  _hello_queued = false;
  _hello_inflight = false;
  _heartbeat_due_ms = 0;
  _seq = 0;
  _session_id = 0;
  _conn_epoch = 0;
  memset(&_session, 0, sizeof(_session));
  memset(&_life, 0, sizeof(_life));
}

void MqttBridgePolicy::noteError() {
  _session.pub_err++;
  _life.totals.pub_err++;
}

void MqttBridgePolicy::onSubscribed() {
  if (_online) return;
  _online = true;
  memset(&_session, 0, sizeof(_session));
  _seq = 0;
  _life.connects++;
  _session_id = _life.connects;
  _conn_epoch++;
  _hello_sent = false;
  _hello_queued = false;
  _hello_inflight = false;
  _heartbeat_due_ms = 0;
}

void MqttBridgePolicy::onDisconnected() {
  if (!_online && _queued == 0) return;
  _online = false;
  _hello_sent = false;
  _hello_queued = false;
  _hello_inflight = false;
  _heartbeat_due_ms = 0;
  for (int i = 0; i < _queued; i++) {
    if (_queue[i].type == MQTT_TYPE_PACKET && _queue[i].has_hash) {
      _seen.remove(_queue[i].hash);
      noteError();
    }
  }
  _queued = 0;
}

void MqttBridgePolicy::fillPublisher(MqttOutbound& item, uint8_t type, const MqttIdentity& id, const uint8_t* body,
                                     uint16_t body_len) {
  memset(&item, 0, sizeof(item));
  item.type = type;
  item.epoch = _conn_epoch;
  _seq++;
  item.len = (uint16_t)mqttEncodeEnvelope(item.bytes, sizeof(item.bytes), type, id.id, id.name, _seq, id.has_time,
                                          id.unix_time, body, body_len);
}

MqttEnqueue MqttBridgePolicy::push(MqttOutbound& item, bool count_packet_reject) {
  if (item.len == 0) {
    if (count_packet_reject) noteError();
    return MqttEnqueue::Failed;
  }
  if (_queued == QUEUE_CAP) {
    if (item.type == MQTT_TYPE_HEARTBEAT) {
      noteError();
      return MqttEnqueue::Failed;
    }
    int beat = -1;
    for (int i = 0; i < _queued; i++) {
      if (_queue[i].type == MQTT_TYPE_HEARTBEAT) {
        beat = i;
        break;
      }
    }
    if (beat < 0) {
      if (count_packet_reject) noteError();
      return MqttEnqueue::Failed;
    }
    for (int i = beat; i < _queued - 1; i++) _queue[i] = _queue[i + 1];
    _queued--;
    _hello_queued = false;
    for (int i = 0; i < _queued; i++) {
      if (_queue[i].type == MQTT_TYPE_HELLO) _hello_queued = true;
    }
    noteError();
  }
  _queue[_queued++] = item;
  if (item.type == MQTT_TYPE_HELLO) _hello_queued = true;
  return MqttEnqueue::Queued;
}

MqttEnqueue MqttBridgePolicy::enqueueHello(const MqttIdentity& id, const MqttRadioDesc& radio) {
  if (!_online || _hello_sent || _hello_queued || _hello_inflight) return MqttEnqueue::Skipped;
  MqttHelloBody hello;
  memset(&hello, 0, sizeof(hello));
  hello.freq_hz = radio.freq_hz;
  hello.bw_hz = radio.bw_hz;
  hello.sf = radio.sf;
  hello.cr = radio.cr;
  hello.tx_dbm = radio.tx_dbm;
  hello.has_ant = radio.has_ant;
  hello.ant_cm = radio.ant_cm;
  hello.has_coords = radio.has_coords;
  hello.lat_e7 = radio.lat_e7;
  hello.lon_e7 = radio.lon_e7;
  hello.forwarding = radio.forwarding;
  hello.session_id = _session_id;
  hello.pkt_pub = _session.pkt_pub;
  hello.pkt_in = _session.pkt_in;
  hello.mqtt_dup = _session.mqtt_dup;
  hello.pub_err = _session.pub_err;
  hello.noise_floor = radio.noise_floor;
  hello.tx_air_secs = radio.tx_air_secs;
  hello.rx_air_secs = radio.rx_air_secs;
  hello.uptime_secs = radio.uptime_secs;
  hello.tx_queue = radio.tx_queue;
  hello.batt_mv = radio.batt_mv;
  hello.temp_c_x10 = radio.temp_c_x10;
  strncpy(hello.firmware, radio.firmware, MQTT_FIRMWARE_MAX);
  hello.firmware[MQTT_FIRMWARE_MAX] = 0;

  uint8_t body[128];
  size_t body_len = mqttEncodeHelloBody(body, sizeof(body), hello);
  MqttOutbound item;
  fillPublisher(item, MQTT_TYPE_HELLO, id, body, (uint16_t)body_len);
  return push(item, false);
}

MqttEnqueue MqttBridgePolicy::enqueuePacket(const mesh::Packet* packet, const uint8_t* raw, uint16_t raw_len,
                                            const MqttIdentity& id) {
  if (!_online || !packet || !raw || raw_len == 0) return MqttEnqueue::Skipped;
  uint8_t hash[MAX_HASH_SIZE];
  packet->calculatePacketHash(hash);
  if (_seen.contains(hash)) return MqttEnqueue::Skipped;

  MqttOutbound item;
  fillPublisher(item, MQTT_TYPE_PACKET, id, raw, raw_len);
  item.has_hash = true;
  memcpy(item.hash, hash, MAX_HASH_SIZE);
  MqttEnqueue result = push(item, true);
  if (result == MqttEnqueue::Queued) _seen.insert(hash);
  return result;
}

void MqttBridgePolicy::pollHeartbeat(uint32_t now_ms, const MqttIdentity& id, const MqttRadioDesc& radio) {
  if (!_online || !_hello_sent) return;
  if ((int32_t)(now_ms - _heartbeat_due_ms) < 0) return;
  for (int i = 0; i < _queued; i++) {
    if (_queue[i].type == MQTT_TYPE_HEARTBEAT) return;
  }

  MqttHeartbeatBody beat;
  memset(&beat, 0, sizeof(beat));
  beat.session_id = _session_id;
  beat.pkt_pub = _session.pkt_pub;
  beat.pkt_in = _session.pkt_in;
  beat.mqtt_dup = _session.mqtt_dup;
  beat.pub_err = _session.pub_err;
  beat.noise_floor = radio.noise_floor;
  beat.tx_air_secs = radio.tx_air_secs;
  beat.rx_air_secs = radio.rx_air_secs;
  beat.uptime_secs = radio.uptime_secs;
  beat.tx_queue = radio.tx_queue;
  beat.batt_mv = radio.batt_mv;
  beat.temp_c_x10 = radio.temp_c_x10;
  strncpy(beat.firmware, radio.firmware, MQTT_FIRMWARE_MAX);
  beat.firmware[MQTT_FIRMWARE_MAX] = 0;
  beat.has_coords = radio.has_coords;
  beat.lat_e7 = radio.lat_e7;
  beat.lon_e7 = radio.lon_e7;
  uint8_t body[96];
  size_t body_len = mqttEncodeHeartbeatBody(body, sizeof(body), beat);
  MqttOutbound item;
  fillPublisher(item, MQTT_TYPE_HEARTBEAT, id, body, (uint16_t)body_len);
  if (push(item, false) == MqttEnqueue::Queued) {
    _heartbeat_due_ms = now_ms + HEARTBEAT_MS;
  } else {
    _heartbeat_due_ms = now_ms + HEARTBEAT_MS;
  }
}

bool MqttBridgePolicy::popOutbound(MqttOutbound& out) {
  if (_queued == 0) return false;
  out = _queue[0];
  for (int i = 0; i < _queued - 1; i++) _queue[i] = _queue[i + 1];
  _queued--;
  if (out.type == MQTT_TYPE_HELLO) {
    _hello_queued = false;
    _hello_inflight = true;
  }
  return true;
}

void MqttBridgePolicy::onPublishOk(const MqttOutbound& item, uint32_t now_ms) {
  if (item.epoch != _conn_epoch) {
    if (item.type == MQTT_TYPE_PACKET) _life.totals.pkt_pub++;
    return;
  }
  if (item.type == MQTT_TYPE_PACKET) {
    _session.pkt_pub++;
    _life.totals.pkt_pub++;
  } else if (item.type == MQTT_TYPE_HELLO) {
    _hello_inflight = false;
    _hello_sent = true;
    _heartbeat_due_ms = now_ms + HEARTBEAT_MS;
    _life.last_hello_ms = now_ms;
    MqttEnvelopeView view;
    if (mqttDecodeEnvelope(item.bytes, item.len, &view) == MqttDecode::Ok && view.has_time) {
      _life.last_hello_unix = view.unix_time;
    }
  } else if (item.type == MQTT_TYPE_HEARTBEAT) {
    _life.last_heartbeat_ms = now_ms;
    MqttEnvelopeView view;
    if (mqttDecodeEnvelope(item.bytes, item.len, &view) == MqttDecode::Ok && view.has_time) {
      _life.last_heartbeat_unix = view.unix_time;
    }
  }
}

void MqttBridgePolicy::defer(const MqttOutbound& item) {
  if (item.epoch != _conn_epoch || !_online) {
    onPublishFail(item);
    return;
  }
  if (item.type == MQTT_TYPE_HELLO) _hello_inflight = false;
  if (_queued >= QUEUE_CAP) {
    int beat = -1;
    for (int i = 0; i < _queued; i++) {
      if (_queue[i].type == MQTT_TYPE_HEARTBEAT) {
        beat = i;
        break;
      }
    }
    if (beat < 0) {
      onPublishFail(item);
      return;
    }
    for (int i = beat; i < _queued - 1; i++) _queue[i] = _queue[i + 1];
    _queued--;
    noteError();
  }
  for (int i = _queued; i > 0; i--) _queue[i] = _queue[i - 1];
  _queue[0] = item;
  _queued++;
  if (item.type == MQTT_TYPE_HELLO) _hello_queued = true;
}

void MqttBridgePolicy::onPublishFail(const MqttOutbound& item) {
  if (item.epoch != _conn_epoch) {
    if (item.type == MQTT_TYPE_PACKET && item.has_hash) {
      _seen.remove(item.hash);
      _life.totals.pub_err++;
    }
    return;
  }
  if (item.type == MQTT_TYPE_PACKET && item.has_hash) _seen.remove(item.hash);
  if (item.type == MQTT_TYPE_HELLO) {
    _hello_queued = false;
    _hello_inflight = false;
  }
  noteError();
}

MqttRx MqttBridgePolicy::receive(const uint8_t* env, size_t len, uint8_t* raw_out, uint16_t* raw_len) {
  MqttEnvelopeView view;
  MqttDecode decoded = mqttDecodeEnvelope(env, len, &view);
  if (decoded != MqttDecode::Ok) return MqttRx::Ignore;
  if (view.type == MQTT_TYPE_HELLO || view.type == MQTT_TYPE_HEARTBEAT) return MqttRx::Ignore;
  if (view.type != MQTT_TYPE_PACKET || view.body_len == 0 || view.body_len > 255) return MqttRx::Ignore;

  mesh::Packet packet;
  if (!packet.readFrom(view.body, (uint8_t)view.body_len)) return MqttRx::Ignore;
  uint8_t hash[MAX_HASH_SIZE];
  packet.calculatePacketHash(hash);
  if (_seen.contains(hash)) {
    _session.mqtt_dup++;
    _life.totals.mqtt_dup++;
    return MqttRx::Duplicate;
  }
  if (raw_out && raw_len) {
    memcpy(raw_out, view.body, view.body_len);
    *raw_len = view.body_len;
  }
  return MqttRx::Packet;
}

void MqttBridgePolicy::commitReceived(const uint8_t hash[MAX_HASH_SIZE]) {
  if (!hash) return;
  _seen.insert(hash);
  _session.pkt_in++;
  _life.totals.pkt_in++;
}

void MqttBridgePolicy::formatStatus(char* reply, size_t cap, bool link_up) const {
  if (!reply || cap == 0) return;
  snprintf(reply, cap, "> %s s %lu %lu %lu %lu c %lu L %lu %lu %lu %lu h %lu b %lu", link_up ? "up" : "down",
           (unsigned long)_session.pkt_pub, (unsigned long)_session.pkt_in, (unsigned long)_session.mqtt_dup,
           (unsigned long)_session.pub_err, (unsigned long)_life.connects, (unsigned long)_life.totals.pkt_pub,
           (unsigned long)_life.totals.pkt_in, (unsigned long)_life.totals.mqtt_dup, (unsigned long)_life.totals.pub_err,
           (unsigned long)_life.last_hello_unix, (unsigned long)_life.last_heartbeat_unix);
}
