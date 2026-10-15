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
import { duration, km, when } from '@/lib/format';
import { cachedCsv, cachedGpx, deleteDrive, type GpxStats, seriesOf, summarise, trackOf, tripFiles } from '@/lib/trip-cache';
import type { Series } from '@/lib/series';
import { LogKeyError } from '@/lib/logtext';
import { endName, loadNames, type Names, tripName } from '@/lib/names';
import { saveLogsToFolder, savedText, shareLog } from '@/lib/share';
import { type CsvSummary, pointAt, type TrackPoint } from '@/lib/trips';

export default function TripScreen() {
  const { key } = useLocalSearchParams<{ key: string }>();
  const trip = tripFiles().find((x) => x.key === key);
  const [track, setTrack] = useState<TrackPoint[] | null>(null);
  const [ready, setReady] = useState(false);
  // In state, not read from the cache during render - see trips.tsx.
  const [g, setG] = useState<GpxStats | null>(null);
  const [c, setC] = useState<CsvSummary | null>(null);
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

  useEffect(() => {
    if (!trip) return;
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
      await attempt(trip.gpx);
      await attempt(trip.csv);
      const tr = trip.gpx && cachedGpx(trip.gpx) ? await trackOf(trip.gpx) : null;
      // Cached with the summary; a log from before the charts gets it now, once.
      const se = trip.csv && cachedCsv(trip.csv) ? await seriesOf(trip.csv).catch(() => null) : null;
      if (live) {
        setG(trip.gpx ? cachedGpx(trip.gpx) : null);
        setC(trip.csv ? cachedCsv(trip.csv) : null);
        setTrack(tr);
        setSeries(se);
        setProblem(issues.length ? [...new Set(issues)].join('\n') : null);
        setReady(true);
      }
    })();
    return () => {
      live = false;
    };
  }, [trip?.gpx, trip?.csv]); // eslint-disable-line react-hooks/exhaustive-deps

  if (!trip) {
    return (
      <Screen>
        <Note tone="bad">{t('That drive is no longer on the phone.')}</Note>
      </Screen>
    );
  }

  const start = g?.start ?? trip.start;
  const secs = g?.start && g.end ? (g.end - g.start) / 1000 : (c?.durationS ?? null);
  const name = tripName(trip, g, names);
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
                ...(trip.gpx ? [{ label: t('Share the track (GPX)'), onPress: () => run('gpx', () => shareLog(trip.gpx!)) }] : []),
                ...(trip.csv ? [{ label: t('Share the car data (CSV)'), onPress: () => run('csv', () => shareLog(trip.csv!)) }] : []),
                {
                  label: t('Save to a folder'),
                  onPress: () =>
                    run('save', async () => {
                      const r = await saveLogsToFolder([trip.gpx, trip.csv].filter((n): n is string => !!n));
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
                            deleteDrive(trip);
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
      {track && track.length > 1 && g?.bbox ? <TrackMap track={track} bbox={g.bbox} mark={mark} /> : null}

      {/* The drive's shape first - where (the map), then how (the charts) -
          and the figures after. */}
      {series ? <DriveChart series={series} onCursor={onCursor} /> : null}

      {g ? (
        <Card title={t('Drive')}>
          {/* The header carries the name; the date goes here then. */}
          {name && start ? <Row label={t('Started')} value={when(start)} /> : null}
          <Row label={t('Distance')} value={km(g.distanceM)} />
          {secs !== null ? <Row label={t('Time')} value={duration(secs)} /> : null}
          <Row label={t('Moving')} value={duration(g.movingS)} />
          <Row label={t('Top speed (GPS)')} value={`${Math.round(g.maxKmh)} km/h`} />
          {g.movingS > 0 ? <Row label={t('Average moving')} value={`${Math.round((g.distanceM / g.movingS) * 3.6)} km/h`} /> : null}
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
          {c.maxObdKmh != null ? <Row label={t('Top speed (speedometer)')} value={`${Math.round(c.maxObdKmh)} km/h`} /> : null}
        </Card>
      ) : null}

    </Screen>
  );
}
