import * as Clipboard from 'expo-clipboard';
import { useEffect, useState } from 'react';
import { StyleSheet, Text, TextInput, View } from 'react-native';

import { Button, Card, Note, Row, useUi } from '@/components/ui';
import { N_, t } from '@/i18n';
import { shortWhen } from '@/lib/format';
import { loadHome, saveHome } from '@/lib/home';
import { scheduleHomeTask } from '@/lib/home-task';
import { getUpload, type PodUpload, saveSettings } from '@/lib/pod';
import { S, themed, useColors } from '@/theme';

// The gauge's log upload (upload.h): as the car starts it joins the phone's
// hotspot, opens a WireGuard tunnel home and sends the server what it lacks
// (server/README.md). Set up here once: the hotspot, the server, and the
// tunnel - the gauge makes its own key, and its public half goes on the
// router as a peer. The password and token are never read back: an empty
// field leaves them as they are.
//
// In three sections, as the way goes: the phone's hotspot, the WireGuard
// VPN through the router, the upload server at home. The VPN's words stay
// English in every language - they are the words the router's own page
// and WireGuard use, to be matched against them.
type Field = 'upSsid' | 'upPass' | 'upUrl' | 'upToken' | 'upMaxMin' | 'wgHost' | 'wgPort' | 'wgPeer' | 'wgAddr';
type FieldDef = {
  key: Field;
  label: string;
  secret?: boolean;
  hint?: string;
  numeric?: boolean;
};
const HOTSPOT: FieldDef[] = [
  { key: 'upSsid', label: N_('Name') },
  { key: 'upPass', label: N_('Password'), secret: true },
];
// Not translated (above).
const VPN: FieldDef[] = [
  {
    key: 'wgHost',
    label: 'Endpoint (router address)',
    hint: 'vpn.example.org',
  },
  { key: 'wgPort', label: 'Endpoint port', numeric: true },
  { key: 'wgPeer', label: 'Router public key' },
  { key: 'wgAddr', label: 'Gauge address in the tunnel', hint: '10.0.0.7' },
];
const SERVER: FieldDef[] = [
  { key: 'upUrl', label: N_('Address'), hint: 'http://192.168.1.10:8650' },
  { key: 'upToken', label: N_('Token'), secret: true },
  // How long the hotspot is the gauge's at most; the rest goes next time.
  { key: 'upMaxMin', label: N_('Time limit, minutes'), numeric: true },
];

function fromPod(u: PodUpload): Record<Field, string> {
  return {
    upSsid: u.ssid,
    upPass: '',
    upUrl: u.url,
    upToken: '',
    upMaxMin: String(u.maxMin ?? 10),
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
  // Whether the phone has the server's token (home.ts) - it is set here,
  // once, beside the gauge's: the gauge never gives it back.
  const [phoneToken, setPhoneToken] = useState(true);

  // The phone pulls from the same server (home.ts, by itself), so what
  // is set here is the phone's too - set in one place. The address comes
  // from the gauge each time; the token only when typed here, as the gauge
  // never gives it back. `token`: just typed, or '' to keep the phone's.
  const toPhone = async (u: PodUpload, token: string) => {
    const h = await loadHome();
    const next = { url: u.url, token: token || h?.token || '' };
    if (u.url && (next.url !== h?.url || next.token !== h?.token)) {
      await saveHome(next);
      await scheduleHomeTask();               // hourly in the background from now on
    }
    setPhoneToken(!!next.token);
  };
  const apply = (u: PodUpload | null) => {
    setUp(u);
    if (u) setForm(fromPod(u));
  };
  const load = async (token = '') => {
    const u = await getUpload();
    apply(u);
    if (u) await toPhone(u, token);
  };
  useEffect(() => {
    let live = true;
    getUpload().then((u) => {
      if (!live) return;
      apply(u);
      if (u) toPhone(u, '').catch((e) => console.warn('home server', e));
    });
    return () => {
      live = false;
    };
  }, []);

  if (up === undefined) return null;
  if (up === null || !form) return null; // older firmware: no upload

  const was = fromPod(up);
  const changed = (Object.keys(form) as Field[]).filter((k) => form[k] !== was[k]);
  const save = async () => {
    setBusy(true);
    setMsg(null);
    try {
      const body = changed.map((k) => `${k}=${encodeURIComponent(form[k].trim())}`).join('&');
      const said = await saveSettings(body);
      setMsg(said);
      // The gauge saves every value (cleaned up: http://, /32, host:port)
      // and says what still looks wrong - so the form shows what it holds.
      // Any other answer leaves the form as typed, nothing to type again.
      if (said.startsWith('Saved')) await load(form.upToken.trim());
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

  const input = (f: FieldDef, label: string) => (
    <View key={f.key} style={styles.field}>
      <Text style={ui.rowLabel}>{label}</Text>
      <TextInput
        style={styles.input}
        value={form[f.key]}
        onChangeText={(v) => setForm({ ...form, [f.key]: v })}
        placeholder={
          f.secret ? ((f.key === 'upPass' ? up.passSet : up.tokenSet) ? t('set - type to change') : t('not set')) : f.hint
        }
        placeholderTextColor={C.dim}
        secureTextEntry={f.secret}
        autoCapitalize="none"
        autoCorrect={false}
        keyboardType={f.numeric ? 'number-pad' : 'default'}
      />
    </View>
  );

  const last = up.last;
  return (
    <Card title={t('Upload to the home server')}>
      <Note>
        {t(
          'As the car starts, the gauge joins the phone’s hotspot, opens the VPN home and uploads the new logs to the server. The phone then syncs them from the server by itself whenever it can reach it - at home on WiFi, or with its own VPN on - so the drives reach the phone without connecting to the gauge.',
        )}
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
      <Text style={styles.section}>{t('Phone hotspot')}</Text>
      {HOTSPOT.map((f) => input(f, t(f.label)))}

      <Text style={styles.section}>WireGuard VPN</Text>
      {VPN.map((f) => input(f, f.label))}
      <View style={styles.field}>
        <Text style={ui.rowLabel}>Gauge public key</Text>
        <Text style={styles.key} selectable>
          {up.wgPub}
        </Text>
      </View>
      <Note>Add it on the router as a peer, with Allowed IPs = the gauge address in the tunnel (/32).</Note>
      <Button title={copied ? 'Copied' : 'Copy the public key'} kind="plain" onPress={copyKey} />

      <Text style={styles.section}>{t('Upload server')}</Text>
      {SERVER.map((f) => input(f, t(f.label)))}
      <Note>
        {t(
          'The server at home, as the gauge reaches it through the VPN. The phone syncs the drives from the same address with the same token - set here once, for both.',
        )}
      </Note>
      {up.tokenSet && !phoneToken ? (
        <Note tone="bad">{t('Type the token once more, for the phone: the gauge never gives it back.')}</Note>
      ) : null}

      <Button title={changed.length ? t('Save') : t('No changes')} disabled={!changed.length} busy={busy} onPress={save} />
      {/* The gauge's own words, in English like its other answers. */}
      {/* "Saved, but ...": saved all the same - the note says what to look at. */}
      {msg ? <Note tone={msg === 'Saved.' ? 'good' : 'bad'}>{msg}</Note> : null}
    </Card>
  );
}

const useStyles = themed((C) =>
  StyleSheet.create({
    field: { gap: S.xs },
    section: {
      color: C.text,
      fontSize: 15,
      fontWeight: '600',
      marginTop: S.sm,
    },
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
