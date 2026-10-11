// SPDX-License-Identifier: BSD-3-Clause
//
// WebSocket client: handshake fidelity, messaging in both delivery
// modes, closing semantics, failures, flow control, proxies.
//
#include <poll.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "common.h"

using namespace crtest;

namespace {

std::string WssUrl(const std::string &path)
{
	return "wss://localhost:" + std::to_string(ServerPorts().https_h1) + path;
}

std::string WsUrl(const std::string &path)
{
	return "ws://localhost:" + std::to_string(ServerPorts().http) + path;
}

// Collects callback events; thread-safe.
struct Events {
	std::mutex mu;
	std::condition_variable cv;
	bool opened = false;
	std::string protocol, extensions;
	int handshake_status = 0;
	std::vector<std::pair<crnet_ws_msg_type, std::string>> messages;
	int closes = 0;
	int error = 999, code = 0;
	std::string reason;

	bool WaitMessages(size_t n, int ms = 5000)
	{
		std::unique_lock<std::mutex> lk(mu);
		return cv.wait_for(lk, std::chrono::milliseconds(ms),
				   [&] { return messages.size() >= n || closes; }) &&
		       messages.size() >= n;
	}
	bool WaitClosed(int ms = 5000)
	{
		std::unique_lock<std::mutex> lk(mu);
		return cv.wait_for(lk, std::chrono::milliseconds(ms),
				   [&] { return closes > 0; });
	}
};

void OnOpen(crnet_ws *, const crnet_response *h, const char *proto,
	    const char *ext, void *ud)
{
	auto *e = static_cast<Events *>(ud);
	std::lock_guard<std::mutex> lk(e->mu);
	e->opened = true;
	e->protocol = proto;
	e->extensions = ext;
	e->handshake_status = crnet_response_status(h);
	e->cv.notify_all();
}

void OnMessage(crnet_ws *, crnet_ws_msg_type t, const void *d, size_t n,
	       void *ud)
{
	auto *e = static_cast<Events *>(ud);
	std::lock_guard<std::mutex> lk(e->mu);
	e->messages.emplace_back(t, std::string(static_cast<const char *>(d), n));
	e->cv.notify_all();
}

void OnClose(crnet_ws *, int err, int code, const char *reason, void *ud)
{
	auto *e = static_cast<Events *>(ud);
	std::lock_guard<std::mutex> lk(e->mu);
	e->closes++;
	e->error = err;
	e->code = code;
	e->reason = reason;
	e->cv.notify_all();
}

const crnet_ws_callbacks kCallbacks = {OnOpen, OnMessage, OnClose};
const crnet_ws_callbacks kQueueMode = {OnOpen, nullptr, OnClose};

crnet_ws *Connect(crnet_client *c, const std::string &url,
		  const crnet_ws_callbacks *cb, void *ud,
		  const char *protocols = nullptr,
		  void (*tweak)(crnet_request *) = nullptr)
{
	crnet_request *req = crnet_request_new(nullptr, url.c_str());
	EXPECT_NE(req, nullptr) << url;
	if (tweak)
		tweak(req);
	crnet_ws *ws = crnet_ws_connect(c, req, protocols, cb, ud);
	crnet_request_free(req);
	return ws;
}

std::string RecvText(crnet_ws *ws, int ms = 5000)
{
	crnet_ws_msg_type t;
	void *d = nullptr;
	size_t n = 0;
	int rc = crnet_ws_recv(ws, &t, &d, &n, ms);
	EXPECT_EQ(rc, CRNET_OK);
	std::string s = d ? std::string(static_cast<char *>(d), n) : "";
	crnet_free(d);
	return s;
}

} // namespace

