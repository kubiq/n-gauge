// Builds assets/places/quarters.tsv - the cadastral areas (katastrální
// území) of the cities where a drive's end is named by the quarter it is
// in, not by the nearest point on a list (places.ts). A city quarter is an
// area: at the far edge of a large one, the GeoNames point of a smaller
// one - even across a river - can be nearer than its own.
//
//   node scripts/make-quarters.ts
//
// From the Czech Statistical Office's open data (RSO), which republishes
// the state register RÚIAN (ČÚZK, CC BY 4.0) - credited in About. Fetched
// by city name: no coordinates are sent. Outlines are simplified to about
// 10 m (Douglas-Peucker) - plenty for "which quarter", and small.
//
// One line per area: name, its box "w,s,e,n", its rings "lon lat,lon lat,..."
// separated by "|" (the first ring of each polygon its outline, then holes;
// polygons separated by "||").

import { writeFileSync } from 'node:fs';

const CITIES = ['Praha', 'Brno', 'Ostrava', 'Plzeň'];
const LAYER = 'https://geodata.csu.gov.cz/server/rest/services/Hosted/Open_data_RSO/FeatureServer/18/query';
const TOLERANCE_DEG = 0.0001;   // ~11 m north-south, ~7 m east-west here

type Pt = [number, number];

function simplify(pts: Pt[], tol: number): Pt[] {
  if (pts.length < 4) return pts;
  const keep = new Array<boolean>(pts.length).fill(false);
  keep[0] = keep[pts.length - 1] = true;
  const stack: [number, number][] = [[0, pts.length - 1]];
  while (stack.length) {
    const [a, b] = stack.pop()!;
    const [ax, ay] = pts[a], [bx, by] = pts[b];
    const dx = bx - ax, dy = by - ay, len = Math.hypot(dx, dy);
    let worst = -1, at = -1;
    for (let i = a + 1; i < b; i++) {
      // A ring is closed - its ends are the same point, a segment of length
      // 0 - so there, the distance is to that point, not to a line.
      const d =
        len === 0
          ? Math.hypot(pts[i][0] - ax, pts[i][1] - ay)
          : Math.abs(dy * pts[i][0] - dx * pts[i][1] + bx * ay - by * ax) / len;
      if (d > worst) {
        worst = d;
        at = i;
      }
    }
    if (worst > tol) {
      keep[at] = true;
      stack.push([a, at], [at, b]);
    }
  }
  return pts.filter((_, i) => keep[i]);
}

const r5 = (v: number) => Math.round(v * 1e5) / 1e5;
const lines: string[] = [];
for (const city of CITIES) {
  const q = new URLSearchParams({ where: `naz_obec='${city}'`, outFields: 'naz_ku', outSR: '4326', f: 'geojson' });
  const res = await fetch(`${LAYER}?${q}`);
  if (!res.ok) throw new Error(`${city}: HTTP ${res.status}`);
  const fc = (await res.json()) as { features: { properties: { naz_ku: string }; geometry: { type: string; coordinates: unknown } }[] };
  for (const f of fc.features) {
    const g = f.geometry;
    const polys = (g.type === 'Polygon' ? [g.coordinates] : g.coordinates) as Pt[][][];
    let [w, s, e, n] = [180, 90, -180, -90];
    const rings = polys.map((poly) =>
      poly
        .map((ring) => {
          const r = simplify(ring, TOLERANCE_DEG);
          for (const [x, y] of r) {
            w = Math.min(w, x); s = Math.min(s, y); e = Math.max(e, x); n = Math.max(n, y);
          }
          return r.map(([x, y]) => `${r5(x)} ${r5(y)}`).join(',');
        })
        .join('|'),
    );
    lines.push([f.properties.naz_ku, [w, s, e, n].map(r5).join(','), rings.join('||')].join('\t'));
  }
  console.log(`${city}: ${fc.features.length} areas`);
}
writeFileSync(new URL('../assets/places/quarters.tsv', import.meta.url), lines.join('\n') + '\n');
console.log(`${lines.length} areas`);
