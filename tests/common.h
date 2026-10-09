// SPDX-License-Identifier: BSD-3-Clause
//
// Shared test fixture: starts tests/server/test_server.py once per test
// process and offers small helpers around the crhttp C API.
//
#ifndef CRHTTP_TESTS_COMMON_H
#define CRHTTP_TESTS_COMMON_H

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <crhttp/crhttp.h>

namespace crtest {

using json = nlohmann::json;

struct Ports {
	int https = 0;		// h2 + http/1.1
	int https_h1 = 0;	// http/1.1 only
	int http = 0;
	int socks5 = 0;
	int http_proxy = 0;
};

// Ports of the running test server (started on first use).
const Ports &ServerPorts();

std::string HttpsUrl(const std::string &path);
std::string HttpsH1Url(const std::string &path);
std::string HttpUrl(const std::string &path);

// A client that trusts the test server's self-signed certificate.
crhttp_client *NewTestClient(crhttp_client_config *cfg = nullptr);

// Owns the pieces of one blocking request.
struct Result {
	int rc = CRHTTP_E_STATE;
	crhttp_response *resp = nullptr;

	Result() = default;
	Result(Result &&o) noexcept : rc(o.rc), resp(std::exchange(o.resp, nullptr)) {}
	~Result() { crhttp_response_free(resp); }

	int status() const { return crhttp_response_status(resp); }
	std::string body() const
	{
		size_t n = 0;
		const char *p = crhttp_response_body(resp, &n);
		return p ? std::string(p, n) : std::string();
	}
	std::string header(const char *name) const
	{
		const char *v = crhttp_response_header(resp, name);
		return v ? v : "";
	}
	std::string protocol() const { return crhttp_response_protocol(resp); }
	int net_error() const { return crhttp_response_net_error(resp); }
	json echo() const { return json::parse(body()); }
};

Result Perform(crhttp_client *c, crhttp_request *req);
Result Get(crhttp_client *c, const std::string &url);

// Header names, in order, from an /echo document (pseudo headers kept).
std::vector<std::string> HeaderNames(const json &echo);
// First value of |name| (case-insensitive) in an /echo document.
std::string EchoHeader(const json &echo, const std::string &name);
bool EchoHasHeader(const json &echo, const std::string &name);

// Fixture that owns a default test client.
class ClientTest : public ::testing::Test {
protected:
	void SetUp() override { client_ = NewTestClient(); ASSERT_NE(client_, nullptr); }
	void TearDown() override { crhttp_client_free(client_); }
	crhttp_client *client_ = nullptr;
};

} // namespace crtest

#endif // CRHTTP_TESTS_COMMON_H
