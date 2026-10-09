// SPDX-License-Identifier: BSD-3-Clause
//
// crhttp_transfer: one request driven through net::URLRequest as an
// asynchronous state machine on the client's network thread.
//
//   StartOnNetworkThread -> [OnReceivedRedirect]* -> OnResponseStarted
//     -> ReadMore <-> OnReadCompleted -> Finish
//
// Every exit path ends in Finish(), which runs on_complete exactly once,
// publishes the result to waiting threads and drops the running
// reference held by the client.
//
#include <limits>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/platform_thread.h"
#include "crhttp/internal.h"
#include "net/base/auth.h"
#include "net/base/elements_upload_data_stream.h"
#include "net/base/load_flags.h"
#include "net/base/upload_bytes_element_reader.h"
#include "net/cert/x509_certificate.h"
#include "net/ssl/ssl_private_key.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_connection_info.h"
#include "net/http/http_response_headers.h"
#include "net/ssl/ssl_cert_request_info.h"
#include "net/ssl/ssl_info.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/redirect_info.h"
#include "net/url_request/referrer_policy.h"

namespace {

constexpr int kReadSize = 64 * 1024;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
	net::DefineNetworkTrafficAnnotation("crhttp", R"(
		semantics {
			sender: "crhttp"
			description: "HTTP request issued by a crhttp user."
			trigger: "crhttp_perform() or crhttp_start()."
			data: "Whatever the embedding application sends."
			destination: OTHER
		}
		policy {
			cookies_allowed: YES
			setting: "Not applicable."
			policy_exception_justification: "Library."
		})");

// Ties a URLRequest back to its transfer for the NetworkDelegate.
const void *const kTransferKey = &kTransferKey;
struct TransferLink : public base::SupportsUserData::Data {
	explicit TransferLink(crhttp_transfer *t) : t(t) {}
	crhttp_transfer *t;
};

// Marks the current thread as running a callback of |t| for the
// duration of the scope (re-entrancy detection in crhttp_transfer_free).
class CallbackScope {
public:
	explicit CallbackScope(std::atomic<base::PlatformThreadId> *slot)
		: slot_(slot),
		  prev_(slot->exchange(base::PlatformThread::CurrentId()))
	{
	}
	~CallbackScope() { slot_->store(prev_); }

private:
	std::atomic<base::PlatformThreadId> *slot_;
	base::PlatformThreadId prev_;
};

} // namespace

namespace crhttp {

crhttp_transfer *TransferForRequest(const net::URLRequest *req)
{
	auto *link =
		static_cast<TransferLink *>(req->GetUserData(kTransferKey));
	return link ? link->t : nullptr;
}

} // namespace crhttp

crhttp_transfer::crhttp_transfer(crhttp_client *client,
				 crhttp::RequestSpec spec,
				 const crhttp_callbacks *cb, void *user_data,
				 crhttp_queue *queue)
	: client_(client),
	  runner_(client->runner()),
	  spec_(std::move(spec)),
	  user_data_(user_data),
	  queue_(queue)
{
	if (cb)
		cb_ = *cb;
}

crhttp_transfer::~crhttp_transfer() = default;

bool crhttp_transfer::in_callback_on_this_thread() const
{
	return callback_thread_.load() == base::PlatformThread::CurrentId();
}

// ------------------------------------------------------------------
// Any thread
// ------------------------------------------------------------------

void crhttp_transfer::Cancel(int reason)
{
	if (done())
		return;
	if (runner_->BelongsToCurrentThread()) {
		FailOnNetworkThread(reason, net::ERR_ABORTED);
		return;
	}
	runner_->PostTask(FROM_HERE,
			  base::BindOnce(&crhttp_transfer::FailOnNetworkThread,
					 base::WrapRefCounted(this), reason,
					 net::ERR_ABORTED));
}

void crhttp_transfer::Pause()
{
	runner_->PostTask(FROM_HERE, base::BindOnce(
					     [](scoped_refptr<crhttp_transfer> t) {
						     t->paused_ = true;
					     },
					     base::WrapRefCounted(this)));
}

void crhttp_transfer::Resume()
{
	runner_->PostTask(FROM_HERE,
			  base::BindOnce(&crhttp_transfer::ResumeOnNetworkThread,
					 base::WrapRefCounted(this)));
}

int crhttp_transfer::Wait(uint64_t timeout_ms)
{
	std::unique_lock<std::mutex> lk(mu_);
	auto pred = [this] { return done(); };
	if (timeout_ms == UINT64_MAX) {
		cv_.wait(lk, pred);
	} else if (timeout_ms > 0) {
		// Clamp to avoid overflow in the clock arithmetic.
		uint64_t ms = std::min<uint64_t>(timeout_ms, 1ull << 40);
		if (!cv_.wait_for(lk, std::chrono::milliseconds(ms), pred))
			return CRHTTP_E_TIMEOUT;
	} else if (!pred()) {
		return CRHTTP_E_TIMEOUT;
	}
	return result_;
}

