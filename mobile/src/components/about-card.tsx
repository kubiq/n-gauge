import Constants from 'expo-constants';

import { Card, Note, Row } from '@/components/ui';
import { t } from '@/i18n';
import { shortWhen } from '@/lib/format';
import { loadGaugeFw } from '@/lib/store';

// Which builds are running - the one place to read both when something is
// wrong. The app's version and commit come from git at build time
// (app.config.ts); the gauge's as it said at the last sync or visit to
// Gauge settings - the app is on its WiFi only then - and when that was.
export function AboutCard() {
  const fw = loadGaugeFw();
  const build = Constants.expoConfig?.extra?.build as string | undefined;
  return (
    <Card title={t('About')}>
      <Row label={t('App')} value={`${Constants.expoConfig?.version ?? '?'}${build ? ` (${build})` : ''}`} />
      {fw ? <Row label={t('Gauge')} value={t('{build}, read {when}', { build: fw.build, when: shortWhen(fw.at) })} /> : null}
      {/* Required by the licence (CC BY 4.0) of the drives' place names. */}
      <Note>{t('Place names © GeoNames (geonames.org), CC BY 4.0.')}</Note>
      <Note>{t('City quarter boundaries © ČÚZK (RÚIAN), via ČSÚ open data, CC BY 4.0.')}</Note>
    </Card>
  );
}
