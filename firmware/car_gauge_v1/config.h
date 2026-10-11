// ============================================================================
// config.h  —  i30 N OBD Gauge (v1)  —  all tunables in one place
// ============================================================================
#pragma once

// ---- Board / CAN pins (Waveshare ESP32-S3 AMOLED 1.75" header) -------------
// Broken-out header: 18, 17, 16, RX, TX, 3.3V, G, 5V - five usable GPIOs, all
// five now assigned. RX/TX are GPIO44/43; the labels describe UART0's default
// function, not a restriction, because the ESP32 routes UART through the GPIO
// matrix. They are free for us at all because CDCOnBoot=cdc puts Arduino's
// Serial on the native USB rather than UART0.
//
// Only ONE signal here has a hard pin requirement: CAN_RX is the ext1 deep
// sleep wake source, so it must be RTC-capable (GPIO 0-21). GPIO43/44 are not.
// CAN_RS wants RTC-capable too, for an unconditional pad hold through sleep -
// which is why the GNSS UART was moved onto 43/44 to free GPIO18 for it. The
// GNSS has no RTC requirement at all, so it pays nothing for the move.
//
// BOARD_CARRIER picks the CAN TX/RX pair for the hardware in use:
//   0 = the hand-wired prototype: TX GPIO17, RX GPIO16 (as soldered).
//   1 = the carrier PCB (hardware/carrier): TX GPIO16, RX GPIO17. Swapped
//       there so the pair runs straight to the transceiver socket instead of
//       crossing. Both 16 and 17 are RTC-capable, so either can be CAN_RX (the
//       ext1 wake source); CAN_RS stays on GPIO18 on both.
// ⚠ Set to 1 when moving to the carrier, or CAN is silent: TX and RX reversed.
#define BOARD_CARRIER      0
#if BOARD_CARRIER
#define CAN_TX_GPIO        16     // TWAI TX -> transceiver CTX / "CAN TX"
#define CAN_RX_GPIO        17     // TWAI RX <- transceiver CRX / "CAN RX"
#else
#define CAN_TX_GPIO        17     // TWAI TX -> transceiver CTX / "CAN TX"
#define CAN_RX_GPIO        16     // TWAI RX <- transceiver CRX / "CAN RX"
#endif
#define GPS_RX_GPIO        44     // GNSS module TX -> here (header "RX")
#define GPS_TX_GPIO        43     // -> GNSS module RX (header "TX")
// Wiring for the GY-GPSV3 / NEO-M9N: module VCC -> 3V3, GND -> GND,
// module TX -> GPIO44, module RX -> GPIO43. Straight through, on the two pins
// actually silkscreened RX/TX.
//
// The direction matters, and is not interchangeable. GPIO43 is UART0 TX, which
// the ROM bootloader drives for the first ~100 ms of every boot:
//   - as GPS_TX (our output) that is harmless - the ROM and we are the only
//     drivers, and the module just sees boot-log garbage and ignores it.
//   - swapped, the module's TX output would fight the ROM's for that window,
//     two push-pull CMOS drivers on one wire. Don't.
// GPIO44 is UART0 RX, an input for the ROM, so the module driving it never
// contends.
//
// Set GPS_TX_GPIO to -1 if that wire isn't connected; the module then runs at
// whatever it defaults to (1 Hz) and configuration is skipped.

// ---- CAN transceiver standby (SN65HVD230 "Rs", pin 8) ----------------------
// Rs is a three-state input selected by voltage, so one GPIO covers it:
//   low  (<0.33*Vcc)   high-speed mode   ~10 mA
//   resistor to GND    slope control     ~10 mA
//   high (>0.75*Vcc)   standby           ~370 uA
//
// Standby disables the driver but keeps the RECEIVER alive, which is the whole
// reason this is usable: RXD still follows the bus, so the CAN-edge wake in
// goToSleep() still arms and the first frame after the key turns still wakes
// us. Measured worth: the transceiver is 3.2 mA of the 14 mA parked draw
// (10 mA with it fitted vs 6.8 mA without), so this recovers ~2.8 mA.
//
// Only the HVD230 keeps its receiver alive in standby. An HVD231/232 shuts the
// receiver down too, which would break the wake path silently - the board
// would sleep through the car starting. Confirm the part before trusting this.
//
// Wiring. The breakout does not bring Rs out to a header (only VCC/GND/RX/TX/
// CANH/CANL), so the wire goes onto IC pin 8 itself - a corner pin of the
// SOIC-8, the easiest on the package to tack onto - or onto the R1 pad that
// connects to it.
//
// NOTHING NEEDS CUTTING. Per the Waveshare reference schematic that these
// boards clone (CAN_board.SchDoc, 2011), Rs is not hard-grounded: it goes to
// GND through R1 = 10K, i.e. the board ships in slope-control mode, not
// high-speed mode. A 10K pull-down is weak enough for a GPIO to simply
// override:
//   driven low   Rs = 0 V                        high-speed mode
//   driven high  Rs = 3.3 * 10k/(10k+~40) ~3.3V  standby (threshold 0.75*Vcc)
// The only cost is 330 uA through R1 while Rs is high, so the saving is ~2.5 mA
// rather than the full ~2.8 mA.
//
// Fit ~1k in series anyway. Not for the reference design - for the fact that
// these are clones and may deviate. With R1 present, 1k still leaves Rs at
// ~3.0 V, comfortably over threshold; if a clone DID hard-ground Rs, the 1k
// caps the fault at ~3.3 mA and the only symptom is that standby never
// engages, instead of a damaged pin. Check before soldering with an ohmmeter
// from pin 8 to GND, unpowered: ~10k is the reference, ~0 ohm is a hard tie.
//
// R1 also makes the failure mode benign: if the pad hold ever did not survive
// sleep, Rs falls to 0 V and the transceiver reverts to high-speed mode - it
// wastes 3.2 mA but never loses the ability to receive, so the CAN wake still
// works. This fails toward working, not toward a board that sleeps through the
// car starting.
//
// UNRELATED BUT FROM THE SAME SCHEMATIC: R2 = 120 ohm is a FIXED bus
// termination across CANH/CANL, with no jumper. The vehicle bus is already
// terminated at both ends, so tapping OBD-II with this board leaves a third
// terminator on a terminated bus (~60 ohm -> ~40 ohm), overloading every
// transmitter on it. It usually still works, which is why it goes unnoticed,
// but it is out of spec - remove R2. It is a far more accessible part than
// pin 8.
#define CAN_RS_CONTROL     1
#define CAN_RS_GPIO        18     // freed by moving the GNSS UART to 43/44.
                                  // RTC-capable (0-21), and the pad hold in
                                  // obd_transceiver_standby() is measured to
                                  // survive deep sleep on it.
//
// Verify on the meter once wired: with Rs held high through sleep the parked
// draw should fall from ~10 mA to ~7.5 mA. If it does not move, suspect the
// solder joint on pin 8 or a clone that hard-grounded Rs - not the hold, which
// is unconditional now that this sits on an RTC pad.

// Navigation rate. 40 ms = 25 Hz, the NEO-M9N's maximum - and per its
// datasheet that 25 Hz holds for all constellation combinations, including
// GPS+GLONASS+Galileo+BeiDou concurrently, so it costs no accuracy.
#define GPS_NAV_RATE_MS    40

