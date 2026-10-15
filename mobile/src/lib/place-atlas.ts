// The place lists drives are named from (places.ts). They ship with the app
// as files (assets/places, built by scripts/make-places.ts and
// make-quarters.ts; metro.config.js): the Czech Republic and its neighbours,
// and the quarters of the Czech cities by their boundaries. A list is read
// only to name a new drive's ends while it is summarised (trip-cache.ts),
// and let go after - nothing stays in memory, nothing goes over the network.

import { Asset } from 'expo-asset';
import { File } from 'expo-file-system';

import { ASSETS, INDEX } from '../../assets/places';
import { QUARTERS } from '../../assets/places/quarters';
import { type Atlas, atlasPlaceAt, countriesAt, parseAtlas, parseQuarters, type Quarter, quarterAt } from './places';

async function readAsset(id: number): Promise<string | null> {
  const [a] = await Asset.loadAsync(id);
  return a.localUri ? new File(a.localUri).text() : null;
}

/**
 * The names for these points ([lon, lat]), in order; null where there is
 * none: the city quarter a point lies in, else the nearest place on the lists.
 */
export async function atlasNames(points: ([number, number] | null)[]): Promise<(string | null)[]> {
  const need = new Set(points.flatMap((p) => (p ? countriesAt(INDEX, p[1], p[0]) : [])));
  const atlases: Atlas[] = [];
  for (const cc of need) {
    const text = await readAsset(ASSETS[cc]);
    if (text) atlases.push(parseAtlas(text));
  }
  // The quarters are Czech cities': read only for a point in the country.
  let quarters: Quarter[] = [];
  if (need.has('CZ')) {
    const text = await readAsset(QUARTERS);
    if (text) quarters = parseQuarters(text);
  }
  return points.map((p) => (p ? (quarterAt(p[1], p[0], quarters) ?? atlasPlaceAt(p[1], p[0], atlases)) : null));
}
