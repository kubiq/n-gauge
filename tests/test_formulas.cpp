// ============================================================================
// test_formulas.cpp  —  the sensor conversions in config.h.
//
// These are the numbers the whole gauge exists to show, and every one of them
// was reverse-engineered from logged CAN payloads rather than documented. That
// makes them exactly the thing a refactor can silently break: a wrong offset
// or a dropped cast still compiles and still displays a plausible temperature.
// Each case below is anchored to a value actually observed on the car, cited
// in the comment, so a future edit has to disagree with recorded reality
// rather than with a number someone invented.
// ============================================================================
#include "test.h"
#include "../firmware/car_gauge_v1/config.h"

static void oil_mode22_matches_observed_values() {
  // From the DID 0xE001 hunt: raw 0xDB (219) was logged at ~114 C, and the
  // validated band was 48-94 C. Build a payload and index it exactly as
  // obd.cpp does, so a change to OIL_MODE22_OFFSET is caught here too.
  unsigned char d[48] = { 0 };

  d[OIL_MODE22_OFFSET] = 131;                 // 131 * 0.7435 - 49.2 = 48.2
  CHECK_EQ(OIL_TEMP_FROM_MODE22(d), 48);

  d[OIL_MODE22_OFFSET] = 193;                 // -> 94.3
  CHECK_EQ(OIL_TEMP_FROM_MODE22(d), 94);

  // Offset must still be inside the payload we bounds-check against.
  CHECK(OIL_MODE22_OFFSET < (int)sizeof(d));
}

// Pins the OFFSET, not the formula - the offset is what was actually wrong,
// and for weeks. The formula tests above index via OIL_MODE22_OFFSET, so they
// follow any change to it silently and cannot catch this class of mistake.
//
// This is one real payload from drive_0181.csv, captured at the instant coolant
// read 70 C during a cold-start warm-up. At that moment the instrument cluster
// showed ~50 C. Warm-up is the one region where the two candidate bytes differ
// by more than 20 C, which is why this sample was chosen: byte 25 is
// coolant-derived and byte 34 is the oil sensor, and here they read 68 and 46.
//
// If someone "fixes" the offset back to 25, this fails with 68 against the 46
// that the car's own dashboard corroborates.
static void oil_mode22_offset_is_the_oil_byte_not_coolant() {
  const unsigned char payload[42] = {
    0x9F,0xB7,0x3F,0x00,0xD2,0xCE,0x4E,0x98,0x16,0x55,0x00,0x00,0x00,0x00,
    0x73,0x00,0xE0,0x24,0x40,0x20,0x08,0x00,0x00,0x95,0x00,0x9E,0xFF,0x03,
    0x55,0x34,0x03,0xD9,0x01,0x5E,0x80,0x45,0xFF,0x7F,0x00,0x00,0x00,0x00,
  };

  // Byte 34 = 0x80 = 128 -> 45.97, truncated to 45 by the (int) cast. Lags the
  // 70 C coolant by minutes, which is what oil does during warm-up, and
  // matches the cluster's ~50 C.
  //
  // Note the cast TRUNCATES rather than rounds, so every reading sits up to
  // 1 C low - and ~0.5 C low on average. Left alone deliberately: the formula
  // was fitted against readings produced this same way, so rounding now would
  // shift the whole curve relative to what validated it.
  CHECK_EQ(OIL_TEMP_FROM_MODE22(payload), 45);

  // Byte 25 = 0x9E = 158 -> 68 C, i.e. coolant. This is the value the gauge
  // wrongly showed while the dashboard read ~50 C - the original bug report.
  CHECK_EQ((int)(payload[25] * 0.7435f - 49.2f), 68);

  // Both bytes share the 101-193 raw range, i.e. the same encoding - which is
  // why the formula did not need refitting when the offset moved.
  CHECK(payload[25] >= 101 && payload[25] <= 193);
  CHECK(payload[34] >= 101 && payload[34] <= 193);

  // A 42-byte payload must actually reach the byte we index.
  CHECK(OIL_MODE22_OFFSET < (int)sizeof(payload));
}

static void oil_mode22_is_monotonic_and_sane() {
  unsigned char d[48] = { 0 };
  int prev = -1000;
  for (int raw = 0; raw <= 255; raw++) {
    d[OIL_MODE22_OFFSET] = (unsigned char)raw;
    const int t = OIL_TEMP_FROM_MODE22(d);
    CHECK(t >= prev);                          // never goes backwards
    prev = t;
  }
  // Full-scale sanity: the sensor cannot report colder than about -50 C or
  // hotter than about 150 C. A formula that exceeds this is wrong.
  d[OIL_MODE22_OFFSET] = 0;
  CHECK(OIL_TEMP_FROM_MODE22(d) > -60 && OIL_TEMP_FROM_MODE22(d) < 0);
  d[OIL_MODE22_OFFSET] = 255;
  CHECK(OIL_TEMP_FROM_MODE22(d) > 100 && OIL_TEMP_FROM_MODE22(d) < 160);
}

static void tpms_pressure_matches_the_dashboard() {
  // The layout was confirmed against the dashboard on 2026-09-20: raw values
  // 191/192/178/176 read as 2.63/2.65/2.45/2.43 bar, front 2.6 and rear 2.4.
  CHECK_NEAR(TPMS_BAR_FROM_RAW(191), 2.634f, 0.01f);
  CHECK_NEAR(TPMS_BAR_FROM_RAW(192), 2.648f, 0.01f);
  CHECK_NEAR(TPMS_BAR_FROM_RAW(178), 2.455f, 0.01f);
  CHECK_NEAR(TPMS_BAR_FROM_RAW(176), 2.427f, 0.01f);
  CHECK_NEAR(TPMS_BAR_FROM_RAW(0), 0.0f, 0.001f);
}

