import { DarkTheme, Stack, ThemeProvider } from 'expo-router';
import { StatusBar } from 'expo-status-bar';

import { HeaderActions } from '@/components/pod-chip';
import { PodProvider } from '@/hooks/use-pod';
import { SyncProvider } from '@/hooks/use-sync';
import { C } from '@/theme';

const theme = {
  ...DarkTheme,
  colors: { ...DarkTheme.colors, background: C.bg, card: C.card, border: C.border, primary: C.accent, text: C.text },
};

export default function RootLayout() {
  return (
    <ThemeProvider value={theme}>
      <PodProvider>
        <SyncProvider>
          <StatusBar style="light" />
          <Stack>
            {/* No tabs: Trips is the app; Settings is in its ⋮ menu, the gauge
                in the header (PodChip) - and on Settings too, whose gauge
                half needs the connection. */}
            <Stack.Screen name="index" options={{ title: 'Trips' }} />
            <Stack.Screen name="settings" options={{ title: 'Settings', headerRight: () => <HeaderActions /> }} />
            <Stack.Screen name="gauge" options={{ title: 'Gauge' }} />
            <Stack.Screen name="scan" options={{ presentation: 'modal', title: 'Scan the gauge’s QR' }} />
            <Stack.Screen name="server" options={{ presentation: 'modal', title: 'Upload server' }} />
            <Stack.Screen name="trip/[key]" options={{ title: 'Drive' }} />
            {/* The gauge in the header wherever a screen may ask to connect;
                not on the Gauge screen itself, nor on the modals. */}
            <Stack.Screen name="log-key" options={{ title: 'Log encryption', headerRight: () => <HeaderActions compact /> }} />
          </Stack>
        </SyncProvider>
      </PodProvider>
    </ThemeProvider>
  );
}
