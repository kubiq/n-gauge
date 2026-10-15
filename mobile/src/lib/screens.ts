// The gauge's screens and the order a tap or swipe cycles through them -
// settings.views on the gauge (settings.h): view ids as digits, in order, a
// screen left out hidden. The ids are its GaugeView enum (ui.h). WiFi is
// always in it: the way to the phone.

import { N_ } from '../i18n/index.ts';

export const WIFI = '6';

/** Every screen, in the gauge's default order (config.h VIEW_ORDER_DEFAULT). Names shown through t(). */
export const SCREENS: { id: string; name: string }[] = [
  { id: '0', name: N_('Oil & coolant') },
  { id: '2', name: N_('Boost') },
  { id: '4', name: N_('Tyres') },
  { id: '5', name: N_('GPS') },
  { id: '7', name: N_('Track') },
  { id: '3', name: N_('G-meter') },
  { id: WIFI, name: N_('WiFi') },
];

export type ScreenItem = { id: string; name: string; on: boolean };

/** The gauge's order as a list: the shown screens in order, then the hidden ones. */
export function screenItems(views: string): ScreenItem[] {
  const shown = [...views].filter((id) => SCREENS.some((s) => s.id === id));
  return [
    ...shown.map((id) => ({ ...SCREENS.find((s) => s.id === id)!, on: true })),
    ...SCREENS.filter((s) => !shown.includes(s.id)).map((s) => ({ ...s, on: s.id === WIFI })),
  ];
}

/** Back to the gauge's form: the shown screens' ids in order, WiFi always among them. */
export function viewsOf(items: ScreenItem[]): string {
  return items
    .filter((s) => s.on || s.id === WIFI)
    .map((s) => s.id)
    .join('');
}

/** The item at `i` moved one place up (-1) or down (+1); unchanged at either end. */
export function moved(items: ScreenItem[], i: number, delta: -1 | 1): ScreenItem[] {
  const j = i + delta;
  if (j < 0 || j >= items.length) return items;
  const out = [...items];
  [out[i], out[j]] = [out[j], out[i]];
  return out;
}
