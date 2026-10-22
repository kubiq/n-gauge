// The gauge's beacon: parked, its WiFi's beacons name the last drive
// (beaconOf - a fingerprint only a phone with the log key can match), and the
// phone reads them from its own WiFi scans without joining
// (modules/pod-wifi, PodBeacon.kt). Asked 2026-10-22: hear of drives not yet
// on the phone without opening the app.
//
// The native side checks every 15 minutes in the background and posts a
// note when the beacon names a drive the phone does not have. Which drives
// those are is said here: every session the phone has had - synced, or
// deleted since - kept in a file of its own, so a drive deleted from both
// the phone and the gauge is still known if the gauge names it.

import { File, Paths } from 'expo-file-system';
import { PermissionsAndroid, Platform } from 'react-native';

import PodWifi from '@modules/pod-wifi';
import { t } from '../i18n/index.ts';
import { beaconOf } from './logcrypt';
import { loadLogKey } from './logkey';
import { loadDeleted, loadInbox } from './store';
import { sessionOf } from './trips';

type State = {
  /** Notify of new drives on the gauge. */
  notify: boolean;
  /** Connect and sync when the app is opened and the gauge has a new one. */
  autoSync: boolean;
  /** Every session the phone has had. */
  sessions: string[];
};

const file = new File(Paths.document, 'beacon.json');

function load(): State {
  try {
    if (file.exists) return { notify: false, autoSync: false, sessions: [], ...JSON.parse(file.textSync()) };
  } catch {
    // A broken file: start again; the sessions come back from the inbox.
  }
  return { notify: false, autoSync: false, sessions: [] };
}

const save = (s: State) => file.write(JSON.stringify(s));

export const loadBeaconNotify = () => load().notify;
export const loadBeaconAutoSync = () => load().autoSync;

/**
 * The background check on or off. On asks for what it needs: location (scan
 * results are given only with it), location "all the time" (for a check
 * with the app closed - Android sends you to its settings for that one) and
 * notifications. False when location was refused: then it stays off.
 */
export async function setBeaconNotify(on: boolean): Promise<boolean> {
  if (on && Platform.OS === 'android') {
    const P = PermissionsAndroid.PERMISSIONS;
    const fine = await PermissionsAndroid.request(P.ACCESS_FINE_LOCATION);
    if (fine !== PermissionsAndroid.RESULTS.GRANTED) return false;
    await PermissionsAndroid.request(P.ACCESS_BACKGROUND_LOCATION);
    if (P.POST_NOTIFICATIONS) await PermissionsAndroid.request(P.POST_NOTIFICATIONS);
  }
  save({ ...load(), notify: on });
  await updateKnownBeacons();
  PodWifi.watchBeacon(on, t('New drives on the gauge'), t('Open the app to sync them.'));
  return true;
}

export function setBeaconAutoSync(on: boolean): void {
  save({ ...load(), autoSync: on });
}

/**
 * Tells the native check the fingerprints of every drive the phone has had,
 * under this phone's log key (zeros without one, as the gauge does). Call
 * when the inbox, the deleted list or the key changes.
 */
export async function updateKnownBeacons(): Promise<string[]> {
  const s = load();
  const names = [...Object.keys(loadInbox()), ...loadDeleted()];
  const all = new Set(s.sessions);
  for (const n of names) {
    const ses = sessionOf(n);
    if (ses) all.add(ses);
  }
  if (all.size !== s.sessions.length) save({ ...s, sessions: [...all] });
  const pub = (await loadLogKey())?.pub ?? null;
  const fps = [...all].map((ses) => beaconOf(pub, ses));
  PodWifi.setKnownBeacons(fps);
  return fps;
}

/**
 * The gauge is beaconing a drive the phone does not have, by the scan
 * results of the last two minutes: its fingerprint, else null.
 */
export async function newDriveBeacon(): Promise<string | null> {
  const fp = PodWifi.seenBeacon(2 * 60_000);
  if (!fp) return null;
  return (await updateKnownBeacons()).includes(fp) ? null : fp;
}
