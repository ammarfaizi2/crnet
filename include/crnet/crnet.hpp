// SPDX-License-Identifier: BSD-3-Clause
//
// Header-only C++17 convenience layer over the crnet C API.
//
// Everything here is inline and talks to libcrnet.so only through the
// C ABI, so it works with any C++ standard library (libcrnet.so uses
// its own private copy of libc++ internally).
//
//   crnet::Client client(crnet::ClientConfig().proxy("socks5://..."));
//   crnet::Response r = client.get("https://example.com/");
//   std::cout << r.status() << "\n" << r.body();
//
//   crnet::WebSocket ws = client.websocket("wss://example.com/chat");
//   ws.wait_open();
//   ws.send_text("hello");
//   std::cout << ws.recv()->data;
//
#ifndef CRNET_CRNET_HPP
#define CRNET_CRNET_HPP

#include "crnet.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace crnet {

class Error : public std::runtime_error {
public:
	explicit Error(int code, const std::string &what)
		: std::runtime_error(what), code_(code) {}
	int code() const noexcept { return code_; }

private:
	int code_;
};

namespace detail {
inline std::string str(const char *s) { return s ? std::string(s) : std::string(); }
inline const char *cstr(const std::string &s) { return s.c_str(); }
} // namespace detail

class ClientConfig {
public:
	ClientConfig() : cfg_(crnet_client_config_new())
	{
		if (!cfg_)
			throw Error(CRNET_E_NOMEM, "crnet_client_config_new");
	}

	ClientConfig &proxy(const std::string &url) { check(crnet_client_config_set_proxy(get(), url.c_str())); return *this; }
	ClientConfig &proxy_bypass(const std::string &rules) { check(crnet_client_config_set_proxy_bypass(get(), rules.c_str())); return *this; }
	ClientConfig &user_agent(const std::string &ua) { check(crnet_client_config_set_user_agent(get(), ua.c_str())); return *this; }
	ClientConfig &sec_ch_ua(const std::string &v) { check(crnet_client_config_set_sec_ch_ua(get(), v.c_str())); return *this; }
	ClientConfig &platform(const std::string &v) { check(crnet_client_config_set_platform(get(), v.c_str())); return *this; }
	ClientConfig &accept_language(const std::string &v) { check(crnet_client_config_set_accept_language(get(), v.c_str())); return *this; }
	ClientConfig &http2(bool on) { crnet_client_config_set_http2(get(), on); return *this; }
	ClientConfig &quic(bool on) { crnet_client_config_set_quic(get(), on); return *this; }
	ClientConfig &builtin_dns(bool on) { crnet_client_config_set_builtin_dns(get(), on); return *this; }
	ClientConfig &cookies(bool on) { crnet_client_config_set_cookies(get(), on); return *this; }
	ClientConfig &insecure(bool on) { crnet_client_config_set_insecure(get(), on); return *this; }
	ClientConfig &host_rules(const std::string &r) { check(crnet_client_config_set_host_rules(get(), r.c_str())); return *this; }
	ClientConfig &timeout_ms(uint64_t ms) { crnet_client_config_set_timeout(get(), ms); return *this; }
	ClientConfig &netlog(const std::string &path) { check(crnet_client_config_set_netlog(get(), path.c_str())); return *this; }

	crnet_client_config *get() const noexcept { return cfg_.get(); }

private:
	static void check(int rc)
	{
		if (rc != CRNET_OK)
			throw Error(rc, crnet_strerror(rc));
	}

	struct Deleter { void operator()(crnet_client_config *c) const { crnet_client_config_free(c); } };
	std::unique_ptr<crnet_client_config, Deleter> cfg_;
};

class Request {
public:
	explicit Request(const std::string &url, const std::string &method = "GET")
		: req_(crnet_request_new(method.c_str(), url.c_str()))
	{
		if (!req_)
			throw Error(CRNET_E_INVAL, "invalid URL: " + url);
	}

