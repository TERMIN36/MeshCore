Import("env")

import os

# Provide PinMode/PinStatus casts without -D...(PinMode) in PLATFORMIO_BUILD_FLAGS
# (that env path breaks Linux CI with "Syntax error: ( unexpected").
casts = os.path.join(env.subst("$PROJECT_DIR"), "arch", "stm32", "radiolib_pin_casts.h")
env.Append(CCFLAGS=["-include", casts])

# Older RadioLib Stm32wlHal still calls ::pinMode/::digitalWrite with uint32_t.
# Newer RadioLib uses RADIOLIB_ARDUINOHAL_*_CAST (covered by the -include above).
path = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"),
    env.subst("$PIOENV"),
    "RadioLib", "src", "hal", "Stm32duino", "Stm32wlHal.cpp",
)
if os.path.isfile(path):
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    updated = text.replace(
        "::pinMode(dwPin, dwMode);",
        "::pinMode(dwPin, (PinMode)dwMode);",
    ).replace(
        "::digitalWrite(dwPin, dwVal);",
        "::digitalWrite(dwPin, (PinStatus)dwVal);",
    )
    if updated != text:
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(updated)
        print("patched", path)
