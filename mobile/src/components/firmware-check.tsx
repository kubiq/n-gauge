import { useState } from 'react';

import { Button, Note } from '@/components/ui';
import { usePod } from '@/hooks/use-pod';
import { t } from '@/i18n';
import { loadReadyFw, lookForFirmware } from '@/lib/fw-download';

// A look for a newer gauge firmware on GitHub now (fw-download.ts), rather
// than at the day's one look (asked 2026-11-11) - in Settings' About, and in
// Gauge settings where it is looked for (asked 2026-11-21). Not while on the
// gauge's WiFi: it has no internet. A release found is downloaded and
// checked against the gauge's key; Gauge settings then offers to install it.
export function FirmwareCheck({ onFound }: { onFound?: () => void }) {
  const pod = usePod();
  const [looking, setLooking] = useState(false);
  const [said, setSaid] = useState<{ text: string; good: boolean } | null>(null);
  const look = async () => {
    setLooking(true);
    setSaid(null);
    try {
      const r = await lookForFirmware(true);
      const ready = loadReadyFw();
      if (ready) {
        setSaid({
          text: t('{tag} downloaded and checked - connect to the gauge to install it.', {
            tag: ready.commit ? `${ready.tag} · ${ready.commit}` : ready.tag,
          }),
          good: true,
        });
        onFound?.();
      } else if (r.endsWith(' is not newer')) {
        // The latest release is the gauge's own, or older: said plainly.
        setSaid({ text: t('Up to date - {tag} is the latest release.', { tag: r.slice(0, -' is not newer'.length) }), good: true });
      } else {
        setSaid({ text: r, good: false });
      }
    } catch (e) {
      setSaid({ text: String((e as Error).message), good: false });
    } finally {
      setLooking(false);
    }
  };
  return (
    <>
      <Button
        title={t('Check for a new gauge firmware')}
        kind="plain"
        busy={looking}
        disabled={pod.state !== 'idle'}
        onPress={look}
      />
      {pod.state !== 'idle' ? <Note>{t('Not while on the gauge’s WiFi - it has no internet.')}</Note> : null}
      {said ? <Note tone={said.good ? 'good' : undefined}>{said.text}</Note> : null}
    </>
  );
}
