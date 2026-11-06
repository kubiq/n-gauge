"""Tests for upload_server.py: python3 -m unittest server/test_upload_server.py"""

import http.client
import os
import tempfile
import threading
import unittest

import upload_server as us


class PlanTest(unittest.TestCase):
    def test_new_short_same_and_longer(self):
        held = {"a.csv": 10, "b.gpx": 5, "c.txt": 99}
        offered = "a.csv 10\nb.gpx 8\nc.txt 50\nd.vbo 3\n"
        self.assertEqual(us.plan(held, offered), "b.gpx 5\nc.txt 0\nd.vbo 0\n")

    def test_junk_lines_and_names_are_ignored(self):
        offered = "../etc/passwd 5\n.hidden 1\nok.csv x\n\nok.csv 2 extra\nfine.csv 4\n"
        self.assertEqual(us.plan({}, offered), "fine.csv 0\n")


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

    def test_no_way_out_of_the_folder(self):
        self.assertEqual(self.req("PUT", "/f/..%2Fx?offset=0", b"x")[0], 400)
        self.assertEqual(self.req("PUT", "/f/../x?offset=0", b"x")[0], 400)
        self.assertEqual(self.req("PUT", "/f/a.part?offset=0", b"x")[0], 400)


if __name__ == "__main__":
    unittest.main()
