# i30 N OBD Gauge — Prototype Guide

Custom driver-side vent mod for a **Hyundai i30 N**: a round AMOLED display showing
**oil temperature** (main goal) and **boost**, read live from the OBD-II port over CAN.

This guide covers the **prototype phase**: prove we can read good values on a cheap
ESP32 before buying the ~$50 Waveshare round display.

---

## Order list

**Prototype (order now — all in stock):**
- ✅ Classic **ESP32** dev board — *(already have)*
- ✅ **SN65HVD230** CAN transceiver, 2-pack *(ordered)* — remove the 120 Ω terminator
- ✅ **OBD-II male pigtail** (flying leads)
- USB power bank — *optional; the laptop's USB powers the ESP32 during the sniffer session*

**Final build:**
- ✅ **Waveshare ESP32-S3 1.75" AMOLED Round Touch, 466×466 — Standard version** (~$50) *(ordered)*
- ✅ **12 V→5 V buck converter** *(ordered)* (step-down regulator — **NOT a level shifter**). Needs to be
  **compact** (fits the tube behind the display) *and* car-tolerant input:
  - ⭐ **Pololu D24V22F5** — 5 V/2.2 A, 6.5–36 V in, ~12.7 × 17.8 mm. Best pick (headroom for WiFi).
  - **Recom R-78HB5.0-1.0** — encapsulated brick, ~11.6 × 8.5 × 10.4 mm, up to 72 V in, 5 V/1 A.
  - Avoid Mini360/MP2307 (≤23 V in, noisy → a car spike can kill them). A bare TPS5430 board
    works electrically but is bulky.
  - **On AliExpress** (Pololu/Recom aren't sold there genuine): pick a mini buck rated **≥36 V
    input**, fixed 5 V, ~2 A — e.g. a **TPS5430-based** mini, or MP4560/MP9486 (40–60 V).
    ⚠ Skip the **MP1584** (27 V max) for the permanent install — a 24 V jump-start is already
    near its limit. It's fine for bench testing though.
  - **Not needed for the prototype** — the ESP32 runs off USB/a power bank. This is a
    final-build part, so you can decide on it later.
  - ⚠ On any of these, parked idle = the buck's **operating no-load draw (a few mA)**, not a
    datasheet shutdown µA figure — that's what the *measure quiescent* task checks.
- ✅ **Inline blade-fuse holder (pigtail)** + **2 A** blade fuse *(ordered)*, in the 12 V line from OBD pin 16
  to the buck. (2 A, not 1 A — avoids nuisance-blowing on the buck's inrush.) Not an "add-a-fuse"
  tap — that's only for Option A. Final-build part.
- 3D-print filament: **PETG or ASA** (not PLA — dash heat)

*(No level shifter needed anywhere — the SN65HVD230 CAN transceiver is 3.3 V logic.)*

**Later (v2):** ✅ GNSS module *(ordered)* (multi-constellation — not GPS-only), read-only:
**VCC (3.3/5 V) · GND · TX → GPIO18**.
- **Track pick ⭐ — u-blox NEO-M9N** (~$25–40): **25 Hz** (smooth racing lines at speed),
  4 constellations at once, ~1.5 m (≈1 m with **EGNOS** enabled). Best precision-per-dollar.
- Budget — **NEO-M8N** (~$12–18): 10 Hz, ~2.5 m, enable **Galileo** in u-center.
- Pro/cm-level — **ZED-F9P** (~$150+): dual-band RTK, centimetre **only with an RTK correction
  feed** + dual-band antenna. Overkill unless you want that.
- **Two settings that matter:** pair with a **good active antenna + ground plane** (sky view;
  the tube by the vent is fine), and set the u-blox **dynamic model to "Automotive"**. Enable
  **EGNOS** (EU SBAS) for the free accuracy bump. Avoid relabeled M6/M7 clones.

---

## Parts for the prototype

| Part | Notes |
|---|---|
| Classic ESP32 dev board | Has the built-in TWAI/CAN controller |
| **SN65HVD230** CAN transceiver (blue "VP230") | 3.3 V logic. **Remove the 120 Ω terminator** (see below) |
| OBD-II male pigtail (flying leads) | Taps CAN-H / CAN-L / GND |
| USB power bank *(optional)* | Only if running the ESP32 without a laptop — the laptop's USB powers it during the sniffer session |
| Multimeter | For the two checks below |

---

## Before wiring: two hardware checks

### 1. Remove the 120 Ω terminator on the SN65HVD230
The car's CAN bus is already terminated. An extra 120 Ω can corrupt the bus.

- Measure resistance across **CANH ↔ CANL** on the transceiver board alone (nothing else connected).
- **~120 Ω** → it has a terminator (marked `121`) → **desolder that resistor**.
- **Open / very high** → no terminator → leave as is.

*(Two board variants both work: the AliExpress one has male header pins for CANH/CANL and
labels CTX/CRX; the Waveshare/Botland one has a screw terminal for CANH/CANL and labels the
logic pins CAN TX / CAN RX. Same SN65HVD230 chip, same wiring.)*

### 2. Identify OBD pins 6 and 14 on the pigtail (don't trust wire colors)
Cheap pigtails have random colors. Use the multimeter in **continuity/beep** mode:
touch one probe to a flying wire, the other to each metal pin inside the OBD plug.

| OBD-II pin | Signal | Goes to |
|---|---|---|
| **6** | CAN-H | transceiver **CANH** |
| **14** | CAN-L | transceiver **CANL** |
| **4** or **5** | Ground | ESP32 **GND** |
| 16 | +12 V (optional, later) | — |

Pin numbers are molded into the connector: pins 1–8 top row, 9–16 bottom row.
**Label the CAN-H and CAN-L wires** once found.

---

## Wiring

> ⚠ **The GPIO numbers below are for the ORIGINAL plain-ESP32 prototype and are
> WRONG for the current build.** The gauge runs on the Waveshare AMOLED board
> using **GPIO17 (TX) / GPIO16 (RX)** — see the table in the next section, which
> is the one to follow. `config.h` is always the authority
> (`CAN_TX_GPIO` / `CAN_RX_GPIO`).

```
ESP32 GPIO5 (CAN TX) ──► SN65HVD230 CTX      <-- historical, now GPIO17
ESP32 GPIO4 (CAN RX) ──► SN65HVD230 CRX      <-- historical, now GPIO16
ESP32 3.3V           ──► SN65HVD230 3V3
ESP32 GND            ──► SN65HVD230 GND
SN65HVD230 CANH      ──► OBD-II pin 6  (CAN-H)
SN65HVD230 CANL      ──► OBD-II pin 14 (CAN-L)
ESP32 GND            ──► OBD-II pin 4 or 5 (GND)
```

### Current wiring (Waveshare ESP32-S3-Touch-AMOLED-1.75)

Six connections. The board's 8-pin header is **H2**; pin 1 is the VBUS end.

| From (board) | H2 pin | To |
|---|---|---|
| GPIO17 — CAN TX | **7** | transceiver **CTX / D** (chip pin 1) |
| GPIO16 — CAN RX | **6** | transceiver **CRX / R** (chip pin 4) |
| 3V3 | **3** | transceiver **VCC** (chip pin 3) |
| GND | **2** | transceiver **GND** (chip pin 2) *and* OBD pin 4 or 5 |
| transceiver **CANH** (chip pin 7) | — | OBD-II **pin 6** |
| transceiver **CANL** (chip pin 6) | — | OBD-II **pin 14** |

Full header, from the board schematic:

| H2 pin | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| Signal | VBUS | GND | 3V3 | GPIO43 | GPIO44 | GPIO16 | GPIO17 | GPIO18 |

**Do not trust the transceiver module's silkscreen.** These boards are printed
on *both* sides and the two sides are mirrored, which already caused one wasted
trip to the car. Go by the chip's own pinout instead (SN65HVD230, from the TI
datasheet): **1 = D (CTX), 2 = GND, 3 = VCC, 4 = R (CRX), 5 = Vref, 6 = CANL,
7 = CANH, 8 = Rs**. Sanity check: the terminator resistor sits between pins 6
and 7, so whichever pair it bridged is CANL/CANH.

The 120 Ω terminator **must stay removed** — the car's bus is already
terminated.

If the GPS module was also disturbed: module **TX → GPIO18** (H2 pin 8),
module **RX → GPIO44** (H2 pin 5). That is a crossover — our TX goes to its RX.
Wiring it straight through leaves the GPS silently stuck at 1 Hz.

- Bus speed: **500 kbit/s**.
- **No extra terminator** on our side.
- Power the ESP32 from your **laptop's USB** during testing — the same cable carries the Serial
  Monitor output (a USB power bank only if you run it without a laptop).

---

## Flashing the sniffer

> **Historical.** This described the initial prototype phase on a plain ESP32
> dev board. Both sniffer sketches have since been removed — the gauge
> firmware does ISO-TP reassembly and SD logging itself. Recover
> `can_sniffer_sd` from git history (commit 54255f0) if promiscuous CAN capture
> is ever needed again. Kept here because the wiring and workflow notes below
> are still how the bus was first explored.

Sketch: `firmware/can_sniffer/can_sniffer.ino` *(removed)*

1. Arduino IDE with the **ESP32 board package** installed (Boards Manager → "esp32" by Espressif).
2. Tools → Board → **ESP32 Dev Module** (or your specific board).
3. Select the correct **Port**.
4. Open the sketch, click **Upload**.
5. Open **Serial Monitor** at **115200 baud**, line ending = **Newline**.

If GPIO4/5 conflict with your specific board, change `CAN_TX_GPIO` / `CAN_RX_GPIO`
at the top of the sketch (avoid strapping pins 0/2/12/15 and input-only 34–39).

---

## Test session in the car

1. Plug the pigtail into the car's OBD-II port (under the dash, driver side).
2. Power the ESP32 from the power bank.
3. **Ignition ON** (engine can be off for sniffing; run it for real oil/boost values).
4. Open Serial Monitor.

### What you should see
- A stream of `[RX] xxx [8] ..` lines = CAN frames. **This confirms your tap works.**
- If nothing streams → see Troubleshooting.

### Try the built-in commands (type + Enter in Serial Monitor)
| Command | Does |
|---|---|
| `s` | toggle the live frame dump on/off (turn OFF to read replies clearly) |
| `oil` | request standard oil temp (Mode 01 PID 5C) |
| `boost` | request manifold pressure (Mode 01 PID 0B) |
| `01 0C` | request RPM (sanity check the request path) |
| `22 XXXX` | enhanced request, 2-byte DID (for the real i30 N oil-temp DID) |
| `help` | reprint commands |

- If `oil` returns a sensible temperature → great, standard PID works.
- If it returns a **NEGATIVE response** → the i30 N doesn't expose 015C; use the
  **Mode 22 enhanced DID** instead (next section).

---

## Getting the REAL oil temp (enhanced Mode 22)

The i30 N's true oil temp (and true boost) live in **enhanced PIDs** requested with
**Mode 22** to ECU header **7E0**. Don't guess the DID — use the community-validated list:

- **Car Scanner ELM OBD2** app → the "Hyundai i30 N" custom-PID profile.
- **Torque Pro** extended-PID CSVs shared on i30 N forums / Reddit.

Each entry gives you: the request bytes (e.g. `22 XXXX`) and the scaling formula
(offset / divisor). Then in the Serial Monitor:

```
22 XXXX          <- the oil-temp DID from the list
```

The sketch prints the raw response bytes for DID `XXXX`. Apply the list's formula
(commonly `value = A - 40` or `(A*256+B)/scale`) to get °C.

**Bring this back to Claude** with the raw bytes + the formula, and we'll bake it into
the final firmware.

---

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| No `[RX]` frames at all | CAN-H/CAN-L swapped → swap pins 6/14. Or ignition not on. Or GND not connected. |
| `twai_start failed` on boot | Wrong TX/RX GPIOs for your board, or pins already used. Change them in the sketch. |
| Frames look garbled / error bursts | Wrong bus speed (must be 500 kbit/s) or the 120 Ω terminator wasn't removed. |
| Requests get NEGATIVE (7F) responses | That PID/DID isn't supported → use the enhanced Mode 22 DID from the community list. |
| Only a few frames, then silence | Bus went to sleep → turn ignition on / wake the car. |

---

## Power plan — Option B (chosen): OBD 12 V + sleep-on-idle

The gauge is powered from **OBD-II pin 16 (+12 V)** via a 12 V→5 V buck, keeping the
whole thing a single OBD-port plug (no fuse-box wiring). Pin 16 is *constant* battery
power, so to avoid draining the battery when parked:

- Firmware watches the CAN bus. **No frames for ~30 s → AMOLED off + `esp_deep_sleep`.**
- Car wakes the bus (ignition on) → ESP32 wakes and resumes the gauge.
- The i30 N diagnostic CAN bus sleeps a short time after ignition-off, so this works.

**⚠️ OPEN TODO — measure quiescent draw.** Once sleep is implemented, measure the actual
parked current on the 12 V feed (multimeter / USB power meter inline). This must include
the **buck converter's own quiescent draw** — many cheap bucks pull several mA at no load.
Target a few mA or less total; if too high, use a low-Iq buck or add a hard cutoff.
(Alternative if you'd rather not rely on sleep: Option A — switched 12 V from a cabin fuse
via an add-a-fuse tap, which cuts power fully with the car.)

## Mechanical — vent cover + display pod

**Location:** the driver-side **fixed side-window demister grille** on the dash top by the
A-pillar (part `97383-G3000-TRY`). It's a **fixed grille** blowing up/out at the glass. So the
display pod mounts **in front of it, angled inboard at the driver**, sitting *out of* the
airflow path: full demist function is kept, no trade-off.

Approach:
1. Pop off the OEM demister grille (clips) and study its mounting + the grille opening pattern.
2. **Measure with calipers or 3D-scan** the opening and the dash-top curvature.
3. Model a **replacement cover** that recreates the fixed grille slots (or reuses the OEM grille
   insert) so demist airflow to the side window is untouched.
4. Add a **low-profile display pod** on the driver/inboard side, in front of the grille, angled
   toward the driver's eyes, with a recessed circular pocket + lip/gasket for the round
   AMOLED. Keep it clear of the grille's outboard airflow path.
5. **Keep it low.** This spot is near the windscreen base — too tall blocks forward view and
   reflects in the glass. Angle the face slightly down.
6. Print in **ASA/PETG** (dash heat + UV); matte/textured finish or wrap to match the dash.
7. Route the display FPC + wiring down behind the **A-pillar trim** / into the dash to the
   ESP32 and power feed.

**Glare/reflection:** a bright AMOLED near the windscreen can reflect. Use auto-dim
(day/night) + an anti-glare film, and angle to minimise windscreen reflection.

### OEM part number (confirmed — PD i30 / Elantra GT crash-pad catalog)
Target = the **fixed side-window demister grille** on the dash top by the A-pillar:
**Nozzle Assy-Side Defroster, LH = `97383-G3000-TRY`** (driver side). RH = `97393-G3000-TRY`.
- Single variant (`-TRY`, black) — **no colour-suffix ambiguity**.
- Order one as a **donor to 3D-scan**; keep the fitted one intact.
- (Not this build: the adjustable louvered corner vent lower down = Duct Assy-Side Air Vent,
  LH = `97480-G3000-[color]`.)

### Custom pod — our approach (fully bespoke)
No usable OEM-vent STL exists, and off-the-shelf gauge holders are RHD-only and sized for
52/60 mm gauges — so this is **modeled from scratch**, exactly for our ~46 mm display. That's
the point: it's our part.

Plan:
1. Remove the OEM **left** vent cover; note the **louver cartridge** and how it clips in.
2. **Capture the geometry** — best is a phone 3D-scan / LiDAR of the removed cover + the dash
   opening; fallback is calipers + reference photos with a scale marker.
3. Design a **two-part piece**: (a) a cover that **re-clips the OEM louver cartridge** (keeps
   OEM airflow + adjustment), and (b) a **pod** in front holding the 46 mm AMOLED at driver
   angle, with a bezel lip, rear cable exit, and snap/screw bosses joining it to the cover.
4. **Print test rings** of just the opening profile first to dial in fit before the full part.
5. Final print in **ASA/PETG**, matte or wrapped to match the dash.

### 3D scanning (phone → cloud) — no NVIDIA needed
Use the **RealityScan phone app** (processes in Epic's cloud). The Linux desktop build needs an
NVIDIA ≥8 GB GPU we don't have, so cloud is the path.

**Photos, not video** — ~40–60 stills, 70–80 % overlap, 3 heights all around, tap-to-focus.

**⚠ The grille is monotone black → featureless = a blobby mesh.** The fix is to add trackable
features to the surface:
- **Stipple it with a white paint marker** (POSCA or similar): **many small ~1–2 mm dots,
  RANDOM spacing (never a grid), densely over the whole show face.** White-on-black is ideal
  contrast; random pattern is what the solver needs (a grid confuses matching).
- It's the **car's own part**, so use a **water-based paint pen** → wipes off with **isopropyl
  alcohol** afterward. Test on an edge first. Keep dots **off the mounting/clip faces**.
- Kill gloss / use soft **diffuse light** so specular hotspots don't move between shots.
- (Avoid uniform white "scanning spray" — that's for laser scanners; photogrammetry needs
  contrast + randomness, not a flat coating.)

**Alignment & scale:**
- Shoot on a **textured background** (newspaper / printed random-dot sheet); drop a couple of
  **printed coded targets** around it.
- **Set scale**: put two marker dots a known distance apart (or caliper the gap) and scale the
  mesh to it.

**Flip the part** to capture the mounting/clip side.

**Hybrid — don't scan the slots.** Thin grille slots reconstruct badly. **Caliper the slot
width/spacing/count and the mounting/clip dimensions** and model those parametrically; use the
scan only for the **overall curved shell**. Export **OBJ** (meshes are ~0.2–0.5 mm noisy — too
coarse for snap-fit tolerances).

### CAD on Linux
Fusion 360 isn't native on Linux, so model in **FreeCAD** (parametric, native — good for the pod
+ snap-fit; import the mesh as a reference) or **Blender** (for mesh cleanup / organic surfaces).

## Feature roadmap

**Hardware note — GPS:** buy the **Standard version** (the GPS variant is out of stock, and
you don't need it). Add an external **GPS module** later (see order list — NEO-M8N + external
antenna, or ATGM336H), read-only into **GPIO18** over UART (NMEA @ 9600). The module sits in
the **tube by the vent opening** near the windscreen, so it gets good sky view — an onboard-
antenna module is fine, no dash-burial signal problem. CAN stays on GPIO16/17; the onboard
buttons don't use the header. Nothing about v1 changes.

### v1 — Core gauge (build first)
- **Oil temperature** (enhanced Mode 22 DID) — the main goal.
- **Cold-engine warning** — "take it easy" state until oil hits ~90 °C, then green "ready".
- **Boost** gauge.
- **Audible over-temp guard** — speaker alarm if oil/coolant exceeds a threshold.
- **G-force meter** from the QMI8658 IMU *(stretch)*.
- **Buttons** to switch gauge screens without touching the display.

### v2 — Data & connectivity (much later)
- **Log OBD + GPS to the TF card** as GPX (or CSV) per drive.
- **Phone app over WiFi** to download session files.
- **Google Maps track** — render the drive as a route in the app, colored by
  speed / boost / oil-temp (track-day heatmap).

### v3 — Nice-to-have
- DTC (fault code) read & clear · shift light · OTA updates · gauge themes · lap timer.

## After the prototype succeeds

1. Confirm oil temp + boost read correctly and update fast enough.
2. Order the **Waveshare ESP32-S3 1.75" AMOLED Round Touch (466×466)** (~$50).
3. Move CAN to the S3 header pins: **GPIO17 = CAN TX, GPIO16 = CAN RX** (GPIO18 free).
4. Build the **LVGL** gauge UI (oil-temp + boost, touch to switch screens, brightness).
5. Add **sleep-on-bus-idle** power management (see Power plan above) and **measure quiescent draw**.
6. 3D-print the vent panel in **PETG/ASA** (not PLA — dash heat), keeping the OEM vent
   louvers functional.
7. Wire final power from **OBD pin 16 → 12 V→5 V buck → display 5 V pin**.

---

## Safety notes

- Reading CAN is passive/low-risk, but never leave anything transmitting garbage while driving.
- Don't power the display from OBD pin 16 (constant 12 V = battery drain) — use switched 12 V.
- Be careful pulling dash trim near the airbag; disconnect the battery for deeper work.
- Keep the screen glanceable and dimmable (driver-distraction rules).

---

## Local sourcing (CZ / EU) — fast, no weeks-long waits

**Prototype parts (order now — arrive in days):**
- **SN65HVD230** — Botland (Waveshare 3945), €4.50, ✅ 24 h dispatch:
  https://botland.store/can-bus-modules/6960-sn65hvd230-can-board-waveshare-3945-5904422300715.html
  - CZ alt: pajenicko 58 Kč (❌ currently out): https://pajenicko.cz/prevodnik-sn65hvd230-can-bus
  - Not stocked: LaskaKit, Dratek, GME (GME has the bare TI chip only).
- **OBD-II connector** — either:
  - GME **MOLEX 51115-1601** (bare male plug, solder pins 6/14/4): https://www.gme.cz/v/1496582/molex-51115-1601-konektor-obd-ii-16pin
  - easydiag.cz male↔female 60 cm, 140 Kč, ✅ in stock (min 2): https://www.easydiag.cz/obd2-16-pin-male-female-60-cm-rozdvojka-konektor-obd-plne-zapojen

**Later parts (no rush):**
- **Round AMOLED display** — not stocked locally; Waveshare direct / AliExpress, or ask LaskaKit to special-order.
- **Compact buck** (Recom R-78 / Pololu) — TME.eu or GME.
- **GNSS M9N + antenna** — AliExpress / Botland.

**Shop search links** (swap the query for any part):
- LaskaKit — https://www.laskakit.cz/vyhledavani/?string=SN65HVD230
- Dratek — https://dratek.cz/hledani.php?heureka=SN65HVD230
- GME — https://www.gme.cz/vyhledavani?query=SN65HVD230
- Botland — https://botland.store/search?s=SN65HVD230
- TME (buck/connectors) — https://www.tme.eu/en/katalog/?search=recom%20r-78

## References & inspiration

- **Boosted Intl — i30/i30N Demister Vent Gauge Holder** (RHD, 52/60 mm) — not usable for us,
  but a good reference for the airflow-retaining, driver-angled vent-replacement approach:
  https://boostedintl.com/product/hyundai-i30-i30n-demister-vent-gauge-holder/
- **PartSouq — i30 genuine parts catalog** (verify vent part number by VIN):
  https://partsouq.com/en/catalog/genuine/pick?c=Hyundai&model=i30
- **OEM RH vent 97490-G4000** (part reference, mirror of our LH part):
  https://www.ebay.co.uk/itm/387308639005
- **Community PID lists** — Car Scanner ELM OBD2 "Hyundai i30 N" custom-PID profile, and
  Torque Pro extended-PID CSVs on i30 N forums / Reddit (enhanced oil-temp & boost DIDs).
