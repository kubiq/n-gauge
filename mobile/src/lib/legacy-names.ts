// One-off: session ids for the logs written before the pod put them in the
// names (2026-10-13). Pairs those files the way the app used to - by the time
// in their names, closest first, within 20 minutes - and gives each pair, and
// each file left over, an id of its own.
//
// Pure and deterministic, so the phone (on its inbox, at start-up) and
// scripts/rename-legacy.ts (on the pod's card in a reader, and on folders of
// saved copies) reach the same names, and a sync afterwards finds nothing new.
// Delete once no un-renamed logs are left anywhere.

// With the extension: scripts/rename-legacy.ts runs this file in Node too.
import { plainName, sessionOf } from './trips.ts';

const LOG = /^(?:trip|drive|track)_.+\.(?:gpx|csv|vbo)(?:\.nge)?$/;
const PAIR_WINDOW_MS = 20 * 60 * 1000;

function stamp(name: string): number | null {
  const m = /_(\d{4})-(\d{2})-(\d{2})_(\d{2})-(\d{2})(?:_\d+)?\.(?:gpx|csv)(?:\.nge)?$/.exec(name);
  return m ? new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5]).getTime() : null;
}

// FNV-1a, 24 bits: six hex characters, like the pod's random ids. Of the
// PLAIN name, so an encrypted log and its decrypted copy get the same id.
function hash6(s: string): string {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 0x01000193) >>> 0;
  return (h & 0xffffff).toString(16).padStart(6, '0');
}

const withSession = (name: string, id: string) => name.replace(/\.(gpx|csv|vbo)(\.nge)?$/, `_${id}.$1$2`);

/** Old name -> new name, for every log in `names` that has no session yet. */
export function legacyRenames(names: string[]): Record<string, string> {
  const old = names.filter((n) => LOG.test(n) && sessionOf(n) === null).sort();
  const gpx = old.filter((n) => n.startsWith('trip_'));
  const csv = old.filter((n) => n.startsWith('drive_'));

  const pairs: { g: string; c: string; d: number }[] = [];
  for (const g of gpx) {
    for (const c of csv) {
      const tg = stamp(g);
      const tc = stamp(c);
      if (tg === null || tc === null) continue;
      const d = Math.abs(tc - tg);
      if (d <= PAIR_WINDOW_MS) pairs.push({ g, c, d });
    }
  }
  pairs.sort((a, b) => a.d - b.d || a.g.localeCompare(b.g) || a.c.localeCompare(b.c));
  const anchor = new Map<string, string>(old.map((n) => [n, n]));   // file -> the file its id comes from
  const paired = new Set<string>();
  for (const { g, c } of pairs) {
    if (paired.has(g) || paired.has(c)) continue;
    paired.add(g).add(c);
    anchor.set(c, g);
  }

  const idOf = new Map<string, string>();
  const used = new Set(names.map(sessionOf).filter((s) => s !== null));
  for (const a of [...new Set(anchor.values())].sort()) {
    let id = hash6(plainName(a));
    for (let k = 1; used.has(id); k++) id = hash6(`${plainName(a)}#${k}`);   // never two sessions on one id
    used.add(id);
    idOf.set(a, id);
  }
  return Object.fromEntries(old.map((n) => [n, withSession(n, idOf.get(anchor.get(n)!)!)]));
}
