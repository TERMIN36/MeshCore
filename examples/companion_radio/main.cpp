#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include <math.h>
#if defined(ESP32) && defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
  #include <esp_pm.h>
#endif
#if defined(ESP32) && ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  #include <HWCDC.h>
#endif
#include "MyMesh.h"
#include "CompanionLink.h"

// Believe it or not, this std C function is busted on some platforms!
static uint32_t _atoi(const char* sp) {
  uint32_t n = 0;
  while (*sp && *sp >= '0' && *sp <= '9') {
    n *= 10;
    n += (*sp++ - '0');
  }
  return n;
}

// interface manager
#include <helpers/MultiSerialInterface.h>
MultiSerialInterface interface_manager;

// include bluetooth interface
#if defined(BLE_PIN_CODE)
  #ifdef ESP32
    // include esp32 bluetooth interface
    #include <helpers/esp32/SerialBLEInterface.h>
    SerialBLEInterface bluetooth_interface;
  #elif defined(NRF52_PLATFORM)
    // include nrf52 bluetooth interface
    #include <helpers/nrf52/SerialBLEInterface.h>
    SerialBLEInterface bluetooth_interface;
  #else
    #error "SerialBLEInterface is not defined for this platform"
  #endif
#endif

// include wifi interface
#if defined(ESP32) && defined(BLE_PIN_CODE)
  #ifndef TCP_PORT
    #define TCP_PORT 5000
  #endif
  #include <helpers/esp32/SerialWifiInterface.h>
  SerialWifiInterface wifi_interface;
#endif

#include <helpers/ArduinoSerialInterface.h>
ArduinoSerialInterface usb_serial_interface;

// include ethernet interface
#if defined(ETHERNET_ENABLED)
  #include <helpers/ethernet/EthernetInterface.h>
  ETHERNET_CLASS ethernet_interface;
#endif

// include hardware serial interface
#if defined(SERIAL_RX)
  #include <helpers/ArduinoSerialInterface.h>
  ArduinoSerialInterface hardware_serial_interface;
  HardwareSerial companion_serial(1);
#endif

// platform file system
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
  #if defined(QSPIFLASH)
    #include <CustomLFS_QSPIFlash.h>
    DataStore store(InternalFS, QSPIFlash, rtc_clock);
  #else
    #if defined(EXTRAFS)
      #include <CustomLFS.h>
      CustomLFS ExtraFS(0xD4000, 0x19000, 128);
      DataStore store(InternalFS, ExtraFS, rtc_clock);
    #else
      DataStore store(InternalFS, rtc_clock);
    #endif
  #endif
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
  DataStore store(LittleFS, rtc_clock);
#elif defined(ESP32)
  #include <SPIFFS.h>
  DataStore store(SPIFFS, rtc_clock);
#endif

/* GLOBAL OBJECTS */
#ifdef DISPLAY_CLASS
  #include "UITask.h"
  UITask ui_task(&board, &interface_manager);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;
MyMesh the_mesh(radio_driver, fast_rng, rtc_clock, tables, store
   #ifdef DISPLAY_CLASS
      , &ui_task
   #endif
);

/* END GLOBAL OBJECTS */

void halt() {
  while (1) ;
}

/* WIFI RECONNECT TRACKERS */
#if defined(ESP32) && defined(BLE_PIN_CODE)
  bool wifi_needs_reconnect = false;
  unsigned long last_wifi_reconnect_attempt = 0;
#endif

#if defined(ESP32) && defined(BLE_PIN_CODE)
// Set once automatic light sleep has programmed the PM table. A later
// setCpu to a different MHz is the reboot from the repeater Wi-Fi work.
static bool companion_clock_locked = false;
#endif

