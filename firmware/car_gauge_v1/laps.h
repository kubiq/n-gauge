// ============================================================================
// laps.h  —  GNSS lap timer: a start/finish gate, crossing detection, and the
//            lap times it produces.
//
// Usage on a track day: stop ON the start/finish line, tap SET, drive. The
// gate's direction is learned as the car pulls away, lap 1 is timed from that
// moment, and every forward crossing closes a lap. (Out-lap only after a wake
// mid-session: then the timer waits for the next crossing.)
//
// Timing comes from the GNSS fixes' own UTC timestamps, with the crossing
// instant interpolated between the two fixes either side of the line - at
// 25 Hz and 200 km/h the car moves 2.2 m between fixes, so taking either
// fix's time outright would be up to 40 ms out.
//
// The gate and the best/last laps survive deep sleep (a pit stop long enough
// for the pod to sleep does not end the session). A power cycle clears them.
// ============================================================================
#pragma once
#include <stdint.h>
#include <stdbool.h>

enum LapState : uint8_t {
  LAP_IDLE,        // no line set
  LAP_WAIT_DIR,    // line set; waiting to learn the direction as the car leaves
  LAP_OUT_LAP,     // direction known; waiting for a crossing (after a wake)
  LAP_TIMING,      // a lap is being timed
};

struct LapInfo {
  LapState state;
  uint32_t currentMs;   // running time of the lap in progress (LAP_TIMING)
  uint32_t lastMs;      // most recent completed lap, 0 = none yet
  uint32_t bestMs;      // best completed lap this session, 0 = none yet
  uint16_t laps;        // completed laps this session
};

void laps_task();                 // in loop(): consumes each new GNSS fix once
bool laps_set_line();             // false if there is no usable fix to set it at
void laps_stop();                 // end the session; clears line and laps
void laps_info(LapInfo *out);
// The start/finish gate, for writing it into a track log: its centre and the
// unit direction of travel across it (x east, y north). False until the line
// is set AND its direction has been learned.
bool laps_gate(int32_t *lat1e7, int32_t *lon1e7, float *dirX, float *dirY);
