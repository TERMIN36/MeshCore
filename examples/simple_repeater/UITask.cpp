#include "UITask.h"
#include "target.h"
#include "MyMesh.h"
#include <helpers/RadioProfiles.h>
#include <Arduino.h>
#include <helpers/CommonCLI.h>
#include <helpers/ui/BatteryLevel.h>

extern MyMesh the_mesh;

#ifndef USER_BTN_PRESSED
#define USER_BTN_PRESSED LOW
#endif

#define AUTO_OFF_MILLIS      20000  // 20 seconds
#define BOOT_SCREEN_MILLIS   4000   // 4 seconds

#define POWEROFF_DELAY 3000

// 'meshcore', 128x13px
static const uint8_t meshcore_logo [] PROGMEM = {
    0x3c, 0x01, 0xe3, 0xff, 0xc7, 0xff, 0x8f, 0x03, 0x87, 0xfe, 0x1f, 0xfe, 0x1f, 0xfe, 0x1f, 0xfe, 
    0x3c, 0x03, 0xe3, 0xff, 0xc7, 0xff, 0x8e, 0x03, 0x8f, 0xfe, 0x3f, 0xfe, 0x1f, 0xff, 0x1f, 0xfe, 
    0x3e, 0x03, 0xc3, 0xff, 0x8f, 0xff, 0x0e, 0x07, 0x8f, 0xfe, 0x7f, 0xfe, 0x1f, 0xff, 0x1f, 0xfc, 
    0x3e, 0x07, 0xc7, 0x80, 0x0e, 0x00, 0x0e, 0x07, 0x9e, 0x00, 0x78, 0x0e, 0x3c, 0x0f, 0x1c, 0x00, 
    0x3e, 0x0f, 0xc7, 0x80, 0x1e, 0x00, 0x0e, 0x07, 0x1e, 0x00, 0x70, 0x0e, 0x38, 0x0f, 0x3c, 0x00, 
    0x7f, 0x0f, 0xc7, 0xfe, 0x1f, 0xfc, 0x1f, 0xff, 0x1c, 0x00, 0x70, 0x0e, 0x38, 0x0e, 0x3f, 0xf8, 
    0x7f, 0x1f, 0xc7, 0xfe, 0x0f, 0xff, 0x1f, 0xff, 0x1c, 0x00, 0xf0, 0x0e, 0x38, 0x0e, 0x3f, 0xf8, 
    0x7f, 0x3f, 0xc7, 0xfe, 0x0f, 0xff, 0x1f, 0xff, 0x1c, 0x00, 0xf0, 0x1e, 0x3f, 0xfe, 0x3f, 0xf0, 
    0x77, 0x3b, 0x87, 0x00, 0x00, 0x07, 0x1c, 0x0f, 0x3c, 0x00, 0xe0, 0x1c, 0x7f, 0xfc, 0x38, 0x00, 
    0x77, 0xfb, 0x8f, 0x00, 0x00, 0x07, 0x1c, 0x0f, 0x3c, 0x00, 0xe0, 0x1c, 0x7f, 0xf8, 0x38, 0x00, 
    0x73, 0xf3, 0x8f, 0xff, 0x0f, 0xff, 0x1c, 0x0e, 0x3f, 0xf8, 0xff, 0xfc, 0x70, 0x78, 0x7f, 0xf8, 
    0xe3, 0xe3, 0x8f, 0xff, 0x1f, 0xfe, 0x3c, 0x0e, 0x3f, 0xf8, 0xff, 0xfc, 0x70, 0x3c, 0x7f, 0xf8, 
    0xe3, 0xe3, 0x8f, 0xff, 0x1f, 0xfc, 0x3c, 0x0e, 0x1f, 0xf8, 0xff, 0xf8, 0x70, 0x3c, 0x7f, 0xf8, 
};

void UITask::begin(NodePrefs* node_prefs, const char* build_date, const char* firmware_version) {
  _prevBtnState = HIGH;
  _auto_off = millis() + AUTO_OFF_MILLIS;
  _started_at = millis();
  _node_prefs = node_prefs;
  _display->turnOn();

#if defined(PIN_USER_BTN) && defined(DISPLAY_CLASS)
  user_btn.begin();
#endif

  // "v1.17.1-0.1.1 (date)" is wider than a 128px screen, so the splash shows the version only
  firmwareVersionNoHash(_version_info, sizeof(_version_info), firmware_version);
}

