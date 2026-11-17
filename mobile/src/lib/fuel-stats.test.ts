/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { fillId } from './fuel-grade.ts';
import { byWeek, carFactor, drivePoints, type FuelDrive, fillIntervals, fillsByDrive, rolling, TANK_L } from './fuel-stats.ts';
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

test('a fill-up during a drive splits it where the sum cannot: no stretch across it', () => {
  const during = [
    drive('a', 0, { fuelL: 4, fuelKm: 50, fuelStartPct: 30, fuelEndPct: 80, refuels: [{ at: H / 4, pt: null, fromPct: 25, toPct: 85 }] }),
    drive('b', 24, { fuelL: 6, fuelKm: 70, fuelStartPct: 95, fuelEndPct: 90 }),
  ];
  const fills = fillsByDrive([...during].reverse());
  assert.equal(fillIntervals(during, fills, {}).length, 0);
  assert.equal(carFactor([]), null);
});
