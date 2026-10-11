/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { checkValue, SETTINGS, saveForm } from './settings-schema.ts';

const byJson = (j: string) => SETTINGS.find((d) => d.json === j)!;

test('numbers go under the form key, not the json name', () => {
  assert.equal(saveForm({ oilWarnC: 125, tpmsLowBarFront: 2.4 }), 'oilWarn=125&tLowF=2.4');
});

test('a bool turned off is still sent, as Present without the key', () => {
  assert.equal(saveForm({ nightOn: false }), 'nightOnPresent=1');
  assert.equal(saveForm({ nightOn: true }), 'nightOnPresent=1&nightOn=on');
});

test('fields not in the table are not sent', () => {
  assert.equal(saveForm({ apSsid: 1 }), '');
});

test('form keys and json names are unique', () => {
  assert.equal(new Set(SETTINGS.map((d) => d.key)).size, SETTINGS.length);
  assert.equal(new Set(SETTINGS.map((d) => d.json)).size, SETTINGS.length);
});

test('range check matches the firmware clamps', () => {
  assert.equal(checkValue(byJson('sleepAfterMin'), 0), '1–60');
  assert.equal(checkValue(byJson('sleepAfterMin'), 5), null);
  assert.equal(checkValue(byJson('sleepAfterMin'), 5.5), 'whole numbers only');
  assert.equal(checkValue(byJson('tpmsLowBarRear'), 2.45), null);
  assert.equal(checkValue(byJson('oilWarnC'), Number.NaN), 'not a number');
});
