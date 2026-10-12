#include "RepeaterWeb.h"

#include "MyMesh.h"
#include <helpers/RadioProfiles.h>
#include "NtpClock.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern MyMesh the_mesh;

#if defined(ESP32)
#include <atomic>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <mbedtls/base64.h>
#ifndef DISABLE_WIFI_OTA
#include <Update.h>
#endif

static const size_t PAGE_BYTES = 73728;
static const size_t LIVE_HTML_BYTES = 20480;
static const size_t LIVE_JSON_BYTES = 24576;
static const size_t STATS_JSON_BYTES = 8192;
// Contiguous heap needed beyond the page itself for TCP/WS while serving HTML.
static const size_t PAGE_HEAP_RESERVE = 24576;

static AsyncWebServer* server = nullptr;
static AsyncWebSocket* socket = nullptr;
static uint32_t server_ip = 0;
static char* live_html = nullptr;
static char* live_json = nullptr;

static const size_t CMD_CAP = 160;
static char submit_cmd[CMD_CAP];
static char result_cmd[2][CMD_CAP];
static char result_reply[2][CMD_CAP];
static std::atomic<uint32_t> submit_seq{0};
static std::atomic<uint32_t> result_seq{0};
static std::atomic<uint32_t> result_slot{0};
static std::atomic<uint32_t> web_reboot_at{0};
static char web_reboot_cmd[CMD_CAP];
static std::atomic_flag submit_gate = ATOMIC_FLAG_INIT;
static RepeaterPageInfo page_buf[2];
static char admin_pass[2][17];
static std::atomic<uint32_t> page_slot{0};
static uint32_t last_snapshot_ms = 0;
static uint32_t session_key = 0;
static char session_pass[17];
static uint32_t last_push_ms = 0;
static uint32_t pushed_submit = 0;
static uint32_t pushed_result = 0;

struct MqttJob {
  char host[64];
  char user[32];
  char pass[40];
  uint16_t port;
  uint8_t set_pass;
  uint32_t seq;
  size_t pem_len;
  char pem[1];
};

static std::atomic<MqttJob*> mqtt_job{nullptr};
static std::atomic<uint32_t> mqtt_seq{0};
static std::atomic<uint32_t> mqtt_off_seq{0};
static std::atomic<uint8_t> mqtt_off{0};
static char mqtt_note[120];
static char tel_note[120];

struct TelJob {
  char iata[8];
  char host[64];
  char user[32];
  char bpass[40];
  uint16_t port;
  uint8_t enable;
  uint8_t tx;
  uint32_t seq;
};

static std::atomic<TelJob*> tel_job{nullptr};
static std::atomic<uint32_t> tel_seq{0};
static std::atomic<uint8_t> tel_hold{0};
static uint8_t tel_hold_en;
static uint8_t tel_hold_tx;
static uint16_t tel_hold_port;
static char tel_hold_iata[8];
static char tel_hold_host[64];
static char tel_hold_user[32];
static char tel_hold_bpass[40];

static char radio_note[120];
static char node_note[120];
static char power_note[120];
static char region_note[120];
static std::atomic<uint8_t> power_job{0xFF};
static std::atomic<uint8_t> power_hold{0xFF};
static std::atomic<RepeaterRadioForm*> radio_job{nullptr};
static std::atomic<RepeaterNodeForm*> node_job{nullptr};
static std::atomic<RepeaterRegionForm*> region_job{nullptr};
static std::atomic<uint8_t> radio_hold{0};
static std::atomic<uint8_t> radio_reboot{0};
static std::atomic<uint8_t> node_hold{0};
static std::atomic<uint8_t> region_hold{0};
static std::atomic<uint8_t> region_reload{0};
static RepeaterRadioForm radio_hold_form;
static RepeaterNodeForm node_hold_form;
static RepeaterRegionForm region_hold_form;

static void setMqttNote(const char* text) {
  strncpy(mqtt_note, text ? text : "", sizeof(mqtt_note) - 1);
  mqtt_note[sizeof(mqtt_note) - 1] = 0;
}

static void setTelNote(const char* text) {
  strncpy(tel_note, text ? text : "", sizeof(tel_note) - 1);
  tel_note[sizeof(tel_note) - 1] = 0;
}

static void setRadioNote(const char* text) {
  strncpy(radio_note, text ? text : "", sizeof(radio_note) - 1);
  radio_note[sizeof(radio_note) - 1] = 0;
}

static void setPowerNote(const char* text) {
  strncpy(power_note, text ? text : "", sizeof(power_note) - 1);
  power_note[sizeof(power_note) - 1] = 0;
}

static void setNodeNote(const char* text) {
  strncpy(node_note, text ? text : "", sizeof(node_note) - 1);
  node_note[sizeof(node_note) - 1] = 0;
}

static void setRegionNote(const char* text) {
  strncpy(region_note, text ? text : "", sizeof(region_note) - 1);
  region_note[sizeof(region_note) - 1] = 0;
}

static void syncSession() {
  uint32_t slot = page_slot.load(std::memory_order_acquire);
  if (strcmp(session_pass, admin_pass[slot]) == 0 && session_key) return;
  strncpy(session_pass, admin_pass[slot], sizeof(session_pass) - 1);
  session_pass[sizeof(session_pass) - 1] = 0;
  session_key = esp_random();
  if (!session_key) session_key = 1;
}

static bool commandPending();

static void publishSnapshot() {
  uint32_t slot = page_slot.load(std::memory_order_relaxed) ^ 1u;
  the_mesh.fillRepeaterPage(page_buf[slot]);
  the_mesh.copyAdminPassword(admin_pass[slot], sizeof(admin_pass[slot]));
  page_slot.store(slot, std::memory_order_release);
  syncSession();
}

static bool socketLive() {
  if (!socket) return false;
  for (const AsyncWebSocketClient& client : socket->getClients()) {
    if (client.status() == WS_CONNECTED) return true;
  }
  return false;
}

static void refreshSnapshot() {
  uint32_t now = millis();
  uint32_t gap = 5000;
  if (last_snapshot_ms && (uint32_t)(now - last_snapshot_ms) < gap) return;
  last_snapshot_ms = now;
  publishSnapshot();
}

static bool enqueueCommand(const char* command) {
  if (!command || !command[0]) return false;
  if (submit_gate.test_and_set(std::memory_order_acquire)) return false;
  if (commandPending()) {
    submit_gate.clear(std::memory_order_release);
    return false;
  }
  strncpy(submit_cmd, command, CMD_CAP - 1);
  submit_cmd[CMD_CAP - 1] = 0;
  submit_seq.fetch_add(1, std::memory_order_release);
  submit_gate.clear(std::memory_order_release);
  return true;
}

static bool commandPending() {
  return submit_seq.load(std::memory_order_acquire) != result_seq.load(std::memory_order_acquire);
}

bool RepeaterWeb::commandQueued() const { return commandPending(); }

bool RepeaterWeb::viewerConnected() const { return socketLive(); }

static const char* skipWs(const char* p, const char* end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  return p;
}

static const char* findJsonKey(const char* json, size_t n, const char* key) {
  size_t klen = strlen(key);
  const char* end = json + n;
  for (const char* p = json; p + klen + 2 < end; p++) {
    if (*p != '"') continue;
    if (memcmp(p + 1, key, klen) != 0 || p[1 + klen] != '"') continue;
    const char* v = skipWs(p + 2 + klen, end);
    if (v < end && *v == ':') return skipWs(v + 1, end);
  }
  return nullptr;
}

static bool parseJsonString(const char* p, const char* end, char* dest, size_t cap, size_t* out_len) {
  if (!p || p >= end || *p != '"') return false;
  p++;
  size_t n = 0;
  while (p < end && *p != '"') {
    char c = *p++;
    if (c == '\\') {
      if (p >= end) return false;
      char e = *p++;
      if (e == 'n') c = '\n';
      else if (e == 'r') c = '\r';
      else if (e == 't') c = '\t';
      else if (e == '"' || e == '\\' || e == '/') c = e;
      else if (e == 'u') {
        if (p + 4 > end) return false;
        int code = 0;
        for (int i = 0; i < 4; i++) {
          char h = *p++;
          code <<= 4;
          if (h >= '0' && h <= '9') code += h - '0';
          else if (h >= 'a' && h <= 'f') code += h - 'a' + 10;
          else if (h >= 'A' && h <= 'F') code += h - 'A' + 10;
          else return false;
        }
        if (code > 0x7F) return false;
        c = (char)code;
      } else return false;
    }
    if (n + 1 >= cap) return false;
    dest[n++] = c;
  }
  if (p >= end || *p != '"') return false;
  dest[n] = 0;
  if (out_len) *out_len = n;
  return true;
}

static bool parseJsonPort(const char* p, const char* end, int* out) {
  p = skipWs(p, end);
  if (!p || p >= end || *p < '0' || *p > '9') return false;
  long value = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    value = value * 10 + (*p++ - '0');
    if (value > 65535) return false;
  }
  if (value < 1) return false;
  *out = (int)value;
  return true;
}

static size_t stripSpace(char* text) {
  size_t w = 0;
  for (size_t i = 0; text[i]; i++) {
    unsigned char c = (unsigned char)text[i];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    text[w++] = text[i];
  }
  text[w] = 0;
  return w;
}

static bool queueMqttJob(const char* host, uint16_t port, const char* user, const char* pass, bool set_pass,
                          const char* pem, size_t pem_len) {
  MqttJob* job = (MqttJob*)malloc(sizeof(MqttJob) + pem_len);
  if (!job) {
    setMqttNote("Не хватило памяти");
    return false;
  }
  memset(job, 0, sizeof(MqttJob));
  strncpy(job->host, host ? host : "", sizeof(job->host) - 1);
  strncpy(job->user, user ? user : "", sizeof(job->user) - 1);
  strncpy(job->pass, pass ? pass : "", sizeof(job->pass) - 1);
  job->port = port;
  job->set_pass = set_pass ? 1 : 0;
  job->seq = mqtt_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
  job->pem_len = pem_len;
  if (pem_len && pem) memcpy(job->pem, pem, pem_len);
  job->pem[pem_len] = 0;
  MqttJob* expected = nullptr;
  if (!mqtt_job.compare_exchange_strong(expected, job)) {
    free(job);
    setMqttNote("Предыдущая строка ещё сохраняется");
    return false;
  }
  setMqttNote("Сохраняем настройки…");
  return true;
}

static void acceptMqttFields(const String& configIn) {
  char* config = (char*)malloc(configIn.length() + 1);
  if (!config) {
    setMqttNote("Не хватило памяти");
    return;
  }
  memcpy(config, configIn.c_str(), configIn.length() + 1);
  if (!stripSpace(config)) {
    setMqttNote("Вставьте строку настройки");
    free(config);
    return;
  }

  size_t slen = strlen(config);
  unsigned char* json = (unsigned char*)malloc(slen + 1);
  if (!json) {
    free(config);
    setMqttNote("Не хватило памяти");
    return;
  }
  size_t olen = 0;
  int decoded = mbedtls_base64_decode(json, slen, &olen, (const unsigned char*)config, slen);
  free(config);
  if (decoded != 0 || olen == 0) {
    free(json);
    setMqttNote("Строка настройки не разбирается");
    return;
  }
  const char* doc = (const char*)json;
  const char* end = doc + olen;
  char host[64];
  char user[32];
  char pass[40];
  char* pem = (char*)malloc(4097);
  if (!pem) {
    free(json);
    setMqttNote("Не хватило памяти");
    return;
  }
  size_t pem_len = 0;
  int port = 0;
  int version = 0;
  const char* ver_at = findJsonKey(doc, olen, "v");
  if (!ver_at || !parseJsonPort(ver_at, end, &version) || version != 1) {
    free(json);
    free(pem);
    setMqttNote("Неизвестная версия строки");
    return;
  }
  const char* host_at = findJsonKey(doc, olen, "host");
  const char* user_at = findJsonKey(doc, olen, "user");
  const char* pass_at = findJsonKey(doc, olen, "pass");
  const char* ca_at = findJsonKey(doc, olen, "ca");
  const char* port_at = findJsonKey(doc, olen, "port");
  bool ok = host_at && user_at && pass_at && ca_at && port_at &&
            parseJsonString(host_at, end, host, sizeof(host), nullptr) &&
            parseJsonString(user_at, end, user, sizeof(user), nullptr) &&
            parseJsonString(pass_at, end, pass, sizeof(pass), nullptr) &&
            parseJsonString(ca_at, end, pem, 4097, &pem_len) &&
            parseJsonPort(port_at, end, &port);
  free(json);
  if (!ok) {
    free(pem);
    setMqttNote("В строке нет адреса, логина, пароля или сертификата");
    return;
  }
  queueMqttJob(host, (uint16_t)port, user, pass, true, pem, pem_len);
  free(pem);
}

static void queueMqttOff() {
  uint32_t seq = mqtt_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
  mqtt_off_seq.store(seq, std::memory_order_relaxed);
  mqtt_off.store(1, std::memory_order_release);
  setMqttNote("Отключаем MQTT…");
}

static void serviceMqttJob() {
  MqttJob* job = mqtt_job.exchange(nullptr);
  bool off = mqtt_off.exchange(0, std::memory_order_acq_rel) != 0;
  uint32_t off_seq = mqtt_off_seq.load(std::memory_order_relaxed);
  if (job && off) {
    if (job->seq > off_seq) off = false;
    else {
      free(job);
      job = nullptr;
    }
  }
  if (job) {
    char reply[120];
    reply[0] = 0;
    the_mesh.applyMqttPanel(job->host, job->port, job->user, job->pass, job->set_pass != 0, job->pem, job->pem_len, reply, sizeof(reply));
    setMqttNote(reply[0] ? reply : "Строка не применена");
    free(job);
    last_snapshot_ms = 0;
  }
  if (off) {
    the_mesh.disableMqtt();
    setMqttNote("MQTT выключен");
    last_snapshot_ms = 0;
  }
}

static void copyCommand(char* dest, size_t cap, const String& value) {
  if (cap == 0) return;
  const char* s = value.c_str();
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  size_t n = 0;
  while (*s && *s != '\r' && *s != '\n' && n + 1 < cap) dest[n++] = *s++;
  while (n && (dest[n - 1] == ' ' || dest[n - 1] == '\t')) n--;
  dest[n] = 0;
}

static void serviceTelJob() {
  TelJob* job = tel_job.exchange(nullptr);
  if (!job) return;
  char reply[120];
  reply[0] = 0;
  the_mesh.applyTelPanel(job->enable != 0, job->iata, job->host, job->port, job->user, job->bpass, job->tx != 0, reply,
                         sizeof(reply));
  setTelNote(reply[0] ? reply : "Настройки не применены");
  tel_hold.store(0, std::memory_order_release);
  free(job);
  last_snapshot_ms = 0;
}

static void serviceRadioJob() {
  RepeaterRadioForm* job = radio_job.exchange(nullptr);
  if (!job) return;
  char reply[120];
  reply[0] = 0;
  bool reboot = false;
  bool ok = the_mesh.applyRadioPanel(*job, reply, sizeof(reply), reboot);
  setRadioNote(reply[0] ? reply : "Настройки радио не применены");
  if (ok && reboot) {
    strncpy(web_reboot_cmd, "reboot", CMD_CAP - 1);
    web_reboot_cmd[CMD_CAP - 1] = 0;
    uint32_t when = millis() + 3000;
    if (!when) when = 1;
    web_reboot_at.store(when, std::memory_order_release);
    radio_reboot.store(1, std::memory_order_release);
  }
  free(job);
  publishSnapshot();
  last_snapshot_ms = millis();
  radio_hold.store(0, std::memory_order_release);
}

static void serviceRegionJob() {
  RepeaterRegionForm* job = region_job.exchange(nullptr);
  if (!job) return;
  char reply[120];
  reply[0] = 0;
  bool ok = the_mesh.applyRegionPanel(*job, reply, sizeof(reply));
  setRegionNote(reply[0] ? reply : "Настройки регионов не применены");
  if (ok) region_reload.store(1, std::memory_order_release);
  free(job);
  publishSnapshot();
  last_snapshot_ms = millis();
  last_push_ms = 0;
  region_hold.store(0, std::memory_order_release);
}

static void serviceNodeJob() {
  RepeaterNodeForm* job = node_job.exchange(nullptr);
  if (!job) return;
  char reply[120];
  reply[0] = 0;
  the_mesh.applyNodePanel(*job, reply, sizeof(reply));
  setNodeNote(reply[0] ? reply : "Настройки узла не применены");
  free(job);
  publishSnapshot();
  last_snapshot_ms = millis();
  node_hold.store(0, std::memory_order_release);
}

static void servicePowerJob() {
  uint8_t mode = power_job.exchange(0xFF, std::memory_order_acq_rel);
  if (mode > WIFI_POWER_LIGHT) return;
  NodePrefs* prefs = the_mesh.getNodePrefs();
  bool station = prefs && prefs->wifi_enabled && prefs->wifi_ssid[0];
  if (prefs) prefs->wifi_ps = mode;
  the_mesh.savePrefs();
  uint8_t active = the_mesh.applyWifiPowerMode();
  if (!station) setPowerNote("Сохранено. Включится вместе с Wi‑Fi.");
  else if (mode == WIFI_POWER_LIGHT && active != WIFI_POWER_LIGHT) {
    setPowerNote("Сон процессора недоступен в этой сборке. Включён сон модема.");
  } else if (mode == WIFI_POWER_LIGHT) setPowerNote("Сон модема и процессора включены.");
  else if (mode == WIFI_POWER_MODEM) setPowerNote("Сон модема включён. Процессор остаётся на рабочей частоте.");
  else setPowerNote("Энергосбережение выключено.");
  if (power_job.load(std::memory_order_acquire) == 0xFF) power_hold.store(0xFF, std::memory_order_release);
  last_snapshot_ms = 0;
}

static void serviceCommand() {
  uint32_t submitted = submit_seq.load(std::memory_order_acquire);
  if (submitted == result_seq.load(std::memory_order_relaxed)) return;

  char command[CMD_CAP];
  memcpy(command, submit_cmd, sizeof(command));
  command[CMD_CAP - 1] = 0;
  char original[CMD_CAP];
  memcpy(original, command, sizeof(original));

  char reply[CMD_CAP];
  reply[0] = 0;
  // Reboot from the page used to reset the chip before the browser followed
  // the redirect, so a refresh stayed on /act or /cmd and the page 404'd.
  if (memcmp(command, "reboot", 6) == 0 || memcmp(command, "clkreboot", 9) == 0) {
    memcpy(web_reboot_cmd, command, CMD_CAP);
    web_reboot_cmd[CMD_CAP - 1] = 0;
    uint32_t when = millis() + 2000;
    if (!when) when = 1;
    web_reboot_at.store(when, std::memory_order_release);
    strncpy(reply, "Перезагрузка…", sizeof(reply) - 1);
  } else {
    the_mesh.handleCommand(0, command, reply);
    if (memcmp(original, "clear stats", 11) == 0) {
      publishSnapshot();
      last_snapshot_ms = millis();
      last_push_ms = 0;
    }
  }
  reply[CMD_CAP - 1] = 0;

  uint32_t slot = result_slot.load(std::memory_order_relaxed) ^ 1u;
  memcpy(result_cmd[slot], original, CMD_CAP);
  memcpy(result_reply[slot], reply, CMD_CAP);
  result_slot.store(slot, std::memory_order_relaxed);
  result_seq.store(submitted, std::memory_order_release);
}

