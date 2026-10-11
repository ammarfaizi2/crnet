// SPDX-License-Identifier: BSD-3-Clause
//
// DNS over HTTPS. The test server's /dns-query resolves names under
// .crnet-test, which ordinary DNS does not know, so a successful request
// to such a name proves it was resolved over HTTPS.
//
#include <chrono>
#include <thread>

#include "common.h"

using namespace crtest;

namespace {

std::string DohTemplate(bool get)
{
	return "https://localhost:" + std::to_string(ServerPorts().https) +
	       (get ? "/dns-query{?dns}" : "/dns-query");
}

crnet_client *DohClient(crnet_secure_dns mode, const std::string &templates)
{
	crnet_client_config *cfg = crnet_client_config_new();
	EXPECT_EQ(crnet_client_config_set_secure_dns(cfg, mode, templates.c_str()), CRNET_OK);
	crnet_client *c = NewTestClient(cfg);
	crnet_client_config_free(cfg);
	return c;
}

std::string DohOnlyUrl(const std::string &host)
{
	return "https://" + host + ":" + std::to_string(ServerPorts().https) + "/echo";
}

bool ServerSawQuery(const std::string &name)
{
	crnet_client *c = NewTestClient();
	Result r = Get(c, HttpsUrl("/echo"));
	crnet_client_free(c);
	json queries = r.echo()["stats"]["doh_queries"];
	for (const json &q : queries) {
		if (q[0] == name)
			return true;
	}
	return false;
}

} // namespace

TEST(Dns, SecureModeResolvesOverHttps)
{
	for (bool get : {true, false}) {
		SCOPED_TRACE(get ? "GET" : "POST");
		std::string host = get ? "get.crnet-test" : "post.crnet-test";
		crnet_client *c = DohClient(CRNET_SECURE_DNS_SECURE, DohTemplate(get));
		Result r = Get(c, DohOnlyUrl(host));
		EXPECT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
		EXPECT_EQ(r.status(), 200);
		EXPECT_FALSE(crnet_response_via_proxy(r.resp));
		EXPECT_TRUE(ServerSawQuery(host));
		crnet_client_free(c);
	}
}

// Secure mode never falls back: a name the DoH server does not know
// fails even if ordinary DNS might know it.
TEST(Dns, SecureModeDoesNotFallBack)
{
	crnet_client *c = DohClient(CRNET_SECURE_DNS_SECURE, DohTemplate(true));
	Result r = Get(c, "https://nx.invalid:1234/");
	EXPECT_EQ(r.rc, CRNET_E_NET);
	EXPECT_EQ(r.net_error(), -105);	// ERR_NAME_NOT_RESOLVED
	EXPECT_TRUE(ServerSawQuery("nx.invalid"));
	crnet_client_free(c);
}

// Automatic mode uses a DoH server once a probe (a lookup of
// www.gstatic.com) succeeded; until then it resolves with ordinary DNS,
// which does not know the name.
TEST(Dns, AutomaticModeUsesDohOnceProbed)
{
	crnet_client *c = DohClient(CRNET_SECURE_DNS_AUTOMATIC, DohTemplate(true));
	bool probed = false;
	for (int i = 0; i < 100 && !probed; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		probed = ServerSawQuery("www.gstatic.com");
	}
	ASSERT_TRUE(probed) << "the client never probed the DoH server";
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	Result r = Get(c, DohOnlyUrl("auto.crnet-test"));
	EXPECT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	EXPECT_TRUE(ServerSawQuery("auto.crnet-test"));
	crnet_client_free(c);
}

TEST(Dns, InvalidSecureDnsSettings)
{
	crnet_client_config *cfg = crnet_client_config_new();
	EXPECT_EQ(crnet_client_config_set_secure_dns(cfg, CRNET_SECURE_DNS_SECURE, nullptr),
		  CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_set_secure_dns(cfg, CRNET_SECURE_DNS_SECURE, ""),
		  CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_set_secure_dns(cfg, CRNET_SECURE_DNS_SECURE,
						     "http://insecure.example/dns-query"),
		  CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_set_secure_dns(cfg, CRNET_SECURE_DNS_AUTOMATIC,
						     "https://a.example/dns-query not-a-url"),
		  CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_set_secure_dns(cfg, static_cast<crnet_secure_dns>(5),
						     "https://a.example/dns-query"),
		  CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_set_secure_dns(
			  cfg, CRNET_SECURE_DNS_SECURE,
			  "https://a.example/dns-query{?dns} https://b.example/q"),
		  CRNET_OK);
	EXPECT_EQ(crnet_client_config_set_secure_dns(cfg, CRNET_SECURE_DNS_OFF, nullptr), CRNET_OK);
	crnet_client_config_free(cfg);
}
