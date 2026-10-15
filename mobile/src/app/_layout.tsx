import { DarkTheme, DefaultTheme, Stack, ThemeProvider } from 'expo-router';
import { StatusBar } from 'expo-status-bar';
import { useMemo } from 'react';

import { HeaderActions } from '@/components/pod-chip';
import { LanguageProvider } from '@/hooks/use-language';
import { PodProvider } from '@/hooks/use-pod';
import { SyncProvider } from '@/hooks/use-sync';
import { ThemeChoiceProvider } from '@/hooks/use-theme';
import { t } from '@/i18n';
import { useColors } from '@/theme';

export default function RootLayout() {
  return (
    <ThemeChoiceProvider>
      <Themed />
    </ThemeChoiceProvider>
  );
}

// Below the theme choice, so the headers and the status bar follow it.
function Themed() {
  const C = useColors();
  const theme = useMemo(() => {
    const base = C.dark ? DarkTheme : DefaultTheme;
    return { ...base, colors: { ...base.colors, background: C.bg, card: C.card, border: C.border, primary: C.accent, text: C.text } };
  }, [C]);
  return (
    <ThemeProvider value={theme}>
      <PodProvider>
        <SyncProvider>
          {/* Inside the connection and the sync: a language change redraws
              the screens, not the link to the gauge. */}
          <LanguageProvider>
            <StatusBar style={C.dark ? 'light' : 'dark'} />
            <Screens />
          </LanguageProvider>
        </SyncProvider>
      </PodProvider>
    </ThemeProvider>
  );
}

// Its own component, rendered inside LanguageProvider: the titles' t() runs
// after the language is set, not before, as it would in Themed's render.
function Screens() {
  return (
    <Stack>
      {/* No tabs: Trips is the app; Settings is in its ⋮ menu, the gauge
          in the header (PodChip) - and on Settings and Gauge settings
          too, which need the connection for the gauge's side. */}
      {/* The main screen carries the app's name; it lists the drives. */}
      <Stack.Screen name="index" options={{ title: 'N Gauge' }} />
      <Stack.Screen name="settings" options={{ title: t('Settings'), headerRight: () => <HeaderActions /> }} />
      <Stack.Screen name="gauge" options={{ title: t('Gauge') }} />
      <Stack.Screen name="gauge-settings" options={{ title: t('Gauge settings'), headerRight: () => <HeaderActions /> }} />
      <Stack.Screen name="scan" options={{ presentation: 'modal', title: t('Scan the gauge’s QR') }} />
      <Stack.Screen name="server" options={{ presentation: 'modal', title: t('Upload server') }} />
      <Stack.Screen name="trip/[key]" options={{ title: t('Drive') }} />
      <Stack.Screen name="name" options={{ presentation: 'modal', title: t('Name') }} />
      {/* The gauge in the header wherever a screen may ask to connect;
          not on the Gauge screen itself, nor on the modals. */}
      <Stack.Screen name="log-key" options={{ title: t('Log encryption'), headerRight: () => <HeaderActions compact /> }} />
    </Stack>
  );
}