TEST_F(ClientTest, WsEchoWithCallbacks)
{
	Events ev;
	crnet_ws *ws = Connect(client_, WssUrl("/ws/echo"), &kCallbacks, &ev);
	ASSERT_NE(ws, nullptr);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	EXPECT_EQ(crnet_ws_get_state(ws), CRNET_WS_OPEN);
	EXPECT_EQ(ev.handshake_status, 101);
	EXPECT_EQ(ev.extensions, "permessage-deflate");
	EXPECT_EQ(ev.protocol, "");
	EXPECT_STREQ(crnet_ws_extensions(ws), "permessage-deflate");

	std::string bin("\x00\x01\x02\xff", 4);
	EXPECT_EQ(crnet_ws_send(ws, CRNET_WS_TEXT, "hello", 5), CRNET_OK);
	EXPECT_EQ(crnet_ws_send(ws, CRNET_WS_BINARY, bin.data(), bin.size()), CRNET_OK);
	EXPECT_EQ(crnet_ws_send(ws, CRNET_WS_TEXT, "", 0), CRNET_OK);
	ASSERT_TRUE(ev.WaitMessages(3));
	EXPECT_EQ(ev.messages[0], std::make_pair(CRNET_WS_TEXT, std::string("hello")));
	EXPECT_EQ(ev.messages[1], std::make_pair(CRNET_WS_BINARY, bin));
	EXPECT_EQ(ev.messages[2], std::make_pair(CRNET_WS_TEXT, std::string()));

	EXPECT_EQ(crnet_ws_close(ws, 4000, "done"), CRNET_OK);
	EXPECT_EQ(crnet_ws_wait_closed(ws, 5000), CRNET_OK);
	EXPECT_EQ(ev.closes, 1);
	EXPECT_EQ(ev.error, CRNET_OK);
	EXPECT_EQ(ev.code, 4000);
	EXPECT_EQ(crnet_ws_get_state(ws), CRNET_WS_CLOSED);
	EXPECT_EQ(crnet_ws_close_code(ws), 4000);
	EXPECT_EQ(crnet_ws_net_error(ws), 0);
	crnet_ws_free(ws);
	EXPECT_EQ(ev.closes, 1);
}

TEST_F(ClientTest, WsQueueModeWithPollableFd)
{
	crnet_ws *ws = Connect(client_, WsUrl("/ws/echo"), nullptr, nullptr);
	ASSERT_NE(ws, nullptr);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	struct pollfd pfd = {crnet_ws_fd(ws), POLLIN, 0};
	EXPECT_EQ(poll(&pfd, 1, 0), 0) << "nothing queued yet";

	crnet_ws_msg_type t;
	void *d;
	size_t n;
	EXPECT_EQ(crnet_ws_recv(ws, &t, &d, &n, 50), CRNET_E_TIMEOUT);

	crnet_ws_send(ws, CRNET_WS_TEXT, "one", 3);
	crnet_ws_send(ws, CRNET_WS_TEXT, "two", 3);
	ASSERT_EQ(poll(&pfd, 1, 5000), 1);
	EXPECT_EQ(RecvText(ws), "one");
	EXPECT_EQ(RecvText(ws), "two");
	EXPECT_EQ(poll(&pfd, 1, 0), 0) << "fd drained with the queue";

	crnet_ws_close(ws, 1000, nullptr);
	ASSERT_EQ(poll(&pfd, 1, 5000), 1) << "fd readable once closed";
	EXPECT_EQ(crnet_ws_recv(ws, &t, &d, &n, 0), CRNET_E_STATE);
	EXPECT_EQ(crnet_ws_wait_closed(ws, 5000), CRNET_OK);
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsHandshakeMatchesChrome)
{
	Get(client_, HttpsUrl("/set-cookie?sid=1"));
	crnet_ws *ws = Connect(client_, WssUrl("/ws/headers"), nullptr, nullptr,
			       "chat, superchat");
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	EXPECT_STREQ(crnet_ws_protocol(ws), "chat");
	json doc = json::parse(RecvText(ws));
	crnet_ws_free(ws);

	// Captured from Chrome 155 (tests/fingerprint/capture_chrome.py).
	EXPECT_EQ(HeaderNames(doc), (std::vector<std::string>{
		"Host", "Connection", "Pragma", "Cache-Control", "User-Agent",
		"Upgrade", "Origin", "Sec-WebSocket-Version", "Accept-Encoding",
		"Accept-Language", "Cookie", "Sec-WebSocket-Key",
		"Sec-WebSocket-Extensions", "Sec-WebSocket-Protocol"}));
	EXPECT_EQ(EchoHeader(doc, "Connection"), "Upgrade");
	EXPECT_EQ(EchoHeader(doc, "Pragma"), "no-cache");
	EXPECT_EQ(EchoHeader(doc, "Cache-Control"), "no-cache");
	EXPECT_EQ(EchoHeader(doc, "User-Agent"), crnet_default_user_agent());
	EXPECT_EQ(EchoHeader(doc, "Upgrade"), "websocket");
	EXPECT_EQ(EchoHeader(doc, "Origin"),
		  "https://localhost:" + std::to_string(ServerPorts().https_h1));
	EXPECT_EQ(EchoHeader(doc, "Sec-WebSocket-Version"), "13");
	EXPECT_EQ(EchoHeader(doc, "Accept-Encoding"), "gzip, deflate, br, zstd");
	EXPECT_EQ(EchoHeader(doc, "Accept-Language"), "en-US,en;q=0.9");
	EXPECT_EQ(EchoHeader(doc, "Cookie"), "sid=1");
	EXPECT_EQ(EchoHeader(doc, "Sec-WebSocket-Extensions"),
		  "permessage-deflate; client_max_window_bits");
	EXPECT_EQ(EchoHeader(doc, "Sec-WebSocket-Protocol"), "chat, superchat");
	EXPECT_FALSE(EchoHasHeader(doc, "sec-ch-ua")) << "Chrome sends none";
}

