// The app's own data as one JSON file (asked 2026-11-12, before a reinstall
// for the new signing key): what an uninstall loses and nothing brings back
// - the places and drive names, the fuel picks, the settings. Each is a small
// file in the app's documents (store.ts and the rest); a backup is their
// contents by name, null for a file that was not there - several settings
// are "on" by their file existing, so a restore deletes those too.
//
// Not in it, on purpose: the drives and their index (they sync back from
// the gauge or the home server), the logs folder (Android's permission for
// it is this install's), the log key and the home server's token (secrets -
// in KeePass, and the token on the server), what the gauge says of itself.
//
// No I/O here, so the tests can run it; backup-card.tsx reads and writes.

export const BACKUP_FILES = [
  'names.json',             // your places, renamed drives (names.ts)
  'fuels.json',             // the fuel at each fill-up (fuel-grade.ts)
  'list-facts.json',        // which figures the trip list shows
  'theme.txt',
  'language.txt',
  'units-mph.txt',
  'keep-connected.txt',     // "Disconnect after syncing" off
  'auto-save.txt',          // "Save new logs after each sync" on
  'beacon.json',            // the beacon's switches and the sessions known
  'delete-on-gauge.json',   // drives deleted in the app, not yet on the gauge
  'delete-on-home.json',    // ...nor on the home server
] as const;

export type BackupFiles = Partial<Record<(typeof BACKUP_FILES)[number], string | null>>;
export type Backup = { app: 'n-gauge'; backup: 1; at: number; files: BackupFiles };

export function makeBackup(files: BackupFiles, at: number): string {
  return JSON.stringify({ app: 'n-gauge', backup: 1, at, files } satisfies Backup, null, 1);
}

/** The backup in `text`, or why not, in English. Only the files it knows. */
export function readBackup(text: string): Backup | string {
  let b: unknown;
  try {
    b = JSON.parse(text);
  } catch {
    return 'not a JSON file';
  }
  const o = b as Partial<Backup> | null;
  if (!o || o.app !== 'n-gauge' || typeof o.files !== 'object' || o.files === null) return 'not an N Gauge backup';
  if (o.backup !== 1) return 'a backup from a newer app';
  const files: BackupFiles = {};
  for (const name of BACKUP_FILES) {
    const v = (o.files as Record<string, unknown>)[name];
    if (typeof v === 'string' || v === null) files[name] = v;
  }
  return { app: 'n-gauge', backup: 1, at: typeof o.at === 'number' ? o.at : 0, files };
}
