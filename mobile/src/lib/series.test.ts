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
