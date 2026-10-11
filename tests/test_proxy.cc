// SPDX-License-Identifier: BSD-3-Clause
//
// Proxies. The test server's proxies resolve "*.crnet-test" to
// 127.0.0.1 themselves; these names do not exist in DNS, so a success
// proves the name was resolved on the proxy side (socks5h semantics).
//
#include "common.h"

using namespace crtest;

namespace {

crnet_client *ProxyClient(const std::string &proxy, const char *bypass = nullptr)
{
	crnet_client_config *cfg = crnet_client_config_new();
	EXPECT_EQ(crnet_client_config_set_proxy(cfg, proxy.c_str()), CRNET_OK);
	EXPECT_EQ(crnet_client_config_set_proxy_bypass(cfg, bypass), CRNET_OK);
	crnet_client *c = NewTestClient(cfg);
	crnet_client_config_free(cfg);
	return c;
}

std::string RemoteOnlyUrl(int port, const std::string &path)
{
	return "https://www.crnet-test:" + std::to_string(port) + path;
}

int SocksConnects()
{
	crnet_client *c = NewTestClient();
	Result r = Get(c, HttpsUrl("/echo"));
	crnet_client_free(c);
	return r.echo()["stats"]["socks5_connects"];
}

} // namespace

TEST(Proxy, Socks5hResolvesRemotely)
{
	const Ports &p = ServerPorts();
	int before = SocksConnects();
	crnet_client *c = ProxyClient("socks5h://127.0.0.1:" + std::to_string(p.socks5));
	Result r = Get(c, RemoteOnlyUrl(p.https, "/echo"));
	ASSERT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	EXPECT_EQ(r.status(), 200);
	EXPECT_EQ(r.protocol(), "h2");
	EXPECT_TRUE(crnet_response_via_proxy(r.resp));
	EXPECT_EQ(EchoHeader(r.echo(), ":authority"),
		  "www.crnet-test:" + std::to_string(p.https));
	EXPECT_GT(SocksConnects(), before);
	crnet_client_free(c);
}

TEST(Proxy, Socks5SchemeIsEquivalent)
{
	const Ports &p = ServerPorts();
	crnet_client *c = ProxyClient("socks5://127.0.0.1:" + std::to_string(p.socks5));
	Result r = Get(c, RemoteOnlyUrl(p.https_h1, "/echo"));
	ASSERT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	EXPECT_EQ(r.protocol(), "http/1.1");
	crnet_client_free(c);
}

TEST(Proxy, HttpConnectProxyWithCredentials)
{
	const Ports &p = ServerPorts();
	crnet_client *c = ProxyClient("http://crnet:secret@127.0.0.1:" +
				      std::to_string(p.http_proxy));
	Result r = Get(c, RemoteOnlyUrl(p.https, "/echo"));
	ASSERT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	EXPECT_EQ(r.status(), 200);
	EXPECT_TRUE(crnet_response_via_proxy(r.resp));
	EXPECT_GT(r.echo()["stats"]["http_proxy_connects"].get<int>(), 0);
	crnet_client_free(c);
}

TEST(Proxy, HttpConnectProxyWrongCredentialsFails)
{
	const Ports &p = ServerPorts();
	crnet_client *c = ProxyClient("http://crnet:wrong@127.0.0.1:" +
				      std::to_string(p.http_proxy));
	Result r = Get(c, RemoteOnlyUrl(p.https, "/echo"));
	EXPECT_NE(r.rc, CRNET_OK);
	EXPECT_NE(r.status(), 200);
	crnet_client_free(c);
}

TEST(Proxy, DeadProxyIsAnError)
{
	crnet_client *c = ProxyClient("socks5h://127.0.0.1:1");
	Result r = Get(c, RemoteOnlyUrl(ServerPorts().https, "/echo"));
	EXPECT_EQ(r.rc, CRNET_E_NET);
	EXPECT_LT(r.net_error(), 0);
	crnet_client_free(c);
}

