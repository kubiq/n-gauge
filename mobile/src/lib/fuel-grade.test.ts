/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { asFuel, type Fuel, fillId, fuelByDrive, fuelLabel, totalsByFuel } from './fuel-grade.ts';
import type { Refuel } from './trips.ts';

const fill = (at: number | null, fromPct = 20, toPct = 90): Refuel => ({ at, pt: null, fromPct, toPct });
const F95: Fuel = { octane: 95, ethanol: 10 };
const F98: Fuel = { octane: 98, ethanol: 5 };
const V100: Fuel = { octane: 100, ethanol: null };

test('a fill-up is known by when, or without a clock by its rounded levels', () => {
  assert.equal(fillId(fill(1_700_000_000_000)), '1700000000000');
  assert.equal(fillId(fill(null, 19.6, 90.4)), '20-90');
  assert.equal(fillId(fill(null, 20.4, 89.6)), '20-90');
});

test('labels: octane, and the ethanol when known', () => {
  assert.equal(fuelLabel(F95), '95 E10');
  assert.equal(fuelLabel(F98), '98 E5');
  assert.equal(fuelLabel(V100), '100');
});

test('stored values are checked', () => {
  assert.deepEqual(asFuel({ octane: 98, ethanol: 5 }), F98);
  assert.deepEqual(asFuel({ octane: 100 }), V100);
  assert.deepEqual(asFuel({ octane: 95, ethanol: 85 }), { octane: 95, ethanol: null });
  assert.equal(asFuel({ octane: 91 }), null);
  assert.equal(asFuel('98'), null);
  assert.equal(asFuel(null), null);
});

test('drives run on the last fill-up before them, until the next', () => {
  const a = fill(1000);
  const b = fill(5000);
  const f = fuelByDrive(
    [
      { key: 'before' },
      { key: 'filled', fills: [a] },
      { key: 'after' },
      { key: 'refilled', fills: [b] },
      { key: 'later' },
    ],
    { [fillId(a)]: F95, [fillId(b)]: V100 },
  );
  assert.deepEqual(f, { before: null, filled: F95, after: F95, refilled: V100, later: V100 });
});

test('a fill-up with no fuel picked: not known from then on', () => {
  const a = fill(1000);
  const b = fill(5000);
  const f = fuelByDrive([{ key: 'x', fills: [a] }, { key: 'y', fills: [b] }, { key: 'z' }], { [fillId(a)]: F98 });
  assert.deepEqual(f, { x: F98, y: null, z: null });
});

test('two fill-ups on one drive: the later one counts', () => {
  const a = fill(1000);
  const b = fill(2000);
  assert.equal(fuelByDrive([{ key: 'x', fills: [a, b] }], { [fillId(a)]: F95, [fillId(b)]: F98 }).x, F98);
});

test('totals per fuel: summed by label, in octane order, short and unknown drives left out', () => {
  const t = totalsByFuel([
    { fuel: V100, fuelL: 3, fuelKm: 30 },
    { fuel: F98, fuelL: 2, fuelKm: 20 },
    { fuel: { octane: 98, ethanol: null }, fuelL: 1, fuelKm: 15 },
    { fuel: { octane: 98, ethanol: 5 }, fuelL: 1, fuelKm: 10 },
    { fuel: F95, fuelL: 0.2, fuelKm: 0.5 },   // under a kilometre
    { fuel: null, fuelL: 5, fuelKm: 50 },
    { fuel: F95, fuelL: null, fuelKm: 40 },
  ]);
  assert.deepEqual(
    t.map((x) => [fuelLabel(x.fuel), x.litres, x.km, x.drives]),
    [
      ['98', 1, 15, 1],
      ['98 E5', 3, 30, 2],
      ['100', 3, 30, 1],
    ],
  );
});
