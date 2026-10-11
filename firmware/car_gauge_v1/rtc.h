// ============================================================================
// rtc.h  —  PCF85063 real-time clock at I2C 0x51, and the one honest answer to
//           "do we know what time it is?".
//
// The board has always had this chip and always ignored it, so the clock was
// wrong on every boot until the GNSS got a fix - which indoors is never. Every
// log written before that point is stamped 1980, which is exactly how
// drive_0242.csv became unreadable: a file you cannot date is a file you skip.
//
// ⚠ There is no backup cell fitted (deliberately - see the power work), so
// this does NOT keep time across a full power removal. What it does keep time
// across is everything that actually happens in the car:
//   - deep sleep, because the 3.3 V rail stays up the whole time;
//   - reboots, watchdog resets and reflashes, which are not power cycles.
// So one GNSS fix on one drive is enough to keep every later log correctly
// stamped, including on a cold morning in a garage with no sky view. Pull the
// 12 V and it forgets, reports itself invalid, and we fall back to waiting for
// the GNSS exactly as before.
//
// The RTC holds UTC, matching the system clock. Timezone is a display concern
// applied via TZ (see settings_apply_tz) and must never be stored here.
// ============================================================================
#pragma once
#include <stdbool.h>

struct RtcState {
  bool present   = false;   // answered on the I2C bus at all
  bool timeValid = false;   // held a plausible time at boot, oscillator OK
  bool seeded    = false;   // GNSS time has been written to it this boot
  long driftSec  = 0;       // last measured RTC-minus-system, signed
};

extern RtcState rtc;

// Probe the chip, force 24-hour mode, and seed the system clock if the RTC
// holds a time worth trusting. Call AFTER bsp_init() (which brings up I2C) and
// BEFORE logger_init(), because log filenames are stamped as they are created.
void rtc_init();

// In loop(): once the GNSS has a fix, push that time into the RTC whenever the
// two have drifted apart by more than RTC_DRIFT_TOL_SEC. Cheap - an I2C read
// every RTC_SYNC_CHECK_MS and a write only when it is actually needed, so
// there is no wear concern the way an NVS-backed clock would have.
void rtc_task();

// Set the clock from an externally supplied UTC epoch - currently the settings
// page handing over the phone's own clock. Sets the system clock AND writes the
// RTC, so it survives the next sleep.
//
// Exists because indoors there is no GNSS fix, so without it a garage-bound
// board can never date a log at all. Returns false if the value is not a
// plausible present: this is the only clock source that comes from outside the
// device, so it is the only one that can be handed an arbitrary number.
bool rtc_set_from_epoch(long long epochUtc);

// THE test for "is the clock trustworthy", for anything that stamps a file.
//
// Use this rather than gps.timeSet. They are not the same question, and the
// difference was a real bug: the system clock survives deep sleep (the ESP32's
// own RTC timer keeps counting), but gps.timeSet is a fresh false on every
// boot - so after each wake the logger blanked its timestamps even though the
// clock was perfectly correct. This asks the clock, not one of its sources.
bool clock_valid();
