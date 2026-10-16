import { router, useFocusEffect, useNavigation } from 'expo-router';
import { useCallback, useLayoutEffect, useRef, useState } from 'react';
import { Alert, Pressable, StyleSheet, Text, View } from 'react-native';

import { LockIcon } from '@/components/lock-icon';
import { HeaderActions } from '@/components/pod-chip';
import { useSync } from '@/hooks/use-sync';
import { Card, Note, Screen, useUi } from '@/components/ui';
import { t, tn } from '@/i18n';
import { duration, km, when } from '@/lib/format';
import { LogKeyError } from '@/lib/logtext';
import { forgetStaleTitles, type Names, tripName } from '@/lib/names';
import { groupByWeek, weekLabel } from '@/lib/weeks';
import { saveLogsToFolder, savedText, trackLogNames } from '@/lib/share';
import { cachedCsv, cachedGpx, type GpxStats, summarise, tripFiles } from '@/lib/trip-cache';
import { type CsvSummary, isEncryptedName, isGpxName, isStub, type TripFiles } from '@/lib/trips';
import { S, themed } from '@/theme';

type Sums = Record<string, GpxStats | CsvSummary>;

// Every drive the phone holds. Summaries missing from the cache (new pulls)
// are worked out here, one file at a time, and the list fills in as they land.
//
// The summaries live in state, read from the cache once per visit and added
// to as each is worked out - never read from disk during render: the React
// Compiler skips re-rendering a row whose props did not change, so a row that
// read the cache itself stayed unread after its summary had landed.
export default function TripsScreen() {
  const [trips, setTrips] = useState<TripFiles[]>([]);
  const [sums, setSums] = useState<Sums>({});
  // Why a file could not be read - an encrypted log without the right key.
  const [problems, setProblems] = useState<Record<string, string>>({});
  const [status, setStatus] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const styles = useStyles();
  // Your places and renamed drives: read on every visit, so a name given on
  // a drive's screen shows on the way back.
  const [names, setNames] = useState<Names>({ places: [], titles: {} });
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
      setNames(forgetStaleTitles(list));
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
          setStatus(t('Reading new drives… {i} of {n}', { i: i + 1, n: todo.length }));
          try {
            await summarise(name);
            const s = isGpxName(name) ? cachedGpx(name) : cachedCsv(name);
            if (s) setSums((prev) => ({ ...prev, [name]: s }));
          } catch (e) {
            const msg = e instanceof LogKeyError ? e.message : t('Could not read it: {error}', { error: String(e) });
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
      // Read now, not from `trips`: the header's ⋮ keeps the function it was
      // given when it last updated (the effect below), and so its list - drives
      // synced since were missing, and "Save all" said all was already there.
      const now = tripFiles();
      const names = [...now.flatMap((t) => [t.gpx, t.csv]).filter((n): n is string => !!n), ...trackLogNames()];
      const r = await saveLogsToFolder(names, setStatus);
      setStatus(null);
      Alert.alert(t('Saved'), savedText(r));
    } catch (e) {
      setStatus(null);
      Alert.alert(t('Save'), String((e as Error).message));
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
    const save = { label: saving ? t('Saving…') : t('Save all to a folder'), onPress: saveAll, disabled: saving };
    navigation.setOptions({
      headerRight: () => (
        <HeaderActions items={[{ label: t('Settings'), onPress: () => router.push('/settings') }, ...(hasTrips ? [save] : [])]} />
      ),
    });
  }, [navigation, hasTrips, saving]);

  // Rows with their summaries; restart fragments and blips hidden (isStub),
  // with a line saying how many - nothing just disappears.
  //
  // A drive is listed once it has been read (or failed to be), not before:
  // shown unread, a blip vanished when its summary landed and the rows below
  // jumped up, and a row's week and title moved from the file name's stamp
  // to the track's - a list that looked shuffled while it filled. The files
  // are read top to bottom, so it now grows in order, with one "Reading new
  // drives…" line above it.
  const rows = trips.map((t) => {
    const g = t.gpx ? (sums[t.gpx] as GpxStats | undefined) : undefined;
    const c = t.csv ? (sums[t.csv] as CsvSummary | undefined) : undefined;
    const problem = (t.gpx && problems[t.gpx]) || (t.csv && problems[t.csv]) || undefined;
    return {
      t,
      g,
      c,
      problem,
      read: !!problem || ((!t.gpx || !!g) && (!t.csv || !!c)),
      name: tripName(t, g, names),
    };
  });
  const [showShort, setShowShort] = useState(false);
  const readOnes = rows.filter((r) => r.read);
  const longOnes = readOnes.filter((r) => r.problem || !isStub(r.g, r.c));
  const hidden = readOnes.length - longOnes.length;
  const shown = showShort ? readOnes : longOnes;
  // By week, each with its totals - of the drives shown, so the hidden
  // blips do not count. The same start and time the rows show.
  const weeks = groupByWeek(shown, (r) => ({
    start: r.g?.start ?? r.t.start,
    distanceM: r.g?.distanceM ?? null,
    secs: r.g?.start && r.g.end ? (r.g.end - r.g.start) / 1000 : (r.c?.durationS ?? null),
  }));

  return (
    <Screen>
      {status ? <Note>{status}</Note> : null}
      {trips.length === 0 ? (
        <Card>
          <Note>{t('No drives on the phone yet. Tap the gauge at the top to connect - new drives come over by themselves.')}</Note>
        </Card>
      ) : (
        weeks.map((w) => (
          <View key={w.monday ?? 'undated'} style={styles.week}>
            <Text style={styles.weekHead}>
              {weekLabel(w.monday)}
              <Text style={styles.weekTotals}>
                {`  ·  ${tn(w.drives, '1 drive', '{n} drives')}`}
                {w.distanceM > 0 ? `  ·  ${km(w.distanceM)}` : ''}
                {w.secs > 0 ? `  ·  ${duration(w.secs)}` : ''}
              </Text>
            </Text>
            {w.items.map((r) => (
              <TripRow key={r.t.key} t={r.t} g={r.g} c={r.c} problem={r.problem} name={r.name} />
            ))}
          </View>
        ))
      )}
      {hidden ? (
        // A tap shows them in place, and hides them again.
        <Pressable onPress={() => setShowShort((s) => !s)} hitSlop={8} style={styles.more}>
          <Text style={styles.moreText}>
            {showShort
              ? tn(hidden, 'Hide the 1 very short drive', 'Hide the {n} very short drives')
              : tn(hidden, 'Show 1 very short drive (under 200 m)', 'Show {n} very short drives (under 200 m)')}
          </Text>
        </Pressable>
      ) : null}
    </Screen>
  );
}

