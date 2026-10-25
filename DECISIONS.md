# Decisions

Why the gauge's firmware and its phone app are the way they are. Every entry
is a choice that could reasonably have gone the other way, with the reason
and — where one exists — the trap that made the reason obvious.

Almost none of this is guessable from the code. This holds settled work. If a
decision here turns out to be wrong, change the entry rather than deleting
it: the wrong turning is usually the useful part.

---

## Architecture

**CAN runs on its own task on core 0. The Arduino loop, and therefore LVGL,
is pinned to core 1.**
Oil temperature updated only sporadically when CAN was serviced from `loop()`.
A dropped ISO-TP consecutive frame aborts the entire multi-frame transfer, and
`lv_timer_handler()` blocks for tens of milliseconds rendering a large label —
long enough to overflow a 5-deep RX queue mid-reply. The queue is now 32 deep
*and* the reception is off the rendering core. There is deliberately no
`obd_task()` to call from `loop()`; that coupling was the bug.

**The GNSS runs on core 0 too** (2026-09-24).
Its UBX handshakes block: MON-VER up to 1500 ms, the CFG-VALSET ACK 1200 ms,
the NAV-PVT poll 800 ms. Stacked up on the loop's core that froze the boot
animation mid-sweep. An interim `bsp_ui_pump()` serviced LVGL inside the wait;
on its own core the blocking costs nothing and the pump is gone. Calling into
LVGL from the GNSS task would now be actively wrong — `lv_timer_handler()` is
not re-entrant.

**Shared state is lock-free, and that constrains the field types.**
`ObdState` and `GpsState` are written on core 0 and read on core 1 with no
mutex. That is only sound because every field is 32 bits or smaller and
naturally aligned, hence atomic on Xtensa. Readers may see one field updated
just before another, which is harmless for a display.
⚠ This is why position is `int32` degrees × 1e7 and not `double`: an 8-byte
double is *not* atomic, so a reader could catch a half-updated coordinate and
log a position that never existed — the same class of bad point the GPX jump
filter exists to reject, but generated internally and untraceable.

**…but atomic fields are not enough when fields must agree with each other.
The position is published under a seqlock.**
Per-field atomicity was the whole argument for moving the GNSS to core 0, and
it was incomplete. Two failures got through: `haveFix` was raised before the
first coordinates were stored, so the logger accepted **(0,0)** as a point —
which then made the next 20 real points look like jumps and added ~5500 km to
the trip. And lat and lon read separately could come from two different fixes.
The writer now bumps `posSeq` to odd, writes lat/lon/alt, bumps it to even, and
only then raises `haveFix`; `gps_read_position()` retries if the counter moved
and refuses until a position has ever been published. The logger takes **one**
snapshot per point and uses it for every check and for the write.
The rule: a single field may be shared lock-free; a *set* of fields that must
be consistent with each other may not.

**One state for the car and what follows from it.** (`podstate.h`,
2026-10-23/24.) Every part used to judge the car by itself, from raw
readings, each its own way: sleep by CAN frames and GNSS over 10 km/h, the
AP by GNSS over 20 km/h for 5 s, the parked log rate by GNSS under 3 km/h for
3 minutes, the drive CSV's `car` column by CAN and rpm. They could disagree,
and each feature added another opinion. Now `loop()` works out once a pass:
the **facts** (copied in; each module still owns its own - the card's
remounts, the GNSS set-up, the lap timer stay inside them), the
**situation** (the car off / on / running; still / moving / driving /
parked, with every hold-off time together) and the **decisions** (the AP
wanted, the log rate, sleep allowed and what holds it, the faults). Moving
goes by the car's speed - exact, 0 standing, no sky needed - with the GNSS's
as the fallback when the car is silent, at 10 km/h, above its wander at a
standstill. The step is pure (`podstate_step`, host-tested); one writer,
`loop()`; the logger and the web server take a copy under a spinlock, since
its fields must agree with each other (the rule above). Switched in one part
at a time, each first replayed over the real drive logs
(`tests/replay/replay_state.sh`) and played on the desk (bench `D`).

**Position is stored as `int32` degrees × 1e7.**
It is also u-blox's own wire format, so this stores what the module sent rather
than a widened copy. ±214.7° covers both ranges at ~1.1 cm; a double over
±180° resolves to sub-nanometre, eight orders of magnitude finer than the
receiver is accurate even with RTK. It is *more* accurate in both consumers:
distances subtract in exact integer arithmetic before scaling, and coordinates
are formatted by `calc_fmt_deg()` straight from the integer with no rounding
anywhere in the output path.

**Pure logic lives in `calc.h`, included by both the firmware and the host
tests.**
So the tests exercise the same code the board runs rather than a
reimplementation. `tests/` also includes `config.h`, so the thresholds under
test are the shipped ones — a test carrying its own copy of a constant stops
testing the shipped behaviour the moment someone tunes it.