#if defined(ESP32) && defined(BLE_PIN_CODE)
#include <BLEDevice.h>
#if __has_include(<soc/rtc_cntl_reg.h>)
#include <soc/rtc_cntl_reg.h>
#define MESHCORE_HAS_RTC_CNTL_REG 1
#endif
#if __has_include(<soc/syscon_reg.h>)
#include <soc/syscon_reg.h>
#endif
#if __has_include(<soc/system_reg.h>)
#include <soc/system_reg.h>
#endif
#if defined(SYSTEM_WIFI_CLK_EN_REG)
#define MESHCORE_HAS_WIFI_CLK_REG 1
#elif __has_include(<soc/dport_reg.h>)
#include <soc/dport_reg.h>
#include <soc/dport_access.h>
#define SYSTEM_WIFI_CLK_EN_REG DPORT_WIFI_CLK_EN_REG
#define SYSTEM_WIFI_CLK_WIFI_BT_COMMON_M DPORT_WIFI_CLK_WIFI_BT_COMMON_M
#define SYSTEM_BT_BASEBAND_EN DPORT_BT_BASEBAND_EN
#define SYSTEM_BT_LC_EN DPORT_BT_LC_EN
#define MESHCORE_HAS_WIFI_CLK_REG 1
// Classic ESP32 keeps Wi-Fi and Bluetooth in one power domain.
#ifndef RTC_CNTL_BT_FORCE_PD
#define RTC_CNTL_BT_FORCE_PD 0
#define RTC_CNTL_BT_FORCE_PU 0
#define RTC_CNTL_BT_FORCE_ISO 0
#define RTC_CNTL_BT_FORCE_NOISO 0
#endif
#ifdef DPORT_CPU_WAIT_MODE_FORCE_ON
#define SYSTEM_CPU_PER_CONF_REG DPORT_CPU_PER_CONF_REG
#define SYSTEM_CPU_WAIT_MODE_FORCE_ON DPORT_CPU_WAIT_MODE_FORCE_ON
#endif
#endif

static bool companion_wifi_fast = false;
static bool companion_ble_down = false;
// Boot leaves the modem domain biased even when the link pref is off.
// The IDF power-down helper does nothing while its refcount is already
// zero, which is this path: setup starts BLE, then "off" only stops
// advertising. Turning BLE on and off drops the refcount and lands at 7 mA.
static bool companion_modem_released = false;

extern "C" {
void phy_get_romfunc_addr(void);
void phy_close_rf(void);
#if !defined(CONFIG_IDF_TARGET_ESP32)
void phy_xpd_tsens(void);
#endif
}

#if MESHCORE_HAS_WIFI_CLK_REG
static inline void companionWifiClkSet(uint32_t mask) {
#if defined(CONFIG_IDF_TARGET_ESP32)
  DPORT_SET_PERI_REG_MASK(SYSTEM_WIFI_CLK_EN_REG, mask);
#else
  REG_SET_BIT(SYSTEM_WIFI_CLK_EN_REG, mask);
#endif
}

static inline void companionWifiClkClr(uint32_t mask) {
#if defined(CONFIG_IDF_TARGET_ESP32)
  DPORT_CLEAR_PERI_REG_MASK(SYSTEM_WIFI_CLK_EN_REG, mask);
#else
  REG_CLR_BIT(SYSTEM_WIFI_CLK_EN_REG, mask);
#endif
}
#endif

#if defined(SYSTEM_CPU_PER_CONF_REG) && defined(SYSTEM_CPU_WAIT_MODE_FORCE_ON)
static inline void companionCpuPerConfClr(uint32_t mask) {
#if defined(CONFIG_IDF_TARGET_ESP32)
  DPORT_CLEAR_PERI_REG_MASK(SYSTEM_CPU_PER_CONF_REG, mask);
#else
  REG_CLR_BIT(SYSTEM_CPU_PER_CONF_REG, mask);
#endif
}
#endif

// phy_close_rf has to run while the modem clocks are still on. Cutting the
// power domain first latches the analog front-end in the last bias.
static void companionCloseRadioFrontEnd() {
#if MESHCORE_HAS_RTC_CNTL_REG && MESHCORE_HAS_WIFI_CLK_REG
  REG_CLR_BIT(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_WIFI_FORCE_PD | RTC_CNTL_BT_FORCE_PD);
  delay(10);
  companionWifiClkSet(SYSTEM_WIFI_CLK_WIFI_BT_COMMON_M | SYSTEM_BT_BASEBAND_EN | SYSTEM_BT_LC_EN);
  REG_CLR_BIT(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_WIFI_FORCE_ISO | RTC_CNTL_BT_FORCE_ISO);
#endif
  phy_get_romfunc_addr();
  phy_close_rf();
#if !defined(CONFIG_IDF_TARGET_ESP32)
  phy_xpd_tsens();
#endif
}

