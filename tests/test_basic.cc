// SPDX-License-Identifier: BSD-3-Clause
//
// Library metadata, argument validation, protocols, status codes,
// response headers and error reporting.
//
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"

using namespace crtest;

TEST(Library, VersionStrings)
{
	std::string cr = crhttp_chromium_version();
	EXPECT_EQ(cr, CRHTTP_TEST_CHROMIUM_VERSION);
	EXPECT_NE(std::string(crhttp_version()).find(cr), std::string::npos);

	std::string major = cr.substr(0, cr.find('.'));
	std::string ua = crhttp_default_user_agent();
	EXPECT_EQ(ua, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
		      "(KHTML, like Gecko) Chrome/" + major +
			      ".0.0.0 Safari/537.36");
	std::string ch = crhttp_default_sec_ch_ua();
	EXPECT_NE(ch.find("\"Google Chrome\";v=\"" + major + "\""), std::string::npos);
	EXPECT_NE(ch.find("\"Chromium\";v=\"" + major + "\""), std::string::npos);
	EXPECT_NE(ch.find("Brand\";v=\""), std::string::npos);
}

TEST(Library, SecChUaMatchesChrome155)
{
	if (std::string(crhttp_chromium_version()).rfind("155.", 0) != 0)
		GTEST_SKIP() << "value captured from Chrome 155";
	EXPECT_STREQ(crhttp_default_sec_ch_ua(),
		     "\"Google Chrome\";v=\"155\", \"Chromium\";v=\"155\", "
		     "\"Not(A:Brand\";v=\"24\"");
}

TEST(Library, StrError)
{
	for (int e = CRHTTP_OK; e >= CRHTTP_E_CALLBACK; e--)
		EXPECT_STRNE(crhttp_strerror(e), "unknown error") << e;
	EXPECT_STREQ(crhttp_strerror(12345), "unknown error");
}

TEST(Library, GlobalInitAfterClientIsRejected)
{
	crhttp_client *c = NewTestClient();
	ASSERT_NE(c, nullptr);
	EXPECT_EQ(crhttp_global_init(nullptr, nullptr), CRHTTP_E_STATE);
	crhttp_client_free(c);
}

TEST(Validation, RequestArguments)
{
	EXPECT_EQ(crhttp_request_new("GET", "not a url"), nullptr);
	EXPECT_EQ(crhttp_request_new("GET", "ftp://example.com/"), nullptr);
	EXPECT_EQ(crhttp_request_new("GET", nullptr), nullptr);
	EXPECT_EQ(crhttp_request_new("BAD METHOD", "https://example.com/"), nullptr);

	crhttp_request *r = crhttp_request_new(nullptr, "https://example.com/");
	ASSERT_NE(r, nullptr);
	EXPECT_EQ(crhttp_request_set_header(r, "Bad Name", "x"), CRHTTP_E_INVAL);
	EXPECT_EQ(crhttp_request_set_header(r, "X-Ok", "bad\r\nvalue"), CRHTTP_E_INVAL);
	EXPECT_EQ(crhttp_request_set_header(r, "X-Ok", "fine"), CRHTTP_OK);
	EXPECT_EQ(crhttp_request_set_body(r, nullptr, 3), CRHTTP_E_INVAL);
	EXPECT_EQ(crhttp_request_set_mode(r, (crhttp_mode)42), CRHTTP_E_INVAL);
	EXPECT_EQ(crhttp_request_set_priority(r, (crhttp_priority)9), CRHTTP_E_INVAL);
	EXPECT_EQ(crhttp_request_set_max_redirects(r, -1), CRHTTP_E_INVAL);
	EXPECT_EQ(crhttp_request_set_url(r, "javascript:alert(1)"), CRHTTP_E_INVAL);
	crhttp_request_free(r);

	crhttp_response *resp = (crhttp_response *)1;
	EXPECT_EQ(crhttp_perform(nullptr, nullptr, &resp), CRHTTP_E_INVAL);
	EXPECT_EQ(resp, nullptr);
}

