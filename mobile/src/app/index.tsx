import { router, useFocusEffect, useNavigation } from 'expo-router';
import { useCallback, useLayoutEffect, useRef, useState } from 'react';
import { Alert, Pressable, StyleSheet, Text, ToastAndroid, View } from 'react-native';

import { LockIcon } from '@/components/lock-icon';
import { HeaderActions } from '@/components/pod-chip';
import { useSync } from '@/hooks/use-sync';
import { Card, Note, Pill, Screen, useUi } from '@/components/ui';
import { t, tn } from '@/i18n';
import { duration, km, mb, when } from '@/lib/format';
import { LogKeyError } from '@/lib/logtext';
import { endName, forgetStaleTitles, type Names, tripName } from '@/lib/names';
import { groupByWeek, mondayOf, weekLabel } from '@/lib/weeks';
import { podTextLogNames, saveLogsToFolder, savedText, trackLogNames } from '@/lib/share';
import { type ListFacts, loadListFacts } from '@/lib/store';
import { cachedCsv, cachedGpx, type GpxStats, summarise, tripFiles } from '@/lib/trip-cache';
import {
  type CsvSummary,
  FILL_MIN_PCT,
  isEncryptedName,
  isGpxName,
  isStub,
  litresPer100,
  type Refuel,
  type TripFiles,
} from '@/lib/trips';
import { S, themed } from '@/theme';

type Sums = Record<string, GpxStats | CsvSummary>;

// Drives this recent are read by themselves; older ones only when their week
// is opened. Reading a drive means decrypting and parsing a 5-17 MB GPX, and
// after a summary change (trip-cache.ts VERSION) that was every drive on the
// phone, months back, on the list's first open (2026-10-20).
const RECENT_MS = 31 * 24 * 3600 * 1000;
// Undated drives (no clock) are few, and have no week to open: read always.
const isOld = (start: number | null) => start !== null && start < Date.now() - RECENT_MS;

// The i30 N's tank, for the litres a fill-up's levels come to.
const TANK_L = 50;

