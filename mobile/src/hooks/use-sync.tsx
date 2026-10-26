// Pod -> phone sync, shared by every screen, and run by itself: each time the
// app connects to the pod it lists the card and pulls what is new. Having to
// open the Sync tab and tap Pull meant drives stayed on the pod unnoticed
// (2026-10-12: two drives, synced "already", never pulled).
//
// Lives beside the connection so it runs from any screen, and so other
// screens can stay out of its way: the pod serves one request at a time, so
// saving its settings or uploading firmware cancels a pull first.
//
// What it did is said in a toast - the files that came over, or why it
// stopped - since there is no screen to watch it on: a sync is a tap on the
// gauge in the header, and the phone leaves the gauge's WiFi when it is done.

import { createContext, type ReactNode, useCallback, useContext, useEffect, useRef, useState } from 'react';
import { AppState, ToastAndroid } from 'react-native';

import { usePod } from '@/hooks/use-pod';
import { t, tn } from '@/i18n';
import { loadBeaconAutoSync, newDriveBeacon, updateKnownBeacons } from '@/lib/beacon';
import { deletePodFile, getLive, listFiles } from '@/lib/pod';
import {
  dropNonLogs,
  type Inbox,
  knownBytes,
  loadDeleted,
  loadDisconnectAfterSync,
  loadInbox,
  loadLastSync,
  pull,
  saveDeleted,
  saveGaugeFw,
  saveLastSync,
} from '@/lib/store';
import { dropStaleCopies, renameLegacyLogs } from '@/lib/trip-cache';
import { type PodFile, planPull, type Pull, pullBytes } from '@/lib/sync-plan';

type Phase = 'idle' | 'checking' | 'pulling';

/**
 * While pulling: the file (1-based) of how many, and the bytes of the whole
 * pull so far against all of it - a 17 MB GPX said nothing of how far it
 * was, only "1/3" (asked 2026-10-22).
 */
export type SyncProgress = { at: number; of: number; got: number; total: number };

type Sync = {
  phase: Phase;
  /** The pod's listing from the last check this connection; null before one. */
  podFiles: PodFile[] | null;
  /** What is still to pull, recomputed as files land. */
  plan: Pull[] | null;
  status: string | null;
  err: string | null;
  inbox: Inbox;
  /** Bumped each time a file lands, for screens that list the inbox. */
  landed: number;
  progress: SyncProgress | null;
  /** When a sync last finished with nothing left to pull (ms), across restarts. */
  lastSync: number | null;
  /** Check, then pull whatever is new. */
  sync: () => Promise<void>;
  check: () => Promise<void>;
  /** Stops a running pull; resolves once it has stopped. */
  cancel: () => Promise<void>;
};

const SyncContext = createContext<Sync | null>(null);

