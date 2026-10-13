// ============================================================================
// test_calc.cpp  —  the pure logic in calc.h.
//
// This is the same code the board runs, not a reimplementation: calc.h is
// included by both. Cases are anchored to values measured on the car or to
// properties that must hold by construction (a projection that removes
// gravity must read zero at rest, whatever the mounting angle).
// ============================================================================
#include "test.h"
#include <cstdlib>      // setenv, for the timezone-independence test
#include <ctime>        // tzset
#include "../firmware/car_gauge_v1/calc.h"
// Included so the thresholds under test are the ones the firmware actually
// uses - a test that carries its own copy of a constant stops being a test of
// the shipped behaviour the moment someone tunes config.h.
#include "../firmware/car_gauge_v1/config.h"

// ---- timezone ---------------------------------------------------------------

static void tz_inverts_the_sign_like_posix_requires() {
  char b[40];
  // The one that matters: Czechia, UTC+1 with EU DST. Verified on the device
  // to convert 22:13:20 UTC into 23:13:20 local.
  calc_build_tz(b, sizeof(b), 1, true);
  CHECK_STREQ(b, "<+01>-1<+02>,M3.5.0,M10.5.0/3");

  calc_build_tz(b, sizeof(b), 1, false);
  CHECK_STREQ(b, "<+01>-1");
}

static void tz_handles_negative_and_zero_offsets() {
  char b[40];
  calc_build_tz(b, sizeof(b), 0, false);
  CHECK_STREQ(b, "<+00>0");
  calc_build_tz(b, sizeof(b), -5, false);      // New York standard time
  CHECK_STREQ(b, "<-05>5");
  calc_build_tz(b, sizeof(b), -5, true);
  CHECK_STREQ(b, "<-05>5<-04>,M3.5.0,M10.5.0/3");
}

static void tz_fits_the_settings_field_at_every_offset() {
  // settings.tz is char[40]; truncation would produce a silently wrong zone
  // rather than an error, so check the whole legal range fits.
  char b[40];
  for (int o = -12; o <= 14; o++) {
    calc_build_tz(b, sizeof(b), o, true);
    CHECK(strlen(b) < sizeof(b) - 1);
  }
}

static void tz_is_safe_with_a_degenerate_buffer() {
  calc_build_tz(nullptr, 10, 1, true);         // must not crash
  char b[1] = { 'x' };
  calc_build_tz(b, 0, 1, true);
  CHECK_EQ(b[0], 'x');                         // untouched when n == 0
}

static void tz_clamps_absurd_offsets_rather_than_truncating() {
  // An out-of-range offset would need up to 46 bytes in a 40-byte field, and a
  // truncated TZ string is a silently wrong clock. Clamp, don't truncate.
  char b[40], ref[40];
  calc_build_tz(ref, sizeof(ref), 14, true);
  calc_build_tz(b, sizeof(b), 9999, true);
  CHECK_STREQ(b, ref);
  calc_build_tz(ref, sizeof(ref), -12, true);
  calc_build_tz(b, sizeof(b), -9999, true);
  CHECK_STREQ(b, ref);
}

// ---- text tidying -----------------------------------------------------------

static void trim_strips_the_space_a_phone_keyboard_adds() {
  char b[33];
  CHECK_EQ(calc_trim_copy(b, sizeof(b), "n-gauge "), 7);
  CHECK_STREQ(b, "n-gauge");
  CHECK_EQ(calc_trim_copy(b, sizeof(b), "  n-gauge"), 7);
  CHECK_STREQ(b, "n-gauge");
  CHECK_EQ(calc_trim_copy(b, sizeof(b), "\t n-gauge \r\n"), 7);
  CHECK_STREQ(b, "n-gauge");
}

static void trim_keeps_inner_spaces() {
  // "my car" is a perfectly good network name; only the ends are noise.
  char b[33];
  calc_trim_copy(b, sizeof(b), "  my  car  ");
  CHECK_STREQ(b, "my  car");
}

static void trim_handles_empty_and_all_space() {
  char b[33];
  CHECK_EQ(calc_trim_copy(b, sizeof(b), ""), 0);
  CHECK_STREQ(b, "");
  CHECK_EQ(calc_trim_copy(b, sizeof(b), "     "), 0);
  CHECK_STREQ(b, "");          // caller rejects this, so it must be detectable
  CHECK_EQ(calc_trim_copy(b, sizeof(b), nullptr), 0);
  CHECK_STREQ(b, "");
}

static void trim_never_overruns_the_destination() {
  char b[8];
  const size_t n = calc_trim_copy(b, sizeof(b), "  abcdefghijklmnop  ");
  CHECK_EQ(n, 7);              // 7 chars plus the NUL
  CHECK_STREQ(b, "abcdefg");
  char one[1] = { 'x' };
  CHECK_EQ(calc_trim_copy(one, sizeof(one), "abc"), 0);
  CHECK_EQ(one[0], '\0');
  CHECK_EQ(calc_trim_copy(nullptr, 10, "abc"), 0);   // must not crash
}

// ---- UTC epoch --------------------------------------------------------------

