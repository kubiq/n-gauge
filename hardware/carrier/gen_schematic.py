#!/usr/bin/env python3
"""Generate the carrier PCB's KiCad schematic from the table below.

    python3 hardware/carrier/gen_schematic.py

The carrier holds no chips of its own: it is sockets for the dev boards plus
the copper between them. So the whole design is a list of connectors and which
net each pin is on - kept here as data, and the .kicad_sch is generated from
it. To change a pin order, edit CONNECTORS and re-run; do not edit the
schematic by hand (it would be overwritten).

Every net is joined by name (net labels), which KiCad treats exactly like
drawn wires. Checked with:
    kicad-cli sch erc hardware/carrier/carrier.kicad_sch
    kicad-cli sch export netlist hardware/carrier/carrier.kicad_sch
"""
import os
import re
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
SYMLIB = "/usr/share/kicad/symbols/Connector_Generic.kicad_sym"
PROJECT = "carrier"

# Nets. Named by what they carry, and for the UART by direction, so the
# TX/RX crossover is visible in the name itself (wiring it straight through
# cost hours on the breadboard).
#   VBUS      5 V from the buck to the display board
#   V12       +12 V from OBD pin 16 (via the inline fuse on the cable), in on
#             J5, out to the buck's IN+ by a wire from pad J7
#   V3V3      3.3 V from the display board to the transceiver and GNSS
#   GPS_RXD   GPIO43 -> GNSS module RX   (board H2 pin 4, printed "TX")
#   GPS_TXD   GNSS module TX -> GPIO44   (board H2 pin 5, printed "RX")
#   CAN_RXD   transceiver R/CRX -> GPIO17 (ext1 deep-sleep wake source)
#   CAN_TXD   GPIO16 -> transceiver D/CTX
#             (the prototype has these two the other way round; the firmware
#              follows BOARD_CARRIER in config.h)
#   CAN_RS    GPIO18 -> transceiver Rs, chip pin 8 (flying wire from a pad)
CONNECTORS = [
    # ref, value, footprint, [net per pin 1..n], note
    ("J1", "Display H2",
     "Connector_PinHeader_2.54mm:PinHeader_1x08_P2.54mm_Vertical",
     ["VBUS", "GND", "V3V3", "GPS_RXD", "GPS_TXD", "CAN_TXD", "CAN_RXD", "CAN_RS"],
     "Male header; the Waveshare board's female H2 plugs onto it. "
     "H2: 1 VBUS(5V) 2 GND 3 3V3 4 GPIO43(TX) 5 GPIO44(RX) 6 GPIO16 7 GPIO17 8 GPIO18. "
     "CAN TX on GPIO16 / RX on GPIO17 - swapped vs the hand-wired prototype to "
     "untangle routing: needs BOARD_CARRIER 1 in config.h."),
    ("J2", "CAN SN65HVD230",
     "Connector_PinSocket_2.54mm:PinSocket_1x06_P2.54mm_Vertical",
     ["CANL", "CANH", "CAN_RXD", "CAN_TXD", "GND", "V3V3"],
     "Breakout pin order as printed: CANL CANH CRX CTX GND 3V3. Remove its R2 (120R)."),
    ("J3", "GNSS GY-GPSV3",
     "Connector_PinSocket_2.54mm:PinSocket_1x04_P2.54mm_Vertical",
     ["V3V3", "GPS_RXD", "GPS_TXD", "GND"],
     "Breakout pin order as printed: VCC RX TX GND. Module RX <- GPIO43, TX -> GPIO44."),
    ("J4", "Buck OUT",
     "Connector_PinSocket_2.54mm:PinSocket_1x04_P2.54mm_Vertical",
     ["GND", "GND", "VBUS", "VBUS"],
     "Buck module output side, 4 pins: GND GND VO+ VO+ (doubled for "
     "grip and current). Its input side is not socketed: IN+ is wired from pad "
     "J7; EN left unconnected (floating = enabled). Set the buck to 5.0 V "
     "before fitting the display."),
    ("J5", "OBD cable",
     "Connector_JST:JST_XH_B4B-XH-A_1x04_P2.50mm_Vertical",
     ["V12", "GND", "CANL", "CANH"],
     "JST pin 1 = OBD 16 (+12 V, via the 2 A inline fuse on the cable), 2 = OBD "
     "4/5 GND, 3 = OBD 14 CAN-L, 4 = OBD 6 CAN-H (L before H so the pair does "
     "not cross on the board - crimp the cable to match)."),
    ("J7", "Buck IN+ wire pad",
     "TestPoint:TestPoint_THTPad_D2.0mm_Drill1.0mm",
     ["V12"],
     "Wire from here to the buck's IN+. Its IN- needs no wire: the module joins "
     "IN- and OUT-, and OUT- is on J4."),
    ("J6", "Rs wire pad",
     "TestPoint:TestPoint_THTPad_D2.0mm_Drill1.0mm",
     ["CAN_RS"],
     "Short wire from here to SN65HVD230 chip pin 8 (Rs). No resistor: the breakout's R1 10k pull-down stays."),
]


