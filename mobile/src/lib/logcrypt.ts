// The pod's encrypted log format, on the phone. The format and its reasons
// are in firmware/car_gauge_v1/logcrypt.h (the pod's writer) and DECISIONS.md;
// this is the same thing in TypeScript, held to the same bytes by the shared
// test vector tests/vectors/logcrypt-v1.json.
//
//   file   = block*,  block = type (1) | length (u32 LE) | body
//   0x01   header: "NGE1" | 0x01 | eph_pub (32)
//   0x02   record: ciphertext | mac (16), XChaCha20-Poly1305, AD = header body
//   key    = BLAKE2b-256(X25519(secret, eph_pub) | eph_pub | our_pub | "n-gauge log v1")
//   nonce  = record index in its segment, u64 LE, then 16 zero bytes

import { xchacha20poly1305 } from '@noble/ciphers/chacha.js';
import { x25519 } from '@noble/curves/ed25519.js';
import { blake2b } from '@noble/hashes/blake2.js';

const TYPE_HEADER = 0x01;
const TYPE_RECORD = 0x02;
const VERSION = 0x01;
const MAGIC = [0x4e, 0x47, 0x45, 0x31]; // "NGE1"
const INFO = new TextEncoder().encode('n-gauge log v1');
const HEADER_BODY = 4 + 1 + 32;
const MAC = 16;
const BLOCK_HEAD = 5;
const MAX_RECORD = 256 * 1024;

function u32(b: Uint8Array, at: number): number {
  return (b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (b[at + 3] << 24)) >>> 0;
}

function nonce(counter: number): Uint8Array {
  const n = new Uint8Array(24);
  // Counters stay far below 2^53; written as u64 LE.
  for (let i = 0, c = counter; i < 8; i++, c = Math.floor(c / 256)) n[i] = c % 256;
  return n;
}

function derive(shared: Uint8Array, ephPub: Uint8Array, ourPub: Uint8Array): Uint8Array {
  const msg = new Uint8Array(96 + INFO.length);
  msg.set(shared, 0);
  msg.set(ephPub, 32);
  msg.set(ourPub, 64);
  msg.set(INFO, 96);
  return blake2b(msg, { dkLen: 32 });
}

/** True if the bytes start like one of the pod's encrypted logs. */
export function isEncrypted(head: Uint8Array): boolean {
  return (
    head.length >= BLOCK_HEAD + 5 &&
    head[0] === TYPE_HEADER &&
    u32(head, 1) === HEADER_BODY &&
    MAGIC.every((m, i) => head[BLOCK_HEAD + i] === m)
  );
}

export type Opened = {
  plain: Uint8Array;
  /** How far the file was valid; less than its length after a cut-off last record. */
  consumed: number;
};

/**
 * Decrypts a whole file. Stops - without throwing - at the first block that
 * is cut short or fails its MAC: that is the end of the file as far as the
 * reader is concerned (the pod lost power mid-write, or the copy is partial).
 */
export function openLog(file: Uint8Array, secret: Uint8Array): Opened {
  const ourPub = x25519.getPublicKey(secret);
  const parts: Uint8Array[] = [];
  let key: Uint8Array | null = null;
  let ad: Uint8Array | null = null;
  let counter = 0;
  let at = 0;
  while (at + BLOCK_HEAD <= file.length) {
    const type = file[at];
    const len = u32(file, at + 1);
    if (len > MAX_RECORD || at + BLOCK_HEAD + len > file.length) break;
    const body = file.subarray(at + BLOCK_HEAD, at + BLOCK_HEAD + len);
    if (type === TYPE_HEADER) {
      if (len !== HEADER_BODY || !MAGIC.every((m, i) => body[i] === m) || body[4] !== VERSION) break;
      const ephPub = body.subarray(5, 37);
      key = derive(x25519.getSharedSecret(secret, ephPub), ephPub, ourPub);
      ad = body.slice();
      counter = 0;
    } else if (type === TYPE_RECORD && key && ad && len >= MAC) {
      try {
        parts.push(xchacha20poly1305(key, nonce(counter), ad).decrypt(body));
      } catch {
        break; // MAC mismatch: corrupt or cut, the end
      }
      counter++;
    } else {
      break;
    }
    at += BLOCK_HEAD + len;
  }
  const plain = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) {
    plain.set(p, o);
    o += p.length;
  }
  return { plain, consumed: at };
}

