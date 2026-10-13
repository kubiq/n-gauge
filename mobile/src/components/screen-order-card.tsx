import { useState } from 'react';
import { Pressable, StyleSheet, Switch, Text, View } from 'react-native';

import { Card, Note, styles as ui } from '@/components/ui';
import { moved, type ScreenItem, screenItems, viewsOf, WIFI } from '@/lib/screens';
import { C, S } from '@/theme';

// Which of the gauge's screens a tap or swipe cycles through, and in what
// order (settings.views on the gauge). Each change goes up through onChange;
// the Settings screen saves it with everything else, under one button.
export function ScreenOrderCard({ views, onChange }: { views: string; onChange: (views: string) => void }) {
  const [items, setItems] = useState<ScreenItem[]>(() => screenItems(views));
  const update = (next: ScreenItem[]) => {
    setItems(next);
    onChange(viewsOf(next));
  };

  return (
    <Card title="Screens">
      <Note>The order a swipe goes through on the gauge. WiFi stays: it is the way to the phone.</Note>
      {items.map((s, i) => (
        <View key={s.id} style={styles.row}>
          <Text style={[ui.rowLabel, styles.name, !s.on && { color: C.border }]}>{s.name}</Text>
          <Arrow label="↑" disabled={i === 0} onPress={() => update(moved(items, i, -1))} />
          <Arrow label="↓" disabled={i === items.length - 1} onPress={() => update(moved(items, i, 1))} />
          <Switch
            value={s.on}
            disabled={s.id === WIFI}
            onValueChange={(on) => update(items.map((x) => (x.id === s.id ? { ...x, on } : x)))}
            trackColor={{ true: C.accent, false: C.border }}
          />
        </View>
      ))}
    </Card>
  );
}

// 48 dp: Android's minimum touch target.
function Arrow({ label, disabled, onPress }: { label: string; disabled: boolean; onPress: () => void }) {
  return (
    <Pressable
      onPress={onPress}
      disabled={disabled}
      accessibilityLabel={label === '↑' ? 'Move up' : 'Move down'}
      style={({ pressed }) => [styles.arrow, pressed && { opacity: 0.5 }]}>
      <Text style={[styles.arrowText, disabled && { color: C.border }]}>{label}</Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  row: { flexDirection: 'row', alignItems: 'center', gap: S.sm, paddingVertical: S.xs },
  name: { flex: 1 },
  arrow: { width: 48, height: 48, alignItems: 'center', justifyContent: 'center', borderRadius: 8, borderWidth: 1, borderColor: C.border },
  arrowText: { color: C.text, fontSize: 20, fontWeight: '600' },
});
