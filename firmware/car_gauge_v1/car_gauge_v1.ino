// ============================================================================
// car_gauge_v1.ino  —  i30 N OBD gauge, v1
//   Oil + coolant temp, cold-engine warning, boost, tyres, GPS, G-meter,
//   button screen-switching and sleep-on-bus-idle. Over-temp warning is
//   visual only - there is no speaker fitted.
//
//   Portable logic lives in obd/ui/power. Board-specific glue is in bsp.cpp
//   (fill from the Waveshare example). Verified DIDs/formulas go in config.h.
// ============================================================================
#include <Arduino.h>
#include "esp_task_wdt.h"
#include "lvgl.h"
#include "config.h"
#include "bsp.h"
#include "obd.h"
#include "gps.h"
#include "settings.h"
#include "webapi.h"
#include "logger.h"
#include "brightness.h"
#include "ota.h"
#include "usbdisk.h"
#include "ui.h"
#include "power.h"
#include "rtc.h"
#include "laps.h"
#include "calc.h"         // calc_drive_update: AP off while driving


#if FEATURE_GMETER
// Raw accelerometer reading -> horizontal g on the screen's axes plus a
// vertical component. The maths is calc_gmeter_basis()/calc_gmeter_project()
// in calc.h, host-tested; this only rebuilds the basis when the calibration
// changes (it is re-zeroed from the phone) rather than on every sample.
static void gmeterProject(float gx, float gy, float gz,
                          float *outX, float *outY, float *outVert) {
  static CalcGBasis basis;
  static bool  haveBasis = false;
  static float basisFrom[3] = { 0, 0, 0 };

  const float off[3] = { settings.gmOffX, settings.gmOffY, settings.gmOffZ };
  if (!haveBasis || off[0] != basisFrom[0] || off[1] != basisFrom[1] ||
      off[2] != basisFrom[2]) {
    basis = calc_gmeter_basis(off, GM_SCREEN_X_AXIS, GM_FLIP_X, GM_FLIP_Y);
    for (int i = 0; i < 3; i++) basisFrom[i] = off[i];
    haveBasis = true;
    Serial.printf("[..] g-meter basis: up=(%.2f,%.2f,%.2f)%s\n",
                  basis.u[0], basis.u[1], basis.u[2],
                  basis.valid ? "" : " - no calibration, assuming Z-up");
  }
  const float a[3] = { gx, gy, gz };
  calc_gmeter_project(&basis, a, outX, outY, outVert);
}

// Applies the calibration, then hands the UI true horizontal g.
static void ui_update_gmeter_raw(float gx, float gy, float gz) {
  float hx, hy, vert;
  gmeterProject(gx, gy, gz, &hx, &hy, &vert);
  ui_update_gmeter(hx, hy, vert);
  logger_note_g(hx, hy);                 // the track log's IMU channels

  // Brake lights on the tyre screen's car, from the same projected value: hy
  // is the force felt forwards, i.e. deceleration. Only once the g-meter is
  // calibrated - uncalibrated, gravity leaks into hy on the tilted pod and a
  // standstill would read as a permanent hard stop.
  // And only while moving: on a slope gravity reads exactly like braking.
  // The filter runs regardless, so pulling away mid-stop is not a step input.
  static CalcBrake brake = {};
  const bool braking = calc_brake_update(&brake, hy, millis(), BRAKE_TAU_MS,
                                         BRAKE_G_ON, BRAKE_G_OFF, BRAKE_MIN_ON_MS);
  const bool moving = obd_speed_fresh() && obd.speedKmh >= BRAKE_MIN_KMH;
  ui_set_brake_lights(settings.gmCal && moving && braking);
  logger_note_brake(settings.gmCal && moving && braking);
}
#endif

#if AP_DRIVE_OFF
// WiFi AP down while driving, back up once stopped - see AP_DRIVE_OFF.
static CalcDrive apDrive = {};
static bool apDroppedForDrive = false;   // we took it down, so we restore it
static bool apManualHold      = false;   // long press while driving: hands off
#if FRAME_STATS
static int  apBenchKmh        = -1;      // 'v<kmh>' override; -1 = real GNSS
#endif