static void companionReleaseModem() {
  if (companion_modem_released) return;
  if (!companion_ble_down || bluetooth_interface.isEnabled()) return;
  if ((WiFi.getMode() & WIFI_MODE_STA) != 0) return;
  companionCloseRadioFrontEnd();
#if MESHCORE_HAS_RTC_CNTL_REG && MESHCORE_HAS_WIFI_CLK_REG
  REG_SET_BIT(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_WIFI_FORCE_ISO | RTC_CNTL_BT_FORCE_ISO);
  REG_SET_BIT(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_WIFI_FORCE_PD | RTC_CNTL_BT_FORCE_PD);
  REG_CLR_BIT(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_WIFI_FORCE_PU | RTC_CNTL_BT_FORCE_PU);
  REG_CLR_BIT(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_WIFI_FORCE_NOISO | RTC_CNTL_BT_FORCE_NOISO);
  companionWifiClkClr(SYSTEM_WIFI_CLK_WIFI_BT_COMMON_M | SYSTEM_BT_BASEBAND_EN | SYSTEM_BT_LC_EN);
  REG_CLR_BIT(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_XTL_FORCE_PU | RTC_CNTL_BBPLL_FORCE_PU |
                                     RTC_CNTL_BBPLL_I2C_FORCE_PU | RTC_CNTL_BB_I2C_FORCE_PU);
#ifdef SYSTEM_CPU_WAIT_MODE_FORCE_ON
  companionCpuPerConfClr(SYSTEM_CPU_WAIT_MODE_FORCE_ON);
#endif
#endif
  companion_modem_released = true;
}

static void companionAllowModem() {
  if (!companion_modem_released) return;
#if MESHCORE_HAS_RTC_CNTL_REG
  REG_CLR_BIT(RTC_CNTL_DIG_PWC_REG, RTC_CNTL_WIFI_FORCE_PD | RTC_CNTL_BT_FORCE_PD);
  REG_CLR_BIT(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_WIFI_FORCE_ISO | RTC_CNTL_BT_FORCE_ISO);
#endif
  companion_modem_released = false;
}

static void companionBleLightStop();
static void companionBleModemOn();
static void companionWifiCpuAwake();

static void companionWifiCpu(bool on) {
  if (companion_clock_locked) {
    companion_wifi_fast = on;
    return;
  }
#ifdef ESP32_CPU_FREQ
  uint32_t mhz = ESP32_CPU_FREQ;
#else
  uint32_t mhz = 80;
#endif
  if (on && mhz < 160) mhz = 160;
  if (getCpuFrequencyMhz() != mhz) setCpuFrequencyMhz(mhz);
  companion_wifi_fast = on;
}

// WIFI_PS_NONE calls pm_set_sleep_type(0), which aborts while the BLE
// controller is still running. Shut the controller down and keep modem sleep.
static void companionBleStop() {
  companionBleLightStop();
  if (bluetooth_interface.isEnabled()) bluetooth_interface.disable();
  if (!companion_ble_down) {
    BLEDevice::deinit(false);
    companion_ble_down = true;
  }
}

static void companionBleStart() {
  companionAllowModem();
  if (companion_ble_down) {
    bluetooth_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
    companion_ble_down = false;
  }
  if (!bluetooth_interface.isEnabled()) bluetooth_interface.enable();
  companionBleModemOn();
}

static void companionWifiStop() {
  companionWifiCpuAwake();
  wifi_interface.disable();
  wifi_needs_reconnect = false;
  if ((WiFi.getMode() & WIFI_MODE_STA) == 0 && !companion_wifi_fast) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  companionWifiCpu(false);
}

static void companionWifiStart(const char* ssid, const char* pass) {
  companionBleStop();
  companionAllowModem();
  companionWifiCpu(true);
  // MAX modem sleep lets the CPU light-sleep between beacons. MIN keeps
  // the CPU running, which is why Wi-Fi never followed BLE into sleep.
  WiFi.setSleep(WIFI_PS_MAX_MODEM);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (pass && pass[0]) WiFi.begin(ssid, pass);
  else WiFi.begin(ssid);
  wifi_interface.begin(TCP_PORT);
  wifi_interface.enable();
}
#endif

