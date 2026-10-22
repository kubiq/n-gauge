import { useMemo, useState } from 'react';
import { type GestureResponderEvent, StyleSheet, Text, View } from 'react-native';
import Svg, { Circle, Line, Path, Rect } from 'react-native-svg';

import { Card } from '@/components/ui';
import { t } from '@/i18n';
import { duration } from '@/lib/format';
import type { Series } from '@/lib/series';
import { type Colors, S, themed, useColors } from '@/theme';

// A drive's readings over time, as small multiples sharing one time axis:
// speed, boost, oil and coolant, voltage - one y-axis each, never two scales
// on one chart. Drag across them: one cursor runs through all, and each
// heading shows its value there; untouched, the headings show the peak.
//
// Series colours from the theme (series1 blue: a lone series and coolant;
// series2 orange: oil, as on the gauge) - the dataviz palette's steps for
// each surface, validated against its card (CVD ΔE 26.8 dark, 24.7 light).
// Text never wears them.

const PLOT_H = 72;
// The column right of the plot for its scale ("1.23", "-0.74", "14.9"),
// and half a scale label's line height, to centre one on its gridline.
const GUTTER = 40;
const LABEL_MID = 7;
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

const PANELS = (s: Series, C: Colors): Panel[] => {
  const BLUE = C.series1, ORANGE = C.series2;
  return [
    { title: t('Speed'), unit: ' km/h', digits: 0, lines: [{ name: t('Speed'), data: s.kmh, color: BLUE }], idle: peak(0, ' km/h') },
    // The true value, vacuum included, with the zero drawn.
    { title: t('Boost'), unit: ' bar', digits: 2, zero: true, lines: [{ name: t('Boost'), data: s.boostBar, color: BLUE }], idle: peak(2, ' bar') },
    {
      title: t('Oil and coolant'),
      unit: ' °C',
      digits: 0,
      lines: [
        { name: t('Oil'), data: s.oilC, color: ORANGE },
        { name: t('Coolant'), data: s.coolantC, color: BLUE },
      ],
      idle: peak(0, ' °C'),
    },
    {
      title: t('Voltage'),
      unit: ' V',
      digits: 1,
      lines: [{ name: t('Voltage'), data: s.volts, color: BLUE }],
      idle: (ls: Line_[]) => {
        const v = finite(ls[0].data);
        return `${Math.min(...v).toFixed(1)}–${Math.max(...v).toFixed(1)} V`;
      },
    },
  ]
    // A channel the log does not have (older logs: no voltage) is no chart.
    .map((p) => ({ ...p, lines: p.lines.filter((l) => finite(l.data).length > 1) }))
    .filter((p) => p.lines.length > 0);
};

// The car not running, shaded behind every chart (asked 2026-10-22: a stop
// with the car switched off was an unexplained gap): off darker, on with the
// engine off lighter. The same neutral as the axes - no series colour.
const OFF_OPACITY = { 0: 0.22, 1: 0.1 } as const;

type Still = { from: number; to: number; state: 0 | 1 };

/** The runs of buckets with the car off, or on with the engine off. */
function stillSpans(car: Series['car']): Still[] {
  const out: Still[] = [];
  car.forEach((s, i) => {
    if (s !== 0 && s !== 1) return;
    const last = out[out.length - 1];
    if (last && last.state === s && last.to === i - 1) last.to = i;
    else out.push({ from: i, to: i, state: s });
  });
  return out;
}

const stateName = (s: 0 | 1 | 2 | null | undefined) =>
  s === 0 ? t('car off') : s === 1 ? t('engine off') : null;

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

/**
 * `onCursor`: the cursor's moment as epoch ms - for the map's dot - or null
 * with no clock in the log (Series.startUtc).
 */
