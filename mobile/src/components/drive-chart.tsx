import { useMemo, useState } from 'react';
import { type GestureResponderEvent, StyleSheet, Text, View } from 'react-native';
import Svg, { Circle, Line, Path } from 'react-native-svg';

import { Card } from '@/components/ui';
import { duration } from '@/lib/format';
import type { Series } from '@/lib/series';
import { C, S } from '@/theme';

// A drive's readings over time, as small multiples sharing one time axis:
// speed, boost, oil and coolant, voltage - one y-axis each, never two scales
// on one chart. Drag across them: one cursor runs through all, and each
// heading shows its value there; untouched, the headings show the peak.
//
// Series colours from the dataviz palette's dark steps, validated against
// C.card (CVD ΔE 26.8): blue for a lone series and for coolant, orange for
// oil - as on the gauge. Text never wears them.
const BLUE = '#3987e5';
const ORANGE = '#d95926';

const PLOT_H = 72;
const PAD_Y = 6;   // the line's 2 px and the cursor's dots stay inside

type Line_ = { name: string; data: (number | null)[]; color: string };
type Panel = {
  title: string;
  unit: string;
  lines: Line_[];
  digits: number;
  /** The heading untouched: a peak, or a range for voltage. */
  idle: (lines: Line_[]) => string;
  zero?: boolean;
};

/** Seconds as "m:ss", or "h:mm:ss" - the cursor's moment, finer than duration(). */
const clock = (s: number) => {
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), ss = Math.floor(s % 60);
  const two = (v: number) => String(v).padStart(2, '0');
  return h ? `${h}:${two(m)}:${two(ss)}` : `${m}:${two(ss)}`;
};

const finite = (d: (number | null)[]) => d.filter((v): v is number => v !== null);
const peak = (digits: number, unit: string) => (lines: Line_[]) =>
  lines
    .map((l) => `${lines.length > 1 ? `${l.name} ` : ''}${Math.max(...finite(l.data)).toFixed(digits)}${unit}`)
    .join('  ·  ');

const PANELS = (s: Series): Panel[] =>
  [
    { title: 'Speed', unit: ' km/h', digits: 0, lines: [{ name: 'Speed', data: s.kmh, color: BLUE }], idle: peak(0, ' km/h') },
    // The true value, vacuum included, with the zero drawn.
    { title: 'Boost', unit: ' bar', digits: 2, zero: true, lines: [{ name: 'Boost', data: s.boostBar, color: BLUE }], idle: peak(2, ' bar') },
    {
      title: 'Oil and coolant',
      unit: ' °C',
      digits: 0,
      lines: [
        { name: 'Oil', data: s.oilC, color: ORANGE },
        { name: 'Coolant', data: s.coolantC, color: BLUE },
      ],
      idle: peak(0, ' °C'),
    },
    {
      title: 'Voltage',
      unit: ' V',
      digits: 1,
      lines: [{ name: 'Voltage', data: s.volts, color: BLUE }],
      idle: (ls: Line_[]) => {
        const v = finite(ls[0].data);
        return `${Math.min(...v).toFixed(1)}–${Math.max(...v).toFixed(1)} V`;
      },
    },
  ]
    // A channel the log does not have (older logs: no voltage) is no chart.
    .map((p) => ({ ...p, lines: p.lines.filter((l) => finite(l.data).length > 1) }))
    .filter((p) => p.lines.length > 0);

/** The line as a path, lifted at gaps: a dropout is not drawn across. */
function pathOf(data: (number | null)[], x: (i: number) => number, y: (v: number) => number): string {
  let d = '';
  let pen = false;
  data.forEach((v, i) => {
    if (v === null) {
      pen = false;
      return;
    }
    d += `${pen ? 'L' : 'M'}${x(i).toFixed(1)},${y(v).toFixed(1)}`;
    pen = true;
  });
  return d;
}

