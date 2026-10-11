// SPDX-License-Identifier: BSD-3-Clause
#include "crnet/socks_auth.h"

#include <map>
#include <mutex>
#include <optional>
#include <utility>

#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "net/base/host_port_pair.h"
#include "net/base/proxy_server.h"
#include "net/base/proxy_string_util.h"
#include "net/socket/socks5_client_socket.h"

namespace crnet {
namespace {

struct Credentials {
	std::string username;
	std::string password;
};

struct Table {
	std::mutex mu;
	// "host:port#partition" -> credentials.
	std::map<std::string, Credentials> entries;
	// Partition -> the partition whose credentials it falls back to.
	std::map<int, int> parents;
};

Table &GetTable()
{
	static base::NoDestructor<Table> table;
	return *table;
}

std::string Key(const std::string &proxy_key, int partition)
{
	return proxy_key + "#" + base::NumberToString(partition);
}

// Called on network threads, by every SOCKS5 connection attempt. Without
// a partition the connection is not a crnet client's (none are), and gets
// no credentials.
bool Lookup(const net::HostPortPair &proxy, std::optional<int> partition,
	    std::string *username, std::string *password)
{
	if (!partition)
		return false;
	Table &t = GetTable();
	std::lock_guard<std::mutex> lk(t.mu);
	const std::string proxy_key = proxy.ToString();
	auto it = t.entries.find(Key(proxy_key, *partition));
	if (it == t.entries.end()) {
		auto parent = t.parents.find(*partition);
		if (parent != t.parents.end())
			it = t.entries.find(Key(proxy_key, parent->second));
	}
	if (it == t.entries.end())
		return false;
	*username = it->second.username;
	*password = it->second.password;
	return true;
}

} // namespace

void InstallSocksCredentialsLookup()
{
	net::SOCKS5ClientSocket::SetCredentialsLookup(&Lookup);
}

std::string SocksProxyKey(const std::string &proxy)
{
	net::ProxyServer server = net::ProxyUriToProxyServer(
		proxy, net::ProxyServer::SCHEME_HTTP);
	if (!server.is_valid() || !server.is_socks() ||
	    server.scheme() != net::ProxyServer::SCHEME_SOCKS5)
		return std::string();
	return server.host_port_pair().ToString();
}

std::string RegisterSocksCredentials(const std::string &proxy_key,
				     int partition, const std::string &username,
				     const std::string &password)
{
	Table &t = GetTable();
	std::lock_guard<std::mutex> lk(t.mu);
	std::string key = Key(proxy_key, partition);
	t.entries[key] = {username, password};
	return key;
}

void UnregisterSocksCredentials(const std::string &key)
{
	Table &t = GetTable();
	std::lock_guard<std::mutex> lk(t.mu);
	t.entries.erase(key);
}

void SetSocksPartitionParent(int partition, int parent)
{
	Table &t = GetTable();
	std::lock_guard<std::mutex> lk(t.mu);
	t.parents[partition] = parent;
}

void ClearSocksPartitionParent(int partition)
{
	Table &t = GetTable();
	std::lock_guard<std::mutex> lk(t.mu);
	t.parents.erase(partition);
}

} // namespace crnet
