#!/usr/bin/env bash
# One-time setup of the Arduino toolchain for this project. Idempotent.
#
# Everything here is a dependency that sketch.yaml cannot express, which is
# why it exists: the SensorLib fork is not in the library registry, and LVGL
# needs its config header in a specific place outside the sketch.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBDIR="${ARDUINO_LIBDIR:-$HOME/Arduino/libraries}"

echo "==> Arduino libraries dir: $LIBDIR"
mkdir -p "$LIBDIR"

echo "==> ESP32 core + registry libraries (versions pinned in sketch.yaml)"
arduino-cli core install esp32:esp32@3.3.8
arduino-cli lib install "lvgl@9.2.2"
arduino-cli lib install "GFX Library for Arduino@1.6.7"
arduino-cli lib install "TinyGPSPlus@1.0.3"

# SensorLib: MUST be Waveshare's fork, not the registry version.
#   - registry 0.4.1 has no TouchDrvCST92xx at all (this board's touch chip)
#   - upstream master has the touch driver but its legacy SensorQMI8658.hpp is
#     a half-migrated shim that crashes at imu.begin() with PC=0x00000000
# Waveshare vendor a working 0.3.1 inside their board-support repo.
if [ -f "$LIBDIR/SensorLib/src/touch/TouchDrvCST92xx.h" ]; then
  echo "==> SensorLib (Waveshare fork) already present, skipping"
else
  echo "==> Fetching Waveshare's SensorLib fork"
  if [ -d "$LIBDIR/SensorLib" ]; then
    mv "$LIBDIR/SensorLib" "$LIBDIR/SensorLib.replaced.$(date +%s)"
    echo "    (moved the previous SensorLib aside rather than deleting it)"
  fi
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT
  curl -sL "https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/archive/refs/heads/main.tar.gz" \
    | tar xz -C "$TMP" --wildcards '*/examples/arduino/libraries/SensorLib/*'
  SRC="$(find "$TMP" -type d -name SensorLib | head -1)"
  [ -n "$SRC" ] || { echo "ERROR: SensorLib not found in the archive"; exit 1; }
  cp -r "$SRC" "$LIBDIR/SensorLib"
fi

# LVGL reads lv_conf.h from one level above its own folder, so the repo's
# copy has to be reachable from there. Symlink so the repo stays the source
# of truth and the two can never drift.
CONF="$REPO/firmware/car_gauge_v1/lv_conf.h"
echo "==> Linking lv_conf.h -> $CONF"
ln -sfn "$CONF" "$LIBDIR/lv_conf.h"

echo
echo "Done. Build with:  ./scripts/build.sh gauge"
