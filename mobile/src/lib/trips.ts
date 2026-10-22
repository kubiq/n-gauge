// Trip stats from the pod's logs. Pure - text in, numbers out - so it runs
// in node:test against real files.
//
// A drive leaves two files with (nearly) the same start stamp in the name:
// trip_<stamp>.gpx, opened at the first GPS fix, and drive_<stamp>.csv,
// opened at the first CAN frame (log_csv.cpp). They are paired by that stamp.

// ---- GPX -------------------------------------------------------------------

/** [lon, lat, km/h] - lon first, as GeoJSON wants it. */
/**
 * [lon, lat, km/h, seconds from the drive's start] - lon first, as GeoJSON
 * wants it. The seconds (null for a point without a clock) let a moment on
 * the charts be found on the map (pointAt).
 */
export type TrackPoint = [number, number, number, number | null];

export type GpxSummary = {
  /** Epoch ms of the first and last point with a time, or null. */
  start: number | null;
  end: number | null;
  distanceM: number;
  /** Time spent above MOVING_KMH. */
  movingS: number;
  maxKmh: number;
  points: number;
  /** Thinned for drawing: a point at least THIN_M from the previous one. */
  track: TrackPoint[];
  /** [minLon, minLat, maxLon, maxLat], or null without points. */
  bbox: [number, number, number, number] | null;
  /** [lon, lat] of the first and last kept points - what the drive is named after (places.ts). */
  from: [number, number] | null;
  to: [number, number] | null;
};

const MOVING_KMH = 3;
const THIN_M = 10;
// Bad fixes. Logs from before the firmware's own quality gate (sats / hdop,
// late September 2026) have them, with no quality data to tell them by - the
// 23.9. drive ended on a fix 13.5 km away; after a tunnel it sat 2.6 km off
// the road for ten seconds claiming 260 km/h; others start on fixes 150 m
// out that converge, or hold one-point spikes of 60 m. From positions and
// times, then, in two passes:
//  1. the drive is cut into runs wherever two consecutive points jump over
//     BREAK_M at more than GLITCH_KMH (so 25 Hz jitter of a few metres does
//     not cut it), at a new <trkseg>, and across a reception gap of GAP_MS;
//  2. runs shorter than RUN_MIN_MS that a jump borders are dropped - spikes,
//     a bad stretch, a bad end, a converging start - but never the longest
//     run. What is kept makes the map, the bounds, the ends, the distance and
//     the top speed.
// Distance is summed between consecutive kept points of a trkseg when the
// step is plausible, so it still bridges a tunnel's gap.
const GLITCH_KMH = 300;
const BREAK_M = 30;
const GAP_MS = 20_000;
const RUN_MIN_MS = 30_000;

const R = 6371008.8;
export function distM(lat1: number, lon1: number, lat2: number, lon2: number): number {
  const rad = Math.PI / 180;
  const dLat = (lat2 - lat1) * rad;
  const dLon = (lon2 - lon1) * rad;
  const a = Math.sin(dLat / 2) ** 2 + Math.cos(lat1 * rad) * Math.cos(lat2 * rad) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(a)));
}

// Matches the trkpt line log_gpx.cpp writes. <time> is empty while the clock
// is unknown. The speed is in the gpxtpx extension; files from before
// 2026-09-23 have a bare <speed> instead, and some have none.
const TRKPT =
  /<trkpt lat="(-?[\d.]+)" lon="(-?[\d.]+)">(?:<ele>[^<]*<\/ele>)?<time>([^<]*)<\/time>.*?(?:<(?:gpxtpx:)?speed>([\d.]+)<\/(?:gpxtpx:)?speed>|<\/trkpt>)/g;

