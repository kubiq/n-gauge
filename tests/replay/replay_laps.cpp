// Replays a recorded GPX through the real lap timer (laps.cpp), fix by fix
// at the recorded times - to see on a desk what it does with real laps:
// where loop closure finds the line, each lap's time, and how far the line
// is (LapInfo.lapM / refM) along each lap.
//
//   tests/replay/replay_laps.sh data/trip_2026-10-11_12-45_568067.gpx [HH:MM:SS]
//
// The optional time (UTC, as in the GPX) is when START is "tapped"; by
// default the first point. Nothing of the board is needed: Arduino.h and
// esp_heap_caps.h are stand-ins (stubs/), gps_read_fix() is fed from here.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Arduino.h"
#include "calc.h"
#include "gps.h"
#include "laps.h"

uint32_t replayMillis = 0;
ReplaySerial Serial;
GpsState gps;

static GpsFix current;
bool gps_read_fix(GpsFix *out) {
  *out = current;
  return true;
}

struct Pt { int32_t lat, lon; uint32_t ms; int sats; float hdop; };

static bool parseTime(const char *s, uint32_t *msOfDay) {
  int h, m;
  float sec;
  // 2026-10-11T10:45:12.36Z
  const char *t = strchr(s, 'T');
  if (!t || sscanf(t + 1, "%d:%d:%f", &h, &m, &sec) != 3) return false;
  *msOfDay = (uint32_t)(h * 3600000 + m * 60000) + (uint32_t)lroundf(sec * 1000.0f);
  return true;
}

static void fmt(uint32_t ms, char *b) { snprintf(b, 16, "%02u:%02u:%02u", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60); }

int main(int argc, char **argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: replay_laps file.gpx [HH:MM:SS]\n"); return 2; }
  FILE *f = std::fopen(argv[1], "r");
  if (!f) { std::perror(argv[1]); return 2; }
  std::vector<Pt> pts;
  char line[1024];
  while (std::fgets(line, sizeof(line), f)) {
    const char *p = std::strstr(line, "<trkpt lat=\"");
    if (!p) continue;
    double lat, lon;
    if (sscanf(p, "<trkpt lat=\"%lf\" lon=\"%lf\"", &lat, &lon) != 2) continue;
    const char *tm = std::strstr(line, "<time>");
    uint32_t ms;
    if (!tm || !parseTime(tm, &ms)) continue;
    const char *sa = std::strstr(line, "<sat>"), *hd = std::strstr(line, "<hdop>");
    Pt q{(int32_t)llround(lat * 1e7), (int32_t)llround(lon * 1e7), ms, sa ? atoi(sa + 5) : 12, hd ? (float)atof(hd + 6) : 1.0f};
    pts.push_back(q);
  }
  std::fclose(f);
  if (pts.empty()) { std::fprintf(stderr, "no points\n"); return 1; }

  uint32_t startAt = pts[0].ms;
  if (argc > 2) {
    int h, m, s;
    if (sscanf(argv[2], "%d:%d:%d", &h, &m, &s) == 3) startAt = (uint32_t)(h * 3600000 + m * 60000 + s * 1000);
  }

  char t0[16], t1[16];
  fmt(pts.front().ms, t0);
  fmt(pts.back().ms, t1);
  std::printf("%zu fixes, %s .. %s UTC; START at the first fix from %s\n", pts.size(), t0, t1, argc > 2 ? argv[2] : t0);

  bool started = false;
  uint16_t laps = 0;
  int lastState = -1;
  uint32_t nextReport = 0;
  for (size_t i = 0; i < pts.size(); i++) {
    const Pt &q = pts[i];
    replayMillis = q.ms - pts[0].ms + 1000;
    if (!started && q.ms >= startAt) {
      laps_start();
      started = true;
    }
    gps.haveFix = true;
    gps.sats = (uint8_t)q.sats;
    gps.hdop = q.hdop;
    current = GpsFix{q.lat, q.lon, 0.0f, q.ms, true, (uint32_t)i + 1};
    laps_task();

    LapInfo li;
    laps_info(&li);
    char now[16];
    fmt(q.ms, now);
    if ((int)li.state != lastState) {
      static const char *names[] = {"idle", "finding the line", "out lap", "timing"};
      std::printf("%s  state: %s\n", now, names[li.state]);
      lastState = (int)li.state;
    }
    if (li.laps != laps) {
      laps = li.laps;
      std::printf("%s  LAP %u: %.2f s   best %.2f s, its length (the reference) %.0f m\n", now, laps,
                  li.lastMs / 1000.0, li.bestMs / 1000.0, li.refM);
      nextReport = 0;
    }
    // Along a timed lap with a reference: how far the line is, every 10 s.
    if (li.state == LAP_TIMING && li.refM > 0.0f && replayMillis >= nextReport) {
      std::printf("%s    lap %u at %5.1f s: driven %6.0f m, %6.0f m to the line\n", now, laps + 1,
                  li.currentMs / 1000.0, li.lapM, calc_lap_left_m(li.refM, li.lapM));
      nextReport = replayMillis + 10000;
    }
  }
  std::printf("done: %u laps\n", laps);
  return 0;
}
