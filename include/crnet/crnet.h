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
#define CRNET_VERSION_MINOR 29
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

/* "0.21.0 (Chromium 155.0.8059.39)". Static storage. */
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
 * ("http://user:pass@host:port") answer proxy auth challenges. In a
 * SOCKS5 URL ("socks5://user:pass@host:port", each 1 to 255 bytes,
 * percent-encoded as needed) they enable username/password
 * authentication (RFC 1929), offered besides "no authentication"; a
 * proxy rejecting them fails requests with net error -185. SOCKS4 has
 * no credentials. NULL means direct. An empty userinfo
 * ("socks5://@host:port", CRNET_HAS_PROXY_NO_CREDENTIALS) means no
 * credentials, the same as none here.
 *
 * Credentials belong to the client (and its requests, see
 * crnet_request_set_proxy()): a client given none offers none, whatever
 * credentials other clients use the same proxy with. Before 0.28.1 the
 * SOCKS5 client found them by proxy host and port across clients,
 * crnet_client_new() refused a second client with other credentials for
 * a SOCKS5 proxy, and one without any could authenticate with another
 * client's.
 */
#define CRNET_HAS_SOCKS5_AUTH 1
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

/*
 * Override the browser identity. NULL restores the default: the
 * platform profile's User-Agent, and the build's sec-ch-ua.
 */
CRNET_API int crnet_client_config_set_user_agent(crnet_client_config *cfg,
						 const char *ua);
CRNET_API int crnet_client_config_set_sec_ch_ua(crnet_client_config *cfg,
						const char *sec_ch_ua);
/*
 * Value of sec-ch-ua-platform without quotes. NULL restores the
 * platform profile's value ("Linux" by default).
 */
CRNET_API int crnet_client_config_set_platform(crnet_client_config *cfg,
					       const char *platform);

/*
 * The operating system the client claims, as one switch. A profile
 * sets every platform-dependent part of the browser identity together,
 * as Chrome sends them on that platform: the User-Agent and
 * sec-ch-ua-platform. Chrome's reduced User-Agent freezes the OS
 * version, so one string covers every release of an OS:
 *
 *	LINUX	"X11; Linux x86_64"			"Linux"
 *	WINDOWS	"Windows NT 10.0; Win64; x64"		"Windows"
 *	MACOS	"Macintosh; Intel Mac OS X 10_15_7"	"macOS"
 *
 * All three are desktop platforms: sec-ch-ua-mobile stays "?0", and
 * the TLS and HTTP/2 handshakes do not depend on the platform.
 *
 * A User-Agent or platform set explicitly takes precedence over the
 * profile, whatever the order of the calls. Default LINUX.
 * CRNET_E_INVAL for an unknown profile.
 */
#define CRNET_HAS_PLATFORM_PROFILE 1
typedef enum crnet_platform_profile {
	CRNET_PLATFORM_LINUX = 0,
	CRNET_PLATFORM_WINDOWS = 1,
	CRNET_PLATFORM_MACOS = 2,
} crnet_platform_profile;
CRNET_API int crnet_client_config_set_platform_profile(
		crnet_client_config *cfg, crnet_platform_profile profile);

/*
 * The User-Agent a platform profile sends, or NULL for an unknown
 * profile. Static storage. crnet_default_user_agent() is LINUX's.
 */
CRNET_API const char *crnet_platform_user_agent(
		crnet_platform_profile profile);
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

/*
 * DNS over HTTPS, as Chrome's "Use secure DNS" setting. |templates| are
 * one or more DoH URI templates separated by whitespace, in Chrome's
 * syntax ("https://dns.example/dns-query{?dns}"; without {?dns} queries
 * are POSTed). CRNET_SECURE_DNS_AUTOMATIC uses them once a probe shows
 * they work and falls back to ordinary DNS; CRNET_SECURE_DNS_SECURE
 * resolves only through them. Host names of the DoH servers themselves
 * are resolved with ordinary DNS. Default CRNET_SECURE_DNS_OFF. No
 * effect when a proxy resolves names. CRNET_E_INVAL for a malformed
 * template or a mode other than OFF without templates.
 */
