#ifdef ESP_PLATFORM

#include "ESP32Board.h"
#include <target.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#include "hal/usb_serial_jtag_ll.h"
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT && defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE
#include <HWCDC.h>
#endif
#endif

static volatile TaskHandle_t s_wait_task = nullptr;
static gpio_num_t s_radio_pin = GPIO_NUM_NC;
// Set only while DIO is armed as a level wake. The ISR clears the CPU
// interrupt once so a line that stays high cannot spin.
static volatile bool s_level_arm = false;

static bool idlePinOk(gpio_num_t pin) {
  return (int)pin >= 0 && (int)pin < GPIO_NUM_MAX;
}

// C3 keeps a USB-Serial/JTAG interrupt armed (secondary console, and HWCDC on
// Xiao). A pending bit rejects light sleep at once. Mask it only while no
// host is on the bus; a host must keep the COM port.
struct C3UsbIdleMask {
  uint32_t ena;
  bool held;
  C3UsbIdleMask() : ena(0), held(false) {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT && defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE
    if (HWCDC::isPlugged()) return;
#endif
    ena = usb_serial_jtag_ll_get_intr_ena_status();
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    held = true;
#endif
  }
  ~C3UsbIdleMask() {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    if (!held) return;
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    if (ena) usb_serial_jtag_ll_ena_intr_mask(ena);
#endif
  }
  void clear() {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    if (held) usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
#endif
  }
};

void IRAM_ATTR ESP32Board::noteRadioIrqFromIsr() {
  TaskHandle_t task = s_wait_task;
  if (!task) return;
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(task, &woken);
  if (woken) portYIELD_FROM_ISR();
}

void IRAM_ATTR ESP32Board::radioWakeIsr(void* arg) {
  (void)arg;
  if (s_level_arm) {
    s_level_arm = false;
    if ((int)s_radio_pin >= 0 && (int)s_radio_pin < GPIO_NUM_MAX) GPIO.pin[s_radio_pin].int_ena = 0;
  }
  if (radio_isr_hook) radio_isr_hook();
}

void ESP32Board::installSafeRadioIsr(void (*hook)(), uint32_t pin) {
  radio_isr_hook = hook;
  if ((int)pin < 0 || (int)pin >= GPIO_NUM_MAX) return;
  s_radio_pin = (gpio_num_t)pin;
  // Keep the pad on its active input during light sleep, or the level never arrives.
  gpio_sleep_sel_dis(s_radio_pin);
  // attachInterrupt already registered the flash trampoline. A second add
  // fails, the edge stays in flash, and a wake from light sleep reboots.
  gpio_isr_handler_remove(s_radio_pin);
  if (gpio_isr_handler_add(s_radio_pin, radioWakeIsr, nullptr) == ESP_OK) radio_isr_safe = true;
}

bool ESP32Board::armRadioLightWake() {
  gpio_num_t dio = (gpio_num_t)getIRQGpio();
  if (!idlePinOk(dio)) return false;
  s_radio_pin = dio;
  gpio_sleep_sel_dis(dio);
  // gpio_wakeup_enable is what light sleep actually watches. A rising-edge
  // interrupt is not delivered until the CPU is already awake, which is why
  // a packet waited for the noise-floor timer.
  if (gpio_wakeup_enable(dio, GPIO_INTR_HIGH_LEVEL) != ESP_OK) return false;
  clearGpioInt(dio);
  portENTER_CRITICAL(&sleepMux);
  if (radio_isr_safe) {
    s_level_arm = true;
    gpio_intr_enable(dio);
  } else {
    s_level_arm = false;
    gpio_intr_disable(dio);
  }
  portEXIT_CRITICAL(&sleepMux);
  return true;
}

void ESP32Board::disarmRadioLightWake() {
  // Interrupts off so the one-shot ISR cannot clear int_ena after we restore
  // the edge handler.
  portENTER_CRITICAL(&sleepMux);
  s_level_arm = false;
  gpio_num_t dio = s_radio_pin;
  if (idlePinOk(dio)) {
    gpio_wakeup_disable(dio);
    // gpio_wakeup_disable on C3 enables the sleep mux. Leave the pad on its
    // active input or the sleep pull reads high and the next wake is rejected.
    gpio_sleep_sel_dis(dio);
    if (radio_isr_safe) {
      gpio_set_intr_type(dio, GPIO_INTR_POSEDGE);
      gpio_intr_enable(dio);
    } else {
      gpio_intr_disable(dio);
    }
  }
  portEXIT_CRITICAL(&sleepMux);
}

