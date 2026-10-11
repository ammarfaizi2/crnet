// SPDX-License-Identifier: BSD-3-Clause
//
// crnet_ws: one WebSocket connection driven through
// net::WebSocketChannel on the client's network thread, the way Chrome's
// network service drives it (services/network/websocket.cc).
//
//   StartOnNetworkThread -> SendAddChannelRequest -> OnAddChannelResponse
//     -> ReadFrames <-> OnDataFrame ...
//     -> OnDropChannel / OnFailChannel / Abort -> Finish
//
// Every exit path ends in Finish(), which runs on_close exactly once.
//
#include "crnet/websocket.h"

#include <stdlib.h>
#include <string.h>

#include <chrono>

#include "base/compiler_specific.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "net/base/auth.h"
#include "net/base/host_port_pair.h"
#include "net/base/isolation_info.h"
#include "net/http/http_network_session.h"
#include "net/http/http_transaction_factory.h"
#include "net/socket/ssl_client_socket.h"
#include "net/base/transport_info.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_response_headers.h"
#include "net/socket/next_proto.h"
#include "net/ssl/ssl_info.h"
#include "net/storage_access_api/status.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/websockets/websocket_channel.h"
#include "net/websockets/websocket_errors.h"
#include "net/websockets/websocket_event_interface.h"
#include "net/websockets/websocket_handshake_request_info.h"
#include "net/websockets/websocket_handshake_response_info.h"
#include "net/websockets/websocket_stream.h"

namespace {

// Received bytes queued for crnet_ws_recv() before reading from the
// network pauses.
constexpr size_t kRecvQueueLimit = 4 * 1024 * 1024;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
	net::DefineNetworkTrafficAnnotation("crnet_websocket", R"(
		semantics {
			sender: "crnet"
			description: "WebSocket opened by a crnet user."
			trigger: "crnet_ws_connect()."
			data: "Whatever the embedding application sends."
			destination: OTHER
		}
		policy {
			cookies_allowed: YES
			setting: "Not applicable."
			policy_exception_justification: "Library."
		})");


// Handshake requests keep //net's (Chrome's) order; only removals and
// the Cookie merge apply.
class WsHeaderHook : public crnet::HeaderHook {
public:
	WsHeaderHook(std::set<std::string> suppressed,
		     std::optional<std::string> user_cookie)
		: suppressed_(std::move(suppressed)),
		  user_cookie_(std::move(user_cookie))
	{
	}

	net::HttpRequestHeaders Finalize(
		const net::URLRequest &req,
		const net::HttpRequestHeaders &in) override
	{
		return crnet::FilterHeaders(in, suppressed_, user_cookie_);
	}

private:
	std::set<std::string> suppressed_;
	std::optional<std::string> user_cookie_;
};

} // namespace

// Owned by the WebSocketChannel; forwards to the crnet_ws, which always
// outlives the channel (the channel is deleted by a task holding a
// reference to the crnet_ws).
class WsEventAdapter : public net::WebSocketEventInterface {
public:
	explicit WsEventAdapter(crnet_ws *ws) : ws_(ws) {}

	void OnCreateURLRequest(net::URLRequest *request) override
	{
		if (ws_->spec_.proxy)
			request->set_proxy_chain_override(
				crnet::ProxyChainOf(ws_->spec_.proxy->proxy));
		if (ws_->partition_)
			request->set_connection_partition(ws_->partition_);
		else if (ws_->identity_)
			request->set_connection_partition(ws_->identity_);
		crnet::HeaderHook::Attach(
			request, std::make_unique<WsHeaderHook>(
					 ws_->headers_.suppressed,
					 ws_->headers_.user_cookie));
	}

	int OnURLRequestConnected(net::URLRequest *request,
				  const net::TransportInfo &info,
				  net::CompletionOnceCallback callback) override
	{
		ws_->OnConnected(info);
		return net::OK;
	}

	void OnAddChannelResponse(
		std::unique_ptr<net::WebSocketHandshakeResponseInfo> response,
		const std::string &selected_subprotocol,
		const std::string &extensions) override
	{
		ws_->OnAddChannelResponse(std::move(response),
					  selected_subprotocol, extensions);
	}

