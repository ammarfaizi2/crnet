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

json EchoOf(crnet_client *c, crnet_request *req)
{
	Result r = Perform(c, req);
	EXPECT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	if (r.rc != CRNET_OK)
		return json::object({{"headers", json::array()}});
	return r.echo();
}

crnet_request *Req(const char *method, const std::string &url,
		   crnet_mode mode = CRNET_MODE_NAVIGATE)
{
	crnet_request *r = crnet_request_new(method, url.c_str());
	crnet_request_set_mode(r, mode);
	return r;
}

const char kNavAccept[] =
	"text/html,application/xhtml+xml,application/xml;q=0.9,image/jxl,"
	"image/avif,image/webp,image/apng,*/*;q=0.8,"
	"application/signed-exchange;v=b3;q=0.7";

} // namespace

TEST_F(ClientTest, NavigateHttp2MatchesChrome)
{
	crnet_request *req = Req("GET", HttpsUrl("/echo"));
	json e = EchoOf(client_, req);
	crnet_request_free(req);

	EXPECT_EQ(HeaderNames(e), (Names{
		":method", ":authority", ":scheme", ":path",
		"sec-ch-ua", "sec-ch-ua-mobile", "sec-ch-ua-platform",
		"upgrade-insecure-requests", "user-agent", "accept",
		"sec-fetch-site", "sec-fetch-mode", "sec-fetch-user",
		"sec-fetch-dest", "accept-encoding", "accept-language",
		"priority"}));
	EXPECT_EQ(EchoHeader(e, "sec-ch-ua"), crnet_default_sec_ch_ua());
	EXPECT_EQ(EchoHeader(e, "sec-ch-ua-mobile"), "?0");
	EXPECT_EQ(EchoHeader(e, "sec-ch-ua-platform"), "\"Linux\"");
	EXPECT_EQ(EchoHeader(e, "upgrade-insecure-requests"), "1");
	EXPECT_EQ(EchoHeader(e, "user-agent"), crnet_default_user_agent());
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
	crnet_request *req = Req("GET", HttpsH1Url("/echo"));
	json e = EchoOf(client_, req);
	crnet_request_free(req);

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
	crnet_request *req = Req("GET", HttpsUrl("/echo"));
	crnet_request_set_header(req, "Referer", HttpsUrl("/capture.html").c_str());
	json e = EchoOf(client_, req);
	crnet_request_free(req);

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
	crnet_request *req = Req("POST", HttpsUrl("/echo"));
	crnet_request_set_header(req, "Referer", HttpsUrl("/form.html").c_str());
	crnet_request_set_header(req, "Content-Type",
				 "application/x-www-form-urlencoded");
	crnet_request_set_body(req, "a=b", 3);
	json e = EchoOf(client_, req);
	crnet_request_free(req);

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
	crnet_request *req = Req("GET", HttpsUrl("/echo"), CRNET_MODE_FETCH);
	crnet_request_set_header(req, "Referer", HttpsUrl("/app.html").c_str());
	json e = EchoOf(client_, req);
	crnet_request_free(req);

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
	crnet_request *req = Req("GET", HttpsUrl("/echo"), CRNET_MODE_FETCH);
	crnet_request_set_header(req, "Accept", "application/json, text/plain, */*");
	json e = EchoOf(client_, req);
	crnet_request_free(req);

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
	crnet_request *req = Req("POST", HttpsUrl("/echo"), CRNET_MODE_FETCH);
	crnet_request_set_header(req, "Content-Type", "application/json");
	crnet_request_set_header(req, "X-Custom", "1");
	crnet_request_set_header(req, "Referer", HttpsUrl("/capture.html").c_str());
	crnet_request_set_body(req, "{\"a\":1}", 7);
	json e = EchoOf(client_, req);
	crnet_request_free(req);

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
	crnet_request *req = Req("GET", HttpsH1Url("/echo"), CRNET_MODE_FETCH);
	crnet_request_set_header(req, "X-Requested-With", "XMLHttpRequest");
	json e = EchoOf(client_, req);
	crnet_request_free(req);

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
	crnet_request *req = Req("GET", url, CRNET_MODE_FETCH);
	crnet_request_set_header(req, "Referer", HttpsUrl("/").c_str());
	json e = EchoOf(client_, req);
	crnet_request_free(req);

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
	crnet_request *req = Req("GET", HttpsUrl("/redirect-to?url=" + cross));
	crnet_request_set_header(req, "Referer", HttpsUrl("/").c_str());
	json e = EchoOf(client_, req);
	crnet_request_free(req);
	EXPECT_EQ(EchoHeader(e, "sec-fetch-site"), "cross-site");
}

