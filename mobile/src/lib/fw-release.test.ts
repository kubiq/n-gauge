/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { ed25519 } from '@noble/curves/ed25519.js';
import { bytesToHex } from '@noble/hashes/utils.js';

import { checkRelease, isNewer, type Latest, parseLatest, signedMessage } from './fw-release.ts';

// A fake image with an app description where the gauge's has one.
function image(elfHash: Uint8Array, size = 4096): Uint8Array {
  const img = new Uint8Array(size);
  new DataView(img.buffer).setUint32(32, 0xabcd5432, true);
  img.set(elfHash, 32 + 144);
  for (let i = 300; i < size; i++) img[i] = i & 0xff;
  return img;
}

const seed = new Uint8Array(32).fill(7);
const pub = bytesToHex(ed25519.getPublicKey(seed));
const elf = new Uint8Array(32).fill(0xab);
const img = image(elf);
const latest: Latest = { hash: bytesToHex(elf), size: img.length, built: 1794408200 };
const sig = ed25519.sign(signedMessage(img, latest.built), seed);

test('latest: as release-fw.py writes it', () => {
  assert.deepEqual(parseLatest(`${'a'.repeat(64)} 2139312 1794408200\n`), {
    hash: 'a'.repeat(64),
    size: 2139312,
    built: 1794408200,
  });
  assert.deepEqual(parseLatest(`${'a'.repeat(64)} 2139312 1794408200 48cfc3a\n`), {
    hash: 'a'.repeat(64),
    size: 2139312,
    built: 1794408200,
    commit: '48cfc3a',
  });
  assert.equal(parseLatest('nonsense'), null);
  assert.equal(parseLatest(`${'a'.repeat(63)} 1 2`), null);
});

test('the signed message: SHA-256, then the build time little-endian', () => {
  const m = signedMessage(new Uint8Array(0), 0x0102030405);
  assert.equal(m.length, 40);
  assert.deepEqual([...m.subarray(32)], [5, 4, 3, 2, 1, 0, 0, 0]);
});

test('a release signed by the gauge key passes', () => {
  assert.equal(checkRelease(img, sig, latest, pub), null);
});

test('anything else does not', () => {
  const other = bytesToHex(ed25519.getPublicKey(new Uint8Array(32).fill(9)));
  assert.equal(checkRelease(img, sig, latest, other), 'bad signature');
  assert.equal(checkRelease(img, sig, latest, ''), 'the gauge has no update key');
  const tampered = img.slice();
  tampered[1000] ^= 1;
  assert.equal(checkRelease(tampered, sig, latest, pub), 'bad signature');
  // An older build time passed off as new: the signature covers it.
  assert.equal(checkRelease(img, sig, { ...latest, built: latest.built + 1 }, pub), 'bad signature');
  assert.equal(checkRelease(img, sig, { ...latest, size: 1 }, pub), 'the image is not its size');
  assert.equal(checkRelease(img, sig, { ...latest, hash: 'c'.repeat(64) }, pub), 'not the image it says');
  assert.equal(checkRelease(new Uint8Array(img.length), sig, latest, pub), 'not a gauge firmware');
});

test('newer: built later and not the running build', () => {
  assert.ok(isNewer(latest, { hash: '11112222', built: latest.built - 1 }));
  assert.ok(!isNewer(latest, { hash: '11112222', built: latest.built }));
  assert.ok(!isNewer(latest, { hash: latest.hash.slice(0, 8), built: 0 }));
  assert.ok(isNewer(latest, { hash: '11112222' }));            // older firmware: no build time
});
