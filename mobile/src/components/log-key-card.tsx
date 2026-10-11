import { router, useFocusEffect } from 'expo-router';
import { useCallback, useState } from 'react';

import { Button, Card, Row } from '@/components/ui';
import { loadLogKey, type LogKey } from '@/lib/logkey';

// The way into log encryption, on the Pod screen: there whether or not the
// app is on the pod's WiFi, since the key itself is the phone's.
export function LogKeyCard() {
  const [key, setKey] = useState<LogKey | null | undefined>(undefined);

  // Fresh on every return, e.g. after creating or importing a key.
  useFocusEffect(
    useCallback(() => {
      loadLogKey().then(setKey);
    }, []),
  );

  if (key === undefined) return null;
  return (
    <Card title="Log encryption">
      <Row label="This phone’s key" value={key ? key.fingerprint : 'not set up'} tone={key ? undefined : 'dim'} />
      <Button title={key ? 'Manage' : 'Set up'} kind="plain" onPress={() => router.push('/log-key')} />
    </Card>
  );
}
