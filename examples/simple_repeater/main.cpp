#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#if defined(ESP32) && defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
  #include <esp_pm.h>
#endif
#if defined(ESP32) && ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  #include <HWCDC.h>
#endif

#include "MyMesh.h"

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(board, display);
#endif

#ifdef ETHERNET_ENABLED
  #define ETHERNET_CLI_BANNER "MeshCore Repeater CLI"
  #include <helpers/nrf52/EthernetCLI.h>
#endif

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

void halt() {
  while (1) ;
}

static char command[160];
#ifdef ETHERNET_ENABLED
static char ethernet_command[160];
#endif

// For power saving
unsigned long POWERSAVING_FIRSTSLEEP_SECS = 120; // The first sleep (if enabled) from boot

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
static unsigned long userBtnDownAt = 0;
#define USER_BTN_HOLD_OFF_MILLIS 1500
#endif

#ifndef AUTO_SHUTDOWN_MILLIVOLTS
#define AUTO_SHUTDOWN_MILLIVOLTS 3000
#endif
// Readings at or below this are treated as a missing battery or a bad ADC
// (for example while powered from mains), not as a drained cell.
#ifndef AUTO_SHUTDOWN_MIN_MILLIVOLTS
#define AUTO_SHUTDOWN_MIN_MILLIVOLTS 500
#endif
static unsigned long next_batt_shutdown_check = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.begin();
#endif

#if defined(MESH_DEBUG) && defined(NRF52_PLATFORM)
  // give some extra time for serial to settle so
  // boot debug messages can be seen on terminal
  delay(5000);
#endif

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) {
    MESH_DEBUG_PRINTLN("Radio init failed!");
    halt();
  }

  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Repeater ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;
#ifdef ETHERNET_ENABLED
  ethernet_command[0] = 0;
#endif

  sensors.begin();

  the_mesh.begin(fs);

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

#ifdef ETHERNET_ENABLED
  ethernet_start_task();
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif

  board.onBootComplete();
}

#if defined(ESP32)
// Heltec V4 Serial is the USB-Serial/JTAG block. Light sleep stops its clock,
// Windows drops the COM port, and the browser's writer is gone before the
// first command. SOF means a host is on the bus; a charger with no data
// does not send SOF, so screen-off sleep still happens there.
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

// Automatic light sleep is entered from the idle task, not only from
// waitAutoLightSleep. This lock does not change the CPU clock.
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
#ifdef DISPLAY_CLASS
  ui_task.pollButton();
#endif
  // Handle Serial CLI
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
      Serial.print(c);
    }
    if (c == '\r') break;
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    Serial.print('\n');
    command[len - 1] = 0;  // replace newline with C string null terminator
    char reply[160];
    reply[0] = 0;
#ifdef ETHERNET_ENABLED
    if (!ethernet_handle_command(command, reply)) {
      the_mesh.handleCommand(0, command, reply);
    }
#else
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
#endif
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

#ifdef ETHERNET_ENABLED
  ethernet_loop_maintain();
  if (ethernet_read_line(ethernet_command, sizeof(ethernet_command))) {
    char reply[160];
    reply[0] = 0;
    if (!ethernet_handle_command(ethernet_command, reply)) {
      the_mesh.handleCommand(0, ethernet_command, reply);
    }
    ethernet_send_reply(reply);
    ethernet_command[0] = 0;
  }
#endif

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_) && !defined(DISPLAY_CLASS)
  // Hold the user button to power off the SenseCAP Solar repeater.
  int btnState = digitalRead(PIN_USER_BTN);
  if (btnState == LOW) {
    if (userBtnDownAt == 0) {
      userBtnDownAt = millis();
    } else if ((unsigned long)(millis() - userBtnDownAt) >= USER_BTN_HOLD_OFF_MILLIS) {
      Serial.println("Powering off...");
      board.powerOff();  // does not return
    }
  } else {
    userBtnDownAt = 0;
  }