TEST_F(ClientTest, WsHeaderOverrides)
{
	crnet_ws *ws = Connect(client_, WsUrl("/ws/headers"), nullptr, nullptr,
			       nullptr, [](crnet_request *r) {
				       crnet_request_set_header(r, "Origin", "https://app.example");
				       crnet_request_set_header(r, "User-Agent", "Bot/1");
				       crnet_request_set_header(r, "Accept-Language", nullptr);
				       crnet_request_set_header(r, "Authorization", "Bearer x");
				       crnet_request_set_header(r, "Cookie", "mine=1");
				       crnet_request_set_header(r, "Sec-WebSocket-Protocol", "superchat");
			       });
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	EXPECT_STREQ(crnet_ws_protocol(ws), "superchat");
	json doc = json::parse(RecvText(ws));
	crnet_ws_free(ws);
	EXPECT_EQ(EchoHeader(doc, "Origin"), "https://app.example");
	EXPECT_EQ(EchoHeader(doc, "User-Agent"), "Bot/1");
	EXPECT_EQ(EchoHeader(doc, "Authorization"), "Bearer x");
	EXPECT_EQ(EchoHeader(doc, "Cookie"), "mine=1");
	EXPECT_FALSE(EchoHasHeader(doc, "Accept-Language"));
	EXPECT_EQ(EchoHeader(doc, "Origin").find("ws"), std::string::npos);
}

TEST_F(ClientTest, WsHttpSchemeIsMapped)
{
	std::string url = "https://localhost:" + std::to_string(ServerPorts().https_h1) +
			  "/ws/echo";
	crnet_ws *ws = Connect(client_, url, nullptr, nullptr);
	ASSERT_NE(ws, nullptr);
	EXPECT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsUrlRejectedForHttpApi)
{
	crnet_request *req = crnet_request_new("GET", WsUrl("/echo").c_str());
	ASSERT_NE(req, nullptr);
	crnet_response *resp = nullptr;
	EXPECT_EQ(crnet_perform(client_, req, &resp), CRNET_E_INVAL);
	EXPECT_EQ(crnet_start(client_, req, nullptr, nullptr), nullptr);
	crnet_request_free(req);
}

TEST_F(ClientTest, WsCookieFromHandshakeLandsInJar)
{
	crnet_ws *ws = Connect(client_, WssUrl("/ws/cookie"), nullptr, nullptr);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	crnet_ws_free(ws);
	char *c = crnet_client_get_cookies(client_, HttpsUrl("/").c_str());
	EXPECT_STREQ(c, "wscookie=1");
	crnet_free(c);
}

