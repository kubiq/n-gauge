/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { moved, screenItems, viewsOf } from './screens.ts';

const ids = (items: { id: string; on: boolean }[]) => items.map((s) => `${s.id}${s.on ? '' : '-'}`).join(' ');

test('screens: the gauge default round-trips', () => {
  assert.equal(viewsOf(screenItems('0245736')), '0245736');
});

test('screens: hidden ones follow the shown, switched off', () => {
  assert.equal(ids(screenItems('506')), '5 0 6 2- 4- 7- 3- 8- 1-');
  assert.equal(viewsOf(screenItems('506')), '506');
});

test('screens: WiFi cannot be hidden, and unknown ids are dropped', () => {
  const items = screenItems('0 x 9 2');                 // x, 9: no such screen in the app
  assert.equal(viewsOf(items.map((s) => ({ ...s, on: false }))), '6');
  assert.equal(ids(items), '0 2 4- 5- 7- 3- 6 8- 1-');
});

test('screens: the upload status is hidden by default, and can be shown', () => {
  assert.equal(ids(screenItems('0245736')), '0 2 4 5 7 3 6 8- 1-');
  // The clock shown (slot 1): kept in its place; WiFi added, as always.
  assert.equal(viewsOf(screenItems('0124573')), '01245736');
  assert.equal(viewsOf(screenItems('02457368')), '02457368');
});

test('screens: moving, and not past either end', () => {
  const items = screenItems('0245736');
  assert.equal(viewsOf(moved(items, 6, -1)), '0245763');  // WiFi up past the g-meter
  assert.equal(moved(items, 0, -1), items);
  assert.equal(moved(items, items.length - 1, 1), items);
});
