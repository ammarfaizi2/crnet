// SPDX-License-Identifier: BSD-3-Clause
//
// The exported C ABI. Thin argument checking and handle plumbing on top
// of crnet_client / crnet_transfer / crnet_queue.
//
#include <stdlib.h>
#include <string.h>

#include <limits>
#include <new>

#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_tokenizer.h"
#include "base/strings/string_util.h"
#include "base/containers/extend.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "crnet/internal.h"
#include "net/base/network_change_notifier.h"
#include "net/cert/x509_certificate.h"
#include "net/dns/public/dns_over_https_config.h"
#include "net/cert/x509_util.h"
#include "net/ssl/openssl_private_key.h"
#include "third_party/boringssl/src/include/openssl/bio.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/pem.h"
#include "third_party/boringssl/src/include/openssl/x509.h"
#include "third_party/boringssl/src/pki/cert_errors.h"
#include "third_party/boringssl/src/pki/parsed_certificate.h"
#include "net/base/scheme_host_port_matcher.h"
#include "net/proxy_resolution/proxy_host_matching_rules.h"
#include "crnet/stream.h"
#include "crnet/websocket.h"
#include "net/http/http_util.h"
#include "url/url_constants.h"

namespace {

using crnet::ClientConfig;

#define CRNET_STR2(x) #x
#define CRNET_STR(x) CRNET_STR2(x)
#define CRNET_VERSION_STRING                                            \
	CRNET_STR(CRNET_VERSION_MAJOR) "." CRNET_STR(CRNET_VERSION_MINOR) "." \
	CRNET_STR(CRNET_VERSION_PATCH)

bool ValidToken(const char *s)
{
	return s && *s && net::HttpUtil::IsToken(s);
}

// Proxy URL -> Chromium proxy rules. Credentials are split off for the
// proxy auth handler (Chromium never takes them from the rules).
// A proxy URL as crnet_client_config_set_proxy() takes it, into a
// Chromium proxy URI ("" for direct) and credentials. An empty userinfo
// ("socks5://@host") sets |*none|: explicitly no credentials.
int ParseProxy(const char *url, std::string *proxy, std::string *user,
	       std::string *pass, bool *none = nullptr)
{
	proxy->clear();
	user->clear();
	pass->clear();
	if (none)
		*none = false;
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

	std::string_view rest = s.substr(sep + 3);
	if (none && !rest.empty() && rest.front() == '@')
		*none = true;
	// Parse authority with a scheme GURL understands.
	GURL u(base::StrCat({"http://", rest}));
	if (!u.is_valid() || !u.has_host())
		return CRNET_E_INVAL;
	if (u.path() != "/" || u.has_query())
		return CRNET_E_INVAL;
	*proxy = base::StrCat({scheme, "://", u.host()});
	if (u.has_port())
		base::StrAppend(proxy, {":", u.port()});
	if (u.has_username()) {
		// Every %XX is a byte: NORMAL would keep "%3A", "%2F" and
		// the like escaped, so ':' or '/' could not be sent.
		*user = base::UnescapeBinaryURLComponent(u.username());
		*pass = base::UnescapeBinaryURLComponent(u.password());
		if (!crnet::ValidProxyCredentials(*proxy, *user, *pass))
			return CRNET_E_INVAL;
	}
	return CRNET_OK;
}

// Every rule must parse: ProxyHostMatchingRules::ParseFromString() drops
// the ones that do not, which would silently send through the proxy what
// was meant to bypass it.
bool ValidBypassRules(const std::string &raw)
{
	net::ProxyHostMatchingRules rules;
	base::StringTokenizer t(
		raw, net::SchemeHostPortMatcher::kParseRuleListDelimiterList);
	while (t.GetNext()) {
		if (base::TrimWhitespaceASCII(t.token_piece(), base::TRIM_ALL)
			    .empty())
			continue;
		if (!rules.AddRuleFromString(t.token_piece()))
			return false;
	}
	return true;
}

int StoreString(std::string *dst, const char *src, const std::string &def)
{
	*dst = src ? src : def;
	return CRNET_OK;
}

// The SOCKS5 reply code a net error stands for, -1 for none: the net
// errors chromium/patches/*-Tell-SOCKS5-failures-apart.patch adds, one
// per reply code.
int SocksReplyOf(int net_error)
{
	if (net_error > net::ERR_SOCKS_GENERAL_FAILURE ||
	    net_error < net::ERR_SOCKS_ADDRESS_TYPE_NOT_SUPPORTED)
		return -1;
	return net::ERR_SOCKS_GENERAL_FAILURE - net_error + 1;
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
	case CRNET_E_PENDING:		return "transfer still running";
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
	std::string proxy, user, pass;
	int rc = ParseProxy(proxy_url, &proxy, &user, &pass);
	if (rc == CRNET_OK) {
		cfg->cfg.proxy = std::move(proxy);
		cfg->cfg.proxy_user = std::move(user);
		cfg->cfg.proxy_pass = std::move(pass);
	}
	return rc;
}

CRNET_API int crnet_client_config_set_proxy_bypass(crnet_client_config *cfg,
						   const char *rules)
{
	if (!cfg)
		return CRNET_E_INVAL;
	std::string r = rules ? rules : "";
	if (!ValidBypassRules(r))
		return CRNET_E_INVAL;
	cfg->cfg.proxy_bypass = std::move(r);
	return CRNET_OK;
}

CRNET_API int crnet_client_config_add_trust_anchors(crnet_client_config *cfg,
						    const char *pem)
{
	if (!cfg || !pem)
		return CRNET_E_INVAL;
	net::CertificateList certs =
		net::X509Certificate::CreateCertificateListFromBytes(
			base::as_byte_span(std::string_view(pem)),
			net::X509Certificate::FORMAT_PEM_CERT_SEQUENCE);
	if (certs.empty())
		return CRNET_E_INVAL;
	std::vector<std::string> der;
	for (const auto &c : certs) {
		bssl::CertErrors errors;
		if (!bssl::ParsedCertificate::Create(
			    bssl::UpRef(c->cert_buffer()),
			    net::x509_util::DefaultParseCertificateOptions(),
			    &errors))
			return CRNET_E_INVAL;
		der.emplace_back(net::x509_util::CryptoBufferAsStringPiece(
			c->cert_buffer()));
	}
	base::Extend(cfg->cfg.trust_anchors, std::move(der));
	return CRNET_OK;
}

CRNET_API int crnet_client_config_add_trust_anchors_file(
	crnet_client_config *cfg, const char *path)
{
	if (!cfg || !path)
		return CRNET_E_INVAL;
	std::string pem;
	if (!base::ReadFileToStringWithMaxSize(base::FilePath(path), &pem,
					       16 * 1024 * 1024))
		return CRNET_E_INVAL;
	return crnet_client_config_add_trust_anchors(cfg, pem.c_str());
}

CRNET_API int crnet_client_config_set_client_cert(crnet_client_config *cfg,
						  const char *cert_pem,
						  const char *key_pem)
{
	if (!cfg || !cert_pem != !key_pem)
		return CRNET_E_INVAL;
	if (!cert_pem) {
		cfg->cfg.client_cert = nullptr;
		cfg->cfg.client_key = nullptr;
		return CRNET_OK;
	}
	// The leaf first, then any intermediates to send along.
	net::CertificateList certs =
		net::X509Certificate::CreateCertificateListFromBytes(
			base::as_byte_span(std::string_view(cert_pem)),
			net::X509Certificate::FORMAT_PEM_CERT_SEQUENCE);
	if (certs.empty())
		return CRNET_E_INVAL;
	std::vector<bssl::UniquePtr<CRYPTO_BUFFER>> intermediates;
	for (size_t i = 1; i < certs.size(); i++)
		intermediates.push_back(bssl::UpRef(certs[i]->cert_buffer()));
	scoped_refptr<net::X509Certificate> chain =
		net::X509Certificate::CreateFromBuffer(
			bssl::UpRef(certs[0]->cert_buffer()),
			std::move(intermediates));

	// Unencrypted PEM key (PKCS#8, or traditional RSA/EC); it must
	// belong to the leaf.
	bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(key_pem, strlen(key_pem)));
	bssl::UniquePtr<EVP_PKEY> pkey(
		PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
	if (!chain || !pkey)
		return CRNET_E_INVAL;
	std::string_view der = net::x509_util::CryptoBufferAsStringPiece(
		chain->cert_buffer());
	const uint8_t *p = reinterpret_cast<const uint8_t *>(der.data());
	bssl::UniquePtr<X509> leaf(d2i_X509(nullptr, &p, der.size()));
	const EVP_PKEY *pub = leaf ? X509_get0_pubkey(leaf.get()) : nullptr;
	if (!pub || EVP_PKEY_cmp(pub, pkey.get()) != 1)
		return CRNET_E_INVAL;
	scoped_refptr<net::SSLPrivateKey> key =
		net::WrapOpenSSLPrivateKey(std::move(pkey));
	if (!key)
		return CRNET_E_INVAL;
	cfg->cfg.client_cert = std::move(chain);
	cfg->cfg.client_key = std::move(key);
	return CRNET_OK;
}

CRNET_API int crnet_client_config_set_client_cert_file(
	crnet_client_config *cfg, const char *cert_path, const char *key_path)
{
	if (!cfg || !cert_path || !key_path)
		return CRNET_E_INVAL;
	std::string cert, key;
	if (!base::ReadFileToStringWithMaxSize(base::FilePath(cert_path),
					       &cert, 1024 * 1024) ||
	    !base::ReadFileToStringWithMaxSize(base::FilePath(key_path), &key,
					       1024 * 1024))
		return CRNET_E_INVAL;
	return crnet_client_config_set_client_cert(cfg, cert.c_str(),
						   key.c_str());
}

CRNET_API void crnet_client_config_set_trust_anchors_only(
	crnet_client_config *cfg, int only)
{
	if (cfg)
		cfg->cfg.trust_anchors_only = !!only;
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
	return StoreString(&cfg->cfg.platform, v, "");
}

CRNET_API int crnet_client_config_set_platform_profile(
	crnet_client_config *cfg, crnet_platform_profile profile)
{
	if (!cfg || !crnet::PlatformUserAgent(profile))
		return CRNET_E_INVAL;
	cfg->cfg.platform_profile = profile;
	return CRNET_OK;
}

CRNET_API const char *crnet_platform_user_agent(
	crnet_platform_profile profile)
{
	const std::string *ua = crnet::PlatformUserAgent(profile);
	return ua ? ua->c_str() : nullptr;
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

CRNET_API void crnet_client_config_set_ignore_ip_address_changes(
	crnet_client_config *cfg, int enable)
{
	if (cfg)
		cfg->cfg.ignore_ip_address_changes = !!enable;
}

CRNET_API int crnet_simulate_ip_address_change(void)
{
	if (!crnet::EnsureGlobalInit(nullptr, nullptr, nullptr))
		return CRNET_E_STATE;
	// Observers hear it on their own threads, as from the OS.
	net::NetworkChangeNotifier::NotifyObserversOfIPAddressChangeForTests();
	return CRNET_OK;
}

CRNET_API int crnet_client_config_set_host_rules(crnet_client_config *cfg,
						 const char *rules)
{
	if (!cfg)
		return CRNET_E_INVAL;
	return StoreString(&cfg->cfg.host_rules, rules, "");
}

CRNET_API int crnet_client_config_set_local_address(
	crnet_client_config *cfg, const char *addresses)
{
	if (!cfg)
		return CRNET_E_INVAL;
	crnet::LocalAddresses local;
	if (addresses && *addresses &&
	    !crnet::ParseLocalAddresses(addresses, &local))
		return CRNET_E_INVAL;
	cfg->cfg.binding.addresses = local;
	return CRNET_OK;
}

CRNET_API int crnet_client_config_set_bind_interface(crnet_client_config *cfg,
						    const char *name)
{
	if (!cfg)
		return CRNET_E_INVAL;
	if (name && *name && !crnet::ValidInterfaceName(name))
		return CRNET_E_INVAL;
	cfg->cfg.binding.interface_name = name ? name : "";
	return CRNET_OK;
}

CRNET_API void crnet_client_config_set_connect_timeout(
	crnet_client_config *cfg, uint64_t timeout_ms)
{
	if (cfg)
		cfg->cfg.connect_timeout_ms = timeout_ms;
}

CRNET_API void crnet_client_config_set_timeout(crnet_client_config *cfg,
					       uint64_t timeout_ms)
{
	if (cfg)
		cfg->cfg.timeout_ms = timeout_ms;
}

CRNET_API int crnet_client_config_set_state_dir(crnet_client_config *cfg,
						const char *dir)
{
	if (!cfg)
		return CRNET_E_INVAL;
	cfg->cfg.state_dir = dir ? dir : "";
	return CRNET_OK;
}

CRNET_API int crnet_client_config_set_secure_dns(crnet_client_config *cfg,
						 crnet_secure_dns mode,
						 const char *templates)
{
	if (!cfg)
		return CRNET_E_INVAL;
	switch (mode) {
	case CRNET_SECURE_DNS_OFF:
		cfg->cfg.secure_dns = mode;
		cfg->cfg.doh_templates.clear();
		return CRNET_OK;
	case CRNET_SECURE_DNS_AUTOMATIC:
	case CRNET_SECURE_DNS_SECURE:
		break;
	default:
		return CRNET_E_INVAL;
	}
	if (!templates)
		return CRNET_E_INVAL;
	// FromString() rejects the whole list if any template is invalid.
	std::optional<net::DnsOverHttpsConfig> doh =
		net::DnsOverHttpsConfig::FromString(templates);
	if (!doh || doh->servers().empty())
		return CRNET_E_INVAL;
	cfg->cfg.secure_dns = mode;
	cfg->cfg.doh_templates = templates;
	return CRNET_OK;
}

CRNET_API int crnet_client_config_set_http_cache(crnet_client_config *cfg,
						 crnet_http_cache type,
						 const char *path,
						 uint64_t max_size)
{
	if (!cfg || max_size > std::numeric_limits<int>::max())
		return CRNET_E_INVAL;
	switch (type) {
	case CRNET_HTTP_CACHE_OFF:
	case CRNET_HTTP_CACHE_MEMORY:
		path = nullptr;
		break;
	case CRNET_HTTP_CACHE_DISK:
		if (!path || !*path)
			return CRNET_E_INVAL;
		break;
	default:
		return CRNET_E_INVAL;
	}
	cfg->cfg.http_cache = type;
	cfg->cfg.http_cache_path = path ? path : "";
	cfg->cfg.http_cache_max_size = max_size;
	return CRNET_OK;
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
	if (c.proxy_credentials) {
		// Only now is the proxy they are for known.
		if (!crnet::ValidProxyCredentials(c.proxy,
						  c.proxy_credentials->first,
						  c.proxy_credentials->second))
			return nullptr;
		c.proxy_user = c.proxy_credentials->first;
		c.proxy_pass = c.proxy_credentials->second;
	}
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

CRNET_API char *crnet_client_export_cookies(crnet_client *client,
					    crnet_cookie_format format)
{
	if (!client || crnet::OnNetworkThread() ||
	    (format != CRNET_COOKIES_JSON && format != CRNET_COOKIES_NETSCAPE))
		return nullptr;
	return strdup(client->ExportCookies(format).c_str());
}

CRNET_API int crnet_client_import_cookies(crnet_client *client,
					  const char *data,
					  crnet_cookie_format format,
					  size_t *imported)
{
	size_t n = 0;
	if (imported)
		*imported = 0;
	if (!client || !data ||
	    (format != CRNET_COOKIES_JSON && format != CRNET_COOKIES_NETSCAPE))
		return CRNET_E_INVAL;
	if (crnet::OnNetworkThread())
		return CRNET_E_WOULD_DEADLOCK;
	int rc = client->ImportCookies(data, format, &n);
	if (imported)
		*imported = n;
	return rc;
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
	// ws:// and wss:// are for crnet_ws_connect() only.
	if (!u.is_valid() || !(u.SchemeIsHTTPOrHTTPS() || u.SchemeIsWSOrWSS()))
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
	req->spec.body.reset();
	req->spec.body_file.reset();
	req->spec.body_stream.reset();
	if (data)
		req->spec.body.emplace(static_cast<const char *>(data), len);
	return CRNET_OK;
}

CRNET_API int crnet_request_set_body_file(crnet_request *req, const char *path,
					  uint64_t offset, uint64_t length)
{
	if (!req || !path || !*path)
		return CRNET_E_INVAL;
	req->spec.body.reset();
	req->spec.body_stream.reset();
	req->spec.body_file = crnet::RequestSpec::File{path, offset, length};
	return CRNET_OK;
}

CRNET_API int crnet_request_set_body_stream(crnet_request *req, int64_t length)
{
	if (!req || length < CRNET_LENGTH_UNKNOWN)
		return CRNET_E_INVAL;
	req->spec.body.reset();
	req->spec.body_file.reset();
	req->spec.body_stream = length;
	return CRNET_OK;
}

CRNET_API int crnet_request_set_response_stream(crnet_request *req, int enable)
{
	if (!req)
		return CRNET_E_INVAL;
	req->spec.stream_response = !!enable;
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

CRNET_API int crnet_transfer_set_progress_callback(crnet_transfer *t,
						   crnet_progress_cb cb,
						   uint64_t interval_ms)
{
	if (!t || (cb && !interval_ms))
		return CRNET_E_INVAL;
	if (t->result() != CRNET_E_PENDING)
		return CRNET_E_STATE;
	t->SetProgressCallback(cb, interval_ms);
	return CRNET_OK;
}

CRNET_API int crnet_request_set_ws_compression(crnet_request *req, int enable)
{
	if (!req)
		return CRNET_E_INVAL;
	req->spec.ws_compression = !!enable;
	return CRNET_OK;
}

CRNET_API int crnet_request_set_cache_mode(crnet_request *req,
					   crnet_cache_mode mode)
{
	if (!req || mode < CRNET_CACHE_DEFAULT ||
	    mode > CRNET_CACHE_ONLY_IF_CACHED)
		return CRNET_E_INVAL;
	req->spec.cache_mode = mode;
	return CRNET_OK;
}

CRNET_API int crnet_request_set_connect_timeout(crnet_request *req,
						uint64_t timeout_ms)
{
	if (!req)
		return CRNET_E_INVAL;
	req->spec.connect_timeout_ms = timeout_ms;
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

CRNET_API int crnet_request_set_fresh_connection(crnet_request *req,
						 int enable)
{
	if (!req)
		return CRNET_E_INVAL;
	req->spec.fresh_connection = !!enable;
	return CRNET_OK;
}

namespace {

// Credentials as crnet_*_set_proxy_credentials() take them; the proxy's
// own rules are checked once it is known.
int CredentialsOf(const void *user, size_t user_len, const void *pass,
		  size_t pass_len,
		  std::optional<std::pair<std::string, std::string>> *out)
{
	if (!user) {
		out->reset();
		return CRNET_OK;
	}
	if ((!pass && pass_len) || user_len > 255 || pass_len > 255 ||
	    (!user_len && pass_len))
		return CRNET_E_INVAL;
	out->emplace(std::string(static_cast<const char *>(user), user_len),
		     pass ? std::string(static_cast<const char *>(pass),
					pass_len)
			  : std::string());
	return CRNET_OK;
}

} // namespace

CRNET_API int crnet_client_config_set_proxy_credentials(
	crnet_client_config *cfg, const void *user, size_t user_len,
	const void *pass, size_t pass_len)
{
	if (!cfg)
		return CRNET_E_INVAL;
	return CredentialsOf(user, user_len, pass, pass_len,
			     &cfg->cfg.proxy_credentials);
}

CRNET_API int crnet_request_set_proxy_credentials(crnet_request *req,
						  const void *user,
						  size_t user_len,
						  const void *pass,
						  size_t pass_len)
{
	if (!req)
		return CRNET_E_INVAL;
	return CredentialsOf(user, user_len, pass, pass_len,
			     &req->spec.proxy_credentials);
}

CRNET_API int crnet_request_set_proxy(crnet_request *req,
				      const char *proxy_url)
{
	if (!req)
		return CRNET_E_INVAL;
	if (!proxy_url) {
		req->spec.proxy.reset();
		return CRNET_OK;
	}
	crnet::ProxySpec p;
	if (!*proxy_url ||
	    ParseProxy(proxy_url, &p.proxy, &p.user, &p.pass,
		       &p.no_credentials) != CRNET_OK ||
	    !crnet::ProxyChainOf(p.proxy).IsValid())
		return CRNET_E_INVAL;
	req->spec.proxy = std::move(p);
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
	if (!client || !req || !req->spec.url.SchemeIsHTTPOrHTTPS())
		return nullptr;
	auto t = base::MakeRefCounted<crnet_transfer>(client, req->spec, cb,
						      user_data, q);
	if (!t->ok())
		return nullptr;
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
	// Streamed bodies need the transfer handle, which perform never
	// exposes; they would stall forever.
	if (!req->spec.url.SchemeIsHTTPOrHTTPS() || req->spec.body_stream ||
	    req->spec.stream_response)
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

CRNET_API int64_t crnet_transfer_progress(const crnet_transfer *t,
					  crnet_progress which)
{
	if (!t || which < 0 || which >= CRNET_PROGRESS_COUNT)
		return -1;
	return t->progress(which);
}

CRNET_API int crnet_transfer_result(const crnet_transfer *t)
{
	return t ? t->result() : CRNET_E_INVAL;
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

CRNET_API int crnet_transfer_wait_response(crnet_transfer *t,
					   uint64_t timeout_ms)
{
	if (!t)
		return CRNET_E_INVAL;
	if (timeout_ms && !t->done() && crnet::OnNetworkThread())
		return CRNET_E_WOULD_DEADLOCK;
	return t->WaitResponse(timeout_ms);
}

CRNET_API int crnet_transfer_write(crnet_transfer *t, const void *data,
				   size_t len, size_t *written,
				   uint64_t timeout_ms)
{
	if (written)
		*written = 0;
	if (!t || (!data && len))
		return CRNET_E_INVAL;
	if (!t->upload())
		return CRNET_E_STATE;
	if (timeout_ms && crnet::OnNetworkThread())
		return CRNET_E_WOULD_DEADLOCK;
	return t->upload()->Write(static_cast<const char *>(data), len, written,
				  timeout_ms);
}

CRNET_API int crnet_transfer_write_end(crnet_transfer *t)
{
	if (!t)
		return CRNET_E_INVAL;
	if (!t->upload())
		return CRNET_E_STATE;
	int rc = t->upload()->End();
	if (rc == CRNET_E_INVAL)
		t->Cancel(CRNET_E_INVAL);	// fewer bytes than declared
	return rc;
}

CRNET_API int crnet_transfer_read(crnet_transfer *t, void *buf, size_t len,
				  size_t *nread, uint64_t timeout_ms)
{
	size_t dummy;
	if (!nread)
		nread = &dummy;
	*nread = 0;
	if (!t || (!buf && len))
		return CRNET_E_INVAL;
	if (!t->download())
		return CRNET_E_STATE;
	if (timeout_ms && crnet::OnNetworkThread())
		return CRNET_E_WOULD_DEADLOCK;
	bool resume = false;
	int rc = t->download()->Read(static_cast<char *>(buf), len, nread,
				     timeout_ms, &resume);
	if (resume)
		t->ResumeDownload();
	return rc;
}

CRNET_API int crnet_transfer_write_fd(const crnet_transfer *t)
{
	return t && t->upload() ? t->upload()->fd() : -1;
}

CRNET_API int crnet_transfer_fd(const crnet_transfer *t)
{
	return t && t->download() ? t->download()->fd() : -1;
}

CRNET_API int crnet_transfer_response_fd(const crnet_transfer *t)
{
	return t ? t->response_fd() : -1;
}

CRNET_API int crnet_transfer_done_fd(const crnet_transfer *t)
{
	return t ? t->done_fd() : -1;
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
	if (!q || !client || !req || !req->spec.url.SchemeIsHTTPOrHTTPS() ||
	    req->spec.body_stream || req->spec.stream_response)
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
// WebSocket
// ------------------------------------------------------------------

CRNET_API crnet_ws *crnet_ws_connect(crnet_client *client,
				     const crnet_request *req,
				     const char *protocols,
				     const crnet_ws_callbacks *cb,
				     void *user_data)
{
	if (!client || !req)
		return nullptr;
	GURL url = req->spec.url;
	if (url.SchemeIsHTTPOrHTTPS()) {
		GURL::Replacements r;
		r.SetSchemeStr(url.SchemeIs(url::kHttpsScheme) ? url::kWssScheme
								 : url::kWsScheme);
		url = url.ReplaceComponents(r);
	}
	if (!url.SchemeIsWSOrWSS())
		return nullptr;
	std::optional<crnet::WsHeaders> headers =
		crnet::BuildWsHeaders(client->config(), req->spec, url, protocols);
	if (!headers)
		return nullptr;

	auto ws = base::MakeRefCounted<crnet_ws>(client, req->spec, url,
						 std::move(*headers), cb,
						 user_data);
	if (!ws->ok() || !client->StartWebSocket(ws))
		return nullptr;
	// The caller's handle reference.
	return ws.release();
}

CRNET_API int crnet_ws_wait_open(crnet_ws *ws, uint64_t timeout_ms)
{
	if (!ws)
		return CRNET_E_INVAL;
	if (timeout_ms && crnet::OnNetworkThread())
		return CRNET_E_WOULD_DEADLOCK;
	return ws->WaitOpen(timeout_ms);
}

CRNET_API int crnet_ws_send(crnet_ws *ws, crnet_ws_msg_type type,
			    const void *data, size_t len)
{
	return ws ? ws->Send(type, data, len) : CRNET_E_INVAL;
}

CRNET_API uint64_t crnet_ws_buffered_amount(const crnet_ws *ws)
{
	return ws ? ws->buffered_amount() : 0;
}

CRNET_API int crnet_ws_recv(crnet_ws *ws, crnet_ws_msg_type *type,
			    void **data, size_t *len, uint64_t timeout_ms)
{
	if (!ws || !data)
		return CRNET_E_INVAL;
	*data = nullptr;
	if (timeout_ms && crnet::OnNetworkThread())
		return CRNET_E_WOULD_DEADLOCK;
	return ws->Recv(type, data, len, timeout_ms);
}

CRNET_API int crnet_ws_fd(const crnet_ws *ws)
{
	return ws ? ws->fd() : -1;
}

CRNET_API int crnet_ws_close(crnet_ws *ws, int code, const char *reason)
{
	if (!ws)
		return CRNET_E_INVAL;
	return ws->Close(code, reason ? reason : "");
}

CRNET_API int crnet_ws_ping(crnet_ws *ws, const void *data, size_t len)
{
	return ws ? ws->Ping(data, len) : CRNET_E_INVAL;
}

CRNET_API void crnet_ws_set_pong_callback(crnet_ws *ws, crnet_ws_pong_cb cb)
{
	if (ws)
		ws->SetPongCallback(cb);
}

CRNET_API void crnet_ws_set_ping_callback(crnet_ws *ws, crnet_ws_pong_cb cb)
{
	if (ws)
		ws->SetPingCallback(cb);
}

CRNET_API void crnet_ws_set_auto_ping(crnet_ws *ws, uint64_t interval_ms)
{
	if (ws)
		ws->SetAutoPing(interval_ms);
}

CRNET_API void crnet_ws_set_idle_timeout(crnet_ws *ws, uint64_t timeout_ms)
{
	if (ws)
		ws->SetIdleTimeout(timeout_ms);
}

CRNET_API void crnet_ws_pause(crnet_ws *ws)
{
	if (ws)
		ws->Pause();
}

CRNET_API void crnet_ws_resume(crnet_ws *ws)
{
	if (ws)
		ws->Resume();
}

CRNET_API int crnet_ws_wait_closed(crnet_ws *ws, uint64_t timeout_ms)
{
	if (!ws)
		return CRNET_E_INVAL;
	if (timeout_ms && !ws->done() && crnet::OnNetworkThread())
		return CRNET_E_WOULD_DEADLOCK;
	return ws->WaitClosed(timeout_ms);
}

CRNET_API int crnet_ws_open_fd(const crnet_ws *ws)
{
	return ws ? ws->open_fd() : -1;
}

CRNET_API int crnet_ws_closed_fd(const crnet_ws *ws)
{
	return ws ? ws->closed_fd() : -1;
}

CRNET_API crnet_ws_state crnet_ws_get_state(const crnet_ws *ws)
{
	return ws ? ws->state() : CRNET_WS_CLOSED;
}

CRNET_API const char *crnet_ws_protocol(const crnet_ws *ws)
{
	return ws && ws->state() != CRNET_WS_CONNECTING
		       ? ws->protocol().c_str()
		       : "";
}

CRNET_API const char *crnet_ws_extensions(const crnet_ws *ws)
{
	return ws && ws->state() != CRNET_WS_CONNECTING
		       ? ws->extensions().c_str()
		       : "";
}

CRNET_API const crnet_response *crnet_ws_handshake_response(
	const crnet_ws *ws)
{
	if (!ws || (ws->state() == CRNET_WS_CONNECTING && !ws->done()))
		return nullptr;
	return ws->handshake();
}

// The close results are written before the state turns CLOSED, which
// on_close already sees, and never again: they are readable from then on,
// on_close included.
CRNET_API int crnet_ws_close_code(const crnet_ws *ws)
{
	return ws && ws->state() == CRNET_WS_CLOSED ? ws->close_code() : 0;
}

CRNET_API const char *crnet_ws_close_reason(const crnet_ws *ws)
{
	return ws && ws->state() == CRNET_WS_CLOSED
		       ? ws->close_reason().c_str()
		       : "";
}

CRNET_API int crnet_ws_net_error(const crnet_ws *ws)
{
	return ws && ws->state() == CRNET_WS_CLOSED ? ws->net_error() : 0;
}

CRNET_API int crnet_ws_socks_reply(const crnet_ws *ws)
{
	return SocksReplyOf(crnet_ws_net_error(ws));
}

CRNET_API void crnet_ws_free(crnet_ws *ws)
{
	if (!ws)
		return;
	if (!ws->done()) {
		ws->Abort(CRNET_E_CANCELED);
		// As crnet_transfer_free(): wait for on_close unless we are
		// on the network thread, where the abort was synchronous.
		if (!crnet::OnNetworkThread())
			ws->WaitClosed(UINT64_MAX);
	}
	ws->Release();
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

CRNET_API int crnet_response_socks_reply(const crnet_response *r)
{
	return r ? SocksReplyOf(r->net_error) : -1;
}

CRNET_API int crnet_response_from_cache(const crnet_response *r)
{
	return r && r->from_cache;
}

CRNET_API int crnet_response_socket_reused(const crnet_response *r)
{
	return r && r->socket_reused;
}

CRNET_API uint64_t crnet_response_connection_id(const crnet_response *r)
{
	return r ? r->connection_id : 0;
}

CRNET_API int64_t crnet_response_timing(const crnet_response *r,
					crnet_timing which)
{
	if (!r || which < 0 || which >= CRNET_TIMING_COUNT)
		return -1;
	return r->timing[static_cast<size_t>(which)];
}

CRNET_API int crnet_response_content_decoded(const crnet_response *r)
{
	return r && !r->decoded_codings.empty();
}

CRNET_API const char *crnet_response_decoded_codings(const crnet_response *r)
{
	return r ? r->decoded_codings.c_str() : nullptr;
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