#define CRNET_HAS_SECURE_DNS 1
typedef enum crnet_secure_dns {
	CRNET_SECURE_DNS_OFF = 0,
	CRNET_SECURE_DNS_AUTOMATIC = 1,
	CRNET_SECURE_DNS_SECURE = 2,
} crnet_secure_dns;
CRNET_API int crnet_client_config_set_secure_dns(crnet_client_config *cfg,
						 crnet_secure_dns mode,
						 const char *templates);

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

/*
 * Certificate verification is Chrome's: the Chrome Root Store, plus
 * roots the user added to the system store (the NSS database on Linux).
 *
 * Add the CERTIFICATE blocks of |pem| as extra trust anchors (roots) for
 * this client, such as a private CA. May be called repeatedly. Returns
 * CRNET_E_INVAL if |pem| holds no certificate or one that does not
 * parse. The anchors' own validity period and constraints are not
 * enforced, as for roots a user adds to Chrome.
 */
#define CRNET_HAS_TRUST_ANCHORS 1
CRNET_API int crnet_client_config_add_trust_anchors(crnet_client_config *cfg,
						    const char *pem);
/* The same, reading the PEM from a file. */
CRNET_API int crnet_client_config_add_trust_anchors_file(
		crnet_client_config *cfg, const char *path);
/*
 * Non-zero: trust only the anchors added above, not the Chrome Root
 * Store or the system store. Default 0.
 */
CRNET_API void crnet_client_config_set_trust_anchors_only(
		crnet_client_config *cfg, int only);

/*
 * A client certificate (TLS client authentication), presented to any
 * server that asks for one, HTTP and WebSocket alike. |cert_pem| holds
 * the certificate, optionally followed by intermediates to send along;
 * |key_pem| its unencrypted private key (PKCS#8, or a traditional RSA or
 * EC key). CRNET_E_INVAL if either does not parse or the key does not
 * match the certificate. NULL for both removes it. Without one, crnet
 * answers a request for a certificate with none, as Chrome does when the
 * user picks none, and the server decides.
 */
#define CRNET_HAS_CLIENT_CERT 1
CRNET_API int crnet_client_config_set_client_cert(crnet_client_config *cfg,
						  const char *cert_pem,
						  const char *key_pem);
/* The same, reading both from files. */
CRNET_API int crnet_client_config_set_client_cert_file(
		crnet_client_config *cfg, const char *cert_path,
		const char *key_path);

/* Default per-request timeout in milliseconds, 0 = none (default). */
CRNET_API void crnet_client_config_set_timeout(crnet_client_config *cfg,
					       uint64_t timeout_ms);

/*
 * Default connect timeout in milliseconds, 0 = none (default). It
 * limits the time until a request has a connection ready to send on:
 * proxy resolution, DNS, TCP (or QUIC), the proxy handshake and TLS,
 * including Chromium's own fallbacks between addresses and protocols.
 * It restarts for every redirect hop, and is met at once by a reused
 * connection. On expiry the transfer fails with CRNET_E_TIMEOUT and net
 * error ERR_CONNECTION_TIMED_OUT (-118); the request timeout reports
 * ERR_TIMED_OUT (-7) instead. HTTP transfers only, not WebSockets.
 */
#define CRNET_HAS_CONNECT_TIMEOUT 1
CRNET_API void crnet_client_config_set_connect_timeout(
		crnet_client_config *cfg, uint64_t timeout_ms);

/*
 * Local (source) addresses for the client's connections, for hosts
 * with several egress addresses: one IP literal, or an IPv4 and an IPv6
 * literal separated by a comma ("192.0.2.10,2001:db8::10"). Every TCP
 * and QUIC socket of the client is bound to the address of its peer's
 * family before it connects: connections to servers and to proxies,
 * WebSockets and DNS-over-HTTPS requests. Plain DNS queries are not
 * bound.
 *
 * A connection to a family without a local address fails with net
 * error ERR_ADDRESS_UNREACHABLE (-109) rather than leave from the
 * system's choice of address; Chromium then tries the server's
 * addresses of the other family, as it does when a family is
 * unreachable. "0.0.0.0" or "::" lets that family use the system's
 * choice. An address the host does not have fails connections with
 * ERR_ADDRESS_INVALID (-108).
 *
 * NULL or "" means the system's choice for both families (default).
 * CRNET_E_INVAL for a malformed list or two addresses of one family.
 */
#define CRNET_HAS_LOCAL_ADDRESS 1
CRNET_API int crnet_client_config_set_local_address(crnet_client_config *cfg,
						    const char *addresses);