// Every drive the phone holds. Summaries missing from the cache (new pulls)
// are worked out here, one file at a time, and the list fills in as they land.
//
// The summaries live in state, read from the cache once per visit and added
// to as each is worked out - never read from disk during render: the React
// Compiler skips re-rendering a row whose props did not change, so a row that
// read the cache itself stayed unread after its summary had landed.
export default function TripsScreen() {
  const [trips, setTrips] = useState<TripFiles[]>([]);
  // Settings, "Trip list" - read again on each focus, so a change shows on return.
  const [facts, setFacts] = useState(loadListFacts);
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
  //
  // New drives are read once the sync has finished, not as each file lands,
  // and - with a list already showing - added to it together: one change,
  // where each drive appearing on its own moved the whole list down again,
  // once per file, while it was being read (2026-10-20). A first visit with
  // nothing read yet still fills in as it goes, rather than staying empty.
  const { landed, phase, progress } = useSync();
  const syncing = phase !== 'idle';
  const again = useRef(false);
  const [rerun, setRerun] = useState(0);
  // Older weeks opened this visit (their Monday): their drives are read too.
  const [opened, setOpened] = useState<ReadonlySet<number>>(() => new Set());

  useFocusEffect(
    useCallback(() => {
      // Read, so they are real dependencies: the triggers for a re-read.
      if (landed < 0 || rerun < 0) return;
      const list = tripFiles();
      const have: Sums = {};
      const todo: string[] = [];
      for (const t of list) {
        const wanted = !isOld(t.start) || (t.start !== null && opened.has(mondayOf(t.start)));
        for (const name of [t.gpx, t.csv]) {
          if (!name) continue;
          const s = isGpxName(name) ? cachedGpx(name) : cachedCsv(name);
          if (s) have[name] = s;
          else if (wanted) todo.push(name);
        }
      }
      setTrips(list);
      setFacts(loadListFacts());
      setNames(forgetStaleTitles(list));
      setSums(have);
      setProblems({});
      // Says plainly whether the cache is doing its job.
      if (working.current) {
        if (todo.length) again.current = true;
        return;
      }
      setStatus(null);
      if (!todo.length || syncing) return;   // after the sync: this runs again then
      working.current = true;
      const together = Object.keys(have).length > 0;
      (async () => {
        const got: Sums = {};
        const bad: Record<string, string> = {};
        for (const [i, name] of todo.entries()) {
          // Only while there is something to wait for; nothing once done.
          setStatus(t('Reading new drives… {i} of {n}', { i: i + 1, n: todo.length }));
          try {
            await summarise(name);
            const s = isGpxName(name) ? cachedGpx(name) : cachedCsv(name);
            if (s && together) got[name] = s;
            else if (s) setSums((prev) => ({ ...prev, [name]: s }));
          } catch (e) {
            const msg = e instanceof LogKeyError ? e.message : t('Could not read it: {error}', { error: String(e) });
            if (together) bad[name] = msg;
            else setProblems((prev) => ({ ...prev, [name]: msg }));
          }
        }
        if (together) {
          setSums((prev) => ({ ...prev, ...got }));
          setProblems((prev) => ({ ...prev, ...bad }));
        }
        setStatus(null);
        working.current = false;
        if (again.current) {
          again.current = false;
          setRerun((n) => n + 1);
        }
      })();
    }, [landed, rerun, syncing, opened]),
  );

  const saveAll = async () => {
    setSaving(true);
    try {
      // Read now, not from `trips`: the header's ⋮ keeps the function it was
      // given when it last updated (the effect below), and so its list - drives
      // synced since were missing, and "Save all" said all was already there.
      const now = tripFiles();
      const names = [
        ...now.flatMap((t) => [t.gpx, t.csv]).filter((n): n is string => !!n),
        ...trackLogNames(),
        ...podTextLogNames(),
      ];
      const r = await saveLogsToFolder(names, setStatus);
      setStatus(null);
      // Done is a toast, like the sync's messages (asked 2026-10-24); files
      // left out keep the box - their names are a list to read.
      if (r.failed.length) Alert.alert(t('Saved'), savedText(r));
      else ToastAndroid.show(savedText(r), ToastAndroid.SHORT);
    } catch (e) {
      setStatus(null);
      Alert.alert(t('Save'), String((e as Error).message));
    } finally {
      setSaving(false);
    }
  };

  // The app's one main screen: in the header the gauge (PodChip, a tap
  // syncs) and ⋮ with Settings - the phone's -, Gauge settings - the gauge's,
  // and how to set one up - and "Save all to a folder": decrypted, under
  // plain names, the way logs reach the PC now that they are encrypted on the
  // card; already-saved files are skipped. Disabled while a save runs.
  const navigation = useNavigation();
  const hasTrips = trips.length > 0;
  useLayoutEffect(() => {
    const save = { label: saving ? t('Saving…') : t('Save all to a folder'), onPress: saveAll, disabled: saving };
    navigation.setOptions({
      headerRight: () => (
        <HeaderActions
          items={[
            { label: t('Settings'), onPress: () => router.push('/settings') },
            { label: t('Gauge settings'), onPress: () => router.push('/gauge-settings') },
            ...(hasTrips ? [save] : []),
          ]}
        />
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
  // drives…" pill over it.
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
  // Fill-ups, on the drive they were on (asked 2026-10-22): during it (its
  // CSV's refuels), or before it - the tank fuller at its start than at the
  // end of the drive before, filled while the gauge slept. In time order.
  const fills: Record<string, Refuel[]> = {};
  let newer: (typeof rows)[number] | null = null;   // the drive after, with a start level
  for (const r of rows) {
    const c = r.c;
    if (c?.refuels.length) fills[r.t.key] = [...c.refuels];
    if (newer && c?.fuelEndPct != null && newer.c!.fuelStartPct! - c.fuelEndPct >= FILL_MIN_PCT) {
      const end = r.g?.end ?? (r.t.start !== null && c.durationS ? r.t.start + c.durationS * 1000 : r.t.start);
      (fills[newer.t.key] ??= []).unshift({
        at: end,
        pt: r.g?.to ?? null,
        place: r.g?.toPlace ?? null,
        fromPct: c.fuelEndPct,
        toPct: newer.c!.fuelStartPct!,
      });
    }
    if (c?.fuelStartPct != null) newer = r;
  }
  const [showShort, setShowShort] = useState(false);
  const readOnes = rows.filter((r) => r.read);
  // A hop to the pump is short, but not hidden with the blips.
  const longOnes = readOnes.filter((r) => r.problem || fills[r.t.key] || !isStub(r.g, r.c));
  const hidden = readOnes.length - longOnes.length;
  const shownSet = new Set(showShort ? readOnes : longOnes);
  // Older drives not read yet stay in their week, counted, until it is opened
  // - and while it is being read, so the week does not vanish and come back.
  // (Recent ones are being read anyway: listed once read, as above.)
  const unread = (r: (typeof rows)[number]) => !r.read && isOld(r.t.start);
  // By week, each with its totals - of the drives shown, so the hidden
  // blips do not count. The same start and time the rows show.
  const weeks = groupByWeek(
    rows.filter((r) => shownSet.has(r) || unread(r)),
    (r) => ({
      start: r.g?.start ?? r.t.start,
      distanceM: r.g?.distanceM ?? null,
      secs: r.g?.start && r.g.end ? (r.g.end - r.g.start) / 1000 : (r.c?.durationS ?? null),
    }),
  );

  return (
    // Progress (a sync's, reading new drives, saving to the folder) floats
    // over the list: as a line above it, it pushed every row down and back up.
    <Screen
      overlay={
        progress ? (
          <Pill>
            {t('Syncing {at} of {of}: {got} of {total}', {
              at: progress.at,
              of: progress.of,
              got: mb(progress.got),
              total: mb(progress.total),
            })}
          </Pill>
        ) : status ? (
          <Pill>{status}</Pill>
        ) : null
      }>
      {trips.length === 0 ? (
        <Card>
          <Note>{t('No drives on the phone yet. Tap the gauge at the top to connect - new drives come over by themselves.')}</Note>
        </Card>
      ) : (
        weeks.map((w) => {
          const waiting = w.items.filter(unread).length;
          const monday = w.monday;
          const isOpened = monday !== null && opened.has(monday);
          return (
            <View key={monday ?? 'undated'} style={styles.week}>
              <Text style={styles.weekHead}>
                {weekLabel(monday)}
                <Text style={styles.weekTotals}>
                  {`  ·  ${tn(w.drives, '1 drive', '{n} drives')}`}
                  {/* Totals only of a whole week: with drives unread they
                      would be a part passed off as the sum. */}
                  {!waiting && w.distanceM > 0 ? `  ·  ${km(w.distanceM)}` : ''}
                  {!waiting && w.secs > 0 ? `  ·  ${duration(w.secs)}` : ''}
                </Text>
              </Text>
              {w.items
                .filter((r) => !unread(r))
                .map((r) => (
                  <TripRow
                    key={r.t.key}
                    t={r.t}
                    g={r.g}
                    c={r.c}
                    problem={r.problem}
                    name={r.name}
                    facts={facts}
                    fills={fills[r.t.key]}
                    names={names}
                  />
                ))}
              {waiting && monday !== null ? (
                isOpened ? (
                  <Text style={styles.moreText}>{t('Reading…')}</Text>
                ) : (
                  <Pressable
                    onPress={() => setOpened((prev) => new Set(prev).add(monday))}
                    hitSlop={8}
                    style={styles.more}>
                    <Text style={styles.moreText}>{tn(waiting, 'Show 1 drive', 'Show {n} drives')}</Text>
                  </Pressable>
                )
              ) : null}
            </View>
          );
        })
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
  facts,
  fills,
  names,
}: {
  t: TripFiles;
  g?: GpxStats;
  c?: CsvSummary;
  problem?: string;
  name: string | null;
  facts: ListFacts;
  fills?: Refuel[];
  names: Names;
}) {
  const start = g?.start ?? trip.start;
  const secs = g?.start && g.end ? (g.end - g.start) / 1000 : (c?.durationS ?? null);
  const per100 = c ? litresPer100(c) : null;
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
        {facts.boost && c?.maxBoostBar != null ? <Fact label={t('boost')} value={`${c.maxBoostBar.toFixed(2)} bar`} /> : null}
        {facts.oil && c?.maxOilC != null ? <Fact label={t('oil')} value={`${Math.round(c.maxOilC)} °C`} /> : null}
        {facts.fuelL && c?.fuelL != null && c.fuelL > 0 ? <Fact label={t('fuel')} value={`${c.fuelL.toFixed(1)} l`} /> : null}
        {facts.per100 && per100 !== null ? <Fact label={t('consumption')} value={`${per100.toFixed(1)} l/100`} /> : null}
      </View>
      {fills?.map((f) => <FillLine key={`${f.at}:${f.fromPct}`} f={f} names={names} />)}
      {!trip.gpx ? <Note>{t('No GPS track - car data only.')}</Note> : null}
      {problem ? <Note tone="bad">{problem}</Note> : null}
    </Pressable>
  );
}

// A fill-up on its drive's card: the levels, the litres they come to and
// where. The litres are the levels' difference over the tank, so roughly:
// the car reads the level in steps.
function FillLine({ f, names }: { f: Refuel; names: Names }) {
  const ui = useUi();
  const styles = useStyles();
  const place = endName(f.pt, f.place ?? null, names);
  const litres = Math.round(((f.toPct - f.fromPct) / 100) * TANK_L);
  return (
    <View style={styles.fuel}>
      <Text style={styles.fuelTitle}>
        {t('Filled up')}
        <Text style={styles.fuelFacts}>
          {`  ·  ${Math.round(f.fromPct)} → ${Math.round(f.toPct)} %  ·  ≈ ${litres} l`}
        </Text>
      </Text>
      {place ? <Text style={ui.note}>{place}</Text> : null}
    </View>
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
    // A fill-up in a drive's card, with a rule of the accent.
    fuel: { borderLeftWidth: 3, borderLeftColor: C.accent, paddingLeft: S.sm },
    fuelTitle: { color: C.text, fontSize: 14, fontWeight: '600' },
    fuelFacts: { color: C.dim, fontWeight: '400' },
    // A link, not a card: 48 dp tall to hit, styled like the app's other links.
    more: { minHeight: 48, justifyContent: 'center' },
    moreText: { color: C.link, fontSize: 14 },
  }),
);
