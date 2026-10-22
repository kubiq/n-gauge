import { useState } from 'react';
import { StyleSheet, Switch, Text, View } from 'react-native';

import { Card, Note } from '@/components/ui';
import { t } from '@/i18n';
import { loadBeaconAutoSync, loadBeaconNotify, setBeaconAutoSync, setBeaconNotify } from '@/lib/beacon';
import { loadDisconnectAfterSync, saveDisconnectAfterSync } from '@/lib/store';
import { S, themed, useColors } from '@/theme';

// Whether the phone leaves the gauge's WiFi once the new logs are on it
// (use-sync.tsx) - on by default, so the maps and place names have internet.
// And the gauge's beacon (beacon.ts): a note when it has a drive the phone
// does not, and syncing it when the app opens - both off by default, as they
// need location.
export function ConnectionCard() {
  const [on, setOn] = useState(loadDisconnectAfterSync);
  const [notify, setNotify] = useState(loadBeaconNotify);
  const [autoSync, setAutoSync] = useState(loadBeaconAutoSync);
  const C = useColors();
  const styles = useStyles();
  const change = (v: boolean) => {
    saveDisconnectAfterSync(v);
    setOn(v);
  };
  const changeNotify = async (v: boolean) => {
    if (!(await setBeaconNotify(v))) return;
    setNotify(v);
    if (!v) {
      setBeaconAutoSync(false);
      setAutoSync(false);
    }
  };
  const changeAutoSync = (v: boolean) => {
    setBeaconAutoSync(v);
    setAutoSync(v);
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
      <View style={styles.row}>
        <Text style={styles.label}>{t('Tell me about new drives')}</Text>
        <Switch value={notify} onValueChange={changeNotify} trackColor={{ true: C.accent }} />
      </View>
      {notify && (
        <View style={styles.row}>
          <Text style={styles.label}>{t('Sync them when the app opens')}</Text>
          <Switch value={autoSync} onValueChange={changeAutoSync} trackColor={{ true: C.accent }} />
        </View>
      )}
      <Note>
        {t(
          'Parked, the gauge’s WiFi says which drive it has last. The phone hears it in its WiFi scans, without connecting, and tells you of a drive it does not have yet - about every 15 minutes. Needs location “all the time”: Android shows WiFi scans only with it.',
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
