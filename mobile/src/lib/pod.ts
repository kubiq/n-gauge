// HTTP client for the pod's own AP (firmware/car_gauge_v1/webapi.cpp).
//
// Only valid while PodWifi has bound the app to the pod's network - otherwise
// 4.3.2.1 is routed out over mobile data and every request just times out.

import { File, UploadType } from 'expo-file-system';

import type { PodSettings } from './settings-schema';
import type { PodFile } from './sync-plan';

// WIFI_AP_IP in config.h. Deliberately outside RFC1918, so Android sends its
// connectivity probe there (see the note in config.h).
export const POD_BASE = 'http://4.3.2.1';

export class PodError extends Error {
  constructor(
    message: string,
    readonly status?: number,
  ) {
    super(message);
  }
}

async function call(path: string, init: RequestInit = {}, timeoutMs = 5000): Promise<Response> {
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), timeoutMs);
  try {
    const r = await fetch(POD_BASE + path, { ...init, signal: ctl.signal });
    if (!r.ok) throw new PodError((await r.text()) || `HTTP ${r.status}`, r.status);
    return r;
  } catch (e) {
    if (e instanceof PodError) throw e;
    throw new PodError(ctl.signal.aborted ? 'The gauge did not answer' : String(e));
  } finally {
    clearTimeout(t);
  }
}

function postForm(path: string, body: string, timeoutMs?: number): Promise<Response> {
  return call(
    path,
    { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body },
    timeoutMs,
  );
}

export type PodLive = {
  can: {
    linkUp: boolean;
    haveOil: boolean;
    oilTempC: number;
    haveCoolant: boolean;
    coolantC: number;
    haveBoost: boolean;
    boostBar: number;
  };
  iso: { ok: number; aborts: number; rxOverflows: number; busOff: boolean; busOffs: number; recoveries: number; errPassive: number };
  gps: { linkUp: boolean; haveFix: boolean; sats: number; speedKmh: number; hdop: number };
  clock: { valid: boolean; setFromGps: boolean; utc: string; local: string; tz: string };
  rtc: { present: boolean; valid: boolean; seeded: boolean; driftSec: number };
  /** The running build - firmware from 2026-10-13 on; older answers lack it. */
  fw?: { build: string; hash: string };
  /** The gauge's own resources (bytes; SD in KB). Firmware from 2026-10-13 on. */
  sys?: {
    heap: number;
    heapMin: number;
    heapTotal: number;
    psram: number;
    psramTotal: number;
    sdUsedKb: number;
    sdTotalKb: number;
    app: number;
    appMax: number;
    tempC: number;
    reset: string;
  };
  uptimeMs: number;
};

export async function getLive(): Promise<PodLive> {
  return (await call('/api/live')).json();
}

export async function getSettings(): Promise<PodSettings> {
  return (await call('/api/settings')).json();
}

/** `body` from saveForm(). Returns the pod's message ("Saved." etc). */
export async function saveSettings(body: string): Promise<string> {
  return (await postForm('/save', body)).text();
}

/**
 * The public key the pod encrypts its logs to (64 hex digits), or 'off'.
 * The pod closes its open logs and starts the next ones under the new key.
 */
export async function setPodLogKey(pubHex: string | 'off'): Promise<string> {
  return saveSettings(`logPub=${encodeURIComponent(pubHex)}`);
}

/** Sets the pod's clock from the phone's. Always a UTC epoch - see handleClockSet(). */
export async function setClock(): Promise<string> {
  return (await postForm('/api/clock', `epoch=${Math.floor(Date.now() / 1000)}`)).text();
}

/** Re-zeroes the g-meter from wherever the pod is sitting right now. */
export async function zeroGmeter(): Promise<string> {
  return saveSettings('gmZero=1');
}

/**
 * Deletes a drive log from the card (POST /api/delete, firmware from
 * 2026-10-14). 'done' when deleted or already gone (410); 'later' when the
 * gauge cannot now - the drive is still being written (409), the card is
 * lent to a PC (503) - or cannot at all: older firmware answers an unknown
 * path with its portal page, a 200 that is not "deleted".
 */
export async function deletePodFile(name: string): Promise<'done' | 'later'> {
  try {
    const r = await postForm('/api/delete', `f=${encodeURIComponent(name)}`);
    return (await r.text()) === 'deleted' ? 'done' : 'later';
  } catch (e) {
    if (e instanceof PodError && e.status === 410) return 'done';
    if (e instanceof PodError && e.status !== undefined) return 'later';
    throw e;   // no answer at all: the connection, not the file
  }
}

/**
 * The card's files, all pages. Needs firmware with `GET /api/files`
 * (added 2026-10-11); older firmware answers 404.
 */
export async function listFiles(): Promise<PodFile[]> {
  const out: PodFile[] = [];
  for (let page = 0; page < 100; page++) {
    const r: { files: PodFile[]; pages: number } = await (await call(`/api/files?p=${page}`, {}, 15000)).json();
    out.push(...r.files);
    if (page + 1 >= r.pages) break;
  }
  return out;
}

/**
 * Bytes [from, to) of a card file, into `dest` (replaced). Asks with an
 * explicit end so the two answers can be told apart by size alone: exactly
 * to - from means the Range was honoured, anything larger means the pod sent
 * the whole file (a firmware without Range support) and the caller must skip
 * the first `from` bytes. Returns which it was.
 */
export async function downloadRange(
  name: string,
  from: number,
  to: number,
  dest: File,
  onProgress?: (bytes: number) => void,
  signal?: AbortSignal,
): Promise<{ whole: boolean }> {
  if (dest.exists) dest.delete();
  // A task rather than downloadFileAsync: only the task takes an AbortSignal,
  // and Cancel has to stop a 17 MB GPX mid-file, not after it.
  const task = File.createDownloadTask(`${POD_BASE}/dl?f=${encodeURIComponent(name)}`, dest, {
    headers: from > 0 ? { Range: `bytes=${from}-${to - 1}` } : {},
    onProgress: onProgress ? (p) => onProgress(p.bytesWritten) : undefined,
    signal,
  });
  // Raced against the signal ourselves: on Android the native promise was
  // never settled after a cancel, so Cancel stopped the transfer and then
  // left the sync waiting forever. The caller gives each pull its own `dest`,
  // so a native transfer still winding down cannot write into the next one.
  await new Promise<void>((resolve, reject) => {
    const aborted = () => reject(new PodError('Cancelled'));
    if (signal?.aborted) return aborted();
    signal?.addEventListener('abort', aborted, { once: true });
    task.downloadAsync().then(
      () => resolve(),
      (e) => reject(e),
    );
  });
  return { whole: from > 0 && dest.size > to - from };
}

/**
 * Firmware upload, as the device page's form does it (multipart, field
 * "fw"). The pod restarts into the new build ~0.5 s after answering, and
 * rolls back by itself if it fails to confirm.
 */
export async function uploadFirmware(bin: File): Promise<string> {
  const r = await bin.upload(`${POD_BASE}/update`, {
    httpMethod: 'POST',
    uploadType: UploadType.MULTIPART,
    fieldName: 'fw',
    mimeType: 'application/octet-stream',
  });
  if (r.status !== 200) throw new PodError(r.body || `HTTP ${r.status}`, r.status);
  return r.body;
}