static void tpms_temperature_matches_the_same_drive() {
  // Same payload: 84/86/81/81 -> 34/36/31/31 C. The fronts reading hotter is
  // what corroborated the wheel ordering on a front-wheel-drive car.
  CHECK_EQ(TPMS_TEMP_FROM_RAW(84), 34);
  CHECK_EQ(TPMS_TEMP_FROM_RAW(86), 36);
  CHECK_EQ(TPMS_TEMP_FROM_RAW(81), 31);
  CHECK_EQ(TPMS_TEMP_FROM_RAW(50), 0);
  CHECK_EQ(TPMS_TEMP_FROM_RAW(0), -50);        // must stay signed
}

static void tpms_offsets_have_stride_four_and_fit() {
  // Solved the hard way: the first attempt assumed a stride of 5 and logged
  // decoded values at the wrong offsets, which cost a drive because the raw
  // payload had not been kept. The real payload is 20 bytes: FF FF 00 00 of
  // flags, then four bytes per wheel.
  // The payload's wheel order is FL, FR, RR, RL (rears checked against the
  // dashboard 2026-10-04), while the screen's slots are FL, FR, RL, RR - so
  // every slot of 4 + k*4 is used exactly once, rears swapped.
  const unsigned short off[] = TPMS_PRESSURE_OFFSET;
  const int n = (int)(sizeof(off) / sizeof(off[0]));
  CHECK_EQ(n, TPMS_WHEELS);
  int used = 0;
  for (int i = 0; i < n; i++) {
    CHECK((off[i] - 4) % 4 == 0 && off[i] >= 4);
    CHECK(off[i] + 1 < 20);                    // temperature byte also inside
    used |= 1 << ((off[i] - 4) / 4);
  }
  CHECK_EQ(used, 0xF);
  CHECK_EQ(off[2], 16);                        // screen RL = payload slot 3
  CHECK_EQ(off[3], 12);                        // screen RR = payload slot 2
}

static void torque_estimate_hits_rated_torque_at_peak_load() {
  // Calibrated 2026-10-04: full-throttle pulls peak at ~230 % absolute load,
  // which must map to the 2021 i30 N's rated 392 Nm; no load, no torque.
  CHECK_NEAR(TORQUE_NM_FROM_LOAD(230.0f), 392.0f, 0.5f);
  CHECK_NEAR(TORQUE_NM_FROM_LOAD(0.0f), 0.0f, 0.01f);
  CHECK(TORQUE_NM_FROM_LOAD(232.5f) <= TORQUE_ARC_MAX_NM);   // logged max fits the arc
}

static void boost_is_gauge_pressure_not_absolute() {
  // MAP is absolute; the gauge shows boost relative to ambient, so atmospheric
  // pressure must read as zero and vacuum must go negative.
  CHECK_NEAR(BOOST_BAR_FROM_KPA(AMBIENT_KPA, AMBIENT_KPA), 0.0f, 0.001f);
  CHECK_NEAR(BOOST_BAR_FROM_KPA(AMBIENT_KPA + 100, AMBIENT_KPA), 1.0f, 0.001f);
  CHECK(BOOST_BAR_FROM_KPA(40, AMBIENT_KPA) < 0.0f);        // idle vacuum
  // Zeroed against the measured baro, not a constant: at 98 kPa ambient
  // (measured 2026-10-04) a 198 kPa manifold is exactly 1 bar of boost.
  CHECK_NEAR(BOOST_BAR_FROM_KPA(198, 98), 1.0f, 0.001f);
}

static void thresholds_are_ordered_sensibly() {
  // Cheap guards against a typo in config.h that would be invisible on screen.
  CHECK(OIL_READY_C < OIL_WARN_C);
  CHECK(TPMS_LOW_BAR_FRONT < TPMS_HIGH_BAR_FRONT);
  CHECK(TPMS_LOW_BAR_REAR < TPMS_HIGH_BAR_REAR);
  // The manual's cold spec (front 2.7, rear 2.4) must sit inside its own
  // axle's band, or a correctly inflated tyre shows red.
  CHECK(TPMS_LOW_BAR_FRONT < 2.7f && 2.7f < TPMS_HIGH_BAR_FRONT);
  CHECK(TPMS_LOW_BAR_REAR < 2.4f && 2.4f < TPMS_HIGH_BAR_REAR);
  CHECK(STATE_HYST_C > 0);
  // Hysteresis must be smaller than the gap between the states it separates,
  // or a state could never be left.
  CHECK(STATE_HYST_C < OIL_WARN_C - OIL_READY_C);
  // A GPX segment must not be split more often than idle points are written,
  // or every parked point starts a new segment. This caught a real bug when
  // the gap was 4 s and idle spacing was 5 s.
  CHECK(GPX_SEGMENT_GAP_MS > GPX_IDLE_POINT_MS);
  // The stale window has to be longer than the poll interval, or a reading is
  // declared stale before its replacement can arrive.
  CHECK(TPMS_STALE_MS > TPMS_POLL_MS);
  CHECK(OBD_STALE_MS > OBD_REQ_TIMEOUT_MS);
}

int main() {
  printf("test_formulas\n");
  TEST(oil_mode22_matches_observed_values);
  TEST(oil_mode22_offset_is_the_oil_byte_not_coolant);
  TEST(oil_mode22_is_monotonic_and_sane);
  TEST(tpms_pressure_matches_the_dashboard);
  TEST(tpms_temperature_matches_the_same_drive);
  TEST(tpms_offsets_have_stride_four_and_fit);
  TEST(boost_is_gauge_pressure_not_absolute);
  TEST(torque_estimate_hits_rated_torque_at_peak_load);
  TEST(thresholds_are_ordered_sensibly);
  TEST_SUMMARY();
}
