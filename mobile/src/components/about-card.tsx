import Constants from 'expo-constants';
import { useEffect, useState } from 'react';

import { Card, Note, Row } from '@/components/ui';
import { usePod } from '@/hooks/use-pod';
import { t } from '@/i18n';
import { getLive } from '@/lib/pod';

// Which builds are running - the one place to read both when something is
// wrong. The app's version and commit come from git at build time
// (app.config.ts), the gauge's from its live status.
export function AboutCard() {
  const connected = usePod().state === 'connected';
  const [fw, setFw] = useState<string | null>(null);

  useEffect(() => {
    if (!connected) return;
    getLive().then(
      // The build time and commit; the image hash is on the gauge's Device page.
      (l) => setFw(l.fw ? l.fw.build : t('older firmware')),
      () => setFw(null),
    );
  }, [connected]);

  const build = Constants.expoConfig?.extra?.build as string | undefined;
  return (
    <Card title={t('About')}>
      <Row label={t('App')} value={`${Constants.expoConfig?.version ?? '?'}${build ? ` (${build})` : ''}`} />
      {connected && fw ? <Row label={t('Gauge')} value={fw} /> : null}
      {/* Required by the licence (CC BY 4.0) of the drives' place names. */}
      <Note>{t('Place names © GeoNames (geonames.org), CC BY 4.0.')}</Note>
      <Note>{t('City quarter boundaries © ČÚZK (RÚIAN), via ČSÚ open data, CC BY 4.0.')}</Note>
    </Card>
  );
}