static void appendRaw(char*& cursor, size_t& left, const char* text) {
  if (!text || left <= 1) return;
  size_t n = strlen(text);
  if (n >= left) n = left - 1;
  memcpy(cursor, text, n);
  cursor += n;
  left -= n;
  *cursor = 0;
}

static void appendEscaped(char*& cursor, size_t& left, const char* text) {
  if (!text) text = "";
  for (const char* p = text; *p && left > 1; p++) {
    const char* repl = nullptr;
    if (*p == '&') repl = "&amp;";
    else if (*p == '<') repl = "&lt;";
    else if (*p == '>') repl = "&gt;";
    else if (*p == '"') repl = "&quot;";
    if (repl) appendRaw(cursor, left, repl);
    else {
      *cursor++ = *p;
      left--;
      *cursor = 0;
    }
  }
}

static void appendFmt(char*& cursor, size_t& left, const char* fmt, ...) {
  char tmp[96];
  va_list args;
  va_start(args, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, args);
  va_end(args);
  appendRaw(cursor, left, tmp);
}

static void row(char*& cursor, size_t& left, const char* label, const char* value) {
  appendRaw(cursor, left, "<tr><td>");
  appendEscaped(cursor, left, label);
  appendRaw(cursor, left, "</td><td>");
  appendEscaped(cursor, left, value);
  appendRaw(cursor, left, "</td></tr>");
}

static void rowFmt(char*& cursor, size_t& left, const char* label, const char* fmt, ...) {
  char tmp[96];
  va_list args;
  va_start(args, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, args);
  va_end(args);
  row(cursor, left, label, tmp);
}

static void formatUptime(char* dest, size_t cap, uint32_t secs) {
  uint32_t days = secs / 86400;
  uint32_t hours = (secs % 86400) / 3600;
  uint32_t mins = (secs % 3600) / 60;
  uint32_t rem = secs % 60;
  if (days) snprintf(dest, cap, "%u д %u ч %u мин", days, hours, mins);
  else if (hours) snprintf(dest, cap, "%u ч %u мин", hours, mins);
  else snprintf(dest, cap, "%u мин %u с", mins, rem);
}

static void formatErrors(char* dest, size_t cap, uint16_t flags) {
  if (!flags) {
    strncpy(dest, "нет", cap - 1);
    dest[cap - 1] = 0;
    return;
  }
  dest[0] = 0;
  size_t left = cap;
  char* cursor = dest;
  if (flags & ERR_EVENT_FULL) appendRaw(cursor, left, "очередь полна");
  if (flags & ERR_EVENT_CAD_TIMEOUT) {
    if (cursor != dest) appendRaw(cursor, left, ", ");
    appendRaw(cursor, left, "таймаут CAD");
  }
  if (flags & ERR_EVENT_STARTRX_TIMEOUT) {
    if (cursor != dest) appendRaw(cursor, left, ", ");
    appendRaw(cursor, left, "таймаут RX");
  }
  if (!dest[0]) snprintf(dest, cap, "0x%04x", flags);
}

static void formatCountdown(char* dest, size_t cap, uint32_t secs) {
  if (secs == REPEATER_TIMER_OFF) {
    strncpy(dest, "выкл", cap - 1);
    dest[cap - 1] = 0;
    return;
  }
  if (secs == REPEATER_TIMER_UNSET) {
    strncpy(dest, "не задан", cap - 1);
    dest[cap - 1] = 0;
    return;
  }
  if (secs == 0) {
    strncpy(dest, "сейчас", cap - 1);
    dest[cap - 1] = 0;
    return;
  }
  formatUptime(dest, cap, secs);
}

static void actionButton(char*& cursor, size_t& left, const char* act, const char* label, const char* confirm) {
  appendRaw(cursor, left, "<form method=\"post\" action=\"/act\"");
  if (confirm) {
    appendRaw(cursor, left, " onsubmit=\"return confirm('");
    appendEscaped(cursor, left, confirm);
    appendRaw(cursor, left, "')\"");
  }
  appendRaw(cursor, left, "><input type=\"hidden\" name=\"act\" value=\"");
  appendEscaped(cursor, left, act);
  appendRaw(cursor, left, "\"><button type=\"submit\"");
  if (confirm) appendRaw(cursor, left, " class=\"warn\"");
  appendRaw(cursor, left, ">");
  appendEscaped(cursor, left, label);
  appendRaw(cursor, left, "</button></form>");
}

static void renderConsoleOut(char*& cursor, size_t& left, bool busy, bool pending,
                             const char* command, const char* reply) {
  if (busy) appendRaw(cursor, left, "<p class=\"note\">Уже выполняется другая команда.</p>");
  if (pending) {
    appendRaw(cursor, left, "<p class=\"note\">Выполняется");
    if (command && command[0]) {
      appendRaw(cursor, left, ": ");
      appendEscaped(cursor, left, command);
    }
    appendRaw(cursor, left, "</p>");
  } else if (command && command[0]) {
    appendRaw(cursor, left, "<pre>");
    appendEscaped(cursor, left, command);
    appendRaw(cursor, left, "\n");
    appendEscaped(cursor, left, (reply && reply[0]) ? reply : "(нет ответа)");
    appendRaw(cursor, left, "</pre>");
  }
}

static const char* antennaKindName(uint8_t kind) {
  switch (kind) {
    case ADV_ANT_OMNI: return "всенаправленная";
    case ADV_ANT_COLLINEAR: return "коллинеар";
    case ADV_ANT_WHIP: return "штырь";
    case ADV_ANT_DIPOLE: return "диполь";
    case ADV_ANT_YAGI: return "яги";
    case ADV_ANT_PANEL: return "панель";
    case ADV_ANT_MAGNET: return "магнитная";
    case ADV_ANT_MAXON: return "Maxon";
    default: return "не указана";
  }
}

static bool renderLive(const RepeaterPageInfo& info, char*& cursor, size_t& left) {
  appendRaw(cursor, left, "<section><h2>Узел</h2><table>");

  row(cursor, left, "Роль", "репитер");
  row(cursor, left, "Плата", info.board);
  row(cursor, left, "Идентификатор", info.id);
  char uptime[40];
  formatUptime(uptime, sizeof(uptime), info.uptime_secs);
  row(cursor, left, "Время работы", uptime);
  if (info.batt_mv) rowFmt(cursor, left, "Напряжение", "%.2f В", info.batt_mv / 1000.0f);
  else row(cursor, left, "Напряжение", "нет данных");
  if (info.temp_c_x10 != REPEATER_TEMP_UNKNOWN) rowFmt(cursor, left, "Температура", "%.1f °C", info.temp_c_x10 / 10.0f);
  else row(cursor, left, "Температура", "нет данных");
  if (info.lat[0]) {
    row(cursor, left, "Широта", info.lat);
    row(cursor, left, "Долгота", info.lon);
  }
  row(cursor, left, "Антенна", antennaKindName(info.ant_kind));
  if (info.ant_height_dm != ADV_ANT_UNSET) rowFmt(cursor, left, "Высота", "%.1f м", info.ant_height_dm / 10.0);
  else row(cursor, left, "Высота", "не указана");
  if (info.ant_bearing != ADV_ANT_UNSET) rowFmt(cursor, left, "Направление", "%u°", info.ant_bearing);
  else row(cursor, left, "Направление", "не указано");
  appendRaw(cursor, left, "</table></section><section><h2>Часы и объявления</h2><table>");
  row(cursor, left, "Часы", info.time_valid ? info.clock_text : "не выставлены");
  const char* ntp_label = "выкл";
  if (info.ntp_state == NTP_STATE_SYNCED) ntp_label = "синхронизировано";
  else if (info.ntp_state == NTP_STATE_WAIT) ntp_label = "ожидание";
  else if (info.ntp_state == NTP_STATE_NO_WIFI) ntp_label = "нет Wi-Fi";
  row(cursor, left, "NTP", ntp_label);
  row(cursor, left, "NTP-сервер", info.ntp_server[0] ? info.ntp_server : "—");
  char countdown[40];
  formatCountdown(countdown, sizeof(countdown), info.next_clock_secs);
  row(cursor, left, "Следующий опрос времени", countdown);
  if (info.advert_local_mins) rowFmt(cursor, left, "Локальное объявление", "каждые %u мин", info.advert_local_mins);
  else row(cursor, left, "Локальное объявление", "выкл");
  formatCountdown(countdown, sizeof(countdown), info.next_local_secs);
  row(cursor, left, "Следующее локальное", countdown);
  if (info.advert_flood_hours) rowFmt(cursor, left, "Flood-объявление", "каждые %u ч", info.advert_flood_hours);
  else row(cursor, left, "Flood-объявление", "выкл");
  formatCountdown(countdown, sizeof(countdown), info.next_flood_secs);
  row(cursor, left, "Следующее flood", countdown);
  appendRaw(cursor, left, "</table>");
  if (!info.clock_count) {
    appendRaw(cursor, left, "<p class=\"sub\">Источники времени не заданы.</p>");
  } else {
    appendRaw(cursor, left, "<table class=\"grid\"><tr><th>Источник</th><th>Идентификатор</th></tr>");
    for (uint8_t i = 0; i < info.clock_count; i++) {
      appendRaw(cursor, left, "<tr><td>");
      appendEscaped(cursor, left, info.clocks[i].name[0] ? info.clocks[i].name : "—");
      appendRaw(cursor, left, "</td><td>");
      appendEscaped(cursor, left, info.clocks[i].id);
      appendRaw(cursor, left, "</td></tr>");
    }
    appendRaw(cursor, left, "</table>");
  }
  appendRaw(cursor, left, "</section><section><h2>Радио</h2><table>");
  rowFmt(cursor, left, "Частота", "%.3f МГц", info.freq);
  rowFmt(cursor, left, "Полоса", "%.2f кГц", info.bw);
  rowFmt(cursor, left, "SF / CR", "%u / %u", info.sf, info.cr);
  rowFmt(cursor, left, "Мощность TX", "%d дБм", (int)info.tx_dbm);
  row(cursor, left, "Ретрансляция", info.forwarding ? "вкл" : "выкл");
  rowFmt(cursor, left, "Шум", "%d дБм", info.noise_floor);
  rowFmt(cursor, left, "Последний RSSI", "%d дБм", info.last_rssi);
  rowFmt(cursor, left, "Последний SNR", "%.1f дБ", info.last_snr);
  appendRaw(cursor, left, "</table></section><section><h2>Соседи</h2>");
  if (!info.neighbour_count) {
    appendRaw(cursor, left, "<p class=\"sub\">Пока никого не слышно.</p>");
  } else {
    appendRaw(cursor, left, "<table class=\"grid\"><tr><th>Имя</th><th>Идентификатор</th><th>SNR</th><th>Назад</th></tr>");
    for (uint8_t i = 0; i < info.neighbour_count; i++) {
      const RepeaterNeighbour& nb = info.neighbours[i];
      appendRaw(cursor, left, "<tr><td>");
      appendEscaped(cursor, left, nb.name[0] ? nb.name : "—");
      appendRaw(cursor, left, "</td><td>");
      appendEscaped(cursor, left, nb.id);
      appendRaw(cursor, left, "</td><td>");
      char snr[16];
      snprintf(snr, sizeof(snr), "%.1f дБ", nb.snr_q4 / 4.0f);
      appendEscaped(cursor, left, snr);
      appendRaw(cursor, left, "</td><td>");
      char ago[40];
      formatUptime(ago, sizeof(ago), nb.secs_ago);
      appendEscaped(cursor, left, ago);
      appendRaw(cursor, left, "</td></tr>");
    }
    appendRaw(cursor, left, "</table>");
  }
  appendRaw(cursor, left, "</section><section><h2>Статистика</h2><table>");
  rowFmt(cursor, left, "Принято", "%u", info.recv);
  rowFmt(cursor, left, "Отправлено", "%u", info.sent);
  rowFmt(cursor, left, "Flood, принято / отправлено", "%u / %u", info.flood_rx, info.flood_tx);
  rowFmt(cursor, left, "Direct, принято / отправлено", "%u / %u", info.direct_rx, info.direct_tx);
  rowFmt(cursor, left, "Ошибки приёма", "%u", info.recv_errors);
  rowFmt(cursor, left, "Эфир TX", "%u с", info.tx_air_secs);
  rowFmt(cursor, left, "Эфир RX", "%u с", info.rx_air_secs);
  rowFmt(cursor, left, "Очередь", "%u", info.queue_len);
  char errors[80];
  formatErrors(errors, sizeof(errors), info.err_flags);
  row(cursor, left, "События ошибок", errors);
  appendRaw(cursor, left, "</table></section><section><h2>Сеть</h2><table>");
  row(cursor, left, "Wi‑Fi", info.ssid);
  char signal[40];
  if (!info.wifi_up) {
    snprintf(signal, sizeof(signal), "нет связи");
  } else if (info.wifi_rssi < 0) {
    const char* word = "плохой";
    if (info.wifi_rssi >= -50) word = "отличный";
    else if (info.wifi_rssi >= -60) word = "хороший";
    else if (info.wifi_rssi >= -70) word = "средний";
    else if (info.wifi_rssi >= -80) word = "слабый";
    snprintf(signal, sizeof(signal), "%d дБм, %s", (int)info.wifi_rssi, word);
  } else {
    snprintf(signal, sizeof(signal), "есть связь");
  }
  row(cursor, left, "Сигнал", signal);
  const char* power = "без энергосбережения";
  if (info.wifi_ps == WIFI_POWER_LIGHT && info.wifi_ps_active == WIFI_POWER_MODEM) power = "сон модема, процессор не спит";
  else if (info.wifi_ps == WIFI_POWER_MODEM) power = "сон модема";
  else if (info.wifi_ps == WIFI_POWER_LIGHT) power = "сон модема и процессора";
  row(cursor, left, "Питание", power);
  row(cursor, left, "Адрес", info.ip);
  row(cursor, left, "MQTT", info.mqtt);
  row(cursor, left, "Хост", info.mqtt_host[0] ? info.mqtt_host : "—");
  if (info.mqtt_port) rowFmt(cursor, left, "Порт", "%u", info.mqtt_port);
  else row(cursor, left, "Порт", "—");
  row(cursor, left, "IP хоста", info.mqtt_ip[0] ? info.mqtt_ip : "—");
  row(cursor, left, "Доступность", info.mqtt_tcp[0] ? info.mqtt_tcp : "—");
  row(cursor, left, "TLS", info.mqtt_tls ? "вкл" : "выкл");
  row(cursor, left, "Сертификат", info.mqtt_cert ? "есть" : "нет");
  if (info.mqtt_ant_m != 0.0f) rowFmt(cursor, left, "Высота MQTT", "%.1f м", info.mqtt_ant_m);
  else row(cursor, left, "Высота MQTT", "нет");
  row(cursor, left, "MeshCoreTel", info.tel[0] ? info.tel : "выкл");
  if (info.tel_host[0]) rowFmt(cursor, left, "Брокер", "%s:%u", info.tel_host, info.tel_port ? info.tel_port : 1883);
  else row(cursor, left, "Брокер", "—");
  row(cursor, left, "Логин", info.tel_user[0] ? info.tel_user : "—");
  row(cursor, left, "IATA", info.tel_iata[0] ? info.tel_iata : "—");
  appendRaw(cursor, left, "</table>");
  if (mqtt_note[0]) {
    appendRaw(cursor, left, "<p class=\"note\">");
    appendEscaped(cursor, left, mqtt_note);
    appendRaw(cursor, left, "</p>");
  }
  if (info.mqtt_enabled) {
    appendRaw(cursor, left,
              "<form method=\"post\" action=\"/mqtt\" onsubmit=\"return confirm('Отключить MQTT?')\">"
              "<input type=\"hidden\" name=\"mode\" value=\"off\">"
              "<button type=\"submit\" class=\"warn\">Отключить MQTT</button></form>");
  }
  appendRaw(cursor, left, "</section>");
  return left > 1;
}

static bool nearFloat(float a, float b) {
  float d = a - b;
  if (d < 0) d = -d;
  return d < 0.06f;
}

static void textField(char*& cursor, size_t& left, const char* label, const char* name, const char* value,
                      const char* extra, bool wide) {
  appendRaw(cursor, left, wide ? "<label class=\"field span2\">" : "<label class=\"field\">");
  appendEscaped(cursor, left, label);
  appendRaw(cursor, left, "<input name=\"");
  appendRaw(cursor, left, name);
  appendRaw(cursor, left, "\" value=\"");
  appendEscaped(cursor, left, value ? value : "");
  appendRaw(cursor, left, "\"");
  if (extra && extra[0]) {
    appendRaw(cursor, left, " ");
    appendRaw(cursor, left, extra);
  }
  appendRaw(cursor, left, "></label>");
}

static void checkField(char*& cursor, size_t& left, const char* name, const char* label, bool on) {
  appendRaw(cursor, left, "<label class=\"switch span2\"><input type=\"checkbox\" name=\"");
  appendRaw(cursor, left, name);
  appendRaw(cursor, left, "\" value=\"on\"");
  if (on) appendRaw(cursor, left, " checked");
  appendRaw(cursor, left, "><i></i><span>");
  appendEscaped(cursor, left, label);
  appendRaw(cursor, left, "</span></label>");
}

static void selectOpen(char*& cursor, size_t& left, const char* label, const char* name) {
  appendRaw(cursor, left, "<label class=\"field\"><span>");
  appendEscaped(cursor, left, label);
  appendRaw(cursor, left, "</span><select name=\"");
  appendRaw(cursor, left, name);
  appendRaw(cursor, left, "\">");
}

static void selectOption(char*& cursor, size_t& left, const char* value, const char* label, bool on) {
  appendRaw(cursor, left, "<option value=\"");
  appendEscaped(cursor, left, value);
  appendRaw(cursor, left, "\"");
  if (on) appendRaw(cursor, left, " selected");
  appendRaw(cursor, left, ">");
  appendEscaped(cursor, left, label);
  appendRaw(cursor, left, "</option>");
}

static void selectClose(char*& cursor, size_t& left) {
  appendRaw(cursor, left, "</select></label>");
}

static void noteLine(char*& cursor, size_t& left, const char* id, const char* text, bool boot = false) {
  appendRaw(cursor, left, "<p id=\"");
  appendRaw(cursor, left, id);
  appendRaw(cursor, left, "\" class=\"note\"");
  if (boot) appendRaw(cursor, left, " data-boot=\"1\"");
  if (!text || !text[0]) appendRaw(cursor, left, " hidden");
  appendRaw(cursor, left, ">");
  appendEscaped(cursor, left, text ? text : "");
  appendRaw(cursor, left, "</p>");
}