TEST_F(ClientTest, RawModeSendsNoBrowserHeaders)
{
	crnet_request *req = Req("GET", HttpsUrl("/echo"), CRNET_MODE_RAW);
	crnet_request_set_header(req, "X-One", "1");
	json e = EchoOf(client_, req);
	crnet_request_free(req);
	for (const char *h : {"sec-ch-ua", "sec-fetch-mode", "upgrade-insecure-requests"})
		EXPECT_FALSE(EchoHasHeader(e, h)) << h;
	EXPECT_EQ(EchoHeader(e, "x-one"), "1");
	// //net still adds these on its own.
	EXPECT_EQ(EchoHeader(e, "user-agent"), crnet_default_user_agent());
	EXPECT_TRUE(EchoHasHeader(e, "accept-encoding"));
}

TEST_F(ClientTest, OverrideKeepsChromePosition)
{
	crnet_request *req = Req("GET", HttpsUrl("/echo"));
	crnet_request_set_header(req, "user-agent", "MyAgent/1.0");
	crnet_request_set_header(req, "Accept-Language", "id-ID,id;q=0.9");
	json e = EchoOf(client_, req);
	crnet_request_free(req);
	Names n = HeaderNames(e);
	EXPECT_EQ(n[8], "user-agent");
	EXPECT_EQ(EchoHeader(e, "user-agent"), "MyAgent/1.0");
	EXPECT_EQ(EchoHeader(e, "accept-language"), "id-ID,id;q=0.9");
	EXPECT_EQ(n[n.size() - 2], "accept-language");
}

TEST_F(ClientTest, NullValueSuppressesDefaults)
{
	crnet_request *req = Req("GET", HttpsUrl("/echo"));
	crnet_request_set_header(req, "sec-ch-ua", nullptr);
	crnet_request_set_header(req, "Accept-Language", nullptr);
	crnet_request_set_header(req, "User-Agent", nullptr);
	json e = EchoOf(client_, req);
	crnet_request_free(req);
	EXPECT_FALSE(EchoHasHeader(e, "sec-ch-ua"));
	EXPECT_FALSE(EchoHasHeader(e, "accept-language"));
	EXPECT_FALSE(EchoHasHeader(e, "user-agent"));
	EXPECT_TRUE(EchoHasHeader(e, "sec-ch-ua-mobile"));
}

TEST_F(ClientTest, ExplicitSecFetchSiteIsKept)
{
	crnet_request *req = Req("GET", HttpsUrl("/echo"));
	crnet_request_set_header(req, "Referer", "https://elsewhere.example/");
	crnet_request_set_header(req, "Sec-Fetch-Site", "same-site");
	json e = EchoOf(client_, req);
	crnet_request_free(req);
	EXPECT_EQ(EchoHeader(e, "sec-fetch-site"), "same-site");
}

TEST_F(ClientTest, CustomHeadersArePlaced)
{
	crnet_request *req = Req("GET", HttpsUrl("/echo"));
	crnet_request_set_header(req, "X-Trace-Id", "abc");
	json e = EchoOf(client_, req);
	crnet_request_free(req);
	Names n = HeaderNames(e);
	auto pos = [&](const char *h) { return std::find(n.begin(), n.end(), h) - n.begin(); };
	EXPECT_EQ(EchoHeader(e, "x-trace-id"), "abc");
	EXPECT_LT(pos("sec-fetch-dest"), pos("x-trace-id"));
	EXPECT_LT(pos("x-trace-id"), pos("accept-encoding"));
}

TEST_F(ClientTest, PriorityOverride)
{
	crnet_request *req = Req("GET", HttpsUrl("/echo"));
	crnet_request_set_priority(req, CRNET_PRIORITY_LOW);
	json e = EchoOf(client_, req);
	crnet_request_free(req);
	EXPECT_EQ(EchoHeader(e, "priority"), "u=2, i");
}

TEST(Identity, ConfiguredBrowserIdentity)
{
	crnet_client_config *cfg = crnet_client_config_new();
	crnet_client_config_set_user_agent(cfg, "UA/1");
	crnet_client_config_set_sec_ch_ua(cfg, "\"X\";v=\"1\"");
	crnet_client_config_set_platform(cfg, "Windows");
	crnet_client_config_set_accept_language(cfg, "id,en;q=0.5");
	crnet_client *c = NewTestClient(cfg);
	crnet_client_config_free(cfg);

	for (crnet_mode m : {CRNET_MODE_NAVIGATE, CRNET_MODE_FETCH, CRNET_MODE_RAW}) {
		crnet_request *req = Req("GET", HttpsUrl("/echo"), m);
		json e = EchoOf(c, req);
		crnet_request_free(req);
		EXPECT_EQ(EchoHeader(e, "user-agent"), "UA/1") << m;
		EXPECT_EQ(EchoHeader(e, "accept-language"), "id,en;q=0.5") << m;
		if (m != CRNET_MODE_RAW) {
			EXPECT_EQ(EchoHeader(e, "sec-ch-ua"), "\"X\";v=\"1\"");
			EXPECT_EQ(EchoHeader(e, "sec-ch-ua-platform"), "\"Windows\"");
		}
	}
	crnet_client_free(c);
}

