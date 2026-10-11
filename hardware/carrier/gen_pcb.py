#!/usr/bin/env python3
"""Create the carrier's starting PCB: a round board with every footprint on it.

    python3 hardware/carrier/gen_pcb.py            # only if carrier.kicad_pcb does not exist
    python3 hardware/carrier/gen_pcb.py --force    # overwrite it

Uses KiCad's own pcbnew module. The connectors and their nets come from
gen_schematic.CONNECTORS, the same table the schematic is made from, so the two
cannot disagree. This is a STARTING point: once the board is opened and
arranged in KiCad, that file is the source of truth - hence no overwrite
without --force.

J1's position comes from Waveshare's dimension drawing (see PLACE) and was
verified with calipers on the real display. The three M3 mounting holes start
on the display board's hole pattern (see HOLES).

This only builds the STARTING board. The committed carrier.kicad_pcb has since
been arranged by hand in KiCad (and J4 changed to 1x04 in place), so do not
regenerate it with --force - that would discard the arrangement.
"""
import math
import os
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_schematic import CONNECTORS  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "carrier.kicad_pcb")
FPLIB = "/usr/share/kicad/footprints"

RADIUS_MM = 23.0          # board is a 46 mm circle
CX, CY = 100.0, 100.0     # board centre on the sheet, mm

# Placement: ref -> (x, y relative to centre in mm, rotation deg, side).
# "front" faces the display; the modules plug in from the back.
# Footprint origins are pin 1, not the centre - the offsets below account for
# that. Front view (looking at the side the display sits on).
#
# J1 from Waveshare's dimension drawing of the display board, which is also
# 46 mm across. The drawing shows the board's back - the side facing the
# carrier - so left and right swap here: VBUS (H2 pin 1) on the left, IO18
# (pin 8) on the right. Pin row 18.30 mm below centre per the drawing. The
# pin-1 x is read from the photo's scale (8.74 px/mm: header strip -10.9..+9.5
# mm, i.e. pins -9.6..+8.2 on the drawing), NOT from its "12.70" dimension,
# which would put the header over the round edge. Verified with calipers on
# the real display (2026-09-30).
J1_PIN1 = (-8.20, 18.30)

# The three modules stand on edge in their sockets, like cards, stacked in
# rows across the back below a ~15 mm area kept free at the top for the GNSS
# antenna: GNSS, transceiver, buck. The OBD plug J5 is turned 90 degrees at
# the side edge (between the buck and J1 there was no room left once the rows
# moved down), and the two wire pads sit at the other side.
# Seen from above each is width x thickness, measured on the real boards.
# ASSUMED: each card is centred on its socket both ways - move an outline if
# a module's header sits off-centre on its board.
MODULES = {
    # ref: (width along the row, thickness across it, label)
    "J3": (27.0, 4.0, "GNSS 27x4"),
    "J2": (17.0, 4.0, "CAN 17x4"),
    "J4": (22.0, 6.0, "Buck 22x6"),
}

# ref -> (x, y, rotation, side, anchor). anchor "pin1" places pad 1 at (x, y);
# "centre" places the middle of the pad row there.
PLACE = {
    "J1": (J1_PIN1[0], J1_PIN1[1], 90, "front", "pin1"),  # display's H2 plugs on here
    "J3": (0.0, -5.5, 90, "back", "centre"),   # GNSS row
    "J2": (0.0, 0.5, 90, "back", "centre"),    # CAN transceiver row
    "J4": (0.0, 7.5, 90, "back", "centre"),    # buck output row (OUT+, GND)
    "J5": (16.5, 4.5, 90, "back", "centre"),   # OBD cable JST-XH 4-pin, turned, at the side edge
    "J6": (-12.0, 0.5, 0, "back", "centre"),   # Rs pad, at the transceiver's end
    "J7": (-14.5, 7.5, 0, "back", "centre"),   # buck IN+ pad, at the buck's end
}
# Kept free for the GNSS antenna: everything above this line (mm from centre,
# negative = up), about 15 mm at the middle.
ANTENNA_Y = -8.0
# Mounting holes to the pod: the display board's own three hole positions from
# Waveshare's drawing (top 20.50 above centre; lower two 13.75 either side,
# 14.70 below) - symmetric, so the mirroring does not move them - so the
# carrier and display line up hole for hole. M3 by choice; the display's own
# holes are M2 standoffs, so the carrier screws to the pod on its own and the
# display rides on the J1 header.
HOLES = [(0.0, -20.50), (-13.75, 14.70), (13.75, 14.70)]
HOLE_FP = "MountingHole:MountingHole_3.2mm_M3"


def mm(v):
    return pcbnew.FromMM(v)


def load(fpid):
    lib, name = fpid.split(":")
    fp = pcbnew.FootprintLoad(os.path.join(FPLIB, lib + ".pretty"), name)
    if fp is None:
        raise SystemExit(f"footprint not found: {fpid}")
    return fp


