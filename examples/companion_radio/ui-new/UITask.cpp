#include "UITask.h"
#include <math.h>
#include <helpers/TxtDataHelpers.h>
#include "../MyMesh.h"
#include "target.h"
#ifdef WIFI_SSID
  #include <WiFi.h>
#endif

#ifndef AUTO_OFF_MILLIS
  #define AUTO_OFF_MILLIS     15000   // 15 seconds
#endif
#define BOOT_SCREEN_MILLIS   3000   // 3 seconds

#ifdef PIN_STATUS_LED
#define LED_ON_MILLIS     20
#define LED_ON_MSG_MILLIS 200
#define LED_CYCLE_MILLIS  4000
#endif

#define LONG_PRESS_MILLIS   1200

#ifndef UI_RECENT_LIST_SIZE
  #define UI_RECENT_LIST_SIZE 4
#endif
#define NEIGHBOR_SCROLL_MS  2000

#define SCAN_SAMPLE_MILLIS    50   // noise scan RSSI sampling, independent of the frame rate
#ifndef SCAN_REFRESH_MILLIS
  #define SCAN_REFRESH_MILLIS       200
#endif
#ifndef SCAN_REFRESH_MILLIS_EINK
  #define SCAN_REFRESH_MILLIS_EINK 3000   // each e-ink update blocks the loop
#endif
#define FSCAN_REFRESH_MILLIS  250   // progress bar while the freq scan runs

#ifndef MSG_PAGE_MILLIS
  #if AUTO_OFF_MILLIS==0   // e-ink: every page flip is a full refresh
    #define MSG_PAGE_MILLIS 10000
  #else
    #define MSG_PAGE_MILLIS  4000
  #endif
#endif

#ifndef FSCAN_MAX
  #define FSCAN_MAX 17
#endif

#if UI_HAS_JOYSTICK
  #define PRESS_LABEL "press Enter"
#else
  #define PRESS_LABEL "long press"
#endif

#include "icons.h"

struct PowerChoice {
  bool boost;
  bool lna;
  bool cpu;
  const char* name;
};

// Names run from most economical to least. Missing options (no FEM, no CPU
// sleep) are skipped, so Eco max is always the cheapest mode on this board.
static const char* savingsName(int index, int count) {
  static const char* labels[] = {"Eco max", "Eco high", "Eco good", "Eco fair", "Eco low", "Eco min"};
  if (count <= 1) return labels[0];
  int slot = index * 5 / (count - 1);
  if (slot < 0) slot = 0;
  if (slot > 5) slot = 5;
  return labels[slot];
}

static int buildPowerChoices(bool fem, bool cpu_ok, PowerChoice* out) {
  int n = 0;
  if (cpu_ok) {
    out[n++] = {false, false, true, NULL};
    out[n++] = {true, false, true, NULL};
    if (fem) out[n++] = {true, true, true, NULL};
  }
  out[n++] = {false, false, false, NULL};
  out[n++] = {true, false, false, NULL};
  if (fem) out[n++] = {true, true, false, NULL};
  for (int i = 0; i < n; i++) out[i].name = savingsName(i, n);
  return n;
}

class SplashScreen : public UIScreen {
  UITask* _task;
  unsigned long dismiss_after;
  char _version_info[24];

public:
  SplashScreen(UITask* task) : _task(task) {
    firmwareVersionNoHash(_version_info, sizeof(_version_info), FIRMWARE_VERSION);

    dismiss_after = millis() + BOOT_SCREEN_MILLIS;
  }

  int render(DisplayDriver& display) override {
    // meshcore logo
    display.setColor(UIColor::corp_blue);
    int logoWidth = 128;
    display.drawXbm((display.width() - logoWidth) / 2, 3, meshcore_logo, logoWidth, 13);

    // meshcore website
    const char* website = "https://meshcore.io";
    display.setColor(UIColor::primary_txt);
    display.setTextSize(1);
    uint16_t websiteWidth = display.getTextWidth(website);
    display.setCursor((display.width() - websiteWidth) / 2, 22);
    display.print(website);

    // version info
    display.setColor(UIColor::primary_txt);
    display.setTextSize(1);
    display.drawTextCentered(display.width()/2, 35, _version_info);

    display.setColor(UIColor::secondary_txt);
    display.setTextSize(1);
    display.drawTextCentered(display.width()/2, 48, "by Termin36");

    return 1000;
  }

  void poll() override {
    if (millis() >= dismiss_after) {
      _task->gotoHomeScreen();
    }
  }
};

class HomeScreen : public UIScreen {
  enum HomePage {
    FIRST,
    RECENT,
    NEIGHBORS,
    RADIO,
    POWER,
    SCAN,
    FSCAN,
    BLUETOOTH,
    ADVERT,
#if ENV_INCLUDE_GPS == 1
    GPS,
    BEACON,
#endif
#if UI_SENSORS_PAGE == 1
    SENSORS,
#endif
    SHUTDOWN,
    Count    // keep as last
  };

  UITask* _task;
  mesh::RTCClock* _rtc;
  SensorManager* _sensors;
  NodePrefs* _node_prefs;
  uint8_t _page;
  bool _shutdown_init;
  AdvertPath recent[UI_RECENT_LIST_SIZE];
  NeighborInfo neighbor_list[NEIGHBOR_TABLE_SIZE];
  int neighbors_scroll_offset = 0;
  unsigned long neighbors_next_scroll = 0;
  int16_t scan_peak;
  int16_t scan_live = -127;
  int32_t scan_sum = 0;       // RSSI samples collected since the last frame
  uint16_t scan_cnt = 0;
  bool scan_pkt = false;
  unsigned long scan_next_sample = 0;

  void sampleNoise() {
    float rssi = radio_driver.getCurrentRSSI();
    int v = (int)(rssi + (rssi < 0 ? -0.5f : 0.5f));
    if (radio_driver.isReceivingPacket()) {
      scan_pkt = true;
      return;
    }
    scan_sum += v;
    scan_cnt++;
    if (scan_peak < -120 || v > scan_peak) scan_peak = (int16_t)v;
  }

  struct FreqScanBin {
    float freq;
    int16_t rssi;
  };
  enum { FSCAN_IDLE, FSCAN_RUN, FSCAN_DONE };
  uint8_t fscan_state;
  uint8_t fscan_i;
  uint8_t fscan_n;
  unsigned long fscan_ready_at;
  FreqScanBin fscan[FSCAN_MAX];
  FreqScanBin fscan_best[3];

  static int cmpFreqScan(const void* a, const void* b) {
    return ((const FreqScanBin*)a)->rssi - ((const FreqScanBin*)b)->rssi;
  }

  void freqScanBand(float center, float& lo, float& hi) {
    if (center >= 850.0f && center < 890.0f) { lo = 863.0f; hi = 870.0f; }
    else if (center >= 900.0f && center < 932.0f) { lo = 902.0f; hi = 928.0f; }
    else { lo = center - 1.0f; hi = center + 1.0f; }
  }

  void applyScanFreq(float f) {
    radio_driver.setFrequency(f);
  }

  void stopFreqScan() {
    if (fscan_state != FSCAN_RUN) return;
    if (fscan_i > 0) {
      fscan_n = fscan_i;
      finishFreqScan();
    } else {
      the_mesh.restoreRadioAfterScan();
      fscan_state = FSCAN_IDLE;
    }
  }

  void startFreqScan() {
    float step = _node_prefs->bw / 1000.0f;
    if (step < 0.025f) step = 0.025f;
    float center = _node_prefs->freq;
    float lo, hi;
    freqScanBand(center, lo, hi);

    fscan_n = 0;
    fscan[fscan_n++].freq = center;
    for (int k = 1; fscan_n < FSCAN_MAX; k++) {
      float down = center - (float)k * step;
      float up = center + (float)k * step;
      bool added = false;
      if (down >= lo - 0.0001f) {
        fscan[fscan_n++].freq = down;
        added = true;
        if (fscan_n >= FSCAN_MAX) break;
      }
      if (up <= hi + 0.0001f) {
        fscan[fscan_n++].freq = up;
        added = true;
      }
      if (!added) break;
    }
    for (int i = 0; i < fscan_n; i++) fscan[i].rssi = 0;
    fscan_i = 0;
    fscan_state = FSCAN_RUN;
    radio_driver.pauseNoiseFloor(true);
    applyScanFreq(fscan[0].freq);
    fscan_ready_at = millis() + 35;
  }

