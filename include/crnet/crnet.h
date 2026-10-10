/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * crnet - a network client library built on the Chromium network stack.
 * It currently provides an HTTP(S) client (HTTP/1.1, HTTP/2, HTTP/3).
 *
 * Requests made through crnet go through Chromium's own //net code
 * (BoringSSL, HTTP/2, QUIC, connection pooling, certificate
 * verification against the Chrome Root Store), so on the wire they look
 * like requests made by the Chrome release crnet was built from.
 *
 * Object model
 * ------------
 *   crnet_client    A network context: one network thread, a connection
 *                    pool, a cookie jar and a proxy setting. A single
 *                    client runs any number of requests concurrently.
 *                    Thread-safe. Create few, share them.
 *   crnet_request   A plain description of a request (method, URL,
 *                    headers, body, options). Owned by the caller, not
 *                    thread-safe, reusable: it is copied when a transfer
 *                    starts.
 *   crnet_transfer  One in-flight (or finished) execution of a request.
 *   crnet_response  Status, headers and (unless streamed) the body.
 *
 * Two ways to run a request:
 *   - crnet_perform(): blocks the calling thread until done.
 *   - crnet_start():   returns immediately; callbacks run on the
 *                       client's network thread.
 *
 * Browser emulation
 * -----------------
 * Unless disabled, crnet fills in the request headers Chrome itself
 * would send for the chosen crnet_mode (sec-ch-ua*, sec-fetch-*,
 * user-agent, accept, accept-encoding, accept-language, priority) in
 * Chrome's order. Headers set with crnet_request_set_header() replace
 * the default in place, or are added if Chrome would not send one.
 *
 * Errors
 * ------
 * Functions returning int return CRNET_OK (0) or a negative
 * crnet_error. Network failures carry the Chromium net error as well,
 * see crnet_response_net_error().
 */
#ifndef CRNET_CRNET_H
#define CRNET_CRNET_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define CRNET_API __declspec(dllimport)
#elif defined(__GNUC__)
#  define CRNET_API __attribute__((visibility("default")))
#else
#  define CRNET_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Version and feature tests
 * -------------------------
 * CRNET_VERSION_* describe the headers being compiled against;
 * crnet_version() reports the library loaded at run time. The minor
 * version goes up whenever a capability is added, and each capability
 * gets a CRNET_HAS_* macro next to its declarations, so one program can
 * build against several crnet releases:
 *
 *	#ifdef CRNET_HAS_WEBSOCKET
 *		...
 *	#endif
 *
 * A CRNET_HAS_* macro is either defined to 1 or not defined at all.
 * Releases before 0.2.0 define none of them.
 */
#define CRNET_VERSION_MAJOR 0
#define CRNET_VERSION_MINOR 4
#define CRNET_VERSION_PATCH 0
/* 0.2.0 is 200, 1.10.3 is 11003: compare with >=. */
#define CRNET_VERSION_NUMBER \
	(CRNET_VERSION_MAJOR * 10000 + CRNET_VERSION_MINOR * 100 + \
	 CRNET_VERSION_PATCH)

typedef struct crnet_client_config crnet_client_config;
typedef struct crnet_client crnet_client;
typedef struct crnet_request crnet_request;
typedef struct crnet_transfer crnet_transfer;
typedef struct crnet_response crnet_response;

typedef enum crnet_error {
	CRNET_OK		=  0,
	CRNET_E_INVAL		= -1,	/* invalid argument */
	CRNET_E_NOMEM		= -2,	/* out of memory */
	CRNET_E_NET		= -3,	/* network error, see net_error */
	CRNET_E_TIMEOUT	= -4,	/* request timeout expired */
	CRNET_E_CANCELED	= -5,	/* canceled by the caller */
	CRNET_E_SHUTDOWN	= -6,	/* the client was destroyed */
	CRNET_E_TOO_LARGE	= -7,	/* body exceeded max_body_size */
	CRNET_E_WOULD_DEADLOCK	= -8,	/* blocking call on network thread */
	CRNET_E_STATE		= -9,	/* operation invalid in this state */
	CRNET_E_CALLBACK	= -10,	/* a callback asked to abort */
	CRNET_E_PENDING		= -11,	/* the transfer is still running */
} crnet_error;

/*
 * Which kind of browser request to imitate. Controls the default
 * header set and the request priority.
 */
