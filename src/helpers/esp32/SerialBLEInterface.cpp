#include "SerialBLEInterface.h"
#include "esp_mac.h"
#include <esp_bt.h>

extern void companionNoteLinkActivity() __attribute__((weak));
extern uint32_t companionIdlePeriodMs() __attribute__((weak));

static void noteLinkActivity() {
  if (companionNoteLinkActivity) companionNoteLinkActivity();
}

// See the following for generating UUIDs:
// https://www.uuidgenerator.net/

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" // UART service UUID
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#define ADVERT_RESTART_DELAY  1000   // millis

// Advertising units are 0.625 ms. A 3 s period still shows up in a scan, but
// the phone's connect window often misses the burst, so the link is flaky.
// 0.5–1 s is long enough for the radio to sleep between bursts and short
// enough that a connect request lands.
#define BLE_ADV_UNITS_MIN  800
#define BLE_ADV_UNITS_MAX  1600
// Connection-interval units are 1.25 ms. Idle grows as 100 * 8^(t/20s),
// from 100 ms to 800 ms. A transfer still uses the fast set, then the
// curve starts over.
#define BLE_CONN_IDLE_LATENCY  0
#define BLE_CONN_IDLE_TIMEOUT  400  // 4 s, in 10 ms units
#define BLE_CONN_FAST_MIN      16   // 20 ms
#define BLE_CONN_FAST_MAX      30   // 37.5 ms
#define BLE_CONN_FAST_LATENCY  0
#define BLE_CONN_FAST_TIMEOUT  400
#define BLE_CONN_FAST_HOLD_MS  2500
#define CONN_MODE_IDLE  1
#define CONN_MODE_FAST  2

static void applyIdleAdvertising(BLEAdvertising* adv) {
  adv->setMinInterval(BLE_ADV_UNITS_MIN);
  adv->setMaxInterval(BLE_ADV_UNITS_MAX);
  adv->setMinPreferred(80);   // 100 ms, first staircase step
  adv->setMaxPreferred(80);
}

void SerialBLEInterface::begin(const char* prefix, char* name, uint32_t pin_code) {
  _pin_code = pin_code;

  if (strcmp(name, "@@MAC") == 0) {
    uint8_t addr[8];
    memset(addr, 0, sizeof(addr));
    esp_efuse_mac_get_default(addr);
    sprintf(name, "%02X%02X%02X%02X%02X%02X",    // modify (IN-OUT param)
          addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
  }
  char dev_name[32+16];
  sprintf(dev_name, "%s%s", prefix, name);

  // Create the BLE Device
  BLEDevice::init(dev_name);
  BLEDevice::setSecurityCallbacks(this);
  BLEDevice::setMTU(MAX_FRAME_SIZE);

  BLESecurity  sec;
  sec.setStaticPIN(pin_code);
  sec.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);

  //BLEDevice::setPower(ESP_PWR_LVL_N8);

  // Create the BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(this);

  // Create the BLE Service
  pService = pServer->createService(SERVICE_UUID);

  // Create a BLE Characteristic
  pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_TX, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic * pRxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_RX, BLECharacteristic::PROPERTY_WRITE);
  pRxCharacteristic->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
  pRxCharacteristic->setCallbacks(this);

  pServer->getAdvertising()->addServiceUUID(SERVICE_UUID);
}

// -------- BLESecurityCallbacks methods

uint32_t SerialBLEInterface::onPassKeyRequest() {
  BLE_DEBUG_PRINTLN("onPassKeyRequest()");
  return _pin_code;
}

void SerialBLEInterface::onPassKeyNotify(uint32_t pass_key) {
  BLE_DEBUG_PRINTLN("onPassKeyNotify(%u)", pass_key);
}

bool SerialBLEInterface::onConfirmPIN(uint32_t pass_key) {
  BLE_DEBUG_PRINTLN("onConfirmPIN(%u)", pass_key);
  return true;
}

bool SerialBLEInterface::onSecurityRequest() {
  BLE_DEBUG_PRINTLN("onSecurityRequest()");
  return true;  // allow
}

void SerialBLEInterface::onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) {
  if (cmpl.success) {
    BLE_DEBUG_PRINTLN(" - SecurityCallback - Authentication Success");
    deviceConnected = true;
  } else {
    BLE_DEBUG_PRINTLN(" - SecurityCallback - Authentication Failure*");

    //pServer->removePeerDevice(pServer->getConnId(), true);
    pServer->disconnect(pServer->getConnId());
    adv_restart_time = millis() + ADVERT_RESTART_DELAY;
  }
}

// -------- BLEServerCallbacks methods

void SerialBLEInterface::onConnect(BLEServer* pServer) {
}

