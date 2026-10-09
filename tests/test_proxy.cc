// SPDX-License-Identifier: BSD-3-Clause
//
// Proxies. The test server's proxies resolve "*.crnet-test" to
// 127.0.0.1 themselves; these names do not exist in DNS, so a success
// proves the name was resolved on the proxy side (socks5h semantics).
//
#include "common.h"

using namespace crtest;

namespace {

crnet_client *ProxyClient(const std::string &proxy)
{
	crnet_client_config *cfg = crnet_client_config_new();
	EXPECT_EQ(crnet_client_config_set_proxy(cfg, proxy.c_str()), CRNET_OK);
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
	crnet_client *proxied = ProxyClient("socks5h://127.0.0.1:" + std::to_string(p.socks5));
	Result a = Get(direct, HttpsUrl("/echo"));
	Result b = Get(proxied, HttpsUrl("/echo"));
	EXPECT_EQ(HeaderNames(a.echo()), HeaderNames(b.echo()));
	crnet_client_free(direct);
	crnet_client_free(proxied);
}