typedef enum crnet_mode {
	/* Top-level document navigation (typing a URL). Default. */
	CRNET_MODE_NAVIGATE	= 0,
	/* fetch()/XHR issued by a page (sec-fetch-mode: cors). */
	CRNET_MODE_FETCH	= 1,
	/* No browser headers; only what //net adds on its own. */
	CRNET_MODE_RAW		= 2,
} crnet_mode;

/* Mirrors Chromium's net::RequestPriority. */
typedef enum crnet_priority {
	CRNET_PRIORITY_DEFAULT	= -1,	/* derived from the mode */
	CRNET_PRIORITY_THROTTLED = 0,
	CRNET_PRIORITY_IDLE	= 1,
	CRNET_PRIORITY_LOWEST	= 2,
	CRNET_PRIORITY_LOW	= 3,
	CRNET_PRIORITY_MEDIUM	= 4,
	CRNET_PRIORITY_HIGHEST	= 5,
} crnet_priority;

/* ---------------------------------------------------------------- */
/* Library                                                          */
/* ---------------------------------------------------------------- */

/* "0.4.0 (Chromium 155.0.8059.39)". Static storage. */
CRNET_API const char *crnet_version(void);

/* Chromium version crnet was built from, e.g. "155.0.8059.39". */
CRNET_API const char *crnet_chromium_version(void);

/* Human readable description of a crnet_error. Static storage. */
CRNET_API const char *crnet_strerror(int err);

/*
 * Default User-Agent / sec-ch-ua values for this build, e.g.
 * "Mozilla/5.0 (X11; Linux x86_64) ... Chrome/155.0.0.0 Safari/537.36".
 * Static storage.
 */
CRNET_API const char *crnet_default_user_agent(void);
CRNET_API const char *crnet_default_sec_ch_ua(void);

/*
 * Process-wide initialisation happens implicitly on the first
 * crnet_client_new(). Calling this first is only needed to choose
 * Chromium feature overrides (comma separated feature names, as with
 * Chrome's --enable-features / --disable-features). Either may be NULL.
 * The environment variables CRNET_ENABLE_FEATURES and
 * CRNET_DISABLE_FEATURES are used when this is never called.
 *
 * Returns CRNET_E_STATE if the library is already initialised.
 * Chromium cannot be torn down and re-initialised within a process.
 */
CRNET_API int crnet_global_init(const char *enable_features,
				const char *disable_features);

/* ---------------------------------------------------------------- */
/* Client configuration                                             */
/* ---------------------------------------------------------------- */

CRNET_API crnet_client_config *crnet_client_config_new(void);
CRNET_API void crnet_client_config_free(crnet_client_config *cfg);

/*
 * Proxy for every request: "socks5://host:port", "socks5h://...",
 * "socks4://...", "http://host:port", "https://host:port", or
 * "direct://". SOCKS5 always resolves host names on the proxy (socks5
 * and socks5h are equivalent). Credentials in an http(s) proxy URL
 * ("http://user:pass@host:port") answer proxy auth challenges; SOCKS
 * authentication is not supported by Chromium. NULL means direct.
 */
CRNET_API int crnet_client_config_set_proxy(crnet_client_config *cfg,
					    const char *proxy_url);

/*
 * Destinations that bypass the proxy, in Chrome's --proxy-bypass-list
 * syntax: rules separated by ',' or ';', each a host pattern with an
 * optional scheme and port ("*.example.com", "http://intranet:8080",
 * ".internal"), an IP literal or CIDR block ("10.0.0.0/8", "[::1]"),
 * "<local>" (host names without a dot) or "<-loopback>".
 *
 * Like Chrome, crnet always sends requests to localhost, *.localhost,
 * loopback and link-local addresses (127/8, ::1, 169.254/16,
 * fe80::/10) direct, even with a proxy and no bypass rules.
 * "<-loopback>" removes that implicit bypass, so those destinations go
 * through the proxy too.
 *
 * NULL or "" means no rules. CRNET_E_INVAL if any rule is malformed.
 */
#define CRNET_HAS_PROXY_BYPASS 1
CRNET_API int crnet_client_config_set_proxy_bypass(crnet_client_config *cfg,
						   const char *rules);

/* Override the browser identity. NULL restores the build default. */
CRNET_API int crnet_client_config_set_user_agent(crnet_client_config *cfg,
						 const char *ua);
CRNET_API int crnet_client_config_set_sec_ch_ua(crnet_client_config *cfg,
						const char *sec_ch_ua);
/* Value of sec-ch-ua-platform without quotes, default "Linux". */
CRNET_API int crnet_client_config_set_platform(crnet_client_config *cfg,
					       const char *platform);
