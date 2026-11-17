/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { fillId } from './fuel-grade.ts';
import {
  byWeek,
  carFactor,
  drivePoints,
  type FuelDrive,
  fillIntervals,
  fillsByDrive,
  rolling,
  type SkippedStretch,
  TANK_L,
  tankFromPump,
} from './fuel-stats.ts';
import type { CsvSummary } from './trips.ts';

const csv = (o: Partial<CsvSummary>): CsvSummary => ({
  rows: 0, durationS: 600, maxBoostBar: null, maxOilC: null, maxCoolantC: null, maxRpm: null, maxObdKmh: null,
  lastOilC: null, fuelL: null, fuelKm: null, fuelStartPct: null, fuelEndPct: null, refuels: [], ended: null, ...o,
});
const H = 3_600_000;
const drive = (key: string, h: number, c: Partial<CsvSummary>): FuelDrive => ({ key, start: h * H, end: h * H + H / 2, c: csv(c) });

// Oldest first: a tank from 80 % down, a fill-up to 90 %, on down, another to 85 %.
const D = [
  drive('a', 0, { fuelL: 4, fuelKm: 50, fuelStartPct: 80, fuelEndPct: 72 }),
  drive('b', 24, { fuelL: 6, fuelKm: 70, fuelStartPct: 72, fuelEndPct: 60 }),
  drive('c', 48, { fuelL: 5, fuelKm: 60, fuelStartPct: 90, fuelEndPct: 80 }),   // filled before it
  drive('d', 72, { fuelL: 20, fuelKm: 250, fuelStartPct: 80, fuelEndPct: 40 }),
  drive('e', 96, { fuelL: 3, fuelKm: 30, fuelStartPct: 85, fuelEndPct: 80 }),   // filled before it
];
const newestFirst = [...D].reverse();

test('fill-ups: found while parked, on the drive after', () => {
  const f = fillsByDrive(newestFirst);
  assert.deepEqual(Object.keys(f).sort(), ['c', 'e']);
  assert.deepEqual([f.c[0].fromPct, f.c[0].toPct, f.c[0].at], [60, 90, 24 * H + H / 2]);
});

test('per drive, the rolling average by distance, per week', () => {
  const p = drivePoints(D);
  assert.equal(p.length, 5);
  assert.equal(p[0].l100, 8);
  const r = rolling(p, 2);
  assert.equal(r[1], ((4 + 6) / (50 + 70)) * 100);
  assert.equal(byWeek(p).reduce((s, w) => s + w.litres, 0), 38);
});

test('between two fill-ups: the car\'s figure against the tank, then the pump', () => {
  const fills = fillsByDrive(newestFirst);
  const tank = fillIntervals(D, fills, {});
  assert.equal(tank.length, 1);
  assert.deepEqual([tank[0].carL, tank[0].km, tank[0].drives, tank[0].by], [25, 310, 2, 'tank']);
  // The level fell 90 -> 40 % before the second fill-up: half the tank.
  assert.equal(tank[0].burnedL, 0.5 * TANK_L);
  // The pump's 24 l at the second, which left the tank at 85 %, the first at 90 %.
  const pumped = fillIntervals(D, fills, { [fillId(fills.e[0])]: 24 });
  assert.equal(pumped[0].by, 'pump');
  assert.equal(pumped[0].burnedL, 24 + 0.9 * TANK_L - 0.85 * TANK_L);
  const f = carFactor(pumped)!;
  assert.equal(f.by, 'pump');
  assert.equal(f.factor, pumped[0].burnedL! / 25);
});

test('the factor: the median of the stretches, not thrown by one with data missing', () => {
  // The real five of 2026-11-17, by the pump.
  const s = (car: number, burned: number) =>
    ({ from: {} as never, to: {} as never, carL: car, km: 100, burnedL: burned, by: 'pump' as const, drives: 3 });
  const f = carFactor([s(40.6, 40.8), s(9.0, 15.2), s(36.7, 43.1), s(32.6, 33.2), s(39.3, 40.0)])!;
  assert.ok(Math.abs(f.factor - 1.02) < 0.01, String(f.factor));
  assert.equal(f.stretches, 5);
});