	Request &method(const std::string &m) { check(crnet_request_set_method(get(), m.c_str())); return *this; }
	Request &url(const std::string &u) { check(crnet_request_set_url(get(), u.c_str())); return *this; }
	Request &header(const std::string &name, const std::string &value) { check(crnet_request_set_header(get(), name.c_str(), value.c_str())); return *this; }
	Request &remove_header(const std::string &name) { check(crnet_request_set_header(get(), name.c_str(), nullptr)); return *this; }
	Request &body(std::string_view data) { check(crnet_request_set_body(get(), data.data(), data.size())); return *this; }
	// Upload a file (or a slice of it) without loading it into memory.
	Request &body_file(const std::string &path, uint64_t offset = 0, uint64_t length = UINT64_MAX)
	{
		check(crnet_request_set_body_file(get(), path.c_str(), offset, length));
		return *this;
	}
	// Body written later with Transfer::write(); see crnet_request_set_body_stream().
	Request &body_stream(int64_t length = CRNET_LENGTH_UNKNOWN) { check(crnet_request_set_body_stream(get(), length)); return *this; }
	// Response body read with Transfer::read() instead of being buffered.
	Request &stream_response(bool on = true) { check(crnet_request_set_response_stream(get(), on)); return *this; }
	Request &mode(crnet_mode m) { check(crnet_request_set_mode(get(), m)); return *this; }
	Request &priority(crnet_priority p) { check(crnet_request_set_priority(get(), p)); return *this; }
	Request &max_redirects(int n) { check(crnet_request_set_max_redirects(get(), n)); return *this; }
	Request &timeout_ms(uint64_t ms) { check(crnet_request_set_timeout(get(), ms)); return *this; }
	Request &decompress(bool on) { check(crnet_request_set_decompress(get(), on)); return *this; }
	Request &max_body_size(uint64_t n) { check(crnet_request_set_max_body_size(get(), n)); return *this; }

	crnet_request *get() const noexcept { return req_.get(); }

private:
	static void check(int rc)
	{
		if (rc != CRNET_OK)
			throw Error(rc, crnet_strerror(rc));
	}

	struct Deleter { void operator()(crnet_request *r) const { crnet_request_free(r); } };
	std::unique_ptr<crnet_request, Deleter> req_;
};

// Non-owning view of a crnet_response; see Response for the owner.
class ResponseView {
public:
	ResponseView() = default;
	explicit ResponseView(const crnet_response *r) : r_(r) {}

	explicit operator bool() const noexcept { return r_ != nullptr; }
	int status() const { return crnet_response_status(r_); }
	std::string status_text() const { return detail::str(crnet_response_status_text(r_)); }
	std::string url() const { return detail::str(crnet_response_url(r_)); }
	int redirect_count() const { return crnet_response_redirect_count(r_); }
	std::string protocol() const { return detail::str(crnet_response_protocol(r_)); }
	std::string remote_address() const { return detail::str(crnet_response_remote_address(r_)); }
	bool via_proxy() const { return crnet_response_via_proxy(r_) != 0; }
	bool content_decoded() const { return crnet_response_content_decoded(r_) != 0; }
	bool socket_reused() const { return crnet_response_socket_reused(r_) != 0; }
	uint64_t connection_id() const { return crnet_response_connection_id(r_); }
	// Microseconds since the transfer started, -1 if it did not happen.
	int64_t timing(crnet_timing which) const { return crnet_response_timing(r_, which); }
	std::string decoded_codings() const { return detail::str(crnet_response_decoded_codings(r_)); }
	int net_error() const { return crnet_response_net_error(r_); }
	std::string error_string() const { return detail::str(crnet_response_error_string(r_)); }

	// First value of |name| (case-insensitive), or "" if absent.
	std::string header(const std::string &name) const { return detail::str(crnet_response_header(r_, name.c_str())); }
	bool has_header(const std::string &name) const { return crnet_response_header(r_, name.c_str()) != nullptr; }

	std::vector<std::pair<std::string, std::string>> headers() const
	{
		std::vector<std::pair<std::string, std::string>> out;
		size_t n = crnet_response_header_count(r_);
		out.reserve(n);
		for (size_t i = 0; i < n; i++) {
			const char *k, *v;
			if (crnet_response_header_at(r_, i, &k, &v) == CRNET_OK)
				out.emplace_back(k, v);
		}
		return out;
	}

	std::string_view body() const
	{
		size_t len = 0;
		const char *p = crnet_response_body(r_, &len);
		return p ? std::string_view(p, len) : std::string_view();
	}

	const crnet_response *get() const noexcept { return r_; }

protected:
	const crnet_response *r_ = nullptr;
};

