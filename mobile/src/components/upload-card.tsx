import { router, useFocusEffect } from 'expo-router';
import { useCallback, useRef, useState } from 'react';

import { Button, Card, Note, Row } from '@/components/ui';
import { t, tn } from '@/i18n';
import { type BackendConfig, loadBackend } from '@/lib/backend';
import { loadInbox, pendingBytes, pendingFiles, uploadInbox } from '@/lib/store';

const mb = (n: number) => `${(n / 1048576).toFixed(1)} MB`;

// Inbox -> home server. Lives on the Pod screen because that is the only
// screen there is off the pod's network, which is the only time the server
// can be reached.
export function UploadCard() {
  const [cfg, setCfg] = useState<BackendConfig | null>(null);
  const [inbox, setInbox] = useState(loadInbox);
  const [busy, setBusy] = useState(false);
  const [status, setStatus] = useState<string | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const abort = useRef<AbortController | null>(null);

  // Fresh on every return here: a pull on the Sync tab, or a new server
  // address, changes both.
  useFocusEffect(
    useCallback(() => {
      setInbox(loadInbox());
      loadBackend().then(setCfg);
    }, []),
  );

  const upload = async () => {
    if (!cfg) return;
    const ctl = new AbortController();
    abort.current = ctl;
    setBusy(true);
    setErr(null);
    try {
      const r = await uploadInbox(cfg, setStatus, ctl.signal);
      setStatus(
        ctl.signal.aborted
          ? tn(r.files, 'Cancelled after 1 file.', 'Cancelled after {n} files.')
          : tn(r.files, 'Uploaded 1 file, {size}.', 'Uploaded {n} files, {size}.', { size: mb(r.bytes) }),
      );
      if (r.problems.length) setErr(r.problems.join('\n'));
    } catch (e) {
      // What finished is recorded; the next upload carries on from there.
      setErr((e as Error).message);
      setStatus(null);
    } finally {
      abort.current = null;
      setInbox(loadInbox());
      setBusy(false);
    }
  };

  const waiting = pendingFiles(inbox);

  return (
    <Card title={t('Upload')}>
      <Row
        label={t('Waiting')}
        value={waiting ? tn(waiting, '1 file, {size}', '{n} files, {size}', { size: mb(pendingBytes(inbox)) }) : t('nothing')}
        tone={waiting ? undefined : 'good'}
      />
      <Row label={t('Server')} value={cfg ? cfg.url.replace(/^https?:\/\//, '') : t('not set')} tone={cfg ? undefined : 'dim'} />
      {busy ? (
        <Button title={t('Cancel')} kind="danger" onPress={() => abort.current?.abort()} />
      ) : (
        <>
          {cfg && waiting ? <Button title={t('Upload')} onPress={upload} /> : null}
          <Button title={cfg ? t('Change server') : t('Set up the server')} kind="plain" onPress={() => router.push('/server')} />
        </>
      )}
      {status ? <Note>{status}</Note> : null}
      {err ? <Note tone="bad">{err}</Note> : null}
    </Card>
  );
}
