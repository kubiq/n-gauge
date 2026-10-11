/// <reference types="node" />
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { test } from 'node:test';

import { distM, pairTrips, parseCsv, parseGpx, stampOf } from './trips.ts';

// One line exactly as logger.cpp writes it.
const pt = (lat: number, lon: number, time: string, ms: number) =>
  `<trkpt lat="${lat}" lon="${lon}"><ele>250.0</ele><time>${time}</time><sat>12</sat><hdop>0.8</hdop>` +
  `<extensions><gpxtpx:TrackPointExtension><gpxtpx:speed>${ms.toFixed(2)}</gpxtpx:speed>` +
  `</gpxtpx:TrackPointExtension></extensions></trkpt>\n`;
const gpx = (...segs: string[][]) =>
  '<?xml version="1.0" encoding="UTF-8"?>\n<gpx version="1.1"><trk><name>x</name><trkseg>\n' +
  segs.map((s) => s.join('')).join('</trkseg><trkseg>\n') +
  '</trkseg></trk></gpx>\n';

test('distance: 0.001 deg of latitude is ~111 m', () => {
  assert.ok(Math.abs(distM(50, 14, 50.001, 14) - 111.2) < 0.5);
});

test('gpx: distance, time, top speed', () => {
  const s = parseGpx(
    gpx([
      pt(50.0, 14.0, '2026-10-11T10:00:00Z', 10),
      pt(50.001, 14.0, '2026-10-11T10:00:10Z', 30),   // 111 m in 10 s
      pt(50.002, 14.0, '2026-10-11T10:00:20Z', 20),
    ]),
  );
  assert.equal(s.points, 3);
  assert.equal(s.start, Date.UTC(2026, 9, 11, 10, 0, 0));
  assert.equal(s.end, Date.UTC(2026, 9, 11, 10, 0, 20));
  assert.ok(Math.abs(s.distanceM - 222.4) < 1);
  assert.equal(Math.round(s.maxKmh), 108);
  assert.equal(s.track.length, 3);
  assert.deepEqual(s.bbox, [14, 50, 14, 50.002]);
});

test('gpx: no distance across a reception gap (new trkseg)', () => {
  const s = parseGpx(
    gpx(
      [pt(50.0, 14.0, '2026-10-11T10:00:00Z', 10), pt(50.001, 14.0, '2026-10-11T10:00:10Z', 10)],
      [pt(50.1, 14.0, '2026-10-11T10:10:00Z', 10), pt(50.101, 14.0, '2026-10-11T10:10:10Z', 10)],
    ),
  );
  assert.ok(Math.abs(s.distanceM - 2 * 111.2) < 1);
});

test('gpx: a position glitch adds no distance', () => {
  const s = parseGpx(
    gpx([
      pt(50.0, 14.0, '2026-10-11T10:00:00Z', 10),
      pt(51.0, 14.0, '2026-10-11T10:00:01Z', 10),   // 111 km in a second
    ]),
  );
  assert.equal(s.distanceM, 0);
});

test('gpx: thins 20 Hz points but keeps the end', () => {
  const pts = [];
  for (let i = 0; i <= 100; i++) pts.push(pt(50 + i * 0.00001, 14, `2026-10-11T10:00:${String(Math.floor(i / 20)).padStart(2, '0')}Z`, 5));
  const s = parseGpx(gpx(pts));   // 101 points, ~1.1 m apart, ~111 m in all
  assert.equal(s.points, 101);
  assert.ok(s.track.length >= 11 && s.track.length <= 13, String(s.track.length));
  assert.equal(s.track[s.track.length - 1][1], 50.001);
});

test('gpx: points without a clock still count distance', () => {
  const s = parseGpx(gpx([pt(50.0, 14.0, '', 10), pt(50.001, 14.0, '', 10)]));
  assert.equal(s.start, null);
  assert.ok(s.distanceM > 100);
});

test('csv: maxima by column name, missing columns are null', () => {
  const s = parseCsv(
    'utc,ms,oilC,coolantC,boostBar,rpm\n' +
      ',1000,40,60,0.10,900\n' +
      ',1500,95,88,1.62,6400\n' +
      ',3000,92,,0.50,\n',
  );
  assert.equal(s.rows, 3);
  assert.equal(s.durationS, 2);
  assert.equal(s.maxOilC, 95);
  assert.equal(s.maxCoolantC, 88);
  assert.equal(s.maxBoostBar, 1.62);
  assert.equal(s.maxRpm, 6400);
  assert.equal(s.maxObdKmh, null);
  assert.equal(s.lastOilC, 92);
});

// The one real drive log in the repo (2026-09, an older header without rpm).
test('csv: the real drive_0181.csv parses', { skip: !existsSync('../data/drive_0181.csv') }, () => {
  const s = parseCsv(readFileSync('../data/drive_0181.csv', 'utf8'));
  assert.ok(s.rows > 10000);
  assert.ok(s.durationS > 3000);
  assert.ok(s.maxCoolantC !== null && s.maxCoolantC > 85 && s.maxCoolantC < 110);
  assert.ok(s.maxOilC !== null && s.maxOilC > 80 && s.maxOilC < 120);
  assert.equal(s.maxRpm, null);
});

// Local time, as the pod names files (seen 2026-10-11: trip_..._15-03.gpx
// held a drive from 15:03 to 15:45 CEST, its first <time> 13:03Z).
test('stamps from dated names are local time, null for numbered ones', () => {
  assert.equal(stampOf('trip_2026-10-11_12-45.gpx'), new Date(2026, 9, 11, 12, 45).getTime());
  assert.equal(stampOf('drive_2026-09-25_14-32_9.csv'), new Date(2026, 8, 25, 14, 32).getTime());
  assert.equal(stampOf('drive_0279.csv'), null);
});

test('encrypted names pair and date like plain ones', () => {
  assert.equal(stampOf('trip_2026-10-11_23-09.gpx.nge'), new Date(2026, 9, 11, 23, 9).getTime());
  const trips = pairTrips(['trip_2026-10-11_23-09.gpx.nge', 'drive_2026-10-11_23-08.csv.nge', 'sleeplog.txt']);
  assert.deepEqual(
    trips.map((t) => [t.gpx ?? null, t.csv ?? null]),
    [['trip_2026-10-11_23-09.gpx.nge', 'drive_2026-10-11_23-08.csv.nge']],
  );
});

test('pairing: nearest csv within the window, leftovers on their own, newest first', () => {
  const trips = pairTrips([
    'trip_2026-10-11_12-45.gpx',
    'drive_2026-10-11_12-44.csv',
    'trip_2026-10-11_15-03.gpx',
    'drive_2026-10-11_15-02.csv',
    'drive_2026-10-11_15-30.csv',   // no gpx: GPS module off
    'drive_0279.csv',               // undated
    'sleeplog.txt',
  ]);
  assert.deepEqual(
    trips.map((t) => [t.gpx ?? null, t.csv ?? null]),
    [
      [null, 'drive_2026-10-11_15-30.csv'],
      ['trip_2026-10-11_15-03.gpx', 'drive_2026-10-11_15-02.csv'],
      ['trip_2026-10-11_12-45.gpx', 'drive_2026-10-11_12-44.csv'],
      [null, 'drive_0279.csv'],
    ],
  );
});