static void radioFromPage(RepeaterRadioForm& form, const RepeaterPageInfo& info) {
  memset(&form, 0, sizeof(form));
  form.freq = info.freq;
  form.bw = info.bw;
  form.airtime = info.airtime_factor;
  form.rx_delay = info.rx_delay;
  form.tx_delay = info.tx_delay;
  form.direct_tx_delay = info.direct_tx_delay;
  form.tx_dbm = info.tx_dbm;
  form.sf = info.sf;
  form.cr = info.cr;
  form.rx_gain = info.rx_gain;
  form.fem_rx = info.fem_rx;
  form.fem_tx = info.fem_tx;
  form.forwarding = info.forwarding;
  form.cad = info.cad;
  form.interference = info.interference;
  form.flood_max = info.flood_max;
  form.flood_max_unscoped = info.flood_max_unscoped;
  form.flood_max_advert = info.flood_max_advert;
  form.path_hash_mode = info.path_hash_bytes ? (uint8_t)(info.path_hash_bytes - 1) : 0;
  if (form.path_hash_mode > 2) form.path_hash_mode = 2;
  form.loop_detect = info.loop_detect;
  form.multi_acks = info.multi_acks ? 1 : 0;
  form.agc_secs = info.agc_secs;
}

static void renderRadioForm(const RepeaterPageInfo& info, char*& cursor, size_t& left) {
  RepeaterRadioForm form;
  if (radio_hold.load(std::memory_order_acquire)) form = radio_hold_form;
  else radioFromPage(form, info);
  char num[24];
  appendRaw(cursor, left,
            "<section id=\"radio\"><h2>Настройки радио</h2>"
            "<p class=\"sub\">«Дальний» — 869.495 МГц, полоса 62.5, SF11, CR 4/5. "
            "«HiNoise» — та же частота, полоса 31.25, SF10, CR 4/7, когда шум на весь эфир. "
            "«Обычный» возвращает параметры, которые были до специального профиля. "
            "Готовый профиль подставляется, только если частоту, полосу, SF и CR не менять. "
            "Иначе сохраняются введённые значения. "
            "Соседи должны стоять на тех же параметрах. На дальнем и HiNoise компаньон включает ретрансляцию. "
            "Если меняются частота, полоса, SF или CR, репитер перезагружается сам.</p>"
            "<form class=\"setup grid\" method=\"post\" action=\"/radio\" autocomplete=\"off\">");
  selectOpen(cursor, left, "Профиль", "profile");
  selectOption(cursor, left, "custom", "Свой", info.radio_profile == RADIO_PROFILE_CUSTOM);
  selectOption(cursor, left, "normal", "Обычный", info.radio_profile == RADIO_PROFILE_NORMAL);
  selectOption(cursor, left, "long", "Дальний", info.radio_profile == RADIO_PROFILE_LONG);
  selectOption(cursor, left, "hinoise", "HiNoise", info.radio_profile == RADIO_PROFILE_HINOISE);
  selectClose(cursor, left);
  snprintf(num, sizeof(num), "%.3f", form.freq);
  textField(cursor, left, "Частота, МГц", "freq", num, "inputmode=\"decimal\" maxlength=\"8\"", false);
  selectOpen(cursor, left, "Полоса, кГц", "bw");
  static const float kBw[] = {7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.67f, 62.5f, 125.0f, 250.0f, 500.0f};
  bool matched = false;
  for (float v : kBw) {
    if (nearFloat(form.bw, v)) matched = true;
  }
  if (!matched) {
    snprintf(num, sizeof(num), "%g", form.bw);
    selectOption(cursor, left, num, num, true);
  }
  for (float v : kBw) {
    snprintf(num, sizeof(num), "%g", v);
    selectOption(cursor, left, num, num, matched && nearFloat(form.bw, v));
  }
  selectClose(cursor, left);
  selectOpen(cursor, left, "SF", "sf");
  for (int s = 5; s <= 12; s++) {
    snprintf(num, sizeof(num), "%d", s);
    selectOption(cursor, left, num, num, form.sf == s);
  }
  selectClose(cursor, left);
  selectOpen(cursor, left, "CR", "cr");
  static const char* crName[] = {"4/5", "4/6", "4/7", "4/8"};
  for (int c = 5; c <= 8; c++) {
    snprintf(num, sizeof(num), "%d", c);
    selectOption(cursor, left, num, crName[c - 5], form.cr == c);
  }
  selectClose(cursor, left);
  snprintf(num, sizeof(num), "%d", (int)form.tx_dbm);
  textField(cursor, left, "Мощность TX, дБм", "tx", num, "inputmode=\"numeric\" maxlength=\"4\"", false);
  snprintf(num, sizeof(num), "%.2f", form.airtime);
  textField(cursor, left, "Коэффициент эфира, 0–9", "af", num, "inputmode=\"decimal\" maxlength=\"6\"", false);
  snprintf(num, sizeof(num), "%.2f", form.rx_delay);
  textField(cursor, left, "Задержка RX, 0–20", "rxd", num, "inputmode=\"decimal\" maxlength=\"6\"", false);
  snprintf(num, sizeof(num), "%.2f", form.tx_delay);
  textField(cursor, left, "Задержка flood, 0–2", "txd", num, "inputmode=\"decimal\" maxlength=\"6\"", false);
  snprintf(num, sizeof(num), "%.2f", form.direct_tx_delay);
  textField(cursor, left, "Задержка direct, 0–2", "dtx", num, "inputmode=\"decimal\" maxlength=\"6\"", false);
  snprintf(num, sizeof(num), "%u", form.interference);
  textField(cursor, left, "Порог помех, 0 — выкл", "ith", num, "inputmode=\"numeric\" maxlength=\"3\"", false);
  snprintf(num, sizeof(num), "%u", form.agc_secs);
  textField(cursor, left, "Сброс AGC, с", "agc", num, "inputmode=\"numeric\" maxlength=\"4\"", false);
  snprintf(num, sizeof(num), "%u", form.flood_max);
  textField(cursor, left, "Максимум flood, 0–64", "fmax", num, "inputmode=\"numeric\" maxlength=\"2\"", false);
  snprintf(num, sizeof(num), "%u", form.flood_max_unscoped);
  textField(cursor, left, "Flood без области, 0–64", "fmaxu", num, "inputmode=\"numeric\" maxlength=\"2\"", false);
  snprintf(num, sizeof(num), "%u", form.flood_max_advert);
  textField(cursor, left, "Flood объявлений, 0–64", "fmaxa", num, "inputmode=\"numeric\" maxlength=\"2\"", false);
  selectOpen(cursor, left, "Path hash", "hash");
  selectOption(cursor, left, "0", "1 байт", form.path_hash_mode == 0);
  selectOption(cursor, left, "1", "2 байта", form.path_hash_mode == 1);
  selectOption(cursor, left, "2", "3 байта", form.path_hash_mode == 2);
  selectClose(cursor, left);
  selectOpen(cursor, left, "Обнаружение петель", "loop");
  selectOption(cursor, left, "0", "выкл", form.loop_detect == 0);
  selectOption(cursor, left, "1", "минимальный", form.loop_detect == 1);
  selectOption(cursor, left, "2", "умеренный", form.loop_detect == 2);
  selectOption(cursor, left, "3", "строгий", form.loop_detect == 3);
  selectClose(cursor, left);
  checkField(cursor, left, "fwd", "Ретрансляция", form.forwarding != 0);
  checkField(cursor, left, "rxgain", "Усиление приёма", form.rx_gain != 0);
  if (info.fem_rx_ok) checkField(cursor, left, "femrx", "Усилитель FEM на приёме", form.fem_rx != 0);
  if (info.fem_tx_ok) checkField(cursor, left, "femtx", "Усилитель FEM на передаче", form.fem_tx != 0);
  checkField(cursor, left, "cad", "CAD перед передачей", form.cad != 0);
  checkField(cursor, left, "acks", "Дополнительный ACK", form.multi_acks != 0);
  appendRaw(cursor, left, "<button class=\"span2\" type=\"submit\">Сохранить радио</button></form>");
  noteLine(cursor, left, "radio-note", radio_note, radio_reboot.load(std::memory_order_acquire) != 0);
  appendRaw(cursor, left, "</section>");
}

static void renderNodeForm(const RepeaterPageInfo& info, char*& cursor, size_t& left) {
  RepeaterNodeForm form;
  if (node_hold.load(std::memory_order_acquire)) form = node_hold_form;
  else {
    memset(&form, 0, sizeof(form));
    strncpy(form.name, info.name, sizeof(form.name) - 1);
    strncpy(form.owner, info.owner, sizeof(form.owner) - 1);
    strncpy(form.lat, info.lat, sizeof(form.lat) - 1);
    strncpy(form.lon, info.lon, sizeof(form.lon) - 1);
    form.ant_kind = info.ant_kind;
    form.ant_height_dm = info.ant_height_dm;
    form.ant_bearing = info.ant_bearing;
    form.advert_mins = info.advert_local_mins;
    form.flood_hours = info.advert_flood_hours;
  }
  char num[16];
  appendRaw(cursor, left,
            "<section id=\"node\"><h2>Узел</h2>"
            "<p class=\"sub\">Имя, координаты и антенна попадают в объявление. Ноль в интервале выключает его. "
            "Направление — азимут: 0° север, по часовой.</p>"
            "<form class=\"setup grid\" method=\"post\" action=\"/node\" autocomplete=\"off\">");
  textField(cursor, left, "Имя", "name", form.name, "maxlength=\"31\"", true);
  appendRaw(cursor, left, "<label class=\"field span2\">Описание<textarea name=\"owner\" maxlength=\"119\" rows=\"3\">");
  appendEscaped(cursor, left, form.owner);
  appendRaw(cursor, left, "</textarea></label>");
  textField(cursor, left, "Широта", "lat", form.lat, "inputmode=\"decimal\" maxlength=\"15\"", false);
  textField(cursor, left, "Долгота", "lon", form.lon, "inputmode=\"decimal\" maxlength=\"15\"", false);
  selectOpen(cursor, left, "Антенна", "ant");
  selectOption(cursor, left, "0", "не указана", form.ant_kind == ADV_ANT_NONE);
  selectOption(cursor, left, "1", "всенаправленная", form.ant_kind == ADV_ANT_OMNI);
  selectOption(cursor, left, "2", "коллинеар", form.ant_kind == ADV_ANT_COLLINEAR);
  selectOption(cursor, left, "3", "штырь", form.ant_kind == ADV_ANT_WHIP);
  selectOption(cursor, left, "4", "диполь", form.ant_kind == ADV_ANT_DIPOLE);
  selectOption(cursor, left, "5", "яги", form.ant_kind == ADV_ANT_YAGI);
  selectOption(cursor, left, "6", "панель", form.ant_kind == ADV_ANT_PANEL);
  selectOption(cursor, left, "7", "магнитная", form.ant_kind == ADV_ANT_MAGNET);
  selectOption(cursor, left, "8", "Maxon", form.ant_kind == ADV_ANT_MAXON);
  selectClose(cursor, left);
  char height[16];
  height[0] = 0;
  if (form.ant_height_dm != ADV_ANT_UNSET) snprintf(height, sizeof(height), "%.1f", form.ant_height_dm / 10.0);
  textField(cursor, left, "Высота, м", "height", height, "inputmode=\"decimal\" maxlength=\"6\" placeholder=\"пусто — не указана\"", false);
  char bearing[8];
  bearing[0] = 0;
  if (form.ant_bearing != ADV_ANT_UNSET) snprintf(bearing, sizeof(bearing), "%u", form.ant_bearing);
  textField(cursor, left, "Направление, °", "brg", bearing, "inputmode=\"numeric\" maxlength=\"3\" placeholder=\"пусто — не указано\"", false);
  snprintf(num, sizeof(num), "%u", form.advert_mins);
  textField(cursor, left, "Локальное объявление, мин", "advert", num, "inputmode=\"numeric\" maxlength=\"3\"", false);
  snprintf(num, sizeof(num), "%u", form.flood_hours);
  textField(cursor, left, "Flood-объявление, ч", "flood", num, "inputmode=\"numeric\" maxlength=\"3\"", false);
  appendRaw(cursor, left, "<button class=\"span2\" type=\"submit\">Сохранить узел</button></form>");
  noteLine(cursor, left, "node-note", node_note);
  appendRaw(cursor, left, "</section>");
}

static void renderPowerForm(const RepeaterPageInfo& info, char*& cursor, size_t& left) {
  uint8_t held = power_hold.load(std::memory_order_acquire);
  uint8_t mode = held <= WIFI_POWER_LIGHT ? held : info.wifi_ps;
  if (mode > WIFI_POWER_LIGHT) mode = WIFI_POWER_NONE;
  appendRaw(cursor, left,
            "<section id=\"power\"><h2>Питание</h2>"
            "<p class=\"sub\">Без энергосбережения радио Wi‑Fi включено постоянно. "
            "Сон модема гасит его между маяками точки доступа и оставляет процессор на рабочей частоте. "
            "Сон модема и процессора дополнительно усыпляет CPU между пакетами, если это позволяет сборка. "
            "Иначе остаётся сон модема. MQTT не отключается. Если брокер начнёт отваливаться, вернитесь к первому режиму.</p>"
            "<form class=\"setup\" method=\"post\" action=\"/power\" autocomplete=\"off\">");
  selectOpen(cursor, left, "Режим", "mode");
  selectOption(cursor, left, "0", "Без энергосбережения", mode == WIFI_POWER_NONE);
  selectOption(cursor, left, "1", "Сон модема Wi‑Fi", mode == WIFI_POWER_MODEM);
  selectOption(cursor, left, "2", "Сон модема и процессора", mode == WIFI_POWER_LIGHT);
  selectClose(cursor, left);
  appendRaw(cursor, left, "<button type=\"submit\">Сохранить питание</button></form>");
  noteLine(cursor, left, "power-note", power_note);
  appendRaw(cursor, left, "</section>");
}

static void appendRegionStatPair(char*& cursor, size_t& left, const char* id_prefix, uint32_t rx, uint32_t fwd) {
  appendRaw(cursor, left, "<div class=\"region-nums\"><span class=\"muted\">принято </span><span id=\"");
  appendRaw(cursor, left, id_prefix);
  appendRaw(cursor, left, "-rx\">");
  appendFmt(cursor, left, "%u", rx);
  appendRaw(cursor, left, "</span><span class=\"muted\"> · переслано </span><span id=\"");
  appendRaw(cursor, left, id_prefix);
  appendRaw(cursor, left, "-fwd\">");
  appendFmt(cursor, left, "%u", fwd);
  appendRaw(cursor, left, "</span></div>");
}

static void renderRegionDrops(const RepeaterPageInfo& info, char*& cursor, size_t& left) {
  appendRaw(cursor, left, "<table class=\"grid\"><tr><th>Отброшено</th><th>Число</th></tr>");
  appendRaw(cursor, left, "<tr><td>Неизвестная метка</td><td id=\"rs-d0\">");
  appendFmt(cursor, left, "%u", info.region_drop_unknown);
  appendRaw(cursor, left, "</td></tr><tr><td>Запрет пересылки у региона</td><td id=\"rs-d1\">");
  appendFmt(cursor, left, "%u", info.region_drop_denied);
  appendRaw(cursor, left, "</td></tr><tr><td>Запрет «без области»</td><td id=\"rs-d2\">");
  appendFmt(cursor, left, "%u", info.region_drop_unscoped);
  appendRaw(cursor, left, "</td></tr><tr><td>Лимит прыжков</td><td id=\"rs-d3\">");
  appendFmt(cursor, left, "%u", info.region_drop_hop);
  appendRaw(cursor, left, "</td></tr><tr><td>Петля</td><td id=\"rs-d4\">");
  appendFmt(cursor, left, "%u", info.region_drop_loop);
  appendRaw(cursor, left, "</td></tr><tr><td>Ретрансляция выключена</td><td id=\"rs-d5\">");
  appendFmt(cursor, left, "%u", info.region_drop_nofwd);
  appendRaw(cursor, left,
            "</td></tr></table><div class=\"actions\" style=\"margin-top:10px\">"
            "<form method=\"post\" action=\"/act\" onsubmit=\"return confirm('Сбросить статистику?')\">"
            "<input type=\"hidden\" name=\"act\" value=\"stats\">"
            "<input type=\"hidden\" name=\"at\" value=\"regions\">"
            "<button type=\"submit\" class=\"warn\">Сбросить статистику</button></form></div>");
}

static void appendRegionStatsJson(const RepeaterPageInfo& info, char*& cursor, size_t& left) {
  appendRaw(cursor, left, "{\"u\":[");
  appendFmt(cursor, left, "%u,%u", info.region_unscoped_rx, info.region_unscoped_fwd);
  appendRaw(cursor, left, "],\"n\":[");
  for (uint8_t i = 0; i < info.region_count; i++) {
    if (i) appendRaw(cursor, left, ",");
    appendRaw(cursor, left, "[");
    appendFmt(cursor, left, "%u,%u", info.regions[i].rx, info.regions[i].fwd);
    appendRaw(cursor, left, "]");
  }
  appendRaw(cursor, left, "],\"d\":[");
  appendFmt(cursor, left, "%u,%u,%u,%u,%u,%u", info.region_drop_unknown, info.region_drop_denied,
            info.region_drop_unscoped, info.region_drop_hop, info.region_drop_loop, info.region_drop_nofwd);
  appendRaw(cursor, left, "]}");
}