export function parseGpx(text: string): GpxSummary {
  // Pass 0: every point, with its trkseg - but one per FIX. A point at
  // exactly the previous one's position is the same fix written again: the
  // gauge writes points every 40 ms whether or not the receiver has moved on,
  // and a receiver at 1 Hz (the carrier PCB, 2026-10-19..20: 95 % repeats)
  // gave runs of 25 copies. Kept, the first new fix after a run came 40 ms
  // after the last copy and a whole second's travel away - 500 m/s, a
  // "glitch" - so nearly every step was thrown out: 2 km for a 14 km drive.
  // The first copy keeps its time (the step is measured from it); the last
  // copy's time is kept as tEnd, so a drive ending parked keeps its end.
  type Pt = { lat: number; lon: number; t: number; tEnd: number; kmh: number; seg: number };
  const pts: Pt[] = [];
  text.split('<trkseg>').forEach((seg, si) => {
    TRKPT.lastIndex = 0;
    for (let m = TRKPT.exec(seg); m; m = TRKPT.exec(seg)) {
      const lat = +m[1], lon = +m[2], t = m[3] ? Date.parse(m[3]) : NaN;
      const last = pts[pts.length - 1];
      if (last && last.seg === si && last.lat === lat && last.lon === lon) {
        if (!Number.isNaN(t)) last.tEnd = t;
        continue;
      }
      pts.push({ lat, lon, t, tEnd: t, kmh: m[4] ? +m[4] * 3.6 : NaN, seg: si });
    }
  });
  // Plausible to go from a to b? Without times (no clock yet) it cannot be
  // judged, so it is.
  const plausible = (a: Pt, b: Pt, d: number) => {
    if (Number.isNaN(a.t) || Number.isNaN(b.t)) return true;
    const dt = (b.t - a.t) / 1000;
    return dt > 0 ? (d / dt) * 3.6 <= GLITCH_KMH : d <= BREAK_M;
  };

  // Pass 1: runs. A cut is a JUMP (a step the car could not have made, even
  // across a gap) or a plain GAP (a new trkseg, a reception gap); a short run
  // goes only if a jump borders it - a short stretch between two tunnels is
  // real.
  type Run = { i: number; j: number; jumpBefore: boolean; jumpAfter: boolean };
  const runs: Run[] = [];
  let from = 0;
  let jumpBefore = false;
  for (let k = 1; k <= pts.length; k++) {
    const a = pts[k - 1], b = pts[k];
    let jump = false, gap = !b;
    if (b) {
      const d = distM(a.lat, a.lon, b.lat, b.lon);
      jump = d > BREAK_M && !plausible(a, b, d);
      gap = b.seg !== a.seg || b.t - a.t > GAP_MS;
    }
    if (jump || gap) {
      runs.push({ i: from, j: k - 1, jumpBefore, jumpAfter: jump });
      from = k;
      jumpBefore = jump;
    }
  }
  const length = (r: Run) => (Number.isNaN(pts[r.j].t - pts[r.i].t) ? Infinity : pts[r.j].t - pts[r.i].t);
  let longest = 0;
  runs.forEach((r, k) => {
    if (length(r) > length(runs[longest])) longest = k;
  });
  const keep = runs
    .filter((r, k) => k === longest || length(r) >= RUN_MIN_MS || !(r.jumpBefore || r.jumpAfter))
    .map((r): [number, number] => [r.i, r.j]);

  // Pass 2: the stats, from the kept points only.
  const s: GpxSummary = {
    start: null, end: null, distanceM: 0, movingS: 0, maxKmh: 0, points: pts.length, track: [], bbox: null, from: null, to: null,
  };
  let prev: Pt | undefined;
  let kept: Pt | undefined;   // the last point that went into the track
  for (const [i, j] of keep) {
    for (let k = i; k <= j; k++) {
      const p = pts[k];
      if (!Number.isNaN(p.t)) {
        if (s.start === null) s.start = p.t;
        s.end = p.tEnd;
      }
      // No distance across a trkseg; across a dropped run or a gap, only a
      // step the car could have made.
      if (prev && prev.seg === p.seg) {
        const d = distM(prev.lat, prev.lon, p.lat, p.lon);
        if (plausible(prev, p, d)) {
          s.distanceM += d;
          const dt = (p.t - prev.t) / 1000;
          if (dt > 0 && dt < 10 && (Number.isNaN(p.kmh) ? (d / dt) * 3.6 : p.kmh) > MOVING_KMH) s.movingS += dt;
        }
      }
      if (!Number.isNaN(p.kmh) && p.kmh > s.maxKmh) s.maxKmh = p.kmh;
      // Thinned for drawing, and each run's last point, so the line reaches it.
      if (!kept || k === j || distM(kept.lat, kept.lon, p.lat, p.lon) >= THIN_M) {
        if (kept !== p) {
          const sec = s.start !== null && !Number.isNaN(p.t) ? Math.round((p.t - s.start) / 100) / 10 : null;
          s.track.push([p.lon, p.lat, Number.isNaN(p.kmh) ? 0 : Math.round(p.kmh), sec]);
        }
        kept = p;
      }
      prev = p;
    }
  }
  // From the kept track - a loop, not Math.min(...): a long drive is too
  // many arguments for some engines.
  for (const [lon, lat] of s.track) {
    if (!s.bbox) s.bbox = [lon, lat, lon, lat];
    else s.bbox = [Math.min(s.bbox[0], lon), Math.min(s.bbox[1], lat), Math.max(s.bbox[2], lon), Math.max(s.bbox[3], lat)];
  }
  if (s.track.length) {
    const a = s.track[0], b = s.track[s.track.length - 1];
    s.from = [a[0], a[1]];
    s.to = [b[0], b[1]];
  }
  return s;
}

