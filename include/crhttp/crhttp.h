/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * crhttp - an HTTP client library built on the Chromium network stack.
 *
 * Requests made through crhttp go through Chromium's own //net code
 * (BoringSSL, HTTP/2, QUIC, connection pooling, certificate
 * verification against the Chrome Root Store), so on the wire they look
 * like requests made by the Chrome release crhttp was built from.
 *
 * Object model
 * ------------
 *   crhttp_client    A network context: one network thread, a connection
 *                    pool, a cookie jar and a proxy setting. A single
 *                    client runs any number of requests concurrently.
 *                    Thread-safe. Create few, share them.
 *   crhttp_request   A plain description of a request (method, URL,
 *                    headers, body, options). Owned by the caller, not
 *                    thread-safe, reusable: it is copied when a transfer
 *                    starts.
 *   crhttp_transfer  One in-flight (or finished) execution of a request.
 *   crhttp_response  Status, headers and (unless streamed) the body.
 *
 * Two ways to run a request:
 *   - crhttp_perform(): blocks the calling thread until done.
 *   - crhttp_start():   returns immediately; callbacks run on the
 *                       client's network thread.
 *
 * Browser emulation
 * -----------------
 * Unless disabled, crhttp fills in the request headers Chrome itself
 * would send for the chosen crhttp_mode (sec-ch-ua*, sec-fetch-*,
 * user-agent, accept, accept-encoding, accept-language, priority) in
 * Chrome's order. Headers set with crhttp_request_set_header() replace
 * the default in place, or are added if Chrome would not send one.
 *
 * Errors
 * ------
 * Functions returning int return CRHTTP_OK (0) or a negative
 * crhttp_error. Network failures carry the Chromium net error as well,
 * see crhttp_response_net_error().
 */
#ifndef CRHTTP_CRHTTP_H
#define CRHTTP_CRHTTP_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define CRHTTP_API __declspec(dllimport)
#elif defined(__GNUC__)
#  define CRHTTP_API __attribute__((visibility("default")))
#else
#  define CRHTTP_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CRHTTP_VERSION_MAJOR 0
#define CRHTTP_VERSION_MINOR 1
#define CRHTTP_VERSION_PATCH 0

typedef struct crhttp_client_config crhttp_client_config;
typedef struct crhttp_client crhttp_client;
typedef struct crhttp_request crhttp_request;
typedef struct crhttp_transfer crhttp_transfer;
typedef struct crhttp_response crhttp_response;

typedef enum crhttp_error {
	CRHTTP_OK		=  0,
	CRHTTP_E_INVAL		= -1,	/* invalid argument */
	CRHTTP_E_NOMEM		= -2,	/* out of memory */
	CRHTTP_E_NET		= -3,	/* network error, see net_error */
	CRHTTP_E_TIMEOUT	= -4,	/* request timeout expired */
	CRHTTP_E_CANCELED	= -5,	/* canceled by the caller */
	CRHTTP_E_SHUTDOWN	= -6,	/* the client was destroyed */
	CRHTTP_E_TOO_LARGE	= -7,	/* body exceeded max_body_size */
	CRHTTP_E_WOULD_DEADLOCK	= -8,	/* blocking call on network thread */
	CRHTTP_E_STATE		= -9,	/* operation invalid in this state */
	CRHTTP_E_CALLBACK	= -10,	/* a callback asked to abort */
} crhttp_error;

/*
 * Which kind of browser request to imitate. Controls the default
 * header set and the request priority.
 */
typedef enum crhttp_mode {
	/* Top-level document navigation (typing a URL). Default. */
	CRHTTP_MODE_NAVIGATE	= 0,
	/* fetch()/XHR issued by a page (sec-fetch-mode: cors). */
	CRHTTP_MODE_FETCH	= 1,
	/* No browser headers; only what //net adds on its own. */
	CRHTTP_MODE_RAW		= 2,
} crhttp_mode;

/* Mirrors Chromium's net::RequestPriority. */
typedef enum crhttp_priority {
	CRHTTP_PRIORITY_DEFAULT	= -1,	/* derived from the mode */
	CRHTTP_PRIORITY_THROTTLED = 0,
	CRHTTP_PRIORITY_IDLE	= 1,
	CRHTTP_PRIORITY_LOWEST	= 2,
	CRHTTP_PRIORITY_LOW	= 3,
	CRHTTP_PRIORITY_MEDIUM	= 4,
	CRHTTP_PRIORITY_HIGHEST	= 5,
} crhttp_priority;