def main():
    if os.path.exists(OUT) and "--force" not in sys.argv:
        raise SystemExit(f"{OUT} exists - it may hold manual layout work. "
                         "Use --force to overwrite.")

    board = pcbnew.BOARD()
    board.SetCopperLayerCount(2)

    # Outline: one circle on Edge.Cuts.
    edge = pcbnew.PCB_SHAPE(board)
    edge.SetShape(pcbnew.SHAPE_T_CIRCLE)
    edge.SetLayer(pcbnew.Edge_Cuts)
    edge.SetWidth(mm(0.1))
    edge.SetCenter(pcbnew.VECTOR2I(mm(CX), mm(CY)))
    edge.SetEnd(pcbnew.VECTOR2I(mm(CX + RADIUS_MM), mm(CY)))
    board.Add(edge)

    nets = {}
    for _, _, _, pins, _ in CONNECTORS:
        for n in pins:
            if n not in nets:
                ni = pcbnew.NETINFO_ITEM(board, n)
                board.Add(ni)
                nets[n] = ni

    for ref, val, fpid, pins, _ in CONNECTORS:
        fp = load(fpid)
        fp.SetReference(ref)
        fp.SetValue(val)
        x, y, rot, side, anchor = PLACE[ref]
        fp.SetPosition(pcbnew.VECTOR2I(mm(CX + x), mm(CY + y)))
        fp.SetOrientationDegrees(rot)
        board.Add(fp)
        if side == "back":
            fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
        if anchor == "centre":
            # Footprint origins are pin 1; shift so the pad row is centred.
            pads = list(fp.Pads())
            mx = sum(p.GetPosition().x for p in pads) / len(pads)
            my = sum(p.GetPosition().y for p in pads) / len(pads)
            fp.Move(pcbnew.VECTOR2I(int(mm(CX + x) - mx), int(mm(CY + y) - my)))
        for pad in fp.Pads():
            num = pad.GetNumber()
            if num.isdigit() and 1 <= int(num) <= len(pins):
                pad.SetNet(nets[pins[int(num) - 1]])

    # Each module's footprint as a card seen from above, on Dwgs.User: not
    # checked by DRC, but it shows the clearance between cards and to the edge.
    for ref, (w, t, label) in MODULES.items():
        x, y = PLACE[ref][0], PLACE[ref][1]
        r = pcbnew.PCB_SHAPE(board)
        r.SetShape(pcbnew.SHAPE_T_RECT)
        r.SetLayer(pcbnew.Dwgs_User)
        r.SetWidth(mm(0.15))
        r.SetStart(pcbnew.VECTOR2I(mm(CX + x - w / 2), mm(CY + y - t / 2)))
        r.SetEnd(pcbnew.VECTOR2I(mm(CX + x + w / 2), mm(CY + y + t / 2)))
        board.Add(r)
        tx = pcbnew.PCB_TEXT(board)
        tx.SetText(label)
        tx.SetLayer(pcbnew.Dwgs_User)
        tx.SetPosition(pcbnew.VECTOR2I(mm(CX + x - w / 2 + 3), mm(CY + y - t / 2 - 0.9)))
        tx.SetTextSize(pcbnew.VECTOR2I(mm(1.0), mm(1.0)))
        board.Add(tx)

    # Mark the antenna area: a chord across the circle at ANTENNA_Y.
    half = math.sqrt(RADIUS_MM ** 2 - ANTENNA_Y ** 2)
    ln = pcbnew.PCB_SHAPE(board)
    ln.SetShape(pcbnew.SHAPE_T_SEGMENT)
    ln.SetLayer(pcbnew.Dwgs_User)
    ln.SetWidth(mm(0.15))
    ln.SetStart(pcbnew.VECTOR2I(mm(CX - half), mm(CY + ANTENNA_Y)))
    ln.SetEnd(pcbnew.VECTOR2I(mm(CX + half), mm(CY + ANTENNA_Y)))
    board.Add(ln)
    at = pcbnew.PCB_TEXT(board)
    at.SetText("GNSS antenna area - keep free")
    at.SetLayer(pcbnew.Dwgs_User)
    at.SetPosition(pcbnew.VECTOR2I(mm(CX), mm(CY + ANTENNA_Y - 6)))
    at.SetTextSize(pcbnew.VECTOR2I(mm(1.2), mm(1.2)))
    board.Add(at)

    for i, (x, y) in enumerate(HOLES, start=1):
        h = load(HOLE_FP)
        h.SetReference(f"H{i}")
        h.SetValue("M3 to pod")
        h.Reference().SetVisible(False)    # near the edge: the text overhangs it
        h.SetPosition(pcbnew.VECTOR2I(mm(CX + x), mm(CY + y)))
        board.Add(h)

    # Say so on the board itself, not only in this script.
    note = pcbnew.PCB_TEXT(board)
    note.SetText("J1 position verified against the display (calipers, 2026-09-30)")
    note.SetLayer(pcbnew.Cmts_User)
    note.SetPosition(pcbnew.VECTOR2I(mm(CX), mm(CY - RADIUS_MM - 3)))
    note.SetTextSize(pcbnew.VECTOR2I(mm(1.2), mm(1.2)))
    board.Add(note)

    board.Save(OUT)
    print(f"wrote {os.path.basename(OUT)}: {len(CONNECTORS)} connectors, "
          f"{len(HOLES)} holes, circle r={RADIUS_MM} mm")


if __name__ == "__main__":
    main()
