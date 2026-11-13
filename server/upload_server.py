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

And for the phone, which pulls the logs from here too (the app's home
server sync) - so they reach it without a trip to the gauge's WiFi:

  GET  /files           one "name size" per line, what is held.
  GET  /f/<name>        the file; with "Range: bytes=a-b" just those bytes
                        (206), as the gauge's own /dl, for a log that grew.
  GET  /fw/latest       the gauge's latest firmware: "<app hash> <size>
                        <build time> <commit>"
  GET  /fw/bin          that image (Range as /f/), and /fw/sig its Ed25519
                        signature - the latest GitHub release of
                        FW_GITHUB_REPO, passed through; 404 while there is
                        none, 502 while GitHub is not reached.
  DELETE /f/<name>      a drive deleted in the app: the file goes, and its
                        name is kept (TOMBSTONES) - never asked of the gauge
                        again nor listed, or the gauge's next upload and the
                        phone's next pull would bring it straight back.

With UPLOAD_TOKEN set, every request but /health needs
"Authorization: Bearer <token>" - a second lock behind the tunnel's.

Standard library only. Settings from the environment:
  UPLOAD_DIR    where the files go (default ./data)
  UPLOAD_PORT   port (default 8650)
  UPLOAD_BIND   address (default 0.0.0.0)
  UPLOAD_TOKEN  the shared token (default: none, no check)
  FW_GITHUB_REPO  "owner/name" whose latest release is the gauge's firmware
                (default: none, no firmware served)
"""

import hmac
import json
import os
import re
import sys
import threading
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

# The card's own names: drive_2026-11-05_11-38_6e27f6.csv.nge, bootlog.txt.
# Nothing with a slash or a leading dot, so nothing outside UPLOAD_DIR.
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,95}$")
MAX_PLAN = 256 * 1024          # bytes of plan: ~5000 files
CHUNK = 64 * 1024
# The names of deleted drives' files, one per line, in UPLOAD_DIR. A
# leading dot: no file name can be that (NAME_RE), so it is never listed.
TOMBSTONES = ".deleted"
RANGE_RE = re.compile(r"^bytes=(\d+)-(\d*)$")
# The gauge's firmware updates: the latest GitHub release of FW_GITHUB_REPO,
# passed through as asked (2026-11-13) - the image, its signature, and
# "latest" (the image's app hash, size, build time and commit, for the gauge
# to tell whether it is new). The server speaks HTTPS to GitHub, the gauge
# plain HTTP in the tunnel: it checks the signature, so it needs no TLS of
# its own. Nothing kept here; a Range goes on to GitHub, so a download
# still resumes. A release out between two requests: a mixed image, which
# the signature turns away.
FW_ASSETS = {
    "/fw/latest": "latest",
    "/fw/bin": "car_gauge_v1.bin",
    "/fw/sig": "car_gauge_v1.bin.sig",
}
GITHUB_API = "https://api.github.com"
GITHUB_S = 20               # seconds GitHub has to answer


def github(url: str, range_: str = ""):
    """A GitHub URL opened - HTTPS, redirects followed (release assets live
    on another host, which takes the Range too). Raises HTTPError."""
    headers = {"User-Agent": "n-gauge-upload"}     # an Accept for files: the API says 415
    if range_:
        headers["Range"] = range_
    return urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=GITHUB_S)


def release_asset(repo: str, name: str, open_=github):
    """The download URL of `name` in `repo`'s latest release, or None."""
    req_url = f"{GITHUB_API}/repos/{repo}/releases/latest"
    with open_(req_url) as r:
        rel = json.loads(r.read())
    return next((a["browser_download_url"] for a in rel.get("assets", []) if a.get("name") == name), None)


