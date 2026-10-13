import { router, useFocusEffect, useNavigation } from 'expo-router';
import { useCallback, useEffect, useLayoutEffect, useState } from 'react';
import { Alert } from 'react-native';

import { OverflowMenu } from '@/components/overflow-menu';
import { Button, Card, Note, Row, Screen } from '@/components/ui';
import { UploadCard } from '@/components/upload-card';
import { UPLOAD_ENABLED } from '@/features';
import { usePod } from '@/hooks/use-pod';
import { useSync } from '@/hooks/use-sync';
import { getLive, type PodLive, setClock, zeroGmeter } from '@/lib/pod';
import { scanPodQr } from '@/lib/scan-qr';
import { pendingBytes, pendingFiles, storedBytes } from '@/lib/store';
import { pullBytes } from '@/lib/sync-plan';

const LIVE_POLL_MS = 2000;
const mb = (n: number) => `${(n / 1048576).toFixed(n < 10 * 1048576 ? 1 : 0)} MB`;

// Everything about the gauge itself, opened from the gauge in the header
// (PodChip) - like a watch's page in Garmin Connect. The rest of the app is
// the phone's own: Trips, and Settings in its ⋮ menu.
export default function GaugeScreen() {
  const pod = usePod();
  const connected = pod.state === 'connected';
  const sync = useSync();
  const syncing = sync.phase !== 'idle';
  const [live, setLive] = useState<PodLive | null>(null);
  const [liveErr, setLiveErr] = useState<string | null>(null);
  const [busy, setBusy] = useState<string | null>(null);

  // A sync on every visit too: a drive log grows while the engine runs, and
  // connecting is not the only time something new can be there.
  const syncNow = sync.sync;
  useFocusEffect(
    useCallback(() => {
      if (connected) syncNow();
    }, [connected, syncNow]),
  );

  // The gauge serves one request at a time: while a sync talks to it, a poll
  // would only queue behind a multi-MB download and time out.
  useEffect(() => {
    // The live card is only rendered while connected, so a stale reading from
    // the last connection is never shown long: the first tick replaces it.
    if (!connected || syncing) return;
    let stop = false;
    const tick = async () => {
      try {
        const l = await getLive();
        if (!stop) {
          setLive(l);
          setLiveErr(null);
        }
      } catch (e) {
        if (!stop) setLiveErr(String((e as Error).message));
      }
    };
    tick();
    const t = setInterval(tick, LIVE_POLL_MS);
    return () => {
      stop = true;
      clearInterval(t);
    };
  }, [connected, syncing]);

  const scan = async () => {
    const r = await scanPodQr();
    if (r.kind === 'ok') await pod.setCreds(r.creds);
    else if (r.kind === 'wrong') Alert.alert('Not the gauge’s code', 'Scan the QR on the gauge’s WiFi screen.');
    else if (r.kind === 'unavailable') router.push('/scan');
  };

  // ⋮ in the header: the rare things. Scanning another gauge replaces the
  // stored one, so it is not a button on the screen.
  const navigation = useNavigation();
  const hasCreds = !!pod.creds;
  useLayoutEffect(() => {
    navigation.setOptions({
      headerRight: () => (
        <OverflowMenu items={hasCreds ? [{ label: 'Scan a different gauge', onPress: scan }] : []} />
      ),
    });
  }, [navigation, hasCreds]); // eslint-disable-line react-hooks/exhaustive-deps

  const run = async (what: string, fn: () => Promise<string>) => {
    setBusy(what);
    try {
      Alert.alert(what, await fn());
    } catch (e) {
      Alert.alert(what, String((e as Error).message));
    } finally {
      setBusy(null);
    }
  };

  if (pod.ready && !pod.creds) {
    return (
      <Screen>
        <Card title="Set up">
          <Note>
            Tap through the views on the gauge to the WiFi screen, then scan its QR code. The app keeps the password,
            so this is needed once.
          </Note>
          <Button title="Scan the gauge’s QR" onPress={scan} />
        </Card>
      </Screen>
    );
  }

  return (
    <Screen>
      <Card title="Connection">
        <Row label="Network" value={pod.creds?.ssid ?? ''} />
        {connected ? (
          <Button title="Disconnect" kind="plain" onPress={pod.disconnect} />
        ) : (
          <Button title="Connect" busy={pod.state === 'connecting'} onPress={pod.connect} />
        )}
        {pod.error ? <Note tone="bad">{pod.error}</Note> : null}
      </Card>

      <Card title="Logs">
        {UPLOAD_ENABLED ? (
          <Row label="Waiting for upload" value={`${pendingFiles(sync.inbox)} files, ${mb(pendingBytes(sync.inbox))}`} />
        ) : (
          <Row label="On the phone" value={`${Object.keys(sync.inbox).length} files, ${mb(storedBytes(sync.inbox))}`} />
        )}
        {connected && sync.plan && sync.podFiles ? (
          <>
            <Row label="On the gauge" value={`${sync.podFiles.length} files`} />
            <Row
              label="New or grown"
              value={sync.plan.length ? `${sync.plan.length} files, ${mb(pullBytes(sync.plan))}` : 'up to date'}
              tone={sync.plan.length ? undefined : 'good'}
            />
            {sync.plan.some((p) => p.restart) ? (
              <Note tone="bad">Some files are smaller than the copies here; they will be fetched again whole.</Note>
            ) : null}
          </>
        ) : null}
        {!connected ? <Note>New logs are pulled by themselves whenever the phone connects.</Note> : null}
        {connected && sync.phase === 'pulling' ? <Button title="Cancel" kind="danger" onPress={sync.cancel} /> : null}
        {connected && sync.phase !== 'pulling' ? (
          <Button
            title={sync.plan?.length ? 'Pull' : 'Check again'}
            kind={sync.plan?.length ? undefined : 'plain'}
            busy={sync.phase === 'checking'}
            disabled={syncing}
            onPress={sync.sync}
          />
        ) : null}
        {connected && sync.status ? <Note>{sync.status}</Note> : null}
        {sync.err ? <Note tone="bad">{sync.err}</Note> : null}
      </Card>

      {connected ? (
        <>
          <Card title="Live">
            {syncing ? <Note>Paused while the logs sync.</Note> : null}
            {liveErr && !syncing ? <Note tone="bad">{liveErr}</Note> : null}
            {live ? <LiveRows l={live} /> : syncing ? null : <Note>Reading…</Note>}
          </Card>
          <Card title="Actions">
            <Button
              title="Set the gauge’s clock from this phone"
              kind="plain"
              busy={busy === 'Clock'}
              onPress={() => run('Clock', setClock)}
            />
            <Button
              title="Zero the g-meter here"
              kind="plain"
              busy={busy === 'G-meter'}
              onPress={() =>
                Alert.alert('Zero the g-meter?', 'Only with the car level and standing still.', [
                  { text: 'Cancel', style: 'cancel' },
                  { text: 'Zero', onPress: () => run('G-meter', zeroGmeter) },
                ])
              }
            />
          </Card>
        </>
      ) : UPLOAD_ENABLED && pod.state === 'idle' ? (
        // Off the gauge's network is the only time the app can reach the server.
        <UploadCard />
      ) : null}
    </Screen>
  );
}

