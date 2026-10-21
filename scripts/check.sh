#!/usr/bin/env bash
# Everything that should pass before a firmware commit: the host tests, the
# firmware as the car runs it AND as the bench runs it (BENCH=1 - code only
# that build compiles rots unseen otherwise: it did, 2026-10-19), and the
# simulator.
#
# Usage: ./scripts/check.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

fail=0
step() {
  local name="$1"; shift
  echo "==> $name"
  # Warnings from our own files count; the toolchain's own do not. (The one
  # exception there was, filtered by its line number, broke as soon as the
  # line moved - it is fixed now instead.)
  local log
  if ! log="$("$@" 2>&1)"; then
    echo "$log" | tail -30
    echo "FAILED: $name"
    fail=1
    return
  fi
  local warn
  warn="$(echo "$log" | grep -E "car_gauge_v1/[^ ]+: warning:" || true)"
  if [ -n "$warn" ]; then
    echo "$warn"
    echo "WARNINGS: $name"
    fail=1
  fi
}

step "host tests"            ./scripts/test.sh
step "firmware (car)"        ./scripts/build.sh gauge
step "firmware (bench)"      env BENCH=1 ./scripts/build.sh gauge
step "simulator"             bash -c 'cmake -S sim -B sim/build >/dev/null && cmake --build sim/build -j"$(nproc)"'

[ "$fail" = 0 ] && echo "all checks passed" || { echo "CHECKS FAILED"; exit 1; }
