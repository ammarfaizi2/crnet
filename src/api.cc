// SPDX-License-Identifier: BSD-3-Clause
//
// The exported C ABI. Thin argument checking and handle plumbing on top
// of crnet_client / crnet_transfer / crnet_queue.
//
#include <stdlib.h>
#include <string.h>

#include <new>

#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "crnet/internal.h"
#include "net/http/http_util.h"

namespace {

using crnet::ClientConfig;

#define CRNET_VERSION_STRING "0.1.0"

bool ValidToken(const char *s)
{
	return s && *s && net::HttpUtil::IsToken(s);
}

// Proxy URL -> Chromium proxy rules. Credentials are split off for the
// proxy auth handler (Chromium never takes them from the rules).
int ParseProxy(const char *url, ClientConfig *cfg)
{
	cfg->proxy.clear();
	cfg->proxy_user.clear();
	cfg->proxy_pass.clear();
	if (!url || !*url)
		return CRNET_OK;

	std::string_view s = url;
	size_t sep = s.find("://");
	if (sep == std::string_view::npos)
		return CRNET_E_INVAL;
	std::string scheme = base::ToLowerASCII(s.substr(0, sep));
	if (scheme == "direct")
		return CRNET_OK;
	// Chromium's socks5 always resolves names on the proxy; its socks4
	// client speaks 4a when given a name.
	if (scheme == "socks5h")
		scheme = "socks5";
	if (scheme == "socks4a")
		scheme = "socks4";
	if (scheme != "http" && scheme != "https" && scheme != "socks4" &&
	    scheme != "socks5")
		return CRNET_E_INVAL;

	// Parse authority with a scheme GURL understands.
	GURL u(base::StrCat({"http://", s.substr(sep + 3)}));
	if (!u.is_valid() || !u.has_host())
		return CRNET_E_INVAL;
	if (u.path() != "/" || u.has_query())
		return CRNET_E_INVAL;
	if (u.has_username()) {
		if (scheme == "socks4" || scheme == "socks5")
			return CRNET_E_INVAL;	// unsupported by Chromium
		cfg->proxy_user = base::UnescapeURLComponent(
			u.username(), base::UnescapeRule::NORMAL);
		cfg->proxy_pass = base::UnescapeURLComponent(
			u.password(), base::UnescapeRule::NORMAL);
	}
	cfg->proxy = base::StrCat({scheme, "://", u.host()});
	if (u.has_port())
		base::StrAppend(&cfg->proxy, {":", u.port()});
	return CRNET_OK;
}

int StoreString(std::string *dst, const char *src, const std::string &def)
{
	*dst = src ? src : def;
	return CRNET_OK;
}

} // namespace