/*
 * A network interface ("eth1", "wg0") the client's connections must
 * use, as SO_BINDTODEVICE does on Linux: the same sockets as
 * crnet_client_config_set_local_address() are bound to it before they
 * connect, so their packets leave through it whatever the routing
 * table says. Combine the two for a source address and an interface.
 * Plain DNS queries are not bound; with a VPN interface, use DNS over
 * HTTPS (crnet_client_config_set_secure_dns()) to keep lookups on it.
 *
 * The interface need not exist yet: connections fail with net error
 * ERR_ADDRESS_UNREACHABLE (-109) while it does not. Before Linux 5.7 the
 * option needs CAP_NET_RAW; without it connections fail with
 * ERR_ACCESS_DENIED (-10).
 *
 * NULL or "" means the routing table's choice (default). CRNET_E_INVAL
 * for a name the kernel would not accept (empty, 16 bytes or longer,
 * "." or "..", or containing '/', ':' or whitespace).
 */
#define CRNET_HAS_BIND_INTERFACE 1
CRNET_API int crnet_client_config_set_bind_interface(crnet_client_config *cfg,
						     const char *name);

/*
 * Chromium's HTTP cache (the one Chrome uses), off by default: every
 * request then goes to the network, as with a cache that is always
 * empty. A client with a cache stores responses the server allows it to
 * and revalidates stale ones (If-None-Match, If-Modified-Since), like a
 * returning browser. As in Chrome, a response whose certificate had an
 * error (accepted with the insecure option) is never stored.
 */
#define CRNET_HAS_HTTP_CACHE 1
typedef enum crnet_http_cache {
	CRNET_HTTP_CACHE_OFF = 0,
	CRNET_HTTP_CACHE_MEMORY = 1,
	/* On disk under |path|, kept across runs. One client at a time
	 * per directory. */
	CRNET_HTTP_CACHE_DISK = 2,
} crnet_http_cache;

/*
 * |max_size| in bytes, 0 = Chromium's default (derived from free disk
 * space for a disk cache); at most 2 GiB. |path| is required for
 * CRNET_HTTP_CACHE_DISK and ignored otherwise.
 */
CRNET_API int crnet_client_config_set_http_cache(crnet_client_config *cfg,
						 crnet_http_cache type,
						 const char *path,
						 uint64_t max_size);

/*
 * Keep network state across runs in directory |dir| (created if
 * missing), as Chrome keeps it in a profile, so that a new client starts
 * where the last one stopped instead of like a fresh browser install:
 *   - "Network Persistent State": HTTP server properties, i.e. Alt-Svc
 *     (HTTP/3) advertisements, broken alternative services, HTTP/2
 *     support and QUIC server information;
 *   - "TransportSecurity": HSTS learned from Strict-Transport-Security
 *     headers.
 * The files are read in the background when the client starts (requests
 * made before that finishes do not see them), saved in batches as the
 * state changes, as Chrome does, and once more when the client is freed;
 * crnet_client_free() returns after that save. One client at a time per
 * directory. Cookies and the HTTP cache have their own settings
 * (crnet_client_export_cookies(), crnet_client_config_set_http_cache()).
 * NULL or "" keeps nothing (default).
 */
#define CRNET_HAS_STATE_DIR 1
CRNET_API int crnet_client_config_set_state_dir(crnet_client_config *cfg,
						const char *dir);

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

/*
 * Saving and restoring the cookie jar, e.g. to keep a session across
 * runs. Like the calls above they block, so they must not be called
 * from a callback.
 */
#define CRNET_HAS_COOKIE_JAR_IO 1
typedef enum crnet_cookie_format {
	/* crnet's JSON: an array of objects with every attribute Chromium
	 * keeps (SameSite, priority, partition, creation and access times,
	 * source scheme and port); lossless. */
	CRNET_COOKIES_JSON = 0,
	/* Netscape cookies.txt as read and written by curl and wget,
	 * "#HttpOnly_" prefix included. Loses SameSite, priority and the
	 * times other than expiry, and cannot hold partitioned (CHIPS)
	 * cookies, which are left out. */
	CRNET_COOKIES_NETSCAPE = 1,
} crnet_cookie_format;

/*
 * All cookies in the jar, session cookies included. Release with
 * crnet_free(). An empty jar (or cookies disabled) gives an empty list
 * in the format. NULL on invalid arguments or on a network thread.
 */
