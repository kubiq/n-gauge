// A recorded drive, played back into the firmware's own state: the CSV into
// ObdState (as the CAN task fills it on the gauge), the GPX into GpsState and
// gps_read_fix(). Time is "replay ms": 0 at the drive's first row.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ReplayRow {
  uint32_t t;   // replay ms
  // NaN where the row has no value.
  float oilC, coolantC, boostBar, kmh, rpm, pedalRaw, absLoad, timing, iat, volts, baro;
  float tyreBar[4], tyreC[4];
  float gx, gy, gz;
};

struct ReplayFix {
  uint32_t t;         // replay ms
  uint32_t msOfDay;   // UTC, as the module reports it
  int32_t lat1e7, lon1e7;
  float altM, kmh, hdop;
  int sats;
};

struct Replay {
  std::vector<ReplayRow> rows;
  std::vector<ReplayFix> fixes;
  uint32_t lengthMs = 0;
  long long startUtcMs = 0;   // epoch ms at replay 0, or 0 when unknown

  bool loadCsv(const std::string &path);
  bool loadGpx(const std::string &path);
  void finish();   // after loading both: align the GPX to the CSV's clock
};

extern Replay replay;
// Feeds everything up to replay time `t` into the firmware's state.
void replay_feed(uint32_t t);
// The time is being sought back: start feeding from the beginning again.
void replay_rewind();
