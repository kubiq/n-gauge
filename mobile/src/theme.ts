// The app's colours, per theme. Dark by default - it is used in a car, and at
// night a white screen dazzles - with the dark themes differing only in
// their accent, plus a Light one and "follow the phone" (Settings).
//
// Every text colour clears 4.5:1 on its card (checked 2026-10-14). The
// chart series are the dataviz palette's steps for each surface: validated
// for colour-blind separation against the card they sit on.
//
// Colours come from useColors(), styles from themed(): a StyleSheet made
// once at import would keep the first theme's colours.

import { createContext, useContext, useMemo } from 'react';

export type Colors = {
  bg: string;
  card: string;
  border: string;
  text: string;
  dim: string;
  accent: string;
  link: string;
  bad: string;
  good: string;
  /** Chart series: a lone series and coolant, and oil. */
  series1: string;
  series2: string;
  /** OpenFreeMap style for the drive map. */
  mapStyle: 'dark' | 'positron';
  dark: boolean;
};

// The pod's own web pages' palette (webapi.cpp), so the app and the
// fallback pages look like one thing.
const DARK_BASE = {
  bg: '#0d1116',
  card: '#151b22',
  border: '#26313C',
  text: '#e7edf3',
  dim: '#8a97a3',
  link: '#17A9E6',
  bad: '#E5544B',
  good: '#4CC38A',
  series1: '#3987e5',
  series2: '#d95926',
  mapStyle: 'dark',
  dark: true,
} as const;

export const THEMES = {
  orange: { label: 'Dark – orange', colors: { ...DARK_BASE, accent: '#E8A33D' } },
  blue: { label: 'Dark – N blue', colors: { ...DARK_BASE, accent: '#4FA3F0' } },
  // Close to `bad` - an error note always says what is wrong in words.
  red: { label: 'Dark – red', colors: { ...DARK_BASE, accent: '#F0646E' } },
  light: {
    label: 'Light',
    colors: {
      bg: '#f2f4f7',
      card: '#ffffff',
      border: '#d5dce3',
      text: '#111820',
      dim: '#5B6773',
      accent: '#A85C00',
      link: '#0B6BB0',
      bad: '#C0392B',
      good: '#1E7E4F',
      series1: '#2a78d6',
      series2: '#eb6834',
      mapStyle: 'positron',
      dark: false,
    },
  },
} satisfies Record<string, { label: string; colors: Colors }>;

export type ThemeId = keyof typeof THEMES;
/** What Settings stores: a theme, or 'system' - Light or Dark – orange as the phone is set. */
export type ThemeChoice = ThemeId | 'system';

export const ColorsContext = createContext<Colors>(THEMES.orange.colors);

export function useColors(): Colors {
  return useContext(ColorsContext);
}

/**
 * A hook for a file's styles, made per theme: `const useStyles =
 * themed((C) => StyleSheet.create({...}))` at the top level, then
 * `const styles = useStyles()` in the component.
 */
export function themed<T>(make: (c: Colors) => T): () => T {
  return function useThemed() {
    const c = useColors();
    return useMemo(() => make(c), [c]);
  };
}

export const S = { xs: 4, sm: 8, md: 16, lg: 24 } as const;
