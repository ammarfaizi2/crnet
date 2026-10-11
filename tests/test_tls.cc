// SPDX-License-Identifier: BSD-3-Clause
//
// Extra trust anchors (the default verifier's rejection of the test
// server is in test_basic.cc). The test server's certificate is issued by a throwaway CA
// (ServerPorts().ca_file).
//
#include <fstream>
#include <sstream>

#include "common.h"

using namespace crtest;

namespace {

// A CA that issued nothing the tests talk to.
const char kUnrelatedCa[] =
	"-----BEGIN CERTIFICATE-----\n"
	"MIIBqzCCAVGgAwIBAgIUUf9/kOQQ6T1AjtY13Q/Y/x5ZOtswCgYIKoZIzj0EAwIw\n"
	"IjEgMB4GA1UEAwwXY3JuZXQgdW5yZWxhdGVkIHRlc3QgQ0EwIBcNMjYxMDExMDEz\n"
	"NDA4WhgPMjEyNjA5MTcwMTM0MDhaMCIxIDAeBgNVBAMMF2NybmV0IHVucmVsYXRl\n"
	"ZCB0ZXN0IENBMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAElxkzzd3BK/k1o6LO\n"
	"gcfzh9ukg3a+Wnfge1rn9PNZJZCOnlN4NU7N+24a2GSpO62L3OoIuJThhAhsqqKc\n"
	"78+7raNjMGEwHQYDVR0OBBYEFMrcKKrMlX2W22riILV1ioeSIGZNMB8GA1UdIwQY\n"
	"MBaAFMrcKKrMlX2W22riILV1ioeSIGZNMA8GA1UdEwEB/wQFMAMBAf8wDgYDVR0P\n"
	"AQH/BAQDAgIEMAoGCCqGSM49BAMCA0gAMEUCIETBJ5to9SI5KdgvpkVUamC033UU\n"
	"UEtdutYMec5MpgUTAiEAwzuFdh/ZxvBJXuINWocqkmlJJY3eCNHKdI0jW9lFDwI=\n"
	"-----END CERTIFICATE-----\n";

constexpr int kErrCertAuthorityInvalid = -202;

std::string ReadFile(const std::string &path)
{
	std::ifstream f(path);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// A client that verifies certificates (unlike NewTestClient()).
crnet_client *VerifyingClient(crnet_client_config *cfg)
{
	ServerPorts();
	crnet_client *c = crnet_client_new(cfg);
	crnet_client_config_free(cfg);
	return c;
}

} // namespace

TEST(Tls, TrustAnchorFromFile)
{
	crnet_client_config *cfg = crnet_client_config_new();
	ASSERT_EQ(crnet_client_config_add_trust_anchors_file(cfg, ServerPorts().ca_file.c_str()),
		  CRNET_OK);
	crnet_client *c = VerifyingClient(cfg);
	for (const std::string &url : {HttpsUrl("/echo"), HttpsH1Url("/echo"),
				       "https://127.0.0.1:" + std::to_string(ServerPorts().https) + "/echo"}) {
		Result r = Get(c, url);
		EXPECT_EQ(r.rc, CRNET_OK) << url << ": " << crnet_response_error_string(r.resp);
		EXPECT_EQ(r.status(), 200);
	}
	crnet_client_free(c);
}

// Several blocks in one string, the test CA among them.
TEST(Tls, TrustAnchorsFromPem)
{
	std::string pem = std::string(kUnrelatedCa) + ReadFile(ServerPorts().ca_file);
	crnet_client_config *cfg = crnet_client_config_new();
	ASSERT_EQ(crnet_client_config_add_trust_anchors(cfg, pem.c_str()), CRNET_OK);
	crnet_client *c = VerifyingClient(cfg);
	Result r = Get(c, HttpsUrl("/echo"));
	EXPECT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	crnet_client_free(c);
}

TEST(Tls, TrustAnchorsOnly)
{
	crnet_client_config *cfg = crnet_client_config_new();
	ASSERT_EQ(crnet_client_config_add_trust_anchors_file(cfg, ServerPorts().ca_file.c_str()),
		  CRNET_OK);
	crnet_client_config_set_trust_anchors_only(cfg, 1);
	crnet_client *c = VerifyingClient(cfg);
	Result r = Get(c, HttpsUrl("/echo"));
	EXPECT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	crnet_client_free(c);

	cfg = crnet_client_config_new();
	ASSERT_EQ(crnet_client_config_add_trust_anchors(cfg, kUnrelatedCa), CRNET_OK);
	crnet_client_config_set_trust_anchors_only(cfg, 1);
	c = VerifyingClient(cfg);
	Result r2 = Get(c, HttpsUrl("/echo"));
	EXPECT_EQ(r2.net_error(), kErrCertAuthorityInvalid);
	crnet_client_free(c);
}

TEST(Tls, MalformedTrustAnchorsAreRejected)
{
	crnet_client_config *cfg = crnet_client_config_new();
	EXPECT_EQ(crnet_client_config_add_trust_anchors(cfg, ""), CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_add_trust_anchors(cfg, "not a certificate"), CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_add_trust_anchors(
			  cfg, "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n"),
		  CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_add_trust_anchors(cfg, nullptr), CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_add_trust_anchors_file(cfg, "/nonexistent/ca.pem"),
		  CRNET_E_INVAL);
	crnet_client_config_free(cfg);
}
