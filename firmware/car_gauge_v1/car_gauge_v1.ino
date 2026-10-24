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
#include "taskwdt.h"      // the background tasks are watched too
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
#include "podstate.h"     // the car, its motion and what follows
#include "rtc.h"
#include "laps.h"
#include "calc.h"
#include "bench.h"        // the desk tools; empty in the car's build
#include "bootlog.h"      // one line per boot on the card
#include "prof.h"         // FRAME_STATS timing; nothing in the car's build


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
}


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
  // Nor a screen hidden since it was stored (settings.views).
  ui_set_view((GaugeView)settings.lastView);
  if (ui_current_view() == VIEW_WIFI || !ui_view_shown(ui_current_view())) ui_set_view(ui_home_view());
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

  // No "clock retained" flag after a deep sleep. Anything that stamps a file
  // asks clock_valid(), which a retained clock passes. gps.timeSet means "the
  // GNSS set it THIS boot", and setting it here on every wake (as it once was)
  // held the GNSS resync off for GPS_TIME_RESYNC_MS and let rtc_task() write
  // the system clock - drifted on the RC oscillator through the sleep - into
  // the crystal RTC. rtc_init() now prefers the crystal on a wake instead.

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
  taskwdt_armed = true;   // the CAN, GNSS and logger tasks subscribe on their next pass

  // If the car is off (bus silent), don't sit awake draining the battery.
  // On a deep-sleep wake this only starts the bus check; power_task() in
  // loop() finishes it, so the splash animates meanwhile.
  bootMark("wdt");
  power_boot_check();
  bootMark("power");
  // The WiFi AP starts from loop() once that check has passed - see there.
}

// ============================================================================
// loop() and the steps it takes, in the order it takes them.
// ============================================================================

static void passTiming(uint32_t now) {
  {
    // Whole-pass time, start to start (render and idle delay included), for
    // the CSV's loopMaxMs - what the logger's own cost is measured against.
    static uint32_t passStartUs = 0;
    const uint32_t us = micros();
    if (passStartUs) logger_note_loop_us(us - passStartUs);
    passStartUs = us;
  }
  {
    // Every 10 s for the first minute, so a serial monitor opened late still
    // catches it. The first loop() pass is when the splash starts moving.
    static uint32_t firstLoopMs = 0, lastPrintMs = 0;
    if (firstLoopMs == 0) { firstLoopMs = now ? now : 1; bootMark("toLoop"); }
    if (now < 60000 && now - lastPrintMs >= 10000) { lastPrintMs = now; bootTimingsPrint(); }
  }
}

// ---- derive states, with hysteresis ----
// A bare comparison makes these flip on alternate updates when the
// temperature sits on a threshold - seen on the car as the screen flashing
// at 89-90 C. Each state now has to be left by STATE_HYST_C, not by a
// fraction of a degree. Latched here, so they persist across passes.
static bool tempCold = true, tempOverOil = false, tempOverCool = false;

static void updateTempStates() {
  // Stale data must present as NO data (obd_*_fresh). A number that has
  // stopped changing looks exactly like a steady reading, so without this a
  // dead CAN link would show a believable oil temp forever - and the
  // over-temp alarm would never fire because the value sits wherever it froze.
  if (obd_oil_fresh()) {
    const int oilTempC = obd.oilTempC;
    if (tempCold && oilTempC >= settings.oilReadyC)                   tempCold = false;
    else if (!tempCold && oilTempC < settings.oilReadyC - STATE_HYST_C) tempCold = true;

    if (!tempOverOil && oilTempC >= settings.oilWarnC)                     tempOverOil = true;
    else if (tempOverOil && oilTempC < settings.oilWarnC - STATE_HYST_C)   tempOverOil = false;
  } else {
    // A warning with no reading behind it is a frozen reading too: a stale
    // overOil would keep the oil screen and the rim saying TOO HOT.
    tempOverOil = false;
  }
  if (obd_coolant_fresh()) {
    if (!tempOverCool && obd.coolantC >= settings.coolantWarnC)                   tempOverCool = true;
    else if (tempOverCool && obd.coolantC < settings.coolantWarnC - STATE_HYST_C) tempOverCool = false;
  } else {
    tempOverCool = false;
  }
}

