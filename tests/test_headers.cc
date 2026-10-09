// SPDX-License-Identifier: BSD-3-Clause
//
// Browser emulation: request headers, their values and their order.
//
// The expected sequences below were captured from Google Chrome 155
// (headless) talking to tests/server/test_server.py; see
// docs/fingerprint.md for how to re-capture them for a new release.
//
#include <algorithm>

#include "common.h"

using namespace crtest;

namespace {

using Names = std::vector<std::string>;

json EchoOf(crhttp_client *c, crhttp_request *req)
{
	Result r = Perform(c, req);
	EXPECT_EQ(r.rc, CRHTTP_OK) << crhttp_response_error_string(r.resp);
	if (r.rc != CRHTTP_OK)
		return json::object({{"headers", json::array()}});
	return r.echo();
}

crhttp_request *Req(const char *method, const std::string &url,
		    crhttp_mode mode = CRHTTP_MODE_NAVIGATE)
{
	crhttp_request *r = crhttp_request_new(method, url.c_str());
	crhttp_request_set_mode(r, mode);
	return r;
}

const char kNavAccept[] =
	"text/html,application/xhtml+xml,application/xml;q=0.9,image/jxl,"
	"image/avif,image/webp,image/apng,*/*;q=0.8,"
	"application/signed-exchange;v=b3;q=0.7";

} // namespace

TEST_F(ClientTest, NavigateHttp2MatchesChrome)
{
	crhttp_request *req = Req("GET", HttpsUrl("/echo"));
	json e = EchoOf(client_, req);
	crhttp_request_free(req);

	EXPECT_EQ(HeaderNames(e), (Names{
		":method", ":authority", ":scheme", ":path",
		"sec-ch-ua", "sec-ch-ua-mobile", "sec-ch-ua-platform",
		"upgrade-insecure-requests", "user-agent", "accept",
		"sec-fetch-site", "sec-fetch-mode", "sec-fetch-user",
		"sec-fetch-dest", "accept-encoding", "accept-language",
		"priority"}));
	EXPECT_EQ(EchoHeader(e, "sec-ch-ua"), crhttp_default_sec_ch_ua());
	EXPECT_EQ(EchoHeader(e, "sec-ch-ua-mobile"), "?0");
	EXPECT_EQ(EchoHeader(e, "sec-ch-ua-platform"), "\"Linux\"");
	EXPECT_EQ(EchoHeader(e, "upgrade-insecure-requests"), "1");
	EXPECT_EQ(EchoHeader(e, "user-agent"), crhttp_default_user_agent());
	EXPECT_EQ(EchoHeader(e, "accept"), kNavAccept);
	EXPECT_EQ(EchoHeader(e, "sec-fetch-site"), "none");
	EXPECT_EQ(EchoHeader(e, "sec-fetch-mode"), "navigate");
	EXPECT_EQ(EchoHeader(e, "sec-fetch-user"), "?1");
	EXPECT_EQ(EchoHeader(e, "sec-fetch-dest"), "document");
	EXPECT_EQ(EchoHeader(e, "accept-encoding"), "gzip, deflate, br, zstd");
	EXPECT_EQ(EchoHeader(e, "accept-language"), "en-US,en;q=0.9");
	EXPECT_EQ(EchoHeader(e, "priority"), "u=0, i");
}

TEST_F(ClientTest, NavigateHttp11MatchesChromeIncludingCase)
{
	crhttp_request *req = Req("GET", HttpsH1Url("/echo"));
	json e = EchoOf(client_, req);
	crhttp_request_free(req);

	EXPECT_EQ(HeaderNames(e), (Names{
		"Host", "Connection", "sec-ch-ua", "sec-ch-ua-mobile",
		"sec-ch-ua-platform", "Upgrade-Insecure-Requests",
		"User-Agent", "Accept", "Sec-Fetch-Site", "Sec-Fetch-Mode",
		"Sec-Fetch-User", "Sec-Fetch-Dest", "Accept-Encoding",
		"Accept-Language"}));
	EXPECT_EQ(EchoHeader(e, "Connection"), "keep-alive");
}

