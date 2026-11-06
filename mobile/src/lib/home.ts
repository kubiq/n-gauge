// The home server (server/upload_server.py): the gauge uploads its logs
// there as the car starts, over the phone's hotspot and a WireGuard VPN
// (upload.h on the gauge). The phone pulls them from there too, so the
// drives are on it almost all the time - without a trip to the gauge's
// WiFi. The same files by the same names as the gauge's own sync, into the
// same inbox, and as encrypted: the server never had the key.
//
// Reached only where the server is: at home on WiFi, or with the phone's own
// VPN up (gauge.necum.eu points to a LAN address). Anywhere else the check
// in reachable() fails quietly.

import * as SecureStore from 'expo-secure-store';

import { t } from '../i18n/index.ts';
import { type Downloader, downloadUrlRange } from './pod';
import type { PodFile } from './sync-plan';

/** The server's address ("http://gauge.example.org") and its token. */
export type HomeServer = { url: string; token: string };

const KEY = 'homeServer';

export async function loadHome(): Promise<HomeServer | null> {
  const s = await SecureStore.getItemAsync(KEY);
  if (!s) return null;
  const h = JSON.parse(s) as HomeServer;
  return h.url ? h : null;
}

/** "gauge.example.org" is taken as http:// - the server has no TLS of its own. */
export function cleanUrl(url: string): string {
  let u = url.trim().replace(/\/+$/, '');
  if (u && !/^[a-z]+:\/\//i.test(u)) u = `http://${u}`;
  return u;
}

export async function saveHome(h: HomeServer | null): Promise<void> {
  if (!h || !h.url.trim()) await SecureStore.deleteItemAsync(KEY);
  else await SecureStore.setItemAsync(KEY, JSON.stringify({ url: cleanUrl(h.url), token: h.token.trim() }));
}

function auth(h: HomeServer): Record<string, string> {
  return h.token ? { Authorization: `Bearer ${h.token}` } : {};
}

async function call(h: HomeServer, path: string, init: RequestInit = {}, timeoutMs = 10000): Promise<Response> {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeoutMs);
  try {
    const r = await fetch(h.url + path, { ...init, headers: { ...auth(h), ...(init.headers ?? {}) }, signal: ctl.signal });
    if (r.status === 401) throw new Error(t('the home server refused the token'));
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    return r;
  } catch (e) {
    if (ctl.signal.aborted) throw new Error(t('the home server did not answer'));
    throw e;
  } finally {
    clearTimeout(timer);
  }
}

/** Is it there at all - at home, or with the VPN up? A quick look, no token. */
export async function reachable(h: HomeServer): Promise<boolean> {
  try {
    return (await (await call(h, '/health', {}, 3000)).text()).trim() === 'ok';
  } catch {
    return false;
  }
}

/** What the server holds, as the gauge's listing is: name and size. */
export async function listHome(h: HomeServer): Promise<PodFile[]> {
  const text = await (await call(h, '/files')).text();
  return text
    .split('\n')
    .map((l) => l.trim().split(' '))
    .filter((p) => p.length === 2 && /^\d+$/.test(p[1]))
    .map(([name, size]) => ({ name, size: Number(size) }));
}

/** For store.ts's pull(): the server's /f/<name>, with Range for a log that grew. */
export function homeDownloader(h: HomeServer): Downloader {
  return (name, from, to, dest, onProgress, signal) =>
    downloadUrlRange(`${h.url}/f/${encodeURIComponent(name)}`, auth(h), from, to, dest, onProgress, signal);
}

/** A drive deleted in the app goes from the server too - and stays gone (its tombstone). */
export async function deleteOnHome(h: HomeServer, name: string): Promise<boolean> {
  try {
    await call(h, `/f/${encodeURIComponent(name)}`, { method: 'DELETE' });
    return true;
  } catch {
    return false;
  }
}