export function DriveChart({ series }: { series: Series }) {
  const panels = useMemo(() => PANELS(series), [series]);
  const n = series.t.length;
  const [width, setWidth] = useState(0);
  const [at, setAt] = useState<number | null>(null);

  // One responder over all the charts: a touch puts the cursor there and a
  // drag moves it. The screen's scroll may still take the touch over - a
  // drag that is mostly vertical scrolls, as anywhere else.
  const pick = (x: number) => {
    if (width > 0) setAt(Math.max(0, Math.min(n - 1, Math.round((x / width) * (n - 1)))));
  };
  const responder = {
    onStartShouldSetResponder: () => true,
    onResponderGrant: (e: GestureResponderEvent) => pick(e.nativeEvent.locationX),
    onResponderMove: (e: GestureResponderEvent) => pick(e.nativeEvent.locationX),
    onResponderTerminationRequest: () => true,
  };

  if (n < 2 || panels.length === 0) return null;
  const x = (i: number) => (i / (n - 1)) * width;

  return (
    <Card title="Over the drive">
      <Text style={styles.hint}>
        {at === null ? 'Drag across the charts to read them at any moment.' : `At ${clock(series.t[at])} of ${clock(series.durationS)}`}
      </Text>
      <View {...responder} onLayout={(e) => setWidth(e.nativeEvent.layout.width)} style={{ gap: S.md }}>
        {panels.map((p) => {
          const all = p.lines.flatMap((l) => finite(l.data));
          let lo = Math.min(...all, ...(p.zero ? [0] : []));
          let hi = Math.max(...all, ...(p.zero ? [0] : []));
          if (hi - lo < 1e-6) {
            lo -= 1;
            hi += 1;
          }
          const y = (v: number) => PAD_Y + (1 - (v - lo) / (hi - lo)) * (PLOT_H - 2 * PAD_Y);
          const value =
            at === null
              ? p.idle(p.lines)
              : p.lines
                  .map((l) => {
                    const v = l.data[at];
                    return `${p.lines.length > 1 ? `${l.name} ` : ''}${v === null ? '–' : v.toFixed(p.digits)}${p.unit}`;
                  })
                  .join('  ·  ');
          return (
            <View key={p.title} style={{ gap: S.xs }}>
              <View style={styles.head}>
                <Text style={styles.title}>{p.title}</Text>
                <Text style={styles.value}>{value}</Text>
              </View>
              {/* Two series: a key, so identity never rests on colour alone. */}
              {p.lines.length > 1 ? (
                <View style={styles.keys}>
                  {p.lines.map((l) => (
                    <View key={l.name} style={styles.key}>
                      <View style={[styles.swatch, { backgroundColor: l.color }]} />
                      <Text style={styles.axis}>{l.name}</Text>
                    </View>
                  ))}
                </View>
              ) : null}
              <View>
                {width > 0 ? (
                  <Svg width={width} height={PLOT_H}>
                    {/* Recessive frame: the top and bottom of the range, and zero. */}
                    <Line x1={0} x2={width} y1={y(hi)} y2={y(hi)} stroke={C.border} strokeWidth={1} />
                    <Line x1={0} x2={width} y1={y(lo)} y2={y(lo)} stroke={C.border} strokeWidth={1} />
                    {p.zero && lo < 0 ? <Line x1={0} x2={width} y1={y(0)} y2={y(0)} stroke={C.dim} strokeWidth={1} /> : null}
                    {p.lines.map((l) => (
                      <Path key={l.name} d={pathOf(l.data, x, y)} stroke={l.color} strokeWidth={2} fill="none" strokeLinejoin="round" strokeLinecap="round" />
                    ))}
                    {at !== null ? (
                      <>
                        <Line x1={x(at)} x2={x(at)} y1={0} y2={PLOT_H} stroke={C.dim} strokeWidth={1} />
                        {p.lines.map((l) =>
                          l.data[at] !== null ? (
                            <Circle key={l.name} cx={x(at)} cy={y(l.data[at]!)} r={4} fill={l.color} stroke={C.card} strokeWidth={2} />
                          ) : null,
                        )}
                      </>
                    ) : null}
                  </Svg>
                ) : null}
                {/* The range's ends, so the shape has a scale. */}
                <Text style={[styles.axis, styles.top]}>{hi.toFixed(p.digits)}</Text>
                <Text style={[styles.axis, styles.bottom]}>{lo.toFixed(p.digits)}</Text>
              </View>
            </View>
          );
        })}
        <View style={styles.head}>
          <Text style={styles.axis}>0 min</Text>
          <Text style={styles.axis}>{duration(series.durationS)}</Text>
        </View>
      </View>
    </Card>
  );
}

const styles = StyleSheet.create({
  hint: { color: C.dim, fontSize: 13 },
  head: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'baseline', gap: S.sm },
  title: { color: C.text, fontSize: 14, fontWeight: '600' },
  value: { color: C.text, fontSize: 14, fontVariant: ['tabular-nums'], flexShrink: 1, textAlign: 'right' },
  keys: { flexDirection: 'row', gap: S.md },
  key: { flexDirection: 'row', alignItems: 'center', gap: S.xs },
  swatch: { width: 14, height: 3, borderRadius: 2 },
  axis: { color: C.dim, fontSize: 11 },
  top: { position: 'absolute', right: 0, top: 0 },
  bottom: { position: 'absolute', right: 0, bottom: 0 },
});