export function SyncProvider({ children }: { children: ReactNode }) {
  const pod = usePod();
  const connected = pod.state === 'connected';
  const [phase, setPhase] = useState<Phase>('idle');
  const [podFiles, setPodFiles] = useState<PodFile[] | null>(null);
  const [plan, setPlan] = useState<Pull[] | null>(null);
  const [status, setStatus] = useState<string | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [inbox, setInbox] = useState(() => {
    // Before anything lists the inbox: Trips pairs by the session in the names.
    try {
      renameLegacyLogs();
    } catch (e) {
      console.warn('renaming old logs failed', e);
    }
    // A numbered copy of a log the gauge renamed since: it would pair with
    // the drive's GPX in place of the whole log (trip-cache.ts).
    try {
      dropStaleCopies();
    } catch (e) {
      console.warn('dropping stale copies failed', e);
    }
    return dropNonLogs();
  });
  const [landed, setLanded] = useState(0);
  const [progress, setProgress] = useState<SyncProgress | null>(null);
  const [lastSync, setLastSync] = useState(loadLastSync);
  // The connection (pod.session) a sync last finished on with nothing left
  // to pull. Per connection, so a new one is not taken as synced before its
  // own sync has begun.
  const [syncedSession, setSyncedSession] = useState(-1);
  const session = useRef(pod.session);
  // Files pulled since this connection began, for the message on leaving it.
  const pulledThisSession = useRef(0);
  useEffect(() => {
    session.current = pod.session;
    pulledThisSession.current = 0;
  }, [pod.session]);
  // One check or pull at a time - the pod serves one request at a time anyway.
  const running = useRef<Promise<void> | null>(null);
  const abort = useRef<AbortController | null>(null);
  // Set by cancel(): also stops a run that is still listing from going on to pull.
  const stopped = useRef(false);

  const listAndPlan = async (): Promise<{ files: PodFile[]; plan: Pull[] }> => {
    setPhase('checking');
    setStatus(null);
    const listed = await listFiles();
    // Drives deleted on the phone go from the gauge too, before planning: a
    // name the gauge no longer has, or has just deleted, leaves the list; one
    // it cannot delete yet (still being written) stays, and is not pulled.
    const deleted = loadDeleted();
    const onGauge = new Set(listed.map((f) => f.name));
    const left: string[] = [];
    for (const n of deleted) {
      if (onGauge.has(n) && (await deletePodFile(n)) === 'later') left.push(n);
    }
    if (left.length !== deleted.length) saveDeleted(left);
    const files = listed.filter((f) => !deleted.includes(f.name) || left.includes(f.name));
    const p = planPull(files, knownBytes(loadInbox()), new Set(left));
    setPodFiles(files);
    setPlan(p);
    return { files, plan: p };
  };

  // True when every file landed.
  const pullPlan = async (files: PodFile[], todo: Pull[]): Promise<boolean> => {
    const ctl = new AbortController();
    abort.current = ctl;
    setPhase('pulling');
    let done = 0;
    const total = pullBytes(todo);
    let before = 0;               // bytes of the files already pulled
    let shownAt = 0;
    try {
      for (const [i, p] of todo.entries()) {
        if (ctl.signal.aborted) break;
        setProgress({ at: i + 1, of: todo.length, got: before, total });
        await pull(
          p,
          (b) => {
            // At most four times a second: every screen re-renders on it.
            const now = Date.now();
            if (now - shownAt < 250) return;
            shownAt = now;
            setProgress({ at: i + 1, of: todo.length, got: before + b, total });
          },
          ctl.signal,
        );
        before += p.size - p.from;
        done++;
        pulledThisSession.current++;
        // Per file, so "On the phone", the count of what is left and the
        // Trips list move as each one lands.
        const now = loadInbox();
        setInbox(now);
        setPlan(planPull(files, knownBytes(now), new Set(loadDeleted())));
        setLanded((n) => n + 1);
      }
      setStatus(tn(done, 'Pulled 1 file.', 'Pulled {n} files.'));
    } catch (e) {
      // Whatever finished is in the inbox; the next check plans the rest.
      if (ctl.signal.aborted) setStatus(t('Cancelled after {done} of {total}.', { done, total: todo.length }));
      else
        setErr(
          t('Stopped after {done} of {total}: {error}', { done, total: todo.length, error: (e as Error).message }),
        );
    } finally {
      abort.current = null;
      setProgress(null);
    }
    return done === todo.length;
  };

  // Each run is one promise in `running`, so cancel() can wait for it.
  const run = useCallback((job: () => Promise<void>): Promise<void> => {
    if (running.current) return running.current;
    stopped.current = false;
    setErr(null);
    const p = (async () => {
      try {
        await job();
      } catch (e) {
        setErr(String((e as Error).message));
      } finally {
        running.current = null;
        setPhase('idle');
      }
    })();
    running.current = p;
    return p;
  }, []);

  const check = useCallback(() => run(async () => void (await listAndPlan())), [run]);

  const sync = useCallback(
    () =>
      run(async () => {
        const r = await listAndPlan();
        const complete = !r.plan.length || (!stopped.current && (await pullPlan(r.files, r.plan)));
        // A log renamed on the gauge since the last sync came again under
        // its dated name: the old numbered copy goes now, not at the next start.
        if (r.plan.length && dropStaleCopies().length) {
          setInbox(loadInbox());
          setLanded((n) => n + 1);
        }
        if (complete) {
          const now = Date.now();
          saveLastSync(now);
          setLastSync(now);
          // Its build, for Settings > About: read while on its WiFi. Older
          // firmware does not say; a failure here is no failed sync.
          try {
            const fw = (await getLive()).fw;
            if (fw) saveGaugeFw(fw.build);
          } catch {
            // the next sync tries again
          }
          setSyncedSession(session.current);
        }
      }),
    [run],
  );

  const cancel = useCallback(async () => {
    stopped.current = true;
    abort.current?.abort();
    await running.current;
  }, []);

  // On every connect. Leaving the pod's network stops a pull - it could not
  // finish anyway. The old listing stays, but screens show it only while
  // connected, and the next connect's check replaces it at once.
  useEffect(() => {
    if (connected) {
      sync();
      return;
    }
    stopped.current = true;
    abort.current?.abort();
  }, [connected, sync]);

  // Synced, nothing left on the gauge: off its WiFi, so the phone is back on
  // the internet - maps, place names - without a trip to Android's settings.
  // Not while a screen that needs the gauge is open (its settings, the log
  // key): then once it closes. A sync that stopped short stays connected, to
  // be tried again. Switched off in Settings ("Disconnect after syncing").
  // Said in a toast, so it does not just look like the WiFi dropping: what
  // came over this connection, and that the phone let go of the gauge.
  const { holders, disconnect } = pod;
  const synced = syncedSession === pod.session;
  useEffect(() => {
    if (!(connected && synced && phase === 'idle' && holders === 0 && loadDisconnectAfterSync())) return;
    const n = pulledThisSession.current;
    disconnect();
    ToastAndroid.show(
      n
        ? tn(n, '1 new file from the gauge - disconnected.', '{n} new files from the gauge - disconnected.')
        : t('Nothing new on the gauge - disconnected.'),
      ToastAndroid.LONG,
    );
  }, [connected, synced, phase, holders, disconnect]);

  // The background check (beacon.ts) learns of each drive as it lands, so
  // its note goes once the drive is on the phone.
  useEffect(() => {
    updateKnownBeacons().catch((e) => console.warn('known beacons', e));
  }, [inbox]);

  // "Sync new drives when the app opens": on opening, the gauge's beacon
  // names a drive the phone does not have - connect, and the connect syncs.
  // Once per drive per run of the app, so a declined prompt is not asked
  // again at each return to the app.
  const { state, creds, connect } = pod;
  const triedBeacon = useRef<string | null>(null);
  useEffect(() => {
    const look = async () => {
      if (state !== 'idle' || !creds || !loadBeaconAutoSync()) return;
      const fp = await newDriveBeacon();
      if (!fp || fp === triedBeacon.current) return;
      triedBeacon.current = fp;
      connect(false);
    };
    look().catch((e) => console.warn('beacon', e));
    const sub = AppState.addEventListener('change', (s) => {
      if (s === 'active') look().catch((e) => console.warn('beacon', e));
    });
    return () => sub.remove();
  }, [state, creds, connect]);

  // A sync that stopped says why - still connected, so a tap on the gauge
  // in the header tries again (pod-chip.tsx).
  useEffect(() => {
    if (err) ToastAndroid.show(t('Sync failed: {error}', { error: err }), ToastAndroid.LONG);
  }, [err]);

  return (
    <SyncContext.Provider
      value={{ phase, podFiles, plan, status, err, inbox, landed, progress, lastSync, sync, check, cancel }}>
      {children}
    </SyncContext.Provider>
  );
}

export function useSync(): Sync {
  const s = useContext(SyncContext);
  if (!s) throw new Error('useSync outside SyncProvider');
  return s;
}
