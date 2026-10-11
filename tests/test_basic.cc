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
	std::string cr = crnet_chromium_version();
	EXPECT_EQ(cr, CRNET_TEST_CHROMIUM_VERSION);
	EXPECT_NE(std::string(crnet_version()).find(cr), std::string::npos);

	// The library loaded is the one these headers describe.
	std::string ver = std::to_string(CRNET_VERSION_MAJOR) + "." +
			  std::to_string(CRNET_VERSION_MINOR) + "." +
			  std::to_string(CRNET_VERSION_PATCH) + " ";
	EXPECT_EQ(std::string(crnet_version()).rfind(ver, 0), 0u)
		<< crnet_version();
	EXPECT_EQ(CRNET_VERSION_NUMBER,
		  CRNET_VERSION_MAJOR * 10000 + CRNET_VERSION_MINOR * 100 +
			  CRNET_VERSION_PATCH);

	std::string major = cr.substr(0, cr.find('.'));
	std::string ua = crnet_default_user_agent();
	EXPECT_EQ(ua, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
		      "(KHTML, like Gecko) Chrome/" + major +
			      ".0.0.0 Safari/537.36");
	std::string ch = crnet_default_sec_ch_ua();
	EXPECT_NE(ch.find("\"Google Chrome\";v=\"" + major + "\""), std::string::npos);
	EXPECT_NE(ch.find("\"Chromium\";v=\"" + major + "\""), std::string::npos);
	EXPECT_NE(ch.find("Brand\";v=\""), std::string::npos);
}

TEST(Library, SecChUaMatchesChrome155)
{
	if (std::string(crnet_chromium_version()).rfind("155.", 0) != 0)
		GTEST_SKIP() << "value captured from Chrome 155";
	EXPECT_STREQ(crnet_default_sec_ch_ua(),
		     "\"Google Chrome\";v=\"155\", \"Chromium\";v=\"155\", "
		     "\"Not(A:Brand\";v=\"24\"");
}

TEST(Library, StrError)
{
	for (int e = CRNET_OK; e >= CRNET_E_CALLBACK; e--)
		EXPECT_STRNE(crnet_strerror(e), "unknown error") << e;
	EXPECT_STREQ(crnet_strerror(12345), "unknown error");
}

TEST(Library, GlobalInitAfterClientIsRejected)
{
	crnet_client *c = NewTestClient();
	ASSERT_NE(c, nullptr);
	EXPECT_EQ(crnet_global_init(nullptr, nullptr), CRNET_E_STATE);
	crnet_client_free(c);
}

TEST(Validation, RequestArguments)
{
	EXPECT_EQ(crnet_request_new("GET", "not a url"), nullptr);
	EXPECT_EQ(crnet_request_new("GET", "ftp://example.com/"), nullptr);
	EXPECT_EQ(crnet_request_new("GET", nullptr), nullptr);
	EXPECT_EQ(crnet_request_new("BAD METHOD", "https://example.com/"), nullptr);

	crnet_request *r = crnet_request_new(nullptr, "https://example.com/");
	ASSERT_NE(r, nullptr);
	EXPECT_EQ(crnet_request_set_header(r, "Bad Name", "x"), CRNET_E_INVAL);
	EXPECT_EQ(crnet_request_set_header(r, "X-Ok", "bad\r\nvalue"), CRNET_E_INVAL);
	EXPECT_EQ(crnet_request_set_header(r, "X-Ok", "fine"), CRNET_OK);
	EXPECT_EQ(crnet_request_set_body(r, nullptr, 3), CRNET_E_INVAL);
	EXPECT_EQ(crnet_request_set_mode(r, (crnet_mode)42), CRNET_E_INVAL);
	EXPECT_EQ(crnet_request_set_priority(r, (crnet_priority)9), CRNET_E_INVAL);
	EXPECT_EQ(crnet_request_set_max_redirects(r, -1), CRNET_E_INVAL);
	EXPECT_EQ(crnet_request_set_url(r, "javascript:alert(1)"), CRNET_E_INVAL);
	crnet_request_free(r);

	crnet_response *resp = (crnet_response *)1;
	EXPECT_EQ(crnet_perform(nullptr, nullptr, &resp), CRNET_E_INVAL);
	EXPECT_EQ(resp, nullptr);
}