crhttp_response *crhttp_transfer::TakeResponse()
{
	if (!done())
		return nullptr;
	return resp_.release();
}

// ------------------------------------------------------------------
// Network thread
// ------------------------------------------------------------------

void crhttp_transfer::StartOnNetworkThread()
{
	DCHECK(runner_->BelongsToCurrentThread());
	if (finished_)
		return;

	const crhttp::ClientConfig &cfg = client_->config();
	bh_ = crhttp::BuildBrowserHeaders(cfg, spec_);

	request_ = client_->context()->CreateRequest(
		spec_.url, bh_.priority, this, kTrafficAnnotation,
		net::handles::kInvalidNetworkHandle);
	request_->SetUserData(kTransferKey,
			      std::make_unique<TransferLink>(this));
	request_->set_method(spec_.method);
	request_->SetExtraRequestHeaders(bh_.extra);
	if (bh_.referrer) {
		request_->set_referrer_policy(net::ReferrerPolicy::NEVER_CLEAR);
		request_->SetReferrer(*bh_.referrer);
	}
	request_->set_initiator(bh_.initiator);
	// Treat every request as first-party, like a top-level navigation or
	// a same-site API call, so SameSite cookies behave as in a browser.
	request_->set_site_for_cookies(
		net::SiteForCookies::FromUrl(spec_.url));
	request_->set_force_main_frame_for_same_site_cookies(
		spec_.mode == CRHTTP_MODE_NAVIGATE);
	request_->SetPriorityIncremental(true);
	if (!spec_.decompress)
		request_->set_client_side_content_decoding_enabled(true);

	int flags = net::LOAD_NORMAL;
	if (!cfg.cookies)
		flags |= net::LOAD_DO_NOT_SAVE_COOKIES;
	request_->SetLoadFlags(flags);

	if (spec_.body) {
		request_->set_upload(net::ElementsUploadDataStream::CreateWithReader(
			net::UploadOwnedBytesElementReader::CreateWithString(
				*spec_.body)));
	}

	uint64_t timeout = spec_.timeout_ms.value_or(cfg.timeout_ms);
	if (timeout) {
		timer_.Start(FROM_HERE,
			     base::Milliseconds(std::min<uint64_t>(
				     timeout,
				     std::numeric_limits<int64_t>::max())),
			     base::BindOnce(&crhttp_transfer::OnTimeout,
					    base::Unretained(this)));
	}

	resp_ = std::make_unique<crhttp_response>();
	resp_->url = spec_.url.spec();
	request_->Start();
}

void crhttp_transfer::FailOnNetworkThread(int err, int net_error)
{
	if (finished_)
		return;
	Finish(err, net_error);
}

void crhttp_transfer::OnTimeout()
{
	FailOnNetworkThread(CRHTTP_E_TIMEOUT, net::ERR_TIMED_OUT);
}

void crhttp_transfer::ResumeOnNetworkThread()
{
	if (!paused_)
		return;
	paused_ = false;
	if (started_reading_ && !read_pending_ && !finished_)
		ReadMore();
}

void crhttp_transfer::FillResponseMeta(net::URLRequest *request)
{
	if (!resp_)
		resp_ = std::make_unique<crhttp_response>();
	crhttp_response &r = *resp_;
	r.url = request->url().spec();
	r.status = request->GetResponseCode();
	r.headers.clear();
	if (const net::HttpResponseHeaders *h = request->response_headers()) {
		r.status_text = h->GetStatusText();
		size_t iter = 0;
		std::string name, value;
		while (h->EnumerateHeaderLines(&iter, &name, &value))
			r.headers.emplace_back(name, value);
	}
	const net::HttpResponseInfo &info = request->response_info();
	r.protocol = info.alpn_negotiated_protocol;
	if (r.protocol.empty() || r.protocol == "unknown")
		r.protocol = std::string(
			net::HttpConnectionInfoToString(info.connection_info));
	r.remote = request->GetResponseRemoteEndpoint().ToString();
	r.via_proxy = info.WasFetchedViaProxy();
}

void crhttp_transfer::OnReceivedRedirect(net::URLRequest *request,
					 const net::RedirectInfo &redirect_info,
					 bool *defer_redirect)
{
	if (resp_->redirects >= spec_.max_redirects) {
		// Hand the 3xx itself to the caller.
		FillResponseMeta(request);
		*defer_redirect = true;
		Finish(CRHTTP_OK, net::OK);
		return;
	}
	resp_->redirects++;
}

