import { useFocusEffect } from 'expo-router';
import { useCallback } from 'react';

import { Button, Card, Note, Row, Screen } from '@/components/ui';
import { usePod } from '@/hooks/use-pod';
import { useSync } from '@/hooks/use-sync';
import { UPLOAD_ENABLED } from '@/features';
import { pendingBytes, pendingFiles, storedBytes } from '@/lib/store';
import { pullBytes } from '@/lib/sync-plan';

const mb = (n: number) => `${(n / 1048576).toFixed(n < 10 * 1048576 ? 1 : 0)} MB`;

// Pod -> phone inbox. The sync itself runs by itself on every connect
// (use-sync.tsx); this tab shows it and can run it again. The inbox goes on
// to the home server from the Pod screen's Upload card, once off the pod's
// network.
export default function SyncScreen() {
  const pod = usePod();
  const connected = pod.state === 'connected';
  const { phase, podFiles, plan, status, err, inbox, sync, cancel } = useSync();

  // Again on every visit: a drive log grows while the engine runs, and
  // connecting is not the only time something new can be there.
  useFocusEffect(
    useCallback(() => {
      if (connected) sync();
    }, [connected, sync]),
  );

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
        {!connected ? <Note>Connect to the pod (Pod tab) - new logs are then pulled by themselves.</Note> : null}
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
          </>
        ) : null}
        {connected && phase === 'pulling' ? <Button title="Cancel" kind="danger" onPress={cancel} /> : null}
        {connected && phase !== 'pulling' ? (
          <Button
            title={plan?.length ? 'Pull' : 'Check again'}
            kind={plan?.length ? undefined : 'plain'}
            busy={phase === 'checking'}
            disabled={phase !== 'idle'}
            onPress={sync}
          />
        ) : null}
        {status ? <Note>{status}</Note> : null}
        {err ? <Note tone="bad">{err}</Note> : null}
      </Card>
    </Screen>
  );
}
