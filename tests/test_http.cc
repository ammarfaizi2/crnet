// SPDX-License-Identifier: BSD-3-Clause
//
// Request/response bodies, content decoding, redirects and cookies.
//
#include <string.h>

#include "common.h"

using namespace crtest;

namespace {

std::string Pattern(size_t n)
{
	std::string s(n, '\0');
	for (size_t i = 0; i < n; i++)
		s[i] = static_cast<char>(i & 0xff);
	return s;
}

const std::string kPlain = [] {
	std::string s;
	for (int i = 0; i < 200; i++)
		s += "crhttp content-encoding test payload. ";
	return s;
}();

} // namespace

TEST_F(ClientTest, PostBinaryBody)
{
	std::string body("\x00\x01\x02 binary \xff\x00 end", 15);
	for (const std::string &url : {HttpsUrl("/echo"), HttpsH1Url("/echo")}) {
		crhttp_request *req = crhttp_request_new("POST", url.c_str());
		crhttp_request_set_mode(req, CRHTTP_MODE_RAW);
		crhttp_request_set_header(req, "Content-Type", "application/octet-stream");
		crhttp_request_set_body(req, body.data(), body.size());
		Result r = Perform(client_, req);
		crhttp_request_free(req);
		ASSERT_EQ(r.rc, CRHTTP_OK);
		json e = r.echo();
		EXPECT_EQ(e["body_len"], body.size());
		EXPECT_EQ(EchoHeader(e, "content-length"), std::to_string(body.size()));
	}
}

TEST_F(ClientTest, PutAndDeleteMethods)
{
	for (const char *m : {"PUT", "DELETE", "PATCH", "OPTIONS"}) {
		crhttp_request *req = crhttp_request_new(m, HttpsUrl("/echo").c_str());
		if (strcmp(m, "DELETE"))
			crhttp_request_set_body(req, "x", 1);
		Result r = Perform(client_, req);
		crhttp_request_free(req);
		ASSERT_EQ(r.rc, CRHTTP_OK) << m;
		EXPECT_EQ(r.echo()["method"], m);
	}
}

TEST_F(ClientTest, LargeDownloadIsExact)
{
	const size_t n = 8 * 1024 * 1024 + 123;
	for (const std::string &url : {HttpsUrl("/bytes/" + std::to_string(n)),
				       HttpsH1Url("/bytes/" + std::to_string(n))}) {
		Result r = Get(client_, url);
		ASSERT_EQ(r.rc, CRHTTP_OK);
		std::string b = r.body();
		ASSERT_EQ(b.size(), n);
		EXPECT_TRUE(b == Pattern(n)) << url;
	}
}

TEST_F(ClientTest, LargeUpload)
{
	std::string body = Pattern(3 * 1024 * 1024);
	crhttp_request *req = crhttp_request_new("POST", HttpsUrl("/echo").c_str());
	crhttp_request_set_body(req, body.data(), body.size());
	Result r = Perform(client_, req);
	crhttp_request_free(req);
	ASSERT_EQ(r.rc, CRHTTP_OK);
	EXPECT_EQ(r.echo()["body_len"], body.size());
}

TEST_F(ClientTest, ChunkedStreamingResponse)
{
	Result r = Get(client_, HttpsH1Url("/stream/50000?chunk=1000&delay_ms=1"));
	ASSERT_EQ(r.rc, CRHTTP_OK);
	EXPECT_EQ(r.body(), Pattern(50000));
}

class EncodingTest : public ClientTest,
		     public ::testing::WithParamInterface<const char *> {};

TEST_P(EncodingTest, DecodedTransparently)
{
	Result r = Get(client_, HttpsUrl(std::string("/encoded/") + GetParam()));
	ASSERT_EQ(r.rc, CRHTTP_OK);
	ASSERT_EQ(r.status(), 200);
	EXPECT_EQ(r.header("content-encoding"), GetParam());
	EXPECT_EQ(r.body(), kPlain);
}

