# crnet TODO

This file tracks planned work for crnet: wanted features, known
problems and improvements that are not done yet.

How to use it:

- Items are grouped by topic. Each item has a slug heading
  (`### some-slug`), not a number, so removing an item never renumbers
  the rest. Refer to items by slug, for example in commit messages
  ("Implements `connect-timeout` from docs/TODO.md").
- Pick a short, unique, lower-case slug when adding an item. Say why
  it matters and what is known about the approach, not only what is
  wanted.
- **When an item is done, remove it from this file in the same commit
  (or series) that completes the work.** If only part of it is done,
  edit the item down to what is left. The git history keeps the record
  of finished items; this file lists open work only.
- Every new public capability also gets a `CRNET_HAS_*` feature-test
  macro and a minor version bump (see "Versions and feature tests" in
  [api.md](api.md)).

## Errors and diagnostics

### socks5-error-detail

Tell SOCKS5 failures apart.

- Today: `net/socket/socks5_client_socket.cc` returns
  `ERR_SOCKS_CONNECTION_FAILED` (-120) for every SOCKS failure, and
  `proxy_fallback.cc` passes it through unchanged. That covers a close
  during the greeting, a method other than 0x00, any non-zero REP, a
  bad version or ATYP, and a truncated reply. The REP byte appears only
  in the NetLog event `SOCKS_SERVER_ERROR`.
- Impact: a caller deciding whether to retry cannot separate a flaky
  proxy (REP 0x04, a truncated reply, a close mid-handshake) from one
  that refuses on principle, so it must treat all of them as final.
- What a caller needs to classify:
  - REP 0x01 and 0x03 to 0x06, and transport failures (EOF, RST,
    timeout): retryable.
  - REP 0x02, 0x07, 0x08 and anything above 0x08: not retryable.
  - Method refused (0xFF): authentication failure.
- Possible designs:
  - A per-response accessor such as `crnet_response_socks_reply(resp)`
    returning the REP byte, or -1 when no reply was read, plus the
    stage that failed (greeting, auth, connect reply) and whether the
    failure was transport or protocol.
  - Distinct net errors via a Chromium patch:
    `ERR_SOCKS_CONNECTION_HOST_UNREACHABLE` (-121) for REP 0x03 and
    0x04 (the code exists and the SOCKS4 client uses it); the original
    transport error kept for a socket that dies mid-handshake instead
    of rewriting it to -120; a separate code for "no acceptable
    method".
  - Without a Chromium patch: a per-transfer NetLog observer that
    captures `SOCKS_SERVER_ERROR.error_code`,
    `SOCKS_UNEXPECTED_AUTH.method`,
    `SOCKS_UNEXPECTEDLY_CLOSED_DURING_*` and
    `SOCKS_UNKNOWN_ADDRESS_TYPE`.
- Testing needs fault injection in the SOCKS5 stub of
  `tests/server/test_server.py` (each REP value, a refused method, a
  truncated reply, a close at each stage); today it only ever accepts.

### content-decoded-flag

Report whether the response body was decoded.

- Today: with decompression on, the body arrives decoded but
  `Content-Encoding` and `Content-Length` still describe the encoded
  bytes. To know which applies, a caller has to mirror Chromium's rule
  from `FilterSourceStream::GetContentEncodingTypes()`: if every coding
  is br, deflate, gzip, x-gzip or zstd, the body was decoded; if any
  coding is unknown (`identity` included), the body is raw. Such a
  mirror can drift on a Chromium bump.
- Wanted: `crnet_response_content_decoded(resp)`, ideally with the
  list of codings applied, taken from what `URLRequestHttpJob` actually
  did.

### connection-timing-info

Connection and timing info on the response.

- Wanted: whether the socket was reused, a connection id, and
  `LoadTimingInfo` (DNS, connect, TLS, send, headers).
- Why: tests and logs can tell which connection carried a request
  without counting accepts at the server, and slow requests can be
  attributed to a phase.

### transfer-progress

Upload and download progress.

- Wanted: bytes sent and total for the request body, bytes received
  (on the wire and decoded) and the expected total, readable at any
  time from another thread, and optionally an `on_progress` callback
  with a rate limit.
- Why: large streamed transfers give no feedback until they finish;
  callers of `crnet_request_set_body_file()` cannot report upload
  progress at all.
- Notes: `URLRequest::GetUploadProgress()` and
  `GetTotalReceivedBytes()` provide the numbers on the network thread;
  publish them through relaxed atomics so reading stays lock-free.

