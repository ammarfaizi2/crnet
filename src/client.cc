// SPDX-License-Identifier: BSD-3-Clause
//
// crnet_client: a URLRequestContext configured like Chrome's, living
// on a dedicated IO thread. All transfers of a client share that
// thread, its connection pools, HTTP/2 sessions and cookie jar.
//
#include <atomic>
#include <limits>
#include <memory>
#include <string_view>

#include "base/base64.h"
#include "base/command_line.h"
#include "base/containers/extend.h"
#include "base/feature_list.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/callback_helpers.h"
#include "base/functional/bind.h"
#include "base/message_loop/message_pump_type.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "components/network_session_configurator/browser/network_session_configurator.h"
#include "crnet/cookie_io.h"
#include "crnet/socks_auth.h"
#include "crnet/state.h"
#include "crnet/internal.h"
#include "crnet/websocket.h"
#include "net/base/features.h"
#include "net/base/network_delegate_impl.h"
#include "net/base/proxy_delegate.h"
#include "net/base/proxy_string_util.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/proxy_resolution/proxy_info.h"
#include "net/cert/caching_cert_verifier.h"
#include "net/cert/cert_verifier.h"
#include "net/cert/cert_verify_proc.h"
#include "net/cert/cert_verify_proc_builtin.h"
#include "net/cert/coalescing_cert_verifier.h"
#include "net/cert/crl_set.h"
#include "net/cert/ct_policy_enforcer.h"
#include "net/cert/do_nothing_ct_verifier.h"
#include "net/cert/multi_threaded_cert_verifier.h"
#include "net/cert/x509_util.h"
#include "net/cert_net/cert_net_fetcher_url_request.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_partition_key_collection.h"
#include "net/cookies/cookie_store.h"
#include "net/dns/host_resolver.h"
#include "net/dns/public/dns_over_https_config.h"
#include "net/dns/public/insecure_dns_mode.h"
#include "net/dns/public/secure_dns_mode.h"
#include "net/http/http_network_session.h"
#include "net/http/http_transaction_factory.h"
#include "net/log/file_net_log_observer.h"
#include "net/log/net_log.h"
#include "net/log/net_log_capture_mode.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_service_fixed.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/quic/quic_context.h"
#include "net/ssl/ssl_config_service.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/url_request_context_builder.h"

#include "net/cert/internal/system_trust_store.h"
#include "net/cert/internal/trust_store_chrome.h"
#include "third_party/boringssl/src/pki/cert_errors.h"
#include "third_party/boringssl/src/pki/parsed_certificate.h"

namespace {

using crnet::ClientConfig;

constexpr net::NetworkTrafficAnnotationTag kProxyAnnotation =
	net::DefineNetworkTrafficAnnotation("crnet_proxy", R"(
		semantics {
			sender: "crnet"
			description: "Proxy configured by the crnet user."
			trigger: "Any crnet request."
			data: "None."
			destination: OTHER
		}
		policy {
			cookies_allowed: NO
			setting: "Not applicable."
			policy_exception_justification: "Library."
		})");

// The SSLContextConfig Chrome's browser process hands to the network
// service: //net defaults plus the TLS Trust Anchor IDs that
// chrome/browser/ssl/ssl_config_service_manager.cc seeds from the
// compiled-in Chrome Root Store. Without them the ClientHello lacks the
// trust_anchors extension Chrome sends.
class ChromeSSLConfigService : public net::SSLConfigService {
public:
	ChromeSSLConfigService()
	{
#if BUILDFLAG(CHROME_ROOT_STORE_SUPPORTED)
		if (base::FeatureList::IsEnabled(
			    net::features::kTLSTrustAnchorIDs)) {
			std::vector<std::vector<uint8_t>> ids;
			if (base::FeatureList::IsEnabled(
				    net::features::kNonMtcTrustAnchorIDs))
				base::Extend(ids,
					     net::TrustStoreChrome::
						     GetTrustAnchorIDsFromCompiledInRootStore());
			if (base::FeatureList::IsEnabled(
				    net::features::kVerifyMTCs))
				base::Extend(ids,
					     net::TrustStoreChrome::
						     GetTrustedMtcCaIDsFromCompiledInRootStore());
			config_.trust_anchor_ids =
				net::x509_util::EncodeTlsRequestedTrustAnchorIDList(
					std::move(ids));
		}
#endif
	}

	net::SSLContextConfig GetSSLContextConfig() override { return config_; }