/** Further than this in time from every track point: no dot (a tunnel, a dropped stretch). */
const POINT_MAX_S = 10;
/** Two track points this close, far apart in time: the car stood between them. */
const STILL_M = 30;

/**
 * The track point nearest `sec` (seconds from the drive's start) - where the
 * car was at a moment picked on the charts - or null if no point is within
 * POINT_MAX_S. Binary search: the track is in time order.
 */
export function pointAt(track: TrackPoint[], sec: number): TrackPoint | null {
  const timed = track.filter((p) => p[3] !== null);
  let lo = 0, hi = timed.length - 1;
  if (hi < 0) return null;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (timed[mid][3]! < sec) lo = mid + 1;
    else hi = mid;
  }
  // lo: the first at or after `sec`; the one before may be nearer.
  const before = lo > 0 ? timed[lo - 1] : null, after = timed[lo];
  // Between two points close together the car stood still - the track keeps
  // a point only every THIN_M moved - so it was at the earlier one, however
  // long ago that is. Far apart, it is a gap (a tunnel): only near a point.
  if (before && after[3]! > sec && distM(before[1], before[0], after[1], after[0]) < STILL_M) return before;
  // After the track's end - the car log runs on while parked - it is there.
  if (sec > after[3]!) return after;
  const best = before && Math.abs(before[3]! - sec) <= Math.abs(after[3]! - sec) ? before : after;
  return Math.abs(best[3]! - sec) <= POINT_MAX_S ? best : null;
}

// ---- CSV -------------------------------------------------------------------

export type CsvSummary = {
  rows: number;
  /** From the `ms` column (uptime), so it is there even without a clock. */
  durationS: number;
  maxBoostBar: number | null;
  maxOilC: number | null;
  maxCoolantC: number | null;
  maxRpm: number | null;
  maxObdKmh: number | null;
  /** The oil temperature at the end of the drive. */
  lastOilC: number | null;
  /**
   * Fuel burned, litres: the car's fuel rate (fuelLh, PID 0x5E) over time.
   * Not the tank level - it sloshes by tens of percent while driving, and 1 %
   * of the tank is a third of a short trip. Null without the column.
   */
  fuelL: number | null;
  /**
   * Distance by the car's own speed (obdKmh) over the same rows, km - what
   * fuelL is divided by. Not the GPS distance: the track starts at the first
   * fix, often minutes after the CSV, and that stretch's fuel would count
   * against no distance.
   */
  fuelKm: number | null;
  /**
   * The tank level (%) at the start and the end of the drive, with the engine
   * running (fuelLevelOf); null without enough readings.
   */
  fuelStartPct: number | null;
  fuelEndPct: number | null;
  /** Fill-ups during the drive: the engine stopped, and started on more fuel. */
  refuels: Refuel[];
};

export type Refuel = {
  /** When the engine stopped (epoch ms, from the utc column), or null. */
  at: number | null;
  /** Where ([lon, lat], as the GPX ends are), or null without a fix. */
  pt: [number, number] | null;
  fromPct: number;
  toPct: number;
  /** The place's name from the lists, filled in when cached (trip-cache.ts). */
  place?: string | null;
};

