// SPDX-License-Identifier: BSD-3-Clause
//
// Saving and restoring the cookie jar.
//
#include <time.h>

#include "common.h"

using namespace crtest;

namespace {

std::string Export(crnet_client *c, crnet_cookie_format f)
{
	char *s = crnet_client_export_cookies(c, f);
	EXPECT_NE(s, nullptr);
	std::string out = s ? s : "";
	crnet_free(s);
	return out;
}

std::string CookieLine(crnet_client *c, const std::string &url)
{
	char *s = crnet_client_get_cookies(c, url.c_str());
	std::string out = s ? s : "";
	crnet_free(s);
	return out;
}

// A jar with a mix of attributes.
crnet_client *FilledClient()
{
	crnet_client *c = NewTestClient();
	EXPECT_EQ(crnet_client_set_cookie(c, "https://localhost/",
					  "host=1; Path=/; Max-Age=3600; Secure; HttpOnly; "
					  "SameSite=Strict; Priority=High"),
		  CRNET_OK);
	EXPECT_EQ(crnet_client_set_cookie(c, "https://www.example.com/",
					  "dom=2; Domain=example.com; Path=/; Max-Age=7200"),
		  CRNET_OK);
	EXPECT_EQ(crnet_client_set_cookie(c, "http://localhost/app/x", "sess=3; Path=/app"),
		  CRNET_OK);
	return c;
}

const char *kUrls[] = {"https://localhost/", "http://localhost/app/y",
		       "https://api.example.com/", "http://www.example.com/"};

} // namespace

TEST(Cookies, JsonRoundTripIsLossless)
{
	crnet_client *a = FilledClient();
	std::string saved = Export(a, CRNET_COOKIES_JSON);
	json doc = json::parse(saved);
	ASSERT_EQ(doc.size(), 3u) << saved;

	crnet_client *b = NewTestClient();
	size_t n = 0;
	ASSERT_EQ(crnet_client_import_cookies(b, saved.c_str(), CRNET_COOKIES_JSON, &n), CRNET_OK);
	EXPECT_EQ(n, 3u);
	// Every attribute, times included, survives.
	EXPECT_EQ(json::parse(Export(b, CRNET_COOKIES_JSON)), doc);
	for (const char *u : kUrls)
		EXPECT_EQ(CookieLine(b, u), CookieLine(a, u)) << u;

	bool saw_strict = false;
	for (const json &c : doc) {
		if (c["name"] == "host") {
			saw_strict = true;
			EXPECT_EQ(c["same_site"], "strict");
			EXPECT_EQ(c["priority"], "high");
			EXPECT_EQ(c["http_only"], true);
			EXPECT_EQ(c["secure"], true);
			EXPECT_TRUE(c["partition"].is_null());
		}
		if (c["name"] == "sess") {
			EXPECT_TRUE(c["expires"].is_null()) << "session cookie";
		}
	}
	EXPECT_TRUE(saw_strict);
	crnet_client_free(a);
	crnet_client_free(b);
}

TEST(Cookies, PartitionedCookieRoundTrip)
{
	const double now_us = static_cast<double>(time(nullptr)) * 1e6;
	json doc = json::array({{
		{"name", "chips"}, {"value", "v"}, {"domain", "localhost"}, {"path", "/"},
		{"secure", true}, {"http_only", false}, {"same_site", "none"},
		{"priority", "medium"}, {"expires", now_us + 3600e6},
		{"creation", now_us}, {"last_access", now_us}, {"last_update", now_us},
		{"source_scheme", "secure"}, {"source_port", 443},
		{"partition", {{"top_level_site", "https://example.com"},
			       {"cross_site_ancestor", false}}},
	}});
	crnet_client *c = NewTestClient();
	size_t n = 0;
	ASSERT_EQ(crnet_client_import_cookies(c, doc.dump().c_str(), CRNET_COOKIES_JSON, &n),
		  CRNET_OK);
	EXPECT_EQ(n, 1u);
	EXPECT_EQ(json::parse(Export(c, CRNET_COOKIES_JSON)), doc);
	// Not representable in cookies.txt.
	EXPECT_EQ(Export(c, CRNET_COOKIES_NETSCAPE).find("chips"), std::string::npos);
	crnet_client_free(c);
}

TEST(Cookies, NetscapeRoundTrip)
{
	crnet_client *a = FilledClient();
	std::string saved = Export(a, CRNET_COOKIES_NETSCAPE);
	EXPECT_EQ(saved.rfind("# Netscape HTTP Cookie File\n", 0), 0u);
	EXPECT_NE(saved.find("#HttpOnly_localhost\tFALSE\t/\tTRUE\t"), std::string::npos) << saved;
	EXPECT_NE(saved.find(".example.com\tTRUE\t/\tFALSE\t"), std::string::npos) << saved;
	EXPECT_NE(saved.find("localhost\tFALSE\t/app\tFALSE\t0\tsess\t3\n"), std::string::npos)
		<< saved;

	crnet_client *b = NewTestClient();
	size_t n = 0;
	ASSERT_EQ(crnet_client_import_cookies(b, saved.c_str(), CRNET_COOKIES_NETSCAPE, &n),
		  CRNET_OK);
	EXPECT_EQ(n, 3u);
	for (const char *u : kUrls)
		EXPECT_EQ(CookieLine(b, u), CookieLine(a, u)) << u;
	crnet_client_free(a);
	crnet_client_free(b);
}

