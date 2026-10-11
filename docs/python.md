# Python bindings

`bindings/python/crnet` is a Python 3 module over the C API. It calls
`libcrnet.so` through `ctypes`, so nothing is compiled when it is
installed, and it needs no C++ on its side: only the library, built as
described in [building.md](building.md).

```python
import crnet

with crnet.Client(proxy="socks5h://127.0.0.1:1080",
                  platform_profile=crnet.PLATFORM_WINDOWS,
                  timeout=30) as client:
    r = client.get("https://example.com/")
    print(r.status, r.protocol, r.header("content-type"))
    print(r.text)
```

## Loading the library

The module loads `libcrnet.so.0` through the system's dynamic loader
(`LD_LIBRARY_PATH`, the `ldconfig` paths), or the file named by the
`CRNET_LIBRARY` environment variable:

```bash
CRNET_LIBRARY=build/lib/libcrnet.so.0 PYTHONPATH=bindings/python python3 app.py
```

Importing fails with `ImportError` when the library is missing or older
than the module needs. There is no package on PyPI yet: put
`bindings/python` on `PYTHONPATH`, or copy the `crnet` directory next
to your program.

## Clients

`crnet.Client(**options)` takes the settings of
`crnet_client_config_set_*()` as keyword arguments, with times in
seconds:

| Option | Value |
|---|---|
| `proxy`, `proxy_bypass`, `host_rules`, `local_address` | strings, as in the C API |
| `user_agent`, `sec_ch_ua`, `platform`, `accept_language`, `platform_profile` | the browser identity; `platform_profile` is `PLATFORM_LINUX`, `PLATFORM_WINDOWS` or `PLATFORM_MACOS` |
| `http2`, `quic`, `builtin_dns`, `cookies`, `insecure` | booleans |
| `secure_dns`, `doh_templates` | `SECURE_DNS_*` and the DoH URI templates |
| `trust_anchors`, `trust_anchors_file`, `trust_anchors_only` | PEM text or paths (one or a list), and whether only they are trusted |
| `client_cert`, `client_cert_file` | `(cert, key)` as PEM text or as paths |
| `http_cache`, `http_cache_path`, `http_cache_max_size` | `HTTP_CACHE_*`, its directory and size |
| `state_dir`, `netlog` | paths |
| `timeout`, `connect_timeout` | seconds |

An unknown option raises `TypeError` and an invalid value `ValueError`.
A client is thread-safe: share one between threads. `close()` (or the
`with` block) releases it.

## Requests

`client.request(method, url, headers=None, body=None, **kw)` sends a
request and returns a `crnet.Response`; `get()`, `head()`, `post()`,
`put()` and `delete()` are shortcuts.

- `headers` is a dict or a list of `(name, value)` pairs. `None` as a
  value removes a header crnet would add (for example `User-Agent`).
- `body` is `bytes`, a `str` (sent as UTF-8), a `pathlib.Path` (the
  file is sent), or a file object or an iterable of `bytes`, which is
  streamed (`body_length=` gives its size; without it HTTP/1.1 uses
  chunked encoding).
- Other keywords: `mode` (`MODE_NAVIGATE`, `MODE_FETCH`, `MODE_RAW`),
  `priority`, `timeout`, `connect_timeout`, `max_redirects`,
  `decompress`, `max_body_size`, `cache_mode`.

A `Response` is a plain Python object: `status`, `reason`, `url`,
`headers` (pairs in received order), `header(name)`,
`header_all(name)`, `body` (`bytes`), `text`, `json()`, `ok`,
`protocol`, `remote_address`, `redirect_count`, `from_cache`,
`via_proxy`, `socket_reused`, `connection_id`, `decoded_codings`.

An HTTP error status is a normal response. A request that fails raises
`crnet.Error`, with `code` (`E_NET`, `E_TIMEOUT`, ...), `net_error`
(Chromium's net error, e.g. -105 for a name that does not resolve) and
`response` (what arrived before the failure, if anything). Timeouts
raise its subclass `crnet.Timeout`.

```python
try:
    r = client.get("https://example.com/", timeout=10)
except crnet.Timeout:
    ...
except crnet.Error as e:
    print("failed:", e, e.net_error)
```

## Streaming

`client.stream()` starts a request whose body is read as it arrives, in
constant memory:

```python
with client.stream("GET", "https://example.com/big.iso") as t:
    print(t.wait_response().status)
    with open("big.iso", "wb") as f:
        for chunk in t:            # or t.read(size, timeout)
            f.write(chunk)
```

Leaving the `with` block cancels a transfer that is still running.

## WebSockets

```python
with client.websocket("wss://example.com/chat", protocols="chat") as ws:
    ws.send("hello")               # str: text, bytes: binary
    print(ws.recv(timeout=5))
    for message in ws:             # until the connection closes
        ...
```

`recv()` returns `None` once the connection is closed and no message is
left, and raises `crnet.Timeout` when nothing arrived in time.
`fileno()` is readable while `recv()` would not block, for `select()`
and event loops. There are also `ping()`, `set_auto_ping()`,
`set_idle_timeout()`, `close(code, reason)`, `wait_closed()`, `state`,
`protocol`, `extensions`, `close_code`, `close_reason` and
`buffered_amount`. A refused handshake raises `crnet.Error` with the
handshake response in `response`.

## Cookies

`set_cookie(url, set_cookie)`, `get_cookies(url)`, `clear_cookies()`,
`export_cookies(format)` and `import_cookies(data, format)`, with
`COOKIES_JSON` (default) or `COOKIES_NETSCAPE`.

## Threads and signals

`ctypes` releases the GIL during every library call, so blocking calls
stop only the calling thread. Waits run in short slices, so Ctrl-C
interrupts a request; the transfer is then cancelled.

The module does not pass Python callbacks to the library: Chromium's
network thread would run them, holding up every request of the client
while it waits for the GIL. Use blocking calls in threads, `stream()`,
or the WebSocket's `fileno()` instead.

## Tests

`bindings/python/tests` runs against the local test server; `ctest`
runs it as the `python_bindings` test (not in sanitizer builds, whose
runtime cannot be loaded into `python3`). By hand:

```bash
CRNET_LIBRARY=build/lib/libcrnet.so.0 PYTHONPATH=bindings/python \
	python3 -m unittest discover -s bindings/python/tests
```
