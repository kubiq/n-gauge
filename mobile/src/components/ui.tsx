import { createContext, type ReactNode, useContext, useState } from 'react';
import { ActivityIndicator, Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { SafeAreaView, useSafeAreaInsets } from 'react-native-safe-area-context';

import { S, themed, useColors } from '@/theme';

/**
 * Every screen sits under a header, which already clears the status bar,
 * and keeps clear of Android's navigation buttons itself - the app draws
 * edge to edge. That space is scroll padding, not a margin, so content
 * still scrolls behind the transparent bar but its end stops above it.
 */
/**
 * `overlay` is drawn over the page, outside its scrolling - for a Pill, so
 * progress takes no room in the content (a line above the trips list pushed
 * every row down and back up again while a sync ran).
 */
export function Screen({ children, overlay }: { children: ReactNode; overlay?: ReactNode }) {
  const insets = useSafeAreaInsets();
  const ui = useUi();
  // Held by something that needs the finger to itself - the map: dragging
  // it with two fingers scrolled the page instead (useHoldScroll).
  const [held, setHeld] = useState(false);
  return (
    <HoldScrollContext.Provider value={setHeld}>
      <SafeAreaView style={ui.screen} edges={[]}>
        {/* "handled": a tap on a button while typing presses it. The default
            spent the first tap only closing the keyboard - Save took two. */}
        <ScrollView
          scrollEnabled={!held}
          keyboardShouldPersistTaps="handled"
          contentContainerStyle={[ui.scroll, { paddingBottom: S.md + insets.bottom }]}>
          {children}
        </ScrollView>
        {overlay}
      </SafeAreaView>
    </HoldScrollContext.Provider>
  );
}

/**
 * Work in progress, floating at the foot of the screen like a toast that
 * stays while the work runs - for a Screen's `overlay`. Not touchable: what
 * is under it can still be tapped.
 */
export function Pill({ children }: { children: ReactNode }) {
  const insets = useSafeAreaInsets();
  const ui = useUi();
  return (
    <View pointerEvents="none" style={[ui.pillWrap, { bottom: S.lg + insets.bottom }]}>
      <Text style={ui.pill}>{children}</Text>
    </View>
  );
}

const HoldScrollContext = createContext<(held: boolean) => void>(() => {});

/**
 * Touch handlers that stop the screen scrolling while a finger is down on
 * the view they are spread on - for one that takes its own gestures (the
 * map's pan and pinch) inside a scrolling screen.
 */
export function useHoldScroll() {
  const hold = useContext(HoldScrollContext);
  return {
    onTouchStart: () => hold(true),
    onTouchEnd: () => hold(false),
    onTouchCancel: () => hold(false),
  };
}

export function Card({ title, children }: { title?: string; children: ReactNode }) {
  const ui = useUi();
  return (
    <View style={ui.card}>
      {title ? <Text style={ui.cardTitle}>{title}</Text> : null}
      {children}
    </View>
  );
}

export function Row({ label, value, tone }: { label: string; value: string; tone?: 'good' | 'bad' | 'dim' }) {
  const C = useColors();
  const ui = useUi();
  const color = tone === 'good' ? C.good : tone === 'bad' ? C.bad : tone === 'dim' ? C.dim : C.text;
  return (
    <View style={ui.row}>
      <Text style={ui.rowLabel}>{label}</Text>
      <Text style={[ui.rowValue, { color }]}>{value}</Text>
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
  const C = useColors();
  const ui = useUi();
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
        ui.button,
        filled && ui.buttonPrimary,
        kind === 'danger' && !inert && ui.buttonDanger,
        pressed && { opacity: 0.6 },
      ]}>
      {busy ? (
        <ActivityIndicator color={filled ? C.bg : C.text} />
      ) : (
        <Text style={[ui.buttonText, filled && { color: C.bg }, inert && { color: C.dim }]}>{title}</Text>
      )}
    </Pressable>
  );
}

/** Small text under a control. `good` for a quiet "done" instead of an alert. */
export function Note({ children, tone }: { children: ReactNode; tone?: 'bad' | 'good' }) {
  const C = useColors();
  const ui = useUi();
  return <Text style={[ui.note, tone === 'bad' && { color: C.bad }, tone === 'good' && { color: C.good }]}>{children}</Text>;
}

/** The shared styles, in the current theme - other screens use them too. */
export const useUi = themed((C) =>
  StyleSheet.create({
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
    // Pill: centred over the foot of the screen, in the card's colours.
    pillWrap: { position: 'absolute', left: S.md, right: S.md, alignItems: 'center' },
    pill: {
      backgroundColor: C.card,
      borderColor: C.border,
      borderWidth: 1,
      borderRadius: 20,
      paddingVertical: S.sm,
      paddingHorizontal: S.md,
      color: C.text,
      fontSize: 13,
      overflow: 'hidden',
    },
    input: {
      color: C.text,
      borderColor: C.border,
      borderWidth: 1,
      borderRadius: 8,
      paddingHorizontal: S.md,
      paddingVertical: 10,
      fontSize: 15,
    },
    // A list of choices, one picked: a gauge setting's options, the theme.
    options: { borderColor: C.border, borderWidth: 1, borderRadius: 8, overflow: 'hidden' },
    option: {
      flexDirection: 'row',
      alignItems: 'center',
      gap: S.sm,
      paddingVertical: 12,
      paddingHorizontal: S.md,
      borderTopColor: C.border,
      borderTopWidth: StyleSheet.hairlineWidth,
    },
    optionOn: { backgroundColor: C.bg },
    radio: { width: 16, height: 16, borderRadius: 8, borderWidth: 2, borderColor: C.dim },
    radioOn: { borderColor: C.accent, backgroundColor: C.accent },
  }),
);
