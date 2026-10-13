import { Tabs } from 'expo-router';

import { HeaderActions } from '@/components/pod-chip';
import { C } from '@/theme';

// The tabs are the phone's own features. The gauge itself - connection,
// sync, live data - is one tap away in every header (PodChip -> /gauge).
// Labels only for now; icons once the screens settle.
export default function TabsLayout() {
  return (
    <Tabs
      screenOptions={{
        // A header on every tab: the title, the gauge (PodChip), and a ⋮
        // menu for the rarer actions - a screen with menu items sets its own
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
      <Tabs.Screen name="index" options={{ title: 'Trips' }} />
      <Tabs.Screen name="settings" options={{ title: 'Settings' }} />
    </Tabs>
  );
}