def uid():
    return str(uuid.uuid4())


def load_symbol(name):
    """The Conn_01xNN definition from KiCad's library, renamed for embedding."""
    s = open(SYMLIB).read()
    start = s.find(f'\t(symbol "{name}"')
    if start < 0:
        raise SystemExit(f"{name} not in {SYMLIB}")
    depth = 0
    for i in range(start, len(s)):
        if s[i] == "(":
            depth += 1
        elif s[i] == ")":
            depth -= 1
            if depth == 0:
                blk = s[start:i + 1]
                break
    # Library-qualified name at the top level only; the unit sub-symbols
    # ("Conn_01x04_1_1") keep their bare names, as KiCad writes them.
    return blk.replace(f'(symbol "{name}"', f'(symbol "Connector_Generic:{name}"', 1)


def pin_offsets(sym_block):
    """Pin number -> (x, y) connection point, in the symbol's own coordinates."""
    out = {}
    for m in re.finditer(r'\(pin \w+ \w+\s*\(at ([-\d.]+) ([-\d.]+) \d+\).*?\(number "(\d+)"',
                         sym_block, re.S):
        out[int(m.group(3))] = (float(m.group(1)), float(m.group(2)))
    return out


def wrap(txt, width=58):
    """Break a note into lines for the sheet (KiCad text takes \\n)."""
    lines, cur = [], ""
    for word in txt.split():
        if cur and len(cur) + 1 + len(word) > width:
            lines.append(cur)
            cur = word
        else:
            cur = f"{cur} {word}".strip()
    if cur:
        lines.append(cur)
    return "\\n".join(lines), len(lines)


def text_block(x, y, txt):
    return f'''\t(text "{txt}"
\t\t(exclude_from_sim no)
\t\t(at {x:.2f} {y:.2f} 0)
\t\t(effects (font (size 1.27 1.27)) (justify left bottom))
\t\t(uuid "{uid()}")
\t)'''