void SerialBLEInterface::onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t *param) {
  BLE_DEBUG_PRINTLN("onConnect(), conn_id=%d, mtu=%d", param->connect.conn_id, pServer->getPeerMTU(param->connect.conn_id));
  last_conn_id = param->connect.conn_id;
  memcpy(peer_addr, param->connect.remote_bda, sizeof(peer_addr));
  peer_known = true;
  conn_mode = 0;
  conn_idle_units = 0;
  conn_fast_until = 0;
  conn_update_not_before = 0;
  conn_params_after = millis() + 1500;
}

void SerialBLEInterface::onMtuChanged(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) {
  BLE_DEBUG_PRINTLN("onMtuChanged(), mtu=%d", pServer->getPeerMTU(param->mtu.conn_id));
}

void SerialBLEInterface::onDisconnect(BLEServer* pServer) {
  BLE_DEBUG_PRINTLN("onDisconnect()");
  deviceConnected = false;
  peer_known = false;
  conn_mode = 0;
  conn_idle_units = 0;
  conn_fast_until = 0;
  conn_update_not_before = 0;
  if (_isEnabled) {
    adv_restart_time = millis() + ADVERT_RESTART_DELAY;
  }
}

// -------- BLECharacteristicCallbacks methods

void SerialBLEInterface::onWrite(BLECharacteristic* pCharacteristic, esp_ble_gatts_cb_param_t* param) {
  uint8_t* rxValue = pCharacteristic->getData();
  int len = pCharacteristic->getLength();

  if (len > MAX_FRAME_SIZE) {
    BLE_DEBUG_PRINTLN("ERROR: onWrite(), frame too big, len=%d", len);
  } else {
    Frame frame = {};
    frame.len = len;
    memcpy(frame.buf, rxValue, len);

    if (xQueueSend(recv_queue, &frame, 0) != pdTRUE) {
      BLE_DEBUG_PRINTLN("ERROR: onWrite(), recv_queue is full!");
    }
  }
}

// ---------- public methods

void SerialBLEInterface::clearBuffers() {
  xQueueReset(recv_queue);
  send_queue_len = 0;
}

void SerialBLEInterface::enable() { 
  if (_isEnabled) return;

  _isEnabled = true;
  clearBuffers();

  // Start the service
  pService->start();

  applyIdleAdvertising(pServer->getAdvertising());
  pServer->getAdvertising()->start();
  adv_restart_time = 0;
}

void SerialBLEInterface::disable() {
  _isEnabled = false;

  BLE_DEBUG_PRINTLN("SerialBLEInterface::disable");

  pServer->getAdvertising()->stop();
  pServer->disconnect(last_conn_id);
  pService->stop();
  oldDeviceConnected = deviceConnected = false;
  peer_known = false;
  conn_mode = 0;
  conn_idle_units = 0;
  conn_fast_until = 0;
  conn_update_not_before = 0;
  adv_restart_time = 0;
}

size_t SerialBLEInterface::writeFrame(const uint8_t src[], size_t len) {
  if (len > MAX_FRAME_SIZE) {
    BLE_DEBUG_PRINTLN("writeFrame(), frame too big, len=%d", len);
    return 0;
  }

  if (deviceConnected && len > 0) {
    noteLinkActivity();
    conn_fast_until = millis() + BLE_CONN_FAST_HOLD_MS;
    if (conn_fast_until == 0) conn_fast_until = 1;
    if (send_queue_len >= FRAME_QUEUE_SIZE) {
      BLE_DEBUG_PRINTLN("writeFrame(), send_queue is full!");
      return 0;
    }

    send_queue[send_queue_len].len = len;  // add to send queue
    memcpy(send_queue[send_queue_len].buf, src, len);
    send_queue_len++;

    return len;
  }
  return 0;
}

#define  BLE_WRITE_MIN_INTERVAL   60

bool SerialBLEInterface::isWriteBusy() const {
  return millis() < _last_write + BLE_WRITE_MIN_INTERVAL;   // still too soon to start another write?
}

