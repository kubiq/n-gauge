/// <reference types="node" />
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { test } from 'node:test';

import { distM, isStub, pairTrips, parseCsv, parseGpx, sessionOf, stampOf } from './trips.ts';

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
      pt(50.001, 14.0, '2026-10-11T10:00:10Z', 11),   // 111 m in 10 s: 40 km/h, as reported
      pt(50.002, 14.0, '2026-10-11T10:00:20Z', 10),
    ]),
  );
  assert.equal(s.points, 3);
  assert.equal(s.start, Date.UTC(2026, 9, 11, 10, 0, 0));
  assert.equal(s.end, Date.UTC(2026, 9, 11, 10, 0, 20));
  assert.ok(Math.abs(s.distanceM - 222.4) < 1);
  assert.equal(Math.round(s.maxKmh), 40);
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

// 2026-10-13, the 23.9. drive: its last fix 13.5 km away, 113 s after the one
// before (431 km/h) - drawn as the end of the drive, with a line out to it.
test('gpx: a bad last fix is not the end of the drive', () => {
  const s = parseGpx(
    gpx([
      pt(50.0, 14.0, '2026-09-23T09:34:54Z', 15),
      pt(50.0001, 14.0, '2026-09-23T09:34:55Z', 15),
      pt(50.1216, 14.0, '2026-09-23T09:36:48Z', 10),   // 13.5 km on
    ]),
  );
  assert.equal(s.end, Date.UTC(2026, 8, 23, 9, 34, 55));
  assert.equal(s.track[s.track.length - 1][1], 50.0001);
  assert.ok(s.bbox![3] < 50.001, 'bounds exclude the bad fix');
});

// The other one: after a reception gap (a tunnel), ten seconds of fixes
// 2.6 km off the road claiming 260 km/h, then a jump back. Its way IN looks
// plausible (2.35 km over 44 s); the jump OUT gives it away.
test('gpx: a bad stretch after a gap is dropped, top speed and all', () => {
  const at = (s: number) => new Date(Date.UTC(2026, 8, 23, 7, 0, 0) + s * 1000).toISOString();
  const pts = [];
  for (let s = 0; s <= 60; s++) pts.push(pt(50.0 + s * 0.0002, 14.0, at(s), 22));        // 80 km/h, 60 s
  for (let s = 105; s <= 115; s++) pts.push(pt(50.0333, 14.0 + s * 0.00001, at(s), 72));  // the bad stretch
  for (let s = 116; s <= 180; s++) pts.push(pt(50.012 + (s - 116) * 0.0002, 14.0, at(s), 22)); // back on the road
  const r = parseGpx(gpx(pts));
  assert.equal(Math.round(r.maxKmh), 79, 'not 260');
  assert.ok(r.bbox![3] < 50.03, 'the bad stretch is not in the bounds');
});

// The real file, where there is one (data/ is not in the repo).
const real = '../data/trip_2026-09-23_08-37_b3edf8.gpx';
test('gpx: the real 23.9. drive - its end and top speed', { skip: !existsSync(real) }, () => {
  const r = parseGpx(readFileSync(real, 'utf8'));
  assert.equal(new Date(r.end!).toISOString().slice(11, 19), '09:34:55');   // not 09:36:48, 13.5 km off
  assert.ok(r.maxKmh < 200, `top ${Math.round(r.maxKmh)} km/h`);           // not 261
  assert.ok(r.distanceM > 145_000 && r.distanceM < 150_000);
});

// September's files: a bare <speed>, not the gpxtpx one - read as no speed at
// all, it showed "Top speed 0 km/h", 7 min moving and 185 km/h average.
test('gpx: the older bare <speed> is read', () => {
  const old = (lat: number, time: string, ms: number) =>
    `<trkpt lat="${lat}" lon="14.0"><ele>355.8</ele><time>${time}</time><sat>4</sat><hdop>9.9</hdop><speed>${ms.toFixed(2)}</speed></trkpt>\n`;
  const s = parseGpx(gpx([old(50.0, '2026-09-20T13:40:00Z', 20), old(50.0005, '2026-09-20T13:40:05Z', 22)]));
  assert.equal(Math.round(s.maxKmh), 79);
  assert.equal(s.movingS, 5);
});

