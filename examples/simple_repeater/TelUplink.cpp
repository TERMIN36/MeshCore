#include "TelUplink.h"

#include <helpers/CommonCLI.h>
#include "Packet.h"
#include <stdio.h>
#include <string.h>

#if defined(ESP32)

#include <WiFi.h>
#include <mqtt_client.h>
#include <esp_idf_version.h>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static const char TEL_HOST[] = "mqtt.meshcoretel.ru";
static const uint16_t TEL_PORT = 1883;
static const uint32_t TEL_STATUS_MS = 300000;
static const uint32_t TEL_MIN_UNIX = 1735689600UL;  // 2025-01-01 UTC
static const unsigned long TEL_RETRY_BASE_MS = 10000;
static const unsigned long TEL_RETRY_MAX_MS = 300000;

static unsigned long retryDelay(uint8_t failures) {
  unsigned long delay_ms = TEL_RETRY_BASE_MS;
  if (failures > 0) {
    uint8_t shifts = failures - 1;
    if (shifts > 5) shifts = 5;
    delay_ms <<= shifts;
  }
  if (delay_ms > TEL_RETRY_MAX_MS) delay_ms = TEL_RETRY_MAX_MS;
  return delay_ms;
}

static void hexUpper(const uint8_t* src, size_t len, char* dst, size_t dst_size) {
  if (!dst || dst_size == 0) return;
  size_t di = 0;
  for (size_t i = 0; i < len && di + 2 < dst_size; i++) {
    snprintf(&dst[di], dst_size - di, "%02X", src[i]);
    di += 2;
  }
  dst[di < dst_size ? di : dst_size - 1] = 0;
}

static void escapeJson(const char* input, char* output, size_t output_size) {
  if (!output || output_size == 0) return;
  size_t oi = 0;
  for (size_t i = 0; input && input[i] && oi + 1 < output_size; i++) {
    char c = input[i];
    const char* escape = nullptr;
    if (c == '\"') escape = "\\\"";
    else if (c == '\\') escape = "\\\\";
    else if (c == '\n') escape = "\\n";
    else if (c == '\r') escape = "\\r";
    else if (c == '\t') escape = "\\t";
    if (escape) {
      for (size_t j = 0; escape[j] && oi + 1 < output_size; j++) output[oi++] = escape[j];
      continue;
    }
    if ((unsigned char)c < 0x20) {
      output[oi++] = '_';
      continue;
    }
    output[oi++] = c;
  }
  output[oi] = 0;
}

static void formatIso(uint32_t unix_time, char* dst, size_t dst_size) {
  if (!dst || dst_size == 0) return;
  dst[0] = 0;
  time_t ts = (time_t)unix_time;
  struct tm tm_utc;
  if (!gmtime_r(&ts, &tm_utc)) return;
  strftime(dst, dst_size, "%Y-%m-%dT%H:%M:%S", &tm_utc);
  size_t len = strlen(dst);
  if (len + 8 < dst_size) memcpy(&dst[len], ".000000", 8);
}

static bool iataReady(const char* iata) {
  if (!iata || !iata[0] || strcmp(iata, "UNSET") == 0) return false;
  return true;
}

void TelUplink::eventThunk(void* arg, const char* base, int32_t event_id, void* event_data) {
  (void)base;
  static_cast<TelUplink*>(arg)->handleEvent(event_id, event_data);
}

void TelUplink::taskMain(void* arg) {
  static_cast<TelUplink*>(arg)->pump();
}

void TelUplink::begin(NodePrefs* prefs, mesh::RTCClock* rtc, const uint8_t pub_key[32]) {
  _prefs = prefs;
  _rtc = rtc;
  if (pub_key) hexUpper(pub_key, 32, _device_id, sizeof(_device_id));
  if (_device_id[0]) {
    snprintf(_client_id, sizeof(_client_id), "mqtt_meshcoretel-%.6s", _device_id);
  } else {
    strncpy(_client_id, "mqtt_meshcoretel", sizeof(_client_id) - 1);
  }
  applyConfig();
}

