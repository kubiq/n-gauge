# car_gauge_v1 — the gauge firmware

Arduino (ESP32 core 3.x) firmware for the Waveshare ESP32-S3 Touch AMOLED
1.75" (466×466), reading a Hyundai i30 N over the OBD-II port's CAN bus.

| File | Does |
|---|---|
| `obd.cpp` | TWAI/CAN at 500 k: Mode 01 and Mode 22 requests, ISO-TP multi-frame reassembly, on its own task on core 0 |
| `ui.cpp`, `ui_*.cpp` | LVGL screens, one file each: `ui_temps` (oil + coolant), `ui_boost`, `ui_gmeter`, `ui_tyres`, `ui_gps` (+ trip), `ui_track` (laps), `ui_wifi`; the notification rim (`ui_notice`), the boot splash (`ui_boot`); `ui.cpp` builds them and switches between them, `ui_priv.h` is what they share |
| `gps.cpp` | u-blox GNSS at 25 Hz (UBX), on core 0; position validation |
| `logger.cpp`, `log_*.cpp` | the SD card, on its own task on core 0: `log_csv` (the drive CSV, a table of columns), `log_gpx` (the GPX track, the trip distance), `log_vbo` (the track session), `log_sink` (batching and encryption), `log_names`; `logger.cpp` runs them, and keeps the sleep log; `log_priv.h` is what they share |
| `laps.cpp` | lap timer against a start line |
| `power.cpp` | deep sleep when the bus goes quiet; wake on CAN, button or motion |
| `webapi.cpp` | the WiFi AP: settings pages, JSON API, card files, firmware upload |
| `ota.cpp`, `rtc.cpp`, `settings.cpp`, `brightness.cpp`, `usbdisk.cpp` | updates with rollback, the clock, NVS settings, day/night dimming, the SD card as a USB drive |
| `bsp.cpp` | board glue: display, touch, IMU, PMU, I/O expander |
| `bench.cpp`, `prof.h` | the desk tools - the serial console, timing (`BENCH=1` builds only) |
| `calc.h` | the pure maths (time zones, GPS checks, g-meter projection, gears…), host-tested in `../../tests` |

**Values worked out on the car (all in `config.h`, the reasoning in
`../../DECISIONS.md`):** oil temperature is byte 34 of Mode 22 DID `0xE001`
(Mode 01 PID `0x5C` is not supported), `°C = raw × 0.7435 − 49.2`, checked
against the dashboard up to 100 °C; coolant as shown is byte 25 of the same
reply; boost from MAP (PID `0x0B`); tyre pressures and temperatures from the
TPMS module itself (request `0x7A0`, DID `0xC00B`), not the engine ECU.

## Toolchain

Fresh machine, two commands:

```sh
./scripts/setup-libs.sh      # core + pinned libs + SensorLib fork + lv_conf link
./scripts/build.sh gauge     # add 'flash' to upload
```

`BENCH=1 ./scripts/build.sh gauge flash` builds for the desk instead: a
serial console (fake CAN, injected readings, the log tail, the clock, RAM, a
render benchmark - the list is in `config.h`, "Bench builds") and frame
timing every 2 s. None of it is in the normal build; `bench.cpp` and
`prof.h` hold it. `scripts/check.sh` builds both, so neither goes stale.

`sketch.yaml` records the exact versions this was built and tested against.
Three things are easy to get wrong and are handled by those scripts:

- **`CDCOnBoot=cdc` is required.** Without it Arduino's `Serial` binds to the
  physical UART0 pins instead of the native USB port, so boot output vanishes
  and it looks like the sketch isn't running.
- **SensorLib must be Waveshare's fork.** Registry 0.4.1 has no CST9217 touch
  driver; upstream master has it but its legacy `SensorQMI8658.hpp` is a
  half-migrated shim that crashes at `imu.begin()` (`PC=0x00000000`).
- **`lv_conf.h` lives in this folder** and is symlinked to
  `~/Arduino/libraries/lv_conf.h`, because LVGL's own sources look one level
  above the library dir. `LV_USE_FLOAT` must stay 1 or every `%f` label
  (boost, tyres, g-meter) renders as garbage.

Flashing goes through `esptool --no-stub`: this board's USB-Serial-JTAG dies
partway through stub transfers, and a failed upload erases the app partition,
leaving a board that looks bricked. `scripts/build.sh gauge flash` does it the
safe way.

## Status

In the car and in daily use: CAN/ISO-TP, oil, coolant, boost, tyres, GPS at
25 Hz with GPX tracks, the g-meter, laps, drive logs, deep sleep at ~11 mA
parked with wake on motion, the phone settings pages and app, OTA updates.

There is no separate CAN sniffer sketch: the gauge does the ISO-TP
reassembly and logs the raw payloads itself (the Full log level).
