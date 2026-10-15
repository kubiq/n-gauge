#!/usr/bin/env bash
# Builds and runs replay_laps: a recorded GPX through the real lap timer.
#   tests/replay/replay_laps.sh data/trip_....gpx [HH:MM:SS START, UTC]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw="$here/../../firmware/car_gauge_v1"
out="${TMPDIR:-/tmp}/replay_laps"
g++ -std=c++17 -O2 -Wall -Wno-unused-function -I "$here/stubs" -I "$fw" \
  "$here/replay_laps.cpp" "$fw/laps.cpp" -o "$out"
exec "$out" "$@"
