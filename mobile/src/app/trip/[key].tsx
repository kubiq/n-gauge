import { router, Stack, useLocalSearchParams } from 'expo-router';
import { useEffect, useState } from 'react';
import { Alert } from 'react-native';

import { HeaderActions } from '@/components/pod-chip';
import { TrackMap } from '@/components/track-map';
import { Card, Note, Row, Screen } from '@/components/ui';
import { duration, km, when } from '@/lib/format';
import { cachedCsv, cachedGpx, deleteDrive, type GpxStats, summarise, trackOf, tripFiles } from '@/lib/trip-cache';
import { LogKeyError } from '@/lib/logtext';
import { saveLogsToFolder, savedText, shareLog } from '@/lib/share';
import { type CsvSummary, isEncryptedName, type TrackPoint } from '@/lib/trips';

export default function TripScreen() {
  const { key } = useLocalSearchParams<{ key: string }>();
  const t = tripFiles().find((x) => x.key === key);
  const [track, setTrack] = useState<TrackPoint[] | null>(null);
  const [ready, setReady] = useState(false);
  // In state, not read from the cache during render - see trips.tsx.
  const [g, setG] = useState<GpxStats | null>(null);
  const [c, setC] = useState<CsvSummary | null>(null);
  const [busy, setBusy] = useState<string | null>(null);

  const run = async (what: string, fn: () => Promise<void>) => {
    setBusy(what);
    try {
      await fn();
    } catch (e) {
      Alert.alert('Files', String((e as Error).message));
    } finally {
      setBusy(null);
    }
  };
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
      <Screen>
        <Note tone="bad">That drive is no longer on the phone.</Note>
      </Screen>
    );
  }

  const start = g?.start ?? t.start;
  const secs = g?.start && g.end ? (g.end - g.start) / 1000 : (c?.durationS ?? null);

  return (
    <Screen>
      <Stack.Screen
        options={{
          title: start ? when(start) : t.key,
          // The gauge, and ⋮: handing the drive on, decrypted (share.ts).
          headerRight: () => (
            <HeaderActions
              compact
              items={[
                ...(t.gpx ? [{ label: 'Share the track (GPX)', onPress: () => run('gpx', () => shareLog(t.gpx!)) }] : []),
                ...(t.csv ? [{ label: 'Share the car data (CSV)', onPress: () => run('csv', () => shareLog(t.csv!)) }] : []),
                {
                  label: 'Save to a folder',
                  onPress: () =>
                    run('save', async () => {
                      const r = await saveLogsToFolder([t.gpx, t.csv].filter((n): n is string => !!n));
                      Alert.alert('Saved', savedText(r));
                    }),
                },
                {
                  // The phone's copy only: a log still on the gauge's card
                  // comes back with the next sync - the dialog says so.
                  label: 'Delete from this phone',
                  onPress: () =>
                    Alert.alert(
                      'Delete this drive?',
                      'It is removed from this phone. If it is still on the gauge’s card, the next sync brings it back.',
                      [
                        { text: 'Cancel', style: 'cancel' },
                        {
                          text: 'Delete',
                          style: 'destructive',
                          onPress: () => {
                            deleteDrive(t);
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
      {!ready ? <Note>Reading the logs…</Note> : null}
      {busy ? <Note>{busy === 'save' ? 'Saving…' : 'Preparing the file…'}</Note> : null}
      {problem ? <Note tone="bad">{problem}</Note> : null}
      {track && track.length > 1 && g?.bbox ? <TrackMap track={track} bbox={g.bbox} /> : null}

      {g ? (
        <Card title="Drive">
          <Row label="Distance" value={km(g.distanceM)} />
          {secs !== null ? <Row label="Time" value={duration(secs)} /> : null}
          <Row label="Moving" value={duration(g.movingS)} />
          <Row label="Top speed (GPS)" value={`${Math.round(g.maxKmh)} km/h`} />
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
          {/* The car's own figure reads a little different from GPS - both are shown, each named. */}
          {c.maxObdKmh != null ? <Row label="Top speed (speedometer)" value={`${Math.round(c.maxObdKmh)} km/h`} /> : null}
        </Card>
      ) : null}

      {/* One plain line instead of file names: what a driver needs is that the
          logs are protected and how to get them out. */}
      <Note>
        {isEncryptedName(t.gpx ?? t.csv ?? '')
          ? 'Encrypted by the gauge. ⋮ shares or saves it decrypted.'
          : '⋮ shares the track or the car data, or saves both to a folder.'}
      </Note>
    </Screen>
  );
}
