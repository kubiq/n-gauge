#!/usr/bin/env python3
"""The gauge's log upload service.

The gauge sends its card's log files here over the phone's hotspot and a
WireGuard tunnel into the home network, as the car starts. The files stay
as the gauge wrote them - its logs are encrypted to the phone's key, so
this service stores ciphertext it cannot read. See README.md.

Every log only ever grows on the card (a reopened file gets a new segment
appended), so an upload resumes by size, as the phone's sync does:

  POST /plan            the gauge's files, one "name size" per line; the
                        answer is one "name offset" per line for each file
                        this side holds less of (offset = what it holds).
                        A file it holds MORE of is a different file under
                        the same name (a new card): offset 0, replaced.
  PUT  /f/<name>?offset=N
                        the bytes from N on, appended. N must be what is
                        held - 409 with the size held otherwise, so the
                        gauge can carry on from there. Offset 0 replaces.
  GET  /health          "ok", for a check from a browser.

With UPLOAD_TOKEN set, every request but /health needs
"Authorization: Bearer <token>" - a second lock behind the tunnel's.

Standard library only. Settings from the environment:
  UPLOAD_DIR    where the files go (default ./data)
  UPLOAD_PORT   port (default 8650)
  UPLOAD_BIND   address (default 0.0.0.0)
  UPLOAD_TOKEN  the shared token (default: none, no check)
"""

import hmac
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

# The card's own names: drive_2026-11-05_11-38_6e27f6.csv.nge, bootlog.txt.
# Nothing with a slash or a leading dot, so nothing outside UPLOAD_DIR.
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,95}$")
MAX_PLAN = 256 * 1024          # bytes of plan: ~5000 files
CHUNK = 64 * 1024


def plan(held: dict, offered: str) -> str:
    """The "name offset" lines for what `offered` has beyond `held`."""
    out = []
    for line in offered.splitlines():
        parts = line.split()
        if len(parts) != 2 or not NAME_RE.match(parts[0]) or not parts[1].isdigit():
            continue
        name, size = parts[0], int(parts[1])
        have = held.get(name, 0)
        if have > size:
            out.append(f"{name} 0")
        elif have < size:
            out.append(f"{name} {have}")
    return "".join(line + "\n" for line in out)


class Handler(BaseHTTPRequestHandler):
    server_version = "n-gauge-upload/1"
    # Set by serve().
    root = "data"
    token = ""

    def _reply(self, code: int, text: str = "") -> None:
        body = text.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _authorised(self) -> bool:
        if not self.token:
            return True
        got = self.headers.get("Authorization", "")
        return hmac.compare_digest(got.encode(), f"Bearer {self.token}".encode())

    def _length(self):
        try:
            n = int(self.headers.get("Content-Length", ""))
        except ValueError:
            return None
        return n if n >= 0 else None

    def _held(self) -> dict:
        held = {}
        for entry in os.scandir(self.root):
            if entry.is_file() and NAME_RE.match(entry.name) and not entry.name.endswith(".part"):
                held[entry.name] = entry.stat().st_size
        return held

    def do_GET(self):
        if urlsplit(self.path).path == "/health":
            return self._reply(200, "ok\n")
        self._reply(404, "not found\n")

    def do_POST(self):
        if urlsplit(self.path).path != "/plan":
            return self._reply(404, "not found\n")
        if not self._authorised():
            return self._reply(401, "token\n")
        n = self._length()
        if n is None or n > MAX_PLAN:
            return self._reply(413, "plan too long\n")
        offered = self.rfile.read(n).decode("utf-8", "replace")
        self._reply(200, plan(self._held(), offered))

    def do_PUT(self):
        url = urlsplit(self.path)
        if not url.path.startswith("/f/"):
            return self._reply(404, "not found\n")
        if not self._authorised():
            return self._reply(401, "token\n")
        name = url.path[3:]
        if not NAME_RE.match(name) or name.endswith(".part"):
            return self._reply(400, "bad name\n")
        try:
            offset = int(parse_qs(url.query).get("offset", [""])[0])
        except ValueError:
            return self._reply(400, "offset\n")
        n = self._length()
        if n is None:
            return self._reply(411, "length\n")
        path = os.path.join(self.root, name)
        have = os.path.getsize(path) if os.path.exists(path) else 0
        if offset != 0 and offset != have:
            return self._reply(409, f"{have}\n")
        # Into a side file, renamed over the real one once all arrived, so
        # a cut-off upload never leaves a half-appended log: the next plan
        # still sees the old size and asks for the same bytes again.
        part = path + ".part"
        try:
            with open(part, "wb") as out:
                if offset:
                    with open(path, "rb") as old:
                        while chunk := old.read(CHUNK):
                            out.write(chunk)
                left = n
                while left:
                    chunk = self.rfile.read(min(CHUNK, left))
                    if not chunk:
                        raise ConnectionError("cut off")
                    out.write(chunk)
                    left -= len(chunk)
                out.flush()
                os.fsync(out.fileno())
            os.replace(part, path)
        except ConnectionError as e:       # the gauge went: nobody to answer
            os.remove(part)
            self.log_message("PUT %s failed: %s", name, e)
            return None
        except OSError as e:
            if os.path.exists(part):
                os.remove(part)
            self.log_message("PUT %s failed: %s", name, e)
            return self._reply(500, "write failed\n")
        self.log_message("PUT %s +%d -> %d", name, n, offset + n)
        self._reply(200, f"{offset + n}\n")


def serve(root: str, port: int, bind: str, token: str) -> ThreadingHTTPServer:
    os.makedirs(root, exist_ok=True)
    Handler.root, Handler.token = root, token
    return ThreadingHTTPServer((bind, port), Handler)


def main() -> None:
    root = os.environ.get("UPLOAD_DIR", "data")
    port = int(os.environ.get("UPLOAD_PORT", "8650"))
    bind = os.environ.get("UPLOAD_BIND", "0.0.0.0")
    token = os.environ.get("UPLOAD_TOKEN", "")
    srv = serve(root, port, bind, token)
    print(f"n-gauge upload: {root} on {bind}:{port}, token {'on' if token else 'OFF'}",
          file=sys.stderr, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
