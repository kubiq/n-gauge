import { router, useNavigation } from 'expo-router';
import { useEffect, useLayoutEffect, useState } from 'react';
import { Alert } from 'react-native';

import { OverflowMenu } from '@/components/overflow-menu';
import { Button, Card, Note, Row, Screen } from '@/components/ui';
import { usePod } from '@/hooks/use-pod';
import { getLive, type PodLive, setClock, zeroGmeter } from '@/lib/pod';
import { scanPodQr } from '@/lib/scan-qr';
import { UploadCard } from '@/components/upload-card';
import { UPLOAD_ENABLED } from '@/features';

const LIVE_POLL_MS = 2000;

export default function PodScreen() {
  const pod = usePod();
  const [live, setLive] = useState<PodLive | null>(null);
  const [liveErr, setLiveErr] = useState<string | null>(null);
  const [busy, setBusy] = useState<string | null>(null);

  useEffect(() => {
    // The live card is only rendered while connected, so a stale reading from
    // the last connection is never shown long: the first tick replaces it.
    if (pod.state !== 'connected') return;
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
  }, [pod.state]);

  const scan = async () => {
    const r = await scanPodQr();
    if (r.kind === 'ok') await pod.setCreds(r.creds);
    else if (r.kind === 'wrong') Alert.alert('Not the pod’s code', 'Scan the QR on the pod’s WiFi screen.');
    else if (r.kind === 'unavailable') router.push('/scan');
  };

  // ⋮ in the header: the rare things. Scanning another pod replaces the
  // stored one, so it is not a button on the screen any more.
  const navigation = useNavigation();
  const hasCreds = !!pod.creds;
  useLayoutEffect(() => {
    navigation.setOptions({
      headerRight: () => (
        <OverflowMenu items={hasCreds ? [{ label: 'Scan a different pod', onPress: scan }] : []} />
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

  return (
    <Screen tabBar>
      <Card title="Connection">
        {!pod.ready ? null : pod.creds ? (
          <>
            <Row label="Network" value={pod.creds.ssid} />
            <Row
              label="Status"
              value={pod.state === 'connected' ? 'connected' : pod.state === 'connecting' ? 'connecting…' : 'not connected'}
              tone={pod.state === 'connected' ? 'good' : 'dim'}
            />
            {pod.state === 'connected' ? (
              <Button title="Disconnect" kind="plain" onPress={pod.disconnect} />
            ) : (
              <Button title="Connect to pod" busy={pod.state === 'connecting'} onPress={pod.connect} />
            )}
            {pod.error ? <Note tone="bad">{pod.error}</Note> : null}
          </>
        ) : (
          <>
            <Note>
              Tap through the views on the gauge to the WiFi screen, then scan its QR code. The app keeps the
              password, so this is needed once.
            </Note>
            <Button title="Scan the pod’s QR" onPress={scan} />
          </>
        )}
      </Card>

      {pod.state === 'connected' ? (
        <>
          <Card title="Live">
            {liveErr ? <Note tone="bad">{liveErr}</Note> : null}
            {live ? <LiveRows l={live} /> : <Note>Reading…</Note>}
          </Card>
          <Card title="Actions">
            <Button
              title="Set the pod’s clock from this phone"
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
        // Off the pod's network is the only time the app can reach the server.
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
