import { router, useFocusEffect } from 'expo-router';
import { useCallback, useState } from 'react';
import { Alert, Pressable, StyleSheet, Text, View } from 'react-native';

import { Card, Note, useUi } from '@/components/ui';
import { t, tn } from '@/i18n';
import { forgetPlaces, loadNames, type Names } from '@/lib/names';
import { S, themed } from '@/theme';

// The places you named from a drive's ⋮ ("Home", "Work"): every drive
// starting or ending within 300 m of one of a place's spots is called by
// it. Here they can be renamed - in the naming screen - or forgotten, each
// name with all its spots. Read on every visit, so a rename shows on the
// way back.
export function PlacesCard() {
  const [names, setNames] = useState<Names>({ places: [], titles: {} });
  const ui = useUi();
  const styles = useStyles();
  useFocusEffect(useCallback(() => setNames(loadNames()), []));

  // One row per name: a place can have several spots - "Home" named at
  // both ends of drives whose first fix landed a few hundred metres apart.
  // Renaming or forgetting here takes every spot of the name (names.ts).
  const rename = (name: string) => router.push({ pathname: '/name', params: { kind: 'group', current: name } });

  const forget = (name: string) =>
    Alert.alert(t('Forget {name}?', { name }), t('Drives from and to it go back to the place names from the map.'), [
      { text: t('Cancel'), style: 'cancel' },
      { text: t('Forget'), style: 'destructive', onPress: () => setNames(forgetPlaces(name)) },
    ]);

  const spots = new Map<string, number>();
  for (const p of names.places) spots.set(p.name, (spots.get(p.name) ?? 0) + 1);
  const places = [...spots].sort(([a], [b]) => a.localeCompare(b));
  return (
    <Card title={t('Your places')}>
      {places.length === 0 ? (
        <Note>{t('None yet. On a drive, ⋮ → Name the start place (or the end) - e.g. Home - and every drive from or to there is called by it.')}</Note>
      ) : (
        places.map(([name, count]) => (
          <View key={name} style={styles.row}>
            <Pressable onPress={() => rename(name)} hitSlop={8} style={styles.name} accessibilityRole="button" accessibilityLabel={t('Rename {name}', { name })}>
              <Text style={ui.rowValue} numberOfLines={1}>
                {name}
                {count > 1 ? <Text style={ui.note}>{`  ·  ${tn(count, '1 spot', '{n} spots')}`}</Text> : null}
              </Text>
            </Pressable>
            <Pressable onPress={() => forget(name)} hitSlop={8} style={styles.action} accessibilityRole="button" accessibilityLabel={t('Forget {name}', { name })}>
              <Text style={styles.link}>{t('Forget')}</Text>
            </Pressable>
          </View>
        ))
      )}
      {places.length ? <Note>{t('Tap a name to rename it. Drives within 300 m of a place take its name.')}</Note> : null}
    </Card>
  );
}

const useStyles = themed((C) =>
  StyleSheet.create({
    // 48 dp tall to hit, as the app's other tap targets.
    row: { flexDirection: 'row', alignItems: 'center', gap: S.sm, minHeight: 48 },
    name: { flex: 1, minHeight: 48, justifyContent: 'center' },
    action: { minHeight: 48, justifyContent: 'center', paddingHorizontal: S.sm },
    link: { color: C.link, fontSize: 14 },
  }),
);