#if BOOT_ANIM_MS > 0
// The bench page's text: what is up, at a glance, without a serial monitor.
static void benchPageText(char *b, size_t n, uint32_t now) {
  uint32_t sdUsed, sdTotal;
  web_sd_space_kb(&sdUsed, &sdTotal);
  char sd[40], wifi[64], gnss[32];
  if (logger_card_shared())   snprintf(sd, sizeof(sd), "SD lent to the PC");
  else if (!logger_card_ok()) snprintf(sd, sizeof(sd), "No SD card");
  else snprintf(sd, sizeof(sd), "SD %.1f / %.1f GB used",
                sdUsed / 1048576.0f, sdTotal / 1048576.0f);
  if (web_is_up()) snprintf(wifi, sizeof(wifi), "WiFi: %s  %s  (%u on)",
                            settings.apSsid, web_ip(), web_clients());
  else             snprintf(wifi, sizeof(wifi), "WiFi off");
  if (!gps.linkUp)       snprintf(gnss, sizeof(gnss), "GPS: no module");
  else if (gps.haveFix)  snprintf(gnss, sizeof(gnss), "GPS: fix, %u sats", gps.sats);
  else                   snprintf(gnss, sizeof(gnss), "GPS: no fix, %u sats", gps.sats);
  // The separator is a bullet (U+2022): Montserrat 14 as built has no
  // middle dot, which drew as nothing.
  snprintf(b, n,
           "On the bench\n"
           "Firmware %s\n%s\n%s\n"
           "%s  \xE2\x80\xA2  CAN: %s\n"
           "RAM %u KB free  \xE2\x80\xA2  PSRAM %.1f MB\n"
           "Chip %.0f\xC2\xB0" "C  \xE2\x80\xA2  up %lu min  \xE2\x80\xA2  %s\n\n"
           "Touch for the gauges",
           ota_build(), wifi, sd, gnss,
           !obd.linkUp ? "driver down" : obd.lastFrameMs ? "talking" : "no car",
           (unsigned)(ESP.getFreeHeap() / 1024), ESP.getFreePsram() / 1048576.0f,
           temperatureRead(), (unsigned long)(now / 60000), web_reset_text());
}

// The splash, once swept, stays up until the car sends a reading: gauges
// before that all read "--" and the gauge looks broken. It goes when a
// reading is fresh, or when the "no car" fault would show anyway (the
// screens then say "--" for a reason), or at a touch. A wake that goes
// back to sleep never shows a gauge at all. The widget updates that follow
// are harmless meanwhile - they write to screens that are not loaded.
//
// On the bench (a PC on USB) it is a status page instead, with no timeout:
// the gauges mean nothing at the desk.
static void bootSplashTask(uint32_t now, bool canGrace) {
  // Gone already: nothing to hold - and no bench page text to build, which
  // this went on doing once a second, for a page no longer there.
  if (!ui_boot_splash_up() || !ui_boot_anim_done()) return;
  const bool bench   = usbdisk_host_connected();
  const bool reading = obd_oil_fresh() || obd_coolant_fresh() || obd_boost_fresh() ||
                       obd_rpm_fresh() || obd_speed_fresh();
  if (ui_boot_take_touch() || reading || (!bench && !(canGrace && obd.linkUp))) {
    ui_boot_anim_end();             // idempotent
  } else if (bench) {
    static uint32_t benchAt = 0;
    // The text once a second; the share button every pass, so it says
    // "Stop sharing" the moment the card is lent (ui_boot_show_bench
    // compares before it redraws anything).
    static char b[400] = "";
    if (benchAt == 0 || now - benchAt >= 1000) {
      benchAt = now ? now : 1;
      benchPageText(b, sizeof(b), now);
    }
    ui_boot_show_bench(b, usbdisk_state() == USBDISK_SHARED);
  }
}
#endif

// The notification rim: which faults are up.
// From the gauge's state (podstate.h), worked out from the same facts as
// before: over-temperature on every screen, not just the oil screen's state
// line (a track day is spent on the track screen - 2026-10-17: oil at 132 C,
// nothing said so there), with the latched states' hysteresis; no car (no
// frame since boot, after a grace; a driver that never started at once); no
// NMEA after its grace, while the GNSS task hunts the baud rate; no card -
// lent to a PC is not missing, the WiFi screen says so; a card failing.
static void faultsTask() {
  uint8_t faults = podstate().faults;
  if (bench_no_rim()) faults = 0;
  ui_set_faults(faults, obd.oilTempC, obd.coolantC);
}