## Connections and proxies

### connect-timeout

A connect timeout, separate from the whole-request timeout.

- Today: only a whole-request timeout exists, so a stalled proxy or
  an unreachable host costs the full request deadline.
- Wanted: `crnet_request_set_connect_timeout(req, ms)` plus a client
  default. It should cover DNS, TCP, the proxy handshake and TLS, and
  fail with a distinguishable error.
- Hook: `URLRequest::Delegate::OnConnected` exists in the pinned
  Chromium (`net/url_request/url_request.h`) and `transfer.cc` does not
  implement it. Arm a timer at start and stop it there.
- Check that it behaves correctly on a reused pooled socket and that
  it applies on each redirect hop.

### proxy-bypass-rules

A proxy bypass list, or a way to disable the implicit loopback bypass.

- Today: Chromium's `ProxyHostMatchingRules::MatchesImplicitRules`
  sends these destinations direct even with a proxy configured:
  `localhost` and `*.localhost`; 127/8, `::1` and IPv4-mapped
  loopback; 169.254/16 and `fe80::/10`.
  `crnet_client_config_set_proxy()` takes no bypass list, so
  `<-loopback>` cannot be expressed: a proxied request to `localhost`
  succeeds without the proxy ever seeing a connection.
- Impact: proxy tests against a loopback server must use a fake
  hostname that the proxy maps to 127.0.0.1 (the test server's
  `.crnet-test` names).
- Wanted: `crnet_client_config_set_proxy_bypass(cfg, rules)` in
  Chromium's bypass syntax, or a boolean meaning "proxy every
  destination". Chrome's behaviour stays the default.

### per-request-proxy

Choose the proxy per request instead of per client.

- Today: the proxy is fixed in the client config, so rotating egress
  needs one client per proxy, each with its own network thread,
  context, DNS cache and connection pools.
- Wanted: `crnet_request_set_proxy(req, uri)`, falling back to the
  client's proxy.
- Notes: connection pools are already keyed by proxy chain, so sharing
  a context is safe. The choice has to reach proxy resolution, for
  example through a `ProxyDelegate`; check what the pinned Chromium
  passes to it.

### socks5-auth

SOCKS5 username/password authentication (RFC 1929).

- Today: Chromium's SOCKS5 client offers only method 0x00, so a proxy
  that requires a credential cannot be used.
- Cost: needs a Chromium patch to the greeting and an auth
  sub-negotiation. Only worth it once a credentialed SOCKS5 proxy is
  actually needed.

### fresh-connection

A per-request fresh connection.

- Wanted: a request flag that neither takes a pooled socket nor returns
  it to the pool, or a client option of zero idle sockets.
- Why: retrying a request that met an anti-bot challenge on a new
  connection; honouring a "no keep-alive" setting;
  `crnet_client_close_idle()` is client-wide and races with concurrent
  requests.

### local-address-binding

Bind outgoing connections to a local address or interface.

- Wanted: a client option to pick the source IP (or interface) for
  TCP and QUIC sockets, for hosts with several egress addresses.
- Notes: the socket pools do not expose a bind address; this likely
  needs a custom `ClientSocketFactory`. Check its effect on Happy
  Eyeballs and on QUIC connection migration.

### secure-dns

DNS over HTTPS.

- Today: crnet resolves with the system resolver or Chrome's built-in
  insecure DNS client.
- Wanted: a DoH server template and mode (automatic or secure only),
  like Chrome's secure DNS setting.
- Why: some networks tamper with or block plain DNS, and DoH still
  delivers HTTPS records (ECH configs, ALPN hints) there.
- Notes: `DnsConfigOverrides` carries the DoH config and secure DNS
  mode into the standalone host resolver.

## TLS

### extra-trust-anchors

Extra trust anchors.

