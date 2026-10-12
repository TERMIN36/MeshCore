#!/usr/bin/env bash
# Build Arduino-ESP32 3.1.3 ESP32-C6 libraries with BLE modem sleep and
# power management, then pack a PlatformIO libs package.
#
# Run on Linux or WSL2, from anywhere:
#   bash arch/esp32/build-c6-pm-core.sh
#
# The C6 boards use pioarduino 53.03.13, which loads prebuilt libs from
# esp32-arduino-lib-builder idf-release/v5.3 (Arduino 3.1.3, IDF 5.3.2).
# This script rebuilds only the ESP32-C6 slice and overlays it on that zip.
#
# Output (gitignored):
#   .cache/framework-arduinoespressif32-c6-pm/

set -euo pipefail
trap 'echo C6_PM_CORE_FAILED' ERR

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FRAGMENT="$ROOT/arch/esp32/sdkconfig.defaults.esp32c6"
WORKDIR="${WORKDIR:-$HOME/meshcore-c6-core-build}"
OUT="${OUT:-$ROOT/.cache/framework-arduinoespressif32-c6-pm}"
JOBS="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
LOG="$ROOT/.cache/c6-pm-core-build.log"
LIB_BUILDER="$WORKDIR/esp32-arduino-lib-builder"
STAMP="$WORKDIR/.c6-pm-core-built"
LIBS_ZIP_URL="https://github.com/espressif/esp32-arduino-lib-builder/releases/download/idf-release_v5.3/esp32-arduino-libs-idf-release_v5.3-489d7a2b-v1.zip"
LIBS_ZIP="$ROOT/.cache/esp32-arduino-libs-idf-release_v5.3-489d7a2b-v1.zip"

mkdir -p "$ROOT/.cache" "$HOME/bin" "$WORKDIR"
exec > >(tee -a "$LOG") 2>&1

echo "Work directory: $WORKDIR"
echo "Parallel jobs: $JOBS"

export PATH="$HOME/bin:$PATH"
export CMAKE_BUILD_PARALLEL_LEVEL="$JOBS"
export IDF_CCACHE_ENABLE=1
export GIT_TERMINAL_PROMPT=0

# pioarduino 53.03.13 loads libs from the idf-release_v5.3 asset published
# 2024-08-19. The tip of release/v5.3 has a newer tflite that this IDF
# does not compile. Stay on the commit that produced that asset.
pin_builder() {
  # Named branch required: detached HEAD breaks the builder's IDF/tool install
  # ("fatal: ref HEAD is not a symbolic ref") and later component resolution.
  git -C "$LIB_BUILDER" fetch --shallow-since=2024-08-01 origin release/v5.3
  local pin
  pin="$(git -C "$LIB_BUILDER" rev-list -n 1 --before=2024-08-20 origin/release/v5.3)"
  if [ -z "$pin" ]; then
    echo "No release/v5.3 commit before 2024-08-20" >&2
    exit 1
  fi
  echo "Pinning lib-builder to $pin (branch meshcore-c6-pin)"
  git -C "$LIB_BUILDER" checkout --force -B meshcore-c6-pin "$pin"
}

if [ ! -d "$LIB_BUILDER/.git" ]; then
  git clone --branch release/v5.3 --depth 1 \
    https://github.com/espressif/esp32-arduino-lib-builder.git "$LIB_BUILDER"
fi
pin_builder

apply_fragment() {
  local defconfig=""
  local candidate
  for candidate in \
    "$LIB_BUILDER/configs/defconfig.esp32c6" \
    "$LIB_BUILDER/configs/esp32c6/sdkconfig.defaults" \
    "$LIB_BUILDER/config/defconfig.esp32c6"
  do
    if [ -f "$candidate" ]; then
      defconfig="$candidate"
      break
    fi
  done
  if [ -z "$defconfig" ]; then
    echo "ESP32-C6 defconfig not found under $LIB_BUILDER" >&2
    find "$LIB_BUILDER" -maxdepth 3 -iname '*esp32c6*' -o -name 'sdkconfig*' | head -50 >&2
    exit 1
  fi
  echo "Using defconfig: $defconfig"
  git -C "$LIB_BUILDER" checkout -- "${defconfig#"$LIB_BUILDER/"}" || true
  printf '\n' >> "$defconfig"
  cat "$FRAGMENT" >> "$defconfig"
  echo "defconfig tail:"
  tail -n 16 "$defconfig"
}

