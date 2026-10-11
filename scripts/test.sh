#!/usr/bin/env bash
# Host-side unit tests. No hardware, no arduino-cli, no network.
#
# The firmware is cross-compiled for the ESP32 by arduino-cli, so these do not
# link against it. Instead they compile the PURE logic - config.h's sensor
# formulas and calc.h's timezone, GPS-plausibility and g-meter maths - with the
# host compiler, exercising the same source the board runs rather than a
# reimplementation that could drift from it.
#
# Usage: ./scripts/test.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# -Wall -Wextra here as well as in the firmware build: these files are small
# enough that any warning is worth reading, and arduino-cli hides warnings by
# default, which is how a real format-string bug survived in webapi.cpp.
CXXFLAGS=(-std=c++17 -Wall -Wextra -O1)

# The one C file the pure code depends on - Monocypher, under logcrypt.h -
# built once with the C compiler and linked into every test, as the firmware
# build links it.
if ! gcc -std=c99 -O1 -c -o "$OUT/monocypher.o" "$REPO/firmware/car_gauge_v1/monocypher.c"; then
  echo "BUILD FAILED: monocypher.c"
  exit 1
fi

fail=0
for src in "$REPO"/tests/test_*.cpp; do
  name="$(basename "$src" .cpp)"
  if ! g++ "${CXXFLAGS[@]}" -o "$OUT/$name" "$src" "$OUT/monocypher.o"; then
    echo "BUILD FAILED: $name"
    fail=1
    continue
  fi
  "$OUT/$name" || fail=1
done

if [ "$fail" -ne 0 ]; then
  echo "TESTS FAILED"
  exit 1
fi
echo "all tests passed"
