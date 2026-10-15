import { router, useFocusEffect } from 'expo-router';
import { useCallback, useState } from 'react';
import { Alert, Pressable, StyleSheet, Text, View } from 'react-native';

import { Card, Note, useUi } from '@/components/ui';
import { t } from '@/i18n';
import { loadNames, nameOwnPlace, type Names } from '@/lib/names';
import { S, themed } from '@/theme';

// The places you named from a drive's ⋮ ("Home", "Work"): every drive
// starting or ending within 300 m of one is called by it. Here they can be
// renamed - in the naming screen, as from a drive - or forgotten. Read on
// every visit, so a rename shows on the way back.
export function PlacesCard() {
  const [names, setNames] = useState<Names>({ places: [], titles: {} });
  const ui = useUi();
  const styles = useStyles();
  useFocusEffect(useCallback(() => setNames(loadNames()), []));

  const rename = (p: Names['places'][number]) =>
    router.push({ pathname: '/name', params: { kind: 'place', lat: String(p.lat), lon: String(p.lon), current: p.name } });

  const forget = (p: Names['places'][number]) =>
    Alert.alert(t('Forget {name}?', { name: p.name }), t('Drives from and to it go back to the place names from the map.'), [
      { text: t('Cancel'), style: 'cancel' },
      { text: t('Forget'), style: 'destructive', onPress: () => setNames(nameOwnPlace(p.lat, p.lon, '')) },
    ]);

  const places = [...names.places].sort((a, b) => a.name.localeCompare(b.name));
  return (
    <Card title={t('Your places')}>
      {places.length === 0 ? (
        <Note>{t('None yet. On a drive, ⋮ → Name the start place (or the end) - e.g. Home - and every drive from or to there is called by it.')}</Note>
      ) : (
        places.map((p) => (
          <View key={`${p.lat},${p.lon}`} style={styles.row}>
            <Pressable onPress={() => rename(p)} hitSlop={8} style={styles.name} accessibilityRole="button" accessibilityLabel={t('Rename {name}', { name: p.name })}>
              <Text style={ui.rowValue} numberOfLines={1}>
                {p.name}
              </Text>
            </Pressable>
            <Pressable onPress={() => forget(p)} hitSlop={8} style={styles.action} accessibilityRole="button" accessibilityLabel={t('Forget {name}', { name: p.name })}>
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
