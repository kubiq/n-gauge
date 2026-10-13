import { useState } from 'react';
import { Pressable, StyleSheet, Switch, Text, View } from 'react-native';

import { Button, Card, Note, styles as ui } from '@/components/ui';
import { moved, type ScreenItem, screenItems, viewsOf, WIFI } from '@/lib/screens';
import { C, S } from '@/theme';

// Which of the gauge's screens a tap or swipe cycles through, and in what
// order (settings.views on the gauge). Saved on its own, not with the table
// above: it is one value, sent as a whole.
export function ScreenOrderCard({
  views,
  busy,
  onSave,
}: {
  views: string;
  busy: boolean;
  onSave: (views: string) => void;
}) {
  const [items, setItems] = useState<ScreenItem[]>(() => screenItems(views));
  const changed = viewsOf(items) !== views;

  return (
    <Card title="Screens">
      <Note>The order a tap or swipe goes through on the gauge. WiFi stays: it is the way to the phone.</Note>
      {items.map((s, i) => (
        <View key={s.id} style={styles.row}>
          <Text style={[ui.rowLabel, styles.name, !s.on && { color: C.dim }]}>{s.name}</Text>
          <Arrow label="↑" disabled={i === 0} onPress={() => setItems(moved(items, i, -1))} />
          <Arrow label="↓" disabled={i === items.length - 1} onPress={() => setItems(moved(items, i, 1))} />
          <Switch
            value={s.on}
            disabled={s.id === WIFI}
            onValueChange={(on) => setItems(items.map((x) => (x.id === s.id ? { ...x, on } : x)))}
            trackColor={{ true: C.accent, false: C.border }}
          />
        </View>
      ))}
      <Button
        title={changed ? 'Save the order' : 'No changes'}
        disabled={!changed}
        busy={busy}
        onPress={() => onSave(viewsOf(items))}
      />
    </Card>
  );
}

function Arrow({ label, disabled, onPress }: { label: string; disabled: boolean; onPress: () => void }) {
  return (
    <Pressable
      onPress={onPress}
      disabled={disabled}
      hitSlop={6}
      style={({ pressed }) => [styles.arrow, pressed && { opacity: 0.5 }]}>
      <Text style={[styles.arrowText, disabled && { color: C.border }]}>{label}</Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  row: { flexDirection: 'row', alignItems: 'center', gap: S.sm, paddingVertical: S.xs },
  name: { flex: 1 },
  arrow: { width: 36, height: 36, alignItems: 'center', justifyContent: 'center', borderRadius: 6, borderWidth: 1, borderColor: C.border },
  arrowText: { color: C.text, fontSize: 18, fontWeight: '600' },
});