// The tank level, for fill-ups. Read only with the engine running: with it off
// the level reads ~20 points high (parked without filling, 2026-10-18: 44 %
// running, 66 % just after; 2026-10-22: 31 % off, 10.6 % on the next start) -
// it is measured against the supply, which drops when the alternator stops.
// Running, it sloshes 8-15 % at a low level, so a level is the median of a
// few minutes, and a fill-up is a rise of FILL_MIN_PCT across a stop: the
// largest change that was not one, in 30 drives, was 9 points.
const RUNNING_RPM = 300;
const STOP_MIN_MS = 30_000;
const LEVEL_WINDOW_MS = 180_000;
const LEVEL_MIN_READINGS = 20;
export const FILL_MIN_PCT = 20;

/** The median of the readings within LEVEL_WINDOW_MS of one end; null with too few. */
function fuelLevelOf(pts: [number, number][], fromEnd: boolean): number | null {
  if (!pts.length) return null;
  const edge = (fromEnd ? pts[pts.length - 1] : pts[0])[0];
  const v = pts.filter(([ms]) => Math.abs(ms - edge) <= LEVEL_WINDOW_MS).map(([, p]) => p);
  if (v.length < LEVEL_MIN_READINGS) return null;
  v.sort((a, b) => a - b);
  return v[v.length >> 1];
}

// A row's rate and speed hold until the next row; a longer gap than this (a
// dropout, the logger paused) counts as nothing rather than as a guess.
const INTEGRATE_MAX_GAP_MS = 5000;

// The header has grown over time (log_csv.cpp), so columns are found by name
// and a column an older file lacks just yields null.
export function parseCsv(text: string): CsvSummary {
  // CRLF, as the gauge writes them - see series.ts: "\r" left in the last
  // column made an empty value 0.
  const lines = text.split(/\r?\n/);
  const head = (lines[0] ?? '').trim().split(',');
  const col = (n: string) => head.indexOf(n);
  const iMs = col('ms'), iBoost = col('boostBar'), iOil = col('oilC'), iCool = col('coolantC');
  const iRpm = col('rpm'), iKmh = col('obdKmh'), iFuel = col('fuelLh');
  const iPct = col('fuelPct'), iUtc = col('utc'), iLat = col('lat'), iLon = col('lon');
  const s: CsvSummary = {
    rows: 0, durationS: 0, maxBoostBar: null, maxOilC: null, maxCoolantC: null, maxRpm: null, maxObdKmh: null, lastOilC: null,
    fuelL: null, fuelKm: null, fuelStartPct: null, fuelEndPct: null, refuels: [],
  };
  // Engine-running stretches, split where it was off STOP_MIN_MS or more:
  // their tank readings [ms, %], and when and where each ended.
  type Run = { pts: [number, number][]; endMs: number; at: number | null; pt: [number, number] | null };
  const runs: Run[] = [];
  let run: Run | null = null;
  let lastPt: [number, number] | null = null;
  let firstMs = NaN, lastMs = NaN;
  // The previous row's uptime, fuel rate and speed, for the integrals.
  let prevMs = NaN, prevLh = NaN, prevKmh = NaN;
  const num = (f: string[], i: number) => (i >= 0 && f[i] ? +f[i] : NaN);
  const max = (cur: number | null, f: string[], i: number) => {
    if (i < 0 || !f[i]) return cur;
    const v = +f[i];
    return Number.isFinite(v) && (cur === null || v > cur) ? v : cur;
  };
  for (let k = 1; k < lines.length; k++) {
    const line = lines[k];
    if (!line) continue;
    const f = line.split(',');
    s.rows++;
    const ms = +f[iMs];
    if (iMs >= 0 && f[iMs] && Number.isFinite(ms)) {
      if (Number.isNaN(firstMs)) firstMs = ms;
      lastMs = ms;
    }
    s.maxBoostBar = max(s.maxBoostBar, f, iBoost);
    s.maxOilC = max(s.maxOilC, f, iOil);
    s.maxCoolantC = max(s.maxCoolantC, f, iCool);
    s.maxRpm = max(s.maxRpm, f, iRpm);
    s.maxObdKmh = max(s.maxObdKmh, f, iKmh);
    if (iOil >= 0 && f[iOil] && Number.isFinite(+f[iOil])) s.lastOilC = +f[iOil];

    const lh = num(f, iFuel), kmh = num(f, iKmh);
    if (Number.isFinite(lh) && s.fuelL === null) s.fuelL = 0;
    if (Number.isFinite(kmh) && s.fuelKm === null) s.fuelKm = 0;
    const dt = ms - prevMs;              // NaN without either; negative after a restart
    if (dt > 0 && dt <= INTEGRATE_MAX_GAP_MS) {
      const h = dt / 3_600_000;
      if (Number.isFinite(prevLh)) s.fuelL! += prevLh * h;
      if (Number.isFinite(prevKmh)) s.fuelKm! += prevKmh * h;
    }
    if (Number.isFinite(ms)) { prevMs = ms; prevLh = lh; prevKmh = kmh; }

    const lat = num(f, iLat), lon = num(f, iLon);
    if (Number.isFinite(lat) && Number.isFinite(lon) && (lat || lon)) lastPt = [lon, lat];
    const rpm = num(f, iRpm);
    if (Number.isFinite(ms) && rpm >= RUNNING_RPM) {
      // A new stretch after a stop - or after a reboot, when ms starts again.
      if (!run || ms - run.endMs >= STOP_MIN_MS || ms < run.endMs) {
        run = { pts: [], endMs: ms, at: null, pt: null };
        runs.push(run);
      }
      run.endMs = ms;
      const utc = iUtc >= 0 && f[iUtc] ? Date.parse(f[iUtc]) : NaN;
      run.at = Number.isFinite(utc) ? utc : null;
      run.pt = lastPt;
      const pct = num(f, iPct);
      if (Number.isFinite(pct)) run.pts.push([ms, pct]);
    }
  }
  // uptime ms can restart if the pod rebooted mid-drive; never negative.
  if (!Number.isNaN(firstMs)) s.durationS = Math.max(0, (lastMs - firstMs) / 1000);
  const levels = runs.map((r) => ({ r, head: fuelLevelOf(r.pts, false), tail: fuelLevelOf(r.pts, true) }));
  s.fuelStartPct = levels.find((l) => l.head !== null)?.head ?? null;
  s.fuelEndPct = levels.findLast((l) => l.tail !== null)?.tail ?? null;
  // Each stretch's start against the last level before it.
  let before: (typeof levels)[number] | null = null;
  for (const l of levels) {
    if (before && l.head !== null && l.head - before.tail! >= FILL_MIN_PCT) {
      s.refuels.push({ at: before.r.at, pt: before.r.pt, fromPct: before.tail!, toPct: l.head });
    }
    if (l.tail !== null) before = l;
  }
  return s;
}