static void apDriveTask(uint32_t now) {
  bool  fix = gps.haveFix;
  float kmh = gps.speedKmh;
#if FRAME_STATS
  // Overrides what THIS logic sees, not the gps fields themselves: the GNSS
  // task rewrites gps.haveFix on every poll, module or not, so a value poked
  // into it lasts about 5 ms.
  if (apBenchKmh >= 0) { fix = apBenchKmh > 0; kmh = (float)apBenchKmh; }
#endif
  const bool wasDriving = apDrive.driving;
  const bool driving = calc_drive_update(&apDrive, fix, kmh, now,
                                         AP_DRIVE_ON_KMH, AP_DRIVE_ON_MS,
                                         AP_DRIVE_OFF_KMH, AP_DRIVE_OFF_MS);
  if (!driving) {
    apManualHold = false;
    if (apDroppedForDrive) {
      apDroppedForDrive = false;
      Serial.println(F("[..] stopped - WiFi AP back up"));
      web_start();
    }
    return;
  }
  if (!wasDriving) Serial.println(F("[..] driving"));
  // Checked every pass, not only on the transition: if a phone was connected
  // when the drive started, the AP goes down once it disconnects.
  if (!apManualHold && !apDroppedForDrive && web_is_up() && web_clients() == 0) {
    apDroppedForDrive = true;
    Serial.println(F("[..] driving - WiFi AP down until stopped"));
    web_stop();
  }
}
#endif