void TelUplink::ensureTask() {
  if (_task_started) return;
  if (!_lock) _lock = xSemaphoreCreateMutex();
  if (xTaskCreate(taskMain, "mqtt-tel", 8192, this, 2, (TaskHandle_t*)&_task) == pdPASS) {
    _task_started = true;
  }
}

void TelUplink::applyConfig() {
  if (_prefs && _prefs->tel_enabled && iataReady(_prefs->tel_iata)) ensureTask();
}

void TelUplink::loop(const TelView& view) {
  if (!_lock && view.enabled) ensureTask();
  SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  _view = view;
  if (lock) xSemaphoreGive(lock);
  if (view.enabled && iataReady(view.iata)) ensureTask();
}

void TelUplink::refreshTopics(const char* iata) {
  strncpy(_live_iata, iata ? iata : "", sizeof(_live_iata) - 1);
  _live_iata[sizeof(_live_iata) - 1] = 0;
  snprintf(_status_topic, sizeof(_status_topic), "meshcore/%s/%s/status", _live_iata, _device_id);
  snprintf(_packets_topic, sizeof(_packets_topic), "meshcore/%s/%s/packets", _live_iata, _device_id);
}

bool TelUplink::publishText(const char* topic, const char* payload, bool retain) {
  if (!topic || !payload) return false;
  SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  int rc = -1;
  if (_client && _up) {
    rc = esp_mqtt_client_enqueue((esp_mqtt_client_handle_t)_client, topic, payload, 0, 1, retain ? 1 : 0, true);
  }
  if (lock) xSemaphoreGive(lock);
  return rc >= 0;
}

void TelUplink::publishStatus(const TelView& view) {
  char origin[80];
  char model[48];
  char firmware[96];
  char radio[48];
  char ts[40];
  const char* name = view.name[0] ? view.name : _device_id;
  escapeJson(name, origin, sizeof(origin));
  escapeJson(view.model, model, sizeof(model));
  escapeJson(view.firmware, firmware, sizeof(firmware));
  snprintf(radio, sizeof(radio), "%.6f,%.1f,%u,%u", (double)view.freq, (double)view.bw, view.sf, view.cr);
  formatIso(view.unix_time, ts, sizeof(ts));

  static char payload[768];
  int len = snprintf(payload, sizeof(payload),
                     "{\"status\":\"online\",\"timestamp\":\"%s\",\"origin\":\"%s\",\"origin_id\":\"%s\","
                     "\"model\":\"%s\",\"firmware_version\":\"%s\",\"radio\":\"%s\",\"client_version\":\"%s\","
                     "\"stats\":{\"battery_mv\":%d,\"uptime_secs\":%lu,\"errors\":%u,\"queue_len\":%lu,"
                     "\"noise_floor\":%d,\"tx_air_secs\":%lu,\"rx_air_secs\":%lu,\"recv_errors\":%lu}}",
                     ts, origin, _device_id, model, firmware, radio, firmware,
                     view.battery_mv, (unsigned long)view.uptime_secs, view.error_flags,
                     (unsigned long)view.queue_len, view.noise_floor,
                     (unsigned long)view.tx_air_secs, (unsigned long)view.rx_air_secs,
                     (unsigned long)view.recv_errors);
  if (len <= 0 || (size_t)len >= sizeof(payload)) return;
  publishText(_status_topic, payload, true);
}

