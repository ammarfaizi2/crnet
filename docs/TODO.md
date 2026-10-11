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

## Connections and proxies

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

### fresh-connection

A per-request fresh connection.

- Wanted: a request flag that neither takes a pooled socket nor returns
  it to the pool, or a client option of zero idle sockets.
- Why: retrying a request that met an anti-bot challenge on a new
  connection; honouring a "no keep-alive" setting;
  `crnet_client_close_idle()` is client-wide and races with concurrent
  requests.

### interface-binding

Bind outgoing connections to a network interface (`SO_BINDTODEVICE`).

- Today: `crnet_client_config_set_local_address()` picks the source
  address, but the kernel's routing still picks the interface, so
  without policy routing a packet can leave through another interface
  than the one that owns the address.
- Notes: the option has to be set on the descriptor before connect.
  `TCPClientSocket` does not expose its descriptor, but it can adopt an
  unconnected `TCPSocket`; the UDP side can reuse the wrapper in
  `src/local_address.cc`. Older kernels require `CAP_NET_RAW` for it.

## Browser emulation

### platform-profiles

More per-OS browser profiles, and captures to back them.

- Today: `crnet_client_config_set_platform_profile()` covers Linux,
  Windows and macOS, with strings taken from the Chromium source
  (`components/embedder_support/user_agent_utils.cc`). Only the Linux
  profile has been compared with captures from a real Chrome.
- Wanted: run `tests/fingerprint/capture_chrome.py` with Chrome on
  Windows and macOS and confirm that nothing but the User-Agent and
  `sec-ch-ua-platform` differs (header order, TLS and HTTP/2 included).
- Maybe: an Android profile (`sec-ch-ua-mobile: ?1`, a "Mobile" User-Agent).
  Chrome on Android may differ beyond headers (field trials, HTTP/2
  settings), so it needs captures from a real device before it is
  offered.

## Build and tooling

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