	net::EchMode GetEchMode(std::string_view hostname) const override
	{
		return net::EchMode::kOpportunistic;
	}

	bool CanShareConnectionWithClientCerts(
		std::string_view hostname) const override
	{
		return false;
	}

private:
	net::SSLContextConfig config_;
};

// Applies crnet's header policy right before each transaction (also
// after redirects) and implements the cookies on/off switch.
class CrNetworkDelegate : public net::NetworkDelegateImpl {
public:
	explicit CrNetworkDelegate(bool cookies) : cookies_(cookies) {}

	int OnBeforeStartTransaction(
		net::URLRequest *request, const net::HttpRequestHeaders &headers,
		OnBeforeStartTransactionCallback callback) override
	{
		crnet::HeaderHook *hook = crnet::HeaderHook::Get(request);
		if (!hook)
			return net::OK;
		std::optional<net::HttpRequestHeaders> out =
			hook->Finalize(*request, headers);
		// Modified headers can only be returned through the callback.
		// It is bound to a weak pointer of the job, so running it after
		// a cancellation is harmless.
		base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
			FROM_HERE,
			base::BindOnce(std::move(callback), net::OK, std::move(out)));
		return net::ERR_IO_PENDING;
	}

	bool OnAnnotateAndMoveUserBlockedCookies(
		const net::URLRequest &request,
		const net::FirstPartySetMetadata &first_party_set_metadata,
		net::CookieAccessResultList &maybe_included_cookies,
		net::CookieAccessResultList &excluded_cookies) override
	{
		if (cookies_)
			return true;
		for (auto &c : maybe_included_cookies) {
			c.access_result.status.AddExclusionReason(
				net::CookieInclusionStatus::ExclusionReason::
					EXCLUDE_USER_PREFERENCES);
			excluded_cookies.push_back(std::move(c));
		}
		maybe_included_cookies.clear();
		return false;
	}

	bool OnCanSetCookie(
		const net::URLRequest &request, const net::CanonicalCookie &cookie,
		net::CookieOptions *options,
		const net::FirstPartySetMetadata &first_party_set_metadata,
		net::CookieInclusionStatus *inclusion_status) override
	{
		return cookies_;
	}

private:
	const bool cookies_;
};

std::unique_ptr<net::ProxyConfigService> MakeProxyConfigService(
	const ClientConfig &cfg)
{
	net::ProxyConfig pc;
	if (cfg.proxy.empty())
		pc = net::ProxyConfig::CreateDirect();
	else
		pc.proxy_rules().ParseFromString(cfg.proxy);
	pc.proxy_rules().bypass_rules.ParseFromString(cfg.proxy_bypass);
	return std::make_unique<net::ProxyConfigServiceFixed>(
		net::ProxyConfigWithAnnotation(pc, kProxyAnnotation));
}

// Hands out one fixed CertVerifyProc. MultiThreadedCertVerifier needs a
// factory for parameter updates, which crnet never makes.
class FixedCertVerifyProcFactory : public net::CertVerifyProcFactory {
public:
	explicit FixedCertVerifyProcFactory(
		scoped_refptr<net::CertVerifyProc> proc)
		: proc_(std::move(proc))
	{
	}

	scoped_refptr<net::CertVerifyProc> CreateCertVerifyProc(
		scoped_refptr<net::CertNetFetcher> cert_net_fetcher,
		const net::CertVerifyProc::ImplParams &impl_params,
		const net::CertVerifyProc::InstanceParams &instance_params) override
	{
		return proc_;
	}

private:
	~FixedCertVerifyProcFactory() override = default;

	const scoped_refptr<net::CertVerifyProc> proc_;
};

