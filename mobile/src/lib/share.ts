// Handing logs on, decrypted: to other apps through Android's share sheet
// (Strava, gpx.studio, OsmAnd, a messenger), or as files into a folder the
// user picks (one synced to the PC, say). The app is where the key is, so it
// is also where encrypted logs become readable again - there is no PC tool.

import { Directory, File, Paths } from 'expo-file-system';
import * as Sharing from 'expo-sharing';

import { t } from '../i18n/index.ts';
import { cleanGpx } from './gpx-share';
import { readLogText } from './logtext';
import { inboxFile, loadInbox, loadLogsFolder, saveLogsFolder } from './store';
import { tripFiles } from './trip-cache';
import { isGpxName, isVboName, plainName } from './trips';

/**
 * The track sessions' .vbo files the phone holds. Not drives, so not in the
 * list (tripFiles), but "Save all to a folder" takes them too - they are
 * what Circuit Tools and other lap analysers open.
 */
export function trackLogNames(): string[] {
  return Object.keys(loadInbox()).filter((n) => isVboName(n) && inboxFile(n).exists).sort();
}

// The pod's own plain-text logs, which GROW: a line per sleep, a line per
// boot. Unlike a drive's files they are never finished, so "already there"
// is no reason to skip one - it is written over with the longer copy. When
// full, the pod moves one aside to a numbered name (sleeplog_0001.txt):
// finished from then on, so saved once like a drive's.
const GROWING_LOGS = ['sleeplog.txt', 'bootlog.txt'];
const POD_TEXT_LOG = /^(?:sleeplog|bootlog)(?:_\d{4})?\.txt$/;

/** The pod's sleep and boot logs the phone holds - "Save all" takes them too. */
export function podTextLogNames(): string[] {
  return Object.keys(loadInbox())
    .filter((n) => POD_TEXT_LOG.test(n) && inboxFile(n).exists)
    .sort();
}

const dir = new Directory(Paths.cache, 'share');

// A track session's .vbo has no type of its own; octet-stream, because the
// folder picker gives a name a type's extension when it lacks one -
// "track_....vbo.txt" as text/plain.
const mimeOf = (name: string) =>
  isGpxName(name) ? 'application/gpx+xml'
  : isVboName(name) ? 'application/octet-stream'
  : POD_TEXT_LOG.test(name) ? 'text/plain'
  : 'text/csv';

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
  if (!(await Sharing.isAvailableAsync())) throw new Error(t('Sharing is not available on this device'));
  if (!dir.exists) dir.create({ intermediates: true });
  for (const old of dir.list()) old.delete();
  const out = new File(dir, plainName(name));
  out.write(await readableText(name));
  await Sharing.shareAsync(out.uri, {
    mimeType: mimeOf(name),
    dialogTitle: isGpxName(name) ? t('Share the track') : t('Share the car data'),
  });
}

export type SaveResult = { saved: number; skipped: number; failed: string[] };

/**
 * What a save did, in one line - or the files it could not save. Not the
 * count of those already in the folder: after the first save that is nearly
 * all of them, and "3 saved, 412 already there" read as a list of skips
 * (2026-10-20).
 */
export function savedText(r: SaveResult): string {
  if (!r.failed.length) return t('Successfully synced.');
  return `${t('Not saved:')}\n${r.failed.join('\n')}`;
}

/** Asks for a folder with Android's picker and remembers it. Throws if none is picked. */
export async function pickLogsFolder(): Promise<Directory> {
  const picked = await Directory.pickDirectoryAsync(loadLogsFolder() ?? undefined);
  saveLogsFolder(picked.uri);
  return picked;
}

/** The folder's name for Settings, from its URI ("primary:Download/N gauge" -> "N gauge"). */
export function folderName(uri: string): string {
  return decodeURIComponent(uri).split(/[/:]/).filter(Boolean).pop() ?? uri;
}

// The remembered folder and what is in it - or, with none remembered or the
// remembered one gone (deleted, permission revoked: list() throws), the
// picker, once. Not asking (the save after a sync - nobody is there to
// pick): null instead of the picker.
async function logsFolder(ask: boolean): Promise<{ target: Directory; there: Map<string, File | Directory> } | null> {
  const listed = (target: Directory) => new Map(target.list().map((e) => [e.name, e] as const));
  const uri = loadLogsFolder();
  if (uri) {
    try {
      const target = new Directory(uri);
      return { target, there: listed(target) };
    } catch {
      saveLogsFolder(null);
    }
  }
  if (!ask) return null;
  const target = await pickLogsFolder();
  return { target, there: listed(target) };
}

/**
 * Every log "Save all to a folder" takes: each drive's GPX and CSV, the
 * track sessions' .vbo files and the pod's sleep and boot logs.
 */
export function allLogNames(): string[] {
  return [
    ...tripFiles().flatMap((t) => [t.gpx, t.csv]).filter((n): n is string => !!n),
    ...trackLogNames(),
    ...podTextLogNames(),
  ];
}

/**
 * Logs into the logs folder (remembered; picked the first time), decrypted.
 * A file already there under the plain name is skipped, so saving again
 * after the next sync only adds what is new. Throws if no folder is picked.
 */
export async function saveLogsToFolder(names: string[], onProgress?: (text: string) => void): Promise<SaveResult> {
  return saveInto((await logsFolder(true))!, names, onProgress);
}

/**
 * After a sync, with "Save new logs after each sync" on: every log into the
 * remembered folder - only the new ones are written, the rest are there.
 * Null with no folder remembered (or one gone): it never opens the picker.
 */
export async function saveNewLogs(): Promise<SaveResult | null> {
  const folder = await logsFolder(false);
  return folder ? saveInto(folder, allLogNames()) : null;
}

async function saveInto(
  { target, there }: { target: Directory; there: Map<string, File | Directory> },
  names: string[],
  onProgress?: (text: string) => void,
): Promise<SaveResult> {
  const res: SaveResult = { saved: 0, skipped: 0, failed: [] };
  for (const [i, name] of names.entries()) {
    const plain = plainName(name);
    const existing = there.get(plain);
    const growing = GROWING_LOGS.includes(plain);
    if (existing && !(growing && existing instanceof File)) {
      res.skipped++;
      continue;
    }
    onProgress?.(t('Saving {at} of {of}: {name}', { at: i + 1, of: names.length, name: plain }));
    try {
      // A growing log already there is written over (createFile would make
      // "bootlog (1).txt" beside it).
      const out = existing instanceof File ? existing : target.createFile(plain, mimeOf(name));
      out.write(await readableText(name));
      res.saved++;
    } catch (e) {
      res.failed.push(`${plain}: ${(e as Error).message}`);
    }
  }
  return res;
}
