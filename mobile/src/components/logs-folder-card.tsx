import { useState } from 'react';
import { Alert, StyleSheet, Switch, Text, View } from 'react-native';

import { Button, Card, Note, Row } from '@/components/ui';
import { t } from '@/i18n';
import { folderName, pickLogsFolder } from '@/lib/share';
import { loadAutoSave, loadLogsFolder, saveAutoSave, saveLogsFolder } from '@/lib/store';
import { S, themed, useColors } from '@/theme';

// Where "Save to a folder" (a drive's ⋮) and "Save all to a folder" (Trips'
// ⋮) write: picked the first time and remembered, changed here. And whether
// each sync saves what it brought there too (use-sync.tsx) - which needs a
// folder remembered, so switching it on without one asks for it first.
export function LogsFolderCard() {
  const [uri, setUri] = useState(loadLogsFolder);
  const [auto, setAuto] = useState(loadAutoSave);
  const C = useColors();
  const styles = useStyles();

  // The folder, from the picker; null if backed out of (the old one kept).
  const pick = async (): Promise<string | null> => {
    try {
      const picked = (await pickLogsFolder()).uri;
      setUri(picked);
      return picked;
    } catch (e) {
      if (!/cancel/i.test(String((e as Error).message))) Alert.alert(t('Folder'), String((e as Error).message));
      return null;
    }
  };

  const forget = () => {
    saveLogsFolder(null);
    setUri(null);
    // Saving after a sync has nowhere to go without one.
    saveAutoSave(false);
    setAuto(false);
  };

  const changeAuto = async (on: boolean) => {
    if (on && !uri && !(await pick())) return;
    saveAutoSave(on);
    setAuto(on);
  };

  return (
    <Card title={t('Logs folder')}>
      <Row label={t('Save to')} value={uri ? folderName(uri) : t('ask every time')} tone={uri ? undefined : 'dim'} />
      <Note>{t('Saved logs are decrypted, under their plain names; files already there are skipped.')}</Note>
      <Button title={uri ? t('Change') : t('Choose a folder')} kind="plain" onPress={pick} />
      {uri ? <Button title={t('Ask every time')} kind="plain" onPress={forget} /> : null}
      <View style={styles.row}>
        <Text style={styles.label}>{t('Save new logs after each sync')}</Text>
        <Switch value={auto} onValueChange={changeAuto} trackColor={{ true: C.accent }} />
      </View>
      <Note>{t('What a sync brings is saved into the folder by itself, decrypted - nothing to do on the phone.')}</Note>
    </Card>
  );
}

const useStyles = themed((C) =>
  StyleSheet.create({
    row: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: S.md },
    label: { color: C.text, fontSize: 15, flexShrink: 1 },
  }),
);