// Chrome's verifier (net::CertVerifier::CreateDefault()): the Chrome Root
// Store, plus the user-added roots of the system store, with the
// configured anchors added, or with the configured anchors alone.
std::unique_ptr<net::CertVerifier> MakeCertVerifier(
	const ClientConfig &cfg, scoped_refptr<net::CertNetFetcher> fetcher)
{
	if (cfg.trust_anchors.empty() && !cfg.trust_anchors_only)
		return net::CertVerifier::CreateDefault(std::move(fetcher));

	net::CertVerifyProc::InstanceParams ip;
	for (const std::string &der : cfg.trust_anchors) {
		bssl::CertErrors errors;
		std::shared_ptr<const bssl::ParsedCertificate> cert =
			bssl::ParsedCertificate::Create(
				net::x509_util::CreateCryptoBuffer(der),
				net::x509_util::DefaultParseCertificateOptions(),
				&errors);
		if (cert)	// validated by the API already
			ip.additional_trust_anchors.push_back(std::move(cert));
	}
	net::CertVerifyProc::ImplParams impl;	// the built-in CRLSet
	scoped_refptr<net::CertVerifyProc> proc;
	if (!cfg.trust_anchors_only) {
		proc = net::CertVerifyProc::CreateBuiltinWithChromeRootStore(
			std::move(fetcher), impl.crl_set,
			std::make_unique<net::DoNothingCTVerifier>(),
			base::MakeRefCounted<net::DefaultCTPolicyEnforcer>(),
			/*root_store_data=*/nullptr,
			/*root_store_mtc_metadata=*/nullptr, ip,
			/*time_tracker=*/std::nullopt);
	} else {
		// The same verifier over an empty Chrome Root Store and no
		// system store. An empty TrustStoreChrome can only be made
		// from a root store proto, which //crnet may not depend on,
		// or with the testing factory.
		proc = net::CreateCertVerifyProcBuiltin(
			std::move(fetcher), impl.crl_set,
			std::make_unique<net::DoNothingCTVerifier>(),
			base::MakeRefCounted<net::DefaultCTPolicyEnforcer>(),
			net::CreateChromeOnlySystemTrustStore(
				net::TrustStoreChrome::CreateTrustStoreForTesting(
					{}, {}, /*version=*/0)),
			ip, /*time_tracker=*/std::nullopt);
	}
	// Wrapped as CreateDefault() does.
	auto factory = base::MakeRefCounted<FixedCertVerifyProcFactory>(proc);
	return std::make_unique<net::CachingCertVerifier>(
		std::make_unique<net::CoalescingCertVerifier>(
			std::make_unique<net::MultiThreadedCertVerifier>(
				std::move(proc), std::move(factory))));
}

void SignalEvent(base::WaitableEvent *ev)
{
	ev->Signal();
}

// "socks5://h" and "socks5://h:1080" name one proxy.
std::string ProxyKey(const std::string &proxy)
{
	return net::ProxyServerToProxyUri(net::ProxyUriToProxyServer(
		proxy, net::ProxyServer::SCHEME_HTTP));
}

// Sends the credentials of a proxy identity (crnet_client::
// ResolveProxyIdentity()) up front, to every HTTP proxy request of its
// connection partition: tunnel requests, and plain requests to the
// proxy (patched OnBeforeHttpRequestToProxy()). They replace whatever
// Chromium's proxy auth cache, which knows one set per proxy, would
// send. Network thread.
class IdentityProxyDelegate final : public net::ProxyDelegate {
public:
	explicit IdentityProxyDelegate(const crnet_client *client)
		: client_(client)
	{
	}

	void OnResolveProxy(const GURL &, const net::NetworkAnonymizationKey &,
			    const std::string &,
			    const net::ProxyRetryInfoMap &,
			    net::ProxyInfo *) override
	{
	}
	void OnFallback(const net::ProxyChain &, int) override {}
	void OnSuccessfulRequestAfterFailures(
		const net::ProxyRetryInfoMap &) override
	{
	}
	base::expected<net::HttpRequestHeaders, net::Error>
	OnBeforeTunnelRequest(const net::ProxyChain &chain, size_t index,
			      OnBeforeTunnelRequestCallback) override
	{
		net::HttpRequestHeaders headers;
		AddCredentials(chain, index, &headers);
		return headers;
	}
	net::Error OnTunnelHeadersReceived(const net::ProxyChain &, size_t,
					   const net::HttpResponseHeaders &,
					   net::CompletionOnceCallback) override
	{
		return net::OK;
	}
	void SetProxyResolutionService(net::ProxyResolutionService *) override
	{
	}
	void OnBeforeHttpRequestToProxy(const net::ProxyChain &chain,
					net::HttpRequestHeaders *headers) override
	{
		AddCredentials(chain, 0, headers);
	}

private:
	void AddCredentials(const net::ProxyChain &chain, size_t index,
			    net::HttpRequestHeaders *headers) const
	{
		if (!chain.opaque_data() || index >= chain.length())
			return;
		const crnet_client::ProxyIdentity *id =
			client_->PartitionIdentity(*chain.opaque_data());
		const net::ProxyServer &server = chain.GetProxyServer(index);
		if (!id || !server.is_http_like() ||
		    ProxyKey(id->proxy) != net::ProxyServerToProxyUri(server))
			return;
		headers->SetHeader(
			net::HttpRequestHeaders::kProxyAuthorization,
			"Basic " + base::Base64Encode(id->user + ":" + id->pass));
	}