// 25 Hz of GGA+RMC is ~4000 B/s ≈ 40 kbps, which does NOT fit 38400 baud, so
// the module is moved to 115200 as part of configuration. The datasheet warns
// about exactly this ("make sure the UART baud rate being used is sufficient
// for the selected navigation rate"). GSV/GSA/GLL/VTG are turned off, which is
// what keeps even 115200 comfortable.
#define GPS_TARGET_BAUD    115200
#define GPS_BAUD_TRY_MS    2500   // no NMEA for this long -> try the next baud

// The GNSS runs on its own task on core 0, like CAN, so its UBX handshakes
// cannot stall the renderer. These two are the task's pacing.
//
// GPS_POLL_MS must keep the UART drained: at 25 Hz with GGA+RMC that is about
// 3.7 kB/s, and the default 256-byte RX buffer fills in ~70 ms, so anything up
// to ~20 ms is safe. 5 ms leaves generous margin at negligible cost.
#define GPS_POLL_MS        5
// How long after boot before "no GPS module" is reported. Must exceed one
// full baud hunt - five candidate rates at GPS_BAUD_TRY_MS each, 12.5 s -
// or the notification rim would flash amber after every boot while the link
// is still being found at a rate it simply has not reached yet.
#define GPS_MISSING_GRACE_MS 15000
// The same for "no CAN": after a start this car's gateway keeps the OBD bus
// silent for ~20 s (2026-10-07/08 logs), so a red rim before then reports a
// normal state. Also held back for as long as a deep-sleep wake is still
// waiting for the bus (60 s after a motion wake). The values show "--"
// meanwhile, as they do without data anyway. A CAN driver that failed to
// start is a real fault and is shown at once.
#define CAN_MISSING_GRACE_MS 30000
// How long gps_park() waits for the task to confirm it sent the PMREQ. Only
// reached on the way into deep sleep, so a few hundred ms costs nothing - and
// it is bounded so a wedged task cannot block sleeping forever.
#define GPS_PARK_ACK_MS    400
#define GPS_FIX_STALE_MS   5000   // fix older than this counts as "no fix"
#define GPS_RAW_DUMP_BYTES 64     // hex/ascii dump of the first bytes, for bring-up
#define GPS_TIME_RESYNC_MS 600000 // re-set the system clock from GPS this often

// 1 = put the module into backup mode (UBX-RXM-PMREQ) before deep sleep.
//
// Measured: the GPS is 17.2 mA of a 24 mA parked draw - 72 %, more than the
// converter, the board and everything else combined. It is also ~36-42 mA at
// the module itself, well above the ~25 mA datasheet figure, because that
// figure is quoted at 1 Hz and we run it at 25 Hz. Parked, it is computing
// twenty-five position solves a second, forever, for a stationary car, and
// nothing reads them.
//
// ⚠ The risk this flag exists for: if the module does not wake reliably, GPS
// is dead for the whole of the next drive and the GPX file never appears. Set
// to 0 to rule it out. Backup mode retains the ephemeris, so a successful
// wake reacquires FASTER than a cold start, not slower.
#define GPS_SLEEP_ON_PARK  1
// Bytes of 0xFF sent at startup to wake a module in backup mode. Any UART
// activity wakes it; the datasheet asks for a dummy byte because the first one
// is consumed rousing the receiver rather than being parsed.
#define GPS_WAKE_PULSE_LEN 8
// Settle time after the wake burst, before we start expecting NMEA. The
// datasheet quotes a few hundred microseconds to leave backup; this is
// generous because getting it wrong costs a whole drive's GPX, and it is paid
// once per boot.
#define GPS_WAKE_SETTLE_MS 50

// ---- Buttons ---------------------------------------------------------------
// The board's "programmable buttons". Confirm GPIOs from Waveshare BSP.
#define BTN_NEXT_GPIO      0      // TODO: verify — cycle gauge screens
#define BTN_NEXT_ACTIVE_LOW 1

// ---- CAN / OBD -------------------------------------------------------------
// OBD diagnostic CAN is 500 kbit/s - fixed in obd.cpp (TWAI_TIMING_CONFIG_500KBITS).
// Requests go out ON REPLY: the next one OBD_REQ_GAP_MS after the previous
// answer lands, or after OBD_REQ_TIMEOUT_MS if none does. It used to be a
// fixed 50 ms timer, but the link benchmark (2026-10-04, 3 drives, 0
// timeouts) measured the engine ECU answering a single PID in 6.7 ms on
// average (max ~17), the multi-frame oil DID in ~27 ms (max ~41), and a
// 6-PID request in ~10 ms - so the timer left most of every slot idle.
// The gap is a courtesy to the ECU, not a measured need.
// What is asked, and how often, is the `schedule` table in obd.cpp.
#define OBD_REQ_GAP_MS      5
#define OBD_REQ_TIMEOUT_MS  150
// The oil DID (oil, and coolant as the dash shows it) once a second: both
// change by well under 1 °C/s, and the over-temp warnings can afford 1 s.
// Every other slot goes to the 6-PID request, so boost gets nearly all of
// them: ~65/s expected (was 9.5/s on the 50 ms timer).
#define OIL_POLL_MS         1000
// The fast request: six Mode 01 PIDs in ONE request to the engine ECU, which
// answers all of them in one reply (benchmark: "41 0B xx 05 xx 0D xx 43 xx xx
// 33 xx"). 0x0B MAP (boost), 0x05 coolant sensor (logged), 0x0D speed (brake
// gate), 0x43 absolute load (torque estimate), 0x0C RPM (track log), 0x49
// accelerator pedal. Sent to 0x7E0, not the broadcast 0x7DF, so the gearbox
// ECU does not answer too. Six is the most Mode 01 allows per request.
// RPM took baro's place (2026-10-08): baro, the zero for boost, changes only
// with weather and altitude, so it has its own slow entry (BARO_POLL_MS).
#define OBD_MULTI_PIDS      { 0x0B, 0x05, 0x0D, 0x43, 0x0C, 0x49 }
#define BARO_POLL_MS        60000
// For the track log (2026-10-09): 0x0E timing advance - where the ECU pulls
// ignition timing (knock, heat), which is also when the torque estimate
// reads high - and 0x0F intake air temperature (heat soak). Their own small
// request, 5 Hz: timing moves within a gear, but not at boost's rate. Costs
// ~7 ms in every 200 (~3.5 % of the fast request's slots).
#define OBD_SLOW_PIDS       { 0x0E, 0x0F }
#define OBD_SLOW_POLL_MS    200

// A value older than this is treated as NO value. This matters more than it
// looks: a frozen number on screen is indistinguishable from a steady reading,
// so without it a dead CAN link would show a plausible oil temp forever and
// the over-temp alarm would never fire. Stale data must read "--".
#define OBD_STALE_MS       5000

// The OBD probe (OBD_PROBE: supported PIDs per ECU, torque DIDs, a bus
// census) and the link benchmark (OBD_BENCH: reply latency per request type)
// were removed 2026-10-09, once answered - see DECISIONS.md: no torque PIDs
// or DIDs, pedal 0x49 supported, a gateway in front of the port, and the
// timings behind OBD_REQ_GAP_MS.

