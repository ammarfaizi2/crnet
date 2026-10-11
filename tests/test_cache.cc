// SPDX-License-Identifier: BSD-3-Clause
//
// The HTTP cache: off by default, in memory or on disk, and the
// per-request cache modes.
//
#include <stdlib.h>
#include <unistd.h>

#include <atomic>

#include "common.h"

using namespace crtest;

namespace {

// A key no other test (or earlier run against the same server) used.
std::string NewKey()
{
	static std::atomic<int> n{0};
	return "k" + std::to_string(getpid()) + "-" + std::to_string(n++);
}

// Verifies the server against the test CA instead of using the insecure
// option: Chromium does not cache responses that had certificate errors.
crnet_client *CacheClient(crnet_http_cache type, const std::string &path = "")
{
	crnet_client_config *cfg = crnet_client_config_new();
	EXPECT_EQ(crnet_client_config_set_http_cache(cfg, type,
						     path.empty() ? nullptr : path.c_str(), 0),
		  CRNET_OK);
	EXPECT_EQ(crnet_client_config_add_trust_anchors_file(cfg, ServerPorts().ca_file.c_str()),
		  CRNET_OK);
	crnet_client *c = crnet_client_new(cfg);
	crnet_client_config_free(cfg);
	return c;
}

Result GetMode(crnet_client *c, const std::string &url, crnet_cache_mode mode)
{
	crnet_request *req = crnet_request_new("GET", url.c_str());
	EXPECT_EQ(crnet_request_set_cache_mode(req, mode), CRNET_OK);
	Result r = Perform(c, req);
	crnet_request_free(req);
	return r;
}

json Stats(const std::string &key)
{
	crnet_client *c = NewTestClient();
	Result r = Get(c, HttpsUrl("/cache-stats/" + key));
	crnet_client_free(c);
	return r.echo();
}

} // namespace

TEST(Cache, OffByDefault)
{
	std::string key = NewKey();
	crnet_client *c = CacheClient(CRNET_HTTP_CACHE_OFF);
	Result a = Get(c, HttpsUrl("/cache/" + key));
	Result b = Get(c, HttpsUrl("/cache/" + key));
	ASSERT_EQ(b.rc, CRNET_OK);
	EXPECT_FALSE(crnet_response_from_cache(b.resp));
	EXPECT_EQ(Stats(key)["requests"], 2);
	crnet_client_free(c);
}

TEST(Cache, FreshEntryIsServedFromMemory)
{
	std::string key = NewKey();
	crnet_client *c = CacheClient(CRNET_HTTP_CACHE_MEMORY);
	Result a = Get(c, HttpsUrl("/cache/" + key));
	Result b = Get(c, HttpsUrl("/cache/" + key));
	ASSERT_EQ(b.rc, CRNET_OK);
	EXPECT_FALSE(crnet_response_from_cache(a.resp));
	EXPECT_TRUE(crnet_response_from_cache(b.resp));
	EXPECT_EQ(b.status(), 200);
	EXPECT_EQ(b.body(), a.body());
	EXPECT_EQ(Stats(key)["requests"], 1);
	crnet_client_free(c);
}

// A stale entry is revalidated with If-None-Match; the 304 hands back the
// stored response.
TEST(Cache, StaleEntryIsRevalidated)
{
	std::string key = NewKey();
	crnet_client *c = CacheClient(CRNET_HTTP_CACHE_MEMORY);
	std::string url = HttpsUrl("/cache/" + key + "?max_age=0");
	Result a = Get(c, url);
	Result b = Get(c, url);
	ASSERT_EQ(b.rc, CRNET_OK);
	EXPECT_EQ(b.status(), 200);
	EXPECT_TRUE(crnet_response_from_cache(b.resp));
	EXPECT_EQ(b.body(), a.body());
	json st = Stats(key);
	EXPECT_EQ(st["requests"], 2);
	EXPECT_EQ(st["conditional"], 1);
	crnet_client_free(c);
}

