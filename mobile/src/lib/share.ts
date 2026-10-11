// Handing logs on, decrypted: to other apps through Android's share sheet
// (Strava, gpx.studio, OsmAnd, a messenger), or as files into a folder the
// user picks (one synced to the PC, say). The app is where the key is, so it
// is also where encrypted logs become readable again - there is no PC tool.

import { Directory, File, Paths } from 'expo-file-system';
import * as Sharing from 'expo-sharing';

import { cleanGpx } from './gpx-share';
import { readLogText } from './logtext';
import { isGpxName, plainName } from './trips';

const dir = new Directory(Paths.cache, 'share');

const mimeOf = (name: string) => (isGpxName(name) ? 'application/gpx+xml' : 'text/csv');

/**
 * A log as other software should get it: decrypted, under its plain name,
 * and a GPX cleaned to valid XML (cleanGpx: whole trkpt lines, one closing
 * tail - the encrypted writer leaves the tail to the reader).
 */
async function readableText(name: string): Promise<string> {
  const text = await readLogText(name);
  return isGpxName(name) ? cleanGpx(text) : text;
}

/** One log to another app. A copy is shared, never the inbox file itself. */
export async function shareLog(name: string): Promise<void> {
  if (!(await Sharing.isAvailableAsync())) throw new Error('Sharing is not available on this device');
  if (!dir.exists) dir.create({ intermediates: true });
  for (const old of dir.list()) old.delete();
  const out = new File(dir, plainName(name));
  out.write(await readableText(name));
  await Sharing.shareAsync(out.uri, {
    mimeType: mimeOf(name),
    dialogTitle: isGpxName(name) ? 'Share the track' : 'Share the car data',
  });
}

export type SaveResult = { saved: number; skipped: number; failed: string[] };

export function savedText(r: SaveResult): string {
  const parts = [`${r.saved} saved`];
  if (r.skipped) parts.push(`${r.skipped} already there`);
  let s = parts.join(', ') + '.';
  if (r.failed.length) s += `\n\nNot saved:\n${r.failed.join('\n')}`;
  return s;
}

/**
 * Logs into a folder the user picks (Android's folder picker), decrypted.
 * A file already there under the plain name is skipped, so saving again
 * after the next sync only adds what is new. Throws if no folder is picked.
 */
export async function saveLogsToFolder(names: string[], onProgress?: (text: string) => void): Promise<SaveResult> {
  const target = await Directory.pickDirectoryAsync();
  const there = new Set(target.list().map((e) => e.name));
  const res: SaveResult = { saved: 0, skipped: 0, failed: [] };
  for (const [i, name] of names.entries()) {
    const plain = plainName(name);
    if (there.has(plain)) {
      res.skipped++;
      continue;
    }
    onProgress?.(`Saving ${i + 1} of ${names.length}: ${plain}`);
    try {
      target.createFile(plain, mimeOf(name)).write(await readableText(name));
      res.saved++;
    } catch (e) {
      res.failed.push(`${plain}: ${(e as Error).message}`);
    }
  }
  return res;
}
