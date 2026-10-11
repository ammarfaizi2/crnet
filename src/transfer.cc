// SPDX-License-Identifier: BSD-3-Clause
//
// crnet_transfer: one request driven through net::URLRequest as an
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

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "base/strings/utf_string_conversions.h"
#include "crnet/internal.h"
#include "crnet/stream.h"
#include "net/base/auth.h"
#include "net/base/elements_upload_data_stream.h"
#include "net/base/load_flags.h"
#include "net/base/load_timing_info.h"
#include "net/base/upload_bytes_element_reader.h"
#include "net/base/upload_file_element_reader.h"
#include "net/cert/x509_certificate.h"
#include "net/ssl/ssl_private_key.h"
#include "net/cookies/site_for_cookies.h"
#include "net/filter/filter_source_stream.h"
#include "net/filter/source_stream_type.h"
#include "net/http/http_connection_info.h"
#include "net/http/http_response_headers.h"
#include "net/log/net_log_source.h"
#include "net/ssl/ssl_cert_request_info.h"
#include "net/ssl/ssl_info.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/redirect_info.h"
#include "net/url_request/referrer_policy.h"

namespace {

constexpr int kReadSize = 64 * 1024;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
	net::DefineNetworkTrafficAnnotation("crnet", R"(
		semantics {
			sender: "crnet"
			description: "HTTP request issued by a crnet user."
			trigger: "crnet_perform() or crnet_start()."
			data: "Whatever the embedding application sends."
			destination: OTHER
		}
		policy {
			cookies_allowed: YES
			setting: "Not applicable."
			policy_exception_justification: "Library."
		})");

// Applies the transfer's header policy (crnet::FinalizeHeaders).
class TransferHeaderHook : public crnet::HeaderHook {
public:
	explicit TransferHeaderHook(crnet_transfer *t) : t_(t) {}

	net::HttpRequestHeaders Finalize(
		const net::URLRequest &req,
		const net::HttpRequestHeaders &in) override
	{
		return crnet::FinalizeHeaders(t_->spec(), t_->browser_headers(),
					      req, in);
	}

private:
	// The URLRequest (owner of this hook) never outlives its transfer.
	crnet_transfer *t_;
};


} // namespace

namespace crnet {

namespace {
const void *const kHeaderHookKey = &kHeaderHookKey;
} // namespace

void HeaderHook::Attach(net::URLRequest *req, std::unique_ptr<HeaderHook> hook)
{
	req->SetUserData(kHeaderHookKey, std::move(hook));
}

HeaderHook *HeaderHook::Get(const net::URLRequest *req)
{
	return static_cast<HeaderHook *>(req->GetUserData(kHeaderHookKey));
}

} // namespace crnet

crnet_transfer::crnet_transfer(crnet_client *client,
			       crnet::RequestSpec spec,
				 const crnet_callbacks *cb, void *user_data,
				 crnet_queue *queue)
	: client_(client),
	  runner_(client->runner()),
	  spec_(std::move(spec)),
	  user_data_(user_data),
	  queue_(queue)
{
	if (cb)
		cb_ = *cb;
	// Created up front: the application may write before the transfer
	// reaches the network thread.
	if (spec_.body_stream)
		upload_ = base::MakeRefCounted<crnet::UploadPipe>(*spec_.body_stream,
								  runner_);
	if (spec_.stream_response && !cb_.on_data)
		download_ = std::make_unique<crnet::DownloadBuffer>();
}

crnet_transfer::~crnet_transfer() = default;

bool crnet_transfer::ok() const
{
	return !download_ || download_->ok();
}


// ------------------------------------------------------------------
// Any thread
// ------------------------------------------------------------------

void crnet_transfer::Cancel(int reason)
{
	if (done())
		return;
	if (runner_->BelongsToCurrentThread()) {
		FailOnNetworkThread(reason, net::ERR_ABORTED);
		return;
	}
	runner_->PostTask(FROM_HERE,
			  base::BindOnce(&crnet_transfer::FailOnNetworkThread,
					 base::WrapRefCounted(this), reason,
					 net::ERR_ABORTED));
}

