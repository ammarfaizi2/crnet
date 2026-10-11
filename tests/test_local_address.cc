// SPDX-License-Identifier: BSD-3-Clause
//
// Binding connections to a local address. Linux routes all of 127/8 to
// the loopback interface, so 127.0.0.2 is a second local address on
// any host.
//
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>

#include "common.h"

using namespace crtest;

namespace {

crnet_client *BoundClient(const char *local, bool verify = false)
{
	crnet_client_config *cfg = crnet_client_config_new();
	EXPECT_EQ(crnet_client_config_set_local_address(cfg, local), CRNET_OK);
	crnet_client *c;
	if (verify) {
		EXPECT_EQ(crnet_client_config_add_trust_anchors_file(
				  cfg, ServerPorts().ca_file.c_str()),
			  CRNET_OK);
		c = crnet_client_new(cfg);
	} else {
		c = NewTestClient(cfg);
	}
	crnet_client_config_free(cfg);
	return c;
}

std::string PeerOf(crnet_client *c, const std::string &url)
{
	Result r = Get(c, url);
	EXPECT_EQ(r.rc, CRNET_OK) << url << ": " << r.net_error();
	if (r.rc != CRNET_OK)
		return "";
	return r.echo().value("peer_addr", "");
}

std::string Url(const char *scheme, int port, const std::string &path)
{
	return std::string(scheme) + "://127.0.0.1:" + std::to_string(port) +
	       path;
}

} // namespace

TEST(LocalAddress, Validation)
{
	crnet_client_config *cfg = crnet_client_config_new();
	for (const char *ok : std::initializer_list<const char *>{
		     "127.0.0.2", "::1", "127.0.0.2,::1",
			       "::1, 127.0.0.2", "0.0.0.0", "", nullptr})
		EXPECT_EQ(crnet_client_config_set_local_address(cfg, ok),
			  CRNET_OK) << (ok ? ok : "NULL");
	for (const char *bad : {"localhost", "eth0", "[::1]", "127.0.0.2:80",
				"127.0.0.2,127.0.0.3", "::1,::2",
				"127.0.0.2,", ",", "1.2.3", "0x7f.0.0.2",
				"127.000.0.2"})
		EXPECT_EQ(crnet_client_config_set_local_address(cfg, bad),
			  CRNET_E_INVAL) << bad;
	EXPECT_EQ(crnet_client_config_set_local_address(nullptr, "::1"),
		  CRNET_E_INVAL);
	crnet_client_config_free(cfg);
}

TEST(LocalAddress, ConnectionsLeaveFromIt)
{
	crnet_client *c = BoundClient("127.0.0.2");
	ASSERT_NE(c, nullptr);
	const Ports &p = ServerPorts();
	EXPECT_EQ(PeerOf(c, Url("https", p.https, "/echo")), "127.0.0.2");
	EXPECT_EQ(PeerOf(c, Url("https", p.https_h1, "/echo")), "127.0.0.2");
	EXPECT_EQ(PeerOf(c, Url("http", p.http, "/echo")), "127.0.0.2");
	crnet_client_free(c);

	// Without the option the system picks 127.0.0.1.
	c = NewTestClient();
	EXPECT_EQ(PeerOf(c, Url("http", p.http, "/echo")), "127.0.0.1");
	crnet_client_free(c);
}

TEST(LocalAddress, WebSocketLeavesFromIt)
{
	crnet_client *c = BoundClient("127.0.0.2");
	ASSERT_NE(c, nullptr);
	crnet_request *req = crnet_request_new(
		nullptr, Url("ws", ServerPorts().http, "/ws/headers").c_str());
	crnet_ws *ws = crnet_ws_connect(c, req, nullptr, nullptr, nullptr);
	crnet_request_free(req);
	ASSERT_NE(ws, nullptr);
	ASSERT_EQ(crnet_ws_wait_open(ws, 5000), CRNET_OK);
	crnet_ws_msg_type type;
	void *data = nullptr;
	size_t len = 0;
	ASSERT_EQ(crnet_ws_recv(ws, &type, &data, &len, 5000), CRNET_OK);
	json doc = json::parse(std::string(static_cast<char *>(data), len));
	crnet_free(data);
	EXPECT_EQ(doc.value("peer_addr", ""), "127.0.0.2");
	crnet_ws_free(ws);
	crnet_client_free(c);
}