TEST(Validation, ProxyStrings)
{
	crnet_client_config *cfg = crnet_client_config_new();
	EXPECT_EQ(crnet_client_config_set_proxy(cfg, "socks5h://1.2.3.4:1080"), CRNET_OK);
	EXPECT_EQ(crnet_client_config_set_proxy(cfg, "socks5://[::1]:1080"), CRNET_OK);
	EXPECT_EQ(crnet_client_config_set_proxy(cfg, "http://u:p@proxy:3128"), CRNET_OK);
	EXPECT_EQ(crnet_client_config_set_proxy(cfg, "https://proxy:443"), CRNET_OK);
	EXPECT_EQ(crnet_client_config_set_proxy(cfg, "direct://"), CRNET_OK);
	EXPECT_EQ(crnet_client_config_set_proxy(cfg, nullptr), CRNET_OK);
	EXPECT_EQ(crnet_client_config_set_proxy(cfg, "ftp://proxy:21"), CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_set_proxy(cfg, "proxy:3128"), CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_set_proxy(cfg, "socks5://u:p@h:1"), CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_set_proxy(cfg, "http://h:1/path"), CRNET_E_INVAL);
	crnet_client_config_free(cfg);
}

TEST(Validation, NullHandlesAreHarmless)
{
	crnet_client_free(nullptr);
	crnet_request_free(nullptr);
	crnet_response_free(nullptr);
	crnet_transfer_free(nullptr);
	crnet_transfer_cancel(nullptr);
	crnet_queue_free(nullptr);
	crnet_client_config_free(nullptr);
	EXPECT_EQ(crnet_response_status(nullptr), 0);
	EXPECT_STREQ(crnet_response_url(nullptr), "");
	EXPECT_EQ(crnet_response_header(nullptr, "x"), nullptr);
	size_t n = 7;
	EXPECT_EQ(crnet_response_body(nullptr, &n), nullptr);
	EXPECT_EQ(n, 0u);
}

TEST_F(ClientTest, GetOverHttp2)
{
	Result r = Get(client_, HttpsUrl("/echo"));
	ASSERT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	EXPECT_EQ(r.status(), 200);
	EXPECT_EQ(r.protocol(), "h2");
	EXPECT_STREQ(crnet_response_status_text(r.resp), "");
	json e = r.echo();
	EXPECT_EQ(e["method"], "GET");
	EXPECT_EQ(e["protocol"], "h2");
	EXPECT_EQ(r.header("content-type"), "application/json");
	EXPECT_EQ(std::string(crnet_response_url(r.resp)), HttpsUrl("/echo"));
	EXPECT_NE(std::string(crnet_response_remote_address(r.resp)).find(
			  std::to_string(ServerPorts().https)),
		  std::string::npos);
	EXPECT_FALSE(crnet_response_via_proxy(r.resp));
}

TEST_F(ClientTest, GetOverHttp11Tls)
{
	Result r = Get(client_, HttpsH1Url("/echo"));
	ASSERT_EQ(r.rc, CRNET_OK);
	EXPECT_EQ(r.protocol(), "http/1.1");
	EXPECT_STREQ(crnet_response_status_text(r.resp), "OK");
	EXPECT_EQ(r.echo()["protocol"], "http/1.1");
}

TEST_F(ClientTest, GetOverPlainHttp)
{
	Result r = Get(client_, HttpUrl("/echo"));
	ASSERT_EQ(r.rc, CRNET_OK);
	EXPECT_EQ(r.protocol(), "http/1.1");
	EXPECT_EQ(r.echo()["tls"], false);
}

TEST(Protocols, Http2CanBeDisabled)
{
	crnet_client_config *cfg = crnet_client_config_new();
	crnet_client_config_set_http2(cfg, 0);
	crnet_client *c = NewTestClient(cfg);
	crnet_client_config_free(cfg);
	Result r = Get(c, HttpsUrl("/echo"));
	ASSERT_EQ(r.rc, CRNET_OK);
	EXPECT_EQ(r.protocol(), "http/1.1");
	crnet_client_free(c);
}

TEST(Tls, UnknownCaIsRejectedByDefault)
{
	ServerPorts();
	crnet_client *c = crnet_client_new(nullptr);
	ASSERT_NE(c, nullptr);
	Result r = Get(c, HttpsUrl("/echo"));
	EXPECT_EQ(r.rc, CRNET_E_NET);
	ASSERT_NE(r.resp, nullptr);
	EXPECT_EQ(r.net_error(), -202);  // ERR_CERT_AUTHORITY_INVALID
	EXPECT_STREQ(crnet_response_error_string(r.resp),
		     "net::ERR_CERT_AUTHORITY_INVALID");
	EXPECT_EQ(r.status(), 0);
	crnet_client_free(c);
}

TEST_F(ClientTest, StatusCodesAreNotErrors)
{
	for (int code : {201, 204, 404, 418, 500, 503}) {
		Result r = Get(client_, HttpsUrl("/status/" + std::to_string(code)));
		ASSERT_EQ(r.rc, CRNET_OK) << code;
		EXPECT_EQ(r.status(), code);
	}
}

TEST_F(ClientTest, HeadRequestHasNoBody)
{
	crnet_request *req = crnet_request_new("HEAD", HttpsUrl("/bytes/1000").c_str());
	Result r = Perform(client_, req);
	crnet_request_free(req);
	ASSERT_EQ(r.rc, CRNET_OK);
	EXPECT_EQ(r.status(), 200);
	EXPECT_EQ(r.body(), "");
}