/* ---------------------------------------------------------------- */
/* Library                                                          */
/* ---------------------------------------------------------------- */

/* "0.1.0 (Chromium 155.0.8059.39)". Static storage. */
CRHTTP_API const char *crhttp_version(void);

/* Chromium version crhttp was built from, e.g. "155.0.8059.39". */
CRHTTP_API const char *crhttp_chromium_version(void);

/* Human readable description of a crhttp_error. Static storage. */
CRHTTP_API const char *crhttp_strerror(int err);

/*
 * Default User-Agent / sec-ch-ua values for this build, e.g.
 * "Mozilla/5.0 (X11; Linux x86_64) ... Chrome/155.0.0.0 Safari/537.36".
 * Static storage.
 */
CRHTTP_API const char *crhttp_default_user_agent(void);
CRHTTP_API const char *crhttp_default_sec_ch_ua(void);

/*
 * Process-wide initialisation happens implicitly on the first
 * crhttp_client_new(). Calling this first is only needed to choose
 * Chromium feature overrides (comma separated feature names, as with
 * Chrome's --enable-features / --disable-features). Either may be NULL.
 * The environment variables CRHTTP_ENABLE_FEATURES and
 * CRHTTP_DISABLE_FEATURES are used when this is never called.
 *
 * Returns CRHTTP_E_STATE if the library is already initialised.
 * Chromium cannot be torn down and re-initialised within a process.
 */
CRHTTP_API int crhttp_global_init(const char *enable_features,
				  const char *disable_features);

/* ---------------------------------------------------------------- */
/* Client configuration                                             */
/* ---------------------------------------------------------------- */

CRHTTP_API crhttp_client_config *crhttp_client_config_new(void);
CRHTTP_API void crhttp_client_config_free(crhttp_client_config *cfg);

/*
 * Proxy for every request: "socks5://host:port", "socks5h://...",
 * "socks4://...", "http://host:port", "https://host:port", or
 * "direct://". SOCKS5 always resolves host names on the proxy (socks5
 * and socks5h are equivalent). Credentials in an http(s) proxy URL
 * ("http://user:pass@host:port") answer proxy auth challenges; SOCKS
 * authentication is not supported by Chromium. NULL means direct.
 */
CRHTTP_API int crhttp_client_config_set_proxy(crhttp_client_config *cfg,
					      const char *proxy_url);

/* Override the browser identity. NULL restores the build default. */
CRHTTP_API int crhttp_client_config_set_user_agent(crhttp_client_config *cfg,
						   const char *ua);
CRHTTP_API int crhttp_client_config_set_sec_ch_ua(crhttp_client_config *cfg,
						  const char *sec_ch_ua);
/* Value of sec-ch-ua-platform without quotes, default "Linux". */
CRHTTP_API int crhttp_client_config_set_platform(crhttp_client_config *cfg,
						 const char *platform);
/* Default "en-US,en;q=0.9". */
CRHTTP_API int crhttp_client_config_set_accept_language(
		crhttp_client_config *cfg, const char *accept_language);

/* Protocol switches. HTTP/2 and QUIC (HTTP/3) default on, as in Chrome. */
CRHTTP_API void crhttp_client_config_set_http2(crhttp_client_config *cfg,
					       int enable);
CRHTTP_API void crhttp_client_config_set_quic(crhttp_client_config *cfg,
					      int enable);

/*
 * Use Chromium's built-in DNS client (as Chrome does on Linux) so that
 * HTTPS/SVCB records are queried, enabling ECH and HTTP/3 discovery.
 * Default on. Off falls back to getaddrinfo(). No effect when a proxy
 * resolves names.
 */
CRHTTP_API void crhttp_client_config_set_builtin_dns(crhttp_client_config *cfg,
						     int enable);

/* In-memory cookie jar shared by all requests of the client. Default on. */
CRHTTP_API void crhttp_client_config_set_cookies(crhttp_client_config *cfg,
						 int enable);

/*
 * Accept any server certificate. For testing only: the TLS handshake
 * itself is unchanged, but the connection is no longer authenticated.
 */
