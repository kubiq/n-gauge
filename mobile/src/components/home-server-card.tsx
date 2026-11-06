import { useEffect, useState } from 'react';

import { Button, Card, Note, Row } from '@/components/ui';
import { useSync } from '@/hooks/use-sync';
import { t, tn } from '@/i18n';
import { shortWhen } from '@/lib/format';
import { type HomeServer, loadHome } from '@/lib/home';

// The home server, as the phone reaches it (home.ts). The drives the gauge
// uploaded as the car started come to the phone from there too, by
// themselves, whenever it is where the server is - so they are on it almost
// all the time. Set up in one place, the gauge's Upload card (it is the
// gauge's server): the phone takes the address and token from there.
export function HomeServerCard() {
  const { syncHome, lastHome, phase } = useSync();
  const [home, setHome] = useState<HomeServer | null | undefined>(undefined);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    let live = true;
    loadHome().then((h) => live && setHome(h));
    return () => {
      live = false;
    };
  }, []);

  if (home === undefined) return null;
  const syncNow = async () => {
    setBusy(true);
    try {
      await syncHome(true);
    } finally {
      setBusy(false);
    }
  };

  return (
    <Card title={t('Home server')}>
      <Note>
        {t(
          'Where the gauge uploads its logs as the car starts. The phone pulls them from there too, whenever it can reach it - at home, or with its VPN on.',
        )}
      </Note>
      {home ? (
        <>
          <Row label={t('Address')} value={home.url.replace(/^http:\/\//, '')} tone="dim" />
          <Row label={t('Last sync from it')} value={lastHome ? tLast(lastHome.files, shortWhen(lastHome.at)) : t('not yet')} tone="dim" />
          {!home.token ? <Note tone="bad">{t('No token yet: type it in the gauge’s settings, Upload to the home server.')}</Note> : null}
          <Button title={t('Sync from the home server')} kind="plain" busy={busy} disabled={phase !== 'idle'} onPress={syncNow} />
        </>
      ) : (
        <Note>{t('Set it up in the gauge’s settings, Upload to the home server - the phone takes it from there.')}</Note>
      )}
    </Card>
  );
}

function tLast(files: number, when: string): string {
  return files ? tn(files, '{when}, 1 new file', '{when}, {n} new files', { when }) : t('{when}, nothing new', { when });
}