CRNET_API char *crnet_client_export_cookies(crnet_client *client,
					    crnet_cookie_format format);

/*
 * Add cookies from |data| in |format| to the jar, replacing cookies of
 * the same name, domain and path. Expired cookies are skipped. If
 * |data| is malformed or holds any cookie a site could not set (bad
 * name or value, a domain cookie for a public suffix such as ".com"),
 * nothing is imported and CRNET_E_INVAL returned. Each cookie is then
 * set as if its site had sent it, which can still refuse one given the
 * jar's contents (a non-secure cookie may not replace a secure one);
 * the others are kept and CRNET_E_INVAL is returned. *imported (may be
 * NULL) receives the number stored. CRNET_E_STATE when cookies are
 * disabled.
 */
CRNET_API int crnet_client_import_cookies(crnet_client *client,
					  const char *data,
					  crnet_cookie_format format,
					  size_t *imported);

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

/*
 * How the request uses the client's HTTP cache, as fetch()'s |cache|
 * option (Chrome maps it to the same Chromium load flags). Without a
 * cache only CRNET_CACHE_ONLY_IF_CACHED matters: it always fails.
 */
typedef enum crnet_cache_mode {
	/* Use a fresh entry, revalidate a stale one, store the result. */
	CRNET_CACHE_DEFAULT = 0,
	/* Neither read nor write the cache. */
	CRNET_CACHE_NO_STORE = 1,
	/* Go to the network unconditionally; store the result. */
	CRNET_CACHE_RELOAD = 2,
	/* Revalidate any entry with the server, even a fresh one. */
	CRNET_CACHE_NO_CACHE = 3,
	/* Use any entry, however stale; the network only on a miss. */
	CRNET_CACHE_FORCE_CACHE = 4,
	/* Use any entry; fail with net::ERR_CACHE_MISS (-400) on a miss. */
	CRNET_CACHE_ONLY_IF_CACHED = 5,
} crnet_cache_mode;
CRNET_API int crnet_request_set_cache_mode(crnet_request *req,
					   crnet_cache_mode mode);
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
/* Overrides the client's connect timeout; 0 = none. */
CRNET_API int crnet_request_set_connect_timeout(crnet_request *req,
						uint64_t timeout_ms);

/*
 * Decode gzip/deflate/br/zstd bodies. Default on. When off, the body is
 * delivered as received (Content-Encoding still applies), except for a
 * response carrying Use-As-Dictionary, which Chromium always decodes;
 * crnet_response_content_decoded() tells.
 */
CRNET_API int crnet_request_set_decompress(crnet_request *req, int enable);

/*
 * A proxy for this request only (HTTP or WebSocket), in the syntax of
 * crnet_client_config_set_proxy(); "direct://" for none. NULL (default)
 * uses the client's proxy. It replaces the client's proxy and bypass
 * rules for every destination of the request, redirects included, with
 * no fallback. The request still shares the client's cookies, cache and
 * connections through the same proxy; connections through different
 * proxies are never shared. CRNET_E_INVAL for a malformed URL or "".
 *
 * Credentials: a proxy's default credentials are the client's own for
 * its configured proxy (none, if it was configured without), or else
 * those of the first request that names the proxy with credentials; a
 * request naming the proxy without credentials uses them, answering
 * auth challenges as configured clients do. Since 0.28.0
 * (CRNET_HAS_PROXY_IDENTITIES) a request naming other credentials is
 * another identity of the proxy, as rotating proxies use
 * ("user-session-42:pass@gateway"): requests of one identity share
 * connections only with each other, and its credentials go with every
 * request to the proxy up front, as HTTP Basic or SOCKS5
 * username/password authentication; a proxy refusing them fails the
 * request (HTTP status 407 through an HTTP proxy, net error -185 through
 * SOCKS5) instead of being asked again. Before 0.28.0 such a request
 * failed with CRNET_E_INVAL.
 *
 * An empty userinfo ("socks5://@host:port", "http://@host:port") means
 * no credentials at all, rather than the default ones: the request
 * offers SOCKS5 "no authentication" only and sends an HTTP proxy no
 * Proxy-Authorization, not even credentials Chromium remembers for it.
 * Named before the proxy has default credentials, it makes "none" the
 * default. (CRNET_HAS_PROXY_NO_CREDENTIALS, 0.29.0.)
 */
