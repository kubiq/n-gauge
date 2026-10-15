#!/usr/bin/env bash
# Build the gauge simulator and run it on a recorded drive:
#   sim/run.sh                                  the newest drive in data/
#   sim/run.sh data/drive_X.csv data/trip_X.gpx a given one (either, or both)
# Needs cmake, a C/C++ compiler, SDL2 and LVGL in ~/Arduino/libraries/lvgl
# (as for the firmware build).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
BUILD="${BUILD:-$HERE/build}"

cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build "$BUILD" -j"$(nproc)"

if [ $# -eq 0 ]; then
  csv="$(ls -1 "$ROOT"/data/drive_*.csv 2>/dev/null | sort | tail -1 || true)"
  if [ -n "$csv" ]; then
    set -- "$csv"
    id="${csv##*_}"; id="${id%.csv}"   # drive_DATE_TIME_ID.csv -> its trip_..._ID.gpx
    gpx="$(ls -1 "$ROOT"/data/trip_*_"$id".gpx 2>/dev/null | head -1 || true)"
    [ -n "$gpx" ] && set -- "$@" "$gpx"
  fi
fi
exec "$BUILD/gauge-sim" "$@"
