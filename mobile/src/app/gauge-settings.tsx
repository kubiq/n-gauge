import { router } from 'expo-router';
import { File } from 'expo-file-system';
import { useEffect, useRef, useState } from 'react';
import { Alert, Pressable, StyleSheet, Switch, Text, TextInput, View } from 'react-native';

import { ScreenOrderCard } from '@/components/screen-order-card';
import { SystemCard } from '@/components/system-card';
import { UploadCard } from '@/components/upload-card';
import { Button, Card, Note, Row, Screen, useUi } from '@/components/ui';
import { useHoldPod, usePod } from '@/hooks/use-pod';
import { useSync } from '@/hooks/use-sync';
import { N_, t } from '@/i18n';
import { clearReadyFw, loadReadyFw, type ReadyFw } from '@/lib/fw-download';
import { isNewer } from '@/lib/fw-release';
import { getLive, getSettings, type PodLive, saveSettings, setClock, uploadFirmware, zeroGmeter } from '@/lib/pod';
import { scanPodQr } from '@/lib/scan-qr';
import { checkValue, GROUP_TITLES, type PodSettings, type SettingDef, SETTINGS, saveForm } from '@/lib/settings-schema';
import { saveGaugeFw } from '@/lib/store';
import { setMph } from '@/lib/units';
import { S, themed, useColors } from '@/theme';

const GROUPS = [...new Set(SETTINGS.map((d) => d.group))];
// A change goes to the gauge this long after the last one: switches flipped
// together, a number typed digit by digit, go as one save.
const SAVE_AFTER_MS = 1000;

