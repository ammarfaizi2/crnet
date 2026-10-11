# SPDX-License-Identifier: BSD-3-Clause
"""Python bindings for crnet, an HTTP and WebSocket client on Chromium's
network stack.

The module calls libcrnet's C API through ctypes: nothing is compiled at
install time, but libcrnet.so.0 must be loadable (on the library path,
or named by the CRNET_LIBRARY environment variable).

    import crnet

    with crnet.Client() as client:
        r = client.get("https://example.com/")
        print(r.status, r.header("content-type"), len(r.body))

Times are in seconds (floats), None meaning no limit. Calls block the
calling thread only; other Python threads keep running. crnet.aio has
the same calls for asyncio.
"""

import json as _json
import os
from ctypes import byref, c_char_p, c_int, c_size_t, c_void_p, \
    create_string_buffer, string_at

from . import _lib
from ._lib import FOREVER as _FOREVER

__all__ = [
    "Client", "Response", "Transfer", "WebSocket", "Error", "Timeout",
    "version", "chromium_version", "default_user_agent",
    "platform_user_agent", "global_init",
]

# crnet_error
OK = 0
E_INVAL = -1
E_NOMEM = -2
E_NET = -3
E_TIMEOUT = -4
E_CANCELED = -5
E_SHUTDOWN = -6
E_TOO_LARGE = -7
E_WOULD_DEADLOCK = -8
E_STATE = -9
E_CALLBACK = -10
E_PENDING = -11

# crnet_mode
MODE_NAVIGATE = 0
MODE_FETCH = 1
MODE_RAW = 2

# crnet_priority
PRIORITY_DEFAULT = -1
PRIORITY_THROTTLED = 0
PRIORITY_IDLE = 1
PRIORITY_LOWEST = 2
PRIORITY_LOW = 3
PRIORITY_MEDIUM = 4
PRIORITY_HIGHEST = 5

# crnet_platform_profile
PLATFORM_LINUX = 0
PLATFORM_WINDOWS = 1
PLATFORM_MACOS = 2

# crnet_secure_dns
SECURE_DNS_OFF = 0
SECURE_DNS_AUTOMATIC = 1
SECURE_DNS_SECURE = 2

# crnet_http_cache
HTTP_CACHE_OFF = 0
HTTP_CACHE_MEMORY = 1
HTTP_CACHE_DISK = 2

# crnet_cache_mode
CACHE_DEFAULT = 0
CACHE_NO_STORE = 1
CACHE_RELOAD = 2
CACHE_NO_CACHE = 3
CACHE_FORCE_CACHE = 4
CACHE_ONLY_IF_CACHED = 5

# crnet_cookie_format
COOKIES_JSON = 0
COOKIES_NETSCAPE = 1

# crnet_ws_state
WS_CONNECTING = 0
WS_OPEN = 1
WS_CLOSING = 2
WS_CLOSED = 3

_WS_TEXT = 1
_WS_BINARY = 2

# How long a blocking call waits at a time, so that Ctrl-C (which
# Python handles between calls) interrupts it.
_SLICE_MS = 200

_CHUNK = 64 * 1024


class Error(Exception):
    """A crnet call failed.

    code is the crnet_error value (E_NET, E_TIMEOUT, ...), net_error
    Chromium's net error (e.g. -105 for ERR_NAME_NOT_RESOLVED; 0 if
    none), and response the Response received before the failure, if
    any.
    """

    def __init__(self, code, message=None, net_error=0, response=None):
        self.code = code
        self.net_error = net_error
        self.response = response
        if message is None:
            message = _lib.crnet_strerror(code).decode()
        super().__init__(message)


class Timeout(Error):
    """A request, read or wait took longer than allowed."""


def _raise(code, message=None, net_error=0, response=None):
    cls = Timeout if code == E_TIMEOUT else Error
    raise cls(code, message, net_error, response)


def _check(rc, what):
    if rc == E_INVAL:
        raise ValueError("%s: invalid argument" % what)
    if rc != OK:
        _raise(rc, "%s: %s" % (what, _lib.crnet_strerror(rc).decode()))