void UITask::renderBatteryIndicator() {
#ifndef BATT_MIN_MILLIVOLTS
#define BATT_MIN_MILLIVOLTS 3000
#endif
#ifndef BATT_MAX_MILLIVOLTS
#define BATT_MAX_MILLIVOLTS 4200
#endif
  uint16_t batteryMilliVolts = _board->getBattMilliVolts();
  static BatteryLevelFilter batt_filter;
  int batteryPercentage = batt_filter.push(batteryMilliVolts, BATT_MIN_MILLIVOLTS, BATT_MAX_MILLIVOLTS)
                          * 100 / BatteryLevelFilter::LEVELS;

  int iconWidth = 24;
  int iconHeight = 10;
  int iconX = _display->width() - iconWidth - 5;
  int iconY = 0;
  _display->setColor(UIColor::primary_txt);
  _display->drawRect(iconX, iconY, iconWidth, iconHeight);
  _display->fillRect(iconX + iconWidth, iconY + (iconHeight / 4), 3, iconHeight / 2);
  int fillWidth = (batteryPercentage * (iconWidth - 4)) / 100;
  if (fillWidth > 0) {
    _display->fillRect(iconX + 2, iconY + 2, fillWidth, iconHeight - 4);
  }
}

void UITask::renderCurrScreen() {
  if (millis() < _started_at + BOOT_SCREEN_MILLIS) { // boot screen
    // meshcore logo
    _display->setColor(UIColor::corp_blue);
    int logoWidth = 128;
    _display->drawXbm((_display->width() - logoWidth) / 2, 3, meshcore_logo, logoWidth, 13);

    // meshcore website
    const char* website = "https://meshcore.io";
    _display->setColor(UIColor::primary_txt);
    _display->setTextSize(1);
    _display->drawTextCentered(_display->width() / 2, 22, website);

    // version info
    _display->setTextSize(1);
    _display->drawTextCentered(_display->width() / 2, 35, _version_info);

    // node type
    const char* node_type = "Repeater by Termin36";
    _display->drawTextCentered(_display->width() / 2, 48, node_type);
  } else if (_powering_off_at > 0) {
    // meshcore logo
    _display->setColor(UIColor::corp_blue);
    int logoWidth = 128;
    _display->drawXbm((_display->width() - logoWidth) / 2, 3, meshcore_logo, logoWidth, 13);

    // meshcore website
    const char* website = "https://meshcore.io";
    _display->setColor(UIColor::primary_txt);
    _display->setTextSize(1);
    _display->drawTextCentered(_display->width()/ 2, 22, website);

    // Powering off
    const char* poweroff_string = "Turning OFF";
    uint16_t poffWidth = _display->getTextWidth(poweroff_string);
    _display->setCursor((_display->width() - poffWidth) / 2, 48);
    _display->drawTextCentered(_display->width()/2, 48, poweroff_string);
  } else if (_page == 1) {
    renderRadioScreen();
  } else if (_page == 2) {
    renderWifiScreen();
  } else {
    renderStatusScreen();
  }
}

void UITask::renderStatusScreen() {
  char tmp[80];
  _display->setCursor(0, 0);
  _display->setTextSize(1);
  _display->setColor(UIColor::primary_txt);
  _display->print(_node_prefs->node_name);
  renderBatteryIndicator();

  // freq / sf
  _display->setCursor(0, 20);
  sprintf(tmp, "FREQ: %06.3f SF%d", _node_prefs->freq, _node_prefs->sf);
  _display->print(tmp);

  // bw / cr
  _display->setCursor(0, 30);
  sprintf(tmp, "BW: %03.2f CR: %d", _node_prefs->bw, _node_prefs->cr);
  _display->print(tmp);

  _display->setCursor(0, 40);
  if (_board->canControlLoRaFemLna()) {
    sprintf(tmp, "LNA: %s  NF: %d", _board->isLoRaFemLnaEnabled() ? "on" : "off",
            radio_driver.getNoiseFloor());
  } else {
    sprintf(tmp, "LNA: n/a  NF: %d", radio_driver.getNoiseFloor());
  }
  _display->print(tmp);
  _display->setCursor(0, 52);
  if (_node_prefs->mqtt_enabled) {
    _display->print(the_mesh.mqttBridgeUp() ? "MQTT: up" : "MQTT: down");
  } else if (_board->canControlLoRaFemLna()) {
    _display->print("1x Radio  3x LNA");
  } else {
    _display->print("click: Radio");
  }
}

