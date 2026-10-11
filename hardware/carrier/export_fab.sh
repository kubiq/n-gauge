#!/usr/bin/env bash
# Export the carrier's manufacturing files and zip them for upload
# (PCBWay / JLCPCB take the zip as-is).
#
#   hardware/carrier/export_fab.sh
#
# Writes hardware/carrier/fab/ (not in git - regenerate it from the board):
#   gerbers/             copper, mask and silkscreen both sides + outline,
#                        Excellon drills in mm (plated and non-plated apart)
#   drill_report.txt     hole sizes and counts, to check against the board
#   carrier_fab_<date>.zip
set -euo pipefail
cd "$(dirname "$0")"

rm -rf fab
mkdir -p fab/gerbers

# Silkscreen is trimmed off the pads (--subtract-soldermask); zones are
# refilled first if the board file's fill is stale (--check-zones).
kicad-cli pcb export gerbers carrier.kicad_pcb -o fab/gerbers/ \
  --layers "F.Cu,B.Cu,F.Mask,B.Mask,F.SilkS,B.SilkS,Edge.Cuts" \
  --subtract-soldermask --check-zones

# Plated (pads, vias) and non-plated (M3 mounting holes) in separate files -
# the fab plates only the first.
kicad-cli pcb export drill carrier.kicad_pcb -o fab/gerbers/ \
  --format excellon --excellon-units mm --excellon-separate-th \
  --generate-map --map-format svg \
  --generate-report --report-path fab/drill_report.txt

zip_name="carrier_fab_$(date +%F).zip"
(cd fab/gerbers && zip -q "../$zip_name" ./*.g?? ./*.gm1 ./*.drl ./*.gbrjob)
echo "==> fab/$zip_name"