TEST(Validation, ProxyStrings)
{
	crhttp_client_config *cfg = crhttp_client_config_new();
	EXPECT_EQ(crhttp_client_config_set_proxy(cfg, "socks5h://1.2.3.4:1080"), CRHTTP_OK);
	EXPECT_EQ(crhttp_client_config_set_proxy(cfg, "socks5://[::1]:1080"), CRHTTP_OK);
	EXPECT_EQ(crhttp_client_config_set_proxy(cfg, "http://u:p@proxy:3128"), CRHTTP_OK);
	EXPECT_EQ(crhttp_client_config_set_proxy(cfg, "https://proxy:443"), CRHTTP_OK);
	EXPECT_EQ(crhttp_client_config_set_proxy(cfg, "direct://"), CRHTTP_OK);
	EXPECT_EQ(crhttp_client_config_set_proxy(cfg, nullptr), CRHTTP_OK);
	EXPECT_EQ(crhttp_client_config_set_proxy(cfg, "ftp://proxy:21"), CRHTTP_E_INVAL);
	EXPECT_EQ(crhttp_client_config_set_proxy(cfg, "proxy:3128"), CRHTTP_E_INVAL);
	EXPECT_EQ(crhttp_client_config_set_proxy(cfg, "socks5://u:p@h:1"), CRHTTP_E_INVAL);
	EXPECT_EQ(crhttp_client_config_set_proxy(cfg, "http://h:1/path"), CRHTTP_E_INVAL);
	crhttp_client_config_free(cfg);
}

TEST(Validation, NullHandlesAreHarmless)
{
	crhttp_client_free(nullptr);
	crhttp_request_free(nullptr);
	crhttp_response_free(nullptr);
	crhttp_transfer_free(nullptr);
	crhttp_transfer_cancel(nullptr);
	crhttp_queue_free(nullptr);
	crhttp_client_config_free(nullptr);
	EXPECT_EQ(crhttp_response_status(nullptr), 0);
	EXPECT_STREQ(crhttp_response_url(nullptr), "");
	EXPECT_EQ(crhttp_response_header(nullptr, "x"), nullptr);
	size_t n = 7;
	EXPECT_EQ(crhttp_response_body(nullptr, &n), nullptr);
	EXPECT_EQ(n, 0u);
}

TEST_F(ClientTest, GetOverHttp2)
{
	Result r = Get(client_, HttpsUrl("/echo"));
	ASSERT_EQ(r.rc, CRHTTP_OK) << crhttp_response_error_string(r.resp);
	EXPECT_EQ(r.status(), 200);
	EXPECT_EQ(r.protocol(), "h2");
	EXPECT_STREQ(crhttp_response_status_text(r.resp), "");
	json e = r.echo();
	EXPECT_EQ(e["method"], "GET");
	EXPECT_EQ(e["protocol"], "h2");
	EXPECT_EQ(r.header("content-type"), "application/json");
	EXPECT_EQ(std::string(crhttp_response_url(r.resp)), HttpsUrl("/echo"));
	EXPECT_NE(std::string(crhttp_response_remote_address(r.resp)).find(
			  std::to_string(ServerPorts().https)),
		  std::string::npos);
	EXPECT_FALSE(crhttp_response_via_proxy(r.resp));
}

TEST_F(ClientTest, GetOverHttp11Tls)
{
	Result r = Get(client_, HttpsH1Url("/echo"));
	ASSERT_EQ(r.rc, CRHTTP_OK);
	EXPECT_EQ(r.protocol(), "http/1.1");
	EXPECT_STREQ(crhttp_response_status_text(r.resp), "OK");
	EXPECT_EQ(r.echo()["protocol"], "http/1.1");
}

TEST_F(ClientTest, GetOverPlainHttp)
{
	Result r = Get(client_, HttpUrl("/echo"));
	ASSERT_EQ(r.rc, CRHTTP_OK);
	EXPECT_EQ(r.protocol(), "http/1.1");
	EXPECT_EQ(r.echo()["tls"], false);
}