TEST_F(ClientTest, NavigateWithRefererIsSameOrigin)
{
	crhttp_request *req = Req("GET", HttpsUrl("/echo"));
	crhttp_request_set_header(req, "Referer", HttpsUrl("/capture.html").c_str());
	json e = EchoOf(client_, req);
	crhttp_request_free(req);

	EXPECT_EQ(HeaderNames(e), (Names{
		":method", ":authority", ":scheme", ":path",
		"sec-ch-ua", "sec-ch-ua-mobile", "sec-ch-ua-platform",
		"upgrade-insecure-requests", "user-agent", "accept",
		"sec-fetch-site", "sec-fetch-mode", "sec-fetch-user",
		"sec-fetch-dest", "referer", "accept-encoding",
		"accept-language", "priority"}));
	EXPECT_EQ(EchoHeader(e, "referer"), HttpsUrl("/capture.html"));
	EXPECT_EQ(EchoHeader(e, "sec-fetch-site"), "same-origin");
}

TEST_F(ClientTest, NavigateFormPostMatchesChrome)
{
	crhttp_request *req = Req("POST", HttpsUrl("/echo"));
	crhttp_request_set_header(req, "Referer", HttpsUrl("/form.html").c_str());
	crhttp_request_set_header(req, "Content-Type",
				  "application/x-www-form-urlencoded");
	crhttp_request_set_body(req, "a=b", 3);
	json e = EchoOf(client_, req);
	crhttp_request_free(req);

	EXPECT_EQ(HeaderNames(e), (Names{
		":method", ":authority", ":scheme", ":path",
		"content-length", "cache-control", "sec-ch-ua",
		"sec-ch-ua-mobile", "sec-ch-ua-platform",
		"upgrade-insecure-requests", "content-type", "user-agent",
		"origin", "accept", "sec-fetch-site", "sec-fetch-mode",
		"sec-fetch-user", "sec-fetch-dest", "referer",
		"accept-encoding", "accept-language", "priority"}));
	EXPECT_EQ(EchoHeader(e, "cache-control"), "max-age=0");
	EXPECT_EQ(EchoHeader(e, "origin"),
		  "https://localhost:" + std::to_string(ServerPorts().https));
	EXPECT_EQ(e["body"], "a=b");
}

TEST_F(ClientTest, FetchGetMatchesChrome)
{
	crhttp_request *req = Req("GET", HttpsUrl("/echo"), CRHTTP_MODE_FETCH);
	crhttp_request_set_header(req, "Referer", HttpsUrl("/app.html").c_str());
	json e = EchoOf(client_, req);
	crhttp_request_free(req);

	EXPECT_EQ(HeaderNames(e), (Names{
		":method", ":authority", ":scheme", ":path",
		"sec-ch-ua-platform", "user-agent", "sec-ch-ua",
		"sec-ch-ua-mobile", "accept", "sec-fetch-site",
		"sec-fetch-mode", "sec-fetch-dest", "referer",
		"accept-encoding", "accept-language", "priority"}));
	EXPECT_EQ(EchoHeader(e, "accept"), "*/*");
	EXPECT_EQ(EchoHeader(e, "sec-fetch-site"), "same-origin");
	EXPECT_EQ(EchoHeader(e, "sec-fetch-mode"), "cors");
	EXPECT_EQ(EchoHeader(e, "sec-fetch-dest"), "empty");
	EXPECT_EQ(EchoHeader(e, "priority"), "u=1, i");
	EXPECT_FALSE(EchoHasHeader(e, "origin")) << "same-origin GET";
	EXPECT_FALSE(EchoHasHeader(e, "upgrade-insecure-requests"));
}

TEST_F(ClientTest, FetchWithPageSetAcceptMovesItLikeChrome)
{
	crhttp_request *req = Req("GET", HttpsUrl("/echo"), CRHTTP_MODE_FETCH);
	crhttp_request_set_header(req, "Accept", "application/json, text/plain, */*");
	json e = EchoOf(client_, req);
	crhttp_request_free(req);

	EXPECT_EQ(HeaderNames(e), (Names{
		":method", ":authority", ":scheme", ":path",
		"sec-ch-ua-platform", "user-agent", "accept", "sec-ch-ua",
		"sec-ch-ua-mobile", "sec-fetch-site", "sec-fetch-mode",
		"sec-fetch-dest", "accept-encoding", "accept-language",
		"priority"}));
	EXPECT_EQ(EchoHeader(e, "accept"), "application/json, text/plain, */*");
}

