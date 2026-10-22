/// <reference types="node" />
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import { x25519 } from '@noble/curves/ed25519.js';

import {
  beaconOf,
  bytesToText,
  fingerprint,
  fingerprintSpaced,
  isEncrypted,
  keyFromText,
  keyToText,
  openLog,
  sealSegment,
} from './logcrypt.ts';

// Written by the firmware's own tests (tests/test_logcrypt.cpp) from the
// pod's C code: this is the check that the phone reads what the pod writes.
const V = JSON.parse(readFileSync(new URL('../../../tests/vectors/logcrypt-v1.json', import.meta.url), 'utf8'));
const hex = (h: string) => Uint8Array.from(h.match(/../g)!.map((b: string) => parseInt(b, 16)));
const toHex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
const text = (b: Uint8Array) => new TextDecoder().decode(b);
const bytes = (s: string) => new TextEncoder().encode(s);

const secret = hex(V.recipientSecret);
const pub = hex(V.recipientPublic);
const file = hex(V.file);

test('the same key pair as the pod’s C code', () => {
  assert.equal(toHex(x25519.getPublicKey(secret)), V.recipientPublic);
  assert.equal(fingerprint(pub), V.fingerprint);
});

test('the same beacon as the pod’s C code', () => {
  assert.equal(beaconOf(pub, V.beaconSession), V.beacon);
});

test('decrypts the file the pod’s C code wrote', () => {
  const { plain, consumed } = openLog(file, secret);
  assert.equal(text(plain), V.plaintext);
  assert.equal(consumed, file.length);
});

test('writes byte for byte what the pod writes', () => {
  const recs = V.records.map(bytes);
  const two = [sealSegment(pub, hex(V.ephemeralSecrets[0]), recs), sealSegment(pub, hex(V.ephemeralSecrets[1]), recs)];
  assert.equal(two.map(toHex).join(''), V.file);
});

test('a cut-off last record: everything before it, no throw', () => {
  const lastLen = V.records[2].length + 21;
  const { plain, consumed } = openLog(file.subarray(0, file.length - 3), secret);
  assert.equal(text(plain), V.plaintext.slice(0, V.plaintext.length - V.records[2].length));
  assert.equal(consumed, file.length - lastLen);
});

// A write that failed mid-block on the pod (a card error), then a new segment.
test('a broken block is skipped to the next segment', () => {
  const recs: string[] = V.records;
  const seg = sealSegment(pub, hex(V.ephemeralSecrets[0]), recs.map(bytes));
  const junk = Uint8Array.from([0x02, 0x10, 0, 0, 0, 0x6a, 0x75, 0x6e, 0x6b]);
  const parts = [seg.subarray(0, seg.length - 7), seg, junk, seg];
  const f = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  parts.reduce((o, p) => (f.set(p, o), o + p.length), 0);
  const { plain, consumed } = openLog(f, secret);
  assert.equal(text(plain), recs[0] + recs[1] + recs.join('') + recs.join(''));
  assert.equal(consumed, f.length);
});

test('the wrong key reads nothing', () => {
  const other = secret.slice();
  // Not bit 0 of byte 0: X25519 clamps the low 3 bits, so that is the same key.
  other[10] ^= 1;
  assert.equal(openLog(file, other).plain.length, 0);
});

test('the key as text: round trip, bare hex accepted, junk refused', () => {
  const t = keyToText(secret);
  assert.ok(t.startsWith('ngauge-log-key-v1:'));
  assert.deepEqual(keyFromText(`  ${t}\n`), secret);
  assert.deepEqual(keyFromText(V.recipientSecret.toUpperCase()), secret);
  assert.equal(keyFromText(t.slice(0, -1)), null);
  assert.equal(keyFromText('ngauge-log-key-v1:' + 'g'.repeat(64)), null);
  assert.equal(keyFromText(''), null);
});

test('fingerprint spaced like the pod shows it', () => {
  assert.equal(fingerprintSpaced(pub).replace(/ /g, ''), V.fingerprint);
  assert.match(fingerprintSpaced(pub), /^[0-9a-f]{4} [0-9a-f]{4} [0-9a-f]{4}$/);
});

test('bytesToText fallback decodes like TextDecoder', () => {
  const s = 'utc,ms\r\nStart ¬ Finish 50.0568\n';
  const enc = bytes(s);
  const saved = globalThis.TextDecoder;
  // @ts-expect-error - simulating a runtime without it
  globalThis.TextDecoder = undefined;
  try {
    assert.equal(bytesToText(enc), s);
  } finally {
    globalThis.TextDecoder = saved;
  }
});

test('tells encrypted files from plain ones', () => {
  assert.ok(isEncrypted(file));
  assert.ok(!isEncrypted(bytes('utc,ms,oilC\n')));
  assert.ok(!isEncrypted(bytes('<?xml version="1.0"?>')));
});