static void renderRegionsForm(const RepeaterPageInfo& info, char*& cursor, size_t& left) {
  const RepeaterRegionForm* held =
      region_hold.load(std::memory_order_acquire) ? &region_hold_form : nullptr;
  uint8_t wildcard_flood = info.wildcard_flood;
  const char* region_default = info.region_default;
  const char* region_home = info.region_home;
  if (held) {
    if (held->op == REGION_OP_FLOOD && (!held->name[0] || strcmp(held->name, "*") == 0)) {
      wildcard_flood = held->flood_ok ? 1 : 0;
    }
    if (held->op == REGION_OP_DEFAULT) region_default = held->name;
    if (held->op == REGION_OP_HOME) region_home = held->name;
  }

  appendRaw(cursor, left,
            "<section id=\"regions\"><h2>Регионы</h2>"
            "<p class=\"sub\">Регион — метка на flood-сообщении в эфире. Репитер пересылает только знакомые метки "
            "с включённой пересылкой. Счётчики в памяти, с перезагрузки или сброса, на диск не пишутся. "
            "Даже при включённой пересылке «без области» дальность режет поле «Flood без области» на вкладке «Радио».</p>"
            "<h3>Области</h3>"
            "<div class=\"region-list\">"
            "<div class=\"region-row\">"
            "<div class=\"region-main\"><strong>Без области</strong>"
            "<span class=\"muted\">сообщения без метки</span></div>");
  appendRegionStatPair(cursor, left, "rs-u", info.region_unscoped_rx, info.region_unscoped_fwd);
  appendRaw(cursor, left,
            "<div class=\"region-actions\">"
            "<form method=\"post\" action=\"/region\" autocomplete=\"off\">"
            "<input type=\"hidden\" name=\"op\" value=\"flood\">"
            "<input type=\"hidden\" name=\"name\" value=\"*\">"
            "<label class=\"switch\"><input type=\"checkbox\" name=\"flood\" value=\"on\"");
  if (wildcard_flood) appendRaw(cursor, left, " checked");
  appendRaw(cursor, left,
            " onchange=\"this.form.submit()\"><i></i><span>Пересылать</span></label></form></div></div>");

  if (!info.region_count) {
    appendRaw(cursor, left,
              "<p class=\"sub\">Пока своих регионов нет. Добавьте область ниже, если нужно принимать или отправлять "
              "сообщения с меткой.</p>");
  }
  uint8_t hide_deeper_than = 255;
  for (uint8_t i = 0; i < info.region_count; i++) {
    const RepeaterRegionEntry& r = info.regions[i];
    uint8_t depth = r.depth > 6 ? 6 : r.depth;
    // While a remove is queued, hide that region and its children immediately.
    if (held && held->op == REGION_OP_REMOVE && held->name[0]) {
      if (strcmp(held->name, r.name) == 0) {
        hide_deeper_than = r.depth;
        continue;
      }
      if (hide_deeper_than != 255 && r.depth > hide_deeper_than) continue;
      if (r.depth <= hide_deeper_than) hide_deeper_than = 255;
    }
    appendRaw(cursor, left, "<div class=\"region-row d");
    appendFmt(cursor, left, "%u", (unsigned)depth);
    appendRaw(cursor, left, "\">");
    appendRaw(cursor, left, "<div class=\"region-main\"><strong>");
    appendEscaped(cursor, left, r.name);
    appendRaw(cursor, left, "</strong>");
    bool is_default = r.is_default != 0;
    bool is_home = r.is_home != 0;
    if (held && held->op == REGION_OP_DEFAULT) {
      is_default = held->name[0] && strcmp(held->name, r.name) == 0;
    }
    if (held && held->op == REGION_OP_HOME) {
      is_home = held->name[0] && strcmp(held->name, r.name) == 0;
    }
    if (is_default) appendRaw(cursor, left, "<span class=\"tag\">по умолчанию</span>");
    if (is_home) appendRaw(cursor, left, "<span class=\"tag\">домашний</span>");
    appendRaw(cursor, left, "</div>");
    char id_prefix[12];
    snprintf(id_prefix, sizeof(id_prefix), "rs-%u", (unsigned)i);
    appendRegionStatPair(cursor, left, id_prefix, r.rx, r.fwd);
    appendRaw(cursor, left, "<div class=\"region-actions\">");
    appendRaw(cursor, left,
              "<form method=\"post\" action=\"/region\" autocomplete=\"off\">"
              "<input type=\"hidden\" name=\"op\" value=\"flood\"><input type=\"hidden\" name=\"name\" value=\"");
    appendEscaped(cursor, left, r.name);
    appendRaw(cursor, left, "\"><label class=\"switch\"><input type=\"checkbox\" name=\"flood\" value=\"on\"");
    bool flood_on = r.flood_ok != 0;
    if (held && held->op == REGION_OP_FLOOD && held->name[0] && strcmp(held->name, r.name) == 0) {
      flood_on = held->flood_ok != 0;
    }
    if (flood_on) appendRaw(cursor, left, " checked");
    appendRaw(cursor, left,
              " onchange=\"this.form.submit()\"><i></i><span>Пересылать</span></label></form>"
              "<form method=\"post\" action=\"/region\" "
              "onsubmit=\"return confirm('Удалить регион и все вложенные?')\">"
              "<input type=\"hidden\" name=\"op\" value=\"remove\"><input type=\"hidden\" name=\"name\" value=\"");
    appendEscaped(cursor, left, r.name);
    appendRaw(cursor, left, "\"><button type=\"submit\" class=\"warn\">Удалить</button></form></div></div>");
  }
  appendRaw(cursor, left, "</div><div id=\"region-stats\">");
  renderRegionDrops(info, cursor, left);
  appendRaw(cursor, left, "</div>");

  appendRaw(cursor, left,
            "<h3>Свои сообщения</h3>"
            "<p class=\"sub\">Эту метку репитер ставит на свои объявления и ответы, когда область запроса неизвестна. "
            "Если оставить «Без метки», соседние репитеры с выключенной пересылкой сообщений без области ваши сообщения дальше не пропустят.</p>"
            "<form class=\"setup\" method=\"post\" action=\"/region\" autocomplete=\"off\">"
            "<input type=\"hidden\" name=\"op\" value=\"default\">");
  selectOpen(cursor, left, "Область по умолчанию", "name");
  selectOption(cursor, left, "", "Без метки", !region_default[0]);
  for (uint8_t i = 0; i < info.region_count; i++) {
    selectOption(cursor, left, info.regions[i].name, info.regions[i].name,
                 region_default[0] && strcmp(region_default, info.regions[i].name) == 0);
  }
  selectClose(cursor, left);
  appendRaw(cursor, left, "<button type=\"submit\">Сохранить</button></form>");

  appendRaw(cursor, left,
            "<h3>Добавить регион</h3>"
            "<p class=\"sub\">Имя — короткая метка для эфира, например Moscow или #Europe. "
            "Буквы, цифры, дефис и решётка. Имена с решёткой и без неё считаются одним и тем же. "
            "Не больше 30 символов и не больше 32 регионов. После добавления пересылка для нового региона уже включена — "
            "при необходимости выключите её в списке.</p>"
            "<form class=\"setup grid\" method=\"post\" action=\"/region\" autocomplete=\"off\">"
            "<input type=\"hidden\" name=\"op\" value=\"add\">");
  textField(cursor, left, "Имя", "name", "", "maxlength=\"30\"", false);
  selectOpen(cursor, left, "Родитель", "parent");
  selectOption(cursor, left, "*", "Без области", true);
  for (uint8_t i = 0; i < info.region_count; i++) {
    selectOption(cursor, left, info.regions[i].name, info.regions[i].name, false);
  }
  selectClose(cursor, left);
  appendRaw(cursor, left, "<button class=\"span2\" type=\"submit\">Добавить</button></form>");

  appendRaw(cursor, left,
            "<h3>Домашний регион</h3>"
            "<p class=\"sub\">Просто пометка, к какой области относится этот репитер. "
            "На пересылку чужих сообщений не влияет.</p>"
            "<form class=\"setup\" method=\"post\" action=\"/region\" autocomplete=\"off\">"
            "<input type=\"hidden\" name=\"op\" value=\"home\">");
  selectOpen(cursor, left, "Домашний регион", "name");
  selectOption(cursor, left, "", "Не задан", !region_home[0]);
  for (uint8_t i = 0; i < info.region_count; i++) {
    selectOption(cursor, left, info.regions[i].name, info.regions[i].name,
                 region_home[0] && strcmp(region_home, info.regions[i].name) == 0);
  }
  selectClose(cursor, left);
  appendRaw(cursor, left, "<button type=\"submit\">Сохранить</button></form>");
  noteLine(cursor, left, "region-note", region_note);
  appendRaw(cursor, left, "</section>");
}

static const char* paneForAnchor(const char* id) {
  if (!id || !id[0]) return "status";
  if (!strcmp(id, "stats") || !strcmp(id, "stat")) return "stats";
  if (!strcmp(id, "radio")) return "radio";
  if (!strcmp(id, "node")) return "node";
  if (!strcmp(id, "regions") || !strcmp(id, "region")) return "regions";
  if (!strcmp(id, "tel") || !strcmp(id, "ntp") || !strcmp(id, "net")) return "net";
  if (!strcmp(id, "power") || !strcmp(id, "fw") || !strcmp(id, "sys") || !strcmp(id, "console")) return "sys";
  return "status";
}

static void renderStatsPane(char*& cursor, size_t& left) {
  appendRaw(cursor, left,
            "<section><h2>Статистика</h2>"
            "<p class=\"sub\">Окна по истории с репитера (шаг 5 мин, до 24 ч). После перезагрузки графики пустые, пока не накопятся сэмплы.</p>"
            "<div class=\"filters\" id=\"stats-filters\">"
            "<button type=\"button\" data-h=\"1\">1ч</button>"
            "<button type=\"button\" data-h=\"3\">3ч</button>"
            "<button type=\"button\" data-h=\"6\">6ч</button>"
            "<button type=\"button\" data-h=\"12\">12ч</button>"
            "<button type=\"button\" data-h=\"24\" class=\"on\">24ч</button>"
            "</div>"
            "<p id=\"stats-note\" class=\"note\" hidden></p>"
            "<div class=\"chart-box\"><h3>Пакеты RX / TX</h3><canvas id=\"c-pkt\" width=\"640\" height=\"180\"></canvas>"
            "<div class=\"legend\"><span><i style=\"background:#60a5fa\"></i>RX</span><span><i style=\"background:#86efac\"></i>TX</span></div></div>"
            "<div class=\"chart-box\"><h3>Flood / Direct (RX)</h3><canvas id=\"c-fd\" width=\"640\" height=\"180\"></canvas>"
            "<div class=\"legend\"><span><i style=\"background:#fbbf24\"></i>Flood</span><span><i style=\"background:#c084fc\"></i>Direct</span></div></div>"
            "<div class=\"chart-box\"><h3>Эфир, с за интервал</h3><canvas id=\"c-air\" width=\"640\" height=\"180\"></canvas>"
            "<div class=\"legend\"><span><i style=\"background:#fb7185\"></i>TX</span><span><i style=\"background:#38bdf8\"></i>RX</span></div></div>"
            "<div class=\"chart-box\"><h3>Шум / RSSI / SNR</h3><canvas id=\"c-rf\" width=\"640\" height=\"180\"></canvas>"
            "<div class=\"legend\"><span><i style=\"background:#9aa4ae\"></i>Шум</span><span><i style=\"background:#60a5fa\"></i>RSSI</span><span><i style=\"background:#86efac\"></i>SNR</span></div></div>"
            "<div class=\"chart-box\"><h3>Очередь</h3><canvas id=\"c-q\" width=\"640\" height=\"180\"></canvas>"
            "<div class=\"legend\"><span><i style=\"background:#fbbf24\"></i>Длина</span></div></div>"
            "<div class=\"chart-box\"><h3>Батарея, мВ</h3><canvas id=\"c-batt\" width=\"640\" height=\"180\"></canvas>"
            "<div class=\"legend\"><span><i style=\"background:#86efac\"></i>Напряжение</span></div></div>"
            "<div class=\"chart-box\"><h3>Температура, °C</h3><canvas id=\"c-temp\" width=\"640\" height=\"180\"></canvas>"
            "<div class=\"legend\"><span><i style=\"background:#fb7185\"></i>MCU</span></div></div>"
            "</section>");
}

static void tabButton(char*& cursor, size_t& left, const char* id, const char* label, const char* active) {
  appendRaw(cursor, left, "<button type=\"button\" data-tab=\"");
  appendRaw(cursor, left, id);
  appendRaw(cursor, left, strcmp(id, active) == 0 ? "\" class=\"on\">" : "\">");
  appendEscaped(cursor, left, label);
  appendRaw(cursor, left, "</button>");
}

static void openPane(char*& cursor, size_t& left, const char* id, const char* active) {
  appendRaw(cursor, left, "<div id=\"pane-");
  appendRaw(cursor, left, id);
  appendRaw(cursor, left, strcmp(id, active) == 0 ? "\" class=\"pane on\">" : "\" class=\"pane\">");
}