def _b(s):
    """str (UTF-8) or bytes for a const char * argument; None stays."""
    if s is None or isinstance(s, bytes):
        return s
    return str(s).encode()


def _s(b):
    return b.decode("utf-8", "replace") if b is not None else None


def _ms(seconds):
    """Seconds (None = no limit) to crnet's milliseconds (0 = none)."""
    if seconds is None:
        return 0
    if seconds < 0:
        raise ValueError("negative time")
    return max(1, int(seconds * 1000))


def _wait_ms(seconds):
    """Seconds (None = forever) for crnet's wait functions."""
    return _FOREVER if seconds is None else _ms(seconds)


def _wait(fn, handle, seconds, finished):
    """Call a crnet wait function in short slices so that Ctrl-C works.
    Returns its result, or E_TIMEOUT once |seconds| passed.

    The wait functions return the outcome they waited for, which can be
    E_TIMEOUT itself (a request that timed out): finished(handle) tells
    that apart from a wait that expired."""
    left = _wait_ms(seconds)
    while True:
        step = min(left, _SLICE_MS)
        rc = fn(handle, step)
        if rc != E_TIMEOUT or finished(handle):
            return rc
        if left != _FOREVER:
            left -= step
            if left <= 0:
                return E_TIMEOUT


def _transfer_done(t):
    return _lib.crnet_transfer_done(t) != 0


def _ws_closed(ws):
    return _lib.crnet_ws_get_state(ws) == WS_CLOSED


def version():
    """The loaded library's version, e.g. "0.25.0"."""
    return _lib.crnet_version().decode().split(" ", 1)[0]


def chromium_version():
    """The Chromium release the library is built from."""
    return _lib.crnet_chromium_version().decode()


def default_user_agent():
    return _lib.crnet_default_user_agent().decode()


def platform_user_agent(profile):
    """The User-Agent a platform profile (PLATFORM_*) sends."""
    ua = _lib.crnet_platform_user_agent(profile)
    if ua is None:
        raise ValueError("unknown platform profile %r" % (profile,))
    return ua.decode()


def global_init(enable_features=None, disable_features=None):
    """Choose Chromium feature overrides (comma-separated names, like
    Chrome's --enable-features). Only before the first Client."""
    _check(_lib.crnet_global_init(_b(enable_features),
                                  _b(disable_features)),
           "global_init")


def _headers_of(resp):
    out = []
    name, value = c_char_p(), c_char_p()
    for i in range(_lib.crnet_response_header_count(resp)):
        if _lib.crnet_response_header_at(resp, i, byref(name),
                                         byref(value)) == OK:
            # Header values are bytes on the wire; latin-1 keeps them.
            out.append((name.value.decode("latin-1"),
                        value.value.decode("latin-1")))
    return out


class Response:
    """A response; a plain Python object, independent of the library.

    status is 0 when no response arrived (see error and net_error).
    headers is the list of (name, value) pairs in received order.
    """

    def __init__(self, resp, with_body=True):
        self.status = _lib.crnet_response_status(resp)
        self.reason = _s(_lib.crnet_response_status_text(resp)) or ""
        self.url = _s(_lib.crnet_response_url(resp)) or ""
        self.headers = _headers_of(resp)
        self.protocol = _s(_lib.crnet_response_protocol(resp)) or ""
        self.remote_address = _s(
            _lib.crnet_response_remote_address(resp)) or ""
        self.redirect_count = _lib.crnet_response_redirect_count(resp)
        self.via_proxy = bool(_lib.crnet_response_via_proxy(resp))
        self.from_cache = bool(_lib.crnet_response_from_cache(resp))
        self.socket_reused = bool(_lib.crnet_response_socket_reused(resp))
        self.connection_id = _lib.crnet_response_connection_id(resp)
        self.decoded_codings = _s(
            _lib.crnet_response_decoded_codings(resp)) or ""
        self.net_error = _lib.crnet_response_net_error(resp)
        self.error = _s(_lib.crnet_response_error_string(resp)) or ""
        self.body = b""
        if with_body:
            n = c_size_t()
            p = _lib.crnet_response_body(resp, byref(n))
            if p and n.value:
                self.body = string_at(p, n.value)

    def header(self, name, default=None):
        """The first header called |name| (any case)."""
        name = name.lower()
        for k, v in self.headers:
            if k.lower() == name:
                return v
        return default

    def header_all(self, name):
        name = name.lower()
        return [v for k, v in self.headers if k.lower() == name]

    @property
    def text(self):
        return self.body.decode("utf-8", "replace")

    def json(self):
        return _json.loads(self.body)

    @property
    def ok(self):
        """True for a status below 400."""
        return 0 < self.status < 400

    def __repr__(self):
        return "<crnet.Response %d %s>" % (self.status, self.url)


