import { Pressable, StyleSheet, Text, View } from 'react-native';

import { Card, Note, useUi } from '@/components/ui';
import { useThemeChoice } from '@/hooks/use-theme';
import { N_, t } from '@/i18n';
import { THEMES, type ThemeChoice } from '@/theme';

// The app's look: three dark themes that differ by accent, Light, or the
// phone's own setting. Applies at once; kept on the phone (use-theme.tsx).
const CHOICES: { id: ThemeChoice; label: string; swatch: string[] }[] = [
  ...(Object.keys(THEMES) as (keyof typeof THEMES)[]).map((id) => ({
    id,
    label: THEMES[id].label,
    swatch: [THEMES[id].colors.card, THEMES[id].colors.accent],
  })),
  { id: 'system', label: N_('Follow the phone'), swatch: [THEMES.light.colors.card, THEMES.orange.colors.card] },
];

export function ThemeCard() {
  const { choice, setChoice } = useThemeChoice();
  const ui = useUi();
  return (
    <Card title={t('Theme')}>
      <View style={ui.options}>
        {CHOICES.map((c) => {
          const on = c.id === choice;
          return (
            <Pressable
              key={c.id}
              onPress={() => setChoice(c.id)}
              accessibilityRole="radio"
              accessibilityState={{ checked: on }}
              style={({ pressed }) => [ui.option, on && ui.optionOn, pressed && { opacity: 0.6 }]}>
              <View style={[ui.radio, on && ui.radioOn]} />
              <Text style={[ui.rowValue, styles.label]}>{t(c.label)}</Text>
              {/* What it looks like: its card and its accent. */}
              <View style={styles.swatch}>
                {c.swatch.map((s, i) => (
                  <View key={i} style={[styles.half, { backgroundColor: s }]} />
                ))}
              </View>
            </Pressable>
          );
        })}
      </View>
      <Note>
        {t('Dark is easier on the eyes in the car at night. Follow the phone: Light by day, Dark – orange by night, as the phone switches.')}
      </Note>
    </Card>
  );
}

const styles = StyleSheet.create({
  label: { flex: 1, textAlign: 'left' },
  swatch: { flexDirection: 'row', width: 32, height: 18, borderRadius: 4, overflow: 'hidden', borderWidth: StyleSheet.hairlineWidth, borderColor: '#8a97a3' },
  half: { flex: 1 },
});
