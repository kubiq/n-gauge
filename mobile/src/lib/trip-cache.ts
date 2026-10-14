// Trip summaries, worked out once per file and cached next to the inbox.
// Parsing a 17 MB GPX takes a second or two on the phone; its summary is a
// few hundred bytes, so the trip list opens at once after the first time.
// The thinned track is cached on its own, so the list never loads tracks.

import { Directory, File, Paths } from 'expo-file-system';

import { legacyRenames } from './legacy-names';
import { readLogText } from './logtext';
import { atlasNames } from './place-atlas';
import { deleteFromInbox, inboxFile, loadInbox, renameInbox } from './store';
import {
  type CsvSummary,
  type GpxSummary,
  isCsvName,
  isGpxName,
  pairTrips,
  parseCsv,
  parseGpx,
  type TrackPoint,
  type TripFiles,
} from './trips';

const dir = new Directory(Paths.document, 'trips');

type Cached<T> = { size: number; v: number; summary: T };
/** A GPX summary as cached: no track, and the places its ends are nearest (place-atlas.ts). */
export type GpxStats = Omit<GpxSummary, 'track'> & { fromPlace: string | null; toPlace: string | null };

// Bump when a summary's shape or its maths change, so old caches are redone.
// 2: bad fixes dropped from the track, ends and top speed (2026-10-13).
// 3: the bare <speed> of September's files read (2026-10-13).
// 4: the drive's two ends and the places nearest them, for its name (2026-10-14).
const VERSION = 4;

function read<T>(f: File, size: number): T | null {
  if (!f.exists) return null;
  try {
    const c: Cached<T> = JSON.parse(f.textSync());
    return c.size === size && c.v === VERSION ? c.summary : null;
  } catch {
    return null;
  }
}

function write<T>(f: File, size: number, summary: T) {
  f.write(JSON.stringify({ size, v: VERSION, summary } satisfies Cached<T>));
}

/**
 * Session ids for logs pulled before the pod named files with them
 * (legacy-names.ts): the inbox and the cached summaries, so nothing is
 * worked out again. A no-op once done; run at start-up.
 */
export function renameLegacyLogs(): void {
  const renames = legacyRenames(Object.keys(loadInbox()).filter((n) => inboxFile(n).exists));
  renameInbox(renames);
  for (const [from, to] of Object.entries(renames)) {
    for (const ext of ['.json', '.track.json']) {
      const src = new File(dir, from + ext);
      const dst = new File(dir, to + ext);
      if (!src.exists) continue;
      if (dst.exists) src.delete();
      else src.moveSync(dst);
    }
  }
}

/**
 * A drive off the phone: its logs and their cached summaries. The gauge's
 * card is not touched - a file still there comes back with the next sync.
 */
export function deleteDrive(t: TripFiles): void {
  const names = [t.gpx, t.csv].filter((n): n is string => !!n);
  deleteFromInbox(names);
  for (const n of names)
    for (const ext of ['.json', '.track.json']) {
      const f = new File(dir, n + ext);
      if (f.exists) f.delete();
    }
}

/** The drives the phone holds, newest first. */
export function tripFiles(): TripFiles[] {
  return pairTrips(Object.keys(loadInbox()).filter((n) => inboxFile(n).exists));
}

/** Cached stats of one GPX, or null if it has to be worked out first. */
export function cachedGpx(name: string): GpxStats | null {
  return read<GpxStats>(new File(dir, `${name}.json`), inboxFile(name).size);
}

export function cachedCsv(name: string): CsvSummary | null {
  return read<CsvSummary>(new File(dir, `${name}.json`), inboxFile(name).size);
}

/** Works out and caches what is missing or stale for one file. */
export async function summarise(name: string): Promise<void> {
  if (!dir.exists) dir.create({ intermediates: true });
  const size = inboxFile(name).size;
  // Encrypted or plain alike (readLogText); the cache holds only the results,
  // so an encrypted log is decrypted once, like a plain one is parsed once.
  if (isGpxName(name)) {
    if (cachedGpx(name)) return;
    const { track, ...summary } = parseGpx(await readLogText(name));
    // Named here, once: a place list is read for it and let go, never held
    // while the drives are shown. A list that will not read leaves the
    // drive with its date for a title, not without a summary.
    const [fromPlace, toPlace] = await atlasNames([summary.from, summary.to]).catch(() => [null, null]);
    const stats: GpxStats = { ...summary, fromPlace, toPlace };
    write(new File(dir, `${name}.track.json`), size, track);
    write(new File(dir, `${name}.json`), size, stats);
  } else if (isCsvName(name)) {
    if (cachedCsv(name)) return;
    write(new File(dir, `${name}.json`), size, parseCsv(await readLogText(name)));
  }
}

export async function trackOf(name: string): Promise<TrackPoint[] | null> {
  const f = new File(dir, `${name}.track.json`);
  let t = read<TrackPoint[]>(f, inboxFile(name).size);
  if (!t) {
    await summarise(name);
    t = read<TrackPoint[]>(f, inboxFile(name).size);
  }
  return t;
}