/**
 * Litres per 100 km for a drive, or null when it says nothing: no fuel
 * column, or under a kilometre (a short hop at idle reads absurd figures).
 */
export function litresPer100(c: Pick<CsvSummary, 'fuelL' | 'fuelKm'>): number | null {
  if (c.fuelL == null || c.fuelKm == null || c.fuelKm < 1) return null;
  return (c.fuelL / c.fuelKm) * 100;
}

// ---- pairing ---------------------------------------------------------------

/**
 * Start time from a dated name, or null. The name is LOCAL time (log_names.cpp
 * names files with localtime_r, in the pod's TZ setting) - unlike the GPX's
 * <time>, which is UTC. Read as the phone's local time, which is the pod's
 * as long as both are set to the same zone.
 */
export function stampOf(name: string): number | null {
  const m = /_(\d{4})-(\d{2})-(\d{2})_(\d{2})-(\d{2})(?:_\d)?_[0-9a-f]{6}\./.exec(name);
  if (!m) return null;
  return new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5]).getTime();
}

// ---- names -------------------------------------------------------------------
// <kind>_<when>_<session>.<ext>[.nge] - "trip_2026-10-12_21-19_3fa81c.gpx".
// <when> is the start (or a number, before the clock was known); <session>
// is the pod's boot or wake that wrote it (log_names.cpp), shared by a drive's
// GPX, CSV and VBO. An encrypted log is its plain name plus ".nge" (logcrypt.h).
export const isEncryptedName = (n: string) => n.endsWith('.nge');
export const plainName = (n: string) => (isEncryptedName(n) ? n.slice(0, -4) : n);
export const isGpxName = (n: string) => /^trip_.+\.gpx(?:\.nge)?$/.test(n);
export const isCsvName = (n: string) => /^drive_.+\.csv(?:\.nge)?$/.test(n);
export const isVboName = (n: string) => /^track_.+\.vbo(?:\.nge)?$/.test(n);
export const sessionOf = (n: string) => /_([0-9a-f]{6})\.(?:gpx|csv|vbo)(?:\.nge)?$/.exec(n)?.[1] ?? null;

