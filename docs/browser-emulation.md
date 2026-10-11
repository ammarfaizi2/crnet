# Browser emulation

crnet aims to be indistinguishable from the Chrome release it was built
from (see `chromium/VERSION`) at the network level. This document lists
what that covers, where each part comes from, and what is out of reach
for a library.

## Layers

| Layer | Source in crnet | Matches Chrome because |
|---|---|---|
| TCP | the OS (or the proxy) | not controlled by Chrome either |
| TLS ClientHello: cipher suites, extensions and their permutation, GREASE, supported groups incl. X25519MLKEM768, key shares, signature algorithms, ALPN, ALPS, certificate compression, ECH (GREASE or real), session tickets | Chromium `//net` + BoringSSL, unmodified | it *is* Chrome's code |
| TLS Trust Anchor IDs extension | `client.cc` (`ChromeSSLConfigService`) | Chrome's browser process configures it from the compiled-in Chrome Root Store; crnet does the same |
| Certificate verification, AIA fetching | Chromium `CertVerifier` with the Chrome Root Store, `CertNetFetcherURLRequest` | same verifier and roots; configured extra trust anchors only affect verification, not the handshake |
| HTTP/2: SETTINGS, connection WINDOW_UPDATE, HEADERS priority, pseudo-header order, `priority` header (RFC 9218) | `//net` + Chrome's `network_session_configurator` | same code and parameters |
| HTTP/3 / QUIC | `//net` (QUICHE) | same code; used when the server advertises h3 |
| DNS: built-in resolver, HTTPS/SVCB records (ECH configs, ALPN hints) | `//net` `HostResolver` in Chrome's Linux mode | same code and mode |
| Content decoding: gzip, deflate, br, zstd | `//net` | same `accept-encoding` and decoders |
| Browser request headers and their order | `profile.cc` | captured from Chrome (see below) |
| Cookies (SameSite, Secure, partitioning rules) | `//net` `CookieMonster` | same cookie store |

Fingerprint check against Chrome 155 (tls.peet.ws), identical for crnet
and Chrome:

```
JA4     t13d1517h2_8daaf6152771_cb7bf5808d99
Akamai  1:65536;2:0;4:6291456;6:262144|15663105|0|m,a,s,p
```

JA3 differs between *any* two Chrome connections, because Chrome
randomly permutes its TLS extensions; JA4 sorts them and is stable.

## Request modes

The headers a browser adds depend on what kind of request it makes. A
request's mode selects which one crnet reproduces.

### `CRNET_MODE_NAVIGATE` (default): a top-level page load

Typed URL, link click or bookmark (`GET`), or a form submission
(`POST`). HTTP/2 headers, exactly as Chrome 155 sends them:

```
sec-ch-ua: "Google Chrome";v="155", "Chromium";v="155", "Not(A:Brand";v="24"
sec-ch-ua-mobile: ?0
sec-ch-ua-platform: "Linux"
upgrade-insecure-requests: 1
user-agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36
accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/jxl,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7
sec-fetch-site: none
sec-fetch-mode: navigate
sec-fetch-user: ?1
sec-fetch-dest: document
[referer]
accept-encoding: gzip, deflate, br, zstd
accept-language: en-US,en;q=0.9
[cookie]
priority: u=0, i
```

A navigation with a non-`GET`/`HEAD` method (a form `POST`) also gets
`cache-control: max-age=0` first and an `origin` header (the referrer's
origin, or `null`), and the request's `content-type` goes between
`upgrade-insecure-requests` and `user-agent`, as in Chrome.

### `CRNET_MODE_FETCH`: a page's `fetch()` or `XMLHttpRequest`

```
[content-length]
[custom headers]
sec-ch-ua-platform: "Linux"
[x-requested-with]
user-agent: ...
[accept, if the page set it]
sec-ch-ua: ...
[content-type]
sec-ch-ua-mobile: ?0
accept: */*            (when the page did not set one)
[origin]               (cross-origin, or any non-GET/HEAD)
sec-fetch-site: same-origin
sec-fetch-mode: cors
sec-fetch-dest: empty
[referer]
accept-encoding: gzip, deflate, br, zstd
accept-language: en-US,en;q=0.9
[cookie]
priority: u=1, i
```

The page is assumed to be the `Referer`'s origin, or, without a
`Referer`, the target's own origin (a same-origin API call).

The slots for `x-requested-with`, a page-set `accept` and `content-type`
reflect where Chrome puts them. Chrome orders page-set headers in an
order that depends on their names; crnet puts unknown custom headers
first, where Chrome put the ones observed so far.

### `CRNET_MODE_RAW`

No browser headers. Only what `//net` itself adds (`User-Agent`,
`Accept-Encoding`, `Accept-Language`, `Cookie`) plus your own headers,
in the order you set them. Use it for API clients that should not
pretend to be a browser page.

### HTTP/1.1

