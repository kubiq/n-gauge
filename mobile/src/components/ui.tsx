import type { ReactNode } from 'react';
import { ActivityIndicator, Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { SafeAreaView, useSafeAreaInsets } from 'react-native-safe-area-context';

import { C, S } from '@/theme';

/**
 * `underHeader`: a stack screen with a header (trip, server, scan), which
 * already clears the status bar - and has no tab bar below it, so it keeps
 * clear of Android's navigation buttons itself: the app draws edge to edge.
 * The space is scroll padding, not a margin, so content still scrolls behind
 * the transparent bar but its end stops above it.
 */
export function Screen({ children, underHeader }: { children: ReactNode; underHeader?: boolean }) {
  const insets = useSafeAreaInsets();
  return (
    <SafeAreaView style={styles.screen} edges={underHeader ? [] : ['top']}>
      <ScrollView contentContainerStyle={[styles.scroll, underHeader && { paddingBottom: S.md + insets.bottom }]}>
        {children}
      </ScrollView>
    </SafeAreaView>
  );
}

export function Card({ title, children }: { title?: string; children: ReactNode }) {
  return (
    <View style={styles.card}>
      {title ? <Text style={styles.cardTitle}>{title}</Text> : null}
      {children}
    </View>
  );
}

export function Row({ label, value, tone }: { label: string; value: string; tone?: 'good' | 'bad' | 'dim' }) {
  const color = tone === 'good' ? C.good : tone === 'bad' ? C.bad : tone === 'dim' ? C.dim : C.text;
  return (
    <View style={styles.row}>
      <Text style={styles.rowLabel}>{label}</Text>
      <Text style={[styles.rowValue, { color }]}>{value}</Text>
    </View>
  );
}

export function Button({
  title,
  onPress,
  busy,
  disabled,
  kind = 'primary',
}: {
  title: string;
  onPress: () => void;
  busy?: boolean;
  disabled?: boolean;
  kind?: 'primary' | 'plain' | 'danger';
}) {
  const off = disabled || busy;
  return (
    <Pressable
      onPress={onPress}
      disabled={off}
      style={({ pressed }) => [
        styles.button,
        kind === 'primary' && styles.buttonPrimary,
        kind === 'danger' && styles.buttonDanger,
        (pressed || off) && { opacity: 0.6 },
      ]}>
      {busy ? (
        <ActivityIndicator color={kind === 'primary' ? C.bg : C.text} />
      ) : (
        <Text style={[styles.buttonText, kind === 'primary' && { color: C.bg }]}>{title}</Text>
      )}
    </Pressable>
  );
}

export function Note({ children, tone }: { children: ReactNode; tone?: 'bad' }) {
  return <Text style={[styles.note, tone === 'bad' && { color: C.bad }]}>{children}</Text>;
}

export const styles = StyleSheet.create({
  screen: { flex: 1, backgroundColor: C.bg },
  scroll: { padding: S.md, gap: S.md },
  card: { backgroundColor: C.card, borderColor: C.border, borderWidth: 1, borderRadius: 12, padding: S.md, gap: S.sm },
  cardTitle: { color: C.accent, fontSize: 13, fontWeight: '600', textTransform: 'uppercase', letterSpacing: 0.8 },
  row: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', gap: S.sm, minHeight: 28 },
  rowLabel: { color: C.dim, fontSize: 15, flexShrink: 1 },
  rowValue: { color: C.text, fontSize: 15, fontVariant: ['tabular-nums'] },
  button: {
    borderColor: C.border,
    borderWidth: 1,
    borderRadius: 8,
    paddingVertical: 12,
    paddingHorizontal: S.md,
    alignItems: 'center',
    minHeight: 46,
    justifyContent: 'center',
  },
  buttonPrimary: { backgroundColor: C.accent, borderColor: C.accent },
  buttonDanger: { borderColor: C.bad },
  buttonText: { color: C.text, fontSize: 15, fontWeight: '600' },
  note: { color: C.dim, fontSize: 13, lineHeight: 18 },
});
