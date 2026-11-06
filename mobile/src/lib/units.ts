// Speed and distance in km/h and km, or mph and miles (asked 2026-11-06).
// The gauge's setting ("mph", Gauge settings > Display) - set once, there:
// the app keeps a copy each time it reads or saves the gauge's settings, so
// the drives read the same as the gauge shows when it is not connected.
// Speed and distance only: temperatures, pressures and consumption stay
// metric, and so do the logs.
//
// Every speed and distance the app shows goes through here, so nothing is
// converted twice or missed. A change redraws the screens, as a language
// change does (use-language.tsx keys them on unitsKey()).

import { File, Paths } from 'expo-file-system';

const KM_PER_MILE = 1.609344;
const file = new File(Paths.document, 'units-mph.txt');

let mph = (() => {
  try {
    return file.exists && file.textSync().trim() === '1';
  } catch {
    return false;
  }
})();
const listeners = new Set<() => void>();

export function isMph(): boolean {
  return mph;
}

/** The gauge's setting, as read from it or just saved to it. */
export function setMph(on: boolean): void {
  if (on === mph) return;
  mph = on;
  try {
    file.write(on ? '1' : '0');
  } catch {
    // shown in this run all the same; read again from the gauge next time
  }
  for (const l of listeners) l();
}

/** For a useSyncExternalStore: told of every change. */
export function onUnitsChange(l: () => void): () => void {
  listeners.add(l);
  return () => listeners.delete(l);
}

/** A km/h figure in the shown unit, as a number - for charts and scales. */
export function speedIn(kmh: number): number {
  return mph ? kmh / KM_PER_MILE : kmh;
}

export function speedUnit(): string {
  return mph ? 'mph' : 'km/h';
}

/** "87 km/h" or "54 mph". */
export function speed(kmh: number): string {
  return `${Math.round(speedIn(kmh))} ${speedUnit()}`;
}

/** "12.3 km" under 10, "123 km" above - or the same in miles. */
export function distance(m: number): string {
  const d = m / 1000 / (mph ? KM_PER_MILE : 1);
  const unit = mph ? 'mi' : 'km';
  return d >= 10 ? `${Math.round(d)} ${unit}` : `${d.toFixed(1)} ${unit}`;
}