The order is the same, preceded by `Host` and `Connection: keep-alive`.
Names use Chrome's HTTP/1.1 spelling: client hints in lower case
(`sec-ch-ua`), the rest capitalised (`User-Agent`, `Sec-Fetch-Site`).
HTTP/2 and HTTP/3 lower-case all names.

### WebSocket handshakes

WebSocket handshakes don't follow these modes: they use exactly what
Chrome sends for `new WebSocket(...)`. That means no client hints and no
fetch metadata; `User-Agent` comes before `Upgrade` and the rest is in
`//net`'s order. See [websocket.md](websocket.md#what-chrome-sends).

## Header rules

- **Overriding**: `crnet_request_set_header()` replaces a default
  in place, keeping Chrome's position and spelling.
- **Removing**: a `NULL` value removes the header, including
  defaults added by `//net` (e.g. `User-Agent`, `Accept-Language`).
- **Unknown headers**: in navigate mode they go after
  `sec-fetch-dest`; in fetch mode, first.
- **`sec-fetch-site`** is computed for every hop of a redirect chain
  from the initiator (the referrer's origin) to each URL in the chain.
  The most distant relation wins: `same-origin` < `same-site` <
  `cross-site`. Without an initiator a navigation is `none`. Setting the
  header explicitly disables the computation.
- **`Referer`** is sent exactly as given (no referrer-policy trimming).
  Browsers trim cross-origin referrers to the origin by default
  (`strict-origin-when-cross-origin`); give the origin yourself where a
  browser would.
- **`Cookie`**: the jar's cookies are merged with a caller-supplied
  `Cookie` header. Caller values come first and win on name clashes.
- **Priority**: navigations use `HIGHEST` (`u=0`), fetches `MEDIUM`
  (`u=1`). Override with `crnet_request_set_priority()`.

## Identity

The defaults describe Chrome on Linux x86-64 for the build's major
version. `sec-ch-ua` uses Chrome's own GREASE algorithm (seeded with the
major version), so it changes correctly when the Chromium pin is bumped.

To present a different platform, pick a platform profile. It sets the
`User-Agent` and `sec-ch-ua-platform` together, as Chrome sends them on
that platform:

```c
crnet_client_config_set_platform_profile(cfg, CRNET_PLATFORM_WINDOWS);
```

| Profile | `User-Agent` platform | `sec-ch-ua-platform` |
|---|---|---|
| `CRNET_PLATFORM_LINUX` (default) | `X11; Linux x86_64` | `"Linux"` |
| `CRNET_PLATFORM_WINDOWS` | `Windows NT 10.0; Win64; x64` | `"Windows"` |
| `CRNET_PLATFORM_MACOS` | `Macintosh; Intel Mac OS X 10_15_7` | `"macOS"` |

The strings come from Chromium's
`components/embedder_support/user_agent_utils.cc`
(`GetUnifiedPlatform()`, `GetPlatformForUAMetadata()`). Chrome's
reduced User-Agent freezes the OS version and CPU, so one string covers
every release of an OS: Windows 11 still says `Windows NT 10.0`, and an
Apple silicon Mac says `Intel Mac OS X 10_15_7`. All three are desktop
platforms (`sec-ch-ua-mobile: ?0`), and nothing else in the request
headers, the TLS handshake or HTTP/2 depends on the platform.

`crnet_client_config_set_user_agent()` and
`crnet_client_config_set_platform()` still override single values, and
take precedence over the profile whatever the order of the calls. Keep
the major version equal to the build's: the TLS and HTTP/2 fingerprint
always says "Chrome 155", and a mismatching `User-Agent` is a classic
bot signal.

## What crnet does not do

crnet is a network stack, not a browser:

- **No JavaScript.** Anti-bot systems that serve a JavaScript challenge
  (Cloudflare "Just a moment...", `cf-mitigated: challenge`, Turnstile)
  expect it to run and post results back. crnet cannot solve those.
  What it does is avoid being challenged in the first place wherever
  the decision is based on the network fingerprint. Real Chrome gets
  challenged sometimes too; it just solves the challenge silently.
- **No subresource behaviour.** A real page load is followed by CSS,
  script, image and beacon requests. Systems that score a session on
  that behaviour can tell the difference over time.
- **No HTTP cache unless enabled, no service workers, no
  prefetch/preconnect.** With `crnet_client_config_set_http_cache()` the
  cache is Chrome's own, so revalidation requests (`If-None-Match`,
  `If-Modified-Since`) are formed as Chrome forms them.
- **No high-entropy client hints** (`sec-ch-ua-full-version-list` etc.)
  unless you add them yourself after a server asked for them with
  `Accept-CH`.
- **No browser-side state** such as Finch field trials. crnet behaves
  like a fresh Chrome install with default features. Use
  `CRNET_ENABLE_FEATURES`/`CRNET_DISABLE_FEATURES` to change features.

A cookie such as `cf_clearance` that a real browser obtained can be
reused through `crnet_client_set_cookie()` or a `Cookie` header, as
long as the site's checks (IP, User-Agent, expiry) still accept it.
