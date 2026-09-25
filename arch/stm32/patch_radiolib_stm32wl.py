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
    updated = text.replace(
        "::pinMode(dwPin, dwMode);",
        "::pinMode(dwPin, RADIOLIB_ARDUINOHAL_PIN_MODE_CAST dwMode);",
    ).replace(
        "::digitalWrite(dwPin, dwVal);",
        "::digitalWrite(dwPin, RADIOLIB_ARDUINOHAL_PIN_STATUS_CAST dwVal);",
    )
    if updated != text:
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(updated)
        print("patched", path)