**The logger runs on its own task on core 0.** (2026-10-11.)
Measured first - `logMaxMs` and `loopMaxMs` columns in the CSV - because the
guess was wrong: building each CSV row in one buffer instead of ~50 prints
changed nothing (the logger's median stayed ~20 ms). A bench test of the card
said why: a 400-byte write costs ~0 ms, a flush 13 ms settled and 40-130 ms on
a fresh file. **The cost is saves, not writes.** Two fixes followed:
- The GPX buffer went from 2 KB to 12 KB. At 25 Hz (~4.5 KB/s) 2 KB filled
  every ~0.4 s, a save each time; 12 KB holds the 2 s of `GPX_FLUSH_MS`.
- The logger left `loop()` for a priority-1 task on core 0 (below CAN at 5 and
  the GNSS at 4). Everything on core 1 that touches the card - sleep, the USB
  share, the phone's downloads, the serial dump - takes `LoggerSdLock`. The
  CSV's IMU columns take the loop's sample, since I2C is shared with touch.
On the road (three drives, 2026-10-11): loop pass median 27-29 -> 11 ms,
passes over 40 ms 22 % -> 4-7 %, max 287-474 -> 147-210 ms, CAN untouched
(0 RX overflows, 0 ISO-TP aborts over 93 k transfers). The card's saves still
take up to ~500 ms - now where nothing waits for them.

**Settings are one table.** (2026-10-09.) Each simple setting is a row of
`settings_table` - NVS key (also the form field), JSON name, type, field,
default, range - and load, save, `/save` and `/api/settings` all walk it,
replacing five hand-kept lists. A stored value out of range or NaN loads as
the default: a brightness of 0 would leave a black screen with the phone page
as the only way back. Rules between settings (oil warning above ready, tyre
low below high) are in `settings_fixup()`. The app keeps the same schema in
`settings-schema.ts`.

**No library swaps.** (Survey, 2026-10-09.) Already on libraries where one
fits: TinyGPSPlus, SensorLib, Arduino_GFX, LVGL's QR, the core's WebServer,
DNSServer, Preferences and Update. Kept our own, each for a reason: ISO-TP
(ours checks which module replied - the oil/TPMS mix-up fix - and handles
short last frames; 99.8-100 % reassembly on drives; the libraries are small
hobby projects), the UBX config (SparkFun's is large, adds a second parser on
the UART, and its blocking waits ignore a sleep request; we send three
messages), and the web server (ESPAsyncWebServer costs internal RAM and
cross-task locking). The core's USBMSC was later replaced too - see USB share.

**The LVGL heap pool is allocated from PSRAM.**
`LV_MEM_SIZE` is 128 KB, which a 466×466 UI needs. In internal DRAM that left
~52 KB free, and `esp_wifi` is not defensive about running out: a failed
allocation deep inside `ieee80211_hostap_attach` surfaces as a null-pointer
panic, not a false return. Every long press rebooted the board.
`AP_MIN_INTERNAL_HEAP` (80 KB) is checked before starting the AP and refuses
with a message you can act on.

---

## Power and sleep

**Option B: constant 12 V from OBD pin 16, with deep sleep on bus idle.**
Chosen over Option A (switched 12 V via an add-a-fuse tap) because it needs no
wiring into the fusebox. The cost is that parked current matters, which is why
there is a whole measurement campaign behind it. Option A remains the fallback
if parked draw ever proves unacceptable, and carries a second benefit:
accessory circuits drop out while cranking, so the buck never sees the worst
moment on the car's electrical system.

**Sleep when nothing holds the pod awake.**
It began as two rules, and only two: USB host seen → never sleep; no CAN
event for 5 minutes → sleep, with `COLD_BOOT_AWAKE_MS` equal to
`BUS_IDLE_SLEEP_MS` so "the bus went quiet" and "the bus never spoke" were
the same rule. Real use added holds one at a time, each from a failure: a
phone on the AP (sleeping under a download cuts the file), a touch (the
screen went dark in someone's hand, engine off), moving by the GNSS (a broken
CAN wire: it slept at 95 km/h, 2026-10-19). Each lasts the "Sleep after"
window after it ends. Since 2026-10-24 they are one list in the gauge's state
(`podstate.h`, `awakeBecause`), logged in the drive CSV's `awake` column - so
"why did it not sleep" is one column - and `power.cpp` sleeps when it is
empty. Checked first against the old code on real drives: the same second.

**"CAN event" means the car is awake, not that something answered us.**
Replies from the TPMS module do not count. Found in the car on 2026-10-02:
the module answers our 5-second poll with the car OFF, so the bus never
looked idle and the pod stayed awake, logging and polling, for 5 hours parked
(one 40,981-row "drive" log; no engine replies in those hours, 121 TPMS
transfers per 10 minutes; no sleep-log entry all day). It never showed on the
bench, which has no TPMS module. The engine ECU is silent with the car off,
so its replies and any broadcast traffic are the signal; and TPMS is polled
only while that signal is fresh, so the pod no longer keeps the module awake
either. A parked wake therefore opens no drive log.

**The USB check is sticky.**
`isPlugged()` is SOF-based — it reports whether the host is *currently*
clocking the bus — so it goes false when the port suspends, which desktop USB
autosuspend does by itself. The board then decided the cable had gone and slept
in the middle of bench work with the cable still in. Latching removes the whole
class of surprise, and is safe because in the car there is no host at all: the
pod is fed on VBUS with no data lines.

**Wake on a CAN edge (`ext1`, ANY_LOW) plus a safety timer.**
Both wake pins idle high — the bus is recessive, the button pulls up — so both
go in one mask under one trigger. Only a pin that reads high *now* is armed: a
pin already low would fire the instant we slept and spin the board.

**The chatter backoff drops the CAN pin only, never the button.**
It used to gate both on one flag, which made the backoff **self-latching**:
once `ext1` was disarmed the button could not wake the board, and a button wake
is one of only two things that clear `fruitlessWakes`. So the counter could
never come down and the board stayed in timer-only mode for good — waking,
blinking, sleeping, ignoring the button. The button cannot chatter on its own;
it goes low because a person pressed it.

**The sleep timer follows the CAN pin, not the mask.**
With CAN armed the timer is only a safety net and can be long. With CAN dropped
it is the only thing that will ever re-check the bus, so it must be the short
backoff — and keying it off "any pin armed" would leave a chattering board
asleep for an hour between bus checks, now that the button alone keeps the mask
non-empty.

**The GNSS is parked with `UBX-RXM-PMREQ` before sleep, by the GNSS task.**
It was 17.2 mA of a 24 mA parked draw, the largest single item. `gps_park()`
only *requests* it: the request comes from `goToSleep()` on core 1 while the
task owns the UART on core 0, and `HardwareSerial` has no locking — two cores
writing the same port would interleave the PMREQ, and a mangled PMREQ leaves
the receiver awake. Handing the work to the port's single owner avoids a mutex
that would otherwise be held across those multi-second UBX waits. A park that
fails to confirm logs loudly, because nothing else would report it.

**The CAN transceiver is put in `Rs` standby before sleep.**
3.2 mA → 0.7 mA. Standby keeps the *receiver* alive, which is the only reason
this is usable at all: RXD still follows the bus so the edge wake still arms.
⚠ Only the SN65HVD230 behaves this way. An HVD231/232 shuts its receiver down
too, which would break the wake path silently — the pod would sleep through the
car starting.

**No trace cutting was needed for `Rs`.**
The reference schematic these breakouts clone takes `Rs` to GND through
R1 = 10 kΩ — slope-control mode, not a hard tie — and a pull-down that weak is
simply overridden by a GPIO. Also makes the failure benign: a lost pad hold
drops `Rs` low, reverting to high-speed mode, which wastes 3.2 mA but never
loses the ability to receive. It fails toward working.

**TPS5430 over the XL4015.**
4.0 mA quiescent and 45% efficient at a 16 mW load, against 5.4 mA and 19%.
⚠ Efficiency at 16 mW is far below any datasheet curve — they stop around 10%
of rated load, ~300 mA for a 3 A part — so this figure could only come from
measurement.

**Bench-only flags must all read 0 for the car.**
`SLEEP_MEASURE_MODE`, `SLEEP_SHOW_REASON`, `SLEEP_LOG_NVS`, `DEV_DISABLE_SLEEP`,
`RTC_SELFTEST`. They are kept behind `#if` and compiled out rather than
deleted, because the ones that report parked behaviour are the first things to
re-enable when it misbehaves somewhere you cannot watch it.

---

## The clock

**`clock_valid()`, not `gps.timeSet`, gates anything that stamps a file.**
They are not the same question, and the difference was a real bug: the system
clock survives deep sleep (the ESP32's own RTC timer keeps counting) but
`gps.timeSet` is a fresh `false` on every boot. So after each wake the logger
blanked its timestamps despite a perfectly correct clock. This predicate, not
the PCF85063, is what actually fixed undated logs.

**`calc_timegm()`, never `mktime()`.**
NMEA gives UTC, but `mktime()` interprets its input as *local* time — and
`settings_apply_tz()` has already `setenv`'d TZ — so it was subtracting the UTC
offset and every logged row was 1–2 hours out while claiming to be UTC. The old
comment ("TZ is UTC, so this is UTC") was true until the timezone setting
existed and then silently stopped being true. `timegm()` is not declared in
this newlib, so `calc.h` does the arithmetic, which also makes it testable.

**No clock source survives a power cycle.** Established by testing all four:
the ESP32 system clock survives sleep and resets but not power loss; the
PCF85063 has no backup cell fitted, so its seed-at-boot path is unreachable;
the GNSS module's cell holds 2.95 V but is not reaching `V_BCKP`. The practical
answers are a GNSS fix (~58 s, needs sky) and the settings page.

**Setting the clock from the browser takes a UTC epoch, never a formatted
local string.** The browser knows local time; the system clock and the RTC both
hold UTC. Parsing a local string would land every timestamp off by the timezone
offset — exactly the `mktime` bug above, reintroduced through a different door.

---

## Data and logging

**Log raw payloads for any format that is not fully understood.**
An earlier version wrote decoded values at guessed offsets, which made the logs
useless for finding the real ones — the decode was wrong, so the record of what
the car said was gone too. The full DID `0xE001` payload and the raw TPMS
payload are both logged verbatim. Never write derived values for an unverified
format: log the bytes.

**Oil temperature is byte 34 of DID `0xE001`, and the discriminator is lag, not
correlation.**
It read byte 25 for weeks, which is coolant-derived. The forum thread gives the
byte as token `AI`, and in Torque/Car-Scanner notation A=0 … Z=25, then AA=26 …
**AI=34** — so `AI` was read as a single letter, landing on Z=25.
⚠ The search method then *confirmed* the mistake: ranking all 42 bytes by
correlation against the coolant warm-up curve picked byte 25 at r=0.998
**because it is coolant**. That criterion cannot distinguish a coolant copy
from the real sensor; it actively prefers the copy, since real oil lags and
therefore correlates worse. Byte 34 scored +0.538 and was discarded for being
right. Confirmed three ways: it lags coolant by 200 s, it matched the cluster
(46 °C against ~50 °C where byte 25 said 68 °C), and under load it runs *above*
coolant, which a coolant-derived value cannot do.
Confirmed against the dashboard on the car 2026-10-02.

**Coolant on screen is byte 25 of DID `0xE001`, not PID 0x05.** (2026-10-03.)
PID 0x05 read 90–93 °C on every warm drive while the cluster stayed under 90.
Byte 25 — the "coolant copy" that misled the oil search — sits 2–3 °C below
PID 0x05 across 70–95 °C (88.1 at PID 90, 89.8 at PID 92), which matches what
the driver sees, and it shares the encoding of byte 34, the dash-confirmed oil
value. The gauge exists to be read next to the dashboard, so it shows the
dashboard's number. PID 0x05 is still polled and logged (`coolantPid05C`) as
the raw sensor. ⚠ The CSV's `coolantC` column changed meaning on that date:
before it was PID 0x05. The coolant warning now trips 2–3 °C (of PID 0x05)
later; the setting is unchanged. Switch: `COOLANT_FROM_E001`.

**The OBD port is behind a gateway: everything is requested, nothing is
heard.** (2026-10-03, OBD probe.) Over 4 drives the census saw no frame that
was not a reply to us, so brake, pedal and steering cannot be read passively;
anything not offered as a PID or DID is out of reach without a tap behind the
gateway, which is not worth it. Engine ECU 0x7E8 supports, in Mode 01:
01 03 04 05 06 07 0B 0C 0D 0E 0F 11 13 15 1C 1F 21 2E 2F 30 31 33 34 3C 3E 41
42 43 44 45 46 47 49 4A 4C 4D 4E 55 56 59 5E (and 0x80 range). No torque
(0x61–0x64), no MAF (0x10), no oil (0x5C). A second ECU, 0x7E9 (likely the
gearbox), answers 01 30 31 41. Measured rates at OBD_REQ_INTERVAL_MS 50:
boost 9.5/s, oil and coolant 4.8/s. Accelerator 0x49 idles at 14.5 %.
Round 2 (2026-10-03/04) tried the Hyundai torque DIDs OBDb lists: ED01–ED06
are refused with NRC 0x31 (they do not exist on this ECU) and E003 is 9
static bytes, not a boost block. **There is no torque value reachable from
the OBD port.** Any torque shown will be an estimate from absolute load
(PID 0x43, logged since), calibrated on a full-throttle pull, and labelled
as an estimate.
The probe and the link benchmark were then **removed** (2026-10-09), not left
switched off: every engine reply still went through their hooks, and they
were 563 lines and 4 KB of flash for questions already answered.

**No oil pressure: the engine has a switch, not a sender.** (2026-10-16.)
The parts catalogues list only an on/off switch for the i30 N's G4KH
(94750-37100: 0.3 bar, one wire) - it lights the dash lamp and has no value
behind it. Mode 01 has no oil pressure PID. A forum claim that a workshop
tool and a Car Scanner profile for another Hyundai engine show it came with
no request, byte or values. In the logged E001 payload (14 drives, 27k rows)
the one pressure-like value, bytes 31-32 little-endian, rises with rpm and
levels off at ~505 from 4000 rpm, and falls as the oil warms - but it trails
rpm by ~1 s more than E001's own rpm byte (byte 8, rpm / 65.6) and reads
280-510 with the engine off, where a sensor reads zero: modelled, not
measured. The test for any future candidate: it follows rpm at once, falls
as the oil warms, and is zero with the engine stopped.

**Boost shows 0.00 below atmospheric, and its zero is the car's baro.**
Negative boost is real — on a petrol engine the throttle holds the manifold
in vacuum at part throttle even with the turbo spinning (~−0.5 bar idle,
~−0.2 cruising) — but on a boost gauge it read as broken, so the number
stops at zero like the arc; the CSV keeps the true value. The zero is
PID 0x33 (98–99 kPa measured) rather than a fixed 101 kPa.

**No brake lights, and no IMU brake channel.** (2026-10-13.) The tyre
screen's car lit its tail lights from the IMU's deceleration (with a speed
gate, since on a slope gravity reads exactly like braking) - and was seen lit
when it should not have been. A guessed brake light that is sometimes wrong is
worse than none, so the lights, the VBO's "Brake IMU" channel and the detector
went. The CAN brake signal is not reachable (the gateway, above); it would be
the way back.

**The GPX rate limiter runs BEFORE the plausibility check.**
This ordering is load-bearing. With the limiter last, the jump check ran every
loop iteration (~5 ms) and refreshed its reference each time, while the
position only changes every 40 ms — so 40 ms of travel was divided by a 5 ms
interval, an ~8× overstatement, and the effective threshold collapsed from
250 km/h to about 31. Ordinary driving was being thrown away. An offline
simulation missed it because it replayed points already 40 ms apart.

**The segment split is emitted after every gate, immediately before the point.**
Placed before the gates it was governed by a timestamp the gates hold stale, so
a spell of poor reception appended `</trkseg><trkseg>` on every call —
kilobytes per second of empty segments.

**Trip distance is summed in the logger (`log_gpx.cpp`), not `gps.cpp`.**
It accumulates at the one line where a fix has passed every gate, because the
logger is the only place that knows a fix is trustworthy. Summing raw positions
would let one tunnel spike add kilometres nobody drove. It is held in
`RTC_DATA_ATTR` so it survives deep sleep — the pod sleeps after five minutes
of bus silence, so one journey with a fuel stop is several wakes, and resetting
per wake (or per GPX file, which amounts to the same thing) would zero the trip
at every stop.

**A drive log exists only if the car was on.** The CSV is created on the first
row after the first CAN frame (`obd.lastFrameMs != 0`), the firmware's single
definition of "the car is on". Creating it on the first row alone still left a
file of a few rows behind every bench boot and every parked wake with a silent
bus: the card reached 245 of them. A GNSS fix deliberately does not count, since
a parked car near a window has one too. Files are named by local start time,
like trip GPX files, and fall back to a number only without a trusted clock.
A numbered file is **renamed** to its dated name as soon as the clock becomes
trusted (usually the first fix, leaving the garage), named after when the
drive *started*. One attempt only, so a card that refuses the rename is not
retried on every row. This is the software answer to the missing RTC backup
cell: the only place to fit one on this board is soldering to CP13, rejected
as too risky on so small a PCB and not sturdy in a car.

**Logging is a level, not a switch.** (2026-10-09; setting `logLevel`,
default Full.) Off (the sleep log only), Trips (GPX, and the VBO in track
mode), Normal (adds the drive CSV), Full (adds the raw hex columns: TPMS and
the oil DID). Below Full those stay as empty columns, so the CSV's layout
never depends on the level. A change closes every file; what the new level
allows reopens as new files. Asked as "a switch to turn logging off if it
slows the gauge": measured instead (above), and fixed where it cost - a switch
would have lost the logs that diagnosed the oil byte, sleep and coolant.

**Every log name carries a session id.** (2026-10-13, bb8b288.) Six random
hex digits per boot or wake, before the extension:
`trip_2026-10-12_21-19_3fa81c.gpx`. On 2026-10-12 the pod restarted a minute
into a drive, and pairing the GPX with its CSV by time took the wrong one;
nothing in the names said which belonged together. Random rather than
counted, so an erased NVS cannot repeat ids already on the card. Logs from
before got ids hashed from their GPX's name (`legacy-names.ts`, and
`scripts/rename-legacy.ts` for the card and saved folders).

**The gear is worked out, not read.** (2026-10-10.) The DCT picks its own
gears and the gateway hides which, but in any gear rpm per km/h is fixed.
Ratios measured from 3957 samples at 12 km/h or more: 122, 74, 50.5, 37.6,
30.5, 25.2, 20.0, and 16.1 for 8th (a motorway stretch). `calc_gear()` takes
the nearest within 7 % - half the gap between 6th and 7th, so no two windows
overlap - above 10 km/h; anything else (shifting, slip, neutral) is gear 0.
Replayed on two drives: 77-85 % of moving rows got a gear, none jumped two.

**Voltage is PID 0x42, on a request of its own, logged last.** (2026-10-14,
21a823d.) The ECU's own supply: the battery engine off, the charging voltage
running - in the supported list since the 2026-10-03 probe. Once a second
(`VOLTS_POLL_MS`): added to the 5 Hz slow request, its two bytes would push
that reply past one CAN frame. The CSV's **last** column, `volts`, so no
column before it moves - the app reads columns by name, but a spreadsheet of
old and new logs side by side does not. In `/api/live` and the app; not on
the gauge's screens (the owner's call). First drives (2026-10-14): 14.4-15.0 V
running (median 14.58), 9.9-10.6 V cranking, ~12.1-12.7 V just after
switching off - so the "16.6 V charging" of September was the meter. ⚠
Switching off, the ECU answers **0.00 V** for ~11 s before going silent:
its shutdown, not the battery. Anything under `VOLTS_MIN_V` (5 V) is no
reading.

