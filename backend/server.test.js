import assert from 'node:assert/strict';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { after, before, test } from 'node:test';
import { gzipSync } from 'node:zlib';

import { createApp, safeName } from './server.js';

let dir, server, base;
const auth = { Authorization: 'Bearer s3cret' };

before(async () => {
  dir = await mkdtemp(join(tmpdir(), 'ngauge-'));
  server = createApp({ dataDir: dir, token: 's3cret', maxBytes: 1000 });
  await new Promise((r) => server.listen(0, r));
  base = `http://127.0.0.1:${server.address().port}`;
});

after(async () => {
  server.close();
  await rm(dir, { recursive: true });
});

const put = (name, offset, body, headers = {}) =>
  fetch(`${base}/api/files/${name}?offset=${offset}`, { method: 'PUT', headers: { ...auth, ...headers }, body });

test('names: the pod whitelist, nothing that leaves the directory', () => {
  assert.ok(safeName('drive_2026-10-11_12-45.csv'));
  assert.ok(safeName('sleeplog.txt'));
  for (const bad of ['', '..', '../x', 'a/b', 'a b', 'x'.repeat(41), '.%2e']) assert.ok(!safeName(bad), bad);
});

test('health needs no token, everything else does', async () => {
  assert.equal((await fetch(`${base}/api/health`)).status, 200);
  assert.equal((await fetch(`${base}/api/files`)).status, 401);
  assert.equal((await fetch(`${base}/api/files`, { headers: { Authorization: 'Bearer nope!!' } })).status, 401);
});

test('append in pieces, then the listing shows the whole size', async () => {
  let r = await put('drive_0001.csv', 0, 'hello ');
  assert.deepEqual(await r.json(), { name: 'drive_0001.csv', size: 6 });
  r = await put('drive_0001.csv', 6, 'world');
  assert.deepEqual(await r.json(), { name: 'drive_0001.csv', size: 11 });
  assert.equal(await readFile(join(dir, 'drive_0001.csv'), 'utf8'), 'hello world');
  const list = await (await fetch(`${base}/api/files`, { headers: auth })).json();
  assert.deepEqual(list, [{ name: 'drive_0001.csv', size: 11 }]);
});

// The reply of a successful PUT was lost and the phone sends it again.
test('a repeated append is refused with the real size, not written twice', async () => {
  await put('trip_0001.gpx', 0, 'abc');
  const r = await put('trip_0001.gpx', 0 + 0 + 2, 'c');   // stale offset
  assert.equal(r.status, 409);
  assert.equal((await r.json()).size, 3);
  assert.equal(await readFile(join(dir, 'trip_0001.gpx'), 'utf8'), 'abc');
});

test('offset 0 on an existing file replaces it', async () => {
  await put('track_0001.vbo', 0, 'old old old');
  await put('track_0001.vbo', 0, 'new');
  assert.equal(await readFile(join(dir, 'track_0001.vbo'), 'utf8'), 'new');
});

test('gzip bodies are stored uncompressed; offsets count raw bytes', async () => {
  await put('sleeplog.txt', 0, gzipSync('line 1\n'), { 'Content-Encoding': 'gzip' });
  const r = await put('sleeplog.txt', 7, gzipSync('line 2\n'), { 'Content-Encoding': 'gzip' });
  assert.equal((await r.json()).size, 14);
  assert.equal(await readFile(join(dir, 'sleeplog.txt'), 'utf8'), 'line 1\nline 2\n');
});

test('a broken gzip body leaves the file exactly as it was', async () => {
  await put('drive_0002.csv', 0, 'keep');
  const r = await put('drive_0002.csv', 4, Buffer.from('not gzip at all'), { 'Content-Encoding': 'gzip' });
  assert.equal(r.status, 400);
  assert.equal(await readFile(join(dir, 'drive_0002.csv'), 'utf8'), 'keep');
});

test('the size limit applies after decompression', async () => {
  const bomb = gzipSync(Buffer.alloc(5000));   // tiny compressed, 5000 raw > 1000
  const r = await put('drive_0003.csv', 0, bomb, { 'Content-Encoding': 'gzip' });
  assert.equal(r.status, 413);
});

test('a failed replacement keeps the old copy', async () => {
  await put('drive_0004.csv', 0, 'precious');
  const r = await put('drive_0004.csv', 0, Buffer.alloc(2000));   // over the limit
  assert.equal(r.status, 413);
  assert.equal(await readFile(join(dir, 'drive_0004.csv'), 'utf8'), 'precious');
});

test('bad names and offsets are refused', async () => {
  // ".." is folded away by URL parsing before it arrives (404); either way
  // nothing may be written outside the data directory.
  assert.ok([400, 404].includes((await put('..', 0, 'x')).status));
  assert.ok([400, 404].includes((await put('%2e%2e', 0, 'x')).status));
  assert.equal((await put('a%2Fb', 0, 'x')).status, 400);
  assert.equal((await put('a%2F..%2F..%2Fescape', 0, 'x')).status, 400);
  assert.equal((await put('ok.csv', -1, 'x')).status, 400);
  assert.equal((await put('ok.csv', 'abc', 'x')).status, 400);
});

test('files that are not ours are not listed', async () => {
  await writeFile(join(dir, 'has space.txt'), 'x');
  const list = await (await fetch(`${base}/api/files`, { headers: auth })).json();
  assert.ok(!list.some((f) => f.name === 'has space.txt'));
});
