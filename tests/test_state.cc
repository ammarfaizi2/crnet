// SPDX-License-Identifier: BSD-3-Clause
//
// Network state kept across runs in a state directory.
//
#include <stdlib.h>
#include <sys/stat.h>

#include <fstream>
#include <sstream>

#include "common.h"

using namespace crtest;

namespace {

class TempDir {
public:
	TempDir()
	{
		std::string tmpl = ::testing::TempDir() + "crnet-state-XXXXXX";
		path_ = mkdtemp(tmpl.data()) ? tmpl : "";
		EXPECT_FALSE(path_.empty());
	}
	~TempDir()
	{
		std::string rm = "rm -rf '" + path_ + "'";
		EXPECT_EQ(system(rm.c_str()), 0);
	}
	const std::string &path() const { return path_; }
	std::string file(const std::string &name) const { return path_ + "/" + name; }

private:
	std::string path_;
};

std::string ReadFile(const std::string &path)
{
	std::ifstream f(path);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// Verifies the server against the test CA: Chromium ignores HSTS and
// Alt-Svc headers on responses with certificate errors.
crnet_client *StateClient(const std::string &dir)
{
	crnet_client_config *cfg = crnet_client_config_new();
	// Chromium ignores HSTS for localhost; the server's certificate names
	// this host.
	EXPECT_EQ(crnet_client_config_set_host_rules(cfg, "MAP hsts.crnet-test 127.0.0.1"),
		  CRNET_OK);
	if (!dir.empty()) {
		EXPECT_EQ(crnet_client_config_set_state_dir(cfg, dir.c_str()), CRNET_OK);
	}
	EXPECT_EQ(crnet_client_config_add_trust_anchors_file(cfg, ServerPorts().ca_file.c_str()),
		  CRNET_OK);
	crnet_client *c = crnet_client_new(cfg);
	crnet_client_config_free(cfg);
	return c;
}

std::string Https(const std::string &path)
{
	return "https://localhost:" + std::to_string(ServerPorts().https) + path;
}

} // namespace

TEST(State, ServerPropertiesSurviveRestarts)
{
	TempDir dir;
	std::string file = dir.file("Network Persistent State");

	crnet_client *a = StateClient(dir.path());
	Result r = Get(a, Https("/response-headers?Alt-Svc=h3%3D%22%3A443%22%3B%20ma%3D86400"));
	ASSERT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	crnet_client_free(a);	// returns once the state is saved
	std::string saved = ReadFile(file);
	EXPECT_NE(saved.find("\"h3\""), std::string::npos) << saved;
	EXPECT_NE(saved.find("supports_spdy"), std::string::npos) << saved;

	// A client that learns nothing new must keep what it loaded, even
	// when freed at once.
	crnet_client *b = StateClient(dir.path());
	crnet_client_free(b);
	EXPECT_EQ(ReadFile(file), saved);
}

TEST(State, HstsSurvivesRestarts)
{
	TempDir dir;
	// http:// to the TLS port only works once HSTS upgrades it.
	std::string host = "hsts.crnet-test:" + std::to_string(ServerPorts().https);
	std::string http_url = "http://" + host + "/echo";

	crnet_client *a = StateClient(dir.path());
	Result before = Get(a, http_url);
	EXPECT_NE(before.rc, CRNET_OK);
	Result r = Get(a, "https://" + host +
				  "/response-headers?Strict-Transport-Security=max-age%3D3600");
	ASSERT_EQ(r.rc, CRNET_OK) << crnet_response_error_string(r.resp);
	Result upgraded_now = Get(a, http_url);
	EXPECT_EQ(upgraded_now.rc, CRNET_OK) << "HSTS applies at once";
	crnet_client_free(a);
	EXPECT_NE(ReadFile(dir.file("TransportSecurity")).find("\"sts\""), std::string::npos);

	crnet_client *b = StateClient(dir.path());
	// The file is read in the background; retry briefly.
	int rc = -1;
	std::string final_url;
	for (int i = 0; i < 50 && rc != CRNET_OK; i++) {
		Result upgraded = Get(b, http_url);
		rc = upgraded.rc;
		final_url = upgraded.resp ? crnet_response_url(upgraded.resp) : "";
	}
	EXPECT_EQ(rc, CRNET_OK);
	EXPECT_EQ(final_url.rfind("https://", 0), 0u) << final_url;
	crnet_client_free(b);

	// Without the directory nothing is remembered.
	crnet_client *c = StateClient("");
	EXPECT_NE(Get(c, http_url).rc, CRNET_OK);
	crnet_client_free(c);
}

TEST(State, DirectoryIsCreated)
{
	TempDir dir;
	std::string sub = dir.file("a/b");
	crnet_client *c = StateClient(sub);
	Get(c, Https("/echo"));
	crnet_client_free(c);
	struct stat st;
	EXPECT_EQ(stat((sub + "/Network Persistent State").c_str(), &st), 0);
}