**Firmware on the card is installed once, by its hash.** (2026-10-09.)
`ota_check_sd()` reads the image's own `app_elf_sha256` from its header
(~300 bytes, nothing hashed) instead of renaming the file to
`firmware_installed.bin`. The last hash acted on goes to NVS **before**
installing: an image that crashes and rolls back is not retried forever, and
an old file left on the card can never overwrite a later USB flash. The file
stays where it is.

**The card has a delete, for drive logs only, from the app.** (2026-10-14,
d45a40c - reversing "no delete endpoint, deliberately: it is reachable by
anyone who joins the AP".) That rule held while a log was often the only copy
of an unrepeatable run. Once the phone synced every log, deleting a drive on
the phone was pointless without it: the next sync pulled the file straight
back. So `POST /api/delete` exists, narrowed to what the reason was about:
- **Drive logs only** - `trip_`/`drive_`/`track_` with `.gpx`/`.csv`/`.vbo`,
  plain or `.nge`. Never the settings, the sleep log or a firmware `.bin`.
- **Not a file this session still writes** (`logger_is_open`, 409) - that
  would lose the rest of the drive - nor while the card is lent to a PC (503).
- **A file already gone is 410, not 404.** An unknown path is answered by the
  captive portal with a 200 page, so the app counts only the body `deleted`
  or a 410 as done, and older firmware without the handler can never look
  like a successful delete.
What still guards it: the AP's per-device passphrase, the drive-log filter,
and the app's confirmation dialog. The `/files` page stays read-only.

**Logs are encrypted to a public key; the pod never holds a key that can
read them.** (Decided and built 2026-10-11; encrypting on the car since that
evening, seen read back by the app 2026-10-13. Bench check: an 80 s run of 43
records decrypted to 159 rows with no plaintext in the file; cost +33 KB flash,
+17 KB RAM. The CSV is staged in 4 KB and written at each 2 s flush, so one
flush is one record.)
The card holds every drive's GPS track, the CSV included (lat/lon columns) -
where the car is parked at night - readable by anyone who takes the card out.
That is the threat: **card theft**. A pod taken whole is covered too, as a
side effect of the design.
- **Asymmetric, so nothing on the pod can decrypt.** The phone app generates
  an X25519 key pair; the private key stays in the app (SecureStore, i.e. the
  Android Keystore), the public key (32 bytes) goes to the pod as a setting
  (NVS). A symmetric key on the pod would need flash encryption and secure
  boot to be any better than the plaintext - one-way eFuses, decided against.
- **Per segment, a throwaway key.** Each time the pod opens a log (a new file,
  or after a boot or wake) it makes an ephemeral X25519 key pair, derives the
  segment key as BLAKE2b-256(X25519(eph, pub) ‖ eph_pub ‖ pub ‖ "n-gauge log
  v1") - as libsodium's key exchange hashes its shared secret with both public
  keys - writes eph_pub in a segment header, and forgets eph's private half.
  Afterwards not even the pod can read what it wrote; the only key ever
  exposed is the open segment's, in RAM - gone when the power is. Nothing
  secret is ever stored on the pod.
- **Records, not a stream.** Each batch the logger already flushes (1-2 s) is
  one XChaCha20-Poly1305 record: the nonce a per-segment counter, the segment
  header as associated data. On the card: `type | length | body` blocks, a
  header block (magic, version, eph_pub) then record blocks (ciphertext +
  16-byte tag). So the file stays **append-only** - the sync's {name, size}
  listing and Range resume keep working unchanged - and a record cut off by a
  power loss fails its tag and is dropped, everything before it intact.
  The exact layout is in `firmware/car_gauge_v1/logcrypt.h`.
- **Monocypher, not the core's mbedTLS.** The plan was ChaCha20-Poly1305 and
  HKDF-SHA256 from mbedTLS, but the Arduino core's prebuilt mbedTLS has
  ChaCha20 and Poly1305 switched off (`CONFIG_MBEDTLS_CHACHA20_C` unset) - and
  a crypto library that is one portable C file (Monocypher 4.0.2, BSD-2/CC0,
  vendored, checked against its GitHub tag and its own test suite) runs the
  same code in the host tests. Its primitives are X25519, XChaCha20-Poly1305
  and BLAKE2b, hence those; each has an audited counterpart in `@noble/*` for
  the app. A ChaCha cipher rather than AES-GCM for the readers' sake: the
  phone decrypts in pure JS, where ChaCha is the faster of the two.
- **Encrypted files are named `<plain name>.nge`** ("N Gauge Encrypted", as
  the `NGE1` magic): `drive_<stamp>.csv.nge`, `trip_<stamp>.gpx.nge`. Readers
  decide by the name, decrypting gives back the plain name, and nobody opens
  ciphertext in a spreadsheet and takes the log for corrupt (suggested by the
  owner, 2026-10-11). Numbered files keep one sequence across both.
- **The throwaway secrets come from a stirred pool, not straight from the
  RNG.** The ESP32-S3's RNG is only guaranteed true-random while the radio
  runs (WiFi or BT). The AP is up from boot, but drops while driving
  (AP_DRIVE_OFF) - exactly when a GPX may open, at the first fix on the road.
  So RNG output is stirred into a BLAKE2b pool on every logger pass with the
  AP up; a secret is BLAKE2b(pool, fresh RNG output), and the pool is then
  ratcheted so the secret cannot be recomputed from it.
- **One test vector holds the readers to the writer.** `tests/test_logcrypt.cpp`
  writes `tests/vectors/logcrypt-v1.json` from the pod's C code and fails if
  the format drifts from it; the app's tests decrypt that file with `@noble/*`
  and re-encrypt it to the same bytes.
- **The GPX's closing tags move to the reader.** The writer keeps
  `</trkseg></trk></gpx>` at the end by writing it and seeking back over it
  at the next batch (`gpxWriteTail()` / `gpxCommit()`) - a rewrite in place,
  which an encrypted file cannot take. Encrypted, the tail is not written;
  the app's parser does not need it, and the app adds it to every GPX it
  shares or saves (`cleanGpx()`).
- **Plaintext on purpose:** `sleeplog.txt` (no position, and it is the file
  read when debugging power on the bench), file names (they give drive
  dates and times, not places), and every file written before a key is set.
  Encryption is on only once the pod has a public key; no key, no change.
- **The private key is exported once to KeePass.** Losing it loses every
  encrypted log, so the app offers export (and import, for a new phone), with
  a short fingerprint to compare against the pod's settings page.
- **The phone is where logs become readable again - no PC tool.** The key is
  on the phone and the app is the centre of the workflow (the owner's call,
  2026-10-11), so the app hands logs on decrypted: "Share" a drive's GPX or
  CSV to another app, or "Save to a folder" / "Save all to a folder" (plain
  names, files already there skipped). CSV analysis on the PC - how the oil
  byte was found - now starts from those copies.
- **Costs accepted:** the card in a reader or shared over USB holds only
  ciphertext; `logger_dump_tail()` and the pod's `/files` page show
  ciphertext.

---

## Lap timing

**Lap 1 starts when the car pulls away, and no lap is under 10 s.**
(2026-10-09/10.) On the first try the loop after setting the line was timed
as an out-lap; lap 1 now runs from the last fix under 5 km/h after it. The
minimum went from 20 s to 10 s after a real 17.7 s test loop was taken for a
double trigger and two loops merged into one 38.5 s lap: GPS jitter
re-crosses a line within 1-2 s, and no track lap is under 10.

**The line is found by loop closure: tap START anywhere.** (2026-10-12,
99556ae, `loopfind.h`; not yet driven.) Setting the line meant stopping on
it - impossible on a real track - and starting from the pit made lap 1 the
drive out of it. Now the gauge drops a breadcrumb every 10 m (in PSRAM,
~40 km before it gives up); after 500 m, the first crumb within 20 m of an
older one, heading within 30°, closes the loop, and that crumb is the line.
The pit exit is never driven again, so it never matches; the heading test and
the minimum length reject figure-8 crossovers, U-turns and out-and-back
roads. Accepted costs: the line is wherever the loop first closes - often
just after pit exit, not the official start/finish - so laps are consistent
but offset from the circuit's own; and the VBO opens once the line is found,
so it misses lap 1.

---

## USB share

**The card as a USB drive is read-only, and empty until shared.**
(2026-10-08/12.) A composite USB device - serial plus mass storage - whose
drive shows no medium until "Share SD card with PC" is tapped on the WiFi
screen (the button exists only with a PC connected). Sharing suspends the
logger: every file closed, no card access, until it ends. Read-only because
a PC writes to a mounted FAT card even when nothing is copied - unmounting
was slow and left "not properly unmounted" - and because a read-only drive
can be taken back by the gauge safely ("Stop sharing"): the worst a PC loses
is a copy in progress.

**Our own `tud_msc_*` callbacks, not the core's USBMSC.** (2026-10-13,
b18b7dc.) After an eject the board reset every ~9 s for minutes. The core's
USBMSC answers a READ with no medium as 0, which TinyUSB defines as "busy,
call again" - so the PC's post-eject read of sector 0 spun forever, starved
`loop()` into the 8 s watchdog, and the PC asked again after the reset. It
depended on timing, which is why six ejects in a row had gone fine the day
before. Ours fails at once with NOT READY / MEDIUM NOT PRESENT (answered in
1 ms) and refuses writes with DATA PROTECT. Verified: 77 files (~250 MB)
read without error, an eject, no resets.
⚠ The first two explanations - the PC's writes, then a race - were both
wrong; read-only was still worth keeping for its own reasons (above).

**Reads are multi-sector, and the PC is told the partition's size.**
(2026-10-11.) One sector per card command made a 4 KB PC request eight
commands; one FatFs read per request took an 8.3 MB file from 571 to
807 kB/s, near full-speed USB's ~1 MB/s. 4-bit SD would not help and cannot
be had: D1 and D2 are not wired on this socket. The same change fixed the
size: the PC had been given the FAT volume's, 8192 sectors short of the
partition ("p1 extends beyond EOD" on every share), so a file in the last
4 MB would have read wrong.

---

## UI

**Every updater compares before touching its object.**
LVGL's label and style setters do *not* compare against the current value: each
call copies the string and invalidates the object, and `loop()` runs at roughly
200 Hz. The 48 px oil number was being redrawn ~200 times a second, and on this
partial-render display a label that large spans several flush strips — which is
why oil visibly blinked and the 28 px coolant number next to it did not.
⚠ For anything full-panel the guard is not an optimisation but the difference
between a rendered element and a strobe.
⚠ Flags too: `lv_obj_remove_flag(HIDDEN)` invalidates the whole object even
when it is already shown. The WiFi screen's countdown cleared HIDDEN on its
full-screen arc every step, so every frame redrew ~199 KPx, the loop ran at
7 Hz there against 121 elsewhere, and touch was read every ~143 ms - swipes
on that screen were missed (2026-10-13, b56f9a4). Changing the flag only on
a real change: 122 Hz.

**Swipe to change screens; taps are for buttons.** (Changed 2026-09-25.)
The hardware buttons are unreachable once the board is in the printed pod, so
the touch panel has to do everything. It originally used a tap anywhere to cycle
screens, which meant a tap could never mean anything else — no screen could
carry a button. Left/right swipes change screen now, and taps are free.
⚠ LVGL still sends `LV_EVENT_CLICKED` at the end of a swipe that *started* on a
button, so every button handler must first ask `touch_was_swipe()`. Otherwise a
swipe across the track screen could press "Set line".

**The screens and their order are a setting; WiFi is always in, and last.**
(2026-10-13, 37822ed.) `settings.views` is the order as digits (default
`0245736`); a screen left out is never cycled to. WiFi cannot be left out -
it is the way to the phone - and sits last: it is for setting up, not for
reading while driving. A boot onto a screen since hidden lands on the first
non-WiFi screen, not always on Oil.

**The hardware button only wakes the board.** (2026-10-13.) With swipes for
screens and a touch restarting the WiFi countdown, the short press (next
screen) and long press (toggle the AP) had nothing left to do, and the button
is unreachable in the mounted pod. Their handling went - and with it the
manual AP hold, the only way to keep the AP up while driving. It stays a
deep-sleep wake source.

**Faults are shown as a coloured rim plus one line of text — the notification
area — not as text in the middle.** It started as a CAN-only red rim and now
carries any fault visible from every screen: no CAN (red), no GPS module and no
SD card (amber). Colour is severity — red means the gauge cannot do its job,
amber means it still works but something is missing — so a missing card does
not look as alarming as losing the bus. One message at a time, starting with the
highest priority and marked "i/n" when there are several; **tapping the text
steps through the rest**, which became possible once screens moved to swipes.
The rim stays the colour of the *worst* active fault even while a minor one is
shown, so tapping to "no SD card" can never make a lost CAN bus look amber.
"No GPS fix" is deliberately not a fault:
normal in a tunnel, and it would flash constantly. The GPS fault waits 15 s
after boot, longer than one full baud hunt.
The shape below is unchanged from the CAN-only version.
A round panel has a natural border, and a coloured edge reads as a state at a
glance without occupying space a gauge needs — text in the middle of a dial
competes with the number beside it. Built as a bordered circle rather than an
`lv_arc`, which would mean stripping a knob and an indicator and then setting
background angles to a full 360° anyway. It sits at 462 px against the gauge
arcs' 420, so the two do not read as the same thing even though both go
`COL_BAD`.
⚠ On `lv_layer_top`, so `CLICKABLE` **and** `SCROLLABLE` must be cleared or it
swallows every touch on every screen. Both are set by default on a new object
in this LVGL (`lv_obj.c` constructor). An earlier version of this note claimed
`CLICKABLE` was not — that was wrong; clearing both was right anyway.

**The boot splash is painted inside `ui_boot_anim_start()`, before `setup()`
continues.**
`lv_scr_load()` only marks a screen active; nothing reaches the panel until a
refresh runs, and that otherwise happened only in `loop()`. So CAN, the GNSS
wake burst, the SD mount and the AP all came up against a black display. Uses
`lv_refr_now()`, not `lv_timer_handler()`: the refresh timer's period is 33 ms,
so a handler call that soon after the load finds it not yet due and paints
nothing.

**The animation is timed from the first `ui_boot_anim_done()` call in `loop()`,
not from when it was built.** Timing from creation meant a slow init burned the
whole window before a single frame was drawn, and the splash appeared for one
frame and vanished.

**Do not animate *into* visibility.** Animate position, not opacity from
transparent. If the animation does not run, an element animated from
`LV_OPA_TRANSP` stays invisible with no clue why; one animated from the wrong
position is merely misplaced.

**The splash stays until the car answers; on the bench it is a status
page.** (2026-10-13, c00924c.) A wake showed the gauges before any CAN data
existed, all reading "--", and the gauge looked broken. After its sweep the
splash now holds until a reading is fresh (oil, coolant, boost, rpm or
speed), or the "no car" fault would show anyway - the screens then say "--"
for a reason - or a touch. A wake that goes back to sleep never shows a gauge.
With a PC on USB the held splash is a status page instead, with no timeout -
build, WiFi, SD, GPS, CAN, RAM, chip temperature, uptime, reset reason -
since gauges mean nothing at the desk. ⚠ Montserrat 14 as built has no
middle dot: "·" drew as nothing, so the separators are bullets.

**Show an update on the gauge itself.** (2026-10-13, e1a57fd.) A firmware
upload from the app left the gauge silent for ~30 s and then restarting. It
now says "Updating firmware...", the megabytes received every 256 KB (a
multipart upload does not give its size up front), then "Update installed,
restarting..." - and clears the notice on a failed or aborted upload.

**Frame budget: what a frame costs, and why the g-meter is built the way it is.**
Measured 2026-09-26 with `FRAME_STATS` (serial stats plus an on-board bench,
`b`), because the first guess — QSPI bandwidth — was wrong for everything
except screen switches:
- **Fixed ~0.4 ms per dirty patch** (render setup plus one bus transaction),
  then roughly 1 ms for a 25 px dot and 1.3 ms for a small label.
- **Large curves are the expensive thing to render**, not pixels. LVGL's
  software renderer pays for every arc or line passing through a patch: the
  g-meter's two rings and two globe ellipses were 45 ms of a 108 ms full
  redraw, and made a 40×40 patch cost 4 ms against 0.7 ms elsewhere. They
  never change, so they are now drawn **once into a canvas in PSRAM** and a
  patch is a row copy (patch 0.95 ms, full redraw 68 ms).
- **A full-panel push is ~54 ms** whatever is drawn, so a screen switch cannot
  be 60 fps on this bus with a blocking flush.
- **The I2C bus runs at 400 kHz.** At 100 kHz the IMU poll was 1.55 ms of every
  loop pass (now 0.6). Every chip on the bus is rated for fast mode.
- **Loop pacing matters as much as render cost.** The refresh timer is only
  checked when `loop()` reaches `lv_timer_handler()`, and LVGL never catches up
  a late frame. A fixed `delay(5)` plus ~2.5 ms of other work made each frame
  one pass late (~52 fps from frames that each fit in budget). So the loop now
  waits briefly for a frame that is about to fall due, and the refresh period
  is 15 ms rather than 16 to absorb 1 ms tick rounding.
- **The g-meter's number is rate-limited to 10 Hz**; the dot runs at frame rate.
  A figure changing 60 times a second cannot be read, and it cost 1.3 ms a frame.
- **Nothing large and mostly empty goes on the top layer as one object.** LVGL
  picks what to draw per dirty patch by bounding box. The fault rim was one
  full-panel circle border, so every patch on every screen paid for it (~1 ms
  a patch) though no rim pixel was there. It is now 48 short line segments,
  each with a box hugging the edge: patches away from the rim skip it.
- **Boost was limited by its data, not by drawing.** With three values in
  turn at 100 ms, a boost sample arrived every 300 ms, so the needle stepped
  ~3 times a second however fast frames ran. Boost now takes every other
  request slot at 50 ms (10 Hz; oil and coolant 5 Hz), and the arc **glides**
  to each sample over two slots instead of jumping. The printed number shows
  each sample exactly on arrival; the arc trails it by at most one glide. A
  value change used to cost ~9 ms (4.2 of it the 48 px number); with the
  glide, frames average 1.8-3.1 ms, worst ~7 ms.

**Every OBD value ages out, and so does every warning derived from it.**
Oil had a timestamp and went to "--" after `OBD_STALE_MS`; boost and coolant
had none, so after a dropout they showed their last value forever, number and
arc. All three are stamped now, and each no-data path also zeroes its arc,
because "--" over a needle still standing at the last value is a frozen
reading that looks real. The over-temp latches clear with their data too: a
latched `overOil` with no oil reading made the screen say "COOLANT TOO HOT".

**The g-meter dot shows the force the driver feels, not the car's
acceleration.** Braking puts the dot up (thrown forward), accelerating down,
turning left right - a ball on the dashboard, the usual in-car convention.
Confirmed 2026-09-27. Beware the bench intuition: pushing the pod *backwards*
by hand reads as braking (dot up), not accelerating - to fake acceleration,
jerk it forwards. The other convention (plot the acceleration itself, as
data-logger g-g plots do) is both flips in config.h, and the tripwire test in
test_calc.cpp would have to be updated with it.

**The WiFi screen is never persisted and never dwelt on.**
The pod boots onto the remembered screen, so storing `VIEW_WIFI` means powering
up displaying the AP passphrase and a QR code that joins the network —
readable, or photographable, through the windscreen of a parked car. The QR is
the worse half: it does not have to be read, only photographed, and whoever
does it never needs to understand what they captured. Both the save *and* the
restore reject it, because a value already in NVS would otherwise keep coming
back forever while the code looked correct.

---

## WiFi and the web UI

**The AP lives at 4.3.2.1, outside RFC1918.**
Android's `NetworkMonitor` aborts its captive-portal probe when the DNS answer
is a private address, so on 192.168.4.1 the portal simply never opened. This
one line is the difference between the portal appearing and not.

**The QR code is the real fix for getting onto the settings page, not the
captive portal.** It depends on nothing the phone's OS has to choose to do.

**The AP passphrase is generated per device and stored in NVS, with no default
in the source.** So a leaked repository is not a leaked device. It also means
the WiFi screen is the only place it can be read, which is why that screen
exists at all.

**`/api/live` does not publish latitude and longitude.**
Nothing consumed them: the endpoint exists to say whether the receiver *works*,
which `haveFix`, `sats` and `hdop` answer. They were also the only personal
data on it, readable by anyone who joins the AP — and where a car is parked is
usually where its owner lives.

**The settings page sends `Cache-Control: no-store`.**
Otherwise a phone keeps serving a page from before the last reflash, which is
invisible and very confusing: a control added in firmware simply is not there,
or is there without its handler.

**The AP goes down while driving.** (2026-09-27, `AP_DRIVE_OFF`.) A 2.4 GHz
transmitter a few centimetres from the GNSS antenna competes with a
-130 dBm signal, and the AP is wanted when the car stands - settings,
downloads - not on the move. Driving is 20 km/h held 5 s; stopped is under
5 km/h or no speed held 60 s, so a red light does not restart it (each start
takes ~46 KB of internal RAM, and `esp_wifi` panics rather than fails).
Keyed on speed, not on the bus being alive: it is alive at the kerb with the
engine running. Never dropped while a phone is connected.
It was keyed on **GNSS** speed until 2026-10-24, and that was wrong in the
way that matters most: with no fix yet - a cold start, the first minutes of
nearly every drive - it counted as stopped, so the AP stayed up right
through the time the receiver most needs a clean sky. 2026-10-23 and 24's
logs: up 2¾, 5½ and 6 minutes into drives the car's own speed said were
drives within seconds. It now follows the gauge's state (`podstate.h`, see
Architecture): the car's speed (OBD), the GNSS's only while the car is
silent. ⚠ The premise is still unmeasured: the comparison drive with the AP
left up is still to do.

