#!/usr/bin/env python3
"""A made-up track session as a GPX, for the lap timer on a desk.

The recorded drives are city loops - traffic lights, a different route each
time - which is not what a track day looks like. This writes one that is: a
closed, smooth ~2.6 km circuit (made up, not a real one), driven at 25 Hz
the way the GNSS module logs, with speeds from the corners (grip, braking,
acceleration), lap after lap:

  an out-lap from standstill, timed laps a few percent apart, one with a
  mistake (a corner taken far too slowly), and a slow in-lap to a stop.

  scripts/make_track_gpx.py [out.gpx]     default data/trip_2026-06-06_10-00_track0.gpx

Then: sim/run.sh data/trip_2026-06-06_10-00_track0.gpx (START on the track
screen), or tests/replay/replay_laps.sh with it.
"""
import math
import random
import sys
from datetime import datetime, timedelta, timezone

OUT = sys.argv[1] if len(sys.argv) > 1 else "data/trip_2026-06-06_10-00_track0.gpx"
START = datetime(2026, 6, 6, 10, 0, 0, tzinfo=timezone.utc)
LAT0, LON0 = 49.80, 15.47      # an open field in the middle of Czechia - no track there
HZ = 25
GRIP = 11.0       # m/s^2 cornering
BRAKE = 9.0       # m/s^2
ACCEL = 4.5       # m/s^2
VMAX = 62.0       # m/s, ~220 km/h
NOISE_M = 0.4     # GNSS position noise, slowly wandering

random.seed(7)    # the same file every time


# ---- the circuit: a closed curve, r(t) around a centre, so it always closes ----
def shape(t):
    r = 420.0 * (1.0 + 0.30 * math.cos(2 * t) + 0.12 * math.sin(3 * t) + 0.05 * math.cos(5 * t + 1.0))
    return r * math.cos(t), 0.75 * r * math.sin(t)


N = 4000
pts = [shape(2 * math.pi * i / N) for i in range(N)]
seg = [math.dist(pts[i], pts[(i + 1) % N]) for i in range(N)]
cum = [0.0]
for d in seg:
    cum.append(cum[-1] + d)
LAP_M = cum[-1]


def radius(i):
    """Radius of curvature at point i, from it and its neighbours 5 apart."""
    a, b, c = pts[(i - 5) % N], pts[i], pts[(i + 5) % N]
    ab, bc, ca = math.dist(a, b), math.dist(b, c), math.dist(c, a)
    cross = abs((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]))
    return float("inf") if cross < 1e-9 else ab * bc * ca / (2 * cross)


# The fastest speed at each point: the corner's limit, then braking into
# corners (backwards) and accelerating out of them (forwards), around twice
# so it is periodic.
vlim = [min(VMAX, math.sqrt(GRIP * radius(i))) for i in range(N)]
v = vlim[:]
for _ in range(2):
    for i in range(2 * N):
        j, k = i % N, (i + 1) % N
        v[k] = min(v[k], math.sqrt(v[j] ** 2 + 2 * ACCEL * seg[j]))
    for i in range(2 * N, 0, -1):
        j, k = i % N, (i - 1) % N
        v[k] = min(v[k], math.sqrt(v[j] ** 2 + 2 * BRAKE * seg[k]))


def at(s):
    """Position (x, y) and ideal speed at s metres along the lap."""
    s %= LAP_M
    lo, hi = 0, N
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if cum[mid] <= s:
            lo = mid
        else:
            hi = mid
    f = (s - cum[lo]) / seg[lo] if seg[lo] else 0.0
    a, b = pts[lo], pts[(lo + 1) % N]
    return a[0] + f * (b[0] - a[0]), a[1] + f * (b[1] - a[1]), v[lo] + f * (v[(lo + 1) % N] - v[lo])


# ---- the session: pace per lap (1.0 = the ideal line), and a mistake ----
PACE = [0.70, 0.93, 0.96, 0.98, 0.97, 0.99, 0.55]   # out-lap ... in-lap
MISTAKE_LAP, MISTAKE_AT, MISTAKE_LEN = 4, 0.55, 180.0   # lap 4 (0-based), 55 % in, 180 m at 45 %

samples = []                 # (t seconds, x, y, speed m/s)
crossings = []               # when the car crossed the line (s = whole laps)
t, s, speed = 0.0, 0.0, 0.0
end = LAP_M * len(PACE) - 300.0   # the in-lap stops short of the line, in the pits
while s < end:
    lap = int(s // LAP_M)
    into = s - lap * LAP_M
    x, y, ideal = at(s)
    target = ideal * PACE[lap]
    if lap == MISTAKE_LAP and MISTAKE_AT * LAP_M <= into < MISTAKE_AT * LAP_M + MISTAKE_LEN:
        target = min(target, 0.45 * ideal)
    if s > end - 150.0:                       # rolling to a stop
        target = min(target, max(0.0, (end - s) / 150.0) * 20.0)
    # The car follows the target within its limits.
    dt = 1.0 / HZ
    if target > speed:
        speed = min(target, speed + ACCEL * dt)
    else:
        speed = max(target, speed - BRAKE * dt)
    samples.append((t, x, y, speed))
    step = max(speed, 0.5) * dt               # never quite standing on the grid
    if int((s + step) // LAP_M) > lap:        # over the line (s = 0) this step
        crossings.append(t + (LAP_M * (lap + 1) - s) / step * dt)
    s += step
    t += dt

# ---- GNSS noise, a slow wander rather than independent jumps ----
nx = ny = 0.0
M_PER_DEG_LAT = 111320.0
M_PER_DEG_LON = 111320.0 * math.cos(math.radians(LAT0))
lines = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<gpx version="1.1" creator="make_track_gpx.py" xmlns="http://www.topografix.com/GPX/1/1"'
    ' xmlns:gpxtpx="http://www.garmin.com/xmlschemas/TrackPointExtension/v2">',
    "<trk><name>Made-up track session</name><trkseg>",
]
for (ts, x, y, sp) in samples:
    nx = 0.98 * nx + random.gauss(0, NOISE_M * 0.2)
    ny = 0.98 * ny + random.gauss(0, NOISE_M * 0.2)
    lat = LAT0 + (y + ny) / M_PER_DEG_LAT
    lon = LON0 + (x + nx) / M_PER_DEG_LON
    when = START + timedelta(seconds=ts)
    stamp = when.strftime("%Y-%m-%dT%H:%M:%S.") + f"{when.microsecond // 10000:02d}Z"
    lines.append(
        f'<trkpt lat="{lat:.7f}" lon="{lon:.7f}"><ele>250.0</ele><time>{stamp}</time>'
        f"<sat>14</sat><hdop>0.7</hdop><extensions><gpxtpx:TrackPointExtension>"
        f"<gpxtpx:speed>{sp:.2f}</gpxtpx:speed></gpxtpx:TrackPointExtension></extensions></trkpt>"
    )
lines.append("</trkseg></trk></gpx>")

with open(OUT, "w") as f:
    f.write("\n".join(lines) + "\n")

print(f"{OUT}: lap {LAP_M:.0f} m, {len(PACE)} laps incl. out/in, {t / 60:.1f} min, {len(samples)} fixes")
# Each lap's true time, line to line, for checking the lap timer against.
for k in range(1, len(crossings)):
    print(f"  lap {k}: {crossings[k] - crossings[k - 1]:.2f} s")
