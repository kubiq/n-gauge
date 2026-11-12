/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { makeBackup, readBackup } from './backup-format.ts';

test('a backup reads back as written, absent files as null', () => {
  const text = makeBackup({ 'names.json': '{"places":[]}', 'theme.txt': 'dark', 'auto-save.txt': null }, 1234);
  const b = readBackup(text);
  assert.ok(typeof b !== 'string');
  assert.equal(b.at, 1234);
  assert.deepEqual(b.files, { 'names.json': '{"places":[]}', 'theme.txt': 'dark', 'auto-save.txt': null });
});

test('only the files it knows: nothing else is ever written back', () => {
  const b = readBackup(JSON.stringify({ app: 'n-gauge', backup: 1, at: 0, files: { 'inbox.json': '{}', '../x': 'y', 'theme.txt': 7 } }));
  assert.ok(typeof b !== 'string');
  assert.deepEqual(b.files, {});
});

test('anything else is refused, with why', () => {
  assert.equal(readBackup('nope'), 'not a JSON file');
  assert.equal(readBackup('{"files":{}}'), 'not an N Gauge backup');
  assert.equal(readBackup('{"app":"n-gauge","backup":2,"files":{}}'), 'a backup from a newer app');
});
