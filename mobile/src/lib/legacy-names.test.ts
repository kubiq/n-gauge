/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { legacyRenames } from './legacy-names.ts';
import { pairTrips, plainName, sessionOf } from './trips.ts';

const OLD = [
  'trip_2026-10-11_12-45.gpx',
  'drive_2026-10-11_12-44.csv',
  'drive_2026-10-11_15-30.csv',      // no gpx
  'drive_0279.csv',                  // undated
  'track_2026-10-11_15-20.vbo',
  // 2026-10-12: a restart a minute into the drive - the closest pair wins
  'trip_2026-10-12_21-18.gpx.nge',
  'trip_2026-10-12_21-19.gpx.nge',
  'drive_2026-10-12_21-21.csv.nge',
  'sleeplog.txt',
];

const pairsOf = (names: string[]) => pairTrips(names).map((t) => [t.gpx ?? null, t.csv ?? null]);

test('legacy: renamed files pair as they did by time', () => {
  const r = legacyRenames(OLD);
  const renamed = OLD.map((n) => r[n] ?? n);
  assert.deepEqual(pairsOf(renamed), [
    [r['trip_2026-10-12_21-19.gpx.nge'], r['drive_2026-10-12_21-21.csv.nge']],
    [r['trip_2026-10-12_21-18.gpx.nge'], null],
    [null, r['drive_2026-10-11_15-30.csv']],
    [r['trip_2026-10-11_12-45.gpx'], r['drive_2026-10-11_12-44.csv']],
    [null, r['drive_0279.csv']],
  ]);
});

test('legacy: only logs without a session, each given one', () => {
  const r = legacyRenames([...OLD, 'trip_2026-10-13_08-00_3fa81c.gpx']);
  assert.equal(Object.keys(r).length, OLD.length - 1);   // not sleeplog, not the new one
  for (const n of Object.values(r)) assert.match(sessionOf(n) ?? '', /^[0-9a-f]{6}$/);
  assert.equal(r['drive_0279.csv'].replace(/_[0-9a-f]{6}\./, '.'), 'drive_0279.csv');
  assert.match(r['trip_2026-10-12_21-19.gpx.nge'], /^trip_2026-10-12_21-19_[0-9a-f]{6}\.gpx\.nge$/);
});

test('legacy: the same names give the same ids - the phone and the card agree', () => {
  assert.deepEqual(legacyRenames(OLD), legacyRenames([...OLD].reverse()));
  assert.deepEqual(legacyRenames(Object.values(legacyRenames(OLD))), {});
});

test('legacy: a decrypted copy gets the id of its encrypted original', () => {
  const r = legacyRenames(OLD);
  const plain = legacyRenames(OLD.map(plainName));
  for (const n of OLD.filter((x) => x in r)) assert.equal(plainName(r[n]), plain[plainName(n)]);
});