void UITask::renderRadioScreen() {
  char tmp[80];
  _display->setCursor(0, 0);
  _display->setTextSize(1);
  _display->setColor(UIColor::primary_txt);
  _display->print("Radio");
  renderBatteryIndicator();

  _display->setCursor(0, 16);
  _display->print(radioProfileLabel(the_mesh.radioProfile()));

  _display->setCursor(0, 28);
  sprintf(tmp, "FQ %06.3f SF%d", _node_prefs->freq, _node_prefs->sf);
  _display->print(tmp);

  _display->setCursor(0, 40);
  sprintf(tmp, "BW %03.2f CR%d", _node_prefs->bw, _node_prefs->cr);
  _display->print(tmp);

  _display->setCursor(0, 52);
  if (_board->canControlLoRaFemLna()) _display->print("3x profile  hold LNA");
  else _display->print("3x: profile");
}

void UITask::renderWifiScreen() {
  _display->setCursor(0, 0);
  _display->setTextSize(1);
  _display->setColor(UIColor::primary_txt);
  _display->print("WiFi");
  renderBatteryIndicator();

  _display->setCursor(0, 16);
  _display->print(_node_prefs->wifi_enabled ? "state: on" : "state: off");

  _display->setCursor(0, 28);
  if (_node_prefs->wifi_ssid[0]) {
    char ssid[33];
    strncpy(ssid, _node_prefs->wifi_ssid, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = 0;
    while (ssid[0] && _display->getTextWidth(ssid) > _display->width()) {
      ssid[strlen(ssid) - 1] = 0;
    }
    _display->print(ssid);
  } else {
    _display->print("ssid: -");
  }

  if (_node_prefs->wifi_enabled) {
    char ip[16];
    _display->setCursor(0, 40);
    if (the_mesh.copyWifiAddress(ip, sizeof(ip))) _display->print(ip);
    else _display->print("...");
  }

  _display->setCursor(0, 52);
  _display->print("3x: on/off");
}

void UITask::showNextPage() {
  if (!_display->isOn()) _display->turnOn();
  _auto_off = millis() + AUTO_OFF_MILLIS;
  // Splash and the power-off frame own the panel until they finish.
  if (_powering_off_at == 0 && millis() >= _started_at + BOOT_SCREEN_MILLIS) {
    _page = (uint8_t)((_page + 1) % 3);
  }
  _next_refresh = 0;
}

void UITask::pollButton() {
#if defined(PIN_USER_BTN) && defined(DISPLAY_CLASS)
  int ev = user_btn.check();
  // A short press is a click. A bouncy press is often reported as a double
  // click, and that used to be ignored, so the WiFi page never appeared.
  // E-ink stays visually on after auto-off, so a click must change the page
  // even when the driver already considers the panel off.
  if (ev == BUTTON_EVENT_CLICK || ev == BUTTON_EVENT_DOUBLE_CLICK) {
    showNextPage();
  } else if (ev == BUTTON_EVENT_TRIPLE_CLICK && _powering_off_at == 0) {
    _display->turnOn();
    _auto_off = millis() + AUTO_OFF_MILLIS;
    if (_page == 2) {
      _node_prefs->wifi_enabled = _node_prefs->wifi_enabled ? 0 : 1;
      the_mesh.savePrefs();
      the_mesh.applyWifiConfig();
    } else if (_page == 1) {
      uint8_t next = radioNextProfile(the_mesh.radioProfile());
      the_mesh.applyRadioProfile(next);
    } else if (_board->canControlLoRaFemLna()) {
      bool enable = !_board->isLoRaFemLnaEnabled();
      if (_board->setLoRaFemLnaEnabled(enable)) {
        _node_prefs->radio_fem_rxgain = enable ? 1 : 0;
        the_mesh.savePrefs();
      }
    }
    _next_refresh = 0;
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
      _display->turnOn();
      _auto_off = millis() + AUTO_OFF_MILLIS;
      if (_page == 1 && _powering_off_at == 0 && _board->canControlLoRaFemLna()) {
        bool enable = !_board->isLoRaFemLnaEnabled();
        if (_board->setLoRaFemLnaEnabled(enable)) {
          _node_prefs->radio_fem_rxgain = enable ? 1 : 0;
          the_mesh.savePrefs();
        }
        _next_refresh = 0;
      } else {
        Serial.println("Powering Off");
        _powering_off_at = millis() + POWEROFF_DELAY;
      }
  }
#endif
}

void UITask::loop() {
  pollButton();

  if (_display->isOn()) {
    if (millis() >= _next_refresh) {
      _display->startFrame();
      renderCurrScreen();
      _display->endFrame();

      _next_refresh = millis() + 1000;   // refresh every second
    }
    if (millis() > _auto_off) {
      _display->turnOff();
    }
  }

  if (_powering_off_at > 0) { // power off timer armed
#ifdef LED_PIN
    digitalWrite(LED_PIN, LED_STATE_ON); // switch on the led until poweroff
#endif
    if (millis() > _powering_off_at) {
      _board->powerOff();  // should not return
    }
  }
}