	void OnPong(base::span<const char> payload) override
	{
		ws_->OnPong(payload);
	}

	void OnDataFrame(bool fin, WebSocketMessageType type,
			 base::span<const char> payload) override
	{
		ws_->OnDataFrame(fin, type, payload);
	}

	bool HasPendingDataFrames() override
	{
		return ws_->HasPendingDataFrames();
	}

	void OnSendDataFrameDone() override { ws_->OnSendDataFrameDone(); }

	void OnClosingHandshake() override { ws_->OnClosingHandshake(); }

	void OnDropChannel(bool was_clean, uint16_t code,
			   const std::string &reason) override
	{
		ws_->OnDropChannel(was_clean, code, reason);
	}

	void OnFailChannel(const std::string &message, int net_error,
			   std::optional<int> response_code) override
	{
		ws_->OnFailChannel(message, net_error, response_code);
	}

	void OnStartOpeningHandshake(
		std::unique_ptr<net::WebSocketHandshakeRequestInfo> request) override
	{
	}

	void OnSSLCertificateError(
		std::unique_ptr<SSLErrorCallbacks> callbacks, const GURL &url,
		int net_error, const net::SSLInfo &ssl_info,
		bool fatal) override
	{
		if (ws_->client_->config().insecure)
			callbacks->ContinueSSLRequest();
		else
			callbacks->CancelSSLRequest(net_error, &ssl_info);
	}

	int OnAuthRequired(
		const net::AuthChallengeInfo &auth_info,
		scoped_refptr<net::HttpResponseHeaders> response_headers,
		const net::IPEndPoint &socket_address,
		base::OnceCallback<void(const net::AuthCredentials *)> callback,
		std::optional<net::AuthCredentials> *credentials) override
	{
		*credentials = std::nullopt;
		// See crnet_transfer::OnAuthRequired() for identities.
		if (!auth_info.is_proxy || ws_->identity_ ||
		    ws_->proxy_auth_tried_ || ws_->proxy_user_.empty())
			return net::OK;	// deliver the 401/407 as a failure

		// WebSocketStream ignores synchronously returned credentials;
		// they must come through |callback|, which is bound to the
		// handshake's delegate. That delegate lives as long as the
		// channel, so only run it while the socket is unfinished.
		ws_->proxy_auth_tried_ = true;
		net::AuthCredentials creds(base::UTF8ToUTF16(ws_->proxy_user_),
					   base::UTF8ToUTF16(ws_->proxy_pass_));
		ws_->runner_->PostTask(
			FROM_HERE,
			base::BindOnce(
				[](scoped_refptr<crnet_ws> ws,
				   base::OnceCallback<void(const net::AuthCredentials *)> cb,
				   net::AuthCredentials creds) {
					if (!ws->finished_ && ws->channel_)
						std::move(cb).Run(&creds);
				},
				base::WrapRefCounted(ws_), std::move(callback),
				std::move(creds)));
		return net::ERR_IO_PENDING;
	}

private:
	crnet_ws *ws_;
};

crnet_ws::crnet_ws(crnet_client *client, crnet::RequestSpec spec, GURL url,
		   crnet::WsHeaders headers, const crnet_ws_callbacks *cb,
		   void *user_data)
	: client_(client),
	  runner_(client->runner()),
	  spec_(std::move(spec)),
	  url_(std::move(url)),
	  headers_(std::move(headers)),
	  user_data_(user_data)
{
	if (cb)
		cb_ = *cb;
}

crnet_ws::~crnet_ws() = default;


void crnet_ws::UpdateFdLocked()
{
	readable_.Set(!queue_.empty() || done());
}

// ------------------------------------------------------------------
// Any thread
// ------------------------------------------------------------------