export function DriveChart({ series, onCursor }: { series: Series; onCursor?: (utcMs: number | null) => void }) {
  const C = useColors();
  const styles = useStyles();
  const panels = useMemo(() => PANELS(series, C), [series, C]);
  // Older caches have no car state: nothing shaded.
  const still = useMemo(() => stillSpans(series.car ?? []), [series]);
  const n = series.t.length;
  const [width, setWidth] = useState(0);
  const [at, setAt] = useState<number | null>(null);

  // One responder over all the charts: a touch puts the cursor there and a
  // drag moves it. The screen's scroll may still take the touch over - a
  // drag that is mostly vertical scrolls, as anywhere else.
  // The plot leaves a column on the right for its scale, so a line near the
  // top or bottom never runs through the numbers.
  const plotW = Math.max(0, width - GUTTER);
  const pick = (x: number) => {
    if (plotW <= 0) return;
    const i = Math.max(0, Math.min(n - 1, Math.round((Math.min(x, plotW) / plotW) * (n - 1))));
    setAt(i);
    onCursor?.(series.startUtc !== null ? series.startUtc + series.t[i] * 1000 : null);
  };
  const responder = {
    onStartShouldSetResponder: () => true,
    onResponderGrant: (e: GestureResponderEvent) => pick(e.nativeEvent.locationX),
    onResponderMove: (e: GestureResponderEvent) => pick(e.nativeEvent.locationX),
    onResponderTerminationRequest: () => true,
  };

  if (n < 2 || panels.length === 0) return null;
  const x = (i: number) => (i / (n - 1)) * plotW;
  // A bucket's band reaches halfway to its neighbours, within the plot.
  const half = plotW / (n - 1) / 2;
  const band = (s: Still) => {
    const x0 = Math.max(0, x(s.from) - half);
    return { x: x0, width: Math.min(plotW, x(s.to) + half) - x0 };
  };
  const atState = at === null ? null : stateName(series.car?.[at]);

  return (
    <Card title={t('Over the drive')}>
      <Text style={styles.hint}>
        {at === null
          ? t('Drag across the charts to read them at any moment.')
          : t('At {at} of {total}', { at: clock(series.t[at]), total: clock(series.durationS) }) +
            (atState ? `  ·  ${atState}` : '')}
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
                {plotW > 0 ? (
                  <Svg width={plotW} height={PLOT_H}>
                    {still.map((s) => (
                      <Rect key={s.from} {...band(s)} y={0} height={PLOT_H} fill={C.dim} fillOpacity={OFF_OPACITY[s.state]} />
                    ))}
                    {/* Recessive frame: the top and bottom of the range, and zero. */}
                    <Line x1={0} x2={plotW} y1={y(hi)} y2={y(hi)} stroke={C.border} strokeWidth={1} />
                    <Line x1={0} x2={plotW} y1={y(lo)} y2={y(lo)} stroke={C.border} strokeWidth={1} />
                    {p.zero && lo < 0 ? <Line x1={0} x2={plotW} y1={y(0)} y2={y(0)} stroke={C.dim} strokeWidth={1} /> : null}
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
                {/* The range's ends, so the shape has a scale: in the
                    column right of the plot, each level with its line. */}
                <Text style={[styles.axis, styles.scale, { top: y(hi) - LABEL_MID }]}>{hi.toFixed(p.digits)}</Text>
                <Text style={[styles.axis, styles.scale, { top: y(lo) - LABEL_MID }]}>{lo.toFixed(p.digits)}</Text>
              </View>
            </View>
          );
        })}
        {/* The time axis spans the plot, not the scale's column. */}
        <View style={[styles.head, { paddingRight: GUTTER }]}>
          <Text style={styles.axis}>0 min</Text>
          <Text style={styles.axis}>{duration(series.durationS)}</Text>
        </View>
        {/* What the shading means, for the states the drive has. */}
        {still.length ? (
          <View style={styles.keys}>
            {([0, 1] as const)
              .filter((st) => still.some((s) => s.state === st))
              .map((st) => (
                <View key={st} style={styles.key}>
                  <View style={[styles.block, { backgroundColor: C.dim, opacity: OFF_OPACITY[st] }]} />
                  <Text style={styles.axis}>{stateName(st)}</Text>
                </View>
              ))}
          </View>
        ) : null}
      </View>
    </Card>
  );
}

const useStyles = themed((C) =>
  StyleSheet.create({
    hint: { color: C.dim, fontSize: 13 },
    head: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'baseline', gap: S.sm },
    title: { color: C.text, fontSize: 14, fontWeight: '600' },
    value: { color: C.text, fontSize: 14, fontVariant: ['tabular-nums'], flexShrink: 1, textAlign: 'right' },
    keys: { flexDirection: 'row', gap: S.md },
    key: { flexDirection: 'row', alignItems: 'center', gap: S.xs },
    swatch: { width: 14, height: 3, borderRadius: 2 },
    block: { width: 14, height: 10, borderRadius: 2 },
    axis: { color: C.dim, fontSize: 11 },
    // In the GUTTER column; `top` set per label, from its line.
    scale: { position: 'absolute', right: 0, width: GUTTER - S.xs, textAlign: 'right', lineHeight: 2 * LABEL_MID },
  }),
);
