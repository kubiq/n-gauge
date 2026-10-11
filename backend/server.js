// Log store for the pod's phone app (mobile/): the phone pulls new logs off
// the pod at the car, then uploads them here over mobile data.
//
// No dependencies - node:http, node:fs and node:zlib are all it needs, and a
// server that only ever appends files is not worth a framework.
//
//   GET  /api/health                        -> {ok: true}
//   GET  /api/files                         -> [{name, size}]   what is stored
//   PUT  /api/files/<name>?offset=N  <body> -> {name, size}     append
//
// A PUT appends its body at `offset`, which must be the stored size (append)
// or 0 (replace the file: the pod's copy shrank, so it is not the one we
// have). Any other offset is a 409 carrying the real size. That is what
// makes a retry safe: if a PUT succeeded but its reply was lost, the phone
// sends it again, gets 409 with size = offset + its length, and knows it is
// done - instead of the bytes landing twice.
//
// The body may be gzipped (Content-Encoding: gzip): the phone compresses
// before uploading, since logs shrink 5-10x and this goes over mobile data.
// Offsets and sizes are always of the UNCOMPRESSED file.

import { createReadStream, createWriteStream } from 'node:fs';
import { mkdir, open, readdir, rename, stat, truncate, unlink } from 'node:fs/promises';
import { createServer } from 'node:http';
import { join } from 'node:path';
import { Transform } from 'node:stream';
import { pipeline } from 'node:stream/promises';
import { timingSafeEqual } from 'node:crypto';
import { createGunzip } from 'node:zlib';

// The pod's own whitelist (safeName() in webapi.cpp): every name it writes
// fits, and nothing that could leave the directory does.
const SAFE_NAME = /^[A-Za-z0-9._-]{1,40}$/;
export function safeName(n) {
  return SAFE_NAME.test(n) && !n.includes('..');
}