TEST_F(ClientTest, FetchPostJsonMatchesChrome)
{
	crhttp_request *req = Req("POST", HttpsUrl("/echo"), CRHTTP_MODE_FETCH);
	crhttp_request_set_header(req, "Content-Type", "application/json");
	crhttp_request_set_header(req, "X-Custom", "1");
	crhttp_request_set_header(req, "Referer", HttpsUrl("/capture.html").c_str());
	crhttp_request_set_body(req, "{\"a\":1}", 7);
	json e = EchoOf(client_, req);
	crhttp_request_free(req);

	EXPECT_EQ(HeaderNames(e), (Names{
		":method", ":authority", ":scheme", ":path",
		"content-length", "x-custom", "sec-ch-ua-platform",
		"user-agent", "sec-ch-ua", "content-type", "sec-ch-ua-mobile",
		"accept", "origin", "sec-fetch-site", "sec-fetch-mode",
		"sec-fetch-dest", "referer", "accept-encoding",
		"accept-language", "priority"}));
	EXPECT_EQ(e["body"], "{\"a\":1}");
}

TEST_F(ClientTest, XhrHeaderPositionMatchesChrome)
{
	crhttp_request *req = Req("GET", HttpsH1Url("/echo"), CRHTTP_MODE_FETCH);
	crhttp_request_set_header(req, "X-Requested-With", "XMLHttpRequest");
	json e = EchoOf(client_, req);
	crhttp_request_free(req);

	EXPECT_EQ(HeaderNames(e), (Names{
		"Host", "Connection", "sec-ch-ua-platform", "X-Requested-With",
		"User-Agent", "sec-ch-ua", "sec-ch-ua-mobile", "Accept",
		"Sec-Fetch-Site", "Sec-Fetch-Mode", "Sec-Fetch-Dest",
		"Accept-Encoding", "Accept-Language"}));
}

TEST_F(ClientTest, FetchCrossSiteAddsOrigin)
{
	// 127.0.0.1 vs localhost: different sites.
	std::string url = "https://127.0.0.1:" + std::to_string(ServerPorts().https) + "/echo";
	crhttp_request *req = Req("GET", url, CRHTTP_MODE_FETCH);
	crhttp_request_set_header(req, "Referer", HttpsUrl("/").c_str());
	json e = EchoOf(client_, req);
	crhttp_request_free(req);

	EXPECT_EQ(EchoHeader(e, "sec-fetch-site"), "cross-site");
	EXPECT_EQ(EchoHeader(e, "origin"),
		  "https://localhost:" + std::to_string(ServerPorts().https));
	Names n = HeaderNames(e);
	auto pos = [&](const char *h) { return std::find(n.begin(), n.end(), h) - n.begin(); };
	EXPECT_LT(pos("accept"), pos("origin"));
	EXPECT_LT(pos("origin"), pos("sec-fetch-site"));
}

TEST_F(ClientTest, SecFetchSiteFollowsRedirectChain)
{
	// Start same-origin, get redirected cross-site: Chrome reports the
	// most distant relation seen along the chain.
	std::string cross = "https://127.0.0.1:" + std::to_string(ServerPorts().https) + "/echo";
	crhttp_request *req = Req("GET", HttpsUrl("/redirect-to?url=" + cross));
	crhttp_request_set_header(req, "Referer", HttpsUrl("/").c_str());
	json e = EchoOf(client_, req);
	crhttp_request_free(req);
	EXPECT_EQ(EchoHeader(e, "sec-fetch-site"), "cross-site");
}

TEST_F(ClientTest, RawModeSendsNoBrowserHeaders)
{
	crhttp_request *req = Req("GET", HttpsUrl("/echo"), CRHTTP_MODE_RAW);
	crhttp_request_set_header(req, "X-One", "1");
	json e = EchoOf(client_, req);
	crhttp_request_free(req);
	for (const char *h : {"sec-ch-ua", "sec-fetch-mode", "upgrade-insecure-requests"})
		EXPECT_FALSE(EchoHasHeader(e, h)) << h;
	EXPECT_EQ(EchoHeader(e, "x-one"), "1");
	// //net still adds these on its own.
	EXPECT_EQ(EchoHeader(e, "user-agent"), crhttp_default_user_agent());
	EXPECT_TRUE(EchoHasHeader(e, "accept-encoding"));
}