def _items(headers):
    if headers is None:
        return []
    if hasattr(headers, "items"):
        return list(headers.items())
    return list(headers)


def _streamed(body):
    return body is not None and not isinstance(
        body, (bytes, bytearray, memoryview, str, os.PathLike))


class _Request:
    """An owned crnet_request built from Python arguments."""

    def __init__(self, method, url, headers=None, body=None, mode=None,
                 priority=None, timeout=None, connect_timeout=None,
                 max_redirects=None, decompress=None, max_body_size=None,
                 cache_mode=None, body_length=None, stream_response=False,
                 ws_compression=None, proxy=None):
        self.ptr = _lib.crnet_request_new(_b(method), _b(url))
        if not self.ptr:
            raise ValueError("invalid request: %s %s" % (method, url))
        try:
            self._apply(headers, body, mode, priority, timeout,
                        connect_timeout, max_redirects, decompress,
                        max_body_size, cache_mode, body_length,
                        stream_response, ws_compression, proxy)
        except BaseException:
            self.free()
            raise

    def _apply(self, headers, body, mode, priority, timeout,
               connect_timeout, max_redirects, decompress, max_body_size,
               cache_mode, body_length, stream_response, ws_compression,
               proxy):
        p = self.ptr
        for name, value in _items(headers):
            # None removes a header crnet would add.
            _check(_lib.crnet_request_set_header(p, _b(name), _b(value)),
                   "header %r" % (name,))
        if isinstance(body, os.PathLike):
            _check(_lib.crnet_request_set_body_file(
                p, os.fsencode(body), 0, _FOREVER), "body file")
        elif _streamed(body):
            length = -1 if body_length is None else int(body_length)
            _check(_lib.crnet_request_set_body_stream(p, length),
                   "body stream")
        elif body is not None:
            data = body.encode() if isinstance(body, str) else bytes(body)
            _check(_lib.crnet_request_set_body(p, data, len(data)), "body")
        if mode is not None:
            _check(_lib.crnet_request_set_mode(p, mode), "mode")
        if priority is not None:
            _check(_lib.crnet_request_set_priority(p, priority),
                   "priority")
        if timeout is not None:
            _check(_lib.crnet_request_set_timeout(p, _ms(timeout)),
                   "timeout")
        if connect_timeout is not None:
            _check(_lib.crnet_request_set_connect_timeout(
                p, _ms(connect_timeout)), "connect_timeout")
        if max_redirects is not None:
            _check(_lib.crnet_request_set_max_redirects(p, max_redirects),
                   "max_redirects")
        if decompress is not None:
            _check(_lib.crnet_request_set_decompress(p, int(decompress)),
                   "decompress")
        if max_body_size is not None:
            _check(_lib.crnet_request_set_max_body_size(p, max_body_size),
                   "max_body_size")
        if cache_mode is not None:
            _check(_lib.crnet_request_set_cache_mode(p, cache_mode),
                   "cache_mode")
        if stream_response:
            _check(_lib.crnet_request_set_response_stream(p, 1),
                   "stream_response")
        if ws_compression is not None:
            _check(_lib.crnet_request_set_ws_compression(
                p, int(ws_compression)), "ws_compression")
        if proxy is not None:
            _check(_lib.crnet_request_set_proxy(p, _b(proxy)), "proxy")

    def free(self):
        if self.ptr:
            _lib.crnet_request_free(self.ptr)
            self.ptr = None


def _chunks(body):
    """Bytes chunks of a streamed body: a file object or an iterable."""
    if hasattr(body, "read"):
        while True:
            chunk = body.read(_CHUNK)
            if not chunk:
                return
            yield chunk.encode() if isinstance(chunk, str) else chunk
    else:
        for chunk in body:
            yield chunk.encode() if isinstance(chunk, str) else bytes(chunk)


