#include "UITask.h"
#include <helpers/ui/BatteryLevel.h>
#include <helpers/ui/ClockFont.h>
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

static void clockYmdFromUnix(uint32_t unix_s, int& year, int& month, int& day, int& wday) {
  uint32_t days = unix_s / 86400UL;
  wday = (int)((days + 4) % 7);  // 1970-01-01 was Thursday; 0 = Sunday
  year = 1970;
  for (;;) {
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    uint32_t diy = leap ? 366 : 365;
    if (days < diy) break;
    days -= diy;
    year++;
  }
  static const uint8_t mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  month = 1;
  for (int i = 0; i < 12; i++) {
    int dim = mdays[i];
    if (i == 1 && leap) dim = 29;
    if (days < (uint32_t)dim) {
      day = (int)days + 1;
      return;
    }
    days -= dim;
    month++;
  }
  day = 1;
}

static void formatClockHm(uint32_t t, int16_t tz_mins, char* buf, size_t n) {
  int64_t local = (int64_t)t + (int64_t)tz_mins * 60;
  if (local < 0) local = 0;
  uint32_t mins = ((uint32_t)local / 60UL) % (24UL * 60UL);
  snprintf(buf, n, "%02u:%02u", (unsigned)(mins / 60UL), (unsigned)(mins % 60UL));
}