TEST_F(ClientTest, WsWithoutDeflate)
{
	crnet_ws *ws = Connect(client_, WsUrl("/ws/echo?deflate=0"), nullptr, nullptr);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	EXPECT_STREQ(crnet_ws_extensions(ws), "");
	crnet_ws_send(ws, CRNET_WS_TEXT, "plain", 5);
	EXPECT_EQ(RecvText(ws), "plain");
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsServerInitiatedClose)
{
	Events ev;
	crnet_ws *ws = Connect(client_, WssUrl("/ws/close?code=4001&reason=bye"),
			       &kCallbacks, &ev);
	EXPECT_EQ(crnet_ws_wait_closed(ws, 5000), CRNET_OK);
	EXPECT_TRUE(ev.opened);
	EXPECT_EQ(ev.error, CRNET_OK);
	EXPECT_EQ(ev.code, 4001);
	EXPECT_EQ(ev.reason, "bye");
	EXPECT_STREQ(crnet_ws_close_reason(ws), "bye");
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsServerCloseAfterMessages)
{
	Events ev;
	crnet_ws *ws = Connect(client_, WssUrl("/ws/echo"), &kCallbacks, &ev);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	crnet_ws_send(ws, CRNET_WS_TEXT, "a", 1);
	crnet_ws_send(ws, CRNET_WS_TEXT, "close-me", 8);
	EXPECT_EQ(crnet_ws_wait_closed(ws, 5000), CRNET_OK);
	EXPECT_EQ(ev.messages.size(), 1u) << "echo before the server's close";
	EXPECT_EQ(ev.code, 1000);
	EXPECT_EQ(ev.reason, "server bye");
	EXPECT_EQ(crnet_ws_send(ws, CRNET_WS_TEXT, "x", 1), CRNET_E_STATE);
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsAbruptDrop)
{
	Events ev;
	crnet_ws *ws = Connect(client_, WssUrl("/ws/drop"), &kCallbacks, &ev);
	EXPECT_EQ(crnet_ws_wait_closed(ws, 5000), CRNET_E_NET);
	EXPECT_EQ(ev.code, 1006);
	EXPECT_LT(crnet_ws_net_error(ws), 0);
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsFragmentedMessagesAreReassembled)
{
	Events ev;
	crnet_ws *ws = Connect(client_, WsUrl("/ws/fragmented?deflate=0"),
			       &kCallbacks, &ev);
	ASSERT_TRUE(ev.WaitMessages(2));
	EXPECT_EQ(ev.messages[0], std::make_pair(CRNET_WS_TEXT, std::string("fragmented")));
	EXPECT_EQ(ev.messages[1],
		  std::make_pair(CRNET_WS_BINARY, std::string("\x00\x01\x02\x03", 4)));
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsLargeMessages)
{
	const size_t kSize = 6 * 1024 * 1024 + 7;
	crnet_ws *ws = Connect(client_, WssUrl("/ws/big?size=" + std::to_string(kSize)),
			       nullptr, nullptr);
	crnet_ws_msg_type t;
	void *d;
	size_t n;
	ASSERT_EQ(crnet_ws_recv(ws, &t, &d, &n, 10000), CRNET_OK);
	ASSERT_EQ(n, kSize);
	const unsigned char *p = static_cast<unsigned char *>(d);
	bool ok = true;
	for (size_t i = 0; i < n && ok; i++)
		ok = p[i] == (i & 0xff);
	EXPECT_TRUE(ok);
	crnet_free(d);

	// And upload one, echoed back.
	std::string big(3 * 1024 * 1024, 'z');
	ASSERT_EQ(crnet_ws_send(ws, CRNET_WS_BINARY, big.data(), big.size()), CRNET_OK);
	ASSERT_EQ(crnet_ws_recv(ws, &t, &d, &n, 10000), CRNET_OK);
	EXPECT_EQ(n, big.size());
	crnet_free(d);
	EXPECT_EQ(crnet_ws_buffered_amount(ws), 0u);
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsMaxMessageSize)
{
	Events ev;
	crnet_ws *ws = Connect(client_, WssUrl("/ws/big?size=100000"), &kCallbacks, &ev,
			       nullptr, [](crnet_request *r) {
				       crnet_request_set_max_body_size(r, 1000);
			       });
	EXPECT_EQ(crnet_ws_wait_closed(ws, 5000), CRNET_E_TOO_LARGE);
	EXPECT_EQ(ev.code, 1009);
	EXPECT_TRUE(ev.messages.empty());
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsBurstWithBackpressureInQueueMode)
{
	// 2000 x 4 KiB = 8 MiB, twice the receive queue limit.
	crnet_ws *ws = Connect(client_, WsUrl("/ws/burst?n=2000&size=4096"),
			       nullptr, nullptr);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	for (uint32_t i = 0; i < 2000; i++) {
		crnet_ws_msg_type t;
		void *d;
		size_t n;
		ASSERT_EQ(crnet_ws_recv(ws, &t, &d, &n, 10000), CRNET_OK) << i;
		const unsigned char *p = static_cast<unsigned char *>(d);
		uint32_t idx = (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
		EXPECT_EQ(idx, i);
		EXPECT_EQ(n, 4096u);
		crnet_free(d);
	}
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsPauseAndResume)
{
	Events ev;
	crnet_ws *ws = Connect(client_, WsUrl("/ws/echo"), &kCallbacks, &ev);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	crnet_ws_pause(ws);
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	crnet_ws_send(ws, CRNET_WS_TEXT, "held", 4);
	EXPECT_FALSE(ev.WaitMessages(1, 300));
	crnet_ws_resume(ws);
	EXPECT_TRUE(ev.WaitMessages(1));
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsServerPingIsAnswered)
{
	crnet_ws *ws = Connect(client_, WsUrl("/ws/ping"), nullptr, nullptr);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	crnet_ws_send(ws, CRNET_WS_TEXT, "stats", 5);
	json stats = json::parse(RecvText(ws));
	EXPECT_GE(stats["ws_pongs"].get<int>(), 1);
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsInvalidUtf8FailsTheConnection)
{
	Events ev;
	crnet_ws *ws = Connect(client_, WsUrl("/ws/badutf8?deflate=0"), &kCallbacks, &ev);
	EXPECT_EQ(crnet_ws_wait_closed(ws, 5000), CRNET_E_NET);
	EXPECT_TRUE(ev.messages.empty());
	EXPECT_EQ(crnet_ws_net_error(ws), -145);  // ERR_WS_PROTOCOL_ERROR
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsRejectedHandshake)
{
	Events ev;
	crnet_ws *ws = Connect(client_, WssUrl("/ws/reject"), &kCallbacks, &ev);
	EXPECT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_E_NET);
	EXPECT_FALSE(ev.opened);
	EXPECT_EQ(ev.closes, 1);
	const crnet_response *h = crnet_ws_handshake_response(ws);
	ASSERT_NE(h, nullptr);
	EXPECT_EQ(crnet_response_status(h), 403);
	EXPECT_NE(ev.reason.find("403"), std::string::npos) << ev.reason;
	EXPECT_EQ(crnet_ws_net_error(ws), -320);  // ERR_INVALID_RESPONSE
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsUnofferedSubprotocolFails)
{
	crnet_ws *ws = Connect(client_, WssUrl("/ws/badproto"), nullptr, nullptr, "chat");
	EXPECT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_E_NET);
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsHandshakeTimeout)
{
	crnet_ws *ws = Connect(client_, WsUrl("/ws/slow-handshake?ms=3000"),
			       nullptr, nullptr, nullptr, [](crnet_request *r) {
				       crnet_request_set_timeout(r, 200);
			       });
	EXPECT_EQ(crnet_ws_send(ws, CRNET_WS_TEXT, "early", 5), CRNET_E_STATE);
	EXPECT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_E_TIMEOUT);
	EXPECT_EQ(crnet_ws_net_error(ws), -7);
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsArgumentValidation)
{
	crnet_ws *ws = Connect(client_, WsUrl("/ws/echo"), nullptr, nullptr);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	EXPECT_EQ(crnet_ws_send(ws, CRNET_WS_TEXT, "\xff\xfe", 2), CRNET_E_INVAL);
	EXPECT_EQ(crnet_ws_send(ws, (crnet_ws_msg_type)7, "x", 1), CRNET_E_INVAL);
	EXPECT_EQ(crnet_ws_close(ws, 1006, nullptr), CRNET_E_INVAL);
	EXPECT_EQ(crnet_ws_close(ws, 2000, nullptr), CRNET_E_INVAL);
	EXPECT_EQ(crnet_ws_close(ws, 1000, std::string(124, 'r').c_str()), CRNET_E_INVAL);
	EXPECT_EQ(crnet_ws_close(ws, 1000, nullptr), CRNET_OK);
	EXPECT_EQ(crnet_ws_close(ws, 1000, nullptr), CRNET_E_STATE);
	crnet_ws_free(ws);

	EXPECT_EQ(Connect(client_, WsUrl("/ws/echo"), nullptr, nullptr, "bad proto"),
		  nullptr);
	EXPECT_EQ(crnet_ws_connect(nullptr, nullptr, nullptr, nullptr, nullptr), nullptr);
}

