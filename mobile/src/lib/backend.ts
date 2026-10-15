// Client for the log store (backend/server.js). Reached over mobile data,
// usually through the phone's VPN - so only while the app is NOT bound to
// the pod's network (PodWifi.disconnect() first).

import { Directory, File, Paths, UploadType } from 'expo-file-system';
import * as SecureStore from 'expo-secure-store';
import { gzipSync } from 'fflate';

import { t } from '../i18n/index.ts';

export type BackendConfig = { url: string; token: string };

const KEY = 'backend';

export async function loadBackend(): Promise<BackendConfig | null> {
  const s = await SecureStore.getItemAsync(KEY);
  return s ? JSON.parse(s) : null;
}

export async function saveBackend(c: BackendConfig): Promise<void> {
  await SecureStore.setItemAsync(KEY, JSON.stringify({ url: c.url.trim().replace(/\/+$/, ''), token: c.token.trim() }));
}

export class BackendError extends Error {
  constructor(
    message: string,
    readonly status?: number,
    /** On a 409: the server's real size of the file. */
    readonly size?: number,
  ) {
    super(message);
  }
}

async function call(c: BackendConfig, path: string, auth = true, timeoutMs = 15000): Promise<Response> {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeoutMs);
  try {
    const r = await fetch(c.url + path, {
      headers: auth ? { Authorization: `Bearer ${c.token}` } : {},
      signal: ctl.signal,
    });
    if (r.status === 401) throw new BackendError(t('The server refused the token'), 401);
    if (!r.ok) throw new BackendError(`HTTP ${r.status}`, r.status);
    return r;
  } catch (e) {
    if (e instanceof BackendError) throw e;
    throw new BackendError(
      ctl.signal.aborted
        ? t('The server did not answer - is the VPN up?')
        : t('Cannot reach the server: {error}', { error: String(e) }),
    );
  } finally {
    clearTimeout(timer);
  }
}

/** Reachable, and the token accepted. Returns how many files it holds. */
export async function testBackend(c: BackendConfig): Promise<number> {
  await call(c, '/api/health', false);
  return (await listServer(c)).length;
}

/** What the server holds: name -> bytes. */
export async function listServer(c: BackendConfig): Promise<Record<string, number>> {
  const files: { name: string; size: number }[] = await (await call(c, '/api/files')).json();
  return Object.fromEntries(files.map((f) => [f.name, f.size]));
}

const tmpDir = new Directory(Paths.cache, 'uploads');

/**
 * Sends raw bytes to be stored at `offset`, gzipped (logs shrink 5-10x, and
 * this is mobile data). Through a file and the native uploader rather than
 * a fetch body, which React Native would copy through base64. Returns the
 * server's new size; a 409 throws with the server's real size.
 */
export async function putChunk(c: BackendConfig, name: string, offset: number, raw: Uint8Array): Promise<number> {
  if (!tmpDir.exists) tmpDir.create({ intermediates: true });
  const tmp = new File(tmpDir, `${Date.now()}.gz`);
  tmp.write(gzipSync(raw, { level: 6 }));
  try {
    const r = await tmp.upload(`${c.url}/api/files/${encodeURIComponent(name)}?offset=${offset}`, {
      httpMethod: 'PUT',
      uploadType: UploadType.BINARY_CONTENT,
      headers: {
        Authorization: `Bearer ${c.token}`,
        'Content-Encoding': 'gzip',
        'Content-Type': 'application/octet-stream',
      },
    });
    let body: { size?: number; error?: string } = {};
    try {
      body = JSON.parse(r.body);
    } catch {}
    if (r.status === 200 && typeof body.size === 'number') return body.size;
    if (r.status === 409) throw new BackendError('Server size changed', 409, body.size);
    if (r.status === 401) throw new BackendError(t('The server refused the token'), 401);
    throw new BackendError(body.error ?? `HTTP ${r.status}`, r.status);
  } catch (e) {
    if (e instanceof BackendError) throw e;
    throw new BackendError(t('Upload failed - is the VPN up? ({error})', { error: String(e) }));
  } finally {
    if (tmp.exists) tmp.delete();
  }
}