  void finishFreqScan() {
    the_mesh.restoreRadioAfterScan();
    FreqScanBin sorted[FSCAN_MAX];
    memcpy(sorted, fscan, fscan_n * sizeof(FreqScanBin));
    qsort(sorted, fscan_n, sizeof(FreqScanBin), cmpFreqScan);
    int out = 0;
    for (int i = 0; i < fscan_n && out < 3; i++) {
      fscan_best[out++] = sorted[i];
    }
    fscan_state = FSCAN_DONE;
  }

  void stepFreqScan() {
    if (millis() < fscan_ready_at || fscan_i >= fscan_n) return;
    int sum = 0, n = 0;
    for (int s = 0; s < 8; s++) {
      if (!radio_driver.isReceivingPacket()) {
        sum += (int)radio_driver.getCurrentRSSI();
        n++;
      }
      delay(2);
    }
    fscan[fscan_i].rssi = (int16_t)(n > 0 ? (sum / n) : (int)radio_driver.getCurrentRSSI());
    fscan_i++;
    if (fscan_i >= fscan_n) {
      finishFreqScan();
      _task->requestRefresh();
    } else {
      applyScanFreq(fscan[fscan_i].freq);
      fscan_ready_at = millis() + 35;
    }
  }


  void renderBatteryIndicator(DisplayDriver& display, uint16_t batteryMilliVolts) {
    // Convert millivolts to percentage
#ifndef BATT_MIN_MILLIVOLTS
  #define BATT_MIN_MILLIVOLTS 3000
#endif
#ifndef BATT_MAX_MILLIVOLTS
  #define BATT_MAX_MILLIVOLTS 4200
#endif
    const int minMilliVolts = BATT_MIN_MILLIVOLTS;
    const int maxMilliVolts = BATT_MAX_MILLIVOLTS;
    int batteryPercentage = ((batteryMilliVolts - minMilliVolts) * 100) / (maxMilliVolts - minMilliVolts);
    if (batteryPercentage < 0) batteryPercentage = 0; // Clamp to 0%
    if (batteryPercentage > 100) batteryPercentage = 100; // Clamp to 100%

    // battery icon
    int iconWidth = 24;
    int iconHeight = 10;
    int iconX = display.width() - iconWidth - 5; // Position the icon near the top-right corner
    int iconY = 0;
    display.setColor(UIColor::title_txt);

    // battery outline
    display.drawRect(iconX, iconY, iconWidth, iconHeight);

    // battery "cap"
    display.fillRect(iconX + iconWidth, iconY + (iconHeight / 4), 3, iconHeight / 2);

    // fill the battery based on the percentage
    int fillWidth = (batteryPercentage * (iconWidth - 4)) / 100;
    display.fillRect(iconX + 2, iconY + 2, fillWidth, iconHeight - 4);

    // show muted icon if buzzer is muted
#ifdef PIN_BUZZER
    if (_task->isBuzzerQuiet()) {
      display.setColor(UIColor::warning_txt);
      display.drawXbm(iconX - 9, iconY + 1, muted_icon, 8, 8);
    }
#endif
  }

  CayenneLPP sensors_lpp;
  int sensors_nb = 0;
  bool sensors_scroll = false;
  int sensors_scroll_offset = 0;
  int next_sensors_refresh = 0;

  void refresh_sensors() {
    if (millis() > next_sensors_refresh) {
      sensors_lpp.reset();
      sensors_nb = 0;
      sensors_lpp.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);
      sensors.querySensors(0xFF, sensors_lpp);
      LPPReader reader (sensors_lpp.getBuffer(), sensors_lpp.getSize());
      uint8_t channel, type;
      while(reader.readHeader(channel, type)) {
        reader.skipData(type);
        sensors_nb ++;
      }
      sensors_scroll = sensors_nb > UI_RECENT_LIST_SIZE;
#if AUTO_OFF_MILLIS > 0
      next_sensors_refresh = millis() + 5000; // refresh sensor values every 5 sec
#else
      next_sensors_refresh = millis() + 60000; // refresh sensor values every 1 min
#endif
    }
  }

  // The gps setting is published only after the module is detected.
  // Its value is "1" only while that module is switched on.
  bool isPageVisible(uint8_t page) const {
#if ENV_INCLUDE_GPS == 1
    if (page == HomePage::BEACON) return _task->getGPSState();
#endif
    return true;
  }

  uint8_t stepPage(int dir) {
    uint8_t page = _page;
    for (uint8_t n = 0; n < HomePage::Count; n++) {
      int next = (int)page + dir;
      if (next < 0) next += HomePage::Count;
      else if (next >= HomePage::Count) next -= HomePage::Count;
      page = (uint8_t)next;
      if (isPageVisible(page)) return page;
    }
    return _page;
  }

