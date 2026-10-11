# WebSockets

crnet opens WebSockets (RFC 6455) through Chromium's own implementation
(`net::WebSocketChannel`), driven like Chrome's network service drives
it. Everything below the API is Chrome's:

- the opening handshake: the same headers in the same order, cookies
  from and to the client's jar, proxies, TLS with Chrome's fingerprint,
- `permessage-deflate` (offered as Chrome does,
  `permessage-deflate; client_max_window_bits`; turn the offer off with
  `crnet_request_set_ws_compression(req, 0)`, at the cost of a
  handshake that differs from Chrome's),
- WebSockets over HTTP/2 (RFC 8441) when the server enables extended
  CONNECT on an existing HTTP/2 connection,
- automatic Pong replies, UTF-8 validation of text messages, the
  closing handshake and its timeouts.

A WebSocket uses the same `crnet_client` as HTTP requests, so it shares
the connection pools, the cookie jar, the proxy setting and the
network thread.

## Quick start

```c
crnet_request *req = crnet_request_new(NULL, "wss://example.com/chat");
crnet_ws *ws = crnet_ws_connect(client, req, "chat, superchat", NULL, NULL);
crnet_request_free(req);

if (crnet_ws_wait_open(ws, 10000) == CRNET_OK) {
	crnet_ws_send(ws, CRNET_WS_TEXT, "hello", 5);

	crnet_ws_msg_type type;
	void *data;
	size_t len;
	if (crnet_ws_recv(ws, &type, &data, &len, 5000) == CRNET_OK) {
		printf("%.*s\n", (int)len, (char *)data);
		crnet_free(data);
	}
	crnet_ws_close(ws, 1000, "bye");
	crnet_ws_wait_closed(ws, 5000);
}
crnet_ws_free(ws);
```

```cpp
crnet::WebSocket ws = client.websocket("wss://example.com/chat", "chat");
ws.wait_open();
ws.send_text("hello");
if (auto m = ws.recv(5000))
	std::cout << m->data << "\n";
ws.close(1000, "bye");
ws.wait_closed();
```

`build/examples/crws` is a small interactive client (stdin to messages):

```bash
build/examples/crws wss://ws.postman-echo.com/raw
```

## Opening

`crnet_ws_connect(client, req, protocols, callbacks, user_data)` returns
at once; the connection opens on the network thread. The request object
supplies:

| From the request | Meaning for a WebSocket |
|---|---|
| URL | `ws://` or `wss://`. `http://`/`https://` is mapped to them. |
| `Origin` header | The handshake `Origin`. Default: the URL's own origin (`https://host` for `wss://host`), like a same-origin page. Browsers always send one; many servers check it. |
| `User-Agent` header | Overrides the client's. |
| `Cookie` header | Merged with the jar's cookies for that URL. |
| `Sec-WebSocket-Protocol` header | Used as the subprotocol list when `protocols` is NULL. |
| other headers | Sent as given (e.g. `Authorization`). A `NULL` value removes a header, including the ones `//net` adds (e.g. `Accept-Language`). |
| timeout | Bounds the opening handshake (`CRNET_E_TIMEOUT`). |
| `max_body_size` | Largest message accepted. A bigger one fails the connection with `CRNET_E_TOO_LARGE` (close code 1009). |

`protocols` is a comma-separated list of subprotocols ("chat, superchat").
After opening, `crnet_ws_protocol()` returns the one the server picked.
A server that picks one that wasn't offered fails the handshake.

`crnet_ws_wait_open()` blocks until the connection is open (`CRNET_OK`)
or has failed (the failure code). `on_open` reports the same thing to
callback users, together with the 101 response
(`crnet_ws_handshake_response()`).

## Sending

`crnet_ws_send(ws, CRNET_WS_TEXT or CRNET_WS_BINARY, data, len)` copies
the message and queues it; it is thread-safe and never blocks.

- Text must be valid UTF-8 (`CRNET_E_INVAL` otherwise).
- Only an open connection accepts messages (`CRNET_E_STATE` while
  connecting, closing or closed). Wait for `on_open`/`wait_open` first.
- `crnet_ws_buffered_amount()` is the number of bytes queued but not yet
  handed to the operating system, like a browser's `bufferedAmount`.
  Producers that may outrun the network should watch it.

## Receiving

Messages are always delivered whole; fragmented frames are reassembled.

**Callback mode**: set `on_message`. It runs on the network thread for
every message. The data is valid only during the call.

**Queue mode**: leave `on_message` NULL. Messages are queued and read
with `crnet_ws_recv(ws, &type, &data, &len, timeout_ms)`. The data is a
`malloc()`ed, NUL-terminated copy that you free with `crnet_free()`.
`crnet_ws_fd()` is readable while `crnet_ws_recv()` would not block (a
message is queued or the connection is closed), so a socket fits into
`poll`/`epoll`/libuv loops. See `examples/crws.c`. `crnet_ws_open_fd()`
and `crnet_ws_closed_fd()` do the same for `crnet_ws_wait_open()` and
`crnet_ws_wait_closed()`, so such a loop need not block while the
handshake or the closing handshake runs.

`crnet_ws_recv()` returns `CRNET_E_STATE` once the connection is closed
and every queued message has been read.

### Flow control

- In queue mode, crnet stops reading from the network while more than
  4 MiB are queued and resumes when you have read some. The server sees
  TCP backpressure.
- `crnet_ws_pause()`/`crnet_ws_resume()` do the same explicitly. Messages
  already in flight when you pause are held back and delivered on
  resume, so `on_message` is not called while paused.

### Idle timeout

`crnet_ws_set_idle_timeout(ws, ms)` drops a connection on which no
message arrived for `ms` milliseconds, without a closing handshake
(the peer is presumed gone); `on_close` reports `CRNET_E_TIMEOUT`, code
1006 and `ERR_TIMED_OUT`. Every received message or pong restarts it, and it
waits while the application is not taking messages (paused, or a full
receive queue). The server's pings do not count: Chromium answers them
on its own without reporting them. 0 turns it off; it may be changed at
any time.

### Pings

`crnet_ws_ping(ws, data, len)` sends a Ping frame (up to 125 bytes),
and `crnet_ws_set_pong_callback()` reports every Pong frame with its
payload. `crnet_ws_set_auto_ping(ws, ms)` pings at a fixed interval;
with an idle timeout somewhat longer than that interval, a connection
whose peer stopped answering is dropped even when no messages are
expected. Chrome offers pages none of this; it changes nothing about
the handshake.

## Closing

```c
crnet_ws_close(ws, 1000, "bye");      /* 1000 or 3000-4999, reason <= 123 bytes */
int rc = crnet_ws_wait_closed(ws, 5000);
```

`on_close` (or `crnet_ws_wait_closed()`) reports exactly once how the
connection ended:

| `error` | `code` / `reason` | When |
|---|---|---|
| `CRNET_OK` | the peer's close code and reason (1005 if it sent none) | a clean closing handshake, started by either side |
| `CRNET_E_NET` | 1006 and a description | the connection failed: TCP drop, protocol violation, refused or invalid handshake, proxy failure |
| `CRNET_E_TIMEOUT` | 1006 | the opening handshake did not finish in time |
| `CRNET_E_TOO_LARGE` | 1009 | a message exceeded `max_body_size` |
| `CRNET_E_CANCELED` | 1006 | `crnet_ws_free()` on an open socket, or `crnet_ws_close()` while connecting |
| `CRNET_E_SHUTDOWN` | 1006 | the client was freed |

`crnet_ws_close_code()`, `crnet_ws_close_reason()` and
`crnet_ws_net_error()` return the same information afterwards. For
failures `crnet_ws_net_error()` gives the Chromium error, e.g.
`ERR_WS_PROTOCOL_ERROR` (-145) for a protocol violation such as invalid
UTF-8, `ERR_INVALID_RESPONSE` (-320) for a refused handshake (the
status, e.g. 403, is in `crnet_ws_handshake_response()`), or
`ERR_TIMED_OUT`.

As in Chrome, **messages that arrive after you called `crnet_ws_close()`
are discarded**. If you expect replies, receive them before closing.
When the server starts closing, crnet delivers everything it sent first
and then completes the handshake.

`crnet_ws_free()` releases the handle. On a socket that is still open
it aborts the connection without a closing handshake and waits for
`on_close`, the same rules as `crnet_transfer_free()`. Call
`crnet_ws_close()` and `crnet_ws_wait_closed()` first for a clean
shutdown.

## Threads

Every function except the blocking ones (`wait_open`, `wait_closed`,
`recv` with a timeout) may be called from any thread, including from
callbacks. The blocking ones return `CRNET_E_WOULD_DEADLOCK` on a crnet
network thread. Callbacks run on the client's network thread and must
not block.

## What Chrome sends

Captured from Chrome 155 (`tests/fingerprint/capture_chrome.py`) and
reproduced by crnet; `tests/test_websocket.cc` checks it:

```
GET /chat HTTP/1.1
Host: example.com
Connection: Upgrade
Pragma: no-cache
Cache-Control: no-cache
User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36
Upgrade: websocket
Origin: https://example.com
Sec-WebSocket-Version: 13
Accept-Encoding: gzip, deflate, br, zstd
Accept-Language: en-US,en;q=0.9
Cookie: ...
Sec-WebSocket-Key: ...
Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits
Sec-WebSocket-Protocol: chat, superchat
```

Chrome sends no client hints (`sec-ch-ua*`) and no fetch metadata
(`sec-fetch-*`) on WebSocket handshakes, and crnet doesn't either.
Everything except `User-Agent` is generated by `//net` itself. Chrome's
renderer passes `User-Agent` as the only extra header, which is why it
precedes `Upgrade`, and crnet does the same.
