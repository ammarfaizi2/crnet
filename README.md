# crnet

crnet is a C/C++ network client library built on Chromium's own network
stack (`//net`). It provides an HTTP(S) client (HTTP/1.1, HTTP/2 and
HTTP/3) and a WebSocket client, with BoringSSL TLS, connection pooling
and certificate verification against the Chrome Root Store.

Requests go through the same code Chrome uses, so the TLS ClientHello
(JA3/JA4), the HTTP/2 connection preface (SETTINGS, WINDOW_UPDATE,
priorities) and the request headers are those of the Chrome release
crnet was built from. Sites that fingerprint clients see crnet as
Chrome. The current build tracks **Chrome 155.0.8059.39**.

```c
#include <crnet/crnet.h>

crnet_client *client = crnet_client_new(NULL);
crnet_request *req = crnet_request_new("GET", "https://example.com/");
crnet_response *resp;

if (crnet_perform(client, req, &resp) == CRNET_OK)
	printf("%d %s\n", crnet_response_status(resp),
	       crnet_response_body(resp, NULL));

crnet_response_free(resp);
crnet_request_free(req);
crnet_client_free(client);
```

```cpp
#include <crnet/crnet.hpp>

crnet::Client client(crnet::ClientConfig().proxy("socks5h://127.0.0.1:1080"));
crnet::Response r = client.get("https://example.com/");
std::cout << r.status() << " " << r.protocol() << "\n" << r.body();
```

## Highlights

- **Chrome's wire fingerprint**: TLS, HTTP/2 and browser request headers
  (client hints, fetch metadata, `accept`, header order and casing) for
  page navigations, form posts, `fetch()` and XHR.
- **One client, many requests**: a client owns one network thread; any
  number of requests run on it concurrently and share connections,
  HTTP/2 sessions and the cookie jar.
- **Three ways to run requests**: blocking, asynchronous callbacks with
  streaming and flow control, or a completion queue with a pollable fd
  for your own event loop.
- **Streamed bodies** for big files: upload from a file or from your own
  writes, read responses as they arrive. Memory stays constant whatever
  the size.
- **WebSockets** through Chromium's implementation: Chrome's handshake,
  permessage-deflate, cookies and proxies; messages via callbacks or a
  pollable queue.
- **Proxies**: SOCKS5 (with remote DNS), SOCKS4, HTTP and HTTPS proxies,
  with proxy authentication for HTTP(S) proxies.
- **A stable C ABI**: `libcrnet.so` exports only `crnet_*` C symbols.
  Chromium, BoringSSL and Chromium's libc++ stay private, so it links
  into any C or C++ program built with any toolchain.

## Quick start

```bash
scripts/fetch-chromium.sh        # once: ~30 GB Chromium checkout
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

```bash
build/examples/crget -i https://tls.peet.ws/api/all
```

## Documentation

See [docs/](docs/README.md):

- [Building and linking](docs/building.md)
- [C API guide](docs/api.md) and [C++ wrapper](docs/cpp.md)
- [WebSockets](docs/websocket.md)
- [Browser emulation](docs/browser-emulation.md)
- [Architecture](docs/architecture.md)
- [Testing](docs/testing.md)
- [Updating Chromium](docs/updating-chromium.md)

## License

BSD-3-Clause, the same license as Chromium; see [LICENSE](LICENSE).

libcrnet.so statically links Chromium and the third-party code it uses
(BoringSSL, QUICHE, ICU, zlib, brotli, zstd, ...). Binary distributions
must also carry their notices; Chromium can generate them with
`tools/licenses/licenses.py`.
