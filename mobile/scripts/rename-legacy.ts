// Gives the logs on the pod's card - and in folders of copies saved from the
// app - the session ids the phone gives its own copies (src/lib/legacy-names.ts,
// the same code), so a sync afterwards finds nothing new.
//
//   node --experimental-strip-types scripts/rename-legacy.ts <card> [<copies>...] [--apply]
//
// <card> is the card's root, in a reader. The ids come from ITS names; the
// copies (decrypted, so without ".nge") follow the card. Prints what it would
// do; --apply does it.

import { existsSync, readdirSync, renameSync } from 'node:fs';
import { join } from 'node:path';

import { legacyRenames } from '../src/lib/legacy-names.ts';
import { plainName } from '../src/lib/trips.ts';

const args = process.argv.slice(2);
const apply = args.includes('--apply');
const [card, ...copies] = args.filter((a) => a !== '--apply');
if (!card) {
  console.error('usage: rename-legacy.ts <card> [<copies>...] [--apply]');
  process.exit(2);
}

const renames = legacyRenames(readdirSync(card));
const plain = Object.fromEntries(Object.entries(renames).map(([a, b]) => [plainName(a), plainName(b)]));

function renameIn(dir: string, map: Record<string, string>) {
  let n = 0;
  for (const name of readdirSync(dir)) {
    const to = map[name];
    if (!to) continue;
    if (existsSync(join(dir, to))) {
      console.error(`skip ${join(dir, name)}: ${to} already exists`);
      continue;
    }
    console.log(`${join(dir, name)} -> ${to}`);
    if (apply) renameSync(join(dir, name), join(dir, to));
    n++;
  }
  console.log(`${dir}: ${n} ${apply ? 'renamed' : 'to rename (dry run, --apply to do it)'}\n`);
}

renameIn(card, renames);
for (const dir of copies) renameIn(dir, { ...renames, ...plain });