TEST_F(ClientTest, WsCloseWhileConnectingAborts)
{
	Events ev;
	crnet_ws *ws = Connect(client_, WsUrl("/ws/slow-handshake?ms=2000"),
			       &kCallbacks, &ev);
	EXPECT_EQ(crnet_ws_close(ws, 1000, nullptr), CRNET_OK);
	EXPECT_EQ(crnet_ws_wait_closed(ws, 5000), CRNET_E_CANCELED);
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsFreeWhileOpenAborts)
{
	auto *ev = new Events;
	crnet_ws *ws = Connect(client_, WsUrl("/ws/echo"), &kCallbacks, ev);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	crnet_ws_free(ws);
	EXPECT_EQ(ev->closes, 1);
	EXPECT_EQ(ev->error, CRNET_E_CANCELED);
	delete ev;
}

TEST_F(ClientTest, WsFreeFromOnClose)
{
	std::atomic<bool> freed{false};
	crnet_ws_callbacks cb = {};
	cb.on_close = [](crnet_ws *ws, int, int, const char *, void *ud) {
		crnet_ws_free(ws);
		static_cast<std::atomic<bool> *>(ud)->store(true);
	};
	crnet_ws *ws = Connect(client_, WsUrl("/ws/close"), &cb, &freed);
	ASSERT_NE(ws, nullptr);
	for (int i = 0; i < 500 && !freed; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	EXPECT_TRUE(freed);
}

TEST(WebSocket, ClientFreeClosesSockets)
{
	crnet_client *c = NewTestClient();
	Events ev[3];
	crnet_ws *ws[3];
	for (int i = 0; i < 3; i++) {
		ws[i] = Connect(c, WsUrl("/ws/echo"), &kCallbacks, &ev[i]);
		ASSERT_EQ(crnet_ws_wait_open(ws[i], 5000), CRNET_OK);
	}
	crnet_client_free(c);
	for (int i = 0; i < 3; i++) {
		EXPECT_EQ(ev[i].error, CRNET_E_SHUTDOWN);
		EXPECT_EQ(crnet_ws_get_state(ws[i]), CRNET_WS_CLOSED);
		crnet_ws_free(ws[i]);
	}
}

TEST_F(ClientTest, WsManyConcurrentSockets)
{
	const int kN = 50;
	std::vector<crnet_ws *> ws;
	for (int i = 0; i < kN; i++)
		ws.push_back(Connect(client_, WssUrl("/ws/echo"), nullptr, nullptr));
	for (int i = 0; i < kN; i++) {
		ASSERT_EQ(crnet_ws_wait_open(ws[i], 10000), CRNET_OK) << i;
		std::string m = "msg" + std::to_string(i);
		crnet_ws_send(ws[i], CRNET_WS_TEXT, m.data(), m.size());
	}
	for (int i = 0; i < kN; i++) {
		EXPECT_EQ(RecvText(ws[i]), "msg" + std::to_string(i));
		crnet_ws_free(ws[i]);
	}
}

TEST(WebSocket, ThroughSocks5Proxy)
{
	crnet_client_config *cfg = crnet_client_config_new();
	std::string p = "socks5h://127.0.0.1:" + std::to_string(ServerPorts().socks5);
	crnet_client_config_set_proxy(cfg, p.c_str());
	crnet_client *c = NewTestClient(cfg);
	crnet_client_config_free(cfg);
	std::string url = "wss://www.crnet-test:" +
			  std::to_string(ServerPorts().https_h1) + "/ws/echo";
	crnet_ws *ws = Connect(c, url, nullptr, nullptr);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	crnet_ws_send(ws, CRNET_WS_TEXT, "via socks", 9);
	EXPECT_EQ(RecvText(ws), "via socks");
	crnet_ws_free(ws);
	crnet_client_free(c);
}

TEST(WebSocket, ThroughHttpProxyWithAuth)
{
	crnet_client_config *cfg = crnet_client_config_new();
	std::string p = "http://crnet:secret@127.0.0.1:" +
			std::to_string(ServerPorts().http_proxy);
	crnet_client_config_set_proxy(cfg, p.c_str());
	crnet_client *c = NewTestClient(cfg);
	crnet_client_config_free(cfg);
	std::string url = "wss://www.crnet-test:" +
			  std::to_string(ServerPorts().https_h1) + "/ws/echo";
	crnet_ws *ws = Connect(c, url, nullptr, nullptr);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	crnet_ws_send(ws, CRNET_WS_TEXT, "via http proxy", 14);
	EXPECT_EQ(RecvText(ws), "via http proxy");
	crnet_ws_free(ws);
	crnet_client_free(c);
}

TEST_F(ClientTest, WsCloseWithoutStatusCode)
{
	crnet_ws *ws = Connect(client_, WsUrl("/ws/echo?emptyclose=1"), nullptr, nullptr);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	crnet_ws_close(ws, 1000, "bye");
	EXPECT_EQ(crnet_ws_wait_closed(ws, 5000), CRNET_OK);
	EXPECT_EQ(crnet_ws_close_code(ws), 1005);  // RFC 6455: no status received
	crnet_ws_free(ws);
}

// ------------------------------------------------------------------
// WebSockets over HTTP/2 (RFC 8441)
// ------------------------------------------------------------------

namespace {

std::string WsH2Host()
{
	return "localhost:" + std::to_string(ServerPorts().https_ws_h2);
}

std::string HeaderValue(const json &doc, const std::string &name)
{
	for (const json &h : doc["headers"]) {
		if (h[0] == name)
			return h[1];
	}
	return "";
}

} // namespace

// Chrome runs a WebSocket over an existing HTTP/2 connection whose server
// announced extended CONNECT; the handshake is then a CONNECT request
// with :protocol websocket on a new stream of that connection.
TEST_F(ClientTest, WsOverHttp2UsesTheExistingConnection)
{
	Result warm = Get(client_, "https://" + WsH2Host() + "/echo");
	ASSERT_EQ(warm.rc, CRNET_OK);
	ASSERT_EQ(warm.protocol(), "h2");

	crnet_ws *ws = Connect(client_, "wss://" + WsH2Host() + "/ws/headers", nullptr, nullptr,
			       "chat");
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK) << crnet_ws_net_error(ws);
	json doc = json::parse(RecvText(ws));
	EXPECT_EQ(doc["http_version"], "h2");
	EXPECT_EQ(HeaderValue(doc, ":method"), "CONNECT");
	EXPECT_EQ(HeaderValue(doc, ":protocol"), "websocket");
	EXPECT_EQ(HeaderValue(doc, ":path"), "/ws/headers");
	EXPECT_EQ(HeaderValue(doc, "sec-websocket-version"), "13");
	EXPECT_EQ(HeaderValue(doc, "sec-websocket-key"), "") << "no key over HTTP/2";
	EXPECT_EQ(doc["protocol"], "chat");
	EXPECT_STREQ(crnet_ws_protocol(ws), "chat");
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsOverHttp2EchoAndClose)
{
	Get(client_, "https://" + WsH2Host() + "/echo");
	crnet_ws *ws = Connect(client_, "wss://" + WsH2Host() + "/ws/echo", nullptr, nullptr);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK) << crnet_ws_net_error(ws);
	EXPECT_NE(std::string(crnet_ws_extensions(ws)).find("permessage-deflate"),
		  std::string::npos);
	std::string big(300000, 'z');	// beyond one flow-control window
	for (const std::string &msg : {std::string("hello over h2"), big}) {
		ASSERT_EQ(crnet_ws_send(ws, CRNET_WS_TEXT, msg.data(), msg.size()), CRNET_OK);
		EXPECT_EQ(RecvText(ws), msg);
	}
	crnet_ws_send(ws, CRNET_WS_TEXT, "stats", 5);
	json st = json::parse(RecvText(ws));
	EXPECT_GT(st["ws_h2_connects"].get<int>(), 0);
	ASSERT_EQ(crnet_ws_close(ws, 1000, "done"), CRNET_OK);
	EXPECT_EQ(crnet_ws_wait_closed(ws, 5000), CRNET_OK);
	EXPECT_EQ(crnet_ws_close_code(ws), 1000);
	crnet_ws_free(ws);
}

