// ============================================================================
// test_gpx_gate.cpp  —  which GNSS fixes become track points and trip
// distance (calc_gpx_gate in calc.h), with the firmware's own thresholds.
// Each case is a way it went wrong on a real drive.
// ============================================================================
#include "test.h"
#include "../firmware/car_gauge_v1/calc.h"
#include "../firmware/car_gauge_v1/config.h"

static const CalcGpxLimits LIM = {
  GPX_POINT_MS, GPX_IDLE_POINT_MS, GPS_MIN_SATS_LOG, GPS_MAX_HDOP_LOG,
  GPX_SEGMENT_GAP_MS, GPX_MAX_JUMP_KMH, GPX_MAX_REJECT_RUN,
  LOG_IDLE_SPEED_KMH, 30u * 60000u,
};

static const int32_t LAT0 = 500000000, LON0 = 144000000;   // 50 N, 14.4 E

// A car driving north at `kmh` from LAT0: its position at time t.
static void at(float kmh, uint32_t tMs, int32_t *lat, int32_t *lon) {
  calc_offset_ll(LAT0, LON0, 0.0f, kmh / 3.6f * tMs / 1000.0f, lat, lon);
}

// Good reception: 12 satellites, HDOP 0.8.
static bool step(CalcGpxGate &g, uint32_t now, int32_t lat, int32_t lon, float kmh,
                 CalcGpxStep *out, bool idle = false, int sats = 12, float hdop = 0.8f) {
  return calc_gpx_gate(&g, &LIM, now, idle, sats, hdop, lat, lon, kmh, out);
}

// The 8x bug: asked every 5 ms while the receiver moves the position every
// 40 ms, at motorway speed. Every 40 ms point must pass, none be rejected as
// a jump, and the distance must be the distance driven.
static void loop_rate_calls_do_not_inflate_the_jump_check() {
  CalcGpxGate g;
  CalcGpxStep s;
  int accepted = 0;
  float metres = 0;
  for (uint32_t t = 1000; t <= 61000; t += 5) {
    const uint32_t fixT = t - t % GPS_NAV_RATE_MS;          // the receiver's epoch
    int32_t lat, lon;
    at(130.0f, fixT, &lat, &lon);
    if (step(g, t, lat, lon, 130.0f, &s)) { accepted++; metres += s.addM; }
  }
  CHECK_EQ(g.rejectRun, 0);
  CHECK(accepted >= 60000 / GPX_POINT_MS - 1);
  CHECK_NEAR(metres, 130.0f / 3.6f * 60.0f, 15.0f);          // ~2167 m in 60 s
}

static void parked_points_are_thinned() {
  CalcGpxGate g;
  CalcGpxStep s;
  CHECK(step(g, 1000, LAT0, LON0, 0, &s, true));
  CHECK(!step(g, 1000 + GPX_IDLE_POINT_MS - 1, LAT0, LON0, 0, &s, true));
  CHECK(step(g, 1000 + GPX_IDLE_POINT_MS, LAT0, LON0, 0, &s, true));
}

// A frozen fix after reception was lost: 0 satellites, HDOP 99.99.
static void a_fix_without_satellites_is_refused_once_sats_were_seen() {
  CalcGpxGate g;
  CalcGpxStep s;
  CHECK(step(g, 1000, LAT0, LON0, 50, &s));
  CHECK(!step(g, 1040, LAT0, LON0, 50, &s, false, 0, 99.99f));
  CHECK(!step(g, 1080, LAT0, LON0, 50, &s, false, GPS_MIN_SATS_LOG - 1, 1.0f));
  CHECK(!step(g, 1120, LAT0, LON0, 50, &s, false, 12, GPS_MAX_HDOP_LOG + 1));
}

// A module that never reports satellites must still log.
static void a_module_without_satellite_counts_still_logs() {
  CalcGpxGate g;
  CalcGpxStep s;
  CHECK(step(g, 1000, LAT0, LON0, 50, &s, false, 0, 99.99f));
  CHECK(step(g, 1040, LAT0, LON0, 50, &s, false, 0, 99.99f));
}