#define CRNET_HAS_REQUEST_PROXY 1
#define CRNET_HAS_PROXY_IDENTITIES 1
#define CRNET_HAS_PROXY_NO_CREDENTIALS 1
CRNET_API int crnet_request_set_proxy(crnet_request *req,
				      const char *proxy_url);

/*
 * Use connections of the request's own (HTTP or WebSocket), as a new
 * browser profile would: no pooled TCP connection, HTTP/2 or HTTP/3
 * session is taken, nothing it opens is used by another request, and
 * its TLS handshakes resume no earlier session. Redirects of the
 * request may reuse its own connections. Once the transfer (or the
 * WebSocket) is over and its request released, its idle connections
 * are closed. For retrying on a new connection, or a "no keep-alive"
 * setting. Default off.
 */
#define CRNET_HAS_FRESH_CONNECTION 1
CRNET_API int crnet_request_set_fresh_connection(crnet_request *req,
						 int enable);

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
 * Progress of a running transfer, readable from any thread at any time
 * (also after it finished, then final). Values are byte counts; a total
 * is -1 while unknown.
 */
#define CRNET_HAS_PROGRESS 1
typedef enum crnet_progress {
	/* Request body bytes handed to the network so far. Sampled every
	 * 100 ms while the body is being sent (as Chrome does), exact once
	 * the response headers have arrived. */
	CRNET_PROGRESS_UPLOAD_SENT = 0,
	/* Size of the request body: 0 without one, -1 for a streamed body
	 * of unknown length. */
	CRNET_PROGRESS_UPLOAD_TOTAL,
	/* Response body bytes received, as sent by the server (before
	 * content decoding). */
	CRNET_PROGRESS_DOWNLOAD_RECEIVED,
	/* Response body bytes after decoding, as delivered to the
	 * application. */
	CRNET_PROGRESS_DOWNLOAD_DECODED,
	/* Content-Length of the response (compare with
	 * DOWNLOAD_RECEIVED); -1 when the server sent none. */
	CRNET_PROGRESS_DOWNLOAD_TOTAL,
	CRNET_PROGRESS_COUNT
} crnet_progress;

/* -1 for an invalid |which|. */
CRNET_API int64_t crnet_transfer_progress(const crnet_transfer *t,
					  crnet_progress which);

/*
 * Progress notifications instead of polling: |cb| is called on the
 * client's network thread, at most every |interval_ms| (at least 1)
 * while the transfer runs and only when a crnet_transfer_progress()
 * value changed, and once more before on_complete if anything changed
 * since. Read the values with crnet_transfer_progress(); |user_data| is
 * the one given to crnet_start(). The upload position is sampled every
 * 100 ms (as in Chrome), or at each call if more often. Thread-safe;
 * call it right after crnet_start() (progress made before it is simply
 * reported by the first call). NULL removes it. CRNET_E_STATE once the
 * transfer finished.
 */
#define CRNET_HAS_PROGRESS_CALLBACK 1
typedef void (*crnet_progress_cb)(crnet_transfer *t, void *user_data);
CRNET_API int crnet_transfer_set_progress_callback(crnet_transfer *t,
						   crnet_progress_cb cb,
						   uint64_t interval_ms);

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
 * Streaming upload: a file descriptor that is readable while
 * crnet_transfer_write() would not block (the send buffer has room, or
 * the write would fail at once because the body was ended or the
 * transfer finished). With it and |timeout_ms| = 0 an event loop can
 * feed the body without a thread of its own. -1 for other transfers.
 */
#define CRNET_HAS_WRITE_FD 1
CRNET_API int crnet_transfer_write_fd(const crnet_transfer *t);

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
 * Waits as file descriptors, for event loops: each is readable from the
 * moment the matching wait would return at once, and stays readable.
 * crnet_transfer_response_fd() goes with crnet_transfer_wait_response()
 * (headers arrived, or the transfer failed first), and
 * crnet_transfer_done_fd() with crnet_transfer_wait() (on_complete
 * returned). Any transfer, streamed or not. The descriptor is made on
 * the first call and belongs to the transfer: do not read from it or
 * close it; it is valid until crnet_transfer_free(). -1 if it could not
 * be made (out of descriptors). See also crnet_ws_open_fd().
 */
