// SPDX-License-Identifier: BSD-3-Clause
//
// Binding a client's connections to chosen local (source) addresses
// and to a network interface. The socket pools take neither, so the
// client gets a ClientSocketFactory that binds every TCP and UDP (QUIC)
// socket it creates before the socket connects.
//
#ifndef CRNET_SRC_LOCAL_ADDRESS_H
#define CRNET_SRC_LOCAL_ADDRESS_H

#include <memory>
#include <optional>
#include <string>
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

// Where a client's sockets are bound before they connect.
struct SocketBinding {
	LocalAddresses addresses;	// none = the system's choice
	std::string interface_name;	// empty = the routing table's choice

	bool empty() const
	{
		return addresses.empty() && interface_name.empty();
	}
};

// Parses "addr" or "addr,addr" (IP literals, IPv6 without brackets).
// Fails on anything malformed and on two addresses of one family.
bool ParseLocalAddresses(std::string_view list, LocalAddresses *out);

// Whether the kernel would accept |name| as an interface name. It need
// not exist.
bool ValidInterfaceName(std::string_view name);

// A factory whose sockets are bound as |binding| says. A socket that
// cannot be bound, or whose peer is of a family without a local
// address, fails to connect instead of connecting unbound.
std::unique_ptr<net::ClientSocketFactory> CreateBindingSocketFactory(
	const SocketBinding &binding);

} // namespace crnet

#endif // CRNET_SRC_LOCAL_ADDRESS_H