// Firmware updates without USB - see ota.h. A new image confirms itself
// after this long (or just before sleep); a reset before that rolls back.
#define OTA_HEALTHY_MS       30000
#define OTA_SD_PATH          "/firmware.bin"            // put the .bin here

// 1 = bench test of the IMU's wake-on-motion, before it is trusted to wake
// the pod: the QMI8658 is armed in WoM mode on INT2 (GPIO21) at boot and
// every change on GPIO21 prints "[WOM] ..." with the time and the STATUS1
// motion flag. Type a number + Enter on serial to re-arm at that threshold
// in mg. Stops the g-meter (WoM mode outputs no data). ⚠ 0 for the car.
// Bench results 2026-10-08, pod on the desk next to the keyboard: at 100 mg
// three minutes of typing gave nothing and a hand pickup fired at once; at
// 50 mg typing alone fired bursts of 200 events. Car default 80, tunable.
#define WOM_BENCH          0
#define WOM_BENCH_MG       100     // starting threshold, mg

// Wake on motion: the IMU wakes the pod (ext0 on GPIO21 = QMI8658 INT2) when
// the car is opened or got into, so it is up before the engine runs - the
// gated OBD bus only wakes it ~20 s after a start. Threshold is a phone
// setting (settings.motionWakeMg, 0 = off). After a motion wake the bus is
// waited for MOTION_WAKE_SAMPLE_MS, not WAKE_SAMPLE_MS, since it comes ~20 s
// after the start. MOTION_FRUITLESS_MAX motion wakes in a row with no car
// (wind, passers-by) stop motion being armed until a timer wake or a drive -
// the same self-healing backoff as the CAN chatter guard.
#define MOTION_WAKE_GPIO         21
#define MOTION_WAKE_MG_DEFAULT   80
#define MOTION_WAKE_SAMPLE_MS    60000
#define MOTION_FRUITLESS_MAX     6
// After arming, INT2 flips to its idle level (~45 ms, seen on the bench) and
// the sensor ignores its first 32 samples (250 ms at 128 Hz); sample the
// level only after both, or ext0 fires the instant we sleep.
#define MOTION_ARM_SETTLE_MS     300
#define MOTION_ARM_STABLE_MS     200    // then the level must hold this long

// Torque ESTIMATE from absolute load (PID 0x43). No torque PID or DID exists
// on this car (probe, 2026-10-03/04); on a turbo petrol absolute load keeps
// rising with boost, unlike calculated load. Calibrated on full-throttle
// pulls (2026-10-04, 3 drives): absolute load peaks at 226-232 %, mapped to
// the 2021 facelift's rated 392 Nm -> 392 / 230 = 1.70 Nm per %. Proportional
// through zero; reads high when timing is pulled (knock, traction control).
// Shown as "~NNN Nm" inside the boost arc.
#define TORQUE_RATED_NM        392
#define TORQUE_PEAK_LOAD_PCT   230.0f
#define TORQUE_NM_FROM_LOAD(p) ((p) * TORQUE_RATED_NM / TORQUE_PEAK_LOAD_PCT)
#define TORQUE_ARC_MAX_NM      400    // full scale of the inner arc

// Gear, from rpm over OBD speed (calc_gear), for the track log and the drive
// CSV - the DCT picks its own gears, but in any gear rpm / km/h is fixed.
// Measured 2026-10-10 from the drive CSVs of 10-08/09 (3957 samples at >= 12
// km/h): the peaks of rpm per km/h, 1st gear first. Steps 1.65, 1.47, 1.33,
// 1.25, 1.22, 1.25 - a real gearbox's shrinking progression. 8th from the
// 2026-10-10 motorway stretch at 110 km/h: 16.1 (6 samples, step 1.24). A 0
// entry would mean "not measured" and never match. GEAR_TOL is half the gap
// between the two closest gears (6th and 7th), so no two gears overlap;
// outside it (shifting, clutch slip, neutral) the gear is 0. Below
// GEAR_MIN_KMH a whole km/h is too coarse.
#define GEAR_RPM_PER_KMH  { 122.0f, 74.0f, 50.5f, 37.6f, 30.5f, 25.2f, 20.0f, 16.1f }
#define GEAR_TOL          0.07f
#define GEAR_MIN_KMH      10.0f

// Accelerator pedal bar on the boost screen, from PID 0x49 (pedal position
// D, raw byte, x100/255 = %). Foot off reads 37 (14.5 %), measured on the car
// 2026-10-03. Floored it reads 212-213 (83 %), measured on full-throttle
// pulls 2026-10-07; 212 so a floored pedal reliably shows a full bar.
#define PEDAL_RAW_IDLE         37
#define PEDAL_RAW_FULL         212

// ---- Watchdog --------------------------------------------------------------
// The entire point of this device is to warn about oil temperature, so a hung
// loop must not be allowed to sit there displaying a stale number. If loop()
// stops feeding the watchdog for this long the board reboots and comes back
// working, which is strictly better than a pretty frozen screen.
#define WDT_TIMEOUT_MS     8000

// ---- Oil temperature -------------------------------------------------------
// Two ways to get it. Try standard first; if the car NAKs it, use Mode 22.
//   Get these from the sniffer session + the Car Scanner "i30 N" PID profile.
// Mode01 PID 0x5C is NOT supported by this car: 193 requests, zero replies
// (not even a NAK) in log_0015.csv. Enhanced Mode22 is the only way.
#define OIL_USE_MODE22     1              // 1 = enhanced Mode22 DID, 0 = Mode01 0x5C
#define OIL_MODE22_DID     0xE001         // verified responding on this car