	const raw_ptr<const crnet_client> client_;
};


} // namespace

net::ProxyChain crnet::ProxyChainOf(const std::string &proxy)
{
	if (proxy.empty())
		return net::ProxyChain::Direct();
	return net::ProxyUriToProxyChain(proxy, net::ProxyServer::SCHEME_HTTP);
}

crnet_client::crnet_client(const ClientConfig &cfg)
	: cfg_(cfg), thread_("crnet-io")
{
	// The configured proxy's credentials come first (its SOCKS5 key is
	// registered in Create()).
	if (!cfg.proxy.empty() && !cfg.proxy_user.empty())
		proxy_creds_[ProxyKey(cfg.proxy)] = {cfg.proxy_user,
						     cfg.proxy_pass};
}

int crnet_client::NewConnectionPartition(std::optional<int> identity)
{
	DCHECK(IsNetworkThread());
	// Process-wide, as the SOCKS5 credentials table is. A fresh partition
	// is closed when its one user is gone, so numbers could only clash
	// after 2^31 partitions with an identity still in use.
	static std::atomic<uint32_t> last{0};
	int partition = static_cast<int>(
		last.fetch_add(1, std::memory_order_relaxed) %
			std::numeric_limits<int>::max() +
		1);
	if (identity) {
		ProxyIdentity id = partition_identities_.at(*identity);
		AddPartitionIdentity(partition, std::move(id));
	}
	return partition;
}

void crnet_client::AddPartitionIdentity(int partition, ProxyIdentity id)
{
	const std::string socks_key = crnet::SocksProxyKey(id.proxy);
	id.socks_key.clear();
	if (!socks_key.empty()) {
		// Unique to the partition, so it cannot clash.
		id.socks_key = crnet::SocksPartitionKey(socks_key, partition);
		crnet::RegisterSocksCredentials(id.socks_key, id.user, id.pass);
	}
	partition_identities_[partition] = std::move(id);
}

void crnet_client::CloseConnectionPartition(int partition)
{
	DCHECK(IsNetworkThread());
	if (context_)
		context_->http_transaction_factory()
			->GetSession()
			->CloseConnectionPartition(partition);
	auto it = partition_identities_.find(partition);
	if (it != partition_identities_.end()) {
		if (!it->second.socks_key.empty())
			crnet::UnregisterSocksCredentials(it->second.socks_key);
		partition_identities_.erase(it);
	}
}

const crnet_client::ProxyIdentity *crnet_client::PartitionIdentity(
	int partition) const
{
	auto it = partition_identities_.find(partition);
	return it == partition_identities_.end() ? nullptr : &it->second;
}

std::optional<int> crnet_client::ResolveProxyIdentity(const std::string &proxy,
						      std::string *user,
						      std::string *pass)
{
	DCHECK(IsNetworkThread());
	if (proxy.empty())
		return std::nullopt;
	const std::string key = ProxyKey(proxy);
	auto it = proxy_creds_.find(key);
	if (user->empty()) {
		if (it != proxy_creds_.end()) {
			*user = it->second.first;
			*pass = it->second.second;
		}
		return std::nullopt;
	}
	if (it != proxy_creds_.end() &&
	    it->second == std::make_pair(*user, *pass))
		return std::nullopt;
	if (it == proxy_creds_.end()) {
		// The first credentials named become the proxy's default, unless
		// another client uses other default ones for this SOCKS5 proxy.
		std::string socks_key = crnet::SocksProxyKey(proxy);
		if (socks_key.empty() ||
		    crnet::RegisterSocksCredentials(socks_key, *user, *pass)) {
			if (!socks_key.empty())
				request_socks_keys_.push_back(std::move(socks_key));
			proxy_creds_[key] = {*user, *pass};
			return std::nullopt;
		}
	}
	auto id = identities_.find({key, *user, *pass});
	if (id != identities_.end())
		return id->second;
	int partition = NewConnectionPartition(std::nullopt);
	AddPartitionIdentity(partition, {proxy, *user, *pass, ""});
	identities_[{key, *user, *pass}] = partition;
	return partition;
}

crnet_client::~crnet_client() = default;

