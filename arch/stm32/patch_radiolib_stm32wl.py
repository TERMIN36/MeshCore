Import("env")

import os

# This RadioLib pin still calls ::pinMode/::digitalWrite with uint32_t.
# stm32duino wants PinMode/PinStatus. Newer RadioLib already inserts the casts.
path = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"),
    env.subst("$PIOENV"),
    "RadioLib", "src", "hal", "Stm32duino", "Stm32wlHal.cpp",
)
if os.path.isfile(path):
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    # Cast in source (not via -D...(PinMode) build_flags): PlatformIO on Linux
    # passes those flags through /bin/sh and fails with "Syntax error: ( unexpected".
    updated = text.replace(
        "::pinMode(dwPin, dwMode);",
        "::pinMode(dwPin, (PinMode)dwMode);",
    ).replace(
        "::digitalWrite(dwPin, dwVal);",
        "::digitalWrite(dwPin, (PinStatus)dwVal);",
    ).replace(
        "::pinMode(dwPin, RADIOLIB_ARDUINOHAL_PIN_MODE_CAST dwMode);",
        "::pinMode(dwPin, (PinMode)dwMode);",
    ).replace(
        "::digitalWrite(dwPin, RADIOLIB_ARDUINOHAL_PIN_STATUS_CAST dwVal);",
        "::digitalWrite(dwPin, (PinStatus)dwVal);",
    )
    if updated != text:
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(updated)
        print("patched", path)