#define CRNET_HAS_WAIT_FDS 1
CRNET_API int crnet_transfer_response_fd(const crnet_transfer *t);
CRNET_API int crnet_transfer_done_fd(const crnet_transfer *t);

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
 *   - crnet_request_set_ws_compression(req, 0), or removing the
 *     "Sec-WebSocket-Extensions" header (NULL value), offers no
 *     permessage-deflate.
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
 * error), and on_open or on_close has returned, so what the callback did
 * is visible to the caller. CRNET_E_TIMEOUT if neither happened within
 * |timeout_ms| (UINT64_MAX = forever). Not on the network thread.
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

/*
 * Whether a WebSocket opened with |req| offers permessage-deflate, as
 * Chrome always does. Default 1. With 0 the handshake has no
 * Sec-WebSocket-Extensions header, so messages travel uncompressed; the
 * handshake then differs from Chrome's.
 */
#define CRNET_HAS_WS_COMPRESSION_SWITCH 1
CRNET_API int crnet_request_set_ws_compression(crnet_request *req,
					       int enable);

/*
 * Drop the connection when no message has arrived for |timeout_ms|
 * (0 = never, the default): on_close then reports CRNET_E_TIMEOUT, code
 * 1006 and net error ERR_TIMED_OUT, and no closing handshake is
 * attempted, as the peer is presumed gone. The time counts while the
 * connection is open and messages are being read; it restarts with
 * every message and every pong (see crnet_ws_set_auto_ping()), and stops
 * while crnet is not reading because the application paused or did not
 * take queued messages. The server's pings do not count: Chromium
 * answers them itself without reporting them.
 * Thread-safe; may be called at any time, also before the connection is
 * open.
 */
#define CRNET_HAS_WS_IDLE_TIMEOUT 1
CRNET_API void crnet_ws_set_idle_timeout(crnet_ws *ws, uint64_t timeout_ms);

/*
 * Pings. Chromium answers the server's pings by itself; these let the
 * application ping the server and see its pongs (which Chrome offers
 * pages neither of).
 *
 * crnet_ws_ping() queues a Ping frame carrying |len| <= 125 bytes.
 * Thread-safe; CRNET_E_STATE unless open, CRNET_E_INVAL if too long.
 */
#define CRNET_HAS_WS_PING 1
CRNET_API int crnet_ws_ping(crnet_ws *ws, const void *data, size_t len);

/*
 * Called on the network thread for every Pong frame, solicited or not,
 * with its payload (valid during the call) and the WebSocket's
 * user_data. A pong also restarts the idle timeout. NULL removes it.
 * Thread-safe.
 */
typedef void (*crnet_ws_pong_cb)(crnet_ws *ws, const void *data, size_t len,
				 void *user_data);
CRNET_API void crnet_ws_set_pong_callback(crnet_ws *ws, crnet_ws_pong_cb cb);

/*
 * Ping the server (empty payload) every |interval_ms| while the
 * connection is open; 0 stops it (default). Together with
 * crnet_ws_set_idle_timeout() longer than the interval this detects a
 * vanished peer even when no messages are expected. Thread-safe; may be
 * called before the connection is open.
 */
CRNET_API void crnet_ws_set_auto_ping(crnet_ws *ws, uint64_t interval_ms);

/* Stop / resume reading messages from the network. Thread-safe. */
CRNET_API void crnet_ws_pause(crnet_ws *ws);
CRNET_API void crnet_ws_resume(crnet_ws *ws);

/*
 * Wait until the connection is closed; returns the close error (see
 * on_close). CRNET_E_TIMEOUT if still open after |timeout_ms|.
 */
CRNET_API int crnet_ws_wait_closed(crnet_ws *ws, uint64_t timeout_ms);

/*
 * Waits as file descriptors (CRNET_HAS_WAIT_FDS, as for transfers):
 * crnet_ws_open_fd() is readable once crnet_ws_wait_open() would return
 * at once (open, or failed), crnet_ws_closed_fd() once
 * crnet_ws_wait_closed() would. Owned by the handle; valid until
 * crnet_ws_free(). -1 if it could not be made.
 */