// DID 0xE001 answers with a 45-byte ISO-TP **multi-frame** payload, so it
// needs Flow Control + Consecutive Frame reassembly - implemented in obd.cpp
// (handleFrame / isoSendFlowControl).
//
// Oil temp is ONE byte at data offset 34.
//
// ⚠ It was offset 25 for a long time, and 25 is COOLANT-DERIVED, not oil. The
// forum thread this came from specifies the byte as token `AI`, in the
// Torque/Car-Scanner letter notation where A is the first data byte: A=0 ...
// Z=25, then AA=26, AB=27 ... AI=34. `AI` was read as a single letter, which
// lands on Z=25. Our indexing needs no rebasing - obd.cpp sets d = &p[3],
// skipping `62 E0 01`, so d[0] IS token A.
//
// Then the mistake was confirmed rather than caught: every byte was ranked by
// correlation against the coolant warm-up curve, and byte 25 won at r=0.998
// precisely BECAUSE it is coolant. That criterion cannot distinguish a coolant
// copy from the real sensor - it actively selects the copy, because real oil
// temp lags and therefore correlates less well.
//
// The discriminator is LAG, not correlation. From drive_0181.csv (2026-09-23,
// 16056 payloads across a cold start, coolant 27 -> 95 °C):
//
//   byte 25 (Z):  rho +0.981 vs coolant, reaches half its rise at t+170s
//   byte 34 (AI): rho +0.538 vs coolant, reaches half its rise at t+370s
//   coolant:                             reaches half its rise at t+170s
//
// Byte 34 lags coolant by 200 s, shares byte 25's raw range (101-193, so the
// same encoding), and is confirmed by two things our own data cannot fake:
//   - When coolant first hit 70 °C, byte 25 decoded to 68 °C and byte 34 to
//     46 °C. The instrument cluster showed ~50 °C. That gap - gauge reading
//     ~70 while the dashboard read ~50 - is the bug report that started this.
//   - Under load (t+2700-3000) byte 34 runs 1-4 °C ABOVE coolant. A
//     coolant-derived value cannot exceed coolant; worked oil can, and does.
//   - At the fully warm plateau, coolant 90.5 °C and byte 34 88.9 °C: they
//     converge, which is what oil and coolant do once heat-soaked.
//
// Formula unchanged, and NOT refitted - the identical raw range says both
// bytes use the same encoding, and it now validates at both ends independently:
// cold-soaked at t=0 it gives 25.9 °C against coolant 27 °C (engine cold, oil
// and coolant necessarily equal), and it converges at the warm plateau. And
// at the hot end: 2026-10-04, raw 201 -> 100.2 °C on a hard drive while the
// dashboard showed ~100 °C. OIL_WARN_C (120) is now a short extrapolation
// from 100, not from 94. The ECU only reports some raw values (175, 176,
// 179, 180, 183, 185, 188, 191, 193, 196, 201 seen) - steps of ~1.5-4 °C,
// coarser when hot - so the reading moves in jumps; that is the source.
//
// Two separate macros on purpose: the Mode22 payload is 42 data bytes and we
// index byte 34, while a Mode01 reply is a single byte in an 8-byte frame.
// Using one macro for both would read far out of bounds on the Mode01 path.
// In both, `d` points at the first data byte (after the echoed DID / PID).
#define OIL_MODE22_OFFSET        34
#define OIL_TEMP_FROM_MODE22(d)  ((int)((d)[OIL_MODE22_OFFSET] * 0.7435f - 49.2f))
#define OIL_TEMP_FROM_MODE01(d)  ((int)(d)[0] - 40)

// Coolant as the DASHBOARD shows it: byte 25 of the same DID, same encoding.
// Mode 01 PID 0x05 read 90-93 °C on every warm drive while the cluster stayed
// under 90 (2026-10-03, noticed in the car). Byte 25 sits 2-3 °C below PID
// 0x05 across 70-95 °C (88.1 at PID 90, 89.8 at PID 92), which matches the
// cluster - the earlier "coolant-derived value" finding, now put to use. It
// shares the encoding of byte 34, the oil value the dashboard confirmed.
// The screen, the coolant warning and the CSV's coolantC use this; PID 0x05
// is still polled and logged (coolantPid05C) as the raw sensor. Note the
// warning therefore trips ~2-3 °C (of PID 0x05) later than before.
#define COOLANT_FROM_E001        1
#define COOLANT_MODE22_OFFSET    25
#define COOLANT_FROM_MODE22(d)   ((int)lroundf((d)[COOLANT_MODE22_OFFSET] * 0.7435f - 49.2f))

// ---- Boost -----------------------------------------------------------------
// Easy start: Mode01 PID 0x0B (intake manifold abs pressure, kPa).
// Later swap to the enhanced charge-pressure DID for true boost.
#define BOOST_USE_MODE22   0
#define BOOST_MODE22_DID   0x0000         // <<< TODO if using enhanced boost
#define BOOST_MODE01_PID   0x0B
// The boost arc glides to each new sample over this long rather than jumping.
// Boost arrives every ~15 ms (a 6-PID reply ~10 ms + the gap), ~45 ms once a
// second when the oil DID takes a slot; this covers the usual gap with
// margin and barely lags.
#define BOOST_GLIDE_MS     30
// Expected mean time between boost samples. Only the bench render test and
// synthetic input use it.
#define BOOST_SAMPLE_MS    15
// Boost = manifold absolute pressure minus ambient. Ambient is the car's own
// barometric reading (PID 0x33) once it has answered; AMBIENT_KPA only until
// then. Measured 98-99 kPa on 2026-10-04, so the fixed 101 read 0.02-0.03 bar
// low. Below zero is real manifold vacuum (part throttle: ~-0.2 cruising,
// ~-0.5 idling) - logged as such, shown as 0.00 (see ui_update_boost).
#define AMBIENT_KPA        101
#define BOOST_BAR_FROM_KPA(kpa, ambient)  (((float)(kpa) - (float)(ambient)) / 100.0f)

// ---- Tyre pressures (TPMS) -------------------------------------------------
// Read from the TPMS module (request 0x7A0, DID 0xC00B) - NOT the engine ECU.
// Sensors are motion-activated and sleep when parked, so expect no readings
// until the car has been driven for a while.
// Red-tyre limits, per axle: this car runs the fronts 0.3 bar harder than the
// rears, so one pair for all four either misses a soft front or flags a
// correct rear. From the owner's manual, i30 N Fastback 235/35 R19 DCT,
// normal load (cold): front 2.7, rear 2.4 bar; maximum load 3.0 / 3.1.
// Low = spec - 0.3. High = maximum-load spec + 0.3, since warm tyres read
// ~0.2-0.4 above their cold pressure and must not go red on a normal drive.
// Defaults only - editable from the phone settings page.
#define TPMS_LOW_BAR_FRONT   2.4f
#define TPMS_HIGH_BAR_FRONT  3.3f
#define TPMS_LOW_BAR_REAR    2.1f
#define TPMS_HIGH_BAR_REAR   3.4f
#define TPMS_BAR_FROM_RAW(raw)  ((raw) / 5.0f * 0.0689476f)   // raw/5 psi -> bar
#define TPMS_TEMP_FROM_RAW(raw) ((int)(raw) - 50)

// The TPMS lives in a DIFFERENT module to the engine ECU, so the request goes
// to 0x7A0 (reply 0x7A8), not 0x7E0/0x7E8.
#define TPMS_REQ_ID        0x7A0
#define TPMS_MODE22_DID    0xC00B
#define TPMS_WHEELS        4
// ✅ Layout SOLVED against the dashboard (drive_0058/0059: front 2.6/2.6,
// rear 2.4/2.4 bar). The payload is 20 bytes; bytes 0-3 are status flags
// (FF FF 00 00) and each wheel then occupies FOUR bytes, not five:
//   pressure at 4 + i*4   -> raw 191, 192, 178, 176 = 2.63, 2.65, 2.45, 2.43 bar
//   temperature at 5 + i*4 -> 34, 36, 31, 31 C
// The temperatures independently corroborate the order: the i30 N is front
// wheel drive, so the fronts do the driving and most of the braking and run
// measurably hotter. The earlier stride-of-5 guess put wheels 2-4 at offsets
// 9/14/19, which read garbage and ran off the end of a 20-byte payload.
//
// Front/rear is confirmed. LEFT vs RIGHT was first assumed FL, FR, RL, RR
// (equal pressures within each axle could not tell them apart); checked
// against the dashboard 2026-10-04: fronts right, rears crossed. So the
// payload order is FL, FR, RR, RL - the screen's slots are FL, FR, RL, RR.
#define TPMS_PRESSURE_OFFSET { 4, 8, 16, 12 }
// How many entries of the table above are trusted. Slots beyond this are
// reported as "not present" rather than shown as bogus pressures.
#define TPMS_TRUSTED_WHEELS 4
// How long a tyre reading stays on screen after its last good value. The
// module answers intermittently while the sensors are waking - a single reply
// with 0x00/0xFF used to blank that wheel at once, which on the car showed as
// pressures appearing, vanishing, then coming back. Tyre pressure changes
// slowly, so holding the last good reading for a couple of minutes is both
// calmer and more honest than flicking to "--".
// Three polls' worth, so one missed or empty reply does not blank a wheel.
#define TPMS_STALE_MS      (3 * TPMS_POLL_MS)

