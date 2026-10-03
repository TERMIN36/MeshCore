#include "MqttBridge.h"

#include <string.h>

#if defined(ESP32)

#include <Arduino.h>
#include <WiFi.h>
#include <mqtt_client.h>
#include <esp_idf_version.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static void copyText(char* dest, size_t cap, const char* src) {
  if (!dest || cap == 0) return;
  if (!src) src = "";
  strncpy(dest, src, cap - 1);
  dest[cap - 1] = 0;
}

void MqttBridge::eventThunk(void* arg, const char* base, int32_t event_id, void* event_data) {
  (void)base;
  static_cast<MqttBridge*>(arg)->handleEvent(event_id, event_data);
}

void MqttBridge::begin(NodePrefs* prefs, mesh::PacketManager* mgr, mesh::RTCClock* rtc, const uint8_t pub_key[32]) {
  _prefs = prefs;
  _mgr = mgr;
  _rtc = rtc;
  if (pub_key) memcpy(_pub, pub_key, 32);
  if (pub_key) {
    snprintf(_client_id, sizeof(_client_id), "mc-%02x%02x%02x%02x", pub_key[0], pub_key[1], pub_key[2], pub_key[3]);
  } else {
    copyText(_client_id, sizeof(_client_id), "mc-repeater");
  }
  applyConfig();
}

void MqttBridge::ensureTask() {
  if (_task_started) return;
  if (!_lock) _lock = xSemaphoreCreateMutex();
  if (!_queue) _queue = xQueueCreate(4, sizeof(Inbound));
  if (!_queue) return;
  // Above the Arduino loop, so a kick during onForward writes the socket before
  // the radio collision delay. Below the mqtt client task, so its read is not stalled.
  if (xTaskCreate(taskMain, "mqtt-br", 10240, this, 3, (TaskHandle_t*)&_task) == pdPASS) {
    _task_started = true;
  }
}

void MqttBridge::kick() {
  if (_task) xTaskNotifyGive((TaskHandle_t)_task);
}

void MqttBridge::applyConfig() {
  if (!_prefs) return;
  Config next{};
  next.enabled = _prefs->mqtt_enabled != 0;
  next.tls = _prefs->mqtt_tls != 0;
  next.port = _prefs->mqtt_port ? _prefs->mqtt_port : 1883;
  copyText(next.tunnel, sizeof(next.tunnel), _prefs->mqtt_tunnel);
  copyText(next.host, sizeof(next.host), _prefs->mqtt_host);
  copyText(next.user, sizeof(next.user), _prefs->mqtt_user);
  copyText(next.pass, sizeof(next.pass), _prefs->mqtt_pass);

  SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  next.ca_ready = caReadyLocked();
  bool changed = next.enabled != _cfg.enabled || next.tls != _cfg.tls || next.port != _cfg.port ||
                 strcmp(next.tunnel, _cfg.tunnel) != 0 || strcmp(next.host, _cfg.host) != 0 ||
                 strcmp(next.user, _cfg.user) != 0 || strcmp(next.pass, _cfg.pass) != 0;
  next.generation = _cfg.generation + (changed ? 1 : 0);
  _cfg = next;
  if (lock) xSemaphoreGive(lock);
  if (configured(next)) ensureTask();
}

bool MqttBridge::caReadyLocked() const {
  if (_ca_pending_valid) return _ca_pending_len > 0;
  return _ca_live_len > 0;
}

bool MqttBridge::configured(const Config& cfg) const {
  if (!cfg.enabled || !cfg.tunnel[0] || !cfg.host[0] || cfg.port == 0) return false;
  if (cfg.tls && !cfg.ca_ready) return false;
  return true;
}

bool MqttBridge::setCaCert(const char* pem, size_t len) {
  char* copy = nullptr;
  if (pem && len) {
    if (!mqttCaPemOk(pem, len)) return false;
    copy = (char*)malloc(len + 1);
    if (!copy) return false;
    memcpy(copy, pem, len);
    copy[len] = 0;
  }
  SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
  if (!lock) {
    lock = xSemaphoreCreateMutex();
    _lock = lock;
  }
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  free(_ca_pending);
  _ca_pending = copy;
  _ca_pending_len = copy ? len : 0;
  _ca_pending_valid = true;
  _cfg.ca_ready = _ca_pending_len > 0;
  _cfg.generation++;
  Config snap = _cfg;
  if (lock) xSemaphoreGive(lock);
  if (configured(snap)) ensureTask();
  return true;
}

