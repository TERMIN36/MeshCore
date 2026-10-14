#pragma once

#include <MeshCore.h>
#include <Arduino.h>
#include <helpers/NRF52Board.h>
#include <helpers/RefCountedDigitalPin.h>
#include "LoRaFEMControl.h"

// Default battery voltage divider multiplier (adc * MV_LSB * cal).
// Can be overridden at build time and at runtime from the Battery page (stored in /batt.cal).
#ifndef T096_BATT_CAL
  #define T096_BATT_CAL 4.9f
#endif

class T096Board : public NRF52BoardDCDC {
protected:
#ifdef NRF52_POWER_MANAGEMENT
  void initiateShutdown(uint8_t reason) override;
#endif
  void variant_shutdown();

  float batt_cal;

public:
  RefCountedDigitalPin periph_power;
  LoRaFEMControl loRaFEMControl;

  T096Board() :periph_power(PIN_VEXT_EN,PIN_VEXT_EN_ACTIVE), NRF52Board("T096_OTA"), batt_cal(T096_BATT_CAL) {}
  void begin();

  void onBeforeTransmit(void) override;
  void onAfterTransmit(void) override;
  uint16_t getBattMilliVolts() override;
  float battCal() const { return batt_cal; }
  void setBattCal(float cal) {
    if (!(cal >= 1.0f)) cal = 1.0f;   // also catches NaN
    if (cal > 9.99f) cal = 9.99f;
    batt_cal = cal;
  }
  const char* getManufacturerName() const override ;
  void powerOff() override;
  bool setLoRaFemLnaEnabled(bool enable) override;
  bool canControlLoRaFemLna() const override;
  bool isLoRaFemLnaEnabled() const override;
};