/**
 * Writer, as the pod does it - for the tests (round trips and the shared
 * vector); the pod is the only thing that writes these for real.
 */
export function sealSegment(recipientPub: Uint8Array, ephSecret: Uint8Array, records: Uint8Array[]): Uint8Array {
  const ephPub = x25519.getPublicKey(ephSecret);
  const key = derive(x25519.getSharedSecret(ephSecret, recipientPub), ephPub, recipientPub);
  const ad = new Uint8Array(HEADER_BODY);
  ad.set(MAGIC, 0);
  ad[4] = VERSION;
  ad.set(ephPub, 5);
  const blocks = [block(TYPE_HEADER, ad)];
  records.forEach((r, i) => blocks.push(block(TYPE_RECORD, xchacha20poly1305(key, nonce(i), ad).encrypt(r))));
  const out = new Uint8Array(blocks.reduce((n, b) => n + b.length, 0));
  let o = 0;
  for (const b of blocks) {
    out.set(b, o);
    o += b.length;
  }
  return out;
}

function block(type: number, body: Uint8Array): Uint8Array {
  const b = new Uint8Array(BLOCK_HEAD + body.length);
  b[0] = type;
  b[1] = body.length & 255;
  b[2] = (body.length >>> 8) & 255;
  b[3] = (body.length >>> 16) & 255;
  b[4] = (body.length >>> 24) & 255;
  b.set(body, BLOCK_HEAD);
  return b;
}

/** First 6 bytes of BLAKE2b-256(pub), hex - shown on the pod and in the app to compare. */
export function fingerprint(pub: Uint8Array): string {
  return toHex(blake2b(pub, { dkLen: 32 }).subarray(0, 6));
}

/** "7d12 5bce 8295", as the pod's Device page shows it. */
export function fingerprintSpaced(pub: Uint8Array): string {
  return fingerprint(pub).replace(/(.{4})(?=.)/g, '$1 ');
}

export function toHex(b: Uint8Array): string {
  return Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
}

export function publicKeyOf(secret: Uint8Array): Uint8Array {
  return x25519.getPublicKey(secret);
}

// ---- the key as text, for KeePass -------------------------------------------
// One line, so it pastes cleanly into a password manager's field. The prefix
// says what it is and which format it reads; the importer also takes the bare
// 64 hex digits, in case the prefix got lost on the way.
export const KEY_TEXT_PREFIX = 'ngauge-log-key-v1:';

export function keyToText(secret: Uint8Array): string {
  return KEY_TEXT_PREFIX + toHex(secret);
}

/** The secret from exported text, or null if it is not one. */
export function keyFromText(text: string): Uint8Array | null {
  let t = text.trim();
  if (t.toLowerCase().startsWith(KEY_TEXT_PREFIX)) t = t.slice(KEY_TEXT_PREFIX.length);
  if (!/^[0-9a-fA-F]{64}$/.test(t)) return null;
  return Uint8Array.from(t.match(/../g)!.map((h) => parseInt(h, 16)));
}

/**
 * Bytes to text. TextDecoder where the runtime has it; otherwise a plain
 * UTF-8 decoder - the logs are ASCII but for one '¬' in the .vbo header.
 */
export function bytesToText(b: Uint8Array): string {
  if (typeof TextDecoder !== 'undefined') return new TextDecoder().decode(b);
  const out: string[] = [];
  let chunk = '';
  for (let i = 0; i < b.length; ) {
    let c = b[i++];
    if (c >= 0xc0 && c < 0xe0 && i < b.length) c = ((c & 0x1f) << 6) | (b[i++] & 0x3f);
    else if (c >= 0xe0 && c < 0xf0 && i + 1 < b.length) c = ((c & 0x0f) << 12) | ((b[i++] & 0x3f) << 6) | (b[i++] & 0x3f);
    chunk += String.fromCharCode(c);
    if (chunk.length >= 8192) {
      out.push(chunk);
      chunk = '';
    }
  }
  out.push(chunk);
  return out.join('');
}
