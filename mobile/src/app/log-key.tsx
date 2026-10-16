import { useEffect, useState } from 'react';
import { Alert, TextInput } from 'react-native';

import { Button, Card, Note, Row, Screen, useUi } from '@/components/ui';
import { useHoldPod, usePod } from '@/hooks/use-pod';
import { t } from '@/i18n';
import { keyFromText, toHex } from '@/lib/logcrypt';
import { copyLogKey, createLogKey, importLogKey, loadLogKey, type LogKey } from '@/lib/logkey';
import { getSettings, setPodLogKey } from '@/lib/pod';
import { useColors } from '@/theme';

// Log encryption: this phone's key (create, export to KeePass, import), and
// - while connected - telling the pod to encrypt to it. Design in DECISIONS.md.
export default function LogKeyScreen() {
  const C = useColors();
  const ui = useUi();
  const pod = usePod();
  useHoldPod();   // telling the gauge the key needs it: no disconnect after syncing
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
      t('Save the key in KeePass now'),
      t(
        'Logs encrypted to this key can only ever be read with it. If the phone is lost and the key is not saved elsewhere, they are lost too.',
      ),
      [
        { text: t('Later'), style: 'cancel' },
        { text: t('Copy it'), onPress: () => copy(k) },
      ],
    );
  };

  const copy = async (k: LogKey) => {
    await copyLogKey(k);
    Alert.alert(t('Copied'), t('Paste it into KeePass now. The clipboard is cleared in a minute.'));
  };

  const doImport = async () => {
    const secret = keyFromText(importText);
    if (!secret) {
      Alert.alert(t('Not a key'), t('Paste the line starting "ngauge-log-key-v1:" from KeePass.'));
      return;
    }
    const apply = async () => {
      setKey(await importLogKey(secret));
      setImporting(false);
      setImportText('');
    };
    if (key && toHex(key.secret) !== toHex(secret)) {
      Alert.alert(
        t('Replace this phone’s key?'),
        t(
          'Logs encrypted to the current key will no longer open on this phone - only with that key. Make sure it is saved in KeePass first.',
        ),
        [
          { text: t('Cancel'), style: 'cancel' },
          { text: t('Replace'), style: 'destructive', onPress: apply },
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
      Alert.alert(t('Gauge'), String((e as Error).message));
    } finally {
      setBusy(null);
    }
  };

  if (key === undefined) return null;

  return (
    <Screen>
      <Card title={t('This phone’s key')}>
        {key ? (
          <>
            <Row label={t('Fingerprint')} value={key.fingerprint} />
            <Note>{t('The gauge’s settings page shows the same fingerprint when it encrypts to this key.')}</Note>
            <Button title={t('Copy the key for KeePass')} kind="plain" onPress={() => copy(key)} />
          </>
        ) : (
          <>
            <Note>
              {t(
                'With a key, the gauge encrypts its logs so that only this phone can read them - a card taken out of the gauge gives nothing away. The gauge only ever gets the public half.',
              )}
            </Note>
            <Button title={t('Create a key')} onPress={create} />
          </>
        )}
        {importing ? (
          <>
            <TextInput
              style={ui.input}
              value={importText}
              onChangeText={setImportText}
              placeholder="ngauge-log-key-v1:…"
              placeholderTextColor={C.dim}
              autoCapitalize="none"
              autoCorrect={false}
              secureTextEntry
            />
            <Button title={t('Import')} onPress={doImport} />
            <Button title={t('Cancel')} kind="plain" onPress={() => setImporting(false)} />
          </>
        ) : (
          <Button
            title={key ? t('Replace with a key from KeePass') : t('Import a key from KeePass')}
            kind="plain"
            onPress={() => setImporting(true)}
          />
        )}
      </Card>

      <Card title={t('The gauge')}>
        {!connected ? (
          <Note>{t('Connect to the gauge (tap it at the top) to set its key.')}</Note>
        ) : podFp === undefined ? (
          <Note>{t('Reading the gauge’s settings… (older firmware does not know about log keys)')}</Note>
        ) : (
          <>
            <Row
              label={t('Logs')}
              value={
                podFp === ''
                  ? t('not encrypted')
                  : podUsesMine
                    ? t('encrypted to this phone')
                    : t('encrypted to another key')
              }
              tone={podFp === '' ? 'dim' : podUsesMine ? 'good' : 'bad'}
            />
            {podFp && !podUsesMine ? (
              <Note tone="bad">
                {t('The gauge encrypts to {fp} - not this phone’s key. Its logs only open with that key.', {
                  fp: podFp.replace(/(.{4})(?=.)/g, '$1 '),
                })}
              </Note>
            ) : null}
            {key && !podUsesMine ? (
              <Button title={t('Encrypt the gauge’s logs with this key')} busy={busy === 'on'} onPress={() => sendToPod(toHex(key.pub))} />
            ) : null}
            {podFp ? (
              <Button
                title={t('Stop encrypting')}
                kind="danger"
                busy={busy === 'off'}
                onPress={() =>
                  Alert.alert(
                    t('Stop encrypting?'),
                    t('New logs will be plain again. Logs already encrypted stay encrypted.'),
                    [
                      { text: t('Cancel'), style: 'cancel' },
                      { text: t('Stop'), style: 'destructive', onPress: () => sendToPod('off') },
                    ],
                  )
                }
              />
            ) : null}
            <Note>{t('A change applies at once: the gauge closes its open logs and starts new files.')}</Note>
          </>
        )}
      </Card>
    </Screen>
  );
}
