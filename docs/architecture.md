# Architecture

## Source layout

```
include/crnet/crnet.h     public C API (the only ABI)
include/crnet/crnet.hpp   header-only C++ wrapper
src/                      the library; linked into Chromium as //crnet
  BUILD.gn                GN shared_library target
  crnet.map               linker version script: export crnet_* only
  internal.h              shared internal declarations
  api.cc                  C entry points, argument checks, proxy parsing
  global.cc               process-wide Chromium initialisation
  event.{h,cc}            OneShotEvent: futex-based completion signal
  client.cc               crnet_client: network thread + URLRequestContext
  transfer.cc             crnet_transfer: one URLRequest state machine
  websocket.{h,cc}        crnet_ws: one WebSocketChannel per WebSocket
  profile.cc              browser headers, sec-ch-ua, header ordering
  queue.cc                crnet_queue: completion queue on an eventfd
  stream.{h,cc}           streamed bodies: UploadPipe + UploadDataStream,
                          DownloadBuffer
cmake/                    args.gn template, pkg-config, CMake package
scripts/                  fetch-chromium.sh, apply-patches.sh
chromium/VERSION          pinned Chromium release
chromium/patches/         the few patches crnet applies to Chromium
examples/, tests/         built with the system toolchain
```

## Threads

```
application threads            crnet
-------------------            -----
crnet_perform() ──post──▶  ┌── client A: "crnet-io" thread (IO message pump) ──┐
crnet_start()   ──post──▶  │  URLRequestContext, sockets, HTTP/2 sessions,     │
callbacks ◀───────────────  │  all transfers of client A as state machines      │
                            └────────────────────────────────────────────────────┘
                            ┌── client B: "crnet-io" thread ─────────────────────┐
                            └────────────────────────────────────────────────────┘
                            process-wide: Chromium ThreadPool (cert verification,
                            getaddrinfo fallback, file I/O, NetworkChangeNotifier)
```

- Every **client** owns one IO thread with Chromium's message pump and
  its own `URLRequestContext`. The context and everything in it is only
  touched on that thread.
- Every **transfer** is a `net::URLRequest` driven asynchronously on
  its client's thread. Starting one is a single task post. There are no
  per-request threads and no blocking round trips per read: data flows
  `URLRequest::Read()` → `on_data` (zero-copy from Chromium's buffer) or
  into the response body.
- **Blocking APIs** wait on a per-transfer one-shot event (a futex,
  `event.h`) that `Finish()` signals. Results are written before the
  signal, so no lock guards them; checking for completion is a single
  atomic load. Only a WebSocket's receive queue, which the network
  thread and `crnet_ws_recv()` callers share, has a mutex.
- **Creating a client** starts its thread and queues the context setup
  as the thread's first task, without waiting for it. Everything posted
  later runs after it.
- One network thread can multiplex thousands of concurrent requests,
  as Chrome's network service does. When a single thread's CPU (TLS,
  decompression) becomes the bottleneck, use several clients.

## Transfer state machine

```
StartOnNetworkThread
   build headers (profile.cc), create URLRequest, Start()
        │
        ├─ OnReceivedRedirect ── within max_redirects? follow : finish with the 3xx
        ├─ OnAuthRequired ────── proxy credentials? SetAuth : CancelAuth (deliver 401/407)
        ├─ OnSSLCertificateError insecure? continue : fail
        ▼
NetworkDelegate::OnBeforeStartTransaction   (each hop, after redirects too)
   merge cookies, recompute sec-fetch-site, put headers in Chrome's order
        ▼
OnResponseStarted ── error? → Finish
   fill response meta, on_response
        ▼
ReadMore ⇄ OnReadCompleted       (pause/resume gate this loop)
   on_data or append; max_body_size
        ▼
Finish(result)                    (exactly once)
   stop timer, cancel+release URLRequest, on_complete, wake waiters,
   enqueue to completion queue, drop the client's reference
```

Lifetime: a transfer is reference counted. Holders are the caller's
handle, the client's running set until `Finish`, a completion queue
until popped, and the task that deletes the `URLRequest`. That last
reference matters: `URLRequest::Cancel()` can still queue a delegate
notification, so the delegate must outlive its request.

## Streamed bodies

Both directions use a bounded byte pipe (1 MiB) between the
application's threads and the network thread; those mutexes are needed
because both sides touch the same bytes.

- **Upload**: `UploadPipe` is filled by `crnet_transfer_write()` and
  drained by `PipeUploadDataStream`, a `net::UploadDataStream` (chunked
  when the length is unknown). A read that finds the pipe empty returns
  `ERR_IO_PENDING` and marks the pipe; the next write posts one wake-up
  task, immediately, before the writer may block on a full buffer.
  Completing a read can synchronously start the next one, so the
  pending state is taken out before completing.
- **File upload** is Chromium's `UploadFileElementReader` on a
  ThreadPool task runner. It is rewindable.
- **Download**: `DownloadBuffer` receives what `URLRequest::Read()`
  produced. When it is full, the transfer stops calling `Read()`, and a
  reader that frees half of it posts a resume. An eventfd mirrors
  "a read would not block".

## WebSockets

A `crnet_ws` owns a `net::WebSocketChannel` on its client's thread and
implements `net::WebSocketEventInterface` (through a small adapter the
channel owns). It plays the role of Chrome's network service
(`services/network/websocket.cc`):

- the handshake gets the renderer's single extra header (`User-Agent`),
  an `Origin`, and a first-party `IsolationInfo` of request type
  `kOther`. A `HeaderHook` on the handshake's `URLRequest` (created via
  `OnCreateURLRequest`) applies header removals and the Cookie merge
  without reordering;
