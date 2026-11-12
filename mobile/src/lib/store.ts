// What the phone keeps between syncs.
//
// - The pod's WiFi credentials, from scanning its QR (SecureStore: the
//   passphrase is the only thing keeping strangers off the pod's settings).
// - The inbox: the phone's copy of the pod's logs, each file whole from
//   byte 0, and per file how many bytes of it the phone has - what planPull()
//   is told we already have, so a sync knows what is new.

import { Directory, File, FileMode, Paths } from 'expo-file-system';
import * as SecureStore from 'expo-secure-store';

import { t } from '../i18n/index.ts';
import { asFuel, type Fuel, type Fuels } from './fuel-grade';
import { GPX_TAIL } from './gpx-share';
import { type Downloader, downloadRange, type PodUpload } from './pod';
import type { PodSettings } from './settings-schema';
import { isPodLog, type Known, type Pull } from './sync-plan';
import type { WifiCreds } from './wifi-qr';

const CREDS_KEY = 'podWifi';

export async function loadCreds(): Promise<WifiCreds | null> {
  const s = await SecureStore.getItemAsync(CREDS_KEY);
  return s ? JSON.parse(s) : null;
}

export async function saveCreds(c: WifiCreds): Promise<void> {
  await SecureStore.setItemAsync(CREDS_KEY, JSON.stringify(c));
}

/** One inbox file: inbox/<name> is `bytes` long, from byte 0 of the pod's. */
export type InboxEntry = { bytes: number };
export type Inbox = Record<string, InboxEntry>;

const inboxDir = new Directory(Paths.document, 'inbox');
const stateFile = new File(Paths.document, 'inbox.json');
const partsDir = new Directory(Paths.cache, 'pulls');
const lastSyncFile = new File(Paths.document, 'last-sync.txt');

/** When a sync last left nothing to pull (ms since epoch), or null. */
export function loadLastSync(): number | null {
  return lastSyncFile.exists ? Number(lastSyncFile.textSync()) || null : null;
}

export function saveLastSync(t: number): void {
  lastSyncFile.write(String(t));
}

// Leave the gauge's WiFi once a sync is done, so the phone is back on the
// internet (maps, place names) - unless a gauge screen is open. On unless
// switched off: the file exists only for "off".
const keepConnectedFile = new File(Paths.document, 'keep-connected.txt');

export function loadDisconnectAfterSync(): boolean {
  return !keepConnectedFile.exists;
}

export function saveDisconnectAfterSync(on: boolean): void {
  if (on) {
    if (keepConnectedFile.exists) keepConnectedFile.delete();
  } else keepConnectedFile.write('1');
}

// Which optional figures a drive shows in the trips list (Settings, "Trip
// list"). All on unless switched off; a missing or unreadable file is that,
// and a figure added later is on for a phone that saved the rest.
const listFactsFile = new File(Paths.document, 'list-facts.json');

export type ListFacts = { boost: boolean; oil: boolean; fuelL: boolean; per100: boolean };
const LIST_FACTS_DEFAULT: ListFacts = { boost: true, oil: true, fuelL: true, per100: true };

export function loadListFacts(): ListFacts {
  try {
    return listFactsFile.exists
      ? { ...LIST_FACTS_DEFAULT, ...JSON.parse(listFactsFile.textSync()) }
      : LIST_FACTS_DEFAULT;
  } catch {
    return LIST_FACTS_DEFAULT;
  }
}

export function saveListFacts(f: ListFacts): void {
  listFactsFile.write(JSON.stringify(f));
}

// The gauge's firmware build as last read from it, and when - kept, since the
// app is on the gauge's WiFi only for a sync (Settings, About shows it).
const gaugeFwFile = new File(Paths.document, 'gauge-fw.json');

// Since 2026-11-11 also its hash, build time and update key (firmware from
// then on says them): a release from GitHub is checked against them while
// the phone has internet - off the gauge's WiFi (fw-release.ts).
export type GaugeFw = { build: string; at: number; hash?: string; built?: number; pub?: string };

