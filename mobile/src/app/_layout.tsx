import { DarkTheme, Stack, ThemeProvider } from 'expo-router';
import { StatusBar } from 'expo-status-bar';

import { PodProvider } from '@/hooks/use-pod';
import { C } from '@/theme';

const theme = {
  ...DarkTheme,
  colors: { ...DarkTheme.colors, background: C.bg, card: C.card, border: C.border, primary: C.accent, text: C.text },
};

export default function RootLayout() {
  return (
    <ThemeProvider value={theme}>
      <PodProvider>
        <StatusBar style="light" />
        <Stack>
          <Stack.Screen name="(tabs)" options={{ headerShown: false }} />
          <Stack.Screen name="scan" options={{ presentation: 'modal', title: 'Scan the pod’s QR' }} />
          <Stack.Screen name="server" options={{ presentation: 'modal', title: 'Upload server' }} />
          <Stack.Screen name="trip/[key]" options={{ title: 'Drive' }} />
          <Stack.Screen name="log-key" options={{ title: 'Log encryption' }} />
        </Stack>
      </PodProvider>
    </ThemeProvider>
  );
}