// can.linkUp is only "the TWAI driver started" (obd.h) - true on the bench
// with nothing on the bus. /api/live has no frame age, so "the car is
// answering" is read from any reading being fresh, as the gauge shows them.
function carTalking(l: PodLive): boolean {
  return l.can.haveOil || l.can.haveCoolant || l.can.haveBoost;
}

function canText(l: PodLive): string {
  if (!l.can.linkUp) return 'CAN driver failed';
  if (l.iso.busOff) return 'bus off';
  return carTalking(l) ? 'answering' : 'no answer';
}

function LiveRows({ l }: { l: PodLive }) {
  const yes = (b: boolean): 'good' | 'bad' => (b ? 'good' : 'bad');
  return (
    <>
      <Row label="Car" value={canText(l)} tone={yes(carTalking(l))} />
      <Row label="Oil" value={l.can.haveOil ? `${l.can.oilTempC} °C` : '—'} />
      <Row label="Coolant" value={l.can.haveCoolant ? `${l.can.coolantC} °C` : '—'} />
      <Row label="Boost" value={l.can.haveBoost ? `${l.can.boostBar.toFixed(2)} bar` : '—'} />
      <Row label="ISO-TP ok / aborts" value={`${l.iso.ok} / ${l.iso.aborts}`} tone={l.iso.aborts ? 'bad' : undefined} />
      <Row
        label="GPS"
        value={!l.gps.linkUp ? 'no module' : l.gps.haveFix ? `fix, ${l.gps.sats} sats, hdop ${l.gps.hdop}` : `no fix, ${l.gps.sats} sats`}
        tone={yes(l.gps.linkUp && l.gps.haveFix)}
      />
      <Row label="Clock" value={l.clock.valid ? l.clock.local : 'not set'} tone={yes(l.clock.valid)} />
      <Row label="Up" value={`${Math.round(l.uptimeMs / 60000)} min`} tone="dim" />
    </>
  );
}
