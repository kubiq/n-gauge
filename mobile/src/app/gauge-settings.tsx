import { router } from 'expo-router';
import { File } from 'expo-file-system';
import { useEffect, useRef, useState } from 'react';
import { Alert, Pressable, StyleSheet, Switch, Text, TextInput, View } from 'react-native';

import { ScreenOrderCard } from '@/components/screen-order-card';
import { SystemCard } from '@/components/system-card';
import { Button, Card, Note, Row, Screen, useUi } from '@/components/ui';
import { useHoldPod, usePod } from '@/hooks/use-pod';
import { useSync } from '@/hooks/use-sync';
import { N_, t, tn } from '@/i18n';
import { getLive, getSettings, type PodLive, saveSettings, setClock, uploadFirmware, zeroGmeter } from '@/lib/pod';
import { scanPodQr } from '@/lib/scan-qr';
import { checkValue, GROUP_TITLES, type PodSettings, type SettingDef, SETTINGS, saveForm } from '@/lib/settings-schema';
import { saveGaugeFw } from '@/lib/store';
import { S, themed, useColors } from '@/theme';

const GROUPS = [...new Set(SETTINGS.map((d) => d.group))];

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
        if (l.fw) saveGaugeFw(l.fw.build);
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

  // The gauge's answer to the last save, shown under the button: a quiet
  // line for "Saved.", not an alert to tap away. Errors still alert.
  const [saved, setSaved] = useState<string | null>(null);

  // The pod serves one request at a time, and a download holds it for up to
  // two minutes: a running sync is stopped first, and picked up again after.
  const sync = useSync();

  const save = async () => {
    setBusy('save');
    setSaved(null);
    try {
      await sync.cancel();
      const body = [saveForm(changes), viewsChanged ? `views=${views}` : ''].filter(Boolean).join('&');
      setSaved(await saveSettings(body));
      setViews(null);
      await load();
    } catch (e) {
      Alert.alert(t('Settings'), String((e as Error).message));
    } finally {
      setBusy(null);
      sync.sync();
    }
  };

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
          <Button
            title={nChanged ? tn(nChanged, 'Save 1 change', 'Save {n} changes') : t('No changes')}
            disabled={!nChanged || problems.length > 0}
            busy={busy === 'save'}
            onPress={save}
          />
          {saved && !nChanged ? <Note tone={saved.startsWith('Saved') ? 'good' : 'bad'}>{saved}</Note> : null}
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
      <Card title={t('Firmware')}>
        <Note>{t('Pick a car_gauge_v1 .bin from the phone’s files. If the new build fails, the gauge rolls back by itself.')}</Note>
        <Button title={t('Update firmware')} kind="plain" busy={busy === 'fw'} onPress={firmware} />
      </Card>
      {live?.sys ? <SystemCard sys={live.sys} iso={live.iso} uptimeMs={live.uptimeMs} /> : null}
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