CRHTTP_API void crhttp_client_config_set_insecure(crhttp_client_config *cfg,
						  int enable);

/*
 * Host resolver rules, same syntax as Chrome's --host-resolver-rules,
 * e.g. "MAP example.com 127.0.0.1:8443, EXCLUDE localhost".
 */
CRHTTP_API int crhttp_client_config_set_host_rules(crhttp_client_config *cfg,
						   const char *rules);

/* Default per-request timeout in milliseconds, 0 = none (default). */
CRHTTP_API void crhttp_client_config_set_timeout(crhttp_client_config *cfg,
						 uint64_t timeout_ms);

/*
 * Write a Chromium NetLog (JSON, viewable with netlog-viewer) to |path|
 * for the lifetime of the client. Debugging aid. NULL disables.
 */
CRHTTP_API int crhttp_client_config_set_netlog(crhttp_client_config *cfg,
					       const char *path);

/* ---------------------------------------------------------------- */
/* Client                                                           */
/* ---------------------------------------------------------------- */

/*
 * Create a client. |cfg| may be NULL for defaults and may be freed
 * right after this returns. Returns NULL on failure.
 */
CRHTTP_API crhttp_client *crhttp_client_new(const crhttp_client_config *cfg);

/*
 * Cancel every unfinished transfer (their on_complete runs with
 * CRHTTP_E_SHUTDOWN), wait for them, and destroy the client. Transfer
 * handles still held by the caller stay valid and must still be freed.
 * Must not be called from a callback of this client.
 */
CRHTTP_API void crhttp_client_free(crhttp_client *client);

/*
 * Cookie jar access (no-ops returning CRHTTP_E_STATE when cookies are
 * disabled). |set_cookie| is a Set-Cookie header value applied as if it
 * were received from |url|.
 */
CRHTTP_API int crhttp_client_set_cookie(crhttp_client *client, const char *url,
					const char *set_cookie);
/*
 * Cookie header value the client would send to |url|. The returned
 * string is malloc()ed and must be released with crhttp_free(). It is
 * "" when there are no cookies.
 */
CRHTTP_API char *crhttp_client_get_cookies(crhttp_client *client,
					   const char *url);
CRHTTP_API int crhttp_client_clear_cookies(crhttp_client *client);

/* Close idle connections in the pool. */
CRHTTP_API void crhttp_client_close_idle(crhttp_client *client);

/* Release memory returned by crhttp. */
CRHTTP_API void crhttp_free(void *ptr);

/* ---------------------------------------------------------------- */
/* Request                                                          */
/* ---------------------------------------------------------------- */

/* |method| NULL means "GET". Returns NULL if |url| is not valid. */
CRHTTP_API crhttp_request *crhttp_request_new(const char *method,
					      const char *url);
CRHTTP_API void crhttp_request_free(crhttp_request *req);

CRHTTP_API int crhttp_request_set_url(crhttp_request *req, const char *url);
CRHTTP_API int crhttp_request_set_method(crhttp_request *req,
					 const char *method);

/*
 * Set a header, replacing any value of the same name (case-insensitive)
 * including a browser default. A NULL |value| removes the header, which
 * also suppresses the browser default of that name.
 *
 * "Referer" is honoured as given. "Cookie" is sent in addition to
 * matching cookies from the client's jar.
 */
CRHTTP_API int crhttp_request_set_header(crhttp_request *req,
					 const char *name, const char *value);

/* Copy |len| bytes as the request body. NULL/0 clears the body. */
CRHTTP_API int crhttp_request_set_body(crhttp_request *req, const void *data,
				       size_t len);

CRHTTP_API int crhttp_request_set_mode(crhttp_request *req, crhttp_mode mode);
CRHTTP_API int crhttp_request_set_priority(crhttp_request *req,
					   crhttp_priority priority);

/*
 * Maximum redirects to follow, default 20 (Chromium's limit). 0 returns
 * the 3xx response itself without following it.
 */
CRHTTP_API int crhttp_request_set_max_redirects(crhttp_request *req,
						int max_redirects);

/* Overrides the client default; 0 = no timeout. */
CRHTTP_API int crhttp_request_set_timeout(crhttp_request *req,
					  uint64_t timeout_ms);

/*
 * Decode gzip/deflate/br/zstd bodies. Default on. When off, the body is
 * delivered exactly as received (Content-Encoding still applies).
 */
