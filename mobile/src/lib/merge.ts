// Drives merged into one (asked 2026-11-15: a stop for food mid-trip, the
// gauge asleep between, made one trip three drives). A merge is the app's own
// record - merges.json (store.ts), groups of file names - and the log files
// stay as they are; "Split" takes it back. By file name, not by a drive's
// key: the key moves from its CSV to its GPX when that arrives.
//
// Pure, so it is tested with node: drives in, one drive out.

import type { GpxStats } from './trip-cache';
import type { Series } from './series';
import { type CsvSummary, FILL_MIN_PCT, type Refuel, type TrackPoint, type TripFiles } from './trips.ts';

/** Each group: the file names (GPX and CSV) of the drives merged. */
export type MergeGroups = string[][];

/** A row of the list: one drive, or several merged - oldest first. */
export type Entry = { key: string; parts: TripFiles[] };

const filesOf = (t: TripFiles) => [t.gpx, t.csv].filter((n): n is string => !!n);

/**
 * The list's entries from its drives (newest first) and the merges: each
 * group with two or more of its drives still here is one entry, in the place
 * of its oldest part - the date it shows - keyed by that part's key.
 */
export function entriesOf(trips: TripFiles[], groups: MergeGroups): Entry[] {
  const groupOf = new Map<string, number>();
  groups.forEach((g, i) => g.forEach((n) => groupOf.set(n, i)));
  const gi = (t: TripFiles) => filesOf(t).map((n) => groupOf.get(n)).find((i) => i !== undefined);
  const members = new Map<number, TripFiles[]>();
  for (const t of trips) {
    const i = gi(t);
    if (i !== undefined) members.set(i, [...(members.get(i) ?? []), t]);
  }
  const out: Entry[] = [];
  for (const t of trips) {
    const i = gi(t);
    const m = i === undefined ? undefined : members.get(i);
    if (!m || m.length < 2) out.push({ key: t.key, parts: [t] });
    // Newest first: the last member met is the oldest - the entry goes there.
    else if (m[m.length - 1] === t) {
      const parts = [...m].reverse();
      out.push({ key: parts[0].key, parts });
    }
  }
  return out;
}

/** The merges with these drives' files joined into one group (merged entries included). */
export function withMerged(groups: MergeGroups, parts: TripFiles[]): MergeGroups {
  const files = new Set(parts.flatMap(filesOf));
  const touched = groups.filter((g) => g.some((n) => files.has(n)));
  for (const g of touched) g.forEach((n) => files.add(n));
  return [...groups.filter((g) => !touched.includes(g)), [...files].sort()];
}

/** The merges without the group holding any of these files. */
export function withoutMerge(groups: MergeGroups, parts: TripFiles[]): MergeGroups {
  const files = new Set(parts.flatMap(filesOf));
  return groups.filter((g) => !g.some((n) => files.has(n)));
}

const maxOf = (vals: (number | null | undefined)[]): number | null => {
  const v = vals.filter((x): x is number => x != null);
  return v.length ? Math.max(...v) : null;
};
const sumOf = (vals: (number | null | undefined)[]): number | null => {
  const v = vals.filter((x): x is number => x != null);
  return v.length ? v.reduce((a, b) => a + b, 0) : null;
};

/** One track's figures from its parts' (oldest first): summed, the peaks, the outer ends. */
export function mergeGpx(parts: GpxStats[]): GpxStats {
  const first = parts[0], last = parts[parts.length - 1];
  const boxes = parts.map((p) => p.bbox).filter((b): b is [number, number, number, number] => !!b);
  return {
    start: parts.find((p) => p.start !== null)?.start ?? null,
    end: [...parts].reverse().find((p) => p.end !== null)?.end ?? null,
    distanceM: parts.reduce((s, p) => s + p.distanceM, 0),
    movingS: parts.reduce((s, p) => s + p.movingS, 0),
    maxKmh: Math.max(...parts.map((p) => p.maxKmh)),
    points: parts.reduce((s, p) => s + p.points, 0),
    bbox: boxes.length
      ? [Math.min(...boxes.map((b) => b[0])), Math.min(...boxes.map((b) => b[1])), Math.max(...boxes.map((b) => b[2])), Math.max(...boxes.map((b) => b[3]))]
      : null,
    from: first.from,
    to: last.to,
    fromPlace: first.fromPlace,
    toPlace: last.toPlace,
    ended: last.ended,
  };
}