def main():
    root = uid()
    sizes = sorted({len(c[3]) for c in CONNECTORS})
    syms = {n: load_symbol(f"Conn_01x{n:02d}") for n in sizes}
    offs = {n: pin_offsets(b) for n, b in syms.items()}

    parts = []
    # Two columns on an A4 sheet, 2.54 mm grid.
    x0, y = 60.96, 40.64
    col = 0
    row_h = 0
    for ref, val, fp, nets, note in CONNECTORS:
        n = len(nets)
        x = x0 + col * 127.0
        pins = "\n".join(f'\t\t(pin "{p}" (uuid "{uid()}"))' for p in range(1, n + 1))
        parts.append(f'''\t(symbol
\t\t(lib_id "Connector_Generic:Conn_01x{n:02d}")
\t\t(at {x:.2f} {y:.2f} 0)
\t\t(unit 1)
\t\t(exclude_from_sim no)
\t\t(in_bom yes)
\t\t(on_board yes)
\t\t(dnp no)
\t\t(uuid "{uid()}")
\t\t(property "Reference" "{ref}" (at {x + 2.54:.2f} {y - 5.08:.2f} 0)
\t\t\t(effects (font (size 1.27 1.27)) (justify left)))
\t\t(property "Value" "{val}" (at {x + 2.54:.2f} {y - 2.54:.2f} 0)
\t\t\t(effects (font (size 1.27 1.27)) (justify left)))
\t\t(property "Footprint" "{fp}" (at {x:.2f} {y:.2f} 0)
\t\t\t(effects (font (size 1.27 1.27)) (hide yes)))
\t\t(property "Datasheet" "~" (at {x:.2f} {y:.2f} 0)
\t\t\t(effects (font (size 1.27 1.27)) (hide yes)))
\t\t(property "Description" "{note}" (at {x:.2f} {y:.2f} 0)
\t\t\t(effects (font (size 1.27 1.27)) (hide yes)))
{pins}
\t\t(instances
\t\t\t(project "{PROJECT}"
\t\t\t\t(path "/{root}"
\t\t\t\t\t(reference "{ref}")
\t\t\t\t\t(unit 1)
\t\t\t\t)
\t\t\t)
\t\t)
\t)''')
        # A net label on every pin's connection point. Symbol Y is up,
        # schematic Y is down, hence y - py.
        for p, net in enumerate(nets, start=1):
            px, py = offs[n][p]
            lx, ly = x + px, y - py
            parts.append(f'''\t(label "{net}"
\t\t(at {lx:.2f} {ly:.2f} 180)
\t\t(effects (font (size 1.27 1.27)) (justify right bottom))
\t\t(uuid "{uid()}")
\t)''')
        # The pin order note right under it, so a reviewer checks it against
        # the physical board without opening properties.
        wrapped, nlines = wrap(note)
        note_y = y + 2.54 * (n - 1) + 7.62
        parts.append(text_block(x - 22.86, note_y, wrapped))
        rows_used = n + 3 + nlines * 1.2
        row_h = max(row_h, rows_used) if col == 1 else rows_used
        col += 1
        if col == 2:
            col = 0
            y += 2.54 * (int(row_h) + 3)

    parts.append(text_block(20.32, 20.32,
        "Carrier PCB - sockets for the dev boards only. Generated by gen_schematic.py; edit the table there."))
    parts.append(text_block(20.32, 25.4,
        "OBD 12 V -> inline 2 A fuse -> J5 -> pad J7 -> wire to buck IN+. Mind the GPS crossover: GPS_RXD = GPIO43 -> module RX."))

    lib = "\n".join("\t\t" + b.replace("\n", "\n\t\t") for b in syms.values())
    body = "\n".join(parts)
    sch = f'''(kicad_sch
\t(version 20250114)
\t(generator "gen_schematic.py")
\t(generator_version "1.0")
\t(uuid "{root}")
\t(paper "A4")
\t(title_block
\t\t(title "i30 N gauge pod - carrier")
\t\t(comment 1 "Sockets: Waveshare AMOLED-1.75, SN65HVD230, GY-GPSV3, TPS5430")
\t)
\t(lib_symbols
{lib}
\t)
{body}
\t(sheet_instances
\t\t(path "/"
\t\t\t(page "1")
\t\t)
\t)
\t(embedded_fonts no)
)
'''
    with open(os.path.join(HERE, f"{PROJECT}.kicad_sch"), "w") as f:
        f.write(sch)
    pro = os.path.join(HERE, f"{PROJECT}.kicad_pro")
    if not os.path.exists(pro):
        with open(pro, "w") as f:
            f.write('{\n  "meta": { "filename": "carrier.kicad_pro", "version": 1 }\n}\n')
    print(f"wrote {PROJECT}.kicad_sch: {len(CONNECTORS)} connectors, "
          f"{len({n for c in CONNECTORS for n in c[3]})} nets")


if __name__ == "__main__":
    main()
