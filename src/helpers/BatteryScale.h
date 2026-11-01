#pragma once

#include <stdint.h>

// Battery scale for UI %, cal adc, and low-batt shutdown.
// On 2S boards (e.g. T-Beam 1W) getBattMilliVolts() is pack voltage after the
// divider. Calibrate with a multimeter on the same node the divider senses
// (pack terminals), not a single cell, when the UI shows pack mV.
//
// Macros only — safe to include from MeshCore.h (no Arduino / MeshCore deps).

#ifndef BATT_SCALE_MIN_MV_DEFAULT
#define BATT_SCALE_MIN_MV_DEFAULT 3000
#endif
#ifndef BATT_SCALE_MAX_MV_DEFAULT
#define BATT_SCALE_MAX_MV_DEFAULT 4200
#endif
#ifndef BATT_CAL_ADC_MIN_MV
#define BATT_CAL_ADC_MIN_MV 500
#endif
#ifndef BATT_CAL_ADC_MAX_MV_FLOOR
#define BATT_CAL_ADC_MAX_MV_FLOOR 5000
#endif

inline uint16_t boardBattMinMv() {
#if defined(BATT_MIN_MILLIVOLTS)
  return (uint16_t)BATT_MIN_MILLIVOLTS;
#else
  return (uint16_t)BATT_SCALE_MIN_MV_DEFAULT;
#endif
}

inline uint16_t boardBattMaxMv() {
#if defined(BATT_MAX_MILLIVOLTS)
  return (uint16_t)BATT_MAX_MILLIVOLTS;
#else
  return (uint16_t)BATT_SCALE_MAX_MV_DEFAULT;
#endif
}

inline uint16_t boardCalAdcMinMv() {
  return (uint16_t)BATT_CAL_ADC_MIN_MV;
}

inline uint16_t boardCalAdcMaxMv() {
  uint16_t max_mv = boardBattMaxMv();
  if (max_mv < (uint16_t)BATT_CAL_ADC_MAX_MV_FLOOR) max_mv = (uint16_t)BATT_CAL_ADC_MAX_MV_FLOOR;
  return max_mv;
}
