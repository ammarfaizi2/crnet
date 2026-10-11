// SPDX-License-Identifier: BSD-3-Clause
//
// Binding a client's connections to chosen local (source) addresses.
// The socket pools do not take a bind address, so the client gets a
// ClientSocketFactory that binds every TCP and UDP (QUIC) socket it
// creates before the socket connects.
//
#ifndef CRNET_SRC_LOCAL_ADDRESS_H
#define CRNET_SRC_LOCAL_ADDRESS_H

#include <memory>
#include <optional>
#include <string_view>

#include "net/base/ip_address.h"

namespace net {
class ClientSocketFactory;
class IPEndPoint;
} // namespace net

namespace crnet {

// At most one address per family.
struct LocalAddresses {
	std::optional<net::IPAddress> v4;
	std::optional<net::IPAddress> v6;

	bool empty() const { return !v4 && !v6; }
	// The address for a connection to |peer|; nullptr when none is set
	// for its family.
	const net::IPAddress *For(const net::IPEndPoint &peer) const;
};

// Parses "addr" or "addr,addr" (IP literals, IPv6 without brackets).
// Fails on anything malformed and on two addresses of one family.
bool ParseLocalAddresses(std::string_view list, LocalAddresses *out);

// A factory whose sockets connect from |local|. A socket that cannot be
// bound, or whose peer is of a family without a local address, fails
// to connect instead of connecting from the default address.
std::unique_ptr<net::ClientSocketFactory> CreateLocalAddressSocketFactory(
	const LocalAddresses &local);

} // namespace crnet

#endif // CRNET_SRC_LOCAL_ADDRESS_H
