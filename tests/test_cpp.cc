// SPDX-License-Identifier: BSD-3-Clause
//
// The header-only C++ wrapper (crnet.hpp).
//
#include <atomic>

#include <crnet/crnet.hpp>

#include "common.h"

using namespace crtest;

namespace {

crnet::Client TestClient()
{
	ServerPorts();
	return crnet::Client(crnet::ClientConfig().insecure(true));
}

} // namespace

TEST(Cpp, GetAndPost)
{
	crnet::Client c = TestClient();
	crnet::Response r = c.get(HttpsUrl("/echo"));
	EXPECT_EQ(r.status(), 200);
	EXPECT_EQ(r.protocol(), "h2");
	EXPECT_TRUE(r.has_header("content-type"));

	crnet::Response p = c.post(HttpsUrl("/echo"), "{\"k\":1}", "application/json");
	json e = json::parse(std::string(p.body()));
	EXPECT_EQ(e["method"], "POST");
	EXPECT_EQ(e["body"], "{\"k\":1}");
	EXPECT_EQ(EchoHeader(e, "content-type"), "application/json");
}

TEST(Cpp, RequestBuilder)
{
	crnet::Client c = TestClient();
	crnet::Request req(HttpsUrl("/echo"), "PUT");
	req.mode(CRNET_MODE_FETCH)
		.header("Accept", "application/json")
		.header("X-A", "1")
		.remove_header("sec-ch-ua")
		.body("data")
		.timeout_ms(5000);
	crnet::Response r = c.perform(req);
	json e = json::parse(std::string(r.body()));
	EXPECT_EQ(e["method"], "PUT");
	EXPECT_EQ(EchoHeader(e, "accept"), "application/json");
	EXPECT_EQ(EchoHeader(e, "sec-fetch-mode"), "cors");
	EXPECT_FALSE(EchoHasHeader(e, "sec-ch-ua"));
}

TEST(Cpp, ErrorsThrow)
{
	crnet::Client c = TestClient();
	try {
		c.get("https://does-not-exist.invalid/");
		FAIL() << "expected crnet::Error";
	} catch (const crnet::Error &e) {
		EXPECT_EQ(e.code(), CRNET_E_NET);
		EXPECT_NE(std::string(e.what()).find("ERR_NAME_NOT_RESOLVED"), std::string::npos);
	}
	EXPECT_THROW(crnet::Request("nope"), crnet::Error);
	EXPECT_THROW(crnet::ClientConfig().proxy("bogus"), crnet::Error);
}

TEST(Cpp, HeadersVectorAndMove)
{
	crnet::Client c = TestClient();
	crnet::Response r = c.get(HttpsH1Url("/dup-headers"));
	auto hs = r.headers();
	int dups = 0;
	for (auto &kv : hs)
		dups += kv.first == "x-dup";
	EXPECT_EQ(dups, 2);
	crnet::Response moved = std::move(r);
	EXPECT_FALSE(static_cast<bool>(r));
	EXPECT_EQ(moved.status(), 200);
}

TEST(Cpp, AsyncHandlers)
{
	crnet::Client c = TestClient();
	std::atomic<size_t> bytes{0};
	std::atomic<int> status{0}, done{0}, err{-1};
	crnet::Handlers h;
	h.on_response = [&](const crnet::ResponseView &r) { status = r.status(); return true; };
	h.on_data = [&](std::string_view d) { bytes += d.size(); return true; };
	h.on_complete = [&](int e, const crnet::ResponseView &) { err = e; done++; };
	crnet::Transfer t = c.start(crnet::Request(HttpsUrl("/bytes/123456")), std::move(h));
	EXPECT_EQ(t.wait(), CRNET_OK);
	EXPECT_TRUE(t.done());
	EXPECT_EQ(status, 200);
	EXPECT_EQ(bytes, 123456u);
	EXPECT_EQ(done, 1);
	EXPECT_EQ(err, CRNET_OK);
}

TEST(Cpp, ProgressHandler)
{
	crnet::Client c = TestClient();
	std::atomic<int> calls{0};
	std::atomic<int64_t> last{-1};
	crnet::Handlers h;
	h.on_progress = [&](crnet_transfer *t) {
		calls++;
		last = crnet_transfer_progress(t, CRNET_PROGRESS_DOWNLOAD_DECODED);
	};
	h.progress_interval_ms = 10;
	crnet::Transfer t = c.start(
		crnet::Request(HttpsUrl("/stream/100000?chunk=10000&delay_ms=20")), std::move(h));
	EXPECT_EQ(t.wait(), CRNET_OK);
	EXPECT_GE(calls, 2);
	EXPECT_EQ(last, 100000);
	EXPECT_EQ(t.progress(CRNET_PROGRESS_DOWNLOAD_DECODED), 100000);
}

