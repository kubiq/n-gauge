// ============================================================================
// podstate.cpp  —  the gauge's state (podstate.h), on the gauge: the facts
// gathered from the modules once per loop() pass, worked out, and shared.
//
// One writer, loop(). The logger and the web server run on other tasks and
// take a copy under a spinlock; loop() itself reads its own without one.
// ============================================================================
#include <Arduino.h>
#include "podstate.h"
#include "config.h"
#include "gps.h"
#include "logger.h"
#include "obd.h"
#include "ota.h"
#include "power.h"
#include "rtc.h"
#include "ui.h"          // UiFault: the same bits as PS_FAULT_*
#include "webapi.h"

// Two enums, so compared as numbers.
#define SAME_BIT(a, b) ((unsigned)(a) == (unsigned)(b))
static_assert(SAME_BIT(PS_FAULT_OIL_HOT, FAULT_OIL_HOT) && SAME_BIT(PS_FAULT_COOL_HOT, FAULT_COOL_HOT) &&
              SAME_BIT(PS_FAULT_CAN, FAULT_CAN) && SAME_BIT(PS_FAULT_GPS, FAULT_GPS) &&
              SAME_BIT(PS_FAULT_SD, FAULT_SD) && SAME_BIT(PS_FAULT_SD_WRITE, FAULT_SD_WRITE),
              "podstate's faults are ui.h's UiFault bits");
#undef SAME_BIT

static PodState live = {};          // loop()'s own
static PodState shared = {};        // the copy the other tasks read
static portMUX_TYPE sharedMux = portMUX_INITIALIZER_UNLOCKED;

static PsCfg cfg() {
  return {
    OBD_STALE_MS, ENGINE_RUNNING_RPM, MOVING_OBD_KMH, SLEEP_HOLD_MOVING_KMH,
    AP_DRIVE_ON_KMH, AP_DRIVE_ON_MS, AP_DRIVE_OFF_KMH, AP_DRIVE_OFF_MS,
    LOG_IDLE_AFTER_MS, power_sleep_after_ms(), COLD_BOOT_AWAKE_MS,
    CAN_MISSING_GRACE_MS, GPS_MISSING_GRACE_MS,
  };
}

// What it says on the serial port: a line when the car, the drive, or a
// decision changes - not each flick between moving and still at a crawl.
static void report(const PodState &was, const PodState &s) {
  const bool driveChanged = (was.motion == PS_DRIVING) != (s.motion == PS_DRIVING) ||
                            (was.motion == PS_PARKED) != (s.motion == PS_PARKED);
  if (was.started && s.car == was.car && !driveChanged && s.wifiWanted == was.wifiWanted &&
      s.logRate == was.logRate && s.sleepOk == was.sleepOk)
    return;
  Serial.printf("[..] state: car %s, %s, wifi %s, log %s, sleep %s (awake 0x%02x)\n",
                ps_car_name(s.car), ps_motion_name(s.motion), s.wifiWanted ? "wanted" : "off",
                s.logRate == PS_LOG_THIN ? "thin" : "full", s.sleepOk ? "ok" : "held",
                s.awakeBecause);
}

void podstate_update(uint32_t now, bool oilHot, bool coolHot) {
  PsFacts f = {};
  f.now = now;
  f.canLinkUp = obd.linkUp;
  f.lastFrameMs = obd.lastFrameMs;      // once: the CAN task writes it
  f.rpmFresh = obd_rpm_fresh();
  f.rpm = (int16_t)obd.rpm;
  f.obdKmhFresh = obd_speed_fresh();
  f.obdKmh = (int16_t)obd.speedKmh;
  f.gpsLink = gps.linkUp;
  f.gpsFix = gps.haveFix;
  f.gpsKmh = gps.speedKmh;
  f.sats = gps.sats;
  f.card = logger_card_shared() ? PS_CARD_SHARED
         : !logger_card_ok() ? PS_CARD_NONE
         : logger_write_failed() ? PS_CARD_FAILED
         : PS_CARD_OK;
  f.clockValid = clock_valid();
  f.apUp = web_is_up();
  f.apClients = web_clients();
  f.downloading = web_download_active();
  f.usbHost = power_usb_host();
  f.lastUserMs = power_last_activity_ms();
  f.waking = power_wake_pending();
  // Once: neither changes while running (a rollback is read off the flash).
  static const PsWake wake = power_wake_cause();
  static const bool rolledBack = ota_rolled_back();
  f.wake = wake;
  f.fw = rolledBack ? PS_FW_ROLLED_BACK : ota_on_probation() ? PS_FW_PROBATION : PS_FW_OK;
  f.oilHot = oilHot;
  f.coolHot = coolHot;

  const PodState was = live;
  podstate_step(&live, f, cfg());
  report(was, live);
  portENTER_CRITICAL(&sharedMux);
  shared = live;
  portEXIT_CRITICAL(&sharedMux);
}

const PodState &podstate() { return live; }

PodState podstate_copy() {
  portENTER_CRITICAL(&sharedMux);
  const PodState s = shared;
  portEXIT_CRITICAL(&sharedMux);
  return s;
}
