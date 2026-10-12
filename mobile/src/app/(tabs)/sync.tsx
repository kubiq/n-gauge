import { useFocusEffect } from 'expo-router';
import { useCallback, useRef, useState } from 'react';

import { Button, Card, Note, Row, Screen } from '@/components/ui';
import { usePod } from '@/hooks/use-pod';
import { listFiles } from '@/lib/pod';
import { UPLOAD_ENABLED } from '@/features';
import { dropNonLogs, knownBytes, loadInbox, pendingBytes, pendingFiles, pull, storedBytes } from '@/lib/store';
import { type PodFile, planPull, type Pull, pullBytes } from '@/lib/sync-plan';

const mb = (n: number) => `${(n / 1048576).toFixed(n < 10 * 1048576 ? 1 : 0)} MB`;

// Pod -> phone inbox. The inbox goes on to the home server from the Pod
// screen's Upload card, once off the pod's network.
export default function SyncScreen() {
  const pod = usePod();
  const connected = pod.state === 'connected';
  // The pod's listing from the last check; the plan is recomputed from it as
  // files arrive, so it counts down without asking the pod again.
  const [podFiles, setPodFiles] = useState<PodFile[] | null>(null);
  const [plan, setPlan] = useState<Pull[] | null>(null);
  const [status, setStatus] = useState<string | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [busy, setBusy] = useState<'check' | 'pull' | null>(null);
  const [inbox, setInbox] = useState(dropNonLogs);
  // One check or pull at a time - the pod serves one request at a time anyway.
  const running = useRef(false);
  const abort = useRef<AbortController | null>(null);

  const check = useCallback(async () => {
    if (running.current) return;
    running.current = true;
    setErr(null);
    setBusy('check');
    try {
      const t0 = Date.now();
      const files = await listFiles();
      // Shown because listing holds the pod's main loop - worth knowing as the card fills.
      setStatus(`Listed in ${((Date.now() - t0) / 1000).toFixed(1)} s`);
      setPodFiles(files);
      setPlan(planPull(files, knownBytes(loadInbox())));
    } catch (e) {
      setErr(String((e as Error).message));
    } finally {
      running.current = false;
      setBusy(null);
    }
  }, []);

  // Every time the tab is opened while connected: what is new is the whole
  // point of opening it.
  useFocusEffect(
    useCallback(() => {
      if (connected) check();
    }, [connected, check]),
  );

  const pullAll = async () => {
    if (!plan || !podFiles || running.current) return;
    running.current = true;
    const ctl = new AbortController();
    abort.current = ctl;
    setErr(null);
    setBusy('pull');
    let done = 0;
    try {
      for (const [i, p] of plan.entries()) {
        if (ctl.signal.aborted) break;
        await pull(
          p,
          (b) => setStatus(`${i + 1}/${plan.length} ${p.name}: ${mb(b)} of ${mb(p.size - p.from)}`),
          ctl.signal,
        );
        done++;
        // Per file, so "On the phone" and the count of what is left move as
        // each one lands.
        const now = loadInbox();
        setInbox(now);
        setPlan(planPull(podFiles, knownBytes(now)));
      }
      setStatus(`Pulled ${done} file${done === 1 ? '' : 's'}.`);
    } catch (e) {
      // Whatever finished is in the inbox; the next check plans the rest.
      if (ctl.signal.aborted) setStatus(`Cancelled after ${done} of ${plan.length}.`);
      else setErr(`Stopped after ${done} of ${plan.length}: ${(e as Error).message}`);
    } finally {
      if (ctl.signal.aborted && done < plan.length) setStatus(`Cancelled after ${done} of ${plan.length}.`);
      abort.current = null;
      running.current = false;
      setBusy(null);
    }
  };

  return (
    <Screen tabBar>
      <Card title="On the phone">
        {UPLOAD_ENABLED ? (
          <>
            <Row label="Waiting for upload" value={`${pendingFiles(inbox)} files, ${mb(pendingBytes(inbox))}`} />
            <Note>To upload: Disconnect on the Pod screen - the server is only reachable off the pod’s WiFi.</Note>
          </>
        ) : (
          <Row label="Logs" value={`${Object.keys(inbox).length} files, ${mb(storedBytes(inbox))}`} />
        )}
      </Card>

      <Card title="Pod">
        {!connected ? <Note>Connect to the pod first (Pod tab).</Note> : null}
        {connected && plan && podFiles ? (
          <>
            <Row label="On the card" value={`${podFiles.length} files`} />
            <Row
              label="New or grown"
              value={plan.length ? `${plan.length} files, ${mb(pullBytes(plan))}` : 'up to date'}
              tone={plan.length ? undefined : 'good'}
            />
            {plan.some((p) => p.restart) ? (
              <Note tone="bad">Some files are smaller than the copies here; they will be fetched again whole.</Note>
            ) : null}
            {busy === 'pull' ? (
              <Button title="Cancel" kind="danger" onPress={() => abort.current?.abort()} />
            ) : (
              <>
                {plan.length ? <Button title="Pull" disabled={busy !== null} onPress={pullAll} /> : null}
                <Button
                  title="Check again"
                  kind="plain"
                  busy={busy === 'check'}
                  disabled={busy !== null}
                  onPress={check}
                />
              </>
            )}
          </>
        ) : connected ? (
          <Button title="Check for new files" busy={busy === 'check'} disabled={busy !== null} onPress={check} />
        ) : null}
        {status ? <Note>{status}</Note> : null}
        {err ? <Note tone="bad">{err}</Note> : null}
      </Card>
    </Screen>
  );
}
