import { useEffect, useState } from 'react';

import { Card, Note, Row } from '@/components/ui';
import { t, tn } from '@/i18n';
import { shortWhen } from '@/lib/format';
import { getNetworks, type SeenNetwork } from '@/lib/pod';

// The WiFi networks the gauge saw around where the car parked (2026-11-02):
// it scans just before it sleeps after a drive (netscan.h on the gauge).
// The first step of uploading the logs from there - allowing one, with its
// password, comes next. Strongest first. Read once, when the screen opens
// on a connected gauge.
export function NetworksCard() {
  const [nets, setNets] = useState<SeenNetwork[] | null>(null);
  useEffect(() => {
    let live = true;
    getNetworks().then((n) => live && setNets([...n].sort((a, b) => b.rssi - a.rssi)));
    return () => {
      live = false;
    };
  }, []);
  return (
    <Card title={t('WiFi where the car parks')}>
      {nets === null ? <Note>{t('Reading…')}</Note> : null}
      {nets?.length === 0 ? (
        <Note>{t('None yet: the gauge looks just before it sleeps after a drive.')}</Note>
      ) : null}
      {nets?.map((n) => (
        <Row
          key={n.ssid}
          label={n.ssid}
          value={[
            `${n.rssi} dBm`,
            n.open ? t('open') : t('password'),
            tn(n.count, 'seen once', 'seen {n}×'),
            n.lastSeen ? shortWhen(n.lastSeen * 1000) : null,
          ]
            .filter(Boolean)
            .join(' · ')}
        />
      ))}
      {nets?.length ? (
        <Note>{t('Seen by the gauge just before it slept after a drive. Allowing one, to upload the logs from there, comes next.')}</Note>
      ) : null}
    </Card>
  );
}
