// ============================================================================
// laps.cpp  —  see laps.h.
// ============================================================================
#include "laps.h"
#include "config.h"
#include "calc.h"
#include "gps.h"
#include "loopfind.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

// ---- session state that must survive deep sleep ----------------------------
// A pit stop with the engine off lets the pod sleep after five minutes, and a
// track-day session should not end because of it. RTC_DATA_ATTR keeps these
// across sleep, and only sleep: every other restart (power, watchdog, crash,
// update) starts them from their initial values - see laps.h.
RTC_DATA_ATTR static bool     searching = false;   // START tapped, line not found yet
RTC_DATA_ATTR static bool     gateSet   = false;   // the line is known
RTC_DATA_ATTR static int32_t  gateLat = 0, gateLon = 0;
RTC_DATA_ATTR static float    dirX = 0.0f, dirY = 0.0f;   // unit direction of travel
RTC_DATA_ATTR static uint32_t lastLapMs = 0, bestLapMs = 0;
// The best lap's length, driven: the reference for how far the line is
// (LapInfo.refM) - the lap being chased.
RTC_DATA_ATTR static float    bestLapM  = 0.0f;
// And its time against distance (calc_lap_prof_add): the live delta compares
// the lap in progress with it at the same distance. 2 KB of RTC memory.
RTC_DATA_ATTR static uint16_t bestProf[TRACK_PROF_N];
RTC_DATA_ATTR static uint16_t bestProfN = 0;
RTC_DATA_ATTR static uint16_t lapCount  = 0;
RTC_DATA_ATTR static bool     gaveUp    = false;

// ---- per-wake state --------------------------------------------------------
// Deliberately NOT kept across sleep: after a wake the car's path since the
// last fix is unknown, so the next crossing starts a fresh timed lap rather
// than closing one that spanned the pit stop.
static bool     inLap         = false;
static uint32_t lapStartFixMs = 0;    // crossing time, UTC ms of day
static uint32_t lapStartMillis = 0;   // the same instant on millis(), for display
static float    lapM          = 0.0f; // driven since the line, this lap (calc_lap_step_m)
static uint32_t lapT          = 0;    // the lap time at lapM (fix time), ms
static uint16_t curProf[TRACK_PROF_N];   // this lap's time against distance
static uint16_t curProfN      = 0;
static bool     haveDelta     = false;
static int32_t  deltaMs       = 0;    // this lap minus the best, at lapM
static bool     havePrev      = false;
static float    prevAlong = 0.0f, prevAcross = 0.0f;
static uint32_t prevFixMs     = 0;
static uint32_t lastSeq       = 0;

// ---- the search for the line (loopfind.h) -----------------------------------
// Breadcrumbs in PSRAM, only while searching: 4096 x 24 B = 96 KB, ~40 km of
// driving before the search gives up. Not kept across sleep - a search that
// spans a wake starts over from where the car is.
#define LAP_CRUMBS 4096
static LfCrumb   *crumbs = nullptr;
static LoopFinder finder;
static bool      searchStarted = false;          // the origin is set, this wake
static int32_t   originLat = 0, originLon = 0;   // breadcrumbs are metres from here
static float     prevX = 0.0f, prevY = 0.0f;     // the previous fix, search-local
static uint32_t  prevSearchFixMs = 0;
static bool      haveSearchPrev = false;

static void forgetPath() { havePrev = false; haveSearchPrev = false; }

// A new lap at the line: nothing driven, and its profile at 0 m / 0 ms.
static void startLapProfile() {
  lapM = 0.0f;
  lapT = 0;
  curProf[0] = 0;
  curProfN = 1;
  haveDelta = false;
}

// The lap goes on to toM metres at toT ms of lap time: its profile filled
// on the way, and the delta to the best lap at the new distance.
static void advanceLap(float toM, uint32_t toT) {
  curProfN = calc_lap_prof_add(curProf, curProfN, TRACK_PROF_N, TRACK_PROF_STEP_M,
                               lapM, lapT, toM, toT);
  lapM = toM;
  lapT = toT;
  const int32_t ref = calc_lap_prof_ms(bestProf, bestProfN, TRACK_PROF_STEP_M, lapM);
  haveDelta = ref >= 0;
  if (haveDelta) deltaMs = (int32_t)lapT - ref;
}

static void freeCrumbs() {
  if (crumbs) heap_caps_free(crumbs);
  crumbs = nullptr;
  searchStarted = false;
}

// A completed lap, crossing the line at crossMs. Returns false - and starts
// nothing - for a double trigger at the line.
static bool closeLap(uint32_t crossMs, uint32_t crossMillis) {
  if (inLap) {
    const uint32_t lapMs = calc_ms_of_day_diff(lapStartFixMs, crossMs);
    if (lapMs < TRACK_MIN_LAP_MS) return false;   // jitter at the line: ignore
    lastLapMs = lapMs;
    if (bestLapMs == 0 || lapMs < bestLapMs) {
      bestLapMs = lapMs;
      bestLapM  = lapM;
      memcpy(bestProf, curProf, curProfN * sizeof(curProf[0]));
      bestProfN = curProfN;
    }
    lapCount++;
    Serial.printf("[OK] lap %u: %lu ms (best %lu)\n", lapCount,
                  (unsigned long)lapMs, (unsigned long)bestLapMs);
  }
  inLap          = true;
  lapStartFixMs  = crossMs;
  lapStartMillis = crossMillis;
  startLapProfile();
  return true;
}

