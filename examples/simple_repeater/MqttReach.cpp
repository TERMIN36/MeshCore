#include "MqttReach.h"

#include <helpers/CommonCLI.h>
#include <stdio.h>
#include <string.h>

#if defined(ESP32)
#include <WiFi.h>
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static constexpr uint32_t REACH_OK_MS = 30000;
static constexpr uint32_t REACH_FAIL_MS = 10000;
static constexpr uint32_t REACH_CONNECT_MS = 8000;

struct ReachTarget {
  char host[64];
  uint16_t port;
  uint8_t wifi;
  uint8_t mqtt_up;
  uint32_t gen;
};

struct ReachView {
  char ip[32];
  char tcp[24];
};

static SemaphoreHandle_t reach_lock = nullptr;
static ReachTarget reach_pending{};
static ReachView reach_latest{};
static ReachTarget reach_shadow{};
static uint32_t reach_not_before = 0;
static bool reach_started = false;

static void reachText(char* dest, size_t cap, const char* text) {
  if (!dest || cap == 0) return;
  strncpy(dest, text ? text : "", cap - 1);
  dest[cap - 1] = 0;
}

static bool reachTake() {
  return reach_lock && xSemaphoreTake(reach_lock, portMAX_DELAY) == pdTRUE;
}

static void reachPublish(uint32_t gen, const char* ip, const char* tcp) {
  if (!reachTake()) return;
  bool changed = false;
  char host[64];
  uint16_t port = 0;
  host[0] = 0;
  if (gen == reach_pending.gen) {
    changed = strcmp(reach_latest.ip, ip) != 0 || strcmp(reach_latest.tcp, tcp) != 0;
    reachText(reach_latest.ip, sizeof(reach_latest.ip), ip);
    reachText(reach_latest.tcp, sizeof(reach_latest.tcp), tcp);
    strncpy(host, reach_pending.host, sizeof(host) - 1);
    host[sizeof(host) - 1] = 0;
    port = reach_pending.port;
  }
  xSemaphoreGive(reach_lock);
  if (changed) Serial.printf("MQTT reach %s:%u ip=%s tcp=%s\n", host, port, ip, tcp);
}

static bool reachTcp(const IPAddress& ip, uint16_t port) {
  WiFiClient client;
  bool open = client.connect(ip, port, (int32_t)REACH_CONNECT_MS) > 0;
  client.stop();
  return open;
}

static void reachSchedule(uint32_t gen, uint8_t probed_up, uint32_t gap_ms) {
  if (!reachTake()) return;
  if (reach_pending.gen == gen) {
    // The session changed while this probe ran. Run again instead of keeping a stale wait.
    if (reach_pending.mqtt_up != probed_up) reach_not_before = 0;
    else reach_not_before = millis() + gap_ms;
  }
  xSemaphoreGive(reach_lock);
}

static void reachTask(void*) {
  uint32_t seen_gen = 0;
  for (;;) {
    ReachTarget cur{};
    uint32_t not_before = 0;
    if (reachTake()) {
      cur = reach_pending;
      not_before = reach_not_before;
      xSemaphoreGive(reach_lock);
    }
    if (!cur.wifi || !cur.host[0] || !cur.port) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    uint32_t now = millis();
    if (cur.gen == seen_gen && not_before && (int32_t)(not_before - now) > 0) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    IPAddress ip;
    if (!WiFi.hostByName(cur.host, ip) || ip == IPAddress((uint32_t)0)) {
      uint8_t mqtt_up = cur.mqtt_up;
      if (reachTake()) {
        if (reach_pending.gen == cur.gen) mqtt_up = reach_pending.mqtt_up;
        xSemaphoreGive(reach_lock);
      }
      reachPublish(cur.gen, "не резолвится", mqtt_up ? "доступен" : "недоступен");
      reachSchedule(cur.gen, mqtt_up, REACH_FAIL_MS);
      seen_gen = cur.gen;
      continue;
    }

    char ip_text[32];
    snprintf(ip_text, sizeof(ip_text), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);

    uint8_t mqtt_up = cur.mqtt_up;
    if (reachTake()) {
      if (reach_pending.gen == cur.gen) mqtt_up = reach_pending.mqtt_up;
      xSemaphoreGive(reach_lock);
    }
    // An open MQTT session already proved the port. A second handshake can be
    // refused when the broker allows one connection, so skip it while the session is up.
    bool open = mqtt_up || reachTcp(ip, cur.port);
    reachPublish(cur.gen, ip_text, open ? "доступен" : "недоступен");
    reachSchedule(cur.gen, mqtt_up, open ? REACH_OK_MS : REACH_FAIL_MS);
    seen_gen = cur.gen;
  }
}
#endif

