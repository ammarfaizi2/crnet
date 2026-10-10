# crnet documentation

crnet is a network client library (HTTP and WebSocket) built on
Chromium's network stack.
These documents describe how to build it, how to use it and how it
works.

| Document | What it covers |
|---|---|
| [building.md](building.md) | Prerequisites, fetching Chromium, building with CMake, build options, installing, linking from your project, deployment |
| [api.md](api.md) | The C API: objects, lifetimes, threading rules, blocking/async/queue usage, errors, reference |
| [cpp.md](cpp.md) | The header-only C++17 wrapper |
| [websocket.md](websocket.md) | WebSocket client: opening, messaging, flow control, closing semantics, Chrome's handshake |
| [browser-emulation.md](browser-emulation.md) | What crnet makes look like Chrome, request modes, header rules, cookies, limits |
| [architecture.md](architecture.md) | Internals: threads, request state machine, Chromium integration, design decisions |
| [testing.md](testing.md) | Running and extending the test suite, the local test server, capturing Chrome's headers |
| [updating-chromium.md](updating-chromium.md) | Moving crnet to a new Chrome release |

## At a glance

```
your program (any compiler, any C++ stdlib)
        |  C ABI: crnet_* functions, opaque handles
        v
+------------------- libcrnet.so ----------------------------+
| api.cc       argument checks, handles                      |
| client.cc    URLRequestContext configured like Chrome      |
| transfer.cc  one URLRequest state machine per request      |
| websocket.cc one WebSocketChannel per WebSocket            |
| profile.cc   Chrome's browser-level headers and their order|
| queue.cc     completion queue (eventfd)                    |
|------------------------------------------------------------|
| Chromium //net, BoringSSL, QUICHE, //base, libc++ (private)|
+------------------------------------------------------------+
```

The public headers are `include/crnet/crnet.h` (C) and
`include/crnet/crnet.hpp` (C++). The headers themselves are documented
inline and are the authoritative reference for every function.