TEST_P(EncodingTest, RawWhenDecompressionDisabled)
{
	crhttp_request *req = crhttp_request_new(
		"GET", HttpsUrl(std::string("/encoded/") + GetParam()).c_str());
	crhttp_request_set_decompress(req, 0);
	Result r = Perform(client_, req);
	crhttp_request_free(req);
	ASSERT_EQ(r.rc, CRHTTP_OK);
	std::string b = r.body();
	EXPECT_NE(b, kPlain);
	EXPECT_LT(b.size(), kPlain.size()) << "compressed bytes expected";
	EXPECT_EQ(std::to_string(b.size()), r.header("content-length"));
	if (!strcmp(GetParam(), "gzip")) {
		EXPECT_EQ(b.substr(0, 2), "\x1f\x8b");
	}
	if (!strcmp(GetParam(), "zstd")) {
		EXPECT_EQ(b.substr(0, 4), "\x28\xb5\x2f\xfd");
	}
}

INSTANTIATE_TEST_SUITE_P(All, EncodingTest,
			 ::testing::Values("gzip", "deflate", "br", "zstd"));

TEST_F(ClientTest, MaxBodySize)
{
	crhttp_request *req = crhttp_request_new("GET", HttpsUrl("/bytes/100000").c_str());
	crhttp_request_set_max_body_size(req, 1000);
	Result r = Perform(client_, req);
	EXPECT_EQ(r.rc, CRHTTP_E_TOO_LARGE);
	crhttp_request_set_max_body_size(req, 100000);
	Result ok = Perform(client_, req);
	EXPECT_EQ(ok.rc, CRHTTP_OK);
	crhttp_request_free(req);
}

TEST_F(ClientTest, FollowsRedirects)
{
	Result r = Get(client_, HttpsUrl("/redirect/3"));
	ASSERT_EQ(r.rc, CRHTTP_OK);
	EXPECT_EQ(r.status(), 200);
	EXPECT_EQ(crhttp_response_redirect_count(r.resp), 3);
	EXPECT_EQ(std::string(crhttp_response_url(r.resp)), HttpsUrl("/echo"));
}

TEST_F(ClientTest, NoRedirectReturns3xx)
{
	crhttp_request *req = crhttp_request_new("GET", HttpsUrl("/redirect/2").c_str());
	crhttp_request_set_max_redirects(req, 0);
	Result r = Perform(client_, req);
	crhttp_request_free(req);
	ASSERT_EQ(r.rc, CRHTTP_OK);
	EXPECT_EQ(r.status(), 302);
	EXPECT_EQ(r.header("location"), "/redirect/1");
	EXPECT_EQ(crhttp_response_redirect_count(r.resp), 0);
	EXPECT_EQ(std::string(crhttp_response_url(r.resp)), HttpsUrl("/redirect/2"));
}

TEST_F(ClientTest, RedirectLimitStopsChain)
{
	crhttp_request *req = crhttp_request_new("GET", HttpsUrl("/redirect/5").c_str());
	crhttp_request_set_max_redirects(req, 2);
	Result r = Perform(client_, req);
	crhttp_request_free(req);
	ASSERT_EQ(r.rc, CRHTTP_OK);
	EXPECT_EQ(r.status(), 302);
	EXPECT_EQ(crhttp_response_redirect_count(r.resp), 2);
	EXPECT_EQ(r.header("location"), "/redirect/2");
}

TEST_F(ClientTest, SeeOtherTurnsPostIntoGet)
{
	std::string target = HttpsUrl("/echo");
	crhttp_request *req = crhttp_request_new(
		"POST", HttpsUrl("/redirect-to?code=303&url=" + target).c_str());
	crhttp_request_set_body(req, "payload", 7);
	Result r = Perform(client_, req);
	crhttp_request_free(req);
	ASSERT_EQ(r.rc, CRHTTP_OK);
	json e = r.echo();
	EXPECT_EQ(e["method"], "GET");
	EXPECT_EQ(e["body_len"], 0);
}

TEST_F(ClientTest, TemporaryRedirectKeepsPost)
{
	std::string target = HttpsUrl("/echo");
	crhttp_request *req = crhttp_request_new(
		"POST", HttpsUrl("/redirect-to?code=307&url=" + target).c_str());
	crhttp_request_set_body(req, "payload", 7);
	Result r = Perform(client_, req);
	crhttp_request_free(req);
	ASSERT_EQ(r.rc, CRHTTP_OK);
	json e = r.echo();
	EXPECT_EQ(e["method"], "POST");
	EXPECT_EQ(e["body"], "payload");
}