public:
  HomeScreen(UITask* task, mesh::RTCClock* rtc, SensorManager* sensors, NodePrefs* node_prefs)
     : _task(task), _rtc(rtc), _sensors(sensors), _node_prefs(node_prefs), _page(0),
       _shutdown_init(false), scan_peak(-127), fscan_state(FSCAN_IDLE), fscan_n(0),
       sensors_lpp(200) {  }

  void poll() override {
    if (_page == HomePage::SCAN || _page == HomePage::FSCAN) {
      _task->keepDisplayAwake();
    }
    if (_page == HomePage::SCAN && (long)(millis() - scan_next_sample) >= 0) {
      scan_next_sample = millis() + SCAN_SAMPLE_MILLIS;
      sampleNoise();
    }
    if (_page == HomePage::FSCAN && fscan_state == FSCAN_RUN) {
      stepFreqScan();
    }
    if (_shutdown_init && !_task->isButtonPressed()) {  // must wait for USR button to be released
      _task->shutdown();
    }
  }

  int render(DisplayDriver& display) override {
    if (!isPageVisible(_page)) _page = stepPage(1);

    display.setColor(UIColor::title_bkg);
    display.fillRect(0, 0, display.width(), 12);
    char tmp[80];
    // node name
    display.setTextSize(1);
    display.setColor(UIColor::title_txt);
    char filtered_name[sizeof(_node_prefs->node_name)];
    display.translateUTF8ToBlocks(filtered_name, _node_prefs->node_name, sizeof(filtered_name));
    display.setCursor(0, 2);
    display.print(filtered_name);

    // battery voltage
    renderBatteryIndicator(display, _task->getBattMilliVolts());

    // curr page indicator
    if (UIColor::title_bkg == UIColor::window_bkg) {
      display.setColor(UIColor::title_txt);
    } else {
      display.setColor(UIColor::title_bkg);
    }
    int y = 14;
    uint8_t visible_count = 0;
    uint8_t visible_index = 0;
    for (uint8_t i = 0; i < HomePage::Count; i++) {
      if (!isPageVisible(i)) continue;
      if (i == _page) visible_index = visible_count;
      visible_count++;
    }
    int x = display.width() / 2 - 5 * (visible_count > 0 ? visible_count - 1 : 0);
    uint8_t drawn = 0;
    for (uint8_t i = 0; i < HomePage::Count; i++) {
      if (!isPageVisible(i)) continue;
      if (drawn == visible_index) {
        display.fillRect(x-1, y-1, 4, 4);
      } else {
        display.fillRect(x, y, 2, 2);
      }
      x += 10;
      drawn++;
    }

    if (_page == HomePage::FIRST) {
      display.setColor(UIColor::primary_txt);
      display.setTextSize(2);
      sprintf(tmp, "MSG: %d", _task->getMsgCount());
      display.drawTextCentered(display.width() / 2, 22, tmp);

      #ifdef WIFI_SSID
        IPAddress ip = WiFi.localIP();
        snprintf(tmp, sizeof(tmp), "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        display.setTextSize(1);
        display.drawTextCentered(display.width() / 2, 54, tmp);
      #endif
      if (_task->hasConnection()) {
        display.setColor(UIColor::warning_txt);
        display.setTextSize(1);
        display.drawTextCentered(display.width() / 2, 43, "< Connected >");

      } else if (the_mesh.getBLEPin() != 0) { // BT pin
        display.setColor(UIColor::warning_txt);
        display.setTextSize(2);
        sprintf(tmp, "Pin:%d", the_mesh.getBLEPin());
        display.drawTextCentered(display.width() / 2, 43, tmp);
      }
    } else if (_page == HomePage::RECENT) {
      the_mesh.getRecentlyHeard(recent, UI_RECENT_LIST_SIZE);
      display.setColor(UIColor::primary_txt);
      int y = 20;
      for (int i = 0; i < UI_RECENT_LIST_SIZE; i++, y += 11) {
        auto a = &recent[i];
        if (a->name[0] == 0) continue;  // empty slot
        int secs = _rtc->getCurrentTime() - a->recv_timestamp;
        if (secs < 60) {
          sprintf(tmp, "%ds", secs);
        } else if (secs < 60*60) {
          sprintf(tmp, "%dm", secs / 60);
        } else {
          sprintf(tmp, "%dh", secs / (60*60));
        }

        int timestamp_width = display.getTextWidth(tmp);
        int max_name_width = display.width() - timestamp_width - 1;

        char filtered_recent_name[sizeof(a->name)];
        display.translateUTF8ToBlocks(filtered_recent_name, a->name, sizeof(filtered_recent_name));
        display.drawTextEllipsized(0, y, max_name_width, filtered_recent_name);
        display.setCursor(display.width() - timestamp_width - 1, y);
        display.print(tmp);
      }
    } else if (_page == HomePage::NEIGHBORS) {
      int n = the_mesh.getNeighbors(neighbor_list, NEIGHBOR_TABLE_SIZE);
      display.setColor(UIColor::primary_txt);
      display.setTextSize(1);
      if (n == 0) {
        neighbors_scroll_offset = 0;
        neighbors_next_scroll = 0;
        display.setColor(UIColor::secondary_txt);
        display.drawTextCentered(display.width() / 2, 28, "no neighbors");
        display.drawTextCentered(display.width() / 2, 64 - 11, "poll: " PRESS_LABEL);
      } else {
        const int line_h = 11;
        const int y0 = 20;
        int visible = (display.height() - y0) / line_h;
        if (visible < 1) visible = 1;
        if (n <= visible) {
          neighbors_scroll_offset = 0;
          neighbors_next_scroll = 0;
        } else {
          if (neighbors_scroll_offset >= n) neighbors_scroll_offset = 0;
          if (neighbors_next_scroll == 0) {
            neighbors_next_scroll = millis() + NEIGHBOR_SCROLL_MS;
          } else if ((long)(millis() - neighbors_next_scroll) >= 0) {
            neighbors_scroll_offset = (neighbors_scroll_offset + 1) % n;
            neighbors_next_scroll = millis() + NEIGHBOR_SCROLL_MS;
          }
        }

        int rows = n < visible ? n : visible;
        for (int i = 0; i < rows; i++) {
          auto a = &neighbor_list[(neighbors_scroll_offset + i) % n];
          int y = y0 + i * line_h;
          float snr = a->snr_x4 / 4.0f;
          sprintf(tmp, "%+.0f", snr);

          int snr_width = display.getTextWidth(tmp);
          int max_name_width = display.width() - snr_width - 1;

          char filtered_name[sizeof(a->name)];
          display.translateUTF8ToBlocks(filtered_name, a->name, sizeof(filtered_name));
          display.drawTextEllipsized(0, y, max_name_width, filtered_name);
          display.setCursor(display.width() - snr_width - 1, y);
          display.print(tmp);
        }
      }
    } else if (_page == HomePage::RADIO) {
      display.setColor(UIColor::primary_txt);
      display.setTextSize(1);
      // freq / sf
      display.setCursor(0, 20);
      sprintf(tmp, "FQ: %06.3f   SF: %d", _node_prefs->freq, _node_prefs->sf);
      display.print(tmp);

      display.setCursor(0, 31);
      sprintf(tmp, "BW: %03.2f     CR: %d", _node_prefs->bw, _node_prefs->cr);
      display.print(tmp);

      // tx power,  noise floor
      display.setCursor(0, 42);
      if (_task->canControlFemLna()) {
        sprintf(tmp, "TX: %ddBm  LNA: %s", _node_prefs->tx_power_dbm,
                _task->isFemLnaEnabled() ? "on" : "off");
      } else {
        sprintf(tmp, "TX: %ddBm  LNA: n/a", _node_prefs->tx_power_dbm);
      }
      display.print(tmp);
      display.setCursor(0, 53);
      sprintf(tmp, "Noise floor: %d", radio_driver.getNoiseFloor());
      display.print(tmp);
    } else if (_page == HomePage::POWER) {
      bool fem = _task->canControlFemLna();
      bool cpu_hw = _task->canSelectMcuSleep();
      bool cpu_ok = cpu_hw && !_task->gpsBlocksCpuSleep() && !_task->bleBlocksCpuSleep();
      bool boost = _node_prefs->rx_boosted_gain != 0;
      bool lna = fem && _node_prefs->radio_fem_rxgain != 0;
      bool cpu = cpu_ok && _node_prefs->mcu_sleep != 0;
      PowerChoice choices[6];
      int n = buildPowerChoices(fem, cpu_ok, choices);
      const char* name = "Custom";
      for (int i = 0; i < n; i++) {
        if (choices[i].boost == boost && choices[i].lna == lna && choices[i].cpu == cpu) {
          name = choices[i].name;
          break;
        }
      }

      // Beacon uses these rows on the 128x64 OLED: title is 12px, dots at y=14.
      display.setTextSize(1);
      int y = 18;
      display.setColor(UIColor::primary_txt);
      display.drawTextLeftAlign(0, y, "Power");
      display.setColor(UIColor::secondary_txt);
      display.drawTextRightAlign(display.width() - 1, y, name);
      y += 12;
      display.setColor(UIColor::primary_txt);
      display.drawTextLeftAlign(0, y, cpu ? "CPU off" : "CPU on");
      display.drawTextRightAlign(display.width() - 1, y, boost ? "RX on" : "RX off");
      y += 12;
      if (fem) {
        display.drawTextLeftAlign(0, y, lna ? "LNA on" : "LNA off");
        y += 12;
      }
      if (y + 8 <= display.height()) {
        const char* hold = NULL;
        if (cpu_hw && _task->gpsBlocksCpuSleep()) hold = "GPS needs CPU on";
        else if (cpu_hw && _task->bleBlocksCpuSleep()) hold = "BLE needs CPU on";
        display.setColor(hold ? UIColor::warning_txt : UIColor::secondary_txt);
        const char* hint = hold ? hold : "mode: " PRESS_LABEL;
        display.drawTextCentered(display.width() / 2, y, hint);
      }
    } else if (_page == HomePage::SCAN) {
      if (scan_cnt == 0) sampleNoise();
      if (scan_cnt > 0) {
        scan_live = (int16_t)((scan_sum - (int32_t)scan_cnt / 2) / (int32_t)scan_cnt);
      }
      int live = scan_live;
      bool pkt = scan_pkt;
      scan_sum = 0;
      scan_cnt = 0;
      scan_pkt = false;
      int nl = radio_driver.getNoiseFloor();

      display.setColor(UIColor::primary_txt);
      display.setTextSize(2);
      sprintf(tmp, "%d", live);
      display.drawTextCentered(display.width() / 2, 18, tmp);

      display.setTextSize(1);
      if (pkt) {
        display.setColor(UIColor::warning_txt);
        display.drawTextRightAlign(display.width() - 1, 18, "PKT");
      }

      display.setColor(UIColor::secondary_txt);
      if (nl == 0) {
        sprintf(tmp, "NL --");
      } else {
        sprintf(tmp, "NL %d", nl);
      }
      display.drawTextLeftAlign(0, 36, tmp);
      if (scan_peak < -120) {
        sprintf(tmp, "pk --");
      } else {
        sprintf(tmp, "pk %d", (int)scan_peak);
      }
      display.drawTextRightAlign(display.width() - 1, 36, tmp);

      const int rssi_min = -120;
      const int rssi_max = -50;
      int bar_x = 2;
      int bar_y = 48;
      int bar_h = 8;
      int bar_w = display.width() - 4;
      int clamped = live;
      if (clamped < rssi_min) clamped = rssi_min;
      if (clamped > rssi_max) clamped = rssi_max;
      int fill = ((clamped - rssi_min) * bar_w) / (rssi_max - rssi_min);

      display.setColor(UIColor::secondary_txt);
      display.drawRect(bar_x, bar_y, bar_w, bar_h);
      display.setColor(UIColor::primary_txt);
      if (fill > 2) {
        display.fillRect(bar_x + 1, bar_y + 1, fill - 2, bar_h - 2);
      }
    } else if (_page == HomePage::FSCAN) {
      display.setTextSize(1);
      if (fscan_state == FSCAN_RUN) {
        display.setColor(UIColor::primary_txt);
        if (fscan_n > 0) {
          sprintf(tmp, "scan %d/%d", (int)fscan_i, (int)fscan_n);
          display.drawTextCentered(display.width() / 2, 22, tmp);
          if (fscan_i < fscan_n) {
            sprintf(tmp, "%06.3f", fscan[fscan_i].freq);
            display.drawTextCentered(display.width() / 2, 36, tmp);
          }
          int bar_w = display.width() - 4;
          int fill = ((int)fscan_i * bar_w) / fscan_n;
          display.setColor(UIColor::secondary_txt);
          display.drawRect(2, 50, bar_w, 8);
          display.setColor(UIColor::primary_txt);
          if (fill > 2) display.fillRect(3, 51, fill - 2, 6);
        }
      } else if (fscan_state == FSCAN_DONE) {
        display.setColor(UIColor::secondary_txt);
        sprintf(tmp, "now %06.3f", _node_prefs->freq);
        display.drawTextLeftAlign(0, 18, tmp);
        display.setColor(UIColor::primary_txt);
        int y = 29;
        for (int i = 0; i < 3; i++, y += 11) {
          if (i >= fscan_n) break;
          bool here = fabsf(fscan_best[i].freq - _node_prefs->freq) < 0.001f;
          sprintf(tmp, "%06.3f  %d%s", fscan_best[i].freq, (int)fscan_best[i].rssi, here ? " *" : "");
          display.drawTextLeftAlign(0, y, tmp);
        }
      } else {
        display.setColor(UIColor::secondary_txt);
        display.drawTextCentered(display.width() / 2, 28, "find quiet freq");
        display.drawTextCentered(display.width() / 2, display.height() - 11, "scan: " PRESS_LABEL);
      }
    } else if (_page == HomePage::BLUETOOTH) {
      display.setColor(UIColor::corp_blue);
      display.drawXbm((display.width() - 32) / 2, 18,
          _task->isBluetoothEnabled() ? bluetooth_on : bluetooth_off,
          32, 32);
      display.setColor(UIColor::secondary_txt);
      display.setTextSize(1);
      display.drawTextCentered(display.width() / 2, 64 - 11, "toggle: " PRESS_LABEL);
    } else if (_page == HomePage::ADVERT) {
      display.setColor(UIColor::corp_blue);
      display.drawXbm((display.width() - 32) / 2, 18, advert_icon, 32, 32);
      display.setColor(UIColor::secondary_txt);
      display.drawTextCentered(display.width() / 2, 64 - 11, "advert: " PRESS_LABEL);
#if ENV_INCLUDE_GPS == 1
    } else if (_page == HomePage::GPS) {
      LocationProvider* nmea = sensors.getLocationProvider();
      char buf[50];
      int y = 20;
      bool gps_state = _task->getGPSState();
      const char* module = gps_state ? "on" : "off";
#ifdef PIN_GPS_SWITCH
      bool hw_gps_state = digitalRead(PIN_GPS_SWITCH);
      if (gps_state != hw_gps_state) {
        module = gps_state ? "off(hw)" : "off(sw)";
      }
#endif
      display.setColor(UIColor::secondary_txt);
      display.drawTextLeftAlign(0, y, "module");
      display.setColor(gps_state ? UIColor::primary_txt : UIColor::warning_txt);
      display.drawTextRightAlign(display.width() - 1, y, module);
      y += 12;
      if (nmea == NULL) {
        display.setColor(UIColor::secondary_txt);
        display.drawTextLeftAlign(0, y, "Can't access GPS");
      } else if (!gps_state) {
        // isEnabled() reads the enable pin. With no pin it stays true, so the
        // chip can keep drawing current after the software switch is off.
        display.setColor(UIColor::secondary_txt);
        display.drawTextLeftAlign(0, y, nmea->isEnabled() ? "stays powered" : "power off");
        y += 12;
      } else {
        display.setColor(UIColor::secondary_txt);
        sprintf(buf, "fix %s", nmea->isValid() ? "yes" : "no");
        display.drawTextLeftAlign(0, y, buf);
        display.setColor(UIColor::primary_txt);
        sprintf(buf, "sat %d", nmea->satellitesCount());
        display.drawTextRightAlign(display.width() - 1, y, buf);
        y += 12;
        display.setColor(UIColor::secondary_txt);
        display.drawTextLeftAlign(0, y, "pos");
        display.setColor(UIColor::primary_txt);
        sprintf(buf, "%.4f %.4f",
          nmea->getLatitude() / 1000000., nmea->getLongitude() / 1000000.);
        display.drawTextRightAlign(display.width() - 1, y, buf);
        y += 12;
        if (y + 8 <= display.height()) {
          display.setColor(UIColor::secondary_txt);
          display.drawTextLeftAlign(0, y, "alt");
          display.setColor(UIColor::primary_txt);
          sprintf(buf, "%.2f", nmea->getAltitude() / 1000.);
          display.drawTextRightAlign(display.width() - 1, y, buf);
          y += 12;
        }
      }
      if (display.height() - 11 >= y) {
        display.setColor(UIColor::secondary_txt);
        display.drawTextCentered(display.width() / 2, display.height() - 11, "toggle: " PRESS_LABEL);
      }
    } else if (_page == HomePage::BEACON) {
      char mode[20];
      char target[32];
      char pos[40];
      char sent[24];
      the_mesh.formatBeacon(mode, sizeof(mode), target, sizeof(target), pos, sizeof(pos), sent, sizeof(sent));
      char kind[16];
      char iv[8];
      kind[0] = iv[0] = 0;
      const char* sp = strchr(mode, ' ');
      if (sp) {
        size_t klen = (size_t)(sp - mode);
        if (klen >= sizeof(kind)) klen = sizeof(kind) - 1;
        memcpy(kind, mode, klen);
        kind[klen] = 0;
        strncpy(iv, sp + 1, sizeof(iv) - 1);
        iv[sizeof(iv) - 1] = 0;
      } else {
        strncpy(kind, mode, sizeof(kind) - 1);
        kind[sizeof(kind) - 1] = 0;
      }
      int y = 18;
      display.setColor(UIColor::primary_txt);
      display.drawTextLeftAlign(0, y, kind);
      if (iv[0]) {
        display.setColor(UIColor::secondary_txt);
        display.drawTextRightAlign(display.width() - 1, y, iv);
      }
      y += 12;
      char shown[32];
      display.translateUTF8ToBlocks(shown, target, sizeof(shown));
      display.setColor(UIColor::primary_txt);
      display.drawTextEllipsized(0, y, display.width() - 1, shown);
      y += 12;
      display.setColor(strcmp(pos, "no fix") == 0 || strcmp(pos, "no gps") == 0
          ? UIColor::warning_txt : UIColor::secondary_txt);
      display.drawTextEllipsized(0, y, display.width() - 1, pos);
      y += 12;
      display.setColor(UIColor::secondary_txt);
      display.drawTextLeftAlign(0, y, sent);
      if (display.height() > 80 && strcmp(kind, "off") != 0) {
        display.drawTextCentered(display.width() / 2, display.height() - 11, "3x time    4x off");
      }
#endif
#if UI_SENSORS_PAGE == 1
    } else if (_page == HomePage::SENSORS) {
      int y = 18;
      refresh_sensors();
      char buf[30];
      char name[30];
      LPPReader r(sensors_lpp.getBuffer(), sensors_lpp.getSize());

      for (int i = 0; i < sensors_scroll_offset; i++) {
        uint8_t channel, type;
        r.readHeader(channel, type);
        r.skipData(type);
      }

      for (int i = 0; i < (sensors_scroll?UI_RECENT_LIST_SIZE:sensors_nb); i++) {
        uint8_t channel, type;
        if (!r.readHeader(channel, type)) { // reached end, reset
          r.reset();
          r.readHeader(channel, type);
        }

        display.setCursor(0, y);
        float v;
        switch (type) {
          case LPP_GPS: // GPS
            float lat, lon, alt;
            r.readGPS(lat, lon, alt);
            strcpy(name, "gps"); sprintf(buf, "%.4f %.4f", lat, lon);
            break;
          case LPP_VOLTAGE:
            r.readVoltage(v);
            strcpy(name, "voltage"); sprintf(buf, "%6.2f", v);
            break;
          case LPP_CURRENT:
            r.readCurrent(v);
            strcpy(name, "current"); sprintf(buf, "%.3f", v);
            break;
          case LPP_TEMPERATURE:
            r.readTemperature(v);
            strcpy(name, "temperature"); sprintf(buf, "%.2f", v);
            break;
          case LPP_RELATIVE_HUMIDITY:
            r.readRelativeHumidity(v);
            strcpy(name, "humidity"); sprintf(buf, "%.2f", v);
            break;
          case LPP_BAROMETRIC_PRESSURE:
            r.readPressure(v);
            strcpy(name, "pressure"); sprintf(buf, "%.2f", v);
            break;
          case LPP_ALTITUDE:
            r.readAltitude(v);
            strcpy(name, "altitude"); sprintf(buf, "%.0f", v);
            break;
          case LPP_POWER:
            r.readPower(v);
            strcpy(name, "power"); sprintf(buf, "%6.2f", v);
            break;
          default:
            r.skipData(type);
            strcpy(name, "unk"); sprintf(buf, "");
        }
        display.setCursor(0, y);
        display.setColor(UIColor::secondary_txt);
        display.print(name);
        display.setColor(UIColor::primary_txt);
        display.setCursor(
          display.width()-display.getTextWidth(buf)-1, y
        );
        display.print(buf);
        y = y + 12;
      }
      if (sensors_scroll) sensors_scroll_offset = (sensors_scroll_offset+1)%sensors_nb;
      else sensors_scroll_offset = 0;
#endif
    } else if (_page == HomePage::SHUTDOWN) {
      display.setColor(UIColor::corp_blue);
      display.setTextSize(1);
      if (_shutdown_init) {
        display.setColor(UIColor::warning_txt);
        display.drawTextCentered(display.width() / 2, 34, "hibernating...");
      } else {
        display.setColor(UIColor::secondary_txt);
        display.drawXbm((display.width() - 32) / 2, 18, power_icon, 32, 32);
        display.drawTextCentered(display.width() / 2, 64 - 11, "hibernate:" PRESS_LABEL);
      }
    }
    if (_page == HomePage::SCAN) {
      return display.isEink() ? SCAN_REFRESH_MILLIS_EINK : SCAN_REFRESH_MILLIS;
    }
    if (_page == HomePage::FSCAN && fscan_state == FSCAN_RUN) {
      // e-ink skips the progress bar, finishing the scan requests a redraw
      return display.isEink() ? 5000 : FSCAN_REFRESH_MILLIS;
    }
    if (_page == HomePage::NEIGHBORS) return 1000;
#if ENV_INCLUDE_GPS == 1
    if (_page == HomePage::BEACON) return 1000;
#endif
    return 5000;   // next render after 5000 ms
  }

  bool handleInput(char c) override {
    if (c == KEY_QUAD) {
#if ENV_INCLUDE_GPS == 1
      if (_page != HomePage::BEACON) return false;
      if (the_mesh.stopBeacon()) {
        _task->notify(UIEventType::ack);
        _task->showAlert("beacon off", 900);
      }
      return true;
#else
      return false;
#endif
    }
    if (c == KEY_SELECT) {
#if ENV_INCLUDE_GPS == 1
      if (_page == HomePage::BEACON && the_mesh.cycleBeaconInterval()) {
        char mode[20];
        char target[32];
        char pos[40];
        char sent[24];
        char alert[48];
        the_mesh.formatBeacon(mode, sizeof(mode), target, sizeof(target), pos, sizeof(pos), sent, sizeof(sent));
        snprintf(alert, sizeof(alert), "%s", mode);
        _task->notify(UIEventType::ack);
        _task->showAlert(alert, 900);
        return true;
      }
#endif
      _task->toggleBuzzer();
      return true;
    }
    if (c == KEY_LEFT || c == KEY_PREV) {
      if (_page == HomePage::FSCAN) stopFreqScan();
      _page = stepPage(-1);
      if (_page == HomePage::NEIGHBORS) {
        neighbors_scroll_offset = 0;
        neighbors_next_scroll = 0;
      }
      return true;
    }
    if (c == KEY_NEXT || c == KEY_RIGHT) {
      if (_page == HomePage::FSCAN) stopFreqScan();
      _page = stepPage(1);
      if (_page == HomePage::RECENT) {
        _task->showAlert("Recent adverts", 800);
      }
      if (_page == HomePage::NEIGHBORS) {
        neighbors_scroll_offset = 0;
        neighbors_next_scroll = 0;
        _task->showAlert("Neighbors", 800);
      }
      if (_page == HomePage::RADIO) {
        _task->showAlert("Radio", 800);
      }
      if (_page == HomePage::POWER) {
        _task->showAlert("Power", 800);
      }
      if (_page == HomePage::SCAN) {
        _task->showAlert("Noise scan", 800);
      }
      if (_page == HomePage::FSCAN) {
        _task->showAlert("Freq scan", 800);
      }
#if ENV_INCLUDE_GPS == 1
      if (_page == HomePage::BEACON) {
        _task->showAlert("Beacon", 800);
      }
#endif
      return true;
    }
    if (c == KEY_ENTER && _page == HomePage::FSCAN) {
      if (fscan_state != FSCAN_RUN) {
        _task->notify(UIEventType::ack);
        startFreqScan();
        _task->showAlert("Scanning...", 600);
      }
      return true;
    }
    if (c == KEY_ENTER && _page == HomePage::SCAN) {
      scan_peak = -127;
      scan_sum = 0;
      scan_cnt = 0;
      _task->showAlert("Peak reset", 600);
      return true;
    }
    if (c == KEY_ENTER && _page == HomePage::NEIGHBORS) {
      neighbors_scroll_offset = 0;
      neighbors_next_scroll = 0;
      _task->notify(UIEventType::ack);
      if (the_mesh.sendNeighborDiscover()) {
        _task->showAlert("Polling...", 1000);
      } else {
        _task->showAlert("Poll failed", 1000);
      }
      return true;
    }
    if (c == KEY_ENTER && _page == HomePage::RADIO) {
      _task->toggleFemLna();
      return true;
    }
    if (c == KEY_ENTER && _page == HomePage::POWER) {
      bool fem = _task->canControlFemLna();
      bool cpu_ok = _task->canSelectMcuSleep() && !_task->gpsBlocksCpuSleep() && !_task->bleBlocksCpuSleep();
      bool boost = _node_prefs->rx_boosted_gain != 0;
      bool lna = fem && _node_prefs->radio_fem_rxgain != 0;
      bool cpu = cpu_ok && _node_prefs->mcu_sleep != 0;
      PowerChoice choices[6];
      int n = buildPowerChoices(fem, cpu_ok, choices);
      int cur = -1;
      for (int i = 0; i < n; i++) {
        if (choices[i].boost == boost && choices[i].lna == lna && choices[i].cpu == cpu) {
          cur = i;
          break;
        }
      }
      int next = (cur < 0) ? 0 : (cur + 1) % n;
      _task->applyPowerProfile(choices[next].boost, choices[next].lna, choices[next].cpu);
      return true;
    }
    if (c == KEY_ENTER && _page == HomePage::BLUETOOTH) {
      if (_task->isBluetoothEnabled()) {  // toggle Bluetooth on/off
        _task->disableBluetooth();
      } else {
        _task->enableBluetooth();
      }
      return true;
    }
    if (c == KEY_ENTER && _page == HomePage::ADVERT) {
      _task->notify(UIEventType::ack);
      if (the_mesh.advert()) {
        _task->showAlert("Advert sent!", 1000);
      } else {
        _task->showAlert("Advert failed..", 1000);
      }
      return true;
    }
#if ENV_INCLUDE_GPS == 1
    if (c == KEY_ENTER && _page == HomePage::GPS) {
      _task->toggleGPS();
      return true;
    }
    if (c == KEY_ENTER && _page == HomePage::BEACON) {
      the_mesh.cycleBeacon();
      char mode[20];
      char target[32];
      char pos[40];
      char sent[24];
      char alert[48];
      the_mesh.formatBeacon(mode, sizeof(mode), target, sizeof(target), pos, sizeof(pos), sent, sizeof(sent));
      snprintf(alert, sizeof(alert), "%s", mode);
      if (target[0]) {
        size_t used = strlen(alert);
        if (used + 2 < sizeof(alert)) {
          alert[used] = ' ';
          alert[used + 1] = 0;
          strncat(alert, target, sizeof(alert) - used - 2);
        }
      }
      _task->notify(UIEventType::ack);
      _task->showAlert(alert, 900);
      return true;
    }
#endif
#if UI_SENSORS_PAGE == 1
    if (c == KEY_ENTER && _page == HomePage::SENSORS) {
      _task->toggleGPS();
      next_sensors_refresh=0;
      return true;
    }
#endif
    if (c == KEY_ENTER && _page == HomePage::SHUTDOWN) {
      _shutdown_init = true;  // need to wait for button to be released
      return true;
    }
    return false;
  }
};