void MqttReach::begin(NodePrefs* prefs) {
  _prefs = prefs;
#if defined(ESP32)
  if (reach_started) return;
  reach_lock = xSemaphoreCreateMutex();
  if (!reach_lock) return;
  reachText(reach_latest.ip, sizeof(reach_latest.ip), "проверка");
  reachText(reach_latest.tcp, sizeof(reach_latest.tcp), "проверка");
  if (xTaskCreate(reachTask, "mqtt-reach", 8192, nullptr, 1, nullptr) != pdPASS) {
    reachText(reach_latest.ip, sizeof(reach_latest.ip), "—");
    reachText(reach_latest.tcp, sizeof(reach_latest.tcp), "—");
    return;
  }
  reach_started = true;
#endif
}

void MqttReach::poll(bool mqtt_up) {
#if defined(ESP32)
  if (!reach_started || !_prefs || !reach_lock) return;
  ReachTarget next{};
  if (_prefs->mqtt_host[0]) strncpy(next.host, _prefs->mqtt_host, sizeof(next.host) - 1);
  next.port = _prefs->mqtt_port;
  next.wifi = WiFi.status() == WL_CONNECTED ? 1 : 0;
  next.mqtt_up = mqtt_up ? 1 : 0;
  next.gen = reach_shadow.gen;
  if (memcmp(&next, &reach_shadow, sizeof(next)) == 0) return;
  if (xSemaphoreTake(reach_lock, pdMS_TO_TICKS(10)) != pdTRUE) return;
  bool endpoint = strcmp(next.host, reach_shadow.host) != 0 || next.port != reach_shadow.port || next.wifi != reach_shadow.wifi;
  if (endpoint) next.gen = reach_shadow.gen + 1;
  if (!endpoint && next.mqtt_up != reach_shadow.mqtt_up) reach_not_before = 0;
  reach_shadow = next;
  reach_pending = next;
  if (endpoint) {
    reach_not_before = 0;
    if (!next.wifi || !next.host[0] || !next.port) {
      reachText(reach_latest.ip, sizeof(reach_latest.ip), "—");
      reachText(reach_latest.tcp, sizeof(reach_latest.tcp), "—");
    } else {
      reachText(reach_latest.ip, sizeof(reach_latest.ip), "проверка");
      reachText(reach_latest.tcp, sizeof(reach_latest.tcp), "проверка");
    }
  }
  xSemaphoreGive(reach_lock);
#else
  (void)mqtt_up;
#endif
}

void MqttReach::copyStatus(char* ip, size_t ip_cap, char* tcp, size_t tcp_cap) const {
  if (ip && ip_cap) ip[0] = 0;
  if (tcp && tcp_cap) tcp[0] = 0;
#if defined(ESP32)
  if (!reach_lock || xSemaphoreTake(reach_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
    if (ip && ip_cap) strncpy(ip, "—", ip_cap - 1);
    if (tcp && tcp_cap) strncpy(tcp, "—", tcp_cap - 1);
    return;
  }
  if (ip && ip_cap) {
    strncpy(ip, reach_latest.ip, ip_cap - 1);
    ip[ip_cap - 1] = 0;
  }
  if (tcp && tcp_cap) {
    strncpy(tcp, reach_latest.tcp, tcp_cap - 1);
    tcp[tcp_cap - 1] = 0;
  }
  xSemaphoreGive(reach_lock);
#else
  if (ip && ip_cap) strncpy(ip, "—", ip_cap - 1);
  if (tcp && tcp_cap) strncpy(tcp, "—", tcp_cap - 1);
#endif
}