bool MqttBridge::hasCaCert() const {
  SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  bool ready = caReadyLocked();
  if (lock) xSemaphoreGive(lock);
  return ready;
}

void MqttBridge::commitCa() {
  SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  if (_ca_pending_valid) {
    free(_ca_live);
    _ca_live = _ca_pending;
    _ca_live_len = _ca_pending_len;
    _ca_pending = nullptr;
    _ca_pending_valid = false;
  }
  if (lock) xSemaphoreGive(lock);
}

bool MqttBridge::isUp() const { return _link_up; }

bool MqttBridge::blocksSleep() const {
  if (!_prefs || !_prefs->mqtt_enabled || !_prefs->wifi_enabled || !_prefs->mqtt_tunnel[0] ||
      !_prefs->mqtt_host[0] || !_prefs->wifi_ssid[0]) {
    return false;
  }
  if (_prefs->mqtt_tls && !hasCaCert()) return false;
  return true;
}

void MqttBridge::formatStatus(char* reply, size_t cap) {
  SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  _policy.formatStatus(reply, cap, _link_up);
  if (lock) xSemaphoreGive(lock);
}

void MqttBridge::fillIdentity(MqttIdentity& id) const {
  memset(&id, 0, sizeof(id));
  if (_prefs) {
    memcpy(id.name, _prefs->node_name, MQTT_NODE_NAME_MAX);
    id.name[MQTT_NODE_NAME_MAX] = 0;
    id.has_time = _prefs->time_valid != 0 && _rtc != nullptr;
    if (id.has_time) id.unix_time = _rtc->getCurrentTime();
  }
  memcpy(id.id, _pub, 32);
}

void MqttBridge::fillRadio(MqttRadioDesc& radio) const {
  memset(&radio, 0, sizeof(radio));
  radio.noise_floor = _noise_floor;
  radio.tx_air_secs = _tx_air_secs;
  radio.rx_air_secs = _rx_air_secs;
  radio.uptime_secs = _uptime_secs;
  radio.tx_queue = _tx_queue;
  radio.batt_mv = _batt_mv;
  radio.temp_c_x10 = _temp_c_x10;
  copyText(radio.firmware, sizeof(radio.firmware), _firmware);
  if (!_prefs) return;
  radio.freq_hz = (uint32_t)(_prefs->freq * 1000000.0 + 0.5);
  radio.bw_hz = (uint32_t)(_prefs->bw * 1000.0 + 0.5);
  radio.sf = _prefs->sf;
  radio.cr = _prefs->cr;
  radio.tx_dbm = _prefs->tx_power_dbm;
  radio.forwarding = _prefs->disable_fwd == 0;
  if (_prefs->mqtt_ant_m > 0.0f) {
    float cm = _prefs->mqtt_ant_m * 100.0f;
    if (cm > 32767.0f) cm = 32767.0f;
    radio.has_ant = true;
    radio.ant_cm = (int16_t)(cm + 0.5f);
  }
  if (_prefs->node_lat != 0.0 || _prefs->node_lon != 0.0) {
    radio.has_coords = true;
    radio.lat_e7 = (int32_t)(_prefs->node_lat * 10000000.0);
    radio.lon_e7 = (int32_t)(_prefs->node_lon * 10000000.0);
  }
}

void MqttBridge::setRadioMetrics(int16_t noise_floor, uint32_t tx_air_secs, uint32_t rx_air_secs, uint32_t uptime_secs,
                                 uint32_t tx_queue, uint16_t batt_mv, int16_t temp_c_x10, const char* firmware) {
  _noise_floor = noise_floor;
  _tx_air_secs = tx_air_secs;
  _rx_air_secs = rx_air_secs;
  _uptime_secs = uptime_secs;
  _tx_queue = tx_queue;
  _batt_mv = batt_mv;
  _temp_c_x10 = temp_c_x10;
  copyText(_firmware, sizeof(_firmware), firmware);
}

void MqttBridge::onRadioTx(mesh::Packet* packet) {
  if (!_link_up || !packet || !_prefs || !_prefs->mqtt_enabled) return;
  uint8_t raw[MAX_TRANS_UNIT];
  uint8_t n = packet->writeTo(raw);
  if (n == 0) return;
  MqttIdentity id;
  fillIdentity(id);
  SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  MqttEnqueue queued = _policy.enqueuePacket(packet, raw, n, id);
  if (lock) xSemaphoreGive(lock);
  if (queued == MqttEnqueue::Queued) kick();
}

