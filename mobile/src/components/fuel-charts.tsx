// The fuel screen's charts (app/fuel.tsx): consumption per drive with its
// rolling average, and litres per week. In the drive chart's way
// (drive-chart.tsx): one scale each, a recessive frame, the scale's ends in
// a column right of the plot, a touch or drag shows the value there in the
// heading - untouched, the heading says the whole. Colours from the theme's
// chart steps (series1 blue, series2 orange); text never wears them.

import { useState } from 'react';
import { type GestureResponderEvent, StyleSheet, Text, View } from 'react-native';
import Svg, { Circle, Line, Path, Rect } from 'react-native-svg';

import { Card } from '@/components/ui';
import { t } from '@/i18n';
import { shortWhen } from '@/lib/format';
import type { DrivePoint, WeekFuel } from '@/lib/fuel-stats';
import { S, themed, useColors } from '@/theme';

const PLOT_H = 140;
const GUTTER = 40;
const LABEL_MID = 7;
const PAD_Y = 8;

/** A touch or drag across the plot: the index under the finger. */
function useCursor(n: number, plotW: number) {
  const [at, setAt] = useState<number | null>(null);
  const pick = (x: number) => {
    if (plotW <= 0 || n === 0) return;
    setAt(Math.max(0, Math.min(n - 1, Math.round((Math.min(x, plotW) / plotW) * (n - 1)))));
  };
  const responder = {
    onStartShouldSetResponder: () => true,
    onResponderGrant: (e: GestureResponderEvent) => pick(e.nativeEvent.locationX),
    onResponderMove: (e: GestureResponderEvent) => pick(e.nativeEvent.locationX),
    onResponderTerminationRequest: () => true,
  };
  return { at, responder };
}

/** A nice top for the scale: the next whole step over the highest value. */
const topOf = (v: number, step: number) => Math.max(step, Math.ceil(v / step) * step);

/**
 * l/100 km per drive (dots, orange) and the rolling average (line, blue) -
 * drives in order, evenly spaced: a gap of days would only push them apart.
 */
export function ConsumptionChart({ points, avg }: { points: DrivePoint[]; avg: number[] }) {
  const C = useColors();
  const styles = useStyles();
  const [width, setWidth] = useState(0);
  const plotW = Math.max(0, width - GUTTER);
  const n = points.length;
  const { at, responder } = useCursor(n, plotW);
  const hi = topOf(Math.max(...points.map((p) => p.l100), ...avg), 5);
  const x = (i: number) => (n < 2 ? plotW / 2 : (i / (n - 1)) * plotW);
  const y = (v: number) => PAD_Y + (1 - v / hi) * (PLOT_H - 2 * PAD_Y);
  const line = avg.map((v, i) => `${i ? 'L' : 'M'}${x(i).toFixed(1)},${y(v).toFixed(1)}`).join('');
  const total = points.reduce((s, p) => s + p.litres, 0) / Math.max(1, points.reduce((s, p) => s + p.km, 0)) * 100;
  const value =
    at === null
      ? t('{n} drives, {v} l/100 overall', { n, v: total.toFixed(1) })
      : `${shortWhen(points[at].at)}  ·  ${points[at].l100.toFixed(1)}  ·  ${t('average')} ${avg[at].toFixed(1)}`;
  return (
    <Card title={t('Consumption per drive')}>
      <Text style={styles.value}>{value}</Text>
      <View style={styles.keys}>
        <View style={styles.key}>
          <View style={[styles.dot, { backgroundColor: C.series2 }]} />
          <Text style={styles.axis}>{t('a drive')}</Text>
        </View>
        <View style={styles.key}>
          <View style={[styles.swatch, { backgroundColor: C.series1 }]} />
          <Text style={styles.axis}>{t('the last 8 drives, by distance')}</Text>
        </View>
      </View>
      <View {...responder} onLayout={(e) => setWidth(e.nativeEvent.layout.width)}>
        {plotW > 0 ? (
          <Svg width={plotW} height={PLOT_H}>
            <Line x1={0} x2={plotW} y1={y(hi)} y2={y(hi)} stroke={C.border} strokeWidth={1} />
            <Line x1={0} x2={plotW} y1={y(hi / 2)} y2={y(hi / 2)} stroke={C.border} strokeWidth={1} strokeDasharray="3 4" />
            <Line x1={0} x2={plotW} y1={y(0)} y2={y(0)} stroke={C.border} strokeWidth={1} />
            {points.map((p, i) => (
              <Circle key={p.key} cx={x(i)} cy={y(Math.min(p.l100, hi))} r={at === i ? 5 : 3} fill={C.series2} fillOpacity={at === i ? 1 : 0.6} />
            ))}
            <Path d={line} stroke={C.series1} strokeWidth={2} fill="none" strokeLinejoin="round" strokeLinecap="round" />
            {at !== null ? <Line x1={x(at)} x2={x(at)} y1={0} y2={PLOT_H} stroke={C.dim} strokeWidth={1} /> : null}
          </Svg>
        ) : null}
        <Text style={[styles.axis, styles.scale, { top: y(hi) - LABEL_MID }]}>{hi}</Text>
        <Text style={[styles.axis, styles.scale, { top: y(hi / 2) - LABEL_MID }]}>{hi / 2}</Text>
        <Text style={[styles.axis, styles.scale, { top: y(0) - LABEL_MID }]}>0</Text>
      </View>
      {n ? (
        <View style={[styles.ends, { paddingRight: GUTTER }]}>
          <Text style={styles.axis}>{shortWhen(points[0].at)}</Text>
          <Text style={styles.axis}>{shortWhen(points[n - 1].at)}</Text>
        </View>
      ) : null}
    </Card>
  );
}

