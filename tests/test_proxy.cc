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

// ------------------------------------------------------------------
// SOCKS5 username/password authentication (RFC 1929)
// ------------------------------------------------------------------

namespace {

std::string AuthProxy(const std::string &userinfo)
{
	return "socks5h://" + userinfo + "@127.0.0.1:" + std::to_string(ServerPorts().socks5_auth);
}

json SocksAuths()
{
	crnet_client *c = NewTestClient();
	Result r = Get(c, HttpsUrl("/echo"));
	crnet_client_free(c);
	return r.echo()["stats"]["socks5_auths"];
}

json SocksGreetings()
{
	crnet_client *c = NewTestClient();
	Result r = Get(c, HttpsUrl("/echo"));
	crnet_client_free(c);
	return r.echo()["stats"]["socks5_greetings"];
}

} // namespace

TEST(Proxy, Socks5UsernamePassword)
{
	size_t before = SocksAuths().size();
	crnet_client *c = ProxyClient(AuthProxy("crnet:secret"));
	ASSERT_NE(c, nullptr);
	Result r = Get(c, RemoteOnlyUrl(ServerPorts().https, "/echo"));
	ASSERT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	EXPECT_TRUE(crnet_response_via_proxy(r.resp));
	crnet_client_free(c);
	json auths = SocksAuths();
	ASSERT_EQ(auths.size(), before + 1);
	EXPECT_EQ(auths.back(), json::array({"crnet", true}));
}

TEST(Proxy, Socks5WrongPasswordIsAnAuthFailure)
{
	crnet_client *c = ProxyClient(AuthProxy("crnet:wrong"));
	Result r = Get(c, RemoteOnlyUrl(ServerPorts().https, "/echo"));
	EXPECT_EQ(r.rc, CRNET_E_NET);
	EXPECT_EQ(r.net_error(), -185);	// ERR_SOCKS_AUTH_FAILED
	crnet_client_free(c);
}

TEST(Proxy, Socks5WithoutCredentialsIsRefused)
{
	crnet_client *c = ProxyClient("socks5h://127.0.0.1:" +
				      std::to_string(ServerPorts().socks5_auth));
	Result r = Get(c, RemoteOnlyUrl(ServerPorts().https, "/echo"));
	EXPECT_EQ(r.net_error(), -199);	// no acceptable methods
	crnet_client_free(c);
}

// Credentials are offered besides "no authentication"; a proxy that
// needs none just picks that.
TEST(Proxy, Socks5CredentialsForAnOpenProxy)
{
	crnet_client *c = ProxyClient("socks5h://crnet:secret@127.0.0.1:" +
				      std::to_string(ServerPorts().socks5));
	Result r = Get(c, RemoteOnlyUrl(ServerPorts().https, "/echo"));
	EXPECT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	crnet_client_free(c);
}

// Each client authenticates with its own credentials, and a client given
// none offers none, whatever other clients use the proxy with.
TEST(Proxy, Socks5CredentialsArePerClient)
{
	const std::string url = RemoteOnlyUrl(ServerPorts().https, "/echo");
	crnet_client *a = ProxyClient(AuthProxy("crnet-a:secret"));
	crnet_client *b = ProxyClient(AuthProxy("crnet-b:secret"));
	crnet_client *none = ProxyClient("socks5h://127.0.0.1:" +
					 std::to_string(ServerPorts().socks5_auth));
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	ASSERT_NE(none, nullptr);
	ASSERT_EQ(Get(a, url).rc, CRNET_OK);
	ASSERT_EQ(Get(b, url).rc, CRNET_OK);
	EXPECT_EQ(SocksAuths().back(), json::array({"crnet-b", true}));

	size_t greetings = SocksGreetings().size();
	size_t auths = SocksAuths().size();
	Result r = Get(none, url);
	EXPECT_EQ(r.net_error(), -199);	// no acceptable methods
	json g = SocksGreetings();
	ASSERT_EQ(g.size(), greetings + 1);
	EXPECT_EQ(g.back(), json::array({0}));	// 05 01 00
	EXPECT_EQ(SocksAuths().size(), auths);
	crnet_client_free(none);
	crnet_client_free(b);
	crnet_client_free(a);
}

TEST(Proxy, Socks5CredentialSyntax)
{
	crnet_client_config *cfg = crnet_client_config_new();
	EXPECT_EQ(crnet_client_config_set_proxy(cfg, "socks5://u%3Ax:p%40ss@127.0.0.1:1080"),
		  CRNET_OK);
	EXPECT_EQ(crnet_client_config_set_proxy(cfg, "socks4://u:p@127.0.0.1:1080"),
		  CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_set_proxy(cfg, "socks5://u@127.0.0.1:1080"),
		  CRNET_E_INVAL) << "RFC 1929 needs a password";
	std::string long_user(256, 'u');
	EXPECT_EQ(crnet_client_config_set_proxy(
			  cfg, ("socks5://" + long_user + ":p@127.0.0.1:1080").c_str()),
		  CRNET_E_INVAL);
	crnet_client_config_free(cfg);
}

