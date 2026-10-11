import { router, useFocusEffect } from 'expo-router';
import { useCallback, useRef, useState } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';

import { Card, Note, Screen, styles as ui } from '@/components/ui';
import { duration, km, when } from '@/lib/format';
import { LogKeyError } from '@/lib/logtext';
import { cachedCsv, cachedGpx, type GpxStats, summarise, tripFiles } from '@/lib/trip-cache';
import { type CsvSummary, isGpxName, type TripFiles } from '@/lib/trips';
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
  const working = useRef(false);

  useFocusEffect(
    useCallback(() => {
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
      setStatus(todo.length ? null : `${Object.keys(have).length} files, all from the cache`);
      if (working.current || !todo.length) return;
      working.current = true;
      (async () => {
        for (const [i, name] of todo.entries()) {
          setStatus(`Reading ${i + 1} of ${todo.length} (${Object.keys(have).length} cached): ${name}`);
          try {
            await summarise(name);
            const s = isGpxName(name) ? cachedGpx(name) : cachedCsv(name);
            if (s) setSums((prev) => ({ ...prev, [name]: s }));
          } catch (e) {
            const msg = e instanceof LogKeyError ? e.message : `Could not read it: ${String(e)}`;
            setProblems((prev) => ({ ...prev, [name]: msg }));
          }
        }
        setStatus(`Read ${todo.length} new, ${Object.keys(have).length} from the cache`);
        working.current = false;
      })();
    }, []),
  );

  return (
    <Screen>
      {status ? <Note>{status}</Note> : null}
      {trips.length === 0 ? (
        <Card>
          <Note>No drives on the phone yet. Connect to the pod and pull them on the Sync tab.</Note>
        </Card>
      ) : (
        trips.map((t) => (
          <TripRow
            key={t.key}
            t={t}
            g={t.gpx ? (sums[t.gpx] as GpxStats | undefined) : undefined}
            c={t.csv ? (sums[t.csv] as CsvSummary | undefined) : undefined}
            problem={(t.gpx && problems[t.gpx]) || (t.csv && problems[t.csv]) || undefined}
          />
        ))
      )}
    </Screen>
  );
}

function TripRow({ t, g, c, problem }: { t: TripFiles; g?: GpxStats; c?: CsvSummary; problem?: string }) {
  const start = g?.start ?? t.start;
  const secs = g?.start && g.end ? (g.end - g.start) / 1000 : (c?.durationS ?? null);
  const pending = !problem && ((t.gpx && !g) || (t.csv && !c));
  return (
    <Pressable
      onPress={() => router.push({ pathname: '/trip/[key]', params: { key: t.key } })}
      style={({ pressed }) => [ui.card, pressed && { opacity: 0.7 }]}>
      <View style={styles.head}>
        <Text style={styles.title}>{start ? when(start) : t.key}</Text>
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
  head: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'baseline' },
  title: { color: C.text, fontSize: 16, fontWeight: '600' },
  big: { color: C.accent, fontSize: 18, fontWeight: '700' },
  facts: { flexDirection: 'row', flexWrap: 'wrap', gap: S.lg },
});
