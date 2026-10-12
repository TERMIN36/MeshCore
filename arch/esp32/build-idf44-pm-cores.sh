#!/usr/bin/env bash
# Build Arduino-ESP32 2.0.17 libraries for classic ESP32 and ESP32-C3
# with power management, then pack a PlatformIO framework for each chip.
# ESP32-S3 is built by build-s3-pm-core.sh and is not touched here.
#
# Run on Linux or WSL2, from anywhere:
#   bash arch/esp32/build-idf44-pm-cores.sh
#
# Reuses the ESP-IDF checkout from build-s3-pm-core.sh when it is present.
# Output (gitignored):
#   .cache/framework-arduinoespressif32-c3-pm/
#   .cache/framework-arduinoespressif32-esp32-pm/

set -euo pipefail
trap 'echo IDF44_PM_CORE_FAILED' ERR

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORKDIR="${WORKDIR:-$HOME/meshcore-s3-core-build}"
JOBS="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
LOG="$ROOT/.cache/idf44-pm-core-build.log"
LIB_BUILDER="$WORKDIR/esp32-arduino-lib-builder"
ARDUINO="$WORKDIR/arduino-esp32"

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

copy_tree() {
  local src="$1"
  local dest="$2"
  mkdir -p "$dest"
  tar -C "$src" --exclude .git --exclude .github -cf - . | tar -C "$dest" -xf -
}

find_stock() {
  local pio_packages="${PIO_PACKAGES:-/mnt/c/Users/termi/.platformio/packages}"
  if [ -n "${STOCK_FRAMEWORK:-}" ] && [ -d "$STOCK_FRAMEWORK/cores/esp32" ]; then
    echo "$STOCK_FRAMEWORK"
    return
  fi
  local candidate
  for candidate in "$pio_packages"/framework-arduinoespressif32*; do
    if [ -d "$candidate/cores/esp32" ]; then
      echo "$candidate"
      return
    fi
  done
}

apply_fragment() {
  local target="$1"
  local fragment="$2"
  local defconfig="$LIB_BUILDER/configs/defconfig.${target}"
  git -C "$LIB_BUILDER" checkout -- "configs/defconfig.${target}"
  printf '\n' >> "$defconfig"
  cat "$fragment" >> "$defconfig"
  echo "defconfig.${target} tail:"
  tail -n 16 "$defconfig"
}

verify_names() {
  local header="$1"
  shift
  local name
  for name in "$@"; do
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

pack_chip() {
  local target="$1"
  local out="$2"
  local description="$3"
  shift 3
  local header
  mapfile -t headers < <(find "$ARDUINO/tools/sdk/${target}" -name sdkconfig.h | sort)
  if [ "${#headers[@]}" -eq 0 ]; then
    echo "No sdkconfig.h under tools/sdk/${target}" >&2
    exit 1
  fi
  for header in "${headers[@]}"; do
    verify_names "$header" "$@"
  done

  local stock
  stock="$(find_stock || true)"
  rm -rf "$out"
  mkdir -p "$out"
  if [ -n "$stock" ]; then
    echo "Base framework: $stock"
    copy_tree "$stock" "$out"
    rm -rf "$out/tools/sdk/${target}"
    mkdir -p "$out/tools/sdk/${target}"
    copy_tree "$ARDUINO/tools/sdk/${target}" "$out/tools/sdk/${target}"
  else
    echo "PlatformIO framework package not found; packing the 2.0.17 checkout"
    copy_tree "$ARDUINO" "$out"
  fi

  if [ ! -f "$out/package.json" ]; then
    cat > "$out/package.json" <<EOF
{
  "name": "framework-arduinoespressif32",
  "version": "3.20017.0",
  "description": "${description}",
  "keywords": ["framework", "arduino", "espressif", "esp32"],
  "license": "LGPL-2.1-or-later"
}
EOF
  fi

  for header in "${headers[@]}"; do
    local rel="${header#"$ARDUINO/"}"
    verify_names "$out/$rel" "$@"
  done

  local tarball="${out}.tar.gz"
  rm -f "$tarball"
  tar -C "$(dirname "$out")" -czf "$tarball" "$(basename "$out")"
  echo "IDF44_PM_CORE_READY $out"
}

if [ -f "$LIB_BUILDER/esp-idf/export.sh" ]; then
  # -s skips install-esp-idf.sh, which is what puts idf.py on PATH.
  set +u
  source "$LIB_BUILDER/esp-idf/export.sh"
  set -u
fi

build_target() {
  local target="$1"
  local fragment="$2"
  local stamp="$ARDUINO/.${target}-pm-core-built"
  if [ "${REBUILD:-0}" != "1" ] && [ -f "$stamp" ]; then
    echo "Libraries already built ($stamp). Set REBUILD=1 to compile again."
    return
  fi
  apply_fragment "$target" "$fragment"
  cd "$LIB_BUILDER"
  local build_args=(-t "$target" -A 2.0.17 -I v4.4.7 -c "$ARDUINO")
  if [ -d "$LIB_BUILDER/esp-idf/components/mbedtls/mbedtls/include" ]; then
    build_args=(-s "${build_args[@]}")
  fi
  ./build.sh "${build_args[@]}"
  touch "$stamp"
}

build_target esp32c3 "$ROOT/arch/esp32/sdkconfig.defaults.esp32c3"
pack_chip esp32c3 \
  "$ROOT/.cache/framework-arduinoespressif32-c3-pm" \
  "Arduino-ESP32 2.0.17 with ESP32-C3 Bluetooth modem sleep and PM" \
  CONFIG_PM_ENABLE \
  CONFIG_FREERTOS_USE_TICKLESS_IDLE \
  CONFIG_BT_CTRL_MODEM_SLEEP \
  CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1 \
  CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL \
  CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP

build_target esp32 "$ROOT/arch/esp32/sdkconfig.defaults.esp32"
pack_chip esp32 \
  "$ROOT/.cache/framework-arduinoespressif32-esp32-pm" \
  "Arduino-ESP32 2.0.17 with classic ESP32 PM and ORIG modem sleep" \
  CONFIG_PM_ENABLE \
  CONFIG_FREERTOS_USE_TICKLESS_IDLE \
  CONFIG_BTDM_CTRL_MODEM_SLEEP \
  CONFIG_BTDM_CTRL_MODEM_SLEEP_MODE_ORIG \
  CONFIG_BTDM_CTRL_LPCLK_SEL_MAIN_XTAL

echo "IDF44_PM_CORES_READY"