class MsgPreviewScreen : public UIScreen {
  UITask* _task;
  mesh::RTCClock* _rtc;

  struct MsgEntry {
    uint32_t timestamp;
    char origin[62];
    char msg[MAX_TEXT_LEN + 1];
  };
  #define MAX_UNREAD_MSGS   32
  int num_unread;
  int head = MAX_UNREAD_MSGS - 1; // index of latest unread message
  MsgEntry unread[MAX_UNREAD_MSGS];

  // long messages are split into screen-sized pages that rotate automatically
  int page_start = 0;   // offset into the filtered text of the page on screen
  int page_next = 0;    // offset of the following page, 0 when the text ends on this page
  int page_num = 0;
  unsigned long page_until = 0;

  void resetPage() {
    page_start = page_next = page_num = 0;
    page_until = millis() + MSG_PAGE_MILLIS;
  }

  void nextPage() {
    if (page_next > 0) {
      page_start = page_next;
      page_num++;
    } else {
      page_start = page_num = 0;
    }
    page_until = millis() + MSG_PAGE_MILLIS;
  }

public:
  MsgPreviewScreen(UITask* task, mesh::RTCClock* rtc) : _task(task), _rtc(rtc) { num_unread = 0; }

  void addPreview(uint8_t path_len, const char* from_name, const char* msg) {
    head = (head + 1) % MAX_UNREAD_MSGS;
    if (num_unread < MAX_UNREAD_MSGS) num_unread++;

    auto p = &unread[head];
    p->timestamp = _rtc->getCurrentTime();
    if (path_len == 0xFF) {
      sprintf(p->origin, "(D) %s:", from_name);
    } else {
      sprintf(p->origin, "(%d) %s:", (uint32_t) path_len, from_name);
    }
    StrHelper::strncpy(p->msg, msg, sizeof(p->msg));
    resetPage();
  }

