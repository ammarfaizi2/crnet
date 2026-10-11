# SPDX-License-Identifier: BSD-3-Clause
"""asyncio support: awaitable requests, streamed transfers and WebSockets.

    import asyncio
    import crnet.aio

    async def main():
        async with crnet.aio.AsyncClient() as client:
            r = await client.get("https://example.com/")
            print(r.status, len(r.body))

    asyncio.run(main())

Every wait is a file descriptor of the library's (crnet_transfer_done_fd(),
crnet_ws_fd(), ...) watched with loop.add_reader(): no thread waits on
the event loop's behalf and no Python code runs on the library's network
thread. It therefore needs an event loop with add_reader() (the default
one on Linux, or uvloop). Cancelling a task cancels its transfer.
"""

import asyncio
from ctypes import byref, c_int, c_size_t, c_void_p, create_string_buffer, \
    string_at

from . import (E_INVAL, E_NOMEM, E_PENDING, E_STATE, E_TIMEOUT, OK,
               WS_CLOSED, WS_CONNECTING, WS_OPEN, Client, Error, Response,
               _CHUNK, _WS_BINARY, _WS_TEXT, _b, _check, _chunks, _lib,
               _ms, _raise, _Request, _s, _streamed)

__all__ = ["AsyncClient", "AsyncTransfer", "AsyncWebSocket"]


async def _readable(fd, timeout=None):
    """Wait until |fd| is readable; False if |timeout| seconds passed
    first. Only one task may wait on a descriptor at a time."""
    if fd < 0:
        raise Error(E_NOMEM, "no file descriptor for the wait")
    loop = asyncio.get_running_loop()
    fut = loop.create_future()

    def ready():
        if not fut.done():
            fut.set_result(True)

    loop.add_reader(fd, ready)
    try:
        if timeout is None:
            await fut
            return True
        done, _ = await asyncio.wait({fut}, timeout=timeout)
        return bool(done)
    finally:
        loop.remove_reader(fd)


class _Waits:
    """Serializes the waits of one handle: a descriptor can only have
    one reader in an event loop, so concurrent waiters take turns."""

    def __init__(self):
        self._lock = None

    async def readable(self, fd, timeout=None):
        if self._lock is None:
            self._lock = asyncio.Lock()
        async with self._lock:
            return await _readable(fd, timeout)


async def _achunks(body):
    """Bytes chunks of an async iterable, a file object or an iterable."""
    if hasattr(body, "__aiter__"):
        async for chunk in body:
            yield chunk.encode() if isinstance(chunk, str) else bytes(chunk)
    else:
        for chunk in _chunks(body):
            yield chunk


