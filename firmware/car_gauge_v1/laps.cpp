// ============================================================================
// laps.cpp  —  see laps.h.
// ============================================================================
#include "laps.h"
#include "config.h"
#include "calc.h"
#include "gps.h"
#include <Arduino.h>
#include <math.h>

// ---- session state that must survive deep sleep ----------------------------
// A pit stop with the engine off lets the pod sleep after five minutes, and a
// track-day session should not end because of it. RTC_DATA_ATTR keeps these
// across sleep; a power cycle clears them, which is someone unplugging the pod.
RTC_DATA_ATTR static bool     gateSet   = false;
RTC_DATA_ATTR static bool     gateArmed = false;   // direction known
RTC_DATA_ATTR static int32_t  gateLat = 0, gateLon = 0;
RTC_DATA_ATTR static float    dirX = 0.0f, dirY = 0.0f;   // unit direction of travel
RTC_DATA_ATTR static uint32_t lastLapMs = 0, bestLapMs = 0;
RTC_DATA_ATTR static uint16_t lapCount  = 0;

// ---- per-wake state --------------------------------------------------------
// Deliberately NOT kept across sleep: after a wake the car's path since the
// last fix is unknown, so the next crossing starts a fresh timed lap rather
// than closing one that spanned the pit stop.
static bool     inLap         = false;
static uint32_t lapStartFixMs = 0;    // crossing time, UTC ms of day
static uint32_t lapStartMillis = 0;   // the same instant on millis(), for display
static bool     havePrev      = false;
static float    prevAlong = 0.0f, prevAcross = 0.0f;
static uint32_t prevFixMs     = 0;
static uint32_t lastSeq       = 0;
// Lap 1 starts when the car pulls away from the line, not at the first
// crossing (2026-10-09: the loop driven after SET was the "lap", and the pod
// counted it as an out-lap). The last fix still slower than TRACK_DEPART_KMH
// after SET - or the SET fix itself, for a flying start - is that moment.
static uint32_t departFixMs = 0, departMillis = 0;

static void forgetPath() { havePrev = false; }

void laps_task() {
  if (!gateSet || !gps.haveFix) { forgetPath(); return; }

  GpsFix f;
  if (!gps_read_fix(&f)) return;
  if (f.seq == lastSeq) return;         // no new fix since last time
  lastSeq = f.seq;
  if (!f.timeOk) { forgetPath(); return; }

  // RMC and GGA both carry a position, so one 40 ms epoch can publish twice
  // with the same time. A zero-length step can never cross anything, but it
  // would overwrite the previous point with a duplicate - skip it.
  if (havePrev && f.fixMs == prevFixMs) return;

  // The same quality gate the GPX track uses. A fix the receiver does not
  // stand behind must not be able to close a lap.
  if (!calc_fix_quality_ok(gps.sats, gps.hdop, GPS_MIN_SATS_LOG, GPS_MAX_HDOP_LOG)) {
    forgetPath();
    return;
  }

  float x, y;
  calc_local_xy(gateLat, gateLon, f.lat1e7, f.lon1e7, &x, &y);

  if (!gateArmed) {
    // Still at the line: lap 1 has not started yet.
    if (gps.speedKmh < TRACK_DEPART_KMH) {
      departFixMs  = f.fixMs;
      departMillis = millis();
    }
    // Learn the direction from where the car goes. At the set point the GNSS
    // course is meaningless (the car is standing still), so wait until it is
    // clearly away and moving - then the bearing from the point to the car is
    // the direction of travel across the line.
    const float d = sqrtf(x * x + y * y);
    if (d >= TRACK_ARM_DIST_M && gps.speedKmh >= TRACK_ARM_KMH) {
      dirX = x / d;
      dirY = y / d;
      gateArmed = true;
      // Lap 1 has been running since the car pulled away; the next crossing
      // closes it like any other lap.
      inLap          = true;
      lapStartFixMs  = departFixMs;
      lapStartMillis = departMillis;
      Serial.printf("[..] lap gate armed: direction %.0f deg, lap 1 running\n",
                    atan2f(dirX, dirY) * 57.2957795f);
    }
    forgetPath();                       // the car is already past the line
    return;
  }

  float along, across;
  calc_gate_coords(x, y, dirX, dirY, &along, &across);

  if (havePrev) {
    const uint32_t dt = calc_ms_of_day_diff(prevFixMs, f.fixMs);
    const float fr = (dt <= TRACK_MAX_FIX_GAP_MS)
        ? calc_gate_cross(prevAlong, prevAcross, along, across, TRACK_GATE_HALF_M)
        : -1.0f;
    if (fr >= 0.0f) {
      // The crossing instant, interpolated between the fix before the line
      // and the fix after it.
      const uint32_t crossMs = (prevFixMs + (uint32_t)lroundf(fr * dt)) % 86400000UL;
      const uint32_t crossMillis = millis() - (uint32_t)lroundf((1.0f - fr) * dt);

      bool startLap = true;
      if (inLap) {
        const uint32_t lapMs = calc_ms_of_day_diff(lapStartFixMs, crossMs);
        if (lapMs < TRACK_MIN_LAP_MS) {
          startLap = false;             // double trigger at the line: ignore
        } else {
          lastLapMs = lapMs;
          if (bestLapMs == 0 || lapMs < bestLapMs) bestLapMs = lapMs;
          lapCount++;
          Serial.printf("[OK] lap %u: %lu ms (best %lu)\n", lapCount,
                        (unsigned long)lapMs, (unsigned long)bestLapMs);
        }
      }
      if (startLap) {
        inLap          = true;
        lapStartFixMs  = crossMs;
        lapStartMillis = crossMillis;
      }
    }
  }

  prevAlong  = along;
  prevAcross = across;
  prevFixMs  = f.fixMs;
  havePrev   = true;
}

bool laps_set_line() {
  GpsFix f;
  if (!gps.haveFix || !gps_read_fix(&f)) return false;
  gateLat   = f.lat1e7;
  gateLon   = f.lon1e7;
  gateSet   = true;
  gateArmed = false;
  lastLapMs = bestLapMs = 0;
  lapCount  = 0;
  inLap     = false;
  departFixMs  = f.fixMs;               // a flying start: lap 1 from here
  departMillis = millis();
  forgetPath();
  Serial.println(F("[OK] lap start/finish line set"));
  return true;
}

void laps_stop() {
  gateSet = gateArmed = false;
  lastLapMs = bestLapMs = 0;
  lapCount  = 0;
  inLap     = false;
  forgetPath();
}

bool laps_gate(int32_t *lat1e7, int32_t *lon1e7, float *dx, float *dy) {
  if (!gateSet || !gateArmed) return false;
  *lat1e7 = gateLat; *lon1e7 = gateLon; *dx = dirX; *dy = dirY;
  return true;
}

void laps_info(LapInfo *out) {
  out->state = !gateSet   ? LAP_IDLE
             : !gateArmed ? LAP_WAIT_DIR
             : !inLap     ? LAP_OUT_LAP
                          : LAP_TIMING;
  out->currentMs = inLap ? millis() - lapStartMillis : 0;
  out->lastMs    = lastLapMs;
  out->bestMs    = bestLapMs;
  out->laps      = lapCount;
}
