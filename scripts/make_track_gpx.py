#!/usr/bin/env python3
"""A made-up track session as a GPX and a drive log, for the gauge on a desk.

The recorded drives are city loops - traffic lights, a different route each
time - which is not what a track day looks like. This writes one that is: a
closed, smooth ~2.7 km circuit (made up, not a real one), driven at 25 Hz
the way the GNSS module logs, with speeds from the corners (grip, braking,
acceleration), lap after lap:

  an out-lap from standstill, timed laps a few percent apart, one with a
  mistake (a corner taken far too slowly), and a slow in-lap to a stop.

And the car's side of it, as the gauge logs it (2 Hz, its CSV columns):
gear and rpm from the speed, pedal, load and boost from accelerating or
not, oil and coolant warming through the session, tyres heating up, and
the g-forces of the corners and the braking.

  scripts/make_track_gpx.py [out.gpx]     default data/trip_2026-06-06_10-00_track0.gpx
                                          and its data/drive_..._track0.csv

Then: sim/run.sh data/drive_2026-06-06_10-00_track0.csv
data/trip_2026-06-06_10-00_track0.gpx (START on the track screen), or
tests/replay/replay_laps.sh with the GPX.
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


def curvature(i):
    """Signed curvature (1/m, + turning left) at point i, from it and its
    neighbours 5 apart."""
    a, b, c = pts[(i - 5) % N], pts[i], pts[(i + 5) % N]
    ab, bc, ca = math.dist(a, b), math.dist(b, c), math.dist(c, a)
    cross = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    return 2 * cross / (ab * bc * ca) if ab * bc * ca > 0 else 0.0


def radius(i):
    k = abs(curvature(i))
    return float("inf") if k < 1e-9 else 1.0 / k


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


def kappa_at(s):
    """Signed curvature at s metres along the lap."""
    s %= LAP_M
    lo, hi = 0, N
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if cum[mid] <= s:
            lo = mid
        else:
            hi = mid
    return curvature(lo)


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

samples = []                 # (t s, x, y, speed m/s, accel m/s^2, curvature 1/m)
crossings = []               # when the car crossed the line (s = whole laps)
t, s, speed = 0.0, 0.0, 0.0
# Standing in the pit lane first, engine running: the gauge zeroes its
# g-meter on a car that is still - a session starting at full throttle would
# zero it on the launch.
x0, y0, _ = at(0.0)
while t < 20.0:
    samples.append((t, x0, y0, 0.0, 0.0, 0.0))
    t += 1.0 / HZ
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
    was = speed
    if target > speed:
        speed = min(target, speed + ACCEL * dt)
    else:
        speed = max(target, speed - BRAKE * dt)
    samples.append((t, x, y, speed, (speed - was) / dt, kappa_at(s)))
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
fixes = []                   # (lat, lon) as written, for the drive log
for (ts, x, y, sp, _, _) in samples:
    nx = 0.98 * nx + random.gauss(0, NOISE_M * 0.2)
    ny = 0.98 * ny + random.gauss(0, NOISE_M * 0.2)
    lat = LAT0 + (y + ny) / M_PER_DEG_LAT
    lon = LON0 + (x + nx) / M_PER_DEG_LON
    fixes.append((lat, lon))
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

# ---- the drive log: the car, as the gauge logs it (its CSV, 2 Hz) ----
# Columns by name, as the gauge writes them; the sim and the app read them by
# name, so only the ones that mean something here are filled.
GEAR_TOP_KMH = [0, 50, 85, 120, 160, 200, 250]   # each gear's speed at 6500 rpm
COLS = ["utc", "ms", "oilC", "coolantC", "boostBar", "gpsFix", "sats", "speedKmh", "lat", "lon",
        "gx", "gy", "gz", "t0bar", "t1bar", "t2bar", "t3bar", "t0c", "t1c", "t2c", "t3c",
        "obdKmh", "absLoadPct", "baroKpa", "pedalRaw", "rpm", "timingDeg", "iatC", "gear", "volts"]
UPTIME_MS = 20000                                  # the gauge booted 20 s before
total = samples[-1][0]
rows = [",".join(COLS)]
for i in range(0, len(samples), HZ // 2):
    ts, _, _, sp, acc, k = samples[i]
    lat, lon = fixes[i]
    kmh = sp * 3.6
    heat = ts / total                              # 0 -> 1 through the session
    gear = next((g for g in range(1, 7) if kmh < GEAR_TOP_KMH[g] * 0.92), 6)
    rpm = max(850.0, kmh / GEAR_TOP_KMH[gear] * 6500.0)
    if kmh < 1.0:
        gear = 0                                   # standing: neutral, idling
    # Accelerating hard: pedal down, boost up with rpm; braking: off it.
    push = max(0.0, min(1.0, acc / ACCEL)) if acc > 0.05 else (0.25 if abs(acc) <= 0.05 and sp > 1 else 0.0)
    boost = -0.6 + push * (0.6 + 1.5 * min(1.0, rpm / 4000.0))
    load = 15.0 + push * 85.0
    pedal = 37 + int(push * 190)
    timing = 8.0 + (1.0 - push) * 22.0
    lateral = sp * sp * k / 9.81                   # + to the left
    longitudinal = acc / 9.81
    tyreBar = 2.40 + 0.35 * heat
    tyreC = 24 + 30 * heat
    when = START + timedelta(seconds=ts)
    vals = {
        "utc": when.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "ms": UPTIME_MS + int(round(ts * 1000)),
        # Oil past the gauge's 120 C warning by the last laps, as on a real
        # track day (2026-10-17: 132 C) - the over-temperature screenshot.
        "oilC": int(round(92 + 34 * heat)), "coolantC": int(round(88 + 8 * heat)),
        "boostBar": f"{boost:.2f}", "gpsFix": 1, "sats": 14, "speedKmh": f"{kmh:.1f}",
        "lat": f"{lat:.7f}", "lon": f"{lon:.7f}",
        # The pod stands with its x axis up (as the real logs show): gravity
        # on x, the car's longitudinal and lateral g on the other two.
        "gx": f"{1.0 + random.gauss(0, 0.01):.3f}",
        "gy": f"{lateral + random.gauss(0, 0.02):.3f}",
        "gz": f"{longitudinal + random.gauss(0, 0.02):.3f}",
        "t0bar": f"{tyreBar + 0.02:.2f}", "t1bar": f"{tyreBar + 0.03:.2f}",
        "t2bar": f"{tyreBar - 0.08:.2f}", "t3bar": f"{tyreBar - 0.07:.2f}",
        "t0c": int(tyreC + 3), "t1c": int(tyreC + 4), "t2c": int(tyreC), "t3c": int(tyreC),
        "obdKmh": int(kmh), "absLoadPct": f"{load:.1f}", "baroKpa": 98, "pedalRaw": pedal,
        "rpm": int(rpm), "timingDeg": f"{timing:.1f}", "iatC": int(28 + 10 * heat), "gear": gear,
        "volts": f"{14.3 + random.gauss(0, 0.05):.2f}",
    }
    rows.append(",".join(str(vals[c]) for c in COLS))

OUT_CSV = OUT.replace("trip_", "drive_").replace(".gpx", ".csv")
with open(OUT_CSV, "w") as f:
    f.write("\n".join(rows) + "\n")
print(f"{OUT_CSV}: {len(rows) - 1} rows")

print(f"{OUT}: lap {LAP_M:.0f} m, {len(PACE)} laps incl. out/in, {t / 60:.1f} min, {len(samples)} fixes")
# Each lap's true time, line to line, for checking the lap timer against.
for k in range(1, len(crossings)):
    print(f"  lap {k}: {crossings[k] - crossings[k - 1]:.2f} s")
