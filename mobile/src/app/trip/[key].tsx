import { Stack, useLocalSearchParams } from 'expo-router';
import { useEffect, useState } from 'react';
import { Alert } from 'react-native';

import { TrackMap } from '@/components/track-map';
import { Button, Card, Note, Row, Screen } from '@/components/ui';
import { duration, km, when } from '@/lib/format';
import { cachedCsv, cachedGpx, type GpxStats, summarise, trackOf, tripFiles } from '@/lib/trip-cache';
import { LogKeyError } from '@/lib/logtext';
import { shareGpx } from '@/lib/share';
import type { CsvSummary, TrackPoint } from '@/lib/trips';

export default function TripScreen() {
  const { key } = useLocalSearchParams<{ key: string }>();
  const t = tripFiles().find((x) => x.key === key);
  const [track, setTrack] = useState<TrackPoint[] | null>(null);
  const [ready, setReady] = useState(false);
  // In state, not read from the cache during render - see trips.tsx.
  const [g, setG] = useState<GpxStats | null>(null);
  const [c, setC] = useState<CsvSummary | null>(null);
  const [sharing, setSharing] = useState(false);
  const [problem, setProblem] = useState<string | null>(null);

  useEffect(() => {
    if (!t) return;
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
      await attempt(t.gpx);
      await attempt(t.csv);
      const tr = t.gpx && cachedGpx(t.gpx) ? await trackOf(t.gpx) : null;
      if (live) {
        setG(t.gpx ? cachedGpx(t.gpx) : null);
        setC(t.csv ? cachedCsv(t.csv) : null);
        setTrack(tr);
        setProblem(issues.length ? [...new Set(issues)].join('\n') : null);
        setReady(true);
      }
    })();
    return () => {
      live = false;
    };
  }, [t?.gpx, t?.csv]); // eslint-disable-line react-hooks/exhaustive-deps

  if (!t) {
    return (
      <Screen underHeader>
        <Note tone="bad">That drive is no longer on the phone.</Note>
      </Screen>
    );
  }

  const start = g?.start ?? t.start;
  const secs = g?.start && g.end ? (g.end - g.start) / 1000 : (c?.durationS ?? null);

  return (
    <Screen underHeader>
      <Stack.Screen options={{ title: start ? when(start) : t.key }} />
      {!ready ? <Note>Reading the logs…</Note> : null}
      {problem ? <Note tone="bad">{problem}</Note> : null}
      {track && track.length > 1 && g?.bbox ? <TrackMap track={track} bbox={g.bbox} /> : null}

      {g ? (
        <Card title="Drive">
          <Row label="Distance" value={km(g.distanceM)} />
          {secs !== null ? <Row label="Time" value={duration(secs)} /> : null}
          <Row label="Moving" value={duration(g.movingS)} />
          <Row label="Top speed" value={`${Math.round(g.maxKmh)} km/h`} />
          {g.movingS > 0 ? <Row label="Average moving" value={`${Math.round((g.distanceM / g.movingS) * 3.6)} km/h`} /> : null}
        </Card>
      ) : null}

      {c ? (
        <Card title="Car">
          {!g && secs !== null ? <Row label="Time" value={duration(secs)} /> : null}
          {c.maxBoostBar != null ? <Row label="Peak boost" value={`${c.maxBoostBar.toFixed(2)} bar`} /> : null}
          {c.maxOilC != null ? <Row label="Peak oil" value={`${Math.round(c.maxOilC)} °C`} /> : null}
          {c.lastOilC != null ? <Row label="Oil at the end" value={`${Math.round(c.lastOilC)} °C`} /> : null}
          {c.maxCoolantC != null ? <Row label="Peak coolant" value={`${Math.round(c.maxCoolantC)} °C`} /> : null}
          {c.maxRpm != null ? <Row label="Peak rpm" value={String(Math.round(c.maxRpm))} /> : null}
          {c.maxObdKmh != null ? <Row label="Top speed (car)" value={`${Math.round(c.maxObdKmh)} km/h`} /> : null}
        </Card>
      ) : null}

      <Card title="Files">
        {t.gpx ? <Row label="Track" value={t.gpx} tone="dim" /> : null}
        {t.csv ? <Row label="Car data" value={t.csv} tone="dim" /> : null}
        {t.gpx ? (
          <Button
            title="Share the track (GPX)"
            kind="plain"
            busy={sharing}
            onPress={async () => {
              setSharing(true);
              try {
                await shareGpx(t.gpx!);
              } catch (e) {
                Alert.alert('Share', String((e as Error).message));
              } finally {
                setSharing(false);
              }
            }}
          />
        ) : null}
      </Card>
    </Screen>
  );
}