static void renderPage(const RepeaterPageInfo& info, bool busy, bool pending,
                       const char* command, const char* reply, char* dest, size_t cap,
                       const char* active) {
  char* cursor = dest;
  size_t left = cap;
  if (!active || !active[0]) active = "status";
  if (cap) dest[0] = 0;
  appendRaw(cursor, left,
            "<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, viewport-fit=cover\">"
            "<title>");
  appendEscaped(cursor, left, info.name[0] ? info.name : "репитер");
  appendRaw(cursor, left,
            "</title><style>"
            ":root{color-scheme:dark;--bg:#0d0f12;--panel:#14171b;--panel-2:#191d22;--panel-3:#20252b;--line:#2b3138;--line-strong:#3b434d;--text:#edf0f3;--muted:#9aa4ae;--accent:#60a5fa;--accent-2:#2563eb;--warning:#fbbf24;--danger:#fb7185}"
            "*{box-sizing:border-box}"
            "body{margin:0;color:var(--text);background-color:#0b0d10;background-image:url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='360' height='320' fill='none'%3E%3Cg stroke='%2360a5fa' stroke-opacity='0.14' stroke-width='1'%3E%3Cpath d='M42 58 L168 96 L286 46 M168 96 L92 196 L214 228 M168 96 L214 228 M286 46 L292 168 L214 228 M92 196 L42 58'/%3E%3C/g%3E%3Cg fill='%23edf0f3' fill-opacity='0.22'%3E%3Ccircle cx='42' cy='58' r='2.2'/%3E%3Ccircle cx='168' cy='96' r='2.6'/%3E%3Ccircle cx='286' cy='46' r='2.2'/%3E%3Ccircle cx='92' cy='196' r='2.2'/%3E%3Ccircle cx='214' cy='228' r='2.4'/%3E%3Ccircle cx='292' cy='168' r='2.2'/%3E%3C/g%3E%3C/svg%3E\");background-size:360px 320px;font:14px/1.45 Inter,ui-sans-serif,system-ui,-apple-system,'Segoe UI',sans-serif}"
            "header.bar{position:-webkit-sticky;position:sticky;top:0;z-index:5;padding-top:env(safe-area-inset-top);background:var(--panel);border-bottom:1px solid var(--line);box-shadow:0 8px 24px rgba(0,0,0,.28)}"
            ".brand{max-width:46rem;margin:0 auto;padding:10px 18px 0;display:flex;flex-wrap:wrap;align-items:baseline;justify-content:space-between;gap:2px 16px}"
            ".brand h1{margin:0;font-size:18px;font-weight:650;letter-spacing:-.01em}"
            ".brand .sub{margin:0}"
            "nav.tabs{max-width:46rem;margin:0 auto;display:flex;gap:2px;padding:6px 8px 0;overflow-x:auto;scrollbar-width:none}"
            "nav.tabs::-webkit-scrollbar{height:0}"
            "nav.tabs button{flex:1 0 auto;white-space:nowrap;margin:0;border:0;border-bottom:2px solid transparent;border-radius:0;background:transparent;color:var(--muted);min-height:36px;padding:8px 10px 7px;font-size:13px}"
            "nav.tabs button.on{color:var(--text);border-bottom-color:var(--accent);background:transparent}"
            "main{max-width:46rem;margin:0 auto;padding:14px 18px 24px}"
            ".sub{color:var(--muted);margin:0 0 14px;font-size:12px}"
            ".pane{display:none}.pane.on{display:block}"
            "section{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:14px;margin:0 0 14px;scroll-margin-top:96px}"
            "h2{margin:0 0 11px;font-size:12px;font-weight:650;letter-spacing:.06em;text-transform:uppercase}"
            "table{width:100%;border-collapse:collapse;font-size:13px}"
            "th,td{text-align:left;padding:8px 4px;border-bottom:1px solid var(--line);vertical-align:top}"
            "td:first-child{color:var(--muted);width:46%;padding-right:12px}"
            "th{color:var(--muted);font-size:11px;font-weight:650;letter-spacing:.05em;text-transform:uppercase}"
            "table.grid td:first-child{color:inherit;width:auto}"
            "form{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:6px 0 10px}"
            "form.setup{display:grid;gap:8px}"
            "form.setup.off{display:none}"
            "form.setup button{justify-self:start}"
            "label.field{display:block;margin:0;color:var(--muted);font-size:11px}"
            ".modes{display:flex;gap:8px;margin:0 0 10px}"
            "button{min-height:30px;padding:6px 10px;border:1px solid var(--accent-2);border-radius:5px;background:var(--accent-2);color:var(--text);font-size:12px;font-family:inherit;cursor:pointer}"
            ".modes button,.actions button{border-color:var(--line-strong);background:var(--panel-3)}"
            ".modes button.on{border-color:var(--accent-2);background:var(--accent-2)}"
            "button.warn{border-color:color-mix(in srgb,var(--danger) 35%,var(--line));background:transparent;color:var(--danger)}"
            "input,select,textarea{width:100%;min-width:0;padding:7px 8px;border:1px solid var(--line-strong);border-radius:5px;outline:none;background:var(--panel-2);color:var(--text);font:inherit}"
            "form:not(.setup) input{width:auto;flex:1}"
            "textarea{min-height:8rem;resize:vertical;font:12px/1.4 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}"
            "pre{margin:8px 0;padding:10px 12px;border:1px solid var(--line);border-radius:5px;background:var(--panel-2);color:var(--text);font:12px/1.55 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;white-space:pre-wrap;overflow-wrap:anywhere}"
            ".note{margin:0 0 10px;color:var(--warning);font-size:12px}.note.ok{color:#86efac}.note.bad{color:var(--danger)}"
            ".actions{display:flex;flex-wrap:wrap;gap:8px}"
            ".actions form{margin:0}"
            "progress{width:100%;height:8px;margin:0;border:0;border-radius:99px;overflow:hidden;background:var(--panel-2);accent-color:var(--accent)}"
            "label.switch{position:relative;display:flex;align-items:center;gap:10px;margin:2px 0;color:var(--text);font-size:13px;cursor:pointer}"
            "label.switch input{position:absolute;opacity:0;width:1px;min-width:0;height:1px;padding:0;border:0}"
            "label.switch i{width:42px;height:24px;border-radius:99px;background:var(--panel-3);border:1px solid var(--line-strong);position:relative;flex:none}"
            "label.switch i:before{content:\"\";position:absolute;width:18px;height:18px;border-radius:50%;background:#edf0f3;top:2px;left:2px}"
            "label.switch input:checked+i{background:var(--accent-2);border-color:var(--accent-2)}"
            "label.switch input:checked+i:before{left:20px}"
            "label.switch input:focus-visible+i{outline:2px solid var(--accent)}"
            "form.setup.grid{grid-template-columns:1fr 1fr}"
            "form.setup.grid .span2{grid-column:1/-1}"
            "form.setup label.field select{display:block;width:100%;margin-top:4px}"
            "form.setup.grid label.field input,form.setup.grid label.field select,form.setup.grid label.field textarea{display:block;width:100%;margin-top:4px}"
            "h3{margin:16px 0 8px;font-size:13px;font-weight:650}"
            ".region-list{display:flex;flex-direction:column;gap:6px;margin:0 0 12px}"
            ".region-row{display:flex;flex-wrap:wrap;gap:8px 12px;align-items:center;justify-content:space-between;"
            "padding:10px 10px 10px 12px;border:1px solid var(--line);border-radius:6px;background:var(--panel-2);"
            "border-left:3px solid var(--line-strong)}"
            ".region-row.d1,.region-row.d2,.region-row.d3,.region-row.d4,.region-row.d5,.region-row.d6{"
            "border-left-color:var(--accent);background:var(--panel)}"
            ".region-row.d1{margin-left:1.1rem}.region-row.d2{margin-left:2.2rem}.region-row.d3{margin-left:3.3rem}"
            ".region-row.d4{margin-left:4.4rem}.region-row.d5{margin-left:5.5rem}.region-row.d6{margin-left:6.6rem}"
            ".region-main{display:flex;flex-wrap:wrap;gap:6px 10px;align-items:baseline;flex:1 1 12rem}"
            ".region-main strong{font-size:13px}"
            ".region-nums{font-size:12px;color:var(--text);white-space:nowrap}"
            ".region-actions{display:flex;flex-wrap:wrap;gap:8px;align-items:center}"
            ".region-actions form{margin:0;display:flex;align-items:center;gap:8px}"
            ".tag{display:inline-block;padding:1px 6px;border:1px solid var(--line-strong);border-radius:999px;"
            "color:var(--muted);font-size:11px}"
            ".muted{color:var(--muted);font-size:12px}"
            ".filters{display:flex;flex-wrap:wrap;gap:6px;margin:0 0 12px}"
            ".filters button{border-color:var(--line-strong);background:var(--panel-3)}"
            ".filters button.on{border-color:var(--accent-2);background:var(--accent-2)}"
            ".chart-box{margin:0 0 14px}.chart-box h3{margin:0 0 6px;font-size:13px;font-weight:650}"
            ".chart-box canvas{width:100%;max-width:100%;height:180px;display:block;background:var(--panel-2);border:1px solid var(--line);border-radius:5px}"
            ".legend{display:flex;gap:10px;flex-wrap:wrap;font-size:11px;color:var(--muted);margin:4px 0 0}"
            ".legend i{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:4px;vertical-align:-1px}"
            "@media(max-width:640px){form.setup.grid{grid-template-columns:1fr}}"
            "</style></head><body><header class=\"bar\"><div class=\"brand\"><h1 id=\"title\">");
  appendEscaped(cursor, left, info.name[0] ? info.name : "репитер");
  appendRaw(cursor, left, "</h1><p class=\"sub\" id=\"ver\">");
  appendEscaped(cursor, left, info.firmware);
  appendRaw(cursor, left, " · ");
  appendEscaped(cursor, left, info.build);
  appendRaw(cursor, left, "</p></div><nav class=\"tabs\" id=\"nav\">");
  tabButton(cursor, left, "status", "Статус", active);
  tabButton(cursor, left, "stats", "Статистика", active);
  tabButton(cursor, left, "radio", "Радио", active);
  tabButton(cursor, left, "node", "Узел", active);
  tabButton(cursor, left, "regions", "Регионы", active);
  tabButton(cursor, left, "net", "Сеть", active);
  tabButton(cursor, left, "sys", "Система", active);
  appendRaw(cursor, left, "</nav></header><main>");
  openPane(cursor, left, "status", active);
  appendRaw(cursor, left, "<div id=\"live\">");
  renderLive(info, cursor, left);
  appendRaw(cursor, left, "</div></div>");
  openPane(cursor, left, "stats", active);
  renderStatsPane(cursor, left);
  appendRaw(cursor, left, "</div>");
  openPane(cursor, left, "radio", active);
  renderRadioForm(info, cursor, left);
  appendRaw(cursor, left, "</div>");
  openPane(cursor, left, "node", active);
  renderNodeForm(info, cursor, left);
  appendRaw(cursor, left, "</div>");
  openPane(cursor, left, "regions", active);
  renderRegionsForm(info, cursor, left);
  appendRaw(cursor, left, "</div>");
  openPane(cursor, left, "net", active);
  appendRaw(cursor, left,
            "<section><h2>Подключение MQTT</h2>"
            "<p class=\"sub\">Вставьте строку настройки из панели целиком или заполните поля.</p>"
            "<div class=\"modes\">"
            "<button type=\"button\" id=\"mqtt-tab-b64\" class=\"on\">Строка</button>"
            "<button type=\"button\" id=\"mqtt-tab-manual\">Вручную</button></div>"
            "<form id=\"mqtt-b64\" class=\"setup\" method=\"post\" action=\"/mqtt\" autocomplete=\"off\">"
            "<input type=\"hidden\" name=\"mode\" value=\"b64\">"
            "<textarea name=\"config\" rows=\"5\" placeholder=\"строка настройки\"></textarea>"
            "<button type=\"submit\">Подключить</button></form>"
            "<form id=\"mqtt-manual\" class=\"setup off\" method=\"post\" action=\"/mqtt\" autocomplete=\"off\">"
            "<input type=\"hidden\" name=\"mode\" value=\"manual\">"
            "<label class=\"field\">Хост</label><input name=\"host\" maxlength=\"63\" value=\"");
  appendEscaped(cursor, left, info.mqtt_host);
  appendRaw(cursor, left, "\"><label class=\"field\">Порт</label><input name=\"port\" maxlength=\"5\" inputmode=\"numeric\" value=\"");
  if (info.mqtt_port) appendFmt(cursor, left, "%u", info.mqtt_port);
  else appendRaw(cursor, left, "8883");
  appendRaw(cursor, left, "\"><label class=\"field\">Логин</label><input name=\"user\" maxlength=\"31\" value=\"");
  appendEscaped(cursor, left, info.mqtt_user);
  appendRaw(cursor, left,
            "\"><label class=\"field\">Пароль</label>"
            "<input name=\"pass\" maxlength=\"39\" type=\"password\" autocomplete=\"new-password\" placeholder=\"пусто — оставить текущий\">"
            "<label class=\"field\">Сертификат</label>"
            "<textarea name=\"ca\" rows=\"6\" placeholder=\"PEM. Пусто — оставить текущий\"></textarea>"
            "<button type=\"submit\">Сохранить</button></form>");
  noteLine(cursor, left, "mqtt-note", mqtt_note);
  appendRaw(cursor, left,
            "</section>"
            "<section id=\"tel\"><h2>MeshCoreTel</h2>"
            "<p class=\"sub\">Вместе с MQTT, обычный TCP без TLS. Статус уходит раз в 5 минут, принятые пакеты — сразу.</p>"
            "<form class=\"setup\" method=\"post\" action=\"/tel\" autocomplete=\"off\">"
            "<label class=\"switch\"><input type=\"checkbox\" name=\"en\" value=\"on\"");
  bool tel_held = tel_hold.load(std::memory_order_acquire) != 0;
  bool tel_on = tel_held ? tel_hold_en != 0 : info.tel_enabled != 0;
  bool tel_tx_on = tel_held ? tel_hold_tx != 0 : info.tel_tx != 0;
  const char* tel_iata = tel_held ? tel_hold_iata : info.tel_iata;
  const char* tel_host = tel_held && tel_hold_host[0] ? tel_hold_host : info.tel_host;
  const char* tel_user = tel_held ? tel_hold_user : info.tel_user;
  const char* tel_bpass = tel_held ? tel_hold_bpass : info.tel_pass;
  uint16_t tel_port = tel_held && tel_hold_port ? tel_hold_port : info.tel_port;
  if (tel_on) appendRaw(cursor, left, " checked");
  appendRaw(cursor, left, "><i></i><span>Включено</span></label>"
            "<label class=\"field\">Брокер</label><input name=\"host\" maxlength=\"63\" value=\"");
  appendEscaped(cursor, left, tel_host);
  appendRaw(cursor, left, "\"><label class=\"field\">Порт</label><input name=\"port\" maxlength=\"5\" inputmode=\"numeric\" value=\"");
  appendFmt(cursor, left, "%u", tel_port ? tel_port : 1883);
  appendRaw(cursor, left, "\"><label class=\"field\">Логин</label><input name=\"user\" maxlength=\"31\" value=\"");
  appendEscaped(cursor, left, tel_user);
  appendRaw(cursor, left, "\"><label class=\"field\">Пароль брокера</label><input name=\"bpass\" maxlength=\"39\" type=\"password\" autocomplete=\"new-password\" value=\"");
  appendEscaped(cursor, left, tel_bpass);
  appendRaw(cursor, left, "\"><label class=\"field\">IATA</label><input name=\"iata\" maxlength=\"7\" autocapitalize=\"characters\" value=\"");
  appendEscaped(cursor, left, tel_iata);
  appendRaw(cursor, left, "\"><label class=\"switch\"><input type=\"checkbox\" name=\"tx\" value=\"on\"");
  if (tel_tx_on) appendRaw(cursor, left, " checked");
  appendRaw(cursor, left, "><i></i><span>Отправлять свои передачи</span></label>"
            "<button type=\"submit\">Сохранить</button></form>");
  noteLine(cursor, left, "tel-note", tel_note);
  appendRaw(cursor, left, "</section><section id=\"ntp\"><h2>NTP</h2>"
            "<p class=\"sub\">Нужен Wi-Fi с доступом к серверу по UDP/123. Пока ответ NTP свежий, опрос времени по эфиру не перезаписывает часы.</p>"
            "<form class=\"setup\" method=\"post\" action=\"/ntp\" autocomplete=\"off\">"
            "<select name=\"en\">");
  appendRaw(cursor, left, info.ntp_enabled ? "<option value=\"on\" selected>вкл</option><option value=\"off\">выкл</option>"
                                           : "<option value=\"on\">вкл</option><option value=\"off\" selected>выкл</option>");
  appendRaw(cursor, left, "</select><input name=\"server\" maxlength=\"63\" placeholder=\"pool.ntp.org\" value=\"");
  appendEscaped(cursor, left, info.ntp_server);
  appendRaw(cursor, left,
            "\"><button type=\"submit\">Сохранить</button></form>"
            "<form method=\"post\" action=\"/ntp\"><input type=\"hidden\" name=\"sync\" value=\"1\">"
            "<button type=\"submit\">Синхронизировать</button></form>");
#ifndef DISABLE_WIFI_OTA
  appendRaw(cursor, left, "</section></div>");
  openPane(cursor, left, "sys", active);
  renderPowerForm(info, cursor, left);
  appendRaw(cursor, left,
            "<section id=\"fw\"><h2>Прошивка</h2>"
            "<p class=\"sub\">Файл .bin этой платы, без merged в имени. Пока идёт запись, эфир почти стоит. После успеха репитер перезагрузится, и эта страница обновится сама.</p>"
            "<form id=\"fw-form\" class=\"setup\" method=\"post\" action=\"/fw\" enctype=\"multipart/form-data\">"
            "<input type=\"file\" name=\"firmware\" accept=\".bin,application/octet-stream\">"
            "<progress id=\"fw-bar\" max=\"100\" value=\"0\" hidden></progress>"
            "<p id=\"fw-note\" class=\"note\" hidden></p>"
            "<button type=\"submit\" class=\"warn\">Загрузить</button></form>");
  appendRaw(cursor, left, "</section><section><h2>Действия</h2><div class=\"actions\">");
#else
  appendRaw(cursor, left, "</section></div>");
  openPane(cursor, left, "sys", active);
  renderPowerForm(info, cursor, left);
  appendRaw(cursor, left, "<section><h2>Действия</h2><div class=\"actions\">");
#endif
  actionButton(cursor, left, "advert", "Отправить объявление", nullptr);
  actionButton(cursor, left, "discover", "Запросить соседей", nullptr);
  actionButton(cursor, left, "clock", "Подтянуть время", nullptr);
  actionButton(cursor, left, "stats", "Сбросить статистику", "Сбросить статистику?");
  actionButton(cursor, left, "reboot", "Перезагрузить", "Перезагрузить репитер?");
  appendRaw(cursor, left, "</div></section><section id=\"console\"><h2>Консоль</h2>");
  appendRaw(cursor, left,
            "<form method=\"post\" action=\"/cmd\" autocomplete=\"off\">"
            "<input name=\"cmd\" maxlength=\"159\" placeholder=\"команда\">"
            "<button type=\"submit\">Выполнить</button></form><div id=\"out\">");
  renderConsoleOut(cursor, left, busy, pending, command, reply);
  appendRaw(cursor, left,
            "</div></section><p class=\"sub\">Вход: пользователь admin и пароль администратора. "
            "Те же команды, что и в последовательной консоли. Данные приходят по WebSocket, страница не перезагружается.</p></div>"
            "<script>(function(){var live=document.getElementById('live'),out=document.getElementById('out'),title=document.getElementById('title');"
            "function setNote(id,text){var el=document.getElementById(id);if(!el)return;el.hidden=!text;el.textContent=text||'';}"
            "var booting=false;"
            "function watchBoot(){if(booting)return;booting=true;setNote('radio-note','Настройки радио сохранены. Репитер перезагружается…');"
            "var started=Date.now(),downSince=0,before=null;"
            "function finish(){setNote('radio-note','Репитер снова в сети. Обновляю страницу…');setTimeout(function(){location.replace('/?at=radio');},600);}"
            "function back(up){var outage=downSince?Date.now()-downSince:0;if(!isFinite(up)||outage<4000)return false;if(before!==null&&up+5<before)return true;return up<180&&(before===null||outage>=15000);}"
            "function probe(){var elapsed=Date.now()-started;"
            "if(elapsed>90000){setNote('radio-note','Репитер не подтвердил перезагрузку. Обновите страницу.');return;}"
            "var req=new XMLHttpRequest();var settled=false;req.open('GET','/up?t='+Date.now());req.timeout=3000;"
            "function fail(){if(settled)return;settled=true;if(!downSince)downSince=Date.now();setNote('radio-note','Репитер перезагружается, жду его в сети…');setTimeout(probe,1000);}"
            "req.onload=function(){if(settled)return;if(req.status!==200){if(!req.status)fail();else{settled=true;setTimeout(probe,700);}return;}settled=true;var up=parseInt(req.responseText,10);if(downSince&&back(up)){finish();return;}if(!downSince)before=up;setTimeout(probe,700);};"
            "req.onerror=req.ontimeout=fail;req.send();}setTimeout(probe,800);}"
            "function apply(m){if(m.t&&title){title.textContent=m.t;document.title=m.t;}if(m.live&&live)live.innerHTML=m.live;if(out)out.innerHTML=m.out||'';"
            "if(m.rb)watchBoot();if(m.rn!==undefined&&!booting)setNote('radio-note',m.rn);if(m.nn!==undefined)setNote('node-note',m.nn);if(m.pn!==undefined)setNote('power-note',m.pn);if(m.mn!==undefined)setNote('mqtt-note',m.mn);if(m.tn!==undefined)setNote('tel-note',m.tn);"
            "if(m.gn!==undefined)setNote('region-note',m.gn);"
            "if(m.rs&&typeof m.rs==='object'){var set=function(id,v){var el=document.getElementById(id);if(el)el.textContent=v;};"
            "if(m.rs.u){set('rs-u-rx',m.rs.u[0]|0);set('rs-u-fwd',m.rs.u[1]|0);}"
            "if(m.rs.n){for(var i=0;i<m.rs.n.length;i++){set('rs-'+i+'-rx',m.rs.n[i][0]|0);set('rs-'+i+'-fwd',m.rs.n[i][1]|0);}}"
            "if(m.rs.d){for(var j=0;j<m.rs.d.length;j++)set('rs-d'+j,m.rs.d[j]|0);}}"
            "if(m.gr){location.replace('/?at=regions&r='+Date.now());return;}}"
            "var b64=document.getElementById('mqtt-b64'),man=document.getElementById('mqtt-manual');"
            "var tb=document.getElementById('mqtt-tab-b64'),tm=document.getElementById('mqtt-tab-manual');"
            "function mqttMode(manual){if(!b64||!man)return;b64.classList.toggle('off',manual);man.classList.toggle('off',!manual);if(tb)tb.classList.toggle('on',!manual);if(tm)tm.classList.toggle('on',manual);}"
            "if(tb)tb.onclick=function(){mqttMode(false);};if(tm)tm.onclick=function(){mqttMode(true);};"
            "var tabSet=false;"
            "var statsH=24,statsTimer=0,statsBusy=false;"
            "function niceNum(range,round){var exp=Math.floor(Math.log(Math.abs(range)||1)/Math.LN10),f=(Math.abs(range)||1)/Math.pow(10,exp),nf;"
            "if(round)nf=f<1.5?1:f<3?2:f<7?5:10;else nf=f<=1?1:f<=2?2:f<=5?5:10;return nf*Math.pow(10,exp);}"
            "function niceScale(mn,mx,ticks){if(!(mx>mn)){mn-=1;mx+=1;}var range=niceNum(mx-mn,false),step=niceNum(range/(ticks-1),true);"
            "return{mn:Math.floor(mn/step)*step,mx:Math.ceil(mx/step)*step,step:step};}"
            "function fmtVal(v){if(!isFinite(v))return'';if(Math.abs(v)>=100||Math.abs(v-Math.round(v))<1e-6)return String(Math.round(v));return(Math.round(v*10)/10).toFixed(1);}"
            "function fmtTime(t0,iv,i,n){if(t0>0){var d=new Date((t0+i*iv)*1000),hh=d.getUTCHours(),mm=d.getUTCMinutes();"
            "return(hh<10?'0':'')+hh+':'+(mm<10?'0':'')+mm;}var ago=(n-1-i)*iv;if(ago<=0)return'сейчас';"
            "if(ago<3600)return'-'+Math.round(ago/60)+'м';var h=ago/3600;return'-'+(h>=10?Math.round(h):(Math.round(h*10)/10))+'ч';}"
            "function chart(cv,series,colors,ymin,ymax,meta){if(!cv)return;var c=cv.getContext('2d'),w=cv.width,h=cv.height,pl=44,pr=10,pt=10,pb=24;"
            "c.clearRect(0,0,w,h);c.fillStyle='#191d22';c.fillRect(0,0,w,h);"
            "if(!series||!series.length||!series[0]||!series[0].length){c.fillStyle='#9aa4ae';c.font='11px sans-serif';c.fillText('нет данных',pl,h/2);return;}"
            "var n=series[0].length,s,i,v,mn=ymin,mx=ymax,smn=Infinity,smx=-Infinity,ok=false;"
            "for(s=0;s<series.length;s++)for(i=0;i<n;i++){v=series[s][i];if(!isFinite(v))continue;ok=true;if(v<smn)smn=v;if(v>smx)smx=v;}"
            "if(!ok){c.fillStyle='#9aa4ae';c.font='11px sans-serif';c.fillText('нет данных',pl,h/2);return;}"
            "if(mn==null)mn=smn;if(mx==null)mx=smx;var sc=niceScale(mn,mx,5);mn=sc.mn;mx=sc.mx;var rng=mx-mn||1,pw=w-pl-pr,ph=h-pt-pb;"
            "var iv=(meta&&meta.iv)||0,t0=(meta&&meta.t0)||0;c.font='10px sans-serif';"
            "for(v=mn;v<=mx+sc.step*0.001;v+=sc.step){var y=pt+ph*(1-(v-mn)/rng);c.strokeStyle='#2b3138';c.beginPath();c.moveTo(pl,y);c.lineTo(w-pr,y);c.stroke();"
            "c.fillStyle='#9aa4ae';c.textAlign='right';c.textBaseline='middle';c.fillText(fmtVal(v),pl-4,y);}"
            "c.strokeStyle='#3b434d';c.beginPath();c.moveTo(pl,pt);c.lineTo(pl,pt+ph);c.lineTo(w-pr,pt+ph);c.stroke();"
            "var xt=Math.min(5,n),xi;c.fillStyle='#9aa4ae';c.textAlign='center';c.textBaseline='top';"
            "for(xi=0;xi<xt;xi++){var idx=xt===1?0:Math.round(xi*(n-1)/(xt-1)),x=pl+pw*(n===1?0.5:idx/(n-1));"
            "c.strokeStyle='#2b3138';c.beginPath();c.moveTo(x,pt+ph);c.lineTo(x,pt+ph+3);c.stroke();c.fillText(fmtTime(t0,iv,idx,n),x,pt+ph+5);}"
            "for(s=0;s<series.length;s++){c.strokeStyle=colors[s]||'#60a5fa';c.lineWidth=1.5;c.beginPath();var pen=false;"
            "for(i=0;i<n;i++){v=series[s][i];if(!isFinite(v)){pen=false;continue;}"
            "var x=pl+pw*(n===1?0.5:i/(n-1)),y=pt+ph*(1-(v-mn)/rng);if(pen)c.lineTo(x,y);else c.moveTo(x,y);pen=true;}c.stroke();}}"
            "function setStatsNote(t,kind){var el=document.getElementById('stats-note');if(!el)return;el.hidden=!t;el.classList.remove('ok','bad');"
            "if(kind)el.classList.add(kind);el.textContent=t||'';}"
            "function drawStats(m){var s=m.s||[],n=s.length,rx=[],tx=[],fr=[],dr=[],atx=[],arx=[],nf=[],rs=[],sn=[],q=[],batt=[],temp=[],i;"
            "var meta={iv:m.iv||0,t0:m.t0||0};"
            "for(i=0;i<n;i++){var r=s[i];rx.push(r[0]|0);tx.push(r[1]|0);fr.push(r[2]|0);dr.push(r[4]|0);"
            "atx.push((r[6]|0)/10);arx.push((r[7]|0)/10);nf.push(r[8]|0);rs.push(r[9]|0);sn.push((r[10]|0)/4);q.push(r[11]|0);"
            "var bv=r[12]|0;batt.push(bv>0?bv:NaN);var tv=r[13]|0;temp.push(tv===-32768?NaN:tv/10);}"
            "chart(document.getElementById('c-pkt'),[rx,tx],['#60a5fa','#86efac'],0,null,meta);"
            "chart(document.getElementById('c-fd'),[fr,dr],['#fbbf24','#c084fc'],0,null,meta);"
            "chart(document.getElementById('c-air'),[atx,arx],['#fb7185','#38bdf8'],0,null,meta);"
            "chart(document.getElementById('c-rf'),[nf,rs,sn],['#9aa4ae','#60a5fa','#86efac'],null,null,meta);"
            "chart(document.getElementById('c-q'),[q],['#fbbf24'],0,null,meta);"
            "chart(document.getElementById('c-batt'),[batt],['#86efac'],null,null,meta);"
            "chart(document.getElementById('c-temp'),[temp],['#fb7185'],null,null,meta);"
            "if(!n)setStatsNote('Пока нет сэмплов. Первый интервал — около 5 минут.','');"
            "else setStatsNote('Точек: '+n+', шаг '+(m.iv||0)+' с'+(m.t0?' · время UTC':' · относительное время'),'ok');}"
            "function loadStats(force){if(statsBusy&&!force)return;var pane=document.getElementById('pane-stats');"
            "if(!pane||!pane.classList.contains('on'))return;statsBusy=true;"
            "fetch('/api/stats?h='+statsH,{credentials:'same-origin'}).then(function(r){if(!r.ok)throw new Error('http');return r.json();})"
            ".then(function(m){drawStats(m);}).catch(function(){setStatsNote('Не удалось загрузить статистику','bad');})"
            ".then(function(){statsBusy=false;});}"
            "function stopStats(){if(statsTimer){clearInterval(statsTimer);statsTimer=0;}}"
            "function armStats(){stopStats();statsTimer=setInterval(function(){loadStats(false);},300000);loadStats(true);}"
            "var sf=document.getElementById('stats-filters');"
            "if(sf)sf.onclick=function(ev){var el=ev.target;while(el&&el!==sf&&!(el.getAttribute&&el.getAttribute('data-h')))el=el.parentNode;"
            "if(!el||el===sf)return;statsH=parseInt(el.getAttribute('data-h'),10)||24;var bt=sf.querySelectorAll('button'),j;"
            "for(j=0;j<bt.length;j++)bt[j].classList.toggle('on',bt[j]===el);loadStats(true);};"
            "function showTab(name){if(!name||!document.getElementById('pane-'+name))return false;"
            "var p=document.querySelectorAll('.pane'),b=document.querySelectorAll('#nav button'),i;"
            "for(i=0;i<p.length;i++)p[i].classList.toggle('on',p[i].id==='pane-'+name);"
            "for(i=0;i<b.length;i++)b[i].classList.toggle('on',b[i].getAttribute('data-tab')===name);"
            "try{sessionStorage.setItem('mc-tab',name);}catch(e){}tabSet=true;"
            "if(name==='stats')armStats();else stopStats();return true;}"
            "function tabOf(el){while(el&&el!==document.body){if(el.classList&&el.classList.contains('pane'))return el.id.slice(5);el=el.parentNode;}return '';}"
            "function reveal(id){if(!id)return false;if(showTab(id))return true;var el=document.getElementById(id),name=el&&tabOf(el);"
            "if(!name||!showTab(name))return false;var pane=document.getElementById('pane-'+name);"
            "if(pane&&el!==pane.firstElementChild&&el.scrollIntoView)el.scrollIntoView();return true;}"
            "var nav=document.getElementById('nav');"
            "if(nav)nav.onclick=function(ev){var el=ev.target;while(el&&el!==nav&&!(el.getAttribute&&el.getAttribute('data-tab')))el=el.parentNode;"
            "if(!el||el===nav)return;var name=el.getAttribute('data-tab');showTab(name);if(history.replaceState)history.replaceState(null,'','#'+name);};"
            "var fwNote=document.getElementById('fw-note');"
            "if(fwNote&&/[?&]fw=1(?:&|$)/.test(location.search)){var ver=document.getElementById('ver');var prev='';"
            "try{prev=sessionStorage.getItem('mc-fw')||'';sessionStorage.removeItem('mc-fw');}catch(e){}"
            "var same=!!(prev&&ver&&prev===ver.textContent);fwNote.hidden=false;fwNote.classList.add(same?'bad':'ok');"
            "fwNote.textContent=same?'Репитер перезагрузился, но версия прошивки не изменилась.':'Прошивка обновлена. Репитер загрузился с новой версией.';"
            "showTab('sys');"
            "if(history.replaceState)history.replaceState(null,'','/#sys');}"
            "var rn0=document.getElementById('radio-note');if(rn0&&rn0.getAttribute('data-boot')==='1')watchBoot();"
            "if(!tabSet){var at=/[?&]at=([A-Za-z0-9_-]+)/.exec(location.search);"
            "if(at){reveal(at[1]);if(history.replaceState)history.replaceState(null,'','/#'+at[1]);}"
            "else if(!reveal((location.hash||'').replace(/^#/,''))){var saved='';"
            "try{saved=sessionStorage.getItem('mc-tab')||'';}catch(e){}if(saved)showTab(saved);}}"
            "var sp0=document.getElementById('pane-stats');if(sp0&&sp0.classList.contains('on'))armStats();"
            "var fw=document.getElementById('fw-form');"
            "if(fw)fw.onsubmit=function(ev){ev.preventDefault();var input=fw.querySelector('input[type=file]');var file=input&&input.files&&input.files[0];"
            "var note=document.getElementById('fw-note'),bar=document.getElementById('fw-bar'),btn=fw.querySelector('button');"
            "function say(t,kind){if(!note)return;note.hidden=false;note.classList.remove('ok','bad');if(kind)note.classList.add(kind);note.textContent=t;}"
            "if(!file){say('Выберите файл .bin','bad');return;}"
            "if(/merged/i.test(file.name)||!/\\.bin$/i.test(file.name)){say('Нужен файл .bin без merged в имени.','bad');return;}"
            "if(!confirm('Загрузить прошивку и перезагрузить репитер?'))return;"
            "var ver=document.getElementById('ver');try{sessionStorage.setItem('mc-fw',ver?ver.textContent:'');}catch(e){}"
            "var fd=new FormData();fd.append('firmware',file,file.name);var xhr=new XMLHttpRequest();var uploaded=false,waiting=false;"
            "function waitBoot(){if(waiting)return;waiting=true;say('Прошивка записана. Начинается перезагрузка…');"
            "if(bar){bar.hidden=false;bar.max=bar.max||100;bar.value=bar.max;}"
            "var started=Date.now(),downSince=0,before=null;"
            "function finish(){say('Репитер снова в сети. Обновляю страницу…','ok');setTimeout(function(){location.replace('/?fw=1');},600);}"
            "function back(up){var outage=downSince?Date.now()-downSince:0;if(!isFinite(up)||outage<4000)return false;if(before!==null&&up+5<before)return true;return up<180&&(before===null||outage>=15000);}"
            "function probe(){var elapsed=Date.now()-started;"
            "if(elapsed>120000){say('Репитер не подтвердил перезагрузку. Обновите страницу и проверьте версию прошивки.','bad');if(btn)btn.disabled=false;return;}"
            "if(!downSince&&before!==null&&elapsed>20000){say('Прошивка записана, но репитер не ушёл в перезагрузку. Обновите страницу.','bad');if(btn)btn.disabled=false;return;}"
            "var req=new XMLHttpRequest();var settled=false;req.open('GET','/fw?t='+Date.now());req.timeout=3000;"
            "function fail(){if(settled)return;settled=true;if(!downSince)downSince=Date.now();say('Репитер перезагружается, жду его в сети…');setTimeout(probe,1000);}"
            "req.onload=function(){if(settled)return;if(req.status!==200){if(!req.status)fail();else{settled=true;setTimeout(probe,700);}return;}settled=true;var up=parseInt(req.responseText,10);if(downSince&&back(up)){finish();return;}if(!downSince)before=up;setTimeout(probe,700);};"
            "req.onerror=req.ontimeout=fail;req.send();}"
            "setTimeout(probe,800);}"
            "xhr.open('POST','/fw');"
            "xhr.upload.onprogress=function(e){if(!e.lengthComputable||!bar)return;bar.hidden=false;bar.max=e.total;bar.value=e.loaded;say('Загрузка '+Math.round(e.loaded*100/e.total)+'%…');};"
            "xhr.upload.onload=function(){uploaded=true;say('Файл передан, идёт запись…');};"
            "xhr.onload=function(){if(xhr.status===200){waitBoot();return;}if(btn)btn.disabled=false;say(xhr.responseText||'Ошибка обновления','bad');};"
            "xhr.onerror=function(){if(uploaded){waitBoot();return;}if(btn)btn.disabled=false;say('Загрузка прервалась. Прошивка не изменена.','bad');};"
            "if(btn)btn.disabled=true;say('Загрузка…');xhr.send(fd);};"
            "function connect(){var ws=new WebSocket((location.protocol=='https:'?'wss://':'ws://')+location.host+'/ws');"
            "ws.onmessage=function(ev){try{apply(JSON.parse(ev.data));}catch(e){}};"
            "ws.onclose=function(){setTimeout(connect,2000);};}connect();})();</script>"
            "</main></body></html>");
}

