// Which fuel went in at a fill-up (asked 2026-11-08) - its octane, 95, 98
// or 100 (Verva 100), and if known its ethanol, E5 or E10 - so the
// consumption can be compared by fuel. The fill-ups themselves are found in
// the logs (the tank level's rise, trips.ts); the fuel is the one thing the
// car does not say, picked on the fill-up's line in the list and kept on the
// phone (store.ts), by the fill-up's id.
//
// A drive runs on the fuel of the last fill-up before it, or on it: one
// picked there, the drives after it are on that fuel until the next fill-up.
// A fill-up with no fuel picked ends it - what is in the tank is not known
// then. The tank still holds some of the old fuel after a fill-up; that is
// not weighed - filled from low, the new fuel is most of it.

import type { Refuel } from './trips.ts';

export const OCTANES = [95, 98, 100] as const;
export const ETHANOLS = [5, 10] as const;
export type Octane = (typeof OCTANES)[number];
export type Ethanol = (typeof ETHANOLS)[number];
/** ethanol null: not known (not picked). */
export type Fuel = { octane: Octane; ethanol: Ethanol | null };
export type Fuels = Record<string, Fuel>;

/** "98 E5", or "98" with the ethanol not known. Also what fuels are told apart by. */
export function fuelLabel(f: Fuel): string {
  return f.ethanol ? `${f.octane} E${f.ethanol}` : String(f.octane);
}

/** A stored value, if it is one - the file may be from another version. */
export function asFuel(v: unknown): Fuel | null {
  if (!v || typeof v !== 'object') return null;
  const { octane, ethanol } = v as Record<string, unknown>;
  if (!(OCTANES as readonly unknown[]).includes(octane)) return null;
  return { octane: octane as Octane, ethanol: (ETHANOLS as readonly unknown[]).includes(ethanol) ? (ethanol as Ethanol) : null };
}

/**
 * A fill-up's id: when the engine stopped for it, which is in the logs and
 * so the same on every read. Without a clock, its levels - rounded, so a
 * re-read that comes out a fraction off still finds it.
 */
export function fillId(f: Pick<Refuel, 'at' | 'fromPct' | 'toPct'>): string {
  return f.at !== null ? String(f.at) : `${Math.round(f.fromPct)}-${Math.round(f.toPct)}`;
}

/**
 * The fuel each drive ran on, by its key. `drives` oldest first, each with
 * its fill-ups (during it, or before it while the gauge slept).
 */
export function fuelByDrive(
  drives: readonly { key: string; fills?: readonly Refuel[] }[],
  fuels: Fuels,
): Record<string, Fuel | null> {
  const out: Record<string, Fuel | null> = {};
  let now: Fuel | null = null;
  for (const d of drives) {
    for (const f of d.fills ?? []) now = fuels[fillId(f)] ?? null;
    out[d.key] = now;
  }
  return out;
}

export type FuelTotal = { fuel: Fuel; litres: number; km: number; drives: number };

/**
 * Litres and km per fuel (fuelLabel), over the drives that say: fuel and
 * distance read and, as litresPer100, at least a kilometre. Lowest octane
 * first, then the ethanol: not known, E5, E10.
 */
export function totalsByFuel(
  drives: readonly { fuel: Fuel | null; fuelL: number | null; fuelKm: number | null }[],
): FuelTotal[] {
  const by = new Map<string, FuelTotal>();
  for (const d of drives) {
    if (!d.fuel || d.fuelL == null || d.fuelKm == null || d.fuelKm < 1) continue;
    const k = fuelLabel(d.fuel);
    const t = by.get(k) ?? { fuel: d.fuel, litres: 0, km: 0, drives: 0 };
    t.litres += d.fuelL;
    t.km += d.fuelKm;
    t.drives++;
    by.set(k, t);
  }
  return [...by.values()].sort(
    (a, b) => a.fuel.octane - b.fuel.octane || (a.fuel.ethanol ?? 0) - (b.fuel.ethanol ?? 0),
  );
}
