import { router, Tabs, usePathname } from 'expo-router';
import { useEffect } from 'react';

import { HeaderActions } from '@/components/pod-chip';
import { usePod } from '@/hooks/use-pod';
import { C } from '@/theme';

// Labels only for now; icons once the screens settle.
export default function TabsLayout() {
  const connected = usePod().state === 'connected';
  const path = usePathname();

  // Sync only works on the pod's network, so off it, it is not offered, and
  // a dropped connection (the pod went to sleep, out of range) on it lands
  // back on Pod, where Connect is. Pod, Trips and Settings are always there:
  // Trips needs the opposite - internet, for the map - and Settings holds
  // this phone's settings as well as the pod's.
  const podOnly = path === '/sync';
  useEffect(() => {
    if (!connected && podOnly) router.navigate('/');
  }, [connected, podOnly]);

  return (
    <Tabs
      screenOptions={{
        // A header on every tab: the title, the pod (PodChip), and a ⋮ menu
        // for the rarer actions - a screen with menu items sets its own
        // headerRight to HeaderActions with them.
        headerShown: true,
        headerRight: () => <HeaderActions />,
        headerStyle: { backgroundColor: C.bg },
        headerTintColor: C.text,
        headerShadowVisible: false,
        headerTitleStyle: { fontWeight: '600' },
        tabBarActiveTintColor: C.accent,
        tabBarInactiveTintColor: C.dim,
        tabBarStyle: { backgroundColor: C.card, borderTopColor: C.border },
        tabBarIconStyle: { display: 'none' },
        tabBarLabelStyle: { fontSize: 15, fontWeight: '600' },
      }}>
      <Tabs.Screen name="index" options={{ title: 'Pod' }} />
      <Tabs.Screen name="sync" options={{ title: 'Sync', href: connected ? undefined : null }} />
      <Tabs.Screen name="trips" options={{ title: 'Trips' }} />
      <Tabs.Screen name="settings" options={{ title: 'Settings' }} />
    </Tabs>
  );
}
