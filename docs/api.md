# C API guide

Everything is declared in `<crnet/crnet.h>`, which documents each
function. This guide explains how the pieces fit together.

## Objects

| Handle | Created by | Freed by | Thread-safe | Notes |
|---|---|---|---|---|
| `crnet_client_config` | `crnet_client_config_new()` | `crnet_client_config_free()` | no | plain settings; may be freed right after `crnet_client_new()` |
| `crnet_client` | `crnet_client_new()` | `crnet_client_free()` | yes | one network thread, connection pools, cookie jar, proxy |
| `crnet_request` | `crnet_request_new()` | `crnet_request_free()` | no | plain description; copied when a transfer starts, so it is reusable |
| `crnet_transfer` | `crnet_start()`, `crnet_queue_pop()` | `crnet_transfer_free()` | yes | one running or finished execution of a request |
| `crnet_response` | `crnet_perform()`, `crnet_transfer_take_response()` | `crnet_response_free()` | read-only | status, headers, body, connection info |
| `crnet_queue` | `crnet_queue_new()` | `crnet_queue_free()` | yes | completion queue with a pollable fd |
| `crnet_ws` | `crnet_ws_connect()` | `crnet_ws_free()` | yes | a WebSocket connection, see [websocket.md](websocket.md) |

Create **few clients and share them**. A client is a full network
context (like a browser profile): it has its own thread, connection
pool, HTTP/2 sessions, DNS cache, TLS session cache and cookie jar.
Requests on the same client reuse connections; requests on different
clients never do.

## A blocking request

```c
#include <stdio.h>
#include <crnet/crnet.h>

int main(void)
{
	crnet_client *client = crnet_client_new(NULL);
	crnet_request *req = crnet_request_new("GET", "https://example.com/");
	crnet_response *resp = NULL;

	int rc = crnet_perform(client, req, &resp);
	if (rc == CRNET_OK) {
		size_t len;
		const char *body = crnet_response_body(resp, &len);
		printf("HTTP %d via %s, %zu bytes\n",
		       crnet_response_status(resp),
		       crnet_response_protocol(resp), len);
	} else {
		fprintf(stderr, "%s: %s\n", crnet_strerror(rc),
			crnet_response_error_string(resp));
	}

	crnet_response_free(resp);
	crnet_request_free(req);
	crnet_client_free(client);
	return rc != CRNET_OK;
}
```

`crnet_perform()` returns `CRNET_OK` whenever an HTTP response arrived,
**whatever its status code**: a 404 or 503 is a successful transfer.
Check `crnet_response_status()`.

On failure the response object is still returned (except for
`CRNET_E_INVAL`, `CRNET_E_NOMEM` and `CRNET_E_WOULD_DEADLOCK`) so the
details can be read:

```c
crnet_response_net_error(resp);     /* e.g. -105 */
crnet_response_error_string(resp);  /* "net::ERR_NAME_NOT_RESOLVED" */
```

Many threads may call `crnet_perform()` on the same client at once;
the requests run concurrently on the client's network thread.

## Configuring a client

```c
crnet_client_config *cfg = crnet_client_config_new();
crnet_client_config_set_proxy(cfg, "socks5h://127.0.0.1:1080");
crnet_client_config_set_accept_language(cfg, "id-ID,id;q=0.9,en;q=0.8");
crnet_client_config_set_timeout(cfg, 30000);            /* ms, per request */
crnet_client *client = crnet_client_new(cfg);
crnet_client_config_free(cfg);
```