TEST_F(ClientTest, RepeatedResponseHeaders)
{
	Result r = Get(client_, HttpsH1Url("/dup-headers"));
	ASSERT_EQ(r.rc, CRNET_OK);
	int dups = 0;
	bool mixed = false;
	for (size_t i = 0; i < crnet_response_header_count(r.resp); i++) {
		const char *k, *v;
		ASSERT_EQ(crnet_response_header_at(r.resp, i, &k, &v), CRNET_OK);
		if (!strcmp(k, "x-dup"))
			dups++;
		if (!strcmp(k, "x-Mixed-Case"))
			mixed = true;
	}
	EXPECT_EQ(dups, 2);
	EXPECT_TRUE(mixed) << "header names keep the server's spelling";
	EXPECT_EQ(r.header("X-DUP"), "one");
	EXPECT_EQ(r.header("x-mixed-case"), "yes");
	EXPECT_EQ(crnet_response_header(r.resp, "absent"), nullptr);
	EXPECT_EQ(crnet_response_header_at(r.resp, 9999, nullptr, nullptr), CRNET_E_INVAL);
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
	EXPECT_EQ(r.rc, CRNET_E_NET);
	EXPECT_EQ(r.net_error(), -102);  // ERR_CONNECTION_REFUSED
}

TEST_F(ClientTest, NameNotResolved)
{
	Result r = Get(client_, "https://does-not-exist.invalid/");
	EXPECT_EQ(r.rc, CRNET_E_NET);
	EXPECT_EQ(r.net_error(), -105);  // ERR_NAME_NOT_RESOLVED
}

TEST_F(ClientTest, ServerDropsConnection)
{
	Result r = Get(client_, HttpsH1Url("/drop"));
	EXPECT_EQ(r.rc, CRNET_E_NET);
	EXPECT_LT(r.net_error(), 0);
	EXPECT_EQ(r.status(), 0);
}

TEST_F(ClientTest, RequestTimeout)
{
	crnet_request *req = crnet_request_new("GET", HttpsUrl("/delay/3000").c_str());
	crnet_request_set_timeout(req, 200);
	Result r = Perform(client_, req);
	crnet_request_free(req);
	EXPECT_EQ(r.rc, CRNET_E_TIMEOUT);
	EXPECT_EQ(r.net_error(), -7);  // ERR_TIMED_OUT
}

TEST(Timeouts, ClientDefaultTimeoutAndPerRequestOverride)
{
	crnet_client_config *cfg = crnet_client_config_new();
	crnet_client_config_set_timeout(cfg, 200);
	crnet_client *c = NewTestClient(cfg);
	crnet_client_config_free(cfg);

	Result r = Get(c, HttpsUrl("/delay/2000"));
	EXPECT_EQ(r.rc, CRNET_E_TIMEOUT);

	crnet_request *req = crnet_request_new("GET", HttpsUrl("/delay/500").c_str());
	crnet_request_set_timeout(req, 0);  // no timeout for this one
	Result r2 = Perform(c, req);
	crnet_request_free(req);
	EXPECT_EQ(r2.rc, CRNET_OK);
	crnet_client_free(c);
}

TEST_F(ClientTest, ReuseRequestObject)
{
	crnet_request *req = crnet_request_new("GET", HttpsUrl("/echo?a=1").c_str());
	Result r1 = Perform(client_, req);
	crnet_request_set_url(req, HttpsUrl("/echo?a=2").c_str());
	Result r2 = Perform(client_, req);
	crnet_request_free(req);
	EXPECT_EQ(r1.echo()["target"], "/echo?a=1");
	EXPECT_EQ(r2.echo()["target"], "/echo?a=2");
}

TEST(HostRules, MapRedirectsResolution)
{
	crnet_client_config *cfg = crnet_client_config_new();
	std::string rule = "MAP www.crnet.example 127.0.0.1:" +
			   std::to_string(ServerPorts().https);
	crnet_client_config_set_host_rules(cfg, rule.c_str());
	crnet_client *c = NewTestClient(cfg);
	crnet_client_config_free(cfg);
	Result r = Get(c, "https://www.crnet.example/echo");
	ASSERT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	EXPECT_EQ(EchoHeader(r.echo(), ":authority"), "www.crnet.example");
	crnet_client_free(c);
}

TEST(NetLog, WritesJsonFile)
{
	std::string path = ::testing::TempDir() + "crnet-netlog-" +
			   std::to_string(getpid()) + ".json";
	crnet_client_config *cfg = crnet_client_config_new();
	crnet_client_config_set_netlog(cfg, path.c_str());
	crnet_client *c = NewTestClient(cfg);
	crnet_client_config_free(cfg);
	Result r = Get(c, HttpsUrl("/echo"));
	EXPECT_EQ(r.rc, CRNET_OK);
	crnet_client_free(c);

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