class Transfer:
    """A running request whose body is read as it arrives
    (Client.stream()). Use it as a context manager, or call close().
    """

    _ptr = None

    def __init__(self, client, req, body):
        self._client = client
        self._ptr = _lib.crnet_start(client._handle(), req.ptr, None, None)
        if not self._ptr:
            raise Error(E_INVAL, "crnet_start failed")
        self.response = None
        try:
            if _streamed(body):
                self._upload(body)
        except BaseException:
            self.close()
            raise

    def _upload(self, body):
        n = c_size_t()
        for chunk in _chunks(body):
            off = 0
            while off < len(chunk):
                part = chunk[off:]
                rc = _lib.crnet_transfer_write(self._ptr, part, len(part),
                                               byref(n), _SLICE_MS)
                off += n.value
                if rc == E_TIMEOUT:
                    continue
                if rc == E_INVAL:
                    # Past the declared length; the transfer would
                    # wait for an end that cannot come.
                    _lib.crnet_transfer_cancel(self._ptr)
                    raise ValueError("body longer than body_length")
                if rc != OK:
                    self._fail(rc)
        rc = _lib.crnet_transfer_write_end(self._ptr)
        if rc != OK:
            self._fail(rc)

    def _fail(self, rc):
        """Raise the transfer's own error (with any response) for rc."""
        _wait(_lib.crnet_transfer_wait, self._ptr, None, _transfer_done)
        result = _lib.crnet_transfer_result(self._ptr)
        resp = _lib.crnet_transfer_response(self._ptr)
        r = Response(resp, with_body=False) if resp else None
        net = r.net_error if r else 0
        msg = (r.error if r and r.error else None)
        _raise(result if result not in (OK, E_PENDING) else rc, msg, net, r)

    def wait_response(self, timeout=None):
        """Wait for the response headers; returns the Response (without
        its body) and sets self.response."""
        if self.response is None:
            rc = _wait(_lib.crnet_transfer_wait_response, self._ptr,
                       timeout, _transfer_done)
            if rc != OK:
                if rc == E_TIMEOUT:
                    _raise(rc)
                self._fail(rc)
            self.response = Response(
                _lib.crnet_transfer_response(self._ptr), with_body=False)
        return self.response

    def read(self, size=_CHUNK, timeout=None):
        """Up to |size| body bytes; b"" at the end of the body."""
        buf = create_string_buffer(size)
        n = c_size_t()
        left = _wait_ms(timeout)
        while True:
            step = min(left, _SLICE_MS)
            rc = _lib.crnet_transfer_read(self._ptr, buf, size, byref(n),
                                          step)
            if rc == OK:
                return buf.raw[:n.value]
            # E_TIMEOUT is also how a timed-out transfer ends.
            if rc != E_TIMEOUT or _transfer_done(self._ptr):
                self._fail(rc)
            if left != _FOREVER:
                left -= step
                if left <= 0:
                    _raise(E_TIMEOUT)

    def iter_bytes(self, chunk_size=_CHUNK, timeout=None):
        """The body in chunks as they arrive; |timeout| applies to each
        read."""
        self.wait_response()
        while True:
            chunk = self.read(chunk_size, timeout)
            if not chunk:
                return
            yield chunk

    def __iter__(self):
        return self.iter_bytes()

    def cancel(self):
        if self._ptr:
            _lib.crnet_transfer_cancel(self._ptr)

    def close(self):
        """Cancel the transfer if it is still running, and release it."""
        if self._ptr:
            _lib.crnet_transfer_cancel(self._ptr)
            _lib.crnet_transfer_free(self._ptr)
            self._ptr = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        self.close()


