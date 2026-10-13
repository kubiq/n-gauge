// Pod -> phone sync, shared by every screen, and run by itself: each time the
// app connects to the pod it lists the card and pulls what is new. Having to
// open the Sync tab and tap Pull meant drives stayed on the pod unnoticed
// (2026-10-12: two drives, synced "already", never pulled).
//
// Lives beside the connection rather than in the Sync tab so it runs from any
// screen, and so other screens can stay out of its way: the pod serves one
// request at a time, so the live readout pauses while a pull runs, and a
// firmware upload cancels it first.

import { createContext, type ReactNode, useCallback, useContext, useEffect, useRef, useState } from 'react';

import { usePod } from '@/hooks/use-pod';
import { listFiles } from '@/lib/pod';
import { dropNonLogs, type Inbox, knownBytes, loadInbox, pull } from '@/lib/store';
import { renameLegacyLogs } from '@/lib/trip-cache';
import { type PodFile, planPull, type Pull } from '@/lib/sync-plan';

const mb = (n: number) => `${(n / 1048576).toFixed(n < 10 * 1048576 ? 1 : 0)} MB`;

type Phase = 'idle' | 'checking' | 'pulling';

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
    return dropNonLogs();
  });
  const [landed, setLanded] = useState(0);
  // One check or pull at a time - the pod serves one request at a time anyway.
  const running = useRef<Promise<void> | null>(null);
  const abort = useRef<AbortController | null>(null);
  // Set by cancel(): also stops a run that is still listing from going on to pull.
  const stopped = useRef(false);

  const listAndPlan = async (): Promise<{ files: PodFile[]; plan: Pull[] }> => {
    setPhase('checking');
    const t0 = Date.now();
    const files = await listFiles();
    const p = planPull(files, knownBytes(loadInbox()));
    // Shown because listing holds the pod's main loop - worth knowing as the card fills.
    setStatus(`Listed in ${((Date.now() - t0) / 1000).toFixed(1)} s`);
    setPodFiles(files);
    setPlan(p);
    return { files, plan: p };
  };

  const pullPlan = async (files: PodFile[], todo: Pull[]) => {
    const ctl = new AbortController();
    abort.current = ctl;
    setPhase('pulling');
    let done = 0;
    try {
      for (const [i, p] of todo.entries()) {
        if (ctl.signal.aborted) break;
        await pull(
          p,
          (b) => setStatus(`${i + 1}/${todo.length} ${p.name}: ${mb(b)} of ${mb(p.size - p.from)}`),
          ctl.signal,
        );
        done++;
        // Per file, so "On the phone", the count of what is left and the
        // Trips list move as each one lands.
        const now = loadInbox();
        setInbox(now);
        setPlan(planPull(files, knownBytes(now)));
        setLanded((n) => n + 1);
      }
      setStatus(`Pulled ${done} file${done === 1 ? '' : 's'}.`);
    } catch (e) {
      // Whatever finished is in the inbox; the next check plans the rest.
      if (ctl.signal.aborted) setStatus(`Cancelled after ${done} of ${todo.length}.`);
      else setErr(`Stopped after ${done} of ${todo.length}: ${(e as Error).message}`);
    } finally {
      abort.current = null;
    }
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
        if (r.plan.length && !stopped.current) await pullPlan(r.files, r.plan);
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

  return (
    <SyncContext.Provider value={{ phase, podFiles, plan, status, err, inbox, landed, sync, check, cancel }}>
      {children}
    </SyncContext.Provider>
  );
}

export function useSync(): Sync {
  const s = useContext(SyncContext);
  if (!s) throw new Error('useSync outside SyncProvider');
  return s;
}
