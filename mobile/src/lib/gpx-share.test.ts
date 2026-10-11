/// <reference types="node" />
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { cleanGpx, GPX_TAIL } from './gpx-share.ts';

const HEAD = '<?xml version="1.0" encoding="UTF-8"?>\n<gpx version="1.1"><trk><name>x</name><trkseg>';
const pt = (i: number) =>
  `<trkpt lat="50.${i}" lon="14.${i}"><ele>250.0</ele><time>2026-10-11T10:00:0${i}Z</time></trkpt>`;

test('a file as the pod writes it comes out the same', () => {
  const f = `${HEAD}\n${pt(1)}\n${pt(2)}\n${GPX_TAIL}`;
  assert.equal(cleanGpx(f), f);
});

// Pulled while open: [..., pt2, TAIL] then continued from that size, while
// the pod had written pt3 over its TAIL - so the copy has the old tail, then
// pt3 missing its first bytes, then pt4 and the new tail.
test('an old tail in the middle and the cut-off point after it are dropped', () => {
  const p3 = pt(3);
  const broken = `${HEAD}\n${pt(1)}\n${pt(2)}\n${GPX_TAIL}${p3.slice(GPX_TAIL.length)}\n${pt(4)}\n${GPX_TAIL}`;
  assert.equal(cleanGpx(broken), `${HEAD}\n${pt(1)}\n${pt(2)}\n${pt(4)}\n${GPX_TAIL}`);
});

test('segment breaks are kept between points, never empty or trailing', () => {
  const f = `${HEAD}\n</trkseg><trkseg>\n${pt(1)}\n</trkseg><trkseg>\n</trkseg><trkseg>\n${pt(2)}\n</trkseg><trkseg>\n${GPX_TAIL}`;
  assert.equal(cleanGpx(f), `${HEAD}\n${pt(1)}\n</trkseg><trkseg>\n${pt(2)}\n${GPX_TAIL}`);
});

test('a track without points is still valid', () => {
  assert.equal(cleanGpx(`${HEAD}\n${GPX_TAIL}`), `${HEAD}\n${GPX_TAIL}`);
});

test('a file that is not the pod’s is left alone', () => {
  assert.equal(cleanGpx('<gpx><rte/></gpx>'), '<gpx><rte/></gpx>');
});
