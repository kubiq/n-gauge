// ============================================================================
// test_loopfind.cpp  —  finding the start/finish line by loop closure.
//
// Synthetic circuits driven point by point: an oval of two 400 m straights
// joined by 80 m-radius hairpins (~1.3 km), a pit lane beside it, a figure-8,
// an out-and-back road, GNSS noise and a car standing still.
// ============================================================================
#include "test.h"
#include <vector>
#include "../firmware/car_gauge_v1/loopfind.h"

struct P { float x, y; };

// The oval, anticlockwise from the start of the lower straight (y = 0),
// one point per metre.
static std::vector<P> oval(int laps) {
  std::vector<P> out;
  const float R = 80.0f, L = 400.0f;
  for (int lap = 0; lap < laps; lap++) {
    for (int i = 0; i < (int)L; i++) out.push_back({(float)i, 0.0f});                 // straight east
    for (int i = 0; i < 251; i++) {                                                    // hairpin up
      const float a = -(float)M_PI / 2 + i / R;
      out.push_back({L + R * cosf(a), R + R * sinf(a)});
    }
    for (int i = 0; i < (int)L; i++) out.push_back({L - i, 2 * R});                     // straight west
    for (int i = 0; i < 251; i++) {                                                    // hairpin down
      const float a = (float)M_PI / 2 + i / R;
      out.push_back({R * cosf(a), R + R * sinf(a)});
    }
  }
  return out;
}

struct Run { int match = LF_NONE; size_t atPoint = 0; LfCrumb gate{}; float closedAt = 0; };

// 25 m/s: a point per metre is a fix every 40 ms.
static Run drive(const std::vector<P> &path, uint16_t cap = 4096) {
  static LfCrumb buf[4096];
  LoopFinder f;
  lf_init(&f, buf, cap);
  Run r;
  for (size_t i = 0; i < path.size(); i++) {
    const int m = lf_add(&f, path[i].x, path[i].y, (uint32_t)(i * 40));
    if (m >= 0 || m == LF_FULL) {
      r.match = m;
      r.atPoint = i;
      if (m >= 0) r.gate = f.crumbs[m];
      r.closedAt = f.dist;
      return r;
    }
  }
  return r;
}

static const float LAP_M = 800.0f + 2 * 251.0f;   // as the generator drives it

static void starting_on_the_line_closes_there_after_one_lap() {
  const Run r = drive(oval(2));
  CHECK(r.match >= 0);
  CHECK(fabsf(r.gate.x) < 20.0f && fabsf(r.gate.y) < 1.0f);   // back at the start
  CHECK(r.closedAt > LAP_M - 25.0f && r.closedAt < LAP_M + 25.0f);
  CHECK(r.gate.fixMs < 1000);                                  // first passed at the start
}

// From a pit lane 40 m below the lower straight, joining it at x = 200.
static void starting_from_the_pit_the_out_lap_is_left_out() {
  std::vector<P> path;
  for (int i = 0; i < 150; i++) path.push_back({(float)(50 + i), -40.0f});            // pit lane
  for (int i = 0; i <= 40; i++) path.push_back({200.0f + i * 0.5f, -40.0f + i});      // merging
  std::vector<P> track = oval(2);
  for (const P &p : track) if (!(p.y == 0.0f && p.x < 220.0f && path.size() < 400)) path.push_back(p);
  const Run r = drive(path);
  CHECK(r.match >= 0);
  CHECK(r.gate.y > -5.0f);                 // on the circuit, not in the pit lane
  CHECK(r.gate.fixMs > 150 * 40);          // first passed after leaving the pit
}

static void a_figure_8_crossover_is_not_a_loop() {
  // Two 300 m loops meeting at the origin. Leaving it east at 0 m; back there
  // at 300 m, east again - too short a loop (LF_MIN_LOOP_M); back at 600 m
  // arriving SOUTH - the heading test refuses it. Only ~10 m later, on the
  // first loop again in the same direction, does it close: a whole figure.
  std::vector<P> path;
  const float R = 300.0f / (2 * (float)M_PI);
  for (int lap = 0; lap < 3; lap++) {
    for (int i = 0; i < 300; i++) { const float a = i / R; path.push_back({R * sinf(a), R - R * cosf(a)}); }
    for (int i = 0; i < 300; i++) { const float a = i / R; path.push_back({R - R * cosf(a), -R * sinf(a)}); }
  }
  const Run r = drive(path);
  CHECK(r.match >= 0);
  CHECK(r.closedAt > 590.0f);              // a whole figure, not half of it
}

static void an_out_and_back_road_never_closes() {
  std::vector<P> path;
  for (int i = 0; i < 1000; i++) path.push_back({(float)i, 0.0f});
  for (int i = 0; i < 1000; i++) path.push_back({1000.0f - i, 3.0f});   // back, the other lane
  CHECK(drive(path).match == LF_NONE);
}

static void gnss_noise_still_closes_on_the_circuit() {
  std::vector<P> path = oval(2);
  uint32_t s = 12345;
  for (P &p : path) {
    s = s * 1103515245u + 12345u; p.x += ((s >> 16) % 600) / 100.0f - 3.0f;   // +-3 m
    s = s * 1103515245u + 12345u; p.y += ((s >> 16) % 600) / 100.0f - 3.0f;
  }
  const Run r = drive(path);
  CHECK(r.match >= 0);
  CHECK(r.closedAt > LAP_M - 40.0f && r.closedAt < LAP_M + 60.0f);
  CHECK(fabsf(r.gate.y) < 10.0f);
}

static void standing_still_drops_no_crumbs() {
  static LfCrumb buf[16];
  LoopFinder f;
  lf_init(&f, buf, 16);
  uint32_t s = 7;
  for (int i = 0; i < 5000; i++) {
    s = s * 1103515245u + 12345u;
    const float j = ((s >> 16) % 400) / 100.0f - 2.0f;   // +-2 m wander
    CHECK(lf_add(&f, j, -j, (uint32_t)(i * 40)) == LF_NONE);
  }
  CHECK(f.n == 1);
}

static void running_out_of_crumbs_gives_up() {
  std::vector<P> path;
  for (int i = 0; i < 2000; i++) path.push_back({(float)i, 0.0f});
  CHECK(drive(path, 50).match == LF_FULL);
}

int main() {
  printf("test_loopfind\n");
  TEST(starting_on_the_line_closes_there_after_one_lap);
  TEST(starting_from_the_pit_the_out_lap_is_left_out);
  TEST(a_figure_8_crossover_is_not_a_loop);
  TEST(an_out_and_back_road_never_closes);
  TEST(gnss_noise_still_closes_on_the_circuit);
  TEST(standing_still_drops_no_crumbs);
  TEST(running_out_of_crumbs_gives_up);
  TEST_SUMMARY();
}