  int render(DisplayDriver& display) override {
    auto p = &unread[head];
    char filtered_msg[sizeof(p->msg)];
    display.translateUTF8ToBlocks(filtered_msg, p->msg, sizeof(filtered_msg));
    int msg_len = strlen(filtered_msg);

    if (page_next > 0 || page_num > 0) {
      if ((long)(millis() - page_until) >= 0) nextPage();
    }
    if (page_start >= msg_len) resetPage();

    // short screens (128x64 OLED and alike) merge the sender into the title row to fit one more text line
    bool compact = display.height() < 100;
    int text_y = compact ? 14 : 25;

    display.setTextSize(1);
    display.setCursor(0, text_y);
    display.setColor(UIColor::primary_txt);
    const char* rest = display.printWordWrap(&filtered_msg[page_start], display.width());
    page_next = (*rest && rest > &filtered_msg[page_start]) ? (int)(rest - filtered_msg) : 0;
    bool paged = page_next > 0 || page_num > 0;

    char tmp[24];
    char age[8];
    int secs = _rtc->getCurrentTime() - p->timestamp;
    if (secs < 60) {
      sprintf(age, "%ds", secs);
    } else if (secs < 60*60) {
      sprintf(age, "%dm", secs / 60);
    } else {
      sprintf(age, "%dh", secs / (60*60));
    }
    char page_tag[8] = "";
    if (paged) sprintf(page_tag, "p%d%s ", page_num + 1, page_next > 0 ? "+" : "");

    char filtered_origin[sizeof(p->origin)];
    display.translateUTF8ToBlocks(filtered_origin, p->origin, sizeof(filtered_origin));

    if (compact) {
      if (num_unread > 1) {
        sprintf(tmp, "%s+%d %s", page_tag, num_unread - 1, age);
      } else {
        sprintf(tmp, "%s%s", page_tag, age);
      }
      int right_w = display.getTextWidth(tmp);
      display.setColor(UIColor::corp_blue);
      display.setCursor(display.width() - right_w - 1, 0);
      display.print(tmp);
      display.setColor(UIColor::secondary_txt);
      display.drawTextEllipsized(0, 0, display.width() - right_w - 4, filtered_origin);
      display.setColor(UIColor::corp_blue);
      display.drawRect(0, 11, display.width(), 1);  // horiz line
    } else {
      display.setCursor(0, 0);
      display.setColor(UIColor::corp_blue);
      sprintf(tmp, "Unread: %d", num_unread);
      display.print(tmp);

      sprintf(tmp, "%s%s", page_tag, age);
      display.setCursor(display.width() - display.getTextWidth(tmp) - 2, 0);
      display.print(tmp);

      display.drawRect(0, 11, display.width(), 1);  // horiz line

      display.setColor(UIColor::secondary_txt);
      display.drawTextEllipsized(0, 14, display.width(), filtered_origin);
    }

#if AUTO_OFF_MILLIS==0 // probably e-ink
    if (paged) {
      long wait = (long)(page_until - millis());
      return wait > 200 ? wait : 200;
    }
    return 10000; // 10 s
#else
    return 1000;  // next render after 1000 ms
#endif
  }

