import { useEffect, useState } from 'react';
import { StyleSheet, Text, TextInput, View } from 'react-native';

import { Button, Card, Note, Row, useUi } from '@/components/ui';
import { useSync } from '@/hooks/use-sync';
import { t, tn } from '@/i18n';
import { shortWhen } from '@/lib/format';
import { cleanUrl, loadHome, saveHome } from '@/lib/home';
import { S, themed, useColors } from '@/theme';

// The home server, as the phone reaches it (home.ts): the same address and
// token as the gauge's Upload card. The drives the gauge uploaded as the car
// started then come to the phone too, by themselves, whenever it is where
// the server is - so they are on it almost all the time.
export function HomeServerCard() {
  const ui = useUi();
  const C = useColors();
  const styles = useStyles();
  const { syncHome, lastHome, phase } = useSync();
  const [url, setUrl] = useState('');
  const [token, setToken] = useState('');
  const [had, setHad] = useState<{ url: string; token: string } | null>(null);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    let live = true;
    loadHome().then((h) => {
      if (!live) return;
      setHad(h ?? { url: '', token: '' });
      setUrl(h?.url ?? '');
    });
    return () => {
      live = false;
    };
  }, []);

  if (!had) return null;
  const changed = cleanUrl(url) !== had.url || token.trim() !== '';
  const save = async () => {
    const next = { url, token: token.trim() || had.token };
    await saveHome(next);
    setHad({ url: cleanUrl(url), token: next.token });
    setUrl(cleanUrl(url));
    setToken('');
  };
  const syncNow = async () => {
    setBusy(true);
    try {
      await syncHome(true);
    } finally {
      setBusy(false);
    }
  };

  return (
    <Card title={t('Home server')}>
      <Note>
        {t(
          'Where the gauge uploads its logs as the car starts. The phone pulls them from there too, whenever it can reach it - at home, or with its VPN on.',
        )}
      </Note>
      <View style={styles.field}>
        <Text style={ui.rowLabel}>{t('Address')}</Text>
        <TextInput
          style={styles.input}
          value={url}
          onChangeText={setUrl}
          placeholder="gauge.example.org"
          placeholderTextColor={C.dim}
          autoCapitalize="none"
          autoCorrect={false}
          keyboardType="url"
        />
      </View>
      <View style={styles.field}>
        <Text style={ui.rowLabel}>{t('Token')}</Text>
        <TextInput
          style={styles.input}
          value={token}
          onChangeText={setToken}
          placeholder={had.token ? t('set - type to change') : t('not set')}
          placeholderTextColor={C.dim}
          secureTextEntry
          autoCapitalize="none"
          autoCorrect={false}
        />
      </View>
      <Button title={changed ? t('Save') : t('No changes')} disabled={!changed} onPress={save} />
      {had.url ? (
        <>
          <Row
            label={t('Last sync from it')}
            value={lastHome ? tLast(lastHome.files, shortWhen(lastHome.at)) : t('not yet')}
            tone="dim"
          />
          <Button title={t('Sync from the home server')} kind="plain" busy={busy} disabled={phase !== 'idle'} onPress={syncNow} />
        </>
      ) : null}
    </Card>
  );
}

function tLast(files: number, when: string): string {
  return files ? tn(files, '{when}, 1 new file', '{when}, {n} new files', { when }) : t('{when}, nothing new', { when });
}

const useStyles = themed((C) =>
  StyleSheet.create({
    field: { gap: S.xs },
    input: {
      color: C.text,
      borderColor: C.border,
      borderWidth: 1,
      borderRadius: 6,
      paddingHorizontal: S.sm,
      paddingVertical: 6,
      fontSize: 15,
    },
  }),
);
