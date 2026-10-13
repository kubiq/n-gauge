// What the phone keeps between syncs.
//
// - The pod's WiFi credentials, from scanning its QR (SecureStore: the
//   passphrase is the only thing keeping strangers off the pod's settings).
// - The inbox: the phone's copy of the pod's logs, each file whole from
//   byte 0 - the trips read them, so they stay after an upload. Per file,
//   the first `uploaded` bytes are on the backend and the `pending` bytes
//   after them are not yet; inbox/<name> is uploaded + pending bytes long.
//   That sum is what planPull() is told we already have, so a sync at the
//   car needs no internet to know what is new.

import { Directory, File, FileMode, Paths } from 'expo-file-system';
import * as SecureStore from 'expo-secure-store';

import { type BackendConfig, BackendError, listServer, putChunk } from './backend';
import { GPX_TAIL } from './gpx-share';
import { downloadRange } from './pod';
import { isPodLog, type Known, type Pull } from './sync-plan';
import { type InboxEntry, planUpload } from './upload-plan';
import type { WifiCreds } from './wifi-qr';

const CREDS_KEY = 'podWifi';

export async function loadCreds(): Promise<WifiCreds | null> {
  const s = await SecureStore.getItemAsync(CREDS_KEY);
  return s ? JSON.parse(s) : null;
}

export async function saveCreds(c: WifiCreds): Promise<void> {
  await SecureStore.setItemAsync(CREDS_KEY, JSON.stringify(c));
}

export type { InboxEntry };
export type Inbox = Record<string, InboxEntry>;

const inboxDir = new Directory(Paths.document, 'inbox');
const stateFile = new File(Paths.document, 'inbox.json');
const partsDir = new Directory(Paths.cache, 'pulls');

function ensureDir() {
  if (!inboxDir.exists) inboxDir.create({ intermediates: true });
}

export function loadInbox(): Inbox {
  return stateFile.exists ? JSON.parse(stateFile.textSync()) : {};
}

function saveInbox(s: Inbox) {
  stateFile.write(JSON.stringify(s));
}

export function knownBytes(s: Inbox): Known {
  const k: Known = {};
  for (const [name, e] of Object.entries(s)) k[name] = e.uploaded + e.pending;
  return k;
}

export function inboxFile(name: string): File {
  return new File(inboxDir, name);
}

/** Pulls one planned file from the pod into the inbox. */
export async function pull(p: Pull, onProgress?: (bytes: number) => void, signal?: AbortSignal): Promise<void> {
  ensureDir();
  const state = loadInbox();
  const dest = inboxFile(p.name);
  let e = state[p.name] ?? { uploaded: 0, pending: 0 };
  if (p.restart) {
    if (dest.exists) dest.delete();
    e = { uploaded: 0, pending: 0 };
  }
  if (p.from !== e.uploaded + e.pending) {
    throw new Error(`${p.name}: planned from ${p.from}, but the inbox ends at ${e.uploaded + e.pending}`);
  }

  // A GPX the pod is still writing ends in closing tags that it overwrites
  // with the next batch (gpxCommit() in logger.cpp). Continuing after them
  // would leave them in the middle of our copy, so continue over them.
  let from = p.from;
  if (p.name.endsWith('.gpx') && !p.restart && dest.exists && endsWithGpxTail(dest)) from -= GPX_TAIL.length;

  // A fresh name per pull, and the leftovers of earlier ones cleared: after
  // a Cancel the native transfer may still be finishing into its old file.
  if (!partsDir.exists) partsDir.create({ intermediates: true });
  for (const old of partsDir.list()) old.delete();
  const tmp = new File(partsDir, `${Date.now()}.part`);
  // A cancelled or failed download rejects here, before the inbox is
  // touched: the file stays as it was and the next sync plans it again.
  const { whole } = await downloadRange(p.name, from, p.size, tmp, onProgress, signal);
  const skip = whole ? from : 0;
  const got = tmp.size - skip;

  if (!dest.exists && skip === 0) {
    tmp.moveSync(dest);
  } else {
    if (!dest.exists) dest.create();
    const src = tmp.open(FileMode.ReadOnly);
    // Read-write at `from`, not append: over a GPX's old tail it overwrites.
    const out = dest.open(FileMode.ReadWrite);
    try {
      src.offset = skip;
      out.offset = from;
      out.writeBytes(src.readBytes(got));
    } finally {
      src.close();
      out.close();
    }
    tmp.delete();
  }

  // The server's copy past `from` is stale too, if it got that far.
  e.uploaded = Math.min(e.uploaded, from);
  e.pending = from + got - e.uploaded;
  state[p.name] = e;
  saveInbox(state);
}

