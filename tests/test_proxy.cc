// SPDX-License-Identifier: BSD-3-Clause
//
// Proxies. The test server's proxies resolve "*.crhttp-test" to
// 127.0.0.1 themselves; these names do not exist in DNS, so a success
// proves the name was resolved on the proxy side (socks5h semantics).
//
#include "common.h"

using namespace crtest;

namespace {

crhttp_client *ProxyClient(const std::string &proxy)
{
	crhttp_client_config *cfg = crhttp_client_config_new();
	EXPECT_EQ(crhttp_client_config_set_proxy(cfg, proxy.c_str()), CRHTTP_OK);
	crhttp_client *c = NewTestClient(cfg);
	crhttp_client_config_free(cfg);
	return c;
}

std::string RemoteOnlyUrl(int port, const std::string &path)
{
	return "https://www.crhttp-test:" + std::to_string(port) + path;
}

int SocksConnects()
{
	crhttp_client *c = NewTestClient();
	Result r = Get(c, HttpsUrl("/echo"));
	crhttp_client_free(c);
	return r.echo()["stats"]["socks5_connects"];
}

} // namespace

TEST(Proxy, Socks5hResolvesRemotely)
{
	const Ports &p = ServerPorts();
	int before = SocksConnects();
	crhttp_client *c = ProxyClient("socks5h://127.0.0.1:" + std::to_string(p.socks5));
	Result r = Get(c, RemoteOnlyUrl(p.https, "/echo"));
	ASSERT_EQ(r.rc, CRHTTP_OK) << crhttp_response_error_string(r.resp);
	EXPECT_EQ(r.status(), 200);
	EXPECT_EQ(r.protocol(), "h2");
	EXPECT_TRUE(crhttp_response_via_proxy(r.resp));
	EXPECT_EQ(EchoHeader(r.echo(), ":authority"),
		  "www.crhttp-test:" + std::to_string(p.https));
	EXPECT_GT(SocksConnects(), before);
	crhttp_client_free(c);
}

TEST(Proxy, Socks5SchemeIsEquivalent)
{
	const Ports &p = ServerPorts();
	crhttp_client *c = ProxyClient("socks5://127.0.0.1:" + std::to_string(p.socks5));
	Result r = Get(c, RemoteOnlyUrl(p.https_h1, "/echo"));
	ASSERT_EQ(r.rc, CRHTTP_OK) << crhttp_response_error_string(r.resp);
	EXPECT_EQ(r.protocol(), "http/1.1");
	crhttp_client_free(c);
}

TEST(Proxy, HttpConnectProxyWithCredentials)
{
	const Ports &p = ServerPorts();
	crhttp_client *c = ProxyClient("http://crhttp:secret@127.0.0.1:" +
				       std::to_string(p.http_proxy));
	Result r = Get(c, RemoteOnlyUrl(p.https, "/echo"));
	ASSERT_EQ(r.rc, CRHTTP_OK) << crhttp_response_error_string(r.resp);
	EXPECT_EQ(r.status(), 200);
	EXPECT_TRUE(crhttp_response_via_proxy(r.resp));
	EXPECT_GT(r.echo()["stats"]["http_proxy_connects"].get<int>(), 0);
	crhttp_client_free(c);
}

TEST(Proxy, HttpConnectProxyWrongCredentialsFails)
{
	const Ports &p = ServerPorts();
	crhttp_client *c = ProxyClient("http://crhttp:wrong@127.0.0.1:" +
				       std::to_string(p.http_proxy));
	Result r = Get(c, RemoteOnlyUrl(p.https, "/echo"));
	EXPECT_NE(r.rc, CRHTTP_OK);
	EXPECT_NE(r.status(), 200);
	crhttp_client_free(c);
}

TEST(Proxy, DeadProxyIsAnError)
{
	crhttp_client *c = ProxyClient("socks5h://127.0.0.1:1");
	Result r = Get(c, RemoteOnlyUrl(ServerPorts().https, "/echo"));
	EXPECT_EQ(r.rc, CRHTTP_E_NET);
	EXPECT_LT(r.net_error(), 0);
	crhttp_client_free(c);
}

TEST(Proxy, HeadersUnchangedThroughProxy)
{
	const Ports &p = ServerPorts();
	crhttp_client *direct = NewTestClient();
	crhttp_client *proxied = ProxyClient("socks5h://127.0.0.1:" + std::to_string(p.socks5));
	Result a = Get(direct, HttpsUrl("/echo"));
	Result b = Get(proxied, HttpsUrl("/echo"));
	EXPECT_EQ(HeaderNames(a.echo()), HeaderNames(b.echo()));
	crhttp_client_free(direct);
	crhttp_client_free(proxied);
}
