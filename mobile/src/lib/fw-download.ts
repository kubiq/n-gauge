// The newest firmware from GitHub Releases, ready for the gauge (asked
// 2026-11-11): looked for while the app is open in front with internet -
// not on the gauge's WiFi, which has none - and at most once a day, kept
// across restarts; never from the background task, for the battery (asked
// so). Checked (fw-release.ts) against the gauge's own
// update key as last read from it (store.ts GaugeFw), and kept only until
// it is installed: Gauge settings offers it then, and drops it after.
// CI signs each release (.github/workflows/ci.yml); one this gauge would
// not take - another key, older, not whole - is never kept.

import { Directory, File, Paths } from 'expo-file-system';

import { checkRelease, isNewer, parseLatest } from './fw-release';
import { loadGaugeFw } from './store';

const REPO = 'kubiq/n-gauge';
const dir = new Directory(Paths.cache, 'firmware');
const readyFile = new File(Paths.document, 'fw-ready.json');
// The whole look at most this long - it must not still be going when the
// phone joins the gauge.
const LOOK_MAX_MS = 30000;
// At most once a day; when it last looked, in a file so a restart of the
// app does not look again.
const EVERY_MS = 24 * 3600 * 1000;
const lookedFile = new File(Paths.document, 'fw-looked.txt');

/** A release downloaded and checked, waiting to go on the gauge. */
export type ReadyFw = { tag: string; hash: string; built: number; size: number; uri: string };

export function loadReadyFw(): ReadyFw | null {
  try {
    if (!readyFile.exists) return null;
    const r = JSON.parse(readyFile.textSync()) as ReadyFw;
    return new File(r.uri).exists ? r : null;
  } catch {
    return null;
  }
}

/** Installed, or no longer wanted: the file and the note go. */
export function clearReadyFw(): void {
  try {
    if (dir.exists) dir.delete();
    if (readyFile.exists) readyFile.delete();
  } catch (e) {
    console.warn('firmware cache', e);
  }
}

async function bytesOf(url: string, signal: AbortSignal): Promise<Uint8Array> {
  const r = await fetch(url, { signal });
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return new Uint8Array(await r.arrayBuffer());
}

/**
 * Looks for a newer release and keeps it if it checks out - at most
 * LOOK_MAX_MS, at most once in EVERY_MS. Does nothing without a gauge read
 * since 2026-11-11 (no key to check by). Returns what happened, in
 * English, for the console.
 */
export async function lookForFirmware(): Promise<string> {
  const gauge = loadGaugeFw();
  if (!gauge?.pub) return 'the gauge has not said its update key yet';
  let looked = 0;
  try {
    looked = lookedFile.exists ? Number(lookedFile.textSync()) || 0 : 0;
  } catch {
    // unreadable: look
  }
  if (Date.now() - looked < EVERY_MS) return 'looked today already';
  const abort = new AbortController();
  const timer = setTimeout(() => abort.abort(), LOOK_MAX_MS);
  try {
    const said = await look(gauge.pub, gauge, abort.signal);
    lookedFile.write(String(Date.now()));   // only a look that got through: offline, the next opening tries
    return said;
  } finally {
    clearTimeout(timer);
  }
}

async function look(pub: string, gauge: { hash?: string; built?: number }, signal: AbortSignal): Promise<string> {
  const r = await fetch(`https://api.github.com/repos/${REPO}/releases/latest`, {
    headers: { Accept: 'application/vnd.github+json' },
    signal,
  });
  if (r.status === 404) return 'no release';
  if (!r.ok) throw new Error(`GitHub: HTTP ${r.status}`);
  const rel = (await r.json()) as { tag_name: string; assets: { name: string; browser_download_url: string }[] };
  const url = (name: string) => rel.assets.find((a) => a.name === name)?.browser_download_url;
  const [binUrl, sigUrl, latestUrl] = [url('car_gauge_v1.bin'), url('car_gauge_v1.bin.sig'), url('latest')];
  if (!binUrl || !sigUrl || !latestUrl) return `${rel.tag_name} is not signed`;

  const latest = parseLatest(new TextDecoder().decode(await bytesOf(latestUrl, signal)));
  if (!latest) return `${rel.tag_name}: no latest`;
  const ready = loadReadyFw();
  if (!isNewer(latest, gauge)) {
    if (ready) clearReadyFw();          // the gauge has it, or newer
    return `${rel.tag_name} is not newer`;
  }
  if (ready?.hash === latest.hash) return `${rel.tag_name} ready already`;

  const [bin, sig] = [await bytesOf(binUrl, signal), await bytesOf(sigUrl, signal)];
  const why = checkRelease(bin, sig, latest, pub);
  if (why) return `${rel.tag_name}: ${why}`;
  clearReadyFw();
  dir.create({ intermediates: true, idempotent: true });
  const f = new File(dir, `car_gauge_v1-${latest.hash.slice(0, 8)}.bin`);
  f.write(bin);
  readyFile.write(JSON.stringify({ tag: rel.tag_name, ...latest, uri: f.uri } satisfies ReadyFw));
  return `${rel.tag_name} downloaded and checked`;
}
