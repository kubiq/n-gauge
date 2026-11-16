import { router, Stack, useFocusEffect, useLocalSearchParams } from 'expo-router';
import { useCallback, useEffect, useState } from 'react';
import { Alert } from 'react-native';

import { DriveChart } from '@/components/drive-chart';
import { HeaderActions } from '@/components/pod-chip';
import { MapLoading, TrackMap } from '@/components/track-map';
import { Card, Note, Row, Screen } from '@/components/ui';
import { usePod } from '@/hooks/use-pod';
import { useSync } from '@/hooks/use-sync';
import { t } from '@/i18n';
import { duration, unfinishedText, when } from '@/lib/format';
import { distance, speed } from '@/lib/units';
import { cachedCsv, cachedGpx, deleteDrive, type GpxStats, seriesOf, summarise, trackOf, tripFiles } from '@/lib/trip-cache';
import type { Series } from '@/lib/series';
import { LogKeyError } from '@/lib/logtext';
import { endName, loadNames, type Names, tripName } from '@/lib/names';
import { loadFinished, loadMerges, saveFinished, saveMerges } from '@/lib/store';
import { saveLogsToFolder, savedText, shareJoinedGpx, shareLog } from '@/lib/share';
import { type CsvSummary, litresPer100, pointAt, type TrackPoint, type TripFiles, unfinished } from '@/lib/trips';
import { entriesOf, mergeCsv, mergeGpx, mergeSeries, mergeTracks, type PartEnd, withoutMerge } from '@/lib/merge';

