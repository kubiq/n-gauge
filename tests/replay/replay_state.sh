#!/usr/bin/env bash
# Builds and runs replay_state: a recorded drive CSV through the gauge's state.
#   tests/replay/replay_state.sh data/drive_....csv
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw="$here/../../firmware/car_gauge_v1"
out="${TMPDIR:-/tmp}/replay_state"
g++ -std=c++17 -O2 -Wall -Wextra -I "$fw" "$here/replay_state.cpp" -o "$out"
exec "$out" "$@"