export type TripFiles = { key: string; gpx?: string; csv?: string; start: number | null };

/**
 * The names you gave drives (keyed by a file's name: the GPX, or the CSV
 * without one) that no file on the phone answers to any more - drives
 * deleted, and a test's leftover "zz-test". Only those: a key whose file is
 * still here keeps its name, even when the drive's key has moved on (its
 * CSV came first and was named, the GPX after).
 */
export function staleTitleKeys(titles: Record<string, string>, trips: TripFiles[]): string[] {
  const files = new Set(trips.flatMap((t) => [t.gpx, t.csv]).filter((n): n is string => !!n));
  return Object.keys(titles).filter((k) => !files.has(k));
}

/**
 * Not a drive worth a row: a restart fragment or the engine on for a moment
 * (2026-10-12: "21:18, 0.0 km, 1 min, top 2 km/h"). With a track: under
 * 200 m and 3 minutes. Car data only: under a minute. Without a summary yet,
 * never - a drive must not vanish while it is still being read.
 */
export function isStub(g?: { distanceM: number; start: number | null; end: number | null }, c?: { durationS: number | null }): boolean {
  if (g) {
    const secs = g.start && g.end ? (g.end - g.start) / 1000 : Infinity;
    return g.distanceM < 200 && secs < 180;
  }
  return c?.durationS != null && c.durationS < 60;
}

/**
 * One entry per drive, newest first: each GPX with the CSV of its session,
 * and every CSV left over on its own. A session can hold more than one of
 * each (a card share or a key change starts new files): they pair in order.
 * Undated (numbered) files come last.
 */
// A log opened before the clock was set is numbered (drive_0001_3082dd.csv)
// and renamed to its dated name once it is (log_csv.cpp) - the same file. A
// phone that synced it before the rename holds both: the numbered copy is
// the dated file's beginning. Found 2026-10-15: the GPX paired with that
// 35 KB copy, the whole 1.7 MB drive log stood alone as a second drive.
const LOG_PARTS = /^(drive|trip|track)_(.+)_([0-9a-f]{6})\.((?:csv|gpx|vbo)(?:\.nge)?)$/;

/**
 * Numbered logs whose dated namesake - same kind, session and extension -
 * is at least as big: [numbered, dated] pairs, each to be checked by its
 * bytes (trip-cache.ts dropStaleCopies) before the numbered one goes.
 */
export function staleCopyCandidates(names: string[], size: (n: string) => number): [string, string][] {
  const parts = names.map((n) => [n, LOG_PARTS.exec(n)] as const).filter((x): x is [string, RegExpExecArray] => !!x[1]);
  const dated = parts.filter(([, m]) => m[2].includes('-'));
  const out: [string, string][] = [];
  for (const [n, m] of parts) {
    if (!/^\d+$/.test(m[2])) continue;
    const d = dated.find(([, dm]) => dm[1] === m[1] && dm[3] === m[3] && dm[4] === m[4]);
    if (d && size(d[0]) >= size(n)) out.push([n, d[0]]);
  }
  return out;
}

export function pairTrips(names: string[]): TripFiles[] {
  const sorted = [...names].sort();
  const csvs = new Map<string, string[]>();
  for (const c of sorted.filter(isCsvName)) {
    const s = sessionOf(c) ?? c;
    csvs.set(s, [...(csvs.get(s) ?? []), c]);
  }
  const out: TripFiles[] = sorted
    .filter(isGpxName)
    .map((g) => ({ key: g, gpx: g, csv: csvs.get(sessionOf(g) ?? g)?.shift(), start: stampOf(g) }));
  for (const c of [...csvs.values()].flat()) out.push({ key: c, csv: c, start: stampOf(c) });
  return out.sort((a, b) => (b.start ?? -Infinity) - (a.start ?? -Infinity) || b.key.localeCompare(a.key));
}
