// How numbers read on the trip screens.

import { locale, t } from '../i18n/index.ts';

// (Distances and speeds: units.ts - km or miles, as the gauge is set.)

/** A file's size: "4.2 MB" under 10 MB, "17 MB" above. */
export function mb(bytes: number): string {
  return `${(bytes / 1048576).toFixed(bytes < 10 * 1048576 ? 1 : 0)} MB`;
}

export function duration(s: number): string {
  const h = Math.floor(s / 3600);
  const m = Math.round((s % 3600) / 60);
  return h ? `${h} h ${String(m).padStart(2, '0')} min` : `${m} min`;
}

/** Short: the time of day if it was today, else the date - "14:45", "11 Oct". */
export function shortWhen(ms: number, now = Date.now()): string {
  const d = new Date(ms);
  return d.toDateString() === new Date(now).toDateString()
    ? d.toLocaleTimeString(locale(), { hour: '2-digit', minute: '2-digit' })
    : d.toLocaleDateString(locale(), { day: 'numeric', month: 'short' });
}

/**
 * A drive whose logs did not end (trips.ts unfinished()). The newest is most
 * likely uploaded while it was still being driven, the rest still on the
 * gauge's card; an older one the gauge lost power or restarted during.
 */
export function unfinishedText(latest: boolean): string {
  return latest
    ? t('Not all of this drive is here yet - the rest comes with the next upload or sync.')
    : t('This drive is cut off - the gauge lost power or restarted before it ended.');
}

/**
 * Local date and time of day, e.g. "Sat 11 Oct, 14:45" - or "Today, 11:16",
 * "Yesterday, 17:43" for the last two days, by the phone's calendar.
 */
export function when(ms: number, now = Date.now()): string {
  const d = new Date(ms);
  // The app's language, not the phone's region: an English UI under Czech
  // dates read as a mistake.
  const time = d.toLocaleTimeString(locale(), { hour: '2-digit', minute: '2-digit' });
  const today = new Date(now);
  const yesterday = new Date(today.getFullYear(), today.getMonth(), today.getDate() - 1);
  if (d.toDateString() === today.toDateString()) return `${t('Today')}, ${time}`;
  if (d.toDateString() === yesterday.toDateString()) return `${t('Yesterday')}, ${time}`;
  const day = d.toLocaleDateString(locale(), { weekday: 'short', day: 'numeric', month: 'short' });
  return `${day}, ${time}`;
}
