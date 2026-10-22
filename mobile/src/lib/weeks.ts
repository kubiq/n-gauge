// The drives list, grouped by week (Monday to Sunday, local time), each with
// its totals: "This week · 4 drives · 62 km · 1 h 35 min". Pure, so it is
// tested with node.

import { locale as appLocale, t } from '../i18n/index.ts';

export type Groupable = {
  /** Epoch ms the drive started, or null (an undated log). */
  start: number | null;
  distanceM: number | null;
  secs: number | null;
};

export type WeekGroup<T> = {
  /** Monday 00:00 local of the week, or null for the undated drives. */
  monday: number | null;
  items: T[];
  drives: number;
  distanceM: number;
  secs: number;
};

/** Monday 00:00 local of the week holding `ms`. */
export function mondayOf(ms: number): number {
  const d = new Date(ms);
  d.setHours(0, 0, 0, 0);
  d.setDate(d.getDate() - ((d.getDay() + 6) % 7));   // getDay: 0 = Sunday
  return d.getTime();
}

/** In the order given (newest first), one group per week; the undated last. */
export function groupByWeek<T>(items: T[], of: (t: T) => Groupable): WeekGroup<T>[] {
  const groups = new Map<number | null, WeekGroup<T>>();
  for (const t of items) {
    const g = of(t);
    const monday = g.start === null ? null : mondayOf(g.start);
    let w = groups.get(monday);
    if (!w) {
      w = { monday, items: [], drives: 0, distanceM: 0, secs: 0 };
      groups.set(monday, w);
    }
    w.items.push(t);
    w.drives++;
    w.distanceM += g.distanceM ?? 0;
    w.secs += g.secs ?? 0;
  }
  const out = [...groups.values()];
  return [...out.filter((w) => w.monday !== null), ...out.filter((w) => w.monday === null)];
}

/**
 * "This week", "Last week", else "29 Sep – 5 Oct" (with the year when not
 * this one). Dates in the app's language, as the rows' (format.ts when()).
 */
export function weekLabel(monday: number | null, now = Date.now(), locale: string = appLocale()): string {
  if (monday === null) return t('Undated');
  const thisWeek = mondayOf(now);
  if (monday === thisWeek) return t('This week');
  if (monday === mondayOf(thisWeek - 1)) return t('Last week');
  const sunday = new Date(monday);
  sunday.setDate(sunday.getDate() + 6);
  const year = new Date(monday).getFullYear() !== new Date(now).getFullYear();
  const day = (d: Date, withYear: boolean) =>
    d.toLocaleDateString(locale, { day: 'numeric', month: 'short', ...(withYear ? { year: 'numeric' } : {}) });
  return `${day(new Date(monday), false)} – ${day(sunday, year)}`;
}
