import { router } from 'expo-router';
import { ActivityIndicator, Pressable, StyleSheet, Text, View } from 'react-native';

import { type MenuItem, OverflowMenu } from '@/components/overflow-menu';
import { usePod } from '@/hooks/use-pod';
import { useSync } from '@/hooks/use-sync';
import { shortWhen } from '@/lib/format';
import { C, S } from '@/theme';

// The gauge in the header of Trips and Settings, the way Garmin Connect shows its watch:
// the connection and the sync at a glance, from any screen. The ring is the
// gauge's round face, its colour the state; a spinner while something is
// going on. A tap connects, or - connected - opens the Gauge screen.
type Status = { title: string; detail: string | null; ring: string; busy: boolean; bad?: boolean };

function status(pod: ReturnType<typeof usePod>, sync: ReturnType<typeof useSync>): Status {
  const synced = sync.lastSync ? `Synced ${shortWhen(sync.lastSync)}` : null;
  if (!pod.creds) return { title: 'Set up the gauge', detail: null, ring: C.dim, busy: false };
  if (pod.state === 'connecting') return { title: 'Connecting…', detail: synced, ring: C.dim, busy: true };
  if (pod.state === 'idle') return { title: 'Not connected', detail: synced, ring: C.dim, busy: false };
  if (sync.progress)
    return { title: 'Connected', detail: `Syncing ${sync.progress.at}/${sync.progress.of}`, ring: C.good, busy: true };
  if (sync.phase !== 'idle') return { title: 'Connected', detail: 'Checking…', ring: C.good, busy: true };
  if (sync.err) return { title: 'Connected', detail: 'Sync failed', ring: C.bad, busy: false, bad: true };
  return { title: 'Connected', detail: synced, ring: C.good, busy: false };
}

// `compact`: the ring alone, for a header whose title needs the room (a
// drive's date); the words stay in the accessibility label.
export function PodChip({ compact = false }: { compact?: boolean }) {
  const pod = usePod();
  const { title, detail, ring, busy, bad } = status(pod, useSync());

  // Not connected: connect and stay put - often the connection is all that
  // was wanted (a setting to save, firmware to send). Otherwise the Gauge screen.
  const onPress = () => {
    if (pod.creds && pod.state === 'idle') pod.connect();
    else router.push('/gauge');
  };

  return (
    <Pressable
      onPress={onPress}
      hitSlop={8}
      accessibilityRole="button"
      accessibilityLabel={`Gauge: ${title}${detail ? `, ${detail}` : ''}`}
      style={({ pressed }) => [styles.chip, pressed && { opacity: 0.6 }]}>
      <View style={styles.face}>
        {busy ? <ActivityIndicator size="small" color={C.accent} /> : <View style={[styles.ring, { borderColor: ring }]} />}
      </View>
      {compact ? null : (
        <View>
          <Text style={styles.title} numberOfLines={1}>
            {title}
          </Text>
          {detail ? (
            <Text style={[styles.detail, bad && { color: C.bad }]} numberOfLines={1}>
              {detail}
            </Text>
          ) : null}
        </View>
      )}
    </Pressable>
  );
}

/** A header's right side: the gauge, then the ⋮ menu if there is one. */
export function HeaderActions({ items = [], compact = false }: { items?: MenuItem[]; compact?: boolean }) {
  return (
    <View style={styles.actions}>
      <PodChip compact={compact} />
      <OverflowMenu items={items} />
    </View>
  );
}

const styles = StyleSheet.create({
  actions: { flexDirection: 'row', alignItems: 'center', gap: S.xs, marginRight: S.xs },
  chip: { flexDirection: 'row', alignItems: 'center', gap: S.sm, paddingHorizontal: S.sm, maxWidth: 170 },
  face: { width: 22, height: 22, alignItems: 'center', justifyContent: 'center' },
  ring: { width: 18, height: 18, borderRadius: 9, borderWidth: 3 },
  title: { color: C.text, fontSize: 13, fontWeight: '600' },
  detail: { color: C.dim, fontSize: 11 },
});
