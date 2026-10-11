// Trip summaries, worked out once per file and cached next to the inbox.
// Parsing a 17 MB GPX takes a second or two on the phone; its summary is a
// few hundred bytes, so the trip list opens at once after the first time.
// The thinned track is cached on its own, so the list never loads tracks.

import { Directory, File, Paths } from 'expo-file-system';

import { readLogText } from './logtext';
import { inboxFile, loadInbox } from './store';
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

type Cached<T> = { size: number; v: 1; summary: T };
export type GpxStats = Omit<GpxSummary, 'track'>;

// Bump when a summary's shape or its maths change, so old caches are redone.
const VERSION = 1;

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
    const { track, ...stats } = parseGpx(await readLogText(name));
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