namespace {

Result GetVia(crnet_client *c, const std::string &url, const std::string &proxy)
{
	crnet_request *req = crnet_request_new("GET", url.c_str());
	EXPECT_EQ(crnet_request_set_proxy(req, proxy.c_str()), CRNET_OK) << proxy;
	Result r;
	r.rc = crnet_perform(c, req, &r.resp);
	crnet_request_free(req);
	return r;
}

std::string Socks5()
{
	return "socks5h://127.0.0.1:" + std::to_string(ServerPorts().socks5);
}

std::string HttpProxy(const std::string &userinfo)
{
	return "http://" + userinfo + "@127.0.0.1:" +
	       std::to_string(ServerPorts().http_proxy);
}

} // namespace

TEST(RequestProxy, Validation)
{
	crnet_request *req = crnet_request_new("GET", "https://example.com/");
	for (const char *ok : {"socks5://127.0.0.1:1080", "http://u:p@proxy:3128",
			       "https://proxy", "socks4://proxy:1080", "direct://"})
		EXPECT_EQ(crnet_request_set_proxy(req, ok), CRNET_OK) << ok;
	EXPECT_EQ(crnet_request_set_proxy(req, nullptr), CRNET_OK);
	for (const char *bad : {"", "proxy:1080", "ftp://proxy", "socks4://u:p@proxy",
				"socks5://proxy/path", "http://"})
		EXPECT_EQ(crnet_request_set_proxy(req, bad), CRNET_E_INVAL) << bad;
	EXPECT_EQ(crnet_request_set_proxy(nullptr, "direct://"), CRNET_E_INVAL);
	crnet_request_free(req);
}

TEST(RequestProxy, OverridesTheClientsProxy)
{
	const Ports &p = ServerPorts();
	int before = SocksConnects();
	// A direct client reaches a name only the proxy can resolve through a
	// request's proxy, and follows redirects through it. Locally the name
	// does not resolve; the host rule keeps the system's DNS out of it.
	crnet_client_config *cfg = crnet_client_config_new();
	ASSERT_EQ(crnet_client_config_set_host_rules(cfg, "MAP www.crnet-test ~NOTFOUND"),
		  CRNET_OK);
	crnet_client *c = NewTestClient(cfg);
	crnet_client_config_free(cfg);
	EXPECT_EQ(Get(c, RemoteOnlyUrl(p.https, "/echo")).net_error(), -105);
	Result r = GetVia(c, RemoteOnlyUrl(p.https, "/redirect/2"), Socks5());
	ASSERT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	EXPECT_EQ(crnet_response_redirect_count(r.resp), 2);
	EXPECT_TRUE(crnet_response_via_proxy(r.resp));
	EXPECT_GT(SocksConnects(), before);
	crnet_client_free(c);

	// A proxied client sends a request direct.
	c = ProxyClient(Socks5(), "<-loopback>");
	r = GetVia(c, HttpsUrl("/echo"), "direct://");
	ASSERT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	EXPECT_FALSE(crnet_response_via_proxy(r.resp));
	EXPECT_TRUE(crnet_response_via_proxy(Get(c, HttpsUrl("/echo")).resp));
	crnet_client_free(c);
}

TEST(RequestProxy, IgnoresBypassRules)
{
	// localhost bypasses every configured proxy, but not a request's.
	crnet_client *c = NewTestClient();
	Result r = GetVia(c, HttpsUrl("/echo"), Socks5());
	ASSERT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	EXPECT_TRUE(crnet_response_via_proxy(r.resp));
	crnet_client_free(c);
}

TEST(RequestProxy, ConnectionsAreNotSharedAcrossProxies)
{
	crnet_client *c = NewTestClient();
	const std::string url = HttpsUrl("/echo");
	Result direct = Get(c, url);
	ASSERT_EQ(direct.rc, CRNET_OK);
	Result proxied = GetVia(c, url, Socks5());
	ASSERT_EQ(proxied.rc, CRNET_OK);
	EXPECT_FALSE(crnet_response_socket_reused(proxied.resp));
	EXPECT_NE(crnet_response_connection_id(proxied.resp),
		  crnet_response_connection_id(direct.resp));
	Result again = GetVia(c, url, Socks5());
	EXPECT_EQ(crnet_response_connection_id(again.resp),
		  crnet_response_connection_id(proxied.resp));
	Result direct2 = Get(c, url);
	EXPECT_EQ(crnet_response_connection_id(direct2.resp),
		  crnet_response_connection_id(direct.resp));
	crnet_client_free(c);
}