// Without an HTTP/2 connection to reuse, Chrome does not open one for a
// WebSocket: the handshake is an HTTP/1.1 upgrade.
TEST_F(ClientTest, WsWithoutAnHttp2ConnectionUsesHttp11)
{
	crnet_ws *ws = Connect(client_, "wss://" + WsH2Host() + "/ws/headers", nullptr, nullptr);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK) << crnet_ws_net_error(ws);
	EXPECT_EQ(json::parse(RecvText(ws))["http_version"], "http/1.1");
	crnet_ws_free(ws);
}

// ------------------------------------------------------------------
// Idle timeout
// ------------------------------------------------------------------

TEST_F(ClientTest, WsIdleTimeoutDropsASilentConnection)
{
	crnet_ws *ws = Connect(client_, WsUrl("/ws/echo"), nullptr, nullptr);
	crnet_ws_set_idle_timeout(ws, 300);	// before it is open
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	auto t0 = std::chrono::steady_clock::now();
	EXPECT_EQ(crnet_ws_wait_closed(ws, 5000), CRNET_E_TIMEOUT);
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			  std::chrono::steady_clock::now() - t0)
			  .count();
	EXPECT_GE(ms, 250);
	EXPECT_LT(ms, 3000);
	EXPECT_EQ(crnet_ws_close_code(ws), 1006);
	EXPECT_EQ(crnet_ws_net_error(ws), -7);	// ERR_TIMED_OUT
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsIdleTimeoutRestartsWithEveryMessage)
{
	crnet_ws *ws = Connect(client_, WsUrl("/ws/echo"), nullptr, nullptr);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	crnet_ws_set_idle_timeout(ws, 400);
	for (int i = 0; i < 10; i++) {	// ~1.5 s, far beyond 400 ms
		ASSERT_EQ(crnet_ws_send(ws, CRNET_WS_TEXT, "beat", 4), CRNET_OK);
		EXPECT_EQ(RecvText(ws), "beat");
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
	}
	EXPECT_EQ(crnet_ws_get_state(ws), CRNET_WS_OPEN);
	crnet_ws_wait_closed(ws, 5000);
	EXPECT_EQ(crnet_ws_get_state(ws), CRNET_WS_CLOSED);
	EXPECT_EQ(crnet_ws_net_error(ws), -7);
	crnet_ws_free(ws);
}

