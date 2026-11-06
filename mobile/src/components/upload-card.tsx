import * as Clipboard from 'expo-clipboard';
import { useEffect, useState } from 'react';
import { StyleSheet, Text, TextInput, View } from 'react-native';

import { Button, Card, Note, Row, useUi } from '@/components/ui';
import { N_, t } from '@/i18n';
import { shortWhen } from '@/lib/format';
import { getUpload, type PodUpload, saveSettings } from '@/lib/pod';
import { S, themed, useColors } from '@/theme';

// The gauge's log upload (upload.h): as the car starts it joins the phone's
// hotspot, opens a WireGuard tunnel home and sends the server what it lacks
// (server/README.md). Set up here once: the hotspot, the server, and the
// tunnel - the gauge makes its own key, and its public half goes on the
// router as a peer. The password and token are never read back: an empty
// field leaves them as they are.
type Field = 'upSsid' | 'upPass' | 'upUrl' | 'upToken' | 'wgHost' | 'wgPort' | 'wgPeer' | 'wgAddr';
const FIELDS: { key: Field; label: string; secret?: boolean; hint?: string; numeric?: boolean }[] = [
  { key: 'upSsid', label: N_('Hotspot name') },
  { key: 'upPass', label: N_('Hotspot password'), secret: true },
  { key: 'upUrl', label: N_('Server'), hint: 'http://192.168.1.10:8650' },
  { key: 'upToken', label: N_('Server token'), secret: true },
  { key: 'wgHost', label: N_('Router address'), hint: 'vpn.example.org' },
  { key: 'wgPort', label: N_('Router port'), numeric: true },
  { key: 'wgPeer', label: N_('Router public key') },
  { key: 'wgAddr', label: N_('Gauge’s tunnel address'), hint: '10.0.0.7' },
];

function fromPod(u: PodUpload): Record<Field, string> {
  return {
    upSsid: u.ssid,
    upPass: '',
    upUrl: u.url,
    upToken: '',
    wgHost: u.wgHost,
    wgPort: String(u.wgPort),
    wgPeer: u.wgPeer,
    wgAddr: u.wgAddr,
  };
}

export function UploadCard() {
  const ui = useUi();
  const C = useColors();
  const styles = useStyles();
  const [up, setUp] = useState<PodUpload | null | undefined>(undefined);
  const [form, setForm] = useState<Record<Field, string> | null>(null);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<string | null>(null);
  const [copied, setCopied] = useState(false);

  const apply = (u: PodUpload | null) => {
    setUp(u);
    if (u) setForm(fromPod(u));
  };
  const load = async () => apply(await getUpload());
  useEffect(() => {
    let live = true;
    getUpload().then((u) => live && apply(u));
    return () => {
      live = false;
    };
  }, []);

  if (up === undefined) return null;
  if (up === null || !form) return null;        // older firmware: no upload

  const was = fromPod(up);
  const changed = (Object.keys(form) as Field[]).filter((k) => form[k] !== was[k]);
  const save = async () => {
    setBusy(true);
    setMsg(null);
    try {
      const body = changed.map((k) => `${k}=${encodeURIComponent(form[k].trim())}`).join('&');
      setMsg(await saveSettings(body));
      await load();
    } catch (e) {
      setMsg(String(e));
    } finally {
      setBusy(false);
    }
  };
  const copyKey = async () => {
    await Clipboard.setStringAsync(up.wgPub);
    setCopied(true);
  };

  const last = up.last;
  return (
    <Card title={t('Upload to the home server')}>
      <Note>
        {t('As the car starts, the gauge joins this hotspot, opens a WireGuard tunnel home and uploads the logs the server lacks.')}
      </Note>
      {last.at || last.msg ? (
        <Row
          label={t('Last upload')}
          value={
            last.ok
              ? t('{files} files, {kb} KB, {when}', {
                  files: last.files,
                  kb: Math.round(last.bytes / 1024),
                  when: last.at ? shortWhen(last.at * 1000) : '',
                })
              : last.msg
          }
          tone={last.ok ? 'good' : 'bad'}
        />
      ) : (
        <Row label={t('Last upload')} value={up.configured ? t('none since it woke') : t('not set up')} tone="dim" />
      )}
      {FIELDS.map((f) => (
        <View key={f.key} style={styles.field}>
          <Text style={ui.rowLabel}>{t(f.label)}</Text>
          <TextInput
            style={styles.input}
            value={form[f.key]}
            onChangeText={(v) => setForm({ ...form, [f.key]: v })}
            placeholder={
              f.secret
                ? (f.key === 'upPass' ? up.passSet : up.tokenSet)
                  ? t('set - type to change')
                  : t('not set')
                : f.hint
            }
            placeholderTextColor={C.dim}
            secureTextEntry={f.secret}
            autoCapitalize="none"
            autoCorrect={false}
            keyboardType={f.numeric ? 'number-pad' : 'default'}
          />
        </View>
      ))}
      <Button
        title={changed.length ? t('Save') : t('No changes')}
        disabled={!changed.length}
        busy={busy}
        onPress={save}
      />
      {/* The gauge's own words, in English like its other answers. */}
      {msg ? <Note tone={msg === 'Saved.' ? 'good' : 'bad'}>{msg}</Note> : null}
      <View style={styles.field}>
        <Text style={ui.rowLabel}>{t('Gauge’s public key')}</Text>
        <Text style={styles.key} selectable>
          {up.wgPub}
        </Text>
      </View>
      <Note>{t('Add it on the router as a WireGuard peer, allowed the gauge’s tunnel address only.')}</Note>
      <Button title={copied ? t('Copied') : t('Copy the public key')} kind="plain" onPress={copyKey} />
    </Card>
  );
}

const useStyles = themed((C) =>
  StyleSheet.create({
    field: { gap: S.xs },
    input: {
      color: C.text,
      borderColor: C.border,
      borderWidth: 1,
      borderRadius: 6,
      paddingHorizontal: S.sm,
      paddingVertical: 6,
      fontSize: 15,
    },
    key: { color: C.text, fontFamily: 'monospace', fontSize: 13 },
  }),
);
