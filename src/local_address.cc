// SPDX-License-Identifier: BSD-3-Clause
#include "crnet/local_address.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_split.h"
#include "net/base/address_list.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/base/sockaddr_storage.h"
#include "net/socket/client_socket_factory.h"
#include "net/socket/datagram_client_socket.h"
#include "net/socket/socket_performance_watcher.h"
#include "net/socket/tcp_client_socket.h"
#include "net/socket/tcp_socket.h"
#include "net/socket/udp_client_socket.h"

namespace crnet {
namespace {

// The error for a peer of a family without a local address: as if there
// were no route to it, which is how Chrome's Happy Eyeballs treats a
// family it cannot use.
constexpr int kNoLocalAddress = net::ERR_ADDRESS_UNREACHABLE;

// Opens a non-blocking socket of |peer|'s family and binds it to the
// local address of that family. Returns a net error; on success |*fd|
// is the caller's.
int OpenBoundSocket(const LocalAddresses &local, int type,
		    const net::IPEndPoint &peer, int *fd)
{
	const net::IPAddress *addr = local.For(peer);
	if (!addr)
		return kNoLocalAddress;
	net::SockaddrStorage storage;
	if (!net::IPEndPoint(*addr, 0).ToSockAddr(storage.addr(),
						   &storage.addr_len))
		return net::ERR_ADDRESS_INVALID;
	int s = socket(storage.addr()->sa_family,
		       type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (s < 0)
		return net::MapSystemError(errno);
	if (bind(s, storage.addr(), storage.addr_len) < 0) {
		int err = net::MapSystemError(errno);
		close(s);
		return err;
	}
	*fd = s;
	return net::OK;
}

// UDPClientSocket is final, and binds only implicitly on connect, so
// QUIC's sockets are wrapped: Connect*() first opens a socket of the
// peer's family, binds it to the local address and hands it to the
// real socket, which then connects it as usual. Everything else is
// passed through.
class BoundDatagramSocket final : public net::DatagramClientSocket {
public:
	BoundDatagramSocket(const LocalAddresses &local,
			    std::unique_ptr<net::UDPClientSocket> inner)
		: local_(local), inner_(std::move(inner))
	{
	}

	int Connect(const net::IPEndPoint &address) override
	{
		int rv = Prepare(address);
		return rv == net::OK ? inner_->Connect(address) : rv;
	}
	int ConnectUsingNetwork(net::handles::NetworkHandle network,
				const net::IPEndPoint &address) override
	{
		int rv = Prepare(address);
		return rv == net::OK
			       ? inner_->ConnectUsingNetwork(network, address)
			       : rv;
	}
	int ConnectUsingDefaultNetwork(const net::IPEndPoint &address) override
	{
		int rv = Prepare(address);
		return rv == net::OK
			       ? inner_->ConnectUsingDefaultNetwork(address)
			       : rv;
	}
	int ConnectAsync(const net::IPEndPoint &address,
			 net::CompletionOnceCallback callback) override
	{
		int rv = Prepare(address);
		return rv == net::OK
			       ? inner_->ConnectAsync(address, std::move(callback))
			       : rv;
	}
	int ConnectUsingNetworkAsync(
		net::handles::NetworkHandle network,
		const net::IPEndPoint &address,
		net::CompletionOnceCallback callback) override
	{
		int rv = Prepare(address);
		return rv == net::OK ? inner_->ConnectUsingNetworkAsync(
					       network, address,
					       std::move(callback))
				     : rv;
	}
	int ConnectUsingDefaultNetworkAsync(
		const net::IPEndPoint &address,
		net::CompletionOnceCallback callback) override
	{
		int rv = Prepare(address);
		return rv == net::OK ? inner_->ConnectUsingDefaultNetworkAsync(
					       address, std::move(callback))
				     : rv;
	}

	net::handles::NetworkHandle GetBoundNetwork() const override
	{
		return inner_->GetBoundNetwork();
	}
	base::expected<net::DatagramsMetadata, net::Error> ReadMultiple(
		net::IOBuffer *buf, size_t buf_len, size_t max_message_size,
		base::OnceCallback<void(
			base::expected<net::DatagramsMetadata, net::Error>)>
			callback) override
	{
		return inner_->ReadMultiple(buf, buf_len, max_message_size,
					    std::move(callback));
	}
	void ApplySocketTag(const net::SocketTag &tag) override
	{
		inner_->ApplySocketTag(tag);
	}
	void EnableRecvOptimization() override
	{
		inner_->EnableRecvOptimization();
	}
	int SetMulticastInterface(uint32_t interface_index) override
	{
		return inner_->SetMulticastInterface(interface_index);
	}
	void SetIOSNetworkServiceType(int type) override
	{
		inner_->SetIOSNetworkServiceType(type);
	}
	void RegisterQuicConnectionClosePayload(
		base::span<uint8_t> payload) override
	{
		inner_->RegisterQuicConnectionClosePayload(payload);
	}
	void UnregisterQuicConnectionClosePayload() override
	{
		inner_->UnregisterQuicConnectionClosePayload();
	}

	// DatagramSocket
	void Close() override { inner_->Close(); }
	int GetPeerAddress(net::IPEndPoint *address) const override
	{
		return inner_->GetPeerAddress(address);
	}
	int GetLocalAddress(net::IPEndPoint *address) const override
	{
		return inner_->GetLocalAddress(address);
	}
	void UseNonBlockingIO() override { inner_->UseNonBlockingIO(); }
	int SetDoNotFragment() override { return inner_->SetDoNotFragment(); }
	int SetRecvTos() override { return inner_->SetRecvTos(); }
	int SetTos(net::DiffServCodePoint dscp, net::EcnCodePoint ecn) override
	{
		return inner_->SetTos(dscp, ecn);
	}
	void SetMsgConfirm(bool confirm) override
	{
		inner_->SetMsgConfirm(confirm);
	}
	const net::NetLogWithSource &NetLog() const override
	{
		return inner_->NetLog();
	}
	net::DscpAndEcn GetLastTos() const override
	{
		return inner_->GetLastTos();
	}

	// Socket
	int Read(net::IOBuffer *buf, int buf_len,
		 net::CompletionOnceCallback callback) override
	{
		return inner_->Read(buf, buf_len, std::move(callback));
	}
	int ReadIfReady(net::IOBuffer *buf, int buf_len,
			net::CompletionOnceCallback callback) override
	{
		return inner_->ReadIfReady(buf, buf_len, std::move(callback));
	}
	int CancelReadIfReady() override { return inner_->CancelReadIfReady(); }
	int Write(net::IOBuffer *buf, int buf_len,
		  net::CompletionOnceCallback callback,
		  const net::NetworkTrafficAnnotationTag &annotation) override
	{
		return inner_->Write(buf, buf_len, std::move(callback),
				     annotation);
	}
	int SetReceiveBufferSize(int32_t size) override
	{
		return inner_->SetReceiveBufferSize(size);
	}
	int SetSendBufferSize(int32_t size) override
	{
		return inner_->SetSendBufferSize(size);
	}
	void SetDnsAliases(std::set<std::string> aliases) override
	{
		inner_->SetDnsAliases(std::move(aliases));
	}
	const std::set<std::string> &GetDnsAliases() const override
	{
		return inner_->GetDnsAliases();
	}

private:
	int Prepare(const net::IPEndPoint &peer)
	{
		int fd;
		int rv = OpenBoundSocket(local_, SOCK_DGRAM, peer, &fd);
		if (rv != net::OK)
			return rv;
		rv = inner_->AdoptOpenedSocket(peer.GetFamily(), fd);
		// Only a refusal for the socket limit leaves |fd| ours; any
		// later failure has closed it already.
		if (rv == net::ERR_INSUFFICIENT_RESOURCES)
			close(fd);
		return rv;
	}

	const LocalAddresses local_;
	const std::unique_ptr<net::UDPClientSocket> inner_;
};

class LocalAddressSocketFactory final : public net::ClientSocketFactory {
public:
	explicit LocalAddressSocketFactory(const LocalAddresses &local)
		: local_(local)
	{
	}

	std::unique_ptr<net::DatagramClientSocket> CreateDatagramClientSocket(
		net::DatagramSocket::BindType /*bind_type*/,
		net::handles::NetworkHandle target_network,
		net::NetLog *net_log, const net::NetLogSource &source) override
	{
		// The socket is bound before it connects, to a port the
		// kernel picks, so the connect must not bind again
		// (RANDOM_BIND would).
		return std::make_unique<BoundDatagramSocket>(
			local_, std::make_unique<net::UDPClientSocket>(
					net::DatagramSocket::DEFAULT_BIND,
					net_log, source, target_network));
	}

	std::unique_ptr<net::TransportClientSocket> CreateTransportClientSocket(
		const net::AddressList &addresses,
		net::handles::NetworkHandle target_network,
		std::unique_ptr<net::SocketPerformanceWatcher> watcher,
		net::NetworkQualityEstimator *nqe, net::NetLog *net_log,
		const net::NetLogSource &source) override
	{
		// //net's connect jobs make one socket per address, so the
		// first address decides.
		auto tcp = net::TCPSocket::Create(std::move(watcher), net_log,
						  source);
		int fd;
		int rv = addresses.empty()
				 ? kNoLocalAddress
				 : OpenBoundSocket(local_, SOCK_STREAM,
						   addresses.front(), &fd);
		if (rv == net::OK)
			rv = tcp->AdoptUnconnectedSocket(fd);	// owns |fd| now
		if (rv != net::OK) {
			// Fail the connect rather than return no socket.
			auto sock = std::make_unique<net::TCPClientSocket>(
				addresses, nullptr, nqe, net_log, source,
				target_network);
			sock->SetBeforeConnectCallback(base::BindRepeating(
				[](int err) { return err; }, rv));
			return sock;
		}

		auto sock = std::make_unique<net::TCPClientSocket>(
			std::move(tcp), addresses, nullptr, nqe);
		// A connect attempt after the first (to another address, or
		// after a disconnect) would use a socket TCPClientSocket opened
		// itself, unbound. //net makes none of these, so this only
		// keeps such a socket from leaving from the default address.
		sock->SetBeforeConnectCallback(base::BindRepeating(
			[](bool &first) -> int {
				bool ok = first;
				first = false;
				return ok ? net::OK : net::ERR_UNEXPECTED;
			},
			base::OwnedRef(true)));
		return sock;
	}

	std::unique_ptr<net::SSLClientSocket> CreateSSLClientSocket(
		net::SSLClientContext *context,
		std::unique_ptr<net::StreamSocket> stream_socket,
		const net::HostPortPair &host_and_port,
		const net::SSLConfig &ssl_config) override
	{
		return net::ClientSocketFactory::GetDefaultFactory()
			->CreateSSLClientSocket(context,
						std::move(stream_socket),
						host_and_port, ssl_config);
	}

private:
	const LocalAddresses local_;
};

} // namespace

const net::IPAddress *LocalAddresses::For(const net::IPEndPoint &peer) const
{
	const std::optional<net::IPAddress> &a =
		peer.address().IsIPv4() ? v4 : v6;
	return a ? &*a : nullptr;
}

bool ParseLocalAddresses(std::string_view list, LocalAddresses *out)
{
	LocalAddresses r;
	for (std::string_view s : base::SplitStringPiece(
		     list, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL)) {
		net::IPAddress a;
		if (!a.AssignFromIPLiteral(s))
			return false;
		// URL-style IPv4 ("1.2.3", "0x7f.1") is too surprising here.
		if (a.IsIPv4() && a.ToString() != s)
			return false;
		std::optional<net::IPAddress> &slot = a.IsIPv4() ? r.v4 : r.v6;
		if (slot)
			return false;
		slot = a;
	}
	*out = r;
	return true;
}

std::unique_ptr<net::ClientSocketFactory> CreateLocalAddressSocketFactory(
	const LocalAddresses &local)
{
	return std::make_unique<LocalAddressSocketFactory>(local);
}

} // namespace crnet
