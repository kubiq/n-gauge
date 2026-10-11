/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { parseWifiQr } from './wifi-qr.ts';

test('reads the payload the pod draws', () => {
  assert.deepEqual(parseWifiQr('WIFI:T:WPA;S:n-gauge;P:abcd23efgh;;'), {
    ssid: 'n-gauge',
    pass: 'abcd23efgh',
  });
});

test('field order does not matter', () => {
  assert.deepEqual(parseWifiQr('WIFI:P:secret99;S:pod;T:WPA;;'), { ssid: 'pod', pass: 'secret99' });
});

test('backslash escapes', () => {
  assert.deepEqual(parseWifiQr('WIFI:T:WPA;S:my\\;pod;P:a\\:b\\\\c;;'), {
    ssid: 'my;pod',
    pass: 'a:b\\c',
  });
});

test('rejects other QR codes', () => {
  assert.equal(parseWifiQr('https://example.com'), null);
  assert.equal(parseWifiQr('WIFI:T:WPA;P:nossid;;'), null);
});