export function loadGaugeFw(): GaugeFw | null {
  try {
    return gaugeFwFile.exists ? JSON.parse(gaugeFwFile.textSync()) : null;
  } catch {
    return null;
  }
}

export function saveGaugeFw(fw: { build: string; hash?: string; built?: number; pub?: string }): void {
  gaugeFwFile.write(
    JSON.stringify({ build: fw.build, hash: fw.hash, built: fw.built, pub: fw.pub, at: Date.now() } satisfies GaugeFw),
  );
}

// Where "Save to a folder" writes: the URI Android's folder picker returned.
// The picker took a persistable permission on it, so it stays usable across
// restarts - until the folder is deleted or the permission revoked.
const logsFolderFile = new File(Paths.document, 'logs-folder.txt');

export function loadLogsFolder(): string | null {
  return logsFolderFile.exists ? logsFolderFile.textSync() || null : null;
}

export function saveLogsFolder(uri: string | null): void {
  if (uri) logsFolderFile.write(uri);
  else if (logsFolderFile.exists) logsFolderFile.delete();
}

// Save new logs into that folder after each sync (use-sync.tsx). Off unless
// switched on: the file exists only for "on".
const autoSaveFile = new File(Paths.document, 'auto-save.txt');

export function loadAutoSave(): boolean {
  return autoSaveFile.exists;
}

export function saveAutoSave(on: boolean): void {
  if (on) autoSaveFile.write('1');
  else if (autoSaveFile.exists) autoSaveFile.delete();
}

// The gauge's settings as last read from it, for Gauge settings off its WiFi
// (asked 2026-11-12: they could not be read without the gauge) - shown
// read-only, with when. Each part as it was read; a newer read replaces it.
const gaugeSnapFile = new File(Paths.document, 'gauge-snapshot.json');

export type GaugeSnapshot = { settings?: PodSettings; settingsAt?: number; upload?: PodUpload; uploadAt?: number };

export function loadGaugeSnapshot(): GaugeSnapshot {
  try {
    return gaugeSnapFile.exists ? JSON.parse(gaugeSnapFile.textSync()) : {};
  } catch {
    return {};
  }
}

export function saveGaugeSettings(settings: PodSettings): void {
  gaugeSnapFile.write(JSON.stringify({ ...loadGaugeSnapshot(), settings, settingsAt: Date.now() }));
}

export function saveGaugeUpload(upload: PodUpload): void {
  gaugeSnapFile.write(JSON.stringify({ ...loadGaugeSnapshot(), upload, uploadAt: Date.now() }));
}

// The fuel picked at each fill-up (fuel-grade.ts), by its id. Only what was
// picked: a fill-up not in it has no fuel set.
const fuelsFile = new File(Paths.document, 'fuels.json');

export function loadFuels(): Fuels {
  try {
    const raw = fuelsFile.exists ? JSON.parse(fuelsFile.textSync()) : {};
    const out: Fuels = {};
    for (const [id, v] of Object.entries(raw ?? {})) {
      const f = asFuel(v);
      if (f) out[id] = f;
    }
    return out;
  } catch {
    return {};
  }
}

/** The fuel of one fill-up; null clears it. Returns them all, as saved. */
export function saveFuel(id: string, fuel: Fuel | null): Fuels {
  const all = loadFuels();
  if (fuel) all[id] = fuel;
  else delete all[id];
  fuelsFile.write(JSON.stringify(all));
  return all;
}

function ensureDir() {
  if (!inboxDir.exists) inboxDir.create({ intermediates: true });
}

// Until 2026-10-18 an entry split its bytes into `uploaded` (to the log
// server, since removed) and `pending`. Read as their sum, and written back
// in the new form once, so a phone that has synced keeps its files - none is
// pulled again.
type OldInboxEntry = { uploaded?: number; pending?: number; bytes?: number };