def plan(held: dict, offered: str, buried: frozenset = frozenset()) -> str:
    """The "name offset" lines for what `offered` has beyond `held`.
    A buried name - deleted in the app - is never asked for."""
    out = []
    for line in offered.splitlines():
        parts = line.split()
        if len(parts) != 2 or not NAME_RE.match(parts[0]) or not parts[1].isdigit():
            continue
        name, size = parts[0], int(parts[1])
        if name in buried:
            continue
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
    fw_repo = ""                   # "" - no firmware: 404
    fw_open = staticmethod(github)
    bury_lock = threading.Lock()

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

    def _buried(self) -> frozenset:
        try:
            with open(os.path.join(self.root, TOMBSTONES)) as f:
                return frozenset(line.strip() for line in f if line.strip())
        except FileNotFoundError:
            return frozenset()

    def _name(self, path: str):
        """The file name in /f/<name>, or None if it is not one."""
        if not path.startswith("/f/"):
            return None
        name = path[3:]
        return name if NAME_RE.match(name) and not name.endswith(".part") else None

    def do_GET(self):
        url = urlsplit(self.path)
        if url.path == "/health":
            return self._reply(200, "ok\n")
        if not self._authorised():
            return self._reply(401, "token\n")
        if url.path == "/files":
            held = self._held()
            return self._reply(200, "".join(f"{n} {s}\n" for n, s in sorted(held.items())))
        if url.path in FW_ASSETS:                  # the gauge's own updates
            return self._send_release(FW_ASSETS[url.path])
        name = self._name(url.path)
        if name is None:
            return self._reply(404, "not found\n")
        return self._send_file(os.path.join(self.root, name))

    def _send_release(self, asset: str):
        """The latest release's `asset` from GitHub, its status, length and
        Content-Range as GitHub gave them (the gauge reads all three)."""
        if not self.fw_repo:
            return self._reply(404, "no firmware\n")
        try:
            where = release_asset(self.fw_repo, asset, self.fw_open)
            if where is None:
                return self._reply(404, "not in the release\n")
            r = self.fw_open(where, self.headers.get("Range", ""))
        except urllib.error.HTTPError as e:        # 404: no release yet; 416: past its end
            self.log_message("firmware %s: GitHub said %d", asset, e.code)
            return self._reply(404 if e.code == 404 else e.code, f"GitHub said {e.code}\n")
        except (OSError, ValueError) as e:         # offline, timed out, junk
            self.log_message("firmware %s: GitHub not reached (%s)", asset, e)
            return self._reply(502, "GitHub not reached\n")
        with r:
            self.send_response(r.status)
            self.send_header("Content-Type", "application/octet-stream")
            for h in ("Content-Length", "Content-Range", "Accept-Ranges"):
                if r.headers.get(h):
                    self.send_header(h, r.headers[h])
            self.end_headers()
            try:
                while chunk := r.read(CHUNK):
                    self.wfile.write(chunk)
            except OSError as e:                   # either end gone: the gauge resumes
                self.log_message("firmware %s cut off: %s", asset, e)
        return None

    def _send_file(self, path: str):
        """The file, or "Range: bytes=a-b" of it (206); 404 if it is not there."""
        try:
            size = os.path.getsize(path)
        except OSError:
            return self._reply(404, "not found\n")
        first, last, code = 0, size - 1, 200
        m = RANGE_RE.match(self.headers.get("Range", ""))
        if m:
            first = int(m.group(1))
            last = min(int(m.group(2)), size - 1) if m.group(2) else size - 1
            if first >= size or last < first:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return None
            code = 206
        self.send_response(code)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(last - first + 1 if size else 0))
        self.send_header("Accept-Ranges", "bytes")
        if code == 206:
            self.send_header("Content-Range", f"bytes {first}-{last}/{size}")
        self.end_headers()
        with open(path, "rb") as f:
            f.seek(first)
            left = last - first + 1 if size else 0
            while left > 0 and (chunk := f.read(min(CHUNK, left))):
                self.wfile.write(chunk)
                left -= len(chunk)
        return None

    def do_DELETE(self):
        name = self._name(urlsplit(self.path).path)
        if name is None:
            return self._reply(404, "not found\n")
        if not self._authorised():
            return self._reply(401, "token\n")
        with self.bury_lock:
            if name not in self._buried():
                with open(os.path.join(self.root, TOMBSTONES), "a") as f:
                    f.write(name + "\n")
            try:
                os.remove(os.path.join(self.root, name))
            except FileNotFoundError:
                pass
        self.log_message("DELETE %s", name)
        self._reply(200, "deleted\n")

    def do_POST(self):
        if urlsplit(self.path).path != "/plan":
            return self._reply(404, "not found\n")
        if not self._authorised():
            return self._reply(401, "token\n")
        n = self._length()
        if n is None or n > MAX_PLAN:
            return self._reply(413, "plan too long\n")
        offered = self.rfile.read(n).decode("utf-8", "replace")
        self._reply(200, plan(self._held(), offered, self._buried()))

    def do_PUT(self):
        url = urlsplit(self.path)
        if not url.path.startswith("/f/"):
            return self._reply(404, "not found\n")
        if not self._authorised():
            return self._reply(401, "token\n")
        name = self._name(url.path)
        if name is None:
            return self._reply(400, "bad name\n")
        if name in self._buried():         # deleted in the app: not again
            return self._reply(410, "deleted\n")
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


def serve(root: str, port: int, bind: str, token: str, fw_repo: str = "") -> ThreadingHTTPServer:
    os.makedirs(root, exist_ok=True)
    Handler.root, Handler.token, Handler.fw_repo = root, token, fw_repo
    return ThreadingHTTPServer((bind, port), Handler)


def main() -> None:
    root = os.environ.get("UPLOAD_DIR", "data")
    port = int(os.environ.get("UPLOAD_PORT", "8650"))
    bind = os.environ.get("UPLOAD_BIND", "0.0.0.0")
    token = os.environ.get("UPLOAD_TOKEN", "")
    repo = os.environ.get("FW_GITHUB_REPO", "")
    srv = serve(root, port, bind, token, repo)
    print(f"n-gauge upload: {root} on {bind}:{port}, token {'on' if token else 'OFF'}, "
          f"firmware {'from ' + repo if repo else 'OFF'}", file=sys.stderr, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
