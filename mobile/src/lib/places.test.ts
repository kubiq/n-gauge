/// <reference types="node" />
import assert from 'node:assert/strict';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { test } from 'node:test';

import { type Atlas, countriesAt, parseAtlas, placeAt, routeName } from './places.ts';
import { parseGpx } from './trips.ts';

// A made-up country, sorted by latitude as the files are.
const atlas: Atlas = [
  ['South', 50.0, 14.0, 1],
  ['Village', 50.02, 14.0, 1],
  ['North', 50.1, 14.0, 1],
];

test('places: the nearest name, none far from everything', () => {
  assert.equal(placeAt(50.001, 14.0, [], [atlas]), 'South');
  assert.equal(placeAt(50.015, 14.0, [], [atlas]), 'Village');
  assert.equal(placeAt(51.0, 14.0, [], [atlas]), null);   // 100 km off: not "North"
});

test('places: a known place nearby beats a nearer hamlet; far off, the hamlet', () => {
  const a: Atlas = [
    ['Quarter', 50.0, 14.0, 1],
    ['Hamlet', 50.015, 14.0, 0],     // 1.7 km north of Quarter
    ['Lone farm', 50.2, 14.0, 0],    // 22 km from anything known
  ];
  assert.equal(placeAt(50.015, 14.0, [], [a]), 'Quarter');   // on the hamlet, Quarter 1.7 km
  assert.equal(placeAt(50.2, 14.0, [], [a]), 'Lone farm');
});

test('places: your own place wins within 300 m, not beyond', () => {
  const own = [{ name: 'Home', lat: 50.0005, lon: 14.0 }];
  assert.equal(placeAt(50.0, 14.0, own, [atlas]), 'Home');       // 56 m
  assert.equal(placeAt(50.01, 14.0, own, [atlas]), 'South');     // 1 km from Home
});

test('places: route names, loops and an unnamed end', () => {
  assert.equal(routeName('South', 'North'), 'South → North');
  assert.equal(routeName('South', 'South'), 'Around South');
  assert.equal(routeName('South', null), 'South → …');
  assert.equal(routeName(null, null), null);
});

test('places: a list as shipped parses', () => {
  assert.deepEqual(parseAtlas('Brno\t49.1952\t16.608\t1\nVes\t49.2\t16.6\t0\n'), [
    ['Brno', 49.1952, 16.608, 1],
    ['Ves', 49.2, 16.6, 0],
  ]);
});

test('places: the countries a point needs', () => {
  const index = [
    { cc: 'AA', bbox: [12, 48, 19, 51] as [number, number, number, number] },
    { cc: 'BB', bbox: [16, 47, 23, 50] as [number, number, number, number] },
  ];
  assert.deepEqual(countriesAt(index, 49.5, 17), ['AA', 'BB']);   // the boxes overlap
  assert.deepEqual(countriesAt(index, 50.5, 14), ['AA']);
  assert.deepEqual(countriesAt(index, 40, 14), []);
});

// The real list, as the app ships it (assets/places).
const cz = 'assets/places/CZ.tsv';
test('places: the Czech list names a town centre', () => {
  const list = parseAtlas(readFileSync(cz, 'utf8'));
  assert.equal(placeAt(49.6899, 14.0104, [], [list]), 'Příbram');
  assert.equal(placeAt(49.1952, 16.608, [], [list]), 'Brno');
});

// Every real drive gets a name at both ends (data/ is not in the repo; the
// names themselves are not written here - the repo is public).
test('places: every real drive is named', { skip: !existsSync('../data') }, () => {
  const list = parseAtlas(readFileSync(cz, 'utf8'));
  const at = (p: [number, number] | null) => (p ? placeAt(p[1], p[0], [], [list]) : null);
  for (const f of readdirSync('../data').filter((n) => /^trip_2026.*\.gpx$/.test(n))) {
    const r = parseGpx(readFileSync(`../data/${f}`, 'utf8'));
    if (!r.from) continue;
    const name = routeName(at(r.from), at(r.to));
    assert.ok(name && !name.includes('…'), `${f}: ${name}`);
  }
});
