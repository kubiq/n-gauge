// Fuel (asked 2026-11-17): consumption over time and per week, by fuel, and
// the car's own figure against what went in at the pump - with the pump's
// litres typed in at each fill-up, the exact measure (fuel-stats.ts).
//
// From the summaries already worked out (trip-cache.ts) - drives not read
// yet are counted, not read here: the list reads them.

import { useFocusEffect } from 'expo-router';
import { useCallback, useState } from 'react';
import { Pressable, StyleSheet, Text, TextInput, View } from 'react-native';

import { ConsumptionChart, WeeklyChart } from '@/components/fuel-charts';
import { Card, Note, Row, Screen, useUi } from '@/components/ui';
import { t } from '@/i18n';
import { shortWhen, when } from '@/lib/format';
import { fillId, fuelByDrive, fuelLabel, type Fuels, totalsByFuel } from '@/lib/fuel-grade';
import {
  byWeek,
  carFactor,
  drivePoints,
  type FuelDrive,
  fillIntervals,
  fillsByDrive,
  rolling,
  type FillInterval,
  type SkippedStretch,
  tankFromPump,
} from '@/lib/fuel-stats';
import { endName, loadNames } from '@/lib/names';
import { loadFillLitres, loadFuelFrom, loadFuels, saveFillLitres, saveFuelFrom } from '@/lib/store';
import { cachedCsv, cachedGpx, summarise, tripFiles } from '@/lib/trip-cache';
import type { Refuel } from '@/lib/trips';
import { distance } from '@/lib/units';
import { S, themed, useColors } from '@/theme';

type Data = {
  /** Newest first, as the list. */
  drives: FuelDrive[];
  unread: number;
  fuels: Fuels;
  pump: Record<string, number>;
  /** The fill-up the comparison counts from (store.ts); null: all. */
  from: string | null;
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
  return { drives, unread, fuels: loadFuels(), pump: loadFillLitres(), from: loadFuelFrom() };
}

export default function FuelScreen() {
  const [data, setData] = useState<Data>(load);
  const [reading, setReading] = useState<{ at: number; of: number } | null>(null);
  // Back from the list, a drive read since counts too - and the car data
  // not read yet (older weeks the list has not opened) is read here: a
  // stretch between fill-ups with an unread drive in it cannot be summed.
  useFocusEffect(
    useCallback(() => {
      setData(load());
      let live = true;
      (async () => {
        const todo = tripFiles()
          .map((f) => f.csv)
          .filter((n): n is string => !!n && !cachedCsv(n));
        for (const [i, name] of todo.entries()) {
          if (!live) return;
          setReading({ at: i + 1, of: todo.length });
          await summarise(name).catch(() => undefined);   // one that will not read is left out
          if (live && (i % 5 === 4 || i === todo.length - 1)) setData(load());
        }
        if (live) setReading(null);
      })();
      return () => {
        live = false;
      };
    }, []),
  );
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
  // Every fill-up, newest first, for the pump's litres.
  const allFills: Refuel[] = data.drives.flatMap((d) => [...(fills[d.key] ?? [])].reverse());
  // The level's 0-100 % in litres, from the receipts: the level reads 0 with
  // litres still in (2026-11-17: ~46 of the book's 50).
  const tank = tankFromPump(allFills, data.pump);
  const skipped: SkippedStretch[] = [];
  const intervals = fillIntervals(oldest, fills, data.pump, skipped, tank.litres);
  // Counted from the fill-up picked (asked 2026-11-17: the stretches from
  // while the gauge was built - in and out of the car - spoil the figure);
  // the ones before it are shown, not counted.
  const fromFill = data.from ? allFills.find((f) => fillId(f) === data.from) : undefined;
  const counts = (i: FillInterval) => !fromFill?.at || (i.from.at !== null && i.from.at >= fromFill.at);
  const factor = carFactor(intervals.filter(counts));
  const pickFrom = (f: Refuel) => {
    const id = data.from === fillId(f) ? null : fillId(f);
    saveFuelFrom(id);
    setData((d) => ({ ...d, from: id }));
  };
  const litres = points.reduce((s, p) => s + p.litres, 0);
  const km = points.reduce((s, p) => s + p.km, 0);
  const names = loadNames();

  const setPump = (f: Refuel, text: string) => {
    const v = Number(text.replace(',', '.'));
    const pump = saveFillLitres(fillId(f), text.trim() && Number.isFinite(v) && v > 0 ? v : null);
    setData((d) => ({ ...d, pump }));
  };

  return (
    <Screen>
      {reading ? (
        <Note>{t('Reading the car data… {at} of {of}', { at: reading.at, of: reading.of })}</Note>
      ) : data.unread ? (
        <Note>{t('{n} drives could not be read.', { n: data.unread })}</Note>
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
          <View key={fillId(i.to)} style={[styles.stretch, !counts(i) && styles.notCounted]}>
            <Text style={counts(i) ? styles.stretchHead : styles.skipHead}>
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
        {/* The ones left out, and why - so a gap is not a mystery. */}
        {skipped.map((s) => (
          <View key={`skip-${fillId(s.to)}`} style={styles.stretch}>
            <Text style={styles.skipHead}>
              {`${s.from.at ? shortWhen(s.from.at) : '?'} → ${s.to.at ? shortWhen(s.to.at) : '?'}`}
            </Text>
            <Text style={ui.note}>
              {s.why === 'nofuel'
                ? t('left out: a drive in it has no car data ({at})', { at: s.at ? when(s.at) : t('undated') })
                : t('left out: a fill-up in it cannot be placed in its drive')}
            </Text>
          </View>
        ))}
        {tank.fills ? (
          <Note>{t('The tank holds {l} l from 0 to 100 % of its level, by {n} receipts.', { l: tank.litres.toFixed(1), n: tank.fills })}</Note>
        ) : null}
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
                    {`${Math.round(f.fromPct)} → ${Math.round(f.toPct)} %  ·  ≈ ${Math.round(((f.toPct - f.fromPct) / 100) * tank.litres)} l`}
                  </Text>
                  {/* Where the comparison counts from: a tap picks, again unpicks. */}
                  {f.at !== null ? (
                    <Pressable onPress={() => pickFrom(f)} hitSlop={8}>
                      <Text style={data.from === id ? styles.fromOn : styles.from}>
                        {data.from === id ? t('counted from here') : t('count from here')}
                      </Text>
                    </Pressable>
                  ) : null}
                </View>
                <TextInput
                  style={[ui.input, styles.litres]}
                  defaultValue={data.pump[id] != null ? String(data.pump[id]) : ''}
                  onEndEditing={(e) => setPump(f, e.nativeEvent.text)}
                  placeholder={t('litres')}
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
    skipHead: { color: C.dim, fontSize: 14, fontWeight: '600' },
    notCounted: { opacity: 0.5 },
    from: { color: C.link, fontSize: 13 },
    fromOn: { color: C.accent, fontSize: 13, fontWeight: '700' },
    fill: { flexDirection: 'row', alignItems: 'center', gap: S.md },
    fillText: { flex: 1, gap: 2 },
    litres: { width: 110, textAlign: 'right' },
  }),
);