/* Default "en-US,en;q=0.9". */
CRNET_API int crnet_client_config_set_accept_language(
		crnet_client_config *cfg, const char *accept_language);

/* Protocol switches. HTTP/2 and QUIC (HTTP/3) default on, as in Chrome. */
CRNET_API void crnet_client_config_set_http2(crnet_client_config *cfg,
					     int enable);
CRNET_API void crnet_client_config_set_quic(crnet_client_config *cfg,
					    int enable);

/*
 * Use Chromium's built-in DNS client (as Chrome does on Linux) so that
 * HTTPS/SVCB records are queried, enabling ECH and HTTP/3 discovery.
 * Default on. Off falls back to getaddrinfo(). No effect when a proxy
 * resolves names.
 */
CRNET_API void crnet_client_config_set_builtin_dns(crnet_client_config *cfg,
						   int enable);

/* In-memory cookie jar shared by all requests of the client. Default on. */
CRNET_API void crnet_client_config_set_cookies(crnet_client_config *cfg,
					       int enable);

/*
 * Accept any server certificate. For testing only: the TLS handshake
 * itself is unchanged, but the connection is no longer authenticated.
 */
CRNET_API void crnet_client_config_set_insecure(crnet_client_config *cfg,
						int enable);

/*
 * Host resolver rules, same syntax as Chrome's --host-resolver-rules,
 * e.g. "MAP example.com 127.0.0.1:8443, EXCLUDE localhost".
 */
CRNET_API int crnet_client_config_set_host_rules(crnet_client_config *cfg,
						 const char *rules);

/* Default per-request timeout in milliseconds, 0 = none (default). */
CRNET_API void crnet_client_config_set_timeout(crnet_client_config *cfg,
					       uint64_t timeout_ms);

/*
 * Write a Chromium NetLog (JSON, viewable with netlog-viewer) to |path|
 * for the lifetime of the client. Debugging aid. NULL disables.
 */
CRNET_API int crnet_client_config_set_netlog(crnet_client_config *cfg,
					     const char *path);

/* ---------------------------------------------------------------- */
/* Client                                                           */
/* ---------------------------------------------------------------- */

/*
 * Create a client. |cfg| may be NULL for defaults and may be freed
 * right after this returns. Does not wait for the network context to be
 * set up; requests may be started immediately. Returns NULL if the
 * network thread cannot be started.
 */
CRNET_API crnet_client *crnet_client_new(const crnet_client_config *cfg);

/*
 * Cancel every unfinished transfer (their on_complete runs with
 * CRNET_E_SHUTDOWN), wait for them, and destroy the client. Transfer
 * handles still held by the caller stay valid and must still be freed.
 * Must not be called from a callback of this client.
 */
CRNET_API void crnet_client_free(crnet_client *client);

/*
 * Cookie jar access (no-ops returning CRNET_E_STATE when cookies are
 * disabled). |set_cookie| is a Set-Cookie header value applied as if it
 * were received from |url|.
 */
CRNET_API int crnet_client_set_cookie(crnet_client *client, const char *url,
				      const char *set_cookie);
/*
 * Cookie header value the client would send to |url|. The returned
 * string is malloc()ed and must be released with crnet_free(). It is
 * "" when there are no cookies.
 */
CRNET_API char *crnet_client_get_cookies(crnet_client *client,
					 const char *url);
CRNET_API int crnet_client_clear_cookies(crnet_client *client);

/* Close idle connections in the pool. */
CRNET_API void crnet_client_close_idle(crnet_client *client);

/* Release memory returned by crnet. */
CRNET_API void crnet_free(void *ptr);

/* ---------------------------------------------------------------- */
/* Request                                                          */
/* ---------------------------------------------------------------- */

/* |method| NULL means "GET". Returns NULL if |url| is not valid. */
CRNET_API crnet_request *crnet_request_new(const char *method,
					      const char *url);
CRNET_API void crnet_request_free(crnet_request *req);

CRNET_API int crnet_request_set_url(crnet_request *req, const char *url);
CRNET_API int crnet_request_set_method(crnet_request *req,
				       const char *method);

/*
 * Set a header, replacing any value of the same name (case-insensitive)
 * including a browser default. A NULL |value| removes the header, which
 * also suppresses the browser default of that name.
 *
 * "Referer" is honoured as given. "Cookie" is sent in addition to
 * matching cookies from the client's jar.
 */
