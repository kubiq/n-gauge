#!/usr/bin/env bash
# Screenshots of the gauge's screens for the READMEs, from the simulator:
# the made-up track session (scripts/make_track_gpx.py - nothing of a real
# drive), each screen cut round like the panel, into docs/gauge/.
#   sim/screenshots.sh [out dir]
# Needs ImageMagick (magick) as well as what sim/run.sh needs.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
OUT="${1:-$ROOT/docs/gauge}"
BUILD="${BUILD:-$HERE/build}"
GPX="$ROOT/data/trip_2026-06-06_10-00_track0.gpx"
CSV="$ROOT/data/drive_2026-06-06_10-00_track0.csv"

[ -f "$GPX" ] && [ -f "$CSV" ] || (cd "$ROOT" && scripts/make_track_gpx.py "$GPX")

cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build "$BUILD" -j"$(nproc)" >/dev/null

RAW="$(mktemp -d)"
trap 'rm -rf "$RAW"' EXIT
"$BUILD/gauge-sim" --shots "$RAW" "$CSV" "$GPX" | grep '^\[shots\]'

mkdir -p "$OUT"
n=0
for name in wake oil boost gmeter tyres gps track oilhot; do
  [ -f "$RAW/$name.ppm" ] || continue
  # Round, as the panel: everything outside the circle transparent.
  magick "$RAW/$name.ppm" -alpha set \
    \( -size 466x466 xc:none -fill white -draw "circle 232.5,232.5 232.5,0.5" \) \
    -compose CopyOpacity -composite "$OUT/$(printf '%02d' $n)-$name.png"
  n=$((n + 1))
done
ls -1 "$OUT"