int64_t ESP32Board::runLightSleep(uint32_t timeout_ms, gpio_num_t dio) {
  int64_t t0 = esp_timer_get_time();
  C3UsbIdleMask usb;
  esp_err_t err = esp_light_sleep_start();
  // A reject with DIO still low is a false wake (USB bit or the C3 sleep
  // pull). One more try after the mask; then drop GPIO wake and finish on
  // the timer so the main loop does not spin at run current.
  if (err == ESP_ERR_SLEEP_REJECT && gpio_get_level(dio) != HIGH) {
    usb.clear();
    err = esp_light_sleep_start();
  }
  if (err == ESP_ERR_SLEEP_REJECT && gpio_get_level(dio) != HIGH && timeout_ms > 0) {
    int64_t used = esp_timer_get_time() - t0;
    uint64_t budget = (uint64_t)timeout_ms * 1000ULL;
    if (used >= 0 && (uint64_t)used < budget) {
      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
      gpio_wakeup_disable(dio);
      gpio_sleep_sel_dis(dio);
      esp_sleep_enable_timer_wakeup(budget - (uint64_t)used);
      esp_light_sleep_start();
    }
  }
  return esp_timer_get_time() - t0;
}

bool ESP32Board::setAutoLightSleep(bool enable) {
  uint32_t mhz = getCpuFrequencyMhz();
  if (mhz < 80) mhz = 80;
  uint32_t xtal = enable ? lightSleepXtalMhz() : mhz;
#if defined(CONFIG_IDF_TARGET_ESP32)
  esp_pm_config_esp32_t cfg = {};
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
  esp_pm_config_esp32s2_t cfg = {};
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  esp_pm_config_esp32s3_t cfg = {};
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  esp_pm_config_esp32c3_t cfg = {};
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  esp_pm_config_esp32c6_t cfg = {};
#elif defined(CONFIG_IDF_TARGET_ESP32H2)
  esp_pm_config_esp32h2_t cfg = {};
#else
  (void)enable;
  return false;
#endif
#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || \
    defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
  cfg.max_freq_mhz = (int)mhz;
  cfg.min_freq_mhz = enable ? (int)xtal : (int)mhz;
  cfg.light_sleep_enable = enable;
  // Mask before PM can sleep. The Arduino GPIO trampoline is in flash, and
  // promoting that service to IRAM makes the same interrupt run while the
  // cache is off — the companion reboots as soon as it tries to sleep.
  // Packets are recovered by the level poll. The wait below only checks
  // the pins on the existing 50 ms rhythm.
  if (enable) holdRadioIrq(true);
  esp_err_t err = esp_pm_configure(&cfg);
  if (enable && err == ESP_ERR_INVALID_ARG && cfg.min_freq_mhz < 80) {
    cfg.min_freq_mhz = 80;
    err = esp_pm_configure(&cfg);
  }
  if ((enable && err != ESP_OK) || (!enable && err == ESP_OK)) holdRadioIrq(false);
  return err == ESP_OK;
#else
  return false;
#endif
}

