#!/usr/bin/env bash
# Build Arduino-ESP32 2.0.17 ESP32-S3 libraries with Bluetooth modem sleep
# and power management, then pack a PlatformIO framework once.
#
# Run on Linux or WSL2, from anywhere:
#   bash arch/esp32/build-s3-pm-core.sh
#
# The firmware CI does not run this. It only downloads the packed framework.
# Rebuild only when this fragment or the Arduino / IDF pin changes.
#
# Output (gitignored):
#   .cache/framework-arduinoespressif32-s3-pm/
#   .cache/framework-arduinoespressif32-s3-pm.tar.gz

set -euo pipefail
trap 'echo S3_PM_CORE_FAILED' ERR

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FRAGMENT="$ROOT/arch/esp32/sdkconfig.defaults.esp32s3"
WORKDIR="${WORKDIR:-$HOME/meshcore-s3-core-build}"
OUT="${OUT:-$ROOT/.cache/framework-arduinoespressif32-s3-pm}"
JOBS="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
LOG="$ROOT/.cache/s3-pm-core-build.log"
LIB_BUILDER="$WORKDIR/esp32-arduino-lib-builder"
ARDUINO="$WORKDIR/arduino-esp32"
STAMP="$ARDUINO/.s3-pm-core-built"

mkdir -p "$ROOT/.cache" "$HOME/bin"
exec > >(tee -a "$LOG") 2>&1

echo "Work directory: $WORKDIR"
echo "Parallel jobs: $JOBS"

export PATH="$HOME/bin:$PATH"
export CMAKE_BUILD_PARALLEL_LEVEL="$JOBS"
export IDF_CCACHE_ENABLE=1
export GIT_TERMINAL_PROMPT=0

if ! command -v jq >/dev/null 2>&1; then
  echo "Fetching jq (no root required)"
  curl -fsSL -o "$HOME/bin/jq" \
    https://github.com/jqlang/jq/releases/download/jq-1.7.1/jq-linux-amd64
  chmod +x "$HOME/bin/jq"
fi

if ! command -v pkg-config >/dev/null 2>&1; then
  echo "Fetching pkg-config (no root required)"
  mkdir -p "$WORKDIR/deps"
  (cd "$WORKDIR/deps" && apt-get download pkg-config)
  rm -rf "$WORKDIR/deps/root"
  dpkg-deb -x "$WORKDIR/deps"/pkg-config_*.deb "$WORKDIR/deps/root"
  export PATH="$WORKDIR/deps/root/usr/bin:$PATH"
fi

python3 -c "import distutils" 2>/dev/null || {
  echo "python3 distutils is missing. On Ubuntu: sudo apt-get install python3-distutils" >&2
  exit 1
}

mkdir -p "$WORKDIR"

if [ ! -d "$LIB_BUILDER/.git" ]; then
  git clone --depth 1 --branch release/v4.4 \
    https://github.com/espressif/esp32-arduino-lib-builder.git "$LIB_BUILDER"
fi

if [ ! -d "$ARDUINO/.git" ]; then
  git clone --depth 1 --branch 2.0.17 \
    https://github.com/espressif/arduino-esp32.git "$ARDUINO"
fi

if [ "${REBUILD:-0}" = "1" ] || [ ! -f "$STAMP" ]; then
  cd "$LIB_BUILDER"
  git checkout -- configs/defconfig.esp32s3
  # The stock file has no trailing newline; appending would glue the first
  # fragment line onto CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH.
  printf '\n' >> configs/defconfig.esp32s3
  cat "$FRAGMENT" >> configs/defconfig.esp32s3
  echo "defconfig.esp32s3 tail:"
  tail -n 20 configs/defconfig.esp32s3

  # -A 2.0.17 is a tag. The builder then git-pulls that checkout and fails
  # because a tag is not a branch. Fetch and checkout, do not pull.
  python3 - "$LIB_BUILDER/tools/update-components.sh" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
start = text.find('if [ "$AR_BRANCH" ]; then')
if start < 0:
    raise SystemExit("AR_BRANCH block missing")
end = text.find("if [ $? -ne 0 ]; then exit 1; fi", start)
if end < 0:
    raise SystemExit("AR_BRANCH status check missing")
block = text[start:end]
if "pull --ff-only" not in block:
    print("arduino checkout already skips pull")
else:
    repl = '''if [ "$AR_BRANCH" ]; then
	git -C "$AR_COMPS/arduino" fetch --tags
	git -C "$AR_COMPS/arduino" checkout "$AR_BRANCH"
fi
'''
    path.write_text(text[:start] + repl + text[end:])
    print("patched arduino checkout to allow tag 2.0.17")
PY

  # release/v4.4 now git-pulls master of camera, littlefs, dsp and tinyusb.
  # Those masters require ESP-IDF 5. The 2.0.17 SDK was built from the commits
  # in framework-arduinoespressif32 tools/sdk/versions.txt.
  python3 - "$LIB_BUILDER/tools/update-components.sh" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