CRNET_API int crnet_request_set_header(crnet_request *req,
				       const char *name, const char *value);

/*
 * Request body. The three ways to provide one are exclusive; the last
 * call wins. All of them send Content-Length when the size is known.
 */

/* Copy |len| bytes as the request body. NULL/0 clears the body. */
CRNET_API int crnet_request_set_body(crnet_request *req, const void *data,
				     size_t len);

/*
 * Streamed bodies: crnet_request_set_body_file(),
 * crnet_request_set_body_stream(), crnet_request_set_response_stream()
 * and the crnet_transfer_write()/read() family.
 */
#define CRNET_HAS_STREAMING 1

/*
 * Stream the body from a file, without loading it into memory: Chromium
 * reads it in the background as the upload proceeds. |length| bytes
 * starting at |offset|; UINT64_MAX means up to the end of the file. The
 * file is opened when the transfer starts (a missing file fails it with
 * net::ERR_FILE_NOT_FOUND). Rewindable: redirects that resend the body
 * (307/308) and authentication retries work.
 */
CRNET_API int crnet_request_set_body_file(crnet_request *req,
					  const char *path, uint64_t offset,
					  uint64_t length);

#define CRNET_LENGTH_UNKNOWN (-1)

/*
 * Stream the body from the application: after starting the transfer,
 * feed it with crnet_transfer_write() and finish with
 * crnet_transfer_write_end(). With a known |length| the request carries
 * Content-Length and exactly that many bytes must be written. With
 * CRNET_LENGTH_UNKNOWN the size is open-ended: HTTP/1.1 then uses
 * Transfer-Encoding: chunked, HTTP/2 and HTTP/3 simply send DATA frames
 * until the end.
 *
 * Such a body is consumed once: a redirect or authentication retry that
 * must resend it fails with net::ERR_UPLOAD_STREAM_REWIND_NOT_SUPPORTED.
 * Only crnet_start() accepts these requests.
 */
CRNET_API int crnet_request_set_body_stream(crnet_request *req,
					    int64_t length);

/*
 * Deliver the response body through crnet_transfer_read() (pull) instead
 * of accumulating it in the response. Reading from the network pauses
 * while the application has more than 1 MiB unread, so arbitrarily large
 * bodies stream in constant memory. Ignored when an on_data callback is
 * set (that is the push-style equivalent). Only crnet_start() accepts
 * these requests.
 */
CRNET_API int crnet_request_set_response_stream(crnet_request *req,
						int enable);

CRNET_API int crnet_request_set_mode(crnet_request *req, crnet_mode mode);
CRNET_API int crnet_request_set_priority(crnet_request *req,
					 crnet_priority priority);

/*
 * Maximum redirects to follow, default 20 (Chromium's limit). 0 returns
 * the 3xx response itself without following it.
 */
CRNET_API int crnet_request_set_max_redirects(crnet_request *req,
					      int max_redirects);

/* Overrides the client default; 0 = no timeout. */
CRNET_API int crnet_request_set_timeout(crnet_request *req,
					uint64_t timeout_ms);

/*
 * Decode gzip/deflate/br/zstd bodies. Default on. When off, the body is
 * delivered exactly as received (Content-Encoding still applies).
 */
CRNET_API int crnet_request_set_decompress(crnet_request *req, int enable);

/* Fail with CRNET_E_TOO_LARGE past this many body bytes. 0 = no limit. */
CRNET_API int crnet_request_set_max_body_size(crnet_request *req,
					      uint64_t max_bytes);

/* ---------------------------------------------------------------- */
/* Blocking API                                                     */
/* ---------------------------------------------------------------- */

/*
 * Run |req| on |client| and wait for the complete response.
 *
 * Returns CRNET_OK when an HTTP response was received, whatever its
 * status code. On any other result *|resp| is still set when possible so
 * the error details can be inspected; it is NULL only for
 * CRNET_E_INVAL, CRNET_E_NOMEM and CRNET_E_WOULD_DEADLOCK. Free it
 * with crnet_response_free().
 *
 * Safe to call from many threads at once on the same client. Must not
 * be called from a crnet callback.
 */
CRNET_API int crnet_perform(crnet_client *client, const crnet_request *req,
			    crnet_response **resp);

/* ---------------------------------------------------------------- */
/* Asynchronous API                                                 */
/* ---------------------------------------------------------------- */

/*
 * All callbacks run on the client's network thread. They must return
 * quickly and must not call blocking crnet functions.
 */
