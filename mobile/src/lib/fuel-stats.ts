// The fuel figures (asked 2026-11-17: a screen of their own, with charts, and
// the car's figure against what went in at the pump). Pure, so it is tested
// with node: drives in, numbers out.
//
// The car's figure: the fuel rate it reports (PID 0x5E), added up over each
// drive (trips.ts parseCsv: fuelL, and fuelKm by its own speed). What went
// in: the tank level's rise at a fill-up (refuels) - coarse, the level is
// read in steps and sloshes, so it is a median of minutes - or better, the
// litres from the pump, typed in at the fill-up. Between two fill-ups, what
// was burned is the pump's litres at the second, plus the level the first
// left over what the second left; without them, the level's fall between.
// Over enough fill-ups that gives the car's figure a factor.

import { FILL_MIN_PCT, type CsvSummary, type Refuel } from './trips.ts';
import { fillId } from './fuel-grade.ts';

/** The i30 N's tank, for the litres a fill-up's levels come to. */
export const TANK_L = 50;

/** A drive as the fuel figures need it. */
export type FuelDrive = {
  key: string;
  /** Epoch ms of its start and end (the track's, or the file name's stamp). */
  start: number | null;
  end: number | null;
  /** Where it ended, for a fill-up after it. */
  to?: [number, number] | null;
  toPlace?: string | null;
  c?: CsvSummary;
};

/**
 * The fill-ups, on the drive they were on (asked 2026-10-22): during it (its
 * CSV's refuels), or before it - the tank fuller at its start than at the
 * end of the drive before, filled while the gauge slept. `drives` newest
 * first, as the list has them; each fill-up in time order.
 */
export function fillsByDrive(drives: readonly FuelDrive[]): Record<string, Refuel[]> {
  const fills: Record<string, Refuel[]> = {};
  let newer: FuelDrive | null = null;   // the drive after, with a start level
  for (const d of drives) {
    const c = d.c;
    if (c?.refuels.length) fills[d.key] = [...c.refuels];
    if (newer && c?.fuelEndPct != null && newer.c!.fuelStartPct! - c.fuelEndPct >= FILL_MIN_PCT) {
      const end = d.end ?? (d.start !== null && c.durationS ? d.start + c.durationS * 1000 : d.start);
      (fills[newer.key] ??= []).unshift({
        at: end,
        pt: d.to ?? null,
        place: d.toPlace ?? null,
        fromPct: c.fuelEndPct,
        toPct: newer.c!.fuelStartPct!,
      });
    }
    if (c?.fuelStartPct != null) newer = d;
  }
  return fills;
}

/** One drive's consumption: the car's figure. */
export type DrivePoint = { key: string; at: number; litres: number; km: number; l100: number };

/** Drives with a figure - fuel and at least a kilometre (as litresPer100) - oldest first. */
export function drivePoints(drives: readonly FuelDrive[]): DrivePoint[] {
  const out: DrivePoint[] = [];
  for (const d of drives) {
    const c = d.c;
    if (!c || c.fuelL == null || c.fuelKm == null || c.fuelKm < 1 || d.start === null) continue;
    out.push({ key: d.key, at: d.start, litres: c.fuelL, km: c.fuelKm, l100: (c.fuelL / c.fuelKm) * 100 });
  }
  return out.sort((a, b) => a.at - b.at);
}

/**
 * A rolling average under each point: the litres over the km of it and the
 * `n - 1` before - by distance, so a short cold hop counts for what it is.
 */
export function rolling(points: readonly DrivePoint[], n = 8): number[] {
  return points.map((_, i) => {
    const w = points.slice(Math.max(0, i - n + 1), i + 1);
    const km = w.reduce((s, p) => s + p.km, 0);
    return km > 0 ? (w.reduce((s, p) => s + p.litres, 0) / km) * 100 : 0;
  });
}

export type WeekFuel = { monday: number; litres: number; km: number };

