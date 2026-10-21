// ============================================================================
// laps.h  —  GNSS lap timer: a start/finish gate, crossing detection, and the
//            lap times it produces.
//
// Usage on a track day: tap START anywhere before going out - the pit, the
// paddock - and drive. Nothing to tap while driving: the pod finds the
// start/finish line by itself when the car first closes a loop (loopfind.h),
// times lap 1 from when the car first passed that point, and from then on
// every forward crossing closes a lap. The drive out of the pit is never part
// of a lap. (After a wake mid-session the gate is kept and the timer waits for
// the next crossing - the out-lap.)
//
// Timing comes from the GNSS fixes' own UTC timestamps, with the crossing
// instant interpolated between the two fixes either side of the line - at
// 25 Hz and 200 km/h the car moves 2.2 m between fixes, so taking either
// fix's time outright would be up to 40 ms out.
//
// The gate and the best/last laps survive deep sleep (a pit stop long enough
// for the pod to sleep does not end the session) - and nothing else: any
// other restart clears them, a watchdog reset, a crash or a firmware update
// as much as a power cycle. RTC memory is only kept across a deep-sleep wake.
// ============================================================================
#pragma once
#include <stdint.h>
#include <stdbool.h>

// In this order: the logger's .vbo opens from LAP_OUT_LAP up (a gate exists).
enum LapState : uint8_t {
  LAP_IDLE,        // no session
  LAP_SEARCH,      // session started; driving until the loop closes and the line is found
  LAP_OUT_LAP,     // line known; waiting for a crossing (after a wake)
  LAP_TIMING,      // a lap is being timed
};

struct LapInfo {
  LapState state;
  uint32_t currentMs;   // running time of the lap in progress (LAP_TIMING)
  uint32_t lastMs;      // most recent completed lap, 0 = none yet
  uint32_t bestMs;      // best completed lap this session, 0 = none yet
  uint16_t laps;        // completed laps this session
  float    searchM;     // driven since START while searching (LAP_SEARCH)
  // How far the line is: the lap in progress so far, and the reference - the
  // best lap's length (0 until a lap is complete). Left = calc_lap_left_m().
  float    lapM;        // driven since the line, this lap (LAP_TIMING)
  float    refM;        // the best lap's length
  // The live delta: this lap's time minus the best lap's at the same
  // distance - negative is faster. Only while a lap is timed against a best
  // one, and within the best lap's length.
  bool     haveDelta;
  int32_t  deltaMs;
  bool     gaveUp;     // the last search ended without finding a loop
};

void laps_task();                 // in loop(): consumes each new GNSS fix once
void laps_start();                // START: begin a session, look for the line
void laps_stop();                 // end the session; clears line and laps
void laps_info(LapInfo *out);
// The start/finish gate, for writing it into a track log: its centre and the
// unit direction of travel across it (x east, y north). False until the line
// has been found.
bool laps_gate(int32_t *lat1e7, int32_t *lon1e7, float *dirX, float *dirY);