typedef struct crnet_callbacks {
	/*
	 * Optional. Response headers are available (after redirects). The
	 * response object is the one later passed to on_complete; its body
	 * is still empty. Return non-zero to abort (CRNET_E_CALLBACK).
	 */
	int (*on_response)(crnet_transfer *t, const crnet_response *resp,
			   void *user_data);

	/*
	 * Optional. Body bytes as they arrive. When set the body is NOT
	 * accumulated in the response. Return non-zero to abort.
	 */
	int (*on_data)(crnet_transfer *t, const void *data, size_t len,
		       void *user_data);

	/*
	 * Optional. Called exactly once when the transfer ends for any
	 * reason, including cancellation and client shutdown. |error| is as
	 * for crnet_perform(). |resp| is owned by the transfer and is final:
	 * crnet does not modify it once on_complete has been entered, so it
	 * may be handed to another thread from here.
	 */
	void (*on_complete)(crnet_transfer *t, int error,
			    const crnet_response *resp, void *user_data);
} crnet_callbacks;

/*
 * Start |req| on |client|. |cb| may be NULL (use crnet_transfer_wait).
 * The returned handle holds a reference that must be released with
 * crnet_transfer_free(). Returns NULL on invalid arguments or if the
 * client is shutting down.
 */
CRNET_API crnet_transfer *crnet_start(crnet_client *client,
					 const crnet_request *req,
					 const crnet_callbacks *cb,
					 void *user_data);

/*
 * Ask the transfer to stop. Asynchronous and thread-safe; on_complete
 * follows with CRNET_E_CANCELED unless the transfer already finished.
 */
CRNET_API void crnet_transfer_cancel(crnet_transfer *t);

/*
 * Stop / resume reading the response body (flow control for on_data).
 * Thread-safe.
 */
CRNET_API void crnet_transfer_pause(crnet_transfer *t);
CRNET_API void crnet_transfer_resume(crnet_transfer *t);

/*
 * Wait until the transfer completes, at most |timeout_ms| milliseconds
 * (UINT64_MAX = forever). Returns the transfer result, or
 * CRNET_E_TIMEOUT if still running when the wait expires (the
 * transfer itself continues). Must not be called on the network thread.
 */
CRNET_API int crnet_transfer_wait(crnet_transfer *t, uint64_t timeout_ms);

/* Non-zero once on_complete has returned. */
CRNET_API int crnet_transfer_done(const crnet_transfer *t);

/*
 * The transfer's result without waiting: the value on_complete receives,
 * available from the moment on_complete is entered (unlike
 * crnet_transfer_wait() and crnet_transfer_done(), which only see the
 * transfer finished once on_complete has returned), or CRNET_E_PENDING
 * while it is still running. Thread-safe, also from inside on_complete.
 * Once it returns anything but CRNET_E_PENDING the response is final and
 * may be read from any thread.
 */
#define CRNET_HAS_TRANSFER_RESULT 1
CRNET_API int crnet_transfer_result(const crnet_transfer *t);

/*
 * The transfer's response; NULL before on_response would be invoked
 * (and after errors that happened before any response). Valid until
 * crnet_transfer_free(). Other threads may read it once
 * crnet_transfer_result() returns anything but CRNET_E_PENDING (its
 * status and headers already after crnet_transfer_wait_response()).
 */
CRNET_API const crnet_response *crnet_transfer_response(
		const crnet_transfer *t);

/*
 * Detach the response from the transfer; the caller then owns it and
 * frees it with crnet_response_free(). Only after completion.
 */
CRNET_API crnet_response *crnet_transfer_take_response(crnet_transfer *t);

/*
 * Wait until the response headers have arrived (CRNET_OK; status and
 * headers of crnet_transfer_response() may then be read from any
 * thread) or the transfer failed before that (its error). Returns
 * CRNET_E_TIMEOUT if neither happened within |timeout_ms|.
 */
CRNET_API int crnet_transfer_wait_response(crnet_transfer *t,
					   uint64_t timeout_ms);

/*
 * Streaming upload (crnet_request_set_body_stream()): append |len| bytes
 * to the request body. Thread-safe. Blocks while the transfer's 1 MiB
 * send buffer is full, at most |timeout_ms| (0 = never block). *written
 * (may be NULL) receives how much was accepted. Returns CRNET_OK once
 * all |len| bytes are accepted, CRNET_E_TIMEOUT if the wait expired
 * first, CRNET_E_INVAL if a declared length would be exceeded,
 * CRNET_E_STATE if the transfer has no streamed body, its end was
 * already written or it finished (or the transfer's error if it failed).
 */