test('gpx: a bad FIRST fix does not throw the drive away', () => {
  const pts = [pt(51.0, 14.0, '2026-09-23T07:00:00Z', 10)];   // 111 km off
  for (let i = 1; i <= 120; i++) pts.push(pt(50.0 + i * 0.000004, 14.0, `2026-09-23T07:00:${String(Math.floor(i / 25) + 1).padStart(2, '0')}Z`, 10));
  const s = parseGpx(gpx(pts));
  assert.ok(s.distanceM > 0, 'the real points count once re-anchored');
  assert.ok(s.bbox![3] < 50.01 || s.bbox![1] > 50.9, 'not one box spanning both');
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
  assert.equal(stampOf('trip_2026-10-11_12-45_3fa81c.gpx'), new Date(2026, 9, 11, 12, 45).getTime());
  assert.equal(stampOf('drive_2026-09-25_14-32_9_3fa81c.csv.nge'), new Date(2026, 8, 25, 14, 32).getTime());
  assert.equal(stampOf('drive_0279_3fa81c.csv'), null);
});

test('stub drives: a restart fragment hides, real drives and unread ones stay', () => {
  const t0 = 1_000_000;
  assert.equal(isStub({ distanceM: 30, start: t0, end: t0 + 60_000 }), true);         // 21:18, 1 min, 0.0 km
  assert.equal(isStub({ distanceM: 12_000, start: t0, end: t0 + 840_000 }), false);   // a drive
  assert.equal(isStub({ distanceM: 50, start: t0, end: t0 + 600_000 }), false);       // 10 min parked: kept
  assert.equal(isStub(undefined, { durationS: 30 }), true);                           // car data, 30 s
  assert.equal(isStub(undefined, { durationS: 70 }), false);
  assert.equal(isStub(), false);                                                       // not read yet
});

test('the session is the six characters before the extension', () => {
  assert.equal(sessionOf('trip_2026-10-12_21-19_3fa81c.gpx.nge'), '3fa81c');
  assert.equal(sessionOf('drive_0279_00b2e1.csv'), '00b2e1');
  assert.equal(sessionOf('drive_2026-10-12_21-21.csv'), null);
});

// 2026-10-12: the pod restarted a minute into a drive. Its GPX from before
// the restart (21:18) was nearer the drive's CSV (21:21) than... nothing in
// the names said which belonged together. The session does.
test('pairing: by session, not by time', () => {
  const trips = pairTrips([
    'trip_2026-10-12_21-18_aaaaaa.gpx.nge',
    'trip_2026-10-12_21-19_bbbbbb.gpx.nge',
    'drive_2026-10-12_21-21_bbbbbb.csv.nge',
    'sleeplog.txt',
  ]);
  assert.deepEqual(
    trips.map((t) => [t.gpx ?? null, t.csv ?? null]),
    [
      ['trip_2026-10-12_21-19_bbbbbb.gpx.nge', 'drive_2026-10-12_21-21_bbbbbb.csv.nge'],
      ['trip_2026-10-12_21-18_aaaaaa.gpx.nge', null],
    ],
  );
});

test('pairing: leftovers on their own, numbered last, a second csv of a session kept', () => {
  const trips = pairTrips([
    'trip_2026-10-11_15-03_cccccc.gpx',
    'drive_2026-10-11_15-02_cccccc.csv',
    'drive_2026-10-11_15-40_cccccc.csv',   // the same session after a card share
    'drive_2026-10-11_16-30_dddddd.csv',   // no gpx: GPS module off
    'drive_0279_eeeeee.csv',
  ]);
  assert.deepEqual(
    trips.map((t) => [t.gpx ?? null, t.csv ?? null]),
    [
      [null, 'drive_2026-10-11_16-30_dddddd.csv'],
      [null, 'drive_2026-10-11_15-40_cccccc.csv'],
      ['trip_2026-10-11_15-03_cccccc.gpx', 'drive_2026-10-11_15-02_cccccc.csv'],
      [null, 'drive_0279_eeeeee.csv'],
    ],
  );
});