- received frames are reassembled into messages. `HasPendingDataFrames()`
  is the backpressure valve: returning true makes the channel stop
  reading (and delay answering a server Close) until `ReadFrames()` is
  called again on resume;
- `OnSendDataFrameDone()` arrives once per `SendFrame()`, in order,
  which drives `bufferedAmount`;
- proxy credentials must be handed over through the auth callback
  (`ERR_IO_PENDING`), because `WebSocketStream` ignores synchronously
  returned ones. The callback is bound to the handshake delegate, so it
  only runs while the socket is still unfinished;
- as with transfers, the channel is deleted on a later task that holds
  a reference to the `crnet_ws`. `on_close` runs exactly once from
  `Finish()`.

## Matching Chrome's network configuration

`//net` alone does not produce Chrome's behaviour: several pieces are
configured by Chrome's browser process and network service. crnet
reproduces them in `client.cc`:

| Chrome component | What it configures | crnet |
|---|---|---|
| `services/network/network_context.cc` + `components/network_session_configurator` | HTTP/2 settings, QUIC parameters | links and calls the same `ParseCommandLineAndFieldTrials()` |
| `chrome/browser/ssl/ssl_config_service_manager.cc` | TLS Trust Anchor IDs from the Chrome Root Store | `ChromeSSLConfigService` does the same |
| `chrome/browser/net/stub_resolver_config_reader.cc` | built-in DNS client on Linux | `InsecureDnsMode::kEnabledBuiltIn` |
| `services/network/...` CertNetFetcher | AIA fetching of missing intermediates | `CertNetFetcherURLRequest` |
| Blink + network service (`sec_header_helpers.cc`, frame accept header, UA client hints) | browser request headers | `profile.cc` |

Where //net offers no hook, crnet patches Chromium, as little as
possible: see `chromium/patches/`, applied by
`scripts/apply-patches.sh` and checked at configure time.

## Process-wide initialisation (`global.cc`)

On the first `crnet_client_new()`, exactly once (`std::call_once`;
later calls cost one atomic load):

1. a leaked `base::AtExitManager` (Chromium singletons register with
   it; running them at process exit while crnet threads may still run is
   unsafe, so they are never run),
2. `base::CommandLine`, logging to stderr (level from `CRNET_LOG_LEVEL`),
3. ICU with embedded data,
4. `base::FeatureList` with the overrides from `crnet_global_init()` or
   `CRNET_ENABLE_FEATURES`/`CRNET_DISABLE_FEATURES`,
5. the `ThreadPool`,
6. a `NetworkChangeNotifier`, living on a ThreadPool sequence (it needs
   a sequence, not a thread). It feeds the system DNS configuration to
   the built-in resolver and lets `//net` react to network changes.

## Design decisions

**C ABI only.** Chromium is compiled against its own libc++ in the
`std::__Cr` namespace, statically linked and hidden. No C++ type can
safely cross the boundary to a program built with libstdc++ or a
different libc++. Opaque C handles avoid the problem entirely; the
C++ convenience layer is header-only and built by the consumer.

**Built inside the Chromium tree.** Extracting `//net` into a standalone
build was rejected. It depends on Chromium's toolchain and build
flags, and the dependency set changes with every release. Building
`//crnet` as a GN target keeps each Chromium upgrade a pin change plus
a rebuild. CMake wraps GN/ninja so consumers never deal with them.

**One thread per client, not a thread pool of contexts.** Sharing one
context per client is what makes connection reuse, HTTP/2 multiplexing
and a shared cookie jar work. Many contexts would each open their own
connections and look less like one browser.

**Browser headers in a NetworkDelegate hook.** `//net` inserts
`Referer`, `Accept-Encoding`, `Accept-Language` and `Cookie` itself, at
different moments. Ordering headers once, right before each
transaction (`OnBeforeStartTransaction`), is the only point where the
final set is known, also after redirects.

**Few Chromium patches.** Earlier designs (chnet) patched Chromium to
keep `Referer`, disable decompression and so on. Current `//net` offers
proper hooks for all of this: `URLRequest::SetReferrer()` with
`NEVER_CLEAR`, `set_client_side_content_decoding_enabled()`, the
NetworkDelegate. crnet patches only what has no hook (a race on a
global that only multi-threaded use exposes, SOCKS5 error detail that
Chromium discards, a WebSocket extension offer it hard-codes), and
keeps each patch small, since every patch makes upgrades costlier.

**Allocator.** PartitionAlloc's malloc shim is disabled
(`use_allocator_shim=false`). A library must not take over the host
process' `malloc`.
