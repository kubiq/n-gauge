// Fuel (asked 2026-11-17): consumption over time and per week, by fuel, and
// the car's own figure against what went in at the pump - with the pump's
// litres typed in at each fill-up, the exact measure (fuel-stats.ts).
//
// From the summaries already worked out (trip-cache.ts) - drives not read
// yet are counted, not read here: the list reads them.

import { useFocusEffect } from 'expo-router';
import { useCallback, useState } from 'react';
import { StyleSheet, Text, TextInput, View } from 'react-native';

import { ConsumptionChart, WeeklyChart } from '@/components/fuel-charts';
import { Card, Note, Row, Screen, useUi } from '@/components/ui';
import { t } from '@/i18n';
import { shortWhen } from '@/lib/format';
import { fillId, fuelByDrive, fuelLabel, type Fuels, totalsByFuel } from '@/lib/fuel-grade';
import {
  byWeek,
  carFactor,
  drivePoints,
  type FuelDrive,
  fillIntervals,
  fillsByDrive,
  rolling,
  TANK_L,
} from '@/lib/fuel-stats';
import { endName, loadNames } from '@/lib/names';
import { loadFillLitres, loadFuels, saveFillLitres } from '@/lib/store';
import { cachedCsv, cachedGpx, tripFiles } from '@/lib/trip-cache';
import type { Refuel } from '@/lib/trips';
import { distance } from '@/lib/units';
import { S, themed, useColors } from '@/theme';

type Data = {
  /** Newest first, as the list. */
  drives: FuelDrive[];
  unread: number;
  fuels: Fuels;
  pump: Record<string, number>;
};

function load(): Data {
  const drives: FuelDrive[] = [];
  let unread = 0;
  for (const t of tripFiles()) {
    const g = t.gpx ? cachedGpx(t.gpx) : null;
    const c = t.csv ? cachedCsv(t.csv) : null;
    if (t.csv && !c) unread++;
    drives.push({ key: t.key, start: g?.start ?? t.start, end: g?.end ?? null, to: g?.to, toPlace: g?.toPlace, c: c ?? undefined });
  }
  return { drives, unread, fuels: loadFuels(), pump: loadFillLitres() };
}