// One fix while searching. Returns true once the line is found - the caller
// then carries on with the same fix as a timed one.
static bool searchStep(const GpsFix &f) {
  if (!crumbs) {
    crumbs = (LfCrumb *)heap_caps_malloc(LAP_CRUMBS * sizeof(LfCrumb), MALLOC_CAP_SPIRAM);
    if (!crumbs) {
      Serial.println(F("[ERR] lap search: no PSRAM for breadcrumbs"));
      searching = false;
      gaveUp = true;
      return false;
    }
  }
  if (!searchStarted) {
    originLat = f.lat1e7;
    originLon = f.lon1e7;
    lf_init(&finder, crumbs, LAP_CRUMBS);
    searchStarted = true;
  }

  float x, y;
  calc_local_xy(originLat, originLon, f.lat1e7, f.lon1e7, &x, &y);
  const int m = lf_add(&finder, x, y, f.fixMs);

  if (m == LF_FULL) {
    Serial.println(F("[..] lap search: no loop in ~40 km - giving up"));
    searching = false;
    gaveUp = true;
    freeCrumbs();
    return false;
  }
  if (m < 0) {
    prevX = x; prevY = y; prevSearchFixMs = f.fixMs; haveSearchPrev = true;
    return false;
  }

  // The loop closed on crumb m: that is the line - its position, across its
  // heading - and the car first passed it at its fix time, so lap 1 has been
  // running since then.
  const LfCrumb c = crumbs[m];
  calc_offset_ll(originLat, originLon, c.x, c.y, &gateLat, &gateLon);
  dirX = c.hx;
  dirY = c.hy;
  gateSet   = true;
  searching = false;
  inLap          = true;
  lapStartFixMs  = c.fixMs;
  lapStartMillis = millis() - calc_ms_of_day_diff(c.fixMs, f.fixMs);
  // Its distance so far, backdated as its time is: the path from the line's
  // crumb to the previous fix - laps_task() adds this last step itself, on
  // its way to the crossing check. (Started at 0 here, lap 1 measured the
  // few metres since the line was noticed: a 197 s lap of "3 m", replayed
  // from 2026-10-11, and the countdown stuck at 0 the lap after.)
  float backM = finder.dist - c.dist - (haveSearchPrev ? sqrtf((x - prevX) * (x - prevX) + (y - prevY) * (y - prevY)) : 0.0f);
  if (backM < 0.0f) backM = 0.0f;
  // Its profile too, from the breadcrumbs since the line - each one's path
  // length and time, every LF_SPACING_M - then on to the previous fix. (As
  // one straight run, line to here, lap 1 was a lap at constant speed, and
  // the delta to it, replayed from 2026-10-11, was seconds off mid-lap.)
  startLapProfile();
  for (uint16_t i = (uint16_t)m + 1; i < finder.n; i++) {
    const float d = crumbs[i].dist - c.dist;
    if (d > backM) break;
    advanceLap(d, calc_ms_of_day_diff(c.fixMs, crumbs[i].fixMs));
  }
  advanceLap(backM, calc_ms_of_day_diff(c.fixMs, haveSearchPrev ? prevSearchFixMs : f.fixMs));
  Serial.printf("[OK] lap line found after %.0f m (direction %.0f deg); lap 1 running\n",
                finder.dist, atan2f(dirX, dirY) * 57.2957795f);

  // The match is noticed up to LF_RADIUS_M late, so the car may be past the
  // line already. Hand the crossing check the previous fix, in the new gate's
  // terms: if this step crossed, lap 1 closes at the interpolated instant,
  // exactly as any later lap does.
  if (haveSearchPrev) {
    calc_gate_coords(prevX - c.x, prevY - c.y, dirX, dirY, &prevAlong, &prevAcross);
    prevFixMs = prevSearchFixMs;
    havePrev  = true;

    // Both fixes past it: the crossing was earlier still, and no step will
    // ever see it. Close lap 1 back then, at the speed along the line.
    float along, across;
    calc_gate_coords(x - c.x, y - c.y, dirX, dirY, &along, &across);
    const uint32_t dt = calc_ms_of_day_diff(prevFixMs, f.fixMs);
    if (prevAlong >= 0.0f && along > prevAlong && dt > 0 && dt <= TRACK_MAX_FIX_GAP_MS) {
      const uint32_t ago = (uint32_t)lroundf(along * dt / (along - prevAlong));
      closeLap((f.fixMs + 86400000UL - ago) % 86400000UL, millis() - ago);
    }
  }
  freeCrumbs();
  return true;
}

