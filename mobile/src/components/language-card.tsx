import { Pressable, StyleSheet, Text, View } from 'react-native';

import { Card, Note, useUi } from '@/components/ui';
import { useLanguage } from '@/hooks/use-language';
import { type LangChoice, t } from '@/i18n';

// The app's language: the phone's (Czech when the phone is, else English),
// or one picked. Each language's name is in that language, so it can be
// found by someone who cannot read the current one.
export function LanguageCard() {
  const { choice, setChoice } = useLanguage();
  const ui = useUi();
  const choices: { id: LangChoice; label: string }[] = [
    { id: 'system', label: t('Follow the phone') },
    { id: 'en', label: 'English' },
    { id: 'cs', label: 'Čeština' },
  ];
  return (
    <Card title={t('Language')}>
      <View style={ui.options}>
        {choices.map((c) => {
          const on = c.id === choice;
          return (
            <Pressable
              key={c.id}
              onPress={() => setChoice(c.id)}
              accessibilityRole="radio"
              accessibilityState={{ checked: on }}
              style={({ pressed }) => [ui.option, on && ui.optionOn, pressed && { opacity: 0.6 }]}>
              <View style={[ui.radio, on && ui.radioOn]} />
              <Text style={[ui.rowValue, styles.label]}>{c.label}</Text>
            </Pressable>
          );
        })}
      </View>
      <Note>{t('Place names stay as they are on the map. The gauge’s own screen is in English.')}</Note>
    </Card>
  );
}

const styles = StyleSheet.create({
  label: { flex: 1, textAlign: 'left' },
});
