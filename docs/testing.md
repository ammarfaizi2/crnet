# Testing

## Running

```bash
cmake --build build
ctest --test-dir build -j "$(nproc)"
```

Every test case runs in its own process (`gtest_discover_tests`) with a
private instance of the local test server, so the suite parallelises
well; on a large machine it finishes in about a second. A single test:

```bash
build/tests/crnet_tests --gtest_filter='ClientTest.NavigateHttp2MatchesChrome'
```

No network access is needed. The only exception is the system resolver,
which is asked about a `.invalid` name.

## What is covered

| File | Area |
|---|---|
| `tests/test_basic.cc` | version/identity strings, argument validation, HTTP/1.1, HTTP/2 and plain HTTP, TLS errors, status codes, response headers, network errors, timeouts, host rules, NetLog |
| `tests/test_headers.cc` | browser emulation: header order, values and casing for navigation, form POST, `fetch()`, XHR, cross-site, redirects, overrides, removal, identity configuration |
| `tests/test_http.cc` | bodies (binary, large up/downloads, chunked), gzip/deflate/br/zstd decoding, raw bodies, size limit, redirects (limits, 303/307 method handling), cookie jar, injected and merged cookies, cookies disabled, per-client jars |
| `tests/test_async.cc` | callback order, streaming, cancellation, abort from callbacks, pause/resume, release while running and from `on_complete`, deadlock refusal, 200 concurrent requests multiplexed on one HTTP/2 connection, parallelism, multi-threaded `perform`, client shutdown with transfers in flight, completion queue |
| `tests/test_proxy.cc` | SOCKS5 with remote name resolution, HTTP CONNECT proxy with authentication, failures, headers unchanged through proxies |
| `tests/test_stream.cc` | streamed bodies: uploads with known and unknown length over HTTP/1.1 and HTTP/2 (Content-Length vs chunked framing), empty bodies, writes from other threads, upload backpressure, length mismatches, writers released on failure, no rewind on 307, file uploads (whole, slices, resent on 307, missing file), streamed downloads (40 MiB both protocols, backpressure, headers before body, pollable fd, timeouts, errors after data, failure before headers), on_data precedence, upload and download together, 16 concurrent streamed transfers |
| `tests/test_websocket.cc` | WebSockets: Chrome's handshake headers, both delivery modes, pollable fd, subprotocols, Origin/header overrides, cookies both ways, deflate on/off, client/server/abrupt closes, close codes, fragmentation, large messages, size limit, backpressure, pause, pings, invalid UTF-8, refused/slow handshakes, release semantics, shutdown, proxies, 50 concurrent sockets |
| `tests/test_cpp.cc` | the C++ wrapper |
| `tests/c_api_smoke.c` | `crnet.h` compiles as strict C99 (`-pedantic -Werror`) and works from C |

## The test server

`tests/server/test_server.py` (Python 3 with `h2`) listens on ephemeral
127.0.0.1 ports and prints them as one JSON line. It provides:

- HTTPS with ALPN `h2` and `http/1.1`, HTTPS restricted to HTTP/1.1, and
  plain HTTP. Its certificate is issued at start-up by a throwaway CA
  whose PEM file the server reports as `ca`. Most test clients use the
  `insecure` option; tests of certificate verification trust the CA
  instead.
- A SOCKS5 proxy and an HTTP CONNECT proxy with Basic auth
  (`crnet:secret`). Both resolve `*.crnet-test` to 127.0.0.1
  themselves. Those names do not exist in DNS, so a successful request
  proves the name was resolved on the proxy.
- Endpoints: `/echo` (the request as JSON, headers in **wire order**,
  plus the client's source port), `/status/N`, `/redirect/N`,
  `/redirect-to?url=&code=`, `/set-cookie?k=v`, `/bytes/N`,
  `/stream/N?chunk=&delay_ms=`, `/delay/MS`, `/encoded/{gzip,deflate,br,zstd}`,
  `/upload-digest` and `/slow-upload?pause_ms=` (SHA-256 and framing of an
  uploaded body),
  `/dup-headers`, `/drop`.
- WebSocket endpoints under `/ws/` (any port, HTTP/1.1 upgrade): echo,
  handshake report (`/ws/headers`), server close/drop, fragmentation,
  bursts, big messages, pings, invalid UTF-8, refused, slow or
  cookie-setting handshakes. The full list is in the server's
  docstring.

HTTP/2 cookie crumbs are joined at the position of the first crumb.
python-h2's default would move them to the end, which would hide
header-order mistakes.

Run it by hand for experiments:

```bash
python3 tests/server/test_server.py
# {"https": 41234, "https_h1": 40111, "http": 39999, "socks5": ..., "http_proxy": ..., "cert": "..."}
build/examples/crget -k -i https://localhost:41234/echo
```

## Header expectations and Chrome captures

The expected header sequences in `tests/test_headers.cc` were captured
from a real Chrome. `tests/fingerprint/capture_chrome.py` repeats that
capture with an installed Chrome against the local test server:

```bash
tests/fingerprint/capture_chrome.py            # readable
tests/fingerprint/capture_chrome.py --json     # machine readable
```

It covers typed and script navigations, a form POST, `fetch()` GET with
and without a page-set `Accept`, a JSON POST with a custom header, a
cross-site `fetch()`, an XHR and a WebSocket handshake, over HTTP/2 and
HTTP/1.1. Compare
its output with the expectations whenever the Chromium version changes
(see [updating-chromium.md](updating-chromium.md)).

To compare crnet with Chrome against public fingerprinting services:

```bash
build/examples/crget https://tls.peet.ws/api/all
google-chrome --headless=new --dump-dom https://tls.peet.ws/api/all
```

`ja4` and `http2.akamai_fingerprint` must be identical. `ja3` differs
between any two Chrome connections (extension permutation).

## Adding tests

Use the `ClientTest` fixture (a client that trusts the test server)
and the helpers in `tests/common.h`: `HttpsUrl()`, `HttpsH1Url()`,
`HttpUrl()`, `Get()`, `Perform()`, `Result::echo()`, `HeaderNames()`,
`EchoHeader()`. New server behaviour goes into `route()` in
`test_server.py`.

Debugging aids:

- `CRNET_LOG_LEVEL=0` prints Chromium's logs to stderr.
- `crnet_client_config_set_netlog(cfg, "/tmp/x.json")` writes a NetLog.
  Load it in Chromium's [netlog viewer](https://netlog-viewer.appspot.com/).
- `-DCRNET_CHROMIUM_DEBUG=ON` builds Chromium with DCHECKs, and
  `-DCRNET_ASAN=ON` with AddressSanitizer.