void laps_task() {
  if ((!gateSet && !searching) || !gps.haveFix) { forgetPath(); return; }

  GpsFix f;
  if (!gps_read_fix(&f)) return;
  if (f.seq == lastSeq) return;         // no new fix since last time
  lastSeq = f.seq;
  // A fix that cannot be used is SKIPPED, the previous point kept - not
  // forgotten. Forgetting it left the next good fix with nothing to pair
  // with, so a crossing between the fix before the bad one and the fix after
  // it was never tested, and two laps ran together as one. A real gap is
  // already safe: a step longer than TRACK_MAX_FIX_GAP_MS is never tested.
  if (!f.timeOk) return;

  // RMC and GGA both carry a position, so one 40 ms epoch can publish twice
  // with the same time. A zero-length step can never cross anything, but it
  // would overwrite the previous point with a duplicate - skip it.
  if (havePrev && f.fixMs == prevFixMs) return;

  // The same quality gate the GPX track uses. A fix the receiver does not
  // stand behind must not be able to close a lap - or a loop. Skipped, like
  // the one without a time above.
  if (!calc_fix_quality_ok(gps.sats, gps.hdop, GPS_MIN_SATS_LOG, GPS_MAX_HDOP_LOG)) return;

  if (searching && !searchStep(f)) return;

  float x, y;
  calc_local_xy(gateLat, gateLon, f.lat1e7, f.lon1e7, &x, &y);
  float along, across;
  calc_gate_coords(x, y, dirX, dirY, &along, &across);

  if (havePrev) {
    const uint32_t dt = calc_ms_of_day_diff(prevFixMs, f.fixMs);
    const float fr = (dt <= TRACK_MAX_FIX_GAP_MS)
        ? calc_gate_cross(prevAlong, prevAcross, along, across, TRACK_GATE_HALF_M)
        : -1.0f;
    // The distance of this step, for how far the line is: split at the line
    // as the time is - the part before it closes this lap, the rest starts
    // the next.
    const float step = (dt <= TRACK_MAX_FIX_GAP_MS)
        ? calc_lap_step_m(prevAlong, prevAcross, along, across, dt, TRACK_MAX_MPS)
        : 0.0f;
    if (fr >= 0.0f) {
      // The crossing instant, interpolated between the fix before the line
      // and the fix after it.
      const uint32_t crossMs = (prevFixMs + (uint32_t)lroundf(fr * dt)) % 86400000UL;
      const uint32_t crossMillis = millis() - (uint32_t)lroundf((1.0f - fr) * dt);
      if (inLap) advanceLap(lapM + fr * step, calc_ms_of_day_diff(lapStartFixMs, crossMs));
      if (closeLap(crossMs, crossMillis))
        advanceLap((1.0f - fr) * step, calc_ms_of_day_diff(crossMs, f.fixMs));
      else if (inLap)   // jitter at the line: the same lap goes on
        advanceLap(lapM + (1.0f - fr) * step, calc_ms_of_day_diff(lapStartFixMs, f.fixMs));
    } else if (inLap) {
      advanceLap(lapM + step, calc_ms_of_day_diff(lapStartFixMs, f.fixMs));
    }
  }

  prevAlong  = along;
  prevAcross = across;
  prevFixMs  = f.fixMs;
  havePrev   = true;
}

void laps_start() {
  freeCrumbs();
  searching = true;
  gateSet   = false;
  gaveUp    = false;
  lastLapMs = bestLapMs = 0;
  bestLapM  = lapM = 0.0f;
  bestProfN = curProfN = 0;
  haveDelta = false;
  lapCount  = 0;
  inLap     = false;
  forgetPath();
  Serial.println(F("[OK] lap session started - finding the line"));
}

void laps_stop() {
  freeCrumbs();
  searching = gateSet = false;
  gaveUp    = false;
  lastLapMs = bestLapMs = 0;
  bestLapM  = lapM = 0.0f;
  bestProfN = curProfN = 0;
  haveDelta = false;
  lapCount  = 0;
  inLap     = false;
  forgetPath();
}

bool laps_gate(int32_t *lat1e7, int32_t *lon1e7, float *dx, float *dy) {
  if (!gateSet) return false;
  *lat1e7 = gateLat; *lon1e7 = gateLon; *dx = dirX; *dy = dirY;
  return true;
}

void laps_info(LapInfo *out) {
  out->state = gateSet   ? (inLap ? LAP_TIMING : LAP_OUT_LAP)
             : searching ? LAP_SEARCH
                         : LAP_IDLE;
  out->currentMs = inLap ? millis() - lapStartMillis : 0;
  out->lastMs    = lastLapMs;
  out->bestMs    = bestLapMs;
  out->laps      = lapCount;
  out->searchM   = (searching && searchStarted) ? finder.dist : 0.0f;
  out->lapM      = inLap ? lapM : 0.0f;
  out->refM      = bestLapM;
  out->haveDelta = inLap && haveDelta;
  out->deltaMs   = deltaMs;
  out->gaveUp    = gaveUp;
}
