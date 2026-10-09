// SPDX-License-Identifier: BSD-3-Clause
//
// Header-only C++17 convenience layer over the crhttp C API.
//
// Everything here is inline and talks to libcrhttp.so only through the
// C ABI, so it works with any C++ standard library (libcrhttp.so uses
// its own private copy of libc++ internally).
//
//   crhttp::Client client(crhttp::ClientConfig().proxy("socks5://..."));
//   crhttp::Response r = client.get("https://example.com/");
//   std::cout << r.status() << "\n" << r.body();
//
#ifndef CRHTTP_CRHTTP_HPP
#define CRHTTP_CRHTTP_HPP

#include "crhttp.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace crhttp {

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
	ClientConfig() : cfg_(crhttp_client_config_new())
	{
		if (!cfg_)
			throw Error(CRHTTP_E_NOMEM, "crhttp_client_config_new");
	}

	ClientConfig &proxy(const std::string &url) { check(crhttp_client_config_set_proxy(get(), url.c_str())); return *this; }
	ClientConfig &user_agent(const std::string &ua) { check(crhttp_client_config_set_user_agent(get(), ua.c_str())); return *this; }
	ClientConfig &sec_ch_ua(const std::string &v) { check(crhttp_client_config_set_sec_ch_ua(get(), v.c_str())); return *this; }
	ClientConfig &platform(const std::string &v) { check(crhttp_client_config_set_platform(get(), v.c_str())); return *this; }
	ClientConfig &accept_language(const std::string &v) { check(crhttp_client_config_set_accept_language(get(), v.c_str())); return *this; }
	ClientConfig &http2(bool on) { crhttp_client_config_set_http2(get(), on); return *this; }
	ClientConfig &quic(bool on) { crhttp_client_config_set_quic(get(), on); return *this; }
	ClientConfig &builtin_dns(bool on) { crhttp_client_config_set_builtin_dns(get(), on); return *this; }
	ClientConfig &cookies(bool on) { crhttp_client_config_set_cookies(get(), on); return *this; }
	ClientConfig &insecure(bool on) { crhttp_client_config_set_insecure(get(), on); return *this; }
	ClientConfig &host_rules(const std::string &r) { check(crhttp_client_config_set_host_rules(get(), r.c_str())); return *this; }
	ClientConfig &timeout_ms(uint64_t ms) { crhttp_client_config_set_timeout(get(), ms); return *this; }
	ClientConfig &netlog(const std::string &path) { check(crhttp_client_config_set_netlog(get(), path.c_str())); return *this; }

	crhttp_client_config *get() const noexcept { return cfg_.get(); }

private:
	static void check(int rc)
	{
		if (rc != CRHTTP_OK)
			throw Error(rc, crhttp_strerror(rc));
	}

	struct Deleter { void operator()(crhttp_client_config *c) const { crhttp_client_config_free(c); } };
	std::unique_ptr<crhttp_client_config, Deleter> cfg_;
};

class Request {
public:
	explicit Request(const std::string &url, const std::string &method = "GET")
		: req_(crhttp_request_new(method.c_str(), url.c_str()))
	{
		if (!req_)
			throw Error(CRHTTP_E_INVAL, "invalid URL: " + url);
	}

	Request &method(const std::string &m) { check(crhttp_request_set_method(get(), m.c_str())); return *this; }
	Request &url(const std::string &u) { check(crhttp_request_set_url(get(), u.c_str())); return *this; }
	Request &header(const std::string &name, const std::string &value) { check(crhttp_request_set_header(get(), name.c_str(), value.c_str())); return *this; }
	Request &remove_header(const std::string &name) { check(crhttp_request_set_header(get(), name.c_str(), nullptr)); return *this; }
	Request &body(std::string_view data) { check(crhttp_request_set_body(get(), data.data(), data.size())); return *this; }
	Request &mode(crhttp_mode m) { check(crhttp_request_set_mode(get(), m)); return *this; }
	Request &priority(crhttp_priority p) { check(crhttp_request_set_priority(get(), p)); return *this; }
	Request &max_redirects(int n) { check(crhttp_request_set_max_redirects(get(), n)); return *this; }
	Request &timeout_ms(uint64_t ms) { check(crhttp_request_set_timeout(get(), ms)); return *this; }
	Request &decompress(bool on) { check(crhttp_request_set_decompress(get(), on)); return *this; }
	Request &max_body_size(uint64_t n) { check(crhttp_request_set_max_body_size(get(), n)); return *this; }

	crhttp_request *get() const noexcept { return req_.get(); }

private:
	static void check(int rc)
	{
		if (rc != CRHTTP_OK)
			throw Error(rc, crhttp_strerror(rc));
	}

	struct Deleter { void operator()(crhttp_request *r) const { crhttp_request_free(r); } };
	std::unique_ptr<crhttp_request, Deleter> req_;
};

// Non-owning view of a crhttp_response; see Response for the owner.
class ResponseView {
public:
	ResponseView() = default;
	explicit ResponseView(const crhttp_response *r) : r_(r) {}

	explicit operator bool() const noexcept { return r_ != nullptr; }
	int status() const { return crhttp_response_status(r_); }
	std::string status_text() const { return detail::str(crhttp_response_status_text(r_)); }
	std::string url() const { return detail::str(crhttp_response_url(r_)); }
	int redirect_count() const { return crhttp_response_redirect_count(r_); }
	std::string protocol() const { return detail::str(crhttp_response_protocol(r_)); }
	std::string remote_address() const { return detail::str(crhttp_response_remote_address(r_)); }
	bool via_proxy() const { return crhttp_response_via_proxy(r_) != 0; }
	int net_error() const { return crhttp_response_net_error(r_); }
	std::string error_string() const { return detail::str(crhttp_response_error_string(r_)); }