// The gauge's own screen, from the main ⋮ (under Settings, which is the
// phone's): setting it up, its settings and screen order, its firmware and
// system, and the few things done to it. Everything else about the gauge is
// a tap on it in the header - a sync.
//
// It connects by itself when opened, and holds the connection while it is
// open (useHoldPod): "Disconnect after syncing" waits for it to close.
export default function GaugeSettingsScreen() {
  const pod = usePod();
  useHoldPod();
  const connected = pod.state === 'connected';

  // Once per gauge set up: a failed connect stays failed, with its reason and
  // a Connect button, rather than retrying in a loop.
  const { creds, state, connect } = pod;
  const autoFor = useRef<object | null>(null);
  useEffect(() => {
    if (creds && state === 'idle' && autoFor.current !== creds) {
      autoFor.current = creds;
      connect();
    }
  }, [creds, state, connect]);
  // A release from GitHub, downloaded and checked while the app was open
  // with internet (fw-download.ts) - offered if newer than what the gauge
  // runs; dropped once installed, or when the gauge has it already.
  const [ready, setReady] = useState<ReadyFw | null>(loadReadyFw);

  const scan = async () => {
    const r = await scanPodQr();
    if (r.kind === 'ok') await pod.setCreds(r.creds);
    else if (r.kind === 'wrong') Alert.alert(t('Not the gauge’s code'), t('Scan the QR on the gauge’s WiFi screen.'));
    else if (r.kind === 'unavailable') router.push('/scan');
  };

  // Its firmware and system, read once a connection: nothing here needs them
  // live. The build is kept for Settings > About.
  const [live, setLive] = useState<PodLive | null>(null);

  const [orig, setOrig] = useState<PodSettings | null>(null);
  // Edits as typed: text for numbers (so "2." survives), booleans as is.
  const [edit, setEdit] = useState<Record<string, string | boolean>>({});
  const [err, setErr] = useState<string | null>(null);
  const [busy, setBusy] = useState<string | null>(null);

  // The actions' answer under them - a quiet line, not an alert to tap away.
  // `what` is English, marked N_, shown through t(); the answer is the
  // gauge's own, in English. Errors still alert.
  const [done, setDone] = useState<string | null>(null);
  const run = async (what: string, fn: () => Promise<string>) => {
    setBusy(what);
    setDone(null);
    try {
      setDone(`${t(what)}: ${await fn()}`);
    } catch (e) {
      Alert.alert(t(what), String((e as Error).message));
    } finally {
      setBusy(null);
    }
  };

  const load = () =>
    getSettings().then(
      (s) => {
        setOrig(s);
        setEdit({});
        setErr(null);
        // The app reads speeds and distances as the gauge is set (units.ts);
        // older firmware has no mph setting.
        if (typeof s.mph === 'boolean') setMph(s.mph);
      },
      (e: Error) => setErr(e.message),
    );

  // Fresh from the pod on every connect: it may have been changed from its own
  // pages, or be a different pod.
  useEffect(() => {
    if (!connected) return;
    load();
    getLive().then(
      (l) => {
        setLive(l);
        if (l.fw) {
          saveGaugeFw(l.fw);
          // A downloaded release the gauge has already, or something newer: gone.
          const r = loadReadyFw();
          if (r && !isNewer(r, l.fw)) {
            clearReadyFw();
            setReady(null);
          }
        }
      },
      () => setLive(null),
    );
  }, [connected]);

  const changes: Record<string, number | boolean> = {};
  const problems: string[] = [];
  for (const d of SETTINGS) {
    if (!(d.json in edit)) continue;
    const e = edit[d.json];
    const v = typeof e === 'boolean' ? e : Number(e.replace(',', '.'));
    if (typeof v === 'number') {
      const p = checkValue(d, v);
      if (p) {
        problems.push(`${t(d.label)}: ${p}`);
        continue;
      }
    }
    if (orig && v !== orig[d.json]) changes[d.json] = v;
  }
  // The screen order, edited in its card and saved with everything else.
  const [views, setViews] = useState<string | null>(null);
  const viewsChanged = views !== null && !!orig?.views && views !== orig.views;
  const nChanged = Object.keys(changes).length + (viewsChanged ? 1 : 0);

  // The gauge's answer to the last save, a quiet line under the settings:
  // "Saved.", or what went wrong.
  const [saved, setSaved] = useState<string | null>(null);

  // The pod serves one request at a time, and a download holds it for up to
  // two minutes: a running sync is stopped first, and picked up again after.
  const sync = useSync();

  // Saved by themselves (asked 2026-11-06), no Save button: SAVE_AFTER_MS
  // after the last change, everything changed and valid in one /save - a
  // number still being typed, or out of range, waits (it is marked). What
  // was sent leaves the edits once the gauge has it; anything changed
  // meanwhile stays, and goes with the next save.
  const save = async () => {
    const sent = { ...changes };
    const sentViews = viewsChanged ? views : null;
    setBusy('save');
    setSaved(null);
    try {
      await sync.cancel();
      const body = [saveForm(sent), sentViews ? `views=${sentViews}` : ''].filter(Boolean).join('&');
      setSaved(await saveSettings(body));
      const s = await getSettings();
      setOrig(s);
      if (typeof s.mph === 'boolean') setMph(s.mph);
      setEdit((e) => {
        const left = { ...e };
        for (const k of Object.keys(sent)) {
          const v = left[k];
          const now = typeof v === 'boolean' ? v : Number(String(v).replace(',', '.'));
          if (now === sent[k]) delete left[k];
        }
        return left;
      });
      if (sentViews) setViews((v) => (v === sentViews ? null : v));
    } catch (e) {
      setSaved(String((e as Error).message));
    } finally {
      setBusy(null);
      sync.sync();
    }
  };
  // Re-run on every change (changeKey), so the timer holds this render's
  // save() - the changes as they are now.
  const changeKey = JSON.stringify([changes, viewsChanged ? views : null]);
  useEffect(() => {
    if (!nChanged || busy) return;
    const timer = setTimeout(save, SAVE_AFTER_MS);
    return () => clearTimeout(timer);
  }, [changeKey, nChanged, busy]); // eslint-disable-line react-hooks/exhaustive-deps

  const firmware = async () => {
    const r = await File.pickFileAsync({ mimeTypes: ['application/octet-stream'] });
    if (r.canceled) return;
    const bin = r.result;
    if (!bin.name.endsWith('.bin')) {
      Alert.alert(t('Firmware'), t('{name} is not a .bin', { name: bin.name }));
      return;
    }
    Alert.alert(
      t('Update firmware?'),
      t('{name}, {size} MB. Takes ~30 s; the gauge restarts.', { name: bin.name, size: (bin.size / 1048576).toFixed(2) }),
      [
        { text: t('Cancel'), style: 'cancel' },
        {
          text: t('Update'),
          onPress: async () => {
            setBusy('fw');
            try {
              // No sync after: the pod restarts, and the reconnect syncs.
              await sync.cancel();
              Alert.alert(t('Firmware'), await uploadFirmware(bin));
            } catch (e) {
              Alert.alert(t('Firmware'), String((e as Error).message));
            } finally {
              setBusy(null);
            }
          },
        },
      ],
    );
  };

  const offer = ready && live?.fw && isNewer(ready, live.fw) ? ready : null;
  const installReady = (r: ReadyFw) =>
    Alert.alert(
      t('Update firmware?'),
      t('{tag} from GitHub, signed - checked against this gauge’s key. Takes ~30 s; the gauge restarts.', { tag: r.tag }),
      [
        { text: t('Cancel'), style: 'cancel' },
        {
          text: t('Update'),
          onPress: async () => {
            setBusy('fw');
            try {
              await sync.cancel();
              const said = await uploadFirmware(new File(r.uri));
              clearReadyFw();
              setReady(null);
              Alert.alert(t('Firmware'), said);
            } catch (e) {
              Alert.alert(t('Firmware'), String((e as Error).message));
            } finally {
              setBusy(null);
            }
          },
        },
      ],
    );

  if (pod.ready && !pod.creds) {
    return (
      <Screen>
        <Card title={t('Set up')}>
          <Note>
            {t(
              'Tap through the views on the gauge to the WiFi screen, then scan its QR code. The app keeps the password, so this is needed once.',
            )}
          </Note>
          <Button title={t('Scan the gauge’s QR')} onPress={scan} />
        </Card>
      </Screen>
    );
  }

  // Scanning another gauge replaces the stored one: rare, so last and plain.
  const another = <Button title={t('Scan a different gauge')} kind="plain" onPress={scan} />;

  if (!connected) {
    // The settings live on the gauge: nothing to show until it answers.
    return (
      <Screen>
        <Card title={pod.state === 'connecting' ? t('Connecting…') : t('Not connected')}>
          <Note>
            {t(
              'The settings are read from the gauge itself - it must be awake (car on, or a door opened). Meanwhile the phone has no internet.',
            )}
          </Note>
          <Button title={t('Connect')} busy={pod.state === 'connecting'} onPress={() => pod.connect()} />
          {pod.error ? <Note tone="bad">{pod.error}</Note> : null}
        </Card>
        {another}
      </Screen>
    );
  }

  return (
    <Screen>
      <Card title={t('Gauge')}>
        <Row label={t('Network')} value={pod.creds?.ssid ?? ''} />
        <Row label={t('Firmware')} value={live ? (live.fw?.build ?? t('older firmware')) : '…'} />
        {/* Leaving this screen lets go by itself; this is for "Disconnect
            after syncing" switched off. */}
        <Button title={t('Disconnect')} kind="plain" onPress={pod.disconnect} />
      </Card>
      {err ? <Note tone="bad">{err}</Note> : null}
      {orig ? null : err ? null : <Note>{t('Reading the gauge’s settings…')}</Note>}
      {orig
        ? GROUPS.map((g) => (
            <Card key={g} title={t(GROUP_TITLES[g])}>
              {SETTINGS.filter((d) => d.group === g).map((d) => (
                <SettingRow
                  key={d.key}
                  d={d}
                  value={d.json in edit ? edit[d.json] : orig[d.json]}
                  changed={d.json in changes}
                  onChange={(v) => setEdit((s) => ({ ...s, [d.json]: v }))}
                />
              ))}
            </Card>
          ))
        : null}
      {orig?.views ? (
        // Keyed by the gauge's value, so a reload after saving - or another
        // gauge - starts the card afresh.
        <ScreenOrderCard key={orig.views} views={orig.views} onChange={setViews} />
      ) : null}
      {orig ? (
        <>
          {problems.length ? <Note tone="bad">{problems.join('\n')}</Note> : null}
          {/* Saved by themselves; the gauge's answer, or that it is going. */}
          {busy === 'save' || nChanged ? (
            <Note>{t('Saving…')}</Note>
          ) : saved ? (
            <Note tone={saved.startsWith('Saved') ? 'good' : 'bad'}>{saved}</Note>
          ) : null}
        </>
      ) : null}
      <Card title={t('Actions')}>
        <Button
          title={t('Set the gauge’s clock from this phone')}
          kind="plain"
          busy={busy === 'Clock'}
          onPress={() => run(N_('Clock'), setClock)}
        />
        <Button
          title={t('Zero the g-meter here')}
          kind="plain"
          busy={busy === 'G-meter'}
          onPress={() =>
            Alert.alert(t('Zero the g-meter?'), t('Only with the car level and standing still.'), [
              { text: t('Cancel'), style: 'cancel' },
              { text: t('Zero'), onPress: () => run(N_('G-meter'), zeroGmeter) },
            ])
          }
        />
        {done ? <Note tone="good">{done}</Note> : null}
      </Card>
      <UploadCard />
      {live?.sys ? <SystemCard sys={live.sys} iso={live.iso} uptimeMs={live.uptimeMs} /> : null}
      {/* Last (asked 2026-11-06): not the first thing passed on the way to
          the other cards. */}
      <Card title={t('Firmware')}>
        {offer ? (
          <>
            {/* Its commit, as the gauge's own build stamp shows one: one
                hash for a build, not the image's too (asked 2026-11-11). */}
            <Row label={t('Newer on GitHub')} value={offer.commit ? `${offer.tag} · ${offer.commit}` : offer.tag} tone="good" />
            <Button title={t('Install {tag}', { tag: offer.tag })} busy={busy === 'fw'} onPress={() => installReady(offer)} />
          </>
        ) : null}
        <Note>
          {t(
            'Once a day, while the app is open with internet, it looks for a newer release on GitHub, checks its signature against this gauge’s key and keeps it until it is installed here.',
          )}
        </Note>
        <Note>{t('Or pick a car_gauge_v1 .bin from the phone’s files. If the new build fails, the gauge rolls back by itself.')}</Note>
        <Button title={t('Update firmware')} kind="plain" busy={busy === 'fw'} onPress={firmware} />
      </Card>
      {another}
    </Screen>
  );
}

