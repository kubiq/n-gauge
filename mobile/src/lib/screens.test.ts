/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { moved, screenItems, viewsOf } from './screens.ts';

const ids = (items: { id: string; on: boolean }[]) => items.map((s) => `${s.id}${s.on ? '' : '-'}`).join(' ');

test('screens: the gauge default round-trips', () => {
  assert.equal(viewsOf(screenItems('0245736')), '0245736');
});

test('screens: hidden ones follow the shown, switched off', () => {
  assert.equal(ids(screenItems('506')), '5 0 6 2- 4- 7- 3-');
  assert.equal(viewsOf(screenItems('506')), '506');
});

test('screens: WiFi cannot be hidden, and unknown ids are dropped', () => {
  const items = screenItems('0 1 9 2');                 // 1 = the old coolant view, 9 = none
  assert.equal(viewsOf(items.map((s) => ({ ...s, on: false }))), '6');
  assert.equal(ids(items), '0 2 4- 5- 7- 3- 6');
});

test('screens: moving, and not past either end', () => {
  const items = screenItems('0245736');
  assert.equal(viewsOf(moved(items, 6, -1)), '0245763');  // WiFi up past the g-meter
  assert.equal(moved(items, 0, -1), items);
  assert.equal(moved(items, 6, 1), items);
});
