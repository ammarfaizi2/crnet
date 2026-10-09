// SPDX-License-Identifier: BSD-3-Clause
//
// The exported C ABI. Thin argument checking and handle plumbing on top
// of crhttp_client / crhttp_transfer / crhttp_queue.
//
#include <stdlib.h>
#include <string.h>

#include <new>

#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "crhttp/internal.h"
#include "net/http/http_util.h"

namespace {

using crhttp::ClientConfig;

#define CRHTTP_VERSION_STRING "0.1.0"

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
		return CRHTTP_OK;

	std::string_view s = url;
	size_t sep = s.find("://");
	if (sep == std::string_view::npos)
		return CRHTTP_E_INVAL;
	std::string scheme = base::ToLowerASCII(s.substr(0, sep));
	if (scheme == "direct")
		return CRHTTP_OK;
	// Chromium's socks5 always resolves names on the proxy; its socks4
	// client speaks 4a when given a name.
	if (scheme == "socks5h")
		scheme = "socks5";
	if (scheme == "socks4a")
		scheme = "socks4";
	if (scheme != "http" && scheme != "https" && scheme != "socks4" &&
	    scheme != "socks5")
		return CRHTTP_E_INVAL;

	// Parse authority with a scheme GURL understands.
	GURL u(base::StrCat({"http://", s.substr(sep + 3)}));
	if (!u.is_valid() || !u.has_host())
		return CRHTTP_E_INVAL;
	if (u.path() != "/" || u.has_query())
		return CRHTTP_E_INVAL;
	if (u.has_username()) {
		if (scheme == "socks4" || scheme == "socks5")
			return CRHTTP_E_INVAL;	// unsupported by Chromium
		cfg->proxy_user = base::UnescapeURLComponent(
			u.username(), base::UnescapeRule::NORMAL);
		cfg->proxy_pass = base::UnescapeURLComponent(
			u.password(), base::UnescapeRule::NORMAL);
	}
	cfg->proxy = base::StrCat({scheme, "://", u.host()});
	if (u.has_port())
		base::StrAppend(&cfg->proxy, {":", u.port()});
	return CRHTTP_OK;
}

int StoreString(std::string *dst, const char *src, const std::string &def)
{
	*dst = src ? src : def;
	return CRHTTP_OK;
}

} // namespace