  bool handleInput(char c) override {
    if (c == KEY_NEXT || c == KEY_RIGHT) {
      head = (head + MAX_UNREAD_MSGS - 1) % MAX_UNREAD_MSGS;
      num_unread--;
      resetPage();
      if (num_unread == 0) {
        _task->gotoHomeScreen();
      }
      return true;
    }
    if (c == KEY_PREV || c == KEY_DOWN) {   // manual page flip (double click on single-button boards)
      if (page_next > 0 || page_num > 0) nextPage();
      return true;
    }
    if (c == KEY_UP) {
      resetPage();
      return true;
    }
    if (c == KEY_ENTER) {
      num_unread = 0;  // clear unread queue
      _task->gotoHomeScreen();
      return true;
    }
    return false;
  }
};

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _display = display;
  _sensors = sensors;
  _auto_off = millis() + AUTO_OFF_MILLIS;

#if defined(PIN_USER_BTN)
  user_btn.begin();
#endif
#if defined(PIN_USER_BTN_ANA)
  analog_btn.begin();
#endif

  _node_prefs = node_prefs;

  if (_display != NULL) {
    _display->turnOn();
  }

#ifdef PIN_BUZZER
  buzzer.begin();
  buzzer.quiet(_node_prefs->buzzer_quiet);
  buzzer.startup();