TEST(Cache, RequestModes)
{
	std::string key = NewKey();
	crnet_client *c = CacheClient(CRNET_HTTP_CACHE_MEMORY);
	std::string url = HttpsUrl("/cache/" + key);

	// Nothing stored yet.
	Result miss = GetMode(c, url, CRNET_CACHE_ONLY_IF_CACHED);
	EXPECT_EQ(miss.rc, CRNET_E_NET);
	EXPECT_EQ(miss.net_error(), -400);	// ERR_CACHE_MISS
	// no-store neither reads nor writes.
	GetMode(c, url, CRNET_CACHE_NO_STORE);
	EXPECT_EQ(GetMode(c, url, CRNET_CACHE_ONLY_IF_CACHED).rc, CRNET_E_NET);
	EXPECT_EQ(Stats(key)["requests"], 1);

	Result stored = GetMode(c, url, CRNET_CACHE_DEFAULT);
	EXPECT_FALSE(crnet_response_from_cache(stored.resp));
	Result hit = GetMode(c, url, CRNET_CACHE_ONLY_IF_CACHED);
	EXPECT_EQ(hit.rc, CRNET_OK);
	EXPECT_TRUE(crnet_response_from_cache(hit.resp));
	EXPECT_EQ(Stats(key)["requests"], 2);

	// no-cache revalidates even the fresh entry.
	Result validated = GetMode(c, url, CRNET_CACHE_NO_CACHE);
	EXPECT_TRUE(crnet_response_from_cache(validated.resp));
	json st = Stats(key);
	EXPECT_EQ(st["requests"], 3);
	EXPECT_EQ(st["conditional"], 1);

	// reload goes to the network unconditionally.
	Result reloaded = GetMode(c, url, CRNET_CACHE_RELOAD);
	EXPECT_FALSE(crnet_response_from_cache(reloaded.resp));
	st = Stats(key);
	EXPECT_EQ(st["requests"], 4);
	EXPECT_EQ(st["conditional"], 1);
	crnet_client_free(c);
}

TEST(Cache, ForceCacheUsesAStaleEntry)
{
	std::string key = NewKey();
	crnet_client *c = CacheClient(CRNET_HTTP_CACHE_MEMORY);
	std::string url = HttpsUrl("/cache/" + key + "?max_age=0");
	Get(c, url);
	Result r = GetMode(c, url, CRNET_CACHE_FORCE_CACHE);
	EXPECT_TRUE(crnet_response_from_cache(r.resp));
	EXPECT_EQ(Stats(key)["requests"], 1);
	crnet_client_free(c);
}

TEST(Cache, DiskCacheOutlivesTheClient)
{
	std::string tmpl = ::testing::TempDir() + "crnet-cache-XXXXXX";
	char *dir = mkdtemp(tmpl.data());
	ASSERT_NE(dir, nullptr);
	std::string key = NewKey();
	std::string url = HttpsUrl("/cache/" + key);

	crnet_client *a = CacheClient(CRNET_HTTP_CACHE_DISK, dir);
	Result first = Get(a, url);
	ASSERT_EQ(first.rc, CRNET_OK);
	crnet_client_free(a);

	crnet_client *b = CacheClient(CRNET_HTTP_CACHE_DISK, dir);
	Result again = Get(b, url);
	ASSERT_EQ(again.rc, CRNET_OK);
	EXPECT_TRUE(crnet_response_from_cache(again.resp));
	EXPECT_EQ(again.body(), first.body());
	EXPECT_EQ(Stats(key)["requests"], 1);
	crnet_client_free(b);
	std::string rm = std::string("rm -rf ") + dir;
	EXPECT_EQ(system(rm.c_str()), 0);
}

TEST(Cache, InvalidSettings)
{
	crnet_client_config *cfg = crnet_client_config_new();
	EXPECT_EQ(crnet_client_config_set_http_cache(cfg, CRNET_HTTP_CACHE_DISK, nullptr, 0),
		  CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_set_http_cache(cfg, CRNET_HTTP_CACHE_MEMORY, nullptr,
						     uint64_t{1} << 40),
		  CRNET_E_INVAL);
	EXPECT_EQ(crnet_client_config_set_http_cache(cfg, static_cast<crnet_http_cache>(7),
						     nullptr, 0),
		  CRNET_E_INVAL);
	crnet_client_config_free(cfg);
	crnet_request *req = crnet_request_new("GET", "https://example.com/");
	EXPECT_EQ(crnet_request_set_cache_mode(req, static_cast<crnet_cache_mode>(9)),
		  CRNET_E_INVAL);
	crnet_request_free(req);
}
