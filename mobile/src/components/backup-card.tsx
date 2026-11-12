import { Directory, File, Paths } from 'expo-file-system';
import * as Sharing from 'expo-sharing';
import { useState } from 'react';
import { Alert } from 'react-native';

import { Button, Card, Note } from '@/components/ui';
import { t } from '@/i18n';
import { BACKUP_FILES, type BackupFiles, makeBackup, readBackup } from '@/lib/backup-format';
import { shortWhen } from '@/lib/format';

// The app's own data to a JSON file and back (backup-format.ts): what an
// uninstall loses for good - your places and drive names, the fuel picks,
// the settings. Asked 2026-11-12, before a reinstall for the app's new
// signing key. Saved through the share sheet (to the Syncthing folder, say);
// restored from a file picked.
export function BackupCard() {
  const [busy, setBusy] = useState<'save' | 'restore' | null>(null);

  const save = async () => {
    setBusy('save');
    try {
      const files: BackupFiles = {};
      for (const name of BACKUP_FILES) {
        const f = new File(Paths.document, name);
        files[name] = f.exists ? f.textSync() : null;
      }
      const dir = new Directory(Paths.cache, 'backup');
      dir.create({ intermediates: true, idempotent: true });
      const day = new Date().toISOString().slice(0, 10);
      const out = new File(dir, `n-gauge-backup-${day}.json`);
      out.write(makeBackup(files, Date.now()));
      await Sharing.shareAsync(out.uri, { mimeType: 'application/json', dialogTitle: t('Save the backup') });
    } catch (e) {
      Alert.alert(t('Back up'), String((e as Error).message));
    } finally {
      setBusy(null);
    }
  };

  const restore = async () => {
    const r = await File.pickFileAsync({ mimeTypes: ['application/json', 'application/octet-stream', '*/*'] });
    if (r.canceled) return;
    const b = readBackup(await r.result.text());
    if (typeof b === 'string') {
      Alert.alert(t('Restore'), b);
      return;
    }
    Alert.alert(
      t('Restore the backup?'),
      t('From {when}. Your places, drive names, fuel picks and settings here are replaced by it.', {
        when: b.at ? shortWhen(b.at) : '?',
      }),
      [
        { text: t('Cancel'), style: 'cancel' },
        {
          text: t('Restore'),
          onPress: () => {
            setBusy('restore');
            try {
              for (const [name, content] of Object.entries(b.files)) {
                const f = new File(Paths.document, name);
                if (content === null) {
                  if (f.exists) f.delete();
                } else f.write(content);
              }
              Alert.alert(t('Restored'), t('Close the app and open it again, so every screen reads it.'));
            } catch (e) {
              Alert.alert(t('Restore'), String((e as Error).message));
            } finally {
              setBusy(null);
            }
          },
        },
      ],
    );
  };

  return (
    <Card title={t('Backup')}>
      <Note>
        {t(
          'Your places, drive names, fuel picks and settings, as one file - what reinstalling the app would lose. The drives come back by themselves, and the log key is in your password manager.',
        )}
      </Note>
      <Button title={t('Save a backup')} kind="plain" busy={busy === 'save'} onPress={save} />
      <Button title={t('Restore from a backup')} kind="plain" busy={busy === 'restore'} onPress={restore} />
    </Card>
  );
}