CRNET_API int crnet_transfer_write(crnet_transfer *t, const void *data,
				   size_t len, size_t *written,
				   uint64_t timeout_ms);

/*
 * End a streamed request body. With a declared length, fewer bytes than
 * declared fail the transfer with CRNET_E_INVAL.
 */
CRNET_API int crnet_transfer_write_end(crnet_transfer *t);

/*
 * Streaming download (crnet_request_set_response_stream()): read up to
 * |len| body bytes, waiting at most |timeout_ms| for some to arrive.
 * Returns CRNET_OK with *nread > 0 for data, CRNET_OK with *nread == 0 at
 * the end of the body, CRNET_E_TIMEOUT when nothing arrived in time, or
 * the transfer's error once the data received before it was read.
 * Thread-safe; blocking reads are not allowed on a network thread.
 */
CRNET_API int crnet_transfer_read(crnet_transfer *t, void *buf, size_t len,
				  size_t *nread, uint64_t timeout_ms);

/*
 * Streaming download: a file descriptor that is readable while
 * crnet_transfer_read() would not block (data buffered, or the transfer
 * finished). -1 for other transfers.
 */
CRNET_API int crnet_transfer_fd(const crnet_transfer *t);

/*
 * Release the handle. If the transfer is still running it is canceled
 * and, unless called from one of this transfer's callbacks, this waits
 * until on_complete has run, so user_data may be freed afterwards.
 * From inside on_complete it simply releases the handle.
 */
CRNET_API void crnet_transfer_free(crnet_transfer *t);

/* ---------------------------------------------------------------- */
/* Completion queue                                                 */
/* ---------------------------------------------------------------- */

/*
 * A completion queue lets an application run many transfers
 * concurrently and collect them, fully buffered, on its own thread: no
 * callback ever runs on the application's behalf. The queue exposes a
 * file descriptor that becomes readable while completed transfers are
 * pending, for use with poll/epoll/libuv/etc. One queue may receive
 * transfers from several clients. Thread-safe.
 */
typedef struct crnet_queue crnet_queue;

CRNET_API crnet_queue *crnet_queue_new(void);

/*
 * Destroy the queue. Transfers still running are canceled and waited
 * for; completed-but-unclaimed transfers are freed.
 */
CRNET_API void crnet_queue_free(crnet_queue *q);

/* Readable while crnet_queue_pop() would return a transfer. */
CRNET_API int crnet_queue_fd(const crnet_queue *q);

/*
 * Start |req| on |client|; when it completes it is appended to |q|.
 * |user_data| is handed back by crnet_queue_pop(). Returns
 * CRNET_OK or a negative crnet_error.
 */
CRNET_API int crnet_queue_start(crnet_queue *q, crnet_client *client,
				const crnet_request *req, void *user_data);

/*
 * Pop one completed transfer, waiting up to |timeout_ms| (0 = poll,
 * UINT64_MAX = forever). On success *|t| owns the transfer (inspect it
 * with crnet_transfer_response()/crnet_transfer_take_response(), its
 * result with crnet_transfer_wait(t, 0), then crnet_transfer_free()).
 * Returns CRNET_OK, CRNET_E_TIMEOUT when nothing completed in time,
 * or CRNET_E_STATE when no transfer is pending or running at all.
 */
CRNET_API int crnet_queue_pop(crnet_queue *q, crnet_transfer **t,
			      void **user_data, uint64_t timeout_ms);

/* Transfers started on |q| and not yet popped (running + completed). */
CRNET_API size_t crnet_queue_pending(const crnet_queue *q);

/* ---------------------------------------------------------------- */
/* WebSocket                                                        */
/* ---------------------------------------------------------------- */

/* The WebSocket client, crnet_ws_*. */
#define CRNET_HAS_WEBSOCKET 1

/*
 * A WebSocket connection (RFC 6455) made through Chromium's WebSocket
 * implementation: the opening handshake (headers in Chrome's order,
 * cookies, proxies, TLS), permessage-deflate compression and the
 * closing handshake behave exactly as in Chrome. WebSockets over
 * HTTP/2 (RFC 8441) are used when the server supports them.
 *
 * Messages are delivered whole (fragmented frames are reassembled),
 * either to the on_message callback or, when it is NULL, to an internal
 * queue read with crnet_ws_recv(), which also offers a pollable fd.
 */
typedef struct crnet_ws crnet_ws;