crnet_client *crnet_client::Create(const ClientConfig &cfg)
{
	// SOCKS5 credentials go into the process-wide table the patched
	// SOCKS5 client reads (crnet/socks_auth.h).
	std::string socks_key;
	if (!cfg.proxy_user.empty()) {
		socks_key = crnet::SocksProxyKey(cfg.proxy);
		if (!socks_key.empty() &&
		    !crnet::RegisterSocksCredentials(socks_key, cfg.proxy_user,
						     cfg.proxy_pass))
			return nullptr;
	}
	auto *c = new crnet_client(cfg);
	c->socks_key_ = std::move(socks_key);
	base::Thread::Options opts(base::MessagePumpType::IO, 0);
	if (!c->thread_.StartWithOptions(std::move(opts))) {
		if (!c->socks_key_.empty())
			crnet::UnregisterSocksCredentials(c->socks_key_);
		delete c;
		return nullptr;
	}
	c->runner_ = c->thread_.task_runner();
	// Made here, not on the network thread, so Destroy() can use it.
	if (!cfg.state_dir.empty())
		c->state_runner_ = crnet::CreateStateRunner();

	// No need to wait for the context: everything later posted to this
	// thread (requests, cookie calls, shutdown) runs after this task. A
	// context that failed to build fails those with CRNET_E_SHUTDOWN.
	c->runner_->PostTask(FROM_HERE,
			     base::BindOnce(
				     [](crnet_client *c) {
					     crnet::MarkNetworkThread();
					     if (!c->InitOnNetworkThread())
						     c->context_.reset();
				     },
				     c));
	return c;
}

bool crnet_client::InitOnNetworkThread()
{
	net::URLRequestContextBuilder b;

	b.set_user_agent(crnet::UserAgentOf(cfg_));
	b.set_accept_language(cfg_.accept_language);
	b.set_enable_brotli(true);
	b.set_enable_zstd(true);
	if (!cfg_.binding.empty())
		b.set_client_socket_factory(
			crnet::CreateBindingSocketFactory(cfg_.binding));
	if (cfg_.http_cache == CRNET_HTTP_CACHE_OFF) {
		b.DisableHttpCache();
	} else {
		net::URLRequestContextBuilder::HttpCacheParams cp;
		cp.type = cfg_.http_cache == CRNET_HTTP_CACHE_DISK
				  ? net::URLRequestContextBuilder::HttpCacheParams::DISK
				  : net::URLRequestContextBuilder::HttpCacheParams::IN_MEMORY;
		cp.path = base::FilePath(cfg_.http_cache_path);
		cp.max_size = static_cast<int>(cfg_.http_cache_max_size);
		b.EnableHttpCache(cp);
	}
	b.set_proxy_config_service(MakeProxyConfigService(cfg_));
	b.set_proxy_delegate(std::make_unique<IdentityProxyDelegate>(this));
	b.set_ssl_config_service(std::make_unique<ChromeSSLConfigService>());
	b.set_network_delegate(std::make_unique<CrNetworkDelegate>(cfg_.cookies));

	// Chrome on Linux resolves with its built-in DNS client, which also
	// fetches HTTPS records (ECH configs, ALPN hints).
	net::HostResolver::ManagerOptions mo;
	mo.insecure_dns_mode = cfg_.builtin_dns
				       ? net::InsecureDnsMode::kEnabledBuiltIn
				       : net::InsecureDnsMode::kDisabled;
	mo.additional_types_via_insecure_dns_enabled = true;
	// Chrome applies its secure DNS setting the same way, as overrides
	// of the system configuration (network::NetworkService::
	// ConfigureStubHostResolver()).
	if (cfg_.secure_dns != CRNET_SECURE_DNS_OFF) {
		mo.dns_config_overrides.dns_over_https_config =
			net::DnsOverHttpsConfig::FromString(cfg_.doh_templates);
		mo.dns_config_overrides.secure_dns_mode =
			cfg_.secure_dns == CRNET_SECURE_DNS_SECURE
				? net::SecureDnsMode::kSecure
				: net::SecureDnsMode::kAutomatic;
	}
	b.set_host_resolver(net::HostResolver::CreateStandaloneResolver(
		net::NetLog::Get(), std::move(mo), cfg_.host_rules,
		/*enable_caching=*/true));

	// HTTP/2 and QUIC parameters exactly as Chrome derives them
	// (services/network/network_context.cc).
	net::HttpNetworkSessionParams sp;
	auto quic = std::make_unique<net::QuicContext>();
	network_session_configurator::ParseCommandLineAndFieldTrials(
		*base::CommandLine::ForCurrentProcess(), !cfg_.quic, &sp,
		quic->params());
	sp.enable_http2 = cfg_.http2;
	sp.enable_quic = cfg_.quic;
	sp.ignore_certificate_errors = cfg_.insecure;
	b.set_http_network_session_params(sp);
	b.set_quic_context(std::move(quic));

	// Chrome fetches missing intermediates (AIA) through the network.
	cert_fetcher_ = base::MakeRefCounted<net::CertNetFetcherURLRequest>();
	b.SetCertVerifier(MakeCertVerifier(cfg_, cert_fetcher_));

	if (state_runner_) {
		base::FilePath dir(cfg_.state_dir);
		state_runner_->PostTask(
			FROM_HERE, base::BindOnce(base::IgnoreResult(
							  &base::CreateDirectory),
						  dir));
		b.SetHttpServerProperties(std::make_unique<net::HttpServerProperties>(
			crnet::CreateServerPropertiesStore(
				dir.Append(crnet::kServerPropertiesFile),
				state_runner_),
			net::NetLog::Get()));
	}

	context_ = b.Build();
	if (!context_)
		return false;
	cert_fetcher_->SetURLRequestContext(context_.get());

	if (state_runner_) {
		hsts_persister_ = std::make_unique<net::TransportSecurityPersister>(
			context_->transport_security_state(), state_runner_,
			base::FilePath(cfg_.state_dir)
				.Append(crnet::kTransportSecurityFile));
	}

	// Automatic mode only uses a DoH server that a probe found working,
	// and nothing probes unless asked; Chrome starts them per network
	// context (network::NetworkContext::ActivateDohProbes()).
	if (cfg_.secure_dns != CRNET_SECURE_DNS_OFF) {
		doh_probes_ = context_->host_resolver()->CreateDohProbeRequest();
		doh_probes_->Start();
	}

	if (!cfg_.netlog_path.empty()) {
		netlog_ = net::FileNetLogObserver::CreateUnbounded(
			base::FilePath(cfg_.netlog_path),
			net::NetLogCaptureMode::kIncludeSensitive, nullptr);
		netlog_->StartObserving(net::NetLog::Get());
	}
	return true;
}

