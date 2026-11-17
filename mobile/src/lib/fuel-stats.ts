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
        parked: true,
      });
    }
    if (c?.fuelStartPct != null) newer = d;
  }
  return fills;
}

/**
 * The tank's usable litres, from the pump: each fill-up's litres over its
 * level's rise, summed (the bigger rises weigh more - a small one's level
 * steps say little). 2026-11-17, five receipts: 46-47.5 l, not the 50 of
 * the book - the level's 0 % is not empty. TANK_L without any.
 */
export function tankFromPump(fills: readonly Refuel[], pump: Record<string, number>): { litres: number; fills: number } {
  let litres = 0, rise = 0, n = 0;
  for (const f of fills) {
    const p = pump[fillId(f)];
    const r = (f.toPct - f.fromPct) / 100;
    if (p == null || r < 0.2) continue;
    litres += p;
    rise += r;
    n++;
  }
  return n ? { litres: litres / rise, fills: n } : { litres: TANK_L, fills: 0 };
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
 * A stretch between two fill-ups that cannot be summed, and why - said on
 * the screen (2026-11-17: six stretches possible, one shown, no telling
 * why): `nofuel` a drive in it without the car's figure (a track with no
 * car data, or none read), at `at`; `unplaced` a fill-up not placeable in
 * its drive's fuel (a summary from before that was kept).
 */
export type SkippedStretch = { from: Refuel; to: Refuel; why: 'nofuel' | 'unplaced'; at: number | null };

/**
 * Each stretch between two fill-ups - made while parked, or during a drive
 * (that drive's fuel split at the stop at the pump, Refuel.fuelBeforeL): the
 * car's figure over it, against what was burned - by the pump's litres at the
 * second fill-up if typed in (`pump`, by fillId), else by the level's fall.
 * `drives` oldest first, `fills` as fillsByDrive gives them.
 */
export function fillIntervals(
  drives: readonly FuelDrive[],
  fills: Record<string, Refuel[]>,
  pump: Record<string, number>,
  skipped: SkippedStretch[] = [],
  tankL = TANK_L,
): FillInterval[] {
  // Where each fill-up falls in the drives' fuel: drive `at`, `off` litres
  // into it - 0 for one made while parked (before the drive), the drive's
  // fuel up to the stop at the pump for one made during it (most of them:
  // the gauge stays awake through a few minutes at the pump - 2026-11-17,
  // seven fill-ups and not one stretch). Null: cannot be placed.
  type Mark = { fill: Refuel; at: number; off: number | null };
  const marks: Mark[] = [];
  drives.forEach((d, i) => {
    for (const f of fills[d.key] ?? []) marks.push({ fill: f, at: i, off: f.parked ? 0 : (f.fuelBeforeL ?? null) });
  });
  // A drive with no car data at all - a track alone - had the car silent: the
  // gauge opens the car's file at its first frame. Nothing burned (2026-11-01,
  // five minutes of GPS jitter in one spot - a wake by motion, parked). One
  // with car data but no fuel figure (older logs) is not known.
  const fuel = (i: number) => (drives[i].c ? (drives[i].c!.fuelL ?? null) : 0);
  // A drive's km for part of its fuel: in proportion - its km are not split
  // at the stop, its fuel is.
  const kmOf = (i: number, litres: number) => {
    const c = drives[i].c;
    return c?.fuelL ? ((c.fuelKm ?? 0) * litres) / c.fuelL : 0;
  };
  const out: FillInterval[] = [];
  for (let k = 0; k + 1 < marks.length; k++) {
    const a = marks[k], b = marks[k + 1];
    if (a.off === null || b.off === null) {
      skipped.push({ from: a.fill, to: b.fill, why: 'unplaced', at: null });
      continue;
    }
    // Every drive it touches with the car's figure, or no sum.
    let missing = -1;
    for (let i = a.at; i <= Math.min(b.at, drives.length - 1); i++) {
      if (fuel(i) === null && !(i === b.at && b.off === 0)) missing = i;
    }
    if (missing >= 0) {
      skipped.push({ from: a.fill, to: b.fill, why: 'nofuel', at: drives[missing].start });
      continue;
    }
    let carL: number, km: number;
    if (a.at === b.at) {
      carL = b.off - a.off;
      km = kmOf(a.at, carL);
    } else {
      carL = fuel(a.at)! - a.off + b.off;
      km = kmOf(a.at, fuel(a.at)! - a.off) + (b.off ? kmOf(b.at, b.off) : 0);
      for (let i = a.at + 1; i < b.at; i++) {
        carL += fuel(i)!;
        km += drives[i].c?.fuelKm ?? 0;
      }
    }
    if (carL <= 0) continue;
    // The drives it touches: from a's to b's, b's only if part of it counts.
    const touched = b.at - a.at + (b.off ? 1 : 0);
    const p = pump[fillId(b.fill)];
    const tank = (pct: number) => (pct / 100) * tankL;
    const burnedL =
      p != null
        ? p + tank(a.fill.toPct) - tank(b.fill.toPct)
        : tank(a.fill.toPct) - tank(b.fill.fromPct);
    out.push({ from: a.fill, to: b.fill, carL, km, burnedL: burnedL > 0 ? burnedL : null, by: p != null ? 'pump' : 'tank', drives: touched });
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
  // The median of the stretches' ratios, not the sum's: one stretch with a
  // drive's data missing, or a partial fill's level read off, pulled the sum
  // far (2026-11-17: x1.00, x1.02, x1.02 - and x1.17, x1.70 - read "9 % low").
  const r = use.map((i) => i.burnedL! / i.carL).sort((a, b) => a - b);
  const mid = r.length >> 1;
  const factor = r.length % 2 ? r[mid] : (r[mid - 1] + r[mid]) / 2;
  return { factor, stretches: use.length, by: pumped.length ? 'pump' : 'tank' };
}