#endif

  if ((long)(millis() - next_batt_shutdown_check) >= 0) {
    uint16_t milliVolts = board.getBattMilliVolts();
    next_batt_shutdown_check = millis() + 8000;
    if (milliVolts > AUTO_SHUTDOWN_MIN_MILLIVOLTS && milliVolts < AUTO_SHUTDOWN_MILLIVOLTS && !board.isExternalPowered()) {
      Serial.println("Low battery. Shutting down.");
#ifdef DISPLAY_CLASS
      display.turnOn();
      display.startFrame();
      display.setTextSize(1);
      display.setColor(UIColor::warning_txt);
      display.drawTextCentered(display.width() / 2, 20, "Low Battery.");
      display.drawTextCentered(display.width() / 2, 36, "Shutting Down!");
      display.endFrame();
      if (!display.isEink()) delay(3000);
#endif
      board.powerOff();
    }
  }

  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif
#if defined(ESP32)
  // Automatic light sleep runs while this task is blocked. LoRa stays in RX.
  // When DIO can wake the sleep, block until the next maintenance deadline.
  // Otherwise keep the short pause: a level check on the next pass catches
  // a packet the sleep missed. A host on native USB, or a CLI line already
  // started, must not enter that sleep: the port disappears mid-connect.
  bool usb_up = usbHostAttached();
  usbKeepCpuAwake(usb_up);
  bool cli_busy = command[0] != 0 || Serial.available();
  if (usb_up && !cli_busy) {
    delay(10);
  } else if (!usb_up && !cli_busy && the_mesh.wifiLightSleep() && !the_mesh.webBusy()) {
    uint32_t sleep_ms = 5000;
#ifdef DISPLAY_CLASS
    // The click is completed in the main loop, not inside the wait. A multi
    // second block swallows the release, so the screen change lands late.
    // While the panel is on it already dominates the current; keep the long
    // sleep for the screen-off idle.
    if (ui_task.keepsCpuAwake() && sleep_ms > 20) sleep_ms = 20;
#endif
    uint32_t radio_ms = the_mesh.millisUntilRadioWork();
    if (radio_ms < sleep_ms) sleep_ms = radio_ms;
#ifdef HAS_EXTERNAL_WATCHDOG
    unsigned long wdt_ms = external_watchdog.getIntervalMs();
    if (wdt_ms < sleep_ms) sleep_ms = (uint32_t)wdt_ms;
#endif
    int slept = sleep_ms > 0 ? board.waitAutoLightSleep(sleep_ms) : 1;
    if (slept < 0) {
      uint32_t irq = board.getIRQGpio();
      if (irq == (uint32_t)-1 || irq > 255 || digitalRead((uint8_t)irq) == LOW) delay(20);
    }
  } else if (!usb_up && !the_mesh.hasPendingWork() && command[0] == 0 && !Serial.available()
#ifdef DISPLAY_CLASS
             && !ui_task.keepsCpuAwake()
#endif
  ) {
    // Wi-Fi is down, so manual light sleep does not drop a station. LoRa stays
    // in RX. Wake on a packet, the button, or the next advert / noise / clock
    // deadline, and never stay down longer than a minute.
    uint32_t sleep_ms = 60000;
    uint32_t mesh_ms = the_mesh.millisUntilIdleWork();
    if (mesh_ms < sleep_ms) sleep_ms = mesh_ms;
#ifdef HAS_EXTERNAL_WATCHDOG
    unsigned long wdt_ms = external_watchdog.getIntervalMs();
    if (wdt_ms < sleep_ms) sleep_ms = (uint32_t)wdt_ms;
#endif
    if (sleep_ms > 0) board.idleSleep(sleep_ms);
  }
#endif
  if (the_mesh.getNodePrefs()->powersaving_enabled && !the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#elif !defined(ESP32)
    if (the_mesh.millisHasNowPassed(POWERSAVING_FIRSTSLEEP_SECS * 1000)) { // To check if it is time to sleep
      board.sleep(30); // Sleep. Wake up after a while or when receiving a LoRa packet
    }
#endif
  }
}