// Once a minute, first poll as soon as the car is on. Tyre pressure barely
// moves, and the wheel sensors themselves transmit rarely; asking every 5 s
// (as before) only spent request slots on repeats.
#define TPMS_POLL_MS       60000

// ---- Thresholds (°C) -------------------------------------------------------
#define OIL_READY_C        90     // below this = "cold, take it easy"; at/above = ready
#define OIL_WARN_C         120    // audible over-temp guard trips here
#define COOLANT_WARN_C     110    // optional coolant guard (Mode01 0x05)
// Hysteresis on every temperature state change. Oil temp dithers by a degree
// around any threshold, so a bare comparison makes the label and arc colour
// flip on alternate updates - observed on the car as the screen "flashing" at
// 89-90 °C. A state, once entered, needs this much movement to leave again.
// It also stops the over-temp beep chattering at the alarm point, which the
// alarm code always claimed the caller handled but nothing actually did.
#define STATE_HYST_C       3

// ---- Power management (Option B: OBD 12V + sleep-on-idle) ------------------
// The board wakes on a CAN-RX edge, so it sleeps until the car actually talks
// instead of polling. The timers below are fallbacks, not the primary path -
// see the reasoning at the top of power.cpp.
// The sleep policy, in full, is two rules:
//   bench (a USB host has been seen)  -> never sleep
//   car   (no CAN event for 5 minutes) -> sleep
// Both timers below are that same 5 minutes on purpose, so "the bus went
// quiet" and "the bus never spoke" are one rule rather than two numbers to
// reason about. Was 30 s, which meant coming back to the car after a short
// stop found the pod already asleep. Five minutes at ~55 mA costs ~4.6 mAh -
// nothing against a car battery - and keeps it awake and reachable.
// The DEFAULT for the phone setting "Sleep after (minutes)" (settings.
// sleepAfterMin, 1-60), which is what power.cpp actually uses. Motion wake
// (2026-10-08) makes a shorter value cheaper than it was: coming back to the
// car, opening the door wakes the pod within a second.
#define BUS_IDLE_SLEEP_MS  300000 // no CAN traffic this long -> sleep
#define WAKE_SAMPLE_MS     1500   // after wake, listen this long for live bus
// How long a COLD boot (power applied by a human, not a timer/edge wake) stays
// awake when no CAN frame ever arrives. Without this the screen goes dark 1.5 s
// after power-up whenever there is no live bus - on a bench supply that is
// indistinguishable from a broken display.
//
// 5 minutes. It costs nothing in the car: this window only applies when NO
// frame ever arrives, and after a drive the board sleeps BUS_IDLE_SLEEP_MS
// after the last frame instead. Pressing the button wakes it again anyway.
// Was 1 minute while measuring sleep current on the bench, which is the one
// task a long window gets in the way of - drop it back if that comes up again.
#define COLD_BOOT_AWAKE_MS 300000
// Safety net only: if the CAN-RX edge wake were misconfigured, this is the
// longest the board could stay asleep with the engine running. Long enough to
// be cheap (one boot an hour), short enough that the failure is survivable.
#define SLEEP_SAFETY_SEC   3600
// Used when edge wake is not usable: the line is stuck dominant, the CAN
// driver failed, or the bus keeps waking us for nothing.
#define SLEEP_BACKOFF_SEC  300
// Consecutive wakes that found no live bus before we stop trusting the edge
// and fall back to SLEEP_BACKOFF_SEC. Guards against a car whose modules
// chatter long after key-off - untested on this car, so it may need tuning.
#define CAN_WAKE_FRUITLESS_MAX 5

// Bench aid: 1 = ignore the USB-host sleep inhibit and use very short sleep
// timers, so the parked wake/sleep cycle can be watched on a desk and still
// recovers by itself. Leave at 0; it makes reflashing a race, because the
// serial port only exists during the brief awake window.
#define SLEEP_BENCH_TEST 0
#if SLEEP_BENCH_TEST
#undef SLEEP_SAFETY_SEC
#undef SLEEP_BACKOFF_SEC
#define SLEEP_SAFETY_SEC   10
#define SLEEP_BACKOFF_SEC  15
#endif

// 1 = measuring parked current on the bench. Shortens only the two windows
// that decide WHEN it sleeps, so a reading takes 20 s instead of 5 minutes,
// and lengthens the wake timers so it stays asleep while the meter settles.
//
// Deliberately NOT folded into SLEEP_BENCH_TEST: that one also makes
// usbAttached() return false, which would let the board sleep with the cable
// attached - and a sleeping board drops off USB, so reflashing becomes a race
// against a ~2 s window. Keeping the USB inhibit intact means the restore
// flash afterwards is ordinary. ⚠ Set back to 0 when finished.
#define SLEEP_MEASURE_MODE 0
#if SLEEP_MEASURE_MODE
#undef COLD_BOOT_AWAKE_MS
#undef BUS_IDLE_SLEEP_MS
#undef SLEEP_SAFETY_SEC
#undef SLEEP_BACKOFF_SEC
#define COLD_BOOT_AWAKE_MS 20000    // sleeps 20 s after power-up, no CAN bus
#define BUS_IDLE_SLEEP_MS  20000
#define SLEEP_SAFETY_SEC   1800     // stays asleep for the reading
#define SLEEP_BACKOFF_SEC  1800
#endif

// 1 = never deep-sleep, at all, ever. Mostly unnecessary now: power.cpp
// already inhibits sleep whenever a USB host is attached, so bench work keeps
// the screen on and the serial port alive without this. Set it only to study
// the no-bus path while powered from something other than a PC.
// ⚠ Must be 0 for the car, or the pod never sleeps on constant 12 V.
#define DEV_DISABLE_SLEEP  0

// Show the sleep decision on the panel before blanking, and hold it long
// enough to read. The decision already goes to serial and /sleeplog.txt, but
// on buck power there is no serial host and often no card - so a board going
// black had no readable explanation at all, which is how "black after half a
// second" survived three wrong theories.
//
// Costs SLEEP_SHOW_REASON_MS of awake current per sleep. Against 5-minute
// windows that is nothing; set it to 0 once the parked behaviour is trusted.
// Was 1 while the parked path was being proved out (2026-09-24). Off now: the
// behaviour is verified, and a 2.5 s hold on every sleep is awake current for
// nothing. Turn it back on if parked behaviour ever needs explaining again.
#define SLEEP_SHOW_REASON     0
#define SLEEP_SHOW_REASON_MS  2500

