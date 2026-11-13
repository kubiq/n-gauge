// A firmware release, checked before it goes to the gauge (asked 2026-11-11):
// the image, its signature and `latest` ("<app hash> <size> <build time> <commit>"),
// as scripts/release-fw.py makes them in CI for a GitHub release. The signature is standard Ed25519 over the image's SHA-256 and its
// build time (8 bytes, little-endian) - what the gauge checks for its own
// downloads (upload.cpp). Here against the key the gauge itself trusts (its
// /api/live fw.pub), since an upload from the phone is not checked there.
//
// No I/O here, so the tests can run it; fw-download.ts fetches and keeps.

import { ed25519 } from '@noble/curves/ed25519.js';
import { sha256 } from '@noble/hashes/sha2.js';
import { bytesToHex, hexToBytes } from '@noble/hashes/utils.js';

/** commit: the build's git commit, as its stamp shows it - absent in releases before 2026-11-11. */
export type Latest = { hash: string; size: number; built: number; commit?: string };

/** `latest` as release-fw.py writes it, or null. */
export function parseLatest(text: string): Latest | null {
  const m = /^([0-9a-f]{64}) (\d+) (\d+)(?: (\S+))?\s*$/.exec(text);
  if (!m) return null;
  const l: Latest = { hash: m[1], size: Number(m[2]), built: Number(m[3]) };
  if (m[4]) l.commit = m[4];
  return l;
}

// esp_app_desc_t follows the image header (24 bytes) and the first segment
// header (8): its magic word, then the ELF's SHA-256 at 144 into it.
const APP_DESC = 24 + 8;
const APP_DESC_MAGIC = 0xabcd5432;
const ELF_SHA_AT = APP_DESC + 144;

/** What the signature covers: the image's SHA-256, then its build time. */
export function signedMessage(image: Uint8Array, built: number): Uint8Array {
  const msg = new Uint8Array(40);
  msg.set(sha256(image), 0);
  let b = BigInt(built);
  for (let i = 0; i < 8; i++, b >>= 8n) msg[32 + i] = Number(b & 0xffn);
  return msg;
}

/** null when the release is whole and signed by `pubHex`; else why not, in English. */
export function checkRelease(image: Uint8Array, sig: Uint8Array, latest: Latest, pubHex: string): string | null {
  if (!/^[0-9a-f]{64}$/.test(pubHex)) return 'the gauge has no update key';
  if (image.length !== latest.size) return 'the image is not its size';
  const view = new DataView(image.buffer, image.byteOffset, image.byteLength);
  if (image.length < ELF_SHA_AT + 32 || view.getUint32(APP_DESC, true) !== APP_DESC_MAGIC)
    return 'not a gauge firmware';
  if (bytesToHex(image.subarray(ELF_SHA_AT, ELF_SHA_AT + 32)) !== latest.hash) return 'not the image it says';
  if (sig.length !== 64) return 'no signature';
  try {
    if (!ed25519.verify(sig, signedMessage(image, latest.built), hexToBytes(pubHex))) return 'bad signature';
  } catch {
    return 'bad signature';
  }
  return null;
}

/**
 * Whether a release is worth installing on the gauge: built after what it
 * runs, as the gauge itself decides (no downgrade), and not that build. The
 * gauge's hash is the short one it reports (8 hex digits). Without a build
 * time (firmware before 2026-11-11), any other build counts.
 */
export function isNewer(latest: Latest, gauge: { hash?: string; built?: number }): boolean {
  if (gauge.hash && latest.hash.startsWith(gauge.hash)) return false;
  return gauge.built === undefined || gauge.built === 0 || latest.built > gauge.built;
}
