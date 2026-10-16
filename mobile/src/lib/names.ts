// What you named yourself: places ("Home", "Work") that drives starting or
// ending near them are named after, and drives renamed outright. Kept on the
// phone only, in one small file. Showing a name needs no place list: the
// lists' names are cached with each drive (trip-cache.ts), and yours - a
// handful - are checked over them.

import { File, Paths } from 'expo-file-system';

import { type OwnPlace, ownPlaceAt, routeName } from './places';
import type { GpxStats } from './trip-cache';
import { staleTitleKeys, type TripFiles } from './trips';

export type Names = {
  places: OwnPlace[];
  /** Trip key -> the name you gave it. */
  titles: Record<string, string>;
};

const file = new File(Paths.document, 'names.json');

export function loadNames(): Names {
  try {
    if (file.exists) return { places: [], titles: {}, ...JSON.parse(file.textSync()) };
  } catch {
    // A broken file: start again rather than fail every screen.
  }
  return { places: [], titles: {} };
}

function save(n: Names): Names {
  file.write(JSON.stringify(n));
  return n;
}

/**
 * Forgets the names of drives the phone no longer has (staleTitleKeys) -
 * deleted ones, a test's leftover. Written only when something goes, and
 * never with no drives listed: an empty list is more likely a folder not
 * read than every drive deleted.
 */
export function forgetStaleTitles(trips: TripFiles[]): Names {
  const n = loadNames();
  if (!trips.length) return n;
  const stale = staleTitleKeys(n.titles, trips);
  if (!stale.length) return n;
  for (const k of stale) delete n.titles[k];
  return save(n);
}

/**
 * The drive's own name, or null to go back to the automatic one. Kept under
 * its key; a name it was given under its CSV, before its GPX came, goes - or
 * that one would still show after going back to automatic.
 */
export function setTripTitle(t: Pick<TripFiles, 'key' | 'csv'>, title: string | null): Names {
  const n = loadNames();
  if (t.csv && t.csv !== t.key) delete n.titles[t.csv];
  if (title) n.titles[t.key] = title;
  else delete n.titles[t.key];
  return save(n);
}

/** Has the drive a name you gave it (under its key, or its CSV's)? */
export function hasTripTitle(t: Pick<TripFiles, 'key' | 'csv'>, n: Names): boolean {
  return !!(n.titles[t.key] || (t.csv && n.titles[t.csv]));
}

/**
 * Names the place at this point: renames your place already there, else
 * adds one. An empty name forgets it.
 */
export function nameOwnPlace(lat: number, lon: number, name: string): Names {
  const n = loadNames();
  const near = ownPlaceAt(lat, lon, n.places);
  n.places = n.places.filter((p) => p !== near);
  if (name) n.places.push({ name, lat: near?.lat ?? lat, lon: near?.lon ?? lon });
  return save(n);
}

// A place can have several spots under one name - "Home" at both ends of
// drives whose first GPS fix came a few hundred metres apart (named twice,
// 2026-10-15). Settings shows them as one, and renames or forgets them
// together; a drive's ⋮ still names its one spot.

/** Every spot called `from` is called `to` now. */
export function renamePlaces(from: string, to: string): Names {
  const n = loadNames();
  n.places = n.places.map((p) => (p.name === from ? { ...p, name: to } : p));
  return save(n);
}

/** Every spot called `name` goes. */
export function forgetPlaces(name: string): Names {
  const n = loadNames();
  n.places = n.places.filter((p) => p.name !== name);
  return save(n);
}

/** What one end of a drive ([lon, lat]) is called: your place there, else the lists' name. */
export function endName(pt: [number, number] | null, listName: string | null, n: Names): string | null {
  return (pt && ownPlaceAt(pt[1], pt[0], n.places)?.name) || listName;
}

/** What a drive is called: your name for it, else where it went, else null (the date then). */
export function tripName(t: TripFiles, g: GpxStats | undefined | null, n: Names): string | null {
  if (n.titles[t.key]) return n.titles[t.key];
  // Named while only its CSV was here, the key then: the GPX came after.
  if (t.csv && n.titles[t.csv]) return n.titles[t.csv];
  if (!g) return null;
  return routeName(endName(g.from, g.fromPlace, n), endName(g.to, g.toPlace, n));
}