bool companionPhoneConnected() {
  NodePrefs* prefs = the_mesh.getNodePrefs();
  uint8_t mode = companionEffectiveLinkMode(prefs ? prefs->link_mode : LINK_MODE_BLE);
#if defined(BLE_PIN_CODE)
  if (mode == LINK_MODE_BLE) return bluetooth_interface.isEnabled() && bluetooth_interface.isConnected();
#endif
#if defined(ESP32) && defined(BLE_PIN_CODE)
  if (mode == LINK_MODE_WIFI) return wifi_interface.isEnabled() && wifi_interface.isConnected();
#endif
  if (mode == LINK_MODE_USB) return usb_serial_interface.isEnabled() && usb_serial_interface.isConnected();
  return false;
}

void companionApplyLink() {
  NodePrefs* prefs = the_mesh.getNodePrefs();
  uint8_t mode = companionEffectiveLinkMode(prefs ? prefs->link_mode : LINK_MODE_BLE);

  if (mode == LINK_MODE_USB) usb_serial_interface.enable();
  else usb_serial_interface.disable();

#if defined(ESP32) && defined(BLE_PIN_CODE)
  if (mode == LINK_MODE_WIFI) {
    if (prefs && prefs->wifi_ssid[0]) companionWifiStart(prefs->wifi_ssid, prefs->wifi_wpass);
    else {
      companionBleStop();
      companionWifiStop();
      companionReleaseModem();
    }
    return;
  }
  if (wifi_interface.isEnabled() || companion_wifi_fast) {
    companionWifiStop();
  }
  if (mode == LINK_MODE_BLE) companionBleStart();
  else {
    companionBleStop();
    companionReleaseModem();
  }
#elif defined(BLE_PIN_CODE)
  if (mode == LINK_MODE_BLE) bluetooth_interface.enable();
  else bluetooth_interface.disable();
#endif
}

bool companionLinkHoldsCpu() {
  // Station sleep is allowed. Holding the CPU for the whole Wi-Fi session
  // kept hasPendingWork from reaching light sleep.
  return false;
}

static uint32_t link_active_ms = 0;

void companionNoteLinkActivity() {
  link_active_ms = millis();
  if (link_active_ms == 0) link_active_ms = 1;
}

uint32_t companionIdlePeriodMs() {
  uint32_t idle = link_active_ms == 0 ? 0 : (uint32_t)(millis() - link_active_ms);
  // 100 * 8^(idle/20s). ~170 ms at 5 s, ~280 ms at 10 s, 800 ms at 20 s.
  const uint32_t ramp_ms = 20000;
  if (idle >= ramp_ms) return 800;
  float ms = 100.f * expf(2.0794415f * ((float)idle / (float)ramp_ms));
  if (ms < 100.f) return 100;
  if (ms > 800.f) return 800;
  return (uint32_t)(ms + 0.5f);
}

bool companionWifiAddress(char* dest, size_t cap) {
  if (!dest || cap == 0) return false;
  dest[0] = 0;
#if defined(ESP32) && defined(BLE_PIN_CODE)
  if (WiFi.status() != WL_CONNECTED) return false;
  IPAddress ip = WiFi.localIP();
  if (ip == IPAddress((uint32_t)0)) return false;
  snprintf(dest, cap, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return dest[0] != 0;
#else
  return false;
#endif
}

void setup() {
  Serial.begin(115200);
  board.begin();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.begin();
#endif

#ifdef DISPLAY_CLASS
  DisplayDriver* disp = NULL;
  if (display.begin()) {
    disp = &display;
    disp->startFrame();
  #ifdef ST7789
    disp->setTextSize(2);
  #endif
    disp->drawTextCentered(disp->width() / 2, 28, "Loading...");
    disp->endFrame();
  }
#endif

  if (!radio_init()) { halt(); }

  fast_rng.begin(radio_driver.getRngSeed());

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  #if defined(QSPIFLASH)
    if (!QSPIFlash.begin()) {
      // debug output might not be available at this point, might be too early. maybe should fall back to InternalFS here?
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: failed to initialize");
    } else {
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: initialized successfully");
    }
  #else
  #if defined(EXTRAFS)
      ExtraFS.begin();
  #endif
  #endif
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );
#elif defined(ESP32)
  SPIFFS.begin(true);
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );
#else
  #error "need to define filesystem"