**New drives are announced in the AP's beacons, not over Bluetooth.**
(2026-10-22.) The phone should learn there is a drive it has not synced
without joining the AP. A BLE advertisement was the obvious tool and was
measured first: linking NimBLE alone makes the core keep the controller's
~21 KB of internal RAM, and with the AP up the pod fell to 13 KB free and
crashed - the AP needs 80 KB free just to start (`AP_MIN_INTERNAL_HEAP`). The
AP beacons ten times a second anyway, so the news rides in it: one vendor
element in the beacons and probe responses, `0xDD`, OUI `02:4E:47`
(locally administered), type 1, then 8 bytes. It costs no RAM, and it is
only on the air when the AP is - parked, never driving.
The 8 bytes are a fingerprint, not the session: BLAKE2b-8 of the log key's
public half, `"n-gauge beacon v1"` and the last drive's 6-character session
(`logcrypt_beacon`; zeros for the key when none is set, as the app assumes).
Only a phone holding the key can tell which drive it names; anyone else sees
8 bytes that change once a drive. A timestamp was the other candidate and
lost for the same reason - anyone could read it. The "last drive" is the
session that last opened a CSV or GPX, kept in NVS (`lastDrive`).
**A drive counts as known once any of it is on the phone.** A fingerprint
that also covered the file size would say "the rest of a drive continued
after a sync" - but parked, the logs keep growing, and it would change all
the time. The rest comes with the next sync anyway, which resumes by bytes.
The phone keeps every session it has ever had, synced or deleted since
(`beacon.json`): a drive deleted from both the phone and the gauge is still
known when the gauge names it.
Phone side: Android gives WiFi scan results only with the location
permission, and to a background check only with it "all the time" - so it
is an opt-in switch ("Tell me about new drives"), off by default. The check
is a WorkManager job every 15 minutes, the shortest periodic work allows; it
reads the phone's own cached scans (an app's own scan requests are throttled
to a few an hour in the background). It compares natively, with no JS - the
app hands it the fingerprints it knows - and posts one note per
fingerprint. The same scans give the AP's BSSID, which makes joining faster
(`WifiNetworkSpecifier`). Seen working 2026-10-24.