| Setting | Default | Notes |
|---|---|---|
| `proxy` | direct | `socks5://`, `socks5h://` (same thing: SOCKS5 always resolves names on the proxy), `socks4://`, `http://[user:pass@]host:port`, `https://...`, `direct://`. `socks5://user:pass@host:port` enables SOCKS5 username/password authentication (RFC 1929); two clients that exist at the same time cannot use one SOCKS5 proxy with different credentials. |
| `proxy_bypass` | none | destinations sent direct, in Chrome's `--proxy-bypass-list` syntax: `*.example.com, 10.0.0.0/8, <local>`. Like Chrome, localhost, loopback and link-local destinations always go direct unless the list contains `<-loopback>`. |
| `platform_profile` | Linux | the operating system to claim: `CRNET_PLATFORM_LINUX`, `CRNET_PLATFORM_WINDOWS` or `CRNET_PLATFORM_MACOS`; sets the User-Agent and `sec-ch-ua-platform` together |
| `user_agent`, `sec_ch_ua`, `platform`, `accept_language` | the platform profile's, `en-US,en;q=0.9` | the browser identity, see [browser-emulation.md](browser-emulation.md); explicit values take precedence over the platform profile |
| `http2`, `quic` | on | QUIC is used for HTTP/3 when the server advertises it (Alt-Svc or DNS). Through a proxy Chromium never uses QUIC. |
| `secure_dns` | off | DNS over HTTPS with Chrome's semantics: `CRNET_SECURE_DNS_AUTOMATIC` uses the DoH templates once a probe shows they work and falls back to ordinary DNS, `CRNET_SECURE_DNS_SECURE` resolves only through them |
| `builtin_dns` | on | Chromium's own DNS client, as Chrome on Linux, including HTTPS records (ECH, HTTP/3 hints) |
| `cookies` | on | in-memory cookie jar shared by the client's requests |
| `insecure` | off | accept any certificate (testing only; the handshake itself is unchanged) |
| `trust_anchors` | none | extra roots (PEM, string or file) on top of Chrome's verification: the Chrome Root Store plus user-added roots of the system store. With `trust_anchors_only` they replace both, e.g. to accept only a private CA. |
| `host_rules` | none | Chrome's `--host-resolver-rules` syntax, e.g. `MAP api.example.com 10.0.0.5:8443` |
| `timeout` | none | default per-request timeout in ms |
| `connect_timeout` | none | default limit in ms on getting a connection ready (proxy resolution, DNS, TCP/QUIC, proxy handshake, TLS), per redirect hop; fails with `CRNET_E_TIMEOUT` and net error `ERR_CONNECTION_TIMED_OUT` (-118) instead of the request timeout's `ERR_TIMED_OUT` (-7) |
| `local_address` | the system's choice | source addresses for the client's connections, one per family (`192.0.2.10,2001:db8::10`): TCP and QUIC sockets to servers and proxies are bound before they connect. A family without an address fails with `ERR_ADDRESS_UNREACHABLE` (-109), so Chromium moves on to the server's addresses of the other family; `0.0.0.0` or `::` leaves a family to the system. Plain DNS queries are not bound. |
| `bind_interface` | the routing table's choice | a network interface (`eth1`, `wg0`) the same sockets are bound to with `SO_BINDTODEVICE`, so they leave through it whatever the routes say; combines with `local_address`. While the interface does not exist, connections fail with `ERR_ADDRESS_UNREACHABLE` (-109). Plain DNS queries are not bound (DoH requests are). Linux before 5.7 requires `CAP_NET_RAW`. |
| `client_cert` | none | a certificate and key (PEM) presented to servers that ask for one, over HTTP and WebSocket; without one crnet answers with none, as Chrome does when the user picks none |
| `state_dir` | none | a directory where the client keeps Chrome's per-profile network state across runs: Alt-Svc/HTTP/3 and HTTP/2 knowledge ("Network Persistent State") and HSTS ("TransportSecurity"); `crnet_client_free()` returns after saving it |
| `http_cache` | off | Chromium's HTTP cache in memory or on disk (`crnet_client_config_set_http_cache()`); requests pick a fetch()-style mode with `crnet_request_set_cache_mode()`, and `crnet_response_from_cache()` tells where a response came from. Responses accepted only through `insecure` are never cached. |
| `netlog` | none | write a Chromium NetLog JSON file (view it with the netlog-viewer) |

## Describing a request