CRNET_API int crnet_ws_open_fd(const crnet_ws *ws);
CRNET_API int crnet_ws_closed_fd(const crnet_ws *ws);

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
 * SOCKS5 failures. crnet's Chromium reports them precisely instead of
 * folding them all into ERR_SOCKS_CONNECTION_FAILED (-120):
 *   -191 .. -198  the proxy refused the CONNECT with reply code 1 .. 8
 *                 (general failure, not allowed by ruleset, network
 *                 unreachable, host unreachable, connection refused,
 *                 TTL expired, command not supported, address type not
 *                 supported);
 *   -199          the proxy accepted none of the offered authentication
 *                 methods;
 *   -185          the proxy rejected the username and password;
 *   -100          ERR_CONNECTION_CLOSED: the proxy closed the connection
 *                 during the handshake (other transport errors, such as
 *                 a reset, keep their own codes);
 *   -120          a malformed reply, an unknown reply code or a method
 *                 that was never offered.
 * crnet_response_socks_reply() returns the reply code (1 .. 8) of a
 * refused CONNECT, and -1 for anything else.
 */
#define CRNET_HAS_SOCKS_ERRORS 1
CRNET_API int crnet_response_socks_reply(const crnet_response *resp);

/*
 * Whether the response came from the HTTP cache, including one the
 * server confirmed with 304 Not Modified (the status is then that of
 * the stored response).
 */
CRNET_API int crnet_response_from_cache(const crnet_response *resp);

/*
 * Connection and timing information, from Chromium's LoadTimingInfo
 * for the final request (the last redirect hop).
 */
#define CRNET_HAS_TIMING 1

/*
 * Whether the request was sent on a connection that already existed (an
 * idle keep-alive socket, or a shared HTTP/2 or HTTP/3 session).
 */
CRNET_API int crnet_response_socket_reused(const crnet_response *resp);

/*
 * Identifies the connection that carried the request: requests with the
 * same id shared one connection (an HTTP/2 or HTTP/3 session counts as
 * one). Unique within the process; 0 when unknown (no response).
 */
CRNET_API uint64_t crnet_response_connection_id(const crnet_response *resp);

typedef enum crnet_timing {
	/* The final hop started; 0 unless redirects were followed. */
	CRNET_TIMING_REQUEST_START = 0,
	CRNET_TIMING_PROXY_RESOLVE_START,
	CRNET_TIMING_PROXY_RESOLVE_END,
	CRNET_TIMING_DNS_START,
	CRNET_TIMING_DNS_END,
	/* TCP (or QUIC) connect; includes the TLS handshake and any proxy
	 * tunnel set-up. */
	CRNET_TIMING_CONNECT_START,
	CRNET_TIMING_CONNECT_END,
	CRNET_TIMING_TLS_START,
	CRNET_TIMING_TLS_END,
	CRNET_TIMING_SEND_START,
	CRNET_TIMING_SEND_END,
	/* First and last byte of the response headers. */
	CRNET_TIMING_HEADERS_START,
	CRNET_TIMING_HEADERS_END,
	/* The transfer finished (on_complete was about to run). */
	CRNET_TIMING_COMPLETE,
	CRNET_TIMING_COUNT
} crnet_timing;

/*
 * When |which| happened, in microseconds since crnet_start() (or
 * crnet_perform() / crnet_queue_start()), or -1 if it did not happen:
 * a reused connection has no DNS, connect or TLS phase, and nothing but
 * CRNET_TIMING_COMPLETE is known for a transfer that failed before the
 * response headers. Times are clamped as in Chrome's DevTools: a phase
 * that started before the hop (a preconnect, a shared session) is
 * reported as starting with it.
 */
CRNET_API int64_t crnet_response_timing(const crnet_response *resp,
					crnet_timing which);

/*
 * Whether crnet removed content codings from the body. When it did, the
 * body is the decoded content while the Content-Encoding and
 * Content-Length headers still describe the bytes as sent. Chromium
 * decodes br, deflate, gzip (and x-gzip) and zstd; when Content-Encoding
 * names anything else, "identity" included, it leaves the whole body
 * as received. With crnet_request_set_decompress(req, 0) the body is
 * left as received too, except that Chromium still decodes a response
 * carrying Use-As-Dictionary. Known from the response headers on.
 */
#define CRNET_HAS_CONTENT_DECODED 1
CRNET_API int crnet_response_content_decoded(const crnet_response *resp);
/*
 * The codings removed, in Content-Encoding order and by canonical name
 * ("gzip", "deflate, br"); "" when the body is as received.
 */
CRNET_API const char *crnet_response_decoded_codings(
		const crnet_response *resp);

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