extern "C" {

// ------------------------------------------------------------------
// Library
// ------------------------------------------------------------------

CRHTTP_API const char *crhttp_version(void)
{
	return CRHTTP_VERSION_STRING " (Chromium " CRHTTP_CHROMIUM_VERSION ")";
}

CRHTTP_API const char *crhttp_chromium_version(void)
{
	return CRHTTP_CHROMIUM_VERSION;
}

CRHTTP_API const char *crhttp_strerror(int err)
{
	switch (err) {
	case CRHTTP_OK:			return "success";
	case CRHTTP_E_INVAL:		return "invalid argument";
	case CRHTTP_E_NOMEM:		return "out of memory";
	case CRHTTP_E_NET:		return "network error";
	case CRHTTP_E_TIMEOUT:		return "timed out";
	case CRHTTP_E_CANCELED:		return "canceled";
	case CRHTTP_E_SHUTDOWN:		return "client shut down";
	case CRHTTP_E_TOO_LARGE:	return "response body too large";
	case CRHTTP_E_WOULD_DEADLOCK:	return "blocking call on a crhttp network thread";
	case CRHTTP_E_STATE:		return "invalid state";
	case CRHTTP_E_CALLBACK:		return "aborted by callback";
	}
	return "unknown error";
}

CRHTTP_API const char *crhttp_default_user_agent(void)
{
	return crhttp::DefaultUserAgent().c_str();
}

CRHTTP_API const char *crhttp_default_sec_ch_ua(void)
{
	return crhttp::DefaultSecChUa().c_str();
}

CRHTTP_API int crhttp_global_init(const char *enable_features,
				  const char *disable_features)
{
	bool already = false;
	if (!crhttp::EnsureGlobalInit(enable_features, disable_features,
				      &already))
		return CRHTTP_E_STATE;
	return already ? CRHTTP_E_STATE : CRHTTP_OK;
}

CRHTTP_API void crhttp_free(void *ptr)
{
	free(ptr);
}

// ------------------------------------------------------------------
// Client configuration
// ------------------------------------------------------------------

CRHTTP_API crhttp_client_config *crhttp_client_config_new(void)
{
	return new (std::nothrow) crhttp_client_config();
}

CRHTTP_API void crhttp_client_config_free(crhttp_client_config *cfg)
{
	delete cfg;
}

CRHTTP_API int crhttp_client_config_set_proxy(crhttp_client_config *cfg,
					      const char *proxy_url)
{
	if (!cfg)
		return CRHTTP_E_INVAL;
	ClientConfig tmp = cfg->cfg;
	int rc = ParseProxy(proxy_url, &tmp);
	if (rc == CRHTTP_OK)
		cfg->cfg = std::move(tmp);
	return rc;
}

CRHTTP_API int crhttp_client_config_set_user_agent(crhttp_client_config *cfg,
						   const char *ua)
{
	if (!cfg)
		return CRHTTP_E_INVAL;
	return StoreString(&cfg->cfg.user_agent, ua, "");
}

CRHTTP_API int crhttp_client_config_set_sec_ch_ua(crhttp_client_config *cfg,
						  const char *v)
{
	if (!cfg)
		return CRHTTP_E_INVAL;
	return StoreString(&cfg->cfg.sec_ch_ua, v, "");
}

CRHTTP_API int crhttp_client_config_set_platform(crhttp_client_config *cfg,
						 const char *v)
{
	if (!cfg)
		return CRHTTP_E_INVAL;
	return StoreString(&cfg->cfg.platform, v, "Linux");
}

CRHTTP_API int crhttp_client_config_set_accept_language(
	crhttp_client_config *cfg, const char *v)
{
	if (!cfg)
		return CRHTTP_E_INVAL;
	return StoreString(&cfg->cfg.accept_language, v, "en-US,en;q=0.9");
}

CRHTTP_API void crhttp_client_config_set_http2(crhttp_client_config *cfg,
					       int enable)
{
	if (cfg)
		cfg->cfg.http2 = !!enable;
}

CRHTTP_API void crhttp_client_config_set_quic(crhttp_client_config *cfg,
					      int enable)
{
	if (cfg)
		cfg->cfg.quic = !!enable;
}

CRHTTP_API void crhttp_client_config_set_builtin_dns(crhttp_client_config *cfg,
						     int enable)
{
	if (cfg)
		cfg->cfg.builtin_dns = !!enable;
}

CRHTTP_API void crhttp_client_config_set_cookies(crhttp_client_config *cfg,
						 int enable)
{
	if (cfg)
		cfg->cfg.cookies = !!enable;
}

CRHTTP_API void crhttp_client_config_set_insecure(crhttp_client_config *cfg,
						  int enable)
{
	if (cfg)
		cfg->cfg.insecure = !!enable;
}

CRHTTP_API int crhttp_client_config_set_host_rules(crhttp_client_config *cfg,
						   const char *rules)
{
	if (!cfg)
		return CRHTTP_E_INVAL;
	return StoreString(&cfg->cfg.host_rules, rules, "");
}

CRHTTP_API void crhttp_client_config_set_timeout(crhttp_client_config *cfg,
						 uint64_t timeout_ms)
{
	if (cfg)
		cfg->cfg.timeout_ms = timeout_ms;
}

CRHTTP_API int crhttp_client_config_set_netlog(crhttp_client_config *cfg,
					       const char *path)
{
	if (!cfg)
		return CRHTTP_E_INVAL;
	return StoreString(&cfg->cfg.netlog_path, path, "");
}

// ------------------------------------------------------------------
// Client
// ------------------------------------------------------------------

CRHTTP_API crhttp_client *crhttp_client_new(const crhttp_client_config *cfg)
{
	if (!crhttp::EnsureGlobalInit(nullptr, nullptr, nullptr))
		return nullptr;
	ClientConfig c = cfg ? cfg->cfg : ClientConfig();
	return crhttp_client::Create(c);
}

CRHTTP_API void crhttp_client_free(crhttp_client *client)
{
	if (!client || client->IsNetworkThread())
		return;
	client->Destroy();
}

CRHTTP_API int crhttp_client_set_cookie(crhttp_client *client, const char *url,
					const char *set_cookie)
{
	if (!client || !url || !set_cookie)
		return CRHTTP_E_INVAL;
	if (crhttp::OnNetworkThread())
		return CRHTTP_E_WOULD_DEADLOCK;
	return client->SetCookie(url, set_cookie);
}

CRHTTP_API char *crhttp_client_get_cookies(crhttp_client *client,
					   const char *url)
{
	if (!client || !url || crhttp::OnNetworkThread())
		return nullptr;
	std::optional<std::string> line = client->GetCookies(url);
	if (!line)
		return nullptr;
	return strdup(line->c_str());
}

CRHTTP_API int crhttp_client_clear_cookies(crhttp_client *client)
{
	if (!client)
		return CRHTTP_E_INVAL;
	if (crhttp::OnNetworkThread())
		return CRHTTP_E_WOULD_DEADLOCK;
	return client->ClearCookies();
}

CRHTTP_API void crhttp_client_close_idle(crhttp_client *client)
{
	if (client)
		client->CloseIdle();
}

// ------------------------------------------------------------------
// Request
// ------------------------------------------------------------------

CRHTTP_API crhttp_request *crhttp_request_new(const char *method,
					      const char *url)
{
	auto *req = new (std::nothrow) crhttp_request();
	if (!req)
		return nullptr;
	if (crhttp_request_set_url(req, url) != CRHTTP_OK ||
	    crhttp_request_set_method(req, method ? method : "GET") !=
		    CRHTTP_OK) {
		delete req;
		return nullptr;
	}
	return req;
}

CRHTTP_API void crhttp_request_free(crhttp_request *req)
{
	delete req;
}

CRHTTP_API int crhttp_request_set_url(crhttp_request *req, const char *url)
{
	if (!req || !url)
		return CRHTTP_E_INVAL;
	GURL u(url);
	if (!u.is_valid() || !u.SchemeIsHTTPOrHTTPS())
		return CRHTTP_E_INVAL;
	req->spec.url = std::move(u);
	return CRHTTP_OK;
}

CRHTTP_API int crhttp_request_set_method(crhttp_request *req,
					 const char *method)
{
	if (!req || !ValidToken(method))
		return CRHTTP_E_INVAL;
	// Chromium normalises the method case like browsers do.
	req->spec.method = method;
	return CRHTTP_OK;
}

CRHTTP_API int crhttp_request_set_header(crhttp_request *req,
					 const char *name, const char *value)
{
	if (!req || !ValidToken(name))
		return CRHTTP_E_INVAL;
	if (value && !net::HttpUtil::IsValidHeaderValue(value))
		return CRHTTP_E_INVAL;
	auto &hs = req->spec.headers;
	for (auto &h : hs) {
		if (base::EqualsCaseInsensitiveASCII(h.first, name)) {
			h.first = name;
			h.second = value ? std::optional<std::string>(value)
					 : std::nullopt;
			return CRHTTP_OK;
		}
	}
	hs.emplace_back(name, value ? std::optional<std::string>(value)
				    : std::nullopt);
	return CRHTTP_OK;
}

CRHTTP_API int crhttp_request_set_body(crhttp_request *req, const void *data,
				       size_t len)
{
	if (!req || (!data && len))
		return CRHTTP_E_INVAL;
	if (!data)
		req->spec.body.reset();
	else
		req->spec.body.emplace(static_cast<const char *>(data), len);
	return CRHTTP_OK;
}

CRHTTP_API int crhttp_request_set_mode(crhttp_request *req, crhttp_mode mode)
{
	if (!req || mode < CRHTTP_MODE_NAVIGATE || mode > CRHTTP_MODE_RAW)
		return CRHTTP_E_INVAL;
	req->spec.mode = mode;
	return CRHTTP_OK;
}

CRHTTP_API int crhttp_request_set_priority(crhttp_request *req,
					   crhttp_priority priority)
{
	if (!req || priority < CRHTTP_PRIORITY_DEFAULT ||
	    priority > CRHTTP_PRIORITY_HIGHEST)
		return CRHTTP_E_INVAL;
	req->spec.priority = priority;
	return CRHTTP_OK;
}

CRHTTP_API int crhttp_request_set_max_redirects(crhttp_request *req,
						int max_redirects)
{
	if (!req || max_redirects < 0)
		return CRHTTP_E_INVAL;
	req->spec.max_redirects = max_redirects;
	return CRHTTP_OK;
}

CRHTTP_API int crhttp_request_set_timeout(crhttp_request *req,
					  uint64_t timeout_ms)
{
	if (!req)
		return CRHTTP_E_INVAL;
	req->spec.timeout_ms = timeout_ms;
	return CRHTTP_OK;
}

CRHTTP_API int crhttp_request_set_decompress(crhttp_request *req, int enable)
{
	if (!req)
		return CRHTTP_E_INVAL;
	req->spec.decompress = !!enable;
	return CRHTTP_OK;
}

CRHTTP_API int crhttp_request_set_max_body_size(crhttp_request *req,
						uint64_t max_bytes)
{
	if (!req)
		return CRHTTP_E_INVAL;
	req->spec.max_body = max_bytes;
	return CRHTTP_OK;
}

// ------------------------------------------------------------------
// Transfers
// ------------------------------------------------------------------

static crhttp_transfer *StartTransfer(crhttp_client *client,
				      const crhttp_request *req,
				      const crhttp_callbacks *cb,
				      void *user_data, crhttp_queue *q)
{
	if (!client || !req)
		return nullptr;
	auto t = base::MakeRefCounted<crhttp_transfer>(client, req->spec, cb,
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

CRHTTP_API crhttp_transfer *crhttp_start(crhttp_client *client,
					 const crhttp_request *req,
					 const crhttp_callbacks *cb,
					 void *user_data)
{
	return StartTransfer(client, req, cb, user_data, nullptr);
}

CRHTTP_API int crhttp_perform(crhttp_client *client, const crhttp_request *req,
			      crhttp_response **resp)
{
	if (resp)
		*resp = nullptr;
	if (!client || !req || !resp)
		return CRHTTP_E_INVAL;
	if (crhttp::OnNetworkThread())
		return CRHTTP_E_WOULD_DEADLOCK;
	crhttp_transfer *t = StartTransfer(client, req, nullptr, nullptr,
					   nullptr);
	if (!t)
		return CRHTTP_E_SHUTDOWN;
	int rc = t->Wait(UINT64_MAX);
	*resp = t->TakeResponse();
	t->Release();
	return rc;
}

CRHTTP_API void crhttp_transfer_cancel(crhttp_transfer *t)
{
	if (t)
		t->Cancel(CRHTTP_E_CANCELED);
}

CRHTTP_API void crhttp_transfer_pause(crhttp_transfer *t)
{
	if (t)
		t->Pause();
}

CRHTTP_API void crhttp_transfer_resume(crhttp_transfer *t)
{
	if (t)
		t->Resume();
}

CRHTTP_API int crhttp_transfer_wait(crhttp_transfer *t, uint64_t timeout_ms)
{
	if (!t)
		return CRHTTP_E_INVAL;
	if (!t->done() && crhttp::OnNetworkThread())
		return CRHTTP_E_WOULD_DEADLOCK;
	return t->Wait(timeout_ms);
}

CRHTTP_API int crhttp_transfer_done(const crhttp_transfer *t)
{
	return t && t->done();
}

CRHTTP_API const crhttp_response *crhttp_transfer_response(
	const crhttp_transfer *t)
{
	return t ? t->response() : nullptr;
}

CRHTTP_API crhttp_response *crhttp_transfer_take_response(crhttp_transfer *t)
{
	return t ? t->TakeResponse() : nullptr;
}

CRHTTP_API void crhttp_transfer_free(crhttp_transfer *t)
{
	if (!t)
		return;
	if (!t->done()) {
		t->Cancel(CRHTTP_E_CANCELED);
		// From another thread, wait so user_data may be freed after
		// we return. On the network thread (inside a callback) the
		// cancel above already completed synchronously, except when
		// called from this transfer's own callback, where waiting
		// would deadlock; the running reference keeps it alive.
		if (!crhttp::OnNetworkThread())
			t->Wait(UINT64_MAX);
	}
	t->Release();
}

// ------------------------------------------------------------------
// Completion queue
// ------------------------------------------------------------------

CRHTTP_API crhttp_queue *crhttp_queue_new(void)
{
	auto *q = new (std::nothrow) crhttp_queue();
	if (q && !q->ok()) {
		delete q;
		return nullptr;
	}
	return q;
}

CRHTTP_API void crhttp_queue_free(crhttp_queue *q)
{
	if (!q)
		return;
	q->Drain();
	delete q;
}

CRHTTP_API int crhttp_queue_fd(const crhttp_queue *q)
{
	return q ? q->fd() : -1;
}

CRHTTP_API int crhttp_queue_start(crhttp_queue *q, crhttp_client *client,
				  const crhttp_request *req, void *user_data)
{
	if (!q || !client || !req)
		return CRHTTP_E_INVAL;
	crhttp_transfer *t = StartTransfer(client, req, nullptr, user_data, q);
	if (!t)
		return CRHTTP_E_SHUTDOWN;
	// The queue holds its own reference; drop the handle one.
	t->Release();
	return CRHTTP_OK;
}

CRHTTP_API int crhttp_queue_pop(crhttp_queue *q, crhttp_transfer **t,
				void **user_data, uint64_t timeout_ms)
{
	if (!q || !t)
		return CRHTTP_E_INVAL;
	*t = nullptr;
	return q->Pop(t, user_data, timeout_ms);
}

CRHTTP_API size_t crhttp_queue_pending(const crhttp_queue *q)
{
	return q ? q->pending() : 0;
}

// ------------------------------------------------------------------
// Response
// ------------------------------------------------------------------

CRHTTP_API void crhttp_response_free(crhttp_response *resp)
{
	delete resp;
}

CRHTTP_API int crhttp_response_status(const crhttp_response *r)
{
	return r ? r->status : 0;
}

CRHTTP_API const char *crhttp_response_status_text(const crhttp_response *r)
{
	return r ? r->status_text.c_str() : "";
}

CRHTTP_API const char *crhttp_response_url(const crhttp_response *r)
{
	return r ? r->url.c_str() : "";
}

CRHTTP_API int crhttp_response_redirect_count(const crhttp_response *r)
{
	return r ? r->redirects : 0;
}

CRHTTP_API size_t crhttp_response_header_count(const crhttp_response *r)
{
	return r ? r->headers.size() : 0;
}

CRHTTP_API int crhttp_response_header_at(const crhttp_response *r,
					 size_t index, const char **name,
					 const char **value)
{
	if (!r || index >= r->headers.size())
		return CRHTTP_E_INVAL;
	if (name)
		*name = r->headers[index].first.c_str();
	if (value)
		*value = r->headers[index].second.c_str();
	return CRHTTP_OK;
}

CRHTTP_API const char *crhttp_response_header(const crhttp_response *r,
					      const char *name)
{
	if (!r || !name)
		return nullptr;
	for (const auto &h : r->headers)
		if (base::EqualsCaseInsensitiveASCII(h.first, name))
			return h.second.c_str();
	return nullptr;
}

CRHTTP_API const char *crhttp_response_body(const crhttp_response *r,
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

CRHTTP_API const char *crhttp_response_protocol(const crhttp_response *r)
{
	return r ? r->protocol.c_str() : "";
}

CRHTTP_API const char *crhttp_response_remote_address(const crhttp_response *r)
{
	return r ? r->remote.c_str() : "";
}

CRHTTP_API int crhttp_response_via_proxy(const crhttp_response *r)
{
	return r && r->via_proxy;
}

CRHTTP_API int crhttp_response_net_error(const crhttp_response *r)
{
	return r ? r->net_error : 0;
}

CRHTTP_API const char *crhttp_response_error_string(const crhttp_response *r)
{
	return r ? r->net_error_str.c_str() : "";
}

} // extern "C"