function SettingRow({
  d,
  value,
  changed,
  onChange,
}: {
  d: SettingDef;
  value: string | number | boolean | undefined;
  changed: boolean;
  onChange: (v: string | boolean) => void;
}) {
  const C = useColors();
  const ui = useUi();
  const styles = useStyles();
  if (d.options) {
    // Label and hint above, the choices full width below: the labels are too
    // long to sit beside the name like a number field does.
    const current = value === undefined ? undefined : Number(value);
    return (
      <View style={{ gap: S.sm }}>
        <Text style={[ui.rowLabel, changed && { color: C.accent }]}>{t(d.label)}</Text>
        <View style={ui.options}>
          {d.options.map((o) => {
            const on = o.value === current;
            return (
              <Pressable
                key={o.value}
                onPress={() => onChange(String(o.value))}
                style={({ pressed }) => [ui.option, on && ui.optionOn, pressed && { opacity: 0.6 }]}>
                <View style={[ui.radio, on && ui.radioOn]} />
                <Text style={[ui.rowValue, on && { color: C.accent }]}>{t(o.label)}</Text>
              </Pressable>
            );
          })}
        </View>
        {d.hint ? <Text style={ui.note}>{t(d.hint)}</Text> : null}
      </View>
    );
  }
  return (
    <View style={ui.row}>
      <View style={{ flexShrink: 1 }}>
        <Text style={[ui.rowLabel, changed && { color: C.accent }]}>{t(d.label)}</Text>
        {d.hint ? <Text style={ui.note}>{t(d.hint)}</Text> : null}
      </View>
      {d.type === 'bool' ? (
        <Switch value={!!value} onValueChange={onChange} trackColor={{ true: C.accent }} />
      ) : (
        <View style={styles.inputWrap}>
          <TextInput
            style={styles.input}
            keyboardType={d.type === 'f32' ? 'decimal-pad' : 'number-pad'}
            value={value === undefined ? '' : String(value)}
            onChangeText={onChange}
            selectTextOnFocus
          />
          {d.unit ? <Text style={ui.note}>{d.unit}</Text> : null}
        </View>
      )}
    </View>
  );
}

const useStyles = themed((C) =>
  StyleSheet.create({
    inputWrap: { flexDirection: 'row', alignItems: 'center', gap: S.xs },
    input: {
      color: C.text,
      borderColor: C.border,
      borderWidth: 1,
      borderRadius: 6,
      minWidth: 72,
      paddingHorizontal: S.sm,
      paddingVertical: 6,
      textAlign: 'right',
      fontSize: 15,
    },
  }),
);