// As written by curl: comments, CRLF, an expired entry.
TEST(Cookies, ImportCurlCookieFile)
{
	std::string file =
		"# Netscape HTTP Cookie File\r\n"
		"# https://curl.se/docs/http-cookies.html\r\n"
		"\r\n"
		".example.com\tTRUE\t/\tFALSE\t0\tsid\tabc\r\n"
		"#HttpOnly_localhost\tFALSE\t/\tTRUE\t" +
		std::to_string(time(nullptr) + 3600) + "\ttok\txyz\r\n"
		"localhost\tFALSE\t/\tFALSE\t1\told\texpired\r\n";
	crnet_client *c = NewTestClient();
	size_t n = 0;
	ASSERT_EQ(crnet_client_import_cookies(c, file.c_str(), CRNET_COOKIES_NETSCAPE, &n),
		  CRNET_OK);
	EXPECT_EQ(n, 2u);
	EXPECT_EQ(CookieLine(c, "https://localhost/"), "tok=xyz");
	EXPECT_EQ(CookieLine(c, "http://www.example.com/"), "sid=abc");
	crnet_client_free(c);
}

TEST(Cookies, ImportedCookiesAreSent)
{
	crnet_client *c = NewTestClient();
	std::string file = "localhost\tFALSE\t/\tFALSE\t0\tfrom_file\tyes\n";
	ASSERT_EQ(crnet_client_import_cookies(c, file.c_str(), CRNET_COOKIES_NETSCAPE, nullptr),
		  CRNET_OK);
	Result r = Get(c, HttpsUrl("/echo"));
	ASSERT_EQ(r.rc, CRNET_OK);
	EXPECT_EQ(EchoHeader(r.echo(), "cookie"), "from_file=yes");
	crnet_client_free(c);
}

TEST(Cookies, MalformedImportStoresNothing)
{
	const struct {
		crnet_cookie_format f;
		const char *data;
	} bad[] = {
		{CRNET_COOKIES_JSON, "not json"},
		{CRNET_COOKIES_JSON, "{}"},
		{CRNET_COOKIES_JSON, "[{\"name\": \"x\"}]"},
		{CRNET_COOKIES_NETSCAPE, "localhost\tFALSE\t/\tFALSE\t0\tok\t1\n"
					 "localhost\tFALSE\t/\n"},
		{CRNET_COOKIES_NETSCAPE, "localhost\tMAYBE\t/\tFALSE\t0\ta\t1\n"},
		{CRNET_COOKIES_NETSCAPE, "localhost\tFALSE\t/\tFALSE\t0\tbad;name\t1\n"},
	};
	crnet_client *c = NewTestClient();
	for (const auto &b : bad) {
		size_t n = 7;
		EXPECT_EQ(crnet_client_import_cookies(c, b.data, b.f, &n), CRNET_E_INVAL) << b.data;
		EXPECT_EQ(n, 0u);
	}
	EXPECT_EQ(json::parse(Export(c, CRNET_COOKIES_JSON)).size(), 0u);
	EXPECT_EQ(crnet_client_import_cookies(c, "", static_cast<crnet_cookie_format>(9), nullptr),
		  CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_export_cookies(c, static_cast<crnet_cookie_format>(9)), nullptr);
	crnet_client_free(c);
}

// Set-Cookie would never store a cookie for a public suffix; an import
// must not either.
TEST(Cookies, PublicSuffixCookieIsRejected)
{
	std::string file = "localhost\tFALSE\t/\tFALSE\t0\tgood\t1\n"
			   ".com\tTRUE\t/\tFALSE\t0\tsuper\t1\n";
	crnet_client *c = NewTestClient();
	size_t n = 7;
	EXPECT_EQ(crnet_client_import_cookies(c, file.c_str(), CRNET_COOKIES_NETSCAPE, &n),
		  CRNET_E_INVAL);
	EXPECT_EQ(n, 0u);
	EXPECT_EQ(CookieLine(c, "http://localhost/"), "");
	EXPECT_EQ(CookieLine(c, "http://example.com/"), "");
	crnet_client_free(c);
}

// Well-formed, but refused when set: a non-secure cookie may not replace
// a secure one. The rest is kept and the caller is told.
TEST(Cookies, RefusedCookieIsReported)
{
	crnet_client *c = NewTestClient();
	ASSERT_EQ(crnet_client_set_cookie(c, "https://www.example.com/", "x=secure; Secure; Path=/"),
		  CRNET_OK);
	std::string file = "www.example.com\tFALSE\t/\tFALSE\t0\tgood\t1\n"
			   "www.example.com\tFALSE\t/\tFALSE\t0\tx\tplain\n";
	size_t n = 0;
	EXPECT_EQ(crnet_client_import_cookies(c, file.c_str(), CRNET_COOKIES_NETSCAPE, &n),
		  CRNET_E_INVAL);
	EXPECT_EQ(n, 1u);
	EXPECT_EQ(CookieLine(c, "https://www.example.com/"), "x=secure; good=1");
	crnet_client_free(c);
}

TEST(Cookies, DisabledJar)
{
	crnet_client_config *cfg = crnet_client_config_new();
	crnet_client_config_set_cookies(cfg, 0);
	crnet_client *c = NewTestClient(cfg);
	crnet_client_config_free(cfg);
	EXPECT_EQ(Export(c, CRNET_COOKIES_JSON), "[]\n");
	EXPECT_EQ(crnet_client_import_cookies(c, "[]", CRNET_COOKIES_JSON, nullptr), CRNET_E_STATE);
	crnet_client_free(c);
}
