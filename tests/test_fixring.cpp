// ============================================================================
// test_fixring.cpp  —  every GNSS fix queued for the logs (fixring.h), and a
// queued fix stamped with its own time (calc_fix_iso). Each case is what a
// card stall would otherwise have cost: the fixes, or their times.
// ============================================================================
#include "test.h"
#include "../firmware/car_gauge_v1/fixring.h"
#include "../firmware/car_gauge_v1/calc.h"

static int store[8];
static FixRing<int, 8> ring;

static void reset() {
  ring.slots = store;
  ring.head = 0;
}

static void a_reader_gets_every_fix_after_it_started_in_order() {
  reset();
  fixring_push(&ring, 1);                     // before the reader: not its
  FixCursor c = {};
  int v = 0;
  uint32_t lost = 0;
  CHECK(!fixring_next(&ring, &c, &v, &lost)); // starts at the head
  for (int i = 2; i <= 6; i++) fixring_push(&ring, i);
  for (int want = 2; want <= 6; want++) {
    CHECK(fixring_next(&ring, &c, &v, &lost));
    CHECK_EQ(v, want);
  }
  CHECK(!fixring_next(&ring, &c, &v, &lost));
  CHECK_EQ(lost, 0);
}

// A card stall: the reader away while 30 fixes come - it gets them all late.
static void a_stall_shorter_than_the_ring_loses_nothing() {
  reset();
  FixCursor c = {};
  int v = 0;
  uint32_t lost = 0;
  fixring_next(&ring, &c, &v, &lost);
  for (int i = 0; i < 7; i++) fixring_push(&ring, 100 + i);   // N - 1 behind: all whole
  for (int i = 0; i < 7; i++) {
    CHECK(fixring_next(&ring, &c, &v, &lost));
    CHECK_EQ(v, 100 + i);
  }
  CHECK_EQ(lost, 0);
}

// Longer than the ring: the oldest whole fixes on, the rest counted.
static void a_stall_longer_than_the_ring_counts_what_it_lost() {
  reset();
  FixCursor c = {};
  int v = 0;
  uint32_t lost = 0;
  fixring_next(&ring, &c, &v, &lost);
  for (int i = 0; i < 20; i++) fixring_push(&ring, i);
  CHECK(fixring_next(&ring, &c, &v, &lost));
  CHECK_EQ(v, 13);                    // the last N - 1 = 7: 13..19
  CHECK_EQ(lost, 13);
  int n = 1;
  while (fixring_next(&ring, &c, &v, &lost)) n++;
  CHECK_EQ(n, 7);
  CHECK_EQ(v, 19);
}

// The GPX and the .vbo: each its own place, neither takes from the other.
static void two_readers_each_get_all_of_them() {
  reset();
  FixCursor gpx = {}, vbo = {};
  int v = 0;
  uint32_t lost = 0;
  fixring_next(&ring, &gpx, &v, &lost);
  fixring_next(&ring, &vbo, &v, &lost);
  for (int i = 0; i < 5; i++) fixring_push(&ring, i);
  int a = 0, b = 0;
  CHECK(fixring_next(&ring, &gpx, &v, &lost));
  a++;
  while (fixring_next(&ring, &vbo, &v, &lost)) b++;
  while (fixring_next(&ring, &gpx, &v, &lost)) a++;
  CHECK_EQ(a, 5);
  CHECK_EQ(b, 5);
  CHECK_EQ(lost, 0);
}

static void the_counter_wrapping_changes_nothing() {
  reset();
  ring.head = 0xFFFFFFFCu;
  FixCursor c = {};
  int v = 0;
  uint32_t lost = 0;
  fixring_next(&ring, &c, &v, &lost);
  for (int i = 0; i < 6; i++) fixring_push(&ring, i);   // head wraps past 0
  for (int i = 0; i < 6; i++) {
    CHECK(fixring_next(&ring, &c, &v, &lost));
    CHECK_EQ(v, i);
  }
  CHECK_EQ(lost, 0);
}

// ---- calc_fix_iso -------------------------------------------------------------
static const long long DAY = calc_timegm(2026, 10, 26, 0, 0, 0);

static void a_fix_is_stamped_with_its_own_time_of_day() {
  char s[32];
  // Written 1.3 s late: the fix's time, not the clock's.
  calc_fix_iso(s, sizeof(s), DAY + 17 * 3600 + 5 * 60 + 6, (17 * 3600 + 5 * 60 + 4) * 1000 + 360);
  CHECK_STREQ(s, "2026-10-26T17:05:04.36Z");
}

static void around_midnight_the_fix_keeps_its_own_day() {
  char s[32];
  // Written just past midnight, the fix from 23:59:59.96 the day before.
  calc_fix_iso(s, sizeof(s), DAY + 1, 86399 * 1000 + 960);
  CHECK_STREQ(s, "2026-10-25T23:59:59.96Z");
  // The clock a touch behind: still 23:59:59, the fix already 00:00:00.20.
  calc_fix_iso(s, sizeof(s), DAY - 1, 200);
  CHECK_STREQ(s, "2026-10-26T00:00:00.20Z");
}

static void civil_dates_round_trip() {
  // 1970-01-01, 2000-01-01, 2026-10-26, 2038-01-19, 1969-12-31
  const long long days[] = {0, 10957, DAY / 86400, 24855, -1};
  for (long long d : days) {
    int y;
    unsigned m, dd;
    calc_civil_from_days(d, &y, &m, &dd);
    CHECK_EQ(calc_days_from_civil(y, m, dd), d);
  }
}

int main() {
  printf("test_fixring\n");
  TEST(a_reader_gets_every_fix_after_it_started_in_order);
  TEST(a_stall_shorter_than_the_ring_loses_nothing);
  TEST(a_stall_longer_than_the_ring_counts_what_it_lost);
  TEST(two_readers_each_get_all_of_them);
  TEST(the_counter_wrapping_changes_nothing);
  TEST(a_fix_is_stamped_with_its_own_time_of_day);
  TEST(around_midnight_the_fix_keeps_its_own_day);
  TEST(civil_dates_round_trip);
  TEST_SUMMARY();
}
