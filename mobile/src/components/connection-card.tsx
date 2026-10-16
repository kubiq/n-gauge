import { useState } from 'react';
import { StyleSheet, Switch, Text, View } from 'react-native';

import { Card, Note } from '@/components/ui';
import { t } from '@/i18n';
import { loadDisconnectAfterSync, saveDisconnectAfterSync } from '@/lib/store';
import { S, themed, useColors } from '@/theme';

// Whether the phone leaves the gauge's WiFi once the new logs are on it
// (use-sync.tsx) - on by default, so the maps and place names have internet.
export function ConnectionCard() {
  const [on, setOn] = useState(loadDisconnectAfterSync);
  const C = useColors();
  const styles = useStyles();
  const change = (v: boolean) => {
    saveDisconnectAfterSync(v);
    setOn(v);
  };
  return (
    <Card title={t('Gauge connection')}>
      <View style={styles.row}>
        <Text style={styles.label}>{t('Disconnect after syncing')}</Text>
        <Switch value={on} onValueChange={change} trackColor={{ true: C.accent }} />
      </View>
      <Note>
        {t(
          'Once the new logs are on the phone, it leaves the gauge’s WiFi and is back on the internet. Not while the gauge’s settings or the log key are open.',
        )}
      </Note>
    </Card>
  );
}

const useStyles = themed((C) =>
  StyleSheet.create({
    row: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: S.md },
    label: { color: C.text, fontSize: 15, flexShrink: 1 },
  }),
);
