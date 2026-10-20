// Trip summaries, worked out once per file and cached next to the inbox.
// Parsing a 17 MB GPX takes a second or two on the phone; its summary is a
// few hundred bytes, so the trip list opens at once after the first time.
// The thinned track is cached on its own, so the list never loads tracks.

import { Directory, File, FileMode, Paths } from 'expo-file-system';

import { legacyRenames } from './legacy-names';
import { readLogText } from './logtext';
import { atlasNames } from './place-atlas';
import { csvSeries, type Series } from './series';
import { deleteFromInbox, inboxFile, loadDeleted, loadInbox, renameInbox, saveDeleted } from './store';
import {
  type CsvSummary,
  type GpxSummary,
  isCsvName,
  isGpxName,
  pairTrips,
  parseCsv,
  parseGpx,
  staleCopyCandidates,
  type TrackPoint,
  type TripFiles,
} from './trips';

const dir = new Directory(Paths.document, 'trips');
// What is cached per log: the summary, a GPX's thinned track, a CSV's series.
const CACHE_EXTS = ['.json', '.track.json', '.series.json'];

type Cached<T> = { size: number; v: number; summary: T };
/** A GPX summary as cached: no track, and the places its ends are nearest (place-atlas.ts). */
export type GpxStats = Omit<GpxSummary, 'track'> & { fromPlace: string | null; toPlace: string | null };

// Bump when a summary's shape or its maths change, so old caches are redone.
// 2: bad fixes dropped from the track, ends and top speed (2026-10-13).
// 3: the bare <speed> of September's files read (2026-10-13).
// 4: the drive's two ends and the places nearest them, for its name (2026-10-14).
// 5: CSV lines split on CRLF - an empty last column (voltage) was 0 (2026-10-14).
// 6: ends in a Czech city named by the quarter they lie in (2026-10-15).
// 7: track points keep their time, the series its start: the chart's cursor
//    shown on the map (2026-10-15).
// 8: fuel burned and the distance it is divided by, from the CSV (2026-10-20).
const VERSION = 8;

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
    for (const ext of CACHE_EXTS) {
      const src = new File(dir, from + ext);
      const dst = new File(dir, to + ext);
      if (!src.exists) continue;
      if (dst.exists) src.delete();
      else src.moveSync(dst);
    }
  }
}

/**
 * A drive gone for good: its logs and cached summaries off the phone, and
 * its names on the "delete on the gauge" list - the next sync deletes them
 * from the card (now, if connected) and never pulls them back meanwhile.
 */
export function deleteDrive(t: TripFiles): void {
  const names = [t.gpx, t.csv].filter((n): n is string => !!n);
  saveDeleted([...loadDeleted(), ...names]);
  deleteFromInbox(names);
  for (const n of names)
    for (const ext of CACHE_EXTS) {
      const f = new File(dir, n + ext);
      if (f.exists) f.delete();
    }
}

/** Is `a` byte for byte the beginning of `b`? Read in chunks - `b` may be a big GPX. */
function isPrefix(a: File, b: File): boolean {
  const n = a.size ?? 0;
  if (n === 0 || (b.size ?? 0) < n) return false;
  const ha = a.open(FileMode.ReadOnly);
  const hb = b.open(FileMode.ReadOnly);
  try {
    for (let done = 0; done < n; ) {
      const len = Math.min(65536, n - done);
      const x = ha.readBytes(len), y = hb.readBytes(len);
      for (let i = 0; i < len; i++) if (x[i] !== y[i]) return false;
      done += len;
    }
    return true;
  } finally {
    ha.close();
    hb.close();
  }
}

/**
 * Removes logs held twice: a numbered copy synced before the gauge renamed
 * the file to its dated name (trips.ts staleCopyCandidates) - only when its
 * bytes are exactly the dated file's beginning, so a real log is never
 * taken for one. The phone's copy only: the gauge no longer has that name.
 * Run at start and after each sync. Returns what went.
 */
export function dropStaleCopies(): string[] {
  const names = Object.keys(loadInbox()).filter((n) => inboxFile(n).exists);
  const drop = staleCopyCandidates(names, (n) => inboxFile(n).size ?? 0)
    .filter(([copy, dated]) => isPrefix(inboxFile(copy), inboxFile(dated)))
    .map(([copy]) => copy);
  if (!drop.length) return [];
  deleteFromInbox(drop);
  for (const n of drop)
    for (const ext of CACHE_EXTS) {
      const f = new File(dir, n + ext);
      if (f.exists) f.delete();
    }
  return drop;
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
    // The chart's series from the same read: a drive opened later needs
    // neither the file nor its decryption again (seriesOf).
    const text = await readLogText(name);
    write(new File(dir, `${name}.series.json`), size, csvSeries(text));
    write(new File(dir, `${name}.json`), size, parseCsv(text));
  }
}

/**
 * A drive's readings over time, for its charts. Cached with the summary; a
 * CSV summarised before the charts existed gets its series here, once.
 */
export async function seriesOf(name: string): Promise<Series | null> {
  const f = new File(dir, `${name}.series.json`);
  const size = inboxFile(name).size;
  const have = read<Series>(f, size);
  if (have) return have;
  if (!dir.exists) dir.create({ intermediates: true });
  const s = csvSeries(await readLogText(name));
  write(f, size, s);
  return s;
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