void TelUplink::onRadio(const mesh::Packet* packet, bool is_tx, int rssi, float snr, int score, int duration) {
  if (!_up || !packet || !_prefs || !_prefs->tel_enabled || !iataReady(_prefs->tel_iata)) return;
  if (is_tx && !_prefs->tel_tx) return;
  if (!_rtc) return;
  uint32_t now = _rtc->getCurrentTime();
  if (now < TEL_MIN_UNIX) return;

  uint8_t raw[MAX_TRANS_UNIT];
  int raw_len = packet->writeTo(raw);
  if (raw_len <= 0) return;
  char raw_hex[MAX_TRANS_UNIT * 2 + 1];
  hexUpper(raw, (size_t)raw_len, raw_hex, sizeof(raw_hex));

  uint8_t packet_hash[MAX_HASH_SIZE];
  packet->calculatePacketHash(packet_hash);
  char hash_hex[MAX_HASH_SIZE * 2 + 1];
  hexUpper(packet_hash, MAX_HASH_SIZE, hash_hex, sizeof(hash_hex));

  char ts[40];
  formatIso(now, ts, sizeof(ts));
  time_t ts_num = (time_t)now;
  struct tm tm_utc;
  char time_only[16];
  char date_only[16];
  time_only[0] = date_only[0] = 0;
  if (gmtime_r(&ts_num, &tm_utc)) {
    strftime(time_only, sizeof(time_only), "%H:%M:%S", &tm_utc);
    strftime(date_only, sizeof(date_only), "%d/%m/%Y", &tm_utc);
  }
  char origin[80];
  const char* name = (_prefs->node_name[0]) ? _prefs->node_name : _device_id;
  escapeJson(name, origin, sizeof(origin));

  static char payload[1280];
  int len = -1;
  if (packet->isRouteDirect() && packet->path_len > 0) {
    char path_info[128];
    snprintf(path_info, sizeof(path_info), "path_%dx%d_%db", (int)packet->getPathHashCount(),
             (int)packet->getPathHashSize(), (int)packet->getPathByteLen());
    if (score >= 0) {
      len = snprintf(payload, sizeof(payload),
                     "{\"origin\":\"%s\",\"origin_id\":\"%s\",\"timestamp\":\"%s\",\"type\":\"PACKET\","
                     "\"direction\":\"%s\",\"time\":\"%s\",\"date\":\"%s\",\"len\":\"%d\",\"packet_type\":\"%u\","
                     "\"route\":\"D\",\"payload_len\":\"%u\",\"raw\":\"%s\",\"SNR\":\"%.1f\",\"RSSI\":\"%d\","
                     "\"score\":\"%d\",\"duration\":\"%d\",\"hash\":\"%s\",\"path\":\"%s\"}",
                     origin, _device_id, ts, is_tx ? "tx" : "rx", time_only, date_only, raw_len,
                     packet->getPayloadType(), packet->payload_len, raw_hex, snr, rssi, score, duration, hash_hex,
                     path_info);
    } else {
      len = snprintf(payload, sizeof(payload),
                     "{\"origin\":\"%s\",\"origin_id\":\"%s\",\"timestamp\":\"%s\",\"type\":\"PACKET\","
                     "\"direction\":\"%s\",\"time\":\"%s\",\"date\":\"%s\",\"len\":\"%d\",\"packet_type\":\"%u\","
                     "\"route\":\"D\",\"payload_len\":\"%u\",\"raw\":\"%s\",\"SNR\":\"%.1f\",\"RSSI\":\"%d\","
                     "\"hash\":\"%s\",\"path\":\"%s\"}",
                     origin, _device_id, ts, is_tx ? "tx" : "rx", time_only, date_only, raw_len,
                     packet->getPayloadType(), packet->payload_len, raw_hex, snr, rssi, hash_hex, path_info);
    }
  } else if (score >= 0) {
    len = snprintf(payload, sizeof(payload),
                   "{\"origin\":\"%s\",\"origin_id\":\"%s\",\"timestamp\":\"%s\",\"type\":\"PACKET\","
                   "\"direction\":\"%s\",\"time\":\"%s\",\"date\":\"%s\",\"len\":\"%d\",\"packet_type\":\"%u\","
                   "\"route\":\"F\",\"payload_len\":\"%u\",\"raw\":\"%s\",\"SNR\":\"%.1f\",\"RSSI\":\"%d\","
                   "\"score\":\"%d\",\"duration\":\"%d\",\"hash\":\"%s\"}",
                   origin, _device_id, ts, is_tx ? "tx" : "rx", time_only, date_only, raw_len,
                   packet->getPayloadType(), packet->payload_len, raw_hex, snr, rssi, score, duration, hash_hex);
  } else {
    len = snprintf(payload, sizeof(payload),
                   "{\"origin\":\"%s\",\"origin_id\":\"%s\",\"timestamp\":\"%s\",\"type\":\"PACKET\","
                   "\"direction\":\"%s\",\"time\":\"%s\",\"date\":\"%s\",\"len\":\"%d\",\"packet_type\":\"%u\","
                   "\"route\":\"F\",\"payload_len\":\"%u\",\"raw\":\"%s\",\"SNR\":\"%.1f\",\"RSSI\":\"%d\","
                   "\"hash\":\"%s\"}",
                   origin, _device_id, ts, is_tx ? "tx" : "rx", time_only, date_only, raw_len,
                   packet->getPayloadType(), packet->payload_len, raw_hex, snr, rssi, hash_hex);
  }
  if (len <= 0 || (size_t)len >= sizeof(payload)) return;
  publishText(_packets_topic, payload, false);
}

