#pragma once

#include <stdint.h>

// Stable 0..10 battery steps for the on-screen indicator.
// A short window is reduced to its median; samples far from that median
// are dropped and the rest are averaged. The averaged voltage is then
// latched to a step, and a step only changes after the reading has moved
// past the boundary by HYSTERESIS_MV. A bounce such as 5/10 -> 6/10 -> 5/10
// therefore does not redraw the icon.
class BatteryLevelFilter {
public:
  static const int WINDOW = 5;
  static const int LEVELS = 10;
  static const int OUTLIER_MV = 80;
  static const int HYSTERESIS_MV = 40;

  BatteryLevelFilter() : _count(0), _idx(0), _level(-1) {
    for (int i = 0; i < WINDOW; i++) _samples[i] = 0;
  }

  int push(uint16_t mv, int min_mv, int max_mv) {
    if (max_mv <= min_mv) return 0;

    _samples[_idx] = mv;
    _idx = (uint8_t)((_idx + 1) % WINDOW);
    if (_count < WINDOW) _count++;

    uint16_t ordered[WINDOW];
    for (int i = 0; i < _count; i++) ordered[i] = _samples[i];
    for (int i = 1; i < _count; i++) {
      uint16_t v = ordered[i];
      int j = i;
      while (j > 0 && ordered[j - 1] > v) {
        ordered[j] = ordered[j - 1];
        j--;
      }
      ordered[j] = v;
    }
    uint16_t median = ordered[_count / 2];

    uint32_t sum = 0;
    int kept = 0;
    for (int i = 0; i < _count; i++) {
      int delta = (int)_samples[i] - (int)median;
      if (delta < 0) delta = -delta;
      if (delta <= OUTLIER_MV) {
        sum += _samples[i];
        kept++;
      }
    }
    int smoothed = kept ? (int)(sum / (uint32_t)kept) : (int)median;

    int span = max_mv - min_mv;
    int raw = (smoothed - min_mv) * LEVELS / span;
    if (raw < 0) raw = 0;
    if (raw > LEVELS) raw = LEVELS;

    if (_level < 0) {
      _level = (int8_t)raw;
    } else if (raw > _level) {
      int boundary = min_mv + (_level + 1) * span / LEVELS;
      if (smoothed >= boundary + HYSTERESIS_MV) _level = (int8_t)raw;
    } else if (raw < _level) {
      int boundary = min_mv + _level * span / LEVELS;
      if (smoothed <= boundary - HYSTERESIS_MV) _level = (int8_t)raw;
    }
    return _level;
  }

  int level() const { return _level < 0 ? 0 : _level; }

  // Millivolts at the bottom of the latched step. Stable until the step changes.
  uint16_t displayMilliVolts(int min_mv, int max_mv) const {
    if (max_mv <= min_mv) return (uint16_t)min_mv;
    return (uint16_t)(min_mv + level() * (max_mv - min_mv) / LEVELS);
  }

private:
  uint16_t _samples[WINDOW];
  uint8_t _count;
  uint8_t _idx;
  int8_t _level;
};