TEST(Proxy, HeadersUnchangedThroughProxy)
{
	const Ports &p = ServerPorts();
	crnet_client *direct = NewTestClient();
	// <-loopback>: the server is on localhost, which would otherwise
	// bypass the proxy and compare two direct requests.
	crnet_client *proxied = ProxyClient("socks5h://127.0.0.1:" + std::to_string(p.socks5),
					    "<-loopback>");
	Result a = Get(direct, HttpsUrl("/echo"));
	Result b = Get(proxied, HttpsUrl("/echo"));
	ASSERT_EQ(b.rc, CRNET_OK) << crnet_response_error_string(b.resp);
	EXPECT_FALSE(crnet_response_via_proxy(a.resp));
	EXPECT_TRUE(crnet_response_via_proxy(b.resp));
	EXPECT_EQ(HeaderNames(a.echo()), HeaderNames(b.echo()));
	crnet_client_free(direct);
	crnet_client_free(proxied);
}

// Like Chrome, loopback destinations skip the proxy unless the bypass
// list says <-loopback>.
TEST(Proxy, LoopbackGoesDirectByDefault)
{
	const Ports &p = ServerPorts();
	int before = SocksConnects();
	crnet_client *c = ProxyClient("socks5h://127.0.0.1:" + std::to_string(p.socks5));
	Result r = Get(c, HttpsUrl("/echo"));
	ASSERT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	EXPECT_FALSE(crnet_response_via_proxy(r.resp));
	EXPECT_EQ(SocksConnects(), before);
	crnet_client_free(c);
}

TEST(Proxy, SubtractLoopbackSendsLoopbackThroughTheProxy)
{
	const Ports &p = ServerPorts();
	int before = SocksConnects();
	crnet_client *c = ProxyClient("socks5h://127.0.0.1:" + std::to_string(p.socks5),
				      "<-loopback>");
	Result r = Get(c, HttpsUrl("/echo"));
	ASSERT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	EXPECT_TRUE(crnet_response_via_proxy(r.resp));
	EXPECT_GT(SocksConnects(), before);
	crnet_client_free(c);
}

TEST(Proxy, SubtractLoopbackDoesNotFallBackToDirect)
{
	crnet_client *c = ProxyClient("socks5h://127.0.0.1:1", "<-loopback>");
	Result r = Get(c, HttpsUrl("/echo"));
	EXPECT_EQ(r.rc, CRNET_E_NET);
	crnet_client_free(c);
}

TEST(Proxy, BypassRuleSendsMatchingHostsDirect)
{
	// Direct connections find www.crnet-test through the host rules; the
	// proxy is not used for it.
	const Ports &p = ServerPorts();
	int before = SocksConnects();
	crnet_client_config *cfg = crnet_client_config_new();
	std::string proxy = "socks5h://127.0.0.1:" + std::to_string(p.socks5);
	EXPECT_EQ(crnet_client_config_set_proxy(cfg, proxy.c_str()), CRNET_OK);
	EXPECT_EQ(crnet_client_config_set_proxy_bypass(cfg, "foo.example; www.crnet-test"),
		  CRNET_OK);
	EXPECT_EQ(crnet_client_config_set_host_rules(cfg, "MAP www.crnet-test 127.0.0.1"),
		  CRNET_OK);
	crnet_client *c = NewTestClient(cfg);
	crnet_client_config_free(cfg);
	Result r = Get(c, RemoteOnlyUrl(p.https, "/echo"));
	ASSERT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	EXPECT_FALSE(crnet_response_via_proxy(r.resp));
	EXPECT_EQ(SocksConnects(), before);

	// Other hosts still use the proxy.
	Result r2 = Get(c, RemoteOnlyUrl(p.https, "/echo").replace(8, 3, "api"));
	ASSERT_EQ(r2.rc, CRNET_OK) << crnet_response_error_string(r2.resp);
	EXPECT_TRUE(crnet_response_via_proxy(r2.resp));
	EXPECT_GT(SocksConnects(), before);
	crnet_client_free(c);
}

