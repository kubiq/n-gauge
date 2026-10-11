import { router, Tabs, usePathname } from 'expo-router';
import { useEffect } from 'react';

import { usePod } from '@/hooks/use-pod';
import { C } from '@/theme';

// Labels only for now; icons once the screens settle.
export default function TabsLayout() {
  const connected = usePod().state === 'connected';
  const path = usePathname();

  // Sync and Settings only work on the pod's network, so off it they are
  // not offered, and a dropped connection (the pod went to sleep, out of
  // range) on one of them lands back on Pod, where Connect is. Pod and Trips
  // are always there - Trips needs the opposite: internet, for the map.
  const podOnly = path === '/sync' || path === '/settings';
  useEffect(() => {
    if (!connected && podOnly) router.navigate('/');
  }, [connected, podOnly]);

  return (
    <Tabs
      screenOptions={{
        headerShown: false,
        tabBarActiveTintColor: C.accent,
        tabBarInactiveTintColor: C.dim,
        tabBarStyle: { backgroundColor: C.card, borderTopColor: C.border },
        tabBarIconStyle: { display: 'none' },
        tabBarLabelStyle: { fontSize: 15, fontWeight: '600' },
      }}>
      <Tabs.Screen name="index" options={{ title: 'Pod' }} />
      <Tabs.Screen name="sync" options={{ title: 'Sync', href: connected ? undefined : null }} />
      <Tabs.Screen name="settings" options={{ title: 'Settings', href: connected ? undefined : null }} />
      <Tabs.Screen name="trips" options={{ title: 'Trips' }} />
    </Tabs>
  );
}