class WebSocket:
    """A WebSocket connection (Client.websocket()). Messages are queued
    until recv() takes them. Use it as a context manager, or call
    close() and then free()."""

    _ptr = None

    def __init__(self, client, req, protocols, timeout):
        self._client = client
        self._ptr = _lib.crnet_ws_connect(client._handle(), req.ptr,
                                          _b(protocols), None, None)
        if not self._ptr:
            raise ValueError("invalid WebSocket request")
        rc = _wait(_lib.crnet_ws_wait_open, self._ptr, timeout,
                   _ws_closed)
        if rc != OK:
            hs = _lib.crnet_ws_handshake_response(self._ptr)
            r = Response(hs, with_body=False) if hs else None
            net = _lib.crnet_ws_net_error(self._ptr)
            self.free()
            _raise(rc, "WebSocket handshake failed: " +
                   (r.error if r and r.error else
                    "HTTP %d" % r.status if r and r.status else
                    _lib.crnet_strerror(rc).decode()), net, r)

    def send(self, message):
        """Queue a message: str as text, bytes as binary."""
        if isinstance(message, str):
            kind, data = _WS_TEXT, message.encode()
        else:
            kind, data = _WS_BINARY, bytes(message)
        _check(_lib.crnet_ws_send(self._ptr, kind, data, len(data)),
               "send")

    def recv(self, timeout=None):
        """The next message (str for text, bytes for binary), or None
        once the connection is closed and nothing is queued. Raises
        Timeout when nothing arrived in time."""
        kind, data, n = c_int(), c_void_p(), c_size_t()
        left = _wait_ms(timeout)
        while True:
            step = min(left, _SLICE_MS)
            rc = _lib.crnet_ws_recv(self._ptr, byref(kind), byref(data),
                                    byref(n), step)
            if rc == OK:
                try:
                    payload = string_at(data, n.value) if n.value else b""
                finally:
                    _lib.crnet_free(data)
                return payload.decode() if kind.value == _WS_TEXT \
                    else payload
            if rc == E_STATE:
                return None
            if rc != E_TIMEOUT:
                _raise(rc)
            if left != _FOREVER:
                left -= step
                if left <= 0:
                    _raise(E_TIMEOUT)

    def __iter__(self):
        while True:
            msg = self.recv()
            if msg is None:
                return
            yield msg

    def ping(self, data=b""):
        data = _b(data)
        _check(_lib.crnet_ws_ping(self._ptr, data, len(data)), "ping")

    def set_auto_ping(self, interval):
        """Ping every |interval| seconds while idle; None stops it."""
        _lib.crnet_ws_set_auto_ping(self._ptr, _ms(interval))

    def set_idle_timeout(self, timeout):
        """Close after |timeout| seconds without a message; None = never."""
        _lib.crnet_ws_set_idle_timeout(self._ptr, _ms(timeout))

    def close(self, code=1000, reason=""):
        """Start the closing handshake (see wait_closed())."""
        if self._ptr and self.state in (WS_CONNECTING, WS_OPEN):
            _check(_lib.crnet_ws_close(self._ptr, code, _b(reason)),
                   "close")

    def wait_closed(self, timeout=None):
        """Wait until the connection is closed."""
        rc = _wait(_lib.crnet_ws_wait_closed, self._ptr, timeout,
                   _ws_closed)
        if rc == E_TIMEOUT and self.state != WS_CLOSED:
            _raise(rc)

    def fileno(self):
        """Readable while recv() would not block, for select()/poll()."""
        return _lib.crnet_ws_fd(self._ptr)

    @property
    def state(self):
        return _lib.crnet_ws_get_state(self._ptr)

    @property
    def protocol(self):
        return _s(_lib.crnet_ws_protocol(self._ptr)) or ""

    @property
    def extensions(self):
        return _s(_lib.crnet_ws_extensions(self._ptr)) or ""

    @property
    def close_code(self):
        return _lib.crnet_ws_close_code(self._ptr)

    @property
    def close_reason(self):
        return _s(_lib.crnet_ws_close_reason(self._ptr)) or ""

    @property
    def buffered_amount(self):
        return _lib.crnet_ws_buffered_amount(self._ptr)

    def free(self):
        """Release the handle; an open connection is aborted."""
        if self._ptr:
            _lib.crnet_ws_free(self._ptr)
            self._ptr = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        try:
            if self._ptr and self.state != WS_CLOSED:
                self.close()
                self.wait_closed(5)
        except Error:
            pass
        finally:
            self.free()

    def __del__(self):
        self.free()


