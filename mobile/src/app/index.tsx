import { router, useFocusEffect, useNavigation } from 'expo-router';
import { useCallback, useLayoutEffect, useRef, useState } from 'react';
import { Alert, Pressable, StyleSheet, Text, View } from 'react-native';

import { LockIcon } from '@/components/lock-icon';
import { HeaderActions } from '@/components/pod-chip';
import { useSync } from '@/hooks/use-sync';
import { Card, Note, Screen, styles as ui } from '@/components/ui';
import { duration, km, when } from '@/lib/format';
import { LogKeyError } from '@/lib/logtext';
import { saveLogsToFolder, savedText } from '@/lib/share';
import { cachedCsv, cachedGpx, type GpxStats, summarise, tripFiles } from '@/lib/trip-cache';
import { type CsvSummary, isEncryptedName, isGpxName, isStub, type TripFiles } from '@/lib/trips';
import { C, S } from '@/theme';

type Sums = Record<string, GpxStats | CsvSummary>;

// Every drive the phone holds. Summaries missing from the cache (new pulls)
// are worked out here, one file at a time, and the list fills in as they land.
//
// The summaries live in state, read from the cache once per visit and added
// to as each is worked out - never read from disk during render: the React
// Compiler skips re-rendering a row whose props did not change, so a row that
// read the cache itself stayed on "Reading…" after its summary had landed.
export default function TripsScreen() {
  const [trips, setTrips] = useState<TripFiles[]>([]);
  const [sums, setSums] = useState<Sums>({});
  // Why a file could not be read - an encrypted log without the right key.
  const [problems, setProblems] = useState<Record<string, string>>({});
  const [status, setStatus] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const working = useRef(false);
  // The sync runs by itself (use-sync.tsx): each file it lands re-reads the
  // list, so a drive appears while this tab is open. Landing during the
  // summaries below, the re-read waits for them (`again`, then `rerun`).
  const { landed } = useSync();
  const again = useRef(false);
  const [rerun, setRerun] = useState(0);

  useFocusEffect(
    useCallback(() => {
      // Read, so they are real dependencies: the triggers for a re-read.
      if (landed < 0 || rerun < 0) return;
      const list = tripFiles();
      const have: Sums = {};
      const todo: string[] = [];
      for (const t of list) {
        for (const name of [t.gpx, t.csv]) {
          if (!name) continue;
          const s = isGpxName(name) ? cachedGpx(name) : cachedCsv(name);
          if (s) have[name] = s;
          else todo.push(name);
        }
      }
      setTrips(list);
      setSums(have);
      setProblems({});
      // Says plainly whether the cache is doing its job.
      if (working.current) {
        if (todo.length) again.current = true;
        return;
      }
      setStatus(null);
      if (!todo.length) return;
      working.current = true;
      (async () => {
        for (const [i, name] of todo.entries()) {
          // Only while there is something to wait for; nothing once done.
          setStatus(`Reading new drives… ${i + 1} of ${todo.length}`);
          try {
            await summarise(name);
            const s = isGpxName(name) ? cachedGpx(name) : cachedCsv(name);
            if (s) setSums((prev) => ({ ...prev, [name]: s }));
          } catch (e) {
            const msg = e instanceof LogKeyError ? e.message : `Could not read it: ${String(e)}`;
            setProblems((prev) => ({ ...prev, [name]: msg }));
          }
        }
        setStatus(null);
        working.current = false;
        if (again.current) {
          again.current = false;
          setRerun((n) => n + 1);
        }
      })();
    }, [landed, rerun]),
  );

  const saveAll = async () => {
    setSaving(true);
    try {
      const names = trips.flatMap((t) => [t.gpx, t.csv]).filter((n): n is string => !!n);
      const r = await saveLogsToFolder(names, setStatus);
      setStatus(null);
      Alert.alert('Saved', savedText(r));
    } catch (e) {
      setStatus(null);
      Alert.alert('Save', String((e as Error).message));
    } finally {
      setSaving(false);
    }
  };

  // The app's one main screen: in the header the gauge (PodChip) and ⋮ with
  // Settings and "Save all to a folder" - decrypted, under plain names, the
  // way logs reach the PC now that they are encrypted on the card; already-
  // saved files are skipped. Disabled while a save runs.
  const navigation = useNavigation();
  const hasTrips = trips.length > 0;
  useLayoutEffect(() => {
    const save = { label: saving ? 'Saving…' : 'Save all to a folder', onPress: saveAll, disabled: saving };
    navigation.setOptions({
      headerRight: () => (
        <HeaderActions items={[{ label: 'Settings', onPress: () => router.push('/settings') }, ...(hasTrips ? [save] : [])]} />
      ),
    });
  }, [navigation, hasTrips, saving]); // eslint-disable-line react-hooks/exhaustive-deps

  // Rows with their summaries; restart fragments and blips hidden (isStub),
  // with a line saying how many - nothing just disappears.
  const rows = trips.map((t) => ({
    t,
    g: t.gpx ? (sums[t.gpx] as GpxStats | undefined) : undefined,
    c: t.csv ? (sums[t.csv] as CsvSummary | undefined) : undefined,
    problem: (t.gpx && problems[t.gpx]) || (t.csv && problems[t.csv]) || undefined,
  }));
  const [showShort, setShowShort] = useState(false);
  const longOnes = rows.filter((r) => r.problem || !isStub(r.g, r.c));
  const hidden = rows.length - longOnes.length;
  const shown = showShort ? rows : longOnes;

  return (
    <Screen>
      {status ? <Note>{status}</Note> : null}
      {trips.length === 0 ? (
        <Card>
          <Note>No drives on the phone yet. Tap the gauge at the top to connect - new drives come over by themselves.</Note>
        </Card>
      ) : (
        shown.map((r) => <TripRow key={r.t.key} t={r.t} g={r.g} c={r.c} problem={r.problem} />)
      )}
      {hidden ? (
        // A tap shows them in place, and hides them again.
        <Pressable onPress={() => setShowShort((s) => !s)} hitSlop={8} style={styles.more}>
          <Text style={styles.moreText}>
            {showShort
              ? `Hide the ${hidden} very short ${hidden === 1 ? 'drive' : 'drives'}`
              : `Show ${hidden} very short ${hidden === 1 ? 'drive' : 'drives'} (under 200 m)`}
          </Text>
        </Pressable>
      ) : null}
    </Screen>
  );
}

