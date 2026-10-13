import { StyleSheet, Text, View } from 'react-native';

import { Card, Row, styles as ui } from '@/components/ui';
import type { PodLive } from '@/lib/pod';
import { C, S } from '@/theme';

type Sys = NonNullable<PodLive['sys']>;

const kb = (n: number) => `${Math.round(n / 1024)} KB`;
const mb = (n: number) => `${(n / 1048576).toFixed(n < 10 * 1048576 ? 1 : 0)} MB`;
const gb = (kbytes: number) => `${(kbytes / 1048576).toFixed(1)} GB`;

// The gauge's own resources, from its live status (/api/live "sys"). Internal
// RAM is the one to watch. The gauge starts WiFi only with 80 KB free
// (AP_MIN_INTERNAL_HEAP) and WiFi then holds ~46 KB of it, so with the app
// connected ~45-55 KB free is normal (seen 2026-10-13: 53 KB, lowest 44 KB).
// Red under 20 KB at its lowest: tight enough that an allocation may fail.
export function SystemCard({ sys, uptimeMs }: { sys: Sys; uptimeMs: number }) {
  const ramLow = sys.heapMin < 20 * 1024;
  return (
    <Card title="System">
      <Bar
        label="RAM free"
        value={`${kb(sys.heap)} of ${kb(sys.heapTotal)}, lowest ${kb(sys.heapMin)}`}
        used={1 - sys.heap / sys.heapTotal}
        bad={ramLow}
      />
      <Bar label="PSRAM free" value={`${mb(sys.psram)} of ${mb(sys.psramTotal)}`} used={1 - sys.psram / sys.psramTotal} />
      {sys.sdTotalKb > 0 ? (
        <Bar
          label="SD card"
          value={`${gb(sys.sdUsedKb)} of ${gb(sys.sdTotalKb)}`}
          used={sys.sdUsedKb / sys.sdTotalKb}
          bad={sys.sdUsedKb / sys.sdTotalKb > 0.9}
        />
      ) : (
        <Row label="SD card" value="not available" tone="dim" />
      )}
      <Bar label="Firmware" value={`${mb(sys.app)} of ${mb(sys.appMax)}`} used={sys.app / sys.appMax} bad={sys.app / sys.appMax > 0.9} />
      <Row label="Chip temperature" value={`${Math.round(sys.tempC)} °C`} tone={sys.tempC > 80 ? 'bad' : undefined} />
      <Row label="Started by" value={sys.reset} tone={/crash|watchdog|dip/.test(sys.reset) ? 'bad' : 'dim'} />
      <Row label="Up" value={`${Math.round(uptimeMs / 60000)} min`} tone="dim" />
    </Card>
  );
}

// A row with a thin bar under it, filled to the part in use (0..1).
function Bar({ label, value, used, bad }: { label: string; value: string; used: number; bad?: boolean }) {
  const fill = Math.min(1, Math.max(0, used));
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

const styles = StyleSheet.create({
  track: { height: 6, borderRadius: 3, backgroundColor: C.border, overflow: 'hidden' },
  fill: { height: 6, borderRadius: 3 },
});