_STRING_OPTIONS = ("proxy", "proxy_bypass", "user_agent", "sec_ch_ua",
                   "platform", "accept_language", "host_rules", "netlog",
                   "state_dir", "local_address", "bind_interface")
_BOOL_OPTIONS = ("http2", "quic", "builtin_dns", "cookies", "insecure",
                 "trust_anchors_only")


class Client:
    """A crnet client: its own network thread, connection pools, cookie
    jar and configuration. Thread-safe; share one between threads.

    Options (all optional), as in crnet_client_config_set_*():
      proxy, proxy_bypass, user_agent, sec_ch_ua, platform,
      accept_language, host_rules, netlog, state_dir, local_address,
      bind_interface (str)
      http2, quic, builtin_dns, cookies, insecure, trust_anchors_only
      platform_profile (PLATFORM_*)
      secure_dns (SECURE_DNS_*), with doh_templates (str)
      trust_anchors: PEM text or a list of PEM texts
      trust_anchors_file: a path or a list of paths
      client_cert: (cert_pem, key_pem); client_cert_file: (cert, key)
      http_cache (HTTP_CACHE_*), http_cache_path, http_cache_max_size
      timeout, connect_timeout (seconds)
    """

    _ptr = None

    def __init__(self, **options):
        cfg = _lib.crnet_client_config_new()
        if not cfg:
            raise MemoryError("crnet_client_config_new")
        try:
            self._configure(cfg, dict(options))
            self._ptr = _lib.crnet_client_new(cfg)
        finally:
            _lib.crnet_client_config_free(cfg)
        if not self._ptr:
            raise Error(E_INVAL, "crnet_client_new failed (see stderr)")

    @staticmethod
    def _configure(cfg, o):
        for name in _STRING_OPTIONS:
            if name in o:
                fn = getattr(_lib, "crnet_client_config_set_" + name)
                _check(fn(cfg, _b(o.pop(name))), name)
        for name in _BOOL_OPTIONS:
            if name in o:
                getattr(_lib, "crnet_client_config_set_" + name)(
                    cfg, int(bool(o.pop(name))))
        if "platform_profile" in o:
            _check(_lib.crnet_client_config_set_platform_profile(
                cfg, o.pop("platform_profile")), "platform_profile")
        if "secure_dns" in o:
            _check(_lib.crnet_client_config_set_secure_dns(
                cfg, o.pop("secure_dns"), _b(o.pop("doh_templates", None))),
                "secure_dns")
        anchors = o.pop("trust_anchors", None)
        for pem in [anchors] if isinstance(anchors, (str, bytes)) else \
                anchors or []:
            _check(_lib.crnet_client_config_add_trust_anchors(cfg, _b(pem)),
                   "trust_anchors")
        files = o.pop("trust_anchors_file", None)
        for path in [files] if isinstance(files, (str, bytes,
                                                  os.PathLike)) else \
                files or []:
            _check(_lib.crnet_client_config_add_trust_anchors_file(
                cfg, os.fsencode(path)), "trust_anchors_file")
        if "client_cert" in o:
            cert, key = o.pop("client_cert")
            _check(_lib.crnet_client_config_set_client_cert(
                cfg, _b(cert), _b(key)), "client_cert")
        if "client_cert_file" in o:
            cert, key = o.pop("client_cert_file")
            _check(_lib.crnet_client_config_set_client_cert_file(
                cfg, os.fsencode(cert), os.fsencode(key)),
                "client_cert_file")
        if "http_cache" in o:
            path = o.pop("http_cache_path", None)
            _check(_lib.crnet_client_config_set_http_cache(
                cfg, o.pop("http_cache"),
                os.fsencode(path) if path is not None else None,
                o.pop("http_cache_max_size", 0)), "http_cache")
        if "timeout" in o:
            _lib.crnet_client_config_set_timeout(cfg, _ms(o.pop("timeout")))
        if "connect_timeout" in o:
            _lib.crnet_client_config_set_connect_timeout(
                cfg, _ms(o.pop("connect_timeout")))
        if o:
            raise TypeError("unknown option(s): " + ", ".join(sorted(o)))

    def _handle(self):
        if not self._ptr:
            raise Error(E_STATE, "the client is closed")
        return self._ptr

    def request(self, method, url, headers=None, body=None, **kw):
        """Send a request and return the whole Response.

        headers: dict or list of (name, value); a None value removes a
        header crnet would add. body: bytes, str (UTF-8), a pathlib.Path
        (the file is sent), or a file object or iterable of bytes
        (streamed; pass body_length if known). Other keywords: mode
        (MODE_*), priority, timeout, connect_timeout, max_redirects,
        decompress, max_body_size, cache_mode, proxy (this request's
        proxy URL, "direct://" for none).

        Raises Error (Timeout for timeouts) when the request fails; an
        HTTP error status is a Response, not an exception.
        """
        req = _Request(method, url, headers, body, **kw)
        try:
            t = Transfer(self, req, body)
        finally:
            req.free()
        try:
            rc = _wait(_lib.crnet_transfer_wait, t._ptr, None,
                       _transfer_done)
            resp = _lib.crnet_transfer_take_response(t._ptr)
            r = None
            if resp:
                try:
                    r = Response(resp)
                finally:
                    _lib.crnet_response_free(resp)
            if rc != OK:
                _raise(rc, r.error if r and r.error else None,
                       r.net_error if r else 0, r)
            return r
        finally:
            t.close()

    def get(self, url, **kw):
        return self.request("GET", url, **kw)

    def head(self, url, **kw):
        return self.request("HEAD", url, **kw)

    def post(self, url, body=None, **kw):
        return self.request("POST", url, body=body, **kw)

    def put(self, url, body=None, **kw):
        return self.request("PUT", url, body=body, **kw)

    def delete(self, url, **kw):
        return self.request("DELETE", url, **kw)

    def stream(self, method, url, headers=None, body=None, **kw):
        """Start a request whose body is read as it arrives, in constant
        memory. Returns a Transfer:

            with client.stream("GET", url) as t:
                print(t.wait_response().status)
                for chunk in t:
                    ...
        """
        req = _Request(method, url, headers, body, stream_response=True,
                       **kw)
        try:
            return Transfer(self, req, body)
        finally:
            req.free()

    def websocket(self, url, protocols=None, headers=None, timeout=None,
                  compression=None, proxy=None):
        """Open a WebSocket (ws:// or wss://) and wait until it is open.
        protocols: comma-separated subprotocols to offer."""
        req = _Request(None, url, headers, ws_compression=compression,
                       proxy=proxy)
        try:
            return WebSocket(self, req, protocols, timeout)
        finally:
            req.free()

    def set_cookie(self, url, set_cookie):
        """Store a cookie as if |url| had sent this Set-Cookie value."""
        _check(_lib.crnet_client_set_cookie(self._handle(), _b(url),
                                            _b(set_cookie)), "set_cookie")

    def _owned_string(self, p):
        if not p:
            return None
        try:
            return string_at(p).decode("utf-8", "replace")
        finally:
            _lib.crnet_free(p)

    def get_cookies(self, url):
        """The Cookie header value the client would send to |url|."""
        return self._owned_string(
            _lib.crnet_client_get_cookies(self._handle(), _b(url))) or ""

    def clear_cookies(self):
        _check(_lib.crnet_client_clear_cookies(self._handle()),
               "clear_cookies")

    def export_cookies(self, format=COOKIES_JSON):
        s = self._owned_string(
            _lib.crnet_client_export_cookies(self._handle(), format))
        if s is None:
            raise Error(E_STATE, "export_cookies failed")
        return s

    def import_cookies(self, data, format=COOKIES_JSON):
        """Add cookies from an export; returns how many were stored."""
        n = c_size_t()
        _check(_lib.crnet_client_import_cookies(self._handle(), _b(data),
                                                format, byref(n)),
               "import_cookies")
        return n.value

    def close_idle(self):
        """Close the client's idle connections."""
        _lib.crnet_client_close_idle(self._handle())

    def close(self):
        """Release the client. Running transfers fail with E_SHUTDOWN."""
        if self._ptr:
            _lib.crnet_client_free(self._ptr)
            self._ptr = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        self.close()
