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
#ifndef CRNET_CRNET_HPP
#define CRNET_CRNET_HPP

#include "crnet.h"

#include <cstdint>
#include <functional>
#include <memory>
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
	ResponseView response() const { return ResponseView(crnet_transfer_response(t_)); }
	Response take_response() { return Response(crnet_transfer_take_response(t_)); }

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

	crnet_transfer *t_ = nullptr;
	// Owned here so it outlives every callback: crnet_transfer_free()
	// waits for on_complete before reset() drops it.
	std::unique_ptr<Handlers> h_;
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

	crnet_client *get() const noexcept { return c_.get(); }

private:
	struct Deleter { void operator()(crnet_client *c) const { crnet_client_free(c); } };
	std::unique_ptr<crnet_client, Deleter> c_;
};

inline std::string version() { return crnet_version(); }
inline std::string chromium_version() { return crnet_chromium_version(); }

} // namespace crnet

#endif // CRNET_CRNET_HPP