void crnet_transfer::Pause()
{
	// From a callback on the network thread it must take effect before
	// the read that would otherwise follow the callback.
	if (runner_->BelongsToCurrentThread()) {
		paused_ = true;
		return;
	}
	runner_->PostTask(FROM_HERE, base::BindOnce(
					     [](scoped_refptr<crnet_transfer> t) {
						     t->paused_ = true;
					     },
					     base::WrapRefCounted(this)));
}

void crnet_transfer::Resume()
{
	runner_->PostTask(FROM_HERE,
			  base::BindOnce(&crnet_transfer::ResumeOnNetworkThread,
					 base::WrapRefCounted(this)));
}

int crnet_transfer::Wait(uint64_t timeout_ms)
{
	if (!done_.Wait(timeout_ms))
		return CRNET_E_TIMEOUT;
	return result_;
}

crnet_response *crnet_transfer::TakeResponse()
{
	if (!done())
		return nullptr;
	return resp_.release();
}

// ------------------------------------------------------------------
// Network thread
// ------------------------------------------------------------------

void crnet_transfer::StartOnNetworkThread()
{
	DCHECK(runner_->BelongsToCurrentThread());
	if (finished_)
		return;

	const crnet::ClientConfig &cfg = client_->config();
	bh_ = crnet::BuildBrowserHeaders(cfg, spec_);

	request_ = client_->context()->CreateRequest(
		spec_.url, bh_.priority, this, kTrafficAnnotation,
		net::handles::kInvalidNetworkHandle);
	crnet::HeaderHook::Attach(request_.get(),
				  std::make_unique<TransferHeaderHook>(this));
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
		spec_.mode == CRNET_MODE_NAVIGATE);
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
	} else if (spec_.body_file) {
		// Read on the ThreadPool as the upload proceeds.
		file_runner_ = base::ThreadPool::CreateTaskRunner(
			{base::MayBlock(), base::TaskPriority::USER_VISIBLE});
		request_->set_upload(net::ElementsUploadDataStream::CreateWithReader(
			std::make_unique<net::UploadFileElementReader>(
				file_runner_.get(),
				base::FilePath(spec_.body_file->path),
				spec_.body_file->offset, spec_.body_file->length,
				base::Time())));
	} else if (upload_) {
		request_->set_upload(std::make_unique<crnet::PipeUploadDataStream>(
			upload_, *spec_.body_stream));
	}

	uint64_t timeout = spec_.timeout_ms.value_or(cfg.timeout_ms);
	if (timeout) {
		timer_.Start(FROM_HERE,
			     base::Milliseconds(std::min<uint64_t>(
				     timeout,
				     std::numeric_limits<int64_t>::max())),
			     base::BindOnce(&crnet_transfer::OnTimeout,
					    base::Unretained(this)));
	}

	resp_ = std::make_unique<crnet_response>();
	resp_->url = spec_.url.spec();
	request_->Start();
}

void crnet_transfer::FailOnNetworkThread(int err, int net_error)
{
	if (finished_)
		return;
	Finish(err, net_error);
}

void crnet_transfer::OnTimeout()
{
	FailOnNetworkThread(CRNET_E_TIMEOUT, net::ERR_TIMED_OUT);
}

void crnet_transfer::ResumeOnNetworkThread()
{
	if (!paused_)
		return;
	paused_ = false;
	if (started_reading_ && !read_pending_ && !finished_)
		ReadMore();
}

void crnet_transfer::ResumeDownload()
{
	runner_->PostTask(FROM_HERE,
			  base::BindOnce(&crnet_transfer::ResumeDownloadOnNetworkThread,
					 base::WrapRefCounted(this)));
}

void crnet_transfer::ResumeDownloadOnNetworkThread()
{
	if (!download_full_)
		return;
	download_full_ = false;
	if (started_reading_ && !read_pending_ && !finished_ && !paused_)
		ReadMore();
}

int crnet_transfer::WaitResponse(uint64_t timeout_ms)
{
	if (!headers_.Wait(timeout_ms))
		return CRNET_E_TIMEOUT;
	return resp_ && resp_->status > 0 ? CRNET_OK : result_;
}

