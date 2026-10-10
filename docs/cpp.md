# C++ wrapper

`<crnet/crnet.hpp>` is a header-only C++17 layer over the C API. It adds
RAII ownership, builder-style setters, `std::string`/`std::string_view`
conversions, exceptions and `std::function` callbacks. It calls
`libcrnet.so` only through the C ABI, so it works with any C++ standard
library and compiler.

```cpp
#include <crnet/crnet.hpp>
#include <iostream>

int main()
{
	crnet::Client client(crnet::ClientConfig()
				     .proxy("socks5h://127.0.0.1:1080")
				     .accept_language("id-ID,id;q=0.9")
				     .timeout_ms(30000));

	crnet::Response r = client.get("https://example.com/");
	std::cout << r.status() << " " << r.protocol() << "\n";
	for (auto &[name, value] : r.headers())
		std::cout << name << ": " << value << "\n";
	std::cout << r.body() << "\n";
}
```

## Classes

| Class | Wraps | Notes |
|---|---|---|
| `crnet::ClientConfig` | `crnet_client_config` | chainable setters; throws `crnet::Error` on invalid values |
| `crnet::Client` | `crnet_client` | `perform()`, `get()`, `post()`, `start()`, cookie helpers |
| `crnet::Request` | `crnet_request` | `Request(url, method = "GET")`, chainable setters |
| `crnet::Response` | owned `crnet_response` | move-only |
| `crnet::ResponseView` | borrowed `const crnet_response *` | what callbacks receive |
| `crnet::Transfer` | `crnet_transfer` | move-only; destructor cancels and waits |
| `crnet::Handlers` | `crnet_callbacks` | `std::function` callbacks |
| `crnet::WebSocket` | `crnet_ws` | move-only; destructor aborts an open connection and waits |
| `crnet::WsHandlers`, `crnet::WsMessage` | `crnet_ws_callbacks`, a received message | |
| `crnet::Error` | `std::runtime_error` + `code()` | a `crnet_error` value |

## Blocking requests

```cpp
crnet::Request req("https://api.example.com/v1/items", "POST");
req.mode(CRNET_MODE_FETCH)
   .header("Content-Type", "application/json")
   .header("Referer", "https://www.example.com/app")
   .body(R"({"name":"x"})")
   .timeout_ms(10000);

try {
	crnet::Response r = client.perform(req);
	if (r.status() == 201)
		std::cout << r.header("location") << "\n";
} catch (const crnet::Error &e) {
	// e.code() is CRNET_E_NET, CRNET_E_TIMEOUT, ...; what() includes the
	// Chromium net error name, e.g. "network error (net::ERR_CONNECTION_RESET)"
}
```

`perform()` throws unless an HTTP response arrived; HTTP error statuses
are returned normally.

## Asynchronous requests

```cpp
crnet::Handlers h;
h.on_response = [](const crnet::ResponseView &r) {
	return r.status() == 200;               // false aborts
};
h.on_data = [&](std::string_view chunk) {
	file.write(chunk.data(), chunk.size());
	return true;                            // false aborts
};
h.on_complete = [](int err, const crnet::ResponseView &r) {
	if (err)
		std::cerr << crnet_strerror(err) << " " << r.error_string() << "\n";
};

crnet::Transfer t = client.start(crnet::Request(url), std::move(h));
// ...
t.wait();
```

The callbacks run on the client's network thread; the same rules as in
the [C API](api.md#asynchronous-requests) apply. The `Transfer` owns the
handlers. Destroying a still-running `Transfer` cancels it and waits for
`on_complete`, so anything the lambdas capture by reference only has to
outlive the `Transfer`.

Many concurrent requests:

```cpp
std::vector<crnet::Transfer> ts;
for (auto &url : urls)
	ts.push_back(client.start(crnet::Request(url)));
for (auto &t : ts) {
	t.wait();
	crnet::Response r = t.take_response();
	// ...
}
```

## WebSockets

```cpp
crnet::WebSocket ws = client.websocket("wss://example.com/chat", "chat");
ws.wait_open();                        // throws crnet::Error on failure
ws.send_text("hello");
ws.send_binary(std::string_view(buf, n));
while (auto m = ws.recv(5000))         // queue mode: no on_message
	std::cout << (m->is_text() ? m->data : "[binary]") << "\n";
ws.close(1000, "bye");
ws.wait_closed();
```

With handlers (network thread):

```cpp
crnet::WsHandlers h;
h.on_open = [](const crnet::ResponseView &resp, const std::string &proto,
	       const std::string &ext) { /* ... */ };
h.on_message = [](crnet_ws_msg_type type, std::string_view data) { /* ... */ };
h.on_close = [](int err, int code, const std::string &reason) { /* ... */ };
crnet::WebSocket ws = client.websocket(crnet::Request(url), "", std::move(h));
```

Pass a `crnet::Request` to set an `Origin`, extra headers, a handshake
timeout or a message size limit. See [websocket.md](websocket.md) for
the semantics.

## Mixing with the C API

`get()` on every wrapper returns the underlying C handle, so C-only
features such as the completion queue can be combined freely:

```cpp
crnet_queue *q = crnet_queue_new();
crnet_queue_start(q, client.get(), crnet::Request(url).get(), tag);
```

See `examples/parallel.cpp` for a complete program.
