// What to pull from the pod. Pure, so it can be tested without a pod.
//
// Files are compared by {name, size} only. The logger appends - and a
// numbered drive log is renamed to its dated name once the clock is known (a
// new name, so a new file here) - so a file that grew only needs its tail,
// fetched with a Range request. One exception: a GPX's closing tags are
// rewritten at every batch; pull() in store.ts continues over them.

/**
 * A file the gauge or the server lists. `open`: the gauge is still writing it
 * (the drive being recorded) - from firmware of 2026-11-16 on; undefined from
 * older firmware and from the server.
 */
export type PodFile = { name: string; size: number; open?: boolean };

/** Bytes of each file the phone already accounts for: uploaded, or held locally until uploaded. */
export type Known = Record<string, number>;

export type Pull = {
  name: string;
  /** First byte to fetch. */
  from: number;
  /** The pod's size now; the pull fetches [from, size). */
  size: number;
  /**
   * The pod's copy is SMALLER than what we have, so it is not the file we
   * know (a card swapped or reformatted, a name reused). Fetch it whole and
   * let the caller drop what it held under that name.
   */
  restart: boolean;
};

// What the pod itself writes (log_*.cpp, laps.cpp, the sleep and boot logs).
// Anything else on the card - a firmware .bin copied there, notes - is the
// owner's, not a log, and is not synced.
// Encrypted logs carry ".nge" after their own extension (logcrypt.h). A full
// sleep or boot log is moved aside to a numbered name (logger.cpp).
const POD_LOG = /^(?:(?:drive_.+\.csv|trip_.+\.gpx|track_.+\.vbo)(?:\.nge)?|(?:sleeplog|bootlog)(?:_\d{4})?\.txt)$/;

export function isPodLog(name: string): boolean {
  return POD_LOG.test(name);
}

/**
 * `deleted`: names deleted from the phone on purpose - never pulled back
 * (store.ts). `behind`: a copy shorter than the phone's is behind, not
 * another file - the home server's, which gets a drive only with the next
 * upload: swapped in, it cut a whole drive pulled from the gauge back to the
 * server's first minutes (2026-11-14: 2.3 km of a 2 h drive). From the gauge
 * a shorter file is another one under the same name (a card wiped), and
 * replaces the phone's.
 */
export function planPull(pod: PodFile[], known: Known, deleted: ReadonlySet<string> = new Set(), behind = false): Pull[] {
  const out: Pull[] = [];
  for (const f of pod) {
    if (!isPodLog(f.name) || deleted.has(f.name)) continue;
    const have = known[f.name] ?? 0;
    if (have === f.size) continue;
    if (have > f.size && behind) continue;
    if (have > f.size) out.push({ name: f.name, from: 0, size: f.size, restart: true });
    else out.push({ name: f.name, from: have, size: f.size, restart: false });
  }
  // Smallest first: a sync cut short by the pod going to sleep then has
  // finished as many whole files as it could, not half of one big GPX.
  return out.sort((a, b) => a.size - a.from - (b.size - b.from));
}

export function pullBytes(plan: Pull[]): number {
  return plan.reduce((n, p) => n + p.size - p.from, 0);
}

/** A file the phone passes on to the home server: bytes [from, size). */
export type Relay = { name: string; from: number; size: number };

/**
 * What the phone passes on to the home server after pulling from the gauge
 * (asked 2026-11-16: the server is the place the logs are kept, and the
 * gauge's own upload comes only with the next drive): the files the gauge
 * listed as closed (`open` false - its drive ended), that the phone holds
 * whole (the gauge's size) and the server holds less of. From the server's
 * size on: never a shorter copy over a longer one. Nothing from a gauge that
 * does not say what is open (older firmware): it might be the drive still
 * being recorded.
 */
export function planRelay(gauge: PodFile[], phone: Known, home: PodFile[], deleted: ReadonlySet<string> = new Set()): Relay[] {
  const onHome = new Map(home.map((f) => [f.name, f.size]));
  const out: Relay[] = [];
  for (const f of gauge) {
    if (f.open !== false || !isPodLog(f.name) || deleted.has(f.name)) continue;
    if (phone[f.name] !== f.size) continue;
    const there = onHome.get(f.name) ?? 0;
    if (there < f.size) out.push({ name: f.name, from: there, size: f.size });
  }
  return out.sort((a, b) => a.size - a.from - (b.size - b.from));
}
