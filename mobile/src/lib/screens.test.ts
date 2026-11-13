/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { moved, screenItems, viewsOf } from './screens.ts';

const ids = (items: { id: string; on: boolean }[]) => items.map((s) => `${s.id}${s.on ? '' : '-'}`).join(' ');

// The gauge's default (config.h VIEW_ORDER_DEFAULT): all but the clock and the status.
const DEFAULT = '01234569';

test('screens: the gauge default round-trips', () => {
  assert.equal(viewsOf(screenItems(DEFAULT)), DEFAULT);
});

test('screens: hidden ones follow the shown, switched off', () => {
  assert.equal(ids(screenItems('419')), '4 1 9 0- 2- 3- 5- 6- 7- 8-');
  assert.equal(viewsOf(screenItems('419')), '419');
});

test('screens: WiFi cannot be hidden, and unknown ids are dropped', () => {
  const items = screenItems('1 x 2');                   // x: no such screen
  assert.equal(viewsOf(items.map((s) => ({ ...s, on: false }))), '9');
  assert.equal(ids(items), '1 2 0- 3- 4- 5- 6- 7- 8- 9');
});

test('screens: the clock and the upload status are hidden by default, and can be shown', () => {
  assert.equal(ids(screenItems(DEFAULT)), '0 1 2 3 4 5 6 9 7- 8-');
  assert.equal(viewsOf(screenItems('0123456789')), '0123456789');
});

test('screens: moving, and not past either end', () => {
  const items = screenItems(DEFAULT);
  assert.equal(viewsOf(moved(items, 7, -1)), '01234596');  // WiFi up past the g-meter
  assert.equal(moved(items, 0, -1), items);
  assert.equal(moved(items, items.length - 1, 1), items);
});
