// The place lists drives are named from (places.ts). They ship with the app
// as files (assets/places, built by scripts/make-places.ts; metro.config.js):
// the Czech Republic and its neighbours. A list is read only to name a new
// drive's ends while it is summarised (trip-cache.ts), and let go after -
// nothing stays in memory, and nothing goes over the network.

import { Asset } from 'expo-asset';
import { File } from 'expo-file-system';

import { ASSETS, INDEX } from '../../assets/places';
import { type Atlas, atlasPlaceAt, countriesAt, parseAtlas } from './places';

/** The lists' names for these points ([lon, lat]), in order; null where there is none. */
export async function atlasNames(points: ([number, number] | null)[]): Promise<(string | null)[]> {
  const need = new Set(points.flatMap((p) => (p ? countriesAt(INDEX, p[1], p[0]) : [])));
  const atlases: Atlas[] = [];
  for (const cc of need) {
    const [a] = await Asset.loadAsync(ASSETS[cc]);
    if (a.localUri) atlases.push(parseAtlas(await new File(a.localUri).text()));
  }
  return points.map((p) => (p ? atlasPlaceAt(p[1], p[0], atlases) : null));
}