# ESP-IDF 5.3's Bluetooth CMake uses CMAKE_CURRENT_FUNCTION_LIST_DIR.
# The toolchain slot installed here is CMake 3.16, which leaves that
# variable empty and then looks for /lib_esp32c6/esp32c6-bt-lib/libble_app.a.
ensure_cmake() {
  local dest="$HOME/opt/cmake-3.30.5-linux-x86_64"
  if [ ! -x "$dest/bin/cmake" ]; then
    echo "Fetching CMake 3.30.5"
    local tar="$WORKDIR/cmake-3.30.5-linux-x86_64.tar.gz"
    curl -fL --retry 3 -o "$tar" \
      https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-linux-x86_64.tar.gz
    mkdir -p "$HOME/opt"
    tar -C "$HOME/opt" -xzf "$tar"
  fi
  local slot
  for slot in "$HOME"/.espressif/tools/cmake/*; do
    if [ -d "$slot" ] && [ ! -L "$slot" ]; then
      if ! "$slot/bin/cmake" --version 2>/dev/null | grep -q 'version 3\.30'; then
        echo "Replacing $slot with CMake 3.30.5"
        rm -rf "$slot"
        ln -s "$dest" "$slot"
      fi
    fi
  done
  export PATH="$dest/bin:$PATH"
}

# IDF 5.3 renamed CONFIG_MAC_BB_PD to CONFIG_ESP_PHY_MAC_BB_PD in Kconfig
# but the C6 controller still tests the old name, so light sleep does not
# compile. Rewrite the old name in the sources before the build.
patch_mac_bb_name() {
  local file
  while IFS= read -r file; do
    sed -i 's/CONFIG_MAC_BB_PD/CONFIG_ESP_PHY_MAC_BB_PD/g' "$file"
  done < <(grep -R -l 'CONFIG_MAC_BB_PD' "$LIB_BUILDER/esp-idf/components" \
    --include='*.c' --include='*.h' || true)
}

if [ "${REBUILD:-0}" = "1" ] || [ ! -f "$STAMP" ]; then
  ensure_cmake
  apply_fragment
  patch_mac_bb_name
  cd "$LIB_BUILDER"
  # The pinned builder fetches its own component set. Drop the tip's copies.
  rm -rf "$LIB_BUILDER/managed_components" "$LIB_BUILDER/build" "$LIB_BUILDER/sdkconfig"
  if [ -x ./build.sh ]; then
    ./build.sh -t esp32c6
  else
    echo "build.sh missing in $LIB_BUILDER" >&2
    exit 1
  fi
  touch "$STAMP"
else
  echo "Libraries already built ($STAMP). Set REBUILD=1 to compile again."
fi

chip_src=""
while IFS= read -r header; do
  dir="$(dirname "$header")"
  while [ "$dir" != "/" ] && [ "$(basename "$dir")" != "esp32c6" ]; do
    dir="$(dirname "$dir")"
  done
  if [ "$(basename "$dir")" = "esp32c6" ]; then
    chip_src="$dir"
    break
  fi
done < <(find "$LIB_BUILDER" "$WORKDIR" -path '*/esp32c6/*' -name sdkconfig.h 2>/dev/null | sort)

if [ -z "$chip_src" ]; then
  echo "Built ESP32-C6 sdkconfig.h not found" >&2
  exit 1
fi
echo "Built chip libs: $chip_src"

verify_header() {
  local header="$1"
  local name
  for name in \
    CONFIG_PM_ENABLE \
    CONFIG_FREERTOS_USE_TICKLESS_IDLE \
    CONFIG_BT_LE_SLEEP_ENABLE \
    CONFIG_BT_LE_LP_CLK_SRC_MAIN_XTAL \
    CONFIG_ESP_PHY_MAC_BB_PD
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

mapfile -t built_headers < <(find "$chip_src" -name sdkconfig.h | sort)
if [ "${#built_headers[@]}" -eq 0 ]; then
  echo "No sdkconfig.h under $chip_src" >&2
  exit 1
fi
for header in "${built_headers[@]}"; do
  verify_header "$header"
done

if [ ! -f "$LIBS_ZIP" ]; then
  echo "Downloading stock Arduino 3.1.3 libs"
  curl -fL --retry 3 -o "$LIBS_ZIP" "$LIBS_ZIP_URL"
fi

rm -rf "$OUT"
mkdir -p "$OUT"
unzip -q "$LIBS_ZIP" -d "$OUT"
# The zip may contain a single top-level directory. Flatten it when the
# chip folders are not already at the package root.
if [ ! -d "$OUT/esp32c6" ]; then
  mapfile -t tops < <(find "$OUT" -mindepth 1 -maxdepth 1 -type d)
  if [ "${#tops[@]}" -eq 1 ] && [ -d "${tops[0]}/esp32c6" ]; then
    tmp="$WORKDIR/c6-libs-flat"
    rm -rf "$tmp"
    mv "${tops[0]}" "$tmp"
    rm -rf "$OUT"
    mv "$tmp" "$OUT"
  fi
fi
if [ ! -d "$OUT/esp32c6" ]; then
  echo "Stock libs package has no esp32c6 directory" >&2
  find "$OUT" -maxdepth 2 -type d | head -40 >&2
  exit 1
fi

rm -rf "$OUT/esp32c6"
mkdir -p "$OUT/esp32c6"
tar -C "$chip_src" --exclude .git -cf - . | tar -C "$OUT/esp32c6" -xf -

mapfile -t packed_headers < <(find "$OUT/esp32c6" -name sdkconfig.h | sort)
if [ "${#packed_headers[@]}" -eq 0 ]; then
  echo "Packed ESP32-C6 sdkconfig.h missing" >&2
  exit 1
fi
for header in "${packed_headers[@]}"; do
  verify_header "$header"
done

if [ ! -f "$OUT/package.json" ]; then
  cat > "$OUT/package.json" <<'EOF'
{
  "name": "framework-arduinoespressif32-libs",
  "version": "5.3.0",
  "description": "Arduino-ESP32 3.1.3 libs with ESP32-C6 BLE modem sleep and PM",
  "keywords": ["framework", "arduino", "espressif", "esp32c6"],
  "license": "LGPL-2.1-or-later"
}
EOF
fi

tarball="${OUT}.tar.gz"
rm -f "$tarball"
tar -C "$(dirname "$OUT")" -czf "$tarball" "$(basename "$OUT")"
echo "C6_PM_CORE_READY $OUT"
echo "C6_PM_CORE_TARBALL $tarball"