- Today: crnet verifies against the Chrome Root Store only, so a
  private CA (or a test server's self-signed certificate) can only be
  used with verification turned off.
- Wanted: `crnet_client_config_add_trust_anchor_pem()` or a file-based
  variant, and optionally an "only these anchors" mode.
- Payoff: the local TLS tests could run with verification on.

### client-certificates

Client certificates, via `OnCertificateRequested` ->
`ContinueWithCertificate` with a PEM certificate and key. Low
priority until a user needs mutual TLS.

## Browser emulation

### platform-profiles

Consistent per-OS browser profiles.

- Today: `crnet_client_config_set_platform()` changes only
  `sec-ch-ua-platform`; the User-Agent must be set separately and
  nothing checks that the two agree. A mismatch is an easy bot signal.
- Wanted: one switch (Linux, Windows, macOS, maybe Android) that sets
  the User-Agent, `sec-ch-ua-platform`, `sec-ch-ua-mobile` and any
  other per-platform difference together, verified against captures
  from real Chrome on each platform.

### http-cache

An optional HTTP cache.

- Today: the context is built with `DisableHttpCache()`, so repeat
  fetches never send `If-None-Match` / `If-Modified-Since` and always
  re-download.
- Wanted: off by default; an in-memory or on-disk cache with a size
  limit, and a per-request "bypass cache" flag.
- Why: saves bandwidth for crawlers that revisit pages, and makes
  revisits look like a real browser's conditional requests.

## Persistence

### cookie-jar-persistence

Cookie jar import and export, in JSON or Netscape format, so a crawl
session (`__cf_bm`, site cookies) survives across runs.

### network-state-persistence

Persist what Chrome keeps between runs besides cookies.

- Today: every new client behaves like a freshly installed browser:
  no HSTS entries learned from sites, no Alt-Svc / HTTP/3 hints, no
  known broken alternative services.
- Wanted: an optional state file holding HTTP server properties
  (Alt-Svc, QUIC server info) and dynamic HSTS, so a returning client
  starts on HTTP/3 and HTTPS like a returning Chrome.
- Notes: Chromium has persisters for both (`HttpServerPropertiesManager`,
  `TransportSecurityPersister`). Pairs with `cookie-jar-persistence`;
  one "profile directory" option could cover all three.

## Streaming

### stream-write-readiness

A pollable "can write" signal for streamed uploads.

- Today: `crnet_transfer_write()` blocks (or times out) when the 1 MiB
  pipe is full, and `crnet_transfer_fd()` only signals readability of
  the response stream. An event-loop caller cannot wait for upload
  space without a thread.
- Wanted: an fd (or the existing one with a direction flag) that is
  readable while the upload pipe has free space, plus a non-blocking
  write that returns how much it accepted.

### transfer-result-getter

A non-blocking result getter.

- Today: `Finish()` in `transfer.cc` signals `done_` only after
  `on_complete` returns. So `crnet_transfer_wait(t, 0)` from another
  thread, right after `on_complete` has handed the transfer off, can
  still return `CRNET_E_TIMEOUT`.
- Wanted: `crnet_transfer_result(t)`, valid once `on_complete` has
  been entered, or signal before the callback. Also document that the
  response object is immutable after `on_complete`.

## WebSocket

### websocket-parity

WebSocket controls that other WebSocket clients commonly offer.

- Missing today:
  - sending a ping and receiving pong events, or an auto-ping
    interval;
  - an idle timeout;
  - a maximum message size;
  - a permessage-deflate on/off switch.
- Note that Chrome itself offers none of these to pages; anything that
  changes the handshake (such as turning permessage-deflate off) makes
  the connection differ from Chrome's, so keep Chrome's behaviour the
  default.

### ws-over-h2-test

A local test for WebSocket over HTTP/2 (RFC 8441). The code path is
Chromium's and works against public servers, but the local test
server only speaks WebSocket over HTTP/1.1, so the suite does not
cover it.

## Build and tooling

### tsan-build

ThreadSanitizer support.

- Today: `libcrnet.so` is not instrumented, so applications running
  under TSan must suppress reports from it.
- Wanted: an optional `CRNET_TSAN` build (`is_tsan=true`), like
  `CRNET_ASAN`. Likely needs clang-built tests and dropping `-z defs`
  for the instrumented build.

### continuous-integration

Automated builds and tests on push.

- Today: there is no CI; everything is run by hand.
- Problem: a Chromium checkout is about 30 GB and a cold build takes
  a long time, so hosted runners are impractical. Options are a
  self-hosted runner that keeps the checkout and `out/` directory
  between runs, or a cached build image.
- Should run the full test suite and the fingerprint comparison
  against the matching Chrome release.

### language-bindings

Bindings for other languages, starting with Python (ctypes or cffi
over the C ABI, no compiler needed at install time). Many crawlers are
written in Python, and the C ABI was designed so that this needs no
C++ on the binding side. Optional.

## Not planned

### verbatim-path

A "send the path verbatim" option. GURL removes dot segments, reads a
backslash as a slash and decodes a percent-encoded host. Bypassing
GURL would break Chromium's assumptions throughout //net and is
unlikely to be worth the cost. Kept here so it is not proposed again
without new reasons.
