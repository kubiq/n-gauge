import { router } from 'expo-router';
import { useEffect, useState } from 'react';
import { Text, TextInput } from 'react-native';

import { Button, Card, Note, Screen, useUi } from '@/components/ui';
import { usePod } from '@/hooks/use-pod';
import { t, tn } from '@/i18n';
import { loadBackend, saveBackend, testBackend } from '@/lib/backend';
import { useColors } from '@/theme';

// Where uploads go: backend/ in this repo, on the home server, reached
// through the phone's VPN.
export default function ServerScreen() {
  const C = useColors();
  const ui = useUi();
  const pod = usePod();
  const [url, setUrl] = useState('');
  const [token, setToken] = useState('');
  const [msg, setMsg] = useState<{ text: string; bad?: boolean } | null>(null);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    loadBackend().then((c) => {
      if (c) {
        setUrl(c.url);
        setToken(c.token);
      }
    });
  }, []);

  const valid = /^https?:\/\/[^\s/]+/.test(url.trim()) && token.trim().length > 0;

  const test = async () => {
    setBusy(true);
    setMsg(null);
    try {
      const n = await testBackend({ url: url.trim().replace(/\/+$/, ''), token: token.trim() });
      setMsg({ text: tn(n, 'Works - the server holds 1 file.', 'Works - the server holds {n} files.') });
    } catch (e) {
      setMsg({ text: (e as Error).message, bad: true });
    } finally {
      setBusy(false);
    }
  };

  const save = async () => {
    await saveBackend({ url, token });
    router.back();
  };

  return (
    <Screen>
      <Card title={t('Server')}>
        <Text style={ui.rowLabel}>{t('Address')}</Text>
        <TextInput
          style={ui.input}
          value={url}
          onChangeText={setUrl}
          placeholder="http://10.8.0.1:8080"
          placeholderTextColor={C.dim}
          autoCapitalize="none"
          autoCorrect={false}
          keyboardType="url"
        />
        <Text style={ui.rowLabel}>{t('Token')}</Text>
        <TextInput
          style={ui.input}
          value={token}
          onChangeText={setToken}
          placeholder={t('TOKEN from the server\'s .env')}
          placeholderTextColor={C.dim}
          autoCapitalize="none"
          autoCorrect={false}
          secureTextEntry
        />
        <Note>{t('The server is backend/ in the repo. The phone reaches it over mobile data, so the VPN must be up.')}</Note>
        {pod.state === 'connected' ? (
          <Note tone="bad">{t('Disconnect from the gauge to test: while connected, the app can only reach the gauge.')}</Note>
        ) : (
          <Button title={t('Test')} kind="plain" busy={busy} disabled={!valid} onPress={test} />
        )}
        {msg ? <Note tone={msg.bad ? 'bad' : undefined}>{msg.text}</Note> : null}
        <Button title={t('Save')} disabled={!valid} onPress={save} />
      </Card>
    </Screen>
  );
}