TEST_F(ClientTest, OverrideKeepsChromePosition)
{
	crhttp_request *req = Req("GET", HttpsUrl("/echo"));
	crhttp_request_set_header(req, "user-agent", "MyAgent/1.0");
	crhttp_request_set_header(req, "Accept-Language", "id-ID,id;q=0.9");
	json e = EchoOf(client_, req);
	crhttp_request_free(req);
	Names n = HeaderNames(e);
	EXPECT_EQ(n[8], "user-agent");
	EXPECT_EQ(EchoHeader(e, "user-agent"), "MyAgent/1.0");
	EXPECT_EQ(EchoHeader(e, "accept-language"), "id-ID,id;q=0.9");
	EXPECT_EQ(n[n.size() - 2], "accept-language");
}

TEST_F(ClientTest, NullValueSuppressesDefaults)
{
	crhttp_request *req = Req("GET", HttpsUrl("/echo"));
	crhttp_request_set_header(req, "sec-ch-ua", nullptr);
	crhttp_request_set_header(req, "Accept-Language", nullptr);
	crhttp_request_set_header(req, "User-Agent", nullptr);
	json e = EchoOf(client_, req);
	crhttp_request_free(req);
	EXPECT_FALSE(EchoHasHeader(e, "sec-ch-ua"));
	EXPECT_FALSE(EchoHasHeader(e, "accept-language"));
	EXPECT_FALSE(EchoHasHeader(e, "user-agent"));
	EXPECT_TRUE(EchoHasHeader(e, "sec-ch-ua-mobile"));
}

TEST_F(ClientTest, ExplicitSecFetchSiteIsKept)
{
	crhttp_request *req = Req("GET", HttpsUrl("/echo"));
	crhttp_request_set_header(req, "Referer", "https://elsewhere.example/");
	crhttp_request_set_header(req, "Sec-Fetch-Site", "same-site");
	json e = EchoOf(client_, req);
	crhttp_request_free(req);
	EXPECT_EQ(EchoHeader(e, "sec-fetch-site"), "same-site");
}

TEST_F(ClientTest, CustomHeadersArePlaced)
{
	crhttp_request *req = Req("GET", HttpsUrl("/echo"));
	crhttp_request_set_header(req, "X-Trace-Id", "abc");
	json e = EchoOf(client_, req);
	crhttp_request_free(req);
	Names n = HeaderNames(e);
	auto pos = [&](const char *h) { return std::find(n.begin(), n.end(), h) - n.begin(); };
	EXPECT_EQ(EchoHeader(e, "x-trace-id"), "abc");
	EXPECT_LT(pos("sec-fetch-dest"), pos("x-trace-id"));
	EXPECT_LT(pos("x-trace-id"), pos("accept-encoding"));
}

TEST_F(ClientTest, PriorityOverride)
{
	crhttp_request *req = Req("GET", HttpsUrl("/echo"));
	crhttp_request_set_priority(req, CRHTTP_PRIORITY_LOW);
	json e = EchoOf(client_, req);
	crhttp_request_free(req);
	EXPECT_EQ(EchoHeader(e, "priority"), "u=2, i");
}

TEST(Identity, ConfiguredBrowserIdentity)
{
	crhttp_client_config *cfg = crhttp_client_config_new();
	crhttp_client_config_set_user_agent(cfg, "UA/1");
	crhttp_client_config_set_sec_ch_ua(cfg, "\"X\";v=\"1\"");
	crhttp_client_config_set_platform(cfg, "Windows");
	crhttp_client_config_set_accept_language(cfg, "id,en;q=0.5");
	crhttp_client *c = NewTestClient(cfg);
	crhttp_client_config_free(cfg);

	for (crhttp_mode m : {CRHTTP_MODE_NAVIGATE, CRHTTP_MODE_FETCH, CRHTTP_MODE_RAW}) {
		crhttp_request *req = Req("GET", HttpsUrl("/echo"), m);
		json e = EchoOf(c, req);
		crhttp_request_free(req);
		EXPECT_EQ(EchoHeader(e, "user-agent"), "UA/1") << m;
		EXPECT_EQ(EchoHeader(e, "accept-language"), "id,en;q=0.5") << m;
		if (m != CRHTTP_MODE_RAW) {
			EXPECT_EQ(EchoHeader(e, "sec-ch-ua"), "\"X\";v=\"1\"");
			EXPECT_EQ(EchoHeader(e, "sec-ch-ua-platform"), "\"Windows\"");
		}
	}
	crhttp_client_free(c);
}
