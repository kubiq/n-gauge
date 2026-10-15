/// <reference types="node" />
import assert from 'node:assert/strict';
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';

import { cs } from './cs.ts';
import { setLang, t, tn } from './index.ts';

// Every source file's t('...') and tn('...', '...') - with literal text, as
// the convention is: a t() of a variable could not be checked here.
function sources(dir: string): string[] {
  return readdirSync(dir).flatMap((n) => {
    const p = join(dir, n);
    if (statSync(p).isDirectory()) return n === 'i18n' ? [] : sources(p);
    return /\.tsx?$/.test(n) && !/\.test\.ts$/.test(n) ? [p] : [];
  });
}
// A JS string literal in ' or ` (no ${} inside), its escapes undone.
const STR = String.raw`'((?:[^'\\]|\\.)*)'|` + '`([^`$]*)`';
const unq = (m: RegExpExecArray, i: number) => (m[i] ?? m[i + 1]).replace(/\\(.)/g, '$1');
// t('...') and the load-time mark N_('...').
const T = new RegExp(String.raw`\b(?:t|N_)\(\s*(?:${STR})`, 'g');
const TN = new RegExp(String.raw`\btn\(\s*[^,]+,\s*(?:${STR})\s*,\s*(?:${STR})`, 'g');

const used = { one: new Set<string>(), plural: new Set<string>() };
for (const f of sources('src')) {
  const text = readFileSync(f, 'utf8');
  for (const m of text.matchAll(T)) used.one.add(unq(m, 1));
  for (const m of text.matchAll(TN)) used.plural.add(`${unq(m, 1)}|${unq(m, 3)}`);
}
const holes = (s: string) => [...s.matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort().join(',');

test('i18n: every t() text has its Czech, with the same placeholders', () => {
  const missing = [...used.one].filter((en) => !(en in cs));
  assert.deepEqual(missing, [], `no Czech for:\n${missing.join('\n')}`);
  for (const en of used.one) assert.equal(holes(cs[en]), holes(en), `placeholders differ: ${en}`);
});

test('i18n: every tn() has three Czech forms, each with the same placeholders', () => {
  const missing = [...used.plural].filter((k) => !(k in cs));
  assert.deepEqual(missing, [], `no Czech for:\n${missing.join('\n')}`);
  for (const k of used.plural) {
    const forms = cs[k].split('|');
    assert.equal(forms.length, 3, `three forms (1 | 2-4 | 5+): ${k}`);
    const want = holes(k.split('|')[1]);
    for (const f of forms) assert.ok(holes(f).split(',').every((h) => !h || want.includes(h)), `${k}: ${f}`);
  }
});

test('i18n: Czech plurals and placeholders', () => {
  setLang('cs');
  const k = '1 drive|{n} drives';
  cs[k] = '{n} jízda|{n} jízdy|{n} jízd';
  assert.deepEqual([1, 2, 4, 5, 0].map((n) => tn(n, '1 drive', '{n} drives')), ['1 jízda', '2 jízdy', '4 jízdy', '5 jízd', '0 jízd']);
  assert.equal(t('No such text {x}', { x: 3 }), 'No such text 3');   // falls back to English
  setLang('en');
  assert.equal(tn(2, '1 drive', '{n} drives'), '2 drives');
});
