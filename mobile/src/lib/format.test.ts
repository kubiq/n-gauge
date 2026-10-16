/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { when } from './format.ts';

const at = (y: number, m: number, d: number, h = 12, min = 0) => new Date(y, m - 1, d, h, min).getTime();

test('when: today and yesterday by name, older days by date', () => {
  const now = at(2026, 10, 16, 14, 30);
  assert.match(when(at(2026, 10, 16, 11, 16), now), /^Today, 11:16/);
  assert.match(when(at(2026, 10, 16, 0, 5), now), /^Today, /);           // just after midnight
  assert.match(when(at(2026, 10, 15, 23, 59), now), /^Yesterday, /);     // just before
  assert.match(when(at(2026, 10, 15, 17, 43), now), /^Yesterday, 17:43/);
  const older = when(at(2026, 10, 14, 17, 43), now);
  assert.doesNotMatch(older, /Today|Yesterday/);
  assert.match(older, /14/);
  // Over a month's turn: 1 Nov's yesterday is 31 Oct.
  assert.match(when(at(2026, 10, 31, 20, 0), at(2026, 11, 1, 9, 0)), /^Yesterday, /);
});