void TelUplink::handleEvent(int32_t event_id, void* event_data) {
  if (_stopping) return;
  auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
  if (event && event->client != (esp_mqtt_client_handle_t)_client) return;
  if (event_id == MQTT_EVENT_CONNECTED) {
    _up = true;
    _failures = 0;
    _cooldown_until = 0;
    Serial.println("Tel mqtt connected");
  } else if (event_id == MQTT_EVENT_DISCONNECTED || event_id == MQTT_EVENT_ERROR) {
    _up = false;
    _announced = false;
    if (_failures < 10) _failures++;
    _need_restart = true;
    uint32_t wait = (uint32_t)retryDelay(_failures);
    _cooldown_until = millis() + wait;
    if (_cooldown_until == 0) _cooldown_until = 1;
    Serial.println(event_id == MQTT_EVENT_ERROR ? "Tel mqtt error" : "Tel mqtt disconnected");
  }
}

void TelUplink::stopClient() {
  _stopping = true;
  _up = false;
  _announced = false;
  SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
  esp_mqtt_client_handle_t client = nullptr;
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  client = (esp_mqtt_client_handle_t)_client;
  _client = nullptr;
  if (lock) xSemaphoreGive(lock);
  if (client) {
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
  }
  _need_restart = false;
  _stopping = false;
}

bool TelUplink::startClient(const TelView& view) {
  refreshTopics(view.iata);
  strncpy(_live_host, view.host[0] ? view.host : TEL_HOST, sizeof(_live_host) - 1);
  _live_host[sizeof(_live_host) - 1] = 0;
  strncpy(_live_user, view.user, sizeof(_live_user) - 1);
  _live_user[sizeof(_live_user) - 1] = 0;
  strncpy(_live_pass, view.pass, sizeof(_live_pass) - 1);
  _live_pass[sizeof(_live_pass) - 1] = 0;
  _live_port = view.port ? view.port : TEL_PORT;
  char origin[80];
  char model[48];
  char firmware[96];
  char radio[48];
  char ts[40];
  const char* name = view.name[0] ? view.name : _device_id;
  escapeJson(name, origin, sizeof(origin));
  escapeJson(view.model, model, sizeof(model));
  escapeJson(view.firmware, firmware, sizeof(firmware));
  snprintf(radio, sizeof(radio), "%.6f,%.1f,%u,%u", (double)view.freq, (double)view.bw, view.sf, view.cr);
  formatIso(view.unix_time, ts, sizeof(ts));
  snprintf(_offline, sizeof(_offline),
           "{\"status\":\"offline\",\"timestamp\":\"%s\",\"origin\":\"%s\",\"origin_id\":\"%s\",\"model\":\"%s\","
           "\"firmware_version\":\"%s\",\"radio\":\"%s\",\"client_version\":\"%s\"}",
           ts, origin, _device_id, model, firmware, radio, firmware);

  esp_mqtt_client_config_t mqtt{};
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  mqtt.broker.address.hostname = _live_host;
  mqtt.broker.address.port = _live_port;
  mqtt.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
  mqtt.credentials.client_id = _client_id;
  if (_live_user[0]) {
    mqtt.credentials.username = _live_user;
    mqtt.credentials.authentication.password = _live_pass;
  }
  mqtt.session.keepalive = 30;
  mqtt.session.last_will.topic = _status_topic;
  mqtt.session.last_will.msg = _offline;
  mqtt.session.last_will.qos = 1;
  mqtt.session.last_will.retain = 1;
  mqtt.network.timeout_ms = 10000;
  mqtt.network.reconnect_timeout_ms = 10000;
  mqtt.network.disable_auto_reconnect = true;
  mqtt.buffer.size = 768;
  mqtt.buffer.out_size = 1280;
  mqtt.outbox.limit = 2;
#else
  mqtt.host = _live_host;
  mqtt.port = _live_port;
  mqtt.transport = MQTT_TRANSPORT_OVER_TCP;
  mqtt.client_id = _client_id;
  if (_live_user[0]) {
    mqtt.username = _live_user;
    mqtt.password = _live_pass;
  }
  mqtt.keepalive = 30;
  mqtt.lwt_topic = _status_topic;
  mqtt.lwt_msg = _offline;
  mqtt.lwt_qos = 1;
  mqtt.lwt_retain = 1;
  mqtt.network_timeout_ms = 10000;
  mqtt.reconnect_timeout_ms = 10000;
  mqtt.disable_auto_reconnect = true;
  mqtt.buffer_size = 768;
  mqtt.out_buffer_size = 1280;
#endif

  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt);
  if (!client) return false;
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, (esp_event_handler_t)eventThunk, this);
  SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
  _client = client;
  if (lock) xSemaphoreGive(lock);
  if (esp_mqtt_client_start(client) != ESP_OK) {
    stopClient();
    return false;
  }
  return true;
}