// ---- push to UI ----
// Every value through obd_*_fresh: stale data presents as no data.
static void gaugesTask(uint32_t now) {
  // The car off or not, from the gauge's state: no frame for OBD_STALE_MS,
  // the TPMS module's aside (it answers with the car parked too).
  ui_set_car_talking(podstate().car != PS_CAR_OFF);
  // Oil first: it owns the state line that the coolant arc shares.
  ui_update_oil(obd_oil_fresh(), obd.oilTempC, tempCold, tempOverOil, tempOverCool);
  ui_update_coolant(obd_coolant_fresh(), obd.coolantC, tempOverCool);
  // The bench's synthetic boost samples at the real rate, not per pass.
  if (bench_synth()) ui_update_boost(true, 0.95f + 0.95f *
                                     sinf((now / BOOST_SAMPLE_MS) * 0.3f));
  else               ui_update_boost(obd_boost_fresh(), obd.boostBar);
  // Torque is an estimate from absolute load - see TORQUE_NM_FROM_LOAD.
  ui_update_torque(obd_absload_fresh(), TORQUE_NM_FROM_LOAD(obd.absLoadPct));
  // Pedal travel, foot off .. floored - see PEDAL_RAW_FULL (still assumed).
  ui_update_pedal(obd_pedal_fresh(),
                  ((int)obd.pedalRaw - PEDAL_RAW_IDLE) * 100 / (PEDAL_RAW_FULL - PEDAL_RAW_IDLE));

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
  ui_update_tpms(haveTyre, tyreBar, tyreTempC);
  // GPS is real even on the bench - it's our own UART, not the car's bus.
  ui_update_gps(gps.linkUp, gps.haveFix, gps.sats, gps.speedKmh, logger_trip_km());

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
}

static void gmeterTask(uint32_t now) {
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
  if (bench_synth()) {                   // the bench's: circling at 0.8 g
    const float a = now * 0.004f;
    ui_update_gmeter(0.8f * cosf(a), 0.8f * sinf(a), 1.0f);
    return;
  }
  ui_update_gmeter_raw(gx, gy, gz);
}

// ---- remember the screen across restarts ----
// Deferred on purpose: cycling through six views to reach the one you want
// would otherwise be six NVS writes. Only the screen you settle on is saved.
static void viewTask(uint32_t now) {
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
  // While the SD card is lent to a PC the screen stays, so its "Stop
  // sharing" button does not vanish - and that only happens
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
    else if (calc_elapsed(now, wifiShownAt, WIFI_VIEW_MAX_MS)) {
      wifiShownAt = 0;
      ui_set_view((GaugeView)settings.lastView);
      // Belt and braces: if that did not take, force a known view rather
      // than leaving the passphrase on screen for another minute, forever.
      if (ui_current_view() == VIEW_WIFI) ui_set_view(ui_home_view());
    }
  } else {
    wifiShownAt = 0;
  }
}

// The SD card as a USB drive, on the bench: a tap shares it, a tap while
// shared takes it back - as do the PC's eject and pulling the cable.
static void usbTask() {
  if (ui_take_share_tap()) {
    if (usbdisk_state() == USBDISK_SHARED) usbdisk_unshare();
    else                                   usbdisk_share();
  }
  usbdisk_task();
  ui_update_usbdisk(usbdisk_host_connected(), usbdisk_state() == USBDISK_SHARED);
}