/** Where and when a part ended (its GPX), for a fill-up after it. */
export type PartEnd = { at: number | null; pt: [number, number] | null; place?: string | null };

/**
 * One drive's car figures from its parts' (oldest first). A fill-up between
 * two parts - the gauge asleep at the pump, the tank fuller when it woke -
 * the list finds between two drives; merged, it is found here, at the end
 * of the part before it (`ends`, one per part).
 */
export function mergeCsv(parts: CsvSummary[], ends: PartEnd[] = []): CsvSummary {
  const last = parts[parts.length - 1];
  const refuels: Refuel[] = [];
  parts.forEach((p, i) => {
    const before = parts[i - 1];
    if (before?.fuelEndPct != null && p.fuelStartPct != null && p.fuelStartPct - before.fuelEndPct >= FILL_MIN_PCT) {
      const e = ends[i - 1];
      refuels.push({ at: e?.at ?? null, pt: e?.pt ?? null, place: e?.place ?? null, fromPct: before.fuelEndPct, toPct: p.fuelStartPct });
    }
    refuels.push(...p.refuels);
  });
  return {
    rows: parts.reduce((s, p) => s + p.rows, 0),
    durationS: parts.reduce((s, p) => s + p.durationS, 0),
    maxBoostBar: maxOf(parts.map((p) => p.maxBoostBar)),
    maxOilC: maxOf(parts.map((p) => p.maxOilC)),
    maxCoolantC: maxOf(parts.map((p) => p.maxCoolantC)),
    maxRpm: maxOf(parts.map((p) => p.maxRpm)),
    maxObdKmh: maxOf(parts.map((p) => p.maxObdKmh)),
    lastOilC: [...parts].reverse().find((p) => p.lastOilC != null)?.lastOilC ?? null,
    fuelL: sumOf(parts.map((p) => p.fuelL)),
    fuelKm: sumOf(parts.map((p) => p.fuelKm)),
    fuelStartPct: parts.find((p) => p.fuelStartPct != null)?.fuelStartPct ?? null,
    fuelEndPct: [...parts].reverse().find((p) => p.fuelEndPct != null)?.fuelEndPct ?? null,
    refuels,
    ended: last.ended,
  };
}

/**
 * The parts' tracks as one, in real time: each point's seconds counted from
 * the first part's start, so a moment on the charts (Series.utc) finds its
 * place on the map as in a single drive. A part without a clock keeps its
 * own seconds after the one before.
 */
export function mergeTracks(parts: { track: TrackPoint[]; start: number | null }[]): TrackPoint[] {
  const t0 = parts.find((p) => p.start !== null)?.start ?? null;
  const out: TrackPoint[] = [];
  let lastSec = 0;
  for (const p of parts) {
    const off = t0 !== null && p.start !== null ? (p.start - t0) / 1000 : lastSec;
    for (const [lon, lat, kmh, sec] of p.track) {
      const s = sec === null ? null : Math.round((sec + off) * 10) / 10;
      out.push([lon, lat, kmh, s]);
      if (s !== null) lastSec = s;
    }
  }
  return out;
}

/**
 * The parts' charts end to end (oldest first). The stop between two parts -
 * minutes or hours - is one bucket of "car off", not its length: a lunch
 * would leave most of the chart empty. So the chart's seconds are driving
 * time, and each bucket carries its own clock (utc) for the map.
 */
export function mergeSeries(parts: Series[]): Series {
  const out: Series = { durationS: 0, startUtc: parts[0]?.startUtc ?? null, t: [], kmh: [], boostBar: [], oilC: [], coolantC: [], volts: [], car: [], utc: [] };
  parts.forEach((p, i) => {
    if (i > 0) {
      // The stop: one empty bucket, the car off.
      out.t.push(out.durationS);
      for (const k of ['kmh', 'boostBar', 'oilC', 'coolantC', 'volts'] as const) out[k].push(null);
      out.car.push(0);
      out.utc!.push(null);
    }
    p.t.forEach((t, b) => {
      out.t.push(Math.round((out.durationS + t) * 100) / 100);
      for (const k of ['kmh', 'boostBar', 'oilC', 'coolantC', 'volts'] as const) out[k].push(p[k][b] ?? null);
      out.car.push(p.car?.[b] ?? null);
      out.utc!.push(p.startUtc !== null ? p.startUtc + t * 1000 : null);
    });
    out.durationS += p.durationS;
  });
  return out;
}
