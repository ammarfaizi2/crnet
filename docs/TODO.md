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