```c
crnet_request *req = crnet_request_new("POST", "https://api.example.com/v1/items");
crnet_request_set_mode(req, CRNET_MODE_FETCH);           /* a page's fetch() */
crnet_request_set_header(req, "Content-Type", "application/json");
crnet_request_set_header(req, "Referer", "https://www.example.com/app");
crnet_request_set_body(req, json, strlen(json));
crnet_request_set_timeout(req, 10000);
crnet_request_set_connect_timeout(req, 3000);          /* overrides the client default */
```

- **Mode** selects which browser request crnet imitates:
  `CRNET_MODE_NAVIGATE` (default; typing a URL or submitting a form),
  `CRNET_MODE_FETCH` (`fetch()`/XHR from a page) or `CRNET_MODE_RAW` (no
  browser headers). See [browser-emulation.md](browser-emulation.md).
- **Headers** set with `crnet_request_set_header()` replace the browser
  default of the same name in place (keeping Chrome's position) or are
  added. Names are case-insensitive. A `NULL` value removes a header,
  including a browser default, e.g. `set_header(req, "sec-ch-ua", NULL)`.
- `Referer` is sent exactly as given. `Cookie` is merged with the
  cookie jar: your values first, then jar cookies with other names.
- The body is copied. `GET` with a body is allowed. Chromium sets
  `Content-Length`.
- `crnet_request_set_max_redirects(req, 0)` returns a 3xx response
  itself instead of following it. With the default (20) the final
  response is returned and `crnet_response_redirect_count()` tells how
  many hops were followed; `crnet_response_url()` is the final URL.
- `crnet_request_set_decompress(req, 0)` delivers the body as received
  (gzip/deflate/br/zstd stay encoded), except for a response carrying
  `Use-As-Dictionary`, which Chromium always decodes; see
  [Responses](#responses).
- `crnet_request_set_max_body_size()` aborts with `CRNET_E_TOO_LARGE`.
- `crnet_request_set_priority()` overrides the HTTP/2/3 priority.
- `crnet_request_set_proxy(req, "socks5h://10.0.0.7:1080")` sends this
  request (and its redirects) through another proxy than the client's,
  or direct with `direct://`, ignoring the bypass rules. Requests through
  different proxies never share a connection but do share the client's
  cookies and cache, so one client can spread its requests over many
  egress proxies. Within a client a proxy has one set of credentials
  (the client's, or the first request's that names some); a request
  naming other ones for it fails with `CRNET_E_INVAL`.
- `crnet_request_set_fresh_connection(req, 1)` gives the request (or
  WebSocket) connections of its own: it takes no pooled connection or
  HTTP/2/3 session, leaves none behind for others (they are closed when
  it is over) and resumes no earlier TLS session. Use it to retry on a
  new connection, or for a "no keep-alive" setting;
  `crnet_client_close_idle()` affects the whole client instead.

A request object is plain data and can be reused and modified after it
was started; each start copies it. Requests with `ws://`/`wss://` URLs
are only accepted by `crnet_ws_connect()` ([websocket.md](websocket.md));
`crnet_perform()` and `crnet_start()` reject them with `CRNET_E_INVAL`.

## Asynchronous requests

```c
static int on_response(crnet_transfer *t, const crnet_response *r, void *ud)
{
	printf("status %d\n", crnet_response_status(r));
	return 0;                       /* non-zero aborts */
}

static int on_data(crnet_transfer *t, const void *buf, size_t len, void *ud)
{
	fwrite(buf, 1, len, (FILE *)ud);
	return 0;                       /* non-zero aborts */
}

static void on_complete(crnet_transfer *t, int err, const crnet_response *r,
			void *ud)
{
	if (err)
		fprintf(stderr, "failed: %s\n", crnet_strerror(err));
}

crnet_callbacks cb = { on_response, on_data, on_complete };
crnet_transfer *t = crnet_start(client, req, &cb, out_file);
/* ... do other work ... */
crnet_transfer_wait(t, UINT64_MAX);
crnet_transfer_free(t);
```

Rules:

- Callbacks run **on the client's network thread**. Keep them short and
  never block in them. Blocking crnet calls (`crnet_perform`,
  `crnet_transfer_wait` on an unfinished transfer, the cookie helpers)
  return `CRNET_E_WOULD_DEADLOCK` there.
- `on_complete` is called **exactly once** for every started transfer:
  on success, error, timeout, cancellation and client shutdown.
- With `on_data` set, the body is streamed and **not** stored in the
  response. Without it, the body is accumulated.
- All callbacks are optional; `crnet_start(client, req, NULL, NULL)`
  plus `crnet_transfer_wait()` is a non-blocking start with a later
  join.
- `crnet_transfer_pause()`/`crnet_transfer_resume()` stop and restart
  reading the body (flow control for slow consumers). Pausing from
  `on_response` or `on_data` takes effect before the next read.
- `crnet_transfer_cancel()` is asynchronous and thread-safe;
  `on_complete` follows with `CRNET_E_CANCELED`.

### Transfer lifetime

The handle returned by `crnet_start()` holds a reference that you
release with `crnet_transfer_free()`:

- Called on another thread while the transfer is still running, it
  cancels it and **waits until `on_complete` has returned**. Your
  `user_data` can be freed right afterwards.
- Called inside `on_complete`, it just releases the handle (a common
  fire-and-forget pattern).
- The response returned by `crnet_transfer_response()` lives as long as
  the handle; `crnet_transfer_take_response()` moves it out to you.

`crnet_transfer_done()` and `crnet_transfer_wait()` report a transfer
finished only once `on_complete` has **returned**. When `on_complete`
hands the transfer to another thread (a work queue, an event loop),
that thread may run before the callback returns; there, use
`crnet_transfer_result()`, which returns the result from the moment
`on_complete` is **entered** and `CRNET_E_PENDING` before. From that
moment the response is final: crnet never modifies it again, so the
other thread may read it right away.

## Streaming bodies

Large uploads and downloads should not be held in memory. crnet streams
bodies in both directions, independently of the wire format. A body of
known size is sent with `Content-Length`. Only an open-ended body on
HTTP/1.1 uses `Transfer-Encoding: chunked`; on HTTP/2 and HTTP/3 it is
simply a sequence of DATA frames.

### Uploading a file

```c
crnet_request *req = crnet_request_new("PUT", "https://example.com/upload");
crnet_request_set_body_file(req, "/data/video.mp4", 0, UINT64_MAX);
crnet_perform(client, req, &resp);              /* or crnet_start() */
```

Chromium reads the file in the background while the upload proceeds.
Memory use does not depend on the file size. `offset`/`length` select a
slice, e.g. for resumable or ranged uploads. Because the file can be
read again, redirects that resend the body (307/308) and
authentication retries work.

### Writing the body yourself

```c
crnet_request_set_body_stream(req, size);        /* or CRNET_LENGTH_UNKNOWN */
crnet_transfer *t = crnet_start(client, req, NULL, NULL);

while ((n = produce(buf, sizeof(buf))) > 0)
	crnet_transfer_write(t, buf, n, NULL, UINT64_MAX);
crnet_transfer_write_end(t);

crnet_transfer_wait(t, UINT64_MAX);
```

- `crnet_transfer_write()` copies into a 1 MiB send buffer and blocks
  while it is full, so a fast producer is paced by the network. With a
  timeout (0 = never block) it returns `CRNET_E_TIMEOUT` and reports in
  `*written` how much it accepted, for event-loop style producers.
- `crnet_transfer_write_fd(t)` is readable while a write would not
  block, so an event loop can wait for room with `poll()`/`epoll` and
  then write with a timeout of 0:

  ```c
  struct pollfd p = { crnet_transfer_write_fd(t), POLLIN };
  /* in the loop, when poll() reports p readable: */
  crnet_transfer_write(t, buf + off, len - off, &w, 0);
  off += w;
  ```
- Writing may start before the transfer reached the network, from any
  thread.
- With a declared size, writing more fails with `CRNET_E_INVAL`, and
  ending early fails the transfer with `CRNET_E_INVAL`.
- If the transfer fails or the server answers without reading the
  whole body, blocked writers return with the transfer's error (or
  `CRNET_E_STATE`).
- Such a body is consumed once. A redirect or auth retry that must
  resend it fails with `net::ERR_UPLOAD_STREAM_REWIND_NOT_SUPPORTED`.

### Reading the response as it arrives

Pull style:

```c
crnet_request_set_response_stream(req, 1);
crnet_transfer *t = crnet_start(client, req, NULL, NULL);

if (crnet_transfer_wait_response(t, UINT64_MAX) == CRNET_OK) {
	const crnet_response *r = crnet_transfer_response(t);
	/* status and headers are available here */
	size_t n;
	while (crnet_transfer_read(t, buf, sizeof(buf), &n, UINT64_MAX) == CRNET_OK && n)
		consume(buf, n);
}
```

`crnet_transfer_read()` returns `CRNET_OK` with `n == 0` at the end of
the body, or the transfer's error once the data received before the
error has been read. crnet stops reading from the network while more
than 1 MiB is unread, so the server sees TCP flow control.
`crnet_transfer_fd()` is readable while a read would not block, for
`poll`/`epoll`.

Push style: set an `on_data` callback (see above) and use
`crnet_transfer_pause()`/`_resume()` for flow control.

Streamed requests need a transfer handle to write or read with, so
`crnet_perform()` and `crnet_queue_start()` reject them with
`CRNET_E_INVAL`. `build/examples/crget` streams both ways (`-T file`,
`-T -` for stdin, `-o file`).

## Progress

`crnet_transfer_progress(t, which)` reads a running transfer's byte
counts from any thread, without locking:

| `which` | Value |
|---|---|
| `CRNET_PROGRESS_UPLOAD_SENT` | request body bytes handed to the network (sampled every 100 ms while sending, like Chrome; exact once the response arrives) |
| `CRNET_PROGRESS_UPLOAD_TOTAL` | request body size; 0 without a body, -1 for a streamed body of unknown length |
| `CRNET_PROGRESS_DOWNLOAD_RECEIVED` | response body bytes as sent by the server (before decoding) |
| `CRNET_PROGRESS_DOWNLOAD_DECODED` | response body bytes delivered to you (after decoding) |
| `CRNET_PROGRESS_DOWNLOAD_TOTAL` | the response's `Content-Length`, -1 without one; compare it with `DOWNLOAD_RECEIVED` |

The values stay readable after the transfer finished.

To be told instead of polling, register a callback right after
`crnet_start()`:

```c
static void on_progress(crnet_transfer *t, void *ud)
{
	printf("%lld/%lld\n",
	       (long long)crnet_transfer_progress(t, CRNET_PROGRESS_DOWNLOAD_RECEIVED),
	       (long long)crnet_transfer_progress(t, CRNET_PROGRESS_DOWNLOAD_TOTAL));
}

crnet_transfer_set_progress_callback(t, on_progress, 100);   /* ms */
```

It runs on the network thread at most once per interval, only when a
value changed, and once more right before `on_complete`. In C++, set
`Handlers::on_progress` (and `progress_interval_ms`).

## Completion queue

A queue lets one application thread run many transfers and collect the
results without any callback running on its behalf. It exposes a file
descriptor for `poll`/`epoll`/libuv and similar event loops.

```c
crnet_queue *q = crnet_queue_new();
for (int i = 0; i < n; i++)
	crnet_queue_start(q, client, reqs[i], &jobs[i]);   /* user_data */

struct pollfd pfd = { crnet_queue_fd(q), POLLIN, 0 };
while (crnet_queue_pending(q) > 0) {
	poll(&pfd, 1, -1);
	crnet_transfer *t;
	void *ud;
	while (crnet_queue_pop(q, &t, &ud, 0) == CRNET_OK) {
		int rc = crnet_transfer_wait(t, 0);              /* its result */
		const crnet_response *r = crnet_transfer_response(t);
		handle((struct job *)ud, rc, r);
		crnet_transfer_free(t);
	}
}
crnet_queue_free(q);
```

- `crnet_queue_pop()` with a timeout of 0 polls, with `UINT64_MAX` it
  blocks. It returns `CRNET_E_TIMEOUT` if nothing finished in time and
  `CRNET_E_STATE` if nothing is pending at all.
- The fd is readable exactly while completed transfers are waiting.
- One queue may receive transfers from several clients.
- `crnet_queue_free()` cancels what is still running and waits for it.

### Waits as descriptors

An event loop that keeps the transfer handle (to cancel it, or to read
a streamed body) can wait on the transfer itself instead: every wait has
a descriptor that becomes readable when the wait would return at once,
and stays readable.

| Descriptor | Readable once | Wait |
|---|---|---|
| `crnet_transfer_response_fd(t)` | the headers arrived or the transfer failed | `crnet_transfer_wait_response()` |
| `crnet_transfer_done_fd(t)` | the transfer finished (`on_complete` returned) | `crnet_transfer_wait()` |
| `crnet_ws_open_fd(ws)` | the WebSocket opened or failed | `crnet_ws_wait_open()` |
| `crnet_ws_closed_fd(ws)` | the WebSocket closed | `crnet_ws_wait_closed()` |

Each is an eventfd made on the first call, owned by the handle: do not
read from it or close it. Together with `crnet_transfer_fd()`,
`crnet_transfer_write_fd()` and `crnet_ws_fd()` they let one thread
drive transfers and WebSockets without blocking and without callbacks.

## Cookies

The jar is in memory and per client. Besides what servers set, you can
manage it directly:

```c
crnet_client_set_cookie(client, "https://example.com/",
			"session=abc; Path=/; Secure; HttpOnly");
char *line = crnet_client_get_cookies(client, "https://example.com/");
/* "session=abc" */
crnet_free(line);
crnet_client_clear_cookies(client);
```

`crnet_client_get_cookies()` returns the `Cookie` header value the
client would send to that URL.

To keep a session across runs, save the jar and load it into the next
client:

```c
char *saved = crnet_client_export_cookies(client, CRNET_COOKIES_JSON);
/* ... write it to a file; next run: */
size_t n;
crnet_client_import_cookies(client, saved, CRNET_COOKIES_JSON, &n);
crnet_free(saved);
```

`CRNET_COOKIES_JSON` keeps every attribute Chromium stores (SameSite,
priority, CHIPS partition, creation and access times) and round-trips
exactly. `CRNET_COOKIES_NETSCAPE` is the `cookies.txt` format of curl
and wget, for exchanging cookies with them; it loses SameSite, priority
and the times other than expiry, and leaves partitioned cookies out.
Session cookies are exported too. An import is checked as a whole
first: a malformed entry or one a site could not set (such as a cookie
for `.com`) rejects all of it.

## Errors

| Code | Meaning |
|---|---|
| `CRNET_OK` | an HTTP response was received |
| `CRNET_E_INVAL` | invalid argument (bad URL, header name, proxy string, ...) |
| `CRNET_E_NOMEM` | allocation failed |
| `CRNET_E_NET` | network failure; see `crnet_response_net_error()` / `_error_string()` |
| `CRNET_E_TIMEOUT` | the request timeout expired (net error `ERR_TIMED_OUT`), or a wait expired |
| `CRNET_E_CANCELED` | canceled with `crnet_transfer_cancel()`/`_free()`/queue free |
| `CRNET_E_SHUTDOWN` | the client was freed while the transfer ran |
| `CRNET_E_TOO_LARGE` | the body exceeded `max_body_size` |
| `CRNET_E_WOULD_DEADLOCK` | a blocking call was made on a crnet network thread |
| `CRNET_E_STATE` | the operation does not apply in the current state |
| `CRNET_E_CALLBACK` | `on_response` or `on_data` returned non-zero |
| `CRNET_E_PENDING` | `crnet_transfer_result()`: the transfer is still running |

SOCKS5 proxy failures are told apart, unlike in Chrome: a refused
CONNECT reports its reply code as net error -191 (code 1) to -198
(code 8), also available as `crnet_response_socks_reply()`; -199 means
the proxy accepted no offered authentication method; -185 that it
rejected the username and password; a proxy closing
the connection during the handshake gives `ERR_CONNECTION_CLOSED`; a
malformed reply stays `ERR_SOCKS_CONNECTION_FAILED` (-120).

Network errors carry Chromium's net error codes (`net/base/net_error_list.h`),
for example `-105 ERR_NAME_NOT_RESOLVED`, `-102 ERR_CONNECTION_REFUSED`,
`-202 ERR_CERT_AUTHORITY_INVALID`, `-312 ERR_UNSAFE_PORT`. Chromium
refuses some ports outright (`ERR_UNSAFE_PORT`), as Chrome does.

## Responses

```c
crnet_response_status(r);           /* 200 */
crnet_response_status_text(r);      /* "OK" on HTTP/1.1, "" on h2/h3 */
crnet_response_header(r, "content-type");    /* first value, any case */
for (size_t i = 0; i < crnet_response_header_count(r); i++) {
	const char *name, *value;           /* in received order */
	crnet_response_header_at(r, i, &name, &value);
}
crnet_response_body(r, &len);       /* NUL-terminated for convenience */
crnet_response_protocol(r);         /* "h2", "http/1.1", "h3" */
crnet_response_remote_address(r);   /* "104.20.36.129:443" or the proxy */
crnet_response_via_proxy(r);
crnet_response_url(r);              /* after redirects */
crnet_response_redirect_count(r);
```

Repeated headers (`Set-Cookie`, ...) appear once per line when iterating.

By default crnet decodes the body (br, deflate, gzip, zstd, like
Chrome), but the headers stay as the server sent them:
`Content-Encoding` and `Content-Length` then describe the encoded bytes,
not the body you get. `crnet_response_content_decoded(r)` tells which
case you have, and `crnet_response_decoded_codings(r)` lists the codings
removed (`"gzip"`, `"gzip, br"`). Both report what Chromium actually
did: it leaves the whole body raw when `Content-Encoding` names a coding
it does not know (`identity` included), and it decodes a response with
`Use-As-Dictionary` even when decompression is off.

Each response also says which connection carried it and how long each
phase took, from Chromium's `LoadTimingInfo` for the final hop:

```c
crnet_response_socket_reused(r);    /* keep-alive or shared h2/h3 session */
crnet_response_connection_id(r);    /* equal ids = same connection */
/* microseconds since crnet_start(), -1 = phase did not happen */
int64_t ttfb = crnet_response_timing(r, CRNET_TIMING_HEADERS_START) -
	       crnet_response_timing(r, CRNET_TIMING_SEND_START);
```

The phases are `REQUEST_START` (the final hop; later than 0 after
redirects), `PROXY_RESOLVE_*`, `DNS_*`, `CONNECT_*` (which includes the
TLS handshake), `TLS_*`, `SEND_*`, `HEADERS_*` and `COMPLETE`. A reused
connection has no DNS, connect or TLS phase.

## Versions and feature tests

`CRNET_VERSION_MAJOR`, `CRNET_VERSION_MINOR` and `CRNET_VERSION_PATCH`
describe the headers a program is compiled against, and
`CRNET_VERSION_NUMBER` combines them for comparisons (0.2.0 is 200,
1.10.3 is 11003). `crnet_version()` reports the library actually
loaded, for example `"0.2.0 (Chromium 155.0.8059.39)"`.

Every capability gets a `CRNET_HAS_*` macro, defined to 1 next to its
declarations in `crnet.h`, and adding one bumps the minor version. Test
the macro, not the version, to decide at compile time whether a
feature can be used:

```c
#ifdef CRNET_HAS_WEBSOCKET
	ws = crnet_ws_connect(client, req, NULL, &cb, NULL);
#else
	/* fall back, or refuse the option */
#endif
```

| Macro | Since | Covers |
|---|---|---|
| `CRNET_HAS_STREAMING` | 0.2.0 | streamed request and response bodies |
| `CRNET_HAS_WEBSOCKET` | 0.2.0 | the WebSocket client, `crnet_ws_*` |
| `CRNET_HAS_TRANSFER_RESULT` | 0.3.0 | `crnet_transfer_result()`, `CRNET_E_PENDING` |
| `CRNET_HAS_PROXY_BYPASS` | 0.4.0 | `crnet_client_config_set_proxy_bypass()` |
| `CRNET_HAS_CONTENT_DECODED` | 0.5.0 | `crnet_response_content_decoded()`, `crnet_response_decoded_codings()` |
| `CRNET_HAS_TIMING` | 0.6.0 | `crnet_response_socket_reused()`, `_connection_id()`, `_timing()` |
| `CRNET_HAS_PROGRESS` | 0.7.0 | `crnet_transfer_progress()` |
| `CRNET_HAS_CONNECT_TIMEOUT` | 0.8.0 | `crnet_client_config_set_connect_timeout()`, `crnet_request_set_connect_timeout()` |
| `CRNET_HAS_WRITE_FD` | 0.9.0 | `crnet_transfer_write_fd()` |
| `CRNET_HAS_TRUST_ANCHORS` | 0.10.0 | `crnet_client_config_add_trust_anchors()`, `_add_trust_anchors_file()`, `_set_trust_anchors_only()` |
| `CRNET_HAS_COOKIE_JAR_IO` | 0.11.0 | `crnet_client_export_cookies()`, `crnet_client_import_cookies()` |
| `CRNET_HAS_HTTP_CACHE` | 0.12.0 | `crnet_client_config_set_http_cache()`, `crnet_request_set_cache_mode()`, `crnet_response_from_cache()` |
| `CRNET_HAS_CLIENT_CERT` | 0.13.0 | `crnet_client_config_set_client_cert()`, `_set_client_cert_file()` |
| `CRNET_HAS_SECURE_DNS` | 0.14.0 | `crnet_client_config_set_secure_dns()` |
| `CRNET_HAS_STATE_DIR` | 0.15.0 | `crnet_client_config_set_state_dir()` |
| `CRNET_HAS_WS_IDLE_TIMEOUT` | 0.16.0 | `crnet_ws_set_idle_timeout()` |
| `CRNET_HAS_SOCKS_ERRORS` | 0.17.0 | distinct SOCKS5 net errors, `crnet_response_socks_reply()` |
| `CRNET_HAS_WS_COMPRESSION_SWITCH` | 0.18.0 | `crnet_request_set_ws_compression()` |
| `CRNET_HAS_PROGRESS_CALLBACK` | 0.19.0 | `crnet_transfer_set_progress_callback()` |
| `CRNET_HAS_WS_PING` | 0.20.0 | `crnet_ws_ping()`, `crnet_ws_set_pong_callback()`, `crnet_ws_set_auto_ping()` |
| `CRNET_HAS_SOCKS5_AUTH` | 0.21.0 | `socks5://user:pass@...` proxy URLs |
| `CRNET_HAS_PLATFORM_PROFILE` | 0.22.0 | `crnet_client_config_set_platform_profile()`, `crnet_platform_user_agent()` |
| `CRNET_HAS_LOCAL_ADDRESS` | 0.23.0 | `crnet_client_config_set_local_address()` |
| `CRNET_HAS_BIND_INTERFACE` | 0.24.0 | `crnet_client_config_set_bind_interface()` |
| `CRNET_HAS_WAIT_FDS` | 0.25.0 | `crnet_transfer_response_fd()`, `_done_fd()`, `crnet_ws_open_fd()`, `_closed_fd()` |
| `CRNET_HAS_REQUEST_PROXY` | 0.26.0 | `crnet_request_set_proxy()` |
| `CRNET_HAS_FRESH_CONNECTION` | 0.27.0 | `crnet_request_set_fresh_connection()` |

Releases before 0.2.0 define no `CRNET_HAS_*` macro. The soname stays
`libcrnet.so.0` while the major version is 0, so a program built
against newer headers than the installed library fails to load with an
undefined `crnet_*` symbol rather than misbehaving.

## Process-wide state

Chromium needs some process-wide singletons. crnet creates them on the
first `crnet_client_new()` and keeps them for the life of the process.
They can't be torn down and re-created. Call `crnet_global_init()` first
only to choose Chromium feature overrides. Fork without exec after
creating a client isn't supported.
