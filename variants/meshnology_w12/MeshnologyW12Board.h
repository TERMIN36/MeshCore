#pragma once

#include <Arduino.h>
#include <helpers/RefCountedDigitalPin.h>
#include <helpers/ESP32Board.h>
#include <driver/rtc_io.h>

#ifndef ADC_MULTIPLIER
  #define ADC_MULTIPLIER 5.42
#endif

class MeshnologyW12Board : public ESP32Board {

public:
  RefCountedDigitalPin periph_power;
  MeshnologyW12Board() : periph_power(PIN_VEXT_EN, PIN_VEXT_EN_ACTIVE) {
    initAdcMultiplier(ADC_MULTIPLIER);
  }

  void begin();
  void onBeforeTransmit(void) override;
  void onAfterTransmit(void) override;
  void enterDeepSleep(uint32_t secs, int pin_wake_btn = -1);
  void powerOff() override;
  uint16_t getBattMilliVolts() override;
  const char* getManufacturerName() const override;
};
