#pragma once
// Applied via CCFLAGS -include (not PLATFORMIO_BUILD_FLAGS / -D...(PinMode)):
// those go through /bin/sh on Linux and break with "Syntax error: ( unexpected".
#define RADIOLIB_ARDUINOHAL_PIN_MODE_CAST (PinMode)
#define RADIOLIB_ARDUINOHAL_PIN_STATUS_CAST (PinStatus)
#define RADIOLIB_ARDUINOHAL_INTERRUPT_MODE_CAST (PinStatus)
