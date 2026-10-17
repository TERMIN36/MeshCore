#pragma once

#include <Arduino.h>
#include <driver/rtc_io.h>
#include <helpers/ESP32Board.h>
#include <helpers/RefCountedDigitalPin.h>

#ifndef ADC_MULTIPLIER
  #define ADC_MULTIPLIER 4.9f
#endif

class HeltecRC32Board : public ESP32Board {
public:
  RefCountedDigitalPin periph_power;

  HeltecRC32Board() : periph_power(SENSOR_POWER_CTRL_PIN, SENSOR_POWER_ON) {
    initAdcMultiplier(ADC_MULTIPLIER);
  }

  void begin();
  void onBeforeTransmit() override;
  void onAfterTransmit() override;
  void powerOff() override;
  uint16_t getBattMilliVolts() override;
  const char* getManufacturerName() const override;
};
