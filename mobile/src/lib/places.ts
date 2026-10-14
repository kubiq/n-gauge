// Drives named after where they start and end: "Kolín → Příbram".
//
// Offline, from GeoNames place lists shipped with the app (assets/places,
// place-atlas.ts) - the coordinates never leave the phone, and naming works
// on the gauge's WiFi, which has no internet. A list is read only when a new
// drive is summarised; the names it gives are cached with the summary
// (trip-cache.ts), so showing the drives never loads one. Your own places
// are a handful, checked on every render: a drive starting within OWN_M of
// "Home" says Home, and renaming Home renames them all at once.
//
// Pure, so the tests run it on real tracks with node.

import { distM } from './trips.ts';

/**
 * One country's places: [name, lat, lon, known], sorted by latitude. Known
 * (1) = 100 people or more; most villages have no population on GeoNames,
 * so the rest are kept too (scripts/make-places.ts).
 */
export type Atlas = [string, number, number, number][];
/** Which country lists exist, and the box each covers: [w, s, e, n]. */
export type AtlasIndex = { cc: string; bbox: [number, number, number, number] }[];
/** A place you named yourself. */
export type OwnPlace = { name: string; lat: number; lon: number };

/** Your own place wins within this - about a car park and the street to it. */
export const OWN_M = 300;
/** Further than this from every place on the list: no name (off the map). */
export const ATLAS_MAX_M = 10_000;
/**
 * A known place this close wins over a nearer unknown one: in a city the
 * quarter people say, not a hamlet by the road with an entry of its own;
 * out in the country, the village itself, populated on GeoNames or not.
 */
export const KNOWN_M = 3_000;
// ATLAS_MAX_M as latitude, for the band search: 1° is ~111 km.
const BAND_DEG = ATLAS_MAX_M / 111_000;

/** A list as shipped: one place per line - name, lat, lon, known. */
export function parseAtlas(text: string): Atlas {
  const out: Atlas = [];
  for (const line of text.split('\n')) {
    const f = line.split('\t');
    if (f.length === 4) out.push([f[0], Number(f[1]), Number(f[2]), Number(f[3])]);
  }
  return out;
}

/** The countries whose box holds the point: their lists are needed to name it. */
export function countriesAt(index: AtlasIndex, lat: number, lon: number): string[] {
  return index.filter(({ bbox: [w, s, e, n] }) => lon >= w && lon <= e && lat >= s && lat <= n).map((c) => c.cc);
}

type Hit = { name: string; d: number } | null;
const closer = (a: Hit, b: Hit) => (b && (!a || b.d < a.d) ? b : a);

/** The nearest place of any kind, and the nearest known one, within ATLAS_MAX_M. */
function nearestIn(atlas: Atlas, lat: number, lon: number): { any: Hit; known: Hit } {
  // The first entry inside the latitude band, by binary search; then walk it.
  let lo = 0, hi = atlas.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (atlas[mid][1] < lat - BAND_DEG) lo = mid + 1;
    else hi = mid;
  }
  let any: Hit = null, known: Hit = null;
  for (let i = lo; i < atlas.length && atlas[i][1] <= lat + BAND_DEG; i++) {
    const d = distM(lat, lon, atlas[i][1], atlas[i][2]);
    if (d > ATLAS_MAX_M) continue;
    any = closer(any, { name: atlas[i][0], d });
    if (atlas[i][3]) known = closer(known, { name: atlas[i][0], d });
  }
  return { any, known };
}

/** The lists' name for a point: a known place within KNOWN_M, else the nearest. */
export function atlasPlaceAt(lat: number, lon: number, atlases: Atlas[]): string | null {
  let any: Hit = null, known: Hit = null;
  for (const a of atlases) {
    const n = nearestIn(a, lat, lon);
    any = closer(any, n.any);
    known = closer(known, n.known);
  }
  return (known && known.d <= KNOWN_M ? known : any)?.name ?? null;
}

/** Your place nearest the point, within OWN_M. */
export function ownPlaceAt(lat: number, lon: number, own: OwnPlace[]): OwnPlace | null {
  let best: OwnPlace | null = null, bd = OWN_M;
  for (const p of own) {
    const d = distM(lat, lon, p.lat, p.lon);
    if (d <= bd) {
      best = p;
      bd = d;
    }
  }
  return best;
}

/** The name for a point: yours within OWN_M, else the lists'. */
export function placeAt(lat: number, lon: number, own: OwnPlace[], atlases: Atlas[]): string | null {
  return ownPlaceAt(lat, lon, own)?.name ?? atlasPlaceAt(lat, lon, atlases);
}

/**
 * "Kolín → Příbram"; "Around Kolín" when both ends are the same place (a
 * loop, or a few hundred metres); null when neither end has a name.
 */
export function routeName(a: string | null, b: string | null): string | null {
  if (!a && !b) return null;
  if (a === b) return `Around ${a}`;
  return `${a ?? '…'} → ${b ?? '…'}`;
}
