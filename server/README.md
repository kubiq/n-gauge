# Log upload service

Where the gauge sends its logs: as the car starts, it joins the phone's
hotspot (the phone turns it on when it connects to the car's Bluetooth),
brings up a WireGuard tunnel into the home network, and uploads what the
card holds that this service does not.

The files arrive as the gauge wrote them. With a log key set, that is
encrypted to the phone's key: this service stores ciphertext it cannot read,
and the app is still where logs become readable.

## Run it

```sh
cd server
echo "UPLOAD_TOKEN=$(openssl rand -hex 16)" > .env
docker compose up -d --build
curl http://localhost:8650/health        # ok
```

The uploads go to `server/data/`, or wherever `UPLOAD_DATA` in `.env` says
(`UPLOAD_DATA=/storage/gauge`) - a folder that exists and that uid 1000, the
container's user, can write. The gauge needs the same token, and the
service's address as seen from inside the tunnel, such as
`http://gauge.example.org` behind a proxy or `http://192.168.1.10:8650`.

Without Docker: `UPLOAD_TOKEN=... python3 upload_server.py` (standard library
only; `UPLOAD_DIR`, `UPLOAD_PORT` and `UPLOAD_BIND` set the rest).

## The protocol

Plain HTTP: it runs inside the WireGuard tunnel, which encrypts and
authenticates the gauge. The token is a second lock, for the rest of the home
network. Every log only ever grows on the card, so an upload resumes by
size, as the phone's sync does:

| Request | Body | Answer |
|---|---|---|
| `POST /plan` | the gauge's files, `name size` per line | `name offset` per line for each file held shorter (offset = bytes held); a file held *longer* is another file under the same name: offset 0 |
| `PUT /f/<name>?offset=N` | the bytes from N on | the new size; `409` with the size held if N is not it; offset 0 replaces |
| `GET /health` | | `ok` (no token) |
| `GET /files` | | `name size` per line - for the phone, which pulls from here too |
| `GET /f/<name>` | | the file; with `Range: bytes=a-b` those bytes (206) |
| `DELETE /f/<name>` | | `deleted` - a drive deleted in the app: the file goes and its name is buried in `.deleted`, never asked of the gauge again (410 if sent) nor listed |
| `GET /fw/latest` | | `<app hash> <size> <build time>` of the firmware held, 404 if none |
| `GET /fw/bin`, `GET /fw/sig` | | the firmware image; its 64-byte signature |

Every request but `/health` needs `Authorization: Bearer <token>`. An upload
is written to a side file and renamed into place once complete, so a
cut-off one leaves the previous file whole and the next plan asks again.

## Firmware updates

The same service can hand the gauge a new firmware: before each upload the
gauge asks `/fw/latest`, and one newer than its own it downloads, checks and
installs once the car is parked, with the usual rollback if it misbehaves.
The files sit in `firmware/` in the data folder, put there by the release
script on the PC that builds the firmware:

```sh
scripts/release-fw.py --init                             # once: the signing key
scripts/release-fw.py --to server:/path/to/data/firmware # build, sign, copy (scp)
```

Only a firmware signed with that key is installed - so the service, or
anyone who gets into it, cannot give the gauge anything else - and only one
built later than the one running, so an old release cannot be served again.
The signature is standard Ed25519 over the image's SHA-256 and its build
time; the details are in `scripts/release-fw.py --help`. The key's public
half goes into the firmware at build time (`scripts/build.sh` reads the
`.pub` beside the key, or `FW_SIGN_PUB`): each builder has their own key,
and a firmware built without one never updates itself. The first firmware
with the key goes on as before, over USB or from the app; the app's Home
server card turns the updates off.

Tests: `python3 -m unittest server/test_upload_server.py`.
