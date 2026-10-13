import { File } from 'expo-file-system';
import { useEffect, useState } from 'react';
import { Alert, Pressable, StyleSheet, Switch, Text, TextInput, View } from 'react-native';

import { LogKeyCard } from '@/components/log-key-card';
import { Button, Card, Note, Screen, styles as ui } from '@/components/ui';
import { usePod } from '@/hooks/use-pod';
import { useSync } from '@/hooks/use-sync';
import { getSettings, saveSettings, uploadFirmware } from '@/lib/pod';
import { checkValue, type PodSettings, type SettingDef, SETTINGS, saveForm } from '@/lib/settings-schema';
import { C, S } from '@/theme';

const GROUPS = [...new Set(SETTINGS.map((d) => d.group))];

export default function SettingsScreen() {
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
  const nChanged = Object.keys(changes).length;

  // The pod serves one request at a time, and a download holds it for up to
  // two minutes: a running sync is stopped first, and picked up again after.
  const sync = useSync();

  const save = async () => {
    setBusy('save');
    try {
      await sync.cancel();
      Alert.alert('Settings', await saveSettings(saveForm(changes)));
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

  return (
    <Screen tabBar>
      {/* This phone's side, always here: the key is the phone's and is needed
          off the gauge's WiFi too. While connected it also shows the gauge's state. */}
      <LogKeyCard />
      {!connected ? (
        <Card title="Gauge">
          <Note>Connect to the gauge (tap it at the top) to change its settings or update its firmware.</Note>
        </Card>
      ) : null}
      {connected && err ? <Note tone="bad">{err}</Note> : null}
      {connected && orig
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
      {connected ? (
        <>
          {problems.length ? <Note tone="bad">{problems.join('\n')}</Note> : null}
          <Button
            title={nChanged ? `Save ${nChanged} change${nChanged === 1 ? '' : 's'}` : 'No changes'}
            disabled={!nChanged || problems.length > 0}
            busy={busy === 'save'}
            onPress={save}
          />
          <Card title="Firmware">
            <Note>
              Pick a car_gauge_v1 .bin from the phone’s files. If the new build fails, the gauge rolls back by itself.
            </Note>
            <Button title="Update firmware" kind="plain" busy={busy === 'fw'} onPress={firmware} />
          </Card>
        </>
      ) : null}
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