CRHTTP_API int crhttp_request_set_decompress(crhttp_request *req, int enable);

/* Fail with CRHTTP_E_TOO_LARGE past this many body bytes. 0 = no limit. */
CRHTTP_API int crhttp_request_set_max_body_size(crhttp_request *req,
						uint64_t max_bytes);

/* ---------------------------------------------------------------- */
/* Blocking API                                                     */
/* ---------------------------------------------------------------- */

/*
 * Run |req| on |client| and wait for the complete response.
 *
 * Returns CRHTTP_OK when an HTTP response was received, whatever its
 * status code. On any other result *|resp| is still set when possible so
 * the error details can be inspected; it is NULL only for
 * CRHTTP_E_INVAL, CRHTTP_E_NOMEM and CRHTTP_E_WOULD_DEADLOCK. Free it
 * with crhttp_response_free().
 *
 * Safe to call from many threads at once on the same client. Must not
 * be called from a crhttp callback.
 */
CRHTTP_API int crhttp_perform(crhttp_client *client, const crhttp_request *req,
			      crhttp_response **resp);

/* ---------------------------------------------------------------- */
/* Asynchronous API                                                 */
/* ---------------------------------------------------------------- */

/*
 * All callbacks run on the client's network thread. They must return
 * quickly and must not call blocking crhttp functions.
 */
typedef struct crhttp_callbacks {
	/*
	 * Optional. Response headers are available (after redirects). The
	 * response object is the one later passed to on_complete; its body
	 * is still empty. Return non-zero to abort (CRHTTP_E_CALLBACK).
	 */
	int (*on_response)(crhttp_transfer *t, const crhttp_response *resp,
			   void *user_data);

	/*
	 * Optional. Body bytes as they arrive. When set the body is NOT
	 * accumulated in the response. Return non-zero to abort.
	 */
	int (*on_data)(crhttp_transfer *t, const void *data, size_t len,
		       void *user_data);

	/*
	 * Optional. Called exactly once when the transfer ends for any
	 * reason, including cancellation and client shutdown. |error| is as
	 * for crhttp_perform(). |resp| is owned by the transfer.
	 */
	void (*on_complete)(crhttp_transfer *t, int error,
			    const crhttp_response *resp, void *user_data);
} crhttp_callbacks;

/*
 * Start |req| on |client|. |cb| may be NULL (use crhttp_transfer_wait).
 * The returned handle holds a reference that must be released with
 * crhttp_transfer_free(). Returns NULL on invalid arguments or if the
 * client is shutting down.
 */
CRHTTP_API crhttp_transfer *crhttp_start(crhttp_client *client,
					 const crhttp_request *req,
					 const crhttp_callbacks *cb,
					 void *user_data);

/*
 * Ask the transfer to stop. Asynchronous and thread-safe; on_complete
 * follows with CRHTTP_E_CANCELED unless the transfer already finished.
 */
CRHTTP_API void crhttp_transfer_cancel(crhttp_transfer *t);

/*
 * Stop / resume reading the response body (flow control for on_data).
 * Thread-safe.
 */
CRHTTP_API void crhttp_transfer_pause(crhttp_transfer *t);
CRHTTP_API void crhttp_transfer_resume(crhttp_transfer *t);

/*
 * Wait until the transfer completes, at most |timeout_ms| milliseconds
 * (UINT64_MAX = forever). Returns the transfer result, or
 * CRHTTP_E_TIMEOUT if still running when the wait expires (the
 * transfer itself continues). Must not be called on the network thread.
 */
CRHTTP_API int crhttp_transfer_wait(crhttp_transfer *t, uint64_t timeout_ms);

/* Non-zero once on_complete has returned. */
CRHTTP_API int crhttp_transfer_done(const crhttp_transfer *t);

/*
 * The transfer's response; NULL before on_response would be invoked
 * (and after errors that happened before any response). Valid until
 * crhttp_transfer_free(). Only safe to read from other threads after
 * the transfer is done.
 */
CRHTTP_API const crhttp_response *crhttp_transfer_response(
		const crhttp_transfer *t);

/*
 * Detach the response from the transfer; the caller then owns it and
 * frees it with crhttp_response_free(). Only after completion.
 */
CRHTTP_API crhttp_response *crhttp_transfer_take_response(crhttp_transfer *t);