extern "C" {

// ------------------------------------------------------------------
// Library
// ------------------------------------------------------------------

CRNET_API const char *crnet_version(void)
{
	return CRNET_VERSION_STRING " (Chromium " CRNET_CHROMIUM_VERSION ")";
}

CRNET_API const char *crnet_chromium_version(void)
{
	return CRNET_CHROMIUM_VERSION;
}

CRNET_API const char *crnet_strerror(int err)
{
	switch (err) {
	case CRNET_OK:			return "success";
	case CRNET_E_INVAL:		return "invalid argument";
	case CRNET_E_NOMEM:		return "out of memory";
	case CRNET_E_NET:		return "network error";
	case CRNET_E_TIMEOUT:		return "timed out";
	case CRNET_E_CANCELED:		return "canceled";
	case CRNET_E_SHUTDOWN:		return "client shut down";
	case CRNET_E_TOO_LARGE:	return "response body too large";
	case CRNET_E_WOULD_DEADLOCK:	return "blocking call on a crnet network thread";
	case CRNET_E_STATE:		return "invalid state";
	case CRNET_E_CALLBACK:		return "aborted by callback";
	}
	return "unknown error";
}

CRNET_API const char *crnet_default_user_agent(void)
{
	return crnet::DefaultUserAgent().c_str();
}

CRNET_API const char *crnet_default_sec_ch_ua(void)
{
	return crnet::DefaultSecChUa().c_str();
}

CRNET_API int crnet_global_init(const char *enable_features,
				  const char *disable_features)
{
	bool already = false;
	if (!crnet::EnsureGlobalInit(enable_features, disable_features,
				      &already))
		return CRNET_E_STATE;
	return already ? CRNET_E_STATE : CRNET_OK;
}

CRNET_API void crnet_free(void *ptr)
{
	free(ptr);
}

// ------------------------------------------------------------------
// Client configuration
// ------------------------------------------------------------------

CRNET_API crnet_client_config *crnet_client_config_new(void)
{
	return new (std::nothrow) crnet_client_config();
}

CRNET_API void crnet_client_config_free(crnet_client_config *cfg)
{
	delete cfg;
}

CRNET_API int crnet_client_config_set_proxy(crnet_client_config *cfg,
					      const char *proxy_url)
{
	if (!cfg)
		return CRNET_E_INVAL;
	ClientConfig tmp = cfg->cfg;
	int rc = ParseProxy(proxy_url, &tmp);
	if (rc == CRNET_OK)
		cfg->cfg = std::move(tmp);
	return rc;
}

CRNET_API int crnet_client_config_set_user_agent(crnet_client_config *cfg,
						   const char *ua)
{
	if (!cfg)
		return CRNET_E_INVAL;
	return StoreString(&cfg->cfg.user_agent, ua, "");
}

CRNET_API int crnet_client_config_set_sec_ch_ua(crnet_client_config *cfg,
						  const char *v)
{
	if (!cfg)
		return CRNET_E_INVAL;
	return StoreString(&cfg->cfg.sec_ch_ua, v, "");
}

CRNET_API int crnet_client_config_set_platform(crnet_client_config *cfg,
						 const char *v)
{
	if (!cfg)
		return CRNET_E_INVAL;
	return StoreString(&cfg->cfg.platform, v, "Linux");
}

CRNET_API int crnet_client_config_set_accept_language(
	crnet_client_config *cfg, const char *v)
{
	if (!cfg)
		return CRNET_E_INVAL;
	return StoreString(&cfg->cfg.accept_language, v, "en-US,en;q=0.9");
}

CRNET_API void crnet_client_config_set_http2(crnet_client_config *cfg,
					       int enable)
{
	if (cfg)
		cfg->cfg.http2 = !!enable;
}

CRNET_API void crnet_client_config_set_quic(crnet_client_config *cfg,
					      int enable)
{
	if (cfg)
		cfg->cfg.quic = !!enable;
}

CRNET_API void crnet_client_config_set_builtin_dns(crnet_client_config *cfg,
						     int enable)
{
	if (cfg)
		cfg->cfg.builtin_dns = !!enable;
}

CRNET_API void crnet_client_config_set_cookies(crnet_client_config *cfg,
						 int enable)
{
	if (cfg)
		cfg->cfg.cookies = !!enable;
}

CRNET_API void crnet_client_config_set_insecure(crnet_client_config *cfg,
						  int enable)
{
	if (cfg)
		cfg->cfg.insecure = !!enable;
}

CRNET_API int crnet_client_config_set_host_rules(crnet_client_config *cfg,
						   const char *rules)
{
	if (!cfg)
		return CRNET_E_INVAL;
	return StoreString(&cfg->cfg.host_rules, rules, "");
}

CRNET_API void crnet_client_config_set_timeout(crnet_client_config *cfg,
						 uint64_t timeout_ms)
{
	if (cfg)
		cfg->cfg.timeout_ms = timeout_ms;
}

CRNET_API int crnet_client_config_set_netlog(crnet_client_config *cfg,
					       const char *path)
{
	if (!cfg)
		return CRNET_E_INVAL;
	return StoreString(&cfg->cfg.netlog_path, path, "");
}

// ------------------------------------------------------------------
// Client
// ------------------------------------------------------------------

CRNET_API crnet_client *crnet_client_new(const crnet_client_config *cfg)
{
	if (!crnet::EnsureGlobalInit(nullptr, nullptr, nullptr))
		return nullptr;
	ClientConfig c = cfg ? cfg->cfg : ClientConfig();
	return crnet_client::Create(c);
}

CRNET_API void crnet_client_free(crnet_client *client)
{
	if (!client || client->IsNetworkThread())
		return;
	client->Destroy();
}

CRNET_API int crnet_client_set_cookie(crnet_client *client, const char *url,
					const char *set_cookie)
{
	if (!client || !url || !set_cookie)
		return CRNET_E_INVAL;
	if (crnet::OnNetworkThread())
		return CRNET_E_WOULD_DEADLOCK;
	return client->SetCookie(url, set_cookie);
}

CRNET_API char *crnet_client_get_cookies(crnet_client *client,
					   const char *url)
{
	if (!client || !url || crnet::OnNetworkThread())
		return nullptr;
	std::optional<std::string> line = client->GetCookies(url);
	if (!line)
		return nullptr;
	return strdup(line->c_str());
}

CRNET_API int crnet_client_clear_cookies(crnet_client *client)
{
	if (!client)
		return CRNET_E_INVAL;
	if (crnet::OnNetworkThread())
		return CRNET_E_WOULD_DEADLOCK;
	return client->ClearCookies();
}

CRNET_API void crnet_client_close_idle(crnet_client *client)
{
	if (client)
		client->CloseIdle();
}

// ------------------------------------------------------------------
// Request
// ------------------------------------------------------------------

CRNET_API crnet_request *crnet_request_new(const char *method,
					      const char *url)
{
	auto *req = new (std::nothrow) crnet_request();
	if (!req)
		return nullptr;
	if (crnet_request_set_url(req, url) != CRNET_OK ||
	    crnet_request_set_method(req, method ? method : "GET") !=
		    CRNET_OK) {
		delete req;
		return nullptr;
	}
	return req;
}

CRNET_API void crnet_request_free(crnet_request *req)
{
	delete req;
}

CRNET_API int crnet_request_set_url(crnet_request *req, const char *url)
{
	if (!req || !url)
		return CRNET_E_INVAL;
	GURL u(url);
	if (!u.is_valid() || !u.SchemeIsHTTPOrHTTPS())
		return CRNET_E_INVAL;
	req->spec.url = std::move(u);
	return CRNET_OK;
}

CRNET_API int crnet_request_set_method(crnet_request *req,
					 const char *method)
{
	if (!req || !ValidToken(method))
		return CRNET_E_INVAL;
	// Chromium normalises the method case like browsers do.
	req->spec.method = method;
	return CRNET_OK;
}

CRNET_API int crnet_request_set_header(crnet_request *req,
					 const char *name, const char *value)
{
	if (!req || !ValidToken(name))
		return CRNET_E_INVAL;
	if (value && !net::HttpUtil::IsValidHeaderValue(value))
		return CRNET_E_INVAL;
	auto &hs = req->spec.headers;
	for (auto &h : hs) {
		if (base::EqualsCaseInsensitiveASCII(h.first, name)) {
			h.first = name;
			h.second = value ? std::optional<std::string>(value)
					 : std::nullopt;
			return CRNET_OK;
		}
	}
	hs.emplace_back(name, value ? std::optional<std::string>(value)
				    : std::nullopt);
	return CRNET_OK;
}

CRNET_API int crnet_request_set_body(crnet_request *req, const void *data,
				       size_t len)
{
	if (!req || (!data && len))
		return CRNET_E_INVAL;
	if (!data)
		req->spec.body.reset();
	else
		req->spec.body.emplace(static_cast<const char *>(data), len);
	return CRNET_OK;
}

CRNET_API int crnet_request_set_mode(crnet_request *req, crnet_mode mode)
{
	if (!req || mode < CRNET_MODE_NAVIGATE || mode > CRNET_MODE_RAW)
		return CRNET_E_INVAL;
	req->spec.mode = mode;
	return CRNET_OK;
}

CRNET_API int crnet_request_set_priority(crnet_request *req,
					   crnet_priority priority)
{
	if (!req || priority < CRNET_PRIORITY_DEFAULT ||
	    priority > CRNET_PRIORITY_HIGHEST)
		return CRNET_E_INVAL;
	req->spec.priority = priority;
	return CRNET_OK;
}

CRNET_API int crnet_request_set_max_redirects(crnet_request *req,
						int max_redirects)
{
	if (!req || max_redirects < 0)
		return CRNET_E_INVAL;
	req->spec.max_redirects = max_redirects;
	return CRNET_OK;
}

CRNET_API int crnet_request_set_timeout(crnet_request *req,
					  uint64_t timeout_ms)
{
	if (!req)
		return CRNET_E_INVAL;
	req->spec.timeout_ms = timeout_ms;
	return CRNET_OK;
}

CRNET_API int crnet_request_set_decompress(crnet_request *req, int enable)
{
	if (!req)
		return CRNET_E_INVAL;
	req->spec.decompress = !!enable;
	return CRNET_OK;
}

CRNET_API int crnet_request_set_max_body_size(crnet_request *req,
						uint64_t max_bytes)
{
	if (!req)
		return CRNET_E_INVAL;
	req->spec.max_body = max_bytes;
	return CRNET_OK;
}

// ------------------------------------------------------------------
// Transfers
// ------------------------------------------------------------------

static crnet_transfer *StartTransfer(crnet_client *client,
				      const crnet_request *req,
				      const crnet_callbacks *cb,
				      void *user_data, crnet_queue *q)
{
	if (!client || !req)
		return nullptr;
	auto t = base::MakeRefCounted<crnet_transfer>(client, req->spec, cb,
						       user_data, q);
	if (q)
		q->Started(t.get());
	if (!client->Start(t)) {
		if (q)
			q->Completed(t.get());
		return nullptr;
	}
	// The caller's handle reference.
	return t.release();
}

CRNET_API crnet_transfer *crnet_start(crnet_client *client,
					 const crnet_request *req,
					 const crnet_callbacks *cb,
					 void *user_data)
{
	return StartTransfer(client, req, cb, user_data, nullptr);
}

CRNET_API int crnet_perform(crnet_client *client, const crnet_request *req,
			      crnet_response **resp)
{
	if (resp)
		*resp = nullptr;
	if (!client || !req || !resp)
		return CRNET_E_INVAL;
	if (crnet::OnNetworkThread())
		return CRNET_E_WOULD_DEADLOCK;
	crnet_transfer *t = StartTransfer(client, req, nullptr, nullptr,
					   nullptr);
	if (!t)
		return CRNET_E_SHUTDOWN;
	int rc = t->Wait(UINT64_MAX);
	*resp = t->TakeResponse();
	t->Release();
	return rc;
}

CRNET_API void crnet_transfer_cancel(crnet_transfer *t)
{
	if (t)
		t->Cancel(CRNET_E_CANCELED);
}

CRNET_API void crnet_transfer_pause(crnet_transfer *t)
{
	if (t)
		t->Pause();
}

CRNET_API void crnet_transfer_resume(crnet_transfer *t)
{
	if (t)
		t->Resume();
}

CRNET_API int crnet_transfer_wait(crnet_transfer *t, uint64_t timeout_ms)
{
	if (!t)
		return CRNET_E_INVAL;
	if (!t->done() && crnet::OnNetworkThread())
		return CRNET_E_WOULD_DEADLOCK;
	return t->Wait(timeout_ms);
}

CRNET_API int crnet_transfer_done(const crnet_transfer *t)
{
	return t && t->done();
}

CRNET_API const crnet_response *crnet_transfer_response(
	const crnet_transfer *t)
{
	return t ? t->response() : nullptr;
}

CRNET_API crnet_response *crnet_transfer_take_response(crnet_transfer *t)
{
	return t ? t->TakeResponse() : nullptr;
}

CRNET_API void crnet_transfer_free(crnet_transfer *t)
{
	if (!t)
		return;
	if (!t->done()) {
		t->Cancel(CRNET_E_CANCELED);
		// From another thread, wait so user_data may be freed after
		// we return. On the network thread (inside a callback) the
		// cancel above already completed synchronously, except when
		// called from this transfer's own callback, where waiting
		// would deadlock; the running reference keeps it alive.
		if (!crnet::OnNetworkThread())
			t->Wait(UINT64_MAX);
	}
	t->Release();
}

// ------------------------------------------------------------------
// Completion queue
// ------------------------------------------------------------------

CRNET_API crnet_queue *crnet_queue_new(void)
{
	auto *q = new (std::nothrow) crnet_queue();
	if (q && !q->ok()) {
		delete q;
		return nullptr;
	}
	return q;
}

CRNET_API void crnet_queue_free(crnet_queue *q)
{
	if (!q)
		return;
	q->Drain();
	delete q;
}

CRNET_API int crnet_queue_fd(const crnet_queue *q)
{
	return q ? q->fd() : -1;
}

CRNET_API int crnet_queue_start(crnet_queue *q, crnet_client *client,
				  const crnet_request *req, void *user_data)
{
	if (!q || !client || !req)
		return CRNET_E_INVAL;
	crnet_transfer *t = StartTransfer(client, req, nullptr, user_data, q);
	if (!t)
		return CRNET_E_SHUTDOWN;
	// The queue holds its own reference; drop the handle one.
	t->Release();
	return CRNET_OK;
}

CRNET_API int crnet_queue_pop(crnet_queue *q, crnet_transfer **t,
				void **user_data, uint64_t timeout_ms)
{
	if (!q || !t)
		return CRNET_E_INVAL;
	*t = nullptr;
	return q->Pop(t, user_data, timeout_ms);
}

CRNET_API size_t crnet_queue_pending(const crnet_queue *q)
{
	return q ? q->pending() : 0;
}

// ------------------------------------------------------------------
// Response
// ------------------------------------------------------------------

CRNET_API void crnet_response_free(crnet_response *resp)
{
	delete resp;
}

CRNET_API int crnet_response_status(const crnet_response *r)
{
	return r ? r->status : 0;
}

CRNET_API const char *crnet_response_status_text(const crnet_response *r)
{
	return r ? r->status_text.c_str() : "";
}

CRNET_API const char *crnet_response_url(const crnet_response *r)
{
	return r ? r->url.c_str() : "";
}

CRNET_API int crnet_response_redirect_count(const crnet_response *r)
{
	return r ? r->redirects : 0;
}

CRNET_API size_t crnet_response_header_count(const crnet_response *r)
{
	return r ? r->headers.size() : 0;
}

CRNET_API int crnet_response_header_at(const crnet_response *r,
					 size_t index, const char **name,
					 const char **value)
{
	if (!r || index >= r->headers.size())
		return CRNET_E_INVAL;
	if (name)
		*name = r->headers[index].first.c_str();
	if (value)
		*value = r->headers[index].second.c_str();
	return CRNET_OK;
}

CRNET_API const char *crnet_response_header(const crnet_response *r,
					      const char *name)
{
	if (!r || !name)
		return nullptr;
	for (const auto &h : r->headers)
		if (base::EqualsCaseInsensitiveASCII(h.first, name))
			return h.second.c_str();
	return nullptr;
}

CRNET_API const char *crnet_response_body(const crnet_response *r,
					    size_t *len)
{
	if (!r) {
		if (len)
			*len = 0;
		return nullptr;
	}
	if (len)
		*len = r->body.size();
	return r->body.c_str();
}

CRNET_API const char *crnet_response_protocol(const crnet_response *r)
{
	return r ? r->protocol.c_str() : "";
}

CRNET_API const char *crnet_response_remote_address(const crnet_response *r)
{
	return r ? r->remote.c_str() : "";
}

CRNET_API int crnet_response_via_proxy(const crnet_response *r)
{
	return r && r->via_proxy;
}

CRNET_API int crnet_response_net_error(const crnet_response *r)
{
	return r ? r->net_error : 0;
}

CRNET_API const char *crnet_response_error_string(const crnet_response *r)
{
	return r ? r->net_error_str.c_str() : "";
}

} // extern "C"