typedef enum crnet_ws_msg_type {
	CRNET_WS_TEXT	= 1,	/* UTF-8 text */
	CRNET_WS_BINARY	= 2,
} crnet_ws_msg_type;

typedef enum crnet_ws_state {
	CRNET_WS_CONNECTING	= 0,
	CRNET_WS_OPEN		= 1,
	CRNET_WS_CLOSING	= 2,
	CRNET_WS_CLOSED		= 3,
} crnet_ws_state;

/*
 * All callbacks run on the client's network thread, must return quickly
 * and must not call blocking crnet functions.
 */
typedef struct crnet_ws_callbacks {
	/*
	 * Optional. The opening handshake succeeded. |handshake| is the
	 * server's 101 response; |protocol| the selected subprotocol and
	 * |extensions| the negotiated extensions ("" when none).
	 */
	void (*on_open)(crnet_ws *ws, const crnet_response *handshake,
			const char *protocol, const char *extensions,
			void *user_data);

	/*
	 * Optional. One complete message. |data| is only valid during the
	 * call. Text messages are valid UTF-8 (Chromium fails the
	 * connection otherwise). When NULL, messages are queued for
	 * crnet_ws_recv().
	 */
	void (*on_message)(crnet_ws *ws, crnet_ws_msg_type type,
			   const void *data, size_t len, void *user_data);

	/*
	 * Optional. Called exactly once when the connection is over, for any
	 * reason. |error| is CRNET_OK after a clean closing handshake (either
	 * side may have started it), otherwise the failure (CRNET_E_NET,
	 * CRNET_E_TIMEOUT, CRNET_E_CANCELED, CRNET_E_SHUTDOWN,
	 * CRNET_E_TOO_LARGE). |code| and |reason| are the close code and
	 * reason (1006 and a description for failures).
	 */
	void (*on_close)(crnet_ws *ws, int error, int code, const char *reason,
			 void *user_data);
} crnet_ws_callbacks;

/*
 * Open a WebSocket. |req| supplies the URL (ws://, wss://; http(s):// is
 * accepted and mapped), extra headers and options:
 *   - "Origin" sets the handshake Origin; by default it is the origin of
 *     the URL itself (http(s) for ws(s)), as for a same-origin page.
 *   - "User-Agent" overrides the client's; NULL values remove headers
 *     as for HTTP requests; "Cookie" is merged with the jar.
 *   - The request timeout bounds the opening handshake.
 *   - max_body_size limits the size of one received message
 *     (CRNET_E_TOO_LARGE).
 * Mode, priority, redirects and decompression do not apply.
 * |protocols| is a comma separated list of subprotocols or NULL.
 *
 * |cb| may be NULL. Returns a handle (to release with crnet_ws_free())
 * or NULL on invalid arguments or client shutdown.
 */
CRNET_API crnet_ws *crnet_ws_connect(crnet_client *client,
				     const crnet_request *req,
				     const char *protocols,
				     const crnet_ws_callbacks *cb,
				     void *user_data);

/*
 * Wait until the connection is open (CRNET_OK) or failed (the close
 * error). CRNET_E_TIMEOUT if neither happened within |timeout_ms|
 * (UINT64_MAX = forever). Not on the network thread.
 */
CRNET_API int crnet_ws_wait_open(crnet_ws *ws, uint64_t timeout_ms);

/*
 * Queue a message (copied). Thread-safe. CRNET_E_STATE unless the
 * connection is open; CRNET_E_INVAL for a TEXT message that is not
 * valid UTF-8.
 */
CRNET_API int crnet_ws_send(crnet_ws *ws, crnet_ws_msg_type type,
			    const void *data, size_t len);

/*
 * Bytes passed to crnet_ws_send() that have not been handed to the
 * operating system yet (like the browser's bufferedAmount).
 */
CRNET_API uint64_t crnet_ws_buffered_amount(const crnet_ws *ws);

/*
 * Receive the next queued message (only when on_message is NULL).
 * *data is malloc()ed and NUL-terminated; release it with crnet_free().
 * Returns CRNET_OK, CRNET_E_TIMEOUT, or, once the connection is closed
 * and the queue is empty, CRNET_E_STATE. Not on the network thread.
 * Reading from the network pauses while too much (4 MiB) is queued.
 */
CRNET_API int crnet_ws_recv(crnet_ws *ws, crnet_ws_msg_type *type,
			    void **data, size_t *len, uint64_t timeout_ms);

