/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { groupByWeek, mondayOf, weekLabel } from './weeks.ts';

const at = (y: number, m: number, d: number, h = 12) => new Date(y, m - 1, d, h).getTime();

test('weeks: Monday 00:00 of the week, Sunday belongs to the week before', () => {
  assert.equal(mondayOf(at(2026, 10, 14)), at(2026, 10, 12, 0));   // Wednesday
  assert.equal(mondayOf(at(2026, 10, 12, 0)), at(2026, 10, 12, 0)); // Monday midnight
  assert.equal(mondayOf(at(2026, 10, 11, 23)), at(2026, 10, 5, 0)); // Sunday night
});

test('weeks: grouped newest first with totals, the undated last', () => {
  const drives = [
    { start: at(2026, 10, 13), distanceM: 12_000, secs: 900 },
    { start: null, distanceM: null, secs: 600 },
    { start: at(2026, 10, 12), distanceM: 8_000, secs: 600 },
    { start: at(2026, 10, 10), distanceM: 20_000, secs: 1800 },
  ];
  const g = groupByWeek(drives, (d) => d);
  assert.deepEqual(
    g.map((w) => [w.monday, w.drives, w.distanceM, w.secs]),
    [
      [at(2026, 10, 12, 0), 2, 20_000, 1500],
      [at(2026, 10, 5, 0), 1, 20_000, 1800],
      [null, 1, 0, 600],
    ],
  );
});

test('weeks: labels', () => {
  const now = at(2026, 10, 14);
  assert.equal(weekLabel(at(2026, 10, 12, 0), now), 'This week');
  assert.equal(weekLabel(at(2026, 10, 5, 0), now), 'Last week');
  assert.equal(weekLabel(at(2026, 9, 28, 0), now, 'en-GB'), '28 Sept – 4 Oct');
  assert.equal(weekLabel(at(2025, 12, 29, 0), now, 'en-GB'), '29 Dec – 4 Jan 2026');
  assert.equal(weekLabel(null, now), 'Undated');
});