// Also keep the last few sleep decisions in NVS, and print them at boot.
//
// /sleeplog.txt is the real record, but it needs an SD card - and on the bench
// there is no card, so the only copy went to a serial port that deep sleep had
// already killed. The on-screen notice does not help either when the board
// wakes and sleeps faster than it can be read. NVS survives both, and printing
// the ring at boot means plugging in USB is enough to read the history.
//
// ⚠ DIAGNOSTIC, and it writes flash on every sleep. A 5-minute parked cycle is
// ~290 writes/day, which is fine for weeks but not for the life of the device.
// Turn it off once the parked behaviour is trusted.
// Off for the same reason, and because this one writes flash on every sleep.
// Kept behind the flag rather than deleted: in the car there is no serial
// either, and unlike /sleeplog.txt this does not need the SD card to be
// present and mounted - so it is the first thing to re-enable if the parked
// behaviour ever misbehaves where it cannot be watched.
#define SLEEP_LOG_NVS         0
#define SLEEP_LOG_NVS_N       8      // ring length; 12 bytes each

// Display refresh period. 15, not 16.7: LVGL does not catch up a late frame,
// and delay() works in whole 1 ms ticks, so frames arrive ~1-2 ms after they
// fall due. Measured on the g-meter under constant motion: 16 gave 56 fps,
// 15 gives 58.5-59.5. Set on the display's own refresh timer, not via
// LV_DEF_REFR_PERIOD, which would also double the rate of the touch
// controller's I2C reads for no gain.
// What a frame costs, measured (partial render, 40-line strip, QSPI):
//   ~0.4 ms fixed per dirty patch (render setup + one bus transaction)
//   ~1 ms for a 25 px dot, ~1.3 ms for a small label
//   ~54 ms just to push a full 466x466 frame - so a SCREEN SWITCH cannot be
//   60 fps on this bus whatever we do; moving needles and dots can.
#define UI_FRAME_MS 15
// loop() waits up to this long for a frame that is about to fall due, instead
// of going round again and drawing it one loop pass late.
#define LV_WAIT_MAX_MS 4

// 1 = print display timing to serial every 2 s: frames actually drawn, time
// spent rendering and pushing pixels over QSPI, pixels pushed, and loop pass
// time. For working out where the frame budget goes. Costs a few micros()
// calls per frame; 0 for the car.
// Serial commands while it is on: 'b' runs a render benchmark of every screen
// (ui_bench), 's' toggles synthetic g-meter and boost input, 'r' hides the
// notification rim as if nothing were faulted, 'c' pretends a CAN frame
// arrived, 't<epoch>' sets the clock, 'f' injects readings that then go
// stale, 'd' prints the tail of the drive log, 'v<kmh>' fakes the speed the
// AP-while-driving logic sees (v0 = no fix, v-1 = real GNSS again),
// 'k<64 hex>' / 'koff' sets the log encryption key.
#define FRAME_STATS 0

// ---- Drive logging (onboard microSD) ---------------------------------------
// Decoded values, plus the raw TPMS payload as hex (its byte layout is still
// unsolved). There is no longer a separate promiscuous frame-capture sketch -
// if full raw CAN capture is ever needed again, recover can_sniffer_sd from
// git history (last present in commit 54255f0).
// 2 Hz is plenty for temperatures and still catches boost/g transients.
// One line per deep sleep, so the parked wake/sleep behaviour can be read off
// the card after a drive. Capped because parked nights would otherwise grow it
// without end.
#define SLEEP_LOG_PATH        "/sleeplog.txt"
#define SLEEP_LOG_MAX_BYTES   262144

// How much goes on the card (LogLevel in logger.h; a phone setting): 0 off,
// 1 trips (GPX), 2 normal (+ drive CSV), 3 full (+ the raw diagnostic hex
// columns). The track .vbo follows track mode from 1 up. Full by default
// while values are still being verified against the car.
#define LOG_LEVEL_DEFAULT     3
#define LOG_INTERVAL_MS       500
// The logger task's pace on core 0 - about what loop() used to give it, and
// well under the 40 ms between GNSS fixes the GPX and .vbo consume.
#define LOGGER_PASS_MS        5
#define LOG_FLUSH_INTERVAL_MS 2000   // data-loss window if power cuts

// Idle throttle. Sitting parked with the engine running writes thousands of
// identical rows and, at 25 Hz, a pile of GPX points all on the same spot.
// After this long stationary, both logs drop to a slow trickle - enough to
// show you were there, without the bulk or the SD wear.
#define LOG_IDLE_AFTER_MS     180000  // 3 min stationary -> throttle
#define LOG_IDLE_INTERVAL_MS  5000    // CSV row rate while idle
#define GPX_IDLE_POINT_MS     5000    // trackpoint rate while idle
#define LOG_IDLE_SPEED_KMH    3.0f    // below this counts as stationary

// Lose the fix for longer than this and the GPX track starts a new <trkseg>.
// Segments are how GPX says "these points are not continuous" - without the
// break, a tool joins the last point before a tunnel to the first one after
// it with a straight line, drawing a path that was never driven.
// ⚠ MUST stay comfortably above GPX_IDLE_POINT_MS: while parked, points are
// deliberately 5 s apart, and a threshold below that would split the track on
// every single idle point.
// GPX point validation. Measured against trip_0004, which sprayed spikes
// across the map coming out of a tunnel:
//   - no filter:                 86 jumps implying >250 km/h, worst 12920
//   - sats/HDOP filter only:     35 jumps, worst still 12920 (the bad points
//                                had 3-4 sats but HDOP of just 1.5-2.2)
//   - plus the jump limit:        2 jumps, worst 534, and 99.2 % of points kept
// So the quality flags are worth having but the distance check is what works.
#define GPS_MIN_SATS_LOG   5      // below this the fix is 2D guesswork
#define GPS_MAX_HDOP_LOG   5.0f   // 99 %+ of real points are under 1.0
#define GPX_MAX_JUMP_KMH   250.0f // implausible for this car; 200 dropped
                                  // 779 legitimate points, 250 drops 69
#define GPX_MAX_REJECT_RUN 20     // then accept anyway, in case the reference
                                  // point is the bad one

#define GPX_SEGMENT_GAP_MS    10000

// GPX trackpoint spacing. Matched to the nav rate so the smoothness we asked
// the module for actually reaches the file. At 10 Hz a point is ~120 bytes,
// so a track costs roughly 4 MB/hour - fine on a multi-GB card. Raise this if
// you would rather have smaller files for daily driving.
#define GPX_POINT_MS          GPS_NAV_RATE_MS
// Flushing on every point would mean 10 SD flushes a second, which stalls the
// render loop badly. Flush on this cadence instead: between flushes the copy
// on the card can be behind, but each flush leaves it valid and complete.
#define GPX_FLUSH_MS          2000

// ---- Feature toggles -------------------------------------------------------
#define FEATURE_GMETER     1      // QMI8658 IMU g-force screen

// ---- G-meter -----------------------------------------------------------
// The trail is the point: a g-g trace shows how smooth the inputs were, which
// is what motorsport uses it for. The sphere around it is decoration.
#define GMETER_FS_G        1.5f   // full-scale radius, g
#define GMETER_TRAIL_N     14     // history dots
#define GMETER_TRAIL_MS    90     // sample spacing - N*this = history length
#define GMETER_LABEL_MS    100    // the "0.83 g" figure: 10 Hz is as fast as it can be read
// Boot splash duration. Non-blocking - the loop keeps running through it, so
// this only decides when the gauges take over, not how long the board is busy.
// Set to 0 to skip it.
#define BOOT_ANIM_MS       2500