/*
 * Readable while crnet_ws_recv() would not block (a message is queued,
 * or the connection is closed). For poll/epoll integration.
 */
CRNET_API int crnet_ws_fd(const crnet_ws *ws);

/*
 * Start the closing handshake. |code| is 1000 or 3000-4999; |reason|
 * may be NULL and must be valid UTF-8 of at most 123 bytes. Closing a
 * connection that is still connecting aborts it (CRNET_E_CANCELED).
 * Thread-safe; on_close follows.
 */
CRNET_API int crnet_ws_close(crnet_ws *ws, int code, const char *reason);

/* Stop / resume reading messages from the network. Thread-safe. */
CRNET_API void crnet_ws_pause(crnet_ws *ws);
CRNET_API void crnet_ws_resume(crnet_ws *ws);

/*
 * Wait until the connection is closed; returns the close error (see
 * on_close). CRNET_E_TIMEOUT if still open after |timeout_ms|.
 */
CRNET_API int crnet_ws_wait_closed(crnet_ws *ws, uint64_t timeout_ms);

CRNET_API crnet_ws_state crnet_ws_get_state(const crnet_ws *ws);

/* Valid once open: selected subprotocol and negotiated extensions. */
CRNET_API const char *crnet_ws_protocol(const crnet_ws *ws);
CRNET_API const char *crnet_ws_extensions(const crnet_ws *ws);

/*
 * The handshake response: the 101 response once open; on a failed
 * handshake its status (e.g. 403) is set when the server answered.
 * NULL before that.
 */
CRNET_API const crnet_response *crnet_ws_handshake_response(
		const crnet_ws *ws);

/*
 * Once closed: close code and reason, and the Chromium net error of a
 * failure (0 otherwise).
 */
CRNET_API int crnet_ws_close_code(const crnet_ws *ws);
CRNET_API const char *crnet_ws_close_reason(const crnet_ws *ws);
CRNET_API int crnet_ws_net_error(const crnet_ws *ws);

/*
 * Release the handle. A connection that is not closed yet is aborted
 * without a closing handshake (on_close gets CRNET_E_CANCELED); for a
 * clean shutdown call crnet_ws_close() and crnet_ws_wait_closed() first.
 * Waits for on_close as crnet_transfer_free() does.
 */
CRNET_API void crnet_ws_free(crnet_ws *ws);

/* ---------------------------------------------------------------- */
/* Response                                                         */
/* ---------------------------------------------------------------- */

CRNET_API void crnet_response_free(crnet_response *resp);

/* HTTP status code, 0 if no response was received. */
CRNET_API int crnet_response_status(const crnet_response *resp);
/* Reason phrase ("OK"); "" for HTTP/2 and HTTP/3. */
CRNET_API const char *crnet_response_status_text(const crnet_response *resp);

/* Final URL after redirects. */
CRNET_API const char *crnet_response_url(const crnet_response *resp);
/* Number of redirects that were followed. */
CRNET_API int crnet_response_redirect_count(const crnet_response *resp);

/* Header lines in received order; names as sent by the server. */
CRNET_API size_t crnet_response_header_count(const crnet_response *resp);
CRNET_API int crnet_response_header_at(const crnet_response *resp,
				       size_t index, const char **name,
					 const char **value);
/*
 * First value of header |name| (case-insensitive) or NULL. Use
 * crnet_response_header_at() for repeated headers such as Set-Cookie.
 */
CRNET_API const char *crnet_response_header(const crnet_response *resp,
					    const char *name);

/* Accumulated body (NULL-terminated for convenience; *len excludes it). */
CRNET_API const char *crnet_response_body(const crnet_response *resp,
					  size_t *len);

/* Negotiated protocol: "h2", "http/1.1", "h3", ... */
CRNET_API const char *crnet_response_protocol(const crnet_response *resp);
/* "ip:port" of the server (or proxy) actually connected to. */
CRNET_API const char *crnet_response_remote_address(
		const crnet_response *resp);
/* Whether the response was obtained through a proxy. */
CRNET_API int crnet_response_via_proxy(const crnet_response *resp);

/*
 * Chromium net error code (0 = OK, e.g. -105 = ERR_NAME_NOT_RESOLVED)
 * and its name ("net::ERR_NAME_NOT_RESOLVED", "" when OK).
 */
CRNET_API int crnet_response_net_error(const crnet_response *resp);
CRNET_API const char *crnet_response_error_string(
		const crnet_response *resp);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CRNET_CRNET_H */