int crnet_ws::Send(crnet_ws_msg_type type, const void *data, size_t len)
{
	if ((type != CRNET_WS_TEXT && type != CRNET_WS_BINARY) ||
	    (!data && len))
		return CRNET_E_INVAL;
	if (type == CRNET_WS_TEXT &&
	    !base::IsStringUTF8(std::string_view(
		    static_cast<const char *>(data), len)))
		return CRNET_E_INVAL;
	if (state() != CRNET_WS_OPEN)
		return CRNET_E_STATE;

	auto buf = base::MakeRefCounted<net::IOBufferWithSize>(
		std::max<size_t>(len, 1));
	// Raw pointer and length come from the C caller.
	if (len)
		UNSAFE_BUFFERS(memcpy(buf->data(), data, len));
	buffered_ += len;
	runner_->PostTask(FROM_HERE,
			  base::BindOnce(&crnet_ws::SendOnNetworkThread,
					 base::WrapRefCounted(this), type,
					 std::move(buf), len));
	return CRNET_OK;
}

int crnet_ws::Close(int code, const std::string &reason)
{
	if (code != net::kWebSocketNormalClosure &&
	    (code < 3000 || code > 4999))
		return CRNET_E_INVAL;
	if (reason.size() > 123 || !base::IsStringUTF8(reason))
		return CRNET_E_INVAL;

	if (state_.load() == CRNET_WS_CONNECTING) {
		// Like WebSocket.close() before onopen: fail the connection.
		Abort(CRNET_E_CANCELED);
		return CRNET_OK;
	}
	int expected = CRNET_WS_OPEN;
	if (!state_.compare_exchange_strong(expected, CRNET_WS_CLOSING))
		return CRNET_E_STATE;
	runner_->PostTask(FROM_HERE,
			  base::BindOnce(&crnet_ws::CloseOnNetworkThread,
					 base::WrapRefCounted(this), code,
					 reason));
	return CRNET_OK;
}

void crnet_ws::Abort(int reason)
{
	if (done())
		return;
	if (runner_->BelongsToCurrentThread()) {
		FailOnNetworkThread(reason, net::ERR_ABORTED);
		return;
	}
	runner_->PostTask(FROM_HERE,
			  base::BindOnce(&crnet_ws::FailOnNetworkThread,
					 base::WrapRefCounted(this), reason,
					 net::ERR_ABORTED));
}

void crnet_ws::Pause()
{
	// Not receiving while paused is the application's choice, not the
	// peer's silence: the idle timer waits for the resume.
	if (runner_->BelongsToCurrentThread()) {
		paused_ = true;
		idle_timer_.Stop();
		return;
	}
	runner_->PostTask(FROM_HERE, base::BindOnce(
					     [](scoped_refptr<crnet_ws> ws) {
						     ws->paused_ = true;
						     ws->idle_timer_.Stop();
					     },
					     base::WrapRefCounted(this)));
}

void crnet_ws::SetIdleTimeout(uint64_t ms)
{
	runner_->PostTask(FROM_HERE,
			  base::BindOnce(
				  [](scoped_refptr<crnet_ws> ws, uint64_t ms) {
					  ws->idle_ms_ = ms;
					  ws->RestartIdleTimer();
				  },
				  base::WrapRefCounted(this), ms));
}

int crnet_ws::Ping(const void *data, size_t len)
{
	if (len > 125 || (len && !data))
		return CRNET_E_INVAL;
	if (state() != CRNET_WS_OPEN)
		return CRNET_E_STATE;
	std::string payload;
	if (len)
		UNSAFE_BUFFERS(payload.assign(static_cast<const char *>(data), len));
	runner_->PostTask(FROM_HERE,
			  base::BindOnce(&crnet_ws::SendPingOnNetworkThread,
					 base::WrapRefCounted(this),
					 std::move(payload)));
	return CRNET_OK;
}

void crnet_ws::SendPingOnNetworkThread(std::string payload)
{
	if (finished_ || !channel_ || state() != CRNET_WS_OPEN)
		return;
	// A CHANNEL_DELETED result means Finish() already ran.
	std::ignore = channel_->SendPing(base::as_byte_span(payload));
}

void crnet_ws::SetPongCallback(crnet_ws_pong_cb cb)
{
	runner_->PostTask(FROM_HERE,
			  base::BindOnce(
				  [](scoped_refptr<crnet_ws> ws,
				     crnet_ws_pong_cb cb) { ws->pong_cb_ = cb; },
				  base::WrapRefCounted(this), cb));
}

