import { StyleSheet, Text, View } from 'react-native';

import { Card, Row, useUi } from '@/components/ui';
import { t } from '@/i18n';
import type { PodLive } from '@/lib/pod';
import { S, themed, useColors } from '@/theme';

type Sys = NonNullable<PodLive['sys']>;

const kb = (n: number) => `${Math.round(n / 1024)} KB`;
const mb = (n: number) => `${(n / 1048576).toFixed(n < 10 * 1048576 ? 1 : 0)} MB`;
const gb = (kbytes: number) => `${(kbytes / 1048576).toFixed(1)} GB`;
const of = (used: string, total: string) => t('{used} of {total}', { used, total });

// The gauge's own resources, from its live status (/api/live "sys"). Internal
// RAM is the one to watch. The gauge starts WiFi only with 80 KB free
// (AP_MIN_INTERNAL_HEAP) and WiFi then holds ~46 KB of it, so with the app
// connected ~45-55 KB free is normal (seen 2026-10-13: 53 KB, lowest 44 KB).
// Red under 20 KB at its lowest: tight enough that an allocation may fail.
export function SystemCard({ sys, iso, uptimeMs }: { sys: Sys; iso: PodLive['iso']; uptimeMs: number }) {
  const ramLow = sys.heapMin < 20 * 1024;
  return (
    <Card title={t('System')}>
      <Bar
        label={t('RAM free')}
        value={t('{free} of {total}, lowest {min}', { free: kb(sys.heap), total: kb(sys.heapTotal), min: kb(sys.heapMin) })}
        used={1 - sys.heap / sys.heapTotal}
        bad={ramLow}
      />
      <Bar label={t('PSRAM free')} value={of(mb(sys.psram), mb(sys.psramTotal))} used={1 - sys.psram / sys.psramTotal} />
      {sys.sdTotalKb > 0 ? (
        <Bar
          label={t('SD card')}
          value={of(gb(sys.sdUsedKb), gb(sys.sdTotalKb))}
          used={sys.sdUsedKb / sys.sdTotalKb}
          bad={sys.sdUsedKb / sys.sdTotalKb > 0.9}
        />
      ) : (
        <Row label={t('SD card')} value={t('not available')} tone="dim" />
      )}
      <Bar label={t('Firmware')} value={of(mb(sys.app), mb(sys.appMax))} used={sys.app / sys.appMax} bad={sys.app / sys.appMax > 0.9} />
      {/* The diagnostic requests to the car (ISO-TP): answered / given up. */}
      <Row label={t('Car requests ok / failed')} value={`${iso.ok} / ${iso.aborts}`} tone={iso.aborts ? 'bad' : 'dim'} />
      <Row label={t('Chip temperature')} value={`${Math.round(sys.tempC)} °C`} tone={sys.tempC > 80 ? 'bad' : undefined} />
      {/* The reset reason is the gauge's own word, left in English. */}
      <Row label={t('Started by')} value={sys.reset} tone={/crash|watchdog|dip/.test(sys.reset) ? 'bad' : 'dim'} />
      <Row label={t('Up')} value={`${Math.round(uptimeMs / 60000)} min`} tone="dim" />
    </Card>
  );
}

// A row with a thin bar under it, filled to the part in use (0..1).
function Bar({ label, value, used, bad }: { label: string; value: string; used: number; bad?: boolean }) {
  const fill = Math.min(1, Math.max(0, used));
  const C = useColors();
  const ui = useUi();
  const styles = useStyles();
  return (
    <View style={{ gap: S.xs }}>
      <View style={ui.row}>
        <Text style={ui.rowLabel}>{label}</Text>
        <Text style={[ui.rowValue, bad && { color: C.bad }]}>{value}</Text>
      </View>
      <View style={styles.track}>
        <View style={[styles.fill, { width: `${fill * 100}%`, backgroundColor: bad ? C.bad : C.accent }]} />
      </View>
    </View>
  );
}

const useStyles = themed((C) =>
  StyleSheet.create({
    track: { height: 6, borderRadius: 3, backgroundColor: C.border, overflow: 'hidden' },
    fill: { height: 6, borderRadius: 3 },
  }),
);