#endif

#ifdef PIN_VIBRATION
  vibration.begin();
#endif

  ui_started_at = millis();
  _alert_expiry = 0;

  splash = new SplashScreen(this);
  home = new HomeScreen(this, &rtc_clock, sensors, node_prefs);
  msg_preview = new MsgPreviewScreen(this, &rtc_clock);
  setCurrScreen(splash);
}

void UITask::showAlert(const char* text, int duration_millis) {
  strcpy(_alert, text);
  _alert_expiry = millis() + duration_millis;
}

void UITask::keepDisplayAwake() {
#if AUTO_OFF_MILLIS > 0
  _auto_off = millis() + AUTO_OFF_MILLIS;
#endif
}

void UITask::notify(UIEventType t) {
#if defined(PIN_BUZZER)
switch(t){
  case UIEventType::contactMessage:
    // gemini's pick
    buzzer.play("MsgRcv3:d=4,o=6,b=200:32e,32g,32b,16c7");
    break;
  case UIEventType::channelMessage:
    buzzer.play("kerplop:d=16,o=6,b=120:32g#,32c#");
    break;
  case UIEventType::ack:
    buzzer.play("ack:d=32,o=8,b=120:c");
    break;
  case UIEventType::roomMessage:
  case UIEventType::newContactMessage:
  case UIEventType::none:
  default:
    break;
}
#endif

#ifdef PIN_VIBRATION
  // Trigger vibration for all UI events except none
  if (t != UIEventType::none) {
    vibration.trigger();
  }
#endif
}


void UITask::msgRead(int msgcount) {
  _msgcount = msgcount;
  if (msgcount == 0) {
    gotoHomeScreen();
  }
}

void UITask::newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) {
  _msgcount = msgcount;

  ((MsgPreviewScreen *) msg_preview)->addPreview(path_len, from_name, text);
  setCurrScreen(msg_preview);

  if (_display != NULL) {
    if (!_display->isOn() && !hasConnection()) {
      _display->turnOn();
    }
    if (_display->isOn()) {
    _auto_off = millis() + AUTO_OFF_MILLIS;  // extend the auto-off timer
    _next_refresh = 100;  // trigger refresh
    }
  }
}

void UITask::userLedHandler() {
#ifdef PIN_STATUS_LED
  int cur_time = millis();
  if (cur_time > next_led_change) {
    if (led_state == 0) {
      led_state = 1;
      if (_msgcount > 0) {
        last_led_increment = LED_ON_MSG_MILLIS;
      } else {
        last_led_increment = LED_ON_MILLIS;
      }
      next_led_change = cur_time + last_led_increment;
    } else {
      led_state = 0;
      next_led_change = cur_time + LED_CYCLE_MILLIS - last_led_increment;
    }
    digitalWrite(PIN_STATUS_LED, led_state == LED_STATE_ON);
  }
#endif
}

void UITask::setCurrScreen(UIScreen* c) {
  curr = c;
  _next_refresh = 100;
}

/*
  hardware-agnostic pre-shutdown activity should be done here
*/
void UITask::shutdown(bool restart){

  #ifdef PIN_BUZZER
  /* note: we have a choice here -
     we can do a blocking buzzer.loop() with non-deterministic consequences
     or we can set a flag and delay the shutdown for a couple of seconds
     while a non-blocking buzzer.loop() plays out in UITask::loop()
  */
  buzzer.shutdown();
  uint32_t buzzer_timer = millis(); // fail-safe shutdown
  while (buzzer.isPlaying() && (millis() - 2500) < buzzer_timer)
    buzzer.loop();

  #endif // PIN_BUZZER

  if (restart) {
    _board->reboot();
  } else {
    // Power off board including radio, display, GPS and components
    _board->powerOff();
  }
}

bool UITask::isButtonPressed() const {
#ifdef PIN_USER_BTN
  return user_btn.isPressed();
#else
  return false;
#endif
}

void UITask::loop() {
  char c = 0;
#if UI_HAS_JOYSTICK
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_ENTER);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_ENTER);  // REVISIT: could be mapped to different key code
  }
  ev = joystick_left.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_LEFT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_LEFT);
  }
  ev = joystick_right.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_RIGHT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_RIGHT);
  }
  ev = back_btn.check();
  if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
    c = handleTripleClick(KEY_SELECT);
  }
#elif defined(PIN_USER_BTN)
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_NEXT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_ENTER);
  } else if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
    c = handleDoubleClick(KEY_PREV);
  } else if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
    c = handleTripleClick(KEY_SELECT);
  } else if (ev == BUTTON_EVENT_QUAD_CLICK) {
    c = handleQuadClick(KEY_QUAD);
  }
#endif
#if defined(UI_HAS_ROTARY_INPUT)
  RotaryInputEvent rotaryEv = rotary_input.poll();
  if (c == 0 && _display != NULL && _display->isOn()) {
    if (rotaryEv == RotaryInputEvent::Next) {
      c = KEY_NEXT;
    } else if (rotaryEv == RotaryInputEvent::Prev) {
      c = KEY_PREV;
    }
  }
#endif
#if defined(PIN_USER_BTN_ANA)
  if (abs(millis() - _analogue_pin_read_millis) > 10) {
    int ev = analog_btn.check();
    if (ev == BUTTON_EVENT_CLICK) {
      c = checkDisplayOn(KEY_NEXT);
    } else if (ev == BUTTON_EVENT_LONG_PRESS) {
      c = handleLongPress(KEY_ENTER);
    } else if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
      c = handleDoubleClick(KEY_PREV);
    } else if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
      c = handleTripleClick(KEY_SELECT);
    } else if (ev == BUTTON_EVENT_QUAD_CLICK) {
      c = handleQuadClick(KEY_QUAD);
    }
    _analogue_pin_read_millis = millis();
  }
#endif
#if defined(BACKLIGHT_BTN)
  if (millis() > next_backlight_btn_check) {
    bool touch_state = digitalRead(PIN_BUTTON2);
#if defined(DISP_BACKLIGHT)
    digitalWrite(DISP_BACKLIGHT, !touch_state);
#elif defined(EXP_PIN_BACKLIGHT)
    expander.digitalWrite(EXP_PIN_BACKLIGHT, !touch_state);
#endif
    next_backlight_btn_check = millis() + 300;
  }
#endif

  if (c != 0 && curr) {
    curr->handleInput(c);
    _auto_off = millis() + AUTO_OFF_MILLIS;   // extend auto-off timer
    _next_refresh = 100;  // trigger refresh
  }

  userLedHandler();

