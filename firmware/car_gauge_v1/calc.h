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
// prof[i] is the lap time, in 10 ms units, at i * stepM along it (prof[0] =
// 0, at the line). One step of the lap, from (d0 m, t0 ms) to (d1, t1),
// fills the marks it passes - each at its interpolated time - after the n
// already filled, up to cap. Returns the new count.
static inline uint16_t calc_lap_prof_add(uint16_t *prof, uint16_t n, uint16_t cap, float stepM,
                                         float d0, uint32_t t0Ms, float d1, uint32_t t1Ms) {
  if (!(d1 > d0)) return n;
  while (n < cap && n * stepM <= d1) {
    float f = (n * stepM - d0) / (d1 - d0);
    if (f < 0.0f) f = 0.0f;   // a mark behind the step (never filled): its start
    const float t = (t0Ms + f * (float)(t1Ms - t0Ms)) / 10.0f;
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
  return (int32_t)lroundf((prof[i] + f * ((float)prof[i + 1] - (float)prof[i])) * 10.0f);
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
// maybe with the encrypted suffix, and sleeplog.txt.
static inline bool calc_safe_name(const char *s, size_t len) {
  if (!s || len == 0 || len > 40) return false;
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
