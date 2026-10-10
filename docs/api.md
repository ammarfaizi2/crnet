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
| `proxy` | direct | `socks5://`, `socks5h://` (same thing: SOCKS5 always resolves names on the proxy), `socks4://`, `http://[user:pass@]host:port`, `https://...`, `direct://`. Chromium supports no SOCKS authentication. |
| `user_agent`, `sec_ch_ua`, `platform`, `accept_language` | Chrome on Linux, `en-US,en;q=0.9` | the browser identity, see [browser-emulation.md](browser-emulation.md) |
| `http2`, `quic` | on | QUIC is used for HTTP/3 when the server advertises it (Alt-Svc or DNS). Through a proxy Chromium never uses QUIC. |
| `builtin_dns` | on | Chromium's own DNS client, as Chrome on Linux, including HTTPS records (ECH, HTTP/3 hints) |
| `cookies` | on | in-memory cookie jar shared by the client's requests |
| `insecure` | off | accept any certificate (testing only; the handshake itself is unchanged) |
| `host_rules` | none | Chrome's `--host-resolver-rules` syntax, e.g. `MAP api.example.com 10.0.0.5:8443` |
| `timeout` | none | default per-request timeout in ms |
| `netlog` | none | write a Chromium NetLog JSON file (view it with the netlog-viewer) |

## Describing a request

```c
crnet_request *req = crnet_request_new("POST", "https://api.example.com/v1/items");
crnet_request_set_mode(req, CRNET_MODE_FETCH);           /* a page's fetch() */
crnet_request_set_header(req, "Content-Type", "application/json");
crnet_request_set_header(req, "Referer", "https://www.example.com/app");
crnet_request_set_body(req, json, strlen(json));
crnet_request_set_timeout(req, 10000);
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
- `crnet_request_set_decompress(req, 0)` delivers the body exactly as
  received (gzip/deflate/br/zstd stay encoded).
- `crnet_request_set_max_body_size()` aborts with `CRNET_E_TOO_LARGE`.
- `crnet_request_set_priority()` overrides the HTTP/2/3 priority.

A request object is plain data and can be reused and modified after it
was started; each start copies it.

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

## Process-wide state

Chromium needs some process-wide singletons. crnet creates them on the
first `crnet_client_new()` and keeps them for the life of the process.
They can't be torn down and re-created. Call `crnet_global_init()` first
only to choose Chromium feature overrides. Fork without exec after
creating a client isn't supported.
