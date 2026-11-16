/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { entriesOf, mergeCsv, mergeGpx, mergeSeries, mergeTracks, withMerged, withoutMerge } from './merge.ts';
import type { Series } from './series.ts';
import type { GpxStats } from './trip-cache.ts';
import type { CsvSummary, TripFiles } from './trips.ts';

const trip = (stamp: string, s: number): TripFiles => ({
  key: `trip_${stamp}.gpx`, gpx: `trip_${stamp}.gpx`, csv: `drive_${stamp}.csv`, start: s,
});
// Newest first, as the list has them.
const C = trip('c', 3000), B = trip('b', 2000), A = trip('a', 1000), Z = trip('z', 500);

test('entries: a merge is one entry, in its oldest part\'s place, keyed by it', () => {
  const groups = [['drive_a.csv', 'drive_b.csv', 'trip_a.gpx', 'trip_b.gpx']];
  const e = entriesOf([C, B, A, Z], groups);
  assert.deepEqual(e.map((x) => x.key), ['trip_c.gpx', 'trip_a.gpx', 'trip_z.gpx']);
  assert.deepEqual(e[1].parts.map((p) => p.key), ['trip_a.gpx', 'trip_b.gpx']);   // oldest first
});

test('entries: a merge with one drive left is that drive alone', () => {
  const e = entriesOf([C, A], [['trip_a.gpx', 'trip_b.gpx']]);
  assert.deepEqual(e.map((x) => x.parts.length), [1, 1]);
});

test('merge, then merge into it, then split', () => {
  let g = withMerged([], [A, B]);
  assert.equal(g.length, 1);
  // A merged entry and one more drive: one group of all three.
  g = withMerged(g, [A, B, C]);
  assert.equal(g.length, 1);
  assert.equal(entriesOf([C, B, A], g).length, 1);
  assert.deepEqual(withoutMerge(g, [B]), []);
});

const gpx = (o: Partial<GpxStats>): GpxStats => ({
  start: null, end: null, distanceM: 0, movingS: 0, maxKmh: 0, points: 0, bbox: null,
  from: null, to: null, fromPlace: null, toPlace: null, ...o,
});

test('gpx: summed, the peaks, the outer ends', () => {
  const m = mergeGpx([
    gpx({ start: 1000, end: 2000, distanceM: 1500, movingS: 100, maxKmh: 90, bbox: [14, 50, 14.1, 50.1], from: [14, 50], to: [14.1, 50.1], fromPlace: 'Home' }),
    gpx({ start: 5000, end: 9000, distanceM: 30000, movingS: 900, maxKmh: 130, bbox: [14.1, 49.9, 14.5, 50.1], from: [14.1, 50.1], to: [14.5, 49.9], toPlace: 'Away', ended: true }),
  ]);
  assert.equal(m.start, 1000);
  assert.equal(m.end, 9000);
  assert.equal(m.distanceM, 31500);
  assert.equal(m.maxKmh, 130);
  assert.deepEqual(m.bbox, [14, 49.9, 14.5, 50.1]);
  assert.deepEqual([m.fromPlace, m.toPlace, m.ended], ['Home', 'Away', true]);
});

const csv = (o: Partial<CsvSummary>): CsvSummary => ({
  rows: 0, durationS: 0, maxBoostBar: null, maxOilC: null, maxCoolantC: null, maxRpm: null, maxObdKmh: null,
  lastOilC: null, fuelL: null, fuelKm: null, fuelStartPct: null, fuelEndPct: null, refuels: [], ended: null, ...o,
});

test('csv: fuel summed for the consumption, the tank from both ends', () => {
  const m = mergeCsv([
    csv({ durationS: 600, fuelL: 1, fuelKm: 10, fuelStartPct: 50, fuelEndPct: 45, maxBoostBar: 1.2, lastOilC: 90 }),
    csv({ durationS: 1200, fuelL: 2, fuelKm: 30, fuelStartPct: 44, fuelEndPct: 30, maxBoostBar: 0.8, ended: true }),
  ]);
  assert.deepEqual([m.durationS, m.fuelL, m.fuelKm], [1800, 3, 40]);
  assert.deepEqual([m.fuelStartPct, m.fuelEndPct, m.maxBoostBar, m.lastOilC, m.ended], [50, 30, 1.2, 90, true]);
});

test('csv: a fill-up between two parts - asleep at the pump - is found, where the first ended', () => {
  const m = mergeCsv(
    [csv({ fuelEndPct: 12, refuels: [] }), csv({ fuelStartPct: 80, fuelEndPct: 70 })],
    [{ at: 5000, pt: [14, 50], place: 'Pump' }, { at: 9000, pt: [14.5, 50] }],
  );
  assert.deepEqual(m.refuels, [{ at: 5000, pt: [14, 50], place: 'Pump', fromPct: 12, toPct: 80 }]);
  // Not a few points of slosh.
  assert.deepEqual(mergeCsv([csv({ fuelEndPct: 40 }), csv({ fuelStartPct: 45 })]).refuels, []);
});

test('tracks: seconds from the first part\'s start, in real time', () => {
  const t = mergeTracks([
    { track: [[14, 50, 10, 0], [14.1, 50, 20, 60]], start: 1_000_000 },
    { track: [[14.1, 50, 0, 0], [14.2, 50, 30, 30]], start: 1_000_000 + 600_000 },   // 10 min later
  ]);
  assert.deepEqual(t.map((p) => p[3]), [0, 60, 600, 630]);
});

const series = (n: number, startUtc: number | null, durationS: number): Series => ({
  durationS, startUtc,
  t: Array.from({ length: n }, (_, i) => ((i + 0.5) / n) * durationS),
  kmh: Array(n).fill(50), boostBar: Array(n).fill(0.5), oilC: Array(n).fill(90), coolantC: Array(n).fill(88),
  volts: Array(n).fill(14), car: Array(n).fill(2),
});

test('series: end to end, the stop one off bucket, each bucket its own clock', () => {
  const m = mergeSeries([series(2, 1_000_000, 100), series(2, 5_000_000, 200)]);
  assert.equal(m.durationS, 300);
  assert.deepEqual(m.car, [2, 2, 0, 2, 2]);
  assert.deepEqual(m.t, [25, 75, 100, 150, 250]);
  assert.equal(m.kmh[2], null);
  assert.deepEqual(m.utc, [1_025_000, 1_075_000, null, 5_050_000, 5_150_000]);
});