#endif

// add bluetooth interface
#if defined(BLE_PIN_CODE)
  bluetooth_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
  interface_manager.addInterface(InterfaceType::Bluetooth, &bluetooth_interface);
#endif

// add wifi interface
#if defined(ESP32) && defined(BLE_PIN_CODE)
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
      if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
          WIFI_DEBUG_PRINTLN("WiFi disconnected. Flagging for reconnect...");
          wifi_needs_reconnect = true;
      } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
          WIFI_DEBUG_PRINTLN("WiFi connected successfully!");
          wifi_needs_reconnect = false;
      }
  });
  interface_manager.addInterface(InterfaceType::WiFi, &wifi_interface);
#endif

  usb_serial_interface.begin(Serial);
  interface_manager.addInterface(InterfaceType::USB, &usb_serial_interface);

// add ethernet interface
#if defined(ETHERNET_ENABLED)
  ethernet_interface.begin();
  interface_manager.addInterface(InterfaceType::Ethernet, &ethernet_interface);
#endif

// add hardware serial interface
#if defined(SERIAL_RX)
  companion_serial.setPins(SERIAL_RX, SERIAL_TX);
  companion_serial.begin(115200);
  hardware_serial_interface.begin(companion_serial);
  interface_manager.addInterface(InterfaceType::HardwareSerial, &hardware_serial_interface);
#endif

  the_mesh.startInterface(interface_manager);
  companionApplyLink();
  sensors.begin();

#if ENV_INCLUDE_GPS == 1
  the_mesh.applyGpsPrefs();
#endif

#ifdef DISPLAY_CLASS
  ui_task.begin(disp, &sensors, the_mesh.getNodePrefs());  // still want to pass this in as dependency, as prefs might be moved
#endif

  board.onBootComplete();
}

#if defined(ESP32) && defined(BLE_PIN_CODE)
#include <sdkconfig.h>
#if (defined(CONFIG_BTDM_CTRL_MODEM_SLEEP) || defined(CONFIG_BT_CTRL_MODEM_SLEEP)) && \
    !defined(CONFIG_IDF_TARGET_ESP32C6) && !defined(CONFIG_IDF_TARGET_ESP32H2)
#include <esp_bt.h>
#define COMPANION_BLE_MODEM_SLEEP 1
#endif

static bool ble_modem_on = false;
static bool ble_cpu_light = false;
static bool ble_cpu_refused = false;

static void companionBleModemOn() {
#if defined(COMPANION_BLE_MODEM_SLEEP)
  if (ble_modem_on) return;
  if (esp_bt_sleep_enable() == ESP_OK) ble_modem_on = true;
#endif
}

// Drop automatic CPU light sleep while the screen or USB needs the CPU.
// Modem sleep stays on so advertising still gates the radio.
static void companionBleCpuAwake() {
  if (!ble_cpu_light) return;
  board.setAutoLightSleep(false);
  ble_cpu_light = false;
}

static void companionBleLightStop() {
  if (ble_cpu_light) {
    board.setAutoLightSleep(false);
    ble_cpu_light = false;
  }
#if defined(COMPANION_BLE_MODEM_SLEEP)
  if (ble_modem_on) {
    esp_bt_sleep_disable();
    ble_modem_on = false;
  }
#endif
}