// The WiFi AP: ONE rule for whether it should be up, and one path that gets
// it there.
//
// Wanted once this boot is known to stay up - right away on a cold boot,
// after the bus check on a deep-sleep wake, so a wake that goes straight back
// to sleep never spends ~46 KB of internal RAM and a radio start-up on an AP
// nobody sees - and not while driving (AP_DRIVE_OFF), unless a phone is on it.
//
// Wanted and down is retried every AP_RETRY_MS: web_start() declines when
// internal RAM is short (AP_MIN_INTERNAL_HEAP), and checks before it
// allocates. There used to be two paths, and each lost the AP its own way:
// after a drive the "we took it down" flag was cleared before trying, so one
// refusal kept the AP down until a reboot (2026-10-12..17, every drive); at
// boot the start was marked done before trying, with the same result
// (2026-10-19 review, A4).
static void apTask(uint32_t now) {
  (void)now;
#if WIFI_AP_ALWAYS
#if AP_DRIVE_OFF
  // The gauge's state (podstate.h): down while driving, by the car's own
  // speed - the GPS's only when the car is silent. By the GPS alone it
  // stayed up 2-5 minutes into a drive, until the first fix (2026-10-23).
  // A connected phone keeps it up; not while a wake is still checking.
  const bool want = podstate().wifiWanted;
#else
  const bool want = !power_wake_pending();
#endif
  static uint32_t lastTryMs = 0;           // 0 = not tried since it was last up
  if (want && !web_is_up() && (lastTryMs == 0 || now - lastTryMs >= AP_RETRY_MS)) {
    static bool firstTry = true;
    if (firstTry) { firstTry = false; bootMark("ap"); }
    lastTryMs = now ? now : 1;
    web_start();
    if (web_is_up()) {
      lastTryMs = 0;
      Serial.println(F("[..] WiFi AP up"));
    }
  } else if (!want && web_is_up()) {
    lastTryMs = 0;
    Serial.println(F("[..] driving - WiFi AP down until stopped"));
    web_stop();
  }
#endif
  web_task();
}

// ---- LVGL ----
// Renders what is due; returns how long until LVGL next has something due.
static uint32_t lvglTask() {
  // If the next frame is due within a few ms, wait for it HERE, after the rest
  // of the pass, rather than going round again: the rest of a pass costs
  // ~2.5 ms, so a frame falling due during the sleep after it was drawn that
  // late every time - 18.5 ms frames, ~52 fps. Bounded, so the IMU and the
  // button are never held off for longer than LV_WAIT_MAX_MS by this (CAN
  // and GNSS have their own tasks on core 0 and are not affected at all).
  static uint32_t lvDueAt = 0;
  {
    const int32_t early = (int32_t)(lvDueAt - millis());
    if (early > 0 && early <= LV_WAIT_MAX_MS) delay(early);
  }
  uint32_t lvIdle;
  {
    PROF_SCOPE(bspLvglStat);
    lvIdle = lv_timer_handler();
  }
  lvDueAt = millis() + lvIdle;
  return lvIdle;
}

// Sleep until LVGL next has something due, capped at 5 ms so the IMU and
// the button are still served ~100 times a second. Never 0: this also
// yields core 1 to lower-priority tasks.
static void idleTask(uint32_t lvIdle) {
  esp_task_wdt_reset();   // we got all the way round: still alive
  delay(lvIdle < 1 ? 1 : lvIdle > 5 ? 5 : lvIdle);
}

void loop() {
  uint32_t lvIdle;
  {
    PROF_SCOPE(bspLoopStat);  // the pass, the idle sleep excluded
    const uint32_t now = millis();
    passTiming(now);
    bootlog_task(now);
    bench_task(now);
    // No gps_task() and no logger_task() here: the GNSS and the logger run on
    // their own tasks on core 0, as CAN does.

    updateTempStates();
    // The car, its motion and what follows, once for the pass (podstate.h).
    podstate_update(now, tempOverOil, tempOverCool);
#if BOOT_ANIM_MS > 0
    // "No car yet" waits out CAN_MISSING_GRACE_MS, and a wake still sampling
    // the bus (power_wake_pending: up to 60 s after a motion wake); a CAN
    // driver that never started (!linkUp) shows at once.
    bootSplashTask(now, now < CAN_MISSING_GRACE_MS || power_wake_pending());
#endif
    faultsTask();
    gaugesTask(now);
    gmeterTask(now);
    // No button handling: screens change by swiping, and the WiFi AP follows
    // the car (up when awake, down while driving). The button only wakes the
    // gauge from deep sleep (power.cpp), like the door opening does.
    viewTask(now);

    rtc_task();                 // keep the PCF85063 in step with GNSS time
    brightness_update(false);   // day/night, every NIGHT_CHECK_MS
    ota_task();                 // confirms a freshly updated firmware once healthy
    usbTask();
    apTask(now);
    power_task();               // sleep if the bus has gone quiet
    lvIdle = lvglTask();
  }
  bsp_frame_stats_print((int)ui_current_view());
  idleTask(lvIdle);
}
