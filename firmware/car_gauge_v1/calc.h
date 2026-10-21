// ============================================================================
// calc.h  —  the firmware's pure computation, with no Arduino dependencies.
//
// Everything here is a function of its arguments alone: no globals, no
// hardware, no time source. That is the point. These are the three pieces of
// logic in the project that are fiddly enough to get quietly wrong - a POSIX
// timezone string with an inverted sign, a distance check that has to reject
// tunnel garbage without rejecting real driving, and a 3-D projection that
// decides which way the g-meter dot moves - and keeping them here means the
// host tests in tests/ exercise the same code the board runs, rather than a
// reimplementation that can drift from it.
//
// Header-only so the Arduino build needs no extra translation unit.
// ============================================================================
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>     // int32_t/uint32_t - used here, so included here, not
                        // borrowed from whatever the includer happened to pull in
#include <stdio.h>
#include <stdlib.h>     // strtoull
#include <string.h>     // strncmp, strchr

// ---- Timezone ---------------------------------------------------------------
// Build a POSIX TZ string for a whole-hour UTC offset, optionally with the EU
// summer-time rule.
//
// The sign is inverted on purpose: POSIX states offsets as "how far WEST of
// Greenwich", so UTC+1 is written -1. That is why the familiar Czech rule
// reads "CET-1CEST" and not "CET+1", and it is the single easiest thing to get
// backwards here. The <+01> bracket form is used instead of a zone
// abbreviation because an offset alone cannot tell us the zone's real name.
// Verified against this board's newlib: "<+01>-1<+02>,M3.5.0,M10.5.0/3" turns
// 22:13:20 UTC into 23:13:20 local.
static inline void calc_build_tz(char *out, size_t n, int offsetHours, bool dst) {
  if (!out || n == 0) return;
  // Clamp to the real range of UTC offsets. The callers already validate, but
  // -Wformat-truncation correctly points out that an unclamped int could need
  // 46 bytes in settings.tz's 40 - and a truncated TZ string is a silently
  // wrong clock rather than a visible error. Cheaper to be safe by
  // construction than to rely on every caller.
  const int o = offsetHours < -12 ? -12 : (offsetHours > 14 ? 14 : offsetHours);
  if (dst) {
    snprintf(out, n, "<%+03d>%d<%+03d>,M3.5.0,M10.5.0/3", o, -o, o + 1);
  } else {
    snprintf(out, n, "<%+03d>%d", o, -o);
  }
}

// ---- UTC epoch from calendar fields ----------------------------------------
// Convert a UTC date/time to a Unix timestamp.
//
// Needed because mktime() interprets its input as LOCAL time, and this
// firmware setenv()s TZ to the user's zone - so using mktime() on the UTC
// fields from NMEA put the system clock out by the UTC offset, and every CSV
// row and GPX point carried that error while claiming to be UTC. timegm()
// would be the right function but is not declared in this newlib build, and
// temporarily setting TZ=UTC around mktime() would mutate global state that
// other code reads concurrently.
//
// Howard Hinnant's days_from_civil: exact for any proleptic Gregorian date,
// no loops, no tables. Month is 1-12 and day is 1-31, i.e. calendar
// convention, NOT struct tm's 0-based month.
static inline long long calc_days_from_civil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);              // [0, 399]
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return (long long)era * 146097LL + (long long)doe - 719468LL;
}

// year is the full year (2026, not 126), month 1-12, day 1-31.
static inline long long calc_timegm(int year, unsigned month, unsigned day,
                                    unsigned hour, unsigned min, unsigned sec) {
  return calc_days_from_civil(year, month, day) * 86400LL
       + (long long)hour * 3600LL + (long long)min * 60LL + (long long)sec;
}

