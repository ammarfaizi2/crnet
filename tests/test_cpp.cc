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