/**
 * Renames inbox files, old name -> new name. A file already there under the
 * new name (pulled again before the rename) wins; the old copy goes.
 */
export function renameInbox(renames: Record<string, string>): void {
  if (!Object.keys(renames).length) return;
  const state = loadInbox();
  for (const [from, to] of Object.entries(renames)) {
    const src = inboxFile(from);
    if (inboxFile(to).exists) src.delete();
    else src.moveSync(inboxFile(to));
    state[to] ??= state[from];
    delete state[from];
  }
  saveInbox(state);
}

function endsWithGpxTail(f: File): boolean {
  if (f.size < GPX_TAIL.length) return false;
  const h = f.open(FileMode.ReadOnly);
  try {
    h.offset = f.size - GPX_TAIL.length;
    const b = h.readBytes(GPX_TAIL.length);
    for (let i = 0; i < b.length; i++) if (b[i] !== GPX_TAIL.charCodeAt(i)) return false;
    return true;
  } finally {
    h.close();
  }
}

/**
 * Drops whatever is in the inbox that is not a pod log - firmware .bin files
 * pulled before planPull() filtered them. Returns the cleaned inbox.
 */
export function dropNonLogs(): Inbox {
  const state = loadInbox();
  let changed = false;
  for (const name of Object.keys(state)) {
    if (isPodLog(name)) continue;
    const f = inboxFile(name);
    if (f.exists) f.delete();
    delete state[name];
    changed = true;
  }
  if (changed) saveInbox(state);
  return state;
}

/** Bytes waiting in the inbox for an upload. */
export function pendingBytes(s: Inbox): number {
  return Object.values(s).reduce((n, e) => n + e.pending, 0);
}

/** Bytes the phone holds of the pod's logs, uploaded or not. */
export function storedBytes(s: Inbox): number {
  return Object.values(s).reduce((n, e) => n + e.uploaded + e.pending, 0);
}

/** Files with bytes waiting for an upload. */
export function pendingFiles(s: Inbox): number {
  return Object.values(s).filter((e) => e.pending > 0).length;
}

// Raw bytes per PUT: small enough to gzip without stalling the UI for long
// and to show progress on a 17 MB GPX, large enough that requests are few.
const UPLOAD_CHUNK = 4 * 1024 * 1024;

export type UploadResult = { files: number; bytes: number; problems: string[] };

/**
 * Sends everything waiting in the inbox to the server, smallest first; the
 * files stay on the phone. A file cut off part way keeps its inbox entry as
 * it was: the next upload sees the server ahead of it and continues from
 * there (planUpload sends from the server's size).
 */
export async function uploadInbox(
  c: BackendConfig,
  onProgress: (text: string) => void,
  signal: AbortSignal,
): Promise<UploadResult> {
  const server = await listServer(c);
  const state = loadInbox();
  const names = Object.keys(state)
    .filter((n) => state[n].pending > 0)
    .sort((a, b) => state[a].pending - state[b].pending);
  const res: UploadResult = { files: 0, bytes: 0, problems: [] };

  for (const [i, name] of names.entries()) {
    if (signal.aborted) break;
    const e = state[name];
    const step = planUpload(e, server[name], !name.endsWith('.gpx'));
    if (step.kind === 'send') {
      const total = e.uploaded + e.pending - step.offset;
      const src = inboxFile(name).open(FileMode.ReadOnly);
      try {
        src.offset = step.offset;
        let offset = step.offset;
        for (let sent = 0; sent < total && !signal.aborted; ) {
          onProgress(`${i + 1}/${names.length} ${name}: ${mbs(sent)} of ${mbs(total)}`);
          const n = Math.min(UPLOAD_CHUNK, total - sent);
          const size = await putChunk(c, name, offset, src.readBytes(n));
          if (size !== offset + n) throw new BackendError(`${name}: server size ${size}, expected ${offset + n}`);
          offset = size;
          sent += n;
          res.bytes += n;
        }
        if (signal.aborted) break;
      } catch (err) {
        if (err instanceof BackendError && err.status === 409) {
          // Someone else changed it in between; the next upload re-plans.
          res.problems.push(`${name}: changed on the server meanwhile - try again`);
          continue;
        }
        throw err;
      } finally {
        src.close();
      }
    }
    // Everything we hold is on the server now ('done' or a finished 'send').
    // The file itself stays: the trips read it.
    state[name] = { uploaded: e.uploaded + e.pending, pending: 0 };
    saveInbox(state);
    res.files++;
  }
  return res;
}

const mbs = (n: number) => `${(n / 1048576).toFixed(1)} MB`;
