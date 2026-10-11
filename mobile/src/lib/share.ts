// Handing a drive's track to other apps (Strava, gpx.studio, OsmAnd, a
// messenger) through Android's share sheet.

import { Directory, File, Paths } from 'expo-file-system';
import * as Sharing from 'expo-sharing';

import { cleanGpx } from './gpx-share';
import { readLogText } from './logtext';
import { plainName } from './trips';

const dir = new Directory(Paths.cache, 'share');

/**
 * Shares a cleaned copy of the inbox GPX, not the inbox file itself: the
 * receiving app gets valid XML however the copy was pulled (gpx-share.ts),
 * and the inbox - the sync's mirror of the pod - is never handed out. An
 * encrypted track is shared decrypted, under its plain name: cleanGpx() adds
 * the closing tags the encrypted writer leaves out.
 */
export async function shareGpx(name: string): Promise<void> {
  if (!(await Sharing.isAvailableAsync())) throw new Error('Sharing is not available on this device');
  if (!dir.exists) dir.create({ intermediates: true });
  for (const old of dir.list()) old.delete();
  const out = new File(dir, plainName(name));
  out.write(cleanGpx(await readLogText(name)));
  await Sharing.shareAsync(out.uri, { mimeType: 'application/gpx+xml', dialogTitle: 'Share the track' });
}