function TripRow({ t, g, c, problem }: { t: TripFiles; g?: GpxStats; c?: CsvSummary; problem?: string }) {
  const start = g?.start ?? t.start;
  const secs = g?.start && g.end ? (g.end - g.start) / 1000 : (c?.durationS ?? null);
  const pending = !problem && ((t.gpx && !g) || (t.csv && !c));
  const encrypted = [t.gpx, t.csv].some((n) => n && isEncryptedName(n));
  return (
    <Pressable
      onPress={() => router.push({ pathname: '/trip/[key]', params: { key: t.key } })}
      style={({ pressed }) => [ui.card, pressed && { opacity: 0.7 }]}>
      <View style={styles.head}>
        <View style={styles.titleRow}>
          <Text style={styles.title}>{start ? when(start) : t.key}</Text>
          {encrypted ? <LockIcon bad={!!problem} /> : null}
        </View>
        {g ? <Text style={styles.big}>{km(g.distanceM)}</Text> : null}
      </View>
      <View style={styles.facts}>
        {secs !== null ? <Fact label="time" value={duration(secs)} /> : null}
        {g ? <Fact label="top" value={`${Math.round(g.maxKmh)} km/h`} /> : null}
        {c?.maxBoostBar != null ? <Fact label="boost" value={`${c.maxBoostBar.toFixed(2)} bar`} /> : null}
        {c?.maxOilC != null ? <Fact label="oil" value={`${Math.round(c.maxOilC)} °C`} /> : null}
      </View>
      {!t.gpx ? <Note>No GPS track - car data only.</Note> : null}
      {pending ? <Note>Reading…</Note> : null}
      {problem ? <Note tone="bad">{problem}</Note> : null}
    </Pressable>
  );
}

function Fact({ label, value }: { label: string; value: string }) {
  return (
    <View>
      <Text style={ui.note}>{label}</Text>
      <Text style={ui.rowValue}>{value}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  head: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  titleRow: { flexDirection: 'row', alignItems: 'center', gap: S.sm },
  title: { color: C.text, fontSize: 16, fontWeight: '600' },
  big: { color: C.accent, fontSize: 18, fontWeight: '700' },
  facts: { flexDirection: 'row', flexWrap: 'wrap', gap: S.lg },
  // A link, not a card: 48 dp tall to hit, styled like the app's other links.
  more: { minHeight: 48, justifyContent: 'center' },
  moreText: { color: C.link, fontSize: 14 },
});