namespace {

json HttpProxyAuths()
{
	crnet_client *c = NewTestClient();
	Result r = Get(c, HttpsUrl("/echo"));
	crnet_client_free(c);
	return r.echo()["stats"]["http_proxy_auths"];
}

// The last |n| users the proxy saw, oldest first.
std::vector<std::string> LastUsers(const json &auths, size_t n)
{
	std::vector<std::string> users;
	for (size_t i = auths.size() >= n ? auths.size() - n : 0; i < auths.size(); i++)
		users.push_back(auths[i][0].is_null() ? "" : auths[i][0].get<std::string>());
	return users;
}

Result Request(crnet_client *c, const std::string &url, const std::string &proxy,
	       bool fresh = false)
{
	crnet_request *req = crnet_request_new("GET", url.c_str());
	EXPECT_EQ(crnet_request_set_proxy(req, proxy.c_str()), CRNET_OK) << proxy;
	crnet_request_set_fresh_connection(req, fresh);
	Result r;
	r.rc = crnet_perform(c, req, &r.resp);
	crnet_request_free(req);
	return r;
}

} // namespace

TEST(RequestProxy, HttpProxyIdentities)
{
	const Ports &p = ServerPorts();
	const std::string url = RemoteOnlyUrl(p.https, "/echo");
	crnet_client *c = NewTestClient();
	// Without credentials the proxy's 407 is the answer.
	EXPECT_NE(GetVia(c, url, "http://127.0.0.1:" + std::to_string(p.http_proxy)).rc,
		  CRNET_OK);

	// The first credentials become the proxy's default; others are
	// identities, with connections of their own.
	size_t before = HttpProxyAuths().size();
	Result a = GetVia(c, url, HttpProxy("crnet-a:secret"));
	ASSERT_EQ(a.rc, CRNET_OK) << crnet_response_error_string(a.resp);
	Result b = GetVia(c, url, HttpProxy("crnet-b:secret"));
	ASSERT_EQ(b.rc, CRNET_OK) << crnet_response_error_string(b.resp);
	EXPECT_NE(crnet_response_connection_id(b.resp), crnet_response_connection_id(a.resp));
	json auths = HttpProxyAuths();
	ASSERT_EQ(auths.size(), before + 2);
	EXPECT_EQ(LastUsers(auths, 2), (std::vector<std::string>{"crnet-a", "crnet-b"}));

	// Each reuses its own connection.
	Result a2 = GetVia(c, url, HttpProxy("crnet-a:secret"));
	Result b2 = GetVia(c, url, HttpProxy("crnet-b:secret"));
	EXPECT_EQ(crnet_response_connection_id(a2.resp), crnet_response_connection_id(a.resp));
	EXPECT_EQ(crnet_response_connection_id(b2.resp), crnet_response_connection_id(b.resp));
	EXPECT_EQ(HttpProxyAuths().size(), before + 2);

	// New tunnels: the default answers as itself, not as the identity
	// Chromium's proxy auth cache saw last.
	const std::string url2 = RemoteOnlyUrl(p.https_h1, "/echo");
	ASSERT_EQ(GetVia(c, url2, HttpProxy("crnet-b:secret")).rc, CRNET_OK);
	ASSERT_EQ(GetVia(c, url2, "http://127.0.0.1:" + std::to_string(p.http_proxy)).rc,
		  CRNET_OK);
	auths = HttpProxyAuths();
	ASSERT_EQ(auths.size(), before + 4);
	EXPECT_EQ(LastUsers(auths, 2), (std::vector<std::string>{"crnet-b", "crnet-a"}));

	// Refused credentials of an identity fail the request at once.
	Result bad = GetVia(c, url, HttpProxy("crnet-c:wrong"));
	EXPECT_NE(bad.rc, CRNET_OK);
	auths = HttpProxyAuths();
	EXPECT_EQ(auths.size(), before + 5);
	EXPECT_EQ(auths.back(), json::array({"crnet-c", false}));
	crnet_client_free(c);
}

TEST(RequestProxy, HttpProxyIdentitiesWithoutATunnel)
{
	const Ports &p = ServerPorts();
	auto user_of = [](const Result &r) {
		EXPECT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
		return r.rc == CRNET_OK ? json::parse(r.body()).value("proxy_user", "") : "";
	};
	// http:// requests go to the proxy itself, which answers them.
	const std::string url = "http://www.crnet-test/plain";
	crnet_client *c = NewTestClient();
	EXPECT_EQ(user_of(GetVia(c, url, HttpProxy("crnet-a:secret"))), "crnet-a");
	EXPECT_EQ(user_of(GetVia(c, url, HttpProxy("crnet-b:secret"))), "crnet-b");
	EXPECT_EQ(user_of(GetVia(c, url, "http://127.0.0.1:" + std::to_string(p.http_proxy))),
		  "crnet-a");
	EXPECT_EQ(user_of(GetVia(c, url, HttpProxy("crnet-b:secret"))), "crnet-b");
	crnet_client_free(c);
}

