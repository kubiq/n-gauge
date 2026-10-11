// What to pull from the pod. Pure, so it can be tested without a pod.
//
// Files are compared by {name, size} only. The logger appends - and a
// numbered drive log is renamed to its dated name once the clock is known (a
// new name, so a new file here) - so a file that grew only needs its tail,
// fetched with a Range request. One exception: a GPX's closing tags are
// rewritten at every batch; pull() in store.ts continues over them.

export type PodFile = { name: string; size: number };

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

// What the pod itself writes (logger.cpp, laps.cpp, power.cpp's sleep log).
// Anything else on the card - a firmware .bin copied there, notes - is the
// owner's, not a log, and is not synced.
const POD_LOG = /^(drive_.+\.csv|trip_.+\.gpx|track_.+\.vbo|sleeplog\.txt)$/;

export function isPodLog(name: string): boolean {
  return POD_LOG.test(name);
}

export function planPull(pod: PodFile[], known: Known): Pull[] {
  const out: Pull[] = [];
  for (const f of pod) {
    if (!isPodLog(f.name)) continue;
    const have = known[f.name] ?? 0;
    if (have === f.size) continue;
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