// A paused application is not a silent peer.
TEST_F(ClientTest, WsIdleTimeoutWaitsWhilePaused)
{
	crnet_ws *ws = Connect(client_, WsUrl("/ws/echo"), nullptr, nullptr);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	crnet_ws_pause(ws);
	crnet_ws_set_idle_timeout(ws, 200);
	std::this_thread::sleep_for(std::chrono::milliseconds(800));
	EXPECT_EQ(crnet_ws_get_state(ws), CRNET_WS_OPEN) << "closed while paused";
	crnet_ws_resume(ws);
	// CRNET_E_TIMEOUT is also what an expired wait returns: check the state.
	crnet_ws_wait_closed(ws, 5000);
	EXPECT_EQ(crnet_ws_get_state(ws), CRNET_WS_CLOSED);
	EXPECT_EQ(crnet_ws_net_error(ws), -7);
	crnet_ws_free(ws);
}

TEST_F(ClientTest, WsIdleTimeoutZeroDisables)
{
	crnet_ws *ws = Connect(client_, WsUrl("/ws/echo"), nullptr, nullptr);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	crnet_ws_set_idle_timeout(ws, 200);
	crnet_ws_set_idle_timeout(ws, 0);
	std::this_thread::sleep_for(std::chrono::milliseconds(600));
	EXPECT_EQ(crnet_ws_get_state(ws), CRNET_WS_OPEN);
	crnet_ws_send(ws, CRNET_WS_TEXT, "still here", 10);
	EXPECT_EQ(RecvText(ws), "still here");
	crnet_ws_free(ws);
}
