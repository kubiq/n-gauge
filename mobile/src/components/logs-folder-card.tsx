import { useState } from 'react';
import { Alert } from 'react-native';

import { Button, Card, Note, Row } from '@/components/ui';
import { folderName, pickLogsFolder } from '@/lib/share';
import { loadLogsFolder, saveLogsFolder } from '@/lib/store';

// Where "Save to a folder" (a drive's ⋮) and "Save all to a folder" (Trips'
// ⋮) write: picked the first time and remembered, changed here.
export function LogsFolderCard() {
  const [uri, setUri] = useState(loadLogsFolder);

  const change = async () => {
    try {
      setUri((await pickLogsFolder()).uri);
    } catch (e) {
      // Backing out of the picker keeps the old folder.
      if (!/cancel/i.test(String((e as Error).message))) Alert.alert('Folder', String((e as Error).message));
    }
  };

  const forget = () => {
    saveLogsFolder(null);
    setUri(null);
  };

  return (
    <Card title="Logs folder">
      <Row label="Save to" value={uri ? folderName(uri) : 'ask every time'} tone={uri ? undefined : 'dim'} />
      <Note>Saved logs are decrypted, under their plain names; files already there are skipped.</Note>
      <Button title={uri ? 'Change' : 'Choose a folder'} kind="plain" onPress={change} />
      {uri ? <Button title="Ask every time" kind="plain" onPress={forget} /> : null}
    </Card>
  );
}
