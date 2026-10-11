# SPDX-License-Identifier: BSD-3-Clause
"""Tests of the Python bindings against tests/server/test_server.py.

Run with libcrnet built:
    CRNET_LIBRARY=build/lib/libcrnet.so.0 PYTHONPATH=bindings/python \\
        python3 -m unittest discover -s bindings/python/tests
"""

import hashlib
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

import crnet

SERVER = os.environ.get("CRNET_TEST_SERVER") or str(
    pathlib.Path(__file__).resolve().parents[3] / "tests" / "server" /
    "test_server.py")

_server = None
ports = {}


def setUpModule():
    global _server
    _server = subprocess.Popen([sys.executable, "-I", SERVER],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    ports.update(json.loads(_server.stdout.readline()))


def tearDownModule():
    _server.stdin.close()
    _server.terminate()
    _server.wait()
    _server.stdout.close()


def url(path, port="https", scheme=None, host="localhost"):
    scheme = scheme or ("http" if port == "http" else "https")
    return "%s://%s:%d%s" % (scheme, host, ports[port], path)


def header_of(echo, name):
    for k, v in echo["headers"]:
        if k.lower() == name.lower():
            return v
    return None


class ClientTest(unittest.TestCase):
    def setUp(self):
        self.c = crnet.Client(insecure=True)

    def tearDown(self):
        self.c.close()


class Library(unittest.TestCase):
    def test_versions(self):
        major, minor, _ = (int(x) for x in crnet.version().split("."))
        self.assertGreaterEqual((major, minor), (0, 23))
        self.assertRegex(crnet.chromium_version(), r"^\d+\.\d+\.\d+\.\d+$")
        self.assertIn("Chrome/", crnet.default_user_agent())
        self.assertIn("Windows NT 10.0",
                      crnet.platform_user_agent(crnet.PLATFORM_WINDOWS))
        with self.assertRaises(ValueError):
            crnet.platform_user_agent(42)

    def test_bad_options(self):
        with self.assertRaises(TypeError):
            crnet.Client(no_such_option=1)
        with self.assertRaises(ValueError):
            crnet.Client(proxy="ftp://proxy:21")
        with self.assertRaises(ValueError):
            crnet.Client(local_address="not-an-ip")

    def test_closed_client(self):
        c = crnet.Client()
        c.close()
        c.close()
        with self.assertRaises(crnet.Error) as cm:
            c.get(url("/echo"))
        self.assertEqual(cm.exception.code, crnet.E_STATE)


class Requests(ClientTest):
    def test_get_over_http2(self):
        r = self.c.get(url("/echo"))
        self.assertEqual(r.status, 200)
        self.assertTrue(r.ok)
        self.assertEqual(r.protocol, "h2")
        self.assertEqual(r.url, url("/echo"))
        e = r.json()
        self.assertEqual(e["method"], "GET")
        self.assertEqual(header_of(e, "user-agent"),
                         crnet.default_user_agent())
        self.assertEqual(r.header("Content-Type"), "application/json")

    def test_http1_and_plain_http(self):
        self.assertEqual(self.c.get(url("/echo", "https_h1")).protocol,
                         "http/1.1")
        self.assertEqual(self.c.get(url("/echo", "http")).status, 200)

    def test_headers_override_and_remove(self):
        r = self.c.get(url("/echo"), mode=crnet.MODE_RAW,
                       headers=[("X-Test", "1"), ("User-Agent", None)])
        e = r.json()
        self.assertEqual(header_of(e, "x-test"), "1")
        self.assertIsNone(header_of(e, "user-agent"))

    def test_bodies(self):
        e = self.c.post(url("/echo"), body=b"\x00bin\xff").json()
        self.assertEqual(e["body_len"], 5)
        e = self.c.post(url("/echo"), body="h\u00e9").json()
        self.assertEqual(e["body"], "h\u00e9")

    def test_body_from_file(self):
        data = os.urandom(300000)
        with tempfile.NamedTemporaryFile() as f:
            f.write(data)
            f.flush()
            d = self.c.post(url("/upload-digest"),
                            body=pathlib.Path(f.name)).json()
        self.assertEqual(d["len"], len(data))
        self.assertEqual(d["sha256"], hashlib.sha256(data).hexdigest())

    def test_streamed_upload(self):
        chunks = [os.urandom(100000) for _ in range(30)]
        d = self.c.post(url("/upload-digest", "https_h1"),
                        body=iter(chunks)).json()
        whole = b"".join(chunks)
        self.assertEqual(d["len"], len(whole))
        self.assertEqual(d["sha256"], hashlib.sha256(whole).hexdigest())
        self.assertEqual(d["transfer_encoding"], "chunked")

        d = self.c.post(url("/upload-digest", "https_h1"),
                        body=iter(chunks), body_length=len(whole)).json()
        self.assertEqual(d["content_length"], str(len(whole)))

        with self.assertRaises(ValueError):
            self.c.post(url("/upload-digest"), body=iter(chunks),
                        body_length=10)

    def test_streamed_download(self):
        n = 5 * 1024 * 1024
        total = 0
        with self.c.stream("GET", url("/bytes/%d" % n)) as t:
            self.assertEqual(t.wait_response().status, 200)
            for chunk in t:
                total += len(chunk)
        self.assertEqual(total, n)

    def test_status_is_not_an_exception(self):
        r = self.c.get(url("/status/404"))
        self.assertEqual(r.status, 404)
        self.assertFalse(r.ok)

    def test_redirects(self):
        r = self.c.get(url("/redirect/2"))
        self.assertEqual(r.redirect_count, 2)
        self.assertEqual(r.url, url("/echo"))

    def test_repeated_headers(self):
        r = self.c.get(url("/dup-headers"))
        self.assertEqual(r.header_all("x-dup"), ["one", "two"])

    def test_decoding(self):
        r = self.c.get(url("/encoded/gzip,br"))
        self.assertEqual(r.decoded_codings, "gzip, br")

    def test_network_error(self):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        with self.assertRaises(crnet.Error) as cm:
            self.c.get("http://127.0.0.1:%d/" % port)
        self.assertEqual(cm.exception.code, crnet.E_NET)
        self.assertEqual(cm.exception.net_error, -102)  # REFUSED

    def test_timeout(self):
        with self.assertRaises(crnet.Timeout) as cm:
            self.c.get(url("/delay/3000"), timeout=0.2)
        self.assertEqual(cm.exception.code, crnet.E_TIMEOUT)

    def test_timeout_while_streaming(self):
        with self.c.stream("GET", url("/stream/100?chunk=10&delay_ms=500"),
                           timeout=0.3) as t:
            self.assertEqual(t.wait_response().status, 200)
            with self.assertRaises(crnet.Timeout):
                for _ in t:
                    pass

    def test_threads_share_a_client(self):
        errors = []

        def work():
            try:
                for _ in range(10):
                    self.assertEqual(self.c.get(url("/echo")).status, 200)
            except Exception as e:  # noqa: BLE001
                errors.append(e)

        threads = [threading.Thread(target=work) for _ in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(errors, [])


class Configuration(unittest.TestCase):
    def test_platform_profile(self):
        with crnet.Client(insecure=True,
                          platform_profile=crnet.PLATFORM_MACOS) as c:
            e = c.get(url("/echo")).json()
        self.assertEqual(header_of(e, "user-agent"),
                         crnet.platform_user_agent(crnet.PLATFORM_MACOS))
        self.assertEqual(header_of(e, "sec-ch-ua-platform"), '"macOS"')

    def test_trust_anchors(self):
        with crnet.Client(trust_anchors_file=ports["ca"]) as c:
            self.assertEqual(c.get(url("/echo")).status, 200)
        with crnet.Client() as c:
            with self.assertRaises(crnet.Error) as cm:
                c.get(url("/echo"))
            self.assertEqual(cm.exception.code, crnet.E_NET)

    def test_local_address(self):
        with crnet.Client(local_address="127.0.0.2") as c:
            e = c.get(url("/echo", "http", host="127.0.0.1")).json()
        self.assertEqual(e["peer_addr"], "127.0.0.2")


class Cookies(ClientTest):
    def test_jar(self):
        self.c.get(url("/set-cookie?a=1&b=2"))
        self.assertEqual(self.c.get_cookies(url("/")), "a=1; b=2")
        self.c.set_cookie(url("/"), "c=3")
        self.assertIn("c=3", self.c.get_cookies(url("/")))

        saved = self.c.export_cookies()
        self.assertEqual(len(json.loads(saved)), 3)
        self.c.clear_cookies()
        self.assertEqual(self.c.get_cookies(url("/")), "")
        self.assertEqual(self.c.import_cookies(saved), 3)
        self.assertIn("a=1", self.c.get_cookies(url("/")))
        self.assertIn("localhost",
                      self.c.export_cookies(crnet.COOKIES_NETSCAPE))


class WebSockets(ClientTest):
    def test_echo(self):
        with self.c.websocket(url("/ws/echo", "http", "ws"),
                              protocols="chat") as ws:
            self.assertEqual(ws.protocol, "chat")
            self.assertEqual(ws.state, crnet.WS_OPEN)
            ws.send("hello")
            self.assertEqual(ws.recv(timeout=5), "hello")
            ws.send(b"\x00\x01")
            self.assertEqual(ws.recv(timeout=5), b"\x00\x01")
            ws.ping(b"p")
            ws.close(1000, "bye")
            ws.wait_closed(5)
            self.assertEqual(ws.state, crnet.WS_CLOSED)
            self.assertEqual(ws.close_code, 1000)
            self.assertIsNone(ws.recv(timeout=1))

    def test_server_close_ends_iteration(self):
        with self.c.websocket(url("/ws/close?code=4001&reason=done",
                                  "http", "ws")) as ws:
            self.assertEqual(list(ws), [])
            self.assertEqual(ws.close_code, 4001)
            self.assertEqual(ws.close_reason, "done")

    def test_recv_timeout(self):
        with self.c.websocket(url("/ws/echo", "http", "ws")) as ws:
            with self.assertRaises(crnet.Timeout):
                ws.recv(timeout=0.1)

    def test_refused_handshake(self):
        with self.assertRaises(crnet.Error) as cm:
            self.c.websocket(url("/ws/reject", "http", "ws"), timeout=5)
        self.assertIsNotNone(cm.exception.response)
        self.assertEqual(cm.exception.response.status, 403)


if __name__ == "__main__":
    unittest.main()