	// First value of |name| (case-insensitive), or "" if absent.
	std::string header(const std::string &name) const { return detail::str(crhttp_response_header(r_, name.c_str())); }
	bool has_header(const std::string &name) const { return crhttp_response_header(r_, name.c_str()) != nullptr; }

	std::vector<std::pair<std::string, std::string>> headers() const
	{
		std::vector<std::pair<std::string, std::string>> out;
		size_t n = crhttp_response_header_count(r_);
		out.reserve(n);
		for (size_t i = 0; i < n; i++) {
			const char *k, *v;
			if (crhttp_response_header_at(r_, i, &k, &v) == CRHTTP_OK)
				out.emplace_back(k, v);
		}
		return out;
	}

	std::string_view body() const
	{
		size_t len = 0;
		const char *p = crhttp_response_body(r_, &len);
		return p ? std::string_view(p, len) : std::string_view();
	}

	const crhttp_response *get() const noexcept { return r_; }

protected:
	const crhttp_response *r_ = nullptr;
};

// Owning response, returned by Client::perform().
class Response : public ResponseView {
public:
	Response() = default;
	explicit Response(crhttp_response *r) : ResponseView(r) {}
	~Response() { crhttp_response_free(const_cast<crhttp_response *>(r_)); }
	Response(Response &&o) noexcept : ResponseView(std::exchange(o.r_, nullptr)) {}
	Response &operator=(Response &&o) noexcept
	{
		if (this != &o) {
			crhttp_response_free(const_cast<crhttp_response *>(r_));
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
	void cancel() { crhttp_transfer_cancel(t_); }
	void pause() { crhttp_transfer_pause(t_); }
	void resume() { crhttp_transfer_resume(t_); }
	bool done() const { return crhttp_transfer_done(t_) != 0; }
	int wait(uint64_t timeout_ms = UINT64_MAX) { return crhttp_transfer_wait(t_, timeout_ms); }
	ResponseView response() const { return ResponseView(crhttp_transfer_response(t_)); }
	Response take_response() { return Response(crhttp_transfer_take_response(t_)); }

	// Cancels if still running and waits for on_complete.
	void reset()
	{
		if (t_)
			crhttp_transfer_free(std::exchange(t_, nullptr));
		h_.reset();
	}

	crhttp_transfer *get() const noexcept { return t_; }

private:
	friend class Client;
	Transfer(crhttp_transfer *t, std::unique_ptr<Handlers> h) : t_(t), h_(std::move(h)) {}

	crhttp_transfer *t_ = nullptr;
	// Owned here so it outlives every callback: crhttp_transfer_free()
	// waits for on_complete before reset() drops it.
	std::unique_ptr<Handlers> h_;
};

class Client {
public:
	Client() : Client(nullptr) {}
	explicit Client(const ClientConfig &cfg) : Client(cfg.get()) {}
	explicit Client(const crhttp_client_config *cfg) : c_(crhttp_client_new(cfg))
	{
		if (!c_)
			throw Error(CRHTTP_E_INVAL, "crhttp_client_new failed");
	}

	// Blocking. Throws crhttp::Error unless an HTTP response arrived.
	Response perform(const Request &req)
	{
		crhttp_response *raw = nullptr;
		int rc = crhttp_perform(get(), req.get(), &raw);
		Response resp(raw);
		if (rc != CRHTTP_OK) {
			std::string msg = crhttp_strerror(rc);
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

	// Asynchronous; see crhttp_start().
	Transfer start(const Request &req, Handlers handlers = {})
	{
		auto h = std::make_unique<Handlers>(std::move(handlers));
		crhttp_callbacks cb{};
		if (h->on_response)
			cb.on_response = [](crhttp_transfer *, const crhttp_response *r, void *ud) -> int {
				return static_cast<Handlers *>(ud)->on_response(ResponseView(r)) ? 0 : 1;
			};
		if (h->on_data)
			cb.on_data = [](crhttp_transfer *, const void *d, size_t n, void *ud) -> int {
				return static_cast<Handlers *>(ud)->on_data(
					std::string_view(static_cast<const char *>(d), n)) ? 0 : 1;
			};
		if (h->on_complete)
			cb.on_complete = [](crhttp_transfer *, int err, const crhttp_response *r, void *ud) {
				static_cast<Handlers *>(ud)->on_complete(err, ResponseView(r));
			};
		crhttp_transfer *t = crhttp_start(get(), req.get(), &cb, h.get());
		if (!t)
			throw Error(CRHTTP_E_INVAL, "crhttp_start failed");
		return Transfer(t, std::move(h));
	}

	void set_cookie(const std::string &url, const std::string &set_cookie)
	{
		int rc = crhttp_client_set_cookie(get(), url.c_str(), set_cookie.c_str());
		if (rc != CRHTTP_OK)
			throw Error(rc, crhttp_strerror(rc));
	}

	std::string cookies(const std::string &url)
	{
		char *s = crhttp_client_get_cookies(get(), url.c_str());
		std::string out = detail::str(s);
		crhttp_free(s);
		return out;
	}

	void clear_cookies() { crhttp_client_clear_cookies(get()); }
	void close_idle() { crhttp_client_close_idle(get()); }

	crhttp_client *get() const noexcept { return c_.get(); }

private:
	struct Deleter { void operator()(crhttp_client *c) const { crhttp_client_free(c); } };
	std::unique_ptr<crhttp_client, Deleter> c_;
};

inline std::string version() { return crhttp_version(); }
inline std::string chromium_version() { return crhttp_chromium_version(); }

} // namespace crhttp

#endif // CRHTTP_CRHTTP_HPP
