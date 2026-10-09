#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Local test server for the crnet test suite.

Listens on 127.0.0.1 (ephemeral ports) and serves:
  * HTTPS with ALPN h2 + http/1.1 (self-signed certificate)
  * HTTPS restricted to http/1.1
  * plain HTTP/1.1
  * a SOCKS5 proxy (no auth, CONNECT only, remote name resolution)
  * an HTTP CONNECT proxy requiring Basic auth (user "crnet", pass "secret")

On start-up it prints one JSON line with the ports to stdout, then runs
until stdin is closed or it receives SIGTERM.

Endpoints (all servers):
  /echo                 JSON description of the request, headers in
                        received order (works for any method)
  /status/<code>        empty response with that status
  /redirect/<n>         302 chain of length n ending in /echo
  /redirect-to?url=U[&code=C]  redirect to U
  /set-cookie?k=v&...   one Set-Cookie per pair
  /bytes/<n>            n bytes of a deterministic pattern
  /stream/<n>?chunk=C&delay_ms=D  n bytes in chunks with delays
  /delay/<ms>           waits, then answers like /echo
  /encoded/<enc>        PLAINTEXT compressed with gzip|deflate|br|zstd
  /dup-headers          repeated response headers
  /drop                 closes the connection without a response
"""
import asyncio
import base64
import gzip
import json
import os
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import zlib
from urllib.parse import parse_qsl, urlsplit

import h2.config
import h2.connection
import h2.events
import h2.exceptions

try:
    import brotli
except ImportError:  # pragma: no cover
    brotli = None

PLAINTEXT = (b"crnet content-encoding test payload. " * 200)
PROXY_USER = "crnet"
PROXY_PASS = "secret"
STATS = {"socks5_connects": 0, "http_proxy_connects": 0}


def pattern(n):
    block = bytes(range(256))
    reps, rem = divmod(n, 256)
    return block * reps + block[:rem]


def compress(enc, data):
    if enc == "gzip":
        return gzip.compress(data)
    if enc == "deflate":
        return zlib.compress(data)
    if enc == "br" and brotli:
        return brotli.compress(data)
    if enc == "zstd":
        return subprocess.run(["zstd", "-q", "-c"], input=data,
                              stdout=subprocess.PIPE, check=True).stdout
    return None


class Request:
    def __init__(self, method, target, headers, body, protocol, tls,
                 peer_port=0):
        self.peer_port = peer_port
        self.method = method
        self.target = target
        self.headers = headers  # list of (name, value) str, received order
        self.body = body
        self.protocol = protocol
        self.tls = tls
        parts = urlsplit(target)
        self.path = parts.path
        self.query = parse_qsl(parts.query, keep_blank_values=True)

    def header(self, name):
        name = name.lower()
        for k, v in self.headers:
            if k.lower() == name:
                return v
        return None


class Response:
    def __init__(self, status=200, headers=None, body=b"", chunks=None,
                 delay=0.0, drop=False):
        self.status = status
        self.headers = headers or []
        self.body = body
        self.chunks = chunks      # list of (delay_seconds, bytes)
        self.delay = delay
        self.drop = drop


def echo_response(req):
    try:
        text = req.body.decode()
        body_field = {"body": text}
    except UnicodeDecodeError:
        body_field = {"body_b64": base64.b64encode(req.body).decode()}
    doc = {
        "method": req.method,
        "target": req.target,
        "path": req.path,
        "protocol": req.protocol,
        "tls": req.tls,
        "headers": [[k, v] for k, v in req.headers],
        "body_len": len(req.body),
        "peer_port": req.peer_port,
        "stats": STATS,
    }
    doc.update(body_field)
    data = json.dumps(doc).encode()
    return Response(200, [("content-type", "application/json")], data)


def route(req):
    p = req.path
    q = dict(req.query)
    seg = p.strip("/").split("/")
    if p == "/echo":
        return echo_response(req)
    if seg[0] == "status" and len(seg) == 2:
        return Response(int(seg[1]), [("content-type", "text/plain")],
                        b"" if seg[1] in ("204", "304") else b"status")
    if seg[0] == "redirect" and len(seg) == 2:
        n = int(seg[1])
        loc = "/echo" if n <= 1 else "/redirect/%d" % (n - 1)
        return Response(302, [("location", loc)], b"")
    if p == "/redirect-to":
        return Response(int(q.get("code", "302")), [("location", q["url"])],
                        b"")
    if p == "/set-cookie":
        hdrs = [("set-cookie", "%s=%s; Path=/" % (k, v)) for k, v in req.query]
        hdrs.append(("content-type", "text/plain"))
        return Response(200, hdrs, b"cookies set")
    if seg[0] == "bytes" and len(seg) == 2:
        return Response(200, [("content-type", "application/octet-stream")],
                        pattern(int(seg[1])))
    if seg[0] == "stream" and len(seg) == 2:
        n = int(seg[1])
        chunk = int(q.get("chunk", "1024"))
        delay = int(q.get("delay_ms", "10")) / 1000.0
        data = pattern(n)
        chunks = [(delay, data[i:i + chunk]) for i in range(0, n, chunk)]
        return Response(200, [("content-type", "application/octet-stream")],
                        chunks=chunks)
    if seg[0] == "delay" and len(seg) == 2:
        r = echo_response(req)
        r.delay = int(seg[1]) / 1000.0
        return r
    if seg[0] == "encoded" and len(seg) == 2:
        data = compress(seg[1], PLAINTEXT)
        if data is None:
            return Response(501, [], b"unsupported encoding")
        return Response(200, [("content-type", "text/plain"),
                              ("content-encoding", seg[1])], data)
    if p == "/dup-headers":
        return Response(200, [("x-dup", "one"), ("x-dup", "two"),
                              ("x-Mixed-Case", "yes"),
                              ("content-type", "text/plain")], b"dup")
    if p == "/drop":
        return Response(drop=True)
    return Response(404, [("content-type", "text/plain")], b"not found")


# ----------------------------------------------------------------------
# HTTP/1.1
# ----------------------------------------------------------------------

REASONS = {200: "OK", 204: "No Content", 301: "Moved Permanently",
           302: "Found", 303: "See Other", 307: "Temporary Redirect",
           308: "Permanent Redirect", 304: "Not Modified",
           401: "Unauthorized", 403: "Forbidden", 404: "Not Found",
           407: "Proxy Authentication Required", 418: "I'm a teapot",
           500: "Internal Server Error", 501: "Not Implemented",
           503: "Service Unavailable"}


async def read_h1_request(reader):
    line = await reader.readline()
    if not line:
        return None
    method, target, _ = line.decode("latin-1").rstrip("\r\n").split(" ", 2)
    headers = []
    while True:
        h = await reader.readline()
        if h in (b"\r\n", b"\n", b""):
            break
        k, v = h.decode("latin-1").rstrip("\r\n").split(":", 1)
        headers.append((k, v.strip()))
    body = b""
    lower = {k.lower(): v for k, v in headers}
    if "content-length" in lower:
        body = await reader.readexactly(int(lower["content-length"]))
    elif lower.get("transfer-encoding", "").lower() == "chunked":
        while True:
            size = int((await reader.readline()).split(b";")[0], 16)
            if size == 0:
                await reader.readline()
                break
            body += await reader.readexactly(size)
            await reader.readline()
    return method, target, headers, body


async def serve_h1(reader, writer, tls):
    peer_port = writer.get_extra_info("peername")[1]
    try:
        while True:
            parsed = await read_h1_request(reader)
            if parsed is None:
                break
            method, target, headers, body = parsed
            req = Request(method, target, headers, body, "http/1.1", tls,
                          peer_port)
            resp = route(req)
            if resp.delay:
                await asyncio.sleep(resp.delay)
            if resp.drop:
                break
            head = "HTTP/1.1 %d %s\r\n" % (resp.status,
                                          REASONS.get(resp.status, "Status"))
            for k, v in resp.headers:
                head += "%s: %s\r\n" % (k, v)
            if resp.chunks is None:
                head += "Content-Length: %d\r\n\r\n" % len(resp.body)
                writer.write(head.encode("latin-1"))
                if method != "HEAD":
                    writer.write(resp.body)
            else:
                head += "Transfer-Encoding: chunked\r\n\r\n"
                writer.write(head.encode("latin-1"))
                for delay, data in resp.chunks:
                    await asyncio.sleep(delay)
                    writer.write(b"%x\r\n%s\r\n" % (len(data), data))
                    await writer.drain()
                writer.write(b"0\r\n\r\n")
            await writer.drain()
    except (ConnectionError, asyncio.IncompleteReadError, ValueError):
        pass
    finally:
        writer.close()


# ----------------------------------------------------------------------
# HTTP/2
# ----------------------------------------------------------------------

class H2Session:
    def __init__(self, reader, writer):
        self.reader = reader
        self.writer = writer
        # python-h2 would join HTTP/2 cookie crumbs *at the end* of the
        # header list; keep the wire order and join them in place.
        cfg = h2.config.H2Configuration(client_side=False,
                                        header_encoding="utf-8",
                                        validate_inbound_headers=False,
                                        normalize_inbound_headers=False)
        self.conn = h2.connection.H2Connection(config=cfg)
        self.streams = {}
        self.window_waiters = {}

    def flush(self):
        data = self.conn.data_to_send()
        if data:
            self.writer.write(data)

    async def run(self):
        self.conn.initiate_connection()
        self.flush()
        try:
            while True:
                data = await self.reader.read(65536)
                if not data:
                    break
                try:
                    events = self.conn.receive_data(data)
                except h2.exceptions.ProtocolError:
                    self.flush()
                    break
                for ev in events:
                    self.on_event(ev)
                self.flush()
                await self.writer.drain()
        except ConnectionError:
            pass
        finally:
            for fut in self.window_waiters.values():
                if not fut.done():
                    fut.set_result(None)
            self.writer.close()

    def on_event(self, ev):
        if isinstance(ev, h2.events.RequestReceived):
            headers, crumbs, at = [], [], None
            for k, v in ev.headers:
                if k == "cookie":
                    if at is None:
                        at = len(headers)
                        headers.append(None)
                    crumbs.append(v)
                else:
                    headers.append((k, v))
            if at is not None:
                headers[at] = ("cookie", "; ".join(crumbs))
            self.streams[ev.stream_id] = [headers, b""]
        elif isinstance(ev, h2.events.DataReceived):
            self.streams[ev.stream_id][1] += ev.data
            self.conn.acknowledge_received_data(ev.flow_controlled_length,
                                                ev.stream_id)
        elif isinstance(ev, h2.events.StreamEnded):
            headers, body = self.streams.pop(ev.stream_id)
            asyncio.ensure_future(self.respond(ev.stream_id, headers, body))
        elif isinstance(ev, h2.events.WindowUpdated):
            for sid, fut in list(self.window_waiters.items()):
                if not fut.done():
                    fut.set_result(None)
        elif isinstance(ev, h2.events.StreamReset):
            self.streams.pop(ev.stream_id, None)

    async def send_data(self, sid, data, end):
        while data:
            window = min(self.conn.local_flow_control_window(sid),
                         self.conn.max_outbound_frame_size)
            if window <= 0:
                fut = asyncio.get_event_loop().create_future()
                self.window_waiters[sid] = fut
                await fut
                continue
            chunk, data = data[:window], data[window:]
            self.conn.send_data(sid, chunk, end_stream=end and not data)
            self.flush()
            await self.writer.drain()

    async def respond(self, sid, headers, body):
        pseudo = {k: v for k, v in headers if k.startswith(":")}
        req = Request(pseudo.get(":method"), pseudo.get(":path"), headers,
                      body, "h2", True,
                      self.writer.get_extra_info("peername")[1])
        req.headers = headers  # keep pseudo headers for order checks
        resp = route(req)
        try:
            if resp.delay:
                await asyncio.sleep(resp.delay)
            if resp.drop:
                self.conn.reset_stream(sid)
                self.flush()
                return
            hdrs = [(":status", str(resp.status))] + resp.headers
            if resp.chunks is None:
                hdrs.append(("content-length", str(len(resp.body))))
                no_body = not resp.body or req.method == "HEAD"
                self.conn.send_headers(sid, hdrs, end_stream=no_body)
                self.flush()
                if not no_body:
                    await self.send_data(sid, resp.body, True)
            else:
                self.conn.send_headers(sid, hdrs)
                self.flush()
                for delay, data in resp.chunks:
                    await asyncio.sleep(delay)
                    await self.send_data(sid, data, False)
                self.conn.end_stream(sid)
            self.flush()
            await self.writer.drain()
        except (h2.exceptions.StreamClosedError, ConnectionError,
                h2.exceptions.ProtocolError):
            pass


async def serve_tls(reader, writer):
    ssl_obj = writer.get_extra_info("ssl_object")
    proto = ssl_obj.selected_alpn_protocol() if ssl_obj else None
    if proto == "h2":
        await H2Session(reader, writer).run()
    else:
        await serve_h1(reader, writer, True)


# ----------------------------------------------------------------------
# Proxies
# ----------------------------------------------------------------------

async def pipe(src, dst):
    try:
        while True:
            data = await src.read(65536)
            if not data:
                break
            dst.write(data)
            await dst.drain()
    except ConnectionError:
        pass
    finally:
        try:
            dst.close()
        except Exception:
            pass


async def tunnel(reader, writer, host, port):
    up_r, up_w = await asyncio.open_connection(host, port)
    await asyncio.gather(pipe(reader, up_w), pipe(up_r, writer))


async def serve_socks5(reader, writer):
    try:
        ver, nmethods = await reader.readexactly(2)
        await reader.readexactly(nmethods)
        if ver != 5:
            writer.close()
            return
        writer.write(b"\x05\x00")
        ver, cmd, _, atyp = await reader.readexactly(4)
        if atyp == 1:
            host = socket.inet_ntoa(await reader.readexactly(4))
        elif atyp == 3:
            n = (await reader.readexactly(1))[0]
            host = (await reader.readexactly(n)).decode()
        elif atyp == 4:
            host = socket.inet_ntop(socket.AF_INET6,
                                    await reader.readexactly(16))
        else:
            writer.close()
            return
        port = struct.unpack("!H", await reader.readexactly(2))[0]
        if cmd != 1:
            writer.write(b"\x05\x07\x00\x01" + b"\x00" * 6)
            writer.close()
            return
        # Names under .crnet-test resolve to localhost through the proxy
        # only, proving that resolution happened remotely.
        target = "127.0.0.1" if host.endswith(".crnet-test") else host
        STATS["socks5_connects"] += 1
        writer.write(b"\x05\x00\x00\x01" + b"\x00" * 6)
        await writer.drain()
        await tunnel(reader, writer, target, port)
    except (ConnectionError, asyncio.IncompleteReadError, OSError):
        writer.close()


async def serve_http_proxy(reader, writer):
    try:
        parsed = await read_h1_request(reader)
        if parsed is None:
            writer.close()
            return
        method, target, headers, _ = parsed
        auth = None
        for k, v in headers:
            if k.lower() == "proxy-authorization":
                auth = v
        want = "Basic " + base64.b64encode(
            ("%s:%s" % (PROXY_USER, PROXY_PASS)).encode()).decode()
        if auth != want:
            writer.write(b"HTTP/1.1 407 Proxy Authentication Required\r\n"
                         b"Proxy-Authenticate: Basic realm=\"crnet\"\r\n"
                         b"Content-Length: 0\r\n\r\n")
            await writer.drain()
            # Keep the connection for the authenticated retry.
            await serve_http_proxy(reader, writer)
            return
        if method != "CONNECT":
            writer.write(b"HTTP/1.1 405 Method Not Allowed\r\n"
                         b"Content-Length: 0\r\n\r\n")
            writer.close()
            return
        host, port = target.rsplit(":", 1)
        target_host = "127.0.0.1" if host.endswith(".crnet-test") else host
        STATS["http_proxy_connects"] += 1
        writer.write(b"HTTP/1.1 200 Connection Established\r\n\r\n")
        await writer.drain()
        await tunnel(reader, writer, target_host, int(port))
    except (ConnectionError, asyncio.IncompleteReadError, OSError,
            ValueError):
        writer.close()


# ----------------------------------------------------------------------

def make_cert(tmpdir):
    key = os.path.join(tmpdir, "key.pem")
    crt = os.path.join(tmpdir, "cert.pem")
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
         "ec_paramgen_curve:prime256v1", "-nodes", "-days", "2",
         "-subj", "/CN=crnet test", "-keyout", key, "-out", crt,
         "-addext",
         "subjectAltName=DNS:localhost,DNS:*.crnet-test,IP:127.0.0.1"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return crt, key


def tls_context(crt, key, alpn):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(crt, key)
    ctx.set_alpn_protocols(alpn)
    return ctx


async def main():
    tmpdir = tempfile.mkdtemp(prefix="crnet-test-")
    crt, key = make_cert(tmpdir)
    host = "127.0.0.1"
    servers = {
        "https": await asyncio.start_server(
            serve_tls, host, 0, ssl=tls_context(crt, key, ["h2", "http/1.1"])),
        "https_h1": await asyncio.start_server(
            serve_tls, host, 0, ssl=tls_context(crt, key, ["http/1.1"])),
        "http": await asyncio.start_server(
            lambda r, w: serve_h1(r, w, False), host, 0),
        "socks5": await asyncio.start_server(serve_socks5, host, 0),
        "http_proxy": await asyncio.start_server(serve_http_proxy, host, 0),
    }
    ports = {k: s.sockets[0].getsockname()[1] for k, s in servers.items()}
    ports["cert"] = crt
    print(json.dumps(ports), flush=True)

    loop = asyncio.get_event_loop()
    stop = loop.create_future()
    loop.add_signal_handler(signal.SIGTERM, stop.set_result, None)
    loop.add_signal_handler(signal.SIGINT, stop.set_result, None)

    def stdin_closed():
        if not sys.stdin.buffer.read1(4096) and not stop.done():
            stop.set_result(None)
    try:
        loop.add_reader(sys.stdin.fileno(), stdin_closed)
    except (PermissionError, OSError):
        pass  # stdin is a regular file or device: rely on signals only
    await stop


if __name__ == "__main__":
    asyncio.run(main())
