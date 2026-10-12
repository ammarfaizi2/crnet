// SPDX-License-Identifier: BSD-3-Clause
//
// Shared test fixture: starts tests/server/test_server.py once per test
// process and offers small helpers around the crnet C API.
//
#ifndef CRNET_TESTS_COMMON_H
#define CRNET_TESTS_COMMON_H

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <crnet/crnet.h>

namespace crtest {

using json = nlohmann::json;

struct Ports {
	int https = 0;		// h2 + http/1.1
	int https_h1 = 0;	// http/1.1 only
	int http = 0;
	int socks5 = 0;
	int socks5_auth = 0;	// requires crnet:secret (RFC 1929)
	int http_proxy = 0;
	int https_ws_h2 = 0;	// h2 that also serves WebSockets (RFC 8441)
	int https_mtls = 0;	// requires a client certificate
	std::string ca_file;	// PEM of the CA that issued the server cert
	std::string client_cert_file;	// for https_mtls, issued by the CA
	std::string client_key_file;
};

// Ports of the running test server (started on first use).
const Ports &ServerPorts();

std::string HttpsUrl(const std::string &path);
std::string HttpsH1Url(const std::string &path);
std::string HttpUrl(const std::string &path);

// A client that accepts any certificate (the `insecure` option) and
// ignores local IP address changes.
crnet_client *NewTestClient(crnet_client_config *cfg = nullptr);

// Owns the pieces of one blocking request.
struct Result {
	int rc = CRNET_E_STATE;
	crnet_response *resp = nullptr;

	Result() = default;
	Result(Result &&o) noexcept : rc(o.rc), resp(std::exchange(o.resp, nullptr)) {}
	Result &operator=(Result &&o) noexcept
	{
		std::swap(rc, o.rc);
		std::swap(resp, o.resp);	// o frees the old response
		return *this;
	}
	~Result() { crnet_response_free(resp); }

	int status() const { return crnet_response_status(resp); }
	std::string body() const
	{
		size_t n = 0;
		const char *p = crnet_response_body(resp, &n);
		return p ? std::string(p, n) : std::string();
	}
	std::string header(const char *name) const
	{
		const char *v = crnet_response_header(resp, name);
		return v ? v : "";
	}
	std::string protocol() const { return crnet_response_protocol(resp); }
	int net_error() const { return crnet_response_net_error(resp); }
	json echo() const { return json::parse(body()); }
};

Result Perform(crnet_client *c, crnet_request *req);
Result Get(crnet_client *c, const std::string &url);

// Header names, in order, from an /echo document (pseudo headers kept).
std::vector<std::string> HeaderNames(const json &echo);
// First value of |name| (case-insensitive) in an /echo document.
std::string EchoHeader(const json &echo, const std::string &name);
bool EchoHasHeader(const json &echo, const std::string &name);

// Fixture that owns a default test client.
class ClientTest : public ::testing::Test {
protected:
	void SetUp() override { client_ = NewTestClient(); ASSERT_NE(client_, nullptr); }
	void TearDown() override { crnet_client_free(client_); }
	crnet_client *client_ = nullptr;
};

} // namespace crtest

#endif // CRNET_TESTS_COMMON_H