marker = "# meshcore-pin-arduino-2.0.17"
if marker not in text:
    text += f"""
{marker}
git -C "$AR_COMPS/esp32-camera" reset --hard f0bb429
git -C "$AR_COMPS/esp_littlefs" reset --hard 41873c2
git -C "$AR_COMPS/esp_littlefs" submodule update --init --recursive
git -C "$AR_COMPS/espressif__esp-dsp" reset --hard 9b4a8b4
git -C "$AR_COMPS/arduino_tinyusb/tinyusb" reset --hard a0e5626bc
"""
    path.write_text(text)
    print("pinned camera, littlefs, dsp and tinyusb to Arduino 2.0.17")
else:
    print("component pins already present")
PY

  proto="$LIB_BUILDER/esp-idf/components/protobuf-c/protobuf-c"
  if [ -d "$LIB_BUILDER/esp-idf/.git" ] && [ ! -f "$proto/protobuf-c/protobuf-c.h" ] && [ ! -f "$proto/protobuf-c.h" ]; then
    echo "Fetching protobuf-c submodule abc67a11"
    rm -rf "$proto"
    mkdir -p "$proto"
    git -C "$proto" init
    git -C "$proto" remote add origin https://github.com/protobuf-c/protobuf-c.git
    git -C "$proto" fetch --depth 1 origin abc67a11c6db271bedbb9f58be85d6f4e2ea8389
    git -C "$proto" checkout FETCH_HEAD
  fi

  # The first submodule update can abort and leave gitlinks with no files.
  # install-esp-idf.sh does not retry that once esp-idf/ already exists.
  if [ -d "$LIB_BUILDER/esp-idf/.git" ] && [ ! -d "$LIB_BUILDER/esp-idf/components/mbedtls/mbedtls/include" ]; then
    echo "Checking out ESP-IDF submodules"
    git -C "$LIB_BUILDER/esp-idf" submodule update --init --recursive
  fi

  build_args=(-t esp32s3 -A 2.0.17 -I v4.4.7 -c "$ARDUINO")
  if [ "${SKIP_ENV:-0}" = "1" ]; then
    build_args=(-s "${build_args[@]}")
  fi
  ./build.sh "${build_args[@]}"
  touch "$STAMP"
else
  echo "Libraries already built ($STAMP). Set REBUILD=1 to compile again."
fi

verify_header() {
  local header="$1"
  local name
  for name in \
    CONFIG_PM_ENABLE \
    CONFIG_FREERTOS_USE_TICKLESS_IDLE \
    CONFIG_BT_CTRL_MODEM_SLEEP \
    CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1 \
    CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL \
    CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP
  do
    if ! grep -q "#define ${name} 1" "$header"; then
      echo "Missing ${name} in ${header}" >&2
      exit 1
    fi
  done
  if grep -q "#define CONFIG_ESP_SLEEP_POWER_DOWN_FLASH 1" "$header"; then
    echo "Flash power-down is enabled in ${header}" >&2
    exit 1
  fi
  echo "sdkconfig ok: $header"
}

mapfile -t headers < <(find "$ARDUINO/tools/sdk/esp32s3" -name sdkconfig.h | sort)
if [ "${#headers[@]}" -eq 0 ]; then
  echo "No sdkconfig.h under tools/sdk/esp32s3" >&2
  exit 1
fi
for header in "${headers[@]}"; do
  verify_header "$header"
done

stock=""
pio_packages="${PIO_PACKAGES:-/mnt/c/Users/termi/.platformio/packages}"
if [ -n "${STOCK_FRAMEWORK:-}" ] && [ -d "$STOCK_FRAMEWORK/cores/esp32" ]; then
  stock="$STOCK_FRAMEWORK"
else
  for candidate in "$pio_packages"/framework-arduinoespressif32*; do
    if [ -d "$candidate/cores/esp32" ]; then
      stock="$candidate"
      break
    fi
  done
fi

copy_tree() {
  local src="$1"
  local dest="$2"
  mkdir -p "$dest"
  tar -C "$src" --exclude .git --exclude .github -cf - . | tar -C "$dest" -xf -
}

rm -rf "$OUT"
mkdir -p "$OUT"
if [ -n "$stock" ]; then
  echo "Base framework: $stock"
  copy_tree "$stock" "$OUT"
  rm -rf "$OUT/tools/sdk/esp32s3"
  mkdir -p "$OUT/tools/sdk/esp32s3"
  copy_tree "$ARDUINO/tools/sdk/esp32s3" "$OUT/tools/sdk/esp32s3"
else
  echo "PlatformIO framework package not found; packing the 2.0.17 checkout"
  copy_tree "$ARDUINO" "$OUT"
fi

if [ ! -f "$OUT/package.json" ]; then
  cat > "$OUT/package.json" <<'EOF'
{
  "name": "framework-arduinoespressif32",
  "version": "3.20017.0",
  "description": "Arduino-ESP32 2.0.17 with ESP32-S3 Bluetooth modem sleep and PM",
  "keywords": ["framework", "arduino", "espressif", "esp32"],
  "license": "LGPL-2.1-or-later"
}
EOF
fi

for header in "${headers[@]}"; do
  rel="${header#"$ARDUINO/"}"
  verify_header "$OUT/$rel"
done

tarball="${OUT}.tar.gz"
rm -f "$tarball"
tar -C "$(dirname "$OUT")" -czf "$tarball" "$(basename "$OUT")"
echo "S3_PM_CORE_READY $OUT"
echo "S3_PM_CORE_TARBALL $tarball"