**No hidden SSID.** (Discussed 2026-10-11.) A hidden AP still sends beacons,
so it does nothing for the GNSS reason above, and Android joins hidden
networks less reliably - the QR flow would get worse to gain little.

---

## The phone app

**A phone app after all.** (Planned and started 2026-10-11, replacing both
the 2026-10-08 "not doing" and a hotspot-upload plan of the same day.) Expo
SDK 57 with expo-router; native code only for joining the pod's WiFi
(`modules/pod-wifi`). What it does that the pod's own pages could not: keep
the logs between syncs, hold the private key the logs are encrypted to, and
join the pod's network by itself.

**One main screen, the gauge in its header; a tap syncs.** (2026-10-13,
reworked 2026-10-18.) Like Garmin Connect shows its watch: the drives are
the app, titled "N Gauge"; the gauge is a ring in the header - grey, green,
red after a failed sync, a spinner while busy. A tap syncs where you are:
it connects, the sync runs by itself, the phone leaves the gauge's WiFi when
it is done (below) and a toast says what came over, or why it stopped. No
tabs. The main ⋮ has Settings - the phone's things: log key, logs folder,
About - and under it Gauge settings, the gauge's one screen: setting it up,
its settings, screen order, firmware, system, clock and g-meter zero. Gauge
settings connects by itself when opened and holds the connection while it
is open. There was a Gauge screen with live readings, reached only by
tapping the ring while connected; once the phone let go after every sync it
- and Gauge settings in its ⋮ - was out of reach, and the live readings
were not wanted, so it went. The app says "gauge", never "pod".