void crnet_client::Destroy()
{
	shutting_down_.store(true);
	if (state_runner_) {
		// Shutting down saves the state, so it must have been loaded
		// first or the save would replace it with what little this
		// client learned. Once the context exists the loads are queued
		// on state_runner_; once that drained, their results are
		// queued on the network thread ahead of the shutdown.
		base::WaitableEvent inited;
		runner_->PostTask(FROM_HERE,
				  base::BindOnce(&base::WaitableEvent::Signal,
						 base::Unretained(&inited)));
		inited.Wait();
		crnet::FlushStateRunner(state_runner_.get());
	}
	base::WaitableEvent ev;
	runner_->PostTask(FROM_HERE,
			  base::BindOnce(&crnet_client::ShutdownOnNetworkThread,
					 base::Unretained(this), &ev));
	ev.Wait();
	thread_.Stop();
	// The state files are written once the saves posted at shutdown ran.
	if (state_runner_)
		crnet::FlushStateRunner(state_runner_.get());
	if (!socks_key_.empty())
		crnet::UnregisterSocksCredentials(socks_key_);
	for (const std::string &key : request_socks_keys_)
		crnet::UnregisterSocksCredentials(key);
	for (const auto &[partition, id] : partition_identities_) {
		if (!id.socks_key.empty())
			crnet::UnregisterSocksCredentials(id.socks_key);
	}
	delete this;
}

void crnet_client::ShutdownOnNetworkThread(base::WaitableEvent *ev)
{
	// Fail everything still running; Finish() removes it from running_
	// and sockets_.
	auto running = running_;
	for (const auto &t : running)
		t->FailOnNetworkThread(CRNET_E_SHUTDOWN, net::ERR_ABORTED);
	auto sockets = sockets_;
	for (const auto &ws : sockets)
		ws->FailOnNetworkThread(CRNET_E_SHUTDOWN, net::ERR_ABORTED);

	// URLRequests are deleted with DeleteSoon(); tear the context down
	// in a later task so they are gone first.
	runner_->PostTask(
		FROM_HERE,
		base::BindOnce(
			[](crnet_client *c, base::WaitableEvent *ev) {
				if (c->cert_fetcher_)
					c->cert_fetcher_->Shutdown();
				c->doh_probes_.reset();
				// Both persisters post their final save to
				// state_runner_ as they go.
				c->hsts_persister_.reset();
				c->context_.reset();
				c->cert_fetcher_ = nullptr;
				if (!c->netlog_) {
					ev->Signal();
					return;
				}
				// The reply runs on this (still alive) thread once
				// the file is complete; the observer itself may go
				// away right now.
				c->netlog_->StopObserving(
					nullptr, base::BindOnce(&SignalEvent, ev));
				c->netlog_.reset();
			},
			base::Unretained(this), ev));
}