void crnet_ws::SetAutoPing(uint64_t interval_ms)
{
	runner_->PostTask(FROM_HERE,
			  base::BindOnce(
				  [](scoped_refptr<crnet_ws> ws, uint64_t ms) {
					  ws->auto_ping_ms_ = ms;
					  ws->RestartAutoPing();
				  },
				  base::WrapRefCounted(this), interval_ms));
}

void crnet_ws::RestartAutoPing()
{
	if (finished_ || state() != CRNET_WS_OPEN || !auto_ping_ms_) {
		auto_ping_timer_.Stop();
		return;
	}
	auto_ping_timer_.Start(
		FROM_HERE,
		base::Milliseconds(std::min<uint64_t>(
			auto_ping_ms_, std::numeric_limits<int64_t>::max())),
		base::BindRepeating(&crnet_ws::SendPingOnNetworkThread,
				    base::Unretained(this), std::string()));
}

void crnet_ws::OnPong(base::span<const char> payload)
{
	if (finished_)
		return;
	// The peer answered: it is alive, whatever the application reads.
	if (idle_timer_.IsRunning())
		idle_timer_.Reset();
	if (pong_cb_)
		pong_cb_(this, payload.data(), payload.size(), user_data_);
}

void crnet_ws::RestartIdleTimer()
{
	if (finished_ || state() != CRNET_WS_OPEN || !idle_ms_ || paused_ ||
	    reading_stopped_) {
		idle_timer_.Stop();
		return;
	}
	idle_timer_.Start(
		FROM_HERE,
		base::Milliseconds(std::min<uint64_t>(
			idle_ms_, std::numeric_limits<int64_t>::max())),
		base::BindOnce(
			[](crnet_ws *ws) {
				// The peer (or the path to it) is gone: drop
				// the connection, no closing handshake.
				ws->Finish(CRNET_E_TIMEOUT,
					   net::kWebSocketErrorAbnormalClosure,
					   "idle timeout", net::ERR_TIMED_OUT);
			},
			base::Unretained(this)));
}

void crnet_ws::Resume()
{
	runner_->PostTask(FROM_HERE,
			  base::BindOnce(&crnet_ws::ResumeOnNetworkThread,
					 base::WrapRefCounted(this)));
}

namespace {

template <typename Pred>
bool WaitFor(std::condition_variable &cv, std::unique_lock<std::mutex> &lk,
	     uint64_t timeout_ms, Pred pred)
{
	if (timeout_ms == UINT64_MAX) {
		cv.wait(lk, pred);
		return true;
	}
	if (timeout_ms == 0)
		return pred();
	uint64_t ms = std::min<uint64_t>(timeout_ms, 1ull << 40);
	return cv.wait_for(lk, std::chrono::milliseconds(ms), pred);
}

} // namespace

int crnet_ws::WaitOpen(uint64_t timeout_ms)
{
	if (!settled_.Wait(timeout_ms))
		return CRNET_E_TIMEOUT;
	return opened_ ? CRNET_OK : result_;
}

int crnet_ws::WaitClosed(uint64_t timeout_ms)
{
	if (!closed_.Wait(timeout_ms))
		return CRNET_E_TIMEOUT;
	return result_;
}

int crnet_ws::Recv(crnet_ws_msg_type *type, void **data, size_t *len,
		   uint64_t timeout_ms)
{
	std::unique_lock<std::mutex> lk(mu_);
	if (!WaitFor(cv_, lk, timeout_ms,
		     [this] { return !queue_.empty() || done(); }))
		return CRNET_E_TIMEOUT;
	if (queue_.empty())
		return CRNET_E_STATE;

	Message m = std::move(queue_.front());
	queue_.pop_front();
	size_t before = queued_bytes_.fetch_sub(m.data.size(),
						std::memory_order_relaxed);
	bool resume = before > kRecvQueueLimit &&
		      before - m.data.size() <= kRecvQueueLimit;
	UpdateFdLocked();
	lk.unlock();

	if (resume)
		Resume();

	char *out = static_cast<char *>(malloc(m.data.size() + 1));
	if (!out)
		return CRNET_E_NOMEM;
	UNSAFE_BUFFERS(memcpy(out, m.data.data(), m.data.size());
		       out[m.data.size()] = '\0');
	if (type)
		*type = m.type;
	if (len)
		*len = m.data.size();
	*data = out;
	return CRNET_OK;
}

