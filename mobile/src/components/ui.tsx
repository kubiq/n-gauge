import type { ReactNode } from 'react';
import { ActivityIndicator, Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { SafeAreaView, useSafeAreaInsets } from 'react-native-safe-area-context';

import { C, S } from '@/theme';

/**
 * Every screen sits under a header, which already clears the status bar,
 * and keeps clear of Android's navigation buttons itself - the app draws
 * edge to edge. That space is scroll padding, not a margin, so content
 * still scrolls behind the transparent bar but its end stops above it.
 */
export function Screen({ children }: { children: ReactNode }) {
  const insets = useSafeAreaInsets();
  return (
    <SafeAreaView style={styles.screen} edges={[]}>
      {/* "handled": a tap on a button while typing presses it. The default
          spent the first tap only closing the keyboard - Save took two. */}
      <ScrollView
        keyboardShouldPersistTaps="handled"
        contentContainerStyle={[styles.scroll, { paddingBottom: S.md + insets.bottom }]}>
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
  // Disabled (and not busy) looks it: no colour at all, dim text - a faded
  // orange "No changes" still read as a button to press. Busy keeps its
  // colour, with the spinner.
  const inert = disabled && !busy;
  const filled = kind === 'primary' && !inert;
  return (
    <Pressable
      onPress={onPress}
      disabled={disabled || busy}
      style={({ pressed }) => [
        styles.button,
        filled && styles.buttonPrimary,
        kind === 'danger' && !inert && styles.buttonDanger,
        pressed && { opacity: 0.6 },
      ]}>
      {busy ? (
        <ActivityIndicator color={filled ? C.bg : C.text} />
      ) : (
        <Text style={[styles.buttonText, filled && { color: C.bg }, inert && { color: C.dim }]}>{title}</Text>
      )}
    </Pressable>
  );
}

/** Small text under a control. `good` for a quiet "done" instead of an alert. */
export function Note({ children, tone }: { children: ReactNode; tone?: 'bad' | 'good' }) {
  return <Text style={[styles.note, tone === 'bad' && { color: C.bad }, tone === 'good' && { color: C.good }]}>{children}</Text>;
}

export const styles = StyleSheet.create({
  screen: { flex: 1, backgroundColor: C.bg },
  scroll: { padding: S.md, gap: S.md },
  card: { backgroundColor: C.card, borderColor: C.border, borderWidth: 1, borderRadius: 12, padding: S.md, gap: S.sm },
  cardTitle: { color: C.accent, fontSize: 13, fontWeight: '600', textTransform: 'uppercase', letterSpacing: 0.8 },
  row: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', gap: S.sm, minHeight: 28 },
  // The label keeps its width; a long value wraps on the right instead. The
  // other way round, a file name squeezed "Track" into "Tra / ck".
  rowLabel: { color: C.dim, fontSize: 15, flexShrink: 0 },
  rowValue: { color: C.text, fontSize: 15, fontVariant: ['tabular-nums'], flexShrink: 1, textAlign: 'right' },
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
