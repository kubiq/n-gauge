# N Gauge

<img src="docs/photos/pod-on.jpg" alt="The pod on the dash, showing oil and coolant on a cold engine" width="100%">

A round AMOLED gauge that lives in the driver-side air vent of a **Hyundai
i30 N**, reading the car live over the OBD-II port: **oil temperature** - the
number the car's own dashboard hides behind a menu - plus coolant, boost, tyre
pressures, a g-meter, GPS speed and a lap timer. It logs every drive to an SD
card, and a phone app pulls the logs off it and shows each drive on a map.

Everything here was built for one car and is in daily use in it. It is a
hobby project, published in case the findings, the code or the approach help
someone with the same car or the same board.

<p>
<img src="docs/photos/driver-view.jpg" alt="The driver's view: the pod above the left vent, beside the cluster" width="49%">
<img src="docs/photos/pod-off.jpg" alt="The pod switched off - a printed housing on the vent" width="49%">
</p>

*From the driver's seat, beside the car's own cluster; and switched off - the
3D-printed housing replaces the left air vent's surround.*

<p>
<img src="mobile/docs/screenshots/01-trips.png" width="32%">
<img src="mobile/docs/screenshots/07-drive-charts.png" width="32%">
<img src="mobile/docs/screenshots/04-gauge.png" width="32%">
</p>

*The phone app: every drive, named after where it went and grouped by week,
with the gauge in the header - its connection and last sync, like a watch in
Garmin Connect; a drive's speed, boost, temperatures and voltage over time;
and a tap on the gauge for its live readings in the car. More in
[mobile/](mobile).*

<p>
<img src="docs/gauge/01-oil.png" alt="Oil and coolant" width="24%">
<img src="docs/gauge/02-boost.png" alt="Boost and torque" width="24%">
<img src="docs/gauge/03-gmeter.png" alt="The g-meter" width="24%">
<img src="docs/gauge/04-tyres.png" alt="Tyre pressures and temperatures" width="24%">
</p>
<p>
<img src="docs/gauge/05-gps.png" alt="GPS speed and trip" width="24%">
<img src="docs/gauge/06-track.png" alt="The lap timer, with the delta to the best lap" width="24%">
<img src="docs/gauge/00-wake.png" alt="Waking up: the N badge until the car answers" width="24%">
</p>

*The gauge's screens - oil and coolant, boost, the g-meter, tyres, GPS, and
the lap timer 1.8 s up on the best lap - and the N badge it wakes up with.
Swiped through on the gauge; these are from the [simulator](sim) on a
made-up track session (`sim/screenshots.sh`).*

## What it does

