import { File } from 'expo-file-system';
import { useEffect, useState } from 'react';
import { Alert, Pressable, StyleSheet, Switch, Text, TextInput, View } from 'react-native';

import { ScreenOrderCard } from '@/components/screen-order-card';
import { Button, Card, Note, Screen, styles as ui } from '@/components/ui';
import { usePod } from '@/hooks/use-pod';
import { useSync } from '@/hooks/use-sync';
import { getSettings, saveSettings, uploadFirmware } from '@/lib/pod';
import { checkValue, type PodSettings, type SettingDef, SETTINGS, saveForm } from '@/lib/settings-schema';
import { C, S } from '@/theme';

const GROUPS = [...new Set(SETTINGS.map((d) => d.group))];

// The gauge's own settings, its screen order and its firmware - opened from
// the Gauge screen, and linked from Settings, which holds the phone's.
export default function GaugeSettingsScreen() {
  const pod = usePod();
  const connected = pod.state === 'connected';
  const [orig, setOrig] = useState<PodSettings | null>(null);
  // Edits as typed: text for numbers (so "2." survives), booleans as is.
  const [edit, setEdit] = useState<Record<string, string | boolean>>({});
  const [err, setErr] = useState<string | null>(null);
  const [busy, setBusy] = useState<string | null>(null);

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
    if (connected) load();
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
        problems.push(`${d.label}: ${p}`);
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
      Alert.alert('Settings', String((e as Error).message));
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
      Alert.alert('Firmware', `${bin.name} is not a .bin`);
      return;
    }
    Alert.alert('Update firmware?', `${bin.name}, ${(bin.size / 1048576).toFixed(2)} MB. Takes ~30 s; the gauge restarts.`, [
      { text: 'Cancel', style: 'cancel' },
      {
        text: 'Update',
        onPress: async () => {
          setBusy('fw');
          try {
            // No sync after: the pod restarts, and the reconnect syncs.
            await sync.cancel();
            Alert.alert('Firmware', await uploadFirmware(bin));
          } catch (e) {
            Alert.alert('Firmware', String((e as Error).message));
          } finally {
            setBusy(null);
          }
        },
      },
    ]);
  };

  if (!connected) {
    // The settings live on the gauge: nothing to show until it answers.
    return (
      <Screen>
        <Card title="Not connected">
          <Note>The settings are read from the gauge itself. Connect to change them or to update its firmware.</Note>
          {pod.creds ? (
            <Button title="Connect" busy={pod.state === 'connecting'} onPress={pod.connect} />
          ) : (
            <Note>Set the gauge up first, on the Gauge screen.</Note>
          )}
          {pod.error ? <Note tone="bad">{pod.error}</Note> : null}
        </Card>
      </Screen>
    );
  }

  return (
    <Screen>
      {err ? <Note tone="bad">{err}</Note> : null}
      {orig ? null : err ? null : <Note>Reading the gauge’s settings…</Note>}
      {orig
        ? GROUPS.map((g) => (
            <Card key={g} title={g}>
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
            title={nChanged ? `Save ${nChanged} change${nChanged === 1 ? '' : 's'}` : 'No changes'}
            disabled={!nChanged || problems.length > 0}
            busy={busy === 'save'}
            onPress={save}
          />
          {saved && !nChanged ? <Note tone={saved.startsWith('Saved') ? 'good' : 'bad'}>{saved}</Note> : null}
        </>
      ) : null}
      <Card title="Firmware">
        <Note>Pick a car_gauge_v1 .bin from the phone’s files. If the new build fails, the gauge rolls back by itself.</Note>
        <Button title="Update firmware" kind="plain" busy={busy === 'fw'} onPress={firmware} />
      </Card>
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
  if (d.options) {
    // Label and hint above, the choices full width below: the labels are too
    // long to sit beside the name like a number field does.
    const current = value === undefined ? undefined : Number(value);
    return (
      <View style={{ gap: S.sm }}>
        <Text style={[ui.rowLabel, changed && { color: C.accent }]}>{d.label}</Text>
        <View style={styles.options}>
          {d.options.map((o) => {
            const on = o.value === current;
            return (
              <Pressable
                key={o.value}
                onPress={() => onChange(String(o.value))}
                style={({ pressed }) => [styles.option, on && styles.optionOn, pressed && { opacity: 0.6 }]}>
                <View style={[styles.radio, on && styles.radioOn]} />
                <Text style={[ui.rowValue, on && { color: C.accent }]}>{o.label}</Text>
              </Pressable>
            );
          })}
        </View>
        {d.hint ? <Text style={ui.note}>{d.hint}</Text> : null}
      </View>
    );
  }
  return (
    <View style={ui.row}>
      <View style={{ flexShrink: 1 }}>
        <Text style={[ui.rowLabel, changed && { color: C.accent }]}>{d.label}</Text>
        {d.hint ? <Text style={ui.note}>{d.hint}</Text> : null}
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

const styles = StyleSheet.create({
  options: { borderColor: C.border, borderWidth: 1, borderRadius: 8, overflow: 'hidden' },
  option: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: S.sm,
    paddingVertical: 12,
    paddingHorizontal: S.md,
    borderTopColor: C.border,
    borderTopWidth: StyleSheet.hairlineWidth,
  },
  optionOn: { backgroundColor: C.bg },
  radio: { width: 16, height: 16, borderRadius: 8, borderWidth: 2, borderColor: C.dim },
  radioOn: { borderColor: C.accent, backgroundColor: C.accent },
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
});
