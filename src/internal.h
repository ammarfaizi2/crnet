// SPDX-License-Identifier: BSD-3-Clause
//
// Internal declarations shared by the crnet implementation. Nothing in
// here is visible to library users; the public ABI is crnet.h only.
//
#ifndef CRNET_SRC_INTERNAL_H
#define CRNET_SRC_INTERNAL_H

#include <stdint.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "crnet/crnet.h"
#include "crnet/event.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace base {
class WaitableEvent;
}

namespace net {
class CertNetFetcherURLRequest;
class FileNetLogObserver;
}

struct crnet_transfer;
struct crnet_client;
struct crnet_queue;

namespace crnet {

// ---------------------------------------------------------------------
// Process-wide state (global.cc)
// ---------------------------------------------------------------------

// Initialise Chromium's process-wide singletons once. Returns false if
// initialisation failed. Thread-safe.
bool EnsureGlobalInit(const char *enable_features,
		      const char *disable_features, bool *already);

// True on any crnet network thread. Blocking calls refuse to run there.
bool OnNetworkThread();
void MarkNetworkThread();

// ---------------------------------------------------------------------
// Configuration and request descriptions (plain data)
// ---------------------------------------------------------------------

struct ClientConfig {
	std::string proxy;		// Chromium proxy rules, "" = direct
	std::string proxy_user;
	std::string proxy_pass;
	std::string proxy_bypass;	// Chromium bypass rules, "" = none
	std::string user_agent;		// "" = build default
	std::string sec_ch_ua;		// "" = build default
	std::string platform = "Linux";
	std::string accept_language = "en-US,en;q=0.9";
	bool http2 = true;
	bool quic = true;
	bool builtin_dns = true;
	bool cookies = true;
	bool insecure = false;
	// Extra roots (DER), and whether they replace the Chrome Root Store.
	std::vector<std::string> trust_anchors;
	bool trust_anchors_only = false;
	std::string host_rules;
	uint64_t timeout_ms = 0;
	uint64_t connect_timeout_ms = 0;
	std::string netlog_path;
};

struct RequestSpec {
	std::string method = "GET";
	GURL url;
	// User headers in the order they were first set. A nullopt value
	// means "remove / suppress the default".
	std::vector<std::pair<std::string, std::optional<std::string>>> headers;
	// At most one of body, body_file and body_stream is set.
	std::optional<std::string> body;
	struct File {
		std::string path;
		uint64_t offset = 0;
		uint64_t length = UINT64_MAX;
	};
	std::optional<File> body_file;
	std::optional<int64_t> body_stream;	// declared length, -1 unknown
	bool stream_response = false;
	crnet_mode mode = CRNET_MODE_NAVIGATE;
	crnet_priority priority = CRNET_PRIORITY_DEFAULT;
	int max_redirects = 20;
	std::optional<uint64_t> timeout_ms;
	std::optional<uint64_t> connect_timeout_ms;
	bool decompress = true;
	uint64_t max_body = 0;

	const std::optional<std::string> *FindHeader(std::string_view name) const;
};

// ---------------------------------------------------------------------
// Browser emulation (profile.cc)
// ---------------------------------------------------------------------

const std::string &DefaultUserAgent();
const std::string &DefaultSecChUa();

// Per-request emulation state computed when the transfer starts.
struct BrowserHeaders {
	// Headers to install with URLRequest::SetExtraRequestHeaders().
	net::HttpRequestHeaders extra;
	// Explicit Referer (handled through URLRequest::SetReferrer()).
	std::optional<std::string> referrer;
	// Caller-provided Cookie value, merged with the jar later.
	std::optional<std::string> user_cookie;
	// Origin the request is "initiated" from (for sec-fetch-site and
	// SameSite cookies). nullopt for browser-initiated navigations.
	std::optional<url::Origin> initiator;
	// Whether sec-fetch-site should be recomputed on each hop.
	bool auto_fetch_site = false;
	// Header names the caller removed explicitly (lower case).
	std::set<std::string> suppressed;
	net::RequestPriority priority = net::DEFAULT_PRIORITY;
};

BrowserHeaders BuildBrowserHeaders(const ClientConfig &cfg,
				   const RequestSpec &spec);

// WebSocket handshake policy: what crnet hands to
// WebSocketChannel::SendAddChannelRequest().
struct WsHeaders {
	// Additional headers, User-Agent first as Chrome's renderer passes
	// it; //net adds the rest (Upgrade, Origin, Sec-WebSocket-*, ...).
	net::HttpRequestHeaders extra;
	url::Origin origin;
	std::vector<std::string> protocols;
	std::optional<std::string> user_cookie;
	std::set<std::string> suppressed;
};

// |url| must already be ws:// or wss://. Fails (nullopt) on an invalid
// Origin header or subprotocol list.
std::optional<WsHeaders> BuildWsHeaders(const ClientConfig &cfg,
					const RequestSpec &spec,
					const GURL &url,
					const char *protocols);

// Drop |suppressed| headers (lower-case names) that //net added back
// and merge a caller-supplied Cookie value with the jar's, keeping the
// order of |in|.
net::HttpRequestHeaders FilterHeaders(
	const net::HttpRequestHeaders &in, const std::set<std::string> &suppressed,
	const std::optional<std::string> &user_cookie);

// Final rewrite right before the transaction starts: FilterHeaders(),
// refresh sec-fetch-site for the current URL chain and put everything
// in Chrome's order. Returns the headers to send.
net::HttpRequestHeaders FinalizeHeaders(const RequestSpec &spec,
					const BrowserHeaders &bh,
					const net::URLRequest &req,
					const net::HttpRequestHeaders &in);

// ---------------------------------------------------------------------
// Objects behind the C handles
// ---------------------------------------------------------------------

std::string NetErrorName(int net_error);

// Attached to every URLRequest whose headers crnet controls. The
// NetworkDelegate asks it for the final request headers right before
// each transaction (including after redirects), once //net has added
// everything of its own.
class HeaderHook : public base::SupportsUserData::Data {
public:
	virtual net::HttpRequestHeaders Finalize(
		const net::URLRequest &req,
		const net::HttpRequestHeaders &in) = 0;