// Modem sleep plus automatic CPU light sleep. The controller wakes itself for
// connection events and adverts. This cap is only how often the button, the
// mesh and a notify run; a manual esp_light_sleep_start() would stop Bluedroid.
static void companionBleLightSleep(uint32_t sleep_ms) {
  if (sleep_ms == 0) return;
#if !defined(COMPANION_BLE_MODEM_SLEEP)
  delay(sleep_ms > 20 ? 20 : sleep_ms);
  return;
#else
  companionBleModemOn();
  // CPU light sleep without modem sleep stalls the controller. Keep trying
  // modem sleep; do not latch a single early failure from setup.
  if (ble_modem_on && !ble_cpu_light && !ble_cpu_refused) {
    if (!board.setAutoLightSleep(true)) {
      ble_cpu_refused = true;
      Serial.println("BLE CPU light sleep unsupported, modem sleep only");
    } else {
      ble_cpu_light = true;
      companion_clock_locked = true;
    }
  }
  if (!ble_modem_on || !ble_cpu_light) {
    delay(sleep_ms > 20 ? 20 : sleep_ms);
    return;
  }
  // A connected phone still light-sleeps between its own events. Advertising
  // can sit longer. delay() does not return on a button press, so a tap while
  // the screen is off never reaches the UI. waitAutoLightSleep wakes on the
  // button and on DIO without calling esp_light_sleep_start (that would stop
  // Bluedroid).
  uint32_t cap = bluetooth_interface.isLinkUp() ? companionIdlePeriodMs() : 400;
  if (sleep_ms > cap) sleep_ms = cap;
  int slept = board.waitAutoLightSleep(sleep_ms);
  if (slept < 0) {
    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < sleep_ms) {
#if defined(PIN_USER_BTN)
      if (digitalRead(PIN_USER_BTN) == USER_BTN_PRESSED) break;
#endif
      uint32_t left = sleep_ms - (uint32_t)(millis() - start);
      if (left > 20) left = 20;
      if (left == 0) break;
      delay(left);
    }
  }
#if defined(DISPLAY_CLASS) && defined(PIN_USER_BTN)
  // The UI samples the pin once per loop. If we sleep again before it sees
  // the release, the click is lost and the screen stays dark.
  if (digitalRead(PIN_USER_BTN) == USER_BTN_PRESSED) {
    uint32_t held = millis();
    while (digitalRead(PIN_USER_BTN) == USER_BTN_PRESSED && (uint32_t)(millis() - held) < 1500) {
      ui_task.loop();
      delay(10);
    }
    ui_task.loop();
  }
#endif
#endif
}
#endif

#if defined(ESP32) && defined(BLE_PIN_CODE)
static bool wifi_cpu_light = false;

static void companionWifiCpuAwake() {
  if (!wifi_cpu_light) return;
  board.setAutoLightSleep(false);
  wifi_cpu_light = false;
}

// BLE is down while STA is up, but esp_light_sleep_start still misses
// beacons. Automatic light sleep lets the Wi-Fi driver wake for DTIM.
static void companionWifiLightSleep(uint32_t sleep_ms) {
  if (sleep_ms == 0) return;
  if (!wifi_cpu_light) {
    if (!board.setAutoLightSleep(true)) {
      delay(sleep_ms > 20 ? 20 : sleep_ms);
      return;
    }
    wifi_cpu_light = true;
    companion_clock_locked = true;
  }
  uint32_t cap = companionIdlePeriodMs();
  if (sleep_ms > cap) sleep_ms = cap;
  int slept = board.waitAutoLightSleep(sleep_ms);
  if (slept < 0) {
    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < sleep_ms) {
#if defined(PIN_USER_BTN)
      if (digitalRead(PIN_USER_BTN) == USER_BTN_PRESSED) break;
#endif
      uint32_t left = sleep_ms - (uint32_t)(millis() - start);
      if (left > 20) left = 20;
      if (left == 0) break;
      delay(left);
    }
  }
#if defined(DISPLAY_CLASS) && defined(PIN_USER_BTN)
  if (digitalRead(PIN_USER_BTN) == USER_BTN_PRESSED) {
    uint32_t held = millis();
    while (digitalRead(PIN_USER_BTN) == USER_BTN_PRESSED && (uint32_t)(millis() - held) < 1500) {
      ui_task.loop();
      delay(10);
    }
    ui_task.loop();
  }
#endif
}
#else
static void companionWifiCpuAwake() {}
static void companionWifiLightSleep(uint32_t) {}
#endif

#if defined(ESP32)
// Same rule as the repeater: a host on USB-Serial/JTAG loses the COM port
// if the CPU light-sleeps. A charger does not send SOF, so that still sleeps.
static bool usbHostAttached() {
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  static uint32_t attached_until = 0;
  if (HWCDC::isPlugged()) {
    attached_until = millis() + 2000;
    if (attached_until == 0) attached_until = 1;
    return true;
  }
  return attached_until != 0 && (int32_t)(millis() - attached_until) < 0;
#else
  return false;
#endif
}

