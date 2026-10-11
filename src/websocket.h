// SPDX-License-Identifier: BSD-3-Clause
//
// crnet_ws: a WebSocket connection driven through net::WebSocketChannel
// on the client's network thread.
//
#ifndef CRNET_SRC_WEBSOCKET_H
#define CRNET_SRC_WEBSOCKET_H

#include <deque>

#include "crnet/fd_flag.h"
#include "crnet/internal.h"
#include "net/websockets/websocket_frame.h"

namespace net {
class WebSocketChannel;
struct TransportInfo;
struct WebSocketHandshakeResponseInfo;
}

struct crnet_ws : public base::RefCountedThreadSafe<crnet_ws> {
public:
	crnet_ws(crnet_client *client, crnet::RequestSpec spec, GURL url,
		 crnet::WsHeaders headers, const crnet_ws_callbacks *cb,
		 void *user_data);

	crnet_ws(const crnet_ws &) = delete;
	crnet_ws &operator=(const crnet_ws &) = delete;

	bool ok() const { return readable_.fd() >= 0; }

	// Any thread.
	int Send(crnet_ws_msg_type type, const void *data, size_t len);
	int Close(int code, const std::string &reason);
	void Abort(int reason);
	void Pause();
	void Resume();
	void SetIdleTimeout(uint64_t ms);
	int Ping(const void *data, size_t len);
	void SetPongCallback(crnet_ws_pong_cb cb);
	void SetAutoPing(uint64_t interval_ms);
	int WaitOpen(uint64_t timeout_ms);
	int WaitClosed(uint64_t timeout_ms);
	int Recv(crnet_ws_msg_type *type, void **data, size_t *len,
		 uint64_t timeout_ms);
	int fd() const { return readable_.fd(); }
	crnet_ws_state state() const
	{
		return static_cast<crnet_ws_state>(state_.load());
	}
	uint64_t buffered_amount() const { return buffered_.load(); }
	bool done() const { return closed_.IsSignaled(); }

	// Valid once open / once done (read-only from then on).
	const std::string &protocol() const { return protocol_; }
	const std::string &extensions() const { return extensions_; }
	const crnet_response *handshake() const { return handshake_.get(); }
	int close_code() const { return close_code_; }
	const std::string &close_reason() const { return close_reason_; }
	int net_error() const { return net_error_; }

	// Network thread.
	void StartOnNetworkThread();
	void FailOnNetworkThread(int err, int net_error);

private:
	friend class base::RefCountedThreadSafe<crnet_ws>;
	friend class WsEventAdapter;
	~crnet_ws();

	struct Message {
		crnet_ws_msg_type type;
		std::string data;
	};

	// WebSocketEventInterface, forwarded by WsEventAdapter.
	void OnConnected(const net::TransportInfo &info);
	void OnAddChannelResponse(
		std::unique_ptr<net::WebSocketHandshakeResponseInfo> response,
		const std::string &protocol, const std::string &extensions);
	void OnDataFrame(bool fin, net::WebSocketFrameHeader::OpCode type,
			 base::span<const char> payload);
	bool HasPendingDataFrames();
	void OnSendDataFrameDone();
	void OnClosingHandshake();
	void OnDropChannel(bool was_clean, uint16_t code,
			   const std::string &reason);
	void OnFailChannel(const std::string &message, int net_error,
			   std::optional<int> response_code);

	void Deliver(crnet_ws_msg_type type, const char *data, size_t len);
	void ReadMore();
	void SendOnNetworkThread(crnet_ws_msg_type type,
				 scoped_refptr<net::IOBuffer> buf, size_t len);
	void CloseOnNetworkThread(int code, std::string reason);
	void ResumeOnNetworkThread();
	void OnTimeout();
	// (Re)arms the idle timer if the connection is open and one is set.
	void RestartIdleTimer();
	// (Re)arms the auto-ping timer if the connection is open and one is
	// set.
	void RestartAutoPing();
	void OnPong(base::span<const char> payload);
	void SendPingOnNetworkThread(std::string payload);
	void Finish(int err, int code, const std::string &reason,
		    int net_error);
	// Keeps the fd readable exactly while Recv() would not block.
	void UpdateFdLocked();

	crnet_client *client_;
	scoped_refptr<base::SingleThreadTaskRunner> runner_;
	crnet::RequestSpec spec_;
	GURL url_;
	crnet::WsHeaders headers_;
	crnet_ws_callbacks cb_{};
	void *user_data_;
	crnet::FdFlag readable_;

	// Network thread only.
	std::unique_ptr<net::WebSocketChannel> channel_;
	base::OneShotTimer timer_;
	base::OneShotTimer idle_timer_;
	uint64_t idle_ms_ = 0;
	base::RepeatingTimer auto_ping_timer_;
	uint64_t auto_ping_ms_ = 0;
	crnet_ws_pong_cb pong_cb_ = nullptr;
	bool finished_ = false;
	bool paused_ = false;
	bool reading_stopped_ = false;
	bool proxy_auth_tried_ = false;
	crnet_ws_msg_type assembling_type_ = CRNET_WS_TEXT;
	std::string assembling_;
	// Messages that arrived while paused (callback mode).
	std::deque<Message> held_;
	std::deque<size_t> in_flight_;
	std::string negotiated_protocol_;
	std::string remote_;

	std::atomic<int> state_{CRNET_WS_CONNECTING};
	std::atomic<uint64_t> buffered_{0};

	// Written on the network thread before settled_/closed_ is signalled.
	std::string protocol_;
	std::string extensions_;
	std::unique_ptr<crnet_response> handshake_;
	int close_code_ = 0;
	std::string close_reason_;
	int net_error_ = 0;

	bool opened_ = false;
	int result_ = CRNET_E_STATE;
	// Signalled when open or finished, whichever comes first.
	crnet::OneShotEvent settled_;
	crnet::OneShotEvent closed_;

	// The receive queue (queue mode) is shared with Recv() callers.
	mutable std::mutex mu_;
	std::condition_variable cv_;
	std::deque<Message> queue_;
	// Changed under mu_, read without it by the network thread's
	// flow-control checks.
	std::atomic<size_t> queued_bytes_{0};
};

#endif // CRNET_SRC_WEBSOCKET_H