	static void Attach(net::URLRequest *req,
			   std::unique_ptr<HeaderHook> hook);
	static HeaderHook *Get(const net::URLRequest *req);
};

} // namespace crnet

struct crnet_client_config {
	crnet::ClientConfig cfg;
};

struct crnet_request {
	crnet::RequestSpec spec;
};

struct crnet_response {
	crnet_response() { timing.fill(-1); }

	int status = 0;
	std::string status_text;
	std::string url;
	int redirects = 0;
	std::vector<std::pair<std::string, std::string>> headers;
	std::string body;
	std::string protocol;
	std::string remote;
	bool via_proxy = false;
	// Content codings removed from the body, "" when it is as received.
	std::string decoded_codings;
	bool socket_reused = false;
	uint64_t connection_id = 0;
	// Microseconds since the transfer was created, -1 = did not happen.
	std::array<int64_t, CRNET_TIMING_COUNT> timing;
	int net_error = 0;
	std::string net_error_str;
};

struct crnet_queue;
struct crnet_ws;

namespace crnet {
class DownloadBuffer;
class UploadPipe;
}

// One execution of a request. Ref-counted: the caller's handle, the
// client's running set and a completion queue may each hold a
// reference. All URLRequest work happens on the client's network
// thread; other threads only see the result once done_ is signalled.
struct crnet_transfer : public base::RefCountedThreadSafe<crnet_transfer>,
			 public net::URLRequest::Delegate {
public:
	crnet_transfer(crnet_client *client, crnet::RequestSpec spec,
		       const crnet_callbacks *cb, void *user_data,
		       crnet_queue *queue);

	crnet_transfer(const crnet_transfer &) = delete;
	crnet_transfer &operator=(const crnet_transfer &) = delete;

	// Any thread.
	void Cancel(int reason);
	void Pause();
	void Resume();
	int Wait(uint64_t timeout_ms);
	bool done() const { return done_.IsSignaled(); }
	int WaitResponse(uint64_t timeout_ms);
	// Streamed bodies (stream.h); null unless requested.
	crnet::UploadPipe *upload() const { return upload_.get(); }
	crnet::DownloadBuffer *download() const { return download_.get(); }
	bool ok() const;
	// Any thread: continue a download stopped by a full buffer.
	void ResumeDownload();
	// Any thread.
	int64_t progress(crnet_progress which) const
	{
		return progress_[which].load(std::memory_order_relaxed);
	}
	// CRNET_E_PENDING until on_complete has been entered.
	int result() const
	{
		return completing_.IsSignaled() ? result_ : CRNET_E_PENDING;
	}
	crnet_response *response() const { return resp_.get(); }
	crnet_response *TakeResponse();
	void *user_data() const { return user_data_; }

	// Network thread.
	void StartOnNetworkThread();
	void FailOnNetworkThread(int err, int net_error);
	const crnet::BrowserHeaders &browser_headers() const { return bh_; }
	const crnet::RequestSpec &spec() const { return spec_; }

	// net::URLRequest::Delegate
	int OnConnected(net::URLRequest *request,
			const net::TransportInfo &info,
			net::CompletionOnceCallback callback) override;
	void OnReceivedRedirect(net::URLRequest *request,
				const net::RedirectInfo &redirect_info,
				bool *defer_redirect) override;
	void OnAuthRequired(net::URLRequest *request,
			    const net::AuthChallengeInfo &auth_info) override;
	void OnCertificateRequested(
		net::URLRequest *request,
		net::SSLCertRequestInfo *cert_request_info) override;
	void OnSSLCertificateError(net::URLRequest *request, int net_error,
				   const net::SSLInfo &ssl_info,
				   bool fatal) override;
	void OnResponseStarted(net::URLRequest *request, int net_error) override;
	void OnReadCompleted(net::URLRequest *request, int bytes_read) override;

private:
	friend class base::RefCountedThreadSafe<crnet_transfer>;
	~crnet_transfer() override;

	void FillResponseMeta(net::URLRequest *request);
	static std::string DecodedCodings(net::URLRequest *request);
	void FillTiming(net::URLRequest *request);
	void SetProgress(crnet_progress which, int64_t v)
	{
		progress_[which].store(v, std::memory_order_relaxed);
	}
	void UpdateUploadProgress();
	void ReadMore();
	// Returns true if reading should continue.
	bool HandleRead(int bytes);
	void Finish(int err, int net_error);
	void OnTimeout();
	void StartConnectTimer();
	void ResumeOnNetworkThread();
	void ResumeDownloadOnNetworkThread();

	crnet_client *client_;
	scoped_refptr<base::SingleThreadTaskRunner> runner_;
	crnet::RequestSpec spec_;
	crnet_callbacks cb_{};
	void *user_data_;
	crnet_queue *queue_;
	// Zero point of the response's timing information.
	const base::TimeTicks created_ = base::TimeTicks::Now();

	// Network thread only.
	crnet::BrowserHeaders bh_;
	std::unique_ptr<net::URLRequest> request_;
	scoped_refptr<net::IOBufferWithSize> buf_;
	base::OneShotTimer timer_;
	base::RepeatingTimer upload_timer_;
	base::OneShotTimer connect_timer_;
	bool started_reading_ = false;
	bool read_pending_ = false;
	bool paused_ = false;
	bool finished_ = false;
	bool proxy_auth_tried_ = false;
	bool download_full_ = false;
	uint64_t body_bytes_ = 0;
	scoped_refptr<base::TaskRunner> file_runner_;

	std::unique_ptr<crnet_response> resp_;

	scoped_refptr<crnet::UploadPipe> upload_;
	std::unique_ptr<crnet::DownloadBuffer> download_;

	// result_ and resp_ are written on the network thread before
	// completing_ (on_complete is about to run) and done_ (it returned)
	// are signalled, or for the response headers before headers_, and
	// only read by other threads after that.
	crnet::OneShotEvent headers_;
	crnet::OneShotEvent completing_;
	crnet::OneShotEvent done_;
	int result_ = CRNET_E_STATE;

	// Written on the network thread, read anywhere; each value stands on
	// its own, so relaxed ordering is enough.
	std::array<std::atomic<int64_t>, CRNET_PROGRESS_COUNT> progress_{};
};