TEST(Cpp, TransferDestructorCancels)
{
	crnet::Client c = TestClient();
	std::atomic<int> err{0};
	{
		crnet::Handlers h;
		h.on_complete = [&](int e, const crnet::ResponseView &) { err = e; };
		crnet::Transfer t = c.start(crnet::Request(HttpsUrl("/delay/5000")), std::move(h));
	}
	EXPECT_EQ(err, CRNET_E_CANCELED);
}

TEST(Cpp, Cookies)
{
	crnet::Client c = TestClient();
	c.set_cookie(HttpsUrl("/"), "a=1");
	EXPECT_EQ(c.cookies(HttpsUrl("/")), "a=1");
	c.clear_cookies();
	EXPECT_EQ(c.cookies(HttpsUrl("/")), "");
}

TEST(Cpp, WebSocketQueueMode)
{
	crnet::Client c = TestClient();
	std::string url = "wss://localhost:" + std::to_string(ServerPorts().https_h1) +
			  "/ws/echo";
	crnet::WebSocket ws = c.websocket(url, "chat");
	ws.wait_open(5000);
	EXPECT_EQ(ws.state(), CRNET_WS_OPEN);
	EXPECT_EQ(ws.protocol(), "chat");
	EXPECT_EQ(ws.handshake().status(), 101);
	ws.send_text("hi");
	ws.send_binary(std::string("\x00\x01", 2));
	auto m1 = ws.recv(5000);
	auto m2 = ws.recv(5000);
	ASSERT_TRUE(m1 && m2);
	EXPECT_TRUE(m1->is_text());
	EXPECT_EQ(m1->data, "hi");
	EXPECT_EQ(m2->type, CRNET_WS_BINARY);
	EXPECT_EQ(m2->data, std::string("\x00\x01", 2));
	ws.close(1000, "bye");
	EXPECT_EQ(ws.wait_closed(5000), CRNET_OK);
	EXPECT_FALSE(ws.recv(0).has_value());
	EXPECT_THROW(ws.send_text("late"), crnet::Error);
}

TEST(Cpp, WebSocketHandlers)
{
	crnet::Client c = TestClient();
	std::string url = "ws://localhost:" + std::to_string(ServerPorts().http) +
			  "/ws/close?code=4002&reason=done";
	std::atomic<int> opened{0}, closed{0}, code{0};
	crnet::WsHandlers h;
	h.on_open = [&](const crnet::ResponseView &r, const std::string &, const std::string &) {
		opened = r.status();
	};
	h.on_close = [&](int err, int c2, const std::string &reason) {
		code = err == CRNET_OK && reason == "done" ? c2 : -1;
		closed++;
	};
	crnet::WebSocket ws = c.websocket(crnet::Request(url), "", std::move(h));
	EXPECT_EQ(ws.wait_closed(5000), CRNET_OK);
	EXPECT_EQ(opened, 101);
	EXPECT_EQ(closed, 1);
	EXPECT_EQ(code, 4002);
}

TEST(Cpp, WebSocketOpenFailureThrows)
{
	crnet::Client c = TestClient();
	std::string url = "wss://localhost:" + std::to_string(ServerPorts().https_h1) +
			  "/ws/reject";
	crnet::WebSocket ws = c.websocket(url);
	try {
		ws.wait_open(5000);
		FAIL() << "expected crnet::Error";
	} catch (const crnet::Error &e) {
		EXPECT_EQ(e.code(), CRNET_E_NET);
		EXPECT_NE(std::string(e.what()).find("403"), std::string::npos) << e.what();
	}
}

TEST(Cpp, StreamedUploadAndDownload)
{
	crnet::Client c = TestClient();
	std::string data(3 * 1024 * 1024 + 1, 'q');

	crnet::Request up(HttpsUrl("/upload-digest"), "POST");
	up.body_stream(data.size()).stream_response();
	crnet::Transfer t = c.start(up);
	for (size_t off = 0; off < data.size(); off += 100000)
		t.write(std::string_view(data).substr(off, 100000));
	t.write_end();
	EXPECT_EQ(t.wait_response(10000).status(), 200);
	std::string body, chunk;
	while (!(chunk = t.read(4096, 10000)).empty())
		body += chunk;
	EXPECT_EQ(json::parse(body)["len"], data.size());

	crnet::Transfer d = c.start(crnet::Request(HttpsUrl("/bytes/1000000")).stream_response());
	size_t total = 0;
	char buf[50000];
	while (size_t n = d.read(buf, sizeof(buf)))
		total += n;
	EXPECT_EQ(total, 1000000u);
}

TEST(Cpp, StreamingErrorsThrow)
{
	crnet::Client c = TestClient();
	crnet::Transfer t = c.start(crnet::Request(HttpsUrl("/delay/2000")).stream_response());
	try {
		t.read(16, 10);
		FAIL() << "expected a timeout";
	} catch (const crnet::Error &e) {
		EXPECT_EQ(e.code(), CRNET_E_TIMEOUT);
	}
	EXPECT_THROW(t.write("x"), crnet::Error);  // not a streamed body
	EXPECT_THROW(crnet::Request(HttpsUrl("/")).body_stream(-5), crnet::Error);
}
