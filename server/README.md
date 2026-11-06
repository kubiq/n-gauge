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

The uploads go to `server/data/`. The gauge needs the same token, and the
service's address as seen from inside the tunnel, such as
`http://192.168.1.10:8650`.

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

Every request but `/health` needs `Authorization: Bearer <token>`. An upload
is written to a side file and renamed into place once complete, so a
cut-off one leaves the previous file whole and the next plan asks again.

Tests: `python3 -m unittest server/test_upload_server.py`.