// ------------------------------------------------------------------
// Network thread
// ------------------------------------------------------------------

void crnet_ws::StartOnNetworkThread()
{
	DCHECK(runner_->BelongsToCurrentThread());
	if (finished_)
		return;

	proxy_user_ = client_->config().proxy_user;
	proxy_pass_ = client_->config().proxy_pass;
	if (spec_.proxy) {
		proxy_user_ = spec_.proxy->user;
		proxy_pass_ = spec_.proxy->pass;
		identity_ = client_->ResolveProxyIdentity(
			spec_.proxy->proxy, &proxy_user_, &proxy_pass_);
	}

	if (spec_.fresh_connection)
		partition_ = client_->NewConnectionPartition(identity_);

	channel_ = std::make_unique<net::WebSocketChannel>(
		std::make_unique<WsEventAdapter>(this), client_->context());

	uint64_t timeout = spec_.timeout_ms.value_or(client_->config().timeout_ms);
	if (timeout) {
		timer_.Start(FROM_HERE,
			     base::Milliseconds(std::min<uint64_t>(
				     timeout,
				     std::numeric_limits<int64_t>::max())),
			     base::BindOnce(&crnet_ws::OnTimeout,
					    base::Unretained(this)));
	}

	// The WebSocket handshake never asks for a client certificate (in
	// Chrome it may not show UI); it uses the one chosen for the server
	// before. Choose the configured one up front.
	const crnet::ClientConfig &cfg = client_->config();
	if (cfg.client_cert) {
		client_->context()
			->http_transaction_factory()
			->GetSession()
			->ssl_client_context()
			->SetClientCertificate(net::HostPortPair::FromURL(url_),
					       cfg.client_cert, cfg.client_key);
	}

	// What a page of |origin| gets: a first-party, non-frame request.
	const url::Origin &origin = headers_.origin;
	net::IsolationInfo isolation = net::IsolationInfo::Create(
		net::IsolationInfo::RequestType::kOther, origin, origin,
		net::SiteForCookies::FromOrigin(origin));
	channel_->SendAddChannelRequest(
		url_, headers_.protocols, origin,
		net::StorageAccessApiStatus::kNone, isolation, headers_.extra,
		net::WebSocketPriorityHint::kDefault, kTrafficAnnotation,
		net::handles::kInvalidNetworkHandle);
}

void crnet_ws::FailOnNetworkThread(int err, int net_error)
{
	if (finished_)
		return;
	Finish(err, net::kWebSocketErrorAbnormalClosure, crnet_strerror(err),
	       net_error);
}

void crnet_ws::OnTimeout()
{
	if (state() == CRNET_WS_CONNECTING)
		Finish(CRNET_E_TIMEOUT, net::kWebSocketErrorAbnormalClosure,
		       "opening handshake timed out", net::ERR_TIMED_OUT);
}

void crnet_ws::OnConnected(const net::TransportInfo &info)
{
	negotiated_protocol_ = net::NextProtoToString(info.negotiated_protocol);
	remote_ = info.endpoint.ToString();
}

