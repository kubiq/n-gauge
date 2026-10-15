// The app's words in the chosen language: English, or Czech.
//
// The English text is the key - t('Delete this drive?') - and each other
// language is a dictionary from it (cs.ts); a text missing there shows in
// English. i18n.test.ts scans the source for every t() and tn() and fails
// on a Czech one missing, or on placeholders that differ.
//
// t() reads the current language from here, not from React, so library
// code (a sync's status line, a drive's "Around ...") uses it too. The
// language provider (use-language.tsx) sets it and redraws the app on a
// change.

import { cs } from './cs.ts';

export type Lang = 'en' | 'cs';
export type LangChoice = Lang | 'system';
type Params = Record<string, string | number>;

const DICTS: Record<Exclude<Lang, 'en'>, Record<string, string>> = { cs };
let current: Lang = 'en';

export function setLang(l: Lang): void {
  current = l;
}

export function lang(): Lang {
  return current;
}

/** For dates and numbers, so they follow the app's language, not the phone's. */
export function locale(): string {
  return current === 'cs' ? 'cs-CZ' : 'en-GB';
}

function fill(s: string, p?: Params): string {
  return p ? s.replace(/\{(\w+)\}/g, (m, k: string) => (k in p ? String(p[k]) : m)) : s;
}

/** `en` in the current language; {name} placeholders from `p`. */
export function t(en: string, p?: Params): string {
  return fill(current === 'en' ? en : (DICTS[current][en] ?? en), p);
}

/**
 * Marks text in a table made at load time - a setting's label, a theme's
 * name - for translation, and returns it unchanged: the screen shows it
 * through t(label), when the language is known. i18n.test.ts finds it by
 * this mark, so its Czech is checked like any t()'s.
 */
export const N_ = (en: string): string => en;

/**
 * A count with its noun: tn(n, '1 drive', '{n} drives'). English has two
 * forms; Czech three - 1, 2-4, 5 and up (and 0) - kept in cs.ts under the
 * key "one|other" as "jedna|dvě až čtyři|pět", '|' between them.
 */
export function tn(n: number, one: string, other: string, p?: Params): string {
  const params = { n, ...p };
  if (current === 'en') return fill(n === 1 ? one : other, params);
  const forms = DICTS[current][`${one}|${other}`]?.split('|');
  if (!forms || forms.length !== 3) return fill(n === 1 ? one : other, params);
  const i = n === 1 ? 0 : Number.isInteger(n) && n >= 2 && n <= 4 ? 1 : 2;
  return fill(forms[i], params);
}
