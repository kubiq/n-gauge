"""Tests for upload_server.py: python3 -m unittest server/test_upload_server.py"""

import http.client
import json
import os
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import logcrypt
import upload_server as us


class PlanTest(unittest.TestCase):
    def test_new_short_same_and_longer(self):
        held = {"a.csv": 10, "b.gpx": 5, "c.txt": 99}
        offered = "a.csv 10\nb.gpx 8\nc.txt 50\nd.vbo 3\n"
        self.assertEqual(us.plan(held, offered), "b.gpx 5\nc.txt 0\nd.vbo 0\n")

    def test_junk_lines_and_names_are_ignored(self):
        offered = "../etc/passwd 5\n.hidden 1\nok.csv x\n\nok.csv 2 extra\nfine.csv 4\n"
        self.assertEqual(us.plan({}, offered), "fine.csv 0\n")


# The log format's shared test vector (logcrypt.h, the app's logcrypt.ts).
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "vectors", "logcrypt-v1.json")) as _f:
    VECTOR = json.load(_f)


class LogcryptTest(unittest.TestCase):
    def test_the_shared_vector(self):
        got = logcrypt.open_log(bytes.fromhex(VECTOR["file"]), bytes.fromhex(VECTOR["recipientSecret"]))
        self.assertEqual(got.decode(), VECTOR["plaintext"])

    def test_cut_short_keeps_what_is_whole(self):
        f = bytes.fromhex(VECTOR["file"])
        got = logcrypt.open_log(f[:-3], bytes.fromhex(VECTOR["recipientSecret"]))
        self.assertTrue(VECTOR["plaintext"].startswith(got.decode()))
        self.assertLess(len(got), len(VECTOR["plaintext"]))

    def test_the_wrong_key_reads_nothing(self):
        self.assertEqual(logcrypt.open_log(bytes.fromhex(VECTOR["file"]), bytes(range(32))), b"")

    def test_key_text(self):
        hexkey = VECTOR["recipientSecret"]
        self.assertEqual(logcrypt.key_from_text(f"ngauge-log-key-v1:{hexkey}\n"), bytes.fromhex(hexkey))
        self.assertEqual(logcrypt.key_from_text(hexkey), bytes.fromhex(hexkey))
        self.assertIsNone(logcrypt.key_from_text("ngauge-log-key-v1:abc"))