// The content codings URLRequestHttpJob decodes, read back from it rather
// than re-derived: every coding Chromium recognises in Content-Encoding,
// unless it left them to the client (decompression off), in which case
// it reports them as client-side codings instead. Valid once the
// response has started, when the job has set up its source stream.
std::string crnet_transfer::DecodedCodings(net::URLRequest *request)
{
	std::vector<net::SourceStreamType> undecoded;
	request->GetClientSideContentDecodingTypes(&undecoded);
	const net::HttpResponseHeaders *h = request->response_headers();
	if (!undecoded.empty() || !h)
		return std::string();
	std::string out;
	for (net::SourceStreamType t :
	     net::FilterSourceStream::GetContentEncodingTypes(*h)) {
		const char *name = nullptr;
		switch (t) {
		case net::SourceStreamType::kBrotli:
			name = "br";
			break;
		case net::SourceStreamType::kDeflate:
			name = "deflate";
			break;
		case net::SourceStreamType::kGzip:
			name = "gzip";
			break;
		case net::SourceStreamType::kZstd:
			name = "zstd";
			break;
		default:
			continue;
		}
		if (!out.empty())
			out += ", ";
		out += name;
	}
	return out;
}

void crnet_transfer::FillResponseMeta(net::URLRequest *request)
{
	if (!resp_)
		resp_ = std::make_unique<crnet_response>();
	crnet_response &r = *resp_;
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
	FillTiming(request);
}

void crnet_transfer::FillTiming(net::URLRequest *request)
{
	net::LoadTimingInfo lt;
	request->GetLoadTimingInfo(&lt);
	crnet_response &r = *resp_;
	r.socket_reused = lt.socket_reused;
	r.connection_id = lt.socket_log_id == net::NetLogSource::kInvalidId
				  ? 0
				  : lt.socket_log_id;
	auto set = [&](crnet_timing which, base::TimeTicks t) {
		r.timing[which] =
			t.is_null() ? -1 : (t - created_).InMicroseconds();
	};
	const net::LoadTimingInfo::ConnectTiming &ct = lt.connect_timing;
	set(CRNET_TIMING_REQUEST_START, lt.request_start);
	set(CRNET_TIMING_PROXY_RESOLVE_START, lt.proxy_resolve_start);
	set(CRNET_TIMING_PROXY_RESOLVE_END, lt.proxy_resolve_end);
	set(CRNET_TIMING_DNS_START, ct.domain_lookup_start);
	set(CRNET_TIMING_DNS_END, ct.domain_lookup_end);
	set(CRNET_TIMING_CONNECT_START, ct.connect_start);
	set(CRNET_TIMING_CONNECT_END, ct.connect_end);
	set(CRNET_TIMING_TLS_START, ct.ssl_start);
	set(CRNET_TIMING_TLS_END, ct.ssl_end);
	set(CRNET_TIMING_SEND_START, lt.send_start);
	set(CRNET_TIMING_SEND_END, lt.send_end);
	set(CRNET_TIMING_HEADERS_START, lt.receive_headers_start);
	set(CRNET_TIMING_HEADERS_END, lt.receive_headers_end);
}

void crnet_transfer::OnReceivedRedirect(net::URLRequest *request,
					const net::RedirectInfo &redirect_info,
					 bool *defer_redirect)
{
	if (resp_->redirects >= spec_.max_redirects) {
		// Hand the 3xx itself to the caller.
		FillResponseMeta(request);
		*defer_redirect = true;
		Finish(CRNET_OK, net::OK);
		return;
	}
	resp_->redirects++;
}

