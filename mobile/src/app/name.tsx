import { router, Stack, useLocalSearchParams, useNavigation } from 'expo-router';
import { useEffect, useRef, useState } from 'react';
import { Alert, StyleSheet, TextInput } from 'react-native';

import { Button, Card, Note, Screen } from '@/components/ui';
import { t, tn } from '@/i18n';
import { forgetPlaces, loadNames, nameOwnPlace, renamePlaces, setTripTitle } from '@/lib/names';
import { ownPlaceAt } from '@/lib/places';
import { S, themed, useColors } from '@/theme';

// Naming, from a drive's ⋮ (Alert.prompt is iOS only):
// - kind=trip: the drive's own name, over the automatic one;
// - kind=place: a place of yours at lat/lon - every drive starting or ending
//   within 300 m of it is then named after it (places.ts).
// `current` is the name shown now, to start from.
export default function NameScreen() {
  const p = useLocalSearchParams<{
    kind: 'trip' | 'place' | 'group';
    key?: string;
    lat?: string;
    lon?: string;
    current?: string;
  }>();
  const trip = p.kind === 'trip';
  // kind=group, from Settings: every spot of the name in `current` at once
  // (names.ts renamePlaces) - a drive's ⋮ names its one spot (kind=place).
  const group = p.kind === 'group' ? (p.current ?? '') : null;
  const lat = Number(p.lat), lon = Number(p.lon);
  const mine = trip || group !== null ? null : ownPlaceAt(lat, lon, loadNames().places);
  const own = trip ? !!(p.key && loadNames().titles[p.key]) : group !== null || !!mine;
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

  const done = () => router.back();
  const save = (value: string | null) => {
    if (trip) setTripTitle(p.key!, value);
    else if (group !== null) {
      if (value) renamePlaces(group, value);
      else forgetPlaces(group);
    } else {
      // A spot of a place with others - "Home" at both ends of drives: the
      // whole place, or this spot alone? Asked only then; one spot, at once.
      const spots = mine ? loadNames().places.filter((q) => q.name === mine.name).length : 0;
      if (mine && spots > 1 && value !== mine.name) {
        Alert.alert(
          tn(spots, '{name} has 1 spot.', '{name} has {n} spots.', { name: mine.name }),
          value ? t('Rename all of them, or just this one?') : t('Forget all of them, or just this one?'),
          [
            { text: t('Cancel'), style: 'cancel' },
            {
              text: t('Just this one'),
              onPress: () => {
                nameOwnPlace(lat, lon, value ?? '');
                done();
              },
            },
            {
              text: tn(spots, 'All 1 spot', 'All {n} spots'),
              onPress: () => {
                if (value) renamePlaces(mine.name, value);
                else forgetPlaces(mine.name);
                done();
              },
            },
          ],
        );
        return;
      }
      nameOwnPlace(lat, lon, value ?? '');
    }
    done();
  };

  return (
    <Screen>
      <Stack.Screen
        options={{ title: trip ? t('Rename the drive') : group !== null || mine ? t('Rename your place') : t('Name this place') }}
      />
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
            : group !== null
              ? t('Every spot of this place is renamed - every drive that starts or ends within 300 m of one.')
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