void MqttBridge::acceptInbound(const uint8_t* data, uint16_t len) {
  if (!_mgr) return;
  uint8_t raw[MAX_TRANS_UNIT];
  uint16_t raw_len = 0;
  SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  MqttRx kind = _policy.receive(data, len, raw, &raw_len);
  if (lock) xSemaphoreGive(lock);
  if (kind != MqttRx::Packet || raw_len == 0 || raw_len > 255) return;

  mesh::Packet* pkt = _mgr->allocNew();
  if (!pkt) return;
  if (!pkt->readFrom(raw, (uint8_t)raw_len)) {
    _mgr->free(pkt);
    return;
  }
  uint8_t hash[MAX_HASH_SIZE];
  pkt->calculatePacketHash(hash);
  if (!_mgr->queueInbound(pkt, millis())) return;
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  _policy.commitReceived(hash);
  if (lock) xSemaphoreGive(lock);
}

void MqttBridge::loop() {
  if (!_task_started || !_prefs) return;
  SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
  if (_link_up) {
    if (lock) xSemaphoreTake(lock, portMAX_DELAY);
    bool want_hello = _policy.needsHello();
    if (lock) xSemaphoreGive(lock);
    if (want_hello) {
      MqttIdentity id;
      MqttRadioDesc radio;
      fillIdentity(id);
      fillRadio(radio);
      if (lock) xSemaphoreTake(lock, portMAX_DELAY);
      _policy.enqueueHello(id, radio);
      if (lock) xSemaphoreGive(lock);
    }
    MqttIdentity id;
    MqttRadioDesc radio;
    fillIdentity(id);
    fillRadio(radio);
    if (lock) xSemaphoreTake(lock, portMAX_DELAY);
    _policy.pollHeartbeat(millis(), id, radio);
    if (lock) xSemaphoreGive(lock);
  }

  if (!_queue) return;
  Inbound inbound;
  while (xQueueReceive((QueueHandle_t)_queue, &inbound, 0) == pdTRUE) {
    acceptInbound(inbound.bytes, inbound.len);
  }
}

void MqttBridge::handleEvent(int32_t event_id, void* event_data) {
  auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
  // stopClient clears _client before destroy, so a late event from the old client must not reset the new one.
  if (event && event->client != (esp_mqtt_client_handle_t)_client) return;
  if (event_id == MQTT_EVENT_CONNECTED) {
    _broker_up = true;
    _link_up = false;
    _sub_pending = false;
    _sub_due = true;
  } else if (event_id == MQTT_EVENT_DISCONNECTED) {
    _broker_up = false;
    _link_up = false;
    SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
    if (lock) xSemaphoreTake(lock, portMAX_DELAY);
    dropInflightLocked();
    _policy.onDisconnected();
    if (lock) xSemaphoreGive(lock);
  } else if (event_id == MQTT_EVENT_PUBLISHED && event) {
    notePublished(event->msg_id);
  } else if (event_id == MQTT_EVENT_SUBSCRIBED) {
    _sub_pending = false;
    _sub_due = false;
    _link_up = true;
    SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
    if (lock) xSemaphoreTake(lock, portMAX_DELAY);
    _policy.onSubscribed();
    if (lock) xSemaphoreGive(lock);
  } else if (event_id == MQTT_EVENT_ERROR && event && event->error_handle) {
    const esp_mqtt_error_codes_t* err = event->error_handle;
    int flags = err->esp_tls_cert_verify_flags;
    Serial.printf("MQTT verify flags=0x%x esp=0x%x mbedtls=%d\n", flags, (unsigned)err->esp_tls_last_esp_err, err->esp_tls_stack_err);
    if (flags & 0x08) Serial.println("MQTT verify: not signed by trusted CA");
    if (flags & 0x04) Serial.println("MQTT verify: name mismatch");
    if (flags & 0x01) Serial.println("MQTT verify: expired");
    if (flags & 0x200) Serial.println("MQTT verify: not yet valid");
  } else if (event_id == MQTT_EVENT_DATA && event && event->data && event->data_len > 1) {
    if (event->current_data_offset != 0 || event->data_len != event->total_data_len) return;
    if (event->data_len > (int)MQTT_ENVELOPE_MAX) return;
    if ((uint8_t)event->data[0] != MQTT_BRIDGE_VERSION) return;
    if ((uint8_t)event->data[1] != MQTT_TYPE_PACKET) return;
    if (!_queue) return;
    Inbound inbound;
    inbound.len = (uint16_t)event->data_len;
    memcpy(inbound.bytes, event->data, event->data_len);
    xQueueSend((QueueHandle_t)_queue, &inbound, 0);
  }
}

