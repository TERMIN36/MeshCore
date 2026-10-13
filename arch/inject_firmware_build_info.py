# Inject release version/date without PLATFORMIO_BUILD_FLAGS.
# Setting that env var forces PlatformIO through /bin/sh and breaks STM32
# build_flags that contain (PinMode)/(PinStatus) casts.
Import("env")

import os

proj = env.subst("$PROJECT_DIR")
hdr = os.path.join(proj, ".pio", "FirmwareBuildInfo.h")
if os.path.isfile(hdr):
    env.Append(CCFLAGS=["-include", hdr])

if os.environ.get("DISABLE_DEBUG") == "1":
    env.Append(
        CCFLAGS=[
            "-UMESH_DEBUG",
            "-UBLE_DEBUG_LOGGING",
            "-UWIFI_DEBUG_LOGGING",
            "-UBRIDGE_DEBUG",
            "-UGPS_NMEA_DEBUG",
            "-UCORE_DEBUG_LEVEL",
            "-UESPNOW_DEBUG_LOGGING",
            "-UDEBUG_RP2040_WIRE",
            "-UDEBUG_RP2040_SPI",
            "-UDEBUG_RP2040_CORE",
            "-UDEBUG_RP2040_PORT",
            "-URADIOLIB_DEBUG_SPI",
            "-UCFG_DEBUG",
            "-URADIOLIB_DEBUG_BASIC",
            "-URADIOLIB_DEBUG_PROTOCOL",
        ]
    )
