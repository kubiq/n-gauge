import { router, useFocusEffect } from 'expo-router';
import { useCallback, useRef, useState } from 'react';

import { Button, Card, Note, Row } from '@/components/ui';
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
          ? `Cancelled after ${r.files} file${r.files === 1 ? '' : 's'}.`
          : `Uploaded ${r.files} file${r.files === 1 ? '' : 's'}, ${mb(r.bytes)}.`,
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
    <Card title="Upload">
      <Row
        label="Waiting"
        value={waiting ? `${waiting} file${waiting === 1 ? '' : 's'}, ${mb(pendingBytes(inbox))}` : 'nothing'}
        tone={waiting ? undefined : 'good'}
      />
      <Row label="Server" value={cfg ? cfg.url.replace(/^https?:\/\//, '') : 'not set'} tone={cfg ? undefined : 'dim'} />
      {busy ? (
        <Button title="Cancel" kind="danger" onPress={() => abort.current?.abort()} />
      ) : (
        <>
          {cfg && waiting ? <Button title="Upload" onPress={upload} /> : null}
          <Button title={cfg ? 'Change server' : 'Set up the server'} kind="plain" onPress={() => router.push('/server')} />
        </>
      )}
      {status ? <Note>{status}</Note> : null}
      {err ? <Note tone="bad">{err}</Note> : null}
    </Card>
  );
}