export default function TripScreen() {
  const { key } = useLocalSearchParams<{ key: string }>();
  // One drive, or several merged (merge.ts): found by any part's key. A
  // merged one stands for its oldest part, as in the list.
  const [merges, setMerges] = useState(loadMerges);
  const entry = entriesOf(tripFiles(), merges).find((e) => e.parts.some((p) => p.key === key));
  const parts = entry?.parts ?? [];
  const merged = parts.length > 1;
  const trip: TripFiles | undefined = !entry
    ? undefined
    : merged
      ? { key: entry.key, gpx: parts.find((p) => p.gpx)?.gpx, csv: parts.find((p) => p.csv)?.csv, start: parts[0].start }
      : parts[0];
  const partKeys = parts.map((p) => `${p.gpx}|${p.csv}`).join(',');
  const [track, setTrack] = useState<TrackPoint[] | null>(null);
  const [ready, setReady] = useState(false);
  // In state, not read from the cache during render - see trips.tsx.
  const [g, setG] = useState<GpxStats | null>(null);
  const [c, setC] = useState<CsvSummary | null>(null);
  // The last part's own: whether the drive ended (its logs').
  const [endG, setEndG] = useState<GpxStats | null>(null);
  const [endC, setEndC] = useState<CsvSummary | null>(null);
  // Merged: the parts' own times summed - the driving, not the stops (as the list).
  const [partsSecs, setPartsSecs] = useState<number | null>(null);
  const [series, setSeries] = useState<Series | null>(null);
  // Where the car was at the charts' cursor: a dot on the map. The chart
  // gives the moment in UTC, the track its seconds from the GPX's start.
  const [mark, setMark] = useState<[number, number] | null>(null);
  const onCursor = (utcMs: number | null) => {
    const p = utcMs !== null && g?.start && track ? pointAt(track, (utcMs - g.start) / 1000) : null;
    setMark(p ? [p[0], p[1]] : null);
  };
  const [busy, setBusy] = useState<string | null>(null);
  const connected = usePod().state === 'connected';
  const sync = useSync();
  // Re-read on focus: back from naming (name.tsx), the new name shows.
  const [names, setNames] = useState<Names>({ places: [], titles: {} });
  useFocusEffect(useCallback(() => setNames(loadNames()), []));

  const run = async (what: string, fn: () => Promise<void>) => {
    setBusy(what);
    try {
      await fn();
    } catch (e) {
      Alert.alert(t('Files'), String((e as Error).message));
    } finally {
      setBusy(null);
    }
  };
  const [problem, setProblem] = useState<string | null>(null);
  // Marked as finished here (store.ts): the note goes, in the list too.
  const [finished, setFinished] = useState<ReadonlySet<string>>(loadFinished);

  useEffect(() => {
    if (!parts.length) return;
    let live = true;
    (async () => {
      // Opened before the list got to it: work it out here. Each file on its
      // own, so a track that will not open still leaves the car data shown.
      const issues: string[] = [];
      const attempt = async (name?: string) => {
        if (!name) return;
        try {
          await summarise(name);
        } catch (e) {
          issues.push(e instanceof LogKeyError ? e.message : `${name}: ${String(e)}`);
        }
      };
      const gs: GpxStats[] = [], cs: CsvSummary[] = [], tracks: { track: TrackPoint[]; start: number | null }[] = [], ses: Series[] = [];
      const ends: PartEnd[] = [];   // each CSV part's end, for a fill-up between parts
      let lastG: GpxStats | null = null, lastC: CsvSummary | null = null;
      let sumSecs: number | null = null;
      for (const p of parts) {
        await attempt(p.gpx);
        await attempt(p.csv);
        const pg = p.gpx ? cachedGpx(p.gpx) : null;
        const pc = p.csv ? cachedCsv(p.csv) : null;
        const ps = pg?.start && pg.end ? (pg.end - pg.start) / 1000 : (pc?.durationS ?? null);
        if (ps !== null) sumSecs = (sumSecs ?? 0) + ps;
        if (pg) {
          gs.push(pg);
          const tr = await trackOf(p.gpx!);
          if (tr) tracks.push({ track: tr, start: pg.start });
        }
        if (pc) {
          cs.push(pc);
          ends.push({ at: pg?.end ?? null, pt: pg?.to ?? null, place: pg?.toPlace ?? null });
          // Cached with the summary; a log from before the charts gets it now, once.
          const se = await seriesOf(p.csv!).catch(() => null);
          if (se) ses.push(se);
        }
        lastG = pg;
        lastC = pc;
      }
      if (live) {
        // One drive as it is; merged, its parts combined (merge.ts).
        setG(gs.length ? (merged ? mergeGpx(gs) : gs[0]) : null);
        setC(cs.length ? (merged ? mergeCsv(cs, ends) : cs[0]) : null);
        setTrack(tracks.length ? (merged ? mergeTracks(tracks) : tracks[0].track) : null);
        setSeries(ses.length ? (merged ? mergeSeries(ses) : ses[0]) : null);
        setEndG(lastG);
        setEndC(lastC);
        setPartsSecs(sumSecs);
        setProblem(issues.length ? [...new Set(issues)].join('\n') : null);
        setReady(true);
      }
    })();
    return () => {
      live = false;
    };
  }, [partKeys]); // eslint-disable-line react-hooks/exhaustive-deps

  if (!trip) {
    return (
      <Screen>
        <Note tone="bad">{t('That drive is no longer on the phone.')}</Note>
      </Screen>
    );
  }

  const start = g?.start ?? trip.start;
  // The whole span; merged, that is with the stops - the driving time is
  // the parts' summed.
  const span = g?.start && g.end ? (g.end - g.start) / 1000 : (c?.durationS ?? null);
  const secs = merged ? partsSecs : span;
  const per100 = c ? litresPer100(c) : null;
  const name = tripName(trip, g, names);
  // Every file of it - all its parts' - and the last part's, which say whether it ended.
  const allFiles = parts.flatMap((p) => [p.gpx, p.csv]).filter((n): n is string => !!n);
  const last = parts[parts.length - 1];
  const files = [last.gpx, last.csv].filter((n): n is string => !!n);
  const marked = files.some((n) => finished.has(n));
  const cutOff = ready && unfinished(endG, endC);
  // A drive whose logs did not end (trips.ts): you can say it is all there
  // is - the power went, and nothing more is coming - or take that back.
  const finishItems =
    cutOff
      ? [
          marked
            ? { label: t('Mark as not finished'), onPress: () => setFinished(saveFinished(files, false)) }
            : { label: t('Mark as finished'), onPress: () => setFinished(saveFinished(files, true)) },
        ]
      : [];
  // Naming, in the modal (name.tsx): the drive itself, or either end as a
  // place of yours - every drive from or to there is then called by it.
  const nameItems = [
    { label: t('Rename this drive'), onPress: () => router.push({ pathname: '/name', params: { kind: 'trip', key: trip.key, current: name ?? '' } }) },
    ...placeItem(t('Name the start place'), g?.from, g?.fromPlace),
    ...placeItem(t('Name the end place'), g?.to, g?.toPlace),
  ];
  function placeItem(label: string, pt: [number, number] | null | undefined, listName: string | null | undefined) {
    if (!pt) return [];
    const current = endName(pt, listName ?? null, names) ?? '';
    return [
      {
        label,
        onPress: () =>
          router.push({ pathname: '/name', params: { kind: 'place', lat: String(pt[1]), lon: String(pt[0]), current } }),
      },
    ];
  }

  return (
    <Screen>
      <Stack.Screen
        options={{
          title: name ?? (start ? when(start) : t('Undated drive')),
          // The gauge, and ⋮: handing the drive on, decrypted (share.ts).
          headerRight: () => (
            <HeaderActions
              compact
              items={[
                ...nameItems,
                ...finishItems,
                // Merged: back into its drives, as they were.
                ...(merged
                  ? [
                      {
                        label: t('Split into its drives'),
                        onPress: () => {
                          const groups = withoutMerge(loadMerges(), parts);
                          saveMerges(groups);
                          setMerges(groups);
                          router.back();
                        },
                      },
                    ]
                  : []),
                // Merged, the track is the parts' joined, a segment each; the
                // car data stays per part - in the folder.
                ...(trip.gpx
                  ? [
                      {
                        label: t('Share the track (GPX)'),
                        onPress: () =>
                          run('gpx', () =>
                            merged ? shareJoinedGpx(parts.map((p) => p.gpx).filter((n): n is string => !!n)) : shareLog(trip.gpx!),
                          ),
                      },
                    ]
                  : []),
                ...(trip.csv && !merged ? [{ label: t('Share the car data (CSV)'), onPress: () => run('csv', () => shareLog(trip.csv!)) }] : []),
                {
                  label: t('Save to a folder'),
                  onPress: () =>
                    run('save', async () => {
                      const r = await saveLogsToFolder(allFiles);
                      Alert.alert(t('Saved'), savedText(r));
                    }),
                },
                {
                  // From the phone at once, and from the gauge's card by
                  // the sync - right now if connected, else on the next
                  // connect; it is never pulled back meanwhile (deleteDrive).
                  label: t('Delete this drive'),
                  onPress: () =>
                    Alert.alert(
                      t('Delete this drive?'),
                      connected
                        ? t('It is removed from this phone and from the gauge’s card.')
                        : t('It is removed from this phone now, and from the gauge’s card the next time the phone connects.'),
                      [
                        { text: t('Cancel'), style: 'cancel' },
                        {
                          text: t('Delete'),
                          style: 'destructive',
                          onPress: () => {
                            // Merged: every part, and the merge with them.
                            for (const p of parts) deleteDrive(p);
                            if (merged) saveMerges(withoutMerge(loadMerges(), parts));
                            if (connected) sync.sync();
                            router.back();
                          },
                        },
                      ],
                    ),
                },
              ].map((it) => ({ ...it, disabled: busy !== null }))}
            />
          ),
        }}
      />
      {/* A drive with a track: the map's box spins while it is read. */}
      {!ready ? trip.gpx ? <MapLoading /> : <Note>{t('Reading the logs…')}</Note> : null}
      {busy ? <Note>{busy === 'save' ? t('Saving…') : t('Preparing the file…')}</Note> : null}
      {problem ? <Note tone="bad">{problem}</Note> : null}
      {merged ? <Note>{t('Merged from {n} drives - its menu splits it again.', { n: parts.length })}</Note> : null}
      {cutOff && !marked ? <Note tone="bad">{unfinishedText(tripFiles()[0]?.key === last.key)}</Note> : null}
      {track && track.length > 1 && g?.bbox ? <TrackMap track={track} bbox={g.bbox} mark={mark} /> : null}

      {/* The drive's shape first - where (the map), then how (the charts) -
          and the figures after. */}
      {series ? <DriveChart series={series} onCursor={onCursor} /> : null}

      {g ? (
        <Card title={t('Drive')}>
          {/* The header carries the name; the date goes here then. */}
          {name && start ? <Row label={t('Started')} value={when(start)} /> : null}
          <Row label={t('Distance')} value={distance(g.distanceM)} />
          {secs !== null ? <Row label={t('Time')} value={duration(secs)} /> : null}
          {merged && span !== null ? <Row label={t('With the stops')} value={duration(span)} /> : null}
          <Row label={t('Moving')} value={duration(g.movingS)} />
          <Row label={t('Top speed (GPS)')} value={speed(g.maxKmh)} />
          {g.movingS > 0 ? <Row label={t('Average moving')} value={speed((g.distanceM / g.movingS) * 3.6)} /> : null}
        </Card>
      ) : null}

      {c ? (
        <Card title={t('Car')}>
          {!g && secs !== null ? <Row label={t('Time')} value={duration(secs)} /> : null}
          {c.maxBoostBar != null ? <Row label={t('Peak boost')} value={`${c.maxBoostBar.toFixed(2)} bar`} /> : null}
          {c.maxOilC != null ? <Row label={t('Peak oil')} value={`${Math.round(c.maxOilC)} °C`} /> : null}
          {c.lastOilC != null ? <Row label={t('Oil at the end')} value={`${Math.round(c.lastOilC)} °C`} /> : null}
          {c.maxCoolantC != null ? <Row label={t('Peak coolant')} value={`${Math.round(c.maxCoolantC)} °C`} /> : null}
          {c.maxRpm != null ? <Row label={t('Peak rpm')} value={String(Math.round(c.maxRpm))} /> : null}
          {/* The car's own figure reads a little different from GPS - both are shown, each named. */}
          {c.maxObdKmh != null ? <Row label={t('Top speed (speedometer)')} value={speed(c.maxObdKmh)} /> : null}
          {/* From the car's fuel rate over time, not the sloshing tank level. */}
          {c.fuelL != null && c.fuelL > 0 ? <Row label={t('Fuel used')} value={`${c.fuelL.toFixed(2)} l`} /> : null}
          {per100 !== null ? <Row label={t('Consumption')} value={`${per100.toFixed(1)} l/100 km`} /> : null}
        </Card>
      ) : null}

    </Screen>
  );
}
