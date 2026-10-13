import { router, useFocusEffect } from 'expo-router';
import { useCallback, useState } from 'react';

import { Button, Card, Row } from '@/components/ui';
import { usePod } from '@/hooks/use-pod';
import { loadLogKey, type LogKey } from '@/lib/logkey';
import { getSettings } from '@/lib/pod';

// The way into log encryption, on the Pod screen: there whether or not the
// app is on the pod's WiFi, since the key itself is the phone's. While
// connected it also says what the pod does, compared by fingerprint.
export function LogKeyCard() {
  const connected = usePod().state === 'connected';
  const [key, setKey] = useState<LogKey | null | undefined>(undefined);
  // The pod's key fingerprint: undefined = not known, '' = plain logs.
  const [podFp, setPodFp] = useState<string | undefined>(undefined);

  // Fresh on every return (after creating a key, or setting the pod's) and
  // on connecting.
  useFocusEffect(
    useCallback(() => {
      loadLogKey().then(setKey);
      if (connected)
        getSettings().then(
          (s) => setPodFp(s.logKey),
          () => setPodFp(undefined),
        );
    }, [connected]),
  );

  if (key === undefined) return null;
  const mine = key ? key.fingerprint.replace(/ /g, '') : null;
  return (
    <Card title="Log encryption">
      <Row label="This phone’s key" value={key ? key.fingerprint : 'not set up'} tone={key ? undefined : 'dim'} />
      {connected && podFp !== undefined ? (
        <Row
          label="Gauge"
          value={podFp === '' ? 'not encrypted' : podFp === mine ? 'encrypted to this phone' : 'encrypted to another key'}
          tone={podFp === '' ? 'dim' : podFp === mine ? 'good' : 'bad'}
        />
      ) : null}
      <Button title={key ? 'Manage' : 'Set up'} kind="plain" onPress={() => router.push('/log-key')} />
    </Card>
  );
}
