import { useEffect, useState } from 'react';
import { Alert, StyleSheet, TextInput } from 'react-native';

import { Button, Card, Note, Row, Screen } from '@/components/ui';
import { usePod } from '@/hooks/use-pod';
import { keyFromText, toHex } from '@/lib/logcrypt';
import { copyLogKey, createLogKey, importLogKey, loadLogKey, type LogKey } from '@/lib/logkey';
import { getSettings, setPodLogKey } from '@/lib/pod';
import { C, S } from '@/theme';

// Log encryption: this phone's key (create, export to KeePass, import), and
// - while connected - telling the pod to encrypt to it. Design in DECISIONS.md.
export default function LogKeyScreen() {
  const pod = usePod();
  const connected = pod.state === 'connected';
  const [key, setKey] = useState<LogKey | null | undefined>(undefined);
  // The pod's key fingerprint: undefined = not known (yet), '' = plain logs.
  const [podFp, setPodFp] = useState<string | undefined>(undefined);
  const [importing, setImporting] = useState(false);
  const [importText, setImportText] = useState('');
  const [busy, setBusy] = useState<string | null>(null);

  useEffect(() => {
    loadLogKey().then(setKey);
  }, []);

  const refreshPod = () =>
    getSettings().then(
      (s) => setPodFp(s.logKey ?? undefined),
      () => setPodFp(undefined),
    );
  useEffect(() => {
    if (connected) refreshPod();
  }, [connected]);

  const mine = key ? key.fingerprint.replace(/ /g, '') : null;
  const podUsesMine = !!mine && podFp === mine;

  const create = async () => {
    const k = await createLogKey();
    setKey(k);
    Alert.alert(
      'Save the key in KeePass now',
      'Logs encrypted to this key can only ever be read with it. If the phone is lost and the key is not saved elsewhere, they are lost too.',
      [
        { text: 'Later', style: 'cancel' },
        { text: 'Copy it', onPress: () => copy(k) },
      ],
    );
  };

  const copy = async (k: LogKey) => {
    await copyLogKey(k);
    Alert.alert('Copied', 'Paste it into KeePass now. The clipboard is cleared in a minute.');
  };

  const doImport = async () => {
    const secret = keyFromText(importText);
    if (!secret) {
      Alert.alert('Not a key', 'Paste the line starting "ngauge-log-key-v1:" from KeePass.');
      return;
    }
    const apply = async () => {
      setKey(await importLogKey(secret));
      setImporting(false);
      setImportText('');
    };
    if (key && toHex(key.secret) !== toHex(secret)) {
      Alert.alert(
        'Replace this phone’s key?',
        'Logs encrypted to the current key will no longer open on this phone - only with that key. Make sure it is saved in KeePass first.',
        [
          { text: 'Cancel', style: 'cancel' },
          { text: 'Replace', style: 'destructive', onPress: apply },
        ],
      );
    } else {
      await apply();
    }
  };

  const sendToPod = async (value: string | 'off') => {
    setBusy(value === 'off' ? 'off' : 'on');
    try {
      await setPodLogKey(value);
      await refreshPod();
    } catch (e) {
      Alert.alert('Pod', String((e as Error).message));
    } finally {
      setBusy(null);
    }
  };

  if (key === undefined) return null;

  return (
    <Screen underHeader>
      <Card title="This phone’s key">
        {key ? (
          <>
            <Row label="Fingerprint" value={key.fingerprint} />
            <Note>The pod’s settings page shows the same fingerprint when it encrypts to this key.</Note>
            <Button title="Copy the key for KeePass" kind="plain" onPress={() => copy(key)} />
          </>
        ) : (
          <>
            <Note>
              With a key, the pod encrypts its logs so that only this phone can read them - a card taken out of the pod
              gives nothing away. The pod only ever gets the public half.
            </Note>
            <Button title="Create a key" onPress={create} />
          </>
        )}
        {importing ? (
          <>
            <TextInput
              style={styles.input}
              value={importText}
              onChangeText={setImportText}
              placeholder="ngauge-log-key-v1:…"
              placeholderTextColor={C.dim}
              autoCapitalize="none"
              autoCorrect={false}
              secureTextEntry
            />
            <Button title="Import" onPress={doImport} />
            <Button title="Cancel" kind="plain" onPress={() => setImporting(false)} />
          </>
        ) : (
          <Button
            title={key ? 'Replace with a key from KeePass' : 'Import a key from KeePass'}
            kind="plain"
            onPress={() => setImporting(true)}
          />
        )}
      </Card>

      <Card title="The pod">
        {!connected ? (
          <Note>Connect to the pod (Pod screen) to set its key.</Note>
        ) : podFp === undefined ? (
          <Note>Reading the pod’s settings… (older firmware does not know about log keys)</Note>
        ) : (
          <>
            <Row
              label="Logs"
              value={podFp === '' ? 'not encrypted' : podUsesMine ? 'encrypted to this phone' : 'encrypted to another key'}
              tone={podFp === '' ? 'dim' : podUsesMine ? 'good' : 'bad'}
            />
            {podFp && !podUsesMine ? (
              <Note tone="bad">
                The pod encrypts to {podFp.replace(/(.{4})(?=.)/g, '$1 ')} - not this phone’s key. Its logs only open with
                that key.
              </Note>
            ) : null}
            {key && !podUsesMine ? (
              <Button title="Encrypt the pod’s logs with this key" busy={busy === 'on'} onPress={() => sendToPod(toHex(key.pub))} />
            ) : null}
            {podFp ? (
              <Button
                title="Stop encrypting"
                kind="danger"
                busy={busy === 'off'}
                onPress={() =>
                  Alert.alert('Stop encrypting?', 'New logs will be plain again. Logs already encrypted stay encrypted.', [
                    { text: 'Cancel', style: 'cancel' },
                    { text: 'Stop', style: 'destructive', onPress: () => sendToPod('off') },
                  ])
                }
              />
            ) : null}
            <Note>A change applies at once: the pod closes its open logs and starts new files.</Note>
          </>
        )}
      </Card>
    </Screen>
  );
}

const styles = StyleSheet.create({
  input: {
    color: C.text,
    borderColor: C.border,
    borderWidth: 1,
    borderRadius: 8,
    paddingHorizontal: S.md,
    paddingVertical: 10,
    fontSize: 15,
  },
});