static void timegm_matches_known_epochs() {
  CHECK_EQ(calc_timegm(1970, 1, 1, 0, 0, 0), 0);
  CHECK_EQ(calc_timegm(2000, 1, 1, 0, 0, 0), 946684800LL);
  CHECK_EQ(calc_timegm(2038, 1, 19, 3, 14, 7), 2147483647LL);  // 32-bit limit
  // The value verified on the device for the timezone work.
  CHECK_EQ(calc_timegm(2023, 11, 14, 22, 13, 20), 1700000000LL);
  // A timestamp from the actual drive log (cross-checked against Python's
  // datetime, not computed by hand - the hand-written value was 5 days out).
  CHECK_EQ(calc_timegm(2026, 9, 22, 16, 9, 59), 1790093399LL);
}

static void timegm_handles_leap_years_and_month_ends() {
  // 2024 is a leap year, 2100 is not, 2000 is (the century rule).
  CHECK_EQ(calc_timegm(2024, 3, 1, 0, 0, 0) - calc_timegm(2024, 2, 28, 0, 0, 0),
           2 * 86400LL);                       // 29 Feb exists
  CHECK_EQ(calc_timegm(2023, 3, 1, 0, 0, 0) - calc_timegm(2023, 2, 28, 0, 0, 0),
           1 * 86400LL);                       // it does not
  CHECK_EQ(calc_timegm(2000, 3, 1, 0, 0, 0) - calc_timegm(2000, 2, 28, 0, 0, 0),
           2 * 86400LL);
  // Every month boundary advances by that month's real length.
  const int len[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
  for (int m = 1; m <= 11; m++) {
    CHECK_EQ(calc_timegm(2026, m + 1, 1, 0, 0, 0) - calc_timegm(2026, m, 1, 0, 0, 0),
             (long long)len[m - 1] * 86400LL);
  }
}

static void timegm_ignores_the_local_timezone() {
  // The whole point: mktime() would shift by the TZ offset, which is how every
  // log timestamp ended up 1-2 hours out. Setting TZ must change nothing here.
  const long long before = calc_timegm(2026, 9, 22, 16, 9, 59);
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  CHECK_EQ(calc_timegm(2026, 9, 22, 16, 9, 59), before);
  setenv("TZ", "<+14>-14", 1);
  tzset();
  CHECK_EQ(calc_timegm(2026, 9, 22, 16, 9, 59), before);
}

// ---- GPS plausibility -------------------------------------------------------

// calc_implied_kmh takes degrees x 1e7, the form GpsState stores. Spelling the
// cases in degrees keeps them readable and checkable against the comments; the
// conversion is the test's business, not the function's.
static inline int32_t DEG(double d) { return (int32_t)lround(d * 1e7); }

static void implied_speed_matches_a_known_distance() {
  // 0.001 degrees of latitude is ~110.54 m. Covered in 1 s that is ~398 km/h.
  CHECK_NEAR(calc_implied_kmh(DEG(50.0), DEG(14.0), DEG(50.001), DEG(14.0), 1.0f), 397.9f, 1.0f);
  // Same displacement over 10 s is a tenth of the speed.
  CHECK_NEAR(calc_implied_kmh(DEG(50.0), DEG(14.0), DEG(50.001), DEG(14.0), 10.0f), 39.8f, 0.5f);
  // Not moving is zero, not NaN.
  CHECK_NEAR(calc_implied_kmh(DEG(50.0), DEG(14.0), DEG(50.0), DEG(14.0), 0.04f), 0.0f, 0.001f);
}

static void fmt_deg_writes_exact_decimals() {
  char b[16];
  calc_fmt_deg(b, sizeof(b), 500012340);
  CHECK_STREQ(b, "50.0012340");
  // The fraction must be zero-padded to 7 digits. Without the %07 the value
  // below would print as "50.123" - a hundred-fold error in position, and one
  // that looks entirely plausible in a GPX file.
  calc_fmt_deg(b, sizeof(b), 500000123);
  CHECK_STREQ(b, "50.0000123");
  calc_fmt_deg(b, sizeof(b), 140000000);
  CHECK_STREQ(b, "14.0000000");
  calc_fmt_deg(b, sizeof(b), 0);
  CHECK_STREQ(b, "0.0000000");
}

static void fmt_deg_handles_the_southern_and_western_hemispheres() {
  char b[16];
  calc_fmt_deg(b, sizeof(b), -337654321);
  CHECK_STREQ(b, "-33.7654321");
  // Sign must survive a whole-degree part of zero, which is where a naive
  // "print the integer part" loses it: -0.5 degrees is off west Africa, not
  // on the meridian.
  calc_fmt_deg(b, sizeof(b), -5000000);
  CHECK_STREQ(b, "-0.5000000");
  // INT32_MIN negated overflows a 32-bit int, which is why the helper widens
  // to 64-bit first. Not a real coordinate, but it must not be undefined.
  calc_fmt_deg(b, sizeof(b), -2147483647);
  CHECK_STREQ(b, "-214.7483647");
}

static void implied_speed_refuses_to_divide_by_zero() {
  // Two points sharing a timestamp used to be possible before the GPX
  // timestamps gained centiseconds; a divide here would have produced inf.
  CHECK_NEAR(calc_implied_kmh(DEG(50.0), DEG(14.0), DEG(50.1), DEG(14.1), 0.0f), 0.0f, 0.0001f);
  CHECK_NEAR(calc_implied_kmh(DEG(50.0), DEG(14.0), DEG(50.1), DEG(14.1), -1.0f), 0.0f, 0.0001f);
}

static void implied_speed_accounts_for_longitude_convergence() {
  // A degree of longitude shrinks with latitude. Ignoring the cosine term
  // would overstate east-west speed by ~35 % at 50 N and wrongly reject real
  // driving as implausible.
  const float ns = calc_implied_kmh(DEG(50.0), DEG(14.0), DEG(50.0 + 0.001), DEG(14.0), 1.0f);
  const float ew = calc_implied_kmh(DEG(50.0), DEG(14.0), DEG(50.0), DEG(14.0 + 0.001), 1.0f);
  CHECK(ew < ns);
  CHECK_NEAR(ew / ns, 0.647f, 0.02f);          // cos(50 deg) * 111320/110540
}

static void real_tunnel_garbage_is_rejected_and_real_driving_is_not() {
  // Taken from trip_0004: a 120.5 m jump in 0.28 s, i.e. ~1549 km/h.
  CHECK(calc_implied_kmh(DEG(50.0), DEG(14.0), DEG(50.0 + 120.5/110540.0), DEG(14.0), 0.28f)
        > GPX_MAX_JUMP_KMH);
  // And a genuine 130 km/h on the motorway must survive: 36 m/s over 0.05 s.
  CHECK(calc_implied_kmh(DEG(50.0), DEG(14.0), DEG(50.0 + (36.1*0.05)/110540.0), DEG(14.0), 0.05f)
        < GPX_MAX_JUMP_KMH);
}

// ---- lap timing -------------------------------------------------------------

static void local_xy_matches_known_offsets() {
  float x, y;
  // 0.001 deg of latitude is ~110.54 m north; longitude does not move.
  calc_local_xy(DEG(50.0), DEG(14.0), DEG(50.001), DEG(14.0), &x, &y);
  CHECK_NEAR(x, 0.0f, 0.01f);
  CHECK_NEAR(y, 110.54f, 0.05f);
  // 0.001 deg of longitude at 50 N is shorter by cos(50): ~71.56 m east.
  calc_local_xy(DEG(50.0), DEG(14.0), DEG(50.0), DEG(14.001), &x, &y);
  CHECK_NEAR(x, 71.56f, 0.05f);
  CHECK_NEAR(y, 0.0f, 0.01f);
}

static void gate_crossing_forwards_is_found_and_interpolated() {
  // Travelling north (d = 0,1), gate on the x axis. From 1 m before the line
  // to 3 m past it: the crossing is a quarter of the way along the step.
  CHECK_NEAR(calc_gate_cross(-1.0f, 0.0f, 3.0f, 0.0f, 25.0f), 0.25f, 0.0001f);
  // Landing exactly on the line counts, at the very end of the step.
  CHECK_NEAR(calc_gate_cross(-2.0f, 0.0f, 0.0f, 0.0f, 25.0f), 1.0f, 0.0001f);
}

static void gate_crossing_backwards_never_counts() {
  // Driving the wrong way over the line - or a return straight passing close
  // to it - must not produce a lap.
  CHECK_NEAR(calc_gate_cross(3.0f, 0.0f, -1.0f, 0.0f, 25.0f), -1.0f, 0.0001f);
  // Neither does a step that stays on one side.
  CHECK_NEAR(calc_gate_cross(-5.0f, 0.0f, -1.0f, 0.0f, 25.0f), -1.0f, 0.0001f);
  CHECK_NEAR(calc_gate_cross(1.0f, 0.0f, 4.0f, 0.0f, 25.0f), -1.0f, 0.0001f);
}

static void gate_crossing_wide_of_the_line_does_not_count() {
  // Crossing the line's infinite extension 40 m to the side is somewhere else
  // on the circuit, not the start/finish.
  CHECK_NEAR(calc_gate_cross(-1.0f, 40.0f, 1.0f, 40.0f, 25.0f), -1.0f, 0.0001f);
  // The sideways position is taken AT the crossing, interpolated: this step
  // starts outside the gate width but crosses the line inside it.
  CHECK(calc_gate_cross(-1.0f, 30.0f, 1.0f, 10.0f, 25.0f) >= 0.0f);
}

static void gate_coords_rotate_with_the_direction_of_travel() {
  // Heading east (d = 1,0): a point 10 m east is 10 m along, 0 across.
  float along, across;
  calc_gate_coords(10.0f, 0.0f, 1.0f, 0.0f, &along, &across);
  CHECK_NEAR(along, 10.0f, 0.001f);
  CHECK_NEAR(across, 0.0f, 0.001f);
  // Heading north: a point 10 m east is 0 along, 10 m to the side.
  calc_gate_coords(10.0f, 0.0f, 0.0f, 1.0f, &along, &across);
  CHECK_NEAR(along, 0.0f, 0.001f);
  CHECK_NEAR(across < 0 ? -across : across, 10.0f, 0.001f);
}

static void lap_time_survives_midnight() {
  // 23:59:50.000 to 00:00:40.000 is a 50 s lap, not minus 23 h 59 min.
  CHECK_EQ(calc_ms_of_day_diff(86390000UL, 40000UL), 50000UL);
  CHECK_EQ(calc_ms_of_day_diff(1000UL, 91000UL), 90000UL);
}

static void fix_quality_gate_uses_satellites_and_hdop() {
  CHECK(calc_fix_quality_ok(12, 0.9f, 5, 5.0f));
  CHECK(!calc_fix_quality_ok(3, 1.5f, 5, 5.0f));   // the tunnel case: good
                                                    // HDOP, too few satellites
  CHECK(!calc_fix_quality_ok(12, 2003.4f, 5, 5.0f)); // observed garbage HDOP
  // hdop not yet reported must NOT disqualify, or a module that never sends
  // GGA would log nothing at all.
  CHECK(calc_fix_quality_ok(12, 0.0f, 5, 5.0f));
  CHECK(calc_fix_quality_ok(12, -1.0f, 5, 5.0f));
}

// ---- g-meter projection -----------------------------------------------------

// Real at-rest readings from the pod, plus two synthetic mounts.
static const float kMounts[][3] = {
  { 0.0f,    0.0f,   -1.0f  },  // flat
  { -0.652f, -0.119f, -0.716f },  // the real pod, first calibration
  { 0.986f,  -0.021f, -0.254f },  // the real pod, later re-zero
  { 0.577f,   0.577f,  0.577f },  // an awkward corner mount
};

static float dot3(const float *p, const float *q) {
  return p[0]*q[0] + p[1]*q[1] + p[2]*q[2];
}

static void gmeter_at_rest_reads_zero_whatever_the_tilt() {
  // The property that prompted the projection: with the pod at 45 degrees the
  // old "pick the biggest axis" approach leaked gravity into the horizontal
  // trace, so the dot sat off-centre at a standstill.
  for (const auto &m : kMounts)
    for (int axis = 0; axis < 3; axis++) {
      const CalcGBasis b = calc_gmeter_basis(m, axis, false, true);
      float x, y, v;
      calc_gmeter_project(&b, m, &x, &y, &v);        // reading == calibration
      CHECK_NEAR(x, 0.0f, 0.001f);
      CHECK_NEAR(y, 0.0f, 0.001f);
      // At rest the vertical part is the LENGTH of the calibration vector,
      // only approximately 1 g: the real pod calibrated at 0.976 (the
      // QMI8658's scale is not exact). The depth cue treats 1.0 as rest, so
      // it reads slightly small at a standstill - cosmetic, and known.
      CHECK_NEAR(v, sqrtf(dot3(m, m)), 0.001f);
    }
}

static void gmeter_basis_is_orthonormal() {
  for (const auto &m : kMounts) {
    const CalcGBasis b = calc_gmeter_basis(m, 1, false, true);
    CHECK(b.valid);
    CHECK_NEAR(dot3(b.u, b.u), 1.0f, 0.001f);
    CHECK_NEAR(dot3(b.e1, b.e1), 1.0f, 0.001f);
    CHECK_NEAR(dot3(b.e2, b.e2), 1.0f, 0.001f);
    CHECK_NEAR(dot3(b.u, b.e1), 0.0f, 0.001f);       // all perpendicular
    CHECK_NEAR(dot3(b.u, b.e2), 0.0f, 0.001f);
    CHECK_NEAR(dot3(b.e1, b.e2), 0.0f, 0.001f);
  }
}

static void gmeter_horizontal_magnitude_is_preserved() {
  // The projection may rotate the frame but must not change how hard the car
  // is accelerating, or the number on screen is simply wrong.
  const float *off = kMounts[1];
  const CalcGBasis b = calc_gmeter_basis(off, 1, false, true);
  for (int deg = 0; deg < 360; deg += 45) {
    const float c = cosf(deg * 3.14159265f / 180), s = sinf(deg * 3.14159265f / 180);
    float a[3];
    for (int i = 0; i < 3; i++) a[i] = off[i] + 0.4f * (c * b.e1[i] + s * b.e2[i]);
    float x, y, v;
    calc_gmeter_project(&b, a, &x, &y, &v);
    CHECK_NEAR(sqrtf(x*x + y*y), 0.4f, 0.001f);
    CHECK_NEAR(v, sqrtf(dot3(off, off)), 0.001f);    // no vertical leak either
  }
}

static void gmeter_screen_axis_reads_as_x() {
  // Push the reading along the chosen board axis (its horizontal part): the
  // dot must move along screen-x only - the 90 degree turn seen on the real
  // pod was exactly this failing.
  const float *off = kMounts[1];
  for (int axis = 0; axis < 3; axis++) {
    const CalcGBasis b = calc_gmeter_basis(off, axis, false, false);
    float h[3] = { 0.0f, 0.0f, 0.0f };
    h[axis] = 1.0f;
    const float hu = dot3(h, b.u);
    for (int i = 0; i < 3; i++) h[i] -= hu * b.u[i];
    const float hn = sqrtf(dot3(h, h));
    float a[3];
    for (int i = 0; i < 3; i++) a[i] = off[i] + 0.3f * h[i] / hn;
    float x, y, v;
    calc_gmeter_project(&b, a, &x, &y, &v);
    CHECK_NEAR(x, 0.3f, 0.001f);
    CHECK_NEAR(y, 0.0f, 0.001f);
  }
}

static void gmeter_flips_invert_one_direction_each() {
  const float *off = kMounts[2];
  const CalcGBasis plain = calc_gmeter_basis(off, 1, false, false);
  float a[3];
  for (int i = 0; i < 3; i++) a[i] = off[i] + 0.2f * plain.e1[i] + 0.1f * plain.e2[i];
  float x0, y0, x, y, v;
  calc_gmeter_project(&plain, a, &x0, &y0, &v);
  const CalcGBasis fx = calc_gmeter_basis(off, 1, true, false);
  calc_gmeter_project(&fx, a, &x, &y, &v);
  CHECK_NEAR(x, -x0, 0.001f);
  CHECK_NEAR(y, y0, 0.001f);
  const CalcGBasis fy = calc_gmeter_basis(off, 1, false, true);
  calc_gmeter_project(&fy, a, &x, &y, &v);
  CHECK_NEAR(x, x0, 0.001f);
  CHECK_NEAR(y, -y0, 0.001f);
}

static void gmeter_flat_mount_gives_the_textbook_mapping() {
  // Board lying flat, Z up, board X across the screen: +X reads as right and
  // +Y as forward (y = u x x), with no flips. A sanity anchor for the maths.
  const float off[3] = { 0.0f, 0.0f, 1.0f };
  const CalcGBasis b = calc_gmeter_basis(off, 0, false, false);
  float x, y, v;
  const float ax[3] = { 0.25f, 0.0f, 1.0f };
  calc_gmeter_project(&b, ax, &x, &y, &v);
  CHECK_NEAR(x, 0.25f, 0.001f);
  CHECK_NEAR(y, 0.0f, 0.001f);
  const float ay[3] = { 0.0f, 0.25f, 1.0f };
  calc_gmeter_project(&b, ay, &x, &y, &v);
  CHECK_NEAR(x, 0.0f, 0.001f);
  CHECK_NEAR(y, 0.25f, 0.001f);
}

static void gmeter_degenerate_calibration_falls_back() {
  // An all-zero at-rest vector means the IMU never answered. Normalising it
  // would divide by zero; the basis must stay usable (board Z as down).
  const float zero[3] = { 0.0f, 0.0f, 0.0f };
  const CalcGBasis b = calc_gmeter_basis(zero, 1, false, true);
  CHECK(!b.valid);
  CHECK_NEAR(b.u[2], 1.0f, 0.001f);
  float x, y, v;
  const float a[3] = { 0.1f, 0.2f, 1.0f };
  calc_gmeter_project(&b, a, &x, &y, &v);
  CHECK(!std::isnan(x) && !std::isnan(y) && !std::isnan(v));
  CHECK_NEAR(v, 1.0f, 0.001f);
  CHECK_NEAR(sqrtf(x*x + y*y), sqrtf(0.1f*0.1f + 0.2f*0.2f), 0.001f);
}

static void gmeter_screen_axis_straight_down_does_not_divide_by_zero() {
  // Nonsense for a readable panel, but a zero-length e1 would put NaN on
  // screen: it must fall back to another axis instead.
  const float off[3] = { 0.0f, 1.0f, 0.0f };           // gravity along board Y
  const CalcGBasis b = calc_gmeter_basis(off, 1, false, false);
  CHECK_NEAR(dot3(b.e1, b.e1), 1.0f, 0.001f);
  CHECK_NEAR(dot3(b.e1, b.u), 0.0f, 0.001f);
  float x, y, v;
  const float a[3] = { 0.3f, 1.0f, 0.0f };
  calc_gmeter_project(&b, a, &x, &y, &v);
  CHECK(!std::isnan(x) && !std::isnan(y));
  CHECK_NEAR(sqrtf(x*x + y*y), 0.3f, 0.001f);
}

static void gmeter_shipped_mapping_is_the_bench_verified_one() {
  // Not maths - a tripwire. These values were checked on the bench on
  // 2026-09-26 by tilting the mounted pod: tilt back -> dot up, right edge
  // down -> dot right. If the pod is remounted they may need to change, but
  // then re-do that tilt check and update this test with the date.
  CHECK(GM_SCREEN_X_AXIS == 1);
  CHECK(!GM_FLIP_X);
  CHECK(GM_FLIP_Y);
}

// ---- drive detection (drops the WiFi AP while driving) -----------------------

// Feed a constant input for `ms`, in 100 ms steps, as the loop would.
static bool drive_for(CalcDrive *d, uint32_t *t, uint32_t ms, bool fix, float kmh) {
  bool r = d->driving;
  for (uint32_t e = 0; e < ms; e += 100, *t += 100)
    r = calc_drive_update(d, fix, kmh, *t, AP_DRIVE_ON_KMH, AP_DRIVE_ON_MS,
                          AP_DRIVE_OFF_KMH, AP_DRIVE_OFF_MS);
  return r;
}

static void drive_starts_only_after_speed_is_held() {
  CalcDrive d = {};
  uint32_t t = 1000;
  CHECK(!drive_for(&d, &t, AP_DRIVE_ON_MS - 500, true, 50.0f));  // not yet
  CHECK(drive_for(&d, &t, 1000, true, 50.0f));                     // now
}

static void a_brief_burst_of_speed_does_not_start_a_drive() {
  // Rolling across a car park, or one bad fix reading 30 km/h.
  CalcDrive d = {};
  uint32_t t = 1000;
  drive_for(&d, &t, AP_DRIVE_ON_MS - 500, true, 50.0f);
  CHECK(!drive_for(&d, &t, 200, true, 2.0f));
  CHECK(!drive_for(&d, &t, AP_DRIVE_ON_MS - 500, true, 50.0f));  // timer restarted
}

static void a_traffic_light_does_not_end_a_drive() {
  CalcDrive d = {};
  uint32_t t = 1000;
  drive_for(&d, &t, AP_DRIVE_ON_MS + 1000, true, 50.0f);
  CHECK(drive_for(&d, &t, 45000, true, 0.0f));                    // 45 s red
  CHECK(drive_for(&d, &t, 10000, true, 40.0f));                   // pulls away
  CHECK(drive_for(&d, &t, AP_DRIVE_OFF_MS - 1000, true, 0.0f));   // stopped again...
  CHECK(!drive_for(&d, &t, 2000, true, 0.0f));                    // ...long enough
}

static void speeds_between_the_thresholds_keep_the_state() {
  const float mid = (AP_DRIVE_ON_KMH + AP_DRIVE_OFF_KMH) / 2;
  CalcDrive d = {};
  uint32_t t = 1000;
  CHECK(!drive_for(&d, &t, 60000, true, mid));        // crawling never starts one
  drive_for(&d, &t, AP_DRIVE_ON_MS + 1000, true, 50.0f);
  CHECK(drive_for(&d, &t, AP_DRIVE_OFF_MS * 3, true, mid));   // nor ends one
}

static void losing_the_fix_ends_a_drive_eventually() {
  // Parked underground after a drive: no fix, ever again this wake.
  CalcDrive d = {};
  uint32_t t = 1000;
  drive_for(&d, &t, AP_DRIVE_ON_MS + 1000, true, 50.0f);
  CHECK(drive_for(&d, &t, AP_DRIVE_OFF_MS - 1000, false, 0.0f));
  CHECK(!drive_for(&d, &t, 2000, false, 0.0f));
  CHECK(!drive_for(&d, &t, 60000, false, 99.0f));   // no fix: speed is not trusted
}

static void drive_timing_survives_millis_wrap() {
  CalcDrive d = {};
  uint32_t t = 0xFFFFFFFFu - 2000;                  // ~49.7 days of uptime
  CHECK(drive_for(&d, &t, AP_DRIVE_ON_MS + 3000, true, 50.0f));
}

// Racelogic's .VBO format page gives latitude "+03119.09973" - 3119.09973
// total minutes = 51.9849955 deg N.
static void vbo_coordinates_are_total_minutes_with_west_positive() {
  char b[20];
  calc_vbo_minutes(b, sizeof(b), 519849955, false);
  CHECK(strcmp(b, "+03119.09973") == 0);
  // Brno-ish 16.45 E: east is NEGATIVE in a .vbo.
  calc_vbo_minutes(b, sizeof(b), 164500000, true);
  CHECK(strcmp(b, "-00987.00000") == 0);
  // And a west longitude comes out positive.
  calc_vbo_minutes(b, sizeof(b), -12345678, true);
  CHECK(strcmp(b, "+00074.07406") == 0);
  calc_vbo_minutes(b, sizeof(b), 0, false);
  CHECK(strcmp(b, "+00000.00000") == 0);
}

static void vbo_time_is_hhmmss_hundredths() {
  char b[16];
  calc_vbo_time(b, sizeof(b), ((14UL * 60 + 30) * 60 + 5) * 1000UL + 120);
  CHECK(strcmp(b, "143005.12") == 0);
  calc_vbo_time(b, sizeof(b), 0);
  CHECK(strcmp(b, "000000.00") == 0);
}

// Anchored to the ratios measured on the car (config.h GEAR_RPM_PER_KMH).
static void gear_comes_from_rpm_over_speed() {
  static const float g[] = GEAR_RPM_PER_KMH;
  const int n = sizeof(g) / sizeof(g[0]);
  CHECK(calc_gear(122 * 20, 20, g, n, GEAR_TOL, GEAR_MIN_KMH) == 1);
  CHECK(calc_gear(2600, 70, g, n, GEAR_TOL, GEAR_MIN_KMH) == 4);   // 37.1 rpm per km/h
  CHECK(calc_gear(2000, 100, g, n, GEAR_TOL, GEAR_MIN_KMH) == 7);
  // Every measured gear finds itself at its own ratio, from 30 km/h up.
  for (int i = 0; i < n; i++)
    if (g[i] > 0) CHECK(calc_gear(g[i] * 60, 60, g, n, GEAR_TOL, GEAR_MIN_KMH) == i + 1);
}

static void no_gear_while_slipping_shifting_or_crawling() {
  static const float g[] = GEAR_RPM_PER_KMH;
  const int n = sizeof(g) / sizeof(g[0]);
  // Between 1st and 2nd (clutch slip pulling away): nothing within tolerance.
  CHECK(calc_gear(95 * 15, 15, g, n, GEAR_TOL, GEAR_MIN_KMH) == 0);
  CHECK(calc_gear(1000, 5, g, n, GEAR_TOL, GEAR_MIN_KMH) == 0);    // below min speed
  CHECK(calc_gear(0, 50, g, n, GEAR_TOL, GEAR_MIN_KMH) == 0);      // no rpm
  // Between 7th and 8th: nothing.
  CHECK(calc_gear(18 * 120, 120, g, n, GEAR_TOL, GEAR_MIN_KMH) == 0);
}

// The tolerance must not let neighbouring gears overlap, or the gear would
// flip between two at the boundary.
static void gear_windows_do_not_overlap() {
  static const float g[] = GEAR_RPM_PER_KMH;
  const int n = sizeof(g) / sizeof(g[0]);
  for (int i = 0; i + 1 < n; i++)
    if (g[i] > 0 && g[i + 1] > 0)
      CHECK(g[i + 1] * (1 + GEAR_TOL) < g[i] * (1 - GEAR_TOL));
}

static void offset_ll_inverts_local_xy() {
  const int32_t lat0 = 492000000, lon0 = 164500000;     // 49.2 N 16.45 E
  int32_t lat, lon;
  calc_offset_ll(lat0, lon0, 25.0f, -10.0f, &lat, &lon);
  float x, y;
  calc_local_xy(lat0, lon0, lat, lon, &x, &y);
  CHECK_NEAR(x, 25.0f, 0.05f);
  CHECK_NEAR(y, -10.0f, 0.05f);
}

// Prague, 50.08 N 14.44 E. At solar noon the sun stands 90 - lat + dec high,
// dec = +-23.44 at the solstices; at solar midnight it is lat + dec - 90.
static void sun_is_high_at_summer_noon_and_low_at_winter_noon() {
  CHECK_NEAR(calc_sun_elevation_deg(1782039720.0, 50.08, 14.44), 63.36, 0.5);  // 2026-06-21 11:02Z
  CHECK_NEAR(calc_sun_elevation_deg(1797850980.0, 50.08, 14.44), 16.48, 0.5);  // 2026-12-21 11:03Z
}

static void sun_is_down_at_midnight_and_crosses_at_sunset() {
  CHECK_NEAR(calc_sun_elevation_deg(1782082920.0, 50.08, 14.44), -16.48, 0.5); // 2026-06-21 23:02Z
  // Prague's midsummer sunset is ~19:15 UTC (21:15 CEST): up before, down after.
  CHECK(calc_sun_elevation_deg(1782068400.0, 50.08, 14.44) > 0.0);           // 19:00Z
  CHECK(calc_sun_elevation_deg(1782070200.0, 50.08, 14.44) < 0.0);           // 19:30Z
}

static void range_reads_the_three_forms() {
  uint32_t a = 0, b = 0;
  CHECK(calc_parse_range("bytes=100-199", 1000, &a, &b) == 1 && a == 100 && b == 199);
  CHECK(calc_parse_range("bytes=600-", 1000, &a, &b) == 1 && a == 600 && b == 999);
  CHECK(calc_parse_range("bytes=-300", 1000, &a, &b) == 1 && a == 700 && b == 999);
}

// What the app sends for a grown file: from the old size to the size the
// listing reported. If the file grew again since, the end still holds.
static void range_end_past_the_file_is_clamped() {
  uint32_t a = 0, b = 0;
  CHECK(calc_parse_range("bytes=900-1999", 1000, &a, &b) == 1 && a == 900 && b == 999);
  CHECK(calc_parse_range("bytes=-5000", 1000, &a, &b) == 1 && a == 0 && b == 999);
}

static void range_past_the_end_is_416() {
  uint32_t a, b;
  CHECK(calc_parse_range("bytes=1000-", 1000, &a, &b) == -1);
  CHECK(calc_parse_range("bytes=0-", 0, &a, &b) == -1);
  CHECK(calc_parse_range("bytes=-1", 0, &a, &b) == -1);
}

// Anything not understood means "send it all", never a 416 or a wrong slice.
static void range_garbage_means_whole_file() {
  uint32_t a, b;
  CHECK(calc_parse_range(nullptr, 1000, &a, &b) == 0);
  CHECK(calc_parse_range("", 1000, &a, &b) == 0);
  CHECK(calc_parse_range("items=0-5", 1000, &a, &b) == 0);
  CHECK(calc_parse_range("bytes=0-5,10-20", 1000, &a, &b) == 0);   // multi-range
  CHECK(calc_parse_range("bytes=500-100", 1000, &a, &b) == 0);     // reversed
  CHECK(calc_parse_range("bytes=abc-", 1000, &a, &b) == 0);
  CHECK(calc_parse_range("bytes=5", 1000, &a, &b) == 0);
  CHECK(calc_parse_range("bytes=-0", 1000, &a, &b) == 0);
  CHECK(calc_parse_range("bytes=-", 1000, &a, &b) == 0);
  CHECK(calc_parse_range("bytes=1-2x", 1000, &a, &b) == 0);
}

static void hex_parses_exactly_or_not_at_all() {
  uint8_t b[4] = {9, 9, 9, 9};
  CHECK(calc_parse_hex("00ff7Ac3", b, 4) && b[0] == 0x00 && b[1] == 0xff && b[2] == 0x7a && b[3] == 0xc3);
  uint8_t keep[4] = {9, 9, 9, 9};
  CHECK(!calc_parse_hex("00ff7Ac", keep, 4));     // one short
  CHECK(!calc_parse_hex("00ff7Ac3a", keep, 4));   // one long
  CHECK(!calc_parse_hex("00ff7Ag3", keep, 4));    // not hex
  CHECK(!calc_parse_hex(" 0ff7Ac3", keep, 4));
  CHECK(!calc_parse_hex(nullptr, keep, 4));
  CHECK(keep[0] == 9 && keep[3] == 9);             // untouched on failure
}

int main() {
  printf("test_calc\n");
  TEST(hex_parses_exactly_or_not_at_all);
  TEST(range_reads_the_three_forms);
  TEST(range_end_past_the_file_is_clamped);
  TEST(range_past_the_end_is_416);
  TEST(range_garbage_means_whole_file);
  TEST(tz_inverts_the_sign_like_posix_requires);
  TEST(tz_handles_negative_and_zero_offsets);
  TEST(tz_fits_the_settings_field_at_every_offset);
  TEST(tz_is_safe_with_a_degenerate_buffer);
  TEST(tz_clamps_absurd_offsets_rather_than_truncating);
  TEST(trim_strips_the_space_a_phone_keyboard_adds);
  TEST(trim_keeps_inner_spaces);
  TEST(trim_handles_empty_and_all_space);
  TEST(trim_never_overruns_the_destination);
  TEST(timegm_matches_known_epochs);
  TEST(timegm_handles_leap_years_and_month_ends);
  TEST(timegm_ignores_the_local_timezone);
  TEST(implied_speed_matches_a_known_distance);
  TEST(fmt_deg_writes_exact_decimals);
  TEST(fmt_deg_handles_the_southern_and_western_hemispheres);
  TEST(implied_speed_refuses_to_divide_by_zero);
  TEST(implied_speed_accounts_for_longitude_convergence);
  TEST(real_tunnel_garbage_is_rejected_and_real_driving_is_not);
  TEST(local_xy_matches_known_offsets);
  TEST(gate_crossing_forwards_is_found_and_interpolated);
  TEST(gate_crossing_backwards_never_counts);
  TEST(gate_crossing_wide_of_the_line_does_not_count);
  TEST(gate_coords_rotate_with_the_direction_of_travel);
  TEST(lap_time_survives_midnight);
  TEST(fix_quality_gate_uses_satellites_and_hdop);
  TEST(gmeter_at_rest_reads_zero_whatever_the_tilt);
  TEST(gmeter_basis_is_orthonormal);
  TEST(gmeter_horizontal_magnitude_is_preserved);
  TEST(gmeter_screen_axis_reads_as_x);
  TEST(gmeter_flips_invert_one_direction_each);
  TEST(gmeter_flat_mount_gives_the_textbook_mapping);
  TEST(gmeter_degenerate_calibration_falls_back);
  TEST(gmeter_screen_axis_straight_down_does_not_divide_by_zero);
  TEST(gmeter_shipped_mapping_is_the_bench_verified_one);
  TEST(drive_starts_only_after_speed_is_held);
  TEST(a_brief_burst_of_speed_does_not_start_a_drive);
  TEST(a_traffic_light_does_not_end_a_drive);
  TEST(speeds_between_the_thresholds_keep_the_state);
  TEST(losing_the_fix_ends_a_drive_eventually);
  TEST(drive_timing_survives_millis_wrap);
  TEST(sun_is_high_at_summer_noon_and_low_at_winter_noon);
  TEST(sun_is_down_at_midnight_and_crosses_at_sunset);
  TEST(vbo_coordinates_are_total_minutes_with_west_positive);
  TEST(vbo_time_is_hhmmss_hundredths);
  TEST(offset_ll_inverts_local_xy);
  TEST(gear_comes_from_rpm_over_speed);
  TEST(no_gear_while_slipping_shifting_or_crawling);
  TEST(gear_windows_do_not_overlap);
  TEST_SUMMARY();
}
