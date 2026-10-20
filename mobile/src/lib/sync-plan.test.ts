/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { planPull, pullBytes } from './sync-plan.ts';

test('new files are fetched whole', () => {
  assert.deepEqual(planPull([{ name: 'drive_0001.csv', size: 100 }], {}), [
    { name: 'drive_0001.csv', from: 0, size: 100, restart: false },
  ]);
});

// 2026-10-14: a drive deleted from the phone came back with the next sync.
test('a drive deleted on purpose is not pulled back', () => {
  const pod = [
    { name: 'trip_2026-10-12_21-18_aaaaaa.gpx.nge', size: 500 },
    { name: 'drive_2026-10-12_21-18_aaaaaa.csv.nge', size: 300 },
  ];
  assert.deepEqual(planPull(pod, {}, new Set(['trip_2026-10-12_21-18_aaaaaa.gpx.nge'])), [
    { name: 'drive_2026-10-12_21-18_aaaaaa.csv.nge', from: 0, size: 300, restart: false },
  ]);
});

test('files we already have are skipped', () => {
  assert.deepEqual(planPull([{ name: 'drive_0001.csv', size: 100 }], { 'drive_0001.csv': 100 }), []);
});

test('a file that grew is fetched from where we stopped', () => {
  assert.deepEqual(planPull([{ name: 'sleeplog.txt', size: 900 }], { 'sleeplog.txt': 600 }), [
    { name: 'sleeplog.txt', from: 600, size: 900, restart: false },
  ]);
});

test('a file that shrank is not ours: fetch it whole', () => {
  assert.deepEqual(planPull([{ name: 'trip_0001.gpx', size: 50 }], { 'trip_0001.gpx': 8000 }), [
    { name: 'trip_0001.gpx', from: 0, size: 50, restart: true },
  ]);
});

test('only the pod’s own logs are synced', () => {
  const plan = planPull(
    [
      { name: 'drive_2026-10-11_12-45.csv', size: 10 },
      { name: 'drive_0279.csv', size: 10 },
      { name: 'trip_2026-10-11_12-45.gpx', size: 10 },
      { name: 'track_0003.vbo', size: 10 },
      { name: 'sleeplog.txt', size: 10 },
      { name: 'bootlog.txt', size: 10 },
      { name: 'drive_2026-10-11_23-09.csv.nge', size: 10 },
      { name: 'trip_2026-10-11_23-09.gpx.nge', size: 10 },
      { name: 'sleeplog.txt.nge', size: 10 },
      { name: 'car_gauge_v1.ino.bin', size: 2_000_000 },
      { name: 'notes.txt', size: 10 },
      { name: 'drive_.csv', size: 10 },
    ],
    {},
  );
  assert.deepEqual(plan.map((p) => p.name).sort(), [
    'bootlog.txt',
    'drive_0279.csv',
    'drive_2026-10-11_12-45.csv',
    'drive_2026-10-11_23-09.csv.nge',
    'sleeplog.txt',
    'track_0003.vbo',
    'trip_2026-10-11_12-45.gpx',
    'trip_2026-10-11_23-09.gpx.nge',
  ]);
});

test('smallest remaining transfer first', () => {
  const plan = planPull(
    [
      { name: 'trip_0001.gpx', size: 8_000_000 },
      { name: 'drive_0002.csv', size: 1_000_000 },
      { name: 'drive_0003.csv', size: 20_000 },
    ],
    { 'drive_0002.csv': 999_000 },
  );
  assert.deepEqual(
    plan.map((p) => p.name),
    ['drive_0002.csv', 'drive_0003.csv', 'trip_0001.gpx'],
  );
  assert.equal(pullBytes(plan), 1_000 + 20_000 + 8_000_000);
});