function TripRow({
  t: trip,
  g,
  c,
  problem,
  name,
}: {
  t: TripFiles;
  g?: GpxStats;
  c?: CsvSummary;
  problem?: string;
  name: string | null;
}) {
  const start = g?.start ?? trip.start;
  const secs = g?.start && g.end ? (g.end - g.start) / 1000 : (c?.durationS ?? null);
  const encrypted = [trip.gpx, trip.csv].some((n) => n && isEncryptedName(n));
  const ui = useUi();
  const styles = useStyles();
  return (
    <Pressable
      onPress={() => router.push({ pathname: '/trip/[key]', params: { key: trip.key } })}
      style={({ pressed }) => [ui.card, pressed && { opacity: 0.7 }]}>
      <View style={styles.head}>
        {/* Where it went (names.ts), the date under it; without a track the
            date is the title. No time anywhere in it (a numbered log, no
            GPS): not the file name. */}
        <View style={styles.titleCol}>
          <View style={styles.titleRow}>
            <Text style={styles.title} numberOfLines={1}>
              {name ?? (start ? when(start) : t('Undated drive'))}
            </Text>
            {encrypted ? <LockIcon bad={!!problem} /> : null}
          </View>
          {name && start ? <Text style={ui.note}>{when(start)}</Text> : null}
        </View>
        {g ? <Text style={styles.big}>{km(g.distanceM)}</Text> : null}
      </View>
      <View style={styles.facts}>
        {secs !== null ? <Fact label={t('time')} value={duration(secs)} /> : null}
        {g ? <Fact label={t('top')} value={`${Math.round(g.maxKmh)} km/h`} /> : null}
        {c?.maxBoostBar != null ? <Fact label={t('boost')} value={`${c.maxBoostBar.toFixed(2)} bar`} /> : null}
        {c?.maxOilC != null ? <Fact label={t('oil')} value={`${Math.round(c.maxOilC)} °C`} /> : null}
      </View>
      {!trip.gpx ? <Note>{t('No GPS track - car data only.')}</Note> : null}
      {problem ? <Note tone="bad">{problem}</Note> : null}
    </Pressable>
  );
}

function Fact({ label, value }: { label: string; value: string }) {
  const ui = useUi();
  return (
    <View>
      <Text style={ui.note}>{label}</Text>
      <Text style={ui.rowValue}>{value}</Text>
    </View>
  );
}

const useStyles = themed((C) =>
  StyleSheet.create({
    head: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', gap: S.sm },
    // Shrinks, so a long route name gives way to the distance, not over it.
    titleCol: { flexShrink: 1 },
    titleRow: { flexDirection: 'row', alignItems: 'center', gap: S.sm },
    title: { color: C.text, fontSize: 16, fontWeight: '600', flexShrink: 1 },
    big: { color: C.accent, fontSize: 18, fontWeight: '700' },
    facts: { flexDirection: 'row', flexWrap: 'wrap', gap: S.lg },
    // A week: its line, then its drives, spaced as the cards are.
    week: { gap: S.md },
    weekHead: { color: C.accent, fontSize: 13, fontWeight: '700', letterSpacing: 0.5, marginTop: S.sm },
    weekTotals: { color: C.dim, fontWeight: '400', letterSpacing: 0 },
    // A link, not a card: 48 dp tall to hit, styled like the app's other links.
    more: { minHeight: 48, justifyContent: 'center' },
    moreText: { color: C.link, fontSize: 14 },
  }),
);
