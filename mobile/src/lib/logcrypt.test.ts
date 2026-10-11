/// <reference types="node" />
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

import { x25519 } from '@noble/curves/ed25519.js';

import { fingerprint, isEncrypted, openLog, sealSegment } from './logcrypt.ts';

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

test('the wrong key reads nothing', () => {
  const other = secret.slice();
  // Not bit 0 of byte 0: X25519 clamps the low 3 bits, so that is the same key.
  other[10] ^= 1;
  assert.equal(openLog(file, other).plain.length, 0);
});

test('tells encrypted files from plain ones', () => {
  assert.ok(isEncrypted(file));
  assert.ok(!isEncrypted(bytes('utc,ms,oilC\n')));
  assert.ok(!isEncrypted(bytes('<?xml version="1.0"?>')));
});