size_t SerialBLEInterface::checkRecvFrame(uint8_t dest[]) {
  // disable() then BLEDevice::deinit() leaves pServer dangling. The
  // interface manager still polls this object while the link is off.
  if (!_isEnabled) return 0;
  if (send_queue_len > 0   // first, check send queue
    && millis() >= _last_write + BLE_WRITE_MIN_INTERVAL    // space the writes apart
  ) {
    _last_write = millis();
    pTxCharacteristic->setValue(send_queue[0].buf, send_queue[0].len);
    pTxCharacteristic->notify();

    BLE_DEBUG_PRINTLN("writeBytes: sz=%d, hdr=%d", (uint32_t)send_queue[0].len, (uint32_t) send_queue[0].buf[0]);

    send_queue_len--;
    for (int i = 0; i < send_queue_len; i++) {   // delete top item from queue
      send_queue[i] = send_queue[i + 1];
    }
  }

  size_t got = 0;
  Frame frame;
  if (xQueueReceive(recv_queue, &frame, 0) == pdTRUE) {
    memcpy(dest, frame.buf, frame.len);
    BLE_DEBUG_PRINTLN("readBytes: sz=%d, hdr=%d", (uint32_t) frame.len, (uint32_t) dest[0]);
    got = frame.len;
    noteLinkActivity();
  }

  if (deviceConnected != oldDeviceConnected) {
    if (!deviceConnected) {    // disconnecting
      clearBuffers();

      BLE_DEBUG_PRINTLN("SerialBLEInterface -> disconnecting...");

      adv_restart_time = millis() + ADVERT_RESTART_DELAY;
    } else {
      BLE_DEBUG_PRINTLN("SerialBLEInterface -> stopping advertising");
      BLE_DEBUG_PRINTLN("SerialBLEInterface -> connecting...");
      // connecting
      // do stuff here on connecting
      pServer->getAdvertising()->stop();
      adv_restart_time = 0;
    }
    oldDeviceConnected = deviceConnected;
  }

  if (adv_restart_time && millis() >= adv_restart_time) {
    if (pServer->getConnectedCount() == 0) {
      BLE_DEBUG_PRINTLN("SerialBLEInterface -> re-starting advertising");
      applyIdleAdvertising(pServer->getAdvertising());
      pServer->getAdvertising()->start();  // re-Start advertising
    }
    adv_restart_time = 0;
  }

  maintainConnParams();
  return got;
}

void SerialBLEInterface::maintainConnParams() {
  if (!deviceConnected || !peer_known) return;
  bool want_fast = send_queue_len > 0 ||
      (conn_fast_until != 0 && (long)(millis() - conn_fast_until) < 0);
  // Let pairing finish before asking for the slow interval. A contact sync
  // may start immediately and needs the fast interval without that wait.
  if (!want_fast && (conn_params_after == 0 || (long)(millis() - conn_params_after) < 0)) return;
  uint8_t want = want_fast ? CONN_MODE_FAST : CONN_MODE_IDLE;
  uint32_t period_ms = 100;
  if (!want_fast && companionIdlePeriodMs) period_ms = companionIdlePeriodMs();
  uint16_t units = (uint16_t)((period_ms * 4) / 5);
  if (units < 80) units = 80;
  if (units > 640) units = 640;
  // The curve moves every wake. Ask the phone only after a 25% rise, about
  // every 2 s. A shorter interval (the user just did something) applies at once.
  if (want == conn_mode) {
    if (want_fast) return;
    if (conn_idle_units != 0 && units >= conn_idle_units &&
        units < conn_idle_units + conn_idle_units / 4) return;
  }
  if (conn_update_not_before != 0 && (long)(millis() - conn_update_not_before) < 0) return;
  esp_ble_conn_update_params_t update = {};
  memcpy(update.bda, peer_addr, sizeof(update.bda));
  if (want_fast) {
    update.min_int = BLE_CONN_FAST_MIN;
    update.max_int = BLE_CONN_FAST_MAX;
    update.latency = BLE_CONN_FAST_LATENCY;
    update.timeout = BLE_CONN_FAST_TIMEOUT;
  } else {
    update.min_int = units;
    update.max_int = units;
    update.latency = BLE_CONN_IDLE_LATENCY;
    update.timeout = BLE_CONN_IDLE_TIMEOUT;
  }
  if (esp_ble_gap_update_conn_params(&update) == ESP_OK) {
    conn_mode = want;
    conn_idle_units = want_fast ? 0 : units;
    // A parameter change can drop the controller out of modem sleep and
    // leave it there. Put it back, or the idle current climbs to ~26 mA.
    // ESP32-C6/H2 Arduino builds do not export esp_bt_sleep_enable().
#if !defined(CONFIG_IDF_TARGET_ESP32C6) && !defined(CONFIG_IDF_TARGET_ESP32H2)
    esp_bt_sleep_enable();
#endif
  } else {
    conn_update_not_before = millis() + 3000;
    if (conn_update_not_before == 0) conn_update_not_before = 1;
  }
}

bool SerialBLEInterface::isConnected() const {
  return deviceConnected;  //pServer != NULL && pServer->getConnectedCount() > 0;
}

bool SerialBLEInterface::isLinkUp() const {
  return deviceConnected || (pServer && pServer->getConnectedCount() > 0);
}
