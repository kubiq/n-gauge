// ============================================================================
// logger.h  —  drive logging to the onboard microSD.
//   Logs DECODED values rather than every CAN frame, so rows stay small and
//   directly plottable. The one exception is the raw TPMS payload, whose byte
//   layout is still unsolved and so must be kept verbatim.
//
//   `oilRaw` is logged alongside the computed temperature on purpose: the
//   oil-temp formula is only validated over 33-53 C, so keeping the raw byte
//   lets the curve be re-derived offline from a hot drive without reflashing.
// ============================================================================
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void logger_init();   // mount the card; no CSV yet - see logger_start()
// Start the logger's own task, on core 0 (2026-10-11): all card writing then
// happens there, so a save - 13 ms settled, up to ~460 ms when the card
// stalls - never holds up the screen on core 1. Call once at the end of
// setup(), after anything else that uses the card alone (the SD firmware
// check).
//
// It samples at LOG_INTERVAL_MS. The CSV is created on the first row after
// the first CAN frame, named by its start time when the clock is set
// (drive_2026-09-25_14-32_3fa81c.csv - the last part is the session, one per
// boot or wake), else numbered - so a boot or a parked wake
// with the car off leaves no file behind.
//
// It also writes a GPX track per trip, alongside the CSV. Opened lazily on
// the first accepted GPS fix rather than at boot, so sitting parked doesn't
// litter the card with empty tracks. Standard GPX 1.1, so it opens in Google
// Earth, Strava, gpx.studio, anything.
void logger_start();

// Anything on core 1 that touches the card takes this lock (the logger's own
// public functions already do). Normally that waits for one save in progress
// - but a card that stalls can hold a save, and so the lock, for seconds, and
// a wait on the loop's core longer than the watchdog's 8 s reboots the pod.
// So core 1 takes it with a limit, LoggerSdLock lock(ms), feeding the watchdog
// while it waits, and checks it: if (!lock) the card is busy - a 503 for the
// phone, or skip it. The plain LoggerSdLock() waits for good; only for where
// the logger cannot be in a save (its own task; the card lent to the PC).
void logger_sd_lock();
bool logger_sd_lock_for(uint32_t ms);
void logger_sd_unlock();
struct LoggerSdLock {
  LoggerSdLock() : held(true) { logger_sd_lock(); }
  explicit LoggerSdLock(uint32_t ms) : held(logger_sd_lock_for(ms)) {}
  ~LoggerSdLock() { if (held) logger_sd_unlock(); }
  explicit operator bool() const { return held; }
  LoggerSdLock(const LoggerSdLock &) = delete;
  LoggerSdLock &operator=(const LoggerSdLock &) = delete;
 private:
  const bool held;
};
// How long the phone's pages wait for the card before answering 503.
#define SD_LOCK_WEB_MS    3000
// How long sleep waits to close the files before sleeping anyway.
#define SD_LOCK_CLOSE_MS  6000
// Just "is a card mounted" - for telling the driver the card is missing. Not
// "is a log open": with a card, no file exists until the car is on.
bool logger_card_ok();
// A card is mounted, but a log file could not be opened on it - full, or
// failing. That writer has stopped; the others, and the phone's file pages,
// carry on. Cleared when the card comes back from the PC.
bool logger_write_failed();
// Bench aid: print the last `bytes` of the open drive log to serial, so its
// rows can be checked without taking the card out of the pod.
void logger_dump_tail(size_t bytes);

// Flush and close cleanly. MUST be called before deep sleep: FATFS caches
// metadata, so sleeping with the file open can leave the last rows missing or
// the file length wrong on the card. Stops the logger for good - for sleep
// and reboots only.
void logger_close();

// One line per sleep, appended to /sleeplog.txt. This is the only way to see
// what the parked wake/sleep cycle actually did in the car - there is no
// serial console in a dash, and every wake is a fresh boot with no memory of
// the last one. Must be called while the card is still mounted, i.e. BEFORE
// logger_close().
void logger_note_sleep(const char *decision, int wakeCause, int canRx,
                       unsigned long fruitless);

// One line per boot that stays up, appended to /bootlog.txt (bootlog.cpp
// writes it): what came up and how - the GPS's configuration and real fix
// rate, the clock, CAN, the card, RAM. The serial console says all of it at
// boot, but the pod in the car has no console; this is the car's record.
void logger_note_boot(const char *line);

// Distance for the current trip, in km.
//
// Lives here rather than in gps.cpp because it is summed only over fixes that
// passed every GPX gate - quality, plausibility, rate - and the logger is the
// only place that knows a fix is trustworthy. Summing raw positions would let
// one tunnel spike add kilometres nobody drove.
//
// Counts only while moving (GNSS speed >= LOG_IDLE_SPEED_KMH), so position
// noise at a standstill does not accumulate. Survives deep sleep, so a fuel
// stop does not reset it. Resets after settings.tripGapMin minutes without an
// accepted point (default TRIP_GAP_MIN, editable from the settings page), or on
// any restart that is not a deep-sleep wake (power, watchdog, crash, update).
float logger_trip_km();

// How much is logged - settings.logLevel. Each level adds to the one below.
// The sleeplog is kept at every level, Off included: it is tiny, and the only
// record of why the pod slept or woke. The trip distance is counted at every
// level too - it comes from the GPS, not from the file.
enum LogLevel : uint8_t {
  LOG_OFF    = 0,   // nothing but the sleeplog
  LOG_TRIPS  = 1,   // GPX tracks, and the .vbo in track mode
  LOG_NORMAL = 2,   // + the drive CSV, decoded values
  // 3, Full, added the raw hex payloads (TPMS, oil DID) until 2026-10-30. A
  // gauge with 3 stored loads the default (settings.cpp: out of range).
};

// The g-meter's latest horizontal g (calibrated, as the screen shows it):
// lateral, and longitudinal as force felt forwards (+ = braking). For the
// track log's IMU channels - see log_vbo.cpp.
void logger_note_g(float lateral, float longitudinal);
// The loop's latest raw IMU sample, for the CSV's gx/gy/gz - the logger runs
// on core 0 and must not use the I2C bus, which core 1 shares with touch.
void logger_note_raw_g(float gx, float gy, float gz);
// How long the previous loop() pass took - for the CSV's loopMaxMs, the
// yardstick for what logging costs (logMaxMs).
void logger_note_loop_us(uint32_t us);

// The SD card lent to the PC as a USB drive (usbdisk.cpp): suspend closes
// every file and stops all card access; resume takes it back (no remount: the
// PC had it read-only) and logging goes on in a new file. logger_card_shared() is true in between. Suspend is false,
// and nothing changes, when the card is too busy to let go of.
bool logger_suspend();
void logger_resume();
bool logger_card_shared();

// Is this card file (a plain name, no slash) open for writing right now?
// The app's delete (/api/delete) refuses those. Under LoggerSdLock.
bool logger_is_open(const char *name);