static void formatClockDate(uint32_t t, int16_t tz_mins, uint8_t lang, char* buf, size_t n) {
  int64_t local = (int64_t)t + (int64_t)tz_mins * 60;
  if (local < 0) local = 0;
  int year, month, day, wday;
  clockYmdFromUnix((uint32_t)local, year, month, day, wday);
  static const char* en[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* ru[] = {"Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб"};
  const char* name = (lang == UI_LANG_EN) ? en[wday] : ru[wday];
  snprintf(buf, n, "%s, %02d.%02d", name, day, month);
  (void)year;
}

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
    static BatteryLevelFilter batt_filter;
    int batteryPercentage = batt_filter.push(batteryMilliVolts, minMilliVolts, maxMilliVolts)
                            * 100 / BatteryLevelFilter::LEVELS;

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

  bool allowsClock() const {
    if (_page == HomePage::SCAN || _page == HomePage::FSCAN) return false;
    if (_page == HomePage::NEIGHBORS) return false;
#if ENV_INCLUDE_GPS == 1
    if (_page == HomePage::BEACON) return false;
#endif
    return true;
  }

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
      // MSG: still waiting for the phone / still kept on this device
      display.setTextSize(2);
      sprintf(tmp, "MSG: %d/%d", _task->getMsgCount(), _task->getDeviceStored());
      display.drawTextCentered(display.width() / 2, 22, tmp);

      bool show_date = !display.isEink() && _rtc->getCurrentTime() >= 1000000000UL;
      #ifdef WIFI_SSID
        IPAddress ip = WiFi.localIP();
        snprintf(tmp, sizeof(tmp), "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        display.setTextSize(1);
        int ip_y = display.height() - 22;
        if (display.height() <= 64) ip_y = show_date ? -1 : 54;
        if (ip_y >= 0) display.drawTextCentered(display.width() / 2, ip_y, tmp);
      #endif
      if (_task->hasConnection()) {
        display.setColor(UIColor::warning_txt);
        display.setTextSize(1);
        display.drawTextCentered(display.width() / 2, 43, "< Connected >");

      } else if (the_mesh.getBLEPin() != 0) { // BT pin
        display.setColor(UIColor::warning_txt);
        bool room = !show_date || display.height() > 64;
        display.setTextSize(room ? 2 : 1);
        sprintf(tmp, "Pin:%d", the_mesh.getBLEPin());
        display.drawTextCentered(display.width() / 2, 43, tmp);
      }
      if (show_date) {
        int16_t tz = _node_prefs ? _node_prefs->tz_offset_mins : 0;
        uint32_t now = _rtc->getCurrentTime();
        char hm[8];
        char date_buf[24];
        char line[36];
        formatClockHm(now, tz, hm, sizeof(hm));
        formatClockDate(now, tz, _node_prefs ? _node_prefs->ui_lang : UI_LANG_RU,
                        date_buf, sizeof(date_buf));
        snprintf(line, sizeof(line), "%s  %s", hm, date_buf);
        display.setTextSize(1);
        display.setColor(UIColor::secondary_txt);
        display.drawTextCentered(display.width() / 2, display.height() - 11, line);
      }
    } else if (_page == HomePage::RECENT) {
      the_mesh.getRecentlyHeard(recent, UI_RECENT_LIST_SIZE);
      int shown = 0;
      display.setColor(UIColor::primary_txt);
      int y = 20;
      for (int i = 0; i < UI_RECENT_LIST_SIZE; i++, y += 11) {
        auto a = &recent[i];
        if (a->name[0] == 0) continue;  // empty slot
        shown++;
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
      if (shown == 0) {
        display.setColor(UIColor::secondary_txt);
        display.drawTextCentered(display.width() / 2, 28, "no adverts");
      }
      // last slot stays free until the list is full, so the send hint fits there
      if (recent[UI_RECENT_LIST_SIZE - 1].name[0] == 0) {
        display.setColor(UIColor::secondary_txt);
        display.drawTextCentered(display.width() / 2, 20 + (UI_RECENT_LIST_SIZE - 1) * 11, "advert: " PRESS_LABEL);
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
    if (_page == HomePage::FIRST && !display.isEink() && _rtc->getCurrentTime() >= 1000000000UL) {
      uint32_t sec = _rtc->getCurrentTime() % 60UL;
      int wait = (int)((60UL - sec) * 1000UL);
      if (wait < 1000) wait = 1000;
      return wait;
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
    if (c == KEY_ENTER && _page == HomePage::RECENT) {
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
    // long press on the main page reopens messages already previewed or dismissed
    if (c == KEY_ENTER && _page == HomePage::FIRST) {
      return _task->reopenMsgPreview();
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
    bool group;
  };
  #define MAX_UNREAD_MSGS   32
  int num_unread;
  int stored = 0;   // messages still held in the ring after they are read or dismissed
  int groups = 0;
  int chats = 0;
  int head = MAX_UNREAD_MSGS - 1;   // message currently on screen
  int latest = MAX_UNREAD_MSGS - 1; // most recently received message
  bool replay = false;
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

  int oldestIndex() const {
    if (stored <= 0) return latest;
    return (latest + MAX_UNREAD_MSGS - (stored - 1)) % MAX_UNREAD_MSGS;
  }

  // oldest message that is still counted as unread
  int oldestUnread() const {
    if (num_unread <= 0) return latest;
    return (latest + MAX_UNREAD_MSGS - (num_unread - 1)) % MAX_UNREAD_MSGS;
  }

  void dropHead() {
    if (stored <= 0) return;
    int oldest = oldestIndex();
    int pos = 0;
    for (int idx = oldest; idx != head && pos < stored; pos++) {
      idx = (idx + 1) % MAX_UNREAD_MSGS;
    }
    if (pos >= stored) return;

    bool was_group = unread[head].group;
    for (int i = pos; i < stored - 1; i++) {
      int dst = (oldest + i) % MAX_UNREAD_MSGS;
      unread[dst] = unread[(dst + 1) % MAX_UNREAD_MSGS];
    }
    stored--;
    if (num_unread > 0) num_unread--;
    if (was_group) { if (groups > 0) groups--; }
    else if (chats > 0) chats--;

    if (stored == 0 || num_unread == 0) {
      num_unread = 0;
      if (stored == 0) groups = chats = 0;
      _task->gotoHomeScreen();
      return;
    }
    latest = (oldest + stored - 1) % MAX_UNREAD_MSGS;
    head = (pos >= stored) ? latest : (oldest + pos) % MAX_UNREAD_MSGS;
    resetPage();
  }

public:
  // The phone took the oldest message that was also shown here. Remove that copy.
  bool dropOldest() {
    if (stored <= 0) return true;
    int oldest = oldestIndex();
    bool viewing = (head == oldest);
    bool was_group = unread[oldest].group;
    bool counted = (num_unread > 0 && num_unread == stored);

    for (int i = 0; i < stored - 1; i++) {
      int dst = (oldest + i) % MAX_UNREAD_MSGS;
      unread[dst] = unread[(dst + 1) % MAX_UNREAD_MSGS];
    }
    stored--;
    if (counted) {
      num_unread--;
      if (was_group) { if (groups > 0) groups--; }
      else if (chats > 0) chats--;
    }
    if (stored == 0) {
      num_unread = 0;
      groups = chats = 0;
      return true;
    }
    latest = (oldest + stored - 1) % MAX_UNREAD_MSGS;
    if (viewing) {
      head = oldest;
      resetPage();
    } else {
      head = (head + MAX_UNREAD_MSGS - 1) % MAX_UNREAD_MSGS;
    }
    return false;
  }

  MsgPreviewScreen(UITask* task, mesh::RTCClock* rtc) : _task(task), _rtc(rtc) { num_unread = 0; }

  int groupCount() const { return groups; }
  int chatCount() const { return chats; }
  int unreadCount() const { return num_unread; }
  int storedCount() const { return stored; }

  bool reopen() {
    if (stored == 0) return false;
    num_unread = stored;
    head = oldestUnread();
    groups = chats = 0;
    for (int i = 0; i < stored; i++) {
      int idx = (latest + MAX_UNREAD_MSGS - i) % MAX_UNREAD_MSGS;
      if (unread[idx].group) groups++;
      else chats++;
    }
    replay = true;
    resetPage();
    return true;
  }

  void addPreview(uint8_t path_len, const char* from_name, const char* msg, bool group) {
    int prev_unread = num_unread;
    int slot = (latest + 1) % MAX_UNREAD_MSGS;
    bool full = stored == MAX_UNREAD_MSGS;
    bool head_overwritten = full && head == slot;
    if (full && num_unread == MAX_UNREAD_MSGS) {
      if (unread[slot].group) { if (groups > 0) groups--; }
      else if (chats > 0) chats--;
    } else if (!full) {
      stored++;
      num_unread++;
    } else if (num_unread < MAX_UNREAD_MSGS) {
      num_unread++;
    }
    latest = slot;
    replay = false;

    auto p = &unread[latest];
    p->group = group;
    if (group) groups++; else chats++;
    p->timestamp = _rtc->getCurrentTime();
    if (path_len == 0xFF) {
      sprintf(p->origin, "(D) %s:", from_name);
    } else {
      sprintf(p->origin, "(%d) %s:", (uint32_t) path_len, from_name);
    }
    StrHelper::strncpy(p->msg, msg, sizeof(p->msg));
    // start at the oldest unread; a message that arrives mid-read waits at the end
    if (prev_unread == 0 || head_overwritten) {
      head = oldestUnread();
      resetPage();
    }
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
      sprintf(tmp, "%s: %d", replay ? "Recent" : "Unread", num_unread);
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
      bool was_group = unread[head].group;
      num_unread--;
      if (was_group) { if (groups > 0) groups--; }
      else if (chats > 0) chats--;
      if (num_unread == 0) {
        _task->gotoHomeScreen();
      } else {
        head = (head + 1) % MAX_UNREAD_MSGS;  // older message first, then newer
        resetPage();
      }
      return true;
    }
    if (c == KEY_SELECT) {  // triple click: drop this message from the buffer
      dropHead();
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
      groups = chats = 0;
      _task->gotoHomeScreen();
      return true;
    }
    return false;
  }
};

static void clockDraw(DisplayDriver& display, const ClockFace& face, int x, int y, const char* text) {
  for (const char* s = text; *s; ) {
    uint16_t cp;
    s = clockNext(s, cp);
    ClockGlyph g;
    if (!clockFind(face, cp, g)) continue;
    if (g.w && g.h) display.blit1(x, y + g.top, g.w, g.h, face.bits + g.offset);
    x += g.advance;
  }
}

class ClockScreen : public UIScreen {
  UITask* _task;
  mesh::RTCClock* _rtc;
  MsgPreviewScreen* _preview;

  NodePrefs* _prefs;

  static void formatTime(uint32_t t, int16_t tz_mins, char* buf) {
    if (t < 1000000000UL) {
      strcpy(buf, "--:--");
      return;
    }
    int64_t local = (int64_t)t + (int64_t)tz_mins * 60;
    if (local < 0) local = 0;
    uint32_t mins = ((uint32_t)local / 60UL) % (24UL * 60UL);
    sprintf(buf, "%02u:%02u", (unsigned)(mins / 60UL), (unsigned)(mins % 60UL));
  }

public:
  ClockScreen(UITask* task, mesh::RTCClock* rtc, MsgPreviewScreen* preview, NodePrefs* prefs)
    : _task(task), _rtc(rtc), _preview(preview), _prefs(prefs) {}

  int render(DisplayDriver& display) override {
    display.setColor(UIColor::primary_txt);
    int fw = display.frameWidth();
    int fh = display.frameHeight();
    const ClockFace& digits = (fw >= 220) ? CLOCK_DIGIT_WIDE : CLOCK_DIGIT_NARROW;

    char time_buf[8];
    uint32_t now = _rtc->getCurrentTime();
    int16_t tz = _prefs ? _prefs->tz_offset_mins : 0;
    formatTime(now, tz, time_buf);

    char date_buf[24] = "";
    int header_h = 0;
    if (now >= 1000000000UL) {
      formatClockDate(now, tz, _prefs ? _prefs->ui_lang : UI_LANG_RU, date_buf, sizeof(date_buf));
      int topd, botd;
      clockInk(CLOCK_HEADER, date_buf, topd, botd);
      int inkd = botd - topd;
      int y = 4 - topd;
      if (y < 1) y = 1;
      clockDraw(display, CLOCK_HEADER, (fw - clockWidth(CLOCK_HEADER, date_buf)) / 2, y, date_buf);
      header_h = inkd + 8;
    }

    char line1[24] = "";
    char line2[24] = "";
    int groups = _preview->groupCount();
    int chats = _preview->chatCount();
    bool en = _prefs && _prefs->ui_lang == UI_LANG_EN;
    if (groups > 0) sprintf(line1, en ? "Groups: %d" : "Группы: %d", groups);
    if (chats > 0) sprintf(line2, en ? "Chat: %d" : "Чаты: %d", chats);
    bool two = line1[0] && line2[0];
    char both[40];
    if (two) sprintf(both, "%s    %s", line1, line2);

    int footer_h = 0;
    if (line1[0] || line2[0]) {
      const char* shown = two ? both : (line1[0] ? line1 : line2);
      bool stack = two && clockWidth(CLOCK_FOOTER, both) > fw - 8;
      int top0, bot0, top1 = 0, bot1 = 0;
      clockInk(CLOCK_FOOTER, stack ? line1 : shown, top0, bot0);
      int ink = bot0 - top0;
      if (stack) {
        clockInk(CLOCK_FOOTER, line2, top1, bot1);
        ink += 4 + (bot1 - top1);
      }
      int y = fh - 6 - ink;
      if (!stack) {
        clockDraw(display, CLOCK_FOOTER, (fw - clockWidth(CLOCK_FOOTER, shown)) / 2, y - top0, shown);
      } else {
        clockDraw(display, CLOCK_FOOTER, (fw - clockWidth(CLOCK_FOOTER, line1)) / 2, y - top0, line1);
        int y2 = y + (bot0 - top0) + 4;
        clockDraw(display, CLOCK_FOOTER, (fw - clockWidth(CLOCK_FOOTER, line2)) / 2, y2 - top1, line2);
      }
      footer_h = ink + 12;
    }

    int top, bot;
    clockInk(digits, time_buf, top, bot);
    int ink = bot - top;
    int area = fh - footer_h - header_h;
    int y = header_h + (area - ink) / 2 - top;
    if (y < 2) y = 2;
    clockDraw(display, digits, (fw - clockWidth(digits, time_buf)) / 2, y, time_buf);

    if (now < 1000000000UL) return 60000;
    uint32_t sec = now % 60UL;
    int wait = (int)((60UL - sec) * 1000UL);
    if (wait < 1000) wait = 1000;
    return wait;
  }

  bool handleInput(char c) override {
    if (c == KEY_ENTER && _preview->reopen()) {
      _task->showMsgPreview();
      return true;
    }
    if (_preview->unreadCount() > 0) _task->showMsgPreview();
    else _task->gotoHomeScreen();
    return true;
  }
};

bool UITask::reopenMsgPreview() {
  if (!((MsgPreviewScreen *) msg_preview)->reopen()) return false;
  showMsgPreview();
  return true;
}

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
  clock = (display != NULL && display->isEink())
      ? new ClockScreen(this, &rtc_clock, (MsgPreviewScreen*)msg_preview, node_prefs) : NULL;
  _idle_since = millis();
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


int UITask::getDeviceStored() const {
  if (!msg_preview) return 0;
  return ((MsgPreviewScreen *) msg_preview)->storedCount();
}

void UITask::clearTakenMsg() {
  if (!msg_preview) return;
  bool empty = ((MsgPreviewScreen *) msg_preview)->dropOldest();
  if (empty && curr == msg_preview) {
    gotoHomeScreen();
  } else if (curr == msg_preview || curr == clock || curr == home) {
    _next_refresh = 100;
  }
}

void UITask::msgRead(int msgcount) {
  _msgcount = msgcount;
  // The phone drains the offline queue as soon as a message arrives. That must
  // not dismiss the e-ink clock; the footer count already updated in newMsg.
  if (msgcount == 0 && curr != clock) {
    gotoHomeScreen();
  } else if (curr == home) {
    _next_refresh = 100;
  }
}

void UITask::newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount, bool group) {
  _msgcount = msgcount;

  ((MsgPreviewScreen *) msg_preview)->addPreview(path_len, from_name, text, group);
  if (curr == clock) {
    _next_refresh = 100;
  } else {
    setCurrScreen(msg_preview);
  }

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
  if (c == home) _idle_since = millis();
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
    _idle_since = millis();
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

  if (_display != NULL && _display->isEink() && clock != NULL && curr == home) {
    if (((HomeScreen*)home)->allowsClock() && (long)(millis() - _idle_since) >= 60000L) {
      _display->clean();
      setCurrScreen(clock);
    }
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
  if (curr == home || curr == msg_preview) return c;
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