static void usbKeepCpuAwake(bool awake) {
#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE && ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  static esp_pm_lock_handle_t lock = nullptr;
  static bool held = false;
  static bool unavailable = false;
  if (unavailable || awake == held) return;
  if (awake) {
    if (!lock && esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb-cli", &lock) != ESP_OK) {
      lock = nullptr;
      unavailable = true;
      return;
    }
    if (lock && esp_pm_lock_acquire(lock) == ESP_OK) held = true;
  } else if (lock && esp_pm_lock_release(lock) == ESP_OK) {
    held = false;
  }
#else
  (void)awake;
#endif
}
#endif

void loop() {
  the_mesh.loop();
  interface_manager.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif

  if (!the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#elif defined(ESP32)
    // Screen-off is the idle state. Stay awake while the panel is on, GPS is
    // holding the CPU, or a USB host is attached. A clean firmware saves
    // Eco max (CPU sleep, RX boost off); an existing prefs file keeps its mode.
    bool usb_up = usbHostAttached();
    usbKeepCpuAwake(usb_up);
#if defined(BLE_PIN_CODE)
    if (bluetooth_interface.isEnabled()) companionBleModemOn();
#endif
    bool screen_off = true;
#ifdef DISPLAY_CLASS
    screen_off = !ui_task.isDisplayOn();
    if (!screen_off) companionNoteLinkActivity();
#endif
    bool gps_hold = false;
#if ENV_INCLUDE_GPS == 1
    gps_hold = sensors.gpsHoldsCpu();
#endif
    if (screen_off && !usb_up && !gps_hold && board.canSelectMcuSleep()) {
      // AGC and the idle noise floor are a minute apart, and the external
      // watchdog is fed every eight minutes. Sleep until the soonest of
      // those, a clock pull, or the e-ink clock, and never longer than a
      // minute. Zero would disable the timer, so a due deadline skips sleep.
      // DIO and the user button still wake immediately.
      uint32_t sleep_ms = 60000;
      uint32_t mesh_ms = the_mesh.millisUntilIdleWork();
      if (mesh_ms < sleep_ms) sleep_ms = mesh_ms;
#ifdef DISPLAY_CLASS
      uint32_t ui_ms = ui_task.millisUntilIdleWake();
      if (ui_ms < sleep_ms) sleep_ms = ui_ms;
#endif
#ifdef HAS_EXTERNAL_WATCHDOG
      unsigned long wdt_ms = external_watchdog.getIntervalMs();
      if (wdt_ms < sleep_ms) sleep_ms = (uint32_t)wdt_ms;
#endif
#if defined(BLE_PIN_CODE)
      // esp_light_sleep_start() halts Bluedroid. While BLE is up, automatic
      // light sleep runs between controller events, connected or advertising.
      if (bluetooth_interface.isEnabled()) {
        if (sleep_ms > 0) companionBleLightSleep(sleep_ms);
      } else if ((WiFi.getMode() & WIFI_MODE_STA) != 0) {
        if (sleep_ms > 0) companionWifiLightSleep(sleep_ms);
      } else {
        companionWifiCpuAwake();
        companionBleLightStop();
        // BLE and Wi-Fi are both off. Wake at least every 350 ms so the
        // button is polled even if the GPIO wake is missed. A packet on DIO
        // still wakes immediately.
        if (sleep_ms > 350) sleep_ms = 350;
        if (sleep_ms > 0) board.idleSleep(sleep_ms);
      }
#else
      if (sleep_ms > 0) board.idleSleep(sleep_ms);
#endif
    } else {
#if defined(BLE_PIN_CODE)
      if (bluetooth_interface.isEnabled()) companionBleCpuAwake();
      else companionWifiCpuAwake();
#endif
      delay(usb_up ? 10 : 20);
    }
#endif
  }

#if defined(ESP32) && defined(BLE_PIN_CODE)
  NodePrefs* prefs = the_mesh.getNodePrefs();
  bool wifi_link = prefs && prefs->link_mode == LINK_MODE_WIFI && prefs->wifi_ssid[0];
  if (wifi_link && wifi_needs_reconnect && (millis() - last_wifi_reconnect_attempt > 10000)) {
    WIFI_DEBUG_PRINTLN("Attempting manual WiFi reconnect...");
    WiFi.disconnect();
    WiFi.reconnect();
    last_wifi_reconnect_attempt = millis();
  }
#endif
}