/*
 * Release the handle. If the transfer is still running it is canceled
 * and, unless called from one of this transfer's callbacks, this waits
 * until on_complete has run, so user_data may be freed afterwards.
 * From inside on_complete it simply releases the handle.
 */
CRHTTP_API void crhttp_transfer_free(crhttp_transfer *t);

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
typedef struct crhttp_queue crhttp_queue;

CRHTTP_API crhttp_queue *crhttp_queue_new(void);

/*
 * Destroy the queue. Transfers still running are canceled and waited
 * for; completed-but-unclaimed transfers are freed.
 */
CRHTTP_API void crhttp_queue_free(crhttp_queue *q);

/* Readable while crhttp_queue_pop() would return a transfer. */
CRHTTP_API int crhttp_queue_fd(const crhttp_queue *q);

/*
 * Start |req| on |client|; when it completes it is appended to |q|.
 * |user_data| is handed back by crhttp_queue_pop(). Returns
 * CRHTTP_OK or a negative crhttp_error.
 */
CRHTTP_API int crhttp_queue_start(crhttp_queue *q, crhttp_client *client,
				  const crhttp_request *req, void *user_data);

/*
 * Pop one completed transfer, waiting up to |timeout_ms| (0 = poll,
 * UINT64_MAX = forever). On success *|t| owns the transfer (inspect it
 * with crhttp_transfer_response()/crhttp_transfer_take_response(), its
 * result with crhttp_transfer_wait(t, 0), then crhttp_transfer_free()).
 * Returns CRHTTP_OK, CRHTTP_E_TIMEOUT when nothing completed in time,
 * or CRHTTP_E_STATE when no transfer is pending or running at all.
 */
CRHTTP_API int crhttp_queue_pop(crhttp_queue *q, crhttp_transfer **t,
				void **user_data, uint64_t timeout_ms);

/* Transfers started on |q| and not yet popped (running + completed). */
CRHTTP_API size_t crhttp_queue_pending(const crhttp_queue *q);

/* ---------------------------------------------------------------- */
/* Response                                                         */
/* ---------------------------------------------------------------- */

CRHTTP_API void crhttp_response_free(crhttp_response *resp);

/* HTTP status code, 0 if no response was received. */
CRHTTP_API int crhttp_response_status(const crhttp_response *resp);
/* Reason phrase ("OK"); "" for HTTP/2 and HTTP/3. */
CRHTTP_API const char *crhttp_response_status_text(const crhttp_response *resp);

/* Final URL after redirects. */
CRHTTP_API const char *crhttp_response_url(const crhttp_response *resp);
/* Number of redirects that were followed. */
CRHTTP_API int crhttp_response_redirect_count(const crhttp_response *resp);

/* Header lines in received order; names as sent by the server. */
CRHTTP_API size_t crhttp_response_header_count(const crhttp_response *resp);
CRHTTP_API int crhttp_response_header_at(const crhttp_response *resp,
					 size_t index, const char **name,
					 const char **value);
/*
 * First value of header |name| (case-insensitive) or NULL. Use
 * crhttp_response_header_at() for repeated headers such as Set-Cookie.
 */
CRHTTP_API const char *crhttp_response_header(const crhttp_response *resp,
					      const char *name);

/* Accumulated body (NULL-terminated for convenience; *len excludes it). */
CRHTTP_API const char *crhttp_response_body(const crhttp_response *resp,
					    size_t *len);

/* Negotiated protocol: "h2", "http/1.1", "h3", ... */
CRHTTP_API const char *crhttp_response_protocol(const crhttp_response *resp);
/* "ip:port" of the server (or proxy) actually connected to. */
CRHTTP_API const char *crhttp_response_remote_address(
		const crhttp_response *resp);
/* Whether the response was obtained through a proxy. */
CRHTTP_API int crhttp_response_via_proxy(const crhttp_response *resp);

/*
 * Chromium net error code (0 = OK, e.g. -105 = ERR_NAME_NOT_RESOLVED)
 * and its name ("net::ERR_NAME_NOT_RESOLVED", "" when OK).
 */
CRHTTP_API int crhttp_response_net_error(const crhttp_response *resp);
CRHTTP_API const char *crhttp_response_error_string(
		const crhttp_response *resp);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CRHTTP_CRHTTP_H */
