// ============================================================================
// loopfind.h  —  finding a circuit's start/finish line by itself: loop closure.
//
// So nobody has to tap the pod while driving, or stop on a track to set the
// line. From START (tapped anywhere, the pit included) a breadcrumb is
// dropped every LF_SPACING_M driven. Once the car has covered LF_MIN_LOOP_M,
// each new breadcrumb is compared with the older ones: within LF_RADIUS_M AND
// heading the same way (within LF_HEADING_DEG) means the car is back on a
// path it already drove - a closed loop. The matched breadcrumb becomes the
// gate (its position, the line across its heading), and lap 1 is timed from
// when the car first passed it.
//
// Why this handles the pit: the path from the pit to the circuit is never
// driven again, so it never matches - the first match is where the pit exit
// joins the loop, and everything before it is the out-lap by construction.
// The heading test keeps a figure-8 crossover, or an out-and-back road, from
// counting: there the car passes the same point at an angle, or backwards.
//
// The line lands where the loop first closes - often just after pit exit -
// not on the official start/finish: laps are consistent, the start offset.
//
// Pure (no Arduino, no GPS): positions come in as local metres, so tests/
// can drive it along synthetic circuits.
// ============================================================================
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define LF_SPACING_M    10.0f   // a breadcrumb per this much driven: ~5 KB a 4 km lap
#define LF_RADIUS_M     20.0f   // "the same place": track width plus GNSS error
#define LF_MIN_LOOP_M   500.0f  // shortest loop: below this it is a roundabout, a U-turn
#define LF_HEADING_DEG  30.0f   // "the same way"

struct LfCrumb {
  float    x, y;       // local metres (east, north) from the first fix
  float    hx, hy;     // unit heading arriving here (from the previous crumb)
  float    dist;       // path length driven to here, metres
  uint32_t fixMs;      // the fix's UTC ms of day - when the car was here
};

struct LoopFinder {
  LfCrumb *crumbs;     // caller's storage (PSRAM on the pod)
  uint16_t cap, n;
  float    lastX, lastY, dist;
};

static inline void lf_init(LoopFinder *f, LfCrumb *buf, uint16_t cap) {
  f->crumbs = buf;
  f->cap    = cap;
  f->n      = 0;
  f->lastX = f->lastY = f->dist = 0.0f;
}

#define LF_NONE  (-1)   // nothing yet
#define LF_FULL  (-2)   // out of breadcrumbs without finding a loop: give up

// One fix. Returns the index of the matched breadcrumb when this fix closes
// the loop, LF_NONE otherwise, or LF_FULL once the storage is used up.
static inline int lf_add(LoopFinder *f, float x, float y, uint32_t fixMs) {
  if (f->n == 0) {
    LfCrumb c = {x, y, 0.0f, 0.0f, 0.0f, fixMs};
    f->crumbs[f->n++] = c;
    f->lastX = x;
    f->lastY = y;
    return LF_NONE;
  }
  const float dx = x - f->lastX, dy = y - f->lastY;
  const float d = sqrtf(dx * dx + dy * dy);
  if (d < LF_SPACING_M) return LF_NONE;     // standing still, or not far enough yet
  if (f->n >= f->cap) return LF_FULL;

  const float hx = dx / d, hy = dy / d;
  f->dist += d;
  LfCrumb c = {x, y, hx, hy, f->dist, fixMs};
  // The first crumb had no direction to arrive from: give it the second's.
  if (f->n == 1) { f->crumbs[0].hx = hx; f->crumbs[0].hy = hy; }
  f->crumbs[f->n++] = c;
  f->lastX = x;
  f->lastY = y;

  // Closed? The nearest older crumb that is close, at least a loop's length
  // back along the path, and was passed in the same direction.
  const float cosMax = cosf(LF_HEADING_DEG * (float)M_PI / 180.0f);
  int best = LF_NONE;
  float bestD2 = LF_RADIUS_M * LF_RADIUS_M;
  for (uint16_t i = 0; i + 1 < f->n; i++) {
    const LfCrumb &o = f->crumbs[i];
    if (f->dist - o.dist < LF_MIN_LOOP_M) break;   // crumbs only get nearer along the path
    const float ex = x - o.x, ey = y - o.y;
    const float d2 = ex * ex + ey * ey;
    if (d2 > bestD2) continue;
    if (o.hx * hx + o.hy * hy < cosMax) continue;   // passed at an angle, or the other way
    best = (int)i;
    bestD2 = d2;
  }
  return best;
}
