/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { speedStops } from './speed-scale.ts';

test('speed stops: equal 5 km/h steps from 0 to just over the top speed', () => {
  assert.deepEqual(speedStops(47), [0, 15, 30, 45, 60]);
  assert.deepEqual(speedStops(132), [0, 35, 70, 105, 140]);
  assert.deepEqual(speedStops(18), [0, 5, 10, 15, 20]);
  assert.deepEqual(speedStops(140), [0, 35, 70, 105, 140]);   // exactly on a stop
  assert.deepEqual(speedStops(0), [0, 5, 10, 15, 20]);        // standing: still a scale
});
