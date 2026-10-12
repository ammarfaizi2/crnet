// SPDX-License-Identifier: BSD-3-Clause
//
// Local IP address changes. crnet_simulate_ip_address_change() makes
// every client hear one, as when an interface comes or goes on the host;
// by default that aborts connects, resolutions and idle connections,
// with crnet_client_config_set_ignore_ip_address_changes() it does not.
//
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "common.h"

using namespace crtest;

namespace {

using Clock = std::chrono::steady_clock;

// A TCP listener that never accepts: connects complete in the kernel,
// and then nothing more is ever said, so a SOCKS5 greeting or a TLS
// handshake on it stays pending.
class SilentListener {
public:
	SilentListener()
	{
		fd_ = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in sa = {};
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		EXPECT_EQ(bind(fd_, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)), 0);
		EXPECT_EQ(listen(fd_, 16), 0);
		socklen_t sl = sizeof(sa);
		EXPECT_EQ(getsockname(fd_, reinterpret_cast<sockaddr *>(&sa), &sl), 0);
		port_ = ntohs(sa.sin_port);
	}
	~SilentListener() { close(fd_); }
	int port() const { return port_; }

private:
	int fd_ = -1;
	int port_ = 0;
};

crnet_client *Client(bool ignore, const char *doh = nullptr)
{
	crnet_client_config *cfg = crnet_client_config_new();
	crnet_client_config_set_ignore_ip_address_changes(cfg, ignore);
	if (doh) {
		EXPECT_EQ(crnet_client_config_set_secure_dns(cfg, CRNET_SECURE_DNS_SECURE, doh),
			  CRNET_OK);
		// Without a QUIC attempt racing the TCP one: when the change
		// aborts the QUIC attempt's resolution, a TCP attempt waiting
		// behind it would start and resolve anew, hiding the abort.
		crnet_client_config_set_quic(cfg, 0);
	}
	crnet_client *c = NewTestClient(cfg);
	crnet_client_config_free(cfg);
	return c;
}

struct Outcome {
	int rc = 0;
	int net_error = 0;
	int ms = 0;
};

// Starts |req| on |c|, reports an address change after 300 ms, and
// returns how the request ended and when.
Outcome RunWithChange(crnet_client *c, crnet_request *req)
{
	const auto start = Clock::now();
	crnet_transfer *t = crnet_start(c, req, nullptr, nullptr);
	EXPECT_NE(t, nullptr);
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	EXPECT_EQ(crnet_simulate_ip_address_change(), CRNET_OK);
	Outcome o;
	o.rc = crnet_transfer_wait(t, UINT64_MAX);
	o.ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
					Clock::now() - start)
					.count());
	o.net_error = crnet_response_net_error(crnet_transfer_response(t));
	crnet_transfer_free(t);
	return o;
}

// A connect stuck in a SOCKS5 greeting (|via_socks|) or a TLS handshake,
// with a 1.5 s connect timeout.
Outcome StalledConnect(bool ignore, bool via_socks)
{
	SilentListener silent;
	crnet_client *c = Client(ignore);
	const std::string port = std::to_string(silent.port());
	crnet_request *req = crnet_request_new(
		"GET", (via_socks ? "https://www.crnet-test/" : "https://127.0.0.1:" + port + "/")
			       .c_str());
	if (via_socks)
		EXPECT_EQ(crnet_request_set_proxy(req, ("socks5h://127.0.0.1:" + port).c_str()),
			  CRNET_OK);
	crnet_request_set_connect_timeout(req, 1500);
	Outcome o = RunWithChange(c, req);
	crnet_request_free(req);
	crnet_client_free(c);
	return o;
}

} // namespace

TEST(NetworkChange, AbortsConnectsByDefault)
{
	for (bool via_socks : {true, false}) {
		SCOPED_TRACE(via_socks ? "SOCKS5 greeting" : "TLS handshake");
		Outcome o = StalledConnect(false, via_socks);
		EXPECT_EQ(o.rc, CRNET_E_NET);
		EXPECT_EQ(o.net_error, -21);	// ERR_NETWORK_CHANGED
		EXPECT_LT(o.ms, 1200);
	}
}

