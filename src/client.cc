// SPDX-License-Identifier: BSD-3-Clause
//
// crnet_client: a URLRequestContext configured like Chrome's, living
// on a dedicated IO thread. All transfers of a client share that
// thread, its connection pools, HTTP/2 sessions and cookie jar.
//
#include <memory>
#include <string_view>

#include "base/command_line.h"
#include "base/containers/extend.h"
#include "base/feature_list.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/message_loop/message_pump_type.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "components/network_session_configurator/browser/network_session_configurator.h"
#include "crnet/internal.h"
#include "net/base/features.h"
#include "net/base/network_delegate_impl.h"
#include "net/cert/cert_verifier.h"
#include "net/cert/x509_util.h"
#include "net/cert_net/cert_net_fetcher_url_request.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_partition_key_collection.h"
#include "net/cookies/cookie_store.h"
#include "net/dns/host_resolver.h"
#include "net/dns/public/insecure_dns_mode.h"
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

#if BUILDFLAG(CHROME_ROOT_STORE_SUPPORTED)
#include "net/cert/internal/trust_store_chrome.h"
#endif

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
	return std::make_unique<net::ProxyConfigServiceFixed>(
		net::ProxyConfigWithAnnotation(pc, kProxyAnnotation));
}

void SignalEvent(base::WaitableEvent *ev)
{
	ev->Signal();
}

} // namespace

crnet_client::crnet_client(const ClientConfig &cfg)
	: cfg_(cfg), thread_("crnet-io")
{
}

crnet_client::~crnet_client() = default;

crnet_client *crnet_client::Create(const ClientConfig &cfg)
{
	auto *c = new crnet_client(cfg);
	base::Thread::Options opts(base::MessagePumpType::IO, 0);
	if (!c->thread_.StartWithOptions(std::move(opts))) {
		delete c;
		return nullptr;
	}
	c->runner_ = c->thread_.task_runner();

	bool ok = false;
	base::WaitableEvent ev;
	c->runner_->PostTask(
		FROM_HERE,
		base::BindOnce(
			[](crnet_client *c, bool *ok, base::WaitableEvent *ev) {
				crnet::MarkNetworkThread();
				*ok = c->InitOnNetworkThread();
				ev->Signal();
			},
			c, &ok, &ev));
	ev.Wait();
	if (!ok) {
		c->Destroy();
		return nullptr;
	}
	return c;
}

bool crnet_client::InitOnNetworkThread()
{
	net::URLRequestContextBuilder b;

	b.set_user_agent(cfg_.user_agent.empty() ? crnet::DefaultUserAgent()
						 : cfg_.user_agent);
	b.set_accept_language(cfg_.accept_language);
	b.set_enable_brotli(true);
	b.set_enable_zstd(true);
	b.DisableHttpCache();
	b.set_proxy_config_service(MakeProxyConfigService(cfg_));
	b.set_ssl_config_service(std::make_unique<ChromeSSLConfigService>());
	b.set_network_delegate(std::make_unique<CrNetworkDelegate>(cfg_.cookies));

	// Chrome on Linux resolves with its built-in DNS client, which also
	// fetches HTTPS records (ECH configs, ALPN hints).
	net::HostResolver::ManagerOptions mo;
	mo.insecure_dns_mode = cfg_.builtin_dns
				       ? net::InsecureDnsMode::kEnabledBuiltIn
				       : net::InsecureDnsMode::kDisabled;
	mo.additional_types_via_insecure_dns_enabled = true;
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
	b.SetCertVerifier(net::CertVerifier::CreateDefault(cert_fetcher_));

	context_ = b.Build();
	if (!context_)
		return false;
	cert_fetcher_->SetURLRequestContext(context_.get());

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
	base::WaitableEvent ev;
	runner_->PostTask(FROM_HERE,
			  base::BindOnce(&crnet_client::ShutdownOnNetworkThread,
					 base::Unretained(this), &ev));
	ev.Wait();
	thread_.Stop();
	delete this;
}

void crnet_client::ShutdownOnNetworkThread(base::WaitableEvent *ev)
{
	// Fail everything still running; Finish() removes it from running_.
	auto running = running_;
	for (const auto &t : running)
		t->FailOnNetworkThread(CRNET_E_SHUTDOWN, net::ERR_ABORTED);

	// URLRequests are deleted with DeleteSoon(); tear the context down
	// in a later task so they are gone first.
	runner_->PostTask(
		FROM_HERE,
		base::BindOnce(
			[](crnet_client *c, base::WaitableEvent *ev) {
				if (c->cert_fetcher_)
					c->cert_fetcher_->Shutdown();
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

int crnet_client::ClearCookies()
{
	if (!cfg_.cookies)
		return CRNET_E_STATE;
	base::WaitableEvent ev;
	runner_->PostTask(
		FROM_HERE,
		base::BindOnce(
			[](crnet_client *c, base::WaitableEvent *ev) {
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