TEST(Identity, PlatformProfiles)
{
	// The reduced platform tokens of
	// components/embedder_support/user_agent_utils.cc.
	const struct {
		crnet_platform_profile p;
		const char *ua_platform;
		const char *ch_platform;
	} cases[] = {
		{CRNET_PLATFORM_LINUX, "X11; Linux x86_64", "\"Linux\""},
		{CRNET_PLATFORM_WINDOWS, "Windows NT 10.0; Win64; x64",
		 "\"Windows\""},
		{CRNET_PLATFORM_MACOS, "Macintosh; Intel Mac OS X 10_15_7",
		 "\"macOS\""},
	};
	// "155.0.8059.39" reduces to "155.0.0.0".
	const std::string full = crnet_chromium_version();
	const std::string product = "Chrome/" +
		full.substr(0, full.find('.')) + ".0.0.0 Safari/537.36";
	EXPECT_STREQ(crnet_platform_user_agent(CRNET_PLATFORM_LINUX),
		     crnet_default_user_agent());

	for (const auto &tc : cases) {
		const std::string ua = std::string("Mozilla/5.0 (") +
			tc.ua_platform +
			") AppleWebKit/537.36 (KHTML, like Gecko) " + product;
		ASSERT_NE(crnet_platform_user_agent(tc.p), nullptr);
		EXPECT_EQ(crnet_platform_user_agent(tc.p), ua);

		crnet_client_config *cfg = crnet_client_config_new();
		ASSERT_EQ(crnet_client_config_set_platform_profile(cfg, tc.p),
			  CRNET_OK);
		crnet_client *c = NewTestClient(cfg);
		crnet_client_config_free(cfg);
		for (crnet_mode m : {CRNET_MODE_NAVIGATE, CRNET_MODE_FETCH,
				     CRNET_MODE_RAW}) {
			crnet_request *req = Req("GET", HttpsUrl("/echo"), m);
			json e = EchoOf(c, req);
			crnet_request_free(req);
			EXPECT_EQ(EchoHeader(e, "user-agent"), ua) << m;
			if (m == CRNET_MODE_RAW)
				continue;
			EXPECT_EQ(EchoHeader(e, "sec-ch-ua-platform"),
				  tc.ch_platform) << m;
			EXPECT_EQ(EchoHeader(e, "sec-ch-ua-mobile"), "?0");
			EXPECT_EQ(EchoHeader(e, "sec-ch-ua"),
				  crnet_default_sec_ch_ua());
		}
		crnet_client_free(c);
	}
}

TEST(Identity, ExplicitValuesWinOverProfile)
{
	crnet_client_config *cfg = crnet_client_config_new();
	EXPECT_EQ(crnet_client_config_set_platform_profile(
			  cfg, static_cast<crnet_platform_profile>(3)),
		  CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_set_platform_profile(
			  cfg, static_cast<crnet_platform_profile>(-1)),
		  CRNET_E_INVAL);
	EXPECT_EQ(crnet_platform_user_agent(
			  static_cast<crnet_platform_profile>(3)),
		  nullptr);

	// Explicit values set before the profile still win.
	crnet_client_config_set_user_agent(cfg, "UA/1");
	crnet_client_config_set_platform(cfg, "Plan 9");
	crnet_client_config_set_platform_profile(cfg, CRNET_PLATFORM_MACOS);
	crnet_client *c = NewTestClient(cfg);
	crnet_request *req = Req("GET", HttpsUrl("/echo"));
	json e = EchoOf(c, req);
	crnet_request_free(req);
	crnet_client_free(c);
	EXPECT_EQ(EchoHeader(e, "user-agent"), "UA/1");
	EXPECT_EQ(EchoHeader(e, "sec-ch-ua-platform"), "\"Plan 9\"");

	// NULL brings back the profile's values.
	crnet_client_config_set_user_agent(cfg, nullptr);
	crnet_client_config_set_platform(cfg, nullptr);
	c = NewTestClient(cfg);
	crnet_client_config_free(cfg);
	req = Req("GET", HttpsUrl("/echo"));
	e = EchoOf(c, req);
	crnet_request_free(req);
	crnet_client_free(c);
	EXPECT_EQ(EchoHeader(e, "user-agent"),
		  crnet_platform_user_agent(CRNET_PLATFORM_MACOS));
	EXPECT_EQ(EchoHeader(e, "sec-ch-ua-platform"), "\"macOS\"");
}

TEST_F(ClientTest, RawModeHonoursRemovalAndCookieMerge)
{
	crnet_client_set_cookie(client_, HttpsUrl("/").c_str(), "jar=1");
	crnet_request *req = Req("GET", HttpsUrl("/echo"), CRNET_MODE_RAW);
	crnet_request_set_header(req, "User-Agent", nullptr);
	crnet_request_set_header(req, "Accept-Language", nullptr);
	crnet_request_set_header(req, "Cookie", "mine=2");
	json e = EchoOf(client_, req);
	crnet_request_free(req);
	EXPECT_FALSE(EchoHasHeader(e, "user-agent"));
	EXPECT_FALSE(EchoHasHeader(e, "accept-language"));
	EXPECT_EQ(EchoHeader(e, "cookie"), "mine=2; jar=1");
}