void TelUplink::pump() {
  for (;;) {
    TelView view;
    SemaphoreHandle_t lock = (SemaphoreHandle_t)_lock;
    if (lock) xSemaphoreTake(lock, portMAX_DELAY);
    view = _view;
    if (lock) xSemaphoreGive(lock);

    bool want = view.enabled && iataReady(view.iata);
    bool wifi = WiFi.status() == WL_CONNECTED;
    if (!want || !wifi || !view.time_ok) {
      if (_client) stopClient();
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    bool endpoint_changed = strcmp(_live_iata, view.iata) != 0 || strcmp(_live_host, view.host) != 0 ||
                            _live_port != view.port || strcmp(_live_user, view.user) != 0 ||
                            strcmp(_live_pass, view.pass) != 0;
    if (_client && endpoint_changed) stopClient();
    if (_need_restart && _client) stopClient();

    uint32_t now_ms = millis();

    if (_cooldown_until && !_client && (int32_t)(_cooldown_until - now_ms) > 0) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    if (!_client && !startClient(view)) {
      if (_failures < 10) _failures++;
      _cooldown_until = millis() + (uint32_t)retryDelay(_failures);
      if (_cooldown_until == 0) _cooldown_until = 1;
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    if (_up && !_announced) {
      publishStatus(view);
      _announced = true;
      _last_status_ms = millis();
      if (_last_status_ms == 0) _last_status_ms = 1;
    } else if (_up && (uint32_t)(millis() - _last_status_ms) >= TEL_STATUS_MS) {
      publishStatus(view);
      _last_status_ms = millis();
    } else if (!_up) {
      _announced = false;
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

TelLink TelUplink::phase() const {
  if (!_prefs || !_prefs->tel_enabled) return TelLink::Off;
  if (!iataReady(_prefs->tel_iata)) return TelLink::NeedIata;
  if (WiFi.status() != WL_CONNECTED) return TelLink::WaitWifi;
  if (!_view.time_ok) return TelLink::WaitTime;
  if (_up) return TelLink::Up;
  if (_client) return TelLink::Connecting;
  return TelLink::Retry;
}

bool TelUplink::blocksSleep() const {
  return _prefs && _prefs->tel_enabled && iataReady(_prefs->tel_iata) && _prefs->wifi_enabled && _prefs->wifi_ssid[0];
}

#else

void TelUplink::begin(NodePrefs*, mesh::RTCClock*, const uint8_t*) {}
void TelUplink::applyConfig() {}
void TelUplink::loop(const TelView&) {}
void TelUplink::onRadio(const mesh::Packet*, bool, int, float, int, int) {}
TelLink TelUplink::phase() const { return TelLink::Off; }
bool TelUplink::blocksSleep() const { return false; }

#endif
