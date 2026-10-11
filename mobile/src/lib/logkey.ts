// This phone's log key: the X25519 secret that opens the pod's encrypted logs
// (logcrypt.ts; design in DECISIONS.md). The pod only ever gets the public
// half. The secret lives in SecureStore (the Android Keystore) and leaves the
// phone only when exported - once, into KeePass: without it, every encrypted
// log is lost with the phone.

import * as Clipboard from 'expo-clipboard';
import * as Crypto from 'expo-crypto';
import * as SecureStore from 'expo-secure-store';

import { fingerprintSpaced, keyToText, publicKeyOf, toHex } from './logcrypt';

const STORE_KEY = 'logSecret';

export type LogKey = {
  secret: Uint8Array;
  pub: Uint8Array;
  /** "7d12 5bce 8295" - the pod's Device page shows the same for its key. */
  fingerprint: string;
};

function wrap(secret: Uint8Array): LogKey {
  const pub = publicKeyOf(secret);
  return { secret, pub, fingerprint: fingerprintSpaced(pub) };
}

let cached: LogKey | null | undefined;

export async function loadLogKey(): Promise<LogKey | null> {
  if (cached !== undefined) return cached;
  const hex = await SecureStore.getItemAsync(STORE_KEY);
  cached = hex && /^[0-9a-f]{64}$/.test(hex) ? wrap(Uint8Array.from(hex.match(/../g)!.map((h) => parseInt(h, 16)))) : null;
  return cached;
}

async function store(secret: Uint8Array): Promise<LogKey> {
  await SecureStore.setItemAsync(STORE_KEY, toHex(secret));
  cached = wrap(secret);
  return cached;
}

/** A new key from the OS's secure random source. */
export function createLogKey(): Promise<LogKey> {
  return store(Crypto.getRandomBytes(32));
}

/** A key from KeePass (keyFromText), e.g. on a new phone. */
export function importLogKey(secret: Uint8Array): Promise<LogKey> {
  return store(secret);
}

const CLEAR_AFTER_MS = 60_000;

/**
 * Copies the key as text for KeePass, and clears the clipboard a minute
 * later if it still holds it: Android cannot be told the copy is sensitive
 * here, so it may show it in the clipboard preview.
 */
export async function copyLogKey(k: LogKey): Promise<void> {
  const text = keyToText(k.secret);
  await Clipboard.setStringAsync(text);
  setTimeout(async () => {
    try {
      if ((await Clipboard.getStringAsync()) === text) await Clipboard.setStringAsync('');
    } catch {}
  }, CLEAR_AFTER_MS);
}