- **Gauges:** oil and coolant (concentric arcs, with a cold-engine "not
  ready" state and over-temperature warnings), boost, a g-meter, the four tyre
  pressures and temperatures, GPS speed and trip distance, a lap timer that
  finds the start/finish line by itself (tap START in the pit, then drive).
- **Logging:** a CSV of every drive (2 Hz, decoded values - battery voltage
  among them - plus the raw payloads), a GPX track at 25 Hz, a sleep log.
  Logs can be encrypted to a key only the phone holds, so a card taken out
  of the gauge gives nothing away.
- **Waking up:** the N badge until the car answers, so the gauges never
  show a row of dashes; with a PC on USB, a status page for the desk.
- **Power:** deep sleep when the car goes quiet, ~11 mA parked (about 57
  days on the battery); wakes on CAN activity, the button, or the door
  opening (the IMU's wake-on-motion).
- **Phone:** its own WiFi access point with settings pages, OTA firmware
  updates with rollback, and an Android app ([mobile/](mobile)) that joins it
  from a QR code on the gauge and syncs the logs by itself. Each drive is
  named after where it started and ended (offline, from GeoNames - your own
  places like "Home" win), grouped by week, with its track on a map and its
  speed, boost, temperatures and voltage as charts. Delete a drive and it
  goes from the gauge too. English or Czech; dark themes or light.

## Hardware

| Part | |
|---|---|
| Waveshare ESP32-S3-Touch-AMOLED-1.75 | 466×466 round AMOLED, touch, IMU, PMU, SD slot |
| SN65HVD230 | CAN transceiver, on OBD pins 6 and 14 |
| u-blox NEO-M9N | GNSS, 25 Hz |
| TPS5430 buck | 12 V → 5 V from OBD pin 16 |
| 3D-printed vent pod | replaces the left air vent; STLs in [mechanical/pod](mechanical/pod) |
| carrier PCB | KiCad, in [hardware/carrier](hardware/carrier) - replaces the breakout boards |

## Things worked out about the i30 N

Most of this took real drives and a lot of wrong turns; the reasoning, and
the wrong turns, are in [DECISIONS.md](DECISIONS.md).

- **Oil temperature is byte 34** of the Mode 22 reply to DID `0xE001` (engine
  ECU, `0x7E0`): `°C = raw × 0.7435 − 49.2`, matched against the dashboard up
  to 100 °C. Mode 01 PID `0x5C` is not supported. Byte 25 of the same reply
  *looks* like oil but is coolant-derived - and correlating bytes against the
  coolant curve picks it, which is how it was wrongly used for weeks.
- **Coolant as the dashboard shows it** is byte 25 of that reply, a few
  degrees below PID `0x05`.
- **Tyre pressures and temperatures** come from the TPMS module (`0x7A0`,
  DID `0xC00B`), not the ECU.
- **The OBD port is behind a gateway:** no broadcast traffic reaches it, only
  replies to requests - so brake, pedal and steering cannot be read passively
  there.
- **Gear** is derived from rpm ÷ speed; the measured ratios are in
  `config.h`.
- **Battery / charging voltage** is plain Mode 01 PID `0x42`: 14.4-15.0 V
  running, 9.9-10.6 V cranking, 12.1-12.7 V just after switching off. Then
  the ECU answers **0.00 V** for a few seconds before going quiet - drop
  it, it is not the battery.

## Repository

| Path | |
|---|---|
| [firmware/car_gauge_v1](firmware/car_gauge_v1) | the gauge firmware (Arduino, ESP32 core 3.x, LVGL 9) |
| [mobile](mobile) | the Android app (Expo / React Native) |
| [backend](backend) | a small log store the app can upload to (Node, Docker) - optional |
| [sim](sim) | the gauge firmware on a PC: its screens in a window, fed a recorded drive |
| [tests](tests) | host tests of the firmware's pure logic |
| [hardware](hardware), [mechanical](mechanical) | carrier PCB, 3D-printed parts |
| [DECISIONS.md](DECISIONS.md) | why things are the way they are |
| [PROTOTYPE_GUIDE.md](PROTOTYPE_GUIDE.md), [PRINTING_GUIDE.md](PRINTING_GUIDE.md) | wiring the prototype; printing the pod |

## Building

Firmware ([details](firmware/car_gauge_v1/README.md)):

```sh
./scripts/setup-libs.sh          # Arduino core, pinned libraries, lv_conf
./scripts/build.sh gauge flash   # compile and flash over USB
./scripts/test.sh                # host tests
```

The gauge on a PC ([details](sim/README.md)) - needs SDL2 and CMake:

```sh
sim/run.sh                       # the newest drive in data/, in a window
scripts/make_track_gpx.py        # a made-up track session to drive it with
sim/screenshots.sh               # these screenshots, into docs/gauge/
```

App ([details](mobile/README.md)):

```sh
cd mobile && ./scripts/build-apk.sh install   # release APK onto a phone over adb
```

## Caveats

- **Not affiliated with Hyundai.** "Hyundai", "i30 N" and the N badge used in
  the boot screen and the app icon are Hyundai's trademarks; they appear here
  only to say which car this is for.
- The gauge only **reads** - standard OBD and diagnostic requests, nothing
  that writes to the car. Still: it is your car, your OBD port and your
  responsibility, and a device on the dash must not distract from driving or
  sit in an airbag's path.
- Built and tested on one car, a European i30 N Fastback. Other model years
  or markets may answer differently.

## Licence

[MIT](LICENSE). Third-party assets keep their own licences - see
[firmware/assets/README.md](firmware/assets/README.md). The place names in
the app (`mobile/assets/places`) are from [GeoNames](https://www.geonames.org),
CC BY 4.0.