void crhttp_transfer::OnAuthRequired(net::URLRequest *request,
				     const net::AuthChallengeInfo &auth_info)
{
	const crhttp::ClientConfig &cfg = client_->config();
	if (auth_info.is_proxy && !proxy_auth_tried_ &&
	    !cfg.proxy_user.empty()) {
		proxy_auth_tried_ = true;
		request->SetAuth(net::AuthCredentials(
			base::UTF8ToUTF16(cfg.proxy_user),
			base::UTF8ToUTF16(cfg.proxy_pass)));
		return;
	}
	// Deliver the 401/407 response as is.
	request->CancelAuth();
}

void crhttp_transfer::OnCertificateRequested(
	net::URLRequest *request, net::SSLCertRequestInfo *cert_request_info)
{
	// No client certificates; continue the handshake without one.
	request->ContinueWithCertificate(nullptr, nullptr);
}

void crhttp_transfer::OnSSLCertificateError(net::URLRequest *request,
					    int net_error,
					    const net::SSLInfo &ssl_info,
					    bool fatal)
{
	if (client_->config().insecure) {
		request->ContinueDespiteLastError();
		return;
	}
	request->CancelWithSSLError(net_error, ssl_info);
}

void crhttp_transfer::OnResponseStarted(net::URLRequest *request,
					int net_error)
{
	if (finished_)
		return;
	if (net_error != net::OK) {
		Finish(CRHTTP_E_NET, net_error);
		return;
	}
	FillResponseMeta(request);

	if (cb_.on_response) {
		int rc;
		{
			CallbackScope scope(&callback_thread_);
			rc = cb_.on_response(this, resp_.get(), user_data_);
		}
		if (finished_)
			return;
		if (rc) {
			Finish(CRHTTP_E_CALLBACK, net::ERR_ABORTED);
			return;
		}
	}

	buf_ = base::MakeRefCounted<net::IOBufferWithSize>(kReadSize);
	started_reading_ = true;
	if (!paused_)
		ReadMore();
}

void crhttp_transfer::ReadMore()
{
	while (!finished_ && !paused_) {
		int n = request_->Read(buf_.get(), buf_->size());
		if (n == net::ERR_IO_PENDING) {
			read_pending_ = true;
			return;
		}
		if (!HandleRead(n))
			return;
	}
}

void crhttp_transfer::OnReadCompleted(net::URLRequest *request,
				      int bytes_read)
{
	read_pending_ = false;
	if (finished_)
		return;
	if (HandleRead(bytes_read))
		ReadMore();
}

bool crhttp_transfer::HandleRead(int n)
{
	if (n < 0) {
		Finish(CRHTTP_E_NET, n);
		return false;
	}
	if (n == 0) {
		Finish(CRHTTP_OK, net::OK);
		return false;
	}

	body_bytes_ += n;
	if (spec_.max_body && body_bytes_ > spec_.max_body) {
		Finish(CRHTTP_E_TOO_LARGE, net::ERR_FILE_TOO_BIG);
		return false;
	}

	if (cb_.on_data) {
		int rc;
		{
			CallbackScope scope(&callback_thread_);
			rc = cb_.on_data(this, buf_->data(), n, user_data_);
		}
		if (finished_)
			return false;
		if (rc) {
			Finish(CRHTTP_E_CALLBACK, net::ERR_ABORTED);
			return false;
		}
	} else {
		resp_->body.append(buf_->data(), n);
	}
	return true;
}

void crhttp_transfer::Finish(int err, int net_error)
{
	DCHECK(runner_->BelongsToCurrentThread());
	DCHECK(!finished_);
	finished_ = true;
	timer_.Stop();

	if (request_) {
		// Cancel() guarantees no further delegate calls (a re-entrant
		// OnReadCompleted is ignored via finished_). We may be inside a
		// URLRequest callback, so delete it on a later task.
		request_->Cancel();
		runner_->DeleteSoon(FROM_HERE, std::move(request_));
	}
	buf_ = nullptr;

	if (!resp_)
		resp_ = std::make_unique<crhttp_response>();
	if (err != CRHTTP_OK) {
		resp_->net_error = net_error;
		resp_->net_error_str = crhttp::NetErrorName(net_error);
	}

	// Keep ourselves alive through the callback and the hand-offs below.
	scoped_refptr<crhttp_transfer> self(this);
	{
		std::lock_guard<std::mutex> lk(mu_);
		result_ = err;
	}
	if (cb_.on_complete) {
		CallbackScope scope(&callback_thread_);
		cb_.on_complete(this, err, resp_.get(), user_data_);
	}
	{
		std::lock_guard<std::mutex> lk(mu_);
		done_.store(true, std::memory_order_release);
	}
	cv_.notify_all();

	if (queue_)
		queue_->Completed(this);
	client_->TransferFinished(this);
}