export function loadInbox(): Inbox {
  if (!stateFile.exists) return {};
  const raw: Record<string, OldInboxEntry> = JSON.parse(stateFile.textSync());
  const inbox: Inbox = {};
  let old = false;
  for (const [name, e] of Object.entries(raw)) {
    if (e.bytes === undefined) old = true;
    inbox[name] = { bytes: e.bytes ?? (e.uploaded ?? 0) + (e.pending ?? 0) };
  }
  if (old) saveInbox(inbox);
  return inbox;
}

function saveInbox(s: Inbox) {
  stateFile.write(JSON.stringify(s));
}

export function knownBytes(s: Inbox): Known {
  const k: Known = {};
  for (const [name, e] of Object.entries(s)) k[name] = e.bytes;
  return k;
}

export function inboxFile(name: string): File {
  return new File(inboxDir, name);
}

/**
 * Pulls one planned file into the inbox - from the pod, or with `download`
 * from the home server (home.ts): the same files, by the same names.
 */
export async function pull(
  p: Pull,
  onProgress?: (bytes: number) => void,
  signal?: AbortSignal,
  download: Downloader = downloadRange,
): Promise<void> {
  ensureDir();
  const state = loadInbox();
  const dest = inboxFile(p.name);
  const had = p.restart ? 0 : (state[p.name]?.bytes ?? 0);
  if (p.restart && dest.exists) dest.delete();
  if (p.from !== had) {
    throw new Error(t('{name}: planned from {from}, but the inbox ends at {end}', { name: p.name, from: p.from, end: had }));
  }

  // A GPX the pod is still writing ends in closing tags that it overwrites
  // with the next batch (gpxWriteTail() in log_gpx.cpp). Continuing after them
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
  const { whole } = await download(p.name, from, p.size, tmp, onProgress, signal);
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

  state[p.name] = { bytes: from + got };
  saveInbox(state);
}

// Names deleted from the phone on purpose, still to be deleted on the gauge.
// The sync never pulls them (planPull) and asks the gauge to delete each
// on every connect (use-sync.tsx), until it has or the file is gone.
const deletedFile = new File(Paths.document, 'delete-on-gauge.json');

export function loadDeleted(): string[] {
  try {
    return deletedFile.exists ? JSON.parse(deletedFile.textSync()) : [];
  } catch {
    return [];
  }
}

export function saveDeleted(names: string[]): void {
  deletedFile.write(JSON.stringify([...new Set(names)]));
}

// The same, still to be deleted on the home server (home.ts) - kept apart:
// the gauge and the server each take their names off when they have.
const homeDeletedFile = new File(Paths.document, 'delete-on-home.json');

export function loadHomeDeleted(): string[] {
  try {
    return homeDeletedFile.exists ? JSON.parse(homeDeletedFile.textSync()) : [];
  } catch {
    return [];
  }
}

export function saveHomeDeleted(names: string[]): void {
  homeDeletedFile.write(JSON.stringify([...new Set(names)]));
}

// The last home-server sync that finished (ms), and what it said - for the
// phone's Settings.
const lastHomeFile = new File(Paths.document, 'last-home-sync.json');
export type HomeSyncResult = { at: number; files: number; error?: string };

export function loadLastHomeSync(): HomeSyncResult | null {
  try {
    return lastHomeFile.exists ? JSON.parse(lastHomeFile.textSync()) : null;
  } catch {
    return null;
  }
}

export function saveLastHomeSync(r: HomeSyncResult): void {
  lastHomeFile.write(JSON.stringify(r));
}

/** Deletes inbox files and their records - the phone's copy only. */
export function deleteFromInbox(names: string[]): void {
  const state = loadInbox();
  for (const n of names) {
    const f = inboxFile(n);
    if (f.exists) f.delete();
    delete state[n];
  }
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

