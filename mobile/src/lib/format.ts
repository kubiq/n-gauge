// How numbers read on the trip screens.

import { locale, t } from '../i18n/index.ts';

export function km(m: number): string {
  return m >= 10000 ? `${Math.round(m / 1000)} km` : `${(m / 1000).toFixed(1)} km`;
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
