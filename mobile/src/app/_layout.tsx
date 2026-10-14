import { DarkTheme, DefaultTheme, Stack, ThemeProvider } from 'expo-router';
import { StatusBar } from 'expo-status-bar';
import { useMemo } from 'react';

import { HeaderActions } from '@/components/pod-chip';
import { PodProvider } from '@/hooks/use-pod';
import { SyncProvider } from '@/hooks/use-sync';
import { ThemeChoiceProvider } from '@/hooks/use-theme';
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
          <StatusBar style={C.dark ? 'light' : 'dark'} />
          <Stack>
            {/* No tabs: Trips is the app; Settings is in its ⋮ menu, the gauge
                in the header (PodChip) - and on Settings and Gauge settings
                too, which need the connection for the gauge's side. */}
            {/* The main screen carries the app's name; it lists the drives. */}
            <Stack.Screen name="index" options={{ title: 'N Gauge' }} />
            <Stack.Screen name="settings" options={{ title: 'Settings', headerRight: () => <HeaderActions /> }} />
            <Stack.Screen name="gauge" options={{ title: 'Gauge' }} />
            <Stack.Screen name="gauge-settings" options={{ title: 'Gauge settings', headerRight: () => <HeaderActions /> }} />
            <Stack.Screen name="scan" options={{ presentation: 'modal', title: 'Scan the gauge’s QR' }} />
            <Stack.Screen name="server" options={{ presentation: 'modal', title: 'Upload server' }} />
            <Stack.Screen name="trip/[key]" options={{ title: 'Drive' }} />
            <Stack.Screen name="name" options={{ presentation: 'modal', title: 'Name' }} />
            {/* The gauge in the header wherever a screen may ask to connect;
                not on the Gauge screen itself, nor on the modals. */}
            <Stack.Screen name="log-key" options={{ title: 'Log encryption', headerRight: () => <HeaderActions compact /> }} />
          </Stack>
        </SyncProvider>
      </PodProvider>
    </ThemeProvider>
  );
}