bool crnet_client::Start(scoped_refptr<crnet_transfer> t)
{
	if (shutting_down_.load())
		return false;
	return runner_->PostTask(
		FROM_HERE,
		base::BindOnce(
			[](crnet_client *c, scoped_refptr<crnet_transfer> t) {
				c->running_.insert(t);
				if (!c->context_ || c->shutting_down_.load()) {
					t->FailOnNetworkThread(CRNET_E_SHUTDOWN,
							       net::ERR_ABORTED);
					return;
				}
				t->StartOnNetworkThread();
			},
			base::Unretained(this), std::move(t)));
}

void crnet_client::TransferFinished(crnet_transfer *t)
{
	running_.erase(base::WrapRefCounted(t));
}

bool crnet_client::StartWebSocket(scoped_refptr<crnet_ws> ws)
{
	if (shutting_down_.load())
		return false;
	return runner_->PostTask(
		FROM_HERE,
		base::BindOnce(
			[](crnet_client *c, scoped_refptr<crnet_ws> ws) {
				c->sockets_.insert(ws);
				if (!c->context_ || c->shutting_down_.load()) {
					ws->FailOnNetworkThread(CRNET_E_SHUTDOWN,
								net::ERR_ABORTED);
					return;
				}
				ws->StartOnNetworkThread();
			},
			base::Unretained(this), std::move(ws)));
}

void crnet_client::WebSocketFinished(crnet_ws *ws)
{
	sockets_.erase(base::WrapRefCounted(ws));
}

// ------------------------------------------------------------------
// Cookie jar and pool helpers (blocking)
// ------------------------------------------------------------------

int crnet_client::SetCookie(const std::string &url, const std::string &line)
{
	GURL u(url);
	if (!u.is_valid())
		return CRNET_E_INVAL;
	if (!cfg_.cookies)
		return CRNET_E_STATE;

	int result = CRNET_E_INVAL;
	base::WaitableEvent ev;
	runner_->PostTask(
		FROM_HERE,
		base::BindOnce(
			[](crnet_client *c, GURL u, std::string line, int *result,
			   base::WaitableEvent *ev) {
				if (!c->context_) {
					*result = CRNET_E_SHUTDOWN;
					ev->Signal();
					return;
				}
				net::CookieInclusionStatus status;
				auto cc = net::CanonicalCookie::Create(
					u, line, base::Time::Now(), std::nullopt,
					std::nullopt, net::CookieSourceType::kOther,
					&status);
				if (!cc) {
					ev->Signal();
					return;
				}
				c->context_->cookie_store()->SetCanonicalCookieAsync(
					std::move(cc), u,
					net::CookieOptions::MakeAllInclusive(),
					base::BindOnce(
						[](int *result, base::WaitableEvent *ev,
						   net::CookieAccessResult r) {
							*result = r.status.IsInclude()
									  ? CRNET_OK
									  : CRNET_E_INVAL;
							ev->Signal();
						},
						result, ev),
					std::nullopt);
			},
			base::Unretained(this), u, line, &result, &ev));
	ev.Wait();
	return result;
}

std::optional<std::string> crnet_client::GetCookies(const std::string &url)
{
	GURL u(url);
	if (!u.is_valid())
		return std::nullopt;
	if (!cfg_.cookies)
		return std::string();

	std::string line;
	base::WaitableEvent ev;
	runner_->PostTask(
		FROM_HERE,
		base::BindOnce(
			[](crnet_client *c, GURL u, std::string *line,
			   base::WaitableEvent *ev) {
				if (!c->context_) {
					ev->Signal();
					return;
				}
				c->context_->cookie_store()->GetCookieListWithOptionsAsync(
					u, net::CookieOptions::MakeAllInclusive(),
					net::CookiePartitionKeyCollection(),
					base::BindOnce(
						[](std::string *line,
						   base::WaitableEvent *ev,
						   const net::CookieAccessResultList &in,
						   const net::CookieAccessResultList &) {
							*line = net::CanonicalCookie::
								BuildCookieLine(in);
							ev->Signal();
						},
						line, ev));
			},
			base::Unretained(this), u, &line, &ev));
	ev.Wait();
	return line;
}