TEST(NetworkChange, ConnectsSurviveWhenIgnored)
{
	for (bool via_socks : {true, false}) {
		SCOPED_TRACE(via_socks ? "SOCKS5 greeting" : "TLS handshake");
		Outcome o = StalledConnect(true, via_socks);
		// They run into their own deadline instead.
		EXPECT_EQ(o.rc, CRNET_E_TIMEOUT);
		EXPECT_EQ(o.net_error, -118);	// ERR_CONNECTION_TIMED_OUT
		EXPECT_GE(o.ms, 1400);
	}
}

TEST(NetworkChange, WebSocketConnects)
{
	for (bool ignore : {false, true}) {
		SCOPED_TRACE(ignore ? "ignored" : "default");
		SilentListener silent;
		crnet_client *c = Client(ignore);
		crnet_request *req = crnet_request_new(nullptr, "wss://www.crnet-test/ws/echo");
		crnet_request_set_proxy(req,
					("socks5h://127.0.0.1:" + std::to_string(silent.port())).c_str());
		crnet_request_set_timeout(req, 1500);
		crnet_ws *ws = crnet_ws_connect(c, req, nullptr, nullptr, nullptr);
		crnet_request_free(req);
		ASSERT_NE(ws, nullptr);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		ASSERT_EQ(crnet_simulate_ip_address_change(), CRNET_OK);
		const int rc = crnet_ws_wait_open(ws, 5000);
		if (ignore) {
			EXPECT_EQ(rc, CRNET_E_TIMEOUT);
			EXPECT_EQ(crnet_ws_net_error(ws), -7);	// the handshake timeout
		} else {
			EXPECT_EQ(rc, CRNET_E_NET);
			EXPECT_EQ(crnet_ws_net_error(ws), -21);
		}
		crnet_ws_free(ws);
		crnet_client_free(c);
	}
}

TEST(NetworkChange, IdleConnections)
{
	for (bool ignore : {false, true}) {
		for (bool h2 : {false, true}) {
			SCOPED_TRACE(std::string(ignore ? "ignored" : "default") +
				     (h2 ? ", h2" : ", http/1.1"));
			crnet_client *c = Client(ignore);
			const std::string url = h2 ? HttpsUrl("/echo") : HttpsH1Url("/echo");
			Result a = Get(c, url);
			ASSERT_EQ(a.rc, CRNET_OK);
			ASSERT_EQ(crnet_simulate_ip_address_change(), CRNET_OK);
			// The notification reaches the network thread by a task.
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			Result b = Get(c, url);
			ASSERT_EQ(b.rc, CRNET_OK);
			EXPECT_EQ(crnet_response_socket_reused(b.resp), ignore);
			EXPECT_EQ(crnet_response_connection_id(b.resp) ==
					  crnet_response_connection_id(a.resp),
				  ignore);
			crnet_client_free(c);
		}
	}
}

// A DNS-over-HTTPS server that answers after 1.5 s keeps the resolution
// in progress while the address changes.
TEST(NetworkChange, ResolutionsInProgress)
{
	const std::string doh = "https://127.0.0.1:" + std::to_string(ServerPorts().https) +
				"/dns-query?delay_ms=1500{&dns}";
	for (bool ignore : {false, true}) {
		SCOPED_TRACE(ignore ? "ignored" : "default");
		crnet_client *c = Client(ignore, doh.c_str());
		crnet_request *req = crnet_request_new(
			"GET", ("https://slow.crnet-test:" + std::to_string(ServerPorts().https) +
				"/echo")
				       .c_str());
		Outcome o = RunWithChange(c, req);
		crnet_request_free(req);
		if (ignore) {
			EXPECT_EQ(o.rc, CRNET_OK) << o.net_error;
			EXPECT_GE(o.ms, 1400);
		} else {
			// The resolver aborts the resolution (ERR_NETWORK_CHANGED),
			// or, if the socket pools hear of the change first, the
			// DoH request's own connection goes and the resolution
			// fails with it (ERR_NAME_NOT_RESOLVED).
			EXPECT_EQ(o.rc, CRNET_E_NET);
			EXPECT_TRUE(o.net_error == -21 || o.net_error == -105) << o.net_error;
			EXPECT_LT(o.ms, 1200);
		}
		crnet_client_free(c);
	}
}
