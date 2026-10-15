// A log's text, whether the pod wrote it plain or encrypted (a ".nge" name).
// One place for this, so the trips and sharing read the same way.

import { t } from '../i18n/index.ts';
import { bytesToText, isEncrypted, openLog } from './logcrypt';
import { loadLogKey } from './logkey';
import { inboxFile } from './store';
import { isEncryptedName } from './trips';

export class LogKeyError extends Error {}

export async function readLogText(name: string): Promise<string> {
  const f = inboxFile(name);
  if (!isEncryptedName(name)) return f.text();
  const key = await loadLogKey();
  if (!key) throw new LogKeyError(t('Encrypted - this phone has no log key (Settings, Log encryption).'));
  const bytes = await f.bytes();
  if (!isEncrypted(bytes)) throw new LogKeyError(t('Not an encrypted log, despite its name.'));
  const { plain } = openLog(bytes, key.secret);
  // A file whose first record does not open was written to another key: an
  // empty result would read as an empty drive, so say what it is.
  if (plain.length === 0 && bytes.length > 64)
    throw new LogKeyError(t('Encrypted to a different key than this phone’s - import that key to read it.'));
  return bytesToText(plain);
}