std::string crnet_client::ExportCookies(crnet_cookie_format format)
{
	net::CookieList cookies;
	if (cfg_.cookies) {
		base::WaitableEvent ev;
		runner_->PostTask(
			FROM_HERE,
			base::BindOnce(
				[](crnet_client *c, net::CookieList *out,
				   base::WaitableEvent *ev) {
					if (!c->context_) {
						ev->Signal();
						return;
					}
					c->context_->cookie_store()->GetAllCookiesAsync(
						base::BindOnce(
							[](net::CookieList *out,
							   base::WaitableEvent *ev,
							   const net::CookieList &all) {
								*out = all;
								ev->Signal();
							},
							out, ev));
				},
				base::Unretained(this), &cookies, &ev));
		ev.Wait();
	}
	return crnet::ExportCookies(cookies, format);
}

int crnet_client::ImportCookies(const std::string &data,
				crnet_cookie_format format, size_t *imported)
{
	*imported = 0;
	if (!cfg_.cookies)
		return CRNET_E_STATE;
	// All or nothing as far as parsing goes: nothing is stored unless
	// every cookie is well-formed.
	std::vector<std::unique_ptr<net::CanonicalCookie>> cookies;
	if (!crnet::ParseCookies(data, format, &cookies))
		return CRNET_E_INVAL;

	struct Import {
		size_t pending = 0;
		size_t ok = 0;
		int result = CRNET_OK;
		base::WaitableEvent done;
	} state;
	state.pending = cookies.size();
	if (!state.pending)
		return CRNET_OK;
	runner_->PostTask(
		FROM_HERE,
		base::BindOnce(
			[](crnet_client *c,
			   std::vector<std::unique_ptr<net::CanonicalCookie>> cookies,
			   Import *state) {
				if (!c->context_) {
					state->result = CRNET_E_SHUTDOWN;
					state->done.Signal();
					return;
				}
				for (auto &cc : cookies) {
					// Set as if received from the site that
					// set it, so Chromium checks it as it would
					// a Set-Cookie header.
					std::string host(base::TrimString(
						cc->Domain(), ".",
						base::TRIM_LEADING));
					bool https = cc->SecureAttribute() ||
						     cc->SourceScheme() ==
							     net::CookieSourceScheme::kSecure;
					std::string port;
					if (cc->SourcePort() > 0)
						port = base::StrCat(
							{":", base::NumberToString(
								      cc->SourcePort())});
					GURL source(base::StrCat(
						{https ? "https://" : "http://", host,
						 port, cc->Path()}));
					c->context_->cookie_store()->SetCanonicalCookieAsync(
						std::move(cc), source,
						net::CookieOptions::MakeAllInclusive(),
						base::BindOnce(
							[](Import *state,
							   net::CookieAccessResult r) {
								if (r.status.IsInclude())
									state->ok++;
								else
									state->result =
										CRNET_E_INVAL;
								if (!--state->pending)
									state->done.Signal();
							},
							state),
						std::nullopt);
				}
			},
			base::Unretained(this), std::move(cookies), &state));
	state.done.Wait();
	*imported = state.ok;
	return state.result;
}

int crnet_client::ClearCookies()
{
	if (!cfg_.cookies)
		return CRNET_E_STATE;
	base::WaitableEvent ev;
	runner_->PostTask(
		FROM_HERE,
		base::BindOnce(
			[](crnet_client *c, base::WaitableEvent *ev) {
				if (!c->context_) {
					ev->Signal();
					return;
				}
				c->context_->cookie_store()->DeleteAllAsync(
					base::BindOnce(
						[](base::WaitableEvent *ev, uint32_t) {
							ev->Signal();
						},
						ev));
			},
			base::Unretained(this), &ev));
	ev.Wait();
	return CRNET_OK;
}

void crnet_client::CloseIdle()
{
	runner_->PostTask(
		FROM_HERE,
		base::BindOnce(
			[](crnet_client *c) {
				if (c->context_)
					c->context_->http_transaction_factory()
						->GetSession()
						->CloseIdleConnections("crnet");
			},
			base::Unretained(this)));
}