// ---- Text tidying -----------------------------------------------------------
// Copy src into dst with leading and trailing whitespace removed; returns the
// resulting length.
//
// Exists because a phone keyboard's autocomplete appends a space, and a
// trailing space in an SSID is *legal* but invisible: the network still works
// and the QR still scans, while anyone typing the name by hand gets it subtly
// wrong with nothing on screen to explain why. Deliberately NOT applied to the
// passphrase - a space there may be intentional, and silently altering a
// credential someone typed is worse than honouring an odd one.
static inline size_t calc_trim_copy(char *dst, size_t dstSize, const char *src) {
  if (!dst || dstSize == 0) return 0;
  dst[0] = '\0';
  if (!src) return 0;
  const char *begin = src;
  while (*begin == ' ' || *begin == '\t' || *begin == '\r' || *begin == '\n') begin++;
  const char *end = begin;
  const char *last = begin;            // one past the final non-space
  while (*end) {
    if (*end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') last = end + 1;
    end++;
  }
  size_t n = (size_t)(last - begin);
  if (n > dstSize - 1) n = dstSize - 1;
  for (size_t i = 0; i < n; i++) dst[i] = begin[i];
  dst[n] = '\0';
  return n;
}

// A WPA2 passphrase softAP() takes: 8 to maxLen printable ASCII characters
// (0x20-0x7E, spaces allowed). Anything else - a control character, UTF-8
// from a phone keyboard - is refused by the WiFi driver, and a refused
// passphrase saved to NVS would leave the AP down on every boot.
static inline bool calc_ap_pass_ok(const char *s, size_t maxLen) {
  if (!s) return false;
  size_t n = 0;
  for (; s[n]; n++)
    if ((unsigned char)s[n] < 0x20 || (unsigned char)s[n] > 0x7E) return false;
  return n >= 8 && n <= maxLen;
}

// ---- GPS plausibility -------------------------------------------------------
// Implied ground speed in km/h between two fixes.
//
// Equirectangular approximation rather than haversine: at the distances
// between consecutive fixes (centimetres to metres at 25 Hz) the error is far
// below GPS noise, and this runs on every logged point.
//
// Returns 0 for a non-positive time delta, so callers cannot divide by zero
// or read a garbage speed from two points sharing a timestamp.
// Takes degrees x 1e7, the form GpsState stores and the module transmits.
//
// Subtracting FIRST, in exact integer arithmetic, is the point. Consecutive
// 25 Hz fixes differ by ~1e-6 degrees against absolute values around 50 and
// 14, so differencing in floating point spends most of the mantissa on the
// part that cancels. After the subtraction every quantity is small and float
// is comfortably enough - which is why no double appears here at all.
static inline float calc_distance_m(int32_t lat0e7, int32_t lon0e7,
                                    int32_t lat1e7, int32_t lon1e7) {
  const float dLat = (float)(lat1e7 - lat0e7) * 1e-7f;
  const float dLon = (float)(lon1e7 - lon0e7) * 1e-7f;
  const float latR = (float)lat0e7 * 1e-7f * 0.01745329252f;
  const float dx = dLon * 111320.0f * cosf(latR);
  const float dy = dLat * 110540.0f;
  return sqrtf(dx * dx + dy * dy);
}

static inline float calc_implied_kmh(int32_t lat0e7, int32_t lon0e7,
                                     int32_t lat1e7, int32_t lon1e7,
                                     float dtSec) {
  if (dtSec <= 0.0f) return 0.0f;
  return calc_distance_m(lat0e7, lon0e7, lat1e7, lon1e7) / dtSec * 3.6f;
}

// Degrees x 1e7 as exact decimal text, e.g. 500001234 -> "50.0001234".
//
// Integer division and remainder, so there is no rounding anywhere in the
// output path - a float conversion would reintroduce exactly the imprecision
// the int32 storage avoids. Seven decimals is the full resolution of the
// format (~1.1 cm); GPX and JSON both accept it.
//
// The negative case is why `a` is computed via int64: -(int32_t)INT32_MIN
// overflows, and latitude/longitude of 0 with a negative sign is a real input
// in the western and southern hemispheres.
static inline void calc_fmt_deg(char *out, unsigned outLen, int32_t v) {
  const unsigned long a = (unsigned long)(v < 0 ? -(long long)v : (long long)v);
  snprintf(out, outLen, "%s%lu.%07lu", v < 0 ? "-" : "",
           a / 10000000UL, a % 10000000UL);
}

// Is this fix worth recording at all?
//
// Measured against a real drive through a tunnel: quality flags
// alone are NOT enough. The worst points carried only 3-4 satellites but
// reported plausible HDOP of 1.5-2.2, and filtering on satellites and HDOP
// still left 35 jumps with a worst case of 12920 km/h. The distance check is
// what a bad fix cannot fake.
//
// hdop <= 0 means "not reported yet" and is not treated as a failure, or a
// module that never sends GGA would log nothing at all.
static inline bool calc_fix_quality_ok(int sats, float hdop,
                                        int minSats, float maxHdop) {
  if (sats < minSats) return false;
  if (hdop > 0.0f && hdop > maxHdop) return false;
  return true;
}

// ---- G-meter projection -----------------------------------------------------
// Turns a raw accelerometer reading into horizontal g plus a vertical
// component, for a pod mounted at an arbitrary angle.
//
// Picking "the axis carrying gravity" does not work: the real pod calibrated
// at x=-0.652 y=-0.119 z=-0.716, i.e. tilted ~45 deg with gravity split almost
// equally between two axes, so any largest-component choice flips around. The
// stable answer is to use the calibrated at-rest vector as "down" and project
// onto the plane perpendicular to it.
//
// The in-plane axes are tied to the board, not left to fall out of the maths:
// screen-x is the board axis that runs left-right across the panel
// (`screenXAxis`), projected into the horizontal plane; screen-y is "forward",
// perpendicular to it. An arbitrary basis was correctly scaled and centred but
// showed the trace turned 90 degrees on the real pod. The flips then set each
// direction's sign; which values are right for the pod is a bench check (see
// GM_SCREEN_X_AXIS in config.h).
struct CalcGBasis {
  float u[3];          // unit "down" vector in board axes (the at-rest reading)
  float e1[3], e2[3];  // screen-x and screen-y, unit, in the horizontal plane
  bool  valid;         // false when the calibration was unusable (Z-up assumed)
};

// `off` is the at-rest reading. A vector much shorter than 1 g cannot be
// gravity, so it is rejected and board Z is taken as "down".
static inline CalcGBasis calc_gmeter_basis(const float off[3], int screenXAxis,
                                           bool flipX, bool flipY) {
  CalcGBasis b;
  const float n = sqrtf(off[0]*off[0] + off[1]*off[1] + off[2]*off[2]);
  b.valid = (n >= 0.5f);
  for (int i = 0; i < 3; i++) b.u[i] = b.valid ? off[i] / n : (i == 2 ? 1.0f : 0.0f);

  // e1 = the screen's left-right board axis with its gravity component
  // removed. That collapses to nothing only if the axis points straight down,
  // which a panel someone reads never does - but then try the next axis
  // rather than divide by zero.
  float e1n = 0.0f;
  for (int k = 0; k < 3 && e1n < 1e-3f; k++) {
    float s[3] = { 0.0f, 0.0f, 0.0f };
    s[((screenXAxis % 3) + 3 + k) % 3] = 1.0f;
    const float su = s[0]*b.u[0] + s[1]*b.u[1] + s[2]*b.u[2];
    for (int i = 0; i < 3; i++) b.e1[i] = s[i] - su * b.u[i];
    e1n = sqrtf(b.e1[0]*b.e1[0] + b.e1[1]*b.e1[1] + b.e1[2]*b.e1[2]);
  }
  for (int i = 0; i < 3; i++) b.e1[i] /= e1n;
  // e2 = u x e1: with x right and z up, z x x = y, i.e. forward.
  b.e2[0] = b.u[1]*b.e1[2] - b.u[2]*b.e1[1];
  b.e2[1] = b.u[2]*b.e1[0] - b.u[0]*b.e1[2];
  b.e2[2] = b.u[0]*b.e1[1] - b.u[1]*b.e1[0];
  if (flipX) for (int i = 0; i < 3; i++) b.e1[i] = -b.e1[i];
  if (flipY) for (int i = 0; i < 3; i++) b.e2[i] = -b.e2[i];
  return b;
}

// outVert keeps its "about 1 g at rest" meaning, because it is measured along
// gravity - which is what the UI's depth cue expects.
static inline void calc_gmeter_project(const CalcGBasis *b, const float a[3],
                                       float *outX, float *outY, float *outVert) {
  const float vert = a[0]*b->u[0] + a[1]*b->u[1] + a[2]*b->u[2];
  float h[3];
  for (int i = 0; i < 3; i++) h[i] = a[i] - vert * b->u[i];   // horizontal part
  *outX    = h[0]*b->e1[0] + h[1]*b->e1[1] + h[2]*b->e1[2];
  *outY    = h[0]*b->e2[0] + h[1]*b->e2[1] + h[2]*b->e2[2];
  *outVert = vert;
}

// ---- Lap timing geometry -----------------------------------------------------
// A start/finish GATE: a line through an origin point, perpendicular to the
// direction of travel. Positions are projected into a local flat frame around
// the origin, in metres - the same equirectangular approximation as
// calc_distance_m(), accurate to well under a centimetre over the tens of
// metres a gate spans.
//
// x = east, y = north, both in metres from the origin. Subtract in int32 first,
// for the same reason calc_distance_m() does.
static inline void calc_local_xy(int32_t lat0e7, int32_t lon0e7,
                                 int32_t lat1e7, int32_t lon1e7,
                                 float *x, float *y) {
  const float latR = (float)lat0e7 * 1e-7f * 0.01745329252f;
  *x = (float)(lon1e7 - lon0e7) * 1e-7f * 111320.0f * cosf(latR);
  *y = (float)(lat1e7 - lat0e7) * 1e-7f * 110540.0f;
}

// The inverse of calc_local_xy(): the point x m east and y m north of
// (lat0, lon0). For drawing the lap gate's end points into a track log.
static inline void calc_offset_ll(int32_t lat0e7, int32_t lon0e7, float x, float y,
                                  int32_t *lat1e7, int32_t *lon1e7) {
  const float latR = (float)lat0e7 * 1e-7f * 0.01745329252f;
  *lat1e7 = lat0e7 + (int32_t)lroundf(y / 110540.0f * 1e7f);
  *lon1e7 = lon0e7 + (int32_t)lroundf(x / (111320.0f * cosf(latR)) * 1e7f);
}

// A coordinate as a Racelogic .vbo writes it: TOTAL arc-minutes (degrees x
// 60, not NMEA degrees-and-minutes), signed, 5 integer and 5 decimal digits -
// "+02952.35012". VBO longitude is positive WEST, the opposite of the usual
// convention, so pass westPositive for longitudes: Czechia comes out negative.
// (Racelogic's .VBO format page and real VBOX files, checked 2026-10-05.)
// Exact integer arithmetic: 1e-7 deg x 60 = 6e-6 min, kept as 1e-5 min.
static inline void calc_vbo_minutes(char *out, size_t n, int32_t deg1e7, bool westPositive) {
  int64_t v = (int64_t)deg1e7 * 6 / 10;               // 1e-5 arc-minutes
  if (westPositive) v = -v;
  const char sign = v < 0 ? '-' : '+';
  if (v < 0) v = -v;
  snprintf(out, n, "%c%05lld.%05lld", sign, (long long)(v / 100000), (long long)(v % 100000));
}

// The gear, from engine speed over road speed: in gear, rpm / kmh is fixed by
// the ratios, one value per gear. `rpmPerKmh` holds them, 1st gear first; a
// 0 entry is a gear not measured yet and never matches. Returns 1..n, or 0
// when nothing is within `tol` (a fraction, e.g. 0.07) - shifting, the
// clutch slipping, coasting in neutral - or below `minKmh`, where a whole
// km/h is too coarse to tell neighbouring gears apart.
static inline int calc_gear(float rpm, float kmh, const float *rpmPerKmh, int n,
                            float tol, float minKmh) {
  if (!(kmh >= minKmh) || !(rpm > 0)) return 0;
  const float r = rpm / kmh;
  int best = 0;
  float bestErr = tol;
  for (int g = 0; g < n; g++) {
    if (rpmPerKmh[g] <= 0) continue;
    const float err = fabsf(r / rpmPerKmh[g] - 1.0f);
    if (err <= bestErr) { bestErr = err; best = g + 1; }
  }
  return best;
}

// UTC time of day as a .vbo writes it: HHMMSS.SS.
static inline void calc_vbo_time(char *out, size_t n, uint32_t msOfDay) {
  msOfDay %= 86400000UL;
  snprintf(out, n, "%02lu%02lu%02lu.%02lu",
           (unsigned long)(msOfDay / 3600000UL), (unsigned long)(msOfDay / 60000UL % 60),
           (unsigned long)(msOfDay / 1000UL % 60), (unsigned long)(msOfDay % 1000UL / 10));
}

// Where a point sits relative to the gate. `along` is the signed distance past
// the line in the direction of travel (negative = not reached it yet);
// `across` is the distance sideways along the line. (dx, dy) is the unit
// direction of travel.
static inline void calc_gate_coords(float x, float y, float dx, float dy,
                                    float *along, float *across) {
  *along  = x * dx + y * dy;
  *across = -x * dy + y * dx;
}

// Did the step from `prev` to `cur` cross the gate FORWARDS, within
// `halfWidth` metres of the origin? If so, returns the fraction 0..1 of the
// way from prev to cur at which it crossed, which the caller uses to
// interpolate the crossing TIME - at 25 Hz and 200 km/h the car moves 2.2 m
// between fixes, so taking either fix's own time would be up to 40 ms out.
// Returns -1 if there was no qualifying crossing.
//
// Forwards only (prev before the line, cur on or past it), so driving the
// wrong way across the line - or a return straight that happens to pass close
// to it - never counts.
static inline float calc_gate_cross(float prevAlong, float prevAcross,
                                    float curAlong, float curAcross,
                                    float halfWidth) {
  if (!(prevAlong < 0.0f && curAlong >= 0.0f)) return -1.0f;
  const float f = -prevAlong / (curAlong - prevAlong);   // denominator > 0
  const float across = prevAcross + f * (curAcross - prevAcross);
  if (across > halfWidth || across < -halfWidth) return -1.0f;
  return f;
}

// One step between two fixes, as distance along the lap: its length in the
// gate's frame (a rotation, so it is the real distance), or 0 for a step the
// car cannot have driven - more than maxMps for the time between - so a fix
// that jumps does not eat the distance left to the line.
static inline float calc_lap_step_m(float prevAlong, float prevAcross,
                                    float curAlong, float curAcross,
                                    uint32_t dtMs, float maxMps) {
  const float d = sqrtf((curAlong - prevAlong) * (curAlong - prevAlong) +
                        (curAcross - prevAcross) * (curAcross - prevAcross));
  return (dtMs > 0 && d <= maxMps * dtMs / 1000.0f) ? d : 0.0f;
}

// Metres left to the line against a reference lap's length (the best lap):
// never below 0 - a shorter lap, through the pit lane, simply reaches the
// line early, and its crossing starts the count again.
static inline float calc_lap_left_m(float refM, float lapM) {
  return refM > lapM ? refM - lapM : 0.0f;
}

// A lap's time against its distance, for the live delta to the best lap:
// prof[i] is the lap time, in CALC_LAP_PROF_UNIT_MS units, at i * stepM along
// it (prof[0] = 0, at the line). One step of the lap, from (d0 m, t0 ms) to
// (d1, t1), fills the marks it passes - each at its interpolated time - after
// the n already filled, up to cap. Returns the new count.
//
// 20 ms units: a uint16 then reaches 21:50. In 10 ms it stopped at 10:55,
// short of a slow Nordschleife lap, and the delta read nonsense from there.
// uint16 because the best lap's profile sits in RTC memory (2 KB of it).
#define CALC_LAP_PROF_UNIT_MS 20
static inline uint16_t calc_lap_prof_add(uint16_t *prof, uint16_t n, uint16_t cap, float stepM,
                                         float d0, uint32_t t0Ms, float d1, uint32_t t1Ms) {
  if (!(d1 > d0)) return n;
  while (n < cap && n * stepM <= d1) {
    float f = (n * stepM - d0) / (d1 - d0);
    if (f < 0.0f) f = 0.0f;   // a mark behind the step (never filled): its start
    const float t = (t0Ms + f * (float)(t1Ms - t0Ms)) / (float)CALC_LAP_PROF_UNIT_MS;
    prof[n++] = t > 65535.0f ? 65535 : (uint16_t)lroundf(t);
  }
  return n;
}

// The time on a lap's profile at d metres along it, in ms - interpolated
// between its marks - or -1 where it has none: beyond its end, or no
// profile.
static inline int32_t calc_lap_prof_ms(const uint16_t *prof, uint16_t n, float stepM, float d) {
  if (n < 2 || d < 0.0f) return -1;
  const float x = d / stepM;
  const uint32_t i = (uint32_t)x;
  if (i + 1 >= n) return -1;
  const float f = x - (float)i;
  return (int32_t)lroundf((prof[i] + f * ((float)prof[i + 1] - (float)prof[i])) *
                          (float)CALC_LAP_PROF_UNIT_MS);
}

// "Is the car being driven?", from GNSS speed, with hysteresis both ways.
// Starts after speed >= onKmh with a fix, held for onMs; ends after speed
// < offKmh OR no fix, held for offMs. Anything between the two speeds keeps
// the current state. offMs is long on purpose: a traffic light must not end
// a drive, or whatever this drives (the WiFi AP) flaps on every red.
//
// Losing the fix counts towards "stopped" rather than freezing the state:
// otherwise a drive that ends in an underground garage would never end.
// A long tunnel therefore ends the drive after offMs, which is the price.
struct CalcDrive {
  bool     driving;
  bool     pending;      // the condition for flipping has started to hold
  uint32_t pendingSince;
};

static inline bool calc_drive_update(CalcDrive *d, bool haveFix, float kmh,
                                     uint32_t now, float onKmh, uint32_t onMs,
                                     float offKmh, uint32_t offMs) {
  const bool flip = d->driving ? (!haveFix || kmh < offKmh)
                               : (haveFix && kmh >= onKmh);
  if (!flip) { d->pending = false; return d->driving; }
  if (!d->pending) { d->pending = true; d->pendingSince = now; }
  if (now - d->pendingSince >= (d->driving ? offMs : onMs)) {
    d->driving = !d->driving;
    d->pending = false;
  }
  return d->driving;
}

// The sun's elevation above the horizon in degrees (negative = below), at a
// UTC unix time and a position - for dimming the display at night. The usual
// low-precision solar position (as in the Astronomical Almanac's approximate
// formulas): about 0.1 degree, far finer than "is it dark yet" needs. No
// refraction correction, which only matters within a degree of the horizon.
// Double precision: the day count since 2000 times 0.9856 loses whole degrees
// of mean anomaly in float.
static inline double calc_sun_elevation_deg(double unixUtc, double latDeg, double lonDeg) {
  const double rad = M_PI / 180.0;
  const double d = unixUtc / 86400.0 - 10957.5;          // days since 2000-01-01 12:00 UTC
  const double g = fmod(357.529 + 0.98560028 * d, 360.0) * rad;   // mean anomaly
  const double q = fmod(280.459 + 0.98564736 * d, 360.0);         // mean longitude
  const double L = (q + 1.915 * sin(g) + 0.020 * sin(2 * g)) * rad;  // ecliptic longitude
  const double e = (23.439 - 0.00000036 * d) * rad;                  // obliquity
  const double ra  = atan2(cos(e) * sin(L), cos(L));
  const double dec = asin(sin(e) * sin(L));
  const double gmstDeg = fmod(280.46061837 + 360.98564736629 * d, 360.0);
  const double ha = (gmstDeg + lonDeg) * rad - ra;                    // local hour angle
  const double lat = latDeg * rad;
  return asin(sin(lat) * sin(dec) + cos(lat) * cos(dec) * cos(ha)) / rad;
}

// Exactly 2*n hex digits (either case) into n bytes - the log encryption key
// arrives from the phone as 64 of them. False, and out untouched, for any
// other length or a non-hex character: a key that is almost right must not
// quietly become a different key.
static inline bool calc_parse_hex(const char *s, uint8_t *out, size_t n) {
  if (!s || strlen(s) != 2 * n) return false;
  uint8_t tmp[64];
  if (n > sizeof(tmp)) return false;
  for (size_t i = 0; i < 2 * n; i++) {
    const char c = s[i];
    const int v = (c >= '0' && c <= '9') ? c - '0'
                : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    if (v < 0) return false;
    if (i % 2 == 0) tmp[i / 2] = (uint8_t)(v << 4);
    else            tmp[i / 2] |= (uint8_t)v;
  }
  memcpy(out, tmp, n);
  return true;
}

// An HTTP Range header ("bytes=a-b", "bytes=a-", "bytes=-n") against a file of
// `size` bytes, for /dl: the phone app resumes a download that dropped
// mid-file, or fetches only the tail of a log that has grown since the last
// sync. One range only - a multi-range request ("a-b,c-d") would need a
// multipart reply, so it is refused rather than half-answered.
// Returns 1 with [*first, *last] (inclusive, clamped to the file) for a
// satisfiable range, 0 for "no usable Range, send the whole file" (absent or
// not understood - RFC 9110 says to ignore it then), and -1 for a range that
// starts past the end, which is a 416.
static inline int calc_parse_range(const char *h, uint32_t size, uint32_t *first, uint32_t *last) {
  if (!h || strncmp(h, "bytes=", 6) != 0) return 0;
  const char *p = h + 6;
  if (strchr(p, ',')) return 0;
  const bool suffix = (*p == '-');
  if (suffix) p++;
  if (*p < '0' || *p > '9') return 0;
  char *end;
  const unsigned long long a = strtoull(p, &end, 10);
  if (suffix) {
    if (*end != '\0' || a == 0) return 0;
    if (size == 0) return -1;
    *first = a >= size ? 0 : (uint32_t)(size - a);
    *last = size - 1;
    return 1;
  }
  if (*end != '-') return 0;
  p = end + 1;
  unsigned long long b = (unsigned long long)size - 1;
  if (*p) {
    if (*p < '0' || *p > '9') return 0;
    b = strtoull(p, &end, 10);
    if (*end != '\0' || b < a) return 0;
  }
  if (a >= size) return -1;
  *first = (uint32_t)a;
  *last = b >= size ? size - 1 : (uint32_t)b;
  return 1;
}

// Milliseconds from a to b, both in milliseconds since UTC midnight, so a lap
// that straddles midnight does not come out as minus 24 hours.
static inline uint32_t calc_ms_of_day_diff(uint32_t a, uint32_t b) {
  const uint32_t day = 86400000UL;
  return (b + day - a) % day;
}

// A screen order as stored (settings.views): view ids as digits, in cycle
// order; a view left out is hidden. Keeps the digits that are in `valid`,
// each once (the first wins), in their order, and appends `required` if it
// is missing - the WiFi screen, so the way to the phone is never hidden.
// Anything else - letters, unknown or repeated ids - is dropped.
static inline void calc_view_order(const char *in, const char *valid, char required,
                                   char *out, size_t n) {
  size_t o = 0;
  bool haveRequired = false;
  for (const char *p = in; *p && o + 2 < n; p++) {
    bool ok = false, seen = false;
    for (const char *v = valid; *v; v++) ok |= (*v == *p);
    for (size_t i = 0; i < o; i++) seen |= (out[i] == *p);
    if (!ok || seen) continue;
    out[o++] = *p;
    haveRequired |= (*p == required);
  }
  if (!haveRequired) out[o++] = required;
  out[o] = '\0';
}

// The card listing's order, newest first (the /files page): a log by what
// follows its type - "drive_2026-10-12_21-21_2df269.csv" by
// "2026-10-12_21-21_2df269.csv" - so a drive's CSV, GPX and VBO sit together
// and the latest drive is on top; numbered (undated) logs come after the
// dated ones; anything else (sleeplog.txt, firmware.bin) last, by name.
// FAT directory order is not age order: a new file takes a freed slot.
// qsort-style: < 0 when a lists before b.
static inline int calc_card_order(const char *a, const char *b) {
  static const char *const kinds[] = { "drive_", "trip_", "track_" };
  const char *ka = nullptr, *kb = nullptr;
  for (const char *k : kinds) {
    const size_t n = strlen(k);
    if (!ka && strncmp(a, k, n) == 0) ka = a + n;
    if (!kb && strncmp(b, k, n) == 0) kb = b + n;
  }
  // Descending: the newer first. A session's files differ only in the
  // extension, so they stay together (.vbo, .gpx, .csv).
  if (ka && kb) {
    const int c = strcmp(kb, ka);
    return c ? c : strcmp(a, b);
  }
  if (ka || kb) return ka ? -1 : 1;             // logs before everything else
  return strcmp(a, b);
}

// ---- ISO-TP (ISO 15765-2) receive -------------------------------------------
// Multi-frame replies: the oil DID (0xE001) answers with a 45-byte payload as
// a First Frame plus Consecutive Frames, sent only after we answer the First
// Frame with a Flow Control. Two modules answer us (the engine ECU on 0x7E8,
// TPMS on 0x7A8) and both reply multi-frame, into one receive context.
// Verified on the car: 99.8 % and 100 % reassembly across two drives.
#define CALC_ISOTP_BUF 96

struct CalcIsoTp {
  uint8_t  buf[CALC_ISOTP_BUF];
  uint16_t expected = 0, got = 0;
  uint8_t  nextSeq = 1;
  uint32_t startMs = 0;
  bool     active = false;
  uint32_t id = 0;          // CAN ID of the responder mid-transfer
};

enum CalcIsoEvent : uint8_t {
  CALC_ISO_NONE,      // nothing for the caller: mid-transfer, or not ISO-TP data
  CALC_ISO_SINGLE,    // a Single Frame: *payload / *len is the UDS payload
  CALC_ISO_FIRST,     // a First Frame: send Flow Control to the responder (id - 8)
  CALC_ISO_DONE,      // a transfer completed: *payload / *len, reassembled
  CALC_ISO_ABORT,     // a frame that broke, or would have broken, a transfer
};

// One received CAN frame. `dlc` is the frame's own: bytes past it are never
// read. This core's TWAI HAL copies only dlc bytes and does NOT zero the
// rest, so data[dlc..7] hold the tail of the PREVIOUS frame - a short final
// Consecutive Frame (some ECUs don't pad it) would otherwise splice bytes
// from an unrelated frame into the payload.
static inline CalcIsoEvent calc_isotp_rx(CalcIsoTp *t, uint32_t id, const uint8_t *data,
                                         uint8_t dlc, uint32_t nowMs,
                                         const uint8_t **payload, uint16_t *len) {
  if (dlc > 8) dlc = 8;
  if (dlc < 2) return CALC_ISO_NONE;            // no PCI + data to read
  const uint8_t pci = data[0] & 0xF0;

  if (pci == 0x00) {                            // Single Frame
    uint8_t n = data[0] & 0x0F;
    if (n > dlc - 1) n = dlc - 1;
    *payload = &data[1];
    *len = n;
    return CALC_ISO_SINGLE;
  }
  if (pci == 0x10) {                            // First Frame
    // With one shared context and no responder check, a TPMS First Frame
    // could reset the buffer mid-oil-transfer, and the oil module's
    // Consecutive Frames would then be appended to the TPMS payload because
    // only the sequence number was validated. That completed "successfully":
    // engine bytes decoded as tyre pressures, which lands around 1.96 bar and
    // tripped a false low-pressure warning. Reject the interloper instead.
    if (t->active && id != t->id) return CALC_ISO_ABORT;
    t->id       = id;
    t->expected = (uint16_t)(((data[0] & 0x0F) << 8) | data[1]);
    if (t->expected > CALC_ISOTP_BUF) t->expected = CALC_ISOTP_BUF;
    t->got = 0;
    for (int i = 2; i < dlc && t->got < t->expected; i++) t->buf[t->got++] = data[i];
    t->nextSeq = 1;
    t->active  = true;
    t->startMs = nowMs;
    return CALC_ISO_FIRST;
  }
  if (pci == 0x20 && t->active) {               // Consecutive Frame
    // Must come from the module whose transfer is actually in flight.
    if (id != t->id) return CALC_ISO_ABORT;
    if ((data[0] & 0x0F) != (t->nextSeq & 0x0F)) {
      t->active = false;
      return CALC_ISO_ABORT;
    }
    for (int i = 1; i < dlc && t->got < t->expected; i++) t->buf[t->got++] = data[i];
    t->nextSeq++;
    if (t->got >= t->expected) {
      t->active = false;
      *payload = t->buf;
      *len = t->got;
      return CALC_ISO_DONE;
    }
  }
  return CALC_ISO_NONE;
}

// A transfer that stalled: true when it was just given up.
static inline bool calc_isotp_expire(CalcIsoTp *t, uint32_t nowMs, uint32_t timeoutMs) {
  if (!t->active || nowMs - t->startMs <= timeoutMs) return false;
  t->active = false;
  return true;
}

// ---- Mode 01 replies ----------------------------------------------------------
// Data bytes of a Mode 01 PID, for walking a multi-PID reply. 0 = not known:
// the walk stops there and hands that PID whatever is left, which is exactly
// right for a single-PID reply.
static inline uint8_t calc_pid_data_len(uint8_t pid) {
  switch (pid) {
    case 0x00: case 0x20: case 0x40: case 0x60: case 0x80: return 4;   // bitmaps
    case 0x34: return 4;
    case 0x0C: case 0x43: case 0x42: case 0x3C: case 0x3E: case 0x44: case 0x59:
    case 0x5E: case 0x63: return 2;
    case 0x04: case 0x05: case 0x0B: case 0x0D: case 0x0E: case 0x0F: case 0x11:
    case 0x2F: case 0x33: case 0x45: case 0x46: case 0x47: case 0x49: case 0x4A:
    case 0x4C: case 0x5C: return 1;
    default: return 0;
  }
}

// A Mode 01 reply [41, PID, data, PID, data, ...] - one PID, or several when
// several were asked for and the ECU packs them in one reply. Calls
// fn(pid, data, bytes) for each. The caller checks p[0] == 0x41, len >= 3.
template <typename F>
static inline void calc_mode01_walk(const uint8_t *p, uint16_t len, F fn) {
  uint16_t i = 1;
  while (i + 1 < len) {
    const uint8_t pid = p[i];
    const uint8_t n = calc_pid_data_len(pid);
    const uint16_t avail = len - i - 1;
    if (n == 0 || n > avail) { fn(pid, &p[i + 1], avail); break; }
    fn(pid, &p[i + 1], n);
    i += 1 + n;
  }
}

// One PID's value from its data bytes (dl of them), per SAE J1979, in the
// unit the firmware keeps it in. False for a PID not decoded here, or too few
// bytes. Raw where the raw byte is what is kept (0x33 kPa, 0x49 pedal,
// 0x0B MAP kPa).
static inline bool calc_mode01_value(uint8_t pid, const uint8_t *d, uint16_t dl, float *v) {
  if (dl < 1) return false;
  const float ab = dl >= 2 ? (float)(d[0] << 8 | d[1]) : 0.0f;
  switch (pid) {
    case 0x05: case 0x0F: case 0x46:          // A - 40 °C: coolant, intake air, ambient
      *v = d[0] - 40.0f; return true;
    case 0x0B: case 0x0D: case 0x33: case 0x49:   // MAP kPa, km/h, baro kPa, pedal raw
      *v = d[0]; return true;
    case 0x0E: *v = d[0] / 2.0f - 64.0f; return true;          // deg before TDC
    case 0x2F: *v = d[0] * 100.0f / 255.0f; return true;       // fuel level %
  }
  if (dl < 2) return false;
  switch (pid) {
    // rpm: (256A + B) / 4, kept as whole rpm - integer division on purpose.
    case 0x0C: *v = (float)((d[0] << 8 | d[1]) / 4); return true;   // NOLINT(bugprone-integer-division)
    case 0x43: *v = ab * 100.0f / 255.0f; return true;         // absolute load %
    case 0x42: *v = ab / 1000.0f; return true;                 // module voltage V
    case 0x5E: *v = ab / 20.0f; return true;                   // fuel rate L/h
    case 0x34: case 0x44: *v = ab * 2.0f / 65536.0f; return true;   // lambda
    case 0x3C: *v = ab / 10.0f - 40.0f; return true;           // catalyst °C
    case 0x59: *v = ab / 10.0f; return true;                   // rail: x10 kPa -> bar
  }
  return false;
}

// ---- the GPX gate ---------------------------------------------------------------
// Which GNSS fixes become track points, and which count towards the trip:
// rate, quality, physical plausibility, in that order. The logger
// (log_gpx.cpp) asks once per pass with the latest fix; what it writes and
// sums is what passes here. "Load-bearing" (DECISIONS.md): every rule below
// is the fix for something that went wrong on a real drive.
struct CalcGpxLimits {
  uint32_t pointMs, idlePointMs;   // the point rate, moving and parked
  int      minSats;                // calc_fix_quality_ok()
  float    maxHdop;
  uint32_t segGapMs;               // a longer gap breaks the track
  float    maxJumpKmh;             // faster than this from the last point: a spike
  uint16_t maxRejectRun;           // then accept anyway
  float    movingKmh;              // below this, distance is not counted
  uint32_t tripGapMs;              // closer than this, provably the same trip
};

struct CalcGpxGate {
  uint32_t lastMs = 0;             // the last accepted point; 0 = none yet
  bool     sawSats = false;        // the module has reported satellites
  int32_t  okLat = 0, okLon = 0;   // the last accepted position, degrees x 1e7
  uint16_t rejectRun = 0;
};

// What an accepted point means for the caller.
struct CalcGpxStep {
  float addM;     // metres for the trip distance
  bool  recent;   // the previous point was under tripGapMs ago (millis says so)
  bool  gap;      // a segment break goes before this point
};

// True when the fix at `nowMs` is a point; *out then says what it means.
static inline bool calc_gpx_gate(CalcGpxGate *g, const CalcGpxLimits *lim, uint32_t nowMs,
                                 bool idle, int sats, float hdop, int32_t lat, int32_t lon,
                                 float kmh, CalcGpxStep *out) {
  // RATE LIMIT FIRST. Everything below judges a candidate against the last
  // point accepted, so it has to run at the point rate, not the loop rate.
  //
  // This ordering is load-bearing, and getting it wrong broke the jump check
  // outright: with the limiter last, the check ran every loop iteration (~5 ms)
  // and refreshed its reference point each time, while the position only
  // changes every GPS_NAV_RATE_MS (40 ms). So 40 ms of travel got divided by a
  // 5 ms interval - roughly an 8x overstatement - and the effective rejection
  // threshold collapsed from 250 km/h to about 31. Ordinary driving was being
  // thrown away.
  const uint32_t interval = idle ? lim->idlePointMs : lim->pointMs;
  if (g->lastMs != 0 && nowMs - g->lastMs < interval) return false;

  // Refuse points the receiver does not actually stand behind. On the first
  // real drive 315 points (0.8 %) went in with <sat>0</sat> and hdop 99.99 -
  // a frozen position with a stale speed, written for up to GPS_FIX_STALE_MS
  // after the fix was lost, in runs of 52 points. In a viewer that reads as
  // the car stopping dead and then teleporting.
  //
  // Only on a module that has proven it reports satellite counts: if GGA were
  // absent, sats would sit at 0 forever and this would silently disable
  // logging altogether.
  if (sats > 0) g->sawSats = true;
  if (g->sawSats && !calc_fix_quality_ok(sats, hdop, lim->minSats, lim->maxHdop)) return false;

  // Physical plausibility, the check that actually fixes tunnels. Coming out
  // of one the track sprayed across the map in straight spikes, with 3-4
  // satellites and a plausible HDOP of 1.5-2.2: quality alone left 35 jumps,
  // the worst at 12920 km/h. A car cannot move that fast, so the distance
  // from the last ACCEPTED point is the reliable discriminator.
  //
  // Across a real gap nothing can be judged - the car moved while there was
  // no fix - so a point after segGapMs is always accepted and the track is
  // broken there instead. The reject-run limit is an escape hatch: if the
  // reference itself is bad, refusing forever would lose the rest of the trip.
  const bool continuous = g->lastMs != 0 && nowMs - g->lastMs <= lim->segGapMs;
  if (continuous) {
    const float jump = calc_implied_kmh(g->okLat, g->okLon, lat, lon,
                                        (nowMs - g->lastMs) / 1000.0f);
    if (jump > lim->maxJumpKmh && g->rejectRun < lim->maxRejectRun) {
      g->rejectRun++;
      return false;
    }
  }

  // Trip distance only across a continuous stretch - beyond segGapMs it would
  // be a straight line between where the signal died and where it returned -
  // and only while moving: standing at lights the position wanders by
  // centimetres to metres between points, at 25 Hz, for minutes. GNSS speed
  // is Doppler-derived and far steadier near zero than position is.
  out->addM   = continuous && kmh >= lim->movingKmh
                    ? calc_distance_m(g->okLat, g->okLon, lat, lon) : 0.0f;
  out->recent = g->lastMs != 0 && nowMs - g->lastMs < lim->tripGapMs;
  out->gap    = g->lastMs != 0 && nowMs - g->lastMs > lim->segGapMs;

  g->rejectRun = 0;
  g->okLat     = lat;
  g->okLon     = lon;
  g->lastMs    = nowMs;
  return true;
}

// ---- bringing the clock to GNSS time --------------------------------------------
// Step the clock (settimeofday), slew it (adjtime - it never runs backwards),
// or leave it. A step backwards is what made GPX times go back - consecutive
// points out of order - so once the clock is a real time, an error up to
// slewMaxUs is slewed; only a larger one, or a clock not yet set at all, is
// stepped. Under keepUs it is left alone.
enum CalcClockFix : uint8_t { CALC_CLOCK_KEEP, CALC_CLOCK_SLEW, CALC_CLOCK_STEP };
static inline CalcClockFix calc_clock_fix(long long sysUs, long long gnssUs, bool clockValid,
                                          long long slewMaxUs, long long keepUs) {
  if (!clockValid) return CALC_CLOCK_STEP;
  const long long d = gnssUs - sysUs;
  const long long a = d < 0 ? -d : d;
  if (a < keepUs) return CALC_CLOCK_KEEP;
  if (a <= slewMaxUs) return CALC_CLOCK_SLEW;
  return CALC_CLOCK_STEP;
}

// ---- is a reading current? -----------------------------------------------------
// Stale data must present as NO data: a number that has stopped changing looks
// exactly like a steady reading. `have` = it ever arrived; `stampMs` = when
// it last did. Signed, as calc_elapsed() below: the stamps are written on the
// CAN and GNSS tasks' core, and one a millisecond newer than `nowMs` is fresh,
// not 49 days old (unsigned, the screen blinked "--" for that pass).
static inline bool calc_fresh(bool have, uint32_t stampMs, uint32_t nowMs,
                              uint32_t maxAgeMs) {
  return have && (int32_t)(nowMs - stampMs) < (int32_t)maxAgeMs;
}

// ---- elapsed time, one way -----------------------------------------------------
// The firmware compared millis() stamps two ways - unsigned (now - t >= X)
// and signed ((int32_t)(now - t) < (int32_t)X) - and they differ exactly
// where it matters: a stamp written by a task on the other core can be a
// millisecond NEWER than the `now` this core sampled. Unsigned, now - t then
// underflows to ~4.29e9 and "long ago" - power.cpp once slept the gauge
// mid-drive that way. Signed, it is a small negative: "just now". So these
// are signed, and THE way; wrap-safe across the 49-day millis() wrap, for
// spans under 2^31 ms (24 days).
// At least `ms` since `since`; a `since` in the future has not elapsed.
static inline bool calc_elapsed(uint32_t nowMs, uint32_t sinceMs, uint32_t ms) {
  return (int32_t)(nowMs - sinceMs) >= (int32_t)ms;
}
// `deadlineMs` reached (a time set as now + something), within 2^31 ms of it.
static inline bool calc_reached(uint32_t nowMs, uint32_t deadlineMs) {
  return (int32_t)(nowMs - deadlineMs) >= 0;
}

// ---- a page number from the phone ---------------------------------------------
// The card listings' ?p=: digits only, 0..maxPage, else -1 (a 400). It was
// String::toInt() - a huge p became INT_MAX, p * perPage overflowed to a
// negative index, and the listing read before its own table. A page past the
// end is not an error: it is an empty page (files can go between requests).
static inline int calc_parse_page(const char *s, int maxPage) {
  if (!s || !*s) return -1;
  long v = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') return -1;
    v = v * 10 + (*s - '0');
    if (v > maxPage) return -1;
  }
  return (int)v;
}

