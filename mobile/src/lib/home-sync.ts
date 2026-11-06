// The home server sync itself (home.ts), apart from React: the app runs it
// (use-sync.tsx - on opening, on return, after leaving the gauge's WiFi) and
// so does the background task (home-task.ts - about every hour, the app
// closed). One at a time: both write the same inbox.
//
// What the server has that the phone lacks comes into the inbox as the
// gauge's own sync brings it - the same files, by the same names - and
// drives deleted in the app go from the server first (it keeps their names).

import { deleteOnHome, homeDownloader, listHome, loadHome, reachable } from './home';
import { knownBytes, loadDeleted, loadHomeDeleted, loadInbox, pull, saveHomeDeleted, saveLastHomeSync } from './store';
import { planPull, pullBytes } from './sync-plan';
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
  | { state: 'done'; files: number; complete: boolean };

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

    const files = await listHome(h);
    const todo = planPull(files, knownBytes(loadInbox()), skip());
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
    return { state: 'done', files: got, complete };
  } finally {
    busy = false;
  }
}