// Owning response, returned by Client::perform().
class Response : public ResponseView {
public:
	Response() = default;
	explicit Response(crnet_response *r) : ResponseView(r) {}
	~Response() { crnet_response_free(const_cast<crnet_response *>(r_)); }
	Response(Response &&o) noexcept : ResponseView(std::exchange(o.r_, nullptr)) {}
	Response &operator=(Response &&o) noexcept
	{
		if (this != &o) {
			crnet_response_free(const_cast<crnet_response *>(r_));
			r_ = std::exchange(o.r_, nullptr);
		}
		return *this;
	}
	Response(const Response &) = delete;
	Response &operator=(const Response &) = delete;
};

// Callbacks for Client::start(); all run on the client's network thread.
struct Handlers {
	// Return false to abort the transfer.
	std::function<bool(const ResponseView &)> on_response;
	// Return false to abort. When set, the body is not accumulated.
	std::function<bool(std::string_view)> on_data;
	std::function<void(int error, const ResponseView &)> on_complete;
};

class Transfer {
public:
	Transfer() = default;
	~Transfer() { reset(); }
	Transfer(Transfer &&o) noexcept : t_(std::exchange(o.t_, nullptr)), h_(std::move(o.h_)) {}
	Transfer &operator=(Transfer &&o) noexcept
	{
		if (this != &o) {
			reset();
			t_ = std::exchange(o.t_, nullptr);
			h_ = std::move(o.h_);
		}
		return *this;
	}
	Transfer(const Transfer &) = delete;
	Transfer &operator=(const Transfer &) = delete;

	explicit operator bool() const noexcept { return t_ != nullptr; }
	void cancel() { crnet_transfer_cancel(t_); }
	void pause() { crnet_transfer_pause(t_); }
	void resume() { crnet_transfer_resume(t_); }
	bool done() const { return crnet_transfer_done(t_) != 0; }
	int wait(uint64_t timeout_ms = UINT64_MAX) { return crnet_transfer_wait(t_, timeout_ms); }
	// Byte counts, -1 for an unknown total; see crnet_transfer_progress().
	int64_t progress(crnet_progress which) const { return crnet_transfer_progress(t_, which); }
	// CRNET_E_PENDING until on_complete has been entered, then the result.
	int result() const { return crnet_transfer_result(t_); }
	ResponseView response() const { return ResponseView(crnet_transfer_response(t_)); }
	Response take_response() { return Response(crnet_transfer_take_response(t_)); }

	// Waits for the response headers; throws crnet::Error on failure or
	// timeout. response() may then be inspected (status, headers).
	ResponseView wait_response(uint64_t timeout_ms = UINT64_MAX)
	{
		check(crnet_transfer_wait_response(t_, timeout_ms));
		return response();
	}

	// Streamed request body (Request::body_stream()). Blocks while the
	// send buffer is full; throws crnet::Error on failure or timeout.
	void write(std::string_view data, uint64_t timeout_ms = UINT64_MAX)
	{
		check(crnet_transfer_write(t_, data.data(), data.size(), nullptr, timeout_ms));
	}
	void write_end() { check(crnet_transfer_write_end(t_)); }

	// Streamed response body (Request::stream_response()). Returns the
	// number of bytes read, 0 at the end of the body; throws
	// crnet::Error on failure or timeout.
	size_t read(void *buf, size_t len, uint64_t timeout_ms = UINT64_MAX)
	{
		size_t n = 0;
		check(crnet_transfer_read(t_, buf, len, &n, timeout_ms));
		return n;
	}
	// Up to |max| bytes as a string; empty at the end of the body.
	std::string read(size_t max = 64 * 1024, uint64_t timeout_ms = UINT64_MAX)
	{
		std::string s(max, '\0');
		s.resize(read(s.data(), s.size(), timeout_ms));
		return s;
	}
	int fd() const { return crnet_transfer_fd(t_); }

	// Cancels if still running and waits for on_complete.
	void reset()
	{
		if (t_)
			crnet_transfer_free(std::exchange(t_, nullptr));
		h_.reset();
	}

	crnet_transfer *get() const noexcept { return t_; }

private:
	friend class Client;
	Transfer(crnet_transfer *t, std::unique_ptr<Handlers> h) : t_(t), h_(std::move(h)) {}

	static void check(int rc)
	{
		if (rc != CRNET_OK)
			throw Error(rc, crnet_strerror(rc));
	}

	crnet_transfer *t_ = nullptr;
	// Owned here so it outlives every callback: crnet_transfer_free()
	// waits for on_complete before reset() drops it.
	std::unique_ptr<Handlers> h_;
};