class AsyncTransfer:
    """A running request whose body is read as it arrives
    (AsyncClient.stream()):

        async with client.stream("GET", url) as t:
            print((await t.wait_response()).status)
            async for chunk in t:
                ...
    """

    _ptr = None

    def __init__(self, client, method, url, headers, body, kw):
        self._client = client
        self._args = (method, url, headers, body, kw)
        self.response = None
        self._waits = _Waits()

    async def _start(self, stream_response):
        method, url, headers, body, kw = self._args
        req = _Request(method, url, headers, body,
                       stream_response=stream_response, **kw)
        try:
            self._ptr = _lib.crnet_start(self._client._handle(), req.ptr,
                                         None, None)
        finally:
            req.free()
        if not self._ptr:
            raise Error(E_INVAL, "crnet_start failed")
        try:
            if _streamed(body):
                await self._upload(body)
        except BaseException:
            self.close()
            raise
        return self

    async def _upload(self, body):
        n = c_size_t()
        async for chunk in _achunks(body):
            off = 0
            while off < len(chunk):
                part = chunk[off:]
                rc = _lib.crnet_transfer_write(self._ptr, part, len(part),
                                               byref(n), 0)
                off += n.value
                if rc == E_TIMEOUT:     # the send buffer is full
                    await self._waits.readable(
                        _lib.crnet_transfer_write_fd(self._ptr))
                    continue
                if rc == E_INVAL:
                    _lib.crnet_transfer_cancel(self._ptr)
                    raise ValueError("body longer than body_length")
                if rc != OK:
                    await self._fail(rc)
        rc = _lib.crnet_transfer_write_end(self._ptr)
        if rc != OK:
            await self._fail(rc)

    async def _done(self):
        await self._waits.readable(_lib.crnet_transfer_done_fd(self._ptr))

    async def _fail(self, rc):
        """Raise the transfer's own error (with any response) for rc."""
        await self._done()
        result = _lib.crnet_transfer_result(self._ptr)
        resp = _lib.crnet_transfer_response(self._ptr)
        r = Response(resp, with_body=False) if resp else None
        _raise(result if result not in (OK, E_PENDING) else rc,
               r.error if r and r.error else None,
               r.net_error if r else 0, r)

    async def _finish(self):
        """Wait for the end of a buffered transfer; its Response."""
        await self._done()
        rc = _lib.crnet_transfer_wait(self._ptr, 0)
        resp = _lib.crnet_transfer_take_response(self._ptr)
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

    async def wait_response(self, timeout=None):
        """Wait for the response headers; returns the Response (without
        its body) and sets self.response."""
        if self.response is None:
            if not await self._waits.readable(
                    _lib.crnet_transfer_response_fd(self._ptr), timeout):
                _raise(E_TIMEOUT)
            rc = _lib.crnet_transfer_wait_response(self._ptr, 0)
            if rc != OK:
                await self._fail(rc)
            self.response = Response(
                _lib.crnet_transfer_response(self._ptr), with_body=False)
        return self.response

    async def read(self, size=_CHUNK, timeout=None):
        """Up to |size| body bytes; b"" at the end of the body."""
        buf = create_string_buffer(size)
        n = c_size_t()
        while True:
            rc = _lib.crnet_transfer_read(self._ptr, buf, size, byref(n), 0)
            if rc == OK:
                return buf.raw[:n.value]
            # E_TIMEOUT is also how a timed-out transfer ends.
            if rc != E_TIMEOUT or _lib.crnet_transfer_done(self._ptr):
                await self._fail(rc)
            if not await self._waits.readable(
                    _lib.crnet_transfer_fd(self._ptr), timeout):
                _raise(E_TIMEOUT)

    async def iter_bytes(self, chunk_size=_CHUNK, timeout=None):
        """The body in chunks as they arrive; |timeout| applies to each
        read."""
        await self.wait_response()
        while True:
            chunk = await self.read(chunk_size, timeout)
            if not chunk:
                return
            yield chunk

    def __aiter__(self):
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

    async def __aenter__(self):
        return await self._start(True)

    async def __aexit__(self, *exc):
        self.close()

    def __del__(self):
        self.close()


