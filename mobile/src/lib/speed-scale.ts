// The drive map's speed colours, fitted to the drive: five stops from 0 to
// just over its top speed, in equal steps of a whole 5 km/h. A fixed
// 0/50/90/130/170+ left a town drive all blue-green under a legend of
// speeds never reached (2026-10-15). Pure, so it is tested with node.

/** The five km/h stops, 0 first; the last at or just above `topKmh`. */
export function speedStops(topKmh: number): number[] {
  const step = Math.max(5, Math.ceil(Math.max(0, topKmh) / 4 / 5) * 5);
  return [0, 1, 2, 3, 4].map((i) => i * step);
}