#ifdef PIN_BUZZER
  if (buzzer.isPlaying())  buzzer.loop();
#endif

  if (curr) curr->poll();

  if (_display != NULL && _display->isOn()) {
    if (millis() >= _next_refresh && curr) {
      _display->startFrame();
      int delay_millis = curr->render(*_display);
      if (millis() < _alert_expiry) {  // render alert popup
        _display->setTextSize(1);
        int y = _display->height() / 3;
        int p = _display->height() / 32;
        _display->setColor(UIColor::popup_bkg);
        _display->fillRect(p, y, _display->width() - p*2, y);
        _display->setColor(UIColor::popup_txt);  // draw box border
        _display->drawRect(p, y, _display->width() - p*2, y);
        _display->drawTextCentered(_display->width() / 2, y + p*3, _alert);
        _next_refresh = _alert_expiry;   // will need refresh when alert is dismissed
      } else {
        _next_refresh = millis() + delay_millis;
      }
      _display->endFrame();
    }
#if AUTO_OFF_MILLIS > 0
#ifdef KEEP_DISPLAY_ON_USB
    // Opt-in: refresh the auto-off deadline while externally powered, so the
    // timer counts from the moment external power is removed. Off by default
    // because OLED panels burn in quickly; only enable for LCD targets or
    // where the display is replaceable.
    if (board.isExternalPowered()) {
      _auto_off = millis() + AUTO_OFF_MILLIS;
    }
#endif
    if (millis() > _auto_off) {
      _display->turnOff();
    }
#endif
  }

#ifdef PIN_VIBRATION
  vibration.loop();
#endif

#ifdef AUTO_SHUTDOWN_MILLIVOLTS
  if (millis() > next_batt_chck) {
    uint16_t milliVolts = getBattMilliVolts();
    if (milliVolts > 0 && milliVolts < AUTO_SHUTDOWN_MILLIVOLTS) {
      if(!board.isExternalPowered()) {
        if (_display != NULL) {
          _display->startFrame();
          _display->setTextSize(2);
          _display->setColor(UIColor::warning_txt);
          _display->drawTextCentered(_display->width() / 2, 20, "Low Battery.");
          _display->drawTextCentered(_display->width() / 2, 40, "Shutting Down!");
          _display->endFrame();
          if (_display->isEink() == false) { delay(3000); }
        }
        shutdown();
      }
    }
    next_batt_chck = millis() + 8000;
  }
#endif
}

char UITask::checkDisplayOn(char c) {
  if (_display != NULL) {
    if (!_display->isOn()) {
      _display->turnOn();   // turn display on and consume event
      c = 0;
    }
    _auto_off = millis() + AUTO_OFF_MILLIS;   // extend auto-off timer
    _next_refresh = 0;  // trigger refresh
  }
  return c;
}

char UITask::handleLongPress(char c) {
  if (millis() - ui_started_at < 8000) {   // long press in first 8 seconds since startup -> CLI/rescue
    the_mesh.enterCLIRescue();
    c = 0;   // consume event
  }
  return c;
}

char UITask::handleDoubleClick(char c) {
  MESH_DEBUG_PRINTLN("UITask: double-click triggered");
  checkDisplayOn(c);
  return c;
}

char UITask::handleQuadClick(char c) {
  c = checkDisplayOn(c);
  if (c == 0) return 0;
  if (curr == home) return c;
  return 0;
}

char UITask::handleTripleClick(char c) {
  MESH_DEBUG_PRINTLN("UITask: triple click triggered");
  c = checkDisplayOn(c);
  if (c == 0) return 0;
  if (curr == home) return c; // home screen: beacon interval, otherwise buzzer
  toggleBuzzer();
  return 0;
}

bool UITask::getGPSState() {
  if (_sensors != NULL) {
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
        return !strcmp(_sensors->getSettingValue(i), "1");
      }
    }
  }
  return false;
}

void UITask::toggleGPS() {
    if (_sensors != NULL) {
    // toggle GPS on/off
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
        if (strcmp(_sensors->getSettingValue(i), "1") == 0) {
          _sensors->setSettingValue("gps", "0");
          _node_prefs->gps_enabled = 0;
          notify(UIEventType::ack);
        } else {
          _sensors->setSettingValue("gps", "1");
          _node_prefs->gps_enabled = 1;
          notify(UIEventType::ack);
        }
        the_mesh.savePrefs();
        if (_node_prefs->gps_enabled && _node_prefs->mcu_sleep) {
          showAlert("GPS needs CPU on", 1200);
        } else {
          showAlert(_node_prefs->gps_enabled ? "GPS: Enabled" : "GPS: Disabled", 800);
        }
        _next_refresh = 0;
        break;
      }
    }
  }
}

bool UITask::isDisplayOn() const {
#if AUTO_OFF_MILLIS > 0
  return _display != NULL && _display->isOn();
#else
  return false;
#endif
}

void UITask::applyPowerProfile(bool rx_boost, bool fem_lna, bool cpu_sleep) {
  if (!radio_driver.setRxBoostedGainMode(rx_boost)) {
    showAlert("RX boost failed", 900);
    return;
  }
  _node_prefs->rx_boosted_gain = rx_boost ? 1 : 0;
  bool cpu_blocked = false;
  bool ble_blocked = bleBlocksCpuSleep();
  if (cpu_sleep && (gpsBlocksCpuSleep() || ble_blocked)) {
    cpu_sleep = false;
    cpu_blocked = true;
  }
  if (_board && _board->canControlLoRaFemLna()) {
    if (!_board->setLoRaFemLnaEnabled(fem_lna)) {
      the_mesh.savePrefs();
      showAlert("LNA failed", 900);
      _next_refresh = 0;
      return;
    }
    _node_prefs->radio_fem_rxgain = fem_lna ? 1 : 0;
  }
  _node_prefs->mcu_sleep = (cpu_sleep && canSelectMcuSleep()) ? 1 : 0;
  the_mesh.savePrefs();
  notify(UIEventType::ack);
  bool fem = canControlFemLna();
  bool cpu_ok = canSelectMcuSleep();
  PowerChoice choices[6];
  int n = buildPowerChoices(fem, cpu_ok, choices);
  bool boost = _node_prefs->rx_boosted_gain != 0;
  bool lna = fem && _node_prefs->radio_fem_rxgain != 0;
  bool cpu = _node_prefs->mcu_sleep != 0;
  const char* name = "Custom";
  for (int i = 0; i < n; i++) {
    if (choices[i].boost == boost && choices[i].lna == lna && choices[i].cpu == cpu) {
      name = choices[i].name;
      break;
    }
  }
  if (cpu_blocked) showAlert(ble_blocked ? "BLE needs CPU on" : "GPS needs CPU on", 1200);
  else showAlert(name, 800);
  _next_refresh = 0;
}

void UITask::toggleFemLna() {
  if (!_board || !_board->canControlLoRaFemLna()) {
    showAlert("LNA: n/a", 800);
    return;
  }
  bool enable = !_board->isLoRaFemLnaEnabled();
  if (!_board->setLoRaFemLnaEnabled(enable)) {
    showAlert("LNA: failed", 800);
    return;
  }
  _node_prefs->radio_fem_rxgain = enable ? 1 : 0;
  the_mesh.savePrefs();
  notify(UIEventType::ack);
  showAlert(enable ? "LNA: on" : "LNA: off", 800);
  _next_refresh = 0;
}

void UITask::toggleBuzzer() {
    // Toggle buzzer quiet mode
  #ifdef PIN_BUZZER
    if (buzzer.isQuiet()) {
      buzzer.quiet(false);
      notify(UIEventType::ack);
    } else {
      buzzer.quiet(true);
    }
    _node_prefs->buzzer_quiet = buzzer.isQuiet();
    the_mesh.savePrefs();
    showAlert(buzzer.isQuiet() ? "Buzzer: OFF" : "Buzzer: ON", 800);
    _next_refresh = 0;  // trigger refresh
  #endif
}