test('a drive with no tank level between the fill-ups still counts in the stretch', () => {
  // As the real list 2026-11-17: seven fill-ups, no stretch found.
  const D2 = [
    drive('a', 0, { fuelL: 4, fuelKm: 50, fuelStartPct: 80, fuelEndPct: 70 }),
    drive('b', 24, { fuelL: 5, fuelKm: 60, fuelStartPct: 95, fuelEndPct: 85 }),   // filled before it
    drive('x', 30, { fuelL: 1, fuelKm: 8 }),                                        // a hop, no level read
    drive('c', 48, { fuelL: 20, fuelKm: 250, fuelStartPct: 80, fuelEndPct: 40 }),
    drive('d', 96, { fuelL: 3, fuelKm: 30, fuelStartPct: 90, fuelEndPct: 85 }),   // filled before it
  ];
  const fills = fillsByDrive([...D2].reverse());
  const s = fillIntervals(D2, fills, {});
  assert.equal(s.length, 1);
  assert.deepEqual([s[0].drives, s[0].carL], [3, 26]);
});

test('the tank, from the pump: litres over the level\'s rise; the book\'s without receipts', () => {
  // The real five receipts of 2026-11-17.
  const f = (from: number, to: number, at: number) => ({ at, pt: null, fromPct: from, toPct: to });
  const fills = [f(13, 100, 1), f(30, 100, 2), f(8, 100, 3), f(8, 100, 4), f(13, 43, 5)];
  const pump = { '1': 40.03, '2': 33.22, '3': 43.12, '4': 41.85, '5': 14.16 };
  const t = tankFromPump(fills, pump);
  assert.equal(t.fills, 5);
  assert.ok(t.litres > 46 && t.litres < 47.5, String(t.litres));
  assert.deepEqual(tankFromPump(fills, {}), { litres: TANK_L, fills: 0 });
});

test('fill-ups during drives - the gauge awake at the pump - split them by the fuel so far', () => {
  // a: 1 l to the pump, then 3 l more. b: whole, 6 l. c: 5 l to the pump.
  const during = [
    drive('a', 0, { fuelL: 4, fuelKm: 40, fuelStartPct: 20, fuelEndPct: 80, refuels: [{ at: H / 4, pt: null, fromPct: 15, toPct: 90, fuelBeforeL: 1 }] }),
    drive('b', 24, { fuelL: 6, fuelKm: 70, fuelStartPct: 80, fuelEndPct: 66 }),
    drive('c', 48, { fuelL: 7, fuelKm: 80, fuelStartPct: 66, fuelEndPct: 95, refuels: [{ at: 48 * H + H / 4, pt: null, fromPct: 60, toPct: 98, fuelBeforeL: 5 }] }),
  ];
  const fills = fillsByDrive([...during].reverse());
  const s = fillIntervals(during, fills, {});
  assert.equal(s.length, 1);
  assert.equal(s[0].carL, 3 + 6 + 5);
  assert.equal(s[0].drives, 3);
  assert.equal(s[0].burnedL, ((90 - 60) / 100) * TANK_L);
  // From a summary before the split was kept: not placed, no stretch - and said so.
  const old = during.map((d) => ({ ...d, c: { ...d.c!, refuels: d.c!.refuels.map(({ fuelBeforeL: _, ...r }) => r) } }));
  const why: SkippedStretch[] = [];
  assert.equal(fillIntervals(old, fillsByDrive([...old].reverse()), {}, why).length, 0);
  assert.deepEqual(why.map((w) => w.why), ['unplaced']);
  // A track alone between - the car silent, a wake by motion - burned nothing.
  const gap = [...during.slice(0, 2), { key: 'gpx', start: 30 * H, end: 30 * H + H / 10 }, during[2]];
  const s2 = fillIntervals(gap, fillsByDrive([...gap].reverse()), {});
  assert.equal(s2.length, 1);
  assert.equal(s2[0].carL, 3 + 6 + 5);
  // Car data without the fuel figure (older logs): not known - left out, and said why.
  const old2 = [...during.slice(0, 2), drive('nofuel', 30, { fuelStartPct: 66, fuelEndPct: 62 }), during[2]];
  const why2: SkippedStretch[] = [];
  assert.equal(fillIntervals(old2, fillsByDrive([...old2].reverse()), {}, why2).length, 0);
  assert.deepEqual(why2.map((w) => [w.why, w.at]), [['nofuel', 30 * H]]);
  assert.equal(carFactor([]), null);
});
