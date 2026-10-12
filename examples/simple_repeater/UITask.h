#pragma once

#include <helpers/ui/DisplayDriver.h>
#include <helpers/CommonCLI.h>

class UITask {
  mesh::MainBoard* _board;
  DisplayDriver* _display;
  unsigned long _next_read, _next_refresh, _auto_off;
  int _prevBtnState;
  NodePrefs* _node_prefs;
  char _version_info[32];
  unsigned long _powering_off_at = 0;
  unsigned long _started_at = 0;
  unsigned long _awake_until = 0;
  uint8_t _page = 0;
  // A pad left in RTC mode reads held across a software reset. Ignore it
  // until a release, or the boot screen ends in a power-off restart.
  bool _btn_live = false;
  unsigned long _restart_after = 0;
  bool _restart_notice = false;

  void renderCurrScreen();
  void renderStatusScreen();
  void renderRadioScreen();
  void renderWifiScreen();
  void renderBatteryIndicator();
  void showNextPage();
public:
  UITask(mesh::MainBoard& board, DisplayDriver& display) : _board(&board), _display(&display) { _next_read = _next_refresh = 0; }
  void begin(NodePrefs* node_prefs, const char* build_date, const char* firmware_version);

  void pollButton();
  void loop();
  // Shown once, just before a restart that drops the CPU back to the idle clock.
  void showRestartNotice();
  // Screen refresh and the click window need the CPU. Off-screen idle can sleep.
  bool keepsCpuAwake() const;
};