int MqttBridge::findPubLocked(int msg_id) const {
  for (int i = 0; i < _inflight_n; i++) {
    if (_inflight[i].id == msg_id) return i;
  }
  return -1;
}

void MqttBridge::removePubLocked(int index) {
  if (index < 0 || index >= _inflight_n) return;
  _inflight[index] = _inflight[_inflight_n - 1];
  _inflight_n--;
}

bool MqttBridge::publishStuckLocked(uint32_t now) const {
  for (int i = 0; i < _inflight_n; i++) {
    if ((uint32_t)(now - _inflight[i].since) > 20000) return true;
  }
  return false;
}

void MqttBridge::dropInflightLocked() {
  for (int i = 0; i < _inflight_n; i++) _policy.onPublishFail(_inflight[i].item);
  _inflight_n = 0;
  _early_pub_id = 0;
  _sub_pending = false;
  _sub_due = false;
}

void MqttBridge::notePublished(int msg_id) {
  SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  int index = findPubLocked(msg_id);
  if (index >= 0) {
    _policy.onPublishOk(_inflight[index].item, millis());
    removePubLocked(index);
  } else {
    _early_pub_id = msg_id;
  }
  if (lock) xSemaphoreGive(lock);
}

void MqttBridge::publishOne() {
  MqttOutbound item;
  SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  // A publish still waiting for its ack must not hold the next frame.
  // Stop only when every tracked slot, and the client outbox, is already full.
  if (_inflight_n >= PUB_CAP) {
    if (lock) xSemaphoreGive(lock);
    return;
  }
  bool has = _policy.popOutbound(item);
  if (has) _early_pub_id = 0;
  if (lock) xSemaphoreGive(lock);
  if (!has) return;

  // The client task flushes its outbox only after poll_read returns, and that
  // poll waits a full second on an idle socket. enqueue therefore holds every
  // ping for up to a second on each repeater. publish writes this frame now.
  // The client lock covers one short TLS record; a full send buffer still
  // returns within network_timeout_ms and the frame is deferred below.
  int rc = -1;
  if (_client && _link_up && _live.tunnel[0]) {
    rc = esp_mqtt_client_publish((esp_mqtt_client_handle_t)_client, _live.tunnel, (const char*)item.bytes, item.len, 1, 0);
  }
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  if (rc > 0 && _early_pub_id == rc) {
    _policy.onPublishOk(item, millis());
    _early_pub_id = 0;
  } else if (rc > 0) {
    // The client already accepted the frame. Losing the slot must not send it again.
    if (_inflight_n < PUB_CAP) {
      _inflight[_inflight_n].id = rc;
      _inflight[_inflight_n].since = millis();
      _inflight[_inflight_n].item = item;
      _inflight_n++;
    } else {
      _policy.onPublishOk(item, millis());
    }
  } else {
    _policy.defer(item);
  }
  if (lock) xSemaphoreGive(lock);
}

bool MqttBridge::subscribeTunnel() {
  if (!_client || !_live.tunnel[0]) return false;
  // Publish stays on this repeater's own tunnel. Peers publish on theirs, so a
  // subscription to only ours hears our echo and nothing from the other repeaters.
  int rc = esp_mqtt_client_subscribe((esp_mqtt_client_handle_t)_client, "#", 0);
  if (rc < 0) {
    _sub_due = false;
    _last_subscribe_ms = millis();
    return false;
  }
  _sub_pending = true;
  _sub_due = false;
  _last_subscribe_ms = millis();
  return true;
}

void MqttBridge::stopClient() {
  _link_up = false;
  _broker_up = false;
  SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  dropInflightLocked();
  _policy.onDisconnected();
  if (lock) xSemaphoreGive(lock);
  if (_client) {
    esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t)_client;
    _client = nullptr;
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
  }
}

void MqttBridge::restartClient() {
  stopClient();
  _cooldown_until = millis() + 5000;
  if (_cooldown_until == 0) _cooldown_until = 1;
}