void crnet_ws::OnAddChannelResponse(
	std::unique_ptr<net::WebSocketHandshakeResponseInfo> response,
	const std::string &protocol, const std::string &extensions)
{
	if (finished_)
		return;
	timer_.Stop();

	auto h = std::make_unique<crnet_response>();
	h->url = response->url.spec();
	h->remote = response->remote_endpoint.ToString();
	if (h->remote.empty())
		h->remote = remote_;
	h->protocol = negotiated_protocol_;
	if (const net::HttpResponseHeaders *hdr = response->headers.get()) {
		h->status = hdr->response_code();
		h->status_text = hdr->GetStatusText();
		size_t iter = 0;
		std::string name, value;
		while (hdr->EnumerateHeaderLines(&iter, &name, &value))
			h->headers.emplace_back(name, value);
	}
	protocol_ = protocol;
	extensions_ = extensions;
	handshake_ = std::move(h);
	opened_ = true;
	state_.store(CRNET_WS_OPEN);
	RestartIdleTimer();
	RestartAutoPing();

	if (cb_.on_open)
		cb_.on_open(this, handshake_.get(), protocol_.c_str(),
			    extensions_.c_str(), user_data_);
	// Only now: a thread woken by crnet_ws_wait_open() may rely on what
	// on_open did (as crnet_transfer_wait() relies on on_complete).
	settled_.Signal();
	if (finished_)
		return;
	// Start receiving outside the channel's own callback.
	runner_->PostTask(FROM_HERE, base::BindOnce(&crnet_ws::ReadMore,
						    base::WrapRefCounted(this)));
}

void crnet_ws::ReadMore()
{
	if (finished_ || !channel_)
		return;
	reading_stopped_ = false;
	RestartIdleTimer();
	// A CHANNEL_DELETED result means Finish() already ran.
	std::ignore = channel_->ReadFrames();
}

void crnet_ws::OnDataFrame(bool fin, net::WebSocketFrameHeader::OpCode type,
			   base::span<const char> payload)
{
	if (finished_)
		return;
	if (idle_timer_.IsRunning())
		idle_timer_.Reset();
	if (type != net::WebSocketFrameHeader::kOpCodeContinuation) {
		assembling_type_ = type == net::WebSocketFrameHeader::kOpCodeText
					   ? CRNET_WS_TEXT
					   : CRNET_WS_BINARY;
		assembling_.clear();
	}
	if (spec_.max_body &&
	    assembling_.size() + payload.size() > spec_.max_body) {
		Finish(CRNET_E_TOO_LARGE, net::kWebSocketErrorMessageTooBig,
		       "message too big", net::ERR_MSG_TOO_BIG);
		return;
	}
	if (fin && assembling_.empty()) {
		// Common case: a single-frame message, no copy.
		Deliver(assembling_type_, payload.data(), payload.size());
		return;
	}
	assembling_.append(payload.data(), payload.size());
	if (fin) {
		std::string msg = std::move(assembling_);
		assembling_.clear();
		Deliver(assembling_type_, msg.data(), msg.size());
	}
}

void crnet_ws::Deliver(crnet_ws_msg_type type, const char *data, size_t len)
{
	// A read may already have been in flight when pause took effect;
	// hold what it produces until resume, like Chrome's network service.
	if (cb_.on_message && (paused_ || !held_.empty())) {
		held_.push_back({type, std::string(data, len)});
		return;
	}
	if (cb_.on_message) {
		cb_.on_message(this, type, data, len, user_data_);
		return;
	}
	{
		std::lock_guard<std::mutex> lk(mu_);
		queue_.push_back({type, std::string(data, len)});
		queued_bytes_.fetch_add(len, std::memory_order_relaxed);
		UpdateFdLocked();
	}
	cv_.notify_all();
}

bool crnet_ws::HasPendingDataFrames()
{
	if (finished_)
		return true;
	bool stop = paused_ || !held_.empty() ||
		    (!cb_.on_message &&
		     queued_bytes_.load(std::memory_order_relaxed) > kRecvQueueLimit);
	if (stop) {
		// Reading stops because the application is not taking
		// messages (paused, or a full receive queue); the idle timer
		// restarts with reading.
		reading_stopped_ = true;
		idle_timer_.Stop();
	}
	return stop;
}

void crnet_ws::ResumeOnNetworkThread()
{
	paused_ = false;
	RestartIdleTimer();
	while (!finished_ && !paused_ && !held_.empty()) {
		Message m = std::move(held_.front());
		held_.pop_front();
		Deliver(m.type, m.data.data(), m.data.size());
	}
	if (finished_ || paused_ || !reading_stopped_)
		return;
	if (!cb_.on_message &&
	    queued_bytes_.load(std::memory_order_relaxed) > kRecvQueueLimit)
		return;
	ReadMore();
}