class AsyncWebSocket:
    """A WebSocket connection (await AsyncClient.websocket()). Messages
    are queued until recv() takes them. Use it with "async with", or call
    close(), wait_closed() and free()."""

    _ptr = None

    def __init__(self, ptr):
        self._ptr = ptr
        self._waits = _Waits()
        self._closed_waits = _Waits()

    @classmethod
    async def _connect(cls, client, req, protocols, timeout):
        ptr = _lib.crnet_ws_connect(client._handle(), req.ptr, _b(protocols),
                                    None, None)
        if not ptr:
            raise ValueError("invalid WebSocket request")
        ws = cls(ptr)
        try:
            opened = await ws._waits.readable(_lib.crnet_ws_open_fd(ptr),
                                              timeout)
            rc = _lib.crnet_ws_wait_open(ptr, 0) if opened else E_TIMEOUT
        except BaseException:
            ws.free()
            raise
        if rc != OK:
            hs = _lib.crnet_ws_handshake_response(ptr)
            r = Response(hs, with_body=False) if hs else None
            net = _lib.crnet_ws_net_error(ptr)
            ws.free()
            _raise(rc, "WebSocket handshake failed: " +
                   (r.error if r and r.error else
                    "HTTP %d" % r.status if r and r.status else
                    _lib.crnet_strerror(rc).decode()), net, r)
        return ws

    def send(self, message):
        """Queue a message (str as text, bytes as binary); it does not
        wait. buffered_amount tells how much is not sent yet."""
        if isinstance(message, str):
            kind, data = _WS_TEXT, message.encode()
        else:
            kind, data = _WS_BINARY, bytes(message)
        _check(_lib.crnet_ws_send(self._ptr, kind, data, len(data)), "send")

    async def recv(self, timeout=None):
        """The next message (str for text, bytes for binary), or None
        once the connection is closed and nothing is queued. Raises
        Timeout when nothing arrived in time."""
        kind, data, n = c_int(), c_void_p(), c_size_t()
        while True:
            rc = _lib.crnet_ws_recv(self._ptr, byref(kind), byref(data),
                                    byref(n), 0)
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
            if not await self._waits.readable(_lib.crnet_ws_fd(self._ptr),
                                              timeout):
                _raise(E_TIMEOUT)

    def __aiter__(self):
        return self

    async def __anext__(self):
        msg = await self.recv()
        if msg is None:
            raise StopAsyncIteration
        return msg

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

    async def wait_closed(self, timeout=None):
        """Wait until the connection is closed."""
        if not await self._closed_waits.readable(
                _lib.crnet_ws_closed_fd(self._ptr), timeout):
            _raise(E_TIMEOUT)

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

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        try:
            if self._ptr and self.state != WS_CLOSED:
                self.close()
                await self.wait_closed(5)
        except Error:
            pass
        finally:
            self.free()

    def __del__(self):
        self.free()


class AsyncClient:
    """A crnet client for asyncio, with the options of crnet.Client.
    One client may serve several event loops and threads at once."""

    def __init__(self, **options):
        self._client = Client(**options)

    def _handle(self):
        return self._client._handle()

    async def request(self, method, url, headers=None, body=None, **kw):
        """Send a request and return the whole Response; the arguments
        are those of crnet.Client.request(), and a streamed body may also
        be an async iterable of bytes."""
        t = AsyncTransfer(self, method, url, headers, body, kw)
        await t._start(False)
        try:
            return await t._finish()
        finally:
            t.close()

    async def get(self, url, **kw):
        return await self.request("GET", url, **kw)

    async def head(self, url, **kw):
        return await self.request("HEAD", url, **kw)

    async def post(self, url, body=None, **kw):
        return await self.request("POST", url, body=body, **kw)

    async def put(self, url, body=None, **kw):
        return await self.request("PUT", url, body=body, **kw)

    async def delete(self, url, **kw):
        return await self.request("DELETE", url, **kw)

    def stream(self, method, url, headers=None, body=None, **kw):
        """A request whose body is read as it arrives; it starts when
        the "async with" block is entered:

            async with client.stream("GET", url) as t:
                async for chunk in t:
                    ...
        """
        return AsyncTransfer(self, method, url, headers, body, kw)

    async def websocket(self, url, protocols=None, headers=None,
                        timeout=None, compression=None):
        """Open a WebSocket (ws:// or wss://) and wait until it is open."""
        req = _Request(None, url, headers, ws_compression=compression)
        try:
            return await AsyncWebSocket._connect(self, req, protocols,
                                                 timeout)
        finally:
            req.free()

    # Cookie calls return at once; they need no awaiting.
    def set_cookie(self, url, set_cookie):
        self._client.set_cookie(url, set_cookie)

    def get_cookies(self, url):
        return self._client.get_cookies(url)

    def clear_cookies(self):
        self._client.clear_cookies()

    def export_cookies(self, *args):
        return self._client.export_cookies(*args)

    def import_cookies(self, *args):
        return self._client.import_cookies(*args)

    def close_idle(self):
        self._client.close_idle()

    def close(self):
        """Release the client. Running transfers fail with E_SHUTDOWN."""
        self._client.close()

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        self.close()