/** Litres and km per week (Monday 00:00, phone time), oldest first. */
export function byWeek(points: readonly DrivePoint[]): WeekFuel[] {
  const by = new Map<number, WeekFuel>();
  for (const p of points) {
    const d = new Date(p.at);
    const monday = new Date(d.getFullYear(), d.getMonth(), d.getDate() - ((d.getDay() + 6) % 7)).getTime();
    const w = by.get(monday) ?? { monday, litres: 0, km: 0 };
    w.litres += p.litres;
    w.km += p.km;
    by.set(monday, w);
  }
  return [...by.values()].sort((a, b) => a.monday - b.monday);
}

/** Between two fill-ups: the car's figure against what was burned. */
export type FillInterval = {
  /** The fill-ups at its ends. */
  from: Refuel;
  to: Refuel;
  /** The car's figure: its drives' fuel added up, and their km. */
  carL: number;
  km: number;
  /** What was burned, litres; null when it cannot be told. */
  burnedL: number | null;
  /** By the pump's litres (exact), or by the tank level (coarse). */
  by: 'pump' | 'tank';
  drives: number;
};

/**
 * Each stretch between two fill-ups made while parked (a fill-up during a
 * drive splits its fuel at a moment the CSV's sum cannot): the car's figure
 * over its drives, against what was burned - by the pump's litres at the
 * second fill-up if typed in (`pump`, by fillId), else by the level's fall.
 * `drives` oldest first, `fills` as fillsByDrive gives them.
 */
export function fillIntervals(
  drives: readonly FuelDrive[],
  fills: Record<string, Refuel[]>,
  pump: Record<string, number>,
): FillInterval[] {
  // Each parked fill-up at the boundary before its drive; during one: a break.
  type Mark = { fill: Refuel; at: number; parked: boolean };
  const marks: Mark[] = [];
  drives.forEach((d, i) => {
    for (const f of fills[d.key] ?? []) {
      const parked = f.fromPct === (drives[i - 1]?.c?.fuelEndPct ?? NaN) && f.toPct === d.c?.fuelStartPct;
      marks.push({ fill: f, at: i, parked });
    }
  });
  const out: FillInterval[] = [];
  for (let k = 0; k + 1 < marks.length; k++) {
    const a = marks[k], b = marks[k + 1];
    if (!a.parked || !b.parked || b.at <= a.at) continue;
    const span = drives.slice(a.at, b.at);
    if (span.some((d) => d.c?.fuelL == null)) continue;   // a drive without the figure: no sum
    const carL = span.reduce((s, d) => s + (d.c!.fuelL ?? 0), 0);
    const km = span.reduce((s, d) => s + (d.c!.fuelKm ?? 0), 0);
    const p = pump[fillId(b.fill)];
    const tank = (pct: number) => (pct / 100) * TANK_L;
    const burnedL =
      p != null
        ? p + tank(a.fill.toPct) - tank(b.fill.toPct)
        : tank(a.fill.toPct) - tank(b.fill.fromPct);
    out.push({ from: a.fill, to: b.fill, carL, km, burnedL: burnedL > 0 ? burnedL : null, by: p != null ? 'pump' : 'tank', drives: span.length });
  }
  return out;
}

/**
 * What the car's figure is off by: burned over the car's, over the stretches
 * that say (some litres at least, so one short stretch's rounding does not
 * decide it). Null with none. The pump's stretches only, once there are any:
 * the tank level is the coarse one.
 */
export function carFactor(intervals: readonly FillInterval[], minL = 5): { factor: number; stretches: number; by: 'pump' | 'tank' } | null {
  const ok = intervals.filter((i) => i.burnedL !== null && i.carL >= minL);
  const pumped = ok.filter((i) => i.by === 'pump');
  const use = pumped.length ? pumped : ok;
  if (!use.length) return null;
  const burned = use.reduce((s, i) => s + i.burnedL!, 0);
  const car = use.reduce((s, i) => s + i.carL, 0);
  return { factor: burned / car, stretches: use.length, by: pumped.length ? 'pump' : 'tank' };
}