TEST(LocalAddress, WildcardLeavesTheChoiceToTheSystem)
{
	crnet_client *c = BoundClient("0.0.0.0");
	ASSERT_NE(c, nullptr);
	EXPECT_EQ(PeerOf(c, Url("http", ServerPorts().http, "/echo")),
		  "127.0.0.1");
	crnet_client_free(c);
}

TEST(LocalAddress, FamilyWithoutAddressFails)
{
	crnet_client *c = BoundClient("::1");
	ASSERT_NE(c, nullptr);
	Result r = Get(c, Url("http", ServerPorts().http, "/echo"));
	EXPECT_EQ(r.rc, CRNET_E_NET);
	EXPECT_EQ(r.net_error(), -109);	// ERR_ADDRESS_UNREACHABLE
	crnet_client_free(c);
}

TEST(LocalAddress, OtherFamilyIsTriedNext)
{
	// "localhost" is ::1 and 127.0.0.1; only IPv4 has an address.
	crnet_client *c = BoundClient("127.0.0.2");
	ASSERT_NE(c, nullptr);
	EXPECT_EQ(PeerOf(c, HttpUrl("/echo")), "127.0.0.2");
	crnet_client_free(c);
}

TEST(LocalAddress, AddressTheHostLacksFails)
{
	std::ifstream f("/proc/sys/net/ipv4/ip_nonlocal_bind");
	int nonlocal = 0;
	if (f >> nonlocal && nonlocal)
		GTEST_SKIP() << "the host allows binding to any address";
	crnet_client *c = BoundClient("192.0.2.1");	// TEST-NET-1
	ASSERT_NE(c, nullptr);
	Result r = Get(c, Url("http", ServerPorts().http, "/echo"));
	EXPECT_EQ(r.rc, CRNET_E_NET);
	EXPECT_EQ(r.net_error(), -108);	// ERR_ADDRESS_INVALID
	crnet_client_free(c);
}

// The test server speaks no QUIC, but a QUIC attempt starts with a UDP
// datagram, and that shows where it came from. Alt-Svc sends the
// client's next request to a UDP port the test listens on.
TEST(LocalAddress, QuicLeavesFromIt)
{
	int udp = socket(AF_INET, SOCK_DGRAM, 0);
	ASSERT_GE(udp, 0);
	sockaddr_in sa = {};
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	ASSERT_EQ(bind(udp, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)), 0);
	socklen_t sl = sizeof(sa);
	ASSERT_EQ(getsockname(udp, reinterpret_cast<sockaddr *>(&sa), &sl), 0);
	const std::string port = std::to_string(ntohs(sa.sin_port));

	// Alt-Svc is ignored on responses with certificate errors.
	crnet_client *c = BoundClient("127.0.0.2", true);
	ASSERT_NE(c, nullptr);
	const int https = ServerPorts().https;
	Result r = Get(c, Url("https", https,
			      "/response-headers?Alt-Svc=h3%3D%22%3A" + port +
				      "%22"));
	ASSERT_EQ(r.rc, CRNET_OK) << r.net_error();
	// Without an idle HTTP/2 connection to reuse, the next request
	// tries QUIC.
	crnet_client_close_idle(c);
	EXPECT_EQ(PeerOf(c, Url("https", https, "/echo")), "127.0.0.2");

	pollfd pfd = {udp, POLLIN, 0};
	ASSERT_EQ(poll(&pfd, 1, 5000), 1) << "no QUIC packet";
	char buf[2048];
	sockaddr_in from = {};
	socklen_t fl = sizeof(from);
	ASSERT_GT(recvfrom(udp, buf, sizeof(buf), 0,
			   reinterpret_cast<sockaddr *>(&from), &fl), 0);
	char text[INET_ADDRSTRLEN] = {};
	inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
	EXPECT_STREQ(text, "127.0.0.2");
	crnet_client_free(c);
	close(udp);
}