// G-meter zero capture. The pod is mounted tilted, so "level on the bench" is
// not level in the car and the dot sits off-centre at rest. Captured once,
// automatically, then persisted - the mounting angle does not change. Re-zero
// from the phone settings page after moving the pod.
#define GM_ZERO_SETTLE_MS  8000   // wait this long after boot before capturing
#define GM_ZERO_MAX_KMH    2.0f   // only capture when effectively stationary
#define GM_ZERO_TOL_G      0.08f  // and when |accel| is within this of 1 g

// How the IMU sits relative to the panel. GM_SCREEN_X_AXIS is the QMI8658 axis
// (0 = X, 1 = Y, 2 = Z) that runs left-right across the screen; the flips
// correct each direction's sign. Bench check, pod held as mounted: tilt the
// right edge down -> dot goes right; tilt the top edge away -> dot goes up
// (the dot shows the force felt, like a ball rolling to the low side, so this
// is also "turn left -> dot right" and "brake -> dot up" in the car).
// Brake lights on the tyre screen's car, from the IMU (no CAN brake signal:
// the OBD port sits behind a gateway). On at BRAKE_G_ON of deceleration, off
// below BRAKE_G_OFF, held at least BRAKE_MIN_ON_MS. 0.18 g is a
// firm-but-normal brake; engine braking alone is ~0.05-0.1 g. BRAKE_TAU_MS
// smooths road vibration without making the light noticeably late.
// Shown only at BRAKE_MIN_KMH or more (OBD speed, fresh): standing on a ~10%
// slope reads like a hard stop, and a brake light while stopped says nothing.
#define BRAKE_G_ON         0.18f
#define BRAKE_G_OFF        0.10f
#define BRAKE_TAU_MS       60
#define BRAKE_MIN_ON_MS    300
#define BRAKE_MIN_KMH      5

#define GM_SCREEN_X_AXIS   1
#define GM_FLIP_X          0
#define GM_FLIP_Y          1      // bench-checked 2026-09-26: tilt back went down

// ---- Phone settings panel (on-demand WiFi AP) ------------------------------
// Long-press the button to bring the AP up, then browse to http://192.168.4.1/
// Off by default: it draws current, and an always-on AP in a parked car is
// both a battery and a privacy nuisance.
#define WIFI_AP_SSID       "n-gauge"
// There is deliberately NO default password any more. The AP is up the whole
// time the board is awake and serves the complete GPS history of every drive,
// and a passphrase committed to a public repository protects none of it - the
// point of WPA2 is lost the moment the key is published. On first boot a
// random one is generated into NVS instead (settings.cpp), shown on the WiFi
// screen so it can be read in the car, and changeable from the phone.
#define WIFI_AP_PASS_LEN   10     // 10 chars from a 32-char alphabet = ~50 bits
// Deliberately excludes 0/O/1/l/I and similar - this gets typed on a phone
// from a screen, in a car, possibly in the dark.
#define WIFI_AP_PASS_ALPHABET "abcdefghjkmnpqrstuvwxyz23456789"
#define BTN_LONG_PRESS_MS  1200          // hold this long to toggle the AP
// 1 = bring the AP up at boot and keep it up while awake. Needed because the
// pod's buttons cannot be reached once it is mounted, so the long-press toggle
// is no longer a usable way in. This does NOT stop the board sleeping - the AP
// is shut down as part of goToSleep(), so parked it goes away with everything
// else. It does cost current while awake, which the engine is paying for.
#define WIFI_AP_ALWAYS     1

// 1 = take the AP down while the car is being driven, and bring it back once
// stopped. A 2.4 GHz transmitter a few centimetres from the GNSS antenna is
// competing with a -130 dBm satellite signal; stationary is also exactly when
// the settings page and log downloads are wanted. Never while a phone is
// connected, and a long press while driving keeps it up until the next stop.
// Keyed on GNSS speed, NOT on CAN: the bus is alive whenever the engine runs,
// including parked at the kerb. The premise (does the AP actually hurt the
// fix?) is still unmeasured - the drive CSV's apUp column plus the GPX
// sat/hdop per point answer it; set this to 0 for a comparison drive.
#define AP_DRIVE_OFF       1
// "Driving" = fix and >= ON_KMH held for ON_MS. "Stopped" = < OFF_KMH or no
// fix, held for OFF_MS. OFF_MS is long so a red light does not restart the AP:
// each start costs ~46 KB of internal RAM (AP_MIN_INTERNAL_HEAP) and esp_wifi
// panics rather than failing cleanly if it is not there, so it must not flap.
#define AP_DRIVE_ON_KMH    20.0f
#define AP_DRIVE_ON_MS     5000
#define AP_DRIVE_OFF_KMH   5.0f
#define AP_DRIVE_OFF_MS    60000

// ⚠ The AP address is deliberately NOT in 192.168.x.x, and that is the whole
// reason the captive portal works.
//
// Android's NetworkMonitor resolves the probe host itself and then, before
// sending any HTTP request, checks the answer:
//   if (mPrivateIpNoInternetEnabled && probeType == PROBE_HTTP
//       && hasPrivateIpAddress(resolvedAddr)) return PRIVATE_IP;
// hasPrivateIpAddress() is true for link-local, IPv6 ULA and site-local - and
// Java's Inet4Address.isSiteLocalAddress() covers 10/8, 172.16/12 and
// 192.168/16. So a hijacked DNS answer of 192.168.4.1 made Android abandon the
// probe client-side: it never reached us at all, which is exactly what the
// logs showed (Firefox's probe arrived, Android's never did - Firefox has no
// such rule).
//
// 4.3.2.1 is outside those ranges, so the answer looks public and the probe is
// actually sent. Every working ESP32 captive-portal project uses a trick like
// this; arduino-esp32's own example used 8.8.4.4 in 2.0.17 and dropped it in
// 3.x, which is why following the current example silently breaks Android.
// Gateway must equal the AP address, and the netmask must be /24-/28.
#define WIFI_AP_IP         4,3,2,1
#define WIFI_AP_NETMASK    255,255,255,0

// A clock reading later than this is real, not an unset 1970 one. Used to
// decide whether the time retained through deep sleep can be trusted.
// 2025-01-01T00:00:00Z - comfortably before this firmware existed.
#define CLOCK_SANE_EPOCH 1735689600L

