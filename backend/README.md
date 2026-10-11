# N Gauge — log store

Where the phone app uploads the pod's logs. The phone pulls new files off the
pod at the car, then sends them here over mobile data (through your VPN). The
logs end up as plain files in `./logs`, exactly as they are on the pod's card.

No npm dependencies, only Node's own modules.

## Run

```sh
cp .env.example .env
openssl rand -hex 24        # put this in .env as TOKEN, and in the app
mkdir -p logs               # before the first `up` - else Docker makes it root-owned
docker compose up -d --build
curl localhost:8080/api/health          # {"ok":true}
```

Without Docker: `TOKEN=... DATA_DIR=./logs node server.js` (Node 22+).

## API

Every call except `/api/health` needs `Authorization: Bearer <TOKEN>`.

| Call | Does |
|---|---|
| `GET /api/health` | `{"ok":true}` |
| `GET /api/files` | `[{name, size}]`, everything stored |
| `PUT /api/files/<name>?offset=N` | appends the body at byte `N`, answers `{name, size}` |
| `GET /api/files/<name>` | the file, for checking what arrived |

- `offset` must be the stored size (append) or `0` (replace: the pod's copy
  shrank, so it is a different file). Anything else is `409` with the real
  `size` - which is how a retried upload whose reply was lost is recognised
  instead of written twice.
- The body may be gzipped (`Content-Encoding: gzip`); it is stored
  uncompressed, and offsets and sizes always count uncompressed bytes.
- A failed upload leaves the file exactly as it was, and a replacement keeps
  the old copy until the new one is complete.
- Names: the pod's whitelist (`A-Z a-z 0-9 . _ -`, 40 chars at most).
- 64 MB per request, counted after decompression.

## Test

```sh
npm test
```