/** Litres per week, bars; a touch says the week's km and l/100 too. */
export function WeeklyChart({ weeks }: { weeks: WeekFuel[] }) {
  const C = useColors();
  const styles = useStyles();
  const [width, setWidth] = useState(0);
  const plotW = Math.max(0, width - GUTTER);
  const n = weeks.length;
  const { at, responder } = useCursor(n, plotW);
  const hi = topOf(Math.max(...weeks.map((w) => w.litres)), 10);
  const slot = n ? plotW / n : 0;
  const barW = Math.max(2, slot - 2);   // a 2 px surface gap between bars
  const y = (v: number) => PAD_Y + (1 - v / hi) * (PLOT_H - 2 * PAD_Y);
  const w = at === null ? null : weeks[at];
  const value =
    w === null
      ? t('{l} l in {n} weeks', { l: Math.round(weeks.reduce((s, x) => s + x.litres, 0)), n })
      : `${t('week of {d}', { d: shortWhen(w.monday) })}  ·  ${w.litres.toFixed(1)} l  ·  ${Math.round(w.km)} km  ·  ${((w.litres / Math.max(1, w.km)) * 100).toFixed(1)} l/100`;
  return (
    <Card title={t('Fuel per week')}>
      <Text style={styles.value}>{value}</Text>
      <View {...responder} onLayout={(e) => setWidth(e.nativeEvent.layout.width)}>
        {plotW > 0 ? (
          <Svg width={plotW} height={PLOT_H}>
            <Line x1={0} x2={plotW} y1={y(hi)} y2={y(hi)} stroke={C.border} strokeWidth={1} />
            <Line x1={0} x2={plotW} y1={y(0)} y2={y(0)} stroke={C.border} strokeWidth={1} />
            {weeks.map((wk, i) => {
              const top = y(wk.litres);
              const h = Math.max(1, y(0) - top);
              return (
                <Rect key={wk.monday} x={i * slot + 1} y={top} width={barW} height={h} rx={Math.min(4, barW / 2)}
                  fill={C.series1} fillOpacity={at === null || at === i ? 1 : 0.45} />
              );
            })}
          </Svg>
        ) : null}
        <Text style={[styles.axis, styles.scale, { top: y(hi) - LABEL_MID }]}>{`${hi} l`}</Text>
        <Text style={[styles.axis, styles.scale, { top: y(0) - LABEL_MID }]}>0</Text>
      </View>
      {n ? (
        <View style={[styles.ends, { paddingRight: GUTTER }]}>
          <Text style={styles.axis}>{shortWhen(weeks[0].monday)}</Text>
          <Text style={styles.axis}>{shortWhen(weeks[n - 1].monday)}</Text>
        </View>
      ) : null}
    </Card>
  );
}

const useStyles = themed((C) =>
  StyleSheet.create({
    value: { color: C.text, fontSize: 14, fontVariant: ['tabular-nums'] },
    axis: { color: C.dim, fontSize: 11 },
    scale: { position: 'absolute', right: 0, width: GUTTER - 4, textAlign: 'right' },
    ends: { flexDirection: 'row', justifyContent: 'space-between' },
    keys: { flexDirection: 'row', flexWrap: 'wrap', gap: S.md },
    key: { flexDirection: 'row', alignItems: 'center', gap: S.xs },
    swatch: { width: 12, height: 3, borderRadius: 2 },
    dot: { width: 8, height: 8, borderRadius: 4 },
  }),
);