static void appendJsonEscaped(char*& cursor, size_t& left, const char* text, size_t n) {
  appendRaw(cursor, left, "\"");
  for (size_t i = 0; i < n && left > 6; i++) {
    unsigned char c = (unsigned char)text[i];
    if (c == '"' || c == '\\') {
      if (left < 3) break;
      *cursor++ = '\\';
      *cursor++ = (char)c;
      left -= 2;
      *cursor = 0;
    } else if (c == '\n') {
      appendRaw(cursor, left, "\\n");
    } else if (c == '\r') {
      appendRaw(cursor, left, "\\r");
    } else if (c < 0x20) {
      char hex[8];
      snprintf(hex, sizeof(hex), "\\u%04x", c);
      appendRaw(cursor, left, hex);
    } else {
      *cursor++ = (char)c;
      left--;
      *cursor = 0;
    }
  }
  appendRaw(cursor, left, "\"");
}

static void releaseLiveBuffers() {
  free(live_html);
  free(live_json);
  live_html = nullptr;
  live_json = nullptr;
}

static bool holdLiveBuffers() {
  if (!live_html) live_html = (char*)malloc(LIVE_HTML_BYTES);
  if (!live_json) live_json = (char*)malloc(LIVE_JSON_BYTES);
  if (live_html && live_json) return true;
  releaseLiveBuffers();
  return false;
}

static bool socketBusy() {
  if (!socket) return true;
  for (const AsyncWebSocketClient& client : socket->getClients()) {
    if (client.status() == WS_CONNECTED && client.queueLen() > 0) return true;
  }
  return false;
}

static void commandView(bool& pending, bool& busy, char* command, char* reply) {
  pending = commandPending();
  busy = false;
  command[0] = 0;
  reply[0] = 0;
  if (pending) {
    memcpy(command, submit_cmd, CMD_CAP);
    command[CMD_CAP - 1] = 0;
    return;
  }
  uint32_t slot = result_slot.load(std::memory_order_acquire);
  memcpy(command, result_cmd[slot], CMD_CAP);
  memcpy(reply, result_reply[slot], CMD_CAP);
  command[CMD_CAP - 1] = 0;
  reply[CMD_CAP - 1] = 0;
}

static void pushLive(bool force) {
  uint32_t now = millis();
  uint32_t submitted = submit_seq.load(std::memory_order_acquire);
  uint32_t finished = result_seq.load(std::memory_order_acquire);
  bool changed = submitted != pushed_submit || finished != pushed_result;
  if (!force && !changed && last_push_ms && (uint32_t)(now - last_push_ms) < 5000) return;
  if (!socket) return;
  socket->cleanupClients();
  if (!socket->count()) {
    releaseLiveBuffers();
    last_push_ms = now;
    pushed_submit = submitted;
    pushed_result = finished;
    return;
  }
  // One queued frame is enough. A full socket queue used to copy the page until the heap died.
  if (socketBusy()) {
    last_push_ms = now;
    return;
  }
  if (!holdLiveBuffers()) {
    last_push_ms = now;
    return;
  }

  bool pending = false;
  bool busy = false;
  char command[CMD_CAP];
  char reply[CMD_CAP];
  commandView(pending, busy, command, reply);
  uint32_t slot = page_slot.load(std::memory_order_acquire);
  char* cursor = live_html;
  size_t left = LIVE_HTML_BYTES;
  live_html[0] = 0;
  if (!renderLive(page_buf[slot], cursor, left)) {
    last_push_ms = now;
    return;
  }

  static char console[1536];
  char* cout = console;
  size_t cleft = sizeof(console);
  console[0] = 0;
  renderConsoleOut(cout, cleft, busy, pending, command, reply);

  cursor = live_json;
  left = LIVE_JSON_BYTES;
  live_json[0] = 0;
  appendRaw(cursor, left, "{\"t\":");
  const char* title = page_buf[slot].name[0] ? page_buf[slot].name : "репитер";
  appendJsonEscaped(cursor, left, title, strlen(title));
  appendRaw(cursor, left, ",\"live\":");
  appendJsonEscaped(cursor, left, live_html, strlen(live_html));
  appendRaw(cursor, left, ",\"out\":");
  appendJsonEscaped(cursor, left, console, strlen(console));
  appendRaw(cursor, left, ",\"rs\":");
  appendRegionStatsJson(page_buf[slot], cursor, left);
  appendRaw(cursor, left, ",\"rn\":");
  appendJsonEscaped(cursor, left, radio_note, strlen(radio_note));
  appendRaw(cursor, left, ",\"nn\":");
  appendJsonEscaped(cursor, left, node_note, strlen(node_note));
  appendRaw(cursor, left, ",\"pn\":");
  appendJsonEscaped(cursor, left, power_note, strlen(power_note));
  appendRaw(cursor, left, ",\"mn\":");
  appendJsonEscaped(cursor, left, mqtt_note, strlen(mqtt_note));
  appendRaw(cursor, left, ",\"tn\":");
  appendJsonEscaped(cursor, left, tel_note, strlen(tel_note));
  appendRaw(cursor, left, ",\"gn\":");
  appendJsonEscaped(cursor, left, region_note, strlen(region_note));
  uint8_t reload_regions = region_reload.load(std::memory_order_acquire);
  appendRaw(cursor, left, reload_regions ? ",\"gr\":1" : ",\"gr\":0");
  appendRaw(cursor, left, radio_reboot.load(std::memory_order_acquire) ? ",\"rb\":1}" : ",\"rb\":0}");
  if (left > 64) {
    socket->textAll(live_json);
    if (reload_regions) region_reload.store(0, std::memory_order_release);
    last_push_ms = now;
    pushed_submit = submitted;
    pushed_result = finished;
  }
}

static bool cookieOk(AsyncWebServerRequest* request) {
  if (!session_key || !request->hasHeader("Cookie")) return false;
  char expect[20];
  snprintf(expect, sizeof(expect), "mc=%08x", session_key);
  return request->getHeader("Cookie")->value().indexOf(expect) >= 0;
}

static bool allowed(AsyncWebServerRequest* request) {
  uint32_t slot = page_slot.load(std::memory_order_acquire);
  if (request->authenticate("admin", admin_pass[slot])) return true;
  request->requestAuthentication(AsyncAuthType::AUTH_BASIC, "repeater",
                                 "Нужен вход admin и пароль администратора репитера.");
  return false;
}

static const char* actionCommand(const String& name) {
  if (name == "advert") return "advert";
  if (name == "discover") return "discover.neighbors";
  if (name == "clock") return "clock pull";
  if (name == "stats") return "clear stats";
  if (name == "reboot") return "reboot";
  return nullptr;
}

static std::atomic_flag stats_gate = ATOMIC_FLAG_INIT;
static char stats_json[STATS_JSON_BYTES];
static RepeaterStatsSlot stats_slots[REPEATER_STATS_EXPORT_MAX];

