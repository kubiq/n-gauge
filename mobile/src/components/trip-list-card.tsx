import { useState } from 'react';
import { StyleSheet, Switch, Text, View } from 'react-native';

import { Card, Note } from '@/components/ui';
import { t } from '@/i18n';
import { type ListFacts, loadListFacts, saveListFacts } from '@/lib/store';
import { S, themed, useColors } from '@/theme';

// Which figures each drive shows in the trips list, beyond its time and top
// speed. A drive's own screen always shows them all.
export function TripListCard() {
  const [facts, setFacts] = useState(loadListFacts);
  const C = useColors();
  const styles = useStyles();
  const change = (key: keyof ListFacts) => (v: boolean) => {
    const next = { ...facts, [key]: v };
    saveListFacts(next);
    setFacts(next);
  };
  const rows: [keyof ListFacts, string][] = [
    ['boost', t('Peak boost')],
    ['oil', t('Peak oil')],
    ['fuelL', t('Fuel used')],
    ['per100', t('Consumption')],
  ];
  return (
    <Card title={t('Trip list')}>
      {rows.map(([key, label]) => (
        <View key={key} style={styles.row}>
          <Text style={styles.label}>{label}</Text>
          <Switch value={facts[key]} onValueChange={change(key)} trackColor={{ true: C.accent }} />
        </View>
      ))}
      <Note>{t('Shown beside each drive in the list. A drive’s own screen always shows them all.')}</Note>
    </Card>
  );
}

const useStyles = themed((C) =>
  StyleSheet.create({
    row: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: S.md },
    label: { color: C.text, fontSize: 15, flexShrink: 1 },
  }),
);