// Callbacks for Client::websocket(); all run on the client's network
// thread. Without on_message, messages are queued for WebSocket::recv().
struct WsHandlers {
	std::function<void(const ResponseView &handshake,
			   const std::string &protocol,
			   const std::string &extensions)> on_open;
	std::function<void(crnet_ws_msg_type, std::string_view)> on_message;
	std::function<void(int error, int code, const std::string &reason)> on_close;
};

struct WsMessage {
	crnet_ws_msg_type type;
	std::string data;
	bool is_text() const noexcept { return type == CRNET_WS_TEXT; }
};

class WebSocket {
public:
	WebSocket() = default;
	~WebSocket() { reset(); }
	WebSocket(WebSocket &&o) noexcept : ws_(std::exchange(o.ws_, nullptr)), h_(std::move(o.h_)) {}
	WebSocket &operator=(WebSocket &&o) noexcept
	{
		if (this != &o) {
			reset();
			ws_ = std::exchange(o.ws_, nullptr);
			h_ = std::move(o.h_);
		}
		return *this;
	}
	WebSocket(const WebSocket &) = delete;
	WebSocket &operator=(const WebSocket &) = delete;

	explicit operator bool() const noexcept { return ws_ != nullptr; }

	// Throws crnet::Error if the connection could not be opened.
	void wait_open(uint64_t timeout_ms = UINT64_MAX)
	{
		int rc = crnet_ws_wait_open(ws_, timeout_ms);
		if (rc != CRNET_OK)
			throw Error(rc, std::string(crnet_strerror(rc)) + " (" +
						detail::str(crnet_ws_close_reason(ws_)) + ")");
	}

	void send_text(std::string_view s) { check(crnet_ws_send(ws_, CRNET_WS_TEXT, s.data(), s.size())); }
	void send_binary(std::string_view s) { check(crnet_ws_send(ws_, CRNET_WS_BINARY, s.data(), s.size())); }

	// Next queued message (no on_message handler), std::nullopt on
	// timeout or once the connection is closed and drained.
	std::optional<WsMessage> recv(uint64_t timeout_ms = UINT64_MAX)
	{
		crnet_ws_msg_type t;
		void *d = nullptr;
		size_t n = 0;
		if (crnet_ws_recv(ws_, &t, &d, &n, timeout_ms) != CRNET_OK)
			return std::nullopt;
		WsMessage m{t, std::string(static_cast<const char *>(d), n)};
		crnet_free(d);
		return m;
	}

	void close(int code = 1000, const std::string &reason = "") { check(crnet_ws_close(ws_, code, reason.c_str())); }
	int wait_closed(uint64_t timeout_ms = UINT64_MAX) { return crnet_ws_wait_closed(ws_, timeout_ms); }
	void pause() { crnet_ws_pause(ws_); }
	void resume() { crnet_ws_resume(ws_); }

	crnet_ws_state state() const { return crnet_ws_get_state(ws_); }
	int fd() const { return crnet_ws_fd(ws_); }
	uint64_t buffered_amount() const { return crnet_ws_buffered_amount(ws_); }
	std::string protocol() const { return detail::str(crnet_ws_protocol(ws_)); }
	std::string extensions() const { return detail::str(crnet_ws_extensions(ws_)); }
	ResponseView handshake() const { return ResponseView(crnet_ws_handshake_response(ws_)); }
	int close_code() const { return crnet_ws_close_code(ws_); }
	std::string close_reason() const { return detail::str(crnet_ws_close_reason(ws_)); }

	// Aborts an open connection (no closing handshake) and waits for
	// on_close; call close() + wait_closed() first for a clean shutdown.
	void reset()
	{
		if (ws_)
			crnet_ws_free(std::exchange(ws_, nullptr));
		h_.reset();
	}

	crnet_ws *get() const noexcept { return ws_; }

private:
	friend class Client;
	WebSocket(crnet_ws *ws, std::unique_ptr<WsHandlers> h) : ws_(ws), h_(std::move(h)) {}

	static void check(int rc)
	{
		if (rc != CRNET_OK)
			throw Error(rc, crnet_strerror(rc));
	}

	crnet_ws *ws_ = nullptr;
	std::unique_ptr<WsHandlers> h_;
};

class Client {
public:
	Client() : Client(nullptr) {}
	explicit Client(const ClientConfig &cfg) : Client(cfg.get()) {}
	explicit Client(const crnet_client_config *cfg) : c_(crnet_client_new(cfg))
	{
		if (!c_)
			throw Error(CRNET_E_INVAL, "crnet_client_new failed");
	}