void crnet_ws::SendOnNetworkThread(crnet_ws_msg_type type,
				   scoped_refptr<net::IOBuffer> buf, size_t len)
{
	if (finished_ || !channel_ || state() != CRNET_WS_OPEN) {
		buffered_ -= len;
		return;
	}
	in_flight_.push_back(len);
	std::ignore = channel_->SendFrame(
		true,
		type == CRNET_WS_TEXT ? net::WebSocketFrameHeader::kOpCodeText
				      : net::WebSocketFrameHeader::kOpCodeBinary,
		std::move(buf), len);
}

void crnet_ws::OnSendDataFrameDone()
{
	if (in_flight_.empty())
		return;
	buffered_ -= in_flight_.front();
	in_flight_.pop_front();
}

void crnet_ws::CloseOnNetworkThread(int code, std::string reason)
{
	if (finished_ || !channel_)
		return;
	std::ignore = channel_->StartClosingHandshake(code, reason);
}

void crnet_ws::OnClosingHandshake()
{
	// The server started closing; the channel answers it itself.
	state_.store(CRNET_WS_CLOSING);
}

void crnet_ws::OnDropChannel(bool was_clean, uint16_t code,
			     const std::string &reason)
{
	Finish(was_clean ? CRNET_OK : CRNET_E_NET, code, reason,
	       was_clean ? net::OK : net::ERR_CONNECTION_CLOSED);
}

void crnet_ws::OnFailChannel(const std::string &message, int net_error,
			     std::optional<int> response_code)
{
	if (response_code && !handshake_) {
		handshake_ = std::make_unique<crnet_response>();
		handshake_->status = *response_code;
		handshake_->url = url_.spec();
		handshake_->protocol = negotiated_protocol_;
		handshake_->remote = remote_;
	}
	// WebSocketChannel reports OK or the generic ERR_FAILED both for a
	// handshake the server refused (with its status code) and for
	// protocol violations (bad frames, invalid UTF-8, ...). Name them.
	if (net_error == net::OK || net_error == net::ERR_FAILED)
		net_error = response_code ? net::ERR_INVALID_RESPONSE
					  : net::ERR_WS_PROTOCOL_ERROR;
	Finish(CRNET_E_NET, net::kWebSocketErrorAbnormalClosure, message,
	       net_error);
}

void crnet_ws::Finish(int err, int code, const std::string &reason,
		      int net_error)
{
	DCHECK(runner_->BelongsToCurrentThread());
	if (finished_)
		return;
	finished_ = true;
	timer_.Stop();
	idle_timer_.Stop();
	auto_ping_timer_.Stop();

	if (channel_) {
		// We may be inside one of the channel's callbacks; delete it on
		// a later task, keeping its event interface's target (us)
		// alive until then.
		runner_->PostTask(
			FROM_HERE,
			base::BindOnce(
				[](std::unique_ptr<net::WebSocketChannel> ch,
				   scoped_refptr<crnet_ws> self) {
					ch.reset();
					if (self->partition_)
						self->client_->CloseConnectionPartition(
							*self->partition_);
				},
				std::move(channel_), base::WrapRefCounted(this)));
	}
	in_flight_.clear();
	buffered_.store(0);

	scoped_refptr<crnet_ws> self(this);
	close_code_ = code;
	close_reason_ = reason;
	net_error_ = err == CRNET_OK ? 0 : net_error;
	result_ = err;
	state_.store(CRNET_WS_CLOSED);
	if (cb_.on_close)
		cb_.on_close(this, err, code, close_reason_.c_str(), user_data_);
	settled_.Signal();
	closed_.Signal();
	{
		// Wake Recv() callers and make the fd readable. Taking the lock
		// after signalling means a Recv() that saw "not done" is
		// already waiting on cv_ when we notify.
		std::lock_guard<std::mutex> lk(mu_);
		UpdateFdLocked();
	}
	cv_.notify_all();
	client_->WebSocketFinished(this);
}
