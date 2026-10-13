#pragma once

#include <MeshCore.h>
#include <Arduino.h>

#ifndef USER_BTN_PRESSED
#define USER_BTN_PRESSED LOW
#endif

#if defined(ESP_PLATFORM)

#include <rom/rtc.h>
#include <sys/time.h>
#include <Wire.h>
#include "soc/rtc.h"
#include "esp_system.h"
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_timer.h>
#include <soc/gpio_reg.h>
#include <soc/gpio_struct.h>
#include <soc/soc_caps.h>
#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
#include <driver/temp_sensor.h>
#endif

class ESP32Board : public mesh::MainBoard {
protected:
  uint8_t startup_reason;
  bool inhibit_sleep = false;
  // GPIO0 has no pull while the pad is in its sleep config, so a low-level
  // wakeup on the user button returns at once and the CPU never stays down.
  static inline bool allowBtnWake = true;
  static inline portMUX_TYPE sleepMux = portMUX_INITIALIZER_UNLOCKED;

  // Register macros stay portable across classic/S3 (uint fields) and C3/C6
  // (typed W1TC structs / no status1 on C3).
  static void clearGpioInt(gpio_num_t pin) {
    if ((int)pin < 0) return;
    const int n = (int)pin;
    if (n < 32) {
      REG_WRITE(GPIO_STATUS_W1TC_REG, 1U << n);
    }
#if defined(GPIO_STATUS1_W1TC_REG)
    else {
      REG_WRITE(GPIO_STATUS1_W1TC_REG, 1U << (n - 32));
    }
#endif
  }

  // gpio_wakeup_enable hands an RTC pad to the RTC controller and can enable
  // the sleep mux. Both survive esp_restart(), digitalRead stays low, and the
  // button looks dead until the RESET pin clears the RTC domain.
  static void releaseRtcPad(int pin) {
    if (pin < 0 || pin >= GPIO_NUM_MAX) return;
    gpio_num_t gpio = (gpio_num_t)pin;
    gpio_sleep_sel_dis(gpio);
    gpio_hold_dis(gpio);
    gpio_wakeup_disable(gpio);
#if SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
    if (!rtc_gpio_is_valid_gpio(gpio)) return;
    rtc_gpio_hold_dis(gpio);
    rtc_gpio_wakeup_disable(gpio);
    rtc_gpio_deinit(gpio);
#endif
  }

  static void restoreUserButton() {
#if defined(PIN_USER_BTN)
    if (PIN_USER_BTN < 0) return;
    gpio_num_t button = (gpio_num_t)PIN_USER_BTN;
    releaseRtcPad(PIN_USER_BTN);
    gpio_reset_pin(button);
    gpio_set_direction(button, GPIO_MODE_INPUT);
#if (USER_BTN_PRESSED) == LOW
    gpio_set_pull_mode(button, GPIO_PULLUP_ONLY);
#else
    gpio_set_pull_mode(button, GPIO_PULLDOWN_ONLY);
#endif
    gpio_sleep_sel_dis(button);
#endif
  }

  // The Arduino GPIO trampoline is in flash. A packet edge during light sleep
  // used to run it before the cache was back and reboot the chip, so the
  // interrupt was masked and the CPU only woke for the noise-floor timer
  // (about 2 s, or 5 s). installSafeRadioIsr replaces that trampoline with
  // an IRAM handler. Light sleep still needs a level wake: the edge ISR does
  // not run while the CPU clock is gated.
  static inline bool radio_irq_held = false;
  static inline bool radio_isr_safe = false;
  static inline void (*radio_isr_hook)() = nullptr;

  static void IRAM_ATTR radioWakeIsr(void* arg);

  void holdRadioIrq(bool hold) {
    gpio_num_t pin = (gpio_num_t)getIRQGpio();
    if ((int)pin < 0 || (int)pin >= GPIO_NUM_MAX) return;
    if (hold && !radio_isr_safe) {
      gpio_intr_disable(pin);
      radio_irq_held = true;
      return;
    }
    if (hold) {
      radio_irq_held = true;
      gpio_sleep_sel_dis(pin);
      gpio_set_intr_type(pin, GPIO_INTR_POSEDGE);
      gpio_intr_enable(pin);
      return;
    }
    radio_irq_held = false;
    gpio_set_intr_type(pin, GPIO_INTR_POSEDGE);
    gpio_intr_enable(pin);
  }