TEST_F(ClientTest, CookieJarRoundTrip)
{
	Result set = Get(client_, HttpsUrl("/set-cookie?sid=abc&theme=dark"));
	ASSERT_EQ(set.rc, CRHTTP_OK);

	Result r = Get(client_, HttpsUrl("/echo"));
	ASSERT_EQ(r.rc, CRHTTP_OK);
	json e = r.echo();
	EXPECT_EQ(EchoHeader(e, "cookie"), "sid=abc; theme=dark");
	// Chrome puts Cookie after Accept-Language, before priority.
	auto n = HeaderNames(e);
	EXPECT_EQ(n[n.size() - 2], "cookie");

	char *line = crhttp_client_get_cookies(client_, HttpsUrl("/").c_str());
	ASSERT_NE(line, nullptr);
	EXPECT_STREQ(line, "sid=abc; theme=dark");
	crhttp_free(line);

	EXPECT_EQ(crhttp_client_clear_cookies(client_), CRHTTP_OK);
	Result after = Get(client_, HttpsUrl("/echo"));
	EXPECT_FALSE(EchoHasHeader(after.echo(), "cookie"));
}

TEST_F(ClientTest, InjectedCookieIsSent)
{
	ASSERT_EQ(crhttp_client_set_cookie(client_, HttpsUrl("/").c_str(),
					   "cf_clearance=xyz; Path=/; Secure"),
		  CRHTTP_OK);
	Result r = Get(client_, HttpsUrl("/echo"));
	EXPECT_EQ(EchoHeader(r.echo(), "cookie"), "cf_clearance=xyz");
	EXPECT_EQ(crhttp_client_set_cookie(client_, "not a url", "a=b"), CRHTTP_E_INVAL);
}

TEST_F(ClientTest, ExplicitCookieHeaderMergesWithJar)
{
	Get(client_, HttpsUrl("/set-cookie?jar=1&both=jar"));
	crhttp_request *req = crhttp_request_new("GET", HttpsUrl("/echo").c_str());
	crhttp_request_set_header(req, "Cookie", "mine=2; both=mine");
	Result r = Perform(client_, req);
	crhttp_request_free(req);
	EXPECT_EQ(EchoHeader(r.echo(), "cookie"), "mine=2; both=mine; jar=1");
}

TEST_F(ClientTest, ExplicitCookieHeaderWithEmptyJar)
{
	crhttp_request *req = crhttp_request_new("GET", HttpsUrl("/echo").c_str());
	crhttp_request_set_header(req, "Cookie", "only=me");
	Result r = Perform(client_, req);
	crhttp_request_free(req);
	EXPECT_EQ(EchoHeader(r.echo(), "cookie"), "only=me");
}

TEST(Cookies, DisabledJarNeitherStoresNorSends)
{
	crhttp_client_config *cfg = crhttp_client_config_new();
	crhttp_client_config_set_cookies(cfg, 0);
	crhttp_client *c = NewTestClient(cfg);
	crhttp_client_config_free(cfg);

	Get(c, HttpsUrl("/set-cookie?a=1"));
	Result r = Get(c, HttpsUrl("/echo"));
	EXPECT_FALSE(EchoHasHeader(r.echo(), "cookie"));
	EXPECT_EQ(crhttp_client_set_cookie(c, HttpsUrl("/").c_str(), "a=1"), CRHTTP_E_STATE);
	char *line = crhttp_client_get_cookies(c, HttpsUrl("/").c_str());
	ASSERT_NE(line, nullptr);
	EXPECT_STREQ(line, "");
	crhttp_free(line);

	// An explicit header still goes out.
	crhttp_request *req = crhttp_request_new("GET", HttpsUrl("/echo").c_str());
	crhttp_request_set_header(req, "Cookie", "x=y");
	Result r2 = Perform(c, req);
	crhttp_request_free(req);
	EXPECT_EQ(EchoHeader(r2.echo(), "cookie"), "x=y");
	crhttp_client_free(c);
}

TEST(Cookies, ClientsHaveSeparateJars)
{
	crhttp_client *a = NewTestClient();
	crhttp_client *b = NewTestClient();
	Get(a, HttpsUrl("/set-cookie?who=a"));
	Result ra = Get(a, HttpsUrl("/echo"));
	Result rb = Get(b, HttpsUrl("/echo"));
	EXPECT_EQ(EchoHeader(ra.echo(), "cookie"), "who=a");
	EXPECT_FALSE(EchoHasHeader(rb.echo(), "cookie"));
	crhttp_client_free(a);
	crhttp_client_free(b);
}