// ---- card file names from the phone ------------------------------------------
// A name the web API may open on the card. Everything there is flat in the
// card root, so a name containing a separator or ".." can only be an attempt
// to escape it.
//
// A whitelist, not a blacklist. The blacklist version was not exploitable,
// but it relied on a check/use mismatch failing closed: String::indexOf() is
// strchr-based and stops at an embedded NUL, while the length check uses
// length() - so "x%00/../secret" passed the separator check, and only survived
// because c_str() truncated at the same NUL before the filesystem saw it. A
// character whitelist over all `len` bytes does not depend on that, and also
// rejects CR/LF (the name is interpolated into a Content-Disposition header).
// It covers every name this firmware writes: drive_/trip_/track_ files, dated
// (drive_2026-09-25_14-32_3fa81c.csv) or numbered (trip_0012_3fa81c.gpx),
// maybe with the encrypted suffix, sleeplog.txt and bootlog.txt, and those
// two moved aside when full (sleeplog_0001.txt).
//
// The longest of those is 39 (drive_2026-09-25_14-32_9_3fa81c.csv.nge), so the
// limit leaves room; it was 40, one character from refusing a real log.
#define CALC_SAFE_NAME_MAX 47
static inline bool calc_safe_name(const char *s, size_t len) {
  if (!s || len == 0 || len > CALC_SAFE_NAME_MAX) return false;
  for (size_t i = 0; i < len; i++) {
    const char c = s[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
    if (c == '.' && i + 1 < len && s[i + 1] == '.') return false;
  }
  return true;
}

// A drive's log, which the app may delete: trip_/drive_/track_ ... .gpx /
// .csv / .vbo, plain or with `cryptSuffix` once. Never the settings, the
// sleep log or a firmware .bin. Pair it with calc_safe_name().
static inline bool calc_is_drive_log(const char *name, const char *cryptSuffix) {
  if (!name) return false;
  const bool kind = strncmp(name, "trip_", 5) == 0 || strncmp(name, "drive_", 6) == 0 ||
                    strncmp(name, "track_", 6) == 0;
  if (!kind) return false;
  size_t len = strlen(name);
  const size_t sl = strlen(cryptSuffix);
  if (len >= sl && strcmp(name + len - sl, cryptSuffix) == 0) len -= sl;
  if (len < 4) return false;
  const char *ext = name + len - 4;
  return strncmp(ext, ".gpx", 4) == 0 || strncmp(ext, ".csv", 4) == 0 ||
         strncmp(ext, ".vbo", 4) == 0;
}

// ---- is a firmware image ours? -------------------------------------------------
// Looks for `mark` in a stream fed in chunks of any size, a match split across
// two chunks included. The firmware carries the mark as a string, so any image
// without it was built from something else. Simple restart on a mismatch, so
// `mark`'s first character must not appear in it again.
struct CalcMarkScan {
  size_t at = 0;      // characters of the mark matched so far
  bool   seen = false;
};

static inline void calc_mark_scan(CalcMarkScan *s, const char *mark,
                                  const uint8_t *d, size_t n) {
  const size_t len = strlen(mark);
  for (size_t i = 0; i < n && !s->seen; i++) {
    if (d[i] == (uint8_t)mark[s->at]) s->seen = ++s->at == len;
    else s->at = d[i] == (uint8_t)mark[0] ? 1 : 0;
  }
}

// ---- a request's form values -----------------------------------------------------
// `form` is "a=1&b=x%20y" - a query string, or an x-www-form-urlencoded body.
// The value of `key`, URL-decoded ('+' is a space, %XX a byte), into out
// (cut to outLen - 1). False when the key is absent; a key with no '=' is
// present with "" (an HTML checkbox can send that). Keys compare as sent.
// The web server hands these over raw; WebServer used to decode them.
static inline int calc_hex_digit(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
       : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static inline bool calc_form_value(const char *form, const char *key, char *out, size_t outLen) {
  if (out && outLen) out[0] = '\0';
  if (!form || !key) return false;
  const size_t kl = strlen(key);
  for (const char *p = form; *p;) {
    const char *end = strchr(p, '&');
    if (!end) end = p + strlen(p);
    const char *eq = (const char *)memchr(p, '=', (size_t)(end - p));
    const char *kEnd = eq ? eq : end;
    if ((size_t)(kEnd - p) == kl && strncmp(p, key, kl) == 0) {
      size_t o = 0;
      for (const char *v = eq ? eq + 1 : end; v < end && out && o + 1 < outLen; v++) {
        int h, l;
        if (*v == '+') out[o++] = ' ';
        else if (*v == '%' && end - v >= 3 &&
                 (h = calc_hex_digit(v[1])) >= 0 && (l = calc_hex_digit(v[2])) >= 0) {
          out[o++] = (char)(h * 16 + l);
          v += 2;
        } else {
          out[o++] = *v;
        }
      }
      if (out && outLen) out[o] = '\0';
      return true;
    }
    p = *end ? end + 1 : end;
  }
  return false;
}

// ---- a multipart upload, streamed --------------------------------------------------
// A firmware image arrives as multipart/form-data - the Device page's form
// and the app both send it so - and is far too big to hold: it is passed on
// as it streams in. The boundary is from the Content-Type header
// (calc_multipart_boundary). Only the FIRST part's content is wanted (both
// senders have just the one); the rest of the body is ignored.
//
// The body is "--B\r\n" headers "\r\n\r\n" content "\r\n--B..." - matched as
// a delimiter "\r\n--B" with a "\r\n" taken as read before the body, so the
// first boundary needs no special case. A delimiter split over two chunks,
// and content bytes that only start like one, are handled: a partly matched
// delimiter is held back and handed on when it turns out not to be one.
// Boundaries cannot contain CR, so a mismatch restarts on '\r' alone.
struct CalcMultipart {
  char   delim[80];    // "\r\n--" + boundary
  size_t dlen = 0;
  size_t match = 0;    // delimiter characters matched so far
  int    state = 0;    // 0 preamble, 1 part headers, 2 content, 3 done
  uint32_t hdrTail = 0;   // the last 4 header bytes, for "\r\n\r\n"
};

// The boundary from a Content-Type value: false if it is not multipart or
// has none (or one too long for CalcMultipart). Quotes are taken off.
static inline bool calc_multipart_boundary(const char *ct, char *out, size_t outLen) {
  if (!ct || strncmp(ct, "multipart/", 10) != 0) return false;
  const char *b = strstr(ct, "boundary=");
  if (!b) return false;
  b += 9;
  const bool quoted = *b == '"';
  if (quoted) b++;
  size_t n = 0;
  while (b[n] && (quoted ? b[n] != '"' : (b[n] != ';' && b[n] != ' '))) n++;
  if (n == 0 || n + 1 > outLen) return false;
  memcpy(out, b, n);
  out[n] = '\0';
  return true;
}

static inline bool calc_multipart_begin(CalcMultipart *m, const char *boundary) {
  const size_t bl = strlen(boundary);
  if (bl == 0 || bl + 5 > sizeof(m->delim)) return false;
  memcpy(m->delim, "\r\n--", 4);
  memcpy(m->delim + 4, boundary, bl + 1);
  m->dlen = bl + 4;
  m->match = 2;              // the "\r\n" before the body
  m->state = 0;
  m->hdrTail = 0;
  return true;
}

// Feeds `n` body bytes; `emit(const uint8_t *, size_t)` gets the content.
// True once the content has ended (its closing delimiter seen).
template <typename Emit>
static inline bool calc_multipart_feed(CalcMultipart *m, const uint8_t *d, size_t n, Emit emit) {
  size_t runStart = 0;               // content bytes from here are emitted as a run
  for (size_t i = 0; i < n && m->state != 3; i++) {
    const uint8_t c = d[i];
    if (m->state == 1) {             // part headers, up to the blank line
      m->hdrTail = (m->hdrTail << 8) | c;
      if (m->hdrTail == 0x0D0A0D0Au) { m->state = 2; runStart = i + 1; }
      continue;
    }
    if (c == (uint8_t)m->delim[m->match]) {
      if (m->state == 2 && m->match == 0 && i > runStart) emit(d + runStart, i - runStart);
      if (++m->match == m->dlen) {   // a whole delimiter
        m->match = 0;
        if (m->state == 0) { m->state = 1; m->hdrTail = 0; }
        else m->state = 3;
      }
      runStart = i + 1;
      continue;
    }
    if (m->match > 0) {              // it was not one after all
      if (m->state == 2) {
        // Held back so far: what matched before this pass - the part that
        // came in an earlier chunk, or earlier in this one.
        emit((const uint8_t *)m->delim, m->match);
      }
      m->match = 0;
      runStart = i;
      if (c == (uint8_t)m->delim[0]) { m->match = 1; runStart = i + 1; continue; }
    }
  }
  if (m->state == 2 && m->match == 0 && runStart < n) emit(d + runStart, n - runStart);
  return m->state == 3;
}