  void restoreRadioIrq() {
    if (radio_irq_held) return;
    holdRadioIrq(false);
  }

public:
  void begin() {
    // for future use, sub-classes SHOULD call this from their begin()
    startup_reason = BD_STARTUP_NORMAL;
    // A crash during light sleep leaves the RTC wake armed. Soft reset does
    // not clear it, so the next boot wakes at once and looks like a reboot loop.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    releaseRtcPad((int)getIRQGpio());
    restoreUserButton();    

  #ifdef ESP32_CPU_FREQ
    setCpuFrequencyMhz(ESP32_CPU_FREQ);
  #endif

  #ifdef PIN_VBAT_READ
    // battery read support
    pinMode(PIN_VBAT_READ, INPUT);
    adcAttachPin(PIN_VBAT_READ);
  #endif

  #ifdef P_LORA_TX_LED
    pinMode(P_LORA_TX_LED, OUTPUT);
    digitalWrite(P_LORA_TX_LED, LOW);
  #endif

  #if defined(PIN_BOARD_SDA) && defined(PIN_BOARD_SCL)
   #if PIN_BOARD_SDA >= 0 && PIN_BOARD_SCL >= 0
    Wire.begin(PIN_BOARD_SDA, PIN_BOARD_SCL);
   #endif
  #else
    Wire.begin();
  #endif    
  }

  // Chip temperature in °C. Averaged as floats: folding temperatureRead() into
  // uint32_t turned NaN into 0 and divided a real reading by about four.
  float getMCUTemperature() override {
#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
    static bool started = false;
    if (!started) {
      temp_sensor_config_t config = {};
      config.dac_offset = TSENS_DAC_L2;
      config.clk_div = 6;
      if (temp_sensor_set_config(config) != ESP_OK) return NAN;
      esp_err_t err = temp_sensor_start();
      if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return NAN;
      started = true;
    }
    float sum = 0;
    int count = 0;
    for (int i = 0; i < 4; i++) {
      float sample = NAN;
      if (temp_sensor_read_celsius(&sample) != ESP_OK || isnan(sample)) continue;
      sum += sample;
      count++;
    }
    return count ? sum / (float)count : NAN;
#else
    float sum = 0;
    int count = 0;
    for (int i = 0; i < 4; i++) {
      float sample = temperatureRead();
      if (isnan(sample)) continue;
      sum += sample;
      count++;
    }
    return count ? sum / (float)count : NAN;
#endif
  }

  virtual void powerOff() override;
  void enterDeepSleep(uint32_t secs);

  uint32_t getIRQGpio() override {
    return P_LORA_DIO_1; // default for SX1262
  }

  bool canSelectMcuSleep() const override { return !inhibit_sleep; }

  // Automatic light sleep (esp_pm), used while a manual esp_light_sleep_start()
  // would drop Bluedroid. min frequency is the XTAL floor light sleep requires.
  // Light sleep is accepted only when that minimum equals the crystal register.
  // After a 240 MHz boot the register often holds the CPU clock. 32 MHz is a
  // real crystal; any other reading on these boards is a 40 MHz crystal.
  static uint32_t lightSleepXtalMhz() {
    uint32_t xtal = getXtalFrequencyMhz();
    if (xtal == 32 || xtal == 40) return xtal;
    rtc_clk_xtal_freq_update(RTC_XTAL_FREQ_40M);
    return 40;
  }

  bool setAutoLightSleep(bool enable);

  // Block until timeout, or until LoRa DIO / the user button wakes the CPU.
  //  0  waited
  //  1  DIO is already high; the caller should service the radio
  // -1  this pin cannot wake light sleep
  int waitAutoLightSleep(uint32_t timeout_ms);

  // Called from the LoRa DIO ISR. Wakes waitAutoLightSleep and stops a level storm.
  static void IRAM_ATTR noteRadioIrqFromIsr();
  // Replace Arduino's flash GPIO trampoline on the LoRa DIO pin. hook must be IRAM.
  static void installSafeRadioIsr(void (*hook)(), uint32_t pin);
  // Light sleep does not run a GPIO edge ISR: the CPU clock is gated. A high
  // level on DIO is what wakes it. The ISR, if installed, is one-shot so a
  // held line cannot storm, then the edge handler is restored.
  bool armRadioLightWake();
  void disarmRadioLightWake();