// Out of a tunnel: a spike kilometres away is refused, the drive goes on
// from the last good point, and nothing is added to the trip.
static void a_tunnel_spike_is_refused_and_adds_no_distance() {
  CalcGpxGate g;
  CalcGpxStep s;
  int32_t lat, lon;
  at(80, 0, &lat, &lon);
  CHECK(step(g, 1000, lat, lon, 80, &s));
  calc_offset_ll(LAT0, LON0, 3000.0f, 0.0f, &lat, &lon);     // 3 km east in 40 ms
  CHECK(!step(g, 1040, lat, lon, 80, &s));
  CHECK_EQ(g.rejectRun, 1);
  at(80, 80, &lat, &lon);
  CHECK(step(g, 1080, lat, lon, 80, &s));
  CHECK(s.addM < 3.0f);                                        // 80 km/h x 80 ms
  CHECK_EQ(g.rejectRun, 0);
}

// If the reference itself was the bad point, refusing forever would lose the
// rest of the trip: after GPX_MAX_REJECT_RUN refusals, accept.
static void a_bad_reference_is_let_go_eventually() {
  CalcGpxGate g;
  CalcGpxStep s;
  CHECK(step(g, 1000, LAT0, LON0, 50, &s));                  // the bad one
  int32_t lat, lon;
  calc_offset_ll(LAT0, LON0, 5000.0f, 0.0f, &lat, &lon);     // where the car really is
  uint32_t t = 1000;
  for (int i = 0; i < GPX_MAX_REJECT_RUN; i++) CHECK(!step(g, t += 40, lat, lon, 50, &s));
  CHECK(step(g, t += 40, lat, lon, 50, &s));
}

// After a real gap the point is accepted whatever the distance, the track
// breaks there, and the straight line across the gap is not trip distance.
static void a_gap_breaks_the_track_and_is_not_distance() {
  CalcGpxGate g;
  CalcGpxStep s;
  CHECK(step(g, 1000, LAT0, LON0, 80, &s));
  CHECK(!s.gap);
  int32_t lat, lon;
  calc_offset_ll(LAT0, LON0, 0.0f, 900.0f, &lat, &lon);
  CHECK(step(g, 1000 + GPX_SEGMENT_GAP_MS + 1, lat, lon, 80, &s));
  CHECK(s.gap);
  CHECK_EQ(s.addM, 0);
}

// Standing at lights the position wanders; none of it is distance.
static void standing_still_adds_no_distance() {
  CalcGpxGate g;
  CalcGpxStep s;
  float metres = 0;
  for (int i = 0; i < 100; i++) {
    int32_t lat, lon;
    calc_offset_ll(LAT0, LON0, (i % 3) * 1.5f, (i % 2) * 1.5f, &lat, &lon);   // ~2 m of wander
    if (step(g, 1000 + i * GPX_POINT_MS, lat, lon, LOG_IDLE_SPEED_KMH - 1, &s)) metres += s.addM;
  }
  CHECK_EQ(metres, 0);
}

// "Same trip" from millis when the previous point is recent - the wall clock
// can jump, millis cannot.
static void recent_says_whether_the_trip_continues() {
  CalcGpxGate g;
  CalcGpxStep s;
  CHECK(step(g, 1000, LAT0, LON0, 0, &s));
  CHECK(!s.recent);                                            // the first point
  CHECK(step(g, 1040, LAT0, LON0, 0, &s));
  CHECK(s.recent);
  CHECK(step(g, 1040 + LIM.tripGapMs, LAT0, LON0, 0, &s));
  CHECK(!s.recent);
}

int main() {
  printf("test_gpx_gate\n");
  TEST(loop_rate_calls_do_not_inflate_the_jump_check);
  TEST(parked_points_are_thinned);
  TEST(a_fix_without_satellites_is_refused_once_sats_were_seen);
  TEST(a_module_without_satellite_counts_still_logs);
  TEST(a_tunnel_spike_is_refused_and_adds_no_distance);
  TEST(a_bad_reference_is_let_go_eventually);
  TEST(a_gap_breaks_the_track_and_is_not_distance);
  TEST(standing_still_adds_no_distance);
  TEST(recent_says_whether_the_trip_continues);
  TEST_SUMMARY();
}