TEST(RequestProxy, Socks5Identities)
{
	const Ports &p = ServerPorts();
	const std::string url = RemoteOnlyUrl(p.https, "/echo");
	size_t before = SocksAuths().size();
	crnet_client *c = NewTestClient();
	Result a = GetVia(c, url, AuthProxy("crnet-a:secret"));
	ASSERT_EQ(a.rc, CRNET_OK) << crnet_response_error_string(a.resp);
	Result b = GetVia(c, url, AuthProxy("crnet-b:secret"));
	ASSERT_EQ(b.rc, CRNET_OK) << crnet_response_error_string(b.resp);
	Result a2 = GetVia(c, url, AuthProxy("crnet-a:secret"));
	Result b2 = GetVia(c, url, AuthProxy("crnet-b:secret"));
	EXPECT_EQ(crnet_response_connection_id(a2.resp), crnet_response_connection_id(a.resp));
	EXPECT_EQ(crnet_response_connection_id(b2.resp), crnet_response_connection_id(b.resp));
	json auths = SocksAuths();
	ASSERT_EQ(auths.size(), before + 2);
	EXPECT_EQ(LastUsers(auths, 2), (std::vector<std::string>{"crnet-a", "crnet-b"}));

	// Without credentials: the default.
	Result d = GetVia(c, RemoteOnlyUrl(p.https_h1, "/echo"),
			  "socks5h://127.0.0.1:" + std::to_string(p.socks5_auth));
	EXPECT_EQ(d.rc, CRNET_OK) << crnet_response_error_string(d.resp);
	EXPECT_EQ(SocksAuths().back(), json::array({"crnet-a", true}));

	Result bad = GetVia(c, url, AuthProxy("crnet-c:wrong"));
	EXPECT_EQ(bad.net_error(), -185);	// ERR_SOCKS_AUTH_FAILED

	// Fresh connections of an identity authenticate as it, each anew.
	before = SocksAuths().size();
	for (int i = 0; i < 2; i++)
		EXPECT_EQ(Request(c, url, AuthProxy("crnet-b:secret"), true).rc, CRNET_OK);
	auths = SocksAuths();
	ASSERT_EQ(auths.size(), before + 2);
	EXPECT_EQ(LastUsers(auths, 2), (std::vector<std::string>{"crnet-b", "crnet-b"}));
	crnet_client_free(c);
}

TEST(RequestProxy, Socks5CredentialsAreTheClients)
{
	const Ports &p = ServerPorts();
	const std::string url = RemoteOnlyUrl(p.https, "/echo");
	const std::string bare = "socks5h://127.0.0.1:" + std::to_string(p.socks5_auth);
	// Credentials another client was given, at client or request level,
	// are never this client's.
	crnet_client *other = ProxyClient(AuthProxy("crnet-x:secret"));
	crnet_client *other2 = NewTestClient();
	ASSERT_EQ(GetVia(other2, url, AuthProxy("crnet-w:secret")).rc, CRNET_OK);
	crnet_client *c = NewTestClient();
	size_t greetings = SocksGreetings().size();
	EXPECT_EQ(GetVia(c, url, bare).net_error(), -199);
	EXPECT_EQ(SocksGreetings().back(), json::array({0}));
	EXPECT_EQ(SocksGreetings().size(), greetings + 1);

	// Fresh connections of a client without credentials offer none
	// either; those of a client with some use them.
	crnet_request *req = crnet_request_new("GET", url.c_str());
	crnet_request_set_proxy(req, bare.c_str());
	crnet_request_set_fresh_connection(req, 1);
	Result f;
	f.rc = crnet_perform(c, req, &f.resp);
	EXPECT_EQ(f.net_error(), -199);
	Result fo;
	fo.rc = crnet_perform(other, req, &fo.resp);
	EXPECT_EQ(fo.rc, CRNET_OK) << crnet_response_error_string(fo.resp);
	EXPECT_EQ(SocksAuths().back(), json::array({"crnet-x", true}));
	crnet_request_free(req);

	// Its own request credentials work beside the others'.
	Result r = GetVia(c, url, AuthProxy("crnet-y:secret"));
	EXPECT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	EXPECT_EQ(SocksAuths().back(), json::array({"crnet-y", true}));
	crnet_client_free(c);
	crnet_client_free(other2);
	crnet_client_free(other);
}