void MqttBridge::startClient(const Config& cfg) {
  _live = cfg;
  if (_live.tls && (!_ca_live || !_ca_live[0])) return;
  esp_mqtt_client_config_t mqtt{};
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  mqtt.broker.address.hostname = _live.host;
  mqtt.broker.address.port = _live.port;
  mqtt.credentials.client_id = _client_id;
  if (_live.user[0]) {
    mqtt.credentials.username = _live.user;
    mqtt.credentials.authentication.password = _live.pass;
  }
  mqtt.session.keepalive = 30;
  mqtt.network.timeout_ms = 10000;
  mqtt.network.reconnect_timeout_ms = 5000;
  mqtt.buffer.size = 1024;
  mqtt.buffer.out_size = 1024;
  mqtt.outbox.limit = 4;
  if (_live.tls) {
    mqtt.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
    mqtt.broker.verification.certificate = _ca_live;
    mqtt.broker.verification.certificate_len = 0;
    mqtt.task.stack_size = 10240;
  }
#else
  mqtt.host = _live.host;
  mqtt.port = _live.port;
  mqtt.client_id = _client_id;
  mqtt.keepalive = 30;
  mqtt.network_timeout_ms = 10000;
  mqtt.reconnect_timeout_ms = 5000;
  mqtt.buffer_size = 1024;
  mqtt.out_buffer_size = 1024;
  mqtt.message_retransmit_timeout = 10000;
  if (_live.user[0]) {
    mqtt.username = _live.user;
    mqtt.password = _live.pass;
  }
  if (_live.tls) {
    mqtt.transport = MQTT_TRANSPORT_OVER_SSL;
    mqtt.cert_pem = _ca_live;
    mqtt.cert_len = 0;
    mqtt.task_stack = 10240;
  }
#endif
  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt);
  if (!client) return;
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, (esp_event_handler_t)eventThunk, this);
  _client = client;
  if (esp_mqtt_client_start(client) != ESP_OK) {
    _client = nullptr;
    esp_mqtt_client_destroy(client);
    return;
  }
}

void MqttBridge::taskMain(void* arg) {
  static_cast<MqttBridge*>(arg)->pump();
}

void MqttBridge::pump() {
  uint32_t applied = 0;
  for (;;) {
    Config cfg;
    SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
    if (lock) xSemaphoreTake(lock, portMAX_DELAY);
    cfg = _cfg;
    if (lock) xSemaphoreGive(lock);

    if (!configured(cfg)) {
      if (_client) stopClient();
      if (cfg.generation != applied) commitCa();
      applied = cfg.generation;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (cfg.generation != applied) {
      if (_client) stopClient();
      commitCa();
      applied = cfg.generation;
    }

    // The repeater station owns the association. Wait until it has an address.
    if (WiFi.status() != WL_CONNECTED) {
      if (_client) stopClient();
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    if (_cooldown_until && (int32_t)(_cooldown_until - millis()) > 0) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    if (!_client) startClient(cfg);
    bool publish_stuck = false;
    SemaphoreHandle_t held = (SemaphoreHandle_t)_lock;
    if (held) xSemaphoreTake(held, portMAX_DELAY);
    publish_stuck = publishStuckLocked(millis());
    if (held) xSemaphoreGive(held);
    if (publish_stuck) {
      restartClient();
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    if (_broker_up && !_link_up && _client && _live.tunnel[0]) {
      if (_sub_pending && (uint32_t)(millis() - _last_subscribe_ms) > 10000) {
        restartClient();
        vTaskDelay(pdMS_TO_TICKS(250));
        continue;
      }
      if (!_sub_pending && (_sub_due || (uint32_t)(millis() - _last_subscribe_ms) > 2000)) subscribeTunnel();
    }
    if (_link_up) {
      for (int n = 0; n < PUB_CAP; n++) publishOne();
    }
    // onRadioTx notifies this task. The timeout is only the idle housekeeping tick.
    // Light sleep needs a longer idle gap; a waiting packet still wakes this task.
    uint32_t idle_ms = (_prefs && _prefs->wifi_ps == WIFI_POWER_LIGHT) ? 100 : 20;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(idle_ms));
  }
}

#else

void MqttBridge::begin(NodePrefs*, mesh::PacketManager*, mesh::RTCClock*, const uint8_t*) {}
void MqttBridge::loop() {}
void MqttBridge::onRadioTx(mesh::Packet*) {}
void MqttBridge::applyConfig() {}
bool MqttBridge::setCaCert(const char* pem, size_t len) {
  if (!pem || len == 0) return true;
  return mqttCaPemOk(pem, len);
}
bool MqttBridge::hasCaCert() const { return false; }
bool MqttBridge::isUp() const { return false; }
bool MqttBridge::blocksSleep() const { return false; }
void MqttBridge::formatStatus(char* reply, size_t cap) {
  if (!reply || cap == 0) return;
  strncpy(reply, "> down", cap - 1);
  reply[cap - 1] = 0;
}
void MqttBridge::setRadioMetrics(int16_t, uint32_t, uint32_t, uint32_t, uint32_t, uint16_t, int16_t, const char*) {}

#endif