export default function FuelScreen() {
  const [data, setData] = useState<Data>(load);
  // Back from the list, a drive read since counts too.
  useFocusEffect(useCallback(() => setData(load()), []));
  const ui = useUi();
  const styles = useStyles();
  const C = useColors();

  const fills = fillsByDrive(data.drives);
  const oldest = [...data.drives].reverse();
  const points = drivePoints(oldest);
  const avg = rolling(points);
  const weeks = byWeek(points);
  const fuelOf = fuelByDrive(oldest.map((d) => ({ key: d.key, fills: fills[d.key] })), data.fuels);
  const byFuel = totalsByFuel(oldest.map((d) => ({ fuel: fuelOf[d.key], fuelL: d.c?.fuelL ?? null, fuelKm: d.c?.fuelKm ?? null })));
  const intervals = fillIntervals(oldest, fills, data.pump);
  const factor = carFactor(intervals);
  const litres = points.reduce((s, p) => s + p.litres, 0);
  const km = points.reduce((s, p) => s + p.km, 0);
  // Every fill-up, newest first, for the pump's litres.
  const allFills: Refuel[] = data.drives.flatMap((d) => [...(fills[d.key] ?? [])].reverse());
  const names = loadNames();

  const setPump = (f: Refuel, text: string) => {
    const v = Number(text.replace(',', '.'));
    const pump = saveFillLitres(fillId(f), text.trim() && Number.isFinite(v) && v > 0 ? v : null);
    setData((d) => ({ ...d, pump }));
  };

  return (
    <Screen>
      {data.unread ? (
        <Note>{t('{n} drives are not read yet - the list reads them; then they count here too.', { n: data.unread })}</Note>
      ) : null}

      <Card title={t('Overall')}>
        {km > 0 ? (
          <>
            <Row label={t('Consumption')} value={`${((litres / km) * 100).toFixed(1)} l/100 km`} />
            {factor ? (
              <Row label={t('Corrected by the pump')} value={`${((litres / km) * 100 * factor.factor).toFixed(1)} l/100 km`} />
            ) : null}
            <Row label={t('Fuel')} value={`${litres.toFixed(0)} l`} />
            <Row label={t('Distance')} value={distance(km * 1000)} />
            {byFuel.map((f) => (
              <Row key={fuelLabel(f.fuel)} label={fuelLabel(f.fuel)} value={`${((f.litres / f.km) * 100).toFixed(1)} l/100  ·  ${distance(f.km * 1000)}`} />
            ))}
          </>
        ) : (
          <Note>{t('No drive with the car’s fuel figure yet.')}</Note>
        )}
      </Card>

      {points.length > 1 ? <ConsumptionChart points={points} avg={avg} /> : null}
      {weeks.length > 1 ? <WeeklyChart weeks={weeks} /> : null}

      <Card title={t('The car’s figure against the pump')}>
        <Note>
          {t(
            'The car reports the fuel it uses; added up over the drives between two fill-ups, it should be what went in at the second. Type in the pump’s litres at each fill-up - the tank level alone is coarse.',
          )}
        </Note>
        {factor ? (
          <Text style={styles.factor}>
            {t('The car’s figure is {pct} {dir}, over {n} stretches by the {by}.', {
              pct: `${Math.abs(Math.round((factor.factor - 1) * 100))} %`,
              dir: factor.factor >= 1 ? t('low') : t('high'),
              n: factor.stretches,
              by: factor.by === 'pump' ? t('pump') : t('tank level'),
            })}
          </Text>
        ) : (
          <Note>{t('Two fill-ups made while parked are needed, with drives between them.')}</Note>
        )}
        {intervals.map((i) => (
          <View key={fillId(i.to)} style={styles.stretch}>
            <Text style={styles.stretchHead}>
              {`${i.from.at ? shortWhen(i.from.at) : '?'} → ${i.to.at ? shortWhen(i.to.at) : '?'}  ·  ${Math.round(i.km)} km`}
            </Text>
            <Text style={ui.note}>
              {t('car {car} l  ·  burned {burned}', {
                car: i.carL.toFixed(1),
                burned: i.burnedL === null ? '?' : `${i.burnedL.toFixed(1)} l (${i.by === 'pump' ? t('pump') : t('tank level')})`,
              })}
              {i.burnedL !== null && i.carL > 0 ? `  ·  ×${(i.burnedL / i.carL).toFixed(2)}` : ''}
            </Text>
          </View>
        ))}
      </Card>

      {allFills.length ? (
        <Card title={t('Fill-ups')}>
          {allFills.map((f) => {
            const where = endName(f.pt, f.place ?? null, names);
            const id = fillId(f);
            return (
              <View key={id} style={styles.fill}>
                <View style={styles.fillText}>
                  <Text style={styles.stretchHead}>{`${f.at ? shortWhen(f.at) : t('Undated')}${where ? `  ·  ${where}` : ''}`}</Text>
                  <Text style={ui.note}>
                    {`${Math.round(f.fromPct)} → ${Math.round(f.toPct)} %  ·  ≈ ${Math.round(((f.toPct - f.fromPct) / 100) * TANK_L)} l`}
                  </Text>
                </View>
                <TextInput
                  style={[ui.input, styles.litres]}
                  defaultValue={data.pump[id] != null ? String(data.pump[id]) : ''}
                  onEndEditing={(e) => setPump(f, e.nativeEvent.text)}
                  placeholder={t('pump l')}
                  placeholderTextColor={C.dim}
                  keyboardType="decimal-pad"
                />
              </View>
            );
          })}
        </Card>
      ) : null}
    </Screen>
  );
}

const useStyles = themed((C) =>
  StyleSheet.create({
    factor: { color: C.text, fontSize: 15, fontWeight: '600' },
    stretch: { gap: 2 },
    stretchHead: { color: C.text, fontSize: 14, fontWeight: '600' },
    fill: { flexDirection: 'row', alignItems: 'center', gap: S.md },
    fillText: { flex: 1, gap: 2 },
    litres: { width: 96, textAlign: 'right' },
  }),
);