TEST(Protocols, Http2CanBeDisabled)
{
	crhttp_client_config *cfg = crhttp_client_config_new();
	crhttp_client_config_set_http2(cfg, 0);
	crhttp_client *c = NewTestClient(cfg);
	crhttp_client_config_free(cfg);
	Result r = Get(c, HttpsUrl("/echo"));
	ASSERT_EQ(r.rc, CRHTTP_OK);
	EXPECT_EQ(r.protocol(), "http/1.1");
	crhttp_client_free(c);
}

TEST(Tls, SelfSignedCertificateIsRejectedByDefault)
{
	ServerPorts();
	crhttp_client *c = crhttp_client_new(nullptr);
	ASSERT_NE(c, nullptr);
	Result r = Get(c, HttpsUrl("/echo"));
	EXPECT_EQ(r.rc, CRHTTP_E_NET);
	ASSERT_NE(r.resp, nullptr);
	EXPECT_EQ(r.net_error(), -202);  // ERR_CERT_AUTHORITY_INVALID
	EXPECT_STREQ(crhttp_response_error_string(r.resp),
		     "net::ERR_CERT_AUTHORITY_INVALID");
	EXPECT_EQ(r.status(), 0);
	crhttp_client_free(c);
}

TEST_F(ClientTest, StatusCodesAreNotErrors)
{
	for (int code : {201, 204, 404, 418, 500, 503}) {
		Result r = Get(client_, HttpsUrl("/status/" + std::to_string(code)));
		ASSERT_EQ(r.rc, CRHTTP_OK) << code;
		EXPECT_EQ(r.status(), code);
	}
}

TEST_F(ClientTest, HeadRequestHasNoBody)
{
	crhttp_request *req = crhttp_request_new("HEAD", HttpsUrl("/bytes/1000").c_str());
	Result r = Perform(client_, req);
	crhttp_request_free(req);
	ASSERT_EQ(r.rc, CRHTTP_OK);
	EXPECT_EQ(r.status(), 200);
	EXPECT_EQ(r.body(), "");
}

TEST_F(ClientTest, RepeatedResponseHeaders)
{
	Result r = Get(client_, HttpsH1Url("/dup-headers"));
	ASSERT_EQ(r.rc, CRHTTP_OK);
	int dups = 0;
	bool mixed = false;
	for (size_t i = 0; i < crhttp_response_header_count(r.resp); i++) {
		const char *k, *v;
		ASSERT_EQ(crhttp_response_header_at(r.resp, i, &k, &v), CRHTTP_OK);
		if (!strcmp(k, "x-dup"))
			dups++;
		if (!strcmp(k, "x-Mixed-Case"))
			mixed = true;
	}
	EXPECT_EQ(dups, 2);
	EXPECT_TRUE(mixed) << "header names keep the server's spelling";
	EXPECT_EQ(r.header("X-DUP"), "one");
	EXPECT_EQ(r.header("x-mixed-case"), "yes");
	EXPECT_EQ(crhttp_response_header(r.resp, "absent"), nullptr);
	EXPECT_EQ(crhttp_response_header_at(r.resp, 9999, nullptr, nullptr), CRHTTP_E_INVAL);
}