  void idleSleep(uint32_t timeout_ms) override {
    if (inhibit_sleep) {
      delay(1); // Give MCU to OTA or WiFi to run
      return;
    }

    gpio_num_t wakeupPin = (gpio_num_t)getIRQGpio();
    if ((int)wakeupPin < 0) return;

    // Skip sleep if a LoRa packet is already waiting. The main loop's level
    // poll calls setFlag(); the edge interrupt would already have missed it.
    if (gpio_get_level(wakeupPin) == HIGH) {
      delay(1);
      return;
    }

    // The edge ISR stays for while the CPU is awake. Light sleep itself only
    // wakes on the level source armed here; otherwise a packet waits for the
    // noise-floor timer.
    if (!armRadioLightWake() || esp_sleep_enable_gpio_wakeup() != ESP_OK) {
      disarmRadioLightWake();
      restoreRadioIrq();
      delay(1);
      return;
    }

    if (timeout_ms > 0) {
      esp_sleep_enable_timer_wakeup((uint64_t)timeout_ms * 1000ULL);
    }

#if defined(PIN_USER_BTN)
    bool btn_wake = false;
    gpio_num_t button = (gpio_num_t)PIN_USER_BTN;
    int pressed = ((USER_BTN_PRESSED) == LOW) ? 0 : 1;
    // Do not switch this pad to its sleep config. On GPIO0 that reads low
    // and the low-level wakeup returns before the CPU has stayed asleep.
    if (allowBtnWake && PIN_USER_BTN >= 0 && (int)button != (int)wakeupPin &&
        gpio_get_level(button) != pressed &&
        gpio_wakeup_enable(button,
            (USER_BTN_PRESSED) == LOW ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL) == ESP_OK) {
      btn_wake = true;
      clearGpioInt(button);
    }
#else
    gpio_num_t button = GPIO_NUM_NC;
    bool btn_wake = false;
    int pressed = 0;
#endif

    // A packet that arrived while wakeup was being armed must not be slept through.
    if (gpio_get_level(wakeupPin) == HIGH) {
      if (btn_wake) restoreUserButton();
      disarmRadioLightWake();
      restoreRadioIrq();
      delay(1);
      return;
    }

    int64_t t0 = esp_timer_get_time();
    esp_light_sleep_start();
    int64_t asleep = esp_timer_get_time() - t0;
    bool dio_high = gpio_get_level(wakeupPin) == HIGH;
#if defined(PIN_USER_BTN)
    bool btn_down = PIN_USER_BTN >= 0 && gpio_get_level(button) == pressed;
#else
    bool btn_down = false;
#endif

    disarmRadioLightWake();
    if (btn_wake) restoreUserButton();
    restoreRadioIrq();

    if (btn_wake && !dio_high && !btn_down && asleep < 2000 &&
        esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
      allowBtnWake = false;
    }
  }

  void sleep(uint32_t secs) override {
    idleSleep(secs > 0 ? secs * 1000UL : 0);
  }

  uint8_t getStartupReason() const override { return startup_reason; }

#if defined(P_LORA_TX_LED)
  void onBeforeTransmit() override {
    if (!isTxLedEnabled()) return;
    digitalWrite(P_LORA_TX_LED, HIGH);   // turn TX LED on
  }
  void onAfterTransmit() override {
    digitalWrite(P_LORA_TX_LED, LOW);   // turn TX LED off
  }
#elif defined(P_LORA_TX_NEOPIXEL_LED)
  #define NEOPIXEL_BRIGHTNESS    64  // white brightness (max 255)

  void onBeforeTransmit() override {
    if (!isTxLedEnabled()) return;
    neopixelWrite(P_LORA_TX_NEOPIXEL_LED, NEOPIXEL_BRIGHTNESS, NEOPIXEL_BRIGHTNESS, NEOPIXEL_BRIGHTNESS);   // turn TX neopixel on (White)
  }
  void onAfterTransmit() override {
    neopixelWrite(P_LORA_TX_NEOPIXEL_LED, 0, 0, 0);   // turn TX neopixel off
  }
#endif

  uint16_t getBattMilliVolts() override {
    #ifdef PIN_VBAT_READ
    analogReadResolution(12);

    uint32_t raw = 0;
    for (int i = 0; i < 4; i++) {
      raw += analogReadMilliVolts(PIN_VBAT_READ);
    }
    raw = raw / 4;

    return (2 * raw);
  #else
    return 0;  // not supported
  #endif
  }

  const char* getManufacturerName() const override {
    return "Generic ESP32";
  }

  void reboot() override {
    esp_restart();
  }

  bool startOTAUpdate(const char* id, char reply[]) override;

  void setInhibitSleep(bool inhibit) {
    inhibit_sleep = inhibit;
  }

  uint32_t getResetReason() const override {
    return esp_reset_reason();
  }

  // https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32/api-reference/system/system.html
  const char* getResetReasonString(uint32_t reason) {
    switch (reason) {
      case ESP_RST_UNKNOWN:
        return "Unknown or first boot";
      case ESP_RST_POWERON:
        return "Power-on reset";
      case ESP_RST_EXT:
        return "External reset";
      case ESP_RST_SW:
        return "Software reset";
      case ESP_RST_PANIC:
        return "Panic / exception reset";
      case ESP_RST_INT_WDT:
        return "Interrupt watchdog reset";
      case ESP_RST_TASK_WDT:
        return "Task watchdog reset";
      case ESP_RST_WDT:
        return "Other watchdog reset";
      case ESP_RST_DEEPSLEEP:
        return "Wake from deep sleep";
      case ESP_RST_BROWNOUT:
        return "Brownout (low voltage)";
      case ESP_RST_SDIO:
        return "SDIO reset";
      default:
        static char buf[40];
        snprintf(buf, sizeof(buf), "Unknown reset reason (%d)", reason);
        return buf;
    }
  }
};

class ESP32RTCClock : public mesh::RTCClock {
public:
  ESP32RTCClock() { }
  void begin() {
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON) {
      // start with some date/time in the recent past
      struct timeval tv;
      tv.tv_sec = 1715770351;  // 15 May 2024, 8:50pm
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
  }
  }
  uint32_t getCurrentTime() override {
    time_t _now;
    time(&_now);
    return _now;
  }
  void setCurrentTime(uint32_t time) override {
    struct timeval tv;
    tv.tv_sec = time;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
  }
};

#endif