class FakeGitHub:
    """The releases API and, behind a redirect as on GitHub, the assets -
    Range and all."""

    def __init__(self, assets: dict):
        outer = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                if self.path == "/repos/o/r/releases/latest":
                    body = json.dumps({"tag_name": "v1", "assets": [
                        {"name": n, "browser_download_url": f"{outer.url}/dl/{n}"} for n in assets]}).encode()
                    self.send_response(200)
                elif self.path.startswith("/dl/"):
                    self.send_response(302)
                    self.send_header("Location", f"{outer.url}/blob/{self.path[4:]}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                elif self.path[6:] in assets:
                    body = assets[self.path[6:]]
                    m = us.RANGE_RE.match(self.headers.get("Range", ""))
                    if m:
                        first = int(m.group(1))
                        self.send_response(206)
                        self.send_header("Content-Range", f"bytes {first}-{len(body) - 1}/{len(body)}")
                        body = body[first:]
                    else:
                        self.send_response(200)
                else:
                    body = b""
                    self.send_response(404)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.url = f"http://127.0.0.1:{self.srv.server_address[1]}"
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.open = True

    def close(self):
        if self.open:
            self.srv.shutdown()
            self.srv.server_close()
            self.open = False


class ServerTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.srv = us.serve(self.dir.name, 0, "127.0.0.1", "tok")
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.port = self.srv.server_address[1]

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        self.dir.cleanup()

    def req(self, method, path, body=b"", token="tok"):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        headers = {"Authorization": f"Bearer {token}"} if token else {}
        c.request(method, path, body=body, headers=headers)
        r = c.getresponse()
        out = (r.status, r.read().decode())
        c.close()
        return out

    def held(self, name):
        with open(os.path.join(self.dir.name, name), "rb") as f:
            return f.read()

    def test_upload_then_resume(self):
        self.assertEqual(self.req("POST", "/plan", b"d.csv 6\n"), (200, "d.csv 0\n"))
        self.assertEqual(self.req("PUT", "/f/d.csv?offset=0", b"abc"), (200, "3\n"))
        self.assertEqual(self.req("POST", "/plan", b"d.csv 6\n"), (200, "d.csv 3\n"))
        self.assertEqual(self.req("PUT", "/f/d.csv?offset=3", b"def"), (200, "6\n"))
        self.assertEqual(self.held("d.csv"), b"abcdef")
        self.assertEqual(self.req("POST", "/plan", b"d.csv 6\n"), (200, ""))

    def test_wrong_offset_says_what_is_held(self):
        self.req("PUT", "/f/d.csv?offset=0", b"abc")
        self.assertEqual(self.req("PUT", "/f/d.csv?offset=1", b"x"), (409, "3\n"))
        self.assertEqual(self.held("d.csv"), b"abc")

    def test_offset_zero_replaces(self):
        self.req("PUT", "/f/d.csv?offset=0", b"old file")
        self.assertEqual(self.req("PUT", "/f/d.csv?offset=0", b"new"), (200, "3\n"))
        self.assertEqual(self.held("d.csv"), b"new")

    def test_token_is_needed_but_not_for_health(self):
        self.assertEqual(self.req("POST", "/plan", b"", token="wrong")[0], 401)
        self.assertEqual(self.req("PUT", "/f/d.csv?offset=0", b"x", token=None)[0], 401)
        self.assertEqual(self.req("GET", "/health", token=None), (200, "ok\n"))

    def test_the_phone_lists_and_pulls_with_range(self):
        self.req("PUT", "/f/b.gpx?offset=0", b"0123456789")
        self.req("PUT", "/f/a.csv?offset=0", b"ab")
        self.assertEqual(self.req("GET", "/files"), (200, "a.csv 2\nb.gpx 10\n"))
        self.assertEqual(self.req("GET", "/f/b.gpx"), (200, "0123456789"))
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        c.request("GET", "/f/b.gpx", headers={"Authorization": "Bearer tok", "Range": "bytes=4-6"})
        r = c.getresponse()
        self.assertEqual((r.status, r.read(), r.getheader("Content-Range")), (206, b"456", "bytes 4-6/10"))
        c.close()
        self.assertEqual(self.req("GET", "/files", token="wrong")[0], 401)
        self.assertEqual(self.req("GET", "/f/nope.csv")[0], 404)

    def test_a_deleted_drive_stays_deleted(self):
        self.req("PUT", "/f/d.csv?offset=0", b"abc")
        self.assertEqual(self.req("DELETE", "/f/d.csv"), (200, "deleted\n"))
        self.assertEqual(self.req("GET", "/files"), (200, ""))
        # The gauge still has it: not asked for, refused if sent anyway.
        self.assertEqual(self.req("POST", "/plan", b"d.csv 3\ne.csv 1\n"), (200, "e.csv 0\n"))
        self.assertEqual(self.req("PUT", "/f/d.csv?offset=0", b"abc")[0], 410)
        # Deleting what is not there buries the name all the same.
        self.assertEqual(self.req("DELETE", "/f/never.csv"), (200, "deleted\n"))
        self.assertEqual(self.req("POST", "/plan", b"never.csv 5\n"), (200, ""))
        self.assertEqual(self.req("DELETE", "/f/x.csv", token="wrong")[0], 401)

    def test_no_repo_no_firmware(self):
        self.assertEqual(self.req("GET", "/fw/latest")[0], 404)

    def test_firmware_is_passed_through_from_github(self):
        gh = FakeGitHub({"latest": b"ab12 5\n", "car_gauge_v1.bin": b"image"})
        self.addCleanup(gh.close)
        old_api = us.GITHUB_API
        us.GITHUB_API = gh.url
        us.Handler.fw_repo = "o/r"
        self.addCleanup(lambda: (setattr(us, "GITHUB_API", old_api), setattr(us.Handler, "fw_repo", "")))
        self.assertEqual(self.req("GET", "/fw/latest"), (200, "ab12 5\n"))
        self.assertEqual(self.req("GET", "/fw/bin"), (200, "image"))
        # A resumed download: the Range reaches the asset host, past a redirect.
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        c.request("GET", "/fw/bin", headers={"Authorization": "Bearer tok", "Range": "bytes=2-"})
        r = c.getresponse()
        self.assertEqual((r.status, r.read(), r.getheader("Content-Length"), r.getheader("Content-Range")),
                         (206, b"age", "3", "bytes 2-4/5"))
        c.close()
        self.assertEqual(self.req("GET", "/fw/sig")[0], 404)       # not in the release
        self.assertEqual(self.req("GET", "/fw/latest", token="wrong")[0], 401)
        gh.close()                                                  # GitHub gone
        self.assertEqual(self.req("GET", "/fw/latest")[0], 502)

    def test_plain_copies_of_encrypted_logs(self):
        # Its own server, with the shared vector's key.
        srv = us.serve(self.dir.name, 0, "127.0.0.1", "tok", log_key=bytes.fromhex(VECTOR["recipientSecret"]))
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        self.addCleanup(lambda: setattr(us.Handler, "decryptor", None))
        port, self.port = self.port, srv.server_address[1]
        self.addCleanup(lambda: setattr(self, "port", port))
        cipher = bytes.fromhex(VECTOR["file"])
        self.assertEqual(self.req("PUT", "/f/drive_x.csv.nge?offset=0", cipher)[0], 200)
        plain = os.path.join(self.dir.name, "plain", "drive_x.csv")
        for _ in range(50):
            if os.path.exists(plain):
                break
            time.sleep(0.05)
        with open(plain) as f:
            self.assertEqual(f.read(), VECTOR["plaintext"])
        # The phone sees the .nge alone; a deleted drive takes its copy along.
        self.assertEqual(self.req("GET", "/files"), (200, f"drive_x.csv.nge {len(cipher)}\n"))
        self.req("DELETE", "/f/drive_x.csv.nge")
        self.assertFalse(os.path.exists(plain))

    def test_no_way_out_of_the_folder(self):
        self.assertEqual(self.req("PUT", "/f/..%2Fx?offset=0", b"x")[0], 400)
        self.assertEqual(self.req("PUT", "/f/../x?offset=0", b"x")[0], 400)
        self.assertEqual(self.req("PUT", "/f/a.part?offset=0", b"x")[0], 400)


if __name__ == "__main__":
    unittest.main()