// Boot step timings, printed from loop() a few seconds in - early boot output
// is lost while USB CDC re-enumerates. Everything after ui_boot_anim_start()
// is time the splash stands frozen on its first frame.
static const char *bootStep[12];
static uint32_t    bootAt[12];
static int         nBootSteps = 0;
static void bootMark(const char *step) {
  if (nBootSteps < 12) { bootStep[nBootSteps] = step; bootAt[nBootSteps++] = millis(); }
}
static void bootTimingsPrint() {
  // The build stamp too: this line repeats for a minute, so it survives the
  // USB re-enumeration that eats the first lines after a reset.
  Serial.printf("[BOOT] firmware %s (%s) |", ota_build(), ota_hash());
  Serial.print(F(" ms:"));
  for (int i = 0; i < nBootSteps; i++)
    Serial.printf(" %s %lu", bootStep[i],
                  (unsigned long)(bootAt[i] - (i ? bootAt[i - 1] : 0)));
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  // Why we rebooted. Distinguishes our own faults (PANIC / TASK_WDT / BROWNOUT)
  // from an external power event (POWERON), which is what a PMU power-key long
  // press or a supply dropout looks like from in here.
  Serial.printf("[BOOT] reset reason %d\n", (int)esp_reset_reason());
  Serial.printf("[BOOT] firmware %s (%s)\n", ota_build(), ota_hash());   // which build is this?
  settings_load();     // before bsp_init: brightness comes from here
  // What happened during the sleeps we could not watch. Printed early so it is
  // the first thing in the log after plugging USB in, which is the only way to
  // read it: the board does this on buck power, with no card and no host.
  settings_dump_sleep_log();
  bsp_init();          // PMU, display panel, touch, backlight
  // After bsp_init (it brings up I2C) and well before logger_init, because log
  // filenames and the GPX header are stamped at creation - a clock seeded even
  // a second too late leaves the file called 1980 regardless.
  rtc_init();          // seed the clock from the PCF85063 if it knows the time
  // Night or day, decided now that the clock is seeded and before the splash
  // paints - at night it must not flash the day brightness first.
  brightness_update(true);
  bsp_lvgl_init();     // LVGL display + input registration
  ui_init();           // build gauge screens
  // Come back up on whatever screen was last in use. ui_set_view validates the
  // value, so a stored view that no longer exists (g-meter compiled out, say)
  // is ignored and we stay on the default.
  //
  // Reject a stored VIEW_WIFI as well as refusing to store one. Both halves are
  // needed: a device that already has it in NVS from before the save guard
  // existed would otherwise keep booting onto the AP passphrase forever, and
  // the code would look perfectly correct while doing it.
  ui_set_view((GaugeView)settings.lastView);
  if (ui_current_view() == VIEW_WIFI) ui_set_view(VIEW_OIL);
  // Then make lastView agree with what actually loaded. ui_set_view() silently
  // ignores a view it cannot show (out of range, or the g-meter compiled out),
  // so without this the screen falls back to the default while lastView keeps
  // the bad value - and the WiFi-screen timeout, which "returns" to lastView,
  // would then do nothing and leave the passphrase up indefinitely.
  settings.lastView = (uint8_t)ui_current_view();
#if BOOT_ANIM_MS > 0
  // Splash while the rest of setup() runs - and it really is on screen for
  // that, because ui_boot_anim_start() now paints it before returning rather
  // than leaving it for the first refresh in loop(). Torn down in loop() once
  // it has had its time; nothing blocks on it.
  ui_boot_anim_start();
#endif
  bootMark("toSplash");
  obd_init();          // start CAN @ 500k (runs on its own task, core 0)
  bootMark("obd");
  gps_init();          // read-only NMEA on GPS_RX_GPIO
  bootMark("gps");

  // The system clock survives deep sleep - the ESP32's own RTC keeps running -
  // but setup() re-runs on every wake, so gps.timeSet went back to false and a
  // perfectly good clock was treated as unset, blanking the timestamp column
  // until the GPS reacquired. Parked, that is every wake. Trust a retained
  // clock: only a genuine cold start can read earlier than CLOCK_SANE_EPOCH.
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP && time(nullptr) > CLOCK_SANE_EPOCH) {
    gps.timeSet = true;
    Serial.println(F("[OK] clock retained across deep sleep"));
  }

  logger_init();       // mount the card; the log file opens on the first row
  // /firmware.bin on the card is installed now (and the pod restarts into
  // it) - before the watchdog and before anything opens a log file.
  if (logger_card_ok()) ota_check_sd();
  // After the SD firmware check, which uses the card alone: from here the
  // logger's own task on core 0 does all the writing (see logger.h).
  logger_start();
  usbdisk_init();      // the card as a USB drive - empty until "Share" is tapped
  bootMark("sd");

  // Watchdog last, so a slow init can't trip it. The ESP32 core may already
  // have started the TWDT, in which case init returns INVALID_STATE and we
  // just reconfigure it instead.
  esp_task_wdt_config_t wdt = {
    .timeout_ms = WDT_TIMEOUT_MS,
    .idle_core_mask = 0,          // don't watch the idle tasks
    .trigger_panic = true,        // reboot rather than sit there frozen
  };
  // Reconfigure first, init only if that says it isn't running yet. The other
  // order also works, but logs a scary "TWDT already initialized" error at
  // boot, because the ESP32 core usually started it already.
  if (esp_task_wdt_reconfigure(&wdt) == ESP_ERR_INVALID_STATE) esp_task_wdt_init(&wdt);
  // The AP credentials, so they can be read on the bench without cycling to
  // the WiFi screen. Printing a passphrase is a deliberate trade: reaching
  // this output needs a USB cable physically plugged into the board, which is
  // strictly more access than joining the AP would give anyway.
  Serial.printf("[..] AP '%s' pass '%s'\n", settings.apSsid, settings.apPass);

  // Worth one line: a wrong g-meter zero is invisible on screen (the dot just
  // rests off-centre) and it persists in NVS, so it needs to be inspectable.
  Serial.printf("[..] g-meter zero: %s x=%.3f y=%.3f z=%.3f\n",
                settings.gmCal ? "calibrated" : "NOT set (will auto-zero)",
                settings.gmOffX, settings.gmOffY, settings.gmOffZ);

  // Report the result: a watchdog that silently failed to subscribe is worse
  // than none, because it looks like protection that isn't there.
  if (esp_task_wdt_add(NULL) == ESP_OK) {
    Serial.printf("[OK] watchdog armed (%d ms)\n", WDT_TIMEOUT_MS);
  } else {
    Serial.println(F("[ERR] watchdog NOT armed - a hung loop will not reboot"));
  }

  // If the car is off (bus silent), don't sit awake draining the battery.
  // On a deep-sleep wake this only starts the bus check; power_task() in
  // loop() finishes it, so the splash animates meanwhile.
  bootMark("wdt");
  power_boot_check();
  bootMark("power");
  // The WiFi AP starts from loop() once that check has passed - see there.
}