export function createApp({ dataDir, token, maxBytes = 64 * 1024 * 1024 }) {
  if (!token) throw new Error('a token is required');
  const want = Buffer.from(`Bearer ${token}`);

  // One write per file at a time; a second waits for the first, so both see
  // the size the other left.
  const busy = new Map();
  async function serialised(name, fn) {
    const prev = busy.get(name) ?? Promise.resolve();
    const run = prev.then(fn, fn);
    const tail = run.catch(() => {});
    busy.set(name, tail);
    try {
      return await run;
    } finally {
      if (busy.get(name) === tail) busy.delete(name);
    }
  }

  function authorised(req) {
    const got = Buffer.from(req.headers.authorization ?? '');
    return got.length === want.length && timingSafeEqual(got, want);
  }

  async function sizeOf(path) {
    try {
      return (await stat(path)).size;
    } catch (e) {
      if (e.code === 'ENOENT') return 0;
      throw e;
    }
  }

  async function list() {
    const out = [];
    for (const e of await readdir(dataDir, { withFileTypes: true })) {
      if (!e.isFile() || !safeName(e.name)) continue;
      out.push({ name: e.name, size: (await stat(join(dataDir, e.name))).size });
    }
    return out.sort((a, b) => a.name.localeCompare(b.name));
  }

  // Counts what goes through and stops past the limit, so a gzip bomb
  // cannot fill the disk: the limit applies to the uncompressed bytes.
  function limiter() {
    let n = 0;
    return new Transform({
      transform(chunk, _enc, cb) {
        n += chunk.length;
        if (n > maxBytes) cb(Object.assign(new Error('body too large'), { status: 413 }));
        else cb(null, chunk);
      },
    });
  }

  async function put(req, name, offset) {
    const path = join(dataDir, name);
    return serialised(name, async () => {
      const have = await sizeOf(path);
      if (offset !== have && offset !== 0) {
        return [409, { error: 'offset does not match the stored size', name, size: have }];
      }
      const gz = (req.headers['content-encoding'] ?? '').toLowerCase() === 'gzip';
      const stages = [req, ...(gz ? [createGunzip()] : []), limiter()];
      if (offset === 0 && have > 0) {
        // A replacement goes to a temp file first: a failed upload must not
        // cost the copy we already had.
        const tmp = `${path}.part`;
        try {
          await pipeline(...stages, createWriteStream(tmp));
          await rename(tmp, path);
        } catch (e) {
          await unlink(tmp).catch(() => {});
          throw e;
        }
      } else {
        try {
          await pipeline(...stages, createWriteStream(path, { flags: 'a' }));
        } catch (e) {
          // Cut back to where it was, so a half-written append is never
          // mistaken for data the next time the phone asks for the size.
          await truncate(path, have).catch(() => {});
          throw e;
        }
      }
      return [200, { name, size: await sizeOf(path) }];
    });
  }

  async function handle(req, res) {
    const url = new URL(req.url, 'http://x');
    if (req.method === 'GET' && url.pathname === '/api/health') return send(res, 200, { ok: true });
    if (!authorised(req)) return send(res, 401, { error: 'bad or missing token' });

    if (req.method === 'GET' && url.pathname === '/api/files') return send(res, 200, await list());

    const m = /^\/api\/files\/([^/]+)$/.exec(url.pathname);
    if (m) {
      const name = decodeURIComponent(m[1]);
      if (!safeName(name)) return send(res, 400, { error: 'bad name' });
      if (req.method === 'PUT') {
        const raw = url.searchParams.get('offset');
        const offset = Number(raw);
        if (raw === null || !Number.isSafeInteger(offset) || offset < 0) {
          return send(res, 400, { error: 'offset must be a whole number of bytes' });
        }
        const [status, body] = await put(req, name, offset);
        return send(res, status, body);
      }
      // Handy for checking what arrived; the app does not need it.
      if (req.method === 'GET') {
        const path = join(dataDir, name);
        const size = await sizeOf(path);
        if (!size) return send(res, 404, { error: 'not found' });
        res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': size });
        return pipeline(createReadStream(path), res);
      }
    }
    return send(res, 404, { error: 'not found' });
  }

  return createServer((req, res) => {
    handle(req, res).catch((e) => {
      const status = e.status ?? (e.code === 'Z_DATA_ERROR' ? 400 : 500);
      if (status === 500) console.error(e);
      if (!res.headersSent) send(res, status, { error: e.message });
      else res.destroy();
      // An error mid-body leaves the rest unread; drop the connection so
      // the client is not left sending into nothing.
      req.resume();
    });
  });
}

function send(res, status, body) {
  const json = JSON.stringify(body);
  res.writeHead(status, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(json) });
  res.end(json);
}

// Run directly (not imported by the tests).
if (import.meta.url === `file://${process.argv[1]}`) {
  const dataDir = process.env.DATA_DIR ?? '/data';
  const port = Number(process.env.PORT ?? 8080);
  const token = process.env.TOKEN;
  if (!token) {
    console.error('TOKEN is not set - refusing to start an open log store.');
    process.exit(1);
  }
  await mkdir(dataDir, { recursive: true });
  // Leftovers of a replacement interrupted by a restart.
  for (const n of await readdir(dataDir)) if (n.endsWith('.part')) await unlink(join(dataDir, n));
  // Check the directory is writable now rather than at the first upload.
  try {
    await (await open(join(dataDir, '.write-test'), 'w')).close();
    await unlink(join(dataDir, '.write-test'));
  } catch (e) {
    console.error(
      `${dataDir} is not writable (${e.code}). With compose's ./logs bind mount: ` +
        'create ./logs yourself before the first `up` - otherwise Docker creates it owned by root.',
    );
    process.exit(1);
  }
  createApp({ dataDir, token }).listen(port, () => console.log(`n-gauge backend on :${port}, logs in ${dataDir}`));
}
