// The home server sync itself (home.ts), apart from React: the app runs it
// (use-sync.tsx - on opening, on return, after leaving the gauge's WiFi) and
// so does the background task (home-task.ts - about every hour, the app
// closed). One at a time: both write the same inbox.
//
// What the server has that the phone lacks comes into the inbox as the
// gauge's own sync brings it - the same files, by the same names - and
// drives deleted in the app go from the server first (it keeps their names).

import { Directory, File, FileMode, Paths } from 'expo-file-system';

import { deleteOnHome, homeDownloader, type HomeServer, listHome, loadHome, putHome, reachable } from './home';
import {
  inboxFile,
  knownBytes,
  loadDeleted,
  loadGaugeFiles,
  loadHomeDeleted,
  loadInbox,
  pull,
  saveHomeDeleted,
  saveLastHomeSync,
} from './store';
import { type PodFile, planPull, planRelay, pullBytes } from './sync-plan';
import { dropStaleCopies } from './trip-cache';

export type HomeSyncProgress = { at: number; of: number; got: number; total: number };

export type HomeSyncHooks = {
  signal?: AbortSignal;
  /** Listing is done, files are coming: the bytes so far against all of them. */
  onProgress?: (p: HomeSyncProgress) => void;
  /** A file landed in the inbox. */
  onLanded?: () => void;
};

/**
 * 'unset': no server set up (the gauge's Upload card). 'away': not reachable
 * from here. 'busy': one is running already. Otherwise how many files came,
 * and whether that was all of them. Anything else wrong throws.
 */
export type HomeSyncResult =
  | { state: 'unset' | 'away' | 'busy' }
  | { state: 'done'; files: number; complete: boolean; relayed: number };

let busy = false;

export async function syncFromHome(hooks: HomeSyncHooks = {}): Promise<HomeSyncResult> {
  if (busy) return { state: 'busy' };
  busy = true;
  try {
    const h = await loadHome();
    if (!h) return { state: 'unset' };
    if (!(await reachable(h))) return { state: 'away' };

    // Deleted drives go from the server first, and are never pulled.
    const pending = loadHomeDeleted();
    const left: string[] = [];
    for (const n of pending) if (!(await deleteOnHome(h, n))) left.push(n);
    if (left.length !== pending.length) saveHomeDeleted(left);
    const skip = () => new Set([...loadDeleted(), ...loadHomeDeleted()]);

    // First what the phone has and the server lacks: finished drives pulled
    // from the gauge since (relay), so the server - the place the logs are
    // kept - has them before the gauge's own next upload. Then listed again.
    const relayed = await relayToHome(h, await listHome(h), skip(), hooks.signal);
    const files = await listHome(h);
    // A shorter copy on the server is behind, not another file (planPull).
    const todo = planPull(files, knownBytes(loadInbox()), skip(), true);
    const download = homeDownloader(h);
    const total = pullBytes(todo);
    let got = 0;
    let before = 0;
    let shownAt = 0;
    for (const [i, p] of todo.entries()) {
      if (hooks.signal?.aborted) break;
      hooks.onProgress?.({ at: i + 1, of: todo.length, got: before, total });
      await pull(
        p,
        (b) => {
          const now = Date.now();
          if (now - shownAt < 250) return;      // at most four a second
          shownAt = now;
          hooks.onProgress?.({ at: i + 1, of: todo.length, got: before + b, total });
        },
        hooks.signal,
        download,
      );
      before += p.size - p.from;
      got++;
      hooks.onLanded?.();
    }
    // A log the gauge renamed since came under its dated name: the old copy goes.
    if (got) dropStaleCopies();
    const complete = got === todo.length;
    if (complete) saveLastHomeSync({ at: Date.now(), files: got });
    return { state: 'done', files: got, complete, relayed };
  } finally {
    busy = false;
  }
}

// A piece at a time, each through a file in the cache: the upload is native
// (home.ts putHome), and the server writes each piece onto what it holds.
const RELAY_PIECE = 4 * 1024 * 1024;
const relayDir = new Directory(Paths.cache, 'relay');

/**
 * The finished drives pulled from the gauge that the server lacks, onto the
 * server (planRelay: closed on the gauge, whole on the phone, from the
 * server's size on). How many files went; one that fails is left for the
 * gauge's own upload, the rest go on.
 */
async function relayToHome(h: HomeServer, home: PodFile[], skip: ReadonlySet<string>, signal?: AbortSignal): Promise<number> {
  const inbox = loadInbox();
  const phone = Object.fromEntries(Object.entries(inbox).map(([n, e]) => [n, e.bytes]));
  const todo = planRelay(loadGaugeFiles(), phone, home, skip);
  if (!todo.length) return 0;
  if (!relayDir.exists) relayDir.create({ intermediates: true });
  let done = 0;
  for (const r of todo) {
    if (signal?.aborted) break;
    const src = inboxFile(r.name).open(FileMode.ReadOnly);
    try {
      let at = r.from;
      while (at < r.size) {
        if (signal?.aborted) break;
        const n = Math.min(RELAY_PIECE, r.size - at);
        src.offset = at;
        const piece = new File(relayDir, 'piece');
        if (piece.exists) piece.delete();
        piece.create();
        piece.write(src.readBytes(n));
        const held = await putHome(h, r.name, at, piece);
        piece.delete();
        if (held !== at + n) throw new Error(`${r.name}: the server holds ${held}, not ${at + n}`);
        at = held;
      }
      if (at === r.size) done++;
    } catch (e) {
      console.warn('relay', r.name, e);   // the gauge's own upload sends it then
    } finally {
      src.close();
    }
  }
  return done;
}
