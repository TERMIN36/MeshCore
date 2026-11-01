#pragma once

#include <stdint.h>
#include "BatteryScale.h"

// Low-battery deep sleep: prefs gate + board default threshold.
#ifndef BATT_SHUTDOWN_MIN_MV
#define BATT_SHUTDOWN_MIN_MV 500
#endif

inline uint16_t boardDefaultBattShutdownMv() {
#if defined(AUTO_SHUTDOWN_MILLIVOLTS)
  return (uint16_t)AUTO_SHUTDOWN_MILLIVOLTS;
#elif defined(BATT_MIN_MILLIVOLTS)
  return (uint16_t)BATT_MIN_MILLIVOLTS;
#else
  return boardBattMinMv();
#endif
}

inline uint16_t effectiveBattShutdownMv(uint16_t prefs_mv) {
  if (prefs_mv != 0) return prefs_mv;
  return boardDefaultBattShutdownMv();
}

// milliVolts must already be a real pack/cell reading (not USB float noise).
inline bool shouldShutdownForLowBattery(uint16_t milliVolts, uint16_t threshold_mv) {
  if (milliVolts <= (uint16_t)BATT_SHUTDOWN_MIN_MV) return false;
  if (threshold_mv == 0) return false;
  if (milliVolts >= threshold_mv) return false;
#if defined(BATT_MIN_MILLIVOLTS)
  // Far below the pack floor is a floating ADC (USB-only), not an empty pack.
  if (milliVolts < (uint16_t)(BATT_MIN_MILLIVOLTS / 2)) return false;
#endif
  return true;
}

inline bool shouldAutoBattShutdown(uint8_t en, uint16_t prefs_mv, uint16_t milliVolts) {
  if (!en) return false;
  return shouldShutdownForLowBattery(milliVolts, effectiveBattShutdownMv(prefs_mv));
}