int ESP32Board::waitAutoLightSleep(uint32_t timeout_ms) {
  gpio_num_t dio = (gpio_num_t)getIRQGpio();
  if (!idlePinOk(dio)) return -1;
  if (gpio_get_level(dio) == HIGH) return 1;
  // Same level wake as manual sleep. The edge handler does not run while
  // automatic light sleep has the CPU clock gated.
  if (!armRadioLightWake()) return -1;

#if defined(PIN_USER_BTN)
  gpio_num_t button = (gpio_num_t)PIN_USER_BTN;
  bool watch_btn = idlePinOk(button) && button != dio;
  bool btn_wake = false;
  if (watch_btn && allowBtnWake) {
    // Leave the pad on its active config. Switching GPIO0 to the sleep
    // pull reads low, the low-level wakeup fires at once, and the CPU
    // stays at full run current.
    gpio_sleep_sel_dis(button);
    int pressed = ((USER_BTN_PRESSED) == LOW) ? 0 : 1;
    gpio_int_type_t level = ((USER_BTN_PRESSED) == LOW) ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL;
    if (gpio_get_level(button) != pressed && gpio_wakeup_enable(button, level) == ESP_OK) {
      btn_wake = true;
      clearGpioInt(button);
    }
  }
#endif
  if (esp_sleep_enable_gpio_wakeup() != ESP_OK) {
    disarmRadioLightWake();
#if defined(PIN_USER_BTN)
    if (btn_wake) restoreUserButton();
#endif
    return -1;
  }

  // Tickless idle sleeps inside the wait. Same pad and USB rules as manual
  // sleep, or C3 rejects every few milliseconds and the CPU never stays down.
  gpio_sleep_sel_dis(dio);
  C3UsbIdleMask usb;
  (void)usb;

  int rc = 0;
  s_wait_task = xTaskGetCurrentTaskHandle();
  if (gpio_get_level(dio) == HIGH) rc = 1;
  else if (timeout_ms > 0) {
    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < timeout_ms) {
      if (gpio_get_level(dio) == HIGH) {
        rc = 1;
        break;
      }
#if defined(PIN_USER_BTN)
      if (watch_btn && digitalRead((uint8_t)button) == USER_BTN_PRESSED) break;
#endif
      uint32_t left = timeout_ms - (uint32_t)(millis() - start);
      if (left > 50) left = 50;
      if (left == 0) break;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(left));
    }
  }
  s_wait_task = nullptr;

  disarmRadioLightWake();
#if defined(PIN_USER_BTN)
  if (btn_wake) restoreUserButton();
#endif
  restoreRadioIrq();
  return rc;
}

#if defined(ADMIN_PASSWORD) && !defined(DISABLE_WIFI_OTA)   // Repeater or Room Server only
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>

#include <SPIFFS.h>

extern "C" void repeaterWebStop() __attribute__((weak));

bool ESP32Board::startOTAUpdate(const char* id, char reply[]) {
  inhibit_sleep = true;   // prevent sleep during OTA
  WiFi.softAP("MeshCore-OTA", NULL);

  sprintf(reply, "Started: http://%s/update", WiFi.softAPIP().toString().c_str());
  MESH_DEBUG_PRINTLN("startOTAUpdate: %s", reply);

  static char id_buf[60];
  sprintf(id_buf, "%s (%s)", id, getManufacturerName());
  static char home_buf[90];
  sprintf(home_buf, "<H2>Hi! I am a MeshCore Repeater. ID: %s</H2>", id);

  if (repeaterWebStop) repeaterWebStop();

  AsyncWebServer* server = new AsyncWebServer(80);

  server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", home_buf);
  });
  server->on("/log", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/packet_log", "text/plain");
  });

  AsyncElegantOTA.setID(id_buf);
  AsyncElegantOTA.begin(server);    // Start ElegantOTA
  server->begin();

  return true;
}

#else
bool ESP32Board::startOTAUpdate(const char* id, char reply[]) {
  return false; // not supported
}
#endif

void ESP32Board::powerOff() {
  enterDeepSleep(0); // Do not wakeup
}

void ESP32Board::enterDeepSleep(uint32_t secs) {
  // Power off the display if any
#ifdef DISPLAY_CLASS
  display.turnOff();
#endif

  // Power off LoRa
  radio_driver.powerOff();

  // Keep LoRa inactive during deepsleep
  digitalWrite(P_LORA_NSS, HIGH);
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
  gpio_hold_en((gpio_num_t)P_LORA_NSS);
#else
  rtc_gpio_hold_en((gpio_num_t)P_LORA_NSS);
#endif

  // Power off GPS if any
  if (sensors.getLocationProvider() != NULL) {
    sensors.getLocationProvider()->stop();
  }

  // Flush serial buffers
  Serial.flush();
  delay(100);

  // Clear stale wakeup sources to avoid ghost wakeup
  // This is required when Power Management and automatic lightsleep are enabled
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  if (secs > 0) {
    esp_sleep_enable_timer_wakeup(secs * 1000000ULL);
  }

  // Finally set ESP32 into deepsleep
  esp_deep_sleep_start(); // CPU halts here and never returns!
}
#endif
