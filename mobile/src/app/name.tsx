import { router, Stack, useLocalSearchParams, useNavigation } from 'expo-router';
import { useEffect, useRef, useState } from 'react';
import { StyleSheet, TextInput } from 'react-native';

import { Button, Card, Note, Screen } from '@/components/ui';
import { t } from '@/i18n';
import { loadNames, nameOwnPlace, setTripTitle } from '@/lib/names';
import { ownPlaceAt } from '@/lib/places';
import { S, themed, useColors } from '@/theme';

// Naming, from a drive's ⋮ (Alert.prompt is iOS only):
// - kind=trip: the drive's own name, over the automatic one;
// - kind=place: a place of yours at lat/lon - every drive starting or ending
//   within 300 m of it is then named after it (places.ts).
// `current` is the name shown now, to start from.
export default function NameScreen() {
  const p = useLocalSearchParams<{ kind: 'trip' | 'place'; key?: string; lat?: string; lon?: string; current?: string }>();
  const trip = p.kind === 'trip';
  const lat = Number(p.lat), lon = Number(p.lon);
  const mine = trip ? null : ownPlaceAt(lat, lon, loadNames().places);
  const own = trip ? !!(p.key && loadNames().titles[p.key]) : !!mine;
  const [text, setText] = useState(p.current ?? '');
  // The keyboard up without a tap on the field - focused once the modal has
  // slid in, not during: autoFocus asked while it still moved, and Android
  // ignored it at times (2026-10-15). The end of the slide is the
  // navigation's transitionEnd; a timer covers it not coming.
  const input = useRef<TextInput>(null);
  const navigation = useNavigation();
  useEffect(() => {
    // Afresh - blurred first: a field focused before its window was active
    // stays focused with no keyboard, and focusing it again did nothing
    // (seen 2026-10-15: focused=true, mInputShown=false).
    const focus = () => {
      const el = input.current;
      if (!el) return;
      if (el.isFocused()) el.blur();
      el.focus();
    };
    const later = () => setTimeout(focus, 50);
    const off = navigation.addListener('transitionEnd' as never, later);
    const timer = setTimeout(focus, 500);
    return () => {
      off();
      clearTimeout(timer);
    };
  }, [navigation]);
  const C = useColors();
  const styles = useStyles();
  const name = text.trim();

  const save = (value: string | null) => {
    if (trip) setTripTitle(p.key!, value);
    else nameOwnPlace(lat, lon, value ?? '');
    router.back();
  };

  return (
    <Screen>
      <Stack.Screen options={{ title: trip ? t('Rename the drive') : mine ? t('Rename your place') : t('Name this place') }} />
      <Card>
        <TextInput
          ref={input}
          style={styles.input}
          value={text}
          onChangeText={setText}
          selectTextOnFocus
          placeholder={trip ? t('e.g. Track day') : t('e.g. Home')}
          placeholderTextColor={C.dim}
          returnKeyType="done"
          onSubmitEditing={() => name && save(name)}
        />
        <Note>
          {trip
            ? t('Only this drive. Without a name of its own it is called after where it started and ended.')
            : t('Every drive that starts or ends within 300 m of here is called by this name.')}
        </Note>
        <Button title={t('Save')} disabled={!name} onPress={() => save(name)} />
        {own ? (
          <Button
            title={trip ? t('Use the automatic name') : t('Forget this place')}
            kind="plain"
            onPress={() => save(null)}
          />
        ) : null}
      </Card>
    </Screen>
  );
}

const useStyles = themed((C) =>
  StyleSheet.create({
    input: {
      color: C.text,
      borderColor: C.border,
      borderWidth: 1,
      borderRadius: 8,
      paddingHorizontal: S.md,
      paddingVertical: 10,
      fontSize: 16,
    },
  }),
);