// A network context bound to its own IO thread.
struct crnet_client {
public:
	static crnet_client *Create(const crnet::ClientConfig &cfg);
	void Destroy();

	// Any thread. Returns false if the client is shutting down.
	bool Start(scoped_refptr<crnet_transfer> t);
	bool StartWebSocket(scoped_refptr<crnet_ws> ws);

	bool IsNetworkThread() const { return runner_->BelongsToCurrentThread(); }
	const scoped_refptr<base::SingleThreadTaskRunner> &runner() const
	{
		return runner_;
	}
	const crnet::ClientConfig &config() const { return cfg_; }

	// Network thread only.
	net::URLRequestContext *context() const { return context_.get(); }
	void TransferFinished(crnet_transfer *t);
	void WebSocketFinished(crnet_ws *ws);

	// Blocking helpers (not on a network thread).
	int SetCookie(const std::string &url, const std::string &line);
	std::optional<std::string> GetCookies(const std::string &url);
	int ClearCookies();
	void CloseIdle();

private:
	explicit crnet_client(const crnet::ClientConfig &cfg);
	~crnet_client();

	bool InitOnNetworkThread();
	void ShutdownOnNetworkThread(base::WaitableEvent *ev);

	crnet::ClientConfig cfg_;
	base::Thread thread_;
	scoped_refptr<base::SingleThreadTaskRunner> runner_;
	std::atomic<bool> shutting_down_{false};

	// Network thread only.
	std::unique_ptr<net::URLRequestContext> context_;
	std::unique_ptr<net::FileNetLogObserver> netlog_;
	scoped_refptr<net::CertNetFetcherURLRequest> cert_fetcher_;
	std::set<scoped_refptr<crnet_transfer>> running_;
	std::set<scoped_refptr<crnet_ws>> sockets_;
};

// Completion queue (queue.cc).
struct crnet_queue {
public:
	crnet_queue();
	~crnet_queue();

	bool ok() const { return efd_ >= 0; }
	int fd() const { return efd_; }
	void Started(crnet_transfer *t);
	void Completed(crnet_transfer *t);
	int Pop(crnet_transfer **t, void **user_data, uint64_t timeout_ms);
	size_t pending() const;
	void Drain();

private:
	int efd_ = -1;
	mutable std::mutex mu_;
	std::condition_variable cv_;
	std::set<scoped_refptr<crnet_transfer>> running_;
	std::deque<scoped_refptr<crnet_transfer>> done_;
};

#endif // CRNET_SRC_INTERNAL_H