#if WOM_BENCH
// See WOM_BENCH in config.h. Polled, not an ISR: the point is to see every
// INT2 change with its timing and the sensor's own status alongside.
static void womBench(uint32_t now) {
  static bool    armed = false;
  static uint8_t thr = WOM_BENCH_MG;
  static int     lastLevel = -1;
  static uint32_t events = 0, armedAt = 0;
  if (!armed) {
    pinMode(21, INPUT);
    const bool ok = bsp_imu_wom_arm(thr);
    armed = true;
    armedAt = now;
    lastLevel = digitalRead(21);
    Serial.printf("[WOM] armed at %u mg: %s, INT2 idle level %d\n",
                  thr, ok ? "ok" : "FAILED", lastLevel);
  }
  const int level = digitalRead(21);
  if (level != lastLevel) {
    const uint8_t st = bsp_imu_status1();
    events++;
    Serial.printf("[WOM] #%lu INT2 %d->%d at %lu.%03lus (thr %u mg, STATUS1 0x%02X%s)\n",
                  (unsigned long)events, lastLevel, level,
                  (unsigned long)((now - armedAt) / 1000), (unsigned long)((now - armedAt) % 1000),
                  thr, st, (st & 0x04) ? " motion" : "");
    lastLevel = level;
  }
  // A number + Enter re-arms at that threshold.
  if (Serial.available()) {
    const long v = Serial.parseInt();
    while (Serial.available()) Serial.read();
    if (v >= 1 && v <= 255) { thr = (uint8_t)v; armed = false; events = 0; }
    else Serial.println(F("[WOM] threshold must be 1-255 mg"));
  }
}
#endif