**Off the gauge's WiFi once synced.** (2026-10-16, cb647a0.) Connected
means no internet (below), so the phone lets go as soon as a sync has left
nothing to pull - unless a screen that needs the gauge is open (Gauge
settings, the log key), then once it closes. A sync that stopped short stays
connected: a tap on the ring tries again. "Disconnect after syncing" in
Settings switches it off; the gauge's firmware build is remembered from the
last sync for About.

**Connected means no internet.** The app joins the pod's network with
`WifiNetworkSpecifier` - Android's one-tap "connect to this device", with
mobile data still the default route - and binds its own process to it, or
requests to 4.3.2.1 would leave over mobile data. So while connected the
app has no internet: no map tiles, no place lookups. Everything that must
work right after a sync works offline - which decided how drives are named
(below). Gauge settings says so while connecting. The QR is read with
Google's code scanner: the gauge's ~12 mm code is too small for the in-app
camera without zoom.

**Sync: automatic, by name and size, smallest first.** (2026-10-11/13.) On
every connect, not on a tap - on 2026-10-12 two drives sat on the card while
the phone only "checked". The pod lists `{name, size}`; a file that grew is
fetched from where the phone stopped, by Range; one smaller on the pod than
on the phone is not the same file and is fetched whole. Smallest first, so a
sync cut short by the pod going to sleep has whole files, not half a GPX.
Only the pod's own logs are synced (it had been pulling firmware `.bin`s).
The pod serves one request at a time: a settings save or firmware upload
stops a sync first.

