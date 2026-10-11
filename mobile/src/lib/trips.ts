// Trip stats from the pod's logs. Pure - text in, numbers out - so it runs
// in node:test against real files.
//
// A drive leaves two files with (nearly) the same start stamp in the name:
// trip_<stamp>.gpx, opened at the first GPS fix, and drive_<stamp>.csv,
// opened at the first CAN frame (logger.cpp). They are paired by that stamp.

// ---- GPX -------------------------------------------------------------------

/** [lon, lat, km/h] - lon first, as GeoJSON wants it. */
export type TrackPoint = [number, number, number];

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
};

const MOVING_KMH = 3;
const THIN_M = 10;
// Faster than this between two points is a position glitch, not driving.
const GLITCH_KMH = 400;

const R = 6371008.8;
export function distM(lat1: number, lon1: number, lat2: number, lon2: number): number {
  const rad = Math.PI / 180;
  const dLat = (lat2 - lat1) * rad;
  const dLon = (lon2 - lon1) * rad;
  const a = Math.sin(dLat / 2) ** 2 + Math.cos(lat1 * rad) * Math.cos(lat2 * rad) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(a)));
}

// Matches the trkpt line logger.cpp writes. <time> is empty while the clock
// is unknown, and older files may lack the speed extension.
const TRKPT =
  /<trkpt lat="(-?[\d.]+)" lon="(-?[\d.]+)">(?:<ele>[^<]*<\/ele>)?<time>([^<]*)<\/time>.*?(?:<gpxtpx:speed>([\d.]+)<\/gpxtpx:speed>|<\/trkpt>)/g;

export function parseGpx(text: string): GpxSummary {
  const s: GpxSummary = { start: null, end: null, distanceM: 0, movingS: 0, maxKmh: 0, points: 0, track: [], bbox: null };
  let minLon = Infinity, minLat = Infinity, maxLon = -Infinity, maxLat = -Infinity;
  // A new <trkseg> is a gap in reception: no distance across it.
  for (const seg of text.split('<trkseg>')) {
    let pLat = NaN, pLon = NaN, pT = NaN;
    let kLat = NaN, kLon = NaN;
    TRKPT.lastIndex = 0;
    for (let m = TRKPT.exec(seg); m; m = TRKPT.exec(seg)) {
      const lat = +m[1], lon = +m[2];
      const t = m[3] ? Date.parse(m[3]) : NaN;
      const kmh = m[4] ? +m[4] * 3.6 : NaN;
      s.points++;
      if (!Number.isNaN(t)) {
        if (s.start === null) s.start = t;
        s.end = t;
      }
      if (!Number.isNaN(pLat)) {
        const d = distM(pLat, pLon, lat, lon);
        const dt = (t - pT) / 1000;
        const glitch = dt > 0 && (d / dt) * 3.6 > GLITCH_KMH;
        if (!glitch) {
          s.distanceM += d;
          if (dt > 0 && dt < 10 && (Number.isNaN(kmh) ? (d / dt) * 3.6 : kmh) > MOVING_KMH) s.movingS += dt;
        }
      }
      if (!Number.isNaN(kmh) && kmh > s.maxKmh) s.maxKmh = kmh;
      if (Number.isNaN(kLat) || distM(kLat, kLon, lat, lon) >= THIN_M) {
        s.track.push([lon, lat, Number.isNaN(kmh) ? 0 : Math.round(kmh)]);
        kLat = lat;
        kLon = lon;
      }
      if (lon < minLon) minLon = lon;
      if (lon > maxLon) maxLon = lon;
      if (lat < minLat) minLat = lat;
      if (lat > maxLat) maxLat = lat;
      pLat = lat;
      pLon = lon;
      pT = t;
    }
    // Keep where the segment ended, so the drawn line reaches it.
    const last = s.track[s.track.length - 1];
    if (!Number.isNaN(pLat) && last && (last[0] !== pLon || last[1] !== pLat)) s.track.push([pLon, pLat, last[2]]);
  }
  if (s.points) s.bbox = [minLon, minLat, maxLon, maxLat];
  return s;
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
};

// The header has grown over time (logger.cpp), so columns are found by name
// and a column an older file lacks just yields null.
export function parseCsv(text: string): CsvSummary {
  const lines = text.split('\n');
  const head = (lines[0] ?? '').trim().split(',');
  const col = (n: string) => head.indexOf(n);
  const iMs = col('ms'), iBoost = col('boostBar'), iOil = col('oilC'), iCool = col('coolantC');
  const iRpm = col('rpm'), iKmh = col('obdKmh');
  const s: CsvSummary = {
    rows: 0, durationS: 0, maxBoostBar: null, maxOilC: null, maxCoolantC: null, maxRpm: null, maxObdKmh: null, lastOilC: null,
  };
  let firstMs = NaN, lastMs = NaN;
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
  }
  // uptime ms can restart if the pod rebooted mid-drive; never negative.
  if (!Number.isNaN(firstMs)) s.durationS = Math.max(0, (lastMs - firstMs) / 1000);
  return s;
}

// ---- pairing ---------------------------------------------------------------

/**
 * Start time from a dated name, or null. The name is LOCAL time (logger.cpp
 * names files with localtime_r, in the pod's TZ setting) - unlike the GPX's
 * <time>, which is UTC. Read as the phone's local time, which is the pod's
 * as long as both are set to the same zone.
 */
export function stampOf(name: string): number | null {
  const m = /_(\d{4})-(\d{2})-(\d{2})_(\d{2})-(\d{2})(?:_\d+)?\.(?:gpx|csv)(?:\.nge)?$/.exec(name);
  if (!m) return null;
  return new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5]).getTime();
}

// ---- names -------------------------------------------------------------------
// An encrypted log is its plain name plus ".nge" (logcrypt.h).
export const isEncryptedName = (n: string) => n.endsWith('.nge');
export const plainName = (n: string) => (isEncryptedName(n) ? n.slice(0, -4) : n);
export const isGpxName = (n: string) => /^trip_.+\.gpx(?:\.nge)?$/.test(n);
export const isCsvName = (n: string) => /^drive_.+\.csv(?:\.nge)?$/.test(n);

export type TripFiles = { key: string; gpx?: string; csv?: string; start: number | null };

// A fix can come minutes after the first CAN frame (a garage), or before it
// (GPS on, engine not yet started).
const PAIR_WINDOW_MS = 20 * 60 * 1000;

/**
 * One entry per drive, newest first: each GPX with the CSV that started
 * nearest to it within PAIR_WINDOW_MS, and every CSV left over on its own.
 * Undated (numbered) files cannot be paired and come last.
 */
export function pairTrips(names: string[]): TripFiles[] {
  const gpx = names.filter(isGpxName);
  const csv = names.filter(isCsvName);
  const usedCsv = new Set<string>();
  const out: TripFiles[] = [];
  for (const g of gpx) {
    const t = stampOf(g);
    let best: string | undefined;
    let bestD = Infinity;
    if (t !== null) {
      for (const c of csv) {
        const tc = stampOf(c);
        if (tc === null || usedCsv.has(c)) continue;
        const d = Math.abs(tc - t);
        if (d <= PAIR_WINDOW_MS && d < bestD) {
          best = c;
          bestD = d;
        }
      }
    }
    if (best) usedCsv.add(best);
    out.push({ key: g, gpx: g, csv: best, start: t });
  }
  for (const c of csv) if (!usedCsv.has(c)) out.push({ key: c, csv: c, start: stampOf(c) });
  return out.sort((a, b) => (b.start ?? -Infinity) - (a.start ?? -Infinity) || b.key.localeCompare(a.key));
}