	// Blocking. Throws crnet::Error unless an HTTP response arrived.
	Response perform(const Request &req)
	{
		crnet_response *raw = nullptr;
		int rc = crnet_perform(get(), req.get(), &raw);
		Response resp(raw);
		if (rc != CRNET_OK) {
			std::string msg = crnet_strerror(rc);
			if (resp && resp.net_error())
				msg += " (" + resp.error_string() + ")";
			throw Error(rc, msg);
		}
		return resp;
	}

	Response get(const std::string &url) { return perform(Request(url)); }

	Response post(const std::string &url, std::string_view body,
		      const std::string &content_type = "application/octet-stream")
	{
		Request req(url, "POST");
		req.header("Content-Type", content_type).body(body);
		return perform(req);
	}

	// Asynchronous; see crnet_start().
	Transfer start(const Request &req, Handlers handlers = {})
	{
		auto h = std::make_unique<Handlers>(std::move(handlers));
		crnet_callbacks cb{};
		if (h->on_response)
			cb.on_response = [](crnet_transfer *, const crnet_response *r, void *ud) -> int {
				return static_cast<Handlers *>(ud)->on_response(ResponseView(r)) ? 0 : 1;
			};
		if (h->on_data)
			cb.on_data = [](crnet_transfer *, const void *d, size_t n, void *ud) -> int {
				return static_cast<Handlers *>(ud)->on_data(
					std::string_view(static_cast<const char *>(d), n)) ? 0 : 1;
			};
		if (h->on_complete)
			cb.on_complete = [](crnet_transfer *, int err, const crnet_response *r, void *ud) {
				static_cast<Handlers *>(ud)->on_complete(err, ResponseView(r));
			};
		crnet_transfer *t = crnet_start(get(), req.get(), &cb, h.get());
		if (!t)
			throw Error(CRNET_E_INVAL, "crnet_start failed");
		return Transfer(t, std::move(h));
	}

	void set_cookie(const std::string &url, const std::string &set_cookie)
	{
		int rc = crnet_client_set_cookie(get(), url.c_str(), set_cookie.c_str());
		if (rc != CRNET_OK)
			throw Error(rc, crnet_strerror(rc));
	}

	std::string cookies(const std::string &url)
	{
		char *s = crnet_client_get_cookies(get(), url.c_str());
		std::string out = detail::str(s);
		crnet_free(s);
		return out;
	}

	void clear_cookies() { crnet_client_clear_cookies(get()); }
	void close_idle() { crnet_client_close_idle(get()); }

	// Open a WebSocket (ws://, wss://). |protocols| is a comma separated
	// subprotocol list. Returns immediately; see WebSocket::wait_open().
	WebSocket websocket(const Request &req, const std::string &protocols = "",
			    WsHandlers handlers = {})
	{
		auto h = std::make_unique<WsHandlers>(std::move(handlers));
		crnet_ws_callbacks cb{};
		if (h->on_open)
			cb.on_open = [](crnet_ws *, const crnet_response *r, const char *p,
					const char *e, void *ud) {
				static_cast<WsHandlers *>(ud)->on_open(ResponseView(r), p, e);
			};
		if (h->on_message)
			cb.on_message = [](crnet_ws *, crnet_ws_msg_type t, const void *d,
					   size_t n, void *ud) {
				static_cast<WsHandlers *>(ud)->on_message(
					t, std::string_view(static_cast<const char *>(d), n));
			};
		if (h->on_close)
			cb.on_close = [](crnet_ws *, int err, int code, const char *reason,
					 void *ud) {
				static_cast<WsHandlers *>(ud)->on_close(err, code, reason);
			};
		crnet_ws *ws = crnet_ws_connect(get(), req.get(),
						protocols.empty() ? nullptr : protocols.c_str(),
						&cb, h.get());
		if (!ws)
			throw Error(CRNET_E_INVAL, "crnet_ws_connect failed");
		return WebSocket(ws, std::move(h));
	}

	WebSocket websocket(const std::string &url, const std::string &protocols = "")
	{
		return websocket(Request(url), protocols);
	}

	crnet_client *get() const noexcept { return c_.get(); }

private:
	struct Deleter { void operator()(crnet_client *c) const { crnet_client_free(c); } };
	std::unique_ptr<crnet_client, Deleter> c_;
};

inline std::string version() { return crnet_version(); }
inline std::string chromium_version() { return crnet_chromium_version(); }

} // namespace crnet

#endif // CRNET_CRNET_HPP
