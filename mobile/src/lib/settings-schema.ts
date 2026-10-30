// The pod's settings table (firmware/car_gauge_v1/settings.cpp, settings_table)
// as the app needs it. ⚠ A copy: change both together. /api/settings reports
// each value under `json`, and /save takes it under `key` - until the pod has
// a JSON /save twin, this table is the mapping between the two.
//
// lo/hi are the firmware's clamps, so the app can say "out of range" before
// the pod silently clamps it.

import { N_, t } from '../i18n/index.ts';

export type SettingType = 'u8' | 'u16' | 'i16' | 'f32' | 'bool';

export type SettingDef = {
  key: string;
  json: string;
  type: SettingType;
  lo: number;
  hi: number;
  label: string;
  unit?: string;
  hint?: string;
  /** A fixed set of values: shown as a list to pick from, not a number field. */
  options?: { value: number; label: string }[];
  group: 'Display' | 'Power' | 'Warnings' | 'Tyres' | 'Logging';
};

/** The groups' card titles, shown through t(). */
export const GROUP_TITLES: Record<SettingDef['group'], string> = {
  Display: N_('Display'),
  Power: N_('Power'),
  Warnings: N_('Warnings'),
  Tyres: N_('Tyres'),
  Logging: N_('Logging'),
};

// label, hint and option labels in English, marked N_: shown through t().
export const SETTINGS: SettingDef[] = [
  { key: 'bright', json: 'brightness', type: 'u8', lo: 10, hi: 255, label: N_('Brightness'), group: 'Display' },
  { key: 'nightOn', json: 'nightOn', type: 'bool', lo: 0, hi: 1, label: N_('Dim at night'), group: 'Display' },
  { key: 'nightBr', json: 'nightBright', type: 'u8', lo: 10, hi: 255, label: N_('Night brightness'), group: 'Display' },
  {
    key: 'womMg', json: 'motionWakeMg', type: 'u8', lo: 0, hi: 255, label: N_('Wake on motion'), unit: 'mg',
    hint: N_('0 = off. Below 30 is taken as 30.'), group: 'Power',
  },
  { key: 'sleepMin', json: 'sleepAfterMin', type: 'u8', lo: 1, hi: 60, label: N_('Sleep after'), unit: 'min', group: 'Power' },
  { key: 'oilRdy', json: 'oilReadyC', type: 'i16', lo: 40, hi: 150, label: N_('Oil ready'), unit: '°C', group: 'Warnings' },
  { key: 'oilWarn', json: 'oilWarnC', type: 'i16', lo: 90, hi: 200, label: N_('Oil warning'), unit: '°C', group: 'Warnings' },
  { key: 'cool', json: 'coolantWarnC', type: 'i16', lo: 90, hi: 200, label: N_('Coolant warning'), unit: '°C', group: 'Warnings' },
  { key: 'tLowF', json: 'tpmsLowBarFront', type: 'f32', lo: 0.5, hi: 5, label: N_('Front low'), unit: 'bar', group: 'Tyres' },
  { key: 'tHighF', json: 'tpmsHighBarFront', type: 'f32', lo: 0.5, hi: 5, label: N_('Front high'), unit: 'bar', group: 'Tyres' },
  { key: 'tLowR', json: 'tpmsLowBarRear', type: 'f32', lo: 0.5, hi: 5, label: N_('Rear low'), unit: 'bar', group: 'Tyres' },
  { key: 'tHighR', json: 'tpmsHighBarRear', type: 'f32', lo: 0.5, hi: 5, label: N_('Rear high'), unit: 'bar', group: 'Tyres' },
  // Labels and hint as on the pod's own Car page (webapi.cpp); LogLevel in logger.h.
  {
    key: 'logLvl', json: 'logLevel', type: 'u8', lo: 0, hi: 2, label: N_('What goes on the SD card'), group: 'Logging',
    options: [
      { value: 0, label: N_('Off') },
      { value: 1, label: N_('Trips - GPS tracks only') },
      { value: 2, label: N_('Normal - tracks + drive data') },
    ],
    hint: N_(
      'Applies at once and starts new files. Track sessions (.vbo) are recorded at every level but Off; the sleep log always is.',
    ),
  },
  {
    key: 'tripGap', json: 'tripGapMin', type: 'u16', lo: 1, hi: 720, label: N_('New trip after a stop of'), unit: 'min',
    group: 'Logging',
  },
];

/** /api/settings as returned: the table by json name, plus a few extras. */
export type PodSettings = Record<string, number | boolean> & {
  night: boolean;
  apSsid: string;
  tzOffset: number;
  tzDst: boolean;
  tz: string;
  /** Fingerprint of the key the pod encrypts its logs to; "" = plain logs. Older firmware: absent. */
  logKey?: string;
  /** The screen cycle, view ids as digits (screens.ts). Older firmware: absent. */
  views?: string;
};

/**
 * Form body for POST /save with only the changed settings. A bool is sent the
 * way the HTML form sends a checkbox: `<key>Present` says it was on the form,
 * and the key itself only when on - absent then means off, not "unchanged".
 */
export function saveForm(changes: Record<string, number | boolean>): string {
  const parts: string[] = [];
  for (const d of SETTINGS) {
    if (!(d.json in changes)) continue;
    const v = changes[d.json];
    if (d.type === 'bool') {
      parts.push(`${d.key}Present=1`);
      if (v) parts.push(`${d.key}=on`);
    } else {
      parts.push(`${d.key}=${encodeURIComponent(String(Number(v)))}`);
    }
  }
  return parts.join('&');
}

/** Why a typed value would be rejected or clamped by the pod, or null if fine. */
export function checkValue(d: SettingDef, v: number): string | null {
  if (!Number.isFinite(v)) return t('not a number');
  if (d.type !== 'f32' && !Number.isInteger(v)) return t('whole numbers only');
  if (v < d.lo || v > d.hi) return `${d.lo}–${d.hi}`;
  return null;
}