TEST(Proxy, MalformedBypassRulesAreRejected)
{
	crnet_client_config *cfg = crnet_client_config_new();
	EXPECT_EQ(crnet_client_config_set_proxy_bypass(cfg, "*.example.com, 10.0.0.0/8, "
							     "<local>; <-loopback>, [::1]:443"),
		  CRNET_OK);
	EXPECT_EQ(crnet_client_config_set_proxy_bypass(cfg, "10.0.0.0/99"), CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_set_proxy_bypass(cfg, "example.com:99999"), CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_set_proxy_bypass(cfg, "ok.example, ://x"), CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_set_proxy_bypass(cfg, ""), CRNET_OK);
	EXPECT_EQ(crnet_client_config_set_proxy_bypass(cfg, nullptr), CRNET_OK);
	EXPECT_EQ(crnet_client_config_set_proxy_bypass(nullptr, ""), CRNET_E_INVAL);
	crnet_client_config_free(cfg);
}

// ------------------------------------------------------------------
// SOCKS5 failures (the test proxy fails on request)
// ------------------------------------------------------------------

namespace {

Result ViaFaultySocks(const std::string &dest_host)
{
	crnet_client *c = ProxyClient("socks5h://127.0.0.1:" +
				      std::to_string(ServerPorts().socks5));
	Result r = Get(c, RemoteOnlyUrl(ServerPorts().https, "/echo").replace(
				  8, std::string("www.crnet-test").size(), dest_host));
	crnet_client_free(c);
	return r;
}

void SetGreetingFault(const std::string &mode)
{
	crnet_client *c = NewTestClient();
	Result r = Get(c, HttpsUrl("/socks5-fault?greeting=" + mode));
	EXPECT_EQ(r.status(), 200);
	crnet_client_free(c);
}

} // namespace

TEST(Proxy, Socks5ReplyCodesAreReported)
{
	for (int rep = 1; rep <= 8; rep++) {
		SCOPED_TRACE(rep);
		Result r = ViaFaultySocks("rep-" + std::to_string(rep) + ".crnet-test");
		EXPECT_EQ(r.rc, CRNET_E_NET);
		EXPECT_EQ(r.net_error(), -190 - rep);
		EXPECT_EQ(crnet_response_socks_reply(r.resp), rep);
	}
	// Unknown codes stay a generic SOCKS failure.
	Result r = ViaFaultySocks("rep-9.crnet-test");
	EXPECT_EQ(r.net_error(), -120);
	EXPECT_EQ(crnet_response_socks_reply(r.resp), -1);
}

TEST(Proxy, Socks5HandshakeFailures)
{
	const struct {
		const char *host;
		int net_error;
	} cases[] = {
		{"close.crnet-test", -100},	// ERR_CONNECTION_CLOSED
		{"truncate.crnet-test", -100},
		{"badatyp.crnet-test", -120},	// malformed reply
	};
	for (const auto &c : cases) {
		SCOPED_TRACE(c.host);
		Result r = ViaFaultySocks(c.host);
		EXPECT_EQ(r.rc, CRNET_E_NET);
		EXPECT_EQ(r.net_error(), c.net_error);
		EXPECT_EQ(crnet_response_socks_reply(r.resp), -1);
	}
}

TEST(Proxy, Socks5GreetingFailures)
{
	const struct {
		const char *mode;
		int net_error;
	} cases[] = {
		{"close", -100},
		{"noauth", -199},	// no acceptable method
		{"badmethod", -120},	// a method that was not offered
		{"badversion", -120},
	};
	for (const auto &c : cases) {
		SCOPED_TRACE(c.mode);
		SetGreetingFault(c.mode);
		Result r = ViaFaultySocks("www.crnet-test");
		EXPECT_EQ(r.rc, CRNET_E_NET);
		EXPECT_EQ(r.net_error(), c.net_error);
	}
}