static void sendStats(AsyncWebServerRequest* request) {
  // Cookie from the page load, or browser-cached Basic auth.
  if (!cookieOk(request)) {
    if (!allowed(request)) return;
  }
  if (stats_gate.test_and_set(std::memory_order_acquire)) {
    request->send(503, "text/plain; charset=utf-8", "busy");
    return;
  }
  // Need room for the String copy inside beginResponse / TCP.
  if (ESP.getMaxAllocHeap() < STATS_JSON_BYTES + 4096) {
    releaseLiveBuffers();
  }
  if (ESP.getMaxAllocHeap() < STATS_JSON_BYTES + 4096) {
    stats_gate.clear(std::memory_order_release);
    request->send(503, "text/plain; charset=utf-8", "no memory");
    return;
  }
  uint8_t hours = 24;
  if (request->hasParam("h")) {
    int h = request->getParam("h")->value().toInt();
    if (h == 1 || h == 3 || h == 6 || h == 12 || h == 24) hours = (uint8_t)h;
  }
  RepeaterStatsSeriesMeta meta;
  uint16_t n = the_mesh.exportStatsHistory(hours, stats_slots, REPEATER_STATS_EXPORT_MAX, meta);
  char* cursor = stats_json;
  size_t left = STATS_JSON_BYTES;
  stats_json[0] = 0;
  appendFmt(cursor, left, "{\"h\":%u,\"iv\":%lu,\"n\":%u,\"t0\":%lu,\"s\":[",
            (unsigned)meta.hours, (unsigned long)meta.interval_secs, (unsigned)n, (unsigned long)meta.t0);
  for (uint16_t i = 0; i < n && left > 64; i++) {
    const RepeaterStatsSlot& s = stats_slots[i];
    if (i) appendRaw(cursor, left, ",");
    appendFmt(cursor, left, "[%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%d,%u,%u,%d]",
              (unsigned)s.recv_d, (unsigned)s.sent_d, (unsigned)s.flood_rx_d, (unsigned)s.flood_tx_d,
              (unsigned)s.direct_rx_d, (unsigned)s.direct_tx_d, (unsigned)s.tx_air_ds, (unsigned)s.rx_air_ds,
              (int)s.noise, (int)s.rssi, (int)s.snr_q4, (unsigned)s.queue,
              (unsigned)s.batt_mv, (int)s.temp_c_x10);
  }
  appendRaw(cursor, left, "]}");
  // Copy into the response so the static buffer can be reused immediately.
  AsyncWebServerResponse* response =
      request->beginResponse(200, "application/json; charset=utf-8", (const char*)stats_json);
  stats_gate.clear(std::memory_order_release);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

static void sendPage(AsyncWebServerRequest* request) {
  if (!allowed(request)) return;
  if (ESP.getMaxAllocHeap() < PAGE_BYTES + PAGE_HEAP_RESERVE) {
    // Drop WS live buffers so a refresh can recover without reboot.
    releaseLiveBuffers();
  }
  char* page = (char*)malloc(PAGE_BYTES);
  if (!page) {
    releaseLiveBuffers();
    page = (char*)malloc(PAGE_BYTES);
  }
  if (!page) {
    request->send(503, "text/plain", "no memory");
    return;
  }
  bool pending = commandPending();
  bool busy = request->hasParam("busy");
  char command[CMD_CAP];
  char reply[CMD_CAP];
  command[0] = 0;
  reply[0] = 0;
  if (pending) {
    memcpy(command, submit_cmd, sizeof(command));
    command[CMD_CAP - 1] = 0;
  } else {
    uint32_t slot = result_slot.load(std::memory_order_acquire);
    memcpy(command, result_cmd[slot], sizeof(command));
    memcpy(reply, result_reply[slot], sizeof(reply));
    command[CMD_CAP - 1] = 0;
    reply[CMD_CAP - 1] = 0;
  }
  uint32_t slot = page_slot.load(std::memory_order_acquire);
  const char* anchor = "status";
  if (request->hasParam("fw")) anchor = "fw";
  else if (request->hasParam("at")) anchor = request->getParam("at")->value().c_str();
  renderPage(page_buf[slot], busy, pending, command, reply, page, PAGE_BYTES, paneForAnchor(anchor));
  AsyncWebServerResponse* response = request->beginResponse(200, "text/html; charset=utf-8", page);
  free(page);
  response->addHeader("Cache-Control", "no-store");
  char cookie[56];
  snprintf(cookie, sizeof(cookie), "mc=%08x; Path=/; HttpOnly; SameSite=Strict", session_key);
  response->addHeader("Set-Cookie", cookie);
  request->send(response);
}

static void copyPem(char*& dest, size_t& len, const String& value) {
  dest = nullptr;
  len = 0;
  const char* s = value.c_str();
  size_t n = value.length();
  while (n && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r')) { s++; n--; }
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
  if (!n) return;
  dest = (char*)malloc(n + 1);
  if (!dest) return;
  size_t w = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\r') continue;
    dest[w++] = s[i];
  }
  dest[w] = 0;
  len = w;
}

static void acceptMqttManual(AsyncWebServerRequest* request) {
  char host[64];
  char user[32];
  char pass[40];
  char port_text[8];
  host[0] = user[0] = pass[0] = port_text[0] = 0;
  if (request->hasParam("host", true)) copyCommand(host, sizeof(host), request->getParam("host", true)->value());
  if (request->hasParam("user", true)) copyCommand(user, sizeof(user), request->getParam("user", true)->value());
  if (request->hasParam("pass", true)) copyCommand(pass, sizeof(pass), request->getParam("pass", true)->value());
  if (request->hasParam("port", true)) copyCommand(port_text, sizeof(port_text), request->getParam("port", true)->value());
  if (!port_text[0]) {
    setMqttNote("Укажите порт");
    return;
  }
  long port = 0;
  for (const char* p = port_text; *p; p++) {
    if (*p < '0' || *p > '9') {
      setMqttNote("Порт должен быть от 1 до 65535");
      return;
    }
    port = port * 10 + (*p - '0');
    if (port > 65535) {
      setMqttNote("Порт должен быть от 1 до 65535");
      return;
    }
  }
  if (port < 1) {
    setMqttNote("Порт должен быть от 1 до 65535");
    return;
  }
  char* pem = nullptr;
  size_t pem_len = 0;
  if (request->hasParam("ca", true)) {
    const String& ca = request->getParam("ca", true)->value();
    if (ca.length() > 4096) {
      setMqttNote("Сертификат слишком длинный");
      return;
    }
    copyPem(pem, pem_len, ca);
    if (ca.length() && !pem && pem_len == 0) {
      const char* s = ca.c_str();
      bool blank = true;
      for (size_t i = 0; i < ca.length(); i++) {
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') blank = false;
      }
      if (!blank) {
        setMqttNote("Не хватило памяти");
        return;
      }
    }
  }
  queueMqttJob(host, (uint16_t)port, user, pass, pass[0] != 0, pem, pem_len);
  free(pem);
}

static void onMqtt(AsyncWebServerRequest* request) {
  if (!allowed(request)) return;
  if (request->hasParam("mode", true) && request->getParam("mode", true)->value() == "off") {
    queueMqttOff();
    request->redirect("/?at=net");
    return;
  }
  if (request->hasParam("mode", true) && request->getParam("mode", true)->value() == "manual") {
    acceptMqttManual(request);
    request->redirect("/?at=net");
    return;
  }
  if (!request->hasParam("config", true)) {
    setMqttNote("Вставьте строку настройки");
    request->redirect("/?at=net");
    return;
  }
  const String& config = request->getParam("config", true)->value();
  if (config.length() > 12000) {
    setMqttNote("Строка слишком длинная");
    request->redirect("/?at=net");
    return;
  }
  acceptMqttFields(config);
  request->redirect("/?at=net");
}

static void commaDot(char* text) {
  for (; text && *text; text++) {
    if (*text == ',') *text = '.';
  }
}

static void readText(AsyncWebServerRequest* request, const char* name, char* dest, size_t cap) {
  dest[0] = 0;
  if (request->hasParam(name, true)) copyCommand(dest, cap, request->getParam(name, true)->value());
}

static bool parseFloatText(char* text, float& out) {
  commaDot(text);
  if (!text[0]) return false;
  char* end = nullptr;
  out = strtof(text, &end);
  return end && end != text && *end == 0;
}

static bool readFloat(AsyncWebServerRequest* request, const char* name, float& out) {
  char buf[24];
  readText(request, name, buf, sizeof(buf));
  return parseFloatText(buf, out);
}

static bool readLong(AsyncWebServerRequest* request, const char* name, long& out) {
  char buf[16];
  readText(request, name, buf, sizeof(buf));
  if (!buf[0]) return false;
  char* end = nullptr;
  out = strtol(buf, &end, 10);
  return end && end != buf && *end == 0;
}

static bool webNameOk(const char* name) {
  if (!name || strlen(name) >= 32) return false;
  for (const char* p = name; *p; p++) {
    if (*p == '[' || *p == ']' || *p == '\\' || *p == ':' || *p == ',' || *p == '?' || *p == '*') return false;
  }
  return true;
}

static bool copyOwner(char* dest, size_t cap, const String& value) {
  if (!cap) return false;
  const char* s = value.c_str();
  size_t n = 0;
  bool started = false;
  for (size_t i = 0; s[i]; i++) {
    char c = s[i];
    if (c == '\r') continue;
    if (!started && (c == ' ' || c == '\t' || c == '\n')) continue;
    started = true;
    if (n + 1 >= cap) return false;
    dest[n++] = c;
  }
  while (n && (dest[n - 1] == ' ' || dest[n - 1] == '\t' || dest[n - 1] == '\n')) n--;
  dest[n] = 0;
  return true;
}

static int queueRadio(const RepeaterRadioForm& form) {
  if (radio_job.load(std::memory_order_acquire)) return 1;
  RepeaterRadioForm* job = (RepeaterRadioForm*)malloc(sizeof(RepeaterRadioForm));
  if (!job) return 2;
  *job = form;
  radio_hold_form = form;
  radio_hold.store(1, std::memory_order_release);
  RepeaterRadioForm* expected = nullptr;
  if (!radio_job.compare_exchange_strong(expected, job)) {
    free(job);
    return 1;
  }
  return 0;
}

static int queueNode(const RepeaterNodeForm& form) {
  if (node_job.load(std::memory_order_acquire)) return 1;
  RepeaterNodeForm* job = (RepeaterNodeForm*)malloc(sizeof(RepeaterNodeForm));
  if (!job) return 2;
  *job = form;
  node_hold_form = form;
  node_hold.store(1, std::memory_order_release);
  RepeaterNodeForm* expected = nullptr;
  if (!node_job.compare_exchange_strong(expected, job)) {
    free(job);
    return 1;
  }
  return 0;
}

static int queueRegion(const RepeaterRegionForm& form) {
  if (region_job.load(std::memory_order_acquire)) return 1;
  RepeaterRegionForm* job = (RepeaterRegionForm*)malloc(sizeof(RepeaterRegionForm));
  if (!job) return 2;
  *job = form;
  region_hold_form = form;
  region_hold.store(1, std::memory_order_release);
  RepeaterRegionForm* expected = nullptr;
  if (!region_job.compare_exchange_strong(expected, job)) {
    free(job);
    region_hold.store(0, std::memory_order_release);
    return 1;
  }
  return 0;
}

static void handleRegion(AsyncWebServerRequest* request) {
  if (!allowed(request)) return;
  char op[12];
  op[0] = 0;
  readText(request, "op", op, sizeof(op));
  RepeaterRegionForm form;
  memset(&form, 0, sizeof(form));
  readText(request, "name", form.name, sizeof(form.name));
  readText(request, "parent", form.parent, sizeof(form.parent));
  if (strcmp(op, "flood") == 0) {
    form.op = REGION_OP_FLOOD;
    form.flood_ok = request->hasParam("flood", true) ? 1 : 0;
  } else if (strcmp(op, "add") == 0) {
    form.op = REGION_OP_ADD;
  } else if (strcmp(op, "remove") == 0) {
    form.op = REGION_OP_REMOVE;
  } else if (strcmp(op, "default") == 0) {
    form.op = REGION_OP_DEFAULT;
  } else if (strcmp(op, "home") == 0) {
    form.op = REGION_OP_HOME;
  } else {
    setRegionNote("Неизвестная операция");
    request->redirect("/?at=regions");
    return;
  }
  int queued = queueRegion(form);
  if (queued == 2) setRegionNote("Не хватило памяти");
  else if (queued) setRegionNote("Предыдущие настройки ещё сохраняются");
  else setRegionNote("Сохраняем настройки регионов…");
  request->redirect("/?at=regions");
}

static void handleRadio(AsyncWebServerRequest* request) {
  if (!allowed(request)) return;
  float freq = 0, bw = 0, air = 0, rxd = 0, txd = 0, dtx = 0;
  long sf = 0, cr = 0, tx = 0, ith = 0, agc = 0, fmax = 0, fmaxu = 0, fmaxa = 0, hash = 0, loop = 0;
  bool parsed = readFloat(request, "freq", freq) && readFloat(request, "bw", bw) &&
                readLong(request, "sf", sf) && readLong(request, "cr", cr) && readLong(request, "tx", tx) &&
                readFloat(request, "af", air) && readFloat(request, "rxd", rxd) &&
                readFloat(request, "txd", txd) && readFloat(request, "dtx", dtx) &&
                readLong(request, "ith", ith) && readLong(request, "agc", agc) &&
                readLong(request, "fmax", fmax) && readLong(request, "fmaxu", fmaxu) &&
                readLong(request, "fmaxa", fmaxa) && readLong(request, "hash", hash) &&
                readLong(request, "loop", loop);
  if (!parsed || !(freq >= 150.0f && freq <= 2500.0f && bw >= 7.0f && bw <= 500.0f && sf >= 5 && sf <= 12 &&
                   cr >= 5 && cr <= 8 && tx >= -9 && tx <= 30 && air >= 0.0f && air <= 9.0f &&
                   rxd >= 0.0f && rxd <= 20.0f && txd >= 0.0f && txd <= 2.0f && dtx >= 0.0f && dtx <= 2.0f &&
                   ith >= 0 && ith <= 255 && agc >= 0 && agc <= 1020 && fmax >= 0 && fmax <= 64 &&
                   fmaxu >= 0 && fmaxu <= 64 && fmaxa >= 0 && fmaxa <= 64 && hash >= 0 && hash <= 2 &&
                   loop >= 0 && loop <= 3)) {
    setRadioNote("Проверьте диапазоны: частота 150–2500, полоса 7–500, SF 5–12, CR 4/5–4/8, мощность −9…30");
    request->redirect("/?at=radio");
    return;
  }
  RepeaterRadioForm form;
  memset(&form, 0, sizeof(form));
  form.freq = freq;
  form.bw = bw;
  form.sf = (uint8_t)sf;
  form.cr = (uint8_t)cr;
  char profile[8];
  profile[0] = 0;
  readText(request, "profile", profile, sizeof(profile));
  if (strcmp(profile, "long") == 0) form.profile = RADIO_PROFILE_LONG;
  else if (strcmp(profile, "hinoise") == 0) form.profile = RADIO_PROFILE_HINOISE;
  else if (strcmp(profile, "normal") == 0) form.profile = RADIO_PROFILE_NORMAL;
  else form.profile = RADIO_PROFILE_CUSTOM;
  form.tx_dbm = (int8_t)tx;
  form.airtime = air;
  form.rx_delay = rxd;
  form.tx_delay = txd;
  form.direct_tx_delay = dtx;
  form.interference = (uint8_t)ith;
  form.agc_secs = (uint16_t)((agc / 4) * 4);
  form.flood_max = (uint8_t)fmax;
  form.flood_max_unscoped = (uint8_t)fmaxu;
  form.flood_max_advert = (uint8_t)fmaxa;
  form.path_hash_mode = (uint8_t)hash;
  form.loop_detect = (uint8_t)loop;
  form.forwarding = request->hasParam("fwd", true) ? 1 : 0;
  form.rx_gain = request->hasParam("rxgain", true) ? 1 : 0;
  form.cad = request->hasParam("cad", true) ? 1 : 0;
  form.multi_acks = request->hasParam("acks", true) ? 1 : 0;
  uint32_t slot = page_slot.load(std::memory_order_acquire);
  form.fem_rx_set = page_buf[slot].fem_rx_ok;
  form.fem_tx_set = page_buf[slot].fem_tx_ok;
  form.fem_rx = (form.fem_rx_set && request->hasParam("femrx", true)) ? 1 : 0;
  form.fem_tx = (form.fem_tx_set && request->hasParam("femtx", true)) ? 1 : 0;
  int queued = queueRadio(form);
  if (queued == 2) setRadioNote("Не хватило памяти");
  else if (queued) setRadioNote("Предыдущие настройки ещё сохраняются");
  else setRadioNote("Сохраняем настройки радио…");
  request->redirect("/?at=radio");
}

static void handleNode(AsyncWebServerRequest* request) {
  if (!allowed(request)) return;
  RepeaterNodeForm form;
  memset(&form, 0, sizeof(form));
  readText(request, "name", form.name, sizeof(form.name));
  if (!webNameOk(form.name)) {
    setNodeNote("В имени нельзя использовать [ ] \\ : , ? *");
    request->redirect("/?at=node");
    return;
  }
  if (request->hasParam("owner", true) &&
      !copyOwner(form.owner, sizeof(form.owner), request->getParam("owner", true)->value())) {
    setNodeNote("Описание владельца длиннее 119 символов");
    request->redirect("/?at=node");
    return;
  }
  readText(request, "lat", form.lat, sizeof(form.lat));
  readText(request, "lon", form.lon, sizeof(form.lon));
  commaDot(form.lat);
  commaDot(form.lon);
  if (form.lat[0] || form.lon[0]) {
    char* lat_end = nullptr;
    char* lon_end = nullptr;
    double lat = strtod(form.lat, &lat_end);
    double lon = strtod(form.lon, &lon_end);
    if (!form.lat[0] || !lat_end || *lat_end || lat < -90.0 || lat > 90.0 ||
        !form.lon[0] || !lon_end || *lon_end || lon < -180.0 || lon > 180.0) {
      setNodeNote("Широта −90…90, долгота −180…180");
      request->redirect("/?at=node");
      return;
    }
  }
  long mins = 0;
  long hours = 0;
  if (!readLong(request, "advert", mins) || !readLong(request, "flood", hours) ||
      mins < 0 || (mins > 0 && mins < 60) || mins > 240 ||
      hours < 0 || (hours > 0 && hours < 3) || hours > 168) {
    setNodeNote("Локальное объявление: 0 или 60–240 мин. Flood: 0 или 3–168 ч");
    request->redirect("/?at=node");
    return;
  }
  form.advert_mins = (uint16_t)((mins / 2) * 2);
  form.flood_hours = (uint8_t)hours;
  long kind = 0;
  if (!readLong(request, "ant", kind) || kind < ADV_ANT_NONE || kind > ADV_ANT_MAXON) {
    setNodeNote("Выберите антенну из списка");
    request->redirect("/?at=node");
    return;
  }
  form.ant_kind = (uint8_t)kind;
  char height[16];
  readText(request, "height", height, sizeof(height));
  commaDot(height);
  if (!height[0]) {
    form.ant_height_dm = ADV_ANT_UNSET;
  } else {
    char* end = nullptr;
    double meters = strtod(height, &end);
    if (!end || end == height || *end || meters < 0.0 || meters > 409.5) {
      setNodeNote("Высота антенны: 0–409.5 м или пусто");
      request->redirect("/?at=node");
      return;
    }
    form.ant_height_dm = (uint16_t)(meters * 10.0 + 0.5);
  }
  char bearing[8];
  readText(request, "brg", bearing, sizeof(bearing));
  if (!bearing[0]) {
    form.ant_bearing = ADV_ANT_UNSET;
  } else {
    char* end = nullptr;
    long deg = strtol(bearing, &end, 10);
    if (!end || end == bearing || *end || deg < 0 || deg > 359) {
      setNodeNote("Направление: 0–359° или пусто. 0° — север");
      request->redirect("/?at=node");
      return;
    }
    form.ant_bearing = (uint16_t)deg;
  }
  int queued = queueNode(form);
  if (queued == 2) setNodeNote("Не хватило памяти");
  else if (queued) setNodeNote("Предыдущие настройки ещё сохраняются");
  else setNodeNote("Сохраняем настройки узла…");
  request->redirect("/?at=node");
}

static void handleCmd(AsyncWebServerRequest* request) {
  if (!allowed(request)) return;
  if (!request->hasParam("cmd", true)) {
    request->redirect("/?at=console");
    return;
  }
  char command[CMD_CAP];
  copyCommand(command, sizeof(command), request->getParam("cmd", true)->value());
  if (!command[0] || !enqueueCommand(command)) {
    request->redirect(command[0] ? "/?at=console&busy=1" : "/?at=console");
    return;
  }
  request->redirect("/?at=console");
}

static bool ntpHostOk(const char* value) {
  if (!value || !value[0]) return false;
  size_t n = strlen(value);
  if (n >= 64) return false;
  for (size_t i = 0; i < n; i++) {
    char c = value[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':';
    if (!ok) return false;
  }
  return true;
}

static void handleTel(AsyncWebServerRequest* request) {
  if (!allowed(request)) return;
  TelJob* job = (TelJob*)malloc(sizeof(TelJob));
  if (!job) {
    setTelNote("Не хватило памяти");
    request->redirect("/?at=tel");
    return;
  }
  memset(job, 0, sizeof(TelJob));
  job->enable = request->hasParam("en", true) && request->getParam("en", true)->value() == "on";
  job->tx = request->hasParam("tx", true) && request->getParam("tx", true)->value() == "on";
  if (request->hasParam("iata", true)) copyCommand(job->iata, sizeof(job->iata), request->getParam("iata", true)->value());
  if (request->hasParam("host", true)) copyCommand(job->host, sizeof(job->host), request->getParam("host", true)->value());
  if (request->hasParam("user", true)) copyCommand(job->user, sizeof(job->user), request->getParam("user", true)->value());
  if (request->hasParam("bpass", true)) copyCommand(job->bpass, sizeof(job->bpass), request->getParam("bpass", true)->value());
  job->port = 0;
  if (request->hasParam("port", true)) {
    char port_text[8];
    port_text[0] = 0;
    copyCommand(port_text, sizeof(port_text), request->getParam("port", true)->value());
    long port = 0;
    bool digits = port_text[0] != 0;
    for (const char* p = port_text; digits && *p; p++) {
      if (*p < '0' || *p > '9') digits = false;
      else {
        port = port * 10 + (*p - '0');
        if (port > 65535) digits = false;
      }
    }
    if (!digits || port < 1) {
      free(job);
      setTelNote("Порт должен быть от 1 до 65535");
      request->redirect("/?at=tel");
      return;
    }
    job->port = (uint16_t)port;
  }
  job->seq = tel_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
  tel_hold_en = job->enable;
  tel_hold_tx = job->tx;
  tel_hold_port = job->port;
  strncpy(tel_hold_iata, job->iata, sizeof(tel_hold_iata) - 1);
  tel_hold_iata[sizeof(tel_hold_iata) - 1] = 0;
  strncpy(tel_hold_host, job->host, sizeof(tel_hold_host) - 1);
  tel_hold_host[sizeof(tel_hold_host) - 1] = 0;
  strncpy(tel_hold_user, job->user, sizeof(tel_hold_user) - 1);
  tel_hold_user[sizeof(tel_hold_user) - 1] = 0;
  strncpy(tel_hold_bpass, job->bpass, sizeof(tel_hold_bpass) - 1);
  tel_hold_bpass[sizeof(tel_hold_bpass) - 1] = 0;
  tel_hold.store(1, std::memory_order_release);
  TelJob* expected = nullptr;
  if (!tel_job.compare_exchange_strong(expected, job)) {
    free(job);
    setTelNote("Предыдущие настройки ещё сохраняются");
    request->redirect("/?at=tel");
    return;
  }
  setTelNote("Сохраняем настройки…");
  request->redirect("/?at=tel");
}

static void handlePower(AsyncWebServerRequest* request) {
  if (!allowed(request)) return;
  long mode = -1;
  if (!readLong(request, "mode", mode) || mode < WIFI_POWER_NONE || mode > WIFI_POWER_LIGHT) {
    setPowerNote("Неизвестный режим");
    request->redirect("/?at=power");
    return;
  }
  power_hold.store((uint8_t)mode, std::memory_order_release);
  power_job.store((uint8_t)mode, std::memory_order_release);
  setPowerNote("Сохраняем…");
  request->redirect("/?at=power");
}

static void handleNtp(AsyncWebServerRequest* request) {
  if (!allowed(request)) return;
  if (request->hasParam("sync", true)) {
    if (!enqueueCommand("ntp sync")) request->redirect("/?at=console&busy=1");
    else request->redirect("/?at=console");
    return;
  }
  const char* mode = "off";
  if (request->hasParam("en", true) && request->getParam("en", true)->value() == "on") mode = "on";
  char server[64];
  server[0] = 0;
  if (request->hasParam("server", true)) copyCommand(server, sizeof(server), request->getParam("server", true)->value());
  char command[CMD_CAP];
  if (server[0] && !ntpHostOk(server)) {
    snprintf(command, sizeof(command), "set ntp.server");
  } else if (server[0]) {
    snprintf(command, sizeof(command), "set ntp %s %s", mode, server);
  } else if (strcmp(mode, "on") == 0) {
    snprintf(command, sizeof(command), "set ntp on");
  } else {
    snprintf(command, sizeof(command), "set ntp off");
  }
  if (!enqueueCommand(command)) request->redirect("/?at=ntp&busy=1");
  else request->redirect("/?at=ntp");
}

static void handleAct(AsyncWebServerRequest* request) {
  if (!allowed(request)) return;
  const char* command = nullptr;
  if (request->hasParam("act", true)) command = actionCommand(request->getParam("act", true)->value());
  char at[16];
  at[0] = 0;
  readText(request, "at", at, sizeof(at));
  const char* pane = "console";
  if (!strcmp(at, "regions") || !strcmp(at, "status") || !strcmp(at, "sys") || !strcmp(at, "stats")) {
    pane = at;
  }
  char loc[40];
  if (!command || !enqueueCommand(command)) {
    snprintf(loc, sizeof(loc), command ? "/?at=%s&busy=1" : "/?at=%s", pane);
    request->redirect(loc);
    return;
  }
  snprintf(loc, sizeof(loc), "/?at=%s", pane);
  request->redirect(loc);
}

static void onUptime(AsyncWebServerRequest* request) {
  if (!allowed(request)) return;
  char body[16];
  snprintf(body, sizeof(body), "%lu", (unsigned long)(millis() / 1000));
  AsyncWebServerResponse* response = request->beginResponse(200, "text/plain; charset=utf-8", body);
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("Connection", "close");
  request->send(response);
}

#ifndef DISABLE_WIFI_OTA

static const uint8_t FW_IDLE = 0;
static const uint8_t FW_RX = 1;
static const uint8_t FW_OK = 2;
static const uint8_t FW_FAIL = 3;

static std::atomic<uint8_t> fw_phase{FW_IDLE};
static std::atomic<uint32_t> fw_reboot_at{0};
static AsyncWebServerRequest* fw_owner = nullptr;
static bool fw_opened = false;
static bool fw_unauth = false;
static char fw_error[96];
// Written before FW_RX is published. The main loop watches this from the other core.
static std::atomic<uint32_t> fw_touch_ms{0};

static void fwNote(const char* text) {
  strncpy(fw_error, text ? text : "", sizeof(fw_error) - 1);
  fw_error[sizeof(fw_error) - 1] = 0;
}

static bool fwIsBin(const char* name) {
  size_t n = strlen(name);
  if (n < 4) return false;
  const char* e = name + n - 4;
  char b = e[1] >= 'A' && e[1] <= 'Z' ? (char)(e[1] - 'A' + 'a') : e[1];
  char i = e[2] >= 'A' && e[2] <= 'Z' ? (char)(e[2] - 'A' + 'a') : e[2];
  char nch = e[3] >= 'A' && e[3] <= 'Z' ? (char)(e[3] - 'A' + 'a') : e[3];
  return e[0] == '.' && b == 'b' && i == 'i' && nch == 'n';
}

static bool fwHasMerged(const char* name) {
  const char word[] = "merged";
  for (size_t i = 0; name[i]; i++) {
    size_t j = 0;
    while (word[j]) {
      char c = name[i + j];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if (!c || c != word[j]) break;
      j++;
    }
    if (!word[j]) return true;
  }
  return false;
}

static const char* fwErrText() {
  uint8_t err = Update.getError();
  if (err == UPDATE_ERROR_SPACE || err == UPDATE_ERROR_SIZE || err == UPDATE_ERROR_NO_PARTITION) {
    return "Файл больше раздела прошивки. Нужен .bin без merged.";
  }
  if (err == UPDATE_ERROR_MAGIC_BYTE) return "Файл не похож на прошивку ESP32.";
  return "Не удалось записать прошивку.";
}

static void fwFail(const char* text) {
  if (fw_opened) {
    Update.printError(Serial);
    Update.abort();
    fw_opened = false;
  }
  fwNote(text);
  board.setInhibitSleep(false);
  fw_phase.store(FW_FAIL, std::memory_order_release);
  Serial.printf("OTA: %s\n", fw_error);
}

static bool fwAuthed(AsyncWebServerRequest* request) {
  if (cookieOk(request)) return true;
  uint32_t slot = page_slot.load(std::memory_order_acquire);
  return request->authenticate("admin", admin_pass[slot]);
}

static void fwAbandon(const char* text) {
  uint8_t expected = FW_RX;
  if (!fw_phase.compare_exchange_strong(expected, FW_FAIL, std::memory_order_acq_rel, std::memory_order_acquire)) return;
  if (fw_opened) {
    Update.printError(Serial);
    Update.abort();
    fw_opened = false;
  }
  fwNote(text);
  board.setInhibitSleep(false);
  Serial.printf("OTA: %s\n", fw_error);
  fw_owner = nullptr;
  fw_phase.store(FW_IDLE, std::memory_order_release);
}

static void fwWatch() {
  if (fw_phase.load(std::memory_order_acquire) != FW_RX) return;
  uint32_t touch = fw_touch_ms.load(std::memory_order_acquire);
  if ((uint32_t)(millis() - touch) < 20000) return;
  fwAbandon("Загрузка прервалась");
}

static bool fwClaim(AsyncWebServerRequest* request) {
  uint8_t phase = fw_phase.load(std::memory_order_acquire);
  if (phase == FW_RX || phase == FW_OK) return false;
  // Stamp the watchdog before publishing FW_RX. Otherwise the other core sees a busy
  // upload with the previous timestamp (zero after boot) and aborts it at once.
  uint32_t now = millis();
  if (!now) now = 1;
  fw_touch_ms.store(now, std::memory_order_relaxed);
  uint8_t expected = phase;
  if (!fw_phase.compare_exchange_strong(expected, FW_RX, std::memory_order_release, std::memory_order_acquire)) return false;
  fw_owner = request;
  fw_opened = false;
  fw_unauth = false;
  fw_error[0] = 0;
  fw_reboot_at.store(0, std::memory_order_relaxed);
  fw_touch_ms.store(millis() ? millis() : 1, std::memory_order_release);
  return true;
}

static void onFwUpload(AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
  if (request != fw_owner) {
    if (!fwClaim(request)) return;
    request->onDisconnect([request]() {
      if (fw_owner != request) return;
      fwAbandon("Загрузка прервалась");
    });
    if (index) {
      fwFail("Загрузка началась не с начала файла.");
      return;
    }
    if (!fwAuthed(request)) {
      fw_unauth = true;
      fwFail("Нужен вход администратора.");
      return;
    }
    if (!fwIsBin(filename.c_str()) || fwHasMerged(filename.c_str())) {
      fwFail("Нужен файл .bin без merged в имени.");
      return;
    }
    board.setInhibitSleep(true);
    // Free WS HTML/JSON buffers so Update has contiguous heap.
    releaseLiveBuffers();
    if (socket) socket->closeAll();
  }
  if (request != fw_owner || fw_phase.load(std::memory_order_acquire) != FW_RX) return;
  uint32_t now = millis();
  fw_touch_ms.store(now ? now : 1, std::memory_order_release);
  if (!fw_opened) {
    if (!len) {
      if (final) fwFail("Файл пустой.");
      return;
    }
    if (data[0] != 0xE9) {
      fwFail("Файл не похож на прошивку ESP32. Нужен .bin без merged.");
      return;
    }
    releaseLiveBuffers();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      Update.printError(Serial);
      fwFail(fwErrText());
      return;
    }
    fw_opened = true;
  }
  if (len && Update.write(data, len) != len) {
    fwFail(fwErrText());
    return;
  }
  if (!final) return;
  if (!Update.end(true)) {
    const char* msg = fwErrText();
    Update.printError(Serial);
    if (Update.isRunning()) Update.abort();
    fw_opened = false;
    fwFail(msg);
    return;
  }
  fw_opened = false;
  uint32_t when = millis() + 2000;
  if (!when) when = 1;
  fw_reboot_at.store(when, std::memory_order_release);
  fw_phase.store(FW_OK, std::memory_order_release);
  Serial.println("OTA: записано, перезагрузка");
}

static void onFwStatus(AsyncWebServerRequest* request) {
  if (!allowed(request)) return;
  // The uptime probe is /fw?t=… . A browser refresh of /fw must get the page, not a number.
  if (!request->hasParam("t")) {
    request->redirect("/?at=fw");
    return;
  }
  char body[16];
  snprintf(body, sizeof(body), "%lu", (unsigned long)(millis() / 1000));
  AsyncWebServerResponse* response = request->beginResponse(200, "text/plain; charset=utf-8", body);
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("Connection", "close");
  request->send(response);
}

static void onFwDone(AsyncWebServerRequest* request) {
  if (request != fw_owner) {
    request->send(409, "text/plain; charset=utf-8", "Обновление уже идёт");
    return;
  }
  if (fw_phase.load(std::memory_order_acquire) == FW_OK) {
    // Restart later from the main loop. Restarting here drops the socket before this answer is sent.
    AsyncWebServerResponse* response = request->beginResponse(200, "text/plain; charset=utf-8", "OK");
    response->addHeader("Connection", "close");
    request->send(response);
    return;
  }
  const char* msg = fw_error[0] ? fw_error : "Ошибка обновления";
  request->send(fw_unauth ? 401 : 400, "text/plain; charset=utf-8", msg);
  fw_owner = nullptr;
  fw_phase.store(FW_IDLE, std::memory_order_release);
}

#endif

extern "C" void repeaterWebStop() {
#ifndef DISABLE_WIFI_OTA
  if (fw_phase.load(std::memory_order_acquire) == FW_RX) {
    fwFail("Загрузка прервана");
    fw_owner = nullptr;
    fw_phase.store(FW_IDLE, std::memory_order_release);
  }
#endif
  releaseLiveBuffers();
  if (socket) {
    socket->closeAll();
    socket = nullptr;
  }
  server_ip = 0;
  if (!server) return;
  server->end();
  delete server;
  server = nullptr;
}

void RepeaterWeb::stop() {
  repeaterWebStop();
}

void RepeaterWeb::poll(bool enabled) {
#ifndef DISABLE_WIFI_OTA
  uint8_t phase = fw_phase.load(std::memory_order_acquire);
  if (phase == FW_RX) {
    fwWatch();
    phase = fw_phase.load(std::memory_order_acquire);
    if (phase == FW_RX) return;
  }
  if (phase == FW_OK) {
    uint32_t when = fw_reboot_at.load(std::memory_order_acquire);
    if (when && (int32_t)(millis() - when) >= 0) {
      Serial.println("OTA: перезагрузка");
      ESP.restart();
    }
    return;
  }
#endif
  uint32_t reboot_at = web_reboot_at.load(std::memory_order_acquire);
  if (reboot_at && (int32_t)(millis() - reboot_at) >= 0) {
    char command[CMD_CAP];
    char reply[CMD_CAP];
    memcpy(command, web_reboot_cmd, CMD_CAP);
    command[CMD_CAP - 1] = 0;
    reply[0] = 0;
    web_reboot_at.store(0, std::memory_order_release);
    Serial.println("Web: перезагрузка");
    the_mesh.handleCommand(0, command, reply);
    return;
  }
  serviceCommand();
  serviceMqttJob();
  serviceTelJob();
  serviceRadioJob();
  serviceNodeJob();
  serviceRegionJob();
  servicePowerJob();

  uint32_t ip = 0;
  if (WiFi.status() == WL_CONNECTED) ip = (uint32_t)WiFi.localIP();
  bool up = enabled && ip != 0;
  if (server && (!up || ip != server_ip)) stop();

  if (enabled || server) refreshSnapshot();
  if (server) pushLive(false);
  if (!up || server) return;

  server = new AsyncWebServer(80);
  socket = new AsyncWebSocket("/ws");
  if (!server || !socket) {
    delete socket;
    delete server;
    socket = nullptr;
    server = nullptr;
    return;
  }
  publishSnapshot();
  socket->handleHandshake([](AsyncWebServerRequest* request) {
    uint32_t slot = page_slot.load(std::memory_order_acquire);
    if (cookieOk(request)) return true;
    return request->authenticate("admin", admin_pass[slot]);
  });
  socket->onEvent([](AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void*, uint8_t*, size_t) {
    if (type == WS_EVT_CONNECT && client) {
      client->setCloseClientOnQueueFull(false);
      last_push_ms = 0;  // deliver pending region reload / notes promptly
    }
  });
  server->addHandler(socket);
  server->on("/", HTTP_GET, sendPage);
  server->on("/api/stats", HTTP_GET, sendStats);
  server->on("/cmd", HTTP_POST, handleCmd);
  server->on("/radio", HTTP_POST, handleRadio);
  server->on("/up", HTTP_GET, onUptime);
  server->on("/node", HTTP_POST, handleNode);
  server->on("/region", HTTP_POST, handleRegion);
  server->on("/power", HTTP_POST, handlePower);
  server->on("/ntp", HTTP_POST, handleNtp);
  server->on("/tel", HTTP_POST, handleTel);
  server->on("/act", HTTP_POST, handleAct);
  server->on("/mqtt", HTTP_POST, onMqtt);
#ifndef DISABLE_WIFI_OTA
  server->on("/fw", HTTP_GET, onFwStatus);
  server->on("/fw", HTTP_POST, onFwDone, onFwUpload);
#endif
  server->onNotFound([](AsyncWebServerRequest* request) {
    if (request->method() != HTTP_GET) {
      request->send(404, "text/plain; charset=utf-8", "not found");
      return;
    }
    if (request->url() == "/favicon.ico") {
      request->send(204);
      return;
    }
    const String& url = request->url();
    if (url == "/radio") request->redirect("/?at=radio");
    else if (url == "/node") request->redirect("/?at=node");
    else if (url == "/region") request->redirect("/?at=regions");
    else if (url == "/power") request->redirect("/?at=power");
    else request->redirect("/");
  });
  server->begin();
  server_ip = ip;
  IPAddress addr = WiFi.localIP();
  Serial.printf("Web: http://%u.%u.%u.%u/\n", addr[0], addr[1], addr[2], addr[3]);
}

bool RepeaterWeb::webBusy() const {
  if (commandPending()) return true;
#ifndef DISABLE_WIFI_OTA
  uint8_t phase = fw_phase.load(std::memory_order_acquire);
  if (phase == FW_RX || phase == FW_OK) return true;
#endif
  return false;
}

#else

extern "C" void repeaterWebStop() {}

void RepeaterWeb::poll(bool) {}
void RepeaterWeb::stop() {}
bool RepeaterWeb::commandQueued() const { return false; }
bool RepeaterWeb::viewerConnected() const { return false; }
bool RepeaterWeb::webBusy() const { return false; }

#endif
