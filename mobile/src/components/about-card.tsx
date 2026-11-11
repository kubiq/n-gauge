import Constants from 'expo-constants';
import { useState } from 'react';

import { Button, Card, Note, Row } from '@/components/ui';
import { usePod } from '@/hooks/use-pod';
import { t } from '@/i18n';
import { shortWhen } from '@/lib/format';
import { loadReadyFw, lookForFirmware } from '@/lib/fw-download';
import { loadGaugeFw } from '@/lib/store';

// Which builds are running - the one place to read both when something is
// wrong. The app's version and commit come from git at build time
// (app.config.ts); the gauge's as it said at the last sync or visit to
// Gauge settings - the app is on its WiFi only then - and when that was.
//
// And a look for a newer gauge firmware on GitHub now (fw-download.ts),
// rather than at the day's one look (asked 2026-11-11). Not while on the
// gauge's WiFi: it has no internet.
export function AboutCard() {
  const fw = loadGaugeFw();
  const build = Constants.expoConfig?.extra?.build as string | undefined;
  const pod = usePod();
  const [looking, setLooking] = useState(false);
  const [said, setSaid] = useState<{ text: string; good: boolean } | null>(null);
  const look = async () => {
    setLooking(true);
    setSaid(null);
    try {
      const r = await lookForFirmware(true);
      const ready = loadReadyFw();
      setSaid(
        ready
          ? {
              text: t('{tag} downloaded and checked - install it in Gauge settings.', {
                tag: ready.commit ? `${ready.tag} · ${ready.commit}` : ready.tag,
              }),
              good: true,
            }
          : { text: r, good: false },
      );
    } catch (e) {
      setSaid({ text: String((e as Error).message), good: false });
    } finally {
      setLooking(false);
    }
  };
  return (
    <Card title={t('About')}>
      <Row label={t('App')} value={`${Constants.expoConfig?.version ?? '?'}${build ? ` (${build})` : ''}`} />
      {fw ? <Row label={t('Gauge')} value={t('{build}, read {when}', { build: fw.build, when: shortWhen(fw.at) })} /> : null}
      <Button
        title={t('Look for a newer gauge firmware')}
        kind="plain"
        busy={looking}
        disabled={pod.state !== 'idle'}
        onPress={look}
      />
      {pod.state !== 'idle' ? <Note>{t('Not while on the gauge’s WiFi - it has no internet.')}</Note> : null}
      {said ? <Note tone={said.good ? 'good' : undefined}>{said.text}</Note> : null}
      {/* Required by the licence (CC BY 4.0) of the drives' place names. */}
      <Note>{t('Place names © GeoNames (geonames.org), CC BY 4.0.')}</Note>
      <Note>{t('City quarter boundaries © ČÚZK (RÚIAN), via ČSÚ open data, CC BY 4.0.')}</Note>
    </Card>
  );
}