// ---- Onboard RTC (PCF85063 at I2C 0x51) ------------------------------------
// Keeps the clock right across deep sleep, reboots and reflashes, so logs are
// correctly dated without waiting for a GNSS fix - which indoors never comes.
// No backup cell is fitted, so a full 12 V removal still loses it; see rtc.h.
#define RTC_ENABLE          1
#define RTC_I2C_ADDR        0x51
// How often to compare the chip against the system clock. Only a read, and
// only while the GNSS has a fix, so this is close to free.
#define RTC_SYNC_CHECK_MS   60000
// Rewrite the chip once it is off by this much. 2 s is well inside what any
// log timestamp cares about, and large enough that a normal crystal is not
// rewritten every single check.
#define RTC_DRIFT_TOL_SEC   2
// Upper sanity bound for an externally supplied clock (the settings page).
// 2100-01-01. Not decoration: the PCF85063 holds the year as two BCD digits,
// so anything past 2099 cannot be stored and would come back as a wrong year.
#define CLOCK_MAX_EPOCH     4102444800LL
// ⚠ DESTRUCTIVE bring-up diagnostic, and it must stay 0. When the chip reports
// a lost oscillator it overwrites the registers with 2000-01-01 to prove the
// write path works. That write throws away the retained last-known time, which
// is the one useful thing a powerless RTC still holds. It has already served
// its purpose: on 2026-09-24 a boot read back 2000-01-01T00:02:12Z - the
// sentinel plus elapsed time - proving both the write path and that the
// oscillator runs. Turn it on only to re-verify a new board.
#define RTC_SELFTEST        0

// The screen you settle on is remembered and restored at the next boot. Saved
// only after it has been unchanged this long, so cycling past screens to reach
// the one you want costs one NVS write rather than one per screen.
#define VIEW_SAVE_AFTER_MS 8000

// How long VIEW_WIFI may stay on screen before it drops back to the remembered
// gauge view.
//
// That screen shows the AP passphrase in 28 px text and a QR code that joins
// the network. On a dash-top pod both are readable through the windscreen of a
// parked car, and the QR does not even need reading - a photograph is enough.
// So it is a screen to visit, not one to sit on.
//
// 60 s is sized for the slower of the two jobs: scanning the QR takes a couple
// of seconds, typing a random 10-character passphrase by hand takes longer.
// Tap back to it if you need another minute.
#define WIFI_VIEW_MAX_MS   60000
// The arc counting that down in this many steps (10 ms each at 60 s): finer
// than a frame, so it sweeps smoothly. LV_USE_FLOAT lets the arc draw
// fractions of a degree.
#define WIFI_ARC_STEPS     6000

// ---- Trip distance ---------------------------------------------------------
// DEFAULT for settings.tripGapMin: a gap this long (minutes) between accepted
// GPS points starts a new trip. Editable from the settings page, because where
// the line falls between "a stop" and "a new journey" is a preference, not a
// fact - someone who commutes past a long queue wants a different number from
// someone doing track days.
//
// Needed at all because the pod is permanently powered and sleeps on bus
// silence, so "since power-on" is not a journey - it would accumulate across
// days. A fuel stop is a few minutes and must NOT reset; parking overnight
// must. 30 minutes separates those comfortably.
//
// Measured on the system clock, so it only applies once the clock is set
// (GNSS, the RTC, or the settings page). With no clock the distance simply
// keeps accumulating, which is the safe direction: too much trip is obvious,
// a silent reset mid-journey is not.
#define TRIP_GAP_MIN       30

// ---- Lap timing ------------------------------------------------------------
// The start/finish gate is set by stopping ON the line and tapping SET. Its
// direction is unknown at that moment - GNSS course is meaningless at a
// standstill - so it is taken from where the car goes next: once it is
// TRACK_ARM_DIST_M from the set point and doing at least TRACK_ARM_KMH, the
// bearing from the point to the car IS the direction of travel.
// Lap 1 is timed from the moment the car pulls away - the last fix slower
// than TRACK_DEPART_KMH after SET - so the first loop counts (2026-10-09);
// there is no out-lap. Only after a wake mid-session does the timer wait
// for the next crossing, the path since the last fix being unknown.
#define TRACK_ARM_DIST_M       20.0f
#define TRACK_ARM_KMH          15.0f
#define TRACK_DEPART_KMH       5.0f
// How far either side of the set point a crossing still counts. Wide enough
// for any line across a racetrack's start/finish straight, narrow enough that
// another part of the circuit crossing the line's extension does not.
#define TRACK_GATE_HALF_M      25.0f
// A "lap" shorter than this is a double trigger at the line, not a lap. GPS
// jitter re-crosses within a second or two; 20 s also swallowed a real 17.7 s
// test loop (2026-10-10), merging two loops into one 38.5 s "lap".
#define TRACK_MIN_LAP_MS       10000UL
// Two fixes further apart than this are not interpolated between: across a
// signal gap the car's path is unknown, and a straight line through it could
// "cross" the gate somewhere the car never went.
#define TRACK_MAX_FIX_GAP_MS   1000UL
// STOP needs a second tap within this long, or a brushed screen mid-session
// would wipe every lap. (SET is only shown with no session running, so it has
// nothing to lose and acts at once.) Also how long a status message stays up.
#define TRACK_CONFIRM_MS       3000UL

// Refuse to start the AP below this much free INTERNAL (non-PSRAM) heap.
// Measured: WiFi.mode(WIFI_AP) costs ~46 KB and softAP a little more. Running
// out is not a graceful failure - esp_wifi panics on the null allocation
// instead of returning an error, which rebooted the board on every long press
// until LVGL's pool was moved to PSRAM (see lv_conf.h).
#define AP_MIN_INTERNAL_HEAP (80 * 1024)

// Log downloads. The default 5 s socket timeout is more than half the 8 s
// watchdog budget, so a stalled client alone can reboot the board; and a whole
// transfer needs a ceiling so one request cannot occupy the loop forever.
#define DL_CLIENT_TIMEOUT_MS 1500
#define DL_MAX_MS            120000

// NOTE: the values below are DEFAULTS ONLY. Anything editable from the phone
// lives in settings.h/NVS at runtime - change it there, not here, unless you
// want to move the factory default. A wiped NVS falls back to these.
#define LCD_BRIGHTNESS_DEFAULT 200   // 0-255

// Night dimming (phone settings: a checkbox and the night value). Night is
// when the sun is NIGHT_SUN_ELEV_DEG or more below the horizon - computed
// from the clock and position (calc_sun_elevation_deg), so it follows sunset
// through the year instead of a fixed hour. -3 is just past sunset, as it
// gets properly dark; civil twilight ends at -6. Position is the last GPS
// fix, or NIGHT_LAT/LON (central Czechia - a few minutes of sunset error
// anywhere in the country) before the first one. No clock, no dimming.
#define NIGHT_ON_DEFAULT         1
#define NIGHT_BRIGHTNESS_DEFAULT 60
#define NIGHT_SUN_ELEV_DEG       (-3.0)
#define NIGHT_HYST_DEG           1.0    // back to day only above -3 + 1 = -2
#define NIGHT_CHECK_MS           30000
#define NIGHT_LAT_DEFAULT        49.8
#define NIGHT_LON_DEFAULT        15.5

// POSIX TZ rule, editable from the phone. GPS gives UTC; this converts it to
// local time INCLUDING daylight saving, so it needs changing twice a year
// only if you use a bare offset instead of a rule. Default = Czechia.
//   Czechia/CET:  CET-1CEST,M3.5.0,M10.5.0/3
//   UK:           GMT0BST,M3.5.0/1,M10.5.0
//   fixed +2, no DST: <+02>-2
// Local time = UTC + this many whole hours, optionally with the EU summer-time
// rule on top. Both are editable from the phone; the POSIX TZ string libc
// actually wants is built from them in settings.cpp. Czechia is +1 with DST.
#define TZ_OFFSET_DEFAULT  1
#define TZ_DST_DEFAULT     1