**No log server.** (Removed 2026-10-18.) The app could upload the logs to a
small server of our own (`backend/`, Node in Docker) - built, never
deployed, switched off behind a flag. Logs leave the phone as files instead
("Save all to a folder", synced on from there), so it went: the server, the
app's Upload card and server screen, and the inbox's uploaded/pending split
(read as one byte count by older phones' `inbox.json`).

**A drive deleted on the phone is deleted on the gauge.** (2026-10-14,
d45a40c.) Its names go on a delete-on-gauge list; every sync asks the gauge
to delete them before planning, and never pulls a listed name meanwhile. A
name leaves the list on "deleted", on 410, or once the gauge no longer has
it - so deleting while not connected, or during a drive still being
written, just waits.

**Each file is summarised once, and the summary cached by size and
version.** Parsing a 17 MB GPX takes a second or two; its summary is a few
hundred bytes, and the thinned track (10 m) is cached apart so the list never
loads tracks. `VERSION` is bumped whenever a summary's maths change, so old
caches are redone. ⚠ The summaries live in screen state, loaded once per
visit - never read from disk during render: the React Compiler memoised such
a read by file name, it never ran again, and rows stayed on "Reading..."
after their summary had landed.

**Bad fixes are dropped in the reader.** (2026-10-13, 8db3d6a.) The 23.9.
drive ended on a fix 13.5 km away and, after a tunnel, spent 10 s 2.6 km off
the road at "260 km/h" - top speed 261. The firmware's plausibility check is
newer than that file, so the app cleans what it reads: the track is cut into
runs at jumps (over 30 m at over 300 km/h), new segments and gaps over 20 s,
and a run under 30 s bordered by a jump is dropped - never the longest, and
not a short run between two plain gaps (between tunnels). Checked on all 33
tracks: only the three September ones changed; 23.9.'s top speed 261 ->
171 km/h. Files from before 2026-09-23 carry a bare `<speed>`, not the GPX
extension's - read as no speed at all, they showed 0 km/h tops (4b461b2).

**GPX and CSV pair by session; restart fragments hide.** By the session id in
their names (above), not by time. A drive under 200 m and 3 min (or car data
under 1 min) is a restart fragment or a blip: hidden, with a "Show N very
short drives" link, never silently dropped.

**Drives are named from place lists shipped in the app.** (2026-10-14,
9095a3c.) "Kolín → Příbram", "Around Kolín" for a loop. Offline on purpose:
no coordinates leave the phone, and drives are summarised right after a sync,
on the gauge's WiFi, with no internet. GeoNames lists for CZ and its
neighbours (SK AT DE PL HU, ~1.9 MB in the APK), built by
`scripts/make-places.ts`:
- **Every populated place is kept, those of 100 people or more marked
  known.** A known place within 3 km wins over a nearer hamlet: in a city the
  quarter people say, not a cluster of houses by the road that happens to
  have its own entry. Filtering by population instead lost whole regions:
  GeoNames has it for a quarter of Slovak and Polish villages, under a tenth
  of Hungarian ones.
- **A list is read only while a new drive is summarised,** as an asset file,
  and let go; the two names are cached with the summary. A required JSON
  would have stayed in memory for the whole run (Germany: 82 k places).
- **Your own places win within 300 m,** checked on every render, so renaming
  "Home" renames every drive from it. They exist because a point per quarter
  cannot know where a quarter ends: at the far edge of a large quarter, the
  point of a smaller one across a river can be nearer than its own.

**Logs become readable again only in the app.** (2026-10-11.) The key is on
the phone, so the phone hands logs on decrypted: share a drive's GPX or CSV,
or save to a folder - plain names, files already there skipped, the folder
remembered (Android's persistable permission). No PC tool.

**The map is MapLibre in a WebView, on OpenFreeMap tiles.** No API key and no
native map SDK to build. Pinned to maplibre-gl 5: version 6 no longer ships
the plain script build a page without a bundler needs. Its credit is a line
under the map in the app's style - MapLibre's own box opened as a white bar.

**The version comes from git.** (2026-10-13.) Every APK was 0.1.0 /
versionCode 1, so which build was on the phone could not be told. versionCode
is the commit count, the version `0.1.<count>`, and About shows the commit
(+changes when built from uncommitted work). `build-apk.sh` runs
`expo prebuild` every time, since that is what writes the version into the
Android project. ⚠ Another rewrite of the history would lower the count, and
Android refuses a lower versionCode as an update.

---

## Working practice

**Check the wiring and the supply before touching firmware.**
A loose breadboard connection to the buck produced "splash for half a second,
then black" and four wrong firmware explanations in a row. The tells were all
present: it was **intermittent** (worked, stopped, worked again with no code
change), it happened **only on buck power and never on USB**, and the panel
went black when nothing in the firmware can do that except the sleep path.
Three checks settle it, none of them firmware archaeology: is the buck still
putting out 5 V while the screen is black (ten seconds with a meter); does
the PCF85063 report a lost oscillator at the next boot (`rtc_init()` prints
it - deep sleep keeps 3.3 V up, so that flag means the rail really went); is
there a sleep record (`SLEEP_LOG_NVS` 1 - it survives having no card and no
serial).

**Verify the instrument before explaining the measurement.**
Six "hardware faults" on this bench have been measurement or tooling artifacts:
a stale serial buffer read as a crash, the bench supply's own OVP reported as a
dead buck, a phantom I2C device at 0x7E, a handheld meter reading 1.14× high
(which alone produced two false alarms and blocked car testing for weeks), my
own `grep` filters hiding the evidence three times, and the loose connection
above.

**A serial capture is an instrument too, and it has two ways to lie.**
On 2026-09-25 a "the board stops printing after 38 s, and what it does print is
garbled" finding turned out to be entirely the capture:
- A capture launched detached (`nohup … &`) reports "finished" as soon as the
  *launcher* exits. Reading its log then means reading it 40 s into a 7-minute
  run and mistaking "not written yet" for "stopped". Run the capture itself as
  the task, so "done" means done.
- Two processes reading the same tty split its bytes between them, which looks
  exactly like dropped characters. Only ever have one reader open.
A proper single-reader 7-minute run then showed 172 of 173 lines clean and no
disconnects. What *was* real that day was a USB disconnect with
`error -71` on re-enumeration: a protocol error, i.e. the cable or connector.

**The USB drops on the bench: the connector after all.** *Revised
2026-09-27.* The "port churn" theory below did not survive: on 2026-09-27 the
board dropped **2 s after being plugged in**, while the firmware was still
compiling and nothing had opened the port, with the pod "maybe moved a
little". A drop with no port activity at all, after a small movement, is a
mechanical contact - the connector or cable - which is also what the
original `error -71` pointed to. The long clean holds were most likely just
long stretches with the pod untouched. What stands from the earlier work:
sleep logic, host autosuspend and crashes really are ruled out. Practical
rules: do not move the pod while flashing or capturing, prefer one long
reader over many short captures, and suspect the connector first next time.

The earlier analysis, kept for the record:

**(Superseded) The USB drops on the bench track port churn, not the cable.**
On 2026-09-25/26 the board fell off USB repeatedly: the kernel logged
`USB disconnect`, re-enumeration failed, and it stayed gone until replugged,
while the board itself kept running (screen and swipes fine). Ruled out, each
with evidence:
- **Our sleep logic**: USB presence latches sleep off for the power cycle, and
  both sleep windows are 5 minutes; drops came 20-60 s after reconnecting.
- **Host autosuspend**: `power/control` is `on` for the device and
  `runtime_suspended_time` stayed 0.
- **A crash**: a reset re-enumerates within seconds (a flash reset does not
  even drop the link); these drops stayed down.
Then one reader held the port open continuously: **26 min, then 2 h 41 min
(the second straight after a flash), no drop at all**, on the same cable, the
pod untouched. Every earlier drop came during a stretch of repeated
open/close/reset - flashes and short captures back to back. So the working
theory is that the ESP32-S3's USB-Serial-JTAG link is fragile under port
churn, not that the connector is loose. Unproven: a drop during a long hold
would reopen the cable question. Practical rule meanwhile: prefer one
long-running reader over many short captures, and on a drop, check the
kernel log (`journalctl -k`) before blaming hardware.

**On buck power the board has no serial console and usually no SD card.**
Every normal diagnostic is blind there, and theorising against an unobservable
system produces plausible stories indefinitely. Make the board report its own
state first — a notice on the panel before sleeping, plus a sleep ring in NVS
that proves its own write path, took ~30 lines and then the answer was
immediate. Build the instrument before the third hypothesis.

**Silent early returns are a bug, not a style.**
"Nothing was recorded" and "this code is not running" must not look identical.
The NVS sleep log shipped that mistake twice in one feature — a size check and
a read-only `Preferences::begin()` that fails when the namespace does not exist
yet — after an afternoon lost to exactly that ambiguity.

**Prove a write path when its absence is the evidence.**
The NVS sleep log writes a sentinel, reads it back and clears it when the
namespace is missing, so "empty" can be reported as *verified* empty. The RTC
did the same at bring-up: a later boot read back the sentinel plus 2 min 12 s
of elapsed time, which proved both the write path and that the oscillator runs.