void crnet_transfer::OnAuthRequired(net::URLRequest *request,
				    const net::AuthChallengeInfo &auth_info)
{
	const crnet::ClientConfig &cfg = client_->config();
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

void crnet_transfer::OnCertificateRequested(
	net::URLRequest *request, net::SSLCertRequestInfo *cert_request_info)
{
	// No client certificates; continue the handshake without one.
	request->ContinueWithCertificate(nullptr, nullptr);
}

void crnet_transfer::OnSSLCertificateError(net::URLRequest *request,
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

void crnet_transfer::OnResponseStarted(net::URLRequest *request,
				       int net_error)
{
	if (finished_)
		return;
	if (net_error != net::OK) {
		Finish(CRNET_E_NET, net_error);
		return;
	}
	FillResponseMeta(request);
	resp_->decoded_codings = DecodedCodings(request);
	headers_.Signal();

	if (cb_.on_response) {
		int rc = cb_.on_response(this, resp_.get(), user_data_);
		if (finished_)
			return;
		if (rc) {
			Finish(CRNET_E_CALLBACK, net::ERR_ABORTED);
			return;
		}
	}

	buf_ = base::MakeRefCounted<net::IOBufferWithSize>(kReadSize);
	started_reading_ = true;
	if (!paused_)
		ReadMore();
}

void crnet_transfer::ReadMore()
{
	while (!finished_ && !paused_ && !download_full_) {
		int n = request_->Read(buf_.get(), buf_->size());
		if (n == net::ERR_IO_PENDING) {
			read_pending_ = true;
			return;
		}
		if (!HandleRead(n))
			return;
	}
}

void crnet_transfer::OnReadCompleted(net::URLRequest *request,
				     int bytes_read)
{
	read_pending_ = false;
	if (finished_)
		return;
	if (HandleRead(bytes_read))
		ReadMore();
}

bool crnet_transfer::HandleRead(int n)
{
	if (n < 0) {
		Finish(CRNET_E_NET, n);
		return false;
	}
	if (n == 0) {
		Finish(CRNET_OK, net::OK);
		return false;
	}

	body_bytes_ += n;
	if (spec_.max_body && body_bytes_ > spec_.max_body) {
		Finish(CRNET_E_TOO_LARGE, net::ERR_FILE_TOO_BIG);
		return false;
	}

	if (cb_.on_data) {
		int rc = cb_.on_data(this, buf_->data(), n, user_data_);
		if (finished_)
			return false;
		if (rc) {
			Finish(CRNET_E_CALLBACK, net::ERR_ABORTED);
			return false;
		}
	} else if (download_) {
		if (download_->Push(buf_->data(), n))
			download_full_ = true;	// ReadMore() stops until resumed
	} else {
		resp_->body.append(buf_->data(), n);
	}
	return true;
}

void crnet_transfer::Finish(int err, int net_error)
{
	DCHECK(runner_->BelongsToCurrentThread());
	DCHECK(!finished_);
	finished_ = true;
	timer_.Stop();

	if (request_) {
		// We may be inside a URLRequest callback, so the request is
		// deleted on a later task. Cancel() can still have queued a
		// delegate notification (OnResponseStarted(ERR_ABORTED)) ahead
		// of that task, so the request keeps its delegate (us) alive
		// until it is gone; finished_ turns such late calls into
		// no-ops.
		request_->Cancel();
		runner_->PostTask(
			FROM_HERE,
			base::BindOnce(
				[](std::unique_ptr<net::URLRequest> req,
				   scoped_refptr<crnet_transfer> self) {
					req.reset();
				},
				std::move(request_), base::WrapRefCounted(this)));
	}
	buf_ = nullptr;

	if (!resp_)
		resp_ = std::make_unique<crnet_response>();
	resp_->timing[CRNET_TIMING_COMPLETE] =
		(base::TimeTicks::Now() - created_).InMicroseconds();
	if (err != CRNET_OK) {
		resp_->net_error = net_error;
		resp_->net_error_str = crnet::NetErrorName(net_error);
	}

	// Keep ourselves alive through the callback and the hand-offs below.
	scoped_refptr<crnet_transfer> self(this);
	result_ = err;
	if (download_)
		download_->Finish(err);
	if (upload_)
		upload_->Close(err == CRNET_OK ? CRNET_E_STATE : err);
	headers_.Signal();
	completing_.Signal();
	if (cb_.on_complete)
		cb_.on_complete(this, err, resp_.get(), user_data_);
	done_.Signal();

	if (queue_)
		queue_->Completed(this);
	client_->TransferFinished(this);
}