TEST_F(ClientTest, ConnectionRefused)
{
	// Grab a free port and close it again (low ports are on Chromium's
	// unsafe-port list and fail with ERR_UNSAFE_PORT instead).
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa = {};
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t len = sizeof(sa);
	ASSERT_EQ(bind(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
	ASSERT_EQ(getsockname(fd, (struct sockaddr *)&sa, &len), 0);
	close(fd);
	Result r = Get(client_, "http://127.0.0.1:" +
					std::to_string(ntohs(sa.sin_port)) + "/");
	EXPECT_EQ(r.rc, CRHTTP_E_NET);
	EXPECT_EQ(r.net_error(), -102);  // ERR_CONNECTION_REFUSED
}

TEST_F(ClientTest, NameNotResolved)
{
	Result r = Get(client_, "https://does-not-exist.invalid/");
	EXPECT_EQ(r.rc, CRHTTP_E_NET);
	EXPECT_EQ(r.net_error(), -105);  // ERR_NAME_NOT_RESOLVED
}

TEST_F(ClientTest, ServerDropsConnection)
{
	Result r = Get(client_, HttpsH1Url("/drop"));
	EXPECT_EQ(r.rc, CRHTTP_E_NET);
	EXPECT_LT(r.net_error(), 0);
	EXPECT_EQ(r.status(), 0);
}

TEST_F(ClientTest, RequestTimeout)
{
	crhttp_request *req = crhttp_request_new("GET", HttpsUrl("/delay/3000").c_str());
	crhttp_request_set_timeout(req, 200);
	Result r = Perform(client_, req);
	crhttp_request_free(req);
	EXPECT_EQ(r.rc, CRHTTP_E_TIMEOUT);
	EXPECT_EQ(r.net_error(), -7);  // ERR_TIMED_OUT
}

TEST(Timeouts, ClientDefaultTimeoutAndPerRequestOverride)
{
	crhttp_client_config *cfg = crhttp_client_config_new();
	crhttp_client_config_set_timeout(cfg, 200);
	crhttp_client *c = NewTestClient(cfg);
	crhttp_client_config_free(cfg);

	Result r = Get(c, HttpsUrl("/delay/2000"));
	EXPECT_EQ(r.rc, CRHTTP_E_TIMEOUT);

	crhttp_request *req = crhttp_request_new("GET", HttpsUrl("/delay/500").c_str());
	crhttp_request_set_timeout(req, 0);  // no timeout for this one
	Result r2 = Perform(c, req);
	crhttp_request_free(req);
	EXPECT_EQ(r2.rc, CRHTTP_OK);
	crhttp_client_free(c);
}

TEST_F(ClientTest, ReuseRequestObject)
{
	crhttp_request *req = crhttp_request_new("GET", HttpsUrl("/echo?a=1").c_str());
	Result r1 = Perform(client_, req);
	crhttp_request_set_url(req, HttpsUrl("/echo?a=2").c_str());
	Result r2 = Perform(client_, req);
	crhttp_request_free(req);
	EXPECT_EQ(r1.echo()["target"], "/echo?a=1");
	EXPECT_EQ(r2.echo()["target"], "/echo?a=2");
}

TEST(HostRules, MapRedirectsResolution)
{
	crhttp_client_config *cfg = crhttp_client_config_new();
	std::string rule = "MAP www.crhttp.example 127.0.0.1:" +
			   std::to_string(ServerPorts().https);
	crhttp_client_config_set_host_rules(cfg, rule.c_str());
	crhttp_client *c = NewTestClient(cfg);
	crhttp_client_config_free(cfg);
	Result r = Get(c, "https://www.crhttp.example/echo");
	ASSERT_EQ(r.rc, CRHTTP_OK) << crhttp_response_error_string(r.resp);
	EXPECT_EQ(EchoHeader(r.echo(), ":authority"), "www.crhttp.example");
	crhttp_client_free(c);
}

TEST(NetLog, WritesJsonFile)
{
	std::string path = ::testing::TempDir() + "crhttp-netlog-" +
			   std::to_string(getpid()) + ".json";
	crhttp_client_config *cfg = crhttp_client_config_new();
	crhttp_client_config_set_netlog(cfg, path.c_str());
	crhttp_client *c = NewTestClient(cfg);
	crhttp_client_config_free(cfg);
	Result r = Get(c, HttpsUrl("/echo"));
	EXPECT_EQ(r.rc, CRHTTP_OK);
	crhttp_client_free(c);

	FILE *f = fopen(path.c_str(), "r");
	ASSERT_NE(f, nullptr);
	std::string data;
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		data.append(buf, n);
	fclose(f);
	unlink(path.c_str());
	json j = json::parse(data);  // throws (fails) on a truncated file
	EXPECT_TRUE(j.contains("events"));
	EXPECT_GT(j["events"].size(), 10u);
}