void loop() {
  const uint32_t now = millis();
  {
    // Whole-pass time, start to start (render and idle delay included), for
    // the CSV's loopMaxMs - what the logger's own cost is measured against.
    static uint32_t passStartUs = 0;
    const uint32_t us = micros();
    if (passStartUs) logger_note_loop_us(us - passStartUs);
    passStartUs = us;
  }
#if WOM_BENCH
  womBench(now);
#endif
  {
    // Every 10 s for the first minute, so a serial monitor opened late still
    // catches it. The first loop() pass is when the splash starts moving.
    static uint32_t firstLoopMs = 0, lastPrintMs = 0;
    if (firstLoopMs == 0) { firstLoopMs = now ? now : 1; bootMark("toLoop"); }
    if (now < 60000 && now - lastPrintMs >= 10000) { lastPrintMs = now; bootTimingsPrint(); }
  }
#if FRAME_STATS
  const uint32_t loopT0 = micros();
  // One reader for the serial debug commands ('b', 's', 'r'), taken once per
  // pass. Each handler used to read for itself, and the 'b' check consumed
  // whatever byte was waiting - so an 's' landing mid-pass was silently eaten.
  const int dbgCmd = Serial.available() ? Serial.read() : -1;
  // 'c' pretends a CAN frame arrived (the drive log opens on the first one),
  // 't<epoch>' sets the clock like the phone page does - together they let
  // the logger's drive-start and rename paths be exercised on the desk.
  if (dbgCmd == 'c' && obd.lastFrameMs == 0) obd.lastFrameMs = now ? now : 1;
  // 'f' injects one set of readings stamped now, then nothing refreshes them -
  // a dropout on demand. OBD values go stale after OBD_STALE_MS; tyres are
  // stamped nearly TPMS_STALE_MS old so they follow a few seconds later.
  // 'd' prints the tail of the drive log to check what the CSV recorded.
  if (dbgCmd == 'f') {
    const uint32_t t = millis();
    obd.oilTempC = 95;  obd.oilRaw = 192; obd.oilMs = t;     obd.haveOil = true;
    obd.coolantC = 90;  obd.coolantMs = t;                   obd.haveCoolant = true;
    obd.boostBar = 1.23f; obd.boostMs = t;                   obd.haveBoost = true;
    obd.speedKmh = 50;  obd.speedMs = t;                     obd.haveSpeed = true;
    obd.absLoadPct = 200; obd.absLoadMs = t;                 obd.haveAbsLoad = true;
    obd.pedalRaw = 120; obd.pedalMs = t;                     obd.havePedal = true;
    // One tyre per case, so a single look shows them all: normal, normal,
    // low, high - the last two should read red.
    static const float bar[4]  = { 2.5f, 2.4f, 1.7f, 3.2f };
    static const int   temp[4] = { 31, 30, 28, 33 };
    for (int i = 0; i < 4; i++) {
      obd.tyreBar[i] = bar[i]; obd.tyreTempC[i] = temp[i];
      obd.tyreMs[i] = t - (TPMS_STALE_MS - 3000);            obd.haveTyre[i] = true;
    }
    Serial.println(F("[..] bench: fake readings injected, stale in ~3-5 s"));
  }
  if (dbgCmd == 'd') logger_dump_tail(6000);
  // 'v<kmh>' fakes the speed the AP-while-driving logic sees (v0 = no fix,
  // v-1 = back to the real GNSS). See apDriveTask for why it cannot simply
  // write gps.speedKmh.
#if AP_DRIVE_OFF
  if (dbgCmd == 'v') {
    apBenchKmh = (int)Serial.parseInt();
    if (apBenchKmh < 0) apBenchKmh = -1;
    Serial.printf("[..] bench: AP logic sees %s\n",
                  apBenchKmh < 0 ? "the real GNSS" :
                  apBenchKmh == 0 ? "no fix" : "a fix at the given speed");
  }
#endif
  if (dbgCmd == 't') {
    const long e = Serial.parseInt();
    Serial.printf("[..] bench: set clock to %ld -> %s\n", e,
                  rtc_set_from_epoch(e) ? "ok" : "rejected");
  }
  // 'k<64 hex>' / 'koff' sets the log encryption key as the phone app does,
  // under the same lock - for checking the encrypted logs on the desk.
  if (dbgCmd == 'k') {
    String hex = Serial.readStringUntil('\n');
    hex.trim();
    LoggerSdLock lock;
    Serial.printf("[..] bench: log key %s -> %s\n", hex.c_str(),
                  settings_set_log_key(hex.c_str()) ? "ok" : "rejected");
  }
#endif

  // No gps_task() here: the GNSS runs on its own task on core 0, spawned by
  // gps_init(), same as CAN.

  // Stale data must present as NO data. A number that has stopped changing
  // looks exactly like a steady reading, so without this a dead CAN link
  // would show a believable oil temp forever - and the over-temp alarm would
  // never fire because the value sits wherever it froze.
  const bool  haveOil   = obd_oil_fresh();
  const int   oilTempC  = obd.oilTempC;
  const bool  haveBoost = obd_boost_fresh();
  const float boostBar  = obd.boostBar;
  const bool  haveCool  = obd_coolant_fresh();

  // Real TPMS (0x7A0 / DID 0xC00B). Front/rear is confirmed; which side of an
  // axle each slot is has not been - see the note in config.h.
  bool  haveTyre[WHEEL_COUNT];
  float tyreBar[WHEEL_COUNT];
  int   tyreTempC[WHEEL_COUNT];
  for (int i = 0; i < WHEEL_COUNT; i++) {
    // Age out rather than trusting a single empty reply - see TPMS_STALE_MS.
    haveTyre[i]  = obd_tyre_fresh(i);
    tyreBar[i]   = obd.tyreBar[i];
    tyreTempC[i] = obd.tyreTempC[i];
  }

  // GPS is real even on the bench - it's our own UART, not the car's bus.
  const bool  gpsLinkUp   = gps.linkUp;
  const bool  gpsHaveFix  = gps.haveFix;
  const int   gpsSats     = gps.sats;
  const float gpsSpeedKmh = gps.speedKmh;

  // ---- derive states, with hysteresis ----
  // A bare comparison makes these flip on alternate updates when the
  // temperature sits on a threshold - seen on the car as the screen flashing
  // at 89-90 C. Each state now has to be left by STATE_HYST_C, not by a
  // fraction of a degree. Latched in statics, so they persist across loops.
  static bool cold = true, overOil = false, overCool = false;
  if (haveOil) {
    if (cold && oilTempC >= settings.oilReadyC)                   cold = false;
    else if (!cold && oilTempC < settings.oilReadyC - STATE_HYST_C) cold = true;

    if (!overOil && oilTempC >= settings.oilWarnC)                      overOil = true;
    else if (overOil && oilTempC < settings.oilWarnC - STATE_HYST_C)    overOil = false;
  } else {
    // A warning with no reading behind it is a frozen reading too. Left
    // latched, a stale overOil made the oil screen say "COOLANT TOO HOT"
    // (ui_update_oil's no-data branch reads `over` as the coolant warning).
    overOil = false;
  }
  if (haveCool) {
    if (!overCool && obd.coolantC >= settings.coolantWarnC)                   overCool = true;
    else if (overCool && obd.coolantC < settings.coolantWarnC - STATE_HYST_C) overCool = false;
  } else {
    overCool = false;
  }
  const bool over = overOil || overCool;

#if BOOT_ANIM_MS > 0
  // Retire the splash once it has run its course. The widget updates below are
  // harmless while it's up - they write to screens that aren't loaded. Idempotent.
  if (ui_boot_anim_done()) ui_boot_anim_end();
#endif

  // ---- push to UI ----
  {
    uint8_t faults = 0;
    // "No car yet" waits out CAN_MISSING_GRACE_MS, and a wake still sampling
    // the bus (power_wake_pending: up to 60 s after a motion wake); a CAN
    // driver that never started (!linkUp) shows at once.
    const bool canGrace = now < CAN_MISSING_GRACE_MS || power_wake_pending();
    if (!obd.linkUp || (obd.lastFrameMs == 0 && !canGrace)) faults |= FAULT_CAN;
    // A grace period, because at boot the GNSS task is still hunting for the
    // module's baud rate, and "not found yet" is not "missing". Checked on
    // gps.linkUp (NMEA decoding at all), not haveFix - see UiFault in ui.h.
    if (!gps.linkUp && now > GPS_MISSING_GRACE_MS) faults |= FAULT_GPS;
    // Lent to a PC as a USB drive is not "missing" - the WiFi screen says so.
    if (!logger_card_ok() && !logger_card_shared()) faults |= FAULT_SD;
#if FRAME_STATS
    // 'r' on the serial console hides the rim, to measure frames as they are
    // in the car, where nothing is normally faulted.
    static bool noRim = false;
    if (dbgCmd == 'r') noRim = !noRim;
    if (noRim) faults = 0;
#endif
    ui_set_faults(faults);
  }
  // The same "car on" test as obd.cpp's schedule: lastFrameMs counts every
  // frame except the TPMS module's, which answers with the car parked too.
  ui_set_car_talking(obd.lastFrameMs != 0 &&
                     (int32_t)(now - obd.lastFrameMs) < (int32_t)OBD_STALE_MS);
  // Oil first: it owns the state line that the coolant arc shares.
  ui_update_oil(haveOil, oilTempC, cold, over);
  ui_update_coolant(haveCool, obd.coolantC, overCool);
#if FRAME_STATS
  // 's' on the serial console toggles synthetic input - the g-meter circling
  // and boost sweeping 0-1.9 bar - so frame rates under constant change can
  // be measured with the pod untouched and no car.
  static bool synth = false;
  if (dbgCmd == 's') synth = !synth;
  // Boost samples at the real rate, not per pass.
  if (synth) ui_update_boost(true, 0.95f + 0.95f *
                             sinf((now / BOOST_SAMPLE_MS) * 0.3f));
  else
#endif
  ui_update_boost(haveBoost, boostBar);
  // Torque is an estimate from absolute load - see TORQUE_NM_FROM_LOAD.
  ui_update_torque(obd_absload_fresh(), TORQUE_NM_FROM_LOAD(obd.absLoadPct));
  // Pedal travel, foot off .. floored - see PEDAL_RAW_FULL (still assumed).
  ui_update_pedal(obd_pedal_fresh(),
                  ((int)obd.pedalRaw - PEDAL_RAW_IDLE) * 100 / (PEDAL_RAW_FULL - PEDAL_RAW_IDLE));
  ui_update_tpms(haveTyre, tyreBar, tyreTempC);
  ui_update_gps(gpsLinkUp, gpsHaveFix, gpsSats, gpsSpeedKmh, logger_trip_km());

  // Lap timer: runs every pass whether or not the track screen is showing, so
  // laps are still counted while another screen is up.
  laps_task();
  {
    LapInfo li;
    laps_info(&li);
    ui_update_track(&li);
  }
  ui_update_wifi(settings.apSsid, settings.apPass, web_is_up(), web_ip(),
                 web_clients());
#if FEATURE_GMETER
  float gx, gy, gz;
  // The CSV's raw IMU columns take this sample: the logger runs on core 0
  // and must not use the I2C bus itself.
  if (bsp_imu_read(&gx, &gy, &gz)) logger_note_raw_g(gx, gy, gz);
  // Auto-zero once, if it has never been calibrated: the pod sits tilted on
  // the dash, so at rest gravity leaks into the lateral/longitudinal axes and
  // the dot parks off-centre. Only capture when the total vector really is
  // ~1 g (i.e. we are not accelerating) and the car is not moving, and only
  // after a settle delay - a zero captured while rolling is worse than none.
  // The mounting angle is fixed, so once is enough; re-zero from the phone.
  if (!settings.gmCal && now > GM_ZERO_SETTLE_MS) {
    const float mag = sqrtf(gx * gx + gy * gy + gz * gz);
    const bool  still = !gps.haveFix || gps.speedKmh < GM_ZERO_MAX_KMH;
    if (still && fabsf(mag - 1.0f) < GM_ZERO_TOL_G) {
      settings_set_gmeter_zero(gx, gy, gz);
      Serial.printf("[OK] g-meter zeroed: x=%.3f y=%.3f z=%.3f\n", gx, gy, gz);
    }
  }
#if FRAME_STATS
  if (synth) {
    const float a = now * 0.004f;
    gx = 0.8f * cosf(a); gy = 0.8f * sinf(a); gz = 1.0f;
    ui_update_gmeter(gx, gy, gz);
  } else
#endif
  ui_update_gmeter_raw(gx, gy, gz);
#endif

  // ---- button: short press cycles screens, long press toggles the AP ----
  {
    static uint32_t downSince = 0;
    static bool     longFired = false;
    if (bsp_button_down()) {
      if (downSince == 0) downSince = now;
      if (!longFired && now - downSince >= BTN_LONG_PRESS_MS) {
        longFired = true;            // fire once per hold, not repeatedly
        // A long press does not go through ui_step_view(), so it needs its own
        // reset - otherwise toggling the AP and then waiting for a phone to
        // associate could run the idle window out before anyone connects.
        power_note_activity();
#if AP_DRIVE_OFF
        // A deliberate toggle while driving wins until the next stop, both
        // ways: on stays on, off is not undone when the car stops.
        if (apDrive.driving) { apManualHold = true; apDroppedForDrive = false; }
#endif
        web_toggle();
      }
    } else {
      // Act on RELEASE, not on press: the press edge happens long before we
      // know whether it will become a long press, so triggering there made a
      // long press cycle the view *and* toggle the AP.
      if (downSince != 0 && !longFired && now - downSince >= 40) ui_step_view(+1);
      downSince = 0;
      longFired = false;
    }
  }

  // ---- remember the screen across restarts ----
  // Deferred on purpose: cycling through six views to reach the one you want
  // would otherwise be six NVS writes. Only the screen you settle on is saved.
  {
    static GaugeView settled  = ui_current_view();
    static uint32_t  changedAt = 0;
    const GaugeView v = ui_current_view();
    if (v != settled) { settled = v; changedAt = now ? now : 1; }
    else if (changedAt && now - changedAt >= VIEW_SAVE_AFTER_MS) {
      changedAt = 0;
      // settings_save_view() refuses VIEW_WIFI itself, so lastView simply stays
      // on the previous gauge view when the WiFi screen is the one settled on.
      if (settings.lastView != (uint8_t)v) settings_save_view((uint8_t)v);
    }

    // ...and do not sit on it either. Not persisting it stops the pod BOOTING
    // into it; this stops it being LEFT on it, which is the same exposure with
    // a slower start. Returns to the remembered view, which cannot itself be
    // VIEW_WIFI thanks to the guard above.
    static uint32_t wifiShownAt = 0;
    // While the SD card is lent to a PC the screen stays, so its status
    // ("SD on PC - eject it there") does not vanish - and that only happens
    // with a PC on the cable, i.e. on the bench, not with a passer-by.
    const bool holdWifi = usbdisk_state() == USBDISK_SHARED;
    // A touch on the screen restarts the countdown too.
    const bool touched = ui_take_wifi_touch();
    if (v == VIEW_WIFI && (holdWifi || touched)) wifiShownAt = now ? now : 1;
    // The arc round the screen counts down to the switch back (-1 = hidden).
    ui_update_wifi_countdown(v != VIEW_WIFI || holdWifi || wifiShownAt == 0 ? -1.0f
        : 1.0f - (float)(now - wifiShownAt) / (float)WIFI_VIEW_MAX_MS);
    if (v == VIEW_WIFI) {
      if (wifiShownAt == 0) wifiShownAt = now ? now : 1;
      else if ((int32_t)(now - wifiShownAt) >= (int32_t)WIFI_VIEW_MAX_MS) {
        wifiShownAt = 0;
        ui_set_view((GaugeView)settings.lastView);
        // Belt and braces: if that did not take, force a known view rather
        // than leaving the passphrase on screen for another minute, forever.
        if (ui_current_view() == VIEW_WIFI) ui_set_view(VIEW_OIL);
      }
    } else {
      wifiShownAt = 0;
    }
  }

  // No logger_task() here any more: it runs on its own task on core 0.
  rtc_task();          // keep the PCF85063 in step with GNSS time
  brightness_update(false);   // day/night, every NIGHT_CHECK_MS
  ota_task();                 // confirms a freshly updated firmware once healthy
  // The SD card as a USB drive, on the bench: share on a tap, take it back
  // when the PC ejects it or the cable goes.
  if (ui_take_share_tap()) usbdisk_share();
  usbdisk_task();
  ui_update_usbdisk(usbdisk_host_connected(), usbdisk_state() == USBDISK_SHARED);
#if WIFI_AP_ALWAYS
  // Once, as soon as this boot is known to stay up: right away on a cold
  // boot, after the bus check on a deep-sleep wake - so a wake that goes
  // straight back to sleep never spends ~46 KB of internal RAM and a radio
  // start-up on an AP nobody sees. Was the end of setup(), behind a blocking
  // bus check that froze the boot splash.
  static bool apStarted = false;
  if (!apStarted && !power_wake_pending()) {
    apStarted = true;
    web_start();
    bootMark("ap");
  }
#endif
#if AP_DRIVE_OFF
  apDriveTask(now);
#endif
  web_task();

  // ---- power: sleep if the bus has gone quiet ----
  power_task();

  // ---- LVGL ----
#if FRAME_STATS
  // 'b' on the serial console runs the render benchmark.
  if (dbgCmd == 'b') ui_bench();
  // How often each OBD value actually arrives - the check for
  // request pacing on the car. Counts changes of the *Ms stamps.
  {
    static uint32_t since = 0, lastB = 0, lastO = 0, lastC = 0, nB = 0, nO = 0, nC = 0;
    if (obd.boostMs != lastB)   { lastB = obd.boostMs;   nB++; }
    if (obd.oilMs != lastO)     { lastO = obd.oilMs;     nO++; }
    if (obd.coolantMs != lastC) { lastC = obd.coolantMs; nC++; }
    if (now - since >= 2000) {
      if (obd.lastFrameMs)
        Serial.printf("[OBD] updates/s: boost %.1f, oil %.1f, coolant %.1f\n",
                      nB * 1000.0f / (now - since), nO * 1000.0f / (now - since),
                      nC * 1000.0f / (now - since));
      nB = nO = nC = 0; since = now;
    }
  }
#endif
  // If the next frame is due within a few ms, wait for it HERE, after the rest
  // of the pass, rather than going round again: the rest of a pass costs
  // ~2.5 ms, so a frame falling due during the sleep below was drawn that
  // late every time - 18.5 ms frames, ~52 fps. Bounded, so the IMU and the
  // button are never held off for longer than LV_WAIT_MAX_MS by this (CAN
  // and GNSS have their own tasks on core 0 and are not affected at all).
  static uint32_t lvDueAt = 0;
  {
    const int32_t early = (int32_t)(lvDueAt - millis());
    if (early > 0 && early <= LV_WAIT_MAX_MS) delay(early);
  }
#if FRAME_STATS
  const uint32_t lvT0 = micros();
  const uint32_t lvIdle = lv_timer_handler();
  const uint32_t lvT1 = micros();
  bsp_frame_stats_loop(lvT1 - loopT0, lvT1 - lvT0,    // excludes the delay below
                       (int)ui_current_view());
#else
  const uint32_t lvIdle = lv_timer_handler();
#endif
  lvDueAt = millis() + lvIdle;

  esp_task_wdt_reset();   // we got all the way round: still alive
  // Sleep until LVGL next has something due, capped at 5 ms so the IMU and
  // the button are still served ~100 times a second. Never 0: this also
  // yields core 1 to lower-priority tasks.
  delay(lvIdle < 1 ? 1 : lvIdle > 5 ? 5 : lvIdle);
}
