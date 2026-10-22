/// <reference types="node" />
import assert from 'node:assert/strict';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { test } from 'node:test';

import { csvSeries } from './series.ts';

const HEAD = 'utc,ms,oilC,coolantC,boostBar,speedKmh,obdKmh,volts\n';

test('series: peaks for speed and boost, means for the rest, gaps stay gaps', () => {
  const s = csvSeries(
    HEAD +
      ',0,80,85,0.2,,40,14.4\n' +
      ',500,82,85,1.5,,60,14.6\n' +   // bucket 0
      ',1000,,86,0.1,,50,\n' +
      ',1500,,86,-0.3,,30,\n',        // bucket 1: no oil, no volts
    2,
  );
  assert.equal(s.durationS, 1.5);
  assert.deepEqual(s.t, [0.38, 1.13]);
  assert.deepEqual(s.kmh, [60, 50]);
  assert.deepEqual(s.boostBar, [1.5, 0.1]);
  assert.deepEqual(s.oilC, [81, null]);
  assert.deepEqual(s.volts, [14.5, null]);
});

test('series: GPS speed for logs without obdKmh, and a restart is left out', () => {
  const s = csvSeries(
    'utc,ms,oilC,speedKmh\n' + ',1000,80,10\n' + ',2000,81,20\n' + ',100,79,99\n' + ',3000,82,30\n',
    3,
  );
  assert.deepEqual(s.kmh, [10, 20, 30]);   // the 99 after the restart (ms 100) is not in
  assert.deepEqual(s.volts, [null, null, null]);
});

test('series: the car\'s state, from its column - and from the readings without it', () => {
  // The gauge's own column wins; the highest state in a bucket.
  const logged = csvSeries('utc,ms,rpm,oilC,car\n,0,800,90,2\n,500,,,0\n,1000,,,1\n,1500,0,90,1\n', 2);
  assert.deepEqual(logged.car, [2, 1]);
  // Older logs: running at 300 rpm, on while the car's readings come, else off.
  const old = csvSeries('utc,ms,rpm,oilC,speedKmh\n,0,800,90,30\n,500,,,0\n,1000,0,90,0\n,1500,,,0\n', 4);
  assert.deepEqual(old.car, [2, 0, 1, 0]);
  // No car columns at all: it cannot say.
  assert.deepEqual(csvSeries('utc,ms,speedKmh\n,0,10\n,500,20\n', 2).car, [null, null]);
});

// 2026-10-14: the gauge writes CRLF, voltage is the last column, and the
// first row of a drive has none yet - its "\r" read as 0 V, and the chart's
// first point (a bucket's mean) said 5.3 V.
test('series: an empty last column on a CRLF line is no reading, not 0', () => {
  const s = csvSeries('utc,ms,volts\r\n,0,\r\n,500,10.60\r\n,1000,14.40\r\n', 2);
  // Bucket 0 holds only the empty row: no reading, not 0 (the bug gave 0).
  assert.deepEqual(s.volts, [null, 12.5]);
});

test('series: the first row\'s time, from the whole-second utc and the uptime', () => {
  // Uptime 1000 at 10:00:00.6 really: utc says :00 there, :01 at uptime 1500.
  const s = csvSeries('utc,ms,volts\n2026-10-14T10:00:00Z,1000,14\n2026-10-14T10:00:01Z,1500,14\n', 2);
  assert.equal(s.startUtc, Date.UTC(2026, 9, 14, 10, 0, 0) + 500);   // within the second's truncation
  assert.equal(csvSeries('utc,ms,volts\n,1000,14\n,1500,14\n', 2).startUtc, null);   // clock not set
});

test('series: nothing to chart', () => {
  assert.equal(csvSeries('').t.length, 0);
  assert.equal(csvSeries(HEAD + ',1000,80,85,0,,0,14\n').t.length, 0);
});

// Real drives, where there are some (data/ is not in the repo).
test('series: real logs stay small', { skip: !existsSync('../data') }, () => {
  for (const f of readdirSync('../data').filter((n) => /^drive_2026-10-1.*\.csv$/.test(n)).slice(0, 5)) {
    const s = csvSeries(readFileSync(`../data/${f}`, 'utf8'));
    assert.ok(s.t.length <= 600, f);
    assert.ok(JSON.stringify(s).length < 40_000, `${f}: ${JSON.stringify(s).length} bytes`);
  }
});
